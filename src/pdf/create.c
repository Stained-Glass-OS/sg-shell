/* sg-pdf -- SG PDF: Create PDF, Recognize Text, the page decorations (header
 * and footer, watermark, Bates numbers, page numbers), Reduce File Size,
 * links, attachments and Read Out Loud.
 *
 *   Create PDF        a blank page; from files (pictures, text, other PDFs,
 *                     office documents -- converted by SG Office's engine or
 *                     LibreOffice); from the scanner (SANE). The new document
 *                     is untitled until it is saved.
 *   Recognize Text    OCR (OCRmyPDF and Tesseract) makes scanned pages
 *                     searchable and selectable; the languages are those
 *                     installed (tesseract-ocr-<lang> packages).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"
#include "resource.h"
#include <shellapi.h>

const WCHAR *const STAMP_LABELS[NSTAMPS] = {
    L"Approved", L"As Is", L"Confidential", L"Departmental", L"Experimental", L"Expired", L"Final", L"For Comment",
    L"For Public Release", L"Not Approved", L"Not For Public Release", L"Sold", L"Top Secret", L"Draft" };
const char *const STAMP_NAMES[NSTAMPS] = {
    "Approved", "AsIs", "Confidential", "Departmental", "Experimental", "Expired", "Final", "ForComment",
    "ForPublicRelease", "NotApproved", "NotForPublicRelease", "Sold", "TopSecret", "Draft" };

static BOOL quiet(void)
{
    return GetEnvironmentVariableW(L"SG_PDF_QUIET", NULL, 0) != 0;
}

static char *unix_esc(const WCHAR *path)
{
    char *u = unix_path(path), *e = NULL;
    WCHAR *wu = u ? from_utf8(u, -1) : NULL;
    if (wu) e = esc_utf8(wu);
    free(wu);
    free(u);
    return e;
}

static char *dlg_text_esc(HWND dlg, int id)
{
    WCHAR t[1024];
    GetDlgItemTextW(dlg, id, t, 1024);
    return esc_utf8(t);
}

static void fail_box(const char *head)
{
    WCHAR *m = from_utf8(head, -1), msg[400];
    const WCHAR *why = m ? m : L"";
    if (!strncmp(head, "ERR ", 4)) {
        const char *sp = strchr(head + 4, ' ');
        free(m);
        m = from_utf8(sp ? sp + 1 : head + 4, -1);
        why = m ? m : L"";
    }
    swprintf(msg, 400, L"%ls", why[0] ? why : L"It could not be done.");
    msg[0] = towupper(msg[0]);
    app_set_status(L"%ls", msg);
    if (!quiet()) MessageBoxW(g_main, msg, APP_NAME, MB_OK | MB_ICONWARNING);
    free(m);
}

/* a request whose answer is a new, untitled document */
static BOOL new_document(const char *line, const WCHAR *name)
{
    char head[1024];
    BYTE *data = NULL;
    DWORD len = 0;
    int rc;
    HCURSOR old;
    if (!tab_new()) return FALSE;          /* the new document in a tab (and an engine) of its own */
    old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    rc = br_request(line, head, sizeof(head), &data, &len);
    SetCursor(old);
    if (rc != 1) { free(data); tab_discard_new(); fail_box(head); return FALSE; }
    app_adopt(head, data, len, name);
    free(data);
    return TRUE;
}

/* ---- Create PDF -------------------------------------------------------------------------------------------- */

static void create_blank(void)
{
    new_document("new\t612\t792\t1", L"Untitled.pdf");
}

static void create_files(void)
{
    WCHAR **files = NULL, name[MAX_PATH], *base, *dot;
    int n = 0, i, len = 0, cap;
    char *line;
    if (!file_dialog_multi(L"Create PDF from Files",
                           L"Files that can be made into a PDF\0*.pdf;*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff;*.webp;*.txt;"
                           L"*.doc;*.docx;*.odt;*.rtf;*.xls;*.xlsx;*.ods;*.ppt;*.pptx;*.odp;*.htm;*.html\0All files (*.*)\0*.*\0",
                           &files, &n) || !n)
        return;
    cap = 64;
    for (i = 0; i < n; i++) cap += (int)wcslen(files[i]) * 6 + 16;
    line = malloc(cap);
    if (line) {
        len = snprintf(line, cap, "create");
#ifdef SG_MUTANT_CREATE
        n = 1;
#endif
        for (i = 0; i < n; i++) {
            char *e = unix_esc(files[i]);
            if (e) len += snprintf(line + len, cap - len, "\t%s", e);
            free(e);
        }
        base = wcsrchr(files[0], L'\\');
        lstrcpynW(name, base ? base + 1 : files[0], MAX_PATH - 5);
        dot = wcsrchr(name, L'.');
        if (dot) *dot = 0;
        lstrcatW(name, L".pdf");
        if (new_document(line, name) && n > 1) app_set_status(L"%d files made into one PDF.", n);
        free(line);
    }
    for (i = 0; i < n; i++) free(files[i]);
    free(files);
}

/* ---- the scanner ----------------------------------------------------------------------------------------------- */

static BOOL g_scan_into;      /* Organize: scan pages into the open document */

static void scan_list(HWND dlg)
{
    char head[256];
    BYTE *data = NULL;
    DWORD len;
    HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    HWND cb = GetDlgItem(dlg, IDC_SC_DEVICE);
    int i;
    for (i = (int)SendMessageW(cb, CB_GETCOUNT, 0, 0) - 1; i >= 0; i--) {
        free((void *)SendMessageW(cb, CB_GETITEMDATA, i, 0));
        SendMessageW(cb, CB_DELETESTRING, i, 0);
    }
    if (br_request("scanners", head, sizeof(head), &data, &len) == 1 && data) {
        char *s = (char *)data, *e;
        for (; s && *s; s = e) {
            char *tab;
            e = strchr(s, '\n');
            if (e) *e++ = 0;
            if (!(tab = strchr(s, '\t'))) continue;
            *tab = 0;
            {
                WCHAR *dev = unesc_utf8(s, -1), *label = unesc_utf8(tab + 1, -1);
                int k = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)(label && label[0] ? label : dev ? dev : L"?"));
                SendMessageW(cb, CB_SETITEMDATA, k, (LPARAM)dev);
                free(label);
            }
        }
    }
    free(data);
    SetCursor(old);
    if (!SendMessageW(cb, CB_GETCOUNT, 0, 0)) {
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"(no scanner found)");
        EnableWindow(GetDlgItem(dlg, IDOK), FALSE);
    } else EnableWindow(GetDlgItem(dlg, IDOK), TRUE);
    SendMessageW(cb, CB_SETCURSEL, 0, 0);
}

static INT_PTR CALLBACK scan_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static const int DPI[] = { 150, 200, 300, 400, 600 };
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        int i;
        for (i = 0; i < 5; i++) { WCHAR t[16]; swprintf(t, 16, L"%d dpi", DPI[i]); SendDlgItemMessageW(dlg, IDC_SC_DPI, CB_ADDSTRING, 0, (LPARAM)t); }
        SendDlgItemMessageW(dlg, IDC_SC_DPI, CB_SETCURSEL, 2, 0);
        SendDlgItemMessageW(dlg, IDC_SC_MODE, CB_ADDSTRING, 0, (LPARAM)L"Colour");
        SendDlgItemMessageW(dlg, IDC_SC_MODE, CB_ADDSTRING, 0, (LPARAM)L"Grey");
        SendDlgItemMessageW(dlg, IDC_SC_MODE, CB_ADDSTRING, 0, (LPARAM)L"Black and white");
        SendDlgItemMessageW(dlg, IDC_SC_MODE, CB_SETCURSEL, 0, 0);
        SendDlgItemMessageW(dlg, IDC_SC_SOURCE, CB_ADDSTRING, 0, (LPARAM)L"Glass (flatbed)");
        SendDlgItemMessageW(dlg, IDC_SC_SOURCE, CB_ADDSTRING, 0, (LPARAM)L"Feeder (ADF)");
        SendDlgItemMessageW(dlg, IDC_SC_SOURCE, CB_SETCURSEL, 0, 0);
        SetDlgItemTextW(dlg, IDC_SC_PAGES, L"1");
        CheckDlgButton(dlg, IDC_SC_OCR, BST_CHECKED);
        scan_list(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_SC_REFRESH) { scan_list(dlg); return TRUE; }
        if (LOWORD(wp) == IDOK) {
            HWND cb = GetDlgItem(dlg, IDC_SC_DEVICE);
            int k = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0), mode = (int)SendDlgItemMessageW(dlg, IDC_SC_MODE, CB_GETCURSEL, 0, 0);
            const WCHAR *dev = k >= 0 ? (const WCHAR *)SendMessageW(cb, CB_GETITEMDATA, k, 0) : NULL;
            char *edev = dev && (LRESULT)dev != CB_ERR ? esc_utf8(dev) : NULL, line[1024];
            int dpi = DPI[max(0, (int)SendDlgItemMessageW(dlg, IDC_SC_DPI, CB_GETCURSEL, 0, 0))];
            int pages = max(1, min(200, (int)GetDlgItemInt(dlg, IDC_SC_PAGES, NULL, FALSE)));
            BOOL adf = SendDlgItemMessageW(dlg, IDC_SC_SOURCE, CB_GETCURSEL, 0, 0) == 1;
            snprintf(line, sizeof(line), "scan\tdevice=%s\tdpi=%d\tmode=%s\tpages=%d%s%s%s", edev ? edev : "", dpi,
                     mode == 1 ? "Gray" : mode == 2 ? "Lineart" : "Color", pages, adf ? "\tsource=ADF" : "",
                     IsDlgButtonChecked(dlg, IDC_SC_OCR) == BST_CHECKED ? "\tocr=eng" : "", "");
            if (g_scan_into && g.npages) {
                size_t n = strlen(line);
                snprintf(line + n, sizeof(line) - n, "\tat=%d", g.current + 1);
            }
            free(edev);
            EndDialog(dlg, IDOK);
            if (g_scan_into && g.npages) {
                if (doc_request(line)) app_set_status(L"The scanned pages were inserted.");
            } else new_document(line, L"Scan.pdf");
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    case WM_DESTROY: {
        HWND cb = GetDlgItem(dlg, IDC_SC_DEVICE);
        int i;
        for (i = (int)SendMessageW(cb, CB_GETCOUNT, 0, 0) - 1; i >= 0; i--) {
            LRESULT d = SendMessageW(cb, CB_GETITEMDATA, i, 0);
            if (d && d != CB_ERR) free((void *)d);
        }
        break;
    }
    }
    return FALSE;
}

static void create_scan(BOOL into)
{
    g_scan_into = into;

    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SCAN), g_main, scan_proc, 0);
}

/* ---- Recognize Text --------------------------------------------------------------------------------------------- */

static const struct { const char *code; const WCHAR *name; } LANGS[] = {
    { "eng", L"English" }, { "deu", L"German" }, { "fra", L"French" }, { "spa", L"Spanish" }, { "ita", L"Italian" },
    { "por", L"Portuguese" }, { "nld", L"Dutch" }, { "pol", L"Polish" }, { "rus", L"Russian" }, { "ukr", L"Ukrainian" },
    { "jpn", L"Japanese" }, { "chi_sim", L"Chinese (Simplified)" }, { "chi_tra", L"Chinese (Traditional)" },
    { "kor", L"Korean" }, { "ara", L"Arabic" }, { "heb", L"Hebrew" }, { "hin", L"Hindi" }, { "tur", L"Turkish" },
    { "swe", L"Swedish" }, { "dan", L"Danish" }, { "nor", L"Norwegian" }, { "fin", L"Finnish" }, { "ces", L"Czech" },
    { "ell", L"Greek" }, { "vie", L"Vietnamese" },
};

static INT_PTR CALLBACK ocr_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        char head[256];
        BYTE *data = NULL;
        DWORD len;
        int n = 0;
        if (br_request("ocrlangs", head, sizeof(head), &data, &len) == 1 && data) {
            char *s = (char *)data, *e;
            for (; s && *s; s = e) {
                WCHAR t[128];
                const WCHAR *name = NULL;
                size_t k;
                e = strchr(s, '\n');
                if (e) *e++ = 0;
                for (k = 0; k < sizeof(LANGS) / sizeof(LANGS[0]); k++) if (!strcmp(LANGS[k].code, s)) name = LANGS[k].name;
                swprintf(t, 128, L"%ls (%hs)", name ? name : L"", s);
                if (!name) swprintf(t, 128, L"%hs", s);
                k = SendDlgItemMessageW(dlg, IDC_OC_LANG, CB_ADDSTRING, 0, (LPARAM)t);
                SendDlgItemMessageW(dlg, IDC_OC_LANG, CB_SETITEMDATA, k, (LPARAM)_strdup(s));
                if (!strcmp(s, "eng")) SendDlgItemMessageW(dlg, IDC_OC_LANG, CB_SETCURSEL, k, 0);
                n++;
            }
        }
        free(data);
        if (SendDlgItemMessageW(dlg, IDC_OC_LANG, CB_GETCURSEL, 0, 0) < 0) SendDlgItemMessageW(dlg, IDC_OC_LANG, CB_SETCURSEL, 0, 0);
        CheckDlgButton(dlg, IDC_OC_ALL, BST_CHECKED);
        SetDlgItemTextW(dlg, IDC_OC_NOTE, n ? L"Scanned pages get an invisible layer of the words recognized on them, so they "
                                             L"can be searched, selected and copied. Pages that have text already are left as they are."
                                           : L"Text recognition is not installed (packages ocrmypdf and tesseract-ocr).");
        EnableWindow(GetDlgItem(dlg, IDOK), n > 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int k = (int)SendDlgItemMessageW(dlg, IDC_OC_LANG, CB_GETCURSEL, 0, 0);
            const char *lang = k >= 0 ? (const char *)SendDlgItemMessageW(dlg, IDC_OC_LANG, CB_GETITEMDATA, k, 0) : "eng";
            char line[256];
            snprintf(line, sizeof(line), "ocr\tlang=%s%s%s", lang && (LRESULT)lang != CB_ERR ? lang : "eng",
                     IsDlgButtonChecked(dlg, IDC_OC_DESKEW) == BST_CHECKED ? "\tdeskew=1" : "", "");
            if (IsDlgButtonChecked(dlg, IDC_OC_CURRENT) == BST_CHECKED) {
                size_t n = strlen(line);
                snprintf(line + n, sizeof(line) - n, "\tpages=%d", g.current);
            }
            EndDialog(dlg, IDOK);
            app_set_status(L"Recognizing text...");
            UpdateWindow(g_main);
            if (doc_request(line)) {
                if (!quiet()) MessageBoxW(g_main, L"Text was recognized. The pages can be searched and their text selected now.",
                                          L"Recognize Text", MB_OK | MB_ICONINFORMATION);
                app_set_status(L"Text recognized.");
            }
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    case WM_DESTROY: {
        int i;
        for (i = (int)SendDlgItemMessageW(dlg, IDC_OC_LANG, CB_GETCOUNT, 0, 0) - 1; i >= 0; i--) {
            LRESULT d = SendDlgItemMessageW(dlg, IDC_OC_LANG, CB_GETITEMDATA, i, 0);
            if (d && d != CB_ERR) free((void *)d);
        }
        break;
    }
    }
    return FALSE;
}

/* ---- header and footer, watermark, Bates numbers, page numbers --------------------------------------------- */

static void pages_spec(HWND dlg, int id, char *out, int cap)
{
    /* "1-3, 5" (1-based, as people count) -> "0-2,4" */
    WCHAR t[256], *s, *ctx = NULL;
    int k = 0;
    GetDlgItemTextW(dlg, id, t, 256);
    out[0] = 0;
    for (s = wcstok_s(t, L", ", &ctx); s && k < cap - 24; s = wcstok_s(NULL, L", ", &ctx)) {
        int a = 0, b = 0;
        if (swscanf(s, L"%d-%d", &a, &b) == 2) k += snprintf(out + k, cap - k, "%s%d-%d", k ? "," : "", max(0, a - 1), max(0, b - 1));
        else if (swscanf(s, L"%d", &a) == 1) k += snprintf(out + k, cap - k, "%s%d", k ? "," : "", max(0, a - 1));
    }
}

static INT_PTR CALLBACK headfoot_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dlg, IDC_HF_SIZE, L"10");
        SetDlgItemTextW(dlg, IDC_HF_MARGIN, L"36");
        SetDlgItemTextW(dlg, IDC_HF_FC, L"Page <<page>> of <<pages>>");
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            char *hl = dlg_text_esc(dlg, IDC_HF_HL), *hc = dlg_text_esc(dlg, IDC_HF_HC), *hr = dlg_text_esc(dlg, IDC_HF_HR);
            char *fl = dlg_text_esc(dlg, IDC_HF_FL), *fc = dlg_text_esc(dlg, IDC_HF_FC), *fr = dlg_text_esc(dlg, IDC_HF_FR);
            char pages[256], sz[16], mg[16];
            WCHAR t[16];
            BOOL ok = TRUE, head = hl && hc && hr && (hl[0] || hc[0] || hr[0]), foot = fl && fc && fr && (fl[0] || fc[0] || fr[0]);
            pages_spec(dlg, IDC_HF_PAGES, pages, sizeof(pages));
            GetDlgItemTextW(dlg, IDC_HF_SIZE, t, 16); snprintf(sz, sizeof(sz), "%g", max(4.0, min(72.0, _wtof(t) > 0 ? _wtof(t) : 10)));
            GetDlgItemTextW(dlg, IDC_HF_MARGIN, t, 16); snprintf(mg, sizeof(mg), "%g", max(0.0, min(200.0, _wtof(t))));
            EndDialog(dlg, IDOK);
#ifdef SG_MUTANT_DECORATE
            head = foot = FALSE;
#endif
            if (head) ok = doc_requestf("decorate\theader\tleft=%s\tcenter=%s\tright=%s\tsize=%s\tmargin=%s\tpages=%s", hl, hc, hr, sz, mg, pages);
            if (ok && foot) doc_requestf("decorate\tfooter\tleft=%s\tcenter=%s\tright=%s\tsize=%s\tmargin=%s\tpages=%s", fl, fc, fr, sz, mg, pages);
            free(hl); free(hc); free(hr); free(fl); free(fc); free(fr);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static const struct { const WCHAR *name; const char *hex; } WM_COLORS[] = {
    { L"Red", "C01818" }, { L"Grey", "808080" }, { L"Black", "000000" }, { L"Blue", "1E50C8" }, { L"Green", "1E8C3C" } };

static INT_PTR CALLBACK watermark_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    int i;
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        CheckDlgButton(dlg, IDC_WM_USETEXT, BST_CHECKED);
        SetDlgItemTextW(dlg, IDC_WM_TEXT, L"CONFIDENTIAL");
        SetDlgItemTextW(dlg, IDC_WM_SIZE, L"60");
        SetDlgItemTextW(dlg, IDC_WM_ROTATE, L"45");
        SetDlgItemTextW(dlg, IDC_WM_OPACITY, L"30");
        for (i = 0; i < 5; i++) SendDlgItemMessageW(dlg, IDC_WM_COLOR, CB_ADDSTRING, 0, (LPARAM)WM_COLORS[i].name);
        SendDlgItemMessageW(dlg, IDC_WM_COLOR, CB_SETCURSEL, 0, 0);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_WM_BROWSE) {
            WCHAR f[MAX_PATH] = L"";
            if (file_dialog(FALSE, L"Watermark Picture", L"Pictures\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff\0", NULL, f, MAX_PATH)) {
                SetDlgItemTextW(dlg, IDC_WM_IMAGE, f);
                CheckDlgButton(dlg, IDC_WM_USEIMG, BST_CHECKED);
                CheckDlgButton(dlg, IDC_WM_USETEXT, BST_UNCHECKED);
            }
            return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            char *text = dlg_text_esc(dlg, IDC_WM_TEXT), pages[256], *img = NULL, line[4096];
            WCHAR t[MAX_PATH];
            double size, rot, op;
            int c = max(0, (int)SendDlgItemMessageW(dlg, IDC_WM_COLOR, CB_GETCURSEL, 0, 0));
            pages_spec(dlg, IDC_WM_PAGES, pages, sizeof(pages));
            GetDlgItemTextW(dlg, IDC_WM_SIZE, t, 16); size = _wtof(t) > 0 ? _wtof(t) : 60;
            GetDlgItemTextW(dlg, IDC_WM_ROTATE, t, 16); rot = _wtof(t);
            GetDlgItemTextW(dlg, IDC_WM_OPACITY, t, 16); op = max(2, min(100, _wtoi(t) ? _wtoi(t) : 30)) / 100.0;
            if (IsDlgButtonChecked(dlg, IDC_WM_USEIMG) == BST_CHECKED) {
                GetDlgItemTextW(dlg, IDC_WM_IMAGE, t, MAX_PATH);
                img = t[0] ? unix_esc(t) : NULL;
                if (!img) { MessageBoxW(dlg, L"Choose the picture.", L"Add Watermark", MB_OK | MB_ICONWARNING); free(text); return TRUE; }
            }
            snprintf(line, sizeof(line), "decorate\twatermark\t%s%s\tsize=%g\trotate=%g\topacity=%.2f\tcolor=%s\tbehind=%d\tpages=%s",
                     img ? "image=" : "text=", img ? img : text ? text : "DRAFT", size, rot, op, WM_COLORS[c].hex,
                     IsDlgButtonChecked(dlg, IDC_WM_BEHIND) == BST_CHECKED, pages);
            free(text);
            free(img);
            EndDialog(dlg, IDOK);
            doc_request(line);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static BOOL g_bates;           /* the Bates dialog numbers pages (FALSE: page numbers) */

static void bates_sample(HWND dlg)
{
    WCHAR pre[128], suf[128], t[300], fmt[160];
    int start = (int)GetDlgItemInt(dlg, IDC_BT_START, NULL, FALSE), digits = (int)GetDlgItemInt(dlg, IDC_BT_DIGITS, NULL, FALSE);
    GetDlgItemTextW(dlg, IDC_BT_PREFIX, pre, 128);
    GetDlgItemTextW(dlg, IDC_BT_SUFFIX, suf, 128);
    if (g_bates) swprintf(t, 300, L"The first page: %ls%0*d%ls", pre, max(1, min(digits, 15)), start, suf);
    else {
        WCHAR *p;
        lstrcpynW(fmt, pre, 160);
        p = wcsstr(fmt, L"<<page>>");
        if (p) { WCHAR rest[160]; lstrcpynW(rest, p + 8, 160); swprintf(p, 160 - (p - fmt), L"%d%ls", start, rest); }
        swprintf(t, 300, L"The first page: %ls", fmt);
    }
    SetDlgItemTextW(dlg, IDC_BT_SAMPLE, t);
}

static INT_PTR CALLBACK bates_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetWindowTextW(dlg, g_bates ? L"Bates Numbering" : L"Page Numbers");
        SetDlgItemTextW(dlg, IDC_BT_MODE, g_bates ? L"&Prefix:" : L"&Format:");
        SetDlgItemTextW(dlg, IDC_BT_PREFIX, g_bates ? L"" : L"Page <<page>>");
        SetDlgItemInt(dlg, IDC_BT_START, 1, FALSE);
        SetDlgItemInt(dlg, IDC_BT_DIGITS, 6, FALSE);
        EnableWindow(GetDlgItem(dlg, IDC_BT_DIGITS), g_bates);
        EnableWindow(GetDlgItem(dlg, IDC_BT_SUFFIX), g_bates);
        SendDlgItemMessageW(dlg, IDC_BT_WHERE, CB_ADDSTRING, 0, (LPARAM)L"Footer");
        SendDlgItemMessageW(dlg, IDC_BT_WHERE, CB_ADDSTRING, 0, (LPARAM)L"Header");
        SendDlgItemMessageW(dlg, IDC_BT_WHERE, CB_SETCURSEL, 0, 0);
        SendDlgItemMessageW(dlg, IDC_BT_ALIGN, CB_ADDSTRING, 0, (LPARAM)L"Left");
        SendDlgItemMessageW(dlg, IDC_BT_ALIGN, CB_ADDSTRING, 0, (LPARAM)L"Center");
        SendDlgItemMessageW(dlg, IDC_BT_ALIGN, CB_ADDSTRING, 0, (LPARAM)L"Right");
        SendDlgItemMessageW(dlg, IDC_BT_ALIGN, CB_SETCURSEL, g_bates ? 2 : 1, 0);
        bates_sample(dlg);
        return TRUE;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE) { bates_sample(dlg); return TRUE; }
        if (LOWORD(wp) == IDOK) {
            static const char *ALIGN[] = { "left", "center", "right" };
            char *pre = dlg_text_esc(dlg, IDC_BT_PREFIX), *suf = dlg_text_esc(dlg, IDC_BT_SUFFIX);
            int start = (int)GetDlgItemInt(dlg, IDC_BT_START, NULL, FALSE), digits = (int)GetDlgItemInt(dlg, IDC_BT_DIGITS, NULL, FALSE);
            int where = (int)SendDlgItemMessageW(dlg, IDC_BT_WHERE, CB_GETCURSEL, 0, 0);
            int al = max(0, (int)SendDlgItemMessageW(dlg, IDC_BT_ALIGN, CB_GETCURSEL, 0, 0));
            EndDialog(dlg, IDOK);
            if (g_bates)
                doc_requestf("decorate\tbates\tprefix=%s\tsuffix=%s\tstart=%d\tdigits=%d\twhere=%s\talign=%s", pre ? pre : "",
                             suf ? suf : "", start, max(1, min(15, digits)), where == 1 ? "header" : "footer", ALIGN[al]);
            else
                doc_requestf("decorate\tpagenumbers\tformat=%s\tstart=%d\twhere=%s\talign=%s", pre && pre[0] ? pre : "<<page>>",
                             start, where == 1 ? "header" : "footer", ALIGN[al]);
            free(pre);
            free(suf);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

/* ---- Reduce File Size ------------------------------------------------------------------------------------------- */

static INT_PTR CALLBACK optimize_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static const int DPI[] = { 72, 96, 150, 200, 300 }, Q[] = { 50, 65, 75, 85, 95 };
    static const WCHAR *const QN[] = { L"Low", L"Medium-low", L"Medium", L"High", L"Highest" };
    int i;
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        for (i = 0; i < 5; i++) {
            WCHAR t[32];
            swprintf(t, 32, L"%d dpi%ls", DPI[i], DPI[i] == 150 ? L" (screen & print)" : DPI[i] == 72 ? L" (screen)" : L"");
            SendDlgItemMessageW(dlg, IDC_OP_DPI, CB_ADDSTRING, 0, (LPARAM)t);
            SendDlgItemMessageW(dlg, IDC_OP_QUALITY, CB_ADDSTRING, 0, (LPARAM)QN[i]);
        }
        SendDlgItemMessageW(dlg, IDC_OP_DPI, CB_SETCURSEL, 2, 0);
        SendDlgItemMessageW(dlg, IDC_OP_QUALITY, CB_SETCURSEL, 2, 0);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int d = DPI[max(0, (int)SendDlgItemMessageW(dlg, IDC_OP_DPI, CB_GETCURSEL, 0, 0))];
            int q = Q[max(0, (int)SendDlgItemMessageW(dlg, IDC_OP_QUALITY, CB_GETCURSEL, 0, 0))];
            BOOL meta = IsDlgButtonChecked(dlg, IDC_OP_META) == BST_CHECKED;
            EndDialog(dlg, IDOK);
            if (doc_requestf("optimize\tdpi=%d\tquality=%d\tmetadata=%d", d, q, meta)) {
                char a[32], b[32], n[32];
                if (br_field(g_last_head, "before", a, sizeof(a)) && br_field(g_last_head, "after", b, sizeof(b))) {
                    WCHAR t[300];
                    swprintf(t, 300, L"%d picture%ls made smaller. The file will be about %.1f MB instead of %.1f MB when saved.",
                             br_field(g_last_head, "images", n, sizeof(n)) ? atoi(n) : 0,
                             br_field(g_last_head, "images", n, sizeof(n)) && atoi(n) == 1 ? L" was" : L"s were",
                             atof(b) / 1048576.0, atof(a) / 1048576.0);
                    app_set_status(L"%ls", t);
                    if (!quiet()) MessageBoxW(g_main, t, L"Reduce File Size", MB_OK | MB_ICONINFORMATION);
                }
            }
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

/* ---- attachments ----------------------------------------------------------------------------------------------- */

void doc_free_attach(void)
{
    int i;
    for (i = 0; i < g.nattach; i++) { free(g.attach[i].key); free(g.attach[i].file); free(g.attach[i].desc); }
    free(g.attach);
    g.attach = NULL;
    g.nattach = 0;
    g.attach_loaded = FALSE;
}

BOOL doc_load_attach(void)
{
    char head[256], num[32], *s, *e;
    BYTE *data = NULL;
    DWORD len;
    int n;
    if (g.attach_loaded) return TRUE;
    doc_free_attach();
    g.attach_loaded = TRUE;
    if (!g.npages || br_request("attachments", head, sizeof(head), &data, &len) != 1) { free(data); return FALSE; }
    n = br_field(head, "n", num, sizeof(num)) ? atoi(num) : 0;
    g.attach = n > 0 ? calloc(n, sizeof(attach_t)) : NULL;
    for (s = (char *)data; g.attach && s && *s && g.nattach < n; s = e) {
        char *f[4] = { 0 }, *t = s;
        int k = 0;
        e = strchr(s, '\n');
        if (e) *e++ = 0;
        while (k < 4) { f[k++] = t; t = strchr(t, '\t'); if (!t) break; *t++ = 0; }
        if (k < 3) continue;
        g.attach[g.nattach].key = unesc_utf8(f[0], -1);
        g.attach[g.nattach].file = unesc_utf8(f[1], -1);
        g.attach[g.nattach].size = (DWORD)strtoul(f[2], NULL, 10);
        g.attach[g.nattach].desc = unesc_utf8(k > 3 ? f[3] : "", -1);
        g.nattach++;
    }
    free(data);
    return TRUE;
}

static BOOL attach_to(int i, const WCHAR *target)
{
    char *ek = esc_utf8(g.attach[i].key), *et = unix_esc(target);
    BOOL ok = FALSE;
    if (ek && et) {
        char *line = malloc(strlen(ek) + strlen(et) + 32);
        if (line) {
            char head[256];
            sprintf(line, "getattachment\t%s\t%s", ek, et);
            ok = br_request(line, head, sizeof(head), NULL, NULL) == 1;
            if (!ok) fail_box(head);
            free(line);
        }
    }
    free(ek);
    free(et);
    return ok;
}

void attach_save(int i)
{
    WCHAR target[MAX_PATH];
    if (i < 0 || i >= g.nattach) return;
    lstrcpynW(target, g.attach[i].file && g.attach[i].file[0] ? g.attach[i].file : L"attachment", MAX_PATH);
    if (!file_dialog(TRUE, L"Save Attachment", L"All files (*.*)\0*.*\0", NULL, target, MAX_PATH)) return;
    if (attach_to(i, target)) app_set_status(L"Saved %ls.", target);
}

void attach_open(int i)
{
    WCHAR dir[MAX_PATH], target[MAX_PATH];
    const WCHAR *name;
    if (i < 0 || i >= g.nattach) return;
    name = g.attach[i].file && g.attach[i].file[0] ? g.attach[i].file : L"attachment";
    if (wcspbrk(name, L"\\/:")) name = L"attachment";
    GetTempPathW(MAX_PATH, dir);
    swprintf(target, MAX_PATH, L"%lsSG PDF attachments", dir);
    CreateDirectoryW(target, NULL);
    swprintf(target, MAX_PATH, L"%lsSG PDF attachments\\%ls", dir, name);
    {
        const WCHAR *ext = wcsrchr(name, L'.');
        static const WCHAR *const RISKY[] = { L".exe", L".com", L".bat", L".cmd", L".msi", L".scr", L".vbs", L".js", L".ps1",
                                             L".lnk", L".pif", L".jar", L".hta", L".wsf", L".reg" };
        size_t k;
        for (k = 0; ext && k < sizeof(RISKY) / sizeof(RISKY[0]); k++)
            if (!_wcsicmp(ext, RISKY[k])) {
                MessageBoxW(g_main, L"This attachment is a program or a script. It is not opened from here; save it and "
                                    L"check it before running it.", L"Attachments", MB_OK | MB_ICONWARNING);
                return;
            }
    }
    if (attach_to(i, target)) ShellExecuteW(g_main, NULL, target, NULL, NULL, SW_SHOWNORMAL);
}

void attach_delete(int i)
{
    char *ek;
    if (i < 0 || i >= g.nattach) return;
    ek = esc_utf8(g.attach[i].key);
    if (ek) doc_requestf("delattachment\t%s", ek);
    free(ek);
}

static void attach_add(void)
{
    WCHAR f[MAX_PATH] = L"";
    char *e;
    if (!file_dialog(FALSE, L"Attach a File", L"All files (*.*)\0*.*\0", NULL, f, MAX_PATH)) return;
    e = unix_esc(f);
    if (e && doc_requestf("addattachment\t%s", e)) {
        side_set_mode(SIDE_ATTACH);
        app_set_status(L"The file is attached.");
    }
    free(e);
}

/* ---- Read Out Loud ------------------------------------------------------------------------------------------- */

void read_aloud(int page, BOOL to_end)
{
    int i, last = to_end ? g.npages - 1 : page;
    size_t cap = 0, n = 0;
    WCHAR *all = NULL;
    char *e;
    if (page < 0 || page >= g.npages) return;
    for (i = page; i <= last && n < 400000; i++) {
        page_t *p;
        if (!page_load_text(i)) continue;
        p = &g.pages[i];
        if (n + p->ntext + 4 > cap) {
            WCHAR *t;
            cap = (n + p->ntext + 4) * 2;
            t = realloc(all, cap * sizeof(WCHAR));
            if (!t) break;
            all = t;
        }
        memcpy(all + n, p->text, p->ntext * sizeof(WCHAR));
        n += p->ntext;
        all[n++] = '\n';
    }
    if (!all || !n) {
        free(all);
        app_set_status(L"There is no text on this page to read. Scanned pages need Recognize Text first.");
        return;
    }
    all[n] = 0;
    e = esc_utf8(all);
    free(all);
    if (e) {
        char *line = malloc(strlen(e) + 16), head[256];
        if (line) {
            sprintf(line, "speak\t%s", e);
            if (br_request(line, head, sizeof(head), NULL, NULL) == 1) { g.reading = TRUE; app_set_status(L"Reading out loud..."); }
            else fail_box(head);
            free(line);
        }
        free(e);
    }
}

void read_stop(void)
{
    char head[128];
    br_request("speak", head, sizeof(head), NULL, NULL);
    g.reading = FALSE;
    app_set_status(L"");
}

/* ---- links --------------------------------------------------------------------------------------------------- */

static frect g_link_box;
static int g_link_page;

static INT_PTR CALLBACK link_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        CheckDlgButton(dlg, IDC_LK_WEB, BST_CHECKED);
        SetDlgItemTextW(dlg, IDC_LK_URL, L"https://");
        SetDlgItemInt(dlg, IDC_LK_PAGENO, 1, FALSE);
        SetFocus(GetDlgItem(dlg, IDC_LK_URL));
        SendDlgItemMessageW(dlg, IDC_LK_URL, EM_SETSEL, 8, 8);
        return FALSE;
    case WM_COMMAND:
        /* typing in a box chooses its kind of link */
        if (HIWORD(wp) == EN_SETFOCUS && (LOWORD(wp) == IDC_LK_URL || LOWORD(wp) == IDC_LK_PAGENO)) {
            CheckRadioButton(dlg, IDC_LK_WEB, IDC_LK_PAGE, LOWORD(wp) == IDC_LK_URL ? IDC_LK_WEB : IDC_LK_PAGE);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            if (IsDlgButtonChecked(dlg, IDC_LK_WEB) == BST_CHECKED) {
                char *u = dlg_text_esc(dlg, IDC_LK_URL);
                if (!u || strlen(u) < 4) { free(u); MessageBoxW(dlg, L"Type the web address.", L"Create Link", MB_OK); return TRUE; }
                EndDialog(dlg, IDOK);
                doc_requestf("addlink\t%d\t%.2f %.2f %.2f %.2f\turi=%s", g_link_page, g_link_box.x1, g_link_box.y1,
                             g_link_box.x2, g_link_box.y2, u);
                free(u);
            } else {
                int pg = (int)GetDlgItemInt(dlg, IDC_LK_PAGENO, NULL, FALSE);
                if (pg < 1 || pg > g.npages) { MessageBoxW(dlg, L"There is no such page.", L"Create Link", MB_OK); return TRUE; }
                EndDialog(dlg, IDOK);
                doc_requestf("addlink\t%d\t%.2f %.2f %.2f %.2f\tpage=%d", g_link_page, g_link_box.x1, g_link_box.y1,
                             g_link_box.x2, g_link_box.y2, pg - 1);
            }
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

void link_create(int page, frect box)
{
    g_link_page = page;
    g_link_box = box;
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_LINK), g_main, link_proc, 0);
}

/* ---- the commands --------------------------------------------------------------------------------------------- */

void create_command(int cmd)
{
    switch (cmd) {
    case CMD_CREATE_BLANK: create_blank(); break;
    case CMD_CREATE_FILES: create_files(); break;
    case CMD_CREATE_SCAN: create_scan(FALSE); break;
    case CMD_ORG_SCAN: if (g.npages) create_scan(TRUE); break;
    case CMD_OCR: if (g.npages) DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_OCR), g_main, ocr_proc, 0); break;
    case CMD_HEADFOOT: if (g.npages) DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_HEADFOOT), g_main, headfoot_proc, 0); break;
    case CMD_WATERMARK: if (g.npages) DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_WATERMARK), g_main, watermark_proc, 0); break;
    case CMD_BATES: case CMD_PAGENUM:
        g_bates = cmd == CMD_BATES;
        if (g.npages) DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_BATES), g_main, bates_proc, 0);
        break;
    case CMD_OPTIMIZE: if (g.npages) DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_OPTIMIZE), g_main, optimize_proc, 0); break;
    case CMD_ADDATTACH: if (g.npages) attach_add(); break;
    case CMD_READ_PAGE: read_aloud(g.current, FALSE); break;
    case CMD_READ_DOC: read_aloud(g.current, TRUE); break;
    case CMD_READ_STOP: read_stop(); break;
    }
}
