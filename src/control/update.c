/* sg-control -- Windows Update: the machine's updates, as Stained Glass
 * handles them (sg-session's staged updates): downloaded daily by
 * sg-update-prepare, installed by PackageKit at the next restart.
 *
 *   /system-update                          an update is staged for the next boot
 *   /var/lib/systemd/timers/stamp-sg-update-prepare.timer   when it last checked
 *   /etc/apt/sources.list.d/NAME.sources    where updates come from
 *   /var/log/apt/history.log                what was installed, and when
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"

enum { CMD_CHECK = SHIELD_ID(CMD_PAGE_FIRST + 1), CMD_RESTART = CMD_PAGE_FIRST + 2, CMD_HISTORY = CMD_PAGE_FIRST + 3 };


static BOOL unix_mtime(const char *path, SYSTEMTIME *st)
{
    WCHAR p[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA a;
    FILETIME local;
    unix_to_dos(path, p, MAX_PATH);
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &a)) return FALSE;
    FileTimeToLocalFileTime(&a.ftLastWriteTime, &local);
    return FileTimeToSystemTime(&local, st);
}

static void read_sources(struct ufacts *u)
{
    WCHAR pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    unix_to_dos("/etc/apt/sources.list.d", pattern, MAX_PATH);
    lstrcatW(pattern, L"\\*.sources");
    if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        char path[512], *text, *line, *next;
        char name[260];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL);
        _snprintf(path, sizeof(path), "/etc/apt/sources.list.d/%s", name);
        path[sizeof(path) - 1] = 0;
        if (!(text = read_unix_file(path, NULL))) continue;
        for (line = text; line && *line && u->nsources < (int)ARRAYSIZE(u->sources); line = next) {
            next = strchr(line, '\n');
            if (next) *next++ = 0;
            if (!strncmp(line, "URIs:", 5)) {
                char *v = line + 5;
                while (*v == ' ') v++;
                MultiByteToWideChar(CP_UTF8, 0, v, -1, u->sources[u->nsources++], 256);
            }
        }
        free(text);
    } while (u->nsources < (int)ARRAYSIZE(u->sources) && FindNextFileW(h, &fd));
    FindClose(h);
}

void update_gather(struct ufacts *u)
{
    SYSTEMTIME st;
    memset(u, 0, sizeof(*u));
    u->pending = unix_path_exists("/system-update");
    if ((u->checked = unix_mtime("/var/lib/systemd/timers/stamp-sg-update-prepare.timer", &st))) {
        WCHAR d[48], t[32];
        GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &st, NULL, d, ARRAYSIZE(d));
        GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, t, ARRAYSIZE(t));
        _snwprintf(u->last, ARRAYSIZE(u->last), L"%ls, %ls", d, t);
    }
    {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Policies\\Microsoft\\Windows\\WindowsUpdate", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            u->managed = TRUE;
            RegCloseKey(k);
        }
    }
    read_sources(u);
}

/* the last entries of apt's history: when, and what */

int update_history(struct hentry *out, int max)
{
    char *text = read_unix_file("/var/log/apt/history.log", NULL), *line, *next;
    struct hentry all[64];
    int n = 0, i;
    if (!text) return 0;
    for (line = text; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strncmp(line, "Start-Date: ", 12)) {
            if (n == (int)ARRAYSIZE(all)) { memmove(all, all + 1, sizeof(all) - sizeof(all[0])); n--; }
            memset(&all[n], 0, sizeof(all[n]));
            MultiByteToWideChar(CP_UTF8, 0, line + 12, -1, all[n].when, ARRAYSIZE(all[n].when));
            n++;
        } else if (n && (!strncmp(line, "Upgrade: ", 9) || !strncmp(line, "Install: ", 9) || !strncmp(line, "Remove: ", 8))) {
            /* "Upgrade: pkg:arch (old, new), ..." -> "Upgraded pkg, pkg, ..." */
            char *p = strchr(line, ':') + 2, summary[400];
            size_t len = 0;
            int count = 0;
            len = _snprintf(summary, sizeof(summary), "%s ", line[0] == 'U' ? "Updated" : line[0] == 'I' ? "Installed" : "Removed");
            while (p && *p && len < sizeof(summary) - 40) {
                char *end = strchr(p, ' '), *colon;
                if (!end) break;
                *end = 0;
                if ((colon = strchr(p, ':'))) *colon = 0;
                len += _snprintf(summary + len, sizeof(summary) - len, "%s%s", count ? ", " : "", p);
                count++;
                p = strstr(end + 1, "), ");
                p = p ? p + 3 : NULL;
            }
            if (p && *p) _snprintf(summary + len, sizeof(summary) - len, ", ...");
            summary[sizeof(summary) - 1] = 0;
            if (!all[n - 1].what[0]) MultiByteToWideChar(CP_UTF8, 0, summary, -1, all[n - 1].what, ARRAYSIZE(all[n - 1].what));
        }
    }
    free(text);
    /* newest first */
    for (i = 0; i < n && i < max; i++) out[i] = all[n - 1 - i];
    return i;
}

void build_update(void)
{
    static const WCHAR *const labels[] = { L"Check for updates", L"View update history", NULL, L"See also", L"Programs and Features" };
    static const int ids[] = { CMD_CHECK, CMD_HISTORY, 0, -1, NAV(PG_PROGRAMS) };
    struct ufacts u;
    struct hentry h[10];
    int x = pg_left_pane(labels, ids, ARRAYSIZE(labels)) + S(36), y = S(24), w = pg_width() - x - S(40), i, nh;
    WCHAR line[512];

    update_gather(&u);
    pg_title(x, y, L"Updates");
    y += S(52);
    if (u.managed) {
        pg_text(x, y, w, S(20), g_font_body, COL_WARN, L"*Some settings are managed by your organization.", DT_SINGLELINE);
        y += S(30);
    }
    pg_fill(x, y, w, S(118), COL_PANE);
    pg_icon(x + S(20), y + S(20), S(56), u.pending ? IC_UPDATE : IC_OK);
    pg_text(x + S(96), y + S(18), w - S(116), S(26), g_font_title, COL_TEXT,
            u.pending ? L"Restart required" : L"You're up to date", DT_SINGLELINE);
    if (u.pending) lstrcpyW(line, L"Updates have been downloaded. Restart your computer to install them.");
    else if (u.checked) _snwprintf(line, ARRAYSIZE(line), L"Last checked: %ls", u.last);
    else lstrcpyW(line, L"This computer has not checked for updates yet. It checks every day.");
    pg_text(x + S(96), y + S(50), w - S(116), S(20), g_font_body, COL_SUBTLE, line, DT_SINGLELINE | DT_END_ELLIPSIS);
    if (u.pending) pg_button(L"Restart now", x + S(96), y + S(78), S(130), CMD_RESTART);
    else pg_link(x + S(94), y + S(80), L"Check for updates", CMD_CHECK, LINK_SHIELD);
    y += S(140);

    pg_para(x, y, w, g_font_body, COL_TEXT,
            L"Updates are downloaded automatically every day and installed the next time you restart, before anyone signs in, "
            L"so nothing changes under a running program.");
    y += S(44);
    pg_text(x, y, w, S(20), g_font_head, COL_TEXT, L"Updates come from", DT_SINGLELINE);
    y += S(24);
    for (i = 0; i < u.nsources; i++) {
        pg_text(x + S(16), y, w - S(16), S(20), g_font_body, COL_SUBTLE, u.sources[i], DT_SINGLELINE | DT_END_ELLIPSIS);
        y += S(22);
    }
    if (!u.nsources) { pg_text(x + S(16), y, w, S(20), g_font_body, COL_SUBTLE, L"(no update sources are configured)", DT_SINGLELINE); y += S(22); }
    y += S(20);

    pg_text(x, y, w, S(24), g_font_cat, COL_TITLE, L"Update history", DT_SINGLELINE);
    pg_rule(x, y + S(26), w);
    y += S(38);
    nh = update_history(h, ARRAYSIZE(h));
    for (i = 0; i < nh; i++) {
        pg_text(x + S(16), y, S(170), S(20), g_font_body, COL_SUBTLE, h[i].when, DT_SINGLELINE);
        pg_text(x + S(190), y, w - S(190), S(20), g_font_body, COL_TEXT, h[i].what[0] ? h[i].what : L"(no packages changed)",
                DT_SINGLELINE | DT_END_ELLIPSIS);
        y += S(24);
    }
    if (!nh) pg_text(x + S(16), y, w, S(20), g_font_body, COL_SUBTLE, L"No updates have been installed yet.", DT_SINGLELINE);
}

BOOL cmd_update(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    switch (id) {
    case CMD_CHECK: if (run_elevated(L"/admin update-check")) refresh_when_back(); return TRUE;
    case CMD_RESTART:
        if (MessageBoxW(g_main, L"Restart now to install the updates? Save your work first.", L"Updates",
                        MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
            ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_FLAG_PLANNED);
        return TRUE;
    case CMD_HISTORY: page_scroll_to(S(300)); return TRUE;
    }
    return FALSE;
}

void dump_update(void)
{
    struct ufacts u;
    struct hentry h[10];
    int i, nh;
    update_gather(&u);
    wprintf(L"update.pending=%ls\n", u.pending ? L"yes" : L"no");
    wprintf(L"update.lastcheck=%ls\n", u.checked ? u.last : L"never");
    wprintf(L"update.managed=%ls\n", u.managed ? L"yes" : L"no");
    for (i = 0; i < u.nsources; i++) wprintf(L"update.source=%ls\n", u.sources[i]);
    nh = update_history(h, ARRAYSIZE(h));
    for (i = 0; i < nh; i++) wprintf(L"update.history=%ls|%ls\n", h[i].when, h[i].what);
}
