/* sg-control -- Settings > Devices: Bluetooth & other devices, Mouse, Typing.
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

void set_build_bluetooth(void)
{
    WCHAR err[256] = L"", line[300];
    BOOL ok, present = FALSE, powered = FALSE;
    char *ans = ctl_run(g_bt_scan ? L"bluetooth scan 8" : L"bluetooth", &ok, err, ARRAYSIZE(err), g_bt_scan ? 30000 : 15000), buf[512];
    const char *pos = NULL;
    int y = st_title(L"Bluetooth & other devices"), i, shown = 0;
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

/* ---- Mouse ---------------------------------------------------------------------------------------- */
enum { CMD_PRIMARY = CMD_PAGE_FIRST + 1, CMD_WHEEL, CMD_LINES, CMD_SPEED, CMD_DBLCLICK, CMD_HOVERSCROLL };

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
    case CMD_HOVERSCROLL:
        reg_set_dword(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"MouseWheelRouting", st_checked(ctl) ? 2 : 0);
        return TRUE;
    }
    return FALSE;
}

/* ---- Typing ------------------------------------------------------------------------------------ */
static const WCHAR TABLET[] = L"Software\\Microsoft\\TabletTip\\1.7";
enum { CMD_AUTOCORRECT = CMD_PAGE_FIRST + 1, CMD_HIGHLIGHT, CMD_DELAY, CMD_RATE, CMD_BLINK, CMD_TRY };

void set_build_typing(void)
{
    UINT delay = 1, rate = 31;
    int y = st_title(L"Typing");
    SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &delay, 0);
    SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &rate, 0);
    y = st_head(y, L"Spelling");
    st_toggle(&y, L"Autocorrect misspelled words", reg_dword(HKEY_CURRENT_USER, TABLET, L"EnableAutocorrection", 1) != 0, CMD_AUTOCORRECT);
    st_toggle(&y, L"Highlight misspelled words", reg_dword(HKEY_CURRENT_USER, TABLET, L"EnableSpellchecking", 1) != 0, CMD_HIGHLIGHT);
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
