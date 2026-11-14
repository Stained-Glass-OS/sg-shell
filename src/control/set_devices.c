/* sg-control -- Settings > Devices: Bluetooth & other devices, Mouse, Typing
 * (spelling, the touch keyboard showing itself, the hardware keyboard).
 *
 * Bluetooth is bluez's, through sg-settingsctl (bluetoothctl as the user);
 * the mouse and keyboard settings are Windows' own (SystemParametersInfo,
 * kept in HKCU\Control Panel), which every Windows program reads.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"

/* ---- Bluetooth & other devices --------------------------------------------------------------- */
enum { CMD_BT_POWER = CMD_PAGE_FIRST + 1, CMD_BT_REFRESH, CMD_BT_ADD, CMD_BT_DEV = CMD_PAGE_FIRST + 100 };
#define MAX_BT 32
static WCHAR g_bt_mac[MAX_BT][24], g_bt_name[MAX_BT][128];
static BOOL g_bt_conn[MAX_BT], g_bt_paired[MAX_BT];
static int g_nbt, g_bt_sel = -1;
static BOOL g_bt_scan;          /* "Add a device": search, then list what was found */
/* A device being paired, in the background as on Windows: the page shows what
 * the device asks for -- a PIN to type on a keyboard -- while it waits
 * (David 2026-11-13: a Bluetooth keyboard and mouse would not pair; the PIN
 * a keyboard wanted typed was never shown) */
static int g_pair_i = -1;
static HANDLE g_pair_thread;
static volatile LONG g_pair_done;
static BOOL g_pair_ok;
static WCHAR g_pair_err[256], g_pair_args[80], g_pair_pin[16], g_pair_name[128];
static BOOL g_pair_confirm;

static DWORD WINAPI pair_thread(void *arg)
{
    (void)arg;
    g_pair_err[0] = 0;
    free(ctl_run(g_pair_args, &g_pair_ok, g_pair_err, ARRAYSIZE(g_pair_err), 120000));
    InterlockedExchange(&g_pair_done, 1);
    return 0;
}

void set_build_bluetooth(void)
{
    WCHAR err[256] = L"", line[300];
    BOOL ok, present = FALSE, powered = FALSE;
    char *ans = ctl_run(g_bt_scan ? L"bluetooth scan 8" : L"bluetooth", &ok, err, ARRAYSIZE(err), g_bt_scan ? 30000 : 15000), buf[512];
    const char *pos = NULL;
    int y = st_title(L"Bluetooth & other devices"), i, shown = 0;
    if (g_pair_i >= 0) {
        /* pairing: what the device asks for, until it is done */
        _snwprintf(line, ARRAYSIZE(line), L"Pairing with %ls", g_pair_name);
        y = st_head(y, line);
        if (g_pair_pin[0] && !g_pair_confirm) {
            y = st_para(y, L"Type this PIN on the keyboard, then press Enter:");
            y = st_head(y, g_pair_pin);
        } else if (g_pair_pin[0]) {
            y = st_para(y, L"Check that this PIN matches the one shown on the device:");
            y = st_head(y, g_pair_pin);
        } else
            y = st_para(y, L"Connecting... Follow any instructions on your device.");
        pg_timer(700);
        return;
    }
    g_nbt = 0;
    if (ans && ctl_line(ans, "BLUETOOTH", NULL, buf, sizeof(buf))) present = !strcmp(buf, "yes");
    if (ans && ctl_line(ans, "POWERED", NULL, buf, sizeof(buf))) powered = !strcmp(buf, "yes");
    while (ans && g_nbt < MAX_BT && ctl_line(ans, "DEVICE", &pos, buf, sizeof(buf))) {
        WCHAR con[8], paired[8];
        ctl_field(buf, 0, g_bt_mac[g_nbt], ARRAYSIZE(g_bt_mac[0]));
        ctl_field(buf, 1, con, ARRAYSIZE(con));
        ctl_field(buf, 2, paired, ARRAYSIZE(paired));
        ctl_field(buf, 3, g_bt_name[g_nbt], ARRAYSIZE(g_bt_name[0]));
        g_bt_conn[g_nbt] = !lstrcmpW(con, L"yes");
        g_bt_paired[g_nbt] = !lstrcmpW(paired, L"yes");
        g_nbt++;
    }
    free(ans);
    if (!present) {
        y = st_head(y, L"Bluetooth");
        y = st_para(y, ok ? L"This PC has no Bluetooth adapter, or Bluetooth is not installed."
                          : L"Bluetooth settings are not available right now.");
        g_bt_scan = FALSE;
    } else {
        st_toggle(&y, L"Bluetooth", powered, CMD_BT_POWER);
        if (powered) {
            WCHAR name[64] = L"";
            DWORD n = ARRAYSIZE(name);
            GetComputerNameW(name, &n);
            _snwprintf(line, ARRAYSIZE(line), L"Now discoverable as \"%ls\"", name);
            y = st_para(y - S(8), line);
            st_button(&y, L"Add Bluetooth or other device", CMD_BT_ADD);
        }
        if (g_bt_scan) {
            y = st_head(y, L"Add a device");
            for (i = 0; i < g_nbt; i++) {
                if (g_bt_paired[i]) continue;
                y = st_card(y, IC_G_DEVICES, g_bt_name[i], g_bt_mac[i]);
                pg_link(st_x() + S(64), y - S(12), L"Pair", CMD_BT_DEV + i * 4 + 3, 0);
                y += S(16);
                shown++;
            }
            if (!shown) y = st_para(y, L"No devices were found. Make sure your device is turned on and discoverable, then try again.");
            g_bt_scan = FALSE;
            shown = 0;
        }
        y = st_head(y, L"Audio and other devices");
        for (i = 0; i < g_nbt; i++) {
            if (!g_bt_paired[i]) continue;
            y = st_card(y, IC_G_DEVICES, g_bt_name[i], g_bt_conn[i] ? L"Connected" : L"Paired");
            if (g_bt_sel == i) {
                pg_control(L"BUTTON", g_bt_conn[i] ? L"Disconnect" : L"Connect", WS_TABSTOP | BS_PUSHBUTTON,
                           st_x() + S(64), y - S(8), S(120), S(30), CMD_BT_DEV + i * 4);
                pg_control(L"BUTTON", L"Remove device", WS_TABSTOP | BS_PUSHBUTTON, st_x() + S(194), y - S(8), S(140), S(30),
                           CMD_BT_DEV + i * 4 + 1);
                y += S(34);
            } else {
                pg_link(st_x() + S(64), y - S(12), L"Select", CMD_BT_DEV + i * 4 + 2, 0);
                y += S(16);
            }
            shown++;
        }
        if (!shown) y = st_para(y, L"No devices are paired with this PC.");
    }
    y += S(8);
    st_button(&y, L"Refresh", CMD_BT_REFRESH);
}

BOOL set_cmd_bluetooth(int id, int code, HWND ctl)
{
    WCHAR args[80], err[256];
    BOOL ok;
    (void)code;
    if (id == CMD_BT_POWER) {
        _snwprintf(args, ARRAYSIZE(args), L"bluetooth power %ls", st_checked(ctl) ? L"on" : L"off");
        free(ctl_run(args, &ok, err, ARRAYSIZE(err), 15000));
        if (!ok) st_status(err[0] ? err : L"Bluetooth could not be switched.");
        else refresh_page();
        return TRUE;
    }
    if (id == CMD_BT_REFRESH) { refresh_page(); return TRUE; }
    if (id == CMD_BT_ADD) {
        HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
        g_bt_scan = TRUE;
        refresh_page();
        SetCursor(old);
        return TRUE;
    }
    if (id >= CMD_BT_DEV && id < CMD_BT_DEV + MAX_BT * 4) {
        int i = (id - CMD_BT_DEV) / 4, what = (id - CMD_BT_DEV) % 4;
        const WCHAR *verb;
        if (i >= g_nbt) return TRUE;
        if (what == 2) { g_bt_sel = i; refresh_page(); return TRUE; }
        if (what == 1 && MessageBoxW(g_main, L"Are you sure you want to remove this device?", L"Remove device",
                                     MB_YESNO | MB_ICONQUESTION) != IDYES) return TRUE;
        if (what == 3) {
            if (g_pair_i >= 0) return TRUE;
            _snwprintf(g_pair_args, ARRAYSIZE(g_pair_args), L"bluetooth pair %ls", g_bt_mac[i]);
            lstrcpynW(g_pair_name, g_bt_name[i], ARRAYSIZE(g_pair_name));
            g_pair_pin[0] = 0;
            g_pair_confirm = FALSE;
            g_pair_done = 0;
#ifndef SG_MUTANT_PAIR_BLOCKS
            if ((g_pair_thread = CreateThread(NULL, 0, pair_thread, NULL, 0, NULL))) {
                g_pair_i = i;
                refresh_page();
                return TRUE;
            }
#endif
        }
        verb = what == 3 ? L"pair" : what == 1 ? L"remove" : g_bt_conn[i] ? L"disconnect" : L"connect";
        _snwprintf(args, ARRAYSIZE(args), L"bluetooth %ls %ls", verb, g_bt_mac[i]);
        free(ctl_run(args, &ok, err, ARRAYSIZE(err), 90000));
        g_bt_sel = -1;
        if (!ok) st_status(err[0] ? err : L"That did not work.");
        else refresh_page();
        return TRUE;
    }
    return FALSE;
}

void set_timer_bluetooth(void)
{
    WCHAR err[64];
    BOOL ok;
    char *ans, buf[64];
    WCHAR pin[16] = L"";
    BOOL confirm = FALSE;

    if (g_pair_i < 0) return;
    if (g_pair_done) {
        WaitForSingleObject(g_pair_thread, 5000);
        CloseHandle(g_pair_thread);
        g_pair_thread = NULL;
        g_pair_i = -1;
        g_bt_sel = -1;
        refresh_page();
        if (!g_pair_ok) st_status(g_pair_err[0] ? g_pair_err : L"The device could not be paired. Try again.");
        return;
    }
    ans = ctl_run(L"bluetooth pairing", &ok, err, ARRAYSIZE(err), 5000);
    if (ans && ctl_line(ans, "PIN", NULL, buf, sizeof(buf))) MultiByteToWideChar(CP_UTF8, 0, buf, -1, pin, ARRAYSIZE(pin));
    else if (ans && ctl_line(ans, "CONFIRM", NULL, buf, sizeof(buf))) {
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, pin, ARRAYSIZE(pin));
        confirm = TRUE;
    }
    free(ans);
    if (lstrcmpW(pin, g_pair_pin) || confirm != g_pair_confirm) {
        lstrcpynW(g_pair_pin, pin, ARRAYSIZE(g_pair_pin));
        g_pair_confirm = confirm;
        refresh_page();
    }
}

/* ---- Mouse ---------------------------------------------------------------------------------------- */
enum { CMD_PRIMARY = CMD_PAGE_FIRST + 1, CMD_WHEEL, CMD_LINES, CMD_SPEED, CMD_DBLCLICK, CMD_HOVERSCROLL,
       CMD_SCROLL_MOUSE, CMD_SCROLL_PAD };

void set_build_mouse(void)
{
    static const WCHAR *const buttons[] = { L"Left", L"Right" };
    static const WCHAR *const wheel[] = { L"Multiple lines at a time", L"One screen at a time" };
    UINT lines = 3, speed = 10;
    int y = st_title(L"Mouse");
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &speed, 0);
    st_combo(&y, L"Select your primary button", buttons, 2, GetSystemMetrics(SM_SWAPBUTTON) ? 1 : 0, CMD_PRIMARY);
    st_slider(&y, L"Cursor speed", 1, 20, speed, CMD_SPEED);
    st_combo(&y, L"Roll the mouse wheel to scroll", wheel, 2, lines == WHEEL_PAGESCROLL ? 1 : 0, CMD_WHEEL);
    if (lines != WHEEL_PAGESCROLL) st_slider(&y, L"Choose how many lines to scroll each time", 1, 100, lines, CMD_LINES);
    st_slider(&y, L"Double-click speed", 200, 900, 1100 - (int)GetDoubleClickTime(), CMD_DBLCLICK);
    st_toggle(&y, L"Scroll inactive windows when I hover over them",
              reg_dword(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"MouseWheelRouting", 2) != 0, CMD_HOVERSCROLL);
    /* the scrolling direction, the compositor's (sg-settingsctl input):
     * "down motion scrolls down" unless natural scrolling is chosen
     * (David 2026-11-13: natural scrolling should not be the default) */
    {
        static const WCHAR *const dir[] = { L"Down motion scrolls down", L"Down motion scrolls up (natural)" };
        WCHAR err[64];
        BOOL ok;
        char *ans = ctl_run(L"input", &ok, err, ARRAYSIZE(err), 5000), buf[64];
        const char *pos = NULL;
        int mouse = 0, pad = 0;
        while (ans && ctl_line(ans, "SCROLL", &pos, buf, sizeof(buf))) {
            if (!strncmp(buf, "mouse\tyes", 9)) mouse = 1;
            if (!strncmp(buf, "touchpad\tyes", 12)) pad = 1;
        }
        free(ans);
        if (ok) {
            st_combo(&y, L"Mouse wheel scrolling direction", dir, 2, mouse, CMD_SCROLL_MOUSE);
            st_combo(&y, L"Touchpad scrolling direction", dir, 2, pad, CMD_SCROLL_PAD);
        }
    }
}

BOOL set_cmd_mouse(int id, int code, HWND ctl)
{
    const UINT F = SPIF_UPDATEINIFILE | SPIF_SENDCHANGE;
    switch (id) {
    case CMD_PRIMARY:
        if (code == CBN_SELCHANGE) SystemParametersInfoW(SPI_SETMOUSEBUTTONSWAP, SendMessageW(ctl, CB_GETCURSEL, 0, 0) == 1, NULL, F);
        return TRUE;
    case CMD_WHEEL:
        if (code == CBN_SELCHANGE) {
            SystemParametersInfoW(SPI_SETWHEELSCROLLLINES, SendMessageW(ctl, CB_GETCURSEL, 0, 0) ? WHEEL_PAGESCROLL : 3, NULL, F);
            refresh_page();
        }
        return TRUE;
    case CMD_LINES:
        if (code == (PG_SCROLL_CODE | TB_ENDTRACK)) SystemParametersInfoW(SPI_SETWHEELSCROLLLINES, (UINT)SendMessageW(ctl, TBM_GETPOS, 0, 0), NULL, F);
        return TRUE;
    case CMD_SPEED:
        if (code == (PG_SCROLL_CODE | TB_ENDTRACK))
            SystemParametersInfoW(SPI_SETMOUSESPEED, 0, (void *)(INT_PTR)SendMessageW(ctl, TBM_GETPOS, 0, 0), F);
        return TRUE;
    case CMD_DBLCLICK:
        if (code == (PG_SCROLL_CODE | TB_ENDTRACK))
            SystemParametersInfoW(SPI_SETDOUBLECLICKTIME, 1100 - (UINT)SendMessageW(ctl, TBM_GETPOS, 0, 0), NULL, F);
        return TRUE;
    case CMD_SCROLL_MOUSE:
    case CMD_SCROLL_PAD:
        if (code == CBN_SELCHANGE) {
            WCHAR args[64], err[128];
            BOOL ok;
            _snwprintf(args, ARRAYSIZE(args), L"input natural %ls %ls", id == CMD_SCROLL_MOUSE ? L"mouse" : L"touchpad",
                       SendMessageW(ctl, CB_GETCURSEL, 0, 0) == 1 ? L"yes" : L"no");
            free(ctl_run(args, &ok, err, ARRAYSIZE(err), 5000));
            if (!ok) st_status(err[0] ? err : L"The scrolling direction could not be changed.");
        }
        return TRUE;
    case CMD_HOVERSCROLL:
        reg_set_dword(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"MouseWheelRouting", st_checked(ctl) ? 2 : 0);
        return TRUE;
    }
    return FALSE;
}

/* ---- AutoPlay ------------------------------------------------------------------------------------
 * Windows' page and Windows' values, read by explorer's AutoPlay (wine-sg
 * 0748, 1514) each time a drive arrives: AutoplayHandlers DisableAutoplay
 * ("Use AutoPlay for all media and devices" off), and the removable drive's
 * default, UserChosenExecuteHandlers StorageOnArrival -- MSPromptEachTime
 * (ask: the notification, and selecting it asks), MSOpenFolder (File
 * Explorer at once), MSTakeNoAction. */
static const WCHAR AUTOPLAY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\AutoplayHandlers";
static const WCHAR AUTOPLAY_CHOSEN[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\AutoplayHandlers\\UserChosenExecuteHandlers";
static const WCHAR *const AUTOPLAY_VALUES[] = { L"MSPromptEachTime", L"MSOpenFolder", L"MSTakeNoAction" };
enum { CMD_AUTOPLAY_ON = CMD_PAGE_FIRST + 1, CMD_AUTOPLAY_REMOVABLE };

void set_build_autoplay(void)
{
    static const WCHAR *const items[] = { L"Ask me every time", L"Open folder to view files (File Explorer)", L"Take no action" };
    WCHAR chosen[64] = L"";
    int y = st_title(L"AutoPlay"), sel = 0, i;
    st_toggle(&y, L"Use AutoPlay for all media and devices", !reg_dword(HKEY_CURRENT_USER, AUTOPLAY, L"DisableAutoplay", 0),
              CMD_AUTOPLAY_ON);
    y = st_head(y, L"Choose AutoPlay defaults");
    reg_sz(HKEY_CURRENT_USER, AUTOPLAY_CHOSEN, L"StorageOnArrival", chosen, ARRAYSIZE(chosen));
    for (i = 0; i < (int)ARRAYSIZE(AUTOPLAY_VALUES); i++)
        if (!lstrcmpiW(chosen, AUTOPLAY_VALUES[i])) sel = i;
    st_combo(&y, L"Removable drive", items, ARRAYSIZE(items), sel, CMD_AUTOPLAY_REMOVABLE);
}

BOOL set_cmd_autoplay(int id, int code, HWND ctl)
{
    switch (id) {
    case CMD_AUTOPLAY_ON:
#ifndef SG_MUTANT_AUTOPLAY_SWITCH_NOOP
        reg_set_dword(HKEY_CURRENT_USER, AUTOPLAY, L"DisableAutoplay", !st_checked(ctl));
#endif
        return TRUE;
    case CMD_AUTOPLAY_REMOVABLE:
        if (code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < (int)ARRAYSIZE(AUTOPLAY_VALUES))
                reg_set_sz(HKEY_CURRENT_USER, AUTOPLAY_CHOSEN, L"StorageOnArrival", AUTOPLAY_VALUES[i]);
        }
        return TRUE;
    }
    return FALSE;
}

/* ---- Typing ------------------------------------------------------------------------------------ */
static const WCHAR TABLET[] = L"Software\\Microsoft\\TabletTip\\1.7";
enum { CMD_AUTOCORRECT = CMD_PAGE_FIRST + 1, CMD_HIGHLIGHT, CMD_DELAY, CMD_RATE, CMD_BLINK, CMD_TRY,
       CMD_TIP_DESKTOP, CMD_TIP_WINDOWED };

void set_build_typing(void)
{
    UINT delay = 1, rate = 31;
    int y = st_title(L"Typing");
    SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0);
    SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &rate, 0);
    y = st_head(y, L"Spelling");
    st_toggle(&y, L"Autocorrect misspelled words", reg_dword(HKEY_CURRENT_USER, TABLET, L"EnableAutocorrection", 1) != 0, CMD_AUTOCORRECT);
    st_toggle(&y, L"Highlight misspelled words", reg_dword(HKEY_CURRENT_USER, TABLET, L"EnableSpellchecking", 1) != 0, CMD_HIGHLIGHT);
    /* the touch keyboard (sg-shell's sg-touchkbd) reads these each time a
     * touch focuses a text field */
    y = st_head(y, L"Touch keyboard");
    st_toggle(&y, L"Show the touch keyboard when not in tablet mode and there's no keyboard attached",
              reg_dword(HKEY_CURRENT_USER, TABLET, L"EnableDesktopModeAutoInvoke", 1) != 0, CMD_TIP_DESKTOP);
    st_toggle(&y, L"Automatically show the touch keyboard in windowed apps when there's no keyboard attached",
              reg_dword(HKEY_CURRENT_USER, TABLET, L"AutoInvokeInWindowedApps", 1) != 0, CMD_TIP_WINDOWED);
    y = st_head(y, L"Hardware keyboard");
    st_slider(&y, L"Repeat delay (long to short)", 0, 3, 3 - (int)delay, CMD_DELAY);
    st_slider(&y, L"Repeat rate (slow to fast)", 0, 31, rate, CMD_RATE);
    st_slider(&y, L"Cursor blink rate", 0, 10, GetCaretBlinkTime() == INFINITE ? 0 : 10 - ((int)GetCaretBlinkTime() - 200) / 100, CMD_BLINK);
    st_edit(&y, L"Click here and hold down a key to test the repeat rate", L"", CMD_TRY);
}

BOOL set_cmd_typing(int id, int code, HWND ctl)
{
    const UINT F = SPIF_UPDATEINIFILE | SPIF_SENDCHANGE;
    int pos = (int)SendMessageW(ctl, TBM_GETPOS, 0, 0);
    switch (id) {
    case CMD_AUTOCORRECT: reg_set_dword(HKEY_CURRENT_USER, TABLET, L"EnableAutocorrection", st_checked(ctl)); return TRUE;
    case CMD_HIGHLIGHT: reg_set_dword(HKEY_CURRENT_USER, TABLET, L"EnableSpellchecking", st_checked(ctl)); return TRUE;
    case CMD_TIP_DESKTOP: reg_set_dword(HKEY_CURRENT_USER, TABLET, L"EnableDesktopModeAutoInvoke", st_checked(ctl)); return TRUE;
    case CMD_TIP_WINDOWED: reg_set_dword(HKEY_CURRENT_USER, TABLET, L"AutoInvokeInWindowedApps", st_checked(ctl)); return TRUE;
    case CMD_DELAY: if (code == (PG_SCROLL_CODE | TB_ENDTRACK)) SystemParametersInfoW(SPI_SETKEYBOARDDELAY, 3 - pos, NULL, F); return TRUE;
    case CMD_RATE: if (code == (PG_SCROLL_CODE | TB_ENDTRACK)) SystemParametersInfoW(SPI_SETKEYBOARDSPEED, pos, NULL, F); return TRUE;
    case CMD_BLINK:
        if (code == (PG_SCROLL_CODE | TB_ENDTRACK)) {
            WCHAR v[16];
            UINT ms = pos ? (UINT)(200 + (10 - pos) * 100) : (UINT)-1;
            SetCaretBlinkTime(ms);
            _snwprintf(v, ARRAYSIZE(v), L"%d", pos ? (int)ms : -1);
            reg_set_sz(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"CursorBlinkRate", v);
        }
        return TRUE;
    }
    return FALSE;
}
