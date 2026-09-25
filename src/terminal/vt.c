/* sg-terminal -- the terminal screen: a VT/xterm parser over a grid of cells.
 *
 * What it understands is what Windows Terminal's hosts send and what
 * console programs write with ENABLE_VIRTUAL_TERMINAL_PROCESSING: printable
 * UTF-8 (wide East Asian characters take two cells), C0 controls (BS, HT,
 * LF, VT, FF, CR, BEL), ESC 7/8/D/E/M/c, charset designations (ignored),
 * CSI cursor movement (A-H, a, d, e, f, `), erasing (J, K, X), inserting and
 * deleting (@, P, L, M), scrolling (S, T) and the scroll region (r), SGR (m:
 * bold, dim, italic, underline, reverse, strike; 16, 256 and RGB colours),
 * save/restore (s, u), device status (5n, 6n) and attributes (c), DEC
 * private modes (?1 cursor keys, ?7 autowrap, ?25 cursor, ?47/?1047/?1049
 * alternate screen, ?2004 bracketed paste), OSC 0/2 (the title), and DCS
 * strings (skipped).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "vt.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { S_GROUND, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_CHARSET, S_DCS, S_DCS_ESC };

static struct vt_cell blank_of(const struct vt *t)
{
    struct vt_cell c = { 0, t->pen.fg, t->pen.bg, 0 };
    if (t->pen.flags & VT_REVERSE) { c.fg = VT_DEFAULT; c.bg = VT_DEFAULT; }
    return c;
}

static int line_alloc(struct vt_line *l, int cols, struct vt_cell blank)
{
    int i;
    l->cells = malloc(sizeof(*l->cells) * (cols > 0 ? cols : 1));
    if (!l->cells) return 0;
    l->width = cols;
    l->wrapped = 0;
    for (i = 0; i < cols; i++) l->cells[i] = blank;
    return 1;
}

static void line_clear(struct vt_line *l, int from, int to, struct vt_cell blank)
{
    int i;
    if (from < 0) from = 0;
    if (to > l->width) to = l->width;
    for (i = from; i < to; i++) l->cells[i] = blank;
}

int vt_init(struct vt *t, int cols, int rows, int scrollback)
{
    struct vt_cell blank = { 0, VT_DEFAULT, VT_DEFAULT, 0 };
    int y;
    memset(t, 0, sizeof(*t));
    if (cols < 2) cols = 2;
    if (rows < 1) rows = 1;
    t->cols = cols; t->rows = rows;
    t->pen = blank;
    t->screen = calloc(rows, sizeof(*t->screen));
    t->sb_cap = scrollback > 0 ? scrollback : 1;
    t->scrollback = calloc(t->sb_cap, sizeof(*t->scrollback));
    if (!t->screen || !t->scrollback) return 0;
    for (y = 0; y < rows; y++) if (!line_alloc(&t->screen[y], cols, blank)) return 0;
    t->top = 0; t->bottom = rows - 1;
    t->cursor_visible = 1;
    t->autowrap = 1;
    return 1;
}

static void free_lines(struct vt_line *l, int n)
{
    int i;
    if (!l) return;
    for (i = 0; i < n; i++) free(l[i].cells);
    free(l);
}

void vt_free(struct vt *t)
{
    free_lines(t->screen, t->rows);
    free_lines(t->main_saved, t->rows);
    free_lines(t->scrollback, t->sb_cap);
    memset(t, 0, sizeof(*t));
}

/* the top line of the main screen leaves for the scrollback */
static void push_scrollback(struct vt *t, struct vt_line *l)
{
    struct vt_line *slot = &t->scrollback[t->sb_head];
    free(slot->cells);
    *slot = *l;
    t->sb_head = (t->sb_head + 1) % t->sb_cap;
    if (t->sb_count < t->sb_cap) t->sb_count++;
}

/* lines [top, bottom] move up by n (down when n < 0); what enters is blank */
static void scroll_region(struct vt *t, int top, int bottom, int n)
{
    struct vt_cell blank = blank_of(t);
    int i, count = bottom - top + 1;
    if (top < 0 || bottom >= t->rows || top > bottom || !n) return;
    if (n > count) n = count;
    if (n < -count) n = -count;
    if (n > 0) {
        for (i = 0; i < n; i++) {
            struct vt_line gone = t->screen[top];
            memmove(&t->screen[top], &t->screen[top + 1], (count - 1) * sizeof(*t->screen));
            if (top == 0 && !t->alt) {
                struct vt_line fresh;
                push_scrollback(t, &gone);
                if (!line_alloc(&fresh, t->cols, blank)) { fresh.cells = NULL; fresh.width = 0; }
                t->screen[bottom] = fresh;
            } else {
                line_clear(&gone, 0, gone.width, blank);
                gone.wrapped = 0;
                t->screen[bottom] = gone;
            }
        }
    } else {
        for (i = 0; i < -n; i++) {
            struct vt_line gone = t->screen[bottom];
            memmove(&t->screen[top + 1], &t->screen[top], (count - 1) * sizeof(*t->screen));
            line_clear(&gone, 0, gone.width, blank);
            gone.wrapped = 0;
            t->screen[top] = gone;
        }
    }
}

static void linefeed(struct vt *t)
{
    if (t->cy == t->bottom) scroll_region(t, t->top, t->bottom, 1);
    else if (t->cy < t->rows - 1) t->cy++;
}

static int char_width(uint32_t c)
{
    if (c < 0x300) return 1;
    if ((c >= 0x300 && c <= 0x36f) || (c >= 0x200b && c <= 0x200f) || (c >= 0xfe00 && c <= 0xfe0f)) return 0;
    if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0x303e) || (c >= 0x3041 && c <= 0x33ff) ||
        (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xa000 && c <= 0xa4cf) ||
        (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe4f) ||
        (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) || (c >= 0x1f300 && c <= 0x1f64f) ||
        (c >= 0x1f900 && c <= 0x1f9ff) || (c >= 0x20000 && c <= 0x3fffd))
        return 2;
    return 1;
}

static void put_char(struct vt *t, uint32_t c)
{
    int w = char_width(c);
    struct vt_line *l;
    if (!w) return;             /* combining marks: not drawn as cells of their own */
    if (t->wrap_pending || (w == 2 && t->cx == t->cols - 1)) {
        if (t->autowrap) {
            t->screen[t->cy].wrapped = 1;
            t->cx = 0;
            linefeed(t);
        }
        t->wrap_pending = 0;
    }
    l = &t->screen[t->cy];
    if (t->cx >= l->width) return;
    l->cells[t->cx] = t->pen;
    l->cells[t->cx].ch = c;
    l->cells[t->cx].flags = (t->pen.flags & ~(VT_WIDE | VT_WIDE_TAIL)) | (w == 2 ? VT_WIDE : 0);
    if (w == 2 && t->cx + 1 < l->width) {
        l->cells[t->cx + 1] = l->cells[t->cx];
        l->cells[t->cx + 1].ch = 0;
        l->cells[t->cx + 1].flags = (l->cells[t->cx].flags & ~VT_WIDE) | VT_WIDE_TAIL;
    }
    if (t->cx + w >= t->cols) { t->cx = t->cols - 1; t->wrap_pending = 1; }
    else t->cx += w;
}

static void clamp_cursor(struct vt *t)
{
    if (t->cx < 0) t->cx = 0;
    if (t->cx >= t->cols) t->cx = t->cols - 1;
    if (t->cy < 0) t->cy = 0;
    if (t->cy >= t->rows) t->cy = t->rows - 1;
    t->wrap_pending = 0;
}

static void reply(struct vt *t, const char *s)
{
    int n = (int)strlen(s);
    if (t->reply_len + n > (int)sizeof(t->reply)) return;
    memcpy(t->reply + t->reply_len, s, n);
    t->reply_len += n;
}

static void set_alt(struct vt *t, int on)
{
    struct vt_cell blank = { 0, VT_DEFAULT, VT_DEFAULT, 0 };
    int y;
    if (on == t->alt) return;
    if (on) {
        struct vt_line *alt = calloc(t->rows, sizeof(*alt));
        if (!alt) return;
        for (y = 0; y < t->rows; y++) line_alloc(&alt[y], t->cols, blank);
        t->main_saved = t->screen;
        t->screen = alt;
        t->saved_cx = t->cx; t->saved_cy = t->cy;
        t->alt = 1;
    } else {
        free_lines(t->screen, t->rows);
        t->screen = t->main_saved;
        t->main_saved = NULL;
        t->cx = t->saved_cx; t->cy = t->saved_cy;
        t->alt = 0;
        clamp_cursor(t);
    }
}

static uint32_t sgr_colour(const int *p, int n, int *i)
{
    /* after 38/48: 5;N or 2;R;G;B (the colon forms arrive the same way) */
    if (*i + 1 < n && p[*i + 1] == 5 && *i + 2 < n) { *i += 2; return (uint32_t)(p[*i] & 0xff); }
    if (*i + 1 < n && p[*i + 1] == 2 && *i + 4 < n) {
        uint32_t c = VT_RGB(p[*i + 2] & 0xff, p[*i + 3] & 0xff, p[*i + 4] & 0xff);
        *i += 4;
        return c;
    }
    *i = n;
    return VT_DEFAULT;
}

static void sgr(struct vt *t, const int *p, int n)
{
    int i;
#ifdef SG_MUTANT_NOSGR
    return;
#endif
    if (!n) { t->pen.fg = t->pen.bg = VT_DEFAULT; t->pen.flags = 0; return; }
    for (i = 0; i < n; i++) {
        int v = p[i];
        if (v == 0) { t->pen.fg = t->pen.bg = VT_DEFAULT; t->pen.flags = 0; }
        else if (v == 1) t->pen.flags |= VT_BOLD;
        else if (v == 2) t->pen.flags |= VT_DIM;
        else if (v == 3) t->pen.flags |= VT_ITALIC;
        else if (v == 4) t->pen.flags |= VT_UNDERLINE;
        else if (v == 7) t->pen.flags |= VT_REVERSE;
        else if (v == 9) t->pen.flags |= VT_STRIKE;
        else if (v == 21 || v == 22) t->pen.flags &= ~(VT_BOLD | VT_DIM);
        else if (v == 23) t->pen.flags &= ~VT_ITALIC;
        else if (v == 24) t->pen.flags &= ~VT_UNDERLINE;
        else if (v == 27) t->pen.flags &= ~VT_REVERSE;
        else if (v == 29) t->pen.flags &= ~VT_STRIKE;
        else if (v >= 30 && v <= 37) t->pen.fg = v - 30;
        else if (v == 38) t->pen.fg = sgr_colour(p, n, &i);
        else if (v == 39) t->pen.fg = VT_DEFAULT;
        else if (v >= 40 && v <= 47) t->pen.bg = v - 40;
        else if (v == 48) t->pen.bg = sgr_colour(p, n, &i);
        else if (v == 49) t->pen.bg = VT_DEFAULT;
        else if (v >= 90 && v <= 97) t->pen.fg = v - 90 + 8;
        else if (v >= 100 && v <= 107) t->pen.bg = v - 100 + 8;
    }
}

static void erase(struct vt *t, int y, int from, int to)
{
    if (y >= 0 && y < t->rows) line_clear(&t->screen[y], from, to, blank_of(t));
}

static void csi(struct vt *t)
{
    int p[32] = { 0 }, n = 0, i = 0, have = 0, a;
    char priv = 0, inter = 0, final = t->seq[t->seq_len - 1];
    if (t->seq_len > 1 && (t->seq[0] == '?' || t->seq[0] == '>' || t->seq[0] == '=' || t->seq[0] == '<')) priv = t->seq[i++];
    for (; i < t->seq_len - 1; i++) {
        char c = t->seq[i];
        if (c >= '0' && c <= '9') { if (n < 32) p[n] = p[n] * 10 + (c - '0'); have = 1; if (n < 32 && p[n] > 65535) p[n] = 65535; }
        else if (c == ';' || c == ':') { if (n < 31) n++; have = 1; }
        else if (c >= 0x20 && c <= 0x2f) inter = c;
    }
    if (have && n < 32) n++;
    a = n && p[0] ? p[0] : 1;
    if (priv == '?') {
        int set = final == 'h';
        if (final != 'h' && final != 'l') return;
        for (i = 0; i < n; i++) switch (p[i]) {
            case 1: t->app_cursor_keys = set; break;
            case 7: t->autowrap = set; break;
            case 25: t->cursor_visible = set; break;
            case 47: case 1047: set_alt(t, set); break;
            case 1049:
                if (set) { t->saved_cx = t->cx; t->saved_cy = t->cy; set_alt(t, 1); }
                else set_alt(t, 0);
                break;
            case 2004: t->bracketed_paste = set; break;
        }
        return;
    }
    if (priv || inter) {
        if (final == 'q' && inter == ' ') return;       /* cursor shape */
        if (final == 'c' && priv == '>') reply(t, "\x1b[>0;10;1c");
        return;
    }
    switch (final) {
    case 'A': t->cy -= a; if (t->cy < t->top && t->cy + a >= t->top) t->cy = t->top; clamp_cursor(t); break;
    case 'B': case 'e': t->cy += a; if (t->cy > t->bottom && t->cy - a <= t->bottom) t->cy = t->bottom; clamp_cursor(t); break;
    case 'C': case 'a': t->cx += a; clamp_cursor(t); break;
    case 'D': t->cx -= a; clamp_cursor(t); break;
    case 'E': t->cy += a; t->cx = 0; clamp_cursor(t); break;
    case 'F': t->cy -= a; t->cx = 0; clamp_cursor(t); break;
    case 'G': case '`': t->cx = a - 1; clamp_cursor(t); break;
    case 'd': t->cy = a - 1; clamp_cursor(t); break;
    case 'H': case 'f': t->cy = a - 1; t->cx = (n > 1 && p[1] ? p[1] : 1) - 1; clamp_cursor(t); break;
    case 'J': {
        int mode = n ? p[0] : 0, y;
        if (mode == 0) { erase(t, t->cy, t->cx, t->cols); for (y = t->cy + 1; y < t->rows; y++) erase(t, y, 0, t->cols); }
        else if (mode == 1) { for (y = 0; y < t->cy; y++) erase(t, y, 0, t->cols); erase(t, t->cy, 0, t->cx + 1); }
        else if (mode == 2) { for (y = 0; y < t->rows; y++) erase(t, y, 0, t->cols); }
        else if (mode == 3) { int k; for (k = 0; k < t->sb_cap; k++) { free(t->scrollback[k].cells); t->scrollback[k].cells = NULL; t->scrollback[k].width = 0; } t->sb_count = t->sb_head = 0; }
        break;
    }
    case 'K': {
        int mode = n ? p[0] : 0;
        if (mode == 0) erase(t, t->cy, t->cx, t->cols);
        else if (mode == 1) erase(t, t->cy, 0, t->cx + 1);
        else erase(t, t->cy, 0, t->cols);
        break;
    }
    case 'X': erase(t, t->cy, t->cx, t->cx + a); break;
    case 'P': case '@': {
        struct vt_line *l = &t->screen[t->cy];
        int count = a > t->cols - t->cx ? t->cols - t->cx : a;
        if (final == 'P') {
            memmove(&l->cells[t->cx], &l->cells[t->cx + count], (t->cols - t->cx - count) * sizeof(*l->cells));
            line_clear(l, t->cols - count, t->cols, blank_of(t));
        } else {
            memmove(&l->cells[t->cx + count], &l->cells[t->cx], (t->cols - t->cx - count) * sizeof(*l->cells));
            line_clear(l, t->cx, t->cx + count, blank_of(t));
        }
        break;
    }
    case 'L': if (t->cy >= t->top && t->cy <= t->bottom) scroll_region(t, t->cy, t->bottom, -a); break;
    case 'M': if (t->cy >= t->top && t->cy <= t->bottom) scroll_region(t, t->cy, t->bottom, a); break;
    case 'S': scroll_region(t, t->top, t->bottom, a); break;
    case 'T': scroll_region(t, t->top, t->bottom, -a); break;
    case 'r': {
        int top = (n && p[0] ? p[0] : 1) - 1, bottom = (n > 1 && p[1] ? p[1] : t->rows) - 1;
        if (top < bottom && bottom < t->rows) { t->top = top; t->bottom = bottom; t->cx = 0; t->cy = 0; t->wrap_pending = 0; }
        break;
    }
    case 'm': sgr(t, p, n); break;
    case 's': t->saved_cx = t->cx; t->saved_cy = t->cy; break;
    case 'u': t->cx = t->saved_cx; t->cy = t->saved_cy; clamp_cursor(t); break;
    case 'n':
        if (n && p[0] == 5) reply(t, "\x1b[0n");
        else if (n && p[0] == 6) {
            char buf[32];
            snprintf(buf, sizeof(buf), "\x1b[%d;%dR", t->cy + 1, t->cx + 1);
            reply(t, buf);
        }
        break;
    case 'c': if (!n || !p[0]) reply(t, "\x1b[?1;0c"); break;
    }
}

static void osc(struct vt *t)
{
    /* seq: "0;title" or "2;title" */
    if (t->seq_len >= 2 && (t->seq[0] == '0' || t->seq[0] == '2') && t->seq[1] == ';') {
        int n = t->seq_len - 2;
        if (n >= (int)sizeof(t->title)) n = sizeof(t->title) - 1;
        memcpy(t->title, t->seq + 2, n);
        t->title[n] = 0;
        t->title_changed = 1;
    }
}

static void esc_final(struct vt *t, char c)
{
    switch (c) {
    case '7': t->saved_cx = t->cx; t->saved_cy = t->cy; t->saved_pen = t->pen; break;
    case '8': t->cx = t->saved_cx; t->cy = t->saved_cy; t->pen = t->saved_pen; clamp_cursor(t); break;
    case 'D': linefeed(t); break;
    case 'E': t->cx = 0; linefeed(t); break;
    case 'M':
        if (t->cy == t->top) scroll_region(t, t->top, t->bottom, -1);
        else if (t->cy > 0) t->cy--;
        break;
    case 'c': vt_reset(t); break;
    }
}

void vt_reset(struct vt *t)
{
    int y;
    set_alt(t, 0);
    t->pen.fg = t->pen.bg = VT_DEFAULT; t->pen.flags = 0; t->pen.ch = 0;
    for (y = 0; y < t->rows; y++) { line_clear(&t->screen[y], 0, t->cols, t->pen); t->screen[y].wrapped = 0; }
    t->cx = t->cy = 0; t->wrap_pending = 0;
    t->top = 0; t->bottom = t->rows - 1;
    t->cursor_visible = 1; t->autowrap = 1; t->app_cursor_keys = 0; t->bracketed_paste = 0;
}

static void control(struct vt *t, unsigned char c)
{
    switch (c) {
    case 7: t->bell++; break;
    case 8: if (t->wrap_pending) t->wrap_pending = 0; else if (t->cx > 0) t->cx--; break;
    case 9: { int x = (t->cx / 8 + 1) * 8; t->cx = x >= t->cols ? t->cols - 1 : x; t->wrap_pending = 0; break; }
    case 10: case 11: case 12: linefeed(t); t->wrap_pending = 0; break;
    case 13: t->cx = 0; t->wrap_pending = 0; break;
    }
}

static void byte(struct vt *t, unsigned char c)
{
    switch (t->state) {
    case S_GROUND:
        if (c == 0x1b) { t->state = S_ESC; t->utf8_left = 0; return; }
        if (c < 0x20) { control(t, c); t->utf8_left = 0; return; }
        if (c == 0x7f) return;
        if (t->utf8_left) {
            if ((c & 0xc0) == 0x80) {
                t->utf8_cp = (t->utf8_cp << 6) | (c & 0x3f);
                if (--t->utf8_left == 0) put_char(t, t->utf8_cp);
                return;
            }
            t->utf8_left = 0;       /* a broken sequence: start again with this byte */
        }
        if (c < 0x80) put_char(t, c);
        else if ((c & 0xe0) == 0xc0) { t->utf8_cp = c & 0x1f; t->utf8_left = 1; }
        else if ((c & 0xf0) == 0xe0) { t->utf8_cp = c & 0x0f; t->utf8_left = 2; }
        else if ((c & 0xf8) == 0xf0) { t->utf8_cp = c & 0x07; t->utf8_left = 3; }
        else put_char(t, 0xfffd);
        return;
    case S_ESC:
        t->seq_len = 0;
        if (c == '[') { t->state = S_CSI; return; }
        if (c == ']') { t->state = S_OSC; return; }
        if (c == 'P') { t->state = S_DCS; return; }
        if (c == '(' || c == ')' || c == '*' || c == '+' || c == '#' || c == '%') { t->state = S_CHARSET; return; }
        t->state = S_GROUND;
        esc_final(t, c);
        return;
    case S_CHARSET: t->state = S_GROUND; return;
    case S_CSI:
        if (c == 0x1b) { t->state = S_ESC; return; }
        if (c < 0x20) { control(t, c); return; }       /* C0 inside CSI acts, as on a VT */
        if (t->seq_len < (int)sizeof(t->seq)) t->seq[t->seq_len++] = c;
        if (c >= 0x40 && c <= 0x7e) { t->state = S_GROUND; csi(t); }
        return;
    case S_OSC:
        if (c == 7) { t->state = S_GROUND; osc(t); return; }
        if (c == 0x1b) { t->state = S_OSC_ESC; return; }
        if (t->seq_len < (int)sizeof(t->seq)) t->seq[t->seq_len++] = c;
        return;
    case S_OSC_ESC:
        t->state = S_GROUND;
        if (c == '\\') osc(t);
        else { t->state = S_ESC; byte(t, c); }
        return;
    case S_DCS:
        if (c == 0x1b) t->state = S_DCS_ESC;
        return;
    case S_DCS_ESC:
        t->state = c == '\\' ? S_GROUND : S_DCS;
        return;
    }
}

void vt_write(struct vt *t, const char *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) byte(t, (unsigned char)data[i]);
    t->generation++;
}

static void resize_lines(struct vt_line *lines, int n, int cols)
{
    struct vt_cell blank = { 0, VT_DEFAULT, VT_DEFAULT, 0 };
    int y, x;
    for (y = 0; y < n; y++) {
        struct vt_line *l = &lines[y];
        struct vt_cell *c;
        if (l->width >= cols) continue;
        c = realloc(l->cells, cols * sizeof(*c));
        if (!c) continue;
        for (x = l->width; x < cols; x++) c[x] = blank;
        l->cells = c;
        l->width = cols;
    }
}

void vt_resize(struct vt *t, int cols, int rows)
{
    struct vt_cell blank = { 0, VT_DEFAULT, VT_DEFAULT, 0 };
    struct vt_line *screen;
    int y, drop = 0;
    if (cols < 2) cols = 2;
    if (rows < 1) rows = 1;
    if (cols == t->cols && rows == t->rows) return;
    if (t->alt) set_alt(t, 0);      /* full-screen programs redraw after a resize */
    /* fewer rows: the top lines go to the scrollback, so the cursor's line stays */
    if (rows < t->rows && t->cy >= rows) drop = t->cy - rows + 1;
    screen = calloc(rows, sizeof(*screen));
    if (!screen) return;
    for (y = 0; y < drop; y++) push_scrollback(t, &t->screen[y]);
    for (y = 0; y < rows; y++) {
        if (y + drop < t->rows) screen[y] = t->screen[y + drop];
        else line_alloc(&screen[y], cols, blank);
    }
    for (y = rows + drop; y < t->rows; y++) free(t->screen[y].cells);
    free(t->screen);
    t->screen = screen;
    resize_lines(t->screen, rows, cols);
    t->cy -= drop;
    t->cols = cols; t->rows = rows;
    t->top = 0; t->bottom = rows - 1;
    clamp_cursor(t);
    t->generation++;
}

const struct vt_line *vt_view_line(const struct vt *t, int offset, int y)
{
    /* view row y, `offset` lines back into the scrollback */
    int idx = y - offset;
    if (idx >= 0) return idx < t->rows ? &t->screen[idx] : NULL;
    idx = -idx;                     /* 1: the newest scrollback line */
    if (idx > t->sb_count) return NULL;
    return &t->scrollback[(t->sb_head - idx + t->sb_cap) % t->sb_cap];
}

int vt_line_text(const struct vt_line *l, int cols, char *out, int cap)
{
    int x, n = 0, end = 0;
    if (!l || cap < 1) { if (cap) out[0] = 0; return 0; }
    if (cols > l->width) cols = l->width;
    for (x = 0; x < cols; x++) {
        uint32_t c = l->cells[x].ch;
        if (l->cells[x].flags & VT_WIDE_TAIL) continue;
        if (!c) c = ' ';
        if (n + 4 >= cap) break;
        if (c < 0x80) out[n++] = (char)c;
        else if (c < 0x800) { out[n++] = 0xc0 | (c >> 6); out[n++] = 0x80 | (c & 0x3f); }
        else if (c < 0x10000) { out[n++] = 0xe0 | (c >> 12); out[n++] = 0x80 | ((c >> 6) & 0x3f); out[n++] = 0x80 | (c & 0x3f); }
        else { out[n++] = 0xf0 | (c >> 18); out[n++] = 0x80 | ((c >> 12) & 0x3f); out[n++] = 0x80 | ((c >> 6) & 0x3f); out[n++] = 0x80 | (c & 0x3f); }
        if (c != ' ') end = n;
    }
    out[end] = 0;
    return end;
}

uint32_t vt_palette_rgb(const uint32_t scheme16[16], int index)
{
    static const unsigned char level[6] = { 0, 95, 135, 175, 215, 255 };
    if (index < 16) return scheme16[index & 15];
    if (index < 232) {
        index -= 16;
        return (uint32_t)level[index / 36] << 16 | (uint32_t)level[(index / 6) % 6] << 8 | level[index % 6];
    }
    index = 8 + (index - 232) * 10;
    return (uint32_t)index << 16 | (uint32_t)index << 8 | (uint32_t)index;
}
