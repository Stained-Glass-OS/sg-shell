/* sg-wordpad -- a document as paragraphs of runs, and RTF to and from it.
 *
 * RTF -> model reads what RichEdit writes (EM_STREAMOUT), and the common
 * parts of what other programs write: groups, the font and colour tables,
 * \u with \uc, \'hh in the document's or font's code page, character
 * properties (\b \i \ul \strike \super \sub \fs \f \cf \highlight \cb),
 * paragraph properties (\q* \li \ri \fi \sb \sa \sl \slmult \tx), RichEdit's
 * bullets and numbering (\pn), pictures (\pict: EMF, WMF, DIB, PNG, JPEG),
 * fields' results; everything else is skipped by group. Written from the
 * RTF 1.9.1 specification.
 *
 * model -> RTF is what EM_STREAMIN reads back.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "wordpad.h"
#include <stdarg.h>
#include <ctype.h>

/* ---------------------------------------------------------------- buffers */

void buf_add(Buf *b, const void *s, size_t n)
{
    if (b->n + n + 1 > b->cap)
    {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        b->p = realloc(b->p, cap);
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

void buf_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }

void buf_printf(Buf *b, const char *fmt, ...)
{
    char tmp[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n < (int)sizeof(tmp)) { buf_add(b, tmp, n); return; }
    {
        char *big = malloc(n + 1);
        va_start(ap, fmt);
        vsnprintf(big, n + 1, fmt, ap);
        va_end(ap);
        buf_add(b, big, n);
        free(big);
    }
}

void buf_xml(Buf *b, const WCHAR *s, int n)
{
    for (int i = 0; i < n; i++)
    {
        WCHAR c = s[i];
        if (c == '&') buf_str(b, "&amp;");
        else if (c == '<') buf_str(b, "&lt;");
        else if (c == '>') buf_str(b, "&gt;");
        else if (c == '"') buf_str(b, "&quot;");
        else if (c < 0x20 && c != '\t' && c != '\n') continue;     /* not allowed in XML 1.0 */
        else
        {
            char u[8];
            int k;
            if (c >= 0xD800 && c < 0xDC00 && i + 1 < n)
            {
                k = WideCharToMultiByte(CP_UTF8, 0, s + i, 2, u, sizeof(u), NULL, NULL);
                i++;
            }
            else k = WideCharToMultiByte(CP_UTF8, 0, &c, 1, u, sizeof(u), NULL, NULL);
            buf_add(b, u, k);
        }
    }
}

/* ---------------------------------------------------------------- the model */

void cp_default(CharProps *cp)
{
    memset(cp, 0, sizeof(*cp));
    lstrcpyW(cp->font, L"Calibri");
    cp->hps = 22;
}

void doc_init(Doc *d) { memset(d, 0, sizeof(*d)); }

int doc_add_row(Doc *d, const Row *r)
{
    d->rows = realloc(d->rows, (d->nrows + 1) * sizeof(Row));
    d->rows[d->nrows] = *r;
    return ++d->nrows;
}

void doc_free(Doc *d)
{
    for (int i = 0; i < d->n; i++)
    {
        Para *p = &d->p[i];
        for (int j = 0; j < p->nruns; j++) { free(p->runs[j].text); free(p->runs[j].data); }
        free(p->runs);
    }
    free(d->p);
    free(d->rows);
    memset(d, 0, sizeof(*d));
}

Para *doc_add_para(Doc *d)
{
    Para *p;
    if (d->n == d->cap)
    {
        d->cap = d->cap ? d->cap * 2 : 32;
        d->p = realloc(d->p, d->cap * sizeof(Para));
    }
    p = &d->p[d->n++];
    memset(p, 0, sizeof(*p));
    p->line = 240;
    return p;
}

Run *para_add_run(Para *p)
{
    Run *r;
    if (p->nruns == p->cap)
    {
        p->cap = p->cap ? p->cap * 2 : 8;
        p->runs = realloc(p->runs, p->cap * sizeof(Run));
    }
    r = &p->runs[p->nruns++];
    memset(r, 0, sizeof(*r));
    return r;
}

static BOOL cp_equal(const CharProps *a, const CharProps *b)
{
    return !lstrcmpiW(a->font, b->font) && a->hps == b->hps && a->bold == b->bold && a->italic == b->italic &&
           a->underline == b->underline && a->strike == b->strike && a->script == b->script &&
           a->has_color == b->has_color && (!a->has_color || a->color == b->color) &&
           a->has_hl == b->has_hl && (!a->has_hl || a->hl == b->hl);
}

void para_add_text(Para *p, const CharProps *cp, const WCHAR *s, int n)
{
    Run *r = p->nruns ? &p->runs[p->nruns - 1] : NULL;
    if (n <= 0) return;
    if (!r || r->pic || !cp_equal(&r->cp, cp))
    {
        r = para_add_run(p);
        r->cp = *cp;
    }
    r->text = realloc(r->text, (r->len + n + 1) * sizeof(WCHAR));
    memcpy(r->text + r->len, s, n * sizeof(WCHAR));
    r->len += n;
    r->text[r->len] = 0;
}

/* ---------------------------------------------------------------- RTF -> model */

enum { RD_TEXT, RD_SKIP, RD_FONTTBL, RD_COLORTBL, RD_PICT, RD_PN, RD_FIELDINST };

typedef struct {
    CharProps cp;
    int font;                   /* \f index, for its code page */
    int uc;
    int dest;
    int fontent;                /* in the font table: the entry's index */
} RState;

typedef struct { int idx; WCHAR name[64]; int charset; } RFont;

typedef struct {
    const char *s; size_t n, i;
    RState st[128]; int depth;
    RFont *fonts; int nfonts;
    COLORREF colors[256]; int ncolors, color_auto0;
    int red, green, blue;
    UINT codepage;
    Doc *doc;
    Para cur;                   /* paragraph properties in force */
    int intbl;                  /* \intbl in force */
    Row rowdef;                 /* the row being defined (\trowd, \cellx) */
    int cur_cell;               /* the cell being filled */
    int row_first;              /* the row's first paragraph, or -1 */
    Buf fontname;
    int skip;                   /* bytes of \u fallback left to skip */
    /* the picture being read */
    Buf pict; int pict_type, picw, pich, goalw, goalh, scalex, scaley, pict_hi, pict_half;
    int pn_list;                /* list style named by the current \pn group */
} Rtf;

/* the paragraph being filled is always the last one; \par starts another */
static Para *open_para(Rtf *r)
{
    if (!r->doc->n) return doc_add_para(r->doc);
    return &r->doc->p[r->doc->n - 1];
}

static void apply_para_props(Rtf *r, Para *p)
{
    Run *runs = p->runs;
    int nruns = p->nruns, cap = p->cap, row = p->row, cell = p->cell, cell_end = p->cell_end;
    int ntabs = r->cur.ntabs;
    *p = r->cur;
    p->runs = runs; p->nruns = nruns; p->cap = cap;
    p->row = row; p->cell = cell; p->cell_end = cell_end;
    p->ntabs = ntabs;
    memcpy(p->tabs, r->cur.tabs, sizeof(p->tabs));
}

static RFont *find_font(Rtf *r, int idx)
{
    for (int i = 0; i < r->nfonts; i++) if (r->fonts[i].idx == idx) return &r->fonts[i];
    return NULL;
}

static UINT charset_cp(int cs)
{
    switch (cs)
    {
    case 0: case 1: return 1252;
    case 2: return CP_SYMBOL;
    case 77: return 10000;
    case 128: return 932;
    case 129: return 949;
    case 134: return 936;
    case 136: return 950;
    case 161: return 1253;
    case 162: return 1254;
    case 163: return 1258;
    case 177: return 1255;
    case 178: return 1256;
    case 186: return 1257;
    case 204: return 1251;
    case 222: return 874;
    case 238: return 1250;
    }
    return 1252;
}

static void emit(Rtf *r, const WCHAR *w, int n)
{
    RState *s = &r->st[r->depth];
    Para *p;
    if (s->dest == RD_FONTTBL) { buf_add(&r->fontname, w, n * sizeof(WCHAR)); return; }
    if (s->dest != RD_TEXT) return;
    p = open_para(r);
    if (!p->nruns) apply_para_props(r, p);
    para_add_text(p, &s->cp, w, n);
}

static void emit_byte(Rtf *r, BYTE b)
{
    RState *s = &r->st[r->depth];
    RFont *f = find_font(r, s->font);
    UINT cp = r->codepage;
    WCHAR w;
    if (f && f->charset && f->charset != 1) cp = charset_cp(f->charset);
    if (cp == CP_SYMBOL) cp = 1252;
    if (!MultiByteToWideChar(cp, 0, (char *)&b, 1, &w, 1)) w = b;
    emit(r, &w, 1);
}

/* ends the paragraph being filled; in a table it belongs to the current
 * cell (cell_end: the \cell that ends the cell) */
static void end_para_ex(Rtf *r, BOOL cell_end)
{
    Para *p = open_para(r);
    if (!p->nruns) apply_para_props(r, p);
    if (r->intbl || cell_end)
    {
        if (r->row_first < 0) r->row_first = r->doc->n - 1;
        p->cell = r->cur_cell;
        p->cell_end = cell_end;
        p->row = -1;                    /* the row's number comes with \row */
    }
    doc_add_para(r->doc);
    if (cell_end) r->cur_cell++;
}

static void end_para(Rtf *r) { end_para_ex(r, FALSE); }

/* \row: the row's paragraphs get its definition */
static void end_row(Rtf *r)
{
    int n, i;
    Row def = r->rowdef;
    if (r->row_first < 0) return;
    if (!def.ncells)
    {
        /* no \cellx: equal cells over six and a half inches */
        def.ncells = max(1, min(r->cur_cell, MAX_CELLS));
        for (i = 0; i < def.ncells; i++) def.cellx[i] = 9360 * (i + 1) / def.ncells;
    }
    n = doc_add_row(r->doc, &def);
    for (i = r->row_first; i < r->doc->n; i++)
        if (r->doc->p[i].row == -1) r->doc->p[i].row = n;
    r->row_first = -1;
    r->cur_cell = 0;
}

static void pict_done(Rtf *r)
{
    Para *p;
    Run *run;
    RState *outer = &r->st[r->depth - 1 >= 0 ? r->depth - 1 : 0];
    if (!r->pict.n || !r->pict_type) { free(r->pict.p); memset(&r->pict, 0, sizeof(r->pict)); return; }
    if (outer->dest != RD_TEXT && outer->dest != RD_PICT) { free(r->pict.p); memset(&r->pict, 0, sizeof(r->pict)); return; }
    p = open_para(r);
    if (!p->nruns) apply_para_props(r, p);
    run = para_add_run(p);
    run->pic = r->pict_type;
    run->data = (BYTE *)r->pict.p;
    run->size = (DWORD)r->pict.n;
    run->w = r->goalw ? r->goalw : r->picw * 15;
    run->h = r->goalh ? r->goalh : r->pich * 15;
    if (r->scalex && r->scalex != 100) run->w = MulDiv(run->w, r->scalex, 100);
    if (r->scaley && r->scaley != 100) run->h = MulDiv(run->h, r->scaley, 100);
    run->cp = outer->cp;
    memset(&r->pict, 0, sizeof(r->pict));
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void control(Rtf *r, const char *w, int has, int v, BOOL star)
{
    RState *s = &r->st[r->depth];
    CharProps *cp = &s->cp;
    if (!has) v = 1;
    if (r->skip && strcmp(w, "par")) { r->skip--; return; }

    /* destinations */
    if (!strcmp(w, "fonttbl")) { s->dest = RD_FONTTBL; return; }
    if (!strcmp(w, "colortbl")) { s->dest = RD_COLORTBL; r->red = r->green = r->blue = 0; return; }
    if (!strcmp(w, "pict")) { s->dest = RD_PICT; r->pict_type = 0; r->picw = r->pich = r->goalw = r->goalh = 0;
                              r->scalex = r->scaley = 100; r->pict_half = 0; memset(&r->pict, 0, sizeof(r->pict)); return; }
    if (!strcmp(w, "shppict")) return;                                  /* its \pict is the picture */
    if (!strcmp(w, "pn")) { s->dest = RD_PN; r->pn_list = 0; return; }
    if (!strcmp(w, "fldinst")) { s->dest = RD_FIELDINST; return; }
    if (!strcmp(w, "fldrslt") || !strcmp(w, "field")) return;           /* the result is the text */
    if (!strcmp(w, "stylesheet") || !strcmp(w, "info") || !strcmp(w, "pntext") || !strcmp(w, "nonshppict") ||
        !strcmp(w, "header") || !strcmp(w, "footer") || !strcmp(w, "headerl") || !strcmp(w, "headerr") ||
        !strcmp(w, "footerl") || !strcmp(w, "footerr") || !strcmp(w, "listtable") || !strcmp(w, "listoverridetable") ||
        !strcmp(w, "footnote") || !strcmp(w, "object") || !strcmp(w, "objdata") || !strcmp(w, "themedata") ||
        !strcmp(w, "colorschememapping") || !strcmp(w, "latentstyles") || !strcmp(w, "datastore") ||
        !strcmp(w, "xmlnstbl") || !strcmp(w, "rsidtbl") || !strcmp(w, "generator") || !strcmp(w, "mmathPr") ||
        !strcmp(w, "pgdsctbl") || !strcmp(w, "filetbl") || !strcmp(w, "revtbl") || !strcmp(w, "bkmkstart") ||
        !strcmp(w, "bkmkend") || !strcmp(w, "annotation") || !strcmp(w, "atnid") || !strcmp(w, "atnauthor"))
    { s->dest = RD_SKIP; return; }
    if (star) { s->dest = RD_SKIP; return; }

    if (s->dest == RD_PN)
    {
        if (!strcmp(w, "pnlvlblt")) r->pn_list = LS_BULLET;
        else if (!strcmp(w, "pndec")) r->pn_list = LS_DECIMAL;
        else if (!strcmp(w, "pnlcltr")) r->pn_list = LS_LALPHA;
        else if (!strcmp(w, "pnucltr")) r->pn_list = LS_UALPHA;
        else if (!strcmp(w, "pnlcrm")) r->pn_list = LS_LROMAN;
        else if (!strcmp(w, "pnucrm")) r->pn_list = LS_UROMAN;
        else if (!strcmp(w, "pnlvlbody") && !r->pn_list) r->pn_list = LS_DECIMAL;
        r->cur.list = r->pn_list;
        return;
    }
    if (s->dest == RD_PICT)
    {
        if (!strcmp(w, "emfblip")) r->pict_type = PIC_EMF;
        else if (!strcmp(w, "pngblip")) r->pict_type = PIC_PNG;
        else if (!strcmp(w, "jpegblip")) r->pict_type = PIC_JPEG;
        else if (!strcmp(w, "wmetafile")) r->pict_type = PIC_WMF;
        else if (!strcmp(w, "dibitmap")) r->pict_type = PIC_DIB;
        else if (!strcmp(w, "picw")) r->picw = v;
        else if (!strcmp(w, "pich")) r->pich = v;
        else if (!strcmp(w, "picwgoal")) r->goalw = v;
        else if (!strcmp(w, "pichgoal")) r->goalh = v;
        else if (!strcmp(w, "picscalex")) r->scalex = v;
        else if (!strcmp(w, "picscaley")) r->scaley = v;
        else if (!strcmp(w, "bin") && has && v > 0)
        {
            size_t n = (size_t)v;
            if (r->i + n > r->n) n = r->n - r->i;
            buf_add(&r->pict, r->s + r->i, n);
            r->i += n;
        }
        return;
    }
    if (s->dest == RD_FONTTBL)
    {
        if (!strcmp(w, "f") && has)
        {
            r->fonts = realloc(r->fonts, (r->nfonts + 1) * sizeof(RFont));
            memset(&r->fonts[r->nfonts], 0, sizeof(RFont));
            r->fonts[r->nfonts].idx = v;
            s->fontent = r->nfonts++;
            r->fontname.n = 0;
        }
        else if (!strcmp(w, "fcharset") && r->nfonts) r->fonts[r->nfonts - 1].charset = v;
        return;
    }
    if (s->dest == RD_COLORTBL)
    {
        if (!strcmp(w, "red")) r->red = v;
        else if (!strcmp(w, "green")) r->green = v;
        else if (!strcmp(w, "blue")) r->blue = v;
        return;
    }
    if (s->dest != RD_TEXT) return;

    if (!strcmp(w, "ansicpg")) { r->codepage = v; return; }
    if (!strcmp(w, "uc")) { s->uc = v; return; }
    if (!strcmp(w, "u"))
    {
        WCHAR c = (WCHAR)(v < 0 ? v + 65536 : v);
        emit(r, &c, 1);
        r->skip = s->uc;
        return;
    }
    if (!strcmp(w, "par") || !strcmp(w, "page") || !strcmp(w, "sect")) { r->skip = 0; end_para(r); return; }
    if (!strcmp(w, "cell") || !strcmp(w, "nestcell")) { r->skip = 0; end_para_ex(r, TRUE); return; }
    if (!strcmp(w, "row") || !strcmp(w, "nestrow")) { end_row(r); return; }
    if (!strcmp(w, "intbl")) { r->intbl = 1; return; }
    if (!strcmp(w, "trowd")) { memset(&r->rowdef, 0, sizeof(r->rowdef)); return; }
    if (!strcmp(w, "trleft")) { r->rowdef.left = v; return; }
    if (!strcmp(w, "cellx")) { if (r->rowdef.ncells < MAX_CELLS) r->rowdef.cellx[r->rowdef.ncells++] = v - r->rowdef.left; return; }
    if (!strcmp(w, "line")) { emit(r, L"\v", 1); return; }
    if (!strcmp(w, "tab")) { emit(r, L"\t", 1); return; }
    if (!strcmp(w, "emdash")) { emit(r, L"\x2014", 1); return; }
    if (!strcmp(w, "endash")) { emit(r, L"\x2013", 1); return; }
    if (!strcmp(w, "bullet")) { emit(r, L"\x2022", 1); return; }
    if (!strcmp(w, "lquote")) { emit(r, L"\x2018", 1); return; }
    if (!strcmp(w, "rquote")) { emit(r, L"\x2019", 1); return; }
    if (!strcmp(w, "ldblquote")) { emit(r, L"\x201C", 1); return; }
    if (!strcmp(w, "rdblquote")) { emit(r, L"\x201D", 1); return; }
    /* character properties */
    if (!strcmp(w, "plain")) { cp_default(cp); if (r->nfonts) { RFont *f = find_font(r, 0); if (f) lstrcpynW(cp->font, f->name, 64); } s->font = 0; return; }
    if (!strcmp(w, "b")) { cp->bold = v != 0; return; }
    if (!strcmp(w, "i")) { cp->italic = v != 0; return; }
    if (!strcmp(w, "ul") || !strcmp(w, "uld") || !strcmp(w, "uldb") || !strcmp(w, "ulw") || !strcmp(w, "uldash") ||
        !strcmp(w, "ulth") || !strcmp(w, "ulwave")) { cp->underline = v != 0; return; }
    if (!strcmp(w, "ulnone")) { cp->underline = 0; return; }
    if (!strcmp(w, "strike") || !strcmp(w, "striked")) { cp->strike = v != 0; return; }
    if (!strcmp(w, "super")) { cp->script = 1; return; }
    if (!strcmp(w, "sub")) { cp->script = -1; return; }
    if (!strcmp(w, "nosupersub")) { cp->script = 0; return; }
    if (!strcmp(w, "up")) { cp->script = v ? 1 : 0; return; }
    if (!strcmp(w, "dn")) { cp->script = v ? -1 : 0; return; }
    if (!strcmp(w, "fs")) { if (has && v > 0) cp->hps = v; return; }
    if (!strcmp(w, "f") && has)
    {
        RFont *f = find_font(r, v);
        s->font = v;
        if (f && f->name[0]) lstrcpynW(cp->font, f->name, 64);
        return;
    }
    if (!strcmp(w, "cf"))
    {
        if (v > 0 && v < r->ncolors) { cp->has_color = 1; cp->color = r->colors[v]; }
        else cp->has_color = 0;
        return;
    }
    if (!strcmp(w, "highlight") || !strcmp(w, "cb") || !strcmp(w, "chcbpat"))
    {
        if (v > 0 && v < r->ncolors) { cp->has_hl = 1; cp->hl = r->colors[v]; }
        else cp->has_hl = 0;
        return;
    }
    /* paragraph properties */
    if (!strcmp(w, "pard"))
    {
        memset(&r->cur, 0, sizeof(r->cur));
        r->cur.line = 240;
        r->intbl = 0;
        return;
    }
    if (!strcmp(w, "ql")) { r->cur.align = AL_LEFT; return; }
    if (!strcmp(w, "qc")) { r->cur.align = AL_CENTER; return; }
    if (!strcmp(w, "qr")) { r->cur.align = AL_RIGHT; return; }
    if (!strcmp(w, "qj")) { r->cur.align = AL_JUSTIFY; return; }
    if (!strcmp(w, "li")) { r->cur.left = v; return; }
    if (!strcmp(w, "ri")) { r->cur.right = v; return; }
    if (!strcmp(w, "fi")) { r->cur.first = v; return; }
    if (!strcmp(w, "sb")) { r->cur.before = v; return; }
    if (!strcmp(w, "sa")) { r->cur.after = v; return; }
    if (!strcmp(w, "sl")) { r->cur.line = v ? abs(v) : 240; return; }
    if (!strcmp(w, "slmult")) { if (!v && r->cur.line) r->cur.line = -r->cur.line; return; }   /* negative: exact twips */
    if (!strcmp(w, "tx")) { if (r->cur.ntabs < 32) r->cur.tabs[r->cur.ntabs++] = v; return; }
}

static void end_group(Rtf *r)
{
    RState *s = &r->st[r->depth];
    if (s->dest == RD_FONTTBL && r->fontname.n && s->fontent >= 0 && s->fontent < r->nfonts)
    {
        WCHAR *nm = (WCHAR *)r->fontname.p;
        int n = (int)(r->fontname.n / sizeof(WCHAR));
        while (n && (nm[n - 1] == ';' || nm[n - 1] == ' ')) n--;
        if (n > 63) n = 63;
        memcpy(r->fonts[s->fontent].name, nm, n * sizeof(WCHAR));
        r->fonts[s->fontent].name[n] = 0;
        r->fontname.n = 0;
    }
    if (s->dest == RD_PICT && (r->depth == 0 || r->st[r->depth - 1].dest != RD_PICT)) pict_done(r);
    if (r->depth > 0) r->depth--;
}

BOOL doc_from_rtf(Doc *d, const char *rtf, size_t len)
{
    Rtf *r = calloc(1, sizeof(Rtf));
    BOOL ok;
    if (!r) return FALSE;
    r->s = rtf; r->n = len; r->doc = d;
    r->codepage = 1252;
    r->cur.line = 240;
    r->row_first = -1;
    cp_default(&r->st[0].cp);
    r->st[0].uc = 1;
    r->st[0].fontent = -1;
    while (r->i < r->n)
    {
        char c = r->s[r->i++];
        RState *s = &r->st[r->depth];
        if (c == '{')
        {
            if (r->depth < (int)ARRAYSIZE(r->st) - 1) { r->st[r->depth + 1] = *s; r->depth++; r->st[r->depth].fontent = s->dest == RD_FONTTBL ? -1 : s->fontent; }
            else r->depth++;            /* too deep: counted, contents skipped */
            if (s->dest == RD_FONTTBL) r->fontname.n = 0;
            continue;
        }
        if (c == '}') { end_group(r); continue; }
        if (c == '\\')
        {
            char w[32];
            int k = 0, neg = 0, has = 0, v = 0;
            BOOL star = FALSE;
            if (r->i >= r->n) break;
            c = r->s[r->i];
            if (!isalpha((unsigned char)c))
            {
                r->i++;
                if (c == '\'')
                {
                    int h1 = r->i < r->n ? hexval(r->s[r->i]) : -1, h2 = r->i + 1 < r->n ? hexval(r->s[r->i + 1]) : -1;
                    if (h1 >= 0 && h2 >= 0)
                    {
                        r->i += 2;
                        if (r->skip) { r->skip--; continue; }
                        if (s->dest == RD_FONTTBL) { WCHAR ch = (WCHAR)(h1 * 16 + h2); buf_add(&r->fontname, &ch, sizeof(ch)); }
                        else emit_byte(r, (BYTE)(h1 * 16 + h2));
                    }
                }
                else if (c == '*')
                {
                    /* \* ignorable destination: look at the word after */
                    while (r->i < r->n && (r->s[r->i] == ' ' || r->s[r->i] == '\r' || r->s[r->i] == '\n')) r->i++;
                    if (r->i < r->n && r->s[r->i] == '\\')
                    {
                        r->i++;
                        while (r->i < r->n && isalpha((unsigned char)r->s[r->i]) && k < 31) w[k++] = r->s[r->i++];
                        w[k] = 0;
                        if (r->i < r->n && (r->s[r->i] == '-' || isdigit((unsigned char)r->s[r->i])))
                        {
                            if (r->s[r->i] == '-') { neg = 1; r->i++; }
                            while (r->i < r->n && isdigit((unsigned char)r->s[r->i])) { v = v * 10 + (r->s[r->i++] - '0'); has = 1; }
                        }
                        if (r->i < r->n && r->s[r->i] == ' ') r->i++;
                        star = TRUE;
                        control(r, w, has, neg ? -v : v, star && strcmp(w, "shppict") && strcmp(w, "pn") && strcmp(w, "fldinst"));
                    }
                }
                else if (c == '\\' || c == '{' || c == '}') { WCHAR ch = c; if (r->skip) r->skip--; else emit(r, &ch, 1); }
                else if (c == '~') emit(r, L"\x00A0", 1);
                else if (c == '_') emit(r, L"\x2011", 1);
                else if (c == '-') ;
                else if (c == '\r' || c == '\n') { if (s->dest == RD_TEXT) end_para(r); }
                continue;
            }
            while (r->i < r->n && isalpha((unsigned char)r->s[r->i]) && k < 31) w[k++] = r->s[r->i++];
            while (r->i < r->n && isalpha((unsigned char)r->s[r->i])) r->i++;
            w[k] = 0;
            if (r->i < r->n && (r->s[r->i] == '-' || isdigit((unsigned char)r->s[r->i])))
            {
                if (r->s[r->i] == '-') { neg = 1; r->i++; }
                while (r->i < r->n && isdigit((unsigned char)r->s[r->i])) { v = v * 10 + (r->s[r->i++] - '0'); has = 1; }
            }
            if (r->i < r->n && r->s[r->i] == ' ') r->i++;
            if (r->depth < (int)ARRAYSIZE(r->st)) control(r, w, has, neg ? -v : v, FALSE);
            continue;
        }
        if (c == '\r' || c == '\n') continue;
        if (r->depth >= (int)ARRAYSIZE(r->st)) continue;
        if (s->dest == RD_PICT)
        {
            int h = hexval(c);
            if (h < 0) continue;
            if (!r->pict_half) { r->pict_hi = h; r->pict_half = 1; }
            else { BYTE b = (BYTE)(r->pict_hi * 16 + h); buf_add(&r->pict, &b, 1); r->pict_half = 0; }
            continue;
        }
        if (s->dest == RD_COLORTBL)
        {
            if (c == ';')
            {
                if (r->ncolors < 256) r->colors[r->ncolors++] = RGB(r->red, r->green, r->blue);
                r->red = r->green = r->blue = 0;
            }
            continue;
        }
        if (s->dest == RD_FONTTBL)
        {
            WCHAR ch = (BYTE)c;
            buf_add(&r->fontname, &ch, sizeof(ch));
            if (c == ';' && r->nfonts)
            {
                WCHAR *nm = (WCHAR *)r->fontname.p;
                int n = (int)(r->fontname.n / sizeof(WCHAR)) - 1;
                while (n > 0 && nm[n - 1] == ' ') n--;
                if (n > 63) n = 63;
                memcpy(r->fonts[r->nfonts - 1].name, nm, n * sizeof(WCHAR));
                r->fonts[r->nfonts - 1].name[n] = 0;
                r->fontname.n = 0;
            }
            continue;
        }
        if (r->skip) { r->skip--; continue; }
        if (s->dest == RD_TEXT)
        {
            /* a run of plain bytes at once */
            size_t st = r->i - 1;
            while (r->i < r->n && r->s[r->i] != '\\' && r->s[r->i] != '{' && r->s[r->i] != '}' &&
                   r->s[r->i] != '\r' && r->s[r->i] != '\n') r->i++;
            {
                RFont *f = find_font(r, s->font);
                UINT cp = r->codepage;
                int n;
                WCHAR *w;
                if (f && f->charset && f->charset != 1) cp = charset_cp(f->charset);
                if (cp == CP_SYMBOL) cp = 1252;
                n = MultiByteToWideChar(cp, 0, r->s + st, (int)(r->i - st), NULL, 0);
                w = malloc((n + 1) * sizeof(WCHAR));
                MultiByteToWideChar(cp, 0, r->s + st, (int)(r->i - st), w, n);
                emit(r, w, n);
                free(w);
            }
        }
    }
    if (r->row_first >= 0) end_row(r);
    /* a last paragraph with nothing in it is the end of the text */
    if (d->n && !d->p[d->n - 1].nruns && d->n > 1)
    {
        free(d->p[d->n - 1].runs);
        d->n--;
    }
    ok = TRUE;
    free(r->fonts);
    free(r->fontname.p);
    free(r->pict.p);
    free(r);
    return ok;
}

/* ---------------------------------------------------------------- model -> RTF */

static int font_index(WCHAR (*fonts)[64], int *nf, const WCHAR *name)
{
    for (int i = 0; i < *nf; i++) if (!lstrcmpiW(fonts[i], name)) return i;
    if (*nf >= 255) return 0;
    lstrcpynW(fonts[*nf], name, 64);
    return (*nf)++;
}

static int color_index(COLORREF *cols, int *nc, COLORREF c)
{
    for (int i = 1; i < *nc; i++) if (cols[i] == c) return i;
    if (*nc >= 255) return 0;
    cols[*nc] = c;
    return (*nc)++;
}

static void rtf_text(Buf *b, const WCHAR *s, int n)
{
    for (int i = 0; i < n; i++)
    {
        WCHAR c = s[i];
        if (c == '\\' || c == '{' || c == '}') { char e[3] = { '\\', (char)c, 0 }; buf_str(b, e); }
        else if (c == '\t') buf_str(b, "\\tab ");
        else if (c == '\v' || c == 0x2028) buf_str(b, "\\line ");
        else if (c == '\r' || c == '\n') continue;
        else if (c >= 0x20 && c < 0x80) { char e = (char)c; buf_add(b, &e, 1); }
        else buf_printf(b, "\\u%d?", (int)(short)c);
    }
}

static void rtf_hex(Buf *b, const BYTE *p, DWORD n)
{
    static const char hex[] = "0123456789abcdef";
    for (DWORD i = 0; i < n; i++)
    {
        char c[2] = { hex[p[i] >> 4], hex[p[i] & 15] };
        buf_add(b, c, 2);
        if (i % 64 == 63) buf_add(b, "\n", 1);
    }
}

char *doc_to_rtf(const Doc *d, size_t *len)
{
    WCHAR (*fonts)[64] = calloc(256, sizeof(*fonts));
    COLORREF cols[256];
    int nf = 0, nc = 1;
    Buf body = { 0 }, out = { 0 };
    static const char *pnkind[] = { "", "\\pnlvlblt", "\\pnlvlbody\\pndec", "\\pnlvlbody\\pnlcltr", "\\pnlvlbody\\pnucltr",
                                    "\\pnlvlbody\\pnlcrm", "\\pnlvlbody\\pnucrm" };
    int sym;
    if (!fonts) return NULL;
    font_index(fonts, &nf, L"Calibri");
    sym = font_index(fonts, &nf, L"Symbol");
    for (int i = 0; i < d->n; i++)
    {
        const Para *p = &d->p[i];
        BOOL intbl = p->row > 0 && p->row <= d->nrows;
        if (intbl && (i == 0 || d->p[i - 1].row != p->row))
        {
            /* a row's definition: its cells' right edges, single borders */
            const Row *row = &d->rows[p->row - 1];
            buf_str(&body, "\\trowd\\trgaph108");
            if (row->left) buf_printf(&body, "\\trleft%d", row->left);
            for (int c = 0; c < row->ncells; c++)
                buf_printf(&body, "\\clbrdrt\\brdrs\\brdrw10\\clbrdrl\\brdrs\\brdrw10\\clbrdrb\\brdrs\\brdrw10"
                                  "\\clbrdrr\\brdrs\\brdrw10\\cellx%d", row->left + row->cellx[c]);
            buf_str(&body, "\n");
        }
        buf_str(&body, intbl ? "\\pard\\intbl" : "\\pard");
        if (p->align == AL_CENTER) buf_str(&body, "\\qc");
        else if (p->align == AL_RIGHT) buf_str(&body, "\\qr");
        else if (p->align == AL_JUSTIFY) buf_str(&body, "\\qj");
        if (p->left) buf_printf(&body, "\\li%d", p->left);
        if (p->right) buf_printf(&body, "\\ri%d", p->right);
        if (p->first) buf_printf(&body, "\\fi%d", p->first);
        if (p->before) buf_printf(&body, "\\sb%d", p->before);
        if (p->after) buf_printf(&body, "\\sa%d", p->after);
        if (p->line > 0 && p->line != 240) buf_printf(&body, "\\sl%d\\slmult1", p->line);
        else if (p->line < 0) buf_printf(&body, "\\sl%d\\slmult0", p->line);
        for (int t = 0; t < p->ntabs; t++) buf_printf(&body, "\\tx%d", p->tabs[t]);
        if (p->list > 0 && p->list <= LS_UROMAN)
        {
            if (p->list == LS_BULLET)
                buf_printf(&body, "{\\*\\pn%s\\pnf%d\\pnindent360{\\pntxtb\\'b7}}", pnkind[p->list], sym);
            else
                buf_printf(&body, "{\\*\\pn%s\\pnindent360\\pnstart1{\\pntxta.}}", pnkind[p->list]);
            if (!p->first && !p->left) buf_str(&body, "\\fi-360\\li720");
        }
        buf_str(&body, " ");
        for (int j = 0; j < p->nruns; j++)
        {
            const Run *r = &p->runs[j];
            const CharProps *cp = &r->cp;
            if (r->pic)
            {
                BYTE *dib = NULL;
                DWORD dsz = 0;
                int wpx = 0, hpx = 0;
                if (r->pic == PIC_EMF || r->pic == PIC_WMF)
                {
                    buf_printf(&body, "{\\pict%s\\picwgoal%d\\pichgoal%d\n", r->pic == PIC_EMF ? "\\emfblip" : "\\wmetafile8", r->w, r->h);
                    rtf_hex(&body, r->data, r->size);
                    buf_str(&body, "}");
                }
                else if (r->pic == PIC_DIB ? (dib = r->data, dsz = r->size, TRUE)
                                           : pic_decode_to_dib(r->data, r->size, &dib, &dsz, &wpx, &hpx))
                {
                    const BITMAPINFOHEADER *bh = (const BITMAPINFOHEADER *)dib;
                    int w = r->w ? r->w : bh->biWidth * 15, h = r->h ? r->h : abs(bh->biHeight) * 15;
                    buf_printf(&body, "{\\pict\\dibitmap0\\picw%ld\\pich%ld\\picwgoal%d\\pichgoal%d\n", bh->biWidth, labs(bh->biHeight), w, h);
                    rtf_hex(&body, dib, dsz);
                    buf_str(&body, "}");
                    if (dib != r->data) free(dib);
                }
                continue;
            }
            buf_printf(&body, "{\\f%d\\fs%d", font_index(fonts, &nf, cp->font[0] ? cp->font : L"Calibri"), cp->hps ? cp->hps : 22);
            if (cp->bold) buf_str(&body, "\\b");
            if (cp->italic) buf_str(&body, "\\i");
            if (cp->underline) buf_str(&body, "\\ul");
            if (cp->strike) buf_str(&body, "\\strike");
            if (cp->script > 0) buf_str(&body, "\\super");
            if (cp->script < 0) buf_str(&body, "\\sub");
            if (cp->has_color) buf_printf(&body, "\\cf%d", color_index(cols, &nc, cp->color));
            if (cp->has_hl) buf_printf(&body, "\\highlight%d", color_index(cols, &nc, cp->hl));
            buf_str(&body, " ");
            rtf_text(&body, r->text, r->len);
            buf_str(&body, "}");
        }
        if (intbl)
        {
            BOOL last = i == d->n - 1 || d->p[i + 1].row != p->row;
            if (p->cell_end || last)
            {
                /* the row's missing cells, then the row's end */
                buf_str(&body, "\\cell ");
                if (last)
                {
                    const Row *row = &d->rows[p->row - 1];
                    for (int c = p->cell + 1; c < row->ncells; c++) buf_str(&body, "\\pard\\intbl\\cell ");
                    buf_str(&body, "\\row\n");
                }
            }
            else buf_str(&body, "\\par\n");
        }
        else if (i < d->n - 1) buf_str(&body, "\\par\n");
    }
    buf_str(&out, "{\\rtf1\\ansi\\ansicpg1252\\deff0\\uc1{\\fonttbl");
    for (int i = 0; i < nf; i++)
    {
        char name[200];
        WideCharToMultiByte(CP_ACP, 0, fonts[i], -1, name, sizeof(name), NULL, NULL);
        buf_printf(&out, "{\\f%d\\fnil%s %s;}", i, i == sym ? "\\fcharset2" : "\\fcharset0", name);
    }
    buf_str(&out, "}{\\colortbl;");
    for (int i = 1; i < nc; i++) buf_printf(&out, "\\red%d\\green%d\\blue%d;", GetRValue(cols[i]), GetGValue(cols[i]), GetBValue(cols[i]));
    buf_str(&out, "}\n");
    buf_add(&out, body.p ? body.p : "", body.n);
    buf_str(&out, "}");
    free(body.p);
    free(fonts);
    *len = out.n;
    return out.p;
}
