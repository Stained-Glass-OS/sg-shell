/* sg-taskmgr -- the list every page shows: our own control, drawn as Windows
 * 10's Task Manager draws its lists -- a two-line header (the total above
 * the column's name), group rows ("Apps (3)"), cells shaded by how busy a
 * process is, a sort caret, the selection kept across refreshes.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "taskmgr.h"

#define GRID_CLASS L"SgTaskmgrGrid"

int grid_row_height(void) { return S(24); }
int grid_header_height(grid_t *g) { return g->noheader ? 0 : S(44); }

static int visible_rows(grid_t *g)
{
    RECT rc;
    int n;
    GetClientRect(g->hwnd, &rc);
    n = (rc.bottom - grid_header_height(g)) / grid_row_height();
    return n > 0 ? n : 0;
}

/* column x positions: the first column takes what the others leave */
static void col_edges(grid_t *g, int *x, int width)
{
    int i, fixed = 0;
    for (i = 1; i < g->ncol; i++) fixed += S(g->cols[i].width);
    x[0] = 0;
    x[1] = max(width - fixed, S(g->cols[0].width));
    for (i = 1; i < g->ncol; i++) x[i + 1] = x[i] + S(g->cols[i].width);
}

static void update_scroll(grid_t *g)
{
    SCROLLINFO si = { sizeof(si), SIF_ALL, 0, 0, 0, 0, 0 };
    int page = visible_rows(g);
    if (g->top > g->nrows - page) g->top = g->nrows - page;
    if (g->top < 0) g->top = 0;
    si.nMax = g->nrows ? g->nrows - 1 : 0;
    si.nPage = page;
    si.nPos = g->top;
    SetScrollInfo(g->hwnd, SB_VERT, &si, TRUE);
}

void grid_begin(grid_t *g)
{
    grow_t *s = grid_selected(g);
    if (s) { g->sel_key = s->key; lstrcpynW(g->sel_skey, s->skey, 64); }
    else if (g->sel < 0) { g->sel_key = 0; g->sel_skey[0] = 0; }
    g->nrows = 0;
}

grow_t *grid_add(grid_t *g)
{
    grow_t *r;
    if (g->nrows == g->cap)
    {
        int cap = g->cap ? g->cap * 2 : 64;
        grow_t *n = realloc(g->rows, cap * sizeof(grow_t));
        if (!n) return NULL;
        g->rows = n; g->cap = cap;
    }
    r = &g->rows[g->nrows++];
    memset(r, 0, sizeof(*r));
    r->group = -1;
    return r;
}

static grid_t *g_sorting;

static int row_cmp(const void *a, const void *b)
{
    const grow_t *x = a, *y = b;
    grid_t *g = g_sorting;
    int c = g->sortcol, r;
    if (x->group != y->group) return x->group - y->group;
    if (x->header != y->header) return x->header ? -1 : 1;
    if (c < 0) return 0;
    if (g->cols[c].numeric)
        r = x->num[c] < y->num[c] ? -1 : x->num[c] > y->num[c] ? 1 : 0;
    else
        r = lstrcmpiW(x->text[c], y->text[c]);
    if (!r) r = lstrcmpiW(x->text[0], y->text[0]);
    return g->sortdesc ? -r : r;
}

void grid_end(grid_t *g)
{
    int i;
    g_sorting = g;
    qsort(g->rows, g->nrows, sizeof(grow_t), row_cmp);
    g->sel = -1;
    if (g->sel_key || g->sel_skey[0])
        for (i = 0; i < g->nrows; i++)
            if (!g->rows[i].header &&
                (g->sel_skey[0] ? !lstrcmpW(g->rows[i].skey, g->sel_skey) : g->rows[i].key == g->sel_key))
            { g->sel = i; break; }
    update_scroll(g);
    InvalidateRect(g->hwnd, NULL, FALSE);
}

grow_t *grid_selected(grid_t *g)
{
    return g->sel >= 0 && g->sel < g->nrows ? &g->rows[g->sel] : NULL;
}

BOOL grid_row_rect(grid_t *g, int i, RECT *out)
{
    RECT rc;
    int y = grid_header_height(g) + (i - g->top) * grid_row_height();
    GetClientRect(g->hwnd, &rc);
    if (i < g->top || y + grid_row_height() > rc.bottom) return FALSE;
    SetRect(out, 0, y, rc.right, y + grid_row_height());
    return TRUE;
}

/* Windows' heat scale: pale yellow when idle, deepening as the value grows */
static COLORREF heat_colour(double f)
{
    static const int stops[3][3] = { { 255, 244, 196 }, { 255, 210, 110 }, { 255, 160, 60 } };
    int i = f < 0.5 ? 0 : 1;
    double t;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    t = f < 0.5 ? f * 2 : (f - 0.5) * 2;
    return RGB(stops[i][0] + (stops[i + 1][0] - stops[i][0]) * t,
               stops[i][1] + (stops[i + 1][1] - stops[i][1]) * t,
               stops[i][2] + (stops[i + 1][2] - stops[i][2]) * t);
}

static void fill(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT rc = { l, t, r, b };
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void paint(grid_t *g, HDC dc, RECT *rc)
{
    int x[GRID_MAXCOL + 1], i, c, hh = grid_header_height(g), rh = grid_row_height();
    int pad = S(8);

    fill(dc, 0, 0, rc->right, rc->bottom, C_BG);
    col_edges(g, x, rc->right);
    SetBkMode(dc, TRANSPARENT);

    if (hh)
    {
        for (c = 0; c < g->ncol; c++)
        {
            RECT t = { x[c] + pad, S(4), x[c + 1] - pad, S(24) }, n = { x[c] + pad, S(22), x[c + 1] - pad, hh - S(4) };
            UINT al = (g->cols[c].right ? DT_RIGHT : DT_LEFT) | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;
            if (g->hover_head == c) fill(dc, x[c], 0, x[c + 1], hh, C_HOVER);
            SetTextColor(dc, C_TEXT);
            if (g->cols[c].total[0])
            {
                SelectObject(dc, g_font_head);
                DrawTextW(dc, g->cols[c].total, -1, &t, al);
            }
            SelectObject(dc, g_font);
            SetTextColor(dc, C_SUBTLE);
            DrawTextW(dc, g->cols[c].name, -1, &n, al | DT_BOTTOM);
            if (c > 0) fill(dc, x[c], S(6), x[c] + 1, hh - S(6), C_LINE);
            if (g->sortcol == c)
            {
                /* the sort caret, centred above the name */
                POINT p[3];
                int cx = (x[c] + x[c + 1]) / 2, cy = S(3), w = S(4);
                HBRUSH br = CreateSolidBrush(C_SUBTLE);
                HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
                if (g->sortdesc) { p[0].x = cx - w; p[0].y = cy; p[1].x = cx + w; p[1].y = cy; p[2].x = cx; p[2].y = cy + w; }
                else { p[0].x = cx - w; p[0].y = cy + w; p[1].x = cx + w; p[1].y = cy + w; p[2].x = cx; p[2].y = cy; }
                Polygon(dc, p, 3);
                SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br);
            }
        }
        fill(dc, 0, hh - 1, rc->right, hh, C_LINE);
    }

    for (i = g->top; i < g->nrows; i++)
    {
        grow_t *r = &g->rows[i];
        int y = hh + (i - g->top) * rh;
        if (y >= rc->bottom) break;
        if (r->header)
        {
            RECT t = { pad, y + S(4), rc->right - pad, y + rh };
            SelectObject(dc, g_font_bold);
            SetTextColor(dc, C_GROUP);
            DrawTextW(dc, r->text[0], -1, &t, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
            continue;
        }
        for (c = 0; c < g->ncol; c++)
            if (g->cols[c].heat && g->heatmax[c] > 0)
                fill(dc, x[c], y, x[c + 1], y + rh, heat_colour(r->num[c] / g->heatmax[c]));
        if (i == g->sel) fill(dc, 0, y, rc->right, y + rh, C_ACCENT_LT);
        else if (i == g->hover) fill(dc, 0, y, x[1], y + rh, C_HOVER);
        SelectObject(dc, g_font);
        SetTextColor(dc, C_TEXT);
        for (c = 0; c < g->ncol; c++)
        {
            RECT t = { x[c] + pad, y, x[c + 1] - pad, y + rh };
            if (c == 0)
            {
                if (r->group >= 0) t.left += S(16);
                if (r->icon)
                {
                    DrawIconEx(dc, t.left, y + (rh - S(16)) / 2, r->icon, S(16), S(16), 0, NULL, DI_NORMAL);
                    t.left += S(24);
                }
            }
            DrawTextW(dc, r->text[c], -1, &t,
                      (g->cols[c].right ? DT_RIGHT : DT_LEFT) | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }
}

static int hit_row(grid_t *g, int y)
{
    int hh = grid_header_height(g), i;
    if (y < hh) return -2;
    i = g->top + (y - hh) / grid_row_height();
    return i < g->nrows ? i : -1;
}

static int hit_col(grid_t *g, int xpos)
{
    RECT rc;
    int x[GRID_MAXCOL + 1], c;
    GetClientRect(g->hwnd, &rc);
    col_edges(g, x, rc.right);
    for (c = 0; c < g->ncol; c++) if (xpos >= x[c] && xpos < x[c + 1]) return c;
    return -1;
}

static void notify(grid_t *g, int code)
{
    SendMessageW(GetParent(g->hwnd), WM_APP_GRID, code, (LPARAM)g);
}

static void select_row(grid_t *g, int i)
{
    if (i < 0 || i >= g->nrows) return;
    /* the group rows are headings, not things to act on */
    if (g->rows[i].header) return;
    if (g->sel != i)
    {
        int page = visible_rows(g);
        g->sel = i;
        g->sel_key = g->rows[i].key;
        lstrcpynW(g->sel_skey, g->rows[i].skey, 64);
        if (i < g->top) g->top = i;
        if (page && i >= g->top + page) g->top = i - page + 1;
        update_scroll(g);
        InvalidateRect(g->hwnd, NULL, FALSE);
        notify(g, GN_SELCHANGE);
    }
}

static void step(grid_t *g, int dir, int count)
{
    int i = g->sel < 0 ? (dir > 0 ? -1 : g->nrows) : g->sel;
    while (count-- > 0)
    {
        int j = i + dir;
        while (j >= 0 && j < g->nrows && g->rows[j].header) j += dir;
        if (j < 0 || j >= g->nrows) break;
        i = j;
    }
    select_row(g, i);
}

static LRESULT CALLBACK grid_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    grid_t *g = (grid_t *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_NCCREATE:
        g = ((CREATESTRUCTW *)lp)->lpCreateParams;
        g->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)g);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps), mem;
        RECT rc;
        HBITMAP bmp;
        HGDIOBJ old;
        GetClientRect(hwnd, &rc);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, max(rc.right, 1), max(rc.bottom, 1));
        old = SelectObject(mem, bmp);
        paint(g, mem, &rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        update_scroll(g);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_VSCROLL:
    {
        SCROLLINFO si = { sizeof(si), SIF_ALL, 0, 0, 0, 0, 0 };
        int page = visible_rows(g);
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (LOWORD(wp))
        {
        case SB_LINEUP: g->top--; break;
        case SB_LINEDOWN: g->top++; break;
        case SB_PAGEUP: g->top -= page; break;
        case SB_PAGEDOWN: g->top += page; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: g->top = si.nTrackPos; break;
        case SB_TOP: g->top = 0; break;
        case SB_BOTTOM: g->top = g->nrows; break;
        }
        update_scroll(g);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        g->top -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 3;
        update_scroll(g);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE:
    {
        int y = (short)HIWORD(lp), row = hit_row(g, y), head = row == -2 ? hit_col(g, (short)LOWORD(lp)) : -1;
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        if (row < 0) row = -1;
        if (row != g->hover || head != g->hover_head)
        {
            g->hover = row; g->hover_head = head;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g->hover = g->hover_head = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    {
        int row = hit_row(g, (short)HIWORD(lp));
        SetFocus(hwnd);
        if (row == -2 && msg == WM_LBUTTONDOWN && !g->noheader)
        {
            int c = hit_col(g, (short)LOWORD(lp));
            if (c >= 0)
            {
                if (g->sortcol == c) g->sortdesc = !g->sortdesc;
                else { g->sortcol = c; g->sortdesc = g->cols[c].numeric; }
                notify(g, GN_SORT);
            }
            return 0;
        }
        if (row >= 0) select_row(g, row);
        return 0;
    }
    case WM_RBUTTONUP:
        if (hit_row(g, (short)HIWORD(lp)) >= 0 && grid_selected(g)) notify(g, GN_RCLICK);
        return 0;
    case WM_LBUTTONDBLCLK:
        if (hit_row(g, (short)HIWORD(lp)) >= 0 && grid_selected(g)) notify(g, GN_DBLCLK);
        return 0;
    case WM_CONTEXTMENU:
        if (lp == (LPARAM)-1 && grid_selected(g)) notify(g, GN_RCLICK);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_KEYDOWN:
        switch (wp)
        {
        case VK_UP: step(g, -1, 1); return 0;
        case VK_DOWN: step(g, 1, 1); return 0;
        case VK_PRIOR: step(g, -1, visible_rows(g)); return 0;
        case VK_NEXT: step(g, 1, visible_rows(g)); return 0;
        case VK_HOME: g->sel = -1; step(g, 1, 1); return 0;
        case VK_END: g->sel = -1; step(g, -1, 1); return 0;
        case VK_DELETE: if (grid_selected(g)) notify(g, GN_DELETE); return 0;
        case VK_RETURN: if (grid_selected(g)) notify(g, GN_DBLCLK); return 0;
        }
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void grid_register(HINSTANCE inst)
{
    WNDCLASSW wc = { 0 };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = grid_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = GRID_CLASS;
    RegisterClassW(&wc);
}

HWND grid_create(grid_t *g, HWND parent, int id)
{
    g->sel = g->hover = g->hover_head = -1;
    return CreateWindowExW(0, GRID_CLASS, L"", WS_CHILD | WS_VSCROLL | WS_TABSTOP, 0, 0, 10, 10, parent,
                           (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), g);
}
