/* sg-smooth.h: drawn art with soft edges.
 *
 * GDI draws lines, ellipses and polygons without antialiasing, so glyphs
 * drawn with them (Start's rail, its power button, the user's round badge)
 * had stepped edges, worse at 150% (David 2026-10-05: "low quality"). The art
 * is drawn SG_SS times larger on black and averaged down to pixels: a pixel
 * is as opaque as the share of it drawn on, coloured by the average of what
 * was drawn there (art never draws pure black), then blended in.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_SMOOTH_H
#define SG_SMOOTH_H

#include <windows.h>
#include <string.h>

#define SG_SS 4

/* draw: the art at SG_SS times its size, into a w*SG_SS x h*SG_SS DC */
typedef void (*sg_art_fn)(HDC dc, int w, int h, const void *arg);

static inline void sg_smooth(HDC dc, int x, int y, int w, int h, sg_art_fn draw, const void *arg)
{
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w * SG_SS, -h * SG_SS, 1, 32, BI_RGB, 0, 0, 0, 0, 0 }, { { 0, 0, 0, 0 } } };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC dcs[2] = { 0 };
    HBITMAP bmps[2] = { 0 }, olds[2] = { 0 };
    BYTE *bits[2] = { 0 };
    int i, px, py, sx, sy, n = SG_SS * SG_SS, W = w * SG_SS;

    if (w <= 0 || h <= 0) return;
    for (i = 0; i < 2; i++) {
        if (i == 1) { bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; }
        dcs[i] = CreateCompatibleDC(dc);
        bmps[i] = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits[i], NULL, 0);
        if (!dcs[i] || !bmps[i]) goto done;
        olds[i] = SelectObject(dcs[i], bmps[i]);
        memset(bits[i], 0, (size_t)bi.bmiHeader.biWidth * -bi.bmiHeader.biHeight * 4);
    }
    draw(dcs[0], w * SG_SS, h * SG_SS, arg);
    GdiFlush();
    for (py = 0; py < h; py++)
        for (px = 0; px < w; px++) {
            unsigned int c[3] = { 0, 0, 0 }, a = 0;
            BYTE *out = bits[1] + (py * w + px) * 4;
            for (sy = 0; sy < SG_SS; sy++)
                for (sx = 0; sx < SG_SS; sx++) {
                    const BYTE *p = bits[0] + ((py * SG_SS + sy) * W + px * SG_SS + sx) * 4;
                    c[0] += p[0]; c[1] += p[1]; c[2] += p[2];
                    if (p[0] | p[1] | p[2]) a += 255;
                }
            a /= n;   /* premultiplied: what was drawn on black is its share already */
            out[0] = (BYTE)min(c[0] / n, a); out[1] = (BYTE)min(c[1] / n, a); out[2] = (BYTE)min(c[2] / n, a); out[3] = (BYTE)a;
        }
    GdiAlphaBlend(dc, x, y, w, h, dcs[1], 0, 0, w, h, bf);
done:
    for (i = 0; i < 2; i++) {
        if (olds[i]) SelectObject(dcs[i], olds[i]);
        if (bmps[i]) DeleteObject(bmps[i]);
        if (dcs[i]) DeleteDC(dcs[i]);
    }
}

/* art never draws pure black (it would read as nothing): the darkest it gives */
static inline COLORREF sg_smooth_colour(COLORREF c) { return c == RGB(0, 0, 0) ? RGB(1, 1, 1) : c; }

#endif
