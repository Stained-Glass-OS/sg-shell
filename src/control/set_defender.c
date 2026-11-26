/* sg-control -- Settings > Update & Security > Virus & threat protection.
 *
 * SG Defender (David 2026-10-03, 2026-11-25): what the SG Store installs is
 * the makers' own software, checked against its fingerprint, and Linux apps
 * come from signed repositories; everything else is scanned by ClamAV
 * (sg-session's sg-defender, as root, with clamd's definitions loaded):
 * each program, DLL, installer and script when it starts (wine-sg asks the
 * service first), downloads as they arrive, and the places malware starts
 * from in a quick scan each day. This page is Windows Security's: the scans
 * (Quick, Full, Custom -- sg-defender-scan, run as this person), Protection
 * history (what was found in this person's files and their scans), and the
 * settings an administrator changes through sg-admind: Real-time protection
 * (off for a while only, as on Windows), SG Defender itself (off gives the
 * memory back), and Exclusions (an elevated window: only administrators see
 * them, as on Windows).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"
#include <shlobj.h>

#define DEFENDER_DIR L"Z:\\var\\lib\\stained-glass\\defender"   /* SG_DEFENDER_DIR: the gate's */
#define DEFENDER_DOCS L"https://freesoft.page/docs/guide/06-viruses.html"
#define SCAN_TOOL L"/usr/bin/sg-defender-scan"                   /* SG_DEFENDER_SCAN_TOOL: the gate's */

enum {
    CMD_DEF_TOGGLE = SHIELD_ID(CMD_PAGE_FIRST + 1), CMD_DEF_LEARN = CMD_PAGE_FIRST + 2,
    CMD_DEF_REALTIME = SHIELD_ID(CMD_PAGE_FIRST + 3), CMD_DEF_EXCLUSIONS = SHIELD_ID(CMD_PAGE_FIRST + 4),
    CMD_DEF_QUICK = CMD_PAGE_FIRST + 5, CMD_DEF_FULL = CMD_PAGE_FIRST + 6, CMD_DEF_CUSTOM = CMD_PAGE_FIRST + 7,
    CMD_DEF_RESTORE = SHIELD_ID(CMD_PAGE_FIRST + 100),     /* + the item */
    CMD_DEF_DELETE = SHIELD_ID(CMD_PAGE_FIRST + 200),
};

static char *slurp(const WCHAR *path, DWORD max)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    char *buf;
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!(buf = malloc(max + 1))) { CloseHandle(h); return NULL; }
    ReadFile(h, buf, max, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    return buf;
}

/* a JSON string or number field, crudely: what sg-defender writes, one per key */
static BOOL json_field(const char *text, const char *key, WCHAR *out, int cch)
{
    char pat[64];
    const char *p, *e;
    int n;
    out[0] = 0;
    if (!text) return FALSE;
    _snprintf(pat, sizeof(pat), "\"%s\": ", key);
    if (!(p = strstr(text, pat))) return FALSE;
    p += strlen(pat);
    if (*p == '"') { p++; e = strchr(p, '"'); }
    else e = p + strcspn(p, ",\n}");
    if (!e) return FALSE;
    n = MultiByteToWideChar(CP_UTF8, 0, p, (int)(e - p), out, cch - 1);
    out[n] = 0;
    return TRUE;
}

/* the next object of a JSON array (scans.json): [*start, *end) of its text */
static BOOL json_next_object(const char **pos, const char **start, const char **end)
{
    const char *p = *pos;
    int depth = 0;
    BOOL str = FALSE;
    for (; *p; p++) {
        if (str) { if (*p == '\\' && p[1]) p++; else if (*p == '"') str = FALSE; continue; }
        if (*p == '"') str = TRUE;
        else if (*p == '{') { if (depth++ == 0) *start = p; }
        else if (*p == '}' && depth > 0 && --depth == 0) { *end = p + 1; *pos = p + 1; return TRUE; }
    }
    return FALSE;
}

/* this person's Unix account: the notices are kept by it */
static unsigned long unix_uid(void)
{
    char *t = slurp(L"Z:\\proc\\self\\status", 4096), *p;
    unsigned long uid = (unsigned long)-1;
    if (t && (p = strstr(t, "\nUid:"))) uid = strtoul(p + 5, NULL, 10);
    free(t);
    return uid;
}

static const WCHAR *defender_dir(void)
{
    static WCHAR dir[MAX_PATH];
    if (!dir[0] && !GetEnvironmentVariableW(L"SG_DEFENDER_DIR", dir, MAX_PATH)) lstrcpyW(dir, DEFENDER_DIR);
    return dir;
}

struct found { WCHAR id[32], name[MAX_PATH], signature[128], time[32], folder[MAX_PATH], how[16], action[16]; };
static struct found g_items[10];   /* what the page shows, for its buttons */
static int g_nitems;
static BOOL g_scan_running;

static int found_items(struct found *items, int max)
{
    WCHAR pat[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0, i, j;
    _snwprintf(pat, MAX_PATH, L"%ls\\notices\\%lu\\*.json", defender_dir(), unix_uid());
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        char *t;
        if (n >= max) break;
        if (!lstrcmpiW(fd.cFileName, L"scans.json")) continue;
        _snwprintf(path, MAX_PATH, L"%ls\\notices\\%lu\\%ls", defender_dir(), unix_uid(), fd.cFileName);
        if (!(t = slurp(path, 8192))) continue;
        json_field(t, "id", items[n].id, 32);
        json_field(t, "name", items[n].name, MAX_PATH);
        json_field(t, "signature", items[n].signature, 128);
        json_field(t, "time", items[n].time, 32);
        json_field(t, "folder", items[n].folder, MAX_PATH);
        if (!json_field(t, "how", items[n].how, 16)) lstrcpyW(items[n].how, L"download");
        if (!json_field(t, "action", items[n].action, 16)) lstrcpyW(items[n].action, L"quarantined");
        free(t);
        if (items[n].name[0] && items[n].id[0]) n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (i = 1; i < n; i++)          /* newest first */
        for (j = i; j > 0 && lstrcmpW(items[j - 1].time, items[j].time) < 0; j--) {
            struct found tmp = items[j]; items[j] = items[j - 1]; items[j - 1] = tmp;
        }
    return n;
}

static const WCHAR *how_text(const WCHAR *how)
{
    if (!lstrcmpW(how, L"image")) return L"stopped when it started";
    if (!lstrcmpW(how, L"msi")) return L"stopped when it was installed";
    if (!lstrcmpW(how, L"script")) return L"stopped when the script ran";
    if (!lstrcmpW(how, L"amsi")) return L"script stopped";
    if (!lstrcmpW(how, L"scan")) return L"found by a scan";
    return L"found when it was downloaded";
}

static const WCHAR *scan_name(const WCHAR *type)
{
    if (!lstrcmpW(type, L"full")) return L"Full scan";
    if (!lstrcmpW(type, L"custom")) return L"Custom scan";
    if (!lstrcmpW(type, L"scheduled")) return L"Scheduled scan";
    return L"Quick scan";
}

/* "2026-11-25T11:38:37" as "2026-11-25 11:38" */
static void when(const WCHAR *iso, WCHAR *out, int cch)
{
    WCHAR *t;
    lstrcpynW(out, iso, cch);
    if ((t = wcschr(out, L'T'))) *t = L' ';
    if (lstrlenW(out) > 16) out[16] = 0;
}

/* the person's scans (scans.json, newest first): the history lines; says
 * whether one is running */
static int scans_history(int y)
{
    WCHAR path[MAX_PATH], type[16], state[16], start[32], files[16], found[16], line[300], at[32];
    const char *pos, *s, *e;
    char *t, *obj;
    int shown = 0;
    _snwprintf(path, MAX_PATH, L"%ls\\notices\\%lu\\scans.json", defender_dir(), unix_uid());
    g_scan_running = FALSE;
    if (!(t = slurp(path, 1 << 20))) return st_para(y, L"No scan has been run yet.");
    pos = t;
    while (shown < 5 && json_next_object(&pos, &s, &e)) {
        if (!(obj = malloc(e - s + 1))) break;
        memcpy(obj, s, e - s);
        obj[e - s] = 0;
        json_field(obj, "type", type, ARRAYSIZE(type));
        json_field(obj, "state", state, ARRAYSIZE(state));
        json_field(obj, "start", start, ARRAYSIZE(start));
        json_field(obj, "files", files, ARRAYSIZE(files));
        json_field(obj, "found", found, ARRAYSIZE(found));
        free(obj);
        when(start, at, ARRAYSIZE(at));
        if (!lstrcmpW(state, L"running")) {
            if (!shown) g_scan_running = TRUE;
            _snwprintf(line, ARRAYSIZE(line), L"%ls running (started %ls): %ls files scanned so far, %ls threats found.",
                       scan_name(type), at, files[0] ? files : L"0", found[0] ? found : L"0");
        } else
            _snwprintf(line, ARRAYSIZE(line), L"%ls, %ls: %ls files scanned, %ls %ls.", scan_name(type), at,
                       files[0] ? files : L"0", lstrcmpW(found, L"0") && found[0] ? found : L"no",
                       !lstrcmpW(found, L"1") ? L"threat found" : L"threats found");
        y = st_text(y, line);
        shown++;
    }
    free(t);
    return shown ? y : st_para(y, L"No scan has been run yet.");
}

void set_build_defender(void)
{
    int y = st_title(L"Virus & threat protection"), i, n;
    WCHAR on[16], rt[16], back[32], engine[200], found[16], scanned[16], blocked[16], line[600], path[MAX_PATH];
    char *status;
    struct found *items = g_items;
    HWND c;
    BOOL running_on, rt_on;

    _snwprintf(path, MAX_PATH, L"%ls\\status.json", defender_dir());
    status = slurp(path, 4096);
    json_field(status, "enabled", on, ARRAYSIZE(on));
    if (!json_field(status, "realtime", rt, ARRAYSIZE(rt))) lstrcpyW(rt, on);
    json_field(status, "realtime_back", back, ARRAYSIZE(back));
    json_field(status, "engine", engine, ARRAYSIZE(engine));
    json_field(status, "found", found, ARRAYSIZE(found));
    json_field(status, "scanned", scanned, ARRAYSIZE(scanned));
    json_field(status, "blocked", blocked, ARRAYSIZE(blocked));
    running_on = status && !lstrcmpW(on, L"true");
    rt_on = running_on && !lstrcmpW(rt, L"true");
    y = st_para(y, L"Software from the SG Store is the makers' own, checked against its fingerprint before it runs, and "
                   L"Linux apps come from signed repositories. Everything else is scanned by SG Defender (ClamAV): each "
                   L"program, installer and script before it starts, downloads as they arrive, and the places malware "
                   L"hides in a quick scan every day. Anything it finds is stopped, moved to quarantine, and you are told.");

    /* ---- the scans */
    y = st_head(y, L"Current threats");
    if (!status) y = st_para(y, L"SG Defender is not running on this PC.");
    else if (rt_on) {
        _snwprintf(line, ARRAYSIZE(line), L"On. %ls programs checked since it started, %ls found.",
                   scanned[0] ? scanned : L"0", found[0] ? found : L"0");
        y = st_para(y, line);
    } else if (running_on) {
        _snwprintf(line, ARRAYSIZE(line), L"Real-time protection is off%ls%ls: programs are not scanned when they start, "
                   L"nor downloads when they arrive. It comes back on by itself.", back[0] ? L" until " : L"",
                   back[0] ? wcschr(back, L'T') ? wcschr(back, L'T') + 1 : back : L"");
        y = st_para(y, line);
    } else y = st_para(y, L"Off: programs and downloads are not scanned. An administrator can turn it on.");
    y = scans_history(y);
    {
        HWND q = pg_control(L"BUTTON", L"Quick scan", WS_TABSTOP | BS_PUSHBUTTON, st_x(), y, S(120), S(32), CMD_DEF_QUICK);
        HWND f = pg_control(L"BUTTON", L"Full scan", WS_TABSTOP | BS_PUSHBUTTON, st_x() + S(132), y, S(120), S(32), CMD_DEF_FULL);
        HWND u = pg_control(L"BUTTON", L"Custom scan", WS_TABSTOP | BS_PUSHBUTTON, st_x() + S(264), y, S(130), S(32), CMD_DEF_CUSTOM);
        if (g_scan_running || !running_on) { EnableWindow(q, FALSE); EnableWindow(f, FALSE); EnableWindow(u, FALSE); }
        y += S(46);
    }
    y = st_para(y, L"A quick scan looks where malware starts from and lives: startup programs, scheduled tasks, services, "
                   L"the system folders, Program Files, Downloads, the Desktop and temporary files. A full scan looks at "
                   L"everything in your files and on drive C:. Scans run in the background, with your rights.");
    if (g_scan_running) pg_timer(1000);

    /* ---- protection history: what was found in this person's files */
    y = st_head(y, L"Protection history");
    n = g_nitems = found_items(items, ARRAYSIZE(g_items));
    if (!n) y = st_para(y, L"Nothing has been found in your files.");
    else y = st_para(y, L"Found and quarantined: these files were moved where nothing can run them. Delete them, or restore "
                        L"one you know is safe -- it goes back where it was and is not stopped again.");
    for (i = 0; i < n; i++) {
        HDC dc;
        SIZE sz;
        HWND b;
        WCHAR at[32];
        when(items[i].time, at, ARRAYSIZE(at));
        if (!lstrcmpW(items[i].how, L"amsi"))
            _snwprintf(line, ARRAYSIZE(line), L"%ls -- %ls (%ls, %ls)", items[i].name, items[i].signature,
                       how_text(items[i].how), at);
        else
            _snwprintf(line, ARRAYSIZE(line), L"%ls -- %ls (%ls, %ls, in %ls)", items[i].name, items[i].signature,
                       how_text(items[i].how), at, items[i].folder);
        y = st_text(y, line);
        if (lstrcmpW(items[i].action, L"quarantined")) { y += S(8); continue; }    /* nothing kept: nothing to restore */
        dc = GetDC(g_page);
        SelectObject(dc, g_font_body);
        GetTextExtentPoint32W(dc, L"Restore", 7, &sz);
        ReleaseDC(g_page, dc);
        b = pg_control(L"BUTTON", L"Delete", WS_TABSTOP | BS_PUSHBUTTON, st_x(), y, S(120), S(32), CMD_DEF_DELETE + i);
        SendMessageW(b, BCM_SETSHIELD, 0, TRUE);
        b = pg_control(L"BUTTON", L"Restore", WS_TABSTOP | BS_PUSHBUTTON, st_x() + S(132), y,
                       sz.cx + S(40) < S(120) ? S(120) : sz.cx + S(40), S(32), CMD_DEF_RESTORE + i);
        SendMessageW(b, BCM_SETSHIELD, 0, TRUE);
        y += S(46);
    }

    /* ---- the settings (an administrator's) */
    y = st_head(y, L"Virus & threat protection settings");
    c = pg_control(SET_TOGGLE_CLASS, L"Real-time protection", WS_TABSTOP, st_x(), y, S(260), S(26), CMD_DEF_REALTIME);
    SendMessageW(c, BM_SETCHECK, rt_on ? BST_CHECKED : BST_UNCHECKED, 0);
    if (!running_on) EnableWindow(c, FALSE);
    y += S(34);
    y = st_para(y, L"Finds and stops malware from starting on this PC. You can turn it off for a while; it comes back on by "
                   L"itself within four hours, or at the next restart. Scheduled scans still run.");
    c = pg_control(SET_TOGGLE_CLASS, L"SG Defender", WS_TABSTOP, st_x(), y, S(260), S(26), CMD_DEF_TOGGLE);
    SendMessageW(c, BM_SETCHECK, running_on ? BST_CHECKED : BST_UNCHECKED, 0);
    y += S(34);
    y = st_para(y, L"Turned off, nothing is scanned and ClamAV gives back the memory its definitions hold (about 1 GB).");
    if (engine[0]) {
        _snwprintf(line, ARRAYSIZE(line), L"Definitions: %ls", engine);
        y = st_text(y, line);
    }

    /* ---- exclusions */
    y = st_head(y, L"Exclusions");
    y = st_para(y, L"Folders and files SG Defender does not scan. Only administrators can see and change them.");
    c = pg_control(L"BUTTON", L"Add or remove exclusions", WS_TABSTOP | BS_PUSHBUTTON, st_x(), y, S(230), S(32),
                   CMD_DEF_EXCLUSIONS);
    SendMessageW(c, BCM_SETSHIELD, 0, TRUE);
    y += S(46);
    st_link(&y, L"How SG Defender protects this PC", CMD_DEF_LEARN);
    free(status);
}

/* each second while a scan runs: the page again (its count), until it ends */
void set_timer_defender(void)
{
    KillTimer(g_page, 1);
    refresh_page();     /* which starts it again while the scan runs */
}

/* sg-defender-scan KIND [FOLDER]: a native program, run as this person */
static BOOL start_scan(const WCHAR *kind, const WCHAR *folder)
{
    static char *(CDECL *to_unix)(const WCHAR *);
    WCHAR tool[MAX_PATH] = SCAN_TOOL, cmd[2048], *p;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    char *unix_folder = NULL;
    GetEnvironmentVariableW(L"SG_DEFENDER_SCAN_TOOL", tool, MAX_PATH);
    if (folder) {
        if (!to_unix) to_unix = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
        if (!to_unix || !(unix_folder = to_unix(folder))) return FALSE;
        if (strchr(unix_folder, '"')) { HeapFree(GetProcessHeap(), 0, unix_folder); return FALSE; }
    }
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\\\\?\\unix%ls %ls%hs%hs%hs", tool, kind, unix_folder ? " \"" : "",
               unix_folder ? unix_folder : "", unix_folder ? "\"" : "");
    cmd[ARRAYSIZE(cmd) - 1] = 0;
    if (unix_folder) HeapFree(GetProcessHeap(), 0, unix_folder);
    for (p = cmd + 8; *p && *p != L' '; p++) if (*p == L'/') *p = L'\\';
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return FALSE;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, 10000);    /* it says "STARTED" at once; the scan runs in the service */
    CloseHandle(pi.hProcess);
    return TRUE;
}

static BOOL pick_folder(HWND owner, WCHAR *out)
{
    BROWSEINFOW bi = { 0 };
    LPITEMIDLIST pidl;
    BOOL ok;
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Choose the folder to scan.";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (!(pidl = SHBrowseForFolderW(&bi))) return FALSE;
    ok = SHGetPathFromIDListW(pidl, out);
    CoTaskMemFree(pidl);
    return ok && out[0];
}

BOOL set_cmd_defender(int id, int code, HWND ctl)
{
    WCHAR args[96], msg[700], folder[MAX_PATH];
    int i;
    (void)code;
    if (id >= CMD_DEF_RESTORE && id < CMD_DEF_RESTORE + g_nitems) {
        i = id - CMD_DEF_RESTORE;
        _snwprintf(msg, ARRAYSIZE(msg), L"Restore %ls?\n\nSG Defender found %ls in it. Restore it only if you know it "
                   L"is safe: it goes back to %ls, and SG Defender will not stop this file again.",
                   g_items[i].name, g_items[i].signature, g_items[i].folder);
        if (MessageBoxW(GetAncestor(ctl, GA_ROOT), msg, L"Virus & threat protection",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return TRUE;
        _snwprintf(args, ARRAYSIZE(args), L"/admin defender-restore %ls", g_items[i].id);
        if (run_elevated(args)) refresh_when_back();
        return TRUE;
    }
    if (id >= CMD_DEF_DELETE && id < CMD_DEF_DELETE + g_nitems) {
        i = id - CMD_DEF_DELETE;
        _snwprintf(args, ARRAYSIZE(args), L"/admin defender-delete %ls", g_items[i].id);
        if (run_elevated(args)) refresh_when_back();
        return TRUE;
    }
    switch (id) {
    case CMD_DEF_TOGGLE:
        _snwprintf(args, ARRAYSIZE(args), L"/admin defender %ls", st_checked(ctl) ? L"on" : L"off");
        if (run_elevated(args)) refresh_when_back(); else refresh_page();
        return TRUE;
    case CMD_DEF_REALTIME:
        _snwprintf(args, ARRAYSIZE(args), L"/admin defender-realtime %ls", st_checked(ctl) ? L"on" : L"off");
        if (run_elevated(args)) refresh_when_back(); else refresh_page();
        return TRUE;
    case CMD_DEF_EXCLUSIONS:
        if (run_elevated(L"/admin defender-exclusions")) refresh_when_back();
        return TRUE;
    case CMD_DEF_QUICK:
    case CMD_DEF_FULL:
    case CMD_DEF_CUSTOM:
        if (id == CMD_DEF_CUSTOM && !pick_folder(GetAncestor(ctl, GA_ROOT), folder)) return TRUE;
        if (!start_scan(id == CMD_DEF_QUICK ? L"quick" : id == CMD_DEF_FULL ? L"full" : L"custom",
                        id == CMD_DEF_CUSTOM ? folder : NULL))
            st_status(L"The scan could not be started: sg-defender-scan is not installed.");
        refresh_page();
        pg_timer(1000);
        return TRUE;
    case CMD_DEF_LEARN:
        ShellExecuteW(NULL, NULL, DEFENDER_DOCS, NULL, NULL, SW_SHOWNORMAL);
        return TRUE;
    }
    return FALSE;
}
