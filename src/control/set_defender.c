/* sg-control -- Settings > Update & Security > Virus & threat protection.
 *
 * SG Defender (David 2026-10-03): what the SG Store installs is the makers'
 * own software, checked against its fingerprint, and Linux apps come from
 * signed repositories; everything else that comes in -- a download, an
 * attachment, a file from a stick -- is scanned by ClamAV before it can run
 * (sg-session's sg-defender, as root, with clamd's definitions kept loaded).
 * On by default; an administrator may turn it off (sg-admind's defender
 * on|off). This page says how it stands and what it found for this person.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"

#define DEFENDER_DIR L"Z:\\var\\lib\\stained-glass\\defender"   /* SG_DEFENDER_DIR: the gate's */
#define DEFENDER_DOCS L"https://freesoft.page/docs/guide/06-viruses.html"

enum {
    CMD_DEF_TOGGLE = SHIELD_ID(CMD_PAGE_FIRST + 1), CMD_DEF_LEARN = CMD_PAGE_FIRST + 2,
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

struct found { WCHAR id[32], name[MAX_PATH], signature[128], time[32], folder[MAX_PATH]; };
static struct found g_items[10];   /* what the page shows, for its buttons */
static int g_nitems;

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
        _snwprintf(path, MAX_PATH, L"%ls\\notices\\%lu\\%ls", defender_dir(), unix_uid(), fd.cFileName);
        if (!(t = slurp(path, 8192))) continue;
        json_field(t, "id", items[n].id, 32);
        json_field(t, "name", items[n].name, MAX_PATH);
        json_field(t, "signature", items[n].signature, 128);
        json_field(t, "time", items[n].time, 32);
        json_field(t, "folder", items[n].folder, MAX_PATH);
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

void set_build_defender(void)
{
    int y = st_title(L"Virus & threat protection"), i, n;
    WCHAR on[16], engine[200], found[16], scanned[16], line[400], path[MAX_PATH];
    char *status;
    struct found *items = g_items;
    HWND c;

    _snwprintf(path, MAX_PATH, L"%ls\\status.json", defender_dir());
    status = slurp(path, 4096);
    json_field(status, "enabled", on, ARRAYSIZE(on));
    json_field(status, "engine", engine, ARRAYSIZE(engine));
    json_field(status, "found", found, ARRAYSIZE(found));
    json_field(status, "scanned", scanned, ARRAYSIZE(scanned));
    y = st_para(y, L"Software from the SG Store is the makers' own, checked against its fingerprint before it runs, and "
                   L"Linux apps come from signed repositories. Everything else that comes in -- downloads, saved "
                   L"attachments, programs copied to the Desktop -- is scanned by SG Defender (ClamAV) before it can run. "
                   L"Anything it finds is moved to quarantine, and you are told.");
    y = st_head(y, L"Scanning downloaded programs");
    if (!status) y = st_para(y, L"SG Defender is not running on this PC.");
    else if (!lstrcmpW(on, L"true")) {
        _snwprintf(line, ARRAYSIZE(line), L"On. %ls programs checked since it started, %ls found.",
                   scanned[0] ? scanned : L"0", found[0] ? found : L"0");
        y = st_para(y, line);
    } else y = st_para(y, L"Off: downloaded programs are not scanned. An administrator can turn it on.");
    c = pg_control(SET_TOGGLE_CLASS, L"Scan downloaded programs", WS_TABSTOP, st_x(), y, S(260), S(26), CMD_DEF_TOGGLE);
    SendMessageW(c, BM_SETCHECK, status && !lstrcmpW(on, L"true") ? BST_CHECKED : BST_UNCHECKED, 0);
    y += S(40);
    if (engine[0]) {
        _snwprintf(line, ARRAYSIZE(line), L"Definitions: %ls", engine);
        y = st_text(y, line);
    }
    y = st_head(y, L"Found and quarantined");
    n = g_nitems = found_items(items, ARRAYSIZE(g_items));
    if (!n) y = st_para(y, L"Nothing has been found in your files.");
    else y = st_para(y, L"These files were moved where nothing can run them. Delete them, or restore one you know is "
                        L"safe -- it goes back where it was and is not stopped again.");
    for (i = 0; i < n; i++) {
        HDC dc;
        SIZE sz;
        HWND b;
        _snwprintf(line, ARRAYSIZE(line), L"%ls -- %ls (%ls, in %ls)", items[i].name, items[i].signature,
                   items[i].time, items[i].folder);
        y = st_text(y, line);
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
    st_link(&y, L"How SG Defender protects this PC", CMD_DEF_LEARN);
    free(status);
}

BOOL set_cmd_defender(int id, int code, HWND ctl)
{
    WCHAR args[96], msg[700];
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
    case CMD_DEF_LEARN:
        ShellExecuteW(NULL, NULL, DEFENDER_DOCS, NULL, NULL, SW_SHOWNORMAL);
        return TRUE;
    }
    return FALSE;
}
