/* sg-pdf -- PDF Viewer: the sidebar -- page thumbnails, and the bookmarks
 * (the document's outline) in a tree.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"

#define TABS_H dpx(40)
#define THUMB_W dpx(116)
#define CELL_PAD dpx(10)
#define LABEL_H dpx(22)

HWND g_side, g_tree;
static HWND g_thumbs;
static int g_tscroll;           /* the thumbnails' scroll position */
static int g_hover_tab = -1;

static void thumb_size(int i, int *w, int *h)
{
    page_t *p = &g.pages[i];
    double pw = p->w, ph = p->h;
    if (g.rot == 90 || g.rot == 270) { double t = pw; pw = ph; ph = t; }
    *w = THUMB_W;
    *h = (int)ceil(ph * THUMB_W / pw);
    if (*h > THUMB_W * 2) { *h = THUMB_W * 2; *w = (int)ceil(pw * *h / ph); }
}

static int cell_y(int i)
{
    int k, y = CELL_PAD;
    for (k = 0; k < i; k++) { int w, h; thumb_size(k, &w, &h); y += h + LABEL_H + CELL_PAD; }
    return y;
}

static int total_h(void)
{
    return g.npages ? cell_y(g.npages) : 0;
}

static void thumbs_scrollbar(void)
{
    RECT rc;
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    GetClientRect(g_thumbs, &rc);
    g_tscroll = max(0, min(g_tscroll, total_h() - rc.bottom));
    si.nMax = max(total_h() - 1, 0);
    si.nPage = rc.bottom;
    si.nPos = g_tscroll;
    SetScrollInfo(g_thumbs, SB_VERT, &si, TRUE);
}

void side_ensure_visible(int page)
{
    RECT rc;
    int y, w, h;
    if (!g_thumbs || g.side != SIDE_THUMBS || page < 0 || page >= g.npages) return;
    GetClientRect(g_thumbs, &rc);
    y = cell_y(page);
    thumb_size(page, &w, &h);
    if (y - CELL_PAD < g_tscroll) g_tscroll = y - CELL_PAD;
    else if (y + h + LABEL_H > g_tscroll + rc.bottom) g_tscroll = y + h + LABEL_H + CELL_PAD - rc.bottom;
    thumbs_scrollbar();
    InvalidateRect(g_thumbs, NULL, FALSE);
}

/* the screen rectangle of a thumbnail, for the dump; FALSE if not shown */
BOOL side_thumb_rect(int i, RECT *out)
{
    RECT rc;
    int w, h, x, y;
    if (!g_thumbs || g.side != SIDE_THUMBS || !IsWindowVisible(g_thumbs)) return FALSE;
    GetClientRect(g_thumbs, &rc);
    thumb_size(i, &w, &h);
    x = (rc.right - w) / 2;
    y = cell_y(i) - g_tscroll;
    if (y + h < 0 || y > rc.bottom) return FALSE;
    SetRect(out, x, y, x + w, y + h);
    MapWindowPoints(g_thumbs, NULL, (POINT *)out, 2);
    return TRUE;
}

static void thumbs_paint(HWND hwnd, HDC out)
{
    RECT rc;
    HDC dc, mem;
    HBITMAP buf, ob;
    HBRUSH bg = CreateSolidBrush(C_SIDE), line = CreateSolidBrush(C_LINE), acc = CreateSolidBrush(C_ACCENT);
    HFONT of;
    int i;
    GetClientRect(hwnd, &rc);
    dc = CreateCompatibleDC(out);
    buf = CreateCompatibleBitmap(out, max(rc.right, 1), max(rc.bottom, 1));
    ob = SelectObject(dc, buf);
    mem = CreateCompatibleDC(out);
    FillRect(dc, &rc, bg);
    of = SelectObject(dc, g_font_small);
    SetBkMode(dc, TRANSPARENT);
    render_clear_wants(TRUE);
    for (i = 0; i < g.npages; i++) {
        int w, h, x, y = cell_y(i) - g_tscroll;
        RECT tr, fr, lr;
        WCHAR num[16];
        thumb_size(i, &w, &h);
        if (y > rc.bottom) break;
        if (y + h + LABEL_H < 0) continue;
        x = (rc.right - w) / 2;
        SetRect(&tr, x, y, x + w, y + h);
        fr = tr;
        if (i == g.current) { InflateRect(&fr, dpx(3), dpx(3)); FillRect(dc, &fr, acc); }
        else { InflateRect(&fr, 1, 1); FillRect(dc, &fr, line); }
        FillRect(dc, &tr, GetStockObject(WHITE_BRUSH));
        if (g.pages[i].thumb) {
            HGDIOBJ o = SelectObject(mem, g.pages[i].thumb);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
            if (g.pages[i].trot == g.rot)
                StretchBlt(dc, x, y, w, h, mem, 0, 0, g.pages[i].tw, g.pages[i].th, SRCCOPY);
            SelectObject(mem, o);
        }
        if (!g.pages[i].thumb || g.pages[i].trot != g.rot || abs(g.pages[i].tw - w) > 2) {
            double pw = (g.rot == 90 || g.rot == 270) ? g.pages[i].h : g.pages[i].w;
            render_want(i, (double)w / pw, g.rot, TRUE);
        }
        swprintf(num, 16, L"%d", i + 1);
        SetRect(&lr, 0, y + h + dpx(4), rc.right, y + h + LABEL_H);
        SetTextColor(dc, i == g.current ? C_ACCENT : C_SUBTEXT);
        DrawTextW(dc, num, -1, &lr, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
    BitBlt(out, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, of);
    SelectObject(dc, ob);
    DeleteObject(buf);
    DeleteDC(dc);
    DeleteDC(mem);
    DeleteObject(bg);
    DeleteObject(line);
    DeleteObject(acc);
}

static LRESULT CALLBACK thumbs_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        thumbs_paint(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: thumbs_scrollbar(); return 0;
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        RECT rc;
        GetClientRect(hwnd, &rc);
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (LOWORD(wp)) {
        case SB_LINEUP: g_tscroll -= dpx(40); break;
        case SB_LINEDOWN: g_tscroll += dpx(40); break;
        case SB_PAGEUP: g_tscroll -= rc.bottom; break;
        case SB_PAGEDOWN: g_tscroll += rc.bottom; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: g_tscroll = si.nTrackPos; break;
        }
        thumbs_scrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        g_tscroll -= GET_WHEEL_DELTA_WPARAM(wp) * dpx(120) / WHEEL_DELTA;
        thumbs_scrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        int y = GET_Y_LPARAM(lp) + g_tscroll, i;
        for (i = 0; i < g.npages; i++) {
            int w, h, top = cell_y(i);
            thumb_size(i, &w, &h);
            if (y >= top - CELL_PAD / 2 && y < top + h + LABEL_H + CELL_PAD / 2) { view_goto_page(i, 0); break; }
        }
        SetFocus(g_view);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the bookmarks ------------------------------------------------------------------------------- */

void side_load_outline(void)
{
    char head[256], num[32], *s, *e;
    BYTE *data;
    DWORD len;
    int n, i;
    HTREEITEM parents[34] = { TVI_ROOT };
    for (i = 0; i < g.noutline; i++) free(g.outline[i].title);
    free(g.outline);
    g.outline = NULL;
    g.noutline = 0;
    if (g_tree) TreeView_DeleteAllItems(g_tree);
    if (br_request("outline", head, sizeof(head), &data, &len) != 1) return;
    n = br_field(head, "n", num, sizeof(num)) ? atoi(num) : 0;
    g.outline = n > 0 ? calloc(n, sizeof(outline_t)) : NULL;
    for (s = (char *)data; g.outline && s && *s && g.noutline < n; s = e) {
        outline_t *o = &g.outline[g.noutline];
        char *t;
        int open = 0;
        e = strchr(s, '\n');
        if (e) *e++ = 0;
        if (sscanf(s, "%d\t%d\t%f\t%d", &o->depth, &o->page, &o->top, &open) != 4) continue;
        if (!(t = strchr(s, '\t')) || !(t = strchr(t + 1, '\t')) || !(t = strchr(t + 1, '\t')) || !(t = strchr(t + 1, '\t'))) continue;
        o->title = from_utf8(t + 1, -1);
        o->depth = max(0, min(o->depth, 32));
        if (g_tree) {
            TVINSERTSTRUCTW is = { 0 };
            HTREEITEM it;
            int d = o->depth;
            while (d > 0 && !parents[d]) d--;
            is.hParent = parents[d];
            is.hInsertAfter = TVI_LAST;
            is.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_STATE;
            is.item.stateMask = TVIS_EXPANDED;
            is.item.state = open ? TVIS_EXPANDED : 0;
            is.item.pszText = o->title ? o->title : L"";
            is.item.lParam = g.noutline;
            it = (HTREEITEM)SendMessageW(g_tree, TVM_INSERTITEMW, 0, (LPARAM)&is);
            parents[o->depth + 1] = it;
            if (o->depth + 2 < 34) parents[o->depth + 2] = NULL;
        }
        g.noutline++;
    }
    free(data);
}

/* ---- the frame: tabs over the two views ---------------------------------------------------------- */

static void tab_rect(int k, RECT *r)
{
    RECT rc;
    GetClientRect(g_side, &rc);
    SetRect(r, dpx(6) + k * (rc.right - dpx(12)) / 2, dpx(4), dpx(6) + (k + 1) * (rc.right - dpx(12)) / 2, TABS_H - dpx(4));
}

BOOL side_tab_center(int k, POINT *pt)
{
    RECT r;
    if (g.side == SIDE_NONE) return FALSE;
    tab_rect(k, &r);
    pt->x = (r.left + r.right) / 2;
    pt->y = (r.top + r.bottom) / 2;
    ClientToScreen(g_side, pt);
    return TRUE;
}

static void side_layout(void)
{
    RECT rc;
    GetClientRect(g_side, &rc);
    MoveWindow(g_thumbs, 0, TABS_H, rc.right, rc.bottom - TABS_H, TRUE);
    MoveWindow(g_tree, 0, TABS_H, rc.right, rc.bottom - TABS_H, TRUE);
    ShowWindow(g_thumbs, g.side == SIDE_THUMBS ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_tree, g.side == SIDE_OUTLINE ? SW_SHOWNA : SW_HIDE);
}

void side_set_mode(int mode)
{
    g.side = mode;
    side_layout();
    app_layout();
    if (mode == SIDE_THUMBS) side_ensure_visible(g.current);
    InvalidateRect(g_side, NULL, TRUE);
    app_status_changed();
}

void side_update(void)
{
    if (g_thumbs && g.side == SIDE_THUMBS) { thumbs_scrollbar(); InvalidateRect(g_thumbs, NULL, FALSE); }
}

static LRESULT CALLBACK side_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static const WCHAR *const NAMES[2] = { L"Thumbnails", L"Bookmarks" };
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, r;
        HBRUSH bg = CreateSolidBrush(C_SIDE), hov = CreateSolidBrush(C_HOVER), acc = CreateSolidBrush(C_ACCENT),
               line = CreateSolidBrush(C_LINE);
        HFONT of = SelectObject(dc, g_font_small);
        int k;
        GetClientRect(hwnd, &rc);
        r = rc;
        r.bottom = TABS_H;
        FillRect(dc, &r, bg);
        SetRect(&r, rc.right - 1, 0, rc.right, rc.bottom);
        FillRect(dc, &r, line);
        SetBkMode(dc, TRANSPARENT);
        for (k = 0; k < 2; k++) {
            BOOL on = g.side == (k ? SIDE_OUTLINE : SIDE_THUMBS);
            tab_rect(k, &r);
            if (k == g_hover_tab && !on) FillRect(dc, &r, hov);
            SetTextColor(dc, on ? C_ACCENT : C_TEXT);
            SelectObject(dc, on ? g_font_bold : g_font_small);
            DrawTextW(dc, NAMES[k], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (on) { RECT u = r; u.top = u.bottom - dpx(2); InflateRect(&u, -dpx(16), 0); FillRect(dc, &u, acc); }
        }
        SelectObject(dc, of);
        DeleteObject(bg); DeleteObject(hov); DeleteObject(acc); DeleteObject(line);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE: side_layout(); return 0;
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int k, h = -1;
        RECT r;
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        for (k = 0; k < 2; k++) { tab_rect(k, &r); if (PtInRect(&r, pt)) h = k; }
        if (h != g_hover_tab) { g_hover_tab = h; InvalidateRect(hwnd, NULL, FALSE); }
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: g_hover_tab = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r;
        int k;
        for (k = 0; k < 2; k++) { tab_rect(k, &r); if (PtInRect(&r, pt)) side_set_mode(k ? SIDE_OUTLINE : SIDE_THUMBS); }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->hwndFrom == g_tree && nm->code == TVN_SELCHANGEDW) {
            NMTREEVIEWW *tv = (NMTREEVIEWW *)lp;
            int k = (int)tv->itemNew.lParam;
            if (k >= 0 && k < g.noutline && g.outline[k].page >= 0) view_goto_page(g.outline[k].page, g.outline[k].top);
        }
        if (nm->hwndFrom == g_tree && nm->code == NM_CLICK) {
            /* a click on the item already selected goes there again */
            TVHITTESTINFO ht = { 0 };
            GetCursorPos(&ht.pt);
            ScreenToClient(g_tree, &ht.pt);
            if (TreeView_HitTest(g_tree, &ht) && ht.hItem == TreeView_GetSelection(g_tree)) {
                TVITEMW it = { TVIF_PARAM, ht.hItem };
                SendMessageW(g_tree, TVM_GETITEMW, 0, (LPARAM)&it);
                if ((int)it.lParam >= 0 && (int)it.lParam < g.noutline && g.outline[it.lParam].page >= 0)
                    view_goto_page(g.outline[it.lParam].page, g.outline[it.lParam].top);
            }
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void side_register(void)
{
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = side_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"SgPdfSide";
    RegisterClassW(&wc);
    wc.lpfnWndProc = thumbs_proc;
    wc.lpszClassName = L"SgPdfThumbs";
    RegisterClassW(&wc);
}

HWND side_create(HWND parent)
{
    g_side = CreateWindowExW(0, L"SgPdfSide", NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, parent, NULL, g_inst, NULL);
    g_thumbs = CreateWindowExW(0, L"SgPdfThumbs", NULL, WS_CHILD | WS_VSCROLL, 0, 0, 10, 10, g_side, NULL, g_inst, NULL);
    g_tree = CreateWindowExW(0, WC_TREEVIEWW, NULL,
                             WS_CHILD | WS_TABSTOP | TVS_HASBUTTONS | TVS_LINESATROOT | TVS_SHOWSELALWAYS |
                             TVS_FULLROWSELECT | TVS_TRACKSELECT,
                             0, 0, 10, 10, g_side, NULL, g_inst, NULL);
    SendMessageW(g_tree, WM_SETFONT, (WPARAM)g_font_small, FALSE);
    SendMessageW(g_tree, TVM_SETBKCOLOR, 0, C_SIDE);
    SendMessageW(g_tree, TVM_SETITEMHEIGHT, dpx(26), 0);
    return g_side;
}
