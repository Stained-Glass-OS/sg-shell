/* sg-paint -- the canvas: showing the picture at a zoom, the tools, the
 * selection (lifted into a floating picture whose alpha is its mask), the
 * text box, and the canvas's own resize handles.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "paint.h"

int zoom_step(int dir);
void canvas_set_size(int w, int h);
void status_message(const WCHAR *s);

enum { M_NONE, M_DRAW, M_SHAPE, M_SELECTING, M_FREESEL, M_MOVING, M_RESIZE, M_TEXTBOX };
enum { H_NONE, H_RIGHT, H_BOTTOM, H_CORNER };

static int g_scx, g_scy;                /* scroll position, in screen pixels */
static int g_mode, g_button, g_handle;
static POINT g_start, g_last, g_cur;    /* picture coordinates */
static int g_mouse_in;
static int g_newsize_w, g_newsize_h;

/* selection */
static int g_sel;
static RECT g_selrc;                    /* picture coordinates; the float's place when lifted */
static BYTE *g_selmask;                 /* free-form: 1 per pixel of g_selrc, NULL = rectangle */
static Img g_float;                     /* lifted pixels; alpha = mask */
static POINT g_move_org;
static POINT *g_fpath; static int g_npath, g_cappath;

/* text */
static HWND g_edit;
static RECT g_textrc;
static HFONT g_editfont;
static HBRUSH g_editbrush;

/* the marker's per-stroke coverage, so a stroke does not darken itself */
static BYTE *g_cover;

static int MARGIN(void) { return S(6); }
static int vw(void) { return max(1, (int)((long long)g_img.w * g_zoom / 1000)); }
static int vh(void) { return max(1, (int)((long long)g_img.h * g_zoom / 1000)); }
static int ox(void) { return MARGIN() - g_scx; }
static int oy(void) { return MARGIN() - g_scy; }

static int fdiv(long long a, long long b) { return (int)(a >= 0 ? a / b : -((-a + b - 1) / b)); }
static POINT to_img(int cx, int cy)
{
    POINT p;
    p.x = fdiv((long long)(cx - ox()) * 1000, g_zoom);
    p.y = fdiv((long long)(cy - oy()) * 1000, g_zoom);
    return p;
}
static int to_cx(int ix) { return ox() + (int)((long long)ix * g_zoom / 1000); }
static int to_cy(int iy) { return oy() + (int)((long long)iy * g_zoom / 1000); }

BOOL sel_active(void) { return g_sel; }
void sel_rect(RECT *r) { *r = g_selrc; }
BOOL text_active(void) { return g_edit != NULL; }

void canvas_cursor_info(int *x, int *y, int *in) { *x = g_cur.x; *y = g_cur.y; *in = g_mouse_in; }

void canvas_dump(FILE *f)
{
    POINT p = { ox(), oy() };
    RECT r;
    ClientToScreen(g_canvas, &p);
    GetClientRect(g_canvas, &r);
    MapWindowPoints(g_canvas, NULL, (POINT *)&r, 2);
    fprintf(f, "canvas %ld %ld %d\n", p.x, p.y, g_zoom);
    fprintf(f, "canvaswin %ld %ld %ld %ld\n", r.left, r.top, r.right, r.bottom);
    fprintf(f, "floating %d\n", g_float.px != NULL);
}

static void update_scroll(void)
{
    RECT rc;
    SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS, 0 };
    int cw = vw() + 2 * MARGIN() + S(8), ch = vh() + 2 * MARGIN() + S(8), i;
    for (i = 0; i < 2; i++)                     /* twice: a bar appearing shrinks the other side */
    {
        GetClientRect(g_canvas, &rc);
        g_scx = max(0, min(g_scx, cw - rc.right));
        g_scy = max(0, min(g_scy, ch - rc.bottom));
        si.nMax = cw - 1; si.nPage = rc.right; si.nPos = g_scx;
        SetScrollInfo(g_canvas, SB_HORZ, &si, TRUE);
        si.nMax = ch - 1; si.nPage = rc.bottom; si.nPos = g_scy;
        SetScrollInfo(g_canvas, SB_VERT, &si, TRUE);
    }
}

static void text_place(void);

void canvas_changed(BOOL resized)
{
    if (!g_canvas) return;
    if (resized) update_scroll();
    text_place();
    InvalidateRect(g_canvas, NULL, FALSE);
    InvalidateRect(g_status, NULL, FALSE);
    InvalidateRect(g_ribbon, NULL, FALSE);
}

void canvas_zoom(int zoom, int ax, int ay)
{
    RECT rc;
    double ix, iy;
    if (zoom < 125) zoom = 125;
    if (zoom > 8000) zoom = 8000;
    GetClientRect(g_canvas, &rc);
    if (ax < 0) { ax = min(rc.right / 2, to_cx(g_img.w) - 1); ay = min(rc.bottom / 2, to_cy(g_img.h) - 1); if (ax < 0) ax = 0; if (ay < 0) ay = 0; }
    ix = (double)(ax - ox()) * 1000 / g_zoom;
    iy = (double)(ay - oy()) * 1000 / g_zoom;
    g_zoom = zoom;
    g_scx = (int)(MARGIN() + ix * zoom / 1000 - ax);
    g_scy = (int)(MARGIN() + iy * zoom / 1000 - ay);
    canvas_changed(TRUE);
    write_dump();
}

/* ---- pixels ---- */
static void put(int x, int y, DWORD v)
{
    if (x >= 0 && y >= 0 && x < g_img.w && y < g_img.h) g_img.px[(size_t)y * g_img.w + x] = v;
}

static void blend(int x, int y, DWORD v, int a)
{
    DWORD *d, o;
    if (x < 0 || y < 0 || x >= g_img.w || y >= g_img.h) return;
    if (g_cover) { BYTE *c = g_cover + (size_t)y * g_img.w + x; if (*c) return; *c = 1; }
    d = g_img.px + (size_t)y * g_img.w + x; o = *d;
    *d = 0xFF000000u |
         (((((v >> 16) & 0xFF) * a + ((o >> 16) & 0xFF) * (255 - a)) / 255) << 16) |
         (((((v >> 8) & 0xFF) * a + ((o >> 8) & 0xFF) * (255 - a)) / 255) << 8) |
         (((v & 0xFF) * a + (o & 0xFF) * (255 - a)) / 255);
}

static void square(int cx, int cy, int s, DWORD v)
{
    int x, y, x0 = cx - (s - 1) / 2, y0 = cy - (s - 1) / 2;
    for (y = y0; y < y0 + s; y++) for (x = x0; x < x0 + s; x++) put(x, y, v);
}

static void disc(int cx, int cy, int s, DWORD v)
{
    int x, y;
    double r = s / 2.0;
    if (s <= 2) { square(cx, cy, s, v); return; }
    for (y = -s; y <= s; y++)
        for (x = -s; x <= s; x++)
            if ((x + 0.5 - (s & 1 ? 0.5 : 0)) * (x + 0.5 - (s & 1 ? 0.5 : 0)) +
                (y + 0.5 - (s & 1 ? 0.5 : 0)) * (y + 0.5 - (s & 1 ? 0.5 : 0)) <= r * r)
                put(cx + x, cy + y, v);
}

static unsigned int rnd(void) { static unsigned int s = 12345; s = s * 1103515245 + 12345; return (s >> 16) & 0x7FFF; }

/* one dab of the current tool at x,y */
static void stamp(int x, int y, int button)
{
    DWORD fg = rgb2px(button == 1 ? g_color1 : g_color2), bg = rgb2px(button == 1 ? g_color2 : g_color1);
    int s = tool_size(), i, j;
    switch (g_tool)
    {
    case T_PENCIL: square(x, y, s, fg); break;
    case T_ERASER:
        if (button == 1) square(x, y, s, rgb2px(g_color2));
        else
        {
            /* the right button replaces colour 1 with colour 2 */
            DWORD from = rgb2px(g_color1) & 0xFFFFFF;
            int x0 = x - (s - 1) / 2, y0 = y - (s - 1) / 2;
            for (j = y0; j < y0 + s; j++)
                for (i = x0; i < x0 + s; i++)
                    if (i >= 0 && j >= 0 && i < g_img.w && j < g_img.h && (g_img.px[(size_t)j * g_img.w + i] & 0xFFFFFF) == from)
                        put(i, j, rgb2px(g_color2));
        }
        break;
    case T_BRUSH:
        switch (g_brush)
        {
        case B_BRUSH: disc(x, y, s, fg); break;
        case B_CALLI1: case B_CALLI2:
            for (i = -s; i <= s; i++) { put(x + i, y + (g_brush == B_CALLI1 ? -i : i), fg); put(x + i + 1, y + (g_brush == B_CALLI1 ? -i : i), fg); }
            break;
        case B_AIRBRUSH:
        {
            int r = 4 + s * 3;
            for (i = 0; i < 6 + s * 2; i++)
            {
                int dx = (int)(rnd() % (2 * r + 1)) - r, dy = (int)(rnd() % (2 * r + 1)) - r;
                if (dx * dx + dy * dy <= r * r) put(x + dx, y + dy, fg);
            }
            break;
        }
        case B_MARKER:
        {
            int m = s * 2 + 2;
            for (j = -m / 2; j < m - m / 2; j++) for (i = -m / 4; i < m - m / 4; i++) blend(x + i, y + j, fg, 150);
            break;
        }
        case B_CRAYON:
        {
            int r = s + 1;
            for (j = -r; j <= r; j++)
                for (i = -r; i <= r; i++)
                    if (i * i + j * j <= r * r && rnd() % 100 < 55) put(x + i, y + j, fg);
            break;
        }
        }
        break;
    }
    (void)bg;
}

static void stroke(POINT a, POINT b, int button)
{
    int dx = abs(b.x - a.x), dy = -abs(b.y - a.y), sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1, err = dx + dy;
    int n = 0;
    for (;;)
    {
        /* the airbrush sprays by time, not along the path */
        if (g_tool != T_BRUSH || g_brush != B_AIRBRUSH || n++ % 4 == 0) stamp(a.x, a.y, button);
        if (a.x == b.x && a.y == b.y) break;
        { int e2 = 2 * err; if (e2 >= dy) { err += dy; a.x += sx; } if (e2 <= dx) { err += dx; a.y += sy; } }
    }
}

/* ---- shapes ---- */
static void draw_shape(POINT a, POINT b, int button, BOOL constrain)
{
    COLORREF line = button == 1 ? g_color1 : g_color2, fill = button == 1 ? g_color2 : g_color1;
    int s = SIZE_PX[g_size_idx];
    RECT r;
    if (constrain)
    {
        int dx = b.x - a.x, dy = b.y - a.y;
        if (g_shape == S_LINE)
        {
            if (abs(dx) > 2 * abs(dy)) b.y = a.y;
            else if (abs(dy) > 2 * abs(dx)) b.x = a.x;
            else { int m = max(abs(dx), abs(dy)); b.x = a.x + (dx < 0 ? -m : m); b.y = a.y + (dy < 0 ? -m : m); }
        }
        else { int m = max(abs(dx), abs(dy)); b.x = a.x + (dx < 0 ? -m : m); b.y = a.y + (dy < 0 ? -m : m); }
    }
    if (g_shape == S_LINE)
    {
        int d = s, ox_ = a.x, oy_ = a.y;
        int dx = abs(b.x - a.x), dy = -abs(b.y - a.y), sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1, err = dx + dy;
        DWORD v = rgb2px(line);
        for (;;)
        {
            if (d <= 2) square(ox_, oy_, d, v); else disc(ox_, oy_, d, v);
            if (ox_ == b.x && oy_ == b.y) break;
            { int e2 = 2 * err; if (e2 >= dy) { err += dy; ox_ += sx; } if (e2 <= dx) { err += dx; oy_ += sy; } }
        }
        return;
    }
    r.left = min(a.x, b.x); r.top = min(a.y, b.y); r.right = max(a.x, b.x) + 1; r.bottom = max(a.y, b.y) + 1;
    if (!g_outline_on && !g_fill_on) return;
    {
        LOGBRUSH lb = { BS_SOLID, line, 0 };
        HPEN pen = g_outline_on ? ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_INSIDEFRAME | PS_ENDCAP_SQUARE | PS_JOIN_MITER, s, &lb, 0, NULL)
                                : GetStockObject(NULL_PEN);
        HBRUSH br = g_fill_on ? CreateSolidBrush(fill) : GetStockObject(NULL_BRUSH);
        HGDIOBJ op = SelectObject(g_imgdc, pen), ob = SelectObject(g_imgdc, br);
        int adj = g_outline_on ? 0 : 1;         /* without a pen GDI leaves off the right and bottom */
        POINT pts[64];
        int n;
        switch (g_shape)
        {
        case S_RECT: Rectangle(g_imgdc, r.left, r.top, r.right + adj, r.bottom + adj); break;
        case S_ROUNDRECT:
        {
            int rr = min(min(r.right - r.left, r.bottom - r.top) / 2, 16 + s);
            RoundRect(g_imgdc, r.left, r.top, r.right + adj, r.bottom + adj, rr, rr);
            break;
        }
        case S_ELLIPSE: Ellipse(g_imgdc, r.left, r.top, r.right + adj, r.bottom + adj); break;
        default:
        {
            RECT inner = r;
            if (g_outline_on) { inner.left += s / 2; inner.top += s / 2; inner.right -= (s + 1) / 2; inner.bottom -= (s + 1) / 2; }
            else { inner.right--; inner.bottom--; }
            shape_points(g_shape, inner, pts, &n);
            if (g_outline_on)
            {
                /* a polygon's corners stay inside the box with a centred pen */
                DeleteObject(SelectObject(g_imgdc, ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, s, &lb, 0, NULL)));
                pen = GetCurrentObject(g_imgdc, OBJ_PEN);
            }
            Polygon(g_imgdc, pts, n);
        }
        }
        SelectObject(g_imgdc, op); SelectObject(g_imgdc, ob);
        if (g_outline_on) DeleteObject(pen);
        if (g_fill_on) DeleteObject(br);
        GdiFlush();
    }
}

/* ---- selection ---- */
static BOOL in_mask(int x, int y)       /* picture coordinates, within g_selrc */
{
    if (x < g_selrc.left || y < g_selrc.top || x >= g_selrc.right || y >= g_selrc.bottom) return FALSE;
    if (g_float.px) return (g_float.px[(size_t)(y - g_selrc.top) * g_float.w + (x - g_selrc.left)] >> 24) != 0;
    if (!g_selmask) return TRUE;
    return g_selmask[(size_t)(y - g_selrc.top) * (g_selrc.right - g_selrc.left) + (x - g_selrc.left)] != 0;
}

void sel_clear(void)
{
    img_free(&g_float);
    free(g_selmask); g_selmask = NULL;
    g_sel = 0;
    SetRectEmpty(&g_selrc);
    if (g_canvas) InvalidateRect(g_canvas, NULL, FALSE);
}

static void lift(void)
{
    int x, y, w = g_selrc.right - g_selrc.left, h = g_selrc.bottom - g_selrc.top;
    DWORD bg = rgb2px(g_color2), key = bg & 0xFFFFFF;
    if (g_float.px || !g_sel || w < 1 || h < 1) return;
    if (!img_alloc(&g_float, w, h, 0)) return;
    undo_push();
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            int ix = g_selrc.left + x, iy = g_selrc.top + y;
            BOOL m = g_selmask ? g_selmask[(size_t)y * w + x] != 0 : TRUE;
            DWORD v = 0;
            if (m && ix >= 0 && iy >= 0 && ix < g_img.w && iy < g_img.h)
            {
                DWORD *p = g_img.px + (size_t)iy * g_img.w + ix;
                v = *p & 0xFFFFFF;
                *p = bg;
                if (!(g_transparent_sel && v == key)) v |= 0xFF000000u;
            }
            g_float.px[(size_t)y * w + x] = v;
        }
    free(g_selmask); g_selmask = NULL;
    set_dirty();
}

void sel_commit(void)
{
    int x, y;
    if (g_float.px)
    {
        for (y = 0; y < g_float.h; y++)
            for (x = 0; x < g_float.w; x++)
            {
                DWORD v = g_float.px[(size_t)y * g_float.w + x];
                int a = v >> 24;
                if (a == 255) put(g_selrc.left + x, g_selrc.top + y, v);
                else if (a) { BYTE *c = (BYTE *)g_cover; g_cover = NULL; blend(g_selrc.left + x, g_selrc.top + y, v, a); g_cover = c; }
            }
        set_dirty();
    }
    sel_clear();
    InvalidateRect(g_status, NULL, FALSE);
}

void sel_all(void)
{
    sel_commit();
    g_tool = T_SELECT;
    g_sel = 1;
    SetRect(&g_selrc, 0, 0, g_img.w, g_img.h);
    ribbon_layout();
    InvalidateRect(g_canvas, NULL, FALSE);
}

void sel_invert(void)
{
    BYTE *m;
    int x, y;
    if (!g_sel) { sel_all(); return; }
    if (g_float.px)
    {
        /* put the lifted pixels down, keeping their shape as the selection */
        RECT r = g_selrc;
        BYTE *mm;
        int w = r.right - r.left, h = r.bottom - r.top;
        if (!(mm = malloc((size_t)w * h))) return;
        for (y = 0; y < h; y++) for (x = 0; x < w; x++) mm[(size_t)y * w + x] = (g_float.px[(size_t)y * w + x] >> 24) != 0;
        sel_commit();
        g_sel = 1; g_selrc = r; g_selmask = mm;
    }
    if (!(m = malloc((size_t)g_img.w * g_img.h))) return;
    for (y = 0; y < g_img.h; y++) for (x = 0; x < g_img.w; x++) m[(size_t)y * g_img.w + x] = !in_mask(x, y);
    free(g_selmask);
    g_selmask = m;
    SetRect(&g_selrc, 0, 0, g_img.w, g_img.h);
    g_tool = T_FREESEL;
    ribbon_layout();
    InvalidateRect(g_canvas, NULL, FALSE);
}

void sel_delete(void)
{
    int x, y;
    if (!g_sel) return;
    if (g_float.px) { img_free(&g_float); }
    else
    {
        DWORD bg = rgb2px(g_color2);
        undo_push();
        for (y = max(0, (int)g_selrc.top); y < min(g_img.h, (int)g_selrc.bottom); y++)
            for (x = max(0, (int)g_selrc.left); x < min(g_img.w, (int)g_selrc.right); x++)
                if (in_mask(x, y)) put(x, y, bg);
    }
    sel_clear();
    set_dirty();
    canvas_changed(FALSE);
}

/* the selection as a picture of its own (what is outside a free-form shape is white) */
static BOOL sel_picture(Img *out)
{
    int x, y, w = g_selrc.right - g_selrc.left, h = g_selrc.bottom - g_selrc.top;
    if (!g_sel || w < 1 || h < 1 || !img_alloc(out, w, h, RGB(255, 255, 255))) return FALSE;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            int ix = g_selrc.left + x, iy = g_selrc.top + y;
            if (g_float.px) { DWORD v = g_float.px[(size_t)y * w + x]; if (v >> 24) out->px[(size_t)y * w + x] = v | 0xFF000000u; }
            else if (in_mask(ix, iy) && ix >= 0 && iy >= 0 && ix < g_img.w && iy < g_img.h)
                out->px[(size_t)y * w + x] = g_img.px[(size_t)iy * g_img.w + ix] | 0xFF000000u;
        }
    return TRUE;
}

BOOL sel_copy(void)
{
    Img p;
    BOOL ok;
    if (!sel_picture(&p)) return FALSE;
    ok = clip_put(&p);
    img_free(&p);
    return ok;
}

void sel_paste(const Img *im)
{
    POINT at;
    int x, y;
    sel_commit();
    undo_push();
    if (im->w > g_img.w || im->h > g_img.h)
    {
        /* Paint grows the picture to take a larger paste */
        Img n;
        int w = max(g_img.w, im->w), h = max(g_img.h, im->h);
        if (img_alloc(&n, w, h, g_color2))
        {
            for (y = 0; y < g_img.h; y++) memcpy(n.px + (size_t)y * w, g_img.px + (size_t)y * g_img.w, g_img.w * 4);
            img_install(&n);
        }
    }
    if (!img_copy(&g_float, im)) { undo_drop_top(); return; }
    for (x = 0; x < g_float.w * g_float.h; x++)
    {
        DWORD v = g_float.px[x] & 0xFFFFFF;
        g_float.px[x] = (g_transparent_sel && v == (rgb2px(g_color2) & 0xFFFFFF)) ? v : v | 0xFF000000u;
    }
    at = to_img(0, 0);
    at.x = max(0, min((int)at.x, g_img.w - 1)); at.y = max(0, min((int)at.y, g_img.h - 1));
    if (at.x + im->w > g_img.w) at.x = max(0, g_img.w - im->w);
    if (at.y + im->h > g_img.h) at.y = max(0, g_img.h - im->h);
    SetRect(&g_selrc, at.x, at.y, at.x + im->w, at.y + im->h);
    g_sel = 1;
    g_tool = T_SELECT;
    set_dirty();
    ribbon_layout();
    canvas_changed(TRUE);
}

void sel_crop(void)
{
    Img p;
    if (!g_sel) return;
    if (!g_float.px) undo_push();
    if (!sel_picture(&p)) return;
    sel_clear();
    img_install(&p);
    set_dirty();
    canvas_changed(TRUE);
}

void sel_transform(int op)
{
    lift();
    if (!g_float.px) return;
    switch (op)
    {
    case CMD_ROT_R: img_rotate(&g_float, 1); break;
    case CMD_ROT_L: img_rotate(&g_float, 3); break;
    case CMD_ROT_180: img_rotate(&g_float, 2); break;
    case CMD_FLIP_V: img_flip(&g_float, TRUE); break;
    case CMD_FLIP_H: img_flip(&g_float, FALSE); break;
    }
    g_selrc.right = g_selrc.left + g_float.w;
    g_selrc.bottom = g_selrc.top + g_float.h;
    canvas_changed(FALSE);
}

BOOL sel_resize(int w, int h, int hsk, int vsk)
{
    lift();
    if (!g_float.px) return FALSE;
    img_scale(&g_float, w, h);
    if (hsk || vsk) img_skew(&g_float, hsk, vsk, RGB(0, 0, 0));
    g_selrc.right = g_selrc.left + g_float.w;
    g_selrc.bottom = g_selrc.top + g_float.h;
    set_dirty();
    canvas_changed(FALSE);
    return TRUE;
}

static void freesel_finish(void)
{
    int i, w, h, x, y;
    RECT b = { 0x7FFFFFFF, 0x7FFFFFFF, -0x7FFFFFFF, -0x7FFFFFFF };
    HDC dc;
    HBITMAP bm;
    DWORD *bits;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), 0, 0, 1, 32, BI_RGB, 0, 0, 0, 0, 0 } };
    if (g_npath < 3) { sel_clear(); return; }
    for (i = 0; i < g_npath; i++)
    {
        b.left = min(b.left, g_fpath[i].x); b.top = min(b.top, g_fpath[i].y);
        b.right = max(b.right, g_fpath[i].x + 1); b.bottom = max(b.bottom, g_fpath[i].y + 1);
    }
    b.left = max(0L, b.left); b.top = max(0L, b.top); b.right = min((LONG)g_img.w, b.right); b.bottom = min((LONG)g_img.h, b.bottom);
    w = b.right - b.left; h = b.bottom - b.top;
    if (w < 1 || h < 1) { sel_clear(); return; }
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    bm = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    dc = CreateCompatibleDC(NULL);
    SelectObject(dc, bm);
    PatBlt(dc, 0, 0, w, h, BLACKNESS);
    for (i = 0; i < g_npath; i++) { g_fpath[i].x -= b.left; g_fpath[i].y -= b.top; }
    SelectObject(dc, GetStockObject(WHITE_BRUSH));
    SelectObject(dc, GetStockObject(WHITE_PEN));
    Polygon(dc, g_fpath, g_npath);
    GdiFlush();
    free(g_selmask);
    g_selmask = malloc((size_t)w * h);
    if (g_selmask) for (y = 0; y < h; y++) for (x = 0; x < w; x++) g_selmask[(size_t)y * w + x] = (bits[(size_t)y * w + x] & 0xFFFFFF) != 0;
    DeleteDC(dc); DeleteObject(bm);
    g_selrc = b;
    g_sel = g_selmask != NULL;
}

/* ---- text ---- */
static void text_place(void)
{
    LOGFONTW lf;
    if (!g_edit) return;
    MoveWindow(g_edit, to_cx(g_textrc.left), to_cy(g_textrc.top),
               to_cx(g_textrc.right) - to_cx(g_textrc.left), to_cy(g_textrc.bottom) - to_cy(g_textrc.top), TRUE);
    lf = g_text_font;
    lf.lfHeight = (LONG)((long long)g_text_font.lfHeight * g_zoom / 1000);
    if (g_editfont) DeleteObject(g_editfont);
    g_editfont = CreateFontIndirectW(&lf);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_editfont, TRUE);
}

static void text_begin(RECT r)
{
    if (r.right - r.left < 8) r.right = r.left + 200;
    if (r.bottom - r.top < 8) r.bottom = r.top + max(24, (int)(-g_text_font.lfHeight * 2));
    g_textrc = r;
    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                             0, 0, 0, 0, g_canvas, (HMENU)1, g_inst, NULL);
    text_place();
    SetFocus(g_edit);
    ribbon_layout();
    write_dump();
}

void text_cancel(void)
{
    if (!g_edit) return;
    DestroyWindow(g_edit);
    g_edit = NULL;
    SetFocus(g_canvas);
    ribbon_layout();
    InvalidateRect(g_canvas, NULL, FALSE);
}

void text_commit(void)
{
    int n;
    WCHAR *s;
    if (!g_edit) return;
    n = GetWindowTextLengthW(g_edit);
    if (n > 0 && (s = malloc((n + 1) * sizeof(WCHAR))))
    {
        HFONT f = CreateFontIndirectW(&g_text_font);
        HGDIOBJ of = SelectObject(g_imgdc, f);
        RECT r = g_textrc;
        GetWindowTextW(g_edit, s, n + 1);
        undo_push();
        if (g_text_opaque) { HBRUSH b = CreateSolidBrush(g_color2); FillRect(g_imgdc, &r, b); DeleteObject(b); }
        SetBkMode(g_imgdc, TRANSPARENT);
        SetTextColor(g_imgdc, g_color1);
        DrawTextW(g_imgdc, s, n, &r, DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        GdiFlush();
        /* GDI writes zero alpha; the picture is opaque */
        {
            int x, y;
            for (y = max(0, (int)r.top); y < min(g_img.h, (int)r.bottom); y++)
                for (x = max(0, (int)r.left); x < min(g_img.w, (int)r.right); x++) g_img.px[(size_t)y * g_img.w + x] |= 0xFF000000u;
        }
        SelectObject(g_imgdc, of); DeleteObject(f);
        free(s);
        set_dirty();
    }
    text_cancel();
}

void canvas_escape(void)
{
    if (g_edit) { text_commit(); return; }
    if (g_mode == M_SHAPE && undo_top())
    {
        Img c;
        if (img_copy(&c, undo_top())) { undo_drop_top(); img_install(&c); }
        ReleaseCapture();
        g_mode = M_NONE;
        canvas_changed(FALSE);
        return;
    }
    sel_commit();
    canvas_changed(FALSE);
}

/* ---- painting ---- */
static void dashed(HDC dc, RECT r)
{
    HPEN white = CreatePen(PS_SOLID, 1, RGB(255, 255, 255)), dot = CreatePen(PS_DOT, 1, RGB(0, 0, 0));
    HGDIOBJ op = SelectObject(dc, white), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, dot);
    SetBkMode(dc, TRANSPARENT);
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(white); DeleteObject(dot);
}

static RECT handle_rect(int which)
{
    RECT r;
    int s = S(5), x = to_cx(g_img.w), y = to_cy(g_img.h);
    switch (which)
    {
    case H_RIGHT: SetRect(&r, x, (to_cy(0) + y) / 2 - s / 2, x + s, (to_cy(0) + y) / 2 - s / 2 + s); break;
    case H_BOTTOM: SetRect(&r, (to_cx(0) + x) / 2 - s / 2, y, (to_cx(0) + x) / 2 - s / 2 + s, y + s); break;
    default: SetRect(&r, x, y, x + s, y + s); break;
    }
    return r;
}

static void paint(HDC dc, RECT rc)
{
    HBRUSH ws = CreateSolidBrush(WORKSPACE), sh = CreateSolidBrush((sgm_dark ? RGB(10, 10, 12) : RGB(188, 184, 200)));
    int x0 = to_cx(0), y0 = to_cy(0), w = vw(), h = vh(), i;
    RECT r;
    HDC src = g_imgdc;
    HBITMAP comp = NULL;
    HDC cdc = NULL;
    FillRect(dc, &rc, ws);
    SetRect(&r, x0 + S(2), y0 + S(2), x0 + w + S(2), y0 + h + S(2));
    FillRect(dc, &r, sh);
    DeleteObject(ws); DeleteObject(sh);

    if (g_float.px)
    {
        /* the lifted selection over the picture, composed for showing */
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), g_img.w, -g_img.h, 1, 32, BI_RGB, 0, 0, 0, 0, 0 } };
        DWORD *bits;
        comp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
        if (comp)
        {
            int x, y;
            memcpy(bits, g_img.px, (size_t)g_img.w * g_img.h * 4);
            for (y = 0; y < g_float.h; y++)
                for (x = 0; x < g_float.w; x++)
                {
                    int ix = g_selrc.left + x, iy = g_selrc.top + y, a;
                    DWORD v = g_float.px[(size_t)y * g_float.w + x], *d, o;
                    if (ix < 0 || iy < 0 || ix >= g_img.w || iy >= g_img.h || !(a = v >> 24)) continue;
                    d = bits + (size_t)iy * g_img.w + ix; o = *d;
                    *d = (((((v >> 16) & 0xFF) * a + ((o >> 16) & 0xFF) * (255 - a)) / 255) << 16) |
                         (((((v >> 8) & 0xFF) * a + ((o >> 8) & 0xFF) * (255 - a)) / 255) << 8) |
                         (((v & 0xFF) * a + (o & 0xFF) * (255 - a)) / 255);
                }
            cdc = CreateCompatibleDC(dc);
            SelectObject(cdc, comp);
            src = cdc;
        }
    }
    if (g_zoom >= 1000)
    {
        /* only what shows, at a whole-pixel scale */
        int z = g_zoom / 1000;
        int ix0 = max(0, (rc.left - x0) / z), iy0 = max(0, (rc.top - y0) / z);
        int ix1 = min(g_img.w, (rc.right - x0) / z + 1), iy1 = min(g_img.h, (rc.bottom - y0) / z + 1);
        SetStretchBltMode(dc, COLORONCOLOR);
        if (ix1 > ix0 && iy1 > iy0)
            StretchBlt(dc, x0 + ix0 * z, y0 + iy0 * z, (ix1 - ix0) * z, (iy1 - iy0) * z, src, ix0, iy0, ix1 - ix0, iy1 - iy0, SRCCOPY);
        if (g_grid && z >= 4)
        {
            HPEN gp = CreatePen(PS_SOLID, 1, RGB(200, 200, 205));
            HGDIOBJ op = SelectObject(dc, gp);
            for (i = ix0; i <= ix1; i++) { MoveToEx(dc, x0 + i * z, y0 + iy0 * z, NULL); LineTo(dc, x0 + i * z, y0 + iy1 * z); }
            for (i = iy0; i <= iy1; i++) { MoveToEx(dc, x0 + ix0 * z, y0 + i * z, NULL); LineTo(dc, x0 + ix1 * z, y0 + i * z); }
            SelectObject(dc, op); DeleteObject(gp);
        }
    }
    else
    {
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchBlt(dc, x0, y0, w, h, src, 0, 0, g_img.w, g_img.h, SRCCOPY);
    }
    if (cdc) DeleteDC(cdc);
    if (comp) DeleteObject(comp);

    /* the canvas's handles */
    for (i = H_RIGHT; i <= H_CORNER; i++)
    {
        RECT hr = handle_rect(i);
        HBRUSH wb = CreateSolidBrush(RGB(255, 255, 255)), eb = CreateSolidBrush(RGB(120, 120, 130));
        FillRect(dc, &hr, wb); FrameRect(dc, &hr, eb);
        DeleteObject(wb); DeleteObject(eb);
    }
    if (g_mode == M_RESIZE)
    {
        RECT nr = { x0, y0, x0 + (int)((long long)g_newsize_w * g_zoom / 1000), y0 + (int)((long long)g_newsize_h * g_zoom / 1000) };
        dashed(dc, nr);
    }
    if (g_mode == M_FREESEL && g_npath > 1)
    {
        HPEN p = CreatePen(PS_SOLID, 1, RGB(0, 0, 0));
        HGDIOBJ op = SelectObject(dc, p);
        MoveToEx(dc, to_cx(g_fpath[0].x), to_cy(g_fpath[0].y), NULL);
        for (i = 1; i < g_npath; i++) LineTo(dc, to_cx(g_fpath[i].x), to_cy(g_fpath[i].y));
        SelectObject(dc, op); DeleteObject(p);
    }
    if (g_sel || g_mode == M_SELECTING || g_mode == M_TEXTBOX)
    {
        RECT s = g_mode == M_TEXTBOX ? g_textrc : g_selrc;
        RECT v = { to_cx(s.left) - 1, to_cy(s.top) - 1, to_cx(s.right) + 1, to_cy(s.bottom) + 1 };
        if (s.right > s.left && s.bottom > s.top) dashed(dc, v);
    }
    if (g_edit)
    {
        RECT v = { to_cx(g_textrc.left) - 1, to_cy(g_textrc.top) - 1, to_cx(g_textrc.right) + 1, to_cy(g_textrc.bottom) + 1 };
        dashed(dc, v);
    }
}

static int hit_handle(int cx, int cy)
{
    int i;
    for (i = H_CORNER; i >= H_RIGHT; i--)
    {
        RECT r = handle_rect(i);
        InflateRect(&r, S(3), S(3));
        if (cx >= r.left && cx < r.right && cy >= r.top && cy < r.bottom) return i;
    }
    return H_NONE;
}

static void path_add(POINT p)
{
    if (g_npath == g_cappath)
    {
        POINT *n = realloc(g_fpath, (g_cappath ? g_cappath * 2 : 256) * sizeof(POINT));
        if (!n) return;
        g_fpath = n; g_cappath = g_cappath ? g_cappath * 2 : 256;
    }
    p.x = max(0, min((int)p.x, g_img.w - 1)); p.y = max(0, min((int)p.y, g_img.h - 1));
    g_fpath[g_npath++] = p;
}

static void norm_sel(void)
{
    g_selrc.left = max(0, min((int)min(g_start.x, g_cur.x), g_img.w));
    g_selrc.top = max(0, min((int)min(g_start.y, g_cur.y), g_img.h));
    g_selrc.right = max(0, min((int)max(g_start.x, g_cur.x) + 1, g_img.w));
    g_selrc.bottom = max(0, min((int)max(g_start.y, g_cur.y) + 1, g_img.h));
}

static void button_down(HWND hwnd, int button, int cx, int cy, WPARAM keys)
{
    POINT p = to_img(cx, cy);
    (void)keys;
    if (g_mode != M_NONE) return;
    if (g_edit && g_tool == T_TEXT)
    {
        /* a click outside the text box puts the text down */
        text_commit();
        return;
    }
    SetFocus(hwnd);
    g_button = button;
    g_start = g_last = g_cur = p;
    if ((g_handle = hit_handle(cx, cy)) != H_NONE && button == 1)
    {
        text_commit();
        sel_commit();
        g_mode = M_RESIZE; g_newsize_w = g_img.w; g_newsize_h = g_img.h;
        SetCapture(hwnd);
        return;
    }
    switch (g_tool)
    {
    case T_PENCIL: case T_BRUSH: case T_ERASER:
        undo_push();
        if (g_tool == T_BRUSH && g_brush == B_MARKER) g_cover = calloc((size_t)g_img.w * g_img.h, 1);
        stamp(p.x, p.y, button);
        if (g_tool == T_BRUSH && g_brush == B_AIRBRUSH) SetTimer(hwnd, 1, 40, NULL);
        g_mode = M_DRAW;
        set_dirty();
        break;
    case T_FILL:
        undo_push();
        flood_fill(p.x, p.y, button == 1 ? g_color1 : g_color2);
        set_dirty();
        break;
    case T_PICKER:
        if (p.x >= 0 && p.y >= 0 && p.x < g_img.w && p.y < g_img.h)
        {
            COLORREF c = px2rgb(g_img.px[(size_t)p.y * g_img.w + p.x]);
            if (button == 1) g_color1 = c; else g_color2 = c;
        }
        set_tool(g_prev_tool == T_PICKER ? T_PENCIL : g_prev_tool);
        InvalidateRect(g_ribbon, NULL, FALSE);
        break;
    case T_MAGNIFIER:
        canvas_zoom(zoom_step(button == 1 ? 1 : -1), cx, cy);
        break;
    case T_TEXT:
        g_mode = M_TEXTBOX;
        SetRect(&g_textrc, p.x, p.y, p.x, p.y);
        break;
    case T_SELECT: case T_FREESEL:
        if (g_sel && in_mask(p.x, p.y))
        {
            lift();
            g_mode = M_MOVING;
            g_move_org.x = g_selrc.left; g_move_org.y = g_selrc.top;
            break;
        }
        sel_commit();
        if (g_tool == T_SELECT) { g_mode = M_SELECTING; SetRectEmpty(&g_selrc); }
        else { g_mode = M_FREESEL; g_npath = 0; path_add(p); }
        break;
    case T_SHAPE:
        undo_push();
        g_mode = M_SHAPE;
        break;
    }
    if (g_mode != M_NONE) SetCapture(hwnd);
    canvas_changed(FALSE);
}

static void mouse_move(HWND hwnd, int cx, int cy, WPARAM keys)
{
    POINT p = to_img(cx, cy);
    RECT rc;
    GetClientRect(hwnd, &rc);
    g_cur = p;
    g_mouse_in = p.x >= 0 && p.y >= 0 && p.x < g_img.w && p.y < g_img.h;
    InvalidateRect(g_status, NULL, FALSE);
    switch (g_mode)
    {
    case M_DRAW:
        stroke(g_last, p, g_button);
        g_last = p;
        break;
    case M_SHAPE:
    {
        Img c;
        if (undo_top() && img_copy(&c, undo_top()))
        {
            memcpy(g_img.px, c.px, (size_t)c.w * c.h * 4);
            img_free(&c);
        }
        draw_shape(g_start, p, g_button, (keys & MK_SHIFT) != 0);
        break;
    }
    case M_SELECTING: norm_sel(); break;
    case M_FREESEL: path_add(p); break;
    case M_MOVING:
    {
        int w = g_selrc.right - g_selrc.left, h = g_selrc.bottom - g_selrc.top;
        g_selrc.left = g_move_org.x + p.x - g_start.x;
        g_selrc.top = g_move_org.y + p.y - g_start.y;
        g_selrc.right = g_selrc.left + w; g_selrc.bottom = g_selrc.top + h;
        set_dirty();
        break;
    }
    case M_RESIZE:
    {
        POINT q = to_img(cx, cy);
        if (g_handle != H_BOTTOM) g_newsize_w = max(1, (int)q.x);
        if (g_handle != H_RIGHT) g_newsize_h = max(1, (int)q.y);
        break;
    }
    case M_TEXTBOX:
        SetRect(&g_textrc, min(g_start.x, p.x), min(g_start.y, p.y), max(g_start.x, p.x), max(g_start.y, p.y));
        break;
    default: return;
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

static void button_up(HWND hwnd, int button)
{
    if (g_mode == M_NONE || button != g_button) return;
    ReleaseCapture();
    KillTimer(hwnd, 1);
    switch (g_mode)
    {
    case M_DRAW: free(g_cover); g_cover = NULL; break;
    case M_SHAPE:
        if (g_start.x == g_cur.x && g_start.y == g_cur.y) undo_drop_top();
        else set_dirty();
        break;
    case M_SELECTING:
        norm_sel();
        g_sel = g_selrc.right - g_selrc.left > 0 && g_selrc.bottom - g_selrc.top > 0 &&
                !(g_start.x == g_cur.x && g_start.y == g_cur.y);
        if (!g_sel) SetRectEmpty(&g_selrc);
        break;
    case M_FREESEL: freesel_finish(); break;
    case M_RESIZE: g_mode = M_NONE; canvas_set_size(g_newsize_w, g_newsize_h); break;
    case M_TEXTBOX: g_mode = M_NONE; text_begin(g_textrc); break;
    }
    g_mode = M_NONE;
    canvas_changed(FALSE);
    write_dump();
}

static void scroll(HWND hwnd, int bar, int code)
{
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    int *pos = bar == SB_HORZ ? &g_scx : &g_scy;
    GetScrollInfo(hwnd, bar, &si);
    switch (code)
    {
    case SB_LINEUP: *pos -= S(20); break;
    case SB_LINEDOWN: *pos += S(20); break;
    case SB_PAGEUP: *pos -= si.nPage; break;
    case SB_PAGEDOWN: *pos += si.nPage; break;
    case SB_THUMBTRACK: case SB_THUMBPOSITION: *pos = si.nTrackPos; break;
    case SB_TOP: *pos = 0; break;
    case SB_BOTTOM: *pos = si.nMax; break;
    default: return;
    }
    update_scroll();
    text_place();
    InvalidateRect(hwnd, NULL, FALSE);
}

LRESULT CALLBACK canvas_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(hwnd, &ps), mem;
        HBITMAP bb;
        GetClientRect(hwnd, &rc);
        mem = CreateCompatibleDC(dc);
        bb = CreateCompatibleBitmap(dc, max(1, (int)rc.right), max(1, (int)rc.bottom));
        SelectObject(mem, bb);
        paint(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        DeleteDC(mem); DeleteObject(bb);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: update_scroll(); InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_HSCROLL: scroll(hwnd, SB_HORZ, LOWORD(wp)); return 0;
    case WM_VSCROLL: scroll(hwnd, SB_VERT, LOWORD(wp)); return 0;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        ScreenToClient(hwnd, &p);
        if (msg == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL))
            canvas_zoom(zoom_step(d > 0 ? 1 : -1), p.x, p.y);
        else
        {
            BOOL h = msg == WM_MOUSEHWHEEL || (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT);
            int *pos = h ? &g_scx : &g_scy;
            *pos += (msg == WM_MOUSEHWHEEL ? d : -d) * S(60) / WHEEL_DELTA;
            update_scroll(); text_place(); InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: button_down(hwnd, 1, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp); return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: button_down(hwnd, 2, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp); return 0;
    case WM_MOUSEMOVE:
        if (!g_mouse_in) { TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 }; TrackMouseEvent(&t); }
        mouse_move(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp);
        return 0;
    case WM_MOUSELEAVE: g_mouse_in = 0; InvalidateRect(g_status, NULL, FALSE); return 0;
    case WM_LBUTTONUP: button_up(hwnd, 1); return 0;
    case WM_RBUTTONUP: button_up(hwnd, 2); return 0;
    case WM_CAPTURECHANGED:
        if (g_mode != M_NONE && (HWND)lp != hwnd) { int b = g_button; button_up(hwnd, b); }
        return 0;
    case WM_TIMER:
        if (g_mode == M_DRAW) { stamp(g_cur.x, g_cur.y, g_button); InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT)
        {
            POINT p;
            LPCWSTR c = (LPCWSTR)IDC_CROSS;
            int h;
            GetCursorPos(&p); ScreenToClient(hwnd, &p);
            h = g_mode == M_RESIZE ? g_handle : hit_handle(p.x, p.y);
            if (h == H_RIGHT) c = (LPCWSTR)IDC_SIZEWE;
            else if (h == H_BOTTOM) c = (LPCWSTR)IDC_SIZENS;
            else if (h == H_CORNER) c = (LPCWSTR)IDC_SIZENWSE;
            else if (g_tool == T_TEXT) c = (LPCWSTR)IDC_IBEAM;
            else if ((g_tool == T_SELECT || g_tool == T_FREESEL) && g_sel)
            {
                POINT q = to_img(p.x, p.y);
                if (in_mask(q.x, q.y)) c = (LPCWSTR)IDC_SIZEALL;
            }
            else if (g_tool == T_FILL || g_tool == T_PICKER || g_tool == T_MAGNIFIER) c = (LPCWSTR)IDC_HAND;
            SetCursor(LoadCursorW(NULL, c));
            return TRUE;
        }
        break;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, g_color1);
        SetBkColor((HDC)wp, g_text_opaque ? g_color2 : RGB(255, 255, 255));
        if (g_editbrush) DeleteObject(g_editbrush);
        g_editbrush = CreateSolidBrush(g_text_opaque ? g_color2 : RGB(255, 255, 255));
        return (LRESULT)g_editbrush;
    case WM_COMMAND:
        if (LOWORD(wp) == 1 && HIWORD(wp) == EN_CHANGE && g_edit)
        {
            /* grow the box downwards as lines are added */
            int lines = (int)SendMessageW(g_edit, EM_GETLINECOUNT, 0, 0);
            int need = lines * (int)(-g_text_font.lfHeight * 1.35) + 4;
            if (need > g_textrc.bottom - g_textrc.top) { g_textrc.bottom = g_textrc.top + need; text_place(); InvalidateRect(hwnd, NULL, FALSE); }
            write_dump();
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
