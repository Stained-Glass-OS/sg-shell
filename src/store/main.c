/* SG Store -- the window and the command line.
 *
 *   sg-store64.exe                 open the store
 *   sg-store64.exe --list          write the catalogue and each app's state to
 *                                  the dump (headless), then exit
 *   sg-store64.exe --check-updates as --list, but resolve each app's newest
 *                                  available version first
 *   sg-store64.exe --install ID    install (or update) one app by ordinal,
 *                                  name or winget id (headless), then exit
 *
 * SG_STORE_DUMP=<file> (a Windows path) receives the catalogue, each app's
 * tier/state/versions and, while the window is open, every clickable thing's
 * screen centre -- for the gate.
 *
 * The Windows and our-own tiers are shown by default; the Linux tier is hidden
 * behind an "Advanced" disclosure. OS updates are not handled here.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "store.h"
#include <shellapi.h>

static app_t g_apps[MAX_APPS];
static int g_napps;
static WCHAR g_dump[MAX_PATH];
static HWND g_wnd;
static HFONT g_f_title, g_f_head, g_f_body, g_f_small;
static int g_dpi = 96, g_scroll, g_extent;
static BOOL g_show_linux;                 /* the Advanced disclosure is open */
static volatile LONG g_cancel;
static int g_busy = -1;                   /* the app being installed, or -1 */

#define dpx(x) MulDiv((x), g_dpi, 96)

/* hit rectangles, for the gate to click */
enum { H_INSTALL, H_OPEN, H_UPDATE, H_CHECKALL, H_LINUX, H_CLOSE };
typedef struct { int verb, idx; RECT rc; } hit_t;
static hit_t g_hits[MAX_APPS * 2 + 8];
static int g_nhits;

static const WCHAR *state_name(int s)
{
    switch (s) {
    case AST_NOT_INSTALLED: return L"not-installed";
    case AST_INSTALLED:     return L"installed";
    case AST_UPDATE:        return L"update";
    case AST_INSTALLING:    return L"installing";
    case AST_DONE:          return L"done";
    default:                return L"failed";
    }
}
static const WCHAR *method_name(int m)
{
    switch (m) {
    case SRC_WINGET:     return L"winget";
    case SRC_PIN:        return L"pin";
    case SRC_OURS_SETUP: return L"ours-setup";
    case SRC_OURS_APT:   return L"ours-apt";
    case SRC_LINUX_APT:  return L"linux-apt";
    default:             return L"unknown";
    }
}

/* ---- the dump ------------------------------------------------------------------------------------- */

static void dumpf(FILE *f, const WCHAR *fmt, ...)
{
    WCHAR buf[2048];
    char u[4096];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, ARRAYSIZE(buf), fmt, ap);
    va_end(ap);
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, u, sizeof(u), NULL, NULL);
    fputs(u, f);
}

static void dump(void)
{
    WCHAR tmp[MAX_PATH + 8];
    FILE *f;
    int i;
    if (!g_dump[0]) return;
    swprintf(tmp, ARRAYSIZE(tmp), L"%ls.tmp", g_dump);
    if (!(f = _wfopen(tmp, L"wb"))) return;
    dumpf(f, L"window %d\n", g_wnd ? 1 : 0);
    dumpf(f, L"show-linux %d\n", g_show_linux ? 1 : 0);
    dumpf(f, L"catalog %d\n", g_napps);
    for (i = 0; i < g_napps; i++) {
        app_t *a = &g_apps[i];
        dumpf(f, L"app %ls tier=%ls method=%ls state=%ls shown=%d installed=%ls available=%ls category=%ls name=%ls\n",
              a->ord, tier_name(a->tier), method_name(a->method), state_name(a->state),
              app_shown_by_default(a) ? 1 : 0,
              a->installed_version[0] ? a->installed_version : L"-",
              a->available_version[0] ? a->available_version : L"-",
              a->category, a->name);
        if (a->msg[0]) dumpf(f, L"msg %ls %ls\n", a->ord, a->msg);
    }
    for (i = 0; i < g_nhits; i++) {
        POINT pt = { (g_hits[i].rc.left + g_hits[i].rc.right) / 2, (g_hits[i].rc.top + g_hits[i].rc.bottom) / 2 };
        const WCHAR *v = g_hits[i].verb == H_INSTALL ? L"install" : g_hits[i].verb == H_OPEN ? L"open" :
                         g_hits[i].verb == H_UPDATE ? L"update" : g_hits[i].verb == H_CHECKALL ? L"checkall" :
                         g_hits[i].verb == H_LINUX ? L"linuxtoggle" : L"close";
        if (g_wnd) ClientToScreen(g_wnd, &pt);
        dumpf(f, L"hit %ls %d %d %d\n", v, g_hits[i].idx, pt.x, pt.y);
    }
    fclose(f);
    MoveFileExW(tmp, g_dump, MOVEFILE_REPLACE_EXISTING);
}

/* ---- opening an installed app ---------------------------------------------------------------------- */

static void open_app(app_t *a)
{
    /* the browsers register a StartMenuInternet command; our suites have an
     * App Paths launcher. Best effort: run the detect name via the shell. */
    if (a->method == SRC_OURS_SETUP) { ShellExecuteW(NULL, NULL, L"sg-documents.exe", NULL, NULL, SW_SHOWNORMAL); return; }
    /* fall back to Apps & features, where every installed app can be launched/removed */
    ShellExecuteW(NULL, NULL, L"ms-settings:appsfeatures", NULL, NULL, SW_SHOWNORMAL);
}

/* ---- the whole catalogue --------------------------------------------------------------------------- */

static void load_all(BOOL check_updates)
{
    int i;
    WCHAR err[512];
    g_napps = catalog_load(g_apps, MAX_APPS);
    for (i = 0; i < g_napps; i++) {
        app_detect(&g_apps[i]);
        if (check_updates) app_check_update(&g_apps[i], err, ARRAYSIZE(err));
    }
}

/* ---- installing on a worker thread ----------------------------------------------------------------- */

static void progress_cb(void *ctx, int stage, ULONGLONG done, ULONGLONG total)
{
    (void)ctx; (void)stage; (void)done; (void)total;
    /* the marquee/label are updated on WM_PAINT; nothing needed per chunk */
}

static DWORD WINAPI install_worker(void *arg)
{
    app_t *a = arg;
    WCHAR err[512];
    int rc;
    a->state = AST_INSTALLING;
    a->msg[0] = 0;
    if (g_wnd) { InvalidateRect(g_wnd, NULL, FALSE); dump(); }
    rc = app_install(a, progress_cb, NULL, &g_cancel, err, ARRAYSIZE(err));
    if (rc == 0) {
        a->state = AST_DONE;
        app_detect(a);              /* pick up the new installed version */
        if (a->state == AST_NOT_INSTALLED) a->state = AST_DONE;
        lstrcpynW(a->msg, L"Installed.", ARRAYSIZE(a->msg));
    } else {
        a->state = AST_FAILED;
        lstrcpynW(a->msg, err[0] ? err : L"The install failed.", ARRAYSIZE(a->msg));
    }
    g_busy = -1;
    if (g_wnd) { InvalidateRect(g_wnd, NULL, FALSE); dump(); }
    return 0;
}

static void start_install(int i)
{
    if (g_busy >= 0 || i < 0 || i >= g_napps) return;
    g_busy = i;
    CloseHandle(CreateThread(NULL, 0, install_worker, &g_apps[i], 0, NULL));
}

/* ---- drawing --------------------------------------------------------------------------------------- */

/* Stained Glass Light-ish palette. */
#define C_BG      RGB(0xf3, 0xf3, 0xf3)
#define C_CARD    RGB(0xff, 0xff, 0xff)
#define C_LINE    RGB(0xe1, 0xe1, 0xe1)
#define C_TEXT    RGB(0x20, 0x20, 0x20)
#define C_SUB     RGB(0x60, 0x60, 0x60)
#define C_ACCENT  RGB(0x10, 0x7c, 0x41)
#define C_OK      RGB(0x10, 0x7c, 0x41)

static void add_hit(int verb, int idx, RECT rc) { if (g_nhits < (int)ARRAYSIZE(g_hits)) { g_hits[g_nhits].verb = verb; g_hits[g_nhits].idx = idx; g_hits[g_nhits].rc = rc; g_nhits++; } }

static void text(HDC dc, HFONT font, COLORREF col, RECT rc, const WCHAR *s, UINT fmt)
{
    HFONT old = SelectObject(dc, font);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &rc, fmt);
    SelectObject(dc, old);
}

static void button(HDC dc, RECT rc, const WCHAR *label, BOOL primary)
{
    HBRUSH b = CreateSolidBrush(primary ? C_ACCENT : C_CARD);
    HPEN pen = CreatePen(PS_SOLID, 1, primary ? C_ACCENT : C_LINE), oldp = SelectObject(dc, pen);
    HBRUSH oldb = SelectObject(dc, b);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, dpx(6), dpx(6));
    SelectObject(dc, oldb); SelectObject(dc, oldp);
    DeleteObject(b); DeleteObject(pen);
    text(dc, g_f_body, primary ? RGB(255, 255, 255) : C_TEXT, rc, label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* one card; returns its bottom in content coordinates */
static int draw_card(HDC dc, int x, int w, int y, int idx)
{
    app_t *a = &g_apps[idx];
    int cardh = dpx(96), pad = dpx(16), badge = dpx(48);
    RECT card = { x, y, x + w, y + cardh }, r;
    HBRUSH cb = CreateSolidBrush(C_CARD);
    HPEN pen = CreatePen(PS_SOLID, 1, C_LINE), oldp = SelectObject(dc, pen);
    HBRUSH oldb = SelectObject(dc, cb);
    RoundRect(dc, card.left, card.top, card.right, card.bottom, dpx(8), dpx(8));
    SelectObject(dc, oldb); SelectObject(dc, oldp);
    DeleteObject(cb); DeleteObject(pen);

    /* letter badge in the entry's colour (no logos) */
    {
        DWORD c = a->colour;
        HBRUSH bb = CreateSolidBrush(RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff));
        HBRUSH ob = SelectObject(dc, bb);
        HPEN np = SelectObject(dc, GetStockObject(NULL_PEN));
        WCHAR letter[2] = { a->name[0], 0 };
        RECT br = { x + pad, y + pad, x + pad + badge, y + pad + badge };
        RoundRect(dc, br.left, br.top, br.right, br.bottom, dpx(8), dpx(8));
        SelectObject(dc, ob); SelectObject(dc, np);
        DeleteObject(bb);
        text(dc, g_f_head, RGB(255, 255, 255), br, letter, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    r.left = x + pad + badge + dpx(14); r.right = x + w - dpx(150); r.top = y + dpx(14); r.bottom = y + dpx(38);
    text(dc, g_f_head, C_TEXT, r, a->name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    r.top = r.bottom; r.bottom = y + dpx(58);
    text(dc, g_f_small, C_SUB, r, a->publisher, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    r.top = r.bottom; r.bottom = y + cardh - dpx(10);
    text(dc, g_f_body, C_SUB, r, a->desc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);

    /* the action button, and state text */
    {
        RECT bt = { x + w - dpx(134), y + dpx(30), x + w - pad, y + dpx(30) + dpx(34) };
        if (idx == g_busy || a->state == AST_INSTALLING) {
            text(dc, g_f_body, C_SUB, bt, L"Installing...", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (a->state == AST_UPDATE) {
            button(dc, bt, L"Update", TRUE); add_hit(H_UPDATE, idx, bt);
        } else if (a->state == AST_INSTALLED || a->state == AST_DONE) {
            button(dc, bt, L"Open", FALSE); add_hit(H_OPEN, idx, bt);
            { RECT s = { x + w - dpx(280), bt.top, x + w - dpx(144), bt.bottom };
              text(dc, g_f_small, C_OK, s, a->state == AST_DONE ? L"Installed" : L"Installed", DT_RIGHT | DT_VCENTER | DT_SINGLELINE); }
        } else if (a->state == AST_FAILED) {
            button(dc, bt, L"Try again", TRUE); add_hit(H_INSTALL, idx, bt);
        } else {
            button(dc, bt, L"Install", TRUE); add_hit(H_INSTALL, idx, bt);
        }
    }
    return y + cardh + dpx(12);
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps), dc;
    RECT rc;
    HBITMAP bmp, oldbmp;
    int x, w, i, top, catleft;
    WCHAR curcat[64] = L"";
    GetClientRect(hwnd, &rc);
    dc = CreateCompatibleDC(wdc);
    bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    oldbmp = SelectObject(dc, bmp);
    { HBRUSH bg = CreateSolidBrush(C_BG); FillRect(dc, &rc, bg); DeleteObject(bg); }

    g_nhits = 0;
    x = dpx(20); w = rc.right - dpx(40); if (w > dpx(720)) { x = (rc.right - dpx(720)) / 2; w = dpx(720); }
    catleft = x;
    top = dpx(18) - g_scroll;

    /* header */
    { RECT h = { x, top, x + w - dpx(160), top + dpx(40) };
      text(dc, g_f_title, C_TEXT, h, L"SG Store", DT_LEFT | DT_VCENTER | DT_SINGLELINE); }
    { RECT bt = { x + w - dpx(150), top + dpx(4), x + w, top + dpx(4) + dpx(32) };
      button(dc, bt, L"Check for updates", FALSE); add_hit(H_CHECKALL, -1, bt); }
    top += dpx(52);
    { RECT sub = { x, top, x + w, top + dpx(20) };
      text(dc, g_f_small, C_SUB, sub, L"App and system updates are handled in Settings > Update & Security.", DT_LEFT | DT_VCENTER | DT_SINGLELINE); }
    top += dpx(30);

    /* windows + ours tiers, grouped by category */
    for (i = 0; i < g_napps; i++) {
        app_t *a = &g_apps[i];
        if (!app_shown_by_default(a)) continue;
        if (lstrcmpiW(a->category, curcat)) {
            RECT ch = { catleft, top, catleft + w, top + dpx(24) };
            lstrcpynW(curcat, a->category, ARRAYSIZE(curcat));
            text(dc, g_f_head, C_TEXT, ch, curcat, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            top += dpx(30);
        }
        top = draw_card(dc, x, w, top, i);
    }

    /* the Advanced disclosure: Linux desktop apps (a last resort) */
    {
        RECT dr = { x, top + dpx(8), x + w, top + dpx(8) + dpx(26) };
        WCHAR lbl[128];
        int nlinux = 0;
        for (i = 0; i < g_napps; i++) if (g_apps[i].tier == TIER_LINUX) nlinux++;
        if (nlinux) {
            swprintf(lbl, ARRAYSIZE(lbl), L"%ls  Advanced: Linux desktop apps (%d) -- we prefer Windows programs",
                     g_show_linux ? L"\x25BC" : L"\x25B6", nlinux);
            text(dc, g_f_body, C_ACCENT, dr, lbl, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            add_hit(H_LINUX, -1, dr);
            top = dr.bottom + dpx(6);
            if (g_show_linux) {
                curcat[0] = 0;
                for (i = 0; i < g_napps; i++) {
                    if (g_apps[i].tier != TIER_LINUX) continue;
                    top = draw_card(dc, x, w, top, i);
                }
            }
        }
    }

    g_extent = top + g_scroll + dpx(20);
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);

    /* keep the scrollbar in step */
    {
        SCROLLINFO si = { sizeof(si) };
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0; si.nMax = g_extent; si.nPage = rc.bottom; si.nPos = g_scroll;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    }
    dump();
}

/* find the hit under a client point (g_hits rects are in client coordinates;
 * the dump converts their centres to screen for the gate's xdotool) */
static int hit_at(int cx, int cy, int *verb, int *idx)
{
    POINT pt = { cx, cy };
    int i;
    for (i = 0; i < g_nhits; i++)
        if (PtInRect(&g_hits[i].rc, pt)) { *verb = g_hits[i].verb; *idx = g_hits[i].idx; return 1; }
    return 0;
}

static void do_checkall(void)
{
    int i;
    WCHAR err[512];
    for (i = 0; i < g_napps; i++) {
        if (g_apps[i].tier == TIER_LINUX) continue;
        app_check_update(&g_apps[i], err, ARRAYSIZE(err));
    }
    InvalidateRect(g_wnd, NULL, FALSE);
    dump();
}

static void scroll_to(HWND hwnd, int pos)
{
    RECT rc; GetClientRect(hwnd, &rc);
    int max = g_extent - rc.bottom;
    if (max < 0) max = 0;
    if (pos < 0) pos = 0;
    if (pos > max) pos = max;
    if (pos != g_scroll) { g_scroll = pos; InvalidateRect(hwnd, NULL, FALSE); }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_LBUTTONUP: {
        int verb, idx;
        if (hit_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &verb, &idx)) {
            if (verb == H_INSTALL || verb == H_UPDATE) start_install(idx);
            else if (verb == H_OPEN) open_app(&g_apps[idx]);
            else if (verb == H_CHECKALL) do_checkall();
            else if (verb == H_LINUX) { g_show_linux = !g_show_linux; InvalidateRect(hwnd, NULL, FALSE); }
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        scroll_to(hwnd, g_scroll - GET_WHEEL_DELTA_WPARAM(wp) / 2);
        return 0;
    case WM_VSCROLL: {
        RECT rc; GetClientRect(hwnd, &rc);
        int pos = g_scroll;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= dpx(40); break;
        case SB_LINEDOWN: pos += dpx(40); break;
        case SB_PAGEUP: pos -= rc.bottom; break;
        case SB_PAGEDOWN: pos += rc.bottom; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = HIWORD(wp); break;
        }
        scroll_to(hwnd, pos);
        return 0;
    }
    case WM_SIZE:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        InterlockedExchange(&g_cancel, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void make_fonts(void)
{
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_f_body = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -dpx(11); g_f_small = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -dpx(15); ncm.lfMessageFont.lfWeight = FW_SEMIBOLD; g_f_head = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -dpx(24); ncm.lfMessageFont.lfWeight = FW_SEMIBOLD; g_f_title = CreateFontIndirectW(&ncm.lfMessageFont);
}

static int window(HINSTANCE inst)
{
    WNDCLASSW wc = { 0 };
    MSG m;
    make_fonts();
    load_all(FALSE);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"SgStore";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"SG Store",
                            WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                            CW_USEDEFAULT, CW_USEDEFAULT, dpx(820), dpx(680), NULL, NULL, inst, NULL);
    if (!g_wnd) return 1;
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    UpdateWindow(g_wnd);
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return 0;
}

/* ---- the command line ------------------------------------------------------------------------------ */

static int find_app(const WCHAR *key)
{
    int i;
    for (i = 0; i < g_napps; i++)
        if (!lstrcmpiW(g_apps[i].ord, key) || !lstrcmpiW(g_apps[i].name, key) ||
            (g_apps[i].winget_id[0] && !lstrcmpiW(g_apps[i].winget_id, key)))
            return i;
    return -1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    int argc, i, rc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    (void)prev; (void)cmdline; (void)show;
    GetEnvironmentVariableW(L"SG_STORE_DUMP", g_dump, MAX_PATH);

    for (i = 1; argv && i < argc; i++) {
        if (!lstrcmpiW(argv[i], L"--list")) {
            load_all(FALSE); dump(); LocalFree(argv); return 0;
        }
        if (!lstrcmpiW(argv[i], L"--check-updates")) {
            load_all(TRUE); dump(); LocalFree(argv); return 0;
        }
        if (!lstrcmpiW(argv[i], L"--install") && i + 1 < argc) {
            int k;
            WCHAR err[512], tmp[MAX_PATH + 8];
            FILE *f;
            load_all(FALSE);
            k = find_app(argv[i + 1]);
            if (k < 0) { LocalFree(argv); return 2; }
            app_detect(&g_apps[k]);
            rc = app_install(&g_apps[k], progress_cb, NULL, &g_cancel, err, ARRAYSIZE(err));
            g_apps[k].state = rc ? AST_FAILED : AST_DONE;
            lstrcpynW(g_apps[k].msg, rc ? err : L"Installed.", ARRAYSIZE(g_apps[k].msg));
            if (!rc) app_detect(&g_apps[k]);
            if (g_dump[0]) {
                swprintf(tmp, ARRAYSIZE(tmp), L"%ls.result", g_dump);
                if ((f = _wfopen(tmp, L"wb"))) {
                    dumpf(f, L"result %ls %ls %d %ls\n", g_apps[k].ord, rc ? L"fail" : L"ok", rc, g_apps[k].msg);
                    fclose(f);
                }
                dump();
            }
            LocalFree(argv);
            return rc;
        }
    }
    if (argv) LocalFree(argv);
    return window(inst);
}
