/* sg-pdf -- SG PDF: the window, the menu, the toolbar, commands and the
 * command line.
 *
 * Stained Glass OS opens and edits PDF files out of the box with this
 * program (the .pdf association). The Linux half is sg-session's `sg-pdf`
 * -- MuPDF -- which renders the pages, reports their text, links, outline,
 * objects, comments and fields, and makes every change; this half lays the
 * pages out, draws them and drives it. The layout is the familiar PDF
 * editor's: a menu bar; a toolbar (sidebar, page box, zoom, fit, rotate,
 * undo, redo, find, save, print, open, the tools pane); the sidebar of
 * thumbnails or bookmarks on the left; the tools pane on the right (Edit
 * PDF, Comment, Fill & Sign, Redact, Organize Pages, Export PDF, Combine
 * Files, Protect) that becomes the open tool's properties; and under the
 * toolbar, the open tool's own bar.
 *
 * Viewing: one continuous scroll of every page; zoom (Ctrl+wheel at the
 * pointer, Ctrl+Plus/Minus, the zoom menu), fit width (the default) and fit
 * page (Ctrl+\ switches), actual size (Ctrl+1); rotate the view (Ctrl+] and
 * Ctrl+[); the page box ("3 of 10", Ctrl+G); find (Ctrl+F, F3); select text
 * and Ctrl+C; links; print (Ctrl+P). Editing: see toolui.c, interact.c,
 * doc.c, organize.c and dialogs.c; Ctrl+S saves, Ctrl+Shift+S saves as,
 * Ctrl+Z undoes and Ctrl+Y redoes.
 *
 * Wine gives a Windows program no pipe to a native one, so the program
 * re-launches itself as `sg-pdf --bridge wine <itself> --bridged <args>` and
 * talks to MuPDF on its standard handles (bridge.c). SG_PDF names another
 * sg-pdf; SG_PDF_DUMP=<file> writes what is shown after every paint (gates).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"
#include <shellapi.h>
#include <commdlg.h>
#include "resource.h"
#include "../sg-mode.h"
#include "../sg-smooth.h"

#define CLASS_NAME L"SgPdfWindow"
#define SETTINGS_KEY L"Software\\Stained Glass\\PDF Viewer"
#define BAR_H dpx(44)
#define SIDE_W dpx(172)

app_t g;
palette_t P;
HWND g_main, g_view, g_bar;
HINSTANCE g_inst;
HFONT g_font, g_font_small, g_font_bold, g_font_title;
int g_printed = -1;
static WCHAR g_dump[MAX_PATH];
static HWND g_page_edit, g_find_edit, g_tip;
static HMENU g_menu;
HMENU g_menu_popup;     /* the menu, shown from the Menu button (home.c) */
static HBRUSH g_editbrush;
static HBRUSH g_dlg_brush, g_dlg_edit_brush;    /* dialogs in dark (dialog_hook) */

const COLORREF COMMENT_COLORS[NCOLORS] = {
    RGB(0xFF, 0xE5, 0x00), RGB(0xFF, 0x3B, 0x30), RGB(0x2E, 0x7D, 0xF6), RGB(0x2E, 0xB8, 0x4D),
    RGB(0xAF, 0x52, 0xDE), RGB(0x10, 0x10, 0x10),
};

/* ---- light and dark -------------------------------------------------------------------------- */

static void set_palette(BOOL dark)
{
    g.dark = dark;
    if (!dark) {
        P.bar = RGB(0xFF, 0xFF, 0xFF); P.line = RGB(0xE0, 0xE0, 0xE0); P.canvas = RGB(0xE9, 0xE9, 0xEC);
        P.side = RGB(0xF3, 0xF3, 0xF3); P.text = RGB(0x1F, 0x1F, 0x1F); P.subtext = RGB(0x60, 0x60, 0x60);
        P.hover = RGB(0xEB, 0xEB, 0xEB); P.press = RGB(0xDD, 0xDD, 0xDD); P.accent = RGB(112, 48, 192);
        P.active = RGB(0xEE, 0xE6, 0xF8); P.shadow = RGB(0xC8, 0xC8, 0xCC); P.pane = RGB(0xFA, 0xFA, 0xFA);
        P.edit = RGB(0xFF, 0xFF, 0xFF); P.disabled = RGB(0xA8, 0xA8, 0xA8);
    } else {
        P.bar = RGB(0x2B, 0x2B, 0x2B); P.line = RGB(0x40, 0x40, 0x40); P.canvas = RGB(0x1C, 0x1C, 0x1E);
        P.side = RGB(0x24, 0x24, 0x26); P.text = RGB(0xF2, 0xF2, 0xF2); P.subtext = RGB(0xAA, 0xAA, 0xAA);
        P.hover = RGB(0x3A, 0x3A, 0x3C); P.press = RGB(0x48, 0x48, 0x4A); P.accent = RGB(0xB9, 0x8E, 0xF5);
        P.active = RGB(0x3B, 0x2F, 0x52); P.shadow = RGB(0x0C, 0x0C, 0x0C); P.pane = RGB(0x26, 0x26, 0x28);
        P.edit = RGB(0x1E, 0x1E, 0x1E); P.disabled = RGB(0x66, 0x66, 0x66);
    }
    if (g_editbrush) DeleteObject(g_editbrush);
    g_editbrush = CreateSolidBrush(P.edit);
    if (g_dlg_brush) { DeleteObject(g_dlg_brush); g_dlg_brush = NULL; }
    if (g_dlg_edit_brush) { DeleteObject(g_dlg_edit_brush); g_dlg_edit_brush = NULL; }
}

/* the menu bar in the app mode: in dark, its items are ours to draw (Wine
 * draws a menu bar in the system's colours), on the toolbar's colour */
static const WCHAR *const MENU_NAMES[] = { L"&File", L"&Edit", L"&View", L"&Tools", L"&Help" };
static HBRUSH g_menubrush;

static void menu_mode(void)
{
    MENUINFO mi = { sizeof(mi) };
    int i;
    if (!g_menu) return;
    for (i = 0; i < 5; i++) {
        HMENU sub = GetSubMenu(g_menu, i);
        if (g.dark) ModifyMenuW(g_menu, i, MF_BYPOSITION | MF_OWNERDRAW | MF_POPUP, (UINT_PTR)sub, (LPCWSTR)(ULONG_PTR)(i + 1));
        else ModifyMenuW(g_menu, i, MF_BYPOSITION | MF_STRING | MF_POPUP, (UINT_PTR)sub, MENU_NAMES[i]);
    }
    if (g_menubrush) DeleteObject(g_menubrush);
    g_menubrush = g.dark ? CreateSolidBrush(C_BAR) : NULL;
    mi.fMask = MIM_BACKGROUND;
    mi.hbrBack = g_menubrush;
    SetMenuInfo(g_menu, &mi);
    if (g_main) DrawMenuBar(g_main);
}

static BOOL menu_measure(MEASUREITEMSTRUCT *mi)
{
    HDC dc;
    SIZE sz;
    const WCHAR *t;
    if (mi->CtlType != ODT_MENU || mi->itemData < 1 || mi->itemData > 5) return FALSE;
    t = MENU_NAMES[mi->itemData - 1];
    dc = GetDC(g_main);
    SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
    GetTextExtentPoint32W(dc, t + 1, lstrlenW(t) - 1, &sz);
    ReleaseDC(g_main, dc);
    mi->itemWidth = sz.cx + dpx(10);
    mi->itemHeight = GetSystemMetrics(SM_CYMENU);
    return TRUE;
}

static BOOL menu_draw(DRAWITEMSTRUCT *di)
{
    HBRUSH b;
    RECT r = di->rcItem;
    if (di->CtlType != ODT_MENU || di->itemData < 1 || di->itemData > 5) return FALSE;
    b = CreateSolidBrush(di->itemState & (ODS_SELECTED | ODS_HOTLIGHT) ? C_HOVER : C_BAR);
    FillRect(di->hDC, &r, b);
    DeleteObject(b);
    SetBkMode(di->hDC, TRANSPARENT);
    SetTextColor(di->hDC, C_TEXT);
    SelectObject(di->hDC, GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(di->hDC, MENU_NAMES[di->itemData - 1], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE |
              (di->itemState & ODS_NOACCEL ? DT_HIDEPREFIX : 0));
    return TRUE;
}

/* dark: the menu bar right of its last item (Wine paints it the system's colour) */
static void menu_fill_rest(HWND hwnd)
{
    MENUBARINFO mb = { sizeof(mb) };
    RECT last, wr, r;
    HDC dc;
    HBRUSH b;
    int n;
    if (!g.dark || !g_menu || !GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mb)) return;
    n = GetMenuItemCount(g_menu);
    if (n <= 0 || !GetMenuItemRect(hwnd, g_menu, n - 1, &last)) return;
    GetWindowRect(hwnd, &wr);
    SetRect(&r, last.right - wr.left, mb.rcBar.top - wr.top, mb.rcBar.right - wr.left, mb.rcBar.bottom - wr.top);
    if (r.right <= r.left) return;
    dc = GetWindowDC(hwnd);
    b = CreateSolidBrush(C_BAR);
    FillRect(dc, &r, b);
    DeleteObject(b);
    ReleaseDC(hwnd, dc);
}

void app_apply_mode(void)
{
    set_palette(sg_apps_dark());
    menu_mode();
    if (g_main) {
        sg_mode_title(g_main, g.dark);
        side_apply_mode();
        RedrawWindow(g_main, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
    }
}

/* ---- dialogs in the app mode ----------------------------------------------------------------------- */

/* Every dialog of ours (and the message boxes) follows the app mode: a hook sees each one start
 * and, in dark, subclasses it to colour its background and controls from the palette. */
static LRESULT CALLBACK dark_dialog(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    (void)id; (void)data;
    switch (msg) {
    case WM_CTLCOLORDLG: case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
        if (!g.dark) break;
        SetTextColor((HDC)wp, C_TEXT);
        SetBkColor((HDC)wp, C_PANE);
        return (LRESULT)g_dlg_brush;
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        if (!g.dark) break;
        SetTextColor((HDC)wp, C_TEXT);
        SetBkColor((HDC)wp, C_EDITBG);
        return (LRESULT)g_dlg_edit_brush;
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, dark_dialog, 1);
        break;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK dialog_hook(int code, WPARAM wp, LPARAM lp)
{
    CWPRETSTRUCT *c = (CWPRETSTRUCT *)lp;
    if (code == HC_ACTION && c->message == WM_INITDIALOG && g.dark) {
        WCHAR cls[16];
        if (GetClassNameW(c->hwnd, cls, 16) && !wcscmp(cls, L"#32770")) {
            if (!g_dlg_brush) g_dlg_brush = CreateSolidBrush(C_PANE);
            if (!g_dlg_edit_brush) g_dlg_edit_brush = CreateSolidBrush(C_EDITBG);
            sg_mode_title(c->hwnd, TRUE);
            SetWindowSubclass(c->hwnd, dark_dialog, 1, 0);
            InvalidateRect(c->hwnd, NULL, TRUE);
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

/* ---- toolbar ----------------------------------------------------------------------------------- */

enum { G_SIDEBAR, G_MINUS, G_PLUS, G_FITWIDTH, G_FITPAGE, G_ROTATE, G_UP, G_DOWN, G_PRINT, G_OPEN, G_SAVE, G_UNDO, G_REDO,
       G_TOOLS, G_NONE };
typedef struct { int cmd, glyph; const WCHAR *tip, *name; RECT rc; } button_t;
enum { B_SIDEBAR, B_ZOOMOUT, B_ZOOM, B_ZOOMIN, B_FIT, B_ROTATE, B_UNDO, B_REDO, B_PREV, B_NEXT, B_SAVE, B_PRINT, B_OPEN,
       B_TOOLS, B_COUNT };
static button_t g_btn[B_COUNT] = {
    { CMD_SIDEBAR, G_SIDEBAR, L"Thumbnails and bookmarks (F4)", L"sidebar" },
    { CMD_ZOOMOUT, G_MINUS, L"Zoom out (Ctrl+Minus)", L"zoomout" },
    { 0, G_NONE, L"Zoom", L"zoom" },
    { CMD_ZOOMIN, G_PLUS, L"Zoom in (Ctrl+Plus)", L"zoomin" },
    { CMD_FITWIDTH, G_FITWIDTH, L"Fit to width / page (Ctrl+\\)", L"fit" },
    { CMD_ROTATE, G_ROTATE, L"Rotate view (Ctrl+])", L"rotate" },
    { CMD_UNDO, G_UNDO, L"Undo (Ctrl+Z)", L"undo" },
    { CMD_REDO, G_REDO, L"Redo (Ctrl+Y)", L"redo" },
    { CMD_FINDPREV, G_UP, L"Previous (Shift+F3)", L"findprev" },
    { CMD_FINDNEXT, G_DOWN, L"Next (F3)", L"findnext" },
    { CMD_SAVE, G_SAVE, L"Save (Ctrl+S)", L"save" },
    { CMD_PRINT, G_PRINT, L"Print (Ctrl+P)", L"print" },
    { CMD_OPEN, G_OPEN, L"Open (Ctrl+O)", L"open" },
    { CMD_PANE, G_TOOLS, L"Tools pane (Shift+F4)", L"tools" },
};
static int g_hover = -1, g_press = -1;
static RECT g_of_rc, g_count_rc;
static int g_seps[6], g_nseps;

int dpx(int px)
{
    return MulDiv(px, g.dpi ? g.dpi : 96, 96);
}

static HFONT make_font(int pt10, int weight)
{
    return CreateFontW(-MulDiv(pt10, g.dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

BOOL app_can(unsigned perm)
{
    return (g.perms & perm) != 0;
}

/* the fonts at the window's DPI (again when it moves to a monitor of another scale) */
static void fonts_again(void)
{
    HFONT old[4] = { g_font, g_font_small, g_font_bold, g_font_title };
    int i;
    g_font = make_font(100, FW_NORMAL);
    g_font_small = make_font(90, FW_NORMAL);
    g_font_bold = make_font(90, FW_SEMIBOLD);
    g_font_title = make_font(120, FW_SEMIBOLD);
    if (g_page_edit) SendMessageW(g_page_edit, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (g_find_edit) SendMessageW(g_find_edit, WM_SETFONT, (WPARAM)g_font, TRUE);
    toolui_fonts();
    side_fonts();
    frame_update();
    for (i = 0; i < 4; i++) if (old[i]) DeleteObject(old[i]);
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
    g_seps[g_nseps++] = x; x += dpx(9);
    SetRect(&g_btn[B_UNDO].rc, x, top, x + b, top + b); x += b;
    SetRect(&g_btn[B_REDO].rc, x, top, x + b, top + b); x += b + dpx(8);
    /* from the right */
    r = rc.right - dpx(8);
    SetRect(&g_btn[B_TOOLS].rc, r - b, top, r, top + b); r -= b + dpx(8);
    g_seps[g_nseps++] = r; r -= dpx(9);
    SetRect(&g_btn[B_OPEN].rc, r - b, top, r, top + b); r -= b;
    SetRect(&g_btn[B_PRINT].rc, r - b, top, r, top + b); r -= b;
    SetRect(&g_btn[B_SAVE].rc, r - b, top, r, top + b); r -= b + dpx(8);
    g_seps[g_nseps++] = r; r -= dpx(9);
    SetRect(&g_btn[B_NEXT].rc, r - b, top, r, top + b); r -= b;
    SetRect(&g_btn[B_PREV].rc, r - b, top, r, top + b); r -= b;
    SetRect(&g_count_rc, r - dpx(76), 0, r - dpx(4), BAR_H); r -= dpx(80);
    MoveWindow(g_find_edit, max(x, r - dpx(200)), (BAR_H - dpx(26)) / 2, max(dpx(40), min(dpx(200), r - x)), dpx(26), TRUE);
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

/* sg-smooth: drawn in a region (draw_glyph, below) */
static void draw_glyph_raw(HDC dc, int glyph, int cx, int cy, COLORREF col)
{
    HPEN pen = CreatePen(PS_SOLID, max(1, dpx(1)), col), op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int s = dpx(8), a = dpx(3);
    switch (glyph) {
    case G_SIDEBAR:
        Rectangle(dc, cx - s, cy - s + dpx(1), cx + s + 1, cy + s);
        line(dc, cx - s + dpx(5), cy - s + dpx(1), cx - s + dpx(5), cy + s);
        break;
    case G_TOOLS: {
        int q = dpx(6), gap = dpx(2);
        Rectangle(dc, cx - q - gap, cy - q - gap, cx - gap + 1, cy - gap + 1);
        Rectangle(dc, cx + gap, cy - q - gap, cx + q + gap + 1, cy - gap + 1);
        Rectangle(dc, cx - q - gap, cy + gap, cx - gap + 1, cy + q + gap + 1);
        Rectangle(dc, cx + gap, cy + gap, cx + q + gap + 1, cy + q + gap + 1);
        break;
    }
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
        int r = dpx(7);
        SetArcDirection(dc, AD_CLOCKWISE);
        Arc(dc, cx - r, cy - r, cx + r + 1, cy + r + 1, cx - r, cy, cx, cy - r);
        line(dc, cx, cy - r, cx - a, cy - r - a);
        line(dc, cx, cy - r, cx - a, cy - r + a);
        break;
    }
    case G_UNDO: case G_REDO: {
        /* a hook: an arc over the top, an arrow head at its start */
        int r = dpx(6), d = glyph == G_UNDO ? 1 : -1, x0 = cx - d * dpx(1);
        SetArcDirection(dc, glyph == G_UNDO ? AD_CLOCKWISE : AD_COUNTERCLOCKWISE);
        Arc(dc, x0 - r, cy - r + dpx(1), x0 + r + 1, cy + r + dpx(2), x0 - d * r, cy + dpx(1), x0 + d * r / 2, cy + r + dpx(2));
        line(dc, x0 - d * r, cy + dpx(1), x0 - d * r - d * a, cy + dpx(1) - a);
        line(dc, x0 - d * r, cy + dpx(1), x0 - d * r + d * a, cy + dpx(1) - a);
        break;
    }
    case G_UP: line(dc, cx - s + dpx(3), cy + dpx(3), cx, cy - dpx(2)); line(dc, cx, cy - dpx(2), cx + s - dpx(3) + 1, cy + dpx(3) + 1); break;
    case G_DOWN: line(dc, cx - s + dpx(3), cy - dpx(2), cx, cy + dpx(3)); line(dc, cx, cy + dpx(3), cx + s - dpx(3) + 1, cy - dpx(2) - 1); break;
    case G_PRINT:
        Rectangle(dc, cx - dpx(4), cy - s, cx + dpx(5), cy - dpx(3));
        Rectangle(dc, cx - s, cy - dpx(3), cx + s + 1, cy + dpx(4));
        Rectangle(dc, cx - dpx(4), cy + dpx(1), cx + dpx(5), cy + s);
        break;
    case G_SAVE: {
        /* a disk: a square with a clipped corner, a label and a shutter */
        POINT pts[6] = { { cx - s, cy - s }, { cx + s - dpx(3), cy - s }, { cx + s, cy - s + dpx(3) },
                         { cx + s, cy + s }, { cx - s, cy + s }, { cx - s, cy - s } };
        Polyline(dc, pts, 6);
        Rectangle(dc, cx - dpx(4), cy - s, cx + dpx(4), cy - dpx(3));
        Rectangle(dc, cx - dpx(5), cy + dpx(1), cx + dpx(6), cy + s);
        break;
    }
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

/* the glyph drawn soft-edged (sg-smooth.h): four times finer, averaged down */
static void draw_glyph(HDC dc, int glyph, int cx, int cy, COLORREF col)
{
    struct sg_ss ss;
    int h = dpx(12);
    HDC big = sg_ss_begin(&ss, dc, cx - h, cy - h, 2 * h + 1, 2 * h + 1, max(1, dpx(1)));
    draw_glyph_raw(big, glyph, cx, cy, col);
    sg_ss_end(&ss);
}

static BOOL btn_enabled(int i)
{
    switch (i) {
    case B_OPEN: case B_SIDEBAR: case B_TOOLS: return TRUE;
    case B_PREV: case B_NEXT: return g.nhits > 0;
    case B_PRINT: return g.npages > 0 && g.bridged;
    case B_UNDO: return g.undo > 0 && g.bridged;
    case B_REDO: return g.redo > 0 && g.bridged;
    case B_SAVE: return g.npages > 0 && g.bridged && g.dirty;
    default: return g.npages > 0;
    }
}

static BOOL btn_on(int i)
{
    return (i == B_SIDEBAR && g.side != SIDE_NONE) || (i == B_TOOLS && g.pane);
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
        COLORREF col = en ? (btn_on(i) ? C_ACCENT : C_TEXT) : C_DISABLED;
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
            WCHAR t[256];
            GetWindowTextW(g_find_edit, t, 256);
            if (g.searched && wcscmp(t, g.needle)) view_find_clear();
        }
        return 0;
    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wp, C_EDITBG);
        SetTextColor((HDC)wp, C_TEXT);
        return (LRESULT)g_editbrush;
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

/* ---- the menu -------------------------------------------------------------------------------------- */

static HMENU build_menu(void)
{
    HMENU bar = CreatePopupMenu(), file = CreatePopupMenu(), edit = CreatePopupMenu(), view = CreatePopupMenu(),
          tools = CreatePopupMenu(), help = CreatePopupMenu(), exp = CreatePopupMenu(), prot = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, CMD_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(file, MF_STRING, CMD_SAVE, L"&Save\tCtrl+S");
    AppendMenuW(file, MF_STRING, CMD_SAVEAS, L"Save &As...\tCtrl+Shift+S");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    {
        HMENU create = CreatePopupMenu();
        AppendMenuW(create, MF_STRING, CMD_CREATE_BLANK, L"&Blank Page");
        AppendMenuW(create, MF_STRING, CMD_CREATE_FILES, L"From &Files...\tCtrl+N");
        AppendMenuW(create, MF_STRING, CMD_CREATE_SCAN, L"From &Scanner...");
        AppendMenuW(create, MF_STRING, CMD_COMBINE, L"&Combine Files into a Single PDF...");
        AppendMenuW(file, MF_POPUP, (UINT_PTR)create, L"Crea&te PDF");
    }
    AppendMenuW(file, MF_STRING, CMD_COMBINE, L"Co&mbine Files...");
    AppendMenuW(file, MF_STRING, CMD_OPTIMIZE, L"Reduce File Si&ze...");
    AppendMenuW(exp, MF_STRING, CMD_EXPORT_DOCX, L"&Word Document (.docx)...");
    AppendMenuW(exp, MF_STRING, CMD_EXPORT_TXT, L"&Text (.txt)...");
    AppendMenuW(exp, MF_STRING, CMD_EXPORT_HTML, L"&HTML Web Page...");
    AppendMenuW(exp, MF_STRING, CMD_EXPORT_PNG, L"&PNG Images...");
    AppendMenuW(exp, MF_STRING, CMD_EXPORT_JPEG, L"&JPEG Images...");
    AppendMenuW(file, MF_POPUP, (UINT_PTR)exp, L"&Export To");
    AppendMenuW(prot, MF_STRING, CMD_PROTECT, L"&Encrypt with Password...");
    AppendMenuW(prot, MF_STRING, CMD_UNPROTECT, L"&Remove Security");
    AppendMenuW(prot, MF_STRING, CMD_UNLOCK, L"Enter &Permissions Password...");
    AppendMenuW(file, MF_POPUP, (UINT_PTR)prot, L"Pro&tect");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, CMD_PRINT, L"&Print...\tCtrl+P");
    AppendMenuW(file, MF_STRING, CMD_PROPERTIES, L"Propert&ies...\tCtrl+D");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, CMD_CLOSE, L"&Close\tCtrl+W");
    AppendMenuW(file, MF_STRING, CMD_EXIT, L"E&xit\tCtrl+Q");
    AppendMenuW(edit, MF_STRING, CMD_UNDO, L"&Undo\tCtrl+Z");
    AppendMenuW(edit, MF_STRING, CMD_REDO, L"&Redo\tCtrl+Y");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, CMD_COPY, L"&Copy\tCtrl+C");
    AppendMenuW(edit, MF_STRING, CMD_DELETE, L"&Delete\tDel");
    AppendMenuW(edit, MF_STRING, CMD_SELECTALL, L"Select &All\tCtrl+A");
    AppendMenuW(edit, MF_SEPARATOR, 0, NULL);
    AppendMenuW(edit, MF_STRING, CMD_FIND, L"&Find\tCtrl+F");
    AppendMenuW(edit, MF_STRING, CMD_FINDREDACT, L"Find Text to Re&dact...");
    AppendMenuW(view, MF_STRING, CMD_ZOOMIN, L"Zoom &In\tCtrl+Plus");
    AppendMenuW(view, MF_STRING, CMD_ZOOMOUT, L"Zoom &Out\tCtrl+Minus");
    AppendMenuW(view, MF_STRING, CMD_ACTUAL, L"&Actual Size\tCtrl+1");
    AppendMenuW(view, MF_STRING, CMD_FITWIDTH, L"Fit &Width\tCtrl+2");
    AppendMenuW(view, MF_STRING, CMD_FITPAGE, L"Fit &Page\tCtrl+0");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    AppendMenuW(view, MF_STRING, CMD_ROTATE, L"Rotate View &Clockwise\tCtrl+Shift+Plus");
    AppendMenuW(view, MF_STRING, CMD_ROTATE_LEFT, L"Rotate View Co&unterclockwise\tCtrl+Shift+Minus");
    AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    {
        HMENU disp = CreatePopupMenu(), nav = CreatePopupMenu(), read = CreatePopupMenu();
        AppendMenuW(disp, MF_STRING, CMD_LAYOUT_SINGLE, L"&Single Page View");
        AppendMenuW(disp, MF_STRING, CMD_LAYOUT_CONT, L"&Enable Scrolling");
        AppendMenuW(disp, MF_STRING, CMD_LAYOUT_TWO, L"&Two Page View");
        AppendMenuW(disp, MF_STRING, CMD_LAYOUT_TWOCONT, L"Two Page S&crolling");
        AppendMenuW(disp, MF_SEPARATOR, 0, NULL);
        AppendMenuW(disp, MF_STRING, CMD_COVER, L"Show Co&ver Page in Two Page View");
        AppendMenuW(view, MF_STRING, CMD_HOMETAB, L"&Home\tCtrl+Tab");
        AppendMenuW(view, MF_STRING, CMD_REOPEN, L"Reopen Documents at &Start");
        AppendMenuW(view, MF_SEPARATOR, 0, NULL);
        AppendMenuW(view, MF_POPUP, (UINT_PTR)disp, L"Page &Display");
        AppendMenuW(view, MF_STRING, CMD_NIGHT, L"Dar&k Pages");
        AppendMenuW(view, MF_SEPARATOR, 0, NULL);
        AppendMenuW(nav, MF_STRING, CMD_THUMBS, L"&Page Thumbnails");
        AppendMenuW(nav, MF_STRING, CMD_OUTLINE, L"&Bookmarks");
        AppendMenuW(nav, MF_STRING, CMD_ATTACHMENTS, L"&Attachments");
        AppendMenuW(nav, MF_STRING, CMD_SIGNATURES, L"&Signatures");
        AppendMenuW(view, MF_POPUP, (UINT_PTR)nav, L"Show Pane&l");
        AppendMenuW(read, MF_STRING, CMD_READ_PAGE, L"Read This &Page Only\tCtrl+Shift+V");
        AppendMenuW(read, MF_STRING, CMD_READ_DOC, L"Read to &End of Document\tCtrl+Shift+B");
        AppendMenuW(read, MF_STRING, CMD_READ_STOP, L"&Stop\tCtrl+Shift+E");
        AppendMenuW(view, MF_POPUP, (UINT_PTR)read, L"&Read Out Loud");
        AppendMenuW(view, MF_SEPARATOR, 0, NULL);
    }
    AppendMenuW(view, MF_STRING, CMD_SIDEBAR, L"&Navigation Pane\tF4");
    AppendMenuW(view, MF_STRING, CMD_PANE, L"&Tools Pane\tShift+F4");
    AppendMenuW(tools, MF_STRING, CMD_TOOL + TOOL_EDIT, L"&Edit PDF");
    AppendMenuW(tools, MF_STRING, CMD_TOOL + TOOL_COMMENT, L"&Comment");
    AppendMenuW(tools, MF_STRING, CMD_TOOL + TOOL_FILL, L"&Fill && Sign");
    AppendMenuW(tools, MF_STRING, CMD_TOOL + TOOL_REDACT, L"&Redact");
    AppendMenuW(tools, MF_STRING, CMD_TOOL + TOOL_ORGANIZE, L"&Organize Pages");
    AppendMenuW(tools, MF_STRING, CMD_TOOL + TOOL_FORM, L"Prepare For&m");
    AppendMenuW(tools, MF_SEPARATOR, 0, NULL);
    AppendMenuW(tools, MF_STRING, CMD_CERTSIGN, L"Si&gn with Certificate...");
    AppendMenuW(tools, MF_STRING, CMD_VALIDATE, L"&Validate All Signatures");
    AppendMenuW(tools, MF_STRING, CMD_MAKEID, L"New &Digital ID...");
    AppendMenuW(tools, MF_SEPARATOR, 0, NULL);
    AppendMenuW(tools, MF_STRING, CMD_OCR, L"Recognize &Text...");
    AppendMenuW(tools, MF_STRING, CMD_HEADFOOT, L"Add &Header && Footer...");
    AppendMenuW(tools, MF_STRING, CMD_WATERMARK, L"Add &Watermark...");
    AppendMenuW(tools, MF_STRING, CMD_BATES, L"&Bates Numbering...");
    AppendMenuW(tools, MF_STRING, CMD_PAGENUM, L"Add Page &Numbers...");
    AppendMenuW(tools, MF_STRING, CMD_ADDATTACH, L"&Attach a File...");
    AppendMenuW(tools, MF_STRING, CMD_FORM_RESET, L"Reset Form...");
    AppendMenuW(tools, MF_SEPARATOR, 0, NULL);
    AppendMenuW(tools, MF_STRING, CMD_EXPORT, L"E&xport PDF...");
    AppendMenuW(tools, MF_STRING, CMD_COMBINE, L"Combine Fi&les...");
    AppendMenuW(tools, MF_STRING, CMD_PROTECT, L"&Protect...");
    AppendMenuW(tools, MF_STRING, CMD_SANITIZE, L"Remove Hidden &Information...");
    AppendMenuW(help, MF_STRING, CMD_SHORTCUTS, L"&Keyboard Shortcuts\tCtrl+?");
    AppendMenuW(help, MF_SEPARATOR, 0, NULL);
    AppendMenuW(help, MF_STRING, CMD_ABOUT, L"&About SG PDF");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"&Edit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tools, L"&Tools");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    return bar;
}

static void menu_state(HMENU m)
{
    BOOL doc = g.npages > 0 && g.bridged;
    static const int NEED_DOC[] = { CMD_SAVEAS, CMD_PRINT, CMD_PROPERTIES, CMD_CLOSE, CMD_COPY, CMD_SELECTALL, CMD_FIND,
                                    CMD_ZOOMIN, CMD_ZOOMOUT, CMD_ACTUAL, CMD_FITWIDTH, CMD_FITPAGE, CMD_ROTATE,
                                    CMD_ROTATE_LEFT, CMD_EXPORT_DOCX, CMD_EXPORT_TXT, CMD_EXPORT_HTML, CMD_EXPORT_PNG,
                                    CMD_EXPORT_JPEG, CMD_EXPORT, CMD_TOOL + TOOL_EDIT, CMD_TOOL + TOOL_COMMENT,
                                    CMD_TOOL + TOOL_FILL, CMD_TOOL + TOOL_REDACT, CMD_TOOL + TOOL_ORGANIZE,
                                    CMD_FINDREDACT, CMD_SANITIZE, CMD_PROTECT, CMD_TOOL + TOOL_FORM, CMD_CERTSIGN,
                                    CMD_VALIDATE, CMD_OCR, CMD_HEADFOOT, CMD_WATERMARK, CMD_BATES, CMD_PAGENUM,
                                    CMD_ADDATTACH, CMD_OPTIMIZE, CMD_READ_PAGE, CMD_READ_DOC, CMD_ATTACHMENTS,
                                    CMD_SIGNATURES };
    int i;
    for (i = 0; i < (int)(sizeof(NEED_DOC) / sizeof(NEED_DOC[0])); i++)
        EnableMenuItem(m, NEED_DOC[i], MF_BYCOMMAND | (doc ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m, CMD_SAVE, MF_BYCOMMAND | (doc && g.dirty ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m, CMD_UNDO, MF_BYCOMMAND | (doc && g.undo ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m, CMD_REDO, MF_BYCOMMAND | (doc && g.redo ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m, CMD_DELETE, MF_BYCOMMAND | (doc && g.pick.kind != PICK_NONE ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m, CMD_UNPROTECT, MF_BYCOMMAND | (doc && g.encrypted && g.perms == 0xFFFF ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m, CMD_UNLOCK, MF_BYCOMMAND | (doc && g.perms != 0xFFFF ? MF_ENABLED : MF_GRAYED));
    CheckMenuItem(m, CMD_SIDEBAR, MF_BYCOMMAND | (g.side != SIDE_NONE ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, CMD_PANE, MF_BYCOMMAND | (g.pane ? MF_CHECKED : MF_UNCHECKED));
    for (i = TOOL_EDIT; i < TOOL_COUNT; i++)
        CheckMenuItem(m, CMD_TOOL + i, MF_BYCOMMAND | (g.tool == i ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(m, CMD_LAYOUT_CONT, CMD_LAYOUT_TWO, CMD_LAYOUT_CONT + g.layout, MF_BYCOMMAND);
    CheckMenuItem(m, CMD_COVER, MF_BYCOMMAND | (g.cover ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, CMD_REOPEN, MF_BYCOMMAND | (tabs_reopen_on() ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, CMD_NIGHT, MF_BYCOMMAND | (g.night ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, CMD_THUMBS, MF_BYCOMMAND | (g.side == SIDE_THUMBS ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, CMD_OUTLINE, MF_BYCOMMAND | (g.side == SIDE_OUTLINE ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, CMD_ATTACHMENTS, MF_BYCOMMAND | (g.side == SIDE_ATTACH ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, CMD_SIGNATURES, MF_BYCOMMAND | (g.side == SIDE_SIGS ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(m, CMD_READ_STOP, MF_BYCOMMAND | (g.reading ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(m, CMD_FORM_RESET, MF_BYCOMMAND | (doc && g.form ? MF_ENABLED : MF_GRAYED));
}

/* ---- the frame ---------------------------------------------------------------------------------- */

void app_layout(void)
{
    RECT rc;
    int side, pane, tb, top, h, rail, x, vw;
    BOOL home = home_shown();
    GetClientRect(g_main, &rc);
    top = 0;
    if (g_tabs) { MoveWindow(g_tabs, 0, 0, rc.right, tabbar_height(), TRUE); top = tabbar_height(); InvalidateRect(g_tabs, NULL, FALSE); }
    if (home) {
        HWND hide[] = { g_bar, g_tbar, g_side, g_sigbar, g_pane, g_view, g_org, g_rail, g_float };
        size_t i;
        for (i = 0; i < sizeof(hide) / sizeof(hide[0]); i++) if (hide[i]) ShowWindow(hide[i], SW_HIDE);
        if (g_home) { MoveWindow(g_home, 0, top, rc.right, max(0, rc.bottom - top), TRUE); ShowWindow(g_home, SW_SHOWNA); }
        return;
    }
    if (g_home) ShowWindow(g_home, SW_HIDE);
    ShowWindow(g_bar, SW_SHOWNA);
    ShowWindow(g_view, g.tool == TOOL_ORGANIZE ? SW_HIDE : SW_SHOWNA);
    if (g_org) ShowWindow(g_org, g.tool == TOOL_ORGANIZE ? SW_SHOWNA : SW_HIDE);
    side = g.side != SIDE_NONE && g.tool != TOOL_ORGANIZE ? SIDE_W : 0;
    pane = g.pane ? toolui_pane_width() : 0;
    tb = toolui_bar_height();
    rail = g_rail ? rail_width() : 0;
    MoveWindow(g_bar, 0, top, rc.right, BAR_H, TRUE);
    top += BAR_H;
    if (g_tbar) {
        MoveWindow(g_tbar, 0, top, rc.right, tb, TRUE);
        ShowWindow(g_tbar, tb ? SW_SHOWNA : SW_HIDE);
        top += tb;
    }
    h = max(0, rc.bottom - top);
    if (g_rail) { MoveWindow(g_rail, 0, top, rail, h, TRUE); ShowWindow(g_rail, SW_SHOWNA); }
    x = rail;
    vw = max(0, rc.right - x - side - pane);
    {
        int sb = sigbar_height();
        if (g_sigbar) {
            MoveWindow(g_sigbar, x + side, top, vw, sb, TRUE);
            ShowWindow(g_sigbar, sb ? SW_SHOWNA : SW_HIDE);
        }
        MoveWindow(g_side, x, top, side, h, TRUE);
        if (g_pane) {
            MoveWindow(g_pane, rc.right - pane, top, pane, h, TRUE);
            ShowWindow(g_pane, pane ? SW_SHOWNA : SW_HIDE);
        }
        top += sb;
        h = max(0, rc.bottom - top);
    }
    ShowWindow(g_side, side ? SW_SHOWNA : SW_HIDE);
    MoveWindow(g_view, x + side, top, vw, h, TRUE);
    if (g_org) MoveWindow(g_org, x + side, top, vw, h, TRUE);
    if (g_float) {
        int fw = float_width(), fh = float_height(), m = dpx(18) + GetSystemMetrics(SM_CXVSCROLL);
        BOOL show = g.npages > 0 && g.tool != TOOL_ORGANIZE && vw > fw + m && h > fh + m;
        SetWindowPos(g_float, HWND_TOP, x + side + vw - fw - m, top + h - fh - m, fw, fh, SWP_NOACTIVATE | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
}

void app_update_title(void)
{
    WCHAR t[MAX_PATH + 64];
    if (g.name[0])
        swprintf(t, MAX_PATH + 64, L"%ls%ls%ls - %ls", g.dirty ? L"*" : L"", g.name,
                 g.npages && g.perms != 0xFFFF ? L" (SECURED)" : L"", APP_NAME);
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
    frame_update();
    app_dump();
}

void app_set_status(const WCHAR *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vswprintf(g.status, 256, fmt, ap);
    va_end(ap);
    g.status[255] = 0;
    toolui_update();
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
    dumpf(f, L"side %ls\n", g.side == SIDE_THUMBS ? L"thumbs" : g.side == SIDE_OUTLINE ? L"outline" :
                            g.side == SIDE_ATTACH ? L"attachments" : g.side == SIDE_SIGS ? L"signatures" : L"none");
    dumpf(f, L"layout %d\ncover %d\nnight %d\nuntitled %d\nreading %d\n", g.layout, g.cover, g.night, g.untitled, g.reading);
    {
        WCHAR b[300];
        int level;
        sig_banner(b, 300, &level);
        if (b[0]) dumpf(f, L"sigbanner %d %ls\n", level, b);
        if (g_sigbar && IsWindowVisible(g_sigbar)) {
            RECT sr;
            GetWindowRect(g_sigbar, &sr);
            dumpf(f, L"sigbar %d %d\n", (sr.left + sr.right) / 2, (sr.top + sr.bottom) / 2);
        }
        for (i = 0; i < g.nsigs; i++)
            dumpf(f, L"sig %d %d %d %d %ls|%ls\n", g.sigs[i].page + 1, g.sigs[i].xref, g.sigs[i].state, g.sigs[i].covers,
                  g.sigs[i].name ? g.sigs[i].name : L"", g.sigs[i].signer ? g.sigs[i].signer : L"");
        for (i = 0; i < g.nattach; i++)
            dumpf(f, L"attachment %d %u %ls\n", i, (unsigned)g.attach[i].size, g.attach[i].file ? g.attach[i].file : L"");
    }
    dumpf(f, L"dark %d\n", g.dark);
    dumpf(f, L"undo %d\nredo %d\ndirty %d\nperms %u\nencrypted %d\nprotect %hs\nform %d\nredactions %d\n",
          g.undo, g.redo, g.dirty, g.perms, g.encrypted, g.protect, g.form, g.nredact);
    dumpf(f, L"generation %d\n", g.generation);
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
        if (pr.bottom < 0 || pr.top > cr.bottom || !IsWindowVisible(g_view)) continue;
        screen_rect(g_view, &pr);
        dumpf(f, L"page %d %d %d %d %d %d\n", i + 1, pr.left, pr.top, pr.right, pr.bottom,
              view_page_ready(i));
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
    if (g.side == SIDE_OUTLINE && g_tree && IsWindowVisible(g_tree)) {
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
    frame_dump(f);
    toolui_dump(f);
    tool_dump(f);
    side_dump(f);
    if (g.tool == TOOL_FORM) form_dump(f);
    org_dump(f);
    dumpf(f, L"focus %ls\n", GetFocus() == g_find_edit ? L"find" : GetFocus() == g_page_edit ? L"page" :
                             GetFocus() == g_view ? L"view" : L"other");
    {
        /* a dialog of ours in front (gates wait for it: under a Wine desktop all windows are one X window) */
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        if (fg && fg != g_main && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId()) {
            WCHAR t[256];
            GetWindowTextW(fg, t, 256);
            dumpf(f, L"dialog %ls\n", t);
        }
    }
    if (g.status[0]) dumpf(f, L"status %ls\n", g.status);
    if (g.error[0]) dumpf(f, L"error %ls\n", g.error);
    fclose(f);
    MoveFileExW(tmp, g_dump, MOVEFILE_REPLACE_EXISTING);
}

/* ---- opening ------------------------------------------------------------------------------------ */

static void free_document(void);
void app_free_document(void) { free_document(); }

static void free_document(void)
{
    int i;
    tool_cancel();
    for (i = 0; i < g.npages; i++) {
        page_t *p = &g.pages[i];
        if (p->bmp) DeleteObject(p->bmp);
        if (p->thumb) DeleteObject(p->thumb);
        doc_free_page_cache(p);
    }
    free(g.pages);
    g.pages = NULL;
    g.npages = 0;
    free(g.org_sel);
    g.org_sel = NULL;
    for (i = 0; i < g.noutline; i++) free(g.outline[i].title);
    free(g.outline);
    g.outline = NULL;
    g.noutline = 0;
    if (g_tree) TreeView_DeleteAllItems(g_tree);
    doc_free_fields();
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
    g.status[0] = 0;
    g.undo = g.redo = 0;
    g.dirty = g.encrypted = g.form = FALSE;
    g.perms = 0xFFFF;
    g.nredact = 0;
    lstrcpyA(g.protect, "keep");
    g.pick.kind = PICK_NONE;
    doc_free_sigs();
    doc_free_attach();
    g.untitled = FALSE;
    if (g.reading) read_stop();
}

/* the pages' sizes and the title from an open (or create) answer */
static void load_pages(const char *head, BYTE *data)
{
    char num[32], *s, *e;
    int n = br_field(head, "pages", num, sizeof(num)) ? atoi(num) : 0, i;
    g.pages = n > 0 ? calloc(n, sizeof(page_t)) : NULL;
    g.org_sel = n > 0 ? calloc(n, sizeof(BOOL)) : NULL;
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
}

static void shown_again(void)
{
    g.hit = -1;
    app_layout();
    view_relayout(FALSE);
    side_update();
    org_update();
    toolui_update();
    app_update_title();
    sigbar_update();
    InvalidateRect(g_view, NULL, FALSE);
    app_status_changed();
}

/* a document made here (Create PDF): untitled until saved */
BOOL app_adopt(const char *head, const BYTE *data, DWORD len, const WCHAR *name)
{
    BYTE *copy = malloc(len + 1);
    render_clear_wants(FALSE);
    render_clear_wants(TRUE);
    g.generation = next_generation();
    free_document();
    if (g.tool != TOOL_NONE) tool_set(TOOL_NONE);
    g.path[0] = 0;
    lstrcpynW(g.name, name && name[0] ? name : L"Untitled.pdf", MAX_PATH);
    if (copy) {
        memcpy(copy, data, len);
        copy[len] = 0;
        load_pages(head, copy);
        free(copy);
    }
    g.untitled = TRUE;
    g.home = FALSE;
    doc_apply_state(head, NULL, 0);
    side_load_outline();
    if (g.form) doc_load_fields();
    shown_again();
    return g.npages > 0;
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

char *unix_path(const WCHAR *path)
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
    char *upath, *eupath, req[MAX_PATH * 4 + 600], *epw, head[512];
    BYTE *data = NULL;
    DWORD len;
    int rc, tries = 0;
    if (!GetFullPathNameW(path, MAX_PATH, full, NULL)) lstrcpynW(full, path, MAX_PATH);
    g.home = FALSE;
    /* open already: its tab to the front */
    {
        int t = tab_find(full);
#ifndef SG_MUTANT_TABREUSE
        if (t >= 0) { tab_switch(t); return TRUE; }
#endif
        (void)t;
    }
    /* a tab of its own (with no document open, the first) */
    if (!tab_new()) { app_layout(); return FALSE; }
    render_clear_wants(FALSE);
    render_clear_wants(TRUE);
    g.generation = next_generation();
    free_document();
    if (g.tool != TOOL_NONE) tool_set(TOOL_NONE);
    lstrcpynW(g.path, full, MAX_PATH);
    base = wcsrchr(full, L'\\');
    lstrcpynW(g.name, base ? base + 1 : full, MAX_PATH);
    app_update_title();
    if (!g.bridged) {
        lstrcpynW(g.error, L"SG PDF needs its Linux half, sg-pdf (package sg-session).", 256);
        goto done;
    }
    if (!(upath = unix_path(full))) {
        lstrcpynW(g.error, L"That file cannot be found.", 256);
        goto done;
    }
    {
        WCHAR *wu = from_utf8(upath, -1);
        eupath = wu ? esc_utf8(wu) : _strdup(upath);
        free(wu);
    }
    for (;;) {
        /* the password travels as it is typed; a tab or new line in it cannot */
        WCHAR clean[128];
        int k;
        lstrcpynW(clean, pw, 128);
        for (k = 0; clean[k]; k++) if (clean[k] == '\t' || clean[k] == '\n' || clean[k] == '\r') clean[k] = ' ';
        epw = esc_utf8(clean);
        snprintf(req, sizeof(req), pw[0] ? "open\t%s\t%s" : "open\t%s", upath, epw ? epw : "");
        if (epw) { SecureZeroMemory(epw, strlen(epw)); free(epw); }
        SecureZeroMemory(clean, sizeof(clean));
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
    free(eupath);
    SecureZeroMemory(pw, sizeof(pw));
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
    load_pages(head, data);
    doc_apply_state(head, NULL, 0);
    if (g.npages) recent_add(g.path);
    free(data);
    if (!g.npages) lstrcpynW(g.error, L"This document has no pages.", 256);
    side_load_outline();
    if (g.form) { doc_load_fields(); doc_load_sigs(); }
done:
    shown_again();
    return g.npages > 0;
}

static void open_dialog(void)
{
    WCHAR file[MAX_PATH] = L"";
    if (file_dialog(FALSE, L"Open", L"PDF documents (*.pdf)\0*.pdf\0All files (*.*)\0*.*\0", L"pdf", file, MAX_PATH))
        app_open(file);
}

static void close_document(void)
{
    if (tab_count()) tab_close(tab_active());
    else shown_again();
}

static void save_settings(void)
{
    HKEY k;
    DWORD pane = g.pane;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    RegSetValueExW(k, L"Sidebar", 0, REG_DWORD, (const BYTE *)&g.side, sizeof(DWORD));
    RegSetValueExW(k, L"ToolsPane", 0, REG_DWORD, (const BYTE *)&pane, sizeof(DWORD));
    {
        DWORD layout = g.layout, cover = g.cover, night = g.night;
        RegSetValueExW(k, L"PageDisplay", 0, REG_DWORD, (const BYTE *)&layout, sizeof(DWORD));
        RegSetValueExW(k, L"CoverPage", 0, REG_DWORD, (const BYTE *)&cover, sizeof(DWORD));
        RegSetValueExW(k, L"DarkPages", 0, REG_DWORD, (const BYTE *)&night, sizeof(DWORD));
    }
    RegCloseKey(k);
}

static void load_settings(void)
{
    DWORD v, cb = sizeof(v);
    g.side = SIDE_NONE;
    g.pane = TRUE;
    if (!RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"Sidebar", RRF_RT_REG_DWORD, NULL, &v, &cb) && v <= SIDE_SIGS)
        g.side = (int)v;
    cb = sizeof(v);
    if (!RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"PageDisplay", RRF_RT_REG_DWORD, NULL, &v, &cb) && v <= LAYOUT_TWO) g.layout = (int)v;
    cb = sizeof(v);
    if (!RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"CoverPage", RRF_RT_REG_DWORD, NULL, &v, &cb)) g.cover = v != 0;
    cb = sizeof(v);
    if (!RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"DarkPages", RRF_RT_REG_DWORD, NULL, &v, &cb)) g.night = v != 0;
    cb = sizeof(v);
    if (!RegGetValueW(HKEY_CURRENT_USER, SETTINGS_KEY, L"ToolsPane", RRF_RT_REG_DWORD, NULL, &v, &cb)) g.pane = v != 0;
}

static void about(void)
{
    MessageBoxW(g_main,
                L"SG PDF\n\nView, edit, comment, fill and sign, redact, organize, protect and export PDF documents.\n\n"
                L"Part of Stained Glass OS. Pages are rendered and edited by MuPDF (sg-session's sg-pdf).\n"
                L"Free software under the GNU Affero General Public License, version 3 or later.",
                L"About SG PDF", MB_OK | MB_ICONINFORMATION);
}

void app_command(int cmd)
{
    if (g.home && g.npages && cmd != CMD_HOMETAB && cmd != CMD_SHORTCUTS && cmd != CMD_ABOUT && cmd != CMD_OPEN &&
        cmd != CMD_EXIT && cmd != CMD_CREATE_BLANK && cmd != CMD_CREATE_FILES && cmd != CMD_CREATE_SCAN && cmd != CMD_COMBINE)
        home_switch(FALSE);
    if (cmd >= CMD_TOOL + TOOL_EDIT && cmd < CMD_TOOL + TOOL_COUNT) {
        tool_set(g.tool == cmd - CMD_TOOL ? TOOL_NONE : cmd - CMD_TOOL);
        app_status_changed();
        return;
    }
    if (cmd >= CMD_SUB && cmd <= CMD_SUB + SUB_LAST) {
        tool_set_sub(cmd - CMD_SUB);
        app_status_changed();
        return;
    }
    if (cmd >= CMD_COLOR && cmd < CMD_COLOR + NCOLORS) {
        g.ccolor = COMMENT_COLORS[cmd - CMD_COLOR];
        toolui_update();
        app_status_changed();
        return;
    }
    if (cmd >= CMD_ORG_ROTL && cmd <= CMD_ORG_REPLACE) {
        org_command(cmd);
        app_status_changed();
        return;
    }
    switch (cmd) {
    case CMD_OPEN: open_dialog(); break;
    case CMD_SAVE: if (g.npages) doc_save(FALSE); break;
    case CMD_SAVEAS: if (g.npages) doc_save(TRUE); break;
    case CMD_CLOSE: close_document(); break;
    case CMD_EXIT: PostMessageW(g_main, WM_CLOSE, 0, 0); break;
    case CMD_UNDO: tool_commit_editor(); doc_undo(-1); break;
    case CMD_REDO: tool_commit_editor(); doc_undo(1); break;
    case CMD_PRINT: print_document(); break;
    case CMD_ZOOMIN: view_zoom_step(1); break;
    case CMD_ZOOMOUT: view_zoom_step(-1); break;
    case CMD_ACTUAL: view_set_zoom(1.0, FIT_NONE, NULL); break;
    case CMD_FITWIDTH: view_set_zoom(g.zoom, FIT_WIDTH, NULL); break;
    case CMD_FITPAGE: view_set_zoom(g.zoom, FIT_PAGE, NULL); break;
    case CMD_ROTATE: case CMD_ROTATE_LEFT: {
        int page = g.current;
        tool_commit_editor();
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
    case CMD_PANE:
        g.pane = !g.pane;
        app_layout();
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
    case CMD_SELECTALL:
        if (g.tool == TOOL_ORGANIZE) org_command(CMD_ORG_SELALL);
        else view_select_all();
        break;
    case CMD_DELETE:
        if (g.tool == TOOL_ORGANIZE) org_command(CMD_ORG_DELETE);
        else tool_delete_pick();
        break;
    case CMD_GOTOPAGE: {
        WCHAR t[16];
        GetWindowTextW(g_page_edit, t, 16);
        SetFocus(g_view);
        view_goto_page(max(1, min(_wtoi(t), g.npages)) - 1, 0);
        break;
    }
    case CMD_FIRST: view_goto_page(0, 0); break;
    case CMD_LAST: view_goto_page(g.npages - 1, 0); break;
    case CMD_PROPERTIES: if (g.npages) dlg_properties(); break;
    case CMD_EXPORT: if (g.npages) dlg_export_as(0); break;
    case CMD_EXPORT_DOCX: case CMD_EXPORT_TXT: case CMD_EXPORT_PNG: case CMD_EXPORT_JPEG: case CMD_EXPORT_HTML:
        if (g.npages) dlg_export_as(cmd - CMD_EXPORT_DOCX + 1);
        break;
    case CMD_COMBINE: dlg_combine(); break;
    case CMD_PROTECT: if (g.npages) dlg_protect(); break;
    case CMD_UNPROTECT:
        if (g.npages && MessageBoxW(g_main, L"Remove the password and the restrictions from this document? This takes effect when you save it.",
                                    L"Remove Security", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            doc_request("protect\tmode=none");
        break;
    case CMD_UNLOCK: if (g.npages) dlg_permissions_password(); break;
    case CMD_SANITIZE: if (g.npages) dlg_sanitize(FALSE); break;
    case CMD_FINDREDACT: if (g.npages) { if (g.tool != TOOL_REDACT) tool_set(TOOL_REDACT); dlg_find_redact(); } break;
    case CMD_APPLYREDACT:
        if (!g.nredact) { MessageBoxW(g_main, L"Nothing is marked for redaction yet. Mark text or an area first.", L"Redact", MB_OK | MB_ICONINFORMATION); break; }
        {
            WCHAR t[300];
            swprintf(t, 300, L"Apply %d redaction mark%ls?\n\nThe marked text, pictures and drawings are removed from the document "
                     L"permanently; they cannot be recovered after you save.", g.nredact, g.nredact == 1 ? L"" : L"s");
            if (MessageBoxW(g_main, t, L"Apply Redactions", MB_OKCANCEL | MB_ICONWARNING) == IDOK && doc_request("redactapply"))
                dlg_sanitize(TRUE);
        }
        break;
    case CMD_FLATTEN:
        if (MessageBoxW(g_main, L"Flatten the form? The filled-in values become part of the page and can no longer be changed.",
                        L"Flatten", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            doc_request("flatten\tforms=1");
        break;
    case CMD_SIGN: if (dlg_signature(g_main)) tool_set_sub(SUB_SIGN); break;
    case CMD_REPLACEIMAGE: {
        WCHAR file[MAX_PATH] = L"";
        if (g.pick.kind == PICK_OBJ && g.pick.page >= 0 && g.pick.page < g.npages &&
            g.pages[g.pick.page].objs[g.pick.index].kind == OBJ_IMAGE &&
            file_dialog(FALSE, L"Replace Image", L"Pictures\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff\0All files (*.*)\0*.*\0",
                        NULL, file, MAX_PATH)) {
            char *u = unix_path(file);
            WCHAR *wu = u ? from_utf8(u, -1) : NULL;
            char *e = wu ? esc_utf8(wu) : NULL;
            if (e) doc_requestf("replaceimage\t%d\t%d\t%s", g.pick.page, g.pages[g.pick.page].objs[g.pick.index].id, e);
            free(e); free(wu); free(u);
        }
        break;
    }
    case CMD_TOOLCLOSE: tool_set(TOOL_NONE); break;
    case CMD_SHORTCUTS: shortcuts_help(); break;
    case CMD_REOPEN: tabs_set_reopen(!tabs_reopen_on()); break;
    case CMD_HOMETAB: home_switch(TRUE); break;
    case CMD_PROPSBAR:
        if (g.tool == TOOL_FORM && form_picked() >= 0) form_props(form_picked());
        else app_command(CMD_PANE);
        break;
    case CMD_ABOUT: about(); break;
    case CMD_CREATE_BLANK: case CMD_CREATE_FILES: case CMD_CREATE_SCAN: case CMD_ORG_SCAN: case CMD_OCR: case CMD_HEADFOOT:
    case CMD_WATERMARK: case CMD_BATES: case CMD_PAGENUM: case CMD_OPTIMIZE: case CMD_ADDATTACH: case CMD_READ_PAGE:
    case CMD_READ_DOC: case CMD_READ_STOP:
        create_command(cmd);
        break;
    case CMD_FORM_DETECT: case CMD_FORM_PROPS: case CMD_FORM_DELETE: case CMD_FORM_CLEARALL:
        if (g.tool != TOOL_FORM && g.npages) tool_set(TOOL_FORM);
        form_command(cmd);
        break;
    case CMD_FORM_RESET:
        if (g.npages && MessageBoxW(g_main, L"Clear every field of the form (back to its default values)?", L"Reset Form",
                                    MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            doc_request("resetform");
        break;
    case CMD_CERTSIGN: if (g.npages) dlg_certsign(0); break;
    case CMD_MAKEID: { WCHAR f[MAX_PATH] = L""; dlg_makeid(g_main, f, MAX_PATH); break; }
    case CMD_VALIDATE: {
        WCHAR t[300];
        int level;
        if (!g.npages) break;
        doc_free_sigs();
        doc_load_sigs();
        sig_banner(t, 300, &level);
        sigbar_update();
        side_set_mode(SIDE_SIGS);
        if (!t[0]) lstrcpyW(t, g.nsigs ? L"The document's signature fields are not signed." : L"This document has no signatures.");
        app_set_status(L"%ls", t);
        if (!GetEnvironmentVariableW(L"SG_PDF_QUIET", NULL, 0))
            MessageBoxW(g_main, t, L"Signatures", MB_OK | (level == 2 ? MB_ICONWARNING : MB_ICONINFORMATION));
        break;
    }
    case CMD_ATTACHMENTS: side_set_mode(g.side == SIDE_ATTACH ? SIDE_NONE : SIDE_ATTACH); save_settings(); break;
    case CMD_SIGNATURES: side_set_mode(g.side == SIDE_SIGS ? SIDE_NONE : SIDE_SIGS); save_settings(); break;
    case CMD_LAYOUT_CONT: view_set_layout(LAYOUT_CONT, g.cover); save_settings(); break;
    case CMD_LAYOUT_TWOCONT: view_set_layout(LAYOUT_TWOCONT, g.cover); save_settings(); break;
    case CMD_LAYOUT_SINGLE: view_set_layout(LAYOUT_SINGLE, g.cover); save_settings(); break;
    case CMD_LAYOUT_TWO: view_set_layout(LAYOUT_TWO, g.cover); save_settings(); break;
    case CMD_COVER: view_set_layout(g.layout, !g.cover); save_settings(); break;
    case CMD_NIGHT:
        g.night = !g.night;
        save_settings();
        InvalidateRect(g_view, NULL, FALSE);
        side_update();
        break;
    case CMD_NEXTPAGE: view_goto_page(min(g.current + (g.layout == LAYOUT_TWO || g.layout == LAYOUT_TWOCONT ? 2 : 1), g.npages - 1), 0); break;
    case CMD_PREVPAGE: view_goto_page(max(g.current - (g.layout == LAYOUT_TWO || g.layout == LAYOUT_TWOCONT ? 2 : 1), 0), 0); break;
    case CMD_STAMPS: {
        HMENU m = CreatePopupMenu();
        POINT pt;
        int i, c;
        for (i = 0; i < NSTAMPS; i++) AppendMenuW(m, MF_STRING | (g.stamp == i ? MF_CHECKED : 0), i + 1, STAMP_LABELS[i]);
        GetCursorPos(&pt);
        c = TrackPopupMenu(m, TPM_RETURNCMD, pt.x, pt.y, 0, g_main, NULL);
        DestroyMenu(m);
        if (c > 0) {
            g.stamp = c - 1;
            if (g.tool != TOOL_COMMENT) tool_set(TOOL_COMMENT);
            tool_set_sub(SUB_STAMP);
            app_set_status(L"Click where the %ls stamp goes (or drag its box).", STAMP_LABELS[g.stamp]);
        }
        break;
    }
    }
    app_status_changed();
}

/* keys that work wherever the focus is */
static BOOL g_alt_alone;

/* a page command from the keyboard (Ctrl+Shift+I, D, R): on the pages selected in Organize Pages, else the
 * current page */
static void page_command(int cmd)
{
    if (!g.npages || home_shown()) return;
    if (g.tool != TOOL_ORGANIZE) {
        int k;
        tool_set(TOOL_ORGANIZE);
        for (k = 0; g.org_sel && k < g.npages; k++) g.org_sel[k] = k == g.current;
        org_update();
    }
    org_command(cmd);
    app_status_changed();
}

static BOOL accelerator(MSG *m)
{
    HWND focus = GetFocus();
    BOOL ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    BOOL in_edit = focus == g_find_edit || focus == g_page_edit || tool_editor_open();
    if (!IsChild(g_main, m->hwnd) && m->hwnd != g_main) return FALSE;
    /* the Menu from the keyboard: Alt (or F10) alone, or Alt and a menu's letter */
    if (m->message == WM_SYSKEYDOWN) {
        static const WCHAR KEYS[] = L"FEVTH";
        const WCHAR *k = m->wParam < 128 ? wcschr(KEYS, (WCHAR)m->wParam) : NULL;
        if (k && *k) { menu_popup((int)(k - KEYS)); return TRUE; }
        if (m->wParam == VK_F10 || m->wParam == VK_MENU) { g_alt_alone = TRUE; return m->wParam == VK_F10 ? (menu_popup(-1), TRUE) : FALSE; }
        g_alt_alone = FALSE;
        return FALSE;
    }
    if (m->message == WM_SYSKEYUP) {
        if (m->wParam == VK_MENU && g_alt_alone) { g_alt_alone = FALSE; menu_popup(-1); return TRUE; }
        return FALSE;
    }
    if (m->message != WM_KEYDOWN) return FALSE;
    g_alt_alone = FALSE;
    switch (m->wParam) {
    case VK_F3: app_command(shift ? CMD_FINDPREV : CMD_FINDNEXT); return TRUE;
    case VK_F4: app_command(shift ? CMD_PANE : CMD_SIDEBAR); return TRUE;
    }
    if (!ctrl) return FALSE;
    if (shift) {
        switch (m->wParam) {
        case VK_OEM_PLUS: case VK_ADD: app_command(CMD_ROTATE); return TRUE;
        case VK_OEM_MINUS: case VK_SUBTRACT: app_command(CMD_ROTATE_LEFT); return TRUE;
        case 'N': SetFocus(g_page_edit); SendMessageW(g_page_edit, EM_SETSEL, 0, -1); return TRUE;
        case 'V': app_command(CMD_READ_PAGE); return TRUE;
        case 'B': app_command(CMD_READ_DOC); return TRUE;
        case 'E': app_command(CMD_READ_STOP); return TRUE;
        case 'I': page_command(CMD_ORG_INSERT); return TRUE;
        case 'D': page_command(CMD_ORG_DELETE); return TRUE;
        case 'R': page_command(CMD_ORG_ROTR); return TRUE;
        case VK_OEM_2: app_command(CMD_SHORTCUTS); return TRUE;
        }
    }
    switch (m->wParam) {
    case VK_TAB: tab_cycle(shift ? -1 : 1); return TRUE;
    case 'N': app_command(CMD_CREATE_FILES); return TRUE;
    case 'E': app_command(CMD_PROPSBAR); return TRUE;
    case VK_OEM_2: app_command(CMD_SHORTCUTS); return TRUE;
    case '2': case VK_NUMPAD2: app_command(CMD_FITWIDTH); return TRUE;
    case 'O': app_command(CMD_OPEN); return TRUE;
    case 'S': app_command(shift ? CMD_SAVEAS : CMD_SAVE); return TRUE;
    case 'P': app_command(CMD_PRINT); return TRUE;
    case 'D': app_command(CMD_PROPERTIES); return TRUE;
    case 'F': app_command(CMD_FIND); return TRUE;
    case 'G': SetFocus(g_page_edit); SendMessageW(g_page_edit, EM_SETSEL, 0, -1); return TRUE;
    case 'W': app_command(CMD_CLOSE); return TRUE;
    case 'Q': PostMessageW(g_main, WM_CLOSE, 0, 0); return TRUE;
    case 'Z': if (!in_edit) { app_command(shift ? CMD_REDO : CMD_UNDO); return TRUE; } break;
    case 'Y': if (!in_edit) { app_command(CMD_REDO); return TRUE; } break;
    case VK_OEM_PLUS: case VK_ADD: app_command(CMD_ZOOMIN); return TRUE;
    case VK_OEM_MINUS: case VK_SUBTRACT: app_command(CMD_ZOOMOUT); return TRUE;
    case '0': case VK_NUMPAD0: app_command(CMD_FITPAGE); return TRUE;
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
    if (sg_mode_changed(msg, lp)) {
        app_apply_mode();
        return 0;
    }
    switch (msg) {
    case WM_NCPAINT: case WM_NCACTIVATE: {
        LRESULT r = DefWindowProcW(hwnd, msg, wp, lp);
        menu_fill_rest(hwnd);
        return r;
    }
    case WM_SIZE: app_layout(); return 0;
    case WM_TIMER:
        if (wp == 7) {
            /* the dump follows dialogs opening and closing */
            static HWND last;
            HWND fg = GetForegroundWindow();
            if (fg != last) { last = fg; app_dump(); }
            return 0;
        }
        break;
    case WM_SETFOCUS: SetFocus(home_shown() ? g_home : g.tool == TOOL_ORGANIZE && g_org ? g_org : g_view); return 0;
    case WM_INITMENUPOPUP: menu_state(g_menu); return 0;
    case WM_MEASUREITEM: if (menu_measure((MEASUREITEMSTRUCT *)lp)) return TRUE; break;
    case WM_DRAWITEM: if (menu_draw((DRAWITEMSTRUCT *)lp)) return TRUE; break;
    case WM_COMMAND:
        if (!lp && HIWORD(wp) == 0) { app_command(LOWORD(wp)); return 0; }
        break;
    case WM_DROPFILES: {
        WCHAR file[MAX_PATH];
        {
            UINT i, n = DragQueryFileW((HDROP)wp, 0xFFFFFFFF, NULL, 0);
            for (i = 0; i < n; i++) if (DragQueryFileW((HDROP)wp, i, file, MAX_PATH)) app_open(file);
        }
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = dpx(560);
        mm->ptMinTrackSize.y = dpx(360);
        return 0;
    }
    case WM_DPICHANGED: {
        RECT *r = (RECT *)lp;
        g.dpi = HIWORD(wp);
        fonts_again();
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        view_relayout(TRUE);
        return 0;
    }
    case WM_CLOSE:
        tool_commit_editor();
        if (!tabs_close_all()) return 0;
        DestroyWindow(hwnd);
        return 0;
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
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES |
                                              ICC_TAB_CLASSES | ICC_UPDOWN_CLASS };
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
    set_palette(sg_apps_dark());
    g.zoom = 1.0;
    g.fit = FIT_WIDTH;
    g.hit = -1;
    g.sel_a.page = g.sel_b.page = -1;
    g.perms = 0xFFFF;
    lstrcpyA(g.protect, "keep");
    lstrcpyW(g.fmt_font, L"Helvetica");
    g.fmt_size = 12;
    g.fmt_color = RGB(0, 0, 0);
    g.ccolor = COMMENT_COLORS[0];
    GetEnvironmentVariableW(L"SG_PDF_DUMP", g_dump, MAX_PATH);
    render_init();
    g.bridged = bridged && br_start();
    load_settings();

    g_font = make_font(100, FW_NORMAL);
    g_font_small = make_font(90, FW_NORMAL);
    g_font_bold = make_font(90, FW_SEMIBOLD);
    g_font_title = make_font(120, FW_SEMIBOLD);

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
    toolui_register();
    org_register();

    SetWindowsHookExW(WH_CALLWNDPROCRET, dialog_hook, NULL, GetCurrentThreadId());
    g_menu = g_menu_popup = build_menu();
    g_main = CreateWindowExW(WS_EX_ACCEPTFILES, CLASS_NAME, APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, dpx(1100), dpx(740), NULL, NULL, inst, NULL);
    if (!g_main) return 1;
    {
        UINT (WINAPI *for_window)(HWND) = (void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        if (for_window && for_window(g_main)) g.dpi = for_window(g_main);
    }
    sg_mode_title(g_main, g.dark);
    menu_mode();
    if (g_dump[0]) SetTimer(g_main, 7, 300, NULL);
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
    frame_create(g_main);
    side_create(g_main);
    sigbar_create(g_main);
    toolui_create(g_main);
    g_view = CreateWindowExW(0, L"SgPdfView", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP | WS_CLIPCHILDREN |
                             WS_CLIPSIBLINGS, 0, 0, 10, 10, g_main, NULL, inst, NULL);
    org_create(g_main);
    float_create(g_main);
    app_layout();
    ShowWindow(g_main, show == SW_SHOWMINNOACTIVE ? show : SW_SHOWNORMAL);
    UpdateWindow(g_main);
    SetFocus(g_view);
    if (g.bridged) render_start_thread();
    if (!file[0] && g.bridged) tabs_reopen();
    if (file[0] && app_open(file) && print_after) app_command(CMD_PRINT);
    else if (!g.bridged) lstrcpynW(g.error, L"SG PDF needs its Linux half, sg-pdf (package sg-session).", 256);
    toolui_update();
    app_status_changed();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (accelerator(&msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
