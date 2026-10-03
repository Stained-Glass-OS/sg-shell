/* SG Store -- system packages: Linux apps from the catalogue and .deb files.
 *
 * A Linux app (Source=linux:apt:<package>) and a .deb file someone downloaded
 * (steam.deb) are installed by apt, as root. The store asks for that the way
 * the Control Panel asks for any administrator's change: ShellExecute "runas"
 * on itself, which wine-sg sends to the elevation broker and its consent
 * prompt (ADR 0012), and the elevated copy (SYSTEM) files the request with
 * sg-admind, which alone does root's part (admin/sg-admind: apt-install,
 * deb-install). No new privilege path.
 *
 *   sg-store64.exe --deb FILE                  what the file is, and Install
 *                                              (File Explorer's .deb verb)
 *   sg-store64.exe --elevated-apt PKG NAME     (runas) apt-get install PKG
 *   sg-store64.exe --elevated-apt-remove PKG NAME
 *                                              (runas) Uninstall: apt-get remove
 *                                              PKG (sg-admind apt-remove)
 *   sg-store64.exe --elevated-deb FILE PKG VER (runas) install FILE, which
 *                                              must be PKG at version VER --
 *                                              what the person was shown
 *
 * The elevated copy shows the progress sg-admind publishes and the result;
 * its exit code tells the copy that asked (SYS_*).
 *
 * The .deb is first copied into a folder only SYSTEM may write
 * (/var/cache/stained-glass-admin/debs), so what is installed can no longer
 * be changed by whoever owns the download; sg-admind copies it again into a
 * folder of root's and checks it is still the package and version shown.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "store.h"
#include <shellapi.h>

typedef char *(CDECL *unixname_t)(const WCHAR *);
typedef WCHAR *(CDECL *dosname_t)(const char *);

static HINSTANCE g_inst;
static HFONT f_body, f_head, f_small;
static int s_dpi = 96;
#define S(x) MulDiv((x), s_dpi, 96)

#define C_BG      RGB(0xf3, 0xf3, 0xf3)
#define C_TEXT    RGB(0x20, 0x20, 0x20)
#define C_SUB     RGB(0x60, 0x60, 0x60)
#define C_ACCENT  RGB(0x10, 0x7c, 0x41)
#define C_ERR     RGB(0xc4, 0x2b, 0x1c)
#define C_LINE    RGB(0xd0, 0xd0, 0xd0)

/* ---- paths ------------------------------------------------------------------------------------ */

BOOL sys_unix_path(const WCHAR *dos, char *out, int cch)
{
    unixname_t to_unix = (unixname_t)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    char *u;
    out[0] = 0;
    if (!to_unix || !(u = to_unix(dos))) return FALSE;
    lstrcpynA(out, u, cch);
    HeapFree(GetProcessHeap(), 0, u);
    return TRUE;
}

static BOOL dos_path(const char *unix_path, WCHAR *out, int cch)
{
    dosname_t to_dos = (dosname_t)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name");
    WCHAR *d;
    out[0] = 0;
    if (!to_dos || !(d = to_dos(unix_path))) return FALSE;
    lstrcpynW(out, d, cch);
    HeapFree(GetProcessHeap(), 0, d);
    return TRUE;
}

/* a Unix directory from the environment (the gate's own), or the system's */
static BOOL env_dir(const WCHAR *var, const char *def, const char *sub, WCHAR *out, int cch)
{
    WCHAR env[400];
    char base[512], full[600];
    lstrcpynA(base, def, sizeof(base));
    if (GetEnvironmentVariableW(var, env, ARRAYSIZE(env)))
        WideCharToMultiByte(CP_UTF8, 0, env, -1, base, sizeof(base), NULL, NULL);
    _snprintf(full, sizeof(full), "%s%s%s", base, sub[0] ? "/" : "", sub);
    full[sizeof(full) - 1] = 0;
    return dos_path(full, out, cch);
}

/* A Unix program, started with a command line; for sg-debinfo and Open. */
static BOOL run_unix(const WCHAR *prog, const WCHAR *args, HANDLE *process)
{
    WCHAR cmd[4096], *p;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\\\\?\\unix%ls%ls%ls", prog, args ? L" " : L"", args ? args : L"");
    cmd[ARRAYSIZE(cmd) - 1] = 0;
    for (p = cmd + 8; *p && *p != L' '; p++) if (*p == L'/') *p = L'\\';
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return FALSE;
    CloseHandle(pi.hThread);
    if (process) *process = pi.hProcess; else CloseHandle(pi.hProcess);
    return TRUE;
}

/* A Linux app's Run: its program, or its desktop entry (a .desktop file),
 * which gio launch starts as a Linux desktop would -- Exec, field codes and
 * all, whichever program the package names (a metapackage's, a renamed one's). */
void sys_run_linux(const WCHAR *unix_path)
{
    size_t n;
    if (!unix_path || unix_path[0] != L'/') return;
    n = wcslen(unix_path);
#ifndef SG_MUTANT_NODESKTOP
    if (n > 8 && !lstrcmpiW(unix_path + n - 8, L".desktop")) {
        WCHAR args[MAX_PATH + 16];
        _snwprintf(args, ARRAYSIZE(args), L"launch %ls", unix_path);
        args[ARRAYSIZE(args) - 1] = 0;
        run_unix(L"/usr/bin/gio", args, NULL);
        return;
    }
#endif
    run_unix(unix_path, NULL, NULL);
}

/* ---- elevation ---------------------------------------------------------------------------------- */

/* Run this program again, elevated, and wait for it: the consent prompt,
 * then the elevated copy's own window. SG_STORE_DIRECT=1 (the gate, which
 * has no broker) runs the copy directly; without SYSTEM's rights it cannot
 * file a request with sg-admind anyway. Returns its exit code (SYS_*). */
static int elevate_wait(const WCHAR *args)
{
    WCHAR self[MAX_PATH], direct[8] = L"";
    HANDLE process = NULL;
    DWORD code = SYS_FAILED;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    if (g_runas_hook) {   /* several apps: the one administrator's helper runs it */
        if (!g_runas_hook(self, args, &code)) return SYS_DENIED;
        return (int)code;
    }
    GetEnvironmentVariableW(L"SG_STORE_DIRECT", direct, ARRAYSIZE(direct));
    if (direct[0] == L'1') {
        WCHAR cmd[4096];
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" %ls", self, args);
        cmd[ARRAYSIZE(cmd) - 1] = 0;
        if (!CreateProcessW(self, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return SYS_FAILED;
        CloseHandle(pi.hThread);
        process = pi.hProcess;
    } else {
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = L"runas";
        sei.lpFile = self;
        sei.lpParameters = args;
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) return GetLastError() == ERROR_CANCELLED ? SYS_DENIED : SYS_FAILED;
        process = sei.hProcess;
    }
    if (!process) return SYS_OK;        /* started, but nothing to wait on */
    WaitForSingleObject(process, INFINITE);
    GetExitCodeProcess(process, &code);
    CloseHandle(process);
    return (int)code;
}

/* ---- several apps under one consent ----------------------------------------------------------------
 *
 * Installing the apps checked in the list (David 2026-10-03: "ask for the
 * admin credentials just once for all the selected apps"): the first that
 * needs an administrator starts this program elevated once more, as a
 * helper -- one consent -- and every administrator's step after it (apt
 * through --elevated-apt, an installer that must run elevated) is run by
 * that helper instead of a consent each. What needs no administrator still
 * runs as the person (a per-user installer stays the person's).
 *
 *   sg-store64.exe --elevated-helper FILE   (runas) FILE: its pipe's name
 *                                           and the store's process id
 *
 * The helper answers only the store that started it: its pipe's name is
 * random (and only in a file of the person's), the client must be that
 * process (GetNamedPipeClientProcessId), and it ends when the store does. */

static WCHAR g_helper_pipe[96];
static BOOL g_helper_up, g_helper_denied;

static BOOL random_id(WCHAR *out);
static char *read_small(const WCHAR *path, DWORD max);

static BOOL helper_start(void)
{
    WCHAR id[40], dir[MAX_PATH], file[MAX_PATH], self[MAX_PATH], args[MAX_PATH + 32], direct[8] = L"";
    char body[160];
    HANDLE h;
    DWORD put, start;
    if (g_helper_up) return TRUE;
    if (g_helper_denied || !random_id(id)) return FALSE;
    _snwprintf(g_helper_pipe, ARRAYSIZE(g_helper_pipe), L"\\\\.\\pipe\\sg-store-helper-%ls", id);
    GetTempPathW(MAX_PATH, dir);
    _snwprintf(file, MAX_PATH, L"%lssg-store-helper-%ls.txt", dir, id);
    file[MAX_PATH - 1] = 0;
    h = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    put = (DWORD)_snprintf(body, sizeof(body), "%ls\n%lu\n", g_helper_pipe, GetCurrentProcessId());
    WriteFile(h, body, put, &put, NULL);
    CloseHandle(h);
    GetModuleFileNameW(NULL, self, MAX_PATH);
    _snwprintf(args, ARRAYSIZE(args), L"--elevated-helper \"%ls\"", file);
    args[ARRAYSIZE(args) - 1] = 0;
    GetEnvironmentVariableW(L"SG_STORE_DIRECT", direct, ARRAYSIZE(direct));
    if (direct[0] == L'1') {   /* the gate: no broker */
        WCHAR cmd[MAX_PATH * 2 + 40];
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" %ls", self, args);
        cmd[ARRAYSIZE(cmd) - 1] = 0;
        if (!CreateProcessW(self, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { DeleteFileW(file); return FALSE; }
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    } else {
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"runas";
        sei.lpFile = self;
        sei.lpParameters = args;
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei)) {
            if (GetLastError() == ERROR_CANCELLED) g_helper_denied = TRUE;   /* not asked again in this batch */
            DeleteFileW(file);
            return FALSE;
        }
    }
    /* its pipe comes up once it is running (after the consent) */
    for (start = GetTickCount(); GetTickCount() - start < 180000; Sleep(100))
        if (WaitNamedPipeW(g_helper_pipe, 100)) { g_helper_up = TRUE; break; }
        else if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES && GetTickCount() - start < 170000)
            start = GetTickCount() - 170000;   /* read by the helper: its pipe is moments away */
    DeleteFileW(file);
    return g_helper_up;
}

/* one request to the helper: RUN FILE ARGS (its exit code back), or QUIT */
static BOOL helper_call(const WCHAR *verb, const WCHAR *file, const WCHAR *args, DWORD *code)
{
    WCHAR req[4096], reply[32] = L"";
    DWORD got = 0;
    HANDLE p;
    int len;
    len = _snwprintf(req, ARRAYSIZE(req), L"%ls\n%ls\n%ls", verb, file ? file : L"", args ? args : L"");
    if (len < 0) return FALSE;
    p = CreateFileW(g_helper_pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (p == INVALID_HANDLE_VALUE) {
        if (!WaitNamedPipeW(g_helper_pipe, 5000)) return FALSE;
        p = CreateFileW(g_helper_pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (p == INVALID_HANDLE_VALUE) return FALSE;
    }
    if (!WriteFile(p, req, (DWORD)(len + 1) * sizeof(WCHAR), &got, NULL) ||
        !ReadFile(p, reply, sizeof(reply) - sizeof(WCHAR), &got, NULL)) { CloseHandle(p); return FALSE; }
    CloseHandle(p);
    reply[got / sizeof(WCHAR)] = 0;
    if (code) *code = (DWORD)_wtoi(reply);
    return reply[0] != L'!';
}

/* fetch.c's hook: an installer that must run elevated, by the helper */
static BOOL helper_runas(const WCHAR *file, const WCHAR *args, DWORD *code)
{
    return helper_start() && helper_call(L"RUN", file, args, code);
}

void sys_batch_begin(void)
{
#ifndef SG_MUTANT_BATCH_PER_APP
    g_helper_denied = FALSE;
    g_runas_hook = helper_runas;
#endif
}

void sys_batch_end(void)
{
    g_runas_hook = NULL;
    if (g_helper_up) helper_call(L"QUIT", NULL, NULL, NULL);
    g_helper_up = FALSE;
}

/* the helper itself (elevated): runs what its store asks, until the store
 * says QUIT or ends */
static DWORD WINAPI helper_watch(void *arg)
{
    WaitForSingleObject((HANDLE)arg, INFINITE);
    ExitProcess(0);
}

static DWORD g_helper_store_pid;

/* one client of the helper: only the store that started it */
static DWORD WINAPI helper_client(void *arg)
{
    HANDLE p = arg;
    WCHAR req[4096], reply[32], *f, *a;
    DWORD got = 0, code = 1, pid = 0;
#ifndef SG_MUTANT_HELPER_ANYONE
    if (!GetNamedPipeClientProcessId(p, &pid) || pid != g_helper_store_pid) {
        WriteFile(p, L"!refused", 9 * sizeof(WCHAR), &got, NULL);
        FlushFileBuffers(p); DisconnectNamedPipe(p); CloseHandle(p);
        return 0;
    }
#else
    (void)pid;
#endif
    if (!ReadFile(p, req, sizeof(req) - sizeof(WCHAR), &got, NULL)) { DisconnectNamedPipe(p); CloseHandle(p); return 0; }
    req[got / sizeof(WCHAR)] = 0;
    if (!wcsncmp(req, L"QUIT\n", 5)) {
        WriteFile(p, L"0", 2 * sizeof(WCHAR), &got, NULL);
        FlushFileBuffers(p); DisconnectNamedPipe(p); CloseHandle(p);
        ExitProcess(SYS_OK);
    }
    if (!wcsncmp(req, L"RUN\n", 4) && (f = req + 4) && (a = wcschr(f, L'\n'))) {
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        *a++ = 0;
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpFile = f;
        sei.lpParameters = a[0] ? a : NULL;
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            WaitForSingleObject(sei.hProcess, INFINITE);
            GetExitCodeProcess(sei.hProcess, &code);
            CloseHandle(sei.hProcess);
            _snwprintf(reply, ARRAYSIZE(reply), L"%lu", code);
        } else _snwprintf(reply, ARRAYSIZE(reply), L"!%lu", GetLastError());
    } else lstrcpyW(reply, L"!bad");
    WriteFile(p, reply, (DWORD)(lstrlenW(reply) + 1) * sizeof(WCHAR), &got, NULL);
    FlushFileBuffers(p); DisconnectNamedPipe(p); CloseHandle(p);
    return 0;
}

int sys_helper_main(const WCHAR *file)
{
    char *t = read_small(file, 512), *nl;
    WCHAR pipe[96];
    DWORD store_pid;
    HANDLE store, p;
    if (!t || !(nl = strchr(t, '\n'))) { free(t); return SYS_FAILED; }
    *nl = 0;
    MultiByteToWideChar(CP_UTF8, 0, t, -1, pipe, ARRAYSIZE(pipe));
    store_pid = strtoul(nl + 1, NULL, 10);
    free(t);
    DeleteFileW(file);   /* read: the store sees it is running */
    {   /* the gate counts the consents: SG_STORE_DUMP's .helper */
        WCHAR d[MAX_PATH], line[200];
        FILE *f;
        if (GetEnvironmentVariableW(L"SG_STORE_DUMP", d, MAX_PATH - 8) && lstrcatW(d, L".helper") && (f = _wfopen(d, L"ab"))) {
            _snwprintf(line, ARRAYSIZE(line), L"helper %lu %ls\n", GetCurrentProcessId(), pipe);
            fprintf(f, "%ls", line);
            fclose(f);
        }
    }
    if (wcsncmp(pipe, L"\\\\.\\pipe\\sg-store-helper-", 25) || !(store = OpenProcess(SYNCHRONIZE, FALSE, store_pid)))
        return SYS_FAILED;
    CloseHandle(CreateThread(NULL, 0, helper_watch, store, 0, NULL));
    g_helper_store_pid = store_pid;
    for (;;) {   /* a listening instance always; each client on a thread of its own */
        p = CreateNamedPipeW(pipe, PIPE_ACCESS_DUPLEX, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                             PIPE_UNLIMITED_INSTANCES, 8192, 8192, 0, NULL);
        if (p == INVALID_HANDLE_VALUE) return SYS_FAILED;
        if (!ConnectNamedPipe(p, NULL) && GetLastError() != ERROR_PIPE_CONNECTED) { CloseHandle(p); continue; }
        CloseHandle(CreateThread(NULL, 0, helper_client, p, 0, NULL));
    }
}

static void outcome_text(int code, const WCHAR *what, BOOL removing, WCHAR *err, int cch)
{
    const WCHAR *verb = removing ? L"uninstalled" : L"installed";
    if (code == SYS_OK) err[0] = 0;
    else if (code == SYS_DENIED) swprintf(err, cch, L"%ls was not %ls: an administrator did not allow it.", what, verb);
    else if (code == SYS_CANCELLED) swprintf(err, cch, L"%ls was not %ls.", what, verb);
    else swprintf(err, cch, L"%ls was not %ls. The %ls window said why.", what, verb, removing ? L"uninstall" : L"installer");
}

static void quote_arg(WCHAR *dst, int cch, const WCHAR *s)
{
    /* the values here never hold quotes (package names, versions, paths) */
    _snwprintf(dst, cch, L"\"%ls\"", s);
    dst[cch - 1] = 0;
}

int sys_install_apt(const app_t *a, WCHAR *err, int cch)
{
    WCHAR args[1024], q1[200], q2[300];
    int code;
    quote_arg(q1, ARRAYSIZE(q1), a->apt_pkg);
    quote_arg(q2, ARRAYSIZE(q2), a->name);
    _snwprintf(args, ARRAYSIZE(args), L"--elevated-apt %ls %ls", q1, q2);
    args[ARRAYSIZE(args) - 1] = 0;
    code = elevate_wait(args);
    outcome_text(code, a->name, FALSE, err, cch);
    return code;
}

/* Uninstall: the same consent, sg-admind's apt-remove */
int sys_remove_apt(const app_t *a, WCHAR *err, int cch)
{
    WCHAR args[1024], q1[200], q2[300];
    int code;
    quote_arg(q1, ARRAYSIZE(q1), a->apt_pkg);
    quote_arg(q2, ARRAYSIZE(q2), a->name);
    _snwprintf(args, ARRAYSIZE(args), L"--elevated-apt-remove %ls %ls", q1, q2);
    args[ARRAYSIZE(args) - 1] = 0;
    code = elevate_wait(args);
    outcome_text(code, a->name, TRUE, err, cch);
    return code;
}

/* ---- the sg-admind spool (the elevated side) ---------------------------------------------------- */

typedef BOOLEAN (WINAPI *rtlgenrandom_t)(PVOID, ULONG);

static BOOL random_id(WCHAR *out)
{
    BYTE b[16];
    int i;
    rtlgenrandom_t gen = (rtlgenrandom_t)(void *)GetProcAddress(LoadLibraryW(L"advapi32.dll"), "SystemFunction036");
    if (!gen || !gen(b, sizeof(b))) return FALSE;
    for (i = 0; i < 16; i++) _snwprintf(out + i * 2, 3, L"%02x", b[i]);
    out[32] = 0;
    return TRUE;
}

static char *read_small(const WCHAR *path, DWORD max)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    char *buf;
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!(buf = malloc(max + 1))) { CloseHandle(h); return NULL; }
    ReadFile(h, buf, max, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    return buf;
}

/* the job the elevated window runs */
typedef struct {
    WCHAR title[160];             /* "Installing GIMP" / "Uninstalling GIMP" */
    WCHAR name[128];              /* "GIMP" */
    BOOL  removing;               /* apt-remove: an uninstall */
    const WCHAR *verb;            /* apt-install | apt-remove | deb-install */
    WCHAR arg[3][MAX_PATH];
    int   nargs;
    WCHAR deb_src[MAX_PATH];      /* deb-install: the file to stage */
    /* progress and result, shared with the window */
    volatile LONG percent;        /* -1: not known yet */
    WCHAR status[256];
    WCHAR result[512];
    volatile LONG done;           /* 1 = finished */
    int   code;                   /* SYS_* */
    HWND  wnd;
    WCHAR dump[MAX_PATH];
} job_t;

static void job_dump(job_t *j, const WCHAR *fmt, ...)
{
    WCHAR buf[1024];
    char u[2048];
    va_list ap;
    FILE *f;
    if (!j->dump[0]) return;
    va_start(ap, fmt);
    _vsnwprintf(buf, ARRAYSIZE(buf), fmt, ap);
    va_end(ap);
    buf[ARRAYSIZE(buf) - 1] = 0;
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, u, sizeof(u), NULL, NULL);
    if ((f = _wfopen(j->dump, L"ab"))) { fprintf(f, "%s\n", u); fclose(f); }
}

static void set_status(job_t *j, int percent, const WCHAR *text)
{
    if (percent >= 0) InterlockedExchange(&j->percent, percent);
    if (text) lstrcpynW(j->status, text, ARRAYSIZE(j->status));
    if (j->wnd) InvalidateRect(j->wnd, NULL, FALSE);
}

/* PROGRESS <percent>\n<what apt is doing>\n, written by sg-admind */
static void read_progress(job_t *j, const WCHAR *path)
{
    char *t = read_small(path, 2048), *nl;
    WCHAR text[256];
    int pct;
    if (!t) return;
    if (!strncmp(t, "PROGRESS ", 9)) {
        pct = atoi(t + 9);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        text[0] = 0;
        if ((nl = strchr(t, '\n'))) {
            char *end = strchr(nl + 1, '\n');
            if (end) *end = 0;
            MultiByteToWideChar(CP_UTF8, 0, nl + 1, -1, text, ARRAYSIZE(text));
        }
        if (pct != j->percent || lstrcmpW(text, j->status)) {
            set_status(j, pct, text[0] ? text : NULL);
            job_dump(j, L"progress %d %ls", pct, text);
        }
    }
    free(t);
}

static BOOL stage_deb(job_t *j, const WCHAR *id, WCHAR *staged_name, int cch)
{
    WCHAR dir[MAX_PATH], dst[MAX_PATH];
    if (!env_dir(L"SG_ADMIN_DEBS", "/var/cache/stained-glass-admin/debs", "", dir, MAX_PATH)) {
        lstrcpynW(j->result, L"The staging folder for packages is missing.", ARRAYSIZE(j->result));
        return FALSE;
    }
    _snwprintf(staged_name, cch, L"%ls.deb", id);
    _snwprintf(dst, MAX_PATH, L"%ls\\%ls", dir, staged_name);
    dst[MAX_PATH - 1] = 0;
    if (!CopyFileW(j->deb_src, dst, TRUE)) {
        DWORD e = GetLastError();
        _snwprintf(j->result, ARRAYSIZE(j->result),
                   e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND
                   ? L"The package file could not be found." : L"The package file could not be read (error %lu).", e);
        j->result[ARRAYSIZE(j->result) - 1] = 0;
        return FALSE;
    }
    return TRUE;
}

static DWORD WINAPI job_thread(void *arg)
{
    job_t *j = arg;
    WCHAR id[40], reqdir[MAX_PATH], repdir[MAX_PATH], tmp[MAX_PATH], req[MAX_PATH], rep[MAX_PATH], prog[MAX_PATH];
    WCHAR staged[80] = L"";
    const WCHAR *fields[5];
    char body[4096];
    int n = 0, len = 0, i;
    HANDLE h;
    DWORD put, start;
    BOOL ok = FALSE;

    j->code = SYS_FAILED;
    set_status(j, -1, j->removing ? L"Asking the system to uninstall it..." : L"Asking the system to install it...");
    if (!random_id(id)) { lstrcpynW(j->result, L"The request could not be made.", ARRAYSIZE(j->result)); goto out; }
    if (!env_dir(L"SG_ADMIN_SPOOL", "/run/stained-glass-admin", "requests", reqdir, MAX_PATH) ||
        !env_dir(L"SG_ADMIN_SPOOL", "/run/stained-glass-admin", "replies", repdir, MAX_PATH)) {
        lstrcpynW(j->result, L"The administration service is not available.", ARRAYSIZE(j->result));
        goto out;
    }
    fields[n++] = j->verb;
    if (!lstrcmpW(j->verb, L"deb-install")) {
        set_status(j, -1, L"Copying the package...");
        if (!stage_deb(j, id, staged, ARRAYSIZE(staged))) goto out;
        fields[n++] = staged;
    }
    for (i = 0; i < j->nargs; i++) fields[n++] = j->arg[i];
    for (i = 0; i < n; i++) {
        char u[1024];
        int k;
        if (wcspbrk(fields[i], L"\r\n")) { lstrcpynW(j->result, L"A value cannot contain a line break.", ARRAYSIZE(j->result)); goto out; }
        k = WideCharToMultiByte(CP_UTF8, 0, fields[i], -1, u, sizeof(u), NULL, NULL);
        if (k <= 0 || len + k + 1 >= (int)sizeof(body)) { lstrcpynW(j->result, L"The request is too long.", ARRAYSIZE(j->result)); goto out; }
        memcpy(body + len, u, k - 1);
        len += k - 1;
        body[len++] = '\n';
    }
    _snwprintf(tmp, MAX_PATH, L"%ls\\.%ls", reqdir, id);
    _snwprintf(req, MAX_PATH, L"%ls\\%ls.req", reqdir, id);
    _snwprintf(rep, MAX_PATH, L"%ls\\%ls.rep", repdir, id);
    _snwprintf(prog, MAX_PATH, L"%ls\\%ls.progress", repdir, id);
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        lstrcpynW(j->result, L"Only an administrator can install system packages.", ARRAYSIZE(j->result));
        goto out;
    }
    ok = WriteFile(h, body, len, &put, NULL) && put == (DWORD)len;
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp, req, 0)) {
        DeleteFileW(tmp);
        lstrcpynW(j->result, L"The request could not be made.", ARRAYSIZE(j->result));
        ok = FALSE;
        goto out;
    }
    ok = FALSE;
    job_dump(j, L"request %ls", j->verb);
    set_status(j, 0, L"Waiting for the package manager...");
    /* apt can take long (a large program, a slow mirror): three hours */
    for (start = GetTickCount(); GetFileAttributesW(rep) == INVALID_FILE_ATTRIBUTES; ) {
        if (GetTickCount() - start > 3 * 3600 * 1000) { lstrcpynW(j->result, L"The install did not finish in time.", ARRAYSIZE(j->result)); goto out; }
        read_progress(j, prog);
        Sleep(250);
    }
    {
        char *t = read_small(rep, 8192);
        WCHAR wide[1024], *nl;
        if (!t) { lstrcpynW(j->result, L"The answer could not be read.", ARRAYSIZE(j->result)); goto out; }
        MultiByteToWideChar(CP_UTF8, 0, t, -1, wide, ARRAYSIZE(wide));
        free(t);
        if (!wcsncmp(wide, L"OK", 2) && (wide[2] == L'\n' || !wide[2])) {
            WCHAR detail[256] = L"", *extra = NULL;
            if (wide[2] == L'\n') {
                lstrcpynW(detail, wide + 3, ARRAYSIZE(detail));
                if ((nl = wcschr(detail, L'\n'))) { *nl = 0; if (!wcsncmp(nl + 1, L"Not installed: ", 15)) extra = nl + 16; }
                if (extra && (nl = wcschr(extra, L'\n'))) *nl = 0;
            }
            ok = TRUE;
            /* the package, and what it recommends that could not be had
             * (Steam's libraries, from Steam's own server) */
            if (j->removing)
                _snwprintf(j->result, ARRAYSIZE(j->result), L"%ls is uninstalled. Your documents and settings are kept.", j->name);
            else if (extra && *extra)
                _snwprintf(j->result, ARRAYSIZE(j->result), L"%ls is installed, without %ls.", detail[0] ? detail : j->title + 11, extra);
            else
                _snwprintf(j->result, ARRAYSIZE(j->result), L"%ls is installed.", detail[0] ? detail : j->title + 11);
        } else if (!wcsncmp(wide, L"FAILED ", 7)) {
            WCHAR *e = wcschr(wide + 7, L'\n');
            if (e) *e = 0;
            lstrcpynW(j->result, wide + 7, ARRAYSIZE(j->result));
        } else lstrcpynW(j->result, L"The administration service gave no answer.", ARRAYSIZE(j->result));
    }
out:
    if (staged[0] && !ok) {
        WCHAR dir[MAX_PATH], f[MAX_PATH];
        if (env_dir(L"SG_ADMIN_DEBS", "/var/cache/stained-glass-admin/debs", "", dir, MAX_PATH)) {
            _snwprintf(f, MAX_PATH, L"%ls\\%ls", dir, staged);
            DeleteFileW(f);         /* sg-admind removes it when it takes it */
        }
    }
    j->code = ok ? SYS_OK : SYS_FAILED;
    if (ok) InterlockedExchange(&j->percent, 100);
    job_dump(j, L"result %ls %ls", ok ? L"ok" : L"fail", j->result);
    InterlockedExchange(&j->done, 1);
    if (j->wnd) { InvalidateRect(j->wnd, NULL, FALSE); PostMessageW(j->wnd, WM_APP, 0, 0); }
    return 0;
}

/* ---- small windows: text, a progress bar, buttons ------------------------------------------------ */

static void make_fonts(void)
{
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    HDC dc = GetDC(NULL);
    if (f_body) return;
    s_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    f_body = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -S(11); f_small = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -S(17); ncm.lfMessageFont.lfWeight = FW_SEMIBOLD; f_head = CreateFontIndirectW(&ncm.lfMessageFont);
}

static void draw_text(HDC dc, HFONT font, COLORREF col, RECT rc, const WCHAR *s, UINT fmt)
{
    HFONT old = SelectObject(dc, font);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &rc, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void draw_button(HDC dc, RECT rc, const WCHAR *label, BOOL primary, BOOL focus)
{
    HBRUSH b = CreateSolidBrush(primary ? C_ACCENT : RGB(255, 255, 255));
    HPEN pen = CreatePen(PS_SOLID, focus ? 2 : 1, focus ? C_TEXT : primary ? C_ACCENT : C_LINE), op = SelectObject(dc, pen);
    HBRUSH ob = SelectObject(dc, b);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, S(6), S(6));
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(pen);
    draw_text(dc, f_body, primary ? RGB(255, 255, 255) : C_TEXT, rc, label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

static void draw_bar(HDC dc, RECT rc, int percent)
{
    HBRUSH track = CreateSolidBrush(RGB(0xe0, 0xe0, 0xe0)), fill = CreateSolidBrush(C_ACCENT);
    RECT f = rc;
    FillRect(dc, &rc, track);
    if (percent < 0) {
        /* not known yet: a moving block */
        int w = (rc.right - rc.left) / 4, span = rc.right - rc.left + w;
        int x = rc.left - w + (int)((GetTickCount() / 8) % (DWORD)span);
        f.left = max(rc.left, x); f.right = min(rc.right, x + w);
    } else f.right = rc.left + (rc.right - rc.left) * percent / 100;
    if (f.right > f.left) FillRect(dc, &f, fill);
    DeleteObject(track); DeleteObject(fill);
}

static void write_screen_hit(const WCHAR *dump, HWND hwnd, const WCHAR *what, RECT rc)
{
    POINT pt = { (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
    char u[256];
    FILE *f;
    if (!dump[0]) return;
    ClientToScreen(hwnd, &pt);
    _snprintf(u, sizeof(u), "hit %ls %ld %ld\n", what, pt.x, pt.y);
    u[sizeof(u) - 1] = 0;
    if ((f = _wfopen(dump, L"ab"))) { fputs(u, f); fclose(f); }
}

/* ---- the elevated window: progress, then the result ------------------------------------------- */

static RECT close_rc(HWND hwnd)
{
    RECT c, b;
    GetClientRect(hwnd, &c);
    b.right = c.right - S(20); b.left = b.right - S(110); b.bottom = c.bottom - S(16); b.top = b.bottom - S(32);
    return b;
}

static LRESULT CALLBACK job_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    job_t *j = (job_t *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        SetTimer(hwnd, 1, 60, NULL);
        return 0;
    case WM_TIMER:
        if (wp == 2) { DestroyWindow(hwnd); return 0; }
        if (j && !j->done) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_APP:
        KillTimer(hwnd, 1);
        InvalidateRect(hwnd, NULL, FALSE);
        UpdateWindow(hwnd);
        write_screen_hit(j->dump, hwnd, L"close", close_rc(hwnd));
        SetForegroundWindow(hwnd);
        /* installed: it goes by itself a moment later (the Store walk: it
         * waited for a click); a failure stays to be read */
#ifndef SG_MUTANT_STAYOPEN
        if (j->code == SYS_OK) SetTimer(hwnd, 2, 1500, NULL);
#endif
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(hwnd, &ps), dc = CreateCompatibleDC(wdc);
        RECT c, r;
        HBITMAP bmp, ob;
        HBRUSH bg = CreateSolidBrush(C_BG);
        GetClientRect(hwnd, &c);
        bmp = CreateCompatibleBitmap(wdc, c.right, c.bottom);
        ob = SelectObject(dc, bmp);
        FillRect(dc, &c, bg);
        DeleteObject(bg);
        r = c; r.left = S(20); r.right -= S(20); r.top = S(16); r.bottom = r.top + S(28);
        {
            WCHAR head[200];
            /* "Installing GIMP", then "GIMP is installed" / "GIMP was not installed" */
            if (j->done && j->removing)
                _snwprintf(head, ARRAYSIZE(head), j->code == SYS_OK ? L"%ls is uninstalled" : L"%ls was not uninstalled", j->name);
            else if (j->done) _snwprintf(head, ARRAYSIZE(head), j->code == SYS_OK ? L"%ls is installed" : L"%ls was not installed", j->title + 11);
            else lstrcpynW(head, j->title, ARRAYSIZE(head));
            head[ARRAYSIZE(head) - 1] = 0;
            draw_text(dc, f_head, C_TEXT, r, head, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        r.top = r.bottom + S(10); r.bottom = r.top + S(8);
        draw_bar(dc, r, j->done ? (j->code == SYS_OK ? 100 : j->percent < 0 ? 0 : j->percent) : j->percent);
        r.top = r.bottom + S(10); r.bottom = r.top + S(44);
        if (j->done)
            draw_text(dc, f_body, j->code == SYS_OK ? C_ACCENT : C_ERR, r, j->result, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
        else
            draw_text(dc, f_body, C_SUB, r, j->status, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
        if (j->done) draw_button(dc, close_rc(hwnd), L"Close", TRUE, TRUE);
        BitBlt(wdc, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT b = close_rc(hwnd);
        if (j->done && PtInRect(&b, pt)) DestroyWindow(hwnd);
        return 0;
    }
    case WM_KEYDOWN:
        if (j->done && (wp == VK_RETURN || wp == VK_ESCAPE || wp == VK_SPACE)) DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        /* never stop apt half way; closing is for when it is done */
        if (j->done) DestroyWindow(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int run_job(job_t *j)
{
    WNDCLASSW wc = { 0 };
    MSG m;
    RECT wa;
    HANDLE t;
    make_fonts();
    g_inst = GetModuleHandleW(NULL);
    GetEnvironmentVariableW(L"SG_STORE_DUMP", j->dump, MAX_PATH);
    if (j->dump[0]) { lstrcatW(j->dump, L".sys"); DeleteFileW(j->dump); }
    InterlockedExchange(&j->percent, -1);
    wc.lpfnWndProc = job_proc;
    wc.hInstance = g_inst;
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"SgStoreInstall";
    RegisterClassW(&wc);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    j->wnd = CreateWindowExW(0, wc.lpszClassName, j->title, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             wa.left + (wa.right - wa.left - S(480)) / 2, wa.top + (wa.bottom - wa.top - S(200)) / 3,
                             S(480), S(200), NULL, NULL, g_inst, j);
    if (!j->wnd) return SYS_FAILED;
    ShowWindow(j->wnd, SW_SHOWNORMAL);
    UpdateWindow(j->wnd);
    job_dump(j, L"window 1");
    if (!(t = CreateThread(NULL, 0, job_thread, j, 0, NULL))) return SYS_FAILED;
    CloseHandle(t);
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    return j->done ? j->code : SYS_FAILED;
}

int sys_elevated_main(int argc, WCHAR **argv, int i)
{
    static job_t j;
    memset(&j, 0, sizeof(j));
    if (!lstrcmpiW(argv[i], L"--elevated-apt-remove") && i + 2 < argc) {
        j.verb = L"apt-remove";
        j.removing = TRUE;
        lstrcpynW(j.arg[0], argv[i + 1], MAX_PATH);
        lstrcpynW(j.name, argv[i + 2], ARRAYSIZE(j.name));
        j.nargs = 1;
        _snwprintf(j.title, ARRAYSIZE(j.title), L"Uninstalling %ls", argv[i + 2]);
    } else if (!lstrcmpiW(argv[i], L"--elevated-apt") && i + 2 < argc) {
        j.verb = L"apt-install";
        lstrcpynW(j.arg[0], argv[i + 1], MAX_PATH);
        j.nargs = 1;
        _snwprintf(j.title, ARRAYSIZE(j.title), L"Installing %ls", argv[i + 2]);
    } else if (!lstrcmpiW(argv[i], L"--elevated-deb") && i + 3 < argc) {
        j.verb = L"deb-install";
        lstrcpynW(j.deb_src, argv[i + 1], MAX_PATH);
        lstrcpynW(j.arg[0], argv[i + 2], MAX_PATH);
        lstrcpynW(j.arg[1], argv[i + 3], MAX_PATH);
        j.nargs = 2;
        _snwprintf(j.title, ARRAYSIZE(j.title), L"Installing %ls", argv[i + 2]);
    } else return SYS_FAILED;
    j.title[ARRAYSIZE(j.title) - 1] = 0;
    return run_job(&j);
}

/* ---- --deb FILE: what the package is, and Install ------------------------------------------------ */

typedef struct {
    WCHAR file[MAX_PATH], shown[MAX_PATH];
    WCHAR pkg[128], version[128], arch[32], maintainer[256], desc[256], homepage[256], size[32], installed[128];
    BOOL  valid, arch_ok;
    WCHAR error[256];
    int   state;                  /* 0 asking, 1 installing, 2 done */
    int   code;
    WCHAR result[512];
    int   focus;                  /* 0 Install, 1 Cancel */
    WCHAR dump[MAX_PATH];
    HWND  wnd;
} debwin_t;

static debwin_t g_deb;

static void field(const char *text, const char *key, WCHAR *out, int cch)
{
    const char *p = text;
    size_t kl = strlen(key);
    out[0] = 0;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == ' ') {
            char v[1024];
            size_t n = len - kl - 1;
            if (n >= sizeof(v)) n = sizeof(v) - 1;
            memcpy(v, p + kl + 1, n);
            v[n] = 0;
            MultiByteToWideChar(CP_UTF8, 0, v, -1, out, cch);
            return;
        }
        p = nl ? nl + 1 : NULL;
    }
}

/* sg-debinfo (a Unix helper, dpkg-deb's reading of the file) */
static void deb_read_info(debwin_t *d)
{
    WCHAR helper[MAX_PATH] = L"/usr/libexec/stained-glass/shell/sg-debinfo", args[2048], tmpdir[MAX_PATH], out[MAX_PATH];
    char ufile[MAX_PATH * 3], uout[MAX_PATH * 3], magic[8] = { 0 };
    WCHAR wufile[MAX_PATH * 3], wuout[MAX_PATH * 3];
    HANDLE h, process;
    DWORD got = 0;
    char *text;
    d->valid = FALSE;
    /* an ar archive whose first member is debian-binary: a .deb */
    h = CreateFileW(d->file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) { lstrcpynW(d->error, L"The file could not be opened.", ARRAYSIZE(d->error)); return; }
    ReadFile(h, magic, 8, &got, NULL);
    CloseHandle(h);
#ifndef SG_MUTANT_NOMAGIC
    if (got != 8 || memcmp(magic, "!<arch>\n", 8)) {
        lstrcpynW(d->error, L"This file is not a Linux software package (.deb), so it cannot be installed.", ARRAYSIZE(d->error));
        return;
    }
#endif
    GetEnvironmentVariableW(L"SG_DEBINFO", helper, MAX_PATH);
    GetTempPathW(MAX_PATH, tmpdir);
    _snwprintf(out, MAX_PATH, L"%lssg-debinfo-%lu.txt", tmpdir, GetCurrentProcessId());
    DeleteFileW(out);
    CloseHandle(CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));   /* so it has a Unix name */
    if (!sys_unix_path(d->file, ufile, sizeof(ufile)) || !sys_unix_path(out, uout, sizeof(uout))) {
        lstrcpynW(d->error, L"The file could not be read.", ARRAYSIZE(d->error));
        return;
    }
    DeleteFileW(out);
    MultiByteToWideChar(CP_UTF8, 0, ufile, -1, wufile, ARRAYSIZE(wufile));
    MultiByteToWideChar(CP_UTF8, 0, uout, -1, wuout, ARRAYSIZE(wuout));
    _snwprintf(args, ARRAYSIZE(args), L"\"%ls\" \"%ls\"", wufile, wuout);
    args[ARRAYSIZE(args) - 1] = 0;
    if (!run_unix(helper, args, &process)) {
        lstrcpynW(d->error, L"Linux packages cannot be read here (sg-debinfo is missing).", ARRAYSIZE(d->error));
        return;
    }
    WaitForSingleObject(process, 30000);
    CloseHandle(process);
    for (got = 0; GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES && got < 5000; got += 50) Sleep(50);
    if (!(text = read_small(out, 65536))) { lstrcpynW(d->error, L"The package could not be read.", ARRAYSIZE(d->error)); return; }
    DeleteFileW(out);
    field(text, "PACKAGE", d->pkg, ARRAYSIZE(d->pkg));
    field(text, "VERSION", d->version, ARRAYSIZE(d->version));
    field(text, "ARCH", d->arch, ARRAYSIZE(d->arch));
    field(text, "MAINTAINER", d->maintainer, ARRAYSIZE(d->maintainer));
    field(text, "DESCRIPTION", d->desc, ARRAYSIZE(d->desc));
    field(text, "HOMEPAGE", d->homepage, ARRAYSIZE(d->homepage));
    field(text, "SIZE", d->size, ARRAYSIZE(d->size));
    field(text, "INSTALLED", d->installed, ARRAYSIZE(d->installed));
    {
        WCHAR ok[8];
        field(text, "ARCH_OK", ok, ARRAYSIZE(ok));
        d->arch_ok = !lstrcmpW(ok, L"yes");
    }
    if (strstr(text, "\nOK") || !strncmp(text, "OK", 2)) d->valid = d->pkg[0] && d->version[0];
    if (!d->valid) {
        field(text, "ERROR invalid", d->error, ARRAYSIZE(d->error));
        if (!d->error[0]) lstrcpynW(d->error, L"This file is not a Linux software package (.deb), so it cannot be installed.", ARRAYSIZE(d->error));
    } else if (!d->arch_ok) {
        d->valid = FALSE;
        _snwprintf(d->error, ARRAYSIZE(d->error), L"This package is built for another kind of processor (%ls), so it cannot be installed on this PC.", d->arch);
    }
    free(text);
}

static void deb_dump(debwin_t *d)
{
    WCHAR tmp[MAX_PATH + 8];
    FILE *f;
    char u[1024];
    if (!d->dump[0]) return;
    _snwprintf(tmp, ARRAYSIZE(tmp), L"%ls.tmp", d->dump);
    if (!(f = _wfopen(tmp, L"wb"))) return;
#define DL(fmt, ...) do { WCHAR w_[1024]; _snwprintf(w_, 1024, fmt, __VA_ARGS__); w_[1023] = 0; \
        WideCharToMultiByte(CP_UTF8, 0, w_, -1, u, sizeof(u), NULL, NULL); fputs(u, f); fputc('\n', f); } while (0)
    DL(L"deb-window %d", d->wnd ? 1 : 0);
    DL(L"valid %d", d->valid ? 1 : 0);
    DL(L"package %ls", d->pkg);
    DL(L"version %ls", d->version);
    DL(L"maker %ls", d->maintainer);
    DL(L"state %d", d->state);
    if (!d->valid) DL(L"error %ls", d->error);
    if (d->state == 2) DL(L"result %d %ls", d->code, d->result);
#undef DL
    fclose(f);
    MoveFileExW(tmp, d->dump, MOVEFILE_REPLACE_EXISTING);
}

static RECT deb_button(HWND hwnd, int which)     /* 0 primary (Install / Close), 1 Cancel */
{
    RECT c, b;
    GetClientRect(hwnd, &c);
    b.bottom = c.bottom - S(18); b.top = b.bottom - S(34);
    b.right = c.right - S(20) - (which ? 0 : S(130)); b.left = b.right - S(120);
    return b;
}

static DWORD WINAPI deb_worker(void *arg)
{
    debwin_t *d = arg;
    WCHAR args[2048], q1[MAX_PATH + 4], q2[200], q3[200], what[300];
    quote_arg(q1, ARRAYSIZE(q1), d->file);
    quote_arg(q2, ARRAYSIZE(q2), d->pkg);
    quote_arg(q3, ARRAYSIZE(q3), d->version);
    _snwprintf(args, ARRAYSIZE(args), L"--elevated-deb %ls %ls %ls", q1, q2, q3);
    args[ARRAYSIZE(args) - 1] = 0;
    d->code = elevate_wait(args);
    _snwprintf(what, ARRAYSIZE(what), L"%ls %ls", d->pkg, d->version);
    if (d->code == SYS_OK) _snwprintf(d->result, ARRAYSIZE(d->result), L"%ls is installed.", what);
    else outcome_text(d->code, what, FALSE, d->result, ARRAYSIZE(d->result));
    d->state = 2;
    PostMessageW(d->wnd, WM_APP, 0, 0);
    return 0;
}

static void deb_start(debwin_t *d)
{
    HANDLE t;
    if (d->state != 0 || !d->valid) return;
    d->state = 1;
    InvalidateRect(d->wnd, NULL, FALSE);
    deb_dump(d);
    if ((t = CreateThread(NULL, 0, deb_worker, d, 0, NULL))) CloseHandle(t);
    else { d->state = 2; d->code = SYS_FAILED; lstrcpynW(d->result, L"The install could not be started.", ARRAYSIZE(d->result)); }
}

static void deb_paint(HWND hwnd, debwin_t *d)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps), dc = CreateCompatibleDC(wdc);
    RECT c, r;
    HBITMAP bmp, ob;
    HBRUSH bg = CreateSolidBrush(C_BG);
    WCHAR line[600];
    int y;
    GetClientRect(hwnd, &c);
    bmp = CreateCompatibleBitmap(wdc, c.right, c.bottom);
    ob = SelectObject(dc, bmp);
    FillRect(dc, &c, bg);
    DeleteObject(bg);

    r.left = S(20); r.right = c.right - S(20); r.top = S(16); r.bottom = r.top + S(30);
    draw_text(dc, f_head, C_TEXT, r, d->valid ? (d->desc[0] ? d->desc : d->pkg) : L"This file cannot be installed", DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    y = r.bottom + S(8);
    if (d->valid) {
        static const WCHAR *const labels[] = { L"Package", L"Version", L"From", L"Web site", L"Size", L"File" };
        const WCHAR *values[6];
        int k;
        values[0] = d->pkg; values[1] = d->version; values[2] = d->maintainer[0] ? d->maintainer : L"(not given)";
        values[3] = d->homepage; values[4] = d->size; values[5] = d->shown;
        for (k = 0; k < 6; k++) {
            RECT lr = { S(20), y, S(110), y + S(22) }, vr = { S(114), y, c.right - S(20), y + S(22) };
            if (!values[k][0]) continue;
            draw_text(dc, f_body, C_SUB, lr, labels[k], DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            draw_text(dc, f_body, C_TEXT, vr, values[k], DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_PATH_ELLIPSIS);
            y += S(22);
        }
        y += S(8);
        r.top = y; r.bottom = y + S(52);
        if (d->state == 0) {
            if (d->installed[0] && lstrcmpW(d->installed, L"-"))
                _snwprintf(line, ARRAYSIZE(line), L"Version %ls is installed now; this replaces it. ", d->installed);
            else line[0] = 0;
            lstrcatW(line, L"This package does not come from SG Store: install it only if you trust who made it. "
                           L"Installing needs an administrator's permission.");
            draw_text(dc, f_small, C_SUB, r, line, DT_LEFT | DT_TOP | DT_WORDBREAK);
        } else if (d->state == 1) {
            draw_text(dc, f_body, C_SUB, r, L"Installing... The installer window shows the progress.", DT_LEFT | DT_TOP | DT_WORDBREAK);
        } else {
            draw_text(dc, f_body, d->code == SYS_OK ? C_ACCENT : C_ERR, r, d->result, DT_LEFT | DT_TOP | DT_WORDBREAK);
        }
    } else {
        r.top = y; r.bottom = y + S(80);
        draw_text(dc, f_body, C_ERR, r, d->error, DT_LEFT | DT_TOP | DT_WORDBREAK);
        r.top = r.bottom; r.bottom = r.top + S(24);
        draw_text(dc, f_small, C_SUB, r, d->shown, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }
    if (d->valid && d->state == 0) {
        draw_button(dc, deb_button(hwnd, 0), L"Install", TRUE, d->focus == 0);
        draw_button(dc, deb_button(hwnd, 1), L"Cancel", FALSE, d->focus == 1);
    } else if (d->state != 1) {
        draw_button(dc, deb_button(hwnd, 1), L"Close", TRUE, TRUE);
    }
    BitBlt(wdc, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static void deb_hits(debwin_t *d)
{
    deb_dump(d);
    if (d->valid && d->state == 0) {
        write_screen_hit(d->dump, d->wnd, L"install", deb_button(d->wnd, 0));
        write_screen_hit(d->dump, d->wnd, L"cancel", deb_button(d->wnd, 1));
    } else if (d->state != 1) write_screen_hit(d->dump, d->wnd, L"close", deb_button(d->wnd, 1));
}

static LRESULT CALLBACK deb_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    debwin_t *d = &g_deb;
    switch (msg) {
    case WM_PAINT:
        deb_paint(hwnd, d);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_APP:
        InvalidateRect(hwnd, NULL, FALSE);
        UpdateWindow(hwnd);
        deb_hits(d);
        SetForegroundWindow(hwnd);
        return 0;
    case WM_LBUTTONUP: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        RECT i = deb_button(hwnd, 0), c = deb_button(hwnd, 1);
        if (d->valid && d->state == 0 && PtInRect(&i, pt)) deb_start(d);
        else if (d->state != 1 && PtInRect(&c, pt)) DestroyWindow(hwnd);
        return 0;
    }
    case WM_KEYDOWN:
        if (d->state == 1) return 0;
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        else if (wp == VK_TAB || wp == VK_LEFT || wp == VK_RIGHT) { d->focus = !d->focus; InvalidateRect(hwnd, NULL, FALSE); }
        else if (wp == VK_RETURN || wp == VK_SPACE) {
            if (d->valid && d->state == 0 && d->focus == 0) deb_start(d);
            else DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        if (d->state != 1) DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int sys_deb_window(HINSTANCE inst, const WCHAR *file)
{
    debwin_t *d = &g_deb;
    WNDCLASSW wc = { 0 };
    MSG m;
    RECT wa;
    const WCHAR *base;
    make_fonts();
    g_inst = inst;
    memset(d, 0, sizeof(*d));
    GetFullPathNameW(file, MAX_PATH, d->file, NULL);
    base = wcsrchr(d->file, L'\\') ? wcsrchr(d->file, L'\\') + 1 : d->file;
    lstrcpynW(d->shown, base, MAX_PATH);
    GetEnvironmentVariableW(L"SG_STORE_DUMP", d->dump, MAX_PATH);
    deb_read_info(d);
    wc.lpfnWndProc = deb_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"SgStoreDeb";
    RegisterClassW(&wc);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    d->wnd = CreateWindowExW(0, wc.lpszClassName, L"Install a Linux package", WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             wa.left + (wa.right - wa.left - S(560)) / 2, wa.top + (wa.bottom - wa.top - S(360)) / 3,
                             S(560), S(360), NULL, NULL, inst, NULL);
    if (!d->wnd) return 1;
    ShowWindow(d->wnd, SW_SHOWNORMAL);
    UpdateWindow(d->wnd);
    deb_hits(d);
    while (GetMessageW(&m, NULL, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    d->wnd = NULL;
    deb_dump(d);
    return d->state == 2 ? d->code : d->valid ? SYS_CANCELLED : SYS_FAILED;
}
