/* sg-browser -- Get a web browser, and "How do you want to open this?".
 *
 * Stained Glass OS ships no web browser (Microsoft Edge is Microsoft's; we
 * never redistribute it, nor anyone else's). This program is what web links
 * and .htm/.html files open with until the user has chosen a browser:
 *
 *   sg-browser64.exe URL|FILE   one browser installed: it opens there and
 *                               becomes the default; several: "How do you
 *                               want to open this?" (Always use this app);
 *                               none: Get a web browser, then it opens in the
 *                               one installed
 *   sg-browser64.exe            Get a web browser (the Start menu's entry)
 *
 * Get a web browser offers Firefox, Chrome and Brave (the list is
 * HKLM\Software\Stained Glass\Web Browsers\Offers, so an organisation can
 * change it). Install downloads the maker's own installer as the winget
 * community repository describes it, checks its SHA-256, and runs it silently
 * (fetch.c); with winget installed, winget does it. Internet Explorer (Wine's,
 * on Gecko) stays available for simple pages.
 *
 * A browser counts as installed when it registers under
 * Software\Clients\StartMenuInternet (HKCU, then HKLM), as Windows browsers
 * do; making it the default is what Settings > Default apps does (the
 * user's Classes for http, https, .htm and .html, and UserChoice).
 *
 * SG_BROWSER_DUMP=<file> writes what is shown and done, for the gate.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "browser.h"
#include <shellapi.h>
#include <shlobj.h>

#define APP_TITLE L"Get a web browser"
#define CLASS_NAME L"SgBrowserWindow"
#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)

#define C_BG      RGB(0xF3, 0xF3, 0xF3)
#define C_CARD    RGB(0xFF, 0xFF, 0xFF)
#define C_LINE    RGB(0xE0, 0xE0, 0xE0)
#define C_TEXT    RGB(0x1F, 0x1F, 0x1F)
#define C_SUB     RGB(0x60, 0x60, 0x60)
#define C_ACCENT  RGB(112, 48, 192)
#define C_ACCENT2 RGB(0x8A, 0x4F, 0xD6)
#define C_HOVER   RGB(0xEB, 0xEB, 0xEB)
#define C_ERROR   RGB(0xC4, 0x2B, 0x1C)
#define C_OK      RGB(0x10, 0x7C, 0x10)

typedef struct { WCHAR id[128], name[128], maker[128], desc[256]; COLORREF colour; } offer_t;
typedef struct { WCHAR key[128], name[128], cmd[1024], url_progid[128], html_progid[128]; BOOL hkcu; } installed_t;
typedef struct { RECT rc; int cmd, arg; } hit_t;

enum { CMD_INSTALL = 1, CMD_CANCEL, CMD_CLOSE, CMD_IE, CMD_PICK, CMD_ALWAYS, CMD_OK, CMD_MORE };
enum { MODE_GET, MODE_CHOOSE };
enum { ST_IDLE, ST_WORKING, ST_DONE, ST_FAILED };

#define MAX_OFFERS 8
#define MAX_INSTALLED 16
#define MAX_HITS 64

static offer_t g_offers[MAX_OFFERS];
static int g_noffers;
static installed_t g_inst[MAX_INSTALLED];
static int g_ninst;
static struct { int state, stage; ULONGLONG done, total; WCHAR msg[512]; } g_os[MAX_OFFERS];
static hit_t g_hits[MAX_HITS];
static int g_nhits, g_hover = -1, g_focus = -1;
static int g_mode = MODE_GET, g_pick = 0, g_busy = -1;
static BOOL g_always = TRUE;
static WCHAR g_target[4096], g_dump[MAX_PATH], g_opened[4096], g_method[16], g_status[512];
static package_t g_pkg;
static volatile LONG g_cancel;
static HWND g_wnd;
static HINSTANCE g_hinst;
static UINT g_dpi = 96;
static HFONT g_f_head, g_f_body, g_f_bold, g_f_small, g_f_letter;

static int dpx(int v) { return MulDiv(v, g_dpi, 96); }

static HFONT font(int pt10, int weight)
{
    return CreateFontW(-MulDiv(pt10, g_dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

/* ---- the dump ---------------------------------------------------------------------------------- */

static void dumpf(FILE *f, const WCHAR *fmt, ...)
{
    WCHAR w[4096];
    char u[12288];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(w, 4096, fmt, ap);
    va_end(ap);
    w[4095] = 0;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), NULL, NULL);
    fputs(u, f);
}

static void dump(void)
{
    WCHAR tmp[MAX_PATH + 8], def[128] = L"";
    DWORD cb = sizeof(def);
    FILE *f;
    int i;
    if (!g_dump[0]) return;
    swprintf(tmp, MAX_PATH + 8, L"%ls.tmp", g_dump);
    if (!(f = _wfopen(tmp, L"wb"))) return;
    dumpf(f, L"mode %ls\n", !g_wnd ? L"none" : g_mode == MODE_GET ? L"get" : L"choose");
    dumpf(f, L"target %ls\n", g_target);
    dumpf(f, L"method %ls\n", g_method);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\http\\UserChoice",
                 L"ProgId", RRF_RT_REG_SZ, NULL, def, &cb);
    dumpf(f, L"default %ls\n", def[0] ? def : L"none");
    for (i = 0; i < g_ninst; i++) dumpf(f, L"installed %ls\t%ls\t%ls\n", g_inst[i].name, g_inst[i].url_progid, g_inst[i].cmd);
    for (i = 0; i < g_noffers; i++) {
        static const WCHAR *const ST[] = { L"idle", L"working", L"done", L"failed" };
        dumpf(f, L"offer %ls %ls %d %ls\n", g_offers[i].id, ST[g_os[i].state], g_os[i].stage, g_os[i].msg);
    }
    for (i = 0; i < g_nhits; i++) {
        POINT pt = { (g_hits[i].rc.left + g_hits[i].rc.right) / 2, (g_hits[i].rc.top + g_hits[i].rc.bottom) / 2 };
        static const WCHAR *const NAMES[] = { L"", L"install", L"cancel", L"close", L"ie", L"pick", L"always", L"ok", L"more" };
        ClientToScreen(g_wnd, &pt);
        dumpf(f, L"hit %ls %d %d %d\n", NAMES[g_hits[i].cmd], g_hits[i].arg, pt.x, pt.y);
    }
    if (g_mode == MODE_CHOOSE) dumpf(f, L"pick %d\nalways %d\n", g_pick, g_always);
    if (g_pkg.id[0])
        dumpf(f, L"package %ls %ls %ls %ls\ninstaller %ls\n", g_pkg.id, g_pkg.version, g_pkg.type, g_pkg.url, g_pkg.command);
    if (g_opened[0]) dumpf(f, L"opened %ls\n", g_opened);
    if (g_status[0]) dumpf(f, L"status %ls\n", g_status);
    fclose(f);
    MoveFileExW(tmp, g_dump, MOVEFILE_REPLACE_EXISTING);
}

/* ---- what is offered, what is installed -------------------------------------------------------- */

static void load_offers(void)
{
    static const offer_t BUILTIN[] = {
        { L"Mozilla.Firefox", L"Mozilla Firefox", L"Mozilla", L"Free and open source, from a non-profit.", RGB(0xE6, 0x60, 0x00) },
        { L"Google.Chrome", L"Google Chrome", L"Google", L"The most used browser, from Google.", RGB(0x1A, 0x73, 0xE8) },
        { L"Brave.Brave", L"Brave", L"Brave Software", L"Blocks ads and trackers by default.", RGB(0xFB, 0x54, 0x2B) },
    };
    HKEY key;
    WCHAR sub[64];
    DWORD i, n;
    g_noffers = 0;
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, BROWSERS_KEY L"\\Offers", 0, KEY_READ, &key)) {
        for (i = 0; n = 64, g_noffers < MAX_OFFERS && !RegEnumKeyExW(key, i, sub, &n, NULL, NULL, NULL, NULL); i++) {
            offer_t *o = &g_offers[g_noffers];
            DWORD cb, col = 0x7030C0;
            memset(o, 0, sizeof(*o));
            cb = sizeof(o->id); RegGetValueW(key, sub, L"Id", RRF_RT_REG_SZ, NULL, o->id, &cb);
            cb = sizeof(o->name); RegGetValueW(key, sub, L"Name", RRF_RT_REG_SZ, NULL, o->name, &cb);
            cb = sizeof(o->maker); RegGetValueW(key, sub, L"Publisher", RRF_RT_REG_SZ, NULL, o->maker, &cb);
            cb = sizeof(o->desc); RegGetValueW(key, sub, L"Description", RRF_RT_REG_SZ, NULL, o->desc, &cb);
            cb = sizeof(col); RegGetValueW(key, sub, L"Colour", RRF_RT_REG_DWORD, NULL, &col, &cb);
            o->colour = RGB((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
            if (o->id[0] && o->name[0]) g_noffers++;
        }
        RegCloseKey(key);
    }
    if (!g_noffers) { memcpy(g_offers, BUILTIN, sizeof(BUILTIN)); g_noffers = ARRAYSIZE(BUILTIN); }
}

static BOOL command_exe_exists(const WCHAR *cmd)
{
    WCHAR exe[MAX_PATH];
    const WCHAR *s = cmd, *e;
    if (*s == '"') { s++; e = wcschr(s, '"'); }
    else e = wcschr(s, ' ');
    if (!e) e = s + wcslen(s);
    if (e - s >= MAX_PATH || e == s) return FALSE;
    lstrcpynW(exe, s, (int)(e - s) + 1);
    return GetFileAttributesW(exe) != INVALID_FILE_ATTRIBUTES;
}

static void load_installed(void)
{
    HKEY roots[2] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }, key;
    int r, k;
    DWORD i, n;
    WCHAR sub[128], path[400];
    g_ninst = 0;
    for (r = 0; r < 2; r++) {
        if (RegOpenKeyExW(roots[r], L"Software\\Clients\\StartMenuInternet", 0, KEY_READ, &key)) continue;
        for (i = 0; n = 128, g_ninst < MAX_INSTALLED && !RegEnumKeyExW(key, i, sub, &n, NULL, NULL, NULL, NULL); i++) {
            installed_t *b = &g_inst[g_ninst];
            DWORD cb;
            BOOL dup = FALSE;
            memset(b, 0, sizeof(*b));
            /* Wine's Internet Explorer is the fallback, not a browser to choose */
            if (!_wcsicmp(sub, L"IEXPLORE.EXE")) continue;
            for (k = 0; k < g_ninst; k++) if (!_wcsicmp(g_inst[k].key, sub)) dup = TRUE;
            if (dup) continue;
            lstrcpynW(b->key, sub, 128);
            b->hkcu = r == 0;
            cb = sizeof(b->name);
            if (RegGetValueW(key, sub, NULL, RRF_RT_REG_SZ, NULL, b->name, &cb) || !b->name[0]) lstrcpynW(b->name, sub, 128);
            swprintf(path, 400, L"%ls\\shell\\open\\command", sub);
            cb = sizeof(b->cmd);
            RegGetValueW(key, path, NULL, RRF_RT_REG_SZ, NULL, b->cmd, &cb);
            swprintf(path, 400, L"%ls\\Capabilities\\URLAssociations", sub);
            cb = sizeof(b->url_progid);
            RegGetValueW(key, path, L"http", RRF_RT_REG_SZ, NULL, b->url_progid, &cb);
            swprintf(path, 400, L"%ls\\Capabilities\\FileAssociations", sub);
            cb = sizeof(b->html_progid);
            if (RegGetValueW(key, path, L".html", RRF_RT_REG_SZ, NULL, b->html_progid, &cb)) {
                cb = sizeof(b->html_progid);
                RegGetValueW(key, path, L".htm", RRF_RT_REG_SZ, NULL, b->html_progid, &cb);
            }
            if (!b->cmd[0] || !command_exe_exists(b->cmd)) continue;
            g_ninst++;
        }
        RegCloseKey(key);
    }
}

static BOOL offer_installed(int k)
{
    WCHAR word[64], *sp;
    int i;
    /* "Mozilla Firefox" is installed when a browser's name has "Firefox" */
    lstrcpynW(word, g_offers[k].name, 64);
    sp = wcsrchr(word, ' ');
    for (i = 0; i < g_ninst; i++) if (StrStrIW(g_inst[i].name, sp ? sp + 1 : word)) return TRUE;
    return FALSE;
}

/* ---- the default browser (as Settings > Default apps sets it) ------------------------------------ */

/* A class's key: the user's own first (a per-user install registers there),
 * then the machine's -- Wine's HKEY_CLASSES_ROOT is only the machine's. */
static LONG open_class(const WCHAR *sub, HKEY *out)
{
    WCHAR path[400];
    swprintf(path, 400, L"Software\\Classes\\%ls", sub);
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, out)) return 0;
    return RegOpenKeyExW(HKEY_CLASSES_ROOT, sub, 0, KEY_READ, out);
}

static void copy_key(HKEY from, HKEY to)
{
    DWORD i, n, cb, type;
    WCHAR name[256];
    BYTE data[4096];
    for (i = 0; n = ARRAYSIZE(name), cb = sizeof(data), !RegEnumValueW(from, i, name, &n, NULL, &type, data, &cb); i++)
        RegSetValueExW(to, name, 0, type, data, cb);
    for (i = 0; n = ARRAYSIZE(name), !RegEnumKeyExW(from, i, name, &n, NULL, NULL, NULL, NULL); i++) {
        HKEY a, b;
        if (RegOpenKeyExW(from, name, 0, KEY_READ, &a)) continue;
        if (!RegCreateKeyExW(to, name, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &b, NULL)) { copy_key(a, b); RegCloseKey(b); }
        RegCloseKey(a);
    }
}

static void set_sz(HKEY root, const WCHAR *sub, const WCHAR *name, const WCHAR *val)
{
    HKEY k;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (DWORD)(wcslen(val) + 1) * sizeof(WCHAR));
    RegCloseKey(k);
}

static void make_default(const installed_t *b)
{
    static const WCHAR *const PROTOS[] = { L"http", L"https" };
    static const WCHAR *const EXTS[] = { L".htm", L".html" };
    WCHAR sub[300];
    int i;
#ifdef SG_MUTANT_NODEFAULT
    return;
#endif
    if (!b->url_progid[0]) return;
    for (i = 0; i < 2; i++) {
        HKEY from, to;
        swprintf(sub, 300, L"Software\\Classes\\%ls", PROTOS[i]);
        RegDeleteTreeW(HKEY_CURRENT_USER, sub);
        if (!open_class(b->url_progid, &from)) {
            if (!RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &to, NULL)) {
                copy_key(from, to);
                RegSetValueExW(to, L"URL Protocol", 0, REG_SZ, (const BYTE *)L"", sizeof(WCHAR));
                RegCloseKey(to);
            }
            RegCloseKey(from);
        }
        swprintf(sub, 300, L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\%ls\\UserChoice", PROTOS[i]);
        set_sz(HKEY_CURRENT_USER, sub, L"ProgId", b->url_progid);
    }
    for (i = 0; i < 2; i++) {
        swprintf(sub, 300, L"Software\\Classes\\%ls", EXTS[i]);
        set_sz(HKEY_CURRENT_USER, sub, NULL, b->html_progid[0] ? b->html_progid : b->url_progid);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

/* open the target in a browser: its URL ProgID's command, else its own */
static BOOL open_in(const installed_t *b, const WCHAR *target)
{
    WCHAR cmd[2048] = L"", sub[300], line[8192], *pct;
    DWORD cb = sizeof(cmd);
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (b->url_progid[0]) {
        HKEY k;
        swprintf(sub, 300, L"%ls\\shell\\open\\command", b->url_progid);
        if (!open_class(sub, &k)) {
            RegGetValueW(k, NULL, NULL, RRF_RT_REG_SZ, NULL, cmd, &cb);
            RegCloseKey(k);
        }
    }
    if (!cmd[0]) lstrcpynW(cmd, b->cmd, 2048);
    if (!target[0]) lstrcpynW(line, b->cmd, 8192);
    else if ((pct = wcsstr(cmd, L"%1"))) {
        *pct = 0;
        swprintf(line, 8192, L"%ls%ls%ls", cmd, target, pct + 2);
    } else swprintf(line, 8192, L"%ls \"%ls\"", cmd, target);
    lstrcpynW(g_opened, line, 4096);
    dump();
    if (!CreateProcessW(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return FALSE;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

static void open_in_ie(void)
{
    swprintf(g_opened, 4096, L"iexplore.exe \"%ls\"", g_target);
    dump();
    ShellExecuteW(NULL, NULL, L"iexplore.exe", g_target, NULL, SW_SHOWNORMAL);
}

/* ---- installing (a thread) ---------------------------------------------------------------------- */

static void progress(void *ctx, int stage, ULONGLONG done, ULONGLONG total)
{
    static DWORD last;
    DWORD now = GetTickCount();
    (void)ctx;
    if (stage == STAGE_DOWNLOAD && now - last < 100 && done != total) return;
    last = now;
    PostMessageW(g_wnd, WM_APP_PROGRESS, stage, (LPARAM)(total ? done * 1000 / total : 0) | ((LPARAM)(total == 0) << 20));
}

typedef struct { int offer; BOOL ok; WCHAR msg[512]; } result_t;

static DWORD WINAPI install_thread(void *arg)
{
    int k = (int)(INT_PTR)arg;
    result_t *r = calloc(1, sizeof(*r));
    WCHAR winget[MAX_PATH];
    if (!r) return 0;
    r->offer = k;
    memset(&g_pkg, 0, sizeof(g_pkg));
    lstrcpynW(g_pkg.id, g_offers[k].id, 128);
    PostMessageW(g_wnd, WM_APP_PROGRESS, STAGE_FIND, 0);
    if (winget_path(winget, MAX_PATH)) {
        lstrcpyW(g_method, L"winget");
        PostMessageW(g_wnd, WM_APP_PROGRESS, STAGE_INSTALL, 0);
        r->ok = winget_install(winget, &g_pkg, r->msg, 512);
    } else {
        lstrcpyW(g_method, L"manifest");
        if (pkg_resolve(g_offers[k].id, &g_pkg, r->msg, 512) &&
            pkg_download(&g_pkg, progress, NULL, &g_cancel, r->msg, 512)) {
            PostMessageW(g_wnd, WM_APP_PROGRESS, STAGE_INSTALL, 0);
            r->ok = pkg_install(&g_pkg, r->msg, 512);
        }
        pkg_cleanup(&g_pkg);
    }
    PostMessageW(g_wnd, WM_APP_DONE, 0, (LPARAM)r);
    return 0;
}

static void start_install(int k)
{
    if (g_busy >= 0 || k < 0 || k >= g_noffers) return;
    g_busy = k;
    g_cancel = 0;
    g_os[k].state = ST_WORKING;
    g_os[k].stage = STAGE_FIND;
    g_os[k].msg[0] = 0;
    CloseHandle(CreateThread(NULL, 0, install_thread, (void *)(INT_PTR)k, 0, NULL));
    InvalidateRect(g_wnd, NULL, FALSE);
}

static void install_done(result_t *r)
{
    int k = r->offer, before = g_ninst, i, pick = -1;
    installed_t old[MAX_INSTALLED];
    memcpy(old, g_inst, sizeof(old));
    g_busy = -1;
    load_installed();
    if (r->ok) {
        /* the browser that was not there before; else one whose name fits */
        for (i = 0; i < g_ninst && pick < 0; i++) {
            int j, seen = 0;
            for (j = 0; j < before; j++) if (!_wcsicmp(old[j].key, g_inst[i].key)) seen = 1;
            if (!seen) pick = i;
        }
        if (pick < 0) {
            WCHAR *sp = wcsrchr(g_offers[k].name, ' ');
            for (i = 0; i < g_ninst && pick < 0; i++) if (StrStrIW(g_inst[i].name, sp ? sp + 1 : g_offers[k].name)) pick = i;
        }
        if (pick < 0) {
            g_os[k].state = ST_FAILED;
            swprintf(g_os[k].msg, 512, L"The installer finished, but %ls did not register as a browser.", g_offers[k].name);
        } else {
            g_os[k].state = ST_DONE;
            make_default(&g_inst[pick]);
            swprintf(g_os[k].msg, 512, L"%ls is installed and is now your default browser.", g_inst[pick].name);
            if (g_target[0]) {
                open_in(&g_inst[pick], g_target);
                swprintf(g_status, 512, L"Opened in %ls.", g_inst[pick].name);
            }
        }
    } else {
        g_os[k].state = ST_FAILED;
        lstrcpynW(g_os[k].msg, r->msg, 512);
    }
    free(r);
    InvalidateRect(g_wnd, NULL, FALSE);
    dump();
}

/* ---- drawing ------------------------------------------------------------------------------------ */

static void add_hit(int l, int t, int r, int b, int cmd, int arg)
{
    if (g_nhits >= MAX_HITS) return;
    SetRect(&g_hits[g_nhits].rc, l, t, r, b);
    g_hits[g_nhits].cmd = cmd;
    g_hits[g_nhits].arg = arg;
    g_nhits++;
}

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc = { l, t, r, b };
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void text(HDC dc, HFONT f, COLORREF c, int l, int t, int r, int b, const WCHAR *s, UINT fmt)
{
    RECT rc = { l, t, r, b };
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &rc, fmt | DT_NOPREFIX);
}

static int button(HDC dc, int r, int t, const WCHAR *label, int cmd, int arg, BOOL primary)
{
    SIZE sz;
    int w, h = dpx(32), l, idx = g_nhits;
    SelectObject(dc, g_f_body);
    GetTextExtentPoint32W(dc, label, lstrlenW(label), &sz);
    w = max(sz.cx + dpx(28), dpx(96));
    l = r - w;
    add_hit(l, t, r, t + h, cmd, arg);
    if (primary) fill(dc, l, t, r, t + h, idx == g_hover ? C_ACCENT2 : C_ACCENT);
    else {
        fill(dc, l, t, r, t + h, C_LINE);
        fill(dc, l + 1, t + 1, r - 1, t + h - 1, idx == g_hover ? C_HOVER : C_CARD);
    }
    if (idx == g_focus) {
        RECT fr = { l - dpx(3), t - dpx(3), r + dpx(3), t + h + dpx(3) };
        DrawFocusRect(dc, &fr);
    }
    text(dc, g_f_body, primary ? RGB(255, 255, 255) : C_TEXT, l, t, r, t + h, label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    return l;
}

static void link(HDC dc, int l, int t, const WCHAR *label, int cmd)
{
    SIZE sz;
    int idx = g_nhits;
    SelectObject(dc, g_f_body);
    GetTextExtentPoint32W(dc, label, lstrlenW(label), &sz);
    add_hit(l, t, l + sz.cx, t + sz.cy, cmd, 0);
    SetTextColor(dc, idx == g_hover ? C_ACCENT2 : C_ACCENT);
    TextOutW(dc, l, t, label, lstrlenW(label));
    if (idx == g_focus) { RECT fr = { l - 2, t - 1, l + sz.cx + 2, t + sz.cy + 1 }; DrawFocusRect(dc, &fr); }
}

static void badge(HDC dc, int x, int y, int d, COLORREF c, WCHAR letter)
{
    HBRUSH br = CreateSolidBrush(c), ob = SelectObject(dc, br);
    HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
    WCHAR s[2] = { letter, 0 };
    Ellipse(dc, x, y, x + d, y + d);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(br);
    text(dc, g_f_letter, RGB(255, 255, 255), x, y, x + d, y + d, s, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void paint(HDC out)
{
    RECT rc;
    HDC dc;
    HBITMAP bmp, ob;
    int m = dpx(28), y, w, i;
    WCHAR s[4200];
    GetClientRect(g_wnd, &rc);
    dc = CreateCompatibleDC(out);
    bmp = CreateCompatibleBitmap(out, rc.right, rc.bottom);
    ob = SelectObject(dc, bmp);
    SetBkMode(dc, TRANSPARENT);
    fill(dc, 0, 0, rc.right, rc.bottom, C_BG);
    g_nhits = 0;
    w = rc.right - 2 * m;
    y = dpx(22);

    if (g_mode == MODE_CHOOSE) {
        text(dc, g_f_head, C_TEXT, m, y, rc.right - m, y + dpx(40), L"How do you want to open this?", DT_LEFT | DT_SINGLELINE);
        y += dpx(46);
        text(dc, g_f_small, C_SUB, m, y, rc.right - m, y + dpx(20), g_target, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
        y += dpx(30);
        for (i = 0; i < g_ninst; i++) {
            int h = dpx(52), idx = g_nhits;
            add_hit(m, y, rc.right - m, y + h, CMD_PICK, i);
            fill(dc, m, y, rc.right - m, y + h, i == g_pick ? RGB(0xEE, 0xE6, 0xF8) : idx == g_hover ? C_HOVER : C_CARD);
            if (i == g_pick) fill(dc, m, y + dpx(10), m + dpx(3), y + h - dpx(10), C_ACCENT);
            badge(dc, m + dpx(14), y + dpx(10), dpx(32), C_ACCENT, g_inst[i].name[0]);
            text(dc, g_f_bold, C_TEXT, m + dpx(58), y, rc.right - m, y + h, g_inst[i].name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (idx == g_focus) { RECT fr = { m + 1, y + 1, rc.right - m - 1, y + h - 1 }; DrawFocusRect(dc, &fr); }
            y += h + dpx(4);
        }
        y += dpx(12);
        {
            int idx = g_nhits, bx = m;
            add_hit(bx, y, bx + dpx(300), y + dpx(22), CMD_ALWAYS, 0);
            fill(dc, bx, y + dpx(2), bx + dpx(18), y + dpx(20), g_always ? C_ACCENT : C_SUB);
            fill(dc, bx + 1, y + dpx(2) + 1, bx + dpx(18) - 1, y + dpx(20) - 1, g_always ? C_ACCENT : C_CARD);
            if (g_always) text(dc, g_f_bold, RGB(255, 255, 255), bx, y + dpx(2), bx + dpx(18), y + dpx(20), L"\x2713", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            text(dc, g_f_body, C_TEXT, bx + dpx(28), y, rc.right - m, y + dpx(22), L"Always use this app to open web links", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            if (idx == g_focus) { RECT fr = { bx - 2, y - 1, bx + dpx(300), y + dpx(23) }; DrawFocusRect(dc, &fr); }
        }
        y = rc.bottom - dpx(56);
        link(dc, m, y + dpx(7), L"Look for another browser", CMD_MORE);
        button(dc, rc.right - m, y, L"OK", CMD_OK, 0, TRUE);
    } else {
        text(dc, g_f_head, C_TEXT, m, y, rc.right - m, y + dpx(40), APP_TITLE, DT_LEFT | DT_SINGLELINE);
        y += dpx(46);
        {
            RECT tr = { m, y, rc.right - m, y + dpx(60) };
            SelectObject(dc, g_f_body);
            SetTextColor(dc, C_SUB);
            DrawTextW(dc, L"Stained Glass OS doesn't come with a web browser. Choose one, and it will be downloaded from its "
                          L"maker, checked, and installed for you. You can change your default browser later in "
                          L"Settings > Apps > Default apps.", -1, &tr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            y += dpx(62);
        }
        if (g_target[0]) {
            swprintf(s, 4200, L"Then it will open %ls", g_target);
            text(dc, g_f_small, C_SUB, m, y, rc.right - m, y + dpx(20), s, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS);
            y += dpx(26);
        }
        for (i = 0; i < g_noffers; i++) {
            int h = dpx(84), bx;
            offer_t *o = &g_offers[i];
            fill(dc, m, y, rc.right - m, y + h, C_LINE);
            fill(dc, m + 1, y + 1, rc.right - m - 1, y + h - 1, C_CARD);
            badge(dc, m + dpx(16), y + dpx(18), dpx(40), o->colour, o->name[0]);
            text(dc, g_f_bold, C_TEXT, m + dpx(70), y + dpx(12), rc.right - m - dpx(140), y + dpx(34), o->name, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (g_os[i].state == ST_WORKING) {
                int pl = m + dpx(70), pr = rc.right - m - dpx(140), pt = y + dpx(44);
                const WCHAR *what = g_os[i].stage == STAGE_FIND ? L"Finding the newest version..." :
                                    g_os[i].stage == STAGE_DOWNLOAD ? L"Downloading from its maker..." : L"Installing...";
                text(dc, g_f_small, C_SUB, pl, y + dpx(34), pr, y + dpx(52), what, DT_LEFT | DT_SINGLELINE);
                fill(dc, pl, pt + dpx(12), pr, pt + dpx(16), C_LINE);
                if (g_os[i].stage == STAGE_DOWNLOAD && g_os[i].total)
                    fill(dc, pl, pt + dpx(12), pl + (int)((pr - pl) * g_os[i].done / 1000), pt + dpx(16), C_ACCENT);
                else if (g_os[i].stage >= STAGE_INSTALL) fill(dc, pl, pt + dpx(12), pr, pt + dpx(16), C_ACCENT2);
                button(dc, rc.right - m - dpx(16), y + dpx(26), L"Cancel", CMD_CANCEL, i, FALSE);
            } else {
                COLORREF c = g_os[i].state == ST_FAILED ? C_ERROR : g_os[i].state == ST_DONE ? C_OK : C_SUB;
                RECT dr = { m + dpx(70), y + dpx(34), rc.right - m - dpx(150), y + h - dpx(6) };
                if (g_os[i].msg[0]) swprintf(s, 4200, L"%ls", g_os[i].msg);
                else swprintf(s, 4200, L"%ls  \x2022  %ls", o->maker, o->desc);
                SelectObject(dc, g_f_small);
                SetTextColor(dc, c);
                DrawTextW(dc, s, -1, &dr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
                if (offer_installed(i) && g_os[i].state != ST_FAILED)
                    text(dc, g_f_body, C_OK, rc.right - m - dpx(140), y, rc.right - m - dpx(16), y + h, L"Installed", DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                else {
                    bx = button(dc, rc.right - m - dpx(16), y + dpx(26), g_os[i].state == ST_FAILED ? L"Try again" : L"Install",
                                CMD_INSTALL, i, TRUE);
                    (void)bx;
                    if (g_busy >= 0) g_hits[g_nhits - 1].cmd = 0;   /* one at a time */
                }
            }
            y += h + dpx(10);
        }
        y = rc.bottom - dpx(56);
        if (g_target[0]) link(dc, m, y + dpx(7), L"Open in Internet Explorer instead (simple pages only)", CMD_IE);
        button(dc, rc.right - m, y, L"Close", CMD_CLOSE, 0, FALSE);
    }
    BitBlt(out, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    (void)w;
}

/* ---- the window --------------------------------------------------------------------------------- */

static void do_cmd(int cmd, int arg)
{
    switch (cmd) {
    case CMD_INSTALL: start_install(arg); break;
    case CMD_CANCEL: InterlockedExchange(&g_cancel, 1); break;
    case CMD_CLOSE: DestroyWindow(g_wnd); return;
    case CMD_IE: open_in_ie(); DestroyWindow(g_wnd); return;
    case CMD_PICK: g_pick = arg; break;
    case CMD_ALWAYS: g_always = !g_always; break;
    case CMD_MORE: g_mode = MODE_GET; g_focus = -1; break;
    case CMD_OK:
        if (g_pick >= 0 && g_pick < g_ninst) {
            if (g_always) make_default(&g_inst[g_pick]);
            open_in(&g_inst[g_pick], g_target);
        }
        DestroyWindow(g_wnd);
        return;
    }
    InvalidateRect(g_wnd, NULL, FALSE);
    dump();
}

static int hit_at(int x, int y)
{
    POINT pt = { x, y };
    int i;
    for (i = 0; i < g_nhits; i++) if (g_hits[i].cmd && PtInRect(&g_hits[i].rc, pt)) return i;
    return -1;
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        dump();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        int h = hit_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        if (h != g_hover) { g_hover = h; InvalidateRect(hwnd, NULL, FALSE); }
        TrackMouseEvent(&tme);
        SetCursor(LoadCursorW(NULL, (LPCWSTR)(h >= 0 ? IDC_HAND : IDC_ARROW)));
        return 0;
    }
    case WM_MOUSELEAVE: g_hover = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONUP: {
        int h = hit_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h >= 0) do_cmd(g_hits[h].cmd, g_hits[h].arg);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { if (g_busy >= 0) InterlockedExchange(&g_cancel, 1); else DestroyWindow(hwnd); return 0; }
        if (wp == VK_TAB && g_nhits) {
            int n = g_nhits, d = GetKeyState(VK_SHIFT) < 0 ? n - 1 : 1, k;
            for (k = 0; k < n; k++) {
                g_focus = ((g_focus < 0 ? (d == 1 ? -1 : 0) : g_focus) + d + n) % n;
                if (g_hits[g_focus].cmd) break;
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if ((wp == VK_RETURN || wp == VK_SPACE) && g_focus >= 0 && g_focus < g_nhits && g_hits[g_focus].cmd) {
            do_cmd(g_hits[g_focus].cmd, g_hits[g_focus].arg);
            return 0;
        }
        if (g_mode == MODE_CHOOSE && (wp == VK_UP || wp == VK_DOWN) && g_ninst) {
            g_pick = (g_pick + (wp == VK_UP ? g_ninst - 1 : 1)) % g_ninst;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (g_mode == MODE_CHOOSE && wp == VK_RETURN) { do_cmd(CMD_OK, 0); return 0; }
        break;
    case WM_APP_PROGRESS:
        if (g_busy >= 0) {
            g_os[g_busy].stage = (int)wp;
            g_os[g_busy].done = lp & 0xFFFFF;
            g_os[g_busy].total = (lp >> 20) & 1 ? 0 : 1000;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_APP_DONE:
        install_done((result_t *)lp);
        return 0;
    case WM_CLOSE:
        if (g_busy >= 0 && g_os[g_busy].stage >= STAGE_INSTALL) return 0;   /* an installer is running */
        InterlockedExchange(&g_cancel, 1);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    typedef BOOL (WINAPI *ctx_fn)(HANDLE);
    ctx_fn set_ctx = (ctx_fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    WNDCLASSW wc = { 0 };
    MSG msg;
    int argc, i, h;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    HDC screen;
    (void)prev; (void)cmdline; (void)show;
    g_hinst = inst;
    for (i = 1; argv && i < argc; i++) if (argv[i][0] && argv[i][0] != '/') { lstrcpynW(g_target, argv[i], 4096); break; }
    LocalFree(argv);
    GetEnvironmentVariableW(L"SG_BROWSER_DUMP", g_dump, MAX_PATH);
    if (set_ctx) set_ctx((HANDLE)-4);
    else SetProcessDPIAware();
    load_offers();
    load_installed();

    if (g_target[0]) {
        /* the user's choice (Default apps, or ours): Wine's HKEY_CLASSES_ROOT is
         * the machine's classes only -- the user's Software\Classes, where
         * Windows keeps a user's defaults, is not merged in -- so web links
         * come here, and here is where the choice is honoured */
        WCHAR choice[128] = L"";
        DWORD cb = sizeof(choice);
        RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\http\\UserChoice",
                     L"ProgId", RRF_RT_REG_SZ, NULL, choice, &cb);
        for (i = 0; choice[0] && i < g_ninst; i++)
            if (!_wcsicmp(g_inst[i].url_progid, choice)) {
                swprintf(g_status, 512, L"%ls is the default browser.", g_inst[i].name);
                open_in(&g_inst[i], g_target);
                return 0;
            }
    }
    if (g_target[0] && g_ninst == 1) {
        /* one browser: it is the one */
        make_default(&g_inst[0]);
        swprintf(g_status, 512, L"%ls is the only browser: it is now the default.", g_inst[0].name);
        open_in(&g_inst[0], g_target);
        return 0;
    }
    if (g_target[0] && g_ninst > 1) g_mode = MODE_CHOOSE;

    screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    g_f_head = font(200, FW_SEMIBOLD);
    g_f_body = font(100, FW_NORMAL);
    g_f_bold = font(110, FW_SEMIBOLD);
    g_f_small = font(90, FW_NORMAL);
    g_f_letter = font(140, FW_BOLD);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);
    h = g_mode == MODE_CHOOSE ? 230 + 56 * g_ninst : 300 + 94 * g_noffers + (g_target[0] ? 26 : 0);
    g_wnd = CreateWindowExW(0, CLASS_NAME, g_mode == MODE_CHOOSE ? L"Open with" : APP_TITLE,
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, dpx(600), dpx(h), NULL, NULL, inst, NULL);
    if (!g_wnd) return 1;
    {
        UINT (WINAPI *for_window)(HWND) = (void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        if (for_window && for_window(g_wnd) && for_window(g_wnd) != g_dpi) {
            g_dpi = for_window(g_wnd);
            SetWindowPos(g_wnd, NULL, 0, 0, dpx(600), dpx(h), SWP_NOMOVE | SWP_NOZORDER);
        }
    }
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    UpdateWindow(g_wnd);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_wnd = NULL;
    dump();
    return 0;
}
