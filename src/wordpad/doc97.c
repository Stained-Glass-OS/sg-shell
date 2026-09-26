/* sg-wordpad -- Word 97-2003 documents (.doc): our own reader, written from
 * Microsoft's published Open Specifications [MS-CFB] (the compound file) and
 * [MS-DOC] (the Word binary format). No converter, nothing bundled.
 *
 * What it reads: the main document's text through the piece table (CLX),
 * compressed (8-bit) or not; character formatting from the CHPX FKPs (bold,
 * italic, underline, strike, super/subscript, size, font, colour, highlight);
 * paragraph formatting from the PAPX FKPs (alignment, indents, spacing, line
 * spacing, lists as bullets) and the paragraph styles' own formatting; tables
 * (sprmTDefTable's cell edges, cell marks, row ends); fields show their
 * result; tabs, line and page breaks. Not read: pictures, headers, footnotes,
 * comments, text boxes. Encrypted documents and Word 6/95 files are refused
 * with Windows' words.
 *
 * The file is hostile input: every offset and count is checked against what
 * is really there, and chains are bounded.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "wordpad.h"

/* ---------------------------------------------------------------- the compound file */

typedef struct {
    const BYTE *p; size_t n;
    DWORD ssz, mssz, cutoff;
    DWORD *fat; DWORD nfat;
    DWORD *mfat; DWORD nmfat;
    BYTE *ministream; size_t nministream;
    BYTE *dir; size_t ndir;
} Cfb;

#define ENDCHAIN 0xFFFFFFFE
static WORD rd16(const BYTE *p) { return (WORD)(p[0] | p[1] << 8); }
static DWORD rd32(const BYTE *p) { return p[0] | p[1] << 8 | p[2] << 16 | (DWORD)p[3] << 24; }

static const BYTE *sector(Cfb *c, DWORD s)
{
    size_t off = ((size_t)s + 1) * c->ssz;
    if (s >= 0xFFFFFFFA || off + c->ssz > c->n) return NULL;
    return c->p + off;
}

/* a chain of sectors (or mini sectors) into a new buffer */
static BYTE *read_chain(Cfb *c, DWORD start, size_t size, BOOL mini, size_t *got)
{
    DWORD unit = mini ? c->mssz : c->ssz, s = start, count = 0, limit = (DWORD)(c->n / c->mssz + 16);
    BYTE *out = malloc(size ? size : 1);
    size_t have = 0;
    if (!out) return NULL;
    while (have < size && s < 0xFFFFFFFA && count++ < limit)
    {
        const BYTE *src;
        size_t take = min((size_t)unit, size - have);
        if (mini)
        {
            if ((size_t)s * unit + take > c->nministream) break;
            src = c->ministream + (size_t)s * unit;
            memcpy(out + have, src, take);
            s = s < c->nmfat ? c->mfat[s] : ENDCHAIN;
        }
        else
        {
            if (!(src = sector(c, s))) break;
            memcpy(out + have, src, take);
            s = s < c->nfat ? c->fat[s] : ENDCHAIN;
        }
        have += take;
    }
    *got = have;
    return out;
}

static BOOL cfb_open(Cfb *c, const BYTE *p, size_t n)
{
    static const BYTE sig[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 };
    DWORD nfatsec, difat, ndifat, dirstart, mfatstart, nmfatsec, i, k = 0;
    size_t got;
    memset(c, 0, sizeof(*c));
    if (n < 512 || memcmp(p, sig, 8)) return FALSE;
    c->p = p; c->n = n;
    c->ssz = 1u << rd16(p + 0x1E);
    c->mssz = 1u << rd16(p + 0x20);
    if (c->ssz != 512 && c->ssz != 4096) return FALSE;
    if (c->mssz != 64) return FALSE;
    nfatsec = rd32(p + 0x2C); dirstart = rd32(p + 0x30); c->cutoff = rd32(p + 0x38);
    mfatstart = rd32(p + 0x3C); nmfatsec = rd32(p + 0x40); difat = rd32(p + 0x44); ndifat = rd32(p + 0x48);
    if (nfatsec > n / c->ssz + 1) return FALSE;
    c->nfat = nfatsec * (c->ssz / 4);
    if (!(c->fat = calloc(c->nfat ? c->nfat : 1, 4))) return FALSE;
    /* the FAT's sectors: 109 in the header, then the DIFAT chain */
    for (i = 0; i < nfatsec && i < 109; i++)
    {
        const BYTE *s = sector(c, rd32(p + 0x4C + i * 4));
        if (!s) return FALSE;
        for (DWORD j = 0; j < c->ssz / 4; j++) c->fat[k++] = rd32(s + j * 4);
    }
    while (i < nfatsec && ndifat-- && difat < 0xFFFFFFFA)
    {
        const BYTE *d = sector(c, difat);
        if (!d) return FALSE;
        for (DWORD j = 0; j < c->ssz / 4 - 1 && i < nfatsec; j++, i++)
        {
            const BYTE *s = sector(c, rd32(d + j * 4));
            if (!s) return FALSE;
            for (DWORD m = 0; m < c->ssz / 4; m++) c->fat[k++] = rd32(s + m * 4);
        }
        difat = rd32(d + c->ssz - 4);
    }
    /* the directory */
    c->dir = read_chain(c, dirstart, (size_t)c->ssz * 4096, FALSE, &got);
    c->ndir = got;
    if (!c->dir || c->ndir < 128) return FALSE;
    /* the mini FAT and the mini stream (the root entry's) */
    if (nmfatsec && mfatstart < 0xFFFFFFFA)
    {
        BYTE *m = read_chain(c, mfatstart, (size_t)nmfatsec * c->ssz, FALSE, &got);
        if (m)
        {
            c->nmfat = (DWORD)(got / 4);
            c->mfat = malloc(got + 4);
            for (DWORD j = 0; j < c->nmfat; j++) c->mfat[j] = rd32(m + j * 4);
            free(m);
        }
        c->ministream = read_chain(c, rd32(c->dir + 0x74), rd32(c->dir + 0x78), FALSE, &c->nministream);
    }
    return TRUE;
}

static void cfb_close(Cfb *c)
{
    free(c->fat); free(c->mfat); free(c->ministream); free(c->dir);
}

/* a stream by name (anywhere in the directory: Word's streams are the root's) */
static BYTE *cfb_stream(Cfb *c, const WCHAR *name, size_t *len)
{
    for (size_t off = 0; off + 128 <= c->ndir; off += 128)
    {
        const BYTE *e = c->dir + off;
        WORD nl = rd16(e + 0x40);
        WCHAR nm[33];
        DWORD size;
        if (e[0x42] != 2 || nl < 2 || nl > 64) continue;
        memcpy(nm, e, nl);
        nm[nl / 2 - 1] = 0;
        if (lstrcmpiW(nm, name)) continue;
        size = rd32(e + 0x78);
        if (size > c->n + (size_t)c->nministream) return NULL;
        return read_chain(c, rd32(e + 0x74), size, size < c->cutoff, len);
    }
    return NULL;
}

/* ---------------------------------------------------------------- the Word document */

typedef struct { DWORD fc, fcend; CharProps cp; } ChpRun;
typedef struct { DWORD fc, fcend; Para pp; int intbl, ttp, istd; BYTE tdef[1 + 2 * (MAX_CELLS + 1)]; int ntdef; } PapRun;

typedef struct {
    const BYTE *wd; size_t nwd;         /* WordDocument */
    const BYTE *tb; size_t ntb;         /* 0Table or 1Table */
    WCHAR (*fonts)[64]; int nfonts;
    ChpRun *chp; int nchp;
    PapRun *pap; int npap;
    /* styles: istd -> paragraph and character properties */
    Para *spara; CharProps *scp; int nstyles;
} Word;

static const COLORREF ICO[17] = {
    0, RGB(0, 0, 0), RGB(0, 0, 255), RGB(0, 255, 255), RGB(0, 255, 0), RGB(255, 0, 255), RGB(255, 0, 0),
    RGB(255, 255, 0), RGB(255, 255, 255), RGB(0, 0, 128), RGB(0, 128, 128), RGB(0, 128, 0), RGB(128, 0, 128),
    RGB(128, 0, 0), RGB(128, 128, 0), RGB(128, 128, 128), RGB(192, 192, 192) };

/* an operand's size from the sprm's spra; variable ones from their first byte */
static int sprm_size(WORD sprm, const BYTE *op, size_t left)
{
    switch (sprm >> 13)
    {
    case 0: case 1: return 1;
    case 2: case 4: case 5: return 2;
    case 3: return 4;
    case 7: return 3;
    case 6:
        if (sprm == 0xD608 || sprm == 0xD606)   /* sprmTDefTable(10): a 2-byte size */
            return left >= 2 ? rd16(op) + 1 : 1;
        if (sprm == 0xC615)                     /* sprmPChgTabs: special */
            return left >= 1 ? (op[0] == 255 && left >= 2 ? 1 + op[1] * 4 + 2 : op[0] + 1) : 1;
        return left >= 1 ? op[0] + 1 : 1;
    }
    return 1;
}

static BOOL flag(BYTE v, BOOL cur) { return v == 0 ? FALSE : v == 1 ? TRUE : v == 129 ? !cur : cur; }

static void apply_chp(Word *w, const BYTE *g, size_t n, CharProps *cp)
{
    size_t i = 0;
    while (i + 2 <= n)
    {
        WORD sprm = rd16(g + i);
        const BYTE *op = g + i + 2;
        int sz = sprm_size(sprm, op, n - i - 2);
        if (i + 2 + sz > n) break;
        switch (sprm)
        {
        case 0x0835: cp->bold = flag(op[0], cp->bold); break;
        case 0x0836: cp->italic = flag(op[0], cp->italic); break;
        case 0x0837: cp->strike = flag(op[0], cp->strike); break;
        case 0x2A53: cp->strike = flag(op[0], cp->strike); break;       /* double strike */
        case 0x2A3E: cp->underline = op[0] != 0; break;
        case 0x4A43: if (rd16(op) >= 2) cp->hps = rd16(op); break;
        case 0x2A48: cp->script = op[0] == 1 ? 1 : op[0] == 2 ? -1 : 0; break;
        case 0x2A42: if (op[0] > 0 && op[0] < 17) { cp->has_color = 1; cp->color = ICO[op[0]]; } else cp->has_color = 0; break;
        case 0x6870: if (op[3] == 0xFF) cp->has_color = 0; else { cp->has_color = 1; cp->color = RGB(op[0], op[1], op[2]); } break;
        case 0x2A0C: if (op[0] > 0 && op[0] < 17) { cp->has_hl = 1; cp->hl = ICO[op[0]]; } else cp->has_hl = 0; break;
        case 0x4A4F: case 0x4A5E:
            if (rd16(op) < w->nfonts && w->fonts[rd16(op)][0]) lstrcpynW(cp->font, w->fonts[rd16(op)], 64);
            break;
        }
        i += 2 + sz;
    }
}

static void apply_pap(const BYTE *g, size_t n, Para *pp, int *intbl, int *ttp, BYTE *tdef, int *ntdef)
{
    size_t i = 0;
    while (i + 2 <= n)
    {
        WORD sprm = rd16(g + i);
        const BYTE *op = g + i + 2;
        int sz = sprm_size(sprm, op, n - i - 2);
        if (i + 2 + sz > n) break;
        switch (sprm)
        {
        case 0x2403: case 0x2461: pp->align = op[0] == 1 ? AL_CENTER : op[0] == 2 ? AL_RIGHT : op[0] == 3 || op[0] == 4 ? AL_JUSTIFY : AL_LEFT; break;
        case 0x840F: case 0x845E: pp->left = (short)rd16(op); break;
        case 0x840E: case 0x845D: pp->right = (short)rd16(op); break;
        case 0x8411: case 0x8460: pp->first = (short)rd16(op); break;
        case 0xA413: pp->before = rd16(op); break;
        case 0xA414: pp->after = rd16(op); break;
        case 0x6412:
        {
            short dya = (short)rd16(op), mult = (short)rd16(op + 2);
            pp->line = mult ? (dya > 0 ? dya : 240) : -abs(dya);
            break;
        }
        case 0x460B: pp->list = rd16(op) ? LS_BULLET : LS_NONE; break;
        case 0x2416: if (intbl) *intbl = op[0]; break;
        case 0x2417: if (ttp) *ttp = op[0]; break;
        case 0xD608:
            if (tdef && sz >= 3)
            {
                int itc = op[2];
                if (itc > MAX_CELLS) itc = MAX_CELLS;
                if (3 + 2 * (itc + 1) <= sz)
                {
                    tdef[0] = (BYTE)itc;
                    memcpy(tdef + 1, op + 3, 2 * (itc + 1));
                    *ntdef = itc;
                }
            }
            break;
        }
        i += 2 + sz;
    }
}

/* the style sheet: each paragraph style's paragraph and character formatting,
 * following its base style ([MS-DOC] 2.9.271 STSH, 2.9.260 STD) */
static void read_styles(Word *w, DWORD fc, DWORD lcb, const CharProps *base)
{
    const BYTE *p = w->tb + fc, *end;
    WORD cbStshi, cstd, cbSTDBaseInFile;
    size_t off;
    int *based;
    if ((size_t)fc + lcb > w->ntb || lcb < 8) return;
    end = p + lcb;
    cbStshi = rd16(p);
    if (2u + cbStshi + 2u > lcb) return;
    cstd = rd16(p + 2);
    cbSTDBaseInFile = rd16(p + 4);
    if (cstd > 4096) return;
    w->nstyles = cstd;
    w->spara = calloc(cstd + 1, sizeof(Para));
    w->scp = calloc(cstd + 1, sizeof(CharProps));
    based = calloc(cstd + 1, sizeof(int));
    for (int i = 0; i < cstd; i++) { w->scp[i] = *base; w->spara[i].line = 240; based[i] = -1; }
    off = 2 + cbStshi;
    /* first pass: the base style of each; the formatting is applied in order,
     * which Word keeps (a base comes before what is based on it) */
    for (int i = 0; i < cstd && p + off + 2 <= end; i++)
    {
        WORD cbStd = rd16(p + off);
        const BYTE *std = p + off + 2, *upx;
        size_t pos;
        int stk, cupx, istdBase;
        off += 2 + cbStd;
        if (!cbStd || std + cbStd > end || cbStd < 10) continue;
        stk = rd16(std + 2) & 0xF;
        istdBase = rd16(std + 2) >> 4;
        cupx = rd16(std + 4) & 0xF;
        if (istdBase < cstd && istdBase != i && istdBase != 0xFFF)
        {
            w->spara[i] = w->spara[istdBase];
            w->scp[i] = w->scp[istdBase];
        }
        /* the name (Xstz: a count and UTF-16), then the UPXs, each padded to even */
        pos = cbSTDBaseInFile;
        if (pos + 2 > cbStd) continue;
        pos += 2 + rd16(std + pos) * 2 + 2;
        for (int u = 0; u < cupx && pos + 2 <= cbStd; u++)
        {
            WORD cbUpx = rd16(std + pos);
            upx = std + pos + 2;
            if (pos + 2 + cbUpx > cbStd) break;
            if (stk == 1 && u == 0 && cbUpx >= 2)       /* paragraph style: UpxPapx (istd, then sprms) */
                apply_pap(upx + 2, cbUpx - 2, &w->spara[i], NULL, NULL, NULL, NULL);
            else if ((stk == 1 && u == 1) || (stk == 2 && u == 0))
                apply_chp(w, upx, cbUpx, &w->scp[i]);
            pos += 2 + cbUpx + (cbUpx & 1);
        }
    }
    free(based);
}

static void read_fonts(Word *w, DWORD fc, DWORD lcb)
{
    const BYTE *p = w->tb + fc;
    size_t off = 4;
    int n;
    if ((size_t)fc + lcb > w->ntb || lcb < 4) return;
    n = rd16(p);
    if (n == 0xFFFF) return;                           /* extended strings: not for fonts */
    if (n > 1024) return;
    w->fonts = calloc(n + 1, sizeof(*w->fonts));
    for (int i = 0; i < n && off < lcb; i++)
    {
        int cb = p[off];
        /* FFN: flags, wWeight, chs, ixchSzAlt, PANOSE (10), FONTSIGNATURE (24), then the name */
        if (off + 1 + cb > lcb) break;
        if (cb > 39)
        {
            const BYTE *nm = p + off + 1 + 39;
            int k = 0;
            while (k < 63 && nm + k * 2 + 1 < p + off + 1 + cb && rd16(nm + k * 2)) { w->fonts[i][k] = rd16(nm + k * 2); k++; }
            w->fonts[i][k] = 0;
        }
        off += 1 + cb;
        w->nfonts = i + 1;
    }
}

/* the CHPX and PAPX FKPs through their bin tables ([MS-DOC] 2.9.33, 2.9.38) */
static void read_fkps(Word *w, DWORD fc, DWORD lcb, BOOL chp, const CharProps *base)
{
    const BYTE *plc = w->tb + fc;
    int n;
    if ((size_t)fc + lcb > w->ntb || lcb < 8) return;
    n = (int)((lcb - 4) / 8);
    for (int i = 0; i < n && i < 100000; i++)
    {
        DWORD pn = rd32(plc + (n + 1) * 4 + i * 4) & 0x3FFFFF;
        const BYTE *fkp;
        int crun;
        if (((size_t)pn + 1) * 512 > w->nwd) continue;
        fkp = w->wd + (size_t)pn * 512;
        crun = fkp[511];
        if (chp)
        {
            if ((crun + 1) * 4 + crun > 511) continue;
            for (int r = 0; r < crun; r++)
            {
                ChpRun run;
                int bo = fkp[(crun + 1) * 4 + r] * 2;
                run.fc = rd32(fkp + r * 4); run.fcend = rd32(fkp + (r + 1) * 4);
                run.cp = *base;
                if (bo && bo < 511 && bo + 1 + fkp[bo] <= 511) apply_chp(w, fkp + bo + 1, fkp[bo], &run.cp);
                w->chp = realloc(w->chp, (w->nchp + 1) * sizeof(ChpRun));
                w->chp[w->nchp++] = run;
            }
        }
        else
        {
            if ((crun + 1) * 4 + crun * 13 > 511) continue;
            for (int r = 0; r < crun; r++)
            {
                PapRun run;
                int bo = fkp[(crun + 1) * 4 + r * 13] * 2, cb, len;
                const BYTE *g;
                memset(&run, 0, sizeof(run));
                run.fc = rd32(fkp + r * 4); run.fcend = rd32(fkp + (r + 1) * 4);
                run.pp.line = 240;
                if (bo && bo < 510)
                {
                    cb = fkp[bo];
                    if (cb) { g = fkp + bo + 1; len = 2 * cb - 1; }
                    else { g = fkp + bo + 2; len = 2 * fkp[bo + 1]; }
                    if (g + len <= fkp + 511 && len >= 2)
                    {
                        run.istd = rd16(g);
                        if (run.istd < w->nstyles) run.pp = w->spara[run.istd];
                        apply_pap(g + 2, len - 2, &run.pp, &run.intbl, &run.ttp, run.tdef, &run.ntdef);
                    }
                }
                w->pap = realloc(w->pap, (w->npap + 1) * sizeof(PapRun));
                w->pap[w->npap++] = run;
            }
        }
    }
}

static const ChpRun *chp_at(Word *w, DWORD fc)
{
    int lo = 0, hi = w->nchp - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (fc < w->chp[mid].fc) hi = mid - 1;
        else if (fc >= w->chp[mid].fcend) lo = mid + 1;
        else return &w->chp[mid];
    }
    return NULL;
}

static const PapRun *pap_at(Word *w, DWORD fc)
{
    int lo = 0, hi = w->npap - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (fc < w->pap[mid].fc) hi = mid - 1;
        else if (fc >= w->pap[mid].fcend) lo = mid + 1;
        else return &w->pap[mid];
    }
    return NULL;
}

static int cmp_chp(const void *a, const void *b) { DWORD x = ((const ChpRun *)a)->fc, y = ((const ChpRun *)b)->fc; return x < y ? -1 : x > y; }
static int cmp_pap(const void *a, const void *b) { DWORD x = ((const PapRun *)a)->fc, y = ((const PapRun *)b)->fc; return x < y ? -1 : x > y; }

BOOL doc97_read(const WCHAR *path, Doc *d, WCHAR *err, int cch)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz;
    BYTE *file = NULL, *wd = NULL, *tb = NULL;
    size_t nwd = 0, ntb = 0;
    DWORD got, ccpText, fcClx, lcbClx, fcChp, lcbChp, fcPap, lcbPap, fcFfn, lcbFfn, fcStsh, lcbStsh;
    Cfb c;
    Word w;
    BOOL ok = FALSE;
    WORD flags, csw, cslw, cbRgFcLcb;
    const BYTE *fib, *lw, *fcl;
    CharProps base;
    int field = 0;                      /* nesting: 0 text, >0 in a field's code */
    int fstack[32], fdepth = 0;         /* per field: 1 in code, 0 in result */
    Para *cur = NULL;
    int cell = 0, row_first = -1;

    memset(&w, 0, sizeof(w));
    memset(&c, 0, sizeof(c));
    if (h == INVALID_HANDLE_VALUE) { lstrcpynW(err, L"The file cannot be opened.", cch); return FALSE; }
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 512 * 1024 * 1024 || !(file = malloc((size_t)sz.QuadPart + 1)) ||
        !ReadFile(h, file, (DWORD)sz.QuadPart, &got, NULL) || got != sz.QuadPart)
    {
        CloseHandle(h); free(file);
        lstrcpynW(err, L"The file cannot be read.", cch);
        return FALSE;
    }
    CloseHandle(h);
    if (!cfb_open(&c, file, got) || !(wd = cfb_stream(&c, L"WordDocument", &nwd)) || nwd < 0x200)
    {
        lstrcpynW(err, L"It is not a Word document.", cch);
        goto done;
    }
    fib = wd;
    if (rd16(fib) != 0xA5EC) { lstrcpynW(err, L"It is not a Word document.", cch); goto done; }
    if (rd16(fib + 2) < 101) { lstrcpynW(err, L"Documents from Word 6.0 and Word 95 are not supported.", cch); goto done; }
    flags = rd16(fib + 0x0A);
    if (flags & 0x0100) { lstrcpynW(err, L"The document is password protected.", cch); goto done; }
    if (!(tb = cfb_stream(&c, (flags & 0x0200) ? L"1Table" : L"0Table", &ntb)))
    {
        lstrcpynW(err, L"The document's table stream is missing.", cch);
        goto done;
    }
    /* the FIB: FibBase (32), csw + fibRgW, cslw + fibRgLw, cbRgFcLcb + the fc/lcb pairs */
    csw = rd16(fib + 32);
    lw = fib + 34 + csw * 2;
    if (lw + 2 - wd > (ptrdiff_t)nwd) goto bad;
    cslw = rd16(lw);
    if (cslw < 11 || lw + 2 + cslw * 4 + 2 - wd > (ptrdiff_t)nwd) goto bad;
    ccpText = rd32(lw + 2 + 3 * 4);
    fcl = lw + 2 + cslw * 4;
    cbRgFcLcb = rd16(fcl);
    fcl += 2;
    if (cbRgFcLcb < 34 || fcl + cbRgFcLcb * 8 - wd > (ptrdiff_t)nwd) goto bad;
    fcStsh = rd32(fcl + 1 * 8); lcbStsh = rd32(fcl + 1 * 8 + 4);
    fcChp = rd32(fcl + 12 * 8); lcbChp = rd32(fcl + 12 * 8 + 4);
    fcPap = rd32(fcl + 13 * 8); lcbPap = rd32(fcl + 13 * 8 + 4);
    fcFfn = rd32(fcl + 15 * 8); lcbFfn = rd32(fcl + 15 * 8 + 4);
    fcClx = rd32(fcl + 33 * 8); lcbClx = rd32(fcl + 33 * 8 + 4);
    if ((size_t)fcClx + lcbClx > ntb) goto bad;

    w.wd = wd; w.nwd = nwd; w.tb = tb; w.ntb = ntb;
    read_fonts(&w, fcFfn, lcbFfn);
    cp_default(&base);
    lstrcpyW(base.font, w.nfonts && w.fonts[0][0] ? w.fonts[0] : L"Times New Roman");
    base.hps = 20;                                      /* Word's default: 10 pt */
    read_styles(&w, fcStsh, lcbStsh, &base);
    if (w.nstyles) base = w.scp[0];
    read_fkps(&w, fcChp, lcbChp, TRUE, &base);
    read_fkps(&w, fcPap, lcbPap, FALSE, &base);
    qsort(w.chp, w.nchp, sizeof(ChpRun), cmp_chp);
    qsort(w.pap, w.npap, sizeof(PapRun), cmp_pap);

    {
        /* the CLX: skip Prcs (0x01), then the Pcdt (0x02) and its piece table */
        const BYTE *x = tb + fcClx, *xe = x + lcbClx;
        DWORD lcb, npieces, cpdone = 0;
        while (x < xe && *x == 0x01) { if (x + 3 > xe) goto bad; x += 3 + rd16(x + 1); }
        if (x + 5 > xe || *x != 0x02) goto bad;
        lcb = rd32(x + 1);
        x += 5;
        if (x + lcb > xe || lcb < 16) goto bad;
        npieces = (lcb - 4) / 12;
        for (DWORD i = 0; i < npieces && cpdone < ccpText; i++)
        {
            DWORD cp0 = rd32(x + i * 4), cp1 = rd32(x + (i + 1) * 4);
            const BYTE *pcd = x + (npieces + 1) * 4 + i * 8;
            DWORD fcv = rd32(pcd + 2), fc;
            BOOL comp = (fcv & 0x40000000) != 0;
            fc = comp ? (fcv & 0x3FFFFFFF) / 2 : fcv;
            if (cp1 < cp0) goto bad;
            for (DWORD k = 0; k < cp1 - cp0 && cpdone < ccpText; k++, cpdone++)
            {
                DWORD at = fc + (comp ? k : k * 2);
                WCHAR ch;
                const ChpRun *cr;
                if (at + (comp ? 1 : 2) > nwd) goto bad;
                if (comp)
                {
                    BYTE b = wd[at];
                    if (!MultiByteToWideChar(1252, 0, (char *)&b, 1, &ch, 1)) ch = b;
                }
                else ch = rd16(wd + at);
                if (!cur) cur = doc_add_para(d);
                if (ch == 0x13) { if (fdepth < 32) fstack[fdepth++] = 1; field++; continue; }
                if (ch == 0x14) { if (fdepth) fstack[fdepth - 1] = 0; continue; }
                if (ch == 0x15) { if (fdepth) fdepth--; if (field) field--; continue; }
                if (fdepth && fstack[fdepth - 1]) continue;     /* a field's code */
                if (ch == 0x0D || ch == 0x07)
                {
                    /* the paragraph (or cell, or row) ends: its properties are those of this mark */
                    const PapRun *pr = pap_at(&w, at);
                    Run *runs = cur->runs;
                    int nruns = cur->nruns, cap = cur->cap;
                    if (pr) *cur = pr->pp; else { memset(cur, 0, sizeof(*cur)); cur->line = 240; }
                    cur->runs = runs; cur->nruns = nruns; cur->cap = cap;
                    if (pr && pr->intbl)
                    {
                        if (pr->ttp)
                        {
                            /* the row's end: a paragraph of its own in Word, not in ours */
                            Row row;
                            int num;
                            memset(&row, 0, sizeof(row));
                            if (pr->ntdef)
                            {
                                short x0 = (short)rd16(pr->tdef + 1);
                                row.ncells = pr->ntdef;
                                for (int k2 = 0; k2 < pr->ntdef; k2++) row.cellx[k2] = (short)rd16(pr->tdef + 1 + 2 * (k2 + 1)) - x0;
                            }
                            else
                            {
                                row.ncells = max(1, cell);
                                for (int k2 = 0; k2 < row.ncells; k2++) row.cellx[k2] = 9360 * (k2 + 1) / row.ncells;
                            }
                            num = doc_add_row(d, &row);
                            for (int k2 = row_first; k2 >= 0 && k2 < d->n; k2++) if (d->p[k2].row == -1) d->p[k2].row = num;
                            /* the row mark's paragraph is dropped: reuse it */
                            free(cur->runs);
                            memset(cur, 0, sizeof(*cur));
                            cur->line = 240;
                            d->n--;
                            cur = NULL;
                            row_first = -1;
                            cell = 0;
                            continue;
                        }
                        if (row_first < 0) row_first = d->n - 1;
                        cur->row = -1;
                        cur->cell = cell;
                        cur->cell_end = ch == 0x07;
                        if (ch == 0x07) cell++;
                    }
                    cur = NULL;
                    continue;
                }
                if (ch == 0x0B) ch = '\v';
                else if (ch == 0x0C) { cur = NULL; continue; }          /* page/section break */
                else if (ch == 0x1E) ch = 0x2011;
                else if (ch == 0x1F || ch == 0x01 || ch == 0x08 || ch == 0x02 || ch == 0x05 || ch < 0x09) continue;
                cr = chp_at(&w, at);
                para_add_text(cur, cr ? &cr->cp : &base, &ch, 1);
            }
        }
    }
    /* rows never closed are plain paragraphs */
    for (int i = 0; i < d->n; i++) if (d->p[i].row == -1) d->p[i].row = 0;
    if (d->n && !d->p[d->n - 1].nruns && d->n > 1) { free(d->p[d->n - 1].runs); d->n--; }
    if (!d->n) doc_add_para(d);
    ok = TRUE;
    goto done;
bad:
    lstrcpynW(err, L"The document is damaged or in a form WordPad does not read.", cch);
done:
    free(w.fonts); free(w.chp); free(w.pap); free(w.spara); free(w.scp);
    cfb_close(&c);
    free(wd); free(tb); free(file);
    return ok;
}
