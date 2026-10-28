/*
 * sg-calendar -- the taskbar clock's calendar flyout (David 2026-10-28: "the taskbar
 * time/date does not come up with a calendar when you click on it").
 *
 * As on Windows 10: a click on the clock opens, above it, the time with its seconds, the
 * long date (a link to the date and time settings), and the month: its name, arrows to the
 * month before and after, the days of the week from the locale's first day, and six weeks
 * of days with today in the accent colour and the other months' days dimmed. The mouse
 * wheel and Page Up / Page Down change the month, Home comes back to this one, a day is
 * selected by a click, Esc or a click elsewhere closes it, and so does a second click on
 * the clock.
 *
 *   sg-calendar64.exe --background   the session's copy, hidden (sg-session starts it)
 *   sg-calendar64.exe --show         open it (or, if it is open, close it): the taskbar's
 *                                    clock posts SgCalendarToggle to SgCalendarHost, and
 *                                    starts this only when no copy runs yet
 *   sg-calendar64.exe --dump YYYY-MM the month's grid as text (the gate's)
 *
 * SG_CALENDAR_DUMP=FILE: the shown state after each paint (the gate's).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include "sg-mode.h"
#include "sg-round.h"
#include "sg-dpi.h"

#define FLY_W 360
#define FLY_H 508
#define CELL_W 46
#define CELL_H 40
#define GRID_X 17
#define GRID_Y 178
#define TICK_TIMER 1

static HWND g_host, g_fly;
static HFONT g_font, g_font_big, g_font_month, g_font_day;
static int g_dpi = 96;
#define S(x) MulDiv((x), g_dpi, 96)
static int g_year, g_month;                 /* the month shown */
static int g_sel_year, g_sel_month, g_sel_day;  /* the day clicked, 0: none */
static int g_hot = -1;                      /* the cell under the pointer, -1: none */
enum { HOT_NONE, HOT_DATE, HOT_UP, HOT_DOWN, HOT_SETTINGS };
static int g_hot_part;
static DWORD g_hidden_tick;                 /* when it hid on losing activation */
static BOOL g_hidden_by_press;              /* ...by a press on the taskbar's clock */
static UINT g_toggle_msg;

/* ---- the month ------------------------------------------------------------------------------ */

static int days_in(int y, int m)
{
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0)) return 29;
    return d[m - 1];
}

/* 0 Sunday .. 6 Saturday */
static int weekday(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* the locale's first day of the week, 0 Sunday .. 6 Saturday (LOCALE_IFIRSTDAYOFWEEK: 0 Monday) */
static int first_weekday(void)
{
    WCHAR v[4] = L"";
    const WCHAR *forced = _wgetenv(L"SG_CALENDAR_FIRSTDAY");
#ifdef SG_MUTANT_CALENDAR_SUNDAY
    return 0;
#endif
    if (forced && *forced) return _wtoi(forced) % 7;
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, v, ARRAYSIZE(v))) return 0;
    return (_wtoi(v) + 1) % 7;
}

/* cell i (0..41) of month y-m: its date */
static void cell_date(int y, int m, int i, int *cy, int *cm, int *cd)
{
    int lead = (weekday(y, m, 1) - first_weekday() + 7) % 7, d = i - lead + 1;
    *cy = y; *cm = m;
    if (d < 1) {
        if (--*cm < 1) { *cm = 12; --*cy; }
        d += days_in(*cy, *cm);
    } else if (d > days_in(y, m)) {
        d -= days_in(y, m);
        if (++*cm > 12) { *cm = 1; ++*cy; }
    }
    *cd = d;
}

static void today(int *y, int *m, int *d)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    *y = st.wYear; *m = st.wMonth; *d = st.wDay;
}

static void show_month(int y, int m)
{
    while (m < 1) { m += 12; y--; }
    while (m > 12) { m -= 12; y++; }
    g_year = y; g_month = m;
    g_hot = -1;
    if (g_fly) InvalidateRect(g_fly, NULL, FALSE);
}

/* ---- where things are ------------------------------------------------------------------------ */

static RECT date_rect(void)     { RECT r = { S(24), S(72), S(FLY_W - 24), S(98) }; return r; }
static RECT up_rect(void)       { RECT r = { S(FLY_W - 92), S(126), S(FLY_W - 56), S(158) }; return r; }
static RECT down_rect(void)     { RECT r = { S(FLY_W - 52), S(126), S(FLY_W - 16), S(158) }; return r; }
static RECT settings_rect(void) { RECT r = { S(24), S(FLY_H - 44), S(240), S(FLY_H - 16) }; return r; }
static RECT cell_rect(int i)
{
    RECT r;
    r.left = S(GRID_X + (i % 7) * CELL_W);
    r.top = S(GRID_Y + 26 + (i / 7) * CELL_H);
    r.right = r.left + S(CELL_W);
    r.bottom = r.top + S(CELL_H);
    return r;
}

static int cell_at(POINT pt)
{
    int i;
    for (i = 0; i < 42; i++) { RECT r = cell_rect(i); if (PtInRect(&r, pt)) return i; }
    return -1;
}

static int part_at(POINT pt)
{
    RECT r;
    if ((r = date_rect(), PtInRect(&r, pt))) return HOT_DATE;
    if ((r = up_rect(), PtInRect(&r, pt))) return HOT_UP;
    if ((r = down_rect(), PtInRect(&r, pt))) return HOT_DOWN;
    if ((r = settings_rect(), PtInRect(&r, pt))) return HOT_SETTINGS;
    return HOT_NONE;
}

/* ---- painting -------------------------------------------------------------------------------- */

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, r, b);
    DeleteObject(b);
}

static void frame(HDC dc, const RECT *r, COLORREF c, int w)
{
    RECT e = *r;
    int i;
    for (i = 0; i < w; i++) {
        HBRUSH b = CreateSolidBrush(c);
        FrameRect(dc, &e, b);
        DeleteObject(b);
        InflateRect(&e, -1, -1);
    }
}

static void link(HDC dc, const WCHAR *text, RECT r, COLORREF c, BOOL hot, UINT align)
{
    LOGFONTW lf;
    HFONT f;
    GetObjectW(g_font, sizeof(lf), &lf);
    lf.lfUnderline = hot;
    f = CreateFontIndirectW(&lf);
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | align | DT_END_ELLIPSIS);
    SelectObject(dc, g_font);
    DeleteObject(f);
}

/* an arrow: up or down, a chevron of two lines */
static void chevron(HDC dc, RECT r, BOOL up, COLORREF c)
{
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, w = S(6), h = S(3);
    HPEN pen = CreatePen(PS_SOLID, S(1) > 1 ? S(1) + 1 : 2, c), old = SelectObject(dc, pen);
    MoveToEx(dc, cx - w, up ? cy + h : cy - h, NULL);
    LineTo(dc, cx, up ? cy - h : cy + h);
    LineTo(dc, cx + w + 1, up ? cy + h + 1 : cy - h - 1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

static void dump_state(void)
{
    const WCHAR *path = _wgetenv(L"SG_CALENDAR_DUMP");
    FILE *f;
    int ty, tm, td, i, y, m, d;
    if (!path || !*path || !(f = _wfopen(path, L"w"))) return;
    today(&ty, &tm, &td);
    fprintf(f, "visible %d\nmonth %04d-%02d\nfirstday %d\ntoday %04d-%02d-%02d\nselected %04d-%02d-%02d\n",
            IsWindowVisible(g_fly) ? 1 : 0, g_year, g_month, first_weekday(), ty, tm, td, g_sel_year, g_sel_month, g_sel_day);
    fprintf(f, "cells");
    for (i = 0; i < 42; i++) { cell_date(g_year, g_month, i, &y, &m, &d); fprintf(f, " %s%d", m == g_month ? "" : "~", d); }
    fprintf(f, "\n");
    {
        RECT w;
        GetWindowRect(g_fly, &w);
        fprintf(f, "rect %ld %ld %ld %ld\n", w.left, w.top, w.right, w.bottom);
    }
    fclose(f);
}

static void fly_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps), dc;
    BOOL dark = sg_system_dark();
    COLORREF bg = dark ? RGB(0x1F, 0x1F, 0x1F) : RGB(0xF2, 0xF2, 0xF2), text = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0),
             subtle = dark ? RGB(0x80, 0x80, 0x80) : RGB(0x8A, 0x8A, 0x8A),
             line = dark ? RGB(0x40, 0x40, 0x40) : RGB(0xD6, 0xD6, 0xD6),
             hot = dark ? RGB(0x5A, 0x5A, 0x5A) : RGB(0xB8, 0xB8, 0xB8),
             accent = sg_accent(), linkc = dark ? RGB(0xB9, 0x8C, 0xF0) : sg_accent();
    RECT c, r;
    WCHAR buf[128];
    SYSTEMTIME st;
    HBITMAP bmp, oldbmp;
    int i, ty, tm, td, first = first_weekday();

    GetClientRect(hwnd, &c);
    /* drawn off screen, then copied: the clock ticks once a second, without a flicker */
    dc = CreateCompatibleDC(wdc);
    bmp = CreateCompatibleBitmap(wdc, c.right, c.bottom);
    oldbmp = SelectObject(dc, bmp);
    fill(dc, &c, bg);
    SetBkMode(dc, TRANSPARENT);

    /* the time, with its seconds, and the long date */
    GetLocalTime(&st);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, NULL, buf, ARRAYSIZE(buf));
    SelectObject(dc, g_font_big);
    SetTextColor(dc, text);
    SetRect(&r, S(22), S(14), S(FLY_W - 16), S(70));
    DrawTextW(dc, buf, -1, &r, DT_SINGLELINE | DT_VCENTER);
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, NULL, buf, ARRAYSIZE(buf), NULL);
    link(dc, buf, date_rect(), linkc, g_hot_part == HOT_DATE, DT_LEFT);

    SetRect(&r, 0, S(112), c.right, S(112) + 1);
    fill(dc, &r, line);

    /* the month, and its arrows */
    {
        SYSTEMTIME ms = { 0 };
        ms.wYear = (WORD)g_year; ms.wMonth = (WORD)g_month; ms.wDay = 1;
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_YEARMONTH, &ms, NULL, buf, ARRAYSIZE(buf), NULL);
    }
    SelectObject(dc, g_font_month);
    SetTextColor(dc, text);
    SetRect(&r, S(24), S(126), S(FLY_W - 100), S(158));
    DrawTextW(dc, buf, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
    r = up_rect();
    if (g_hot_part == HOT_UP) fill(dc, &r, line);
    chevron(dc, r, TRUE, text);
    r = down_rect();
    if (g_hot_part == HOT_DOWN) fill(dc, &r, line);
    chevron(dc, r, FALSE, text);

    /* the days of the week, from the locale's first */
    SelectObject(dc, g_font_day);
    for (i = 0; i < 7; i++) {
        int wd = (first + i) % 7;   /* 0 Sunday */
        WCHAR name[16] = L"";
        /* LOCALE_SSHORTESTDAYNAME1 is Monday ... 7 Sunday */
        if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SSHORTESTDAYNAME1 + (wd + 6) % 7, name, ARRAYSIZE(name)) || !name[0]) {
            GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SABBREVDAYNAME1 + (wd + 6) % 7, name, ARRAYSIZE(name));
            name[2] = 0;
        }
        SetRect(&r, S(GRID_X + i * CELL_W), S(GRID_Y), S(GRID_X + (i + 1) * CELL_W), S(GRID_Y + 24));
        SetTextColor(dc, text);
        DrawTextW(dc, name, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }

    /* six weeks of days */
    today(&ty, &tm, &td);
    for (i = 0; i < 42; i++) {
        int y, m, d;
        BOOL is_today, in_month, selected;
        cell_date(g_year, g_month, i, &y, &m, &d);
        r = cell_rect(i);
        is_today = y == ty && m == tm && d == td;
        in_month = m == g_month;
        selected = g_sel_day && y == g_sel_year && m == g_sel_month && d == g_sel_day;
        if (is_today) {
            RECT in = r;
            InflateRect(&in, -S(1), -S(1));
            fill(dc, &in, accent);
            if (selected || i == g_hot) frame(dc, &in, dark ? RGB(0, 0, 0) : RGB(0xFF, 0xFF, 0xFF), S(2));
        } else if (selected) {
            RECT in = r;
            InflateRect(&in, -S(1), -S(1));
            frame(dc, &in, accent, S(2));
        } else if (i == g_hot) {
            RECT in = r;
            InflateRect(&in, -S(1), -S(1));
            frame(dc, &in, hot, S(2));
        }
        swprintf(buf, ARRAYSIZE(buf), L"%d", d);
        SetTextColor(dc, is_today ? RGB(0xFF, 0xFF, 0xFF) : in_month ? text : subtle);
        DrawTextW(dc, buf, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }

    SetRect(&r, 0, S(FLY_H - 56), c.right, S(FLY_H - 56) + 1);
    fill(dc, &r, line);
    SelectObject(dc, g_font);
    link(dc, L"Date and time settings", settings_rect(), linkc, g_hot_part == HOT_SETTINGS, DT_LEFT);

    BitBlt(wdc, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
    dump_state();
}

/* ---- showing and hiding ---------------------------------------------------------------------- */

static void place_flyout(void)
{
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    SetWindowPos(g_fly, HWND_TOPMOST, work.right - S(FLY_W + 12), work.bottom - S(FLY_H + 12), S(FLY_W), S(FLY_H), SWP_NOACTIVATE);
}

static void show_flyout(void)
{
    int y, m, d;
    today(&y, &m, &d);
    g_sel_day = 0;
    show_month(y, m);       /* opens on this month, as on Windows */
    place_flyout();
    ShowWindow(g_fly, SW_SHOW);
    SetForegroundWindow(g_fly);
    SetTimer(g_fly, TICK_TIMER, 1000, NULL);
    InvalidateRect(g_fly, NULL, FALSE);
    dump_state();
}

static void hide_flyout(void)
{
    if (!IsWindowVisible(g_fly)) return;
    ShowWindow(g_fly, SW_HIDE);
    KillTimer(g_fly, TICK_TIMER);
    dump_state();
}

/* The press of a second click on the clock takes the activation from the flyout, which
 * hides; the click's release then asked to open it again. A deactivation by a press on
 * the taskbar's clock (the taskbar's right end) leaves it closed for that click
 * (sg-flyout.h does the same for tray icons). */
static BOOL pressed_on_clock(void)
{
    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
    RECT r;
    POINT pt;
    if (!tray || !(GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000)) return FALSE;
    if (!GetWindowRect(tray, &r) || !GetCursorPos(&pt) || !PtInRect(&r, pt)) return FALSE;
    return r.right - r.left > r.bottom - r.top ? pt.x >= r.right - S(120) : pt.y >= r.bottom - S(80);
}

static void toggle(void)
{
#ifndef SG_MUTANT_CALENDAR_NOTOGGLE
    if (IsWindowVisible(g_fly)) { hide_flyout(); return; }
#endif
#ifndef SG_MUTANT_CALENDAR_REOPENS
    if (g_hidden_by_press && GetTickCount() - g_hidden_tick < 3000) { g_hidden_by_press = FALSE; return; }
#endif
    show_flyout();
}

static void make_fonts(void);

static LRESULT CALLBACK fly_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT: fly_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED:
        g_dpi = (int)sg_dpi_new(wp);
        make_fonts();
        sg_dpi_apply_rect(hwnd, lp);
        place_flyout();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_TIMER:
        if (wp == TICK_TIMER) { RECT r = { 0, 0, S(FLY_W), S(112) }; InvalidateRect(hwnd, &r, FALSE); }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && IsWindowVisible(hwnd)) {
            g_hidden_by_press = pressed_on_clock();
            g_hidden_tick = GetTickCount();
            hide_flyout();
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) hide_flyout();
        else if (wp == VK_PRIOR) show_month(g_year, g_month - 1);
        else if (wp == VK_NEXT) show_month(g_year, g_month + 1);
        else if (wp == VK_HOME) { int y, m, d; today(&y, &m, &d); show_month(y, m); }
        return 0;
    case WM_MOUSEWHEEL:
        show_month(g_year, g_month + ((short)HIWORD(wp) > 0 ? -1 : 1));
        return 0;
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int cell = cell_at(pt), part = part_at(pt);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        if (cell != g_hot || part != g_hot_part) { g_hot = cell; g_hot_part = part; InvalidateRect(hwnd, NULL, FALSE); }
        SetCursor(LoadCursorW(NULL, (const WCHAR *)(part == HOT_DATE || part == HOT_SETTINGS ? IDC_HAND : IDC_ARROW)));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hot != -1 || g_hot_part) { g_hot = -1; g_hot_part = HOT_NONE; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int cell = cell_at(pt);
        switch (part_at(pt))
        {
        case HOT_UP: show_month(g_year, g_month - 1); return 0;
        case HOT_DOWN: show_month(g_year, g_month + 1); return 0;
        case HOT_DATE:
        case HOT_SETTINGS:
            hide_flyout();
            if ((INT_PTR)ShellExecuteW(NULL, NULL, L"ms-settings:dateandtime", NULL, NULL, SW_SHOWNORMAL) <= 32)
                ShellExecuteW(NULL, NULL, L"control.exe", L"timedate.cpl", NULL, SW_SHOWNORMAL);
            return 0;
        }
        if (cell >= 0) {
            int y, m, d;
            cell_date(g_year, g_month, cell, &y, &m, &d);
            g_sel_year = y; g_sel_month = m; g_sel_day = d;
            /* a day of the month before or after: that month, as on Windows */
            if (m != g_month) show_month(y, m);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_toggle_msg && g_toggle_msg) { toggle(); return 0; }
    if (sg_mode_changed(msg, lp)) { if (IsWindowVisible(g_fly)) InvalidateRect(g_fly, NULL, FALSE); return 0; }
    if (msg == WM_TIMECHANGE && IsWindowVisible(g_fly)) { InvalidateRect(g_fly, NULL, FALSE); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HFONT make_font(int height, int weight)
{
    return CreateFontW(-S(height), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void make_fonts(void)
{
    if (g_font) DeleteObject(g_font);
    if (g_font_big) DeleteObject(g_font_big);
    if (g_font_month) DeleteObject(g_font_month);
    if (g_font_day) DeleteObject(g_font_day);
    g_font = make_font(14, FW_NORMAL);
    g_font_big = make_font(40, FW_LIGHT);
    g_font_month = make_font(15, FW_SEMIBOLD);
    g_font_day = make_font(13, FW_NORMAL);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSW wc = { 0 };
    MSG msg;
    HWND running;
    BOOL show_now = cmdline && wcsstr(cmdline, L"--show") != NULL;
    const WCHAR *dump = cmdline ? wcsstr(cmdline, L"--dump") : NULL;

    (void)prev; (void)show;
    if (dump) {
        /* the month's grid, for the gate: --dump YYYY-MM */
        int y = 0, m = 0, i, cy, cm, cd;
        swscanf(dump + 6, L" %d-%d", &y, &m);
        if (y < 1601 || m < 1 || m > 12) return 2;
        printf("firstday %d\ncells", first_weekday());
        for (i = 0; i < 42; i++) { cell_date(y, m, i, &cy, &cm, &cd); printf(" %s%d", cm == m ? "" : "~", cd); }
        printf("\n");
        return 0;
    }
    g_toggle_msg = RegisterWindowMessageW(L"SgCalendarToggle");
    if ((running = FindWindowW(L"SgCalendarHost", NULL))) {   /* one per session: that one opens or closes */
        if (show_now) PostMessageW(running, g_toggle_msg, 0, 0);
        return 0;
    }
    sg_dpi_init();
    g_dpi = (int)sg_dpi_for(NULL);
    make_fonts();

    wc.lpfnWndProc = host_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SgCalendarHost";
    RegisterClassW(&wc);
    g_host = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Calendar", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    wc.lpfnWndProc = fly_proc;
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.lpszClassName = L"SgCalendarFlyout";
    RegisterClassW(&wc);
    /* owned by the (never shown) host: no taskbar button of its own */
    g_fly = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Calendar", WS_POPUP,
                            0, 0, S(FLY_W), S(FLY_H), g_host, NULL, inst, NULL);
    sg_round_corners(g_fly);
    {
        int y, m, d;
        today(&y, &m, &d);
        show_month(y, m);
    }
    if (show_now) show_flyout();
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
