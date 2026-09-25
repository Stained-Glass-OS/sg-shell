/* sg-control -- Settings > Apps: Apps & features, Default apps, Startup.
 *
 * Apps & features is Programs and Features (programs.c) -- the same
 * Uninstall keys, the same uninstallers -- in Windows 10's list. Default
 * apps writes the per-user class registrations (HKCU\Software\Classes),
 * which HKCR is made of and ShellExecute reads. Startup keeps Task
 * Manager's StartupApproved values, which wineboot honours (wine-sg 0125).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"
#include <shlobj.h>

/* ---- Apps & features -------------------------------------------------------------------------- */
enum { CMD_FILTER = CMD_PAGE_FIRST + 20, CMD_SORT, CMD_SELECT = CMD_PAGE_FIRST + 1000, CMD_UNINSTALL_SEL = CMD_PAGE_FIRST + 30,
       CMD_MODIFY_SEL };
static WCHAR g_filter[128];
static int g_sort, g_sel = -1;
static HWND g_filter_edit;

static int g_order[2048];

static int cmp_size(const void *a, const void *b)
{
    const struct program *p = prog_get(*(const int *)a), *q = prog_get(*(const int *)b);
    return p->size_kb < q->size_kb ? 1 : p->size_kb > q->size_kb ? -1 : 0;
}

void set_build_apps(void)
{
    static const WCHAR *const sorts[] = { L"Sort by: Name", L"Sort by: Size" };
    int y = st_title(L"Apps & features"), n = prog_load(), i, shown = 0, m = 0;
    WCHAR line[400], size[32], lower[256], filt[128];
    y = st_head(y, L"Apps & features");
    y = st_para(y, L"Search, sort, and filter by drive. If you would like to uninstall or move an app, select it from the list.");
    g_filter_edit = st_edit(&y, NULL, g_filter, CMD_FILTER);
    st_combo(&y, NULL, sorts, 2, g_sort, CMD_SORT);
    lstrcpynW(filt, g_filter, ARRAYSIZE(filt));
    CharLowerW(filt);
    for (i = 0; i < n && m < (int)ARRAYSIZE(g_order); i++) {
        lstrcpynW(lower, prog_get(i)->name, ARRAYSIZE(lower));
        CharLowerW(lower);
        if (filt[0] && !wcsstr(lower, filt)) continue;
        g_order[m++] = i;
    }
    if (g_sort == 1) qsort(g_order, m, sizeof(int), cmp_size);
    _snwprintf(line, ARRAYSIZE(line), L"%d apps found", m);
    y = st_text(y, line);
    for (i = 0; i < m; i++) {
        const struct program *p = prog_get(g_order[i]);
        size[0] = 0;
        if (p->size_kb) format_size((ULONGLONG)p->size_kb * 1024, size, ARRAYSIZE(size));
        if (g_sel == g_order[i]) pg_fill(st_x(), y - S(4), st_w(), S(96), RGB(0xF2, 0xF2, 0xF2));
        pg_icon(st_x() + S(8), y + S(2), S(32), IC_G_APPS);
        pg_link(st_x() + S(52), y, p->name, CMD_SELECT + g_order[i], 0);
        pg_text(st_x() + st_w() - S(120), y, S(112), S(20), g_font_body, COL_TEXT, size, DT_SINGLELINE | DT_RIGHT);
        _snwprintf(line, ARRAYSIZE(line), L"%ls%ls%ls", p->publisher, p->publisher[0] && p->version[0] ? L"   " : L"", p->version);
        pg_text(st_x() + S(56), y + S(22), st_w() - S(190), S(18), g_font_small, COL_SUBTLE, line, DT_SINGLELINE | DT_END_ELLIPSIS);
        pg_text(st_x() + st_w() - S(120), y + S(22), S(112), S(18), g_font_small, COL_SUBTLE, p->date, DT_SINGLELINE | DT_RIGHT);
        y += S(48);
        if (g_sel == g_order[i]) {
            HWND a = pg_control(L"BUTTON", L"Modify", WS_TABSTOP | BS_PUSHBUTTON, st_x() + st_w() - S(250), y, S(116), S(30), CMD_MODIFY_SEL);
            HWND b = pg_control(L"BUTTON", L"Uninstall", WS_TABSTOP | BS_PUSHBUTTON, st_x() + st_w() - S(124), y, S(116), S(30), CMD_UNINSTALL_SEL);
            EnableWindow(a, prog_can(p, PROG_CHANGE));
            EnableWindow(b, prog_can(p, PROG_UNINSTALL));
            y += S(46);
        }
        shown++;
    }
    if (!shown) y = st_para(y, g_filter[0] ? L"We couldn't find any apps that match your search." : L"No apps are installed.");
    y = st_head(y + S(8), L"Related settings");
    st_link(&y, L"Programs and Features", CMD_PAGE_FIRST + 40);
}

BOOL set_cmd_apps(int id, int code, HWND ctl)
{
    if (id >= CMD_SELECT && id < CMD_SELECT + 2048) { g_sel = id - CMD_SELECT; refresh_page(); return TRUE; }
    switch (id) {
    case CMD_FILTER:
        /* never rebuild inside the edit's own notification: it is destroyed by the rebuild */
        if (code == EN_CHANGE) { GetWindowTextW(ctl, g_filter, ARRAYSIZE(g_filter)); PostMessageW(g_page, WM_COMMAND, MAKEWPARAM(CMD_FILTER, 0xFFFE), 0); }
        else if (code == 0xFFFE) {
            refresh_page();
            if ((ctl = GetDlgItem(g_page, CMD_FILTER))) {
                SetFocus(ctl);
                SendMessageW(ctl, EM_SETSEL, lstrlenW(g_filter), lstrlenW(g_filter));
            }
        }
        return TRUE;
    case CMD_SORT: if (code == CBN_SELCHANGE) { g_sort = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0); refresh_page(); } return TRUE;
    case CMD_UNINSTALL_SEL: case CMD_MODIFY_SEL: {
        const struct program *p = prog_get(g_sel);
        if (!p) return TRUE;
        if (id == CMD_UNINSTALL_SEL) {
            WCHAR q[400];
            _snwprintf(q, ARRAYSIZE(q), L"This app and its related info will be uninstalled.\n\n%ls", p->name);
            if (MessageBoxW(g_main, q, L"Uninstall", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) return TRUE;
        }
        if (!prog_run(p, id == CMD_UNINSTALL_SEL ? PROG_UNINSTALL : PROG_CHANGE, FALSE)) st_status(L"The app's own uninstaller could not be started.");
        return TRUE;
    }
    case PROG_DONE: g_sel = -1; refresh_page(); return TRUE;
    case CMD_PAGE_FIRST + 40: ShellExecuteW(NULL, NULL, L"control.exe", L"appwiz.cpl", NULL, SW_SHOWNORMAL); return TRUE;
    }
    return FALSE;
}

/* ---- Default apps ---------------------------------------------------------------------------------- */
static const struct { const WCHAR *label, *exts[8], *proto; } KINDS[] = {
    { L"Photo viewer", { L".jpg", L".jpeg", L".png", L".gif", L".bmp", L".tif", L".tiff", NULL } },
    { L"Music player", { L".mp3", L".wav", L".ogg", L".flac", L".wma", L".m4a", NULL } },
    { L"Video player", { L".mp4", L".mkv", L".avi", L".webm", L".wmv", L".mov", NULL } },
    { L"Text editor", { L".txt", L".log", L".ini", NULL } },
    { L"PDF viewer", { L".pdf", NULL } },
    { L"Web browser", { L".htm", L".html", NULL }, L"http" },
    { L"Email", { NULL }, L"mailto" },
};
#define NKINDS ((int)ARRAYSIZE(KINDS))
#define MAX_CHOICES 16
static WCHAR g_choices[NKINDS][MAX_CHOICES][128];     /* ProgIDs */
static int g_nchoices[NKINDS];
enum { CMD_KIND_FIRST = CMD_PAGE_FIRST + 1, CMD_RESET = CMD_PAGE_FIRST + 50 };

/* Wine's HKEY_CLASSES_ROOT is the machine's classes only: the user's
 * Software\Classes (a per-user install's ProgIDs, the user's choices) is
 * not merged in as on Windows, so look there first. */
static LONG cls_open(const WCHAR *sub, HKEY *out)
{
    WCHAR path[400];
    _snwprintf(path, ARRAYSIZE(path), L"Software\\Classes\\%ls", sub);
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, out)) return 0;
    return RegOpenKeyExW(HKEY_CLASSES_ROOT, sub, 0, KEY_READ, out);
}

static LONG cls_get(const WCHAR *sub, const WCHAR *value, WCHAR *out, DWORD *cb)
{
    WCHAR path[400];
    DWORD size = *cb;
    _snwprintf(path, ARRAYSIZE(path), L"Software\\Classes\\%ls", sub);
    if (!RegGetValueW(HKEY_CURRENT_USER, path, value, RRF_RT_REG_SZ, NULL, out, cb)) return 0;
    *cb = size;
    return RegGetValueW(HKEY_CLASSES_ROOT, sub, value, RRF_RT_REG_SZ, NULL, out, cb);
}

static void progid_name(const WCHAR *progid, WCHAR *out, int cch)
{
    WCHAR cmd[MAX_PATH * 2] = L"", sub[300], *exe, *end;
    DWORD cb;
    out[0] = 0;
    cb = cch * sizeof(WCHAR);
    if (cls_get(progid, L"FriendlyTypeName", out, &cb) || out[0] == L'@') out[0] = 0;
    _snwprintf(sub, ARRAYSIZE(sub), L"%ls\\shell\\open\\command", progid);
    cb = sizeof(cmd);
    cls_get(sub, NULL, cmd, &cb);
    /* the program's own name reads best: "sg-photos64.exe" -> its FileDescription, else the file name */
    exe = cmd[0] == L'"' ? cmd + 1 : cmd;
    if ((end = wcschr(exe, cmd[0] == L'"' ? L'"' : L' '))) *end = 0;
    if (exe[0]) {
        DWORD h, size = GetFileVersionInfoSizeW(exe, &h);
        void *buf = size ? malloc(size) : NULL;
        WCHAR *desc;
        UINT len;
        if (buf && GetFileVersionInfoW(exe, 0, size, buf) &&
            VerQueryValueW(buf, L"\\StringFileInfo\\040904b0\\FileDescription", (void **)&desc, &len) && len > 1)
            lstrcpynW(out, desc, cch);
        free(buf);
    }
    if (!out[0]) {
        cb = cch * sizeof(WCHAR);
        if (cls_get(progid, NULL, out, &cb) || !out[0]) {
            const WCHAR *base = wcsrchr(exe, L'\\');
            lstrcpynW(out, base ? base + 1 : exe[0] ? exe : progid, cch);
        }
    }
}

static void current_progid(int k, WCHAR *out, int cch)
{
    DWORD cb = cch * sizeof(WCHAR);
    out[0] = 0;
    /* the user's choice first: Wine's HKEY_CLASSES_ROOT is the machine's
     * classes only, so what set_default wrote is not seen through it */
    if (KINDS[k].proto) {
        WCHAR sub[200];
        _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\%ls\\UserChoice", KINDS[k].proto);
        if (!RegGetValueW(HKEY_CURRENT_USER, sub, L"ProgId", RRF_RT_REG_SZ, NULL, out, &cb) && out[0]) return;
        cb = cch * sizeof(WCHAR);
    }
    if (KINDS[k].exts[0]) {
        WCHAR sub[200];
        _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls", KINDS[k].exts[0]);
        if (!RegGetValueW(HKEY_CURRENT_USER, sub, NULL, RRF_RT_REG_SZ, NULL, out, &cb) && out[0]) return;
        cb = cch * sizeof(WCHAR);
    }
    if (KINDS[k].exts[0]) RegGetValueW(HKEY_CLASSES_ROOT, KINDS[k].exts[0], NULL, RRF_RT_REG_SZ, NULL, out, &cb);
    else {
        WCHAR sub[64];
        _snwprintf(sub, ARRAYSIZE(sub), L"%ls\\shell\\open\\command", KINDS[k].proto);
        lstrcpynW(out, KINDS[k].proto, cch);
    }
}

static void add_choice(int k, const WCHAR *progid)
{
    int i;
    WCHAR sub[300];
    HKEY key;
    if (!progid[0] || g_nchoices[k] >= MAX_CHOICES) return;
    for (i = 0; i < g_nchoices[k]; i++) if (!lstrcmpiW(g_choices[k][i], progid)) return;
    _snwprintf(sub, ARRAYSIZE(sub), L"%ls\\shell\\open\\command", progid);
    if (cls_open(sub, &key)) return;     /* it must open something */
    RegCloseKey(key);
    lstrcpynW(g_choices[k][g_nchoices[k]++], progid, 128);
}

static void load_choices(int k)
{
    WCHAR cur[128], sub[128], name[128];
    HKEY key;
    DWORD i, n;
    int e;
    g_nchoices[k] = 0;
    current_progid(k, cur, ARRAYSIZE(cur));
    add_choice(k, cur);
    for (e = 0; KINDS[k].exts[e]; e++) {
        WCHAR usub[200];
        _snwprintf(sub, ARRAYSIZE(sub), L"%ls\\OpenWithProgids", KINDS[k].exts[e]);
        _snwprintf(usub, ARRAYSIZE(usub), L"Software\\Classes\\%ls", sub);
        if (!RegOpenKeyExW(HKEY_CURRENT_USER, usub, 0, KEY_READ, &key)) {
            for (i = 0; n = ARRAYSIZE(name), !RegEnumValueW(key, i, name, &n, NULL, NULL, NULL, NULL); i++) add_choice(k, name);
            RegCloseKey(key);
        }
        if (RegOpenKeyExW(HKEY_CLASSES_ROOT, sub, 0, KEY_READ, &key)) continue;
        for (i = 0; n = ARRAYSIZE(name), !RegEnumValueW(key, i, name, &n, NULL, NULL, NULL, NULL); i++) add_choice(k, name);
        RegCloseKey(key);
    }
    /* browsers and mail programs register as clients */
    if (KINDS[k].proto) {
        const WCHAR *clients = !lstrcmpW(KINDS[k].proto, L"http") ? L"Software\\Clients\\StartMenuInternet" : L"Software\\Clients\\Mail";
        HKEY roots[2] = { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER };
        int r;
        for (r = 0; r < 2; r++) {
            if (RegOpenKeyExW(roots[r], clients, 0, KEY_READ, &key)) continue;
            for (i = 0; n = ARRAYSIZE(name), !RegEnumKeyExW(key, i, name, &n, NULL, NULL, NULL, NULL); i++) {
                WCHAR assoc[300], progid[128];
                DWORD cb = sizeof(progid);
                _snwprintf(assoc, ARRAYSIZE(assoc), L"%ls\\Capabilities\\URLAssociations", name);
                if (!RegGetValueW(key, assoc, KINDS[k].proto, RRF_RT_REG_SZ, NULL, progid, &cb)) add_choice(k, progid);
            }
            RegCloseKey(key);
        }
    }
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

static void set_default(int k, const WCHAR *progid)
{
    WCHAR sub[300];
    int e;
    for (e = 0; KINDS[k].exts[e]; e++) {
        _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls", KINDS[k].exts[e]);
        reg_set_sz(HKEY_CURRENT_USER, sub, NULL, progid);
    }
    if (KINDS[k].proto) {
        /* a protocol is not a ProgID: its shell verbs are the chosen program's */
        HKEY from, to;
        _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls", KINDS[k].proto);
        RegDeleteTreeW(HKEY_CURRENT_USER, sub);
        if (!cls_open(progid, &from)) {
            if (!RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &to, NULL)) {
                copy_key(from, to);
                RegSetValueExW(to, L"URL Protocol", 0, REG_SZ, (const BYTE *)L"", sizeof(WCHAR));
                RegCloseKey(to);
            }
            RegCloseKey(from);
        }
        if (!lstrcmpW(KINDS[k].proto, L"http")) {
            WCHAR s2[64] = L"Software\\Classes\\https";
            RegDeleteTreeW(HKEY_CURRENT_USER, s2);
            if (!cls_open(progid, &from)) {
                if (!RegCreateKeyExW(HKEY_CURRENT_USER, s2, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &to, NULL)) {
                    copy_key(from, to);
                    RegSetValueExW(to, L"URL Protocol", 0, REG_SZ, (const BYTE *)L"", sizeof(WCHAR));
                    RegCloseKey(to);
                }
                RegCloseKey(from);
            }
        }
        _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\%ls\\UserChoice", KINDS[k].proto);
        reg_set_sz(HKEY_CURRENT_USER, sub, L"ProgId", progid);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

void set_build_defaultapps(void)
{
    int y = st_title(L"Default apps"), k, i, sel;
    WCHAR names[MAX_CHOICES][128], cur[128];
    const WCHAR *items[MAX_CHOICES + 1];
    y = st_head(y, L"Choose default apps");
    for (k = 0; k < NKINDS; k++) {
        load_choices(k);
        current_progid(k, cur, ARRAYSIZE(cur));
        sel = -1;
        for (i = 0; i < g_nchoices[k]; i++) {
            progid_name(g_choices[k][i], names[i], ARRAYSIZE(names[i]));
            items[i] = names[i];
            if (!lstrcmpiW(g_choices[k][i], cur)) sel = i;
        }
        items[g_nchoices[k]] = L"Choose a default";
        if (sel < 0) sel = g_nchoices[k];
        {
            HWND c = st_combo(&y, KINDS[k].label, items, g_nchoices[k] + (sel == g_nchoices[k]), sel, CMD_KIND_FIRST + k);
            if (!g_nchoices[k]) EnableWindow(c, FALSE);
        }
    }
    y = st_head(y, L"Reset to the recommended defaults");
    st_button(&y, L"Reset", CMD_RESET);
}

BOOL set_cmd_defaultapps(int id, int code, HWND ctl)
{
    if (id >= CMD_KIND_FIRST && id < CMD_KIND_FIRST + NKINDS) {
        int k = id - CMD_KIND_FIRST, i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        if (code == CBN_SELCHANGE && i >= 0 && i < g_nchoices[k]) { set_default(k, g_choices[k][i]); refresh_page(); }
        return TRUE;
    }
    if (id == CMD_RESET) {
        int k, e;
        WCHAR sub[300];
        for (k = 0; k < NKINDS; k++) {
            for (e = 0; KINDS[k].exts[e]; e++) {
                _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls", KINDS[k].exts[e]);
                { HKEY key; if (!RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_SET_VALUE, &key)) { RegDeleteValueW(key, NULL); RegCloseKey(key); } }
            }
            if (KINDS[k].proto) {
                _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls", KINDS[k].proto);
                RegDeleteTreeW(HKEY_CURRENT_USER, sub);
                if (!lstrcmpW(KINDS[k].proto, L"http")) RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\https");
            }
        }
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
        st_status(L"Your default apps are back to the recommended ones.");
        return TRUE;
    }
    return FALSE;
}

/* ---- Startup ------------------------------------------------------------------------------------- */
#define APPROVED L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved"
struct sitem { HKEY root; WCHAR approved[160], value[128], name[128], cmd[MAX_PATH]; BOOL on; };
static struct sitem g_st[64];
static int g_nst;
enum { CMD_ST_FIRST = CMD_PAGE_FIRST + 1 };

static BOOL approved_on(HKEY root, const WCHAR *sub, const WCHAR *value)
{
    BYTE b[12];
    DWORD cb = sizeof(b), type;
    HKEY k;
    BOOL on = TRUE;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k)) return TRUE;
    if (!RegQueryValueExW(k, value, NULL, &type, b, &cb) && type == REG_BINARY && cb >= 1) on = !(b[0] & 1);
    RegCloseKey(k);
    return on;
}

static void add_item(HKEY root, const WCHAR *approved, const WCHAR *value, const WCHAR *cmd)
{
    struct sitem *s;
    WCHAR *dot;
    if (g_nst >= (int)ARRAYSIZE(g_st)) return;
    s = &g_st[g_nst++];
    s->root = root;
    lstrcpynW(s->approved, approved, ARRAYSIZE(s->approved));
    lstrcpynW(s->value, value, ARRAYSIZE(s->value));
    lstrcpynW(s->cmd, cmd, ARRAYSIZE(s->cmd));
    lstrcpynW(s->name, value, ARRAYSIZE(s->name));
    if ((dot = wcsrchr(s->name, L'.')) && (!_wcsicmp(dot, L".lnk") || !_wcsicmp(dot, L".url"))) *dot = 0;
    s->on = approved_on(root, approved, value);
}

static void read_run(HKEY root, REGSAM view, const WCHAR *approved)
{
    HKEY k;
    DWORD i, n, cb, type;
    WCHAR name[128], data[MAX_PATH];
    if (RegOpenKeyExW(root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ | view, &k)) return;
    for (i = 0; n = ARRAYSIZE(name), cb = sizeof(data) - sizeof(WCHAR), !RegEnumValueW(k, i, name, &n, NULL, &type, (BYTE *)data, &cb); i++) {
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        data[cb / sizeof(WCHAR)] = 0;
        add_item(root, approved, name, data);
    }
    RegCloseKey(k);
}

static void read_folder(int csidl, HKEY root)
{
    WCHAR dir[MAX_PATH], pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (FAILED(SHGetFolderPathW(NULL, csidl, NULL, 0, dir))) return;
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
        if (!_wcsicmp(fd.cFileName, L"desktop.ini")) continue;
        _snwprintf(path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        add_item(root, APPROVED L"\\StartupFolder", fd.cFileName, path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void set_build_startup(void)
{
    int y = st_title(L"Startup"), i;
    g_nst = 0;
    read_run(HKEY_CURRENT_USER, 0, APPROVED L"\\Run");
    read_run(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, APPROVED L"\\Run");
    read_run(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, APPROVED L"\\Run32");
    read_folder(CSIDL_STARTUP, HKEY_CURRENT_USER);
    read_folder(CSIDL_COMMON_STARTUP, HKEY_LOCAL_MACHINE);
    y = st_head(y, L"Startup Apps");
    y = st_para(y, L"Apps can be configured to start when you sign in. In most cases, they'll start minimized or might only "
                   L"start a background task.");
    for (i = 0; i < g_nst; i++) {
        HWND c;
        pg_text(st_x(), y + S(4), st_w() - S(140), S(22), g_font_body, COL_TEXT, g_st[i].name, DT_SINGLELINE | DT_END_ELLIPSIS);
        pg_text(st_x(), y + S(26), st_w() - S(140), S(18), g_font_small, COL_SUBTLE, g_st[i].cmd, DT_SINGLELINE | DT_PATH_ELLIPSIS);
        c = pg_control(SET_TOGGLE_CLASS, g_st[i].name, WS_TABSTOP, st_x() + st_w() - S(110), y + S(6), S(110), S(26), CMD_ST_FIRST + i);
        SendMessageW(c, BM_SETCHECK, g_st[i].on ? BST_CHECKED : BST_UNCHECKED, 0);
        y += S(56);
    }
    if (!g_nst) y = st_para(y, L"No apps start when you sign in.");
}

BOOL set_cmd_startup(int id, int code, HWND ctl)
{
    (void)code;
    if (id >= CMD_ST_FIRST && id < CMD_ST_FIRST + g_nst) {
        struct sitem *s = &g_st[id - CMD_ST_FIRST];
        BYTE b[12] = { 0 };
        HKEY k;
        BOOL on = st_checked(ctl);
        b[0] = on ? 0x02 : 0x03;
        if (!on) { FILETIME ft; GetSystemTimeAsFileTime(&ft); memcpy(b + 4, &ft, sizeof(ft)); }
        if (!RegCreateKeyExW(s->root, s->approved, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) {
            if (RegSetValueExW(k, s->value, 0, REG_BINARY, b, sizeof(b)))
                st_status(L"Only an administrator can change an app that starts for everyone.");
            RegCloseKey(k);
        } else st_status(L"Only an administrator can change an app that starts for everyone.");
        s->on = on;
        return TRUE;
    }
    return FALSE;
}
