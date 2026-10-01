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
enum { CMD_MODE = CMD_PAGE_FIRST + 1, CMD_APPS_MODE, CMD_SYS_MODE, CMD_TRANSPARENCY, CMD_STYLE, CMD_ACC_FIRST = CMD_PAGE_FIRST + 300,
       CMD_CUSTOM = CMD_PAGE_FIRST + 400 };

void set_build_colors(void)
{
    static const WCHAR *const modes[] = { L"Light", L"Dark", L"Custom" };
    static const WCHAR *const ld[] = { L"Light", L"Dark" };
    static const WCHAR *const styles[] = { L"Classic: square corners", L"Rounded: round corners, a taller taskbar",
                                           L"Horizon: a blue title bar", L"Glass: a pale glass frame" };
    struct pstate st;
    int y = st_title(L"Colors"), i, mode;
    pers_read(&st);
    g_preview_pic = !st.solid && st.source[0] ? pers_thumb(st.source, S(192), S(108)) : NULL;
    pg_control(L"SgCplPreview", L"", 0, st_x(), y, S(320), S(210), -1);
    y += S(228);
    mode = st.apps_light && st.system_light ? 0 : !st.apps_light && !st.system_light ? 1 : 2;
    st_combo(&y, L"Choose your color", modes, 3, mode, CMD_MODE);
    if (mode == 2) {
        st_combo(&y, L"Choose your default system mode", ld, 2, st.system_light ? 0 : 1, CMD_SYS_MODE);
        st_combo(&y, L"Choose your default app mode", ld, 2, st.apps_light ? 0 : 1, CMD_APPS_MODE);
    }
    st_combo(&y, L"Window style", styles, 4, look_frame_style(), CMD_STYLE);
    st_toggle(&y, L"Transparency effects", reg_dword(HKEY_CURRENT_USER, PERSONALIZE, L"EnableTransparency", 1) != 0, CMD_TRANSPARENCY);
    y = st_head(y, L"Choose your accent color");
    y = st_text(y, L"Accent colors");
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
    case CMD_TRANSPARENCY: failed(effects_set(L"transparency", st_checked(ctl))); return TRUE;
    case CMD_STYLE:
        if (code == CBN_SELCHANGE) failed(look_set_frame((int)SendMessageW(ctl, CB_GETCURSEL, 0, 0)));
        return TRUE;
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

/* The lock screen is drawn by the machine account, which cannot read this
 * user's files: the choice is published for it by sg-settingsctl (as this
 * user, into the lock-screen drop that sg-lockd checks). PIC NULL = the
 * system's picture. */
static void lock_publish(const WCHAR *pic)
{
    static char *(CDECL *to_unix)(const WCHAR *);
    WCHAR args[MAX_PATH * 2 + 32], err[160];
    char *unix_path;
    BOOL ok;
    if (!pic) lstrcpyW(args, L"lockscreen picture default");
    else {
        if (!to_unix) to_unix = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
        if (!to_unix || !(unix_path = to_unix(pic))) { failed(L"the picture is not on a drive this PC can read"); return; }
        if (strchr(unix_path, '"')) { HeapFree(GetProcessHeap(), 0, unix_path); failed(L"the picture's name has a quotation mark"); return; }
        _snwprintf(args, ARRAYSIZE(args), L"lockscreen picture \"%hs\"", unix_path);
        args[ARRAYSIZE(args) - 1] = 0;
        HeapFree(GetProcessHeap(), 0, unix_path);
    }
    free(ctl_run(args, &ok, err, ARRAYSIZE(err), 15000));
    if (!ok) failed(err[0] ? err : L"the lock screen could not be told");
}

static void lock_publish_signin(BOOL on)
{
    WCHAR err[160];
    BOOL ok;
    free(ctl_run(on ? L"lockscreen signin yes" : L"lockscreen signin no", &ok, err, ARRAYSIZE(err), 15000));
    if (!ok) failed(err[0] ? err : L"the lock screen could not be told");
}

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
        const WCHAR *name = wcsrchr(g_pics[i], L'\\');
        SetWindowTextW(t, name ? name + 1 : g_pics[i]);
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
    if (id >= CMD_LOCK_PIC && id < CMD_LOCK_PIC + g_npics) {
        reg_set_sz(HKEY_CURRENT_USER, SG_LOCK, L"Picture", g_pics[id - CMD_LOCK_PIC]);
        refresh_page();
        lock_publish(g_pics[id - CMD_LOCK_PIC]);
        return TRUE;
    }
    switch (id) {
    case CMD_LOCK_BROWSE: {
        WCHAR file[MAX_PATH];
        if (browse_picture(file)) { reg_set_sz(HKEY_CURRENT_USER, SG_LOCK, L"Picture", file); refresh_page(); lock_publish(file); }
        return TRUE;
    }
    case CMD_LOCK_SIGNIN:
        reg_set_dword(HKEY_CURRENT_USER, SG_LOCK, L"ShowOnSignIn", st_checked(ctl));
        lock_publish_signin(st_checked(ctl));
        return TRUE;
    case CMD_LOCK_TIMEOUT: navigate(PG_S_POWER); return TRUE;
    }
    return FALSE;
}

/* ---- Themes: a picture, a mode and an accent together --------------------------------------------- */
enum { CMD_THEME_FIRST = CMD_PAGE_FIRST + 1, CMD_LOOK = CMD_PAGE_FIRST + 100 };
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
    y = st_head(y, L"Look");
    {
        /* short names: the descriptions did not fit the box (David 2026-09-29) */
        static const WCHAR *const looks[] = { L"Classic", L"Rounded", L"Horizon", L"Glass", L"Mixed" };
        BOOL r = look_rounded(), centred = reg_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarAl", 0) == 1,
             cstart = reg_dword(HKEY_CURRENT_USER, L"Software\\Stained Glass\\Start", L"Centered", 0) != 0;
        DWORD bar = look_taskbar_style();
        int frame = look_frame_style();   /* the windows' frames are part of a look too */
        int look = r || centred || cstart ? (r && centred && cstart && !bar ? LOOK_ROUNDED : 4)
                 : bar == 1 ? (frame == LOOK_HORIZON ? LOOK_HORIZON : 4) : bar == 2 ? (frame == LOOK_GLASS ? LOOK_GLASS : 4)
                 : frame == LOOK_CLASSIC ? LOOK_CLASSIC : 4;
        st_combo(&y, L"Choose a look", looks, look == 4 ? 5 : 4, look, CMD_LOOK);
    }
    y = st_para(y, L"Classic: square windows, with the taskbar's buttons and Start at the left. "
                   L"Rounded: round corners, with the taskbar's buttons and Start in the middle. "
                   L"Horizon: a bright blue taskbar with a green Start button. "
                   L"Glass: a dark glass taskbar with a round Start button and big icons.");
    y = st_para(y, L"A look sets the window style, the taskbar and Start together. Each can still be changed on its own "
                   L"under Colors, Taskbar and Start, to mix them (Mixed).");
}

BOOL set_cmd_themes(int id, int code, HWND ctl)
{
    if (id == CMD_LOOK) {
        LRESULT sel = SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        if (code == CBN_SELCHANGE && sel >= 0 && sel < LOOK_COUNT) failed(look_apply((int)sel));
        return TRUE;
    }
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
enum { CMD_RECENT = CMD_PAGE_FIRST + 1, CMD_APPLIST, CMD_MORETILES, CMD_FULLSCREEN, CMD_MOSTUSED, CMD_SUGGEST, CMD_LAYOUT };
#define CDM L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager"
static void tray_settings_changed(void);

void set_build_start(void)
{
    static const WCHAR *layouts[] = { L"Tiles (the default)", L"Centered: pinned and recent apps" };
    int y = st_title(L"Start");
    /* the newer look: Start and the taskbar's buttons in the middle, Start
     * as a grid of pinned apps over recommended ones (sg-start's centred
     * layout); Tiles is the classic one, as installed */
    st_combo(&y, L"Start layout", layouts, 2, reg_dword(HKEY_CURRENT_USER, SG_START, L"Centered", 0) ? 1 : 0, CMD_LAYOUT);
    st_toggle(&y, L"Show more tiles on Start", reg_dword(HKEY_CURRENT_USER, SG_START, L"MoreTiles", 0) != 0, CMD_MORETILES);
    st_toggle(&y, L"Show app list in Start menu", reg_dword(HKEY_CURRENT_USER, SG_START, L"ShowAppList", 1) != 0, CMD_APPLIST);
    st_toggle(&y, L"Show recently added apps", reg_dword(HKEY_CURRENT_USER, SG_START, L"ShowRecentlyAdded", 1) != 0, CMD_RECENT);
    st_toggle(&y, L"Show most used apps", reg_dword(HKEY_CURRENT_USER, ADVANCED, L"Start_TrackProgs", 1) != 0, CMD_MOSTUSED);
    st_toggle(&y, L"Show suggestions occasionally in Start",
              reg_dword(HKEY_CURRENT_USER, CDM, L"SubscribedContent-338388Enabled", 1) != 0, CMD_SUGGEST);
    st_toggle(&y, L"Use Start full screen", reg_dword(HKEY_CURRENT_USER, SG_START, L"FullScreen", 0) != 0, CMD_FULLSCREEN);
}

BOOL set_cmd_start(int id, int code, HWND ctl)
{
    const WCHAR *v = id == CMD_RECENT ? L"ShowRecentlyAdded" : id == CMD_APPLIST ? L"ShowAppList" :
                     id == CMD_MORETILES ? L"MoreTiles" : id == CMD_FULLSCREEN ? L"FullScreen" : NULL;
    if (id == CMD_LAYOUT)
    {
        int sel = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        if (code != CBN_SELCHANGE || sel < 0) return TRUE;
        reg_set_dword(HKEY_CURRENT_USER, SG_START, L"Centered", sel == 1);
        /* the taskbar's buttons move to the middle with it, and back */
        reg_set_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarAl", sel == 1);
        tray_settings_changed();
        return TRUE;
    }
    (void)code;
    /* Windows keeps these two where Windows programs look for them */
    if (id == CMD_MOSTUSED) { reg_set_dword(HKEY_CURRENT_USER, ADVANCED, L"Start_TrackProgs", st_checked(ctl)); return TRUE; }
    if (id == CMD_SUGGEST) { reg_set_dword(HKEY_CURRENT_USER, CDM, L"SubscribedContent-338388Enabled", st_checked(ctl)); return TRUE; }
    if (!v) return FALSE;
    reg_set_dword(HKEY_CURRENT_USER, SG_START, v, st_checked(ctl));
    return TRUE;
}

/* ---- Taskbar ----------------------------------------------------------------------------------- */
/* Where Windows keeps them (Explorer\Advanced, Search), the position and
 * auto-hide in our own key; every change is announced with WM_SETTINGCHANGE
 * "TraySettings", which the taskbar (wine-sg 0164) reads them again on. */
enum { CMD_LOCKBAR = CMD_PAGE_FIRST + 1, CMD_AUTOHIDE, CMD_SMALL, CMD_ALIGN, CMD_PEEK, CMD_BADGES,
       CMD_POSITION, CMD_COMBINE, CMD_TASKVIEW, CMD_SEARCH, CMD_TBCOLOR, CMD_DESKTOPS, CMD_TBSTYLE };
#define SG_TASKBAR L"Software\\Stained Glass\\Taskbar"
#define SEARCH_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Search"

void set_build_taskbar(void)
{
    static const WCHAR *const align[] = { L"Left", L"Center" };
    /* in Windows' order; the values are the ABE_ edges */
    static const WCHAR *const where[] = { L"Left", L"Top", L"Right", L"Bottom" };
    static const WCHAR *const combine[] = { L"Always, hide labels", L"When taskbar is full", L"Never" };
    static const WCHAR *const search[] = { L"Hidden", L"Show search icon", L"Show search box" };
    static const WCHAR *const colors[] = { L"Follow the system mode", L"Dark", L"Light", L"Light blue", L"Accent color" };
    static const WCHAR *const styles[] = { L"Flat (the default)", L"Horizon: bright blue", L"Glass: dark glass" };
    DWORD color;
    DWORD pos = reg_dword(HKEY_CURRENT_USER, SG_TASKBAR, L"Position", 3), glom = reg_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarGlomLevel", 2);
    DWORD box = reg_dword(HKEY_CURRENT_USER, SEARCH_KEY, L"SearchboxTaskbarMode", 0);
    int y = st_title(L"Taskbar");
    st_toggle(&y, L"Lock the taskbar", reg_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarSizeMove", 0) == 0, CMD_LOCKBAR);
    st_toggle(&y, L"Automatically hide the taskbar in desktop mode",
              reg_dword(HKEY_CURRENT_USER, SG_TASKBAR, L"AutoHide", 0) != 0, CMD_AUTOHIDE);
    st_toggle(&y, L"Use small taskbar buttons", reg_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarSmallIcons", 0) != 0, CMD_SMALL);
    st_toggle(&y, L"Show badges on taskbar buttons", reg_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarBadges", 1) != 0, CMD_BADGES);
    st_toggle(&y, L"Use Peek to preview the desktop when you move your mouse to the Show desktop button",
              reg_dword(HKEY_CURRENT_USER, ADVANCED, L"DisablePreviewDesktop", 1) == 0, CMD_PEEK);
    st_toggle(&y, L"Show Task View button", reg_dword(HKEY_CURRENT_USER, ADVANCED, L"ShowTaskViewButton", 1) != 0, CMD_TASKVIEW);
    st_toggle(&y, L"Show virtual desktops on the taskbar", reg_dword(HKEY_CURRENT_USER, SG_TASKBAR, L"ShowDesktops", 0) != 0, CMD_DESKTOPS);
    st_combo(&y, L"Taskbar location on screen", where, 4, pos <= 3 ? (int)pos : 3, CMD_POSITION);
    st_combo(&y, L"Combine taskbar buttons", combine, 3, glom <= 2 ? (int)glom : 2, CMD_COMBINE);
    st_combo(&y, L"Search", search, 3, box <= 2 ? (int)box : 0, CMD_SEARCH);
    st_combo(&y, L"Taskbar alignment", align, 2, reg_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarAl", 0) == 1 ? 1 : 0, CMD_ALIGN);
    color = reg_dword(HKEY_CURRENT_USER, SG_TASKBAR, L"Color", 0);
    st_combo(&y, L"Taskbar color", colors, 5, color <= 4 ? (int)color : 0, CMD_TBCOLOR);
    /* the taskbar of older desktops (wine-sg 0600); its own colours replace Taskbar color */
    st_combo(&y, L"Taskbar style", styles, 3, (int)look_taskbar_style(), CMD_TBSTYLE);
}

static void tray_settings_changed(void)
{
    DWORD_PTR r;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 2000, &r);
}

BOOL set_cmd_taskbar(int id, int code, HWND ctl)
{
    int sel = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
    switch (id) {
    case CMD_LOCKBAR: reg_set_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarSizeMove", !st_checked(ctl)); break;
    case CMD_AUTOHIDE: reg_set_dword(HKEY_CURRENT_USER, SG_TASKBAR, L"AutoHide", st_checked(ctl)); break;
    case CMD_SMALL: reg_set_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarSmallIcons", st_checked(ctl)); break;
    case CMD_BADGES: reg_set_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarBadges", st_checked(ctl)); break;
    case CMD_PEEK: reg_set_dword(HKEY_CURRENT_USER, ADVANCED, L"DisablePreviewDesktop", !st_checked(ctl)); break;
    case CMD_TASKVIEW: reg_set_dword(HKEY_CURRENT_USER, ADVANCED, L"ShowTaskViewButton", st_checked(ctl)); break;
    case CMD_DESKTOPS: reg_set_dword(HKEY_CURRENT_USER, SG_TASKBAR, L"ShowDesktops", st_checked(ctl)); break;
    case CMD_TBCOLOR:
        if (code != CBN_SELCHANGE || sel < 0) return TRUE;
        reg_set_dword(HKEY_CURRENT_USER, SG_TASKBAR, L"Color", sel);
        break;
    case CMD_TBSTYLE:
        if (code != CBN_SELCHANGE || sel < 0 || sel > 2) return TRUE;
        reg_set_dword(HKEY_CURRENT_USER, SG_TASKBAR, L"Style", sel);
        break;
    case CMD_ALIGN:
        if (code != CBN_SELCHANGE) return TRUE;
        reg_set_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarAl", sel == 1);
        break;
    case CMD_POSITION:
        if (code != CBN_SELCHANGE || sel < 0) return TRUE;
        reg_set_dword(HKEY_CURRENT_USER, SG_TASKBAR, L"Position", sel);
        break;
    case CMD_COMBINE:
        if (code != CBN_SELCHANGE || sel < 0) return TRUE;
        reg_set_dword(HKEY_CURRENT_USER, ADVANCED, L"TaskbarGlomLevel", sel);
        break;
    case CMD_SEARCH:
        if (code != CBN_SELCHANGE || sel < 0) return TRUE;
        reg_set_dword(HKEY_CURRENT_USER, SEARCH_KEY, L"SearchboxTaskbarMode", sel);
        break;
    default:
        return FALSE;
    }
    tray_settings_changed();
    return TRUE;
}

/* ---- Share & reset ---------------------------------------------------------------------------- */
/* a look saved to a file, read back, or put back as a new account has it
 * (lookshare.c; David 2026-09-29) */
enum { CMD_LOOK_EXPORT = CMD_PAGE_FIRST + 1, CMD_LOOK_IMPORT, CMD_LOOK_RESET };

void set_build_lookshare(void)
{
    int y = st_title(L"Share & reset");
    y = st_para(y, L"Your look is the choices on these pages: the window style, light or dark, the accent color, the "
                   L"desktop picture, and how the taskbar and Start are laid out. Your pins, tiles and files are not part of it.");
    y = st_head(y, L"Share your look");
    y = st_para(y, L"Save your look to a file to keep it, to use it on another computer, or to give it to someone.");
    st_button(&y, L"Save my look...", CMD_LOOK_EXPORT);
    st_button(&y, L"Use a saved look...", CMD_LOOK_IMPORT);
    y = st_head(y, L"Reset");
    y = st_para(y, L"Put back the look a new account starts with: the Classic look and the Stained Glass theme.");
    st_button(&y, L"Reset to the default look", CMD_LOOK_RESET);
}

static BOOL look_file(WCHAR *file, BOOL save)
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    file[0] = 0;
    if (save) lstrcpyW(file, L"My look.sglook");
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"Stained Glass look (*.sglook)\0*.sglook\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"sglook";
    ofn.Flags = OFN_EXPLORER | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
}

BOOL set_cmd_lookshare(int id, int code, HWND ctl)
{
    WCHAR file[MAX_PATH];
    const WCHAR *why;
    (void)code; (void)ctl;
    switch (id)
    {
    case CMD_LOOK_EXPORT:
        if (look_file(file, TRUE)) { if (!(why = look_export(file))) st_status(L"Your look is saved."); else failed(why); }
        return TRUE;
    case CMD_LOOK_IMPORT:
        if (look_file(file, FALSE)) { if (!(why = look_import(file))) { st_status(L"The saved look is in use."); refresh_page(); } else failed(why); }
        return TRUE;
    case CMD_LOOK_RESET:
        if (MessageBoxW(g_main, L"Put back the default look? Your pins, tiles and files are kept.", L"Reset",
                        MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
        {
            if (!(why = look_reset())) { st_status(L"The default look is back."); refresh_page(); } else failed(why);
        }
        return TRUE;
    }
    return FALSE;
}

/* ---- Effects ---------------------------------------------------------------------------------- */
/* Animations to set up and play with (David 2026-09-29). "Show animations in
 * Windows" is Windows' own switch (SPI_SETCLIENTAREAANIMATION, kept in
 * UserPreferencesMask): with it off nothing below moves. */
#define SG_EFFECTS L"Software\\Stained Glass\\Effects"
enum { CMD_ANIMATIONS = CMD_PAGE_FIRST + 1, CMD_SLIDE, CMD_SHADOWS, CMD_OPEN, CMD_MINIMIZE, CMD_WOBBLY, CMD_MOVING };

/* The window effects are drawn by the desktop's compositor (sg-compositor's
 * sg-deskcomp), a Linux program: it reads them from
 * $XDG_CONFIG_HOME/stained-glass/effects.conf, which this writes after every
 * change -- the choices kept here (Software\Stained Glass\Effects), the
 * look's shadow and "Show animations". */
static const WCHAR *const OPEN_KEYS[] = { L"none", L"fade", L"zoom" };
static const WCHAR *const MINIMIZE_KEYS[] = { L"none", L"scale", L"lamp" };

static DWORD effects_choice(const WCHAR *name, DWORD def, DWORD max)
{
    DWORD v = reg_dword(HKEY_CURRENT_USER, SG_EFFECTS, name, def);
    return v <= max ? v : def;
}

const WCHAR *effects_write_conf(void)
{
    WCHAR home[MAX_PATH] = L"", dir[MAX_PATH], path[MAX_PATH], *dos;
    WCHAR *(CDECL *to_dos)(const char *) = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name");
    char unix_dir[MAX_PATH * 3], text[512];
    int frame = look_frame_style();
    HANDLE f;
    DWORD done;

#ifdef SG_MUTANT_NOEFFECTSCONF
    to_dos = NULL;
#endif
    if (!to_dos) return NULL;   /* not under Wine: no compositor to tell */
    if (GetEnvironmentVariableW(L"XDG_CONFIG_HOME", home, MAX_PATH) && home[0])
    {
        _snwprintf(dir, MAX_PATH, L"%ls/stained-glass", home);
        dir[MAX_PATH - 1] = 0;
        WideCharToMultiByte(CP_UTF8, 0, dir, -1, unix_dir, sizeof(unix_dir), NULL, NULL);
        if (!(dos = to_dos(unix_dir))) return L"the effects' folder could not be found";
        lstrcpynW(path, dos, MAX_PATH);
        HeapFree(GetProcessHeap(), 0, dos);
    }
    /* Wine hands $HOME over as WINEHOMEDIR, a DOS path behind \??\ */
    else if (GetEnvironmentVariableW(L"WINEHOMEDIR", home, MAX_PATH) && !wcsncmp(home, L"\\??\\", 4) && home[4])
    {
        _snwprintf(path, MAX_PATH, L"%ls\\.config\\stained-glass", home + 4);
        path[MAX_PATH - 1] = 0;
    }
    else return L"no home folder to keep the effects in";
    SHCreateDirectoryExW(NULL, path, NULL);
    lstrcatW(path, L"\\effects.conf");
    _snprintf(text, sizeof(text),
              "# Written by Settings > Personalization > Effects; read by sg-deskcomp.\n"
              "shadows=%d\nshadow=%s\nanimations=%d\nopen=%ls\nminimize=%ls\nwobbly=%d\nmoving=%d\n",
              effects_choice(L"Shadows", 1, 1) != 0,
              frame == LOOK_HORIZON ? "horizon" : frame == LOOK_GLASS ? "glass" : "modern",
              effects_animations(),
              OPEN_KEYS[effects_choice(L"WindowOpen", 0, 2)], MINIMIZE_KEYS[effects_choice(L"WindowMinimize", 0, 2)],
              effects_choice(L"Wobbly", 0, 1) != 0, effects_choice(L"MovingTranslucent", 0, 1) != 0);
    text[sizeof(text) - 1] = 0;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return L"the effects could not be saved for the compositor";
    WriteFile(f, text, (DWORD)strlen(text), &done, NULL);
    CloseHandle(f);
    return NULL;
}

BOOL effects_animations(void)
{
    /* read where Windows keeps it (UserPreferencesMask, byte 4, 0x02), not
     * SPI_GETCLIENTAREAANIMATION: Wine caches parameters per process, and a
     * change made by another process (sg-settings --set, a second window)
     * stayed unseen here */
    BYTE mask[16];
    DWORD size = sizeof(mask);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"UserPreferencesMask", RRF_RT_REG_BINARY,
                     NULL, mask, &size) || size < 5)
        return TRUE;
    return (mask[4] & 0x02) != 0;
}

const WCHAR *effects_set(const WCHAR *what, BOOL on)
{
    if (!lstrcmpW(what, L"animations")) {
        if (!SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0, (void *)(INT_PTR)on, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
            return L"the setting could not be saved";
        return effects_write_conf();   /* the compositor's effects stop too */
    }
    if (!lstrcmpW(what, L"slide")) return reg_set_dword(HKEY_CURRENT_USER, SG_EFFECTS, L"SlideDesktops", on) ? NULL : L"the setting could not be saved";
    if (!lstrcmpW(what, L"transparency")) {
        /* Colors > Transparency effects: the taskbar and Start frosted (wine-sg
         * 0745); the taskbar reads it again on "ImmersiveColorSet" */
        DWORD_PTR r;
        if (!reg_set_dword(HKEY_CURRENT_USER, PERSONALIZE, L"EnableTransparency", on)) return L"the setting could not be saved";
#ifndef SG_MUTANT_NOTRANSPARENCYNOTE
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 2000, &r);
#endif
        return NULL;
    }
    {
        static const struct { const WCHAR *key, *value; } toggles[] = {
            { L"shadows", L"Shadows" }, { L"wobbly", L"Wobbly" }, { L"moving", L"MovingTranslucent" },
        };
        int i;
        for (i = 0; i < (int)ARRAYSIZE(toggles); i++)
            if (!lstrcmpW(what, toggles[i].key))
                return reg_set_dword(HKEY_CURRENT_USER, SG_EFFECTS, toggles[i].value, on) ? effects_write_conf()
                                                                                         : L"the setting could not be saved";
    }
    return L"unknown effect";
}

/* the open/close and minimize animations: their kinds by name */
const WCHAR *effects_set_kind(const WCHAR *what, const WCHAR *kind)
{
    const WCHAR *const *keys = !lstrcmpW(what, L"open") ? OPEN_KEYS : !lstrcmpW(what, L"minimize") ? MINIMIZE_KEYS : NULL;
    int i;
    if (!keys) return L"unknown effect";
    for (i = 0; i < 3; i++)
        if (!lstrcmpW(kind, keys[i]))
            return reg_set_dword(HKEY_CURRENT_USER, SG_EFFECTS, keys == OPEN_KEYS ? L"WindowOpen" : L"WindowMinimize", i)
                   ? effects_write_conf() : L"the setting could not be saved";
    return L"unknown kind";
}

void set_build_effects(void)
{
    int y = st_title(L"Effects");
    y = st_para(y, L"How things move. With animations off, windows and desktops change at once.");
    st_toggle(&y, L"Show animations", effects_animations(), CMD_ANIMATIONS);
    y = st_head(y, L"Animations");
    st_toggle(&y, L"Slide between virtual desktops", reg_dword(HKEY_CURRENT_USER, SG_EFFECTS, L"SlideDesktops", 1) != 0, CMD_SLIDE);
    y = st_para(y, L"Task View's windows fly into place when it opens, while animations are on.");
    y = st_head(y, L"Window effects");
    {
        static const WCHAR *const opens[] = { L"None", L"Fade", L"Zoom" };
        static const WCHAR *const mins[] = { L"None", L"Shrink", L"Magic lamp" };
        st_toggle(&y, L"Shadows under windows and menus", effects_choice(L"Shadows", 1, 1) != 0, CMD_SHADOWS);
        st_combo(&y, L"Opening and closing windows", opens, 3, (int)effects_choice(L"WindowOpen", 0, 2), CMD_OPEN);
        st_combo(&y, L"Minimizing windows", mins, 3, (int)effects_choice(L"WindowMinimize", 0, 2), CMD_MINIMIZE);
        st_toggle(&y, L"Wobbly windows while dragging", effects_choice(L"Wobbly", 0, 1) != 0, CMD_WOBBLY);
        st_toggle(&y, L"See-through windows while moving", effects_choice(L"MovingTranslucent", 0, 1) != 0, CMD_MOVING);
    }
    y = st_para(y, L"The shadows follow the look: soft for Classic and Rounded, small for Horizon, deep for Glass. "
                   L"The window effects apply at once; with animations off they stay still.");
}

BOOL set_cmd_effects(int id, int code, HWND ctl)
{
    (void)code;
    if (id == CMD_ANIMATIONS) { failed(effects_set(L"animations", st_checked(ctl))); return TRUE; }
    if (id == CMD_SLIDE) { failed(effects_set(L"slide", st_checked(ctl))); return TRUE; }
    if (id == CMD_SHADOWS) { failed(effects_set(L"shadows", st_checked(ctl))); return TRUE; }
    if (id == CMD_WOBBLY) { failed(effects_set(L"wobbly", st_checked(ctl))); return TRUE; }
    if (id == CMD_MOVING) { failed(effects_set(L"moving", st_checked(ctl))); return TRUE; }
    if ((id == CMD_OPEN || id == CMD_MINIMIZE) && code == CBN_SELCHANGE) {
        LRESULT sel = SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        if (sel >= 0 && sel < 3)
            failed(effects_set_kind(id == CMD_OPEN ? L"open" : L"minimize", (id == CMD_OPEN ? OPEN_KEYS : MINIMIZE_KEYS)[sel]));
        return TRUE;
    }
    return FALSE;
}
