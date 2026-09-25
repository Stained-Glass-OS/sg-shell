/* sg-fontview -- reading what a font file says about itself. See fontinfo.h.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "fontinfo.h"

#include <stdio.h>
#include <string.h>

static unsigned be16(const unsigned char *p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned long be32(const unsigned char *p)
{
    return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 | (unsigned long)p[2] << 8 | p[3];
}
static unsigned le16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static unsigned long le32(const unsigned char *p)
{
    return p[0] | (unsigned long)p[1] << 8 | (unsigned long)p[2] << 16 | (unsigned long)p[3] << 24;
}

/* does [off, off+n) lie inside the buffer? (no overflow) */
static int inside(size_t len, unsigned long off, unsigned long n)
{
    return off <= len && n <= len - off;
}

static void put_utf8(char *out, size_t cch, size_t *o, unsigned long c)
{
    char b[4];
    size_t n, i;
    if (c < 0x80) { b[0] = (char)c; n = 1; }
    else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) {
        b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); n = 3;
    } else {
        b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 0x3F));
        b[2] = (char)(0x80 | (c >> 6 & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); n = 4;
    }
    if (*o + n >= cch) return;
    for (i = 0; i < n; i++) out[(*o)++] = b[i];
}

/* UTF-16BE (Windows and Unicode platforms) to UTF-8, control characters dropped */
static void from_utf16be(const unsigned char *p, size_t n, char *out, size_t cch)
{
    size_t i, o = 0;
    for (i = 0; i + 1 < n; i += 2) {
        unsigned long c = be16(p + i);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
            unsigned long lo = be16(p + i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
        }
        if (c < 0x20 && c != '\n') c = ' ';
        if (c == '\n') c = ' ';
        put_utf8(out, cch, &o, c);
    }
    out[o] = 0;
}

/* Mac Roman / 8-bit names: ASCII as is, the rest as Latin-1 (close enough
 * for names, which are almost always ASCII) */
static void from_8bit(const unsigned char *p, size_t n, char *out, size_t cch)
{
    size_t i, o = 0;
    for (i = 0; i < n; i++) put_utf8(out, cch, &o, p[i] < 0x20 ? ' ' : p[i]);
    out[o] = 0;
}

/* how good a name record is: Windows English > Windows any > Unicode > Mac English */
static int name_rank(unsigned platform, unsigned encoding, unsigned language)
{
    if (platform == 3 && (encoding == 1 || encoding == 10 || encoding == 0))
        return language == 0x0409 ? 4 : 3;
    if (platform == 0) return 2;
    if (platform == 1 && encoding == 0 && language == 0) return 1;
    return 0;
}

static void read_names(const unsigned char *d, size_t len, unsigned long off, unsigned long size, struct fi_face *f)
{
    static const int ids[] = { 0, 1, 2, 4, 5, 8, 9, 10, 13, 16 };
    int best[sizeof(ids) / sizeof(ids[0])] = { 0 };
    unsigned count, storage, i;
    size_t k;

    if (!inside(len, off, size) || size < 6) return;
    count = be16(d + off + 2);
    storage = be16(d + off + 4);
    if (6 + (unsigned long)count * 12 > size) return;
    for (i = 0; i < count; i++) {
        const unsigned char *r = d + off + 6 + i * 12;
        unsigned platform = be16(r), encoding = be16(r + 2), language = be16(r + 4), id = be16(r + 6);
        unsigned n = be16(r + 8), so = be16(r + 10);
        unsigned long at = off + storage + so;
        int rank = name_rank(platform, encoding, language);
        char *dst = NULL;
        size_t cch = 0;
        if (!rank || !n || !inside(len, at, n) || (unsigned long)storage + so + n > size) continue;
        for (k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) if (ids[k] == (int)id) break;
        if (k == sizeof(ids) / sizeof(ids[0]) || rank <= best[k]) continue;
        switch (id) {
        case 0:  dst = f->copyright; cch = sizeof(f->copyright); break;
#ifndef SG_MUTANT_NAMEID
        case 1:  dst = f->family; cch = sizeof(f->family); break;
        case 4:  dst = f->full; cch = sizeof(f->full); break;
#else   /* the gate's mutant: the family and full names swapped */
        case 4:  dst = f->family; cch = sizeof(f->family); break;
        case 1:  dst = f->full; cch = sizeof(f->full); break;
#endif
        case 2:  dst = f->subfamily; cch = sizeof(f->subfamily); break;
        case 5:  dst = f->version; cch = sizeof(f->version); break;
        case 8:  dst = f->manufacturer; cch = sizeof(f->manufacturer); break;
        case 9:  dst = f->designer; cch = sizeof(f->designer); break;
        case 10: dst = f->description; cch = sizeof(f->description); break;
        case 13: dst = f->license; cch = sizeof(f->license); break;
        case 16: dst = f->typo_family; cch = sizeof(f->typo_family); break;
        }
        if (platform == 1) from_8bit(d + at, n, dst, cch);
        else from_utf16be(d + at, n, dst, cch);
        best[k] = rank;
    }
}

/* one sfnt (a .ttf/.otf, or one face of a collection) at offset off */
static int parse_sfnt(const unsigned char *d, size_t len, unsigned long off, struct fi_face *f)
{
    unsigned long tag, head_off = 0, os2_off = 0, os2_len = 0, name_off = 0, name_len = 0;
    unsigned n, i;

    memset(f, 0, sizeof(*f));
    if (!inside(len, off, 12)) return -1;
    tag = be32(d + off);
    if (tag != 0x00010000UL && tag != 0x4F54544FUL /* OTTO */ && tag != 0x74727565UL /* true */) return -1;
    n = be16(d + off + 4);
    if (!n || !inside(len, off + 12, (unsigned long)n * 16)) return -1;
    for (i = 0; i < n; i++) {
        const unsigned char *e = d + off + 12 + i * 16;
        unsigned long t = be32(e), to = be32(e + 8), tl = be32(e + 12);
        if (!inside(len, to, tl)) continue;
        switch (t) {
        case 0x676C7966UL: f->flags |= FI_TT_OUTLINES; break;          /* glyf */
        case 0x43464620UL: case 0x43464632UL: f->flags |= FI_PS_OUTLINES; break;   /* CFF, CFF2 */
        case 0x47535542UL: case 0x47504F53UL: f->flags |= FI_OT_LAYOUT; break;      /* GSUB, GPOS */
        case 0x44534947UL: f->flags |= FI_SIGNED; break;               /* DSIG */
        case 0x68656164UL: if (tl >= 54) head_off = to; break;        /* head */
        case 0x4F532F32UL: os2_off = to; os2_len = tl; break;          /* OS/2 */
        case 0x6E616D65UL: name_off = to; name_len = tl; break;        /* name */
        }
    }
    if (!name_off) return -1;
    read_names(d, len, name_off, name_len, f);
    if (!f->family[0]) return -1;
    if (!f->full[0]) snprintf(f->full, sizeof(f->full), "%s %s", f->family, f->subfamily);
    f->weight = 400;
    if (os2_off && os2_len >= 64) {
        unsigned w = be16(d + os2_off + 4), sel = be16(d + os2_off + 62);
        if (w >= 1 && w <= 1000) f->weight = (int)w;
        f->italic = sel & 1;
    } else if (head_off) {
        unsigned style = be16(d + head_off + 44);
        if (style & 1) f->weight = 700;
        f->italic = (style & 2) != 0;
    }
    return 0;
}

/* a .fon: an NE executable whose RT_FONT resources are FNT fonts */
static int parse_fon(const unsigned char *d, size_t len, struct fi_file *out)
{
    unsigned long ne, rt, shift;
    unsigned long p;
    struct fi_face *f = &out->faces[0];

    if (!inside(len, 0, 64) || d[0] != 'M' || d[1] != 'Z') return -1;
    ne = le32(d + 0x3C);
    if (!inside(len, ne, 64) || d[ne] != 'N' || d[ne + 1] != 'E') return -1;
    rt = ne + le16(d + ne + 0x24);
    if (!inside(len, rt, 2)) return -1;
    shift = le16(d + rt);
    if (shift > 16) return -1;
    memset(f, 0, sizeof(*f));
    for (p = rt + 2; inside(len, p, 2) && le16(d + p); ) {
        unsigned type, count, i;
        if (!inside(len, p, 8)) return -1;
        type = le16(d + p);
        count = le16(d + p + 2);
        if (!inside(len, p + 8, (unsigned long)count * 12)) return -1;
        for (i = 0; type == 0x8008 && i < count; i++) {        /* RT_FONT */
            const unsigned char *r = d + p + 8 + i * 12;
            unsigned long fo = (unsigned long)le16(r) << shift, fl = (unsigned long)le16(r + 2) << shift;
            unsigned long face;
            if (!inside(len, fo, fl) || fl < 118) continue;
            if (!f->family[0]) {
                face = le32(d + fo + 105);
                if (face < fl) {
                    size_t n = 0;
                    while (fo + face + n < len && face + n < fl && d[fo + face + n] && n < sizeof(f->family) - 1) n++;
                    from_8bit(d + fo + face, n, f->family, sizeof(f->family));
                }
                {
                    size_t n = 0;
                    while (n < 60 && d[fo + 6 + n]) n++;
                    from_8bit(d + fo + 6, n, f->copyright, sizeof(f->copyright));
                }
                f->weight = (int)le16(d + fo + 83);
                if (f->weight < 1 || f->weight > 1000) f->weight = 400;
                f->italic = d[fo + 80] != 0;
                snprintf(f->version, sizeof(f->version), "%u.%02u", le16(d + fo) >> 8, le16(d + fo) & 0xFF);
            }
            if (f->npoints < (int)(sizeof(f->points) / sizeof(f->points[0]))) {
                int pt = (int)le16(d + fo + 68), j, dup = 0;
                for (j = 0; j < f->npoints; j++) if (f->points[j] == pt) dup = 1;
                if (!dup) f->points[f->npoints++] = pt;
            }
        }
        p += 8 + (unsigned long)count * 12;
    }
    if (!f->family[0]) return -1;
    snprintf(f->full, sizeof(f->full), "%s", f->family);
    snprintf(f->subfamily, sizeof(f->subfamily), "%s", f->weight >= 600 ? (f->italic ? "Bold Italic" : "Bold")
                                                       : (f->italic ? "Italic" : "Regular"));
    f->flags = FI_BITMAP;
    out->kind = FI_FON;
    out->nfaces = 1;
    return 0;
}

int fi_parse(const unsigned char *d, size_t len, struct fi_file *out)
{
    memset(out, 0, sizeof(*out));
    if (!d || len < 12) return -1;
    if (be32(d) == 0x74746366UL) {                     /* ttcf */
        unsigned long n = be32(d + 8), i;
        if (!n || !inside(len, 12, n * 4)) return -1;
        for (i = 0; i < n && out->nfaces < FI_MAXFACES; i++)
            if (!parse_sfnt(d, len, be32(d + 12 + i * 4), &out->faces[out->nfaces])) out->nfaces++;
        if (!out->nfaces) return -1;
        out->kind = FI_TTC;
        return 0;
    }
    if (d[0] == 'M' && d[1] == 'Z') return parse_fon(d, len, out);
    if (parse_sfnt(d, len, 0, &out->faces[0])) return -1;
    out->kind = FI_SFNT;
    out->nfaces = 1;
    return 0;
}

void fi_describe(const struct fi_face *f, char *out, size_t cch)
{
    size_t o = 0;
    out[0] = 0;
#define ADD(s) do { o += (size_t)snprintf(out + o, o < cch ? cch - o : 0, "%s%s", o ? ", " : "", s); } while (0)
    if (f->flags & FI_BITMAP) { ADD("Raster font"); return; }
    if (f->flags & FI_OT_LAYOUT) ADD("OpenType Layout");
    if (f->flags & FI_SIGNED) ADD("Digitally Signed");
    if (f->flags & FI_TT_OUTLINES) ADD("TrueType Outlines");
    if (f->flags & FI_PS_OUTLINES) ADD("PostScript Outlines");
#undef ADD
    if (o >= cch && cch) out[cch - 1] = 0;
}

void fi_registry_name(const struct fi_file *f, char *out, size_t cch)
{
    size_t o = 0;
    int i;
    out[0] = 0;
    if (!f->nfaces) return;
    if (f->kind == FI_FON) {
        const struct fi_face *x = &f->faces[0];
        o = (size_t)snprintf(out, cch, "%s", x->family);
        for (i = 0; i < x->npoints && o < cch; i++)
            o += (size_t)snprintf(out + o, cch - o, "%s%d", i ? "," : " ", x->points[i]);
        return;
    }
    for (i = 0; i < f->nfaces && o < cch; i++)
        o += (size_t)snprintf(out + o, cch - o, "%s%s", i ? " & " : "", f->faces[i].full);
    if (o < cch) snprintf(out + o, cch - o, " (%s)", (f->faces[0].flags & FI_PS_OUTLINES) &&
                          !(f->faces[0].flags & FI_TT_OUTLINES) ? "OpenType" : "TrueType");
}
