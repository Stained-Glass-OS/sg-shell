/* sg-netclient.h -- how sg-shell's network programs talk to sg-netctl.
 *
 * Network settings live in NetworkManager, behind sg-netd (sg-session's
 * sg-netctl). A Windows program cannot reach a Unix socket -- Wine has no
 * AF_UNIX -- and Wine gives a native program started from a GUI process no
 * stdin or stdout. So each network program re-launches itself through
 * `sg-netctl --bridge wine <itself> --bridged ...`: the bridge starts it with
 * pipes as its standard handles and answers each request written to them,
 * asking sg-netd, which decides from the Unix uid what the user may do. The
 * program decides nothing.
 *
 * Wire format: one JSON line {"argv": [...], "secret": "..."} out; lines back,
 * the last "OK" or "ERROR <kind> <message>". See sg-session's CLAUDE.md.
 *
 * Included by sg-ncpa.c and sg-netflyout.c (sg-shell builds one file per
 * program).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_NETCLIENT_H
#define SG_NETCLIENT_H

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <wctype.h>
#include <stdlib.h>
#include <string.h>

#define NET_MAX_LINES 1024

struct net_reply {
    int ok;                 /* 1: OK; 0: ERROR */
    char kind[32];          /* denied, invalid, notfound, auth, unsupported, failed */
    char message[512];      /* the error text, UTF-8 */
    int nlines;
    char *lines[NET_MAX_LINES];  /* everything before the last line, UTF-8 */
};

static HANDLE g_net_in, g_net_out;
static CRITICAL_SECTION g_net_lock;
static BOOL g_net_bridged;

static void net_reply_free(struct net_reply *r)
{
    int i;
    for (i = 0; i < r->nlines; i++) free(r->lines[i]);
    r->nlines = 0;
}

/* The value after "KEY " in the reply lines, from line `from` until the next
 * "END" (an adapter's block); the n-th match. NULL if none. */
static const char *net_field(const struct net_reply *r, int from, const char *key, int n)
{
    size_t kl = strlen(key);
    int i;
    for (i = from; i < r->nlines; i++)
    {
        if (i > from && !strcmp(r->lines[i], "END")) break;
        if (!strncmp(r->lines[i], key, kl) && r->lines[i][kl] == ' ' && n-- == 0)
            return r->lines[i] + kl + 1;
    }
    return NULL;
}

static void utf8_to_w(const char *s, WCHAR *out, int cch)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, out, cch)) out[0] = 0;
    out[cch - 1] = 0;
}

static char *w_to_utf8(const WCHAR *s)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    char *out = malloc(n > 0 ? n : 1);
    if (!out) return NULL;
    if (n <= 0 || !WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL)) out[0] = 0;
    return out;
}

/* Append a JSON string (UTF-8 in) to buf. */
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

static BOOL net_read_line(char *buf, size_t cap)
{
    size_t i = 0;
    DWORD got;
    for (;;)
    {
        char c;
        if (!ReadFile(g_net_in, &c, 1, &got, NULL) || !got) return FALSE;
        if (c == '\n') break;
        if (c != '\r' && i + 1 < cap) buf[i++] = c;
    }
    buf[i] = 0;
    return TRUE;
}

/* One request, blocking. argv is UTF-8. secret may be NULL. */
static void net_request_argv(int argc, const char *const *argv, const char *secret, struct net_reply *r)
{
    static char req[16384];
    char line[4096];
    DWORD written;
    int i;

    memset(r, 0, sizeof(*r));
    if (!g_net_bridged)
    {
        strcpy(r->kind, "failed");
        strcpy(r->message, "the network settings service is not connected");
        return;
    }
    strcpy(req, "{\"argv\": [");
    for (i = 0; i < argc; i++)
    {
        if (i) strcat(req, ", ");
        json_str(req, sizeof(req), argv[i]);
    }
    strcat(req, "]");
    if (secret)
    {
        strcat(req, ", \"secret\": ");
        json_str(req, sizeof(req), secret);
    }
    strcat(req, "}\n");

    EnterCriticalSection(&g_net_lock);
    if (!WriteFile(g_net_out, req, (DWORD)strlen(req), &written, NULL))
    {
        LeaveCriticalSection(&g_net_lock);
        strcpy(r->kind, "failed");
        strcpy(r->message, "the network settings service went away");
        return;
    }
    SecureZeroMemory(req, sizeof(req));
    for (;;)
    {
        if (!net_read_line(line, sizeof(line)))
        {
            strcpy(r->kind, "failed");
            strcpy(r->message, "the network settings service went away");
            break;
        }
        if (!strcmp(line, "OK")) { r->ok = 1; break; }
        if (!strncmp(line, "ERROR ", 6))
        {
            const char *sp = strchr(line + 6, ' ');
            size_t kl = sp ? (size_t)(sp - (line + 6)) : strlen(line + 6);
            if (kl >= sizeof(r->kind)) kl = sizeof(r->kind) - 1;
            memcpy(r->kind, line + 6, kl);
            r->kind[kl] = 0;
            snprintf(r->message, sizeof(r->message), "%s", sp ? sp + 1 : "");
            break;
        }
        if (r->nlines < NET_MAX_LINES) r->lines[r->nlines++] = _strdup(line);
    }
    LeaveCriticalSection(&g_net_lock);
}

/* A request from a space-free list of words: net_request(r, NULL, "adapters", NULL). */
static void __attribute__((unused)) net_request(struct net_reply *r, const char *secret, ...)
{
    const char *argv[32];
    int argc = 0;
    va_list ap;
    va_start(ap, secret);
    while (argc < 32 && (argv[argc] = va_arg(ap, const char *))) argc++;
    va_end(ap);
    net_request_argv(argc, argv, secret, r);
}

/* A request on a worker thread while this thread keeps pumping messages, so a
 * window stays responsive while NetworkManager takes its time (joining a
 * network can take tens of seconds). */
struct net_job { int argc; const char *argv[32]; const char *secret; struct net_reply *r; };

static DWORD WINAPI net_job_thread(void *arg)
{
    struct net_job *j = arg;
    net_request_argv(j->argc, j->argv, j->secret, j->r);
    return 0;
}

static void net_request_pumped(int argc, const char *const *argv, const char *secret, struct net_reply *r)
{
    struct net_job j;
    HANDLE th;
    int i;
    j.argc = argc > 32 ? 32 : argc;
    for (i = 0; i < j.argc; i++) j.argv[i] = argv[i];
    j.secret = secret;
    j.r = r;
    th = CreateThread(NULL, 0, net_job_thread, &j, 0, NULL);
    if (!th) { net_request_argv(argc, argv, secret, r); return; }
    for (;;)
    {
        MSG msg;
        DWORD w = MsgWaitForMultipleObjects(1, &th, FALSE, INFINITE, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) break;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CloseHandle(th);
}

/* Where sg-netctl is, as a \\?\unix\ path Wine can start. */
static void net_ctl_path(WCHAR *out, size_t cch)
{
    WCHAR unix_path[MAX_PATH] = L"/usr/bin/sg-netctl";
    WCHAR *p;
    GetEnvironmentVariableW(L"SG_NETCTL", unix_path, MAX_PATH);
    _snwprintf(out, cch, L"\\\\?\\unix%ls", unix_path);
    out[cch - 1] = 0;
    for (p = out + 8; *p; p++) if (*p == '/') *p = '\\';
}

/* Is --bridged on the command line? Then the pipes are our standard handles. */
static BOOL net_init(const WCHAR *cmdline)
{
    InitializeCriticalSection(&g_net_lock);
    if (!wcsstr(cmdline, L"--bridged")) return FALSE;
    g_net_in = GetStdHandle(STD_INPUT_HANDLE);
    g_net_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_net_bridged = g_net_in && g_net_in != INVALID_HANDLE_VALUE && g_net_out && g_net_out != INVALID_HANDLE_VALUE;
    return TRUE;
}

/* Not bridged yet: start ourselves again through the bridge, with the same
 * arguments, and let this copy exit. Returns FALSE if that failed. */
static BOOL net_relaunch(const WCHAR *args)
{
    static WCHAR cmd[8192];
    WCHAR self[MAX_PATH], ctl[MAX_PATH + 16];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;

    GetModuleFileNameW(NULL, self, MAX_PATH);
    net_ctl_path(ctl, MAX_PATH + 16);
    _snwprintf(cmd, 8192, L"%ls --bridge wine \"%ls\" --bridged %ls", ctl, self, args ? args : L"");
    cmd[8191] = 0;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return FALSE;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

/* The arguments after the program name, as the user gave them. */
static const WCHAR *net_args_after_program(const WCHAR *cmdline)
{
    const WCHAR *p = cmdline;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Report text for the gates: to stderr, which the bridge passes through. */
static void net_report(const char *fmt, ...)
{
    char buf[4096];
    DWORD written;
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = sizeof(buf) - 1;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, n, &written, NULL);
}

/* An error from sg-netd, in the words Windows would use. */
static void net_error_text(const struct net_reply *r, WCHAR *out, int cch)
{
    WCHAR msg[512];
    utf8_to_w(r->message, msg, 512);
    if (!strcmp(r->kind, "denied"))
        _snwprintf(out, cch, L"You need to be an administrator to change these settings.\n\n"
                              L"Ask an administrator to make the change, or sign in with an administrator account.");
    else if (!strcmp(r->kind, "auth"))
        _snwprintf(out, cch, L"The network security key isn't correct. Please try again.");
    else if (msg[0])
        _snwprintf(out, cch, L"%lc%ls.", towupper(msg[0]), msg + 1);
    else
        _snwprintf(out, cch, L"The change could not be made.");
    out[cch - 1] = 0;
}

#endif
