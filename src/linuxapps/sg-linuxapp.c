/*
 * Linux apps in Start. A Linux program installed from its package (the SG
 * Store's Linux apps, a .deb, Flatpak) says how to start it in a .desktop
 * file; Start shows shortcuts. This makes one from the other:
 *
 *   sg-linuxapp64.exe --run FILE.desktop   starts the Linux program (gio launch)
 *   sg-linuxapp64.exe --open FILE.desktop ARG   the same, opening ARG (a file
 *                                          or a URL) -- an app chosen in
 *                                          Settings > Default apps
 *   sg-linuxapp64.exe --sync               Start's "Linux apps" folder, once
 *   sg-linuxapp64.exe --watch              the same, then again whenever the
 *                                          applications folders change (Start
 *                                          starts this, one per session)
 *
 * Each app shown by a Linux desktop -- Type=Application, not NoDisplay or
 * Hidden, not for one desktop only (OnlyShowIn), its TryExec present, not
 * on the hidden list (SG_LINUXAPPS_HIDDEN, /usr/share/stained-glass/
 * linux-apps-hidden: the image's own plumbing, xterm and the like), not
 * brought in as a dependency of another (sg-linuxapp-deps) -- gets
 * "Programs\Linux apps\NAME.lnk", which starts this program with --run, and
 * the app's own icon: its PNGs from the icon theme, put into an .ico.
 * An app that opens files or links (MimeType=) is also a program Windows
 * programs and Settings > Default apps know: a ProgID SG.LinuxApp.<id> in
 * the user's classes, offered for the extensions of its types
 * (OpenWithProgids), and a browser or mail program among the clients
 * (Software\Clients). Firefox ESR could not be made the default browser.
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

/* the user's Unix home: Wine gives its programs no HOME, but WINEHOMEDIR
 * (\??\Z:\home\user) -- without it ~/.local/share and ~/.config were missed */
static BOOL home_unix(char *out, int len)
{
    WCHAR w[MAX_PATH];
    DWORD n;
    char *u;
    if (env_a("HOME", out, len) && out[0]) return TRUE;
#ifdef SG_MUTANT_HOME_ENV_ONLY
    return FALSE;
#endif
    n = GetEnvironmentVariableW(L"WINEHOMEDIR", w, MAX_PATH);
    if (!n || n >= MAX_PATH || !p_unix_name) return FALSE;
    if (!(u = p_unix_name(!wcsncmp(w, L"\\??\\", 4) ? w + 4 : w))) return FALSE;
    lstrcpynA(out, u, len);
    HeapFree(GetProcessHeap(), 0, u);
    return out[0] != 0;
}

/* ---- the .desktop files ------------------------------------------------ */

struct app {
    char id[128];               /* file name without .desktop */
    char file[MAX_PATH];        /* Unix path */
    WCHAR name[128];
    WCHAR comment[256];
    char icon[256];
    char wmclass[128];          /* StartupWMClass: its windows' class */
    char exe[128];              /* Exec's program name: often its windows' class too */
    char mime[4096];            /* MimeType=: what it opens, ';'-separated */
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

/* What came with something else: the desktop ids sg-linuxapp-deps lists
 * (a package apt installed as a dependency, not one a program the person
 * installed is just a wrapper for -- KDE's System Settings with KDE Connect).
 * Asked once a sync; without the helper nothing more is left out. */
static char *g_dep_hidden;

static void load_dep_hidden(void)
{
    LONG (WINAPI *spawnvp)(char * const argv[], int wait);
    char helper[MAX_PATH], *uout = NULL, *argv[3];
    WCHAR tmp[MAX_PATH], out[MAX_PATH];

    free(g_dep_hidden);
    g_dep_hidden = NULL;
    if (!env_a("SG_LINUXAPP_DEPS", helper, sizeof(helper))) strcpy(helper, "/usr/libexec/stained-glass/sg-linuxapp-deps");
    spawnvp = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__wine_unix_spawnvp");
    if (!spawnvp || !p_unix_name || !exists(helper) || !GetTempPathW(MAX_PATH, tmp) ||
        !GetTempFileNameW(tmp, L"sgd", 0, out) || !(uout = p_unix_name(out))) return;
    argv[0] = helper; argv[1] = uout; argv[2] = NULL;
    if (!spawnvp(argv, TRUE)) g_dep_hidden = read_file(out, 1 << 16, NULL);
    DeleteFileW(out);
    HeapFree(GetProcessHeap(), 0, uout);
}

static BOOL in_list(const char *list, const char *id)
{
    const char *p = list;
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

static BOOL hidden_id(const char *id)
{
#ifndef SG_MUTANT_NODEPS
    if (in_list(g_dep_hidden, id)) return TRUE;
#endif
    return in_list(g_hidden, id);
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
    if (entry_value(text, "StartupWMClass", v, sizeof(v))) lstrcpynA(a->wmclass, v, sizeof(a->wmclass));
    entry_value(text, "MimeType", a->mime, sizeof(a->mime));
    if (entry_value(text, "Exec", v, sizeof(v))) {
        char *end = v + strcspn(v, " \t"), *base;
        *end = 0;
        base = strrchr(v, '/') ? strrchr(v, '/') + 1 : v;
        if (strcmp(base, "env") && strcmp(base, "flatpak") && strcmp(base, "sh")) lstrcpynA(a->exe, base, sizeof(a->exe));
    }
    g_napps++;
out:
    free(text);
}

/* the data folders, most particular first (XDG base directories, Flatpak's) */
static int data_dirs(char dirs[][MAX_PATH], int max)
{
    char home[MAX_PATH] = "", buf[4096], *p, *next;
    int n = 0;

    home_unix(home, sizeof(home));
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

/* Icon= as a scalable (SVG) theme icon or path: GNOME's apps ship only those */
static BOOL find_svg(const char *icon, char *out)
{
    char dirs[16][MAX_PATH], name[256], *ext;
    int nd, i;

    if (!icon[0]) return FALSE;
    if (icon[0] == '/') {
        if ((ext = strrchr(icon, '.')) && !_stricmp(ext, ".svg") && exists(icon)) { lstrcpynA(out, icon, MAX_PATH); return TRUE; }
        return FALSE;
    }
    lstrcpynA(name, icon, sizeof(name));
    if ((ext = strrchr(name, '.')) && !_stricmp(ext, ".svg")) *ext = 0;
    if (strchr(name, '/') || strstr(name, "..")) return FALSE;
    nd = data_dirs(dirs, 16);
    for (i = 0; i < nd; i++) {
        if (_snprintf(out, MAX_PATH, "%s/icons/hicolor/scalable/apps/%s.svg", dirs[i], name) < 0) continue;
        out[MAX_PATH - 1] = 0;
        if (exists(out)) return TRUE;
    }
    if (_snprintf(out, MAX_PATH, "/usr/share/pixmaps/%s.svg", name) > 0 && exists(out)) return TRUE;
    return FALSE;
}

/* the SVG drawn at 256 and 48 px by rsvg-convert, as PNGs in DIR (kept: made once) */
static int svg_pngs(const char *icon, const WCHAR *dir, const char *id, char found[][MAX_PATH], int max)
{
    static const int px[] = { 256, 48 };
    LONG (WINAPI *spawnvp)(char * const argv[], int wait);
    char svg[MAX_PATH], *udir;
    int i, n = 0;

    if (!find_svg(icon, svg) || !p_unix_name) return 0;
    spawnvp = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__wine_unix_spawnvp");
    if (!(udir = p_unix_name(dir))) return 0;
    for (i = 0; i < (int)ARRAYSIZE(px) && n < max; i++) {
        char out[MAX_PATH], size[8], tool[] = "/usr/bin/rsvg-convert", w[] = "-w", h[] = "-h", o[] = "-o";
        char *argv[9];
        if (_snprintf(out, sizeof(out), "%s/%s-%d.png", udir, id, px[i]) < 0) continue;
        out[sizeof(out) - 1] = 0;
        if (!exists(out) && spawnvp && exists(tool)) {
            _snprintf(size, sizeof(size), "%d", px[i]);
            argv[0] = tool; argv[1] = w; argv[2] = size; argv[3] = h; argv[4] = size;
            argv[5] = o; argv[6] = out; argv[7] = svg; argv[8] = NULL;
            spawnvp(argv, TRUE);
        }
        if (exists(out)) lstrcpynA(found[n++], out, MAX_PATH);
    }
    HeapFree(GetProcessHeap(), 0, udir);
    return n;
}

/* the .ico for an app, written into DIR; FALSE if it has no PNG or SVG icon */
static BOOL make_icon(const struct app *a, const WCHAR *dir, WCHAR *ico, int len)
{
    char pngs[4][MAX_PATH];
    unsigned char *data[4];
    DWORD size[4], w[4], h[4], off, i, count = 0;
    int n = find_pngs(a->icon, pngs, 4);
#ifndef SG_MUTANT_NOSVG
    if (!n) n = svg_pngs(a->icon, dir, a->id, pngs, 4);
#endif
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

/* copies of an app's .ico named after its windows' class (StartupWMClass,
 * and Exec's program name: GTK's default class), for the taskbar: it shows a
 * Linux window with the icon named after the window's class */
static void icon_aliases(const struct app *a, const WCHAR *dir, const WCHAR *ico)
{
    const char *names[2] = { a->wmclass, a->exe };
    int i;
    for (i = 0; i < 2; i++) {
        WCHAR alias[MAX_PATH];
        if (!names[i][0] || !_stricmp(names[i], a->id) || strpbrk(names[i], "\\/:*?\"<>|")) continue;
        if (i == 1 && !_stricmp(names[1], a->wmclass)) continue;
        _snwprintf(alias, MAX_PATH, L"%ls\\%hs.ico", dir, names[i]);
        alias[MAX_PATH - 1] = 0;
        CopyFileW(ico, alias, FALSE);
    }
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

/* ---- what the apps open: ProgIDs for Default apps and Windows programs -- */

/* the types Settings > Default apps chooses for, and their extensions */
static const struct { const char *mime; const WCHAR *exts[4]; } MIME_EXTS[] = {
    { "image/jpeg", { L".jpg", L".jpeg" } }, { "image/png", { L".png" } }, { "image/gif", { L".gif" } },
    { "image/bmp", { L".bmp" } }, { "image/tiff", { L".tif", L".tiff" } },
    { "audio/mpeg", { L".mp3" } }, { "audio/x-wav", { L".wav" } }, { "audio/wav", { L".wav" } },
    { "audio/ogg", { L".ogg" } }, { "audio/x-vorbis+ogg", { L".ogg" } }, { "audio/flac", { L".flac" } },
    { "audio/x-flac", { L".flac" } }, { "audio/x-ms-wma", { L".wma" } }, { "audio/mp4", { L".m4a" } },
    { "audio/x-m4a", { L".m4a" } },
    { "video/mp4", { L".mp4" } }, { "video/x-matroska", { L".mkv" } }, { "video/x-msvideo", { L".avi" } },
    { "video/webm", { L".webm" } }, { "video/x-ms-wmv", { L".wmv" } }, { "video/quicktime", { L".mov" } },
    { "text/plain", { L".txt", L".log", L".ini" } }, { "application/pdf", { L".pdf" } },
    { "application/vnd.openxmlformats-officedocument.wordprocessingml.document", { L".docx" } },
    { "application/msword", { L".doc" } }, { "application/vnd.ms-word.document.macroEnabled.12", { L".docm" } },
    { "application/vnd.oasis.opendocument.text", { L".odt" } },
    { "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet", { L".xlsx" } },
    { "application/vnd.ms-excel", { L".xls" } }, { "application/vnd.ms-excel.sheet.macroEnabled.12", { L".xlsm" } },
    { "application/vnd.oasis.opendocument.spreadsheet", { L".ods" } }, { "text/csv", { L".csv" } },
    { "application/vnd.openxmlformats-officedocument.presentationml.presentation", { L".pptx" } },
    { "application/vnd.ms-powerpoint", { L".ppt" } },
    { "application/vnd.ms-powerpoint.presentation.macroEnabled.12", { L".pptm" } },
    { "application/vnd.oasis.opendocument.presentation", { L".odp" } },
    { "text/html", { L".htm", L".html" } },
};
#define PROGID_PREFIX L"SG.LinuxApp."
#define CLIENT_PREFIX L"SG Linux "

static BOOL has_mime(const struct app *a, const char *mime)
{
    size_t n = strlen(mime);
    const char *p = a->mime;
    while (*p) {
        if (!strncmp(p, mime, n) && (p[n] == ';' || !p[n])) return TRUE;
        if (!(p = strchr(p, ';'))) break;
        p++;
    }
    return FALSE;
}

static void set_sz(HKEY root, const WCHAR *sub, const WCHAR *name, const WCHAR *value)
{
    HKEY k;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)value, (lstrlenW(value) + 1) * sizeof(WCHAR));
    RegCloseKey(k);
}

static void set_none(HKEY root, const WCHAR *sub, const WCHAR *name)
{
    HKEY k;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    RegSetValueExW(k, name, 0, REG_NONE, NULL, 0);
    RegCloseKey(k);
}

/* a ProgID for an app that opens files or links; FALSE: it opens none of
 * the types Default apps chooses for */
static BOOL register_app(const struct app *a, const WCHAR *self, const WCHAR *desktop, const WCHAR *ico, WCHAR *progid, int len)
{
    WCHAR sub[300], cmd[MAX_PATH * 3], id[128];
    BOOL any = FALSE, web = has_mime(a, "x-scheme-handler/http") || has_mime(a, "x-scheme-handler/https"),
         mail = has_mime(a, "x-scheme-handler/mailto");
    int i, e;

    for (i = 0; i < (int)ARRAYSIZE(MIME_EXTS) && !any; i++) any = has_mime(a, MIME_EXTS[i].mime);
    if (!any && !web && !mail) return FALSE;
    MultiByteToWideChar(CP_UTF8, 0, a->id, -1, id, ARRAYSIZE(id));
    _snwprintf(progid, len, PROGID_PREFIX L"%ls", id);
    progid[len - 1] = 0;
    _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls", progid);
    set_sz(HKEY_CURRENT_USER, sub, NULL, a->name);
    set_sz(HKEY_CURRENT_USER, sub, L"FriendlyTypeName", a->name);
    {
        WCHAR desk[140];
        _snwprintf(desk, ARRAYSIZE(desk), L"%ls.desktop", id);
        set_sz(HKEY_CURRENT_USER, sub, L"LinuxDesktopId", desk);   /* Settings tells the Linux side too */
    }
    if (ico) {
        WCHAR di[MAX_PATH + 8];
        _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls\\DefaultIcon", progid);
        _snwprintf(di, ARRAYSIZE(di), L"%ls,0", ico);
        set_sz(HKEY_CURRENT_USER, sub, NULL, di);
    }
    _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls\\shell\\open\\command", progid);
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" --open \"%ls\" \"%%1\"", self, desktop);
    set_sz(HKEY_CURRENT_USER, sub, NULL, cmd);
    for (i = 0; i < (int)ARRAYSIZE(MIME_EXTS); i++) {
        if (!has_mime(a, MIME_EXTS[i].mime)) continue;
        for (e = 0; e < 4 && MIME_EXTS[i].exts[e]; e++) {
            _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls\\OpenWithProgids", MIME_EXTS[i].exts[e]);
            set_none(HKEY_CURRENT_USER, sub, progid);
        }
    }
    if (web || mail) {
        WCHAR base[200];
        _snwprintf(base, ARRAYSIZE(base), L"Software\\Clients\\%ls\\" CLIENT_PREFIX L"%ls",
                   web ? L"StartMenuInternet" : L"Mail", id);
        set_sz(HKEY_CURRENT_USER, base, NULL, a->name);
        _snwprintf(sub, ARRAYSIZE(sub), L"%ls\\Capabilities", base);
        set_sz(HKEY_CURRENT_USER, sub, L"ApplicationName", a->name);
        _snwprintf(sub, ARRAYSIZE(sub), L"%ls\\Capabilities\\URLAssociations", base);
        if (web) { set_sz(HKEY_CURRENT_USER, sub, L"http", progid); set_sz(HKEY_CURRENT_USER, sub, L"https", progid); }
        if (mail) set_sz(HKEY_CURRENT_USER, sub, L"mailto", progid);
    }
    return TRUE;
}

static BOOL made_progid(WCHAR (*made)[128], int n, const WCHAR *progid)
{
    int i;
    for (i = 0; i < n; i++) if (!lstrcmpiW(made[i], progid)) return TRUE;
    return FALSE;
}

/* the ProgIDs and clients of apps that are gone */
static void unregister_gone(WCHAR (*made)[128], int nmade)
{
    static const WCHAR *clients[] = { L"Software\\Clients\\StartMenuInternet", L"Software\\Clients\\Mail" };
    WCHAR name[260], sub[400];
    DWORD n, i;
    HKEY k;
    int c, m, e;

    if (!RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, KEY_READ, &k)) {
        WCHAR (*gone)[128] = calloc(MAX_APPS, sizeof(*gone));
        int ngone = 0;
        for (i = 0; gone && (n = ARRAYSIZE(name), !RegEnumKeyExW(k, i, name, &n, NULL, NULL, NULL, NULL)); i++)
            if (!_wcsnicmp(name, PROGID_PREFIX, lstrlenW(PROGID_PREFIX)) && !made_progid(made, nmade, name) && ngone < MAX_APPS)
                lstrcpynW(gone[ngone++], name, 128);
        RegCloseKey(k);
        for (c = 0; c < ngone; c++) {
            _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls", gone[c]);
            RegDeleteTreeW(HKEY_CURRENT_USER, sub);
            RegDeleteKeyW(HKEY_CURRENT_USER, sub);
            for (m = 0; m < (int)ARRAYSIZE(MIME_EXTS); m++)
                for (e = 0; e < 4 && MIME_EXTS[m].exts[e]; e++) {
                    HKEY ow;
                    _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls\\OpenWithProgids", MIME_EXTS[m].exts[e]);
                    if (!RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_SET_VALUE, &ow)) { RegDeleteValueW(ow, gone[c]); RegCloseKey(ow); }
                }
        }
        free(gone);
    }
    for (c = 0; c < 2; c++) {
        WCHAR gone[32][260];
        int ng = 0;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, clients[c], 0, KEY_READ, &k)) continue;
        for (i = 0; n = ARRAYSIZE(name), !RegEnumKeyExW(k, i, name, &n, NULL, NULL, NULL, NULL); i++) {
            WCHAR progid[200];
            if (_wcsnicmp(name, CLIENT_PREFIX, lstrlenW(CLIENT_PREFIX))) continue;
            _snwprintf(progid, ARRAYSIZE(progid), PROGID_PREFIX L"%ls", name + lstrlenW(CLIENT_PREFIX));
            if (!made_progid(made, nmade, progid) && ng < 32) lstrcpynW(gone[ng++], name, 260);
        }
        RegCloseKey(k);
        for (m = 0; m < ng; m++) {
            _snwprintf(sub, ARRAYSIZE(sub), L"%ls\\%ls", clients[c], gone[m]);
            RegDeleteTreeW(HKEY_CURRENT_USER, sub);
            RegDeleteKeyW(HKEY_CURRENT_USER, sub);
        }
    }
}

static void sync_default_browser(void);

static int sync_apps(void)
{
    WCHAR programs[MAX_PATH], common[MAX_PATH], folder[MAX_PATH], icons[MAX_PATH], self[MAX_PATH], pattern[MAX_PATH];
    WCHAR (*made)[MAX_PATH], (*progids)[128];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int i, j, nmade = 0, nprogids = 0;

    if (!SHGetSpecialFolderPathW(NULL, programs, CSIDL_PROGRAMS, TRUE)) return 1;
    if (!SHGetSpecialFolderPathW(NULL, icons, CSIDL_LOCAL_APPDATA, TRUE)) return 1;
    if (!SHGetSpecialFolderPathW(NULL, common, CSIDL_COMMON_PROGRAMS, FALSE)) common[0] = 0;
    _snwprintf(folder, MAX_PATH, L"%ls\\" FOLDER_NAME, programs);
    lstrcatW(icons, L"\\Stained Glass");
    CreateDirectoryW(icons, NULL);
    lstrcatW(icons, L"\\Linux app icons");
    CreateDirectoryW(icons, NULL);
    GetModuleFileNameW(NULL, self, MAX_PATH);

    load_dep_hidden();
    scan_apps();
    if (!(made = calloc(g_napps + 1, sizeof(*made)))) return 1;
    if (!(progids = calloc(g_napps + 1, sizeof(*progids)))) { free(made); return 1; }
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
        if (has_icon) icon_aliases(&g_apps[i], icons, ico);
        write_link(lnk, self, args, has_icon ? ico : NULL, g_apps[i].comment);
        lstrcpynW(made[nmade++], lnk, MAX_PATH);
#ifndef SG_MUTANT_NO_PROGIDS
        if (register_app(&g_apps[i], self, file, has_icon ? ico : NULL, progids[nprogids], 128)) nprogids++;
#endif
    }
    unregister_gone(progids, nprogids);
    free(progids);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
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
    sync_default_browser();
    return 0;
}

/* ---- the default browser, both sides ------------------------------------ */

/* David 2026-10-03: Linux Firefox chosen at installation was not the default
 * browser in Settings > Default apps, and Firefox's own "make default"
 * (xdg-settings, the user's mimeapps.list) did not reach it either. At each
 * sync, and whenever the user's mimeapps.list changes:
 *  - a default browser newly chosen on the Linux side that is one of these
 *    Linux apps becomes the Windows side's too;
 *  - with no default chosen and one browser installed (Windows or Linux),
 *    that browser is the default, on both sides.
 * The Linux side's last seen choice is kept (HKCU\Software\Stained Glass\
 * Default browser, LinuxSeen); the first time it is only noted, so an
 * earlier choice made in Settings is not undone. */
#define DEFBROWSER_KEY L"Software\\Stained Glass\\Default browser"

static BOOL progid_opens(const WCHAR *progid)
{
    WCHAR sub[300];
    HKEY k;
    _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls\\shell\\open\\command", progid);
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &k)) { RegCloseKey(k); return TRUE; }
    _snwprintf(sub, ARRAYSIZE(sub), L"%ls\\shell\\open\\command", progid);
    if (!RegOpenKeyExW(HKEY_CLASSES_ROOT, sub, 0, KEY_READ, &k)) { RegCloseKey(k); return TRUE; }
    return FALSE;
}

/* the installed browsers' ProgIDs (Software\Clients\StartMenuInternet, HKCU and HKLM) */
static int installed_browsers(WCHAR (*out)[128], int max)
{
    HKEY roots[2] = { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }, key;
    WCHAR name[200], assoc[300], progid[128];
    DWORD i, n, cb;
    int r, c = 0, j;
    for (r = 0; r < 2; r++) {
        if (RegOpenKeyExW(roots[r], L"Software\\Clients\\StartMenuInternet", 0, KEY_READ, &key)) continue;
        for (i = 0; n = ARRAYSIZE(name), !RegEnumKeyExW(key, i, name, &n, NULL, NULL, NULL, NULL); i++) {
            BOOL dup = FALSE;
            if (!_wcsicmp(name, L"IEXPLORE.EXE")) continue;     /* Wine's own, as sg-browser counts them */
            cb = sizeof(progid);
            _snwprintf(assoc, ARRAYSIZE(assoc), L"%ls\\Capabilities\\URLAssociations", name);
            if (RegGetValueW(key, assoc, L"http", RRF_RT_REG_SZ, NULL, progid, &cb) || !progid[0] || !progid_opens(progid)) continue;
            for (j = 0; j < c; j++) dup |= !lstrcmpiW(out[j], progid);
            if (!dup && c < max) lstrcpynW(out[c++], progid, 128);
        }
        RegCloseKey(key);
    }
    return c;
}

/* the user's default browser on the Linux side: mimeapps.list's x-scheme-handler/http */
static BOOL linux_default_browser(WCHAR *desk, int cch)
{
    char cfg[MAX_PATH] = "", path[MAX_PATH], *buf, *l, *e;
    WCHAR w[MAX_PATH];
    BOOL in_defaults = FALSE, found = FALSE;
    desk[0] = 0;
    if (!env_a("XDG_CONFIG_HOME", cfg, sizeof(cfg)) || !cfg[0]) {
        char home[MAX_PATH] = "";
        if (!home_unix(home, sizeof(home)) || !home[0]) return FALSE;
        _snprintf(cfg, sizeof(cfg), "%s/.config", home);
    }
    if (_snprintf(path, sizeof(path), "%s/mimeapps.list", cfg) < 0 || !dos_path(path, w, MAX_PATH)) return FALSE;
    if (!(buf = read_file(w, 1 << 20, NULL))) return FALSE;
    for (l = buf; l && *l && !found; l = e ? e + 1 : NULL) {
        if ((e = strchr(l, '\n'))) *e = 0;
        if (l[0] == '[') { in_defaults = !strncmp(l, "[Default Applications]", 22); continue; }
        if (in_defaults && !strncmp(l, "x-scheme-handler/http=", 22)) {
            char *v = l + 22, *semi = strchr(v, ';');
            if (semi) *semi = 0;
            while (*v == ' ') v++;
            if (*v) { MultiByteToWideChar(CP_UTF8, 0, v, -1, desk, cch); desk[cch - 1] = 0; found = TRUE; }
        }
    }
    free(buf);
    return found;
}

static void copy_tree(HKEY from, HKEY to)
{
    DWORD i, n, cb, type;
    WCHAR name[256];
    BYTE data[4096];
    for (i = 0; n = ARRAYSIZE(name), cb = sizeof(data), !RegEnumValueW(from, i, name, &n, NULL, &type, data, &cb); i++)
        RegSetValueExW(to, name, 0, type, data, cb);
    for (i = 0; n = ARRAYSIZE(name), !RegEnumKeyExW(from, i, name, &n, NULL, NULL, NULL, NULL); i++) {
        HKEY a, b;
        if (RegOpenKeyExW(from, name, 0, KEY_READ, &a)) continue;
        if (!RegCreateKeyExW(to, name, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &b, NULL)) { copy_tree(a, b); RegCloseKey(b); }
        RegCloseKey(a);
    }
}

/* the browser for the Windows side, as Settings > Default apps makes it
 * (set_apps.c's set_default): .htm/.html, http and https, UserChoice */
static void set_windows_browser(const WCHAR *progid)
{
    static const WCHAR *const exts[] = { L".htm", L".html" }, *const protos[] = { L"http", L"https" };
    WCHAR sub[300];
    HKEY from, to;
    int i;
    for (i = 0; i < 2; i++) {
        _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls", exts[i]);
        set_sz(HKEY_CURRENT_USER, sub, NULL, progid);
    }
    for (i = 0; i < 2; i++) {
        _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls", protos[i]);
        RegDeleteTreeW(HKEY_CURRENT_USER, sub);
        _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls", progid);
        if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &from) && RegOpenKeyExW(HKEY_CLASSES_ROOT, progid, 0, KEY_READ, &from))
            continue;
        _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Classes\\%ls", protos[i]);
        if (!RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &to, NULL)) {
            copy_tree(from, to);
            RegSetValueExW(to, L"URL Protocol", 0, REG_SZ, (const BYTE *)L"", sizeof(WCHAR));
            RegCloseKey(to);
        }
        RegCloseKey(from);
    }
    set_sz(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\http\\UserChoice", L"ProgId", progid);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

/* the Linux side's, for a Linux app (xdg-mime, as Settings does) */
static void set_linux_browser(const WCHAR *desk)
{
    LONG (WINAPI *spawnvp)(char * const argv[], int wait) =
        (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__wine_unix_spawnvp");
    char d[160], tool[] = "/usr/bin/xdg-mime", verb[] = "default", m1[] = "x-scheme-handler/http",
         m2[] = "x-scheme-handler/https", m3[] = "text/html", *argv[] = { tool, verb, d, m1, m2, m3, NULL };
    char cfg[MAX_PATH] = "", home[MAX_PATH] = "";
    WCHAR wcfg[MAX_PATH];
    if (!spawnvp) return;
    /* xdg-mime loses the first type when the folder is not there yet */
    if ((env_a("XDG_CONFIG_HOME", cfg, sizeof(cfg)) && cfg[0]) ||
        (home_unix(home, sizeof(home)) && home[0] && _snprintf(cfg, sizeof(cfg), "%s/.config", home) > 0))
        if (dos_path(cfg, wcfg, MAX_PATH)) CreateDirectoryW(wcfg, NULL);
    WideCharToMultiByte(CP_UTF8, 0, desk, -1, d, sizeof(d), NULL, NULL);
    spawnvp(argv, TRUE);
}

static void sync_default_browser(void)
{
    WCHAR win[128] = L"", desk[160], seen[160] = L"", lprogid[200] = L"", browsers[16][128];
    DWORD cb = sizeof(win);
    BOOL have_seen, have_linux, linux_app = FALSE;
    int n;

#ifdef SG_MUTANT_NO_DEFAULT_BROWSER_SYNC
    return;
#endif
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\http\\UserChoice",
                     L"ProgId", RRF_RT_REG_SZ, NULL, win, &cb) || !progid_opens(win)
        || !lstrcmpiW(win, L"http"))    /* Wine's default: the protocol itself, no browser chosen */
        win[0] = 0;
    cb = sizeof(seen);
    have_seen = !RegGetValueW(HKEY_CURRENT_USER, DEFBROWSER_KEY, L"LinuxSeen", RRF_RT_REG_SZ, NULL, seen, &cb);
    have_linux = linux_default_browser(desk, ARRAYSIZE(desk));
    if (have_linux) {
        WCHAR id[160], *dot;
        lstrcpynW(id, desk, ARRAYSIZE(id));
        if ((dot = wcsstr(id, L".desktop")) && !dot[8]) *dot = 0;
        _snwprintf(lprogid, ARRAYSIZE(lprogid), PROGID_PREFIX L"%ls", id);
        linux_app = progid_opens(lprogid);
    }
    if (have_linux && linux_app && lstrcmpiW(win, lprogid) && ((have_seen && lstrcmpiW(desk, seen)) || !win[0])) {
        /* chosen on the Linux side (Firefox's "make default", xdg-settings) */
        set_windows_browser(lprogid);
    } else if (!win[0] && (n = installed_browsers(browsers, 16)) == 1) {
        /* one browser: the default, both sides */
        set_windows_browser(browsers[0]);
        if (!_wcsnicmp(browsers[0], PROGID_PREFIX, lstrlenW(PROGID_PREFIX))) {
            WCHAR d[160];
            _snwprintf(d, ARRAYSIZE(d), L"%ls.desktop", browsers[0] + lstrlenW(PROGID_PREFIX));
            set_linux_browser(d);
        }
        linux_default_browser(desk, ARRAYSIZE(desk));
        have_linux = desk[0] != 0;
    }
    set_sz(HKEY_CURRENT_USER, DEFBROWSER_KEY, L"LinuxSeen", have_linux ? desk : L"");
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

/* an app chosen in Default apps, opening a file (a Windows path: given as
 * the Unix one) or a URL */
static int open_with_app(const WCHAR *desktop, const WCHAR *arg)
{
    LONG (WINAPI *spawnvp)(char * const argv[], int wait);
    char *unix_desktop = NULL, *unix_arg = NULL, gio[] = "/usr/bin/gio", launch[] = "launch", url[4096];
    char *argv[5];
    LONG r = 1;
    BOOL is_path = arg[0] && (arg[1] == L':' || (arg[0] == L'\\' && arg[1] == L'\\'));

    spawnvp = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__wine_unix_spawnvp");
    if (p_unix_name) unix_desktop = p_unix_name(desktop);
    if (is_path && p_unix_name) unix_arg = p_unix_name(arg);
    else WideCharToMultiByte(CP_UTF8, 0, arg, -1, url, sizeof(url), NULL, NULL);
    if (spawnvp && unix_desktop) {
        argv[0] = gio; argv[1] = launch; argv[2] = unix_desktop;
        argv[3] = arg[0] ? (unix_arg ? unix_arg : url) : NULL;
        argv[4] = NULL;
        r = spawnvp(argv, FALSE);
    }
    if (unix_desktop) HeapFree(GetProcessHeap(), 0, unix_desktop);
    if (unix_arg) HeapFree(GetProcessHeap(), 0, unix_arg);
    if (r) {
        WCHAR msg[MAX_PATH * 2 + 64];
        _snwprintf(msg, ARRAYSIZE(msg), L"The Linux app could not open:\n%ls", arg);
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

        int cfg_ev = -1;
        char cfg[MAX_PATH] = "", home[MAX_PATH] = "";
        WCHAR wcfg[MAX_PATH];

        sync_apps();
        for (i = 0; i < nd; i++) {
            char dir[MAX_PATH];
            WCHAR w[MAX_PATH];
            HANDLE e;
            if (_snprintf(dir, sizeof(dir), "%s/applications", dirs[i]) < 0 || !dos_path(dir, w, MAX_PATH)) continue;
            e = FindFirstChangeNotificationW(w, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE);
            if (e != INVALID_HANDLE_VALUE) ev[nev++] = e;
        }
        /* the user's config folder, where mimeapps.list is: the default browser only */
        if ((env_a("XDG_CONFIG_HOME", cfg, sizeof(cfg)) && cfg[0]) ||
            (home_unix(home, sizeof(home)) && home[0] && _snprintf(cfg, sizeof(cfg), "%s/.config", home) > 0)) {
            HANDLE e;
            if (nev < 16 && dos_path(cfg, wcfg, MAX_PATH) &&
                (e = FindFirstChangeNotificationW(wcfg, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE)) != INVALID_HANDLE_VALUE)
                { cfg_ev = nev; ev[nev++] = e; }
        }
        /* a change, or every ten minutes (a folder made since: Flatpak's first app) */
        for (;;) {
            r = nev ? WaitForMultipleObjects(nev, ev, FALSE, 600000) : (Sleep(600000), WAIT_TIMEOUT);
            if (cfg_ev < 0 || r != WAIT_OBJECT_0 + (DWORD)cfg_ev) break;
            Sleep(500);
            sync_default_browser();
            FindNextChangeNotification(ev[cfg_ev]);
        }
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
    else if (argv && argc >= 3 && !lstrcmpiW(argv[1], L"--open")) ret = open_with_app(argv[2], argc >= 4 ? argv[3] : L"");
    else if (argv && argc >= 2 && !lstrcmpiW(argv[1], L"--sync")) ret = sync_apps();
    else if (argv && argc >= 2 && !lstrcmpiW(argv[1], L"--watch")) ret = watch_apps();
    CoUninitialize();
    return ret;
}
