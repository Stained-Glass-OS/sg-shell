/* sg-mmc -- the console host: services.msc, eventvwr.msc, devmgmt.msc,
 * diskmgmt.msc, compmgmt.msc (and eventvwr.exe, as sg-eventvwr64.exe).
 *
 * Windows' MMC lays a console out as a menu bar and toolbar, the console tree
 * on the left, the result pane in the middle and the Actions pane on the right,
 * with a status bar. This is that frame, in our own code, and the snap-ins
 * (services.c, events.c, devices.c, disks.c, users.c, shares.c) fill it. There
 * is no MMC snap-in COM: a .msc file only names the console (wine-sg 0142
 * writes ours with a StainedGlass element; any .msc is also recognised by
 * its file name).
 *
 * Which console: the program's own name (sg-eventvwr = Event Viewer), else the
 * first argument -- a .msc path or a console name (services, eventvwr,
 * devmgmt, diskmgmt, compmgmt).
 *
 * SG_MMC_DUMP=<file> writes what the window shows after every change (the
 * tree, the list's rows and their screen positions, the verbs and where their
 * toolbar buttons and Actions links are, the banner, the last message) for
 * the gates.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "mmc.h"
#include <stdarg.h>
#include <uxtheme.h>

int g_dpi = 96;
HINSTANCE g_inst;
HFONT g_font, g_font_bold, g_font_head;
HWND g_main;
HIMAGELIST g_icons, g_icons32;

static HWND g_tree, g_list, g_toolbar, g_status, g_actions, g_banner, g_empty;
static node_t g_roots;              /* a sentinel: its children are the roots */
static node_t *g_cur;
static node_t *g_initial;
static WCHAR g_console[32] = L"";   /* services, eventvwr, ... */
static WCHAR g_title[128] = L"Console Root";
static BOOL g_show_tree = TRUE, g_show_actions = TRUE, g_show_toolbar = TRUE;
static int g_tree_w = 270, g_actions_w = 210;
static RECT g_center, g_list_rc;
static WCHAR g_banner_text[512];
static WCHAR g_last_msg[1024];
static WCHAR g_empty_text[256];
static BOOL g_custom_list_rc;
static int g_sort_col = 0;
static BOOL g_sort_desc;
static BOOL g_numeric[MAX_COLS];
static int g_ncols;
static node_t *g_history[64];
static int g_hist_n, g_hist_pos = -1;
static BOOL g_hist_nav;
static WCHAR g_dump_path[MAX_PATH];

/* verbs of the selected row and of the node, as last queried */
static verbs_t g_row_verbs, g_node_verbs;
static BOOL g_have_row;
static LPARAM g_row_key;
static BOOL g_custom_have;      /* a custom view's own selection */
static LPARAM g_custom_key;
static WCHAR g_custom_name[128];

enum
{
    ID_TREE = 100, ID_LIST, ID_TOOLBAR, ID_STATUS, ID_ACTIONS, ID_BANNER,
    TB_BACK = 200, TB_FORWARD, TB_UP, TB_TREE, TB_PROPS, TB_REFRESH, TB_EXPORT, TB_HELP,
    TB_VERB = 300,                  /* + index into g_row_verbs, row verbs with icons */
    TB_NODEVERB = 340,              /* + index into g_node_verbs */
    CMD_EXIT = 400, CMD_SHOWTREE, CMD_SHOWACTIONS, CMD_ABOUT, CMD_REFRESH, CMD_PROPS, CMD_EXPORT, CMD_HELP,
    CMD_ROWVERB = 500,              /* + index */
    CMD_NODEVERB = 600,             /* + index */
    TIMER_TICK = 1, TIMER_DUMP,
};
#define FIXED_BUTTONS 11

/* ---- nodes -------------------------------------------------------------------------------- */

node_t *node_add(node_t *parent, const WCHAR *title, int icon, const snapin_t *ops, void *data)
{
    node_t *n = calloc(1, sizeof(*n)), **pp;
    if (!n) return NULL;
    if (!parent) parent = &g_roots;
    lstrcpynW(n->title, title, ARRAY_SIZE(n->title));
    n->icon = icon;
    n->ops = ops;
    n->data = data;
    n->parent = parent;
    for (pp = &parent->child; *pp; pp = &(*pp)->next) ;
    *pp = n;
    return n;
}

void node_free_children(node_t *n)
{
    node_t *c = n->child, *next;
    while (c)
    {
        next = c->next;
        node_free_children(c);
        if (c->hti && g_tree) TreeView_DeleteItem(g_tree, c->hti);
        free(c);
        c = next;
    }
    n->child = NULL;
}

node_t *node_current(void) { return g_cur; }

static void tree_insert(node_t *n)
{
    TVINSERTSTRUCTW is = { 0 };
    node_t *c;
    is.hParent = n->parent && n->parent != &g_roots ? n->parent->hti : TVI_ROOT;
    is.hInsertAfter = TVI_LAST;
    is.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_CHILDREN;
    is.item.pszText = n->title;
    is.item.iImage = is.item.iSelectedImage = n->icon;
    is.item.lParam = (LPARAM)n;
    is.item.cChildren = n->child || (n->ops && n->ops->expand && !n->expanded) ? 1 : 0;
    n->hti = (HTREEITEM)SendMessageW(g_tree, TVM_INSERTITEMW, 0, (LPARAM)&is);
    for (c = n->child; c; c = c->next) tree_insert(c);
}

static void node_expand(node_t *n)
{
    node_t *c;
    if (n->expanded) return;
    n->expanded = TRUE;
    if (n->ops && n->ops->expand)
    {
        n->ops->expand(n);
        for (c = n->child; c; c = c->next) if (!c->hti) tree_insert(c);
        if (!n->child && n->hti)
        {
            TVITEMW it = { TVIF_CHILDREN, n->hti };
            it.cChildren = 0;
            SendMessageW(g_tree, TVM_SETITEMW, 0, (LPARAM)&it);
        }
    }
}

void node_refresh_tree(node_t *n)
{
    node_t *c;
    for (c = n->child; c; c = c->next) if (!c->hti) tree_insert(c);
    if (n->hti)
    {
        TVITEMW it = { TVIF_CHILDREN | TVIF_TEXT, n->hti };
        it.cChildren = n->child ? 1 : 0;
        it.pszText = n->title;
        SendMessageW(g_tree, TVM_SETITEMW, 0, (LPARAM)&it);
    }
}

static void layout(void);

void node_select(node_t *n)
{
    if (!n) return;
    if (n->hti && TreeView_GetSelection(g_tree) != n->hti)
    {
        TreeView_SelectItem(g_tree, n->hti);   /* comes back through TVN_SELCHANGED */
        return;
    }
    if (g_cur && g_cur != n && g_cur->ops && g_cur->ops->hide) g_cur->ops->hide(g_cur);
    g_cur = n;
    if (!g_hist_nav)
    {
        if (g_hist_pos < 0 || g_history[g_hist_pos] != n)
        {
            if (g_hist_pos + 1 >= (int)ARRAY_SIZE(g_history))
            {
                memmove(g_history, g_history + 1, sizeof(g_history) - sizeof(g_history[0]));
                g_hist_pos--;
            }
            g_history[++g_hist_pos] = n;
            g_hist_n = g_hist_pos + 1;
        }
    }
    node_expand(n);
    g_custom_have = FALSE;
    g_custom_list_rc = FALSE;
    n->custom = FALSE;
    frame_banner(NULL);
    pane_empty_text(NULL);
    ShowWindow(g_list, SW_SHOW);
    if (n->ops && n->ops->show) n->ops->show(n);
    else
    {
        /* a folder: its children as the result */
        static const WCHAR *const cols[] = { L"Name", L"Description" };
        static const int widths[] = { 260, 400 };
        node_t *c;
        pane_columns(cols, widths, 2);
        pane_sort(-1, FALSE);       /* the console's own order */
        pane_begin();
        for (c = n->child; c; c = c->next)
        {
            const WCHAR *cells[2] = { c->title, c->desc };
            pane_add((LPARAM)c, c->icon, cells);
        }
        pane_end();
    }
    layout();
    frame_update_verbs();
    SetWindowTextW(g_main, g_title);
    frame_dump_later();
}

/* ---- the result list ---------------------------------------------------------------------- */

HWND pane_list(void) { return g_list; }

void pane_columns(const WCHAR *const *names, const int *widths, int n)
{
    LVCOLUMNW c = { 0 };
    int i;
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
    while (ListView_DeleteColumn(g_list, 0)) ;
    for (i = 0; i < n && i < MAX_COLS; i++)
    {
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = (WCHAR *)names[i];
        c.cx = S(widths[i]);
        c.iSubItem = i;
        SendMessageW(g_list, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
        g_numeric[i] = FALSE;
    }
    g_ncols = n;
    g_sort_col = 0;
    g_sort_desc = FALSE;
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
}

void pane_numeric(int col) { if (col >= 0 && col < MAX_COLS) g_numeric[col] = TRUE; }

static LPARAM g_keep_key;
static BOOL g_keep_have;
static int g_keep_top;

void pane_begin(void)
{
    g_keep_have = pane_selected(&g_keep_key);
    g_keep_top = ListView_GetTopIndex(g_list);
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);
}

int pane_add(LPARAM key, int icon, const WCHAR *const *cells)
{
    LVITEMW it = { 0 };
    int i, idx;
    it.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
    it.iItem = ListView_GetItemCount(g_list);
    it.pszText = (WCHAR *)(cells[0] ? cells[0] : L"");
    it.iImage = icon;
    it.lParam = key;
    idx = (int)SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
    for (i = 1; i < g_ncols; i++)
    {
        LVITEMW s = { 0 };
        s.iSubItem = i;
        s.pszText = (WCHAR *)(cells[i] ? cells[i] : L"");
        SendMessageW(g_list, LVM_SETITEMTEXTW, idx, (LPARAM)&s);
    }
    return idx;
}

static int find_key(LPARAM key)
{
    LVFINDINFOW fi = { LVFI_PARAM };
    fi.lParam = key;
    return (int)SendMessageW(g_list, LVM_FINDITEMW, -1, (LPARAM)&fi);
}

void pane_set(LPARAM key, int col, const WCHAR *text)
{
    int i = find_key(key);
    LVITEMW s = { 0 };
    if (i < 0) return;
    s.iSubItem = col;
    s.pszText = (WCHAR *)text;
    SendMessageW(g_list, LVM_SETITEMTEXTW, i, (LPARAM)&s);
}

void pane_set_icon(LPARAM key, int icon)
{
    LVITEMW it = { LVIF_IMAGE };
    it.iItem = find_key(key);
    if (it.iItem < 0) return;
    it.iImage = icon;
    SendMessageW(g_list, LVM_SETITEMW, 0, (LPARAM)&it);
}

static int CALLBACK compare_rows(LPARAM a, LPARAM b, LPARAM unused)
{
    (void)unused;
    WCHAR ta[512], tb[512];
    LVITEMW it = { 0 };
    int r;
    it.iSubItem = g_sort_col;
    it.cchTextMax = 512;
    it.pszText = ta;
    SendMessageW(g_list, LVM_GETITEMTEXTW, a, (LPARAM)&it);
    it.pszText = tb;
    SendMessageW(g_list, LVM_GETITEMTEXTW, b, (LPARAM)&it);
    if (g_numeric[g_sort_col])
    {
        double x = wcstod(ta, NULL), y = wcstod(tb, NULL);
        r = x < y ? -1 : x > y ? 1 : 0;
    }
    else r = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS, ta, -1, tb, -1) - 2;
    return g_sort_desc ? -r : r;
}

static void sort_arrows(void)
{
    HWND head = ListView_GetHeader(g_list);
    int i;
    for (i = 0; i < g_ncols; i++)
    {
        HDITEMW h = { HDI_FORMAT };
        SendMessageW(head, HDM_GETITEMW, i, (LPARAM)&h);
        h.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
        if (i == g_sort_col && g_sort_col >= 0) h.fmt |= g_sort_desc ? HDF_SORTDOWN : HDF_SORTUP;
        SendMessageW(head, HDM_SETITEMW, i, (LPARAM)&h);
    }
}

void pane_sort(int col, BOOL desc)
{
    g_sort_col = col;
    g_sort_desc = desc;
    if (col >= 0) SendMessageW(g_list, LVM_SORTITEMSEX, 0, (LPARAM)compare_rows);
    sort_arrows();
}

void pane_end(void)
{
    int i;
    if (g_sort_col >= 0) SendMessageW(g_list, LVM_SORTITEMSEX, 0, (LPARAM)compare_rows);
    sort_arrows();
    if (g_keep_have && (i = find_key(g_keep_key)) >= 0)
    {
        ListView_SetItemState(g_list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (g_keep_top > 0 && g_keep_top < ListView_GetItemCount(g_list))
    {
        ListView_EnsureVisible(g_list, ListView_GetItemCount(g_list) - 1, FALSE);
        ListView_EnsureVisible(g_list, g_keep_top, FALSE);
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, NULL, TRUE);
    ShowWindow(g_empty, ListView_GetItemCount(g_list) || !g_empty_text[0] || !IsWindowVisible(g_list) ? SW_HIDE : SW_SHOW);
    frame_dump_later();
}

BOOL pane_selected(LPARAM *key)
{
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    LVITEMW it = { LVIF_PARAM };
    if (i < 0) return FALSE;
    it.iItem = i;
    SendMessageW(g_list, LVM_GETITEMW, 0, (LPARAM)&it);
    *key = it.lParam;
    return TRUE;
}

BOOL pane_select_key(LPARAM key)
{
    int i = find_key(key);
    if (i < 0) return FALSE;
    ListView_SetItemState(g_list, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(g_list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(g_list, i, FALSE);
    return TRUE;
}

void pane_empty_text(const WCHAR *text)
{
    lstrcpynW(g_empty_text, text ? text : L"", ARRAY_SIZE(g_empty_text));
    SetWindowTextW(g_empty, g_empty_text);
    ShowWindow(g_empty, g_empty_text[0] && IsWindowVisible(g_list) && !ListView_GetItemCount(g_list) ? SW_SHOW : SW_HIDE);
}

void pane_show_list(BOOL show)
{
    ShowWindow(g_list, show ? SW_SHOW : SW_HIDE);
    if (!show) ShowWindow(g_empty, SW_HIDE);
    if (g_cur) g_cur->custom = !show;
}

void pane_list_rect(RECT *rc) { *rc = g_center; }

void pane_set_list_rect(const RECT *rc)
{
    g_list_rc = *rc;
    g_custom_list_rc = TRUE;
    MoveWindow(g_list, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, TRUE);
    MoveWindow(g_empty, rc->left + S(1), rc->top + S(28), rc->right - rc->left - S(2), S(40), TRUE);
}

/* ---- status, banner, messages ----------------------------------------------------------- */

void frame_status(const WCHAR *fmt, ...)
{
    WCHAR buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, ARRAY_SIZE(buf), fmt, ap);
    va_end(ap);
    buf[ARRAY_SIZE(buf) - 1] = 0;
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)buf);
    frame_dump_later();
}

void frame_banner(const WCHAR *text)
{
    lstrcpynW(g_banner_text, text ? text : L"", ARRAY_SIZE(g_banner_text));
    InvalidateRect(g_banner, NULL, TRUE);
    layout();
    frame_dump_later();
}

int frame_message(UINT flags, const WCHAR *title, const WCHAR *fmt, ...)
{
    va_list ap;
    int r;
    va_start(ap, fmt);
    _vsnwprintf(g_last_msg, ARRAY_SIZE(g_last_msg), fmt, ap);
    va_end(ap);
    g_last_msg[ARRAY_SIZE(g_last_msg) - 1] = 0;
    frame_dump();
    r = MessageBoxW(g_main, g_last_msg, title ? title : g_title, flags);
    frame_dump_later();
    return r;
}

const WCHAR *console_name(void) { return g_console; }
void frame_set_initial(node_t *n) { g_initial = n; }

/* ---- verbs: toolbar, Action menu, Actions pane ------------------------------------------ */

typedef struct action_link
{
    RECT rc;
    int cmd;            /* CMD_* */
    WCHAR text[96];
    int icon;
    BOOL header, enabled;
} action_link_t;
static action_link_t g_links[64];
static int g_nlinks, g_link_hot = -1;

static void add_link(int cmd, const WCHAR *text, int icon, BOOL header, BOOL enabled)
{
    action_link_t *l;
    const WCHAR *s;
    WCHAR *d;
    if (g_nlinks >= (int)ARRAY_SIZE(g_links)) return;
    l = &g_links[g_nlinks++];
    memset(l, 0, sizeof(*l));
    l->cmd = cmd;
    l->icon = icon;
    l->header = header;
    l->enabled = enabled;
    for (s = text, d = l->text; *s && d < l->text + ARRAY_SIZE(l->text) - 1; s++)
        if (*s != '&') *d++ = *s;
    *d = 0;
}

static void build_links(void)
{
    WCHAR name[128] = L"";
    int i;
    g_nlinks = 0;
    if (!g_cur) return;
    add_link(0, g_cur->title, -1, TRUE, TRUE);
    for (i = 0; i < g_node_verbs.n; i++)
        add_link(CMD_NODEVERB + i, g_node_verbs.v[i].name, g_node_verbs.v[i].icon, FALSE, g_node_verbs.v[i].enabled);
    add_link(CMD_REFRESH, L"Refresh", IC_REFRESH, FALSE, TRUE);
    if (!g_cur->custom) add_link(CMD_EXPORT, L"Export List...", IC_EXPORT, FALSE, TRUE);
    add_link(CMD_HELP, L"Help", IC_HELP, FALSE, TRUE);
    if (g_have_row)
    {
        int i2 = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
        if (i2 >= 0) ListView_GetItemText(g_list, i2, 0, name, ARRAY_SIZE(name));
        if (g_cur->custom) lstrcpynW(name, g_custom_name[0] ? g_custom_name : L"Selected Item", ARRAY_SIZE(name));
        add_link(0, name, -1, TRUE, TRUE);
        for (i = 0; i < g_row_verbs.n; i++)
            add_link(CMD_ROWVERB + i, g_row_verbs.v[i].name, g_row_verbs.v[i].icon, FALSE, g_row_verbs.v[i].enabled);
        if (g_cur->ops && g_cur->ops->open) add_link(CMD_PROPS, L"Properties", IC_PROPS, FALSE, TRUE);
        add_link(CMD_HELP, L"Help", IC_HELP, FALSE, TRUE);
    }
    InvalidateRect(g_actions, NULL, TRUE);
}

static void rebuild_toolbar(void)
{
    int n = (int)SendMessageW(g_toolbar, TB_BUTTONCOUNT, 0, 0), i;
    TBBUTTON b;
    while (n > FIXED_BUTTONS) SendMessageW(g_toolbar, TB_DELETEBUTTON, --n, 0);
    SendMessageW(g_toolbar, TB_ENABLEBUTTON, TB_BACK, g_hist_pos > 0);
    SendMessageW(g_toolbar, TB_ENABLEBUTTON, TB_FORWARD, g_hist_pos + 1 < g_hist_n);
    SendMessageW(g_toolbar, TB_ENABLEBUTTON, TB_UP, g_cur && g_cur->parent && g_cur->parent != &g_roots);
    SendMessageW(g_toolbar, TB_ENABLEBUTTON, TB_PROPS, g_have_row && g_cur && g_cur->ops && g_cur->ops->open);
    SendMessageW(g_toolbar, TB_ENABLEBUTTON, TB_EXPORT, g_cur && !g_cur->custom);
    for (i = 0; i < g_node_verbs.n; i++)
    {
        if (g_node_verbs.v[i].icon < 0) continue;
        memset(&b, 0, sizeof(b));
        b.iBitmap = g_node_verbs.v[i].icon;
        b.idCommand = TB_NODEVERB + i;
        b.fsState = g_node_verbs.v[i].enabled ? TBSTATE_ENABLED : 0;
        b.fsStyle = BTNS_BUTTON;
        SendMessageW(g_toolbar, TB_ADDBUTTONSW, 1, (LPARAM)&b);
    }
    for (i = 0; i < g_row_verbs.n; i++)
    {
        if (g_row_verbs.v[i].icon < 0) continue;
        memset(&b, 0, sizeof(b));
        b.iBitmap = g_row_verbs.v[i].icon;
        b.idCommand = TB_VERB + i;
        b.fsState = g_row_verbs.v[i].enabled ? TBSTATE_ENABLED : 0;
        b.fsStyle = BTNS_BUTTON;
        SendMessageW(g_toolbar, TB_ADDBUTTONSW, 1, (LPARAM)&b);
    }
}

void frame_update_verbs(void)
{
    memset(&g_node_verbs, 0, sizeof(g_node_verbs));
    memset(&g_row_verbs, 0, sizeof(g_row_verbs));
    g_have_row = FALSE;
    if (g_cur && g_cur->ops && g_cur->ops->verbs)
    {
        g_cur->ops->verbs(g_cur, 0, FALSE, &g_node_verbs);
        if (!g_cur->custom) g_have_row = pane_selected(&g_row_key);
        else { g_have_row = g_custom_have; g_row_key = g_custom_key; }
        if (g_have_row) g_cur->ops->verbs(g_cur, g_row_key, TRUE, &g_row_verbs);
    }
    else if (g_cur && !g_cur->custom) g_have_row = pane_selected(&g_row_key);
    rebuild_toolbar();
    build_links();
    frame_dump_later();
}

/* a custom view reports its own selection: key and whether there is one */
void frame_custom_selection(LPARAM key, BOOL have, const WCHAR *name)
{
    g_custom_have = have;
    g_custom_key = key;
    lstrcpynW(g_custom_name, name ? name : L"", ARRAY_SIZE(g_custom_name));
    memset(&g_row_verbs, 0, sizeof(g_row_verbs));
    g_have_row = have;
    g_row_key = key;
    if (have && g_cur && g_cur->ops && g_cur->ops->verbs) g_cur->ops->verbs(g_cur, key, TRUE, &g_row_verbs);
    rebuild_toolbar();
    build_links();
    frame_dump_later();
}

static void export_list(void);

static void run_cmd(int cmd)
{
    if (!g_cur) return;
    if (cmd >= CMD_ROWVERB && cmd < CMD_ROWVERB + MAX_VERBS)
    {
        verb_t *v = &g_row_verbs.v[cmd - CMD_ROWVERB];
        if (cmd - CMD_ROWVERB < g_row_verbs.n && v->enabled && g_cur->ops->invoke)
            g_cur->ops->invoke(g_cur, g_row_key, TRUE, v->id);
    }
    else if (cmd >= CMD_NODEVERB && cmd < CMD_NODEVERB + MAX_VERBS)
    {
        verb_t *v = &g_node_verbs.v[cmd - CMD_NODEVERB];
        if (cmd - CMD_NODEVERB < g_node_verbs.n && v->enabled && g_cur->ops->invoke)
            g_cur->ops->invoke(g_cur, 0, FALSE, v->id);
    }
    else switch (cmd)
    {
    case CMD_REFRESH:
        node_select(g_cur);
        break;
    case CMD_PROPS:
        if (g_have_row && g_cur->ops && g_cur->ops->open) g_cur->ops->open(g_cur, g_row_key);
        break;
    case CMD_EXPORT:
        export_list();
        break;
    case CMD_HELP:
    case CMD_ABOUT:
        frame_message(MB_OK | MB_ICONINFORMATION, L"About",
                      L"%ls\n\nStained Glass OS administrative console.\n"
                      L"The console tree is on the left, what the selected item holds in the middle, "
                      L"and what you can do with it in the Actions pane on the right.", g_title);
        break;
    }
    frame_update_verbs();
}

/* Action > Export List...: the list as tab-separated text, as MMC writes it */
static void export_list(void)
{
    WCHAR path[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    HANDLE f;
    int rows = ListView_GetItemCount(g_list), r, c;
    DWORD w;

    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"Text (Tab Delimited) (*.txt)\0*.txt\0Text (Comma Delimited) (*.csv)\0*.csv\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { frame_message(MB_ICONERROR, NULL, L"The file could not be written."); return; }
    WriteFile(f, "\xFF\xFE", 2, &w, NULL);
    for (r = -1; r < rows; r++)
    {
        for (c = 0; c < g_ncols; c++)
        {
            WCHAR t[512] = L"";
            const WCHAR *sep = c + 1 < g_ncols ? (ofn.nFilterIndex == 2 ? L"," : L"\t") : L"\r\n";
            if (r < 0)
            {
                LVCOLUMNW col = { LVCF_TEXT };
                col.pszText = t;
                col.cchTextMax = 512;
                SendMessageW(g_list, LVM_GETCOLUMNW, c, (LPARAM)&col);
            }
            else ListView_GetItemText(g_list, r, c, t, 512);
            WriteFile(f, t, (DWORD)(wcslen(t) * sizeof(WCHAR)), &w, NULL);
            WriteFile(f, sep, (DWORD)(wcslen(sep) * sizeof(WCHAR)), &w, NULL);
        }
    }
    CloseHandle(f);
}

static HMENU build_action_menu(HMENU m, BOOL for_row)
{
    int i;
    while (GetMenuItemCount(m) > 0) DeleteMenu(m, 0, MF_BYPOSITION);
    if (for_row && g_have_row)
    {
        for (i = 0; i < g_row_verbs.n; i++)
        {
            if (g_row_verbs.v[i].separator_before && i) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING | (g_row_verbs.v[i].enabled ? 0 : MF_GRAYED), CMD_ROWVERB + i, g_row_verbs.v[i].name);
        }
        if (g_row_verbs.n) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, CMD_REFRESH, L"Re&fresh");
        if (g_cur && g_cur->ops && g_cur->ops->open)
        {
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, CMD_PROPS, L"P&roperties");
            SetMenuDefaultItem(m, CMD_PROPS, FALSE);
        }
    }
    else
    {
        for (i = 0; i < g_node_verbs.n; i++)
        {
            if (g_node_verbs.v[i].separator_before && i) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING | (g_node_verbs.v[i].enabled ? 0 : MF_GRAYED), CMD_NODEVERB + i, g_node_verbs.v[i].name);
        }
        if (g_node_verbs.n) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, CMD_REFRESH, L"Re&fresh");
        if (g_cur && !g_cur->custom) AppendMenuW(m, MF_STRING, CMD_EXPORT, L"Export &List...");
        if (g_have_row && g_cur && g_cur->ops && g_cur->ops->open)
        {
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, CMD_PROPS, L"P&roperties");
        }
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_HELP, L"&Help");
    return m;
}

/* ---- the Actions pane (our own drawing) --------------------------------------------------- */

static LRESULT CALLBACK actions_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc, r;
        int i, y = 0, lh = S(24);
        HBRUSH bg = CreateSolidBrush(C_BG), sf = CreateSolidBrush(C_SURFACE), ln = CreateSolidBrush(C_LINE);
        GetClientRect(h, &rc);
        FillRect(dc, &rc, bg);
        r = rc; r.right = r.left + 1;
        FillRect(dc, &r, ln);
        SetBkMode(dc, TRANSPARENT);
        r = rc; r.left += 1; r.bottom = r.top + lh;
        FillRect(dc, &r, sf);
        SelectObject(dc, g_font_bold);
        SetTextColor(dc, C_HEAD);
        r.left += S(10);
        DrawTextW(dc, L"Actions", -1, &r, DT_SINGLELINE | DT_VCENTER);
        y = lh + S(4);
        for (i = 0; i < g_nlinks; i++)
        {
            action_link_t *l = &g_links[i];
            if (l->header)
            {
                if (i) y += S(6);
                SetRect(&l->rc, 1, y, rc.right, y + lh);
                r = l->rc;
                r.bottom -= 1;
                FillRect(dc, &r, sf);
                SelectObject(dc, g_font_bold);
                SetTextColor(dc, C_TEXT);
                r.left += S(10);
                r.right -= S(6);
                DrawTextW(dc, l->text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                y += lh;
                continue;
            }
            SetRect(&l->rc, 1, y, rc.right, y + S(22));
            if (i == g_link_hot && l->enabled)
            {
                HBRUSH hb = CreateSolidBrush(C_HOVER);
                FillRect(dc, &l->rc, hb);
                DeleteObject(hb);
            }
            if (l->icon >= 0)
                ImageList_Draw(g_icons, l->icon, dc, S(12), y + (S(22) - S(16)) / 2, l->enabled ? ILD_NORMAL : ILD_BLEND50);
            r = l->rc;
            r.left = S(34);
            r.right -= S(6);
            SelectObject(dc, g_font);
            SetTextColor(dc, l->enabled ? (i == g_link_hot ? C_ACCENT : C_TEXT) : C_SUBTLE);
            DrawTextW(dc, l->text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            y += S(22);
        }
        DeleteObject(bg); DeleteObject(sf); DeleteObject(ln);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int i, hot = -1;
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        for (i = 0; i < g_nlinks; i++)
            if (!g_links[i].header && PtInRect(&g_links[i].rc, pt)) hot = i;
        if (hot != g_link_hot) { g_link_hot = hot; InvalidateRect(h, NULL, FALSE); }
        TrackMouseEvent(&tme);
        SetCursor(LoadCursorW(NULL, hot >= 0 && g_links[hot].enabled ? (LPCWSTR)IDC_HAND : (LPCWSTR)IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        g_link_hot = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int i;
        for (i = 0; i < g_nlinks; i++)
            if (!g_links[i].header && g_links[i].enabled && PtInRect(&g_links[i].rc, pt))
            {
                run_cmd(g_links[i].cmd);
                break;
            }
        return 0;
    }
    case WM_SETCURSOR:
        return TRUE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK banner_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        HBRUSH b = CreateSolidBrush(C_WARN_BG);
        GetClientRect(h, &rc);
        FillRect(dc, &rc, b);
        DeleteObject(b);
        ImageList_Draw(g_icons, IC_WARNING, dc, S(8), (rc.bottom - S(16)) / 2, ILD_NORMAL);
        rc.left += S(32);
        rc.right -= S(8);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_font);
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, g_banner_text, -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- layout ---------------------------------------------------------------------------------- */

static void layout(void)
{
    RECT rc, tb, sb;
    int top, bottom, left, right, bh = g_banner_text[0] ? S(30) : 0;
    if (!g_main || !g_toolbar) return;
    GetClientRect(g_main, &rc);
    SendMessageW(g_toolbar, TB_AUTOSIZE, 0, 0);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    GetWindowRect(g_toolbar, &tb);
    GetWindowRect(g_status, &sb);
    ShowWindow(g_toolbar, g_show_toolbar ? SW_SHOW : SW_HIDE);
    top = g_show_toolbar ? tb.bottom - tb.top : 0;
    bottom = rc.bottom - (sb.bottom - sb.top);
    left = g_show_tree ? S(g_tree_w) : 0;
    right = g_show_actions ? rc.right - S(g_actions_w) : rc.right;
    ShowWindow(g_tree, g_show_tree ? SW_SHOW : SW_HIDE);
    MoveWindow(g_tree, 0, top, left ? left - S(4) : 0, bottom - top, TRUE);
    ShowWindow(g_actions, g_show_actions ? SW_SHOW : SW_HIDE);
    MoveWindow(g_actions, right, top, rc.right - right, bottom - top, TRUE);
    ShowWindow(g_banner, bh ? SW_SHOW : SW_HIDE);
    MoveWindow(g_banner, left, top, right - left, bh, TRUE);
    SetRect(&g_center, left, top + bh, right, bottom);
    if (!g_custom_list_rc || (g_cur && !(g_cur->ops && g_cur->ops->layout)))
    {
        MoveWindow(g_list, g_center.left, g_center.top, g_center.right - g_center.left, g_center.bottom - g_center.top, TRUE);
        MoveWindow(g_empty, g_center.left + S(1), g_center.top + S(28), g_center.right - g_center.left - S(2), S(40), TRUE);
    }
    if (g_cur && g_cur->ops && g_cur->ops->layout) g_cur->ops->layout(g_cur, &g_center);
    InvalidateRect(g_main, NULL, FALSE);
}

/* ---- the dump ---------------------------------------------------------------------------- */

static void dump_tree(FILE *f, node_t *n, int depth)
{
    for (; n; n = n->next)
    {
        RECT r;
        POINT pt = { -1, -1 };
        memcpy(&r, &n->hti, sizeof(n->hti));
        if (n->hti && IsWindowVisible(g_tree) && SendMessageW(g_tree, TVM_GETITEMRECT, TRUE, (LPARAM)&r))
            screen_center(g_tree, &r, &pt);
        fprintf(f, "TREE %d %ld %ld %s%ls\n", depth, pt.x, pt.y, n == g_cur ? "* " : "", n->title);
        if (n->child) dump_tree(f, n->child, depth + 1);
    }
}

static void dump_verbs(FILE *f, const char *kind, verbs_t *vs, int tb_base, int cmd_base)
{
    int i, j;
    for (i = 0; i < vs->n; i++)
    {
        POINT tb = { -1, -1 }, ac = { -1, -1 };
        RECT r;
        int idx = (int)SendMessageW(g_toolbar, TB_COMMANDTOINDEX, tb_base + i, 0);
        if (vs->v[i].icon >= 0 && idx >= 0 && SendMessageW(g_toolbar, TB_GETITEMRECT, idx, (LPARAM)&r))
            screen_center(g_toolbar, &r, &tb);
        for (j = 0; j < g_nlinks; j++)
            if (g_links[j].cmd == cmd_base + i && g_show_actions) screen_center(g_actions, &g_links[j].rc, &ac);
        fprintf(f, "VERB %s %d %d %ld %ld %ld %ld %ls\n", kind, vs->v[i].id, vs->v[i].enabled,
                tb.x, tb.y, ac.x, ac.y, vs->v[i].name);
    }
}

void frame_dump(void)
{
    FILE *f;
    int i, c, rows;
    WCHAR tmp[MAX_PATH + 8];
    RECT r;
    POINT pt;
    static const struct { int id; const char *name; } fixed[] = {
        { TB_BACK, "back" }, { TB_FORWARD, "forward" }, { TB_UP, "up" }, { TB_TREE, "tree" },
        { TB_PROPS, "properties" }, { TB_REFRESH, "refresh" }, { TB_EXPORT, "export" }, { TB_HELP, "help" } };

    if (!g_dump_path[0]) return;
    _snwprintf(tmp, ARRAY_SIZE(tmp), L"%ls.tmp", g_dump_path);
    if (!(f = _wfopen(tmp, L"wb"))) return;
    GetWindowRect(g_main, &r);
    fprintf(f, "TITLE %ls\nCONSOLE %ls\nWINDOW %ld %ld %ld %ld\n", g_title, g_console, r.left, r.top, r.right, r.bottom);
    fprintf(f, "ADMIN %d\nBRIDGED %d\n", is_admin(), sys_bridged());
    fprintf(f, "NODE %ls\n", g_cur ? g_cur->title : L"");
    fprintf(f, "CUSTOM %d\n", g_cur ? g_cur->custom : 0);
    dump_tree(f, g_roots.child, 0);
    for (i = 0; i < (int)ARRAY_SIZE(fixed); i++)
    {
        int idx = (int)SendMessageW(g_toolbar, TB_COMMANDTOINDEX, fixed[i].id, 0);
        pt.x = pt.y = -1;
        if (idx >= 0 && SendMessageW(g_toolbar, TB_GETITEMRECT, idx, (LPARAM)&r)) screen_center(g_toolbar, &r, &pt);
        fprintf(f, "BUTTON %s %d %ld %ld\n", fixed[i].name,
                (int)SendMessageW(g_toolbar, TB_ISBUTTONENABLED, fixed[i].id, 0), pt.x, pt.y);
    }
    if (IsWindowVisible(g_list))
    {
        for (c = 0; c < g_ncols; c++)
        {
            WCHAR t[128] = L"";
            LVCOLUMNW col = { LVCF_TEXT };
            col.pszText = t;
            col.cchTextMax = 128;
            SendMessageW(g_list, LVM_GETCOLUMNW, c, (LPARAM)&col);
            fprintf(f, "COL %d %ls\n", c, t);
        }
        rows = ListView_GetItemCount(g_list);
        fprintf(f, "ROWS %d\n", rows);
        for (i = 0; i < rows && i < 5000; i++)
        {
            LVITEMW it = { LVIF_PARAM | LVIF_STATE };
            it.iItem = i;
            it.stateMask = LVIS_SELECTED;
            SendMessageW(g_list, LVM_GETITEMW, 0, (LPARAM)&it);
            pt.x = pt.y = -1;
            r.left = LVIR_LABEL;
            if (SendMessageW(g_list, LVM_GETITEMRECT, i, (LPARAM)&r))
            {
                RECT cl, hr = { 0 };
                GetClientRect(g_list, &cl);
                GetWindowRect(ListView_GetHeader(g_list), &hr);
                if (r.top >= hr.bottom - hr.top && r.bottom <= cl.bottom) screen_center(g_list, &r, &pt);
            }
            fprintf(f, "ROW %lld %ld %ld %d", (long long)it.lParam, pt.x, pt.y, (it.state & LVIS_SELECTED) ? 1 : 0);
            for (c = 0; c < g_ncols; c++)
            {
                WCHAR t[512] = L"";
                ListView_GetItemText(g_list, i, c, t, 512);
                fprintf(f, "\t%ls", t);
            }
            fputc('\n', f);
        }
    }
    fprintf(f, "EMPTY %ls\n", IsWindowVisible(g_empty) ? g_empty_text : L"");
    dump_verbs(f, "node", &g_node_verbs, TB_NODEVERB, CMD_NODEVERB);
    if (g_have_row) dump_verbs(f, "row", &g_row_verbs, TB_VERB, CMD_ROWVERB);
    for (i = 0; i < g_nlinks; i++)
    {
        pt.x = pt.y = -1;
        if (g_show_actions) screen_center(g_actions, &g_links[i].rc, &pt);
        fprintf(f, "LINK %d %ld %ld %ls\n", g_links[i].header, pt.x, pt.y, g_links[i].text);
    }
    fprintf(f, "BANNER %ls\n", g_banner_text);
    {
        WCHAR st[512] = L"";
        SendMessageW(g_status, SB_GETTEXTW, 0, (LPARAM)st);
        fprintf(f, "STATUS %ls\n", st);
    }
    {
        WCHAR m[1024], *p;
        lstrcpynW(m, g_last_msg, ARRAY_SIZE(m));
        for (p = m; *p; p++) if (*p == '\n') *p = '|'; else if (*p == '\r') *p = ' ';
        fprintf(f, "MSG %ls\n", m);
    }
    if (g_cur && g_cur->ops && g_cur->ops->dump) g_cur->ops->dump(g_cur, f);
    fprintf(f, "END\n");
    fclose(f);
    MoveFileExW(tmp, g_dump_path, MOVEFILE_REPLACE_EXISTING);
}

void frame_dump_later(void)
{
    if (g_dump_path[0] && g_main) SetTimer(g_main, TIMER_DUMP, 150, NULL);
}

/* ---- the main window ------------------------------------------------------------------------ */

static void create_toolbar(HWND h)
{
    TBBUTTON b[FIXED_BUTTONS];
    static const struct { int id, icon; } items[FIXED_BUTTONS] = {
        { TB_BACK, IC_BACK }, { TB_FORWARD, IC_FORWARD }, { 0, 0 }, { TB_UP, IC_UP }, { TB_TREE, IC_TREE },
        { 0, 0 }, { TB_PROPS, IC_PROPS }, { TB_REFRESH, IC_REFRESH }, { TB_EXPORT, IC_EXPORT }, { 0, 0 },
        { TB_HELP, IC_HELP } };
    int i;
    g_toolbar = CreateWindowExW(0, TOOLBARCLASSNAMEW, NULL,
                                WS_CHILD | WS_VISIBLE | TBSTYLE_FLAT | TBSTYLE_TOOLTIPS | CCS_NODIVIDER | CCS_TOP,
                                0, 0, 0, 0, h, (HMENU)ID_TOOLBAR, g_inst, NULL);
    SendMessageW(g_toolbar, TB_BUTTONSTRUCTSIZE, sizeof(TBBUTTON), 0);
    SendMessageW(g_toolbar, TB_SETIMAGELIST, 0, (LPARAM)g_icons);
    SendMessageW(g_toolbar, TB_SETEXTENDEDSTYLE, 0, TBSTYLE_EX_DOUBLEBUFFER);
    memset(b, 0, sizeof(b));
    for (i = 0; i < FIXED_BUTTONS; i++)
    {
        b[i].iBitmap = items[i].id ? items[i].icon : 0;
        b[i].idCommand = items[i].id;
        b[i].fsState = TBSTATE_ENABLED;
        b[i].fsStyle = items[i].id ? BTNS_BUTTON : BTNS_SEP;
    }
    SendMessageW(g_toolbar, TB_ADDBUTTONSW, FIXED_BUTTONS, (LPARAM)b);
}

static HMENU create_menu(void)
{
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), action = CreatePopupMenu(), view = CreatePopupMenu(),
          help = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, CMD_EXIT, L"E&xit");
    AppendMenuW(action, MF_STRING, CMD_REFRESH, L"Re&fresh");
    AppendMenuW(view, MF_STRING, CMD_SHOWTREE, L"Console &Tree");
    AppendMenuW(view, MF_STRING, CMD_SHOWACTIONS, L"&Action Pane");
    AppendMenuW(help, MF_STRING, CMD_ABOUT, L"&About");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)action, L"&Action");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    return bar;
}

static const WCHAR *tip_text(int id)
{
    switch (id)
    {
    case TB_BACK: return L"Back";
    case TB_FORWARD: return L"Forward";
    case TB_UP: return L"Up One Level";
    case TB_TREE: return L"Show/Hide Console Tree";
    case TB_PROPS: return L"Properties";
    case TB_REFRESH: return L"Refresh";
    case TB_EXPORT: return L"Export List";
    case TB_HELP: return L"Help";
    }
    if (id >= TB_VERB && id < TB_VERB + g_row_verbs.n) return g_row_verbs.v[id - TB_VERB].name;
    if (id >= TB_NODEVERB && id < TB_NODEVERB + g_node_verbs.n) return g_node_verbs.v[id - TB_NODEVERB].name;
    return L"";
}

static void go_history(int delta)
{
    int p = g_hist_pos + delta;
    if (p < 0 || p >= g_hist_n) return;
    g_hist_pos = p;
    g_hist_nav = TRUE;
    node_select(g_history[p]);
    g_hist_nav = FALSE;
}

static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SIZE:
        layout();
        frame_dump_later();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(520);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = S(360);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_DUMP) { KillTimer(h, TIMER_DUMP); frame_dump(); }
        else if (wp == TIMER_TICK && g_cur && g_cur->ops && g_cur->ops->tick) g_cur->ops->tick(g_cur);
        return 0;
    case WM_INITMENUPOPUP:
        if (LOWORD(lp) == 1) build_action_menu((HMENU)wp, GetFocus() == g_list);
        else if (LOWORD(lp) == 2)
        {
            CheckMenuItem((HMENU)wp, CMD_SHOWTREE, g_show_tree ? MF_CHECKED : MF_UNCHECKED);
            CheckMenuItem((HMENU)wp, CMD_SHOWACTIONS, g_show_actions ? MF_CHECKED : MF_UNCHECKED);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case CMD_EXIT: DestroyWindow(h); return 0;
        case TB_BACK: go_history(-1); return 0;
        case TB_FORWARD: go_history(1); return 0;
        case TB_UP: if (g_cur && g_cur->parent && g_cur->parent != &g_roots) node_select(g_cur->parent); return 0;
        case TB_TREE: case CMD_SHOWTREE: g_show_tree = !g_show_tree; layout(); frame_dump_later(); return 0;
        case CMD_SHOWACTIONS: g_show_actions = !g_show_actions; layout(); frame_dump_later(); return 0;
        case TB_PROPS: run_cmd(CMD_PROPS); return 0;
        case TB_REFRESH: run_cmd(CMD_REFRESH); return 0;
        case TB_EXPORT: run_cmd(CMD_EXPORT); return 0;
        case TB_HELP: run_cmd(CMD_HELP); return 0;
        }
        if (LOWORD(wp) >= TB_VERB && LOWORD(wp) < TB_VERB + MAX_VERBS) { run_cmd(CMD_ROWVERB + LOWORD(wp) - TB_VERB); return 0; }
        if (LOWORD(wp) >= TB_NODEVERB && LOWORD(wp) < TB_NODEVERB + MAX_VERBS) { run_cmd(CMD_NODEVERB + LOWORD(wp) - TB_NODEVERB); return 0; }
        if (LOWORD(wp) >= CMD_REFRESH && LOWORD(wp) < CMD_NODEVERB + MAX_VERBS) { run_cmd(LOWORD(wp)); return 0; }
        break;
    case WM_NOTIFY:
    {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->code == TTN_GETDISPINFOW)
        {
            NMTTDISPINFOW *di = (NMTTDISPINFOW *)lp;
            lstrcpynW(di->szText, tip_text((int)nh->idFrom), ARRAY_SIZE(di->szText));
            return 0;
        }
        if (nh->idFrom == ID_TREE)
        {
            if (nh->code == TVN_SELCHANGEDW)
            {
                NMTREEVIEWW *tv = (NMTREEVIEWW *)lp;
                if (tv->itemNew.lParam) node_select((node_t *)tv->itemNew.lParam);
            }
            else if (nh->code == TVN_ITEMEXPANDEDW) frame_dump_later();
            else if (nh->code == TVN_ITEMEXPANDINGW)
            {
                NMTREEVIEWW *tv = (NMTREEVIEWW *)lp;
                if (tv->itemNew.lParam) node_expand((node_t *)tv->itemNew.lParam);
            }
            else if (nh->code == NM_RCLICK)
            {
                TVHITTESTINFO ht = { 0 };
                HMENU m;
                int cmd;
                GetCursorPos(&ht.pt);
                ScreenToClient(g_tree, &ht.pt);
                if (SendMessageW(g_tree, TVM_HITTEST, 0, (LPARAM)&ht) && ht.hItem) TreeView_SelectItem(g_tree, ht.hItem);
                m = build_action_menu(CreatePopupMenu(), FALSE);
                GetCursorPos(&ht.pt);
                cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, ht.pt.x, ht.pt.y, 0, h, NULL);
                DestroyMenu(m);
                if (cmd) run_cmd(cmd);
                return TRUE;
            }
        }
        else if (nh->idFrom == ID_LIST)
        {
            if (nh->code == LVN_ITEMCHANGED)
            {
                NMLISTVIEW *lv = (NMLISTVIEW *)lp;
                if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_SELECTED))
                {
                    LPARAM key;
                    BOOL have = pane_selected(&key);
                    frame_update_verbs();
                    if (g_cur && g_cur->ops && g_cur->ops->selchange) g_cur->ops->selchange(g_cur, key, have);
                }
            }
            else if (nh->code == LVN_COLUMNCLICK)
            {
                NMLISTVIEW *lv = (NMLISTVIEW *)lp;
                pane_sort(lv->iSubItem, lv->iSubItem == g_sort_col ? !g_sort_desc : FALSE);
                frame_dump_later();
            }
            else if (nh->code == NM_DBLCLK || (nh->code == LVN_KEYDOWN && ((NMLVKEYDOWN *)lp)->wVKey == VK_RETURN))
            {
                LPARAM key;
                if (pane_selected(&key) && g_cur)
                {
                    node_t *c;
                    /* a folder's row is a child node: open it */
                    for (c = g_cur->child; c; c = c->next) if ((LPARAM)c == key) { node_select(c); return 0; }
                    if (g_cur->ops && g_cur->ops->open) g_cur->ops->open(g_cur, key);
                }
            }
            else if (nh->code == NM_RCLICK)
            {
                POINT pt;
                HMENU m;
                int cmd;
                frame_update_verbs();
                m = build_action_menu(CreatePopupMenu(), TRUE);
                GetCursorPos(&pt);
                cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, NULL);
                DestroyMenu(m);
                if (cmd) run_cmd(cmd);
                return TRUE;
            }
        }
        break;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- the consoles ----------------------------------------------------------------------------- */

static void pick_console(const WCHAR *cmdline)
{
    WCHAR self[MAX_PATH], *base, arg[MAX_PATH] = L"", *p;
    int argc, i;
    WCHAR **argv = CommandLineToArgvW(cmdline, &argc);

    GetModuleFileNameW(NULL, self, MAX_PATH);
    base = wcsrchr(self, '\\') ? wcsrchr(self, '\\') + 1 : self;
    if (!_wcsnicmp(base, L"sg-eventvwr", 11)) lstrcpyW(g_console, L"eventvwr");
    if (!_wcsnicmp(base, L"sg-msinfo32", 11)) lstrcpyW(g_console, L"msinfo32");
    for (i = 1; argv && i < argc && !g_console[0]; i++)
    {
        if (argv[i][0] == '-' || argv[i][0] == '/') continue;
        lstrcpynW(arg, argv[i], MAX_PATH);
        p = wcsrchr(arg, '\\') ? wcsrchr(arg, '\\') + 1 : arg;
        if (wcsrchr(p, '/')) p = wcsrchr(p, '/') + 1;
        if (wcslen(p) > 4 && !_wcsicmp(p + wcslen(p) - 4, L".msc"))
        {
            /* ours name the console inside; any other by its file name */
            HANDLE f = CreateFileW(arg, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            char buf[2048] = "";
            DWORD got = 0;
            if (f != INVALID_HANDLE_VALUE)
            {
                const char *c;
                ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
                CloseHandle(f);
                buf[got] = 0;
                if ((c = strstr(buf, "<StainedGlass Console=\"")))
                {
                    c += 23;
                    for (i = 0; c[i] && c[i] != '"' && i < 31; i++) g_console[i] = c[i];
                    g_console[i] = 0;
                    break;
                }
            }
            p[wcslen(p) - 4] = 0;
        }
        lstrcpynW(g_console, p, ARRAY_SIZE(g_console));
    }
    if (argv) LocalFree(argv);
    for (p = g_console; *p; p++) *p = towlower(*p);
}

static void build_console(void)
{
    node_t *root, *tools, *storage, *apps;
    if (!wcscmp(g_console, L"services"))
    {
        lstrcpyW(g_title, L"Services");
        services_create(NULL);
    }
    else if (!wcscmp(g_console, L"eventvwr"))
    {
        lstrcpyW(g_title, L"Event Viewer");
        events_create(NULL);
    }
    else if (!wcscmp(g_console, L"devmgmt"))
    {
        lstrcpyW(g_title, L"Device Manager");
        devices_create(NULL);
        g_show_tree = FALSE;
    }
    else if (!wcscmp(g_console, L"diskmgmt"))
    {
        lstrcpyW(g_title, L"Disk Management");
        disks_create(NULL);
        g_show_tree = FALSE;
    }
    else if (!wcscmp(g_console, L"lusrmgr"))
    {
        lstrcpyW(g_title, L"Local Users and Groups");
        users_create(NULL);
    }
    else if (!wcscmp(g_console, L"msinfo32"))
    {
        lstrcpyW(g_title, L"System Information");
        msinfo_create();
        g_show_actions = FALSE;
        g_show_toolbar = FALSE;
        g_tree_w = 230;
    }
    else if (!wcscmp(g_console, L"fsmgmt"))
    {
        lstrcpyW(g_title, L"Shared Folders");
        shares_create(NULL);
    }
    else
    {
        lstrcpyW(g_console, L"compmgmt");
        lstrcpyW(g_title, L"Computer Management");
        root = node_add(NULL, L"Computer Management (Local)", IC_COMPUTER, NULL, NULL);
        tools = node_add(root, L"System Tools", IC_TOOLS, NULL, NULL);
        lstrcpyW(tools->desc, L"Event Viewer, shared folders, local users and groups, and devices");
        events_create(tools);
        shares_create(tools);
        users_create(tools);
        devices_create(tools);
        storage = node_add(root, L"Storage", IC_STORAGEFOLDER, NULL, NULL);
        lstrcpyW(storage->desc, L"The disks and volumes of this computer");
        disks_create(storage);
        apps = node_add(root, L"Services and Applications", IC_FOLDER, NULL, NULL);
        lstrcpyW(apps->desc, L"Services and the Stained Glass system services under them");
        services_create(apps);
        root->expanded = tools->expanded = storage->expanded = apps->expanded = TRUE;
    }
}

static HFONT make_font(int pt, int weight)
{
    LOGFONTW lf = { 0 };
    lf.lfHeight = -MulDiv(pt, g_dpi, 72);
    lf.lfWeight = weight;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lstrcpyW(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
}

static void load_icons(void)
{
    HBITMAP bmp = LoadImageW(g_inst, MAKEINTRESOURCEW(2), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
    int sz = g_dpi >= 144 ? 32 : 16;
    g_icons = ImageList_Create(sz, sz, ILC_COLOR32, IC_COUNT, 4);
    if (sz == 32)
    {
        HBITMAP big = LoadImageW(g_inst, MAKEINTRESOURCEW(3), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);
        if (big) { DeleteObject(bmp); bmp = big; }
    }
    if (bmp) ImageList_Add(g_icons, bmp, NULL);
    DeleteObject(bmp);
    g_icons32 = ImageList_Create(32, 32, ILC_COLOR32, IC_COUNT, 4);
    if ((bmp = LoadImageW(g_inst, MAKEINTRESOURCEW(3), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION)))
    {
        ImageList_Add(g_icons32, bmp, NULL);
        DeleteObject(bmp);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, WCHAR *cmdline, int show)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES | ICC_LISTVIEW_CLASSES };
    MSG msg;
    HDC dc;
    const WCHAR *full = GetCommandLineW(), *args = full;
    HACCEL acc;
    ACCEL accels[] = { { FVIRTKEY, VK_F5, CMD_REFRESH }, { FVIRTKEY | FALT, VK_RETURN, CMD_PROPS },
                       { FVIRTKEY | FALT, VK_LEFT, TB_BACK }, { FVIRTKEY | FALT, VK_RIGHT, TB_FORWARD },
                       { FVIRTKEY, VK_F1, CMD_HELP } };

    (void)prev; (void)cmdline;
    g_inst = inst;
    if (*args == '"') { args++; while (*args && *args != '"') args++; if (*args) args++; }
    else while (*args && *args != ' ' && *args != '\t') args++;
    while (*args == ' ' || *args == '\t') args++;

    /* the Linux side: through sg-sysinfo's bridge, unless there is none */
    if (!sys_init(full) && !wcsstr(full, L"--no-bridge") && sys_relaunch(args)) return 0;

    SetProcessDPIAware();
    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    g_font = make_font(9, FW_NORMAL);
    g_font_bold = make_font(9, FW_SEMIBOLD);
    g_font_head = make_font(12, FW_SEMIBOLD);
    GetEnvironmentVariableW(L"SG_MMC_DUMP", g_dump_path, MAX_PATH);
    load_icons();
    pick_console(full);
    /* msinfo32 /report FILE: the report, no window */
    if (!wcscmp(g_console, L"msinfo32"))
    {
        int argc, i;
        WCHAR **argv = CommandLineToArgvW(full, &argc);
        for (i = 1; argv && i + 1 < argc; i++)
            if (!_wcsicmp(argv[i], L"/report") || !_wcsicmp(argv[i], L"-report")) return msinfo_report(argv[i + 1]);
    }

    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"MMCMainFrame";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = actions_proc;
    wc.hIcon = NULL;
    wc.lpszClassName = L"SgMmcActions";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = banner_proc;
    wc.lpszClassName = L"SgMmcBanner";
    RegisterClassExW(&wc);

    build_console();
    {
        RECT work;
        int w, hgt;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        w = min(S(1180), (work.right - work.left) * 92 / 100);
        hgt = min(S(720), (work.bottom - work.top) * 90 / 100);
        g_main = CreateWindowExW(0, L"MMCMainFrame", g_title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                 work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - hgt) / 2,
                                 w, hgt, NULL, create_menu(), inst, NULL);
    }
    create_toolbar(g_main);
    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, NULL, WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                               0, 0, 0, 0, g_main, (HMENU)ID_STATUS, inst, NULL);
    g_tree = CreateWindowExW(0, WC_TREEVIEWW, NULL,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES | TVS_LINESATROOT |
                             TVS_SHOWSELALWAYS, 0, 0, 0, 0, g_main, (HMENU)ID_TREE, inst, NULL);
    TreeView_SetImageList(g_tree, g_icons, TVSIL_NORMAL);
    g_list = CreateWindowExW(0, WC_LISTVIEWW, NULL,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL |
                             LVS_SHAREIMAGELISTS, 0, 0, 0, 0, g_main, (HMENU)ID_LIST, inst, NULL);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP |
                                              LVS_EX_LABELTIP);
    ListView_SetImageList(g_list, g_icons, LVSIL_SMALL);
    SetWindowTheme(g_list, L"Explorer", NULL);
    SetWindowTheme(g_tree, L"Explorer", NULL);
    g_empty = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_CENTER | SS_NOPREFIX, 0, 0, 0, 0, g_main, NULL, inst, NULL);
    g_actions = CreateWindowExW(0, L"SgMmcActions", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g_main,
                                (HMENU)ID_ACTIONS, inst, NULL);
    g_banner = CreateWindowExW(0, L"SgMmcBanner", NULL, WS_CHILD, 0, 0, 0, 0, g_main, (HMENU)ID_BANNER, inst, NULL);
    SendMessageW(g_tree, WM_SETFONT, (WPARAM)g_font, 0);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, 0);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, 0);
    SendMessageW(g_empty, WM_SETFONT, (WPARAM)g_font, 0);

    {
        node_t *n;
        for (n = g_roots.child; n; n = n->next) tree_insert(n);
        for (n = g_roots.child; n; n = n->next)
        {
            node_t *c;
            if (n->expanded || n->child) TreeView_Expand(g_tree, n->hti, TVE_EXPAND);
            for (c = n->child; c; c = c->next) if (c->expanded && c->child) TreeView_Expand(g_tree, c->hti, TVE_EXPAND);
        }
    }
    layout();
    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    /* the console's first item: the first root with a view of its own, or the root */
    {
        node_t *first = g_initial ? g_initial : g_roots.child, *p;
        for (p = first->parent; p && p != &g_roots; p = p->parent) if (p->hti) TreeView_Expand(g_tree, p->hti, TVE_EXPAND);
        node_select(first);
    }
    SetTimer(g_main, TIMER_TICK, 1000, NULL);
    acc = CreateAcceleratorTableW(accels, ARRAY_SIZE(accels));
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        if (TranslateAcceleratorW(g_main, acc, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
