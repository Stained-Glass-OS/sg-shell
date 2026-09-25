/* sg-control -- Programs and Features: what is installed, from the
 * Uninstall keys installers write, and their own uninstallers.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"
#include <shellapi.h>

enum { ID_LIST = CMD_PAGE_FIRST + 1, ID_UNINSTALL, ID_CHANGE, ID_REPAIR, CMD_DONE };


static struct program *g_progs;
static int g_nprogs;
static HWND g_list, g_btn_un, g_btn_ch, g_btn_rep;
static int g_detail_y;

static const WCHAR UNINSTALL[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

static BOOL is_guid(const WCHAR *s)
{
    return lstrlenW(s) == 38 && s[0] == L'{' && s[37] == L'}';
}

/* a string value, or empty */
static void qsz(HKEY k, const WCHAR *name, WCHAR *out, DWORD cch)
{
    DWORD cb = (cch - 1) * sizeof(WCHAR), t;
    if (RegQueryValueExW(k, name, NULL, &t, (BYTE *)out, &cb) != ERROR_SUCCESS || (t != REG_SZ && t != REG_EXPAND_SZ))
        out[0] = 0;
    else out[cb / sizeof(WCHAR) < cch ? cb / sizeof(WCHAR) : cch - 1] = 0;
}

static DWORD qdw(HKEY k, const WCHAR *name, DWORD def)
{
    DWORD v = def, cb = sizeof(v), t;
    if (RegQueryValueExW(k, name, NULL, &t, (BYTE *)&v, &cb) != ERROR_SUCCESS || t != REG_DWORD) v = def;
    return v;
}

static void read_key(HKEY parent, const WCHAR *sub, const WCHAR *scope)
{
    HKEY k;
    struct program p;
    WCHAR release[64] = L"", parent_key[64] = L"";
    DWORD date;
    if (RegOpenKeyExW(parent, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    memset(&p, 0, sizeof(p));
    lstrcpynW(p.key, sub, ARRAYSIZE(p.key));
    p.scope = scope;
#define SZ(name, field) qsz(k, name, p.field, ARRAYSIZE(p.field))
#define DW(name, def) qdw(k, name, def)
    SZ(L"DisplayName", name);
    if (!p.name[0] || DW(L"SystemComponent", 0) == 1) { RegCloseKey(k); return; }
    {
        DWORD cb = sizeof(release) - sizeof(WCHAR), t;
        if (RegQueryValueExW(k, L"ReleaseType", NULL, &t, (BYTE *)release, &cb) != ERROR_SUCCESS) release[0] = 0;
        cb = sizeof(parent_key) - sizeof(WCHAR);
        if (RegQueryValueExW(k, L"ParentKeyName", NULL, &t, (BYTE *)parent_key, &cb) != ERROR_SUCCESS) parent_key[0] = 0;
    }
    /* updates to a program are listed under "installed updates", not here */
    if (parent_key[0] || !lstrcmpiW(release, L"Update") || !lstrcmpiW(release, L"Hotfix") ||
        !lstrcmpiW(release, L"Security Update")) { RegCloseKey(k); return; }
    SZ(L"Publisher", publisher);
    SZ(L"DisplayVersion", version);
    SZ(L"UninstallString", uninstall);
    SZ(L"QuietUninstallString", quiet);
    SZ(L"ModifyPath", modify);
    SZ(L"DisplayIcon", icon);
    SZ(L"HelpLink", help);
    if (!p.help[0]) SZ(L"URLInfoAbout", help);
    {
        WCHAR d[16] = L"";
        DWORD cb = sizeof(d) - sizeof(WCHAR), t;
        if (RegQueryValueExW(k, L"InstallDate", NULL, &t, (BYTE *)d, &cb) == ERROR_SUCCESS && t == REG_SZ && lstrlenW(d) == 8) {
            SYSTEMTIME st = { 0 };
            st.wYear = (WORD)_wtoi(((WCHAR[5]){ d[0], d[1], d[2], d[3], 0 }));
            st.wMonth = (WORD)_wtoi(((WCHAR[3]){ d[4], d[5], 0 }));
            st.wDay = (WORD)_wtoi(((WCHAR[3]){ d[6], d[7], 0 }));
            if (!GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, p.date, ARRAYSIZE(p.date))) p.date[0] = 0;
        }
    }
    date = DW(L"EstimatedSize", 0);
    p.size_kb = date;
    p.msi = DW(L"WindowsInstaller", 0) == 1 && is_guid(sub);
    p.no_modify = DW(L"NoModify", 0) == 1;
    p.no_repair = DW(L"NoRepair", 0) == 1;
    p.no_remove = DW(L"NoRemove", 0) == 1;
#undef SZ
#undef DW
    RegCloseKey(k);
    {
        struct program *n = realloc(g_progs, (g_nprogs + 1) * sizeof(*n));
        if (!n) return;
        g_progs = n;
        g_progs[g_nprogs++] = p;
    }
}

static void read_root(HKEY root, REGSAM view, const WCHAR *scope)
{
    HKEY k;
    WCHAR sub[256];
    DWORD i, n;
    if (RegOpenKeyExW(root, UNINSTALL, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return;
    for (i = 0; n = ARRAYSIZE(sub), RegEnumKeyExW(k, i, sub, &n, NULL, NULL, NULL, NULL) == ERROR_SUCCESS; i++)
        read_key(k, sub, scope);
    RegCloseKey(k);
}

static int by_name(const void *a, const void *b)
{
    return lstrcmpiW(((const struct program *)a)->name, ((const struct program *)b)->name);
}

static void load_programs(void)
{
    free(g_progs);
    g_progs = NULL;
    g_nprogs = 0;
    read_root(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, L"machine");
    read_root(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, L"machine (32-bit)");
    read_root(HKEY_CURRENT_USER, 0, L"user");
    qsort(g_progs, g_nprogs, sizeof(*g_progs), by_name);
}

int program_count(void)
{
    load_programs();
    return g_nprogs;
}

/* for Settings > Apps */
int prog_load(void) { load_programs(); return g_nprogs; }
const struct program *prog_get(int i) { return i >= 0 && i < g_nprogs ? &g_progs[i] : NULL; }

/* the command Windows would run for this action */
static BOOL command_for(const struct program *p, int action, WCHAR *out, int cch);
BOOL prog_can(const struct program *p, int action)
{
    WCHAR cmd[1100];
    return command_for(p, action, cmd, ARRAYSIZE(cmd));
}

static BOOL command_for(const struct program *p, int action, WCHAR *out, int cch)
{
    out[0] = 0;
    switch (action) {
    case ID_UNINSTALL:
        if (p->no_remove) return FALSE;
        if (p->uninstall[0]) ExpandEnvironmentStringsW(p->uninstall, out, cch);
        else if (p->msi) _snwprintf(out, cch, L"msiexec.exe /x%ls", p->key);
        break;
    case ID_CHANGE:
        if (p->no_modify) return FALSE;
        if (p->modify[0]) ExpandEnvironmentStringsW(p->modify, out, cch);
        else if (p->msi) _snwprintf(out, cch, L"msiexec.exe /i%ls", p->key);
        break;
    case ID_REPAIR:
        if (p->no_repair || !p->msi) return FALSE;
        _snwprintf(out, cch, L"msiexec.exe /fomus%ls", p->key);
        break;
    }
    out[cch - 1] = 0;
    return out[0] != 0;
}

struct waiter { HANDLE process; };
static DWORD WINAPI wait_thread(void *arg)
{
    HANDLE h = arg;
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);
    if (g_page) PostMessageW(g_page, WM_COMMAND, CMD_DONE, 0);
    return 0;
}

/* Run it; with wait, block until it ends. Returns FALSE if it did not start. */
BOOL prog_run(const struct program *p, int action, BOOL wait)
{
    WCHAR cmd[1100];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    if (!command_for(p, action, cmd, ARRAYSIZE(cmd))) return FALSE;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        /* some write a document or a bare path: let the shell open it */
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        sei.lpFile = cmd;
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) return FALSE;
        pi.hProcess = sei.hProcess;
        pi.hThread = NULL;
    }
    if (pi.hThread) CloseHandle(pi.hThread);
    if (!pi.hProcess) return TRUE;
    if (wait) { WaitForSingleObject(pi.hProcess, INFINITE); CloseHandle(pi.hProcess); }
    else { HANDLE t = CreateThread(NULL, 0, wait_thread, pi.hProcess, 0, NULL); if (t) CloseHandle(t); else CloseHandle(pi.hProcess); }
    return TRUE;
}

static int selected(void)
{
    int i = (int)SendMessageW(g_list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    LVITEMW it = { LVIF_PARAM, i };
    if (i < 0 || !SendMessageW(g_list, LVM_GETITEMW, 0, (LPARAM)&it)) return -1;
    return (int)it.lParam;
}

static HWND g_foot1, g_foot2;
static WCHAR g_totals[2][128];

static void show_details(void)
{
    int s = selected();
    WCHAR a[512], b[512], size[32] = L"";
    EnableWindow(g_btn_un, s >= 0 && !g_progs[s].no_remove && (g_progs[s].uninstall[0] || g_progs[s].msi));
    EnableWindow(g_btn_ch, s >= 0 && !g_progs[s].no_modify && (g_progs[s].modify[0] || g_progs[s].msi));
    EnableWindow(g_btn_rep, s >= 0 && g_progs[s].msi && !g_progs[s].no_repair);
    /* the footer: the selected program, or the totals */
    if (s < 0) { SetWindowTextW(g_foot1, g_totals[0]); SetWindowTextW(g_foot2, g_totals[1]); return; }
    if (g_progs[s].size_kb) format_size((ULONGLONG)g_progs[s].size_kb * 1024, size, ARRAYSIZE(size));
    _snwprintf(a, ARRAYSIZE(a), L"%ls    Product version: %ls    Size: %ls", g_progs[s].publisher[0] ? g_progs[s].publisher : g_progs[s].name,
               g_progs[s].version[0] ? g_progs[s].version : L"-", size[0] ? size : L"-");
    _snwprintf(b, ARRAYSIZE(b), L"Help link: %ls", g_progs[s].help[0] ? g_progs[s].help : L"-");
    a[ARRAYSIZE(a) - 1] = b[ARRAYSIZE(b) - 1] = 0;
    SetWindowTextW(g_foot1, a);
    SetWindowTextW(g_foot2, b);
}

void build_programs(void)
{
    static const WCHAR *const labels[] = { L"View installed updates", NULL, L"See also", L"Windows Update", L"System" };
    static const int ids[] = { NAV(PG_UPDATE), 0, -1, NAV(PG_UPDATE), NAV(PG_SYSTEM) };
    int x = pg_left_pane(labels, ids, ARRAYSIZE(labels)) + S(28), y = S(24), w = pg_width() - x - S(24), h = pg_height();
    LVCOLUMNW col = { LVCF_TEXT | LVCF_WIDTH };
    HIMAGELIST il;
    int i;
    ULONGLONG total = 0;
    WCHAR line[128];

    load_programs();
    pg_title(x, y, L"Uninstall or change a program");
    y += S(34);
    pg_para(x, y, w, g_font_body, COL_TEXT, L"To uninstall a program, select it from the list and then click Uninstall, Change, or Repair.");
    y += S(30);
    g_btn_un = pg_button(L"Uninstall", x, y, S(100), ID_UNINSTALL);
    g_btn_ch = pg_button(L"Change", x + S(108), y, S(90), ID_CHANGE);
    g_btn_rep = pg_button(L"Repair", x + S(206), y, S(90), ID_REPAIR);
    y += S(38);

    g_detail_y = h - S(76);
    g_list = pg_control(WC_LISTVIEWW, L"", WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
                        x, y, w, g_detail_y - y - S(12), ID_LIST);
    SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    col.cx = w * 34 / 100; col.pszText = L"Name"; SendMessageW(g_list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.cx = w * 24 / 100; col.pszText = L"Publisher"; SendMessageW(g_list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    col.cx = w * 13 / 100; col.pszText = L"Installed On"; SendMessageW(g_list, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
    col.cx = w * 11 / 100; col.pszText = L"Size"; col.mask |= LVCF_FMT; col.fmt = LVCFMT_RIGHT; SendMessageW(g_list, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);
    col.mask &= ~LVCF_FMT;
    col.cx = w * 15 / 100; col.pszText = L"Version"; SendMessageW(g_list, LVM_INSERTCOLUMNW, 4, (LPARAM)&col);

    il = ImageList_Create(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), ILC_COLOR32 | ILC_MASK, g_nprogs + 1, 4);
    ImageList_AddIcon(il, LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION));
    SendMessageW(g_list, LVM_SETIMAGELIST, LVSIL_SMALL, (LPARAM)il);
    for (i = 0; i < g_nprogs; i++) {
        struct program *p = &g_progs[i];
        LVITEMW it = { LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE, i };
        WCHAR size[32] = L"", file[MAX_PATH + 8], *comma;
        HICON icon = NULL;
        int img = 0;
        if (p->icon[0]) {
            int idx = 0;
            ExpandEnvironmentStringsW(p->icon, file, ARRAYSIZE(file));
            if ((comma = wcsrchr(file, L','))) { idx = _wtoi(comma + 1); *comma = 0; }
            if (file[0] == L'"') { memmove(file, file + 1, lstrlenW(file) * sizeof(WCHAR)); if ((comma = wcschr(file, L'"'))) *comma = 0; }
            if (ExtractIconExW(file, idx, NULL, &icon, 1) == 1 && icon) { img = ImageList_AddIcon(il, icon); DestroyIcon(icon); }
        }
        if (p->size_kb) { format_size((ULONGLONG)p->size_kb * 1024, size, ARRAYSIZE(size)); total += (ULONGLONG)p->size_kb * 1024; }
        it.pszText = p->name; it.lParam = i; it.iImage = img;
        SendMessageW(g_list, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.mask = LVIF_TEXT;
        it.iSubItem = 1; it.pszText = p->publisher; SendMessageW(g_list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 2; it.pszText = p->date; SendMessageW(g_list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 3; it.pszText = size; SendMessageW(g_list, LVM_SETITEMW, 0, (LPARAM)&it);
        it.iSubItem = 4; it.pszText = p->version; SendMessageW(g_list, LVM_SETITEMW, 0, (LPARAM)&it);
    }
    /* the footer: how many, how big -- or the selected program */
    pg_icon(x, g_detail_y, S(40), IC_PROG);
    if (total) { WCHAR t[32]; format_size(total, t, ARRAYSIZE(t)); _snwprintf(g_totals[0], 128, L"Currently installed programs    Total size: %ls", t); }
    else lstrcpyW(g_totals[0], L"Currently installed programs");
    _snwprintf(g_totals[1], 128, L"%d %ls installed", g_nprogs, g_nprogs == 1 ? L"program" : L"programs");
    g_foot1 = pg_control(L"STATIC", g_totals[0], SS_NOPREFIX | SS_ENDELLIPSIS, x + S(52), g_detail_y, w - S(52), S(20), -1);
    g_foot2 = pg_control(L"STATIC", g_totals[1], SS_NOPREFIX | SS_ENDELLIPSIS, x + S(52), g_detail_y + S(22), w - S(52), S(20), -1);
    (void)line;
    show_details();
}

BOOL cmd_programs(int id, int code, HWND ctl)
{
    int s;
    (void)code; (void)ctl;
    switch (id) {
    case ID_UNINSTALL: case ID_CHANGE: case ID_REPAIR:
        if ((s = selected()) < 0) return TRUE;
        if (!prog_run(&g_progs[s], id, FALSE)) {
            WCHAR msg[512];
            _snwprintf(msg, ARRAYSIZE(msg), L"%ls could not be %ls: its %ls program could not be started.", g_progs[s].name,
                       id == ID_UNINSTALL ? L"uninstalled" : id == ID_CHANGE ? L"changed" : L"repaired",
                       id == ID_UNINSTALL ? L"uninstall" : L"setup");
            message(g_main, L"Programs and Features", msg, TRUE);
        }
        return TRUE;
    case CMD_DONE:
        if (current_page() == PG_PROGRAMS) refresh_page();
        return TRUE;
    }
    return FALSE;
}

LRESULT notify_programs(NMHDR *nm)
{
    if (nm->hwndFrom != g_list) return 0;
    if (nm->code == LVN_ITEMCHANGED) show_details();
    else if (nm->code == NM_DBLCLK && selected() >= 0) cmd_programs(ID_UNINSTALL, 0, NULL);
    return 0;
}

void dump_programs(void)
{
    int i;
    WCHAR cmd[1100];
    load_programs();
    wprintf(L"programs.count=%d\n", g_nprogs);
    for (i = 0; i < g_nprogs; i++) {
        const struct program *p = &g_progs[i];
        command_for(p, ID_UNINSTALL, cmd, ARRAYSIZE(cmd));
        wprintf(L"program=%ls|%ls|%ls|%ls|%lu|%ls|%ls\n", p->name, p->publisher, p->version, p->date,
                (unsigned long)p->size_kb, p->scope, cmd);
    }
}

/* --uninstall NAME: what the Uninstall button does, for the gate and scripts */
int programs_uninstall_cli(const WCHAR *name)
{
    int i;
    load_programs();
    for (i = 0; i < g_nprogs; i++)
        if (!lstrcmpiW(g_progs[i].name, name)) return prog_run(&g_progs[i], ID_UNINSTALL, TRUE) ? 0 : 1;
    return 3;
}
