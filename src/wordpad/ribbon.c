/* sg-wordpad -- the ribbon (File, Home, View) and the status bar, drawn here.
 *
 * As Paint's: a list of items laid out in 96-dpi units on every change of
 * tab; drawing, hover, clicks and the dump all walk the same list. The font
 * name and size are real combo boxes (children of the ribbon) placed where
 * the layout says.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "wordpad.h"

enum { K_TAB, K_FILETAB, K_BIG, K_SMALL, K_ICON, K_GROUP, K_CHECK, K_LAUNCH, K_COMBO };

typedef struct {
    int cmd, kind, glyph, drop;         /* drop: the command for the arrow part */
    const WCHAR *label;
    RECT rc;
} Item;

static Item g_items[96];
static int g_nitems, g_hot = -1, g_down = -1;
static int g_group_x;
static RECT g_seps[12];
static int g_nseps;
static WNDPROC g_combo_edit_proc;

int ribbon_height(void) { return S(122); }
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

static void group_end(const WCHAR *label, int width, int launcher)
{
    add(0, K_GROUP, 0, label, g_group_x, 102, width, 16);
    if (launcher) add(launcher, K_LAUNCH, G_LAUNCHER, L"More options", g_group_x + width - 14, 104, 12, 12);
    g_group_x += width;
    if (g_nseps < (int)ARRAYSIZE(g_seps)) SetRect(&g_seps[g_nseps++], S(g_group_x + 2), S(30), S(g_group_x + 3), S(116));
    g_group_x += 6;
}

BOOL ribbon_item_rect(int cmd, RECT *r)
{
    for (int i = 0; i < g_nitems; i++)
        if (g_items[i].cmd == cmd && g_items[i].kind != K_GROUP) { *r = g_items[i].rc; return TRUE; }
    return FALSE;
}

void ribbon_layout(void)
{
    int x;
    Item *it;
    g_nitems = 0; g_nseps = 0; g_hot = -1;
    add(CMD_FILE_MENU, K_FILETAB, 0, L"File", 0, 0, 56, 26);
    add(CMD_TAB_HOME, K_TAB, 0, L"Home", 58, 0, 62, 26);
    add(CMD_TAB_VIEW, K_TAB, 0, L"View", 122, 0, 62, 26);
    g_group_x = 4;
    if (g_tab == 0)
    {
        /* Clipboard */
        x = g_group_x;
        it = add(CMD_PASTE, K_BIG, G_PASTE, L"Paste", x, 30, 46, 70); it->drop = CMD_PASTE_MENU;
        add(CMD_CUT, K_SMALL, G_CUT, L"Cut", x + 48, 32, 58, 22);
        add(CMD_COPY, K_SMALL, G_COPY, L"Copy", x + 48, 56, 58, 22);
        group_end(L"Clipboard", 108, 0);
        /* Font: the two boxes, grow/shrink; then the effects and colours */
        x = g_group_x;
        add(CMD_FONTFACE, K_COMBO, 0, L"Font family", x + 2, 32, 148, 22);
        add(CMD_FONTSIZE, K_COMBO, 0, L"Font size", x + 152, 32, 46, 22);
        add(CMD_GROW, K_ICON, G_GROW, L"Grow font", x + 200, 32, 24, 22);
        add(CMD_SHRINK, K_ICON, G_SHRINK, L"Shrink font", x + 226, 32, 24, 22);
        add(CMD_BOLD, K_ICON, G_BOLD, L"Bold", x + 2, 62, 24, 24);
        add(CMD_ITALIC, K_ICON, G_ITALIC, L"Italic", x + 28, 62, 24, 24);
        add(CMD_UNDERLINE, K_ICON, G_UNDERLINE, L"Underline", x + 54, 62, 24, 24);
        add(CMD_STRIKE, K_ICON, G_STRIKE, L"Strikethrough", x + 80, 62, 24, 24);
        add(CMD_SUBSCRIPT, K_ICON, G_SUB, L"Subscript", x + 106, 62, 24, 24);
        add(CMD_SUPERSCRIPT, K_ICON, G_SUPER, L"Superscript", x + 132, 62, 24, 24);
        it = add(CMD_HIGHLIGHT, K_ICON, G_HIGHLIGHT, L"Text highlight color", x + 162, 62, 38, 24); it->drop = CMD_HIGHLIGHT_MENU;
        it = add(CMD_COLOR, K_ICON, G_COLOR, L"Text color", x + 204, 62, 38, 24); it->drop = CMD_COLOR_MENU;
        group_end(L"Font", 254, CMD_FONTDLG);
        /* Paragraph */
        x = g_group_x;
        add(CMD_INDENT_LESS, K_ICON, G_INDENT_LESS, L"Decrease indent", x + 2, 32, 24, 24);
        add(CMD_INDENT_MORE, K_ICON, G_INDENT_MORE, L"Increase indent", x + 28, 32, 24, 24);
        it = add(CMD_LIST, K_ICON, G_LIST, L"Start a list", x + 56, 32, 38, 24); it->drop = CMD_LIST_MENU;
        it = add(CMD_SPACING_MENU, K_ICON, G_SPACING, L"Line spacing", x + 98, 32, 38, 24); it->drop = CMD_SPACING_MENU;
        add(CMD_ALIGN_LEFT, K_ICON, G_ALEFT, L"Align text left", x + 2, 62, 24, 24);
        add(CMD_ALIGN_CENTER, K_ICON, G_ACENTER, L"Center", x + 28, 62, 24, 24);
        add(CMD_ALIGN_RIGHT, K_ICON, G_ARIGHT, L"Align text right", x + 54, 62, 24, 24);
        add(CMD_ALIGN_JUSTIFY, K_ICON, G_AJUSTIFY, L"Justify", x + 80, 62, 24, 24);
        add(CMD_PARADLG, K_ICON, G_PARA, L"Paragraph", x + 112, 62, 24, 24);
        group_end(L"Paragraph", 140, CMD_PARADLG);
        /* Insert */
        x = g_group_x;
        add(CMD_PICTURE, K_BIG, G_PICTURE, L"Picture", x, 30, 52, 70);
        add(CMD_TABLE, K_BIG, G_TABLE, L"Table", x + 54, 30, 48, 70);
        add(CMD_DATETIME, K_BIG, G_DATETIME, L"Date and time", x + 104, 30, 62, 70);
        group_end(L"Insert", 168, 0);
        /* Editing */
        x = g_group_x;
        add(CMD_FIND, K_SMALL, G_FIND, L"Find", x, 32, 90, 22);
        add(CMD_REPLACE, K_SMALL, G_REPLACE, L"Replace", x, 56, 90, 22);
        add(CMD_SELECTALL, K_SMALL, G_SELECTALL, L"Select all", x, 80, 90, 22);
        group_end(L"Editing", 92, 0);
    }
    else
    {
        x = g_group_x;
        add(CMD_ZOOMIN, K_BIG, G_ZOOMIN, L"Zoom in", x, 30, 50, 70);
        add(CMD_ZOOMOUT, K_BIG, G_ZOOMOUT, L"Zoom out", x + 52, 30, 54, 70);
        add(CMD_ZOOM100, K_BIG, G_ZOOM100, L"100%", x + 108, 30, 46, 70);
        group_end(L"Zoom", 156, 0);
        x = g_group_x;
        add(CMD_RULER, K_CHECK, 0, L"Ruler", x + 4, 36, 100, 22);
        add(CMD_STATUSBAR, K_CHECK, 0, L"Status bar", x + 4, 62, 100, 22);
        group_end(L"Show or hide", 108, 0);
        x = g_group_x;
        it = add(CMD_WRAP_MENU, K_BIG, G_WRAP, L"Word wrap", x, 30, 60, 70); it->drop = CMD_WRAP_MENU;
        it = add(CMD_UNITS_MENU, K_BIG, G_UNITS, L"Measurement units", x + 62, 30, 80, 70); it->drop = CMD_UNITS_MENU;
        group_end(L"Settings", 144, 0);
    }
    /* the combo boxes follow their items */
    {
        RECT r;
        BOOL home = g_tab == 0;
        if (home && ribbon_item_rect(CMD_FONTFACE, &r)) MoveWindow(g_face, r.left, r.top, r.right - r.left, S(300), TRUE);
        if (home && ribbon_item_rect(CMD_FONTSIZE, &r)) MoveWindow(g_size, r.left, r.top, r.right - r.left, S(300), TRUE);
        ShowWindow(g_face, home ? SW_SHOWNA : SW_HIDE);
        ShowWindow(g_size, home ? SW_SHOWNA : SW_HIDE);
    }
}

void ribbon_dump(FILE *f)
{
    for (int i = 0; i < g_nitems; i++)
    {
        const Item *it = &g_items[i];
        POINT a = { it->rc.left, it->rc.top }, b = { it->rc.right, it->rc.bottom };
        if (it->kind == K_GROUP) continue;
        ClientToScreen(g_ribbon, &a); ClientToScreen(g_ribbon, &b);
        fprintf(f, "item %d %ld %ld %ld %ld %d %ls\n", it->cmd, a.x, a.y, b.x, b.y, ribbon_is_on(it->cmd), it->label);
    }
}

/* ---------------------------------------------------------------- drawing */

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, r, b);
    DeleteObject(b);
}

static void frame(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FrameRect(dc, r, b);
    DeleteObject(b);
}

static void draw_text(HDC dc, const WCHAR *s, RECT r, UINT fmt, COLORREF c, HFONT f)
{
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(dc, of);
}

static void arrow(HDC dc, int cx, int cy, COLORREF c)
{
    POINT p[3] = { { cx - S(3), cy - S(1) }, { cx + S(3), cy - S(1) }, { cx, cy + S(2) } };
    HBRUSH b = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, p, 3);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
}

static void draw_item(HDC dc, const Item *it, int idx)
{
    RECT r = it->rc;
    BOOL hot = idx == g_hot, down = idx == g_down, on = ribbon_is_on(it->cmd);
    int gs;
    switch (it->kind)
    {
    case K_FILETAB:
        fill(dc, &r, hot ? RGB(130, 70, 206) : ACCENT);
        draw_text(dc, it->label, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE, RGB(255, 255, 255), g_font);
        return;
    case K_TAB:
        if (on)
        {
            RECT b = r;
            fill(dc, &r, RIBBON_BG);
            b.bottom = b.top + 1; fill(dc, &b, LINE_GREY);
            b = r; b.right = b.left + 1; fill(dc, &b, LINE_GREY);
            b = r; b.left = b.right - 1; fill(dc, &b, LINE_GREY);
        }
        else if (hot) fill(dc, &r, RGB(240, 240, 244));
        draw_text(dc, it->label, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE, on ? ACCENT : RGB(40, 40, 48), g_font);
        return;
    case K_GROUP:
        draw_text(dc, it->label, r, DT_CENTER | DT_VCENTER | DT_SINGLELINE, TEXT_GREY, g_font_small);
        return;
    case K_COMBO:
        return;
    default:
        break;
    }
    if (on && it->kind != K_CHECK) { fill(dc, &r, ACCENT_DOWN); frame(dc, &r, ACCENT_EDGE); }
    else if (down) { fill(dc, &r, ACCENT_DOWN); frame(dc, &r, ACCENT_EDGE); }
    else if (hot) { fill(dc, &r, ACCENT_HOT); frame(dc, &r, ACCENT_EDGE); }
    switch (it->kind)
    {
    case K_BIG:
    {
        RECT t = r;
        gs = S(32);
        glyph_draw(dc, it->glyph, (r.left + r.right - gs) / 2, r.top + S(4), gs);
        t.top = r.top + S(40);
        if (it->drop) { t.bottom -= S(8); }
        draw_text(dc, it->label, t, DT_CENTER | DT_WORDBREAK, RGB(30, 30, 38), g_font_small);
        if (it->drop) arrow(dc, (r.left + r.right) / 2, r.bottom - S(6), RGB(60, 60, 70));
        if (it->drop && it->drop != it->cmd && (hot || down))
        {
            RECT l = r; l.top = r.top + S(38); l.bottom = l.top + 1; fill(dc, &l, ACCENT_EDGE);
        }
        break;
    }
    case K_SMALL:
    {
        RECT t = r;
        gs = S(16);
        glyph_draw(dc, it->glyph, r.left + S(3), (r.top + r.bottom - gs) / 2, gs);
        t.left += S(23);
        draw_text(dc, it->label, t, DT_LEFT | DT_VCENTER | DT_SINGLELINE, RGB(30, 30, 38), g_font_small);
        break;
    }
    case K_ICON:
    case K_LAUNCH:
    {
        int w = r.right - r.left, h = r.bottom - r.top, iw = it->drop ? w - S(12) : w;
        gs = it->kind == K_LAUNCH ? S(10) : S(18);
        glyph_draw(dc, it->glyph, r.left + (iw - gs) / 2, r.top + (h - gs) / 2 - (it->cmd == CMD_COLOR || it->cmd == CMD_HIGHLIGHT ? S(2) : 0), gs);
        if (it->cmd == CMD_COLOR || it->cmd == CMD_HIGHLIGHT)
        {
            /* the colour the button applies, as a bar under the glyph */
            RECT bar = { r.left + (iw - gs) / 2, r.bottom - S(6), r.left + (iw + gs) / 2, r.bottom - S(2) };
            BOOL none = it->cmd == CMD_COLOR ? g_text_auto : g_hl_none;
            COLORREF c = it->cmd == CMD_COLOR ? g_text_color : g_hl_color;
            if (none) frame(dc, &bar, TEXT_GREY); else fill(dc, &bar, c);
        }
        if (it->drop)
        {
            if (hot || down) { RECT l = { r.right - S(12), r.top + S(2), r.right - S(11), r.bottom - S(2) }; fill(dc, &l, ACCENT_EDGE); }
            arrow(dc, r.right - S(6), (r.top + r.bottom) / 2, RGB(60, 60, 70));
        }
        break;
    }
    case K_CHECK:
    {
        RECT b = { r.left + S(2), (r.top + r.bottom) / 2 - S(7), r.left + S(16), (r.top + r.bottom) / 2 + S(7) }, t = r;
        fill(dc, &b, on ? ACCENT : RGB(255, 255, 255));
        frame(dc, &b, on ? ACCENT : RGB(120, 120, 130));
        if (on)
        {
            HPEN p = CreatePen(PS_SOLID, S(2), RGB(255, 255, 255));
            HGDIOBJ op = SelectObject(dc, p);
            MoveToEx(dc, b.left + S(3), b.top + S(7), NULL);
            LineTo(dc, b.left + S(6), b.top + S(10));
            LineTo(dc, b.left + S(11), b.top + S(4));
            SelectObject(dc, op); DeleteObject(p);
        }
        t.left = b.right + S(6);
        draw_text(dc, it->label, t, DT_LEFT | DT_VCENTER | DT_SINGLELINE, RGB(30, 30, 38), g_font_small);
        break;
    }
    }
}

static void paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps), mem;
    RECT rc, band;
    HBITMAP bmp;
    HGDIOBJ ob;
    GetClientRect(h, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    ob = SelectObject(mem, bmp);
    band = rc; band.bottom = S(26);
    fill(mem, &band, RGB(255, 255, 255));
    band.top = band.bottom; band.bottom = rc.bottom;
    fill(mem, &band, RIBBON_BG);
    band.top = rc.bottom - 1; fill(mem, &band, LINE_GREY);
    band = rc; band.top = S(26) - 1; band.bottom = S(26); fill(mem, &band, LINE_GREY);
    for (int i = 0; i < g_nitems; i++) draw_item(mem, &g_items[i], i);
    for (int i = 0; i < g_nseps; i++) fill(mem, &g_seps[i], LINE_GREY);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
    EndPaint(h, &ps);
}

static int hit(int x, int y)
{
    POINT p = { x, y };
    for (int i = 0; i < g_nitems; i++)
        if (g_items[i].kind != K_GROUP && g_items[i].kind != K_COMBO && PtInRect(&g_items[i].rc, p)) return i;
    return -1;
}

/* which command a click at (x, y) on item i means: the arrow part runs drop */
static int click_cmd(int i, int x, int y)
{
    const Item *it = &g_items[i];
    if (!it->drop) return it->cmd;
    if (it->kind == K_BIG) return y >= it->rc.top + S(38) ? it->drop : it->cmd;
    return x >= it->rc.right - S(12) ? it->drop : it->cmd;
}

/* ---------------------------------------------------------------- the combo boxes */

static int CALLBACK font_cb(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM l)
{
    (void)tm; (void)type;
    if (lf->lfFaceName[0] == '@') return 1;
    if (SendMessageW((HWND)l, CB_FINDSTRINGEXACT, -1, (LPARAM)lf->lfFaceName) == CB_ERR)
        SendMessageW((HWND)l, CB_ADDSTRING, 0, (LPARAM)lf->lfFaceName);
    return 1;
}

static LRESULT CALLBACK combo_edit_sub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    extern void apply_font_face(const WCHAR *face);
    extern void apply_font_size(const WCHAR *s);
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB))
    {
        HWND combo = GetParent(h);
        if (w == VK_RETURN)
        {
            WCHAR t[64];
            GetWindowTextW(combo, t, ARRAYSIZE(t));
            if (combo == g_face) apply_font_face(t); else apply_font_size(t);
        }
        else refresh_state();
        SetFocus(g_edit);
        return 0;
    }
    if (m == WM_CHAR && (w == '\r' || w == 27 || w == '\t')) return 0;
    return CallWindowProcW(g_combo_edit_proc, h, m, w, l);
}

void ribbon_create_controls(void)
{
    static const WCHAR *sizes[] = { L"8", L"9", L"10", L"11", L"12", L"14", L"16", L"18", L"20", L"22", L"24", L"26",
                                    L"28", L"36", L"48", L"72" };
    LOGFONTW lf;
    HDC dc;
    COMBOBOXINFO ci = { sizeof(ci) };
    g_face = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWN | CBS_SORT | CBS_AUTOHSCROLL,
                             0, 0, 0, 0, g_ribbon, (HMENU)CMD_FONTFACE, g_inst, NULL);
    g_size = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL,
                             0, 0, 0, 0, g_ribbon, (HMENU)CMD_FONTSIZE, g_inst, NULL);
    SendMessageW(g_face, WM_SETFONT, (WPARAM)g_font, 0);
    SendMessageW(g_size, WM_SETFONT, (WPARAM)g_font, 0);
    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    dc = GetDC(NULL);
    EnumFontFamiliesExW(dc, &lf, font_cb, (LPARAM)g_face, 0);
    ReleaseDC(NULL, dc);
    for (int i = 0; i < (int)ARRAYSIZE(sizes); i++) SendMessageW(g_size, CB_ADDSTRING, 0, (LPARAM)sizes[i]);
    SendMessageW(g_face, CB_SETMINVISIBLE, 16, 0);
    if (GetComboBoxInfo(g_face, &ci) && ci.hwndItem)
        g_combo_edit_proc = (WNDPROC)SetWindowLongPtrW(ci.hwndItem, GWLP_WNDPROC, (LONG_PTR)combo_edit_sub);
    ci.cbSize = sizeof(ci);
    if (GetComboBoxInfo(g_size, &ci) && ci.hwndItem)
        SetWindowLongPtrW(ci.hwndItem, GWLP_WNDPROC, (LONG_PTR)combo_edit_sub);
}

void combo_notify(HWND combo, int code)
{
    extern void apply_font_face(const WCHAR *face);
    extern void apply_font_size(const WCHAR *s);
    if (code == CBN_SELENDOK)
    {
        WCHAR t[64];
        int i = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
        if (i < 0) return;
        SendMessageW(combo, CB_GETLBTEXT, i, (LPARAM)t);
        if (combo == g_face) apply_font_face(t); else apply_font_size(t);
        SetFocus(g_edit);
    }
}

LRESULT CALLBACK ribbon_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_PAINT: paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE:
    {
        int i = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (i != g_hot)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            g_hot = i;
            TrackMouseEvent(&tme);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hot = -1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        g_down = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (g_down >= 0 && (g_items[g_down].kind == K_TAB || g_items[g_down].kind == K_FILETAB))
        {
            int cmd = g_items[g_down].cmd;
            g_down = -1;
            do_command(cmd);
            return 0;
        }
        SetCapture(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
    {
        int i = hit(GET_X_LPARAM(l), GET_Y_LPARAM(l)), d = g_down;
        ReleaseCapture();
        g_down = -1;
        InvalidateRect(h, NULL, FALSE);
        if (i >= 0 && i == d) do_command(click_cmd(i, GET_X_LPARAM(l), GET_Y_LPARAM(l)));
        return 0;
    }
    case WM_COMMAND:
        return SendMessageW(g_main, m, w, l);
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---------------------------------------------------------------- the status bar */

static RECT g_zminus, g_zplus, g_ztrack, g_zlabel;

static int zoom_to_x(int z)
{
    int mid = (g_ztrack.left + g_ztrack.right) / 2, half = (g_ztrack.right - g_ztrack.left) / 2;
    if (z <= 100) return mid - MulDiv(100 - z, half, 90);
    return mid + MulDiv(z - 100, half, 400);
}

static int x_to_zoom(int x)
{
    int mid = (g_ztrack.left + g_ztrack.right) / 2, half = (g_ztrack.right - g_ztrack.left) / 2, z;
    if (x <= mid) z = 100 - MulDiv(mid - x, 90, half);
    else z = 100 + MulDiv(x - mid, 400, half);
    if (z > 90 && z < 110) z = 100;
    return max(10, min(500, z));
}

static void status_layout(HWND h)
{
    RECT rc;
    GetClientRect(h, &rc);
    SetRect(&g_zplus, rc.right - S(24), 0, rc.right - S(8), rc.bottom);
    SetRect(&g_ztrack, g_zplus.left - S(104), 0, g_zplus.left - S(4), rc.bottom);
    SetRect(&g_zminus, g_ztrack.left - S(20), 0, g_ztrack.left - S(4), rc.bottom);
    SetRect(&g_zlabel, g_zminus.left - S(52), 0, g_zminus.left - S(6), rc.bottom);
}

void status_dump(FILE *f)
{
    POINT a;
    if (!g_statusbar_on) return;
    status_layout(g_status);
    a.x = (g_zminus.left + g_zminus.right) / 2; a.y = (g_zminus.top + g_zminus.bottom) / 2;
    ClientToScreen(g_status, &a);
    fprintf(f, "zoomminus %ld %ld\n", a.x, a.y);
    a.x = (g_zplus.left + g_zplus.right) / 2; a.y = (g_zplus.top + g_zplus.bottom) / 2;
    ClientToScreen(g_status, &a);
    fprintf(f, "zoomplus %ld %ld\n", a.x, a.y);
    a.x = g_ztrack.left; a.y = (g_ztrack.top + g_ztrack.bottom) / 2;
    ClientToScreen(g_status, &a);
    fprintf(f, "zoomtrack %ld %ld %ld\n", a.x, a.y, a.x + (g_ztrack.right - g_ztrack.left));
}

LRESULT CALLBACK status_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    static BOOL drag;
    switch (m)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc, r;
        WCHAR t[16];
        HPEN p;
        HGDIOBJ op;
        int tx, cy;
        GetClientRect(h, &rc);
        status_layout(h);
        fill(dc, &rc, RGB(240, 240, 244));
        r = rc; r.bottom = 1; fill(dc, &r, LINE_GREY);
        swprintf(t, 16, L"%d%%", g_zoom);
        draw_text(dc, t, g_zlabel, DT_RIGHT | DT_VCENTER | DT_SINGLELINE, RGB(40, 40, 48), g_font_small);
        cy = (rc.top + rc.bottom) / 2;
        p = CreatePen(PS_SOLID, S(1) < 1 ? 1 : S(1), RGB(60, 60, 70));
        op = SelectObject(dc, p);
        MoveToEx(dc, g_zminus.left + S(3), cy, NULL); LineTo(dc, g_zminus.right - S(3), cy);
        MoveToEx(dc, g_zplus.left + S(3), cy, NULL); LineTo(dc, g_zplus.right - S(3), cy);
        MoveToEx(dc, (g_zplus.left + g_zplus.right) / 2, cy - S(5), NULL); LineTo(dc, (g_zplus.left + g_zplus.right) / 2, cy + S(5));
        MoveToEx(dc, g_ztrack.left, cy, NULL); LineTo(dc, g_ztrack.right, cy);
        MoveToEx(dc, (g_ztrack.left + g_ztrack.right) / 2, cy - S(3), NULL); LineTo(dc, (g_ztrack.left + g_ztrack.right) / 2, cy + S(4));
        SelectObject(dc, op); DeleteObject(p);
        tx = zoom_to_x(g_zoom);
        SetRect(&r, tx - S(3), cy - S(7), tx + S(3), cy + S(7));
        fill(dc, &r, ACCENT);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        status_layout(h);
        if (PtInRect(&g_zminus, pt)) do_command(CMD_ZOOMOUT);
        else if (PtInRect(&g_zplus, pt)) do_command(CMD_ZOOMIN);
        else if (pt.x >= g_ztrack.left - S(3) && pt.x <= g_ztrack.right + S(3))
        {
            extern void set_zoom(int z);
            drag = TRUE;
            SetCapture(h);
            set_zoom(x_to_zoom(pt.x));
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (drag)
        {
            extern void set_zoom(int z);
            set_zoom(x_to_zoom(GET_X_LPARAM(l)));
        }
        return 0;
    case WM_LBUTTONUP:
        if (drag) { drag = FALSE; ReleaseCapture(); write_dump(); }
        return 0;
    }
    (void)w;
    return DefWindowProcW(h, m, w, l);
}
