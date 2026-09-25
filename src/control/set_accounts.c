/* sg-control -- Settings > Accounts: Your info, Sign-in options, Other users.
 *
 * The accounts are the machine's (users.c reads them); every change is an
 * administrator's and goes through the Control Panel's elevated dialogs
 * (/admin user-add, user-type, user-password, user-remove) and sg-admind.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"

enum {
    CMD_PASSWORD = SHIELD_ID(CMD_PAGE_FIRST + 1), CMD_TYPE_ME = SHIELD_ID(CMD_PAGE_FIRST + 2),
    CMD_ADD = SHIELD_ID(CMD_PAGE_FIRST + 3), CMD_CHTYPE = SHIELD_ID(CMD_PAGE_FIRST + 4), CMD_REMOVE = SHIELD_ID(CMD_PAGE_FIRST + 5),
    CMD_REQUIRE = CMD_PAGE_FIRST + 6, CMD_SHOWDETAILS = CMD_PAGE_FIRST + 7, CMD_MANAGE = CMD_PAGE_FIRST + 8,
    CMD_OTHERS = CMD_PAGE_FIRST + 9, CMD_PICK = CMD_PAGE_FIRST + 100,
};
static int g_pick = -1;
static WCHAR g_pick_name[64];

static void me(WCHAR *name, int cch, BOOL *domain, WCHAR *full, int fcch, WCHAR *realm, int rcch)
{
    WCHAR dom[128], role[32], wg[64];
    const struct account *acc;
    int n = acc_load(&acc), i;
    current_user(name, cch, dom, ARRAYSIZE(dom));
    machine_role(role, realm, wg, rcch);
    full[0] = 0;
    *domain = role[0] != 0;
    for (i = 0; i < n; i++)
        if (!lstrcmpiW(acc[i].name, name)) { *domain = FALSE; lstrcpynW(full, acc[i].full, fcch); }
}

void set_build_yourinfo(void)
{
    WCHAR name[128], full[128], realm[128];
    BOOL domain;
    int y = st_title(L"Your info");
    me(name, ARRAYSIZE(name), &domain, full, ARRAYSIZE(full), realm, ARRAYSIZE(realm));
    pg_icon(st_x(), y, S(96), IC_USER);
    pg_text(st_x() + S(116), y + S(10), st_w() - S(116), S(32), g_font_title, COL_TEXT, full[0] ? full : name, DT_SINGLELINE | DT_END_ELLIPSIS);
    pg_text(st_x() + S(116), y + S(50), st_w() - S(116), S(22), g_font_body, COL_SUBTLE, domain ? L"Domain account" : L"Local Account", DT_SINGLELINE);
    pg_text(st_x() + S(116), y + S(72), st_w() - S(116), S(22), g_font_body, COL_SUBTLE,
            unix_group_has("sg-admins", name) ? L"Administrator" : L"Standard user", DT_SINGLELINE);
    y += S(120);
    if (domain) {
        WCHAR line[300];
        _snwprintf(line, ARRAYSIZE(line), L"Your account belongs to the %ls domain; its administrators manage it.", realm);
        y = st_para(y, line);
        return;
    }
    st_link(&y, L"Change your account type", CMD_TYPE_ME);
    st_link(&y, L"Manage my account in Control Panel", CMD_MANAGE);
}

void set_build_signin(void)
{
    static const WCHAR *const req[] = { L"When PC wakes up from sleep", L"Never" };
    WCHAR name[128], full[128], realm[128];
    BOOL domain;
    int y = st_title(L"Sign-in options");
    me(name, ARRAYSIZE(name), &domain, full, ARRAYSIZE(full), realm, ARRAYSIZE(realm));
    y = st_head(y, L"Manage how you sign in to your device");
    y = st_card(y, IC_G_PRIVACY, L"Password", domain ? L"Sign in with your domain password" : L"Sign in with your account's password");
    if (!domain) st_button(&y, L"Change", CMD_PASSWORD);
    y = st_card(y, IC_G_ACCOUNTS, L"Sign-in PIN", L"Not available on this device");
    y = st_head(y, L"Require sign-in");
    st_combo(&y, L"If you've been away, when should Windows require you to sign in again?", req, 2,
             reg_dword(HKEY_CURRENT_USER, L"Software\\Stained Glass\\SignIn", L"RequireOnWake", 1) ? 0 : 1, CMD_REQUIRE);
    y = st_head(y, L"Privacy");
    st_toggle(&y, L"Show account details such as my name on the sign-in screen",
              reg_dword(HKEY_CURRENT_USER, L"Software\\Stained Glass\\SignIn", L"ShowDetails", 1) != 0, CMD_SHOWDETAILS);
}

void set_build_otherusers(void)
{
    const struct account *acc;
    WCHAR me_name[128], dom[128];
    int n = acc_load(&acc), y = st_title(L"Other users"), i, shown = 0;
    current_user(me_name, ARRAYSIZE(me_name), dom, ARRAYSIZE(dom));
    y = st_head(y, L"Other users");
    st_button(&y, L"Add someone else to this PC", CMD_ADD);
    for (i = 0; i < n; i++) {
        if (!lstrcmpiW(acc[i].name, me_name)) continue;
        y = st_card(y, IC_G_ACCOUNTS, acc[i].full[0] ? acc[i].full : acc[i].name,
                    acc[i].admin ? L"Administrator - Local account" : L"Local account");
        if (g_pick == i && !lstrcmpW(g_pick_name, acc[i].name)) {
            pg_control(L"BUTTON", L"Change account type", WS_TABSTOP | BS_PUSHBUTTON, st_x() + S(64), y - S(8), S(180), S(30), CMD_CHTYPE);
            pg_control(L"BUTTON", L"Remove", WS_TABSTOP | BS_PUSHBUTTON, st_x() + S(254), y - S(8), S(110), S(30), CMD_REMOVE);
            SendMessageW(GetDlgItem(g_page, CMD_CHTYPE), BCM_SETSHIELD, 0, TRUE);
            SendMessageW(GetDlgItem(g_page, CMD_REMOVE), BCM_SETSHIELD, 0, TRUE);
            y += S(34);
        } else { pg_link(st_x() + S(64), y - S(12), L"Select", CMD_PICK + i, 0); y += S(16); }
        shown++;
    }
    if (!shown) y = st_para(y, L"No one else has an account on this PC.");
}

BOOL set_cmd_accounts(int id, int code, HWND ctl)
{
    WCHAR name[128], dom[128];
    (void)code;
    current_user(name, ARRAYSIZE(name), dom, ARRAYSIZE(dom));
    if (id >= CMD_PICK && id < CMD_PICK + 128) {
        const struct account *acc;
        int n = acc_load(&acc);
        g_pick = id - CMD_PICK;
        if (g_pick < n) lstrcpynW(g_pick_name, acc[g_pick].name, ARRAYSIZE(g_pick_name));
        refresh_page();
        return TRUE;
    }
    switch (id) {
    case CMD_PASSWORD: acc_elevate(L"user-password", name); return TRUE;
    case CMD_TYPE_ME: acc_elevate(L"user-type", name); return TRUE;
    case CMD_ADD: acc_elevate(L"user-add", NULL); return TRUE;
    case CMD_CHTYPE: if (g_pick_name[0]) acc_elevate(L"user-type", g_pick_name); return TRUE;
    case CMD_REMOVE: if (g_pick_name[0]) { acc_elevate(L"user-remove", g_pick_name); g_pick = -1; } return TRUE;
    case CMD_MANAGE: ShellExecuteW(NULL, NULL, L"control.exe", L"userpasswords", NULL, SW_SHOWNORMAL); return TRUE;
    case CMD_REQUIRE:
        if (code == CBN_SELCHANGE)
            reg_set_dword(HKEY_CURRENT_USER, L"Software\\Stained Glass\\SignIn", L"RequireOnWake", SendMessageW(ctl, CB_GETCURSEL, 0, 0) == 0);
        return TRUE;
    case CMD_SHOWDETAILS: reg_set_dword(HKEY_CURRENT_USER, L"Software\\Stained Glass\\SignIn", L"ShowDetails", st_checked(ctl)); return TRUE;
    }
    return FALSE;
}
