/* sg-pdf -- SG PDF: signing with a certificate, and the document's signatures.
 *
 * Fill & Sign > Sign with Certificate (or a click on an empty signature
 * field): the digital ID (a .pfx/.p12 file and its password; a new
 * self-signed one can be made here), a reason and a location; then a box is
 * drawn where the signature goes (an empty field is used as it is), and the
 * signed document is saved -- signing is the last thing done to a file, as
 * the familiar editor asks: the signature covers the file as it is then.
 * sg-pdf appends it as an incremental update, so earlier signatures stay
 * valid.
 *
 * The Signatures pane and the bar over the pages say whether the
 * signatures are valid; a click on a signature shows who signed, when, why,
 * and whether the signer's identity is confirmed (a trusted certificate).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"
#include "resource.h"

/* ---- the document's signatures ------------------------------------------------------------------------ */

void doc_free_sigs(void)
{
    int i;
    for (i = 0; i < g.nsigs; i++) {
        free(g.sigs[i].name); free(g.sigs[i].signer); free(g.sigs[i].time); free(g.sigs[i].reason); free(g.sigs[i].detail);
    }
    free(g.sigs);
    g.sigs = NULL;
    g.nsigs = 0;
    g.sigs_loaded = FALSE;
}

static int split(char *s, char **f, int max)
{
    int n = 0;
    while (n < max) {
        f[n++] = s;
        s = strchr(s, '\t');
        if (!s) break;
        *s++ = 0;
    }
    return n;
}

BOOL doc_load_sigs(void)
{
    char head[512], num[32], *s, *e;
    BYTE *data = NULL;
    DWORD len;
    int n;
    if (g.sigs_loaded) return TRUE;
    doc_free_sigs();
    g.sigs_loaded = TRUE;
    if (!g.npages || br_request("signatures", head, sizeof(head), &data, &len) != 1) { free(data); return FALSE; }
    n = br_field(head, "n", num, sizeof(num)) ? atoi(num) : 0;
    g.sigs = n > 0 ? calloc(n, sizeof(sig_t)) : NULL;
    for (s = (char *)data; g.sigs && s && *s && g.nsigs < n; s = e) {
        char *f[10] = { 0 };
        sig_t *sg = &g.sigs[g.nsigs];
        e = strchr(s, '\n');
        if (e) *e++ = 0;
        if (split(s, f, 10) < 10) continue;
        sg->page = atoi(f[0]);
        sg->xref = atoi(f[1]);
        sg->name = unesc_utf8(f[2], -1);
        sg->state = !strcmp(f[3], "valid") ? SIG_VALID : !strcmp(f[3], "unknown") ? SIG_UNKNOWN :
                    !strcmp(f[3], "invalid") ? SIG_INVALID : SIG_UNSIGNED;
        sg->covers = atoi(f[4]) != 0;
        sscanf(f[5], "%f %f %f %f", &sg->box.x1, &sg->box.y1, &sg->box.x2, &sg->box.y2);
        sg->signer = unesc_utf8(f[6], -1);
        sg->time = unesc_utf8(f[7], -1);
        sg->reason = unesc_utf8(f[8], -1);
        sg->detail = unesc_utf8(f[9], -1);
        g.nsigs++;
    }
    free(data);
    return TRUE;
}

/* the line over the pages: level 0 good, 1 a warning, 2 a problem; out empty when nothing is signed */
void sig_banner(WCHAR *out, int cap, int *level)
{
    int i, signed_n = 0, bad = 0, unknown = 0;
    out[0] = 0;
    *level = 0;
    if (!g.npages) return;
    doc_load_sigs();
    for (i = 0; i < g.nsigs; i++) {
        if (g.sigs[i].state == SIG_UNSIGNED) continue;
        signed_n++;
        if (g.sigs[i].state == SIG_INVALID) bad++;
        if (g.sigs[i].state == SIG_UNKNOWN) unknown++;
    }
    if (!signed_n) return;
    if (bad) {
        *level = 2;
        swprintf(out, cap, L"At least one signature is invalid: the document was changed after it was signed, or the signature is damaged.");
    } else if (unknown) {
        *level = 1;
        swprintf(out, cap, L"Signed, and the signature%ls valid, but the signer's identity %ls not confirmed by a trusted certificate.",
                 signed_n == 1 ? L" is" : L"s are", unknown == 1 && signed_n == 1 ? L"is" : L"of at least one signature is");
    } else swprintf(out, cap, L"Signed and all signatures are valid.");
    if (g.dirty && !*level) *level = 1;
}

void sig_show(int i)
{
    sig_t *s;
    WCHAR t[2048];
    const WCHAR *state;
    if (i < 0 || i >= g.nsigs) return;
    s = &g.sigs[i];
    if (s->state == SIG_UNSIGNED) {
        swprintf(t, 2048, L"The signature field \"%ls\" is not signed yet.\n\nSign it with a certificate?", s->name ? s->name : L"");
        if (MessageBoxW(g_main, t, L"Signature", MB_YESNO | MB_ICONQUESTION) == IDYES) dlg_certsign(s->xref);
        return;
    }
    state = s->state == SIG_VALID ? L"The signature is valid, and the signer's certificate is trusted." :
            s->state == SIG_UNKNOWN ? L"The signature is valid: the document has not been changed since it was signed. "
                                      L"The signer's identity is not confirmed." :
            L"The signature is NOT valid.";
    swprintf(t, 2048, L"Signed by: %ls\nSigned on: %ls\n%ls%ls%ls\n%ls\n\n%ls%ls%ls%ls",
             s->signer && s->signer[0] ? s->signer : L"(unknown)", s->time && s->time[0] ? s->time : L"(not given)",
             s->reason && s->reason[0] ? L"Reason: " : L"", s->reason ? s->reason : L"", s->reason && s->reason[0] ? L"\n" : L"",
             state, s->detail && s->detail[0] ? s->detail : L"", s->detail && s->detail[0] ? L"." : L"",
             s->state != SIG_INVALID && !s->covers ? L"\n\nThe document has been changed since this signature was applied "
                                                     L"(a later revision: another signature, or form filling)." : L"",
             s->state == SIG_UNKNOWN ? L"\n\nTrust this signer's certificate on this computer?" : L"");
    if (s->state == SIG_UNKNOWN) {
        if (MessageBoxW(g_main, t, L"Signature", MB_YESNO | MB_ICONINFORMATION) == IDYES &&
            doc_requestf("trustsigner\t%d\t%d", s->page, s->xref)) {
            doc_free_sigs();
            doc_load_sigs();
            app_set_status(L"The signer's certificate is trusted on this computer now.");
            InvalidateRect(g_view, NULL, FALSE);
            side_update();
        }
    } else MessageBoxW(g_main, t, L"Signature", MB_OK | (s->state == SIG_VALID ? MB_ICONINFORMATION : MB_ICONWARNING));
}

/* ---- digital IDs --------------------------------------------------------------------------------------------- */

static char *unix_esc(const WCHAR *path)
{
    char *u = unix_path(path), *e = NULL;
    WCHAR *wu = u ? from_utf8(u, -1) : NULL;
    if (wu) e = esc_utf8(wu);
    free(wu);
    free(u);
    return e;
}

static char *wesc(const WCHAR *w)
{
    return esc_utf8(w ? w : L"");
}

static WCHAR *g_mi_out;
static int g_mi_cap;

static INT_PTR CALLBACK makeid_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        WCHAR name[128] = L"", path[MAX_PATH];
        DWORD n = 128;
        GetUserNameW(name, &n);
        SetDlgItemTextW(dlg, IDC_MI_NAME, name);
        if (GetEnvironmentVariableW(L"USERPROFILE", path, MAX_PATH)) {
            lstrcatW(path, L"\\Documents\\My Digital ID.pfx");
            SetDlgItemTextW(dlg, IDC_MI_FILE, path);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_MI_BROWSE) {
            WCHAR f[MAX_PATH];
            GetDlgItemTextW(dlg, IDC_MI_FILE, f, MAX_PATH);
            if (file_dialog(TRUE, L"Save Digital ID", L"Digital IDs (*.pfx)\0*.pfx\0", L"pfx", f, MAX_PATH))
                SetDlgItemTextW(dlg, IDC_MI_FILE, f);
            return TRUE;
        }
        if (LOWORD(wp) == IDOK) {
            WCHAR name[128], org[128], mail[128], pw[128], pw2[128], file[MAX_PATH];
            char *en, *eo, *em, *ep, *ef;
            BOOL ok = FALSE;
            GetDlgItemTextW(dlg, IDC_MI_NAME, name, 128);
            GetDlgItemTextW(dlg, IDC_MI_ORG, org, 128);
            GetDlgItemTextW(dlg, IDC_MI_EMAIL, mail, 128);
            GetDlgItemTextW(dlg, IDC_MI_PW, pw, 128);
            GetDlgItemTextW(dlg, IDC_MI_PW2, pw2, 128);
            GetDlgItemTextW(dlg, IDC_MI_FILE, file, MAX_PATH);
            if (!name[0] || !pw[0] || !file[0]) {
                MessageBoxW(dlg, L"Give a name, a password and where to save the digital ID.", L"New Digital ID", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (wcscmp(pw, pw2)) {
                MessageBoxW(dlg, L"The passwords are not the same.", L"New Digital ID", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            en = wesc(name); eo = wesc(org); em = wesc(mail); ep = wesc(pw); ef = unix_esc(file);
            if (en && eo && em && ep && ef)
                ok = doc_requestf("makeid\t%s\t%s\tname=%s\torg=%s\temail=%s", ef, ep, en, eo, em);
            if (ep) SecureZeroMemory(ep, strlen(ep));
            free(en); free(eo); free(em); free(ep); free(ef);
            SecureZeroMemory(pw2, sizeof(pw2));
            if (ok) {
                if (g_mi_out) lstrcpynW(g_mi_out, file, g_mi_cap);
                SecureZeroMemory(pw, sizeof(pw));
                EndDialog(dlg, IDOK);
            }
            SecureZeroMemory(pw, sizeof(pw));
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

void dlg_makeid(HWND owner, WCHAR *out_path, int cap)
{
    g_mi_out = out_path;
    g_mi_cap = cap;
    if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_MAKEID), owner, makeid_proc, 0) == IDOK)
        app_set_status(L"Your digital ID was made.");
}

/* ---- the dialog ---------------------------------------------------------------------------------------- */

static void id_info(HWND dlg)
{
    WCHAR file[MAX_PATH], pw[128], *w;
    char *ef, *ep, head[256];
    BYTE *data = NULL;
    DWORD len;
    GetDlgItemTextW(dlg, IDC_CS_FILE, file, MAX_PATH);
    GetDlgItemTextW(dlg, IDC_CS_PW, pw, 128);
    if (!file[0] || !pw[0]) {
        SetDlgItemTextW(dlg, IDC_CS_INFO, L"Choose your digital ID and type its password. Without one, make a new "
                                          L"self-signed digital ID.");
        return;
    }
    ef = unix_esc(file);
    ep = wesc(pw);
    SecureZeroMemory(pw, sizeof(pw));
    if (ef && ep) {
        char line[MAX_PATH * 4 + 300];
        snprintf(line, sizeof(line), "idinfo\t%s\t%s", ef, ep);
        if (br_request(line, head, sizeof(head), &data, &len) == 1 && data) {
            WCHAR t[600], *nl;
            w = from_utf8((char *)data, (int)len);
            if (w) {
                /* "name ...\nissuer ...\nexpires ...\nselfsigned N" */
                lstrcpyW(t, L"Signs as: ");
                for (nl = w; *nl; nl++) if (*nl == '\n') *nl = 0x01;
                {
                    WCHAR *p = w, *name = L"", *exp = L"", *self = L"0";
                    while (p && *p) {
                        WCHAR *end = wcschr(p, 0x01);
                        if (end) *end = 0;
                        if (!wcsncmp(p, L"name ", 5)) name = p + 5;
                        else if (!wcsncmp(p, L"expires ", 8)) exp = p + 8;
                        else if (!wcsncmp(p, L"selfsigned ", 11)) self = p + 11;
                        p = end ? end + 1 : NULL;
                    }
                    swprintf(t, 600, L"Signs as: %ls\nValid until %ls.%ls", name, exp,
                             self[0] == '1' ? L"\nA self-signed digital ID: others see the signer's identity as not "
                                              L"confirmed until they trust it." : L"");
                }
                SetDlgItemTextW(dlg, IDC_CS_INFO, t);
                free(w);
            }
        } else {
            WCHAR *m = from_utf8(head, -1);
            SetDlgItemTextW(dlg, IDC_CS_INFO, m && strstr(head, "password") ? L"The password is not right for this digital ID."
                                                                          : L"That file is not a digital ID this program can read.");
            free(m);
        }
        free(data);
        SecureZeroMemory(line, sizeof(line));
    }
    if (ep) { SecureZeroMemory(ep, strlen(ep)); free(ep); }
    free(ef);
}

static INT_PTR CALLBACK certsign_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: {
        WCHAR file[MAX_PATH] = L"";
        DWORD cb = sizeof(file);
        if (!g.cert_file[0] && !RegGetValueW(HKEY_CURRENT_USER, L"Software\\Stained Glass\\SG PDF", L"DigitalID",
                                             RRF_RT_REG_SZ, NULL, file, &cb))
            lstrcpynW(g.cert_file, file, MAX_PATH);
        SetDlgItemTextW(dlg, IDC_CS_FILE, g.cert_file);
        SetDlgItemTextW(dlg, IDC_CS_REASON, g.cert_reason);
        SetDlgItemTextW(dlg, IDC_CS_LOCATION, g.cert_location);
        EnableWindow(GetDlgItem(dlg, IDC_CS_PICTURE), g.sig_kind != 0 && g.sig_data != NULL);
        CheckDlgButton(dlg, IDC_CS_PICTURE, g.sig_kind && g.sig_data ? BST_CHECKED : BST_UNCHECKED);
        id_info(dlg);
        if (g.cert_file[0]) SetFocus(GetDlgItem(dlg, IDC_CS_PW));
        return !g.cert_file[0];
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CS_BROWSE: {
            WCHAR f[MAX_PATH];
            GetDlgItemTextW(dlg, IDC_CS_FILE, f, MAX_PATH);
            if (file_dialog(FALSE, L"Digital ID", L"Digital IDs (*.pfx;*.p12)\0*.pfx;*.p12\0All files (*.*)\0*.*\0", NULL, f, MAX_PATH)) {
                SetDlgItemTextW(dlg, IDC_CS_FILE, f);
                id_info(dlg);
            }
            return TRUE;
        }
        case IDC_CS_NEWID: {
            WCHAR f[MAX_PATH] = L"";
            dlg_makeid(dlg, f, MAX_PATH);
            if (f[0]) { SetDlgItemTextW(dlg, IDC_CS_FILE, f); SetDlgItemTextW(dlg, IDC_CS_PW, L""); id_info(dlg); }
            return TRUE;
        }
        case IDC_CS_PW: case IDC_CS_FILE:
            if (HIWORD(wp) == EN_KILLFOCUS) id_info(dlg);
            return TRUE;
        case IDOK:
            GetDlgItemTextW(dlg, IDC_CS_FILE, g.cert_file, MAX_PATH);
            GetDlgItemTextW(dlg, IDC_CS_PW, g.cert_pw, 128);
            GetDlgItemTextW(dlg, IDC_CS_REASON, g.cert_reason, 128);
            GetDlgItemTextW(dlg, IDC_CS_LOCATION, g.cert_location, 128);
            if (!g.cert_file[0] || !g.cert_pw[0]) {
                MessageBoxW(dlg, L"Choose your digital ID and type its password.", L"Sign with a Certificate", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Stained Glass\\SG PDF", L"DigitalID", REG_SZ, g.cert_file,
                            (lstrlenW(g.cert_file) + 1) * sizeof(WCHAR));
            EndDialog(dlg, IsDlgButtonChecked(dlg, IDC_CS_PICTURE) == BST_CHECKED ? 2 : IDOK);
            return TRUE;
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

static BOOL g_with_picture;

/* the handwritten signature (Fill & Sign > Sign) as a picture beside the certificate's text: a BMP
 * file sg-pdf reads (ink strokes and typed names are drawn here; a picture is used as it is) */
static BOOL picture_file(WCHAR *out)
{
    WCHAR dir[MAX_PATH];
    HDC dc, mem;
    HBITMAP bmp;
    BITMAPINFO bi = { 0 };
    void *bits;
    int w = 600, h = 200;
    FILE *f;
    if (!g.sig_kind || !g.sig_data) return FALSE;
    if (g.sig_kind == 3) { lstrcpynW(out, g.sig_data, MAX_PATH); return TRUE; }
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"sgs", 0, out);
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    dc = GetDC(NULL);
    mem = CreateCompatibleDC(dc);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!bmp) { DeleteDC(mem); return FALSE; }
    SelectObject(mem, bmp);
    {
        RECT r = { 0, 0, w, h };
        FillRect(mem, &r, GetStockObject(WHITE_BRUSH));
    }
    if (g.sig_kind == 1) {
        /* "x y x y ...;x y ..." normalized to the box */
        HPEN pen = CreatePen(PS_SOLID, 5, RGB(0x0D, 0x1A, 0x66));
        WCHAR *copy = _wcsdup(g.sig_data), *stroke, *ctx = NULL;
        SelectObject(mem, pen);
        for (stroke = copy ? wcstok_s(copy, L";", &ctx) : NULL; stroke; stroke = wcstok_s(NULL, L";", &ctx)) {
            WCHAR *p = stroke, *end;
            int k = 0;
            for (;;) {
                double x = wcstod(p, &end), y;
                if (end == p) break;
                p = end;
                y = wcstod(p, &end);
                if (end == p) break;
                p = end;
                if (k++ == 0) MoveToEx(mem, (int)(x * w), (int)(y * h), NULL);
                else LineTo(mem, (int)(x * w), (int)(y * h));
            }
        }
        free(copy);
        DeleteObject(pen);
    } else {
        HFONT font = CreateFontW(-120, 0, 0, 0, FW_NORMAL, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0,
                                 L"Z003"), of = SelectObject(mem, font);
        RECT r = { 0, 0, w, h };
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(0x0D, 0x1A, 0x66));
        DrawTextW(mem, g.sig_data, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(mem, of);
        DeleteObject(font);
    }
    GdiFlush();
    f = _wfopen(out, L"wb");
    if (f) {
        BITMAPFILEHEADER fh = { 0x4D42 };
        DWORD stride = ((w * 3 + 3) & ~3), size = stride * h;
        fh.bfOffBits = sizeof(fh) + sizeof(BITMAPINFOHEADER);
        fh.bfSize = fh.bfOffBits + size;
        bi.bmiHeader.biSizeImage = size;
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi.bmiHeader, sizeof(BITMAPINFOHEADER), 1, f);
        fwrite(bits, size, 1, f);
        fclose(f);
    }
    DeleteDC(mem);
    DeleteObject(bmp);
    return f != NULL;
}

/* sign and save: field (xref) or a new box at page/r */
static BOOL do_sign(int field, int page, frect r)
{
    WCHAR target[MAX_PATH], pic[MAX_PATH] = L"", *dot;
    char *et, *ef, *ep, *er, *el, *epic = NULL, line[MAX_PATH * 8 + 1024];
    BOOL ok = FALSE;
    lstrcpynW(target, g.path[0] ? g.path : L"Signed.pdf", MAX_PATH);
    dot = wcsrchr(target, L'.');
    if (dot && !_wcsicmp(dot, L".pdf") && wcslen(target) < MAX_PATH - 10) lstrcpyW(dot, L"_signed.pdf");
    if (!file_dialog(TRUE, L"Save the Signed Document As", L"PDF documents (*.pdf)\0*.pdf\0", L"pdf", target, MAX_PATH)) return FALSE;
    if (g_with_picture) picture_file(pic);
    et = unix_esc(target); ef = unix_esc(g.cert_file); ep = wesc(g.cert_pw); er = wesc(g.cert_reason); el = wesc(g.cert_location);
    if (pic[0]) epic = unix_esc(pic);
    if (et && ef && ep && er && el) {
        int n;
        if (field) n = snprintf(line, sizeof(line), "certsign\t%s\t%s\t%s\tfield=%d\tpage=%d\treason=%s\tlocation=%s", et, ef, ep,
                                field, page, er, el);
        else n = snprintf(line, sizeof(line), "certsign\t%s\t%s\t%s\tpage=%d\trect=%.2f %.2f %.2f %.2f\treason=%s\tlocation=%s",
                          et, ef, ep, page, r.x1, r.y1, r.x2, r.y2, er, el);
        if (epic && n > 0 && n < (int)sizeof(line) - 20) snprintf(line + n, sizeof(line) - n, "\tpicture=%s", epic);
#ifndef SG_MUTANT_CERTSIGN
        ok = doc_request(line);
#endif
        SecureZeroMemory(line, sizeof(line));
    }
    if (ep) { SecureZeroMemory(ep, strlen(ep)); free(ep); }
    SecureZeroMemory(g.cert_pw, sizeof(g.cert_pw));
    free(et); free(ef); free(er); free(el); free(epic);
    if (pic[0] && g.sig_kind != 3) DeleteFileW(pic);
    if (ok) {
        WCHAR *base;
        lstrcpynW(g.path, target, MAX_PATH);
        base = wcsrchr(target, L'\\');
        lstrcpynW(g.name, base ? base + 1 : target, MAX_PATH);
        g.untitled = FALSE;
        doc_free_sigs();
        doc_load_sigs();
        app_update_title();
        app_set_status(L"Signed and saved as %ls.", g.name);
        side_update();
    }
    return ok;
}

BOOL dlg_certsign(int field_xref)
{
    INT_PTR r;
    int i, page = 0;
    frect none = { 0, 0, 0, 0 };
    if (!g.npages || !g.bridged) return FALSE;
    tool_commit_editor();
    /* an encrypted document is signed as it is and stays encrypted (sg-pdf derives its key from the
     * password it was opened with); only a security change not yet saved has to be saved first */
    if (g.protect[0] && strcmp(g.protect, "keep")) {
        MessageBoxW(g_main, L"The document's security changes when it is saved. Save it first, then sign it.", L"Sign",
                    MB_OK | MB_ICONINFORMATION);
        return FALSE;
    }
    r = DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_CERTSIGN), g_main, certsign_proc, 0);
    if (r != IDOK && r != 2) { SecureZeroMemory(g.cert_pw, sizeof(g.cert_pw)); return FALSE; }
    g_with_picture = r == 2;
    if (field_xref) {
        for (i = 0; i < g.nfields; i++) if (g.fields[i].xref == field_xref) page = g.fields[i].page;
        return do_sign(field_xref, page, none);
    }
    g.cert_field = 0;
    if (g.tool != TOOL_FILL) tool_set(TOOL_FILL);
    tool_set_sub(SUB_CERTSIGN);
    app_set_status(L"Drag a box where the signature goes (or click for a box of the usual size).");
    return TRUE;
}

BOOL sign_at(int page, frect r)
{
    BOOL ok = do_sign(0, page, r);
    g.sub = SUB_SELECT;
    toolui_update();
    return ok;
}

/* ---- the signature bar ------------------------------------------------------------------------------------------ */

HWND g_sigbar;
static WCHAR g_banner[300];
static int g_banner_level;

int sigbar_height(void)
{
    return g_banner[0] && g.npages ? dpx(32) : 0;
}

void sigbar_update(void)
{
    WCHAR old[300];
    int was = sigbar_height();
    lstrcpynW(old, g_banner, 300);
    if (g.npages && (g.form || g.nsigs)) sig_banner(g_banner, 300, &g_banner_level);
    else g_banner[0] = 0;
    if (was != sigbar_height()) app_layout();
    if (g_sigbar) InvalidateRect(g_sigbar, NULL, FALSE);
}

static LRESULT CALLBACK sigbar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, t;
        COLORREF bg = g_banner_level == 2 ? RGB(0xFD, 0xE7, 0xE9) : g_banner_level == 1 ? RGB(0xFF, 0xF4, 0xCE) : RGB(0xDF, 0xF6, 0xDD);
        COLORREF fg = g_banner_level == 2 ? RGB(0x9A, 0x10, 0x18) : g_banner_level == 1 ? RGB(0x6B, 0x4A, 0x00) : RGB(0x10, 0x5C, 0x20);
        HBRUSH b = CreateSolidBrush(g.dark ? RGB(GetRValue(bg) / 5, GetGValue(bg) / 5, GetBValue(bg) / 5) : bg);
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, b);
        DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g.dark ? RGB(0xF0, 0xF0, 0xF0) : fg);
        SelectObject(dc, g_font_bold);
        t = rc;
        t.left += dpx(14);
        t.right -= dpx(150);
        DrawTextW(dc, g_banner, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, g_font_small);
        t = rc;
        t.right -= dpx(14);
        SetTextColor(dc, g.dark ? RGB(0x9C, 0xC8, 0xFF) : RGB(0x1A, 0x4F, 0xC0));
        DrawTextW(dc, L"Signature Panel", -1, &t, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONUP:
        side_set_mode(SIDE_SIGS);
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void sigbar_create(HWND parent)
{
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = sigbar_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
    wc.lpszClassName = L"SgPdfSigBar";
    RegisterClassW(&wc);
    g_sigbar = CreateWindowExW(0, L"SgPdfSigBar", NULL, WS_CHILD, 0, 0, 10, 10, parent, NULL, g_inst, NULL);
}
