/* sg-control -- Stained Glass Firewall: Settings > Network & Internet >
 * Firewall, Allowed apps and Inbound port rules, the Control Panel's
 * "Allow an app through firewall", and the firewall's question when a
 * program starts listening ("Stained Glass Firewall has blocked some
 * features of this app").
 *
 * The firewall is sg-session's sg-firewall (David 2026-10-07): connections
 * other computers start are blocked except those allowed, networks are
 * Public or Private, programs are allowed by their path. What it is doing is
 * in its status file, which anyone may read; changing it is an
 * administrator's, as on Windows: the elevated copy of this program (SYSTEM)
 * files the change with sg-admind ("firewall ..."), which runs sg-firewall.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"
#include <commdlg.h>

#define FW_DOCS L"https://freesoft.page/docs/guide/firewall.html"
#define FW_STATUS "/run/stained-glass-firewall/status"
#define FW_RUN "/run/stained-glass-firewall"

enum {
    CMD_FW_TOGGLE = SHIELD_ID(CMD_PAGE_FIRST + 1),          /* + profile */
    CMD_FW_NETWORK = SHIELD_ID(CMD_PAGE_FIRST + 10),        /* + network */
    CMD_FW_APPS = CMD_PAGE_FIRST + 40, CMD_FW_RULES, CMD_FW_LEARN,
    CMD_FW_RESET = SHIELD_ID(CMD_PAGE_FIRST + 45),
    CMD_FW_CHANGE = SHIELD_ID(CMD_PAGE_FIRST + 46),
    CMD_FW_ADDAPP = CMD_PAGE_FIRST + 47, CMD_FW_ADDPORT,
    CMD_FW_PRIV = CMD_PAGE_FIRST + 100,     /* + row: the Private box */
    CMD_FW_PUB = CMD_PAGE_FIRST + 300,      /* + row: the Public box */
    CMD_FW_REMOVE = CMD_PAGE_FIRST + 500,   /* + row */
    CMD_FW_PNAME = CMD_PAGE_FIRST + 700, CMD_FW_PPORTS, CMD_FW_PPROTO, CMD_FW_PACTION, CMD_FW_PPRIV, CMD_FW_PPUB,
};

static const char *const PROFILE_KEYS[3] = { "domain", "private", "public" };
static const WCHAR *const PROFILE_TITLES[3] = { L"Domain network", L"Private network", L"Public network" };

/* ---- the firewall's status -------------------------------------------------------- */
struct fw_rule { WCHAR id[64], action[8], profiles[32], proto[8], ports[64], program[MAX_PATH], name[128], source[16]; BOOL enabled; };
struct fw_net { WCHAR uuid[64], dev[32], category[16], name[128]; };
struct fw_group { WCHAR key[32], profiles[32], title[64]; };
struct fw_status {
    BOOL running;
    BOOL on[3];
    WCHAR current[48];
    struct fw_net nets[8]; int nnets;
    struct fw_group groups[8]; int ngroups;
    struct fw_rule *rules; int nrules;
};

static void field(const char *line, int i, WCHAR *out, int cch)
{
    const char *p = line, *e;
    int n;
    out[0] = 0;
    while (i-- > 0) { if (!(p = strchr(p, '\t'))) return; p++; }
    e = p + strcspn(p, "\t\n");
    n = MultiByteToWideChar(CP_UTF8, 0, p, (int)(e - p), out, cch - 1);
    out[n > 0 ? n : 0] = 0;
}

static const char *status_path(void)
{
    static char path[MAX_PATH];
    WCHAR env[MAX_PATH];
    if (!path[0]) {
        if (GetEnvironmentVariableW(L"SG_FIREWALL_STATUS", env, MAX_PATH) && env[0] == L'/')
            WideCharToMultiByte(CP_UTF8, 0, env, -1, path, sizeof(path), NULL, NULL);
        else strcpy(path, FW_STATUS);
    }
    return path;
}

static void fw_free(struct fw_status *s) { free(s->rules); memset(s, 0, sizeof(*s)); }

static void fw_load(struct fw_status *s)
{
    char *text = read_unix_file(status_path(), NULL), *line, *next;
    int cap = 0;
    memset(s, 0, sizeof(*s));
    s->on[0] = s->on[1] = s->on[2] = TRUE;
    if (!text) return;
    s->running = !strncmp(text, "state\trunning", 13);
    for (line = text; line && *line; line = next) {
        WCHAR w[64];
        if ((next = strchr(line, '\n'))) *next++ = 0;
        if (!strncmp(line, "profile\t", 8)) {
            int i;
            field(line, 1, w, 64);
            for (i = 0; i < 3; i++) {
                WCHAR k[16];
                MultiByteToWideChar(CP_UTF8, 0, PROFILE_KEYS[i], -1, k, 16);
                if (!lstrcmpW(w, k)) { field(line, 2, w, 64); s->on[i] = lstrcmpW(w, L"off") != 0; }
            }
        } else if (!strncmp(line, "current\t", 8)) field(line, 1, s->current, ARRAYSIZE(s->current));
        else if (!strncmp(line, "network\t", 8) && s->nnets < (int)ARRAYSIZE(s->nets)) {
            struct fw_net *n = &s->nets[s->nnets++];
            field(line, 1, n->uuid, ARRAYSIZE(n->uuid)); field(line, 2, n->dev, ARRAYSIZE(n->dev));
            field(line, 3, n->category, ARRAYSIZE(n->category)); field(line, 5, n->name, ARRAYSIZE(n->name));
            if (!n->name[0]) lstrcpynW(n->name, n->dev, ARRAYSIZE(n->name));
        } else if (!strncmp(line, "group\t", 6) && s->ngroups < (int)ARRAYSIZE(s->groups)) {
            struct fw_group *g = &s->groups[s->ngroups++];
            field(line, 1, g->key, ARRAYSIZE(g->key)); field(line, 2, g->profiles, ARRAYSIZE(g->profiles));
            field(line, 3, g->title, ARRAYSIZE(g->title));
        } else if (!strncmp(line, "rule\t", 5)) {
            struct fw_rule *r;
            if (s->nrules == cap) {
                struct fw_rule *nr = realloc(s->rules, (cap = cap ? cap * 2 : 16) * sizeof(*nr));
                if (!nr) break;
                s->rules = nr;
            }
            r = &s->rules[s->nrules++];
            field(line, 1, r->id, ARRAYSIZE(r->id));
            field(line, 2, w, 64); r->enabled = !lstrcmpW(w, L"1");
            field(line, 3, r->action, ARRAYSIZE(r->action)); field(line, 4, r->profiles, ARRAYSIZE(r->profiles));
            field(line, 5, r->proto, ARRAYSIZE(r->proto)); field(line, 6, r->ports, ARRAYSIZE(r->ports));
            field(line, 7, r->program, ARRAYSIZE(r->program)); field(line, 8, r->name, ARRAYSIZE(r->name));
            field(line, 9, r->source, ARRAYSIZE(r->source));
            if (!lstrcmpW(r->program, L"*")) r->program[0] = 0;
        }
    }
    free(text);
}

static BOOL has_profile(const WCHAR *profiles, const WCHAR *p)
{
    return !lstrcmpW(profiles, L"all") || wcsstr(profiles, p) != NULL;
}

/* an allowed app's boxes: Private (with Domain, as Windows' list shows them) and Public */
static BOOL rule_private(const struct fw_rule *r) { return r->enabled && !lstrcmpW(r->action, L"allow") && has_profile(r->profiles, L"private"); }
static BOOL rule_public(const struct fw_rule *r) { return r->enabled && !lstrcmpW(r->action, L"allow") && has_profile(r->profiles, L"public"); }

static void profiles_arg(BOOL priv, BOOL pub, WCHAR *out, int cch)
{
    _snwprintf(out, cch, L"%ls%ls%ls", priv ? L"domain,private" : L"", priv && pub ? L"," : L"", pub ? L"public" : L"");
    if (!out[0]) lstrcpynW(out, L"none", cch);
}

/* a change: elevated, at once; otherwise through the elevated copy */
static BOOL fw_change(const WCHAR *const *fields, int n)
{
    WCHAR msg[512];
    if (!admin_request(fields, n, msg, ARRAYSIZE(msg), 60000)) {
        message(g_main, L"Stained Glass Firewall", msg[0] ? msg : L"The firewall could not be changed.", TRUE);
        return FALSE;
    }
    Sleep(300);     /* the firewall picks the change up within a moment; its status follows */
    return TRUE;
}

/* ---- Settings > Network & Internet > Firewall -------------------------------------- */
static struct fw_status g_fw;

void set_build_firewall(void)
{
    int y = st_title(L"Firewall & network protection"), i, j;
    WCHAR line[512];
    fw_free(&g_fw);
    fw_load(&g_fw);
    y = st_para(y, L"Stained Glass Firewall blocks connections that other computers start to this PC, except the ones you "
                   L"allow. Connections your apps make, and the answers to them, are never blocked.");
    if (!g_fw.running) {
        y = st_para(y, L"The firewall is not running, so connections from other computers are not filtered. "
                       L"Restart the PC, or ask an administrator to start it (sg-firewall.service).");
    }
    for (i = 0; i < 3; i++) {
        WCHAR key[16], title[96], nets[300] = L"";
        HWND c;
        BOOL active;
        MultiByteToWideChar(CP_UTF8, 0, PROFILE_KEYS[i], -1, key, 16);
        active = wcsstr(g_fw.current, key) != NULL;
        if (i == 0 && !active) continue;        /* Domain: only on a domain's network */
        for (j = 0; j < g_fw.nnets; j++)
            if (!lstrcmpW(g_fw.nets[j].category, key)) {
                if (nets[0]) wcsncat(nets, L", ", ARRAYSIZE(nets) - wcslen(nets) - 1);
                wcsncat(nets, g_fw.nets[j].name, ARRAYSIZE(nets) - wcslen(nets) - 1);
            }
        _snwprintf(title, ARRAYSIZE(title), L"%ls%ls", PROFILE_TITLES[i], active ? L" (active)" : L"");
        y = st_head(y, title);
        _snwprintf(line, ARRAYSIZE(line), active ? L"Networks: %ls" : L"Not connected to a %ls.%ls",
                   active ? nets : (i == 1 ? L"private network" : L"public network"), L"");
        y = st_text(y, line);
        c = pg_control(SET_TOGGLE_CLASS, L"Stained Glass Firewall", WS_TABSTOP, st_x(), y, S(280), S(26), CMD_FW_TOGGLE + i);
        SendMessageW(c, BM_SETCHECK, g_fw.on[i] ? BST_CHECKED : BST_UNCHECKED, 0);
        y += S(36);
        if (!g_fw.on[i]) {
            _snwprintf(line, ARRAYSIZE(line), L"The firewall is off for %ls networks. Other computers on them can reach "
                       L"every program on this PC that listens. Your PC may be vulnerable.", i == 1 ? L"private" : i == 2 ? L"public" : L"domain");
            y = st_para(y, line);
        }
    }

    y = st_head(y, L"Network profile");
    if (!g_fw.nnets) y = st_para(y, L"This PC is not connected to a network.");
    for (j = 0; j < g_fw.nnets && j < 8; j++) {
        static const WCHAR *const kinds[] = { L"Public", L"Private" };
        WCHAR label[200];
        if (!lstrcmpW(g_fw.nets[j].category, L"domain")) {
            _snwprintf(line, ARRAYSIZE(line), L"%ls: Domain network (this PC's domain)", g_fw.nets[j].name);
            y = st_text(y, line);
            continue;
        }
        _snwprintf(label, ARRAYSIZE(label), L"%ls", g_fw.nets[j].name);
        st_combo(&y, label, kinds, 2, !lstrcmpW(g_fw.nets[j].category, L"private") ? 1 : 0, CMD_FW_NETWORK + j);
    }
    y = st_para(y, L"Public: your PC is hidden from other devices on the network, and only apps you allow on public "
                   L"networks can be reached. Use it on networks you don't trust, such as in a cafe or an airport. "
                   L"Private: a network you trust, at home or at work; your PC can be found, and the apps you allow "
                   L"on private networks can be reached.");

    y = st_head(y, L"More settings");
    st_link(&y, L"Allow an app through firewall", CMD_FW_APPS);
    st_link(&y, L"Inbound port rules (advanced)", CMD_FW_RULES);
    st_link(&y, L"Restore firewalls to default", CMD_FW_RESET);
    st_link(&y, L"How the firewall works", CMD_FW_LEARN);
}

/* ---- Allowed apps ------------------------------------------------------------------ */
struct fw_row { BOOL group; int index; };
static struct fw_row g_rows[200];
static int g_nrows;

static int row_cmp(const void *a, const void *b)
{
    const struct fw_row *x = a, *y = b;
    const WCHAR *nx = x->group ? g_fw.groups[x->index].title : g_fw.rules[x->index].name;
    const WCHAR *ny = y->group ? g_fw.groups[y->index].title : g_fw.rules[y->index].name;
    return lstrcmpiW(nx, ny);
}

void set_build_fwapps(void)
{
    int y = st_title(L"Allowed apps"), i, x = st_x(), w = st_w(), cpriv, cpub;
    BOOL admin = is_elevated();
    HWND c;
    fw_free(&g_fw);
    fw_load(&g_fw);
    y = st_para(y, L"Allow apps to communicate through Stained Glass Firewall. An allowed app can be reached by other "
                   L"computers on the networks ticked for it, while it is running.");
    if (!admin) {
        HWND b = pg_control(L"BUTTON", L"Change settings", WS_TABSTOP | BS_PUSHBUTTON, x, y, S(160), S(32), CMD_FW_CHANGE);
        SendMessageW(b, BCM_SETSHIELD, 0, TRUE);
        y += S(44);
    }
    y = st_head(y, L"Allowed apps and features:");
    cpriv = x + w - S(170);
    cpub = x + w - S(90);
    pg_text(x, y, cpriv - x, S(20), g_font_small, COL_SUBTLE, L"Name", DT_LEFT | DT_SINGLELINE);
    pg_text(cpriv, y, S(80), S(20), g_font_small, COL_SUBTLE, L"Private", DT_LEFT | DT_SINGLELINE);
    pg_text(cpub, y, S(80), S(20), g_font_small, COL_SUBTLE, L"Public", DT_LEFT | DT_SINGLELINE);
    y += S(24);
    g_nrows = 0;
    for (i = 0; i < g_fw.ngroups && g_nrows < (int)ARRAYSIZE(g_rows); i++) g_rows[g_nrows++] = (struct fw_row){ TRUE, i };
    for (i = 0; i < g_fw.nrules && g_nrows < (int)ARRAYSIZE(g_rows); i++)
        if (g_fw.rules[i].program[0] && lstrcmpW(g_fw.rules[i].program, L"@system")) g_rows[g_nrows++] = (struct fw_row){ FALSE, i };
    qsort(g_rows, g_nrows, sizeof(g_rows[0]), row_cmp);
    for (i = 0; i < g_nrows; i++) {
        const struct fw_row *r = &g_rows[i];
        BOOL priv, pub;
        const WCHAR *name, *sub;
        if (r->group) {
            name = g_fw.groups[r->index].title;
            priv = has_profile(g_fw.groups[r->index].profiles, L"private");
            pub = has_profile(g_fw.groups[r->index].profiles, L"public");
            sub = L"Built in";
        } else {
            const struct fw_rule *fr = &g_fw.rules[r->index];
            name = fr->name;
            priv = rule_private(fr);
            pub = rule_public(fr);
            sub = fr->program;
        }
        pg_text(x, y, cpriv - x - S(8), S(20), g_font_body, COL_TEXT, name, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        pg_text(x, y + S(20), cpriv - x - S(8), S(18), g_font_small, COL_SUBTLE, sub, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        c = pg_control(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, cpriv + S(16), y + S(4), S(20), S(20), CMD_FW_PRIV + i);
        SendMessageW(c, BM_SETCHECK, priv ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(c, L"Private");    /* named for the reader and the gate; drawn without text */
        EnableWindow(c, admin);
        c = pg_control(L"BUTTON", L"", WS_TABSTOP | BS_AUTOCHECKBOX, cpub + S(16), y + S(4), S(20), S(20), CMD_FW_PUB + i);
        SendMessageW(c, BM_SETCHECK, pub ? BST_CHECKED : BST_UNCHECKED, 0);
        SetWindowTextW(c, L"Public");
        EnableWindow(c, admin);
        y += S(46);
        if (!r->group && admin) {
            pg_link(x, y - S(6), L"Remove", CMD_FW_REMOVE + i, 0);
            y += S(24);
        }
    }
    if (!g_nrows) y = st_para(y, L"No apps are allowed yet.");
    y += S(8);
    if (admin) st_button(&y, L"Allow another app...", CMD_FW_ADDAPP);
    st_link(&y, L"What are the risks of allowing an app through a firewall?", CMD_FW_LEARN);
}

/* ---- Inbound port rules --------------------------------------------------------------- */
static HWND g_pf[6];
static int g_port_rows[100], g_nport_rows;

void set_build_fwrules(void)
{
    static const WCHAR *const protos[] = { L"TCP", L"UDP" };
    static const WCHAR *const actions[] = { L"Allow the connection", L"Block the connection" };
    int y = st_title(L"Inbound port rules"), i, x = st_x();
    BOOL admin = is_elevated();
    WCHAR line[400];
    fw_free(&g_fw);
    fw_load(&g_fw);
    y = st_para(y, L"A port rule opens (or closes) a port whatever listens on it -- for a server you run that never asks, "
                   L"for example. Most apps don't need one: allow the app instead, and its ports open while it runs.");
    if (!admin) {
        HWND b = pg_control(L"BUTTON", L"Change settings", WS_TABSTOP | BS_PUSHBUTTON, x, y, S(160), S(32), CMD_FW_CHANGE);
        SendMessageW(b, BCM_SETSHIELD, 0, TRUE);
        y += S(44);
    }
    y = st_head(y, L"Rules");
    g_nport_rows = 0;
    for (i = 0; i < g_fw.nrules && g_nport_rows < (int)ARRAYSIZE(g_port_rows); i++) {
        const struct fw_rule *r = &g_fw.rules[i];
        if (r->program[0]) continue;
        _snwprintf(line, ARRAYSIZE(line), L"%ls -- %ls %ls, %ls networks: %ls%ls", r->name,
                   !lstrcmpW(r->proto, L"any") ? L"TCP and UDP" : !lstrcmpW(r->proto, L"tcp") ? L"TCP" : L"UDP",
                   r->ports, !lstrcmpW(r->profiles, L"all") ? L"all" : r->profiles,
                   !lstrcmpW(r->action, L"allow") ? L"allowed" : L"blocked", r->enabled ? L"" : L" (off)");
        y = st_text(y, line);
        if (admin) { pg_link(x, y - S(4), L"Remove", CMD_FW_REMOVE + g_nport_rows, 0); y += S(26); }
        g_port_rows[g_nport_rows++] = i;
    }
    if (!g_nport_rows) y = st_para(y, L"There are no port rules.");
    if (!admin) return;
    y = st_head(y, L"New inbound rule");
    g_pf[0] = st_edit(&y, L"Name", L"", CMD_FW_PNAME);
    g_pf[1] = st_edit(&y, L"Ports (such as 8080, or 5000-5010)", L"", CMD_FW_PPORTS);
    g_pf[2] = st_combo(&y, L"Protocol", protos, 2, 0, CMD_FW_PPROTO);
    g_pf[3] = st_combo(&y, L"Action", actions, 2, 0, CMD_FW_PACTION);
    st_checkbox(&y, L"Private networks", TRUE, CMD_FW_PPRIV);
    g_pf[4] = GetDlgItem(g_page, CMD_FW_PPRIV);
    st_checkbox(&y, L"Public networks", FALSE, CMD_FW_PPUB);
    g_pf[5] = GetDlgItem(g_page, CMD_FW_PPUB);
    st_button(&y, L"Add rule", CMD_FW_ADDPORT);
}

/* ---- commands ------------------------------------------------------------------------- */
static void program_name(const WCHAR *path, WCHAR *name, int cch, WCHAR *publisher, int pcch);

static void add_app(void)
{
    WCHAR file[MAX_PATH] = L"", name[128], pub[128], unix_path[MAX_PATH], args[MAX_PATH + 160];
    OPENFILENAMEW ofn = { sizeof(ofn) };
    (void)args; (void)unix_path;
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"Programs (*.exe)\0*.exe\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Allow an app";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;
    program_name(file, name, ARRAYSIZE(name), pub, ARRAYSIZE(pub));
    {
        const WCHAR *req[] = { L"firewall", L"rule-add", L"allow", L"domain,private", L"any", L"*", file, name, L"user" };
        if (fw_change(req, 9)) refresh_page();
    }
}

static void add_port(void)
{
    WCHAR name[128], ports[64], profiles[40];
    int proto = (int)SendMessageW(g_pf[2], CB_GETCURSEL, 0, 0), action = (int)SendMessageW(g_pf[3], CB_GETCURSEL, 0, 0);
    GetWindowTextW(g_pf[0], name, ARRAYSIZE(name));
    GetWindowTextW(g_pf[1], ports, ARRAYSIZE(ports));
    if (!ports[0]) { st_status(L"Type the ports the rule is for."); return; }
    profiles_arg(st_checked(g_pf[4]), st_checked(g_pf[5]), profiles, ARRAYSIZE(profiles));
    if (!lstrcmpW(profiles, L"none")) { st_status(L"Tick the networks the rule is for."); return; }
    if (!name[0]) _snwprintf(name, ARRAYSIZE(name), L"Port %ls", ports);
    {
        const WCHAR *req[] = { L"firewall", L"rule-add", action == 1 ? L"block" : L"allow", profiles,
                               proto == 1 ? L"udp" : L"tcp", ports, L"*", name, L"user" };
        if (fw_change(req, 9)) refresh_page();
    }
}

BOOL set_cmd_firewall(int id, int code, HWND ctl)
{
    WCHAR args[600];
    int i;
    if (id >= CMD_FW_TOGGLE && id < CMD_FW_TOGGLE + 3) {
        WCHAR key[16];
        MultiByteToWideChar(CP_UTF8, 0, PROFILE_KEYS[id - CMD_FW_TOGGLE], -1, key, 16);
        _snwprintf(args, ARRAYSIZE(args), L"/admin firewall-profile %ls %ls", key, st_checked(ctl) ? L"on" : L"off");
        if (run_elevated(args)) refresh_when_back(); else refresh_page();
        return TRUE;
    }
    if (id >= CMD_FW_NETWORK && id < CMD_FW_NETWORK + g_fw.nnets && code == CBN_SELCHANGE) {
        const struct fw_net *n = &g_fw.nets[id - CMD_FW_NETWORK];
        int sel = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        _snwprintf(args, ARRAYSIZE(args), L"/admin firewall-network %ls %ls \"%ls\"", n->uuid, sel == 1 ? L"private" : L"public", n->name);
        if (run_elevated(args)) refresh_when_back(); else refresh_page();
        return TRUE;
    }
    if (id >= CMD_FW_NETWORK && id < CMD_FW_NETWORK + 8) return TRUE;
    /* the allowed apps' boxes and Remove (elevated: at once) */
    if (id >= CMD_FW_PRIV && id < CMD_FW_PRIV + g_nrows && code == BN_CLICKED) i = id - CMD_FW_PRIV;
    else if (id >= CMD_FW_PUB && id < CMD_FW_PUB + g_nrows && code == BN_CLICKED) i = id - CMD_FW_PUB;
    else i = -1;
    if (i >= 0) {
        WCHAR profiles[40];
        HWND priv = GetDlgItem(g_page, CMD_FW_PRIV + i), pub = GetDlgItem(g_page, CMD_FW_PUB + i);
        profiles_arg(st_checked(priv), st_checked(pub), profiles, ARRAYSIZE(profiles));
        if (g_rows[i].group) {
            const WCHAR *req[] = { L"firewall", L"group", g_fw.groups[g_rows[i].index].key, profiles };
            fw_change(req, 4);
        } else {
            const WCHAR *req[] = { L"firewall", L"rule-set", g_fw.rules[g_rows[i].index].id,
                                   lstrcmpW(profiles, L"none") ? L"1" : L"0", L"allow", profiles };
            fw_change(req, 6);
        }
        refresh_page();
        return TRUE;
    }
    if (id >= CMD_FW_REMOVE && id < CMD_FW_REMOVE + 200) {
        int k = id - CMD_FW_REMOVE, ri;
        if ((current_page() == PG_S_FWRULES)) ri = k < g_nport_rows ? g_port_rows[k] : -1;
        else ri = k < g_nrows && !g_rows[k].group ? g_rows[k].index : -1;
        if (ri >= 0) {
            const WCHAR *req[] = { L"firewall", L"rule-remove", g_fw.rules[ri].id };
            if (fw_change(req, 3)) refresh_page();
        }
        return TRUE;
    }
    switch (id) {
    case CMD_FW_APPS: navigate(PG_S_FWAPPS); return TRUE;
    case CMD_FW_RULES: navigate(PG_S_FWRULES); return TRUE;
    case CMD_FW_LEARN: ShellExecuteW(NULL, NULL, FW_DOCS, NULL, NULL, SW_SHOWNORMAL); return TRUE;
    case CMD_FW_RESET:
        if (MessageBoxW(g_main, L"Restore the firewall's default settings?\n\nEvery app and port you allowed is removed "
                        L"(apps will ask again), and the firewall is turned on for every network. Your networks stay "
                        L"Public or Private as they are.", L"Restore defaults", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return TRUE;
        if (run_elevated(L"/admin firewall-reset")) refresh_when_back();
        return TRUE;
    case CMD_FW_CHANGE:
        if (run_elevated((current_page() == PG_S_FWRULES) ? L"ms-settings:network-firewall-rules" : L"ms-settings:network-firewall-apps"))
            refresh_when_back();
        return TRUE;
    case CMD_FW_ADDAPP: add_app(); return TRUE;
    case CMD_FW_ADDPORT: add_port(); return TRUE;
    case CMD_FW_PNAME: case CMD_FW_PPORTS: case CMD_FW_PPROTO: case CMD_FW_PACTION: case CMD_FW_PPRIV: case CMD_FW_PPUB:
        return TRUE;
    }
    return FALSE;
}

/* ---- the elevated half: /admin firewall-... ------------------------------------------- */
int firewall_admin(int argc, WCHAR **argv)
{
    WCHAR msg[512];
    const WCHAR *req[9];
    int n = 0;
    if (!lstrcmpW(argv[0], L"firewall-profile") && argc > 2) {
        req[n++] = L"firewall"; req[n++] = L"profile"; req[n++] = argv[1]; req[n++] = argv[2];
    } else if (!lstrcmpW(argv[0], L"firewall-network") && argc > 2) {
        req[n++] = L"firewall"; req[n++] = L"network"; req[n++] = argv[1]; req[n++] = argv[2];
        if (argc > 3 && argv[3][0]) req[n++] = argv[3];
    } else if (!lstrcmpW(argv[0], L"firewall-reset")) {
        req[n++] = L"firewall"; req[n++] = L"reset";
    } else if (!lstrcmpW(argv[0], L"firewall-allow") && argc > 3) {
        /* the question's Allow access: PROFILES PROGRAM NAME */
        req[n++] = L"firewall"; req[n++] = L"rule-add"; req[n++] = L"allow"; req[n++] = argv[1];
        req[n++] = L"any"; req[n++] = L"*"; req[n++] = argv[2]; req[n++] = argv[3]; req[n++] = L"prompt";
    } else return 2;
    if (admin_request(req, n, msg, ARRAYSIZE(msg), 60000)) return 0;
    message(NULL, L"Stained Glass Firewall", msg[0] ? msg : L"The firewall could not be changed.", TRUE);
    return 1;
}

/* ---- the question: "Stained Glass Firewall has blocked some features of this app" ------ */
/* the program's own name and maker, from its version resource (a Windows program) */
static void program_name(const WCHAR *path, WCHAR *name, int cch, WCHAR *publisher, int pcch)
{
    DWORD handle, size;
    void *data;
    const WCHAR *base = wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : wcsrchr(path, L'/') ? wcsrchr(path, L'/') + 1 : path;
    lstrcpynW(name, base, cch);
    if (lstrlenW(name) > 4 && !lstrcmpiW(name + lstrlenW(name) - 4, L".exe")) name[lstrlenW(name) - 4] = 0;
    lstrcpynW(publisher, L"Unknown", pcch);
    if (path[0] == L'/') return;
    if (!(size = GetFileVersionInfoSizeW(path, &handle)) || !(data = malloc(size))) return;
    if (GetFileVersionInfoW(path, 0, size, data)) {
        struct { WORD lang, cp; } *tr;
        UINT len;
        WCHAR q[80], *v;
        if (VerQueryValueW(data, L"\\VarFileInfo\\Translation", (void **)&tr, &len) && len >= 4) {
            _snwprintf(q, ARRAYSIZE(q), L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
            if (VerQueryValueW(data, q, (void **)&v, &len) && len > 1 && v[0]) lstrcpynW(name, v, cch);
            _snwprintf(q, ARRAYSIZE(q), L"\\StringFileInfo\\%04x%04x\\CompanyName", tr->lang, tr->cp);
            if (VerQueryValueW(data, q, (void **)&v, &len) && len > 1 && v[0]) lstrcpynW(publisher, v, pcch);
        }
    }
    free(data);
}

struct prompt {
    WCHAR program[MAX_PATH], name[128], publisher[128], ask[MAX_PATH], category[16];
    BOOL linux_app;
    HICON icon;
    HWND priv, pub, allow;
    int result;     /* 1 allow, 0 cancel */
};
static struct prompt *g_prompt;
enum { ID_FW_ALLOW = 100, ID_FW_PRIV, ID_FW_PUB, ID_FW_RISKS };

static LRESULT CALLBACK prompt_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct prompt *p = g_prompt;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, band;
        WCHAR line[400];
        GetClientRect(hwnd, &rc);
        band = rc; band.bottom = S(64);
        FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
        { HBRUSH b = CreateSolidBrush(RGB(0xF0, 0xF0, 0xF0)); FillRect(dc, &band, b); DeleteObject(b); }
        draw_icon(dc, IC_SHIELD, S(16), S(14), S(36));
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_font_head);
        SetTextColor(dc, RGB(0x1e, 0x39, 0x87));
        band.left = S(64); band.right -= S(12);
        DrawTextW(dc, L"Stained Glass Firewall has blocked some features of this app", -1, &band,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, g_font_body);
        SetTextColor(dc, RGB(0, 0, 0));
        _snwprintf(line, ARRAYSIZE(line), L"Stained Glass Firewall has blocked some features of %ls on all public and private networks.", p->name);
        rc.left = S(20); rc.top = S(78); rc.right -= S(20); rc.bottom = rc.top + S(40);
        DrawTextW(dc, line, -1, &rc, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        if (p->icon) DrawIconEx(dc, S(28), S(124), p->icon, S(32), S(32), 0, NULL, DI_NORMAL);
        {
            static const WCHAR *const labels[] = { L"Name:", L"Publisher:", L"Path:" };
            const WCHAR *values[3] = { p->name, p->publisher, p->program };
            int i;
            for (i = 0; i < 3; i++) {
                RECT l = { S(76), S(122) + i * S(22), S(160), S(122) + i * S(22) + S(20) };
                RECT v = { S(160), l.top, S(560), l.bottom };
                DrawTextW(dc, labels[i], -1, &l, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX);
                DrawTextW(dc, values[i], -1, &v, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_PATH_ELLIPSIS);
            }
        }
        _snwprintf(line, ARRAYSIZE(line), L"Allow %ls to communicate on these networks:", p->name);
        rc.top = S(198); rc.bottom = rc.top + S(22);
        DrawTextW(dc, line, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        if (GetDlgCtrlID((HWND)lp) == ID_FW_RISKS) SetTextColor((HDC)wp, RGB(0x00, 0x66, 0xcc));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_FW_ALLOW: {
            WCHAR profiles[40], args[MAX_PATH + 300];
            BOOL a = SendMessageW(p->priv, BM_GETCHECK, 0, 0) == BST_CHECKED, b = SendMessageW(p->pub, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!a && !b) {
                MessageBoxW(hwnd, L"Tick the networks this app may communicate on, or choose Cancel.", L"Security Alert", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            profiles_arg(a, b, profiles, ARRAYSIZE(profiles));
            _snwprintf(args, ARRAYSIZE(args), L"/admin firewall-allow %ls \"%ls\" \"%ls\"", profiles, p->program, p->name);
            g_main = hwnd;
            if (!run_elevated(args)) return 0;     /* not allowed by an administrator: the question stays */
            p->result = 1;
            DestroyWindow(hwnd);
            return 0;
        }
        case IDCANCEL:
            p->result = 0;
            DestroyWindow(hwnd);
            return 0;
        case ID_FW_RISKS:
            ShellExecuteW(NULL, NULL, FW_DOCS, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        break;
    case WM_CLOSE:
        p->result = 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* the question's answer file beside the question: <id>.cancel holds the name */
static void prompt_cancel(const struct prompt *p)
{
    WCHAR path[MAX_PATH], *dot;
    HANDLE h;
    char name[400];
    DWORD n, w;
    lstrcpynW(path, p->ask, MAX_PATH);
    if (!(dot = wcsrchr(path, L'.'))) return;
    lstrcpyW(dot, L".cancel");
    n = WideCharToMultiByte(CP_UTF8, 0, p->name, -1, name, sizeof(name) - 2, NULL, NULL);
    if (!n) return;
    name[n - 1] = '\n';
    h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, name, n, &w, NULL);
    CloseHandle(h);
}

static void prompt_dump(const struct prompt *p, HWND hwnd)
{
    WCHAR path[MAX_PATH];
    FILE *f;
    RECT r;
    if (!GetEnvironmentVariableW(L"SG_FIREWALL_PROMPT_DUMP", path, MAX_PATH) || !(f = _wfopen(path, L"w, ccs=UTF-8"))) return;
    GetWindowRect(p->allow, &r);
    fwprintf(f, L"title=%ls\nheadline=Stained Glass Firewall has blocked some features of this app\nname=%ls\npublisher=%ls\npath=%ls\n"
             L"private=%d\npublic=%d\nallow_at=%ld,%ld\n", p->name, p->name, p->publisher, p->program,
             SendMessageW(p->priv, BM_GETCHECK, 0, 0) == BST_CHECKED, SendMessageW(p->pub, BM_GETCHECK, 0, 0) == BST_CHECKED,
             (r.left + r.right) / 2, (r.top + r.bottom) / 2);
    GetWindowRect(hwnd, &r);
    fwprintf(f, L"window=%ld,%ld,%ld,%ld\n", r.left, r.top, r.right, r.bottom);
    fclose(f);
}

int firewall_prompt_main(const WCHAR *ask)
{
    struct prompt p = { 0 };
    char *text, *line, *next, unix_path[MAX_PATH * 3];
    WNDCLASSW wc = { 0 };
    HWND hwnd, c;
    MSG msg;
    int w = S(560), h = S(400);
    WCHAR open_path[MAX_PATH], *dot;

    /* the question is ours now: <id>.open, so the network icon does not ask it again */
    lstrcpynW(p.ask, ask, MAX_PATH);
    lstrcpynW(open_path, ask, MAX_PATH);
    if ((dot = wcsrchr(open_path, L'.')) && !lstrcmpiW(dot, L".ask")) {
        lstrcpyW(dot, L".open");
        if (MoveFileExW(ask, open_path, MOVEFILE_REPLACE_EXISTING)) lstrcpynW(p.ask, open_path, MAX_PATH);
    }
    {
        HANDLE f = CreateFileW(p.ask, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
        DWORD got = 0;
        if (f == INVALID_HANDLE_VALUE) return 1;
        ReadFile(f, unix_path, sizeof(unix_path) - 1, &got, NULL);
        CloseHandle(f);
        unix_path[got] = 0;
        text = unix_path;
    }
    for (line = text; line && *line; line = next) {
        if ((next = strchr(line, '\n'))) *next++ = 0;
        if (!strncmp(line, "program=", 8)) MultiByteToWideChar(CP_UTF8, 0, line + 8, -1, p.program, MAX_PATH);
        else if (!strncmp(line, "kind=", 5)) p.linux_app = !strcmp(line + 5, "linux");
        else if (!strncmp(line, "category=", 9)) MultiByteToWideChar(CP_UTF8, 0, line + 9, -1, p.category, 16);
    }
    if (!p.program[0]) return 1;
    if (p.linux_app) {
        WCHAR dos[MAX_PATH];
        char u[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, p.program, -1, u, sizeof(u), NULL, NULL);
        unix_to_dos(u, dos, MAX_PATH);
        program_name(p.program, p.name, ARRAYSIZE(p.name), p.publisher, ARRAYSIZE(p.publisher));
        ExtractIconExW(dos, 0, &p.icon, NULL, 1);
    } else {
        program_name(p.program, p.name, ARRAYSIZE(p.name), p.publisher, ARRAYSIZE(p.publisher));
        ExtractIconExW(p.program, 0, &p.icon, NULL, 1);
    }
    if (!p.icon) p.icon = LoadIconW(NULL, (const WCHAR *)IDI_APPLICATION);
    g_prompt = &p;

    wc.lpfnWndProc = prompt_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.lpszClassName = L"SgFirewallPrompt";
    wc.hIcon = LoadIconW(NULL, (const WCHAR *)IDI_SHIELD);
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST | WS_EX_CONTROLPARENT, wc.lpszClassName, L"Security Alert",
                           WS_POPUP | WS_CAPTION | WS_SYSMENU, (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
                           (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h, NULL, NULL, g_inst, NULL);
    if (!hwnd) return 1;
    {
        RECT rc;
        int cw;
        GetClientRect(hwnd, &rc);
        cw = rc.right;
        p.priv = CreateWindowW(L"BUTTON", L"Private networks, such as my home or work network",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, S(36), S(222), cw - S(56), S(22),
                               hwnd, (HMENU)ID_FW_PRIV, g_inst, NULL);
        p.pub = CreateWindowW(L"BUTTON", L"Public networks, such as those in airports and coffee shops (not recommended "
                              L"because these networks often have little or no security)",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX | BS_MULTILINE | BS_TOP, S(36), S(246),
                              cw - S(80), S(40), hwnd, (HMENU)ID_FW_PUB, g_inst, NULL);
        /* a link, as Windows has it */
        c = CreateWindowW(L"STATIC", L"What are the risks of allowing an app through a firewall?",
                          WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_LEFT | SS_NOPREFIX, S(20), rc.bottom - S(84),
                          cw - S(40), S(20), hwnd, (HMENU)ID_FW_RISKS, g_inst, NULL);
        SetClassLongPtrW(c, GCLP_HCURSOR, (LONG_PTR)LoadCursorW(NULL, (const WCHAR *)IDC_HAND));
        SendMessageW(c, WM_SETFONT, (WPARAM)g_font_small, TRUE);
        p.allow = CreateWindowW(L"BUTTON", L"Allow access", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                cw - S(220), rc.bottom - S(42), S(110), S(30), hwnd, (HMENU)ID_FW_ALLOW, g_inst, NULL);
        SendMessageW(p.allow, BCM_SETSHIELD, 0, TRUE);
        c = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                          cw - S(100), rc.bottom - S(42), S(84), S(30), hwnd, (HMENU)IDCANCEL, g_inst, NULL);
        SendMessageW(p.priv, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(p.pub, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(p.allow, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_font_body, TRUE);
        /* as Windows: the network this PC is on now is ticked */
#ifndef SG_MUTANT_FW_PROMPT_TICKS_NOTHING
        SendMessageW(lstrcmpW(p.category, L"public") ? p.priv : p.pub, BM_SETCHECK, BST_CHECKED, 0);
#endif
        SetFocus(c);
    }
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd);
    UpdateWindow(hwnd);
    prompt_dump(&p, hwnd);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (!p.result) prompt_cancel(&p);
    (void)unix_path;
    return p.result ? 0 : 1;
}
