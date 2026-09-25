/* sg-control -- the home page (category view), the category pages and
 * "All Control Panel Items".
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"

/* commands the navigation pages share */
enum {
    CMD_INET = CMD_PAGE_FIRST + 1, CMD_JOY, CMD_DESK, CMD_NCPA,
    CMD_RENAME = SHIELD_ID(CMD_PAGE_FIRST + 10),
    CMD_TIMEZONE = SHIELD_ID(CMD_PAGE_FIRST + 11),
    CMD_HOSTED = CMD_PAGE_FIRST + 100,        /* + index into the hosted list */
};

struct task { const WCHAR *label; int id; };
struct applet { const WCHAR *name; int icon; int id; const WCHAR *keywords; struct task tasks[3]; };

/* ---- the applets, as they appear in the category pages and All Items ---------- */
static const struct applet SYSTEM_A = { L"System", IC_SYSTEM, NAV(PG_SYSTEM), L"computer name domain workgroup ram processor about",
    { { L"View amount of RAM and processor speed", NAV(PG_SYSTEM) }, { L"Rename this computer", CMD_RENAME } } };
static const struct applet UPDATE_A = { L"Windows Update", IC_UPDATE, NAV(PG_UPDATE), L"updates upgrade patch apt",
    { { L"Check for updates", NAV(PG_UPDATE) }, { L"View update history", NAV(PG_UPDATE) } } };
static const struct applet NETCENTER_A = { L"Network and Sharing Center", IC_NETCENTER, NAV(PG_NETWORK), L"network ip address adapter ethernet wifi dns",
    { { L"View network status and tasks", NAV(PG_NETWORK) }, { L"View network connections", CMD_NCPA } } };
static const struct applet INET_A = { L"Internet Options", IC_INET, CMD_INET, L"browser proxy homepage cookies",
    { { L"Change your homepage", CMD_INET }, { L"Delete browsing history and cookies", CMD_INET } } };
static const struct applet GAME_A = { L"Game Controllers", IC_GAME, CMD_JOY, L"joystick gamepad controller",
    { { L"Set up USB game controllers", CMD_JOY } } };
static const struct applet DISPLAY_A = { L"Display", IC_DISPLAY, CMD_DESK, L"screen resolution monitor",
    { { L"Adjust screen resolution", CMD_DESK } } };
static const struct applet PROGRAMS_A = { L"Programs and Features", IC_PROG, NAV(PG_PROGRAMS), L"uninstall remove software applications",
    { { L"Uninstall a program", NAV(PG_PROGRAMS) } } };
static const struct applet USERS_A = { L"User Accounts", IC_USERS, NAV(PG_USERS), L"account password administrator user",
    { { L"Change account type", NAV(PG_USERS_MANAGE) }, { L"Remove user accounts", NAV(PG_USERS_MANAGE) } } };
static const struct applet PERSONAL_A = { L"Personalization", IC_PERSONAL, NAV(PG_PERSONALIZE), L"wallpaper background color colour theme dark light accent",
    { { L"Change the desktop background", NAV(PG_PERSONALIZE) }, { L"Change accent color", NAV(PG_PERSONALIZE) },
      { L"Choose light or dark mode", NAV(PG_PERSONALIZE) } } };
static const struct applet DATETIME_A = { L"Date and Time", IC_DATETIME, NAV(PG_DATETIME), L"clock time zone ntp calendar",
    { { L"Set the time and date", NAV(PG_DATETIME) }, { L"Change the time zone", CMD_TIMEZONE } } };
static const struct applet SPEECH_A = { L"Speech Recognition", IC_SPEECH, NAV(PG_SPEECH), L"speech voice typing dictation microphone dictate talk",
    { { L"Set up voice typing", NAV(PG_SPEECH) }, { L"Set up a microphone", NAV(PG_SPEECH) } } };
static const struct applet NCPA_A = { L"Network Connections", IC_NET, CMD_NCPA, L"adapter ethernet wifi tcp ip settings", { { 0 } } };

static const struct applet *const ALL[] = {
    &DATETIME_A, &DISPLAY_A, &GAME_A, &INET_A, &NETCENTER_A, &NCPA_A, &PERSONAL_A,
    &PROGRAMS_A, &SPEECH_A, &SYSTEM_A, &USERS_A, &UPDATE_A,
};

struct category { enum page_id page; int icon; const WCHAR *title; struct task links[3]; const struct applet *applets[4]; };
static const struct category CATS[] = {
    { PG_CAT_SYSSEC, IC_SYSSEC, L"System and Security",
      { { L"View basic information about this computer", NAV(PG_SYSTEM) }, { L"Check for updates", NAV(PG_UPDATE) } },
      { &SYSTEM_A, &UPDATE_A } },
    { PG_CAT_NET, IC_NET, L"Network and Internet",
      { { L"View network status and tasks", NAV(PG_NETWORK) }, { L"Internet Options", CMD_INET } },
      { &NETCENTER_A, &INET_A } },
    { PG_CAT_HW, IC_HW, L"Hardware and Sound",
      { { L"Set up game controllers", CMD_JOY }, { L"Adjust screen resolution", CMD_DESK } },
      { &DISPLAY_A, &GAME_A, &SPEECH_A } },
    { PG_CAT_PROG, IC_PROG, L"Programs",
      { { L"Uninstall a program", NAV(PG_PROGRAMS) } },
      { &PROGRAMS_A } },
    { PG_CAT_USERS, IC_USERS, L"User Accounts",
      { { L"Change account type", NAV(PG_USERS_MANAGE) }, { L"Add or remove user accounts", NAV(PG_USERS_MANAGE) } },
      { &USERS_A } },
    { PG_CAT_APPEAR, IC_APPEAR, L"Appearance and Personalization",
      { { L"Change the desktop background", NAV(PG_PERSONALIZE) }, { L"Choose light or dark mode", NAV(PG_PERSONALIZE) } },
      { &PERSONAL_A, &DISPLAY_A } },
    { PG_CAT_CLOCK, IC_CLOCK, L"Clock and Region",
      { { L"Set the time and date", NAV(PG_DATETIME) }, { L"Change the time zone", CMD_TIMEZONE } },
      { &DATETIME_A } },
};

static struct cpl_item g_hosted[32];
static int g_nhosted = -1;

static void load_hosted(void)
{
    if (g_nhosted < 0) g_nhosted = cpl_list(g_hosted, ARRAYSIZE(g_hosted));
}

/* ---- home: the category view ---------------------------------------------------- */
void build_home(void)
{
    int w = pg_width(), colw, x0 = S(48), y0 = S(84), i;
    pg_title(S(40), S(24), L"Adjust your computer's settings");
    pg_text(w - S(260), S(28), S(70), S(20), g_font_body, COL_TEXT, L"View by:", DT_SINGLELINE | DT_RIGHT);
    pg_link(w - S(180), S(26), L"Category", CMD_CATEGORY, LINK_BOLD);
    pg_link(w - S(110), S(26), L"Large icons", CMD_ALL_ITEMS, 0);
    colw = (w - x0 * 2) / 2;
    if (colw < S(300)) colw = S(300);
    for (i = 0; i < (int)ARRAYSIZE(CATS); i++) {
        int x = x0 + (i % 2) * colw, y = y0 + (i / 2) * S(104), j;
        pg_icon(x, y, S(48), CATS[i].icon);
        pg_link(x + S(62), y - S(2), CATS[i].title, NAV(CATS[i].page), LINK_CATEGORY);
        for (j = 0; j < 3 && CATS[i].links[j].label; j++)
            pg_link(x + S(62), y + S(28) + j * S(22), CATS[i].links[j].label, CATS[i].links[j].id,
                    IS_SHIELD(CATS[i].links[j].id) ? LINK_SHIELD : 0);
    }
}

/* ---- a category: its applets and their tasks --------------------------------------- */
void build_category(void)
{
    const struct category *cat = NULL;
    const WCHAR *labels[ARRAYSIZE(CATS)];
    int ids[ARRAYSIZE(CATS)], i, n = 0, x, y, w = pg_width();
    for (i = 0; i < (int)ARRAYSIZE(CATS); i++) {
        if (CATS[i].page == current_page()) cat = &CATS[i];
        labels[n] = CATS[i].title; ids[n] = NAV(CATS[i].page); n++;
    }
    if (!cat) return;
    x = pg_left_pane(labels, ids, n) + S(36);
    y = S(28);
    for (i = 0; i < 4 && cat->applets[i]; i++) {
        const struct applet *a = cat->applets[i];
        int tx = x + S(56), j;
        pg_icon(x, y, S(40), a->icon);
        pg_link(tx, y - S(2), a->name, a->id, LINK_CATEGORY);
        for (j = 0; j < 3 && a->tasks[j].label; j++) {
            HWND l = pg_link(tx, y + S(28), a->tasks[j].label, a->tasks[j].id, IS_SHIELD(a->tasks[j].id) ? LINK_SHIELD : 0);
            RECT r; GetWindowRect(l, &r);
            tx += (r.right - r.left) + S(8);
            if (j + 1 < 3 && a->tasks[j + 1].label) {
                pg_text(tx, y + S(30), S(10), S(18), g_font_body, COL_RULE, L"|", DT_SINGLELINE);
                tx += S(14);
            }
            if (tx > w - S(200)) break;
        }
        y += S(76);
        pg_rule(x, y - S(14), w - x - S(36));
    }
    /* third-party applets live under Hardware and Sound, as on Windows */
    if (cat->page == PG_CAT_HW) {
        load_hosted();
        for (i = 0; i < g_nhosted; i++) {
            pg_icon(x, y, S(40), IC_GENERIC);
            pg_link(x + S(56), y - S(2), g_hosted[i].name, CMD_HOSTED + i, LINK_CATEGORY);
            pg_text(x + S(56), y + S(28), w - x - S(120), S(20), g_font_body, COL_SUBTLE, g_hosted[i].info, DT_SINGLELINE | DT_END_ELLIPSIS);
            y += S(76);
        }
    }
}

/* ---- all items: a grid, filtered by the search box ------------------------------- */
static BOOL matches(const WCHAR *name, const WCHAR *keywords)
{
    WCHAR hay[512], needle[128];
    if (!g_search_text[0]) return TRUE;
    _snwprintf(hay, ARRAYSIZE(hay), L"%ls %ls", name, keywords ? keywords : L"");
    hay[ARRAYSIZE(hay) - 1] = 0;
    lstrcpynW(needle, g_search_text, ARRAYSIZE(needle));
    CharLowerW(hay); CharLowerW(needle);
    return wcsstr(hay, needle) != NULL;
}

void build_all(void)
{
    int w = pg_width(), cols, colw, i, n = 0, x0 = S(40), y0 = S(84);
    load_hosted();
    if (g_search_text[0]) {
        WCHAR t[200];
        _snwprintf(t, ARRAYSIZE(t), L"Search results for \x201C%ls\x201D", g_search_text);
        t[ARRAYSIZE(t) - 1] = 0;
        pg_title(S(40), S(24), t);
    } else pg_title(S(40), S(24), L"Adjust your computer's settings");
    pg_text(w - S(260), S(28), S(70), S(20), g_font_body, COL_TEXT, L"View by:", DT_SINGLELINE | DT_RIGHT);
    pg_link(w - S(180), S(26), L"Category", CMD_CATEGORY, 0);
    pg_link(w - S(110), S(26), L"Large icons", CMD_ALL_ITEMS, LINK_BOLD);
    colw = S(280);
    cols = (w - x0 * 2) / colw;
    if (cols < 1) cols = 1;
    for (i = 0; i < (int)ARRAYSIZE(ALL); i++) {
        int x, y;
        if (!matches(ALL[i]->name, ALL[i]->keywords)) continue;
        x = x0 + (n % cols) * colw; y = y0 + (n / cols) * S(56);
        pg_icon(x, y, S(32), ALL[i]->icon);
        pg_link(x + S(42), y + S(6), ALL[i]->name, ALL[i]->id, 0);
        n++;
    }
    for (i = 0; i < g_nhosted; i++) {
        int x, y;
        if (!matches(g_hosted[i].name, g_hosted[i].info)) continue;
        x = x0 + (n % cols) * colw; y = y0 + (n / cols) * S(56);
        pg_icon(x, y, S(32), IC_GENERIC);
        pg_link(x + S(42), y + S(6), g_hosted[i].name, CMD_HOSTED + i, 0);
        n++;
    }
    if (!n) pg_text(x0, y0, w - x0 * 2, S(24), g_font_body, COL_SUBTLE, L"No items match your search.", DT_SINGLELINE);
}

BOOL cmd_home(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    switch (id) {
    case CMD_ALL_ITEMS: navigate(PG_ALL); return TRUE;
    case CMD_CATEGORY: navigate(PG_HOME); return TRUE;
    case CMD_INET: cpl_open_file(L"inetcpl.cpl", NULL); return TRUE;
    case CMD_JOY: cpl_open_file(L"joy.cpl", NULL); return TRUE;
    case CMD_DESK: cpl_open_file(L"desk.cpl", NULL); return TRUE;
    case CMD_NCPA: if (!open_network_connections()) navigate(PG_NETWORK); return TRUE;
    case CMD_RENAME: if (run_elevated(L"/admin rename")) refresh_when_back(); return TRUE;
    case CMD_TIMEZONE: if (run_elevated(L"/admin timezone")) refresh_when_back(); return TRUE;
    }
    if (id >= CMD_HOSTED && id < CMD_HOSTED + g_nhosted) {
        WCHAR arg[16];
        _snwprintf(arg, ARRAYSIZE(arg), L"@%d", g_hosted[id - CMD_HOSTED].index);
        cpl_open_file(g_hosted[id - CMD_HOSTED].file, arg);
        return TRUE;
    }
    return FALSE;
}

void dump_items(void)
{
    int i;
    load_hosted();
    for (i = 0; i < (int)ARRAYSIZE(CATS); i++) wprintf(L"category=%ls\n", CATS[i].title);
    for (i = 0; i < (int)ARRAYSIZE(ALL); i++) wprintf(L"item=%ls\n", ALL[i]->name);
    for (i = 0; i < g_nhosted; i++) wprintf(L"item=%ls (%ls)\n", g_hosted[i].name, g_hosted[i].file);
}
