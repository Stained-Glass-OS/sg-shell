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
 * Engine -> bar: STATE <what> [detail], LEVEL <0-100>, TEXT <JSON string>.
 * Bar -> engine: one JSON object a line, {"cmd": "start", ...}, stop,
 * cancel, preload, unload, quit.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAR_W 360
#define BAR_H 56
#define MIC_R 18
#define WM_ENGINE (WM_APP + 1)
#define WM_ENGINE_GONE (WM_APP + 2)
#define TIMER_HOLD 1
#define TIMER_UNLOAD 2
#define TIMER_ANIM 3
#define HOLD_DELAY_MS 250
#define UNLOAD_AFTER_MS (5 * 60 * 1000)
#define CLASS_NAME L"SgDictateBar"
#define MUTEX_NAME L"Local\\StainedGlassVoiceTyping"
#define SPEECH_KEY L"Software\\Stained Glass\\Speech"

#define COL_BG      RGB(0x1F, 0x1F, 0x1F)
#define COL_EDGE    RGB(0x3A, 0x3A, 0x3A)
#define COL_TEXT    RGB(0xFF, 0xFF, 0xFF)
#define COL_SUBTLE  RGB(0xA8, 0xA8, 0xA8)
#define COL_ACCENT  RGB(0x7B, 0x2F, 0xBE)
#define COL_RING    RGB(0xB9, 0x8C, 0xF0)
#define COL_IDLE    RGB(0x44, 0x44, 0x44)
#define COL_HOVER   RGB(0x33, 0x33, 0x33)

enum state { ST_IDLE, ST_LOADING, ST_LISTENING, ST_NOMODEL, ST_OFF, ST_NOMIC, ST_ERROR, ST_NOENGINE, ST_PRIVACY };
enum hot { HOT_NONE, HOT_GEAR, HOT_MIC, HOT_CLOSE };

struct settings {
    DWORD enabled, continuous, spoken, autopunct, fillers, numbers, paste, hold, holdkey;
    WCHAR mic[256];
};

static HWND g_wnd;
static HANDLE g_in, g_out;
static BOOL g_bridged, g_background, g_visible, g_closing, g_hold_active, g_hold_pending;
static BOOL g_hold_shown;       /* the hold key opened the bar: it goes when done */
static enum state g_state = ST_IDLE;
static enum hot g_hot;
static int g_level;
static double g_ring;           /* the ring's radius beyond the button, eased */
static HWND g_last_target;      /* where the last text went */
static HFONT g_font, g_font_small;
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
    g_set.spoken = reg_dword(k, L"SpokenPunctuation", 1);
    g_set.autopunct = reg_dword(k, L"AutoPunctuation", 1);
    g_set.fillers = reg_dword(k, L"RemoveFillers", 1);
    g_set.numbers = reg_dword(k, L"FormatNumbers", 1);
    g_set.paste = reg_dword(k, L"InsertMethod", 0) == 1;
    g_set.hold = reg_dword(k, L"HoldToTalk", 0);
    g_set.holdkey = reg_dword(k, L"HoldKey", VK_RCONTROL);
    g_set.mic[0] = 0;
    if (!k || RegQueryValueExW(k, L"Microphone", NULL, &type, (BYTE *)g_set.mic, &sz) || type != REG_SZ)
        g_set.mic[0] = 0;
    g_set.mic[255] = 0;
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

static void insert_text(const char *json)
{
    WCHAR *text = json_to_w(json);
    HWND fg = GetForegroundWindow();
    WCHAR cls[64] = L"";
    if (!text) return;
    if (fg) GetClassNameW(fg, cls, 64);
    if (g_set.paste) paste_text(text); else type_text(text);
    g_last_target = fg;
    report("sg-dictate: inserted %d characters into %ls by %s\n", (int)wcslen(text), cls,
           g_set.paste ? "paste" : "typing");
    free(text);
}

/* ---- listening -------------------------------------------------------------------------------- */

static void set_state(enum state s)
{
    static const char *const names[] = { "idle", "loading", "listening", "nomodel", "off", "nomic",
                                         "error", "noengine", "privacy" };
    if (g_state != s) report("sg-dictate: state %s\n", names[s]);
    g_state = s;
    if (s != ST_LISTENING) g_level = 0;
    if (g_wnd) InvalidateRect(g_wnd, NULL, FALSE);
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
    char req[1024], mic[768], tail[8];
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
    json_str(req, sizeof(req) - 16, mic);
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
    SetWindowPos(g_wnd, HWND_TOPMOST, (mi.rcWork.left + mi.rcWork.right - BAR_W) / 2, mi.rcWork.top + 12,
                 BAR_W, BAR_H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
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

static void engine_line(char *line)
{
    if (!strncmp(line, "LEVEL ", 6))
    {
        g_level = atoi(line + 6);
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
                SetTimer(g_wnd, TIMER_HOLD, HOLD_DELAY_MS, NULL);
            }
            else if (!down)
            {
                KillTimer(g_wnd, TIMER_HOLD);
                g_hold_pending = FALSE;
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

static void fill_circle(HDC dc, int cx, int cy, int r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c), ob = SelectObject(dc, b);
    HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

/* A microphone: a capsule on a stand. */
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
#define CLOSE_X (BAR_W - 28)

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps), dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, BAR_W, BAR_H), obmp = SelectObject(dc, bmp);
    RECT r = { 0, 0, BAR_W, BAR_H }, tr;
    HBRUSH bg = CreateSolidBrush(COL_BG);
    HPEN edge = CreatePen(PS_SOLID, 1, COL_EDGE), op;
    int cy = BAR_H / 2;

    FillRect(dc, &r, bg);
    op = SelectObject(dc, edge);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, 0, 0, BAR_W, BAR_H);
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
    if (g_state <= ST_LISTENING)
        DrawTextW(dc, status_text(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    else
    {
        /* A longer message wraps: centre the lines it takes. */
        RECT m = tr;
        int h = DrawTextW(dc, status_text(), -1, &m, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        tr.top = (BAR_H - h) / 2;
        DrawTextW(dc, status_text(), -1, &tr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    }

    BitBlt(wdc, 0, 0, BAR_W, BAR_H, dc, 0, 0, SRCCOPY);
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
        if (!wcscmp(cmd, L"/background") && !g_set.hold) return 0;
        if (relaunch(args)) return 0;
        /* No engine to be had: show the bar anyway, saying so. */
    }

    mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
        return forward(cmd) ? 0 : 1;

    InitializeCriticalSection(&g_out_lock);
    if (g_bridged)
    {
        g_in = GetStdHandle(STD_INPUT_HANDLE);
        g_out = GetStdHandle(STD_OUTPUT_HANDLE);
        g_bridged = g_in && g_in != INVALID_HANDLE_VALUE && g_out && g_out != INVALID_HANDLE_VALUE;
    }

    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_small = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
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
