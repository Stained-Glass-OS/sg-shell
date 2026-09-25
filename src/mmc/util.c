/* sg-mmc -- helpers: the sg-sysinfo bridge client, in-memory dialogs,
 * formatting, elevation.
 *
 * The Linux side (devices, disks, the journal, accounts, shares) is
 * sg-session's sg-sysinfo. A Windows program cannot reach a Unix socket (Wine
 * has no AF_UNIX) and Wine gives a native program started from a GUI process
 * no stdio, so the console re-launches itself as `sg-sysinfo --bridge wine
 * <itself> --bridged ...`: the bridge starts it with pipes as its standard
 * handles and answers each JSON-line request (as sg-netctl does for the
 * network programs; see src/sg-netclient.h).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "mmc.h"
#include <stdarg.h>
#include <string.h>

/* ---- the bridge ------------------------------------------------------------- */

static HANDLE g_in, g_out;
static CRITICAL_SECTION g_lock;
static BOOL g_bridged;

BOOL sys_bridged(void) { return g_bridged; }

void utf8_to_w(const char *s, WCHAR *out, int cch)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, out, cch)) out[0] = 0;
    out[cch - 1] = 0;
}

char *w_to_utf8(const WCHAR *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *out = malloc(n > 0 ? n : 1);
    if (!out) return NULL;
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL)) out[0] = 0;
    return out;
}

static void json_str(char *buf, size_t cap, const char *s)
{
    size_t n = strlen(buf);
    if (n + 2 >= cap) return;
    buf[n++] = '"';
    for (; *s && n + 8 < cap; s++)
    {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { buf[n++] = '\\'; buf[n++] = c; }
        else if (c < 0x20) n += snprintf(buf + n, cap - n, "\\u%04x", c);
        else buf[n++] = c;
    }
    buf[n++] = '"';
    buf[n] = 0;
}

/* one line from the bridge, grown as needed; NULL at the end */
static char *read_line(void)
{
    static char chunk[65536];
    static DWORD have, pos;
    size_t cap = 256, len = 0;
    char *line = malloc(cap);

    if (!line) return NULL;
    for (;;)
    {
        char c;
        if (pos == have)
        {
            pos = have = 0;
            if (!ReadFile(g_in, chunk, sizeof(chunk), &have, NULL) || !have) { free(line); return NULL; }
        }
        c = chunk[pos++];
        if (c == '\n') break;
        if (c == '\r') continue;
        if (len + 2 > cap)
        {
            char *p = realloc(line, cap * 2);
            if (!p) { free(line); return NULL; }
            line = p;
            cap *= 2;
        }
        line[len++] = c;
    }
    line[len] = 0;
    return line;
}

void sys_free(sys_reply_t *r)
{
    int i;
    for (i = 0; i < r->nlines; i++) free(r->lines[i]);
    free(r->lines);
    r->lines = NULL;
    r->nlines = 0;
}

void sys_request_argv(sys_reply_t *r, int argc, const char *const *argv)
{
    static char req[16384];
    DWORD written;
    int i, cap = 0;

    memset(r, 0, sizeof(*r));
    if (!g_bridged)
    {
        strcpy(r->kind, "failed");
        strcpy(r->message, "the Stained Glass system information service is not connected");
        return;
    }
    strcpy(req, "{\"argv\": [");
    for (i = 0; i < argc; i++)
    {
        if (i) strcat(req, ", ");
        json_str(req, sizeof(req), argv[i]);
    }
    strcat(req, "]}\n");

    EnterCriticalSection(&g_lock);
    if (!WriteFile(g_out, req, (DWORD)strlen(req), &written, NULL))
    {
        LeaveCriticalSection(&g_lock);
        strcpy(r->kind, "failed");
        strcpy(r->message, "the system information service went away");
        return;
    }
    for (;;)
    {
        char *line = read_line();
        if (!line)
        {
            strcpy(r->kind, "failed");
            strcpy(r->message, "the system information service went away");
            break;
        }
        if (!strcmp(line, "OK")) { r->ok = 1; free(line); break; }
        if (!strncmp(line, "ERROR ", 6))
        {
            const char *sp = strchr(line + 6, ' ');
            size_t kl = sp ? (size_t)(sp - (line + 6)) : strlen(line + 6);
            if (kl >= sizeof(r->kind)) kl = sizeof(r->kind) - 1;
            memcpy(r->kind, line + 6, kl);
            r->kind[kl] = 0;
            snprintf(r->message, sizeof(r->message), "%s", sp ? sp + 1 : "");
            free(line);
            break;
        }
        if (r->nlines >= SYS_MAX_LINES) { free(line); continue; }
        if (r->nlines == cap)
        {
            char **p = realloc(r->lines, (cap ? cap * 2 : 256) * sizeof(char *));
            if (!p) { free(line); continue; }
            r->lines = p;
            cap = cap ? cap * 2 : 256;
        }
        r->lines[r->nlines++] = line;
    }
    LeaveCriticalSection(&g_lock);
}

void sys_request(sys_reply_t *r, ...)
{
    const char *argv[32];
    int argc = 0;
    va_list ap;
    va_start(ap, r);
    while (argc < 32 && (argv[argc] = va_arg(ap, const char *))) argc++;
    va_end(ap);
    sys_request_argv(r, argc, argv);
}

const char *sys_field(const sys_reply_t *r, int from, const char *key)
{
    size_t kl = strlen(key);
    int i;
    for (i = from; i >= 0 && i < r->nlines; i++)
    {
        if (i > from && !strcmp(r->lines[i], "END")) break;
        if (!strncmp(r->lines[i], key, kl) && (r->lines[i][kl] == ' ' || !r->lines[i][kl]))
            return r->lines[i][kl] ? r->lines[i] + kl + 1 : "";
    }
    return NULL;
}

int sys_next_block(const sys_reply_t *r, int from, const char *head)
{
    size_t hl = strlen(head);
    int i;
    for (i = from < 0 ? 0 : from; i < r->nlines; i++)
        if (!strncmp(r->lines[i], head, hl) && r->lines[i][hl] == ' ') return i;
    return -1;
}

static void sysinfo_path(WCHAR *out, size_t cch)
{
    WCHAR unix_path[MAX_PATH] = L"/usr/bin/sg-sysinfo";
    WCHAR *p;
    GetEnvironmentVariableW(L"SG_SYSINFO", unix_path, MAX_PATH);
    _snwprintf(out, cch, L"\\\\?\\unix%ls", unix_path);
    out[cch - 1] = 0;
    for (p = out + 8; *p; p++) if (*p == '/') *p = '\\';
}

BOOL sys_init(const WCHAR *cmdline)
{
    InitializeCriticalSection(&g_lock);
    if (!wcsstr(cmdline, L"--bridged")) return FALSE;
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_bridged = g_in && g_in != INVALID_HANDLE_VALUE && g_out && g_out != INVALID_HANDLE_VALUE;
    return TRUE;
}

BOOL sys_relaunch(const WCHAR *args)
{
    static WCHAR cmd[8192];
    WCHAR self[MAX_PATH], ctl[MAX_PATH + 16];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    sysinfo_path(ctl, MAX_PATH + 16);
    if (GetFileAttributesW(ctl) == INVALID_FILE_ATTRIBUTES) return FALSE;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    _snwprintf(cmd, 8192, L"%ls --bridge wine \"%ls\" --bridged %ls", ctl, self, args ? args : L"");
    cmd[8191] = 0;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return FALSE;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

/* ---- in-memory dialogs -------------------------------------------------------- */

static void dlg_str(dlgt_t *d, const WCHAR *s)
{
    while (*s) *d->p++ = *s++;
    *d->p++ = 0;
}

void dlg_begin(dlgt_t *d, const WCHAR *title, DWORD style, short cx, short cy)
{
    memset(d, 0, sizeof(*d));
    d->t = (DLGTEMPLATE *)d->buf;
    d->t->style = style ? style : (DS_SHELLFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    d->t->style |= DS_SETFONT;
    d->t->cx = cx;
    d->t->cy = cy;
    d->p = (WORD *)(d->t + 1);
    *d->p++ = 0;
    *d->p++ = 0;
    dlg_str(d, title);
    *d->p++ = 9;
    dlg_str(d, L"Segoe UI");
}

void dlg_item(dlgt_t *d, const WCHAR *cls, WORD atom, const WCHAR *text, WORD id, DWORD style,
              short x, short y, short cx, short cy)
{
    DLGITEMTEMPLATE *it;
    d->p = (WORD *)(((ULONG_PTR)d->p + 3) & ~(ULONG_PTR)3);
    it = (DLGITEMTEMPLATE *)d->p;
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy;
    it->id = id;
    d->p = (WORD *)(it + 1);
    if (cls) dlg_str(d, cls);
    else { *d->p++ = 0xFFFF; *d->p++ = atom; }
    dlg_str(d, text ? text : L"");
    *d->p++ = 0;
    d->t->cdit++;
}

/* ---- formatting ------------------------------------------------------------------ */

void error_text(DWORD err, WCHAR *out, int cch)
{
    WCHAR *p;
    if (!FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, out, cch, NULL))
    {
        if (err == ERROR_ACCESS_DENIED) lstrcpynW(out, L"Access is denied.", cch);
        else _snwprintf(out, cch, L"The operation failed.");
    }
    out[cch - 1] = 0;
    for (p = out + wcslen(out); p > out && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' '); p--) p[-1] = 0;
}

void fmt_bytes(ULONGLONG b, WCHAR *out, int cch)
{
    static const WCHAR *const units[] = { L"bytes", L"KB", L"MB", L"GB", L"TB", L"PB" };
    double v = (double)b;
    int u = 0;
    while (v >= 1024.0 && u < 5) { v /= 1024.0; u++; }
    if (!u) _snwprintf(out, cch, L"%llu bytes", b);
    else if (v >= 100) _snwprintf(out, cch, L"%.0f %ls", v, units[u]);
    else if (v >= 10) _snwprintf(out, cch, L"%.1f %ls", v, units[u]);
    else _snwprintf(out, cch, L"%.2f %ls", v, units[u]);
    out[cch - 1] = 0;
}

void fmt_time(ULONGLONG unix_seconds, WCHAR *out, int cch)
{
    ULARGE_INTEGER t;
    FILETIME ft, lft;
    SYSTEMTIME st;
    WCHAR d[64], h[64];
    t.QuadPart = unix_seconds * 10000000ULL + 116444736000000000ULL;
    ft.dwLowDateTime = t.LowPart;
    ft.dwHighDateTime = t.HighPart;
    FileTimeToLocalFileTime(&ft, &lft);
    FileTimeToSystemTime(&lft, &st);
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, d, 64);
    GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &st, NULL, h, 64);
    _snwprintf(out, cch, L"%ls %ls", d, h);
    out[cch - 1] = 0;
}

/* ---- who we are ---------------------------------------------------------------------- */

BOOL is_admin(void)
{
    SID_IDENTIFIER_AUTHORITY nt = { SECURITY_NT_AUTHORITY };
    PSID admins;
    BOOL member = FALSE;
    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins))
        return FALSE;
    if (!CheckTokenMembership(NULL, admins, &member)) member = FALSE;
    FreeSid(admins);
    return member;
}

/* Run this console again as an administrator (ShellExecute "runas": the
 * elevation broker's consent prompt), then leave. */
BOOL relaunch_elevated(void)
{
    WCHAR self[MAX_PATH];
    const WCHAR *args = GetCommandLineW();
    SHELLEXECUTEINFOW sei = { sizeof(sei) };

    GetModuleFileNameW(NULL, self, MAX_PATH);
    if (*args == '"') { args++; while (*args && *args != '"') args++; if (*args) args++; }
    else while (*args && *args != ' ' && *args != '\t') args++;
    while (*args == ' ' || *args == '\t') args++;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    sei.hwnd = g_main;
    return ShellExecuteExW(&sei);
}

void screen_center(HWND h, const RECT *rc, POINT *out)
{
    out->x = (rc->left + rc->right) / 2;
    out->y = (rc->top + rc->bottom) / 2;
    ClientToScreen(h, out);
}
