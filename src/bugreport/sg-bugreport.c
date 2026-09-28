/*
 * Report a problem (sg-bugreport.exe): what a tester sends back.
 *
 *   sg-bugreport.exe                     a report about the system
 *   sg-bugreport.exe --run FILE [ARGS]   run FILE (a program or its shortcut)
 *                                        with Wine's debug logging, and report
 *                                        on it when it ends
 *   sg-bugreport.exe --window HWND       a report about a running window's
 *                                        program (the taskbar button's menu)
 *
 * The report is plain text: the tester's notes, then everything collected --
 * the program and its version, the machine (sg-bugreport-info), and the
 * debug log's highlights (sg-debug-run). All of it is shown before anything
 * leaves the window. Copy puts it on the clipboard; Save writes a .txt file;
 * Email opens the tester's own mail program addressed to the project, the
 * report on the clipboard to paste in full. Nothing is sent by this program.
 *
 * The debug log itself stays in %LOCALAPPDATA%\Stained Glass\Reports\<time>,
 * named in the report, for when a developer asks for all of it.
 *
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define COBJMACROS
#include <windows.h>
#include <commdlg.h>
#include <ctype.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define REPORT_TO     L"ke7oxh@gmail.com"
#define SHELL_DIR     "/usr/libexec/stained-glass/shell/"
#define ID_NOTES      101
#define ID_DETAILS    102
#define ID_COPY       103
#define ID_SAVE       104
#define ID_EMAIL      105
#define ID_CLOSE      106
#define ID_NOW        107
#define WM_RUN_DONE   (WM_APP + 1)
#define BANNER_H      64

static HINSTANCE g_inst;
static HWND g_main, g_notes, g_details, g_status, g_buttons[5], g_label_notes, g_label_details;
static HFONT g_font, g_title_font, g_mono;
static HBRUSH g_white;

/* what the report is about */
static WCHAR g_program[MAX_PATH];     /* its file, or empty for a system report */
static WCHAR g_args[2048];            /* for --run */
static WCHAR g_workdir[MAX_PATH];     /* for --run */
static WCHAR g_title[256];            /* its name as the tester knows it */
static WCHAR g_window_title[256];     /* for --window */
static DWORD g_pid;                   /* for --window */
static BOOL  g_running;               /* --run, still running */
static int   g_exit_code = -1;
static WCHAR g_dir[MAX_PATH];         /* this report's folder */
static char  g_udir[MAX_PATH * 3];    /* the same, as a Unix path */
static WCHAR *g_report;               /* the details, as shown */

static LONG (WINAPI *p_spawnvp)(char * const argv[], int wait);
static char * (CDECL *p_unix_name)(const WCHAR *dos);

/* ---- small helpers ---------------------------------------------------- */

static char *to_utf8(const WCHAR *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = HeapAlloc(GetProcessHeap(), 0, n ? n : 1);
    if (!n) { s[0] = 0; return s; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* a text file as UTF-16, at most max bytes of it (its end if longer); NULL if missing */
static WCHAR *read_text(const WCHAR *path, DWORD max)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    char *buf;
    WCHAR *w;
    int n;

    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    if (size > max) { SetFilePointer(f, -(LONG)max, NULL, FILE_END); size = max; }
    buf = HeapAlloc(GetProcessHeap(), 0, size + 1);
    ReadFile(f, buf, size, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;
    n = MultiByteToWideChar(CP_UTF8, 0, buf, got, NULL, 0);
    w = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(WCHAR));
    MultiByteToWideChar(CP_UTF8, 0, buf, got, w, n);
    w[n] = 0;
    HeapFree(GetProcessHeap(), 0, buf);
    return w;
}

/* growable UTF-16 text, with Windows line ends (an edit control needs them) */
struct text { WCHAR *s; size_t len, cap; };

static void add(struct text *t, const WCHAR *s)
{
    for (; s && *s; s++) {
        if (t->len + 3 >= t->cap) {
            t->cap = t->cap ? t->cap * 2 : 4096;
            t->s = t->s ? HeapReAlloc(GetProcessHeap(), 0, t->s, t->cap * sizeof(WCHAR))
                        : HeapAlloc(GetProcessHeap(), 0, t->cap * sizeof(WCHAR));
        }
        if (*s == '\n' && (t->len == 0 || t->s[t->len - 1] != '\r')) t->s[t->len++] = '\r';
        t->s[t->len++] = *s;
    }
    if (t->s) t->s[t->len] = 0;
}

static void addf(struct text *t, const WCHAR *fmt, ...)
{
    WCHAR buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, ARRAYSIZE(buf) - 1, fmt, ap);
    buf[ARRAYSIZE(buf) - 1] = 0;
    va_end(ap);
    add(t, buf);
}

static BOOL spawn(char **argv, BOOL wait)
{
    return p_spawnvp && !p_spawnvp(argv, wait);
}

/* ---- what the report is about ---------------------------------------- */

/* a shortcut stands for its target */
static void resolve_shortcut(void)
{
    IShellLinkW *link;
    IPersistFile *file;

    if (lstrcmpiW(PathFindExtensionW(g_program), L".lnk")) return;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link)))
        return;
    if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
        if (SUCCEEDED(IPersistFile_Load(file, g_program, STGM_READ))) {
            WCHAR target[MAX_PATH] = L"", args[2048] = L"", dir[MAX_PATH] = L"";
            IShellLinkW_GetPath(link, target, ARRAYSIZE(target), NULL, 0);
            IShellLinkW_GetArguments(link, args, ARRAYSIZE(args));
            IShellLinkW_GetWorkingDirectory(link, dir, ARRAYSIZE(dir));
            if (target[0]) {
                lstrcpynW(g_title, PathFindFileNameW(g_program), ARRAYSIZE(g_title));
                PathRemoveExtensionW(g_title);
                lstrcpynW(g_program, target, ARRAYSIZE(g_program));
                if (!g_args[0]) lstrcpynW(g_args, args, ARRAYSIZE(g_args));
                if (!g_workdir[0]) lstrcpynW(g_workdir, dir, ARRAYSIZE(g_workdir));
            }
        }
        IPersistFile_Release(file);
    }
    IShellLinkW_Release(link);
}

static void version_string(const WCHAR *path, const WCHAR *name, WCHAR *out, int size)
{
    DWORD handle, len = GetFileVersionInfoSizeW(path, &handle);
    struct { WORD lang, cp; } *tr;
    UINT n;
    void *data, *value;
    WCHAR key[128];

    out[0] = 0;
    if (!len || !(data = HeapAlloc(GetProcessHeap(), 0, len))) return;
    if (GetFileVersionInfoW(path, 0, len, data) &&
        VerQueryValueW(data, L"\\VarFileInfo\\Translation", (void **)&tr, &n) && n >= sizeof(*tr)) {
        _snwprintf(key, ARRAYSIZE(key), L"\\StringFileInfo\\%04x%04x\\%s", tr->lang, tr->cp, name);
        if (VerQueryValueW(data, key, &value, &n) && n) lstrcpynW(out, value, size);
    }
    HeapFree(GetProcessHeap(), 0, data);
}

/* 32- or 64-bit, from the PE header */
static const WCHAR *machine_of(const WCHAR *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    IMAGE_DOS_HEADER dos;
    DWORD got, sig;
    WORD machine = 0;

    if (f == INVALID_HANDLE_VALUE) return L"?";
    if (ReadFile(f, &dos, sizeof(dos), &got, NULL) && got == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE &&
        SetFilePointer(f, dos.e_lfanew, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
        ReadFile(f, &sig, sizeof(sig), &got, NULL) && sig == IMAGE_NT_SIGNATURE)
        ReadFile(f, &machine, sizeof(machine), &got, NULL);
    CloseHandle(f);
    switch (machine) {
    case IMAGE_FILE_MACHINE_AMD64: return L"64-bit (x64)";
    case IMAGE_FILE_MACHINE_I386:  return L"32-bit (x86)";
    case IMAGE_FILE_MACHINE_ARM64: return L"ARM64";
    case 0: return L"no program header (not an .exe)";
    default: return L"other";
    }
}

/* %LOCALAPPDATA%\Stained Glass\Reports\YYYYMMDD-HHMMSS, and its Unix path */
static void make_report_dir(void)
{
    WCHAR base[MAX_PATH];
    SYSTEMTIME t;
    char *u;

    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, ARRAYSIZE(base))) GetTempPathW(ARRAYSIZE(base), base);
    GetLocalTime(&t);
    _snwprintf(g_dir, ARRAYSIZE(g_dir), L"%s\\Stained Glass\\Reports\\%04u%02u%02u-%02u%02u%02u", base,
               t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    SHCreateDirectoryExW(NULL, g_dir, NULL);
    if (p_unix_name && (u = p_unix_name(g_dir))) {
        lstrcpynA(g_udir, u, sizeof(g_udir));
        HeapFree(GetProcessHeap(), 0, u);
    }
}

static void collect_system(void)
{
    char out[sizeof(g_udir) + 16];
    char *argv[] = { (char *)SHELL_DIR "sg-bugreport-info", out, NULL };
    if (!g_udir[0]) return;
    snprintf(out, sizeof(out), "%s/info.txt", g_udir);
    spawn(argv, TRUE);
}

static void summarize_log(void)
{
    char *argv[] = { (char *)SHELL_DIR "sg-debug-run", (char *)"--summarize", g_udir, NULL };
    if (g_udir[0]) spawn(argv, TRUE);
}

/* ---- the report ------------------------------------------------------- */

static void build_report(void)
{
    struct text t = { 0 };
    WCHAR path[MAX_PATH], *part, value[256];
    SYSTEMTIME now;

    GetLocalTime(&now);
    add(&t, L"Stained Glass OS problem report\n");
    addf(&t, L"Created:       %04u-%02u-%02u %02u:%02u\n", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute);
    if (g_program[0]) {
        add(&t, L"\n== Program ==\n");
        addf(&t, L"Name:          %s\n", g_title);
        addf(&t, L"File:          %s\n", g_program);
        version_string(g_program, L"ProductName", value, ARRAYSIZE(value));
        if (value[0]) addf(&t, L"Product:       %s\n", value);
        version_string(g_program, L"CompanyName", value, ARRAYSIZE(value));
        if (value[0]) addf(&t, L"Company:       %s\n", value);
        version_string(g_program, L"FileVersion", value, ARRAYSIZE(value));
        if (value[0]) addf(&t, L"Version:       %s\n", value);
        addf(&t, L"Kind:          %s\n", machine_of(g_program));
        if (g_window_title[0]) addf(&t, L"Window:        %s (process %lu)\n", g_window_title, g_pid);
        if (g_args[0]) addf(&t, L"Arguments:     %s\n", g_args);
    }
    if (g_udir[0] && !g_window_title[0] && g_program[0]) {
        if (g_running) add(&t, L"Still running: this report was made before the program ended.\n");
        else {
            _snwprintf(path, ARRAYSIZE(path), L"%s\\seconds", g_dir);
            part = read_text(path, 64);
            if (part) part[wcscspn(part, L"\r\n")] = 0;
            addf(&t, L"Ran for:       %s seconds, exit code %d\n", part ? part : L"?", g_exit_code);
            if (part) HeapFree(GetProcessHeap(), 0, part);
        }
        addf(&t, L"Debug log:     %s\\log\n", g_dir);
    }

    add(&t, L"\n== System ==\n");
    _snwprintf(path, ARRAYSIZE(path), L"%s\\info.txt", g_dir);
    if ((part = read_text(path, 64 * 1024))) { add(&t, part); HeapFree(GetProcessHeap(), 0, part); }
    else add(&t, L"(the system details could not be collected)\n");

    if (g_program[0] && !g_window_title[0]) {
        add(&t, L"\n== Program log highlights ==\n");
        _snwprintf(path, ARRAYSIZE(path), L"%s\\highlights", g_dir);
        if ((part = read_text(path, 96 * 1024))) { add(&t, part); HeapFree(GetProcessHeap(), 0, part); }
        else add(&t, L"(no log yet)\n");
    }
    if (g_report) HeapFree(GetProcessHeap(), 0, g_report);
    g_report = t.s;
    SetWindowTextW(g_details, g_report ? g_report : L"");
}

/* the notes, then the details: what is copied, saved and sent */
static WCHAR *full_report(void)
{
    struct text t = { 0 };
    int n = GetWindowTextLengthW(g_notes);
    WCHAR *notes = HeapAlloc(GetProcessHeap(), 0, (n + 1) * sizeof(WCHAR));

    GetWindowTextW(g_notes, notes, n + 1);
    add(&t, L"== What happened (the tester's words) ==\n");
    add(&t, n ? notes : L"(no notes)");
    add(&t, L"\n\n");
    add(&t, g_report);
    HeapFree(GetProcessHeap(), 0, notes);
    return t.s;
}

static BOOL copy_report(void)
{
    WCHAR *r = full_report();
    size_t bytes = (lstrlenW(r) + 1) * sizeof(WCHAR);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    BOOL ok = FALSE;

    if (mem) {
        memcpy(GlobalLock(mem), r, bytes);
        GlobalUnlock(mem);
        if (OpenClipboard(g_main)) {
            EmptyClipboard();
            ok = SetClipboardData(CF_UNICODETEXT, mem) != NULL;
            CloseClipboard();
        }
        if (!ok) GlobalFree(mem);
    }
    HeapFree(GetProcessHeap(), 0, r);
    return ok;
}

static BOOL write_report(const WCHAR *path)
{
    WCHAR *r = full_report();
    char *u = to_utf8(r);
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD done;
    BOOL ok = f != INVALID_HANDLE_VALUE && WriteFile(f, u, lstrlenA(u), &done, NULL);

    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    HeapFree(GetProcessHeap(), 0, u);
    HeapFree(GetProcessHeap(), 0, r);
    return ok;
}

static void default_name(WCHAR *out, int size)
{
    SYSTEMTIME t;
    WCHAR name[80];
    int i;

    GetLocalTime(&t);
    lstrcpynW(name, g_program[0] ? g_title : L"System", ARRAYSIZE(name));
    for (i = 0; name[i]; i++) if (wcschr(L"\\/:*?\"<>| ", name[i])) name[i] = '-';
    _snwprintf(out, size, L"Problem report - %s - %04u%02u%02u-%02u%02u.txt", name, t.wYear, t.wMonth, t.wDay,
               t.wHour, t.wMinute);
}

static void save_report(void)
{
    WCHAR file[MAX_PATH], desktop[MAX_PATH] = L"";
    OPENFILENAMEW ofn;

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    default_name(file, ARRAYSIZE(file));
    SHGetFolderPathW(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desktop);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = ARRAYSIZE(file);
    ofn.lpstrInitialDir = desktop;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) return;
    SetWindowTextW(g_status, write_report(file) ? L"Saved." : L"The report could not be saved there.");
}

static void url_encode(struct text *t, const WCHAR *s)
{
    char *u = to_utf8(s), *p;
    WCHAR buf[4] = { 0 };
    for (p = u; *p; p++) {
        unsigned char c = *p;
        if (isalnum(c) || strchr("-_.~", c)) { buf[0] = c; buf[1] = 0; }
        else _snwprintf(buf, ARRAYSIZE(buf), L"%%%02X", c);
        /* add() would turn \n into \r\n: these are already escaped */
        add(t, buf);
    }
    HeapFree(GetProcessHeap(), 0, u);
}

/* the tester's own mail program, addressed; the full report on the clipboard
 * and in a file beside the log, since a mailto: link cannot carry all of it */
static void email_report(void)
{
    struct text body = { 0 }, url = { 0 }, subject = { 0 };
    WCHAR file[MAX_PATH], name[MAX_PATH], *r = full_report(), msg[1024];
    BOOL copied = copy_report(), saved;
    INT_PTR rc;
    int keep = lstrlenW(r) > 1500 ? 1500 : lstrlenW(r);

    default_name(name, ARRAYSIZE(name));
    _snwprintf(file, ARRAYSIZE(file), L"%s\\%s", g_dir, name);
    saved = write_report(file);

    addf(&subject, L"Stained Glass OS problem report: %s", g_program[0] ? g_title : L"system");
    r[keep] = 0;
    add(&body, L"(The full report is on the clipboard -- please paste it below this line, or attach ");
    addf(&body, L"\"%s\".)\n\n", name);
    add(&body, r);
    if (keep == 1500) add(&body, L"\n[...]\n");
    add(&url, L"mailto:" REPORT_TO L"?subject=");
    url_encode(&url, subject.s);
    add(&url, L"&body=");
    url_encode(&url, body.s);

    rc = (INT_PTR)ShellExecuteW(g_main, NULL, url.s, NULL, NULL, SW_SHOWNORMAL);
    if (rc > 32) {
        SetWindowTextW(g_status, copied ? L"Your mail program is open. The full report is on the clipboard: paste it into the message."
                                        : L"Your mail program is open.");
    } else {
        _snwprintf(msg, ARRAYSIZE(msg),
                   L"No mail program is set up on this PC.\n\n%s%sSend it to " REPORT_TO L" from any mail service "
                   L"(for example, web mail in a browser).",
                   copied ? L"The report is on the clipboard, ready to paste.\n" : L"",
                   saved ? L"It is also saved in your Reports folder, which opens now.\n\n" : L"\n");
        MessageBoxW(g_main, msg, L"Email report", MB_OK | MB_ICONINFORMATION);
        if (saved) ShellExecuteW(g_main, L"open", g_dir, NULL, NULL, SW_SHOWNORMAL);
    }
    HeapFree(GetProcessHeap(), 0, r);
    HeapFree(GetProcessHeap(), 0, body.s);
    HeapFree(GetProcessHeap(), 0, url.s);
    HeapFree(GetProcessHeap(), 0, subject.s);
}

/* ---- running the program ---------------------------------------------- */

static DWORD WINAPI run_thread(void *arg)
{
    char *argv[64], *prog, *workdir = NULL;
    int argc = 0, n = 0, i;
    WCHAR **list = NULL;
    LONG rc;

    (void)arg;
    argv[argc++] = (char *)SHELL_DIR "sg-debug-run";
    argv[argc++] = g_udir;
    if (g_workdir[0] && p_unix_name && (workdir = p_unix_name(g_workdir))) {
        argv[argc++] = (char *)"--cd";
        argv[argc++] = workdir;
    }
    argv[argc++] = prog = to_utf8(g_program);
    if (g_args[0] && (list = CommandLineToArgvW(g_args, &n)))
        for (i = 0; i < n && argc < (int)ARRAYSIZE(argv) - 1; i++) argv[argc++] = to_utf8(list[i]);
    argv[argc] = NULL;
    rc = p_spawnvp ? p_spawnvp(argv, TRUE) : -1;
    PostMessageW(g_main, WM_RUN_DONE, (WPARAM)rc, 0);
    return 0;
}

/* ---- the window ------------------------------------------------------- */

static void layout(void)
{
    RECT rc;
    int w, h, x = 24, y = BANNER_H + 14, bw = 128, i, notes_h = 96;
    static const int order[5] = { 0, 1, 2, 3, 4 };

    GetClientRect(g_main, &rc);
    w = rc.right - 2 * x;
    h = rc.bottom;
    MoveWindow(g_label_notes, x, y, w, 20, TRUE);
    y += 22;
    MoveWindow(g_notes, x, y, w, notes_h, TRUE);
    y += notes_h + 12;
    MoveWindow(g_label_details, x, y, w, 20, TRUE);
    y += 22;
    MoveWindow(g_details, x, y, w, h - y - 90, TRUE);
    MoveWindow(g_status, x, h - 80, w, 20, TRUE);
    for (i = 4; i >= 0; i--) {
        HWND b = g_buttons[order[i]];
        if (!IsWindowVisible(b)) continue;
        MoveWindow(b, x + w - bw, h - 48, bw, 32, TRUE);
        w -= bw + 8;
    }
}

static HWND make(const WCHAR *cls, const WCHAR *text, DWORD style, int id, HFONT font)
{
    HWND w = CreateWindowExW(!lstrcmpW(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             0, 0, 10, 10, g_main, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(w, WM_SETFONT, (WPARAM)font, FALSE);
    return w;
}

static void paint_banner(HDC dc)
{
    RECT rc, band;
    WCHAR sub[400];

    GetClientRect(g_main, &rc);
    band = rc;
    band.bottom = BANNER_H;
    {
        HBRUSH b = CreateSolidBrush(RGB(0x2A, 0x16, 0x4C));
        FillRect(dc, &band, b);
        DeleteObject(b);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    SelectObject(dc, g_title_font);
    band.left = 24;
    band.top = 8;
    DrawTextW(dc, L"Report a problem", -1, &band, DT_LEFT | DT_TOP | DT_SINGLELINE);
    if (g_program[0]) {
        if (g_running) _snwprintf(sub, ARRAYSIZE(sub), L"%s is running with debug logging. Make the problem happen, then close it.", g_title);
        else _snwprintf(sub, ARRAYSIZE(sub), L"About %s", g_title);
    } else lstrcpynW(sub, L"About this PC", ARRAYSIZE(sub));
    SelectObject(dc, g_font);
    SetTextColor(dc, RGB(0xD8, 0xCC, 0xF0));
    band.top = 38;
    DrawTextW(dc, sub, -1, &band, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        layout();
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 640;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 480;
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_banner(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_white);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == g_details) return DefWindowProcW(hwnd, msg, wp, lp);
        SetBkColor((HDC)wp, RGB(0xFF, 0xFF, 0xFF));
        SetTextColor((HDC)wp, (HWND)lp == g_status ? RGB(0x5A, 0x2D, 0x91) : RGB(0x20, 0x20, 0x20));
        return (LRESULT)g_white;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_COPY:
            SetWindowTextW(g_status, copy_report() ? L"Copied. Paste it into a message, a chat or an issue."
                                                   : L"The report could not be copied.");
            return 0;
        case ID_SAVE: save_report(); return 0;
        case ID_EMAIL: email_report(); return 0;
        case ID_NOW:
            SetWindowTextW(g_status, L"Reading the log so far...");
            summarize_log();
            build_report();
            for (int i = 0; i < 3; i++) EnableWindow(g_buttons[i], TRUE);
            SetWindowTextW(g_status, L"This report shows the log so far. It updates when the program ends.");
            return 0;
        case ID_CLOSE: DestroyWindow(hwnd); return 0;
        }
        break;
    case WM_RUN_DONE:
        g_running = FALSE;
        g_exit_code = (int)wp;
        ShowWindow(g_buttons[4], SW_HIDE);
        EnableWindow(g_buttons[0], TRUE);
        EnableWindow(g_buttons[1], TRUE);
        EnableWindow(g_buttons[2], TRUE);
        layout();
        build_report();
        InvalidateRect(hwnd, NULL, TRUE);
        SetWindowTextW(g_status, L"The program has ended. Add what happened, then copy, save or email the report.");
        SetForegroundWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void parse_command_line(void)
{
    int argc, i;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    for (i = 1; argv && i < argc; i++) {
        if (!lstrcmpiW(argv[i], L"--run") && i + 1 < argc) {
            struct text t = { 0 };
            lstrcpynW(g_program, argv[++i], ARRAYSIZE(g_program));
            for (i++; i < argc; i++) {
                if (t.len) add(&t, L" ");
                if (wcschr(argv[i], ' ')) { add(&t, L"\""); add(&t, argv[i]); add(&t, L"\""); }
                else add(&t, argv[i]);
            }
            if (t.s) lstrcpynW(g_args, t.s, ARRAYSIZE(g_args));
            g_running = TRUE;
        } else if (!lstrcmpiW(argv[i], L"--window") && i + 1 < argc) {
            HWND w = (HWND)(ULONG_PTR)wcstoull(argv[++i], NULL, 0);
            HANDLE proc;
            DWORD size = ARRAYSIZE(g_program);
            GetWindowThreadProcessId(w, &g_pid);
            GetWindowTextW(w, g_window_title, ARRAYSIZE(g_window_title));
            if (!g_window_title[0]) lstrcpyW(g_window_title, L"(untitled)");
            if ((proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_pid))) {
                if (!QueryFullProcessImageNameW(proc, 0, g_program, &size)) g_program[0] = 0;
                CloseHandle(proc);
            }
        }
    }
    if (g_program[0]) {
        resolve_shortcut();
        if (!g_title[0]) {
            WCHAR product[256];
            version_string(g_program, L"FileDescription", product, ARRAYSIZE(product));
            if (product[0]) lstrcpynW(g_title, product, ARRAYSIZE(g_title));
            else {
                lstrcpynW(g_title, PathFindFileNameW(g_program), ARRAYSIZE(g_title));
                PathRemoveExtensionW(g_title);
            }
        }
        if (g_running && !g_workdir[0]) {
            lstrcpynW(g_workdir, g_program, ARRAYSIZE(g_workdir));
            PathRemoveFileSpecW(g_workdir);
        }
    }
    LocalFree(argv);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSW wc = { 0 };
    NONCLIENTMETRICSW ncm;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    MSG msg;
    int i;
    static const WCHAR *labels[5] = { L"Copy report", L"Save...", L"Email report", L"Close", L"Report now" };
    static const int ids[5] = { ID_COPY, ID_SAVE, ID_EMAIL, ID_CLOSE, ID_NOW };

    (void)prev; (void)cmdline;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    g_inst = inst;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    p_spawnvp = (void *)GetProcAddress(ntdll, "__wine_unix_spawnvp");
    p_unix_name = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    parse_command_line();

    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    g_title_font = CreateFontW(-24, 0, 0, 0, FW_LIGHT, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                               ncm.lfMessageFont.lfFaceName);
    g_mono = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                         FIXED_PITCH | FF_MODERN, L"Consolas");
    g_white = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));

    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"SgBugReport";
    RegisterClassW(&wc);
    g_main = CreateWindowExW(0, wc.lpszClassName, L"Report a problem", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 760, 680, NULL, NULL, inst, NULL);

    g_label_notes = make(L"STATIC", L"What happened? What did you do, and what did you expect? (Optional.)", 0, 0, g_font);
    g_notes = make(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP, ID_NOTES, g_font);
    g_label_details = make(L"STATIC", L"Details that go with it -- this is everything the report contains:", 0, 0, g_font);
    g_details = make(L"EDIT", L"Collecting the details...", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_HSCROLL |
                     ES_AUTOHSCROLL | WS_TABSTOP, ID_DETAILS, g_mono);
    SendMessageW(g_details, EM_SETLIMITTEXT, 0, 0);
    g_status = make(L"STATIC", L"", SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0, g_font);
    for (i = 0; i < 5; i++)
        g_buttons[i] = make(L"BUTTON", labels[i], WS_TABSTOP | (i == 2 ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON), ids[i], g_font);
    if (!g_running) ShowWindow(g_buttons[4], SW_HIDE);
    layout();
    ShowWindow(g_main, show ? show : SW_SHOWNORMAL);
    UpdateWindow(g_main);
    SetFocus(g_notes);

    make_report_dir();
    collect_system();
    if (!p_spawnvp || !g_udir[0])
        SetWindowTextW(g_status, L"The details could not be collected here: this needs Stained Glass OS.");
    if (g_running) {
        for (i = 0; i < 3; i++) EnableWindow(g_buttons[i], FALSE);
        build_report();
        SetWindowTextW(g_status, L"Waiting for the program to end. \"Report now\" reports without waiting.");
        CloseHandle(CreateThread(NULL, 0, run_thread, NULL, 0, NULL));
    } else {
        build_report();
        SetWindowTextW(g_status, L"Add what happened, then copy, save or email the report.");
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(g_main, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
