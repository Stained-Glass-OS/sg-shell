/* sg-terminal -- Windows Terminal (wt.exe): tabs of shells on pseudo consoles.
 *
 * Each tab is a program on a pseudo console of its own (CreatePseudoConsole;
 * Wine's conhost turns the console API into VT text on a pipe), read on a
 * thread and fed to the screen (vt.c), which is drawn with a fixed-pitch
 * font. Keys go back as the VT sequences conhost understands. Profiles are
 * found on the machine: PowerShell 7, Command Prompt, Git Bash, Windows
 * PowerShell. wt.exe's command line: -p PROFILE, -d DIR, --title T, a
 * command line, and new-tab (nt) subcommands separated by ';'.
 *
 * Keys: Ctrl+Shift+T new tab, Ctrl+Shift+W close, Ctrl+Tab/Ctrl+Shift+Tab,
 * Ctrl+Alt+1..9 go to a tab, Ctrl+Shift+1..9 open a profile, Ctrl+Shift+C
 * or Ctrl+C with a selection copies, Ctrl+Shift+V or Ctrl+V pastes,
 * Ctrl+= / Ctrl+- / Ctrl+0 zoom, Shift+PgUp/PgDn/Up/Down and the wheel
 * scroll back, F11 or Alt+Enter full screen, Ctrl+, settings.
 *
 * SG_TERMINAL_DUMP=<file>: what the window shows (tabs, their places on
 * screen, the active tab's rows and colours, the cursor, the selection),
 * rewritten when it changes, for the gate.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _WIN32_WINNT 0x0A00
#define NTDDI_VERSION 0x0A000006
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "vt.h"

#define MAX_TABS 32
#define MAX_PROFILES 8
#define WM_PTY_OUTPUT (WM_APP + 1)
#define WM_PTY_EXIT   (WM_APP + 2)
#define TIMER_BLINK 1
#define TIMER_DUMP  2
#define SCROLLBACK 9001
#define REG_KEY L"Software\\Stained Glass\\Terminal"

/* ---- look: our own colour scheme, "Stained Glass Night" --------------------------------- */
static const uint32_t SCHEME[16] = {
    0x2A2635, 0xE0556A, 0x4FC98E, 0xE7C564, 0x5A8DF0, 0xB27CF0, 0x45C6D1, 0xD4CFDF,
    0x6E6880, 0xFF7A8C, 0x72E6AB, 0xFFE08A, 0x82ABFF, 0xCFA0FF, 0x6FE3EC, 0xFFFFFF,
};
#define SCHEME_BG     0x1D1A26
#define SCHEME_FG     0xE8E4F0
#define SCHEME_CURSOR 0xC9A7FF
#define SCHEME_SEL    0x55427E
#define STRIP_BG      RGB(0x12, 0x10, 0x18)
#define STRIP_HOT     RGB(0x2A, 0x26, 0x35)
#define ACCENT        RGB(0x9B, 0x6C, 0xF0)
#define PAD 6

static COLORREF rgb_of(uint32_t v) { return RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff); }

/* ---- profiles ------------------------------------------------------------------------------- */
struct profile { WCHAR name[64], cmd[MAX_PATH + 64], letter; COLORREF colour; };
static struct profile g_profiles[MAX_PROFILES];
static int g_nprofiles, g_default_profile;

static BOOL exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static void add_profile(const WCHAR *name, const WCHAR *cmd, WCHAR letter, COLORREF colour)
{
    struct profile *p;
    if (g_nprofiles >= MAX_PROFILES) return;
    p = &g_profiles[g_nprofiles++];
    lstrcpynW(p->name, name, ARRAYSIZE(p->name));
    lstrcpynW(p->cmd, cmd, ARRAYSIZE(p->cmd));
    p->letter = letter;
    p->colour = colour;
}

static BOOL find_pwsh(WCHAR *out)
{
    WCHAR buf[MAX_PATH], *file;
    DWORD cb = sizeof(buf);
    if (!RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\pwsh.exe", NULL,
                      RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, buf, &cb) && exists(buf)) { lstrcpyW(out, buf); return TRUE; }
    if (SearchPathW(NULL, L"pwsh.exe", NULL, MAX_PATH, buf, &file) && exists(buf)) { lstrcpyW(out, buf); return TRUE; }
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\PowerShell\\7\\pwsh.exe", buf, MAX_PATH);
    if (exists(buf)) { lstrcpyW(out, buf); return TRUE; }
    return FALSE;
}

static void load_profiles(void)
{
    WCHAR path[MAX_PATH], cmd[MAX_PATH + 64], def[64] = L"";
    DWORD cb = sizeof(def);
    int i;
    g_nprofiles = 0;
    if (find_pwsh(path)) { _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\"", path); add_profile(L"PowerShell", cmd, L'P', RGB(0x2F, 0x6F, 0xD0)); }
    if (!GetEnvironmentVariableW(L"ComSpec", path, MAX_PATH) || !exists(path)) lstrcpyW(path, L"cmd.exe");
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\"", path);
    add_profile(L"Command Prompt", cmd, L'C', RGB(0x4A, 0x4A, 0x4A));
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\Git\\bin\\bash.exe", path, MAX_PATH);
    if (exists(path)) { _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" --login -i", path); add_profile(L"Git Bash", cmd, L'G', RGB(0xE0, 0x5A, 0x2B)); }
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", path, MAX_PATH);
    if (exists(path)) { _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\"", path); add_profile(L"Windows PowerShell", cmd, L'W', RGB(0x1B, 0x4F, 0x9A)); }
    g_default_profile = 0;
    RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"DefaultProfile", RRF_RT_REG_SZ, NULL, def, &cb);
    for (i = 0; i < g_nprofiles; i++) if (!lstrcmpiW(g_profiles[i].name, def)) g_default_profile = i;
}

static int profile_by_name(const WCHAR *name)
{
    int i;
    for (i = 0; i < g_nprofiles; i++) if (!lstrcmpiW(g_profiles[i].name, name)) return i;
    /* Windows Terminal's names for them */
    if (!lstrcmpiW(name, L"cmd")) return profile_by_name(L"Command Prompt");
    if (!lstrcmpiW(name, L"pwsh") || !lstrcmpiW(name, L"PowerShell 7")) return profile_by_name(L"PowerShell");
    return -1;
}

/* ---- tabs -------------------------------------------------------------------------------------- */
struct tab {
    int used, id, profile;
    struct vt vt;
    HPCON pc;
    HANDLE in_w, out_r, process, reader;
    BOOL alive;
    DWORD exit_code;
    WCHAR title[160], fixed_title[160], cmd[1024], dir[MAX_PATH];
    int scroll;                     /* lines back into the scrollback, 0: live */
    BOOL sel_active, selecting;
    int sel_ax, sel_ay, sel_bx, sel_by;     /* view coordinates */
};
static struct tab g_tabs[MAX_TABS];
static int g_order[MAX_TABS], g_ntabs, g_active = -1, g_next_id = 1;

static HWND g_wnd, g_view;
static HINSTANCE g_inst;
static HFONT g_font, g_font_bold, g_ui_font, g_ui_small;
static WCHAR g_face[LF_FACESIZE] = L"Consolas";
static int g_font_pt = 12, g_dpi = 96, g_cw = 8, g_ch = 16, g_cols = 120, g_rows = 30;
static int g_tab_h = 40;
static BOOL g_cursor_on = TRUE, g_fullscreen;
static WINDOWPLACEMENT g_saved_place = { sizeof(g_saved_place) };
static int g_hot_tab = -1, g_hot_close = -1, g_hot_btn;    /* 1: +, 2: the menu */
static DWORD g_last_dump_gen;

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static struct tab *active_tab(void) { return g_active >= 0 && g_active < g_ntabs ? &g_tabs[g_order[g_active]] : NULL; }
static struct tab *tab_by_id(int id)
{
    int i;
    for (i = 0; i < MAX_TABS; i++) if (g_tabs[i].used && g_tabs[i].id == id) return &g_tabs[i];
    return NULL;
}

static void pty_send(struct tab *t, const char *s, int n)
{
    DWORD w;
    if (!t || !t->alive || !t->in_w || n <= 0) return;
    WriteFile(t->in_w, s, n, &w, NULL);
}

static void pty_send_w(struct tab *t, const WCHAR *s, int n)
{
    char buf[4096];
    int len = WideCharToMultiByte(CP_UTF8, 0, s, n, buf, sizeof(buf), NULL, NULL);
    pty_send(t, buf, len);
}

struct chunk { int id; DWORD len; char data[1]; };

static DWORD WINAPI reader_thread(void *arg)
{
    struct tab *t = arg;
    int id = t->id;
    HANDLE out = t->out_r;
    char buf[8192];
    DWORD n;
    while (ReadFile(out, buf, sizeof(buf), &n, NULL) && n) {
        struct chunk *c = malloc(sizeof(*c) + n);
        if (!c) continue;
        c->id = id; c->len = n;
        memcpy(c->data, buf, n);
        if (!PostMessageW(g_wnd, WM_PTY_OUTPUT, 0, (LPARAM)c)) free(c);
    }
    return 0;
}

static DWORD WINAPI waiter_thread(void *arg)
{
    struct tab *t = arg;
    int id = t->id;
    WaitForSingleObject(t->process, INFINITE);
    Sleep(150);         /* the last output comes through first */
    PostMessageW(g_wnd, WM_PTY_EXIT, id, 0);
    return 0;
}

static DWORD WINAPI closer_thread(void *arg)
{
    ClosePseudoConsole((HPCON)arg);     /* waits for conhost: not on the window's thread */
    return 0;
}

static void update_title(void);
static void layout_tabs(void);
static void write_dump(BOOL force);

static BOOL spawn(struct tab *t)
{
    HANDLE in_r = NULL, out_w = NULL;
    COORD size = { (SHORT)g_cols, (SHORT)g_rows };
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SIZE_T len = 0;
    WCHAR cmd[1024], session[48];
    HRESULT hr;

    if (!CreatePipe(&in_r, &t->in_w, NULL, 0) || !CreatePipe(&t->out_r, &out_w, NULL, 0)) return FALSE;
    hr = CreatePseudoConsole(size, in_r, out_w, 0, &t->pc);
    CloseHandle(in_r); CloseHandle(out_w);
    if (FAILED(hr)) { CloseHandle(t->in_w); CloseHandle(t->out_r); t->in_w = t->out_r = NULL; return FALSE; }
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.lpTitle = t->title;     /* the console's first title: the profile's name */
    InitializeProcThreadAttributeList(NULL, 1, 0, &len);
    si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, len);
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &len);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, t->pc, sizeof(t->pc), NULL, NULL);
    /* as Windows Terminal does: the shell can tell it runs in one */
    _snwprintf(session, ARRAYSIZE(session), L"{%08lx-%04x-4000-8000-%012llx}", GetCurrentProcessId(), t->id & 0xffff,
               (unsigned long long)GetTickCount64());
    SetEnvironmentVariableW(L"WT_SESSION", session);
    SetEnvironmentVariableW(L"WT_PROFILE_ID", g_profiles[t->profile].name);
    lstrcpynW(cmd, t->cmd, ARRAYSIZE(cmd));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, NULL,
                        t->dir[0] ? t->dir : NULL, &si.StartupInfo, &pi)) {
        char msg[600];
        int n = snprintf(msg, sizeof(msg), "\x1b[91m[error 0x%08lx when launching `", GetLastError());
        n += WideCharToMultiByte(CP_UTF8, 0, t->cmd, -1, msg + n, sizeof(msg) - n - 8, NULL, NULL) - 1;
        n += snprintf(msg + n, sizeof(msg) - n, "']\x1b[m\r\n");
        vt_write(&t->vt, msg, n);
        DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
        CloseHandle(CreateThread(NULL, 0, closer_thread, t->pc, 0, NULL));
        t->pc = NULL;
        CloseHandle(t->in_w); CloseHandle(t->out_r); t->in_w = t->out_r = NULL;
        t->alive = FALSE; t->exit_code = 1;
        return FALSE;
    }
    DeleteProcThreadAttributeList(si.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    CloseHandle(pi.hThread);
    t->process = pi.hProcess;
    t->alive = TRUE;
    t->reader = CreateThread(NULL, 0, reader_thread, t, 0, NULL);
    CloseHandle(CreateThread(NULL, 0, waiter_thread, t, 0, NULL));
    return TRUE;
}

static int new_tab(int profile, const WCHAR *cmd, const WCHAR *dir, const WCHAR *title)
{
    int slot;
    struct tab *t;
    if (g_ntabs >= MAX_TABS) return -1;
    if ((profile < 0 || profile >= g_nprofiles) && cmd && cmd[0]) {
        /* a command line given without a profile: the profile that runs the same program, if one does */
        WCHAR first[MAX_PATH], *base, *dot;
        int i, argc2;
        WCHAR **a = CommandLineToArgvW(cmd, &argc2);
        if (a && argc2) {
            lstrcpynW(first, a[0], MAX_PATH);
            base = wcsrchr(first, L'\\') ? wcsrchr(first, L'\\') + 1 : first;
            if ((dot = wcsrchr(base, L'.')) && !_wcsicmp(dot, L".exe")) *dot = 0;
            for (i = 0; i < g_nprofiles; i++) {
                WCHAR **b = CommandLineToArgvW(g_profiles[i].cmd, &argc2);
                if (b && argc2) {
                    WCHAR *pb = wcsrchr(b[0], L'\\') ? wcsrchr(b[0], L'\\') + 1 : b[0], *pd;
                    if ((pd = wcsrchr(pb, L'.')) && !_wcsicmp(pd, L".exe")) *pd = 0;
                    if (!_wcsicmp(pb, base)) profile = i;
                }
                LocalFree(b);
                if (profile >= 0) break;
            }
        }
        LocalFree(a);
    }
    if (profile < 0 || profile >= g_nprofiles) profile = g_default_profile;
    for (slot = 0; slot < MAX_TABS && g_tabs[slot].used; slot++) ;
    t = &g_tabs[slot];
    memset(t, 0, sizeof(*t));
    t->used = 1;
    t->id = g_next_id++;
    t->profile = profile;
    if (!vt_init(&t->vt, g_cols, g_rows, SCROLLBACK)) { t->used = 0; return -1; }
    lstrcpynW(t->cmd, cmd && cmd[0] ? cmd : g_profiles[profile].cmd, ARRAYSIZE(t->cmd));
    if (dir && dir[0]) lstrcpynW(t->dir, dir, MAX_PATH);
    else SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, t->dir);       /* Windows Terminal starts in %USERPROFILE% */
    if (title && title[0]) lstrcpynW(t->fixed_title, title, ARRAYSIZE(t->fixed_title));
    lstrcpynW(t->title, t->fixed_title[0] ? t->fixed_title : g_profiles[profile].name, ARRAYSIZE(t->title));
    g_order[g_ntabs++] = slot;
    g_active = g_ntabs - 1;
    spawn(t);
    layout_tabs();
    update_title();
    if (g_view) { InvalidateRect(g_view, NULL, FALSE); SetFocus(g_view); }
    InvalidateRect(g_wnd, NULL, FALSE);
    write_dump(TRUE);
    return slot;
}

static void close_tab(int index)
{
    struct tab *t;
    if (index < 0 || index >= g_ntabs) return;
    t = &g_tabs[g_order[index]];
    if (t->alive && t->process) TerminateProcess(t->process, 1);
    if (t->pc) { CloseHandle(CreateThread(NULL, 0, closer_thread, t->pc, 0, NULL)); t->pc = NULL; }
    if (t->in_w) CloseHandle(t->in_w);
    /* the reader ends when the pipe does; its handle is closed by then */
    if (t->reader) CloseHandle(t->reader);
    if (t->process) CloseHandle(t->process);
    vt_free(&t->vt);
    t->used = 0;
    memmove(&g_order[index], &g_order[index + 1], (g_ntabs - index - 1) * sizeof(g_order[0]));
    g_ntabs--;
    if (!g_ntabs) { DestroyWindow(g_wnd); return; }
    if (g_active >= g_ntabs) g_active = g_ntabs - 1;
    else if (g_active > index) g_active--;
    layout_tabs();
    update_title();
    InvalidateRect(g_wnd, NULL, FALSE);
    InvalidateRect(g_view, NULL, FALSE);
    write_dump(TRUE);
}

static void activate(int index)
{
    if (index < 0 || index >= g_ntabs) return;
    g_active = index;
    update_title();
    InvalidateRect(g_wnd, NULL, FALSE);
    InvalidateRect(g_view, NULL, FALSE);
    write_dump(TRUE);
}

static void update_title(void)
{
    struct tab *t = active_tab();
    SetWindowTextW(g_wnd, t ? t->title : L"Terminal");
}

/* ---- fonts and sizes ---------------------------------------------------------------------------- */
static int CALLBACK font_found(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    (void)lf; (void)tm; (void)type;
    *(BOOL *)lp = TRUE;
    return 0;
}

static BOOL have_font(const WCHAR *face)
{
    LOGFONTW lf = { 0 };
    BOOL found = FALSE;
    HDC dc = GetDC(NULL);
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    EnumFontFamiliesExW(dc, &lf, font_found, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

static void make_fonts(void)
{
    static const WCHAR *const faces[] = { L"Cascadia Mono", L"Cascadia Code", L"Consolas", L"DejaVu Sans Mono",
                                          L"Liberation Mono", L"Courier New" };
    WCHAR want[LF_FACESIZE] = L"";
    DWORD cb = sizeof(want), pt = 0;
    HDC dc;
    TEXTMETRICW tm;
    size_t i;
    if (g_font) DeleteObject(g_font);
    if (g_font_bold) DeleteObject(g_font_bold);
    if (!g_face[0] || !lstrcmpW(g_face, L"Consolas")) {
        if (!RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"FontFace", RRF_RT_REG_SZ, NULL, want, &cb) && want[0] && have_font(want))
            lstrcpyW(g_face, want);
        else for (i = 0; i < ARRAYSIZE(faces); i++) if (have_font(faces[i])) { lstrcpyW(g_face, faces[i]); break; }
    }
    cb = sizeof(pt);
    if (!g_font_pt && !RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"FontSize", RRF_RT_REG_DWORD, NULL, &pt, &cb) && pt >= 6 && pt <= 72)
        g_font_pt = pt;
    if (!g_font_pt) g_font_pt = 12;
    g_font = CreateFontW(-MulDiv(g_font_pt, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, g_face);
    g_font_bold = CreateFontW(-MulDiv(g_font_pt, g_dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, g_face);
    dc = GetDC(NULL);
    SelectObject(dc, g_font);
    GetTextMetricsW(dc, &tm);
    {
        SIZE sz;
        GetTextExtentPoint32W(dc, L"M", 1, &sz);
        g_cw = sz.cx > 0 ? sz.cx : tm.tmAveCharWidth;
    }
    g_ch = tm.tmHeight + tm.tmExternalLeading;
    ReleaseDC(NULL, dc);
}

static void resize_grid(void)
{
    RECT r;
    int cols, rows, i, sb = GetSystemMetrics(SM_CXVSCROLL);
    (void)sb;
    GetClientRect(g_view, &r);
    cols = (r.right - 2 * S(PAD)) / g_cw;
    rows = (r.bottom - 2 * S(PAD)) / g_ch;
    if (cols < 10) cols = 10;
    if (rows < 2) rows = 2;
    if (cols == g_cols && rows == g_rows) return;
    g_cols = cols; g_rows = rows;
    for (i = 0; i < g_ntabs; i++) {
        struct tab *t = &g_tabs[g_order[i]];
        COORD c = { (SHORT)cols, (SHORT)rows };
        vt_resize(&t->vt, cols, rows);
#ifndef SG_MUTANT_NORESIZE
        if (t->pc) ResizePseudoConsole(t->pc, c);
#endif
    }
    write_dump(TRUE);
}

/* ---- the tab strip ------------------------------------------------------------------------------- */
static RECT g_tab_rects[MAX_TABS], g_plus_rect, g_menu_rect;

static void layout_tabs(void)
{
    RECT r;
    int i, x = S(8), w, avail;
    GetClientRect(g_wnd, &r);
    avail = r.right - S(8) - S(96);
    w = g_ntabs ? avail / g_ntabs : 0;
    if (w > S(240)) w = S(240);
    if (w < S(60)) w = S(60);
    for (i = 0; i < g_ntabs; i++) { SetRect(&g_tab_rects[i], x, S(6), x + w, g_tab_h); x += w + S(1); }
    SetRect(&g_plus_rect, x + S(4), S(8), x + S(4) + S(36), g_tab_h - S(4));
    SetRect(&g_menu_rect, g_plus_rect.right, S(8), g_plus_rect.right + S(28), g_tab_h - S(4));
}

static RECT close_rect(int i)
{
    RECT c = g_tab_rects[i];
    c.left = c.right - S(30); c.right -= S(6);
    c.top += S(7); c.bottom -= S(7);
    return c;
}

static void draw_glyph_x(HDC dc, RECT r, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, S(1) > 1 ? S(1) : 1, c);
    HGDIOBJ o = SelectObject(dc, p);
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, a = S(4);
    MoveToEx(dc, cx - a, cy - a, NULL); LineTo(dc, cx + a + 1, cy + a + 1);
    MoveToEx(dc, cx + a, cy - a, NULL); LineTo(dc, cx - a - 1, cy + a + 1);
    SelectObject(dc, o); DeleteObject(p);
}

static void paint_strip(HDC dc, RECT *client)
{
    RECT strip = { 0, 0, client->right, g_tab_h };
    HBRUSH b = CreateSolidBrush(STRIP_BG);
    int i;
    FillRect(dc, &strip, b); DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_ui_font);
    for (i = 0; i < g_ntabs; i++) {
        struct tab *t = &g_tabs[g_order[i]];
        RECT r = g_tab_rects[i], txt, ic;
        BOOL act = i == g_active, hot = i == g_hot_tab;
        if (act || hot) {
            b = CreateSolidBrush(act ? rgb_of(SCHEME_BG) : STRIP_HOT);
            FillRect(dc, &r, b); DeleteObject(b);
        }
        if (act) {
            RECT line = { r.left, r.top, r.right, r.top + S(2) };
            b = CreateSolidBrush(ACCENT); FillRect(dc, &line, b); DeleteObject(b);
        }
        /* the profile's badge: a letter on its colour */
        SetRect(&ic, r.left + S(10), (r.top + r.bottom) / 2 - S(8), r.left + S(26), (r.top + r.bottom) / 2 + S(8));
        b = CreateSolidBrush(g_profiles[t->profile].colour); FillRect(dc, &ic, b); DeleteObject(b);
        SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
        SelectObject(dc, g_ui_small);
        DrawTextW(dc, &g_profiles[t->profile].letter, 1, &ic, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, g_ui_font);
        SetRect(&txt, r.left + S(34), r.top, r.right - S(32), r.bottom);
        SetTextColor(dc, act ? RGB(0xFF, 0xFF, 0xFF) : RGB(0xB8, 0xB2, 0xC4));
        DrawTextW(dc, t->title, -1, &txt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (act || hot) {
            RECT c = close_rect(i);
            if (g_hot_close == i) { b = CreateSolidBrush(RGB(0x44, 0x3F, 0x52)); FillRect(dc, &c, b); DeleteObject(b); }
            draw_glyph_x(dc, c, RGB(0xE0, 0xDC, 0xE8));
        }
        if (!t->alive && t->exit_code) {
            RECT dot = { r.right - S(40), (r.top + r.bottom) / 2 - S(3), r.right - S(34), (r.top + r.bottom) / 2 + S(3) };
            b = CreateSolidBrush(RGB(0xE0, 0x55, 0x6A)); FillRect(dc, &dot, b); DeleteObject(b);
        }
    }
    /* + and the drop-down */
    for (i = 1; i <= 2; i++) {
        RECT r = i == 1 ? g_plus_rect : g_menu_rect;
        HPEN p;
        HGDIOBJ o;
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        if (g_hot_btn == i) { b = CreateSolidBrush(STRIP_HOT); FillRect(dc, &r, b); DeleteObject(b); }
        p = CreatePen(PS_SOLID, 1, RGB(0xE0, 0xDC, 0xE8));
        o = SelectObject(dc, p);
        if (i == 1) {
            MoveToEx(dc, cx - S(6), cy, NULL); LineTo(dc, cx + S(7), cy);
            MoveToEx(dc, cx, cy - S(6), NULL); LineTo(dc, cx, cy + S(7));
        } else {
            MoveToEx(dc, cx - S(4), cy - S(2), NULL); LineTo(dc, cx, cy + S(2)); LineTo(dc, cx + S(5), cy - S(3));
        }
        SelectObject(dc, o); DeleteObject(p);
    }
}

/* ---- the terminal view ------------------------------------------------------------------------------ */
static uint32_t colour(uint32_t c, BOOL fg, uint8_t flags)
{
    if (c == VT_DEFAULT) return fg ? SCHEME_FG : SCHEME_BG;
    if (VT_IS_RGB(c)) return c & 0xffffff;
    /* bold text in the first eight colours shows bright, as Windows Terminal does by default */
    if (fg && (flags & VT_BOLD) && c < 8) c += 8;
    return vt_palette_rgb(SCHEME, (int)(c & 0xff));
}

static BOOL in_selection(const struct tab *t, int x, int y)
{
    int ax = t->sel_ax, ay = t->sel_ay, bx = t->sel_bx, by = t->sel_by;
    if (!t->sel_active) return FALSE;
    if (ay > by || (ay == by && ax > bx)) { int tx = ax, ty = ay; ax = bx; ay = by; bx = tx; by = ty; }
    if (y < ay || y > by) return FALSE;
    if (y == ay && x < ax) return FALSE;
    if (y == by && x > bx) return FALSE;
    return TRUE;
}

static void paint_view(HDC dc, RECT *client)
{
    struct tab *t = active_tab();
    HBRUSH b = CreateSolidBrush(rgb_of(SCHEME_BG));
    int y, x;
    FillRect(dc, client, b); DeleteObject(b);
    if (!t) return;
    SetBkMode(dc, OPAQUE);
    for (y = 0; y < t->vt.rows; y++) {
        const struct vt_line *l = vt_view_line(&t->vt, t->scroll, y);
        int py = S(PAD) + y * g_ch;
        if (!l) continue;
        for (x = 0; x < t->vt.cols && x < l->width; ) {
            /* a run of cells drawn alike */
            const struct vt_cell *c0 = &l->cells[x];
            BOOL sel0 = in_selection(t, x, y);
            WCHAR text[1024];
            INT dx[1024];
            int n = 0, x0 = x;
            uint32_t fg, bg;
            RECT rc;
            while (x < t->vt.cols && x < l->width && n < 1000) {
                const struct vt_cell *c = &l->cells[x];
                if (c->fg != c0->fg || c->bg != c0->bg || (c->flags & ~(VT_WIDE | VT_WIDE_TAIL)) != (c0->flags & ~(VT_WIDE | VT_WIDE_TAIL)) ||
                    in_selection(t, x, y) != sel0)
                    break;
                if (c->flags & VT_WIDE_TAIL) { if (n) dx[n - 1] += g_cw; x++; continue; }
                if (c->ch > 0xffff) {
                    uint32_t v = c->ch - 0x10000;
                    text[n] = 0xd800 + (v >> 10); dx[n++] = 0;
                    text[n] = 0xdc00 + (v & 0x3ff); dx[n++] = g_cw;
                } else { text[n] = c->ch ? (WCHAR)c->ch : L' '; dx[n++] = g_cw; }
                x++;
            }
            fg = colour(c0->fg, TRUE, c0->flags);
            bg = colour(c0->bg, FALSE, c0->flags);
            if (c0->flags & VT_REVERSE) { uint32_t s = fg; fg = bg; bg = s; }
            if (sel0) { bg = SCHEME_SEL; fg = 0xFFFFFF; }
            if (c0->flags & VT_DIM) fg = ((fg >> 1) & 0x7f7f7f) + ((bg >> 1) & 0x7f7f7f);
            SelectObject(dc, (c0->flags & VT_BOLD) ? g_font_bold : g_font);
            SetTextColor(dc, rgb_of(fg));
            SetBkColor(dc, rgb_of(bg));
            SetRect(&rc, S(PAD) + x0 * g_cw, py, S(PAD) + x * g_cw, py + g_ch);
            ExtTextOutW(dc, rc.left, py, ETO_OPAQUE | ETO_CLIPPED, &rc, text, n, dx);
            if (c0->flags & (VT_UNDERLINE | VT_STRIKE)) {
                HPEN p = CreatePen(PS_SOLID, 1, rgb_of(fg));
                HGDIOBJ o = SelectObject(dc, p);
                int ly = (c0->flags & VT_UNDERLINE) ? py + g_ch - 2 : py + g_ch / 2;
                MoveToEx(dc, rc.left, ly, NULL); LineTo(dc, rc.right, ly);
                SelectObject(dc, o); DeleteObject(p);
            }
        }
    }
    /* the cursor: a bar, hollow when the window is not in front */
    if (t->vt.cursor_visible && !t->scroll && t->alive) {
        RECT c;
        int cx = S(PAD) + t->vt.cx * g_cw, cy = S(PAD) + t->vt.cy * g_ch;
        HBRUSH cb = CreateSolidBrush(rgb_of(SCHEME_CURSOR));
        if (GetFocus() == g_view) {
            if (g_cursor_on) { SetRect(&c, cx, cy, cx + (S(2) > 1 ? S(2) : 2), cy + g_ch); FillRect(dc, &c, cb); }
        } else { SetRect(&c, cx, cy, cx + g_cw, cy + g_ch); FrameRect(dc, &c, cb); }
        DeleteObject(cb);
    }
}

static void update_scrollbar(void)
{
    struct tab *t = active_tab();
    SCROLLINFO si = { sizeof(si), SIF_ALL | SIF_DISABLENOSCROLL };
    if (!t) return;
    si.nMin = 0;
    si.nMax = t->vt.sb_count + t->vt.rows - 1;
    si.nPage = t->vt.rows;
    si.nPos = t->vt.sb_count - t->scroll;
    SetScrollInfo(g_view, SB_VERT, &si, TRUE);
}

static void scroll_view(struct tab *t, int lines)
{
    int s = t->scroll + lines;
    if (s < 0) s = 0;
    if (s > t->vt.sb_count) s = t->vt.sb_count;
    if (s == t->scroll) return;
    t->scroll = s;
    t->sel_active = FALSE;
    update_scrollbar();
    InvalidateRect(g_view, NULL, FALSE);
    write_dump(TRUE);
}

/* the selection as text: rows joined by CR LF, a soft-wrapped row joined to the next */
static int selection_text(struct tab *t, WCHAR *out, int cap)
{
    int ax = t->sel_ax, ay = t->sel_ay, bx = t->sel_bx, by = t->sel_by, y, n = 0;
    if (!t->sel_active) { out[0] = 0; return 0; }
    if (ay > by || (ay == by && ax > bx)) { int tx = ax, ty = ay; ax = bx; ay = by; bx = tx; by = ty; }
    for (y = ay; y <= by; y++) {
        const struct vt_line *l = vt_view_line(&t->vt, t->scroll, y);
        int x0 = y == ay ? ax : 0, x1 = y == by ? bx : t->vt.cols - 1, x, end;
        if (!l) continue;
        if (x1 >= l->width) x1 = l->width - 1;
        /* trailing blanks of a row are not copied */
        for (end = x1; end >= x0 && (!l->cells[end].ch || l->cells[end].ch == ' '); end--) ;
        for (x = x0; x <= end && n < cap - 4; x++) {
            uint32_t c = l->cells[x].ch;
            if (l->cells[x].flags & VT_WIDE_TAIL) continue;
            if (c > 0xffff) { c -= 0x10000; out[n++] = 0xd800 + (c >> 10); out[n++] = 0xdc00 + (c & 0x3ff); }
            else out[n++] = c ? (WCHAR)c : L' ';
        }
        if (y < by && !l->wrapped && n < cap - 3) { out[n++] = L'\r'; out[n++] = L'\n'; }
    }
    out[n] = 0;
    return n;
}

static void copy_selection(struct tab *t)
{
    static WCHAR buf[65536];
    int n = selection_text(t, buf, ARRAYSIZE(buf));
    HGLOBAL h;
    if (!n || !OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    if ((h = GlobalAlloc(GMEM_MOVEABLE, (n + 1) * sizeof(WCHAR)))) {
        memcpy(GlobalLock(h), buf, (n + 1) * sizeof(WCHAR));
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
    t->sel_active = FALSE;
    InvalidateRect(g_view, NULL, FALSE);
    write_dump(TRUE);
}

static void paste(struct tab *t)
{
    HANDLE h;
    const WCHAR *src;
    WCHAR *buf;
    int n = 0, i;
    if (!t || !OpenClipboard(g_wnd)) return;
    if ((h = GetClipboardData(CF_UNICODETEXT)) && (src = GlobalLock(h))) {
        int len = lstrlenW(src);
        if ((buf = malloc((len + 1) * sizeof(WCHAR)))) {
            /* line breaks are Enter */
            for (i = 0; i < len; i++) {
                if (src[i] == L'\r' && src[i + 1] == L'\n') continue;
                buf[n++] = src[i] == L'\n' ? L'\r' : src[i];
            }
            for (i = 0; i < n; i += 1024) pty_send_w(t, buf + i, n - i > 1024 ? 1024 : n - i);
            free(buf);
        }
        GlobalUnlock(h);
    }
    CloseClipboard();
    t->scroll = 0;
}

static void cell_at(int px, int py, int *x, int *y)
{
    struct tab *t = active_tab();
    *x = (px - S(PAD)) / g_cw;
    *y = (py - S(PAD)) / g_ch;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (t && *x >= t->vt.cols) *x = t->vt.cols - 1;
    if (t && *y >= t->vt.rows) *y = t->vt.rows - 1;
}

static BOOL word_char(uint32_t c) { return c && c != ' ' && !wcschr(L"\"'()[]{}<>,;|`", (WCHAR)c); }

static const char *key_sequence(WPARAM vk, BOOL app_keys, char *buf)
{
    int mod = 1 + (GetKeyState(VK_SHIFT) < 0 ? 1 : 0) + (GetKeyState(VK_MENU) < 0 ? 2 : 0) + (GetKeyState(VK_CONTROL) < 0 ? 4 : 0);
    char letter = 0;
    int num = 0;
    switch (vk) {
    case VK_UP: letter = 'A'; break;
    case VK_DOWN: letter = 'B'; break;
    case VK_RIGHT: letter = 'C'; break;
    case VK_LEFT: letter = 'D'; break;
    case VK_HOME: letter = 'H'; break;
    case VK_END: letter = 'F'; break;
    case VK_F1: letter = 'P'; break;
    case VK_F2: letter = 'Q'; break;
    case VK_F3: letter = 'R'; break;
    case VK_F4: letter = 'S'; break;
    case VK_INSERT: num = 2; break;
    case VK_DELETE: num = 3; break;
    case VK_PRIOR: num = 5; break;
    case VK_NEXT: num = 6; break;
    case VK_F5: num = 15; break;
    case VK_F6: num = 17; break;
    case VK_F7: num = 18; break;
    case VK_F8: num = 19; break;
    case VK_F9: num = 20; break;
    case VK_F10: num = 21; break;
    case VK_F11: num = 23; break;
    case VK_F12: num = 24; break;
    default: return NULL;
    }
    if (letter) {
        if (mod > 1) sprintf(buf, "\x1b[1;%d%c", mod, letter);
        else if (letter >= 'P') sprintf(buf, "\x1bO%c", letter);
        else sprintf(buf, app_keys ? "\x1bO%c" : "\x1b[%c", letter);
    } else if (mod > 1) sprintf(buf, "\x1b[%d;%d~", num, mod);
    else sprintf(buf, "\x1b[%d~", num);
    return buf;
}

static void zoom(int delta)
{
    if (delta == 0) g_font_pt = 12;
    else g_font_pt += delta;
    if (g_font_pt < 6) g_font_pt = 6;
    if (g_font_pt > 48) g_font_pt = 48;
    make_fonts();
    g_cols = g_rows = 0;
    resize_grid();
    InvalidateRect(g_view, NULL, FALSE);
}

static void toggle_fullscreen(void)
{
    if (!g_fullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        GetWindowPlacement(g_wnd, &g_saved_place);
        GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(g_wnd, GWL_STYLE, GetWindowLongW(g_wnd, GWL_STYLE) & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(g_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        g_fullscreen = TRUE;
    } else {
        SetWindowLongW(g_wnd, GWL_STYLE, GetWindowLongW(g_wnd, GWL_STYLE) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_wnd, &g_saved_place);
        SetWindowPos(g_wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        g_fullscreen = FALSE;
    }
}

static void show_menu(void);
static void show_settings(void);

/* shortcuts that are the terminal's, not the shell's */
static BOOL shortcut(WPARAM vk)
{
    BOOL ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    struct tab *t = active_tab();
    if (ctrl && shift && !alt) {
        if (vk == 'T') { new_tab(g_default_profile, NULL, NULL, NULL); return TRUE; }
        if (vk == 'W') { close_tab(g_active); return TRUE; }
        if (vk == 'C') { if (t) copy_selection(t); return TRUE; }
        if (vk == 'V') { paste(t); return TRUE; }
        if (vk == VK_TAB) { activate((g_active + g_ntabs - 1) % g_ntabs); return TRUE; }
        if (vk == VK_SPACE) { show_menu(); return TRUE; }
        if (vk >= '1' && vk <= '9') { if ((int)(vk - '1') < g_nprofiles) new_tab(vk - '1', NULL, NULL, NULL); return TRUE; }
    }
    if (ctrl && !shift && !alt) {
        if (vk == VK_TAB) { activate((g_active + 1) % g_ntabs); return TRUE; }
        if (vk == 'C' && t && t->sel_active) { copy_selection(t); return TRUE; }
        if (vk == 'V') { paste(t); return TRUE; }
        if (vk == VK_OEM_PLUS || vk == VK_ADD) { zoom(1); return TRUE; }
        if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) { zoom(-1); return TRUE; }
        if (vk == '0' || vk == VK_NUMPAD0) { zoom(0); return TRUE; }
        if (vk == VK_OEM_COMMA) { show_settings(); return TRUE; }
    }
    if (ctrl && alt && !shift && vk >= '1' && vk <= '9') { activate(vk - '1'); return TRUE; }
    if (shift && !ctrl && !alt && t) {
        if (vk == VK_PRIOR) { scroll_view(t, t->vt.rows - 1); return TRUE; }
        if (vk == VK_NEXT) { scroll_view(t, -(t->vt.rows - 1)); return TRUE; }
    }
    if (ctrl && shift && t && (vk == VK_UP || vk == VK_DOWN)) { scroll_view(t, vk == VK_UP ? 1 : -1); return TRUE; }
    if (vk == VK_F11 && !ctrl && !alt && !shift) { toggle_fullscreen(); return TRUE; }
    if (vk == VK_RETURN && alt) { toggle_fullscreen(); return TRUE; }
    return FALSE;
}

static LRESULT CALLBACK view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct tab *t = active_tab();
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps), mem;
        RECT r;
        HBITMAP bmp;
        HGDIOBJ old;
        GetClientRect(hwnd, &r);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
        old = SelectObject(mem, bmp);
        paint_view(mem, &r);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE: resize_grid(); update_scrollbar(); InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_SETFOCUS: case WM_KILLFOCUS: g_cursor_on = TRUE; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        char buf[32];
        const char *seq;
        if (shortcut(wp)) {
            /* the key was the terminal's: its character must not reach the shell too */
            MSG m;
            while (PeekMessageW(&m, hwnd, WM_CHAR, WM_CHAR, PM_REMOVE)) ;
            return 0;
        }
        if (!t) return 0;
        if (!t->alive) {
            /* the process has ended: Enter starts it again, Ctrl+D closes the tab */
            if (wp == VK_RETURN) { vt_write(&t->vt, "\r\n", 2); spawn(t); InvalidateRect(g_wnd, NULL, FALSE); }
            else if (wp == 'D' && GetKeyState(VK_CONTROL) < 0) close_tab(g_active);
            return 0;
        }
        if ((seq = key_sequence(wp, t->vt.app_cursor_keys, buf))) {
            t->scroll = 0; t->sel_active = FALSE;
            pty_send(t, seq, (int)strlen(seq));
            return 0;
        }
        if (msg == WM_SYSKEYDOWN && wp != VK_F4 && wp != VK_MENU) {
            /* Alt+key: ESC then the key, as terminals send it */
            BYTE state[256];
            WCHAR ch[4];
            GetKeyboardState(state);
            state[VK_MENU] = state[VK_LMENU] = state[VK_RMENU] = 0;
            if (ToUnicode((UINT)wp, (lp >> 16) & 0xff, state, ch, 4, 0) == 1) {
                char out[8];
                int n = WideCharToMultiByte(CP_UTF8, 0, ch, 1, out + 1, 6, NULL, NULL);
                out[0] = 0x1b;
                pty_send(t, out, n + 1);
                return 0;
            }
        }
        break;
    }
    case WM_CHAR: {
        WCHAR c = (WCHAR)wp;
        if (!t || !t->alive) return 0;
        t->scroll = 0;
        t->sel_active = FALSE;
        if (c == 8) { pty_send(t, "\x7f", 1); return 0; }     /* Backspace is DEL, as on a VT */
        if (c == L'\r') { pty_send(t, "\r", 1); return 0; }
        pty_send_w(t, &c, 1);
        return 0;
    }
    case WM_SYSCHAR: return 0;
    case WM_MOUSEWHEEL:
        if (GetKeyState(VK_CONTROL) < 0) { zoom(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1); return 0; }
        if (t) scroll_view(t, GET_WHEEL_DELTA_WPARAM(wp) * 3 / WHEEL_DELTA);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        if (!t) return 0;
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (LOWORD(wp)) {
        case SB_LINEUP: scroll_view(t, 1); break;
        case SB_LINEDOWN: scroll_view(t, -1); break;
        case SB_PAGEUP: scroll_view(t, t->vt.rows - 1); break;
        case SB_PAGEDOWN: scroll_view(t, -(t->vt.rows - 1)); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: scroll_view(t, (t->vt.sb_count - si.nTrackPos) - t->scroll); break;
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        if (t) {
            int x, y;
            cell_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &x, &y);
            t->sel_ax = t->sel_bx = x; t->sel_ay = t->sel_by = y;
            t->sel_active = FALSE; t->selecting = TRUE;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (t && t->selecting) {
            int x, y;
            cell_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &x, &y);
            if (x != t->sel_bx || y != t->sel_by) { t->sel_bx = x; t->sel_by = y; t->sel_active = TRUE; InvalidateRect(hwnd, NULL, FALSE); }
        }
        return 0;
    case WM_LBUTTONUP:
        if (t && t->selecting) { t->selecting = FALSE; ReleaseCapture(); write_dump(TRUE); }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (t) {
            /* a word */
            int x, y, a, b;
            const struct vt_line *l;
            cell_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &x, &y);
            if (!(l = vt_view_line(&t->vt, t->scroll, y)) || !word_char(l->cells[x].ch)) return 0;
            for (a = x; a > 0 && word_char(l->cells[a - 1].ch); a--) ;
            for (b = x; b + 1 < t->vt.cols && word_char(l->cells[b + 1].ch); b++) ;
            t->sel_ax = a; t->sel_bx = b; t->sel_ay = t->sel_by = y; t->sel_active = TRUE;
            InvalidateRect(hwnd, NULL, FALSE);
            write_dump(TRUE);
        }
        return 0;
    case WM_RBUTTONUP:
        /* right-click: copy the selection, or paste */
        if (t) { if (t->sel_active) copy_selection(t); else paste(t); }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the drop-down menu and settings ------------------------------------------------------------------- */
enum { M_PROFILE = 100, M_SETTINGS = 200, M_ABOUT, M_COMMANDS };

static void show_menu(void)
{
    HMENU m = CreatePopupMenu();
    POINT pt = { g_menu_rect.left, g_menu_rect.bottom };
    int i, cmd;
    for (i = 0; i < g_nprofiles; i++) {
        WCHAR label[96];
        _snwprintf(label, ARRAYSIZE(label), L"%ls\tCtrl+Shift+%d", g_profiles[i].name, i + 1);
        AppendMenuW(m, MF_STRING, M_PROFILE + i, label);
    }
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, M_SETTINGS, L"Settings\tCtrl+,");
    AppendMenuW(m, MF_STRING, M_ABOUT, L"About");
    ClientToScreen(g_wnd, &pt);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g_wnd, NULL);
    DestroyMenu(m);
    if (cmd >= M_PROFILE && cmd < M_PROFILE + g_nprofiles) new_tab(cmd - M_PROFILE, NULL, NULL, NULL);
    else if (cmd == M_SETTINGS) show_settings();
    else if (cmd == M_ABOUT)
        MessageBoxW(g_wnd, L"Terminal\n\nTabs of shells on pseudo consoles, for Stained Glass OS.\n"
                    L"Free software under the AGPL.", L"About", MB_OK | MB_ICONINFORMATION);
}

/* Settings: the default profile, the font and its size -- kept in HKCU\Software\Stained Glass\Terminal */
static HWND g_set_dlg;
static LRESULT CALLBACK settings_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            WCHAR face[LF_FACESIZE], size[8];
            int p = (int)SendDlgItemMessageW(hwnd, 10, CB_GETCURSEL, 0, 0), pt;
            GetDlgItemTextW(hwnd, 11, face, LF_FACESIZE);
            GetDlgItemTextW(hwnd, 12, size, 8);
            pt = _wtoi(size);
            if (p >= 0 && p < g_nprofiles) {
                HKEY k;
                if (!RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) {
                    DWORD d = pt;
                    RegSetValueExW(k, L"DefaultProfile", 0, REG_SZ, (BYTE *)g_profiles[p].name, (lstrlenW(g_profiles[p].name) + 1) * sizeof(WCHAR));
                    RegSetValueExW(k, L"FontFace", 0, REG_SZ, (BYTE *)face, (lstrlenW(face) + 1) * sizeof(WCHAR));
                    if (pt >= 6 && pt <= 72) RegSetValueExW(k, L"FontSize", 0, REG_DWORD, (BYTE *)&d, sizeof(d));
                    RegCloseKey(k);
                }
                g_default_profile = p;
            }
            if (face[0] && have_font(face)) lstrcpynW(g_face, face, LF_FACESIZE);
            if (pt >= 6 && pt <= 72) g_font_pt = pt;
            make_fonts();
            g_cols = g_rows = 0;
            resize_grid();
            InvalidateRect(g_view, NULL, FALSE);
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: g_set_dlg = NULL; EnableWindow(g_wnd, TRUE); SetForegroundWindow(g_wnd); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void show_settings(void)
{
    WNDCLASSW wc = { 0 };
    HWND c;
    WCHAR size[8];
    int i, y = S(16), x = S(16), w = S(360);
    if (g_set_dlg) { SetForegroundWindow(g_set_dlg); return; }
    wc.lpfnWndProc = settings_proc; wc.hInstance = g_inst; wc.lpszClassName = L"SgTerminalSettings";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1); wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    g_set_dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"SgTerminalSettings", L"Settings",
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, S(400), S(290), g_wnd, NULL, g_inst, NULL);
#define ADD(cls, txt, style, X, Y, W, H, id) do { c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | (style), X, Y, W, H, g_set_dlg, (HMENU)(INT_PTR)(id), g_inst, NULL); \
        SendMessageW(c, WM_SETFONT, (WPARAM)g_ui_font, TRUE); } while (0)
    ADD(L"STATIC", L"Default profile", 0, x, y, w, S(20), -1); y += S(22);
    ADD(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, S(200), 10); y += S(36);
    for (i = 0; i < g_nprofiles; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)g_profiles[i].name);
    SendMessageW(c, CB_SETCURSEL, g_default_profile, 0);
    ADD(L"STATIC", L"Font face", 0, x, y, w, S(20), -1); y += S(22);
    ADD(L"EDIT", g_face, WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, x, y, w, S(26), 11); y += S(36);
    ADD(L"STATIC", L"Font size", 0, x, y, w, S(20), -1); y += S(22);
    _snwprintf(size, ARRAYSIZE(size), L"%d", g_font_pt);
    ADD(L"EDIT", size, WS_TABSTOP | WS_BORDER | ES_NUMBER, x, y, S(80), S(26), 12); y += S(44);
    ADD(L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, x + w - S(200), y, S(96), S(30), IDOK);
    ADD(L"BUTTON", L"Cancel", WS_TABSTOP, x + w - S(96), y, S(96), S(30), IDCANCEL);
#undef ADD
    EnableWindow(g_wnd, FALSE);
    ShowWindow(g_set_dlg, SW_SHOW);
}

/* ---- the dump, for the gate ------------------------------------------------------------------------------- */
static void write_dump(BOOL force)
{
    static WCHAR path[MAX_PATH];
    static int have = -1;
    WCHAR tmp[MAX_PATH + 8];
    FILE *f;
    struct tab *t = active_tab();
    RECT wr;
    int i, y;
    char line[4096];
    if (have < 0) have = GetEnvironmentVariableW(L"SG_TERMINAL_DUMP", path, MAX_PATH) > 0;
    if (!have || !g_wnd) return;
    if (!force && t && t->vt.generation == g_last_dump_gen) return;
    g_last_dump_gen = t ? (DWORD)t->vt.generation : 0;
    _snwprintf(tmp, ARRAYSIZE(tmp), L"%ls.tmp", path);
    if (!(f = _wfopen(tmp, L"wb"))) return;
    GetWindowRect(g_wnd, &wr);
    {
        WCHAR title[256];
        GetWindowTextW(g_wnd, title, ARRAYSIZE(title));
        WideCharToMultiByte(CP_UTF8, 0, title, -1, line, sizeof(line), NULL, NULL);
        fprintf(f, "window %s\nrect %ld %ld %ld %ld\nsize %d %d\n", line, wr.left, wr.top, wr.right, wr.bottom, g_cols, g_rows);
        WideCharToMultiByte(CP_UTF8, 0, g_face, -1, line, sizeof(line), NULL, NULL);
        fprintf(f, "font %d %d %d %s\n", g_font_pt, g_cw, g_ch, line);
    }
    for (i = 0; i < g_nprofiles; i++) {
        WideCharToMultiByte(CP_UTF8, 0, g_profiles[i].name, -1, line, sizeof(line), NULL, NULL);
        fprintf(f, "profile %d %s%s\n", i + 1, line, i == g_default_profile ? " (default)" : "");
    }
    fprintf(f, "tabs %d active %d\n", g_ntabs, g_active + 1);
    for (i = 0; i < g_ntabs; i++) {
        struct tab *u = &g_tabs[g_order[i]];
        POINT c = { (g_tab_rects[i].left + g_tab_rects[i].right) / 2, (g_tab_rects[i].top + g_tab_rects[i].bottom) / 2 };
        char title[512], prof[128];
        ClientToScreen(g_wnd, &c);
        WideCharToMultiByte(CP_UTF8, 0, u->title, -1, title, sizeof(title), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, g_profiles[u->profile].name, -1, prof, sizeof(prof), NULL, NULL);
        fprintf(f, "tab %d at=%ld,%ld profile=%s alive=%d exit=%lu: %s\n", i + 1, c.x, c.y, prof, u->alive, u->exit_code, title);
    }
    {
        POINT a = { (g_plus_rect.left + g_plus_rect.right) / 2, (g_plus_rect.top + g_plus_rect.bottom) / 2 };
        POINT b = { (g_menu_rect.left + g_menu_rect.right) / 2, (g_menu_rect.top + g_menu_rect.bottom) / 2 };
        POINT v = { S(PAD), S(PAD) };
        ClientToScreen(g_wnd, &a); ClientToScreen(g_wnd, &b); ClientToScreen(g_view, &v);
        fprintf(f, "plus %ld %ld\nmenu %ld %ld\norigin %ld %ld\n", a.x, a.y, b.x, b.y, v.x, v.y);
    }
    if (t) {
        static WCHAR sel[8192];
        fprintf(f, "cursor %d %d %d\nscroll %d %d\n", t->vt.cx, t->vt.cy, t->vt.cursor_visible, t->scroll, t->vt.sb_count);
        for (y = 0; y < t->vt.rows; y++) {
            const struct vt_line *l = vt_view_line(&t->vt, t->scroll, y);
            int x, n;
            vt_line_text(l, t->vt.cols, line, sizeof(line));
            fprintf(f, "row %d: %s\n", y, line);
            /* the foreground of each cell: 0-9a-f a colour, - the default, * RGB or 256 */
            if (!l) continue;
            for (n = x = 0; x < t->vt.cols && x < l->width; x++) {
                uint32_t c = l->cells[x].fg;
                line[n++] = c == VT_DEFAULT ? '-' : (c < 16 ? "0123456789abcdef"[c] : '*');
            }
            while (n && line[n - 1] == '-') n--;
            line[n] = 0;
            if (n) fprintf(f, "fg %d: %s\n", y, line);
        }
        selection_text(t, sel, ARRAYSIZE(sel));
        WideCharToMultiByte(CP_UTF8, 0, sel, -1, line, sizeof(line), NULL, NULL);
        fprintf(f, "selection %s\n", line);
    }
    fclose(f);
    MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

/* ---- the window ---------------------------------------------------------------------------------------- */
static int tab_at(int x, int y, BOOL *on_close)
{
    POINT pt = { x, y };
    int i;
    *on_close = FALSE;
    for (i = 0; i < g_ntabs; i++)
        if (PtInRect(&g_tab_rects[i], pt)) { RECT c = close_rect(i); *on_close = PtInRect(&c, pt); return i; }
    return -1;
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: return 0;
    case WM_SIZE: {
        RECT r;
        GetClientRect(hwnd, &r);
        layout_tabs();
        if (g_view) MoveWindow(g_view, 0, g_tab_h, r.right, r.bottom - g_tab_h > 0 ? r.bottom - g_tab_h : 0, TRUE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps), mem;
        RECT r;
        HBITMAP bmp;
        HGDIOBJ old;
        GetClientRect(hwnd, &r);
        r.bottom = g_tab_h;
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
        old = SelectObject(mem, bmp);
        paint_strip(mem, &r);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        BOOL on_close;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int t = tab_at(pt.x, pt.y, &on_close), btn = PtInRect(&g_plus_rect, pt) ? 1 : PtInRect(&g_menu_rect, pt) ? 2 : 0;
        if (t != g_hot_tab || (on_close ? t : -1) != g_hot_close || btn != g_hot_btn) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            g_hot_tab = t; g_hot_close = on_close ? t : -1; g_hot_btn = btn;
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE: g_hot_tab = g_hot_close = -1; g_hot_btn = 0; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONUP: {
        BOOL on_close;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int t = tab_at(pt.x, pt.y, &on_close);
        if (t >= 0) { if (on_close) close_tab(t); else activate(t); }
        else if (PtInRect(&g_plus_rect, pt)) new_tab(g_default_profile, NULL, NULL, NULL);
        else if (PtInRect(&g_menu_rect, pt)) show_menu();
        if (g_view && IsWindow(g_view)) SetFocus(g_view);
        return 0;
    }
    case WM_MBUTTONUP: {
        BOOL on_close;
        int t = tab_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &on_close);
        if (t >= 0) close_tab(t);
        return 0;
    }
    case WM_SETFOCUS: if (g_view) SetFocus(g_view); return 0;
    case WM_ACTIVATE: if (LOWORD(wp) != WA_INACTIVE && g_view) SetFocus(g_view); return 0;
    case WM_PTY_OUTPUT: {
        struct chunk *c = (struct chunk *)lp;
        struct tab *t = tab_by_id(c->id);
        if (t) {
            vt_write(&t->vt, c->data, c->len);
            if (t->vt.reply_len) { pty_send(t, t->vt.reply, t->vt.reply_len); t->vt.reply_len = 0; }
            if (t->vt.title_changed) {
                t->vt.title_changed = 0;
                if (!t->fixed_title[0]) {
                    int len;
                    MultiByteToWideChar(CP_UTF8, 0, t->vt.title, -1, t->title, ARRAYSIZE(t->title));
                    /* a shell that names only a prefix ("Administrator: " and an empty console
                     * title) gets the profile's name after it */
                    len = lstrlenW(t->title);
                    if (!len || (len >= 2 && t->title[len - 2] == L':' && t->title[len - 1] == L' '))
                        wcsncat(t->title, g_profiles[t->profile].name, ARRAYSIZE(t->title) - len - 1);
                    if (t == active_tab()) update_title();
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            if (t->vt.bell) { t->vt.bell = 0; MessageBeep(MB_OK); }
            if (t == active_tab()) {
                t->sel_active = t->sel_active && t->selecting;
                InvalidateRect(g_view, NULL, FALSE);
                update_scrollbar();
            }
        }
        free(c);
        return 0;
    }
    case WM_PTY_EXIT: {
        struct tab *t = tab_by_id((int)wp);
        int i;
        if (!t || !t->alive) return 0;
        GetExitCodeProcess(t->process, &t->exit_code);
        t->alive = FALSE;
        if (t->pc) { CloseHandle(CreateThread(NULL, 0, closer_thread, t->pc, 0, NULL)); t->pc = NULL; }
        if (t->in_w) { CloseHandle(t->in_w); t->in_w = NULL; }
        CloseHandle(t->process); t->process = NULL;
        for (i = 0; i < g_ntabs; i++) if (&g_tabs[g_order[i]] == t) break;
        if (!t->exit_code) { close_tab(i); return 0; }     /* a graceful exit closes the tab, as Windows Terminal's default */
        {
            char msg[160];
            int n = snprintf(msg, sizeof(msg), "\r\n\x1b[m[process exited with code %lu (0x%08lx)]\r\n"
                             "You can now close this terminal with Ctrl+D, or press Enter to restart.\r\n",
                             t->exit_code, t->exit_code);
            vt_write(&t->vt, msg, n);
        }
        InvalidateRect(g_view, NULL, FALSE);
        InvalidateRect(hwnd, NULL, FALSE);
        write_dump(TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_BLINK) { g_cursor_on = !g_cursor_on; InvalidateRect(g_view, NULL, FALSE); }
        else if (wp == TIMER_DUMP) write_dump(FALSE);
        return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO *mm = (MINMAXINFO *)lp; mm->ptMinTrackSize.x = S(360); mm->ptMinTrackSize.y = S(200); return 0; }
    case WM_CLOSE:
        if (g_ntabs > 1 && !GetEnvironmentVariableW(L"SG_TERMINAL_DUMP", NULL, 0) &&
            MessageBoxW(hwnd, L"Do you want to close all tabs?", L"Close all tabs?", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        int i;
        for (i = 0; i < g_ntabs; i++) { struct tab *t = &g_tabs[g_order[i]]; if (t->alive && t->process) TerminateProcess(t->process, 1); }
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- wt.exe's command line ------------------------------------------------------------------------------- */
struct tab_request { int profile; WCHAR cmd[1024], dir[MAX_PATH], title[160]; };

static void append_arg(WCHAR *cmd, int cch, const WCHAR *a)
{
    BOOL quote = !a[0] || wcspbrk(a, L" \t\"");
    int n = lstrlenW(cmd);
    if (n && n < cch - 1) cmd[n++] = L' ';
    if (!quote) { lstrcpynW(cmd + n, a, cch - n); return; }
    if (n < cch - 1) cmd[n++] = L'"';
    for (; *a && n < cch - 3; a++) { if (*a == L'"') cmd[n++] = L'\\'; cmd[n++] = *a; }
    cmd[n++] = L'"';
    cmd[n] = 0;
}

static int parse_command_line(int argc, WCHAR **argv, struct tab_request *req, int max, BOOL *maximized, BOOL *full)
{
    int n = 0, i = 1;
    while (i <= argc && n < max) {
        struct tab_request *r = &req[n];
        memset(r, 0, sizeof(*r));
        r->profile = -1;
        if (i < argc && (!lstrcmpiW(argv[i], L"new-tab") || !lstrcmpiW(argv[i], L"nt") ||
                         !lstrcmpiW(argv[i], L"split-pane") || !lstrcmpiW(argv[i], L"sp"))) i++;
        for (; i < argc; i++) {
            const WCHAR *a = argv[i];
            if (!lstrcmpW(a, L";")) break;
            if ((!lstrcmpW(a, L"-p") || !lstrcmpiW(a, L"--profile")) && i + 1 < argc) { r->profile = profile_by_name(argv[++i]); continue; }
            if ((!lstrcmpW(a, L"-d") || !lstrcmpiW(a, L"--startingDirectory")) && i + 1 < argc) { lstrcpynW(r->dir, argv[++i], MAX_PATH); continue; }
            if (!lstrcmpiW(a, L"--title") && i + 1 < argc) { lstrcpynW(r->title, argv[++i], ARRAYSIZE(r->title)); continue; }
            if ((!lstrcmpW(a, L"-w") || !lstrcmpiW(a, L"--window")) && i + 1 < argc) { i++; continue; }
            if (!lstrcmpW(a, L"-M") || !lstrcmpiW(a, L"--maximized")) { *maximized = TRUE; continue; }
            if (!lstrcmpW(a, L"-F") || !lstrcmpiW(a, L"--fullscreen")) { *full = TRUE; continue; }
            if (!lstrcmpiW(a, L"--focus") || !lstrcmpW(a, L"-f") || !lstrcmpiW(a, L"-H") || !lstrcmpiW(a, L"-V")) continue;
            /* the rest, up to a lone ';', is the command line */
            for (; i < argc && lstrcmpW(argv[i], L";"); i++) {
                WCHAR arg[1024];
                size_t len;
                lstrcpynW(arg, argv[i], ARRAYSIZE(arg));
                len = wcslen(arg);
                /* "cmd ; nt" typed without spaces round the ';' */
                if (len > 1 && arg[len - 1] == L';') { arg[len - 1] = 0; append_arg(r->cmd, ARRAYSIZE(r->cmd), arg); i++; goto next; }
                append_arg(r->cmd, ARRAYSIZE(r->cmd), arg);
            }
            break;
        }
        if (i < argc && !lstrcmpW(argv[i], L";")) i++;
next:
        n++;
        if (i >= argc) break;
    }
    return n;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    WNDCLASSW wc = { 0 };
    int argc = 0, i, n;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    struct tab_request req[16];
    BOOL maximized = FALSE, full = FALSE;
    MSG msg;
    HDC dc;
    (void)prev; (void)cmdline;

    g_inst = inst;
    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    if (g_dpi < 96) g_dpi = 96;
    g_tab_h = S(40);
    g_font_pt = 0;
    load_profiles();
    make_fonts();
    g_ui_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_ui_small = CreateFontW(-MulDiv(7, g_dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    n = parse_command_line(argc, argv, req, ARRAYSIZE(req), &maximized, &full);

    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_IBEAM);
    wc.lpfnWndProc = view_proc; wc.lpszClassName = L"SgTerminalView"; wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); wc.style = 0;
    wc.lpfnWndProc = main_proc; wc.lpszClassName = L"SgTerminalWindow";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);

    {
        /* 120 x 30 cells, as Windows Terminal opens, within the work area */
        RECT work, r = { 0, 0, 120 * g_cw + 2 * S(PAD) + GetSystemMetrics(SM_CXVSCROLL), 30 * g_ch + 2 * S(PAD) + g_tab_h };
        AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        if (r.right - r.left > (work.right - work.left) * 95 / 100) r.right = r.left + (work.right - work.left) * 95 / 100;
        if (r.bottom - r.top > (work.bottom - work.top) * 95 / 100) r.bottom = r.top + (work.bottom - work.top) * 95 / 100;
        g_wnd = CreateWindowExW(WS_EX_APPWINDOW, L"SgTerminalWindow", L"Terminal", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    }
    if (!g_wnd) return 1;
    g_view = CreateWindowExW(0, L"SgTerminalView", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 0, 0, g_wnd, NULL, inst, NULL);
    {
        RECT r;
        GetClientRect(g_wnd, &r);
        MoveWindow(g_view, 0, g_tab_h, r.right, r.bottom - g_tab_h, FALSE);
        g_cols = g_rows = 0;
        resize_grid();
    }
    for (i = 0; i < n; i++) new_tab(req[i].profile, req[i].cmd, req[i].dir, req[i].title);
    if (!g_ntabs) new_tab(g_default_profile, NULL, NULL, NULL);
    activate(0);
    ShowWindow(g_wnd, maximized ? SW_SHOWMAXIMIZED : show ? show : SW_SHOWNORMAL);
    UpdateWindow(g_wnd);
    if (full) toggle_fullscreen();
    SetFocus(g_view);
    SetTimer(g_wnd, TIMER_BLINK, GetCaretBlinkTime() != INFINITE ? GetCaretBlinkTime() : 530, NULL);
    SetTimer(g_wnd, TIMER_DUMP, 200, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_set_dlg && IsDialogMessageW(g_set_dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
