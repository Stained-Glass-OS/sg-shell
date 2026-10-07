/* sg-control -- Settings (SystemSettings, ms-settings:): shared declarations.
 *
 * Settings is this program in another frame: the Windows 10 Settings window
 * (a navigation pane beside the page) around the same page machinery as the
 * Control Panel (main.c), and pages that share the Control Panel's logic --
 * personalize.c, programs.c, users.c, system.c, datetime.c, update.c,
 * speech.c -- rather than repeat it.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_SETTINGS_H
#define SG_SETTINGS_H

#include "control.h"
#include <shellapi.h>

/* pages: title, "parent" (unused by Settings' frame), builder, commands, notify, timer */
#define SETTINGS_PAGE_DEFS \
    [PG_S_HOME]         = { L"Home",                         PG_COUNT,  set_build_home,      set_cmd_home }, \
    [PG_S_SEARCH]       = { L"Search results",               PG_S_HOME, set_build_search,    set_cmd_home }, \
    [PG_S_DISPLAY]      = { L"Display",                      PG_S_HOME, set_build_display,   set_cmd_display, NULL, set_timer_display }, \
    [PG_S_SOUND]        = { L"Sound",                        PG_S_HOME, set_build_sound,     set_cmd_sound, NULL, set_timer_sound }, \
    [PG_S_NOTIFY]       = { L"Notifications & actions",      PG_S_HOME, set_build_notify,    set_cmd_notify }, \
    [PG_S_POWER]        = { L"Power & sleep",                PG_S_HOME, set_build_power,     set_cmd_power }, \
    [PG_S_STORAGE]      = { L"Storage",                      PG_S_HOME, set_build_storage,   set_cmd_storage }, \
    [PG_S_MULTITASK]    = { L"Multitasking",                 PG_S_HOME, set_build_multitask, set_cmd_multitask }, \
    [PG_S_ABOUT]        = { L"About",                        PG_S_HOME, set_build_about,     set_cmd_about }, \
    [PG_S_ENVVARS]      = { L"Environment variables",        PG_S_HOME, set_build_envvars,   set_cmd_envvars }, \
    [PG_S_BLUETOOTH]    = { L"Bluetooth & other devices",    PG_S_HOME, set_build_bluetooth, set_cmd_bluetooth }, \
    [PG_S_PRINTERS]     = { L"Printers & scanners",          PG_S_HOME, set_build_printers,  set_cmd_printers }, \
    [PG_S_MOUSE]        = { L"Mouse",                        PG_S_HOME, set_build_mouse,     set_cmd_mouse }, \
    [PG_S_TYPING]       = { L"Typing",                       PG_S_HOME, set_build_typing,    set_cmd_typing }, \
    [PG_S_NETSTATUS]    = { L"Status",                       PG_S_HOME, set_build_netstatus, set_cmd_net }, \
    [PG_S_WIFI]         = { L"Wi-Fi",                        PG_S_HOME, set_build_wifi,      set_cmd_net }, \
    [PG_S_ETHERNET]     = { L"Ethernet",                     PG_S_HOME, set_build_ethernet,  set_cmd_net }, \
    [PG_S_PROXY]        = { L"Proxy",                        PG_S_HOME, set_build_proxy,     set_cmd_proxy }, \
    [PG_S_FIREWALL]     = { L"Firewall",                     PG_S_HOME, set_build_firewall,  set_cmd_firewall }, \
    [PG_S_FWAPPS]       = { L"Allowed apps",                 PG_S_HOME, set_build_fwapps,    set_cmd_firewall }, \
    [PG_S_FWRULES]      = { L"Inbound port rules",           PG_S_HOME, set_build_fwrules,   set_cmd_firewall }, \
    [PG_S_BACKGROUND]   = { L"Background",                   PG_S_HOME, set_build_background, set_cmd_background }, \
    [PG_S_COLORS]       = { L"Colors",                       PG_S_HOME, set_build_colors,    set_cmd_colors }, \
    [PG_S_LOCKSCREEN]   = { L"Lock screen",                  PG_S_HOME, set_build_lockscreen, set_cmd_lockscreen }, \
    [PG_S_THEMES]       = { L"Themes",                       PG_S_HOME, set_build_themes,    set_cmd_themes }, \
    [PG_S_START]        = { L"Start",                        PG_S_HOME, set_build_start,     set_cmd_start }, \
    [PG_S_TASKBAR]      = { L"Taskbar",                      PG_S_HOME, set_build_taskbar,   set_cmd_taskbar }, \
    [PG_S_EFFECTS]      = { L"Effects",                      PG_S_HOME, set_build_effects,   set_cmd_effects }, \
    [PG_S_LOOKSHARE]    = { L"Share & reset",                PG_S_HOME, set_build_lookshare, set_cmd_lookshare }, \
    [PG_S_APPS]         = { L"Apps & features",              PG_S_HOME, set_build_apps,      set_cmd_apps }, \
    [PG_S_DEFAULTAPPS]  = { L"Default apps",                 PG_S_HOME, set_build_defaultapps, set_cmd_defaultapps }, \
    [PG_S_STARTUP]      = { L"Startup",                      PG_S_HOME, set_build_startup,   set_cmd_startup }, \
    [PG_S_YOURINFO]     = { L"Your info",                    PG_S_HOME, set_build_yourinfo,  set_cmd_accounts }, \
    [PG_S_SIGNIN]       = { L"Sign-in options",              PG_S_HOME, set_build_signin,    set_cmd_accounts }, \
    [PG_S_OTHERUSERS]   = { L"Other users",                  PG_S_HOME, set_build_otherusers, set_cmd_accounts }, \
    [PG_S_KIOSK]        = { L"Kiosk",                        PG_S_HOME, set_build_kiosk,     set_cmd_kiosk }, \
    [PG_S_DATETIME]     = { L"Date & time",                  PG_S_HOME, set_build_datetime,  set_cmd_datetime, NULL, set_timer_datetime }, \
    [PG_S_REGION]       = { L"Region",                       PG_S_HOME, set_build_region,    set_cmd_region }, \
    [PG_S_EOA_DISPLAY]  = { L"Display",                      PG_S_HOME, set_build_eoa_display, set_cmd_eoa }, \
    [PG_S_EOA_KEYBOARD] = { L"Keyboard",                     PG_S_HOME, set_build_eoa_keyboard, set_cmd_eoa }, \
    [PG_S_EOA_MOUSE]    = { L"Mouse pointer",                PG_S_HOME, set_build_eoa_mouse, set_cmd_eoa }, \
    [PG_S_EOA_MAGNIFIER] = { L"Magnifier",                   PG_S_HOME, set_build_eoa_magnifier, set_cmd_eoa }, \
    [PG_S_PRIV_GENERAL] = { L"General",                      PG_S_HOME, set_build_priv_general, set_cmd_privacy }, \
    [PG_S_PRIV_MIC]     = { L"Microphone",                   PG_S_HOME, set_build_priv_mic,  set_cmd_privacy }, \
    [PG_S_PRIV_CAMERA]  = { L"Camera",                       PG_S_HOME, set_build_priv_camera, set_cmd_privacy }, \
    [PG_S_PRIV_LOCATION]= { L"Location",                     PG_S_HOME, set_build_priv_location, set_cmd_privacy }, \
    [PG_S_UPDATE]       = { L"Updates",                      PG_S_HOME, set_build_update,    set_cmd_update, NULL, set_timer_update }, \
    [PG_S_RECOVERY]     = { L"Recovery",                     PG_S_HOME, set_build_recovery,  set_cmd_recovery }, \
    [PG_S_DEFENDER]     = { L"Virus & threat protection",    PG_S_HOME, set_build_defender,  set_cmd_defender },

/* ---- building a Settings page (settings.c) ----------------------------------------------- */
int  st_x(void);                        /* the page's left margin */
int  st_w(void);                        /* the width a column of settings takes */
int  st_title(const WCHAR *title);      /* the page's title; returns the y below it */
int  st_head(int y, const WCHAR *s);    /* a section heading */
int  st_para(int y, const WCHAR *s);    /* wrapped, subtle text */
int  st_text(int y, const WCHAR *s);    /* one line of ordinary text */
int  st_row(int y, const WCHAR *label, const WCHAR *value);     /* a spec: label and value */
HWND st_toggle(int *y, const WCHAR *label, BOOL on, int id);    /* label, then the switch */
HWND st_combo(int *y, const WCHAR *label, const WCHAR *const *items, int n, int sel, int id);
HWND st_slider(int *y, const WCHAR *label, int lo, int hi, int pos, int id);
HWND st_button(int *y, const WCHAR *text, int id);
HWND st_link(int *y, const WCHAR *text, int id);
HWND st_edit(int *y, const WCHAR *label, const WCHAR *text, int id);
void st_checkbox(int *y, const WCHAR *label, BOOL on, int id);
int  st_card(int y, int icon, const WCHAR *title, const WCHAR *sub);     /* an icon and two lines */
void st_status(const WCHAR *msg);       /* a line at the foot of the window: what happened */
BOOL st_checked(HWND c);                /* a toggle's or a check box's state */
COLORREF st_accent(void);
#define SET_TOGGLE_CLASS L"SgSetCtl"

/* ---- the native half: sg-settingsctl (sg-session) -------------------------------------- */
/* Run it with ARGS; returns its answer (UTF-8 lines, caller frees) or NULL.
 * *ok is TRUE when the last line is OK; *err gets the ERROR line's message. */
char *ctl_run(const WCHAR *args, BOOL *ok, WCHAR *err, int cch, DWORD timeout_ms);
/* the next line starting with KEY (and a space); advances *pos. Returns the rest, or NULL */
const char *ctl_line(const char *text, const char *key, const char **pos, char *buf, int cch);
/* split a tab-separated field i of s into out (UTF-8 -> WCHAR) */
BOOL ctl_field(const char *s, int i, WCHAR *out, int cch);

/* ---- page builders ------------------------------------------------------------------------- */
void set_build_home(void);      void set_build_search(void);    BOOL set_cmd_home(int, int, HWND);
void set_build_display(void);   BOOL set_cmd_display(int, int, HWND);   void set_timer_display(void);
void set_build_sound(void);     BOOL set_cmd_sound(int, int, HWND);     void set_timer_sound(void);
void set_build_notify(void);    BOOL set_cmd_notify(int, int, HWND);
void set_build_power(void);     BOOL set_cmd_power(int, int, HWND);
void set_build_storage(void);   BOOL set_cmd_storage(int, int, HWND);
void set_build_multitask(void); BOOL set_cmd_multitask(int, int, HWND);
void set_build_about(void);     BOOL set_cmd_about(int, int, HWND);
void set_build_envvars(void);   BOOL set_cmd_envvars(int, int, HWND);
void set_build_bluetooth(void); BOOL set_cmd_bluetooth(int, int, HWND);
void set_build_printers(void);  BOOL set_cmd_printers(int, int, HWND);
void set_build_mouse(void);     BOOL set_cmd_mouse(int, int, HWND);
void set_build_typing(void);    BOOL set_cmd_typing(int, int, HWND);
void set_build_netstatus(void); void set_build_wifi(void); void set_build_ethernet(void); BOOL set_cmd_net(int, int, HWND);
void set_build_proxy(void);     BOOL set_cmd_proxy(int, int, HWND);
void set_build_firewall(void);  void set_build_fwapps(void); void set_build_fwrules(void); BOOL set_cmd_firewall(int, int, HWND);
void set_build_background(void); BOOL set_cmd_background(int, int, HWND);
void set_build_colors(void);    BOOL set_cmd_colors(int, int, HWND);
void set_build_lockscreen(void); BOOL set_cmd_lockscreen(int, int, HWND);
void set_build_themes(void);    BOOL set_cmd_themes(int, int, HWND);
void set_build_lookshare(void); BOOL set_cmd_lookshare(int, int, HWND);
void set_build_effects(void);   BOOL set_cmd_effects(int, int, HWND);
void set_build_start(void);     BOOL set_cmd_start(int, int, HWND);
void set_build_taskbar(void);   BOOL set_cmd_taskbar(int, int, HWND);
void set_build_apps(void);      BOOL set_cmd_apps(int, int, HWND);
void set_build_defaultapps(void); BOOL set_cmd_defaultapps(int, int, HWND);
void set_build_startup(void);   BOOL set_cmd_startup(int, int, HWND);
void set_build_yourinfo(void);  void set_build_signin(void); void set_build_otherusers(void); BOOL set_cmd_accounts(int, int, HWND);
void set_build_datetime(void);  BOOL set_cmd_datetime(int, int, HWND); void set_timer_datetime(void);
void set_build_region(void);    BOOL set_cmd_region(int, int, HWND);
void set_build_eoa_display(void); void set_build_eoa_keyboard(void); void set_build_eoa_mouse(void); void set_build_eoa_magnifier(void); BOOL set_cmd_eoa(int, int, HWND);
void set_build_priv_general(void); void set_build_priv_mic(void); void set_build_priv_camera(void);
void set_build_priv_location(void); BOOL set_cmd_privacy(int, int, HWND);
void set_build_update(void);    BOOL set_cmd_update(int, int, HWND);    void set_timer_update(void);
void set_build_recovery(void);  BOOL set_cmd_recovery(int, int, HWND);
void set_build_defender(void);  BOOL set_cmd_defender(int, int, HWND);

void set_build_kiosk(void);     BOOL set_cmd_kiosk(int, int, HWND);

/* ---- automatic sign-in and the kiosk app (kiosk.c) ------------------------------------- */
/* /etc/stained-glass/autologon.conf, which sg-admind writes */
struct autologon { WCHAR user[64], name[128], app[MAX_PATH], args[1024], dir[MAX_PATH], desktop[1024]; };
void autologon_read(struct autologon *a);
/* Start's apps (.lnk/.url in the Start Menu folders, Start's own programs), by name */
struct start_app { WCHAR name[128], path[MAX_PATH]; };
int  start_apps(const struct start_app **out);
BOOL start_app_pick(const WCHAR *title, const WCHAR *intro, const WCHAR *ok, struct start_app *out);
BOOL start_app_resolve(const WCHAR *path, WCHAR *target, WCHAR *args, int acch, WCHAR *dir);
BOOL start_app_linux(const WCHAR *target, const WCHAR *args, WCHAR *unix_path, int cch);
BOOL startup_add_app(const struct start_app *a);   /* a shortcut in the person's Startup folder */

/* the microphone switch voice typing honours (privacy) */
BOOL privacy_mic_allowed(void);

#endif
