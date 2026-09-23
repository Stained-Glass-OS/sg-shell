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

/* The Start mark: an arched stained-glass window.
 *
 * A lancet window -- a rounded arch over a rectangular body -- with vertical
 * leading dividing it into three coloured lights. It reads unmistakably as
 * stained glass, the project's namesake, and looks nothing like any operating
 * system's logo: no four-square flag, no single glyph anyone else uses. On
 * hover the glass goes to one tint so the whole mark reads as a button.
 */
static void draw_start_glyph(HDC dc, RECT area, BOOL hot)
{
    int cx = (area.left + area.right) / 2;
    int cy = (area.top + area.bottom) / 2;
    int w = 16, h = 20;                     /* window bounds */
    int left = cx - w / 2, right = cx + w / 2;
    int top = cy - h / 2, bottom = cy + h / 2;
    int arch = 7;                            /* height of the arched top */
    int body = top + arch;                   /* where the arch meets the body */
    int l3 = left + w / 3, l23 = left + 2 * w / 3;
    static const COLORREF light[3] = {
        RGB(0x00, 0x78, 0xD7),               /* blue  */
        RGB(0xE8, 0xB3, 0x00),               /* amber */
        RGB(0xC0, 0x3A, 0x4B),               /* rose  */
    };
    HRGN win, arc, tmp;
    HPEN lead = CreatePen(PS_SOLID, 1, COL_BAR);
    HPEN oldpen;
    int i;

    /* The window silhouette: a rectangle body plus an elliptical arch on top,
     * unioned, used as a clip so the coloured lights fill exactly the glass. */
    win = CreateRectRgn(left, body, right, bottom);
    arc = CreateEllipticRgn(left, top, right, body + arch);
    tmp = CreateRectRgn(0, 0, 0, 0);
    CombineRgn(tmp, win, arc, RGN_OR);
    SelectClipRgn(dc, tmp);

    for (i = 0; i < 3; i++)
    {
        int x0 = (i == 0) ? left : (i == 1) ? l3 : l23;
        int x1 = (i == 0) ? l3   : (i == 1) ? l23 : right;
        HBRUSH b = CreateSolidBrush(hot ? COL_BAR_HOVER : light[i]);
        RECT strip = { x0, top, x1, bottom };
        FillRect(dc, &strip, b);
        DeleteObject(b);
    }
    SelectClipRgn(dc, NULL);

    /* The leading: outline the silhouette and draw the two vertical cames. */
    oldpen = SelectObject(dc, lead);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Arc(dc, left, top, right, body + arch, left, body, right, body);
    MoveToEx(dc, left, body, NULL);  LineTo(dc, left, bottom);
    MoveToEx(dc, right, body, NULL); LineTo(dc, right, bottom);
    MoveToEx(dc, left, bottom, NULL); LineTo(dc, right, bottom);
    MoveToEx(dc, l3, top + 2, NULL);  LineTo(dc, l3, bottom);
    MoveToEx(dc, l23, top + 2, NULL); LineTo(dc, l23, bottom);

    SelectObject(dc, oldpen);
    DeleteObject(lead);
    DeleteObject(win); DeleteObject(arc); DeleteObject(tmp);
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
