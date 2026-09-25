/* sg-control -- Settings: the Windows 10 Settings window.
 *
 * SystemSettings.exe, Win+I and every ms-settings: URI arrive here. The
 * window is a navigation pane (back, Home, search, the category's pages)
 * beside a page; Home is a grid of the categories. Pages are built with the
 * Control Panel's page machinery (main.c) and share its logic; this file is
 * the frame, the controls a Settings page is made of (headings, switches,
 * sliders, cards), the ms-settings: map, and the bridge to sg-settingsctl,
 * the native half in sg-session.
 *
 * SG_SETTINGS_DUMP=<file>: after every page is shown, what it shows (the
 * page, its texts and controls) is written there, UTF-8, for the gates.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"
#include <shellapi.h>
#include <windowsx.h>

#define NAV_W        300
#define COL_NAV      RGB(0xF3, 0xF3, 0xF3)
#define COL_NAV_HOT  RGB(0xE6, 0xE6, 0xE6)
#define COL_NAV_SEL  RGB(0xE0, 0xDA, 0xEC)
#define COL_CARD     RGB(0xF7, 0xF7, 0xF7)
#define ID_NAV       20
#define ID_NAVSEARCH 21
#define CMD_HOMESEARCH (CMD_PAGE_FIRST + 900)
#define CMD_RESULT     (CMD_PAGE_FIRST + 1000)   /* + page */
#define CMD_TILE       (CMD_PAGE_FIRST + 1500)   /* + category */

static HWND g_nav, g_nav_search;
static COLORREF g_accent;
static WCHAR g_status[256];
static enum page_id g_status_page = PG_COUNT;

/* ---- categories ----------------------------------------------------------------------- */
struct category { const WCHAR *name, *sub; int icon; enum page_id pages[8]; const WCHAR *keywords; };
static const struct category CATS[] = {
    { L"System", L"Display, sound, notifications, power", IC_G_SYSTEM,
      { PG_S_DISPLAY, PG_S_SOUND, PG_S_NOTIFY, PG_S_POWER, PG_S_STORAGE, PG_S_MULTITASK, PG_S_ABOUT, PG_COUNT } },
    { L"Devices", L"Bluetooth, mouse, typing", IC_G_DEVICES,
      { PG_S_BLUETOOTH, PG_S_MOUSE, PG_S_TYPING, PG_COUNT } },
    { L"Network & Internet", L"Wi-Fi, Ethernet, proxy", IC_G_NETWORK,
      { PG_S_NETSTATUS, PG_S_WIFI, PG_S_ETHERNET, PG_S_PROXY, PG_COUNT } },
    { L"Personalization", L"Background, lock screen, colors", IC_G_PERSONAL,
      { PG_S_BACKGROUND, PG_S_COLORS, PG_S_LOCKSCREEN, PG_S_THEMES, PG_S_START, PG_S_TASKBAR, PG_COUNT } },
    { L"Apps", L"Uninstall, defaults, startup", IC_G_APPS,
      { PG_S_APPS, PG_S_DEFAULTAPPS, PG_S_STARTUP, PG_COUNT } },
    { L"Accounts", L"Your account, sign-in, other users", IC_G_ACCOUNTS,
      { PG_S_YOURINFO, PG_S_SIGNIN, PG_S_OTHERUSERS, PG_COUNT } },
    { L"Time & Language", L"Speech, region, date", IC_G_TIME,
      { PG_S_DATETIME, PG_S_REGION, PG_SPEECH, PG_COUNT } },
    { L"Ease of Access", L"Text size, magnifier, keyboard, mouse pointer", IC_G_EOA,
      { PG_S_EOA_DISPLAY, PG_S_EOA_MAGNIFIER, PG_S_EOA_KEYBOARD, PG_S_EOA_MOUSE, PG_COUNT } },
    { L"Privacy", L"Microphone, camera, location", IC_G_PRIVACY,
      { PG_S_PRIV_GENERAL, PG_S_PRIV_MIC, PG_S_PRIV_CAMERA, PG_S_PRIV_LOCATION, PG_COUNT } },
    { L"Update & Security", L"Windows Update, recovery", IC_G_UPDATE,
      { PG_S_UPDATE, PG_S_RECOVERY, PG_COUNT } },
};
#define NCATS ((int)ARRAYSIZE(CATS))

/* words a search finds each page by, beyond its title */
static const struct { enum page_id page; const WCHAR *words; } KEYWORDS[] = {
    { PG_S_DISPLAY, L"screen resolution monitor scale brightness night light orientation" },
    { PG_S_SOUND, L"audio volume speakers headphones microphone output input" },
    { PG_S_NOTIFY, L"toast focus assist quiet hours banners" },
    { PG_S_POWER, L"sleep screen timeout battery suspend" },
    { PG_S_STORAGE, L"disk space drive free temporary files cleanup" },
    { PG_S_MULTITASK, L"snap windows alt tab virtual desktops" },
    { PG_S_ABOUT, L"pc name rename computer device specifications version edition processor ram" },
    { PG_S_BLUETOOTH, L"devices pair wireless headset" },
    { PG_S_MOUSE, L"primary button scroll wheel pointer speed double click" },
    { PG_S_TYPING, L"keyboard repeat rate delay cursor blink spelling" },
    { PG_S_NETSTATUS, L"network connection adapter internet sharing" },
    { PG_S_WIFI, L"wireless wlan hotspot" },
    { PG_S_ETHERNET, L"wired lan ip address dns" },
    { PG_S_PROXY, L"proxy server internet options" },
    { PG_S_BACKGROUND, L"wallpaper desktop picture solid color" },
    { PG_S_COLORS, L"accent dark mode light mode theme colour" },
    { PG_S_LOCKSCREEN, L"lock screen picture timeout" },
    { PG_S_THEMES, L"theme fonts cursor" },
    { PG_S_START, L"start menu recently added most used" },
    { PG_S_TASKBAR, L"taskbar alignment small buttons" },
    { PG_S_APPS, L"uninstall programs remove install features" },
    { PG_S_DEFAULTAPPS, L"default programs browser email photos music file types" },
    { PG_S_STARTUP, L"startup apps run at sign in" },
    { PG_S_YOURINFO, L"account user name picture administrator" },
    { PG_S_SIGNIN, L"password sign in pin lock" },
    { PG_S_OTHERUSERS, L"family add user remove account" },
    { PG_S_DATETIME, L"clock time zone date internet time ntp" },
    { PG_S_REGION, L"country locale formats currency" },
    { PG_SPEECH, L"voice typing dictation microphone speech recognition" },
    { PG_S_EOA_DISPLAY, L"text size bigger animations transparency scroll bars" },
    { PG_S_EOA_KEYBOARD, L"sticky keys filter keys toggle keys underline access keys" },
    { PG_S_EOA_MOUSE, L"pointer size cursor" },
    { PG_S_EOA_MAGNIFIER, L"magnifier zoom lens docked full screen invert colours colors" },
    { PG_S_PRIV_GENERAL, L"privacy advertising" },
    { PG_S_PRIV_MIC, L"microphone access voice" },
    { PG_S_PRIV_CAMERA, L"camera webcam access" },
    { PG_S_PRIV_LOCATION, L"location gps" },
    { PG_S_UPDATE, L"windows update check for updates install restart history" },
    { PG_S_RECOVERY, L"reset restart advanced startup recovery" },
};

static int category_of(enum page_id p)
{
    int c, i;
    for (c = 0; c < NCATS; c++)
        for (i = 0; CATS[c].pages[i] != PG_COUNT; i++) if (CATS[c].pages[i] == p) return c;
    return -1;
}

/* ---- ms-settings: ----------------------------------------------------------------------- */
static const struct { const WCHAR *uri; enum page_id page; } URIS[] = {
    { L"", PG_S_HOME }, { L"home", PG_S_HOME },
    { L"system", PG_S_DISPLAY }, { L"display", PG_S_DISPLAY }, { L"display-advanced", PG_S_DISPLAY },
    { L"nightlight", PG_S_DISPLAY }, { L"screenrotation", PG_S_DISPLAY }, { L"easeofaccess-display", PG_S_EOA_DISPLAY },
    { L"sound", PG_S_SOUND }, { L"sound-devices", PG_S_SOUND }, { L"apps-volume", PG_S_SOUND },
    { L"notifications", PG_S_NOTIFY }, { L"quiethours", PG_S_NOTIFY }, { L"quietmomentshome", PG_S_NOTIFY },
    { L"powersleep", PG_S_POWER }, { L"batterysaver", PG_S_POWER }, { L"batterysaver-settings", PG_S_POWER },
    { L"storagesense", PG_S_STORAGE }, { L"storagepolicies", PG_S_STORAGE }, { L"savelocations", PG_S_STORAGE },
    { L"multitasking", PG_S_MULTITASK }, { L"clipboard", PG_S_MULTITASK },
    { L"about", PG_S_ABOUT }, { L"deviceencryption", PG_S_ABOUT },
    { L"devices", PG_S_BLUETOOTH }, { L"bluetooth", PG_S_BLUETOOTH }, { L"connecteddevices", PG_S_BLUETOOTH },
    { L"printers", PG_S_BLUETOOTH }, { L"usb", PG_S_BLUETOOTH }, { L"autoplay", PG_S_BLUETOOTH },
    { L"mousetouchpad", PG_S_MOUSE }, { L"devices-touchpad", PG_S_MOUSE },
    { L"typing", PG_S_TYPING }, { L"devicestyping-hwkbtextsuggestions", PG_S_TYPING },
    { L"network", PG_S_NETSTATUS }, { L"network-status", PG_S_NETSTATUS }, { L"network-wifi", PG_S_WIFI },
    { L"network-wifisettings", PG_S_WIFI }, { L"network-ethernet", PG_S_ETHERNET }, { L"network-proxy", PG_S_PROXY },
    { L"network-vpn", PG_S_NETSTATUS }, { L"network-dialup", PG_S_NETSTATUS }, { L"network-airplanemode", PG_S_WIFI },
    { L"datausage", PG_S_NETSTATUS },
    { L"personalization", PG_S_BACKGROUND }, { L"personalization-background", PG_S_BACKGROUND },
    { L"personalization-colors", PG_S_COLORS }, { L"colors", PG_S_COLORS }, { L"lockscreen", PG_S_LOCKSCREEN },
    { L"themes", PG_S_THEMES }, { L"fonts", PG_S_THEMES }, { L"personalization-start", PG_S_START },
    { L"personalization-start-places", PG_S_START }, { L"taskbar", PG_S_TASKBAR },
    { L"appsfeatures", PG_S_APPS }, { L"appsfeatures-app", PG_S_APPS }, { L"optionalfeatures", PG_S_APPS },
    { L"appsforwebsites", PG_S_DEFAULTAPPS }, { L"defaultapps", PG_S_DEFAULTAPPS }, { L"startupapps", PG_S_STARTUP },
    { L"yourinfo", PG_S_YOURINFO }, { L"accounts", PG_S_YOURINFO }, { L"emailandaccounts", PG_S_YOURINFO },
    { L"signinoptions", PG_S_SIGNIN }, { L"signinoptions-launchfaceenrollment", PG_S_SIGNIN },
    { L"otherusers", PG_S_OTHERUSERS }, { L"family-group", PG_S_OTHERUSERS }, { L"workplace", PG_S_YOURINFO },
    { L"dateandtime", PG_S_DATETIME }, { L"regionformatting", PG_S_REGION }, { L"region", PG_S_REGION },
    { L"regionlanguage", PG_S_REGION }, { L"keyboard", PG_S_REGION }, { L"speech", PG_SPEECH },
    { L"easeofaccess", PG_S_EOA_DISPLAY }, { L"easeofaccess-keyboard", PG_S_EOA_KEYBOARD },
    { L"easeofaccess-mouse", PG_S_EOA_MOUSE }, { L"easeofaccess-magnifier", PG_S_EOA_MAGNIFIER }, { L"easeofaccess-cursorandpointersize", PG_S_EOA_MOUSE },
    { L"easeofaccess-mousepointer", PG_S_EOA_MOUSE }, { L"easeofaccess-highcontrast", PG_S_EOA_DISPLAY },
    { L"easeofaccess-colorfilter", PG_S_EOA_DISPLAY }, { L"easeofaccess-visualeffects", PG_S_EOA_DISPLAY },
    { L"privacy", PG_S_PRIV_GENERAL }, { L"privacy-general", PG_S_PRIV_GENERAL },
    { L"privacy-microphone", PG_S_PRIV_MIC }, { L"privacy-webcam", PG_S_PRIV_CAMERA },
    { L"privacy-location", PG_S_PRIV_LOCATION }, { L"privacy-speech", PG_S_PRIV_GENERAL },
    { L"windowsupdate", PG_S_UPDATE }, { L"windowsupdate-action", PG_S_UPDATE },
    { L"windowsupdate-history", PG_S_UPDATE }, { L"windowsupdate-options", PG_S_UPDATE },
    { L"windowsupdate-restartoptions", PG_S_UPDATE }, { L"recovery", PG_S_RECOVERY },
    { L"troubleshoot", PG_S_RECOVERY }, { L"activation", PG_S_ABOUT }, { L"windowsdefender", PG_S_UPDATE },
};

/* the page a command line names: ms-settings:NAME, --page NAME or a bare NAME */
static enum page_id page_for(const WCHAR *arg, BOOL *known)
{
    WCHAR name[128], *q;
    size_t i;
    *known = TRUE;
    if (!_wcsnicmp(arg, L"ms-settings:", 12)) arg += 12;
    lstrcpynW(name, arg, ARRAYSIZE(name));
    if ((q = wcspbrk(name, L"?#"))) *q = 0;
    while ((q = wcsrchr(name, L'/')) && !q[1]) *q = 0;
#ifdef SG_MUTANT_URI
    *known = FALSE;
    return PG_S_HOME;
#endif
    for (i = 0; i < ARRAYSIZE(URIS); i++) if (!_wcsicmp(name, URIS[i].uri)) return URIS[i].page;
    *known = FALSE;
    return PG_S_HOME;       /* Windows opens Home for a name it does not know */
}

/* ---- the controls of a page ---------------------------------------------------------------- */
COLORREF st_accent(void) { return g_accent; }
int st_x(void) { return S(28); }
int st_w(void)
{
    int w = pg_width() - 2 * st_x();
    return w > S(680) ? S(680) : w;
}

int st_title(const WCHAR *title)
{
    int y = S(20);
    pg_text(st_x(), y, pg_width() - st_x() - S(16), S(44), g_font_title, COL_TEXT, title, DT_SINGLELINE | DT_END_ELLIPSIS);
    y += S(56);
    if (g_status[0] && g_status_page == current_page()) {
        y += pg_para(st_x(), y, st_w(), g_font_body, g_accent, g_status) + S(12);
    }
    return y;
}

int st_head(int y, const WCHAR *s)
{
    y += S(8);
    pg_text(st_x(), y, st_w(), S(28), g_font_head, COL_TEXT, s, DT_SINGLELINE | DT_END_ELLIPSIS);
    return y + S(36);
}

int st_para(int y, const WCHAR *s) { return y + pg_para(st_x(), y, st_w(), g_font_body, COL_SUBTLE, s) + S(10); }

int st_text(int y, const WCHAR *s)
{
    pg_text(st_x(), y, st_w(), S(22), g_font_body, COL_TEXT, s, DT_SINGLELINE | DT_END_ELLIPSIS);
    return y + S(26);
}

int st_row(int y, const WCHAR *label, const WCHAR *value)
{
    pg_text(st_x(), y, S(170), S(22), g_font_body, COL_TEXT, label, DT_SINGLELINE | DT_END_ELLIPSIS);
    pg_text(st_x() + S(180), y, st_w() - S(180), S(22), g_font_body, COL_TEXT, value, DT_SINGLELINE | DT_END_ELLIPSIS);
    return y + S(28);
}

HWND st_toggle(int *y, const WCHAR *label, BOOL on, int id)
{
    HWND c;
    if (label) { pg_text(st_x(), *y, st_w(), S(22), g_font_body, COL_TEXT, label, DT_SINGLELINE | DT_END_ELLIPSIS); *y += S(26); }
    c = pg_control(SET_TOGGLE_CLASS, label ? label : L"", WS_TABSTOP, st_x(), *y, S(110), S(26), id);
    SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    *y += S(40);
    return c;
}

HWND st_combo(int *y, const WCHAR *label, const WCHAR *const *items, int n, int sel, int id)
{
    HWND c;
    int i;
    if (label) { pg_text(st_x(), *y, st_w(), S(22), g_font_body, COL_TEXT, label, DT_SINGLELINE | DT_END_ELLIPSIS); *y += S(26); }
    c = pg_control(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, st_x(), *y, S(300), S(300), id);
    for (i = 0; i < n; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)items[i]);
    SendMessageW(c, CB_SETCURSEL, sel, 0);
    *y += S(44);
    return c;
}

HWND st_slider(int *y, const WCHAR *label, int lo, int hi, int pos, int id)
{
    HWND c;
    if (label) { pg_text(st_x(), *y, st_w(), S(22), g_font_body, COL_TEXT, label, DT_SINGLELINE | DT_END_ELLIPSIS); *y += S(26); }
    c = pg_control(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS | TBS_BOTH, st_x(), *y, S(300), S(30), id);
    SendMessageW(c, TBM_SETRANGE, FALSE, MAKELPARAM(lo, hi));
    SendMessageW(c, TBM_SETPAGESIZE, 0, (hi - lo) / 10 ? (hi - lo) / 10 : 1);
    SendMessageW(c, TBM_SETPOS, TRUE, pos);
    *y += S(44);
    return c;
}

HWND st_button(int *y, const WCHAR *text, int id)
{
    HDC dc = GetDC(g_page);
    SIZE sz;
    HWND c;
    SelectObject(dc, g_font_body);
    GetTextExtentPoint32W(dc, text, lstrlenW(text), &sz);
    ReleaseDC(g_page, dc);
    c = pg_control(L"BUTTON", text, WS_TABSTOP | BS_PUSHBUTTON, st_x(), *y, sz.cx + S(40) < S(120) ? S(120) : sz.cx + S(40), S(32), id);
    if (IS_SHIELD(id)) SendMessageW(c, BCM_SETSHIELD, 0, TRUE);
    *y += S(46);
    return c;
}

HWND st_link(int *y, const WCHAR *text, int id)
{
    HWND c = pg_link(st_x(), *y, text, id, 0);
    *y += S(30);
    return c;
}

HWND st_edit(int *y, const WCHAR *label, const WCHAR *text, int id)
{
    HWND c;
    if (label) { pg_text(st_x(), *y, st_w(), S(22), g_font_body, COL_TEXT, label, DT_SINGLELINE | DT_END_ELLIPSIS); *y += S(26); }
    c = pg_control(L"EDIT", text, WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, st_x(), *y, S(300), S(28), id);
    *y += S(42);
    return c;
}

void st_checkbox(int *y, const WCHAR *label, BOOL on, int id)
{
    HWND c = pg_control(L"BUTTON", label, WS_TABSTOP | BS_AUTOCHECKBOX, st_x(), *y, st_w(), S(26), id);
    SendMessageW(c, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    *y += S(32);
}

int st_card(int y, int icon, const WCHAR *title, const WCHAR *sub)
{
    pg_fill(st_x(), y, st_w(), S(64), COL_CARD);
    pg_icon(st_x() + S(14), y + S(14), S(36), icon);
    pg_text(st_x() + S(64), y + S(10), st_w() - S(80), S(22), g_font_head, COL_TEXT, title, DT_SINGLELINE | DT_END_ELLIPSIS);
    if (sub) pg_text(st_x() + S(64), y + S(34), st_w() - S(80), S(20), g_font_small, COL_SUBTLE, sub, DT_SINGLELINE | DT_END_ELLIPSIS);
    return y + S(76);
}

BOOL st_checked(HWND c) { return c && SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED; }

void st_status(const WCHAR *msg)
{
    lstrcpynW(g_status, msg ? msg : L"", ARRAYSIZE(g_status));
    g_status_page = current_page();
    refresh_page();
}

/* ---- the switch (a Windows 10 toggle): a button that says On or Off ---------------------- */
static LRESULT CALLBACK toggle_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LONG_PTR on = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case BM_GETCHECK: return (on & 1) ? BST_CHECKED : BST_UNCHECKED;
    case BM_SETCHECK: SetWindowLongPtrW(hwnd, GWLP_USERDATA, (on & ~1) | (wp == BST_CHECKED)); InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_BUTTON | DLGC_WANTCHARS;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_MOUSEMOVE:
        if (!(on & 2)) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, on | 2); TrackMouseEvent(&t); InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE: SetWindowLongPtrW(hwnd, GWLP_USERDATA, on & ~2); InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: SetFocus(hwnd); return 0;
    case WM_LBUTTONUP:
    case WM_CHAR:
        if (msg == WM_CHAR && wp != ' ') break;
        if (!IsWindowEnabled(hwnd)) return 0;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, on ^ 1);
        InvalidateRect(hwnd, NULL, FALSE);
        SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), (LPARAM)hwnd);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r, pill;
        BOOL checked = on & 1, en = IsWindowEnabled(hwnd), hot = (on & 2) != 0;
        COLORREF fg = !en ? RGB(0xA0, 0xA0, 0xA0) : checked ? g_accent : hot ? RGB(0, 0, 0) : RGB(0x33, 0x33, 0x33);
        HBRUSH b;
        HPEN pen;
        int h, kx;
        GetClientRect(hwnd, &r);
        b = CreateSolidBrush(COL_BG); FillRect(dc, &r, b); DeleteObject(b);
        h = S(20);
        SetRect(&pill, S(1), (r.bottom - h) / 2, S(1) + S(44), (r.bottom - h) / 2 + h);
        pen = CreatePen(PS_SOLID, S(2) > 1 ? S(2) : 1, fg);
        b = CreateSolidBrush(checked ? fg : COL_BG);
        SelectObject(dc, pen); SelectObject(dc, b);
        RoundRect(dc, pill.left, pill.top, pill.right, pill.bottom, h, h);
        SelectObject(dc, GetStockObject(NULL_PEN));
        DeleteObject(b);
        b = CreateSolidBrush(checked ? RGB(0xFF, 0xFF, 0xFF) : fg);
        SelectObject(dc, b);
        kx = checked ? pill.right - S(15) : pill.left + S(5);
        Ellipse(dc, kx, pill.top + S(5), kx + S(10) + 1, pill.top + S(15) + 1);
        SelectObject(dc, GetStockObject(WHITE_BRUSH));
        DeleteObject(b); DeleteObject(pen);
        SelectObject(dc, g_font_body);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, en ? COL_TEXT : RGB(0xA0, 0xA0, 0xA0));
        TextOutW(dc, pill.right + S(12), (r.bottom - S(18)) / 2, checked ? L"On" : L"Off", checked ? 2 : 3);
        if (GetFocus() == hwnd && g_kbd_cues) { RECT f = pill; InflateRect(&f, S(3), S(3)); DrawFocusRect(dc, &f); }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- Home: the categories as tiles ----------------------------------------------------------- */
static LRESULT CALLBACK tile_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int cat = GetDlgCtrlID(hwnd) - CMD_TILE;
    BOOL hot = GetPropW(hwnd, L"hot") != NULL;
    switch (msg) {
    case WM_GETDLGCODE:
        if (lp && ((MSG *)lp)->message == WM_KEYDOWN && ((MSG *)lp)->wParam == VK_RETURN) return DLGC_WANTMESSAGE;
        return DLGC_WANTCHARS;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_MOUSEMOVE:
        if (!hot) { TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 }; SetPropW(hwnd, L"hot", (HANDLE)1); TrackMouseEvent(&t); InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_MOUSELEAVE: RemovePropW(hwnd, L"hot"); InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_NCDESTROY: RemovePropW(hwnd, L"hot"); break;
    case WM_LBUTTONDOWN: SetFocus(hwnd); return 0;
    case WM_KEYDOWN: if (wp != VK_RETURN) break; /* fall through */
    case WM_LBUTTONUP:
        PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), (LPARAM)hwnd);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r, t;
        HBRUSH b;
        GetClientRect(hwnd, &r);
        b = CreateSolidBrush(COL_BG); FillRect(dc, &r, b); DeleteObject(b);
        if (hot || GetFocus() == hwnd) {
            b = CreateSolidBrush(hot ? RGB(0xCC, 0xCC, 0xCC) : g_accent);
            FrameRect(dc, &r, b); DeleteObject(b);
        }
        if (cat >= 0 && cat < NCATS) {
            draw_icon(dc, CATS[cat].icon, S(16), (r.bottom - S(36)) / 2, S(36));
            SetBkMode(dc, TRANSPARENT);
            SetRect(&t, S(68), S(14), r.right - S(8), S(38));
            SelectObject(dc, g_font_cat); SetTextColor(dc, COL_TEXT);
            DrawTextW(dc, CATS[cat].name, -1, &t, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SetRect(&t, S(68), S(38), r.right - S(8), r.bottom - S(4));
            SelectObject(dc, g_font_small); SetTextColor(dc, COL_SUBTLE);
            DrawTextW(dc, CATS[cat].sub, -1, &t, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static WNDPROC g_edit_proc;
static LRESULT CALLBACK search_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r;
    if (msg == WM_KEYDOWN && wp == VK_RETURN) {     /* Enter: the first result */
        WCHAR text[128];
        GetWindowTextW(hwnd, text, ARRAYSIZE(text));
        if (text[0]) PostMessageW(g_page, WM_COMMAND, MAKEWPARAM(CMD_RESULT, 0xFFFF), 0);
        return 0;
    }
    if (msg == WM_GETDLGCODE && lp && ((MSG *)lp)->message == WM_KEYDOWN && ((MSG *)lp)->wParam == VK_RETURN)
        return DLGC_WANTMESSAGE | DLGC_HASSETSEL | DLGC_WANTCHARS;
    if (msg == WM_CHAR && (wp == '\r' || wp == '\n')) return 0;
    r = CallWindowProcW(g_edit_proc, hwnd, msg, wp, lp);
    if (msg == WM_PAINT && !GetWindowTextLengthW(hwnd)) {
        HDC dc = GetDC(hwnd);
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.left += S(6);
        SelectObject(dc, g_font_body);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0x80, 0x80, 0x80));
        DrawTextW(dc, L"Find a setting", -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        draw_icon(dc, IC_G_SEARCH, rc.right - S(24), (rc.bottom - S(16)) / 2, S(16));
        ReleaseDC(hwnd, dc);
    }
    return r;
}

static HWND make_search(HWND parent, int x, int y, int w, int id)
{
    HWND e = CreateWindowExW(0, L"EDIT", g_search_text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                             x, y, w, S(32), parent, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(e, WM_SETFONT, (WPARAM)g_font_body, TRUE);
    SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(S(6), S(28)));
    g_edit_proc = (WNDPROC)SetWindowLongPtrW(e, GWLP_WNDPROC, (LONG_PTR)search_edit_proc);
    return e;
}

void set_build_home(void)
{
    int w = pg_width(), tw = S(290), th = S(84), cols, x0, y = S(28), i;
    HWND e;
    pg_text(0, y, w, S(44), g_font_big, COL_TEXT, L"Windows Settings", DT_SINGLELINE | DT_CENTER);
    y += S(64);
    e = make_search(g_page, (w - S(420)) / 2, y, S(420), CMD_HOMESEARCH);
    (void)e;
    y += S(64);
    cols = (w - S(48)) / (tw + S(8));
    if (cols > 5) cols = 5;
    if (cols < 1) cols = 1;
    x0 = (w - cols * (tw + S(8))) / 2;
    for (i = 0; i < NCATS; i++)
        pg_control(L"SgSetTile", CATS[i].name, WS_TABSTOP, x0 + (i % cols) * (tw + S(8)), y + (i / cols) * (th + S(8)), tw, th, CMD_TILE + i);
}

static BOOL matches(enum page_id p, const WCHAR *q)
{
    WCHAR hay[512], needle[128], *tok, *ctx;
    size_t i;
    int c = category_of(p);
    _snwprintf(hay, ARRAYSIZE(hay), L"%ls %ls", g_pages[p].title, c >= 0 ? CATS[c].name : L"");
    for (i = 0; i < ARRAYSIZE(KEYWORDS); i++)
        if (KEYWORDS[i].page == p) { lstrcatW(hay, L" "); wcsncat(hay, KEYWORDS[i].words, ARRAYSIZE(hay) - lstrlenW(hay) - 1); }
    CharLowerW(hay);
    lstrcpynW(needle, q, ARRAYSIZE(needle));
    CharLowerW(needle);
    /* every word typed must be found */
    for (tok = wcstok_s(needle, L" ", &ctx); tok; tok = wcstok_s(NULL, L" ", &ctx))
        if (!wcsstr(hay, tok)) return FALSE;
    return TRUE;
}

static int search_results(enum page_id *out, int max)
{
    int c, i, n = 0;
    if (!g_search_text[0]) return 0;
    for (c = 0; c < NCATS; c++)
        for (i = 0; CATS[c].pages[i] != PG_COUNT && n < max; i++)
            if (matches(CATS[c].pages[i], g_search_text)) out[n++] = CATS[c].pages[i];
    return n;
}

void set_build_search(void)
{
    enum page_id res[64];
    int n = search_results(res, ARRAYSIZE(res)), i, y;
    WCHAR line[256];
    y = st_title(L"Search results");
    if (!n) {
        _snwprintf(line, ARRAYSIZE(line), L"We couldn't find anything for \"%ls\".", g_search_text);
        st_para(y, line);
        return;
    }
    for (i = 0; i < n; i++) {
        int c = category_of(res[i]);
        _snwprintf(line, ARRAYSIZE(line), L"%ls  \x203A  %ls", c >= 0 ? CATS[c].name : L"", g_pages[res[i]].title);
        st_link(&y, line, CMD_RESULT + res[i]);
    }
}

BOOL set_cmd_home(int id, int code, HWND ctl)
{
    if (id >= CMD_TILE && id < CMD_TILE + NCATS) { navigate(CATS[id - CMD_TILE].pages[0]); return TRUE; }
    if (id == CMD_RESULT && code == 0xFFFF) {       /* Enter in a search box */
        enum page_id res[4];
        if (search_results(res, 4)) { g_search_text[0] = 0; SetWindowTextW(g_nav_search, L""); g_keep_focus = NULL; navigate(res[0]); }
        return TRUE;
    }
    if (id > CMD_RESULT && id < CMD_RESULT + PG_COUNT) {
        g_search_text[0] = 0; g_keep_focus = NULL;
        SetWindowTextW(g_nav_search, L"");
        navigate((enum page_id)(id - CMD_RESULT));
        return TRUE;
    }
    if (id == CMD_HOMESEARCH && code == EN_CHANGE) {
        /* the rebuild destroys this edit: never inside its own notification */
        GetWindowTextW(ctl, g_search_text, ARRAYSIZE(g_search_text));
        if (g_search_text[0]) PostMessageW(g_page, WM_COMMAND, MAKEWPARAM(CMD_HOMESEARCH, 0xFFFE), 0);
        return TRUE;
    }
    if (id == CMD_HOMESEARCH && code == 0xFFFE && g_search_text[0]) {
        SetWindowTextW(g_nav_search, g_search_text);    /* EN_CHANGE there navigates */
        SetFocus(g_nav_search);
        SendMessageW(g_nav_search, EM_SETSEL, lstrlenW(g_search_text), lstrlenW(g_search_text));
        if (current_page() != PG_S_SEARCH) { g_keep_focus = g_nav_search; navigate(PG_S_SEARCH); }
        return TRUE;
    }
    return FALSE;
}

/* ---- the navigation pane ------------------------------------------------------------------ */
#define NAV_BACK_H  48
#define NAV_HOME_Y  52
#define NAV_ITEM_H  40
#define NAV_SEARCH_Y 100
#define NAV_CAT_Y   148
#define NAV_LIST_Y  184

static int g_nav_hot = -2, g_nav_focus;     /* -1 back, 0 Home, 1.. the category's pages */

static int nav_count(void)
{
    int c = category_of(current_page()), n = 0;
    if (c < 0) return 0;
    while (CATS[c].pages[n] != PG_COUNT) n++;
    return n;
}

static int nav_hit(int y)
{
    if (y < S(NAV_BACK_H)) return -1;
    if (y >= S(NAV_HOME_Y) && y < S(NAV_HOME_Y + NAV_ITEM_H)) return 0;
    if (y >= S(NAV_LIST_Y)) {
        int i = (y - S(NAV_LIST_Y)) / S(NAV_ITEM_H);
        if (i < nav_count()) return i + 1;
    }
    return -2;
}

static void nav_go(int item)
{
    int c = category_of(current_page());
    if (item == -1) { if (!nav_back()) navigate(PG_S_HOME); }
    else if (item == 0) navigate(PG_S_HOME);
    else if (c >= 0 && item - 1 < nav_count()) navigate(CATS[c].pages[item - 1]);
}

static LRESULT CALLBACK nav_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_GETDLGCODE: return DLGC_WANTARROWS | (lp && ((MSG *)lp)->message == WM_KEYDOWN && ((MSG *)lp)->wParam == VK_RETURN ? DLGC_WANTMESSAGE : 0);
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_KEYDOWN: {
        int n = nav_count();
        if (wp == VK_UP && g_nav_focus > -1) { g_nav_focus--; InvalidateRect(hwnd, NULL, FALSE); }
        else if (wp == VK_DOWN && g_nav_focus < n) { g_nav_focus++; InvalidateRect(hwnd, NULL, FALSE); }
        else if (wp == VK_RETURN || wp == VK_SPACE) nav_go(g_nav_focus);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int h = nav_hit(GET_Y_LPARAM(lp));
        if (h != g_nav_hot) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
            g_nav_hot = h; TrackMouseEvent(&t); InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE: g_nav_hot = -2; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONUP: {
        int h = nav_hit(GET_Y_LPARAM(lp));
        if (h == -1 && GET_X_LPARAM(lp) > S(56)) return 0;   /* the caption, not the button */
        if (h >= -1) { g_nav_focus = h; nav_go(h); }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_NAVSEARCH && HIWORD(wp) == EN_CHANGE) {
            GetWindowTextW(g_nav_search, g_search_text, ARRAYSIZE(g_search_text));
            g_keep_focus = g_nav_search;
            if (g_search_text[0] || current_page() == PG_S_SEARCH) {
                if (current_page() != PG_S_SEARCH) navigate(PG_S_SEARCH); else refresh_page();
            }
            return 0;
        }
        break;
    case WM_CTLCOLOREDIT: return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r, it;
        HBRUSH b;
        int c = category_of(current_page()), n = nav_count(), i;
        BOOL focus = GetFocus() == hwnd;
        GetClientRect(hwnd, &r);
        b = CreateSolidBrush(COL_NAV); FillRect(dc, &r, b); DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        /* back, and the window's name */
        SetRect(&it, 0, 0, S(48), S(NAV_BACK_H));
        if (g_nav_hot == -1) { b = CreateSolidBrush(COL_NAV_HOT); FillRect(dc, &it, b); DeleteObject(b); }
        {
            COLORREF old = g_glyph_color;
            g_glyph_color = COL_TEXT;
            draw_icon(dc, IC_G_BACK, S(16), (S(NAV_BACK_H) - S(16)) / 2, S(16));
            g_glyph_color = old;
        }
        if (focus && g_nav_focus == -1 && g_kbd_cues) DrawFocusRect(dc, &it);
        SelectObject(dc, g_font_small); SetTextColor(dc, COL_TEXT);
        SetRect(&it, S(56), 0, r.right, S(NAV_BACK_H));
        DrawTextW(dc, L"Settings", -1, &it, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        /* Home */
        SetRect(&it, 0, S(NAV_HOME_Y), r.right, S(NAV_HOME_Y + NAV_ITEM_H));
        if (g_nav_hot == 0) { b = CreateSolidBrush(COL_NAV_HOT); FillRect(dc, &it, b); DeleteObject(b); }
        {
            COLORREF old = g_glyph_color;
            g_glyph_color = COL_TEXT;
            draw_icon(dc, IC_G_HOME, S(16), it.top + (S(NAV_ITEM_H) - S(16)) / 2, S(16));
            g_glyph_color = old;
        }
        SelectObject(dc, g_font_body); SetTextColor(dc, COL_TEXT);
        it.left = S(48);
        DrawTextW(dc, L"Home", -1, &it, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        if (focus && g_nav_focus == 0 && g_kbd_cues) { it.left = 0; DrawFocusRect(dc, &it); }
        /* the category and its pages */
        if (c >= 0) {
            SetRect(&it, S(16), S(NAV_CAT_Y), r.right - S(8), S(NAV_LIST_Y));
            SelectObject(dc, g_font_head);
            DrawTextW(dc, CATS[c].name, -1, &it, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            for (i = 0; i < n; i++) {
                enum page_id p = CATS[c].pages[i];
                BOOL sel = p == current_page();
                SetRect(&it, 0, S(NAV_LIST_Y) + i * S(NAV_ITEM_H), r.right, S(NAV_LIST_Y) + (i + 1) * S(NAV_ITEM_H));
                if (sel || g_nav_hot == i + 1) {
                    b = CreateSolidBrush(sel ? COL_NAV_SEL : COL_NAV_HOT); FillRect(dc, &it, b); DeleteObject(b);
                }
                if (sel) {
                    RECT bar = { 0, it.top + S(8), S(4), it.bottom - S(8) };
                    b = CreateSolidBrush(g_accent); FillRect(dc, &bar, b); DeleteObject(b);
                }
                SelectObject(dc, g_font_body); SetTextColor(dc, COL_TEXT);
                {
                    RECT t = it;
                    t.left = S(48);
                    DrawTextW(dc, g_pages[p].title, -1, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                }
                if (focus && g_nav_focus == i + 1 && g_kbd_cues) DrawFocusRect(dc, &it);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the window ------------------------------------------------------------------------------ */
static void layout(void)
{
    RECT r;
    BOOL nav = current_page() != PG_S_HOME;
    GetClientRect(g_main, &r);
    ShowWindow(g_nav, nav ? SW_SHOWNA : SW_HIDE);
    if (nav) {
        MoveWindow(g_nav, 0, 0, S(NAV_W), r.bottom, TRUE);
        MoveWindow(g_nav_search, S(12), S(NAV_SEARCH_Y), S(NAV_W) - S(24), S(32), TRUE);
        MoveWindow(g_page, S(NAV_W), 0, r.right - S(NAV_W) > 0 ? r.right - S(NAV_W) : 0, r.bottom, TRUE);
    } else MoveWindow(g_page, 0, 0, r.right, r.bottom, TRUE);
}

static WCHAR g_dump_tmp[MAX_PATH + 8];
static void write_dump(void)
{
    WCHAR path[MAX_PATH];
    FILE *f;
    if (!GetEnvironmentVariableW(L"SG_SETTINGS_DUMP", path, MAX_PATH)) return;
    /* written whole, then renamed: a reader never sees half a dump */
    {
        WCHAR tmp[MAX_PATH + 8];
        _snwprintf(tmp, ARRAYSIZE(tmp), L"%ls.tmp", path);
        tmp[ARRAYSIZE(tmp) - 1] = 0;
        if (!(f = _wfopen(tmp, L"w, ccs=UTF-8"))) return;
        lstrcpyW(g_dump_tmp, tmp);
    }
    {
        int c = category_of(current_page());
        WCHAR title[128];
        GetWindowTextW(g_main, title, ARRAYSIZE(title));
        RECT wr;
        GetWindowRect(g_main, &wr);
        fwprintf(f, L"window %ls\nrect %ld %ld %ld %ld\npage %ls\ncategory %ls\nnav %ls\n", title, wr.left, wr.top, wr.right, wr.bottom,
                 g_pages[current_page()].title, c >= 0 ? CATS[c].name : L"", IsWindowVisible(g_nav) ? L"shown" : L"hidden");
        fwprintf(f, L"accent %02X%02X%02X\n", GetRValue(g_accent), GetGValue(g_accent), GetBValue(g_accent));
        if (IsWindowVisible(g_nav) && g_nav_focus > 0) {
            /* the selected page's accent bar, on screen */
            POINT pt = { S(2), S(NAV_LIST_Y) + (g_nav_focus - 1) * S(NAV_ITEM_H) + S(NAV_ITEM_H) / 2 };
            ClientToScreen(g_nav, &pt);
            fwprintf(f, L"accentbar %ld %ld\n", pt.x, pt.y);
        }
        {
            POINT pt = { S(NAV_W) / 2, S(NAV_SEARCH_Y) + S(16) };
            ClientToScreen(g_nav, &pt);
            fwprintf(f, L"navsearch %ld %ld\n", pt.x, pt.y);
        }
    }
    page_dump(f);
    fclose(f);
    MoveFileExW(g_dump_tmp, path, MOVEFILE_REPLACE_EXISTING);
}

void settings_dump(void) { write_dump(); }

void settings_page_shown(void)
{
    static enum page_id last = PG_COUNT;
    enum page_id p = current_page();
    if (p != g_status_page) g_status[0] = 0;
    if (p != last) {
        int c = category_of(p), i;
        g_nav_focus = 0;
        if (c >= 0) for (i = 0; CATS[c].pages[i] != PG_COUNT; i++) if (CATS[c].pages[i] == p) g_nav_focus = i + 1;
        if ((last == PG_S_HOME) != (p == PG_S_HOME) || last == PG_COUNT) { last = p; layout(); refresh_page(); return; }
        last = p;
    }
    InvalidateRect(g_nav, NULL, FALSE);
    write_dump();
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (g_page && wp != SIZE_MINIMIZED) { layout(); refresh_page(); }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(700); mm->ptMinTrackSize.y = S(460);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && refresh_pending()) refresh_page();
        break;
    case WM_SETTINGCHANGE:
        /* the accent changed (here or elsewhere): the switches and links follow */
        if (lp && !lstrcmpW((const WCHAR *)lp, L"ImmersiveColorSet")) {
            DWORD a = reg_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0xFFC03070);
            g_accent = g_glyph_color = g_col_link = a & 0xFFFFFF;
            InvalidateRect(hwnd, NULL, TRUE);
            RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN);
        }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HFONT font(int pt10, int weight)
{
    return CreateFontW(-MulDiv(pt10, g_dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

/* Another Settings window is already open: hand it the page and bring it forward. */
#define SET_COPYDATA_PAGE 0x53475350
static BOOL hand_off(enum page_id p)
{
    HWND other = FindWindowW(L"SgSettingsWindow", NULL);
    COPYDATASTRUCT cd = { SET_COPYDATA_PAGE, sizeof(p), &p };
    DWORD_PTR r;
    if (!other || GetEnvironmentVariableW(L"SG_SETTINGS_NEW_WINDOW", NULL, 0)) return FALSE;
    if (!SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &r)) return FALSE;
    if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
    SetForegroundWindow(other);
    return TRUE;
}

static LRESULT CALLBACK main_proc_outer(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COPYDATA) {
        COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp;
        if (cd->dwData == SET_COPYDATA_PAGE && cd->cbData == sizeof(enum page_id)) {
            enum page_id p = *(enum page_id *)cd->lpData;
            if (p < PG_COUNT) navigate(p);
            return TRUE;
        }
        return FALSE;
    }
    return main_proc(hwnd, msg, wp, lp);
}

int settings_main(int argc, WCHAR **argv, int show)
{
    WNDCLASSW wc = { 0 };
    HDC dc;
    DWORD a;
    enum page_id start = PG_S_HOME;
    BOOL known = TRUE;
    MSG msg;
    int i;

    g_settings = TRUE;
    for (i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--settings")) continue;
        if ((!wcscmp(argv[i], L"--page") || !_wcsicmp(argv[i], L"/page")) && i + 1 < argc) i++;
        if (!wcscmp(argv[i], L"--resolve") && i + 1 < argc) {
            /* what the URI would open, without opening it */
            enum page_id p = page_for(argv[i + 1], &known);
            int c = category_of(p);
            wprintf(L"page=%ls\ncategory=%ls\nknown=%ls\n", g_pages[p].title, c >= 0 ? CATS[c].name : L"", known ? L"yes" : L"no");
            fflush(stdout);
            return 0;
        }
        start = page_for(argv[i], &known);
        break;
    }
    if (hand_off(start)) return 0;

    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    if (g_dpi < 96) g_dpi = 96;
    g_font_title = font(200, FW_LIGHT);
    g_font_big   = font(220, FW_LIGHT);
    g_font_head  = font(115, FW_SEMIBOLD);
    g_font_cat   = font(105, FW_NORMAL);
    g_font_body  = font(100, FW_NORMAL);
    g_font_small = font(85, FW_NORMAL);
    a = reg_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0xFFC03070);
    g_accent = g_glyph_color = a & 0xFFFFFF;
    g_col_link = g_accent;
    g_col_link_hot = RGB(0x33, 0x33, 0x33);

    register_page_classes();
    pers_register_classes();
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpfnWndProc = toggle_proc; wc.lpszClassName = SET_TOGGLE_CLASS; wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
    RegisterClassW(&wc);
    wc.lpfnWndProc = tile_proc; wc.lpszClassName = L"SgSetTile";
    RegisterClassW(&wc);
    wc.lpfnWndProc = nav_proc; wc.lpszClassName = L"SgSetNav"; wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    wc.lpfnWndProc = main_proc_outer; wc.lpszClassName = L"SgSettingsWindow";
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    RegisterClassW(&wc);

    {
        RECT work;
        int w = S(1000), h = S(700);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        if (w > (work.right - work.left) * 85 / 100) w = (work.right - work.left) * 85 / 100;
        if (h > (work.bottom - work.top) * 92 / 100) h = (work.bottom - work.top) * 92 / 100;
        g_main = CreateWindowExW(WS_EX_CONTROLPARENT, L"SgSettingsWindow", L"Settings", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                 CW_USEDEFAULT, CW_USEDEFAULT, w, h, NULL, NULL, g_inst, NULL);
    }
    if (!g_main) return 1;
    g_nav = CreateWindowExW(WS_EX_CONTROLPARENT, L"SgSetNav", L"Navigation", WS_CHILD | WS_TABSTOP | WS_CLIPCHILDREN,
                            0, 0, 0, 0, g_main, (HMENU)ID_NAV, g_inst, NULL);
    g_nav_search = make_search(g_nav, 0, 0, 0, ID_NAVSEARCH);
    g_page = CreateWindowExW(WS_EX_CONTROLPARENT, L"SgCplPage", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                             0, 0, 0, 0, g_main, NULL, g_inst, NULL);
    navigate(start);
    layout();
    ShowWindow(g_main, show ? show : SW_SHOW);
    UpdateWindow(g_main);
    refresh_page();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && !g_kbd_cues && (msg.wParam == VK_TAB || (msg.wParam >= VK_LEFT && msg.wParam <= VK_DOWN))) {
            g_kbd_cues = TRUE;
            if (GetFocus()) InvalidateRect(GetFocus(), NULL, FALSE);
        }
        if (msg.message == WM_SYSKEYDOWN && msg.wParam == VK_LEFT) { if (!nav_back()) navigate(PG_S_HOME); continue; }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_BROWSER_BACK) { nav_back(); continue; }
        if (IsDialogMessageW(g_main, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

/* ---- sg-settingsctl ----------------------------------------------------------------------------- */
char *ctl_run(const WCHAR *args, BOOL *ok, WCHAR *err, int cch, DWORD timeout_ms)
{
    static LONG seq;
    static char *(CDECL *to_unix)(const WCHAR *);
    WCHAR tool[MAX_PATH] = L"/usr/bin/sg-settingsctl", dir[MAX_PATH], dos[MAX_PATH], cmd[2048], *p;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    char *unix_out, *text = NULL, *last, *end;
    DWORD waited = 0;
    HANDLE h;
    *ok = FALSE;
    if (err) err[0] = 0;
    if (!to_unix) to_unix = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    GetEnvironmentVariableW(L"SG_SETTINGSCTL", tool, MAX_PATH);
    if (!to_unix || !GetTempPathW(MAX_PATH, dir)) { if (err) lstrcpynW(err, L"No temporary folder.", cch); return NULL; }
    _snwprintf(dos, MAX_PATH, L"%lssg-settings-%lu-%ld.txt", dir, GetCurrentProcessId(), InterlockedIncrement(&seq));
    dos[MAX_PATH - 1] = 0;
    DeleteFileW(dos);
    CloseHandle(CreateFileW(dos, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));     /* so it has a Unix name */
    if (!(unix_out = to_unix(dos))) return NULL;
    DeleteFileW(dos);
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\\\\?\\unix%ls %ls --out %S", tool, args, unix_out);
    cmd[ARRAYSIZE(cmd) - 1] = 0;
    HeapFree(GetProcessHeap(), 0, unix_out);
    for (p = cmd + 8; *p && *p != L' '; p++) if (*p == L'/') *p = L'\\';
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        if (err) lstrcpynW(err, L"This setting needs sg-settingsctl, which is not installed.", cch);
        return NULL;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    /* the answer appears whole (it is renamed into place) */
    while (GetFileAttributesW(dos) == INVALID_FILE_ATTRIBUTES && waited < timeout_ms) { Sleep(40); waited += 40; }
    h = CreateFileW(dos, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD size = GetFileSize(h, NULL), got = 0;
        if (size < (1u << 22) && (text = malloc(size + 1))) { ReadFile(h, text, size, &got, NULL); text[got] = 0; }
        CloseHandle(h);
        DeleteFileW(dos);
    }
    if (!text) { if (err) lstrcpynW(err, L"The setting did not answer in time.", cch); return NULL; }
    /* the last line: OK or ERROR kind message */
    end = text + strlen(text);
    while (end > text && (end[-1] == '\n' || end[-1] == '\r')) end--;
    for (last = end; last > text && last[-1] != '\n'; last--) ;
    if (end - last == 2 && !strncmp(last, "OK", 2)) *ok = TRUE;
    else if (err && !strncmp(last, "ERROR ", 6)) {
        char *msg = strchr(last + 6, ' '), save = *end;
        *end = 0;
        MultiByteToWideChar(CP_UTF8, 0, msg ? msg + 1 : last + 6, -1, err, cch);
        *end = save;
    }
    return text;
}

const char *ctl_line(const char *text, const char *key, const char **pos, char *buf, int cch)
{
    const char *p = pos && *pos ? *pos : text;
    size_t kl = strlen(key);
    if (!text) return NULL;
    while (p && *p) {
        const char *nl = strchr(p, '\n'), *next = nl ? nl + 1 : p + strlen(p);
        size_t len = (nl ? (size_t)(nl - p) : strlen(p));
        if (len > kl && !strncmp(p, key, kl) && p[kl] == ' ') {
            size_t n = len - kl - 1;
            if (n >= (size_t)cch) n = cch - 1;
            memcpy(buf, p + kl + 1, n);
            buf[n] = 0;
            if (n && buf[n - 1] == '\r') buf[n - 1] = 0;
            if (pos) *pos = next;
            return buf;
        }
        p = next;
    }
    if (pos) *pos = p;
    return NULL;
}

BOOL ctl_field(const char *s, int i, WCHAR *out, int cch)
{
    const char *p = s, *t;
    char tmp[1024];
    size_t n;
    out[0] = 0;
    while (i-- > 0) { if (!(p = strchr(p, '\t'))) return FALSE; p++; }
    t = strchr(p, '\t');
    n = t ? (size_t)(t - p) : strlen(p);
    if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
    memcpy(tmp, p, n);
    tmp[n] = 0;
    MultiByteToWideChar(CP_UTF8, 0, tmp, -1, out, cch);
    return TRUE;
}
