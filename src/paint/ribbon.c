/* sg-paint -- the ribbon (File, Home, View) and the status bar, drawn here.
 *
 * The ribbon is a list of items laid out in 96-dpi units on every change of
 * tab, tool or size; drawing, hover and clicks all walk the same list, and
 * the dump (SG_PAINT_DUMP) prints each item's screen rectangle for the gate.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "paint.h"

enum { K_TAB, K_FILETAB, K_BIG, K_SMALL, K_ICON, K_SWATCH, K_PAL, K_GROUP, K_CHECK };

typedef struct {
    int cmd, kind, glyph, drop;         /* drop: a menu command for the lower part / arrow */
    const WCHAR *label;
    RECT rc;
} Item;

static Item g_items[160];
static int g_nitems, g_hot = -1, g_down = -1;
static int g_group_x;                   /* where the next group starts */
static RECT g_seps[16];
static int g_nseps;

static const WCHAR *BRUSH_NAMES[B_COUNT] = { L"Brush", L"Calligraphy brush 1", L"Calligraphy brush 2", L"Airbrush", L"Marker", L"Crayon" };
static const int BRUSH_GLYPHS[B_COUNT] = { G_BRUSH, G_CALLI1, G_CALLI2, G_AIRBRUSH, G_MARKER, G_CRAYON };
static const WCHAR *SHAPE_NAMES[S_COUNT] = { L"Line", L"Rectangle", L"Rounded rectangle", L"Oval", L"Triangle", L"Right triangle",
    L"Diamond", L"Pentagon", L"Hexagon", L"Right arrow", L"Left arrow", L"Up arrow", L"Down arrow", L"Four-point star",
    L"Five-point star", L"Heart" };

int ribbon_height(void) { return S(118); }
int status_height(void) { return S(24); }

static Item *add(int cmd, int kind, int glyph, const WCHAR *label, int x, int y, int w, int h)
{
    Item *it;
    if (g_nitems == ARRAYSIZE(g_items)) return &g_items[g_nitems - 1];
    it = &g_items[g_nitems++];
    memset(it, 0, sizeof(*it));
    it->cmd = cmd; it->kind = kind; it->glyph = glyph; it->label = label;
    SetRect(&it->rc, S(x), S(y), S(x + w), S(y + h));
    return it;
}

/* a group: items are placed from g_group_x; returns its left edge */
static void group_end(const WCHAR *label, int width)
{
    add(0, K_GROUP, 0, label, g_group_x, 98, width, 16);
    g_group_x += width;
    if (g_nseps < (int)ARRAYSIZE(g_seps)) SetRect(&g_seps[g_nseps++], S(g_group_x + 2), S(28), S(g_group_x + 3), S(112));
    g_group_x += 5;
}

static int text_w(const WCHAR *s)
{
    HDC dc = GetDC(NULL);
    HGDIOBJ o = SelectObject(dc, g_font);
    SIZE sz;
    GetTextExtentPoint32W(dc, s, lstrlenW(s), &sz);
    SelectObject(dc, o);
    ReleaseDC(NULL, dc);
    return MulDiv(sz.cx, 96, g_dpi);
}

void ribbon_layout(void)
{
    int x, i;
    Item *it;
    g_nitems = 0; g_nseps = 0; g_hot = -1;
    add(CMD_FILE_MENU, K_FILETAB, 0, L"File", 0, 0, 56, 24);
    add(CMD_TAB_HOME, K_TAB, 0, L"Home", 58, 0, 62, 24);
    add(CMD_TAB_VIEW, K_TAB, 0, L"View", 122, 0, 62, 24);
    g_group_x = 4;
    if (g_tab == 0)
    {
        /* Clipboard */
        x = g_group_x;
        it = add(CMD_PASTE, K_BIG, G_PASTE, L"Paste", x, 28, 44, 68); it->drop = CMD_PASTEFROM;
        add(CMD_CUT, K_SMALL, G_CUT, L"Cut", x + 46, 30, 56, 22);
        add(CMD_COPY, K_SMALL, G_COPY, L"Copy", x + 46, 53, 56, 22);
        group_end(L"Clipboard", 104);
        /* Image */
        x = g_group_x;
        it = add(CMD_TOOL_BASE + (g_tool == T_FREESEL ? T_FREESEL : T_SELECT), K_BIG, g_tool == T_FREESEL ? G_FREESEL : G_SELECT,
                 L"Select", x, 28, 46, 68);
        it->drop = CMD_SELECT_MENU;
        add(CMD_CROP, K_SMALL, G_CROP, L"Crop", x + 48, 30, 68, 22);
        add(CMD_RESIZE, K_SMALL, G_RESIZE, L"Resize", x + 48, 53, 68, 22);
        it = add(CMD_ROTATE_MENU, K_SMALL, G_ROTATE, L"Rotate", x + 48, 76, 68, 22); it->drop = CMD_ROTATE_MENU;
        group_end(L"Image", 118);
        /* Tools */
        x = g_group_x;
        {
            static const int tools[6] = { T_PENCIL, T_FILL, T_TEXT, T_ERASER, T_PICKER, T_MAGNIFIER };
            static const int glyphs[6] = { G_PENCIL, G_FILL, G_TEXT, G_ERASER, G_PICKER, G_MAGNIFIER };
            static const WCHAR *names[6] = { L"Pencil", L"Fill with color", L"Text", L"Eraser", L"Color picker", L"Magnifier" };
            for (i = 0; i < 6; i++) add(CMD_TOOL_BASE + tools[i], K_ICON, glyphs[i], names[i], x + 4 + (i % 3) * 26, 34 + (i / 3) * 30, 24, 24);
        }
        group_end(L"Tools", 84);
        if (text_active())
        {
            /* the text tool's own group, where Brushes and Shapes were */
            x = g_group_x;
            add(CMD_TEXT_FONT, K_BIG, G_TEXT, L"Font", x, 28, 46, 68);
            add(CMD_TEXT_OPAQUE, K_CHECK, 0, L"Opaque", x + 50, 34, 90, 22);
            add(CMD_TEXT_TRANSPARENT, K_CHECK, 0, L"Transparent", x + 50, 60, 90, 22);
            group_end(L"Text", 144);
        }
        else
        {
            /* Brushes */
            x = g_group_x;
            it = add(CMD_BRUSH_BASE + g_brush, K_BIG, BRUSH_GLYPHS[g_brush], L"Brushes", x, 28, 52, 68); it->drop = CMD_BRUSH_MENU;
            group_end(L"Brushes", 52);
            /* Shapes: a gallery, and Outline and Fill */
            x = g_group_x;
            for (i = 0; i < S_COUNT; i++)
                add(CMD_SHAPE_BASE + i, K_ICON, G_SHAPE_BASE + i, SHAPE_NAMES[i], x + 2 + (i % 6) * 21, 30 + (i / 6) * 22, 20, 20);
            it = add(CMD_OUTLINE_MENU, K_SMALL, G_OUTLINE, L"Outline", x + 130, 32, 66, 22); it->drop = CMD_OUTLINE_MENU;
            it = add(CMD_FILL_MENU, K_SMALL, G_FILLSHAPE, L"Fill", x + 130, 58, 66, 22); it->drop = CMD_FILL_MENU;
            group_end(L"Shapes", 198);
        }
        /* Size */
        x = g_group_x;
        it = add(CMD_SIZE_MENU, K_BIG, G_SIZE, L"Size", x, 28, 42, 68); it->drop = CMD_SIZE_MENU;
        group_end(L"Size", 42);
        /* Colors */
        x = g_group_x;
        add(CMD_COLOR1, K_SWATCH, 0, L"Color 1", x, 28, 42, 68);
        add(CMD_COLOR2, K_SWATCH, 1, L"Color 2", x + 43, 28, 42, 68);
        for (i = 0; i < 30; i++) add(CMD_PALETTE_BASE + i, K_PAL, 0, NULL, x + 89 + (i % 10) * 20, 31 + (i / 10) * 21, 19, 19);
        add(CMD_EDITCOLORS, K_BIG, G_EDITCOLORS, L"Edit colors", x + 291, 28, 60, 68);
        group_end(L"Colors", 352);
    }
    else
    {
        x = g_group_x;
        add(CMD_ZOOMIN, K_BIG, G_ZOOMIN, L"Zoom in", x, 28, 50, 68);
        add(CMD_ZOOMOUT, K_BIG, G_ZOOMOUT, L"Zoom out", x + 52, 28, 54, 68);
        add(CMD_ZOOM100, K_BIG, G_ZOOM100, L"100%", x + 108, 28, 46, 68);
        group_end(L"Zoom", 156);
        x = g_group_x;
        add(CMD_GRID, K_CHECK, 0, L"Gridlines", x + 4, 34, 100, 22);
        add(CMD_STATUSBAR, K_CHECK, 0, L"Status bar", x + 4, 60, 100, 22);
        group_end(L"Show or hide", 108);
        x = g_group_x;
        add(CMD_FULLSCREEN, K_BIG, G_FULLSCREEN, L"Full screen", x, 28, 58, 68);
        group_end(L"Display", 60);
    }
    (void)text_w;
    if (g_ribbon) InvalidateRect(g_ribbon, NULL, FALSE);
}

static BOOL checked(const Item *it)
{
    int c = it->cmd;
    if (c >= CMD_TOOL_BASE && c < CMD_TOOL_BASE + T_COUNT) return g_tool == c - CMD_TOOL_BASE;
    if (c >= CMD_BRUSH_BASE && c < CMD_BRUSH_BASE + B_COUNT) return g_tool == T_BRUSH;
    if (c >= CMD_SHAPE_BASE && c < CMD_SHAPE_BASE + S_COUNT) return g_tool == T_SHAPE && g_shape == c - CMD_SHAPE_BASE;
    if (c == CMD_COLOR1) return g_color_sel == 0;
    if (c == CMD_COLOR2) return g_color_sel == 1;
    if (c == CMD_TAB_HOME) return g_tab == 0;
    if (c == CMD_TAB_VIEW) return g_tab == 1;
    if (c == CMD_GRID) return g_grid;
    if (c == CMD_STATUSBAR) return g_statusbar_on;
    if (c == CMD_TEXT_OPAQUE) return g_text_opaque;
    if (c == CMD_TEXT_TRANSPARENT) return !g_text_opaque;
    return FALSE;
}

static void fill(HDC dc, const RECT *r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FillRect(dc, r, b); DeleteObject(b); }
static void frame(HDC dc, const RECT *r, COLORREF c) { HBRUSH b = CreateSolidBrush(c); FrameRect(dc, r, b); DeleteObject(b); }

static void arrow(HDC dc, int cx, int cy)
{
    POINT p[3] = { { cx - S(3), cy - S(1) }, { cx + S(3), cy - S(1) }, { cx, cy + S(2) } };
    HBRUSH b = CreateSolidBrush(TEXT_GREY);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, p, 3);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
}

static void draw_item(HDC dc, int idx)
{
    Item *it = &g_items[idx];
    RECT r = it->rc, t;
    BOOL hot = idx == g_hot, on = checked(it);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font);
    switch (it->kind)
    {
    case K_FILETAB:
        fill(dc, &r, hot ? RGB(92, 34, 164) : ACCENT);
        SetTextColor(dc, RGB(255, 255, 255));
        DrawTextW(dc, it->label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    case K_TAB:
        if (on)
        {
            RECT b = r; b.bottom += 1;
            fill(dc, &b, RIBBON_BG);
            frame(dc, &b, LINE_GREY);
            b.top = b.bottom - 1; b.left++; b.right--; fill(dc, &b, RIBBON_BG);
        }
        else if (hot) fill(dc, &r, ACCENT_HOT);
        SetTextColor(dc, on ? (sgm_dark ? RGB(179, 139, 235) : ACCENT) : (sgm_dark ? RGB(235, 235, 240) : RGB(40, 40, 48)));
        DrawTextW(dc, it->label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    case K_GROUP:
        SetTextColor(dc, TEXT_GREY);
        SelectObject(dc, g_font_small);
        DrawTextW(dc, it->label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return;
    case K_PAL:
    {
        int i = it->cmd - CMD_PALETTE_BASE;
        RECT in = r;
        BOOL empty = i >= 20 && i - 20 >= g_ncustom;
        frame(dc, &r, hot ? ACCENT : (sgm_dark ? RGB(100, 100, 108) : RGB(160, 160, 168)));
        InflateRect(&in, -S(2), -S(2));
        if (!empty) fill(dc, &in, g_palette[i]);
        else fill(dc, &in, (sgm_dark ? RGB(50, 50, 50) : RGB(250, 250, 250)));
        return;
    }
    }
    if (on) { fill(dc, &r, ACCENT_DOWN); frame(dc, &r, ACCENT_EDGE); }
    else if (hot) { fill(dc, &r, ACCENT_HOT); frame(dc, &r, ACCENT_EDGE); }
    SetTextColor(dc, (sgm_dark ? RGB(235, 235, 240) : RGB(30, 30, 36)));
    switch (it->kind)
    {
    case K_BIG:
        glyph_draw(dc, it->glyph, r.left + (r.right - r.left - S(32)) / 2, r.top + S(4), S(32));
        t = r; t.top += S(38); t.bottom = t.top + S(16);
        DrawTextW(dc, it->label, -1, &t, DT_CENTER | DT_TOP | DT_SINGLELINE);
        if (it->drop) arrow(dc, (r.left + r.right) / 2, r.top + S(60));
        break;
    case K_SMALL:
        glyph_draw(dc, it->glyph, r.left + S(3), r.top + (r.bottom - r.top - S(16)) / 2, S(16));
        t = r; t.left += S(23);
        DrawTextW(dc, it->label, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (it->drop) arrow(dc, r.right - S(8), (r.top + r.bottom) / 2);
        break;
    case K_ICON:
    {
        int s = min(r.right - r.left, r.bottom - r.top) - S(6);
        glyph_draw(dc, it->glyph, r.left + (r.right - r.left - s) / 2, r.top + (r.bottom - r.top - s) / 2, s);
        break;
    }
    case K_CHECK:
    {
        RECT b = { r.left + S(4), r.top + (r.bottom - r.top - S(13)) / 2, r.left + S(17), r.top + (r.bottom - r.top - S(13)) / 2 + S(13) };
        fill(dc, &b, on ? ACCENT : (sgm_dark ? RGB(43, 43, 43) : RGB(255, 255, 255)));
        frame(dc, &b, on ? ACCENT : (sgm_dark ? RGB(150, 150, 158) : RGB(120, 120, 128)));
        if (on)
        {
            HPEN p = CreatePen(PS_SOLID, max(1, S(2)), RGB(255, 255, 255));
            HGDIOBJ op = SelectObject(dc, p);
            MoveToEx(dc, b.left + S(3), b.top + S(6), NULL); LineTo(dc, b.left + S(5), b.top + S(9)); LineTo(dc, b.left + S(10), b.top + S(3));
            SelectObject(dc, op); DeleteObject(p);
        }
        t = r; t.left += S(22);
        DrawTextW(dc, it->label, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        break;
    }
    case K_SWATCH:
    {
        RECT sw = { r.left + (r.right - r.left - S(30)) / 2, r.top + S(6), 0, 0 };
        sw.right = sw.left + S(30); sw.bottom = sw.top + S(30);
        frame(dc, &sw, RGB(120, 120, 128));
        InflateRect(&sw, -S(2), -S(2));
        fill(dc, &sw, it->glyph ? g_color2 : g_color1);
        t = r; t.top += S(40); t.bottom = r.bottom;
        DrawTextW(dc, it->label, -1, &t, DT_CENTER | DT_TOP | DT_WORDBREAK);
        break;
    }
    }
}

static void ribbon_paint(HWND hwnd, HDC dc)
{
    RECT rc, r;
    int i;
    GetClientRect(hwnd, &rc);
    fill(dc, &rc, (sgm_dark ? RGB(32, 32, 32) : RGB(255, 255, 255)));
    r = rc; r.top = S(24); fill(dc, &r, RIBBON_BG);
    r.bottom = r.top + 1; fill(dc, &r, LINE_GREY);
    r = rc; r.top = r.bottom - 1; fill(dc, &r, LINE_GREY);
    for (i = 0; i < g_nseps; i++) fill(dc, &g_seps[i], LINE_GREY);
    for (i = 0; i < g_nitems; i++) draw_item(dc, i);
}

static int hit(int x, int y)
{
    int i;
    POINT p = { x, y };
    for (i = 0; i < g_nitems; i++)
        if (g_items[i].kind != K_GROUP && PtInRect(&g_items[i].rc, p)) return i;
    return -1;
}

static int popup(HMENU m, const RECT *under)
{
    POINT p = { under->left, under->bottom };
    int r;
    ClientToScreen(g_ribbon, &p);
    r = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON, p.x, p.y, 0, g_main, NULL);
    DestroyMenu(m);
    return r;
}

static void menu_for(int which, const RECT *r)
{
    HMENU m = CreatePopupMenu(), sub;
    int i, cmd = 0;
    switch (which)
    {
    case CMD_FILE_MENU:
        AppendMenuW(m, MF_STRING, CMD_NEW, L"&New\tCtrl+N");
        AppendMenuW(m, MF_STRING, CMD_OPEN, L"&Open\tCtrl+O");
        AppendMenuW(m, MF_STRING, CMD_SAVE, L"&Save\tCtrl+S");
        sub = CreatePopupMenu();
        AppendMenuW(sub, MF_STRING, CMD_SAVEAS_PNG, L"&PNG picture");
        AppendMenuW(sub, MF_STRING, CMD_SAVEAS_JPEG, L"&JPEG picture");
        AppendMenuW(sub, MF_STRING, CMD_SAVEAS_BMP, L"&BMP picture");
        AppendMenuW(sub, MF_STRING, CMD_SAVEAS_GIF, L"&GIF picture");
        AppendMenuW(sub, MF_STRING, CMD_SAVEAS_TIFF, L"&TIFF picture");
        AppendMenuW(sub, MF_SEPARATOR, 0, NULL);
        AppendMenuW(sub, MF_STRING, CMD_SAVEAS, L"&Other formats...\tF12");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"Save &as");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, CMD_PROPERTIES, L"Prop&erties\tCtrl+E");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, CMD_ABOUT, L"A&bout Paint");
        AppendMenuW(m, MF_STRING, CMD_EXIT, L"E&xit");
        break;
    case CMD_PASTEFROM:
        AppendMenuW(m, MF_STRING, CMD_PASTE, L"&Paste\tCtrl+V");
        AppendMenuW(m, MF_STRING, CMD_PASTEFROM, L"Paste &from...");
        break;
    case CMD_SELECT_MENU:
        AppendMenuW(m, MF_STRING | (g_tool == T_SELECT ? MF_CHECKED : 0), CMD_TOOL_BASE + T_SELECT, L"&Rectangular selection");
        AppendMenuW(m, MF_STRING | (g_tool == T_FREESEL ? MF_CHECKED : 0), CMD_TOOL_BASE + T_FREESEL, L"&Free-form selection");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, CMD_SELECTALL, L"Select &all\tCtrl+A");
        AppendMenuW(m, MF_STRING, CMD_INVERTSEL, L"&Invert selection");
        AppendMenuW(m, MF_STRING, CMD_DELETE, L"&Delete\tDel");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (g_transparent_sel ? MF_CHECKED : 0), CMD_TRANSPARENT, L"&Transparent selection");
        break;
    case CMD_ROTATE_MENU:
        AppendMenuW(m, MF_STRING, CMD_ROT_R, L"Rotate &right 90\x00b0");
        AppendMenuW(m, MF_STRING, CMD_ROT_L, L"Rotate &left 90\x00b0");
        AppendMenuW(m, MF_STRING, CMD_ROT_180, L"Rotate &180\x00b0");
        AppendMenuW(m, MF_STRING, CMD_FLIP_V, L"Flip &vertical");
        AppendMenuW(m, MF_STRING, CMD_FLIP_H, L"Flip &horizontal");
        break;
    case CMD_BRUSH_MENU:
        for (i = 0; i < B_COUNT; i++)
            AppendMenuW(m, MF_STRING | (g_brush == i ? MF_CHECKED : 0), CMD_BRUSH_BASE + i, BRUSH_NAMES[i]);
        break;
    case CMD_SIZE_MENU:
        for (i = 0; i < 4; i++)
        {
            static const WCHAR *names[4] = { L"&1px", L"&3px", L"&5px", L"&8px" };
            AppendMenuW(m, MF_STRING | (g_size_idx == i ? MF_CHECKED : 0), CMD_SIZE_BASE + i, names[i]);
        }
        break;
    case CMD_OUTLINE_MENU:
        AppendMenuW(m, MF_STRING | (!g_outline_on ? MF_CHECKED : 0), CMD_OUTLINE_NONE, L"&No outline");
        AppendMenuW(m, MF_STRING | (g_outline_on ? MF_CHECKED : 0), CMD_OUTLINE_SOLID, L"&Solid color");
        break;
    case CMD_FILL_MENU:
        AppendMenuW(m, MF_STRING | (!g_fill_on ? MF_CHECKED : 0), CMD_FILL_NONE, L"&No fill");
        AppendMenuW(m, MF_STRING | (g_fill_on ? MF_CHECKED : 0), CMD_FILL_SOLID, L"&Solid color");
        break;
    }
    cmd = popup(m, r);
    if (cmd) do_command(cmd);
}

static void click(int idx, BOOL right, int y)
{
    Item *it = &g_items[idx];
    int c = it->cmd;
    if (it->kind == K_PAL)
    {
        int i = c - CMD_PALETTE_BASE;
        if (i >= 20 && i - 20 >= g_ncustom) { do_command(CMD_EDITCOLORS); return; }
        if (right || g_color_sel == 1) g_color2 = g_palette[i]; else g_color1 = g_palette[i];
        InvalidateRect(g_ribbon, NULL, FALSE);
        write_dump();
        return;
    }
    if (c == CMD_COLOR1 || c == CMD_COLOR2) { g_color_sel = c == CMD_COLOR2; InvalidateRect(g_ribbon, NULL, FALSE); write_dump(); return; }
    if (c == CMD_FILE_MENU || c == CMD_SELECT_MENU || c == CMD_ROTATE_MENU || c == CMD_BRUSH_MENU || c == CMD_SIZE_MENU ||
        c == CMD_OUTLINE_MENU || c == CMD_FILL_MENU)
    {
        menu_for(c, &it->rc);
        return;
    }
    /* a big button's lower part opens its menu */
    if (it->drop && it->kind == K_BIG && y >= it->rc.top + S(38)) { menu_for(it->drop == CMD_PASTEFROM ? CMD_PASTEFROM : it->drop, &it->rc); return; }
    do_command(c);
}

void ribbon_dump(FILE *f)
{
    int i;
    for (i = 0; i < g_nitems; i++)
    {
        RECT r = g_items[i].rc;
        if (g_items[i].kind == K_GROUP) continue;
        MapWindowPoints(g_ribbon, NULL, (POINT *)&r, 2);
        fprintf(f, "item %d %ld %ld %ld %ld %ls\n", g_items[i].cmd, r.left, r.top, r.right, r.bottom,
                g_items[i].label ? g_items[i].label : L"-");
    }
}

LRESULT CALLBACK ribbon_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(hwnd, &ps), mem = CreateCompatibleDC(dc);
        HBITMAP bb;
        GetClientRect(hwnd, &rc);
        bb = CreateCompatibleBitmap(dc, max(1, (int)rc.right), max(1, (int)rc.bottom));
        SelectObject(mem, bb);
        ribbon_paint(hwnd, mem);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        DeleteDC(mem); DeleteObject(bb);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE:
    {
        int h = hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h != g_hot)
        {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&t);
            g_hot = h;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE: g_hot = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:
        g_down = hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP:
    {
        int h = hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h >= 0 && h == g_down) click(h, msg == WM_RBUTTONUP, GET_Y_LPARAM(lp));
        g_down = -1;
        SetFocus(g_canvas);
        return 0;
    }
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the status bar ---- */
static RECT g_zminus, g_zplus, g_ztrack;
static const int ZLEVELS[] = { 125, 250, 500, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000 };

LRESULT CALLBACK status_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc, t;
        WCHAR s[64];
        int x, y, in, i, pos = 0;
        HDC dc = BeginPaint(hwnd, &ps), mem = CreateCompatibleDC(dc);
        HBITMAP bb;
        GetClientRect(hwnd, &rc);
        bb = CreateCompatibleBitmap(dc, max(1, (int)rc.right), max(1, (int)rc.bottom));
        SelectObject(mem, bb);
        fill(mem, &rc, (sgm_dark ? RGB(43, 43, 43) : RGB(243, 243, 246)));
        t = rc; t.bottom = 1; fill(mem, &t, LINE_GREY);
        SelectObject(mem, g_font);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, (sgm_dark ? RGB(235, 235, 240) : RGB(40, 40, 48)));
        canvas_cursor_info(&x, &y, &in);
        t = rc; t.left = S(10); t.right = S(170);
        if (in) { _snwprintf(s, ARRAYSIZE(s), L"\x2316  %d, %dpx", x, y); DrawTextW(mem, s, -1, &t, DT_VCENTER | DT_SINGLELINE); }
        if (sel_active())
        {
            RECT r;
            sel_rect(&r);
            t.left = S(180); t.right = S(340);
            _snwprintf(s, ARRAYSIZE(s), L"\x2b1a  %ld \x00d7 %ldpx", r.right - r.left, r.bottom - r.top);
            DrawTextW(mem, s, -1, &t, DT_VCENTER | DT_SINGLELINE);
        }
        t.left = S(350); t.right = S(520);
        _snwprintf(s, ARRAYSIZE(s), L"\x25a1  %d \x00d7 %dpx", g_img.w, g_img.h);
        DrawTextW(mem, s, -1, &t, DT_VCENTER | DT_SINGLELINE);
        /* zoom: percentage, -, a slider over the levels, + */
        t = rc; t.right -= S(8); t.left = t.right - S(16);
        g_zplus = t;
        t.right = t.left - S(4); t.left = t.right - S(110); g_ztrack = t;
        t.right = t.left - S(4); t.left = t.right - S(16); g_zminus = t;
        t.right = t.left - S(6); t.left = t.right - S(50);
        if (g_zoom % 1000 == 0 || g_zoom >= 1000) _snwprintf(s, ARRAYSIZE(s), L"%d%%", g_zoom / 10);
        else _snwprintf(s, ARRAYSIZE(s), L"%d.%d%%", g_zoom / 10, g_zoom % 10);
        DrawTextW(mem, s, -1, &t, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(mem, L"\x2212", -1, &g_zminus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawTextW(mem, L"+", -1, &g_zplus, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        {
            RECT line = g_ztrack, th;
            line.top = (rc.bottom - S(2)) / 2; line.bottom = line.top + S(2);
            fill(mem, &line, (sgm_dark ? RGB(90, 90, 98) : RGB(160, 160, 168)));
            for (i = 0; i < (int)ARRAYSIZE(ZLEVELS); i++) if (ZLEVELS[i] <= g_zoom) pos = i;
            x = g_ztrack.left + (g_ztrack.right - g_ztrack.left) * pos / ((int)ARRAYSIZE(ZLEVELS) - 1);
            SetRect(&th, x - S(3), (rc.bottom - S(14)) / 2, x + S(3), (rc.bottom - S(14)) / 2 + S(14));
            fill(mem, &th, ACCENT);
        }
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        DeleteDC(mem); DeleteObject(bb);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN:
    {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (PtInRect(&g_zminus, p)) do_command(CMD_ZOOMOUT);
        else if (PtInRect(&g_zplus, p)) do_command(CMD_ZOOMIN);
        else if (PtInRect(&g_ztrack, p))
        {
            int n = ARRAYSIZE(ZLEVELS) - 1, i = ((p.x - g_ztrack.left) * n + (g_ztrack.right - g_ztrack.left) / 2) / (g_ztrack.right - g_ztrack.left);
            canvas_zoom(ZLEVELS[max(0, min(n, i))], -1, -1);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
