/* sg-pdf -- PDF Viewer: the window, the toolbar, commands and the command line.
 *
 * Stained Glass OS opens PDF files out of the box with this viewer (Windows
 * opens them in its browser, which we do not ship). The Linux half is
 * sg-session's `sg-pdf` -- Debian's poppler -- which renders the pages and
 * reports their text, links and outline; this half lays them out and draws
 * them:
 *
 *   - one continuous scroll of every page; zoom (Ctrl+wheel at the pointer,
 *     Ctrl+Plus/Minus, the zoom menu), fit width (the default) and fit page
 *     (Ctrl+\ switches), actual size (Ctrl+1); rotate (Ctrl+] and Ctrl+[)
 *   - the page box ("3 of 10", Ctrl+G), PgUp/PgDn, Home/End, arrows, Space
 *   - a sidebar of page thumbnails, or the document's bookmarks
 *   - find (Ctrl+F; Enter/F3 next, Shift+Enter/Shift+F3 previous), every hit
 *     highlighted, the current one stronger, "2 of 7"
 *   - select text by dragging (a double-click selects a word; Ctrl+A all),
 *     Ctrl+C copies it
 *   - links: inside the document they go to their page, web and mail links
 *     open with the default program
 *   - print (Ctrl+P), open (Ctrl+O, or drop a file on the window),
 *     document properties
 *
 * Wine gives a Windows program no pipe to a native one, so the viewer
 * re-launches itself as `sg-pdf --bridge wine <itself> --bridged <args>` and
 * talks to poppler on its standard handles (bridge.c). SG_PDF names another
 * sg-pdf; SG_PDF_DUMP=<file> writes what is shown after every paint (gates).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"
#include <shellapi.h>
#include <commdlg.h>
#include "resource.h"

#define CLASS_NAME L"SgPdfWindow"
#define SETTINGS_KEY L"Software\\Stained Glass\\PDF Viewer"
#define BAR_H dpx(44)
#define SIDE_W dpx(172)

app_t g;
HWND g_main, g_view, g_bar;
HINSTANCE g_inst;
HFONT g_font, g_font_small, g_font_bold;
int g_printed = -1;
static WCHAR g_dump[MAX_PATH];
static HWND g_page_edit, g_find_edit, g_tip;

/* ---- toolbar ----------------------------------------------------------------------------------- */

enum { G_SIDEBAR, G_MINUS, G_PLUS, G_FITWIDTH, G_FITPAGE, G_ROTATE, G_UP, G_DOWN, G_PRINT, G_OPEN, G_NONE };
typedef struct { int cmd, glyph; const WCHAR *tip, *name; RECT rc; } button_t;
enum { B_SIDEBAR, B_ZOOMOUT, B_ZOOM, B_ZOOMIN, B_FIT, B_ROTATE, B_PREV, B_NEXT, B_PRINT, B_OPEN, B_COUNT };
static button_t g_btn[B_COUNT] = {
    { CMD_SIDEBAR, G_SIDEBAR, L"Thumbnails and bookmarks", L"sidebar" },
    { CMD_ZOOMOUT, G_MINUS, L"Zoom out (Ctrl+Minus)", L"zoomout" },
    { 0, G_NONE, L"Zoom", L"zoom" },
    { CMD_ZOOMIN, G_PLUS, L"Zoom in (Ctrl+Plus)", L"zoomin" },
    { CMD_FITWIDTH, G_FITWIDTH, L"Fit to width / page (Ctrl+\\)", L"fit" },
    { CMD_ROTATE, G_ROTATE, L"Rotate (Ctrl+])", L"rotate" },
    { CMD_FINDPREV, G_UP, L"Previous (Shift+F3)", L"findprev" },
    { CMD_FINDNEXT, G_DOWN, L"Next (F3)", L"findnext" },
    { CMD_PRINT, G_PRINT, L"Print (Ctrl+P)", L"print" },
    { CMD_OPEN, G_OPEN, L"Open (Ctrl+O)", L"open" },
};
static int g_hover = -1, g_press = -1;
static RECT g_of_rc, g_count_rc;
static int g_seps[4], g_nseps;

int dpx(int px)
{
    return MulDiv(px, g.dpi ? g.dpi : 96, 96);
}

static HFONT make_font(int pt10, int weight)
{
    return CreateFontW(-MulDiv(pt10, g.dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void bar_layout(void)
{
    RECT rc;
    int x = dpx(8), b = dpx(34), top = (BAR_H - b) / 2, r;
    HDC dc;
    SIZE sz;
    WCHAR of[32];
    GetClientRect(g_bar, &rc);
    g_nseps = 0;
    SetRect(&g_btn[B_SIDEBAR].rc, x, top, x + b, top + b); x += b + dpx(8);
    g_seps[g_nseps++] = x; x += dpx(9);
    MoveWindow(g_page_edit, x, (BAR_H - dpx(26)) / 2, dpx(44), dpx(26), TRUE); x += dpx(50);
    swprintf(of, 32, L"of %d", g.npages);
    dc = GetDC(g_bar);
    SelectObject(dc, g_font);
    GetTextExtentPoint32W(dc, of, lstrlenW(of), &sz);
    ReleaseDC(g_bar, dc);
    SetRect(&g_of_rc, x, 0, x + sz.cx + dpx(4), BAR_H); x = g_of_rc.right + dpx(8);
    g_seps[g_nseps++] = x; x += dpx(9);
    SetRect(&g_btn[B_ZOOMOUT].rc, x, top, x + b, top + b); x += b;
    SetRect(&g_btn[B_ZOOM].rc, x, top, x + dpx(58), top + b); x += dpx(58);
    SetRect(&g_btn[B_ZOOMIN].rc, x, top, x + b, top + b); x += b + dpx(4);
    SetRect(&g_btn[B_FIT].rc, x, top, x + b, top + b); x += b;
    SetRect(&g_btn[B_ROTATE].rc, x, top, x + b, top + b); x += b + dpx(8);
    /* from the right */
    r = rc.right - dpx(8);
    SetRect(&g_btn[B_OPEN].rc, r - b, top, r, top + b); r -= b;
    SetRect(&g_btn[B_PRINT].rc, r - b, top, r, top + b); r -= b + dpx(8);
    g_seps[g_nseps++] = r; r -= dpx(9);
    SetRect(&g_btn[B_NEXT].rc, r - b, top, r, top + b); r -= b;
    SetRect(&g_btn[B_PREV].rc, r - b, top, r, top + b); r -= b;
    SetRect(&g_count_rc, r - dpx(76), 0, r - dpx(4), BAR_H); r -= dpx(80);
    MoveWindow(g_find_edit, max(x, r - dpx(200)), (BAR_H - dpx(26)) / 2, min(dpx(200), r - x), dpx(26), TRUE);
    /* tooltips follow the buttons */
    if (g_tip) {
        int i;
        for (i = 0; i < B_COUNT; i++) {
            TTTOOLINFOW ti = { sizeof(ti) };
            ti.hwnd = g_bar;
            ti.uId = i + 1;
            ti.rect = g_btn[i].rc;
            SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        }
    }
}

static void line(HDC dc, int x1, int y1, int x2, int y2)
{
    MoveToEx(dc, x1, y1, NULL);
    LineTo(dc, x2, y2);
}

static void draw_glyph(HDC dc, int glyph, int cx, int cy, COLORREF col)
{
    HPEN pen = CreatePen(PS_SOLID, max(1, dpx(1)), col), op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int s = dpx(8), a = dpx(3);
    switch (glyph) {
    case G_SIDEBAR:
        Rectangle(dc, cx - s, cy - s + dpx(1), cx + s + 1, cy + s);
        line(dc, cx - s + dpx(5), cy - s + dpx(1), cx - s + dpx(5), cy + s);
        break;
    case G_MINUS: line(dc, cx - s + dpx(2), cy, cx + s - dpx(1), cy); break;
    case G_PLUS:
        line(dc, cx - s + dpx(2), cy, cx + s - dpx(1), cy);
        line(dc, cx, cy - s + dpx(2), cx, cy + s - dpx(1));
        break;
    case G_FITWIDTH:
        Rectangle(dc, cx - s, cy - s + dpx(2), cx + s + 1, cy + s - dpx(1));
        line(dc, cx - s + dpx(3), cy, cx + s - dpx(2), cy);
        line(dc, cx - s + dpx(3), cy, cx - s + dpx(3) + a, cy - a); line(dc, cx - s + dpx(3), cy, cx - s + dpx(3) + a, cy + a);
        line(dc, cx + s - dpx(3), cy, cx + s - dpx(3) - a, cy - a); line(dc, cx + s - dpx(3), cy, cx + s - dpx(3) - a, cy + a);
        break;
    case G_FITPAGE:
        Rectangle(dc, cx - s + dpx(2), cy - s, cx + s - dpx(1), cy + s + 1);
        line(dc, cx, cy - s + dpx(3), cx, cy + s - dpx(2));
        line(dc, cx, cy - s + dpx(3), cx - a, cy - s + dpx(3) + a); line(dc, cx, cy - s + dpx(3), cx + a, cy - s + dpx(3) + a);
        line(dc, cx, cy + s - dpx(3), cx - a, cy + s - dpx(3) - a); line(dc, cx, cy + s - dpx(3), cx + a, cy + s - dpx(3) - a);
        break;
    case G_ROTATE: {
        /* three quarters of a circle, clockwise, ending in an arrow at the top */
        int r = dpx(7);
        SetArcDirection(dc, AD_CLOCKWISE);
        Arc(dc, cx - r, cy - r, cx + r + 1, cy + r + 1, cx - r, cy, cx, cy - r);
        line(dc, cx, cy - r, cx - a, cy - r - a);
        line(dc, cx, cy - r, cx - a, cy - r + a);
        break;
    }
    case G_UP: line(dc, cx - s + dpx(3), cy + dpx(3), cx, cy - dpx(2)); line(dc, cx, cy - dpx(2), cx + s - dpx(3) + 1, cy + dpx(3) + 1); break;
    case G_DOWN: line(dc, cx - s + dpx(3), cy - dpx(2), cx, cy + dpx(3)); line(dc, cx, cy + dpx(3), cx + s - dpx(3) + 1, cy - dpx(2) - 1); break;
    case G_PRINT:
        Rectangle(dc, cx - dpx(4), cy - s, cx + dpx(5), cy - dpx(3));
        Rectangle(dc, cx - s, cy - dpx(3), cx + s + 1, cy + dpx(4));
        Rectangle(dc, cx - dpx(4), cy + dpx(1), cx + dpx(5), cy + s);
        break;
    case G_OPEN: {
        POINT pts[6] = { { cx - s, cy + s - dpx(2) }, { cx - s, cy - s + dpx(2) }, { cx - dpx(2), cy - s + dpx(2) },
                         { cx, cy - s + dpx(4) }, { cx + s, cy - s + dpx(4) }, { cx + s, cy + s - dpx(2) } };
        Polyline(dc, pts, 6);
        line(dc, cx + s, cy + s - dpx(2), cx - s, cy + s - dpx(2));
        line(dc, cx - s, cy - dpx(2), cx + s, cy - dpx(2));
        break;
    }
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

static BOOL btn_enabled(int i)
{
    switch (i) {
    case B_OPEN: case B_SIDEBAR: return TRUE;
    case B_PREV: case B_NEXT: return g.nhits > 0;
    case B_PRINT: return g.npages > 0 && g.bridged;
    default: return g.npages > 0;
    }
}

static BOOL btn_on(int i)
{
    return i == B_SIDEBAR && g.side != SIDE_NONE;
}

static void bar_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC out = BeginPaint(hwnd, &ps), dc;
    RECT rc, r;
    HBITMAP buf, ob;
    HBRUSH bg = CreateSolidBrush(C_BAR), ln = CreateSolidBrush(C_LINE), hov = CreateSolidBrush(C_HOVER),
           prs = CreateSolidBrush(C_PRESS), act = CreateSolidBrush(C_ACTIVE);
    WCHAR text[64];
    int i;
    GetClientRect(hwnd, &rc);
    dc = CreateCompatibleDC(out);
    buf = CreateCompatibleBitmap(out, rc.right, rc.bottom);
    ob = SelectObject(dc, buf);
    FillRect(dc, &rc, bg);
    SetRect(&r, 0, rc.bottom - 1, rc.right, rc.bottom);
    FillRect(dc, &r, ln);
    for (i = 0; i < g_nseps; i++) { SetRect(&r, g_seps[i], dpx(10), g_seps[i] + 1, BAR_H - dpx(10)); FillRect(dc, &r, ln); }
    SelectObject(dc, g_font);
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < B_COUNT; i++) {
        button_t *b = &g_btn[i];
        BOOL en = btn_enabled(i);
        COLORREF col = en ? (btn_on(i) ? C_ACCENT : C_TEXT) : RGB(0xA8, 0xA8, 0xA8);
        if (en && i == g_press && i == g_hover) FillRect(dc, &b->rc, prs);
        else if (en && i == g_hover) FillRect(dc, &b->rc, hov);
        else if (btn_on(i)) FillRect(dc, &b->rc, act);
        if (i == B_ZOOM) {
            if (g.npages) swprintf(text, 64, L"%d%%", (int)floor(g.zoom * 100 + 0.5));
            else lstrcpyW(text, L"");
            SetTextColor(dc, col);
            DrawTextW(dc, text, -1, &b->rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            int glyph = b->glyph;
            if (i == B_FIT) glyph = g.fit == FIT_WIDTH ? G_FITPAGE : G_FITWIDTH;
            draw_glyph(dc, glyph, (b->rc.left + b->rc.right) / 2, (b->rc.top + b->rc.bottom) / 2, col);
        }
    }
    swprintf(text, 64, L"of %d", g.npages);
    SetTextColor(dc, C_SUBTEXT);
    DrawTextW(dc, text, -1, &g_of_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    text[0] = 0;
    if (g.searched && g.nhits) swprintf(text, 64, L"%d of %d", g.hit + 1, g.nhits);
    else if (g.searched) lstrcpyW(text, L"No results");
    SetTextColor(dc, g.searched && !g.nhits ? RGB(0xC4, 0x2B, 0x1C) : C_SUBTEXT);
    SelectObject(dc, g_font_small);
    DrawTextW(dc, text, -1, &g_count_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    BitBlt(out, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(buf);
    DeleteDC(dc);
    DeleteObject(bg); DeleteObject(ln); DeleteObject(hov); DeleteObject(prs); DeleteObject(act);
    EndPaint(hwnd, &ps);
}

static int btn_at(POINT pt)
{
    int i;
    for (i = 0; i < B_COUNT; i++) if (PtInRect(&g_btn[i].rc, pt)) return i;
    return -1;
}

static void zoom_menu(void)
{
    static const int Z[] = { 50, 75, 100, 125, 150, 200, 300, 400 };
    HMENU m = CreatePopupMenu();
    POINT pt = { g_btn[B_ZOOM].rc.left, g_btn[B_ZOOM].rc.bottom };
    WCHAR t[16];
    int i, cmd;
    AppendMenuW(m, MF_STRING | (g.fit == FIT_WIDTH ? MF_CHECKED : 0), 1, L"Fit to width");
    AppendMenuW(m, MF_STRING | (g.fit == FIT_PAGE ? MF_CHECKED : 0), 2, L"Fit to page");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < (int)(sizeof(Z) / sizeof(Z[0])); i++) {
        swprintf(t, 16, L"%d%%", Z[i]);
        AppendMenuW(m, MF_STRING | (g.fit == FIT_NONE && (int)floor(g.zoom * 100 + 0.5) == Z[i] ? MF_CHECKED : 0), 10 + i, t);
    }
    ClientToScreen(g_bar, &pt);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, g_main, NULL);
    DestroyMenu(m);
    if (cmd == 1) view_set_zoom(g.zoom, FIT_WIDTH, NULL);
    else if (cmd == 2) view_set_zoom(g.zoom, FIT_PAGE, NULL);
    else if (cmd >= 10) view_set_zoom(Z[cmd - 10] / 100.0, FIT_NONE, NULL);
}

static LRESULT CALLBACK bar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: bar_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: bar_layout(); return 0;
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = btn_at(pt);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        if (h != g_hover) { g_hover = h; InvalidateRect(hwnd, NULL, FALSE); }
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: g_hover = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        g_press = btn_at(pt);
        if (g_press >= 0) SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int b = btn_at(pt), p = g_press;
        g_press = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        InvalidateRect(hwnd, NULL, FALSE);
        if (b >= 0 && b == p && btn_enabled(b)) {
            if (b == B_ZOOM) zoom_menu();
            else if (b == B_FIT) app_command(g.fit == FIT_WIDTH ? CMD_FITPAGE : CMD_FITWIDTH);
            else app_command(g_btn[b].cmd);
        }
        return 0;
    }
    case WM_COMMAND:
        if ((HWND)lp == g_find_edit && HIWORD(wp) == EN_CHANGE) {
            /* a new word: the hits of the old one go */
            WCHAR t[256];
            GetWindowTextW(g_find_edit, t, 256);
            if (g.searched && wcscmp(t, g.needle)) view_find_clear();
        }
        return 0;
    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wp, RGB(0xFF, 0xFF, 0xFF));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Enter, Escape and Tab in the toolbar's edits */
static WNDPROC g_edit_proc;
static LRESULT CALLBACK edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            if (hwnd == g_find_edit) app_command(GetKeyState(VK_SHIFT) < 0 ? CMD_FINDPREV : CMD_FINDNEXT);
            else app_command(CMD_GOTOPAGE);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            if (hwnd == g_find_edit) { SetWindowTextW(hwnd, L""); view_find_clear(); }
            SetFocus(g_view);
            app_status_changed();
            return 0;
        }
        if (wp == VK_TAB) { SetFocus(hwnd == g_page_edit ? g_find_edit : g_view); return 0; }
    }
    if (msg == WM_CHAR && (wp == '\r' || wp == 27 || wp == '\t')) return 0;
    return CallWindowProcW(g_edit_proc, hwnd, msg, wp, lp);
}

/* ---- the frame ---------------------------------------------------------------------------------- */

void app_layout(void)
{
    RECT rc;
    int side;
    GetClientRect(g_main, &rc);
    side = g.side != SIDE_NONE ? SIDE_W : 0;
    MoveWindow(g_bar, 0, 0, rc.right, BAR_H, TRUE);
    MoveWindow(g_side, 0, BAR_H, side, max(0, rc.bottom - BAR_H), TRUE);
    ShowWindow(g_side, side ? SW_SHOWNA : SW_HIDE);
    MoveWindow(g_view, side, BAR_H, max(0, rc.right - side), max(0, rc.bottom - BAR_H), TRUE);
}

void app_update_title(void)
{
    WCHAR t[MAX_PATH + 32];
    if (g.name[0]) swprintf(t, MAX_PATH + 32, L"%ls - %ls", g.name, APP_NAME);
    else lstrcpyW(t, APP_NAME);
    SetWindowTextW(g_main, t);
}

void app_status_changed(void)
{
    WCHAR t[32];
    if (g_page_edit && GetFocus() != g_page_edit) {
        if (g.npages) swprintf(t, 32, L"%d", g.current + 1); else t[0] = 0;
        SetWindowTextW(g_page_edit, t);
    }
    if (g_bar) { bar_layout(); InvalidateRect(g_bar, NULL, FALSE); }
    app_dump();
}

/* ---- the dump, for gates ------------------------------------------------------------------------ */

static void dumpf(FILE *f, const WCHAR *fmt, ...)
{
    WCHAR w[2048];
    char u[6144];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(w, 2048, fmt, ap);
    va_end(ap);
    w[2047] = 0;
    to_utf8(w, u, sizeof(u));
    fputs(u, f);
}

static void screen_rect(HWND hwnd, RECT *r)
{
    MapWindowPoints(hwnd, NULL, (POINT *)r, 2);
}

BOOL side_thumb_rect(int i, RECT *out);
BOOL side_tab_center(int k, POINT *pt);

void app_dump(void)
{
    WCHAR tmp[MAX_PATH + 8], title[MAX_PATH + 40];
    FILE *f;
    RECT vr;
    int i, sel = 0;
    if (!g_dump[0] || !g_main) return;
    swprintf(tmp, MAX_PATH + 8, L"%ls.tmp", g_dump);
    if (!(f = _wfopen(tmp, L"wb"))) return;
    GetWindowTextW(g_main, title, MAX_PATH + 40);
    dumpf(f, L"file %ls\n", g.path);
    dumpf(f, L"title %ls\n", title);
    dumpf(f, L"bridged %d\n", g.bridged);
    dumpf(f, L"pages %d\n", g.npages);
    dumpf(f, L"current %d\n", g.npages ? g.current + 1 : 0);
    dumpf(f, L"zoom %d\n", (int)floor(g.zoom * 100 + 0.5));
    dumpf(f, L"fit %ls\n", g.fit == FIT_WIDTH ? L"width" : g.fit == FIT_PAGE ? L"page" : L"none");
    dumpf(f, L"rot %d\n", g.rot);
    dumpf(f, L"side %ls\n", g.side == SIDE_THUMBS ? L"thumbs" : g.side == SIDE_OUTLINE ? L"outline" : L"none");
    dumpf(f, L"outline %d\n", g.noutline);
    for (i = 0; i < g.noutline; i++) dumpf(f, L"bookmark %d %d %ls\n", g.outline[i].depth, g.outline[i].page + 1, g.outline[i].title ? g.outline[i].title : L"");
    dumpf(f, L"hits %d\n", g.nhits);
    dumpf(f, L"hit %d\n", g.hit + 1);
    if (g.hit >= 0 && g.hit < g.nhits) {
        RECT hr;
        view_box_to_client(g.hits[g.hit].page, &g.hits[g.hit].box, &hr);
        screen_rect(g_view, &hr);
        dumpf(f, L"hitrect %d %d %d %d %d\n", g.hits[g.hit].page + 1, hr.left, hr.top, hr.right, hr.bottom);
    }
    if (g.has_sel) {
        int pg;
        caret_t a = g.sel_a, b = g.sel_b;
        if (a.page > b.page || (a.page == b.page && a.pos > b.pos)) { caret_t t = a; a = b; b = t; }
        for (pg = a.page; pg <= b.page; pg++) sel += (pg == b.page ? b.pos : g.pages[pg].ntext) - (pg == a.page ? a.pos : 0);
    }
    dumpf(f, L"selected %d\n", sel);
    dumpf(f, L"printed %d\n", g_printed);
    GetClientRect(g_view, &vr);
    screen_rect(g_view, &vr);
    dumpf(f, L"view %d %d %d %d\n", vr.left, vr.top, vr.right, vr.bottom);
    for (i = 0; i < g.npages; i++) {
        RECT pr, cr;
        page_t *p = &g.pages[i];
        view_page_rect(i, &pr);
        GetClientRect(g_view, &cr);
        if (pr.bottom < 0 || pr.top > cr.bottom) continue;
        screen_rect(g_view, &pr);
        dumpf(f, L"page %d %d %d %d %d %d\n", i + 1, pr.left, pr.top, pr.right, pr.bottom,
              p->bmp && fabs(p->bscale - view_scale()) < 1e-4 && p->brot == g.rot);
        /* the page's first word, where it is on the screen */
        if (page_load_text(i) && p->ntext) {
            int a = 0, b;
            while (a < p->ntext && !iswalpha(p->text[a])) a++;
            b = a;
            while (b < p->ntext && iswalnum(p->text[b])) b++;
            if (b > a) {
                frect u = p->boxes[a];
                WCHAR word[64];
                RECT wr;
                int k;
                for (k = a; k < b; k++) {
                    u.x1 = min(u.x1, p->boxes[k].x1); u.y1 = min(u.y1, p->boxes[k].y1);
                    u.x2 = max(u.x2, p->boxes[k].x2); u.y2 = max(u.y2, p->boxes[k].y2);
                }
                lstrcpynW(word, p->text + a, min(b - a + 1, 64));
                view_box_to_client(i, &u, &wr);
                screen_rect(g_view, &wr);
                dumpf(f, L"word %d %d %d %d %d %ls\n", i + 1, wr.left, wr.top, wr.right, wr.bottom, word);
            }
        }
        if (page_load_links(i)) {
            int k;
            for (k = 0; k < p->nlinks; k++) {
                RECT lr;
                view_box_to_client(i, &p->links[k].box, &lr);
                screen_rect(g_view, &lr);
                dumpf(f, L"link %d %d %d %d %d %d %ls\n", i + 1, lr.left, lr.top, lr.right, lr.bottom,
                      p->links[k].page + 1, p->links[k].uri ? p->links[k].uri : L"");
            }
        }
    }
    for (i = 0; i < g.npages; i++) {
        RECT tr;
        if (side_thumb_rect(i, &tr)) dumpf(f, L"thumb %d %d %d %d %d %d\n", i + 1, tr.left, tr.top, tr.right, tr.bottom, g.pages[i].thumb != NULL);
    }
    {
        POINT pt;
        if (side_tab_center(0, &pt)) dumpf(f, L"tab thumbs %d %d\n", pt.x, pt.y);
        if (side_tab_center(1, &pt)) dumpf(f, L"tab outline %d %d\n", pt.x, pt.y);
    }
    if (g.side == SIDE_OUTLINE && g_tree) {
        HTREEITEM it = TreeView_GetFirstVisible(g_tree);
        while (it) {
            RECT ir;
            TVITEMW tv = { TVIF_PARAM, it };
            if (!TreeView_GetItemRect(g_tree, it, &ir, TRUE)) break;
            SendMessageW(g_tree, TVM_GETITEMW, 0, (LPARAM)&tv);
            screen_rect(g_tree, &ir);
            if ((int)tv.lParam >= 0 && (int)tv.lParam < g.noutline)
                dumpf(f, L"treeitem %d %d %ls\n", (ir.left + ir.right) / 2, (ir.top + ir.bottom) / 2,
                      g.outline[tv.lParam].title ? g.outline[tv.lParam].title : L"");
            it = TreeView_GetNextVisible(g_tree, it);
        }
    }
    for (i = 0; i < B_COUNT; i++) {
        RECT br = g_btn[i].rc;
        screen_rect(g_bar, &br);
        dumpf(f, L"button %ls %d %d\n", g_btn[i].name, (br.left + br.right) / 2, (br.top + br.bottom) / 2);
    }
    {
        RECT er;
        GetWindowRect(g_find_edit, &er);
        dumpf(f, L"findbox %d %d\n", (er.left + er.right) / 2, (er.top + er.bottom) / 2);
        GetWindowRect(g_page_edit, &er);
        dumpf(f, L"pagebox %d %d\n", (er.left + er.right) / 2, (er.top + er.bottom) / 2);
    }
    dumpf(f, L"focus %ls\n", GetFocus() == g_find_edit ? L"find" : GetFocus() == g_page_edit ? L"page" :
                             GetFocus() == g_view ? L"view" : L"other");
    if (g.error[0]) dumpf(f, L"error %ls\n", g.error);
    fclose(f);
    MoveFileExW(tmp, g_dump, MOVEFILE_REPLACE_EXISTING);
}

/* ---- opening ------------------------------------------------------------------------------------ */

static void free_document(void)
{
    int i, k;
    for (i = 0; i < g.npages; i++) {
        page_t *p = &g.pages[i];
        if (p->bmp) DeleteObject(p->bmp);
        if (p->thumb) DeleteObject(p->thumb);
        free(p->text);
        free(p->boxes);
        for (k = 0; k < p->nlinks; k++) free(p->links[k].uri);
        free(p->links);
    }
    free(g.pages);
    g.pages = NULL;
    g.npages = 0;
    for (i = 0; i < g.noutline; i++) free(g.outline[i].title);
    free(g.outline);
    g.outline = NULL;
    g.noutline = 0;
    if (g_tree) TreeView_DeleteAllItems(g_tree);
    free(g.hits);
    g.hits = NULL;
    g.nhits = 0;
    g.hit = -1;
    g.searched = FALSE;
    g.needle[0] = 0;
    g.has_sel = FALSE;
    g.sel_a.page = g.sel_b.page = -1;
    g.current = 0;
    g.doc_title[0] = 0;
    g.error[0] = 0;
}

static INT_PTR CALLBACK password_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static WCHAR *out;
    switch (msg) {
    case WM_INITDIALOG: {
        WCHAR t[MAX_PATH + 64];
        out = (WCHAR *)lp;
        swprintf(t, MAX_PATH + 64, L"\"%ls\" is protected. Enter the document's password to open it.", g.name);
        SetDlgItemTextW(dlg, IDC_PW_TEXT, t);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) { GetDlgItemTextW(dlg, IDC_PW_EDIT, out, 128); EndDialog(dlg, IDOK); return TRUE; }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static char *unix_path(const WCHAR *path)
{
    static char *(CDECL *to_unix)(const WCHAR *);
    char *u, *copy;
    if (!to_unix) to_unix = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    if (!to_unix || !(u = to_unix(path))) return NULL;
    copy = _strdup(u);
    HeapFree(GetProcessHeap(), 0, u);
    return copy;
}

BOOL app_open(const WCHAR *path)
{
    WCHAR full[MAX_PATH], pw[128] = L"", *base;
    char *upath, req[MAX_PATH * 4 + 300], pw8[400], head[256], num[32], *s, *e;
    BYTE *data = NULL;
    DWORD len;
    int rc, n, i, tries = 0;
    if (!GetFullPathNameW(path, MAX_PATH, full, NULL)) lstrcpynW(full, path, MAX_PATH);
    render_clear_wants(FALSE);
    render_clear_wants(TRUE);
    g.generation++;
    free_document();
    lstrcpynW(g.path, full, MAX_PATH);
    base = wcsrchr(full, L'\\');
    lstrcpynW(g.name, base ? base + 1 : full, MAX_PATH);
    app_update_title();
    if (!g.bridged) {
        lstrcpynW(g.error, L"PDF viewing needs its Linux half, sg-pdf (package sg-session).", 256);
        goto done;
    }
    if (!(upath = unix_path(full))) {
        lstrcpynW(g.error, L"That file cannot be found.", 256);
        goto done;
    }
    for (;;) {
        to_utf8(pw, pw8, sizeof(pw8));
        for (s = pw8; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
        snprintf(req, sizeof(req), pw[0] ? "open\t%s\t%s" : "open\t%s", upath, pw8);
        rc = br_request(req, head, sizeof(head), &data, &len);
        if (rc == 0 && strstr(head, "ERR password") && tries++ < 5) {
            SecureZeroMemory(pw, sizeof(pw));
            if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_PASSWORD), g_main, password_proc, (LPARAM)pw) != IDOK) {
                lstrcpynW(g.error, L"This document is protected by a password.", 256);
                break;
            }
            continue;
        }
        break;
    }
    SecureZeroMemory(pw, sizeof(pw));
    SecureZeroMemory(pw8, sizeof(pw8));
    SecureZeroMemory(req, sizeof(req));
    free(upath);
    if (rc != 1) {
        if (!g.error[0]) {
            WCHAR *msg = from_utf8(head, -1);
            if (strstr(head, "ERR open")) lstrcpynW(g.error, L"This file could not be opened. It may be damaged, or not a PDF file.", 256);
            else if (msg) swprintf(g.error, 256, L"The document could not be opened (%ls).", msg);
            free(msg);
        }
        free(data);
        goto done;
    }
    n = br_field(head, "pages", num, sizeof(num)) ? atoi(num) : 0;
    g.pages = n > 0 ? calloc(n, sizeof(page_t)) : NULL;
    for (s = (char *)data, i = 0; s && *s; s = e) {
        e = strchr(s, '\n');
        if (e) *e++ = 0;
        if (!strncmp(s, "size ", 5) && g.pages && i < n) {
            double w = 0, h = 0;
            sscanf(s + 5, "%lf %lf", &w, &h);
            g.pages[i].w = w > 1 ? w : 612;
            g.pages[i].h = h > 1 ? h : 792;
            i++;
        } else if (!strncmp(s, "title ", 6)) {
            WCHAR *t = from_utf8(s + 6, -1);
            if (t) { lstrcpynW(g.doc_title, t, 256); free(t); }
        }
    }
    g.npages = i;
    free(data);
    if (!g.npages) lstrcpynW(g.error, L"This document has no pages.", 256);
    side_load_outline();
done:
    g.hit = -1;
    view_relayout(FALSE);
    side_update();
    InvalidateRect(g_view, NULL, FALSE);
    app_status_changed();
    return g.npages > 0;
}

static void open_dialog(void)
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR file[MAX_PATH] = L"";
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"PDF documents (*.pdf)\0*.pdf\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) app_open(file);
}

static void properties(void)
{
    WCHAR t[1024], size[64] = L"";
    if (!g.npages) return;
    swprintf(size, 64, L"%.1f x %.1f in", g.pages[0].w / 72.0, g.pages[0].h / 72.0);
    swprintf(t, 1024, L"File:\t%ls\nTitle:\t%ls\nPages:\t%d\nPage size:\t%ls", g.path,
             g.doc_title[0] ? g.doc_title : L"(none)", g.npages, size);
    MessageBoxW(g_main, t, L"Document properties", MB_OK | MB_ICONINFORMATION);
}

static void save_settings(void)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    RegSetValueExW(k, L"Sidebar", 0, REG_DWORD, (const BYTE *)&g.side, sizeof(DWORD));
    RegCloseKey(k);
}

static void load_settings(void)
{
    DWORD v, cb = sizeof(v);
    g.side = SIDE_NONE;
    if (!RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"Sidebar", RRF_RT_REG_DWORD, NULL, &v, &cb) && v <= SIDE_OUTLINE)
        g.side = (int)v;
}

void app_command(int cmd)
{
    switch (cmd) {
    case CMD_OPEN: open_dialog(); break;
    case CMD_PRINT: print_document(); break;
    case CMD_ZOOMIN: view_zoom_step(1); break;
    case CMD_ZOOMOUT: view_zoom_step(-1); break;
    case CMD_ACTUAL: view_set_zoom(1.0, FIT_NONE, NULL); break;
    case CMD_FITWIDTH: view_set_zoom(g.zoom, FIT_WIDTH, NULL); break;
    case CMD_FITPAGE: view_set_zoom(g.zoom, FIT_PAGE, NULL); break;
    case CMD_ROTATE: case CMD_ROTATE_LEFT: {
        int page = g.current;
        g.rot = (g.rot + (cmd == CMD_ROTATE ? 90 : 270)) % 360;
        view_relayout(TRUE);
        view_goto_page(page, 0);
        side_update();
        break;
    }
    case CMD_SIDEBAR:
        side_set_mode(g.side == SIDE_NONE ? (g.noutline ? SIDE_OUTLINE : SIDE_THUMBS) : SIDE_NONE);
        save_settings();
        break;
    case CMD_THUMBS: side_set_mode(SIDE_THUMBS); save_settings(); break;
    case CMD_OUTLINE: side_set_mode(SIDE_OUTLINE); save_settings(); break;
    case CMD_FIND:
        SetFocus(g_find_edit);
        SendMessageW(g_find_edit, EM_SETSEL, 0, -1);
        break;
    case CMD_FINDNEXT: case CMD_FINDPREV: {
        WCHAR t[256];
        GetWindowTextW(g_find_edit, t, 256);
        view_find(t, cmd == CMD_FINDNEXT ? 1 : -1);
        break;
    }
    case CMD_COPY: view_copy(); break;
    case CMD_SELECTALL: view_select_all(); break;
    case CMD_GOTOPAGE: {
        WCHAR t[16];
        GetWindowTextW(g_page_edit, t, 16);
        SetFocus(g_view);
        view_goto_page(max(1, min(_wtoi(t), g.npages)) - 1, 0);
        break;
    }
    case CMD_FIRST: view_goto_page(0, 0); break;
    case CMD_LAST: view_goto_page(g.npages - 1, 0); break;
    case CMD_PROPERTIES: properties(); break;
    }
    app_status_changed();
}

/* keys that work wherever the focus is */
static BOOL accelerator(MSG *m)
{
    HWND focus = GetFocus();
    BOOL ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    BOOL in_edit = focus == g_find_edit || focus == g_page_edit;
    if (m->message != WM_KEYDOWN) return FALSE;
    if (!IsChild(g_main, m->hwnd) && m->hwnd != g_main) return FALSE;
    switch (m->wParam) {
    case VK_F3: app_command(shift ? CMD_FINDPREV : CMD_FINDNEXT); return TRUE;
    }
    if (!ctrl) return FALSE;
    switch (m->wParam) {
    case 'O': app_command(CMD_OPEN); return TRUE;
    case 'P': app_command(CMD_PRINT); return TRUE;
    case 'F': app_command(CMD_FIND); return TRUE;
    case 'G': SetFocus(g_page_edit); SendMessageW(g_page_edit, EM_SETSEL, 0, -1); return TRUE;
    case 'W': PostMessageW(g_main, WM_CLOSE, 0, 0); return TRUE;
    case VK_OEM_PLUS: case VK_ADD: app_command(CMD_ZOOMIN); return TRUE;
    case VK_OEM_MINUS: case VK_SUBTRACT: app_command(CMD_ZOOMOUT); return TRUE;
    case '0': case VK_NUMPAD0: app_command(CMD_FITWIDTH); return TRUE;
    case '1': case VK_NUMPAD1: app_command(CMD_ACTUAL); return TRUE;
    case VK_OEM_5: app_command(g.fit == FIT_WIDTH ? CMD_FITPAGE : CMD_FITWIDTH); return TRUE;
    case VK_OEM_6: app_command(CMD_ROTATE); return TRUE;
    case VK_OEM_4: app_command(CMD_ROTATE_LEFT); return TRUE;
    case VK_HOME: if (!in_edit) { app_command(CMD_FIRST); return TRUE; } break;
    case VK_END: if (!in_edit) { app_command(CMD_LAST); return TRUE; } break;
    case 'C': if (!in_edit) { app_command(CMD_COPY); return TRUE; } break;
    case 'A': if (!in_edit) { app_command(CMD_SELECTALL); return TRUE; } break;
    }
    return FALSE;
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: app_layout(); return 0;
    case WM_SETFOCUS: SetFocus(g_view); return 0;
    case WM_DROPFILES: {
        WCHAR file[MAX_PATH];
        if (DragQueryFileW((HDROP)wp, 0, file, MAX_PATH)) app_open(file);
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = dpx(480);
        mm->ptMinTrackSize.y = dpx(300);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        g.dpi = HIWORD(wp);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        view_relayout(TRUE);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- start ------------------------------------------------------------------------------------- */

static const WCHAR *args_after_program(const WCHAR *cmd)
{
    BOOL q = FALSE;
    while (*cmd && (q || (*cmd != ' ' && *cmd != '\t'))) { if (*cmd == '"') q = !q; cmd++; }
    while (*cmd == ' ' || *cmd == '\t') cmd++;
    return cmd;
}

/* Not bridged yet: start ourselves again through sg-pdf's bridge. */
static BOOL relaunch(const WCHAR *args)
{
    static WCHAR cmd[8192];
    WCHAR self[MAX_PATH], helper[MAX_PATH + 16], unix_helper[MAX_PATH] = L"/usr/bin/sg-pdf", *p;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    GetEnvironmentVariableW(L"SG_PDF", unix_helper, MAX_PATH);
    swprintf(helper, MAX_PATH + 16, L"\\\\?\\unix%ls", unix_helper);
    for (p = helper + 8; *p; p++) if (*p == '/') *p = '\\';
    if (GetFileAttributesW(helper) == INVALID_FILE_ATTRIBUTES) return FALSE;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    swprintf(cmd, 8192, L"\"%ls\" --bridge wine \"%ls\" --bridged %ls", helper, self, args ? args : L"");
    cmd[8191] = 0;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return FALSE;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    typedef BOOL (WINAPI *ctx_fn)(HANDLE);
    ctx_fn set_ctx = (ctx_fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    const WCHAR *args = args_after_program(GetCommandLineW());
    WCHAR file[MAX_PATH] = L"";
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    WNDCLASSW wc = { 0 };
    MSG msg;
    HDC screen;
    BOOL bridged = FALSE, print_after = FALSE;
    int argc, i;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    (void)prev; (void)cmdline;
    g_inst = inst;

    for (i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--bridged")) bridged = TRUE;
        else if (!_wcsicmp(argv[i], L"/p") || !_wcsicmp(argv[i], L"-p")) print_after = TRUE;
        else if (argv[i][0] != '-' && argv[i][0] != '/' && !file[0]) lstrcpynW(file, argv[i], MAX_PATH);
        else if (argv[i][0] == '/' && argv[i][1] != 0 && wcschr(argv[i] + 1, '/') && !file[0])
            lstrcpynW(file, argv[i], MAX_PATH);   /* a Unix path */
    }
    LocalFree(argv);
    /* a file:// URI names the file */
    if (!_wcsnicmp(file, L"file:", 5)) {
        WCHAR p[MAX_PATH];
        DWORD n = MAX_PATH;
        HRESULT (WINAPI *from_url)(const WCHAR *, WCHAR *, DWORD *, DWORD) =
            (void *)GetProcAddress(LoadLibraryW(L"shlwapi.dll"), "PathCreateFromUrlW");
        if (from_url && from_url(file, p, &n, 0) == S_OK) lstrcpynW(file, p, MAX_PATH);
    }
    if (!bridged && !GetEnvironmentVariableW(L"SG_PDF_NO_BRIDGE", NULL, 0) && relaunch(args)) return 0;

    if (set_ctx) set_ctx((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
    else SetProcessDPIAware();
    InitCommonControlsEx(&icc);
    screen = GetDC(NULL);
    g.dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    g.zoom = 1.0;
    g.fit = FIT_WIDTH;
    g.hit = -1;
    g.sel_a.page = g.sel_b.page = -1;
    GetEnvironmentVariableW(L"SG_PDF_DUMP", g_dump, MAX_PATH);
    render_init();
    g.bridged = bridged && br_start();
    load_settings();

    g_font = make_font(100, FW_NORMAL);
    g_font_small = make_font(90, FW_NORMAL);
    g_font_bold = make_font(90, FW_SEMIBOLD);

    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);
    wc.lpfnWndProc = bar_proc;
    wc.hIcon = NULL;
    wc.lpszClassName = L"SgPdfBar";
    RegisterClassW(&wc);
    view_register();
    side_register();

    g_main = CreateWindowExW(WS_EX_ACCEPTFILES, CLASS_NAME, APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, dpx(980), dpx(720), NULL, NULL, inst, NULL);
    if (!g_main) return 1;
    {
        /* the window's DPI, now that it has one */
        UINT (WINAPI *for_window)(HWND) = (void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        if (for_window && for_window(g_main)) g.dpi = for_window(g_main);
    }
    g_bar = CreateWindowExW(0, L"SgPdfBar", NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, g_main, NULL, inst, NULL);
    g_page_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_CENTER | ES_NUMBER | ES_AUTOHSCROLL,
                                  0, 0, 10, 10, g_bar, NULL, inst, NULL);
    g_find_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  0, 0, 10, 10, g_bar, NULL, inst, NULL);
    SendMessageW(g_page_edit, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(g_find_edit, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(g_find_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Find in document");
    SendMessageW(g_find_edit, EM_LIMITTEXT, 250, 0);
    g_edit_proc = (WNDPROC)SetWindowLongPtrW(g_find_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
    SetWindowLongPtrW(g_page_edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            0, 0, 0, 0, g_main, NULL, inst, NULL);
    for (i = 0; g_tip && i < B_COUNT; i++) {
        TTTOOLINFOW ti = { sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = g_bar;
        ti.uId = i + 1;
        ti.lpszText = (WCHAR *)g_btn[i].tip;
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }
    side_create(g_main);
    g_view = CreateWindowExW(0, L"SgPdfView", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP,
                             0, 0, 10, 10, g_main, NULL, inst, NULL);
    app_layout();
    ShowWindow(g_main, show == SW_SHOWMINNOACTIVE ? show : SW_SHOWNORMAL);
    UpdateWindow(g_main);
    SetFocus(g_view);
    if (g.bridged) render_start_thread();
    if (file[0] && app_open(file) && print_after) app_command(CMD_PRINT);
    else if (!g.bridged) lstrcpynW(g.error, L"PDF viewing needs its Linux half, sg-pdf (package sg-session).", 256);
    app_status_changed();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (accelerator(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
