/* sg-pdf -- SG PDF: the sidebar (the navigation pane) -- page thumbnails,
 * the bookmarks (the document's outline) in a tree, the attachments (open,
 * save, attach, delete) and the signatures (each one's state; a
 * double-click shows its details).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"

#define TABS_H dpx(40)        /* Pages, Bookmarks over the views */
#define BTABS_H (g_extra ? dpx(34) : 0)   /* Attachments, Signatures under them, when the document has
                                           * either (or one of them is open), as the familiar viewer
                                           * shows its paper clip and signature panes */
static BOOL g_extra;
#define NTABS 4
#define THUMB_W dpx(116)
#define CELL_PAD dpx(10)
#define LABEL_H dpx(22)

HWND g_side, g_tree;
static HWND g_thumbs, g_alist, g_slist, g_abtn[4], g_sbtn;
static const WCHAR *const ABTN[4] = { L"Open", L"Save...", L"Attach...", L"Delete" };
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
        if (!g.pages[i].thumb || g.pages[i].tstale || g.pages[i].trot != g.rot || abs(g.pages[i].tw - w) > 2) {
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
    int col = k % 2;
    GetClientRect(g_side, &rc);
    if (k < 2) SetRect(r, dpx(6) + col * (rc.right - dpx(12)) / 2, dpx(4), dpx(6) + (col + 1) * (rc.right - dpx(12)) / 2,
                       TABS_H - dpx(4));
    else SetRect(r, dpx(6) + col * (rc.right - dpx(12)) / 2, rc.bottom - BTABS_H + dpx(3),
                 dpx(6) + (col + 1) * (rc.right - dpx(12)) / 2, rc.bottom - dpx(3));
}

static int tab_mode(int k)
{
    static const int M[NTABS] = { SIDE_THUMBS, SIDE_OUTLINE, SIDE_ATTACH, SIDE_SIGS };
    return M[k];
}

/* ---- attachments and signatures ------------------------------------------------------------------------------ */

static void lists_fill(void)
{
    int i;
    if (g.side == SIDE_ATTACH && g_alist) {
        doc_load_attach();
        SendMessageW(g_alist, LB_RESETCONTENT, 0, 0);
        for (i = 0; i < g.nattach; i++) {
            WCHAR t[400];
            DWORD sz = g.attach[i].size;
            swprintf(t, 400, L"%ls  (%ls%lu %ls)", g.attach[i].file ? g.attach[i].file : L"?",
                     g.attach[i].key && g.attach[i].key[0] == '@' ? L"comment, " : L"",
                     sz >= 1048576 ? sz / 1048576 : sz >= 1024 ? sz / 1024 : sz, sz >= 1048576 ? L"MB" : sz >= 1024 ? L"KB" : L"bytes");
            SendMessageW(g_alist, LB_ADDSTRING, 0, (LPARAM)t);
        }
        if (!g.nattach) SendMessageW(g_alist, LB_ADDSTRING, 0, (LPARAM)L"(no attachments)");
    }
    if (g.side == SIDE_SIGS && g_slist) {
        doc_load_sigs();
        SendMessageW(g_slist, LB_RESETCONTENT, 0, 0);
        for (i = 0; i < g.nsigs; i++) {
            WCHAR t[400];
            sig_t *sg = &g.sigs[i];
            swprintf(t, 400, L"%ls %ls%ls%ls", sg->state == SIG_VALID ? L"\x2714" : sg->state == SIG_UNKNOWN ? L"\x26A0" :
                                             sg->state == SIG_INVALID ? L"\x2716" : L"\x25AD",
                     sg->state == SIG_UNSIGNED ? L"Unsigned field: " : L"", sg->state == SIG_UNSIGNED ? (sg->name ? sg->name : L"")
                                                                       : (sg->signer ? sg->signer : L""),
                     sg->state == SIG_VALID ? L" (valid)" : sg->state == SIG_UNKNOWN ? L" (valid, identity unknown)" :
                     sg->state == SIG_INVALID ? L" (INVALID)" : L"");
            SendMessageW(g_slist, LB_ADDSTRING, 0, (LPARAM)t);
        }
        if (!g.nsigs) SendMessageW(g_slist, LB_ADDSTRING, 0, (LPARAM)L"(no signature fields)");
    }
}

void side_fonts(void)
{
    int i;
    if (g_tree) SendMessageW(g_tree, WM_SETFONT, (WPARAM)g_font_small, TRUE);
    if (g_alist) SendMessageW(g_alist, WM_SETFONT, (WPARAM)g_font_small, TRUE);
    if (g_slist) SendMessageW(g_slist, WM_SETFONT, (WPARAM)g_font_small, TRUE);
    for (i = 0; i < 4; i++) if (g_abtn[i]) SendMessageW(g_abtn[i], WM_SETFONT, (WPARAM)g_font_small, TRUE);
    if (g_sbtn) SendMessageW(g_sbtn, WM_SETFONT, (WPARAM)g_font_small, TRUE);
    if (g_tree) SendMessageW(g_tree, TVM_SETITEMHEIGHT, dpx(26), 0);
    if (g_side) { SendMessageW(g_side, WM_SIZE, 0, 0); InvalidateRect(g_side, NULL, TRUE); }
}

void side_dump(FILE *f)
{
    int i;
    char buf[512];
    POINT pt;
    for (i = 2; i < (g_extra ? NTABS : 2); i++)
        if (side_tab_center(i, &pt)) fprintf(f, "tab %s %ld %ld\n", i == 2 ? "attachments" : "signatures", pt.x, pt.y);
    if (g.side == SIDE_ATTACH && g_alist && IsWindowVisible(g_alist)) {
        for (i = 0; i < 4; i++) {
            RECT r;
            GetWindowRect(g_abtn[i], &r);
            to_utf8(ABTN[i], buf, sizeof(buf));
            fprintf(f, "attachbtn %s %ld %ld\n", buf, (r.left + r.right) / 2, (r.top + r.bottom) / 2);
        }
        for (i = 0; i < g.nattach; i++) {
            RECT r;
            if (SendMessageW(g_alist, LB_GETITEMRECT, i, (LPARAM)&r) == LB_ERR) continue;
            MapWindowPoints(g_alist, NULL, (POINT *)&r, 2);
            fprintf(f, "attachitem %d %ld %ld\n", i, (r.left + r.right) / 2, (r.top + r.bottom) / 2);
        }
    }
    if (g.side == SIDE_SIGS && g_slist && IsWindowVisible(g_slist)) {
        for (i = 0; i < g.nsigs; i++) {
            RECT r;
            WCHAR t[400];
            if (SendMessageW(g_slist, LB_GETITEMRECT, i, (LPARAM)&r) == LB_ERR) continue;
            SendMessageW(g_slist, LB_GETTEXT, i, (LPARAM)t);
            to_utf8(t, buf, sizeof(buf));
            MapWindowPoints(g_slist, NULL, (POINT *)&r, 2);
            fprintf(f, "sigitem %d %ld %ld %s\n", i, (r.left + r.right) / 2, (r.top + r.bottom) / 2, buf);
        }
    }
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

static BOOL extra_tabs(void)
{
    if (g.side == SIDE_ATTACH || g.side == SIDE_SIGS) return TRUE;
    if (!g.npages || !g.bridged) return FALSE;
    if (doc_load_attach() && g.nattach) return TRUE;
    return g.form && doc_load_sigs() && g.nsigs > 0;
}

static void side_layout(void)
{
    RECT rc;
    int i, bh = dpx(26), bw;
    g_extra = extra_tabs();
    GetClientRect(g_side, &rc);
    rc.bottom = max(TABS_H, rc.bottom - BTABS_H);     /* the views end over the lower tabs */
    MoveWindow(g_thumbs, 0, TABS_H, rc.right, rc.bottom - TABS_H, TRUE);
    MoveWindow(g_tree, 0, TABS_H, rc.right, rc.bottom - TABS_H, TRUE);
    ShowWindow(g_thumbs, g.side == SIDE_THUMBS ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_tree, g.side == SIDE_OUTLINE ? SW_SHOWNA : SW_HIDE);
    bw = (rc.right - dpx(10)) / 2;
    for (i = 0; i < 4; i++) {
        MoveWindow(g_abtn[i], dpx(4) + (i % 2) * (bw + dpx(2)), TABS_H + dpx(4) + (i / 2) * (bh + dpx(2)), bw, bh, TRUE);
        ShowWindow(g_abtn[i], g.side == SIDE_ATTACH ? SW_SHOWNA : SW_HIDE);
    }
    MoveWindow(g_alist, 0, TABS_H + 2 * bh + dpx(10), rc.right, max(0, rc.bottom - TABS_H - 2 * bh - dpx(10)), TRUE);
    ShowWindow(g_alist, g.side == SIDE_ATTACH ? SW_SHOWNA : SW_HIDE);
    MoveWindow(g_sbtn, dpx(4), TABS_H + dpx(4), rc.right - dpx(8), bh, TRUE);
    ShowWindow(g_sbtn, g.side == SIDE_SIGS ? SW_SHOWNA : SW_HIDE);
    MoveWindow(g_slist, 0, TABS_H + bh + dpx(8), rc.right, max(0, rc.bottom - TABS_H - bh - dpx(8)), TRUE);
    ShowWindow(g_slist, g.side == SIDE_SIGS ? SW_SHOWNA : SW_HIDE);
    lists_fill();
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
    if (g_side && g.side != SIDE_NONE && extra_tabs() != g_extra) { side_layout(); InvalidateRect(g_side, NULL, TRUE); }
    if (g_thumbs && g.side == SIDE_THUMBS) { thumbs_scrollbar(); InvalidateRect(g_thumbs, NULL, FALSE); }
    if (g.side == SIDE_ATTACH || g.side == SIDE_SIGS) lists_fill();
}

static LRESULT CALLBACK side_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static const WCHAR *const NAMES[NTABS] = { L"Pages", L"Bookmarks", L"Attachments", L"Signatures" };
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
        if (g_extra) {
            r = rc;
            r.top = rc.bottom - BTABS_H;
            FillRect(dc, &r, bg);
            r.bottom = r.top + 1;
            FillRect(dc, &r, line);
        }
        SetRect(&r, rc.right - 1, 0, rc.right, rc.bottom);
        FillRect(dc, &r, line);
        SetBkMode(dc, TRANSPARENT);
        for (k = 0; k < (g_extra ? NTABS : 2); k++) {
            BOOL on = g.side == tab_mode(k);
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
        for (k = 0; k < (g_extra ? NTABS : 2); k++) { tab_rect(k, &r); if (PtInRect(&r, pt)) h = k; }
        if (h != g_hover_tab) { g_hover_tab = h; InvalidateRect(hwnd, NULL, FALSE); }
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: g_hover_tab = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r;
        int k;
        for (k = 0; k < (g_extra ? NTABS : 2); k++) { tab_rect(k, &r); if (PtInRect(&r, pt)) side_set_mode(tab_mode(k)); }
        return 0;
    }
    case WM_COMMAND: {
        HWND c = (HWND)lp;
        int i, sel;
        if (c == g_alist && HIWORD(wp) == LBN_DBLCLK) attach_open((int)SendMessageW(g_alist, LB_GETCURSEL, 0, 0));
        if (c == g_slist && HIWORD(wp) == LBN_DBLCLK) {
            sel = (int)SendMessageW(g_slist, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g.nsigs) { view_goto_page(g.sigs[sel].page, max(0.0f, g.sigs[sel].box.y1 - 40)); sig_show(sel); }
        }
        if (c == g_slist && HIWORD(wp) == LBN_SELCHANGE) {
            sel = (int)SendMessageW(g_slist, LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g.nsigs) view_goto_page(g.sigs[sel].page, max(0.0f, g.sigs[sel].box.y1 - 40));
        }
        if (c == g_sbtn && HIWORD(wp) == BN_CLICKED) app_command(CMD_VALIDATE);
        for (i = 0; i < 4; i++) if (c == g_abtn[i] && HIWORD(wp) == BN_CLICKED) {
            sel = (int)SendMessageW(g_alist, LB_GETCURSEL, 0, 0);
            if (i == 2) app_command(CMD_ADDATTACH);
            else if (sel < 0 || sel >= g.nattach) app_set_status(L"Choose an attachment first.");
            else if (i == 0) attach_open(sel);
            else if (i == 1) attach_save(sel);
            else attach_delete(sel);
            SetFocus(g_alist);
        }
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
    case WM_CTLCOLORLISTBOX: case WM_CTLCOLORBTN: case WM_CTLCOLORSTATIC: {
        static HBRUSH b;
        static COLORREF bc = (COLORREF)-1;
        if (bc != C_SIDE) { if (b) DeleteObject(b); b = CreateSolidBrush(C_SIDE); bc = C_SIDE; }
        SetBkColor((HDC)wp, C_SIDE);
        SetTextColor((HDC)wp, C_TEXT);
        return (LRESULT)b;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* the app mode changed: the tree's colours (the rest is drawn from the palette) */
void side_apply_mode(void)
{
    if (!g_tree) return;
    SendMessageW(g_tree, TVM_SETBKCOLOR, 0, C_SIDE);
    SendMessageW(g_tree, TVM_SETTEXTCOLOR, 0, C_TEXT);
    InvalidateRect(g_tree, NULL, TRUE);
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
    SendMessageW(g_tree, TVM_SETTEXTCOLOR, 0, C_TEXT);
    SendMessageW(g_tree, TVM_SETITEMHEIGHT, dpx(26), 0);
    {
        int i;
        g_alist = CreateWindowExW(0, L"LISTBOX", NULL, WS_CHILD | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                  0, 0, 10, 10, g_side, NULL, g_inst, NULL);
        g_slist = CreateWindowExW(0, L"LISTBOX", NULL, WS_CHILD | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                  0, 0, 10, 10, g_side, NULL, g_inst, NULL);
        for (i = 0; i < 4; i++)
            g_abtn[i] = CreateWindowExW(0, L"BUTTON", ABTN[i], WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, g_side, NULL, g_inst, NULL);
        g_sbtn = CreateWindowExW(0, L"BUTTON", L"Validate All", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, g_side, NULL, g_inst, NULL);
        side_fonts();
    }
    return g_side;
}
