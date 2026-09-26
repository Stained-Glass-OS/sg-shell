/* sg-terminal -- Windows Terminal (wt.exe): tabs of shells on pseudo consoles,
 * split into panes.
 *
 * Each pane is a program on a pseudo console of its own (CreatePseudoConsole;
 * Wine's conhost turns the console API into VT text on a pipe), read on a
 * thread and fed to the pane's screen (vt.c), which its own child window
 * draws with a fixed-pitch font. A tab is a tree of panes: a leaf is a pane,
 * a split puts two subtrees side by side or one above the other. Keys go to
 * the focused pane as the VT sequences conhost understands. Profiles are
 * found on the machine (PowerShell 7, Command Prompt, Git Bash, Windows
 * PowerShell) and merged with settings.json's -- the user's own profiles,
 * colour schemes, fonts and keys, as Windows Terminal keeps them
 * (wtsettings.c), re-read when the file changes. A pane divider is dragged
 * with the mouse. wt.exe's command line: -p PROFILE, -d DIR, --title T, a command
 * line, and new-tab (nt), split-pane (sp; -H, -V, -s), move-focus (mf) and
 * focus-tab (ft -t) subcommands separated by ';'.
 *
 * Keys: Ctrl+Shift+T new tab, Ctrl+Shift+W close the pane (the tab with its
 * last pane), Ctrl+Tab/Ctrl+Shift+Tab, Ctrl+Alt+1..9 go to a tab,
 * Ctrl+Shift+1..9 open a profile, Alt+Shift+Plus split the pane to the right,
 * Alt+Shift+Minus split it downwards, Alt+Shift+D duplicate it (along its
 * longer side), Alt+arrows move the focus between panes, Alt+Shift+arrows
 * move the divider, Ctrl+Shift+F find (Enter: the next match up, Shift+Enter
 * down, Alt+C match case, Escape closes), Ctrl+Shift+C or Ctrl+C with a
 * selection copies, Ctrl+Shift+V or Ctrl+V pastes, Ctrl+= / Ctrl+- / Ctrl+0
 * zoom, Shift+PgUp/PgDn/Up/Down and the wheel scroll back, F11 or Alt+Enter
 * full screen, Ctrl+, settings.
 *
 * SG_TERMINAL_DUMP=<file>: what the window shows (tabs, their places on
 * screen, the active tab's panes and their rows, the focused pane's rows and
 * colours, the cursor, the selection, the search), rewritten when it
 * changes, for the gate.
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
#include <wctype.h>
#include <limits.h>
#include "vt.h"
#include "wtsettings.h"

#define MAX_TABS 32
#define MAX_PANES 64
#define MAX_NODES 128
#define MAX_PROFILES TS_MAX_PROFILES
#define TIMER_SETTINGS 3
#define WM_PTY_OUTPUT (WM_APP + 1)
#define WM_PTY_EXIT   (WM_APP + 2)
#define TIMER_BLINK 1
#define TIMER_DUMP  2
#define SCROLLBACK 9001

/* ---- look: our own colour scheme, "Stained Glass Night" --------------------------------- */
/* the colour schemes are wtsettings.c's (Stained Glass Night is the default) */
#define SCHEME_BG     0x1D1A26   /* the find box's chrome */
#define SCHEME_FG     0xE8E4F0
#define SCHEME_CURSOR 0xC9A7FF
#define SCHEME_SEL    0x55427E
#define SCHEME_MATCH  0x6A5320      /* a search match */
#define SCHEME_CURMATCH 0xE7C564    /* the current one */
#define STRIP_BG      RGB(0x12, 0x10, 0x18)
#define STRIP_HOT     RGB(0x2A, 0x26, 0x35)
#define ACCENT        RGB(0x9B, 0x6C, 0xF0)
#define PAD 6
#define GAP 4                       /* between panes; the focused pane's frame is drawn in it */

static COLORREF rgb_of(uint32_t v) { return RGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff); }

/* ---- profiles: found on the machine, then merged with settings.json (wtsettings.c) ------------ */
static struct ts_profile g_profiles[MAX_PROFILES], g_found[8];
static int g_nprofiles, g_nfound, g_default_profile;
static struct ts_settings g_set;
static HICON g_icons[MAX_PROFILES];
static ULONGLONG g_set_stamp;

static BOOL exists(const WCHAR *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

/* the GUIDs Windows Terminal gives the same profiles (Git Bash: Git for Windows' own) */
static void add_found(const WCHAR *guid, const WCHAR *name, const WCHAR *cmd, WCHAR letter, COLORREF colour)
{
    struct ts_profile *p;
    if (g_nfound >= (int)ARRAYSIZE(g_found)) return;
    p = &g_found[g_nfound++];
    memset(p, 0, sizeof(*p));
    lstrcpynW(p->guid, guid, ARRAYSIZE(p->guid));
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

static HICON load_icon(const WCHAR *spec)
{
    WCHAR path[MAX_PATH], *comma;
    int index = 0;
    HICON icon = NULL;
    if (!spec[0]) return NULL;
    ExpandEnvironmentStringsW(spec, path, MAX_PATH);
    if ((comma = wcsrchr(path, L',')) && comma > wcsrchr(path, L'\\')) { index = _wtoi(comma + 1); *comma = 0; }
    if (wcslen(path) > 4 && !_wcsicmp(path + wcslen(path) - 4, L".ico"))
        return LoadImageW(NULL, path, IMAGE_ICON, 16, 16, LR_LOADFROMFILE);
    if (ExtractIconExW(path, index, NULL, &icon, 1) == 1) return icon;
    return NULL;
}

static void load_profiles(void)
{
    WCHAR path[MAX_PATH], cmd[MAX_PATH + 64];
    int i;
    g_nfound = 0;
    if (find_pwsh(path)) { _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\"", path); add_found(L"{574e775e-4f2a-5b96-ac1e-a2962a402336}", L"PowerShell", cmd, L'P', RGB(0x2F, 0x6F, 0xD0)); }
    if (!GetEnvironmentVariableW(L"ComSpec", path, MAX_PATH) || !exists(path)) lstrcpyW(path, L"cmd.exe");
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\"", path);
    add_found(L"{0caa0dad-35be-5f56-a8ff-afceeeaa6101}", L"Command Prompt", cmd, L'C', RGB(0x4A, 0x4A, 0x4A));
    ExpandEnvironmentStringsW(L"%ProgramFiles%\\Git\\bin\\bash.exe", path, MAX_PATH);
    if (exists(path)) { _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" --login -i", path); add_found(L"{2ece5bfe-50ed-5f3a-ab87-5cd4baafed2b}", L"Git Bash", cmd, L'G', RGB(0xE0, 0x5A, 0x2B)); }
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", path, MAX_PATH);
    if (exists(path)) { _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\"", path); add_found(L"{61c54bbd-c2c6-5271-96e7-009a87ff44bf}", L"Windows PowerShell", cmd, L'W', RGB(0x1B, 0x4F, 0x9A)); }
    ts_load(&g_set, g_found, g_nfound);
    g_set_stamp = ts_file_stamp(&g_set);
    for (i = 0; i < g_nprofiles; i++) if (g_icons[i]) { DestroyIcon(g_icons[i]); g_icons[i] = NULL; }
    g_nprofiles = 0;
    for (i = 0; i < g_set.np && g_nprofiles < MAX_PROFILES; i++) {
        g_profiles[g_nprofiles] = g_set.p[i];
        g_icons[g_nprofiles] = load_icon(g_set.p[i].icon);
        g_nprofiles++;
    }
    if (!g_nprofiles) { g_profiles[0] = g_found[g_nfound ? g_nfound - 1 : 0]; g_nprofiles = 1; }   /* never none */
    g_default_profile = 0;
    for (i = 0; i < g_nprofiles; i++)
        if (!lstrcmpiW(g_profiles[i].guid, g_set.def) || !lstrcmpiW(g_profiles[i].name, g_set.def)) { g_default_profile = i; break; }
    if (i == g_nprofiles) for (i = 0; i < g_nprofiles; i++) if (!g_profiles[i].hidden) { g_default_profile = i; break; }
}

static int profile_by_name(const WCHAR *name)
{
    int i;
    for (i = 0; i < g_nprofiles; i++) if (!lstrcmpiW(g_profiles[i].name, name) || !lstrcmpiW(g_profiles[i].guid, name)) return i;
    /* Windows Terminal's names for them */
    if (!lstrcmpiW(name, L"cmd")) return profile_by_name(L"Command Prompt");
    if (!lstrcmpiW(name, L"pwsh") || !lstrcmpiW(name, L"PowerShell 7")) return profile_by_name(L"PowerShell");
    return -1;
}

/* the n-th profile shown in the menu (hidden ones are not), or -1 */
static int visible_profile(int n)
{
    int i;
    for (i = 0; i < g_nprofiles; i++) if (!g_profiles[i].hidden && !n--) return i;
    return -1;
}

/* the profile that runs the same program as a command line, or -1 */
static int profile_for_command(const WCHAR *cmd)
{
    WCHAR first[MAX_PATH], *base, *dot;
    int i, argc2, profile = -1;
    WCHAR **a = CommandLineToArgvW(cmd, &argc2);
    if (a && argc2) {
        lstrcpynW(first, a[0], MAX_PATH);
        base = wcsrchr(first, L'\\') ? wcsrchr(first, L'\\') + 1 : first;
        if ((dot = wcsrchr(base, L'.')) && !_wcsicmp(dot, L".exe")) *dot = 0;
        for (i = 0; i < g_nprofiles && profile < 0; i++) {
            WCHAR **b;
            if (!g_profiles[i].detected) continue;
            b = CommandLineToArgvW(g_profiles[i].cmd, &argc2);
            if (b && argc2) {
                WCHAR *pb = wcsrchr(b[0], L'\\') ? wcsrchr(b[0], L'\\') + 1 : b[0], *pd;
                if ((pd = wcsrchr(pb, L'.')) && !_wcsicmp(pd, L".exe")) *pd = 0;
                if (!_wcsicmp(pb, base)) profile = i;
            }
            LocalFree(b);
        }
    }
    LocalFree(a);
    return profile;
}

/* ---- panes, the tree of a tab, tabs --------------------------------------------------------------- */
struct tfont { WCHAR face[LF_FACESIZE]; int pt; HFONT f, fb; int cw, ch; };

struct pane {
    int used, id, profile, tab;     /* tab: the owning tab's slot */
    WCHAR guid[40];                 /* the profile's, which survives a reload of the settings */
    struct vt vt;
    HPCON pc;
    HANDLE in_w, out_r, process, reader;
    BOOL alive;
    DWORD exit_code;
    WCHAR title[160], cmd[1024], dir[MAX_PATH];
    int scroll;                     /* lines back into the scrollback, 0: live */
    BOOL sel_active, selecting;
    int sel_ax, sel_ay, sel_bx, sel_by;     /* view coordinates */
    HWND view;
    int cols, rows;                 /* the grid its window holds */
    /* search (Ctrl+Shift+F) */
    HWND find, find_edit;
    WCHAR needle[128];
    BOOL match_case;
    int nmatches, cur_line, cur_col, cur_len, cur_index;    /* cur_line: absolute, 0 = the oldest scrollback line */
};

enum { NODE_LEAF, SPLIT_V /* side by side */, SPLIT_H /* one above the other */ };
struct node { int used, kind, pane, a, b, parent, ratio; /* ratio: the first child's share, per mille */
              RECT box, gap; /* a split's rectangle and the divider between its halves, client coordinates */ };

struct tab { int used, id, root, focus; WCHAR fixed_title[160]; };

static struct pane g_panes[MAX_PANES];
static struct node g_nodes[MAX_NODES];
static struct tab g_tabs[MAX_TABS];
static int g_order[MAX_TABS], g_ntabs, g_active = -1, g_next_id = 1;

static HWND g_wnd;
static HINSTANCE g_inst;
static HFONT g_font, g_font_bold, g_ui_font, g_ui_small;
static WCHAR g_face[LF_FACESIZE] = L"Consolas";
static int g_font_pt = 12, g_dpi = 96, g_cw = 8, g_ch = 16, g_cols = 120, g_rows = 30, g_zoom;
static struct tfont g_fonts[16];
static int g_nfonts, g_drag = -1;       /* g_drag: the split node whose divider is being dragged */
static int g_tab_h = 40;
static BOOL g_cursor_on = TRUE, g_fullscreen;
static WINDOWPLACEMENT g_saved_place = { sizeof(g_saved_place) };
static int g_hot_tab = -1, g_hot_close = -1, g_hot_btn;    /* 1: +, 2: the menu */
static unsigned long long g_last_dump_gen;

static int S(int v) { return MulDiv(v, g_dpi, 96); }
static struct tab *active_tab(void) { return g_active >= 0 && g_active < g_ntabs ? &g_tabs[g_order[g_active]] : NULL; }
static struct pane *active_pane(void)
{
    struct tab *t = active_tab();
    return t && t->focus >= 0 ? &g_panes[t->focus] : NULL;
}
static struct pane *pane_by_id(int id)
{
    int i;
    for (i = 0; i < MAX_PANES; i++) if (g_panes[i].used && g_panes[i].id == id) return &g_panes[i];
    return NULL;
}
static int pane_slot(const struct pane *p) { return (int)(p - g_panes); }
static int tab_index(int slot)
{
    int i;
    for (i = 0; i < g_ntabs; i++) if (g_order[i] == slot) return i;
    return -1;
}

static int new_node(int kind, int pane, int parent)
{
    int i;
    for (i = 0; i < MAX_NODES && g_nodes[i].used; i++) ;
    if (i == MAX_NODES) return -1;
    memset(&g_nodes[i], 0, sizeof(g_nodes[i]));
    g_nodes[i].used = 1; g_nodes[i].kind = kind; g_nodes[i].pane = pane; g_nodes[i].parent = parent;
    g_nodes[i].a = g_nodes[i].b = -1; g_nodes[i].ratio = 500;
    return i;
}

static int leaf_of(int root, int pane)
{
    int r;
    if (root < 0) return -1;
    if (g_nodes[root].kind == NODE_LEAF) return g_nodes[root].pane == pane ? root : -1;
    if ((r = leaf_of(g_nodes[root].a, pane)) >= 0) return r;
    return leaf_of(g_nodes[root].b, pane);
}

/* the panes of a subtree, in order (left to right, top to bottom) */
static int leaves(int root, int *out, int n)
{
    if (root < 0) return n;
    if (g_nodes[root].kind == NODE_LEAF) { out[n++] = g_nodes[root].pane; return n; }
    n = leaves(g_nodes[root].a, out, n);
    return leaves(g_nodes[root].b, out, n);
}

static void pty_send(struct pane *p, const char *s, int n)
{
    DWORD w;
    if (!p || !p->alive || !p->in_w || n <= 0) return;
    WriteFile(p->in_w, s, n, &w, NULL);
}

static void pty_send_w(struct pane *p, const WCHAR *s, int n)
{
    char buf[4096];
    int len = WideCharToMultiByte(CP_UTF8, 0, s, n, buf, sizeof(buf), NULL, NULL);
    pty_send(p, buf, len);
}

/* where typed keys go: the focused pane (the mutant: the tab's first pane, whichever has the focus) */
static struct pane *input_pane(struct pane *p)
{
#ifdef SG_MUTANT_PANEINPUT
    int all[MAX_PANES];
    if (p && p->tab >= 0 && g_tabs[p->tab].used && leaves(g_tabs[p->tab].root, all, 0)) return &g_panes[all[0]];
#endif
    return p;
}

struct chunk { int id; DWORD len; char data[1]; };

static DWORD WINAPI reader_thread(void *arg)
{
    struct pane *p = arg;
    int id = p->id;
    HANDLE out = p->out_r;
    char buf[8192];
    DWORD n;
    while (ReadFile(out, buf, sizeof(buf), &n, NULL) && n) {
        struct chunk *c = malloc(sizeof(*c) + n);
        if (!c) continue;
        c->id = id; c->len = n;
        memcpy(c->data, buf, n);
        if (!PostMessageW(g_wnd, WM_PTY_OUTPUT, 0, (LPARAM)c)) free(c);
    }
    CloseHandle(out);
    return 0;
}

struct wait_arg { int id; HANDLE process; };
static DWORD WINAPI exit_watch(void *arg)
{
    struct wait_arg *w = arg;
    WaitForSingleObject(w->process, INFINITE);
    Sleep(150);         /* the last output comes through first */
    PostMessageW(g_wnd, WM_PTY_EXIT, w->id, 0);
    CloseHandle(w->process);
    free(w);
    return 0;
}

static DWORD WINAPI closer_thread(void *arg)
{
    ClosePseudoConsole((HPCON)arg);     /* waits for conhost: not on the window's thread */
    return 0;
}

static void update_title(void);
static void layout_tabs(void);
static void layout_panes(void);
static void write_dump(BOOL force);
static void find_close(struct pane *p);

static BOOL spawn(struct pane *p)
{
    HANDLE in_r = NULL, out_w = NULL;
    COORD size = { (SHORT)p->cols, (SHORT)p->rows };
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    SIZE_T len = 0;
    WCHAR cmd[1024], session[48];
    HRESULT hr;
    struct wait_arg *w;

    if (!CreatePipe(&in_r, &p->in_w, NULL, 0) || !CreatePipe(&p->out_r, &out_w, NULL, 0)) return FALSE;
    hr = CreatePseudoConsole(size, in_r, out_w, 0, &p->pc);
    CloseHandle(in_r); CloseHandle(out_w);
    if (FAILED(hr)) { CloseHandle(p->in_w); CloseHandle(p->out_r); p->in_w = p->out_r = NULL; return FALSE; }
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.lpTitle = p->title;     /* the console's first title: the profile's name */
    InitializeProcThreadAttributeList(NULL, 1, 0, &len);
    si.lpAttributeList = HeapAlloc(GetProcessHeap(), 0, len);
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &len);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, p->pc, sizeof(p->pc), NULL, NULL);
    /* as Windows Terminal does: the shell can tell it runs in one */
    _snwprintf(session, ARRAYSIZE(session), L"{%08lx-%04x-4000-8000-%012llx}", GetCurrentProcessId(), p->id & 0xffff,
               (unsigned long long)GetTickCount64());
    SetEnvironmentVariableW(L"WT_SESSION", session);
    SetEnvironmentVariableW(L"WT_PROFILE_ID", g_profiles[p->profile].guid);
    lstrcpynW(cmd, p->cmd, ARRAYSIZE(cmd));
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, NULL,
                        p->dir[0] ? p->dir : NULL, &si.StartupInfo, &pi)) {
        char msg[600];
        int n = snprintf(msg, sizeof(msg), "\x1b[91m[error 0x%08lx when launching `", GetLastError());
        n += WideCharToMultiByte(CP_UTF8, 0, p->cmd, -1, msg + n, sizeof(msg) - n - 8, NULL, NULL) - 1;
        n += snprintf(msg + n, sizeof(msg) - n, "']\x1b[m\r\n");
        vt_write(&p->vt, msg, n);
        DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
        CloseHandle(CreateThread(NULL, 0, closer_thread, p->pc, 0, NULL));
        p->pc = NULL;
        CloseHandle(p->in_w); CloseHandle(p->out_r); p->in_w = p->out_r = NULL;
        p->alive = FALSE; p->exit_code = 1;
        return FALSE;
    }
    DeleteProcThreadAttributeList(si.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    CloseHandle(pi.hThread);
    p->process = pi.hProcess;
    p->alive = TRUE;
    if (p->reader) CloseHandle(p->reader);
    p->reader = CreateThread(NULL, 0, reader_thread, p, 0, NULL);
    if ((w = malloc(sizeof(*w)))) {
        w->id = p->id;
        DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &w->process, SYNCHRONIZE, FALSE, 0);
        CloseHandle(CreateThread(NULL, 0, exit_watch, w, 0, NULL));
    }
    return TRUE;
}

/* a pane, its window and its screen; not started (spawn) yet */
static int new_pane(int tab, int profile, const WCHAR *cmd, const WCHAR *dir)
{
    int slot;
    struct pane *p;
    for (slot = 0; slot < MAX_PANES && g_panes[slot].used; slot++) ;
    if (slot == MAX_PANES) return -1;
    if ((profile < 0 || profile >= g_nprofiles) && cmd && cmd[0]) profile = profile_for_command(cmd);
    if (profile < 0 || profile >= g_nprofiles) profile = g_default_profile;
    p = &g_panes[slot];
    memset(p, 0, sizeof(*p));
    p->used = 1;
    p->id = g_next_id++;
    p->profile = profile;
    p->tab = tab;
    p->cols = g_cols; p->rows = g_rows;
    p->cur_line = -1;
    if (!vt_init(&p->vt, p->cols, p->rows, SCROLLBACK)) { p->used = 0; return -1; }
    lstrcpynW(p->cmd, cmd && cmd[0] ? cmd : g_profiles[profile].cmd, ARRAYSIZE(p->cmd));
    lstrcpynW(p->guid, g_profiles[profile].guid, ARRAYSIZE(p->guid));
    if (dir && dir[0]) lstrcpynW(p->dir, dir, MAX_PATH);
    else if (g_profiles[profile].dir[0]) {
        /* the profile's startingDirectory: environment variables, and ~ for the user's folder */
        WCHAR raw[MAX_PATH];
        if (g_profiles[profile].dir[0] == L'~') {
            SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, raw);
            wcsncat(raw, g_profiles[profile].dir + 1, MAX_PATH - wcslen(raw) - 1);
        } else lstrcpynW(raw, g_profiles[profile].dir, MAX_PATH);
        ExpandEnvironmentStringsW(raw, p->dir, MAX_PATH);
    }
    else SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, p->dir);       /* Windows Terminal starts in %USERPROFILE% */
    lstrcpynW(p->title, g_profiles[profile].name, ARRAYSIZE(p->title));
    p->view = CreateWindowExW(0, L"SgTerminalView", L"", WS_CHILD | WS_VSCROLL | WS_CLIPCHILDREN, 0, 0, 0, 0, g_wnd, NULL, g_inst, NULL);
    SetWindowLongPtrW(p->view, GWLP_USERDATA, slot + 1);
    return slot;
}

static void free_pane(struct pane *p)
{
    if (p->alive && p->process) TerminateProcess(p->process, 1);
    if (p->pc) { CloseHandle(CreateThread(NULL, 0, closer_thread, p->pc, 0, NULL)); p->pc = NULL; }
    if (p->in_w) CloseHandle(p->in_w);
    /* the reader ends when the pipe does, and closes it */
    if (p->reader) CloseHandle(p->reader);
    if (p->process) CloseHandle(p->process);
    if (p->view) DestroyWindow(p->view);
    vt_free(&p->vt);
    p->used = 0;
}

static int new_tab(int profile, const WCHAR *cmd, const WCHAR *dir, const WCHAR *title)
{
    int slot, ps;
    struct tab *t;
    if (g_ntabs >= MAX_TABS) return -1;
    for (slot = 0; slot < MAX_TABS && g_tabs[slot].used; slot++) ;
    t = &g_tabs[slot];
    memset(t, 0, sizeof(*t));
    if ((ps = new_pane(slot, profile, cmd, dir)) < 0) return -1;
    t->used = 1;
    t->id = g_next_id++;
    t->focus = ps;
    t->root = new_node(NODE_LEAF, ps, -1);
    if (title && title[0]) lstrcpynW(t->fixed_title, title, ARRAYSIZE(t->fixed_title));
    g_order[g_ntabs++] = slot;
    g_active = g_ntabs - 1;
    layout_tabs();
    layout_panes();
    spawn(&g_panes[ps]);
    update_title();
    SetFocus(g_panes[ps].view);
    InvalidateRect(g_wnd, NULL, FALSE);
    write_dump(TRUE);
    return slot;
}

/* free a subtree's nodes (not its panes) */
static void free_nodes(int n)
{
    if (n < 0) return;
    if (g_nodes[n].kind != NODE_LEAF) { free_nodes(g_nodes[n].a); free_nodes(g_nodes[n].b); }
    g_nodes[n].used = 0;
}

static void close_tab(int index)
{
    struct tab *t;
    int all[MAX_PANES], n, i;
    if (index < 0 || index >= g_ntabs) return;
    t = &g_tabs[g_order[index]];
    n = leaves(t->root, all, 0);
    free_nodes(t->root);
    for (i = 0; i < n; i++) free_pane(&g_panes[all[i]]);
    t->used = 0;
    memmove(&g_order[index], &g_order[index + 1], (g_ntabs - index - 1) * sizeof(g_order[0]));
    g_ntabs--;
    if (!g_ntabs) { DestroyWindow(g_wnd); return; }
    if (g_active >= g_ntabs) g_active = g_ntabs - 1;
    else if (g_active > index) g_active--;
    layout_tabs();
    layout_panes();
    update_title();
    if (active_pane()) SetFocus(active_pane()->view);
    InvalidateRect(g_wnd, NULL, FALSE);
    write_dump(TRUE);
}

/* close one pane; its sibling takes the place of both. The tab goes with its last pane. */
static void close_pane(struct pane *p)
{
    struct tab *t = &g_tabs[p->tab];
    int leaf = leaf_of(t->root, pane_slot(p)), parent, sib, all[MAX_PANES];
    struct node *pn;
    if (leaf < 0) return;
    if (leaf == t->root) { close_tab(tab_index(p->tab)); return; }
    parent = g_nodes[leaf].parent;
    pn = &g_nodes[parent];
    sib = pn->a == leaf ? pn->b : pn->a;
    free_pane(p);
    g_nodes[leaf].used = 0;
    /* the sibling moves up into the parent's node, so the parent's parent needs no change */
    pn->kind = g_nodes[sib].kind; pn->pane = g_nodes[sib].pane;
    pn->a = g_nodes[sib].a; pn->b = g_nodes[sib].b; pn->ratio = g_nodes[sib].ratio;
    if (pn->a >= 0) g_nodes[pn->a].parent = parent;
    if (pn->b >= 0) g_nodes[pn->b].parent = parent;
    g_nodes[sib].used = 0;
    if (t->focus == pane_slot(p) || !g_panes[t->focus].used) { leaves(parent, all, 0); t->focus = all[0]; }
    layout_panes();
    update_title();
    if (&g_tabs[g_order[g_active]] == t) SetFocus(g_panes[t->focus].view);
    InvalidateRect(g_wnd, NULL, FALSE);
    write_dump(TRUE);
}

/* split the active tab's focused pane: kind SPLIT_V puts the new pane to its right, SPLIT_H below,
 * 0 along the longer side; share: the new pane's part, per mille */
static int split_pane(int kind, int profile, const WCHAR *cmd, const WCHAR *dir, int share)
{
    struct tab *t = active_tab();
    struct pane *f;
    int leaf, ps, a, b;
    if (!t) return -1;
    f = &g_panes[t->focus];
    if ((leaf = leaf_of(t->root, t->focus)) < 0) return -1;
    if (!kind) {
        RECT r;
        GetClientRect(f->view, &r);
        kind = r.right + GetSystemMetrics(SM_CXVSCROLL) >= r.bottom ? SPLIT_V : SPLIT_H;
    }
    if ((ps = new_pane(f->tab, profile, cmd, dir)) < 0) return -1;
    a = new_node(NODE_LEAF, t->focus, leaf);
    b = new_node(NODE_LEAF, ps, leaf);
    if (a < 0 || b < 0) { if (a >= 0) g_nodes[a].used = 0; free_pane(&g_panes[ps]); return -1; }
    g_nodes[leaf].kind = kind; g_nodes[leaf].pane = -1;
    g_nodes[leaf].a = a; g_nodes[leaf].b = b;
    g_nodes[leaf].ratio = share > 0 && share < 1000 ? 1000 - share : 500;
    t->focus = ps;
    layout_panes();                 /* the new pane's size, and the old one's pseudo console resized */
    spawn(&g_panes[ps]);
    update_title();
    SetFocus(g_panes[ps].view);
    InvalidateRect(g_wnd, NULL, FALSE);
    write_dump(TRUE);
    return ps;
}

/* the focus to the pane beside the focused one: dx/dy the direction. FALSE if there is none. */
static BOOL move_focus(int dx, int dy)
{
    struct tab *t = active_tab();
    RECT fr, r;
    int all[MAX_PANES], n, i, best = -1, best_d = INT_MAX;
    if (!t) return FALSE;
    n = leaves(t->root, all, 0);
    if (n < 2) return FALSE;
    GetWindowRect(g_panes[t->focus].view, &fr);
    for (i = 0; i < n; i++) {
        int d, overlap;
        if (all[i] == t->focus) continue;
        GetWindowRect(g_panes[all[i]].view, &r);
        if (dx) {
            if (dx > 0 ? r.left < fr.right : r.right > fr.left) continue;
            overlap = min(r.bottom, fr.bottom) - max(r.top, fr.top);
            d = dx > 0 ? r.left - fr.right : fr.left - r.right;
        } else {
            if (dy > 0 ? r.top < fr.bottom : r.bottom > fr.top) continue;
            overlap = min(r.right, fr.right) - max(r.left, fr.left);
            d = dy > 0 ? r.top - fr.bottom : fr.top - r.bottom;
        }
        if (overlap <= 0) continue;
        if (d < best_d) { best_d = d; best = all[i]; }
    }
    if (best < 0) return FALSE;
    t->focus = best;
    SetFocus(g_panes[best].view);
    update_title();
    InvalidateRect(g_wnd, NULL, FALSE);
    write_dump(TRUE);
    return TRUE;
}

/* move the divider of the nearest split around the focused pane that runs that way. FALSE if none. */
static BOOL resize_pane(int dx, int dy)
{
    struct tab *t = active_tab();
    int n, kind = dx ? SPLIT_V : SPLIT_H;
    if (!t) return FALSE;
    for (n = g_nodes[leaf_of(t->root, t->focus)].parent; n >= 0; n = g_nodes[n].parent)
        if (g_nodes[n].kind == kind) {
            int r = g_nodes[n].ratio + 50 * (dx ? dx : dy);
            if (r < 100) r = 100;
            if (r > 900) r = 900;
            if (r == g_nodes[n].ratio) return TRUE;
            g_nodes[n].ratio = r;
            layout_panes();
            InvalidateRect(g_wnd, NULL, FALSE);
            write_dump(TRUE);
            return TRUE;
        }
    return FALSE;
}

static void activate(int index)
{
    if (index < 0 || index >= g_ntabs) return;
    g_active = index;
    layout_panes();
    update_title();
    if (active_pane()) SetFocus(active_pane()->view);
    InvalidateRect(g_wnd, NULL, FALSE);
    write_dump(TRUE);
}

static const WCHAR *tab_title(const struct tab *t)
{
    return t->fixed_title[0] ? t->fixed_title : g_panes[t->focus].title;
}

static void update_title(void)
{
    struct tab *t = active_tab();
    SetWindowTextW(g_wnd, t ? tab_title(t) : L"Terminal");
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

/* a face that exists: the one asked for, else the first installed fixed-pitch one we like */
static void pick_face(const WCHAR *want, WCHAR *out)
{
    static const WCHAR *const faces[] = { L"Cascadia Mono", L"Cascadia Code", L"Consolas", L"DejaVu Sans Mono",
                                          L"Liberation Mono", L"Courier New" };
    size_t i;
    if (want && want[0] && have_font(want)) { lstrcpynW(out, want, LF_FACESIZE); return; }
    for (i = 0; i < ARRAYSIZE(faces); i++) if (have_font(faces[i])) { lstrcpyW(out, faces[i]); return; }
    lstrcpyW(out, L"Courier New");
}

/* a font of a face and size (points, before the zoom), made once */
static struct tfont *font_for(const WCHAR *want, int pt)
{
    WCHAR face[LF_FACESIZE];
    struct tfont *f;
    HDC dc;
    TEXTMETRICW tm;
    SIZE sz;
    int i;
    pick_face(want, face);
    if (pt < 4) pt = 12;
    pt += g_zoom;
    if (pt < 6) pt = 6;
    if (pt > 72) pt = 72;
    for (i = 0; i < g_nfonts; i++) if (g_fonts[i].pt == pt && !lstrcmpiW(g_fonts[i].face, face)) return &g_fonts[i];
    if (g_nfonts == ARRAYSIZE(g_fonts)) return &g_fonts[0];
    f = &g_fonts[g_nfonts++];
    lstrcpyW(f->face, face);
    f->pt = pt;
    f->f = CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, face);
    f->fb = CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, face);
    dc = GetDC(NULL);
    SelectObject(dc, f->f);
    GetTextMetricsW(dc, &tm);
    GetTextExtentPoint32W(dc, L"M", 1, &sz);
    f->cw = sz.cx > 0 ? sz.cx : tm.tmAveCharWidth;
    f->ch = tm.tmHeight + tm.tmExternalLeading;
    ReleaseDC(NULL, dc);
    return f;
}

/* a profile's font: its own face and size, else the settings' defaults */
static struct tfont *profile_font(int profile)
{
    const struct ts_profile *p = profile >= 0 && profile < g_nprofiles ? &g_profiles[profile] : NULL;
    return font_for(p && p->face[0] ? p->face : g_set.face, p && p->size ? p->size : g_set.size ? g_set.size : 12);
}

static struct tfont *pane_font(const struct pane *p) { return profile_font(p->profile); }

static const struct ts_scheme *pane_scheme(const struct pane *p)
{
    return ts_scheme(&g_set, p && p->profile >= 0 && p->profile < g_nprofiles ? g_profiles[p->profile].scheme : NULL);
}

/* after a change of settings or zoom: the fonts made again; the default profile's is the window's */
static void make_fonts(void)
{
    int i;
    struct tfont *f;
    for (i = 0; i < g_nfonts; i++) { DeleteObject(g_fonts[i].f); DeleteObject(g_fonts[i].fb); }
    g_nfonts = 0;
    f = profile_font(g_default_profile);
    lstrcpyW(g_face, f->face);
    g_font_pt = f->pt;
    g_font = f->f; g_font_bold = f->fb;
    g_cw = f->cw; g_ch = f->ch;
}

/* the grid a pane's window holds; its screen and pseudo console follow */
static void pane_grid(struct pane *p, BOOL force)
{
    RECT r;
    int cols, rows;
    struct tfont *f = pane_font(p);
    GetClientRect(p->view, &r);
    if (r.right <= 0 || r.bottom <= 0) return;
    cols = (r.right - 2 * S(PAD)) / f->cw;
    rows = (r.bottom - 2 * S(PAD)) / f->ch;
    if (cols < 10) cols = 10;
    if (rows < 2) rows = 2;
    if (!force && cols == p->cols && rows == p->rows && cols == p->vt.cols && rows == p->vt.rows) return;
    p->cols = cols; p->rows = rows;
    vt_resize(&p->vt, cols, rows);
#ifndef SG_MUTANT_NORESIZE
    if (p->pc) { COORD c = { (SHORT)cols, (SHORT)rows }; ResizePseudoConsole(p->pc, c); }
#endif
    if (p->scroll > p->vt.sb_count) p->scroll = p->vt.sb_count;
    write_dump(TRUE);
}

/* the default grid: what a single pane fills the window's terminal area with */
static void default_grid(void)
{
    RECT r;
    GetClientRect(g_wnd, &r);
    g_cols = (r.right - GetSystemMetrics(SM_CXVSCROLL) - 2 * S(PAD)) / g_cw;
    g_rows = (r.bottom - g_tab_h - 2 * S(PAD)) / g_ch;
    if (g_cols < 10) g_cols = 10;
    if (g_rows < 2) g_rows = 2;
}

static RECT pane_area(void)
{
    RECT r;
    GetClientRect(g_wnd, &r);
    r.top = g_tab_h;
    if (r.bottom < r.top) r.bottom = r.top;
    return r;
}

static void place_find(struct pane *p);

static void layout_node(int n, RECT r)
{
    struct node *nd = &g_nodes[n];
    RECT a = r, b = r;
    if (nd->kind == NODE_LEAF) {
        struct pane *p = &g_panes[nd->pane];
        MoveWindow(p->view, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        ShowWindow(p->view, SW_SHOWNA);
        pane_grid(p, FALSE);
        place_find(p);
        return;
    }
    nd->box = r;
    nd->gap = r;
    if (nd->kind == SPLIT_V) {
        int w = r.right - r.left - S(GAP);
        a.right = r.left + w * nd->ratio / 1000;
        b.left = a.right + S(GAP);
        nd->gap.left = a.right; nd->gap.right = b.left;
    } else {
        int h = r.bottom - r.top - S(GAP);
        a.bottom = r.top + h * nd->ratio / 1000;
        b.top = a.bottom + S(GAP);
        nd->gap.top = a.bottom; nd->gap.bottom = b.top;
    }
    layout_node(nd->a, a);
    layout_node(nd->b, b);
}

static int divider_at(int n, POINT pt)
{
    struct node *nd = &g_nodes[n];
    RECT g;
    int r;
    if (nd->kind == NODE_LEAF) return -1;
    g = nd->gap;
    /* a little wider than the gap, for a comfortable grab */
    if (nd->kind == SPLIT_V) InflateRect(&g, S(2), 0); else InflateRect(&g, 0, S(2));
    if (PtInRect(&g, pt)) return n;
    if ((r = divider_at(nd->a, pt)) >= 0) return r;
    return divider_at(nd->b, pt);
}

/* the divider of split n dragged to x (or y): its first half's share follows */
static void drag_divider(int n, POINT pt)
{
    struct node *nd = &g_nodes[n];
    int span, pos, r;
#ifdef SG_MUTANT_NODRAG
    return;
#endif
    if (nd->kind == SPLIT_V) { span = nd->box.right - nd->box.left - S(GAP); pos = pt.x - nd->box.left - S(GAP) / 2; }
    else { span = nd->box.bottom - nd->box.top - S(GAP); pos = pt.y - nd->box.top - S(GAP) / 2; }
    if (span <= 0) return;
    r = (int)((long long)pos * 1000 / span);
    if (r < 50) r = 50;
    if (r > 950) r = 950;
    if (r == nd->ratio) return;
    nd->ratio = r;
    layout_panes();
    InvalidateRect(g_wnd, NULL, FALSE);
    write_dump(TRUE);
}

static void layout_panes(void)
{
    int i, j, all[MAX_PANES], n;
    struct tab *act = active_tab();
    for (i = 0; i < g_ntabs; i++) {
        struct tab *t = &g_tabs[g_order[i]];
        if (t == act) continue;
        n = leaves(t->root, all, 0);
        for (j = 0; j < n; j++) ShowWindow(g_panes[all[j]].view, SW_HIDE);
    }
    if (act) layout_node(act->root, pane_area());
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
        struct pane *p = &g_panes[t->focus];
        RECT r = g_tab_rects[i], txt, ic;
        BOOL act = i == g_active, hot = i == g_hot_tab;
        if (act || hot) {
            b = CreateSolidBrush(act ? rgb_of(pane_scheme(p)->bg) : STRIP_HOT);
            FillRect(dc, &r, b); DeleteObject(b);
        }
        if (act) {
            RECT line = { r.left, r.top, r.right, r.top + S(2) };
            b = CreateSolidBrush(ACCENT); FillRect(dc, &line, b); DeleteObject(b);
        }
        /* the focused pane's profile's badge: a letter on its colour */
        SetRect(&ic, r.left + S(10), (r.top + r.bottom) / 2 - S(8), r.left + S(26), (r.top + r.bottom) / 2 + S(8));
        if (g_icons[p->profile]) DrawIconEx(dc, ic.left, ic.top, g_icons[p->profile], S(16), S(16), 0, NULL, DI_NORMAL);
        else {
            b = CreateSolidBrush(g_profiles[p->profile].colour); FillRect(dc, &ic, b); DeleteObject(b);
            SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
            SelectObject(dc, g_ui_small);
            DrawTextW(dc, &g_profiles[p->profile].letter, 1, &ic, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, g_ui_font);
        }
        SetRect(&txt, r.left + S(34), r.top, r.right - S(32), r.bottom);
        SetTextColor(dc, act ? RGB(0xFF, 0xFF, 0xFF) : RGB(0xB8, 0xB2, 0xC4));
        DrawTextW(dc, tab_title(t), -1, &txt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (act || hot) {
            RECT c = close_rect(i);
            if (g_hot_close == i) { b = CreateSolidBrush(RGB(0x44, 0x3F, 0x52)); FillRect(dc, &c, b); DeleteObject(b); }
            draw_glyph_x(dc, c, RGB(0xE0, 0xDC, 0xE8));
        }
        if (!p->alive && p->exit_code) {
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

/* the area around the panes: the gaps between them, and the focused one's frame when there are several */
static void paint_area(HDC dc)
{
    RECT area = pane_area(), fr;
    struct tab *t = active_tab();
    int all[MAX_PANES];
    HBRUSH b = CreateSolidBrush(STRIP_BG);
    FillRect(dc, &area, b); DeleteObject(b);
    if (!t || leaves(t->root, all, 0) < 2) return;
    GetWindowRect(g_panes[t->focus].view, &fr);
    MapWindowPoints(NULL, g_wnd, (POINT *)&fr, 2);
    InflateRect(&fr, S(2), S(2));
    IntersectClipRect(dc, area.left, area.top, area.right, area.bottom);
    b = CreateSolidBrush(ACCENT);
    FrameRect(dc, &fr, b);
    InflateRect(&fr, -1, -1);
    FrameRect(dc, &fr, b);
    DeleteObject(b);
}

/* ---- search ------------------------------------------------------------------------------------------ */
static int total_lines(const struct pane *p) { return p->vt.sb_count + p->vt.rows; }
static const struct vt_line *abs_line(const struct pane *p, int a) { return vt_view_line(&p->vt, p->vt.sb_count, a); }
static int view_top(const struct pane *p) { return p->vt.sb_count - p->scroll; }

static BOOL search_line(const struct pane *p, int a)
{
#ifdef SG_MUTANT_NOSEARCH
    return a >= p->vt.sb_count;     /* the mutant: only the screen, never the scrollback */
#else
    (void)p; (void)a;
    return TRUE;
#endif
}

/* the matches in one line: start and end (exclusive) columns; returns how many */
static int line_matches(const struct pane *p, const struct vt_line *l, int *starts, int *ends, int max)
{
    int n = 0, len = lstrlenW(p->needle), x, cols;
    if (!l || !len) return 0;
    cols = l->width < p->vt.cols ? l->width : p->vt.cols;
    for (x = 0; x < cols && n < max; ) {
        int i, cx = x;
        for (i = 0; i < len && cx < cols; i++) {
            uint32_t c = l->cells[cx].ch ? l->cells[cx].ch : ' ';
            WCHAR w = p->needle[i];
            if (c > 0xffff) break;
            if (p->match_case ? (WCHAR)c != w : towlower((WCHAR)c) != towlower(w)) break;
            cx++;
            while (cx < cols && (l->cells[cx].flags & VT_WIDE_TAIL)) cx++;
        }
        if (i == len) { starts[n] = x; ends[n] = cx; n++; x = cx; }
        else x++;
    }
    return n;
}

static void count_matches(struct pane *p)
{
    int a, n = 0, s[256], e[256], k, idx = 0;
    for (a = 0; a < total_lines(p); a++) {
        if (!search_line(p, a)) continue;
        k = line_matches(p, abs_line(p, a), s, e, 256);
        if (a < p->cur_line) idx += k;
        else if (a == p->cur_line) { int j; for (j = 0; j < k; j++) if (s[j] < p->cur_col) idx++; }
        n += k;
    }
    p->nmatches = n;
    p->cur_index = p->cur_line >= 0 ? idx + 1 : 0;
}

/* scroll so the current match is shown, near the middle when it was not */
static void show_match(struct pane *p)
{
    int top = view_top(p);
    if (p->cur_line < 0) return;
    if (p->cur_line < top || p->cur_line >= top + p->vt.rows) {
        int s = p->vt.sb_count - (p->cur_line - p->vt.rows / 2);
        if (s < 0) s = 0;
        if (s > p->vt.sb_count) s = p->vt.sb_count;
        p->scroll = s;
    }
}

/* the next match from (line, col): dir -1 up (older), +1 down; from_inclusive: a match at (line, col) counts */
static BOOL step_match(struct pane *p, int line, int col, int dir, BOOL inclusive)
{
    int total = total_lines(p), s[256], e[256], k, j, pass, a;
    for (pass = 0; pass < 2; pass++) {
        for (a = line; a >= 0 && a < total; a += dir) {
            if (!search_line(p, a)) continue;
            k = line_matches(p, abs_line(p, a), s, e, 256);
            if (dir < 0) {
                for (j = k - 1; j >= 0; j--)
                    if (a != line || pass || (inclusive ? s[j] <= col : s[j] < col)) goto found;
            } else {
                for (j = 0; j < k; j++)
                    if (a != line || pass || (inclusive ? s[j] >= col : s[j] > col)) goto found;
            }
            continue;
found:
            p->cur_line = a; p->cur_col = s[j]; p->cur_len = e[j] - s[j];
            return TRUE;
        }
        /* wrap round */
        line = dir < 0 ? total - 1 : 0;
        col = dir < 0 ? INT_MAX : -1;
        inclusive = TRUE;
    }
    return FALSE;
}

static void search_update(struct pane *p, int dir, BOOL restart)
{
    if (!p->needle[0]) { p->cur_line = -1; p->nmatches = 0; p->cur_index = 0; }
    else if (restart) {
        /* as typed: the nearest match up from the bottom of what is shown */
        int line = view_top(p) + p->vt.rows - 1;
        if (!step_match(p, line, INT_MAX, -1, TRUE)) p->cur_line = -1;
    } else if (p->cur_line >= 0) {
        if (!step_match(p, p->cur_line, p->cur_col, dir, FALSE)) p->cur_line = -1;
    } else if (!step_match(p, view_top(p) + p->vt.rows - 1, INT_MAX, dir, TRUE)) p->cur_line = -1;
    count_matches(p);
    show_match(p);
    InvalidateRect(p->view, NULL, FALSE);
    if (p->find) InvalidateRect(p->find, NULL, FALSE);
    write_dump(TRUE);
}

/* the search box: a panel at the pane's top right with the text, match case, up, down and close */
enum { FB_CASE, FB_UP, FB_DOWN, FB_CLOSE, FB_COUNT };
static RECT find_button(int i)
{
    RECT r;
    int x = S(190) + i * S(30);
    SetRect(&r, x, S(5), x + S(28), S(31));
    return r;
}

static void place_find(struct pane *p)
{
    RECT r;
    int w = S(190) + FB_COUNT * S(30) + S(6);
    if (!p->find) return;
    GetClientRect(p->view, &r);
    SetWindowPos(p->find, HWND_TOP, r.right - w - S(12) > 0 ? r.right - w - S(12) : 0, S(4), w, S(36), SWP_NOACTIVATE);
}

static WNDPROC g_edit_proc;
static struct pane *find_owner(HWND hwnd)
{
    int i;
    for (i = 0; i < MAX_PANES; i++) if (g_panes[i].used && (g_panes[i].find == hwnd || g_panes[i].find_edit == hwnd)) return &g_panes[i];
    return NULL;
}

static void find_toggle_case(struct pane *p)
{
    p->match_case = !p->match_case;
    search_update(p, -1, TRUE);
}

static LRESULT CALLBACK find_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct pane *p = find_owner(hwnd);
    if (p) {
        if (msg == WM_KEYDOWN && wp == VK_RETURN) { search_update(p, GetKeyState(VK_SHIFT) < 0 ? 1 : -1, FALSE); return 0; }
        if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { find_close(p); return 0; }
        if ((msg == WM_SYSKEYDOWN || msg == WM_KEYDOWN) && wp == 'C' && GetKeyState(VK_MENU) < 0) { find_toggle_case(p); return 0; }
        if (msg == WM_CHAR && (wp == L'\r' || wp == 27)) return 0;
        if (msg == WM_SYSCHAR) return 0;
    }
    return CallWindowProcW(g_edit_proc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK find_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct pane *p = find_owner(hwnd);
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        HBRUSH b;
        int i;
        GetClientRect(hwnd, &r);
        b = CreateSolidBrush(STRIP_HOT); FillRect(dc, &r, b); DeleteObject(b);
        b = CreateSolidBrush(ACCENT); FrameRect(dc, &r, b); DeleteObject(b);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_ui_font);
        for (i = 0; i < FB_COUNT && p; i++) {
            RECT c = find_button(i);
            static const WCHAR *const labels[] = { L"Aa", L"\x2191", L"\x2193", L"" };
            if (i == FB_CASE && p->match_case) { b = CreateSolidBrush(ACCENT); FillRect(dc, &c, b); DeleteObject(b); }
            SetTextColor(dc, RGB(0xE8, 0xE4, 0xF0));
            if (i == FB_CLOSE) draw_glyph_x(dc, c, RGB(0xE0, 0xDC, 0xE8));
            else DrawTextW(dc, labels[i], -1, &c, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLOREDIT: {
        static HBRUSH eb;
        if (!eb) eb = CreateSolidBrush(rgb_of(SCHEME_BG));
        SetTextColor((HDC)wp, rgb_of(SCHEME_FG));
        SetBkColor((HDC)wp, rgb_of(SCHEME_BG));
        return (LRESULT)eb;
    }
    case WM_COMMAND:
        if (p && HIWORD(wp) == EN_CHANGE) {
            GetWindowTextW(p->find_edit, p->needle, ARRAYSIZE(p->needle));
            search_update(p, -1, TRUE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (p) {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            int i;
            for (i = 0; i < FB_COUNT; i++) {
                RECT c = find_button(i);
                if (!PtInRect(&c, pt)) continue;
                if (i == FB_CASE) find_toggle_case(p);
                else if (i == FB_UP) search_update(p, -1, FALSE);
                else if (i == FB_DOWN) search_update(p, 1, FALSE);
                else { find_close(p); return 0; }
                SetFocus(p->find_edit);
            }
        }
        return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void find_open(struct pane *p)
{
    if (!p) return;
    if (!p->find) {
        p->find = CreateWindowExW(0, L"SgTerminalFind", L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, p->view, NULL, g_inst, NULL);
        p->find_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, S(8), S(9), S(174), S(20),
                                       p->find, (HMENU)1, g_inst, NULL);
        SendMessageW(p->find_edit, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
        g_edit_proc = (WNDPROC)SetWindowLongPtrW(p->find_edit, GWLP_WNDPROC, (LONG_PTR)find_edit_proc);
    }
    place_find(p);
    ShowWindow(p->find, SW_SHOW);
    SetWindowTextW(p->find_edit, p->needle);
    SendMessageW(p->find_edit, EM_SETSEL, 0, -1);
    SetFocus(p->find_edit);
    search_update(p, -1, TRUE);
}

static void find_close(struct pane *p)
{
    if (!p->find) return;
    DestroyWindow(p->find);
    p->find = p->find_edit = NULL;
    InvalidateRect(p->view, NULL, FALSE);
    SetFocus(p->view);
    write_dump(TRUE);
}

/* ---- the terminal view ------------------------------------------------------------------------------ */
static uint32_t colour(const struct ts_scheme *sc, uint32_t c, BOOL fg, uint8_t flags)
{
    if (c == VT_DEFAULT) return fg ? sc->fg : sc->bg;
    if (VT_IS_RGB(c)) return c & 0xffffff;
    /* bold text in the first eight colours shows bright, as Windows Terminal does by default */
    if (fg && (flags & VT_BOLD) && c < 8) c += 8;
    return vt_palette_rgb(sc->table, (int)(c & 0xff));
}

static BOOL in_selection(const struct pane *t, int x, int y)
{
    int ax = t->sel_ax, ay = t->sel_ay, bx = t->sel_bx, by = t->sel_by;
    if (!t->sel_active) return FALSE;
    if (ay > by || (ay == by && ax > bx)) { int tx = ax, ty = ay; ax = bx; ay = by; bx = tx; by = ty; }
    if (y < ay || y > by) return FALSE;
    if (y == ay && x < ax) return FALSE;
    if (y == by && x > bx) return FALSE;
    return TRUE;
}

/* 0, or 1 a match, 2 the current match, for each cell of view row y */
static void row_highlights(const struct pane *p, int y, const struct vt_line *l, uint8_t *hl, int cols)
{
    int s[256], e[256], k, j, x, a = view_top(p) + y;
    memset(hl, 0, cols);
    if (!p->find || !p->needle[0] || !search_line(p, a)) return;
    k = line_matches(p, l, s, e, 256);
    for (j = 0; j < k; j++)
        for (x = s[j]; x < e[j] && x < cols; x++) hl[x] = (a == p->cur_line && s[j] == p->cur_col) ? 2 : 1;
}

static void paint_view(struct pane *t, HDC dc, RECT *client)
{
    const struct ts_scheme *sc = pane_scheme(t);
    struct tfont *F = t ? pane_font(t) : NULL;
    HBRUSH b = CreateSolidBrush(rgb_of(sc->bg));
    int y, x, g_cw = F ? F->cw : 8, g_ch = F ? F->ch : 16;
    static uint8_t hl[2048];
    FillRect(dc, client, b); DeleteObject(b);
    if (!t) return;
    SetBkMode(dc, OPAQUE);
    for (y = 0; y < t->vt.rows; y++) {
        const struct vt_line *l = vt_view_line(&t->vt, t->scroll, y);
        int py = S(PAD) + y * g_ch;
        if (!l) continue;
        row_highlights(t, y, l, hl, t->vt.cols < 2048 ? t->vt.cols : 2048);
        for (x = 0; x < t->vt.cols && x < l->width; ) {
            /* a run of cells drawn alike */
            const struct vt_cell *c0 = &l->cells[x];
            BOOL sel0 = in_selection(t, x, y);
            uint8_t h0 = x < 2048 ? hl[x] : 0;
            WCHAR text[1024];
            INT dx[1024];
            int n = 0, x0 = x;
            uint32_t fg, bg;
            RECT rc;
            while (x < t->vt.cols && x < l->width && n < 1000) {
                const struct vt_cell *c = &l->cells[x];
                if (c->fg != c0->fg || c->bg != c0->bg || (c->flags & ~(VT_WIDE | VT_WIDE_TAIL)) != (c0->flags & ~(VT_WIDE | VT_WIDE_TAIL)) ||
                    in_selection(t, x, y) != sel0 || (x < 2048 ? hl[x] : 0) != h0)
                    break;
                if (c->flags & VT_WIDE_TAIL) { if (n) dx[n - 1] += g_cw; x++; continue; }
                if (c->ch > 0xffff) {
                    uint32_t v = c->ch - 0x10000;
                    text[n] = 0xd800 + (v >> 10); dx[n++] = 0;
                    text[n] = 0xdc00 + (v & 0x3ff); dx[n++] = g_cw;
                } else { text[n] = c->ch ? (WCHAR)c->ch : L' '; dx[n++] = g_cw; }
                x++;
            }
            fg = colour(sc, c0->fg, TRUE, c0->flags);
            bg = colour(sc, c0->bg, FALSE, c0->flags);
            if (c0->flags & VT_REVERSE) { uint32_t s = fg; fg = bg; bg = s; }
            if (h0 == 1) { bg = SCHEME_MATCH; fg = 0xFFFFFF; }
            if (h0 == 2) { bg = SCHEME_CURMATCH; fg = 0x1D1A26; }
            if (sel0) { bg = sc->sel; fg = 0xFFFFFF; }
            if (c0->flags & VT_DIM) fg = ((fg >> 1) & 0x7f7f7f) + ((bg >> 1) & 0x7f7f7f);
            SelectObject(dc, (c0->flags & VT_BOLD) ? F->fb : F->f);
            SetTextColor(dc, rgb_of(fg));
            SetBkColor(dc, rgb_of(bg));
            SetRect(&rc, S(PAD) + x0 * g_cw, py, S(PAD) + x * g_cw, py + g_ch);
            ExtTextOutW(dc, rc.left, py, ETO_OPAQUE | ETO_CLIPPED, &rc, text, n, dx);
            if (c0->flags & (VT_UNDERLINE | VT_STRIKE)) {
                HPEN pen = CreatePen(PS_SOLID, 1, rgb_of(fg));
                HGDIOBJ o = SelectObject(dc, pen);
                int ly = (c0->flags & VT_UNDERLINE) ? py + g_ch - 2 : py + g_ch / 2;
                MoveToEx(dc, rc.left, ly, NULL); LineTo(dc, rc.right, ly);
                SelectObject(dc, o); DeleteObject(pen);
            }
        }
    }
    /* the cursor: a bar, hollow when the pane does not have the focus */
    if (t->vt.cursor_visible && !t->scroll && t->alive) {
        RECT c;
        int cx = S(PAD) + t->vt.cx * g_cw, cy = S(PAD) + t->vt.cy * g_ch;
        HBRUSH cb = CreateSolidBrush(rgb_of(sc->cursor));
        if (GetFocus() == t->view) {
            if (g_cursor_on) { SetRect(&c, cx, cy, cx + (S(2) > 1 ? S(2) : 2), cy + g_ch); FillRect(dc, &c, cb); }
        } else { SetRect(&c, cx, cy, cx + g_cw, cy + g_ch); FrameRect(dc, &c, cb); }
        DeleteObject(cb);
    }
}

static void update_scrollbar(struct pane *t)
{
    SCROLLINFO si = { sizeof(si), SIF_ALL | SIF_DISABLENOSCROLL };
    if (!t) return;
    si.nMin = 0;
    si.nMax = t->vt.sb_count + t->vt.rows - 1;
    si.nPage = t->vt.rows;
    si.nPos = t->vt.sb_count - t->scroll;
    SetScrollInfo(t->view, SB_VERT, &si, TRUE);
}

static void scroll_view(struct pane *t, int lines)
{
    int s = t->scroll + lines;
    if (s < 0) s = 0;
    if (s > t->vt.sb_count) s = t->vt.sb_count;
    if (s == t->scroll) return;
    t->scroll = s;
    t->sel_active = FALSE;
    update_scrollbar(t);
    InvalidateRect(t->view, NULL, FALSE);
    write_dump(TRUE);
}

/* the selection as text: rows joined by CR LF, a soft-wrapped row joined to the next */
static int selection_text(struct pane *t, WCHAR *out, int cap)
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

static void copy_selection(struct pane *t)
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
    InvalidateRect(t->view, NULL, FALSE);
    write_dump(TRUE);
}

static void paste(struct pane *t)
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

static void cell_at(struct pane *t, int px, int py, int *x, int *y)
{
    struct tfont *f = t ? pane_font(t) : NULL;
    *x = (px - S(PAD)) / (f ? f->cw : g_cw);
    *y = (py - S(PAD)) / (f ? f->ch : g_ch);
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

static void regrid_all(void)
{
    int i;
    for (i = 0; i < MAX_PANES; i++) if (g_panes[i].used) { g_panes[i].cols = 0; pane_grid(&g_panes[i], FALSE); InvalidateRect(g_panes[i].view, NULL, FALSE); }
    default_grid();
}

static void zoom(int delta)
{
    if (delta == 0) g_zoom = 0;
    else g_zoom += delta;
    if (g_zoom < -20) g_zoom = -20;
    if (g_zoom > 36) g_zoom = 36;
    make_fonts();
    regrid_all();
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

static void duplicate_pane(void)
{
    struct pane *f = active_pane();
    if (f) split_pane(0, f->profile, f->cmd, f->dir, 0);
}

/* shortcuts that are the terminal's, not the shell's */
/* a settings.json action; FALSE leaves the key to the shell */
static BOOL run_action(const struct ts_action *a)
{
    struct pane *t = active_pane();
    int prof = a->profile[0] ? profile_by_name(a->profile) : a->index >= 0 ? visible_profile(a->index) : -1;
    switch (a->action) {
    case A_UNBOUND: return FALSE;
    case A_NEWTAB: new_tab(prof >= 0 ? prof : g_default_profile, NULL, NULL, NULL); return TRUE;
    case A_CLOSEPANE: if (t) close_pane(t); return TRUE;
    case A_CLOSETAB: if (g_active >= 0) close_tab(g_active); return TRUE;
    case A_NEXTTAB: if (g_ntabs) activate((g_active + 1) % g_ntabs); return TRUE;
    case A_PREVTAB: if (g_ntabs) activate((g_active + g_ntabs - 1) % g_ntabs); return TRUE;
    case A_SWITCHTAB: activate(a->index); return TRUE;
    case A_SPLIT: {
        RECT r = { 0 };
        int kind = a->split == 1 ? SPLIT_V : SPLIT_H;
        if (!a->split && t) { GetClientRect(t->view, &r); kind = r.right >= r.bottom ? SPLIT_V : SPLIT_H; }
        split_pane(kind, prof >= 0 ? prof : g_default_profile, NULL, NULL, 0);
        return TRUE;
    }
    case A_DUPLICATE: duplicate_pane(); return TRUE;
    case A_MOVEFOCUS: return move_focus(a->dx, a->dy);
    case A_RESIZE: return resize_pane(a->dx, a->dy);
    case A_FIND: find_open(t); return TRUE;
    case A_COPY: if (t) copy_selection(t); return TRUE;
    case A_PASTE: paste(t); return TRUE;
    case A_SETTINGS: show_settings(); return TRUE;
    case A_FULLSCREEN: toggle_fullscreen(); return TRUE;
    case A_FONTUP: zoom(a->dx ? a->dx : 1); return TRUE;
    case A_FONTDOWN: zoom(a->dx ? a->dx : -1); return TRUE;
    case A_FONTRESET: zoom(0); return TRUE;
    case A_MENU: show_menu(); return TRUE;
    case A_SCROLLUP: if (t) scroll_view(t, 1); return TRUE;
    case A_SCROLLDOWN: if (t) scroll_view(t, -1); return TRUE;
    }
    return FALSE;
}

static BOOL shortcut(WPARAM vk)
{
    BOOL ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    struct pane *t = active_pane();
    int i, mods = (ctrl ? TS_CTRL : 0) | (shift ? TS_SHIFT : 0) | (alt ? TS_ALT : 0);
    /* the user's keys (settings.json) first; "unbound" gives a key back to the shell */
    for (i = 0; i < g_set.nactions; i++)
        if (g_set.actions[i].vk == vk && g_set.actions[i].mods == mods) return run_action(&g_set.actions[i]);
    if (ctrl && shift && !alt) {
        if (vk == 'T') { new_tab(g_default_profile, NULL, NULL, NULL); return TRUE; }
        if (vk == 'W') { if (t) close_pane(t); return TRUE; }
        if (vk == 'C') { if (t) copy_selection(t); return TRUE; }
        if (vk == 'V') { paste(t); return TRUE; }
        if (vk == 'F') { find_open(t); return TRUE; }
        if (vk == VK_TAB) { activate((g_active + g_ntabs - 1) % g_ntabs); return TRUE; }
        if (vk == VK_SPACE) { show_menu(); return TRUE; }
        if (vk >= '1' && vk <= '9') { if (visible_profile(vk - '1') >= 0) new_tab(visible_profile(vk - '1'), NULL, NULL, NULL); return TRUE; }
    }
    if (alt && shift && !ctrl) {
        if (vk == VK_OEM_PLUS || vk == VK_ADD) { split_pane(SPLIT_V, g_default_profile, NULL, NULL, 0); return TRUE; }
        if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) { split_pane(SPLIT_H, g_default_profile, NULL, NULL, 0); return TRUE; }
        if (vk == 'D') { duplicate_pane(); return TRUE; }
        if (vk == VK_LEFT || vk == VK_RIGHT) return resize_pane(vk == VK_LEFT ? -1 : 1, 0);
        if (vk == VK_UP || vk == VK_DOWN) return resize_pane(0, vk == VK_UP ? -1 : 1);
    }
    if (alt && !shift && !ctrl) {
        /* a move with nowhere to go leaves the key to the shell */
        if (vk == VK_LEFT || vk == VK_RIGHT) return move_focus(vk == VK_LEFT ? -1 : 1, 0);
        if (vk == VK_UP || vk == VK_DOWN) return move_focus(0, vk == VK_UP ? -1 : 1);
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

static struct pane *view_pane(HWND hwnd)
{
    LONG_PTR v = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    return v > 0 && v <= MAX_PANES && g_panes[v - 1].used ? &g_panes[v - 1] : NULL;
}

static LRESULT CALLBACK view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct pane *t = view_pane(hwnd), *in;
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
        paint_view(t, mem, &r);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE: if (t) { pane_grid(t, FALSE); update_scrollbar(t); place_find(t); } InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_SETFOCUS:
        if (t && t->tab >= 0 && g_tabs[t->tab].focus != pane_slot(t)) {
            g_tabs[t->tab].focus = pane_slot(t);
            update_title();
            InvalidateRect(g_wnd, NULL, FALSE);
            write_dump(TRUE);
        }
        /* fall through */
    case WM_KILLFOCUS: g_cursor_on = TRUE; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS | DLGC_WANTTAB;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        char buf[32];
        const char *seq;
        if (shortcut(wp)) {
            /* the key was the terminal's: its character must not reach the shell too */
            MSG m;
            while (PeekMessageW(&m, NULL, WM_CHAR, WM_CHAR, PM_REMOVE)) ;
            while (PeekMessageW(&m, NULL, WM_SYSCHAR, WM_SYSCHAR, PM_REMOVE)) ;
            return 0;
        }
        if (!t || !view_pane(hwnd)) return 0;
        in = input_pane(t);
        if (!in->alive) {
            /* the process has ended: Enter starts it again, Ctrl+D closes the pane */
            if (wp == VK_RETURN) { vt_write(&in->vt, "\r\n", 2); spawn(in); InvalidateRect(g_wnd, NULL, FALSE); }
            else if (wp == 'D' && GetKeyState(VK_CONTROL) < 0) close_pane(in);
            return 0;
        }
        if ((seq = key_sequence(wp, in->vt.app_cursor_keys, buf))) {
            in->scroll = 0; in->sel_active = FALSE;
            pty_send(in, seq, (int)strlen(seq));
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
                pty_send(in, out, n + 1);
                return 0;
            }
        }
        break;
    }
    case WM_CHAR: {
        WCHAR c = (WCHAR)wp;
        if (!t) return 0;
        in = input_pane(t);
        if (!in->alive) return 0;
        in->scroll = 0;
        in->sel_active = FALSE;
        if (c == 8) { pty_send(in, "\x7f", 1); return 0; }     /* Backspace is DEL, as on a VT */
        if (c == L'\r') { pty_send(in, "\r", 1); return 0; }
        pty_send_w(in, &c, 1);
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
            cell_at(t, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &x, &y);
            t->sel_ax = t->sel_bx = x; t->sel_ay = t->sel_by = y;
            t->sel_active = FALSE; t->selecting = TRUE;
            SetCapture(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (t && t->selecting) {
            int x, y;
            cell_at(t, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &x, &y);
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
            cell_at(t, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &x, &y);
            if (!(l = vt_view_line(&t->vt, t->scroll, y)) || x >= l->width || !word_char(l->cells[x].ch)) return 0;
            for (a = x; a > 0 && word_char(l->cells[a - 1].ch); a--) ;
            for (b = x; b + 1 < t->vt.cols && b + 1 < l->width && word_char(l->cells[b + 1].ch); b++) ;
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
enum { M_PROFILE = 100, M_SETTINGS = 200, M_ABOUT, M_SPLIT_RIGHT, M_SPLIT_DOWN, M_DUPLICATE, M_FIND, M_CLOSE_PANE };

static int g_menu_items[MAX_PROFILES], g_nmenu;
static BOOL g_menu_open;

static void show_menu(void)
{
    HMENU m = CreatePopupMenu();
    POINT pt = { g_menu_rect.left, g_menu_rect.bottom };
    int i, cmd;
    g_nmenu = 0;
    for (i = 0; i < g_nprofiles; i++) {
        WCHAR label[96];
        if (g_profiles[i].hidden) continue;
        if (g_nmenu < 9) _snwprintf(label, ARRAYSIZE(label), L"%ls\tCtrl+Shift+%d", g_profiles[i].name, g_nmenu + 1);
        else lstrcpynW(label, g_profiles[i].name, ARRAYSIZE(label));
        AppendMenuW(m, MF_STRING, M_PROFILE + i, label);
        g_menu_items[g_nmenu++] = i;
    }
    g_menu_open = TRUE;
    write_dump(TRUE);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, M_SPLIT_RIGHT, L"Split pane right\tAlt+Shift+Plus");
    AppendMenuW(m, MF_STRING, M_SPLIT_DOWN, L"Split pane down\tAlt+Shift+Minus");
    AppendMenuW(m, MF_STRING, M_DUPLICATE, L"Duplicate pane\tAlt+Shift+D");
    AppendMenuW(m, MF_STRING, M_CLOSE_PANE, L"Close pane\tCtrl+Shift+W");
    AppendMenuW(m, MF_STRING, M_FIND, L"Find\tCtrl+Shift+F");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, M_SETTINGS, L"Settings\tCtrl+,");
    AppendMenuW(m, MF_STRING, M_ABOUT, L"About");
    ClientToScreen(g_wnd, &pt);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g_wnd, NULL);
    DestroyMenu(m);
    g_menu_open = FALSE;
    if (cmd >= M_PROFILE && cmd < M_PROFILE + g_nprofiles) new_tab(cmd - M_PROFILE, NULL, NULL, NULL);
    else if (0) ;
    else if (cmd == M_SPLIT_RIGHT) split_pane(SPLIT_V, g_default_profile, NULL, NULL, 0);
    else if (cmd == M_SPLIT_DOWN) split_pane(SPLIT_H, g_default_profile, NULL, NULL, 0);
    else if (cmd == M_DUPLICATE) duplicate_pane();
    else if (cmd == M_CLOSE_PANE) { if (active_pane()) close_pane(active_pane()); }
    else if (cmd == M_FIND) find_open(active_pane());
    else if (cmd == M_SETTINGS) show_settings();
    else if (cmd == M_ABOUT)
        MessageBoxW(g_wnd, L"Terminal\n\nTabs and panes of shells on pseudo consoles, for Stained Glass OS.\n"
                    L"Free software under the AGPL.", L"About", MB_OK | MB_ICONINFORMATION);
}

/* Settings: the default profile, the font and its size -- kept in settings.json (wtsettings.c) */
static HWND g_set_dlg;

/* the settings (re)read: profiles, schemes, keys, fonts; panes keep their profiles by GUID */
static void apply_settings(void)
{
    int i, j;
    load_profiles();
    for (i = 0; i < MAX_PANES; i++) {
        struct pane *p = &g_panes[i];
        if (!p->used) continue;
        for (j = 0; j < g_nprofiles && lstrcmpiW(g_profiles[j].guid, p->guid); j++) ;
        p->profile = j < g_nprofiles ? j : g_default_profile;
    }
    make_fonts();
    regrid_all();
    for (i = 0; i < MAX_PANES; i++) if (g_panes[i].used) InvalidateRect(g_panes[i].view, NULL, FALSE);
    InvalidateRect(g_wnd, NULL, FALSE);
    write_dump(TRUE);
}
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
            /* into settings.json, keeping everything else there; then applied as any edit of it is */
            if (!ts_save_prefs(&g_set, p >= 0 && p < g_nprofiles ? g_profiles[p].guid : NULL, face, pt))
                MessageBoxW(hwnd, g_set.error[0] ? g_set.error : L"The settings file could not be written.", L"Settings", MB_OK | MB_ICONWARNING);
            apply_settings();
            EnableWindow(g_wnd, TRUE);      /* before it goes, or another program's window is activated */
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) { EnableWindow(g_wnd, TRUE); DestroyWindow(hwnd); return 0; }
        if (LOWORD(wp) == 13) {
            /* the JSON file itself, in the editor for .json (Notepad) */
            WCHAR cmd[MAX_PATH + 4];
            _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\"", g_set.path);
            if ((INT_PTR)ShellExecuteW(hwnd, L"open", L"notepad.exe", cmd, NULL, SW_SHOWNORMAL) <= 32)
                ShellExecuteW(hwnd, L"open", g_set.path, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        break;
    case WM_CLOSE: EnableWindow(g_wnd, TRUE); DestroyWindow(hwnd); return 0;
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
                                WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, S(400), S(330), g_wnd, NULL, g_inst, NULL);
#define ADD(cls, txt, style, X, Y, W, H, id) do { c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | (style), X, Y, W, H, g_set_dlg, (HMENU)(INT_PTR)(id), g_inst, NULL); \
        SendMessageW(c, WM_SETFONT, (WPARAM)g_ui_font, TRUE); } while (0)
    ADD(L"STATIC", L"Default profile", 0, x, y, w, S(20), -1); y += S(22);
    ADD(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, S(200), 10); y += S(36);
    for (i = 0; i < g_nprofiles; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)g_profiles[i].name);
    SendMessageW(c, CB_SETCURSEL, g_default_profile, 0);
    ADD(L"STATIC", L"Font face", 0, x, y, w, S(20), -1); y += S(22);
    ADD(L"EDIT", g_face, WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, x, y, w, S(26), 11); y += S(36);
    ADD(L"STATIC", L"Font size", 0, x, y, w, S(20), -1); y += S(22);
    _snwprintf(size, ARRAYSIZE(size), L"%d", g_set.size ? g_set.size : 12);
    ADD(L"EDIT", size, WS_TABSTOP | WS_BORDER | ES_NUMBER, x, y, S(80), S(26), 12); y += S(34);
    ADD(L"STATIC", g_set.error[0] ? g_set.error : g_set.path, SS_PATHELLIPSIS, x, y, w, S(20), -1); y += S(30);
    ADD(L"BUTTON", L"Open JSON file", WS_TABSTOP, x, y, S(130), S(30), 13);
    ADD(L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, x + w - S(200), y, S(96), S(30), IDOK);
    ADD(L"BUTTON", L"Cancel", WS_TABSTOP, x + w - S(96), y, S(96), S(30), IDCANCEL);
#undef ADD
    EnableWindow(g_wnd, FALSE);
    ShowWindow(g_set_dlg, SW_SHOW);
    write_dump(TRUE);
}

/* ---- the dump, for the gate ------------------------------------------------------------------------------- */
static unsigned long long dump_generation(void)
{
    struct tab *t = active_tab();
    int all[MAX_PANES], n, i;
    unsigned long long g = 0;
    if (!t) return 0;
    n = leaves(t->root, all, 0);
    for (i = 0; i < n; i++) g += g_panes[all[i]].vt.generation * (i + 1);
    return g;
}

static void write_dump(BOOL force)
{
    static WCHAR path[MAX_PATH];
    static int have = -1;
    WCHAR tmp[MAX_PATH + 8];
    FILE *f;
    struct tab *tb = active_tab();
    struct pane *t = active_pane();
    RECT wr;
    int i, y, all[MAX_PANES], np = 0;
    unsigned long long gen;
    char line[4096];
    if (have < 0) have = GetEnvironmentVariableW(L"SG_TERMINAL_DUMP", path, MAX_PATH) > 0;
    if (!have || !g_wnd) return;
    gen = dump_generation();
    if (!force && gen == g_last_dump_gen) return;
    g_last_dump_gen = gen;
    _snwprintf(tmp, ARRAYSIZE(tmp), L"%ls.tmp", path);
    if (!(f = _wfopen(tmp, L"wb"))) return;
    GetWindowRect(g_wnd, &wr);
    {
        WCHAR title[256];
        GetWindowTextW(g_wnd, title, ARRAYSIZE(title));
        WideCharToMultiByte(CP_UTF8, 0, title, -1, line, sizeof(line), NULL, NULL);
        fprintf(f, "window %s\nrect %ld %ld %ld %ld\nsize %d %d\n", line, wr.left, wr.top, wr.right, wr.bottom,
                t ? t->vt.cols : g_cols, t ? t->vt.rows : g_rows);
        WideCharToMultiByte(CP_UTF8, 0, g_face, -1, line, sizeof(line), NULL, NULL);
        fprintf(f, "font %d %d %d %s\n", g_font_pt, g_cw, g_ch, line);
    }
    WideCharToMultiByte(CP_UTF8, 0, g_set.path, -1, line, sizeof(line), NULL, NULL);
    fprintf(f, "settings %s\n", line);
    WideCharToMultiByte(CP_UTF8, 0, g_set.error, -1, line, sizeof(line), NULL, NULL);
    fprintf(f, "settingserror %s\n", line);
    fprintf(f, "actions %d\n", g_set.nactions);
    for (i = 0; i < g_nprofiles; i++) {
        char guid[64], dir[MAX_PATH * 3], sch[128];
        WideCharToMultiByte(CP_UTF8, 0, g_profiles[i].name, -1, line, sizeof(line), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, g_profiles[i].guid, -1, guid, sizeof(guid), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, g_profiles[i].dir, -1, dir, sizeof(dir), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, ts_scheme(&g_set, g_profiles[i].scheme)->name, -1, sch, sizeof(sch), NULL, NULL);
        fprintf(f, "profile %d %s%s\n", i + 1, line, i == g_default_profile ? " (default)" : "");
        fprintf(f, "profileinfo %d guid=%s hidden=%d user=%d icon=%d scheme=%s font=%s,%d dir=%s\n", i + 1, guid,
                g_profiles[i].hidden, g_profiles[i].user, g_icons[i] != NULL, sch, "", profile_font(i)->pt, dir);
    }
    if (g_set_dlg) {
        static const int ids[] = { 10, 11, 12, 13, IDOK, IDCANCEL };
        size_t k;
        for (k = 0; k < ARRAYSIZE(ids); k++) {
            HWND c = GetDlgItem(g_set_dlg, ids[k]);
            RECT cr;
            if (!c) continue;
            GetWindowRect(c, &cr);
            fprintf(f, "setctl %d %ld %ld\n", ids[k], (cr.left + cr.right) / 2, (cr.top + cr.bottom) / 2);
        }
    }
    fprintf(f, "menuopen %d\n", g_menu_open);
    for (i = 0; i < g_nmenu && g_menu_open; i++) {
        WideCharToMultiByte(CP_UTF8, 0, g_profiles[g_menu_items[i]].name, -1, line, sizeof(line), NULL, NULL);
        fprintf(f, "menuitem %d %s\n", i + 1, line);
    }
    fprintf(f, "tabs %d active %d\n", g_ntabs, g_active + 1);
    for (i = 0; i < g_ntabs; i++) {
        struct tab *u = &g_tabs[g_order[i]];
        struct pane *up = &g_panes[u->focus];
        POINT c = { (g_tab_rects[i].left + g_tab_rects[i].right) / 2, (g_tab_rects[i].top + g_tab_rects[i].bottom) / 2 };
        char title[512], prof[128];
        int k[MAX_PANES];
        ClientToScreen(g_wnd, &c);
        WideCharToMultiByte(CP_UTF8, 0, tab_title(u), -1, title, sizeof(title), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, g_profiles[up->profile].name, -1, prof, sizeof(prof), NULL, NULL);
        fprintf(f, "tab %d at=%ld,%ld profile=%s alive=%d exit=%lu panes=%d: %s\n", i + 1, c.x, c.y, prof, up->alive, up->exit_code,
                leaves(u->root, k, 0), title);
    }
    {
        POINT a = { (g_plus_rect.left + g_plus_rect.right) / 2, (g_plus_rect.top + g_plus_rect.bottom) / 2 };
        POINT b = { (g_menu_rect.left + g_menu_rect.right) / 2, (g_menu_rect.top + g_menu_rect.bottom) / 2 };
        POINT v = { S(PAD), S(PAD) };
        ClientToScreen(g_wnd, &a); ClientToScreen(g_wnd, &b);
        if (t) ClientToScreen(t->view, &v);
        fprintf(f, "plus %ld %ld\nmenu %ld %ld\norigin %ld %ld\n", a.x, a.y, b.x, b.y, v.x, v.y);
    }
    /* the active tab's panes: each one's place, grid and shown rows */
    if (tb) np = leaves(tb->root, all, 0);
    fprintf(f, "panes %d\n", np);
    for (i = 0; i < np; i++) {
        struct pane *p = &g_panes[all[i]];
        RECT r;
        char title[512], prof[128];
        GetWindowRect(p->view, &r);
        WideCharToMultiByte(CP_UTF8, 0, p->title, -1, title, sizeof(title), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, g_profiles[p->profile].name, -1, prof, sizeof(prof), NULL, NULL);
        fprintf(f, "pane %d id=%d at=%ld,%ld rect=%ld,%ld,%ld,%ld size=%dx%d profile=%s alive=%d focus=%d bg=%06x font=%d: %s\n", i + 1, p->id,
                (r.left + r.right) / 2, (r.top + r.bottom) / 2, r.left, r.top, r.right, r.bottom, p->vt.cols, p->vt.rows, prof,
                p->alive, p == t, pane_scheme(p)->bg, pane_font(p)->pt, title);
    }
    /* the dividers of the active tab's splits, on the screen */
    for (i = 0; tb && i < MAX_NODES; i++) {
        struct node *nd = &g_nodes[i];
        POINT c;
        int k, r2 = tb->root, up;
        if (!nd->used || nd->kind == NODE_LEAF) continue;
        for (up = i; up >= 0 && up != r2; up = g_nodes[up].parent) ;
        if (up != r2) continue;
        c.x = (nd->gap.left + nd->gap.right) / 2; c.y = (nd->gap.top + nd->gap.bottom) / 2;
        ClientToScreen(g_wnd, &c);
        k = nd->kind == SPLIT_V;
        fprintf(f, "divider %d %s %ld %ld ratio=%d\n", i, k ? "v" : "h", c.x, c.y, nd->ratio);
    }
    for (i = 0; i < np; i++) {
        struct pane *p = &g_panes[all[i]];
        for (y = 0; y < p->vt.rows; y++) {
            vt_line_text(vt_view_line(&p->vt, p->scroll, y), p->vt.cols, line, sizeof(line));
            if (line[0]) fprintf(f, "prow %d %d: %s\n", p->id, y, line);
        }
    }
    if (t) {
        static WCHAR sel[8192];
        fprintf(f, "focus %d\ncursor %d %d %d\nscroll %d %d\n", t->id, t->vt.cx, t->vt.cy, t->vt.cursor_visible, t->scroll, t->vt.sb_count);
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
        /* the search: its state, the current match (absolute line and, when shown, its view row) */
        WideCharToMultiByte(CP_UTF8, 0, t->needle, -1, line, sizeof(line), NULL, NULL);
        fprintf(f, "search open=%d case=%d matches=%d current=%d line=%d col=%d viewrow=%d needle=%s\n", t->find != NULL,
                t->match_case, t->nmatches, t->cur_index, t->cur_line, t->cur_line >= 0 ? t->cur_col : -1,
                t->cur_line >= view_top(t) && t->cur_line < view_top(t) + t->vt.rows ? t->cur_line - view_top(t) : -1, line);
        if (t->find) {
            RECT er;
            GetWindowRect(t->find_edit, &er);
            fprintf(f, "findbox %ld %ld\n", (er.left + er.right) / 2, (er.top + er.bottom) / 2);
            for (i = 0; i < FB_COUNT; i++) {
                RECT c = find_button(i);
                POINT pt = { (c.left + c.right) / 2, (c.top + c.bottom) / 2 };
                ClientToScreen(t->find, &pt);
                fprintf(f, "findbutton %s %ld %ld\n", (const char *[]){ "case", "up", "down", "close" }[i], pt.x, pt.y);
            }
        }
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
    case WM_SIZE:
        layout_tabs();
        layout_panes();
        default_grid();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
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
        paint_area(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && active_tab()) {
            POINT pt;
            int n;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            if (g_drag >= 0 || (n = divider_at(active_tab()->root, pt)) >= 0) {
                n = g_drag >= 0 ? g_drag : n;
                SetCursor(LoadCursorW(NULL, (LPCWSTR)(g_nodes[n].kind == SPLIT_V ? IDC_SIZEWE : IDC_SIZENS)));
                return TRUE;
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (active_tab() && (g_drag = divider_at(active_tab()->root, pt)) >= 0) SetCapture(hwnd);
        return 0;
    }
    case WM_CAPTURECHANGED: g_drag = -1; return 0;
    case WM_MOUSEMOVE: {
        BOOL on_close;
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_drag >= 0) { drag_divider(g_drag, pt); return 0; }
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
        if (g_drag >= 0) {
            g_drag = -1;
            ReleaseCapture();
            if (active_pane()) SetFocus(active_pane()->view);
            write_dump(TRUE);
            return 0;
        }
        if (t >= 0) { if (on_close) close_tab(t); else activate(t); }
        else if (PtInRect(&g_plus_rect, pt)) new_tab(g_default_profile, NULL, NULL, NULL);
        else if (PtInRect(&g_menu_rect, pt)) show_menu();
        if (active_pane() && IsWindow(active_pane()->view)) SetFocus(active_pane()->view);
        return 0;
    }
    case WM_MBUTTONUP: {
        BOOL on_close;
        int t = tab_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &on_close);
        if (t >= 0) close_tab(t);
        return 0;
    }
    case WM_SETFOCUS: if (active_pane()) SetFocus(active_pane()->view); return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && active_pane()) {
            struct pane *p = active_pane();
            SetFocus(p->find ? p->find_edit : p->view);
        }
        return 0;
    case WM_PTY_OUTPUT: {
        struct chunk *c = (struct chunk *)lp;
        struct pane *t = pane_by_id(c->id);
        if (t) {
            vt_write(&t->vt, c->data, c->len);
            if (t->vt.reply_len) { pty_send(t, t->vt.reply, t->vt.reply_len); t->vt.reply_len = 0; }
            if (t->vt.title_changed) {
                t->vt.title_changed = 0;
                {
                    int len;
                    MultiByteToWideChar(CP_UTF8, 0, t->vt.title, -1, t->title, ARRAYSIZE(t->title));
                    /* a shell that names only a prefix ("Administrator: " and an empty console
                     * title) gets the profile's name after it */
                    len = lstrlenW(t->title);
                    if (!len || (len >= 2 && t->title[len - 2] == L':' && t->title[len - 1] == L' '))
                        wcsncat(t->title, g_profiles[t->profile].name, ARRAYSIZE(t->title) - len - 1);
                    if (t == active_pane()) update_title();
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            if (t->vt.bell) { t->vt.bell = 0; MessageBeep(MB_OK); }
            t->sel_active = t->sel_active && t->selecting;
            if (t->find && t->needle[0]) count_matches(t);
            InvalidateRect(t->view, NULL, FALSE);
            update_scrollbar(t);
        }
        free(c);
        return 0;
    }
    case WM_PTY_EXIT: {
        struct pane *t = pane_by_id((int)wp);
        if (!t || !t->alive || !t->process) return 0;
        GetExitCodeProcess(t->process, &t->exit_code);
        t->alive = FALSE;
        if (t->pc) { CloseHandle(CreateThread(NULL, 0, closer_thread, t->pc, 0, NULL)); t->pc = NULL; }
        if (t->in_w) { CloseHandle(t->in_w); t->in_w = NULL; }
        CloseHandle(t->process); t->process = NULL;
        if (!t->exit_code) { close_pane(t); return 0; }     /* a graceful exit closes the pane, as Windows Terminal's default */
        {
            char msg[160];
            int n = snprintf(msg, sizeof(msg), "\r\n\x1b[m[process exited with code %lu (0x%08lx)]\r\n"
                             "You can now close this terminal with Ctrl+D, or press Enter to restart.\r\n",
                             t->exit_code, t->exit_code);
            vt_write(&t->vt, msg, n);
        }
        InvalidateRect(t->view, NULL, FALSE);
        InvalidateRect(hwnd, NULL, FALSE);
        write_dump(TRUE);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_BLINK) { g_cursor_on = !g_cursor_on; if (active_pane()) InvalidateRect(active_pane()->view, NULL, FALSE); }
        else if (wp == TIMER_DUMP) write_dump(FALSE);
        else if (wp == TIMER_SETTINGS && ts_file_stamp(&g_set) != g_set_stamp) apply_settings();     /* the file was edited */
        return 0;
    case WM_SYSCOMMAND:
        /* Alt pressed and released on its own would enter the window menu and eat the next keys */
        if ((wp & 0xfff0) == SC_KEYMENU && !lp) return 0;
        break;
    case WM_GETMINMAXINFO: { MINMAXINFO *mm = (MINMAXINFO *)lp; mm->ptMinTrackSize.x = S(360); mm->ptMinTrackSize.y = S(200); return 0; }
    case WM_CLOSE:
        if (g_ntabs > 1 && !GetEnvironmentVariableW(L"SG_TERMINAL_DUMP", NULL, 0) &&
            MessageBoxW(hwnd, L"Do you want to close all tabs?", L"Close all tabs?", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return 0;
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: {
        int i;
        for (i = 0; i < MAX_PANES; i++) if (g_panes[i].used && g_panes[i].alive && g_panes[i].process) TerminateProcess(g_panes[i].process, 1);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- wt.exe's command line ------------------------------------------------------------------------------- */
enum { REQ_TAB, REQ_SPLIT, REQ_FOCUS, REQ_FOCUS_TAB };
struct request { int kind, profile, split, share, dx, dy, index; WCHAR cmd[1024], dir[MAX_PATH], title[160]; };

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

static BOOL direction(const WCHAR *a, int *dx, int *dy)
{
    *dx = *dy = 0;
    if (!lstrcmpiW(a, L"left")) *dx = -1;
    else if (!lstrcmpiW(a, L"right")) *dx = 1;
    else if (!lstrcmpiW(a, L"up")) *dy = -1;
    else if (!lstrcmpiW(a, L"down")) *dy = 1;
    else return FALSE;
    return TRUE;
}

static int parse_command_line(int argc, WCHAR **argv, struct request *req, int max, BOOL *maximized, BOOL *full)
{
    int n = 0, i = 1;
    while (i <= argc && n < max) {
        struct request *r = &req[n];
        memset(r, 0, sizeof(*r));
        r->profile = -1;
        r->kind = REQ_TAB;
        if (i < argc && (!lstrcmpiW(argv[i], L"new-tab") || !lstrcmpiW(argv[i], L"nt"))) i++;
        else if (i < argc && (!lstrcmpiW(argv[i], L"split-pane") || !lstrcmpiW(argv[i], L"sp"))) { r->kind = REQ_SPLIT; i++; }
        else if (i < argc && (!lstrcmpiW(argv[i], L"move-focus") || !lstrcmpiW(argv[i], L"mf"))) {
            r->kind = REQ_FOCUS;
            if (++i < argc && direction(argv[i], &r->dx, &r->dy)) i++;
            if (i < argc && !lstrcmpW(argv[i], L";")) i++;
            n++;
            if (i >= argc) break;
            continue;
        } else if (i < argc && (!lstrcmpiW(argv[i], L"focus-tab") || !lstrcmpiW(argv[i], L"ft"))) {
            r->kind = REQ_FOCUS_TAB;
            if (++i + 1 < argc && (!lstrcmpW(argv[i], L"-t") || !lstrcmpiW(argv[i], L"--target"))) { r->index = _wtoi(argv[i + 1]); i += 2; }
            if (i < argc && !lstrcmpW(argv[i], L";")) i++;
            n++;
            if (i >= argc) break;
            continue;
        }
        for (; i < argc; i++) {
            const WCHAR *a = argv[i];
            if (!lstrcmpW(a, L";")) break;
            if ((!lstrcmpW(a, L"-p") || !lstrcmpiW(a, L"--profile")) && i + 1 < argc) { r->profile = profile_by_name(argv[++i]); continue; }
            if ((!lstrcmpW(a, L"-d") || !lstrcmpiW(a, L"--startingDirectory")) && i + 1 < argc) { lstrcpynW(r->dir, argv[++i], MAX_PATH); continue; }
            if (!lstrcmpiW(a, L"--title") && i + 1 < argc) { lstrcpynW(r->title, argv[++i], ARRAYSIZE(r->title)); continue; }
            if ((!lstrcmpW(a, L"-w") || !lstrcmpiW(a, L"--window")) && i + 1 < argc) { i++; continue; }
            if (!lstrcmpW(a, L"-M") || !lstrcmpiW(a, L"--maximized")) { *maximized = TRUE; continue; }
            if (!lstrcmpW(a, L"-F") || !lstrcmpiW(a, L"--fullscreen")) { *full = TRUE; continue; }
            if (r->kind == REQ_SPLIT) {
                /* -H: the new pane below; -V: to the right; -s: its share of the space */
                if (!lstrcmpW(a, L"-H") || !lstrcmpiW(a, L"--horizontal")) { r->split = SPLIT_H; continue; }
                if (!lstrcmpW(a, L"-V") || !lstrcmpiW(a, L"--vertical")) { r->split = SPLIT_V; continue; }
                if ((!lstrcmpW(a, L"-s") || !lstrcmpiW(a, L"--size")) && i + 1 < argc) { r->share = (int)(_wtof(argv[++i]) * 1000); continue; }
                if (!lstrcmpW(a, L"-D") || !lstrcmpiW(a, L"--duplicate")) { r->profile = -2; continue; }
            }
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

static void run_request(const struct request *r)
{
    switch (r->kind) {
    case REQ_TAB: new_tab(r->profile, r->cmd, r->dir, r->title); break;
    case REQ_SPLIT:
        if (!g_ntabs) { new_tab(r->profile >= 0 ? r->profile : -1, r->cmd, r->dir, r->title); break; }
        if (r->profile == -2) {         /* --duplicate */
            struct pane *f = active_pane();
            if (f) split_pane(r->split, f->profile, f->cmd, f->dir, r->share);
        } else split_pane(r->split, r->profile, r->cmd, r->dir, r->share);
        break;
    case REQ_FOCUS: move_focus(r->dx, r->dy); break;
    case REQ_FOCUS_TAB: activate(r->index); break;
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    WNDCLASSW wc = { 0 };
    int argc = 0, i, n;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    static struct request req[16];
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
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);     /* CoCreateGuid, ShellExecute */
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
    wc.lpfnWndProc = find_proc; wc.lpszClassName = L"SgTerminalFind";
    RegisterClassW(&wc);
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
    default_grid();
    for (i = 0; i < n; i++) run_request(&req[i]);
    if (!g_ntabs) new_tab(g_default_profile, NULL, NULL, NULL);
    for (i = 0; i < n && req[i].kind != REQ_FOCUS_TAB; i++) ;
    if (i == n) activate(0);        /* the first tab in front, unless focus-tab named one */
    ShowWindow(g_wnd, maximized ? SW_SHOWMAXIMIZED : show ? show : SW_SHOWNORMAL);
    UpdateWindow(g_wnd);
    if (full) toggle_fullscreen();
    if (active_pane()) SetFocus(active_pane()->view);
    SetTimer(g_wnd, TIMER_BLINK, GetCaretBlinkTime() != INFINITE ? GetCaretBlinkTime() : 530, NULL);
    SetTimer(g_wnd, TIMER_DUMP, 200, NULL);
    SetTimer(g_wnd, TIMER_SETTINGS, 1000, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_set_dlg && IsDialogMessageW(g_set_dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
