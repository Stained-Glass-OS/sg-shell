/* sg-pdf -- SG PDF: the frame around the pages, laid out as the familiar
 * professional PDF editor's current desktop look (our own drawing and words):
 *
 *   the tab strip     Menu (the whole menu as one button; Alt and Alt+F/E/V/
 *                     T/H open it from the keyboard), the Home tab and the
 *                     document's tab (its name, a close button)
 *   Home              the tools gallery (a card a tool: Edit PDF, Create PDF,
 *                     Export PDF, Comment, Combine Files, Organize Pages,
 *                     Redact, Protect, Fill & Sign, Prepare Form, Scan & OCR,
 *                     Compress, Certificates) and the recent files; a card
 *                     whose tool needs a document asks for one first
 *   the quick tools   a rail down the left: select, add a comment, highlight,
 *                     draw, add text, sign
 *   page controls     floating at the bottom right of the pages: previous and
 *                     next page, the page number, zoom out and in, fit
 *
 * The rest -- the top bar (page box, zoom, search, save, print), the "All
 * tools" pane on the right that becomes the open tool's panel, the tool's own
 * bar -- is main.c's and toolui.c's, as before.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"
#include <commdlg.h>

#define TABBAR_H dpx(38)
#define RAIL_W dpx(46)
#define RECENT_KEY L"Software\\Stained Glass\\PDF Viewer\\Recent"
#define NRECENT 12

HWND g_tabs, g_home, g_rail, g_float;
extern HMENU g_menu_popup;

/* ---- recent files ------------------------------------------------------------------------------------------ */

typedef struct { WCHAR path[MAX_PATH]; FILETIME when; } recent_t;
static recent_t g_recent[NRECENT];
static int g_nrecent;

static void recent_load(void)
{
    HKEY k;
    int i;
    g_nrecent = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RECENT_KEY, 0, KEY_QUERY_VALUE, &k)) return;
    for (i = 0; i < NRECENT; i++) {
        WCHAR name[8], v[MAX_PATH + 40], *tab;
        DWORD cb = sizeof(v), type;
        swprintf(name, 8, L"%d", i);
        if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)v, &cb) || type != REG_SZ) continue;
        v[MAX_PATH + 39] = 0;
        tab = wcschr(v, '\t');
        if (tab) {
            ULONGLONG t = _wcstoui64(tab + 1, NULL, 10);
            *tab = 0;
            g_recent[g_nrecent].when.dwLowDateTime = (DWORD)t;
            g_recent[g_nrecent].when.dwHighDateTime = (DWORD)(t >> 32);
        } else memset(&g_recent[g_nrecent].when, 0, sizeof(FILETIME));
        lstrcpynW(g_recent[g_nrecent].path, v, MAX_PATH);
        if (g_recent[g_nrecent].path[0]) g_nrecent++;
    }
    RegCloseKey(k);
}

static void recent_save(void)
{
    HKEY k;
    int i;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RECENT_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    for (i = 0; i < NRECENT; i++) {
        WCHAR name[8], v[MAX_PATH + 40];
        swprintf(name, 8, L"%d", i);
        if (i >= g_nrecent) { RegDeleteValueW(k, name); continue; }
        swprintf(v, MAX_PATH + 40, L"%ls\t%I64u", g_recent[i].path,
                 ((ULONGLONG)g_recent[i].when.dwHighDateTime << 32) | g_recent[i].when.dwLowDateTime);
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (lstrlenW(v) + 1) * sizeof(WCHAR));
    }
    RegCloseKey(k);
}

void recent_add(const WCHAR *path)
{
    recent_t r;
    int i, k = 0;
    if (!path || !path[0]) return;
    recent_load();
    lstrcpynW(r.path, path, MAX_PATH);
    GetSystemTimeAsFileTime(&r.when);
    for (i = 0; i < g_nrecent; i++) if (_wcsicmp(g_recent[i].path, path)) g_recent[k++] = g_recent[i];
    g_nrecent = min(k, NRECENT - 1);
    memmove(&g_recent[1], &g_recent[0], g_nrecent * sizeof(recent_t));
    g_recent[0] = r;
    g_nrecent++;
    recent_save();
    if (g_home) InvalidateRect(g_home, NULL, FALSE);
}

/* ---- where things are ----------------------------------------------------------------------------------------- */

BOOL home_shown(void)
{
    return g.home || (!g.npages && !g.path[0]);
}

/* ---- the tab strip ------------------------------------------------------------------------------------------- */

enum { TB_MENU, TB_HOME, TB_N };
#define HIT_DOC 100          /* + the tab's index */
#define HIT_CLOSE 200        /* + the tab's index */
static RECT g_tab_rc[TB_N], g_doc_rc[32], g_close_rc[32];
static int g_tab_hover = -1;
static int g_drag = -1, g_drag_to = -1, g_drag_x;   /* a tab dragged to another place */

static void tab_label(int i, WCHAR *t, int cap)
{
    const app_t *d = tab_doc(i);
    lstrcpynW(t, d && d->name[0] ? d->name : L"Untitled", cap);
}

static void tabs_layout(HDC dc)
{
    RECT rc;
    SIZE sz;
    int x = dpx(6), h = TABBAR_H, w, i, n = tab_count(), avail;
    GetClientRect(g_tabs, &rc);
    SelectObject(dc, g_font);
    GetTextExtentPoint32W(dc, L"Menu", 4, &sz);
    SetRect(&g_tab_rc[TB_MENU], x, dpx(4), x + dpx(34) + sz.cx, h - dpx(4));
    x = g_tab_rc[TB_MENU].right + dpx(10);
    GetTextExtentPoint32W(dc, L"Home", 4, &sz);
    SetRect(&g_tab_rc[TB_HOME], x, dpx(4), x + dpx(44) + sz.cx, h);
    x = g_tab_rc[TB_HOME].right + dpx(2);
    avail = rc.right - x - dpx(8);
    for (i = 0; i < n && i < 32; i++) {
        WCHAR t[MAX_PATH];
        tab_label(i, t, MAX_PATH);
        SelectObject(dc, g_font_bold);     /* the tab in front is bold: room for that */
        GetTextExtentPoint32W(dc, t, lstrlenW(t), &sz);
        w = min(dpx(240), max(dpx(110), sz.cx + dpx(76) + (tab_doc(i)->dirty ? dpx(14) : 0)));
        if (n * (w + dpx(2)) > avail) w = max(dpx(72), avail / n - dpx(2));
        SetRect(&g_doc_rc[i], x, dpx(4), x + w, h);
        SetRect(&g_close_rc[i], x + w - dpx(28), dpx(4) + (h - dpx(4) - dpx(20)) / 2, x + w - dpx(8),
                dpx(4) + (h - dpx(4) - dpx(20)) / 2 + dpx(20));
        x += w + dpx(2);
    }
}

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, r, b);
    DeleteObject(b);
}

static void tabs_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC out = BeginPaint(hwnd, &ps), dc;
    RECT rc, r;
    HBITMAP buf, ob;
    BOOL home = home_shown();
    int i, n = min(tab_count(), 32);
    GetClientRect(hwnd, &rc);
    dc = CreateCompatibleDC(out);
    buf = CreateCompatibleBitmap(out, max(rc.right, 1), max(rc.bottom, 1));
    ob = SelectObject(dc, buf);
    fill(dc, &rc, C_SIDE);
    SetBkMode(dc, TRANSPARENT);
    tabs_layout(dc);
    /* Menu */
    r = g_tab_rc[TB_MENU];
    if (g_tab_hover == TB_MENU) fill(dc, &r, C_HOVER);
    pdf_glyph(dc, T_MENU, r.left + dpx(16), (r.top + r.bottom) / 2, dpx(14), C_TEXT, C_ACCENT);
    r.left += dpx(30);
    SetTextColor(dc, C_TEXT);
    SelectObject(dc, g_font);
    DrawTextW(dc, L"Menu", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    /* Home */
    r = g_tab_rc[TB_HOME];
    fill(dc, &r, home ? C_BAR : g_tab_hover == TB_HOME ? C_HOVER : C_SIDE);
    pdf_glyph(dc, T_HOME, r.left + dpx(18), (r.top + r.bottom) / 2, dpx(14), home ? C_ACCENT : C_TEXT, C_ACCENT);
    r.left += dpx(34);
    SelectObject(dc, home ? g_font_bold : g_font);
    SetTextColor(dc, C_TEXT);
    DrawTextW(dc, L"Home", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (home) { RECT u = g_tab_rc[TB_HOME]; u.bottom = u.top + dpx(2); fill(dc, &u, C_ACCENT); }
    /* the documents */
    for (i = 0; i < n; i++) {
        BOOL on = !home && i == tab_active();
        const app_t *d = tab_doc(i);
        WCHAR t[MAX_PATH];
        r = g_doc_rc[i];
        if (g_drag >= 0 && i == g_drag) OffsetRect(&r, g_drag_x, 0);
        fill(dc, &r, on ? C_BAR : g_tab_hover == HIT_DOC + i ? C_HOVER : C_SIDE);
        if (on) { RECT u = r; u.bottom = u.top + dpx(2); fill(dc, &u, C_ACCENT); }
        { RECT sep = { r.right, r.top + dpx(8), r.right + 1, r.bottom - dpx(6) }; fill(dc, &sep, C_LINE); }
        pdf_glyph(dc, T_CREATE, r.left + dpx(16), (r.top + r.bottom) / 2, dpx(12), C_ACCENT, C_ACCENT);
        {
            RECT tr = r;
            tr.left += dpx(30);
            tr.right = r.right - dpx(32) - (d->dirty ? dpx(12) : 0);
            tab_label(i, t, MAX_PATH);
            SelectObject(dc, on ? g_font_bold : g_font);
            SetTextColor(dc, C_TEXT);
            DrawTextW(dc, t, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        {
            RECT x = g_close_rc[i];
            if (g_drag >= 0 && i == g_drag) OffsetRect(&x, g_drag_x, 0);
            if (d->dirty) {
                /* unsaved changes: a dot before the close button */
                int cx = x.left - dpx(8), cy = (x.top + x.bottom) / 2;
                pdf_glyph(dc, T_DOT, cx, cy, dpx(10), C_ACCENT, C_ACCENT);
            }
            if (g_tab_hover == HIT_CLOSE + i) fill(dc, &x, C_PRESS);
            pdf_glyph(dc, T_CLOSE, (x.left + x.right) / 2, (x.top + x.bottom) / 2, dpx(12), C_SUBTEXT, C_ACCENT);
        }
    }
    if (g_drag >= 0 && g_drag_to >= 0 && g_drag_to != g_drag) {
        RECT mark = g_doc_rc[g_drag_to];
        mark.right = mark.left + dpx(3);
        if (g_drag_to > g_drag) { mark.left = g_doc_rc[g_drag_to].right - dpx(3); mark.right = g_doc_rc[g_drag_to].right; }
        fill(dc, &mark, C_ACCENT);
    }
    SetRect(&r, 0, rc.bottom - 1, rc.right, rc.bottom);
    fill(dc, &r, C_LINE);
    BitBlt(out, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(buf);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static int tabs_hit(POINT pt)
{
    int i, n = min(tab_count(), 32);
    for (i = 0; i < n; i++) {
        if (PtInRect(&g_close_rc[i], pt)) return HIT_CLOSE + i;
        if (PtInRect(&g_doc_rc[i], pt)) return HIT_DOC + i;
    }
    if (PtInRect(&g_tab_rc[TB_HOME], pt)) return TB_HOME;
    if (PtInRect(&g_tab_rc[TB_MENU], pt)) return TB_MENU;
    return -1;
}

/* the tab a dragged tab would go to, for x */
static int drop_index(int x)
{
    int i, n = min(tab_count(), 32);
    for (i = 0; i < n; i++) if (x < (g_doc_rc[i].left + g_doc_rc[i].right) / 2) return i > g_drag ? i - 1 : i;
    return n - 1;
}

/* the menu: all of it from the Menu button (index -1), or one of its menus (Alt+F, Alt+E ...) */
void menu_popup(int index)
{
    RECT r = g_tab_rc[TB_MENU];
    HMENU m = index < 0 ? g_menu_popup : GetSubMenu(g_menu_popup, index);
    if (!m) return;
    MapWindowPoints(g_tabs, NULL, (POINT *)&r, 2);
    g.menu_open = TRUE;
    app_dump();
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, g_main, NULL);
    g.menu_open = FALSE;
    app_dump();
}

void home_switch(BOOL home)
{
#ifdef SG_MUTANT_TABS
    home = g.home;
#endif
    if (home && !g.home) tool_commit_editor();
    g.home = home && (g.npages || g.path[0]) ? TRUE : home;
    app_layout();
    InvalidateRect(g_tabs, NULL, FALSE);
    if (!home_shown()) SetFocus(g.tool == TOOL_ORGANIZE && g_org ? g_org : g_view);
    else SetFocus(g_home);
    app_status_changed();
}

static LRESULT CALLBACK tabs_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    static POINT down;
    switch (msg) {
    case WM_PAINT: tabs_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        int h = tabs_hit(pt);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        if (g_drag >= 0 && GetCapture() == hwnd) {
            if (abs(pt.x - down.x) > dpx(6) || g_drag_x) {
                g_drag_x = pt.x - down.x;
                g_drag_to = drop_index(pt.x);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (h != g_tab_hover) { g_tab_hover = h; InvalidateRect(hwnd, NULL, FALSE); }
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: g_tab_hover = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        int h = tabs_hit(pt);
        if (h == TB_MENU) menu_popup(-1);
        else if (h == TB_HOME) home_switch(TRUE);
        else if (h >= HIT_CLOSE) tab_close(h - HIT_CLOSE);
        else if (h >= HIT_DOC) {
            tab_switch(h - HIT_DOC);
            g_drag = h - HIT_DOC;
            g_drag_to = -1;
            g_drag_x = 0;
            down = pt;
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_drag >= 0) {
            int from = g_drag, to = g_drag_to;
            g_drag = g_drag_to = -1;
            g_drag_x = 0;
            if (GetCapture() == hwnd) ReleaseCapture();
            if (to >= 0 && to != from) tab_move(from, to);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (g_drag >= 0) { g_drag = g_drag_to = -1; g_drag_x = 0; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_MBUTTONUP: {
        int h = tabs_hit(pt);
        if (h >= HIT_DOC && h < HIT_CLOSE) tab_close(h - HIT_DOC);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- Home --------------------------------------------------------------------------------------------------- */

typedef struct { int cmd, glyph; const WCHAR *name, *desc; const char *key; BOOL needs_doc; } card_t;
static const card_t CARDS[] = {
    { CMD_TOOL + TOOL_EDIT, T_EDIT, L"Edit PDF", L"Change text and pictures, add links", "edit", TRUE },
    { CMD_CREATE_FILES, T_CREATE, L"Create PDF", L"From files, pictures, office documents", "create", FALSE },
    { CMD_EXPORT, T_EXPORT, L"Export PDF", L"To Word, text, web page or pictures", "export", TRUE },
    { CMD_TOOL + TOOL_COMMENT, T_COMMENT, L"Comment", L"Notes, highlights, drawings, stamps", "comment", TRUE },
    { CMD_COMBINE, T_COMBINE, L"Combine Files", L"Several files into one PDF", "combine", FALSE },
    { CMD_TOOL + TOOL_ORGANIZE, T_ORGANIZE, L"Organize Pages", L"Reorder, rotate, insert, delete", "organize", TRUE },
    { CMD_TOOL + TOOL_REDACT, T_REDACT, L"Redact", L"Remove content for good", "redact", TRUE },
    { CMD_PROTECT, T_PROTECT, L"Protect", L"Passwords and permissions", "protect", TRUE },
    { CMD_TOOL + TOOL_FILL, T_FILL, L"Fill & Sign", L"Fill in forms, sign", "fill", TRUE },
    { CMD_TOOL + TOOL_FORM, T_FORM, L"Prepare Form", L"Make a fillable form", "form", TRUE },
    { CMD_OCR, T_OCR, L"Scan & OCR", L"Recognize text in scans", "ocr", TRUE },
    { CMD_OPTIMIZE, T_COMPRESS, L"Compress", L"Make the file smaller", "compress", TRUE },
    { CMD_CERTSIGN, T_CERT, L"Certificates", L"Sign with a digital ID, validate", "certificates", TRUE },
};
#define NCARDS ((int)(sizeof(CARDS) / sizeof(CARDS[0])))

static RECT g_card_rc[NCARDS], g_recent_rc[NRECENT], g_open_rc, g_scanbtn_rc;
static int g_home_hover = -1, g_home_scroll, g_home_h;

static void home_layout(HDC dc)
{
    RECT rc;
    int x0 = dpx(32), y = dpx(24) - g_home_scroll, cw = dpx(214), ch = dpx(84), gap = dpx(12), cols, i, w;
    (void)dc;
    GetClientRect(g_home, &rc);
    w = rc.right - 2 * x0;
    cols = max(1, (w + gap) / (cw + gap));
    SetRect(&g_open_rc, x0, y + dpx(44), x0 + dpx(150), y + dpx(78));
    SetRect(&g_scanbtn_rc, x0 + dpx(160), y + dpx(44), x0 + dpx(320), y + dpx(78));
    y += dpx(100) + dpx(34);
    for (i = 0; i < NCARDS; i++) {
        int c = i % cols, r = i / cols;
        SetRect(&g_card_rc[i], x0 + c * (cw + gap), y + r * (ch + gap), x0 + c * (cw + gap) + cw, y + r * (ch + gap) + ch);
    }
    y += ((NCARDS + cols - 1) / cols) * (ch + gap) + dpx(18) + dpx(34);
    for (i = 0; i < NRECENT; i++) {
        if (i < g_nrecent) SetRect(&g_recent_rc[i], x0, y + i * dpx(42), rc.right - x0, y + i * dpx(42) + dpx(40));
        else SetRectEmpty(&g_recent_rc[i]);
    }
    y += max(1, g_nrecent) * dpx(42) + dpx(24);
    g_home_h = y + g_home_scroll;
}

static void home_scrollbar(void)
{
    RECT rc;
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    GetClientRect(g_home, &rc);
    g_home_scroll = max(0, min(g_home_scroll, g_home_h - rc.bottom));
    si.nMax = max(0, g_home_h - 1);
    si.nPage = rc.bottom;
    si.nPos = g_home_scroll;
    SetScrollInfo(g_home, SB_VERT, &si, TRUE);
}

static void heading(HDC dc, int x, int y, const WCHAR *t)
{
    RECT r = { x, y, x + dpx(600), y + dpx(30) };
    SelectObject(dc, g_font_title);
    SetTextColor(dc, C_TEXT);
    DrawTextW(dc, t, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void when_text(const FILETIME *ft, WCHAR *out, int cap)
{
    SYSTEMTIME st, now;
    FILETIME lft;
    out[0] = 0;
    if (!ft->dwLowDateTime && !ft->dwHighDateTime) return;
    FileTimeToLocalFileTime(ft, &lft);
    FileTimeToSystemTime(&lft, &st);
    GetLocalTime(&now);
    if (st.wYear == now.wYear && st.wMonth == now.wMonth && st.wDay == now.wDay)
        GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, out, cap);
    else GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, out, cap);
}

static void home_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC out = BeginPaint(hwnd, &ps), dc;
    RECT rc, r;
    HBITMAP buf, ob;
    int i, x0 = dpx(32);
    GetClientRect(hwnd, &rc);
    dc = CreateCompatibleDC(out);
    buf = CreateCompatibleBitmap(out, max(rc.right, 1), max(rc.bottom, 1));
    ob = SelectObject(dc, buf);
    fill(dc, &rc, C_CANVAS);
    SetBkMode(dc, TRANSPARENT);
    home_layout(dc);
    /* welcome */
    {
        RECT t = { x0, dpx(24) - g_home_scroll, rc.right - x0, dpx(24) - g_home_scroll + dpx(36) };
        HFONT big = CreateFontW(-MulDiv(170, g.dpi, 720), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                                L"Segoe UI"), of = SelectObject(dc, big);
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, L"Welcome to SG PDF", -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of);
        DeleteObject(big);
    }
    /* Open a file, Create from scanner */
    r = g_open_rc;
    fill(dc, &r, g_home_hover == 100 ? RGB(GetRValue(C_ACCENT) * 4 / 5, GetGValue(C_ACCENT) * 4 / 5, GetBValue(C_ACCENT) * 4 / 5)
                                     : C_ACCENT);
    pdf_glyph(dc, T_OPENFILE, r.left + dpx(20), (r.top + r.bottom) / 2, dpx(14), RGB(0xFF, 0xFF, 0xFF), C_ACCENT);
    r.left += dpx(38);
    SelectObject(dc, g_font_bold);
    SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    DrawTextW(dc, L"Open a file", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    r = g_scanbtn_rc;
    {
        RECT o = r;
        InflateRect(&o, 1, 1);
        fill(dc, &o, C_LINE);
        fill(dc, &r, g_home_hover == 101 ? C_HOVER : C_BAR);
    }
    pdf_glyph(dc, T_SCAN, r.left + dpx(20), (r.top + r.bottom) / 2, dpx(14), C_TEXT, C_ACCENT);
    r.left += dpx(38);
    SelectObject(dc, g_font);
    SetTextColor(dc, C_TEXT);
    DrawTextW(dc, L"Scan a document", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    /* tools */
    heading(dc, x0, g_card_rc[0].top - dpx(38), L"All tools");
    for (i = 0; i < NCARDS; i++) {
        RECT c = g_card_rc[i], o = c, t;
        InflateRect(&o, 1, 1);
        fill(dc, &o, g_home_hover == i ? C_ACCENT : C_LINE);
        fill(dc, &c, g_home_hover == i ? C_HOVER : C_BAR);
        {
            RECT bg = { c.left + dpx(12), c.top + dpx(14), c.left + dpx(52), c.top + dpx(54) };
            fill(dc, &bg, C_ACTIVE);
            pdf_glyph(dc, CARDS[i].glyph, (bg.left + bg.right) / 2, (bg.top + bg.bottom) / 2, dpx(22), C_ACCENT, C_ACCENT);
        }
        SetRect(&t, c.left + dpx(64), c.top + dpx(14), c.right - dpx(8), c.top + dpx(36));
        SelectObject(dc, g_font_bold);
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, CARDS[i].name, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetRect(&t, c.left + dpx(64), c.top + dpx(36), c.right - dpx(8), c.bottom - dpx(6));
        SelectObject(dc, g_font_small);
        SetTextColor(dc, C_SUBTEXT);
        DrawTextW(dc, CARDS[i].desc, -1, &t, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    /* recent */
    heading(dc, x0, g_card_rc[NCARDS - 1].bottom + dpx(18), L"Recent files");
    if (!g_nrecent) {
        RECT t = { x0, g_card_rc[NCARDS - 1].bottom + dpx(52), rc.right - x0, g_card_rc[NCARDS - 1].bottom + dpx(90) };
        SelectObject(dc, g_font);
        SetTextColor(dc, C_SUBTEXT);
        DrawTextW(dc, L"The PDF files you open are listed here.", -1, &t, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }
    for (i = 0; i < g_nrecent; i++) {
        RECT row = g_recent_rc[i], t;
        WCHAR name[MAX_PATH], folder[MAX_PATH], when[64], *slash;
        lstrcpynW(folder, g_recent[i].path, MAX_PATH);
        slash = wcsrchr(folder, L'\\');
        lstrcpynW(name, slash ? slash + 1 : folder, MAX_PATH);
        if (slash) *slash = 0; else folder[0] = 0;
        fill(dc, &row, g_home_hover == 200 + i ? C_HOVER : C_BAR);
        pdf_glyph(dc, T_CREATE, row.left + dpx(20), (row.top + row.bottom) / 2, dpx(14), C_ACCENT, C_ACCENT);
        SetRect(&t, row.left + dpx(42), row.top, row.left + (row.right - row.left) / 2, row.bottom);
        SelectObject(dc, g_font);
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, name, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SetRect(&t, row.left + (row.right - row.left) / 2, row.top, row.right - dpx(130), row.bottom);
        SelectObject(dc, g_font_small);
        SetTextColor(dc, C_SUBTEXT);
        DrawTextW(dc, folder, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        when_text(&g_recent[i].when, when, 64);
        SetRect(&t, row.right - dpx(124), row.top, row.right - dpx(12), row.bottom);
        DrawTextW(dc, when, -1, &t, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    if (g.error[0] && !g.path[0]) {
        RECT t = { x0, rc.bottom - dpx(40), rc.right - x0, rc.bottom - dpx(12) };
        SelectObject(dc, g_font);
        SetTextColor(dc, C_REDMARK);
        DrawTextW(dc, g.error, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    BitBlt(out, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(buf);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
    home_scrollbar();
}

static int home_hit(POINT pt)
{
    int i;
    if (PtInRect(&g_open_rc, pt)) return 100;
    if (PtInRect(&g_scanbtn_rc, pt)) return 101;
    for (i = 0; i < NCARDS; i++) if (PtInRect(&g_card_rc[i], pt)) return i;
    for (i = 0; i < g_nrecent; i++) if (PtInRect(&g_recent_rc[i], pt)) return 200 + i;
    return -1;
}

/* a tool card: its tool on the open document (asking for one when there is none) */
void home_card(int i)
{
    const card_t *c;
    if (i < 0 || i >= NCARDS) return;
    c = &CARDS[i];
    if (c->needs_doc && !g.npages) {
        WCHAR file[MAX_PATH] = L"";
        if (!file_dialog(FALSE, L"Open a File to Use the Tool With", L"PDF documents (*.pdf)\0*.pdf\0All files (*.*)\0*.*\0",
                         L"pdf", file, MAX_PATH) || !app_open(file))
            return;
    }
    if (c->needs_doc) home_switch(FALSE);
#ifdef SG_MUTANT_HOMECARD
    return;
#endif
    if (c->cmd >= CMD_TOOL && c->cmd < CMD_TOOL + TOOL_COUNT) {
        if (g.tool != c->cmd - CMD_TOOL) tool_set(c->cmd - CMD_TOOL);
        if (!g.pane) app_command(CMD_PANE);
    } else app_command(c->cmd);
    if (g.npages && !c->needs_doc) home_switch(FALSE);
}

static LRESULT CALLBACK home_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: home_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_SHOWWINDOW: if (wp) recent_load(); break;
    case WM_MOUSEWHEEL:
        g_home_scroll -= GET_WHEEL_DELTA_WPARAM(wp) * dpx(80) / WHEEL_DELTA;
        home_scrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (LOWORD(wp)) {
        case SB_LINEUP: g_home_scroll -= dpx(40); break;
        case SB_LINEDOWN: g_home_scroll += dpx(40); break;
        case SB_PAGEUP: g_home_scroll -= (int)si.nPage; break;
        case SB_PAGEDOWN: g_home_scroll += (int)si.nPage; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: g_home_scroll = si.nTrackPos; break;
        }
        home_scrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = home_hit(pt);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        if (h != g_home_hover) { g_home_hover = h; InvalidateRect(hwnd, NULL, FALSE); }
        SetCursor(LoadCursorW(NULL, (LPCWSTR)(h >= 0 ? IDC_HAND : IDC_ARROW)));
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: g_home_hover = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = home_hit(pt);
        SetFocus(hwnd);
        if (h == 100) app_command(CMD_OPEN);
        else if (h == 101) app_command(CMD_CREATE_SCAN);
        else if (h >= 200 && h < 200 + g_nrecent) {
            WCHAR p[MAX_PATH];
            lstrcpynW(p, g_recent[h - 200].path, MAX_PATH);
            app_open(p);
        } else if (h >= 0 && h < NCARDS) home_card(h);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_NEXT) { g_home_scroll += dpx(300); home_scrollbar(); InvalidateRect(hwnd, NULL, FALSE); }
        if (wp == VK_PRIOR) { g_home_scroll -= dpx(300); home_scrollbar(); InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the quick tools rail ------------------------------------------------------------------------------------ */

typedef struct { int glyph; const WCHAR *tip; const char *name; } rail_t;
static const rail_t RAIL[] = {
    { T_SELECT, L"Select text and images", "select" },
    { T_NOTE, L"Add a comment (sticky note)", "comment" },
    { T_HIGHLIGHT, L"Highlight text", "highlight" },
    { T_PEN, L"Draw free form", "draw" },
    { T_TEXT, L"Add text anywhere", "text" },
    { T_SIGN, L"Sign: a handwritten signature or a certificate", "sign" },
};
#define NRAIL ((int)(sizeof(RAIL) / sizeof(RAIL[0])))
static RECT g_rail_rc[NRAIL];
static int g_rail_hover = -1;
static HWND g_rail_tip;

static BOOL rail_on(int i)
{
    switch (i) {
    case 0: return g.tool == TOOL_NONE;
    case 1: return g.tool == TOOL_COMMENT && g.sub == SUB_NOTE;
    case 2: return g.tool == TOOL_COMMENT && g.sub == SUB_HIGHLIGHT;
    case 3: return g.tool == TOOL_COMMENT && g.sub == SUB_INK;
    case 4: return g.tool == TOOL_FILL && g.sub == SUB_FILLTEXT;
    case 5: return g.tool == TOOL_FILL && (g.sub == SUB_SIGN || g.sub == SUB_CERTSIGN);
    }
    return FALSE;
}

void rail_command(int i)
{
    static const int TOOL[NRAIL] = { TOOL_NONE, TOOL_COMMENT, TOOL_COMMENT, TOOL_COMMENT, TOOL_FILL, TOOL_FILL };
    static const int SUB[NRAIL] = { SUB_SELECT, SUB_NOTE, SUB_HIGHLIGHT, SUB_INK, SUB_FILLTEXT, -1 };
    if (i < 0 || i >= NRAIL || !g.npages) return;
#ifdef SG_MUTANT_RAIL
    i = 0;
#endif
    if (i == 5) {
        /* sign: by hand (typed, drawn, a picture) or with a certificate */
        HMENU m = CreatePopupMenu();
        RECT r = g_rail_rc[5];
        int c;
        AppendMenuW(m, MF_STRING, CMD_SIGN, L"Add a &handwritten signature...");
        AppendMenuW(m, MF_STRING, CMD_CERTSIGN, L"Sign with a &certificate...");
        MapWindowPoints(g_rail, NULL, (POINT *)&r, 2);
        g.menu_open = TRUE;
        app_dump();
        c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.right, r.top, 0, g_main, NULL);
        g.menu_open = FALSE;
        DestroyMenu(m);
        if (c) {
            if (g.tool != TOOL_FILL) tool_set(TOOL_FILL);
            app_command(c);
        }
        if (g_rail) InvalidateRect(g_rail, NULL, FALSE);
        app_status_changed();
        return;
    }
    if (g.tool != TOOL[i]) tool_set(TOOL[i]);
    if (i == 5) app_command(CMD_SIGN);
    else if (TOOL[i] != TOOL_NONE) tool_set_sub(SUB[i]);
    if (g_rail) InvalidateRect(g_rail, NULL, FALSE);
    app_status_changed();
}

static void rail_layout(void)
{
    int i, b = dpx(36), x = (RAIL_W - b) / 2, y = dpx(10);
    for (i = 0; i < NRAIL; i++) {
        SetRect(&g_rail_rc[i], x, y, x + b, y + b);
        y += b + dpx(6);
        if (i == 0) y += dpx(8);
    }
}

static void rail_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC out = BeginPaint(hwnd, &ps), dc;
    RECT rc, r;
    HBITMAP buf, ob;
    int i;
    GetClientRect(hwnd, &rc);
    dc = CreateCompatibleDC(out);
    buf = CreateCompatibleBitmap(out, max(rc.right, 1), max(rc.bottom, 1));
    ob = SelectObject(dc, buf);
    fill(dc, &rc, C_BAR);
    rail_layout();
    for (i = 0; i < NRAIL; i++) {
        BOOL on = rail_on(i), en = g.npages > 0 && g.bridged;
        r = g_rail_rc[i];
        if (on) fill(dc, &r, C_ACTIVE);
        else if (i == g_rail_hover && en) fill(dc, &r, C_HOVER);
        pdf_glyph(dc, RAIL[i].glyph, (r.left + r.right) / 2, (r.top + r.bottom) / 2, dpx(18),
                  !en ? C_DISABLED : on ? C_ACCENT : C_TEXT, i == 2 ? RGB(0xFF, 0xD8, 0x00) : C_ACCENT);
        if (i == 0) { RECT s = { r.left + dpx(4), r.bottom + dpx(6), r.right - dpx(4), r.bottom + dpx(7) }; fill(dc, &s, C_LINE); }
    }
    SetRect(&r, rc.right - 1, 0, rc.right, rc.bottom);
    fill(dc, &r, C_LINE);
    BitBlt(out, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(buf);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK rail_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    int i, h = -1;
    for (i = 0; i < NRAIL; i++) if (PtInRect(&g_rail_rc[i], pt)) h = i;
    switch (msg) {
    case WM_PAINT: rail_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE:
        if (h != g_rail_hover) {
            TTTOOLINFOW ti = { sizeof(ti) };
            g_rail_hover = h;
            InvalidateRect(hwnd, NULL, FALSE);
            if (g_rail_tip) {
                ti.hwnd = hwnd;
                ti.uId = 1;
                ti.lpszText = (WCHAR *)(h >= 0 ? RAIL[h].tip : L"");
                if (h >= 0) ti.rect = g_rail_rc[h];
                SendMessageW(g_rail_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
                SendMessageW(g_rail_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
            }
        }
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    case WM_MOUSELEAVE: g_rail_hover = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: if (h >= 0) rail_command(h); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the floating page controls ------------------------------------------------------------------------------ */

enum { FL_PREV, FL_PAGE, FL_NEXT, FL_ZOOMOUT, FL_ZOOMIN, FL_FIT, FL_N };
static const char *const FL_NAMES[FL_N] = { "prev", "page", "next", "zoomout", "zoomin", "fit" };
static RECT g_fl_rc[FL_N];
static int g_fl_hover = -1;

int float_width(void) { return dpx(250); }
int float_height(void) { return dpx(40); }

static void float_layout(void)
{
    int b = dpx(32), x = dpx(4), y = (float_height() - b) / 2;
    SetRect(&g_fl_rc[FL_PREV], x, y, x + b, y + b); x += b;
    SetRect(&g_fl_rc[FL_PAGE], x, y, x + dpx(72), y + b); x += dpx(72);
    SetRect(&g_fl_rc[FL_NEXT], x, y, x + b, y + b); x += b + dpx(10);
    SetRect(&g_fl_rc[FL_ZOOMOUT], x, y, x + b, y + b); x += b;
    SetRect(&g_fl_rc[FL_ZOOMIN], x, y, x + b, y + b); x += b;
    SetRect(&g_fl_rc[FL_FIT], x, y, x + b, y + b);
}

static void float_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC out = BeginPaint(hwnd, &ps), dc;
    RECT rc, r;
    HBITMAP buf, ob;
    static const int G[FL_N] = { T_UP, 0, T_DOWN, T_MINUS, T_PLUS, T_FITG };
    int i;
    WCHAR t[32];
    GetClientRect(hwnd, &rc);
    dc = CreateCompatibleDC(out);
    buf = CreateCompatibleBitmap(out, max(rc.right, 1), max(rc.bottom, 1));
    ob = SelectObject(dc, buf);
    fill(dc, &rc, C_LINE);
    r = rc;
    InflateRect(&r, -1, -1);
    fill(dc, &r, C_BAR);
    SetBkMode(dc, TRANSPARENT);
    float_layout();
    for (i = 0; i < FL_N; i++) {
        r = g_fl_rc[i];
        if (i == FL_PAGE) {
            swprintf(t, 32, L"%d / %d", g.npages ? g.current + 1 : 0, g.npages);
            SelectObject(dc, g_font);
            SetTextColor(dc, C_TEXT);
            DrawTextW(dc, t, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            continue;
        }
        if (i == g_fl_hover) fill(dc, &r, C_HOVER);
        pdf_glyph(dc, G[i], (r.left + r.right) / 2, (r.top + r.bottom) / 2, dpx(16), C_TEXT, C_ACCENT);
    }
    {
        RECT s = { g_fl_rc[FL_NEXT].right + dpx(4), dpx(9), g_fl_rc[FL_NEXT].right + dpx(5), rc.bottom - dpx(9) };
        fill(dc, &s, C_LINE);
    }
    BitBlt(out, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(buf);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK float_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    int i, h = -1;
    for (i = 0; i < FL_N; i++) if (i != FL_PAGE && PtInRect(&g_fl_rc[i], pt)) h = i;
    switch (msg) {
    case WM_PAINT: float_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE:
        if (h != g_fl_hover) { g_fl_hover = h; InvalidateRect(hwnd, NULL, FALSE); }
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    case WM_MOUSELEAVE: g_fl_hover = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        switch (h) {
        case FL_PREV: app_command(CMD_PREVPAGE); break;
        case FL_NEXT:
#ifndef SG_MUTANT_FLOATNAV
            app_command(CMD_NEXTPAGE);
#endif
            break;
        case FL_ZOOMOUT: app_command(CMD_ZOOMOUT); break;
        case FL_ZOOMIN: app_command(CMD_ZOOMIN); break;
        case FL_FIT: app_command(g.fit == FIT_WIDTH ? CMD_FITPAGE : CMD_FITWIDTH); break;
        }
        SetFocus(g_view);
        return 0;
    case WM_SETCURSOR: SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW)); return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the whole --------------------------------------------------------------------------------------------- */

int tabbar_height(void) { return TABBAR_H; }
int rail_width(void) { return RAIL_W; }

void frame_update(void)
{
    if (g_tabs) InvalidateRect(g_tabs, NULL, FALSE);
    if (g_rail) InvalidateRect(g_rail, NULL, FALSE);
    if (g_float) InvalidateRect(g_float, NULL, FALSE);
    if (g_home && IsWindowVisible(g_home)) InvalidateRect(g_home, NULL, FALSE);
}

void frame_create(HWND parent)
{
    WNDCLASSW wc = { 0 };
    TTTOOLINFOW ti = { sizeof(ti) };
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpfnWndProc = tabs_proc;
    wc.lpszClassName = L"SgPdfTabs";
    RegisterClassW(&wc);
    wc.lpfnWndProc = home_proc;
    wc.lpszClassName = L"SgPdfHome";
    RegisterClassW(&wc);
    wc.lpfnWndProc = rail_proc;
    wc.lpszClassName = L"SgPdfRail";
    RegisterClassW(&wc);
    wc.lpfnWndProc = float_proc;
    wc.lpszClassName = L"SgPdfFloat";
    RegisterClassW(&wc);
    g_tabs = CreateWindowExW(0, L"SgPdfTabs", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, NULL, g_inst, NULL);
    g_home = CreateWindowExW(0, L"SgPdfHome", NULL, WS_CHILD | WS_VSCROLL | WS_TABSTOP, 0, 0, 10, 10, parent, NULL, g_inst, NULL);
    g_rail = CreateWindowExW(0, L"SgPdfRail", NULL, WS_CHILD, 0, 0, 10, 10, parent, NULL, g_inst, NULL);
    g_rail_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                                 parent, NULL, g_inst, NULL);
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = g_rail;
    ti.uId = 1;
    ti.lpszText = L"";
    if (g_rail_tip) SendMessageW(g_rail_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    recent_load();
}

/* the floating controls go over the pages: made after the view, and kept above it */
void float_create(HWND parent)
{
    g_float = CreateWindowExW(0, L"SgPdfFloat", NULL, WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, parent, NULL, g_inst, NULL);
}

void frame_dump(FILE *f)
{
    int i;
    RECT r;
    fprintf(f, "home %d\n", home_shown());
    fprintf(f, "menuopen %d\n", g.menu_open);
    if (g_tabs) {
        int n = min(tab_count(), 32);
        r = g_tab_rc[TB_MENU];
        MapWindowPoints(g_tabs, NULL, (POINT *)&r, 2);
        fprintf(f, "doctab menu %ld %ld 0\n", (r.left + r.right) / 2, (r.top + r.bottom) / 2);
        r = g_tab_rc[TB_HOME];
        MapWindowPoints(g_tabs, NULL, (POINT *)&r, 2);
        fprintf(f, "doctab home %ld %ld %d\n", (r.left + r.right) / 2, (r.top + r.bottom) / 2, home_shown());
        fprintf(f, "tabs %d %d\n", tab_count(), tab_active() + 1);
        for (i = 0; i < n; i++) {
            RECT c = g_close_rc[i];
            char name[MAX_PATH * 3];
            WCHAR t[MAX_PATH];
            r = g_doc_rc[i];
            MapWindowPoints(g_tabs, NULL, (POINT *)&r, 2);
            MapWindowPoints(g_tabs, NULL, (POINT *)&c, 2);
            tab_label(i, t, MAX_PATH);
            to_utf8(t, name, sizeof(name));
            if (i == tab_active()) {
                fprintf(f, "doctab doc %ld %ld %d\n", (r.left + r.right) / 2, (r.top + r.bottom) / 2, !home_shown());
                fprintf(f, "doctab docclose %ld %ld 0\n", (c.left + c.right) / 2, (c.top + c.bottom) / 2);
            }
            fprintf(f, "tabdoc %d %ld %ld %d %d %ld %ld %s\n", i + 1, (r.left + r.right) / 2, (r.top + r.bottom) / 2,
                    i == tab_active() && !home_shown(), tab_doc(i)->dirty, (c.left + c.right) / 2, (c.top + c.bottom) / 2, name);
        }
    }
    if (g_home && IsWindowVisible(g_home)) {
        for (i = 0; i < NCARDS; i++) {
            r = g_card_rc[i];
            MapWindowPoints(g_home, NULL, (POINT *)&r, 2);
            fprintf(f, "homecard %s %ld %ld\n", CARDS[i].key, (r.left + r.right) / 2, (r.top + r.bottom) / 2);
        }
        r = g_open_rc;
        MapWindowPoints(g_home, NULL, (POINT *)&r, 2);
        fprintf(f, "homeopen %ld %ld\n", (r.left + r.right) / 2, (r.top + r.bottom) / 2);
        for (i = 0; i < g_nrecent; i++) {
            char buf[MAX_PATH * 3];
            r = g_recent_rc[i];
            MapWindowPoints(g_home, NULL, (POINT *)&r, 2);
            to_utf8(g_recent[i].path, buf, sizeof(buf));
            fprintf(f, "recent %d %ld %ld %s\n", i, (r.left + r.right) / 2, (r.top + r.bottom) / 2, buf);
        }
    }
    if (g_rail && IsWindowVisible(g_rail)) {
        rail_layout();
        for (i = 0; i < NRAIL; i++) {
            r = g_rail_rc[i];
            MapWindowPoints(g_rail, NULL, (POINT *)&r, 2);
            fprintf(f, "railbtn %s %ld %ld %d\n", RAIL[i].name, (r.left + r.right) / 2, (r.top + r.bottom) / 2, rail_on(i));
        }
    }
    if (g_float && IsWindowVisible(g_float)) {
        float_layout();
        for (i = 0; i < FL_N; i++) {
            r = g_fl_rc[i];
            MapWindowPoints(g_float, NULL, (POINT *)&r, 2);
            fprintf(f, "floatbtn %s %ld %ld\n", FL_NAMES[i], (r.left + r.right) / 2, (r.top + r.bottom) / 2);
        }
    }
}

/* ---- Help > Keyboard Shortcuts ------------------------------------------------------------------------------ */

void shortcuts_help(void)
{
    MessageBoxW(g_main,
                L"Files\n"
                L"  Ctrl+O  Open      Ctrl+N  Create PDF from files      Ctrl+S  Save      Ctrl+Shift+S  Save As\n"
                L"  Ctrl+P  Print      Ctrl+D  Document properties      Ctrl+W  Close      Ctrl+Q  Exit\n"
                L"  Ctrl+Tab  Home and the document\n\n"
                L"View\n"
                L"  Ctrl+Plus / Ctrl+Minus  Zoom      Ctrl+0  Fit page      Ctrl+1  Actual size      Ctrl+2  Fit width\n"
                L"  Ctrl+Shift+Plus / Ctrl+Shift+Minus  Rotate the view      F4  Navigation pane      Shift+F4  Tools pane\n"
                L"  Ctrl+Shift+N or Ctrl+G  Go to page      Home / End  First / last page\n"
                L"  F11 or Ctrl+L  Full screen (a page per screen; click, arrows or Space turn pages, Esc ends)\n"
                L"  Ctrl+Shift+V  Read this page out loud      Ctrl+Shift+B  Read to the end      Ctrl+Shift+E  Stop reading\n\n"
                L"Edit\n"
                L"  Ctrl+Z / Ctrl+Y  Undo / Redo      Ctrl+C  Copy      Ctrl+A  Select all      Ctrl+F  Find      F3  Find next\n"
                L"  Ctrl+E  Properties (the selected field, else the tools pane)      Del  Delete the selection\n\n"
                L"Pages\n"
                L"  Ctrl+Shift+I  Insert pages      Ctrl+Shift+D  Delete pages      Ctrl+Shift+R  Rotate pages\n\n"
                L"Menu\n"
                L"  Alt or F10  Menu      Alt+F / Alt+E / Alt+V / Alt+T / Alt+H  File, Edit, View, Tools, Help\n"
                L"  Ctrl+?  These shortcuts",
                L"Keyboard Shortcuts", MB_OK | MB_ICONINFORMATION);
}
