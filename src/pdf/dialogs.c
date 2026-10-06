/* sg-pdf -- SG PDF: the dialogs -- text, signature, Protect, Export,
 * Combine Files, document properties, Find Text to Redact, Remove Hidden
 * Information, Split, the permissions password -- and the file dialogs.
 *
 * The file dialogs are Windows' own (comdlg32). SG_PDF_FILE_ANSWERS names a
 * text file whose lines answer them in turn (a line of paths joined by "|"
 * for a dialog taking several; an empty line cancels): the gates' way past
 * a dialog that is not ours. SG_PDF_QUIET leaves out the "done" messages.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"
#include <commdlg.h>
#include "resource.h"

static BOOL quiet(void)
{
    return GetEnvironmentVariableW(L"SG_PDF_QUIET", NULL, 0) != 0;
}

static void done_message(const WCHAR *text)
{
    app_set_status(L"%ls", text);
    if (!quiet()) MessageBoxW(g_main, text, APP_NAME, MB_OK | MB_ICONINFORMATION);
}

/* ---- file dialogs -------------------------------------------------------------------------------------------- */

/* the next scripted answer; FALSE when there is no script */
static BOOL scripted(WCHAR *out, int cap)
{
    static int line;
    WCHAR path[MAX_PATH];
    FILE *f;
    char buf[4096];
    int k = 0;
    if (!GetEnvironmentVariableW(L"SG_PDF_FILE_ANSWERS", path, MAX_PATH)) return FALSE;
    out[0] = 0;
    if (!(f = _wfopen(path, L"rb"))) return TRUE;
    while (fgets(buf, sizeof(buf), f)) {
        if (k++ == line) {
            WCHAR *w;
            buf[strcspn(buf, "\r\n")] = 0;
            w = from_utf8(buf, -1);
            if (w) { lstrcpynW(out, w, cap); free(w); }
            break;
        }
    }
    fclose(f);
    line++;
    return TRUE;
}

BOOL file_dialog(BOOL save, const WCHAR *title, const WCHAR *filter, const WCHAR *defext, WCHAR *out, int cap)
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR ans[MAX_PATH * 4];
    if (scripted(ans, MAX_PATH * 4)) {
        lstrcpynW(out, ans, cap);
        return out[0] != 0;
    }
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = out;
    ofn.nMaxFile = cap;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defext;
    ofn.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | (save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    return save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
}

BOOL file_dialog_multi(const WCHAR *title, const WCHAR *filter, WCHAR ***files, int *n)
{
    static WCHAR buf[32768];
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR *s;
    *files = NULL;
    *n = 0;
    if (scripted(buf, 32768)) {
        WCHAR *ctx = NULL, *t;
        for (t = wcstok_s(buf, L"|", &ctx); t; t = wcstok_s(NULL, L"|", &ctx)) {
            WCHAR **nf = realloc(*files, (*n + 1) * sizeof(WCHAR *));
            if (!nf) break;
            *files = nf;
            (*files)[(*n)++] = _wcsdup(t);
        }
        return *n > 0;
    }
    buf[0] = 0;
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = 32768;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT;
    if (!GetOpenFileNameW(&ofn)) return FALSE;
    s = buf + wcslen(buf) + 1;
    if (!*s) {  /* one file: the whole path */
        *files = malloc(sizeof(WCHAR *));
        (*files)[0] = _wcsdup(buf);
        *n = 1;
        return TRUE;
    }
    for (; *s; s += wcslen(s) + 1) {
        WCHAR full[MAX_PATH * 2], **nf = realloc(*files, (*n + 1) * sizeof(WCHAR *));
        if (!nf) break;
        *files = nf;
        swprintf(full, MAX_PATH * 2, L"%ls\\%ls", buf, s);
        (*files)[(*n)++] = _wcsdup(full);
    }
    return *n > 0;
}

/* a Windows path -> the escaped Unix path sg-pdf takes; free it */
static char *engine_path(const WCHAR *path)
{
    char *u = unix_path(path), *e = NULL;
    WCHAR *wu = u ? from_utf8(u, -1) : NULL;
    if (wu) e = esc_utf8(wu);
    free(wu);
    free(u);
    return e;
}

/* ---- a line or a paragraph of text ------------------------------------------------------------------------- */

typedef struct { const WCHAR *title, *prompt; WCHAR *buf; int cap; } text_args;

static INT_PTR CALLBACK text_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    text_args *a = (text_args *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        size_t n, i, k = 0;
        WCHAR *t;
        a = (text_args *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetWindowTextW(dlg, a->title);
        SetDlgItemTextW(dlg, IDC_TX_PROMPT, a->prompt);
        n = wcslen(a->buf);
        t = calloc(n * 2 + 1, sizeof(WCHAR));
        for (i = 0; t && i < n; i++) { if (a->buf[i] == '\n') t[k++] = '\r'; t[k++] = a->buf[i]; }
        SetDlgItemTextW(dlg, IDC_TX_EDIT, t ? t : a->buf);
        free(t);
        SendDlgItemMessageW(dlg, IDC_TX_EDIT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, IDC_TX_EDIT));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int i, k = 0;
            GetDlgItemTextW(dlg, IDC_TX_EDIT, a->buf, a->cap);
            for (i = 0; a->buf[i]; i++) if (a->buf[i] != '\r') a->buf[k++] = a->buf[i];
            a->buf[k] = 0;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

BOOL dlg_text(HWND owner, const WCHAR *title, const WCHAR *prompt, WCHAR *buf, int cap, BOOL multiline)
{
    text_args a = { title, prompt, buf, cap };
    return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(multiline ? IDD_TEXTM : IDD_TEXT1), owner, text_proc, (LPARAM)&a) == IDOK;
}

/* ---- the signature ----------------------------------------------------------------------------------------- */

static float *g_sig;            /* strokes: x, y pairs in 0..1; a pair of -1 ends a stroke */
static int g_nsig, g_capsig;
static BOOL g_sig_down;
static WCHAR g_sig_name[128];
static HWND g_sig_dlg;

static void sig_add(float x, float y)
{
    if (g_nsig + 2 > g_capsig) {
        float *t = realloc(g_sig, (g_capsig = g_capsig ? g_capsig * 2 : 2048) * sizeof(float));
        if (!t) return;
        g_sig = t;
    }
    g_sig[g_nsig++] = x;
    g_sig[g_nsig++] = y;
}

static LRESULT CALLBACK pad_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    RECT rc;
    GetClientRect(hwnd, &rc);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        HPEN pen = CreatePen(PS_SOLID, max(2, dpx(2)), RGB(0x0D, 0x1A, 0x66)), op;
        RECT base = { dpx(12), rc.bottom - dpx(18), rc.right - dpx(12), rc.bottom - dpx(17) };
        int i;
        BOOL preview = GetDlgCtrlID(hwnd) == IDC_SG_PREVIEW;
        FillRect(dc, &rc, GetStockObject(WHITE_BRUSH));
        FillRect(dc, &base, (HBRUSH)GetStockObject(LTGRAY_BRUSH));
        if (preview) {
            HFONT f = CreateFontW(-(rc.bottom * 45 / 100), 0, 0, 0, FW_NORMAL, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0,
                                  CLEARTYPE_QUALITY, 0, L"Z003"), of = SelectObject(dc, f);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(0x0D, 0x1A, 0x66));
            DrawTextW(dc, g_sig_name, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, of);
            DeleteObject(f);
        } else {
            op = SelectObject(dc, pen);
            for (i = 0; i + 1 < g_nsig; i += 2) {
                int x = (int)(g_sig[i] * rc.right), y = (int)(g_sig[i + 1] * rc.bottom);
                if (g_sig[i] < 0) continue;
                if (i == 0 || g_sig[i - 2] < 0) MoveToEx(dc, x, y, NULL); else LineTo(dc, x, y);
            }
            SelectObject(dc, op);
        }
        DeleteObject(pen);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetCapture(hwnd);
        g_sig_down = TRUE;
        sig_add((float)GET_X_LPARAM(lp) / max(1, rc.right), (float)GET_Y_LPARAM(lp) / max(1, rc.bottom));
        return 0;
    case WM_MOUSEMOVE:
        if (g_sig_down) {
            float x = (float)GET_X_LPARAM(lp) / max(1, rc.right), y = (float)GET_Y_LPARAM(lp) / max(1, rc.bottom);
            sig_add(max(0.0f, min(1.0f, x)), max(0.0f, min(1.0f, y)));
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_sig_down) { sig_add(-1, -1); g_sig_down = FALSE; ReleaseCapture(); InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    }
    (void)wp;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void sig_mode(HWND dlg)
{
    BOOL type = IsDlgButtonChecked(dlg, IDC_SG_TYPE), draw = IsDlgButtonChecked(dlg, IDC_SG_DRAW),
         image = IsDlgButtonChecked(dlg, IDC_SG_IMAGE);
    ShowWindow(GetDlgItem(dlg, IDC_SG_NAME), type ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(dlg, IDC_SG_PREVIEW), type ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(dlg, IDC_SG_PAD), draw ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(dlg, IDC_SG_CLEAR), draw ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(dlg, IDC_SG_FILE), image ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(dlg, IDC_SG_BROWSE), image ? SW_SHOW : SW_HIDE);
}

static INT_PTR CALLBACK sig_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        DWORD n = 128;
        g_sig_dlg = dlg;
        g_nsig = 0;
        if (!g_sig_name[0]) GetUserNameW(g_sig_name, &n);
        SetDlgItemTextW(dlg, IDC_SG_NAME, g_sig_name);
        CheckRadioButton(dlg, IDC_SG_TYPE, IDC_SG_IMAGE, IDC_SG_TYPE);
        sig_mode(dlg);
        SetFocus(GetDlgItem(dlg, IDC_SG_NAME));
        SendDlgItemMessageW(dlg, IDC_SG_NAME, EM_SETSEL, 0, -1);
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_SG_TYPE: case IDC_SG_DRAW: case IDC_SG_IMAGE: sig_mode(dlg); return TRUE;
        case IDC_SG_NAME:
            if (HIWORD(wp) == EN_CHANGE) {
                GetDlgItemTextW(dlg, IDC_SG_NAME, g_sig_name, 128);
                InvalidateRect(GetDlgItem(dlg, IDC_SG_PREVIEW), NULL, FALSE);
            }
            return TRUE;
        case IDC_SG_CLEAR: g_nsig = 0; InvalidateRect(GetDlgItem(dlg, IDC_SG_PAD), NULL, FALSE); return TRUE;
        case IDC_SG_BROWSE: {
            WCHAR f[MAX_PATH] = L"";
            if (file_dialog(FALSE, L"Signature Image", L"Pictures\0*.png;*.jpg;*.jpeg;*.gif;*.bmp\0All files (*.*)\0*.*\0", NULL, f, MAX_PATH))
                SetDlgItemTextW(dlg, IDC_SG_FILE, f);
            return TRUE;
        }
        case IDOK: {
            free(g.sig_data);
            g.sig_data = NULL;
            g.sig_kind = 0;
            if (IsDlgButtonChecked(dlg, IDC_SG_TYPE)) {
                GetDlgItemTextW(dlg, IDC_SG_NAME, g_sig_name, 128);
                if (!g_sig_name[0]) { MessageBeep(MB_ICONWARNING); return TRUE; }
                g.sig_kind = 2;
                g.sig_data = _wcsdup(g_sig_name);
            } else if (IsDlgButtonChecked(dlg, IDC_SG_DRAW)) {
                /* strokes: "x y x y;x y ..." */
                size_t cap = g_nsig * 12 + 16, k = 0;
                int i;
                WCHAR *s = malloc(cap * sizeof(WCHAR));
                BOOL start = TRUE;
                if (!s || g_nsig < 4) { free(s); MessageBeep(MB_ICONWARNING); return TRUE; }
                for (i = 0; i + 1 < g_nsig; i += 2) {
                    if (g_sig[i] < 0) { if (k && s[k - 1] != ';') s[k++] = ';'; start = TRUE; continue; }
                    k += swprintf(s + k, cap - k, start ? L"%.4f %.4f" : L" %.4f %.4f", g_sig[i], g_sig[i + 1]);
                    start = FALSE;
                }
                while (k && s[k - 1] == ';') k--;
                s[k] = 0;
                g.sig_kind = 1;
                g.sig_data = s;
            } else {
                WCHAR f[MAX_PATH];
                GetDlgItemTextW(dlg, IDC_SG_FILE, f, MAX_PATH);
                if (!f[0] || GetFileAttributesW(f) == INVALID_FILE_ATTRIBUTES) { MessageBeep(MB_ICONWARNING); return TRUE; }
                g.sig_kind = 3;
                g.sig_data = _wcsdup(f);
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

BOOL dlg_signature(HWND owner)
{
    static BOOL registered;
    if (!registered) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = pad_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_CROSS);
        wc.lpszClassName = L"SgPdfSigPad";
        RegisterClassW(&wc);
        registered = TRUE;
    }
    if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SIGNATURE), owner, sig_proc, 0) != IDOK) return FALSE;
    app_set_status(L"Click on the page where the signature goes, or drag a box for it.");
    return TRUE;
}

/* ---- Protect ------------------------------------------------------------------------------------------------ */

static void protect_enable(HWND dlg)
{
    BOOL o = IsDlgButtonChecked(dlg, IDC_PR_OPEN), r = IsDlgButtonChecked(dlg, IDC_PR_RESTRICT);
    int i;
    EnableWindow(GetDlgItem(dlg, IDC_PR_UPW), o);
    EnableWindow(GetDlgItem(dlg, IDC_PR_UPW2), o);
    EnableWindow(GetDlgItem(dlg, IDC_PR_OPW), r);
    EnableWindow(GetDlgItem(dlg, IDC_PR_OPW2), r);
    for (i = IDC_PR_PRINT; i <= IDC_PR_MODIFY; i++) EnableWindow(GetDlgItem(dlg, i), r);
}

static INT_PTR CALLBACK protect_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        CheckDlgButton(dlg, IDC_PR_OPEN, BST_CHECKED);
        CheckDlgButton(dlg, IDC_PR_PRINT, BST_CHECKED);
        CheckDlgButton(dlg, IDC_PR_COPY, BST_CHECKED);
        protect_enable(dlg);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_PR_OPEN: case IDC_PR_RESTRICT: protect_enable(dlg); return TRUE;
        case IDOK: {
            WCHAR u[128], u2[128], o[128], o2[128];
            BOOL open = IsDlgButtonChecked(dlg, IDC_PR_OPEN), restrict_ = IsDlgButtonChecked(dlg, IDC_PR_RESTRICT);
            char perms[160] = "", *eu, *eo, line[1024];
            static const struct { int id; const char *name; } P2[] = {
                { IDC_PR_PRINT, "print,print_hq" }, { IDC_PR_COPY, "copy,accessibility" }, { IDC_PR_ANNOT, "annotate" },
                { IDC_PR_FORM, "form" }, { IDC_PR_ASSEMBLE, "assemble" }, { IDC_PR_MODIFY, "modify" } };
            int i;
            GetDlgItemTextW(dlg, IDC_PR_UPW, u, 128);
            GetDlgItemTextW(dlg, IDC_PR_UPW2, u2, 128);
            GetDlgItemTextW(dlg, IDC_PR_OPW, o, 128);
            GetDlgItemTextW(dlg, IDC_PR_OPW2, o2, 128);
            if (!open && !restrict_) { EndDialog(dlg, IDCANCEL); return TRUE; }
            if (open && (!u[0] || wcscmp(u, u2))) {
                MessageBoxW(dlg, u[0] ? L"The two passwords to open the document are not the same." : L"Type a password to open the document.",
                            L"Protect", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (restrict_ && (!o[0] || wcscmp(o, o2))) {
                MessageBoxW(dlg, o[0] ? L"The two permissions passwords are not the same." : L"Type a permissions password.",
                            L"Protect", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (open && restrict_ && !wcscmp(u, o)) {
                MessageBoxW(dlg, L"The permissions password must differ from the password to open the document.",
                            L"Protect", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            for (i = 0; restrict_ && i < 6; i++)
                if (IsDlgButtonChecked(dlg, P2[i].id)) { if (perms[0]) lstrcatA(perms, ","); lstrcatA(perms, P2[i].name); }
            if (!restrict_) lstrcpyA(perms, "print,print_hq,copy,accessibility,annotate,form,assemble,modify");
            eu = esc_utf8(open ? u : L"");
            eo = esc_utf8(restrict_ ? o : u);
            snprintf(line, sizeof(line), "protect\tmode=aes256\tuser=%s\towner=%s\tperms=%s", eu ? eu : "", eo ? eo : "", perms);
            if (eu) SecureZeroMemory(eu, strlen(eu));
            if (eo) SecureZeroMemory(eo, strlen(eo));
            free(eu);
            free(eo);
            SecureZeroMemory(u, sizeof(u)); SecureZeroMemory(u2, sizeof(u2));
            SecureZeroMemory(o, sizeof(o)); SecureZeroMemory(o2, sizeof(o2));
            EndDialog(dlg, IDOK);
            if (doc_request(line)) done_message(L"The document will be encrypted (AES-256) when you save it.");
            SecureZeroMemory(line, sizeof(line));
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

void dlg_protect(void)
{
    if (g.perms != 0xFFFF && !dlg_permissions_password()) return;
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_PROTECT), g_main, protect_proc, 0);
    app_status_changed();
}

static INT_PTR CALLBACK owner_pw_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static WCHAR *out;
    switch (msg) {
    case WM_INITDIALOG:
        out = (WCHAR *)lp;
        SetWindowTextW(dlg, L"Permissions Password");
        SetDlgItemTextW(dlg, IDC_PW_TEXT, L"This document is secured. Enter its permissions password to change it or its security.");
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) { GetDlgItemTextW(dlg, IDC_PW_EDIT, out, 128); EndDialog(dlg, IDOK); return TRUE; }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

BOOL dlg_permissions_password(void)
{
    WCHAR pw[128] = L"";
    char *e, line[512];
    BOOL ok;
    if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_PASSWORD), g_main, owner_pw_proc, (LPARAM)pw) != IDOK) return FALSE;
    e = esc_utf8(pw);
    snprintf(line, sizeof(line), "unlock\t%s", e ? e : "");
    if (e) { SecureZeroMemory(e, strlen(e)); free(e); }
    SecureZeroMemory(pw, sizeof(pw));
    ok = doc_request(line);
    SecureZeroMemory(line, sizeof(line));
    toolui_update();
    app_update_title();
    return ok;
}

/* ---- Export ------------------------------------------------------------------------------------------------ */

static int g_export_kind;

static void export_note(HWND dlg)
{
    const WCHAR *t = L"";
    BOOL img = IsDlgButtonChecked(dlg, IDC_EX_PNG) || IsDlgButtonChecked(dlg, IDC_EX_JPEG);
    if (IsDlgButtonChecked(dlg, IDC_EX_DOCX))
        t = L"The text becomes paragraphs with its fonts, sizes, bold, italics and colours, and the pictures are kept, "
            L"a page break between pages. The layout itself is not rebuilt: tables, columns and exact positions reflow.";
    else if (img) t = L"Each page becomes a picture, named after the file with its page number.";
    else if (IsDlgButtonChecked(dlg, IDC_EX_TXT)) t = L"The text in reading order; a page break character between pages.";
    else t = L"The text of each page as HTML.";
    SetDlgItemTextW(dlg, IDC_EX_NOTE, t);
    EnableWindow(GetDlgItem(dlg, IDC_EX_DPI), img);
}

static INT_PTR CALLBACK export_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static const int IDS[] = { IDC_EX_DOCX, IDC_EX_TXT, IDC_EX_PNG, IDC_EX_JPEG, IDC_EX_HTML };
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        static const WCHAR *DPI[] = { L"72 dpi", L"96 dpi", L"150 dpi", L"300 dpi" };
        int i;
        for (i = 0; i < 4; i++) SendDlgItemMessageW(dlg, IDC_EX_DPI, CB_ADDSTRING, 0, (LPARAM)DPI[i]);
        SendDlgItemMessageW(dlg, IDC_EX_DPI, CB_SETCURSEL, 2, 0);
        CheckDlgButton(dlg, IDS[g_export_kind >= 1 && g_export_kind <= 5 ? g_export_kind - 1 : 0], BST_CHECKED);
        CheckDlgButton(dlg, IDC_EX_ALL, BST_CHECKED);
        export_note(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_EX_DOCX: case IDC_EX_TXT: case IDC_EX_PNG: case IDC_EX_JPEG: case IDC_EX_HTML: export_note(dlg); return TRUE;
        case IDC_EX_RANGETXT:
            if (HIWORD(wp) == EN_CHANGE) CheckRadioButton(dlg, IDC_EX_ALL, IDC_EX_RANGE, IDC_EX_RANGE);
            return TRUE;
        case IDOK: {
            static const WCHAR *FILTERS[] = {
                L"Word document (*.docx)\0*.docx\0", L"Text (*.txt)\0*.txt\0", L"PNG image (*.png)\0*.png\0",
                L"JPEG image (*.jpg)\0*.jpg\0", L"HTML web page (*.html)\0*.html\0" };
            static const WCHAR *EXT[] = { L"docx", L"txt", L"png", L"jpg", L"html" };
            static const char *KIND[] = { "docx", "txt", "png", "jpeg", "html" };
            static const int DPIS[] = { 72, 96, 150, 300 };
            WCHAR file[MAX_PATH], *dot, range[128];
            char pages[512] = "", *ep, line[MAX_PATH * 8], head[512];
            BYTE *data = NULL;
            DWORD len;
            int k = 0, i, dpi = DPIS[max(0, (int)SendDlgItemMessageW(dlg, IDC_EX_DPI, CB_GETCURSEL, 0, 0)) % 4];
            for (i = 0; i < 5; i++) if (IsDlgButtonChecked(dlg, IDS[i])) k = i;
            if (IsDlgButtonChecked(dlg, IDC_EX_CURRENT)) snprintf(pages, sizeof(pages), "%d", g.current);
            else if (IsDlgButtonChecked(dlg, IDC_EX_RANGE)) {
                /* "1-3, 5" as the user counts -> 0-based */
                WCHAR *s, *ctx = NULL;
                int n = 0;
                GetDlgItemTextW(dlg, IDC_EX_RANGETXT, range, 128);
                for (s = wcstok_s(range, L", ", &ctx); s; s = wcstok_s(NULL, L", ", &ctx)) {
                    int a = 0, b = 0;
                    if (swscanf(s, L"%d-%d", &a, &b) == 2) n += snprintf(pages + n, sizeof(pages) - n, "%s%d-%d", n ? "," : "", a - 1, b - 1);
                    else if (swscanf(s, L"%d", &a) == 1) n += snprintf(pages + n, sizeof(pages) - n, "%s%d", n ? "," : "", a - 1);
                }
                if (!pages[0]) { MessageBoxW(dlg, L"Type the pages to export, as 1-3, 5.", L"Export", MB_OK | MB_ICONWARNING); return TRUE; }
            }
            lstrcpynW(file, g.name, MAX_PATH);
            if ((dot = wcsrchr(file, '.'))) *dot = 0;
            if (!file_dialog(TRUE, L"Export", FILTERS[k], EXT[k], file, MAX_PATH)) return TRUE;
            EndDialog(dlg, IDOK);
            if (!(ep = engine_path(file))) return TRUE;
            snprintf(line, sizeof(line), "export\t%s\t%s\tdpi=%d%s%s", KIND[k], ep, dpi, pages[0] ? "\tpages=" : "", pages);
            free(ep);
            if (br_request(line, head, sizeof(head), &data, &len) == 1) {
                WCHAR t[MAX_PATH + 100];
                char num[16];
                int n = br_field(head, "n", num, sizeof(num)) ? atoi(num) : 1;
                if (n > 1) swprintf(t, MAX_PATH + 100, L"Exported %d files, beginning with the name \"%ls\".", n, file);
                else swprintf(t, MAX_PATH + 100, L"Exported to \"%ls\".", file);
                done_message(t);
            } else {
                WCHAR *m = from_utf8(head, -1);
                app_set_status(L"The export failed: %ls", m ? m : L"");
                if (!quiet()) MessageBoxW(g_main, g.status, APP_NAME, MB_OK | MB_ICONWARNING);
                free(m);
            }
            free(data);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

void dlg_export_as(int kind)
{
    if (!app_can(16)) {
        MessageBoxW(g_main, L"This document's security does not allow copying its content.", APP_NAME, MB_OK | MB_ICONWARNING);
        return;
    }
    g_export_kind = kind;
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_EXPORT), g_main, export_proc, 0);
    app_status_changed();
}

/* ---- Combine Files -------------------------------------------------------------------------------------------- */

static INT_PTR CALLBACK combine_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND list = GetDlgItem(dlg, IDC_CB_LIST);
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        if (g.npages && g.path[0]) SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)g.path);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CB_ADD: {
            WCHAR **files;
            int n, i;
            if (file_dialog_multi(L"Add Files", L"PDF documents and pictures\0*.pdf;*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff\0"
                                                L"All files (*.*)\0*.*\0", &files, &n)) {
                for (i = 0; i < n; i++) { SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)files[i]); free(files[i]); }
                free(files);
            }
            return TRUE;
        }
        case IDC_CB_OPEN: {
            /* the documents open in SG PDF's tabs (their saved files) */
            static WCHAR paths[32][MAX_PATH];
            int n = tabs_paths(paths, 32), i, have = 0;
            for (i = 0; i < n; i++)
                if (SendMessageW(list, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)paths[i]) == LB_ERR) {
                    SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)paths[i]);
                    have++;
                }
            if (!have) MessageBeep(MB_ICONINFORMATION);
            return TRUE;
        }
        case IDC_CB_REMOVE: {
            int i = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
            if (i >= 0) SendMessageW(list, LB_DELETESTRING, i, 0);
            return TRUE;
        }
        case IDC_CB_UP: case IDC_CB_DOWN: {
            int i = (int)SendMessageW(list, LB_GETCURSEL, 0, 0), j = LOWORD(wp) == IDC_CB_UP ? i - 1 : i + 1;
            WCHAR t[MAX_PATH * 2];
            if (i < 0 || j < 0 || j >= (int)SendMessageW(list, LB_GETCOUNT, 0, 0)) return TRUE;
            SendMessageW(list, LB_GETTEXT, i, (LPARAM)t);
            SendMessageW(list, LB_DELETESTRING, i, 0);
            SendMessageW(list, LB_INSERTSTRING, j, (LPARAM)t);
            SendMessageW(list, LB_SETCURSEL, j, 0);
            return TRUE;
        }
        case IDOK: {
            int n = (int)SendMessageW(list, LB_GETCOUNT, 0, 0), i;
            WCHAR out[MAX_PATH] = L"Combined.pdf";
            char *line, *e;
            size_t cap, k;
            if (n < 1) { MessageBoxW(dlg, L"Add the files to combine first.", L"Combine Files", MB_OK | MB_ICONINFORMATION); return TRUE; }
            if (!file_dialog(TRUE, L"Save the Combined PDF", L"PDF documents (*.pdf)\0*.pdf\0", L"pdf", out, MAX_PATH)) return TRUE;
            cap = (size_t)(n + 1) * MAX_PATH * 8 + 64;
            if (!(line = malloc(cap))) return TRUE;
            e = engine_path(out);
            k = snprintf(line, cap, "combine\t%s", e ? e : "");
            free(e);
            for (i = 0; i < n; i++) {
                WCHAR t[MAX_PATH * 2];
                SendMessageW(list, LB_GETTEXT, i, (LPARAM)t);
                e = engine_path(t);
                if (e) k += snprintf(line + k, cap - k, "\t%s", e);
                free(e);
            }
            EndDialog(dlg, IDOK);
            {
                char head[512];
                BYTE *data = NULL;
                DWORD len;
                if (br_request(line, head, sizeof(head), &data, &len) == 1) {
                    free(data);
                    app_open(out);
                } else {
                    WCHAR *m = from_utf8(head, -1);
                    app_set_status(L"The files could not be combined: %ls", m ? m : L"");
                    if (!quiet()) MessageBoxW(g_main, g.status, APP_NAME, MB_OK | MB_ICONWARNING);
                    free(m);
                    free(data);
                }
            }
            free(line);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

void dlg_combine(void)
{
    if (!g.bridged) return;
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_COMBINE), g_main, combine_proc, 0);
    app_status_changed();
}

/* ---- document properties ----------------------------------------------------------------------------------------- */

static WCHAR g_pp[4][512];

static void read_properties(void)
{
    char head[512], *s, *e;
    BYTE *data = NULL;
    DWORD len;
    int i;
    for (i = 0; i < 4; i++) g_pp[i][0] = 0;
    if (br_request("state", head, sizeof(head), &data, &len) != 1) { free(data); return; }
    for (s = (char *)data; s && *s; s = e) {
        static const char *KEYS[] = { "title ", "author ", "subject ", "keywords " };
        e = strchr(s, '\n');
        if (e) *e++ = 0;
        for (i = 0; i < 4; i++) if (!strncmp(s, KEYS[i], strlen(KEYS[i]))) {
            WCHAR *w = from_utf8(s + strlen(KEYS[i]), -1);
            if (w) { lstrcpynW(g_pp[i], w, 512); free(w); }
        }
    }
    free(data);
}

static INT_PTR CALLBACK props_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static const int IDS[4] = { IDC_PP_TITLE, IDC_PP_AUTHOR, IDC_PP_SUBJECT, IDC_PP_KEYWORDS };
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        WCHAR t[1200];
        WIN32_FILE_ATTRIBUTE_DATA fa;
        double kb = 0;
        int i;
        if (GetFileAttributesExW(g.path, GetFileExInfoStandard, &fa)) kb = ((double)fa.nFileSizeHigh * 4294967296.0 + fa.nFileSizeLow) / 1024;
        swprintf(t, 1200, L"File:\t%ls\nSize:\t%.1f KB\nPages:\t%d\nPage size:\t%.2f x %.2f in\nSecurity:\t%ls", g.path, kb, g.npages,
                 g.npages ? g.pages[0].w / 72.0 : 0, g.npages ? g.pages[0].h / 72.0 : 0,
                 !g.encrypted ? L"None" : g.perms == 0xFFFF ? L"Password (AES), full access" : L"Password, restricted");
        SetDlgItemTextW(dlg, IDC_PP_INFO, t);
        read_properties();
        for (i = 0; i < 4; i++) SetDlgItemTextW(dlg, IDS[i], g_pp[i]);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            static const char *KEYS[] = { "title", "author", "subject", "keywords" };
            char line[4096];
            int i, k = snprintf(line, sizeof(line), "properties"), changed = 0;
            for (i = 0; i < 4; i++) {
                WCHAR v[512];
                char *e;
                GetDlgItemTextW(dlg, IDS[i], v, 512);
                if (!wcscmp(v, g_pp[i])) continue;
                e = esc_utf8(v);
                if (e) { k += snprintf(line + k, sizeof(line) - k, "\t%s=%s", KEYS[i], e); changed++; }
                free(e);
            }
            EndDialog(dlg, IDOK);
            if (changed) doc_request(line);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

void dlg_properties(void)
{
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_PROPERTIES), g_main, props_proc, 0);
    app_status_changed();
}

/* ---- Find Text to Redact -------------------------------------------------------------------------------------- */

static const char *PATTERN_KEYS[] = { "phone", "email", "ssn", "card", "date" };

static BOOL find_request(HWND dlg, BOOL mark, int *hits)
{
    WCHAR t[512];
    char what[1600], flags[4] = "", line[1800], head[512], num[16];
    BYTE *data = NULL;
    DWORD len;
    int k = 0;
    if (IsDlgButtonChecked(dlg, IDC_FR_PATTERN)) {
        int i = (int)SendDlgItemMessageW(dlg, IDC_FR_PATLIST, CB_GETCURSEL, 0, 0);
        snprintf(what, sizeof(what), "pattern:%s", PATTERN_KEYS[i < 0 || i > 4 ? 0 : i]);
    } else {
        char *e;
        GetDlgItemTextW(dlg, IDC_FR_TEXT, t, 512);
        if (!t[0]) { MessageBeep(MB_ICONWARNING); SetFocus(GetDlgItem(dlg, IDC_FR_TEXT)); return FALSE; }
        if (!(e = esc_utf8(t))) return FALSE;
        lstrcpynA(what, e, sizeof(what));
        free(e);
        if (IsDlgButtonChecked(dlg, IDC_FR_CASE)) flags[k++] = 'c';
        if (IsDlgButtonChecked(dlg, IDC_FR_WHOLE)) flags[k++] = 'w';
    }
    flags[k] = 0;
    snprintf(line, sizeof(line), "redactfind\t%s\t%s\t%d", what, flags, mark ? 1 : 0);
    if (mark) return doc_request(line);
    if (br_request(line, head, sizeof(head), &data, &len) != 1) { free(data); return FALSE; }
    *hits = br_field(head, "n", num, sizeof(num)) ? atoi(num) : 0;
    free(data);
    return TRUE;
}

static INT_PTR CALLBACK findredact_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        static const WCHAR *NAMES[] = { L"Phone numbers", L"E-mail addresses", L"Social security numbers", L"Credit card numbers", L"Dates" };
        int i;
        for (i = 0; i < 5; i++) SendDlgItemMessageW(dlg, IDC_FR_PATLIST, CB_ADDSTRING, 0, (LPARAM)NAMES[i]);
        SendDlgItemMessageW(dlg, IDC_FR_PATLIST, CB_SETCURSEL, 0, 0);
        CheckRadioButton(dlg, IDC_FR_PHRASE, IDC_FR_PATTERN, IDC_FR_PHRASE);
        SetFocus(GetDlgItem(dlg, IDC_FR_TEXT));
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_FR_TEXT:
            if (HIWORD(wp) == EN_CHANGE) CheckRadioButton(dlg, IDC_FR_PHRASE, IDC_FR_PATTERN, IDC_FR_PHRASE);
            return TRUE;
        case IDC_FR_PATLIST:
            if (HIWORD(wp) == CBN_SELCHANGE) CheckRadioButton(dlg, IDC_FR_PHRASE, IDC_FR_PATTERN, IDC_FR_PATTERN);
            return TRUE;
        case IDC_FR_FIND: {
            int n = 0;
            WCHAR t[64];
            if (find_request(dlg, FALSE, &n)) {
                swprintf(t, 64, n == 1 ? L"1 result" : L"%d results", n);
                SetDlgItemTextW(dlg, IDC_FR_RESULT, t);
            }
            return TRUE;
        }
        case IDOK: {
            int n = 0;
            if (find_request(dlg, TRUE, &n)) EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

void dlg_find_redact(void)
{
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_FINDREDACT), g_main, findredact_proc, 0);
    toolui_update();
    app_status_changed();
}

/* ---- Remove Hidden Information --------------------------------------------------------------------------------- */

static BOOL g_sanitize_after;

static INT_PTR CALLBACK sanitize_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static const struct { int id; const char *key; } O[] = {
        { IDC_SN_METADATA, "metadata" }, { IDC_SN_ATTACH, "attachments" }, { IDC_SN_COMMENTS, "comments" },
        { IDC_SN_FORMS, "forms" }, { IDC_SN_HIDDEN, "hidden_text" }, { IDC_SN_LAYERS, "layers" },
        { IDC_SN_BOOKMARKS, "bookmarks" }, { IDC_SN_LINKS, "links" }, { IDC_SN_THUMBS, "thumbnails" } };
    int i;
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG:
        SetDlgItemTextW(dlg, IDC_SN_HEAD, g_sanitize_after
            ? L"The redactions are applied. Remove hidden information too? What is ticked is removed from the document."
            : L"Information a reader does not see, but that stays in the file. What is ticked is removed from the document.");
        for (i = 0; i < 9; i++) CheckDlgButton(dlg, O[i].id, O[i].id == IDC_SN_BOOKMARKS || O[i].id == IDC_SN_FORMS ? BST_UNCHECKED : BST_CHECKED);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            char line[512];
            int k = snprintf(line, sizeof(line), "sanitize");
            for (i = 0; i < 9; i++) k += snprintf(line + k, sizeof(line) - k, "\t%s=%d", O[i].key, IsDlgButtonChecked(dlg, O[i].id) ? 1 : 0);
            EndDialog(dlg, IDOK);
            if (doc_request(line)) done_message(L"The hidden information was removed. Save the document to keep the change.");
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

void dlg_sanitize(BOOL after_apply)
{
    g_sanitize_after = after_apply;
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SANITIZE), g_main, sanitize_proc, 0);
    toolui_update();
    app_status_changed();
}

/* ---- Split ------------------------------------------------------------------------------------------------------ */

static INT_PTR CALLBACK split_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        WCHAR t[MAX_PATH + 100];
        SetDlgItemInt(dlg, IDC_SP_EVERY, max(1, g.npages / 2), FALSE);
        swprintf(t, MAX_PATH + 100, L"The files are saved beside the document, named \"%ls\" with _Part1, _Part2...", g.name);
        SetDlgItemTextW(dlg, IDC_SP_INFO, t);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int n = GetDlgItemInt(dlg, IDC_SP_EVERY, NULL, FALSE);
            WCHAR folder[MAX_PATH], base[MAX_PATH], *s;
            char *ef, *eb, line[MAX_PATH * 16], head[512], num[16];
            BYTE *data = NULL;
            DWORD len;
            if (n < 1) { MessageBeep(MB_ICONWARNING); return TRUE; }
            lstrcpynW(folder, g.path, MAX_PATH);
            if ((s = wcsrchr(folder, '\\'))) *s = 0;
            lstrcpynW(base, g.name, MAX_PATH);
            if ((s = wcsrchr(base, '.'))) *s = 0;
            ef = engine_path(folder);
            eb = esc_utf8(base);
            EndDialog(dlg, IDOK);
            snprintf(line, sizeof(line), "split\t%d\t%s\t%s", n, ef ? ef : "", eb ? eb : "");
            free(ef);
            free(eb);
            if (br_request(line, head, sizeof(head), &data, &len) == 1) {
                WCHAR t[MAX_PATH + 100];
                swprintf(t, MAX_PATH + 100, L"Split into %d files in \"%ls\".",
                         br_field(head, "n", num, sizeof(num)) ? atoi(num) : 0, folder);
                done_message(t);
            } else {
                WCHAR *m = from_utf8(head, -1);
                app_set_status(L"The document could not be split: %ls", m ? m : L"");
                if (!quiet()) MessageBoxW(g_main, g.status, APP_NAME, MB_OK | MB_ICONWARNING);
                free(m);
            }
            free(data);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

void dlg_split(void)
{
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SPLIT), g_main, split_proc, 0);
    app_status_changed();
}
