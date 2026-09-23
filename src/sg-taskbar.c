/* sg-taskbar: the Stained Glass OS taskbar.
 *
 * A Windows 10-style taskbar, and the first of the panels that give the system
 * its appearance (ADR 0007). It is a program of its own, not part of Wine's
 * explorer: it docks to the edge of the screen through the AppBar protocol
 * (SHAppBarMessage) exactly as any third-party taskbar would, so it reserves
 * space that maximised windows respect, while Wine's explorer keeps owning
 * Shell_TrayWnd and the tray protocol underneath. That boundary is also the
 * licence boundary -- this side is AGPL, ours; explorer stays LGPL Wine.
 *
 * This first version establishes the dock and the frame: a full-width bar at
 * the bottom with a Start button. Start is left-aligned, with centering a
 * settings option to come (David's call). The clock, task buttons and tray
 * follow.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 */
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <time.h>

/* Windows 10 taskbar metrics and palette. The colours are the flat dark scheme
 * Windows 10 ships; they are plain RGB values, no Microsoft asset. */
#define TASKBAR_HEIGHT   40
#define COL_BAR          RGB(0x1F, 0x1F, 0x1F)
#define COL_BAR_HOVER    RGB(0x3A, 0x3A, 0x3A)
#define COL_ACCENT       RGB(0x00, 0x78, 0xD7)  /* the default Win10 accent blue */
#define COL_TEXT         RGB(0xFF, 0xFF, 0xFF)
#define START_WIDTH      48
#define CLOCK_WIDTH      88

static const WCHAR CLASS_NAME[] = L"SgTaskbar";
static UINT_PTR g_clock_timer = 1;
static BOOL g_start_hot = FALSE;

static RECT start_rect(HWND hwnd)
{
    RECT c;
    GetClientRect(hwnd, &c);
    RECT r = { 0, 0, START_WIDTH, c.bottom };
    return r;
}

static RECT clock_rect(HWND hwnd)
{
    RECT c;
    GetClientRect(hwnd, &c);
    RECT r = { c.right - CLOCK_WIDTH, 0, c.right, c.bottom };
    return r;
}

/* The Start mark: four stained-glass tiles set as a diamond.
 *
 * Four small diamond tiles -- each a square turned on its point -- arranged
 * around the centre as a larger diamond, held in dark leading. Purple,
 * magenta, turquoise and amber: the project's own palette, sharing no colour
 * with any operating system's logo. On hover all four go to one tint so the
 * mark reads as a single button.
 */
static void draw_start_glyph(HDC dc, RECT area, BOOL hot)
{
    int cx = (area.left + area.right) / 2;
    int cy = (area.top + area.bottom) / 2;
    int half = 6;                            /* half-diagonal of each tile */
    int off = half + 2;                      /* centre-to-tile spacing (with leading) */
    static const COLORREF glass[4] = {
        RGB(0x7B, 0x2F, 0xBE),               /* purple    (top)    */
        RGB(0xC4, 0x2E, 0x8E),               /* magenta   (right)  */
        RGB(0xE8, 0xA2, 0x00),               /* amber     (bottom) */
        RGB(0x12, 0xB5, 0xB0),               /* turquoise (left)   */
    };
    /* Tile centres at the four compass points around the mark's centre. */
    const POINT at[4] = {
        { cx, cy - off }, { cx + off, cy }, { cx, cy + off }, { cx - off, cy },
    };
    int i;

    for (i = 0; i < 4; i++)
    {
        POINT p[4] = {
            { at[i].x,        at[i].y - half },   /* top    */
            { at[i].x + half, at[i].y        },   /* right  */
            { at[i].x,        at[i].y + half },   /* bottom */
            { at[i].x - half, at[i].y        },   /* left   */
        };
        HBRUSH b = CreateSolidBrush(hot ? COL_BAR_HOVER : glass[i]);
        HBRUSH oldb = SelectObject(dc, b);
        HPEN oldp = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, p, 4);
        SelectObject(dc, oldp);
        SelectObject(dc, oldb);
        DeleteObject(b);
    }
}

static void draw_clock(HDC dc, RECT area)
{
    WCHAR line1[16], line2[16];
    SYSTEMTIME st;
    RECT top = area, bottom = area;

    GetLocalTime(&st);
    GetTimeFormatEx(NULL, TIME_NOSECONDS, &st, NULL, line1, ARRAYSIZE(line1));
    GetDateFormatEx(NULL, DATE_SHORTDATE, &st, NULL, line2, ARRAYSIZE(line2), NULL);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, COL_TEXT);
    top.bottom = (area.top + area.bottom) / 2 + 2;
    bottom.top = top.bottom;
    DrawTextW(dc, line1, -1, &top, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
    DrawTextW(dc, line2, -1, &bottom, DT_CENTER | DT_TOP | DT_SINGLELINE);
}

static void on_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT c;
    HBRUSH bar = CreateSolidBrush(COL_BAR);
    RECT sr = start_rect(hwnd);

    GetClientRect(hwnd, &c);
    FillRect(dc, &c, bar);
    if (g_start_hot)
    {
        HBRUSH hot = CreateSolidBrush(COL_BAR_HOVER);
        FillRect(dc, &sr, hot);
        DeleteObject(hot);
    }
    draw_start_glyph(dc, sr, g_start_hot);
    draw_clock(dc, clock_rect(hwnd));

    DeleteObject(bar);
    EndPaint(hwnd, &ps);
}

/* Dock as an AppBar on the bottom edge: register, ask the shell for the space
 * a full-width bottom bar would take, claim it, and move there. */
static void appbar_dock(HWND hwnd)
{
    APPBARDATA abd = { sizeof(abd) };
    int screen_w = GetSystemMetrics(SM_CXSCREEN);
    int screen_h = GetSystemMetrics(SM_CYSCREEN);

    abd.hWnd = hwnd;
    abd.uCallbackMessage = WM_USER + 1;
    SHAppBarMessage(ABM_NEW, &abd);

    abd.uEdge = ABE_BOTTOM;
    abd.rc.left = 0;
    abd.rc.right = screen_w;
    abd.rc.top = screen_h - TASKBAR_HEIGHT;
    abd.rc.bottom = screen_h;
    SHAppBarMessage(ABM_QUERYPOS, &abd);
    /* honour the top the shell hands back, keep our height */
    abd.rc.bottom = abd.rc.top + TASKBAR_HEIGHT;
    SHAppBarMessage(ABM_SETPOS, &abd);

    MoveWindow(hwnd, abd.rc.left, abd.rc.top,
               abd.rc.right - abd.rc.left, abd.rc.bottom - abd.rc.top, TRUE);
}

static void appbar_remove(HWND hwnd)
{
    APPBARDATA abd = { sizeof(abd) };
    abd.hWnd = hwnd;
    SHAppBarMessage(ABM_REMOVE, &abd);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        appbar_dock(hwnd);
        SetTimer(hwnd, g_clock_timer, 1000, NULL);
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE:
    {
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        RECT sr = start_rect(hwnd);
        BOOL hot = PtInRect(&sr, pt);
        if (hot != g_start_hot) { g_start_hot = hot; InvalidateRect(hwnd, &sr, FALSE); }
        if (hot)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_start_hot) { g_start_hot = FALSE; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN:
    {
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        RECT sr = start_rect(hwnd);
        if (PtInRect(&sr, pt))
        {
            /* The Start panel is a separate program (sg-start), launched by a
             * documented signal. Until it exists, this is where it hooks in. */
            HWND panel = FindWindowW(L"SgStartPanel", NULL);
            if (panel) PostMessageW(panel, WM_USER + 10, 0, 0);
        }
        return 0;
    }
    case WM_PAINT:
        on_paint(hwnd);
        return 0;
    case WM_DESTROY:
        appbar_remove(hwnd);
        KillTimer(hwnd, g_clock_timer);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSW wc = { 0 };
    HWND hwnd;
    MSG msg;

    (void)prev; (void)cmd; (void)show;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = CLASS_NAME;
    if (!RegisterClassW(&wc)) return 1;

    /* WS_POPUP + tool window: no caption, and it stays off the Alt-Tab list and
     * (with topmost) above ordinary windows, the way a taskbar should. */
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, CLASS_NAME, L"sg-taskbar",
                           WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);

    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
