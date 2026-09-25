/* sg-fontview -- reading what a font file says about itself.
 *
 * Plain C over a byte buffer (no Windows headers), so test/fontinfo-test.c
 * runs it natively. Font files come from anywhere -- a download, an email --
 * so every offset is checked against the buffer before it is read.
 *
 *   TrueType / OpenType (.ttf .otf): the sfnt table directory; the 'name'
 *     table (Windows' English names first), 'OS/2' weight and italic, 'head'
 *     macStyle when there is no OS/2, which outlines (glyf / CFF) and
 *     whether it has OpenType Layout (GSUB/GPOS) or a signature (DSIG).
 *   TrueType collections (.ttc): each face in the 'ttcf' header.
 *   Windows bitmap fonts (.fon): the NE executable's RT_FONT resources, their
 *     FNT headers (face name, copyright, point size).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_FONTINFO_H
#define SG_FONTINFO_H

#include <stddef.h>

#define FI_MAXFACES 64

enum fi_kind { FI_UNKNOWN, FI_SFNT, FI_TTC, FI_FON };

enum {
    FI_TT_OUTLINES = 1,     /* glyf */
    FI_PS_OUTLINES = 2,     /* CFF / CFF2 */
    FI_OT_LAYOUT   = 4,     /* GSUB or GPOS */
    FI_SIGNED      = 8,     /* DSIG */
    FI_BITMAP      = 16,    /* a .fon's raster font */
};

/* strings are UTF-8 */
struct fi_face {
    char family[128];       /* name ID 1 (what GDI's LOGFONT names) */
    char subfamily[64];     /* name ID 2 */
    char full[192];         /* name ID 4 */
    char version[128];      /* name ID 5 */
    char copyright[512];    /* name ID 0 */
    char typo_family[128];  /* name ID 16 */
    char manufacturer[128]; /* name ID 8 */
    char designer[128];     /* name ID 9 */
    char description[512];  /* name ID 10 */
    char license[512];      /* name ID 13 */
    int weight;             /* 100..900 */
    int italic;
    unsigned flags;         /* FI_* */
    int points[16];         /* a .fon's sizes */
    int npoints;
};

struct fi_file {
    enum fi_kind kind;
    int nfaces;
    struct fi_face faces[FI_MAXFACES];
};

/* 0 when the buffer is a font we understand (out filled), -1 otherwise */
int fi_parse(const unsigned char *data, size_t len, struct fi_file *out);

/* "OpenType Layout, Digitally Signed, TrueType Outlines", as Windows' font
 * viewer describes a file */
void fi_describe(const struct fi_face *f, char *out, size_t cch);

/* the registry value name Windows gives a file: "Arial Bold (TrueType)",
 * "Cambria & Cambria Math (TrueType)" for a collection, "Modern (All res)"
 * style for bitmap fonts */
void fi_registry_name(const struct fi_file *f, char *out, size_t cch);

#endif
