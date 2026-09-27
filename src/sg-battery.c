/* sg-battery -- the taskbar's battery icon and its flyout.
 *
 * A notification-area icon (Shell_NotifyIcon, as sg-netflyout's) that says
 * how charged the battery is: the battery's outline filled to its charge, a
 * bolt while it charges. Its tooltip reads as Windows' does ("63% remaining",
 * "63% available (plugged in, charging)", "Fully charged (100%)"); clicking it
 * opens a flyout with the charge, the time left, and "Battery settings"
 * (Settings > Power). GetSystemPowerStatus is the source: wine-sg reads the
 * kernel's /sys/class/power_supply. A computer with no battery gets no icon:
 * the program exits.
 *
 *   sg-battery              the icon (sg-session starts it with the shell)
 *   sg-battery --dump       what it would show (stderr): the gate
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include "sg-mode.h"

#define FLY_W 360
#define FLY_H 150
#define WM_TRAY (WM_APP + 1)

static HWND g_tray, g_fly;
static NOTIFYICONDATAW g_nid;
static SYSTEM_POWER_STATUS g_ps;
static HFONT g_font, g_font_big, g_font_small;
static BOOL g_link_hot;
static UINT g_taskbar_created;

/* --- the state ------------------------------------------------------------------ */

static BOOL read_status(void)
{
    return GetSystemPowerStatus(&g_ps) && g_ps.BatteryFlag != 255 && !(g_ps.BatteryFlag & 128);
}

static int percent(void) { return g_ps.BatteryLifePercent <= 100 ? g_ps.BatteryLifePercent : 0; }
static BOOL plugged_in(void) { return g_ps.ACLineStatus == 1; }
static BOOL charging(void) { return (g_ps.BatteryFlag & 8) != 0; }
static BOOL full(void) { return plugged_in() && !charging() && percent() >= 95; }

static void describe(WCHAR *out, int cch)
{
    if (full()) _snwprintf(out, cch, L"Fully charged (%d%%)", percent());
    else if (plugged_in()) _snwprintf(out, cch, L"%d%% available (plugged in, %ls)", percent(), charging() ? L"charging" : L"not charging");
    else if (g_ps.BatteryLifeTime != (DWORD)-1)
        _snwprintf(out, cch, L"%lu hr %02lu min (%d%%) remaining", g_ps.BatteryLifeTime / 3600,
                   g_ps.BatteryLifeTime / 60 % 60, percent());
    else _snwprintf(out, cch, L"%d%% remaining", percent());
    out[cch - 1] = 0;
}

/* --- the icon, drawn here at four times the size and scaled down ------------------ */

static HICON make_icon(int size)
{
    enum { K = 4 };
    int S = size * K, x, y, i;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), S, -S, 1, 32, BI_RGB } };
    BITMAPINFO bo = { { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB } };
    DWORD *big, *px;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP canvas = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&big, NULL, 0), color, mask, old;
    ICONINFO ii = { TRUE };
    HICON icon;
    int left = S / 16, right = S - S / 8, top = S * 5 / 16, bottom = S * 11 / 16, pw = S / 14;
    HPEN pen = CreatePen(PS_SOLID, pw, RGB(0xFF, 0xFF, 0xFF)), op;
    HBRUSH white = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF)), ob;
    RECT fill, nub;

    old = SelectObject(dc, canvas);
    memset(big, 0, S * S * 4);
    /* the body and its terminal */
    op = SelectObject(dc, pen);
    ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, left, top, right, bottom);
    SetRect(&nub, right, top + (bottom - top) / 3, right + S / 16, bottom - (bottom - top) / 3);
    FillRect(dc, &nub, white);
    /* the charge */
    SetRect(&fill, left + pw * 2, top + pw * 2, left + pw * 2 + (right - left - pw * 4) * percent() / 100, bottom - pw * 2);
    if (fill.right > fill.left) FillRect(dc, &fill, white);
    if (plugged_in())
    {
        /* a bolt across it, cut out of the fill and drawn over */
        POINT bolt[6] = { { S * 9 / 16, S * 3 / 16 }, { S * 5 / 16, S * 17 / 32 }, { S / 2, S * 17 / 32 },
                          { S * 7 / 16, S * 13 / 16 }, { S * 11 / 16, S * 15 / 32 }, { S / 2, S * 15 / 32 } };
        HPEN edge = CreatePen(PS_SOLID, pw * 2, RGB(0, 0, 0));
        SelectObject(dc, edge);
        SelectObject(dc, white);
        Polygon(dc, bolt, 6);
        SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, bolt, 6);
        SelectObject(dc, op);
        DeleteObject(edge);
    }
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen); DeleteObject(white);
    GdiFlush();

    color = CreateDIBSection(dc, &bo, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++)
        {
            int sum = 0, u, v;
            BYTE a;
            for (v = 0; v < K; v++) for (u = 0; u < K; u++) sum += big[(y * K + v) * S + x * K + u] & 0xFF;
            a = (BYTE)(sum / (K * K));
            /* white on the dark taskbar, black on the light one */
            px[y * size + x] = a ? ((DWORD)a << 24) | (sg_system_dark() ? 0x00FFFFFF : 0) : 0;
        }
    (void)i;
    SelectObject(dc, old);
    DeleteObject(canvas);
    mask = CreateBitmap(size, size, 1, 1, NULL);
    ii.hbmColor = color;
    ii.hbmMask = mask;
    icon = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask); DeleteDC(dc);
    return icon;
}

static void tray_update(BOOL add)
{
    HICON old = g_nid.hIcon;
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_tray;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = make_icon(GetSystemMetrics(SM_CXSMICON));
    describe(g_nid.szTip, ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_nid);
    if (old) DestroyIcon(old);
}

/* --- the flyout ------------------------------------------------------------------- */

static RECT link_rect(void) { RECT r = { 20, FLY_H - 40, 200, FLY_H - 16 }; return r; }

static void fly_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    BOOL dark = sg_system_dark();
    COLORREF bg = dark ? RGB(0x1F, 0x1F, 0x1F) : RGB(0xF2, 0xF2, 0xF2), text = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0),
             subtle = dark ? RGB(0xA8, 0xA8, 0xA8) : RGB(0x5A, 0x5A, 0x5A);
    HBRUSH b = CreateSolidBrush(bg);
    RECT c, r;
    WCHAR buf[128];
    HICON big;

    GetClientRect(hwnd, &c);
    FillRect(dc, &c, b);
    DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);
    big = make_icon(48);
    DrawIconEx(dc, 20, 22, big, 48, 48, 0, NULL, DI_NORMAL);
    DestroyIcon(big);
    SelectObject(dc, g_font_big);
    SetTextColor(dc, text);
    SetRect(&r, 84, 16, FLY_W - 16, 60);
    _snwprintf(buf, ARRAYSIZE(buf), L"%d%%", percent());
    DrawTextW(dc, buf, -1, &r, DT_SINGLELINE | DT_VCENTER);
    SelectObject(dc, g_font_small);
    SetTextColor(dc, subtle);
    SetRect(&r, 84, 60, FLY_W - 16, 80);
    describe(buf, ARRAYSIZE(buf));
    DrawTextW(dc, buf, -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, g_font);
    SetTextColor(dc, dark ? RGB(0xB9, 0x8C, 0xF0) : sg_accent());
    r = link_rect();
    {
        LOGFONTW lf;
        HFONT ul;
        GetObjectW(g_font, sizeof(lf), &lf);
        lf.lfUnderline = g_link_hot;
        ul = CreateFontIndirectW(&lf);
        SelectObject(dc, ul);
        DrawTextW(dc, L"Battery settings", -1, &r, DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, g_font);
        DeleteObject(ul);
    }
    EndPaint(hwnd, &ps);
}

static void place_flyout(void)
{
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    SetWindowPos(g_fly, HWND_TOPMOST, work.right - FLY_W - 12, work.bottom - FLY_H - 12, FLY_W, FLY_H, SWP_NOACTIVATE);
}

static LRESULT CALLBACK fly_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT: fly_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT r = link_rect();
        BOOL hot = PtInRect(&r, pt);
        if (hot != g_link_hot) { g_link_hot = hot; InvalidateRect(hwnd, &r, FALSE); }
        SetCursor(LoadCursorW(NULL, (const WCHAR *)(hot ? IDC_HAND : IDC_ARROW)));
        return 0;
    }
    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT r = link_rect();
        if (PtInRect(&r, pt))
        {
            ShowWindow(hwnd, SW_HIDE);
            ShellExecuteW(NULL, NULL, L"ms-settings:batterysaver", NULL, NULL, SW_SHOWNORMAL);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbar_created && g_taskbar_created) { tray_update(TRUE); return 0; }
    if (sg_mode_changed(msg, lp))
    {
        tray_update(FALSE);
        if (IsWindowVisible(g_fly)) InvalidateRect(g_fly, NULL, FALSE);
        return 0;
    }
    switch (msg)
    {
    case WM_TRAY:
        if (lp == WM_LBUTTONUP)
        {
            if (IsWindowVisible(g_fly)) ShowWindow(g_fly, SW_HIDE);
            else
            {
                read_status();
                place_flyout();
                ShowWindow(g_fly, SW_SHOW);
                SetForegroundWindow(g_fly);
                InvalidateRect(g_fly, NULL, FALSE);
            }
        }
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
        {
            HMENU m = CreatePopupMenu();
            POINT p;
            AppendMenuW(m, MF_STRING, 1, L"Power && sleep settings");
            GetCursorPos(&p);
            SetForegroundWindow(hwnd);
            if (TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, hwnd, NULL) == 1)
                ShellExecuteW(NULL, NULL, L"ms-settings:powersleep", NULL, NULL, SW_SHOWNORMAL);
            DestroyMenu(m);
        }
        return 0;
    case WM_POWERBROADCAST:
    case WM_TIMER:
        if (read_status())
        {
            tray_update(FALSE);
            if (IsWindowVisible(g_fly)) InvalidateRect(g_fly, NULL, FALSE);
        }
        return msg == WM_POWERBROADCAST ? TRUE : 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HFONT make_font(int height, int weight)
{
    return CreateFontW(-height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSW wc = { 0 };
    MSG msg;

    (void)prev; (void)show;
    if (!read_status())
    {
        /* no battery: no icon */
        if (cmdline && wcsstr(cmdline, L"--dump")) fprintf(stderr, "BATTERY none\n");
        return 0;
    }
    if (cmdline && wcsstr(cmdline, L"--dump"))
    {
        WCHAR tip[128];
        char a[256];
        describe(tip, ARRAYSIZE(tip));
        WideCharToMultiByte(CP_UTF8, 0, tip, -1, a, sizeof(a), NULL, NULL);
        fprintf(stderr, "BATTERY %d%% %s %s\nTIP %s\n", percent(), plugged_in() ? "ac" : "battery",
                charging() ? "charging" : "not-charging", a);
        return 0;
    }
    if (FindWindowW(L"SgBatteryTray", NULL)) return 0;   /* one per session */

    g_font = make_font(14, FW_NORMAL);
    g_font_small = make_font(12, FW_NORMAL);
    g_font_big = make_font(34, FW_LIGHT);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    wc.lpfnWndProc = tray_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SgBatteryTray";
    RegisterClassW(&wc);
    g_tray = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Battery", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    wc.lpfnWndProc = fly_proc;
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.lpszClassName = L"SgBatteryFlyout";
    RegisterClassW(&wc);
    /* owned by the (never shown) tray window: no taskbar button of its own */
    g_fly = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Battery", WS_POPUP,
                            0, 0, FLY_W, FLY_H, g_tray, NULL, inst, NULL);
    tray_update(TRUE);
    SetTimer(g_tray, 1, 30000, NULL);
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
