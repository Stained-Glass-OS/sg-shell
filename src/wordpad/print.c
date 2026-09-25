/* sg-wordpad -- printing, print preview and page setup.
 *
 * Pages are laid out by RichEdit itself with EM_FORMATRANGE (wine-sg 0184
 * implements it: Wine's RichEdit had it as a stub), a page at a time, from
 * the character where the last page stopped. The margins and the paper
 * are Page Setup's (kept in WordPad's Options key).
 *
 * SG_WORDPAD_PRINT_EMF=<directory> makes Print and /p write each page as
 * page<N>.emf there instead of to a printer, for the gate.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "wordpad.h"

static PAGESETUPDLGW g_psd;
static HGLOBAL g_devmode, g_devnames;

static void no_printer(void)
{
    MessageBoxW(g_main, L"Before you can perform printer-related tasks such as page setup or printing a document, "
                        L"you need to install a printer.", L"WordPad", MB_OK | MB_ICONWARNING);
}

/* one page from cp into hdc; returns the next page's first character */
static LONG format_page(HDC hdc, HDC target, LONG cp, LONG end, BOOL render)
{
    FORMATRANGE fr;
    int dpix = GetDeviceCaps(hdc, LOGPIXELSX), dpiy = GetDeviceCaps(hdc, LOGPIXELSY);
    int offx = MulDiv(GetDeviceCaps(hdc, PHYSICALOFFSETX), 1440, dpix);
    int offy = MulDiv(GetDeviceCaps(hdc, PHYSICALOFFSETY), 1440, dpiy);
    memset(&fr, 0, sizeof(fr));
    fr.hdc = hdc;
    fr.hdcTarget = target;
    SetRect(&fr.rcPage, 0, 0, g_pagew, g_pageh);
    SetRect(&fr.rc, g_margins.left - offx, g_margins.top - offy, g_pagew - g_margins.right - offx, g_pageh - g_margins.bottom - offy);
    fr.chrg.cpMin = cp;
    fr.chrg.cpMax = end;
    return (LONG)SendMessageW(g_edit, EM_FORMATRANGE, render, (LPARAM)&fr);
}

static LONG text_end(void)
{
    GETTEXTLENGTHEX tl = { GTL_NUMCHARS | GTL_PRECISE, 1200 };
    return (LONG)SendMessageW(g_edit, EM_GETTEXTLENGTHEX, (WPARAM)&tl, 0);
}

/* the first character of every page, measured on target */
static int paginate(HDC target, LONG **starts)
{
    LONG end = text_end(), cp = 0;
    int n = 0, cap = 16;
    *starts = malloc(cap * sizeof(LONG));
    do
    {
        LONG next;
        if (n == cap) { cap *= 2; *starts = realloc(*starts, cap * sizeof(LONG)); }
        (*starts)[n++] = cp;
        next = format_page(target, target, cp, -1, FALSE);
        if (next <= cp) break;              /* nothing fitted: stop rather than loop */
        cp = next;
    } while (cp < end && n < 10000);
    SendMessageW(g_edit, EM_FORMATRANGE, FALSE, 0);
    return n;
}

static void print_to_emf(const WCHAR *dir)
{
    HDC ref = GetDC(NULL), dc;
    RECT r = { 0, 0, MulDiv(g_pagew, 2540, 1440), MulDiv(g_pageh, 2540, 1440) };
    LONG *starts;
    int n;
    WCHAR path[MAX_PATH];
    HDC measure = CreateCompatibleDC(ref);
    n = paginate(measure, &starts);
    for (int i = 0; i < n; i++)
    {
        swprintf(path, MAX_PATH, L"%ls\\page%d.emf", dir, i + 1);
        dc = CreateEnhMetaFileW(ref, path, &r, L"WordPad\0Page\0");
        format_page(dc, measure, starts[i], -1, TRUE);
        DeleteEnhMetaFile(CloseEnhMetaFile(dc));
    }
    SendMessageW(g_edit, EM_FORMATRANGE, FALSE, 0);
    swprintf(path, MAX_PATH, L"%ls\\pages.txt", dir);
    {
        FILE *f = _wfopen(path, L"w");
        if (f) { fprintf(f, "%d\n", n); for (int i = 0; i < n; i++) fprintf(f, "%ld\n", starts[i]); fclose(f); }
    }
    free(starts);
    DeleteDC(measure);
    ReleaseDC(NULL, ref);
}

static void print_dc(HDC dc, int from, int to)
{
    DOCINFOW di;
    WCHAR title[MAX_PATH];
    LONG *starts;
    int n;
    const WCHAR *p = wcsrchr(g_path, '\\');
    lstrcpynW(title, g_path[0] ? (p ? p + 1 : g_path) : L"Document", MAX_PATH);
    n = paginate(dc, &starts);
    memset(&di, 0, sizeof(di));
    di.cbSize = sizeof(di);
    di.lpszDocName = title;
    if (StartDocW(dc, &di) > 0)
    {
        for (int i = max(from, 1) - 1; i < n && i < to; i++)
        {
            StartPage(dc);
            format_page(dc, dc, starts[i], -1, TRUE);
            EndPage(dc);
        }
        EndDoc(dc);
    }
    SendMessageW(g_edit, EM_FORMATRANGE, FALSE, 0);
    free(starts);
}

void print_document(BOOL dialog)
{
    PRINTDLGW pd;
    WCHAR dir[MAX_PATH];
    if (GetEnvironmentVariableW(L"SG_WORDPAD_PRINT_EMF", dir, MAX_PATH)) { print_to_emf(dir); return; }
    memset(&pd, 0, sizeof(pd));
    pd.lStructSize = sizeof(pd);
    pd.hwndOwner = g_main;
    pd.hDevMode = g_devmode;
    pd.hDevNames = g_devnames;
    pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE | (dialog ? 0 : PD_RETURNDEFAULT);
    pd.nFromPage = pd.nMinPage = 1;
    pd.nToPage = pd.nMaxPage = 9999;
    if (!PrintDlgW(&pd))
    {
        if (CommDlgExtendedError() == PDERR_NODEFAULTPRN || CommDlgExtendedError() == PDERR_NODEVICES) no_printer();
        return;
    }
    g_devmode = pd.hDevMode; g_devnames = pd.hDevNames;
    print_dc(pd.hDC, (pd.Flags & PD_PAGENUMS) ? pd.nFromPage : 1, (pd.Flags & PD_PAGENUMS) ? pd.nToPage : 9999);
    DeleteDC(pd.hDC);
}

void print_file_silently(const WCHAR *printer)
{
    WCHAR dir[MAX_PATH];
    HDC dc = NULL;
    if (GetEnvironmentVariableW(L"SG_WORDPAD_PRINT_EMF", dir, MAX_PATH)) { print_to_emf(dir); return; }
    if (printer) dc = CreateDCW(NULL, printer, NULL, NULL);
    else
    {
        PRINTDLGW pd;
        memset(&pd, 0, sizeof(pd));
        pd.lStructSize = sizeof(pd);
        pd.Flags = PD_RETURNDC | PD_RETURNDEFAULT;
        if (PrintDlgW(&pd)) dc = pd.hDC;
    }
    if (!dc) return;
    print_dc(dc, 1, 9999);
    DeleteDC(dc);
}

void page_setup(void)
{
    memset(&g_psd, 0, sizeof(g_psd));
    g_psd.lStructSize = sizeof(g_psd);
    g_psd.hwndOwner = g_main;
    g_psd.hDevMode = g_devmode;
    g_psd.hDevNames = g_devnames;
    g_psd.Flags = PSD_MARGINS | (g_units == 1 ? PSD_INHUNDREDTHSOFMILLIMETERS : PSD_INTHOUSANDTHSOFINCHES);
    if (g_units == 1)
        SetRect(&g_psd.rtMargin, MulDiv(g_margins.left, 2540, 1440), MulDiv(g_margins.top, 2540, 1440),
                MulDiv(g_margins.right, 2540, 1440), MulDiv(g_margins.bottom, 2540, 1440));
    else
        SetRect(&g_psd.rtMargin, MulDiv(g_margins.left, 1000, 1440), MulDiv(g_margins.top, 1000, 1440),
                MulDiv(g_margins.right, 1000, 1440), MulDiv(g_margins.bottom, 1000, 1440));
    if (!PageSetupDlgW(&g_psd))
    {
        if (CommDlgExtendedError() == PDERR_NODEFAULTPRN || CommDlgExtendedError() == PDERR_NODEVICES) no_printer();
        return;
    }
    g_devmode = g_psd.hDevMode; g_devnames = g_psd.hDevNames;
    if (g_psd.Flags & PSD_INHUNDREDTHSOFMILLIMETERS)
    {
        SetRect(&g_margins, MulDiv(g_psd.rtMargin.left, 1440, 2540), MulDiv(g_psd.rtMargin.top, 1440, 2540),
                MulDiv(g_psd.rtMargin.right, 1440, 2540), MulDiv(g_psd.rtMargin.bottom, 1440, 2540));
        g_pagew = MulDiv(g_psd.ptPaperSize.x, 1440, 2540); g_pageh = MulDiv(g_psd.ptPaperSize.y, 1440, 2540);
    }
    else
    {
        SetRect(&g_margins, MulDiv(g_psd.rtMargin.left, 1440, 1000), MulDiv(g_psd.rtMargin.top, 1440, 1000),
                MulDiv(g_psd.rtMargin.right, 1440, 1000), MulDiv(g_psd.rtMargin.bottom, 1440, 1000));
        g_pagew = MulDiv(g_psd.ptPaperSize.x, 1440, 1000); g_pageh = MulDiv(g_psd.ptPaperSize.y, 1440, 1000);
    }
    if (g_pagew < 1440) g_pagew = 12240;
    if (g_pageh < 1440) g_pageh = 15840;
    write_dump();
}

/* ---------------------------------------------------------------- print preview */

static HWND g_prev;
static LONG *g_starts;
static int g_npages, g_page, g_two, g_pzoom = 100;
static HDC g_measure;
enum { PB_PRINT = 1, PB_NEXT, PB_PREV, PB_TWO, PB_ZIN, PB_ZOUT, PB_CLOSE, PB_COUNT };
static const WCHAR *PB_NAMES[PB_COUNT] = { NULL, L"Print", L"Next page", L"Previous page", L"Two pages", L"Zoom in",
                                           L"Zoom out", L"Close print preview" };
static RECT g_pb[PB_COUNT];

static void preview_layout(void)
{
    HDC dc = GetDC(g_prev);
    HGDIOBJ o = SelectObject(dc, g_font);
    int x = S(8);
    for (int i = 1; i < PB_COUNT; i++)
    {
        SIZE sz;
        const WCHAR *t = i == PB_TWO && g_two ? L"One page" : PB_NAMES[i];
        GetTextExtentPoint32W(dc, t, lstrlenW(t), &sz);
        SetRect(&g_pb[i], x, S(6), x + sz.cx + S(20), S(34));
        x = g_pb[i].right + S(6);
    }
    SelectObject(dc, o);
    ReleaseDC(g_prev, dc);
}

static void draw_page(HDC dc, int idx, RECT box)
{
    /* the page at 96 dpi in a bitmap, then scaled into the box */
    int w = MulDiv(g_pagew, 96, 1440), h = MulDiv(g_pageh, 96, 1440);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);
    RECT r = { 0, 0, w, h };
    FillRect(mem, &r, GetStockObject(WHITE_BRUSH));
    format_page(mem, g_measure, g_starts[idx], -1, TRUE);
    SetStretchBltMode(dc, HALFTONE);
    StretchBlt(dc, box.left, box.top, box.right - box.left, box.bottom - box.top, mem, 0, 0, w, h, SRCCOPY);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
    FrameRect(dc, &box, GetStockObject(GRAY_BRUSH));
}

static void preview_paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps), mem;
    RECT rc, bar;
    HBITMAP bmp;
    HGDIOBJ ob, of;
    HBRUSH b;
    int n = g_two && g_page + 1 < g_npages ? 2 : 1, avail_h, ph, pw, x0;
    GetClientRect(h, &rc);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    ob = SelectObject(mem, bmp);
    b = CreateSolidBrush(WORKSPACE); FillRect(mem, &rc, b); DeleteObject(b);
    bar = rc; bar.bottom = S(40);
    b = CreateSolidBrush(RIBBON_BG); FillRect(mem, &bar, b); DeleteObject(b);
    of = SelectObject(mem, g_font);
    SetBkMode(mem, TRANSPARENT);
    for (int i = 1; i < PB_COUNT; i++)
    {
        BOOL disabled = (i == PB_NEXT && g_page + n >= g_npages) || (i == PB_PREV && g_page == 0);
        b = CreateSolidBrush(ACCENT_HOT); FillRect(mem, &g_pb[i], b); DeleteObject(b);
        b = CreateSolidBrush(ACCENT_EDGE); FrameRect(mem, &g_pb[i], b); DeleteObject(b);
        SetTextColor(mem, disabled ? RGB(160, 160, 170) : RGB(30, 30, 38));
        DrawTextW(mem, i == PB_TWO && g_two ? L"One page" : PB_NAMES[i], -1, &g_pb[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    {
        WCHAR t[64];
        RECT tr = rc; tr.top = S(6); tr.bottom = S(34); tr.right -= S(10);
        swprintf(t, 64, L"Page %d of %d", g_page + 1, g_npages);
        SetTextColor(mem, RGB(30, 30, 38));
        DrawTextW(mem, t, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(mem, of);
    avail_h = rc.bottom - S(40) - S(24);
    ph = MulDiv(avail_h, g_pzoom, 100);
    pw = MulDiv(ph, g_pagew, g_pageh);
    x0 = (rc.right - (pw * n + S(20) * (n - 1))) / 2;
    for (int i = 0; i < n; i++)
    {
        RECT box = { x0 + i * (pw + S(20)), S(52), x0 + i * (pw + S(20)) + pw, S(52) + ph };
        draw_page(mem, g_page + i, box);
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, ob); DeleteObject(bmp); DeleteDC(mem);
    EndPaint(h, &ps);
}

static void preview_cmd(int i)
{
    int n = g_two ? 2 : 1;
    switch (i)
    {
    case PB_PRINT: EnableWindow(g_main, TRUE); DestroyWindow(g_prev); print_document(TRUE); return;
    case PB_NEXT: if (g_page + n < g_npages) g_page += n; break;
    case PB_PREV: g_page = max(0, g_page - n); break;
    case PB_TWO: g_two = !g_two; preview_layout(); break;
    case PB_ZIN: g_pzoom = min(400, g_pzoom + 25); break;
    case PB_ZOUT: g_pzoom = max(25, g_pzoom - 25); break;
    case PB_CLOSE: EnableWindow(g_main, TRUE); DestroyWindow(g_prev); return;
    }
    InvalidateRect(g_prev, NULL, FALSE);
    write_dump();
}

static LRESULT CALLBACK preview_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m)
    {
    case WM_PAINT: preview_paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        for (int i = 1; i < PB_COUNT; i++) if (PtInRect(&g_pb[i], pt)) { preview_cmd(i); return 0; }
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) preview_cmd(PB_CLOSE);
        else if (w == VK_NEXT || w == VK_RIGHT) preview_cmd(PB_NEXT);
        else if (w == VK_PRIOR || w == VK_LEFT) preview_cmd(PB_PREV);
        return 0;
    case WM_DESTROY:
        free(g_starts); g_starts = NULL;
        SendMessageW(g_edit, EM_FORMATRANGE, FALSE, 0);
        if (g_measure) { DeleteDC(g_measure); g_measure = NULL; }
        g_prev = NULL;
        EnableWindow(g_main, TRUE);
        SetForegroundWindow(g_main);
        write_dump();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void preview_dump(FILE *f)
{
    POINT a;
    if (!g_prev) return;
    fprintf(f, "preview %d %d %d\n", g_page + 1, g_npages, g_two);
    for (int i = 1; i < PB_COUNT; i++)
    {
        a.x = (g_pb[i].left + g_pb[i].right) / 2; a.y = (g_pb[i].top + g_pb[i].bottom) / 2;
        ClientToScreen(g_prev, &a);
        fprintf(f, "previewbutton %d %ld %ld %ls\n", i, a.x, a.y, PB_NAMES[i]);
    }
}

void print_preview(void)
{
    static BOOL reg;
    RECT r;
    HDC screen;
    if (!reg)
    {
        WNDCLASSW wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = preview_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"SgWordPadPreview";
        RegisterClassW(&wc);
        reg = TRUE;
    }
    if (g_prev) return;
    screen = GetDC(NULL);
    g_measure = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    g_npages = paginate(g_measure, &g_starts);
    g_page = 0;
    GetWindowRect(g_main, &r);
    g_prev = CreateWindowExW(0, L"SgWordPadPreview", L"Print preview - WordPad", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             r.left, r.top, r.right - r.left, r.bottom - r.top, g_main, NULL, g_inst, NULL);
    preview_layout();
    EnableWindow(g_main, FALSE);
    write_dump();
}
