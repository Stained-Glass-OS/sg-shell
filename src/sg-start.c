/* sg-start: the Stained Glass OS Start menu.
 *
 * A Windows 10-class Start menu, a surface Wine's explorer does not provide
 * (ADR 0007). It runs alongside the session with a hidden listener window;
 * explorer posts SG_START_TOGGLE to "SgStartPanel" when the Start button or
 * the Windows key is pressed (wine-sg patches 0012 and 0071).
 *
 * The panel, our own design in the project palette:
 *   - a rail down the left: the menu button (expands the rail with labels),
 *     and at the bottom the signed-in user, Documents, Pictures, Settings and
 *     Power (Lock, Sign out, Sleep and Hibernate when logind allows them,
 *     Restart, Shut down, as policy allows);
 *   - every app, alphabetically with letter headers and a "Recently added"
 *     group, read from the user's and the common Start Menu folders
 *     (recursively, .lnk and .url), each with its own icon;
 *   - pinned tiles on the right (HKCU\Software\Stained Glass\Start\Pinned).
 * Typing searches apps and settings (Enter runs the best match, Escape
 * clears, then closes). Right-click: Pin/Unpin, Run as administrator (the
 * runas verb, which wine-sg 0022 hands to the elevation broker), Open file
 * location, Uninstall. Arrows, Tab and Enter work; clicking away closes it.
 *
 * SG_START_DUMP=<file> makes it write what it shows after every paint, for
 * the gate (test/start-check.sh).
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 */
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#define SG_START_TOGGLE (WM_USER + 10)

/* sizes at 96 DPI; S() scales them */
#define RAIL_W      48
#define RAIL_OPEN_W 232
#define LIST_W      320
#define GAP         16
#define TILE        100
#define TILE_GAP    4
#define TILE_COLS   3
#define PANEL_W     (RAIL_W + LIST_W + GAP + TILE_COLS * TILE + (TILE_COLS - 1) * TILE_GAP + GAP)
#define PANEL_H     640
#define ROW_H       36
#define BEST_H      56
#define TOP_PAD     8
#define SEARCH_H    32
#define TASKBAR_H   40

#define COL_PANEL    RGB(0x1F, 0x1F, 0x1F)
#define COL_RAIL     RGB(0x17, 0x17, 0x17)
#define COL_RAIL_OPEN RGB(0x26, 0x26, 0x26)
#define COL_HOVER    RGB(0x33, 0x33, 0x33)
#define COL_PRESS    RGB(0x44, 0x44, 0x44)
#define COL_TEXT     RGB(0xFF, 0xFF, 0xFF)
#define COL_SUBTLE   RGB(0xA8, 0xA8, 0xA8)
#define COL_ACCENT   RGB(0x7B, 0x2F, 0xBE)
#define COL_ACCENT_HI RGB(0x8A, 0x3F, 0xCE)
#define COL_ACCENT_BR RGB(0x8A, 0x2B, 0xE2)
#define COL_FIELD    RGB(0x2B, 0x2B, 0x2B)
#define COL_DIM      RGB(0x55, 0x55, 0x55)
#define COL_SCROLL   RGB(0x5A, 0x5A, 0x5A)

static const WCHAR LISTENER_CLASS[] = L"SgStartPanel";
static const WCHAR PANEL_CLASS[]    = L"SgStartWindow";
static const WCHAR START_KEY[]      = L"Software\\Stained Glass\\Start";

static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* ---- what the menu offers ---------------------------------------------- */

enum kind { K_APP, K_SETTING };

struct entry {
    WCHAR name[128];
    WCHAR path[MAX_PATH];   /* .lnk, .url or program */
    WCHAR args[128];
    WCHAR keywords[160];    /* settings: more words to find it by */
    enum kind kind;
    FILETIME created;
    HICON icon;             /* 32 px, cached by path */
    BOOL recent;
};

static struct entry *g_apps;
static int g_napps, g_capapps;

/* Settings and Control Panel pages, found by search as Windows' settings are */
static struct entry g_settings[] = {
    { .name = L"Settings", .path = L"ms-settings:", .args = L"", .keywords = L"settings system preferences options", .kind = K_SETTING },
    { .name = L"Display settings", .path = L"ms-settings:display", .args = L"", .keywords = L"screen resolution monitor scale night light", .kind = K_SETTING },
    { .name = L"Sound settings", .path = L"ms-settings:sound", .args = L"", .keywords = L"audio volume speakers microphone output input", .kind = K_SETTING },
    { .name = L"Bluetooth and other devices", .path = L"ms-settings:bluetooth", .args = L"", .keywords = L"bluetooth devices pair", .kind = K_SETTING },
    { .name = L"Default apps", .path = L"ms-settings:defaultapps", .args = L"", .keywords = L"default programs browser file types", .kind = K_SETTING },
    { .name = L"Microphone privacy settings", .path = L"ms-settings:privacy-microphone", .args = L"", .keywords = L"microphone privacy access", .kind = K_SETTING },
    { .name = L"Power & sleep settings", .path = L"ms-settings:powersleep", .args = L"", .keywords = L"power sleep screen timeout", .kind = K_SETTING },
    { .name = L"About your PC", .path = L"ms-settings:about", .args = L"", .keywords = L"about pc name rename specifications version", .kind = K_SETTING },
    { .name = L"Control Panel", .path = L"control.exe", .args = L"", .keywords = L"settings control panel", .kind = K_SETTING },
    { .name = L"Apps & features", .path = L"ms-settings:appsfeatures", .args = L"", .keywords = L"programs uninstall remove install add apps features", .kind = K_SETTING },
    { .name = L"System", .path = L"control.exe", .args = L"/name Microsoft.System", .keywords = L"about pc computer name rename domain join edition", .kind = K_SETTING },
    { .name = L"Network Connections", .path = L"control.exe", .args = L"ncpa.cpl", .keywords = L"network adapter ethernet wifi ip address dhcp static dns", .kind = K_SETTING },
    { .name = L"Network and Sharing Center", .path = L"control.exe", .args = L"/name Microsoft.NetworkAndSharingCenter", .keywords = L"network internet sharing status", .kind = K_SETTING },
    { .name = L"Date and time", .path = L"ms-settings:dateandtime", .args = L"", .keywords = L"clock time zone date", .kind = K_SETTING },
    { .name = L"User Accounts", .path = L"control.exe", .args = L"userpasswords", .keywords = L"users account password administrator family", .kind = K_SETTING },
    { .name = L"Personalization", .path = L"control.exe", .args = L"/name Microsoft.Personalization", .keywords = L"background wallpaper colors colour theme dark light accent", .kind = K_SETTING },
    { .name = L"Windows Update", .path = L"ms-settings:windowsupdate", .args = L"", .keywords = L"update updates upgrade", .kind = K_SETTING },
    { .name = L"Display", .path = L"control.exe", .args = L"desk.cpl", .keywords = L"screen resolution monitor scale", .kind = K_SETTING },
    { .name = L"Internet Options", .path = L"control.exe", .args = L"inetcpl.cpl", .keywords = L"internet proxy browser", .kind = K_SETTING },
};
#define NSETTINGS ((int)ARRAYSIZE(g_settings))

/* index space: apps 0..napps-1, settings 1000+i */
#define SETTING(i) (1000 + (i))
static struct entry *entry_of(int id)
{
    if (id >= 1000) return (id - 1000 < NSETTINGS) ? &g_settings[id - 1000] : NULL;
    return (id >= 0 && id < g_napps) ? &g_apps[id] : NULL;
}

/* icon cache, across rebuilds of the list */
struct icon_cache { WCHAR path[MAX_PATH]; HICON icon; };
static struct icon_cache *g_icons;
static int g_nicons, g_capicons;

/* A program's own icon, as the Start menu shows it: a shortcut's icon
 * location, or its target's; an executable's first icon (ExtractIconEx --
 * Wine's SHGetFileInfo gives every program the generic one); else what the
 * shell shows for the file. */
static HICON own_icon(const WCHAR *path)
{
    WCHAR target[MAX_PATH], full[MAX_PATH], *ext;
    SHFILEINFOW sfi;
    HICON big = NULL;
    int index = 0;

    lstrcpynW(target, path, MAX_PATH);
    if ((ext = wcsrchr(path, '.')) && !lstrcmpiW(ext, L".lnk"))
    {
        IShellLinkW *link;
        IPersistFile *file;
        if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link)))
        {
            if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file)))
            {
                if (SUCCEEDED(IPersistFile_Load(file, path, STGM_READ)))
                {
                    WCHAR icon[MAX_PATH] = L"";
                    if (SUCCEEDED(IShellLinkW_GetIconLocation(link, icon, MAX_PATH, &index)) && icon[0])
                    {
                        ExpandEnvironmentStringsW(icon, full, MAX_PATH);
                        if (ExtractIconExW(full, index, &big, NULL, 1) == 1 && big) { IPersistFile_Release(file); IShellLinkW_Release(link); return big; }
                    }
                    if (FAILED(IShellLinkW_GetPath(link, target, MAX_PATH, NULL, 0)) || !target[0]) lstrcpynW(target, path, MAX_PATH);
                }
                IPersistFile_Release(file);
            }
            IShellLinkW_Release(link);
        }
    }
    lstrcpynW(full, target, MAX_PATH);
    if (!wcschr(target, '\\') && !SearchPathW(NULL, target, NULL, MAX_PATH, full, NULL)) lstrcpynW(full, target, MAX_PATH);
    if ((ext = wcsrchr(full, '.')) && (!lstrcmpiW(ext, L".exe") || !lstrcmpiW(ext, L".dll")))
    {
        if (ExtractIconExW(full, 0, &big, NULL, 1) == 1 && big) return big;
        /* no icon of its own: the shell's program icon (Wine's
         * SHGetStockIconInfo answers S_OK with no icon) */
        memset(&sfi, 0, sizeof(sfi));
        SHGetFileInfoW(L"program.exe", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                       SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES);
        if (sfi.hIcon) return sfi.hIcon;
    }
    memset(&sfi, 0, sizeof(sfi));
    SHGetFileInfoW(full, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON);
    return sfi.hIcon;
}

static HICON icon_for(const WCHAR *path)
{
    HICON icon;
    int i;

    for (i = 0; i < g_nicons; i++)
        if (!lstrcmpiW(g_icons[i].path, path)) return g_icons[i].icon;
    icon = own_icon(path);
    if (g_nicons == g_capicons)
    {
        int cap = g_capicons ? g_capicons * 2 : 128;
        struct icon_cache *n = realloc(g_icons, cap * sizeof(*n));
        if (!n) return icon;
        g_icons = n; g_capicons = cap;
    }
    lstrcpynW(g_icons[g_nicons].path, path, MAX_PATH);
    g_icons[g_nicons].icon = icon;
    g_nicons++;
    return icon;
}

/* the shell's folder icon: File Explorer's, when its program carries none */
static HICON folder_icon(void)
{
    static HICON icon;
    WCHAR dir[MAX_PATH];
    SHFILEINFOW sfi = { 0 };
    if (!icon && GetWindowsDirectoryW(dir, MAX_PATH) &&
        SHGetFileInfoW(dir, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON)) icon = sfi.hIcon;
    return icon;
}

static int app_cmp(const void *a, const void *b)
{
    return lstrcmpiW(((const struct entry *)a)->name, ((const struct entry *)b)->name);
}

static void add_app(const WCHAR *name, const WCHAR *path, const FILETIME *created)
{
    int i;
    for (i = 0; i < g_napps; i++)
        if (!lstrcmpiW(g_apps[i].name, name)) return;   /* one per name: the user's wins */
    if (g_napps == g_capapps)
    {
        int cap = g_capapps ? g_capapps * 2 : 128;
        struct entry *n = realloc(g_apps, cap * sizeof(*n));
        if (!n) return;
        g_apps = n; g_capapps = cap;
    }
    memset(&g_apps[g_napps], 0, sizeof(*g_apps));
    lstrcpynW(g_apps[g_napps].name, name, 128);
    lstrcpynW(g_apps[g_napps].path, path, MAX_PATH);
    if (created) g_apps[g_napps].created = *created;
    g_apps[g_napps].kind = K_APP;
    g_napps++;
}

/* .lnk and .url files under a Programs tree, a few levels deep */
static void scan_dir(const WCHAR *dir, int depth)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (depth > 4 || _snwprintf(pattern, MAX_PATH, L"%s\\*", dir) < 0) return;
    if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        WCHAR full[MAX_PATH], name[128], *ext;
        if (fd.cFileName[0] == '.') continue;
        if (_snwprintf(full, MAX_PATH, L"%s\\%s", dir, fd.cFileName) < 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { scan_dir(full, depth + 1); continue; }
        if (!(ext = wcsrchr(fd.cFileName, '.')) || (lstrcmpiW(ext, L".lnk") && lstrcmpiW(ext, L".url"))) continue;
        lstrcpynW(name, fd.cFileName, (int)min(128, ext - fd.cFileName + 1));
        add_app(name, full, &fd.ftCreationTime);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void scan_programs(int csidl)
{
    WCHAR base[MAX_PATH];
    if (SHGetSpecialFolderPathW(NULL, base, csidl, FALSE)) scan_dir(base, 0);
}

/* a program of ours installed beside this one */
static void add_beside(const WCHAR *name, const WCHAR *file)
{
    WCHAR self[MAX_PATH], *slash;
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!n || n >= MAX_PATH || !(slash = wcsrchr(self, '\\'))) return;
    if ((size_t)(slash + 1 - self) + wcslen(file) + 1 > MAX_PATH) return;
    lstrcpyW(slash + 1, file);
    if (GetFileAttributesW(self) != INVALID_FILE_ATTRIBUTES) add_app(name, self, NULL);
}

static ULONGLONG ft64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

/* When this user's Start menu first ran: "Recently added" is what came after */
static ULONGLONG first_run(void)
{
    ULONGLONG t = 0;
    DWORD size = sizeof(t);
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, START_KEY, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &key, NULL)) return 0;
    if (RegQueryValueExW(key, L"FirstRun", NULL, NULL, (BYTE *)&t, &size) || !t)
    {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        t = ft64(now);
        RegSetValueExW(key, L"FirstRun", 0, REG_QWORD, (BYTE *)&t, sizeof(t));
    }
    RegCloseKey(key);
    return t;
}

static int recents[3], g_nrecent;

static BOOL reg_show_recent(void)
{
    DWORD v = 1, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, START_KEY, L"ShowRecentlyAdded", RRF_RT_REG_DWORD, NULL, &v, &size);
    return v != 0;
}

static void build_list(void)
{
    ULONGLONG since = first_run(), now64, week = 7ULL * 24 * 3600 * 10000000ULL;
    FILETIME now;
    int i, j;

    g_napps = 0;
    scan_programs(CSIDL_PROGRAMS);
    scan_programs(CSIDL_COMMON_PROGRAMS);
    add_app(L"Notepad", L"notepad.exe", NULL);
    add_app(L"File Explorer", L"explorer.exe", NULL);
    add_app(L"Command Prompt", L"cmd.exe", NULL);
    add_beside(L"Task Manager", L"sg-taskmgr64.exe");
    add_beside(L"Remote Desktop Connection", L"sg-mstsc64.exe");
    add_beside(L"Control Panel", L"sg-control64.exe");
    add_beside(L"Settings", L"sg-settings64.exe");
    add_beside(L"Terminal", L"sg-terminal64.exe");
    add_beside(L"Alarms & Clock", L"sg-clock64.exe");
    add_beside(L"Media Player", L"sg-media64.exe");
    add_beside(L"Calculator", L"sg-calc64.exe");
    add_beside(L"Photos", L"sg-photos64.exe");
    add_beside(L"Paint", L"sg-paint64.exe");
    add_beside(L"Sticky Notes", L"sg-sticky64.exe");
    add_beside(L"Snipping Tool", L"sg-snip64.exe");
    add_beside(L"Character Map", L"sg-charmap64.exe");
    add_beside(L"PDF Viewer", L"sg-pdf64.exe");
    add_beside(L"Magnifier", L"sg-magnify64.exe");
    add_beside(L"On-Screen Keyboard", L"sg-osk64.exe");
    add_beside(L"Get a web browser", L"sg-browser64.exe");
    qsort(g_apps, g_napps, sizeof(*g_apps), app_cmp);
    for (i = 0; i < g_napps; i++)
    {
        g_apps[i].icon = icon_for(g_apps[i].path);
        if (!lstrcmpiW(g_apps[i].path, L"explorer.exe")) g_apps[i].icon = folder_icon();
    }

    /* the newest three added since the first run, within a week */
    GetSystemTimeAsFileTime(&now);
    now64 = ft64(now);
    g_nrecent = 0;
    for (i = 0; i < g_napps; i++)
    {
        ULONGLONG c = ft64(g_apps[i].created);
        g_apps[i].recent = c > since && now64 - c < week;
        if (!g_apps[i].recent) continue;
        for (j = g_nrecent; j > 0 && ft64(g_apps[recents[j - 1]].created) < c; j--)
            if (j < 3) recents[j] = recents[j - 1];
        if (j < 3) { recents[j] = i; if (g_nrecent < 3) g_nrecent++; }
    }
}

static int find_app(const WCHAR *name)
{
    int i;
    for (i = 0; i < g_napps; i++) if (!lstrcmpiW(g_apps[i].name, name)) return i;
    return -1;
}

/* ---- pinned tiles -------------------------------------------------------- */

#define MAX_PINS 24
static WCHAR g_pins[MAX_PINS][128];
static int g_npins;

static void save_pins(void)
{
    WCHAR buf[MAX_PINS * 129 + 1], *p = buf;
    HKEY key;
    int i;
    for (i = 0; i < g_npins; i++) { lstrcpyW(p, g_pins[i]); p += wcslen(p) + 1; }
    *p++ = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, START_KEY, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL)) return;
    RegSetValueExW(key, L"Pinned", 0, REG_MULTI_SZ, (BYTE *)buf, (DWORD)((p - buf) * sizeof(WCHAR)));
    RegCloseKey(key);
}

static void load_pins(void)
{
    WCHAR buf[MAX_PINS * 129 + 1], *p;
    DWORD size = sizeof(buf) - sizeof(WCHAR), type;
    HKEY key;
    int i;

    g_npins = 0;
    memset(buf, 0, sizeof(buf));
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, START_KEY, 0, KEY_READ, &key))
    {
        if (!RegQueryValueExW(key, L"Pinned", NULL, &type, (BYTE *)buf, &size) && type == REG_MULTI_SZ)
        {
            for (p = buf; *p && g_npins < MAX_PINS; p += wcslen(p) + 1) lstrcpynW(g_pins[g_npins++], p, 128);
            RegCloseKey(key);
            return;
        }
        RegCloseKey(key);
    }
    /* first time: the everyday things */
    {
        static const WCHAR *const defaults[] = { L"File Explorer", L"Control Panel", L"Notepad",
                                                 L"PowerShell", L"Remote Desktop Connection" };
        for (i = 0; i < (int)ARRAYSIZE(defaults); i++)
        {
            int a = find_app(defaults[i]), j;
            if (a < 0 && !lstrcmpW(defaults[i], L"PowerShell"))
                for (j = 0; j < g_napps; j++) if (wcsstr(g_apps[j].name, L"PowerShell")) { a = j; break; }
            if (a >= 0) lstrcpynW(g_pins[g_npins++], g_apps[a].name, 128);
        }
        save_pins();
    }
}

static int pin_index(const WCHAR *name)
{
    int i;
    for (i = 0; i < g_npins; i++) if (!lstrcmpiW(g_pins[i], name)) return i;
    return -1;
}

static void pin(const WCHAR *name, BOOL on)
{
    int i = pin_index(name);
    if (on && i < 0 && g_npins < MAX_PINS) lstrcpynW(g_pins[g_npins++], name, 128);
    else if (!on && i >= 0)
    {
        memmove(g_pins[i], g_pins[i + 1], (g_npins - i - 1) * sizeof(g_pins[0]));
        g_npins--;
    }
    save_pins();
}

/* ---- the rows the list shows ----------------------------------------------- */

enum row_type { R_HEADER, R_ITEM, R_BEST };
struct row { enum row_type type; int id; WCHAR label[64]; int y, h; };

#define MAX_ROWS 2048
static struct row g_rows[MAX_ROWS];
static int g_nrows, g_content_h;
static WCHAR g_search[64];

static void add_row(enum row_type type, int id, const WCHAR *label)
{
    struct row *r;
    if (g_nrows >= MAX_ROWS) return;
    r = &g_rows[g_nrows++];
    r->type = type;
    r->id = id;
    lstrcpynW(r->label, label ? label : L"", 64);
    r->h = S(type == R_BEST ? BEST_H : ROW_H);
    r->y = g_content_h;
    g_content_h += r->h;
}

/* how well a name answers the search: 0 not at all */
static int score(const struct entry *e, const WCHAR *q)
{
    WCHAR name[128], words[320], *hit;
    size_t n = wcslen(q);
    int i;

    if (!n) return 0;
    for (i = 0; e->name[i] && i < 127; i++) name[i] = towlower(e->name[i]);
    name[i] = 0;
    if (!wcsncmp(name, q, n)) return 100;
    for (hit = wcsstr(name, q); hit; hit = wcsstr(hit + 1, q))
        if (hit > name && !iswalnum(hit[-1])) return 80;   /* a word starts with it */
    if (wcsstr(name, q)) return 60;
    _snwprintf(words, ARRAYSIZE(words), L" %s ", e->keywords);
    for (i = 0; words[i]; i++) words[i] = towlower(words[i]);
    for (hit = wcsstr(words, q); hit; hit = wcsstr(hit + 1, q))
        if (!iswalnum(hit[-1])) return 40;
    return 0;
}

struct hit { int id, score; };
static int hit_cmp(const void *a, const void *b)
{
    const struct hit *x = a, *y = b;
    if (x->score != y->score) return y->score - x->score;
    return lstrcmpiW(entry_of(x->id)->name, entry_of(y->id)->name);
}

static void build_rows(void)
{
    int i;
    g_nrows = 0;
    g_content_h = S(TOP_PAD);

    if (g_search[0])
    {
        static struct hit hits[1024];
        WCHAR q[64];
        int n = 0, apps = 0, settings = 0;

        for (i = 0; g_search[i]; i++) q[i] = towlower(g_search[i]);
        q[i] = 0;
        g_content_h += S(SEARCH_H) + S(8);
        for (i = 0; i < g_napps && n < 1000; i++)
            if ((hits[n].score = score(&g_apps[i], q))) { hits[n].id = i; n++; }
        for (i = 0; i < NSETTINGS && n < 1024; i++)
            if ((hits[n].score = score(&g_settings[i], q) - 5) > 0) { hits[n].id = SETTING(i); n++; }
        qsort(hits, n, sizeof(*hits), hit_cmp);
        if (!n) { add_row(R_HEADER, -1, L"No results"); return; }
        add_row(R_HEADER, -1, L"Best match");
        add_row(R_BEST, hits[0].id, NULL);
        for (i = 1; i < n; i++) if (hits[i].id < 1000) apps++; else settings++;
        if (apps)
        {
            add_row(R_HEADER, -1, L"Apps");
            for (i = 1; i < n; i++) if (hits[i].id < 1000) add_row(R_ITEM, hits[i].id, NULL);
        }
        if (settings)
        {
            add_row(R_HEADER, -1, L"Settings");
            for (i = 1; i < n; i++) if (hits[i].id >= 1000) add_row(R_ITEM, hits[i].id, NULL);
        }
        return;
    }

    /* Settings > Personalization > Start: "Show recently added apps" */
    if (g_nrecent && reg_show_recent())
    {
        add_row(R_HEADER, -1, L"Recently added");
        for (i = 0; i < g_nrecent; i++) add_row(R_ITEM, recents[i], NULL);
    }
    {
        WCHAR last = 0;
        for (i = 0; i < g_napps; i++)
        {
            WCHAR c = towupper(g_apps[i].name[0]), label[2];
            if (!iswalpha(c)) c = '#';
            if (c != last)
            {
                label[0] = c; label[1] = 0;
                add_row(R_HEADER, -1, label);
                last = c;
            }
            add_row(R_ITEM, i, NULL);
        }
    }
}

/* ---- state ---------------------------------------------------------------- */

static HWND g_panel;
static HFONT g_font, g_font_small, g_font_bold, g_font_big, g_font_glyph;
static int g_panel_w, g_panel_h;
static int g_scroll;                /* pixels */
static int g_hot_row = -1, g_hot_tile = -1, g_hot_rail = -1;
static int g_sel_row = -1, g_sel_tile = -1;   /* keyboard selection */
static BOOL g_rail_open;
static const WCHAR *g_menu;         /* the popup menu showing, for the gate */
static WCHAR g_menu_items[256];
static const WCHAR *g_dump_path;
static LARGE_INTEGER g_open_start;
static double g_open_ms = -1;
static WCHAR g_launched[160];

enum { RAIL_MENU, RAIL_USER, RAIL_DOCS, RAIL_PICS, RAIL_SETTINGS, RAIL_POWER, RAIL_COUNT };
static const WCHAR *const rail_labels[RAIL_COUNT] = { L"Start", L"", L"Documents", L"Pictures", L"Settings", L"Power" };

static int list_left(void) { return S(RAIL_W); }
static int list_top(void) { return 0; }
static int list_bottom(void) { return g_panel_h; }
static int tiles_left(void) { return S(RAIL_W + LIST_W + GAP); }
static int tiles_top(void) { return S(TOP_PAD) + S(32); }

static RECT rail_rect(int i)
{
    RECT r;
    int w = g_rail_open ? S(RAIL_OPEN_W) : S(RAIL_W);
    if (i == RAIL_MENU) SetRect(&r, 0, S(4), w, S(4) + S(RAIL_W));
    else
    {
        int from_bottom = RAIL_COUNT - i;   /* POWER at the bottom */
        SetRect(&r, 0, g_panel_h - from_bottom * S(RAIL_W) - S(4), w, g_panel_h - (from_bottom - 1) * S(RAIL_W) - S(4));
    }
    return r;
}

static RECT tile_rect(int i)
{
    RECT r;
    int c = i % TILE_COLS, rw = i / TILE_COLS;
    SetRect(&r, tiles_left() + c * S(TILE + TILE_GAP), tiles_top() + rw * S(TILE + TILE_GAP),
            tiles_left() + c * S(TILE + TILE_GAP) + S(TILE), tiles_top() + rw * S(TILE + TILE_GAP) + S(TILE));
    return r;
}

/* tiles whose app is still there */
static int tile_app(int i) { return i < g_npins ? find_app(g_pins[i]) : -1; }

/* ---- the gate's view -------------------------------------------------------- */

/* a line of the dump, in UTF-8 without a byte-order mark */
static void dprint(FILE *f, const WCHAR *fmt, ...)
{
    WCHAR line[512];
    char utf8[1536];
    va_list ap;
    int n;
    va_start(ap, fmt);
    _vsnwprintf(line, ARRAYSIZE(line) - 1, fmt, ap);
    va_end(ap);
    line[ARRAYSIZE(line) - 1] = 0;
    n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), NULL, NULL);
    if (n > 1) fwrite(utf8, 1, n - 1, f);
}

static void dump(void)
{
    FILE *f;
    int i;
    if (!g_dump_path || !(f = _wfopen(g_dump_path, L"wb"))) return;
    {
        RECT wr;
        GetWindowRect(g_panel, &wr);
        dprint(f, L"visible=%d\n", IsWindowVisible(g_panel));
        dprint(f, L"rect=%ld,%ld,%ld,%ld\n", wr.left, wr.top, wr.right, wr.bottom);
    }
    dprint(f, L"search=%ls\n", g_search);
    dprint(f, L"rail_open=%d\n", g_rail_open);
    dprint(f, L"open_ms=%d\n", (int)g_open_ms);
    dprint(f, L"apps=%d\n", g_napps);
    dprint(f, L"menu=%ls\n", g_menu ? g_menu : L"");
    dprint(f, L"menu_items=%ls\n", g_menu ? g_menu_items : L"");
    dprint(f, L"launched=%ls\n", g_launched);
    for (i = 0; i < g_nrows; i++)
    {
        const struct entry *e = entry_of(g_rows[i].id);
        if (g_rows[i].type == R_HEADER) dprint(f, L"header %ls\n", g_rows[i].label);
        else dprint(f, L"%ls %ls%ls\n", g_rows[i].type == R_BEST ? L"best" : L"item", e ? e->name : L"?",
                      e && e->kind == K_APP && !e->icon ? L" (no icon)" : L"");
    }
    for (i = 0; i < g_npins; i++) if (tile_app(i) >= 0) dprint(f, L"tile %ls\n", g_pins[i]);
    if (g_sel_row >= 0 && g_sel_row < g_nrows && entry_of(g_rows[g_sel_row].id))
        dprint(f, L"selected %ls\n", entry_of(g_rows[g_sel_row].id)->name);
    if (g_sel_tile >= 0 && tile_app(g_sel_tile) >= 0) dprint(f, L"selected-tile %ls\n", g_pins[g_sel_tile]);
    fclose(f);
}

/* ---- actions ------------------------------------------------------------------- */

static void show_panel(BOOL show);

static void run_entry(const struct entry *e, const WCHAR *verb)
{
    if (!e) return;
    _snwprintf(g_launched, ARRAYSIZE(g_launched), L"%ls%ls", verb ? L"runas " : L"", e->name);
    show_panel(FALSE);
    ShellExecuteW(NULL, verb, e->path, e->args[0] ? e->args : NULL, NULL, SW_SHOWNORMAL);
}

static void open_location(const struct entry *e)
{
    WCHAR full[MAX_PATH], param[MAX_PATH + 16];
    lstrcpynW(full, e->path, MAX_PATH);
    if (!wcschr(full, '\\')) SearchPathW(NULL, e->path, NULL, MAX_PATH, full, NULL);
    _snwprintf(param, ARRAYSIZE(param), L"/select,\"%ls\"", full);
    show_panel(FALSE);
    ShellExecuteW(NULL, NULL, L"explorer.exe", param, NULL, SW_SHOWNORMAL);
}

/* Machine policy (Group Policy): NoClose removes restart and shut down,
 * StartMenuLogoff removes sign out. SHRestricted reads HKLM first (wine-sg
 * 0025), which a user cannot override. */
static BOOL may_shut_down(void) { return !SHRestricted(REST_NOCLOSE); }
static BOOL may_sign_out(void) { return !SHRestricted(REST_STARTMENULOGOFF); }

/* ---- Sleep and Hibernate --------------------------------------------------------
 * logind decides, through sg-session's sg-settingsctl (SG_SETTINGSCTL names
 * another, the gate's stand-in): `sleep-caps` says whether this session may
 * suspend or hibernate (polkit's "challenge" counts as no: no agent runs to
 * ask), and `sleep suspend|hibernate` locks the session, then asks logind. A
 * Windows program gets no pipe to a native one, so the answer comes back in a
 * file, as Settings' does. The capabilities are asked at start-up and again
 * after every power menu, on a thread, so the menu never waits for them. */
static volatile LONG g_can_suspend, g_can_hibernate, g_caps_busy;

static char *power_ctl(const WCHAR *args, DWORD timeout_ms)
{
    static LONG seq;
    char *(CDECL *to_unix)(const WCHAR *) = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    WCHAR tool[MAX_PATH] = L"/usr/bin/sg-settingsctl", dir[MAX_PATH], dos[MAX_PATH], cmd[1024], *p;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    char *unix_out, *text = NULL;
    DWORD waited = 0;
    HANDLE h;
    GetEnvironmentVariableW(L"SG_SETTINGSCTL", tool, MAX_PATH);
    if (!to_unix || !GetTempPathW(MAX_PATH, dir)) return NULL;
    _snwprintf(dos, MAX_PATH, L"%lssg-start-%lu-%ld.txt", dir, GetCurrentProcessId(), InterlockedIncrement(&seq));
    dos[MAX_PATH - 1] = 0;
    CloseHandle(CreateFileW(dos, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));   /* so it has a Unix name */
    unix_out = to_unix(dos);
    DeleteFileW(dos);
    if (!unix_out) return NULL;
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\\\\?\\unix%ls %ls --out %S", tool, args, unix_out);
    cmd[ARRAYSIZE(cmd) - 1] = 0;
    HeapFree(GetProcessHeap(), 0, unix_out);
    for (p = cmd + 8; *p && *p != L' '; p++) if (*p == L'/') *p = L'\\';
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return NULL;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    while (GetFileAttributesW(dos) == INVALID_FILE_ATTRIBUTES && waited < timeout_ms) { Sleep(50); waited += 50; }
    h = CreateFileW(dos, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD size = GetFileSize(h, NULL), got = 0;
        if (size < 65536 && (text = malloc(size + 1))) { ReadFile(h, text, size, &got, NULL); text[got] = 0; }
        CloseHandle(h);
        DeleteFileW(dos);
    }
    return text;
}

static BOOL cap_yes(const char *text, const char *line)
{
    const char *p = text ? strstr(text, line) : NULL;
    return p && (p[strlen(line)] == '\n' || p[strlen(line)] == '\r');
}

static DWORD WINAPI caps_thread(void *arg)
{
    char *text = power_ctl(L"sleep-caps", 20000);
    (void)arg;
    InterlockedExchange(&g_can_suspend, cap_yes(text, "CAN suspend yes"));
    InterlockedExchange(&g_can_hibernate, cap_yes(text, "CAN hibernate yes"));
    free(text);
    InterlockedExchange(&g_caps_busy, 0);
    return 0;
}

static void refresh_sleep_caps(void)
{
    HANDLE t;
    if (InterlockedCompareExchange(&g_caps_busy, 1, 0)) return;
    if ((t = CreateThread(NULL, 0, caps_thread, NULL, 0, NULL))) CloseHandle(t);
    else InterlockedExchange(&g_caps_busy, 0);
}

static DWORD WINAPI sleep_thread(void *arg)
{
    free(power_ctl(arg, 60000));
    return 0;
}

static void go_to_sleep(BOOL hibernate)
{
    HANDLE t = CreateThread(NULL, 0, sleep_thread, (void *)(hibernate ? L"sleep hibernate" : L"sleep suspend"), 0, NULL);
    if (t) CloseHandle(t);
}

/* The menus are drawn dark, as the Start menu is: owner-drawn items whose
 * labels live here while the menu is up. */
#define MENU_MAX 12
static const WCHAR *g_mlabels[MENU_MAX];
static int g_nmlabels;
#define COL_MENU     RGB(0x2B, 0x2B, 0x2B)
#define COL_MENU_SEL RGB(0x41, 0x41, 0x41)

static HMENU menu_new(void)
{
    MENUINFO mi = { sizeof(mi), MIM_BACKGROUND };
    HMENU m = CreatePopupMenu();
    static HBRUSH bg;
    if (!bg) bg = CreateSolidBrush(COL_MENU);
    mi.hbrBack = bg;
    SetMenuInfo(m, &mi);
    g_nmlabels = 0;
    return m;
}

static void menu_add(HMENU m, UINT id, const WCHAR *label)
{
    if (g_nmlabels >= MENU_MAX) return;
    g_mlabels[g_nmlabels] = label;   /* NULL: a separator */
    AppendMenuW(m, MF_OWNERDRAW | (label ? 0 : MF_DISABLED), id, (LPCWSTR)(ULONG_PTR)(g_nmlabels + 1));
    g_nmlabels++;
}

static const WCHAR *menu_label(ULONG_PTR data) { return data && data <= (ULONG_PTR)g_nmlabels ? g_mlabels[data - 1] : NULL; }

static void menu_measure(MEASUREITEMSTRUCT *mis)
{
    const WCHAR *label = menu_label(mis->itemData);
    HDC dc = GetDC(g_panel);
    RECT r = { 0, 0, 0, 0 };
    if (!label) { mis->itemWidth = S(40); mis->itemHeight = S(9); ReleaseDC(g_panel, dc); return; }
    SelectObject(dc, g_font);
    DrawTextW(dc, label, -1, &r, DT_SINGLELINE | DT_CALCRECT);
    mis->itemWidth = r.right + S(48);
    mis->itemHeight = S(32);
    ReleaseDC(g_panel, dc);
}

static void menu_draw(DRAWITEMSTRUCT *dis)
{
    const WCHAR *label = menu_label(dis->itemData);
    RECT r = dis->rcItem;
    HBRUSH b;
    if (!label)
    {
        RECT line = { r.left + S(10), (r.top + r.bottom) / 2, r.right - S(10), (r.top + r.bottom) / 2 + 1 };
        b = CreateSolidBrush(COL_MENU); FillRect(dis->hDC, &r, b); DeleteObject(b);
        b = CreateSolidBrush(RGB(0x55, 0x55, 0x55)); FillRect(dis->hDC, &line, b); DeleteObject(b);
        return;
    }
    b = CreateSolidBrush((dis->itemState & ODS_SELECTED) ? COL_MENU_SEL : COL_MENU);
    FillRect(dis->hDC, &r, b);
    DeleteObject(b);
    r.left += S(16);
    SetBkMode(dis->hDC, TRANSPARENT);
    SelectObject(dis->hDC, g_font);
    SetTextColor(dis->hDC, (dis->itemState & ODS_GRAYED) ? COL_DIM : COL_TEXT);
    DrawTextW(dis->hDC, label, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT |
              ((dis->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0));
}

/* an owner-drawn menu's mnemonics: the letter after '&' */
static LRESULT menu_char(WCHAR c, HMENU m)
{
    int i, n = GetMenuItemCount(m);
    for (i = 0; i < n; i++)
    {
        MENUITEMINFOW mii = { sizeof(mii), MIIM_DATA };
        const WCHAR *label, *amp;
        if (!GetMenuItemInfoW(m, i, TRUE, &mii) || !(label = menu_label(mii.dwItemData))) continue;
        if ((amp = wcschr(label, '&')) && towlower(amp[1]) == towlower(c)) return MAKELRESULT(i, MNC_EXECUTE);
    }
    return MAKELRESULT(0, MNC_IGNORE);
}

/* a context menu drops from the pointer; the power menu, at the bottom, rises */
static int track(HMENU menu, POINT pt, const WCHAR *what)
{
    UINT align = !lstrcmpW(what, L"power") ? TPM_BOTTOMALIGN : TPM_TOPALIGN;
    int cmd, i;
    g_menu_items[0] = 0;
    for (i = 0; i < g_nmlabels; i++)
    {
        WCHAR item[64], *amp;
        if (!g_mlabels[i]) continue;
        lstrcpynW(item, g_mlabels[i], ARRAYSIZE(item));
        while ((amp = wcschr(item, '&'))) memmove(amp, amp + 1, (wcslen(amp) + 1) * sizeof(WCHAR));
        if (g_menu_items[0]) wcsncat(g_menu_items, L",", ARRAYSIZE(g_menu_items) - wcslen(g_menu_items) - 1);
        wcsncat(g_menu_items, item, ARRAYSIZE(g_menu_items) - wcslen(g_menu_items) - 1);
    }
    g_menu = what;
    dump();
    cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | align, pt.x, pt.y, 0, g_panel, NULL);
    g_menu = NULL;
    return cmd;
}

static void power_menu(POINT pt)
{
    enum { P_LOCK = 1, P_SIGNOUT, P_SLEEP, P_HIBERNATE, P_RESTART, P_SHUTDOWN };
    HMENU m = menu_new();
    int cmd;
    menu_add(m, P_LOCK, L"&Lock");
    if (may_sign_out()) menu_add(m, P_SIGNOUT, L"Sign &out");
    if (may_shut_down())
    {
        menu_add(m, 0, NULL);
        if (g_can_suspend) menu_add(m, P_SLEEP, L"&Sleep");
        if (g_can_hibernate) menu_add(m, P_HIBERNATE, L"&Hibernate");
        menu_add(m, P_RESTART, L"&Restart");
        menu_add(m, P_SHUTDOWN, L"Sh&ut down");
    }
    cmd = track(m, pt, L"power");
    DestroyMenu(m);
    refresh_sleep_caps();   /* for the next time: a lid, a dock, a policy may change them */
    if (!cmd) { dump(); return; }
    show_panel(FALSE);
    switch (cmd)
    {
    case P_LOCK:     LockWorkStation(); break;   /* wine-sg routes it to the compositor */
    case P_SIGNOUT:  if (may_sign_out()) ExitWindowsEx(EWX_LOGOFF, 0); break;
    case P_SLEEP:    if (may_shut_down()) { lstrcpyW(g_launched, L"Sleep"); dump(); go_to_sleep(FALSE); } break;
    case P_HIBERNATE: if (may_shut_down()) { lstrcpyW(g_launched, L"Hibernate"); dump(); go_to_sleep(TRUE); } break;
    case P_RESTART:  if (may_shut_down()) ExitWindowsEx(EWX_REBOOT, 0); break;
    case P_SHUTDOWN: if (may_shut_down()) ExitWindowsEx(EWX_SHUTDOWN, 0); break;
    }
}

static void entry_menu(int id, POINT pt)
{
    enum { C_PIN = 1, C_RUNAS, C_LOCATION, C_UNINSTALL };
    struct entry *e = entry_of(id);
    HMENU m;
    BOOL pinned;
    int cmd;

    if (!e) return;
    pinned = pin_index(e->name) >= 0;
    m = menu_new();
    if (e->kind == K_APP)
    {
        menu_add(m, C_PIN, pinned ? L"Un&pin from Start" : L"&Pin to Start");
        menu_add(m, 0, NULL);
        menu_add(m, C_RUNAS, L"Run as &administrator");
        menu_add(m, C_LOCATION, L"Open file &location");
        menu_add(m, 0, NULL);
        menu_add(m, C_UNINSTALL, L"&Uninstall");
    }
    else menu_add(m, C_RUNAS, L"&Open");
    cmd = track(m, pt, e->kind == K_APP ? L"context" : L"context-setting");
    DestroyMenu(m);
    switch (cmd)
    {
    case C_PIN:       pin(e->name, !pinned); InvalidateRect(g_panel, NULL, FALSE); break;
    case C_RUNAS:     run_entry(e, e->kind == K_APP ? L"runas" : NULL); break;
    case C_LOCATION:  open_location(e); break;
    case C_UNINSTALL: show_panel(FALSE); ShellExecuteW(NULL, NULL, L"control.exe", L"appwiz.cpl", NULL, SW_SHOWNORMAL); break;
    default:          dump(); break;
    }
}

static void open_folder(int csidl)
{
    WCHAR path[MAX_PATH];
    if (!SHGetSpecialFolderPathW(NULL, path, csidl, TRUE)) return;
    show_panel(FALSE);
    ShellExecuteW(NULL, NULL, L"explorer.exe", path, NULL, SW_SHOWNORMAL);
}

static void rail_action(int i, POINT screen)
{
    switch (i)
    {
    case RAIL_MENU:     g_rail_open = !g_rail_open; InvalidateRect(g_panel, NULL, FALSE); break;
    case RAIL_USER:     show_panel(FALSE); ShellExecuteW(NULL, NULL, L"control.exe", L"userpasswords", NULL, SW_SHOWNORMAL); break;
    case RAIL_DOCS:     open_folder(CSIDL_PERSONAL); break;
    case RAIL_PICS:     open_folder(CSIDL_MYPICTURES); break;
    case RAIL_SETTINGS:
        show_panel(FALSE);
        /* Settings (ms-settings:), or the Control Panel where Settings is not installed */
        if ((INT_PTR)ShellExecuteW(NULL, NULL, L"ms-settings:", NULL, NULL, SW_SHOWNORMAL) <= 32)
            ShellExecuteW(NULL, NULL, L"control.exe", NULL, NULL, SW_SHOWNORMAL);
        break;
    case RAIL_POWER:    power_menu(screen); break;
    }
}

/* ---- drawing -------------------------------------------------------------------- */

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, r, b);
    DeleteObject(b);
}

static void frame(HDC dc, const RECT *r, COLORREF c, int w)
{
    RECT e;
    SetRect(&e, r->left, r->top, r->right, r->top + w); fill(dc, &e, c);
    SetRect(&e, r->left, r->bottom - w, r->right, r->bottom); fill(dc, &e, c);
    SetRect(&e, r->left, r->top, r->left + w, r->bottom); fill(dc, &e, c);
    SetRect(&e, r->right - w, r->top, r->right, r->bottom); fill(dc, &e, c);
}

static void text(HDC dc, const WCHAR *s, RECT r, HFONT font, COLORREF c, UINT flags)
{
    SelectObject(dc, font);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, flags | DT_SINGLELINE | DT_NOPREFIX);
}

static const WCHAR *user_name(void)
{
    static WCHAR name[128];
    DWORD n = ARRAYSIZE(name);
    if (!name[0] && !GetUserNameW(name, &n)) lstrcpyW(name, L"User");
    return name;
}

/* the rail's glyphs, drawn with lines: menu, documents, pictures, settings, power */
static void draw_glyph_scaled(HDC dc, int which, int cx, int cy, COLORREF c, int scale)
{
    HPEN pen = CreatePen(PS_SOLID, max(1, S(1) * scale + (g_dpi >= 144)), c), old = SelectObject(dc, pen);
    int u = S(1) * scale;
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    switch (which)
    {
    case RAIL_MENU:
        MoveToEx(dc, cx - 8 * u, cy - 5 * u, NULL); LineTo(dc, cx + 8 * u, cy - 5 * u);
        MoveToEx(dc, cx - 8 * u, cy, NULL);         LineTo(dc, cx + 8 * u, cy);
        MoveToEx(dc, cx - 8 * u, cy + 5 * u, NULL); LineTo(dc, cx + 8 * u, cy + 5 * u);
        break;
    case RAIL_DOCS:   /* a page with a folded corner */
        MoveToEx(dc, cx - 6 * u, cy - 8 * u, NULL); LineTo(dc, cx + 2 * u, cy - 8 * u);
        LineTo(dc, cx + 6 * u, cy - 4 * u); LineTo(dc, cx + 6 * u, cy + 8 * u);
        LineTo(dc, cx - 6 * u, cy + 8 * u); LineTo(dc, cx - 6 * u, cy - 8 * u);
        MoveToEx(dc, cx + 2 * u, cy - 8 * u, NULL); LineTo(dc, cx + 2 * u, cy - 4 * u); LineTo(dc, cx + 6 * u, cy - 4 * u);
        break;
    case RAIL_PICS:   /* a frame with a mountain and the sun */
        Rectangle(dc, cx - 8 * u, cy - 6 * u, cx + 9 * u, cy + 7 * u);
        MoveToEx(dc, cx - 7 * u, cy + 5 * u, NULL); LineTo(dc, cx - 2 * u, cy); LineTo(dc, cx + 2 * u, cy + 4 * u);
        LineTo(dc, cx + 4 * u, cy + 2 * u); LineTo(dc, cx + 8 * u, cy + 6 * u);
        Ellipse(dc, cx + 2 * u, cy - 4 * u, cx + 6 * u, cy);
        break;
    case RAIL_SETTINGS: /* a gear: a ring and eight teeth */
    {
        int i;
        static const int dx[8] = { 0, 7, 10, 7, 0, -7, -10, -7 }, dy[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
        Ellipse(dc, cx - 6 * u, cy - 6 * u, cx + 7 * u, cy + 7 * u);
        Ellipse(dc, cx - 2 * u, cy - 2 * u, cx + 3 * u, cy + 3 * u);
        for (i = 0; i < 8; i++)
        {
            MoveToEx(dc, cx + dx[i] * u * 6 / 10, cy + dy[i] * u * 6 / 10, NULL);
            LineTo(dc, cx + dx[i] * u * 9 / 10, cy + dy[i] * u * 9 / 10);
        }
        break;
    }
    case RAIL_POWER:   /* a ring open at the top, and the stem through the gap */
        Arc(dc, cx - 7 * u, cy - 6 * u, cx + 8 * u, cy + 9 * u, cx - 3 * u, cy - 6 * u, cx + 4 * u, cy - 6 * u);
        MoveToEx(dc, cx, cy - 8 * u, NULL); LineTo(dc, cx, cy + 1 * u);
        break;
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

static void draw_glyph(HDC dc, int which, int cx, int cy, COLORREF c) { draw_glyph_scaled(dc, which, cx, cy, c, 1); }

static BOOL is_control_panel(const struct entry *e) { return e->kind == K_APP && !lstrcmpiW(e->name, L"Control Panel"); }

/* settings, and the Control Panel: a gear on an accent square, as Windows
 * marks its settings in results */
static void draw_badge(HDC dc, int x, int cy, int size)
{
    RECT b = { x, cy - size / 2, x + size, cy + size / 2 };
    fill(dc, &b, COL_ACCENT);
    draw_glyph(dc, RAIL_SETTINGS, x + size / 2, cy, COL_TEXT);
}

static void draw_avatar(HDC dc, int cx, int cy)
{
    WCHAR initial[2] = { towupper(user_name()[0]), 0 };
    HBRUSH b = CreateSolidBrush(COL_ACCENT), ob = SelectObject(dc, b);
    HPEN p = CreatePen(PS_SOLID, 1, COL_ACCENT_BR), op = SelectObject(dc, p);
    RECT r = { cx - S(12), cy - S(12), cx + S(12), cy + S(12) };
    Ellipse(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
    text(dc, initial, r, g_font_bold, COL_TEXT, DT_CENTER | DT_VCENTER);
}

static void draw_rail(HDC dc)
{
    RECT rail = { 0, 0, g_rail_open ? S(RAIL_OPEN_W) : S(RAIL_W), g_panel_h };
    int i;

    fill(dc, &rail, g_rail_open ? COL_RAIL_OPEN : COL_RAIL);
    for (i = 0; i < RAIL_COUNT; i++)
    {
        RECT r = rail_rect(i), label = r;
        int cx = r.left + S(RAIL_W) / 2, cy = (r.top + r.bottom) / 2;
        if (i == g_hot_rail) fill(dc, &r, COL_HOVER);
        if (i == RAIL_USER) draw_avatar(dc, cx, cy);
        else draw_glyph(dc, i, cx, cy, COL_TEXT);
        if (g_rail_open)
        {
            label.left += S(RAIL_W);
            text(dc, i == RAIL_USER ? user_name() : rail_labels[i], label, i == RAIL_MENU ? g_font_bold : g_font,
                 COL_TEXT, DT_LEFT | DT_VCENTER);
        }
    }
    if (g_rail_open)
    {
        /* an edge between the open rail and the list beneath it */
        RECT edge = { rail.right, 0, rail.right + S(1), g_panel_h };
        fill(dc, &edge, RGB(0x3A, 0x3A, 0x3A));
    }
}

static void draw_list(HDC dc)
{
    RECT clip = { list_left(), list_top(), list_left() + S(LIST_W), list_bottom() };
    HRGN rgn = CreateRectRgnIndirect(&clip);
    int i;

    SelectClipRgn(dc, rgn);
    if (g_search[0])
    {
        /* the search field, where the typing goes */
        RECT field = { list_left() + S(12), S(TOP_PAD), list_left() + S(LIST_W) - S(12), S(TOP_PAD) + S(SEARCH_H) }, t;
        SIZE sz;
        fill(dc, &field, COL_FIELD);
        frame(dc, &field, COL_ACCENT_BR, S(1) + (g_dpi >= 144));
        t = field; t.left += S(10);
        text(dc, g_search, t, g_font, COL_TEXT, DT_LEFT | DT_VCENTER);
        SelectObject(dc, g_font);
        GetTextExtentPoint32W(dc, g_search, (int)wcslen(g_search), &sz);
        { RECT caret = { t.left + sz.cx + S(1), field.top + S(7), t.left + sz.cx + S(2), field.bottom - S(7) }; fill(dc, &caret, COL_TEXT); }
    }
    for (i = 0; i < g_nrows; i++)
    {
        const struct row *rw = &g_rows[i];
        const struct entry *e = entry_of(rw->id);
        int y = rw->y - g_scroll;
        RECT r = { list_left() + S(4), y, list_left() + S(LIST_W) - S(8), y + rw->h }, t;

        if (r.bottom < 0 || r.top > g_panel_h) continue;
        if (g_search[0] && r.top < S(TOP_PAD) + S(SEARCH_H)) continue;
        if (rw->type == R_HEADER)
        {
            t = r; t.left += S(12);
            text(dc, rw->label, t, g_search[0] ? g_font_small : g_font_bold,
                 rw->label[1] ? COL_TEXT : COL_SUBTLE, DT_LEFT | DT_VCENTER);
            continue;
        }
        if (i == g_hot_row) fill(dc, &r, COL_HOVER);
        if (i == g_sel_row) frame(dc, &r, COL_SUBTLE, S(1) + (g_dpi >= 144));
        if (rw->type == R_BEST && i != g_hot_row) fill(dc, &r, RGB(0x29, 0x29, 0x29));
        if (e && (e->kind == K_SETTING || !e->icon || is_control_panel(e)))
            draw_badge(dc, r.left + S(10), (r.top + r.bottom) / 2, rw->type == R_BEST ? S(32) : S(24));
        else if (e && e->icon)
        {
            int sz = rw->type == R_BEST ? S(32) : S(24);
            DrawIconEx(dc, r.left + S(10), (r.top + r.bottom - sz) / 2, e->icon, sz, sz, 0, NULL, DI_NORMAL);
        }
        t = r;
        t.left += rw->type == R_BEST ? S(54) : S(46);
        t.right -= S(6);
        if (rw->type == R_BEST)
        {
            RECT sub = t;
            t.bottom = (r.top + r.bottom) / 2 + S(2);
            sub.top = t.bottom;
            text(dc, e->name, t, g_font, COL_TEXT, DT_LEFT | DT_BOTTOM | DT_END_ELLIPSIS);
            text(dc, e->kind == K_APP ? L"App" : L"Settings", sub, g_font_small, COL_SUBTLE, DT_LEFT | DT_TOP);
        }
        else text(dc, e->name, t, g_font, COL_TEXT, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
    }
    /* a thin scroll indicator while there is more */
    if (g_content_h > g_panel_h)
    {
        int track = g_panel_h - S(16), bar = max(S(24), track * g_panel_h / g_content_h);
        int top = S(8) + (track - bar) * g_scroll / max(1, g_content_h - g_panel_h);
        RECT b = { list_left() + S(LIST_W) - S(5), top, list_left() + S(LIST_W) - S(2), top + bar };
        fill(dc, &b, COL_SCROLL);
    }
    SelectClipRgn(dc, NULL);
    DeleteObject(rgn);
}

static void draw_tiles(HDC dc)
{
    RECT head = { tiles_left(), S(TOP_PAD), g_panel_w - S(GAP), S(TOP_PAD) + S(28) };
    int i, n = 0;

    text(dc, L"Pinned", head, g_font_small, COL_SUBTLE, DT_LEFT | DT_VCENTER);
    for (i = 0; i < g_npins; i++)
    {
        int a = tile_app(i);
        RECT r, name;
        if (a < 0) continue;
        r = tile_rect(i);
        fill(dc, &r, i == g_hot_tile ? COL_ACCENT_HI : COL_ACCENT);
        if (i == g_hot_tile || i == g_sel_tile) frame(dc, &r, i == g_sel_tile ? COL_TEXT : RGB(0xB8, 0x8C, 0xE8), S(2));
        if (is_control_panel(&g_apps[a]))
        {
            RECT gr = { (r.left + r.right) / 2 - S(16), r.top + S(22), (r.left + r.right) / 2 + S(16), r.top + S(54) };
            draw_glyph_scaled(dc, RAIL_SETTINGS, (gr.left + gr.right) / 2, (gr.top + gr.bottom) / 2, COL_TEXT, 2);
        }
        else if (g_apps[a].icon)
            DrawIconEx(dc, (r.left + r.right) / 2 - S(16), r.top + S(22), g_apps[a].icon, S(32), S(32), 0, NULL, DI_NORMAL);
        /* the name at the bottom, on two lines when it needs them, as Windows' tiles */
        name = r;
        name.left += S(8); name.right -= S(6); name.bottom -= S(5);
        {
            RECT calc = { 0, 0, name.right - name.left, 0 };
            int line;
            SelectObject(dc, g_font_small);
            DrawTextW(dc, L"Ag", -1, &calc, DT_CALCRECT | DT_SINGLELINE);
            line = calc.bottom;
            calc.right = name.right - name.left; calc.bottom = 0;
            DrawTextW(dc, g_apps[a].name, -1, &calc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            name.top = name.bottom - min(calc.bottom, 2 * line);
            SetTextColor(dc, COL_TEXT);
            DrawTextW(dc, g_apps[a].name, -1, &name, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
        }
        n++;
    }
    if (!n)
    {
        RECT r = { tiles_left(), tiles_top(), g_panel_w - S(GAP), tiles_top() + S(40) };
        text(dc, L"Right-click an app to pin it here", r, g_font_small, COL_SUBTLE, DT_LEFT | DT_TOP);
    }
}

static void on_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps), mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, g_panel_w, g_panel_h), oldbmp = SelectObject(mem, bmp);
    RECT all = { 0, 0, g_panel_w, g_panel_h };

    fill(mem, &all, COL_PANEL);
    SetBkMode(mem, TRANSPARENT);
    draw_list(mem);
    draw_tiles(mem);
    draw_rail(mem);   /* last: the open rail lies over the list */
    frame(mem, &all, RGB(0x3A, 0x3A, 0x3A), S(1));
    BitBlt(dc, 0, 0, g_panel_w, g_panel_h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldbmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);

    if (g_open_ms < 0 && g_open_start.QuadPart)
    {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        g_open_ms = (double)(now.QuadPart - g_open_start.QuadPart) * 1000.0 / freq.QuadPart;
    }
    dump();
}

/* ---- hit testing, selection, scrolling ------------------------------------------ */

static int rail_at(POINT pt)
{
    int i, w = g_rail_open ? S(RAIL_OPEN_W) : S(RAIL_W);
    if (pt.x >= w) return -1;
    for (i = 0; i < RAIL_COUNT; i++)
    {
        RECT r = rail_rect(i);
        if (PtInRect(&r, pt)) return i;
    }
    return -2;   /* the rail's empty space */
}

static int row_at(POINT pt)
{
    int i, y = pt.y + g_scroll;
    if (pt.x < list_left() || pt.x >= list_left() + S(LIST_W)) return -1;
    if (g_search[0] && pt.y < S(TOP_PAD) + S(SEARCH_H)) return -1;
    for (i = 0; i < g_nrows; i++)
        if (g_rows[i].type != R_HEADER && y >= g_rows[i].y && y < g_rows[i].y + g_rows[i].h) return i;
    return -1;
}

static int tile_at(POINT pt)
{
    int i;
    for (i = 0; i < g_npins; i++)
    {
        RECT r = tile_rect(i);
        if (tile_app(i) >= 0 && PtInRect(&r, pt)) return i;
    }
    return -1;
}

static int max_scroll(void) { return max(0, g_content_h + S(8) - g_panel_h); }

static void scroll_to(int y)
{
    g_scroll = min(max(0, y), max_scroll());
}

static void ensure_visible(int row)
{
    int top = g_search[0] ? S(TOP_PAD) + S(SEARCH_H) + S(8) : 0;
    if (row < 0) return;
    if (g_rows[row].y - g_scroll < top) scroll_to(g_rows[row].y - top);
    else if (g_rows[row].y + g_rows[row].h - g_scroll > g_panel_h) scroll_to(g_rows[row].y + g_rows[row].h - g_panel_h + S(8));
}

static int next_item(int from, int dir)
{
    int i;
    for (i = from + dir; i >= 0 && i < g_nrows; i += dir)
        if (g_rows[i].type != R_HEADER) return i;
    return from;
}

static int first_item(void) { return next_item(-1, 1); }

static void search_changed(void)
{
    build_rows();
    g_scroll = 0;
    g_hot_row = -1;
    g_sel_tile = -1;
    g_sel_row = g_search[0] ? first_item() : -1;
    if (g_sel_row >= 0 && g_rows[g_sel_row].type == R_HEADER) g_sel_row = -1;
    InvalidateRect(g_panel, NULL, FALSE);
}

static void show_panel(BOOL show)
{
    if (show)
    {
        int sh = GetSystemMetrics(SM_CYSCREEN), sw = GetSystemMetrics(SM_CXSCREEN);
        RECT work;
        QueryPerformanceCounter(&g_open_start);
        g_open_ms = -1;
        build_list();
        load_pins();
        g_search[0] = 0;
        g_rail_open = FALSE;
        build_rows();
        g_scroll = 0;
        g_hot_row = g_hot_tile = g_hot_rail = g_sel_row = g_sel_tile = -1;
        g_launched[0] = 0;
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) || work.bottom <= 0) SetRect(&work, 0, 0, sw, sh - S(TASKBAR_H));
        if (work.bottom >= sh) work.bottom = sh - S(TASKBAR_H);   /* the taskbar is always there */
        g_panel_w = min(S(PANEL_W), sw);
        g_panel_h = min(S(PANEL_H), work.bottom - work.top);
        SetWindowPos(g_panel, HWND_TOPMOST, 0, work.bottom - g_panel_h, g_panel_w, g_panel_h, SWP_NOACTIVATE);
        ShowWindow(g_panel, SW_SHOW);
        SetForegroundWindow(g_panel);
        SetFocus(g_panel);
        InvalidateRect(g_panel, NULL, FALSE);
    }
    else if (IsWindowVisible(g_panel))
    {
        ShowWindow(g_panel, SW_HIDE);
        dump();
    }
}

static void activate_selection(void)
{
    if (g_sel_tile >= 0 && tile_app(g_sel_tile) >= 0) run_entry(&g_apps[tile_app(g_sel_tile)], NULL);
    else if (g_sel_row >= 0 && g_sel_row < g_nrows) run_entry(entry_of(g_rows[g_sel_row].id), NULL);
    else if (g_search[0] && first_item() >= 0) run_entry(entry_of(g_rows[first_item()].id), NULL);
}

static void key_down(WPARAM vk)
{
    int tiles = 0, i;
    for (i = 0; i < g_npins; i++) if (tile_app(i) >= 0) tiles = i + 1;

    switch (vk)
    {
    case VK_ESCAPE:
        if (g_search[0]) { g_search[0] = 0; search_changed(); }
        else show_panel(FALSE);
        return;
    case VK_RETURN:
        activate_selection();
        return;
    case VK_TAB:
        if (g_sel_tile >= 0 || !tiles) { g_sel_tile = -1; g_sel_row = g_sel_row >= 0 ? g_sel_row : first_item(); }
        else { g_sel_row = -1; g_sel_tile = 0; }
        break;
    case VK_DOWN: case VK_UP:
        if (g_sel_tile >= 0)
        {
            int t = g_sel_tile + (vk == VK_DOWN ? TILE_COLS : -TILE_COLS);
            if (t >= 0 && t < tiles) g_sel_tile = t;
        }
        else
        {
            g_sel_row = g_sel_row < 0 ? first_item() : next_item(g_sel_row, vk == VK_DOWN ? 1 : -1);
            ensure_visible(g_sel_row);
        }
        break;
    case VK_LEFT: case VK_RIGHT:
        if (g_sel_tile >= 0)
        {
            int t = g_sel_tile + (vk == VK_RIGHT ? 1 : -1);
            if (t >= 0 && t < tiles) g_sel_tile = t;
            else if (t < 0) { g_sel_tile = -1; g_sel_row = first_item(); }
        }
        else if (vk == VK_RIGHT && tiles) { g_sel_row = -1; g_sel_tile = 0; }
        break;
    case VK_BACK:
        if (g_search[0]) { g_search[wcslen(g_search) - 1] = 0; search_changed(); }
        return;
    case VK_APPS:
        if (g_sel_row >= 0)
        {
            RECT r;
            POINT pt;
            GetWindowRect(g_panel, &r);
            pt.x = r.left + list_left() + S(60);
            pt.y = r.top + g_rows[g_sel_row].y - g_scroll + g_rows[g_sel_row].h;
            entry_menu(g_rows[g_sel_row].id, pt);
        }
        return;
    default:
        return;
    }
    InvalidateRect(g_panel, NULL, FALSE);
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };

    switch (msg)
    {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: on_paint(hwnd); return 0;
    case WM_MOUSEMOVE:
    {
        int row = row_at(pt), tile = tile_at(pt), rail = rail_at(pt);
        if (rail != -1) { row = -1; tile = -1; }   /* the rail lies on top */
        if (rail < 0) rail = -1;
        if (row != g_hot_row || tile != g_hot_tile || rail != g_hot_rail)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            g_hot_row = row; g_hot_tile = tile; g_hot_rail = rail;
            InvalidateRect(hwnd, NULL, FALSE);
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hot_row = g_hot_tile = g_hot_rail = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        scroll_to(g_scroll - GET_WHEEL_DELTA_WPARAM(wp) * S(ROW_H) * 3 / WHEEL_DELTA);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
    {
        int rail = rail_at(pt), row, tile;
        POINT screen = pt;
        ClientToScreen(hwnd, &screen);
        if (rail >= 0) { rail_action(rail, screen); return 0; }
        if (rail == -2) return 0;
        if (g_rail_open) { g_rail_open = FALSE; InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if ((row = row_at(pt)) >= 0) { run_entry(entry_of(g_rows[row].id), NULL); return 0; }
        if ((tile = tile_at(pt)) >= 0) { run_entry(&g_apps[tile_app(tile)], NULL); return 0; }
        return 0;
    }
    case WM_RBUTTONUP:
    {
        int row = row_at(pt), tile = tile_at(pt);
        POINT screen = pt;
        ClientToScreen(hwnd, &screen);
        if (rail_at(pt) != -1) return 0;
        if (row >= 0) entry_menu(g_rows[row].id, screen);
        else if (tile >= 0) entry_menu(tile_app(tile), screen);
        return 0;
    }
    case WM_KEYDOWN:
        key_down(wp);
        return 0;
    case WM_MEASUREITEM:
        if (((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_MENU) { menu_measure((MEASUREITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_DRAWITEM:
        if (((DRAWITEMSTRUCT *)lp)->CtlType == ODT_MENU) { menu_draw((DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_MENUCHAR:
        return menu_char(LOWORD(wp), (HMENU)lp);
    case WM_CHAR:
        if (wp >= 32 && wp != 127 && wcslen(g_search) + 1 < ARRAYSIZE(g_search))
        {
            size_t n = wcslen(g_search);
            if (!n && wp == ' ') return 0;
            g_search[n] = (WCHAR)wp;
            g_search[n + 1] = 0;
            search_changed();
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !g_menu) show_panel(FALSE);   /* click-away closes */
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* The hidden listener explorer posts SG_START_TOGGLE to. */
static LRESULT CALLBACK listener_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == SG_START_TOGGLE)
    {
        show_panel(!IsWindowVisible(g_panel));
        return 0;
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HFONT make_font(int pt10, int weight)
{
    return CreateFontW(-MulDiv(pt10, g_dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    static WCHAR dump_path[MAX_PATH];
    WNDCLASSW lc = { 0 }, pc = { 0 };
    HDC screen = GetDC(NULL);
    MSG msg;

    (void)prev; (void)cmd; (void)show;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);   /* shortcuts' icons come through IShellLink */
    SetProcessDPIAware();
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    if (g_dpi < 96) g_dpi = 96;
    if (GetEnvironmentVariableW(L"SG_START_DUMP", dump_path, MAX_PATH)) g_dump_path = dump_path;

    g_font       = make_font(105, FW_NORMAL);
    g_font_small = make_font(90, FW_NORMAL);
    g_font_bold  = make_font(105, FW_SEMIBOLD);
    g_font_big   = make_font(150, FW_NORMAL);
    g_font_glyph = make_font(120, FW_NORMAL);

    lc.lpfnWndProc = listener_proc; lc.hInstance = inst; lc.lpszClassName = LISTENER_CLASS;
    RegisterClassW(&lc);
    pc.lpfnWndProc = panel_proc; pc.hInstance = inst; pc.lpszClassName = PANEL_CLASS;
    pc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&pc);

    CreateWindowW(LISTENER_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
    g_panel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, PANEL_CLASS, L"Start",
                              WS_POPUP, 0, 0, S(PANEL_W), S(PANEL_H), NULL, NULL, inst, NULL);
    /* warm the list and its icons, so the first opening is quick */
    build_list();
    first_run();
    refresh_sleep_caps();

    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
