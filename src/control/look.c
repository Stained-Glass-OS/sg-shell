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

/* each style's title bar and frame sizes, as its era had them, in pixels at
 * 96 DPI, and whether the title is bold (Horizon's was) */
static void look_metrics(int style)
{
    static const int caption[LOOK_COUNT] = { 18, 18, 25, 21 }, button[LOOK_COUNT] = { 18, 18, 25, 26 };
    static const int border[LOOK_COUNT] = { 1, 1, 2, 7 };   /* Wine keeps the padded border in this one */
    NONCLIENTMETRICSW ncm;

    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) return;
    if (ncm.iCaptionHeight == caption[style] && ncm.iCaptionWidth == button[style] && ncm.iBorderWidth == border[style] &&
        (ncm.lfCaptionFont.lfWeight >= FW_BOLD) == (style == LOOK_HORIZON))
        return;
    ncm.iCaptionHeight = caption[style];
    ncm.iCaptionWidth = button[style];
    ncm.iBorderWidth = border[style];
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

/* the whole look: Classic (the default), Rounded, Horizon or Glass. Horizon
 * and Glass keep square windows and Start at the left; Horizon's buttons
 * have labels (combined when the bar is full), Glass's are icons with the
 * pins, as those desktops had them; neither had Task View. */
const WCHAR *look_apply(int look)
{
    BOOL rounded = look == LOOK_ROUNDED;
    DWORD glom = look == LOOK_ROUNDED || look == LOOK_GLASS ? 0 : look == LOOK_HORIZON ? 1 : 2;

    if (look < 0 || look >= LOOK_COUNT) return L"unknown look";
    reg_set_dword(HKEY_CURRENT_USER, ADV, L"TaskbarAl", rounded ? 1 : 0);
    reg_set_dword(HKEY_CURRENT_USER, ADV, L"TaskbarGlomLevel", glom);
    reg_set_dword(HKEY_CURRENT_USER, SEARCHKEY, L"SearchboxTaskbarMode", rounded ? 1 : 0);
    reg_set_dword(HKEY_CURRENT_USER, ADV, L"ShowTaskViewButton", look == LOOK_HORIZON || look == LOOK_GLASS ? 0 : 1);
    reg_set_dword(HKEY_CURRENT_USER, TASKBAR, L"ShowDesktops", 0);   /* off by default in every look; the Taskbar toggle turns it on */
    reg_set_dword(HKEY_CURRENT_USER, TASKBAR, L"Color", 0);
    reg_set_dword(HKEY_CURRENT_USER, TASKBAR, L"Style", look == LOOK_HORIZON ? 1 : look == LOOK_GLASS ? 2 : 0);
    reg_set_dword(HKEY_CURRENT_USER, START, L"Centered", rounded ? 1 : 0);
    if (rounded || look == LOOK_GLASS) default_pins();
    broadcast(L"TraySettings");
    return look_set_frame(look);   /* the windows' frames: the look's own */
}
