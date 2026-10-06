/* sg-pdf -- SG PDF: Organize Pages -- the document's pages as a grid of
 * thumbnails in place of the continuous view. A click selects a page (Ctrl
 * adds or removes one, Shift a run, Ctrl+A all), a drag moves the selected
 * pages to where the insertion bar shows, a double-click goes to the page;
 * the tool's bar and pane rotate, delete, insert (a blank page, or a PDF's
 * pages or a picture from a file), extract and split.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"

HWND g_org;
static int g_oscroll, g_drop = -1, g_opress = -1;
static BOOL g_odrag;
static POINT g_odown;

#define CELL_W dpx(168)
#define CELL_H dpx(214)
#define THUMB_BOX_W dpx(132)
#define THUMB_BOX_H dpx(172)
#define PAD dpx(16)

static int cols(void)
{
    RECT rc;
    GetClientRect(g_org, &rc);
    return max(1, (rc.right - PAD) / CELL_W);
}

static void cell_rect(int i, RECT *r)
{
    RECT rc;
    int c = cols(), x0;
    GetClientRect(g_org, &rc);
    x0 = max(PAD, (rc.right - c * CELL_W) / 2);
    r->left = x0 + (i % c) * CELL_W;
    r->top = PAD + (i / c) * CELL_H - g_oscroll;
    r->right = r->left + CELL_W;
    r->bottom = r->top + CELL_H;
}

static void thumb_rect(int i, RECT *r, int *w, int *h)
{
    RECT c;
    double pw = g.pages[i].w, ph = g.pages[i].h, s;
    if (g.rot == 90 || g.rot == 270) { double t = pw; pw = ph; ph = t; }
    s = min(THUMB_BOX_W / pw, THUMB_BOX_H / ph);
    *w = max(1, (int)(pw * s));
    *h = max(1, (int)(ph * s));
    cell_rect(i, &c);
    r->left = c.left + (CELL_W - *w) / 2;
    r->top = c.top + dpx(10) + (THUMB_BOX_H - *h);
    r->right = r->left + *w;
    r->bottom = r->top + *h;
}

static int total_h(void)
{
    return g.npages ? PAD * 2 + ((g.npages + cols() - 1) / cols()) * CELL_H : 0;
}

static void org_scrollbar(void)
{
    RECT rc;
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    GetClientRect(g_org, &rc);
    g_oscroll = max(0, min(g_oscroll, total_h() - rc.bottom));
    si.nMax = max(total_h() - 1, 0);
    si.nPage = rc.bottom;
    si.nPos = g_oscroll;
    SetScrollInfo(g_org, SB_VERT, &si, TRUE);
}

int org_selected(int *out, int cap)
{
    int i, n = 0;
    for (i = 0; i < g.npages && g.org_sel; i++) if (g.org_sel[i] && n < cap) out[n++] = i;
    return n;
}

static int cell_at(POINT pt)
{
    int i;
    for (i = 0; i < g.npages; i++) {
        RECT r;
        cell_rect(i, &r);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

/* where a drop at pt puts the pages: before this index (0..npages) */
static int drop_at(POINT pt)
{
    int i, best = g.npages;
    for (i = 0; i < g.npages; i++) {
        RECT r;
        cell_rect(i, &r);
        if (pt.y < r.top) break;
        if (pt.y < r.bottom) {
            if (pt.x < (r.left + r.right) / 2) return i;
            best = i + 1;
        }
    }
    return best;
}

static void org_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC out = BeginPaint(hwnd, &ps), dc, mem;
    RECT rc;
    HBITMAP buf, ob;
    HBRUSH bg = CreateSolidBrush(C_CANVAS), acc = CreateSolidBrush(C_ACCENT), act = CreateSolidBrush(C_ACTIVE),
           sh = CreateSolidBrush(C_SHADOW);
    int i;
    GetClientRect(hwnd, &rc);
    dc = CreateCompatibleDC(out);
    buf = CreateCompatibleBitmap(out, max(rc.right, 1), max(rc.bottom, 1));
    ob = SelectObject(dc, buf);
    mem = CreateCompatibleDC(out);
    FillRect(dc, &rc, bg);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font_small);
    render_clear_wants(TRUE);
    for (i = 0; i < g.npages; i++) {
        RECT c, t, f, lr;
        int w, h;
        WCHAR num[16];
        page_t *p = &g.pages[i];
        BOOL sel = g.org_sel && g.org_sel[i];
        cell_rect(i, &c);
        if (c.bottom < 0) continue;
        if (c.top > rc.bottom) break;
        thumb_rect(i, &t, &w, &h);
        if (sel) { RECT a = c; InflateRect(&a, -dpx(4), -dpx(2)); FillRect(dc, &a, act); }
        f = t;
        OffsetRect(&f, dpx(1), dpx(2));
        FillRect(dc, &f, sh);
        f = t;
        if (sel) { InflateRect(&f, dpx(3), dpx(3)); FillRect(dc, &f, acc); }
        FillRect(dc, &t, GetStockObject(WHITE_BRUSH));
        if (p->thumb && p->trot == g.rot) {
            HGDIOBJ o = SelectObject(mem, p->thumb);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
            StretchBlt(dc, t.left, t.top, w, h, mem, 0, 0, p->tw, p->th, SRCCOPY);
            SelectObject(mem, o);
        }
        if (!p->thumb || p->tstale || p->trot != g.rot || abs(p->tw - w) > 2) {
            double pw = (g.rot == 90 || g.rot == 270) ? p->h : p->w;
            render_want(i, (double)w / pw, g.rot, TRUE);
        }
        swprintf(num, 16, L"%d", i + 1);
        SetRect(&lr, c.left, t.bottom + dpx(6), c.right, c.bottom);
        SetTextColor(dc, sel ? C_ACCENT : C_TEXT);
        SelectObject(dc, sel ? g_font_bold : g_font_small);
        DrawTextW(dc, num, -1, &lr, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
    if (g_odrag && g_drop >= 0) {
        RECT c, bar;
        if (g_drop < g.npages) { cell_rect(g_drop, &c); SetRect(&bar, c.left - dpx(2), c.top + dpx(8), c.left + dpx(2), c.bottom - dpx(24)); }
        else if (g.npages) { cell_rect(g.npages - 1, &c); SetRect(&bar, c.right - dpx(2), c.top + dpx(8), c.right + dpx(2), c.bottom - dpx(24)); }
        else SetRectEmpty(&bar);
        FillRect(dc, &bar, acc);
    }
    BitBlt(out, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(buf);
    DeleteDC(dc);
    DeleteDC(mem);
    DeleteObject(bg); DeleteObject(acc); DeleteObject(act); DeleteObject(sh);
    EndPaint(hwnd, &ps);
}

static void select_click(int i, BOOL ctrl, BOOL shift)
{
    int k;
    if (!g.org_sel || i < 0) return;
    if (shift && g.org_anchor >= 0 && g.org_anchor < g.npages) {
        if (!ctrl) for (k = 0; k < g.npages; k++) g.org_sel[k] = FALSE;
        for (k = min(i, g.org_anchor); k <= max(i, g.org_anchor); k++) g.org_sel[k] = TRUE;
    } else if (ctrl) {
        g.org_sel[i] = !g.org_sel[i];
        g.org_anchor = i;
    } else {
        for (k = 0; k < g.npages; k++) g.org_sel[k] = k == i;
        g.org_anchor = i;
    }
    g.current = i;
}

static void spec(char *out, int cap)
{
    int sel[4096], n = org_selected(sel, 4096);
    doc_pages_spec(out, cap, sel, n);
}

static void keep_selection(BOOL *before, int n)
{
    if (g.org_sel && n == g.npages) memcpy(g.org_sel, before, n * sizeof(BOOL));
}

void org_command(int cmd)
{
    char pages[16384], *e, line[20000];
    int sel[4096], n = org_selected(sel, 4096), at = n ? sel[n - 1] + 1 : g.npages, np = g.npages;
    BOOL *before;
    WCHAR file[MAX_PATH] = L"";
    if (!g.npages) return;
    spec(pages, sizeof(pages));
    before = malloc(np * sizeof(BOOL));
    if (before && g.org_sel) memcpy(before, g.org_sel, np * sizeof(BOOL));
    switch (cmd) {
    case CMD_ORG_SELALL:
        if (g.org_sel) { int k; for (k = 0; k < g.npages; k++) g.org_sel[k] = TRUE; }
        break;
    case CMD_ORG_ROTL: case CMD_ORG_ROTR:
        if (n && doc_requestf("rotate\t%s\t%d", pages, cmd == CMD_ORG_ROTR ? 90 : -90) && before) keep_selection(before, np);
        break;
    case CMD_ORG_DELETE: {
        WCHAR t[160];
        if (!n) break;
        if (n >= g.npages) { MessageBoxW(g_main, L"A document keeps at least one page.", L"Delete Pages", MB_OK | MB_ICONINFORMATION); break; }
        swprintf(t, 160, n == 1 ? L"Delete page %d?" : L"Delete the %d selected pages?", n == 1 ? sel[0] + 1 : n);
        if (GetEnvironmentVariableW(L"SG_PDF_QUIET", NULL, 0) || MessageBoxW(g_main, t, L"Delete Pages", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            doc_requestf("delete\t%s", pages);
        break;
    }
    case CMD_ORG_BLANK:
        doc_requestf("insertblank\t%d", at);
        break;
    case CMD_ORG_REPLACE:
        if (!n) break;
        if (file_dialog(FALSE, L"Replace Pages", L"PDF documents\0*.pdf\0All files (*.*)\0*.*\0", NULL, file, MAX_PATH)) {
            WCHAR from[32] = L"1", prompt[200];
            char *u = unix_path(file);
            WCHAR *wu = u ? from_utf8(u, -1) : NULL;
            e = wu ? esc_utf8(wu) : NULL;
            swprintf(prompt, 200, L"Replace the %d selected page%ls with pages of the other document, from its page:", n, n == 1 ? L"" : L"s");
            if (e && dlg_text(g_main, L"Replace Pages", prompt, from, 32, FALSE)) {
                snprintf(line, sizeof(line), "replacepages\t%s\t%s\t%d", pages, e, max(1, _wtoi(from)));
                if (doc_request(line) && before) keep_selection(before, np);
            }
            free(e); free(wu); free(u);
        }
        break;
    case CMD_ORG_INSERT:
        if (file_dialog(FALSE, L"Insert Pages", L"PDF documents and pictures\0*.pdf;*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff\0"
                                               L"All files (*.*)\0*.*\0", NULL, file, MAX_PATH)) {
            char *u = unix_path(file);
            WCHAR *wu = u ? from_utf8(u, -1) : NULL;
            e = wu ? esc_utf8(wu) : NULL;
            if (e) doc_requestf("insertfile\t%d\t%s", at, e);
            free(e); free(wu); free(u);
        }
        break;
    case CMD_ORG_EXTRACT:
        if (!n) break;
        {
            WCHAR *dot;
            lstrcpynW(file, g.name, MAX_PATH);
            if ((dot = wcsrchr(file, '.'))) *dot = 0;
            lstrcatW(file, L" (extract).pdf");
        }
        if (file_dialog(TRUE, L"Extract Pages", L"PDF documents (*.pdf)\0*.pdf\0", L"pdf", file, MAX_PATH)) {
            char *u = unix_path(file);
            WCHAR *wu = u ? from_utf8(u, -1) : NULL;
            BOOL del = FALSE;
            e = wu ? esc_utf8(wu) : NULL;
            if (n < g.npages && !GetEnvironmentVariableW(L"SG_PDF_QUIET", NULL, 0))
                del = MessageBoxW(g_main, L"Also delete the extracted pages from this document?", L"Extract Pages",
                                  MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
            if (e) {
                snprintf(line, sizeof(line), "extract\t%s\t%s\t%d", pages, e, del ? 1 : 0);
                if (doc_request(line)) app_set_status(L"The pages were saved to \"%ls\".", file);
            }
            free(e); free(wu); free(u);
        }
        break;
    case CMD_ORG_SPLIT:
        dlg_split();
        break;
    }
    free(before);
    org_update();
    toolui_update();
}

static LRESULT CALLBACK org_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: org_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: org_scrollbar(); InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        RECT rc;
        GetClientRect(hwnd, &rc);
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (LOWORD(wp)) {
        case SB_LINEUP: g_oscroll -= dpx(40); break;
        case SB_LINEDOWN: g_oscroll += dpx(40); break;
        case SB_PAGEUP: g_oscroll -= rc.bottom; break;
        case SB_PAGEDOWN: g_oscroll += rc.bottom; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: g_oscroll = si.nTrackPos; break;
        }
        org_scrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
        g_oscroll -= GET_WHEEL_DELTA_WPARAM(wp) * dpx(120) / WHEEL_DELTA;
        org_scrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int i = cell_at(pt);
        SetFocus(hwnd);
        g_odown = pt;
        g_opress = i;
        g_odrag = FALSE;
        if (i >= 0 && g.org_sel) {
            BOOL ctrl = (wp & MK_CONTROL) != 0, shift = (wp & MK_SHIFT) != 0;
            /* a press on a selected page may start a drag of the whole selection */
            if (!g.org_sel[i] || ctrl || shift) select_click(i, ctrl, shift);
            SetCapture(hwnd);
        } else if (g.org_sel) {
            int k;
            for (k = 0; k < g.npages; k++) g.org_sel[k] = FALSE;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        toolui_update();
        app_status_changed();
        return 0;
    }
    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd && g_opress >= 0) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            RECT rc;
            if (!g_odrag && abs(pt.x - g_odown.x) + abs(pt.y - g_odown.y) > dpx(6)) g_odrag = TRUE;
            if (g_odrag) {
                g_drop = drop_at(pt);
                GetClientRect(hwnd, &rc);
                if (pt.y < dpx(20)) { g_oscroll -= dpx(20); org_scrollbar(); }
                else if (pt.y > rc.bottom - dpx(20)) { g_oscroll += dpx(20); org_scrollbar(); }
                SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_SIZEALL));
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) {
            ReleaseCapture();
            if (g_odrag && g_drop >= 0) {
                char pages[16384];
                int sel[4096], n = org_selected(sel, 4096), k, first;
                spec(pages, sizeof(pages));
                first = g_drop - 0;
                g_odrag = FALSE;
                if (n && doc_requestf("move\t%s\t%d", pages, g_drop)) {
                    /* the moved pages stay selected where they went */
                    int before = 0;
                    for (k = 0; k < n; k++) if (sel[k] < first) before++;
                    if (g.org_sel) {
                        for (k = 0; k < g.npages; k++) g.org_sel[k] = k >= first - before && k < first - before + n;
                    }
                }
            } else if (g_opress >= 0 && !g_odrag && !(wp & (MK_CONTROL | MK_SHIFT))) select_click(g_opress, FALSE, FALSE);
            g_odrag = FALSE;
            g_drop = -1;
            g_opress = -1;
            InvalidateRect(hwnd, NULL, FALSE);
            toolui_update();
            app_status_changed();
        }
        return 0;
    case WM_LBUTTONDBLCLK: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int i = cell_at(pt);
        if (i >= 0) { tool_set(TOOL_NONE); view_goto_page(i, 0); }
        return 0;
    }
    case WM_RBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int i = cell_at(pt), cmd;
        HMENU m;
        if (i < 0) return 0;
        if (g.org_sel && !g.org_sel[i]) select_click(i, FALSE, FALSE);
        InvalidateRect(hwnd, NULL, FALSE);
        m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, CMD_ORG_ROTL, L"Rotate &Counterclockwise");
        AppendMenuW(m, MF_STRING, CMD_ORG_ROTR, L"Rotate C&lockwise");
        AppendMenuW(m, MF_STRING, CMD_ORG_DELETE, L"&Delete Pages\tDel");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, CMD_ORG_BLANK, L"Insert &Blank Page");
        AppendMenuW(m, MF_STRING, CMD_ORG_INSERT, L"&Insert from File...");
        AppendMenuW(m, MF_STRING, CMD_ORG_EXTRACT, L"E&xtract Pages...");
        ClientToScreen(hwnd, &pt);
        cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, NULL);
        DestroyMenu(m);
        if (cmd) app_command(cmd);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_DELETE) { app_command(CMD_ORG_DELETE); return 0; }
        if (wp == 'A' && GetKeyState(VK_CONTROL) < 0) { app_command(CMD_ORG_SELALL); return 0; }
        if (wp == VK_ESCAPE) { tool_set(TOOL_NONE); return 0; }
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void org_register(void)
{
    WNDCLASSW wc = { 0 };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = org_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"SgPdfOrganize";
    RegisterClassW(&wc);
}

HWND org_create(HWND parent)
{
    g_org = CreateWindowExW(0, L"SgPdfOrganize", NULL, WS_CHILD | WS_VSCROLL | WS_TABSTOP, 0, 0, 10, 10, parent, NULL, g_inst, NULL);
    return g_org;
}

void org_show(BOOL on)
{
    if (!g_org) return;
    if (on && g.org_sel && g.npages) {
        int k, any = 0;
        for (k = 0; k < g.npages; k++) any |= g.org_sel[k];
        if (!any && g.current >= 0 && g.current < g.npages) g.org_sel[g.current] = TRUE;
        g.org_anchor = g.current;
    }
    ShowWindow(g_org, on ? SW_SHOW : SW_HIDE);
    ShowWindow(g_view, on ? SW_HIDE : SW_SHOW);
    app_layout();
    org_scrollbar();
    SetFocus(on ? g_org : g_view);
}

void org_update(void)
{
    if (!g_org || !IsWindowVisible(g_org)) return;
    org_scrollbar();
    InvalidateRect(g_org, NULL, FALSE);
}

void org_dump(FILE *f)
{
    int i;
    RECT rc;
    if (!g_org || !IsWindowVisible(g_org)) return;
    GetClientRect(g_org, &rc);
    fprintf(f, "organize 1\n");
    for (i = 0; i < g.npages; i++) {
        RECT c;
        cell_rect(i, &c);
        if (c.bottom < 0 || c.top > rc.bottom) continue;
        MapWindowPoints(g_org, NULL, (POINT *)&c, 2);
        fprintf(f, "org %d %ld %ld %ld %ld %d %d\n", i + 1, c.left, c.top, c.right, c.bottom, g.org_sel ? g.org_sel[i] : 0,
                g.pages[i].thumb != NULL && !g.pages[i].tstale);
    }
}
