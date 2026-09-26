/* sg-wordpad -- OpenDocument Text (.odt): our own reader and writer of the
 * subset WordPad's model has, written from OASIS OpenDocument 1.2/1.3.
 *
 * Read: text:p and text:h with their paragraph styles (automatic styles in
 * content.xml, named ones in styles.xml, following parent-style-name):
 * alignment, margins, text indent, spacing, line height, tab stops; text:span
 * text properties (weight, style, underline, line-through, position, size,
 * font, colour, background); text:s, text:tab, text:line-break; lists
 * (text:list with its list style's first level: bullet or number format);
 * draw:frame/draw:image pictures from the package; tables flattened.
 *
 * Write: mimetype first and stored, the manifest, content.xml with an
 * automatic style per distinct paragraph and text format, list styles,
 * pictures as PNG in Pictures/.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "wordpad.h"
#include "../zip/zipcore.h"

static int zfind(zarchive *z, const char *name)
{
    WCHAR w[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, MAX_PATH);
    for (int i = 0; i < z->n; i++) if (!lstrcmpW(z->e[i].name, w)) return i;
    return -1;
}

static char *zget(zarchive *z, const char *name, size_t *len)
{
    int i = zfind(z, name);
    BYTE *p = NULL;
    size_t n = 0;
    if (i < 0 || zip_read(z, i, &p, &n)) return NULL;
    p = realloc(p, n + 1);
    p[n] = 0;
    if (len) *len = n;
    return (char *)p;
}

static XNode *zxml(zarchive *z, const char *name)
{
    size_t n;
    char *s = zget(z, name, &n);
    XNode *x;
    if (!s) return NULL;
    x = xml_parse(s, n);
    free(s);
    return x;
}

/* "1.5in", "2.54cm", "12pt", "10mm", "1pc", "96px" -> twips */
static int length_twips(const char *v)
{
    double d;
    char *end;
    if (!v) return 0;
    d = strtod(v, &end);
    if (!strncmp(end, "in", 2)) return (int)(d * 1440 + (d < 0 ? -0.5 : 0.5));
    if (!strncmp(end, "cm", 2)) return (int)(d * 1440 / 2.54 + (d < 0 ? -0.5 : 0.5));
    if (!strncmp(end, "mm", 2)) return (int)(d * 144 / 2.54 + (d < 0 ? -0.5 : 0.5));
    if (!strncmp(end, "pt", 2)) return (int)(d * 20 + (d < 0 ? -0.5 : 0.5));
    if (!strncmp(end, "pc", 2)) return (int)(d * 240 + (d < 0 ? -0.5 : 0.5));
    if (!strncmp(end, "px", 2)) return (int)(d * 15 + (d < 0 ? -0.5 : 0.5));
    return (int)(d * 20);
}

typedef struct {
    const XNode *node;
    const char *name, *family, *parent;
} OStyle;

typedef struct {
    zarchive *z;
    Doc *d;
    OStyle *st; int nst;
    XNode *content, *styles;
    int in_table;
} OReader;

static void collect(OReader *r, const XNode *x)
{
    if (!x) return;
    for (int i = 0; i < x->nkids; i++)
    {
        const XNode *k = x->kids[i];
        const char *n = k->name ? xml_local(k->name) : NULL;
        if (!n) continue;
        if (!strcmp(n, "style") || !strcmp(n, "list-style") || !strcmp(n, "default-style"))
        {
            r->st = realloc(r->st, (r->nst + 1) * sizeof(OStyle));
            r->st[r->nst].node = k;
            r->st[r->nst].name = xml_attr(k, "name");
            r->st[r->nst].family = !strcmp(n, "list-style") ? "list" : xml_attr(k, "family");
            r->st[r->nst].parent = xml_attr(k, "parent-style-name");
            if (!strcmp(n, "default-style")) r->st[r->nst].name = "#default";
            r->nst++;
        }
        else if (!strcmp(n, "styles") || !strcmp(n, "automatic-styles") || !strcmp(n, "document-styles") ||
                 !strcmp(n, "document-content"))
            collect(r, k);
    }
}

static const OStyle *find(OReader *r, const char *name, const char *family)
{
    if (!name) return NULL;
    /* the last one wins: automatic styles (content.xml) are collected last */
    for (int i = r->nst - 1; i >= 0; i--)
        if (r->st[i].name && !strcmp(r->st[i].name, name) && (!family || !r->st[i].family || !strcmp(r->st[i].family, family)))
            return &r->st[i];
    return NULL;
}

static void text_props(const XNode *tp, CharProps *cp)
{
    const char *v;
    if (!tp) return;
    if ((v = xml_attr(tp, "font-weight"))) cp->bold = !strcmp(v, "bold") || atoi(v) >= 600;
    if ((v = xml_attr(tp, "font-style"))) cp->italic = !strcmp(v, "italic") || !strcmp(v, "oblique");
    if ((v = xml_attr(tp, "text-underline-style"))) cp->underline = strcmp(v, "none") != 0;
    if ((v = xml_attr(tp, "text-line-through-style"))) cp->strike = strcmp(v, "none") != 0;
    if ((v = xml_attr(tp, "text-position")))
    {
        if (!strncmp(v, "super", 5)) cp->script = 1;
        else if (!strncmp(v, "sub", 3)) cp->script = -1;
        else { int p = atoi(v); cp->script = p > 0 ? 1 : p < 0 ? -1 : 0; }
    }
    if ((v = xml_attr(tp, "font-size")) && !strchr(v, '%')) { int t = length_twips(v); if (t > 0) cp->hps = t / 10; }
    if ((v = xml_attr(tp, "font-name")) || (v = xml_attr(tp, "font-family")))
    {
        char f[64];
        size_t n = strlen(v);
        if (n && (v[0] == '\'' || v[0] == '"')) { v++; n -= 2; }
        if (n > 63) n = 63;
        memcpy(f, v, n); f[n] = 0;
        MultiByteToWideChar(CP_UTF8, 0, f, -1, cp->font, 64);
    }
    if ((v = xml_attr(tp, "color")) && v[0] == '#')
    {
        unsigned c = (unsigned)strtoul(v + 1, NULL, 16);
        cp->has_color = 1; cp->color = RGB((c >> 16) & 255, (c >> 8) & 255, c & 255);
    }
    if ((v = xml_attr(tp, "background-color")))
    {
        if (v[0] == '#')
        {
            unsigned c = (unsigned)strtoul(v + 1, NULL, 16);
            cp->has_hl = 1; cp->hl = RGB((c >> 16) & 255, (c >> 8) & 255, c & 255);
        }
        else cp->has_hl = 0;
    }
}

static void para_props(const XNode *pp, Para *p)
{
    const char *v;
    if (!pp) return;
    if ((v = xml_attr(pp, "text-align")))
    {
        if (!strcmp(v, "center")) p->align = AL_CENTER;
        else if (!strcmp(v, "end") || !strcmp(v, "right")) p->align = AL_RIGHT;
        else if (!strcmp(v, "justify")) p->align = AL_JUSTIFY;
        else p->align = AL_LEFT;
    }
    if ((v = xml_attr(pp, "margin-left"))) p->left = length_twips(v);
    if ((v = xml_attr(pp, "margin-right"))) p->right = length_twips(v);
    if ((v = xml_attr(pp, "text-indent"))) p->first = length_twips(v);
    if ((v = xml_attr(pp, "margin-top"))) p->before = length_twips(v);
    if ((v = xml_attr(pp, "margin-bottom"))) p->after = length_twips(v);
    if ((v = xml_attr(pp, "line-height")))
    {
        if (strchr(v, '%')) p->line = atoi(v) * 240 / 100;
        else if (strcmp(v, "normal")) p->line = -length_twips(v);
    }
    {
        const XNode *tabs = xml_child(pp, "tab-stops");
        if (tabs)
        {
            p->ntabs = 0;
            for (int i = 0; i < tabs->nkids && p->ntabs < 32; i++)
                if (tabs->kids[i]->name && !strcmp(xml_local(tabs->kids[i]->name), "tab-stop"))
                    p->tabs[p->ntabs++] = length_twips(xml_attr(tabs->kids[i], "position"));
        }
    }
}

static void style_text(OReader *r, const char *name, const char *family, CharProps *cp, int depth)
{
    const OStyle *s = find(r, name, family);
    if (!s || depth > 10) return;
    if (s->parent) style_text(r, s->parent, family, cp, depth + 1);
    text_props(xml_child(s->node, "text-properties"), cp);
}

static void style_para(OReader *r, const char *name, Para *p, int depth)
{
    const OStyle *s = find(r, name, "paragraph");
    if (!s || depth > 10) return;
    if (s->parent) style_para(r, s->parent, p, depth + 1);
    para_props(xml_child(s->node, "paragraph-properties"), p);
}

static int list_kind(OReader *r, const char *name)
{
    const OStyle *s = find(r, name, "list");
    if (!s) return LS_BULLET;
    for (int i = 0; i < s->node->nkids; i++)
    {
        const XNode *k = s->node->kids[i];
        const char *n = k->name ? xml_local(k->name) : NULL;
        const char *lv = xml_attr(k, "level");
        if (!n || (lv && atoi(lv) != 1)) continue;
        if (!strcmp(n, "list-level-style-bullet")) return LS_BULLET;
        if (!strcmp(n, "list-level-style-number"))
        {
            const char *f = xml_attr(k, "num-format");
            if (!f || !*f) return LS_NONE;
            if (!strcmp(f, "a")) return LS_LALPHA;
            if (!strcmp(f, "A")) return LS_UALPHA;
            if (!strcmp(f, "i")) return LS_LROMAN;
            if (!strcmp(f, "I")) return LS_UROMAN;
            return LS_DECIMAL;
        }
    }
    return LS_BULLET;
}

static void add_utf8(Para *p, const CharProps *cp, const char *s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    WCHAR *w = malloc(n * sizeof(WCHAR)), *o = w;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    /* white space collapses as ODF says: runs of spaces/newlines are one space */
    for (WCHAR *i = w; *i; i++)
    {
        WCHAR c = *i;
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (c == ' ' && o > w && o[-1] == ' ') continue;
        *o++ = c;
    }
    para_add_text(p, cp, w, (int)(o - w));
    free(w);
}

static void inline_content(OReader *r, const XNode *x, Para *p, const CharProps *cp, int depth)
{
    if (depth > 32) return;
    for (int i = 0; i < x->nkids; i++)
    {
        const XNode *k = x->kids[i];
        const char *n = k->name ? xml_local(k->name) : NULL;
        if (!n) { if (k->text) add_utf8(p, cp, k->text); continue; }
        if (!strcmp(n, "span") || !strcmp(n, "a"))
        {
            CharProps c2 = *cp;
            style_text(r, xml_attr(k, "style-name"), "text", &c2, 0);
            inline_content(r, k, p, &c2, depth + 1);
        }
        else if (!strcmp(n, "s"))
        {
            int c = xml_attr(k, "c") ? atoi(xml_attr(k, "c")) : 1;
            for (int j = 0; j < c && j < 1000; j++) para_add_text(p, cp, L" ", 1);
        }
        else if (!strcmp(n, "tab")) para_add_text(p, cp, L"\t", 1);
        else if (!strcmp(n, "line-break")) para_add_text(p, cp, L"\v", 1);
        else if (!strcmp(n, "frame"))
        {
            const XNode *img = xml_child(k, "image");
            const char *href = img ? xml_attr(img, "href") : NULL;
            size_t sz;
            char *data;
            if (href && (data = zget(r->z, href, &sz)))
            {
                Run *run = para_add_run(p);
                const char *ext = strrchr(href, '.');
                run->pic = ext && (!_stricmp(ext, ".jpg") || !_stricmp(ext, ".jpeg")) ? PIC_JPEG :
                           ext && !_stricmp(ext, ".emf") ? PIC_EMF : ext && !_stricmp(ext, ".wmf") ? PIC_WMF : PIC_PNG;
                run->data = (BYTE *)data; run->size = (DWORD)sz;
                run->w = length_twips(xml_attr(k, "width"));
                run->h = length_twips(xml_attr(k, "height"));
                run->cp = *cp;
            }
        }
        else if (!strcmp(n, "soft-page-break") || !strcmp(n, "bookmark") || !strcmp(n, "note")) ;
        else if (strcmp(n, "annotation")) inline_content(r, k, p, cp, depth + 1);
    }
}

static void blocks(OReader *r, const XNode *x, int list, int depth);

/* a column's width from its style */
static int column_width(OReader *r, const char *style)
{
    const OStyle *s = find(r, style, "table-column");
    const XNode *p = s ? xml_child(s->node, "table-column-properties") : NULL;
    const char *w = p ? xml_attr(p, "column-width") : NULL;
    return w ? length_twips(w) : 0;
}

/* the rows of a table or of a row group (header rows, rows) */
static void read_rows(OReader *r, const XNode *g, const int *widths, int nw, int depth)
{
    if (depth > 8) return;
    for (int i = 0; i < g->nkids; i++)
    {
        const XNode *tr = g->kids[i];
        const char *n = tr->name ? xml_local(tr->name) : NULL;
        Row row;
        int x = 0, col = 0, first = r->d->n, cell = 0, num;
        if (!n) continue;
        if (!strcmp(n, "table-header-rows") || !strcmp(n, "table-rows") || !strcmp(n, "table-row-group"))
        {
            read_rows(r, tr, widths, nw, depth + 1);
            continue;
        }
        if (strcmp(n, "table-row")) continue;
        memset(&row, 0, sizeof(row));
        for (int j = 0; j < tr->nkids && row.ncells < MAX_CELLS; j++)
        {
            const XNode *tc = tr->kids[j];
            const char *cn = tc->name ? xml_local(tc->name) : NULL;
            int span = 1, width = 0, start = r->d->n;
            if (!cn || strcmp(cn, "table-cell")) continue;
            if (xml_attr(tc, "number-columns-spanned")) span = max(1, atoi(xml_attr(tc, "number-columns-spanned")));
            for (int k = 0; k < span; k++) width += col + k < nw ? widths[col + k] : 0;
            if (width <= 0) width = 2000 * span;
            col += span;
            x += width;
            row.cellx[row.ncells++] = x;
            blocks(r, tc, 0, depth + 1);
            if (r->d->n == start) doc_add_para(r->d);
            for (int k = start; k < r->d->n; k++) { r->d->p[k].cell = cell; r->d->p[k].cell_end = k == r->d->n - 1; r->d->p[k].row = -1; }
            cell++;
        }
        if (!row.ncells) continue;
        num = doc_add_row(r->d, &row);
        for (int k = first; k < r->d->n; k++) if (r->d->p[k].row == -1) r->d->p[k].row = num;
    }
}

/* table:table -- its rows become Rows, each cell's paragraphs are marked */
static void read_table(OReader *r, const XNode *t, int depth)
{
    int widths[MAX_CELLS], nw = 0;
    for (int i = 0; i < t->nkids && nw < MAX_CELLS; i++)
    {
        const XNode *k = t->kids[i];
        if (k->name && !strcmp(xml_local(k->name), "table-column"))
        {
            int rep = xml_attr(k, "number-columns-repeated") ? atoi(xml_attr(k, "number-columns-repeated")) : 1;
            int w = column_width(r, xml_attr(k, "style-name"));
            for (int j = 0; j < rep && nw < MAX_CELLS; j++) widths[nw++] = w;
        }
    }
    read_rows(r, t, widths, nw, depth);
}

static void blocks(OReader *r, const XNode *x, int list, int depth)
{
    if (depth > 32) return;
    for (int i = 0; i < x->nkids; i++)
    {
        const XNode *k = x->kids[i];
        const char *n = k->name ? xml_local(k->name) : NULL;
        if (!n) continue;
        if (!strcmp(n, "p") || !strcmp(n, "h"))
        {
            Para *p = doc_add_para(r->d);
            CharProps cp;
            const char *sn = xml_attr(k, "style-name");
            cp_default(&cp);
            style_text(r, "#default", "paragraph", &cp, 0);
            style_para(r, "#default", p, 0);
            if (!strcmp(n, "h") && !sn) { cp.bold = 1; cp.hps = 32; }
            style_para(r, sn, p, 0);
            style_text(r, sn, "paragraph", &cp, 0);
            if (list) { p->list = list; if (!p->left) { p->left = 720; p->first = -360; } }
            inline_content(r, k, p, &cp, 0);
        }
        else if (!strcmp(n, "list"))
        {
            const char *sn = xml_attr(k, "style-name");
            blocks(r, k, sn ? list_kind(r, sn) : (list ? list : LS_BULLET), depth + 1);
        }
        else if (!strcmp(n, "table") && !r->in_table) { r->in_table++; read_table(r, k, depth + 1); r->in_table--; }
        else if (!strcmp(n, "list-item") || !strcmp(n, "list-header") || !strcmp(n, "section") || !strcmp(n, "table") ||
                 !strcmp(n, "table-row") || !strcmp(n, "table-cell") || !strcmp(n, "table-header-rows") ||
                 !strcmp(n, "table-rows") || !strcmp(n, "text") || !strcmp(n, "body") || !strcmp(n, "document-content"))
            blocks(r, k, list, depth + 1);
    }
}

BOOL odt_read(const WCHAR *path, Doc *d, WCHAR *err, int cch)
{
    zarchive z;
    OReader r;
    memset(&r, 0, sizeof(r));
    if (zip_open(&z, path)) { lstrcpynW(err, L"It is not an OpenDocument file.", cch); return FALSE; }
    r.z = &z; r.d = d;
    if (!(r.content = zxml(&z, "content.xml")))
    {
        zip_close(&z);
        lstrcpynW(err, L"The document has no content (content.xml).", cch);
        return FALSE;
    }
    r.styles = zxml(&z, "styles.xml");
    collect(&r, r.styles);
    collect(&r, r.content);
    blocks(&r, r.content, 0, 0);
    if (!d->n) doc_add_para(d);
    free(r.st);
    xml_free(r.content); xml_free(r.styles);
    zip_close(&z);
    return TRUE;
}

/* ---------------------------------------------------------------- writing */

/* a signed length that prints right for negatives too */
static void len_in(Buf *b, const char *attr, int twips)
{
    double in = twips / 1440.0;
    char t[32];
    snprintf(t, sizeof(t), "%.4f", in);
    buf_printf(b, " %s=\"%sin\"", attr, t);
}

static BOOL para_same(const Para *a, const Para *b)
{
    return a->align == b->align && a->left == b->left && a->right == b->right && a->first == b->first &&
           a->before == b->before && a->after == b->after && a->line == b->line && a->ntabs == b->ntabs &&
           !memcmp(a->tabs, b->tabs, a->ntabs * sizeof(int));
}

static BOOL cp_same(const CharProps *a, const CharProps *b)
{
    return !lstrcmpiW(a->font, b->font) && a->hps == b->hps && a->bold == b->bold && a->italic == b->italic &&
           a->underline == b->underline && a->strike == b->strike && a->script == b->script &&
           a->has_color == b->has_color && (!a->has_color || a->color == b->color) &&
           a->has_hl == b->has_hl && (!a->has_hl || a->hl == b->hl);
}

static void write_text(Buf *b, const WCHAR *s, int n)
{
    int st = 0;
    for (int i = 0; i <= n; i++)
    {
        BOOL sp2 = i < n && s[i] == ' ' && (i == 0 || s[i - 1] == ' ' || i == n - 1);
        if (i == n || s[i] == '\t' || s[i] == '\v' || sp2)
        {
            if (i > st) buf_xml(b, s + st, i - st);
            if (i < n) buf_str(b, s[i] == '\t' ? "<text:tab/>" : s[i] == '\v' ? "<text:line-break/>" : "<text:s/>");
            st = i + 1;
        }
    }
}

BOOL odt_write(const WCHAR *path, const Doc *d, WCHAR *err, int cch)
{
    Buf body = { 0 }, autos = { 0 }, c = { 0 }, man = { 0 };
    const Para **pstyles = calloc(d->n + 1, sizeof(Para *));
    const CharProps **tstyles = NULL;
    int np = 0, nt = 0, nimg = 0, r = 0, curlist = 0;
    struct { BYTE *png; DWORD size; } *imgs = NULL;
    zwriter *w;
    static const char *numfmt[] = { "", "", "1", "a", "A", "i", "I" };
    int ntables = 0;

    for (int i = 0; i < d->n; i++)
    {
        const Para *p = &d->p[i];
        int ps = -1;
        for (int j = 0; j < np; j++) if (para_same(pstyles[j], p)) { ps = j; break; }
        if (ps < 0) { ps = np; pstyles[np++] = p; }
        if (p->row > 0 && p->row <= d->nrows)
        {
            const Row *row = &d->rows[p->row - 1];
            BOOL row_start = i == 0 || d->p[i - 1].row != p->row;
            if (i == 0 || !(d->p[i - 1].row > 0))
            {
                if (curlist) { buf_str(&body, "</text:list>"); curlist = 0; }
                ntables++;
                buf_printf(&body, "<table:table table:name=\"Table%d\" table:style-name=\"Tb%d\">", ntables, ntables);
                buf_printf(&autos, "<style:style style:name=\"Tb%d\" style:family=\"table\"><style:table-properties", ntables);
                len_in(&autos, "style:width", row->cellx[row->ncells - 1]);
                buf_str(&autos, " table:align=\"left\"/></style:style>");
                for (int c = 0; c < row->ncells; c++)
                {
                    buf_printf(&autos, "<style:style style:name=\"Tb%d.C%d\" style:family=\"table-column\"><style:table-column-properties", ntables, c + 1);
                    len_in(&autos, "style:column-width", row->cellx[c] - (c ? row->cellx[c - 1] : 0));
                    buf_str(&autos, "/></style:style>");
                    buf_printf(&body, "<table:table-column table:style-name=\"Tb%d.C%d\"/>", ntables, c + 1);
                }
            }
            if (row_start) buf_str(&body, "<table:table-row>");
            if (row_start || d->p[i - 1].cell_end)
                buf_str(&body, "<table:table-cell table:style-name=\"TbCell\" office:value-type=\"string\">");
        }
        if (p->list != curlist)
        {
            if (curlist) buf_str(&body, "</text:list>");
            if (p->list) buf_printf(&body, "<text:list text:style-name=\"L%d\">", p->list);
            curlist = p->list;
        }
        if (curlist) buf_str(&body, "<text:list-item>");
        buf_printf(&body, "<text:p text:style-name=\"P%d\">", ps + 1);
        for (int j = 0; j < p->nruns; j++)
        {
            const Run *run = &p->runs[j];
            int ts = -1;
            if (run->pic)
            {
                BYTE *png;
                DWORD size;
                int wpx, hpx, wt, ht;
                if (!pic_to_png(run->pic, run->data, run->size, run->w, run->h, &png, &size, &wpx, &hpx)) continue;
                imgs = realloc(imgs, (nimg + 1) * sizeof(*imgs));
                imgs[nimg].png = png; imgs[nimg].size = size;
                nimg++;
                wt = run->w ? run->w : wpx * 15; ht = run->h ? run->h : hpx * 15;
                buf_printf(&body, "<draw:frame draw:name=\"Picture %d\" text:anchor-type=\"as-char\"", nimg);
                len_in(&body, "svg:width", wt); len_in(&body, "svg:height", ht);
                buf_printf(&body, " draw:z-index=\"0\"><draw:image xlink:href=\"Pictures/image%d.png\" xlink:type=\"simple\" "
                                  "xlink:show=\"embed\" xlink:actuate=\"onLoad\"/></draw:frame>", nimg);
                continue;
            }
            for (int k = 0; k < nt; k++) if (cp_same(tstyles[k], &run->cp)) { ts = k; break; }
            if (ts < 0) { tstyles = realloc(tstyles, (nt + 1) * sizeof(*tstyles)); ts = nt; tstyles[nt++] = &run->cp; }
            buf_printf(&body, "<text:span text:style-name=\"T%d\">", ts + 1);
            write_text(&body, run->text, run->len);
            buf_str(&body, "</text:span>");
        }
        buf_str(&body, "</text:p>");
        if (p->row > 0 && p->row <= d->nrows)
        {
            const Row *row = &d->rows[p->row - 1];
            BOOL last = i == d->n - 1 || d->p[i + 1].row != p->row;
            if (curlist && (p->cell_end || last)) { buf_str(&body, "</text:list-item></text:list>"); curlist = 0; }
            if (p->cell_end || last) buf_str(&body, "</table:table-cell>");
            if (last)
            {
                for (int c = p->cell + 1; c < row->ncells; c++)
                    buf_str(&body, "<table:table-cell table:style-name=\"TbCell\" office:value-type=\"string\"><text:p/></table:table-cell>");
                buf_str(&body, "</table:table-row>");
                if (i == d->n - 1 || !(d->p[i + 1].row > 0)) buf_str(&body, "</table:table>");
            }
        }
        if (curlist) buf_str(&body, "</text:list-item>");
    }
    if (curlist) buf_str(&body, "</text:list>");

    for (int i = 0; i < np; i++)
    {
        const Para *p = pstyles[i];
        static const char *al[] = { "start", "center", "end", "justify" };
        buf_printf(&autos, "<style:style style:name=\"P%d\" style:family=\"paragraph\"><style:paragraph-properties fo:text-align=\"%s\"",
                   i + 1, al[p->align & 3]);
        len_in(&autos, "fo:margin-left", p->left);
        len_in(&autos, "fo:margin-right", p->right);
        len_in(&autos, "fo:text-indent", p->first);
        len_in(&autos, "fo:margin-top", p->before);
        len_in(&autos, "fo:margin-bottom", p->after);
        if (p->line > 0) buf_printf(&autos, " fo:line-height=\"%d%%\"", p->line * 100 / 240);
        else if (p->line < 0) len_in(&autos, "fo:line-height", -p->line);
        if (p->ntabs)
        {
            buf_str(&autos, "><style:tab-stops>");
            for (int t = 0; t < p->ntabs; t++) { buf_str(&autos, "<style:tab-stop"); len_in(&autos, "style:position", p->tabs[t]); buf_str(&autos, "/>"); }
            buf_str(&autos, "</style:tab-stops></style:paragraph-properties></style:style>");
        }
        else buf_str(&autos, "/></style:style>");
    }
    for (int i = 0; i < nt; i++)
    {
        const CharProps *cp = tstyles[i];
        buf_printf(&autos, "<style:style style:name=\"T%d\" style:family=\"text\"><style:text-properties", i + 1);
        if (cp->font[0])
        {
            buf_str(&autos, " style:font-name=\""); buf_xml(&autos, cp->font, lstrlenW(cp->font));
            buf_str(&autos, "\" fo:font-family=\"'"); buf_xml(&autos, cp->font, lstrlenW(cp->font)); buf_str(&autos, "'\"");
        }
        if (cp->hps) buf_printf(&autos, " fo:font-size=\"%d%spt\"", cp->hps / 2, cp->hps % 2 ? ".5" : "");
        if (cp->bold) buf_str(&autos, " fo:font-weight=\"bold\"");
        if (cp->italic) buf_str(&autos, " fo:font-style=\"italic\"");
        if (cp->underline) buf_str(&autos, " style:text-underline-style=\"solid\" style:text-underline-width=\"auto\" style:text-underline-color=\"font-color\"");
        if (cp->strike) buf_str(&autos, " style:text-line-through-style=\"solid\"");
        if (cp->script) buf_printf(&autos, " style:text-position=\"%s 58%%\"", cp->script > 0 ? "super" : "sub");
        if (cp->has_color) buf_printf(&autos, " fo:color=\"#%02x%02x%02x\"", GetRValue(cp->color), GetGValue(cp->color), GetBValue(cp->color));
        if (cp->has_hl) buf_printf(&autos, " fo:background-color=\"#%02x%02x%02x\"", GetRValue(cp->hl), GetGValue(cp->hl), GetBValue(cp->hl));
        buf_str(&autos, "/></style:style>");
    }
    buf_str(&autos, "<style:style style:name=\"TbCell\" style:family=\"table-cell\"><style:table-cell-properties "
                    "fo:padding=\"0.04in\" fo:border=\"0.5pt solid #000000\"/></style:style>");
    for (int l = 1; l <= LS_UROMAN; l++)
    {
        if (l == LS_BULLET)
            buf_str(&autos, "<text:list-style style:name=\"L1\"><text:list-level-style-bullet text:level=\"1\" text:bullet-char=\"\xE2\x80\xA2\">"
                            "<style:list-level-properties text:list-level-position-and-space-mode=\"label-alignment\">"
                            "<style:list-level-label-alignment text:label-followed-by=\"listtab\" fo:text-indent=\"-0.25in\" fo:margin-left=\"0.5in\"/>"
                            "</style:list-level-properties></text:list-level-style-bullet></text:list-style>");
        else
            buf_printf(&autos, "<text:list-style style:name=\"L%d\"><text:list-level-style-number text:level=\"1\" style:num-suffix=\".\" style:num-format=\"%s\">"
                               "<style:list-level-properties text:list-level-position-and-space-mode=\"label-alignment\">"
                               "<style:list-level-label-alignment text:label-followed-by=\"listtab\" fo:text-indent=\"-0.25in\" fo:margin-left=\"0.5in\"/>"
                               "</style:list-level-properties></text:list-level-style-number></text:list-style>", l, numfmt[l]);
    }

    buf_str(&c, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<office:document-content xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
                "xmlns:table=\"urn:oasis:names:tc:opendocument:xmlns:table:1.0\" "
                "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
                "xmlns:text=\"urn:oasis:names:tc:opendocument:xmlns:text:1.0\" "
                "xmlns:draw=\"urn:oasis:names:tc:opendocument:xmlns:drawing:1.0\" "
                "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" "
                "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
                "xmlns:svg=\"urn:oasis:names:tc:opendocument:xmlns:svg-compatible:1.0\" office:version=\"1.3\">"
                "<office:automatic-styles>");
    buf_add(&c, autos.p ? autos.p : "", autos.n);
    buf_str(&c, "</office:automatic-styles><office:body><office:text>");
    buf_add(&c, body.p ? body.p : "", body.n);
    buf_str(&c, "</office:text></office:body></office:document-content>");

    buf_str(&man, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                  "<manifest:manifest xmlns:manifest=\"urn:oasis:names:tc:opendocument:xmlns:manifest:1.0\" manifest:version=\"1.3\">"
                  "<manifest:file-entry manifest:full-path=\"/\" manifest:version=\"1.3\" manifest:media-type=\"application/vnd.oasis.opendocument.text\"/>"
                  "<manifest:file-entry manifest:full-path=\"content.xml\" manifest:media-type=\"text/xml\"/>"
                  "<manifest:file-entry manifest:full-path=\"styles.xml\" manifest:media-type=\"text/xml\"/>"
                  "<manifest:file-entry manifest:full-path=\"meta.xml\" manifest:media-type=\"text/xml\"/>");
    for (int i = 1; i <= nimg; i++)
        buf_printf(&man, "<manifest:file-entry manifest:full-path=\"Pictures/image%d.png\" manifest:media-type=\"image/png\"/>", i);
    buf_str(&man, "</manifest:manifest>");

    if (!(w = zw_open(path))) { lstrcpynW(err, L"The file cannot be created.", cch); goto fail; }
    {
        static const char mime[] = "application/vnd.oasis.opendocument.text";
        Buf styles = { 0 };
        static const char meta[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
            "xmlns:meta=\"urn:oasis:names:tc:opendocument:xmlns:meta:1.0\" office:version=\"1.3\">"
            "<office:meta><meta:generator>Stained Glass WordPad</meta:generator></office:meta></office:document-meta>";
        buf_str(&styles, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<office:document-styles xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
            "xmlns:style=\"urn:oasis:names:tc:opendocument:xmlns:style:1.0\" "
            "xmlns:fo=\"urn:oasis:names:tc:opendocument:xmlns:xsl-fo-compatible:1.0\" office:version=\"1.3\">"
            "<office:styles><style:default-style style:family=\"paragraph\">"
            "<style:text-properties style:font-name=\"Calibri\" fo:font-family=\"Calibri\" fo:font-size=\"11pt\"/>"
            "</style:default-style></office:styles><office:automatic-styles><style:page-layout style:name=\"pm1\">"
            "<style:page-layout-properties");
        len_in(&styles, "fo:page-width", g_pagew); len_in(&styles, "fo:page-height", g_pageh);
        len_in(&styles, "fo:margin-top", g_margins.top); len_in(&styles, "fo:margin-bottom", g_margins.bottom);
        len_in(&styles, "fo:margin-left", g_margins.left); len_in(&styles, "fo:margin-right", g_margins.right);
        buf_str(&styles, "/></style:page-layout></office:automatic-styles><office:master-styles>"
            "<style:master-page style:name=\"Standard\" style:page-layout-name=\"pm1\"/></office:master-styles></office:document-styles>");
        r |= zw_add_mem(w, L"mimetype", mime, sizeof(mime) - 1, TRUE);
        r |= zw_add_mem(w, L"META-INF/manifest.xml", man.p, (DWORD)man.n, FALSE);
        r |= zw_add_mem(w, L"content.xml", c.p, (DWORD)c.n, FALSE);
        r |= zw_add_mem(w, L"styles.xml", styles.p, (DWORD)styles.n, FALSE);
        r |= zw_add_mem(w, L"meta.xml", meta, sizeof(meta) - 1, FALSE);
        free(styles.p);
        for (int i = 0; i < nimg; i++)
        {
            WCHAR nm[64];
            swprintf(nm, 64, L"Pictures/image%d.png", i + 1);
            r |= zw_add_mem(w, nm, imgs[i].png, imgs[i].size, TRUE);
        }
        if (zw_close(w, !r) || r) { lstrcpynW(err, L"The file cannot be written.", cch); goto fail; }
    }
    for (int i = 0; i < nimg; i++) free(imgs[i].png);
    free(imgs); free(body.p); free(autos.p); free(c.p); free(man.p); free(pstyles); free(tstyles);
    return TRUE;
fail:
    for (int i = 0; i < nimg; i++) free(imgs[i].png);
    free(imgs); free(body.p); free(autos.p); free(c.p); free(man.p); free(pstyles); free(tstyles);
    return FALSE;
}
