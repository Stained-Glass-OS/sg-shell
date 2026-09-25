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
    PG_UPDATE, PG_NETWORK,
    PG_COUNT
};

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
extern BOOL g_kbd_cues;        /* show focus rectangles */
void page_scroll_to(int y);     /* scroll the page to a content position */
void users_reset(void);         /* forget Manage Accounts' selection */   /* rebuild the page when the window is active again */
BOOL open_network_connections(void);   /* sg-ncpa, if installed */
extern WCHAR g_search_text[128];
enum page_id current_page(void);

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
    IC_GAME, IC_DISPLAY, IC_GENERIC, IC_USER, IC_SHIELD, IC_OK, IC_WARN, IC_REFRESH,
    IC_COUNT
};
void draw_icon(HDC dc, int icon, int x, int y, int size);

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

#endif
