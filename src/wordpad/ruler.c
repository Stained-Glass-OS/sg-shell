/* sg-wordpad -- the ruler: the line's width in the chosen units, and the
 * current paragraph's indents and tab stops, which can be dragged.
 *
 * Its 0 is where the text starts in the edit control (the formatting
 * rectangle's left, less the horizontal scroll), and it scales with the zoom.
 * Markers: first-line indent (top), hanging indent (bottom), left indent (the
 * box under it: moves both), right indent (bottom right). A click on the
 * bottom half adds a tab stop; dragging one off the ruler removes it.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "wordpad.h"

enum { D_NONE, D_FIRST, D_HANGING, D_LEFT, D_RIGHT, D_TAB };
static int g_drag, g_drag_tab, g_drag_val;

int ruler_height(void) { return S(26); }

static double px_per_twip(void) { return g_dpi / 1440.0 * g_zoom / 100.0; }
int twips_to_px(int twips) { return (int)(twips * px_per_twip() + (twips >= 0 ? 0.5 : -0.5)); }
int px_to_twips(int px) { return (int)(px / px_per_twip() + (px >= 0 ? 0.5 : -0.5)); }

static int origin(void)
{
    SCROLLINFO si = { sizeof(si), SIF_POS };
    POINT o = { 0, 0 };
    int x = edit_left_px();
    if (GetScrollInfo(g_edit, SB_HORZ, &si)) x -= si.nPos;
    ClientToScreen(g_edit, &o);
    ScreenToClient(g_ruler, &o);
    return o.x + x;
}

static int band_width(void)
{
    RECT rc;
    if (g_wrap == 2) return twips_to_px(line_width_twips());
    GetClientRect(g_edit, &rc);
    return rc.right - edit_left_px() - S(4);
}

typedef struct { int left, first, right; int ntabs; LONG tabs[MAX_TAB_STOPS]; } Ind;

static Ind get_ind(void)
{
    PARAFORMAT2 pf;
    Ind in;
    memset(&pf, 0, sizeof(pf));
    pf.cbSize = sizeof(pf);
    SendMessageW(g_edit, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
    in.left = pf.dxStartIndent + pf.dxOffset;
    in.first = -pf.dxOffset;
    in.right = pf.dxRightIndent;
    in.ntabs = pf.cTabCount;
    for (int i = 0; i < pf.cTabCount; i++) in.tabs[i] = pf.rgxTabs[i] & 0xFFFFFF;
    return in;
}

static void set_tabs(const LONG *tabs, int n)
{
    PARAFORMAT2 pf;
    memset(&pf, 0, sizeof(pf));
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_TABSTOPS;
    pf.cTabCount = (SHORT)n;
    for (int i = 0; i < n; i++) pf.rgxTabs[i] = tabs[i];
    SendMessageW(g_edit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    refresh_state();
}

static void unit(int *major, int *minor, int *label_step)
{
    switch (g_units)
    {
    case 1: *major = 567; *minor = 4; *label_step = 1; break;        /* cm */
    case 2: *major = 1440; *minor = 8; *label_step = 72; break;      /* points: labels 72, 144 */
    case 3: *major = 1440; *minor = 6; *label_step = 6; break;       /* picas: 6 to the inch */
    default: *major = 1440; *minor = 8; *label_step = 1; break;      /* inches */
    }
}

static void fill(HDC dc, const RECT *r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, r, b); DeleteObject(b); }

static void marker(HDC dc, int x, int y, int kind)
{
    /* kind 0: pointing down (top), 1: pointing up (bottom), 2: box */
    POINT p[5];
    int n = 5, w = S(5);
    HBRUSH b = CreateSolidBrush(RGB(250, 250, 252));
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(90, 90, 100));
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, pen);
    if (kind == 0) { p[0].x = x - w; p[0].y = y; p[1].x = x + w; p[1].y = y; p[2].x = x + w; p[2].y = y + S(4); p[3].x = x; p[3].y = y + S(8); p[4].x = x - w; p[4].y = y + S(4); }
    else if (kind == 1) { p[0].x = x; p[0].y = y; p[1].x = x + w; p[1].y = y + S(4); p[2].x = x + w; p[2].y = y + S(8); p[3].x = x - w; p[3].y = y + S(8); p[4].x = x - w; p[4].y = y + S(4); }
    else { Rectangle(dc, x - w, y, x + w + 1, y + S(5)); n = 0; }
    if (n) Polygon(dc, p, n);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(pen);
}

static void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps), mem;
    RECT rc, band;
    HBITMAP bmp;
    HGDIOBJ ob, of;
    int o = origin(), bw = band_width(), major, minor, step, top = S(5), bot = S(19);
    Ind in = get_ind();
    HPEN pen;
    GetClientRect(h, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    ob = SelectObject(mem, bmp);
    fill(mem, &rc, WORKSPACE);
    SetRect(&band, o, top, o + bw, bot);
    fill(mem, &band, RGB(255, 255, 255));
    band.top = rc.bottom - 1; band.bottom = rc.bottom; band.left = 0; band.right = rc.right; fill(mem, &band, LINE_GREY);
    unit(&major, &minor, &step);
    pen = CreatePen(PS_SOLID, 1, RGB(110, 110, 120));
    SelectObject(mem, pen);
    of = SelectObject(mem, g_font_small);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(60, 60, 70));
    for (int i = 1; ; i++)
    {
        int tw = (int)((double)major * i / minor), x = o + twips_to_px(tw);
        if (x > o + bw || x > rc.right) break;
        if (i % minor == 0)
        {
            WCHAR t[8];
            RECT tr = { x - S(12), top, x + S(12), bot };
            swprintf(t, 8, L"%d", (i / minor) * step);
            DrawTextW(mem, t, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        else
        {
            int len = (minor == 8 && i % 4 == 0) || (minor != 8 && minor % 2 == 0 && i % (minor / 2) == 0) ? S(4) : S(2);
            MoveToEx(mem, x, (top + bot) / 2 - len / 2, NULL);
            LineTo(mem, x, (top + bot) / 2 + len / 2 + 1);
        }
    }
    SelectObject(mem, of);
    /* tab stops: small L shapes on the bottom edge */
    for (int i = 0; i < in.ntabs; i++)
    {
        int x = o + twips_to_px(in.tabs[i]);
        MoveToEx(mem, x, bot - S(6), NULL); LineTo(mem, x, bot - 1); LineTo(mem, x + S(5), bot - 1);
    }
    SelectObject(mem, GetStockObject(BLACK_PEN));
    DeleteObject(pen);
    marker(mem, o + twips_to_px(in.left + in.first), 0, 0);
    marker(mem, o + twips_to_px(in.left), bot - S(7), 1);
    marker(mem, o + twips_to_px(in.left), bot + S(1), 2);
    if (g_wrap == 2) marker(mem, o + bw - twips_to_px(in.right), bot - S(7), 1);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
    EndPaint(h, &ps);
}

void ruler_dump(FILE *f)
{
    Ind in;
    POINT a;
    int o, bot = S(19);
    if (!g_ruler_on) return;
    in = get_ind();
    o = origin();
#define PT(name, px, py) do { a.x = (px); a.y = (py); ClientToScreen(g_ruler, &a); fprintf(f, name " %ld %ld\n", a.x, a.y); } while (0)
    PT("ruler_origin", o, S(12));
    PT("ruler_first", o + twips_to_px(in.left + in.first), S(3));
    PT("ruler_hanging", o + twips_to_px(in.left), bot - S(3));
    PT("ruler_left", o + twips_to_px(in.left), bot + S(3));
    if (g_wrap == 2) PT("ruler_right", o + band_width() - twips_to_px(in.right), bot - S(3));
#undef PT
    fprintf(f, "ruler_twips_per_100px %d\n", px_to_twips(100));
}

static int snap(int twips)
{
    int q = g_units == 1 ? 142 : 90;           /* 1/4 cm, 1/16 inch */
    return (twips + q / 2) / q * q;
}

LRESULT CALLBACK ruler_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_PAINT: paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN:
    {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l), o = origin(), bot = S(19);
        Ind in = get_ind();
        int xf = o + twips_to_px(in.left + in.first), xl = o + twips_to_px(in.left), xr = o + band_width() - twips_to_px(in.right);
        g_drag = D_NONE;
        if (abs(x - xf) <= S(6) && y < S(10)) g_drag = D_FIRST;
        else if (abs(x - xl) <= S(6) && y > bot) g_drag = D_LEFT;
        else if (abs(x - xl) <= S(6) && y >= bot - S(8)) g_drag = D_HANGING;
        else if (g_wrap == 2 && abs(x - xr) <= S(6) && y >= bot - S(8)) g_drag = D_RIGHT;
        else
        {
            for (int i = 0; i < in.ntabs; i++)
                if (abs(x - (o + twips_to_px(in.tabs[i]))) <= S(4) && y >= S(10)) { g_drag = D_TAB; g_drag_tab = i; break; }
            if (g_drag == D_NONE && y >= S(10) && x > o && x < o + band_width() && in.ntabs < MAX_TAB_STOPS)
            {
                /* a new tab stop, kept in order */
                LONG t = snap(px_to_twips(x - o));
                int j = in.ntabs;
                while (j > 0 && in.tabs[j - 1] > t) { in.tabs[j] = in.tabs[j - 1]; j--; }
                if (j > 0 && in.tabs[j - 1] == t) return 0;
                in.tabs[j] = t; in.ntabs++;
                set_tabs(in.tabs, in.ntabs);
                g_drag = D_TAB; g_drag_tab = j;
            }
        }
        if (g_drag != D_NONE) SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        int x = GET_X_LPARAM(l), o = origin(), t;
        Ind in;
        if (g_drag == D_NONE) return 0;
        in = get_ind();
        t = snap(px_to_twips(x - o));
        switch (g_drag)
        {
        case D_FIRST: set_indents(in.left, max(t, 0) - in.left, -1); break;
        case D_HANGING:
        {
            int first_abs = in.left + in.first;
            t = max(t, 0);
            set_indents(t, first_abs - t, -1);
            break;
        }
        case D_LEFT: t = max(t, 0); set_indents(t, max(in.first, -t), -1); break;
        case D_RIGHT: set_indents(-1, INT_MIN, max(0, line_width_twips() - t)); break;
        case D_TAB:
            if (g_drag_tab < in.ntabs && t > 0) { in.tabs[g_drag_tab] = t; set_tabs(in.tabs, in.ntabs); }
            break;
        }
        g_drag_val = t;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_drag == D_TAB && (GET_Y_LPARAM(l) > ruler_height() + S(8) || GET_Y_LPARAM(l) < -S(8)))
        {
            Ind in = get_ind();
            if (g_drag_tab < in.ntabs)
            {
                for (int i = g_drag_tab; i < in.ntabs - 1; i++) in.tabs[i] = in.tabs[i + 1];
                set_tabs(in.tabs, in.ntabs - 1);
            }
        }
        if (g_drag != D_NONE) { ReleaseCapture(); g_drag = D_NONE; SetFocus(g_edit); write_dump(); }
        return 0;
    }
    (void)w;
    return DefWindowProcW(h, m, w, l);
}
