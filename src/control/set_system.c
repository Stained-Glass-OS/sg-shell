/* sg-control -- Settings > System: Display, Sound, Notifications & actions,
 * Power & sleep, Storage, Multitasking, About.
 *
 * Where Windows keeps a setting, it is kept there (LogPixels, the
 * notification switches, the snap switch); what only the machine can do --
 * the compositor's outputs and colour temperature, the sound server, the
 * idle timers -- goes through sg-settingsctl, and renaming the PC through
 * the Control Panel's elevated dialogs and sg-admind.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"
#include <shlobj.h>

static const WCHAR DESKTOP[] = L"Control Panel\\Desktop";

/* ---- Display ------------------------------------------------------------------------------- */
enum {
    CMD_NIGHT = CMD_PAGE_FIRST + 1, CMD_NIGHT_STRENGTH, CMD_SCALE, CMD_RES, CMD_OUTPUT, CMD_KEEP, CMD_REVERT,
    CMD_ADVANCED,
};

struct mode { int w, h; WCHAR text[48]; char spec[48]; };
static struct mode g_modes[64];
static int g_nmodes, g_cur_mode = -1;
static BOOL g_modes_ctl;                    /* the compositor's modes, through sg-settingsctl */
static char g_output[64];
static WCHAR g_output_desc[128];
static int g_revert_left;                   /* > 0: "keep these display settings?" is counting */
static char g_prev_spec[48];
static DEVMODEW g_prev_dm;

static void load_modes(void)
{
    BOOL ok;
    WCHAR err[256];
    char *ans = ctl_run(L"display", &ok, err, ARRAYSIZE(err), 6000), buf[512];
    const char *pos = NULL;
    g_nmodes = 0; g_cur_mode = -1; g_modes_ctl = FALSE; g_output[0] = 0; g_output_desc[0] = 0;
    if (ans && ok && ctl_line(ans, "OUTPUT", &pos, buf, sizeof(buf))) {
        char *tab = strchr(buf, '\t');
        if (tab) *tab = 0;
        lstrcpynA(g_output, buf, sizeof(g_output));
        if (tab) { *tab = '\t'; ctl_field(buf, 3, g_output_desc, ARRAYSIZE(g_output_desc)); }
        pos = NULL;
        while (g_nmodes < (int)ARRAYSIZE(g_modes) && ctl_line(ans, "MODE", &pos, buf, sizeof(buf))) {
            WCHAR name[64], spec[48], cur[8], pref[8];
            struct mode *m = &g_modes[g_nmodes];
            double hz = 0;
            ctl_field(buf, 0, name, ARRAYSIZE(name));
            ctl_field(buf, 1, spec, ARRAYSIZE(spec));
            ctl_field(buf, 2, cur, ARRAYSIZE(cur));
            ctl_field(buf, 3, pref, ARRAYSIZE(pref));
            if (lstrcmpA(g_output, "") && WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), NULL, NULL) && strcmp(buf, g_output)) continue;
            if (swscanf(spec, L"%dx%d@%lf", &m->w, &m->h, &hz) < 2) continue;
            WideCharToMultiByte(CP_UTF8, 0, spec, -1, m->spec, sizeof(m->spec), NULL, NULL);
            _snwprintf(m->text, ARRAYSIZE(m->text), L"%d \x00D7 %d%ls%ls", m->w, m->h,
                       hz > 0 ? L"" : L"", !lstrcmpW(pref, L"yes") ? L" (Recommended)" : L"");
            if (hz > 0) {
                WCHAR hzs[24];
                _snwprintf(hzs, ARRAYSIZE(hzs), L", %.0f Hz", hz);
                wcsncat(m->text, hzs, ARRAYSIZE(m->text) - lstrlenW(m->text) - 1);
            }
            if (!lstrcmpW(cur, L"yes")) g_cur_mode = g_nmodes;
            g_nmodes++;
        }
        g_modes_ctl = g_nmodes > 0;
    }
    free(ans);
    if (g_modes_ctl) return;
    /* Wine's own list: the display driver's modes */
    {
        DEVMODEW dm = { .dmSize = sizeof(dm) }, cur = { .dmSize = sizeof(cur) };
        DWORD i;
        EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &cur);
        for (i = 0; EnumDisplaySettingsW(NULL, i, &dm) && g_nmodes < (int)ARRAYSIZE(g_modes); i++) {
            int j, dup = 0;
            if (dm.dmBitsPerPel && dm.dmBitsPerPel < 24) continue;
            for (j = 0; j < g_nmodes; j++) if (g_modes[j].w == (int)dm.dmPelsWidth && g_modes[j].h == (int)dm.dmPelsHeight) dup = 1;
            if (dup) continue;
            g_modes[g_nmodes].w = dm.dmPelsWidth; g_modes[g_nmodes].h = dm.dmPelsHeight;
            _snwprintf(g_modes[g_nmodes].text, ARRAYSIZE(g_modes[0].text), L"%d \x00D7 %d", (int)dm.dmPelsWidth, (int)dm.dmPelsHeight);
            g_nmodes++;
        }
        for (i = 0; i < (DWORD)g_nmodes; i++)
            if (g_modes[i].w == (int)cur.dmPelsWidth && g_modes[i].h == (int)cur.dmPelsHeight) g_cur_mode = i;
        if (g_cur_mode < 0 && cur.dmPelsWidth && g_nmodes < (int)ARRAYSIZE(g_modes)) {
            g_modes[g_nmodes].w = cur.dmPelsWidth; g_modes[g_nmodes].h = cur.dmPelsHeight;
            _snwprintf(g_modes[g_nmodes].text, ARRAYSIZE(g_modes[0].text), L"%d \x00D7 %d", (int)cur.dmPelsWidth, (int)cur.dmPelsHeight);
            g_cur_mode = g_nmodes++;
        }
    }
}

static const int SCALES[] = { 100, 125, 150, 175, 200, 225, 250 };

void set_build_display(void)
{
    const WCHAR *items[64];
    WCHAR scales[ARRAYSIZE(SCALES)][32];
    const WCHAR *sitems[ARRAYSIZE(SCALES)];
    int y = st_title(L"Display"), i, dpi, sel = 0;
    BOOL ok, night = FALSE, avail = FALSE;
    int temp = 4000;
    char *ans, buf[256];
    HWND c;

    if (g_revert_left > 0) {
        WCHAR line[160];
        y = st_head(y, L"Keep these display settings?");
        _snwprintf(line, ARRAYSIZE(line), L"Reverting to previous display settings in %d seconds.", g_revert_left);
        pg_text(st_x(), y, st_w(), S(22), g_font_body, COL_TEXT, line, DT_SINGLELINE);
        y += S(34);
        st_button(&y, L"Keep changes", CMD_KEEP);
        y -= S(46);
        pg_control(L"BUTTON", L"Revert", WS_TABSTOP | BS_PUSHBUTTON, st_x() + S(150), y, S(120), S(32), CMD_REVERT);
        y += S(56);
        pg_timer(1000);
    }

    /* Night light: the compositor's colour temperature */
    y = st_head(y, L"Brightness and color");
    ans = ctl_run(L"nightlight", &ok, NULL, 0, 5000);
    if (ans && ok && ctl_line(ans, "NIGHTLIGHT", NULL, buf, sizeof(buf))) {
        WCHAR f[16];
        ctl_field(buf, 0, f, ARRAYSIZE(f)); night = !lstrcmpW(f, L"on");
        ctl_field(buf, 1, f, ARRAYSIZE(f)); temp = _wtoi(f);
        ctl_field(buf, 2, f, ARRAYSIZE(f)); avail = !lstrcmpW(f, L"yes");
    }
    free(ans);
    c = st_toggle(&y, L"Night light", night, CMD_NIGHT);
    EnableWindow(c, avail);
    if (!avail) y = st_para(y - S(8), L"Night light needs the Stained Glass compositor, and is not available in this session.");
    else {
        c = st_slider(&y, L"Strength", 0, 100, (6500 - temp) * 100 / 4600, CMD_NIGHT_STRENGTH);
        y = st_para(y - S(6), L"Night light shows warmer colors to help you sleep.");
    }

    /* Scale: LogPixels, which Windows programs read at start */
    y = st_head(y, L"Scale and layout");
    dpi = (int)reg_dword(HKEY_CURRENT_USER, DESKTOP, L"LogPixels", g_dpi);
    for (i = 0; i < (int)ARRAYSIZE(SCALES); i++) {
        _snwprintf(scales[i], ARRAYSIZE(scales[i]), L"%d%%%ls", SCALES[i], SCALES[i] == 100 ? L" (Recommended)" : L"");
        sitems[i] = scales[i];
        if (MulDiv(SCALES[i], 96, 100) == dpi) sel = i;
    }
    st_combo(&y, L"Change the size of text, apps, and other items", sitems, ARRAYSIZE(SCALES), sel, CMD_SCALE);

    load_modes();
    for (i = 0; i < g_nmodes; i++) items[i] = g_modes[i].text;
    c = st_combo(&y, L"Display resolution", items, g_nmodes, g_cur_mode, CMD_RES);
    if (!g_nmodes) EnableWindow(c, FALSE);
    if (g_modes_ctl && g_output_desc[0]) {
        WCHAR line[200];
        _snwprintf(line, ARRAYSIZE(line), L"Display: %ls", g_output_desc);
        y = st_para(y - S(6), line);
    }
    y = st_head(y, L"Multiple displays");
    y = st_para(y, L"Older displays might not always connect automatically. The displays this PC has are arranged by the "
                   L"compositor, in the order they were connected.");
    st_link(&y, L"Advanced display settings", CMD_ADVANCED);
}

static BOOL apply_mode(int i, BOOL remember)
{
    WCHAR args[128], err[256];
    BOOL ok;
    if (i < 0 || i >= g_nmodes) return FALSE;
    if (g_modes_ctl) {
        char *ans;
        if (remember && g_cur_mode >= 0) lstrcpyA(g_prev_spec, g_modes[g_cur_mode].spec);
        _snwprintf(args, ARRAYSIZE(args), L"display mode %S %S", g_output, g_modes[i].spec);
        ans = ctl_run(args, &ok, err, ARRAYSIZE(err), 10000);
        free(ans);
        if (!ok) { st_status(err[0] ? err : L"The display did not accept that resolution."); return FALSE; }
        return TRUE;
    } else {
        DEVMODEW dm = { .dmSize = sizeof(dm) };
        if (remember) { g_prev_dm.dmSize = sizeof(g_prev_dm); EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &g_prev_dm); }
        dm.dmPelsWidth = g_modes[i].w; dm.dmPelsHeight = g_modes[i].h;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        if (ChangeDisplaySettingsExW(NULL, &dm, NULL, CDS_UPDATEREGISTRY, NULL) != DISP_CHANGE_SUCCESSFUL) {
            st_status(L"The display did not accept that resolution.");
            return FALSE;
        }
        return TRUE;
    }
}

static void revert_mode(void)
{
    g_revert_left = 0;
    KillTimer(g_page, 1);
    if (g_modes_ctl && g_prev_spec[0]) {
        WCHAR args[128];
        BOOL ok;
        _snwprintf(args, ARRAYSIZE(args), L"display mode %S %S", g_output, g_prev_spec);
        free(ctl_run(args, &ok, NULL, 0, 10000));
    } else if (!g_modes_ctl && g_prev_dm.dmPelsWidth) {
        g_prev_dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        ChangeDisplaySettingsExW(NULL, &g_prev_dm, NULL, CDS_UPDATEREGISTRY, NULL);
    }
    refresh_page();
}

void set_timer_display(void)
{
    if (g_revert_left <= 0) { KillTimer(g_page, 1); return; }
    if (--g_revert_left <= 0) revert_mode();
    else refresh_page();
}

BOOL set_cmd_display(int id, int code, HWND ctl)
{
    WCHAR args[64];
    BOOL ok;
    switch (id) {
    case CMD_NIGHT:
        _snwprintf(args, ARRAYSIZE(args), L"nightlight %ls", st_checked(ctl) ? L"on" : L"off");
        free(ctl_run(args, &ok, NULL, 0, 5000));
        refresh_page();
        return TRUE;
    case CMD_NIGHT_STRENGTH:
        if (code == (PG_SCROLL_CODE | TB_ENDTRACK)) {
            int pos = (int)SendMessageW(ctl, TBM_GETPOS, 0, 0);
            _snwprintf(args, ARRAYSIZE(args), L"nightlight --temp %d", 6500 - pos * 46);
            free(ctl_run(args, &ok, NULL, 0, 5000));
        }
        return TRUE;
    case CMD_SCALE:
        if (code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < (int)ARRAYSIZE(SCALES)) {
                reg_set_dword(HKEY_CURRENT_USER, DESKTOP, L"LogPixels", MulDiv(SCALES[i], 96, 100));
                st_status(L"Some apps won't respond to scaling changes until you close and open them again, "
                          L"or sign out.");
            }
        }
        return TRUE;
    case CMD_RES:
        if (code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (i != g_cur_mode && apply_mode(i, TRUE)) { g_revert_left = 15; refresh_page(); }
        }
        return TRUE;
    case CMD_KEEP: g_revert_left = 0; KillTimer(g_page, 1); refresh_page(); return TRUE;
    case CMD_REVERT: revert_mode(); return TRUE;
    case CMD_ADVANCED: cpl_open_file(L"desk.cpl", NULL); return TRUE;
    }
    return FALSE;
}

/* ---- Sound --------------------------------------------------------------------------------- */
enum { CMD_OUT = CMD_PAGE_FIRST + 1, CMD_OUT_VOL, CMD_IN, CMD_IN_VOL, CMD_MUTE, CMD_SPEECH, CMD_MICPRIV };

struct sdev { WCHAR name[200], desc[200]; int vol; BOOL def, muted; };
static struct sdev g_sinks[24], g_sources[24];
static int g_nsinks, g_nsources;

static void load_sound(WCHAR *err, int cch, BOOL *ok)
{
    char *ans = ctl_run(L"sound", ok, err, cch, 8000), buf[1024];
    const char *pos = NULL;
    g_nsinks = g_nsources = 0;
    if (!ans) { *ok = FALSE; return; }
    while (ctl_line(ans, "SINK", &pos, buf, sizeof(buf)) && g_nsinks < (int)ARRAYSIZE(g_sinks)) {
        struct sdev *d = &g_sinks[g_nsinks++];
        WCHAR f[16];
        ctl_field(buf, 0, d->name, ARRAYSIZE(d->name));
        ctl_field(buf, 1, f, ARRAYSIZE(f)); d->def = !lstrcmpW(f, L"yes");
        ctl_field(buf, 2, f, ARRAYSIZE(f)); d->vol = _wtoi(f);
        ctl_field(buf, 3, f, ARRAYSIZE(f)); d->muted = !lstrcmpW(f, L"yes");
        ctl_field(buf, 4, d->desc, ARRAYSIZE(d->desc));
    }
    pos = NULL;
    while (ctl_line(ans, "SOURCE", &pos, buf, sizeof(buf)) && g_nsources < (int)ARRAYSIZE(g_sources)) {
        struct sdev *d = &g_sources[g_nsources++];
        WCHAR f[16];
        ctl_field(buf, 0, d->name, ARRAYSIZE(d->name));
        ctl_field(buf, 1, f, ARRAYSIZE(f)); d->def = !lstrcmpW(f, L"yes");
        ctl_field(buf, 2, f, ARRAYSIZE(f)); d->vol = _wtoi(f);
        ctl_field(buf, 3, f, ARRAYSIZE(f)); d->muted = !lstrcmpW(f, L"yes");
        ctl_field(buf, 4, d->desc, ARRAYSIZE(d->desc));
    }
    free(ans);
}

static int default_of(const struct sdev *d, int n)
{
    int i;
    for (i = 0; i < n; i++) if (d[i].def) return i;
    return n ? 0 : -1;
}

void set_build_sound(void)
{
    const WCHAR *items[24];
    WCHAR err[256] = L"";
    BOOL ok;
    int y = st_title(L"Sound"), i, d;
    HWND c;
    load_sound(err, ARRAYSIZE(err), &ok);
    if (!ok) {
        WCHAR line[400];
        _snwprintf(line, ARRAYSIZE(line), L"Sound settings are not available right now. %ls", err);
        y = st_para(y, line);
    }
    y = st_head(y, L"Output");
    for (i = 0; i < g_nsinks; i++) items[i] = g_sinks[i].desc;
    d = default_of(g_sinks, g_nsinks);
    c = st_combo(&y, L"Choose your output device", items, g_nsinks, d, CMD_OUT);
    if (!g_nsinks) { EnableWindow(c, FALSE); y = st_para(y - S(6), L"No output devices found."); }
    c = st_slider(&y, L"Master volume", 0, 100, d >= 0 ? min(g_sinks[d].vol, 100) : 0, CMD_OUT_VOL);
    EnableWindow(c, d >= 0);
    if (d >= 0) st_checkbox(&y, L"Mute", g_sinks[d].muted, CMD_MUTE);

    y = st_head(y, L"Input");
    for (i = 0; i < g_nsources; i++) items[i] = g_sources[i].desc;
    d = default_of(g_sources, g_nsources);
    c = st_combo(&y, L"Choose your input device", items, g_nsources, d, CMD_IN);
    if (!g_nsources) { EnableWindow(c, FALSE); y = st_para(y - S(6), L"No input devices found."); }
    c = st_slider(&y, L"Input volume", 0, 100, d >= 0 ? min(g_sources[d].vol, 100) : 0, CMD_IN_VOL);
    EnableWindow(c, d >= 0);
    if (!privacy_mic_allowed())
        y = st_para(y, L"Microphone access for this device is off, so apps and voice typing cannot use it.");

    y = st_head(y, L"Related settings");
    st_link(&y, L"Microphone privacy settings", CMD_MICPRIV);
    st_link(&y, L"Speech recognition", CMD_SPEECH);
}

static void sound_set(const WCHAR *verb, const WCHAR *kind, const WCHAR *name, const WCHAR *value)
{
    WCHAR args[600], err[256];
    BOOL ok;
    _snwprintf(args, ARRAYSIZE(args), L"sound %ls %ls \"%ls\"%ls%ls", verb, kind, name, value ? L" " : L"", value ? value : L"");
    free(ctl_run(args, &ok, err, ARRAYSIZE(err), 8000));
    if (!ok) st_status(err[0] ? err : L"The sound setting could not be changed.");
}

BOOL set_cmd_sound(int id, int code, HWND ctl)
{
    WCHAR v[16];
    int i, d;
    switch (id) {
    case CMD_OUT: case CMD_IN:
        if (code == CBN_SELCHANGE) {
            i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (id == CMD_OUT && i >= 0 && i < g_nsinks) sound_set(L"default", L"sink", g_sinks[i].name, NULL);
            if (id == CMD_IN && i >= 0 && i < g_nsources) sound_set(L"default", L"source", g_sources[i].name, NULL);
            refresh_page();
        }
        return TRUE;
    case CMD_OUT_VOL: case CMD_IN_VOL:
        if (code == (PG_SCROLL_CODE | TB_ENDTRACK)) {
            _snwprintf(v, ARRAYSIZE(v), L"%d", (int)SendMessageW(ctl, TBM_GETPOS, 0, 0));
            if (id == CMD_OUT_VOL && (d = default_of(g_sinks, g_nsinks)) >= 0) sound_set(L"volume", L"sink", g_sinks[d].name, v);
            if (id == CMD_IN_VOL && (d = default_of(g_sources, g_nsources)) >= 0) sound_set(L"volume", L"source", g_sources[d].name, v);
        }
        return TRUE;
    case CMD_MUTE:
        if ((d = default_of(g_sinks, g_nsinks)) >= 0) sound_set(L"mute", L"sink", g_sinks[d].name, st_checked(ctl) ? L"yes" : L"no");
        return TRUE;
    case CMD_SPEECH: navigate(PG_SPEECH); return TRUE;
    case CMD_MICPRIV: navigate(PG_S_PRIV_MIC); return TRUE;
    }
    return FALSE;
}

/* ---- Notifications & actions -------------------------------------------------------------- */
static const WCHAR PUSH[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications";
static const WCHAR NOTIFY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings";
static const WCHAR SG_NOTIFY[] = L"Software\\Stained Glass\\Notifications";
enum { CMD_TOASTS = CMD_PAGE_FIRST + 1, CMD_LOCKTOASTS, CMD_SOUNDS, CMD_FOCUS, CMD_TIPS };

void set_build_notify(void)
{
    static const WCHAR *const focus[] = { L"Off", L"Priority only", L"Alarms only" };
    int y = st_title(L"Notifications & actions");
    y = st_head(y, L"Notifications");
    st_toggle(&y, L"Get notifications from apps and other senders",
              reg_dword(HKEY_CURRENT_USER, PUSH, L"ToastEnabled", 1) != 0, CMD_TOASTS);
    st_checkbox(&y, L"Show notifications on the lock screen",
                reg_dword(HKEY_CURRENT_USER, NOTIFY, L"NOC_GLOBAL_SETTING_ALLOW_TOASTS_ABOVE_LOCK", 1) != 0, CMD_LOCKTOASTS);
    st_checkbox(&y, L"Allow notifications to play sounds",
                reg_dword(HKEY_CURRENT_USER, NOTIFY, L"NOC_GLOBAL_SETTING_ALLOW_NOTIFICATION_SOUND", 1) != 0, CMD_SOUNDS);
    st_checkbox(&y, L"Get tips, tricks, and suggestions as you use Windows",
                reg_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager",
                          L"SubscribedContent-338389Enabled", 0) != 0, CMD_TIPS);
    y += S(8);
    y = st_head(y, L"Focus assist");
    st_combo(&y, L"Choose which notifications you see and hear", focus, 3,
             (int)reg_dword(HKEY_CURRENT_USER, SG_NOTIFY, L"FocusAssist", 0) % 3, CMD_FOCUS);
}

BOOL set_cmd_notify(int id, int code, HWND ctl)
{
    switch (id) {
    case CMD_TOASTS: reg_set_dword(HKEY_CURRENT_USER, PUSH, L"ToastEnabled", st_checked(ctl)); return TRUE;
    case CMD_LOCKTOASTS: reg_set_dword(HKEY_CURRENT_USER, NOTIFY, L"NOC_GLOBAL_SETTING_ALLOW_TOASTS_ABOVE_LOCK", st_checked(ctl)); return TRUE;
    case CMD_SOUNDS: reg_set_dword(HKEY_CURRENT_USER, NOTIFY, L"NOC_GLOBAL_SETTING_ALLOW_NOTIFICATION_SOUND", st_checked(ctl)); return TRUE;
    case CMD_TIPS:
        reg_set_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager",
                      L"SubscribedContent-338389Enabled", st_checked(ctl));
        return TRUE;
    case CMD_FOCUS:
        if (code == CBN_SELCHANGE) reg_set_dword(HKEY_CURRENT_USER, SG_NOTIFY, L"FocusAssist", (DWORD)SendMessageW(ctl, CB_GETCURSEL, 0, 0));
        return TRUE;
    }
    return FALSE;
}

/* ---- Power & sleep --------------------------------------------------------------------------- */
enum { CMD_SCREEN = CMD_PAGE_FIRST + 1, CMD_SLEEP, CMD_POWEROPTS };
static const int MINUTES[] = { 1, 2, 3, 5, 10, 15, 20, 25, 30, 45, 60, 120, 180, 240, 300, 0 };

static int minutes_index(int m)
{
    int i;
    for (i = 0; i < (int)ARRAYSIZE(MINUTES); i++) if (MINUTES[i] == m) return i;
    return ARRAYSIZE(MINUTES) - 1;
}

void set_build_power(void)
{
    WCHAR labels[ARRAYSIZE(MINUTES)][24];
    const WCHAR *items[ARRAYSIZE(MINUTES)];
    char *ans, buf[128];
    BOOL ok;
    WCHAR err[256] = L"";
    int y = st_title(L"Power & sleep"), i, screen = 10, sleep = 0;
    BOOL avail = FALSE;
    HWND a, b;
    for (i = 0; i < (int)ARRAYSIZE(MINUTES); i++) {
        if (!MINUTES[i]) lstrcpyW(labels[i], L"Never");
        else if (MINUTES[i] >= 60) _snwprintf(labels[i], ARRAYSIZE(labels[i]), L"%d hour%ls", MINUTES[i] / 60, MINUTES[i] >= 120 ? L"s" : L"");
        else _snwprintf(labels[i], ARRAYSIZE(labels[i]), L"%d minute%ls", MINUTES[i], MINUTES[i] > 1 ? L"s" : L"");
        items[i] = labels[i];
    }
    ans = ctl_run(L"power", &ok, err, ARRAYSIZE(err), 5000);
    if (ans && ok && ctl_line(ans, "POWER", NULL, buf, sizeof(buf))) {
        WCHAR f[16];
        ctl_field(buf, 0, f, ARRAYSIZE(f)); screen = _wtoi(f);
        ctl_field(buf, 1, f, ARRAYSIZE(f)); sleep = _wtoi(f);
        ctl_field(buf, 2, f, ARRAYSIZE(f)); avail = !lstrcmpW(f, L"yes");
    }
    free(ans);
    y = st_head(y, L"Screen");
    a = st_combo(&y, L"When plugged in, turn off after", items, ARRAYSIZE(MINUTES), minutes_index(screen), CMD_SCREEN);
    y = st_head(y, L"Sleep");
    b = st_combo(&y, L"When plugged in, PC goes to sleep after", items, ARRAYSIZE(MINUTES), minutes_index(sleep), CMD_SLEEP);
    if (!ok) { EnableWindow(a, FALSE); EnableWindow(b, FALSE); }
    if (!avail)
        y = st_para(y, ok ? L"These take effect in a Stained Glass session: they are saved, and applied the next time you sign in."
                          : L"Power settings are not available right now.");
    y = st_head(y, L"Save energy and battery life");
    y = st_para(y, L"Set shorter times above to save energy. The screen turns off first; the PC sleeps later.");
}

BOOL set_cmd_power(int id, int code, HWND ctl)
{
    WCHAR args[64], err[256];
    BOOL ok;
    int i;
    if ((id != CMD_SCREEN && id != CMD_SLEEP) || code != CBN_SELCHANGE) return id == CMD_POWEROPTS;
    i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
    if (i < 0 || i >= (int)ARRAYSIZE(MINUTES)) return TRUE;
    _snwprintf(args, ARRAYSIZE(args), L"power %ls %d", id == CMD_SCREEN ? L"--screen" : L"--sleep", MINUTES[i]);
    free(ctl_run(args, &ok, err, ARRAYSIZE(err), 5000));
    if (!ok) st_status(err[0] ? err : L"The power setting could not be saved.");
    return TRUE;
}

/* ---- Storage --------------------------------------------------------------------------------- */
enum { CMD_SENSE = CMD_PAGE_FIRST + 1, CMD_CLEAN, CMD_OPEN_FIRST = CMD_PAGE_FIRST + 10 };
static const WCHAR SENSE[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\StorageSense\\Parameters\\StoragePolicy";

static ULONGLONG dir_size(const WCHAR *dir, int depth, int *budget)
{
    WCHAR pattern[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    ULONGLONG total = 0;
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if (--*budget <= 0) break;
        if (!lstrcmpW(fd.cFileName, L".") || !lstrcmpW(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 12 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                _snwprintf(sub, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
                total += dir_size(sub, depth + 1, budget);
            }
        } else total += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

static const struct { int csidl; const WCHAR *name; } FOLDERS[] = {
    { CSIDL_PERSONAL, L"Documents" }, { CSIDL_MYPICTURES, L"Pictures" }, { CSIDL_MYMUSIC, L"Music" },
    { CSIDL_MYVIDEO, L"Videos" }, { CSIDL_DESKTOPDIRECTORY, L"Desktop" },
};

static int clean_temp(const WCHAR *dir, ULONGLONG *freed)
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!lstrcmpW(fd.cFileName, L".") || !lstrcmpW(fd.cFileName, L"..")) continue;
        _snwprintf(path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            n += clean_temp(path, freed);
            RemoveDirectoryW(path);
        } else if (DeleteFileW(path)) { n++; *freed += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

void set_build_storage(void)
{
    WCHAR drives[128], *d, line[256], a[32], b[32], dir[MAX_PATH];
    int y = st_title(L"Storage"), i, budget;
    st_toggle(&y, L"Storage Sense", reg_dword(HKEY_CURRENT_USER, SENSE, L"01", 0) != 0, CMD_SENSE);
    y = st_para(y - S(8), L"Storage Sense can automatically free up space by getting rid of files you don't need, like "
                          L"temporary files.");
    y = st_head(y, L"Local Disk");
    GetLogicalDriveStringsW(ARRAYSIZE(drives), drives);
    for (d = drives; *d; d += lstrlenW(d) + 1) {
        ULARGE_INTEGER avail, total, free_;
        UINT type = GetDriveTypeW(d);
        if (type != DRIVE_FIXED) continue;
        if (!GetDiskFreeSpaceExW(d, &avail, &total, &free_) || !total.QuadPart) continue;
        format_size(total.QuadPart - free_.QuadPart, a, ARRAYSIZE(a));
        format_size(total.QuadPart, b, ARRAYSIZE(b));
        _snwprintf(line, ARRAYSIZE(line), L"%ls  %ls used of %ls", d, a, b);
        pg_text(st_x(), y, st_w(), S(22), g_font_body, COL_TEXT, line, DT_SINGLELINE | DT_END_ELLIPSIS);
        y += S(26);
        pg_fill(st_x(), y, S(400), S(10), RGB(0xE0, 0xE0, 0xE0));
        pg_fill(st_x(), y, (int)(S(400) * ((double)(total.QuadPart - free_.QuadPart) / total.QuadPart)), S(10), st_accent());
        y += S(24);
    }
    y = st_head(y, L"This is how your storage is used");
    for (i = 0; i < (int)ARRAYSIZE(FOLDERS); i++) {
        if (FAILED(SHGetFolderPathW(NULL, FOLDERS[i].csidl, NULL, 0, dir))) continue;
        budget = 20000;
        format_size(dir_size(dir, 0, &budget), a, ARRAYSIZE(a));
        y = st_row(y, FOLDERS[i].name, a);
    }
    GetTempPathW(MAX_PATH, dir);
    budget = 20000;
    format_size(dir_size(dir, 0, &budget), a, ARRAYSIZE(a));
    y = st_row(y, L"Temporary files", a);
    y += S(8);
    st_button(&y, L"Remove temporary files", CMD_CLEAN);
}

BOOL set_cmd_storage(int id, int code, HWND ctl)
{
    (void)code;
    if (id == CMD_SENSE) { reg_set_dword(HKEY_CURRENT_USER, SENSE, L"01", st_checked(ctl)); return TRUE; }
    if (id == CMD_CLEAN) {
        WCHAR dir[MAX_PATH], msg[128], size[32];
        ULONGLONG freed = 0;
        int n;
        GetTempPathW(MAX_PATH, dir);
        if (lstrlenW(dir) > 3 && dir[lstrlenW(dir) - 1] == L'\\') dir[lstrlenW(dir) - 1] = 0;
        n = clean_temp(dir, &freed);
        format_size(freed, size, ARRAYSIZE(size));
        _snwprintf(msg, ARRAYSIZE(msg), L"Removed %d temporary files (%ls). Files in use were left alone.", n, size);
        st_status(msg);
        return TRUE;
    }
    return FALSE;
}

/* ---- Multitasking ------------------------------------------------------------------------------- */
static const WCHAR ADVANCED[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
enum { CMD_SNAP = CMD_PAGE_FIRST + 1, CMD_VD_TASKBAR, CMD_VD_ALTTAB };

void set_build_multitask(void)
{
    static const WCHAR *const scope[] = { L"Only the desktop I'm using", L"All desktops" };
    WCHAR v[8] = L"1";
    int y = st_title(L"Multitasking");
    y = st_head(y, L"Snap windows");
    reg_sz(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WindowArrangementActive", v, ARRAYSIZE(v));
    st_toggle(&y, L"Arrange windows automatically by dragging them to the sides or corners of the screen", v[0] != L'0', CMD_SNAP);
    y = st_head(y, L"Virtual desktops");
    st_combo(&y, L"On the taskbar, show windows that are open on", scope, 2,
             reg_dword(HKEY_CURRENT_USER, ADVANCED, L"VirtualDesktopTaskbarFilter", 1) ? 0 : 1, CMD_VD_TASKBAR);
    st_combo(&y, L"Pressing Alt+Tab shows windows that are open on", scope, 2,
             reg_dword(HKEY_CURRENT_USER, ADVANCED, L"VirtualDesktopAltTabFilter", 1) ? 0 : 1, CMD_VD_ALTTAB);
    y = st_para(y, L"Win+Tab opens Task View, where you can add desktops. Win+Ctrl+Left and Right switch between them.");
}

BOOL set_cmd_multitask(int id, int code, HWND ctl)
{
    switch (id) {
    case CMD_SNAP:
        reg_set_sz(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WindowArrangementActive", st_checked(ctl) ? L"1" : L"0");
        SystemParametersInfoW(SPI_SETWINARRANGING, st_checked(ctl), NULL, SPIF_SENDCHANGE);
        return TRUE;
    case CMD_VD_TASKBAR: case CMD_VD_ALTTAB:
        if (code == CBN_SELCHANGE)
            reg_set_dword(HKEY_CURRENT_USER, ADVANCED, id == CMD_VD_TASKBAR ? L"VirtualDesktopTaskbarFilter" : L"VirtualDesktopAltTabFilter",
                          SendMessageW(ctl, CB_GETCURSEL, 0, 0) == 0);
        return TRUE;
    }
    return FALSE;
}

/* ---- About ------------------------------------------------------------------------------------ */
enum { CMD_RENAME = SHIELD_ID(CMD_PAGE_FIRST + 1), CMD_RENAME_ADV = SHIELD_ID(CMD_PAGE_FIRST + 2), CMD_COPY = CMD_PAGE_FIRST + 3,
       CMD_COPY2 = CMD_PAGE_FIRST + 4 };

static void about_text(WCHAR *out, int cch, BOOL windows)
{
    struct sysfacts f;
    WCHAR guid[64] = L"";
    sys_gather(&f);
    reg_sz(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Cryptography", L"MachineGuid", guid, ARRAYSIZE(guid));
    if (!windows)
        _snwprintf(out, cch, L"Device name\t%ls\r\nProcessor\t%ls\r\nInstalled RAM\t%ls\r\nDevice ID\t%ls\r\nSystem type\t%ls\r\n",
                   f.computer, f.cpu, f.ram, guid[0] ? guid : L"-", f.arch);
    else
        _snwprintf(out, cch, L"Edition\t%ls\r\nVersion\t%ls\r\n", f.edition, f.os_build);
    out[cch - 1] = 0;
}

void set_build_about(void)
{
    struct sysfacts f;
    WCHAR guid[64] = L"", line[256];
    const char *(CDECL *wine_version)(void) = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
    int y = st_title(L"About");
    sys_gather(&f);
    reg_sz(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Cryptography", L"MachineGuid", guid, ARRAYSIZE(guid));
    y = st_card(y, IC_G_PC, f.computer, f.admin_account ? L"Your PC, and you are an administrator" : L"Your PC");
    y = st_head(y, L"Device specifications");
    y = st_row(y, L"Device name", f.computer);
    y = st_row(y, L"Full device name", f.fqdn);
    y = st_row(y, L"Processor", f.cpu);
    y = st_row(y, L"Installed RAM", f.ram);
    y = st_row(y, L"Device ID", guid[0] ? guid : L"-");
    y = st_row(y, L"System type", f.arch);
    if (f.role[0]) y = st_row(y, L"Domain", f.realm);
    y += S(8);
    st_button(&y, L"Copy", CMD_COPY);
    st_button(&y, L"Rename this PC", CMD_RENAME);
    y = st_head(y, L"Windows specifications");
    y = st_row(y, L"Edition", f.edition);
    y = st_row(y, L"Version", f.os_build);
    if (wine_version) { _snwprintf(line, ARRAYSIZE(line), L"Wine %S (wine-sg)", wine_version()); y = st_row(y, L"Compatibility", line); }
    y += S(8);
    st_button(&y, L"Copy", CMD_COPY2);
    y = st_para(y, L"Stained Glass OS is free software: its programs are licensed under the GPL, LGPL and AGPL.");
    y = st_head(y, L"Related settings");
    st_link(&y, L"Rename this PC (advanced)", CMD_RENAME_ADV);
}

BOOL set_cmd_about(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    switch (id) {
    case CMD_RENAME: if (run_elevated(L"/admin rename-pc")) refresh_when_back(); return TRUE;
    case CMD_RENAME_ADV: if (run_elevated(L"/admin rename")) refresh_when_back(); return TRUE;
    case CMD_COPY: case CMD_COPY2: {
        WCHAR text[1024];
        HGLOBAL h;
        about_text(text, ARRAYSIZE(text), id == CMD_COPY2);
        if (OpenClipboard(g_main)) {
            EmptyClipboard();
            if ((h = GlobalAlloc(GMEM_MOVEABLE, (lstrlenW(text) + 1) * sizeof(WCHAR)))) {
                lstrcpyW(GlobalLock(h), text);
                GlobalUnlock(h);
                SetClipboardData(CF_UNICODETEXT, h);
            }
            CloseClipboard();
        }
        return TRUE;
    }
    }
    return FALSE;
}
