/* sg-restart-notice -- "Restart to finish": the popup.
 *
 * Something installed needs a restart to take effect -- a new kernel (a
 * security update, a Surface's touch and pen support), told by
 * /run/reboot-required, Debian's convention. The session's helper loop
 * (sg-session's sg-restart-notify) starts this once per boot. It shows,
 * above the tray as a notification does, what needs the restart, with
 * "Restart now" and "Later"; it goes by itself after half a minute, not while
 * the pointer is on it. It is also kept in the notification centre's history
 * (sg-notify; wine-sg 0815's HKCU\Software\Stained Glass\Notifications\
 * History), whose entry opens Settings > Update.
 *
 *   sg-restart-notice
 *
 * Built a second time with SG_BOOTSPACE as sg-bootspace-notice: "An
 * update could not be installed" -- a new Linux kernel did not fit on the
 * boot partition (sg-session's sg-boot-space left
 * /var/lib/stained-glass/notices/boot-space: kernel=, running=), the PC
 * keeps starting the kernel it has and tries again at each start; "Settings"
 * (Settings > Update) and "Dismiss". Started by sg-session's
 * sg-boot-space-notify once per boot. SG_BOOTSPACE_FILE (a Windows path) is
 * the gate's notice.
 *
 * SG_RESTART_FILE (a Windows path) is the gate's reboot-required;
 * SG_RESTART_NOTICE_DUMP=<file> writes what is shown there (UTF-8);
 * SG_RESTART_NOTICE_REBOOT=<file>: "Restart now" writes "reboot" there
 * instead of restarting.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sg-mode.h"
#include "sg-smooth.h"
#include "sg-dpi.h"

#ifdef SG_BOOTSPACE
#define CLASS_NAME L"SgBootSpaceNotice"
#define WINDOW_NAME L"Update could not be installed"
#define BUTTON_1 L"Settings"
#define BUTTON_2 L"Dismiss"
#else
#define CLASS_NAME L"SgRestartNotice"
#define WINDOW_NAME L"Restart to finish"
#define BUTTON_1 L"Restart now"
#define BUTTON_2 L"Later"
#endif
#define W_DIP 380
#define H_DIP 150
#define TIMEOUT_MS notice_timeout()

/* how long it shows: 30 s (SG_NOTICE_TIMEOUT_MS: the gates', which measure
 * it across a display scale change) */
static UINT notice_timeout(void)
{
    WCHAR v[16];
    UINT ms = 0;
    if (GetEnvironmentVariableW(L"SG_NOTICE_TIMEOUT_MS", v, 16)) ms = (UINT)wcstoul(v, NULL, 10);
    return ms >= 1000 ? ms : 30000;
}
#define RESTART_FILE L"Z:\\run\\reboot-required"
#define HISTORY_KEY L"Software\\Stained Glass\\Notifications\\History"

#ifdef SG_BOOTSPACE
#define BOOTSPACE_FILE L"Z:\\var\\lib\\stained-glass\\notices\\boot-space"
static WCHAR g_title[96] = L"An update could not be installed", g_body[900];
#else
static WCHAR g_title[96] = L"Restart to finish updating", g_body[900];
#endif
static HFONT g_font, g_font_bold, g_font_small;
static RECT g_review, g_dismiss, g_close;
static int g_hot;       /* 1 review, 2 dismiss, 3 close */
static BOOL g_tracking;
static int g_dpi = 96;

static int S(int dip) { return MulDiv(dip, g_dpi, 96); }

static char *slurp(const WCHAR *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    char *buf;
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!(buf = malloc(8193))) { CloseHandle(h); return NULL; }
    ReadFile(h, buf, 8192, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    return buf;
}

#ifdef SG_BOOTSPACE
/* "key=value" from the notice, as UTF-16 (empty if missing) */
static void field(const char *t, const char *key, WCHAR *out, int len)
{
    const char *p = t, *e;
    size_t k = strlen(key);
    int n;
    out[0] = 0;
    while (p && *p) {
        if (!strncmp(p, key, k) && p[k] == '=') {
            p += k + 1;
            for (e = p; *e && *e != '\n' && *e != '\r'; e++) ;
            n = MultiByteToWideChar(CP_UTF8, 0, p, (int)(e - p), out, len - 1);
            out[n > 0 ? n : 0] = 0;
            return;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
}

/* the kernel left out and the one still started */
static BOOL load(void)
{
    WCHAR path[MAX_PATH], kernel[128], running[128];
    char *t;
    if (!GetEnvironmentVariableW(L"SG_BOOTSPACE_FILE", path, MAX_PATH)) lstrcpyW(path, BOOTSPACE_FILE);
    if (!(t = slurp(path))) return FALSE;
    field(t, "kernel", kernel, ARRAYSIZE(kernel));
    field(t, "running", running, ARRAYSIZE(running));
    free(t);
    _snwprintf(g_body, ARRAYSIZE(g_body),
               L"There is not enough room on the boot partition for the new Linux kernel%ls%ls. "
               L"This PC keeps starting with %ls%ls, and tries again at each start.",
               kernel[0] ? L" " : L"", kernel, running[0] ? L"Linux " : L"the Linux it has", running);
    return TRUE;
}
#else
/* what needs the restart: reboot-required's line ("*** System restart
 * required: Surface touch and pen support ***"), else a new kernel */
static BOOL load(void)
{
    WCHAR path[MAX_PATH], why[300];
    char *t, *p, *e;
    int n;
    if (!GetEnvironmentVariableW(L"SG_RESTART_FILE", path, MAX_PATH)) lstrcpyW(path, RESTART_FILE);
    if (!(t = slurp(path))) return FALSE;
    why[0] = 0;
    p = t;
    while (*p == '*' || *p == ' ') p++;
    if (!_strnicmp(p, "System restart required", 23)) { p += 23; while (*p == ':' || *p == ' ') p++; }
    for (e = p; *e && *e != '\n' && *e != '\r' && *e != '*'; e++) ;
    while (e > p && e[-1] == ' ') e--;
    n = MultiByteToWideChar(CP_UTF8, 0, p, (int)(e - p), why, ARRAYSIZE(why) - 1);
    why[n > 0 ? n : 0] = 0;
    free(t);
    _snwprintf(g_body, ARRAYSIZE(g_body), L"%ls was installed and starts working when this PC restarts.",
               why[0] ? why : L"An update");
    return TRUE;
}
#endif

/* kept in the notification centre's history, as a toast is */
static void remember(void)
{
    HKEY hist, item;
    DWORD next = 0, size = sizeof(next);
    WCHAR name[16];
    FILETIME ft;
    ULONGLONG now;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, HISTORY_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hist, NULL)) return;
    RegQueryValueExW(hist, L"Next", NULL, NULL, (BYTE *)&next, &size);
    /* named as wine-sg's toasts are (eight digits): the centre and windows.ui
     * find an entry by its number in that form -- "0" was never cleared */
    _snwprintf(name, ARRAYSIZE(name), L"%08lu", next);
    if (!RegCreateKeyExW(hist, name, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &item, NULL)) {
        GetSystemTimeAsFileTime(&ft);
        now = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        RegSetValueExW(item, L"App", 0, REG_SZ, (const BYTE *)L"Stained Glass OS", sizeof(L"Stained Glass OS"));
        RegSetValueExW(item, L"Title", 0, REG_SZ, (const BYTE *)g_title, (lstrlenW(g_title) + 1) * sizeof(WCHAR));
        RegSetValueExW(item, L"Body", 0, REG_SZ, (const BYTE *)g_body, (lstrlenW(g_body) + 1) * sizeof(WCHAR));
        /* a link: Protocol 1 and the link in Launch, as a toast's */
        RegSetValueExW(item, L"Launch", 0, REG_SZ, (const BYTE *)L"ms-settings:windowsupdate",
                       sizeof(L"ms-settings:windowsupdate"));
        {
            DWORD one = 1;
            RegSetValueExW(item, L"Protocol", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
        }
        RegSetValueExW(item, L"Time", 0, REG_QWORD, (const BYTE *)&now, sizeof(now));
        RegCloseKey(item);
        next++;
        RegSetValueExW(hist, L"Next", 0, REG_DWORD, (const BYTE *)&next, sizeof(next));
    }
    RegCloseKey(hist);
    PostMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"SgNotificationsChanged"), 0, 0);
}

#ifdef SG_BOOTSPACE
/* "Settings": Settings > Update (the gate: "settings" in
 * SG_RESTART_NOTICE_REBOOT's file instead) */
static void restart(void)
{
    WCHAR path[MAX_PATH];
    FILE *f;
    if (GetEnvironmentVariableW(L"SG_RESTART_NOTICE_REBOOT", path, MAX_PATH)) {
        if ((f = _wfopen(path, L"w"))) { fputs("settings\n", f); fclose(f); }
        return;
    }
    ShellExecuteW(NULL, L"open", L"ms-settings:windowsupdate", NULL, NULL, SW_SHOWNORMAL);
}
#else
static void restart(void)
{
    WCHAR path[MAX_PATH];
    FILE *f;
    if (GetEnvironmentVariableW(L"SG_RESTART_NOTICE_REBOOT", path, MAX_PATH)) {
        if ((f = _wfopen(path, L"w"))) { fputs("reboot\n", f); fclose(f); }
        return;
    }
    ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_FLAG_PLANNED);
}
#endif

static void dump(void)
{
    WCHAR path[MAX_PATH];
    FILE *f;
    if (!GetEnvironmentVariableW(L"SG_RESTART_NOTICE_DUMP", path, MAX_PATH)) return;
    if (!(f = _wfopen(path, L"w, ccs=UTF-8"))) return;
    fwprintf(f, L"title: %ls\nbody: %ls\nbuttons: %ls, %ls\n", g_title, g_body, BUTTON_1, BUTTON_2);
    fclose(f);
}

static HFONT font(int pt, int weight)
{
    return CreateFontW(-S(pt), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void button(HDC dc, const RECT *r, const WCHAR *text, BOOL primary, BOOL hot, BOOL dark)
{
    COLORREF bg = primary ? sg_accent() : dark ? RGB(0x3C, 0x3C, 0x3C) : RGB(0xE5, 0xE5, 0xE5);
    HBRUSH b;
    RECT t = *r;
    if (hot) bg = sg_mix(bg, dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 12);
    b = CreateSolidBrush(bg);
    FillRect(dc, r, b);
    DeleteObject(b);
    SetTextColor(dc, primary ? RGB(255, 255, 255) : dark ? RGB(255, 255, 255) : RGB(0, 0, 0));
    SelectObject(dc, g_font);
    DrawTextW(dc, text, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    BOOL dark = sg_system_dark();
    COLORREF bg = dark ? RGB(0x2B, 0x2B, 0x2B) : RGB(0xF3, 0xF3, 0xF3);
    COLORREF fg = dark ? RGB(255, 255, 255) : RGB(0, 0, 0), sub = dark ? RGB(0xC8, 0xC8, 0xC8) : RGB(0x5A, 0x5A, 0x5A);
    RECT rc, r;
    HBRUSH b;
    GetClientRect(hwnd, &rc);
    b = CreateSolidBrush(bg); FillRect(dc, &rc, b); DeleteObject(b);
    r = rc; r.right = r.left + S(4);
    b = CreateSolidBrush(sg_accent()); FillRect(dc, &r, b); DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);

#ifdef SG_BOOTSPACE
    /* an exclamation mark in a ring, drawn as the arrow is */
    {
        struct sg_ss ss;
        HDC big = sg_ss_begin(&ss, dc, S(17), S(7), S(23), S(24), S(2));
        HPEN pen = CreatePen(PS_SOLID, S(2), sg_accent());
        HGDIOBJ op = SelectObject(big, pen), ob = SelectObject(big, GetStockObject(NULL_BRUSH));
        Ellipse(big, S(20), S(12), S(36), S(28));     /* sg-smooth: in the region */
        MoveToEx(big, S(28), S(15), NULL); LineTo(big, S(28), S(22));
        MoveToEx(big, S(28), S(24), NULL); LineTo(big, S(28), S(25));
        SelectObject(big, op); SelectObject(big, ob);
        sg_ss_end(&ss);
        DeleteObject(pen);
    }
#else
    /* a restart arrow: drawn, as the icon font's is not every machine's */
    {
        /* drawn soft-edged (sg-smooth.h): four times finer, averaged down */
        struct sg_ss ss;
        HDC big = sg_ss_begin(&ss, dc, S(17), S(7), S(23), S(24), S(2));
        HPEN pen = CreatePen(PS_SOLID, S(2), sg_accent());
        HGDIOBJ op = SelectObject(big, pen), ob = SelectObject(big, GetStockObject(NULL_BRUSH));
        /* most of a circle, open at the top right, with an arrowhead there */
        MoveToEx(big, S(33), S(15), NULL);
        AngleArc(big, S(28), S(20), S(7), 45, 290);   /* sg-smooth: in the region */
        MoveToEx(big, S(33), S(15), NULL); LineTo(big, S(28), S(15));
        MoveToEx(big, S(33), S(15), NULL); LineTo(big, S(33), S(10));
        SelectObject(big, op); SelectObject(big, ob);
        sg_ss_end(&ss);
        DeleteObject(pen);
    }
#endif
    SelectObject(dc, g_font_small);
    SetTextColor(dc, sub);
    r = rc; r.left = S(44); r.top = S(11); r.bottom = S(30);
    DrawTextW(dc, L"Stained Glass OS", -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
    SetTextColor(dc, fg);
    r = g_close;
    DrawTextW(dc, L"\x2715", -1, &r, DT_CENTER | DT_SINGLELINE | DT_VCENTER);

    SelectObject(dc, g_font_bold);
    r = rc; r.left = S(20); r.right -= S(16); r.top = S(36); r.bottom = S(58);
    DrawTextW(dc, g_title, -1, &r, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, g_font);
    SetTextColor(dc, sub);
    r.top = S(60); r.bottom = g_review.top - S(8);
    DrawTextW(dc, g_body, -1, &r, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);

    button(dc, &g_review, BUTTON_1, TRUE, g_hot == 1, dark);
    button(dc, &g_dismiss, BUTTON_2, FALSE, g_hot == 2, dark);
    EndPaint(hwnd, &ps);
}

static int hit(LPARAM lp)
{
    POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
    if (PtInRect(&g_review, pt)) return 1;
    if (PtInRect(&g_dismiss, pt)) return 2;
    if (PtInRect(&g_close, pt)) return 3;
    return 0;
}

static int g_stacked;   /* notices of its kind above which it sits */

/* the buttons' places in a W x H notice */
static void notice_rects(int w, int h)
{
    SetRect(&g_close, w - S(40), S(6), w - S(8), S(32));
    SetRect(&g_review, S(20), h - S(48), S(20) + (w - S(52)) / 2, h - S(16));
    SetRect(&g_dismiss, g_review.right + S(12), g_review.top, w - S(20), g_review.bottom);
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: paint(hwnd); return 0;
#ifndef SG_MUTANT_NOTICE_DPI_IGNORED
    case WM_DPICHANGED: {
        /* a new display scale while it shows (per-monitor v2, sg-dpi.h): its
         * text and buttons at it, in the corner above the taskbar again */
        HFONT old[3] = { g_font, g_font_bold, g_font_small };
        RECT work;
        int i, w, h;
        g_dpi = (int)sg_dpi_new(wp);
        g_font = font(10, FW_NORMAL);
        g_font_bold = font(11, FW_SEMIBOLD);
        g_font_small = font(9, FW_NORMAL);
        for (i = 0; i < 3; i++) DeleteObject(old[i]);
        w = S(W_DIP); h = S(H_DIP);
        notice_rects(w, h);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        SetWindowPos(hwnd, NULL, work.right - w - S(12), work.bottom - (h + S(12)) * (g_stacked + 1), w, h,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
#endif
    case WM_MOUSEMOVE: {
        int h = hit(lp);
        if (!g_tracking) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_tracking = TRUE;
            KillTimer(hwnd, 1);     /* not going while it is being read */
        }
        if (h != g_hot) { g_hot = h; InvalidateRect(hwnd, NULL, FALSE); }
        SetCursor(LoadCursorW(NULL, h ? (LPCWSTR)IDC_HAND : (LPCWSTR)IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        g_hot = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        SetTimer(hwnd, 1, TIMEOUT_MS / 2, NULL);
        return 0;
    case WM_LBUTTONUP:
        switch (hit(lp)) {
        case 1:
#ifndef SG_MUTANT_RESTART_BUTTON
            restart();
#endif
            DestroyWindow(hwnd);
            return 0;
        case 2: case 3: DestroyWindow(hwnd); return 0;
        default: return 0;   /* the text: nothing (a restart is never by accident) */
        }
    case WM_TIMER: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    WNDCLASSW wc = { 0 };
    RECT work;
    HWND hwnd, other = NULL;
    HDC dc;
    MSG m;
    int argc, w, h, stacked = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    (void)prev; (void)cmd; (void)show;
    (void)argc; (void)argv;
    if (!load()) return 0;
    dump();
    remember();
    /* drawn at the display scale (its sizes go by g_dpi), a new one too
     * (WM_DPICHANGED) -- unaware, Wine scaled its picture, soft at 175% */
#ifndef SG_MUTANT_DPI_UNAWARE
    sg_dpi_init();
#endif

    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    g_font = font(10, FW_NORMAL);
    g_font_bold = font(11, FW_SEMIBOLD);
    g_font_small = font(9, FW_NORMAL);
    w = S(W_DIP); h = S(H_DIP);
    notice_rects(w, h);

    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    while ((other = FindWindowExW(NULL, other, CLASS_NAME, NULL))) stacked++;
    g_stacked = stacked;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, CLASS_NAME, WINDOW_NAME,
                           WS_POPUP | WS_BORDER, work.right - w - S(12), work.bottom - (h + S(12)) * (stacked + 1),
                           w, h, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetTimer(hwnd, 1, TIMEOUT_MS, NULL);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
