/* sg-wordpad -- the ribbon's pictures, drawn here (no borrowed artwork).
 *
 * As Paint's and the Control Panel's: plain GDI on a 64-unit grid at four
 * times the size over a key colour, box-filtered down to premultiplied
 * alpha, cached per glyph and size.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "wordpad.h"

#define SS 4
#define KEY RGB(0xFF, 0x00, 0xFF)
#define INK RGB(56, 56, 66)
#define PURPLE ACCENT
#define LILAC RGB(180, 150, 230)
#define PAPER RGB(255, 255, 255)
#define WOOD RGB(232, 190, 120)
#define BLUE RGB(63, 72, 204)

static int g_n;
static int P(int v) { return v * g_n / 64; }

static HPEN pen(COLORREF c, int w)
{
    LOGBRUSH lb = { BS_SOLID, c, 0 };
    return ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, P(w) > 0 ? P(w) : 1, &lb, 0, NULL);
}

static void lines(HDC dc, COLORREF c, int w, const int *xy, int n)
{
    HPEN p = pen(c, w);
    HGDIOBJ op = SelectObject(dc, p);
    MoveToEx(dc, P(xy[0]), P(xy[1]), NULL);
    for (int i = 1; i < n; i++) LineTo(dc, P(xy[2 * i]), P(xy[2 * i + 1]));
    SelectObject(dc, op); DeleteObject(p);
}

static void line(HDC dc, COLORREF c, int w, int x1, int y1, int x2, int y2)
{
    int xy[4] = { x1, y1, x2, y2 };
    lines(dc, c, w, xy, 2);
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

static void poly(HDC dc, COLORREF c, const int *xy, int n)
{
    POINT pts[32];
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
    for (int i = 0; i < n && i < 32; i++) { pts[i].x = P(xy[2 * i]); pts[i].y = P(xy[2 * i + 1]); }
    Polygon(dc, pts, n);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br);
}

/* text in the glyph: size and cell on the 64 grid */
static void text(HDC dc, const WCHAR *s, COLORREF c, int size, int weight, BOOL italic, BOOL underline, BOOL strike,
                 int l, int t, int r, int b)
{
    HFONT f = CreateFontW(-P(size), 0, 0, 0, weight, italic, underline, strike, DEFAULT_CHARSET, 0, 0,
                          NONANTIALIASED_QUALITY, 0, L"Segoe UI");
    HGDIOBJ of = SelectObject(dc, f);
    RECT rc = { P(l), P(t), P(r), P(b) };
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of); DeleteObject(f);
}

static void text_lines(HDC dc, int l, int t, int r, int n, int gap, COLORREF c)
{
    for (int i = 0; i < n; i++) line(dc, c, 4, l, t + i * gap, r, t + i * gap);
}

static void draw(HDC dc, int g)
{
    switch (g)
    {
    case G_PASTE:
        rect(dc, WOOD, RGB(170, 120, 60), 3, 10, 10, 50, 58, 6);
        rect(dc, RGB(120, 120, 130), CLR_NONE, 0, 20, 5, 40, 16, 4);
        rect(dc, PAPER, INK, 3, 26, 22, 58, 60, 0);
        line(dc, LILAC, 3, 32, 32, 52, 32); line(dc, LILAC, 3, 32, 40, 52, 40); line(dc, LILAC, 3, 32, 48, 46, 48);
        break;
    case G_CUT:
        line(dc, INK, 5, 22, 6, 40, 40); line(dc, INK, 5, 42, 6, 24, 40);
        ellipse(dc, CLR_NONE, PURPLE, 5, 10, 38, 28, 56);
        ellipse(dc, CLR_NONE, PURPLE, 5, 36, 38, 54, 56);
        break;
    case G_COPY:
        rect(dc, PAPER, INK, 4, 8, 6, 38, 42, 0);
        rect(dc, PAPER, PURPLE, 4, 24, 22, 56, 58, 0);
        break;
    case G_GROW:
        text(dc, L"A", INK, 44, FW_NORMAL, FALSE, FALSE, FALSE, 0, 8, 46, 64);
        { int t[] = { 46, 22, 62, 22, 54, 10 }; poly(dc, PURPLE, t, 3); }
        break;
    case G_SHRINK:
        text(dc, L"A", INK, 34, FW_NORMAL, FALSE, FALSE, FALSE, 0, 16, 42, 64);
        { int t[] = { 44, 10, 60, 10, 52, 22 }; poly(dc, PURPLE, t, 3); }
        break;
    case G_BOLD: text(dc, L"B", INK, 48, FW_HEAVY, FALSE, FALSE, FALSE, 0, 0, 64, 64); break;
    case G_ITALIC: text(dc, L"I", INK, 48, FW_NORMAL, TRUE, FALSE, FALSE, 0, 0, 64, 64); break;
    case G_UNDERLINE:
        text(dc, L"U", INK, 44, FW_NORMAL, FALSE, FALSE, FALSE, 0, -4, 64, 56);
        line(dc, INK, 4, 16, 56, 48, 56);
        break;
    case G_STRIKE:
        text(dc, L"abc", INK, 34, FW_NORMAL, FALSE, FALSE, FALSE, 0, 0, 64, 64);
        line(dc, PURPLE, 4, 4, 34, 60, 34);
        break;
    case G_SUB:
        text(dc, L"x", INK, 42, FW_NORMAL, FALSE, FALSE, FALSE, 0, 0, 44, 58);
        text(dc, L"2", PURPLE, 26, FW_BOLD, FALSE, FALSE, FALSE, 36, 30, 64, 64);
        break;
    case G_SUPER:
        text(dc, L"x", INK, 42, FW_NORMAL, FALSE, FALSE, FALSE, 0, 10, 44, 64);
        text(dc, L"2", PURPLE, 26, FW_BOLD, FALSE, FALSE, FALSE, 36, 0, 64, 32);
        break;
    case G_HIGHLIGHT:
    {
        /* a marker over a bar (the bar's colour is painted by the ribbon) */
        int body[] = { 22, 38, 42, 12, 54, 22, 34, 48 };
        int tip[] = { 22, 38, 34, 48, 26, 52, 16, 50 };
        poly(dc, RGB(90, 90, 100), body, 4);
        poly(dc, RGB(250, 220, 40), tip, 4);
        break;
    }
    case G_COLOR:
        text(dc, L"A", INK, 44, FW_BOLD, FALSE, FALSE, FALSE, 0, -6, 64, 52);
        break;
    case G_INDENT_LESS:
    case G_INDENT_MORE:
        text_lines(dc, 28, 10, 58, 4, 12, INK);
        line(dc, INK, 4, 6, 58, 58, 58);
        if (g == G_INDENT_MORE) { int t[] = { 6, 14, 20, 26, 6, 38 }; poly(dc, PURPLE, t, 3); }
        else { int t[] = { 20, 14, 6, 26, 20, 38 }; poly(dc, PURPLE, t, 3); }
        break;
    case G_LIST:
        for (int i = 0; i < 3; i++)
        {
            ellipse(dc, PURPLE, CLR_NONE, 0, 6, 8 + i * 18, 16, 18 + i * 18);
            line(dc, INK, 4, 24, 13 + i * 18, 58, 13 + i * 18);
        }
        break;
    case G_SPACING:
        text_lines(dc, 28, 12, 58, 4, 12, INK);
        { int u[] = { 12, 6, 4, 18, 20, 18 }, d[] = { 12, 58, 4, 46, 20, 46 }; poly(dc, PURPLE, u, 3); poly(dc, PURPLE, d, 3); }
        line(dc, PURPLE, 4, 12, 16, 12, 48);
        break;
    case G_ALEFT: case G_ACENTER: case G_ARIGHT: case G_AJUSTIFY:
    {
        static const int w[5] = { 52, 36, 52, 30, 44 };
        for (int i = 0; i < 5; i++)
        {
            int len = g == G_AJUSTIFY && i < 4 ? 52 : w[i], l;
            if (g == G_ACENTER) l = 32 - len / 2;
            else if (g == G_ARIGHT) l = 58 - len;
            else l = 6;
            line(dc, INK, 4, l, 10 + i * 11, l + len, 10 + i * 11);
        }
        break;
    }
    case G_PARA:
        text(dc, L"\x00B6", PURPLE, 48, FW_NORMAL, FALSE, FALSE, FALSE, 0, 0, 64, 64);
        break;
    case G_PICTURE:
        rect(dc, RGB(200, 228, 250), INK, 3, 6, 10, 58, 54, 0);
        ellipse(dc, RGB(255, 200, 40), CLR_NONE, 0, 36, 16, 48, 28);
        { int m[] = { 8, 52, 24, 30, 36, 44, 44, 36, 56, 52 }; poly(dc, RGB(60, 150, 70), m, 5); }
        break;
    case G_DATETIME:
        rect(dc, PAPER, INK, 3, 6, 10, 50, 54, 3);
        rect(dc, PURPLE, CLR_NONE, 0, 6, 10, 50, 20, 3);
        for (int r = 0; r < 3; r++) for (int c = 0; c < 4; c++) rect(dc, LILAC, CLR_NONE, 0, 11 + c * 9, 25 + r * 9, 17 + c * 9, 31 + r * 9, 0);
        ellipse(dc, PAPER, INK, 3, 32, 32, 62, 62);
        line(dc, INK, 3, 47, 47, 47, 38); line(dc, INK, 3, 47, 47, 55, 50);
        break;
    case G_FIND:
        ellipse(dc, PAPER, INK, 5, 8, 8, 40, 40);
        line(dc, PURPLE, 7, 36, 36, 56, 56);
        break;
    case G_REPLACE:
        text(dc, L"ab", INK, 28, FW_NORMAL, FALSE, FALSE, FALSE, 0, 0, 36, 30);
        text(dc, L"ac", PURPLE, 28, FW_NORMAL, FALSE, FALSE, FALSE, 26, 32, 64, 62);
        { int a[] = { 16, 34, 16, 48, 26, 48 }; lines(dc, INK, 3, a, 3); int t[] = { 24, 42, 32, 48, 24, 54 }; poly(dc, INK, t, 3); }
        break;
    case G_SELECTALL:
        for (int i = 0; i < 4; i++) rect(dc, RGB(214, 196, 240), CLR_NONE, 0, 6, 6 + i * 14, 58, 16 + i * 14, 0);
        text_lines(dc, 10, 11, 54, 4, 14, INK);
        break;
    case G_ZOOMIN:
    case G_ZOOMOUT:
        ellipse(dc, PAPER, INK, 5, 6, 6, 44, 44);
        line(dc, INK, 7, 40, 40, 58, 58);
        line(dc, PURPLE, 5, 16, 25, 34, 25);
        if (g == G_ZOOMIN) line(dc, PURPLE, 5, 25, 16, 25, 34);
        break;
    case G_ZOOM100:
        rect(dc, PAPER, INK, 4, 10, 4, 54, 60, 0);
        text(dc, L"100", PURPLE, 22, FW_BOLD, FALSE, FALSE, FALSE, 10, 18, 54, 46);
        break;
    case G_RULER:
        rect(dc, RGB(250, 246, 230), INK, 3, 4, 20, 60, 44, 0);
        for (int i = 0; i < 7; i++) line(dc, INK, 3, 10 + i * 8, 20, 10 + i * 8, i % 2 ? 28 : 34);
        break;
    case G_STATUS:
        rect(dc, PAPER, INK, 3, 4, 8, 60, 56, 0);
        rect(dc, PURPLE, CLR_NONE, 0, 4, 46, 60, 56, 0);
        break;
    case G_WRAP:
        text_lines(dc, 6, 12, 58, 2, 14, INK);
        { int a[] = { 58, 26, 58, 40, 26, 40 }; lines(dc, PURPLE, 4, a, 3); int t[] = { 28, 32, 18, 40, 28, 48 }; poly(dc, PURPLE, t, 3); }
        line(dc, INK, 4, 6, 54, 30, 54);
        break;
    case G_UNITS:
        rect(dc, RGB(250, 246, 230), INK, 3, 4, 14, 60, 50, 0);
        for (int i = 0; i < 7; i++) line(dc, INK, 3, 10 + i * 8, 14, 10 + i * 8, i % 2 ? 22 : 30);
        text(dc, L"cm", PURPLE, 20, FW_BOLD, FALSE, FALSE, FALSE, 10, 28, 56, 50);
        break;
    case G_TABLE:
        rect(dc, PAPER, INK, 3, 6, 10, 58, 54, 0);
        rect(dc, PURPLE, CLR_NONE, 0, 6, 10, 58, 21, 0);
        line(dc, INK, 3, 6, 32, 58, 32); line(dc, INK, 3, 6, 43, 58, 43);
        line(dc, INK, 3, 23, 10, 23, 54); line(dc, INK, 3, 41, 10, 41, 54);
        break;
    case G_LAUNCHER:
        line(dc, TEXT_GREY, 5, 12, 12, 12, 52); line(dc, TEXT_GREY, 5, 12, 12, 52, 12);
        line(dc, TEXT_GREY, 5, 24, 24, 52, 52);
        { int t[] = { 54, 54, 54, 34, 34, 54 }; poly(dc, TEXT_GREY, t, 3); }
        break;
    }
}

static HBITMAP render(int glyph, int size)
{
    int big = size * SS;
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
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
        {
            DWORD a = 0, rr = 0, gg = 0, b = 0;
            for (int j = 0; j < SS; j++)
                for (int i = 0; i < SS; i++)
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
    static struct { int g, size; HBITMAP bmp; } cache[96];
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
