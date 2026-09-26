/* sg-wordpad -- Office Open XML documents (.docx): our own reader and writer
 * of the WordprocessingML subset WordPad's model has, written from ECMA-376
 * (Part 1: document, styles, numbering, relationships; Part 2: packaging).
 *
 * Read: paragraphs (w:p) with alignment, indents, spacing, tab stops, list
 * membership (numbering.xml's first level: bullet or number format) and
 * paragraph styles (their run and paragraph properties, following basedOn);
 * runs (w:r) with bold, italic, underline, strike, super/subscript, size,
 * font, colour, highlight or shading, character styles; text, tabs, breaks,
 * symbols; pictures (w:drawing's a:blip, VML v:imagedata) from the package;
 * hyperlinks', insertions', smart tags' and content controls' runs; tables
 * flattened to their paragraphs. Deletions are skipped.
 *
 * Write: the same, one w:p per paragraph, pictures as PNG parts, a
 * numbering part for the lists used, a styles part with the defaults.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "wordpad.h"
#include "../zip/zipcore.h"

/* ---------------------------------------------------------------- package helpers */

static int zfind(zarchive *z, const char *name)
{
    WCHAR w[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, MAX_PATH);
    for (int i = 0; i < z->n; i++) if (!lstrcmpiW(z->e[i].name, w)) return i;
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

static BOOL on_off(const XNode *x)
{
    const char *v = xml_attr(x, "val");
    if (!v) return TRUE;
    return !(!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "none"));
}

static int ival(const XNode *x, const char *attr, int def)
{
    const char *v = xml_attr(x, attr);
    return v ? atoi(v) : def;
}

static COLORREF hex_color(const char *v, BOOL *ok)
{
    unsigned c;
    *ok = FALSE;
    if (!v || strlen(v) < 6 || !strcmp(v, "auto")) return 0;
    if (v[0] == '#') v++;
    c = (unsigned)strtoul(v, NULL, 16);
    *ok = TRUE;
    return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255);
}

static const struct { const char *name; COLORREF c; } HL[] = {
    { "yellow", RGB(255, 255, 0) }, { "green", RGB(0, 255, 0) }, { "cyan", RGB(0, 255, 255) },
    { "magenta", RGB(255, 0, 255) }, { "blue", RGB(0, 0, 255) }, { "red", RGB(255, 0, 0) },
    { "darkBlue", RGB(0, 0, 128) }, { "darkCyan", RGB(0, 128, 128) }, { "darkGreen", RGB(0, 128, 0) },
    { "darkMagenta", RGB(128, 0, 128) }, { "darkRed", RGB(128, 0, 0) }, { "darkYellow", RGB(128, 128, 0) },
    { "darkGray", RGB(128, 128, 128) }, { "lightGray", RGB(192, 192, 192) }, { "black", RGB(0, 0, 0) },
    { "white", RGB(255, 255, 255) },
};

/* ---------------------------------------------------------------- reading */

typedef struct {
    char *id;
    const XNode *rpr, *ppr;
    char *based;
} Style;

typedef struct {
    zarchive *z;
    Doc *d;
    Style *styles; int nstyles;
    XNode *stylesx, *numx, *relsx;
    CharProps base;             /* docDefaults */
    Para basepara;
    int in_table;               /* a table inside a cell is read as the cell's paragraphs */
} Reader;

static const Style *find_style(Reader *r, const char *id)
{
    if (!id) return NULL;
    for (int i = 0; i < r->nstyles; i++) if (!strcmp(r->styles[i].id, id)) return &r->styles[i];
    return NULL;
}

static void apply_rpr(const XNode *rpr, CharProps *cp)
{
    if (!rpr) return;
    for (int i = 0; i < rpr->nkids; i++)
    {
        const XNode *k = rpr->kids[i];
        const char *n = k->name ? xml_local(k->name) : NULL;
        BOOL ok;
        if (!n) continue;
        if (!strcmp(n, "b")) cp->bold = on_off(k);
        else if (!strcmp(n, "i")) cp->italic = on_off(k);
        else if (!strcmp(n, "u")) cp->underline = on_off(k);
        else if (!strcmp(n, "strike") || !strcmp(n, "dstrike")) cp->strike = on_off(k);
        else if (!strcmp(n, "vertAlign"))
        {
            const char *v = xml_attr(k, "val");
            cp->script = v && !strcmp(v, "superscript") ? 1 : v && !strcmp(v, "subscript") ? -1 : 0;
        }
        else if (!strcmp(n, "sz")) { int v = ival(k, "val", 0); if (v > 0) cp->hps = v; }
        else if (!strcmp(n, "rFonts"))
        {
            const char *f = xml_attr(k, "ascii");
            if (!f) f = xml_attr(k, "hAnsi");
            if (f) MultiByteToWideChar(CP_UTF8, 0, f, -1, cp->font, 64);
        }
        else if (!strcmp(n, "color"))
        {
            COLORREF c = hex_color(xml_attr(k, "val"), &ok);
            cp->has_color = ok; cp->color = c;
        }
        else if (!strcmp(n, "highlight"))
        {
            const char *v = xml_attr(k, "val");
            cp->has_hl = 0;
            for (int j = 0; v && j < (int)ARRAYSIZE(HL); j++) if (!strcmp(v, HL[j].name)) { cp->has_hl = 1; cp->hl = HL[j].c; }
        }
        else if (!strcmp(n, "shd"))
        {
            COLORREF c = hex_color(xml_attr(k, "fill"), &ok);
            if (ok && c != RGB(255, 255, 255)) { cp->has_hl = 1; cp->hl = c; }
        }
    }
}

static void apply_ppr(Reader *r, const XNode *ppr, Para *p);

static void style_rpr(Reader *r, const char *id, CharProps *cp, int depth)
{
    const Style *s = find_style(r, id);
    if (!s || depth > 10) return;
    if (s->based) style_rpr(r, s->based, cp, depth + 1);
    apply_rpr(s->rpr, cp);
}

static void style_ppr(Reader *r, const char *id, Para *p, int depth)
{
    const Style *s = find_style(r, id);
    if (!s || depth > 10) return;
    if (s->based) style_ppr(r, s->based, p, depth + 1);
    apply_ppr(r, s->ppr, p);
}

/* numbering.xml: numId -> abstractNumId -> the first level's numFmt */
static int list_style(Reader *r, int numid, int ilvl)
{
    const XNode *root = xml_child(r->numx, "numbering");
    char want[16];
    const char *abs = NULL;
    if (!root || numid <= 0) return numid > 0 ? LS_BULLET : LS_NONE;
    snprintf(want, sizeof(want), "%d", numid);
    for (int i = 0; i < root->nkids; i++)
    {
        const XNode *k = root->kids[i];
        if (k->name && !strcmp(xml_local(k->name), "num") && xml_attr(k, "numId") && !strcmp(xml_attr(k, "numId"), want))
        {
            const XNode *a = xml_child(k, "abstractNumId");
            abs = a ? xml_attr(a, "val") : NULL;
        }
    }
    for (int i = 0; abs && i < root->nkids; i++)
    {
        const XNode *k = root->kids[i];
        if (k->name && !strcmp(xml_local(k->name), "abstractNum") && xml_attr(k, "abstractNumId") &&
            !strcmp(xml_attr(k, "abstractNumId"), abs))
        {
            for (int j = 0; j < k->nkids; j++)
            {
                const XNode *l = k->kids[j];
                if (l->name && !strcmp(xml_local(l->name), "lvl") && ival(l, "ilvl", 0) == ilvl)
                {
                    const XNode *f = xml_child(l, "numFmt");
                    const char *v = f ? xml_attr(f, "val") : NULL;
                    if (!v || !strcmp(v, "bullet")) return LS_BULLET;
                    if (!strcmp(v, "lowerLetter")) return LS_LALPHA;
                    if (!strcmp(v, "upperLetter")) return LS_UALPHA;
                    if (!strcmp(v, "lowerRoman")) return LS_LROMAN;
                    if (!strcmp(v, "upperRoman")) return LS_UROMAN;
                    if (!strcmp(v, "none")) return LS_NONE;
                    return LS_DECIMAL;
                }
            }
        }
    }
    return LS_BULLET;
}

static void apply_ppr(Reader *r, const XNode *ppr, Para *p)
{
    if (!ppr) return;
    for (int i = 0; i < ppr->nkids; i++)
    {
        const XNode *k = ppr->kids[i];
        const char *n = k->name ? xml_local(k->name) : NULL;
        if (!n) continue;
        if (!strcmp(n, "jc"))
        {
            const char *v = xml_attr(k, "val");
            if (!v) continue;
            if (!strcmp(v, "center")) p->align = AL_CENTER;
            else if (!strcmp(v, "right") || !strcmp(v, "end")) p->align = AL_RIGHT;
            else if (!strcmp(v, "both") || !strcmp(v, "distribute")) p->align = AL_JUSTIFY;
            else p->align = AL_LEFT;
        }
        else if (!strcmp(n, "ind"))
        {
            const char *v;
            if ((v = xml_attr(k, "left")) || (v = xml_attr(k, "start"))) p->left = atoi(v);
            if ((v = xml_attr(k, "right")) || (v = xml_attr(k, "end"))) p->right = atoi(v);
            if ((v = xml_attr(k, "firstLine"))) p->first = atoi(v);
            if ((v = xml_attr(k, "hanging"))) p->first = -atoi(v);
        }
        else if (!strcmp(n, "spacing"))
        {
            const char *v, *rule = xml_attr(k, "lineRule");
            if ((v = xml_attr(k, "before"))) p->before = atoi(v);
            if ((v = xml_attr(k, "after"))) p->after = atoi(v);
            if ((v = xml_attr(k, "line")))
            {
                int l = atoi(v);
                if (!rule || !strcmp(rule, "auto")) p->line = l > 0 ? l : 240;
                else p->line = -l;          /* exact / at least: twips */
            }
        }
        else if (!strcmp(n, "numPr"))
        {
            const XNode *id = xml_child(k, "numId"), *lv = xml_child(k, "ilvl");
            p->list = list_style(r, id ? ival(id, "val", 0) : 0, lv ? ival(lv, "val", 0) : 0);
        }
        else if (!strcmp(n, "tabs"))
        {
            p->ntabs = 0;
            for (int j = 0; j < k->nkids && p->ntabs < 32; j++)
            {
                const XNode *t = k->kids[j];
                const char *v = xml_attr(t, "val");
                if (t->name && !strcmp(xml_local(t->name), "tab") && (!v || strcmp(v, "clear")))
                    p->tabs[p->ntabs++] = ival(t, "pos", 0);
            }
        }
    }
}

/* a picture part named by a relationship id */
static BOOL rel_target(Reader *r, const char *id, char *out, size_t cch)
{
    const XNode *root = xml_child(r->relsx, "Relationships");
    if (!root || !id) return FALSE;
    for (int i = 0; i < root->nkids; i++)
    {
        const XNode *k = root->kids[i];
        const char *rid = xml_attr(k, "Id"), *t = xml_attr(k, "Target");
        if (rid && t && !strcmp(rid, id))
        {
            if (t[0] == '/') snprintf(out, cch, "%s", t + 1);
            else if (!strncmp(t, "../", 3)) snprintf(out, cch, "%s", t + 3);
            else snprintf(out, cch, "word/%s", t);
            return TRUE;
        }
    }
    return FALSE;
}

static void add_picture(Reader *r, Para *p, const char *rid, int w, int h, const CharProps *cp)
{
    char name[MAX_PATH];
    size_t n;
    char *data;
    Run *run;
    const char *ext;
    if (!rel_target(r, rid, name, sizeof(name))) return;
    if (!(data = zget(r->z, name, &n))) return;
    run = para_add_run(p);
    ext = strrchr(name, '.');
    run->pic = ext && (!_stricmp(ext, ".jpg") || !_stricmp(ext, ".jpeg")) ? PIC_JPEG :
               ext && !_stricmp(ext, ".emf") ? PIC_EMF : ext && !_stricmp(ext, ".wmf") ? PIC_WMF : PIC_PNG;
    if (run->pic == PIC_WMF && n > 22 && *(DWORD *)data == 0x9AC6CDD7)      /* placeable header: drop it */
    {
        memmove(data, data + 22, n - 22);
        n -= 22;
    }
    run->data = (BYTE *)data;
    run->size = (DWORD)n;
    run->w = w; run->h = h;
    run->cp = *cp;
}

static const XNode *find_desc(const XNode *x, const char *local)
{
    if (!x) return NULL;
    if (x->name && !strcmp(xml_local(x->name), local)) return x;
    for (int i = 0; i < x->nkids; i++) { const XNode *f = find_desc(x->kids[i], local); if (f) return f; }
    return NULL;
}

static void read_run(Reader *r, const XNode *run, Para *p, const CharProps *pcp)
{
    CharProps cp = *pcp;
    const XNode *rpr = xml_child(run, "rPr");
    if (rpr)
    {
        const XNode *rs = xml_child(rpr, "rStyle");
        if (rs) style_rpr(r, xml_attr(rs, "val"), &cp, 0);
        apply_rpr(rpr, &cp);
    }
    for (int i = 0; i < run->nkids; i++)
    {
        const XNode *k = run->kids[i];
        const char *n = k->name ? xml_local(k->name) : NULL;
        if (!n) continue;
        if (!strcmp(n, "t"))
        {
            for (int j = 0; j < k->nkids; j++)
                if (k->kids[j]->text)
                {
                    int wl = MultiByteToWideChar(CP_UTF8, 0, k->kids[j]->text, -1, NULL, 0);
                    WCHAR *w = malloc(wl * sizeof(WCHAR));
                    MultiByteToWideChar(CP_UTF8, 0, k->kids[j]->text, -1, w, wl);
                    para_add_text(p, &cp, w, wl - 1);
                    free(w);
                }
        }
        else if (!strcmp(n, "tab")) para_add_text(p, &cp, L"\t", 1);
        else if (!strcmp(n, "br") || !strcmp(n, "cr")) para_add_text(p, &cp, L"\v", 1);
        else if (!strcmp(n, "noBreakHyphen")) para_add_text(p, &cp, L"\x2011", 1);
        else if (!strcmp(n, "sym"))
        {
            const char *c = xml_attr(k, "char");
            if (c) { WCHAR w = (WCHAR)strtoul(c, NULL, 16); if (w >= 0xF000) w -= 0xF000; para_add_text(p, &cp, &w, 1); }
        }
        else if (!strcmp(n, "drawing"))
        {
            const XNode *blip = find_desc(k, "blip"), *ext = find_desc(k, "extent");
            int w = ext ? (int)(_atoi64(xml_attr(ext, "cx") ? xml_attr(ext, "cx") : "0") / 635) : 0;
            int h = ext ? (int)(_atoi64(xml_attr(ext, "cy") ? xml_attr(ext, "cy") : "0") / 635) : 0;
            if (blip) add_picture(r, p, xml_attr(blip, "embed"), w, h, &cp);
        }
        else if (!strcmp(n, "pict") || !strcmp(n, "object"))
        {
            const XNode *im = find_desc(k, "imagedata");
            if (im) add_picture(r, p, xml_attr(im, "id"), 0, 0, &cp);
        }
    }
}

static void read_inline(Reader *r, const XNode *x, Para *p, const CharProps *cp)
{
    for (int i = 0; i < x->nkids; i++)
    {
        const XNode *k = x->kids[i];
        const char *n = k->name ? xml_local(k->name) : NULL;
        if (!n) continue;
        if (!strcmp(n, "r")) read_run(r, k, p, cp);
        else if (!strcmp(n, "hyperlink") || !strcmp(n, "ins") || !strcmp(n, "smartTag") || !strcmp(n, "fldSimple") ||
                 !strcmp(n, "customXml") || !strcmp(n, "sdtContent") || !strcmp(n, "sdt") || !strcmp(n, "moveTo"))
            read_inline(r, k, p, cp);
    }
}

static void read_block(Reader *r, const XNode *x, int depth);

/* a w:tbl: its rows become Rows, each cell's paragraphs are marked with it */
static void read_table(Reader *r, const XNode *tbl, int depth)
{
    const XNode *grid = xml_child(tbl, "tblGrid");
    int gridw[MAX_CELLS], ngrid = 0;
    for (int i = 0; grid && i < grid->nkids && ngrid < MAX_CELLS; i++)
        if (grid->kids[i]->name && !strcmp(xml_local(grid->kids[i]->name), "gridCol"))
            gridw[ngrid++] = ival(grid->kids[i], "w", 0);
    r->in_table++;
    for (int i = 0; i < tbl->nkids; i++)
    {
        const XNode *tr = tbl->kids[i];
        Row row;
        int x = 0, col = 0, first = r->d->n, cell = 0, n;
        if (!tr->name || strcmp(xml_local(tr->name), "tr")) continue;
        memset(&row, 0, sizeof(row));
        for (int j = 0; j < tr->nkids && row.ncells < MAX_CELLS; j++)
        {
            const XNode *tc = tr->kids[j], *pr, *w, *span;
            int width = 0, spans = 1, start = r->d->n;
            if (!tc->name || strcmp(xml_local(tc->name), "tc")) continue;
            pr = xml_child(tc, "tcPr");
            if ((span = xml_child(pr, "gridSpan"))) spans = max(1, ival(span, "val", 1));
            if ((w = xml_child(pr, "tcW")) && (!xml_attr(w, "type") || !strcmp(xml_attr(w, "type"), "dxa")))
                width = ival(w, "w", 0);
            if (width <= 0)
                for (int k = 0; k < spans; k++) width += col + k < ngrid ? gridw[col + k] : 0;
            if (width <= 0) width = 2000;
            col += spans;
            x += width;
            row.cellx[row.ncells++] = x;
            read_block(r, tc, depth + 1);
            if (r->d->n == start) { Para *p = doc_add_para(r->d); *p = r->basepara; p->runs = NULL; p->nruns = p->cap = 0; }
            for (int k = start; k < r->d->n; k++) { r->d->p[k].cell = cell; r->d->p[k].cell_end = k == r->d->n - 1; r->d->p[k].row = -1; }
            cell++;
        }
        if (!row.ncells) continue;
        n = doc_add_row(r->d, &row);
        for (int k = first; k < r->d->n; k++) if (r->d->p[k].row == -1) r->d->p[k].row = n;
    }
    r->in_table--;
}

static void read_block(Reader *r, const XNode *x, int depth)
{
    if (depth > 32) return;
    for (int i = 0; i < x->nkids; i++)
    {
        const XNode *k = x->kids[i];
        const char *n = k->name ? xml_local(k->name) : NULL;
        if (!n) continue;
        if (!strcmp(n, "p"))
        {
            Para *p = doc_add_para(r->d);
            const XNode *ppr = xml_child(k, "pPr");
            CharProps cp = r->base;
            Run *runs;
            *p = r->basepara;
            p->runs = NULL; p->nruns = p->cap = 0;
            if (ppr)
            {
                const XNode *ps = xml_child(ppr, "pStyle");
                if (ps) { style_ppr(r, xml_attr(ps, "val"), p, 0); style_rpr(r, xml_attr(ps, "val"), &cp, 0); }
                else { style_ppr(r, "Normal", p, 0); style_rpr(r, "Normal", &cp, 0); }
                apply_ppr(r, ppr, p);
            }
            else { style_ppr(r, "Normal", p, 0); style_rpr(r, "Normal", &cp, 0); }
            runs = p->runs; (void)runs;
            read_inline(r, k, p, &cp);
        }
        else if (!strcmp(n, "tbl") && !r->in_table) read_table(r, k, depth + 1);
        else if (!strcmp(n, "tbl") || !strcmp(n, "tr") || !strcmp(n, "tc") || !strcmp(n, "sdt") ||
                 !strcmp(n, "sdtContent") || !strcmp(n, "customXml") || !strcmp(n, "ins"))
            read_block(r, k, depth + 1);
    }
}

BOOL docx_read(const WCHAR *path, Doc *d, WCHAR *err, int cch)
{
    zarchive z;
    Reader r;
    XNode *doc, *body;
    const XNode *root;
    memset(&r, 0, sizeof(r));
    if (zip_open(&z, path)) { lstrcpynW(err, L"It is not an Office Open XML document.", cch); return FALSE; }
    r.z = &z; r.d = d;
    if (!(doc = zxml(&z, "word/document.xml")))
    {
        zip_close(&z);
        lstrcpynW(err, L"The document has no main part (word/document.xml).", cch);
        return FALSE;
    }
    r.stylesx = zxml(&z, "word/styles.xml");
    r.numx = zxml(&z, "word/numbering.xml");
    r.relsx = zxml(&z, "word/_rels/document.xml.rels");
    cp_default(&r.base);
    r.basepara.line = 240;
    if ((root = xml_child(r.stylesx, "styles")))
    {
        const XNode *dd = xml_child(root, "docDefaults");
        if (dd)
        {
            const XNode *rd = xml_child(dd, "rPrDefault"), *pd = xml_child(dd, "pPrDefault");
            if (rd) apply_rpr(xml_child(rd, "rPr"), &r.base);
            if (pd) apply_ppr(&r, xml_child(pd, "pPr"), &r.basepara);
        }
        for (int i = 0; i < root->nkids; i++)
        {
            const XNode *s = root->kids[i];
            const XNode *b;
            if (!s->name || strcmp(xml_local(s->name), "style") || !xml_attr(s, "styleId")) continue;
            r.styles = realloc(r.styles, (r.nstyles + 1) * sizeof(Style));
            r.styles[r.nstyles].id = _strdup(xml_attr(s, "styleId"));
            r.styles[r.nstyles].rpr = xml_child(s, "rPr");
            r.styles[r.nstyles].ppr = xml_child(s, "pPr");
            b = xml_child(s, "basedOn");
            r.styles[r.nstyles].based = b && xml_attr(b, "val") ? _strdup(xml_attr(b, "val")) : NULL;
            r.nstyles++;
        }
    }
    body = xml_child(xml_child(doc, "document"), "body");
    if (body) read_block(&r, body, 0);
    if (!d->n) doc_add_para(d);
    for (int i = 0; i < r.nstyles; i++) { free(r.styles[i].id); free(r.styles[i].based); }
    free(r.styles);
    xml_free(doc); xml_free(r.stylesx); xml_free(r.numx); xml_free(r.relsx);
    zip_close(&z);
    return TRUE;
}

/* ---------------------------------------------------------------- writing */

static void utf8(Buf *b, const WCHAR *s)
{
    buf_xml(b, s, lstrlenW(s));
}

static const char *hl_name(COLORREF c)
{
    for (int j = 0; j < (int)ARRAYSIZE(HL); j++) if (HL[j].c == c) return HL[j].name;
    return NULL;
}

static void write_rpr(Buf *b, const CharProps *cp)
{
    buf_str(b, "<w:rPr>");
    if (cp->font[0])
    {
        buf_str(b, "<w:rFonts w:ascii=\""); utf8(b, cp->font);
        buf_str(b, "\" w:hAnsi=\""); utf8(b, cp->font);
        buf_str(b, "\" w:cs=\""); utf8(b, cp->font);
        buf_str(b, "\"/>");
    }
#ifndef SG_MUTANT_DOCXRPR
    if (cp->bold) buf_str(b, "<w:b/>");
#endif
    if (cp->italic) buf_str(b, "<w:i/>");
    if (cp->strike) buf_str(b, "<w:strike/>");
    if (cp->has_color) buf_printf(b, "<w:color w:val=\"%02X%02X%02X\"/>", GetRValue(cp->color), GetGValue(cp->color), GetBValue(cp->color));
    if (cp->hps) buf_printf(b, "<w:sz w:val=\"%d\"/><w:szCs w:val=\"%d\"/>", cp->hps, cp->hps);
    if (cp->has_hl)
    {
        const char *n = hl_name(cp->hl);
        if (n) buf_printf(b, "<w:highlight w:val=\"%s\"/>", n);
        else buf_printf(b, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%02X%02X%02X\"/>", GetRValue(cp->hl), GetGValue(cp->hl), GetBValue(cp->hl));
    }
    if (cp->underline) buf_str(b, "<w:u w:val=\"single\"/>");
    if (cp->script) buf_printf(b, "<w:vertAlign w:val=\"%s\"/>", cp->script > 0 ? "superscript" : "subscript");
    buf_str(b, "</w:rPr>");
}

static void write_text(Buf *b, const WCHAR *s, int n)
{
    int st = 0;
    for (int i = 0; i <= n; i++)
    {
        if (i == n || s[i] == '\t' || s[i] == '\v')
        {
            if (i > st)
            {
                buf_str(b, "<w:t xml:space=\"preserve\">");
                buf_xml(b, s + st, i - st);
                buf_str(b, "</w:t>");
            }
            if (i < n) buf_str(b, s[i] == '\t' ? "<w:tab/>" : "<w:br/>");
            st = i + 1;
        }
    }
}

static const char *NUMFMT[] = { "", "bullet", "decimal", "lowerLetter", "upperLetter", "lowerRoman", "upperRoman" };

BOOL docx_write(const WCHAR *path, const Doc *d, WCHAR *err, int cch)
{
    Buf doc = { 0 }, rels = { 0 }, ct = { 0 }, num = { 0 };
    zwriter *w;
    int nimg = 0, used[7] = { 0 }, r = 0;
    struct { BYTE *png; DWORD size; } *imgs = NULL;

    buf_str(&doc, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                  "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
                  "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
                  "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
                  "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                  "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><w:body>");
    for (int i = 0; i < d->n; i++)
    {
        const Para *p = &d->p[i];
#ifdef SG_MUTANT_NOTABLE
        if (0)
#else
        if (p->row > 0 && p->row <= d->nrows)
#endif
        {
            const Row *row = &d->rows[p->row - 1];
            BOOL row_start = i == 0 || d->p[i - 1].row != p->row;
            if (i == 0 || !(d->p[i - 1].row > 0))
            {
                buf_str(&doc, "<w:tbl><w:tblPr><w:tblW w:w=\"0\" w:type=\"auto\"/><w:tblBorders>"
                              "<w:top w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
                              "<w:left w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
                              "<w:bottom w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
                              "<w:right w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
                              "<w:insideH w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
                              "<w:insideV w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"auto\"/>"
                              "</w:tblBorders><w:tblLook w:val=\"04A0\"/></w:tblPr><w:tblGrid>");
                for (int c = 0; c < row->ncells; c++)
                    buf_printf(&doc, "<w:gridCol w:w=\"%d\"/>", row->cellx[c] - (c ? row->cellx[c - 1] : 0));
                buf_str(&doc, "</w:tblGrid>");
            }
            if (row_start) buf_str(&doc, "<w:tr>");
            if (row_start || d->p[i - 1].cell_end)
                buf_printf(&doc, "<w:tc><w:tcPr><w:tcW w:w=\"%d\" w:type=\"dxa\"/></w:tcPr>",
                           p->cell < row->ncells ? row->cellx[p->cell] - (p->cell ? row->cellx[p->cell - 1] : 0) : 2000);
        }
        buf_str(&doc, "<w:p><w:pPr>");
        if (p->list) { used[p->list] = 1; buf_printf(&doc, "<w:numPr><w:ilvl w:val=\"0\"/><w:numId w:val=\"%d\"/></w:numPr>", p->list); }
        if (p->ntabs)
        {
            buf_str(&doc, "<w:tabs>");
            for (int t = 0; t < p->ntabs; t++) buf_printf(&doc, "<w:tab w:val=\"left\" w:pos=\"%d\"/>", p->tabs[t]);
            buf_str(&doc, "</w:tabs>");
        }
        buf_printf(&doc, "<w:spacing w:before=\"%d\" w:after=\"%d\" ", p->before, p->after);
        if (p->line >= 0) buf_printf(&doc, "w:line=\"%d\" w:lineRule=\"auto\"/>", p->line ? p->line : 240);
        else buf_printf(&doc, "w:line=\"%d\" w:lineRule=\"exact\"/>", -p->line);
        if (p->left || p->right || p->first)
        {
            buf_printf(&doc, "<w:ind w:left=\"%d\" w:right=\"%d\" ", p->left, p->right);
            if (p->first >= 0) buf_printf(&doc, "w:firstLine=\"%d\"/>", p->first);
            else buf_printf(&doc, "w:hanging=\"%d\"/>", -p->first);
        }
        if (p->align) buf_printf(&doc, "<w:jc w:val=\"%s\"/>", p->align == AL_CENTER ? "center" : p->align == AL_RIGHT ? "right" : "both");
        buf_str(&doc, "</w:pPr>");
        for (int j = 0; j < p->nruns; j++)
        {
            const Run *run = &p->runs[j];
            if (run->pic)
            {
                BYTE *png;
                DWORD size;
                int wpx, hpx;
                long long cx, cy;
                if (!pic_to_png(run->pic, run->data, run->size, run->w, run->h, &png, &size, &wpx, &hpx)) continue;
                imgs = realloc(imgs, (nimg + 1) * sizeof(*imgs));
                imgs[nimg].png = png; imgs[nimg].size = size;
                nimg++;
                cx = (long long)(run->w ? run->w : wpx * 15) * 635;
                cy = (long long)(run->h ? run->h : hpx * 15) * 635;
                buf_printf(&doc, "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">"
                                 "<wp:extent cx=\"%lld\" cy=\"%lld\"/><wp:docPr id=\"%d\" name=\"Picture %d\"/>"
                                 "<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                                 "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"%d\" name=\"image%d.png\"/><pic:cNvPicPr/></pic:nvPicPr>"
                                 "<pic:blipFill><a:blip r:embed=\"rIdImg%d\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                                 "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%lld\" cy=\"%lld\"/></a:xfrm>"
                                 "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr></pic:pic>"
                                 "</a:graphicData></a:graphic></wp:inline></w:drawing></w:r>",
                           cx, cy, nimg, nimg, nimg, nimg, nimg, cx, cy);
                continue;
            }
            buf_str(&doc, "<w:r>");
            write_rpr(&doc, &run->cp);
            write_text(&doc, run->text, run->len);
            buf_str(&doc, "</w:r>");
        }
        buf_str(&doc, "</w:p>");
#ifdef SG_MUTANT_NOTABLE
        if (0)
#else
        if (p->row > 0 && p->row <= d->nrows)
#endif
        {
            const Row *row = &d->rows[p->row - 1];
            BOOL last = i == d->n - 1 || d->p[i + 1].row != p->row;
            if (p->cell_end || last) buf_str(&doc, "</w:tc>");
            if (last)
            {
                for (int c = p->cell + 1; c < row->ncells; c++)
                    buf_printf(&doc, "<w:tc><w:tcPr><w:tcW w:w=\"%d\" w:type=\"dxa\"/></w:tcPr><w:p/></w:tc>",
                               row->cellx[c] - row->cellx[c - 1]);
                buf_str(&doc, "</w:tr>");
                if (i == d->n - 1 || !(d->p[i + 1].row > 0)) buf_str(&doc, "</w:tbl>");
            }
        }
    }
    if (d->n && d->p[d->n - 1].row > 0) buf_str(&doc, "<w:p/>");     /* a document does not end in a table */
    buf_printf(&doc, "<w:sectPr><w:pgSz w:w=\"%d\" w:h=\"%d\"/><w:pgMar w:top=\"%ld\" w:right=\"%ld\" w:bottom=\"%ld\" "
                     "w:left=\"%ld\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>",
               g_pagew, g_pageh, g_margins.top, g_margins.right, g_margins.bottom, g_margins.left);

    buf_str(&num, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                  "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">");
    for (int l = 1; l <= LS_UROMAN; l++)
        if (used[l])
            buf_printf(&num, "<w:abstractNum w:abstractNumId=\"%d\"><w:multiLevelType w:val=\"singleLevel\"/>"
                             "<w:lvl w:ilvl=\"0\"><w:start w:val=\"1\"/><w:numFmt w:val=\"%s\"/><w:lvlText w:val=\"%s\"/>"
                             "<w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"720\" w:hanging=\"360\"/></w:pPr>%s</w:lvl></w:abstractNum>",
                       l, NUMFMT[l], l == LS_BULLET ? "\xE2\x80\xA2" : "%1.",
                       l == LS_BULLET ? "<w:rPr><w:rFonts w:ascii=\"Symbol\" w:hAnsi=\"Symbol\" w:hint=\"default\"/></w:rPr>" : "");
    for (int l = 1; l <= LS_UROMAN; l++)
        if (used[l]) buf_printf(&num, "<w:num w:numId=\"%d\"><w:abstractNumId w:val=\"%d\"/></w:num>", l, l);
    buf_str(&num, "</w:numbering>");

    buf_str(&rels, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                   "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                   "<Relationship Id=\"rIdStyles\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
                   "<Relationship Id=\"rIdNumbering\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>");
    for (int i = 1; i <= nimg; i++)
        buf_printf(&rels, "<Relationship Id=\"rIdImg%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" Target=\"media/image%d.png\"/>", i, i);
    buf_str(&rels, "</Relationships>");

    buf_str(&ct, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
                 "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                 "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                 "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                 "<Default Extension=\"png\" ContentType=\"image/png\"/>"
                 "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
                 "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
                 "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
                 "<Override PartName=\"/docProps/app.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.extended-properties+xml\"/>"
                 "</Types>");

    if (!(w = zw_open(path))) { lstrcpynW(err, L"The file cannot be created.", cch); goto fail; }
    {
        static const char pkgrels[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
            "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
            "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
            "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties\" Target=\"docProps/app.xml\"/>"
            "</Relationships>";
        static const char styles[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
            "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
            "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\" w:cs=\"Calibri\"/>"
            "<w:sz w:val=\"22\"/><w:szCs w:val=\"22\"/></w:rPr></w:rPrDefault>"
            "<w:pPrDefault><w:pPr><w:spacing w:after=\"0\" w:line=\"240\" w:lineRule=\"auto\"/></w:pPr></w:pPrDefault></w:docDefaults>"
            "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/></w:style>"
            "</w:styles>";
        static const char app[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
            "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\">"
            "<Application>Stained Glass WordPad</Application></Properties>";
        r |= zw_add_mem(w, L"[Content_Types].xml", ct.p, (DWORD)ct.n, FALSE);
        r |= zw_add_mem(w, L"_rels/.rels", pkgrels, sizeof(pkgrels) - 1, FALSE);
        r |= zw_add_mem(w, L"docProps/app.xml", app, sizeof(app) - 1, FALSE);
        r |= zw_add_mem(w, L"word/document.xml", doc.p, (DWORD)doc.n, FALSE);
        r |= zw_add_mem(w, L"word/styles.xml", styles, sizeof(styles) - 1, FALSE);
        r |= zw_add_mem(w, L"word/numbering.xml", num.p, (DWORD)num.n, FALSE);
        r |= zw_add_mem(w, L"word/_rels/document.xml.rels", rels.p, (DWORD)rels.n, FALSE);
        for (int i = 0; i < nimg; i++)
        {
            WCHAR nm[64];
            swprintf(nm, 64, L"word/media/image%d.png", i + 1);
            r |= zw_add_mem(w, nm, imgs[i].png, imgs[i].size, TRUE);
        }
        if (zw_close(w, !r) || r) { lstrcpynW(err, L"The file cannot be written.", cch); goto fail; }
    }
    for (int i = 0; i < nimg; i++) free(imgs[i].png);
    free(imgs); free(doc.p); free(rels.p); free(ct.p); free(num.p);
    return TRUE;
fail:
    for (int i = 0; i < nimg; i++) free(imgs[i].png);
    free(imgs); free(doc.p); free(rels.p); free(ct.p); free(num.p);
    return FALSE;
}
