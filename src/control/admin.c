/* sg-control -- the elevated half: consent, the administration spool, and
 * the dialogs an administrator uses to change the machine.
 *
 * Changing the computer's name, its domain, its accounts or its time zone is
 * an administrator's act. The Control Panel asks for it the Windows way --
 * ShellExecute "runas" on itself, which wine-sg sends to the elevation broker
 * and its consent prompt (ADR 0012) -- and the elevated copy (SYSTEM) files
 * the request with sg-admind, which alone does root's part. See
 * admin/sg-admind for the spool and the list of operations.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"
#include <shellapi.h>

/* ---- elevation ----------------------------------------------------------------- */
BOOL run_elevated(const WCHAR *args)
{
    WCHAR self[MAX_PATH];
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    GetModuleFileNameW(NULL, self, MAX_PATH);
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = g_main;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei);
}

BOOL is_elevated(void)
{
    HANDLE tok;
    char buf[256];
    DWORD len = sizeof(buf);
    BOOL sys = FALSE, admin = FALSE;
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD n = sizeof(sid);
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenUser, buf, len, &len))
            sys = IsWellKnownSid(((TOKEN_USER *)buf)->User.Sid, WinLocalSystemSid);
        CloseHandle(tok);
    }
    if (CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, sid, &n)) CheckTokenMembership(NULL, sid, &admin);
    return sys || admin;
}

/* ---- the spool -------------------------------------------------------------------- */
static void spool_dir(const char *sub, WCHAR *out, int cch)
{
    char unix_path[512];
    WCHAR env[400];
    char base[400] = "/run/stained-glass-admin";
    /* the gate's own spool; the broker passes no such variable to elevated programs */
    if (GetEnvironmentVariableW(L"SG_ADMIN_SPOOL", env, ARRAYSIZE(env)))
        WideCharToMultiByte(CP_UTF8, 0, env, -1, base, sizeof(base), NULL, NULL);
    _snprintf(unix_path, sizeof(unix_path), "%s/%s", base, sub);
    unix_path[sizeof(unix_path) - 1] = 0;
    unix_to_dos(unix_path, out, cch);
}

typedef BOOLEAN (WINAPI *rtlgenrandom_t)(PVOID, ULONG);

static BOOL random_id(WCHAR *out)
{
    BYTE b[16];
    int i;
    rtlgenrandom_t gen = (rtlgenrandom_t)(void *)GetProcAddress(LoadLibraryW(L"advapi32.dll"), "SystemFunction036");
    if (!gen || !gen(b, sizeof(b))) return FALSE;
    for (i = 0; i < 16; i++) _snwprintf(out + i * 2, 3, L"%02x", b[i]);
    out[32] = 0;
    return TRUE;
}

/* wait for a file to appear, keeping this thread's windows alive */
static BOOL wait_for(const WCHAR *path, DWORD timeout_ms)
{
    DWORD start = GetTickCount();
    while (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        MSG msg;
        if (GetTickCount() - start > timeout_ms) return FALSE;
        MsgWaitForMultipleObjects(0, NULL, FALSE, 100, QS_ALLINPUT);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); return FALSE; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return TRUE;
}

BOOL admin_request(const WCHAR *const *fields, int n, WCHAR *msg, int cch, DWORD timeout_ms)
{
    WCHAR dir[MAX_PATH], tmp[MAX_PATH], req[MAX_PATH], rep[MAX_PATH], id[40];
    char *body = NULL;
    size_t len = 0, cap = 0;
    HANDLE h;
    DWORD written;
    int i;
    BOOL ok = FALSE;

    msg[0] = 0;
    if (!random_id(id)) { lstrcpynW(msg, L"The request could not be made.", cch); return FALSE; }
    for (i = 0; i < n; i++) {                       /* UTF-8, one field per line */
        int need = WideCharToMultiByte(CP_UTF8, 0, fields[i], -1, NULL, 0, NULL, NULL);
        if (wcspbrk(fields[i], L"\r\n")) { lstrcpynW(msg, L"A value cannot contain a line break.", cch); goto out; }
        if (len + need + 1 > cap) {
            char *nb;
            cap = (len + need + 1) * 2;
            if (!(nb = realloc(body, cap))) goto out;
            body = nb;
        }
        WideCharToMultiByte(CP_UTF8, 0, fields[i], -1, body + len, need, NULL, NULL);
        len += need - 1;
        body[len++] = '\n';
    }

    spool_dir("requests", dir, MAX_PATH);
    _snwprintf(tmp, MAX_PATH, L"%ls\\.%ls", dir, id);
    _snwprintf(req, MAX_PATH, L"%ls\\%ls.req", dir, id);
    spool_dir("replies", dir, MAX_PATH);
    _snwprintf(rep, MAX_PATH, L"%ls\\%ls.rep", dir, id);

    /* written under another name and renamed: whole when it appears */
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        lstrcpynW(msg, is_elevated() ? L"The administration service is not available."
                                     : L"You need to be an administrator to make this change.", cch);
        goto out;
    }
    ok = WriteFile(h, body, (DWORD)len, &written, NULL) && written == len;
    CloseHandle(h);
    SecureZeroMemory(body, cap);
    if (!ok || !MoveFileExW(tmp, req, 0)) {
        DeleteFileW(tmp);
        lstrcpynW(msg, L"The request could not be made.", cch);
        ok = FALSE;
        goto out;
    }
    ok = FALSE;
    if (!wait_for(rep, timeout_ms)) {
        lstrcpynW(msg, L"The change did not complete in time.", cch);
        goto out;
    }
    {
        HANDLE r = CreateFileW(rep, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        char buf[4096];
        DWORD got = 0;
        WCHAR wide[4096], *nl;
        if (r == INVALID_HANDLE_VALUE) { lstrcpynW(msg, L"The answer could not be read.", cch); goto out; }
        ReadFile(r, buf, sizeof(buf) - 1, &got, NULL);
        CloseHandle(r);
        buf[got] = 0;
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, wide, ARRAYSIZE(wide));
        if (!wcsncmp(wide, L"OK", 2) && (wide[2] == L'\n' || !wide[2])) {
            ok = TRUE;
            if (wide[2] == L'\n') { lstrcpynW(msg, wide + 3, cch); if ((nl = wcschr(msg, L'\n'))) *nl = 0; }
        } else if (!wcsncmp(wide, L"FAILED ", 7)) {
            lstrcpynW(msg, wide + 7, cch);
            if ((nl = wcschr(msg, L'\n'))) *nl = 0;
        } else lstrcpynW(msg, L"The administration service gave no answer.", cch);
    }
out:
    if (body) { SecureZeroMemory(body, cap); free(body); }
    return ok;
}

/* ---- small modal forms ---------------------------------------------------------------- */
#define FORM_FIRST 100
struct form { struct form_field *f; int n; HWND *ctl; BOOL done, ok; };

static LRESULT CALLBACK form_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct form *fm = (struct form *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        return 0;
    case DM_GETDEFID: return MAKELONG(IDOK, DC_HASDEFID);
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_COMMAND:
        if (fm && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) {
            int i;
            fm->ok = LOWORD(wp) == IDOK;
            if (fm->ok)
                for (i = 0; i < fm->n; i++) {
                    struct form_field *f = &fm->f[i];
                    HWND c = fm->ctl[i];
                    if (!c || !f->value) continue;
                    switch (f->kind) {
                    case FF_TEXT: case FF_PASSWORD: GetWindowTextW(c, f->value, f->cch); break;
                    case FF_RADIO_FIRST: case FF_RADIO: case FF_CHECK:
                        lstrcpynW(f->value, SendMessageW(c, BM_GETCHECK, 0, 0) == BST_CHECKED ? L"1" : L"0", f->cch);
                        break;
                    case FF_COMBO: {
                        LRESULT sel = SendMessageW(c, CB_GETCURSEL, 0, 0);
                        if (sel >= 0 && sel < f->nopt) lstrcpynW(f->value, f->options[sel], f->cch);
                        break;
                    }
                    }
                }
            fm->done = TRUE;
            return 0;
        }
        break;
    case WM_CLOSE: if (fm) { fm->ok = FALSE; fm->done = TRUE; } return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

BOOL run_form(HWND owner, const WCHAR *title, const WCHAR *intro, struct form_field *f, int n,
              const WCHAR *ok_label, BOOL shield)
{
    static BOOL registered;
    struct form fm = { f, n, NULL, FALSE, FALSE };
    int w = S(480), x = S(24), y = S(20), cw = w - S(48), i;
    HWND hwnd, focus = NULL;
    HDC dc;
    RECT r;
    MSG msg;
    (void)shield;

    if (!registered) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = form_proc; wc.hInstance = g_inst; wc.lpszClassName = L"SgCplForm";
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
        RegisterClassW(&wc);
        registered = TRUE;
    }
    if (!(fm.ctl = calloc(n, sizeof(HWND)))) return FALSE;
    hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"SgCplForm", title,
                           WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, w, S(200),
                           owner, NULL, g_inst, &fm);
    if (!hwnd) { free(fm.ctl); return FALSE; }

#define ADD(cls, text, style, X, Y, W, H, ID) do { \
        HWND c_ = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | (style), X, Y, W, H, hwnd, (HMENU)(INT_PTR)(ID), g_inst, NULL); \
        SendMessageW(c_, WM_SETFONT, (WPARAM)g_font_body, TRUE); last = c_; } while (0)
    {
        HWND last = NULL;
        dc = GetDC(hwnd);
        SelectObject(dc, g_font_body);
        if (intro) {
            SetRect(&r, 0, 0, cw, 0);
            DrawTextW(dc, intro, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            ADD(L"STATIC", intro, SS_NOPREFIX, x, y, cw, r.bottom, -1);
            y += r.bottom + S(16);
        }
        for (i = 0; i < n; i++) {
            struct form_field *ff = &f[i];
            switch (ff->kind) {
            case FF_NOTE:
                SetRect(&r, 0, 0, cw, 0);
                DrawTextW(dc, ff->label, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                ADD(L"STATIC", ff->label, SS_NOPREFIX, x, y, cw, r.bottom, -1);
                y += r.bottom + S(12);
                break;
            case FF_TEXT: case FF_PASSWORD:
                ADD(L"STATIC", ff->label, SS_NOPREFIX, x, y, cw, S(18), -1);
                y += S(20);
                ADD(L"EDIT", ff->value ? ff->value : L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL |
                    (ff->kind == FF_PASSWORD ? ES_PASSWORD : 0), x, y, cw, S(26), FORM_FIRST + i);
                if (ff->cch) SendMessageW(last, EM_LIMITTEXT, ff->cch - 1, 0);
                fm.ctl[i] = last;
                if (!focus) focus = last;
                y += S(36);
                break;
            case FF_RADIO_FIRST: case FF_RADIO: case FF_CHECK:
                ADD(L"BUTTON", ff->label, WS_TABSTOP | (ff->kind == FF_CHECK ? BS_AUTOCHECKBOX : BS_AUTORADIOBUTTON) |
                    (ff->kind == FF_RADIO_FIRST ? WS_GROUP : 0), x, y, cw, S(24), FORM_FIRST + i);
                if (ff->value && ff->value[0] == L'1') SendMessageW(last, BM_SETCHECK, BST_CHECKED, 0);
                fm.ctl[i] = last;
                y += S(28);
                if (i + 1 >= n || (f[i + 1].kind != FF_RADIO && ff->kind != FF_CHECK)) y += S(8);
                break;
            case FF_COMBO: {
                int j;
                ADD(L"STATIC", ff->label, SS_NOPREFIX, x, y, cw, S(18), -1);
                y += S(20);
                ADD(L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, x, y, cw, S(320), FORM_FIRST + i);
                for (j = 0; j < ff->nopt; j++) {
                    SendMessageW(last, CB_ADDSTRING, 0, (LPARAM)ff->options[j]);
                    if (ff->value && !lstrcmpW(ff->value, ff->options[j])) SendMessageW(last, CB_SETCURSEL, j, 0);
                }
                fm.ctl[i] = last;
                if (!focus) focus = last;
                y += S(40);
                break;
            }
            }
        }
        ReleaseDC(hwnd, dc);
        y += S(8);
        ADD(L"BUTTON", ok_label ? ok_label : L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, w - S(24) - S(196) - S(8), y, S(100), S(30), IDOK);
        ADD(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, w - S(24) - S(96), y, S(96), S(30), IDCANCEL);
        y += S(30) + S(20);
    }
#undef ADD
    /* size the window to its content, centred on the owner or the screen */
    SetRect(&r, 0, 0, w, y);
    AdjustWindowRectEx(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    {
        RECT o;
        int ww = r.right - r.left, hh = r.bottom - r.top;
        if (owner && GetWindowRect(owner, &o)) ;
        else SystemParametersInfoW(SPI_GETWORKAREA, 0, &o, 0);
        SetWindowPos(hwnd, HWND_TOP, o.left + (o.right - o.left - ww) / 2, o.top + (o.bottom - o.top - hh) / 3, ww, hh, 0);
    }
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(focus ? focus : GetDlgItem(hwnd, IDOK));
    while (!fm.done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    /* scrub what was typed into password boxes before the window goes */
    for (i = 0; i < n; i++) if (f[i].kind == FF_PASSWORD && fm.ctl[i]) SetWindowTextW(fm.ctl[i], L"");
    DestroyWindow(hwnd);
    free(fm.ctl);
    return fm.ok;
}

void message(HWND owner, const WCHAR *title, const WCHAR *text, BOOL error)
{
    MessageBoxW(owner, text, title, MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION) | (owner ? 0 : MB_SETFOREGROUND));
}

/* a "please wait" window while a long change runs */
static HWND busy(const WCHAR *title, const WCHAR *text)
{
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, L"STATIC", title, WS_POPUP | WS_CAPTION | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, S(420), S(110), NULL, NULL, g_inst, NULL);
    HWND t = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_CENTER, S(16), S(26), S(388), S(40), w, NULL, g_inst, NULL);
    RECT wa;
    SendMessageW(t, WM_SETFONT, (WPARAM)g_font_body, TRUE);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(w, HWND_TOPMOST, (wa.right - S(420)) / 2, (wa.bottom - S(110)) / 3, 0, 0, SWP_NOSIZE);
    UpdateWindow(w);
    return w;
}

/* ---- the elevated dialogs ------------------------------------------------------------- */
static BOOL valid_computer_name(const WCHAR *s)
{
    int i, n = lstrlenW(s);
    if (n < 1 || n > 15 || s[0] == L'-' || s[n - 1] == L'-') return FALSE;
    for (i = 0; i < n; i++)
        if (!((s[i] >= L'a' && s[i] <= L'z') || (s[i] >= L'A' && s[i] <= L'Z') || (s[i] >= L'0' && s[i] <= L'9') || s[i] == L'-'))
            return FALSE;
    return TRUE;
}

static BOOL valid_account_name(const WCHAR *s)
{
    int i, n = lstrlenW(s);
    if (n < 1 || n > 32 || !((s[0] >= L'a' && s[0] <= L'z') || s[0] == L'_')) return FALSE;
    for (i = 1; i < n; i++)
        if (!((s[i] >= L'a' && s[i] <= L'z') || (s[i] >= L'0' && s[i] <= L'9') || s[i] == L'-' || s[i] == L'_')) return FALSE;
    return TRUE;
}

static void offer_restart(const WCHAR *why)
{
    WCHAR text[512];
    _snwprintf(text, ARRAYSIZE(text), L"%ls\n\nYou must restart your computer to apply these changes. Restart now?", why);
    text[ARRAYSIZE(text) - 1] = 0;
    if (MessageBoxW(NULL, text, L"Restart your computer", MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES)
        ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_FLAG_PLANNED);
}

/* Settings > About > Rename this PC: Windows 10's "Rename your PC" -- one
 * name, then a restart offer. */
static int do_rename_pc(void)
{
    WCHAR role[32], realm[128], domain[64], name[64] = L"", old[64] = L"", err[512] = L"", msg[512];
    DWORD n = ARRAYSIZE(old);
    machine_role(role, realm, domain, ARRAYSIZE(realm));
    if (role[0]) {
        _snwprintf(msg, ARRAYSIZE(msg), L"This PC is %ls the %ls domain. Its name is managed through the domain.",
                   !lstrcmpW(role, L"dc") ? L"the domain controller of" : L"a member of", realm);
        message(NULL, L"Rename your PC", msg, FALSE);
        return 0;
    }
    GetComputerNameExW(ComputerNamePhysicalDnsHostname, old, &n);
    for (;;) {
        struct form_field f[] = {
            { L"Current PC name:", NULL, 0, FF_NOTE },
            { old, NULL, 0, FF_NOTE },
            { L"New name:", name, 16, FF_TEXT },
        };
        if (!run_form(NULL, L"Rename your PC", err[0] ? err :
                      L"You can use a combination of letters, hyphens, and numbers.", f, ARRAYSIZE(f), L"Next", TRUE))
            return 1;
        if (!valid_computer_name(name)) {
            lstrcpyW(err, L"The name can have up to 15 letters, numbers and hyphens, and cannot start or end with a hyphen.");
            continue;
        }
        if (!lstrcmpiW(name, old)) return 0;
        {
            const WCHAR *req[] = { L"hostname", name };
            if (!admin_request(req, 2, msg, ARRAYSIZE(msg), 60000)) { message(NULL, L"Rename your PC", msg, TRUE); continue; }
        }
        _snwprintf(msg, ARRAYSIZE(msg), L"Your PC will be renamed to %ls after it restarts.", name);
        offer_restart(msg);
        return 0;
    }
}

static int do_rename(void)
{
    WCHAR role[32], realm[128], domain[64], name[64] = L"", err[512] = L"", msg[512];
    WCHAR workgroup[2] = L"1", dom[2] = L"0", realm_in[128] = L"";
    DWORD n = ARRAYSIZE(name);
    machine_role(role, realm, domain, ARRAYSIZE(realm));
    if (role[0]) {
        _snwprintf(msg, ARRAYSIZE(msg), L"This computer is %ls the %ls domain. Its name and membership are managed "
                   L"through the domain.", !lstrcmpW(role, L"dc") ? L"the domain controller of" : L"a member of", realm);
        message(NULL, L"Computer Name/Domain Changes", msg, FALSE);
        return 0;
    }
    GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &n);
    for (;;) {
        struct form_field f[] = {
            { L"Computer name:", name, 16, FF_TEXT },
            { L"Member of:", NULL, 0, FF_NOTE },
            { L"Workgroup (this computer is not part of a domain)", workgroup, 2, FF_RADIO_FIRST },
            { L"Domain", dom, 2, FF_RADIO },
            { L"Domain name (for example, corp.example.com):", realm_in, ARRAYSIZE(realm_in), FF_TEXT },
        };
        WCHAR old[64];
        DWORD on = ARRAYSIZE(old);
        GetComputerNameExW(ComputerNamePhysicalDnsHostname, old, &on);
        if (!run_form(NULL, L"Computer Name/Domain Changes", err[0] ? err :
                      L"You can change the name and the membership of this computer. Changes might affect access to network resources.",
                      f, ARRAYSIZE(f), L"OK", TRUE))
            return 1;
        if (!valid_computer_name(name)) {
            lstrcpyW(err, L"The computer name can have up to 15 letters, numbers and hyphens, and cannot start or end with a hyphen.");
            continue;
        }
        if (dom[0] == L'1' && !realm_in[0]) { lstrcpyW(err, L"Type the name of the domain to join."); continue; }
        if (lstrcmpiW(name, old)) {
            const WCHAR *req[] = { L"hostname", name };
            if (!admin_request(req, 2, msg, ARRAYSIZE(msg), 60000)) { message(NULL, L"Computer Name/Domain Changes", msg, TRUE); continue; }
        }
        if (dom[0] == L'1') {
            WCHAR user[65] = L"administrator", pw[257] = L"", dc[32] = L"";
            struct form_field c[] = {
                { L"Enter the name and password of an account with permission to join the domain.", NULL, 0, FF_NOTE },
                { L"User name:", user, ARRAYSIZE(user), FF_TEXT },
                { L"Password:", pw, ARRAYSIZE(pw), FF_PASSWORD },
                { L"Domain controller address (optional, found through DNS if empty):", dc, ARRAYSIZE(dc), FF_TEXT },
            };
            HWND wait;
            BOOL ok;
            if (!run_form(NULL, L"Security", NULL, c, ARRAYSIZE(c), L"OK", FALSE)) { SecureZeroMemory(pw, sizeof(pw)); return 1; }
            {
                const WCHAR *req[] = { L"join-domain", realm_in, user, dc, pw };
                wait = busy(L"Computer Name/Domain Changes", L"Joining the domain. This can take a minute...");
                ok = admin_request(req, 5, msg, ARRAYSIZE(msg), 11 * 60 * 1000);
                DestroyWindow(wait);
            }
            SecureZeroMemory(pw, sizeof(pw));
            if (!ok) { message(NULL, L"Computer Name/Domain Changes", msg, TRUE); continue; }
            _snwprintf(msg, ARRAYSIZE(msg), L"Welcome to the %ls domain.", realm_in);
            offer_restart(msg);
            return 0;
        }
        if (lstrcmpiW(name, old)) offer_restart(L"The computer has been renamed.");
        return 0;
    }
}

static int do_user_add(void)
{
    WCHAR name[33] = L"", full[65] = L"", pw[257] = L"", pw2[257] = L"", std[2] = L"1", adm[2] = L"0", err[256] = L"", msg[512];
    for (;;) {
        struct form_field f[] = {
            { L"User name:", name, ARRAYSIZE(name), FF_TEXT },
            { L"Full name (optional):", full, ARRAYSIZE(full), FF_TEXT },
            { L"Password:", pw, ARRAYSIZE(pw), FF_PASSWORD },
            { L"Confirm password:", pw2, ARRAYSIZE(pw2), FF_PASSWORD },
            { L"Standard user: can use most programs and change settings that do not affect other users.", std, 2, FF_RADIO_FIRST },
            { L"Administrator: has complete control of this computer.", adm, 2, FF_RADIO },
        };
        BOOL ok;
        if (!run_form(NULL, L"Create an account", err[0] ? err : L"Create an account for someone who uses this computer.",
                      f, ARRAYSIZE(f), L"Create Account", TRUE))
            break;
        if (!valid_account_name(name)) { lstrcpyW(err, L"The user name must start with a lower-case letter and use only lower-case letters, numbers, - and _."); continue; }
        if (!pw[0]) { lstrcpyW(err, L"Type a password for the new account."); continue; }
        if (lstrcmpW(pw, pw2)) { lstrcpyW(err, L"The passwords do not match."); pw[0] = pw2[0] = 0; continue; }
        {
            const WCHAR *req[] = { L"user-add", name, full, adm[0] == L'1' ? L"administrator" : L"standard", pw };
            ok = admin_request(req, 5, msg, ARRAYSIZE(msg), 60000);
        }
        if (!ok) { lstrcpynW(err, msg, ARRAYSIZE(err)); continue; }
        break;
    }
    SecureZeroMemory(pw, sizeof(pw)); SecureZeroMemory(pw2, sizeof(pw2));
    return 0;
}

static int do_user_type(const WCHAR *name)
{
    WCHAR std[2], adm[2], title[128], msg[512];
    BOOL is_admin = unix_group_has("sg-admins", name);
    lstrcpyW(std, is_admin ? L"0" : L"1"); lstrcpyW(adm, is_admin ? L"1" : L"0");
    _snwprintf(title, ARRAYSIZE(title), L"Change account type for %ls", name);
    {
        struct form_field f[] = {
            { L"Standard: can use most programs and change settings that do not affect other users.", std, 2, FF_RADIO_FIRST },
            { L"Administrator: has complete control of this computer.", adm, 2, FF_RADIO },
        };
        const WCHAR *req[] = { L"user-type", name, NULL };
        if (!run_form(NULL, title, L"Choose a new account type.", f, ARRAYSIZE(f), L"Change Account Type", TRUE)) return 1;
        req[2] = adm[0] == L'1' ? L"administrator" : L"standard";
        if (!admin_request(req, 3, msg, ARRAYSIZE(msg), 60000)) { message(NULL, title, msg, TRUE); return 1; }
        message(NULL, title, L"The account type has been changed. It takes effect the next time the user signs in.", FALSE);
    }
    return 0;
}

static int do_user_password(const WCHAR *name)
{
    WCHAR pw[257] = L"", pw2[257] = L"", title[128], err[256] = L"", msg[512];
    _snwprintf(title, ARRAYSIZE(title), L"Change %ls's password", name);
    for (;;) {
        struct form_field f[] = {
            { L"New password:", pw, ARRAYSIZE(pw), FF_PASSWORD },
            { L"Confirm new password:", pw2, ARRAYSIZE(pw2), FF_PASSWORD },
        };
        BOOL ok;
        if (!run_form(NULL, title, err[0] ? err : L"The new password takes effect immediately.", f, ARRAYSIZE(f), L"Change Password", TRUE)) break;
        if (!pw[0]) { lstrcpyW(err, L"Type a new password."); continue; }
        if (lstrcmpW(pw, pw2)) { lstrcpyW(err, L"The passwords do not match."); pw[0] = pw2[0] = 0; continue; }
        {
            const WCHAR *req[] = { L"user-password", name, pw };
            ok = admin_request(req, 3, msg, ARRAYSIZE(msg), 60000);
        }
        if (!ok) { lstrcpynW(err, msg, ARRAYSIZE(err)); pw[0] = pw2[0] = 0; continue; }
        break;
    }
    SecureZeroMemory(pw, sizeof(pw)); SecureZeroMemory(pw2, sizeof(pw2));
    return 0;
}

static int do_user_remove(const WCHAR *name)
{
    WCHAR del[2] = L"0", keep[2] = L"1", title[128], intro[512], msg[512];
    _snwprintf(title, ARRAYSIZE(title), L"Delete %ls's account?", name);
    _snwprintf(intro, ARRAYSIZE(intro), L"Before you delete %ls's account, choose whether to keep the files in their "
               L"home folder. The account cannot be recovered once it is deleted.", name);
    {
        struct form_field f[] = {
            { L"Keep files: the account is removed, its home folder stays.", keep, 2, FF_RADIO_FIRST },
            { L"Delete files: the account and everything in its home folder are removed.", del, 2, FF_RADIO },
        };
        const WCHAR *req[] = { L"user-remove", name, NULL };
        if (!run_form(NULL, title, intro, f, ARRAYSIZE(f), L"Delete Account", TRUE)) return 1;
        req[2] = del[0] == L'1' ? L"delete" : L"keep";
        if (!admin_request(req, 3, msg, ARRAYSIZE(msg), 120000)) { message(NULL, L"Delete Account", msg, TRUE); return 1; }
    }
    return 0;
}

/* the IANA zones, from tzdata's own table */
int load_zones(WCHAR ***out)
{
    char *text = read_unix_file("/usr/share/zoneinfo/zone1970.tab", NULL), *line, *next;
    WCHAR **z = NULL;
    int n = 0, cap = 0;
    if (!text) return 0;
    for (line = text; line && *line; line = next) {
        char *f[4] = { 0 }, *p = line;
        int k = 0;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (line[0] == '#') continue;
        while (k < 4 && p) { f[k++] = p; p = strchr(p, '\t'); if (p) *p++ = 0; }
        if (k < 3) continue;
        if (n == cap) { WCHAR **nz = realloc(z, (cap = cap ? cap * 2 : 256) * sizeof(*z)); if (!nz) break; z = nz; }
        z[n] = malloc(128 * sizeof(WCHAR));
        MultiByteToWideChar(CP_UTF8, 0, f[2], -1, z[n], 128);
        n++;
    }
    free(text);
    if (n == cap) { WCHAR **nz = realloc(z, (cap + 1) * sizeof(*z)); if (!nz) { *out = z; return n; } z = nz; }
    z[n++] = _wcsdup(L"UTC");
    {
        int i, j;
        for (i = 1; i < n; i++) {           /* insertion sort: a few hundred names */
            WCHAR *t = z[i];
            for (j = i - 1; j >= 0 && lstrcmpW(z[j], t) > 0; j--) z[j + 1] = z[j];
            z[j + 1] = t;
        }
    }
    *out = z;
    return n;
}

/* The IANA zone: what /etc/localtime links to (the zone systemd set), or
 * Debian's /etc/timezone. Wine resolves the link for us. */
/* The zone's name as the list has it: Debian's zoneinfo/UTC is a link to
 * Etc/UTC (and UCT, Universal, Zulu are more names for it), which is not in
 * zone1970.tab -- the list offers "UTC". */
static void zone_canonical(WCHAR *z)
{
    static const WCHAR *const utc[] = { L"Etc/UTC", L"Etc/UCT", L"Etc/Universal", L"Etc/Zulu",
                                        L"UCT", L"Universal", L"Zulu", L"Etc/GMT", L"Etc/GMT0",
                                        L"Etc/GMT+0", L"Etc/GMT-0", L"Etc/Greenwich", L"GMT" };
    int i;
    for (i = 0; i < (int)ARRAYSIZE(utc); i++)
        if (!lstrcmpW(z, utc[i])) { lstrcpyW(z, L"UTC"); return; }
}

static void current_zone_raw(WCHAR *out, int cch);

void current_zone(WCHAR *out, int cch)
{
    current_zone_raw(out, cch);
    if (cch >= 4) zone_canonical(out);
}

static void current_zone_raw(WCHAR *out, int cch)
{
    WCHAR path[MAX_PATH], final[MAX_PATH], *z;
    HANDLE h;
    char *t, *nl, lt[MAX_PATH] = "/etc/localtime";
    out[0] = 0;
    /* SG_SETTINGS_LOCALTIME: another localtime link, for the gate */
    if (GetEnvironmentVariableA("SG_SETTINGS_LOCALTIME", lt, sizeof(lt)) && lt[0] != '/') strcpy(lt, "/etc/localtime");
    unix_to_dos(lt, path, MAX_PATH);
    h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        if (GetFinalPathNameByHandleW(h, final, MAX_PATH, 0) && (z = wcsstr(final, L"\\zoneinfo\\"))) {
            WCHAR *c;
            lstrcpynW(out, z + 10, cch);
            for (c = out; *c; c++) if (*c == L'\\') *c = L'/';
        }
        CloseHandle(h);
        if (out[0]) return;
    }
    if (!(t = read_unix_file("/etc/timezone", NULL))) return;
    if ((nl = strchr(t, '\n'))) *nl = 0;
    MultiByteToWideChar(CP_UTF8, 0, t, -1, out, cch);
    free(t);
}

BOOL ntp_enabled(void)
{
    return unix_path_exists("/etc/systemd/system/sysinit.target.wants/systemd-timesyncd.service");
}

static int do_timezone(void)
{
    WCHAR **zones = NULL, zone[128], was[128], ntp[2], msg[512];
    int n = load_zones(&zones), i;
    BOOL had_ntp = ntp_enabled();
    current_zone(was, ARRAYSIZE(was));
    lstrcpyW(zone, was);
    lstrcpyW(ntp, had_ntp ? L"1" : L"0");
    {
        struct form_field f[] = {
            { L"Time zone:", zone, ARRAYSIZE(zone), FF_COMBO, (const WCHAR *const *)zones, n },
            { L"Synchronize the clock with an Internet time server", ntp, 2, FF_CHECK },
        };
        if (run_form(NULL, L"Time Zone Settings", L"Choose the time zone this computer's clock shows.", f, ARRAYSIZE(f), L"OK", TRUE)) {
            if (zone[0] && lstrcmpW(zone, was)) {
                const WCHAR *req[] = { L"timezone", zone };
                if (!admin_request(req, 2, msg, ARRAYSIZE(msg), 60000)) message(NULL, L"Time Zone Settings", msg, TRUE);
            }
            if ((ntp[0] == L'1') != had_ntp) {
                const WCHAR *req[] = { L"ntp", ntp[0] == L'1' ? L"on" : L"off" };
                if (!admin_request(req, 2, msg, ARRAYSIZE(msg), 60000)) message(NULL, L"Time Zone Settings", msg, TRUE);
            }
        }
    }
    for (i = 0; i < n; i++) free(zones[i]);
    free(zones);
    return 0;
}

static int do_update_check(void)
{
    WCHAR msg[512];
    const WCHAR *req[] = { L"update-check" };
    if (!admin_request(req, 1, msg, ARRAYSIZE(msg), 60000)) { message(NULL, L"Updates", msg, TRUE); return 1; }
    message(NULL, L"Updates", L"Checking for updates. Updates that are found are downloaded now and installed "
            L"the next time you restart your computer.", FALSE);
    return 0;
}

/* Settings > Date & time: one change each, no form -- the page chose already */
static int do_one(const WCHAR *title, const WCHAR *const *req, int n)
{
    WCHAR msg[512];
    if (admin_request(req, n, msg, ARRAYSIZE(msg), 60000)) return 0;
    message(NULL, title, msg, TRUE);
    return 1;
}

/* "Change the date and time": the form Windows 10 shows, then sg-admind's time */
static int do_set_time(void)
{
    SYSTEMTIME now;
    WCHAR date[16], clock[16], msg[512];
    GetLocalTime(&now);
    _snwprintf(date, ARRAYSIZE(date), L"%04d-%02d-%02d", now.wYear, now.wMonth, now.wDay);
    _snwprintf(clock, ARRAYSIZE(clock), L"%02d:%02d", now.wHour, now.wMinute);
    for (;;) {
        struct form_field f[] = {
            { L"Date (year-month-day):", date, ARRAYSIZE(date), FF_TEXT },
            { L"Time (hours:minutes, 24-hour):", clock, ARRAYSIZE(clock), FF_TEXT },
        };
        int y, mo, d, h, mi;
        WCHAR when[40];
        if (!run_form(NULL, L"Change date and time", NULL, f, ARRAYSIZE(f), L"Change", TRUE)) return 1;
        if (swscanf(date, L"%d-%d-%d", &y, &mo, &d) != 3 || swscanf(clock, L"%d:%d", &h, &mi) != 2 ||
            mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59) {
            message(NULL, L"Change date and time", L"Type the date as year-month-day and the time as hours:minutes.", TRUE);
            continue;
        }
        _snwprintf(when, ARRAYSIZE(when), L"%04d-%02d-%02d %02d:%02d:00", y, mo, d, h, mi);
        {
            const WCHAR *req[] = { L"time", when };
            if (admin_request(req, 2, msg, ARRAYSIZE(msg), 60000)) return 0;
        }
        message(NULL, L"Change date and time", msg, TRUE);
    }
}

int admin_main(int argc, WCHAR **argv)
{
    if (argc < 1) return 2;
    if (!lstrcmpW(argv[0], L"rename")) return do_rename();
    if (!lstrcmpW(argv[0], L"rename-pc")) return do_rename_pc();
    if (!lstrcmpW(argv[0], L"user-add")) return do_user_add();
    if (!lstrcmpW(argv[0], L"user-type") && argc > 1) return do_user_type(argv[1]);
    if (!lstrcmpW(argv[0], L"user-password") && argc > 1) return do_user_password(argv[1]);
    if (!lstrcmpW(argv[0], L"user-remove") && argc > 1) return do_user_remove(argv[1]);
    if (!lstrcmpW(argv[0], L"timezone")) return do_timezone();
    if (!lstrcmpW(argv[0], L"update-check")) return do_update_check();
    if (!lstrcmpW(argv[0], L"set-zone") && argc > 1) {
        const WCHAR *req[] = { L"timezone", argv[1] };
        return do_one(L"Time zone", req, 2);
    }
    if (!lstrcmpW(argv[0], L"ntp") && argc > 1) {
        const WCHAR *req[] = { L"ntp", argv[1] };
        return do_one(L"Set time automatically", req, 2);
    }
    if (!lstrcmpW(argv[0], L"set-time")) return do_set_time();
    if (!lstrcmpW(argv[0], L"consent") && argc > 2) return privacy_admin_consent(argv[1], argv[2]);
    return 2;
}

/* /admin-do VERB ARGS...: the same requests without dialogs, for scripts and
 * the gate. A password, where the verb takes one, is read from stdin. */
int admin_do(int argc, WCHAR **argv)
{
    static const WCHAR *const secret[] = { L"join-domain", L"user-add", L"user-password" };
    const WCHAR *fields[8];
    WCHAR pw[257] = L"", msg[512];
    int n = 0, i;
    BOOL ok, needs_pw = FALSE;
    if (argc < 1 || argc > 7) return 2;
    for (i = 0; i < (int)ARRAYSIZE(secret); i++) if (!lstrcmpW(argv[0], secret[i])) needs_pw = TRUE;
    for (i = 0; i < argc; i++) fields[n++] = argv[i];
    if (needs_pw) {
        char buf[512];
        DWORD got = 0;
        char *nl;
        ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, sizeof(buf) - 1, &got, NULL);
        buf[got] = 0;
        if ((nl = strpbrk(buf, "\r\n"))) *nl = 0;
        MultiByteToWideChar(CP_UTF8, 0, buf, -1, pw, ARRAYSIZE(pw));
        SecureZeroMemory(buf, sizeof(buf));
        fields[n++] = pw;
    }
    ok = admin_request(fields, n, msg, ARRAYSIZE(msg), !lstrcmpW(argv[0], L"join-domain") ? 11 * 60 * 1000 : 60000);
    SecureZeroMemory(pw, sizeof(pw));
    wprintf(ok ? L"OK%ls%ls\n" : L"FAILED %ls%ls\n", ok && msg[0] ? L" " : L"", msg);
    fflush(stdout);
    return ok ? 0 : 1;
}
