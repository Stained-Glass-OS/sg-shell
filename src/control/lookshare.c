/*
 * Share and reset a look (Settings > Personalization > Share & reset).
 *
 * A look is the choices Personalization makes -- the window style, light or
 * dark, the accent, the desktop picture, and how the taskbar and Start are
 * laid out -- not the user's pins, tiles or files. It is saved as a small
 * .sglook file (an INI file) that names each choice, and read back through
 * the same setters the pages use: nothing in a shared file reaches the
 * registry except those choices, checked one by one. "Reset" puts back what
 * a new account starts with: the Classic look and the Stained Glass theme.
 *
 * David 2026-09-29: "a new tab under the personalizations where you can
 * share/export/import your selected settings, or reset to stock settings."
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"
#include "wallpaper.h"

#define SG_START    L"Software\\Stained Glass\\Start"
#define SG_TASKBAR  L"Software\\Stained Glass\\Taskbar"
#define ADVANCED    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced"
#define SEARCH_KEY  L"Software\\Microsoft\\Windows\\CurrentVersion\\Search"
#define PERSONALIZE L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"

/* the layout choices a look carries: [section] name = key\value, each a DWORD
 * kept within 0..max */
static const struct choice { const WCHAR *section, *name, *key, *value; DWORD max; } CHOICES[] = {
    { L"Taskbar", L"Alignment",   ADVANCED,   L"TaskbarAl", 1 },
    { L"Taskbar", L"Combine",     ADVANCED,   L"TaskbarGlomLevel", 2 },
    { L"Taskbar", L"SmallIcons",  ADVANCED,   L"TaskbarSmallIcons", 1 },
    { L"Taskbar", L"Badges",      ADVANCED,   L"TaskbarBadges", 1 },
    { L"Taskbar", L"NoPeek",      ADVANCED,   L"DisablePreviewDesktop", 1 },
    { L"Taskbar", L"TaskView",    ADVANCED,   L"ShowTaskViewButton", 1 },
    { L"Taskbar", L"Search",      SEARCH_KEY, L"SearchboxTaskbarMode", 2 },
    { L"Taskbar", L"Position",    SG_TASKBAR, L"Position", 3 },
    { L"Taskbar", L"AutoHide",    SG_TASKBAR, L"AutoHide", 1 },
    { L"Taskbar", L"Color",       SG_TASKBAR, L"Color", 4 },
    { L"Taskbar", L"Style",       SG_TASKBAR, L"Style", 2 },
    { L"Taskbar", L"Desktops",    SG_TASKBAR, L"ShowDesktops", 1 },
    { L"Start",   L"Centered",    SG_START,   L"Centered", 1 },
    { L"Start",   L"MoreTiles",   SG_START,   L"MoreTiles", 1 },
    { L"Start",   L"AppList",     SG_START,   L"ShowAppList", 1 },
    { L"Start",   L"RecentlyAdded", SG_START, L"ShowRecentlyAdded", 1 },
    { L"Start",   L"FullScreen",  SG_START,   L"FullScreen", 1 },
    { L"Start",   L"MostUsed",    ADVANCED,   L"Start_TrackProgs", 1 },
    { L"Colors",  L"Transparency", PERSONALIZE, L"EnableTransparency", 1 },
};

static BOOL value_of(HKEY root, const WCHAR *key, const WCHAR *value, DWORD *out)
{
    DWORD size = sizeof(*out);
    return !RegGetValueW(root, key, value, RRF_RT_REG_DWORD, NULL, out, &size);
}

static void settings_changed(void)
{
    static const WCHAR *const areas[] = { L"TraySettings", L"ImmersiveColorSet", L"WindowsThemeElement" };
    DWORD_PTR r;
    int i;
    for (i = 0; i < (int)ARRAYSIZE(areas); i++)
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)areas[i], SMTO_ABORTIFHUNG, 2000, &r);
}

static void rgb_text(COLORREF c, WCHAR *out)
{
    _snwprintf(out, 8, L"%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
}

static BOOL parse_rgb_text(const WCHAR *s, COLORREF *c)
{
    WCHAR *end;
    unsigned long v;
    if (lstrlenW(s) != 6) return FALSE;
    v = wcstoul(s, &end, 16);
    if (*end) return FALSE;
    *c = RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
    return TRUE;
}

const WCHAR *look_export(const WCHAR *path)
{
    struct pstate s;
    WCHAR num[16], rgb[8];
    DWORD v;
    int i;

    pers_read(&s);
    DeleteFileW(path);
    if (!WritePrivateProfileStringW(L"Look", L"Format", L"1", path)) return L"the file could not be written";
    WritePrivateProfileStringW(L"Look", L"Style", look_rounded() ? L"rounded" : L"classic", path);
    WritePrivateProfileStringW(L"Look", L"AppsMode", s.apps_light ? L"light" : L"dark", path);
    WritePrivateProfileStringW(L"Look", L"SystemMode", s.system_light ? L"light" : L"dark", path);
    rgb_text(s.accent, rgb);
    WritePrivateProfileStringW(L"Look", L"Accent", rgb, path);
    if (s.solid)
    {
        rgb_text(s.background, rgb);
        WritePrivateProfileStringW(L"Look", L"Background", rgb, path);
    }
    else
    {
        WritePrivateProfileStringW(L"Look", L"Wallpaper", s.source[0] ? s.source : s.wallpaper, path);
        WritePrivateProfileStringW(L"Look", L"Fit", PERS_FIT_NAMES[s.style], path);
    }
    for (i = 0; i < (int)ARRAYSIZE(CHOICES); i++)
    {
        if (!value_of(HKEY_CURRENT_USER, CHOICES[i].key, CHOICES[i].value, &v)) continue;
        _snwprintf(num, ARRAYSIZE(num), L"%lu", v);
        WritePrivateProfileStringW(CHOICES[i].section, CHOICES[i].name, num, path);
    }
    WritePrivateProfileStringW(NULL, NULL, NULL, path);   /* flush */
    return NULL;
}

const WCHAR *look_import(const WCHAR *path)
{
    WCHAR text[MAX_PATH];
    COLORREF c;
    const WCHAR *why = NULL;
    int i, style;

    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return L"the file was not found";
    GetPrivateProfileStringW(L"Look", L"Format", L"", text, ARRAYSIZE(text), path);
    if (lstrcmpW(text, L"1")) return L"this is not a look saved by Stained Glass OS";

    GetPrivateProfileStringW(L"Look", L"Style", L"", text, ARRAYSIZE(text), path);
    if (!lstrcmpiW(text, L"classic") || !lstrcmpiW(text, L"rounded"))
        if (!why) why = look_set_style(!lstrcmpiW(text, L"rounded"));
    GetPrivateProfileStringW(L"Look", L"AppsMode", L"", text, ARRAYSIZE(text), path);
    if (!lstrcmpiW(text, L"light") || !lstrcmpiW(text, L"dark")) pers_set_mode(TRUE, !lstrcmpiW(text, L"light"));
    GetPrivateProfileStringW(L"Look", L"SystemMode", L"", text, ARRAYSIZE(text), path);
    if (!lstrcmpiW(text, L"light") || !lstrcmpiW(text, L"dark")) pers_set_mode(FALSE, !lstrcmpiW(text, L"light"));
    GetPrivateProfileStringW(L"Look", L"Accent", L"", text, ARRAYSIZE(text), path);
    if (parse_rgb_text(text, &c)) pers_set_accent(c);
    GetPrivateProfileStringW(L"Look", L"Background", L"", text, ARRAYSIZE(text), path);
    if (parse_rgb_text(text, &c)) pers_set_background(c);
    else
    {
        WCHAR fit[32];
        GetPrivateProfileStringW(L"Look", L"Wallpaper", L"", text, ARRAYSIZE(text), path);
        GetPrivateProfileStringW(L"Look", L"Fit", L"", fit, ARRAYSIZE(fit), path);
        for (style = 0; style < WP_COUNT; style++) if (!lstrcmpiW(fit, PERS_FIT_NAMES[style])) break;
        if (style == WP_COUNT) style = WP_FILL;
        /* a picture from another machine may not be here: keep ours then */
        if (text[0] && GetFileAttributesW(text) != INVALID_FILE_ATTRIBUTES) pers_set_wallpaper(text, style);
    }
    for (i = 0; i < (int)ARRAYSIZE(CHOICES); i++)
    {
        UINT v = GetPrivateProfileIntW(CHOICES[i].section, CHOICES[i].name, (UINT)-1, path);
        if (v == (UINT)-1 || v > CHOICES[i].max) continue;   /* absent, or not a value this choice takes */
        reg_set_dword(HKEY_CURRENT_USER, CHOICES[i].key, CHOICES[i].value, v);
    }
    settings_changed();
    look_wait();
    return why;
}

const WCHAR *look_reset(void)
{
    const WCHAR *why;
    int i;

    for (i = 0; i < (int)ARRAYSIZE(CHOICES); i++)
        RegDeleteKeyValueW(HKEY_CURRENT_USER, CHOICES[i].key, CHOICES[i].value);
    if ((why = look_apply(LOOK_CLASSIC))) return why;     /* Classic: the taskbar and Start as a new account has them */
    pers_set_mode(TRUE, TRUE);
    pers_set_mode(FALSE, TRUE);
    pers_set_accent(RGB(0x7B, 0x2F, 0xBE));        /* the Stained Glass theme */
    if (GetFileAttributesW(L"Z:\\usr\\share\\stained-glass\\wallpapers\\stained-glass.jpg") != INVALID_FILE_ATTRIBUTES)
        pers_set_wallpaper(L"Z:\\usr\\share\\stained-glass\\wallpapers\\stained-glass.jpg", WP_FILL);
    settings_changed();
    look_wait();
    return NULL;
}
