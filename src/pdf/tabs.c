/* sg-pdf -- SG PDF: documents in tabs.
 *
 * Each open document is a tab with an engine of its own: sg-pdf's bridge
 * starts one `sg-pdf --serve` process a document ("@N" before a request goes
 * to engine N, "@N quit" ends it), so a document's memory goes when its tab
 * closes and an idle one costs nothing but its pages of memory.
 *
 * The program's state `g` is the document in front, as it always was; the
 * other tabs' states are kept here whole (their pages' pictures, page, zoom,
 * scroll, search, selection, fields ...) and swapped in when their tab is
 * chosen. What belongs to the window rather than a document -- the panes,
 * the page display, dark pages, the format for new text, the signature
 * ready to place -- stays with the window. A tab's tool is closed when
 * another tab comes to the front.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"

#define MAX_TABS_HARD 32
#define OPEN_KEY L"Software\\Stained Glass\\PDF Viewer"

static app_t g_docs[MAX_TABS_HARD];     /* the tabs in order; g_docs[g_active] is stale: the live one is g */
static int g_ntabs, g_active = -1, g_engine_next, g_gen_next = 1000;

int next_generation(void)
{
    return ++g_gen_next;
}

int tab_count(void) { return g_ntabs; }
int tab_active(void) { return g_active; }

const app_t *tab_doc(int i)
{
    if (i < 0 || i >= g_ntabs) return NULL;
    return i == g_active ? &g : &g_docs[i];
}

int tab_max(void)
{
    WCHAR v[16];
    int n = 20;
    if (GetEnvironmentVariableW(L"SG_PDF_MAX_TABS", v, 16)) n = _wtoi(v);
    return max(1, min(n, MAX_TABS_HARD));
}

/* what belongs to the window, not to a document */
static void keep_ui(app_t *dst, const app_t *src)
{
    dst->side = src->side;
    dst->pane = src->pane;
    dst->layout = src->layout;
    dst->cover = src->cover;
    dst->night = src->night;
    dst->reading = src->reading;
    dst->home = src->home;
    dst->menu_open = src->menu_open;
    memcpy(dst->fmt_font, src->fmt_font, sizeof(dst->fmt_font));
    dst->fmt_size = src->fmt_size;
    dst->fmt_color = src->fmt_color;
    dst->fmt_style = src->fmt_style;
    dst->fmt_align = src->fmt_align;
    dst->ccolor = src->ccolor;
    dst->sig_kind = src->sig_kind;
    dst->sig_data = src->sig_data;
    memcpy(dst->image_path, src->image_path, sizeof(dst->image_path));
    dst->stamp = src->stamp;
    memcpy(dst->cert_file, src->cert_file, sizeof(dst->cert_file));
    memcpy(dst->cert_reason, src->cert_reason, sizeof(dst->cert_reason));
    memcpy(dst->cert_location, src->cert_location, sizeof(dst->cert_location));
    dst->bridged = src->bridged;
    dst->dpi = src->dpi;
    dst->dark = src->dark;
}

/* a document's state as a new one starts: nothing open, the view's defaults */
static void fresh(app_t *a, const app_t *ui)
{
    memset(a, 0, sizeof(*a));
    keep_ui(a, ui);
    a->zoom = 1.0;
    a->fit = FIT_WIDTH;
    a->hit = -1;
    a->sel_a.page = a->sel_b.page = -1;
    a->perms = 0xFFFF;
    lstrcpyA(a->protect, "keep");
    a->generation = next_generation();
}

/* the document in front is going behind (or away): what was in hand for it ends */
static void leave_front(void)
{
    tool_commit_editor();
    if (g.tool != TOOL_NONE) tool_set(TOOL_NONE);
    tool_cancel();
    render_clear_wants(FALSE);
    render_clear_wants(TRUE);
}

/* the document now in front: drawn, listed and laid out as its own */
static void came_to_front(void)
{
    g.generation = next_generation();      /* pictures still being made for the other one are dropped */
    g.home = FALSE;
    side_load_outline();
    view_relayout(TRUE);
    org_update();
    side_update();
    sigbar_update();
    toolui_update();
    app_update_title();
    app_layout();
    InvalidateRect(g_view, NULL, FALSE);
    frame_update();
    app_status_changed();
}

/* a new tab for a document about to be opened or made; FALSE: too many open */
BOOL tab_new(void)
{
    app_t ui;
    if (g_ntabs >= tab_max()) {
        WCHAR t[200];
        swprintf(t, 200, L"%d documents are open, as many as SG PDF keeps. Close one to open another.", g_ntabs);
        app_set_status(L"%ls", t);
        if (!GetEnvironmentVariableW(L"SG_PDF_QUIET", NULL, 0)) MessageBoxW(g_main, t, APP_NAME, MB_OK | MB_ICONINFORMATION);
        return FALSE;
    }
    if (g_ntabs) {
        leave_front();
        g_docs[g_active] = g;
    }
    ui = g;
    fresh(&g, &ui);
    g.engine = ++g_engine_next;
    g_active = g_ntabs++;
    if (g_tree) TreeView_DeleteAllItems(g_tree);
    return TRUE;
}

/* a new tab whose document could not be made: gone again, the one before in front */
void tab_discard_new(void)
{
    char head[64];
    if (g_ntabs <= 0) return;
    br_request("quit", head, sizeof(head), NULL, NULL);
    g_ntabs--;
    if (!g_ntabs) { g_active = -1; g.engine = 0; return; }
    {
        app_t ui = g;
        g_active = min(g_active, g_ntabs - 1);
        g = g_docs[g_active];
        keep_ui(&g, &ui);
    }
    came_to_front();
}

int tab_find(const WCHAR *path)
{
    int i;
    for (i = 0; i < g_ntabs; i++) {
        const app_t *d = tab_doc(i);
        if (d->path[0] && !_wcsicmp(d->path, path)) return i;
    }
    return -1;
}

void tab_switch(int i)
{
    app_t ui;
    if (i < 0 || i >= g_ntabs) return;
    if (i == g_active) { home_switch(FALSE); return; }
    leave_front();
#ifndef SG_MUTANT_TABKEEP
    g_docs[g_active] = g;
#endif
    ui = g;
    g = g_docs[i];
    keep_ui(&g, &ui);
    g_active = i;
    came_to_front();
}

/* the tab's document closed (after asking about its changes); FALSE: the person kept it */
BOOL tab_close(int i)
{
    char head[64];
    if (i < 0 || i >= g_ntabs) return TRUE;
    if (i != g_active) tab_switch(i);
    if (!doc_close_prompt()) return FALSE;
    leave_front();
    br_request("quit", head, sizeof(head), NULL, NULL);
    app_free_document();
    memmove(&g_docs[i], &g_docs[i + 1], (g_ntabs - i - 1) * sizeof(app_t));
    g_ntabs--;
    if (!g_ntabs) {
        app_t ui = g;
        fresh(&g, &ui);
        g_active = -1;
        g.path[0] = g.name[0] = 0;
        app_layout();
        frame_update();
        app_update_title();
        app_status_changed();
        return TRUE;
    }
    {
        app_t ui = g;
        g_active = min(i, g_ntabs - 1);
        g = g_docs[g_active];
        keep_ui(&g, &ui);
    }
    came_to_front();
    return TRUE;
}

/* every tab closed (the window closing); FALSE: one was kept */
BOOL tabs_close_all(void)
{
    tabs_remember();
    while (g_ntabs > 0)
        if (!tab_close(g_ntabs - 1)) return FALSE;
    return TRUE;
}

void tab_move(int from, int to)
{
    app_t t;
    if (from < 0 || from >= g_ntabs || to < 0 || to >= g_ntabs || from == to) return;
    g_docs[g_active] = g;            /* the live one into its place for the move */
    t = g_docs[from];
    if (from < to) memmove(&g_docs[from], &g_docs[from + 1], (to - from) * sizeof(app_t));
    else memmove(&g_docs[to + 1], &g_docs[to], (from - to) * sizeof(app_t));
    g_docs[to] = t;
    if (g_active == from) g_active = to;
    else if (from < g_active && to >= g_active) g_active--;
    else if (from > g_active && to <= g_active) g_active++;
    frame_update();
    app_dump();
}

/* Ctrl+Tab and Ctrl+Shift+Tab: Home, then each tab in turn */
void tab_cycle(int dir)
{
    int pos = home_shown() ? -1 : g_active, n = g_ntabs + 1;
    pos = ((pos + 1 + dir) % n + n) % n - 1;
    if (pos < 0) home_switch(TRUE);
    else tab_switch(pos);
}

/* ---- the open documents again at the next start (an option) ----------------------------------------------- */

BOOL tabs_reopen_on(void)
{
    DWORD v = 0, cb = sizeof(v);
    return !RegGetValueW(HKEY_CURRENT_USER, OPEN_KEY, L"ReopenDocuments", RRF_RT_REG_DWORD, NULL, &v, &cb) && v;
}

void tabs_set_reopen(BOOL on)
{
    DWORD v = on;
    RegSetKeyValueW(HKEY_CURRENT_USER, OPEN_KEY, L"ReopenDocuments", REG_DWORD, &v, sizeof(v));
}

void tabs_remember(void)
{
    WCHAR *list;
    size_t cap = 2, k = 0;
    int i;
    for (i = 0; i < g_ntabs; i++) cap += lstrlenW(tab_doc(i)->path) + 1;
    if (!(list = calloc(cap, sizeof(WCHAR)))) return;
    for (i = 0; i < g_ntabs; i++) {
        const app_t *d = tab_doc(i);
        if (!d->path[0]) continue;
        lstrcpyW(list + k, d->path);
        k += lstrlenW(d->path) + 1;
    }
    list[k++] = 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, OPEN_KEY, L"OpenDocuments", REG_MULTI_SZ, list, (DWORD)(k * sizeof(WCHAR)));
    free(list);
}

void tabs_reopen(void)
{
    WCHAR buf[8192], *p;
    DWORD cb = sizeof(buf);
    if (!tabs_reopen_on()) return;
    if (RegGetValueW(HKEY_CURRENT_USER, OPEN_KEY, L"OpenDocuments", RRF_RT_REG_MULTI_SZ, NULL, buf, &cb)) return;
    for (p = buf; *p; p += lstrlenW(p) + 1)
        if (GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES) app_open(p);
}

/* Combine Files: the open documents' files */
int tabs_paths(WCHAR paths[][MAX_PATH], int cap)
{
    int i, n = 0;
    for (i = 0; i < g_ntabs && n < cap; i++)
        if (tab_doc(i)->path[0]) lstrcpynW(paths[n++], tab_doc(i)->path, MAX_PATH);
    return n;
}
