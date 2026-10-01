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
#include <shlwapi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include "sg-mode.h"
#include "sg-round.h"

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

/* the centred layout (g_centered) */
#define C_W        640
#define C_H        624
#define C_SEARCH_Y 20
#define C_SEARCH_H 36
#define C_HEAD_Y   68
#define C_BODY_Y   104
#define C_CELL_W   96
#define C_CELL_H   84
#define C_COLS     6
#define C_ROWS     3
#define C_REC_H    52
#define C_BAR_H    64
enum { C_SEARCH = 1, C_MORE, C_USER, C_POWER, C_PIN = 100, C_REC = 200 };
/* the Horizon look's Start (Taskbar Style 1), laid out as the menus of that
 * era were (our own drawing, at that era's proportions): the user and a
 * picture on a blue band, an orange rule under it; pinned programs (the
 * web browser as "Internet", mail as "E-mail", in bold with what they are
 * under them) and the most used programs on white at the left, "All
 * Programs" with a green arrow at its foot, which opens the programs as
 * cascading menus; places on pale blue at the right, the first ones bold;
 * Log Off and Turn Off Computer on a blue band at the bottom. */
#define X_W        384
#define X_H        506
#define X_HEAD     58
#define X_FOOT     42
#define X_LEFT_W   192
#define X_ALL_H    34
#define X_PIN_H    42
#define X_MFU_H    36
#define X_PLACE_H  26
#define X_PLACE1_H 30
#define X_NPINS    3
#define X_NMFU     6
#define X_EDGE     2
#define X_HOVER_TIMER 0x5847
/* the Glass look's Start (Taskbar Style 2), as the menus of that era were,
 * our own drawing: a glass frame; programs on white at the left, "All
 * Programs" under them opening the list in place, a search box below; the
 * user's picture over a glass column of places at the right, plain text;
 * a Shut down button and its arrow at the foot of that column */
#define G_W        430
#define G_H        520
#define G_FRAME    7
#define G_LEFT_W   246
#define G_FOOT     46
#define G_ROW      38
#define G_ALL_H    32
#define G_PLACE_H  29
enum { X_USER = 1, X_ALL, X_LOGOFF, X_TURNOFF, X_PIN = 100, X_MFU = 200, X_PLACE = 300 };


/* Start follows the Windows mode (SystemUsesLightTheme, sg-mode.h), as
 * Windows 10's does: dark unless the shell is set to light */
struct start_palette {
    COLORREF panel, rail, rail_open, hover, press, text, subtle, field, dim, scroll;
    COLORREF menu, menu_sel, line, edge, best;
};
static const struct start_palette dark_palette = {
    RGB(0x1F, 0x1F, 0x1F), RGB(0x17, 0x17, 0x17), RGB(0x26, 0x26, 0x26), RGB(0x33, 0x33, 0x33),
    RGB(0x44, 0x44, 0x44), RGB(0xFF, 0xFF, 0xFF), RGB(0xA8, 0xA8, 0xA8), RGB(0x2B, 0x2B, 0x2B),
    RGB(0x55, 0x55, 0x55), RGB(0x5A, 0x5A, 0x5A),
    RGB(0x2B, 0x2B, 0x2B), RGB(0x41, 0x41, 0x41), RGB(0x55, 0x55, 0x55), RGB(0x3A, 0x3A, 0x3A), RGB(0x29, 0x29, 0x29),
};
static const struct start_palette light_palette = {
    RGB(0xF2, 0xF2, 0xF2), RGB(0xE6, 0xE6, 0xE6), RGB(0xEC, 0xEC, 0xEC), RGB(0xDA, 0xDA, 0xDA),
    RGB(0xC8, 0xC8, 0xC8), RGB(0x00, 0x00, 0x00), RGB(0x5A, 0x5A, 0x5A), RGB(0xFF, 0xFF, 0xFF),
    RGB(0xB0, 0xB0, 0xB0), RGB(0xA0, 0xA0, 0xA0),
    RGB(0xF9, 0xF9, 0xF9), RGB(0xE0, 0xE0, 0xE0), RGB(0xCC, 0xCC, 0xCC), RGB(0xCC, 0xCC, 0xCC), RGB(0xE4, 0xE4, 0xE4),
};
/* the taskbar's looks (HKCU\Software\Stained Glass\Taskbar Style, wine-sg
 * 0600) give Start their colours and a frame in the bar's colour: Horizon a
 * light panel in a blue frame, Glass a dark blue-grey one with a light rim */
static const struct start_palette horizon_palette = {
    RGB(0xF6, 0xF9, 0xFE), RGB(0xD3, 0xE2, 0xFA), RGB(0xE2, 0xEC, 0xFC), RGB(0xC2, 0xD8, 0xFA),
    RGB(0xA4, 0xC4, 0xF6), RGB(0x00, 0x00, 0x00), RGB(0x4A, 0x5A, 0x78), RGB(0xFF, 0xFF, 0xFF),
    RGB(0xA0, 0xA8, 0xB8), RGB(0x9A, 0xB0, 0xD4),
    RGB(0xF8, 0xFA, 0xFE), RGB(0xC2, 0xD8, 0xFA), RGB(0xC8, 0xD4, 0xE8), RGB(0x2A, 0x62, 0xD6), RGB(0xDC, 0xE8, 0xFA),
};
static const struct start_palette glass_palette = {
    RGB(0x1E, 0x27, 0x33), RGB(0x15, 0x1C, 0x26), RGB(0x23, 0x2E, 0x3C), RGB(0x34, 0x46, 0x5C),
    RGB(0x45, 0x5C, 0x78), RGB(0xFF, 0xFF, 0xFF), RGB(0xA8, 0xB4, 0xC4), RGB(0x2A, 0x36, 0x44),
    RGB(0x5A, 0x66, 0x76), RGB(0x5C, 0x6A, 0x7C),
    RGB(0x24, 0x2F, 0x3C), RGB(0x3A, 0x4E, 0x66), RGB(0x44, 0x54, 0x6A), RGB(0x8A, 0x9A, 0xB0), RGB(0x2A, 0x38, 0x48),
};
static const struct start_palette *g_pal = &dark_palette;
static int g_look_frame;   /* the look's frame, px at 96 DPI (0: the flat look's 1 px edge) */
#define COL_PANEL    (g_pal->panel)
#define COL_RAIL     (g_pal->rail)
#define COL_RAIL_OPEN (g_pal->rail_open)
#define COL_HOVER    (g_pal->hover)
#define COL_PRESS    (g_pal->press)
#define COL_TEXT     (g_pal->text)
#define COL_SUBTLE   (g_pal->subtle)
#define COL_ACCENT   (sg_accent())
#define COL_ACCENT_HI (sg_accent_light(10))
#define COL_ACCENT_BR (sg_accent_light(12))
#define COL_ON_ACCENT RGB(0xFF, 0xFF, 0xFF)   /* text and glyphs on the accent, either mode */
#define COL_FIELD    (g_pal->field)
#define COL_DIM      (g_pal->dim)
#define COL_SCROLL   (g_pal->scroll)

static const WCHAR LISTENER_CLASS[] = L"SgStartPanel";
static const WCHAR PANEL_CLASS[]    = L"SgStartWindow";
static const WCHAR START_KEY[]      = L"Software\\Stained Glass\\Start";

static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* Settings > Personalization > Start, read at every opening */
static const WCHAR ADVANCED_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
static const WCHAR CDM_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager";
static BOOL g_app_list = TRUE;      /* "Show app list in Start menu" */
static BOOL g_show_list = TRUE;     /* the list column is shown now (the app list, or search, or All apps) */
static BOOL g_fullscreen;           /* "Use Start full screen" */
static int g_tile_cols = 3;         /* "Show more tiles on Start": 4 */
/* "Start layout: Centered" (Settings > Personalization > Start): a panel in
 * the middle, over the centred taskbar -- a search box, a grid of the pinned
 * apps, the most used ones as "Recommended", the user and power at the
 * bottom; search and All apps show the list in it. The tiles are the
 * default. */
static BOOL g_centered;
static BOOL g_xp;                  /* the two-column Start of the Horizon and Glass looks (see X_W) */
static BOOL g_seven;               /* ... the Glass look's (see G_W) */
static UINT g_acrylic;             /* frosted, this opaque in percent (0: not) */
static int g_hot_x = -1;           /* its part under the pointer */
static int g_mfu[X_NMFU], g_nmfu;  /* its most used programs */
static int g_hot_c = -1;           /* the centred layout's part under the pointer */
static int g_rec[6], g_nrec;       /* its Recommended: the most used apps */

static DWORD reg_value(const WCHAR *key, const WCHAR *name, DWORD def)
{
    DWORD v = def, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, NULL, &v, &size)) v = def;
    return v;
}

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
    { .name = L"Updates", .path = L"ms-settings:windowsupdate", .args = L"", .keywords = L"windows update updates upgrade", .kind = K_SETTING },
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

/* more words to find an app by, as Windows' search finds Notepad for "txt" */
static const struct { const WCHAR *name, *words; } app_keywords[] = {
    { L"Notepad", L"txt text editor notes log ini" },
    { L"WordPad", L"rtf doc docx document word writer text" },
    { L"Paint", L"draw drawing picture image bmp png mspaint" },
    { L"Photos", L"picture pictures image images photo jpg jpeg png gif viewer" },
    { L"Media Player", L"music video movie audio mp3 mp4 mkv player" },
    { L"Calculator", L"calc math sums" },
    { L"Linux Terminal", L"linux terminal root bash shell sudo console driver xterm command line" },
    { L"Report a problem", L"bug report feedback debug log crash tester issue help" },
    { L"Command Prompt", L"cmd console dos shell terminal" },
    { L"Terminal", L"console shell cmd powershell prompt" },
    { L"PowerShell 7", L"pwsh ps shell console terminal" },
    { L"Task Manager", L"taskmgr processes performance end task kill" },
    { L"Snipping Tool", L"screenshot screen capture snip print screen" },
    { L"SG PDF", L"pdf document viewer editor edit redact sign form comment combine" },
    { L"File Explorer", L"files folders explorer documents downloads" },
    { L"Alarms & Clock", L"alarm timer stopwatch clock world" },
    { L"Sticky Notes", L"notes note memo" },
    { L"Character Map", L"symbols characters unicode charmap emoji" },
    { L"Magnifier", L"zoom magnify accessibility" },
    { L"On-Screen Keyboard", L"osk keyboard accessibility touch" },
    { L"Get a web browser", L"browser internet firefox chrome edge brave" },
    { L"SG Store", L"store apps install download update programs software browser office" },
    { L"Remote Desktop Connection", L"rdp mstsc remote" },
    { L"SG Office Documents", L"office word docx doc odt document writer letter text" },
    { L"SG Office Spreadsheets", L"office excel xlsx xls ods csv spreadsheet sheet calc table" },
    { L"SG Office Presentations", L"office powerpoint pptx ppt odp presentation slides slideshow impress" },
};

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
    for (i = 0; i < (int)ARRAYSIZE(app_keywords); i++)
        if (!lstrcmpiW(app_keywords[i].name, name)) lstrcpynW(g_apps[g_napps].keywords, app_keywords[i].words, 160);
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

/* shortcuts at the top of a Start Menu folder, beside Programs (SumatraPDF
 * puts its own there; Start lists them as it does Programs') -- the files
 * only: Programs is scanned on its own */
static void scan_start_root(int csidl)
{
#ifndef SG_MUTANT_NOSTARTROOT
    WCHAR base[MAX_PATH], pattern[MAX_PATH], full[MAX_PATH], name[128], *ext;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!SHGetSpecialFolderPathW(NULL, base, csidl, FALSE) || _snwprintf(pattern, MAX_PATH, L"%s\\*", base) < 0) return;
    if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!(ext = wcsrchr(fd.cFileName, '.')) || (lstrcmpiW(ext, L".lnk") && lstrcmpiW(ext, L".url"))) continue;
        if (_snwprintf(full, MAX_PATH, L"%s\\%s", base, fd.cFileName) < 0) continue;
        lstrcpynW(name, fd.cFileName, (int)min(128, ext - fd.cFileName + 1));
        add_app(name, full, &fd.ftCreationTime);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    (void)csidl;
#endif
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

/* "Show most used apps": what this user starts from Start, counted as
 * Windows counts it only while Start_TrackProgs is on */
static const WCHAR USAGE_KEY[] = L"Software\\Stained Glass\\Start\\Usage";
static BOOL track_progs(void) { return reg_value(ADVANCED_KEY, L"Start_TrackProgs", 1) != 0; }

/* the counts, read in one pass per opening (a registry read per app was
 * most of the time Start took to open) */
struct usage { WCHAR name[128]; DWORD count; };
static struct usage g_usage[64];
static int g_nusage;

static void usage_load(void)
{
    HKEY key;
    DWORD i, len, type, count, size;
    g_nusage = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, USAGE_KEY, 0, KEY_READ, &key)) return;
    for (i = 0; g_nusage < (int)ARRAYSIZE(g_usage); i++)
    {
        len = ARRAYSIZE(g_usage[0].name); size = sizeof(count);
        if (RegEnumValueW(key, i, g_usage[g_nusage].name, &len, NULL, &type, (BYTE *)&count, &size)) break;
        if (type != REG_DWORD || !count) continue;
        g_usage[g_nusage++].count = count;
    }
    RegCloseKey(key);
}

static DWORD usage_of(const WCHAR *name)
{
    int i;
    for (i = 0; i < g_nusage; i++) if (!lstrcmpiW(g_usage[i].name, name)) return g_usage[i].count;
    return 0;
}

static void count_launch(const WCHAR *name)
{
    DWORD n = reg_value(USAGE_KEY, name, 0) + 1;
    HKEY key;
    if (!track_progs() || RegCreateKeyExW(HKEY_CURRENT_USER, USAGE_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL)) return;
    RegSetValueExW(key, name, 0, REG_DWORD, (BYTE *)&n, sizeof(n));
    RegCloseKey(key);
}

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
    scan_start_root(CSIDL_STARTMENU);
    scan_start_root(CSIDL_COMMON_STARTMENU);
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
    add_beside(L"WordPad", L"sg-wordpad64.exe");
    add_beside(L"SG PDF", L"sg-pdf64.exe");
    /* SG Office (package sg-office): listed when it is installed */
    add_beside(L"SG Office Documents", L"sg-documents64.exe");
    add_beside(L"SG Office Spreadsheets", L"sg-spreadsheets64.exe");
    add_beside(L"SG Office Presentations", L"sg-presentations64.exe");
    add_beside(L"Magnifier", L"sg-magnify64.exe");
    add_beside(L"On-Screen Keyboard", L"sg-osk64.exe");
    add_beside(L"Get a web browser", L"sg-browser64.exe");
    add_beside(L"SG Store", L"sg-store64.exe");
    add_beside(L"Linux Terminal", L"sg-rootterm64.exe");
    add_beside(L"Report a problem", L"sg-bugreport64.exe");
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
                                                 L"PowerShell", L"Remote Desktop Connection", L"SG Store" };
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

static int list_top(void);

static void build_rows(void)
{
    int i;
    g_nrows = 0;
    g_content_h = g_centered || g_xp ? list_top() : S(TOP_PAD);

    if (g_search[0])
    {
        static struct hit hits[1024];
        WCHAR q[64];
        int n = 0, apps = 0, settings = 0;

        for (i = 0; g_search[i]; i++) q[i] = towlower(g_search[i]);
        q[i] = 0;
        if (!g_centered) g_content_h += S(SEARCH_H) + S(8);   /* the centred layout's box is above the list; Horizon's is its first row */
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

    /* Settings > Personalization > Start: "Show most used apps" -- the five
     * this user starts most, from Start */
    usage_load();
    if (track_progs() && g_nusage)
    {
        int most[5], counts[5], n = 0, j;
        for (i = 0; i < g_napps; i++)
        {
            DWORD c = usage_of(g_apps[i].name);
            if (!c) continue;
            for (j = n; j > 0 && counts[j - 1] < (int)c; j--)
                if (j < 5) { most[j] = most[j - 1]; counts[j] = counts[j - 1]; }
            if (j < 5) { most[j] = i; counts[j] = c; if (n < 5) n++; }
        }
        if (n) add_row(R_HEADER, -1, L"Most used");
        for (j = 0; j < n; j++) add_row(R_ITEM, most[j], NULL);
    }
    /* "Show recently added apps" */
    if (g_nrecent && reg_show_recent())
    {
        add_row(R_HEADER, -1, L"Recently added");
        for (i = 0; i < g_nrecent; i++) add_row(R_ITEM, recents[i], NULL);
    }
    /* "Show suggestions occasionally in Start": one of the apps that came
     * with the system and this user has never started -- another each day.
     * Nothing from outside the machine. */
    if (reg_value(CDM_KEY, L"SubscribedContent-338388Enabled", 1))
    {
        static const WCHAR *const offer[] = { L"Calculator", L"Photos", L"Paint", L"Sticky Notes", L"Media Player",
                                              L"Snipping Tool", L"Alarms & Clock", L"Terminal", L"Character Map" };
        SYSTEMTIME st;
        int k, n = ARRAYSIZE(offer), start;
        GetLocalTime(&st);
        start = (st.wYear * 372 + st.wMonth * 31 + st.wDay) % n;
        for (k = 0; k < n; k++)
        {
            int a = find_app(offer[(start + k) % n]);
            if (a < 0 || usage_of(g_apps[a].name) || g_apps[a].recent) continue;
            add_row(R_HEADER, -1, L"Suggested");
            add_row(R_ITEM, a, NULL);
            break;
        }
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

static int list_left(void) { return g_seven ? S(G_FRAME) + S(2) : g_xp ? 0 : g_centered ? S(24) : S(RAIL_W); }
static int list_top(void) { return g_seven ? S(G_FRAME) + S(4) : g_xp ? S(X_HEAD) + S(8) : g_centered ? S(C_BODY_Y) : 0; }
static int list_bottom(void)
{
    if (g_seven) return g_panel_h - S(G_FOOT) - S(G_ALL_H) - S(8);
    return g_xp ? g_panel_h - S(X_FOOT) - S(X_ALL_H) : g_centered ? g_panel_h - S(C_BAR_H) : g_panel_h;
}
static int list_w(void) { return !g_show_list ? 0 : g_seven ? S(G_LEFT_W) - S(4) : g_xp ? S(X_LEFT_W) : g_centered ? g_panel_w - S(48) : S(LIST_W); }
static int tiles_left(void) { return S(RAIL_W) + list_w() + S(GAP); }
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
    int c = i % g_tile_cols, rw = i / g_tile_cols;
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

static int x_nmfu(void);
static RECT x_head_rect(void), x_right_rect(void), x_all_rect(void), x_logoff_rect(void), x_turnoff_rect(void), x_foot_rect(void);
static RECT g_left_rect(void), g_search_rect(void);
struct x_pin { WCHAR label[64], sub[128], path[MAX_PATH]; HICON icon; int app; };
static struct x_pin g_xpins[X_NPINS];
static int g_nxpins;

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
    dprint(f, L"mode=%ls\n", g_pal == &dark_palette ? L"dark" : L"light");
    dprint(f, L"look=%ls\n", g_pal == &horizon_palette ? L"horizon" : g_pal == &glass_palette ? L"glass" : L"flat");
    dprint(f, L"list=%d tile_cols=%d fullscreen=%d\n", g_show_list, g_tile_cols, g_fullscreen);
    dprint(f, L"centered=%d\n", g_centered);
    dprint(f, L"xp=%d\n", g_xp && !g_seven);
    dprint(f, L"seven=%d\n", g_seven);
    dprint(f, L"dropshadow=%d\n", (GetClassLongW(g_panel, GCL_STYLE) & CS_DROPSHADOW) != 0);
    dprint(f, L"acrylic=%u\n", (UINT)(UINT_PTR)GetPropW(g_panel, L"__wine_sg_acrylic"));
    if (g_xp)
    {
        RECT r;
        for (i = 0; i < x_nmfu(); i++) dprint(f, L"mfu %ls\n", g_apps[g_mfu[i]].name);
        for (i = 0; i < g_nxpins; i++) dprint(f, L"xpin %ls|%ls\n", g_xpins[i].label, g_xpins[i].sub);
        r = x_head_rect(); dprint(f, L"xhead=%ld,%ld,%ld,%ld\n", r.left, r.top, r.right, r.bottom);
        r = x_right_rect(); dprint(f, L"xright=%ld,%ld,%ld,%ld\n", r.left, r.top, r.right, r.bottom);
        r = x_all_rect(); dprint(f, L"xall=%ld,%ld,%ld,%ld\n", r.left, r.top, r.right, r.bottom);
        r = x_logoff_rect(); dprint(f, L"xlogoff=%ld,%ld,%ld,%ld\n", r.left, r.top, r.right, r.bottom);
        r = x_turnoff_rect(); dprint(f, L"xturnoff=%ld,%ld,%ld,%ld\n", r.left, r.top, r.right, r.bottom);
        r = x_foot_rect(); dprint(f, L"xfoot=%ld,%ld,%ld,%ld\n", r.left, r.top, r.right, r.bottom);
        if (g_seven)
        {
            r = g_left_rect(); dprint(f, L"gleft=%ld,%ld,%ld,%ld\n", r.left, r.top, r.right, r.bottom);
            r = g_search_rect(); dprint(f, L"gsearch=%ld,%ld,%ld,%ld\n", r.left, r.top, r.right, r.bottom);
        }
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
    if (g_centered) for (i = 0; i < g_nrec; i++) dprint(f, L"rec %ls\n", g_apps[g_rec[i]].name);
    fclose(f);
}

/* ---- actions ------------------------------------------------------------------- */

static void show_panel(BOOL show);
static void show_list(BOOL on);
static void query_taskbar(void);

static void run_entry(const struct entry *e, const WCHAR *verb)
{
    if (!e) return;
    _snwprintf(g_launched, ARRAYSIZE(g_launched), L"%ls%ls", verb ? L"runas " : L"", e->name);
    if (e->kind == K_APP) count_launch(e->name);
    show_panel(FALSE);
    /* Linux Terminal as administrator is Linux's root, after the user's own
     * password (sudo): the terminal itself asks, not the elevation broker */
    if (verb && !lstrcmpW(verb, L"runas") && wcsstr(e->path, L"sg-rootterm64.exe"))
    {
        ShellExecuteW(NULL, NULL, e->path, L"--admin", NULL, SW_SHOWNORMAL);
        return;
    }
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
#define COL_MENU     (g_pal->menu)
#define COL_MENU_SEL (g_pal->menu_sel)

static HMENU menu_new(void)
{
    MENUINFO mi = { sizeof(mi), MIM_BACKGROUND };
    HMENU m = CreatePopupMenu();
    static HBRUSH bg;
    static COLORREF made = CLR_INVALID;
    if (made != COL_MENU) { if (bg) DeleteObject(bg); bg = CreateSolidBrush(made = COL_MENU); }
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
        b = CreateSolidBrush(g_pal->line); FillRect(dis->hDC, &line, b); DeleteObject(b);
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

/* ---- the taskbar's pins (wine-sg 0485): shortcuts in User Pinned\TaskBar,
 * in the order HKCU\Software\Stained Glass\Taskbar PinOrder lists --------- */

static BOOL taskbar_pin_path(const struct entry *e, WCHAR *out)
{
    WCHAR dir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, dir))) return FALSE;
    lstrcatW(dir, L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar");
    SHCreateDirectoryExW(NULL, dir, NULL);
    _snwprintf(out, MAX_PATH, L"%ls\\%ls.lnk", dir, e->name);
    out[MAX_PATH - 1] = 0;
    return TRUE;
}

static BOOL taskbar_pinned(const struct entry *e)
{
    WCHAR lnk[MAX_PATH];
    return taskbar_pin_path(e, lnk) && GetFileAttributesW(lnk) != INVALID_FILE_ATTRIBUTES;
}

/* the order, less (or plus, at its end) one shortcut's file name */
static void taskbar_order(const WCHAR *file, BOOL add)
{
    static const WCHAR key[] = L"Software\\Stained Glass\\Taskbar";
    WCHAR old[4096] = L"", out[4096], *p = out;
    const WCHAR *q;
    DWORD size = sizeof(old) - 2 * sizeof(WCHAR);
    HKEY k;

    RegGetValueW(HKEY_CURRENT_USER, key, L"PinOrder", RRF_RT_REG_MULTI_SZ, NULL, old, &size);
    for (q = old; *q && p - out < 4000; q += lstrlenW(q) + 1)
    {
        if (!lstrcmpiW(q, file)) continue;
        lstrcpyW(p, q);
        p += lstrlenW(p) + 1;
    }
    if (add && p - out + lstrlenW(file) < 4000) { lstrcpyW(p, file); p += lstrlenW(p) + 1; }
    *p++ = 0;
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL))
    {
        RegSetValueExW(k, L"PinOrder", 0, REG_MULTI_SZ, (BYTE *)out, (DWORD)((p - out) * sizeof(WCHAR)));
        RegCloseKey(k);
    }
}

static void taskbar_pin(const struct entry *e, BOOL on)
{
    WCHAR lnk[MAX_PATH];
    DWORD_PTR r;
    const WCHAR *ext;

    if (!taskbar_pin_path(e, lnk)) return;
    if (!on) DeleteFileW(lnk);
    else if ((ext = wcsrchr(e->path, L'.')) && !lstrcmpiW(ext, L".lnk")) CopyFileW(e->path, lnk, FALSE);
    else
    {
        IShellLinkW *link;
        IPersistFile *file;
        if (SUCCEEDED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link)))
        {
            IShellLinkW_SetPath(link, e->path);
            if (e->args[0]) IShellLinkW_SetArguments(link, e->args);
            if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file)))
            {
                IPersistFile_Save(file, lnk, TRUE);
                IPersistFile_Release(file);
            }
            IShellLinkW_Release(link);
        }
    }
    taskbar_order(wcsrchr(lnk, L'\\') + 1, on);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 2000, &r);
}

/* "Run with debugging" (David 2026-09-29: a mode the app is started in, as
 * Run as administrator is): Report a problem runs it with Wine's debug log
 * and, when it ends or crashes, shows what a developer needs, to be sent on */
static void run_debugging(const struct entry *e)
{
    WCHAR tool[MAX_PATH], params[MAX_PATH + 160], *slash;

    GetModuleFileNameW(NULL, tool, MAX_PATH);
    if (!(slash = wcsrchr(tool, '\\'))) return;
    lstrcpyW(slash + 1, L"sg-bugreport64.exe");
    _snwprintf(params, ARRAYSIZE(params), L"--run \"%ls\"%ls%ls", e->path, e->args[0] ? L" " : L"", e->args);
    _snwprintf(g_launched, ARRAYSIZE(g_launched), L"debug %ls", e->name);
    show_panel(FALSE);
    ShellExecuteW(NULL, NULL, tool, params, NULL, SW_SHOWNORMAL);
}

static void entry_menu(int id, POINT pt)
{
    enum { C_PIN = 1, C_RUNAS, C_LOCATION, C_UNINSTALL, C_TASKBAR, C_DEBUG };
    struct entry *e = entry_of(id);
    HMENU m;
    BOOL pinned, on_taskbar;
    int cmd;

    if (!e) return;
    pinned = pin_index(e->name) >= 0;
    on_taskbar = taskbar_pinned(e);
    m = menu_new();
    if (e->kind == K_APP)
    {
        menu_add(m, C_PIN, pinned ? L"Un&pin from Start" : L"&Pin to Start");
        menu_add(m, C_TASKBAR, on_taskbar ? L"Unpin from tas&kbar" : L"Pin to tas&kbar");
        menu_add(m, 0, NULL);
        menu_add(m, C_RUNAS, L"Run as &administrator");
        menu_add(m, C_DEBUG, L"Run with &debugging");
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
    case C_TASKBAR:   taskbar_pin(e, !on_taskbar); break;
    case C_RUNAS:     run_entry(e, e->kind == K_APP ? L"runas" : NULL); break;
    case C_DEBUG:     run_debugging(e); break;
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
    case RAIL_MENU:
        /* the app list is off: the menu button is "All apps" */
        if (!g_app_list && !g_search[0]) { show_list(!g_show_list); InvalidateRect(g_panel, NULL, FALSE); break; }
        g_rail_open = !g_rail_open; InvalidateRect(g_panel, NULL, FALSE); break;
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
    draw_glyph(dc, RAIL_SETTINGS, x + size / 2, cy, COL_ON_ACCENT);
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
    text(dc, initial, r, g_font_bold, COL_ON_ACCENT, DT_CENTER | DT_VCENTER);
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
        fill(dc, &edge, g_pal->edge);
    }
}

static void draw_list(HDC dc)
{
    RECT clip = { list_left(), list_top(), list_left() + list_w(), list_bottom() };
    HRGN rgn;
    int i;

    if (!g_show_list) return;   /* the app list is off */
    rgn = CreateRectRgnIndirect(&clip);

    SelectClipRgn(dc, rgn);
    if (g_search[0] && !g_centered)
    {
        /* the search field, where the typing goes */
        int ft = g_xp ? list_top() : S(TOP_PAD);
        RECT field = { list_left() + S(12), ft, list_left() + list_w() - S(12), ft + S(SEARCH_H) }, t;
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
        RECT r = { list_left() + S(4), y, list_left() + list_w() - S(8), y + rw->h }, t;

        if (r.bottom < list_top() || r.top > list_bottom()) continue;
        if (g_search[0] && !g_centered && r.top < (g_xp ? list_top() : S(TOP_PAD)) + S(SEARCH_H)) continue;
        if (rw->type == R_HEADER)
        {
            t = r; t.left += S(12);
            text(dc, rw->label, t, g_search[0] ? g_font_small : g_font_bold,
                 rw->label[1] ? COL_TEXT : COL_SUBTLE, DT_LEFT | DT_VCENTER);
            continue;
        }
        if (i == g_hot_row) fill(dc, &r, COL_HOVER);
        if (i == g_sel_row) frame(dc, &r, COL_SUBTLE, S(1) + (g_dpi >= 144));
        if (rw->type == R_BEST && i != g_hot_row) fill(dc, &r, g_pal->best);
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
        RECT b = { list_left() + list_w() - S(5), top, list_left() + list_w() - S(2), top + bar };
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
            draw_glyph_scaled(dc, RAIL_SETTINGS, (gr.left + gr.right) / 2, (gr.top + gr.bottom) / 2, COL_ON_ACCENT, 2);
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
            SetTextColor(dc, COL_ON_ACCENT);
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

/* ---- the centred layout ---------------------------------------------------------- */

static RECT c_search_rect(void) { RECT r = { S(32), S(C_SEARCH_Y), g_panel_w - S(32), S(C_SEARCH_Y + C_SEARCH_H) }; return r; }
static RECT c_more_rect(void) { RECT r = { g_panel_w - S(150), S(C_HEAD_Y), g_panel_w - S(32), S(C_HEAD_Y) + S(28) }; return r; }
static int c_grid_left(void) { return (g_panel_w - C_COLS * S(C_CELL_W)) / 2; }
static RECT c_pin_rect(int i)
{
    RECT r;
    int col = i % C_COLS, row = i / C_COLS;
    SetRect(&r, c_grid_left() + col * S(C_CELL_W), S(C_BODY_Y) + row * S(C_CELL_H),
            c_grid_left() + (col + 1) * S(C_CELL_W), S(C_BODY_Y) + (row + 1) * S(C_CELL_H));
    return r;
}
static int c_rec_top(void) { return S(C_BODY_Y) + C_ROWS * S(C_CELL_H) + S(44); }
static RECT c_rec_rect(int i)
{
    RECT r;
    int w = (g_panel_w - S(64)) / 2, col = i % 2, row = i / 2;
    SetRect(&r, S(32) + col * w, c_rec_top() + row * S(C_REC_H), S(32) + (col + 1) * w, c_rec_top() + (row + 1) * S(C_REC_H));
    return r;
}
static RECT c_bar_rect(void) { RECT r = { 0, g_panel_h - S(C_BAR_H), g_panel_w, g_panel_h }; return r; }
static RECT c_user_rect(void) { RECT b = c_bar_rect(), r = { S(24), b.top + S(12), S(280), b.bottom - S(12) }; return r; }
static RECT c_power_rect(void) { RECT b = c_bar_rect(), r = { g_panel_w - S(72), b.top + S(12), g_panel_w - S(24), b.bottom - S(12) }; return r; }

/* the most used apps (Start's own count of launches), not already pinned */
static void c_recommend(void)
{
    int i, j, k;
    DWORD best[6];
    g_nrec = 0;
    if (!track_progs()) return;
    for (i = 0; i < g_napps; i++)
    {
        DWORD u = usage_of(g_apps[i].name);
        if (!u || pin_index(g_apps[i].name) >= 0) continue;
        for (j = 0; j < g_nrec && best[j] >= u; j++);
        if (j >= 6) continue;
        for (k = min(g_nrec, 5); k > j; k--) { best[k] = best[k - 1]; g_rec[k] = g_rec[k - 1]; }
        best[j] = u; g_rec[j] = i;
        if (g_nrec < 6) g_nrec++;
    }
}

static int c_hit(POINT pt)
{
    RECT r = c_search_rect();
    int i;
    if (PtInRect(&r, pt)) return C_SEARCH;
    r = c_user_rect(); if (PtInRect(&r, pt)) return C_USER;
    r = c_power_rect(); if (PtInRect(&r, pt)) return C_POWER;
    if (g_show_list) return -1;
    r = c_more_rect(); if (PtInRect(&r, pt)) return C_MORE;
    for (i = 0; i < min(g_npins, C_COLS * C_ROWS); i++)
    {
        r = c_pin_rect(i);
        if (tile_app(i) >= 0 && PtInRect(&r, pt)) return C_PIN + i;
    }
    for (i = 0; i < g_nrec; i++)
    {
        r = c_rec_rect(i);
        if (PtInRect(&r, pt)) return C_REC + i;
    }
    return -1;
}

static void draw_entry_icon(HDC dc, const struct entry *e, int x, int y, int size)
{
    if (e->kind == K_SETTING || !e->icon || is_control_panel(e)) draw_badge(dc, x, y + size / 2, size);
    else DrawIconEx(dc, x, y, e->icon, size, size, 0, NULL, DI_NORMAL);
}

static void draw_centered(HDC dc)
{
    RECT r = c_search_rect(), t, bar = c_bar_rect();
    int i;

    /* the search box: always there, as the newer Start's is */
    fill(dc, &r, COL_FIELD);
    frame(dc, &r, g_search[0] ? COL_ACCENT_BR : g_pal->edge, S(1));
    t = r; t.left += S(14);
    if (g_search[0])
    {
        SIZE sz;
        text(dc, g_search, t, g_font, COL_TEXT, DT_LEFT | DT_VCENTER);
        SelectObject(dc, g_font);
        GetTextExtentPoint32W(dc, g_search, (int)wcslen(g_search), &sz);
        { RECT caret = { t.left + sz.cx + S(1), r.top + S(9), t.left + sz.cx + S(2), r.bottom - S(9) }; fill(dc, &caret, COL_TEXT); }
    }
    else text(dc, L"Type here to search", t, g_font, COL_SUBTLE, DT_LEFT | DT_VCENTER);

    if (g_show_list)
    {
        RECT head = { S(40), S(C_HEAD_Y), g_panel_w - S(32), S(C_HEAD_Y) + S(28) };
        text(dc, g_search[0] ? L"Results" : L"All apps", head, g_font_bold, COL_TEXT, DT_LEFT | DT_VCENTER);
        draw_list(dc);
    }
    else
    {
        RECT head = { S(40), S(C_HEAD_Y), S(300), S(C_HEAD_Y) + S(28) }, more = c_more_rect();
        text(dc, L"Pinned", head, g_font_bold, COL_TEXT, DT_LEFT | DT_VCENTER);
        if (g_hot_c == C_MORE) fill(dc, &more, COL_HOVER);
        text(dc, L"All apps  \x203A", more, g_font, COL_TEXT, DT_CENTER | DT_VCENTER);
        for (i = 0; i < min(g_npins, C_COLS * C_ROWS); i++)
        {
            int a = tile_app(i);
            RECT c = c_pin_rect(i), label;
            if (a < 0) continue;
            InflateRect(&c, -S(4), -S(2));
            if (g_hot_c == C_PIN + i) fill(dc, &c, COL_HOVER);
            if (g_sel_tile == i) frame(dc, &c, COL_SUBTLE, S(1));
            draw_entry_icon(dc, &g_apps[a], (c.left + c.right - S(32)) / 2, c.top + S(10), S(32));
            label = c; label.top += S(48); label.left += S(4); label.right -= S(4);
            text(dc, g_apps[a].name, label, g_font_small, COL_TEXT, DT_CENTER | DT_TOP | DT_END_ELLIPSIS | DT_SINGLELINE);
        }
        if (!g_npins)
        {
            RECT none = { S(40), S(C_BODY_Y), g_panel_w - S(40), S(C_BODY_Y) + S(40) };
            text(dc, L"Right-click an app in All apps and choose Pin to Start.", none, g_font, COL_SUBTLE, DT_LEFT | DT_VCENTER);
        }
        head.top = c_rec_top() - S(36); head.bottom = head.top + S(28);
        text(dc, L"Recommended", head, g_font_bold, COL_TEXT, DT_LEFT | DT_VCENTER);
        for (i = 0; i < g_nrec; i++)
        {
            const struct entry *e = &g_apps[g_rec[i]];
            RECT c = c_rec_rect(i), name, sub;
            InflateRect(&c, -S(4), -S(2));
            if (g_hot_c == C_REC + i) fill(dc, &c, COL_HOVER);
            draw_entry_icon(dc, e, c.left + S(10), (c.top + c.bottom - S(32)) / 2, S(32));
            name = c; name.left += S(54); name.right -= S(6); name.bottom = (c.top + c.bottom) / 2 + S(2);
            sub = name; sub.top = name.bottom; sub.bottom = c.bottom;
            text(dc, e->name, name, g_font, COL_TEXT, DT_LEFT | DT_BOTTOM | DT_END_ELLIPSIS | DT_SINGLELINE);
            text(dc, L"Frequently used", sub, g_font_small, COL_SUBTLE, DT_LEFT | DT_TOP | DT_SINGLELINE);
        }
        if (!g_nrec)
        {
            RECT none = { S(40), c_rec_top(), g_panel_w - S(40), c_rec_top() + S(40) };
            text(dc, L"The apps you use most will show here.", none, g_font, COL_SUBTLE, DT_LEFT | DT_VCENTER);
        }
    }

    /* the user, and power, along the bottom */
    fill(dc, &bar, COL_RAIL);
    r = c_user_rect();
    if (g_hot_c == C_USER) fill(dc, &r, COL_HOVER);
    draw_avatar(dc, r.left + S(24), (r.top + r.bottom) / 2);
    t = r; t.left += S(48);
    text(dc, user_name(), t, g_font, COL_TEXT, DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS | DT_SINGLELINE);
    r = c_power_rect();
    if (g_hot_c == C_POWER) fill(dc, &r, COL_HOVER);
    draw_glyph(dc, RAIL_POWER, (r.left + r.right) / 2, (r.top + r.bottom) / 2, COL_TEXT);
}

static HFONT make_font(int pt10, int weight);

/* ---- the Horizon look's Start (X_W) ------------------------------------------------ */

struct x_place { const WCHAR *label; int csidl; const WCHAR *cmd, *args; BOOL bold; int icon; };
/* icon: shell32's resource, when the place is not a folder */
static const struct x_place x_places[] = {
    { L"Documents",  CSIDL_PERSONAL,    NULL, NULL, TRUE, 0 },
    { L"Pictures",   CSIDL_MYPICTURES,  NULL, NULL, TRUE, 0 },
    { L"Music",      CSIDL_MYMUSIC,     NULL, NULL, TRUE, 0 },
    { L"This PC",    -1, L"explorer.exe", L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", TRUE, 16 },
    { NULL },
    { L"Control Panel", -1, L"control.exe", NULL, FALSE, 36 },
    { L"Settings",   -1, L"ms-settings:", NULL, FALSE, 22 },
    { L"Default Programs", -1, L"ms-settings:defaultapps", NULL, FALSE, 14 },
    { L"Devices and Printers", -1, L"control.exe", L"printers", FALSE, 17 },
    { NULL },
    { L"Search",     -1, L"search", NULL, FALSE, 23 },
    { L"Run...",     -1, L"run", NULL, FALSE, 25 },
};
#define X_NPLACES ((int)ARRAYSIZE(x_places))

/* the pinned programs (g_xpins): the web browser and mail first, as
 * "Internet" and "E-mail" with their own names under them, then the user's */
static HFONT g_xfont, g_xfont_bold, g_xfont_name, g_xfont_sub;
static void x_fonts(void);
static BOOL g_xc_open;   /* the All Programs menus are showing */

static HICON x_shell_icon(int id, int size)
{
    HICON icon = NULL;
    WCHAR shell32[MAX_PATH];
    GetSystemDirectoryW(shell32, MAX_PATH);
    lstrcatW(shell32, L"\\shell32.dll");
    if (size > 16) ExtractIconExW(shell32, -id, &icon, NULL, 1);
    else ExtractIconExW(shell32, -id, NULL, &icon, 1);
    return icon;
}

static void x_fonts(void)
{
    /* the era's small interface type: 8 pt, the user's name larger and bold */
    if (g_xfont) return;
    g_xfont = make_font(80, FW_NORMAL);
    g_xfont_bold = make_font(80, FW_BOLD);
    g_xfont_sub = make_font(75, FW_NORMAL);
    g_xfont_name = make_font(115, FW_BOLD);
}

static void x_load_pins(void)
{
    static const struct { const WCHAR *label, *scheme; } special[] = { { L"Internet", L"http" }, { L"E-mail", L"mailto" } };
    int i, k;

    for (i = 0; i < g_nxpins; i++) if (g_xpins[i].icon) DestroyIcon(g_xpins[i].icon);
    g_nxpins = 0;
    for (i = 0; i < (int)ARRAYSIZE(special) && g_nxpins < X_NPINS; i++)
    {
        struct x_pin *p = &g_xpins[g_nxpins];
        DWORD len = MAX_PATH, sublen = ARRAYSIZE(p->sub);
        memset(p, 0, sizeof(*p));
        if (FAILED(AssocQueryStringW(0, ASSOCSTR_EXECUTABLE, special[i].scheme, L"open", p->path, &len)) || !p->path[0] ||
            GetFileAttributesW(p->path) == INVALID_FILE_ATTRIBUTES)
            continue;
        /* mail handled by the web browser (or Wine's forwarder to the host's
         * one) is no mail program of its own */
        if (i == 1 && (wcsstr(p->path, L"winebrowser") || (g_nxpins && !lstrcmpiW(p->path, g_xpins[0].path)))) continue;
        if (FAILED(AssocQueryStringW(0, ASSOCSTR_FRIENDLYAPPNAME, special[i].scheme, L"open", p->sub, &sublen)) || !p->sub[0])
        {
            const WCHAR *base = wcsrchr(p->path, '\\') ? wcsrchr(p->path, '\\') + 1 : p->path;
            lstrcpynW(p->sub, base, ARRAYSIZE(p->sub));
            if (wcsrchr(p->sub, '.')) *wcsrchr(p->sub, '.') = 0;
        }
        lstrcpynW(p->label, special[i].label, ARRAYSIZE(p->label));
        ExtractIconExW(p->path, 0, &p->icon, NULL, 1);
        if (!p->icon) p->icon = x_shell_icon(i ? 18 : 14, 32);   /* a globe; for mail, the network */
        if (wcsstr(p->path, L"winebrowser")) lstrcpynW(p->sub, L"Web browser", ARRAYSIZE(p->sub));
        p->app = -1;
        g_nxpins++;
    }
    for (k = 0; k < g_npins && g_nxpins < X_NPINS; k++)
    {
        struct x_pin *p = &g_xpins[g_nxpins];
        int app = tile_app(k);
        if (app < 0) continue;
        memset(p, 0, sizeof(*p));
        lstrcpynW(p->label, g_apps[app].name, ARRAYSIZE(p->label));
        p->app = app;
        g_nxpins++;
    }
}

static RECT g_left_rect(void) { RECT r = { S(G_FRAME), S(G_FRAME), S(G_FRAME) + S(G_LEFT_W), g_panel_h - S(G_FOOT) }; return r; }
static RECT x_head_rect(void)
{
    /* Glass: the user's picture, at the top of the right column */
    RECT r = { 0, 0, g_panel_w, S(X_HEAD) };
    if (g_seven) SetRect(&r, g_left_rect().right + (g_panel_w - S(G_FRAME) - g_left_rect().right - S(52)) / 2, S(G_FRAME) + S(4),
                         0, S(G_FRAME) + S(4) + S(52)), r.right = r.left + S(52);
    return r;
}
static RECT x_foot_rect(void) { RECT r = { 0, g_panel_h - S(g_seven ? G_FOOT : X_FOOT), g_panel_w, g_panel_h }; return r; }
static RECT x_right_rect(void)
{
    RECT r = { S(X_LEFT_W), S(X_HEAD) + S(2), g_panel_w - S(X_EDGE), g_panel_h - S(X_FOOT) };
    if (g_seven) SetRect(&r, g_left_rect().right + S(4), S(G_FRAME), g_panel_w - S(G_FRAME), g_panel_h - S(G_FOOT));
    return r;
}
static RECT x_all_rect(void)
{
    RECT r = { S(X_EDGE), g_panel_h - S(X_FOOT) - S(X_ALL_H) - S(3), S(X_LEFT_W), g_panel_h - S(X_FOOT) - S(3) };
    if (g_seven) { RECT l = g_left_rect(); SetRect(&r, l.left + S(4), l.bottom - S(G_ALL_H) - S(4), l.right - S(4), l.bottom - S(4)); }
    return r;
}
static RECT g_search_rect(void)
{
    RECT l = g_left_rect(), r = { l.left + S(8), g_panel_h - S(G_FOOT) + S(10), l.right - S(8), g_panel_h - S(G_FOOT) + S(10) + S(26) };
    return r;
}
static int x_npins(void) { return g_nxpins; }
static RECT x_pin_rect(int i)
{
    RECT r = { S(X_EDGE) + S(3), S(X_HEAD) + S(8) + i * S(X_PIN_H), S(X_LEFT_W) - S(3), S(X_HEAD) + S(8) + (i + 1) * S(X_PIN_H) };
    if (g_seven) { RECT l = g_left_rect(); SetRect(&r, l.left + S(4), l.top + S(6) + i * S(G_ROW), l.right - S(4), l.top + S(6) + (i + 1) * S(G_ROW)); }
    return r;
}
static int x_mfu_top(void)
{
    if (g_seven) return g_left_rect().top + S(6) + x_npins() * S(G_ROW) + (x_npins() ? S(11) : 0);
    return S(X_HEAD) + S(8) + x_npins() * S(X_PIN_H) + (x_npins() ? S(11) : 0);
}
/* as many of the most used as the column holds */
static int x_nmfu(void)
{
    int room = x_all_rect().top - S(11) - x_mfu_top();
    return max(0, min(g_nmfu, room / S(g_seven ? G_ROW : X_MFU_H)));
}
static RECT x_mfu_rect(int i)
{
    RECT r = { S(X_EDGE) + S(3), x_mfu_top() + i * S(X_MFU_H), S(X_LEFT_W) - S(3), x_mfu_top() + (i + 1) * S(X_MFU_H) };
    if (g_seven) SetRect(&r, g_left_rect().left + S(4), x_mfu_top() + i * S(G_ROW), g_left_rect().right - S(4), x_mfu_top() + (i + 1) * S(G_ROW));
    return r;
}
static RECT x_place_rect(int i)
{
    int j, y = S(X_HEAD) + S(8);
    RECT r;
    if (g_seven)
    {
        /* under the picture and the user's name, plain rows */
        RECT c = x_right_rect();
        y = x_head_rect().bottom + S(10) + S(G_PLACE_H);
        for (j = 0; j < i; j++) y += !x_places[j].label ? S(9) : S(G_PLACE_H);
        SetRect(&r, c.left + S(4), y, c.right - S(4), y + S(G_PLACE_H));
        return r;
    }
    for (j = 0; j < i; j++) y += !x_places[j].label ? S(9) : x_places[j].bold ? S(X_PLACE1_H) : S(X_PLACE_H);
    SetRect(&r, S(X_LEFT_W) + S(4), y, g_panel_w - S(X_EDGE) - S(4),
            y + (x_places[i].bold ? S(X_PLACE1_H) : S(X_PLACE_H)));
    return r;
}
/* Glass: the user's name, a row of its own under the picture */
static RECT g_name_rect(void)
{
    RECT c = x_right_rect(), r = { c.left + S(4), x_head_rect().bottom + S(10), c.right - S(4), x_head_rect().bottom + S(10) + S(G_PLACE_H) };
    return r;
}
/* a foot button: its icon, a gap and its label */
static int x_button_w(const WCHAR *label)
{
    HDC dc = GetDC(NULL);
    SIZE sz = { 0 };
    HGDIOBJ old;
    x_fonts();
    old = SelectObject(dc, g_xfont);
    GetTextExtentPoint32W(dc, label, lstrlenW(label), &sz);
    SelectObject(dc, old);
    ReleaseDC(NULL, dc);
    return S(2) + S(24) + S(5) + sz.cx + S(6);
}
static RECT x_turnoff_rect(void)
{
    RECT f = x_foot_rect(), r = { g_panel_w - S(8) - x_button_w(L"Turn Off Computer"), f.top + S(7), g_panel_w - S(8), f.bottom - S(7) };
    if (g_seven)
    {
        /* Glass: "Shut down", in the right column's foot, its arrow beside it */
        RECT c = x_right_rect();
        int w = S(84), total = w + S(24);
        SetRect(&r, (c.left + c.right - total) / 2, f.top + S(10), (c.left + c.right - total) / 2 + w, f.top + S(10) + S(26));
    }
    return r;
}
static RECT x_logoff_rect(void)
{
    RECT t = x_turnoff_rect(), r = { t.left - S(6) - x_button_w(L"Log Off"), t.top, t.left - S(6), t.bottom };
    if (g_seven) SetRect(&r, t.right, t.top, t.right + S(24), t.bottom);   /* the arrow: the other power choices */
    return r;
}

/* the most used programs (Start's count of launches) not pinned; with none
 * counted yet, the first programs by name, so the column is never empty */
static void x_most_used(void)
{
    int i, j, k;
    DWORD best[X_NMFU];
    g_nmfu = 0;
    for (i = 0; i < g_napps; i++)
    {
        DWORD u = track_progs() ? usage_of(g_apps[i].name) : 0;
        if (!u || pin_index(g_apps[i].name) >= 0) continue;
        for (j = 0; j < g_nmfu && best[j] >= u; j++);
        if (j >= X_NMFU) continue;
        for (k = min(g_nmfu, X_NMFU - 1); k > j; k--) { best[k] = best[k - 1]; g_mfu[k] = g_mfu[k - 1]; }
        best[j] = u; g_mfu[j] = i;
        if (g_nmfu < X_NMFU) g_nmfu++;
    }
    if (!g_nmfu)
    {
        /* nothing counted yet (a new user): the everyday programs, as a new
         * profile's menu had them, then others by name */
        static const WCHAR *const usual[] = { L"Notepad", L"Paint", L"Calculator", L"Command Prompt",
                                              L"Media Player", L"WordPad", L"Terminal", L"Photos" };
        for (k = 0; k < (int)ARRAYSIZE(usual) && g_nmfu < X_NMFU; k++)
            if ((i = find_app(usual[k])) >= 0 && pin_index(usual[k]) < 0) g_mfu[g_nmfu++] = i;
        for (i = 0; i < g_napps && g_nmfu < X_NMFU; i++)
        {
            if (g_apps[i].kind != K_APP || pin_index(g_apps[i].name) >= 0) continue;
            for (j = 0; j < g_nmfu && g_mfu[j] != i; j++);
            if (j == g_nmfu) g_mfu[g_nmfu++] = i;
        }
    }
}

static int x_hit(POINT pt)
{
    RECT r;
    int i;
    r = x_head_rect(); if (PtInRect(&r, pt)) return X_USER;
    if (g_seven) { r = g_name_rect(); if (PtInRect(&r, pt)) return X_USER; }
    r = x_logoff_rect(); if ((g_seven || may_sign_out()) && PtInRect(&r, pt)) return X_LOGOFF;
    r = x_turnoff_rect(); if (PtInRect(&r, pt)) return X_TURNOFF;
    r = x_all_rect(); if (PtInRect(&r, pt)) return X_ALL;
    for (i = 0; i < X_NPLACES; i++)
    {
        r = x_place_rect(i);
        if (x_places[i].label && PtInRect(&r, pt)) return X_PLACE + i;
    }
    if (g_show_list) return -1;
    for (i = 0; i < x_npins(); i++) { r = x_pin_rect(i); if (PtInRect(&r, pt)) return X_PIN + i; }
    for (i = 0; i < x_nmfu(); i++) { r = x_mfu_rect(i); if (PtInRect(&r, pt)) return X_MFU + i; }
    return -1;
}

static void x_gradient(HDC dc, const RECT *r, COLORREF top, COLORREF bottom)
{
    TRIVERTEX v[2] = {
        { r->left, r->top, (COLOR16)(GetRValue(top) << 8), (COLOR16)(GetGValue(top) << 8), (COLOR16)(GetBValue(top) << 8), 0 },
        { r->right, r->bottom, (COLOR16)(GetRValue(bottom) << 8), (COLOR16)(GetGValue(bottom) << 8), (COLOR16)(GetBValue(bottom) << 8), 0 } };
    GRADIENT_RECT gr = { 0, 1 };
    GdiGradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}

/* left to right: a, b at the middle, a again */
static void x_hrule(HDC dc, int left, int right, int y, int h, COLORREF ends, COLORREF middle)
{
    TRIVERTEX v[3] = {
        { left, y, (COLOR16)(GetRValue(ends) << 8), (COLOR16)(GetGValue(ends) << 8), (COLOR16)(GetBValue(ends) << 8), 0 },
        { (left + right) / 2, y + h, (COLOR16)(GetRValue(middle) << 8), (COLOR16)(GetGValue(middle) << 8), (COLOR16)(GetBValue(middle) << 8), 0 },
        { right, y, (COLOR16)(GetRValue(ends) << 8), (COLOR16)(GetGValue(ends) << 8), (COLOR16)(GetBValue(ends) << 8), 0 } };
    GRADIENT_RECT gr[2] = { { 0, 1 }, { 1, 2 } };
    v[1].y = y + h;
    GdiGradientFill(dc, v, 2, &gr[0], 1, GRADIENT_FILL_RECT_H);
    v[1].y = y;
    v[2].y = y + h;
    GdiGradientFill(dc, v + 1, 2, &gr[0], 1, GRADIENT_FILL_RECT_H);
}

static void x_place_icon(HDC dc, int i, int x, int y, int size)
{
    SHFILEINFOW sfi = { 0 };
    WCHAR path[MAX_PATH];
    const struct x_place *p = &x_places[i];
    HICON icon = NULL;
    if (p->csidl >= 0 && SHGetFolderPathW(NULL, p->csidl, NULL, 0, path) == S_OK &&
        SHGetFileInfoW(path, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) && sfi.hIcon)
        icon = sfi.hIcon;
    else if (p->cmd && !wcsncmp(p->cmd, L"ms-settings:", 12))
    {
        /* Settings' own picture: the program that opens ms-settings: */
        WCHAR exe[MAX_PATH];
        DWORD len = MAX_PATH;
        if (SUCCEEDED(AssocQueryStringW(0, ASSOCSTR_EXECUTABLE, L"ms-settings", L"open", exe, &len)))
            ExtractIconExW(exe, 0, &icon, NULL, 1);
        if (!icon) icon = x_shell_icon(p->icon, size);
    }
    else if (p->icon) icon = x_shell_icon(p->icon, size);
    if (icon)
    {
        DrawIconEx(dc, x, y, icon, size, size, 0, NULL, DI_NORMAL);
        DestroyIcon(icon);
        return;
    }
    draw_badge(dc, x, y + size / 2, size);
}

/* the user's picture: a landscape of our own -- sky, sun, two hills */
static void x_picture(HDC dc, RECT in)
{
    int w = in.right - in.left, h = in.bottom - in.top;
    HRGN clip = CreateRectRgn(in.left, in.top, in.right, in.bottom);
    HBRUSH b;
    HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
    HGDIOBJ ob;
    SelectClipRgn(dc, clip);
    x_gradient(dc, &in, RGB(0x4A, 0x96, 0xF0), RGB(0xC8, 0xE4, 0xFC));
    b = CreateSolidBrush(RGB(0xFF, 0xD8, 0x40)); ob = SelectObject(dc, b);
    Ellipse(dc, in.left + w * 6 / 10, in.top + h / 8, in.left + w * 6 / 10 + w / 4, in.top + h / 8 + w / 4);
    SelectObject(dc, ob); DeleteObject(b);
    b = CreateSolidBrush(RGB(0x5C, 0xB8, 0x3C)); ob = SelectObject(dc, b);
    Ellipse(dc, in.left - w / 2, in.top + h * 6 / 10, in.left + w * 7 / 10, in.bottom + h / 2);
    SelectObject(dc, ob); DeleteObject(b);
    b = CreateSolidBrush(RGB(0x3C, 0x96, 0x28)); ob = SelectObject(dc, b);
    Ellipse(dc, in.left + w * 3 / 10, in.top + h * 7 / 10, in.right + w / 2, in.bottom + h / 2);
    SelectObject(dc, ob); DeleteObject(b);
    SelectObject(dc, op);
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
}

/* a rounded square, lit at the top, with a white rim */
static void x_tile(HDC dc, RECT box, COLORREF top, COLORREF bottom)
{
    HRGN rgn = CreateRoundRectRgn(box.left, box.top, box.right + 1, box.bottom + 1, S(5), S(5));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xFF, 0xFF, 0xFF)), op;
    HBRUSH ob;
    SelectClipRgn(dc, rgn);
    x_gradient(dc, &box, top, bottom);
    SelectClipRgn(dc, NULL);
    DeleteObject(rgn);
    op = SelectObject(dc, pen); ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, box.left, box.top, box.right, box.bottom, S(5), S(5));
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
}

static void x_button(HDC dc, RECT r, const WCHAR *label, int glyph, BOOL hot)
{
    int sz = S(24);
    RECT box = { r.left + S(2), r.top + (r.bottom - r.top - sz) / 2, r.left + S(2) + sz, r.top + (r.bottom - r.top + sz) / 2 }, t = r;
    if (hot)
    {
        HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, S(6), S(6));
        HBRUSH b = CreateSolidBrush(RGB(0x3C, 0x82, 0xF0));
        FillRgn(dc, rgn, b);
        DeleteObject(b); DeleteObject(rgn);
    }
    if (glyph == RAIL_POWER)
    {
        x_tile(dc, box, RGB(0xF2, 0x8C, 0x60), RGB(0xC8, 0x34, 0x10));
        draw_glyph(dc, glyph, (box.left + box.right) / 2, (box.top + box.bottom) / 2, RGB(0xFF, 0xFF, 0xFF));
    }
    else
    {
        /* log off: a door, and an arrow going out of it */
        RECT door = { box.left + S(5), box.top + S(5), box.left + S(12), box.bottom - S(5) }, shaft;
        POINT tip[3];
        HBRUSH w = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF)), ob;
        HPEN op;
        x_tile(dc, box, RGB(0xFA, 0xD0, 0x50), RGB(0xD8, 0x8C, 0x10));
        ob = SelectObject(dc, w); op = SelectObject(dc, GetStockObject(NULL_PEN));
        frame(dc, &door, RGB(0xFF, 0xFF, 0xFF), S(1));
        SetRect(&shaft, door.left + S(4), (box.top + box.bottom) / 2 - S(1), box.right - S(8), (box.top + box.bottom) / 2 + S(1));
        fill(dc, &shaft, RGB(0xFF, 0xFF, 0xFF));
        tip[0].x = box.right - S(9); tip[0].y = (box.top + box.bottom) / 2 - S(4);
        tip[1].x = box.right - S(9); tip[1].y = (box.top + box.bottom) / 2 + S(4);
        tip[2].x = box.right - S(4); tip[2].y = (box.top + box.bottom) / 2;
        Polygon(dc, tip, 3);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(w);
    }
    t.left = box.right + S(5);
    text(dc, label, t, g_xfont, RGB(0xFF, 0xFF, 0xFF), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

/* a row's highlight: the era's selection blue */
#define X_SEL RGB(0x31, 0x6A, 0xC5)
#define X_NAVY RGB(0x00, 0x15, 0x6E)

static void draw_xp(HDC dc)
{
    RECT head = x_head_rect(), foot = x_foot_rect(), right = x_right_rect(), r, t, line, all;
    int i, e = S(X_EDGE);

    x_fonts();
    /* the frame: the bands' blue down both sides */
    r.left = 0; r.top = 0; r.right = g_panel_w; r.bottom = g_panel_h;
    fill(dc, &r, RGB(0x1C, 0x5A, 0xD6));

    /* the user, on a blue band lit at the top */
    x_gradient(dc, &head, RGB(0x1E, 0x6C, 0xF2), RGB(0x0E, 0x4C, 0xCE));
    line = head; line.bottom = line.top + S(4); x_gradient(dc, &line, RGB(0x8A, 0xBC, 0xFA), RGB(0x2A, 0x74, 0xF0));
    {
        RECT pic = { S(6), (S(X_HEAD) - S(48)) / 2 + S(1), S(6) + S(48), (S(X_HEAD) - S(48)) / 2 + S(1) + S(48) }, shadow = pic, in = pic;
        HRGN rgn;
        HBRUSH b;
        OffsetRect(&shadow, S(2), S(2));
        rgn = CreateRoundRectRgn(shadow.left, shadow.top, shadow.right + 1, shadow.bottom + 1, S(7), S(7));
        b = CreateSolidBrush(RGB(0x0A, 0x32, 0x8C)); FillRgn(dc, rgn, b); DeleteObject(b); DeleteObject(rgn);
        rgn = CreateRoundRectRgn(pic.left, pic.top, pic.right + 1, pic.bottom + 1, S(7), S(7));
        b = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF)); FillRgn(dc, rgn, b); DeleteObject(b); DeleteObject(rgn);
        InflateRect(&in, -S(3), -S(3));
        x_picture(dc, in);
    }
    t = head; t.left = S(64); t.right -= S(8);
    {
        RECT sh = t; OffsetRect(&sh, 1, 1);
        text(dc, user_name(), sh, g_xfont_name, RGB(0x0A, 0x2C, 0x80), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        text(dc, user_name(), t, g_xfont_name, RGB(0xFF, 0xFF, 0xFF), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (g_hot_x == X_USER) { line = head; line.top = head.bottom - S(2); fill(dc, &line, RGB(0x7C, 0xB4, 0xF8)); }
    /* the orange rule: bright in the middle, the band's blue at the ends */
    x_hrule(dc, 0, g_panel_w, head.bottom, S(2), RGB(0x5C, 0x8C, 0xE0), RGB(0xF6, 0x9A, 0x3C));

    /* the places, on pale blue with a blue edge */
    fill(dc, &right, RGB(0xD3, 0xE5, 0xFA));
    line = right; line.right = line.left + S(1); fill(dc, &line, RGB(0x95, 0xBD, 0xEE));
    for (i = 0; i < X_NPLACES; i++)
    {
        r = x_place_rect(i);
        if (!x_places[i].label)
        {
            x_hrule(dc, r.left + S(4), r.right - S(4), r.top + S(4), S(1), RGB(0xD3, 0xE5, 0xFA), RGB(0x94, 0xB8, 0xE8));
            continue;
        }
        if (g_hot_x == X_PLACE + i) fill(dc, &r, X_SEL);
        x_place_icon(dc, i, r.left + S(2), r.top + (r.bottom - r.top - S(24)) / 2, S(24));
        t = r; t.left += S(32);
        text(dc, x_places[i].label, t, x_places[i].bold ? g_xfont_bold : g_xfont,
             g_hot_x == X_PLACE + i ? RGB(0xFF, 0xFF, 0xFF) : X_NAVY, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    /* the programs, on white */
    r.left = e; r.top = S(X_HEAD) + S(2); r.right = S(X_LEFT_W); r.bottom = foot.top;
    fill(dc, &r, RGB(0xFF, 0xFF, 0xFF));
    if (g_show_list) draw_list(dc);
    else
    {
        for (i = 0; i < x_npins(); i++)
        {
            const struct x_pin *pin = &g_xpins[i];
            BOOL hot = g_hot_x == X_PIN + i;
            r = x_pin_rect(i);
            if (hot) fill(dc, &r, X_SEL);
            if (pin->app >= 0) draw_entry_icon(dc, &g_apps[pin->app], r.left + S(4), r.top + (r.bottom - r.top - S(32)) / 2, S(32));
            else if (pin->icon) DrawIconEx(dc, r.left + S(4), r.top + (r.bottom - r.top - S(32)) / 2, pin->icon, S(32), S(32), 0, NULL, DI_NORMAL);
            t = r; t.left += S(42);
            if (pin->sub[0])
            {
                RECT top = t, sub = t;
                top.bottom = (t.top + t.bottom) / 2 + S(1); sub.top = top.bottom - S(1);
                text(dc, pin->label, top, g_xfont_bold, hot ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0), DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
                text(dc, pin->sub, sub, g_xfont_sub, hot ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x7A, 0x7A, 0x7A), DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            else text(dc, pin->label, t, g_xfont_bold, hot ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (x_npins())
            x_hrule(dc, e + S(8), S(X_LEFT_W) - S(8), x_mfu_top() - S(6), S(1), RGB(0xFF, 0xFF, 0xFF), RGB(0xC8, 0xC8, 0xC8));
        for (i = 0; i < x_nmfu(); i++)
        {
            const struct entry *en = &g_apps[g_mfu[i]];
            BOOL hot = g_hot_x == X_MFU + i;
            r = x_mfu_rect(i);
            if (hot) fill(dc, &r, X_SEL);
            draw_entry_icon(dc, en, r.left + S(4), r.top + (r.bottom - r.top - S(32)) / 2, S(32));
            t = r; t.left += S(42);
            text(dc, en->name, t, g_xfont, hot ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    /* All Programs (Back from a search), under a rule, a green arrow after it */
    all = x_all_rect();
    x_hrule(dc, e + S(8), S(X_LEFT_W) - S(8), all.top - S(4), S(1), RGB(0xFF, 0xFF, 0xFF), RGB(0xC8, 0xC8, 0xC8));
    if (g_hot_x == X_ALL || g_xc_open) fill(dc, &all, X_SEL);
    {
        BOOL back = g_show_list || g_search[0];
        const WCHAR *label = back ? L"Back" : L"All Programs";
        SIZE sz;
        int w, x;
        RECT arrow;
        POINT tri[3];
        HBRUSH wb = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF)), ob;
        HPEN op;
        SelectObject(dc, g_xfont_bold);
        GetTextExtentPoint32W(dc, label, lstrlenW(label), &sz);
        w = sz.cx + S(8) + S(20);
        x = (all.left + all.right - w) / 2 + S(10);
        t = all; t.left = x; t.right = x + sz.cx;
        text(dc, label, t, g_xfont_bold, g_hot_x == X_ALL || g_xc_open ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetRect(&arrow, t.right + S(8), (all.top + all.bottom) / 2 - S(10), t.right + S(8) + S(20), (all.top + all.bottom) / 2 + S(10));
        x_tile(dc, arrow, RGB(0x6C, 0xD0, 0x5C), RGB(0x24, 0x8C, 0x1C));
        if (back) { tri[0].x = arrow.right - S(7); tri[0].y = arrow.top + S(5); tri[1].x = arrow.right - S(7); tri[1].y = arrow.bottom - S(5); tri[2].x = arrow.left + S(6); tri[2].y = (arrow.top + arrow.bottom) / 2; }
        else { tri[0].x = arrow.left + S(7); tri[0].y = arrow.top + S(5); tri[1].x = arrow.left + S(7); tri[1].y = arrow.bottom - S(5); tri[2].x = arrow.right - S(6); tri[2].y = (arrow.top + arrow.bottom) / 2; }
        ob = SelectObject(dc, wb); op = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, tri, 3);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(wb);
    }

    /* Log Off and Turn Off Computer, on a blue band */
    x_gradient(dc, &foot, RGB(0x2A, 0x72, 0xEC), RGB(0x10, 0x4A, 0xC8));
    line = foot; line.bottom = line.top + S(1); fill(dc, &line, RGB(0x70, 0xA6, 0xF4));
    if (may_sign_out()) x_button(dc, x_logoff_rect(), L"Log Off", RAIL_USER, g_hot_x == X_LOGOFF);
    x_button(dc, x_turnoff_rect(), L"Turn Off Computer", RAIL_POWER, g_hot_x == X_TURNOFF);
}

static void x_run_pin(int i)
{
    const struct x_pin *pin = &g_xpins[i];
    if (i < 0 || i >= g_nxpins) return;
    if (pin->app >= 0) { run_entry(&g_apps[pin->app], NULL); return; }
    lstrcpynW(g_launched, pin->label, ARRAYSIZE(g_launched));
    show_panel(FALSE);
    if (!lstrcmpW(pin->label, L"E-mail")) ShellExecuteW(NULL, NULL, L"mailto:", NULL, NULL, SW_SHOWNORMAL);
    else ShellExecuteW(NULL, NULL, pin->path, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- All Programs: the Start Menu's programs as cascading menus ------------------- */

#define XC_TAG 0x40000000
#define XC_FIRST 0x7000
struct xc_item { WCHAR name[128], path[MAX_PATH]; BOOL folder; HICON icon; };
static struct xc_item *g_xc;
static int g_nxc, g_capxc;

static int xc_add(const WCHAR *name, const WCHAR *path, BOOL folder)
{
    struct xc_item *grown;
    if (g_nxc == g_capxc)
    {
        int cap = g_capxc ? 2 * g_capxc : 256;
        if (!(grown = realloc(g_xc, cap * sizeof(*g_xc)))) return -1;
        g_xc = grown; g_capxc = cap;
    }
    lstrcpynW(g_xc[g_nxc].name, name, ARRAYSIZE(g_xc[0].name));
    lstrcpynW(g_xc[g_nxc].path, path, MAX_PATH);
    g_xc[g_nxc].folder = folder;
    g_xc[g_nxc].icon = NULL;
    return g_nxc++;
}

static void xc_free(void)
{
    int i;
    for (i = 0; i < g_nxc; i++) if (g_xc[i].icon) DestroyIcon(g_xc[i].icon);
    g_nxc = 0;
}

struct xc_found { WCHAR name[128]; WCHAR path[2][MAX_PATH]; int npaths; BOOL folder; };
static int __cdecl xc_compare(const void *a, const void *b)
{
    const struct xc_found *fa = a, *fb = b;
    if (fa->folder != fb->folder) return fa->folder ? -1 : 1;   /* folders first, as then */
    return lstrcmpiW(fa->name, fb->name);
}

/* one level: the shortcuts and folders of up to two folders (the user's and
 * everyone's Programs), same-named folders as one */
static HMENU xc_build(WCHAR (*dirs)[MAX_PATH], int ndirs, int depth, BOOL root);
static void xc_fill(HMENU menu, WCHAR (*dirs)[MAX_PATH], int ndirs, int depth, BOOL root)
{
    struct xc_found *found = NULL, *grown;
    int nfound = 0, cap = 0, i, j, d;

    for (d = 0; d < ndirs; d++)
    {
        WCHAR pattern[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        if (_snwprintf(pattern, MAX_PATH, L"%s\\*", dirs[d]) < 0) continue;
        if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) continue;
        do {
            WCHAR name[128], *ext;
            BOOL folder = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (fd.cFileName[0] == '.') continue;
            if (folder && root && !lstrcmpiW(fd.cFileName, L"Programs")) continue;
            if (folder && depth >= 4) continue;
            if (!folder)
            {
                if (!(ext = wcsrchr(fd.cFileName, '.')) || (lstrcmpiW(ext, L".lnk") && lstrcmpiW(ext, L".url"))) continue;
                lstrcpynW(name, fd.cFileName, (int)min(128, ext - fd.cFileName + 1));
            }
            else lstrcpynW(name, fd.cFileName, 128);
            for (j = 0; j < nfound; j++)
                if (found[j].folder == folder && !lstrcmpiW(found[j].name, name)) break;
            if (j == nfound)
            {
                if (nfound == cap)
                {
                    cap = cap ? 2 * cap : 32;
                    if (!(grown = realloc(found, cap * sizeof(*found)))) break;
                    found = grown;
                }
                lstrcpyW(found[j].name, name);
                found[j].folder = folder;
                found[j].npaths = 0;
                nfound++;
            }
            if (found[j].npaths < 2)
                _snwprintf(found[j].path[found[j].npaths++], MAX_PATH, L"%s\\%s", dirs[d], fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (nfound) qsort(found, nfound, sizeof(*found), xc_compare);
    for (i = 0; i < nfound; i++)
    {
        int at = xc_add(found[i].name, found[i].path[0], found[i].folder);
        MENUITEMINFOW item;
        if (at < 0) break;
        memset(&item, 0, sizeof(item));
        item.cbSize = sizeof(item);
        item.fMask = MIIM_FTYPE | MIIM_DATA | MIIM_ID;
        item.fType = MFT_OWNERDRAW;
        item.dwItemData = XC_TAG | at;
        item.wID = XC_FIRST + at;
        if (found[i].folder)
        {
            HMENU sub = xc_build(found[i].path, found[i].npaths, depth + 1, FALSE);
            if (!GetMenuItemCount(sub)) { DestroyMenu(sub); continue; }
            item.fMask |= MIIM_SUBMENU;
            item.hSubMenu = sub;
        }
        InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &item);
    }
    free(found);
}

static HMENU xc_build(WCHAR (*dirs)[MAX_PATH], int ndirs, int depth, BOOL root)
{
    HMENU menu = CreatePopupMenu();
    MENUINFO mi;

    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    mi.fMask = MIM_BACKGROUND | MIM_STYLE;
    mi.hbrBack = GetSysColorBrush(COLOR_MENU);
    mi.dwStyle = MNS_CHECKORBMP;
    SetMenuInfo(menu, &mi);
    xc_fill(menu, dirs, ndirs, depth, root);
    return menu;
}

static void xc_measure(MEASUREITEMSTRUCT *mis)
{
    const struct xc_item *it = &g_xc[mis->itemData & ~XC_TAG];
    HDC dc = GetDC(g_panel);
    SIZE sz;
    x_fonts();
    SelectObject(dc, g_xfont);
    GetTextExtentPoint32W(dc, it->name, lstrlenW(it->name), &sz);
    ReleaseDC(g_panel, dc);
    mis->itemWidth = min(sz.cx, S(300)) + S(16) + S(10) + S(24);
    mis->itemHeight = S(22);
}

static void xc_draw(DRAWITEMSTRUCT *dis)
{
    struct xc_item *it = &g_xc[dis->itemData & ~XC_TAG];
    BOOL sel = (dis->itemState & ODS_SELECTED) != 0;
    RECT r = dis->rcItem, t;
    HBRUSH b = CreateSolidBrush(sel ? X_SEL : RGB(0xFF, 0xFF, 0xFF));
    FillRect(dis->hDC, &r, b);
    DeleteObject(b);
    if (!it->icon)
    {
        SHFILEINFOW sfi = { 0 };
        if (SHGetFileInfoW(it->path, 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON)) it->icon = sfi.hIcon;
    }
    if (it->icon) DrawIconEx(dis->hDC, r.left + S(4), r.top + (r.bottom - r.top - S(16)) / 2, it->icon, S(16), S(16), 0, NULL, DI_NORMAL);
    t = r; t.left += S(4) + S(16) + S(6); t.right -= S(14);
    SetBkMode(dis->hDC, TRANSPARENT);
    text(dis->hDC, it->name, t, g_xfont, sel ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

/* the menus, from the All Programs button: to its right, rising from its
 * foot; a choice starts that program and Start closes */
static void x_cascade(void)
{
    WCHAR dirs[2][MAX_PATH];
    int n = 0, cmd, i, count;
    HMENU menu;
    RECT all = x_all_rect();
    POINT pt;

    xc_free();
    /* the shortcuts at the top of the Start Menu folders first, a line under
     * them, then Programs */
    if (SHGetSpecialFolderPathW(NULL, dirs[n], CSIDL_STARTMENU, FALSE)) n++;
    if (SHGetSpecialFolderPathW(NULL, dirs[n], CSIDL_COMMON_STARTMENU, FALSE)) n++;
    menu = xc_build(dirs, n, 0, TRUE);
    if (GetMenuItemCount(menu))
    {
        MENUITEMINFOW sep;
        memset(&sep, 0, sizeof(sep));
        sep.cbSize = sizeof(sep);
        sep.fMask = MIIM_FTYPE;
        sep.fType = MFT_SEPARATOR;
        InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &sep);
    }
    n = 0;
    if (SHGetSpecialFolderPathW(NULL, dirs[n], CSIDL_PROGRAMS, FALSE)) n++;
    if (SHGetSpecialFolderPathW(NULL, dirs[n], CSIDL_COMMON_PROGRAMS, FALSE)) n++;
    xc_fill(menu, dirs, n, 0, FALSE);
    pt.x = all.right - S(4); pt.y = all.bottom;
    ClientToScreen(g_panel, &pt);
    g_menu_items[0] = 0;
    count = GetMenuItemCount(menu);
    for (i = 0; i < count; i++)
    {
        MENUITEMINFOW mii = { sizeof(mii), MIIM_DATA };
        if (!GetMenuItemInfoW(menu, i, TRUE, &mii) || !(mii.dwItemData & XC_TAG)) continue;
        if (g_menu_items[0]) wcsncat(g_menu_items, L",", ARRAYSIZE(g_menu_items) - wcslen(g_menu_items) - 1);
        wcsncat(g_menu_items, g_xc[mii.dwItemData & ~XC_TAG].name, ARRAYSIZE(g_menu_items) - wcslen(g_menu_items) - 1);
    }
    g_menu = L"allprograms";
    g_xc_open = TRUE;
    InvalidateRect(g_panel, NULL, FALSE);
    UpdateWindow(g_panel);
    dump();
    cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_panel, NULL);
    g_menu = NULL;
    g_xc_open = FALSE;
    DestroyMenu(menu);
    if (cmd >= XC_FIRST && cmd - XC_FIRST < g_nxc && !g_xc[cmd - XC_FIRST].folder)
    {
        const struct xc_item *it = &g_xc[cmd - XC_FIRST];
        lstrcpynW(g_launched, it->name, ARRAYSIZE(g_launched));
        count_launch(it->name);
        show_panel(FALSE);
        ShellExecuteW(NULL, NULL, it->path, NULL, NULL, SW_SHOWNORMAL);
        return;
    }
    InvalidateRect(g_panel, NULL, FALSE);
    dump();
}

/* ---- the Glass look's Start (G_W) --------------------------------------------- */

static void g_round_fill(HDC dc, RECT r, int radius, COLORREF fill, COLORREF edge)
{
    HBRUSH b = CreateSolidBrush(fill), ob = SelectObject(dc, b);
    HPEN p = CreatePen(PS_SOLID, 1, edge), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

/* a glossy button: light above its middle, darker below, a dark edge */
static void g_gloss(HDC dc, RECT r, BOOL hot)
{
    RECT top = r, bottom = r;
    HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, S(5), S(5));
    HPEN p = CreatePen(PS_SOLID, 1, RGB(0x2C, 0x40, 0x5A)), op;
    HBRUSH ob;
    top.bottom = bottom.top = (r.top + r.bottom) / 2;
    SelectClipRgn(dc, rgn);
    x_gradient(dc, &top, hot ? RGB(0xF0, 0xF8, 0xFF) : RGB(0xEA, 0xF0, 0xF8), hot ? RGB(0xD6, 0xEC, 0xFC) : RGB(0xD2, 0xDC, 0xEA));
    x_gradient(dc, &bottom, hot ? RGB(0xA8, 0xD6, 0xF6) : RGB(0xB4, 0xC4, 0xD8), hot ? RGB(0xC4, 0xE4, 0xFA) : RGB(0xC6, 0xD4, 0xE6));
    SelectClipRgn(dc, NULL);
    DeleteObject(rgn);
    op = SelectObject(dc, p); ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, S(5), S(5));
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(p);
}

static void g_triangle(HDC dc, int cx, int cy, int size, BOOL left, COLORREF c)
{
    POINT t[3];
    HBRUSH b = CreateSolidBrush(c), ob = SelectObject(dc, b);
    HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
    if (left) { t[0].x = cx + size / 2; t[0].y = cy - size; t[1].x = cx + size / 2; t[1].y = cy + size; t[2].x = cx - size / 2; t[2].y = cy; }
    else { t[0].x = cx - size / 2; t[0].y = cy - size; t[1].x = cx - size / 2; t[1].y = cy + size; t[2].x = cx + size / 2; t[2].y = cy; }
    Polygon(dc, t, 3);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
}

static void draw_seven(HDC dc)
{
    RECT all = { 0, 0, g_panel_w, g_panel_h }, left = g_left_rect(), r, t, line;
    int i;

    x_fonts();
    /* the glass: deep blue, lit along the top, a pale rim */
    x_gradient(dc, &all, RGB(0x3E, 0x5E, 0x86), RGB(0x1A, 0x2C, 0x44));
    line = all; line.bottom = S(24); x_gradient(dc, &line, RGB(0x6C, 0x8C, 0xB4), RGB(0x40, 0x60, 0x88));
    frame(dc, &all, RGB(0xA4, 0xBA, 0xD6), 1);
    r = all; InflateRect(&r, -1, -1); frame(dc, &r, RGB(0x24, 0x38, 0x52), 1);

    /* the programs, on white */
    g_round_fill(dc, left, S(6), RGB(0xFF, 0xFF, 0xFF), RGB(0x30, 0x48, 0x66));
    if (g_show_list) draw_list(dc);
    else
    {
        for (i = 0; i < x_npins(); i++)
        {
            const struct x_pin *pin = &g_xpins[i];
            BOOL hot = g_hot_x == X_PIN + i;
            r = x_pin_rect(i);
            if (hot) g_round_fill(dc, r, S(5), RGB(0xE4, 0xF2, 0xFC), RGB(0x9C, 0xC8, 0xEC));
            if (pin->app >= 0) draw_entry_icon(dc, &g_apps[pin->app], r.left + S(6), r.top + (r.bottom - r.top - S(32)) / 2, S(32));
            else if (pin->icon) DrawIconEx(dc, r.left + S(6), r.top + (r.bottom - r.top - S(32)) / 2, pin->icon, S(32), S(32), 0, NULL, DI_NORMAL);
            t = r; t.left += S(46);
            text(dc, pin->sub[0] && pin->app < 0 ? pin->sub : pin->label, t, g_font, RGB(0, 0, 0), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (x_npins())
            x_hrule(dc, left.left + S(10), left.right - S(10), x_mfu_top() - S(6), S(1), RGB(0xFF, 0xFF, 0xFF), RGB(0xC8, 0xD0, 0xDA));
        for (i = 0; i < x_nmfu(); i++)
        {
            const struct entry *en = &g_apps[g_mfu[i]];
            BOOL hot = g_hot_x == X_MFU + i;
            r = x_mfu_rect(i);
            if (hot) g_round_fill(dc, r, S(5), RGB(0xE4, 0xF2, 0xFC), RGB(0x9C, 0xC8, 0xEC));
            draw_entry_icon(dc, en, r.left + S(6), r.top + (r.bottom - r.top - S(32)) / 2, S(32));
            t = r; t.left += S(46);
            text(dc, en->name, t, g_font, RGB(0, 0, 0), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    /* All Programs, or Back, in the white column's foot */
    r = x_all_rect();
    x_hrule(dc, left.left + S(10), left.right - S(10), r.top - S(3), S(1), RGB(0xFF, 0xFF, 0xFF), RGB(0xC8, 0xD0, 0xDA));
    if (g_hot_x == X_ALL) g_round_fill(dc, r, S(5), RGB(0xE4, 0xF2, 0xFC), RGB(0x9C, 0xC8, 0xEC));
    {
        BOOL back = g_show_list || g_search[0];
        g_triangle(dc, r.left + S(14), (r.top + r.bottom) / 2, S(4), back, RGB(0x30, 0x30, 0x30));
        t = r; t.left += S(26);
        text(dc, back ? L"Back" : L"All Programs", t, g_font, RGB(0, 0, 0), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    /* the search box, under the white column */
    r = g_search_rect();
    g_round_fill(dc, r, S(5), RGB(0xFF, 0xFF, 0xFF), RGB(0x8C, 0xA4, 0xC2));
    t = r; t.left += S(8); t.right -= S(26);
    if (g_search[0]) text(dc, g_search, t, g_font, RGB(0, 0, 0), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    else text(dc, L"Search programs and files", t, g_xfont_sub, RGB(0x80, 0x80, 0x80), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    {
        /* a magnifier at its right end */
        int cx = r.right - S(14), cy = (r.top + r.bottom) / 2 - S(1);
        HPEN p = CreatePen(PS_SOLID, S(2), RGB(0x50, 0x68, 0x88)), op = SelectObject(dc, p);
        HBRUSH ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, cx - S(5), cy - S(5), cx + S(3), cy + S(3));
        MoveToEx(dc, cx + S(2), cy + S(2), NULL); LineTo(dc, cx + S(6), cy + S(6));
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(p);
    }

    /* the user's picture over the glass column, then the places, plain text */
    {
        RECT pic = x_head_rect(), in = pic;
        g_round_fill(dc, pic, S(8), RGB(0xF4, 0xF8, 0xFC), RGB(0x20, 0x34, 0x50));
        InflateRect(&in, -S(4), -S(4));
        x_picture(dc, in);
    }
    r = g_name_rect();
    if (g_hot_x == X_USER) g_round_fill(dc, r, S(5), RGB(0x4E, 0x6E, 0x96), RGB(0x9C, 0xBC, 0xE4));
    t = r; t.left += S(10);
    text(dc, user_name(), t, g_xfont_bold, RGB(0xFF, 0xFF, 0xFF), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    for (i = 0; i < X_NPLACES; i++)
    {
        r = x_place_rect(i);
        if (!x_places[i].label)
        {
            x_hrule(dc, r.left + S(6), r.right - S(6), r.top + S(4), S(1), RGB(0x34, 0x4C, 0x6C), RGB(0x6C, 0x88, 0xAC));
            continue;
        }
        if (g_hot_x == X_PLACE + i) g_round_fill(dc, r, S(5), RGB(0x4E, 0x6E, 0x96), RGB(0x9C, 0xBC, 0xE4));
        t = r; t.left += S(10);
        text(dc, x_places[i].label, t, g_font, RGB(0xFF, 0xFF, 0xFF), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    /* Shut down and its arrow */
    r = x_turnoff_rect();
    g_gloss(dc, r, g_hot_x == X_TURNOFF);
    text(dc, L"Shut down", r, g_font, RGB(0x10, 0x18, 0x24), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    r = x_logoff_rect();
    g_gloss(dc, r, g_hot_x == X_LOGOFF);
    g_triangle(dc, (r.left + r.right) / 2, (r.top + r.bottom) / 2, S(4), FALSE, RGB(0x10, 0x18, 0x24));
}

static void x_logoff_menu(POINT pt)
{
    enum { L_LOCK = 1, L_SIGNOUT };
    HMENU m = menu_new();
    int cmd;
    menu_add(m, L_LOCK, L"&Lock");
    if (may_sign_out()) menu_add(m, L_SIGNOUT, L"&Log off");
    cmd = track(m, pt, L"logoff");
    DestroyMenu(m);
    if (!cmd) { dump(); return; }
    show_panel(FALSE);
    if (cmd == L_LOCK) LockWorkStation();
    else if (cmd == L_SIGNOUT && may_sign_out()) ExitWindowsEx(EWX_LOGOFF, 0);
}

/* shell32's Run box (ordinal 61, RunFileDlg), as the shell's own Start opens it */
static DWORD WINAPI x_run_thread(void *arg)
{
    void (WINAPI *run)(HWND, HICON, const WCHAR *, const WCHAR *, const WCHAR *, UINT) =
        (void *)GetProcAddress(GetModuleHandleW(L"shell32.dll"), (const char *)61);
    (void)arg;
    if (run) run(NULL, NULL, NULL, NULL, NULL, 0);
    return 0;
}

static void x_open_place(int i)
{
    const struct x_place *p = &x_places[i];
    lstrcpynW(g_launched, p->label, ARRAYSIZE(g_launched));
    show_panel(FALSE);
    if (p->csidl >= 0) { open_folder(p->csidl); return; }
    if (!lstrcmpW(p->cmd, L"run"))
    {
        /* the Run box, on a thread of its own: Start stays usable while it is open */
        HANDLE t = CreateThread(NULL, 0, x_run_thread, NULL, 0, NULL);
        if (t) CloseHandle(t);
        return;
    }
    if (!lstrcmpW(p->cmd, L"ms-settings:") && (INT_PTR)ShellExecuteW(NULL, NULL, p->cmd, NULL, NULL, SW_SHOWNORMAL) > 32) return;
    ShellExecuteW(NULL, NULL, !lstrcmpW(p->cmd, L"ms-settings:") ? L"control.exe" : p->cmd, p->args, NULL, SW_SHOWNORMAL);
}

static void on_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps), mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, g_panel_w, g_panel_h), oldbmp = SelectObject(mem, bmp);
    RECT all = { 0, 0, g_panel_w, g_panel_h };

    fill(mem, &all, COL_PANEL);
    SetBkMode(mem, TRANSPARENT);
    if (g_xp && g_seven) draw_seven(mem);
    else if (g_xp) draw_xp(mem);
    else if (g_centered) draw_centered(mem);
    else
    {
        draw_list(mem);
        draw_tiles(mem);
        draw_rail(mem);   /* last: the open rail lies over the list */
        frame(mem, &all, g_pal->edge, S(1));
    }
    if (g_look_frame) frame(mem, &all, g_pal->edge, S(g_look_frame));
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
    if (pt.x < list_left() || pt.x >= list_left() + list_w()) return -1;
    if (pt.y < list_top() || pt.y >= list_bottom()) return -1;
    if (g_search[0] && !g_centered && pt.y < (g_xp ? list_top() : S(TOP_PAD)) + S(SEARCH_H)) return -1;
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

static int max_scroll(void) { return max(0, g_content_h + S(8) - list_bottom()); }

static void scroll_to(int y)
{
    g_scroll = min(max(0, y), max_scroll());
}

static void ensure_visible(int row)
{
    int top = g_centered ? list_top() : g_search[0] ? (g_xp ? list_top() : S(TOP_PAD)) + S(SEARCH_H) + S(8) : g_xp ? list_top() : 0;
    if (row < 0) return;
    if (g_rows[row].y - g_scroll < top) scroll_to(g_rows[row].y - top);
    else if (g_rows[row].y + g_rows[row].h - g_scroll > list_bottom()) scroll_to(g_rows[row].y + g_rows[row].h - list_bottom() + S(8));
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
    show_list(g_search[0] != 0);
    build_rows();
    g_scroll = 0;
    g_hot_row = -1;
    g_sel_tile = -1;
    g_sel_row = g_search[0] ? first_item() : -1;
    if (g_sel_row >= 0 && g_rows[g_sel_row].type == R_HEADER) g_sel_row = -1;
    InvalidateRect(g_panel, NULL, FALSE);
}

static void load_start_settings(void)
{
    g_app_list = reg_value(START_KEY, L"ShowAppList", 1) != 0;
    g_show_list = g_app_list;
    g_fullscreen = reg_value(START_KEY, L"FullScreen", 0) != 0;
    g_tile_cols = reg_value(START_KEY, L"MoreTiles", 0) ? 4 : 3;
    g_centered = reg_value(START_KEY, L"Centered", 0) != 0;
    g_seven = !g_centered && reg_value(L"Software\\Stained Glass\\Taskbar", L"Style", 0) == 2;
    g_xp = !g_centered && (reg_value(L"Software\\Stained Glass\\Taskbar", L"Style", 0) == 1 || g_seven);
    if (g_xp)
    {
        g_app_list = FALSE;          /* the list shows for a search and All Programs */
        g_show_list = FALSE;
        g_fullscreen = FALSE;
    }
    if (g_centered)
    {
        g_app_list = FALSE;          /* the list shows for a search and All apps */
        g_show_list = FALSE;
        g_fullscreen = FALSE;
        g_tile_cols = C_COLS;        /* the arrow keys move over the grid */
    }
}

/* Start's place: beside the Start button, on whichever edge the taskbar is
 * (SHAppBarMessage says where, even while it hides itself), or all of the
 * screen but the taskbar with "Use Start full screen" */
/* where the taskbar is: asked once, and again when Settings changes it or
 * the screen changes (a round trip to the shell at every opening made Start
 * four times slower to open) */
static APPBARDATA g_bar;
static BOOL g_bar_known;

static void query_taskbar(void)
{
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    memset(&g_bar, 0, sizeof(g_bar));
    g_bar.cbSize = sizeof(g_bar);
    g_bar.uEdge = ABE_BOTTOM;
    if (!SHAppBarMessage(ABM_GETTASKBARPOS, &g_bar) || (g_bar.rc.bottom - g_bar.rc.top < 2 && g_bar.rc.right - g_bar.rc.left < 2))
        SetRect(&g_bar.rc, 0, sh - S(TASKBAR_H), sw, sh);
    if (g_bar.rc.bottom - g_bar.rc.top <= 1) g_bar.rc.top = sh - S(TASKBAR_H);   /* an older shell's 1 px answer */
    g_bar_known = TRUE;
}

static void layout_panel(void)
{
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN), x, y;
    APPBARDATA abd;
    RECT work = { 0, 0, sw, sh };

    if (!g_bar_known) query_taskbar();
    abd = g_bar;
    switch (abd.uEdge)
    {
    case ABE_TOP:   work.top = abd.rc.bottom; break;
    case ABE_LEFT:  work.left = abd.rc.right; break;
    case ABE_RIGHT: work.right = abd.rc.left; break;
    default:        work.bottom = abd.rc.top; break;
    }
    if (g_centered)
    {
        HRGN round;
        /* the centred panel follows the taskbar's alignment: over a left-aligned
         * Start button when the taskbar is Left, else centred on the screen
         * (Settings > Personalization > Taskbar > Taskbar alignment) */
        BOOL centre_al = reg_value(ADVANCED_KEY, L"TaskbarAl", 1) != 0;
        g_panel_w = min(S(C_W), work.right - work.left);
        g_panel_h = min(S(C_H), work.bottom - work.top - S(12));
        x = !centre_al && (abd.uEdge == ABE_BOTTOM || abd.uEdge == ABE_TOP) ? work.left + S(12)
                                                                            : (work.left + work.right - g_panel_w) / 2;
        y = abd.uEdge == ABE_TOP ? work.top + S(12) : work.bottom - g_panel_h - S(12);
        round = CreateRoundRectRgn(0, 0, g_panel_w + 1, g_panel_h + 1, S(16), S(16));
        SetWindowRgn(g_panel, round, TRUE);
        SetWindowPos(g_panel, HWND_TOPMOST, x, y, g_panel_w, g_panel_h, SWP_NOACTIVATE);
        return;
    }
    if (g_xp)
    {
        /* over the Start button, round at the top as those menus were */
        HRGN round;
        g_panel_w = min(S(g_seven ? G_W : X_W), work.right - work.left);
        g_panel_h = min(S(g_seven ? G_H : X_H), work.bottom - work.top);
        x = abd.uEdge == ABE_RIGHT ? work.right - g_panel_w : work.left;
        y = abd.uEdge == ABE_TOP ? work.top : work.bottom - g_panel_h;
        /* Horizon's round at the top only; Glass's all round */
        round = CreateRoundRectRgn(0, 0, g_panel_w + 1, g_panel_h + (g_seven ? 1 : S(12)), S(12), S(12));
        SetWindowRgn(g_panel, round, TRUE);
        SetWindowPos(g_panel, HWND_TOPMOST, x, y, g_panel_w, g_panel_h, SWP_NOACTIVATE);
        return;
    }
    if (g_fullscreen)
    {
        g_panel_w = work.right - work.left;
        g_panel_h = work.bottom - work.top;
        x = work.left; y = work.top;
    }
    else
    {
        g_panel_w = min(S(RAIL_W) + list_w() + S(GAP) + g_tile_cols * S(TILE) + (g_tile_cols - 1) * S(TILE_GAP) + S(GAP),
                        work.right - work.left);
        g_panel_h = min(S(PANEL_H), work.bottom - work.top);
        x = abd.uEdge == ABE_RIGHT ? work.right - g_panel_w : work.left;
        y = abd.uEdge == ABE_BOTTOM || abd.uEdge > ABE_BOTTOM ? work.bottom - g_panel_h : work.top;
    }
    /* a whole rectangle, not none: a hidden window's region set to none kept
     * the rounded one of the look before (Horizon's) and cut the panel short */
    SetWindowRgn(g_panel, CreateRectRgn(0, 0, g_panel_w, g_panel_h), TRUE);
    SetWindowPos(g_panel, HWND_TOPMOST, x, y, g_panel_w, g_panel_h, SWP_NOACTIVATE);
}

/* with the app list turned off, the list column appears for a search and
 * for "All apps" (the menu button), and goes again */
static void show_list(BOOL on)
{
    if (g_app_list || g_show_list == on) return;
    g_show_list = on;
    layout_panel();
}

static void show_panel(BOOL show)
{
    if (show)
    {
        int sh = GetSystemMetrics(SM_CYSCREEN), sw = GetSystemMetrics(SM_CXSCREEN);
        RECT work;
        QueryPerformanceCounter(&g_open_start);
        g_open_ms = -1;
        g_pal = sg_system_dark() ? &dark_palette : &light_palette;
        switch (reg_value(L"Software\\Stained Glass\\Taskbar", L"Style", 0))
        {
        case 1: g_pal = &horizon_palette; g_look_frame = 0; break;   /* draw_xp draws its own */
        case 2: g_pal = &glass_palette; g_look_frame = 2; break;
        default: g_look_frame = 0; break;
        }
        build_list();
        load_pins();
        g_search[0] = 0;
        g_rail_open = FALSE;
        load_start_settings();   /* before build_rows(): the list/tiles are built for the current layout */
        build_rows();
        g_scroll = 0;
        g_hot_row = g_hot_tile = g_hot_rail = g_sel_row = g_sel_tile = -1;
        g_launched[0] = 0;
        (void)work; (void)sh; (void)sw;
        if (g_centered) c_recommend();
        if (g_xp) { x_most_used(); x_load_pins(); }
        g_hot_c = g_hot_x = -1;
        layout_panel();
        {
            /* "Transparency effects" (Personalization > Colors): Start
             * frosted, as Windows' is -- the desktop's compositor blurs what
             * is below it (wine-sg 0745's __wine_sg_acrylic); the Glass look
             * more, Horizon not at all */
            DWORD on = 1, size = sizeof(on);
            RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"EnableTransparency", RRF_RT_REG_DWORD, NULL, &on, &size);
            g_acrylic = !on || (g_xp && !g_seven) ? 0 : g_pal == &glass_palette ? 72 : 88;
#ifdef SG_MUTANT_NOSTARTFROST
            g_acrylic = 0;
#endif
            if (g_acrylic) SetPropW(g_panel, L"__wine_sg_acrylic", (HANDLE)(UINT_PTR)g_acrylic);
            else RemovePropW(g_panel, L"__wine_sg_acrylic");
            SetWindowPos(g_panel, 0, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
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
        else if (g_centered && g_show_list) { show_list(FALSE); build_rows(); g_scroll = 0; InvalidateRect(g_panel, NULL, FALSE); }
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
            int t = g_sel_tile + (vk == VK_DOWN ? g_tile_cols : -g_tile_cols);
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
        if (g_xp)
        {
            int part = x_hit(pt), row = g_show_list ? row_at(pt) : -1;
            if (part != g_hot_x || row != g_hot_row)
            {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                /* resting on All Programs opens them, as then */
                if (part == X_ALL && !g_show_list && !g_search[0] && !g_seven) SetTimer(hwnd, X_HOVER_TIMER, 450, NULL);
                else KillTimer(hwnd, X_HOVER_TIMER);
                g_hot_x = part; g_hot_row = row;
                InvalidateRect(hwnd, NULL, FALSE);
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        if (g_centered)
        {
            int part = c_hit(pt), row = g_show_list ? row_at(pt) : -1;
            if (part != g_hot_c || row != g_hot_row)
            {
                TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
                g_hot_c = part; g_hot_row = row;
                InvalidateRect(hwnd, NULL, FALSE);
                TrackMouseEvent(&tme);
            }
            return 0;
        }
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
        g_hot_row = g_hot_tile = g_hot_rail = g_hot_c = g_hot_x = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEWHEEL:
        scroll_to(g_scroll - GET_WHEEL_DELTA_WPARAM(wp) * S(ROW_H) * 3 / WHEEL_DELTA);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (g_xp)
        {
            int part = x_hit(pt), row;
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            if (part == X_USER) rail_action(RAIL_USER, screen);
            else if (part == X_LOGOFF && g_seven) power_menu(screen);
            else if (part == X_LOGOFF && may_sign_out()) x_logoff_menu(screen);
            else if (part == X_TURNOFF) power_menu(screen);
            else if (part == X_ALL)
            {
                KillTimer(hwnd, X_HOVER_TIMER);
                if (g_search[0]) { g_search[0] = 0; search_changed(); }
                else if (g_show_list) { show_list(FALSE); InvalidateRect(hwnd, NULL, FALSE); }
                else if (g_seven) { show_list(TRUE); build_rows(); g_scroll = 0; InvalidateRect(hwnd, NULL, FALSE); }   /* in place, as then */
                else x_cascade();
            }
            else if (part >= X_PLACE && part < X_PLACE + X_NPLACES) x_open_place(part - X_PLACE);
            else if (part >= X_MFU && part < X_MFU + x_nmfu()) run_entry(&g_apps[g_mfu[part - X_MFU]], NULL);
            else if (part >= X_PIN && part < X_MFU) x_run_pin(part - X_PIN);
            else if (g_show_list && (row = row_at(pt)) >= 0) run_entry(entry_of(g_rows[row].id), NULL);
            return 0;
        }
        if (g_centered)
        {
            int part = c_hit(pt), row;
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            if (part == C_USER) rail_action(RAIL_USER, screen);
            else if (part == C_POWER) power_menu(screen);
            else if (part == C_MORE) { show_list(TRUE); build_rows(); g_scroll = 0; InvalidateRect(hwnd, NULL, FALSE); }
            else if (part >= C_REC && part < C_REC + g_nrec) run_entry(&g_apps[g_rec[part - C_REC]], NULL);
            else if (part >= C_PIN && part < C_REC) run_entry(&g_apps[tile_app(part - C_PIN)], NULL);
            else if (g_show_list && (row = row_at(pt)) >= 0) run_entry(entry_of(g_rows[row].id), NULL);
            else if (g_show_list && !g_search[0] && pt.y >= S(C_HEAD_Y) && pt.y < S(C_BODY_Y))
            {
                show_list(FALSE);   /* the "All apps" heading: back to Pinned */
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
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
        if (g_xp)
        {
            int part = x_hit(pt), row;
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            if (part >= X_MFU && part < X_MFU + x_nmfu()) entry_menu(g_mfu[part - X_MFU], screen);
            else if (part >= X_PIN && part < X_MFU && g_xpins[part - X_PIN].app >= 0) entry_menu(g_xpins[part - X_PIN].app, screen);
            else if (g_show_list && (row = row_at(pt)) >= 0) entry_menu(g_rows[row].id, screen);
            return 0;
        }
        if (g_centered)
        {
            int part = c_hit(pt), row;
            POINT screen = pt;
            ClientToScreen(hwnd, &screen);
            if (part >= C_REC && part < C_REC + g_nrec) entry_menu(g_rec[part - C_REC], screen);
            else if (part >= C_PIN && part < C_REC) entry_menu(tile_app(part - C_PIN), screen);
            else if (g_show_list && (row = row_at(pt)) >= 0) entry_menu(g_rows[row].id, screen);
            return 0;
        }
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
    case WM_SETTINGCHANGE:
        /* the taskbar moved (Settings > Taskbar) */
        if (lp && !lstrcmpW((const WCHAR *)lp, L"TraySettings")) g_bar_known = FALSE;
        break;
    case WM_DISPLAYCHANGE:
        g_bar_known = FALSE;
        break;
    case WM_MEASUREITEM:
        if (((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_MENU && (((MEASUREITEMSTRUCT *)lp)->itemData & XC_TAG))
        { xc_measure((MEASUREITEMSTRUCT *)lp); return TRUE; }
        if (((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_MENU) { menu_measure((MEASUREITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_DRAWITEM:
        if (((DRAWITEMSTRUCT *)lp)->CtlType == ODT_MENU && (((DRAWITEMSTRUCT *)lp)->itemData & XC_TAG))
        { xc_draw((DRAWITEMSTRUCT *)lp); return TRUE; }
        if (((DRAWITEMSTRUCT *)lp)->CtlType == ODT_MENU) { menu_draw((DRAWITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_TIMER:
        if (wp == X_HOVER_TIMER)
        {
            KillTimer(hwnd, X_HOVER_TIMER);
            if (g_xp && g_hot_x == X_ALL && !g_menu && IsWindowVisible(hwnd)) x_cascade();
            return 0;
        }
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
        /* wparam 1: the taskbar's search -- open, ready for typing */
        if (wp == 1) { if (!IsWindowVisible(g_panel)) show_panel(TRUE); }
        else show_panel(!IsWindowVisible(g_panel));
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
#ifndef SG_MUTANT_NOSTARTSHADOW
    pc.style = CS_DROPSHADOW;   /* a shadow under it, drawn by the desktop's compositor (wine-sg 0744, sg-deskcomp) */
#endif
    RegisterClassW(&pc);

    CreateWindowW(LISTENER_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
    g_panel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, PANEL_CLASS, L"Start",
                              WS_POPUP, 0, 0, S(PANEL_W), S(PANEL_H), NULL, NULL, inst, NULL);
    sg_round_corners(g_panel);   /* the tiles' Start, in the Rounded style (the centred one has its own) */
    /* Linux apps' shortcuts (src/linuxapps): made now and kept up to date
     * while the session lasts, by a program of their own */
    {
        WCHAR self[MAX_PATH], cmdline[MAX_PATH + 16], *slash;
        STARTUPINFOW si;
        PROCESS_INFORMATION pi;
        DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
        memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
        if (n && n < MAX_PATH - 24 && (slash = wcsrchr(self, '\\'))) {
            lstrcpyW(slash + 1, L"sg-linuxapp64.exe");
            _snwprintf(cmdline, ARRAYSIZE(cmdline), L"\"%ls\" --watch", self);
            cmdline[ARRAYSIZE(cmdline) - 1] = 0;
            if (GetFileAttributesW(self) != INVALID_FILE_ATTRIBUTES &&
                CreateProcessW(self, cmdline, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            }
        }
    }
    /* Linux programs take this user's light or dark mode and accent from the
     * start (sg-settingsctl look; Settings tells it again on a change) */
    {
        LONG (WINAPI *spawnvp)(char * const argv[], int wait) =
            (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__wine_unix_spawnvp");
        DWORD accent = reg_value(L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0xFFBE2F7B);   /* ABGR */
        char tool[] = "/usr/bin/sg-settingsctl", verb[] = "look", mode[8], hex[8];
        char *argv[] = { tool, verb, mode, hex, NULL };
        strcpy(mode, reg_value(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", 1) ? "light" : "dark");
        snprintf(hex, sizeof(hex), "%02x%02x%02x", (unsigned)(accent & 0xFF), (unsigned)((accent >> 8) & 0xFF), (unsigned)((accent >> 16) & 0xFF));
        if (spawnvp && GetFileAttributesW(L"\\\\?\\unix\\usr\\bin\\sg-settingsctl") != INVALID_FILE_ATTRIBUTES) spawnvp(argv, FALSE);
    }
    /* warm the list and its icons, so the first opening is quick */
    build_list();
    first_run();
    refresh_sleep_caps();
    query_taskbar();

    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
