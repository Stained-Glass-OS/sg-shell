/* sg-control -- the Control Panel's icons, drawn here (no borrowed artwork).
 *
 * Each icon is drawn with plain GDI on a 64-unit grid at four times its size
 * over a key colour, then box-filtered down to premultiplied alpha and
 * alpha-blended: smooth edges without depending on anti-aliasing in GDI+.
 * Results are cached per icon and size.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"
#include <math.h>

#define SS 4                            /* supersampling factor */
#define KEY RGB(0xFF, 0x00, 0xFF)       /* never used by an icon */

static int g_n;                         /* the big canvas's size */
static int P(int v) { return v * g_n / 64; }

static void brush_ellipse(HDC dc, COLORREF c, int l, int t, int r, int b)
{
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, P(l), P(t), P(r), P(b));
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br);
}

static void brush_rect(HDC dc, COLORREF c, int l, int t, int r, int b, int round)
{
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
    if (round) RoundRect(dc, P(l), P(t), P(r), P(b), P(round), P(round));
    else Rectangle(dc, P(l), P(t), P(r) + 1, P(b) + 1);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br);
}

static void brush_poly(HDC dc, COLORREF c, const int *xy, int n)
{
    POINT pts[16];
    int i;
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
    for (i = 0; i < n && i < 16; i++) { pts[i].x = P(xy[2 * i]); pts[i].y = P(xy[2 * i + 1]); }
    Polygon(dc, pts, n);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br);
}

static HPEN round_pen(COLORREF c, int w)
{
    LOGBRUSH lb = { BS_SOLID, c, 0 };
    return ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, P(w) > 0 ? P(w) : 1, &lb, 0, NULL);
}

static void line(HDC dc, COLORREF c, int w, const int *xy, int n)
{
    HPEN pen = round_pen(c, w);
    HGDIOBJ op = SelectObject(dc, pen);
    int i;
    MoveToEx(dc, P(xy[0]), P(xy[1]), NULL);
    for (i = 1; i < n; i++) LineTo(dc, P(xy[2 * i]), P(xy[2 * i + 1]));
    SelectObject(dc, op); DeleteObject(pen);
}

static void ring(HDC dc, COLORREF c, int w, int l, int t, int r, int b)
{
    HPEN pen = round_pen(c, w);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, P(l), P(t), P(r), P(b));
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
}

static void arc(HDC dc, COLORREF c, int w, int l, int t, int r, int b, int x1, int y1, int x2, int y2)
{
    HPEN pen = round_pen(c, w);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Arc(dc, P(l), P(t), P(r), P(b), P(x1), P(y1), P(x2), P(y2));
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
}

/* the pieces several icons share */
static void monitor(HDC dc, COLORREF screen)
{
    brush_rect(dc, RGB(0x2E, 0x38, 0x48), 4, 8, 60, 46, 6);
    brush_rect(dc, screen, 8, 12, 56, 42, 0);
    brush_rect(dc, RGB(0x6B, 0x74, 0x82), 26, 46, 38, 54, 0);
    brush_rect(dc, RGB(0x2E, 0x38, 0x48), 16, 53, 48, 58, 4);
}

static void logo(HDC dc, int cx, int cy, int s)   /* the Stained Glass diamonds */
{
    int a[] = { cx - s, cy, cx, cy - s, cx + s, cy, cx, cy + s };
    int d = s + s / 3;
    int t1[8], t2[8], t3[8], i;
    for (i = 0; i < 8; i += 2) {
        t1[i] = a[i] - d / 2; t1[i + 1] = a[i + 1];
        t2[i] = a[i] + d / 2; t2[i + 1] = a[i + 1];
        t3[i] = a[i];         t3[i + 1] = a[i + 1] + d / 2 + 1;
    }
    brush_poly(dc, RGB(0x1F, 0xB5, 0xB0), t1, 4);
    brush_poly(dc, RGB(0x9B, 0x3C, 0xC9), t2, 4);
    brush_poly(dc, RGB(0xF2, 0xA0, 0x1F), t3, 4);
}

static void globe(HDC dc, COLORREF fill, int l, int t, int r, int b)
{
    int cx = (l + r) / 2, w = r - l;
    brush_ellipse(dc, fill, l, t, r, b);
    ring(dc, RGB(0xFF, 0xFF, 0xFF), 2, cx - w / 5, t + 1, cx + w / 5, b - 1);
    { int h[] = { l + 2, (t + b) / 2, r - 2, (t + b) / 2 }; line(dc, RGB(0xFF, 0xFF, 0xFF), 2, h, 2); }
    { int v[] = { cx, t + 1, cx, b - 1 }; line(dc, RGB(0xFF, 0xFF, 0xFF), 2, v, 2); }
    arc(dc, RGB(0xFF, 0xFF, 0xFF), 2, l + 4, t - (b - t) / 3, r - 4, (t + b) / 2 - 2, r, (t + b) / 2, l, (t + b) / 2);
    arc(dc, RGB(0xFF, 0xFF, 0xFF), 2, l + 4, (t + b) / 2 + 2, r - 4, b + (b - t) / 3, l, (t + b) / 2, r, (t + b) / 2);
}

static void person(HDC dc, COLORREF c, int cx, int top, int s)
{
    brush_ellipse(dc, c, cx - s / 3, top, cx + s / 3, top + s * 2 / 3);
    {
        HBRUSH br = CreateSolidBrush(c);
        HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
        Chord(dc, P(cx - s * 3 / 5), P(top + s * 3 / 4), P(cx + s * 3 / 5), P(top + s * 2),
              P(cx + s), P(top + s * 11 / 8), P(cx - s), P(top + s * 11 / 8));
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br);
    }
}

static void check(HDC dc, COLORREF c, int w, int x, int y, int s)
{
    int pts[] = { x, y, x + s / 3, y + s / 3, x + s, y - s / 2 };
    line(dc, c, w, pts, 3);
}


/* ---- Settings' glyphs: thin line drawings in one colour --------------------------------- */
COLORREF g_glyph_color = RGB(0x70, 0x30, 0xC0);

static void rect_line(HDC dc, COLORREF c, int w, int l, int t, int r, int b)
{
    int pts[] = { l, t, r, t, r, b, l, b, l, t };
    line(dc, c, w, pts, 5);
}

static void paint_glyph(HDC dc, int icon)
{
    COLORREF c = g_glyph_color;
    int w = 3;
    switch (icon) {
    case IC_G_SYSTEM: {                 /* a laptop */
        int base[] = { 4, 50, 60, 50 };
        rect_line(dc, c, w, 12, 14, 52, 44);
        line(dc, c, w, base, 2);
        break;
    }
    case IC_G_PC: {                     /* a monitor on a stand */
        int stand[] = { 32, 46, 32, 54 }, foot[] = { 20, 55, 44, 55 };
        rect_line(dc, c, w, 6, 10, 58, 46);
        line(dc, c, w, stand, 2); line(dc, c, w, foot, 2);
        break;
    }
    case IC_G_DEVICES: {                /* a keyboard and a speaker */
        int i;
        rect_line(dc, c, w, 4, 28, 40, 52);
        for (i = 0; i < 4; i++) { int k[] = { 10 + i * 8, 36, 12 + i * 8, 36 }; line(dc, c, w, k, 2); }
        { int sp[] = { 12, 44, 32, 44 }; line(dc, c, w, sp, 2); }
        rect_line(dc, c, w, 46, 12, 60, 52);
        ring(dc, c, w, 48, 34, 58, 44);
        break;
    }
    case IC_G_NETWORK: {                /* a globe */
        int h[] = { 8, 32, 56, 32 }, v[] = { 32, 8, 32, 56 };
        ring(dc, c, w, 8, 8, 56, 56);
        ring(dc, c, w, 21, 8, 43, 56);
        line(dc, c, w, h, 2); line(dc, c, w, v, 2);
        break;
    }
    case IC_G_PERSONAL: {               /* a brush over a canvas */
        int handle[] = { 54, 8, 30, 34 };
        rect_line(dc, c, w, 6, 14, 44, 52);
        line(dc, c, w + 2, handle, 2);
        brush_ellipse(dc, c, 22, 30, 34, 42);
        break;
    }
    case IC_G_APPS: {                   /* a list of apps */
        int i;
        for (i = 0; i < 3; i++) {
            int y = 12 + i * 16, l[] = { 28, y + 6, 58, y + 6 };
            rect_line(dc, c, w, 8, y, 20, y + 12);
            line(dc, c, w, l, 2);
        }
        break;
    }
    case IC_G_ACCOUNTS:                 /* a person */
        ring(dc, c, w, 20, 6, 44, 30);
        arc(dc, c, w, 8, 34, 56, 78, 56, 56, 8, 56);
        break;
    case IC_G_TIME: {                   /* a clock and a letter */
        int hands[] = { 24, 14, 24, 26, 32, 30 }, a[] = { 38, 58, 48, 34, 58, 58 }, bar[] = { 42, 50, 54, 50 };
        ring(dc, c, w, 6, 4, 42, 40);
        line(dc, c, w, hands, 3);
        line(dc, c, w, a, 3); line(dc, c, w, bar, 2);
        break;
    }
    case IC_G_EOA: {                    /* a figure with arms out, in a circle */
        int arms[] = { 18, 26, 32, 28, 46, 26 }, body[] = { 32, 28, 32, 40 }, legs[] = { 24, 52, 32, 40, 40, 52 };
        ring(dc, c, w, 4, 4, 60, 60);
        brush_ellipse(dc, c, 28, 12, 36, 20);
        line(dc, c, w, arms, 3); line(dc, c, w, body, 2); line(dc, c, w, legs, 3);
        break;
    }
    case IC_G_PRIVACY: {                /* a padlock */
        rect_line(dc, c, w, 12, 28, 52, 58);
        arc(dc, c, w, 20, 6, 44, 46, 44, 26, 20, 26);
        { int s1[] = { 20, 26, 20, 28 }, s2[] = { 44, 26, 44, 28 }; line(dc, c, w, s1, 2); line(dc, c, w, s2, 2); }
        brush_ellipse(dc, c, 28, 38, 36, 46);
        break;
    }
    case IC_G_UPDATE: {                 /* two arrows chasing each other */
        int h1[] = { 50, 8, 52, 20, 40, 20 }, h2[] = { 14, 56, 12, 44, 24, 44 };
        arc(dc, c, w, 8, 8, 56, 56, 52, 20, 8, 32);
        arc(dc, c, w, 8, 8, 56, 56, 12, 44, 56, 32);
        line(dc, c, w, h1, 3); line(dc, c, w, h2, 3);
        break;
    }
    case IC_G_HOME: {                   /* a house */
        int roof[] = { 6, 30, 32, 8, 58, 30 }, walls[] = { 14, 24, 14, 56, 50, 56, 50, 24 }, door[] = { 26, 56, 26, 40, 38, 40, 38, 56 };
        line(dc, c, w, roof, 3); line(dc, c, w, walls, 4); line(dc, c, w, door, 4);
        break;
    }
    case IC_G_SEARCH: {                 /* a magnifier */
        int handle[] = { 38, 38, 56, 56 };
        ring(dc, c, w, 8, 8, 42, 42);
        line(dc, c, w + 1, handle, 2);
        break;
    }
    case IC_G_BACK: {                   /* an arrow to the left */
        int shaft[] = { 8, 32, 56, 32 }, head[] = { 26, 14, 8, 32, 26, 50 };
        line(dc, c, w, shaft, 2); line(dc, c, w, head, 3);
        break;
    }
    }
}

static void paint_icon(HDC dc, int icon)
{
    if (icon >= IC_G_SYSTEM && icon < IC_COUNT) { paint_glyph(dc, icon); return; }
    switch (icon) {
    case IC_SHIELD: {                   /* the elevation shield: gold and blue */
        int left[] = { 32, 4, 32, 60, 10, 38, 8, 12 };
        int right[] = { 32, 4, 56, 12, 54, 38, 32, 60 };
        brush_poly(dc, RGB(0xF2, 0xB1, 0x1B), left, 4);
        brush_poly(dc, RGB(0x1E, 0x6F, 0xD9), right, 4);
        break;
    }
    case IC_SYSSEC: {
        int s[] = { 32, 4, 56, 12, 54, 36, 32, 60, 10, 36, 8, 12 };
        int hi[] = { 32, 4, 32, 60, 10, 36, 8, 12 };
        brush_poly(dc, RGB(0x1E, 0x6F, 0xD9), s, 6);
        brush_poly(dc, RGB(0x3D, 0x8B, 0xEB), hi, 4);
        check(dc, RGB(0xFF, 0xFF, 0xFF), 6, 20, 34, 24);
        break;
    }
    case IC_NET: globe(dc, RGB(0x1C, 0x8A, 0xDB), 6, 6, 58, 58); break;
    case IC_INET:
        globe(dc, RGB(0x1C, 0x8A, 0xDB), 4, 4, 50, 50);
        brush_rect(dc, RGB(0xF2, 0xA0, 0x1F), 36, 38, 60, 60, 4);
        ring(dc, RGB(0xF2, 0xA0, 0x1F), 4, 41, 28, 55, 44);
        brush_rect(dc, RGB(0xFF, 0xFF, 0xFF), 46, 44, 50, 52, 0);
        break;
    case IC_NETCENTER:
        monitor(dc, RGB(0x3A, 0x7B, 0xD5));
        globe(dc, RGB(0x10, 0x9E, 0x5A), 30, 22, 62, 54);
        break;
    case IC_HW:                         /* a printer */
        brush_rect(dc, RGB(0xFF, 0xFF, 0xFF), 16, 6, 48, 26, 0);
        ring(dc, RGB(0xB8, 0xC0, 0xCC), 1, 16, 6, 48, 26);
        brush_rect(dc, RGB(0x5B, 0x65, 0x74), 4, 22, 60, 48, 8);
        brush_rect(dc, RGB(0x3E, 0x47, 0x55), 12, 40, 52, 58, 3);
        brush_rect(dc, RGB(0xFF, 0xFF, 0xFF), 16, 44, 48, 58, 0);
        brush_ellipse(dc, RGB(0x3C, 0xD0, 0x6E), 48, 28, 54, 34);
        break;
    case IC_GAME:
        brush_rect(dc, RGB(0x3E, 0x47, 0x55), 4, 18, 60, 48, 24);
        brush_ellipse(dc, RGB(0x3E, 0x47, 0x55), 4, 26, 26, 58);
        brush_ellipse(dc, RGB(0x3E, 0x47, 0x55), 38, 26, 60, 58);
        brush_rect(dc, RGB(0xFF, 0xFF, 0xFF), 14, 30, 28, 34, 0);
        brush_rect(dc, RGB(0xFF, 0xFF, 0xFF), 19, 25, 23, 39, 0);
        brush_ellipse(dc, RGB(0x3C, 0xD0, 0x6E), 40, 24, 47, 31);
        brush_ellipse(dc, RGB(0xE8, 0x4A, 0x3D), 46, 31, 53, 38);
        brush_ellipse(dc, RGB(0x1E, 0x6F, 0xD9), 34, 31, 41, 38);
        brush_ellipse(dc, RGB(0xF2, 0xB1, 0x1B), 40, 37, 47, 44);
        break;
    case IC_PROG:                       /* an app grid */
        brush_rect(dc, RGB(0x1E, 0x6F, 0xD9), 6, 6, 30, 30, 8);
        brush_rect(dc, RGB(0x10, 0x9E, 0x5A), 34, 6, 58, 30, 8);
        brush_rect(dc, RGB(0xF2, 0xA0, 0x1F), 6, 34, 30, 58, 8);
        brush_rect(dc, RGB(0x9B, 0x3C, 0xC9), 34, 34, 58, 58, 8);
        break;
    case IC_USERS:
        person(dc, RGB(0x8A, 0x9B, 0xB5), 40, 8, 22);
        person(dc, RGB(0x1E, 0x6F, 0xD9), 26, 16, 26);
        break;
    case IC_USER:                       /* an account picture placeholder */
        brush_ellipse(dc, RGB(0xC9, 0xD2, 0xDF), 0, 0, 64, 64);
        brush_ellipse(dc, RGB(0xFF, 0xFF, 0xFF), 21, 12, 43, 34);
        {
            HRGN clip = CreateEllipticRgn(P(0), P(0), P(64), P(64));
            SelectClipRgn(dc, clip);
            brush_ellipse(dc, RGB(0xFF, 0xFF, 0xFF), 10, 38, 54, 76);
            SelectClipRgn(dc, NULL);
            DeleteObject(clip);
        }
        break;
    case IC_APPEAR:
        monitor(dc, RGB(0x24, 0x70, 0x94));
        { int a[] = { 8, 42, 30, 12, 40, 42 }; brush_poly(dc, RGB(0x1F, 0xB5, 0xB0), a, 3); }
        { int b[] = { 26, 42, 44, 18, 56, 42 }; brush_poly(dc, RGB(0x9B, 0x3C, 0xC9), b, 3); }
        { int c[] = { 40, 42, 56, 26, 56, 42 }; brush_poly(dc, RGB(0xF2, 0xA0, 0x1F), c, 3); }
        break;
    case IC_PERSONAL:                   /* a palette */
        brush_ellipse(dc, RGB(0xF3, 0xD9, 0xA4), 4, 8, 60, 56);
        brush_ellipse(dc, RGB(0xFF, 0xFF, 0xFF), 36, 34, 48, 46);
        brush_ellipse(dc, RGB(0xE8, 0x4A, 0x3D), 14, 20, 24, 30);
        brush_ellipse(dc, RGB(0x1E, 0x6F, 0xD9), 28, 14, 38, 24);
        brush_ellipse(dc, RGB(0x10, 0x9E, 0x5A), 42, 18, 52, 28);
        brush_ellipse(dc, RGB(0x9B, 0x3C, 0xC9), 14, 34, 24, 44);
        break;
    case IC_CLOCK:
        brush_ellipse(dc, RGB(0x1E, 0x6F, 0xD9), 4, 4, 60, 60);
        brush_ellipse(dc, RGB(0xFF, 0xFF, 0xFF), 9, 9, 55, 55);
        { int h[] = { 32, 32, 32, 16 }; line(dc, RGB(0x2E, 0x38, 0x48), 4, h, 2); }
        { int m[] = { 32, 32, 43, 38 }; line(dc, RGB(0x2E, 0x38, 0x48), 4, m, 2); }
        brush_ellipse(dc, RGB(0x2E, 0x38, 0x48), 29, 29, 35, 35);
        break;
    case IC_DATETIME:
        brush_rect(dc, RGB(0xFF, 0xFF, 0xFF), 4, 8, 52, 54, 6);
        ring(dc, RGB(0xB8, 0xC0, 0xCC), 1, 4, 8, 52, 54);
        brush_rect(dc, RGB(0xE8, 0x4A, 0x3D), 4, 8, 52, 20, 6);
        brush_rect(dc, RGB(0xE8, 0x4A, 0x3D), 4, 14, 52, 20, 0);
        {
            int r, c;
            for (r = 0; r < 3; r++) for (c = 0; c < 4; c++)
                brush_rect(dc, RGB(0x8A, 0x9B, 0xB5), 10 + c * 10, 26 + r * 9, 15 + c * 10, 30 + r * 9, 0);
        }
        brush_ellipse(dc, RGB(0x1E, 0x6F, 0xD9), 34, 34, 62, 62);
        brush_ellipse(dc, RGB(0xFF, 0xFF, 0xFF), 37, 37, 59, 59);
        { int h[] = { 48, 48, 48, 40, }; line(dc, RGB(0x2E, 0x38, 0x48), 3, h, 2); }
        { int m[] = { 48, 48, 54, 50 }; line(dc, RGB(0x2E, 0x38, 0x48), 3, m, 2); }
        break;
    case IC_SYSTEM:
        monitor(dc, RGB(0x1E, 0x4E, 0x9A));
        logo(dc, 32, 25, 9);
        break;
    case IC_DISPLAY:
        monitor(dc, RGB(0x3A, 0x7B, 0xD5));
        brush_rect(dc, RGB(0x7F, 0xB2, 0xF0), 8, 12, 56, 24, 0);
        break;
    case IC_UPDATE:
        arc(dc, RGB(0x1E, 0x6F, 0xD9), 7, 8, 8, 56, 56, 56, 26, 14, 14);
        arc(dc, RGB(0x1E, 0x6F, 0xD9), 7, 8, 8, 56, 56, 8, 38, 50, 50);
        { int a[] = { 42, 4, 58, 22, 38, 26 }; brush_poly(dc, RGB(0x1E, 0x6F, 0xD9), a, 3); }
        { int b[] = { 22, 60, 6, 42, 26, 38 }; brush_poly(dc, RGB(0x1E, 0x6F, 0xD9), b, 3); }
        break;
    case IC_REFRESH:                    /* the navigation bar's: a grey circular arrow */
        arc(dc, RGB(0x44, 0x44, 0x44), 6, 10, 10, 54, 54, 40, 11, 54, 26);
        { int a[] = { 36, 0, 58, 12, 40, 26 }; brush_poly(dc, RGB(0x44, 0x44, 0x44), a, 3); }
        break;
    case IC_SPEECH:                     /* a microphone on a stand, in a blue disc */
        brush_ellipse(dc, RGB(0x1E, 0x6F, 0xD9), 2, 2, 62, 62);
        brush_rect(dc, RGB(0xFF, 0xFF, 0xFF), 24, 10, 40, 38, 16);
        arc(dc, RGB(0xFF, 0xFF, 0xFF), 4, 16, 18, 48, 46, 48, 32, 16, 32);
        { int st[] = { 32, 46, 32, 52 }; line(dc, RGB(0xFF, 0xFF, 0xFF), 4, st, 2); }
        { int ft[] = { 24, 53, 40, 53 }; line(dc, RGB(0xFF, 0xFF, 0xFF), 4, ft, 2); }
        break;
    case IC_FONTS:                      /* a folder with a large A on it */
        brush_rect(dc, RGB(0xD2, 0xA0, 0x32), 4, 10, 28, 20, 4);
        brush_rect(dc, RGB(0xD2, 0xA0, 0x32), 4, 16, 60, 56, 4);
        brush_rect(dc, RGB(0xEC, 0xBE, 0x50), 4, 22, 60, 56, 4);
        { int a[] = { 18, 50, 32, 16, 46, 50 }; line(dc, RGB(0x2E, 0x38, 0x48), 6, a, 3); }
        { int b[] = { 24, 38, 40, 38 }; line(dc, RGB(0x2E, 0x38, 0x48), 5, b, 2); }
        { int u[] = { 14, 53, 50, 53 }; line(dc, RGB(0x70, 0x30, 0xC0), 3, u, 2); }
        break;
    case IC_OK:
        brush_ellipse(dc, RGB(0x10, 0x7C, 0x10), 2, 2, 62, 62);
        check(dc, RGB(0xFF, 0xFF, 0xFF), 7, 18, 34, 28);
        break;
    case IC_WARN: {
        int t[] = { 32, 4, 62, 58, 2, 58 };
        brush_poly(dc, RGB(0xF2, 0xB1, 0x1B), t, 3);
        brush_rect(dc, RGB(0x1A, 0x1A, 0x1A), 29, 20, 35, 42, 0);
        brush_rect(dc, RGB(0x1A, 0x1A, 0x1A), 29, 47, 35, 53, 0);
        break;
    }
    default: {                          /* a gear */
        int i;
        for (i = 0; i < 8; i++) {
            double a = i * 3.14159265 / 4, ca = cos(a), sa = sin(a);
            int t[] = { 32 + (int)(ca * 30 - sa * 5), 32 + (int)(sa * 30 + ca * 5),
                        32 + (int)(ca * 30 + sa * 5), 32 + (int)(sa * 30 - ca * 5),
                        32 + (int)(ca * 18 + sa * 7), 32 + (int)(sa * 18 - ca * 7),
                        32 + (int)(ca * 18 - sa * 7), 32 + (int)(sa * 18 + ca * 7) };
            brush_poly(dc, RGB(0x6B, 0x74, 0x82), t, 4);
        }
        brush_ellipse(dc, RGB(0x6B, 0x74, 0x82), 10, 10, 54, 54);
        brush_ellipse(dc, RGB(0xFF, 0xFF, 0xFF), 22, 22, 42, 42);
        break;
    }
    }
}

struct cached { int icon, size; COLORREF color; HBITMAP bmp; };
static struct cached g_cache[96];
static int g_ncache;

static HBITMAP render(int icon, int size)
{
    int big = size * SS, x, y;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), big, -big, 1, 32, BI_RGB } };
    BITMAPINFO si = { { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB } };
    DWORD *src, *dst;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&src, NULL, 0), hs;
    HGDIOBJ old;
    if (!hb) { DeleteDC(dc); return NULL; }
    old = SelectObject(dc, hb);
    { HBRUSH k = CreateSolidBrush(KEY); RECT r = { 0, 0, big, big }; FillRect(dc, &r, k); DeleteObject(k); }
    g_n = big;
    paint_icon(dc, icon);
    GdiFlush();
    hs = CreateDIBSection(dc, &si, DIB_RGB_COLORS, (void **)&dst, NULL, 0);
    if (hs) {
        for (y = 0; y < size; y++)
            for (x = 0; x < size; x++) {
                unsigned r = 0, g = 0, b = 0, a = 0;
                int i, j;
                for (j = 0; j < SS; j++)
                    for (i = 0; i < SS; i++) {
                        DWORD p = src[(y * SS + j) * big + x * SS + i] & 0xFFFFFF;
                        if (p == 0xFF00FF) continue;           /* the key: transparent */
                        r += (p >> 16) & 0xFF; g += (p >> 8) & 0xFF; b += p & 0xFF; a += 255;
                    }
                /* premultiplied: the sums over all SS*SS samples */
                dst[y * size + x] = ((a / (SS * SS)) << 24) | ((r / (SS * SS)) << 16) | ((g / (SS * SS)) << 8) | (b / (SS * SS));
            }
    }
    SelectObject(dc, old);
    DeleteObject(hb);
    DeleteDC(dc);
    return hs;
}

void draw_icon(HDC dc, int icon, int x, int y, int size)
{
    HBITMAP bmp = NULL;
    int i;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC mem;
    if (size <= 0) return;
    for (i = 0; i < g_ncache; i++)
        if (g_cache[i].icon == icon && g_cache[i].size == size &&
            (icon < IC_G_SYSTEM || g_cache[i].color == g_glyph_color)) { bmp = g_cache[i].bmp; break; }
    if (!bmp) {
        if (!(bmp = render(icon, size))) return;
        if (g_ncache < (int)ARRAYSIZE(g_cache)) {
            g_cache[g_ncache].icon = icon; g_cache[g_ncache].size = size; g_cache[g_ncache].color = g_glyph_color;
            g_cache[g_ncache].bmp = bmp; g_ncache++;
        }
    }
    mem = CreateCompatibleDC(dc);
    SelectObject(mem, bmp);
    AlphaBlend(dc, x, y, size, size, mem, 0, 0, size, size, bf);
    DeleteDC(mem);
    if (i == g_ncache && g_ncache == (int)ARRAYSIZE(g_cache)) DeleteObject(bmp);
}
