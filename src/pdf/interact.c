/* sg-pdf -- SG PDF: the mouse and the keyboard on the pages, by tool.
 *
 *   Edit PDF      the page's text blocks, pictures and paths outlined under
 *                 the pointer; a click selects one (handles at its corners
 *                 and sides), a drag moves it, a handle resizes it, Del
 *                 deletes it; a double-click (or Enter) on text opens an
 *                 editor over the block -- the text reflows in the block's
 *                 width when it is committed (a click elsewhere, Ctrl+Enter;
 *                 Escape cancels). Add text: a click opens an editor there;
 *                 Add image: a click or a drag places the chosen picture.
 *   Comment       Highlight, Underline and Strikethrough: drag over text;
 *                 a sticky note: a click, then its text; a text box: a click
 *                 or a drag, then type; rectangle, oval, arrow, line: a
 *                 drag; free-form: draw. With Select, a click picks a
 *                 comment, a drag moves it, a double-click edits its text.
 *   Fill & Sign   (and with no tool) a click on a text field types in it, on
 *                 a check box or radio button sets it, on a list offers its
 *                 choices; Add text types anywhere; Sign places the
 *                 signature made in its dialog where the page is clicked.
 *   Redact        drag over text to mark it, or a box over anything; a
 *                 click picks a mark, Del removes it.
 *
 * Coordinates: points on the page as shown (its /Rotate applied), what
 * sg-pdf takes; view.c maps them to the window and back, the view's own
 * rotation included.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"

#define WM_APP_COMMIT (WM_APP + 10)

enum { DRAG_NONE, DRAG_ARMED, DRAG_MOVE, DRAG_RESIZE, DRAG_RECT, DRAG_LINE, DRAG_INK, DRAG_SELECT };
enum { ED_NONE, ED_OBJTEXT, ED_ADDTEXT, ED_FREETEXT, ED_FIELD };

static struct {
    int mode, page, handle;
    float x0, y0, x1, y1;       /* where it started, where it is (page points) */
    frect orig;                 /* the picked thing's box when it started */
    POINT down;
} D;

static float *g_ink;            /* the stroke being drawn: x, y pairs */
static int g_nink, g_capink;

static struct {
    HWND hwnd;
    int mode, page, obj_id, field_xref;
    frect box;                  /* page points */
    HFONT font;
    float size;
    WCHAR *orig;
    BOOL closing;
} E;

static int g_hover_page = -1, g_hover_obj = -1;
static WNDPROC g_editor_proc;

/* ---- geometry ------------------------------------------------------------------------------------------ */

static void to_client(int page, const frect *b, RECT *r)
{
    view_box_to_client(page, b, r);
}

static frect norm(float x0, float y0, float x1, float y1)
{
    frect r = { min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1) };
    return r;
}

static BOOL page_point(POINT pt, BOOL clamp, int *page, float *x, float *y)
{
    int p = view_page_at(pt, clamp);
    if (p < 0) return FALSE;
    view_client_to_page(p, pt.x, pt.y, x, y);
    *page = p;
    return TRUE;
}

static float clampf(float v, float a, float b)
{
    return v < a ? a : v > b ? b : v;
}

/* page size as shown by the engine (its /Rotate applied) */
static void page_size(int page, float *w, float *h)
{
    *w = (float)g.pages[page].w;
    *h = (float)g.pages[page].h;
}

static char *fmt_rect(char *buf, int cap, frect r)
{
    snprintf(buf, cap, "%.2f %.2f %.2f %.2f", r.x1, r.y1, r.x2, r.y2);
    return buf;
}

/* ---- picking ----------------------------------------------------------------------------------------------- */

static int obj_at(int page, POINT pt)
{
    page_t *p = &g.pages[page];
    RECT pr;
    int k, best = -1;
    double best_area = 1e30, page_area;
    if (!doc_load_objects(page)) return -1;
    view_page_rect(page, &pr);
    page_area = (double)(pr.right - pr.left) * (pr.bottom - pr.top);
    /* the smallest object that holds the point, whatever its kind (a line over
     * a picture, text on a coloured box); a path as big as the page (a
     * background) only when nothing else is there */
    for (k = 0; k < p->nobjs; k++) {
        RECT r;
        double area;
        to_client(page, &p->objs[k].box, &r);
        InflateRect(&r, dpx(3), dpx(3));
        if (!PtInRect(&r, pt)) continue;
        area = (double)(r.right - r.left) * (r.bottom - r.top);
        if (p->objs[k].kind == OBJ_PATH && area > 0.6 * page_area) area += page_area;
        if (area < best_area) { best_area = area; best = k; }
    }
    return best;
}

static int annot_at(int page, POINT pt, BOOL marks)
{
    page_t *p = &g.pages[page];
    int k, best = -1;
    double best_area = 1e30;
    if (!doc_load_annots(page)) return -1;
    for (k = 0; k < p->nannots; k++) {
        RECT r;
        double area;
        BOOL is_mark = !strcmp(p->annots[k].type, "Redact");
        if (is_mark != marks) continue;
        to_client(page, &p->annots[k].box, &r);
        InflateRect(&r, dpx(2), dpx(2));
        if (!PtInRect(&r, pt)) continue;
        area = (double)(r.right - r.left) * (r.bottom - r.top);
        if (area < best_area) { best_area = area; best = k; }
    }
    return best;
}

static int field_at(POINT pt)
{
    int i;
    for (i = 0; i < g.nfields; i++) {
        RECT r;
        to_client(g.fields[i].page, &g.fields[i].box, &r);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

static BOOL pick_box(frect *b)
{
    page_t *p;
    if (g.pick.kind == PICK_NONE || g.pick.page < 0 || g.pick.page >= g.npages) return FALSE;
    p = &g.pages[g.pick.page];
    if (g.pick.kind == PICK_OBJ && g.pick.index < p->nobjs) { *b = p->objs[g.pick.index].box; return TRUE; }
    if (g.pick.kind == PICK_ANNOT && g.pick.index < p->nannots) { *b = p->annots[g.pick.index].box; return TRUE; }
    return FALSE;
}

static void handle_rects(const RECT *r, RECT h[8])
{
    int s = dpx(4), cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2, i;
    POINT c[8] = { { r->left, r->top }, { cx, r->top }, { r->right, r->top }, { r->right, cy },
                   { r->right, r->bottom }, { cx, r->bottom }, { r->left, r->bottom }, { r->left, cy } };
    for (i = 0; i < 8; i++) SetRect(&h[i], c[i].x - s, c[i].y - s, c[i].x + s + 1, c[i].y + s + 1);
}

static int handle_at(POINT pt)
{
    frect b;
    RECT r, h[8];
    int i;
    if (g.tool != TOOL_EDIT || g.pick.kind != PICK_OBJ || !pick_box(&b)) return -1;
    to_client(g.pick.page, &b, &r);
    handle_rects(&r, h);
    for (i = 0; i < 8; i++) { InflateRect(&h[i], dpx(2), dpx(2)); if (PtInRect(&h[i], pt)) return i; }
    return -1;
}

/* the dragged box: moved by (dx, dy), or resized by a handle */
static frect dragged_box(int mode)
{
    frect b = D.orig;
    float dx = D.x1 - D.x0, dy = D.y1 - D.y0;
    if (mode == DRAG_MOVE) { b.x1 += dx; b.x2 += dx; b.y1 += dy; b.y2 += dy; return b; }
    switch (D.handle) {
    case 0: b.x1 += dx; b.y1 += dy; break;
    case 1: b.y1 += dy; break;
    case 2: b.x2 += dx; b.y1 += dy; break;
    case 3: b.x2 += dx; break;
    case 4: b.x2 += dx; b.y2 += dy; break;
    case 5: b.y2 += dy; break;
    case 6: b.x1 += dx; b.y2 += dy; break;
    case 7: b.x1 += dx; break;
    }
    return norm(b.x1, b.y1, b.x2, b.y2);
}

/* after a change the objects are new: pick the one of this kind nearest a box */
static void repick_obj(int page, int kind, frect want)
{
    page_t *p = &g.pages[page];
    int k, best = -1;
    float bestd = 1e30f;
    g.pick.kind = PICK_NONE;
    if (!doc_load_objects(page)) return;
    for (k = 0; k < p->nobjs; k++) {
        float d;
        if (p->objs[k].kind != kind) continue;
        d = fabsf(p->objs[k].box.x1 - want.x1) + fabsf(p->objs[k].box.y1 - want.y1);
        if (d < bestd) { bestd = d; best = k; }
    }
    if (best >= 0 && bestd < 40) { g.pick.kind = PICK_OBJ; g.pick.page = page; g.pick.index = best; }
    toolui_format_from_pick();
}

/* ---- the editor over the page --------------------------------------------------------------------------- */

BOOL tool_editor_open(void)
{
    return E.hwnd != NULL;
}

static void editor_place(void)
{
    RECT r;
    if (!E.hwnd) return;
    to_client(E.page, &E.box, &r);
    InflateRect(&r, dpx(2), dpx(2));
    if (E.mode != ED_FIELD) r.bottom = max(r.bottom, r.top + (int)(E.size * view_scale() * 1.6));
    SetWindowPos(E.hwnd, HWND_TOP, r.left, r.top, max(dpx(40), r.right - r.left), max(dpx(18), r.bottom - r.top),
                 SWP_NOACTIVATE);
}

static void editor_close(void)
{
    HWND h = E.hwnd;
    if (!h) return;
    E.hwnd = NULL;
    DestroyWindow(h);
    if (E.font) DeleteObject(E.font);
    E.font = NULL;
    free(E.orig);
    E.orig = NULL;
    E.mode = ED_NONE;
    InvalidateRect(g_view, NULL, FALSE);
}

static WCHAR *editor_text(void)
{
    int n = GetWindowTextLengthW(E.hwnd), i, k = 0;
    WCHAR *t = calloc(n + 1, sizeof(WCHAR));
    if (!t) return NULL;
    GetWindowTextW(E.hwnd, t, n + 1);
    for (i = 0; t[i]; i++) if (t[i] != '\r') t[k++] = t[i];
    t[k] = 0;
    return t;
}

void tool_commit_editor(void)
{
    WCHAR *t;
    char *et, rb[128];
    int mode = E.mode, page = E.page;
    frect box = E.box;
    if (!E.hwnd || E.closing) return;
    E.closing = TRUE;
    t = editor_text();
    et = t ? esc_utf8(t) : NULL;
    if (!t || !et) { free(t); free(et); E.closing = FALSE; editor_close(); return; }
    switch (mode) {
    case ED_OBJTEXT:
        if (!E.orig || wcscmp(t, E.orig)) {
            int id = E.obj_id;
            editor_close();
            if (doc_requestf("edittext\t%d\t%d\t-\t%s", page, id, et)) repick_obj(page, OBJ_TEXT, box);
        } else editor_close();
        break;
    case ED_ADDTEXT:
        editor_close();
        if (t[0]) {
            char *font = esc_utf8(g.fmt_font);
            if (font && doc_requestf("addtext\t%d\t%s\t%s\tfont=%s\tsize=%.2f\tcolor=%02X%02X%02X\tbold=%d\titalic=%d\talign=%d",
                                     page, fmt_rect(rb, sizeof(rb), box), et, font, g.fmt_size, GetRValue(g.fmt_color),
                                     GetGValue(g.fmt_color), GetBValue(g.fmt_color), g.fmt_style & 1, (g.fmt_style >> 1) & 1,
                                     g.fmt_align) && g.tool == TOOL_EDIT)
                repick_obj(page, OBJ_TEXT, box);
            free(font);
        }
        if (g.tool == TOOL_EDIT || g.tool == TOOL_FILL) g.sub = SUB_SELECT;
        break;
    case ED_FREETEXT:
        editor_close();
        if (t[0])
            doc_requestf("annot\t%d\tfreetext\trect=%s\ttext=%s\tfontsize=12\tcolor=%02X%02X%02X", page,
                         fmt_rect(rb, sizeof(rb), box), et,
                         GetRValue(g.ccolor == COMMENT_COLORS[0] ? RGB(0x10, 0x10, 0x10) : g.ccolor),
                         GetGValue(g.ccolor == COMMENT_COLORS[0] ? RGB(0x10, 0x10, 0x10) : g.ccolor),
                         GetBValue(g.ccolor == COMMENT_COLORS[0] ? RGB(0x10, 0x10, 0x10) : g.ccolor));
        g.sub = SUB_SELECT;
        break;
    case ED_FIELD: {
        int xref = E.field_xref;
        BOOL changed = !E.orig || wcscmp(t, E.orig);
        editor_close();
        if (changed) doc_requestf("setfield\t%d\t%d\t%s", page, xref, et);
        break;
    }
    default:
        editor_close();
    }
    free(t);
    free(et);
    E.closing = FALSE;
    toolui_update();
    app_status_changed();
}

static LRESULT CALLBACK editor_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { editor_close(); SetFocus(g_view); app_status_changed(); return 0; }
        if (wp == VK_RETURN && (GetKeyState(VK_CONTROL) < 0 || !(GetWindowLongW(hwnd, GWL_STYLE) & ES_MULTILINE))) {
            PostMessageW(g_view, WM_APP_COMMIT, 0, 0);
            return 0;
        }
        if (wp == VK_TAB && E.mode == ED_FIELD) { PostMessageW(g_view, WM_APP_COMMIT, 0, 0); return 0; }
        break;
    case WM_CHAR:
        if (wp == 27 || (wp == '\t' && E.mode == ED_FIELD)) return 0;
        if ((wp == '\r' || wp == '\n') && (GetKeyState(VK_CONTROL) < 0 || !(GetWindowLongW(hwnd, GWL_STYLE) & ES_MULTILINE))) return 0;
        break;
    case WM_KILLFOCUS:
        if (!E.closing) PostMessageW(g_view, WM_APP_COMMIT, 0, 0);
        break;
    }
    return CallWindowProcW(g_editor_proc, hwnd, msg, wp, lp);
}

static void editor_open(int mode, int page, frect box, const WCHAR *text, const WCHAR *face, float size, int style,
                        COLORREF color, BOOL multiline)
{
    int px;
    (void)color;
    tool_commit_editor();
    E.mode = mode;
    E.page = page;
    E.box = box;
    E.size = size > 0 ? size : 12;
    E.orig = text ? _wcsdup(text) : NULL;
    px = max(6, (int)(E.size * view_scale() + 0.5));
    E.font = CreateFontW(-px, 0, 0, 0, style & 1 ? FW_BOLD : FW_NORMAL, (style & 2) != 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                         CLEARTYPE_QUALITY, 0, face && face[0] ? face : L"Liberation Sans");
    E.hwnd = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER |
                             (multiline ? ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN : ES_AUTOHSCROLL),
                             0, 0, 10, 10, g_view, NULL, g_inst, NULL);
    SendMessageW(E.hwnd, WM_SETFONT, (WPARAM)E.font, FALSE);
    if (text) {
        /* CRLF for the edit control */
        size_t n = wcslen(text), i, k = 0;
        WCHAR *t = calloc(n * 2 + 1, sizeof(WCHAR));
        for (i = 0; t && i < n; i++) { if (text[i] == '\n') t[k++] = '\r'; t[k++] = text[i]; }
        if (t) { SetWindowTextW(E.hwnd, t); free(t); }
    }
    g_editor_proc = (WNDPROC)SetWindowLongPtrW(E.hwnd, GWLP_WNDPROC, (LONG_PTR)editor_proc);
    editor_place();
    SetFocus(E.hwnd);
    SendMessageW(E.hwnd, EM_SETSEL, 0, -1);
    app_status_changed();
}

static void edit_obj_text(int page, int k)
{
    obj_t *o = &g.pages[page].objs[k];
    frect b = o->box;
    if (o->kind != OBJ_TEXT) return;
    g.pick.kind = PICK_OBJ;
    g.pick.page = page;
    g.pick.index = k;
    toolui_format_from_pick();
    E.obj_id = o->id;
    editor_open(ED_OBJTEXT, page, b, o->text, o->font, o->size, o->style, o->color, TRUE);
}

/* ---- requests ------------------------------------------------------------------------------------------- */

static void rgb_hex(COLORREF c, char out[8])
{
    snprintf(out, 8, "%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
}

static BOOL rects_of_selection(char *out, int cap, int page)
{
    frect r[512];
    int n = view_selection_rects(page, r, 512), i, k = 0;
    out[0] = 0;
    for (i = 0; i < n && k < cap - 64; i++)
        k += snprintf(out + k, cap - k, "%s%.2f %.2f %.2f %.2f", i ? ";" : "", r[i].x1, r[i].y1, r[i].x2, r[i].y2);
    return n > 0;
}

/* the selection made by dragging: a comment of each page's part, or redaction marks */
static void selection_to(const char *what)
{
    static char rects[512 * 48];
    char col[8];
    int pg, first = g.sel_a.page < g.sel_b.page ? g.sel_a.page : g.sel_b.page, last = max(g.sel_a.page, g.sel_b.page);
    if (!view_has_selection()) return;
    rgb_hex(g.ccolor, col);
    for (pg = first; pg <= last && pg >= 0 && pg < g.npages; pg++) {
        if (!rects_of_selection(rects, sizeof(rects), pg)) continue;
        if (!strcmp(what, "redact")) doc_requestf("redactmark\t%d\trects=%s", pg, rects);
        else doc_requestf("annot\t%d\t%s\trects=%s\tcolor=%s", pg, what, rects, col);
    }
    view_clear_selection();
}

static void new_rect_request(int page, frect r)
{
    char rb[128], col[8];
    float w, h;
    page_size(page, &w, &h);
    r.x1 = clampf(r.x1, 0, w); r.x2 = clampf(r.x2, 0, w);
    r.y1 = clampf(r.y1, 0, h); r.y2 = clampf(r.y2, 0, h);
    rgb_hex(g.ccolor, col);
    fmt_rect(rb, sizeof(rb), r);
    switch (g.tool == TOOL_REDACT ? SUB_MARKAREA : g.sub) {
    case SUB_MARKAREA: doc_requestf("redactmark\t%d\trects=%s", page, rb); break;
    case SUB_RECT: doc_requestf("annot\t%d\trect\trect=%s\tcolor=%s\twidth=2", page, rb, col); break;
    case SUB_ELLIPSE: doc_requestf("annot\t%d\tellipse\trect=%s\tcolor=%s\twidth=2", page, rb, col); break;
    case SUB_ADDIMAGE: {
        char *u = unix_path(g.image_path);
        WCHAR *wu = u ? from_utf8(u, -1) : NULL;
        char *e = wu ? esc_utf8(wu) : NULL;
        if (e && doc_requestf("addimage\t%d\t%s\t%s", page, rb, e)) repick_obj(page, OBJ_IMAGE, r);
        free(e); free(wu); free(u);
        g.sub = SUB_SELECT;
        break;
    }
    case SUB_SIGN:
        if (g.sig_kind && g.sig_data) {
            static const char *KIND[] = { "", "ink", "text", "image" };
            WCHAR *data = g.sig_data;
            char *e = NULL;
            if (g.sig_kind == 3) {
                char *u = unix_path(g.sig_data);
                WCHAR *wu = u ? from_utf8(u, -1) : NULL;
                e = wu ? esc_utf8(wu) : NULL;
                free(wu); free(u);
            } else e = esc_utf8(data);
            if (e) doc_requestf("signature\t%d\t%s\t%s\t%s", page, rb, KIND[g.sig_kind], e);
            free(e);
        }
        g.sub = SUB_SELECT;
        break;
    }
    toolui_update();
}

static void set_field(int i, const WCHAR *value)
{
    char *e = esc_utf8(value);
    if (e) doc_requestf("setfield\t%d\t%d\t%s", g.fields[i].page, g.fields[i].xref, e);
    free(e);
}

static void click_field(int i)
{
    field_t *f = &g.fields[i];
    if (f->flags & 1) { app_set_status(L"This field is read-only."); return; }
    switch (f->type) {
    case FLD_TEXT: {
        float size = f->fontsize > 0 ? f->fontsize : min(12.0f, max(6.0f, (f->box.y2 - f->box.y1) * 0.65f));
        E.field_xref = f->xref;
        editor_open(ED_FIELD, f->page, f->box, f->value, L"Liberation Sans", size, 0, 0, (f->flags & 4096) != 0);
        break;
    }
    case FLD_CHECK:
        set_field(i, f->value && !wcscmp(f->value, L"1") ? L"0" : L"1");
        break;
    case FLD_RADIO:
        if (!f->value || wcscmp(f->value, L"1")) set_field(i, L"1");
        break;
    case FLD_COMBO: case FLD_LIST: {
        HMENU m = CreatePopupMenu();
        WCHAR *opts = f->options ? _wcsdup(f->options) : NULL, *s, *ctx = NULL;
        WCHAR *items[128];
        int n = 0, cmd;
        RECT r;
        for (s = opts ? wcstok_s(opts, L"\n", &ctx) : NULL; s && n < 128; s = wcstok_s(NULL, L"\n", &ctx)) {
            items[n] = s;
            AppendMenuW(m, MF_STRING | (f->value && !wcscmp(f->value, s) ? MF_CHECKED : 0), n + 1, s);
            n++;
        }
        to_client(f->page, &f->box, &r);
        MapWindowPoints(g_view, NULL, (POINT *)&r, 2);
        cmd = n ? TrackPopupMenu(m, TPM_RETURNCMD, r.left, r.bottom, 0, g_main, NULL) : 0;
        DestroyMenu(m);
        if (cmd > 0 && cmd <= n) set_field(i, items[cmd - 1]);
        free(opts);
        break;
    }
    default:
        app_set_status(L"This kind of field cannot be filled here.");
    }
}

/* ---- the mouse ----------------------------------------------------------------------------------------------- */

static BOOL fields_live(void)
{
    return g.form && (g.tool == TOOL_NONE || (g.tool == TOOL_FILL && g.sub == SUB_SELECT));
}

static BOOL rect_sub(void)
{
    if (g.tool == TOOL_REDACT) return g.sub == SUB_MARKAREA || g.sub == SUB_MARKTEXT;
    return g.sub == SUB_RECT || g.sub == SUB_ELLIPSE || g.sub == SUB_ADDIMAGE || g.sub == SUB_SIGN;
}

static BOOL text_sub(void)
{
    return (g.tool == TOOL_COMMENT && (g.sub == SUB_HIGHLIGHT || g.sub == SUB_UNDERLINE || g.sub == SUB_STRIKE)) ||
           (g.tool == TOOL_REDACT && g.sub == SUB_MARKTEXT);
}

static void context_menu(POINT pt)
{
    HMENU m = CreatePopupMenu();
    int cmd, page, k;
    POINT sp = pt;
    page = view_page_at(pt, FALSE);
    if (page < 0) { DestroyMenu(m); return; }
    if (g.tool == TOOL_EDIT && (k = obj_at(page, pt)) >= 0) {
        obj_t *o = &g.pages[page].objs[k];
        g.pick.kind = PICK_OBJ; g.pick.page = page; g.pick.index = k;
        toolui_format_from_pick();
        if (o->kind == OBJ_TEXT) AppendMenuW(m, MF_STRING, 1, L"&Edit Text");
        if (o->kind == OBJ_IMAGE) AppendMenuW(m, MF_STRING, 2, L"&Replace Image...");
        AppendMenuW(m, MF_STRING, 3, L"&Delete");
    } else if ((g.tool == TOOL_COMMENT && (k = annot_at(page, pt, FALSE)) >= 0) ||
               (g.tool == TOOL_REDACT && (k = annot_at(page, pt, TRUE)) >= 0)) {
        g.pick.kind = PICK_ANNOT; g.pick.page = page; g.pick.index = k;
        if (g.tool == TOOL_COMMENT) AppendMenuW(m, MF_STRING, 4, L"&Open Note...");
        AppendMenuW(m, MF_STRING, 3, g.tool == TOOL_REDACT ? L"&Remove Mark" : L"&Delete");
    } else {
        DestroyMenu(m);
        return;
    }
    InvalidateRect(g_view, NULL, FALSE);
    ClientToScreen(g_view, &sp);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, sp.x, sp.y, 0, g_main, NULL);
    DestroyMenu(m);
    if (cmd == 1) edit_obj_text(g.pick.page, g.pick.index);
    else if (cmd == 2) app_command(CMD_REPLACEIMAGE);
    else if (cmd == 3) tool_delete_pick();
    else if (cmd == 4) {
        annot_t *a = &g.pages[g.pick.page].annots[g.pick.index];
        WCHAR buf[4096];
        lstrcpynW(buf, a->contents ? a->contents : L"", 4096);
        if (dlg_text(g_main, L"Note", L"Text of the comment:", buf, 4096, TRUE)) {
            char *e = esc_utf8(buf);
            if (e) doc_requestf("setannot\t%d\t%d\ttext=%s", g.pick.page, a->xref, e);
            free(e);
        }
    }
    app_status_changed();
}

BOOL tool_mouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    int page, k;
    float x, y;
    (void)wp;
    if (!g.npages || !g.bridged) return FALSE;

    if (msg == WM_RBUTTONUP) {
        if (g.tool == TOOL_EDIT || g.tool == TOOL_COMMENT || g.tool == TOOL_REDACT) {
            int before = g.pick.kind;
            context_menu(pt);
            return before != PICK_NONE || g.pick.kind != PICK_NONE;
        }
        return FALSE;
    }

    if (msg == WM_LBUTTONDOWN) {
        if (E.hwnd) {
            RECT er;
            GetWindowRect(E.hwnd, &er);
            MapWindowPoints(NULL, g_view, (POINT *)&er, 2);
            if (!PtInRect(&er, pt)) { tool_commit_editor(); return TRUE; }
            return FALSE;
        }
        SetFocus(hwnd);
        D.mode = DRAG_NONE;
        D.down = pt;
        /* forms: in plain viewing and in Fill & Sign */
        if (fields_live() && (k = field_at(pt)) >= 0) { click_field(k); return TRUE; }
        if (g.tool == TOOL_NONE || g.tool == TOOL_ORGANIZE) return FALSE;
        if (text_sub() && view_over_text(pt)) return FALSE;     /* the view selects text */
        if (!page_point(pt, FALSE, &page, &x, &y)) {
            if (g.pick.kind != PICK_NONE) { g.pick.kind = PICK_NONE; InvalidateRect(hwnd, NULL, FALSE); toolui_update(); }
            return TRUE;
        }
        D.page = page;
        D.x0 = D.x1 = x;
        D.y0 = D.y1 = y;
        SetCapture(hwnd);
        if (g.tool == TOOL_EDIT && g.sub == SUB_SELECT) {
            int h = handle_at(pt);
            if (h >= 0) { D.mode = DRAG_RESIZE; D.handle = h; pick_box(&D.orig); page = g.pick.page; D.page = page; return TRUE; }
            k = obj_at(page, pt);
            if (k >= 0) {
                g.pick.kind = PICK_OBJ; g.pick.page = page; g.pick.index = k;
                D.orig = g.pages[page].objs[k].box;
                D.mode = DRAG_ARMED;
                toolui_format_from_pick();
            } else g.pick.kind = PICK_NONE;
            toolui_update();
            InvalidateRect(hwnd, NULL, FALSE);
            return TRUE;
        }
        if (g.tool == TOOL_COMMENT && g.sub == SUB_SELECT) {
            k = annot_at(page, pt, FALSE);
            if (k >= 0) {
                g.pick.kind = PICK_ANNOT; g.pick.page = page; g.pick.index = k;
                D.orig = g.pages[page].annots[k].box;
                D.mode = DRAG_ARMED;
            } else g.pick.kind = PICK_NONE;
            toolui_update();
            InvalidateRect(hwnd, NULL, FALSE);
            return TRUE;
        }
        if (g.tool == TOOL_REDACT && (k = annot_at(page, pt, TRUE)) >= 0 && !view_over_text(pt)) {
            /* a click on a mark picks it; a drag from it still marks */
            g.pick.kind = PICK_ANNOT; g.pick.page = page; g.pick.index = k;
            D.mode = DRAG_RECT;
            InvalidateRect(hwnd, NULL, FALSE);
            toolui_update();
            return TRUE;
        }
        if (g.tool == TOOL_REDACT) g.pick.kind = PICK_NONE;
        if (rect_sub() || g.sub == SUB_FREETEXT) { D.mode = DRAG_RECT; return TRUE; }
        if (g.sub == SUB_LINE || g.sub == SUB_ARROW) { D.mode = DRAG_LINE; return TRUE; }
        if (g.sub == SUB_INK) {
            D.mode = DRAG_INK;
            g_nink = 0;
            if (!g_ink) { g_capink = 4096; g_ink = malloc(g_capink * sizeof(float)); }
            if (g_ink) { g_ink[g_nink++] = x; g_ink[g_nink++] = y; }
            return TRUE;
        }
        if (g.sub == SUB_ADDTEXT || g.sub == SUB_FILLTEXT) {
            float w, h, size = g.fmt_size > 0 ? g.fmt_size : 12;
            frect b;
            page_size(page, &w, &h);
            b.x1 = x; b.y1 = y - size * 0.8f;
            b.x2 = min(w - 4, x + max(160.0f, size * 14)); b.y2 = b.y1 + size * 1.3f;
            ReleaseCapture();
            if (g.tool == TOOL_FILL) {
                editor_open(ED_ADDTEXT, page, b, NULL, L"Liberation Sans", size, 0, 0, TRUE);
            } else editor_open(ED_ADDTEXT, page, b, NULL, g.fmt_font, size, g.fmt_style, g.fmt_color, TRUE);
            return TRUE;
        }
        if (g.sub == SUB_NOTE) {
            WCHAR buf[4096] = L"";
            char col[8];
            ReleaseCapture();
            if (dlg_text(g_main, L"Sticky Note", L"Note:", buf, 4096, TRUE)) {
                char *e = esc_utf8(buf);
                rgb_hex(g.ccolor, col);
                if (e) doc_requestf("annot\t%d\tnote\tpoint=%.2f %.2f\ttext=%s\tcolor=%s", page, x, y, e, col);
                free(e);
            }
            g.sub = SUB_SELECT;
            toolui_update();
            return TRUE;
        }
        ReleaseCapture();
        return TRUE;
    }

    if (msg == WM_MOUSEMOVE) {
        if (D.mode == DRAG_NONE || GetCapture() != hwnd) {
            /* hover: Edit PDF outlines what is under the pointer */
            if (g.tool == TOOL_EDIT && g.sub == SUB_SELECT && !E.hwnd) {
                int hp = view_page_at(pt, FALSE), ho = hp >= 0 ? obj_at(hp, pt) : -1;
                if (hp != g_hover_page || ho != g_hover_obj) {
                    g_hover_page = hp;
                    g_hover_obj = ho;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            if (g.tool != TOOL_NONE && g.tool != TOOL_ORGANIZE && !text_sub() && GetCapture() != hwnd) {
                tool_setcursor(pt);
                return TRUE;
            }
            return FALSE;
        }
        view_client_to_page(D.page, pt.x, pt.y, &x, &y);
        {
            float w, h;
            page_size(D.page, &w, &h);
            D.x1 = clampf(x, 0, w);
            D.y1 = clampf(y, 0, h);
        }
        if (D.mode == DRAG_ARMED && abs(pt.x - D.down.x) + abs(pt.y - D.down.y) > dpx(3)) D.mode = DRAG_MOVE;
        if (D.mode == DRAG_INK && g_ink) {
            if (g_nink + 2 > g_capink) { float *t = realloc(g_ink, (g_capink *= 2) * sizeof(float)); if (t) g_ink = t; }
            if (g_nink + 2 <= g_capink) { g_ink[g_nink++] = D.x1; g_ink[g_nink++] = D.y1; }
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return TRUE;
    }

    if (msg == WM_LBUTTONUP) {
        int mode = D.mode;
        char rb[128], col[8];
        BOOL small = abs(pt.x - D.down.x) + abs(pt.y - D.down.y) <= dpx(3);
        if (text_sub() && GetCapture() == hwnd && mode == DRAG_NONE) {
            /* the view's text selection has ended: comment or mark it */
            KillTimer(hwnd, 1);
            ReleaseCapture();
            g.selecting = FALSE;
            if (view_has_selection()) {
                if (g.tool == TOOL_REDACT) selection_to("redact");
                else selection_to(g.sub == SUB_HIGHLIGHT ? "highlight" : g.sub == SUB_UNDERLINE ? "underline" : "strikeout");
            }
            app_status_changed();
            return TRUE;
        }
        if (mode == DRAG_NONE) return FALSE;
        D.mode = DRAG_NONE;
        if (GetCapture() == hwnd) ReleaseCapture();
        rgb_hex(g.ccolor, col);
        switch (mode) {
        case DRAG_ARMED:
            break;
        case DRAG_MOVE: case DRAG_RESIZE: {
            frect b = dragged_box(mode);
            if (g.pick.kind == PICK_OBJ) {
                obj_t *o = &g.pages[g.pick.page].objs[g.pick.index];
                int kind = o->kind;
                if (b.x2 - b.x1 < 2 || b.y2 - b.y1 < 1) break;
                if (doc_requestf("moveobj\t%d\t%d\t%s", g.pick.page, o->id, fmt_rect(rb, sizeof(rb), b)))
                    repick_obj(D.page, kind, b);
            } else if (g.pick.kind == PICK_ANNOT) {
                annot_t *a = &g.pages[g.pick.page].annots[g.pick.index];
                int pg = g.pick.page;
                if (doc_requestf("moveannot\t%d\t%d\t%s", pg, a->xref, fmt_rect(rb, sizeof(rb), b))) {
                    doc_load_annots(pg);
                    g.pick.kind = PICK_NONE;
                }
            }
            break;
        }
        case DRAG_RECT: {
            frect r = norm(D.x0, D.y0, D.x1, D.y1);
            if (g.tool == TOOL_REDACT && small && g.pick.kind == PICK_ANNOT) break;   /* a click picked a mark */
            if (small) {
                /* a click: a box of a useful size at the point */
                float w = 180, h = 40;
                if (g.sub == SUB_ADDIMAGE) { w = 200; h = 150; }
                else if (g.sub == SUB_SIGN) { w = 200; h = 60; }
                else if (g.tool == TOOL_REDACT) break;
                else if (g.sub == SUB_RECT || g.sub == SUB_ELLIPSE) { w = 100; h = 60; }
                r.x1 = D.x0; r.y1 = D.y0; r.x2 = D.x0 + w; r.y2 = D.y0 + h;
            }
            if (g.sub == SUB_FREETEXT) {
                if (r.y2 - r.y1 < 20) r.y2 = r.y1 + 24;
                editor_open(ED_FREETEXT, D.page, r, NULL, L"Liberation Sans", 12, 0, 0, TRUE);
            } else new_rect_request(D.page, r);
            break;
        }
        case DRAG_LINE:
            if (!small)
                doc_requestf("annot\t%d\t%s\tline=%.2f %.2f %.2f %.2f\tcolor=%s\twidth=2", D.page,
                             g.sub == SUB_ARROW ? "arrow" : "line", D.x0, D.y0, D.x1, D.y1, col);
            break;
        case DRAG_INK:
            if (g_ink && g_nink >= 4) {
                char *buf = malloc(g_nink * 16 + 64);
                int i, n = 0;
                if (buf) {
                    for (i = 0; i + 1 < g_nink; i += 2) n += sprintf(buf + n, "%s%.2f %.2f", i ? " " : "", g_ink[i], g_ink[i + 1]);
                    doc_requestf("annot\t%d\tink\tink=%s\tcolor=%s\twidth=2", D.page, buf, col);
                    free(buf);
                }
            }
            g_nink = 0;
            break;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        toolui_update();
        app_status_changed();
        return TRUE;
    }

    if (msg == WM_LBUTTONDBLCLK) {
        if (g.tool == TOOL_EDIT && page_point(pt, FALSE, &page, &x, &y) && (k = obj_at(page, pt)) >= 0 &&
            g.pages[page].objs[k].kind == OBJ_TEXT) {
            D.mode = DRAG_NONE;
            if (GetCapture() == hwnd) ReleaseCapture();
            edit_obj_text(page, k);
            return TRUE;
        }
        if (g.tool == TOOL_COMMENT && page_point(pt, FALSE, &page, &x, &y) && (k = annot_at(page, pt, FALSE)) >= 0) {
            annot_t *a = &g.pages[page].annots[k];
            WCHAR buf[4096];
            D.mode = DRAG_NONE;
            if (GetCapture() == hwnd) ReleaseCapture();
            lstrcpynW(buf, a->contents ? a->contents : L"", 4096);
            if (dlg_text(g_main, L"Note", L"Text of the comment:", buf, 4096, TRUE)) {
                char *e = esc_utf8(buf);
                if (e) doc_requestf("setannot\t%d\t%d\ttext=%s", page, a->xref, e);
                free(e);
            }
            return TRUE;
        }
        if (g.tool != TOOL_NONE && g.tool != TOOL_ORGANIZE && !text_sub()) return TRUE;
        return FALSE;
    }
    return FALSE;
}

BOOL tool_setcursor(POINT pt)
{
    int page, h;
    LPCWSTR c = NULL;
    if (!g.npages) return FALSE;
    page = view_page_at(pt, FALSE);
    if (fields_live() && field_at(pt) >= 0) c = (LPCWSTR)IDC_HAND;
    else if (page < 0 || g.tool == TOOL_NONE || g.tool == TOOL_ORGANIZE) return c ? (SetCursor(LoadCursorW(NULL, c)), TRUE) : FALSE;
    else if (g.tool == TOOL_EDIT && g.sub == SUB_SELECT) {
        h = handle_at(pt);
        if (h >= 0) c = (LPCWSTR)(h == 0 || h == 4 ? IDC_SIZENWSE : h == 2 || h == 6 ? IDC_SIZENESW : h == 1 || h == 5 ? IDC_SIZENS : IDC_SIZEWE);
        else if (obj_at(page, pt) >= 0) c = (LPCWSTR)IDC_SIZEALL;
        else c = (LPCWSTR)IDC_ARROW;
    } else if (g.sub == SUB_ADDTEXT || g.sub == SUB_FILLTEXT) c = (LPCWSTR)IDC_IBEAM;
    else if (text_sub()) c = view_over_text(pt) ? (LPCWSTR)IDC_IBEAM : (LPCWSTR)IDC_CROSS;
    else if (g.sub == SUB_SELECT) c = (LPCWSTR)IDC_ARROW;
    else c = (LPCWSTR)IDC_CROSS;
    SetCursor(LoadCursorW(NULL, c));
    return TRUE;
}

/* ---- the keyboard ------------------------------------------------------------------------------------------------ */

void tool_delete_pick(void)
{
    page_t *p;
    if (g.pick.kind == PICK_NONE || g.pick.page < 0 || g.pick.page >= g.npages) return;
    p = &g.pages[g.pick.page];
    if (g.pick.kind == PICK_OBJ && g.pick.index < p->nobjs) {
        int page = g.pick.page, id = p->objs[g.pick.index].id;
        g.pick.kind = PICK_NONE;
        doc_requestf("delobj\t%d\t%d", page, id);
    } else if (g.pick.kind == PICK_ANNOT && g.pick.index < p->nannots) {
        int page = g.pick.page, xref = p->annots[g.pick.index].xref;
        g.pick.kind = PICK_NONE;
        doc_requestf("delannot\t%d\t%d", page, xref);
    }
    toolui_update();
    app_status_changed();
}

BOOL tool_key(WPARAM vk)
{
    if (!g.npages) return FALSE;
    if (vk == VK_DELETE && g.pick.kind != PICK_NONE) { tool_delete_pick(); return TRUE; }
    if (vk == VK_ESCAPE && (g.pick.kind != PICK_NONE || D.mode != DRAG_NONE || g.sub != (g.tool == TOOL_REDACT ? SUB_MARKTEXT : SUB_SELECT))) {
        tool_cancel();
        g.pick.kind = PICK_NONE;
        if (g.tool != TOOL_NONE) g.sub = g.tool == TOOL_REDACT ? SUB_MARKTEXT : SUB_SELECT;
        toolui_update();
        InvalidateRect(g_view, NULL, FALSE);
        app_status_changed();
        return g.tool != TOOL_NONE;
    }
    if (vk == VK_RETURN && g.tool == TOOL_EDIT && g.pick.kind == PICK_OBJ && g.pick.page < g.npages &&
        g.pick.index < g.pages[g.pick.page].nobjs && g.pages[g.pick.page].objs[g.pick.index].kind == OBJ_TEXT) {
        edit_obj_text(g.pick.page, g.pick.index);
        return TRUE;
    }
    return FALSE;
}

void tool_cancel(void)
{
    if (D.mode != DRAG_NONE && GetCapture() == g_view) ReleaseCapture();
    D.mode = DRAG_NONE;
    g_nink = 0;
    if (E.hwnd) editor_close();
}

void tool_after_change(void)
{
    /* the document changed under the tool: what was read from it is gone */
    if (E.hwnd && !E.closing) editor_close();
    g.pick.kind = PICK_NONE;
    g_hover_page = g_hover_obj = -1;
    if (D.mode != DRAG_NONE && GetCapture() == g_view) ReleaseCapture();
    D.mode = DRAG_NONE;
}

/* ---- drawing --------------------------------------------------------------------------------------------------- */

static void frame(HDC dc, RECT r, COLORREF c, int style, int width)
{
    LOGBRUSH lb = { BS_SOLID, c, 0 };
    HPEN pen = style == PS_SOLID ? CreatePen(PS_SOLID, width, c) : ExtCreatePen(PS_GEOMETRIC | style, width, &lb, 0, NULL);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

void fields_paint(HDC dc, int page)
{
    int i;
    HBRUSH b;
    if (!g.form || !g.nfields || (g.tool != TOOL_NONE && g.tool != TOOL_FILL)) return;
    b = CreateSolidBrush(C_FIELD);
    for (i = 0; i < g.nfields; i++) {
        RECT r;
        HGDIOBJ old;
        if (g.fields[i].page != page || (g.fields[i].flags & 1)) continue;
        to_client(page, &g.fields[i].box, &r);
        old = SelectObject(dc, b);
        PatBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0x00A000C9 /* DPa */);
        SelectObject(dc, old);
    }
    DeleteObject(b);
}

void tool_paint(HDC dc)
{
    frect b;
    RECT r, h[8];
    int i;
    HBRUSH white = GetStockObject(WHITE_BRUSH), acc = CreateSolidBrush(C_ACCENT);
    COLORREF accent = RGB(112, 48, 192);
    if (E.hwnd) editor_place();
    if (g.tool == TOOL_EDIT && g.npages) {
        /* every object faintly while the tool is open is too busy: the hovered one */
        if (g_hover_page >= 0 && g_hover_page < g.npages && g_hover_obj >= 0 && g_hover_obj < g.pages[g_hover_page].nobjs &&
            !(g.pick.kind == PICK_OBJ && g.pick.page == g_hover_page && g.pick.index == g_hover_obj)) {
            to_client(g_hover_page, &g.pages[g_hover_page].objs[g_hover_obj].box, &r);
            InflateRect(&r, dpx(2), dpx(2));
            frame(dc, r, RGB(0x2E, 0x7D, 0xF6), PS_DOT, 1);
        }
    }
    if (pick_box(&b)) {
        BOOL mark = g.pick.kind == PICK_ANNOT && !strcmp(g.pages[g.pick.page].annots[g.pick.index].type, "Redact");
        to_client(g.pick.page, &b, &r);
        InflateRect(&r, dpx(2), dpx(2));
        frame(dc, r, mark ? C_REDMARK : accent, PS_SOLID, max(1, dpx(1)));
        if (g.tool == TOOL_EDIT && g.pick.kind == PICK_OBJ) {
            handle_rects(&r, h);
            for (i = 0; i < 8; i++) { FillRect(dc, &h[i], acc); InflateRect(&h[i], -1, -1); FillRect(dc, &h[i], white); }
        }
    }
    if (D.mode == DRAG_MOVE || D.mode == DRAG_RESIZE) {
        frect nb = dragged_box(D.mode);
        to_client(g.pick.kind != PICK_NONE ? g.pick.page : D.page, &nb, &r);
        frame(dc, r, accent, PS_DASH, 1);
    } else if (D.mode == DRAG_RECT) {
        frect nb = norm(D.x0, D.y0, D.x1, D.y1);
        COLORREF c = g.tool == TOOL_REDACT ? C_REDMARK : g.tool == TOOL_COMMENT ? g.ccolor : accent;
        to_client(D.page, &nb, &r);
        if (g.sub == SUB_ELLIPSE) {
            HPEN pen = CreatePen(PS_SOLID, max(1, dpx(2)), c);
            HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, r.left, r.top, r.right, r.bottom);
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen);
        } else frame(dc, r, c, g.tool == TOOL_COMMENT ? PS_SOLID : PS_DASH, g.tool == TOOL_COMMENT ? max(1, dpx(2)) : 1);
    } else if (D.mode == DRAG_LINE) {
        POINT a, c2;
        HPEN pen = CreatePen(PS_SOLID, max(1, dpx(2)), g.ccolor);
        HGDIOBJ op = SelectObject(dc, pen);
        view_page_to_client(D.page, D.x0, D.y0, &a);
        view_page_to_client(D.page, D.x1, D.y1, &c2);
        MoveToEx(dc, a.x, a.y, NULL);
        LineTo(dc, c2.x, c2.y);
        SelectObject(dc, op);
        DeleteObject(pen);
    } else if (D.mode == DRAG_INK && g_ink && g_nink >= 4) {
        HPEN pen = CreatePen(PS_SOLID, max(1, dpx(2)), g.ccolor);
        HGDIOBJ op = SelectObject(dc, pen);
        POINT a;
        view_page_to_client(D.page, g_ink[0], g_ink[1], &a);
        MoveToEx(dc, a.x, a.y, NULL);
        for (i = 2; i + 1 < g_nink; i += 2) { view_page_to_client(D.page, g_ink[i], g_ink[i + 1], &a); LineTo(dc, a.x, a.y); }
        SelectObject(dc, op);
        DeleteObject(pen);
    }
    DeleteObject(acc);
}

/* ---- the dump ------------------------------------------------------------------------------------------------------- */

static void dump_text(FILE *f, const WCHAR *t)
{
    char buf[256];
    WCHAR w[64];
    int i;
    lstrcpynW(w, t ? t : L"", 64);
    for (i = 0; w[i]; i++) if (w[i] == '\n' || w[i] == '\r' || w[i] == '\t') w[i] = ' ';
    to_utf8(w, buf, sizeof(buf));
    fputs(buf, f);
}

void tool_dump(FILE *f)
{
    int i, k;
    RECT vr;
    GetClientRect(g_view, &vr);
    if (g.pick.kind != PICK_NONE) fprintf(f, "pick %s %d %d\n", g.pick.kind == PICK_OBJ ? "obj" : "annot", g.pick.page + 1, g.pick.index);
    else fprintf(f, "pick none\n");
    if (E.hwnd) {
        RECT r;
        GetWindowRect(E.hwnd, &r);
        fprintf(f, "editor %ld %ld %ld %ld %d\n", r.left, r.top, r.right, r.bottom, E.mode);
    }
    if (!IsWindowVisible(g_view)) return;
    for (i = 0; i < g.npages; i++) {
        RECT pr;
        page_t *p = &g.pages[i];
        view_page_rect(i, &pr);
        if (pr.bottom < 0 || pr.top > vr.bottom) continue;
        if (g.tool == TOOL_EDIT && doc_load_objects(i)) {
            for (k = 0; k < p->nobjs; k++) {
                RECT r;
                to_client(i, &p->objs[k].box, &r);
                MapWindowPoints(g_view, NULL, (POINT *)&r, 2);
                fprintf(f, "obj %d %d %s %ld %ld %ld %ld ", i + 1, k, p->objs[k].kind == OBJ_TEXT ? "text" : p->objs[k].kind == OBJ_IMAGE ? "image" : "path",
                        r.left, r.top, r.right, r.bottom);
                dump_text(f, p->objs[k].text);
                fputc('\n', f);
            }
        }
        if ((g.tool == TOOL_COMMENT || g.tool == TOOL_REDACT) && doc_load_annots(i)) {
            for (k = 0; k < p->nannots; k++) {
                RECT r;
                to_client(i, &p->annots[k].box, &r);
                MapWindowPoints(g_view, NULL, (POINT *)&r, 2);
                fprintf(f, "annot %d %d %s %ld %ld %ld %ld ", i + 1, p->annots[k].xref, p->annots[k].type, r.left, r.top, r.right, r.bottom);
                dump_text(f, p->annots[k].contents);
                fputc('\n', f);
            }
        }
    }
    for (i = 0; i < g.nfields; i++) {
        RECT r;
        char name[256];
        to_client(g.fields[i].page, &g.fields[i].box, &r);
        MapWindowPoints(g_view, NULL, (POINT *)&r, 2);
        to_utf8(g.fields[i].name ? g.fields[i].name : L"", name, sizeof(name));
        fprintf(f, "field %d %d %d %ld %ld %ld %ld %s=", g.fields[i].page + 1, g.fields[i].xref, g.fields[i].type,
                r.left, r.top, r.right, r.bottom, name);
        dump_text(f, g.fields[i].value);
        fputc('\n', f);
    }
}
