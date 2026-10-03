/* sg-defender-notice -- "SG Defender quarantined a file": the popup.
 *
 * sg-defender (sg-session, root) moves an infected download to quarantine
 * and leaves a notice for its owner; the session's helper loop
 * (sg-defender-notify) starts this once per notice, with its id. It shows,
 * above the tray as a notification does, what was found in which file and
 * where it was, with "Review" -- Settings > Virus & threat protection, where
 * the file can be restored or deleted -- and "Dismiss". It goes by itself
 * after half a minute, not while the pointer is on it. Several stack upward.
 *
 *   sg-defender-notice ID
 *
 * SG_DEFENDER_DIR (a Windows path) is the gate's defender folder;
 * SG_DEFENDER_NOTICE_DUMP=<file> writes what is shown there (UTF-8).
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

#define CLASS_NAME L"SgDefenderNotice"
#define W_DIP 380
#define H_DIP 150
#define TIMEOUT_MS 30000
#define DEFENDER_DIR L"Z:\\var\\lib\\stained-glass\\defender"
#define COL_THREAT RGB(0xC4, 0x2B, 0x1C)

static WCHAR g_title[96] = L"Threat quarantined", g_body[900];
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

/* a string field of sg-defender's notice ("key": "value") */
static void field(const char *text, const char *key, WCHAR *out, int cch)
{
    char pat[64];
    const char *p, *e;
    int n;
    out[0] = 0;
    _snprintf(pat, sizeof(pat), "\"%s\": \"", key);
    if (!text || !(p = strstr(text, pat))) return;
    p += strlen(pat);
    if (!(e = strchr(p, '"'))) return;
    n = MultiByteToWideChar(CP_UTF8, 0, p, (int)(e - p), out, cch - 1);
    out[n > 0 ? n : 0] = 0;
}

static unsigned long unix_uid(void)
{
    char *t = slurp(L"Z:\\proc\\self\\status"), *p;
    unsigned long uid = (unsigned long)-1;
    if (t && (p = strstr(t, "\nUid:"))) uid = strtoul(p + 5, NULL, 10);
    free(t);
    return uid;
}

static BOOL load(const WCHAR *id)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH], name[MAX_PATH], folder[MAX_PATH], sig[128];
    const WCHAR *where, *p;
    char *t;
    for (p = id; *p; p++)       /* an id, not a path */
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || *p == '-')) return FALSE;
    if (!GetEnvironmentVariableW(L"SG_DEFENDER_DIR", dir, MAX_PATH)) lstrcpyW(dir, DEFENDER_DIR);
    _snwprintf(path, MAX_PATH, L"%ls\\notices\\%lu\\%ls.json", dir, unix_uid(), id);
    if (!(t = slurp(path))) return FALSE;
    field(t, "name", name, MAX_PATH);
    field(t, "folder", folder, MAX_PATH);
    field(t, "signature", sig, ARRAYSIZE(sig));
    free(t);
    if (!name[0]) return FALSE;
    /* the folder as a person names it: Downloads, Desktop, or the folder in them */
    where = wcsrchr(folder, '/');
    where = where && where[1] ? where + 1 : folder;
    _snwprintf(g_body, ARRAYSIZE(g_body),
               L"%ls (in %ls) contained %ls. It was moved to quarantine, where it can't run.",
               name, where, sig[0] ? sig : L"a threat");
    return TRUE;
}

static void dump(void)
{
    WCHAR path[MAX_PATH];
    FILE *f;
    if (!GetEnvironmentVariableW(L"SG_DEFENDER_NOTICE_DUMP", path, MAX_PATH)) return;
    if (!(f = _wfopen(path, L"w, ccs=UTF-8"))) return;
    fwprintf(f, L"title: %ls\nbody: %ls\nbuttons: Review, Dismiss\n", g_title, g_body);
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
    b = CreateSolidBrush(COL_THREAT); FillRect(dc, &r, b); DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);

    /* the shield: drawn, as the icon font's is not every machine's */
    {
        POINT sh[] = { { S(20), S(14) }, { S(28), S(11) }, { S(36), S(14) }, { S(36), S(21) },
                       { S(28), S(28) }, { S(20), S(21) } };
        HPEN pen = CreatePen(PS_SOLID, 1, COL_THREAT);
        b = CreateSolidBrush(COL_THREAT);
        SelectObject(dc, pen); SelectObject(dc, b);
        Polygon(dc, sh, ARRAYSIZE(sh));
        DeleteObject(b); DeleteObject(pen);
    }
    SelectObject(dc, g_font_small);
    SetTextColor(dc, sub);
    r = rc; r.left = S(44); r.top = S(11); r.bottom = S(30);
    DrawTextW(dc, L"SG Defender", -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
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

    button(dc, &g_review, L"Review", TRUE, g_hot == 1, dark);
    button(dc, &g_dismiss, L"Dismiss", FALSE, g_hot == 2, dark);
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

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: paint(hwnd); return 0;
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
        case 2: case 3: DestroyWindow(hwnd); return 0;
        case 1: default:   /* the notice itself opens what it is about, as a toast does */
            ShellExecuteW(NULL, NULL, L"ms-settings:windowsdefender", NULL, NULL, SW_SHOWNORMAL);
            DestroyWindow(hwnd);
            return 0;
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
    if (argc < 2 || !load(argv[1])) return 0;
    dump();

    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    g_font = font(10, FW_NORMAL);
    g_font_bold = font(11, FW_SEMIBOLD);
    g_font_small = font(9, FW_NORMAL);
    w = S(W_DIP); h = S(H_DIP);
    SetRect(&g_close, w - S(40), S(6), w - S(8), S(32));
    SetRect(&g_review, S(20), h - S(48), S(20) + (w - S(52)) / 2, h - S(16));
    SetRect(&g_dismiss, g_review.right + S(12), g_review.top, w - S(20), g_review.bottom);

    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    while ((other = FindWindowExW(NULL, other, CLASS_NAME, NULL))) stacked++;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, CLASS_NAME, L"SG Defender",
                           WS_POPUP | WS_BORDER, work.right - w - S(12), work.bottom - (h + S(12)) * (stacked + 1),
                           w, h, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    MessageBeep(MB_ICONWARNING);
    SetTimer(hwnd, 1, TIMEOUT_MS, NULL);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
