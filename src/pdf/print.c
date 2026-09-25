/* sg-pdf -- PDF Viewer: printing.
 *
 * The Print dialog (printer, pages, copies), then each page rendered by
 * poppler at the printer's resolution (at most 300 dpi -- beyond that the
 * bitmap grows and nothing gets sharper on paper), turned to suit the paper
 * (a landscape page on portrait paper is turned a quarter), fitted into the
 * printable area and centred. SG_PDF_PRINT_TO=<file> prints to that file on
 * the default printer without asking (the gate's way in).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"
#include <commdlg.h>
#include <cderr.h>
#include <winspool.h>

static BOOL print_page(HDC dc, int page)
{
    page_t *p = &g.pages[page];
    int hres = GetDeviceCaps(dc, HORZRES), vres = GetDeviceCaps(dc, VERTRES);
    int dpix = GetDeviceCaps(dc, LOGPIXELSX), dpiy = GetDeviceCaps(dc, LOGPIXELSY);
    double pw = p->w, ph = p->h, fit, scale, rdpi;
    int rot = 0, w, h, dw, dh, x, y;
    char line[96], head[256], num[32];
    BYTE *data;
    DWORD len;
    BITMAPINFO bi = { 0 };
    BOOL ok = FALSE;
    if (hres <= 0 || vres <= 0 || dpix <= 0 || dpiy <= 0) return FALSE;
    /* turn a page whose shape is the paper's other way round */
    if ((pw > ph) != (hres > vres) && fabs(pw - ph) > 1) { rot = 90; pw = p->h; ph = p->w; }
    fit = min(hres / (pw * dpix / 72.0), vres / (ph * dpiy / 72.0));
    dw = (int)(pw * dpix / 72.0 * fit);
    dh = (int)(ph * dpiy / 72.0 * fit);
    rdpi = min((double)dpix, 300.0) * min(fit, 1.0);
    scale = rdpi / 72.0;
    snprintf(line, sizeof(line), "render\t%d\t%.5f\t%d", page, scale, rot);
    if (br_request(line, head, sizeof(head), &data, &len) != 1) return FALSE;
    w = br_field(head, "w", num, sizeof(num)) ? atoi(num) : 0;
    h = br_field(head, "h", num, sizeof(num)) ? atoi(num) : 0;
    if (w > 0 && h > 0 && len == (DWORD)w * h * 4) {
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        x = (hres - dw) / 2;
        y = (vres - dh) / 2;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        ok = StretchDIBits(dc, x, y, dw, dh, 0, 0, w, h, data, &bi, DIB_RGB_COLORS, SRCCOPY) != (int)GDI_ERROR;
    }
    free(data);
    return ok;
}

static HDC default_printer_dc(void)
{
    WCHAR name[256];
    DWORD n = 256;
    if (!GetDefaultPrinterW(name, &n)) return NULL;
    return CreateDCW(NULL, name, NULL, NULL);
}

void print_document(void)
{
    PRINTDLGW pd = { sizeof(pd) };
    DOCINFOW di = { sizeof(di) };
    WCHAR to[MAX_PATH] = L"";
    HDC dc;
    int from = 1, until = g.npages, copies = 1, c, i, done = 0;
    HCURSOR old;
    if (!g.npages || !g.bridged) return;
    if (GetEnvironmentVariableW(L"SG_PDF_PRINT_TO", to, MAX_PATH) && to[0]) {
        dc = default_printer_dc();
        di.lpszOutput = to;
    } else {
        pd.hwndOwner = g_main;
        pd.Flags = PD_RETURNDC | PD_ALLPAGES | PD_NOSELECTION | PD_USEDEVMODECOPIESANDCOLLATE;
        pd.nMinPage = 1;
        pd.nMaxPage = (WORD)min(g.npages, 0xFFFF);
        pd.nFromPage = 1;
        pd.nToPage = pd.nMaxPage;
        if (!PrintDlgW(&pd)) {
            if (CommDlgExtendedError() == PDERR_NODEFAULTPRN)
                MessageBoxW(g_main, L"No printers are installed. Add one in Settings > Devices > Printers & scanners.",
                            APP_NAME, MB_OK | MB_ICONINFORMATION);
            return;
        }
        dc = pd.hDC;
        if (pd.Flags & PD_PAGENUMS) { from = pd.nFromPage; until = pd.nToPage; }
        copies = max(1, pd.nCopies);
    }
    if (!dc) {
        MessageBoxW(g_main, L"The printer could not be opened.", APP_NAME, MB_OK | MB_ICONWARNING);
        return;
    }
    di.lpszDocName = g.name;
    old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    if (StartDocW(dc, &di) > 0) {
        for (c = 0; c < copies; c++)
            for (i = max(from, 1); i <= min(until, g.npages); i++) {
                if (StartPage(dc) <= 0) break;
                if (print_page(dc, i - 1)) done++;
                EndPage(dc);
            }
        EndDoc(dc);
    }
    SetCursor(old);
    DeleteDC(dc);
    if (pd.hDevMode) GlobalFree(pd.hDevMode);
    if (pd.hDevNames) GlobalFree(pd.hDevNames);
    {
        /* for the gate */
        extern int g_printed;
        g_printed = done;
    }
    app_status_changed();
}
