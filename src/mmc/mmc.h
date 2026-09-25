/* sg-mmc -- the administrative consoles (services.msc, eventvwr.msc, devmgmt.msc,
 * diskmgmt.msc, compmgmt.msc): a lightweight console host in our own code -- a
 * console tree, a result pane and an Actions pane, as Windows' MMC lays them
 * out -- and the snap-ins that fill it. No MMC snap-in COM: a .msc file only
 * names which console to open.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_MMC_H
#define SG_MMC_H

#define WIN32_LEAN_AND_MEAN
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* ---- the palette (Stained Glass Light) ---------------------------------- */
#define C_BG        RGB(255, 255, 255)
#define C_SURFACE   RGB(243, 243, 243)
#define C_LINE      RGB(222, 222, 222)
#define C_TEXT      RGB(0, 0, 0)
#define C_SUBTLE    RGB(96, 96, 96)
#define C_ACCENT    RGB(112, 48, 192)
#define C_ACCENT_LT RGB(231, 222, 246)
#define C_HOVER     RGB(242, 237, 250)
#define C_HEAD      RGB(80, 30, 150)
#define C_WARN_BG   RGB(255, 244, 206)

extern int g_dpi;
#define S(v) MulDiv((v), g_dpi, 96)
extern HINSTANCE g_inst;
extern HFONT g_font, g_font_bold, g_font_head;
extern HWND g_main;

/* ---- icons: one strip drawn at build time (gen-icons.py) ------------------ */
enum
{
    IC_COMPUTER, IC_SERVICES, IC_SERVICE, IC_EVENTS, IC_LOG, IC_ERROR, IC_WARNING, IC_INFO,
    IC_DEVMGR, IC_DEVICE, IC_DISPLAY, IC_NETWORK, IC_DISK, IC_SOUND, IC_USB, IC_KEYBOARD,
    IC_MOUSE, IC_CPU, IC_SYSTEM, IC_DISKMGMT, IC_USERS, IC_USER, IC_GROUP, IC_SHARE,
    IC_FOLDER, IC_SGLOGO, IC_AUDIT_OK, IC_AUDIT_FAIL, IC_NODRIVER, IC_CDROM, IC_BATTERY, IC_CAMERA,
    IC_BLUETOOTH, IC_PRINTER, IC_MONITOR, IC_STORAGE, IC_HID, IC_TOOLS, IC_STORAGEFOLDER, IC_SESSION,
    /* toolbar */
    IC_BACK, IC_FORWARD, IC_UP, IC_TREE, IC_REFRESH, IC_PROPS, IC_HELP, IC_START,
    IC_STOP, IC_PAUSE, IC_RESTART, IC_EXPORT, IC_FILTER, IC_CLEAR, IC_SCAN, IC_ACTIONS,
    IC_COUNT
};
extern HIMAGELIST g_icons;          /* 16x16 (scaled for dpi) */
extern HIMAGELIST g_icons32;        /* 32x32, for dialogs */

/* ---- the console tree ------------------------------------------------------ */
typedef struct node node_t;
typedef struct pane pane_t;

#define MAX_VERBS 16
typedef struct verb
{
    int id;                 /* the snap-in's own id */
    WCHAR name[64];         /* menu text, may carry & */
    int icon;               /* toolbar icon, -1 = menu/Actions only */
    BOOL enabled;
    BOOL separator_before;
} verb_t;

typedef struct verbs
{
    verb_t v[MAX_VERBS];
    int n;
} verbs_t;

typedef struct snapin
{
    /* create the node's children (called once, before first expand/select) */
    void (*expand)(node_t *n);
    /* fill the result pane for n: columns and rows (pane_*), or a custom view */
    void (*show)(node_t *n);
    /* what can be done: row = the selected row's key, or -1 for the node itself */
    void (*verbs)(node_t *n, LPARAM row, BOOL have_row, verbs_t *out);
    void (*invoke)(node_t *n, LPARAM row, BOOL have_row, int verb);
    /* double-click / Enter / Properties on a row */
    void (*open)(node_t *n, LPARAM row);
    /* the selected row changed (e.g. a preview pane) */
    void (*selchange)(node_t *n, LPARAM row, BOOL have_row);
    /* once a second while shown (optional) */
    void (*tick)(node_t *n);
    /* the custom view needs laying out in rc (client coordinates of the main window) */
    void (*layout)(node_t *n, const RECT *rc);
    /* leaving this node */
    void (*hide)(node_t *n);
    /* extra dump lines for gates */
    void (*dump)(node_t *n, FILE *f);
} snapin_t;

struct node
{
    WCHAR title[128];
    WCHAR desc[256];            /* the result pane's header line, optional */
    int icon;
    const snapin_t *ops;
    void *data;
    LPARAM param;
    node_t *parent, *child, *next;
    HTREEITEM hti;
    BOOL expanded;
    BOOL custom;                /* the snap-in draws its own result view */
};

node_t *node_add(node_t *parent, const WCHAR *title, int icon, const snapin_t *ops, void *data);
void node_free_children(node_t *n);
void node_select(node_t *n);
node_t *node_current(void);
void node_refresh_tree(node_t *n);     /* after changing children */

/* ---- the result pane (a report list) -------------------------------------- */
#define MAX_COLS 12
void pane_columns(const WCHAR *const *names, const int *widths, int n);  /* resets the list */
void pane_numeric(int col);             /* sort that column by number */
void pane_begin(void);                  /* before re-adding rows: keeps selection by key */
int pane_add(LPARAM key, int icon, const WCHAR *const *cells);
void pane_set(LPARAM key, int col, const WCHAR *text);
void pane_set_icon(LPARAM key, int icon);
void pane_end(void);                    /* sorts, restores the selection */
void pane_sort(int col, BOOL desc);
BOOL pane_selected(LPARAM *key);
BOOL pane_select_key(LPARAM key);
HWND pane_list(void);
void pane_empty_text(const WCHAR *text);   /* shown when there are no rows */
void pane_show_list(BOOL show);         /* a custom view hides the list */
void pane_list_rect(RECT *rc);          /* where the list goes, in main-window client coords */
void pane_set_list_rect(const RECT *rc);/* a custom view may shrink the list (preview panes) */

/* ---- frame services -------------------------------------------------------- */
void frame_status(const WCHAR *fmt, ...);
void frame_update_verbs(void);          /* re-query verbs: toolbar, Action menu, Actions pane */
void frame_custom_selection(LPARAM key, BOOL have, const WCHAR *name);  /* a custom view's selection */
void frame_set_initial(node_t *n);      /* the node to open first (e.g. eventvwr /c:System) */  /* a custom view's selection */
void frame_banner(const WCHAR *text);   /* a yellow bar above the result pane; NULL hides */
int frame_message(UINT flags, const WCHAR *title, const WCHAR *fmt, ...);
void frame_dump(void);                  /* rewrite SG_MMC_DUMP */
void frame_dump_later(void);
BOOL is_admin(void);
BOOL relaunch_elevated(void);
const WCHAR *console_name(void);
void error_text(DWORD err, WCHAR *out, int cch);
void fmt_bytes(ULONGLONG b, WCHAR *out, int cch);
void fmt_time(ULONGLONG unix_seconds, WCHAR *out, int cch);   /* local date and time */
void screen_center(HWND h, const RECT *client_rc, POINT *out);

/* ---- the Linux side: sg-session's sg-sysinfo, through its bridge ----------- */
#define SYS_MAX_LINES 60000
typedef struct sys_reply
{
    int ok;
    char kind[32];
    char message[512];
    int nlines;
    char **lines;
} sys_reply_t;

BOOL sys_init(const WCHAR *cmdline);    /* TRUE if bridged */
BOOL sys_bridged(void);
BOOL sys_relaunch(const WCHAR *args);
void sys_request(sys_reply_t *r, ...);  /* NULL-terminated UTF-8 argv */
void sys_request_argv(sys_reply_t *r, int argc, const char *const *argv);
void sys_free(sys_reply_t *r);
/* the value after "KEY " between line `from` and the next END; NULL if none */
const char *sys_field(const sys_reply_t *r, int from, const char *key);
int sys_next_block(const sys_reply_t *r, int from, const char *head); /* index of next line starting "head " or -1 */
void utf8_to_w(const char *s, WCHAR *out, int cch);
char *w_to_utf8(const WCHAR *s);

/* ---- in-memory dialogs ----------------------------------------------------- */
typedef struct dlgt
{
    WORD buf[16384];
    WORD *p;
    DLGTEMPLATE *t;
} dlgt_t;
void dlg_begin(dlgt_t *d, const WCHAR *title, DWORD style, short cx, short cy);
void dlg_item(dlgt_t *d, const WCHAR *cls, WORD atom, const WCHAR *text, WORD id, DWORD style,
              short x, short y, short cx, short cy);
#define ATOM_BUTTON 0x80
#define ATOM_EDIT   0x81
#define ATOM_STATIC 0x82
#define ATOM_LISTBOX 0x83
#define ATOM_COMBO  0x85
#define D_LABEL(d, text, x, y, cx) dlg_item(d, NULL, ATOM_STATIC, text, 0xFFFF, SS_LEFT | SS_NOPREFIX, x, y, cx, 8)
#define D_VALUE(d, id, x, y, cx) dlg_item(d, NULL, ATOM_STATIC, L"", id, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, x, y, cx, 8)
#define D_BUTTON(d, text, id, x, y, cx) dlg_item(d, NULL, ATOM_BUTTON, text, id, BS_PUSHBUTTON | WS_TABSTOP, x, y, cx, 14)
#define D_GROUP(d, text, x, y, cx, cy) dlg_item(d, NULL, ATOM_BUTTON, text, 0xFFFF, BS_GROUPBOX, x, y, cx, cy)
#define D_EDITRO(d, id, x, y, cx, cy, extra) dlg_item(d, NULL, ATOM_EDIT, L"", id, ES_READONLY | WS_TABSTOP | (extra), x, y, cx, cy)

/* ---- the consoles ------------------------------------------------------------ */
node_t *services_create(node_t *parent);    /* Services (Local) + Stained Glass system services */
node_t *events_create(node_t *parent);      /* Event Viewer */
node_t *devices_create(node_t *parent);     /* Device Manager */
node_t *disks_create(node_t *parent);       /* Disk Management */
node_t *users_create(node_t *parent);       /* Local Users and Groups */
node_t *shares_create(node_t *parent);      /* Shared Folders */
node_t *msinfo_create(void);                 /* System Information */
int msinfo_report(const WCHAR *path);        /* msinfo32 /report FILE */

#endif
