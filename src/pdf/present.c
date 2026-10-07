/* sg-pdf -- SG PDF: Full Screen Mode, a presentation (View > Full Screen
 * Mode, F11 or Ctrl+L).
 *
 * The document one page per screen: the page as large as the screen holds
 * it, centred on black, on the monitor the window is on, over everything
 * (the taskbar too). A click, Right, Down, Space, Enter, Page Down or N goes
 * to the next page; Left, Up, Backspace, Page Up, P or a right click to the
 * one before; Home and End to the first and the last; the mouse wheel turns
 * pages too. Esc, F11 or Ctrl+L ends it, and the window shows the page the
 * presentation was on. The pointer hides after two seconds still.
 *
 * Pages are rendered at the screen's size by the engine (the same render
 * request the view makes), the next page ahead of time so turning is at
 * once.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"

#define PRESENT_CLASS L"SgPdfPresent"
#define TIMER_POINTER 1

static HWND g_present;
static int g_ppage;
static struct { int page, gen, rot, w, h; HBITMAP bmp; } g_cache[2];
static POINT g_last_pt;
static BOOL g_pointer_hidden;

BOOL present_active(void) { return g_present != NULL; }
int present_page(void) { return g_present ? g_ppage : -1; }

static void cache_free(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        if (g_cache[i].bmp) DeleteObject(g_cache[i].bmp);
        g_cache[i].bmp = NULL;
        g_cache[i].page = -1;
    }
}

/* the page rendered to fit the screen (the cache holds the page shown and the next one) */
static HBITMAP page_bitmap(int page, int *w, int *h)
{
    RECT rc;
    double pw, ph, s;
    char line[128];
    int i, slot;
    if (page < 0 || page >= g.npages) return NULL;
    for (i = 0; i < 2; i++)
        if (g_cache[i].bmp && g_cache[i].page == page && g_cache[i].gen == g.generation && g_cache[i].rot == g.rot) {
            *w = g_cache[i].w;
            *h = g_cache[i].h;
            return g_cache[i].bmp;
        }
    GetClientRect(g_present, &rc);
    pw = g.pages[page].w;
    ph = g.pages[page].h;
    if (g.rot == 90 || g.rot == 270) { double t = pw; pw = ph; ph = t; }
    if (pw <= 0 || ph <= 0 || rc.right <= 0 || rc.bottom <= 0) return NULL;
    s = min(rc.right / pw, rc.bottom / ph);
    /* the slot not holding the page shown */
    slot = g_cache[0].page == g_ppage && g_cache[0].bmp ? 1 : 0;
    if (g_cache[slot].bmp) DeleteObject(g_cache[slot].bmp);
    g_cache[slot].bmp = NULL;
    snprintf(line, sizeof(line), "render\t%d\t%.5f\t%d", page, s, g.rot);
    if (!br_request_into_dib(g.engine, line, &g_cache[slot].bmp, &g_cache[slot].w, &g_cache[slot].h, NULL, NULL))
        return NULL;
    g_cache[slot].page = page;
    g_cache[slot].gen = g.generation;
    g_cache[slot].rot = g.rot;
    *w = g_cache[slot].w;
    *h = g_cache[slot].h;
    return g_cache[slot].bmp;
}

static void go(int page)
{
    int w, h;
    if (page < 0) page = 0;
    if (page >= g.npages) page = g.npages - 1;
    g_ppage = page;
    InvalidateRect(g_present, NULL, FALSE);
    UpdateWindow(g_present);
    page_bitmap(page + 1, &w, &h);      /* the next page, ahead of time */
    app_dump();
}

void present_stop(void)
{
    HWND w = g_present;
    int page = g_ppage;
    if (!w) return;
    g_present = NULL;
    KillTimer(w, TIMER_POINTER);
    if (g_pointer_hidden) { ShowCursor(TRUE); g_pointer_hidden = FALSE; }
    DestroyWindow(w);
    cache_free();
    if (g_main) {
        SetForegroundWindow(g_main);
        SetFocus(g_view);
        if (page >= 0 && page < g.npages) view_goto_page(page, 0);
    }
    app_dump();
}

static void pointer_moved(HWND hwnd, LPARAM lp)
{
    POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
    if (pt.x == g_last_pt.x && pt.y == g_last_pt.y) return;
    g_last_pt = pt;
    if (g_pointer_hidden) { ShowCursor(TRUE); g_pointer_hidden = FALSE; }
    SetTimer(hwnd, TIMER_POINTER, 2000, NULL);
}

static LRESULT CALLBACK present_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        int w = 0, h = 0;
        HBITMAP bmp;
        GetClientRect(hwnd, &rc);
        bmp = page_bitmap(g_ppage, &w, &h);
        if (bmp) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, bmp);
            int x = (rc.right - w) / 2, y = (rc.bottom - h) / 2;
            RECT r;
            BitBlt(dc, x, y, w, h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
            /* black around the page */
            SetRect(&r, 0, 0, rc.right, y); FillRect(dc, &r, GetStockObject(BLACK_BRUSH));
            SetRect(&r, 0, y + h, rc.right, rc.bottom); FillRect(dc, &r, GetStockObject(BLACK_BRUSH));
            SetRect(&r, 0, y, x, y + h); FillRect(dc, &r, GetStockObject(BLACK_BRUSH));
            SetRect(&r, x + w, y, rc.right, y + h); FillRect(dc, &r, GetStockObject(BLACK_BRUSH));
        } else FillRect(dc, &rc, GetStockObject(BLACK_BRUSH));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN: {
        BOOL ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (wp) {
        case VK_ESCAPE: case VK_F11: present_stop(); return 0;
        case 'L': if (ctrl) present_stop(); return 0;
#ifndef SG_MUTANT_PRESENT_STUCK
        case VK_RIGHT: case VK_DOWN: case VK_SPACE: case VK_RETURN: case VK_NEXT: case 'N': go(g_ppage + 1); return 0;
#endif
        case VK_LEFT: case VK_UP: case VK_BACK: case VK_PRIOR: case 'P': go(g_ppage - 1); return 0;
        case VK_HOME: go(0); return 0;
        case VK_END: go(g.npages - 1); return 0;
        }
        return 0;
    }
    case WM_SYSKEYDOWN:
        if (wp == VK_F4) { present_stop(); return 0; }
        break;
    case WM_LBUTTONDOWN:
        go(GetKeyState(VK_SHIFT) < 0 ? g_ppage - 1 : g_ppage + 1);
        return 0;
    case WM_RBUTTONDOWN:
        go(g_ppage - 1);
        return 0;
    case WM_MOUSEWHEEL:
        go(GET_WHEEL_DELTA_WPARAM(wp) < 0 ? g_ppage + 1 : g_ppage - 1);
        return 0;
    case WM_MOUSEMOVE:
        pointer_moved(hwnd, lp);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_POINTER) {
            KillTimer(hwnd, TIMER_POINTER);
            if (!g_pointer_hidden) { ShowCursor(FALSE); g_pointer_hidden = TRUE; }
        }
        return 0;
    case WM_ACTIVATE:
        /* another window in front (Alt+Tab): the presentation ends, as the familiar viewers do */
        if (LOWORD(wp) == WA_INACTIVE && g_present == hwnd && lp && (HWND)lp != hwnd) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        break;
    case WM_CLOSE:
        present_stop();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void present_start(void)
{
    static BOOL registered;
    MONITORINFO mi = { sizeof(mi) };
    RECT r;
    if (g_present || !g.npages || !g.bridged) return;
    tool_commit_editor();
    if (!registered) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = present_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = PRESENT_CLASS;
        RegisterClassW(&wc);
        registered = TRUE;
    }
#ifndef SG_MUTANT_PRESENT_WINDOWED
    if (GetMonitorInfoW(MonitorFromWindow(g_main, MONITOR_DEFAULTTONEAREST), &mi)) r = mi.rcMonitor;
    else SetRect(&r, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
#else
    (void)mi;
    SetRect(&r, 100, 100, 900, 700);
#endif
    cache_free();
    g_ppage = g.current >= 0 && g.current < g.npages ? g.current : 0;
    g_last_pt.x = g_last_pt.y = -1;
    g_present = CreateWindowExW(WS_EX_TOPMOST, PRESENT_CLASS, L"SG PDF - Full Screen", WS_POPUP,
                                r.left, r.top, r.right - r.left, r.bottom - r.top, g_main, NULL, g_inst, NULL);
    if (!g_present) return;
    ShowWindow(g_present, SW_SHOW);
    SetForegroundWindow(g_present);
    SetFocus(g_present);
    SetTimer(g_present, TIMER_POINTER, 2000, NULL);
    go(g_ppage);
}
