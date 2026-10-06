/* smooth-shapes-probe: draws each of sg-smooth.h's drop-in shapes and a glyph
 * in a region on a memory bitmap, then reports, for each, the colours along
 * its edge and where it lies, against GDI's own drawing of the same call:
 *
 *   SHAPE name colours=N gdi=M dx=.. dy=.. crisp=0|1
 *
 * colours: distinct colours in the shape's box (GDI's: 2 or 3); dx, dy: how
 * far the smooth shape's centre of ink lies from GDI's (pixels); crisp: a
 * level edge of a large rounded rectangle is exactly the pen's colour.
 * Writes the two drawings side by side to argv[1] (a .bmp) when given.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include "../src/sg-smooth.h"

#define W 200
#define H 120
static BYTE *bits[2];
static HDC mem[2];

static void make(int i)
{
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB, 0, 0, 0, 0, 0 }, { { 0, 0, 0, 0 } } };
    mem[i] = CreateCompatibleDC(0);
    SelectObject(mem[i], CreateDIBSection(0, &bi, DIB_RGB_COLORS, (void **)&bits[i], NULL, 0));
    memset(bits[i], 0xF0, W * H * 4);
}

static void glyph(HDC dc)   /* a check mark and a ring, as a glyph function draws them */
{
    HPEN p = CreatePen(PS_SOLID, 2, RGB(0x20, 0x20, 0x20)), op = SelectObject(dc, p);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    MoveToEx(dc, 152, 92, NULL); LineTo(dc, 158, 98); LineTo(dc, 170, 84);
    Ellipse(dc, 174, 82, 192, 100);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(p);
}

static void draw(HDC dc, int smooth)
{
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x30, 0x30, 0x30)), pen2 = CreatePen(PS_SOLID, 2, RGB(0x70, 0x30, 0xC0));
    HBRUSH br = CreateSolidBrush(RGB(0x70, 0x30, 0xC0));
    POINT tri[3] = { { 110, 10 }, { 140, 40 }, { 104, 46 } }, zig[4] = { { 150, 10 }, { 160, 40 }, { 172, 12 }, { 190, 44 } };
    SelectObject(dc, pen); SelectObject(dc, br);
    if (smooth) sg_ellipse(dc, 10, 10, 50, 50); else Ellipse(dc, 10, 10, 50, 50);
    SelectObject(dc, GetStockObject(NULL_PEN));
    if (smooth) sg_round_rect(dc, 60, 10, 100, 50, 16, 16); else RoundRect(dc, 60, 10, 100, 50, 16, 16);
    SelectObject(dc, pen);
    if (smooth) sg_polygon(dc, tri, 3); else Polygon(dc, tri, 3);
    SelectObject(dc, pen2);
    if (smooth) sg_polyline(dc, zig, 4); else Polyline(dc, zig, 4);
    if (smooth) sg_arc(dc, 10, 60, 50, 100, 50, 80, 10, 80); else Arc(dc, 10, 60, 50, 100, 50, 80, 10, 80);
    SelectObject(dc, pen);
    if (smooth) sg_line(dc, 60, 60, 90, 100); else { MoveToEx(dc, 60, 60, NULL); LineTo(dc, 90, 100); }
    if (smooth) sg_round_rect(dc, 96, 58, 146, 110, 12, 12); else RoundRect(dc, 96, 58, 146, 110, 12, 12);
    if (smooth) { struct sg_ss ss; HDC big = sg_ss_begin(&ss, dc, 148, 78, 48, 26, 2); glyph(big); sg_ss_end(&ss); }
    else glyph(dc);
    SelectObject(dc, GetStockObject(BLACK_PEN)); SelectObject(dc, GetStockObject(WHITE_BRUSH));
    DeleteObject(pen); DeleteObject(pen2); DeleteObject(br);
}

static void report(const char *name, int l, int t, int r, int b)
{
    int i, x, y, n[2];
    double cx[2], cy[2];
    for (i = 0; i < 2; i++)
    {
        DWORD seen[4096];
        double sw = 0, sx = 0, sy = 0;
        n[i] = 0;
        for (y = t; y < b; y++)
            for (x = l; x < r; x++)
            {
                const BYTE *p = bits[i] + (y * W + x) * 4;
                DWORD c = p[0] | p[1] << 8 | p[2] << 16;
                int k, ink = 3 * 0xF0 - (p[0] + p[1] + p[2]);
                for (k = 0; k < n[i] && seen[k] != c; k++) ;
                if (k == n[i] && n[i] < 4096) seen[n[i]++] = c;
                if (ink < 0) ink = -ink;
                sw += ink; sx += ink * x; sy += ink * y;
            }
        cx[i] = sw ? sx / sw : 0; cy[i] = sw ? sy / sw : 0;
    }
    printf("SHAPE %s colours=%d gdi=%d dx=%.2f dy=%.2f\n", name, n[1], n[0], cx[1] - cx[0], cy[1] - cy[0]);
}

int main(int argc, char **argv)
{
    int x;
    make(0); make(1);
    draw(mem[0], 0);
    draw(mem[1], 1);
    GdiFlush();
    report("ellipse", 8, 8, 53, 53);
    report("round_rect", 58, 8, 103, 53);
    report("polygon", 100, 6, 146, 50);
    report("polyline", 146, 6, 196, 50);
    report("arc", 8, 58, 53, 103);
    report("line", 58, 58, 93, 103);
    report("region", 148, 78, 196, 104);
    /* the large rounded rectangle's top edge, mid-way: GDI's pen colour, not a blend */
    {
        int crisp = 1;
        for (x = 110; x < 132; x++)
        {
            const BYTE *p = bits[1] + (58 * W + x) * 4, *q = bits[0] + (58 * W + x) * 4;
            if (p[0] != q[0] || p[1] != q[1] || p[2] != q[2]) crisp = 0;
        }
        printf("CRISP %d\n", crisp);
    }
    if (argc > 1)
    {
        FILE *f = fopen(argv[1], "wb");
        BITMAPFILEHEADER fh = { 0x4D42, 0, 0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) };
        BITMAPINFOHEADER ih = { sizeof(BITMAPINFOHEADER), 2 * W, -H, 1, 32, BI_RGB, 0, 0, 0, 0, 0 };
        int y;
        fh.bfSize = fh.bfOffBits + 2 * W * H * 4;
        if (f)
        {
            fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
            for (y = 0; y < H; y++) { fwrite(bits[0] + y * W * 4, W * 4, 1, f); fwrite(bits[1] + y * W * 4, W * 4, 1, f); }
            fclose(f);
        }
    }
    return 0;
}
