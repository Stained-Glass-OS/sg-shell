/* sg-dictate -- voice typing: the dictation toolbar (Windows' Win+H).
 *
 * A small dark bar at the top of the screen: the microphone button, what is
 * happening ("Listening..."), a level ring around the microphone while it
 * listens, a gear for Control Panel > Speech, and close. It never takes the
 * focus: it is WS_EX_NOACTIVATE, so the program being typed into keeps it,
 * and what is said arrives there as typing (SendInput KEYEVENTF_UNICODE) or,
 * if the user chose it, as a paste.
 *
 * Recognition is native (sg-session's sg-dictate: Parakeet on the CPU). Wine
 * gives a Windows program no AF_UNIX and no pipes to a native one, so this
 * re-launches itself as `sg-dictate --bridge wine <itself> --bridged ...`:
 * the engine starts it with pipes as its standard handles, as sg-netctl's
 * bridge does for the network programs.
 *
 *   sg-dictate [/toggle]     open the bar and listen; if it is open, stop
 *                            and close it (explorer's Win+H runs this)
 *   sg-dictate /background   stay resident for hold-to-talk, if it is on
 *   sg-dictate /reload       tell a running one the settings changed
 *
 * One instance: a second one hands its command to the first (WM_COPYDATA)
 * and exits.
 *
 * Settings: HKCU\Software\Stained Glass\Speech (Control Panel > Speech).
 * Engine -> bar: STATE <what> [detail], LEVEL <0-100>, TEXT <JSON string>,
 * PARTIAL <JSON string> (what is being said, shown in the bar and never
 * typed; "" clears it; TEXT replaces it), CMD delete|undo (spoken
 * commands: take back the last text typed, or press Ctrl+Z).
 * Bar -> engine: one JSON object a line, {"cmd": "start", ...}, stop,
 * cancel, preload, unload, quit.
 *
 * SG_DICTATE_DUMP=<file> (gates only) writes the state and the partial text
 * shown after every change. Nothing else ever records what was said.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include "sg-mode.h"
#include "sg-smooth.h"
#include <shellapi.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#define BAR_W 360
#define BAR_W_WIDE 600  /* while it shows what is being said */
#define BAR_H 56
#define MIC_R 18
#define WM_ENGINE (WM_APP + 1)
#define WM_ENGINE_GONE (WM_APP + 2)
#define WM_HOLD_RELEASED (WM_APP + 3)
#define TIMER_HOLD 1
#define TIMER_UNLOAD 2
#define TIMER_ANIM 3
#define TIMER_FLUSH 4
/* After the hold key's release, before what was heard is typed: the release
 * reaches the program first -- typed at once with Alt as the hold key, the
 * words went in as Alt+letters, menu keys (David 2026-10-02). */
#define FLUSH_DELAY_MS 150
#define HOLD_DELAY_MS 250
#define UNLOAD_AFTER_MS (5 * 60 * 1000)
#define CLASS_NAME L"SgDictateBar"
#define MUTEX_NAME L"Local\\StainedGlassVoiceTyping"
#define SPEECH_KEY L"Software\\Stained Glass\\Speech"

#define COL_BG      RGB(0x1F, 0x1F, 0x1F)
#define COL_EDGE    RGB(0x3A, 0x3A, 0x3A)
#define COL_TEXT    RGB(0xFF, 0xFF, 0xFF)
#define COL_SUBTLE  RGB(0xA8, 0xA8, 0xA8)
#define COL_ACCENT  (sg_accent())
#define COL_RING    RGB(0xB9, 0x8C, 0xF0)
#define COL_IDLE    RGB(0x44, 0x44, 0x44)
#define COL_HOVER   RGB(0x33, 0x33, 0x33)

enum state { ST_IDLE, ST_LOADING, ST_LISTENING, ST_NOMODEL, ST_OFF, ST_NOMIC, ST_ERROR, ST_NOENGINE, ST_PRIVACY };
enum hot { HOT_NONE, HOT_GEAR, HOT_MIC, HOT_CLOSE };

struct settings {
    DWORD enabled, continuous, spoken, autopunct, fillers, numbers, paste, hold, holdkey;
    WCHAR mic[256], language[16];
};

static HWND g_wnd;
static HANDLE g_in, g_out;
static BOOL g_bridged, g_background, g_visible, g_closing, g_hold_active, g_hold_pending;
static BOOL g_hold_shown;       /* the hold key opened the bar: it goes when done */
static BOOL g_hold_down;        /* the hold key is down: what is heard waits for its release */
static WCHAR *g_held_text;      /* ...here */
static enum state g_state = ST_IDLE;
static enum hot g_hot;
static int g_level;
static double g_ring;           /* the ring's radius beyond the button, eased */
static HWND g_last_target;      /* where the last text went */
static int g_last_len;          /* how many characters that was, for "delete that" */
static WCHAR *g_partial;        /* what is being said, not yet final: shown, never typed */
static int g_bar_w = BAR_W;
static HFONT g_font, g_font_small, g_font_partial;
static struct settings g_set;
static HHOOK g_kbhook;
static CRITICAL_SECTION g_out_lock;

/* Diagnostics for the gates, to stderr (the bridge passes it through). Never
 * what was said. */
static void report(const char *fmt, ...)
{
    char buf[512];
    DWORD n;
    va_list ap;
    int len;
    va_start(ap, fmt);
    len = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (len < 0) return;
    if (len >= (int)sizeof(buf)) len = sizeof(buf) - 1;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, len, &n, NULL);
}

/* ---- settings ---------------------------------------------------------------------- */

static DWORD reg_dword(HKEY k, const WCHAR *name, DWORD def)
{
    DWORD v, sz = sizeof(v), type;
    if (k && !RegQueryValueExW(k, name, NULL, &type, (BYTE *)&v, &sz) && type == REG_DWORD) return v;
    return def;
}

static void load_settings(void)
{
    HKEY k = NULL;
    DWORD sz = sizeof(g_set.mic), type;
    RegOpenKeyExW(HKEY_CURRENT_USER, SPEECH_KEY, 0, KEY_READ, &k);
    g_set.enabled = reg_dword(k, L"Enabled", 0);
    g_set.continuous = reg_dword(k, L"Continuous", 1);
    g_set.spoken = reg_dword(k, L"SpokenPunctuation", 0);  /* the model punctuates */
    g_set.autopunct = reg_dword(k, L"AutoPunctuation", 1);
    g_set.fillers = reg_dword(k, L"RemoveFillers", 1);
    g_set.numbers = reg_dword(k, L"FormatNumbers", 1);
    g_set.paste = reg_dword(k, L"InsertMethod", 0) == 1;
    g_set.hold = reg_dword(k, L"HoldToTalk", 0);
    g_set.holdkey = reg_dword(k, L"HoldKey", VK_RCONTROL);
#ifndef SG_MUTANT_ALT_HOLD
    /* Alt cannot be the key: pressed and released, a program's menu bar
     * takes the keyboard and the words went into the menu (David
     * 2026-10-02). Speech Recognition no longer offers it; an Alt kept from
     * before is Right Ctrl. */
    if (g_set.holdkey == VK_MENU || g_set.holdkey == VK_LMENU || g_set.holdkey == VK_RMENU)
        g_set.holdkey = VK_RCONTROL;
#endif
    g_set.mic[0] = 0;
    if (!k || RegQueryValueExW(k, L"Microphone", NULL, &type, (BYTE *)g_set.mic, &sz) || type != REG_SZ)
        g_set.mic[0] = 0;
    g_set.mic[255] = 0;
    sz = sizeof(g_set.language);
    if (!k || RegQueryValueExW(k, L"Language", NULL, &type, (BYTE *)g_set.language, &sz) || type != REG_SZ)
        lstrcpyW(g_set.language, L"en-US");
    g_set.language[15] = 0;
    if (k) RegCloseKey(k);
}

/* ---- the engine ---------------------------------------------------------------------- */

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

static void engine_send(const char *line)
{
    DWORD n;
    if (!g_bridged) return;
    EnterCriticalSection(&g_out_lock);
    WriteFile(g_out, line, (DWORD)strlen(line), &n, NULL);
    WriteFile(g_out, "\n", 1, &n, NULL);
    LeaveCriticalSection(&g_out_lock);
}

static void engine_cmd(const char *cmd)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"cmd\": \"%s\"}", cmd);
    engine_send(buf);
}

/* A thread reads the engine's lines and posts each to the window. */
static DWORD WINAPI reader_thread(void *arg)
{
    char line[65536];
    size_t n = 0;
    (void)arg;
    for (;;)
    {
        char c;
        DWORD got;
        if (!ReadFile(g_in, &c, 1, &got, NULL) || !got) break;
        if (c == '\r') continue;
        if (c != '\n')
        {
            if (n + 1 < sizeof(line)) line[n++] = c;
            continue;
        }
        line[n] = 0;
        n = 0;
        PostMessageW(g_wnd, WM_ENGINE, 0, (LPARAM)_strdup(line));
    }
    PostMessageW(g_wnd, WM_ENGINE_GONE, 0, 0);
    return 0;
}

/* Where sg-dictate is, as a \\?\unix\ path Wine can start. */
static void engine_path(WCHAR *out, size_t cch)
{
    WCHAR unix_path[MAX_PATH] = L"/usr/bin/sg-dictate";
    WCHAR *p;
    GetEnvironmentVariableW(L"SG_DICTATE", unix_path, MAX_PATH);
    _snwprintf(out, cch, L"\\\\?\\unix%ls", unix_path);
    out[cch - 1] = 0;
    for (p = out + 8; *p; p++) if (*p == '/') *p = '\\';
}

/* Not bridged yet: start ourselves again through the engine's bridge. */
static BOOL relaunch(const WCHAR *args)
{
    static WCHAR cmd[4096];
    WCHAR self[MAX_PATH], eng[MAX_PATH + 16];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    engine_path(eng, MAX_PATH + 16);
    _snwprintf(cmd, 4096, L"%ls --bridge wine \"%ls\" --bridged %ls", eng, self, args ? args : L"");
    cmd[4095] = 0;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return FALSE;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

/* ---- typing what was said ------------------------------------------------------------------ */

static void add_key(INPUT *in, int *n, WORD vk, WORD scan, DWORD flags)
{
    memset(&in[*n], 0, sizeof(INPUT));
    in[*n].type = INPUT_KEYBOARD;
    in[*n].ki.wVk = vk;
    in[*n].ki.wScan = scan;
    in[*n].ki.dwFlags = flags;
    (*n)++;
}

/* ---- Linux programs (David 2026-10-03: "voice typing doesn't make it into
 * the Linux side, like Firefox for Linux") -------------------------------
 * A Linux program's window is in a Wine frame (SgLinuxWindow, explorer's)
 * but is an X client with the X focus of its own: SendInput reaches the
 * frame, never the program. Its text goes to xdotool instead, which types
 * into the X window that has the focus -- any character, mapping a spare key
 * for those the keyboard has not. SG_XDOTOOL: the gate's stand-in. */
static BOOL linux_in_front(HWND fg)
{
    WCHAR cls[32] = L"";
#ifdef SG_MUTANT_LINUX_BY_SENDINPUT
    return FALSE;
#endif
    return fg && GetClassNameW(fg, cls, ARRAYSIZE(cls)) && !lstrcmpW(cls, L"SgLinuxWindow");
}

/* xdotool ARGS... (the last one may be long text), one after another.
 * Wine signals a Unix program's handle as soon as it starts, so waiting on
 * it waited for nothing: a phrase typed while the last one still was mixed
 * their letters (David 2026-10-04, "nWuiet hs wtihtec ..."). A worker runs
 * them in order, each to its end (__wine_unix_spawnvp, waiting). */
struct xjob { struct xjob *next; char **argv; };
static CRITICAL_SECTION g_xlock;
static struct xjob *g_xhead, *g_xtail;
static HANDLE g_xevent;

static DWORD WINAPI xdotool_worker(void *arg)
{
    LONG (WINAPI *spawnvp)(char * const argv[], int wait) =
        (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__wine_unix_spawnvp");
    (void)arg;
    for (;;)
    {
        struct xjob *j;
        WaitForSingleObject(g_xevent, INFINITE);
        for (;;)
        {
            char **a;
            EnterCriticalSection(&g_xlock);
            if ((j = g_xhead) && !(g_xhead = j->next)) g_xtail = NULL;
            LeaveCriticalSection(&g_xlock);
            if (!j) break;
#ifndef SG_MUTANT_XDOTOOL_OVERLAP
            if (spawnvp) spawnvp(j->argv, TRUE);
#else
            if (spawnvp) spawnvp(j->argv, FALSE);
#endif
            for (a = j->argv; *a; a++) free(*a);
            free(j->argv);
            free(j);
        }
    }
    return 0;
}

static char *utf8(const WCHAR *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc(n > 0 ? n : 1);
    if (s) { if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL); else s[0] = 0; }
    return s;
}

static BOOL run_xdotool(const WCHAR *const *args, int n)
{
    static char *(CDECL *unix_name)(const WCHAR *);
    WCHAR tool[MAX_PATH] = L"";
    struct xjob *j;
    int i;
    if (!g_xevent)
    {
        InitializeCriticalSection(&g_xlock);
        g_xevent = CreateEventW(NULL, FALSE, FALSE, NULL);
        CloseHandle(CreateThread(NULL, 0, xdotool_worker, NULL, 0, NULL));
        unix_name = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    }
    if (!(j = calloc(1, sizeof(*j))) || !(j->argv = calloc(n + 2, sizeof(char *)))) { free(j); return FALSE; }
    /* SG_XDOTOOL: another one (the gate's), a Windows path */
    if (GetEnvironmentVariableW(L"SG_XDOTOOL", tool, MAX_PATH) && unix_name)
    {
        char *u = unix_name(tool);
        if (u) { j->argv[0] = _strdup(u); HeapFree(GetProcessHeap(), 0, u); }
    }
    if (!j->argv[0]) j->argv[0] = _strdup("/usr/bin/xdotool");
    for (i = 0; i < n; i++) j->argv[i + 1] = utf8(args[i]);
    EnterCriticalSection(&g_xlock);
    if (g_xtail) g_xtail->next = j; else g_xhead = j;
    g_xtail = j;
    LeaveCriticalSection(&g_xlock);
    SetEvent(g_xevent);
    return TRUE;
}

static void type_text_linux(const WCHAR *text)
{
    const WCHAR *args[] = { L"type", L"--clearmodifiers", L"--delay", L"6", L"--", text };
    run_xdotool(args, ARRAYSIZE(args));
}

static void type_text(const WCHAR *text)
{
    size_t len = wcslen(text), i;
    INPUT *in = calloc(len * 2 + 2, sizeof(INPUT));
    int n = 0;
    if (!in) return;
    for (i = 0; i < len; i++)
    {
        if (text[i] == '\n')
        {
            /* A line break is the Enter key, which every program understands. */
            add_key(in, &n, VK_RETURN, (WORD)MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC), 0);
            add_key(in, &n, VK_RETURN, (WORD)MapVirtualKeyW(VK_RETURN, MAPVK_VK_TO_VSC), KEYEVENTF_KEYUP);
            continue;
        }
        add_key(in, &n, 0, text[i], KEYEVENTF_UNICODE);
        add_key(in, &n, 0, text[i], KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
    }
    SendInput(n, in, sizeof(INPUT));
    free(in);
}

/* The clipboard way: put the text there, press Ctrl+V, and put back the text
 * that was there before. */
static void paste_text(const WCHAR *text)
{
    size_t bytes = (wcslen(text) + 1) * sizeof(WCHAR);
    WCHAR *saved = NULL;
    HGLOBAL mem;
    INPUT in[4];
    int n = 0;

    if (!OpenClipboard(g_wnd)) { type_text(text); return; }
    if ((mem = GetClipboardData(CF_UNICODETEXT)))
    {
        WCHAR *p = GlobalLock(mem);
        if (p) { saved = _wcsdup(p); GlobalUnlock(mem); }
    }
    EmptyClipboard();
    if ((mem = GlobalAlloc(GMEM_MOVEABLE, bytes)))
    {
        WCHAR *p = GlobalLock(mem);
        const WCHAR *s;
        WCHAR *d = p;
        for (s = text; *s; s++)  /* Windows text on the clipboard ends lines with CR LF */
        {
            if (*s == '\n' && (size_t)(d - p) + 3 <= bytes / sizeof(WCHAR)) *d++ = '\r';
            *d++ = *s;
        }
        *d = 0;
        GlobalUnlock(mem);
        SetClipboardData(CF_UNICODETEXT, mem);
    }
    CloseClipboard();
    add_key(in, &n, VK_CONTROL, 0, 0);
    add_key(in, &n, 'V', 0, 0);
    add_key(in, &n, 'V', 0, KEYEVENTF_KEYUP);
    add_key(in, &n, VK_CONTROL, 0, KEYEVENTF_KEYUP);
    SendInput(n, in, sizeof(INPUT));
    if (saved)
    {
        /* Let the target read it first: its paste is a message behind ours. */
        MSG msg;
        DWORD until = GetTickCount() + 300;
        while (GetTickCount() < until)
        {
            while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            Sleep(20);
        }
        if (OpenClipboard(g_wnd))
        {
            EmptyClipboard();
            if ((mem = GlobalAlloc(GMEM_MOVEABLE, (wcslen(saved) + 1) * sizeof(WCHAR))))
            {
                wcscpy(GlobalLock(mem), saved);
                GlobalUnlock(mem);
                SetClipboardData(CF_UNICODETEXT, mem);
            }
            CloseClipboard();
        }
        free(saved);
    }
}

/* A JSON string (the engine's TEXT) as UTF-16. */
static WCHAR *json_to_w(const char *s)
{
    size_t len = strlen(s);
    char *u = malloc(len + 1);
    WCHAR *w;
    size_t n = 0;
    int wn;
    if (!u) return NULL;
    if (*s == '"') s++;
    while (*s && *s != '"')
    {
        if (*s != '\\') { u[n++] = *s++; continue; }
        s++;
        switch (*s)
        {
        case 'n': u[n++] = '\n'; s++; break;
        case 't': u[n++] = '\t'; s++; break;
        case 'r': s++; break;
        case 'u':
        {
            unsigned cp = 0;
            int i;
            for (i = 1; i <= 4 && s[i]; i++)
            {
                char c = s[i];
                cp = cp * 16 + (c >= '0' && c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            }
            s += i;
            /* The engine writes UTF-8 (ensure_ascii off), so only controls
             * and the odd surrogate come escaped; encode as UTF-8. */
            if (cp < 0x80) u[n++] = (char)cp;
            else if (cp < 0x800) { u[n++] = 0xC0 | (cp >> 6); u[n++] = 0x80 | (cp & 0x3F); }
            else { u[n++] = 0xE0 | (cp >> 12); u[n++] = 0x80 | ((cp >> 6) & 0x3F); u[n++] = 0x80 | (cp & 0x3F); }
            break;
        }
        default: if (*s) u[n++] = *s++; break;
        }
    }
    u[n] = 0;
    wn = MultiByteToWideChar(CP_UTF8, 0, u, -1, NULL, 0);
    w = calloc(wn > 0 ? wn : 1, sizeof(WCHAR));
    if (w && wn > 0) MultiByteToWideChar(CP_UTF8, 0, u, -1, w, wn);
    free(u);
    return w;
}

/* For the gates: the state and the partial text, rewritten on each change. */
static void dump(void)
{
    static const char *const names[] = { "idle", "loading", "listening", "nomodel", "off", "nomic",
                                         "error", "noengine", "privacy" };
    WCHAR path[MAX_PATH];
    char *u = NULL;
    FILE *f;
    int n;
    RECT r = { 0, 0, 0, 0 };
    if (!GetEnvironmentVariableW(L"SG_DICTATE_DUMP", path, MAX_PATH)) return;
    if (!(f = _wfopen(path, L"wb"))) return;
    fprintf(f, "state %s\n", names[g_state]);
    GetWindowRect(g_wnd, &r);
    fprintf(f, "visible %d\nrect %ld %ld %ld %ld\n", g_visible, r.left, r.top, r.right, r.bottom);
    if (g_partial && (n = WideCharToMultiByte(CP_UTF8, 0, g_partial, -1, NULL, 0, NULL, NULL)) > 0 &&
        (u = malloc(n)))
    {
        char *c;
        WideCharToMultiByte(CP_UTF8, 0, g_partial, -1, u, n, NULL, NULL);
        for (c = u; *c; c++) if (*c == '\n') *c = '|';
    }
    fprintf(f, "partial %s\n", u ? u : "");
    fprintf(f, "last_len %d\n", g_last_len);
    free(u);
    fclose(f);
}

/* The bar grows while it shows what is being said, and shrinks again. */
static void size_bar(void)
{
    int want = g_partial && *g_partial ? BAR_W_WIDE : BAR_W;
    MONITORINFO mi = { sizeof(mi) };
    POINT pt = { 0, 0 };
    if (want == g_bar_w) return;
    g_bar_w = want;
    if (!g_visible) return;
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    SetWindowPos(g_wnd, HWND_TOPMOST, (mi.rcWork.left + mi.rcWork.right - g_bar_w) / 2, mi.rcWork.top + 12,
                 g_bar_w, BAR_H, SWP_NOACTIVATE);
}

static void set_partial(WCHAR *text)
{
    free(g_partial);
    g_partial = text && *text ? text : NULL;
    if (text && !*text) free(text);
    size_bar();
    if (g_wnd) InvalidateRect(g_wnd, NULL, FALSE);
    dump();
}

/* ---- phrases: a short phrase said, the person's own text typed --------------------------------- */

/* Speech Recognition > Phrases (HKCU\...\Speech\Phrases: value name the
 * phrase, data the text): word replacement, and whole paragraphs from a few
 * words, as dictation programs' "auto-text" ("adult male chart" -> the
 * paragraph). Matched on words, ignoring case and the punctuation the model
 * adds; the longest phrase at a word wins. Said on its own, the phrase is
 * replaced whole -- the model's full stop and capital go with it. */
#define PHRASES_KEY L"Software\\Stained Glass\\Speech\\Phrases"
#define MAX_PHRASES 256
#define MAX_PHRASE_WORDS 16

struct phrase { WCHAR *words[MAX_PHRASE_WORDS]; int n; WCHAR *text; };

static BOOL word_char(WCHAR c) { return iswalnum(c) || c == '\''; }

/* the words of s (lower case), at most max; their spans when asked */
static int split_words(const WCHAR *s, WCHAR **words, int *start, int *end, int max)
{
    int n = 0, i = 0, len = (int)wcslen(s);
    while (i < len && n < max)
    {
        int b;
        while (i < len && !word_char(s[i])) i++;
        if (i >= len) break;
        b = i;
        while (i < len && word_char(s[i])) i++;
        if (words)
        {
            int k;
            if (!(words[n] = calloc(i - b + 1, sizeof(WCHAR)))) break;
            for (k = 0; k < i - b; k++) words[n][k] = towlower(s[b + k]);
        }
        if (start) start[n] = b;
        if (end) end[n] = i;
        n++;
    }
    return n;
}

static int load_phrases(struct phrase *ph)
{
    HKEY k;
    DWORD i, n = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, PHRASES_KEY, 0, KEY_READ, &k)) return 0;
    for (i = 0; n < MAX_PHRASES; i++)
    {
        WCHAR name[256];
        DWORD cname = ARRAYSIZE(name), type, cb = 0;
        WCHAR *data, *src, *dst;
        LONG r = RegEnumValueW(k, i, name, &cname, NULL, &type, NULL, &cb);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
        if (!(data = calloc(cb / sizeof(WCHAR) + 2, sizeof(WCHAR)))) break;
        cname = ARRAYSIZE(name);
        if (RegEnumValueW(k, i, name, &cname, NULL, &type, (BYTE *)data, &cb)) { free(data); continue; }
        for (src = dst = data; *src; src++) if (*src != '\r') *dst++ = *src;   /* the edit box's CR LF */
        *dst = 0;
        memset(&ph[n], 0, sizeof(ph[n]));
        ph[n].n = split_words(name, ph[n].words, NULL, NULL, MAX_PHRASE_WORDS);
        ph[n].text = data;
        if (ph[n].n) n++;
        else free(data);
    }
    RegCloseKey(k);
    return (int)n;
}

static void free_phrases(struct phrase *ph, int n)
{
    int i, j;
    for (i = 0; i < n; i++)
    {
        for (j = 0; j < ph[i].n; j++) free(ph[i].words[j]);
        free(ph[i].text);
    }
}

/* text with the phrases replaced: a new string, or text itself */
static WCHAR *apply_phrases(WCHAR *text)
{
    static struct phrase ph[MAX_PHRASES];
    WCHAR *words[512];
    int start[512], end[512], nw, np, i, j, w, best, bestn;
    size_t cap, len = 0;
    WCHAR *out;
#ifdef SG_MUTANT_NO_PHRASES
    return text;
#endif
    if (!(np = load_phrases(ph))) return text;
    nw = split_words(text, words, start, end, ARRAYSIZE(words));
    /* said on its own: the whole of it, with what was before the first word
     * (the engine's space) */
    for (i = 0; i < np; i++)
    {
        if (ph[i].n != nw) continue;
        for (j = 0; j < nw && !wcscmp(ph[i].words[j], words[j]); j++) ;
        if (j < nw) continue;
        cap = (nw ? start[0] : 0) + wcslen(ph[i].text) + 1;
        if ((out = calloc(cap, sizeof(WCHAR))))
        {
            wcsncpy(out, text, nw ? start[0] : 0);
            wcscat(out, ph[i].text);
            for (w = 0; w < nw; w++) free(words[w]);
            free_phrases(ph, np);
            free(text);
            return out;
        }
    }
    /* within what was said: word by word, the longest phrase first */
    cap = wcslen(text) + 1;
    for (i = 0; i < np; i++) cap += wcslen(ph[i].text) * (nw / ph[i].n + 1);
    if (!(out = calloc(cap, sizeof(WCHAR)))) goto done;
    {
        int pos = 0;
        for (w = 0; w < nw; )
        {
            best = -1; bestn = 0;
            for (i = 0; i < np; i++)
            {
                if (ph[i].n <= bestn || w + ph[i].n > nw) continue;
                for (j = 0; j < ph[i].n && !wcscmp(ph[i].words[j], words[w + j]); j++) ;
                if (j == ph[i].n) { best = i; bestn = ph[i].n; }
            }
            if (best < 0) { w++; continue; }
            wcsncpy(out + len, text + pos, start[w] - pos); len += start[w] - pos;
            wcscpy(out + len, ph[best].text); len += wcslen(ph[best].text);
            pos = end[w + bestn - 1];
            w += bestn;
        }
        wcscpy(out + len, text + pos);
    }
    free(text);
    text = out;
done:
    for (w = 0; w < nw; w++) free(words[w]);
    free_phrases(ph, np);
    return text;
}

static void insert_now(WCHAR *text)
{
    HWND fg = GetForegroundWindow();
    WCHAR cls[64] = L"";
    /* a long phrase's text (a paragraph) is pasted: typed, it took seconds
     * and any key pressed meanwhile went into the middle of it */
    BOOL paste = g_set.paste || wcslen(text) > 200;
    if (fg) GetClassNameW(fg, cls, 64);
    if (linux_in_front(fg))
    {
        type_text_linux(text);
        paste = FALSE;
    }
    else if (paste) paste_text(text); else type_text(text);
    g_last_len = (int)wcslen(text);
    g_last_target = fg;
    report("sg-dictate: inserted %d characters into %ls by %s\n", (int)wcslen(text), cls,
           linux_in_front(fg) ? "xdotool" : paste ? "paste" : "typing");
    free(text);
}

static void insert_text(const char *json)
{
    WCHAR *text = json_to_w(json);
    if (!text) return;
    text = apply_phrases(text);
    set_partial(NULL);  /* the final text replaces what was shown */
#ifndef SG_MUTANT_TYPE_WHILE_HELD
    /* Hold-to-talk: a pause while the key is still down ends a phrase, and
     * typed now it went in with the key held -- Right Ctrl+letters,
     * shortcuts, not text. It waits for the key's release. */
    if (g_hold_active && g_hold_down)
    {
        size_t had = g_held_text ? wcslen(g_held_text) : 0;
        WCHAR *n = realloc(g_held_text, (had + wcslen(text) + 1) * sizeof(WCHAR));
        if (n)
        {
            wcscpy(n + had, text);
            g_held_text = n;
            free(text);
            return;
        }
    }
#endif
    insert_now(text);
}

static void flush_held_text(void)
{
    WCHAR *text = g_held_text;
    g_held_text = NULL;
    if (text) insert_now(text);
}

/* ---- listening -------------------------------------------------------------------------------- */

static void set_state(enum state s)
{
    static const char *const names[] = { "idle", "loading", "listening", "nomodel", "off", "nomic",
                                         "error", "noengine", "privacy" };
    if (g_state != s) report("sg-dictate: state %s\n", names[s]);
    g_state = s;
    if (s != ST_LISTENING) g_level = 0;
    if (s != ST_LISTENING && s != ST_LOADING && g_partial) set_partial(NULL);
    if (g_wnd) InvalidateRect(g_wnd, NULL, FALSE);
    dump();
}

static BOOL listening(void) { return g_state == ST_LISTENING || g_state == ST_LOADING; }

/* The character before the caret, when the focus is an edit control we can
 * ask (Edit, RichEdit): so the first words go in with a space before them,
 * or none after a line break or at the start. Empty when unknown. */
static void caret_context(HWND fg, char *out, size_t cap)
{
    GUITHREADINFO gi = { sizeof(gi) };
    WCHAR cls[64], *text, ch[2] = { 0, 0 };
    DWORD_PTR sel = 0, len = 0;
    DWORD start;
    out[0] = 0;
    if (!fg || !GetGUIThreadInfo(GetWindowThreadProcessId(fg, NULL), &gi) || !gi.hwndFocus) return;
    GetClassNameW(gi.hwndFocus, cls, 64);
    if (_wcsicmp(cls, L"Edit") && _wcsnicmp(cls, L"RichEdit", 8)) return;
    if (!SendMessageTimeoutW(gi.hwndFocus, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 500, &sel)) return;
    start = LOWORD(sel);
    if (!start) { strcpy(out, "\n"); return; }  /* the start: as after a line break */
    SendMessageTimeoutW(gi.hwndFocus, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG, 500, &len);
    if (start > len || !(text = calloc(start + 2, sizeof(WCHAR)))) return;
    if (SendMessageTimeoutW(gi.hwndFocus, WM_GETTEXT, start + 1, (LPARAM)text, SMTO_ABORTIFHUNG, 500, NULL))
        ch[0] = text[start - 1] == '\r' ? '\n' : text[start - 1];
    free(text);
    if (ch[0]) WideCharToMultiByte(CP_UTF8, 0, ch, -1, out, (int)cap, NULL, NULL);
}

/* Settings > Privacy > Microphone: the device's switch (HKLM), apps' and
 * desktop apps' (HKCU) -- Windows' ConsentStore values. Any "Deny" and voice
 * typing does not listen. */
static BOOL consent_denied(HKEY root, const WCHAR *sub)
{
    WCHAR v[16] = L"";
    DWORD cb = sizeof(v);
    WCHAR key[200] = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone";
    if (sub) { lstrcatW(key, L"\\"); lstrcatW(key, sub); }
    if (RegGetValueW(root, key, L"Value", RRF_RT_REG_SZ, NULL, v, &cb)) return FALSE;
    return !_wcsicmp(v, L"Deny");
}

static BOOL microphone_allowed(void)
{
    return !consent_denied(HKEY_LOCAL_MACHINE, NULL) && !consent_denied(HKEY_CURRENT_USER, NULL) &&
           !consent_denied(HKEY_CURRENT_USER, L"NonPackaged");
}

static void start_listening(BOOL hold)
{
    char req[1200], mic[768], tail[8], lang[32];
    HWND fg = GetForegroundWindow();
    load_settings();
    if (!g_bridged) { set_state(ST_NOENGINE); return; }
    if (!g_set.enabled) { set_state(ST_OFF); return; }
    if (!microphone_allowed()) { set_state(ST_PRIVACY); return; }
    WideCharToMultiByte(CP_UTF8, 0, g_set.mic, -1, mic, sizeof(mic), NULL, NULL);
    snprintf(req, sizeof(req), "{\"cmd\": \"start\", \"continuous\": %s, \"spoken\": %s, \"auto\": %s, "
             "\"fillers\": %s, \"numbers\": %s, \"fresh\": %s, \"mic\": ",
             hold || g_set.continuous ? "true" : "false", g_set.spoken ? "true" : "false",
             g_set.autopunct ? "true" : "false", g_set.fillers ? "true" : "false",
             g_set.numbers ? "true" : "false", fg != g_last_target ? "true" : "false");
    json_str(req, sizeof(req) - 64, mic);
    WideCharToMultiByte(CP_UTF8, 0, g_set.language, -1, lang, sizeof(lang), NULL, NULL);
    /* "delete that", "stop listening": on whatever spoken punctuation is */
    strcat(req, ", \"partials\": true, \"commands\": true, \"language\": ");
    json_str(req, sizeof(req) - 16, lang);
    caret_context(fg, tail, sizeof(tail));
    if (tail[0])
    {
        strcat(req, ", \"tail\": ");
        json_str(req, sizeof(req) - 2, tail);
    }
    strcat(req, "}");
    engine_send(req);
    KillTimer(g_wnd, TIMER_UNLOAD);
    set_state(ST_LOADING);
}

static void show_bar(void)
{
    MONITORINFO mi = { sizeof(mi) };
    POINT pt = { 0, 0 };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    SetWindowPos(g_wnd, HWND_TOPMOST, (mi.rcWork.left + mi.rcWork.right - g_bar_w) / 2, mi.rcWork.top + 12,
                 g_bar_w, BAR_H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_visible = TRUE;
    g_closing = FALSE;
    SetTimer(g_wnd, TIMER_ANIM, 40, NULL);
}

/* Close the bar. What was said so far is still typed: the engine finishes
 * it, and the process goes when it says it is idle -- or stays, hidden, for
 * hold-to-talk. */
static void close_bar(void)
{
    ShowWindow(g_wnd, SW_HIDE);
    g_visible = FALSE;
    KillTimer(g_wnd, TIMER_ANIM);
    if (listening())
    {
        g_closing = TRUE;
        engine_cmd("stop");
        return;
    }
    if (g_background && g_set.hold) { SetTimer(g_wnd, TIMER_UNLOAD, UNLOAD_AFTER_MS, NULL); return; }
    engine_cmd("quit");
    DestroyWindow(g_wnd);
}

static void toggle(void)
{
    if (g_visible) { close_bar(); return; }
    show_bar();
    start_listening(FALSE);
}

static void open_settings(void)
{
    /* microphone access off: Settings' privacy page says why, and turns it on */
    if (g_state == ST_PRIVACY &&
        (INT_PTR)ShellExecuteW(NULL, NULL, L"ms-settings:privacy-microphone", NULL, NULL, SW_SHOWNORMAL) > 32) return;
    ShellExecuteW(NULL, NULL, L"control.exe", L"/name Microsoft.SpeechRecognition", NULL, SW_SHOWNORMAL);
}

/* "Delete that": as many Backspaces as the last text had characters (a line
 * break was one Enter), if it went to the window still in front. "Undo that":
 * the program's own Ctrl+Z. */
static void spoken_command(const char *what)
{
    HWND fg = GetForegroundWindow();
    INPUT *in;
    int n = 0, i;
    if (!strcmp(what, "delete"))
    {
        if (fg != g_last_target || g_last_len <= 0)
        {
            report("sg-dictate: nothing to delete\n");
            return;
        }
        if (linux_in_front(fg))
        {
            WCHAR count[16];
            const WCHAR *args[] = { L"key", L"--clearmodifiers", L"--repeat", count, L"BackSpace" };
            swprintf(count, 16, L"%d", g_last_len);
            run_xdotool(args, ARRAYSIZE(args));
            report("sg-dictate: deleted %d characters\n", g_last_len);
            g_last_len = 0;
            dump();
            return;
        }
        if (!(in = calloc(g_last_len * 2, sizeof(INPUT)))) return;
        for (i = 0; i < g_last_len; i++)
        {
            add_key(in, &n, VK_BACK, (WORD)MapVirtualKeyW(VK_BACK, MAPVK_VK_TO_VSC), 0);
            add_key(in, &n, VK_BACK, (WORD)MapVirtualKeyW(VK_BACK, MAPVK_VK_TO_VSC), KEYEVENTF_KEYUP);
        }
        SendInput(n, in, sizeof(INPUT));
        free(in);
        report("sg-dictate: deleted %d characters\n", g_last_len);
        g_last_len = 0;
    }
    else if (!strcmp(what, "undo") && linux_in_front(fg))
    {
        const WCHAR *args[] = { L"key", L"--clearmodifiers", L"ctrl+z" };
        run_xdotool(args, ARRAYSIZE(args));
        g_last_len = 0;
        report("sg-dictate: undo\n");
    }
    else if (!strcmp(what, "undo"))
    {
        INPUT k[4];
        add_key(k, &n, VK_CONTROL, 0, 0);
        add_key(k, &n, 'Z', 0, 0);
        add_key(k, &n, 'Z', 0, KEYEVENTF_KEYUP);
        add_key(k, &n, VK_CONTROL, 0, KEYEVENTF_KEYUP);
        SendInput(n, k, sizeof(INPUT));
        g_last_len = 0;
        report("sg-dictate: undo\n");
    }
    dump();
}

static void engine_line(char *line)
{
    if (!strncmp(line, "LEVEL ", 6))
    {
        g_level = atoi(line + 6);
        return;
    }
    if (!strncmp(line, "PARTIAL ", 8))
    {
#ifdef SG_MUTANT_PARTIAL_TYPED
        insert_text(line + 8);
#else
        WCHAR *text = json_to_w(line + 8);
        set_partial(text);
        report("sg-dictate: partial %d characters\n", g_partial ? (int)wcslen(g_partial) : 0);
#endif
        return;
    }
    if (!strncmp(line, "CMD ", 4))
    {
        spoken_command(line + 4);
        return;
    }
    if (!strncmp(line, "TEXT ", 5))
    {
        insert_text(line + 5);
        return;
    }
    if (strncmp(line, "STATE ", 6)) return;
    line += 6;
    if (!strcmp(line, "listening")) set_state(ST_LISTENING);
    else if (!strcmp(line, "loading")) set_state(ST_LOADING);
    else if (!strncmp(line, "nomodel", 7)) set_state(ST_NOMODEL);
    else if (!strncmp(line, "nomic", 5)) set_state(ST_NOMIC);
    else if (!strncmp(line, "error", 5)) set_state(ST_ERROR);
    else if (!strncmp(line, "idle", 4))
    {
        set_state(ST_IDLE);
        if (!g_hold_down && g_held_text) SetTimer( g_wnd, TIMER_FLUSH, FLUSH_DELAY_MS, NULL );
        g_hold_active = FALSE;
        if (g_hold_shown && g_visible)
        {
            g_hold_shown = FALSE;
            ShowWindow(g_wnd, SW_HIDE);
            g_visible = FALSE;
            KillTimer(g_wnd, TIMER_ANIM);
        }
        if (g_closing)
        {
            g_closing = FALSE;
            if (g_background && g_set.hold) SetTimer(g_wnd, TIMER_UNLOAD, UNLOAD_AFTER_MS, NULL);
            else { engine_cmd("quit"); DestroyWindow(g_wnd); }
        }
        else if (g_background && !g_visible)
            SetTimer(g_wnd, TIMER_UNLOAD, UNLOAD_AFTER_MS, NULL);
    }
}

/* ---- hold-to-talk --------------------------------------------------------------------------- */

/* Held alone for HOLD_DELAY_MS, the key starts listening; released, it
 * stops. Pressed with another key it is a shortcut (Right Ctrl+C), and
 * nothing happens. The key itself is never swallowed. */
static LRESULT CALLBACK kb_hook(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && g_set.hold)
    {
        const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
        BOOL down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        if (k->vkCode == g_set.holdkey)
        {
            if (down && !g_hold_pending && !g_hold_active)
            {
                g_hold_pending = TRUE;
                g_hold_down = TRUE;
                SetTimer(g_wnd, TIMER_HOLD, HOLD_DELAY_MS, NULL);
            }
            else if (!down)
            {
                KillTimer(g_wnd, TIMER_HOLD);
                g_hold_pending = FALSE;
                g_hold_down = FALSE;
                /* typed once the key is up (posted: the release is
                 * delivered first), then the rest after stop */
                if (g_hold_active) PostMessageW(g_wnd, WM_HOLD_RELEASED, 0, 0);
                if (g_hold_active) engine_cmd("stop");
            }
        }
        else if (down && g_hold_pending)
        {
            KillTimer(g_wnd, TIMER_HOLD);
            g_hold_pending = FALSE;
        }
    }
    return CallNextHookEx(g_kbhook, code, wp, lp);
}

static void update_hook(void)
{
    if (g_set.hold && !g_kbhook)
        g_kbhook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_hook, GetModuleHandleW(NULL), 0);
    else if (!g_set.hold && g_kbhook)
    {
        UnhookWindowsHookEx(g_kbhook);
        g_kbhook = NULL;
    }
}

/* ---- drawing ---------------------------------------------------------------------------------- */

/* The toolbar's round buttons and glyphs, drawn soft-edged (sg-smooth.h:
 * four times larger, averaged down): GDI's ellipses, polygons and pens gave
 * the microphone, the gear and the circles stepped edges (David 2026-10-06:
 * "low quality"). g_icon_scale: the glyphs' size at the window's display
 * scale (1.0 = as designed at 100%). */
static double g_icon_scale = 1.0;

struct circle_art { COLORREF c; };
static void circle_art(HDC dc, int w, int h, const void *arg)
{
    HBRUSH b = CreateSolidBrush(sg_smooth_colour(((const struct circle_art *)arg)->c)), ob = SelectObject(dc, b);
    HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, 0, 0, w + 1, h + 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

static void fill_circle(HDC dc, int cx, int cy, int r, COLORREF c)
{
#ifndef SG_MUTANT_JAGGED_DICTATE
    struct circle_art a = { c };
    sg_smooth(dc, cx - r, cy - r, 2 * r + 1, 2 * r + 1, circle_art, &a);
#else
    HBRUSH b = CreateSolidBrush(c), ob = SelectObject(dc, b);
    HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);   /* sg-smooth: the mutant's jagged circle */
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
#endif
}

/* a glyph on a square HALF units either side of its centre, u big pixels a unit */
struct glyph_art { int which; COLORREF c, bg; double half; };
enum { GLYPH_MIC, GLYPH_GEAR, GLYPH_CLOSE };

static void glyph_art(HDC dc, int w, int h, const void *arg)
{
    const struct glyph_art *g = arg;
    double u = w / (2.0 * g->half), cx = w / 2.0, cy = h / 2.0;
    LOGBRUSH lb = { BS_SOLID, sg_smooth_colour(g->c), 0 };
#define GX(v) ((int)lround(cx + (v) * u))
#define GY(v) ((int)lround(cy + (v) * u))
    switch (g->which)
    {
    case GLYPH_MIC:   /* a capsule on a stand */
    {
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND, (DWORD)lround(1.6 * u), &lb, 0, NULL);
        HBRUSH b = CreateSolidBrush(lb.lbColor);
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN)), ob = SelectObject(dc, b);
        RoundRect(dc, GX(-3.6), GY(-10), GX(3.6), GY(3.5), (int)lround(7.2 * u), (int)lround(7.2 * u));
        SelectObject(dc, pen);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        Arc(dc, GX(-7), GY(-6.5), GX(7), GY(7.5), GX(7), GY(0.5), GX(-7), GY(0.5));
        MoveToEx(dc, GX(0), GY(7.5), NULL); LineTo(dc, GX(0), GY(11));
        MoveToEx(dc, GX(-4), GY(11), NULL); LineTo(dc, GX(4), GY(11));
        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(pen); DeleteObject(b);
        break;
    }
    case GLYPH_GEAR:  /* eight square teeth round a ring, a hole in the middle */
    {
        POINT pts[32];
        int t, q;
        HBRUSH b = CreateSolidBrush(lb.lbColor), hole = CreateSolidBrush(sg_smooth_colour(g->bg));
        HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN)), ob = SelectObject(dc, b);
        for (t = 0; t < 8; t++)
        {
            static const double ang[4] = { -13, -7, 7, 13 }, rad[4] = { 6.8, 9.2, 9.2, 6.8 };
            for (q = 0; q < 4; q++)
            {
                double a = (t * 45 + ang[q]) * 3.14159265358979 / 180;
                pts[t * 4 + q].x = GX(rad[q] * sin(a));
                pts[t * 4 + q].y = GY(-rad[q] * cos(a));
            }
        }
        Polygon(dc, pts, 32);
        SelectObject(dc, hole);
        Ellipse(dc, GX(-3.2), GY(-3.2), GX(3.2), GY(3.2));
        SelectObject(dc, op); SelectObject(dc, ob);
        DeleteObject(b); DeleteObject(hole);
        break;
    }
    case GLYPH_CLOSE:  /* an X */
    {
        HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND, (DWORD)lround(1.2 * u), &lb, 0, NULL);
        HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, GX(-5), GY(-5), NULL); LineTo(dc, GX(5), GY(5));
        MoveToEx(dc, GX(5), GY(-5), NULL); LineTo(dc, GX(-5), GY(5));
        SelectObject(dc, op);
        DeleteObject(pen);
        break;
    }
    }
#undef GX
#undef GY
}

static void draw_glyph(HDC dc, int which, int cx, int cy, COLORREF c, COLORREF bg)
{
    struct glyph_art g = { which, c, bg, 12 };
    int half = (int)lround(12 * g_icon_scale);
    sg_smooth(dc, cx - half, cy - half, 2 * half, 2 * half, glyph_art, &g);
}

#ifndef SG_MUTANT_JAGGED_DICTATE
static void draw_mic(HDC dc, int cx, int cy, COLORREF c) { draw_glyph(dc, GLYPH_MIC, cx, cy, c, c); }
static void draw_gear(HDC dc, int cx, int cy, COLORREF c, COLORREF bg) { draw_glyph(dc, GLYPH_GEAR, cx, cy, c, bg); }
static void draw_close(HDC dc, int cx, int cy, COLORREF c) { draw_glyph(dc, GLYPH_CLOSE, cx, cy, c, c); }
#else
/* A microphone: a capsule on a stand. */
/* sg-smooth: the mutant's jagged drawing (SG_MUTANT_JAGGED_DICTATE) */
static void draw_mic(HDC dc, int cx, int cy, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c), ob = SelectObject(dc, b);
    HPEN pen = CreatePen(PS_SOLID, 2, c), op = SelectObject(dc, pen);
    RoundRect(dc, cx - 4, cy - 10, cx + 5, cy + 4, 9, 9);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Arc(dc, cx - 8, cy - 6, cx + 9, cy + 8, cx + 8, cy, cx - 8, cy);
    MoveToEx(dc, cx, cy + 8, NULL);
    LineTo(dc, cx, cy + 12);
    MoveToEx(dc, cx - 4, cy + 12, NULL);
    LineTo(dc, cx + 5, cy + 12);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(b);
}

/* A gear: eight teeth round a ring. */
/* sg-smooth: the mutant's jagged drawing (SG_MUTANT_JAGGED_DICTATE) */
static void draw_gear(HDC dc, int cx, int cy, COLORREF c, COLORREF bg)
{
    POINT pts[32];
    int i;
    HBRUSH b = CreateSolidBrush(c), ob = SelectObject(dc, b);
    HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
    for (i = 0; i < 32; i++)
    {
        double a = (i / 32.0) * 2 * 3.14159265358979 + 3.14159265358979 / 32;
        double r = (i % 4 == 0 || i % 4 == 1) ? 9.0 : 6.5;
        pts[i].x = cx + (int)lround(r * cos(a));
        pts[i].y = cy + (int)lround(r * sin(a));
    }
    Polygon(dc, pts, 32);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
    fill_circle(dc, cx, cy, 3, bg);
}

static void draw_close(HDC dc, int cx, int cy, COLORREF c)
{
    HPEN pen = CreatePen(PS_SOLID, 1, c), op = SelectObject(dc, pen);
    MoveToEx(dc, cx - 5, cy - 5, NULL); LineTo(dc, cx + 6, cy + 6);
    MoveToEx(dc, cx + 5, cy - 5, NULL); LineTo(dc, cx - 6, cy + 6);
    SelectObject(dc, op);
    DeleteObject(pen);
}
#endif

static const WCHAR *status_text(void)
{
    switch (g_state)
    {
    case ST_LOADING:   return L"Getting ready\x2026";
    case ST_LISTENING: return L"Listening\x2026";
    case ST_NOMODEL:   return L"Voice typing isn't set up yet. Select the gear to set it up.";
    case ST_OFF:       return L"Voice typing is off. Select the gear to turn it on.";
    case ST_NOMIC:     return L"We couldn't find a microphone.";
    case ST_ERROR:     return L"Something went wrong. Try again.";
    case ST_NOENGINE:  return L"Voice typing isn't available on this PC.";
    case ST_PRIVACY:   return L"Microphone access is off. Select the gear to change it.";
    default:           return L"Click the mic to start";
    }
}

#define GEAR_X 28
#define MIC_X 84
#define CLOSE_X (g_bar_w - 28)

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps), dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, g_bar_w, BAR_H), obmp = SelectObject(dc, bmp);
    RECT r = { 0, 0, g_bar_w, BAR_H }, tr;
    HBRUSH bg = CreateSolidBrush(COL_BG);
    HPEN edge = CreatePen(PS_SOLID, 1, COL_EDGE), op;
    int cy = BAR_H / 2;

    FillRect(dc, &r, bg);
    op = SelectObject(dc, edge);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 0, 0, g_bar_w, BAR_H);
    SelectObject(dc, op);

    if (g_hot == HOT_GEAR) fill_circle(dc, GEAR_X, cy, 16, COL_HOVER);
    draw_gear(dc, GEAR_X, cy, COL_SUBTLE, g_hot == HOT_GEAR ? COL_HOVER : COL_BG);
    if (g_hot == HOT_CLOSE) fill_circle(dc, CLOSE_X, cy, 16, COL_HOVER);
    draw_close(dc, CLOSE_X, cy, COL_SUBTLE);

    /* The listening indicator: the button in the accent colour, and a ring
     * that follows the voice. Nobody should have to wonder whether the
     * microphone is open. */
    if (g_state == ST_LISTENING)
    {
        int ring = (int)g_ring;
        if (ring > 0) fill_circle(dc, MIC_X, cy, MIC_R + ring, COL_RING);
        fill_circle(dc, MIC_X, cy, MIC_R, COL_ACCENT);
    }
    else
        fill_circle(dc, MIC_X, cy, MIC_R, g_hot == HOT_MIC ? RGB(0x55, 0x55, 0x55) : COL_IDLE);
    draw_mic(dc, MIC_X, cy, COL_TEXT);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, g_state == ST_LISTENING || g_state == ST_LOADING || g_state == ST_IDLE ? COL_TEXT : COL_SUBTLE);
    SelectObject(dc, g_state <= ST_LISTENING ? g_font : g_font_small);
    SetRect(&tr, MIC_X + MIC_R + 16, 4, CLOSE_X - 20, BAR_H - 4);
    if (g_partial && g_state <= ST_LISTENING)
    {
        /* What is being said, greyed and in italics: not typed yet. Its end
         * is what matters, so a long one loses its start ("...the end"). */
        const WCHAR *p = g_partial;
        WCHAR buf[4096];
        SIZE sz;
        int w = tr.right - tr.left;
        SelectObject(dc, g_font_partial);
        SetTextColor(dc, COL_SUBTLE);
        while (*p && GetTextExtentPoint32W(dc, p, (int)wcslen(p), &sz) && sz.cx > w - 16)
        {
            const WCHAR *sp = wcschr(p + 1, ' ');
            p = sp ? sp : p + 1;
        }
        _snwprintf(buf, 4096, L"%ls%ls", p == g_partial ? L"" : L"\x2026", p);
        buf[4095] = 0;
        {
            WCHAR *c;
            for (c = buf; *c; c++) if (*c == '\n') *c = ' ';
        }
        DrawTextW(dc, buf, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    else if (g_state <= ST_LISTENING)
        DrawTextW(dc, status_text(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    else
    {
        /* A longer message wraps: centre the lines it takes. */
        RECT m = tr;
        int h = DrawTextW(dc, status_text(), -1, &m, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        tr.top = (BAR_H - h) / 2;
        DrawTextW(dc, status_text(), -1, &tr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }

    BitBlt(wdc, 0, 0, g_bar_w, BAR_H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, obmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    DeleteObject(bg);
    DeleteObject(edge);
    EndPaint(hwnd, &ps);
}

static enum hot hit(int x, int y)
{
    int cy = BAR_H / 2;
    if ((x - GEAR_X) * (x - GEAR_X) + (y - cy) * (y - cy) <= 18 * 18) return HOT_GEAR;
    if ((x - MIC_X) * (x - MIC_X) + (y - cy) * (y - cy) <= (MIC_R + 4) * (MIC_R + 4)) return HOT_MIC;
    if ((x - CLOSE_X) * (x - CLOSE_X) + (y - cy) * (y - cy) <= 18 * 18) return HOT_CLOSE;
    return HOT_NONE;
}

/* ---- the window -------------------------------------------------------------------------------- */

static void command(const WCHAR *cmd)
{
    if (!wcscmp(cmd, L"/reload") || !wcscmp(cmd, L"reload"))
    {
        load_settings();
        update_hook();
        if (!g_visible && !listening() && !(g_background && g_set.hold))
        {
            engine_cmd("quit");
            DestroyWindow(g_wnd);
        }
        return;
    }
    if (!wcscmp(cmd, L"/background") || !wcscmp(cmd, L"background"))
    {
        /* Speech Recognition turned hold-to-talk on while the bar ran: its
         * settings and the key's hook, now -- the bar kept the old ones and
         * the key did nothing until the next sign-in (David 2026-10-02). */
#ifndef SG_MUTANT_HOLD_STALE
        load_settings();
        update_hook();
#endif
        g_background = TRUE;
        return;
    }
    toggle();
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE:
    {
        enum hot h = hit((short)LOWORD(lp), (short)HIWORD(lp));
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        if (h != g_hot) { g_hot = h; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hot) { g_hot = HOT_NONE; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_LBUTTONUP:
        switch (hit((short)LOWORD(lp), (short)HIWORD(lp)))
        {
        case HOT_GEAR: open_settings(); break;
        case HOT_CLOSE: close_bar(); break;
        case HOT_MIC:
            if (listening()) engine_cmd("stop"); else start_listening(FALSE);
            break;
        default: break;
        }
        return 0;
    case WM_HOLD_RELEASED:
#ifndef SG_MUTANT_FLUSH_AT_ONCE
        SetTimer( hwnd, TIMER_FLUSH, FLUSH_DELAY_MS, NULL );
#else
        flush_held_text();
#endif
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ANIM)
        {
            double target = g_state == ST_LISTENING ? g_level * 10.0 / 100.0 : 0;
            double next = g_ring + (target - g_ring) * 0.35;
            if (fabs(next - g_ring) > 0.05 || (int)next != (int)g_ring)
            {
                g_ring = next;
                InvalidateRect(hwnd, NULL, FALSE);
            }
        }
        else if (wp == TIMER_FLUSH)
        {
            KillTimer( hwnd, TIMER_FLUSH );
            flush_held_text();
        }
        else if (wp == TIMER_HOLD)
        {
            KillTimer(hwnd, TIMER_HOLD);
            if (g_hold_pending && !listening())
            {
                g_hold_pending = FALSE;
                g_hold_active = TRUE;
                if (!g_visible) { show_bar(); g_hold_shown = TRUE; }
                g_closing = FALSE;
                start_listening(TRUE);
                /* Released before the engine answered is handled by the
                 * hook: it sends stop, which the engine queues after start. */
            }
        }
        else if (wp == TIMER_UNLOAD)
        {
            KillTimer(hwnd, TIMER_UNLOAD);
            if (!listening()) engine_cmd("unload");
        }
        return 0;
    case WM_COPYDATA:
    {
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lp;
        WCHAR cmd[64] = L"";
        if (cd->dwData == 0x5344 && cd->cbData < sizeof(cmd))
        {
            memcpy(cmd, cd->lpData, cd->cbData);
            cmd[cd->cbData / sizeof(WCHAR)] = 0;
            command(cmd);
        }
        return TRUE;
    }
    case WM_ENGINE:
    {
        char *line = (char *)lp;
        if (line) { engine_line(line); free(line); }
        return 0;
    }
    case WM_ENGINE_GONE:
        /* The engine went away (and the bridge with it): nothing to type
         * with. */
        g_bridged = FALSE;
        report("sg-dictate: the engine went away\n");
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_kbhook) UnhookWindowsHookEx(g_kbhook);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Hand a command to the running instance. */
static BOOL forward(const WCHAR *cmd)
{
    HWND w = NULL;
    COPYDATASTRUCT cd;
    int i;
    for (i = 0; i < 40 && !(w = FindWindowW(CLASS_NAME, NULL)); i++) Sleep(50);
    if (!w) return FALSE;
    cd.dwData = 0x5344;
    cd.cbData = (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR));
    cd.lpData = (void *)cmd;
    SendMessageTimeoutW(w, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, NULL);
    return TRUE;
}

static const WCHAR *args_after_program(const WCHAR *cmdline)
{
    const WCHAR *p = cmdline;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* The session's helper keeper (sg-session) starts "/background" at sign-in
 * and again whenever it is not running. With hold-to-talk off it ends at
 * once, as it should -- and the keeper started it again, ten times, at every
 * sign-in. It now leaves $XDG_RUNTIME_DIR/sg-dictate-idle saying so, which
 * the keeper honours; the listener (Speech Recognition starts it when
 * hold-to-talk is turned on) takes it away. */
static void mark_idle(BOOL idle)
{
#ifndef SG_MUTANT_NO_IDLE_MARK
    WCHAR dir[MAX_PATH], path[MAX_PATH + 32];
    DWORD n = GetEnvironmentVariableW(L"XDG_RUNTIME_DIR", dir, MAX_PATH);
    HANDLE f;
    WCHAR *c;

    if (!n || n >= MAX_PATH || dir[0] != '/') return;
    swprintf(path, ARRAYSIZE(path), L"Z:%ls/sg-dictate-idle", dir);
    for (c = path; *c; c++) if (*c == '/') *c = '\\';
    if (!idle) { DeleteFileW(path); return; }
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
#else
    (void)idle;
#endif
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline_unused, int show)
{
    const WCHAR *cmdline = GetCommandLineW(), *args = args_after_program(cmdline);
    WCHAR cmd[64] = L"/toggle";
    WNDCLASSW wc = { 0 };
    HANDLE mutex;
    HWND owner;
    MSG msg;
    (void)prev; (void)cmdline_unused; (void)show;

    /* The command: the first switch that is not --bridged. */
    {
        int argc, i;
        WCHAR **argv = CommandLineToArgvW(cmdline, &argc);
        for (i = 1; argv && i < argc; i++)
        {
            if (!wcscmp(argv[i], L"--bridged")) { g_bridged = TRUE; continue; }
            if (argv[i][0] == '/' || argv[i][0] == '-')
            {
                lstrcpynW(cmd, argv[i], 64);
                if (cmd[0] == '-') cmd[0] = '/';
                break;
            }
        }
        LocalFree(argv);
    }
    load_settings();

    if (!g_bridged)
    {
        /* Someone already has the bar (or is listening for the hold key):
         * they do it. */
        if (FindWindowW(CLASS_NAME, NULL)) return forward(cmd) ? 0 : 1;
        if (!wcscmp(cmd, L"/reload")) return 0;
        if (!wcscmp(cmd, L"/background") && !g_set.hold) { mark_idle(TRUE); return 0; }
        if (relaunch(args)) return 0;
        /* No engine to be had: show the bar anyway, saying so. */
    }

    mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return forward(cmd) ? 0 : 1;
    mark_idle(FALSE);   /* running now: the keeper keeps it so */

    InitializeCriticalSection(&g_out_lock);
    if (g_bridged)
    {
        g_in = GetStdHandle(STD_INPUT_HANDLE);
        g_out = GetStdHandle(STD_OUTPUT_HANDLE);
        g_bridged = g_in && g_in != INVALID_HANDLE_VALUE && g_out && g_out != INVALID_HANDLE_VALUE;
    }

    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_small = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_partial = CreateFontW(-15, 0, 0, 0, FW_NORMAL, TRUE, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);
    /* Owned by a window that is never shown: no taskbar button, whatever
     * rule the taskbar keeps for tool windows. */
    owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"Static", NULL, WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, CLASS_NAME, L"Voice typing",
                            WS_POPUP, 0, 0, BAR_W, BAR_H, owner, NULL, inst, NULL);
    if (!g_wnd) return 1;
    if (g_bridged) CloseHandle(CreateThread(NULL, 0, reader_thread, NULL, 0, NULL));

    if (!wcscmp(cmd, L"/background"))
    {
        g_background = TRUE;
        update_hook();
        report("sg-dictate: waiting for the hold-to-talk key\n");
    }
    else
    {
        /* Win+H: the model loads while the bar appears; nothing said while
         * it does is lost (the engine keeps the audio). */
        g_background = g_set.hold;
        update_hook();
        engine_cmd("preload");
        toggle();
    }

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    engine_cmd("quit");
    if (mutex) CloseHandle(mutex);
    return 0;
}
