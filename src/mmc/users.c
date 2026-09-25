/* sg-mmc -- Local Users and Groups (lusrmgr.msc; in Computer Management) and
 * Shared Folders (fsmgmt.msc): read-only views of this computer's Stained
 * Glass accounts and groups, and of what it shares over SMB (Samba).
 *
 * The accounts are Linux's (sg-sysinfo users/groups): a person with a Windows
 * session is in `sgwine` ("Users"), an administrator in `sg-admins`
 * ("Administrators"), and `sgsystem` is the Windows SYSTEM account. They are
 * changed in Control Panel > User Accounts (sg-admind), not here. Shares are
 * Samba's (testparm, net usershare); sessions and open files need an
 * administrator (sg-sysinfod).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "mmc.h"
#include <string.h>

enum { V_MANAGE = 1 };

static sys_reply_t g_users, g_groups, g_shares, g_sessions, g_files;

static const char *fld(sys_reply_t *r, int b, const char *k) { const char *v = sys_field(r, b, k); return v ? v : ""; }

static void info_message(sys_reply_t *r, int b, const WCHAR *title)
{
    WCHAR text[4096] = L"", line[512];
    int i;
    for (i = b; i < r->nlines && strcmp(r->lines[i], "END"); i++)
    {
        utf8_to_w(r->lines[i], line, 512);
        if (wcslen(text) + wcslen(line) + 3 < ARRAY_SIZE(text)) { wcscat(text, line); wcscat(text, L"\n"); }
    }
    frame_message(MB_OK | MB_ICONINFORMATION, title, L"%ls", text);
}

static void manage_verbs(node_t *n, LPARAM key, BOOL have, verbs_t *out)
{
    (void)n; (void)key;
    if (!have) out->v[out->n++] = (verb_t){ V_MANAGE, L"&Manage accounts in Control Panel", IC_USERS, TRUE, FALSE };
}

static void manage_invoke(node_t *n, LPARAM key, BOOL have, int verb)
{
    (void)n; (void)key; (void)have;
    if (verb == V_MANAGE) ShellExecuteW(g_main, NULL, L"control.exe", L"userpasswords2", NULL, SW_SHOWNORMAL);
}

static void not_available(sys_reply_t *r)
{
    WCHAR m[512];
    utf8_to_w(r->message[0] ? r->message : "the system information service is not connected", m, 512);
    m[0] = towupper(m[0]);
    pane_empty_text(m);
    frame_status(L"%ls", m);
}

/* ---- users ------------------------------------------------------------------------------------ */

static void show_users(node_t *n)
{
    static const WCHAR *const cols[] = { L"Name", L"Full Name", L"Description", L"Windows", L"Administrator" };
    static const int widths[] = { 150, 180, 330, 80, 100 };
    int b;
    (void)n;
    pane_columns(cols, widths, 5);
    sys_free(&g_users);
    sys_request(&g_users, "users", NULL);
    pane_begin();
    for (b = sys_next_block(&g_users, 0, "USER"); b >= 0; b = sys_next_block(&g_users, b + 1, "USER"))
    {
        WCHAR c[5][256];
        const WCHAR *p[5] = { c[0], c[1], c[2], c[3], c[4] };
        const char *desc = fld(&g_users, b, "DESCRIPTION");
        BOOL sys = !strcmp(fld(&g_users, b, "SYSTEM-ACCOUNT"), "yes");
        utf8_to_w(g_users.lines[b] + 5, c[0], 256);
        utf8_to_w(fld(&g_users, b, "FULL-NAME"), c[1], 256);
        if (*desc) utf8_to_w(desc, c[2], 256);
        else if (!strcmp(fld(&g_users, b, "WINDOWS"), "yes")) lstrcpyW(c[2], L"A Stained Glass account");
        else lstrcpyW(c[2], L"A Linux account without a Windows session");
        if (!strcmp(fld(&g_users, b, "DISABLED"), "yes")) wcscat(c[2], L" (disabled)");
        lstrcpyW(c[3], !strcmp(fld(&g_users, b, "WINDOWS"), "yes") ? L"Yes" : L"No");
        lstrcpyW(c[4], !strcmp(fld(&g_users, b, "ADMIN"), "yes") ? L"Yes" : L"No");
        pane_add(b + 1, sys ? IC_SGLOGO : IC_USER, p);
    }
    pane_end();
    if (!g_users.ok) not_available(&g_users);
    else frame_status(L"Accounts are changed in Control Panel > User Accounts");
    frame_banner(L"Shown read-only. Stained Glass accounts are managed in Control Panel > User Accounts.");
}

static void open_user(node_t *n, LPARAM key)
{
    WCHAR name[128];
    (void)n;
    if (key < 1 || key > g_users.nlines) return;
    utf8_to_w(g_users.lines[key - 1] + 5, name, 128);
    info_message(&g_users, (int)key - 1, name);
}

static void dump_users(node_t *n, FILE *f) { (void)n; fprintf(f, "USERSOK %d\n", g_users.ok); }

static const snapin_t users_ops = {
    NULL, show_users, manage_verbs, manage_invoke, open_user, NULL, NULL, NULL, NULL, dump_users
};

/* ---- groups ------------------------------------------------------------------------------------ */

static void show_groups(node_t *n)
{
    static const WCHAR *const cols[] = { L"Name", L"Description", L"Members", L"Linux group" };
    static const int widths[] = { 160, 330, 260, 110 };
    int b;
    (void)n;
    pane_columns(cols, widths, 4);
    sys_free(&g_groups);
    sys_request(&g_groups, "groups", NULL);
    pane_begin();
    for (b = sys_next_block(&g_groups, 0, "GROUP"); b >= 0; b = sys_next_block(&g_groups, b + 1, "GROUP"))
    {
        WCHAR c[4][512];
        const WCHAR *p[4] = { c[0], c[1], c[2], c[3] };
        const char *win = fld(&g_groups, b, "WINDOWS-NAME");
        utf8_to_w(g_groups.lines[b] + 6, c[3], 512);
#ifdef SG_MUTANT_WINNAME
        win = "";
#endif
        if (*win) utf8_to_w(win, c[0], 512);
        else lstrcpynW(c[0], c[3], 512);
        utf8_to_w(fld(&g_groups, b, "DESCRIPTION"), c[1], 512);
        utf8_to_w(fld(&g_groups, b, "MEMBERS"), c[2], 512);
        pane_add(b + 1, IC_GROUP, p);
    }
    pane_end();
    if (!g_groups.ok) not_available(&g_groups);
    frame_banner(L"Shown read-only. Administrators is sg-admins, Users is sgwine on the Linux side.");
}

static void open_group(node_t *n, LPARAM key)
{
    WCHAR name[128];
    (void)n;
    if (key < 1 || key > g_groups.nlines) return;
    utf8_to_w(g_groups.lines[key - 1] + 6, name, 128);
    info_message(&g_groups, (int)key - 1, name);
}

static const snapin_t groups_ops = {
    NULL, show_groups, manage_verbs, manage_invoke, open_group, NULL, NULL, NULL, NULL, NULL
};

node_t *users_create(node_t *parent)
{
    node_t *n = node_add(parent, L"Local Users and Groups", IC_USERS, NULL, NULL), *c;
    lstrcpyW(n->desc, L"The Stained Glass accounts and groups on this computer (read-only)");
    c = node_add(n, L"Users", IC_FOLDER, &users_ops, NULL);
    lstrcpyW(c->desc, L"Accounts");
    c = node_add(n, L"Groups", IC_FOLDER, &groups_ops, NULL);
    lstrcpyW(c->desc, L"Groups of accounts");
    return n;
}

/* ---- Shared Folders ------------------------------------------------------------------------------- */

/* a Linux path as Windows programs see it (Wine's own conversion) */
static void dos_path(const char *unix_path, WCHAR *out, int cch)
{
    static WCHAR *(CDECL *to_dos)(const char *);
    WCHAR *p;
    if (!to_dos) to_dos = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name");
    if (to_dos && *unix_path == '/' && (p = to_dos(unix_path)))
    {
        lstrcpynW(out, p, cch);
        HeapFree(GetProcessHeap(), 0, p);
        return;
    }
    utf8_to_w(unix_path, out, cch);
}

static void show_shares(node_t *n)
{
    static const WCHAR *const cols[] = { L"Share Name", L"Folder Path", L"Type", L"Description", L"Access" };
    static const int widths[] = { 140, 280, 80, 260, 110 };
    int b;
    (void)n;
    pane_columns(cols, widths, 5);
    sys_free(&g_shares);
    sys_request(&g_shares, "shares", NULL);
    pane_begin();
    for (b = sys_next_block(&g_shares, 0, "SHARE"); b >= 0; b = sys_next_block(&g_shares, b + 1, "SHARE"))
    {
        WCHAR c[5][512];
        const WCHAR *p[5] = { c[0], c[1], c[2], c[3], c[4] };
        BOOL printer = !strcmp(fld(&g_shares, b, "PRINTABLE"), "yes");
        utf8_to_w(g_shares.lines[b] + 6, c[0], 512);
        dos_path(fld(&g_shares, b, "PATH"), c[1], 512);
        lstrcpyW(c[2], printer ? L"Printer" : L"Windows");
        utf8_to_w(fld(&g_shares, b, "COMMENT"), c[3], 512);
        _snwprintf(c[4], 512, L"%ls%ls", !strcmp(fld(&g_shares, b, "READONLY"), "yes") ? L"Read" : L"Read/Write",
                   !strcmp(fld(&g_shares, b, "GUEST"), "yes") ? L", guests" : L"");
        pane_add(b + 1, printer ? IC_PRINTER : IC_SHARE, p);
    }
    pane_end();
    if (!g_shares.ok) not_available(&g_shares);
    else frame_status(L"Shared over SMB by Samba");
}

static void open_share(node_t *n, LPARAM key)
{
    WCHAR name[128];
    (void)n;
    if (key < 1 || key > g_shares.nlines) return;
    utf8_to_w(g_shares.lines[key - 1] + 6, name, 128);
    info_message(&g_shares, (int)key - 1, name);
}

static const snapin_t shares_ops = { NULL, show_shares, NULL, NULL, open_share };

static void show_list(sys_reply_t *r, const char *cmd, const char *head, const WCHAR *const *cols, const int *widths,
                      const char *const *keys, int n)
{
    int b, k;
    pane_columns(cols, widths, n);
    sys_free(r);
    sys_request(r, cmd, NULL);
    pane_begin();
    for (b = sys_next_block(r, 0, head); b >= 0; b = sys_next_block(r, b + 1, head))
    {
        WCHAR c[8][512];
        const WCHAR *p[8];
        utf8_to_w(r->lines[b] + strlen(head) + 1, c[0], 512);
        for (k = 1; k < n; k++) utf8_to_w(fld(r, b, keys[k]), c[k], 512);
        for (k = 0; k < n; k++) p[k] = c[k];
        pane_add(b + 1, !strcmp(head, "SESSION") ? IC_SESSION : IC_LOG, p);
    }
    pane_end();
    if (!r->ok)
    {
        if (!strcmp(r->kind, "denied")) pane_empty_text(L"You need to be an administrator to see this.");
        else not_available(r);
    }
}

static void show_sessions(node_t *n)
{
    static const WCHAR *const cols[] = { L"Session", L"User", L"Computer", L"Protocol", L"Encrypted" };
    static const int widths[] = { 90, 150, 180, 110, 90 };
    static const char *const keys[] = { "", "USER", "MACHINE", "PROTOCOL", "ENCRYPTED" };
    (void)n;
    show_list(&g_sessions, "sessions", "SESSION", cols, widths, keys, 5);
}

static void show_files(node_t *n)
{
    static const WCHAR *const cols[] = { L"Open File", L"Accessed By", L"Share", L"Open Mode" };
    static const int widths[] = { 320, 150, 140, 120 };
    static const char *const keys[] = { "", "USER", "SHARE", "MODE" };
    (void)n;
    show_list(&g_files, "openfiles", "OPENFILE", cols, widths, keys, 4);
}

static const snapin_t sessions_ops = { NULL, show_sessions };
static const snapin_t files_ops = { NULL, show_files };

node_t *shares_create(node_t *parent)
{
    node_t *n = node_add(parent, L"Shared Folders", IC_SHARE, NULL, NULL), *c;
    lstrcpyW(n->desc, L"The folders this computer shares (Samba)");
    c = node_add(n, L"Shares", IC_FOLDER, &shares_ops, NULL);
    lstrcpyW(c->desc, L"Shared folders and printers");
    c = node_add(n, L"Sessions", IC_FOLDER, &sessions_ops, NULL);
    lstrcpyW(c->desc, L"Who is connected");
    c = node_add(n, L"Open Files", IC_FOLDER, &files_ops, NULL);
    lstrcpyW(c->desc, L"Files open over the network");
    return n;
}
