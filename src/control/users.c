/* sg-control -- User Accounts: your account, and the machine's local
 * accounts. Listing needs no privilege (the account database is public);
 * every change is an administrator's, through the elevated dialogs.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"

enum {
    CMD_MY_TYPE = SHIELD_ID(CMD_PAGE_FIRST + 1),
    CMD_MY_PASSWORD = SHIELD_ID(CMD_PAGE_FIRST + 2),
    CMD_ADD = SHIELD_ID(CMD_PAGE_FIRST + 3),
    CMD_TYPE = SHIELD_ID(CMD_PAGE_FIRST + 4),
    CMD_PASSWORD = SHIELD_ID(CMD_PAGE_FIRST + 5),
    CMD_REMOVE = SHIELD_ID(CMD_PAGE_FIRST + 6),
    CMD_SELECT = CMD_PAGE_FIRST + 100,       /* + account index */
};

struct account { WCHAR name[64], full[128]; BOOL admin; };
static struct account g_acc[128];
static int g_nacc, g_sel = -1;

/* the people's accounts in /etc/passwd: uid 1000-59999 with a login shell */
static void load_accounts(void)
{
    char *text = read_unix_file("/etc/passwd", NULL), *line, *next;
    g_nacc = 0;
    if (!text) return;
    for (line = text; line && *line && g_nacc < (int)ARRAYSIZE(g_acc); line = next) {
        char *f[7], *p = line, *comma;
        int k = 0;
        long uid;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        while (k < 7 && p) { f[k++] = p; p = strchr(p, ':'); if (p) *p++ = 0; }
        if (k < 7) continue;
        uid = strtol(f[2], NULL, 10);
        if (uid < 1000 || uid >= 60000) continue;
        if (strstr(f[6], "nologin") || strstr(f[6], "/false")) continue;
        MultiByteToWideChar(CP_UTF8, 0, f[0], -1, g_acc[g_nacc].name, ARRAYSIZE(g_acc[0].name));
        if ((comma = strchr(f[4], ','))) *comma = 0;
        MultiByteToWideChar(CP_UTF8, 0, f[4], -1, g_acc[g_nacc].full, ARRAYSIZE(g_acc[0].full));
        g_acc[g_nacc].admin = unix_group_has("sg-admins", g_acc[g_nacc].name);
        g_nacc++;
    }
    free(text);
}

static int find_local(const WCHAR *name)
{
    int i;
    for (i = 0; i < g_nacc; i++) if (!lstrcmpiW(g_acc[i].name, name)) return i;
    return -1;
}

static void elevate(const WCHAR *verb, const WCHAR *name)
{
    WCHAR args[160];
    if (name) _snwprintf(args, ARRAYSIZE(args), L"/admin %ls \"%ls\"", verb, name);
    else _snwprintf(args, ARRAYSIZE(args), L"/admin %ls", verb);
    args[ARRAYSIZE(args) - 1] = 0;
    if (run_elevated(args)) refresh_when_back();
}

/* ---- your account ------------------------------------------------------------------ */
void build_users(void)
{
    static const WCHAR *const labels[] = { L"Manage another account", L"Add a user account", NULL, L"See also", L"System" };
    static const int ids[] = { NAV(PG_USERS_MANAGE), CMD_ADD, 0, -1, NAV(PG_SYSTEM) };
    WCHAR name[128], dom[128], role[32], realm[128], wg[64], line[256];
    int x = pg_left_pane(labels, ids, ARRAYSIZE(labels)) + S(36), y = S(24), me;
    BOOL domain;

    load_accounts();
    current_user(name, ARRAYSIZE(name), dom, ARRAYSIZE(dom));
    machine_role(role, realm, wg, ARRAYSIZE(realm));
    me = find_local(name);
    domain = me < 0 && role[0];

    pg_title(x, y, L"Make changes to your user account");
    y += S(56);
    pg_icon(x, y, S(96), IC_USER);
    pg_text(x + S(116), y + S(8), S(420), S(28), g_font_title, COL_TEXT,
            me >= 0 && g_acc[me].full[0] ? g_acc[me].full : name, DT_SINGLELINE | DT_END_ELLIPSIS);
    if (domain) _snwprintf(line, ARRAYSIZE(line), L"Domain account (%ls)", realm[0] ? realm : dom);
    else lstrcpyW(line, L"Local account");
    pg_text(x + S(116), y + S(40), S(420), S(20), g_font_body, COL_SUBTLE, line, DT_SINGLELINE);
    pg_text(x + S(116), y + S(60), S(420), S(20), g_font_body, COL_SUBTLE,
            unix_group_has("sg-admins", name) ? L"Administrator" : L"Standard user", DT_SINGLELINE);
    y += S(120);

    if (domain) {
        _snwprintf(line, ARRAYSIZE(line), L"Your account belongs to the %ls domain. Its password and membership are "
                   L"managed by the domain's administrators.", realm[0] ? realm : dom);
        pg_para(x, y, S(560), g_font_body, COL_TEXT, line);
        return;
    }
    pg_link(x, y, L"Change your password", CMD_MY_PASSWORD, LINK_SHIELD); y += S(28);
    pg_link(x, y, L"Change your account type", CMD_MY_TYPE, LINK_SHIELD); y += S(28);
    pg_link(x, y, L"Manage another account", NAV(PG_USERS_MANAGE), 0); y += S(28);
}

BOOL cmd_users(int id, int code, HWND ctl)
{
    WCHAR name[128], dom[128];
    (void)code; (void)ctl;
    current_user(name, ARRAYSIZE(name), dom, ARRAYSIZE(dom));
    switch (id) {
    case CMD_MY_TYPE: elevate(L"user-type", name); return TRUE;
    case CMD_MY_PASSWORD: elevate(L"user-password", name); return TRUE;
    case CMD_ADD: elevate(L"user-add", NULL); return TRUE;
    }
    return FALSE;
}

/* ---- manage accounts ------------------------------------------------------------------ */
void build_users_manage(void)
{
    int x = S(40), y = S(24), w = pg_width(), cols, colw = S(300), i;
    load_accounts();
    if (g_sel >= g_nacc) g_sel = -1;
    pg_title(x, y, g_sel < 0 ? L"Choose the user you would like to change" : L"Make changes to this account");
    y += S(52);
    if (g_sel >= 0) {
        struct account *a = &g_acc[g_sel];
        pg_icon(x, y, S(72), IC_USER);
        pg_text(x + S(90), y + S(6), S(400), S(26), g_font_title, COL_TEXT, a->full[0] ? a->full : a->name, DT_SINGLELINE | DT_END_ELLIPSIS);
        pg_textf(x + S(90), y + S(34), S(400), g_font_body, COL_SUBTLE, L"%ls  \x2022  Local account  \x2022  %ls", a->name,
                 a->admin ? L"Administrator" : L"Standard user");
        y += S(96);
        pg_link(x, y, L"Change the password", CMD_PASSWORD, LINK_SHIELD); y += S(28);
        pg_link(x, y, L"Change the account type", CMD_TYPE, LINK_SHIELD); y += S(28);
        pg_link(x, y, L"Delete the account", CMD_REMOVE, LINK_SHIELD); y += S(28);
        pg_link(x, y, L"Manage another account", NAV(PG_USERS_MANAGE), 0);
        return;
    }
    cols = (w - x * 2) / colw;
    if (cols < 1) cols = 1;
    for (i = 0; i < g_nacc; i++) {
        int cx = x + (i % cols) * colw, cy = y + (i / cols) * S(84);
        pg_icon(cx, cy, S(56), IC_USER);
        pg_link(cx + S(68), cy + S(4), g_acc[i].full[0] ? g_acc[i].full : g_acc[i].name, CMD_SELECT + i, LINK_BOLD);
        pg_text(cx + S(70), cy + S(28), colw - S(80), S(18), g_font_body, COL_SUBTLE, g_acc[i].admin ? L"Administrator" : L"Standard user", DT_SINGLELINE);
        pg_text(cx + S(70), cy + S(46), colw - S(80), S(18), g_font_small, COL_SUBTLE, g_acc[i].name, DT_SINGLELINE);
    }
    if (!g_nacc) { pg_text(x, y, w - x * 2, S(20), g_font_body, COL_SUBTLE, L"There are no local accounts.", DT_SINGLELINE); i = 1; }
    y += ((i + cols - 1) / cols) * S(84) + S(16);
    pg_rule(x, y, w - x * 2);
    pg_link(x, y + S(16), L"Add a new user", CMD_ADD, LINK_SHIELD);
}

BOOL cmd_users_manage(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    if (id >= CMD_SELECT && id < CMD_SELECT + g_nacc) { g_sel = id - CMD_SELECT; refresh_page(); return TRUE; }
    switch (id) {
    case CMD_ADD: elevate(L"user-add", NULL); return TRUE;
    case CMD_TYPE: if (g_sel >= 0) elevate(L"user-type", g_acc[g_sel].name); return TRUE;
    case CMD_PASSWORD: if (g_sel >= 0) elevate(L"user-password", g_acc[g_sel].name); return TRUE;
    case CMD_REMOVE: if (g_sel >= 0) { elevate(L"user-remove", g_acc[g_sel].name); g_sel = -1; } return TRUE;
    }
    return FALSE;
}

LRESULT notify_users(NMHDR *nm) { (void)nm; return 0; }

/* leaving the page forgets the selection */
void users_reset(void) { g_sel = -1; }

void dump_users(void)
{
    WCHAR name[128], dom[128], role[32], realm[128], wg[64];
    int i, me;
    load_accounts();
    current_user(name, ARRAYSIZE(name), dom, ARRAYSIZE(dom));
    machine_role(role, realm, wg, ARRAYSIZE(realm));
    me = find_local(name);
    wprintf(L"user.current=%ls\n", name);
    wprintf(L"user.current.type=%ls\n", unix_group_has("sg-admins", name) ? L"administrator" : L"standard");
    wprintf(L"user.current.kind=%ls\n", me >= 0 || !role[0] ? L"local" : L"domain");
    for (i = 0; i < g_nacc; i++)
        wprintf(L"account=%ls|%ls|%ls\n", g_acc[i].name, g_acc[i].full, g_acc[i].admin ? L"administrator" : L"standard");
}
