/* sg-control -- Settings > Accounts > Kiosk (automatic sign-in and the kiosk
 * app: Windows' AutoAdminLogon and assigned access), the Start apps picker
 * it shares with Settings > Apps > Startup, and the question every page
 * here asks: does this account sign in by itself?
 *
 * The machine's part is root's: sg-admind's autologon and kiosk (an
 * administrator's, through /admin), kept in /etc/stained-glass/autologon.conf
 * and greetd's initial session. sg-session's sg-kiosk reads the file in the
 * kiosk account's session and starts the app (sg-kiosk64.exe for a Windows
 * program). See stained-glass docs/guide, "A kiosk tablet".
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define COBJMACROS
#include "settings.h"
#include <shlobj.h>
#include <objbase.h>

/* ---- /etc/stained-glass/autologon.conf ------------------------------------------------ */
void autologon_read(struct autologon *a)
{
    WCHAR path[MAX_PATH] = L"Z:\\etc\\stained-glass\\autologon.conf", *p;
    char buf[8192], *line, *next;
    DWORD n = 0;
    HANDLE h;
    memset(a, 0, sizeof(*a));
    /* the gate's own file, a Unix path */
    if (GetEnvironmentVariableW(L"SG_AUTOLOGON_CONF", path + 2, MAX_PATH - 2) && path[2] == L'/')
        for (p = path + 2; *p; p++) if (*p == L'/') *p = L'\\';
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    if (!ReadFile(h, buf, sizeof(buf) - 1, &n, NULL)) n = 0;
    CloseHandle(h);
    buf[n] = 0;
    for (line = buf; line && *line; line = next) {
        char *eq;
        WCHAR *dst = NULL;
        int cch = 0;
        if ((next = strchr(line, '\n'))) *next++ = 0;
        if (line[0] == '#' || !(eq = strchr(line, '='))) continue;
        *eq = 0;
        if (!strcmp(line, "user")) { dst = a->user; cch = ARRAYSIZE(a->user); }
        else if (!strcmp(line, "kiosk-name")) { dst = a->name; cch = ARRAYSIZE(a->name); }
        else if (!strcmp(line, "kiosk-app")) { dst = a->app; cch = ARRAYSIZE(a->app); }
        else if (!strcmp(line, "kiosk-args")) { dst = a->args; cch = ARRAYSIZE(a->args); }
        else if (!strcmp(line, "kiosk-dir")) { dst = a->dir; cch = ARRAYSIZE(a->dir); }
        else if (!strcmp(line, "kiosk-desktop")) { dst = a->desktop; cch = ARRAYSIZE(a->desktop); }
        if (dst) MultiByteToWideChar(CP_UTF8, 0, eq + 1, -1, dst, cch);
    }
}

/* this account signs in by itself when the PC starts */
BOOL autologon_is_me(void)
{
    struct autologon a;
    WCHAR me[128], dom[128];
    autologon_read(&a);
    if (!a.user[0]) return FALSE;
    current_user(me, ARRAYSIZE(me), dom, ARRAYSIZE(dom));
    return !lstrcmpiW(me, a.user);
}

/* ---- Start's apps --------------------------------------------------------------------- */
static struct start_app *g_sa;
static int g_nsa, g_capsa;

static void sa_add(const WCHAR *name, const WCHAR *path)
{
    int i;
    for (i = 0; i < g_nsa; i++) if (!lstrcmpiW(g_sa[i].name, name)) return;   /* one per name: the user's wins */
    if (g_nsa == g_capsa) {
        int cap = g_capsa ? g_capsa * 2 : 128;
        struct start_app *n = realloc(g_sa, cap * sizeof(*n));
        if (!n) return;
        g_sa = n; g_capsa = cap;
    }
    lstrcpynW(g_sa[g_nsa].name, name, ARRAYSIZE(g_sa[g_nsa].name));
    lstrcpynW(g_sa[g_nsa].path, path, ARRAYSIZE(g_sa[g_nsa].path));
    g_nsa++;
}

/* .lnk and .url files under a Start Menu folder, as Start lists them */
static void sa_scan(const WCHAR *dir, int depth, BOOL recurse)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (depth > 4 || _snwprintf(pattern, MAX_PATH, L"%ls\\*", dir) < 0) return;
    if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        WCHAR full[MAX_PATH], name[128], *ext;
        if (fd.cFileName[0] == '.') continue;
        if (_snwprintf(full, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName) < 0) continue;
        full[MAX_PATH - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* the Startup folder is what starts, not an app */
            if (recurse && lstrcmpiW(fd.cFileName, L"Startup")) sa_scan(full, depth + 1, TRUE);
            continue;
        }
        if (!(ext = wcsrchr(fd.cFileName, '.')) || (lstrcmpiW(ext, L".lnk") && lstrcmpiW(ext, L".url"))) continue;
        lstrcpynW(name, fd.cFileName, (int)min(128, ext - fd.cFileName + 1));
        sa_add(name, full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int sa_cmp(const void *a, const void *b)
{
    return lstrcmpiW(((const struct start_app *)a)->name, ((const struct start_app *)b)->name);
}

/* Start's own programs beside this one (sg-start's list) and Wine's */
static const struct { const WCHAR *name, *file; } BESIDE[] = {
    { L"Task Manager", L"sg-taskmgr64.exe" }, { L"Remote Desktop Connection", L"sg-mstsc64.exe" },
    { L"Control Panel", L"sg-control64.exe" }, { L"Settings", L"sg-settings64.exe" }, { L"Terminal", L"sg-terminal64.exe" },
    { L"Alarms & Clock", L"sg-clock64.exe" }, { L"Media Player", L"sg-media64.exe" }, { L"Calculator", L"sg-calc64.exe" },
    { L"Photos", L"sg-photos64.exe" }, { L"Paint", L"sg-paint64.exe" }, { L"Sticky Notes", L"sg-sticky64.exe" },
    { L"Snipping Tool", L"sg-snip64.exe" }, { L"Character Map", L"sg-charmap64.exe" }, { L"WordPad", L"sg-wordpad64.exe" },
    { L"SG PDF", L"sg-pdf64.exe" }, { L"SG Office Documents", L"sg-documents64.exe" },
    { L"SG Office Spreadsheets", L"sg-spreadsheets64.exe" }, { L"SG Office Presentations", L"sg-presentations64.exe" },
    { L"Magnifier", L"sg-magnify64.exe" }, { L"On-Screen Keyboard", L"sg-osk64.exe" }, { L"SG Store", L"sg-store64.exe" },
};

int start_apps(const struct start_app **out)
{
    static const int roots[] = { CSIDL_PROGRAMS, CSIDL_COMMON_PROGRAMS };
    static const int tops[] = { CSIDL_STARTMENU, CSIDL_COMMON_STARTMENU };
    WCHAR dir[MAX_PATH], self[MAX_PATH], *slash;
    int i;
    g_nsa = 0;
    for (i = 0; i < 2; i++) if (SHGetSpecialFolderPathW(NULL, dir, roots[i], FALSE)) sa_scan(dir, 0, TRUE);
    for (i = 0; i < 2; i++) if (SHGetSpecialFolderPathW(NULL, dir, tops[i], FALSE)) sa_scan(dir, 0, FALSE);
    if (SearchPathW(NULL, L"notepad.exe", NULL, MAX_PATH, dir, NULL)) sa_add(L"Notepad", dir);
    if (SearchPathW(NULL, L"cmd.exe", NULL, MAX_PATH, dir, NULL)) sa_add(L"Command Prompt", dir);
    if (GetModuleFileNameW(NULL, self, MAX_PATH) && (slash = wcsrchr(self, '\\')))
        for (i = 0; i < (int)ARRAYSIZE(BESIDE); i++) {
            lstrcpyW(slash + 1, BESIDE[i].file);
            if (GetFileAttributesW(self) != INVALID_FILE_ATTRIBUTES) sa_add(BESIDE[i].name, self);
        }
    if (g_nsa) qsort(g_sa, g_nsa, sizeof(*g_sa), sa_cmp);
    *out = g_sa;
    return g_nsa;
}

/* one app, chosen from a list of Start's (SG_TEST_PICK: the gate names it) */
BOOL start_app_pick(const WCHAR *title, const WCHAR *intro, const WCHAR *ok, struct start_app *out)
{
    const struct start_app *apps;
    const WCHAR **names;
    WCHAR chosen[128] = L"", test[128];
    int n = start_apps(&apps), i;
    BOOL done = FALSE;
    if (!n) { message(g_main, title, L"Start has no apps to choose from.", TRUE); return FALSE; }
    if (GetEnvironmentVariableW(L"SG_TEST_PICK", test, ARRAYSIZE(test))) lstrcpynW(chosen, test, ARRAYSIZE(chosen));
    else {
        struct form_field f = { L"App:", chosen, ARRAYSIZE(chosen), FF_COMBO, NULL, n };
        if (!(names = calloc(n, sizeof(*names)))) return FALSE;
        for (i = 0; i < n; i++) names[i] = apps[i].name;
        f.options = names;
        lstrcpynW(chosen, names[0], ARRAYSIZE(chosen));
        done = run_form(g_main, title, intro, &f, 1, ok, FALSE);
        free(names);
        if (!done) return FALSE;
    }
    for (i = 0; i < n; i++)
        if (!lstrcmpiW(apps[i].name, chosen)) { *out = apps[i]; return TRUE; }
    return FALSE;
}

/* what a Start entry starts: a shortcut's target, arguments and folder; a
 * program, itself */
BOOL start_app_resolve(const WCHAR *path, WCHAR *target, WCHAR *args, int acch, WCHAR *dir)
{
    const WCHAR *ext = wcsrchr(path, '.');
    IShellLinkW *link;
    IPersistFile *file;
    BOOL ok = FALSE;
    target[0] = args[0] = dir[0] = 0;
    if (!ext || lstrcmpiW(ext, L".lnk")) {
        if (ext && !lstrcmpiW(ext, L".exe")) { lstrcpynW(target, path, MAX_PATH); return TRUE; }
        return FALSE;
    }
    CoInitialize(NULL);
    if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) {
        if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
            if (SUCCEEDED(IPersistFile_Load(file, path, STGM_READ)) &&
                SUCCEEDED(IShellLinkW_GetPath(link, target, MAX_PATH, NULL, SLGP_RAWPATH)) && target[0]) {
                WCHAR expanded[MAX_PATH];
                if (ExpandEnvironmentStringsW(target, expanded, MAX_PATH)) lstrcpynW(target, expanded, MAX_PATH);
                IShellLinkW_GetArguments(link, args, acch);
                IShellLinkW_GetWorkingDirectory(link, dir, MAX_PATH);
                ok = TRUE;
            }
            IPersistFile_Release(file);
        }
        IShellLinkW_Release(link);
    }
    return ok;
}

/* a Linux app's shortcut (sg-linuxapp64.exe --run "Z:\...\x.desktop"): its
 * .desktop file, as a Unix path */
BOOL start_app_linux(const WCHAR *target, const WCHAR *args, WCHAR *unix_path, int cch)
{
    const WCHAR *slash = wcsrchr(target, '\\'), *q, *e;
    WCHAR win[MAX_PATH];
    char *(CDECL *to_unix)(const WCHAR *);
    char *u;
    if (!slash || lstrcmpiW(slash + 1, L"sg-linuxapp64.exe") || wcsncmp(args, L"--run \"", 7)) return FALSE;
    q = args + 7;
    if (!(e = wcschr(q, L'"')) || e - q >= MAX_PATH) return FALSE;
    lstrcpynW(win, q, (int)(e - q) + 1);
    to_unix = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    if (!to_unix || !(u = to_unix(win))) return FALSE;
    MultiByteToWideChar(CP_UTF8, 0, u, -1, unix_path, cch);
    HeapFree(GetProcessHeap(), 0, u);
    return TRUE;
}

/* ---- Settings > Apps > Startup's "Add an app": a shortcut in the person's
 * Startup folder, which the shell opens at sign-in (wine-sg 0754) ------------------------- */
BOOL startup_add_app(const struct start_app *a)
{
    WCHAR dir[MAX_PATH], dst[MAX_PATH];
    const WCHAR *ext = wcsrchr(a->path, '.');
    BOOL ok = FALSE;
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_STARTUP | CSIDL_FLAG_CREATE, NULL, 0, dir))) return FALSE;
    _snwprintf(dst, MAX_PATH, L"%ls\\%ls%ls", dir, a->name, ext && !lstrcmpiW(ext, L".url") ? L".url" : L".lnk");
    dst[MAX_PATH - 1] = 0;
#ifndef SG_MUTANT_STARTUP_ADD_NOOP
    if (ext && (!lstrcmpiW(ext, L".lnk") || !lstrcmpiW(ext, L".url"))) ok = CopyFileW(a->path, dst, FALSE);
    else {
        IShellLinkW *link;
        IPersistFile *file;
        CoInitialize(NULL);
        if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) {
            IShellLinkW_SetPath(link, a->path);
            if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
                ok = SUCCEEDED(IPersistFile_Save(file, dst, TRUE));
                IPersistFile_Release(file);
            }
            IShellLinkW_Release(link);
        }
    }
#endif
    /* turned off before (Task Manager, the switch): on, now that it was asked for */
    if (ok) {
        HKEY k;
        if (!RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder",
                           0, KEY_SET_VALUE, &k)) {
            RegDeleteValueW(k, wcsrchr(dst, '\\') + 1);
            RegCloseKey(k);
        }
    }
    return ok;
}

/* ---- the page ----------------------------------------------------------------------------- */
enum { CMD_K_USER = CMD_PAGE_FIRST + 1, CMD_K_CHOOSE = SHIELD_ID(CMD_PAGE_FIRST + 2),
       CMD_K_NONE = SHIELD_ID(CMD_PAGE_FIRST + 3) };
#define MAX_KIOSK_USERS 64
static WCHAR g_kusers[MAX_KIOSK_USERS][64];
static int g_nkusers;

void set_build_kiosk(void)
{
    const struct account *acc;
    const WCHAR *items[MAX_KIOSK_USERS + 1];
    struct autologon a;
    WCHAR line[400];
    int y = st_title(L"Kiosk"), n = acc_load(&acc), i, sel = 0;

    autologon_read(&a);
    y = st_para(y, L"Set this PC up for one job: an account that signs in by itself when the PC starts, and one app that "
                   L"opens full screen in it and opens again whenever it is closed -- a music player on a tablet, a "
                   L"sign-in sheet by the door.");
    y = st_head(y, L"Automatic sign-in");
    items[0] = L"Nobody (show the sign-in screen)";
    g_nkusers = 0;
    for (i = 0; i < n && g_nkusers < MAX_KIOSK_USERS; i++) {
        lstrcpynW(g_kusers[g_nkusers], acc[i].name, ARRAYSIZE(g_kusers[0]));
        items[g_nkusers + 1] = g_kusers[g_nkusers];
        if (!lstrcmpiW(acc[i].name, a.user)) sel = g_nkusers + 1;
        g_nkusers++;
    }
    st_combo(&y, L"When this PC starts, sign in automatically as", items, g_nkusers + 1, sel, CMD_K_USER);
    y = st_para(y, L"Anyone who can turn this PC on can use that account without its password, so choose a standard "
                   L"account made for the job. Its screen does not lock when it turns off, and its saved passwords stay "
                   L"locked. Changing this needs an administrator; it takes effect the next time the PC starts.");
    y = st_head(y, L"Kiosk app");
    if (!a.user[0]) y = st_para(y, L"Choose an account to sign in automatically first: the kiosk app runs in that account.");
    else {
        if (a.name[0]) _snwprintf(line, ARRAYSIZE(line), L"%ls opens full screen when %ls signs in, and again whenever it is closed.",
                                  a.name, a.user);
        else _snwprintf(line, ARRAYSIZE(line), L"No kiosk app: %ls gets the ordinary desktop.", a.user);
        line[ARRAYSIZE(line) - 1] = 0;
        y = st_text(y, line);
        st_button(&y, a.name[0] ? L"Choose another app" : L"Choose an app", CMD_K_CHOOSE);
        if (a.name[0]) st_button(&y, L"Don't use a kiosk app", CMD_K_NONE);
    }
    y = st_head(y, L"Leaving the kiosk");
    y = st_para(y, L"Press Ctrl+Alt+Del and choose Sign out: the sign-in screen appears, and an administrator can sign in "
                   L"and change this page. In the kiosk the taskbar hides at the bottom edge of the screen -- point or "
                   L"touch there, or press the Start key, for Start.");
}

/* "x" for a command line: CommandLineToArgvW gives back exactly s */
static void quote_arg(WCHAR *out, int cch, const WCHAR *s)
{
    int o = 0, bs = 0;
    if (o < cch - 1) out[o++] = L'"';
    for (; *s && o < cch - 4; s++) {
        if (*s == L'\\') { bs++; out[o++] = *s; continue; }
        if (*s == L'"') { while (bs-- > 0 && o < cch - 4) out[o++] = L'\\'; out[o++] = L'\\'; }
        bs = 0;
        out[o++] = *s;
    }
    while (bs-- > 0 && o < cch - 3) out[o++] = L'\\';
    out[o++] = L'"';
    out[o] = 0;
}

static void kiosk_request(const WCHAR *name, const WCHAR *app, const WCHAR *args, const WCHAR *dir, const WCHAR *desktop)
{
    const WCHAR *v[5] = { name, app, args, dir, desktop };
    WCHAR cmd[4096] = L"/admin kiosk", q[1100];
    int i;
    for (i = 0; i < 5; i++) {
        quote_arg(q, ARRAYSIZE(q), v[i]);
        if (lstrlenW(cmd) + lstrlenW(q) + 2 >= (int)ARRAYSIZE(cmd)) return;
        lstrcatW(cmd, L" ");
        lstrcatW(cmd, q);
    }
    if (run_elevated(cmd)) refresh_when_back(); else refresh_page();
}

BOOL set_cmd_kiosk(int id, int code, HWND ctl)
{
    WCHAR cmd[160];
    if (id == CMD_K_USER) {
        LRESULT i = SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        if (code != CBN_SELCHANGE || i < 0 || i > g_nkusers) return TRUE;
        _snwprintf(cmd, ARRAYSIZE(cmd), L"/admin autologon \"%ls\"", i ? g_kusers[i - 1] : L"");
        cmd[ARRAYSIZE(cmd) - 1] = 0;
        if (run_elevated(cmd)) refresh_when_back(); else refresh_page();
        return TRUE;
    }
    if (id == CMD_K_NONE) { kiosk_request(L"", L"", L"", L"", L""); return TRUE; }
    if (id == CMD_K_CHOOSE) {
        struct start_app app;
        WCHAR target[MAX_PATH], args[1024], dir[MAX_PATH], desktop[1024];
        if (!start_app_pick(L"Choose the kiosk app", L"The app opens full screen when the account signs in, and opens "
                            L"again whenever it is closed.", L"Choose", &app)) return TRUE;
        if (!start_app_resolve(app.path, target, args, ARRAYSIZE(args), dir)) {
            message(g_main, L"Kiosk app", L"That app cannot be the kiosk app: it is not a program or a shortcut to one.", TRUE);
            return TRUE;
        }
        if (start_app_linux(target, args, desktop, ARRAYSIZE(desktop))) kiosk_request(app.name, L"", L"", L"", desktop);
        else kiosk_request(app.name, target, args, dir, L"");
        return TRUE;
    }
    return FALSE;
}
