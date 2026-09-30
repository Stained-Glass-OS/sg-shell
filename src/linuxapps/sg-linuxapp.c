/*
 * Linux apps in Start. A Linux program installed from its package (the SG
 * Store's Linux apps, a .deb, Flatpak) says how to start it in a .desktop
 * file; Start shows shortcuts. This makes one from the other:
 *
 *   sg-linuxapp64.exe --run FILE.desktop   starts the Linux program (gio launch)
 *   sg-linuxapp64.exe --sync               Start's "Linux apps" folder, once
 *   sg-linuxapp64.exe --watch              the same, then again whenever the
 *                                          applications folders change (Start
 *                                          starts this, one per session)
 *
 * Each app shown by a Linux desktop -- Type=Application, not NoDisplay or
 * Hidden, not for one desktop only (OnlyShowIn), its TryExec present, not
 * on the hidden list (SG_LINUXAPPS_HIDDEN, /usr/share/stained-glass/
 * linux-apps-hidden: the image's own plumbing, xterm and the like) -- gets
 * "Programs\Linux apps\NAME.lnk", which starts this program with --run, and
 * the app's own icon: its PNGs from the icon theme, put into an .ico.
 * Shortcuts in that folder whose app has gone are removed; the folder is ours.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <stdio.h>
#include <wchar.h>

#define FOLDER_NAME L"Linux apps"
#define MAX_APPS 512

typedef char *(CDECL *unix_name_fn)(const WCHAR *);
typedef WCHAR *(CDECL *dos_name_fn)(const char *);

static unix_name_fn p_unix_name;
static dos_name_fn p_dos_name;

static void init_wine(void)
{
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    p_unix_name = (unix_name_fn)(void *)GetProcAddress(k, "wine_get_unix_file_name");
    p_dos_name = (dos_name_fn)(void *)GetProcAddress(k, "wine_get_dos_file_name");
}

/* a Unix path as a Windows one (Z:\...) */
static BOOL dos_path(const char *unix_path, WCHAR *out, int len)
{
    WCHAR *d;
    if (p_dos_name && (d = p_dos_name(unix_path))) {
        lstrcpynW(out, d, len);
        HeapFree(GetProcessHeap(), 0, d);
        return TRUE;
    }
    if (MultiByteToWideChar(CP_UTF8, 0, unix_path, -1, out + 2, len - 2) <= 0) return FALSE;
    out[0] = 'Z'; out[1] = ':';
    for (d = out; *d; d++) if (*d == '/') *d = '\\';
    return TRUE;
}

static BOOL exists(const char *unix_path)
{
    WCHAR w[MAX_PATH];
    return dos_path(unix_path, w, MAX_PATH) && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
}

/* a whole (small) file, NUL-terminated; free() it */
static char *read_file(const WCHAR *path, DWORD max, DWORD *size)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    DWORD n = 0, got = 0;
    char *buf = NULL;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    n = GetFileSize(h, NULL);
    if (n != INVALID_FILE_SIZE && n <= max && (buf = malloc(n + 1))) {
        if (!ReadFile(h, buf, n, &got, NULL) || got != n) { free(buf); buf = NULL; }
        else { buf[n] = 0; if (size) *size = n; }
    }
    CloseHandle(h);
    return buf;
}

static BOOL env_a(const char *name, char *out, int len)
{
    DWORD n = GetEnvironmentVariableA(name, out, len);
    return n > 0 && (int)n < len;
}

/* ---- the .desktop files ------------------------------------------------ */

struct app {
    char id[128];               /* file name without .desktop */
    char file[MAX_PATH];        /* Unix path */
    WCHAR name[128];
    WCHAR comment[256];
    char icon[256];
};

static struct app *g_apps;
static int g_napps;

/* the value of KEY in the [Desktop Entry] group, or NULL */
static const char *entry_value(const char *text, const char *key, char *out, int len)
{
    const char *p = text;
    size_t klen = strlen(key);
    BOOL in_group = FALSE;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        if (n && p[n - 1] == '\r') n--;
        if (p[0] == '[') in_group = n == 15 && !strncmp(p, "[Desktop Entry]", 15);
        else if (in_group && n > klen && !strncmp(p, key, klen)) {
            const char *v = p + klen;
            while (v < p + n && *v == ' ') v++;
            if (v < p + n && *v == '=') {
                v++;
                while (v < p + n && *v == ' ') v++;
                n -= (size_t)(v - p);
                if ((int)n >= len) n = len - 1;
                memcpy(out, v, n);
                out[n] = 0;
                return out;
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    return NULL;
}

static BOOL is_true(const char *text, const char *key)
{
    char v[16];
    return entry_value(text, key, v, sizeof(v)) && !strcmp(v, "true");
}

/* the hidden list: one desktop id a line, "prefix*" for a family, # comments */
static char *g_hidden;

static void load_hidden(void)
{
    char path[MAX_PATH];
    WCHAR w[MAX_PATH];
    if (!env_a("SG_LINUXAPPS_HIDDEN", path, sizeof(path))) strcpy(path, "/usr/share/stained-glass/linux-apps-hidden");
    if (dos_path(path, w, MAX_PATH)) g_hidden = read_file(w, 1 << 16, NULL);
}

static BOOL hidden_id(const char *id)
{
    const char *p = g_hidden;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        while (n && (p[n - 1] == '\r' || p[n - 1] == ' ')) n--;
        if (n && p[0] != '#') {
            if (p[n - 1] == '*' ? !strncmp(id, p, n - 1) : (strlen(id) == n && !strncmp(id, p, n))) return TRUE;
        }
        if (!eol) break;
        p = eol + 1;
    }
    return FALSE;
}

/* TryExec: a program name found on the usual path, or an absolute path */
static BOOL try_exec_ok(const char *prog)
{
    static const char *dirs[] = { "/usr/local/bin", "/usr/bin", "/bin", "/usr/games", "/usr/local/games", "/usr/sbin", NULL };
    char path[MAX_PATH];
    int i;
    if (prog[0] == '/') return exists(prog);
    for (i = 0; dirs[i]; i++) {
        if (_snprintf(path, sizeof(path), "%s/%s", dirs[i], prog) < 0) continue;
        path[sizeof(path) - 1] = 0;
        if (exists(path)) return TRUE;
    }
    return FALSE;
}

static void add_desktop_file(const char *unix_file, const char *id)
{
    WCHAR w[MAX_PATH];
    char *text, v[512];
    struct app *a;
    int i;

    for (i = 0; i < g_napps; i++) if (!strcmp(g_apps[i].id, id)) return;   /* an earlier folder's wins */
    if (g_napps >= MAX_APPS || hidden_id(id) || !dos_path(unix_file, w, MAX_PATH)) return;
    if (!(text = read_file(w, 1 << 20, NULL))) return;
    if (!entry_value(text, "Type", v, sizeof(v)) || strcmp(v, "Application")) goto out;
    if (is_true(text, "NoDisplay") || is_true(text, "Hidden")) goto out;
    if (entry_value(text, "OnlyShowIn", v, sizeof(v)) && v[0]) goto out;
    if (!entry_value(text, "Exec", v, sizeof(v)) || !v[0]) goto out;
    if (entry_value(text, "TryExec", v, sizeof(v)) && v[0] && !try_exec_ok(v)) goto out;
    if (!entry_value(text, "Name", v, sizeof(v)) || !v[0]) goto out;
    a = &g_apps[g_napps];
    memset(a, 0, sizeof(*a));
    lstrcpynA(a->id, id, sizeof(a->id));
    lstrcpynA(a->file, unix_file, sizeof(a->file));
    MultiByteToWideChar(CP_UTF8, 0, v, -1, a->name, ARRAYSIZE(a->name));
    a->name[ARRAYSIZE(a->name) - 1] = 0;
    if (entry_value(text, "Comment", v, sizeof(v))) {
        MultiByteToWideChar(CP_UTF8, 0, v, -1, a->comment, ARRAYSIZE(a->comment));
        a->comment[ARRAYSIZE(a->comment) - 1] = 0;
    }
    if (entry_value(text, "Icon", v, sizeof(v))) lstrcpynA(a->icon, v, sizeof(a->icon));
    g_napps++;
out:
    free(text);
}

/* the data folders, most particular first (XDG base directories, Flatpak's) */
static int data_dirs(char dirs[][MAX_PATH], int max)
{
    char home[MAX_PATH] = "", buf[4096], *p, *next;
    int n = 0;

    env_a("HOME", home, sizeof(home));
    if (env_a("XDG_DATA_HOME", dirs[n], MAX_PATH)) n++;
    else if (home[0] && _snprintf(dirs[n], MAX_PATH, "%s/.local/share", home) > 0) n++;
    if (home[0] && n < max && _snprintf(dirs[n], MAX_PATH, "%s/.local/share/flatpak/exports/share", home) > 0) n++;
    if (!env_a("XDG_DATA_DIRS", buf, sizeof(buf)) || !buf[0])
        strcpy(buf, "/usr/local/share:/usr/share:/var/lib/flatpak/exports/share");
    for (p = buf; p && *p && n < max; p = next) {
        if ((next = strchr(p, ':'))) *next++ = 0;
        if (p[0] == '/') lstrcpynA(dirs[n++], p, MAX_PATH);
    }
    return n;
}

static void scan_apps(void)
{
    char dirs[16][MAX_PATH];
    int nd = data_dirs(dirs, 16), i;

    g_napps = 0;
    for (i = 0; i < nd; i++) {
        char dir[MAX_PATH];
        WCHAR pattern[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        if (_snprintf(dir, sizeof(dir), "%s/applications", dirs[i]) < 0 || !dos_path(dir, pattern, MAX_PATH - 16)) continue;
        lstrcatW(pattern, L"\\*.desktop");
        if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) continue;
        do {
            char name[MAX_PATH], file[MAX_PATH], *ext;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL)) continue;
            if (!(ext = strrchr(name, '.')) || strcmp(ext, ".desktop")) continue;
            if (_snprintf(file, sizeof(file), "%s/%s", dir, name) < 0) continue;
            file[sizeof(file) - 1] = 0;
            *ext = 0;
            add_desktop_file(file, name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

/* ---- icons: the theme's PNGs, put into an .ico ------------------------- */

static BOOL png_size(const unsigned char *b, DWORD n, DWORD *w, DWORD *h)
{
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
    if (n < 24 || memcmp(b, sig, 8) || memcmp(b + 12, "IHDR", 4)) return FALSE;
    *w = (DWORD)b[16] << 24 | b[17] << 16 | b[18] << 8 | b[19];
    *h = (DWORD)b[20] << 24 | b[21] << 16 | b[22] << 8 | b[23];
    return *w && *h && *w <= 256 && *h <= 256;
}

/* Icon= names a theme icon, or is a path; the PNGs found, largest first, up to 4 */
static int find_pngs(const char *icon, char found[][MAX_PATH], int max)
{
    static const char *sizes[] = { "256x256", "128x128", "64x64", "48x48", "32x32", NULL };
    char dirs[16][MAX_PATH], name[256], *ext;
    int nd, i, s, n = 0;

    if (!icon[0]) return 0;
    if (icon[0] == '/') {
        if ((ext = strrchr(icon, '.')) && !_stricmp(ext, ".png") && exists(icon)) { lstrcpynA(found[0], icon, MAX_PATH); return 1; }
        return 0;
    }
    lstrcpynA(name, icon, sizeof(name));
    if ((ext = strrchr(name, '.')) && (!_stricmp(ext, ".png") || !_stricmp(ext, ".svg") || !_stricmp(ext, ".xpm"))) *ext = 0;
    if (strchr(name, '/') || strstr(name, "..")) return 0;
    nd = data_dirs(dirs, 16);
    for (s = 0; sizes[s] && n < max; s++)
        for (i = 0; i < nd; i++) {
            char p[MAX_PATH];
            if (_snprintf(p, sizeof(p), "%s/icons/hicolor/%s/apps/%s.png", dirs[i], sizes[s], name) < 0) continue;
            p[sizeof(p) - 1] = 0;
            if (exists(p)) { lstrcpynA(found[n++], p, MAX_PATH); break; }
        }
    if (!n) {
        char p[MAX_PATH];
        if (_snprintf(p, sizeof(p), "/usr/share/pixmaps/%s.png", name) > 0 && exists(p)) lstrcpynA(found[n++], p, MAX_PATH);
    }
    return n;
}

/* the .ico for an app, written into DIR; FALSE if it has no PNG icon */
static BOOL make_icon(const struct app *a, const WCHAR *dir, WCHAR *ico, int len)
{
    char pngs[4][MAX_PATH];
    unsigned char *data[4];
    DWORD size[4], w[4], h[4], off, i, count = 0;
    int n = find_pngs(a->icon, pngs, 4);
    HANDLE f;
    BOOL ok = FALSE;

    for (i = 0; i < (DWORD)n; i++) {
        WCHAR wp[MAX_PATH];
        if (!dos_path(pngs[i], wp, MAX_PATH)) continue;
        if (!(data[count] = (unsigned char *)read_file(wp, 4 << 20, &size[count]))) continue;
        if (!png_size(data[count], size[count], &w[count], &h[count])) { free(data[count]); continue; }
        count++;
    }
    if (!count) return FALSE;
    _snwprintf(ico, len, L"%ls\\%hs.ico", dir, a->id);
    ico[len - 1] = 0;
    f = CreateFileW(ico, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WORD head[3] = { 0, 1, (WORD)count };
        DWORD put;
        ok = WriteFile(f, head, sizeof(head), &put, NULL);
        off = 6 + 16 * count;
        for (i = 0; i < count && ok; i++) {
            BYTE e[16] = { 0 };
            e[0] = w[i] >= 256 ? 0 : (BYTE)w[i];
            e[1] = h[i] >= 256 ? 0 : (BYTE)h[i];
            e[4] = 1; e[6] = 32;
            memcpy(e + 8, &size[i], 4);
            memcpy(e + 12, &off, 4);
            off += size[i];
            ok = WriteFile(f, e, 16, &put, NULL);
        }
        for (i = 0; i < count && ok; i++) ok = WriteFile(f, data[i], size[i], &put, NULL);
        CloseHandle(f);
        if (!ok) DeleteFileW(ico);
    }
    for (i = 0; i < count; i++) free(data[i]);
    return ok;
}

/* ---- the shortcuts ----------------------------------------------------- */

static void safe_name(const WCHAR *in, WCHAR *out, int len)
{
    int i, j = 0;
    for (i = 0; in[i] && j < len - 1; i++)
        out[j++] = wcschr(L"\\/:*?\"<>|", in[i]) || in[i] < 32 ? L' ' : in[i];
    while (j && (out[j - 1] == L' ' || out[j - 1] == L'.')) j--;
    out[j] = 0;
}

/* writes the shortcut unless one saying the same is there (keeps its date:
 * Start's "Recently added" goes by it) */
static void write_link(const WCHAR *lnk, const WCHAR *target, const WCHAR *args, const WCHAR *icon, const WCHAR *desc)
{
    IShellLinkW *link;
    IPersistFile *file;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) return;
    if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
        BOOL same = FALSE;
        if (GetFileAttributesW(lnk) != INVALID_FILE_ATTRIBUTES && SUCCEEDED(IPersistFile_Load(file, lnk, STGM_READ))) {
            WCHAR a[1024] = L"", ic[MAX_PATH] = L"", d[256] = L"";
            int idx = 0;
            IShellLinkW_GetArguments(link, a, ARRAYSIZE(a));
            IShellLinkW_GetIconLocation(link, ic, ARRAYSIZE(ic), &idx);
            IShellLinkW_GetDescription(link, d, ARRAYSIZE(d));
            same = !lstrcmpW(a, args) && !lstrcmpiW(ic, icon ? icon : L"") && !lstrcmpW(d, desc);
        }
        if (!same) {
            IShellLinkW_SetPath(link, target);
            IShellLinkW_SetArguments(link, args);
            IShellLinkW_SetDescription(link, desc);
            IShellLinkW_SetIconLocation(link, icon ? icon : L"", 0);
            IPersistFile_Save(file, lnk, TRUE);
        }
        IPersistFile_Release(file);
    }
    IShellLinkW_Release(link);
}

/* a shortcut NAME.lnk somewhere under DIR but our folder (a Windows program
 * of the same name: Steam for Windows beside Steam for Linux) */
static BOOL name_taken(const WCHAR *dir, const WCHAR *name, int depth)
{
    WCHAR pattern[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    BOOL found = FALSE;

    if (depth > 3 || _snwprintf(pattern, MAX_PATH, L"%ls\\*", dir) < 0) return FALSE;
    if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (fd.cFileName[0] == '.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!depth && !lstrcmpiW(fd.cFileName, FOLDER_NAME)) continue;
            if (_snwprintf(sub, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName) >= 0) found = name_taken(sub, name, depth + 1);
        } else {
            WCHAR *ext = wcsrchr(fd.cFileName, '.');
            found = ext && !lstrcmpiW(ext, L".lnk") && (size_t)(ext - fd.cFileName) == wcslen(name)
                    && !wcsnicmp(fd.cFileName, name, ext - fd.cFileName);
        }
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

static int sync_apps(void)
{
    WCHAR programs[MAX_PATH], common[MAX_PATH], folder[MAX_PATH], icons[MAX_PATH], self[MAX_PATH], pattern[MAX_PATH];
    WCHAR (*made)[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int i, j, nmade = 0;

    if (!SHGetSpecialFolderPathW(NULL, programs, CSIDL_PROGRAMS, TRUE)) return 1;
    if (!SHGetSpecialFolderPathW(NULL, icons, CSIDL_LOCAL_APPDATA, TRUE)) return 1;
    if (!SHGetSpecialFolderPathW(NULL, common, CSIDL_COMMON_PROGRAMS, FALSE)) common[0] = 0;
    _snwprintf(folder, MAX_PATH, L"%ls\\" FOLDER_NAME, programs);
    lstrcatW(icons, L"\\Stained Glass");
    CreateDirectoryW(icons, NULL);
    lstrcatW(icons, L"\\Linux app icons");
    CreateDirectoryW(icons, NULL);
    GetModuleFileNameW(NULL, self, MAX_PATH);

    scan_apps();
    if (!(made = calloc(g_napps + 1, sizeof(*made)))) return 1;
    if (g_napps) CreateDirectoryW(folder, NULL);
    for (i = 0; i < g_napps; i++) {
        WCHAR name[140], lnk[MAX_PATH], args[1024], file[MAX_PATH], ico[MAX_PATH];
        BOOL dup = FALSE, has_icon;
        safe_name(g_apps[i].name, name, ARRAYSIZE(name));
        if (!name[0]) continue;
        for (j = 0; j < i; j++) if (!lstrcmpiW(g_apps[j].name, g_apps[i].name)) dup = TRUE;
        if (name_taken(programs, name, 0) || (common[0] && name_taken(common, name, 0))) lstrcatW(name, L" (Linux)");
        if (dup) _snwprintf(lnk, MAX_PATH, L"%ls\\%ls (%hs).lnk", folder, name, g_apps[i].id);
        else _snwprintf(lnk, MAX_PATH, L"%ls\\%ls.lnk", folder, name);
        lnk[MAX_PATH - 1] = 0;
        if (!dos_path(g_apps[i].file, file, MAX_PATH)) continue;
        _snwprintf(args, ARRAYSIZE(args), L"--run \"%ls\"", file);
        args[ARRAYSIZE(args) - 1] = 0;
        has_icon = make_icon(&g_apps[i], icons, ico, MAX_PATH);
        write_link(lnk, self, args, has_icon ? ico : NULL, g_apps[i].comment);
        lstrcpynW(made[nmade++], lnk, MAX_PATH);
    }
    /* what is no longer there */
    _snwprintf(pattern, MAX_PATH, L"%ls\\*.lnk", folder);
    if ((h = FindFirstFileW(pattern, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            WCHAR lnk[MAX_PATH];
            BOOL keep = FALSE;
            _snwprintf(lnk, MAX_PATH, L"%ls\\%ls", folder, fd.cFileName);
            for (j = 0; j < nmade && !keep; j++) keep = !lstrcmpiW(made[j], lnk);
            if (!keep) DeleteFileW(lnk);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!nmade) RemoveDirectoryW(folder);
    free(made);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, folder, NULL);
    return 0;
}

/* ---- starting an app --------------------------------------------------- */

static int run_app(const WCHAR *file)
{
    LONG (WINAPI *spawnvp)(char * const argv[], int wait);
    char *unix_file = NULL, gio[] = "/usr/bin/gio", launch[] = "launch";
    char *argv[4];
    LONG r = 1;

    spawnvp = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__wine_unix_spawnvp");
    if (p_unix_name) unix_file = p_unix_name(file);
    if (spawnvp && unix_file) {
        argv[0] = gio; argv[1] = launch; argv[2] = unix_file; argv[3] = NULL;
        r = spawnvp(argv, FALSE);
    }
    if (unix_file) HeapFree(GetProcessHeap(), 0, unix_file);
    if (r) {
        WCHAR msg[MAX_PATH + 64];
        _snwprintf(msg, ARRAYSIZE(msg), L"The Linux app could not be started:\n%ls", file);
        msg[ARRAYSIZE(msg) - 1] = 0;
        MessageBoxW(NULL, msg, L"Linux apps", MB_OK | MB_ICONERROR);
    }
    return r ? 1 : 0;
}

/* ---- keeping Start up to date ------------------------------------------ */

static int watch_apps(void)
{
    HANDLE mutex = CreateMutexW(NULL, TRUE, L"Local\\SG.LinuxApps.Watch");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) return 0;   /* one per session */
    for (;;) {
        char dirs[16][MAX_PATH];
        HANDLE ev[16];
        int nd = data_dirs(dirs, 16), nev = 0, i;
        DWORD r;

        sync_apps();
        for (i = 0; i < nd; i++) {
            char dir[MAX_PATH];
            WCHAR w[MAX_PATH];
            HANDLE e;
            if (_snprintf(dir, sizeof(dir), "%s/applications", dirs[i]) < 0 || !dos_path(dir, w, MAX_PATH)) continue;
            e = FindFirstChangeNotificationW(w, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
            if (e != INVALID_HANDLE_VALUE) ev[nev++] = e;
        }
        /* a change, or every ten minutes (a folder made since: Flatpak's first app) */
        r = nev ? WaitForMultipleObjects(nev, ev, FALSE, 600000) : (Sleep(600000), WAIT_TIMEOUT);
        for (i = 0; i < nev; i++) FindCloseChangeNotification(ev[i]);
        if (r != WAIT_TIMEOUT) Sleep(2000);   /* a package puts in several files: once, after */
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    int argc = 0, ret = 2;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    (void)inst; (void)prev; (void)cmdline; (void)show;
    init_wine();
    load_hidden();
    if (!(g_apps = calloc(MAX_APPS, sizeof(*g_apps)))) return 1;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (argv && argc >= 3 && !lstrcmpiW(argv[1], L"--run")) ret = run_app(argv[2]);
    else if (argv && argc >= 2 && !lstrcmpiW(argv[1], L"--sync")) ret = sync_apps();
    else if (argv && argc >= 2 && !lstrcmpiW(argv[1], L"--watch")) ret = watch_apps();
    CoUninitialize();
    return ret;
}
