/* sg-control -- the Stained Glass OS Control Panel: the window, navigation,
 * page machinery and control.exe's command line.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"
#include <shellapi.h>
#include <windowsx.h>
#include <stdarg.h>

HWND g_main, g_page;
HINSTANCE g_inst;
int g_dpi = 96;
HFONT g_font_title, g_font_head, g_font_body, g_font_small, g_font_cat, g_font_big;

static HWND g_back, g_fwd, g_up, g_addr, g_search, g_refresh;
static enum page_id g_cur = PG_HOME;
static enum page_id g_hist[64];
static int g_hist_n, g_hist_pos = -1;
static int g_pane_w;            /* the left pane's width on this page, 0: none */
static int g_scroll, g_content_h;
static BOOL g_refresh_on_activate;
BOOL g_kbd_cues;                /* show focus rectangles: the keyboard has been used */
WCHAR g_search_text[128];

#define NAVBAR_H 48
#define ID_BACK    10
#define ID_FWD     11
#define ID_UP      12
#define ID_SEARCH  13
#define ID_REFRESH 14

const struct page_def g_pages[PG_COUNT] = {
    [PG_HOME]         = { L"Control Panel",              PG_COUNT,      build_home,         cmd_home },
    [PG_ALL]          = { L"All Control Panel Items",    PG_HOME,       build_all,          cmd_home },
    [PG_CAT_SYSSEC]   = { L"System and Security",        PG_HOME,       build_category,     cmd_home },
    [PG_CAT_NET]      = { L"Network and Internet",       PG_HOME,       build_category,     cmd_home },
    [PG_CAT_HW]       = { L"Hardware and Sound",         PG_HOME,       build_category,     cmd_home },
    [PG_CAT_PROG]     = { L"Programs",                   PG_HOME,       build_category,     cmd_home },
    [PG_CAT_USERS]    = { L"User Accounts",              PG_HOME,       build_category,     cmd_home },
    [PG_CAT_APPEAR]   = { L"Appearance and Personalization", PG_HOME,   build_category,     cmd_home },
    [PG_CAT_CLOCK]    = { L"Clock and Region",           PG_HOME,       build_category,     cmd_home },
    [PG_SYSTEM]       = { L"System",                     PG_CAT_SYSSEC, build_system,       cmd_system },
    [PG_PROGRAMS]     = { L"Programs and Features",      PG_CAT_PROG,   build_programs,     cmd_programs, notify_programs },
    [PG_USERS]        = { L"User Accounts",              PG_CAT_USERS,  build_users,        cmd_users },
    [PG_USERS_MANAGE] = { L"Manage Accounts",            PG_USERS,      build_users_manage, cmd_users_manage, notify_users },
    [PG_DATETIME]     = { L"Date and Time",              PG_CAT_CLOCK,  build_datetime,     cmd_datetime, NULL, timer_datetime },
    [PG_PERSONALIZE]  = { L"Personalization",            PG_CAT_APPEAR, build_personalize,  cmd_personalize, notify_personalize },
    [PG_UPDATE]       = { L"Windows Update",             PG_CAT_SYSSEC, build_update,       cmd_update },
    [PG_NETWORK]      = { L"Network and Sharing Center", PG_CAT_NET,    build_network,      cmd_network },
    [PG_SPEECH]       = { L"Speech Recognition",         PG_CAT_HW,     build_speech,       cmd_speech, NULL, timer_speech },
    [PG_ADMINTOOLS]   = { L"Administrative Tools",       PG_CAT_SYSSEC, build_admintools,   cmd_admintools },
};

int S(int dip) { return MulDiv(dip, g_dpi, 96); }
enum page_id current_page(void) { return g_cur; }

/* ---- painted items -------------------------------------------------------------- */
enum item_kind { IT_TEXT, IT_ICON, IT_FILL, IT_SWATCH };
struct item {
    enum item_kind kind;
    RECT r;
    HFONT font;
    COLORREF color;
    UINT fmt;
    int icon, selected;
    WCHAR *text;
};
static struct item *g_items;
static int g_nitems, g_items_cap;

static struct item *new_item(enum item_kind k, int x, int y, int w, int h)
{
    struct item *it;
    if (g_nitems == g_items_cap) {
        int cap = g_items_cap ? g_items_cap * 2 : 64;
        struct item *n = realloc(g_items, cap * sizeof(*n));
        if (!n) return NULL;
        g_items = n; g_items_cap = cap;
    }
    it = &g_items[g_nitems++];
    memset(it, 0, sizeof(*it));
    it->kind = k;
    SetRect(&it->r, x, y, x + w, y + h);
    if (y + h > g_content_h) g_content_h = y + h;
    return it;
}

static void clear_items(void)
{
    int i;
    for (i = 0; i < g_nitems; i++) free(g_items[i].text);
    g_nitems = 0;
}

void pg_text(int x, int y, int w, int h, HFONT f, COLORREF c, const WCHAR *s, UINT fmt)
{
    struct item *it = new_item(IT_TEXT, x, y, w, h);
    if (!it) return;
    it->font = f; it->color = c; it->fmt = fmt | DT_NOPREFIX;
    it->text = _wcsdup(s ? s : L"");
}

int pg_para(int x, int y, int w, HFONT f, COLORREF c, const WCHAR *s)
{
    HDC dc = GetDC(g_page);
    RECT r = { 0, 0, w, 0 };
    HGDIOBJ old = SelectObject(dc, f);
    DrawTextW(dc, s, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(g_page, dc);
    pg_text(x, y, w, r.bottom + 2, f, c, s, DT_WORDBREAK);
    return r.bottom + 2;
}

void pg_textf(int x, int y, int w, HFONT f, COLORREF c, const WCHAR *fmt, ...)
{
    WCHAR buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, ARRAYSIZE(buf) - 1, fmt, ap);
    buf[ARRAYSIZE(buf) - 1] = 0;
    va_end(ap);
    pg_text(x, y, w, S(20), f, c, buf, DT_SINGLELINE | DT_END_ELLIPSIS);
}

void pg_icon(int x, int y, int size, int icon)
{
    struct item *it = new_item(IT_ICON, x, y, size, size);
    if (it) it->icon = icon;
}

void pg_fill(int x, int y, int w, int h, COLORREF c)
{
    struct item *it = new_item(IT_FILL, x, y, w, h);
    if (it) it->color = c;
}

void pg_rule(int x, int y, int w) { pg_fill(x, y, w, 1, COL_RULE); }

void pg_swatch(int x, int y, int w, int h, COLORREF c, BOOL selected)
{
    struct item *it = new_item(IT_SWATCH, x, y, w, h);
    if (it) { it->color = c; it->selected = selected; }
}

void pg_title(int x, int y, const WCHAR *s)
{
    pg_text(x, y, pg_width() - x - S(24), S(28), g_font_title, COL_TITLE, s, DT_SINGLELINE | DT_END_ELLIPSIS);
}

int pg_width(void) { RECT r; GetClientRect(g_page, &r); return r.right; }
int pg_height(void) { RECT r; GetClientRect(g_page, &r); return r.bottom; }

HWND pg_control(const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y - g_scroll, w, h,
                             g_page, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font_body, TRUE);
    if (y + h > g_content_h) g_content_h = y + h;
    return c;
}

HWND pg_button(const WCHAR *text, int x, int y, int w, int id)
{
    return pg_control(L"BUTTON", text, WS_TABSTOP | BS_PUSHBUTTON, x, y, w, S(28), id);
}

void pg_timer(UINT ms) { SetTimer(g_page, 1, ms, NULL); }

int pg_left_pane(const WCHAR *const *labels, const int *ids, int n)
{
    int w = S(230), y = S(20), i;
    g_pane_w = w;
    pg_fill(0, 0, w, 32000, COL_PANE);
    pg_fill(w - 1, 0, 1, 32000, COL_PANE_EDGE);
    pg_link(S(20), y, L"Control Panel Home", NAV(PG_HOME), LINK_BOLD);
    y += S(34);
    for (i = 0; i < n; i++) {
        if (!labels[i]) { y += S(16); continue; }                  /* a gap */
        if (ids[i] < 0) {                                         /* a heading */
            pg_text(S(20), y, w - S(30), S(20), g_font_small, COL_SUBTLE, labels[i], DT_SINGLELINE);
            y += S(22);
            continue;
        }
        pg_link(S(20), y, labels[i], ids[i], IS_SHIELD(ids[i]) ? LINK_SHIELD : 0);
        y += S(26);
    }
    /* the pane's filler items must not stretch the scrollable height */
    g_content_h = y;
    return w;
}

/* ---- links: child windows, so they take focus, Tab and Enter ---------------- */
struct link { int flags; BOOL hot; COLORREF bg; };

static SIZE link_size(HWND hwnd, const WCHAR *s, int flags)
{
    HDC dc = GetDC(hwnd);
    SIZE sz = { 0, 0 };
    SelectObject(dc, (flags & LINK_CATEGORY) ? g_font_cat : (flags & LINK_BOLD) ? g_font_head : g_font_body);
    GetTextExtentPoint32W(dc, s, lstrlenW(s), &sz);
    ReleaseDC(hwnd, dc);
    if (flags & LINK_SHIELD) sz.cx += S(20);
    sz.cx += S(4); sz.cy += S(4);
    return sz;
}

static LRESULT CALLBACK link_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct link *l = (struct link *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE: {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        l = calloc(1, sizeof(*l));
        if (!l) return FALSE;
        l->flags = (int)(INT_PTR)cs->lpCreateParams;
        l->bg = cs->x < g_pane_w ? COL_PANE : COL_BG;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)l);
        break;
    }
    case WM_NCDESTROY: free(l); SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); break;
    case WM_SETCURSOR: SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND)); return TRUE;
    case WM_MOUSEMOVE:
        if (l && !l->hot) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
            l->hot = TRUE; TrackMouseEvent(&t); InvalidateRect(hwnd, NULL, TRUE);
        }
        return 0;
    case WM_MOUSELEAVE: if (l) { l->hot = FALSE; InvalidateRect(hwnd, NULL, TRUE); } return 0;
    case WM_LBUTTONDOWN: SetFocus(hwnd); return 0;
    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT r; GetClientRect(hwnd, &r);
        if (PtInRect(&r, pt))
            PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), (LPARAM)hwnd);
        return 0;
    }
    case WM_GETDLGCODE:
        if (lp && ((MSG *)lp)->message == WM_KEYDOWN && ((MSG *)lp)->wParam == VK_RETURN) return DLGC_WANTMESSAGE;
        return DLGC_WANTCHARS;
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), (LPARAM)hwnd);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == ' ') {
            PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), (LPARAM)hwnd);
            return 0;
        }
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: InvalidateRect(hwnd, NULL, TRUE); break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        WCHAR s[256];
        RECT r;
        HBRUSH bg;
        int x = S(2);
        BOOL enabled = IsWindowEnabled(hwnd);
        GetClientRect(hwnd, &r);
        bg = CreateSolidBrush(l ? l->bg : COL_BG);
        FillRect(dc, &r, bg);
        DeleteObject(bg);
        GetWindowTextW(hwnd, s, ARRAYSIZE(s));
        if (l && (l->flags & LINK_SHIELD)) { draw_icon(dc, IC_SHIELD, x, (r.bottom - S(16)) / 2, S(16)); x += S(20); }
        {
            HFONT f = (l && (l->flags & LINK_CATEGORY)) ? g_font_cat : (l && (l->flags & LINK_BOLD)) ? g_font_head : g_font_body;
            LOGFONTW lf;
            HFONT use = f, under = NULL;
            if (l && l->hot && enabled) {
                GetObjectW(f, sizeof(lf), &lf); lf.lfUnderline = TRUE;
                under = CreateFontIndirectW(&lf); use = under;
            }
            SelectObject(dc, use);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, !enabled ? COL_SUBTLE : (l && l->hot) ? COL_LINK_HOT :
                             (l && (l->flags & LINK_CATEGORY)) ? COL_CATLINK : COL_LINK);
            TextOutW(dc, x, S(2), s, lstrlenW(s));
            if (under) { SelectObject(dc, g_font_body); DeleteObject(under); }
        }
        if (GetFocus() == hwnd && g_kbd_cues) DrawFocusRect(dc, &r);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

HWND pg_link(int x, int y, const WCHAR *s, int id, int flags)
{
    SIZE sz = link_size(g_page, s, flags);
    HWND c = CreateWindowExW(0, L"SgCplLink", s, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                             x, y - g_scroll, sz.cx, sz.cy, g_page, (HMENU)(INT_PTR)id, g_inst,
                             (LPVOID)(INT_PTR)flags);
    if (y + sz.cy > g_content_h) g_content_h = y + sz.cy;
    return c;
}

/* ---- the page window ------------------------------------------------------------ */
static HBRUSH g_bg_brush, g_pane_brush;

static void update_scrollbar(void)
{
    SCROLLINFO si = { sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS };
    si.nMin = 0; si.nMax = g_content_h + S(24); si.nPage = pg_height(); si.nPos = g_scroll;
    SetScrollInfo(g_page, SB_VERT, &si, TRUE);
}

static void scroll_to(int y)
{
    int max = g_content_h + S(24) - pg_height();
    if (y > max) y = max;
    if (y < 0) y = 0;
    if (y == g_scroll) return;
    ScrollWindowEx(g_page, 0, g_scroll - y, NULL, NULL, NULL, NULL, SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
    g_scroll = y;
    update_scrollbar();
}

static void paint_items(HDC dc, const RECT *clip)
{
    int i;
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < g_nitems; i++) {
        struct item *it = &g_items[i];
        RECT r = it->r, tmp;
        OffsetRect(&r, 0, -g_scroll);
        if (!IntersectRect(&tmp, &r, clip)) continue;
        switch (it->kind) {
        case IT_FILL: {
            HBRUSH b = CreateSolidBrush(it->color);
            FillRect(dc, &r, b); DeleteObject(b);
            break;
        }
        case IT_SWATCH: {
            HBRUSH b = CreateSolidBrush(it->color);
            RECT in = r;
            if (it->selected) {
                HBRUSH k = CreateSolidBrush(COL_TEXT);
                FillRect(dc, &r, k); DeleteObject(k);
                InflateRect(&in, -S(3), -S(3));
                k = CreateSolidBrush(COL_BG); FillRect(dc, &in, k); DeleteObject(k);
                InflateRect(&in, -S(2), -S(2));
            }
            FillRect(dc, &in, b); DeleteObject(b);
            break;
        }
        case IT_ICON: draw_icon(dc, it->icon, r.left, r.top, r.right - r.left); break;
        case IT_TEXT:
            SelectObject(dc, it->font);
            SetTextColor(dc, it->color);
            DrawTextW(dc, it->text, -1, &r, it->fmt);
            break;
        }
    }
}

static LRESULT CALLBACK page_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c; GetClientRect(hwnd, &c);
        /* paint into a buffer: no flicker while scrolling or ticking */
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        FillRect(mem, &c, g_bg_brush);
        paint_items(mem, &ps.rcPaint);
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
               ps.rcPaint.bottom - ps.rcPaint.top, mem, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: {
        RECT r; POINT pt;
        GetWindowRect((HWND)lp, &r);
        pt.x = r.left; pt.y = r.top; ScreenToClient(hwnd, &pt);
        SetBkMode((HDC)wp, TRANSPARENT);
        SetTextColor((HDC)wp, COL_TEXT);
        return (LRESULT)(pt.x < g_pane_w ? g_pane_brush : g_bg_brush);
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (IS_NAV(id)) { navigate((enum page_id)(id - NAV_BASE)); return 0; }
        if (g_pages[g_cur].command && g_pages[g_cur].command(id, HIWORD(wp), (HWND)lp)) return 0;
        return 0;
    }
    case WM_NOTIFY:
        if (g_pages[g_cur].notify) return g_pages[g_cur].notify((NMHDR *)lp);
        return 0;
    case WM_TIMER:
        if (g_pages[g_cur].timer) g_pages[g_cur].timer();
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (LOWORD(wp)) {
        case SB_LINEUP: scroll_to(g_scroll - S(40)); break;
        case SB_LINEDOWN: scroll_to(g_scroll + S(40)); break;
        case SB_PAGEUP: scroll_to(g_scroll - (int)si.nPage); break;
        case SB_PAGEDOWN: scroll_to(g_scroll + (int)si.nPage); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: scroll_to(si.nTrackPos); break;
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        scroll_to(g_scroll - GET_WHEEL_DELTA_WPARAM(wp) * S(40) / WHEEL_DELTA);
        return 0;
    case WM_SIZE:
        update_scrollbar();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- navigation ----------------------------------------------------------------- */
static void layout(void);

static void set_title(void)
{
    SetWindowTextW(g_main, g_pages[g_cur].title);
    InvalidateRect(g_addr, NULL, TRUE);
    EnableWindow(g_back, g_hist_pos > 0);
    EnableWindow(g_fwd, g_hist_pos < g_hist_n - 1);
    EnableWindow(g_up, g_pages[g_cur].parent != PG_COUNT);
    InvalidateRect(g_back, NULL, TRUE);
    InvalidateRect(g_fwd, NULL, TRUE);
    InvalidateRect(g_up, NULL, TRUE);
}

static void show_page(enum page_id p)
{
    HWND child;
    RECT rc;
    g_cur = p;
    /* a new page: its children, items and scroll position go */
    KillTimer(g_page, 1);
    while ((child = GetWindow(g_page, GW_CHILD))) DestroyWindow(child);
    clear_items();
    g_pane_w = 0; g_scroll = 0; g_content_h = 0;
    SetScrollPos(g_page, SB_VERT, 0, FALSE);
    g_pages[p].build();
    update_scrollbar();
    GetClientRect(g_page, &rc);
    InvalidateRect(g_page, NULL, TRUE);
    set_title();
    /* focus the page's first control, so the keyboard starts on the content */
    child = GetNextDlgTabItem(g_page, NULL, FALSE);
    if (child && GetForegroundWindow() == g_main && GetFocus() != g_search) SetFocus(child);
}

void navigate(enum page_id p)
{
    if (p >= PG_COUNT) return;
    if (p != PG_ALL) { g_search_text[0] = 0; if (g_search && GetWindowTextLengthW(g_search)) SetWindowTextW(g_search, L""); }
    if (p != g_cur) users_reset();
    if (g_hist_pos < 0 || g_hist[g_hist_pos] != p) {
        if (g_hist_pos == ARRAYSIZE(g_hist) - 1) { memmove(g_hist, g_hist + 1, sizeof(g_hist) - sizeof(g_hist[0])); g_hist_pos--; }
        g_hist[++g_hist_pos] = p;
        g_hist_n = g_hist_pos + 1;
    }
    show_page(p);
}

/* a rebuild keeps the reader's place: a change far down the page does not jump it to the top */
void refresh_page(void)
{
    int old = g_scroll;
    show_page(g_cur);
    scroll_to(old);
}

void page_scroll_to(int y) { scroll_to(y); }

/* After an elevated program ran, the page is rebuilt when this window is
 * active again: what it changed shows without a manual refresh. */
void refresh_when_back(void) { g_refresh_on_activate = TRUE; }

/* ---- the navigation bar -------------------------------------------------------- */
static void draw_arrow(HDC dc, RECT *r, int dir, BOOL enabled, BOOL hot)
{
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2, a = S(6);
    HPEN pen = CreatePen(PS_SOLID, S(2), enabled ? (hot ? COL_LINK_HOT : RGB(0x40, 0x40, 0x40)) : RGB(0xC0, 0xC0, 0xC0));
    HGDIOBJ old = SelectObject(dc, pen);
    switch (dir) {
    case 0: MoveToEx(dc, cx + a, cy, NULL); LineTo(dc, cx - a, cy); MoveToEx(dc, cx - a + S(5), cy - S(5), NULL);
            LineTo(dc, cx - a, cy); LineTo(dc, cx - a + S(5), cy + S(5) + 1); break;           /* back */
    case 1: MoveToEx(dc, cx - a, cy, NULL); LineTo(dc, cx + a, cy); MoveToEx(dc, cx + a - S(5), cy - S(5), NULL);
            LineTo(dc, cx + a, cy); LineTo(dc, cx + a - S(5), cy + S(5) + 1); break;           /* forward */
    case 2: MoveToEx(dc, cx, cy + a, NULL); LineTo(dc, cx, cy - a); MoveToEx(dc, cx - S(5), cy - a + S(5), NULL);
            LineTo(dc, cx, cy - a); LineTo(dc, cx + S(5) + 1, cy - a + S(5) + 1); break;       /* up */
    case 3:                                                                                   /* refresh */
        draw_icon(dc, enabled ? IC_REFRESH : IC_REFRESH, cx - S(8), cy - S(8), S(16));
        break;
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

static WNDPROC g_btn_proc;
static LRESULT CALLBACK navbtn_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEMOVE && !GetPropW(hwnd, L"hot")) {
        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
        SetPropW(hwnd, L"hot", (HANDLE)1); TrackMouseEvent(&t); InvalidateRect(hwnd, NULL, FALSE);
    } else if (msg == WM_MOUSELEAVE) {
        RemovePropW(hwnd, L"hot"); InvalidateRect(hwnd, NULL, FALSE);
    } else if (msg == WM_NCDESTROY) RemovePropW(hwnd, L"hot");
    return CallWindowProcW(g_btn_proc, hwnd, msg, wp, lp);
}

/* the address bar: a breadcrumb of the page's ancestry; each segment navigates */
struct crumb { enum page_id page; RECT r; };
static struct crumb g_crumbs[4];
static int g_ncrumbs;

static LRESULT CALLBACK addr_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r; GetClientRect(hwnd, &r);
        enum page_id chain[4];
        int n = 0, i, x;
        HBRUSH b = CreateSolidBrush(COL_BG);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xD9, 0xD9, 0xD9));
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, b);
        Rectangle(dc, r.left, r.top, r.right, r.bottom);
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen); DeleteObject(b);
        for (enum page_id p = g_cur; p != PG_COUNT && n < 4; p = g_pages[p].parent) chain[n++] = p;
        draw_icon(dc, IC_SYSTEM, S(6), (r.bottom - S(18)) / 2, S(18));
        x = S(30);
        SelectObject(dc, g_font_body);
        SetBkMode(dc, TRANSPARENT);
        g_ncrumbs = 0;
        for (i = n - 1; i >= 0; i--) {
            const WCHAR *t = g_pages[chain[i]].title;
            SIZE sz;
            GetTextExtentPoint32W(dc, t, lstrlenW(t), &sz);
            SetTextColor(dc, COL_TEXT);
            TextOutW(dc, x, (r.bottom - sz.cy) / 2, t, lstrlenW(t));
            g_crumbs[g_ncrumbs].page = chain[i];
            SetRect(&g_crumbs[g_ncrumbs].r, x, 0, x + sz.cx, r.bottom);
            g_ncrumbs++;
            x += sz.cx + S(8);
            if (i) {
                SetTextColor(dc, COL_SUBTLE);
                TextOutW(dc, x, (r.bottom - sz.cy) / 2, L"\x203A", 1);
                x += S(14);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR: {
        POINT pt; int i;
        GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
        for (i = 0; i < g_ncrumbs - 1; i++)
            if (PtInRect(&g_crumbs[i].r, pt)) { SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND)); return TRUE; }
        break;
    }
    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int i;
        for (i = 0; i < g_ncrumbs - 1; i++)
            if (PtInRect(&g_crumbs[i].r, pt)) { navigate(g_crumbs[i].page); break; }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static WNDPROC g_edit_proc;
static LRESULT CALLBACK search_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT r = CallWindowProcW(g_edit_proc, hwnd, msg, wp, lp);
    if (msg == WM_PAINT && !GetWindowTextLengthW(hwnd) && GetFocus() != hwnd) {
        HDC dc = GetDC(hwnd);
        RECT rc;
        GetClientRect(hwnd, &rc);
        rc.left += S(4);
        SelectObject(dc, g_font_body);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0x80, 0x80, 0x80));
        DrawTextW(dc, L"Search Control Panel", -1, &rc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        ReleaseDC(hwnd, dc);
    } else if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) InvalidateRect(hwnd, NULL, TRUE);
    return r;
}

static void layout(void)
{
    RECT r;
    int w, sw = S(240), y = (S(NAVBAR_H) - S(30)) / 2;
    GetClientRect(g_main, &r);
    w = r.right;
    MoveWindow(g_back, S(8), y, S(30), S(30), TRUE);
    MoveWindow(g_fwd, S(40), y, S(30), S(30), TRUE);
    MoveWindow(g_up, S(76), y, S(30), S(30), TRUE);
    MoveWindow(g_addr, S(114), y, w - sw - S(114) - S(56), S(30), TRUE);
    MoveWindow(g_refresh, w - sw - S(50), y, S(30), S(30), TRUE);
    MoveWindow(g_search, w - sw - S(12), y + S(3), sw, S(24), TRUE);
    MoveWindow(g_page, 0, S(NAVBAR_H), w, r.bottom - S(NAVBAR_H), TRUE);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        if (g_page) {
            layout();
            if (wp != SIZE_MINIMIZED) refresh_page();   /* pages lay out to the width */
        }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(820); mm->ptMinTrackSize.y = S(520);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && g_refresh_on_activate) {
            g_refresh_on_activate = FALSE;
            refresh_page();
        }
        break;
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(hwnd, &r); r.bottom = S(NAVBAR_H);
        FillRect((HDC)wp, &r, g_bg_brush);
        r.top = r.bottom - 1;
        { HBRUSH b = CreateSolidBrush(COL_RULE); FillRect((HDC)wp, &r, b); DeleteObject(b); }
        return 1;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        int dir = di->CtlID == ID_BACK ? 0 : di->CtlID == ID_FWD ? 1 : di->CtlID == ID_UP ? 2 : 3;
        BOOL hot = GetPropW(di->hwndItem, L"hot") != NULL && !(di->itemState & ODS_DISABLED);
        HBRUSH b = CreateSolidBrush(hot ? RGB(0xE5, 0xF1, 0xFB) : COL_NAVBAR);
        FillRect(di->hDC, &di->rcItem, b); DeleteObject(b);
        draw_arrow(di->hDC, &di->rcItem, dir, !(di->itemState & ODS_DISABLED), hot);
        if ((di->itemState & ODS_FOCUS) && g_kbd_cues) DrawFocusRect(di->hDC, &di->rcItem);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BACK: if (g_hist_pos > 0) show_page(g_hist[--g_hist_pos]); return 0;
        case ID_FWD: if (g_hist_pos < g_hist_n - 1) show_page(g_hist[++g_hist_pos]); return 0;
        case ID_UP: if (g_pages[g_cur].parent != PG_COUNT) navigate(g_pages[g_cur].parent); return 0;
        case ID_REFRESH: refresh_page(); return 0;
        case ID_SEARCH:
            if (HIWORD(wp) == EN_CHANGE) {
                GetWindowTextW(g_search, g_search_text, ARRAYSIZE(g_search_text));
                if (g_search_text[0] || g_cur == PG_ALL) {
                    if (g_cur != PG_ALL) navigate(PG_ALL); else refresh_page();
                }
            }
            return 0;
        }
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- fonts ------------------------------------------------------------------------- */
static HFONT font(int pt, int weight)
{
    return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static void make_fonts(void)
{
    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    if (g_dpi < 96) g_dpi = 96;
    g_font_title = font(12, FW_NORMAL);
    g_font_cat   = font(11, FW_NORMAL);
    g_font_head  = font(9, FW_SEMIBOLD);
    g_font_body  = font(9, FW_NORMAL);
    g_font_small = font(8, FW_NORMAL);
    g_font_big   = font(24, FW_LIGHT);
}

/* ---- control.exe's command line ------------------------------------------------ */
/* What Windows programs and scripts pass to control.exe, and where it goes. A
 * hosted applet (a .cpl this Control Panel does not replace) runs as itself. */
struct target { const WCHAR *name; enum page_id page; const WCHAR *cpl; };
static const struct target TARGETS[] = {
    /* .cpl files */
    { L"appwiz.cpl",   PG_PROGRAMS },   { L"timedate.cpl", PG_DATETIME },
    { L"sysdm.cpl",    PG_SYSTEM },     { L"desk.cpl",     PG_COUNT, L"desk.cpl" },
    { L"ncpa.cpl",     PG_NETWORK },    { L"inetcpl.cpl",  PG_COUNT, L"inetcpl.cpl" },
    { L"joy.cpl",      PG_COUNT, L"joy.cpl" },
    { L"nusrmgr.cpl",  PG_USERS },      { L"wscui.cpl",    PG_CAT_SYSSEC },
    { L"intl.cpl",     PG_DATETIME },   { L"mmsys.cpl",    PG_CAT_HW },
    { L"main.cpl",     PG_CAT_HW },     { L"powercfg.cpl", PG_CAT_HW },
    { L"hdwwiz.cpl",   PG_CAT_HW },     { L"wuaucpl.cpl",  PG_UPDATE },
    { L"firewall.cpl", PG_CAT_SYSSEC },
    /* keywords */
    { L"userpasswords",  PG_USERS },    { L"userpasswords2", PG_USERS_MANAGE },
    { L"desktop",        PG_PERSONALIZE }, { L"color",       PG_PERSONALIZE },
    { L"date/time",      PG_DATETIME }, { L"international", PG_DATETIME },
    { L"netconnections", PG_NETWORK },  { L"update",        PG_UPDATE },
    { L"system",         PG_SYSTEM },   { L"printers",      PG_CAT_HW },
    { L"mouse",          PG_CAT_HW },   { L"keyboard",      PG_CAT_HW },
    { L"admintools",     PG_ADMINTOOLS },
    /* canonical names (control /name ...) */
    { L"Microsoft.System",                  PG_SYSTEM },
    { L"Microsoft.ProgramsAndFeatures",     PG_PROGRAMS },
    { L"Microsoft.UserAccounts",            PG_USERS },
    { L"Microsoft.DateAndTime",             PG_DATETIME },
    { L"Microsoft.RegionAndLanguage",       PG_DATETIME },
    { L"Microsoft.Personalization",         PG_PERSONALIZE },
    { L"Microsoft.WindowsUpdate",           PG_UPDATE },
    { L"Microsoft.NetworkAndSharingCenter", PG_NETWORK },
    { L"Microsoft.NetworkConnections",      PG_NETWORK },
    { L"Microsoft.InternetOptions",         PG_COUNT, L"inetcpl.cpl" },
    { L"Microsoft.GameControllers",         PG_COUNT, L"joy.cpl" },
    { L"Microsoft.Display",                 PG_COUNT, L"desk.cpl" },
    { L"Microsoft.DevicesAndPrinters",      PG_CAT_HW },
    { L"Microsoft.ActionCenter",            PG_CAT_SYSSEC },
    { L"Microsoft.SecurityAndMaintenance",  PG_CAT_SYSSEC },
    { L"Microsoft.SpeechRecognition",       PG_SPEECH },
    { L"Microsoft.AdministrativeTools",     PG_ADMINTOOLS },
    /* our own page names, for --page */
    { L"home", PG_HOME }, { L"all", PG_ALL }, { L"programs", PG_PROGRAMS }, { L"users", PG_USERS },
    { L"accounts", PG_USERS_MANAGE }, { L"datetime", PG_DATETIME }, { L"personalization", PG_PERSONALIZE },
    { L"network", PG_NETWORK }, { L"speech", PG_SPEECH }, { L"administrative-tools", PG_ADMINTOOLS }, { L"cat-system", PG_CAT_SYSSEC }, { L"cat-network", PG_CAT_NET },
    { L"cat-hardware", PG_CAT_HW }, { L"cat-programs", PG_CAT_PROG }, { L"cat-users", PG_CAT_USERS },
    { L"cat-appearance", PG_CAT_APPEAR }, { L"cat-clock", PG_CAT_CLOCK },
};

/* Network Connections is its own program (sg-ncpa), when it is installed. */
BOOL open_network_connections(void)
{
    WCHAR path[MAX_PATH], *slash;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!(slash = wcsrchr(path, L'\\'))) return FALSE;
    lstrcpyW(slash + 1, sizeof(void *) == 8 ? L"sg-ncpa64.exe" : L"sg-ncpa32.exe");
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return FALSE;
    if (!CreateProcessW(path, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return FALSE;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return TRUE;
}

/* Resolve a control.exe argument. Returns the page, or PG_COUNT with *cpl set
 * to a hosted applet, or PG_COUNT with *cpl NULL for "unknown". */
static enum page_id resolve(const WCHAR *arg, const WCHAR **cpl)
{
    WCHAR name[MAX_PATH], *comma;
    const WCHAR *base;
    size_t i;
    *cpl = NULL;
    lstrcpynW(name, arg, MAX_PATH);
    if ((comma = wcschr(name, L','))) *comma = 0;          /* appwiz.cpl,,2 / desk.cpl,@0 */
    base = wcsrchr(name, L'\\') ? wcsrchr(name, L'\\') + 1 : name;
    for (i = 0; i < ARRAYSIZE(TARGETS); i++)
        if (!_wcsicmp(base, TARGETS[i].name)) {
            *cpl = TARGETS[i].cpl;
            return TARGETS[i].page;
        }
    return PG_COUNT;
}

/* open the applet: our page, a hosted .cpl, or a .cpl file named by path */
static int open_target(const WCHAR *arg, enum page_id *page)
{
    const WCHAR *cpl;
    WCHAR base[MAX_PATH];
    const WCHAR *b = wcsrchr(arg, L'\\') ? wcsrchr(arg, L'\\') + 1 : arg;
    lstrcpynW(base, b, MAX_PATH);
    if (wcschr(base, L',')) *wcschr(base, L',') = 0;
    if (!_wcsicmp(base, L"ncpa.cpl") || !_wcsicmp(arg, L"netconnections") ||
        !_wcsicmp(arg, L"Microsoft.NetworkConnections")) {
        if (open_network_connections()) return 1;         /* handled: no window of ours */
    }
    *page = resolve(arg, &cpl);
    if (*page != PG_COUNT) return 0;
    if (cpl) return cpl_run_inproc(cpl, NULL) >= 0 ? 1 : 0;
    /* any other .cpl -- a third party's -- runs as itself */
    if (wcslen(base) > 4 && !_wcsicmp(base + wcslen(base) - 4, L".cpl")) {
        const WCHAR *comma = wcschr(arg, L',');
        WCHAR file[MAX_PATH];
        lstrcpynW(file, arg, MAX_PATH);
        if (wcschr(file, L',')) *wcschr(file, L',') = 0;
        return cpl_run_inproc(file, comma ? comma + 1 : NULL) >= 0 ? 1 : 0;
    }
    *page = PG_HOME;
    return 0;
}

/* ---- --dump: what the panel shows, for the gate ----------------------------------- */
static int dump(const WCHAR *what)
{
    struct { const WCHAR *name; void (*fn)(void); } parts[] = {
        { L"system", dump_system }, { L"programs", dump_programs }, { L"users", dump_users },
        { L"datetime", dump_datetime }, { L"personalization", dump_personalize },
        { L"update", dump_update }, { L"network", dump_network }, { L"speech", dump_speech }, { L"admintools", dump_admintools },
        { L"items", dump_items },
    };
    size_t i;
    BOOL any = FALSE;
    for (i = 0; i < ARRAYSIZE(parts); i++)
        if (!what || !_wcsicmp(what, parts[i].name)) { parts[i].fn(); any = TRUE; }
    fflush(stdout);
    return any ? 0 : 2;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSW wc = { 0 };
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_DATE_CLASSES | ICC_TAB_CLASSES };
    enum page_id start = PG_HOME;
    int argc = 0, i;
    WCHAR **argv;
    MSG msg;
    (void)prev; (void)cmd;

    g_inst = inst;
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    InitCommonControlsEx(&icc);
    make_fonts();
    g_bg_brush = CreateSolidBrush(COL_BG);
    g_pane_brush = CreateSolidBrush(COL_PANE);

    if (argc >= 2) {
        if (!wcscmp(argv[1], L"--dump")) return dump(argc > 2 ? argv[2] : NULL);
        if (!wcscmp(argv[1], L"--set")) return personalize_set(argc - 2, argv + 2);
        if (!wcscmp(argv[1], L"--uninstall") && argc > 2) return programs_uninstall_cli(argv[2]);
        if (!wcscmp(argv[1], L"--resolve") && argc > 2) {
            /* what control.exe ARG would open, without opening it */
            const WCHAR *cpl, *a = argv[2];
            enum page_id p;
            WCHAR base[MAX_PATH];
            lstrcpynW(base, wcsrchr(a, L'\\') ? wcsrchr(a, L'\\') + 1 : a, MAX_PATH);
            if (wcschr(base, L',')) *wcschr(base, L',') = 0;
            if (!_wcsicmp(base, L"ncpa.cpl") || !_wcsicmp(a, L"netconnections") || !_wcsicmp(a, L"Microsoft.NetworkConnections"))
                wprintf(L"program=sg-ncpa (else page=%ls)\n", g_pages[PG_NETWORK].title);
            else if ((p = resolve(a, &cpl)) != PG_COUNT) wprintf(L"page=%ls\n", g_pages[p].title);
            else if (cpl) wprintf(L"cpl=%ls\n", cpl);
            else if (wcslen(base) > 4 && !_wcsicmp(base + wcslen(base) - 4, L".cpl")) wprintf(L"cpl=%ls\n", a);
            else wprintf(L"page=%ls\n", g_pages[PG_HOME].title);
            fflush(stdout);
            return 0;
        }
        if (!_wcsicmp(argv[1], L"/admin")) return admin_main(argc - 2, argv + 2);
        if (!_wcsicmp(argv[1], L"/admin-do")) return admin_do(argc - 2, argv + 2);
        if (!_wcsicmp(argv[1], L"/cpl") && argc > 2) return cpl_run_inproc(argv[2], argc > 3 ? argv[3] : NULL) >= 0 ? 0 : 1;
        for (i = 1; i < argc; i++) {
            const WCHAR *a = argv[i];
            if ((!_wcsicmp(a, L"/name") || !_wcsicmp(a, L"-name") || !wcscmp(a, L"--page")) && i + 1 < argc) a = argv[++i];
            else if (!_wcsicmp(a, L"/page") && i + 1 < argc) { i++; continue; }
            if (open_target(a, &start)) return 0;
            break;
        }
    }

    wc.lpfnWndProc = link_proc; wc.hInstance = inst; wc.lpszClassName = L"SgCplLink";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
    RegisterClassW(&wc);
    wc.lpfnWndProc = page_proc; wc.lpszClassName = L"SgCplPage"; wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    wc.lpfnWndProc = addr_proc; wc.lpszClassName = L"SgCplAddress";
    RegisterClassW(&wc);
    wc.lpfnWndProc = main_proc; wc.lpszClassName = L"SgControlWindow";
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    RegisterClassW(&wc);

    g_main = CreateWindowExW(WS_EX_CONTROLPARENT, L"SgControlWindow", L"Control Panel",
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, S(1000), S(700), NULL, NULL, inst, NULL);
    if (!g_main) return 1;
    g_back = CreateWindowW(L"BUTTON", L"Back", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, g_main, (HMENU)ID_BACK, inst, NULL);
    g_fwd = CreateWindowW(L"BUTTON", L"Forward", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, g_main, (HMENU)ID_FWD, inst, NULL);
    g_up = CreateWindowW(L"BUTTON", L"Up", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, g_main, (HMENU)ID_UP, inst, NULL);
    g_refresh = CreateWindowW(L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, g_main, (HMENU)ID_REFRESH, inst, NULL);
    g_btn_proc = (WNDPROC)SetWindowLongPtrW(g_back, GWLP_WNDPROC, (LONG_PTR)navbtn_proc);
    SetWindowLongPtrW(g_fwd, GWLP_WNDPROC, (LONG_PTR)navbtn_proc);
    SetWindowLongPtrW(g_up, GWLP_WNDPROC, (LONG_PTR)navbtn_proc);
    SetWindowLongPtrW(g_refresh, GWLP_WNDPROC, (LONG_PTR)navbtn_proc);
    g_addr = CreateWindowW(L"SgCplAddress", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g_main, NULL, inst, NULL);
    g_search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL,
                               0, 0, 0, 0, g_main, (HMENU)ID_SEARCH, inst, NULL);
    SendMessageW(g_search, WM_SETFONT, (WPARAM)g_font_body, TRUE);
    g_edit_proc = (WNDPROC)SetWindowLongPtrW(g_search, GWLP_WNDPROC, (LONG_PTR)search_proc);
    g_page = CreateWindowExW(WS_EX_CONTROLPARENT, L"SgCplPage", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPCHILDREN,
                             0, 0, 0, 0, g_main, NULL, inst, NULL);
    layout();
    navigate(start);
    ShowWindow(g_main, show ? show : SW_SHOW);
    UpdateWindow(g_main);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && !g_kbd_cues && (msg.wParam == VK_TAB || (msg.wParam >= VK_LEFT && msg.wParam <= VK_DOWN))) {
            g_kbd_cues = TRUE;
            if (GetFocus()) InvalidateRect(GetFocus(), NULL, FALSE);
        }
        if (IsDialogMessageW(g_main, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
