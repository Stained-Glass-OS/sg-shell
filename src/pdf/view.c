/* sg-pdf -- SG PDF: the pages, in one continuous scroll.
 *
 * Pages are laid out top to bottom at the zoom (fit width, fit page, or a
 * percentage), each drawn from the bitmap the render thread made for this
 * zoom and rotation; while a new one is made, the old one is stretched in
 * its place, or white paper is shown. Search hits and the selection are
 * multiplied into the page (DPa), so the text under them stays readable.
 *
 * Text is selected by dragging (double-click: a word) over the character
 * boxes poppler reports; links are followed with a click.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"
#include <shellapi.h>
#include <wctype.h>

#define GAP dpx(10)
#define MARGIN dpx(14)
#define T_AUTOSCROLL 1

static HCURSOR g_cursor;
static void relayout_at_page(int page);
static void flip_to(int dir);
static POINT g_down;
static int g_down_link_page = -1, g_down_link = -1;
static BOOL g_link_pending;

double view_scale(void)
{
    return g.zoom * g.dpi / 72.0;
}

static void disp_size(int i, double s, int *w, int *h)
{
    page_t *p = &g.pages[i];
    int a = (int)ceil(p->w * s), b = (int)ceil(p->h * s);
    if (g.rot == 90 || g.rot == 270) { *w = b; *h = a; }
    else { *w = a; *h = b; }
}

static void client_size(int *cw, int *ch)
{
    RECT rc;
    GetClientRect(g_view, &rc);
    *cw = rc.right;
    *ch = rc.bottom;
}

void view_page_rect(int i, RECT *rc)
{
    page_t *p = &g.pages[i];
    rc->left = p->x - g.sx;
    rc->top = p->y - g.sy;
    rc->right = rc->left + p->dw;
    rc->bottom = rc->top + p->dh;
}

void view_page_to_client(int i, float x, float y, POINT *pt)
{
    page_t *p = &g.pages[i];
    double s = view_scale(), dx, dy;
    switch (g.rot) {
    case 90:  dx = (p->h - y) * s; dy = x * s; break;
    case 180: dx = (p->w - x) * s; dy = (p->h - y) * s; break;
    case 270: dx = y * s; dy = (p->w - x) * s; break;
    default:  dx = x * s; dy = y * s; break;
    }
    pt->x = p->x - g.sx + (int)floor(dx + 0.5);
    pt->y = p->y - g.sy + (int)floor(dy + 0.5);
}

void view_client_to_page(int i, int cx, int cy, float *x, float *y)
{
    page_t *p = &g.pages[i];
    double s = view_scale();
    double dx = (cx - (p->x - g.sx)) / s, dy = (cy - (p->y - g.sy)) / s;
    switch (g.rot) {
    case 90:  *x = (float)dy; *y = (float)(p->h - dx); break;
    case 180: *x = (float)(p->w - dx); *y = (float)(p->h - dy); break;
    case 270: *x = (float)(p->w - dy); *y = (float)dx; break;
    default:  *x = (float)dx; *y = (float)dy; break;
    }
}

void view_box_to_client(int page, const frect *b, RECT *rc)
{
    POINT a, c;
    view_page_to_client(page, b->x1, b->y1, &a);
    view_page_to_client(page, b->x2, b->y2, &c);
    rc->left = min(a.x, c.x);
    rc->right = max(a.x, c.x);
    rc->top = min(a.y, c.y);
    rc->bottom = max(a.y, c.y);
    if (rc->right == rc->left) rc->right++;
    if (rc->bottom == rc->top) rc->bottom++;
}

/* ---- layout and scrolling ------------------------------------------------------------------------ */

static void set_scrollbars(void)
{
    int cw, ch;
    SCROLLINFO si = { sizeof(si), SIF_ALL | SIF_DISABLENOSCROLL };
    client_size(&cw, &ch);
    si.nMin = 0;
    si.nMax = max(g.doch - 1, 0);
    si.nPage = ch;
    si.nPos = g.sy;
    SetScrollInfo(g_view, SB_VERT, &si, TRUE);
    si.fMask = SIF_ALL;
    si.nMax = max(g.docw - 1, 0);
    si.nPage = cw;
    si.nPos = g.sx;
    SetScrollInfo(g_view, SB_HORZ, &si, TRUE);
}

static BOOL flip(void);

static int page_at_y(int docy)
{
    int i;
    for (i = 0; i < g.npages; i++) {
        if (g.pages[i].y <= -1000000) continue;     /* not in the spread shown */
        if (docy < g.pages[i].y + g.pages[i].dh + GAP / 2) return i;
    }
    for (i = g.npages - 1; i > 0 && g.pages[i].y <= -1000000; i--) ;
    return max(0, i);
}

static void update_current(void)
{
    int cw, ch, cur;
    if (!g.npages) return;
    client_size(&cw, &ch);
    cur = page_at_y(g.sy + ch / 2);
    /* at the very end the last page is the current one */
    if (g.sy + ch >= g.doch - 1 && !flip()) cur = g.npages - 1;
    if (g.sy <= 0 && !flip()) cur = 0;
    if (flip()) cur = g.current;
    if (cur != g.current) {
        g.current = cur;
        side_ensure_visible(cur);
        side_update();
    }
    app_status_changed();
}

void view_scroll_to(int x, int y)
{
    int cw, ch;
    client_size(&cw, &ch);
    x = max(0, min(x, g.docw - cw));
    y = max(0, min(y, g.doch - ch));
    if (x == g.sx && y == g.sy) { update_current(); return; }
    g.sx = x;
    g.sy = y;
    set_scrollbars();
    InvalidateRect(g_view, NULL, FALSE);
    update_current();
}

static BOOL two_up(void)
{
#ifdef SG_MUTANT_TWOUP
    return FALSE;
#endif
    return g.layout == LAYOUT_TWO || g.layout == LAYOUT_TWOCONT;
}

static BOOL flip(void)
{
    return g.layout == LAYOUT_SINGLE || g.layout == LAYOUT_TWO;
}

/* the first page of the spread (the pages shown side by side) page i is in */
static int spread_of(int i)
{
    if (!two_up()) return i;
    if (g.cover) return i == 0 ? 0 : ((i - 1) / 2) * 2 + 1;
    return (i / 2) * 2;
}

static int spread_len(int first)
{
    if (!two_up() || (g.cover && first == 0) || first + 1 >= g.npages) return 1;
    return 2;
}

#define HIDDEN_Y (-4000000)

static void layout(double s)
{
    int i, y, maxw = 0, cw, ch, cur = g.npages ? spread_of(max(0, min(g.current, g.npages - 1))) : 0;
    client_size(&cw, &ch);
    for (i = 0; i < g.npages; i++) disp_size(i, s, &g.pages[i].dw, &g.pages[i].dh);
    for (i = 0; i < g.npages; i += spread_len(spread_of(i))) {
        int a = spread_of(i), n = spread_len(a), w = g.pages[a].dw + (n > 1 ? GAP + g.pages[a + 1].dw : 0);
        if (flip() && a != cur) continue;
        maxw = max(maxw, w);
    }
    g.docw = max(maxw + 2 * MARGIN, cw);
    y = MARGIN;
    for (i = 0; i < g.npages;) {
        int a = spread_of(i), n = spread_len(a), w, h;
        if (flip() && a != cur) {
            for (; i < a + n; i++) { g.pages[i].x = 0; g.pages[i].y = HIDDEN_Y; }
            continue;
        }
        w = g.pages[a].dw + (n > 1 ? GAP + g.pages[a + 1].dw : 0);
        h = max(g.pages[a].dh, n > 1 ? g.pages[a + 1].dh : 0);
        g.pages[a].x = (g.docw - w) / 2;
        g.pages[a].y = y + (h - g.pages[a].dh) / 2;
        if (n > 1) {
            g.pages[a + 1].x = g.pages[a].x + g.pages[a].dw + GAP;
            g.pages[a + 1].y = y + (h - g.pages[a + 1].dh) / 2;
        }
        y += h + GAP;
        i = a + n;
    }
    g.doch = g.npages ? y - GAP + MARGIN : 0;
}

void view_set_layout(int layout, BOOL cover)
{
    int cur = g.current;
    g.layout = layout;
    g.cover = cover;
    relayout_at_page(cur);
    app_status_changed();
}

static double fit_zoom(void)
{
    int i, cw, ch;
    double best = 0;
    client_size(&cw, &ch);
    for (i = 0; i < g.npages; i++) {
        double pw = g.pages[i].w, ph = g.pages[i].h, z;
        if (g.rot == 90 || g.rot == 270) { double t = pw; pw = ph; ph = t; }
        z = (cw - 2 * MARGIN - (two_up() ? GAP : 0)) / ((two_up() ? 2 * pw : pw) * g.dpi / 72.0);
        if (g.fit == FIT_PAGE) z = min(z, (ch - 2 * MARGIN) / (ph * g.dpi / 72.0));
        if (z > best) best = z;
        if (i > 64) break;  /* the first pages decide */
    }
    return max(0.1, min(best, 5.0));
}

/* Lay out again; the point at `anchor` (client; NULL: the middle) stays put. */
static void relayout_at(POINT *anchor)
{
    int cw, ch, ax, ay, pg = 0;
    double fx = 0.5, fy = 0;
    client_size(&cw, &ch);
    ax = anchor ? anchor->x : cw / 2;
    ay = anchor ? anchor->y : ch / 2;
    if (g.npages && g.doch) {
        page_t *p;
        pg = page_at_y(g.sy + ay);
        p = &g.pages[pg];
        fx = p->dw ? (double)(g.sx + ax - p->x) / p->dw : 0.5;
        fy = p->dh ? (double)(g.sy + ay - p->y) / p->dh : 0;
    }
    if (g.fit != FIT_NONE) {
        /* twice: a scroll bar that comes or goes changes the width */
        g.zoom = fit_zoom();
        layout(view_scale());
        set_scrollbars();
        g.zoom = fit_zoom();
    }
    layout(view_scale());
    if (g.npages) {
        page_t *p = &g.pages[pg];
        g.sx = p->x + (int)(fx * p->dw) - ax;
        g.sy = p->y + (int)(fy * p->dh) - ay;
    }
    g.sx = max(0, min(g.sx, g.docw - cw));
    g.sy = max(0, min(g.sy, g.doch - ch));
    set_scrollbars();
    InvalidateRect(g_view, NULL, FALSE);
    update_current();
}

/* lay out again with the page `page` at the top */
static void relayout_at_page(int page)
{
    int cw, ch;
    if (page >= 0 && page < g.npages) g.current = page;
    if (g.fit != FIT_NONE) {
        g.zoom = fit_zoom();
        layout(view_scale());
        set_scrollbars();
        g.zoom = fit_zoom();
    }
    layout(view_scale());
    client_size(&cw, &ch);
    g.sx = max(0, min(g.sx, g.docw - cw));
    if (g.npages && page >= 0 && page < g.npages) g.sy = max(0, min(g.pages[page].y - GAP, g.doch - ch));
    set_scrollbars();
    InvalidateRect(g_view, NULL, FALSE);
    update_current();
    side_ensure_visible(g.current);
    side_update();
}

void view_relayout(BOOL keep_anchor)
{
    relayout_at(NULL);
    if (!keep_anchor) {
        /* a new document starts at its top, centred */
        int cw, ch;
        client_size(&cw, &ch);
        g.sx = max(0, (g.docw - cw) / 2);
        g.sy = 0;
        set_scrollbars();
        InvalidateRect(g_view, NULL, FALSE);
        update_current();
    }
}

void view_set_zoom(double zoom, int fit, POINT *anchor)
{
    g.fit = fit;
    if (fit == FIT_NONE) g.zoom = max(0.1, min(zoom, 5.0));
    relayout_at(anchor);
    app_status_changed();
}

static const int STEPS[] = { 10, 25, 33, 50, 67, 75, 80, 90, 100, 110, 125, 150, 175, 200, 250, 300, 400, 500 };

void view_zoom_step(int dir)
{
    int i, cur = (int)floor(g.zoom * 100 + 0.5), n = (int)(sizeof(STEPS) / sizeof(STEPS[0]));
    int z = cur;
    if (dir > 0) { for (i = 0; i < n; i++) if (STEPS[i] > cur) { z = STEPS[i]; break; } }
    else { for (i = n - 1; i >= 0; i--) if (STEPS[i] < cur) { z = STEPS[i]; break; } }
    view_set_zoom(z / 100.0, FIT_NONE, NULL);
}

void view_goto_page(int page, float top)
{
    int y;
    if (page < 0 || page >= g.npages) return;
    if (flip() && spread_of(page) != spread_of(g.current)) relayout_at_page(page);
    y = g.pages[page].y - GAP;
    if (top > 0 && g.rot == 0) y = g.pages[page].y + (int)(top * view_scale()) - GAP;
    view_scroll_to(g.sx, y);
    g.current = page;
    side_ensure_visible(page);
    side_update();
    app_status_changed();
}

/* ---- text and links --------------------------------------------------------------------------- */

BOOL page_load_text(int i)
{
    page_t *p = &g.pages[i];
    char line[64], head[256], num[32];
    BYTE *data;
    DWORD len;
    int n;
    if (p->text_loaded) return p->text != NULL || p->ntext == 0;
    snprintf(line, sizeof(line), "text\t%d", i);
    if (br_request(line, head, sizeof(head), &data, &len) < 0) return FALSE;
    p->text_loaded = TRUE;
    n = br_field(head, "n", num, sizeof(num)) ? atoi(num) : 0;
    if (n > 0 && data && len == (DWORD)n * 18) {
        p->text = malloc((n + 1) * sizeof(WCHAR));
        p->boxes = malloc(n * sizeof(frect));
        if (p->text && p->boxes) {
            memcpy(p->text, data, n * 2);
            p->text[n] = 0;
            memcpy(p->boxes, data + n * 2, n * sizeof(frect));
            p->ntext = n;
        }
    }
    free(data);
    return TRUE;
}

BOOL page_load_links(int i)
{
    page_t *p = &g.pages[i];
    char line[64], head[256], num[32], *s, *e;
    BYTE *data;
    DWORD len;
    int n;
    if (p->links_loaded) return TRUE;
    snprintf(line, sizeof(line), "links\t%d", i);
    if (br_request(line, head, sizeof(head), &data, &len) < 0) return FALSE;
    p->links_loaded = TRUE;
    n = br_field(head, "n", num, sizeof(num)) ? atoi(num) : 0;
    if (n > 0 && data) {
        p->links = calloc(n, sizeof(link_t));
        for (s = (char *)data; p->links && s && *s && p->nlinks < n; s = e) {
            link_t *l = &p->links[p->nlinks];
            char kind[8] = "", *tab;
            e = strchr(s, '\n');
            if (e) *e++ = 0;
            if (sscanf(s, "%f %f %f %f", &l->box.x1, &l->box.y1, &l->box.x2, &l->box.y2) != 4) continue;
            if (!(tab = strchr(s, '\t'))) continue;
            lstrcpynA(kind, tab + 1, 5);
            if (!strncmp(kind, "goto", 4)) {
                if (sscanf(tab + 6, "%d\t%f", &l->page, &l->top) != 2) continue;
            } else if (!strncmp(kind, "uri", 3)) {
                l->page = -1;
                l->uri = from_utf8(tab + 5, -1);
            } else continue;
            p->nlinks++;
        }
    }
    free(data);
    return TRUE;
}

static int link_at(POINT pt, int *page)
{
    int i, k;
    for (i = 0; i < g.npages; i++) {
        RECT rc;
        view_page_rect(i, &rc);
        if (!PtInRect(&rc, pt)) continue;
        if (!page_load_links(i)) return -1;
        for (k = 0; k < g.pages[i].nlinks; k++) {
            RECT lr;
            view_box_to_client(i, &g.pages[i].links[k].box, &lr);
            if (PtInRect(&lr, pt)) { *page = i; return k; }
        }
        return -1;
    }
    return -1;
}

static void follow_link(int page, int k)
{
    link_t *l = &g.pages[page].links[k];
    if (l->page >= 0) { view_goto_page(l->page, l->top); return; }
    if (l->uri && (!_wcsnicmp(l->uri, L"http://", 7) || !_wcsnicmp(l->uri, L"https://", 8) ||
                   !_wcsnicmp(l->uri, L"mailto:", 7)))
        ShellExecuteW(g_main, NULL, l->uri, NULL, NULL, SW_SHOWNORMAL);
}

/* the page under a client point; clamp: the nearest page */
int view_page_at(POINT pt, BOOL clamp)
{
    int i;
    for (i = 0; i < g.npages; i++) {
        RECT rc;
        view_page_rect(i, &rc);
        if (PtInRect(&rc, pt)) return i;
        if (clamp && (pt.y < rc.bottom + GAP / 2 || i == g.npages - 1)) return i;
    }
    return -1;
}

/* the caret position (between characters) nearest a point; clamp: to the nearest page */
static BOOL caret_at(POINT pt, caret_t *c, BOOL clamp)
{
    int i, pg = -1, best = -1;
    float x, y, bestd = 1e30f;
    page_t *p;
    for (i = 0; i < g.npages; i++) {
        RECT rc;
        view_page_rect(i, &rc);
        if (pt.y < rc.bottom + GAP / 2 || i == g.npages - 1) {
            if (!clamp && !PtInRect(&rc, pt)) return FALSE;
            pg = i;
            break;
        }
    }
    if (pg < 0) return FALSE;
    if (!page_load_text(pg)) return FALSE;
    p = &g.pages[pg];
    view_client_to_page(pg, pt.x, pt.y, &x, &y);
    c->page = pg;
    c->pos = 0;
    for (i = 0; i < p->ntext; i++) {
        frect *b = &p->boxes[i];
        float d;
        if (p->text[i] == '\n' || b->x2 <= b->x1) continue;
        if (y >= b->y1 && y <= b->y2) d = x < b->x1 ? b->x1 - x : x > b->x2 ? x - b->x2 : 0;
        else d = 10000.0f + (y < b->y1 ? b->y1 - y : y - b->y2) * 4 + fabsf(x - (b->x1 + b->x2) / 2) / 100;
        if (d < bestd) { bestd = d; best = i; }
    }
    if (best >= 0) {
        frect *b = &p->boxes[best];
        c->pos = x > (b->x1 + b->x2) / 2 ? best + 1 : best;
        /* above the first line or below the last: the start or the end */
        if (bestd >= 10000.0f) c->pos = y < b->y1 ? best : best + 1;
    }
    return TRUE;
}

static BOOL over_text(POINT pt)
{
    int i, k;
    float x, y;
    for (i = 0; i < g.npages; i++) {
        RECT rc;
        view_page_rect(i, &rc);
        if (!PtInRect(&rc, pt)) continue;
        if (!page_load_text(i)) return FALSE;
        view_client_to_page(i, pt.x, pt.y, &x, &y);
        for (k = 0; k < g.pages[i].ntext; k++) {
            frect *b = &g.pages[i].boxes[k];
            if (x >= b->x1 - 2 && x <= b->x2 + 2 && y >= b->y1 && y <= b->y2) return TRUE;
        }
        return FALSE;
    }
    return FALSE;
}

BOOL view_over_text(POINT pt)
{
    return over_text(pt);
}

static int caret_cmp(const caret_t *a, const caret_t *b)
{
    return a->page != b->page ? a->page - b->page : a->pos - b->pos;
}

static void sel_order(caret_t *a, caret_t *b)
{
    if (caret_cmp(&g.sel_a, &g.sel_b) <= 0) { *a = g.sel_a; *b = g.sel_b; }
    else { *a = g.sel_b; *b = g.sel_a; }
}

/* the selected text, "\r\n" between lines and pages; free it */
static WCHAR *selected_text(void)
{
    caret_t a, b;
    size_t cap = 256, n = 0;
    WCHAR *out = malloc(cap * sizeof(WCHAR));
    int pg;
    if (!out || !g.has_sel) { free(out); return NULL; }
    sel_order(&a, &b);
    for (pg = a.page; pg <= b.page; pg++) {
        page_t *p = &g.pages[pg];
        int from = pg == a.page ? a.pos : 0, to, i;
#ifdef SG_MUTANT_COPY
        from++;
#endif
        if (!page_load_text(pg)) break;
        to = pg == b.page ? b.pos : p->ntext;
        if (pg > a.page && n && out[n - 1] != '\n') { out[n++] = '\r'; out[n++] = '\n'; }
        for (i = from; i < to && i < p->ntext; i++) {
            if (n + 4 >= cap) { WCHAR *t = realloc(out, (cap *= 2) * sizeof(WCHAR)); if (!t) break; out = t; }
            if (p->text[i] == '\n') out[n++] = '\r';
            out[n++] = p->text[i];
        }
        if (n + 4 >= cap) { WCHAR *t = realloc(out, (cap *= 2) * sizeof(WCHAR)); if (!t) break; out = t; }
    }
    out[n] = 0;
    return out;
}

void view_copy(void)
{
    WCHAR *text = selected_text();
    HGLOBAL mem;
    size_t len;
    if (!text) return;
    len = (wcslen(text) + 1) * sizeof(WCHAR);
    if ((mem = GlobalAlloc(GMEM_MOVEABLE, len))) {
        memcpy(GlobalLock(mem), text, len);
        GlobalUnlock(mem);
        if (OpenClipboard(g_main)) {
            EmptyClipboard();
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
            CloseClipboard();
        } else GlobalFree(mem);
    }
    free(text);
}

BOOL view_has_selection(void)
{
    return g.has_sel;
}

void view_clear_selection(void)
{
    if (!g.has_sel) return;
    g.has_sel = FALSE;
    InvalidateRect(g_view, NULL, FALSE);
    app_status_changed();
}

/* the selected characters of one page as rectangles, one a line (page points) */
int view_selection_rects(int page, frect *out, int cap)
{
    caret_t a, b;
    page_t *p;
    int from, to, k, n = 0;
    if (!g.has_sel || page < 0 || page >= g.npages) return 0;
    sel_order(&a, &b);
    if (page < a.page || page > b.page || !page_load_text(page)) return 0;
    p = &g.pages[page];
    from = page == a.page ? a.pos : 0;
    to = page == b.page ? b.pos : p->ntext;
    for (k = from; k < to && k < p->ntext; k++) {
        frect *c = &p->boxes[k];
        if (p->text[k] == '\n' || c->x2 <= c->x1) continue;
        if (n && fabsf(c->y1 - out[n - 1].y1) < 0.5f * (out[n - 1].y2 - out[n - 1].y1) && c->x1 >= out[n - 1].x1 - 1) {
            out[n - 1].x1 = min(out[n - 1].x1, c->x1); out[n - 1].x2 = max(out[n - 1].x2, c->x2);
            out[n - 1].y1 = min(out[n - 1].y1, c->y1); out[n - 1].y2 = max(out[n - 1].y2, c->y2);
        } else if (n < cap) out[n++] = *c;
    }
    return n;
}

void view_select_all(void)
{
    int i;
    if (!g.npages) return;
    for (i = 0; i < g.npages; i++) page_load_text(i);
    g.sel_a.page = 0;
    g.sel_a.pos = 0;
    g.sel_b.page = g.npages - 1;
    g.sel_b.pos = g.pages[g.npages - 1].ntext;
    g.has_sel = TRUE;
    InvalidateRect(g_view, NULL, FALSE);
    app_status_changed();
}

static void select_word(POINT pt)
{
    caret_t c;
    page_t *p;
    int a, b;
    if (!caret_at(pt, &c, FALSE)) return;
    p = &g.pages[c.page];
    if (!p->ntext) return;
    a = min(c.pos, p->ntext - 1);
    if (a > 0 && !iswalnum(p->text[a]) && iswalnum(p->text[a - 1])) a--;
    if (!iswalnum(p->text[a])) return;
    b = a;
    while (a > 0 && iswalnum(p->text[a - 1])) a--;
    while (b < p->ntext && iswalnum(p->text[b])) b++;
    g.sel_a.page = g.sel_b.page = c.page;
    g.sel_a.pos = a;
    g.sel_b.pos = b;
    g.has_sel = TRUE;
    InvalidateRect(g_view, NULL, FALSE);
    app_status_changed();
}

/* ---- search ----------------------------------------------------------------------------------- */

void view_find_clear(void)
{
    free(g.hits);
    g.hits = NULL;
    g.nhits = 0;
    g.hit = -1;
    g.searched = FALSE;
    g.needle[0] = 0;
    InvalidateRect(g_view, NULL, FALSE);
    app_status_changed();
}

static void show_hit(void)
{
    RECT rc;
    int cw, ch, x = g.sx, y = g.sy;
    if (g.hit < 0 || g.hit >= g.nhits) return;
    client_size(&cw, &ch);
    view_box_to_client(g.hits[g.hit].page, &g.hits[g.hit].box, &rc);
    if (rc.top < dpx(40) || rc.bottom > ch - dpx(40)) y = g.sy + (rc.top + rc.bottom) / 2 - ch / 2;
    if (rc.left < 0 || rc.right > cw) x = g.sx + (rc.left + rc.right) / 2 - cw / 2;
    view_scroll_to(x, y);
    InvalidateRect(g_view, NULL, FALSE);
}

void view_find(const WCHAR *needle, int dir)
{
    if (!needle[0]) { view_find_clear(); return; }
    if (!g.searched || wcscmp(needle, g.needle)) {
        char req[1200], head[256], num[32], esc[1024], *s, *e;
        BYTE *data;
        DWORD len;
        int n, i;
        view_find_clear();
        lstrcpynW(g.needle, needle, 256);
        to_utf8(needle, esc, sizeof(esc));
        for (s = esc; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
        snprintf(req, sizeof(req), "find\t%s", esc);
        g.searched = TRUE;
        if (br_request(req, head, sizeof(head), &data, &len) == 1) {
            n = br_field(head, "n", num, sizeof(num)) ? atoi(num) : 0;
            g.hits = n > 0 ? calloc(n, sizeof(hit_t)) : NULL;
            for (s = (char *)data; g.hits && s && *s && g.nhits < n; s = e) {
                hit_t *h = &g.hits[g.nhits];
                e = strchr(s, '\n');
                if (e) *e++ = 0;
                if (sscanf(s, "%d %f %f %f %f", &h->page, &h->box.x1, &h->box.y1, &h->box.x2, &h->box.y2) == 5 &&
                    h->page >= 0 && h->page < g.npages)
                    g.nhits++;
            }
            free(data);
        }
        /* the first hit from the current page on */
        g.hit = -1;
        for (i = 0; i < g.nhits; i++) if (g.hits[i].page >= g.current) { g.hit = i; break; }
        if (g.hit < 0 && g.nhits) g.hit = 0;
        if (dir < 0 && g.hit >= 0) g.hit = (g.hit + g.nhits - 1) % g.nhits;
    } else if (g.nhits) {
        g.hit = (g.hit + (dir < 0 ? g.nhits - 1 : 1)) % g.nhits;
    }
    show_hit();
    app_status_changed();
}

/* ---- rendering results ----------------------------------------------------------------------------- */

/* the scale page i's whole bitmap is drawn at: the view's, or smaller for a page too large to draw
 * whole (its part in view comes as a tile at the view's scale) */
static double base_scale(int i, double s)
{
    double px = (double)g.pages[i].dw * g.pages[i].dh;
    return px > TILE_PIXELS ? s * sqrt(TILE_PIXELS / px) : s;
}

/* the page is drawn as sharp as the zoom asks: its bitmap (and, for a big page, the tile in view) */
BOOL view_page_ready(int i)
{
    page_t *p = &g.pages[i];
    double s = view_scale(), bs = base_scale(i, s);
    RECT rc, cr, vis;
    if (!p->bmp || p->stale || fabs(p->bscale - bs) > 1e-4 || p->brot != g.rot) return FALSE;
    if (bs == s) return TRUE;
    view_page_rect(i, &rc);
    GetClientRect(g_view, &cr);
    if (!IntersectRect(&vis, &rc, &cr)) return TRUE;
    OffsetRect(&vis, -rc.left, -rc.top);
    return p->tile && p->tlgen == g.generation && fabs(p->tlscale - s) < 1e-4 && p->tlrot == g.rot &&
           vis.left >= p->tlx && vis.top >= p->tly && vis.right <= p->tlx + p->tlw && vis.bottom <= p->tly + p->tlh;
}

void view_rendered_tile(int page, double scale, int rot, int gen, HBITMAP bmp, int x, int y, int w, int h)
{
    page_t *p;
    RECT rc;
    if (gen != g.generation || page < 0 || page >= g.npages || !bmp || fabs(scale - view_scale()) > 1e-4 || rot != g.rot) {
        if (bmp) DeleteObject(bmp);
        return;
    }
    p = &g.pages[page];
    if (p->tile) DeleteObject(p->tile);
    p->tile = bmp;
    p->tlx = x; p->tly = y; p->tlw = w; p->tlh = h;
    p->tlscale = scale;
    p->tlrot = rot;
    p->tlgen = gen;
    view_page_rect(page, &rc);
    InvalidateRect(g_view, &rc, FALSE);
}

void view_rendered(int page, BOOL thumb, double scale, int rot, int gen, HBITMAP bmp, int w, int h)
{
    page_t *p;
    if (gen != g.generation || page < 0 || page >= g.npages || !bmp) { if (bmp) DeleteObject(bmp); return; }
    p = &g.pages[page];
    if (thumb) {
        if (p->thumb) DeleteObject(p->thumb);
        p->thumb = bmp;
        p->tw = w;
        p->th = h;
        p->trot = rot;
        p->tstale = FALSE;
        side_update();
        org_update();
        app_dump();
        return;
    }
    if (fabs(scale - base_scale(page, view_scale())) > 1e-4 || rot != g.rot) {
        /* made for a zoom we left: still better than nothing, or than a page
         * drawn before the last change */
        if (p->bmp && !p->stale) { DeleteObject(bmp); return; }
    }
    if (p->bmp) DeleteObject(p->bmp);
    p->bmp = bmp;
    p->bw = w;
    p->bh = h;
    p->bscale = scale;
    p->brot = rot;
    p->stale = FALSE;
    {
        RECT rc;
        view_page_rect(page, &rc);
        InvalidateRect(g_view, &rc, FALSE);
    }
    view_free_far();
}

void view_free_far(void)
{
    int cw, ch, i, first = -1, last = -1;
    client_size(&cw, &ch);
    for (i = 0; i < g.npages; i++) {
        page_t *p = &g.pages[i];
        if (p->y + p->dh >= g.sy && p->y <= g.sy + ch) { if (first < 0) first = i; last = i; }
    }
    if (first < 0) return;
    for (i = 0; i < g.npages; i++) {
        if ((i < first - 3 || i > last + 3) && g.pages[i].bmp) { DeleteObject(g.pages[i].bmp); g.pages[i].bmp = NULL; }
        if ((i < first || i > last) && g.pages[i].tile) { DeleteObject(g.pages[i].tile); g.pages[i].tile = NULL; }
    }
}

/* ---- painting ---------------------------------------------------------------------------------- */

static void multiply_rect(HDC dc, const RECT *rc, HBRUSH br)
{
    HGDIOBJ old = SelectObject(dc, br);
    PatBlt(dc, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, 0x00A000C9 /* DPa */);
    SelectObject(dc, old);
}

static void paint(HDC out)
{
    RECT cr;
    HDC dc, mem;
    HBITMAP buf, oldbuf;
    HBRUSH canvas = CreateSolidBrush(C_CANVAS), paper = GetStockObject(g.night ? BLACK_BRUSH : WHITE_BRUSH);
#ifndef SG_MUTANT_NIGHT
    DWORD rop = g.night ? NOTSRCCOPY : SRCCOPY;   /* Dark Pages: the page's colours turned over */
#else
    DWORD rop = SRCCOPY;
#endif
    HBRUSH shadow = CreateSolidBrush(C_SHADOW), selb = CreateSolidBrush(C_SELECT);
    HBRUSH hitb = CreateSolidBrush(C_HIT), curb = CreateSolidBrush(C_HITCUR);
    double s = view_scale();
    int i, last_visible = -1;
    GetClientRect(g_view, &cr);
    dc = CreateCompatibleDC(out);
    buf = CreateCompatibleBitmap(out, max(cr.right, 1), max(cr.bottom, 1));
    oldbuf = SelectObject(dc, buf);
    FillRect(dc, &cr, canvas);
    mem = CreateCompatibleDC(out);
    render_clear_wants(FALSE);

    if (!g.npages) {
        HFONT of = SelectObject(dc, g_font);
        const WCHAR *msg = g.error[0] ? g.error : g.path[0] ? L"Opening..." : L"Open a PDF file (Ctrl+O), or drop one here.";
        RECT tr = cr;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_SUBTEXT);
        DrawTextW(dc, msg, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
    }

    for (i = 0; i < g.npages; i++) {
        page_t *p = &g.pages[i];
        RECT rc, sh;
        int k;
        if (p->y + p->dh < g.sy || p->y > g.sy + cr.bottom) continue;
        last_visible = i;
        view_page_rect(i, &rc);
        sh = rc;
        OffsetRect(&sh, dpx(1), dpx(2));
        InflateRect(&sh, 1, 1);
        FillRect(dc, &sh, shadow);
        if (p->bmp) {
            HGDIOBJ ob = SelectObject(mem, p->bmp);
            if (fabs(p->bscale - s) < 1e-4 && p->brot == g.rot) {
                FillRect(dc, &rc, paper);
                BitBlt(dc, rc.left, rc.top, min(p->bw, rc.right - rc.left), min(p->bh, rc.bottom - rc.top), mem, 0, 0, rop);
            } else if (p->brot == g.rot) {
                SetStretchBltMode(dc, HALFTONE);
                SetBrushOrgEx(dc, 0, 0, NULL);
                StretchBlt(dc, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, mem, 0, 0, p->bw, p->bh, rop);
            } else FillRect(dc, &rc, paper);
            SelectObject(mem, ob);
        } else FillRect(dc, &rc, paper);
        {
            double bs = base_scale(i, s);
            if (!p->bmp || p->stale || fabs(p->bscale - bs) > 1e-4 || p->brot != g.rot) render_want(i, bs, g.rot, FALSE);
            if (bs != s) {
                /* the part in view, sharp: a tile at the view's scale */
                RECT vis;
                if (IntersectRect(&vis, &rc, &cr)) {
                    int bx = vis.left - rc.left, by = vis.top - rc.top, bw = vis.right - vis.left, bh = vis.bottom - vis.top;
                    BOOL have = p->tile && p->tlgen == g.generation && fabs(p->tlscale - s) < 1e-4 && p->tlrot == g.rot &&
                                bx >= p->tlx && by >= p->tly && bx + bw <= p->tlx + p->tlw && by + bh <= p->tly + p->tlh;
                    if (p->tile && p->tlgen == g.generation && fabs(p->tlscale - s) < 1e-4 && p->tlrot == g.rot) {
                        HGDIOBJ ob = SelectObject(mem, p->tile);
                        BitBlt(dc, rc.left + p->tlx, rc.top + p->tly, p->tlw, p->tlh, mem, 0, 0, rop);
                        SelectObject(mem, ob);
                    }
                    if (!have) {
                        int m = max(cr.right, cr.bottom) / 2, tx = max(0, bx - m), ty = max(0, by - m);
                        render_want_tile(i, s, g.rot, tx, ty, min(p->dw - tx, bw + 2 * m), min(p->dh - ty, bh + 2 * m));
                    }
                }
            }
        }
        fields_paint(dc, i);

        /* search hits, the current one stronger */
#ifdef SG_MUTANT_NOHITS
        if (0)
#endif
        for (k = 0; k < g.nhits; k++) {
            RECT hr;
            if (g.hits[k].page != i) continue;
            view_box_to_client(i, &g.hits[k].box, &hr);
            InflateRect(&hr, 1, 1);
            multiply_rect(dc, &hr, k == g.hit ? curb : hitb);
        }
        /* the selection */
        if (g.has_sel) {
            caret_t a, b;
            sel_order(&a, &b);
            if (i >= a.page && i <= b.page && page_load_text(i)) {
                int from = i == a.page ? a.pos : 0, to = i == b.page ? b.pos : p->ntext;
                for (k = from; k < to && k < p->ntext; k++) {
                    RECT sr;
                    if (p->text[k] == '\n' || p->boxes[k].x2 <= p->boxes[k].x1) continue;
                    view_box_to_client(i, &p->boxes[k], &sr);
                    multiply_rect(dc, &sr, selb);
                }
            }
        }
    }
    /* the next page too, so scrolling on finds it ready */
    if (last_visible >= 0 && last_visible + 1 < g.npages) {
        page_t *p = &g.pages[last_visible + 1];
        double bs = base_scale(last_visible + 1, s);
        if (!p->bmp || p->stale || fabs(p->bscale - bs) > 1e-4 || p->brot != g.rot) render_want(last_visible + 1, bs, g.rot, FALSE);
    }
    tool_paint(dc);

    BitBlt(out, 0, 0, cr.right, cr.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldbuf);
    DeleteObject(buf);
    DeleteDC(dc);
    DeleteDC(mem);
    DeleteObject(canvas);
    DeleteObject(shadow);
    DeleteObject(selb);
    DeleteObject(hitb);
    DeleteObject(curb);
}

/* ---- the window ---------------------------------------------------------------------------------- */

/* page at a time: the next (dir 1) or previous spread */
static void flip_to(int dir)
{
    int a = spread_of(g.current), to;
    if (dir > 0) to = a + spread_len(a);
    else to = a > 0 ? spread_of(a - 1) : -1;
    if (to < 0 || to >= g.npages) return;
    relayout_at_page(to);
    if (dir < 0) {
        int cw, ch;
        client_size(&cw, &ch);
        view_scroll_to(g.sx, g.doch);
    }
    app_status_changed();
}

static void scroll_msg(int bar, int code)
{
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    int cw, ch, line = dpx(40), pos;
    client_size(&cw, &ch);
    GetScrollInfo(g_view, bar, &si);
    pos = bar == SB_VERT ? g.sy : g.sx;
    switch (code) {
    case SB_LINEUP: pos -= line; break;
    case SB_LINEDOWN: pos += line; break;
    case SB_PAGEUP: pos -= (bar == SB_VERT ? ch : cw) - line; break;
    case SB_PAGEDOWN: pos += (bar == SB_VERT ? ch : cw) - line; break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
    case SB_TOP: pos = 0; break;
    case SB_BOTTOM: pos = 1 << 30; break;
    default: return;
    }
    if (bar == SB_VERT) view_scroll_to(g.sx, pos);
    else view_scroll_to(pos, g.sy);
}

static void drag_to(POINT pt)
{
    caret_t c;
    if (caret_at(pt, &c, TRUE)) {
        g.sel_b = c;
        g.has_sel = caret_cmp(&g.sel_a, &g.sel_b) != 0;
        InvalidateRect(g_view, NULL, FALSE);
    }
}

static LRESULT CALLBACK view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_MOUSEMOVE: case WM_LBUTTONDBLCLK: case WM_RBUTTONUP:
        if (tool_mouse(hwnd, msg, wp, lp)) return 0;
        break;
    case WM_APP + 10:
        tool_commit_editor();
        return 0;
    case WM_CTLCOLOREDIT:
        /* the editor over the page: the page's white */
        SetBkColor((HDC)wp, RGB(0xFF, 0xFF, 0xFF));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        app_dump();
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (g.npages) relayout_at(NULL);
        return 0;
    case WM_VSCROLL: scroll_msg(SB_VERT, LOWORD(wp)); return 0;
    case WM_COMMAND: return 0;
    case WM_HSCROLL: scroll_msg(SB_HORZ, LOWORD(wp)); return 0;
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int z = (int)floor(g.zoom * 100 + 0.5);
            ScreenToClient(hwnd, &pt);
            z = delta > 0 ? z * 11 / 10 + 1 : z * 10 / 11;
            view_set_zoom(z / 100.0, FIT_NONE, &pt);
        } else if (flip()) {
            int cw, ch;
            client_size(&cw, &ch);
            if (delta < 0 && g.sy + ch >= g.doch - 1) flip_to(1);
            else if (delta > 0 && g.sy <= 0) flip_to(-1);
            else view_scroll_to(g.sx, g.sy - delta * dpx(120) / WHEEL_DELTA);
        } else view_scroll_to(g.sx, g.sy - delta * dpx(120) / WHEEL_DELTA);
        return 0;
    }
    case WM_MOUSEHWHEEL:
        view_scroll_to(g.sx + GET_WHEEL_DELTA_WPARAM(wp) * dpx(120) / WHEEL_DELTA, g.sy);
        return 0;
    case WM_KEYDOWN: {
        int cw, ch;
        if (tool_key(wp)) return 0;
        client_size(&cw, &ch);
        switch (wp) {
        case VK_UP: view_scroll_to(g.sx, g.sy - dpx(40)); return 0;
        case VK_DOWN: view_scroll_to(g.sx, g.sy + dpx(40)); return 0;
        case VK_LEFT:
            if (flip() && g.docw <= cw) flip_to(-1);
            else if (g.docw <= cw) view_goto_page(max(g.current - 1, 0), 0);
            else view_scroll_to(g.sx - dpx(40), g.sy);
            return 0;
        case VK_RIGHT:
            if (flip() && g.docw <= cw) flip_to(1);
            else if (g.docw <= cw) view_goto_page(min(g.current + 1, g.npages - 1), 0);
            else view_scroll_to(g.sx + dpx(40), g.sy);
            return 0;
        case VK_PRIOR:
            if (flip() && g.sy <= 0) { flip_to(-1); return 0; }
            view_scroll_to(g.sx, g.sy - (ch - dpx(40)));
            return 0;
        case VK_NEXT: case VK_SPACE:
            if (flip() && g.sy + ch >= g.doch - 1 && !(wp == VK_SPACE && GetKeyState(VK_SHIFT) < 0)) { flip_to(1); return 0; }
            if (wp == VK_SPACE && GetKeyState(VK_SHIFT) < 0) view_scroll_to(g.sx, g.sy - (ch - dpx(40)));
            else view_scroll_to(g.sx, g.sy + (ch - dpx(40)));
            return 0;
        case VK_HOME: if (flip()) view_goto_page(0, 0); else view_scroll_to(0, 0); return 0;
        case VK_END: if (flip()) view_goto_page(g.npages - 1, 0); else view_scroll_to(0, g.doch); return 0;
        case VK_ESCAPE:
            if (g.has_sel) { g.has_sel = FALSE; InvalidateRect(hwnd, NULL, FALSE); app_status_changed(); }
            return 0;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        caret_t c;
        int page = -1, k;
        SetFocus(hwnd);
        SetCapture(hwnd);
        g_down = pt;
        k = link_at(pt, &page);
        g_link_pending = k >= 0;
        g_down_link_page = page;
        g_down_link = k;
        if (g.has_sel) { g.has_sel = FALSE; InvalidateRect(hwnd, NULL, FALSE); }
        if (GetKeyState(VK_SHIFT) < 0 && g.sel_a.page >= 0 && caret_at(pt, &c, TRUE)) {
            g.sel_b = c;
            g.has_sel = caret_cmp(&g.sel_a, &g.sel_b) != 0;
            g.selecting = TRUE;
        } else if (caret_at(pt, &c, TRUE)) {
            g.sel_a = g.sel_b = c;
            g.selecting = TRUE;
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int page;
        if (g.selecting && GetCapture() == hwnd) {
            if (abs(pt.x - g_down.x) + abs(pt.y - g_down.y) > dpx(3)) g_link_pending = FALSE;
            if (!g_link_pending) drag_to(pt);
            SetTimer(hwnd, T_AUTOSCROLL, 50, NULL);
            g_cursor = LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM);
        } else if (link_at(pt, &page) >= 0) g_cursor = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
        else if (over_text(pt)) g_cursor = LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM);
        else g_cursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        SetCursor(g_cursor);
        return 0;
    }
    case WM_TIMER:
        if (wp == T_AUTOSCROLL) {
            POINT pt;
            int cw, ch, dy = 0, dx = 0;
            if (!g.selecting || GetCapture() != hwnd) { KillTimer(hwnd, T_AUTOSCROLL); return 0; }
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            client_size(&cw, &ch);
            if (pt.y < 0) dy = pt.y; else if (pt.y > ch) dy = pt.y - ch;
            if (pt.x < 0) dx = pt.x; else if (pt.x > cw) dx = pt.x - cw;
            if (dx || dy) { view_scroll_to(g.sx + dx, g.sy + dy); if (!g_link_pending) drag_to(pt); }
        }
        return 0;
    case WM_LBUTTONUP:
        KillTimer(hwnd, T_AUTOSCROLL);
        if (GetCapture() == hwnd) ReleaseCapture();
        g.selecting = FALSE;
        if (g_link_pending && g_down_link >= 0) {
            g_link_pending = FALSE;
            g.has_sel = FALSE;
            follow_link(g_down_link_page, g_down_link);
        }
        app_status_changed();
        return 0;
    case WM_LBUTTONDBLCLK: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        select_word(pt);
        return 0;
    }
    case WM_RBUTTONUP: {
        HMENU m = CreatePopupMenu();
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int cmd;
        AppendMenuW(m, MF_STRING | (g.has_sel ? 0 : MF_GRAYED), CMD_COPY, L"Copy\tCtrl+C");
        AppendMenuW(m, MF_STRING, CMD_SELECTALL, L"Select all\tCtrl+A");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, CMD_ROTATE, L"Rotate clockwise\tCtrl+]");
        AppendMenuW(m, MF_STRING, CMD_ROTATE_LEFT, L"Rotate counterclockwise\tCtrl+[");
        AppendMenuW(m, MF_STRING, CMD_PRINT, L"Print...\tCtrl+P");
        AppendMenuW(m, MF_STRING, CMD_PROPERTIES, L"Document properties");
        ClientToScreen(hwnd, &pt);
        cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(m);
        if (cmd) app_command(cmd);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (tool_setcursor(pt)) return TRUE;
            if (g_cursor) { SetCursor(g_cursor); return TRUE; }
        }
        break;
    case WM_APP_RENDERED:
        return bridge_on_rendered(lp);
    case WM_APP_GONE:
        if (g.bridged) {
            g.bridged = FALSE;
            if (!g.npages) lstrcpynW(g.error, L"The PDF engine (sg-pdf) stopped.", 256);
            InvalidateRect(hwnd, NULL, FALSE);
            app_status_changed();
        }
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void view_register(void)
{
    WNDCLASSW wc = { 0 };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = view_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"SgPdfView";
    RegisterClassW(&wc);
    g_cursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
}
