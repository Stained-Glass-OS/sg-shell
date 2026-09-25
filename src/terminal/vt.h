/* sg-terminal -- the terminal screen: a VT/xterm parser over a grid of cells
 * with scrollback. Plain C with no Windows headers, so the parser is tested
 * natively (test/terminal-vt-test.c) as well as used by the Windows program.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_TERMINAL_VT_H
#define SG_TERMINAL_VT_H

#include <stdint.h>
#include <stddef.h>

/* a colour: a palette index 0-255, the default, or 24-bit RGB */
#define VT_DEFAULT   0x01000000u
#define VT_RGB(r, g, b) (0x02000000u | ((uint32_t)(r) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(b))
#define VT_IS_RGB(c) (((c) & 0xff000000u) == 0x02000000u)

enum { VT_BOLD = 1, VT_UNDERLINE = 2, VT_REVERSE = 4, VT_ITALIC = 8, VT_DIM = 16, VT_STRIKE = 32, VT_WIDE = 64,
       VT_WIDE_TAIL = 128 };

struct vt_cell {
    uint32_t ch;            /* a code point; 0 is an empty cell (drawn as a space) */
    uint32_t fg, bg;
    uint8_t flags;
};

struct vt_line {
    struct vt_cell *cells;
    int width;              /* cells allocated */
    int wrapped;            /* this line continues on the next (a soft wrap) */
};

struct vt {
    int cols, rows;
    struct vt_line *screen;         /* rows lines, the one shown */
    struct vt_line *main_saved;     /* the main screen while the alternate one is shown */
    struct vt_line *scrollback;     /* a ring of lines scrolled off the top of the main screen */
    int sb_cap, sb_count, sb_head;  /* sb_head: where the next line goes */
    int cx, cy, wrap_pending;
    int saved_cx, saved_cy;
    struct vt_cell pen;             /* the attributes new characters get */
    struct vt_cell saved_pen;
    int top, bottom;                /* scroll region, inclusive */
    int cursor_visible, alt, app_cursor_keys, bracketed_paste, autowrap;
    char title[256];
    int title_changed;
    int bell;                       /* BEL count since last looked at */
    /* the parser */
    int state;
    char seq[512];
    int seq_len;
    uint32_t utf8_cp;
    int utf8_left;
    /* replies to the program (device status reports): appended here */
    char reply[256];
    int reply_len;
    unsigned long long generation;  /* bumped on every change, for redraws */
};

int  vt_init(struct vt *t, int cols, int rows, int scrollback);
void vt_free(struct vt *t);
void vt_resize(struct vt *t, int cols, int rows);
void vt_write(struct vt *t, const char *data, size_t len);       /* the program's output, UTF-8 */
void vt_reset(struct vt *t);

/* the line shown at view row y when scrolled back `offset` lines (0: live) */
const struct vt_line *vt_view_line(const struct vt *t, int offset, int y);
int  vt_line_text(const struct vt_line *l, int cols, char *out, int cap);   /* UTF-8, trailing blanks trimmed */

/* the 256-colour palette entry, as 0xRRGGBB (0-15 from the scheme) */
uint32_t vt_palette_rgb(const uint32_t scheme16[16], int index);

#endif
