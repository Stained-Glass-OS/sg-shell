/*
 * Stained Glass Settings -- the look: the window style (square or rounded
 * corners) and the two whole looks, Classic and Rounded, that set the style,
 * the taskbar and Start together, and two more with the taskbar of older
 * desktops: Horizon (a bright blue bar, a green Start button) and Glass (a
 * dark glass bar, a round Start orb). Every part stays choosable on its own
 * page afterwards, so the looks mix: a rounded style with a classic taskbar,
 * a centred taskbar with square windows, any taskbar colour or style.
 *
 *   HKCU\Software\Stained Glass\Style Rounded       wine-sg 0483, 0484
 *   HKCU\Software\Stained Glass\Style Frame         wine-sg 0742 (0 flat, 1 Horizon, 2 Glass)
 *   HKCU\Software\Stained Glass\Taskbar Color, ShowDesktops, PinOrder (0484, 0485)
 *   HKCU\Software\Stained Glass\Taskbar Style       wine-sg 0600 (0 flat, 1 Horizon, 2 Glass)
 *   HKCU\Software\Stained Glass\Start Centered      sg-start
 *
 * Each part has the four looks of its own (David 2026-10-02: "Every aspect
 * has Classic, Rounded, Horizon, Glass as a choice ... Themes should show
 * Custom when you start mixing and matching"):
 *   window frames  Style\Rounded + Style\Frame (above)
 *   the taskbar    Taskbar\Look 0-3 (wine-sg 0802; Taskbar\Style kept for it)
 *   Start          Start\Look 0-3 (sg-start; Start\Centered kept for it)
 * A look sets all three; Themes shows the look they share, or Custom.
 *
 * The title bars take a share of the screen: their sizes are the look's at
 * up to 800 px of height and grow with it in eighths (1080 p: 11/8), unless
 * Style\ScaleWithScreen is 0 (David 2026-10-02: "kinda small on a 1080p
 * screen ... we need it to take up a % of the screen").
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define COBJMACROS
#include "control.h"
#include <shlobj.h>
#include <shlwapi.h>

static const WCHAR STYLE[] = L"Software\\Stained Glass\\Style";
static const WCHAR TASKBAR[] = L"Software\\Stained Glass\\Taskbar";
static const WCHAR START[] = L"Software\\Stained Glass\\Start";
static const WCHAR ADV[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
static const WCHAR SEARCHKEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Search";

static void broadcast(const WCHAR *what)
{
    DWORD_PTR r;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)what, SMTO_ABORTIFHUNG, 2000, &r);
}

/* every program shapes its windows' corners again at a frame change; a
 * program reads the style at most twice a second, so the frames change a
 * little after the style did */
static BOOL CALLBACK reframe(HWND hwnd, LPARAM lp)
{
    (void)lp;
    if (IsWindowVisible(hwnd) && (GetWindowLongW(hwnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION)
        SetWindowPos(hwnd, 0, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    return TRUE;
}

static DWORD WINAPI reframe_later(void *arg)
{
    (void)arg;
    Sleep(700);
    EnumWindows(reframe, 0);
    return 0;
}

const WCHAR *const LOOK_KEYS[LOOK_COUNT] = { L"classic", L"rounded", L"horizon", L"glass" };

DWORD look_taskbar_style(void)
{
    DWORD v = reg_dword(HKEY_CURRENT_USER, TASKBAR, L"Style", 0);
    return v <= 2 ? v : 0;
}

BOOL look_rounded(void)
{
    return reg_dword(HKEY_CURRENT_USER, STYLE, L"Rounded", 0) != 0;
}

/* the window style: LOOK_CLASSIC, LOOK_ROUNDED, or the Horizon and Glass
 * frames (Style\Frame 1 and 2, wine-sg 0742) */
int look_frame_style(void)
{
    DWORD frame = reg_dword(HKEY_CURRENT_USER, STYLE, L"Frame", 0);
    if (frame == 1) return LOOK_HORIZON;
    if (frame == 2) return LOOK_GLASS;
    return look_rounded() ? LOOK_ROUNDED : LOOK_CLASSIC;
}

/* the title bars' scale for this screen, in eighths: 8 up to 800 px of
 * height, then with it (900 p 9, 1080 p 11, 1440 p 14, 2160 p 22), at most
 * 24. The height is the screen's at the display scale (LogPixels): the
 * sizes are kept at 96 DPI and Wine draws them at the scale, so at 175% on
 * a 1824 px screen they are 1042 px's -- 10 eighths, made 1.75 times as
 * large: the share of the screen they have at 1080p and 100%. At 100%
 * nothing changes. */
int look_scale8(void)
{
    DEVMODEW dm = { .dmSize = sizeof(dm) };
    int h = GetSystemMetrics(SM_CYSCREEN), s;
    /* the mode just set (a resolution change in this process) before the metric follows */
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsHeight >= 200) h = (int)dm.dmPelsHeight;
    {
        /* the gates' stand-in for a bigger screen than their X server's */
        WCHAR fake[16];
        if (GetEnvironmentVariableW(L"SG_FAKE_SCREEN_HEIGHT", fake, ARRAYSIZE(fake)) && _wtoi(fake) >= 200) h = _wtoi(fake);
    }
#ifndef SG_MUTANT_TITLE_DOUBLE_SCALE
    {
        DWORD dpi = reg_dword(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"LogPixels", 96);
        if (dpi > 96 && dpi <= 480) h = MulDiv(h, 96, (int)dpi);
    }
#endif
    if (!reg_dword(HKEY_CURRENT_USER, STYLE, L"ScaleWithScreen", 1)) return 8;
#ifdef SG_MUTANT_NOSCALE
    return 8;
#endif
    s = (h * 8 + 400) / 800;
    return s < 8 ? 8 : s > 24 ? 24 : s;
}

static int scaled(int px, int s8) { return (px * s8 + 4) / 8; }

/* each style's title bar and frame sizes, as its era had them, in pixels at
 * 96 DPI and up to 800 px of screen, and whether the title is bold
 * (Horizon's was); scaled to the screen (look_scale8). The caption font
 * grows with them: its size at scale 1 is kept (Style\CaptionFontBase). */
static void look_metrics(int style)
{
    static const int caption[LOOK_COUNT] = { 18, 18, 25, 21 }, button[LOOK_COUNT] = { 18, 18, 25, 26 };
    static const int border[LOOK_COUNT] = { 1, 1, 2, 7 };   /* Wine keeps the padded border in this one */
    NONCLIENTMETRICSW ncm;
    int s8 = look_scale8(), font_base;
    LONG font;

    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) return;
    /* the caption font's height at scale 1: as it was found the first time */
    font_base = (int)reg_dword(HKEY_CURRENT_USER, STYLE, L"CaptionFontBase", 0);
    if (!font_base || font_base > 64) {
        DWORD was = reg_dword(HKEY_CURRENT_USER, STYLE, L"Scale8", 8);
        font_base = ncm.lfCaptionFont.lfHeight < 0 ? -ncm.lfCaptionFont.lfHeight : 12;
        if (was > 8 && was <= 24) font_base = (font_base * 8 + (int)was / 2) / (int)was;
        if (font_base < 8 || font_base > 64) font_base = 12;
        reg_set_dword(HKEY_CURRENT_USER, STYLE, L"CaptionFontBase", font_base);
    }
    font = -scaled(font_base, s8);
    reg_set_dword(HKEY_CURRENT_USER, STYLE, L"Scale8", s8);
    if (ncm.iCaptionHeight == scaled(caption[style], s8) && ncm.iCaptionWidth == scaled(button[style], s8) &&
        ncm.iBorderWidth == scaled(border[style], s8) && ncm.lfCaptionFont.lfHeight == font &&
        (ncm.lfCaptionFont.lfWeight >= FW_BOLD) == (style == LOOK_HORIZON))
        return;
    ncm.iCaptionHeight = scaled(caption[style], s8);
    ncm.iCaptionWidth = scaled(button[style], s8);
    ncm.iBorderWidth = scaled(border[style], s8);
    ncm.lfCaptionFont.lfHeight = font;
    ncm.lfCaptionFont.lfWeight = style == LOOK_HORIZON ? FW_BOLD : FW_NORMAL;
#ifndef SG_MUTANT_NOMETRICS
    SystemParametersInfoW(SPI_SETNONCLIENTMETRICS, sizeof(ncm), &ncm, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);
#endif
}

static HANDLE g_reframe;

const WCHAR *look_set_frame(int style)
{
    if (style < 0 || style >= LOOK_COUNT) return L"unknown window style";
    if (!reg_set_dword(HKEY_CURRENT_USER, STYLE, L"Rounded", style == LOOK_ROUNDED ? 1 : 0) ||
        !reg_set_dword(HKEY_CURRENT_USER, STYLE, L"Frame", style == LOOK_HORIZON ? 1 : style == LOOK_GLASS ? 2 : 0))
        return L"the setting could not be saved";
    look_metrics(style);
    effects_write_conf();              /* the compositor's shadows take the look's manner */
    broadcast(L"ImmersiveColorSet");   /* the taskbar takes the style's height and look */
    if (g_reframe) CloseHandle(g_reframe);
    g_reframe = CreateThread(NULL, 0, reframe_later, NULL, 0, NULL);
    return NULL;
}

const WCHAR *look_set_style(BOOL rounded)
{
    return look_set_frame(rounded ? LOOK_ROUNDED : LOOK_CLASSIC);
}

/* the screen changed size (the session's watcher, Settings' resolution):
 * the title bars take their share of the new one, the compositor's for
 * Linux programs too */
const WCHAR *look_rescale(void)
{
    look_metrics(look_frame_style());
    effects_write_conf();
    if (g_reframe) CloseHandle(g_reframe);
    g_reframe = CreateThread(NULL, 0, reframe_later, NULL, 0, NULL);
    return NULL;
}

/* before the program ends (--set): the windows have their new corners */
void look_wait(void)
{
    if (g_reframe) WaitForSingleObject(g_reframe, 5000);
}

/* ---- pins: shortcuts in User Pinned\TaskBar, as the taskbar keeps them (0485) ---- */
static BOOL pins_dir(WCHAR *dir)
{
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, dir))) return FALSE;
    lstrcatW(dir, L"\\Microsoft\\Internet Explorer\\Quick Launch\\User Pinned\\TaskBar");
    SHCreateDirectoryExW(NULL, dir, NULL);
    return TRUE;
}

static BOOL pin(const WCHAR *dir, const WCHAR *name, const WCHAR *exe)
{
    WCHAR lnk[MAX_PATH];
    IShellLinkW *link;
    IPersistFile *file;
    BOOL ok = FALSE;

    if (!exe[0] || GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return FALSE;
    _snwprintf(lnk, MAX_PATH, L"%ls\\%ls.lnk", dir, name);
    lnk[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(lnk) != INVALID_FILE_ATTRIBUTES) return TRUE;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) return FALSE;
    IShellLinkW_SetPath(link, exe);
    if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
        ok = SUCCEEDED(IPersistFile_Save(file, lnk, TRUE));
        IPersistFile_Release(file);
    }
    IShellLinkW_Release(link);
    return ok;
}

/* the Rounded look's pins, if nothing is pinned yet: File Explorer, the web
 * browser that opens links, and Settings -- in that order */
static void default_pins(void)
{
    WCHAR dir[MAX_PATH], browser[MAX_PATH] = L"", pattern[MAX_PATH], order[3 * 64 + 1], *p = order;
    const WCHAR *names[3] = { L"File Explorer", L"Web browser", L"Settings" };
    WCHAR paths[3][MAX_PATH];
    DWORD len = MAX_PATH;
    WIN32_FIND_DATAW fd;
    HANDLE find;
    HKEY key;
    int i;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (!pins_dir(dir)) return;
    _snwprintf(pattern, MAX_PATH, L"%ls\\*.lnk", dir);
    if ((find = FindFirstFileW(pattern, &fd)) != INVALID_HANDLE_VALUE) { FindClose(find); return; }
    AssocQueryStringW(0, ASSOCSTR_EXECUTABLE, L"http", L"open", browser, &len);
    GetWindowsDirectoryW(paths[0], MAX_PATH);
    lstrcatW(paths[0], L"\\explorer.exe");
    lstrcpynW(paths[1], browser, MAX_PATH);
    GetModuleFileNameW(NULL, paths[2], MAX_PATH);   /* this program: Settings */
    for (i = 0; i < 3; i++) {
        if (!pin(dir, names[i], paths[i])) continue;
        lstrcpyW(p, names[i]); lstrcatW(p, L".lnk");
        p += lstrlenW(p) + 1;
    }
    *p++ = 0;
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, TASKBAR, 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL)) {
        RegSetValueExW(key, L"PinOrder", 0, REG_MULTI_SZ, (BYTE *)order, (DWORD)((p - order) * sizeof(WCHAR)));
        RegCloseKey(key);
    }
}

/* the taskbar's look: Taskbar\Look, or as an older Settings left it (the
 * Horizon and Glass bars by Taskbar\Style, the Rounded one by the window style) */
int look_taskbar_look(void)
{
    DWORD v = reg_dword(HKEY_CURRENT_USER, TASKBAR, L"Look", 0xffffffff);
    if (v < LOOK_COUNT) return (int)v;
    v = look_taskbar_style();
    return v == 1 ? LOOK_HORIZON : v == 2 ? LOOK_GLASS : look_rounded() ? LOOK_ROUNDED : LOOK_CLASSIC;
}

/* Start's look: Start\Look, or as an older Settings left it */
int look_start_look(void)
{
    DWORD v = reg_dword(HKEY_CURRENT_USER, START, L"Look", 0xffffffff);
    if (v < LOOK_COUNT) return (int)v;
    if (reg_dword(HKEY_CURRENT_USER, START, L"Centered", 0)) return LOOK_ROUNDED;
    v = look_taskbar_style();
    return v == 1 ? LOOK_HORIZON : v == 2 ? LOOK_GLASS : LOOK_CLASSIC;
}

/* the look the window frames, the taskbar and Start share, or -1: Custom */
int look_whole(void)
{
    int f = look_frame_style();
#ifdef SG_MUTANT_NOCUSTOM
    return f;
#endif
    return look_taskbar_look() == f && look_start_look() == f ? f : -1;
}

/* the taskbar in a look: Classic (labelled buttons at the left, Task View),
 * Rounded (taller, icons in the middle, a search box), Horizon (a bright blue
 * bar, labelled buttons combined when full) or Glass (a dark glass bar, icon
 * buttons with pins); neither older bar had Task View */
const WCHAR *look_set_taskbar(int look)
{
    BOOL rounded = look == LOOK_ROUNDED;
    DWORD glom = look == LOOK_ROUNDED || look == LOOK_GLASS ? 0 : look == LOOK_HORIZON ? 1 : 2;

    if (look < 0 || look >= LOOK_COUNT) return L"unknown look";
    if (!reg_set_dword(HKEY_CURRENT_USER, TASKBAR, L"Look", look)) return L"the setting could not be saved";
    reg_set_dword(HKEY_CURRENT_USER, TASKBAR, L"Style", look == LOOK_HORIZON ? 1 : look == LOOK_GLASS ? 2 : 0);
    reg_set_dword(HKEY_CURRENT_USER, ADV, L"TaskbarAl", rounded ? 1 : 0);
    reg_set_dword(HKEY_CURRENT_USER, ADV, L"TaskbarGlomLevel", glom);
    reg_set_dword(HKEY_CURRENT_USER, SEARCHKEY, L"SearchboxTaskbarMode", rounded ? 1 : 0);
    reg_set_dword(HKEY_CURRENT_USER, ADV, L"ShowTaskViewButton", look == LOOK_HORIZON || look == LOOK_GLASS ? 0 : 1);
    reg_set_dword(HKEY_CURRENT_USER, TASKBAR, L"ShowDesktops", 0);   /* off by default in every look; the Taskbar toggle turns it on */
    reg_set_dword(HKEY_CURRENT_USER, TASKBAR, L"Color", 0);
    if (rounded || look == LOOK_GLASS) default_pins();
    broadcast(L"TraySettings");
    broadcast(L"ImmersiveColorSet");   /* the taskbar takes the look's height and colours */
    return NULL;
}

/* Start in a look: Classic (tiles), Rounded (centred, pinned and
 * recommended), Horizon (two columns, All Programs) or Glass (two columns,
 * a search box); sg-start reads it each time it opens */
const WCHAR *look_set_start(int look)
{
    if (look < 0 || look >= LOOK_COUNT) return L"unknown look";
    if (!reg_set_dword(HKEY_CURRENT_USER, START, L"Look", look)) return L"the setting could not be saved";
    reg_set_dword(HKEY_CURRENT_USER, START, L"Centered", look == LOOK_ROUNDED);
    return NULL;
}

/* the whole look: Classic (the default), Rounded, Horizon or Glass -- the
 * window frames, the taskbar and Start each in it; afterwards each can be
 * changed on its own page (Themes then says Custom) */
const WCHAR *look_apply(int look)
{
    const WCHAR *why;
    if (look < 0 || look >= LOOK_COUNT) return L"unknown look";
    if ((why = look_set_taskbar(look)) || (why = look_set_start(look))) return why;
    return look_set_frame(look);   /* the windows' frames: the look's own */
}
