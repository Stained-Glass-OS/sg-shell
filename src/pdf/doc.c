/* sg-pdf -- SG PDF: the document's changes.
 *
 * Every edit is one request to sg-pdf (MuPDF), which changes the document it
 * holds and answers with the document's new state: its pages' sizes, undo
 * and redo depth, unsaved changes, security and redaction marks. Here that
 * answer is applied: pages are laid out again when their number or sizes
 * changed, what was shown stays on the screen until it is drawn again
 * (stale), and everything read from the old document -- text, links,
 * objects, comments, fields, bookmarks -- is read again when next needed.
 *
 * Saving is sg-pdf's full rewrite of the file (no incremental update, no
 * unreferenced objects: what an edit or a redaction removed is gone from
 * the file), to the same name or another; with redaction marks not yet
 * applied it asks first.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"

static int split_tabs(char *s, char **f, int max)
{
    int n = 0;
    while (n < max) {
        f[n++] = s;
        s = strchr(s, '\t');
        if (!s) break;
        *s++ = 0;
    }
    return n;
}

static COLORREF hex_color(const char *s)
{
    unsigned v = 0;
    if (!s || *s == '-' || sscanf(s, "%x", &v) != 1) return RGB(0, 0, 0);
    return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
}

static BOOL parse_rect(const char *s, frect *r)
{
    return sscanf(s, "%f %f %f %f", &r->x1, &r->y1, &r->x2, &r->y2) == 4;
}

void doc_free_page_cache(page_t *p)
{
    int k;
    free(p->text);
    free(p->boxes);
    p->text = NULL;
    p->boxes = NULL;
    p->ntext = 0;
    p->text_loaded = FALSE;
    for (k = 0; k < p->nlinks; k++) free(p->links[k].uri);
    free(p->links);
    p->links = NULL;
    p->nlinks = 0;
    p->links_loaded = FALSE;
    for (k = 0; k < p->nobjs; k++) free(p->objs[k].text);
    free(p->objs);
    p->objs = NULL;
    p->nobjs = 0;
    p->objs_loaded = FALSE;
    for (k = 0; k < p->nannots; k++) { free(p->annots[k].author); free(p->annots[k].contents); }
    free(p->annots);
    p->annots = NULL;
    p->nannots = 0;
    p->annots_loaded = FALSE;
}

char g_last_head[1024];          /* the last change's answer (xref=, found=, ...) */

static void free_field(field_t *f)
{
    free(f->name); free(f->value); free(f->options); free(f->fmt); free(f->calc); free(f->tooltip);
}

void doc_free_fields(void)
{
    int i;
    for (i = 0; i < g.nfields; i++) free_field(&g.fields[i]);
    free(g.fields);
    g.fields = NULL;
    g.nfields = 0;
    g.fields_loaded = FALSE;
}

/* the state fields of an answer, and (data) the pages' sizes */
void doc_apply_state(const char *head, const BYTE *data, DWORD len)
{
    char num[32], *copy, *s, *e;
    int n, i, k;
    double *w = NULL, *h = NULL;
    BOOL resized = FALSE;
    if (br_field(head, "undo", num, sizeof(num))) g.undo = atoi(num);
    if (br_field(head, "redo", num, sizeof(num))) g.redo = atoi(num);
    if (br_field(head, "dirty", num, sizeof(num))) g.dirty = atoi(num) != 0;
    if (br_field(head, "perms", num, sizeof(num))) g.perms = (unsigned)strtoul(num, NULL, 10);
    if (br_field(head, "encrypted", num, sizeof(num))) g.encrypted = atoi(num) != 0;
    if (br_field(head, "form", num, sizeof(num))) g.form = atoi(num) != 0;
    if (br_field(head, "redactions", num, sizeof(num))) g.nredact = atoi(num);
    if (br_field(head, "protect", num, sizeof(num))) lstrcpynA(g.protect, num, sizeof(g.protect));
    if (!data) return;

    /* the document changed: its pages */
    n = br_field(head, "pages", num, sizeof(num)) ? atoi(num) : g.npages;
    if (n <= 0) return;
    w = calloc(n, sizeof(double));
    h = calloc(n, sizeof(double));
    copy = malloc(len + 1);
    if (!w || !h || !copy) { free(w); free(h); free(copy); return; }
    memcpy(copy, data, len);
    copy[len] = 0;
    for (s = copy, i = 0; s && *s; s = e) {
        e = strchr(s, '\n');
        if (e) *e++ = 0;
        if (!strncmp(s, "size ", 5) && i < n) {
            sscanf(s + 5, "%lf %lf", &w[i], &h[i]);
            if (w[i] <= 1) w[i] = 612;
            if (h[i] <= 1) h[i] = 792;
            i++;
        } else if (!strncmp(s, "title ", 6)) {
            WCHAR *t = from_utf8(s + 6, -1);
            if (t) { lstrcpynW(g.doc_title, t, 256); free(t); }
        }
    }
    free(copy);
    g.generation++;
    render_clear_wants(FALSE);
    render_clear_wants(TRUE);
    if (n != g.npages) resized = TRUE;
    for (k = 0; k < n && !resized; k++)
        if (fabs(w[k] - g.pages[k].w) > 0.01 || fabs(h[k] - g.pages[k].h) > 0.01) resized = TRUE;
    if (resized) {
        page_t *np = calloc(n, sizeof(page_t));
        BOOL *ns = calloc(n, sizeof(BOOL));
        if (!np || !ns) { free(np); free(ns); free(w); free(h); return; }
        for (k = 0; k < g.npages; k++) {
            page_t *p = &g.pages[k];
            doc_free_page_cache(p);
            /* a page whose size is the same keeps its picture until it is drawn again */
            if (k < n && fabs(w[k] - p->w) < 0.01 && fabs(h[k] - p->h) < 0.01) {
                np[k].bmp = p->bmp; np[k].bw = p->bw; np[k].bh = p->bh; np[k].brot = p->brot; np[k].bscale = p->bscale;
                np[k].thumb = p->thumb; np[k].tw = p->tw; np[k].th = p->th; np[k].trot = p->trot;
                np[k].stale = np[k].tstale = TRUE;
            } else {
                if (p->bmp) DeleteObject(p->bmp);
                if (p->thumb) DeleteObject(p->thumb);
            }
        }
        free(g.pages);
        free(g.org_sel);
        g.pages = np;
        g.org_sel = ns;
        g.npages = n;
        for (k = 0; k < n; k++) { g.pages[k].w = w[k]; g.pages[k].h = h[k]; }
        if (g.current >= n) g.current = n - 1;
    } else {
        for (k = 0; k < n; k++) {
            page_t *p = &g.pages[k];
            doc_free_page_cache(p);
            p->stale = p->tstale = TRUE;
        }
    }
    free(w);
    free(h);
    /* what pointed into the old document */
    free(g.hits);
    g.hits = NULL;
    g.nhits = 0;
    g.hit = -1;
    g.searched = FALSE;
    g.has_sel = FALSE;
    g.sel_a.page = g.sel_b.page = -1;
    doc_free_fields();
    if (g.form || g.tool == TOOL_FORM) doc_load_fields();
    {
        BOOL had_sigs = g.nsigs > 0;
        doc_free_sigs();
        if (had_sigs || g.form) sigbar_update();
    }
    doc_free_attach();
    side_load_outline();
    tool_after_change();
    if (resized) view_relayout(TRUE);
    InvalidateRect(g_view, NULL, FALSE);
    side_update();
    org_update();
    toolui_update();
    app_update_title();
}

static const WCHAR *refusal_text(const char *head, WCHAR *buf, int cap)
{
    WCHAR *m;
    const char *msg = head;
    if (!strncmp(head, "ERR ", 4)) {
        msg = strchr(head + 4, ' ');
        msg = msg ? msg + 1 : head + 4;
    }
    m = from_utf8(msg, -1);
    if (strstr(head, "ERR secured"))
        lstrcpynW(buf, L"This document's security does not allow this change. Enter the permissions password "
                       L"(File > Protect) to change it.", cap);
    else if (strstr(head, "not connected") || !head[0])
        lstrcpynW(buf, L"The PDF engine (sg-pdf) stopped.", cap);
    else {
        lstrcpynW(buf, m && m[0] ? m : L"The change could not be made.", cap);
        buf[0] = towupper(buf[0]);
    }
    free(m);
    return buf;
}

BOOL doc_request(const char *line)
{
    char head[1024];
    BYTE *data = NULL;
    DWORD len = 0;
    int rc;
    HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    rc = br_request(line, head, sizeof(head), &data, &len);
    SetCursor(old);
    if (rc == 1) {
        g.status[0] = 0;
        lstrcpynA(g_last_head, head, sizeof(g_last_head));
        doc_apply_state(head, data, len);
        free(data);
        app_status_changed();
        return TRUE;
    }
    free(data);
    {
        WCHAR msg[300];
        refusal_text(head, msg, 300);
        app_set_status(L"%ls", msg);
        if (!GetEnvironmentVariableW(L"SG_PDF_QUIET", NULL, 0))
            MessageBoxW(g_main, msg, APP_NAME, MB_OK | MB_ICONWARNING);
    }
    return FALSE;
}

BOOL doc_requestf(const char *fmt, ...)
{
    va_list ap;
    int n;
    char *line;
    BOOL ok;
    va_start(ap, fmt);
    n = _vscprintf(fmt, ap);
    va_end(ap);
    if (n < 0 || !(line = malloc(n + 1))) return FALSE;
    va_start(ap, fmt);
    vsnprintf(line, n + 1, fmt, ap);
    va_end(ap);
    ok = doc_request(line);
    free(line);
    return ok;
}

void doc_undo(int dir)
{
    if (!g.npages) return;
    if (dir < 0 ? !g.undo : !g.redo) return;
    doc_request(dir < 0 ? "undo" : "redo");
}

void doc_pages_spec(char *out, int cap, const int *pages, int n)
{
    int i, k = 0;
    out[0] = 0;
    for (i = 0; i < n && k < cap - 12; i++) k += snprintf(out + k, cap - k, i ? ",%d" : "%d", pages[i]);
}

/* ---- what the tools read ------------------------------------------------------------------------- */

BOOL doc_load_objects(int page)
{
    page_t *p;
    char line[64], head[512], num[32], *s, *e;
    BYTE *data = NULL;
    DWORD len;
    int n;
    if (page < 0 || page >= g.npages) return FALSE;
    p = &g.pages[page];
    if (p->objs_loaded) return TRUE;
    snprintf(line, sizeof(line), "objects\t%d", page);
    if (br_request(line, head, sizeof(head), &data, &len) != 1) { free(data); return FALSE; }
    p->objs_loaded = TRUE;
    n = br_field(head, "n", num, sizeof(num)) ? atoi(num) : 0;
    p->objs = n > 0 ? calloc(n, sizeof(obj_t)) : NULL;
    for (s = (char *)data; p->objs && s && *s && p->nobjs < n; s = e) {
        char *f[10] = { 0 };
        int nf;
        obj_t *o = &p->objs[p->nobjs];
        e = strchr(s, '\n');
        if (e) *e++ = 0;
        nf = split_tabs(s, f, 10);
        if (nf < 3 || !parse_rect(f[2], &o->box)) continue;
        o->id = atoi(f[1]);
        if (!strcmp(f[0], "text") && nf >= 10) {
            WCHAR *font = unesc_utf8(f[3], -1);
            o->kind = OBJ_TEXT;
            if (font) { lstrcpynW(o->font, font, 64); free(font); }
            o->size = (float)atof(f[4]);
            o->color = hex_color(f[5]);
            o->style = atoi(f[6]);
            o->align = atoi(f[7]);
            o->lh = (float)atof(f[8]);
            o->text = unesc_utf8(f[9], -1);
        } else if (!strcmp(f[0], "image")) {
            o->kind = OBJ_IMAGE;
            o->xref = nf > 3 ? atoi(f[3]) : 0;
        } else if (!strcmp(f[0], "path")) {
            o->kind = OBJ_PATH;
        } else continue;
        p->nobjs++;
    }
    free(data);
    return TRUE;
}

BOOL doc_load_annots(int page)
{
    page_t *p;
    char line[64], head[512], num[32], *s, *e;
    BYTE *data = NULL;
    DWORD len;
    int n;
    if (page < 0 || page >= g.npages) return FALSE;
    p = &g.pages[page];
    if (p->annots_loaded) return TRUE;
    snprintf(line, sizeof(line), "annots\t%d", page);
    if (br_request(line, head, sizeof(head), &data, &len) != 1) { free(data); return FALSE; }
    p->annots_loaded = TRUE;
    n = br_field(head, "n", num, sizeof(num)) ? atoi(num) : 0;
    p->annots = n > 0 ? calloc(n, sizeof(annot_t)) : NULL;
    for (s = (char *)data; p->annots && s && *s && p->nannots < n; s = e) {
        char *f[10] = { 0 };
        annot_t *a = &p->annots[p->nannots];
        int nf;
        e = strchr(s, '\n');
        if (e) *e++ = 0;
        if ((nf = split_tabs(s, f, 10)) < 7 || !parse_rect(f[3], &a->box)) continue;
        a->replies = nf > 8 ? atoi(f[8]) : 0;
        if (nf > 9) lstrcpynA(a->status, f[9], sizeof(a->status));
        a->xref = atoi(f[1]);
        lstrcpynA(a->type, f[2], sizeof(a->type));
        a->color = hex_color(f[4]);
        a->author = unesc_utf8(f[5], -1);
        a->contents = unesc_utf8(f[6], -1);
        p->nannots++;
    }
    free(data);
    return TRUE;
}

BOOL doc_load_fields(void)
{
    char head[512], num[32], *s, *e;
    BYTE *data = NULL;
    DWORD len;
    int n;
    doc_free_fields();
    if (br_request("fields", head, sizeof(head), &data, &len) != 1) { free(data); return FALSE; }
    g.fields_loaded = TRUE;
    n = br_field(head, "n", num, sizeof(num)) ? atoi(num) : 0;
    g.fields = n > 0 ? calloc(n, sizeof(field_t)) : NULL;
    for (s = (char *)data; g.fields && s && *s && g.nfields < n; s = e) {
        static const char *TYPES[] = { "text", "checkbox", "radio", "combo", "list", "button", "signature" };
        char *f[12] = { 0 };
        field_t *fl = &g.fields[g.nfields];
        int k, nf;
        e = strchr(s, '\n');
        if (e) *e++ = 0;
        memset(fl, 0, sizeof(*fl));
        if ((nf = split_tabs(s, f, 12)) < 8 || !parse_rect(f[3], &fl->box)) continue;
        fl->fmt = unesc_utf8(nf > 9 ? f[9] : "", -1);
        fl->calc = unesc_utf8(nf > 10 ? f[10] : "", -1);
        fl->tooltip = unesc_utf8(nf > 11 ? f[11] : "", -1);
        fl->page = atoi(f[0]);
        fl->xref = atoi(f[1]);
        fl->type = FLD_OTHER;
        for (k = 0; k < 7; k++) if (!strcmp(f[2], TYPES[k])) fl->type = k;
        fl->flags = atoi(f[4]);
        fl->name = unesc_utf8(f[5], -1);
        fl->value = unesc_utf8(f[6], -1);
        fl->options = unesc_utf8(f[7], -1);
        fl->fontsize = f[8] ? (float)atof(f[8]) : 0;
        if (fl->page < 0 || fl->page >= g.npages) { free_field(fl); continue; }
        g.nfields++;
    }
    free(data);
    return TRUE;
}

/* ---- saving ------------------------------------------------------------------------------------------ */

BOOL doc_save(BOOL save_as)
{
    WCHAR target[MAX_PATH];
    char *u, *eu, line[MAX_PATH * 8];
    WCHAR *wu;
    BOOL apply = FALSE, ok;
    if (!g.npages || !g.bridged) return FALSE;
    tool_commit_editor();
    lstrcpynW(target, g.path, MAX_PATH);
    if (save_as || !g.path[0]) {
        if (!file_dialog(TRUE, L"Save As", L"PDF documents (*.pdf)\0*.pdf\0", L"pdf", target, MAX_PATH)) return FALSE;
    }
    if (g.nredact > 0) {
        WCHAR t[400];
        int r;
        swprintf(t, 400, L"%d redaction mark%ls not been applied. Apply %ls before saving?\n\n"
                 L"Yes: the marked content is removed for good.\nNo: the marks are saved as they are, and the content "
                 L"under them stays in the file.", g.nredact, g.nredact == 1 ? L" has" : L"s have", g.nredact == 1 ? L"it" : L"them");
        r = MessageBoxW(g_main, t, L"Redact", MB_YESNOCANCEL | MB_ICONWARNING);
        if (r == IDCANCEL) return FALSE;
        apply = r == IDYES;
    }
    if (!(u = unix_path(target))) {
        MessageBoxW(g_main, L"That folder cannot be written to.", APP_NAME, MB_OK | MB_ICONWARNING);
        return FALSE;
    }
    wu = from_utf8(u, -1);
    eu = wu ? esc_utf8(wu) : NULL;
    free(wu);
    free(u);
    if (!eu) return FALSE;
    snprintf(line, sizeof(line), apply ? "save\t%s\tapply=1" : "save\t%s", eu);
    free(eu);
    ok = doc_request(line);
    if (ok) {
        WCHAR *base;
        lstrcpynW(g.path, target, MAX_PATH);
        base = wcsrchr(target, L'\\');
        lstrcpynW(g.name, base ? base + 1 : target, MAX_PATH);
        g.untitled = FALSE;
        recent_add(g.path);
        app_set_status(L"Saved.");
        app_update_title();
    }
    return ok;
}

BOOL doc_close_prompt(void)
{
    WCHAR t[MAX_PATH + 100];
    int r;
    tool_commit_editor();
    if (!g.npages || !g.dirty || !g.bridged) return TRUE;
    swprintf(t, MAX_PATH + 100, L"Do you want to save changes to \"%ls\" before closing?", g.name);
    r = MessageBoxW(g_main, t, APP_NAME, MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return FALSE;
    if (r == IDYES) return doc_save(FALSE);
    return TRUE;
}
