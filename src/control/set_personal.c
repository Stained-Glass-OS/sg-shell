/* sg-control -- Settings > Personalization: Background, Colors, Lock screen,
 * Themes, Start, Taskbar.
 *
 * Background and Colors are the Control Panel's Personalization (personalize.c)
 * in Windows 10 Settings' layout: the same functions write the same values
 * (TranscodedWallpaper, AccentColor and the accent palette, the light/dark
 * modes) and announce them the same way, so the desktop, the taskbar and
 * every program that reads them follow at once.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"
#include "wallpaper.h"
#include <shlobj.h>
#include <commdlg.h>

static const WCHAR PERSONALIZE[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
static const WCHAR ADVANCED[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
static const WCHAR SG_START[] = L"Software\\Stained Glass\\Start";
static const WCHAR SG_LOCK[] = L"Software\\Stained Glass\\LockScreen";
static const WCHAR SG_WALLPAPERS[] = L"Z:\\usr\\share\\stained-glass\\wallpapers";

static void failed(const WCHAR *why)
{
    WCHAR msg[256];
    if (!why) { refresh_page(); return; }
    _snwprintf(msg, ARRAYSIZE(msg), L"The change could not be made: %ls.", why);
    st_status(msg);
}

static BOOL browse_picture(WCHAR *file)
{
    WCHAR start[MAX_PATH];
    OPENFILENAMEW ofn = { sizeof(ofn) };
    file[0] = 0;
    SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, 0, start);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"Pictures (*.jpg; *.jpeg; *.png; *.bmp; *.gif; *.tif)\0*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = start;
    ofn.lpstrTitle = L"Choose a picture";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    return GetOpenFileNameW(&ofn);
}

/* ---- Background ------------------------------------------------------------------------------- */
enum { CMD_BGTYPE = CMD_PAGE_FIRST + 1, CMD_FIT, CMD_BROWSE, CMD_PIC_FIRST = CMD_PAGE_FIRST + 100,
       CMD_BG_FIRST = CMD_PAGE_FIRST + 200 };
static WCHAR g_pics[12][MAX_PATH];
static int g_npics;

void set_build_background(void)
{
    static const WCHAR *const types[] = { L"Picture", L"Solid color" };
    struct pstate st;
    int y = st_title(L"Background"), i, cols, tw = S(120), th = S(68);
    pers_read(&st);
    g_preview_pic = !st.solid && st.source[0] ? pers_thumb(st.source, S(192), S(108)) : NULL;
    pg_control(L"SgCplPreview", L"", 0, st_x(), y, S(320), S(210), -1);
    y += S(228);
    st_combo(&y, L"Background", types, 2, st.solid ? 1 : 0, CMD_BGTYPE);
    if (!st.solid) {
        g_npics = pers_pictures(g_pics, ARRAYSIZE(g_pics));
        y = st_text(y, L"Choose your picture");
        cols = st_w() / (tw + S(8));
        if (cols < 1) cols = 1;
        for (i = 0; i < g_npics; i++) {
            HWND t = pg_control(L"SgCplTile", L"", WS_TABSTOP, st_x() + (i % cols) * (tw + S(8)), y + (i / cols) * (th + S(8)), tw, th, CMD_PIC_FIRST + i);
            HBITMAP b = pers_thumb(g_pics[i], S(112), S(63));
            const WCHAR *name = wcsrchr(g_pics[i], L'\\');
            SetWindowTextW(t, name ? name + 1 : g_pics[i]);
            if (b) SendMessageW(t, TILE_SETBITMAP, (WPARAM)b, 0); else SendMessageW(t, TILE_SETCOLOR, RGB(0xDD, 0xDD, 0xDD), 0);
            SendMessageW(t, TILE_SETSEL, !lstrcmpiW(g_pics[i], st.source), 0);
        }
        y += g_npics ? ((g_npics + cols - 1) / cols) * (th + S(8)) + S(8) : 0;
        st_button(&y, L"Browse", CMD_BROWSE);
        st_combo(&y, L"Choose a fit", PERS_FIT_NAMES, WP_COUNT, st.style, CMD_FIT);
    } else {
        y = st_text(y, L"Choose your background color");
        for (i = 0; i < 12; i++) {
            HWND t = pg_control(L"SgCplTile", L"", WS_TABSTOP, st_x() + (i % 6) * S(52), y + (i / 6) * S(52), S(48), S(48), CMD_BG_FIRST + i);
            SendMessageW(t, TILE_SETCOLOR, PERS_BACKGROUNDS[i], 0);
            SendMessageW(t, TILE_SETSEL, PERS_BACKGROUNDS[i] == st.background, 0);
        }
    }
}

BOOL set_cmd_background(int id, int code, HWND ctl)
{
    struct pstate st;
    pers_read(&st);
    if (id >= CMD_PIC_FIRST && id < CMD_PIC_FIRST + g_npics) {
        HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
        const WCHAR *r = pers_set_wallpaper(g_pics[id - CMD_PIC_FIRST], st.style);
        SetCursor(old);
        failed(r);
        return TRUE;
    }
    if (id >= CMD_BG_FIRST && id < CMD_BG_FIRST + 12) { failed(pers_set_background(PERS_BACKGROUNDS[id - CMD_BG_FIRST])); return TRUE; }
    switch (id) {
    case CMD_BGTYPE:
        if (code == CBN_SELCHANGE) {
            LRESULT sel = SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (sel == 1 && !st.solid) failed(pers_set_background(st.background));
            else if (sel == 0 && st.solid) {
                WCHAR pic[MAX_PATH] = L"";
                reg_sz(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Wallpapers",
                       L"BackgroundHistoryPath0", pic, MAX_PATH);
                if (!pic[0] || GetFileAttributesW(pic) == INVALID_FILE_ATTRIBUTES) {
                    g_npics = pers_pictures(g_pics, ARRAYSIZE(g_pics));
                    if (g_npics) lstrcpyW(pic, g_pics[0]);
                }
                if (pic[0]) failed(pers_set_wallpaper(pic, st.style));
                else st_status(L"There is no picture to show. Use Browse to choose one.");
            }
        }
        return TRUE;
    case CMD_FIT:
        if (code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < WP_COUNT && st.source[0]) failed(pers_set_wallpaper(st.source, sel));
        }
        return TRUE;
    case CMD_BROWSE: {
        WCHAR file[MAX_PATH];
        if (browse_picture(file)) failed(pers_set_wallpaper(file, st.style));
        return TRUE;
    }
    }
    return FALSE;
}

/* ---- Colors --------------------------------------------------------------------------------- */
enum { CMD_MODE = CMD_PAGE_FIRST + 1, CMD_APPS_MODE, CMD_SYS_MODE, CMD_TRANSPARENCY, CMD_ACC_FIRST = CMD_PAGE_FIRST + 300,
       CMD_CUSTOM = CMD_PAGE_FIRST + 400 };

void set_build_colors(void)
{
    static const WCHAR *const modes[] = { L"Light", L"Dark", L"Custom" };
    static const WCHAR *const ld[] = { L"Light", L"Dark" };
    struct pstate st;
    int y = st_title(L"Colors"), i, mode;
    pers_read(&st);
    g_preview_pic = !st.solid && st.source[0] ? pers_thumb(st.source, S(192), S(108)) : NULL;
    pg_control(L"SgCplPreview", L"", 0, st_x(), y, S(320), S(210), -1);
    y += S(228);
    mode = st.apps_light && st.system_light ? 0 : !st.apps_light && !st.system_light ? 1 : 2;
    st_combo(&y, L"Choose your color", modes, 3, mode, CMD_MODE);
    if (mode == 2) {
        st_combo(&y, L"Choose your default Windows mode", ld, 2, st.system_light ? 0 : 1, CMD_SYS_MODE);
        st_combo(&y, L"Choose your default app mode", ld, 2, st.apps_light ? 0 : 1, CMD_APPS_MODE);
    }
    st_toggle(&y, L"Transparency effects", reg_dword(HKEY_CURRENT_USER, PERSONALIZE, L"EnableTransparency", 1) != 0, CMD_TRANSPARENCY);
    y = st_head(y, L"Choose your accent color");
    y = st_text(y, L"Windows colors");
    for (i = 0; i < 20; i++) {
        HWND t = pg_control(L"SgCplTile", L"", WS_TABSTOP, st_x() + (i % 10) * S(48), y + (i / 10) * S(48), S(44), S(44), CMD_ACC_FIRST + i);
        WCHAR name[16];
        _snwprintf(name, ARRAYSIZE(name), L"#%02X%02X%02X", GetRValue(PERS_ACCENTS[i]), GetGValue(PERS_ACCENTS[i]), GetBValue(PERS_ACCENTS[i]));
        SetWindowTextW(t, name);
        SendMessageW(t, TILE_SETCOLOR, PERS_ACCENTS[i], 0);
        SendMessageW(t, TILE_SETSEL, PERS_ACCENTS[i] == st.accent, 0);
    }
    y += S(104);
    st_button(&y, L"Custom color", CMD_CUSTOM);
}

BOOL set_cmd_colors(int id, int code, HWND ctl)
{
#ifdef SG_MUTANT_ACCENT
    if (id >= CMD_ACC_FIRST && id < CMD_ACC_FIRST + 20) { failed(pers_set_accent(PERS_ACCENTS[(id - CMD_ACC_FIRST + 1) % 20])); return TRUE; }
#endif
    if (id >= CMD_ACC_FIRST && id < CMD_ACC_FIRST + 20) { failed(pers_set_accent(PERS_ACCENTS[id - CMD_ACC_FIRST])); return TRUE; }
    switch (id) {
    case CMD_MODE:
        if (code == CBN_SELCHANGE) {
            LRESULT sel = SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (sel == 0 || sel == 1) { pers_set_mode(TRUE, sel == 0); failed(pers_set_mode(FALSE, sel == 0)); }
            else {
                /* Custom: Windows dark, apps light -- Windows 10's own default */
                pers_set_mode(TRUE, TRUE);
                failed(pers_set_mode(FALSE, FALSE));
            }
        }
        return TRUE;
    case CMD_APPS_MODE: case CMD_SYS_MODE:
        if (code == CBN_SELCHANGE) failed(pers_set_mode(id == CMD_APPS_MODE, SendMessageW(ctl, CB_GETCURSEL, 0, 0) == 0));
        return TRUE;
    case CMD_TRANSPARENCY: reg_set_dword(HKEY_CURRENT_USER, PERSONALIZE, L"EnableTransparency", st_checked(ctl)); return TRUE;
    case CMD_CUSTOM: {
        static COLORREF custom[16];
        struct pstate st;
        CHOOSECOLORW cc = { sizeof(cc) };
        pers_read(&st);
        cc.hwndOwner = g_main; cc.rgbResult = st.accent; cc.lpCustColors = custom; cc.Flags = CC_RGBINIT | CC_FULLOPEN;
        if (ChooseColorW(&cc)) failed(pers_set_accent(cc.rgbResult));
        return TRUE;
    }
    }
    return FALSE;
}

/* ---- Lock screen ------------------------------------------------------------------------------- */
enum { CMD_LOCK_BROWSE = CMD_PAGE_FIRST + 1, CMD_LOCK_SIGNIN, CMD_LOCK_TIMEOUT, CMD_LOCK_PIC = CMD_PAGE_FIRST + 100 };

void set_build_lockscreen(void)
{
    WCHAR pic[MAX_PATH] = L"";
    int y = st_title(L"Lock screen"), i, cols, tw = S(120), th = S(68);
    reg_sz(HKEY_CURRENT_USER, SG_LOCK, L"Picture", pic, MAX_PATH);
    g_npics = pers_pictures(g_pics, ARRAYSIZE(g_pics));
    y = st_text(y, L"Choose your picture");
    cols = st_w() / (tw + S(8));
    if (cols < 1) cols = 1;
    for (i = 0; i < g_npics; i++) {
        HWND t = pg_control(L"SgCplTile", L"", WS_TABSTOP, st_x() + (i % cols) * (tw + S(8)), y + (i / cols) * (th + S(8)), tw, th, CMD_LOCK_PIC + i);
        HBITMAP b = pers_thumb(g_pics[i], S(112), S(63));
        if (b) SendMessageW(t, TILE_SETBITMAP, (WPARAM)b, 0);
        SendMessageW(t, TILE_SETSEL, pic[0] ? !lstrcmpiW(g_pics[i], pic) : i == 0, 0);
    }
    y += g_npics ? ((g_npics + cols - 1) / cols) * (th + S(8)) + S(8) : 0;
    st_button(&y, L"Browse", CMD_LOCK_BROWSE);
    st_toggle(&y, L"Show lock screen background picture on the sign-in screen",
              reg_dword(HKEY_CURRENT_USER, SG_LOCK, L"ShowOnSignIn", 1) != 0, CMD_LOCK_SIGNIN);
    y = st_para(y, L"The lock screen shows this picture the next time this PC locks.");
    st_link(&y, L"Screen timeout settings", CMD_LOCK_TIMEOUT);
}

BOOL set_cmd_lockscreen(int id, int code, HWND ctl)
{
    (void)code;
    if (id >= CMD_LOCK_PIC && id < CMD_LOCK_PIC + g_npics) { reg_set_sz(HKEY_CURRENT_USER, SG_LOCK, L"Picture", g_pics[id - CMD_LOCK_PIC]); refresh_page(); return TRUE; }
    switch (id) {
    case CMD_LOCK_BROWSE: {
        WCHAR file[MAX_PATH];
        if (browse_picture(file)) { reg_set_sz(HKEY_CURRENT_USER, SG_LOCK, L"Picture", file); refresh_page(); }
        return TRUE;
    }
    case CMD_LOCK_SIGNIN: reg_set_dword(HKEY_CURRENT_USER, SG_LOCK, L"ShowOnSignIn", st_checked(ctl)); return TRUE;
    case CMD_LOCK_TIMEOUT: navigate(PG_S_POWER); return TRUE;
    }
    return FALSE;
}

/* ---- Themes: a picture, a mode and an accent together --------------------------------------------- */
enum { CMD_THEME_FIRST = CMD_PAGE_FIRST + 1 };
static const struct { const WCHAR *name, *file; BOOL light; COLORREF accent; } THEMES[] = {
    { L"Stained Glass", L"stained-glass.jpg", TRUE, RGB(0x7B, 0x2F, 0xBE) },
    { L"Stained Glass Night", L"stained-glass-night.jpg", FALSE, RGB(0x9B, 0x3C, 0xC9) },
};

void set_build_themes(void)
{
    struct pstate st;
    int y = st_title(L"Themes"), i;
    WCHAR path[MAX_PATH], line[200];
    pers_read(&st);
    y = st_head(y, L"Current theme: Custom");
    _snwprintf(line, ARRAYSIZE(line), L"Background: %ls   Color: #%02X%02X%02X   Mode: %ls",
               st.solid ? L"Solid color" : (wcsrchr(st.source, L'\\') ? wcsrchr(st.source, L'\\') + 1 : st.source),
               GetRValue(st.accent), GetGValue(st.accent), GetBValue(st.accent), st.apps_light ? L"Light" : L"Dark");
    y = st_para(y, line);
    y = st_head(y, L"Change theme");
    for (i = 0; i < (int)ARRAYSIZE(THEMES); i++) {
        HWND t;
        HBITMAP b;
        _snwprintf(path, MAX_PATH, L"%ls\\%ls", SG_WALLPAPERS, THEMES[i].file);
        t = pg_control(L"SgCplTile", THEMES[i].name, WS_TABSTOP, st_x() + i * S(210), y, S(200), S(112), CMD_THEME_FIRST + i);
        if ((b = pers_thumb(path, S(192), S(104)))) SendMessageW(t, TILE_SETBITMAP, (WPARAM)b, 0);
        else SendMessageW(t, TILE_SETCOLOR, THEMES[i].accent, 0);
        SendMessageW(t, TILE_SETSEL, !lstrcmpiW(path, st.source) && st.apps_light == THEMES[i].light, 0);
        pg_text(st_x() + i * S(210), y + S(118), S(200), S(22), g_font_body, COL_TEXT, THEMES[i].name, DT_SINGLELINE);
    }
    y += S(150);
    y = st_para(y, L"A theme is a desktop picture, an accent color and light or dark mode together.");
}

BOOL set_cmd_themes(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    if (id >= CMD_THEME_FIRST && id < CMD_THEME_FIRST + (int)ARRAYSIZE(THEMES)) {
        int i = id - CMD_THEME_FIRST;
        WCHAR path[MAX_PATH];
        const WCHAR *why;
        _snwprintf(path, MAX_PATH, L"%ls\\%ls", SG_WALLPAPERS, THEMES[i].file);
        pers_set_mode(TRUE, THEMES[i].light);
        pers_set_mode(FALSE, THEMES[i].light);
        pers_set_accent(THEMES[i].accent);
        why = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES ? pers_set_wallpaper(path, WP_FILL) : NULL;
        failed(why);
        return TRUE;
    }
    return FALSE;
}

/* ---- Start ------------------------------------------------------------------------------------ */
enum { CMD_RECENT = CMD_PAGE_FIRST + 1, CMD_APPLIST, CMD_MORETILES, CMD_FULLSCREEN };

void set_build_start(void)
{
    int y = st_title(L"Start");
    st_toggle(&y, L"Show more tiles on Start", reg_dword(HKEY_CURRENT_USER, SG_START, L"MoreTiles", 0) != 0, CMD_MORETILES);
    st_toggle(&y, L"Show app list in Start menu", reg_dword(HKEY_CURRENT_USER, SG_START, L"ShowAppList", 1) != 0, CMD_APPLIST);
    st_toggle(&y, L"Show recently added apps", reg_dword(HKEY_CURRENT_USER, SG_START, L"ShowRecentlyAdded", 1) != 0, CMD_RECENT);
    st_toggle(&y, L"Use Start full screen", reg_dword(HKEY_CURRENT_USER, SG_START, L"FullScreen", 0) != 0, CMD_FULLSCREEN);
}

BOOL set_cmd_start(int id, int code, HWND ctl)
{
    const WCHAR *v = id == CMD_RECENT ? L"ShowRecentlyAdded" : id == CMD_APPLIST ? L"ShowAppList" :
                     id == CMD_MORETILES ? L"MoreTiles" : id == CMD_FULLSCREEN ? L"FullScreen" : NULL;
    (void)code;
    if (!v) return FALSE;
    reg_set_dword(HKEY_CURRENT_USER, SG_START, v, st_checked(ctl));
    return TRUE;
}

/* ---- Taskbar ----------------------------------------------------------------------------------- */
enum { CMD_LOCKBAR = CMD_PAGE_FIRST + 1, CMD_AUTOHIDE, CMD_SMALL, CMD_ALIGN, CMD_PEEK, CMD_BADGES };

void set_build_taskbar(void)
{
    static const WCHAR *const align[] = { L"Left", L"Center" };
    int y = st_title(L"Taskbar");
    st_toggle(&y, L"Lock the taskbar", reg_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarSizeMove", 0) == 0, CMD_LOCKBAR);
    st_toggle(&y, L"Automatically hide the taskbar in desktop mode",
              reg_dword(HKEY_CURRENT_USER, L"Software\\Stained Glass\\Taskbar", L"AutoHide", 0) != 0, CMD_AUTOHIDE);
    st_toggle(&y, L"Use small taskbar buttons", reg_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarSmallIcons", 0) != 0, CMD_SMALL);
    st_toggle(&y, L"Show badges on taskbar buttons", reg_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarBadges", 1) != 0, CMD_BADGES);
    st_toggle(&y, L"Use Peek to preview the desktop when you move your mouse to the Show desktop button",
              reg_dword(HKEY_CURRENT_USER, ADVANCED, L"DisablePreviewDesktop", 1) == 0, CMD_PEEK);
    st_combo(&y, L"Taskbar alignment", align, 2, reg_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarAl", 0) == 1 ? 1 : 0, CMD_ALIGN);
    y = st_para(y, L"Some taskbar changes take effect the next time you sign in.");
}

BOOL set_cmd_taskbar(int id, int code, HWND ctl)
{
    switch (id) {
    case CMD_LOCKBAR: reg_set_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarSizeMove", !st_checked(ctl)); return TRUE;
    case CMD_AUTOHIDE: reg_set_dword(HKEY_CURRENT_USER, L"Software\\Stained Glass\\Taskbar", L"AutoHide", st_checked(ctl)); return TRUE;
    case CMD_SMALL: reg_set_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarSmallIcons", st_checked(ctl)); return TRUE;
    case CMD_BADGES: reg_set_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarBadges", st_checked(ctl)); return TRUE;
    case CMD_PEEK: reg_set_dword(HKEY_CURRENT_USER, ADVANCED, L"DisablePreviewDesktop", !st_checked(ctl)); return TRUE;
    case CMD_ALIGN:
        if (code == CBN_SELCHANGE) {
            DWORD_PTR r;
            reg_set_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarAl", SendMessageW(ctl, CB_GETCURSEL, 0, 0) == 1);
            SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 2000, &r);
        }
        return TRUE;
    }
    return FALSE;
}
