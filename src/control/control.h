/* sg-control -- the Stained Glass OS Control Panel: shared declarations.
 *
 * One window: a navigation bar (back, forward, up, a breadcrumb address and a
 * search box) over a page. A page is a child window rebuilt on every visit
 * from the machine's live state: painted items (text, icons, rules) plus real
 * child controls -- links are windows of their own, so Tab and Enter reach
 * everything, and IsDialogMessage does the keyboard work.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_CONTROL_H
#define SG_CONTROL_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- pages ---------------------------------------------------------------- */
enum page_id {
    PG_HOME, PG_ALL,
    PG_CAT_SYSSEC, PG_CAT_NET, PG_CAT_HW, PG_CAT_PROG, PG_CAT_USERS, PG_CAT_APPEAR, PG_CAT_CLOCK,
    PG_SYSTEM, PG_PROGRAMS, PG_USERS, PG_USERS_MANAGE, PG_DATETIME, PG_PERSONALIZE,
    PG_UPDATE, PG_NETWORK, PG_SPEECH, PG_ADMINTOOLS,
    /* Settings (SystemSettings, ms-settings:): the same page machinery in
     * the Settings window (settings.c) */
    PG_S_HOME, PG_S_SEARCH,
    PG_S_DISPLAY, PG_S_SOUND, PG_S_NOTIFY, PG_S_POWER, PG_S_STORAGE, PG_S_MULTITASK, PG_S_ABOUT,
    PG_S_BLUETOOTH, PG_S_MOUSE, PG_S_TYPING,
    PG_S_NETSTATUS, PG_S_WIFI, PG_S_ETHERNET, PG_S_PROXY,
    PG_S_BACKGROUND, PG_S_COLORS, PG_S_LOCKSCREEN, PG_S_THEMES, PG_S_START, PG_S_TASKBAR,
    PG_S_APPS, PG_S_DEFAULTAPPS, PG_S_STARTUP,
    PG_S_YOURINFO, PG_S_SIGNIN, PG_S_OTHERUSERS,
    PG_S_DATETIME, PG_S_REGION,
    PG_S_EOA_DISPLAY, PG_S_EOA_KEYBOARD, PG_S_EOA_MOUSE, PG_S_EOA_MAGNIFIER,
    PG_S_PRIV_GENERAL, PG_S_PRIV_MIC, PG_S_PRIV_CAMERA, PG_S_PRIV_LOCATION,
    PG_S_UPDATE, PG_S_RECOVERY,
    PG_COUNT
};
#define IS_SETTINGS_PAGE(p) ((p) >= PG_S_HOME && (p) < PG_COUNT)

struct page_def {
    const WCHAR *title;         /* the breadcrumb's last segment and the window title */
    enum page_id parent;        /* the breadcrumb before it (PG_COUNT: none) */
    void (*build)(void);        /* add the page's items and controls */
    BOOL (*command)(int id, int code, HWND ctl);    /* its own WM_COMMANDs */
    LRESULT (*notify)(NMHDR *nm);                   /* its own WM_NOTIFYs */
    void (*timer)(void);        /* WM_TIMER on the page, if it asked for one */
};
extern const struct page_def g_pages[PG_COUNT];

/* navigation commands: a link with id NAV(page) goes to that page */
#define NAV_BASE 1000
#define NAV(p)   (NAV_BASE + (p))
#define IS_NAV(id) ((id) >= NAV_BASE && (id) < NAV_BASE + PG_COUNT)
/* commands every page understands */
#define CMD_ALL_ITEMS   1100
#define CMD_CATEGORY    1101
#define CMD_CPL_FIRST   1200    /* ... + index: open a hosted .cpl applet */
#define CMD_PAGE_FIRST  2000    /* page-specific commands from here */
#define SHIELD_ID(n)    (0x4000 | (n))  /* a command that elevates: its link shows the shield */
#define IS_SHIELD(id)   (((id) & 0x4000) != 0)

void navigate(enum page_id p);
void refresh_page(void);
void refresh_when_back(void);
BOOL refresh_pending(void);     /* and clear it */
extern BOOL g_kbd_cues;        /* show focus rectangles */
void page_scroll_to(int y);
BOOL nav_can_back(void);
BOOL nav_back(void);
int  page_scroll_pos(void);
extern HWND g_keep_focus;     /* scroll the page to a content position */
void users_reset(void);         /* forget Manage Accounts' selection */   /* rebuild the page when the window is active again */
BOOL open_network_connections(void);   /* sg-ncpa, if installed */
BOOL open_fonts_folder(void);          /* sg-fontview /folder (fontview.exe) */
extern WCHAR g_search_text[128];
enum page_id current_page(void);
/* the Settings window (settings.c): the same pages, another frame */
extern BOOL g_settings;
extern COLORREF g_col_link, g_col_link_hot;
void register_page_classes(void);
int  settings_main(int argc, WCHAR **argv, int show);
void settings_page_shown(void);
void settings_dump(void);        /* SG_SETTINGS_DUMP, now */
BOOL settings_page_key(MSG *msg);
/* what the page shows, as text, for the gates (SG_SETTINGS_DUMP) */
void page_dump(FILE *f);
/* WM_HSCROLL from a slider on the page reaches its command() with this code */
#define PG_SCROLL_CODE 0x7F00
#define IS_SCROLL_CODE(c) (((c) & 0xFF00) == PG_SCROLL_CODE)

/* ---- look ----------------------------------------------------------------- */
#define COL_BG        RGB(0xFF, 0xFF, 0xFF)
#define COL_PANE      RGB(0xF4, 0xF7, 0xFC)
#define COL_PANE_EDGE RGB(0xE3, 0xE8, 0xF0)
#define COL_TEXT      RGB(0x1A, 0x1A, 0x1A)
#define COL_SUBTLE    RGB(0x60, 0x60, 0x60)
#define COL_TITLE     RGB(0x1E, 0x32, 0x87)
#define COL_LINK      RGB(0x00, 0x66, 0xCC)
#define COL_LINK_HOT  RGB(0x33, 0x99, 0xFF)
#define COL_CATLINK   RGB(0x0E, 0x7A, 0x0D)
#define COL_RULE      RGB(0xDD, 0xDD, 0xDD)
#define COL_OK        RGB(0x10, 0x7C, 0x10)
#define COL_WARN      RGB(0xC4, 0x2B, 0x1C)
#define COL_NAVBAR    RGB(0xFF, 0xFF, 0xFF)

extern HWND g_main, g_page;
extern HINSTANCE g_inst;
extern int g_dpi;
extern HFONT g_font_title, g_font_head, g_font_body, g_font_small, g_font_cat, g_font_big;
int S(int dip);                 /* DIPs to pixels at the window's DPI */

/* ---- building a page ---------------------------------------------------------- */
enum { LINK_PLAIN = 0, LINK_CATEGORY = 1, LINK_SHIELD = 2, LINK_BOLD = 4 };
void pg_text(int x, int y, int w, int h, HFONT f, COLORREF c, const WCHAR *s, UINT fmt);
int  pg_para(int x, int y, int w, HFONT f, COLORREF c, const WCHAR *s);   /* wraps; returns height */
void pg_textf(int x, int y, int w, HFONT f, COLORREF c, const WCHAR *fmt, ...);
HWND pg_link(int x, int y, const WCHAR *s, int id, int flags);
void pg_icon(int x, int y, int size, int icon);
void pg_rule(int x, int y, int w);
void pg_fill(int x, int y, int w, int h, COLORREF c);
void pg_swatch(int x, int y, int w, int h, COLORREF c, BOOL selected);
HWND pg_control(const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id);
HWND pg_button(const WCHAR *text, int x, int y, int w, int id);
void pg_timer(UINT ms);
/* the left task pane: "Control Panel Home" and the page's links; returns its width */
int  pg_left_pane(const WCHAR *const *labels, const int *ids, int n);
int  pg_width(void);
int  pg_height(void);
/* a title in the Control Panel's blue */
void pg_title(int x, int y, const WCHAR *s);

/* ---- icons ---------------------------------------------------------------- */
enum icon {
    IC_SYSSEC, IC_NET, IC_HW, IC_PROG, IC_USERS, IC_APPEAR, IC_CLOCK,
    IC_SYSTEM, IC_UPDATE, IC_DATETIME, IC_PERSONAL, IC_NETCENTER, IC_INET,
    IC_GAME, IC_DISPLAY, IC_GENERIC, IC_USER, IC_SHIELD, IC_OK, IC_WARN, IC_REFRESH, IC_SPEECH, IC_ADMINTOOLS, IC_FONTS,
    /* Settings' line glyphs, in the accent colour (g_glyph_color) */
    IC_G_SYSTEM, IC_G_DEVICES, IC_G_NETWORK, IC_G_PERSONAL, IC_G_APPS, IC_G_ACCOUNTS, IC_G_TIME,
    IC_G_EOA, IC_G_PRIVACY, IC_G_UPDATE, IC_G_HOME, IC_G_SEARCH, IC_G_BACK, IC_G_PC,
    IC_COUNT
};
void draw_icon(HDC dc, int icon, int x, int y, int size);
extern COLORREF g_glyph_color;

/* ---- the machine: facts shared by pages and --dump ----------------------------- */
void reg_sz(HKEY root, const WCHAR *sub, const WCHAR *val, WCHAR *out, DWORD cch);
DWORD reg_dword(HKEY root, const WCHAR *sub, const WCHAR *val, DWORD def);
BOOL reg_set_sz(HKEY root, const WCHAR *sub, const WCHAR *val, const WCHAR *data);
BOOL reg_set_dword(HKEY root, const WCHAR *sub, const WCHAR *val, DWORD data);
/* read a whole Unix text file through Wine's Z: (UTF-8); caller frees */
char *read_unix_file(const char *unix_path, DWORD *len);
BOOL unix_path_exists(const char *unix_path);
void unix_to_dos(const char *unix_path, WCHAR *out, int cch);
/* the machine's role: "" (workgroup), "member" or "dc", and realm / NetBIOS domain */
void machine_role(WCHAR *role, WCHAR *realm, WCHAR *domain, int cch);
void current_user(WCHAR *name, int cch, WCHAR *domain, int dcch);
BOOL unix_group_has(const char *group, const WCHAR *user);
void format_size(ULONGLONG bytes, WCHAR *out, int cch);

/* ---- elevation and the administration spool (admin.c) ------------------------ */
/* Run this program elevated (ShellExecute "runas": the broker's consent
 * prompt), with the given arguments. Returns FALSE if it did not start. */
BOOL run_elevated(const WCHAR *args);
/* File a request with sg-admind and wait for the answer. fields: the verb,
 * then its arguments (a password last). Returns TRUE for OK; msg gets the
 * reason on failure, or the first detail line on success. */
BOOL admin_request(const WCHAR *const *fields, int n, WCHAR *msg, int cch, DWORD timeout_ms);
BOOL is_elevated(void);
void current_zone(WCHAR *out, int cch);     /* the IANA zone, from /etc/timezone */
BOOL ntp_enabled(void);
int  load_zones(WCHAR ***out);
int  privacy_admin_consent(const WCHAR *cap, const WCHAR *on_off);   /* set_misc.c */       /* IANA zones, sorted; free each and the array */
int  admin_main(int argc, WCHAR **argv);        /* /admin VERB ...: the elevated dialogs */
int  admin_do(int argc, WCHAR **argv);          /* /admin-do VERB ...: non-interactive */
/* a small modal form: title, fields, returns TRUE on OK */
struct form_field {
    const WCHAR *label;
    WCHAR *value;               /* in and out; radios and checks: L"1" or L"0" */
    int cch;
    int kind;
    const WCHAR *const *options;    /* FF_COMBO */
    int nopt;
};
enum { FF_TEXT, FF_PASSWORD, FF_RADIO_FIRST, FF_RADIO, FF_NOTE, FF_CHECK, FF_COMBO };
BOOL run_form(HWND owner, const WCHAR *title, const WCHAR *intro, struct form_field *f, int n,
              const WCHAR *ok_label, BOOL shield);
void message(HWND owner, const WCHAR *title, const WCHAR *text, BOOL error);

/* ---- personalization (personalize.c), shared with Settings ------------------------- */
struct pstate {
    WCHAR wallpaper[MAX_PATH], source[MAX_PATH];
    int style;
    BOOL solid;
    COLORREF background, accent;
    BOOL apps_light, system_light;
};
void pers_read(struct pstate *s);
const WCHAR *pers_set_wallpaper(const WCHAR *src, int style);   /* NULL, or why not */
const WCHAR *pers_set_background(COLORREF c);
const WCHAR *pers_set_accent(COLORREF c);
const WCHAR *pers_set_mode(BOOL apps, BOOL light);
extern const COLORREF PERS_ACCENTS[20], PERS_BACKGROUNDS[12];
extern const WCHAR *const PERS_FIT_NAMES[];
int  pers_pictures(WCHAR (*out)[MAX_PATH], int max);         /* Windows' and ours */
HBITMAP pers_thumb(const WCHAR *path, int w, int h);          /* cached */
void pers_register_classes(void);   /* SgCplTile (a colour or a picture), SgCplPreview */
#define TILE_SETCOLOR  (WM_USER + 1)
#define TILE_SETBITMAP (WM_USER + 2)
#define TILE_SETSEL    (WM_USER + 3)
extern HBITMAP g_preview_pic;

/* ---- programs (programs.c), shared with Settings > Apps ------------------------------- */
struct program {
    WCHAR name[256], publisher[256], version[64], date[32], key[256], icon[MAX_PATH + 8], help[256];
    WCHAR uninstall[1024], quiet[1024], modify[1024];
    DWORD size_kb;
    BOOL msi, no_modify, no_repair, no_remove;
    const WCHAR *scope;
};
enum { PROG_UNINSTALL = CMD_PAGE_FIRST + 2, PROG_CHANGE, PROG_REPAIR, PROG_DONE };
int  prog_load(void);                       /* reads the Uninstall keys; returns the count */
const struct program *prog_get(int i);
BOOL prog_can(const struct program *p, int action);
BOOL prog_run(const struct program *p, int action, BOOL wait);   /* PROG_DONE posted to g_page after */

/* ---- accounts (users.c) ---------------------------------------------------------------- */
struct account { WCHAR name[64], full[128]; BOOL admin; };
int  acc_load(const struct account **out);      /* local people's accounts */
void acc_elevate(const WCHAR *verb, const WCHAR *name);   /* /admin VERB [NAME], elevated */

/* ---- facts shared with Settings ------------------------------------------------------ */
struct sysfacts {
    WCHAR edition[64], computer[64], fqdn[256], user[256], arch[32], os_build[64], cpu[128], ram[32];
    WCHAR role[32], realm[128], domain[64];
    BOOL elevated, admin_account;
    int policy_count, program_count;
};
void sys_gather(struct sysfacts *f);
struct zone_facts { WCHAR key[128], display[256], iana[128], offset[32], dst[256]; BOOL ntp; };
void zone_get(struct zone_facts *z);
struct ufacts {
    BOOL pending, checked, managed;
    WCHAR last[96];
    WCHAR sources[8][256];
    int nsources;
};
struct hentry { WCHAR when[40]; WCHAR what[400]; };
void update_gather(struct ufacts *u);
int  update_history(struct hentry *out, int max);

/* ---- network adapters (network.c) ------------------------------------------------------ */
struct adapter {
    WCHAR name[128], desc[256], type[32], ipv4[128], ipv6[256], gateway[128], dns[256], mac[32], speed[32];
    BOOL up, internet;
};
int load_adapters(struct adapter *out, int max);

/* ---- hosted .cpl applets ------------------------------------------------------ */
struct cpl_item { WCHAR file[MAX_PATH]; WCHAR name[128]; WCHAR info[256]; int index; };
int  cpl_list(struct cpl_item *out, int max);   /* third-party and Wine .cpl files */
BOOL cpl_open_file(const WCHAR *file, const WCHAR *arg);   /* in a process of its own */
int  cpl_run_inproc(const WCHAR *file, const WCHAR *arg);  /* /cpl FILE: run it here */

/* ---- per-page dumps (for the gate) ---------------------------------------------- */
void dump_system(void);
void dump_programs(void);
void dump_users(void);
void dump_datetime(void);
void dump_personalize(void);
void dump_update(void);
void dump_network(void);
void dump_speech(void);
void dump_items(void);
int  personalize_set(int argc, WCHAR **argv);
int  programs_uninstall_cli(const WCHAR *name);   /* --uninstall NAME */   /* --set KIND VALUE...: non-interactive */

/* page builders */
void build_home(void);   void build_all(void);   void build_category(void);
BOOL cmd_home(int, int, HWND);
void build_system(void);        BOOL cmd_system(int, int, HWND);
void build_programs(void);      BOOL cmd_programs(int, int, HWND);   LRESULT notify_programs(NMHDR *);
void build_users(void);         BOOL cmd_users(int, int, HWND);
void build_users_manage(void);  BOOL cmd_users_manage(int, int, HWND); LRESULT notify_users(NMHDR *);
void build_datetime(void);      BOOL cmd_datetime(int, int, HWND);   void timer_datetime(void);
void build_personalize(void);   BOOL cmd_personalize(int, int, HWND); LRESULT notify_personalize(NMHDR *);
void build_update(void);        BOOL cmd_update(int, int, HWND);
void build_network(void);       BOOL cmd_network(int, int, HWND);
void build_speech(void);        BOOL cmd_speech(int, int, HWND);     void timer_speech(void);
void build_admintools(void);    BOOL cmd_admintools(int, int, HWND); void dump_admintools(void);

#endif
