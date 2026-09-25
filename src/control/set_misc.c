/* sg-control -- Settings > Ease of Access, Privacy, Update & Security.
 *
 * Ease of Access is SystemParametersInfo and the Accessibility keys, as on
 * Windows. Privacy keeps Windows' ConsentStore values: the device switch in
 * HKLM (an administrator's, written by the elevated copy of this program)
 * and the per-user switches in HKCU -- voice typing (sg-dictate) does not
 * listen when either says Deny. Windows Update is sg-session's staged
 * updates: apt's list of what is pending (sg-settingsctl), a check through
 * sg-admind, the installation at the next restart.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"

/* ---- Ease of Access --------------------------------------------------------------------------- */
static const WCHAR ACCESS[] = L"Software\\Microsoft\\Accessibility";
static const WCHAR PERSONALIZE[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
enum {
    CMD_TEXTSIZE = CMD_PAGE_FIRST + 1, CMD_ANIM, CMD_TRANSP, CMD_SCROLLBARS, CMD_STICKY, CMD_TOGGLE, CMD_FILTER,
    CMD_UNDERLINE, CMD_PRTSC, CMD_CURSOR, CMD_TRAILS, CMD_APPLYTEXT,
    CMD_MAG_ON, CMD_MAG_ZOOM, CMD_MAG_INC, CMD_MAG_START, CMD_MAG_INVERT, CMD_MAG_VIEW, CMD_MAG_FMOUSE,
    CMD_MAG_FFOCUS, CMD_MAG_FCARET, CMD_OSK,
};

void set_build_eoa_display(void)
{
    BOOL anim = TRUE;
    int y = st_title(L"Display");
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    y = st_head(y, L"Make text bigger");
    st_slider(&y, L"Drag the slider until the sample text is easy to read", 100, 225,
              (int)reg_dword(HKEY_CURRENT_USER, ACCESS, L"TextScaleFactor", 100), CMD_TEXTSIZE);
    st_button(&y, L"Apply", CMD_APPLYTEXT);
    y = st_head(y, L"Simplify and personalize Windows");
    st_toggle(&y, L"Show animations in Windows", anim, CMD_ANIM);
    st_toggle(&y, L"Show transparency in Windows", reg_dword(HKEY_CURRENT_USER, PERSONALIZE, L"EnableTransparency", 1) != 0, CMD_TRANSP);
    st_toggle(&y, L"Automatically hide scroll bars in Windows",
              reg_dword(HKEY_CURRENT_USER, L"Control Panel\\Accessibility", L"DynamicScrollbars", 0) != 0, CMD_SCROLLBARS);
}

/* ---- Ease of Access > Magnifier (sg-shell's magnify.exe) and the On-Screen Keyboard ----------- */
static const WCHAR MAGKEY[] = L"Software\\Microsoft\\ScreenMagnifier";
static const WCHAR RUNKEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const DWORD MAG_ZOOMS[] = { 100, 125, 150, 200, 250, 300, 400, 500, 600, 800, 1000, 1200, 1600 };
static const DWORD MAG_INCS[] = { 25, 50, 100, 150, 200, 400 };
static const DWORD MAG_VIEWS[] = { 2, 1, 3 };  /* Docked, Full screen, Lens: MagnificationMode */

static HWND magnifier_window(void) { return FindWindowW(L"SgMagnifier", NULL); }

/* a running Magnifier takes the command (and reads its settings again) */
static void magnifier_run(const WCHAR *args)
{
    ShellExecuteW(NULL, NULL, L"magnify.exe", args, NULL, SW_SHOWNORMAL);
}

static void magnifier_reload(void)
{
    if (magnifier_window()) magnifier_run(L"/reload");
}

static BOOL run_value(const WCHAR *name)
{
    HKEY k;
    BOOL ret = FALSE;
    if (!RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, KEY_QUERY_VALUE, &k))
    {
        ret = !RegQueryValueExW(k, name, NULL, NULL, NULL, NULL);
        RegCloseKey(k);
    }
    return ret;
}

void set_build_eoa_magnifier(void)
{
    static const WCHAR *views[] = { L"Docked", L"Full screen", L"Lens" };
    static WCHAR zoom_text[ARRAYSIZE(MAG_ZOOMS)][8], inc_text[ARRAYSIZE(MAG_INCS)][8];
    const WCHAR *zooms[ARRAYSIZE(MAG_ZOOMS)], *incs[ARRAYSIZE(MAG_INCS)];
    DWORD zoom = reg_dword(HKEY_CURRENT_USER, MAGKEY, L"Magnification", 200);
    DWORD inc = reg_dword(HKEY_CURRENT_USER, MAGKEY, L"ZoomIncrement", 100);
    DWORD mode = reg_dword(HKEY_CURRENT_USER, MAGKEY, L"MagnificationMode", 1);
    int i, zsel = 3, isel = 2, vsel = 1;
    int y = st_title(L"Magnifier");

    for (i = 0; i < (int)ARRAYSIZE(MAG_ZOOMS); i++)
    {
        _snwprintf(zoom_text[i], 8, L"%lu%%", (unsigned long)MAG_ZOOMS[i]);
        zooms[i] = zoom_text[i];
        if (MAG_ZOOMS[i] == zoom) zsel = i;
    }
    for (i = 0; i < (int)ARRAYSIZE(MAG_INCS); i++)
    {
        _snwprintf(inc_text[i], 8, L"%lu%%", (unsigned long)MAG_INCS[i]);
        incs[i] = inc_text[i];
        if (MAG_INCS[i] == inc) isel = i;
    }
    for (i = 0; i < 3; i++) if (MAG_VIEWS[i] == mode) vsel = i;
    y = st_para(y, L"Magnifier makes part or all of your screen bigger so you can see words and images better.");
    y = st_head(y, L"Use Magnifier");
    st_toggle(&y, L"Turn on Magnifier", magnifier_window() != NULL, CMD_MAG_ON);
    y = st_para(y - S(6), L"Press the Windows logo key + Plus (+) to turn on Magnifier. Press the Windows logo key + Esc to turn it off.");
    st_combo(&y, L"Change zoom level", zooms, ARRAYSIZE(MAG_ZOOMS), zsel, CMD_MAG_ZOOM);
    st_combo(&y, L"Change zoom increments", incs, ARRAYSIZE(MAG_INCS), isel, CMD_MAG_INC);
    st_toggle(&y, L"Start Magnifier after sign-in", run_value(L"Magnifier"), CMD_MAG_START);
    st_toggle(&y, L"Invert colors (Ctrl + Alt + I)", reg_dword(HKEY_CURRENT_USER, MAGKEY, L"Invert", 0) != 0, CMD_MAG_INVERT);
    y = st_head(y, L"Change Magnifier view");
    st_combo(&y, L"Choose a view", views, 3, vsel, CMD_MAG_VIEW);
    y = st_para(y - S(6), L"Full screen: Ctrl + Alt + F. Lens: Ctrl + Alt + L. Docked: Ctrl + Alt + D.");
    y = st_head(y, L"Have Magnifier follow");
    st_checkbox(&y, L"The mouse pointer", reg_dword(HKEY_CURRENT_USER, MAGKEY, L"FollowMouse", 1) != 0, CMD_MAG_FMOUSE);
    st_checkbox(&y, L"The keyboard focus", reg_dword(HKEY_CURRENT_USER, MAGKEY, L"FollowFocus", 1) != 0, CMD_MAG_FFOCUS);
    st_checkbox(&y, L"The text cursor", reg_dword(HKEY_CURRENT_USER, MAGKEY, L"FollowCaret", 1) != 0, CMD_MAG_FCARET);
}

static BOOL set_cmd_magnifier(int id, int code, HWND ctl, BOOL on)
{
    int i;
    switch (id) {
    case CMD_MAG_ON:
        if (on && !magnifier_window()) magnifier_run(NULL);
        else if (!on && magnifier_window()) PostMessageW(magnifier_window(), WM_CLOSE, 0, 0);
        return TRUE;
    case CMD_MAG_ZOOM:
        if (code == CBN_SELCHANGE && (i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0)) >= 0) {
            reg_set_dword(HKEY_CURRENT_USER, MAGKEY, L"Magnification", MAG_ZOOMS[i]);
            magnifier_reload();
        }
        return TRUE;
    case CMD_MAG_INC:
        if (code == CBN_SELCHANGE && (i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0)) >= 0) {
            reg_set_dword(HKEY_CURRENT_USER, MAGKEY, L"ZoomIncrement", MAG_INCS[i]);
            magnifier_reload();
        }
        return TRUE;
    case CMD_MAG_VIEW:
        if (code == CBN_SELCHANGE && (i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0)) >= 0) {
            reg_set_dword(HKEY_CURRENT_USER, MAGKEY, L"MagnificationMode", MAG_VIEWS[i]);
            magnifier_reload();
        }
        return TRUE;
    case CMD_MAG_START:
    {
        HKEY k;
        if (!RegCreateKeyExW(HKEY_CURRENT_USER, RUNKEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) {
            if (on) RegSetValueExW(k, L"Magnifier", 0, REG_SZ, (const BYTE *)L"magnify.exe", sizeof(L"magnify.exe"));
            else RegDeleteValueW(k, L"Magnifier");
            RegCloseKey(k);
        }
        return TRUE;
    }
    case CMD_MAG_INVERT: reg_set_dword(HKEY_CURRENT_USER, MAGKEY, L"Invert", on); magnifier_reload(); return TRUE;
    case CMD_MAG_FMOUSE: reg_set_dword(HKEY_CURRENT_USER, MAGKEY, L"FollowMouse", on); magnifier_reload(); return TRUE;
    case CMD_MAG_FFOCUS: reg_set_dword(HKEY_CURRENT_USER, MAGKEY, L"FollowFocus", on); magnifier_reload(); return TRUE;
    case CMD_MAG_FCARET: reg_set_dword(HKEY_CURRENT_USER, MAGKEY, L"FollowCaret", on); magnifier_reload(); return TRUE;
    case CMD_OSK:
    {
        HWND osk = FindWindowW(L"OSKMainClass", NULL);
        if (on && !osk) ShellExecuteW(NULL, NULL, L"osk.exe", NULL, NULL, SW_SHOWNORMAL);
        else if (!on && osk) PostMessageW(osk, WM_CLOSE, 0, 0);
        return TRUE;
    }
    }
    return FALSE;
}

void set_build_eoa_keyboard(void)
{
    STICKYKEYS sk = { sizeof(sk) };
    TOGGLEKEYS tk = { sizeof(tk) };
    FILTERKEYS fk = { sizeof(fk) };
    BOOL cues = FALSE;
    int y = st_title(L"Keyboard");
    SystemParametersInfoW(SPI_GETSTICKYKEYS, sizeof(sk), &sk, 0);
    SystemParametersInfoW(SPI_GETTOGGLEKEYS, sizeof(tk), &tk, 0);
    SystemParametersInfoW(SPI_GETFILTERKEYS, sizeof(fk), &fk, 0);
    SystemParametersInfoW(SPI_GETKEYBOARDCUES, 0, &cues, 0);
    y = st_head(y, L"Use the On-Screen Keyboard");
    st_toggle(&y, L"Turns on the On-Screen Keyboard", FindWindowW(L"OSKMainClass", NULL) != NULL, CMD_OSK);
    y = st_para(y - S(6), L"Press the Windows logo key + Ctrl + O to turn the On-Screen Keyboard on or off.");
    y = st_head(y, L"Use Sticky Keys");
    st_toggle(&y, L"Press one key at a time for keyboard shortcuts", (sk.dwFlags & SKF_STICKYKEYSON) != 0, CMD_STICKY);
    y = st_head(y, L"Use Toggle Keys");
    st_toggle(&y, L"Play a sound whenever you press Caps Lock, Num Lock, or Scroll Lock", (tk.dwFlags & TKF_TOGGLEKEYSON) != 0, CMD_TOGGLE);
    y = st_head(y, L"Use Filter Keys");
    st_toggle(&y, L"Ignore brief or repeated keystrokes and change keyboard repeat rates", (fk.dwFlags & FKF_FILTERKEYSON) != 0, CMD_FILTER);
    y = st_head(y, L"Make it easier to use keyboard shortcuts");
    st_toggle(&y, L"Underline access keys when available", cues, CMD_UNDERLINE);
    y = st_head(y, L"Print Screen shortcut");
    st_toggle(&y, L"Use the PrtScn button to open screen snipping",
              reg_dword(HKEY_CURRENT_USER, L"Control Panel\\Keyboard", L"PrintScreenKeyForSnippingEnabled", 1) != 0, CMD_PRTSC);
}

void set_build_eoa_mouse(void)
{
    UINT trails = 0;
    int y = st_title(L"Mouse pointer");
    SystemParametersInfoW(SPI_GETMOUSETRAILS, 0, &trails, 0);
    y = st_head(y, L"Change pointer size");
    st_slider(&y, NULL, 1, 15, ((int)reg_dword(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize", 32) - 16) / 16, CMD_CURSOR);
    y = st_para(y - S(6), L"Programs use the new size the next time they start.");
    y = st_head(y, L"Pointer trails");
    st_toggle(&y, L"Show pointer trails", trails > 1, CMD_TRAILS);
}

BOOL set_cmd_eoa(int id, int code, HWND ctl)
{
    const UINT F = SPIF_UPDATEINIFILE | SPIF_SENDCHANGE;
    BOOL on = st_checked(ctl);
    if (set_cmd_magnifier(id, code, ctl, on)) return TRUE;
    switch (id) {
    case CMD_TEXTSIZE: return TRUE;
    case CMD_APPLYTEXT: {
        HWND s = GetDlgItem(g_page, CMD_TEXTSIZE);
        DWORD_PTR r;
        if (s) reg_set_dword(HKEY_CURRENT_USER, ACCESS, L"TextScaleFactor", (DWORD)SendMessageW(s, TBM_GETPOS, 0, 0));
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"WindowMetrics", SMTO_ABORTIFHUNG, 2000, &r);
        st_status(L"Some apps won't show the new text size until you close and open them again.");
        return TRUE;
    }
    case CMD_ANIM: SystemParametersInfoW(SPI_SETCLIENTAREAANIMATION, 0, (void *)(INT_PTR)on, F); return TRUE;
    case CMD_TRANSP: reg_set_dword(HKEY_CURRENT_USER, PERSONALIZE, L"EnableTransparency", on); return TRUE;
    case CMD_SCROLLBARS: reg_set_dword(HKEY_CURRENT_USER, L"Control Panel\\Accessibility", L"DynamicScrollbars", on); return TRUE;
    case CMD_STICKY: {
        STICKYKEYS sk = { sizeof(sk) };
        SystemParametersInfoW(SPI_GETSTICKYKEYS, sizeof(sk), &sk, 0);
        sk.dwFlags = on ? sk.dwFlags | SKF_STICKYKEYSON : sk.dwFlags & ~SKF_STICKYKEYSON;
        SystemParametersInfoW(SPI_SETSTICKYKEYS, sizeof(sk), &sk, F);
        return TRUE;
    }
    case CMD_TOGGLE: {
        TOGGLEKEYS tk = { sizeof(tk) };
        SystemParametersInfoW(SPI_GETTOGGLEKEYS, sizeof(tk), &tk, 0);
        tk.dwFlags = on ? tk.dwFlags | TKF_TOGGLEKEYSON : tk.dwFlags & ~TKF_TOGGLEKEYSON;
        SystemParametersInfoW(SPI_SETTOGGLEKEYS, sizeof(tk), &tk, F);
        return TRUE;
    }
    case CMD_FILTER: {
        FILTERKEYS fk = { sizeof(fk) };
        SystemParametersInfoW(SPI_GETFILTERKEYS, sizeof(fk), &fk, 0);
        fk.dwFlags = on ? fk.dwFlags | FKF_FILTERKEYSON : fk.dwFlags & ~FKF_FILTERKEYSON;
        SystemParametersInfoW(SPI_SETFILTERKEYS, sizeof(fk), &fk, F);
        return TRUE;
    }
    case CMD_UNDERLINE: SystemParametersInfoW(SPI_SETKEYBOARDCUES, 0, (void *)(INT_PTR)on, F); return TRUE;
    case CMD_PRTSC: reg_set_dword(HKEY_CURRENT_USER, L"Control Panel\\Keyboard", L"PrintScreenKeyForSnippingEnabled", on); return TRUE;
    case CMD_CURSOR:
        if (code == (PG_SCROLL_CODE | TB_ENDTRACK)) {
            reg_set_dword(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"CursorBaseSize", 16 + 16 * (DWORD)SendMessageW(ctl, TBM_GETPOS, 0, 0));
            SystemParametersInfoW(SPI_SETCURSORS, 0, NULL, F);
        }
        return TRUE;
    case CMD_TRAILS: SystemParametersInfoW(SPI_SETMOUSETRAILS, on ? 7 : 0, NULL, F); return TRUE;
    }
    return FALSE;
}

/* ---- Privacy ---------------------------------------------------------------------------------- */
#define CONSENT L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\"
enum { CMD_DEVICE = SHIELD_ID(CMD_PAGE_FIRST + 1), CMD_APPS = CMD_PAGE_FIRST + 2, CMD_DESKTOP = CMD_PAGE_FIRST + 3,
       CMD_ADID = CMD_PAGE_FIRST + 4, CMD_LANGLIST = CMD_PAGE_FIRST + 5, CMD_TRACK = CMD_PAGE_FIRST + 6,
       CMD_SUGGEST = CMD_PAGE_FIRST + 7 };
static const WCHAR *g_cap;      /* the page's capability: microphone, webcam, location */

static BOOL consent(HKEY root, const WCHAR *cap, const WCHAR *sub)
{
    WCHAR key[300], v[16] = L"";
    _snwprintf(key, ARRAYSIZE(key), CONSENT L"%ls%ls%ls", cap, sub ? L"\\" : L"", sub ? sub : L"");
    reg_sz(root, key, L"Value", v, ARRAYSIZE(v));
    return lstrcmpiW(v, L"Deny") != 0;
}

static void set_consent(HKEY root, const WCHAR *cap, const WCHAR *sub, BOOL allow)
{
    WCHAR key[300];
    _snwprintf(key, ARRAYSIZE(key), CONSENT L"%ls%ls%ls", cap, sub ? L"\\" : L"", sub ? sub : L"");
    reg_set_sz(root, key, L"Value", allow ? L"Allow" : L"Deny");
}

/* what voice typing and Sound ask: may a desktop program use the microphone? */
BOOL privacy_mic_allowed(void)
{
    return consent(HKEY_LOCAL_MACHINE, L"microphone", NULL) && consent(HKEY_CURRENT_USER, L"microphone", NULL) &&
           consent(HKEY_CURRENT_USER, L"microphone", L"NonPackaged");
}

/* /admin consent CAP on|off: the device-wide switch, by the elevated copy */
int privacy_admin_consent(const WCHAR *cap, const WCHAR *v)
{
    if (lstrcmpW(cap, L"microphone") && lstrcmpW(cap, L"webcam") && lstrcmpW(cap, L"location")) return 2;
    set_consent(HKEY_LOCAL_MACHINE, cap, NULL, !lstrcmpW(v, L"on"));
    return consent(HKEY_LOCAL_MACHINE, cap, NULL) == !lstrcmpW(v, L"on") ? 0 : 1;
}

static void build_capability(const WCHAR *title, const WCHAR *cap, const WCHAR *what)
{
    WCHAR line[300];
    BOOL device = consent(HKEY_LOCAL_MACHINE, cap, NULL);
    int y = st_title(title);
    HWND c;
    g_cap = cap;
    _snwprintf(line, ARRAYSIZE(line), L"Allow access to the %ls on this device", what);
    y = st_head(y, line);
    _snwprintf(line, ARRAYSIZE(line), L"%ls access for this device is %ls", title, device ? L"on" : L"off");
    y = st_text(y, line);
    st_button(&y, L"Change", CMD_DEVICE);
    _snwprintf(line, ARRAYSIZE(line), L"Allow apps to access your %ls", what);
    y = st_head(y, line);
    c = st_toggle(&y, NULL, consent(HKEY_CURRENT_USER, cap, NULL), CMD_APPS);
    EnableWindow(c, device);
    _snwprintf(line, ARRAYSIZE(line), L"Allow desktop apps to access your %ls", what);
    y = st_head(y, line);
    c = st_toggle(&y, NULL, consent(HKEY_CURRENT_USER, cap, L"NonPackaged"), CMD_DESKTOP);
    EnableWindow(c, device && consent(HKEY_CURRENT_USER, cap, NULL));
    if (!lstrcmpW(cap, L"microphone"))
        y = st_para(y, L"Voice typing (Win+H) is a desktop app: with any of these off, it does not listen.");
}

void set_build_priv_mic(void) { build_capability(L"Microphone", L"microphone", L"microphone"); }
void set_build_priv_camera(void) { build_capability(L"Camera", L"webcam", L"camera"); }
void set_build_priv_location(void) { build_capability(L"Location", L"location", L"location"); }

void set_build_priv_general(void)
{
    int y = st_title(L"General");
    y = st_head(y, L"Change privacy options");
    st_toggle(&y, L"Let apps use advertising ID to make ads more interesting to you based on your app activity",
              reg_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", L"Enabled", 0) != 0, CMD_ADID);
    st_toggle(&y, L"Let websites provide locally relevant content by accessing my language list",
              reg_dword(HKEY_CURRENT_USER, L"Control Panel\\International\\User Profile", L"HttpAcceptLanguageOptOut", 0) == 0, CMD_LANGLIST);
    st_toggle(&y, L"Let Windows track app launches to improve Start and search results",
              reg_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"Start_TrackProgs", 1) != 0, CMD_TRACK);
    st_toggle(&y, L"Show me suggested content in the Settings app",
              reg_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager",
                        L"SubscribedContent-338393Enabled", 0) != 0, CMD_SUGGEST);
    y = st_para(y, L"Stained Glass OS sends no usage data anywhere. These switches are kept for programs that read them.");
}

BOOL set_cmd_privacy(int id, int code, HWND ctl)
{
    WCHAR args[80];
    (void)code;
    switch (id) {
    case CMD_DEVICE:
        _snwprintf(args, ARRAYSIZE(args), L"/admin consent %ls %ls", g_cap, consent(HKEY_LOCAL_MACHINE, g_cap, NULL) ? L"off" : L"on");
        if (run_elevated(args)) refresh_when_back();
        return TRUE;
    case CMD_APPS: set_consent(HKEY_CURRENT_USER, g_cap, NULL, st_checked(ctl)); refresh_page(); break;
    case CMD_DESKTOP: set_consent(HKEY_CURRENT_USER, g_cap, L"NonPackaged", st_checked(ctl)); break;
    case CMD_ADID: reg_set_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\AdvertisingInfo", L"Enabled", st_checked(ctl)); return TRUE;
    case CMD_LANGLIST: reg_set_dword(HKEY_CURRENT_USER, L"Control Panel\\International\\User Profile", L"HttpAcceptLanguageOptOut", !st_checked(ctl)); return TRUE;
    case CMD_TRACK: reg_set_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced", L"Start_TrackProgs", st_checked(ctl)); return TRUE;
    case CMD_SUGGEST:
        reg_set_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager",
                      L"SubscribedContent-338393Enabled", st_checked(ctl));
        return TRUE;
    default: return FALSE;
    }
    /* voice typing re-reads its settings when told */
    {
        HWND bar = FindWindowW(L"SgDictateBar", NULL);
        if (bar && !privacy_mic_allowed()) PostMessageW(bar, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

/* ---- Windows Update ------------------------------------------------------------------------------ */
enum { CMD_CHECK = SHIELD_ID(CMD_PAGE_FIRST + 1), CMD_RESTART = CMD_PAGE_FIRST + 2, CMD_REFRESH = CMD_PAGE_FIRST + 3 };

void set_build_update(void)
{
    struct ufacts u;
    struct hentry h[8];
    WCHAR line[400], err[256] = L"";
    BOOL ok, staged = FALSE;
    char *ans, buf[512];
    const char *pos = NULL;
    int y = st_title(L"Updates"), n = 0, i, nh;
    update_gather(&u);
    ans = ctl_run(L"updates", &ok, err, ARRAYSIZE(err), 60000);
    if (ans && ctl_line(ans, "STAGED", NULL, buf, sizeof(buf))) staged = !strcmp(buf, "yes");
    staged = staged || u.pending;
    while (ans && ctl_line(ans, "UPDATE", &pos, buf, sizeof(buf))) n++;
    if (staged) y = st_card(y, IC_G_UPDATE, L"Restart required", L"Updates are downloaded and will install when you restart.");
    else if (n) {
        _snwprintf(line, ARRAYSIZE(line), L"%d update%ls available", n, n == 1 ? L" is" : L"s are");
        y = st_card(y, IC_G_UPDATE, L"Updates available", line);
    } else y = st_card(y, IC_G_UPDATE, L"You're up to date", u.checked ? u.last : L"Stained Glass OS checks for updates every day.");
    if (u.checked) { _snwprintf(line, ARRAYSIZE(line), L"Last checked: %ls", u.last); y = st_text(y, line); }
    if (u.managed) y = st_para(y, L"*Some settings are managed by your organization.");
    y += S(4);
    if (staged) st_button(&y, L"Restart now", CMD_RESTART);
    st_button(&y, L"Check for updates", CMD_CHECK);
    if (n) {
        y = st_head(y, L"Available updates");
        pos = NULL;
        for (i = 0; i < 12 && ctl_line(ans, "UPDATE", &pos, buf, sizeof(buf)); i++) {
            WCHAR pkg[128], from[64], to[64];
            ctl_field(buf, 0, pkg, ARRAYSIZE(pkg)); ctl_field(buf, 1, from, ARRAYSIZE(from)); ctl_field(buf, 2, to, ARRAYSIZE(to));
            _snwprintf(line, ARRAYSIZE(line), L"%ls %ls (installed: %ls)", pkg, to, from);
            y = st_text(y, line);
        }
        if (n > 12) { _snwprintf(line, ARRAYSIZE(line), L"... and %d more", n - 12); y = st_text(y, line); }
        y = st_para(y, L"Updates download in the background and install the next time you restart.");
    } else if (!ok && err[0]) y = st_para(y, err);
    free(ans);
    y = st_head(y, L"Update history");
    nh = update_history(h, ARRAYSIZE(h));
    for (i = 0; i < nh; i++) {
        pg_text(st_x(), y, st_w(), S(22), g_font_body, COL_TEXT, h[i].what, DT_SINGLELINE | DT_END_ELLIPSIS);
        pg_text(st_x(), y + S(22), st_w(), S(18), g_font_small, COL_SUBTLE, h[i].when, DT_SINGLELINE);
        y += S(46);
    }
    if (!nh) y = st_para(y, L"No updates have been installed yet.");
    if (u.nsources) {
        y = st_head(y, L"Where updates come from");
        for (i = 0; i < u.nsources; i++) y = st_text(y, u.sources[i]);
    }
}

BOOL set_cmd_update(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    switch (id) {
    case CMD_CHECK: if (run_elevated(L"/admin update-check")) refresh_when_back(); return TRUE;
    case CMD_RESTART:
        if (MessageBoxW(g_main, L"Restart now to install the updates? Save your work first.", L"Updates",
                        MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_FLAG_PLANNED);
        return TRUE;
    case CMD_REFRESH: refresh_page(); return TRUE;
    }
    return FALSE;
}

/* ---- Recovery ------------------------------------------------------------------------------------- */
enum { CMD_ADV_RESTART = CMD_PAGE_FIRST + 1, CMD_RESET_INFO = CMD_PAGE_FIRST + 2 };

void set_build_recovery(void)
{
    int y = st_title(L"Recovery");
    y = st_head(y, L"Reset this PC");
    y = st_para(y, L"To start again with a clean system, reinstall Stained Glass OS from its installation media: "
                   L"Setup can keep your other partitions. Back up your files first.");
    y = st_head(y, L"Advanced startup");
    y = st_para(y, L"Start up from a device or disc (such as a USB drive or DVD), or choose another system to start: "
                   L"the boot menu is shown when your PC restarts.");
    st_button(&y, L"Restart now", CMD_ADV_RESTART);
    y = st_head(y, L"More recovery options");
    y = st_para(y, L"Updates are installed before anyone signs in, at the next restart, so an interrupted update does not "
                   L"leave programs half replaced.");
}

BOOL set_cmd_recovery(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    if (id == CMD_ADV_RESTART) {
        if (MessageBoxW(g_main, L"Restart now? Save your work first.", L"Advanced startup", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_FLAG_PLANNED);
        return TRUE;
    }
    return FALSE;
}
