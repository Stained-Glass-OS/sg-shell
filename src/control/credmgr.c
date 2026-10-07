/* sg-control -- Credential Manager, and the sign-in notice for saved
 * passwords that did not open.
 *
 * The page (Control Panel > User Accounts > Credential Manager, `control
 * /name Microsoft.CredentialManager`, keymgr.dll) lists the credentials
 * programs saved (CredEnumerate: generic ones -- Git Credential Manager's,
 * mail and chat programs' -- and network and domain ones -- Remote Desktop's
 * TERMSRV/..., mapped drives') and removes them (CredDelete). wine-sg (1380)
 * keeps them in the person's keyring, with their Linux programs' passwords,
 * opened by the sign-in password; the page says where they are kept.
 *
 * When that keyring did not open at sign-in -- an administrator reset the
 * password, and it is still protected with the old one -- the sign-in
 * notice (`/keyring-signin`, from the machine's Run key) and the page say
 * so and offer what Windows users would expect: unlock them with the
 * password used before (sg-session's sg-keyring then re-encrypts them with
 * the current one, so the next sign-in opens them by itself), or start
 * over with no saved passwords (the old ones are kept aside, unread).
 *
 * The keyring's side is sg-keyring, a Unix program of sg-session's run
 * through Wine's \\?\unix\ path with the passwords on its standard input
 * (a file in the person's private runtime directory, removed at once),
 * never on a command line. SG_KEYRING_HELPER (a Windows path) is the gate's.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"
#include <wincred.h>

#ifndef CRED_ENUMERATE_ALL_CREDENTIALS
#define CRED_ENUMERATE_ALL_CREDENTIALS 0x1
#endif

enum { CMD_CRED_REMOVE = CMD_PAGE_FIRST + 1100, CMD_CRED_UNLOCK = CMD_PAGE_FIRST + 1090 };
#define MAX_SHOWN 200

static const WCHAR KEYRING_HELPER[] = L"\\\\?\\unix\\usr\\libexec\\stained-glass\\sg-keyring";

/* ---- sg-keyring -------------------------------------------------------------------- */

/* A new file in the person's private runtime directory (0700, a tmpfs),
 * gone when its handle closes: Wine gives a Unix program no pipes as its
 * standard input and output (wine-sg 0078), only files. */
static HANDLE runtime_file(const WCHAR *what)
{
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    WCHAR dir[MAX_PATH], path[MAX_PATH + 96];
    static LONG seq;
    DWORD n;

    if (!GetEnvironmentVariableW(L"XDG_RUNTIME_DIR", dir, ARRAYSIZE(dir)) || dir[0] != '/') return INVALID_HANDLE_VALUE;
    _snwprintf(path, ARRAYSIZE(path), L"\\\\?\\unix%ls/sg-keyring-%ls-%lu-%lu-%ld", dir, what, GetCurrentProcessId(),
               GetTickCount(), InterlockedIncrement(&seq));
    path[ARRAYSIZE(path) - 1] = 0;
    for (n = 8; path[n]; n++) if (path[n] == '/') path[n] = '\\';
    return CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa,
                       CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
}

/* Runs sg-keyring VERB with `input` on its standard input; the line it
 * answers goes to out (UTF-8). FALSE if it could not be run, or gave no
 * answer within a minute. */
static BOOL keyring_helper(const WCHAR *verb, const char *input, DWORD inlen, char *out, DWORD outcap)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { 0 };
    WCHAR helper[MAX_PATH], cmd[MAX_PATH + 64];
    HANDLE in, res;
    DWORD n, i;
    BOOL ok;

    out[0] = 0;
    if (!GetEnvironmentVariableW(L"SG_KEYRING_HELPER", helper, MAX_PATH)) lstrcpyW(helper, KEYRING_HELPER);
    if (GetFileAttributesW(helper) == INVALID_FILE_ATTRIBUTES) return FALSE;
    if ((in = runtime_file(L"in")) == INVALID_HANDLE_VALUE) return FALSE;
    if ((res = runtime_file(L"out")) == INVALID_HANDLE_VALUE) { CloseHandle(in); return FALSE; }
    if (inlen) WriteFile(in, input, inlen, &n, NULL);
    SetFilePointer(in, 0, NULL, FILE_BEGIN);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = res;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" %ls", helper, verb);
    cmd[ARRAYSIZE(cmd) - 1] = 0;
    ok = CreateProcessW(helper, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    /* the passwords' file goes now: the program has its own descriptor */
    CloseHandle(in);
    /* its answer: one line (a Unix program started so has no process handle) */
    for (i = 0; ok && i < 600; i++) {
        OVERLAPPED at = { 0 };
        DWORD got = 0;
        if (ReadFile(res, out, outcap - 1, &got, &at) && got) {
            out[got] = 0;
            if (strchr(out, '\n')) break;
        }
        out[0] = 0;
        Sleep(100);
    }
    CloseHandle(res);
    out[strcspn(out, "\r\n")] = 0;
    return ok && out[0];
}

/* "open", "locked", "none" or "unavailable" (also when sg-keyring is missing) */
static void keyring_status(char *out, DWORD cap)
{
    if (!keyring_helper(L"status", NULL, 0, out, cap) || !out[0]) lstrcpynA(out, "unavailable", cap);
}

/* password1 NUL [password2 NUL], UTF-8, for the helper's standard input */
static DWORD pack(char *buf, DWORD cap, const WCHAR *a, const WCHAR *b)
{
    DWORD len = 0;
    int n = WideCharToMultiByte(CP_UTF8, 0, a, -1, buf, cap, NULL, NULL);
    if (n <= 0) return 0;
    len = n;
    if (b) {
        n = WideCharToMultiByte(CP_UTF8, 0, b, -1, buf + len, cap - len, NULL, NULL);
        if (n <= 0) return 0;
        len += n;
    }
    return len;
}

/* The question, as a form: unlock with the old password, or start over.
 * TRUE once the keyring is open. */
static BOOL keyring_ask(HWND owner)
{
    WCHAR use_old[2] = L"1", old[256] = L"", start_over[2] = L"0", cur[256] = L"";
    const WCHAR *intro =
        L"Your password was changed without your old one -- for example, an administrator reset it. "
        L"The passwords that apps, websites and networks saved for you are still protected with the "
        L"password you used before, so they can't be opened with the new one.\n\n"
        L"If you remember the old password, type it to unlock them; from then on they open with your "
        L"current password. Otherwise you can start over: apps will ask you to sign in to them again.";
    BOOL done = FALSE;

    for (;;) {
        struct form_field f[] = {
            { L"Unlock them with the password I used before", use_old, 2, FF_RADIO_FIRST },
            { L"Password used before:", old, ARRAYSIZE(old), FF_PASSWORD },
            { L"Start over with no saved passwords", start_over, 2, FF_RADIO },
            { L"Your current password:", cur, ARRAYSIZE(cur), FF_PASSWORD },
            { L"You can do this later in Control Panel > User Accounts > Credential Manager.", NULL, 0, FF_NOTE },
        };
        char in[1100], reply[200];
        DWORD len;
        BOOL recover;

        if (!run_form(owner, L"Saved passwords are locked", intro, f, ARRAYSIZE(f), L"Continue", FALSE)) break;
        recover = use_old[0] == L'1';
        if (!cur[0] || (recover && !old[0])) {
            message(owner, L"Saved passwords", recover ? L"Type the password you used before and your current password."
                                                       : L"Type your current password.", TRUE);
            continue;
        }
#ifndef SG_MUTANT_KEYRING_NOTICE_ORDER
        len = recover ? pack(in, sizeof(in), old, cur) : pack(in, sizeof(in), cur, NULL);
#else
        len = recover ? pack(in, sizeof(in), cur, old) : pack(in, sizeof(in), cur, NULL);
#endif
        if (!len || !keyring_helper(recover ? L"recover" : L"reset", in, len, reply, sizeof(reply))) {
            SecureZeroMemory(in, sizeof(in));
            message(owner, L"Saved passwords", L"The keyring service could not be reached.", TRUE);
            break;
        }
        SecureZeroMemory(in, sizeof(in));
        if (!strcmp(reply, "OK")) {
            done = TRUE;
            message(owner, L"Saved passwords", recover ? L"Your saved passwords are unlocked. From now on they open when you sign in."
                                                      : L"You are starting over: apps will ask you to sign in to them again.", FALSE);
            break;
        }
        if (!strcmp(reply, "FAIL current")) {
            cur[0] = 0;
            message(owner, L"Saved passwords", L"That is not your current password.", TRUE);
        } else if (!strcmp(reply, "FAIL old")) {
            old[0] = 0;
            message(owner, L"Saved passwords", L"That is not the password your saved passwords were protected with.", TRUE);
        } else {
            message(owner, L"Saved passwords", L"Your saved passwords could not be changed.", TRUE);
            break;
        }
    }
    SecureZeroMemory(old, sizeof(old));
    SecureZeroMemory(cur, sizeof(cur));
    return done;
}

/* /keyring-signin: at sign-in, the question if the keyring did not open */
int keyring_signin_main(void)
{
    char st[64];
    keyring_status(st, sizeof(st));
#ifndef SG_MUTANT_KEYRING_NOTICE
    if (strcmp(st, "locked")) return 0;
#else
    return 0;
#endif
    return keyring_ask(NULL) ? 0 : 1;
}

/* ---- the page ---------------------------------------------------------------------- */
static CREDENTIALW **g_creds;
static DWORD g_ncreds;
static int g_order[MAX_SHOWN], g_nshown;

static void load_creds(void)
{
    DWORD i;
    if (g_creds) CredFree(g_creds);
    g_creds = NULL;
    g_ncreds = 0;
    g_nshown = 0;
    if (!CredEnumerateW(NULL, CRED_ENUMERATE_ALL_CREDENTIALS, &g_ncreds, &g_creds)) { g_creds = NULL; g_ncreds = 0; }
    /* generic first, then network and domain ones */
    for (i = 0; i < g_ncreds && g_nshown < MAX_SHOWN; i++)
        if (g_creds[i]->Type == CRED_TYPE_GENERIC) g_order[g_nshown++] = (int)i;
    for (i = 0; i < g_ncreds && g_nshown < MAX_SHOWN; i++)
        if (g_creds[i]->Type == CRED_TYPE_DOMAIN_PASSWORD) g_order[g_nshown++] = (int)i;
}

static void when(const FILETIME *ft, WCHAR *out, int cch)
{
    SYSTEMTIME st, lt;
    out[0] = 0;
    if (!ft->dwHighDateTime && !ft->dwLowDateTime) return;
    if (!FileTimeToSystemTime(ft, &st) || !SystemTimeToTzSpecificLocalTime(NULL, &st, &lt)) return;
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &lt, NULL, out, cch);
}

void build_credmgr(void)
{
    const WCHAR *labels[2] = { L"User Accounts", L"Manage Accounts" };
    int ids[2] = { NAV(PG_USERS), NAV(PG_USERS_MANAGE) };
    int x = pg_left_pane(labels, ids, 2) + S(36), y = S(28), w = pg_width(), cw = w - x - S(40), i, last_type = -1;
    char st[64];

    pg_title(x, y, L"Manage your credentials");
    y += S(40);
    y += pg_para(x, y, cw, g_font_body, COL_TEXT,
                 L"View and delete your saved logon information for websites, connected applications and networks.") + S(16);

    keyring_status(st, sizeof(st));
    if (!strcmp(st, "locked")) {
        pg_icon(x, y, S(32), IC_WARN);
        pg_text(x + S(44), y, cw - S(44), S(22), g_font_head, COL_TEXT, L"Your saved passwords are locked", DT_SINGLELINE);
        y += S(26);
        y += pg_para(x + S(44), y, cw - S(44), g_font_body, COL_SUBTLE,
                     L"They are protected with a password that is no longer your sign-in password -- for example, "
                     L"because an administrator reset it. Until they are unlocked, programs cannot use them.") + S(6);
        pg_link(x + S(44), y, L"Unlock them, or start over...", CMD_CRED_UNLOCK, 0);
        y += S(36);
    } else if (!strcmp(st, "open")) {
        pg_icon(x, y, S(32), IC_OK);
        y += pg_para(x + S(44), y + S(6), cw - S(44), g_font_body, COL_SUBTLE,
                     L"Saved credentials are kept in your keyring, encrypted with your sign-in password.") + S(22);
    } else {
        pg_icon(x, y, S(32), IC_WARN);
        y += pg_para(x + S(44), y + S(6), cw - S(44), g_font_body, COL_SUBTLE,
                     L"No keyring is available, so saved credentials are kept in this account's registry, "
                     L"scrambled but not encrypted.") + S(22);
    }

    load_creds();
    if (!g_nshown) {
        pg_rule(x, y, cw);
        y += S(12);
        pg_text(x, y, cw, S(22), g_font_body, COL_SUBTLE, L"No saved credentials.", DT_SINGLELINE);
        return;
    }
    for (i = 0; i < g_nshown; i++) {
        const CREDENTIALW *c = g_creds[g_order[i]];
        WCHAR line[600], date[64];
        if ((int)c->Type != last_type) {
            last_type = (int)c->Type;
            y += S(6);
            pg_text(x, y, cw, S(24), g_font_head, COL_TITLE,
                    c->Type == CRED_TYPE_GENERIC ? L"Generic Credentials" : L"Network and Domain Credentials", DT_SINGLELINE);
            y += S(30);
            pg_rule(x, y, cw);
            y += S(8);
        }
        pg_icon(x, y, S(32), c->Type == CRED_TYPE_GENERIC ? IC_SHIELD : IC_NET);
        pg_text(x + S(44), y - S(2), cw - S(160), S(20), g_font_body, COL_TEXT, c->TargetName,
                DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        when(&c->LastWritten, date, ARRAYSIZE(date));
        _snwprintf(line, ARRAYSIZE(line), L"User name: %ls%ls%ls", c->UserName && c->UserName[0] ? c->UserName : L"(none)",
                   date[0] ? L"    Modified: " : L"", date);
        line[ARRAYSIZE(line) - 1] = 0;
        pg_text(x + S(44), y + S(18), cw - S(160), S(18), g_font_small, COL_SUBTLE, line,
                DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        pg_link(x + cw - S(90), y + S(6), L"Remove", CMD_CRED_REMOVE + i, 0);
        y += S(48);
    }
}

BOOL cmd_credmgr(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    if (id == CMD_CRED_UNLOCK) {
        if (keyring_ask(g_main)) refresh_page();
        return TRUE;
    }
    if (id >= CMD_CRED_REMOVE && id < CMD_CRED_REMOVE + g_nshown && g_creds) {
        const CREDENTIALW *c = g_creds[g_order[id - CMD_CRED_REMOVE]];
        WCHAR q[700];
        _snwprintf(q, ARRAYSIZE(q), L"Are you sure you want to permanently delete this credential?\n\n%ls", c->TargetName);
        q[ARRAYSIZE(q) - 1] = 0;
        if (MessageBoxW(g_main, q, L"Delete Credential", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return TRUE;
        if (!CredDeleteW(c->TargetName, c->Type, 0))
            message(g_main, L"Delete Credential", L"The credential could not be deleted.", TRUE);
        refresh_page();
        return TRUE;
    }
    return FALSE;
}

/* --dump credentials: the keyring's state and what the page lists (never a secret) */
void dump_credmgr(void)
{
    char st[64];
    int i;
    keyring_status(st, sizeof(st));
    wprintf(L"keyring=%hs\n", st);
    load_creds();
    for (i = 0; i < g_nshown; i++) {
        const CREDENTIALW *c = g_creds[g_order[i]];
        wprintf(L"credential=%ls\t%ls\t%ls\n", c->Type == CRED_TYPE_GENERIC ? L"generic" : L"domain", c->TargetName,
                c->UserName ? c->UserName : L"");
    }
}

/* --credential-delete TARGET [generic|domain]: the page's Remove, for the gate */
int credmgr_delete_cli(const WCHAR *target, const WCHAR *type)
{
    DWORD t = type && !_wcsicmp(type, L"domain") ? CRED_TYPE_DOMAIN_PASSWORD : CRED_TYPE_GENERIC;
    return CredDeleteW(target, t, 0) ? 0 : 1;
}
