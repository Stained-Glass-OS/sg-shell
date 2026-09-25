/* sg-paint -- the ribbon's pictures, drawn here (no borrowed artwork).
 *
 * As the Control Panel's icons: plain GDI on a 64-unit grid at four times the
 * size over a key colour, box-filtered down to premultiplied alpha, cached.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "paint.h"

#define SS 4
#define KEY RGB(0xFF, 0x00, 0xFF)
#define INK RGB(56, 56, 66)
#define PURPLE ACCENT
#define LILAC RGB(180, 150, 230)
#define PAPER RGB(255, 255, 255)
#define WOOD RGB(232, 190, 120)

static int g_n;
static int P(int v) { return v * g_n / 64; }

static void fill_poly(HDC dc, COLORREF c, const POINT *src, int n)
{
    POINT pts[64];
    int i;
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
    for (i = 0; i < n && i < 64; i++) { pts[i].x = P(src[i].x); pts[i].y = P(src[i].y); }
    Polygon(dc, pts, n);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br);
}

static void poly(HDC dc, COLORREF c, const int *xy, int n)
{
    POINT pts[32];
    int i;
    for (i = 0; i < n && i < 32; i++) { pts[i].x = xy[2 * i]; pts[i].y = xy[2 * i + 1]; }
    fill_poly(dc, c, pts, n);
}

static HPEN pen(COLORREF c, int w)
{
    LOGBRUSH lb = { BS_SOLID, c, 0 };
    return ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, P(w) > 0 ? P(w) : 1, &lb, 0, NULL);
}

static void lines(HDC dc, COLORREF c, int w, const int *xy, int n)
{
    HPEN p = pen(c, w);
    HGDIOBJ op = SelectObject(dc, p);
    int i;
    MoveToEx(dc, P(xy[0]), P(xy[1]), NULL);
    for (i = 1; i < n; i++) LineTo(dc, P(xy[2 * i]), P(xy[2 * i + 1]));
    SelectObject(dc, op); DeleteObject(p);
}

static void outline_poly(HDC dc, COLORREF c, int w, const POINT *src, int n)
{
    POINT pts[64];
    int i;
    HPEN p = pen(c, w);
    HGDIOBJ op = SelectObject(dc, p), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    for (i = 0; i < n && i < 64; i++) { pts[i].x = P(src[i].x); pts[i].y = P(src[i].y); }
    Polygon(dc, pts, n);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(p);
}

static void rect(HDC dc, COLORREF fill, COLORREF edge, int w, int l, int t, int r, int b, int round)
{
    HBRUSH br = fill == CLR_NONE ? GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN p = edge == CLR_NONE ? GetStockObject(NULL_PEN) : pen(edge, w);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, p);
    if (round) RoundRect(dc, P(l), P(t), P(r), P(b), P(round), P(round));
    else Rectangle(dc, P(l), P(t), P(r), P(b));
    SelectObject(dc, ob); SelectObject(dc, op);
    if (fill != CLR_NONE) DeleteObject(br);
    if (edge != CLR_NONE) DeleteObject(p);
}

static void ellipse(HDC dc, COLORREF fill, COLORREF edge, int w, int l, int t, int r, int b)
{
    HBRUSH br = fill == CLR_NONE ? GetStockObject(NULL_BRUSH) : CreateSolidBrush(fill);
    HPEN p = edge == CLR_NONE ? GetStockObject(NULL_PEN) : pen(edge, w);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, p);
    Ellipse(dc, P(l), P(t), P(r), P(b));
    SelectObject(dc, ob); SelectObject(dc, op);
    if (fill != CLR_NONE) DeleteObject(br);
    if (edge != CLR_NONE) DeleteObject(p);
}

static void dashed_rect(HDC dc, int l, int t, int r, int b)
{
    int x, y;
    for (x = l; x < r; x += 8) { int s[4] = { x, t, min(x + 4, r), t }; int u[4] = { x, b, min(x + 4, r), b };
                                 lines(dc, INK, 3, s, 2); lines(dc, INK, 3, u, 2); }
    for (y = t; y < b; y += 8) { int s[4] = { l, y, l, min(y + 4, b) }; int u[4] = { r, y, r, min(y + 4, b) };
                                 lines(dc, INK, 3, s, 2); lines(dc, INK, 3, u, 2); }
}

/* the polygonal shapes, fitted to r (also used to draw them into pictures) */
void shape_points(int shape, RECT r, POINT *pts, int *n)
{
    double l = r.left, t = r.top, w = r.right - r.left, h = r.bottom - r.top;
    int i;
#define PT(fx, fy) do { pts[*n].x = (LONG)floor(l + (fx) * w + 0.5); pts[*n].y = (LONG)floor(t + (fy) * h + 0.5); (*n)++; } while (0)
    *n = 0;
    switch (shape)
    {
    case S_TRIANGLE: PT(0.5, 0); PT(1, 1); PT(0, 1); break;
    case S_RTRIANGLE: PT(0, 0); PT(1, 1); PT(0, 1); break;
    case S_DIAMOND: PT(0.5, 0); PT(1, 0.5); PT(0.5, 1); PT(0, 0.5); break;
    case S_PENTAGON:
        for (i = 0; i < 5; i++) { double a = -M_PI / 2 + i * 2 * M_PI / 5; PT(0.5 + 0.5 * cos(a), 0.5 + 0.5 * sin(a) * 1.05 + 0.02); }
        break;
    case S_HEXAGON:
        PT(0.25, 0); PT(0.75, 0); PT(1, 0.5); PT(0.75, 1); PT(0.25, 1); PT(0, 0.5); break;
    case S_ARROW_R: PT(0, 0.25); PT(0.6, 0.25); PT(0.6, 0); PT(1, 0.5); PT(0.6, 1); PT(0.6, 0.75); PT(0, 0.75); break;
    case S_ARROW_L: PT(1, 0.25); PT(0.4, 0.25); PT(0.4, 0); PT(0, 0.5); PT(0.4, 1); PT(0.4, 0.75); PT(1, 0.75); break;
    case S_ARROW_U: PT(0.25, 1); PT(0.25, 0.4); PT(0, 0.4); PT(0.5, 0); PT(1, 0.4); PT(0.75, 0.4); PT(0.75, 1); break;
    case S_ARROW_D: PT(0.25, 0); PT(0.25, 0.6); PT(0, 0.6); PT(0.5, 1); PT(1, 0.6); PT(0.75, 0.6); PT(0.75, 0); break;
    case S_STAR4:
        for (i = 0; i < 8; i++) { double a = -M_PI / 2 + i * M_PI / 4, rr = (i & 1) ? 0.18 : 0.5; PT(0.5 + rr * cos(a), 0.5 + rr * sin(a)); }
        break;
    case S_STAR5:
        for (i = 0; i < 10; i++) { double a = -M_PI / 2 + i * M_PI / 5, rr = (i & 1) ? 0.2 : 0.5; PT(0.5 + rr * cos(a), 0.55 + rr * sin(a)); }
        break;
    case S_HEART:
        for (i = 0; i < 40; i++)
        {
            double a = i * 2 * M_PI / 40, x = 16 * pow(sin(a), 3),
                   y = 13 * cos(a) - 5 * cos(2 * a) - 2 * cos(3 * a) - cos(4 * a);
            PT(0.5 + x / 34.0, 0.42 - y / 30.0);
        }
        break;
    }
#undef PT
}

static void draw_shape_glyph(HDC dc, int s)
{
    RECT r = { 8, 12, 56, 52 };
    POINT pts[64];
    int n, i;
    switch (s)
    {
    case S_LINE: { int xy[] = { 10, 54, 54, 10 }; lines(dc, INK, 4, xy, 2); return; }
    case S_RECT: rect(dc, CLR_NONE, INK, 4, 8, 14, 56, 50, 0); return;
    case S_ROUNDRECT: rect(dc, CLR_NONE, INK, 4, 8, 14, 56, 50, 18); return;
    case S_ELLIPSE: ellipse(dc, CLR_NONE, INK, 4, 6, 14, 58, 50); return;
    }
    shape_points(s, r, pts, &n);
    for (i = 0; i < n; i++) (void)pts[i];
    outline_poly(dc, INK, 4, pts, n);
}

static void draw(HDC dc, int g)
{
    switch (g)
    {
    case G_PASTE:
        rect(dc, WOOD, RGB(170, 120, 60), 3, 10, 10, 50, 58, 6);
        rect(dc, RGB(120, 120, 130), CLR_NONE, 0, 20, 5, 40, 16, 4);
        rect(dc, PAPER, INK, 3, 26, 22, 58, 60, 0);
        { int a[] = { 32, 32, 52, 32 }, b[] = { 32, 40, 52, 40 }, c[] = { 32, 48, 46, 48 };
          lines(dc, LILAC, 3, a, 2); lines(dc, LILAC, 3, b, 2); lines(dc, LILAC, 3, c, 2); }
        break;
    case G_CUT:
        { int a[] = { 22, 6, 40, 40 }, b[] = { 42, 6, 24, 40 }; lines(dc, INK, 5, a, 2); lines(dc, INK, 5, b, 2); }
        ellipse(dc, CLR_NONE, PURPLE, 5, 10, 38, 28, 56);
        ellipse(dc, CLR_NONE, PURPLE, 5, 36, 38, 54, 56);
        break;
    case G_COPY:
        rect(dc, PAPER, INK, 4, 8, 6, 38, 42, 0);
        rect(dc, PAPER, PURPLE, 4, 24, 22, 56, 58, 0);
        break;
    case G_SELECT: dashed_rect(dc, 8, 10, 56, 54); break;
    case G_FREESEL:
    {
        /* a dashed loop: every other segment of a closed curve */
        static const int xy[] = { 10, 30, 14, 18, 24, 11, 36, 10, 48, 15, 55, 26, 54, 38, 46, 49, 34, 54, 22, 51, 13, 43 };
        int i, n = ARRAYSIZE(xy) / 2;
        for (i = 0; i < n; i += 2)
        {
            int s2[4] = { xy[2 * i], xy[2 * i + 1], xy[(2 * i + 2) % (2 * n)], xy[(2 * i + 3) % (2 * n)] };
            lines(dc, INK, 3, s2, 2);
        }
        break;
    }
    case G_CROP:
        { int a[] = { 18, 4, 18, 46, 60, 46 }, b[] = { 4, 18, 46, 18, 46, 60 };
          lines(dc, INK, 5, a, 3); lines(dc, PURPLE, 5, b, 3); }
        break;
    case G_RESIZE:
        rect(dc, PAPER, INK, 3, 6, 18, 40, 58, 0);
        rect(dc, CLR_NONE, PURPLE, 3, 20, 6, 58, 44, 0);
        { int a[] = { 26, 38, 44, 20 }, h[] = { 34, 20, 44, 20, 44, 30 }; lines(dc, PURPLE, 4, a, 2); lines(dc, PURPLE, 4, h, 3); }
        break;
    case G_ROTATE:
        rect(dc, PAPER, INK, 3, 22, 26, 52, 56, 0);
        { HPEN p = pen(PURPLE, 5); HGDIOBJ op = SelectObject(dc, p);
          Arc(dc, P(6), P(8), P(42), P(44), P(8), P(34), P(38), P(12)); SelectObject(dc, op); DeleteObject(p); }
        { int a[] = { 30, 4, 40, 12, 30, 20 }; lines(dc, PURPLE, 5, a, 3); }
        break;
    case G_PENCIL:
        { int body[] = { 44, 6, 58, 20, 22, 56, 8, 42 }, tip[] = { 8, 42, 22, 56, 4, 60 };
          poly(dc, RGB(247, 200, 70), body, 4); poly(dc, WOOD, tip, 3);
          { int t2[] = { 4, 60, 9, 55, 7, 58 }; poly(dc, INK, t2, 3); }
          { int e[] = { 44, 6, 58, 20 }; lines(dc, RGB(220, 120, 140), 6, e, 2); }
          { int o[] = { 44, 6, 58, 20, 22, 56, 8, 42, 44, 6 }; lines(dc, INK, 2, o, 5); } }
        break;
    case G_FILL:
        { int can[] = { 10, 30, 32, 8, 54, 30, 32, 52 }; poly(dc, PAPER, can, 4);
          { int o[] = { 10, 30, 32, 8, 54, 30, 32, 52, 10, 30 }; lines(dc, INK, 3, o, 5); }
          { int paint[] = { 10, 30, 54, 30, 32, 52 }; poly(dc, PURPLE, paint, 3); }
          ellipse(dc, PURPLE, CLR_NONE, 0, 50, 36, 60, 56); }
        break;
    case G_TEXT:
        { int a[] = { 10, 56, 32, 6, 54, 56 }, b[] = { 18, 40, 46, 40 }; lines(dc, INK, 6, a, 3); lines(dc, INK, 5, b, 2); }
        break;
    case G_ERASER:
        { int top[] = { 30, 8, 58, 30, 36, 52, 8, 30 }, bot[] = { 8, 30, 22, 41, 12, 52, 2, 42 };
          poly(dc, RGB(245, 160, 190), top, 4); (void)bot;
          { int b2[] = { 8, 30, 22, 42, 16, 48, 4, 38 }; poly(dc, PAPER, b2, 4); }
          { int o[] = { 30, 8, 58, 30, 36, 52, 8, 30, 30, 8 }; lines(dc, INK, 3, o, 5); }
          { int f[] = { 6, 58, 40, 58 }; lines(dc, INK, 3, f, 2); } }
        break;
    case G_PICKER:
        { int stem[] = { 10, 54, 36, 28 }; lines(dc, INK, 6, stem, 2);
          ellipse(dc, INK, CLR_NONE, 0, 34, 6, 58, 30);
          { int b[] = { 28, 22, 42, 36 }; lines(dc, INK, 8, b, 2); }
          ellipse(dc, PURPLE, CLR_NONE, 0, 4, 50, 14, 60); }
        break;
    case G_MAGNIFIER:
        ellipse(dc, RGB(236, 230, 250), INK, 5, 6, 6, 42, 42);
        { int h[] = { 38, 38, 56, 56 }; lines(dc, INK, 8, h, 2); }
        break;
    case G_BRUSH: case G_CALLI1: case G_CALLI2: case G_AIRBRUSH: case G_MARKER: case G_CRAYON:
        if (g == G_BRUSH)
        { int handle[] = { 58, 4, 34, 32 }; lines(dc, WOOD, 7, handle, 2);
          { int ferrule[] = { 36, 30, 28, 38 }; lines(dc, RGB(150, 150, 160), 8, ferrule, 2); }
          { int tip[] = { 30, 32, 36, 38, 22, 56, 8, 58, 14, 44 }; poly(dc, PURPLE, tip, 5); } }
        else if (g == G_AIRBRUSH)
        { rect(dc, RGB(150, 150, 160), INK, 2, 34, 22, 54, 58, 4); rect(dc, INK, CLR_NONE, 0, 38, 14, 50, 22, 0);
          { int dots[][2] = { {8,10},{16,20},{6,26},{22,8},{14,32},{24,24},{10,40},{26,34} }; int i;
            for (i = 0; i < 8; i++) ellipse(dc, PURPLE, CLR_NONE, 0, dots[i][0], dots[i][1], dots[i][0] + 5, dots[i][1] + 5); } }
        else if (g == G_MARKER)
        { int body[] = { 50, 4, 60, 14, 30, 44, 20, 34 }; poly(dc, RGB(120, 120, 130), body, 4);
          { int tip[] = { 20, 34, 30, 44, 16, 50, 14, 40 }; poly(dc, PURPLE, tip, 4); }
          { int stroke[] = { 4, 58, 26, 58 }; lines(dc, PURPLE, 6, stroke, 2); } }
        else if (g == G_CRAYON)
        { int body[] = { 52, 4, 60, 12, 24, 48, 16, 40 }; poly(dc, PURPLE, body, 4);
          { int tip[] = { 16, 40, 24, 48, 8, 56 }; poly(dc, LILAC, tip, 3); }
          { int i; for (i = 0; i < 5; i++) { int s[] = { 30 + i * 4, 56 - (i & 1) * 3, 32 + i * 4, 58 }; lines(dc, PURPLE, 2, s, 2); } } }
        else
        { int stroke1[] = { 8, 54, 36, 26 }; int nib[] = { 34, 28, 50, 6, 58, 14, 40, 34 };
          lines(dc, PURPLE, g == G_CALLI1 ? 3 : 7, stroke1, 2); poly(dc, INK, nib, 4);
          { int slash[] = { 10, 20, 24, 6 }; int back[] = { 10, 6, 24, 20 }; lines(dc, PURPLE, 4, g == G_CALLI1 ? slash : back, 2); } }
        break;
    case G_SIZE:
        { int a[] = { 8, 12, 56, 12 }, b[] = { 8, 26, 56, 26 }, c[] = { 8, 42, 56, 42 };
          lines(dc, INK, 2, a, 2); lines(dc, INK, 5, b, 2); lines(dc, INK, 9, c, 2); }
        break;
    case G_EDITCOLORS:
        ellipse(dc, RGB(236, 230, 250), INK, 3, 4, 8, 60, 56);
        ellipse(dc, RGB(220, 60, 60), CLR_NONE, 0, 14, 16, 24, 26);
        ellipse(dc, RGB(240, 190, 40), CLR_NONE, 0, 28, 13, 38, 23);
        ellipse(dc, RGB(60, 170, 90), CLR_NONE, 0, 42, 18, 52, 28);
        ellipse(dc, PURPLE, CLR_NONE, 0, 44, 32, 54, 42);
        ellipse(dc, PAPER, INK, 2, 16, 34, 30, 48);
        break;
    case G_ZOOMIN: case G_ZOOMOUT:
        ellipse(dc, PAPER, INK, 5, 6, 6, 42, 42);
        { int h[] = { 38, 38, 56, 56 }, m[] = { 16, 24, 32, 24 }, v[] = { 24, 16, 24, 32 };
          lines(dc, INK, 8, h, 2); lines(dc, PURPLE, 5, m, 2); if (g == G_ZOOMIN) lines(dc, PURPLE, 5, v, 2); }
        break;
    case G_ZOOM100:
        rect(dc, PAPER, INK, 3, 6, 10, 58, 54, 0);
        { int a[] = { 16, 20, 20, 16, 20, 46 }; lines(dc, PURPLE, 4, a, 3); }
        { int a[] = { 28, 22, 30, 30 }; (void)a; }
        ellipse(dc, CLR_NONE, PURPLE, 4, 26, 16, 36, 46);
        ellipse(dc, CLR_NONE, PURPLE, 4, 40, 16, 50, 46);
        break;
    case G_FULLSCREEN:
        rect(dc, PAPER, INK, 3, 6, 10, 58, 54, 0);
        { int a[] = { 14, 26, 14, 18, 22, 18 }, b[] = { 42, 18, 50, 18, 50, 26 }, c[] = { 50, 38, 50, 46, 42, 46 }, d[] = { 22, 46, 14, 46, 14, 38 };
          lines(dc, PURPLE, 4, a, 3); lines(dc, PURPLE, 4, b, 3); lines(dc, PURPLE, 4, c, 3); lines(dc, PURPLE, 4, d, 3); }
        break;
    case G_OUTLINE:
        rect(dc, CLR_NONE, INK, 5, 10, 12, 54, 52, 0);
        break;
    case G_FILLSHAPE:
        rect(dc, LILAC, INK, 3, 10, 12, 54, 52, 0);
        break;
    default:
        if (g >= G_SHAPE_BASE && g < G_COUNT) draw_shape_glyph(dc, g - G_SHAPE_BASE);
        break;
    }
}

static HBITMAP render(int glyph, int size)
{
    int big = size * SS, x, y;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), big, -big, 1, 32, BI_RGB, 0, 0, 0, 0, 0 } };
    DWORD *src, *dst;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bb = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&src, NULL, 0), out;
    HGDIOBJ old = SelectObject(dc, bb);
    RECT r = { 0, 0, big, big };
    HBRUSH k = CreateSolidBrush(KEY);
    FillRect(dc, &r, k); DeleteObject(k);
    g_n = big;
    draw(dc, glyph);
    GdiFlush();
    bi.bmiHeader.biWidth = size; bi.bmiHeader.biHeight = -size;
    out = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&dst, NULL, 0);
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++)
        {
            DWORD a = 0, rr = 0, gg = 0, b = 0;
            int i, j;
            for (j = 0; j < SS; j++)
                for (i = 0; i < SS; i++)
                {
                    DWORD p = src[(y * SS + j) * big + x * SS + i] & 0xFFFFFF;
                    if (p == 0xFF00FF) continue;
                    a += 255; rr += (p >> 16) & 0xFF; gg += (p >> 8) & 0xFF; b += p & 0xFF;
                }
            dst[y * size + x] = ((a / (SS * SS)) << 24) | ((rr / (SS * SS)) << 16) | ((gg / (SS * SS)) << 8) | (b / (SS * SS));
        }
    SelectObject(dc, old); DeleteObject(bb); DeleteDC(dc);
    return out;
}

void glyph_draw(HDC dc, int glyph, int x, int y, int size)
{
    static struct { int g, size; HBITMAP bmp; } cache[128];
    static int ncache;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HBITMAP bmp = NULL;
    HDC mem;
    HGDIOBJ old;
    int i;
    if (size < 4) return;
    for (i = 0; i < ncache; i++) if (cache[i].g == glyph && cache[i].size == size) { bmp = cache[i].bmp; break; }
    if (!bmp)
    {
        bmp = render(glyph, size);
        if (ncache < (int)ARRAYSIZE(cache)) { cache[ncache].g = glyph; cache[ncache].size = size; cache[ncache].bmp = bmp; ncache++; }
    }
    mem = CreateCompatibleDC(dc);
    old = SelectObject(mem, bmp);
    AlphaBlend(dc, x, y, size, size, mem, 0, 0, size, size, bf);
    SelectObject(mem, old); DeleteDC(mem);
    if (i == ncache && ncache == (int)ARRAYSIZE(cache)) DeleteObject(bmp);
}
