/* sg-clock -- Alarms & Clock: alarms, world clock, timers and a stopwatch.
 *
 * Windows 10's Alarms & Clock, drawn here in the Stained Glass Light palette:
 * four pages under a pivot (Alarm, World Clock, Timer, Stopwatch), cards for
 * alarms, cities and timers, and a notification (our own toast, above the
 * taskbar, with a chime of our own) when an alarm goes off or a timer ends.
 * Alarms and timers keep running when the window is closed: the program
 * stays in the background while any alarm is on or timer runs, and starts
 * with the session (HKCU Run, /background) while any alarm is on. Windows'
 * own caveat applies: notifications show only while the PC is awake.
 *
 * Kept in HKCU\Software\Stained Glass\Clock: Alarms, Cities and Timers
 * (REG_MULTI_SZ), Page. One instance: a second start hands its page to the
 * first. ms-clock: (and ms-clock:alarm|worldclock|timer|stopwatch) opens it.
 *
 * SG_CLOCK_DUMP=<file>: what the window and the notifications show, with the
 * screen centres of everything that can be clicked, for the gate.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <wchar.h>

#define C_BG       RGB(0xF3, 0xF3, 0xF3)
#define C_CARD     RGB(0xFF, 0xFF, 0xFF)
#define C_CARD_HOT RGB(0xF7, 0xF4, 0xFB)
#define C_EDGE     RGB(0xE3, 0xE0, 0xE8)
#define C_TEXT     RGB(0x1A, 0x1A, 0x1A)
#define C_TEXT2    RGB(0x60, 0x60, 0x68)
#define C_DIM      RGB(0xA8, 0xA6, 0xAE)
#define C_ACCENT   RGB(0x70, 0x30, 0xC0)
#define C_ACCENT_HOT RGB(0x86, 0x4E, 0xD0)
#define REG_KEY L"Software\\Stained Glass\\Clock"
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE L"Stained Glass Clock"
#define MAX_ALARMS 32
#define MAX_CITIES 24
#define MAX_TIMERS 16
#define MAX_LAPS 99
#define MAX_TOASTS 8
#define MAX_BTNS 160
#define TIMER_TICK 1
#define TIMER_FAST 2

enum page { P_ALARM, P_WORLD, P_TIMER, P_STOPWATCH, P_COUNT };
static const WCHAR *const PAGE_NAMES[P_COUNT] = { L"Alarm", L"World Clock", L"Timer", L"Stopwatch" };
static const char *const PAGE_KEYS[P_COUNT] = { "alarm", "worldclock", "timer", "stopwatch" };

struct alarm { int hour, minute, days; BOOL on; WCHAR name[64]; int snooze_min; ULONGLONG snoozed_until; int fired_key; };
struct city { WCHAR key[128], name[128]; };
struct ctimer { WCHAR name[64]; LONGLONG total_ms, left_ms; BOOL running; ULONGLONG started, left_at_start; };
struct toast { HWND hwnd; int kind; int index; WCHAR title[64], body[160], sub[64]; };

static struct alarm g_alarms[MAX_ALARMS];
static int g_nalarms;
static struct city g_cities[MAX_CITIES];
static int g_ncities;
static struct ctimer g_timers[MAX_TIMERS];
static int g_ntimers;
static LONGLONG g_sw_base;                  /* stopwatch: time before the current run, ms */
static ULONGLONG g_sw_started;              /* GetTickCount64 at start, 0: stopped */
static LONGLONG g_laps[MAX_LAPS];
static int g_nlaps;
static struct toast g_toasts[MAX_TOASTS];
static int g_ntoasts;

static HWND g_wnd;
static HINSTANCE g_inst;
static enum page g_page;
static int g_dpi = 96, g_scroll, g_hot = -1;
static HFONT g_f_body, g_f_small, g_f_head, g_f_big, g_f_huge, g_f_pivot, g_f_pivot_on, g_f_glyph;
static BOOL g_background;                   /* started hidden (/background), or window closed */
static char *g_chime;                       /* the notification sound, a WAV in memory */
static int g_chime_len;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* ---- time helpers ------------------------------------------------------------------------- */
static LONGLONG sw_elapsed(void)
{
#ifdef SG_MUTANT_STOPWATCH
    return g_sw_base;
#endif
    return g_sw_base + (g_sw_started ? (LONGLONG)(GetTickCount64() - g_sw_started) : 0);
}

static void fmt_hms(LONGLONG ms, WCHAR *out, int cch, BOOL centis)
{
    LONGLONG s = ms / 1000;
    if (centis) _snwprintf(out, cch, L"%02lld:%02lld:%02lld.%02lld", s / 3600, s / 60 % 60, s % 60, (ms % 1000) / 10);
    else _snwprintf(out, cch, L"%02lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
}

static void fmt_clock(int h, int m, WCHAR *out, int cch)
{
    SYSTEMTIME st = { 2000, 1, 0, 1, (WORD)h, (WORD)m, 0, 0 };
    if (!GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, out, cch)) _snwprintf(out, cch, L"%02d:%02d", h, m);
}

static void days_text(int days, WCHAR *out, int cch)
{
    static const WCHAR *const d[7] = { L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat", L"Sun" };
    int i;
    if (!days) { lstrcpynW(out, L"Only once", cch); return; }
    if (days == 0x7f) { lstrcpynW(out, L"Every day", cch); return; }
    if (days == 0x1f) { lstrcpynW(out, L"Weekdays", cch); return; }
    if (days == 0x60) { lstrcpynW(out, L"Weekends", cch); return; }
    out[0] = 0;
    for (i = 0; i < 7; i++) if (days & (1 << i)) {
        if (out[0]) wcsncat(out, L", ", cch - lstrlenW(out) - 1);
        wcsncat(out, d[i], cch - lstrlenW(out) - 1);
    }
}

/* ---- storage ----------------------------------------------------------------------------------- */
static void save_multi(const WCHAR *value, WCHAR *buf, int len)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    buf[len] = 0;
    RegSetValueExW(k, value, 0, REG_MULTI_SZ, (BYTE *)buf, (len + 1) * sizeof(WCHAR));
    RegCloseKey(k);
}

static void update_run_key(void)
{
    /* start with the session while any alarm is on, as Windows keeps alarms */
    HKEY k;
    int i;
    BOOL any = FALSE;
    for (i = 0; i < g_nalarms; i++) if (g_alarms[i].on) any = TRUE;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    if (any) {
        WCHAR self[MAX_PATH], cmd[MAX_PATH + 32];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" /background", self);
        RegSetValueExW(k, RUN_VALUE, 0, REG_SZ, (BYTE *)cmd, (lstrlenW(cmd) + 1) * sizeof(WCHAR));
    } else RegDeleteValueW(k, RUN_VALUE);
    RegCloseKey(k);
}

static void save_all(void)
{
    static WCHAR buf[32768];
    int n = 0, i;
    for (i = 0; i < g_nalarms; i++)
        n += _snwprintf(buf + n, ARRAYSIZE(buf) - n - 2, L"%02d:%02d|%d|%d|%d|%ls", g_alarms[i].hour, g_alarms[i].minute,
                        g_alarms[i].on, g_alarms[i].days, g_alarms[i].snooze_min, g_alarms[i].name) + 1;
    save_multi(L"Alarms", buf, n);
    n = 0;
    for (i = 0; i < g_ncities; i++) n += _snwprintf(buf + n, ARRAYSIZE(buf) - n - 2, L"%ls|%ls", g_cities[i].key, g_cities[i].name) + 1;
    save_multi(L"Cities", buf, n);
    n = 0;
    for (i = 0; i < g_ntimers; i++) n += _snwprintf(buf + n, ARRAYSIZE(buf) - n - 2, L"%lld|%ls", g_timers[i].total_ms, g_timers[i].name) + 1;
    save_multi(L"Timers", buf, n);
    update_run_key();
}

static void load_all(void)
{
    static WCHAR buf[32768];
    DWORD cb;
    WCHAR *p;
    HKEY k;
    BOOL have = !RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &k);
    g_nalarms = g_ncities = g_ntimers = 0;
    if (have) {
        cb = sizeof(buf) - 4;
        if (!RegQueryValueExW(k, L"Alarms", NULL, NULL, (BYTE *)buf, &cb)) {
            buf[cb / 2] = buf[cb / 2 + 1] = 0;
            for (p = buf; *p && g_nalarms < MAX_ALARMS; p += lstrlenW(p) + 1) {
                struct alarm *a = &g_alarms[g_nalarms];
                int on = 1, n = 0;
                memset(a, 0, sizeof(*a));
                if (swscanf(p, L"%d:%d|%d|%d|%d|%n", &a->hour, &a->minute, &on, &a->days, &a->snooze_min, &n) >= 5 && n) {
                    a->on = on; lstrcpynW(a->name, p + n, ARRAYSIZE(a->name)); a->fired_key = -1; g_nalarms++;
                }
            }
        }
        cb = sizeof(buf) - 4;
        if (!RegQueryValueExW(k, L"Cities", NULL, NULL, (BYTE *)buf, &cb)) {
            buf[cb / 2] = buf[cb / 2 + 1] = 0;
            for (p = buf; *p && g_ncities < MAX_CITIES; p += lstrlenW(p) + 1) {
                WCHAR *bar = wcschr(p, L'|');
                if (!bar) continue;
                lstrcpynW(g_cities[g_ncities].key, p, (int)(bar - p + 1) < 128 ? (int)(bar - p + 1) : 128);
                lstrcpynW(g_cities[g_ncities].name, bar + 1, 128);
                g_ncities++;
            }
        }
        cb = sizeof(buf) - 4;
        if (!RegQueryValueExW(k, L"Timers", NULL, NULL, (BYTE *)buf, &cb)) {
            buf[cb / 2] = buf[cb / 2 + 1] = 0;
            for (p = buf; *p && g_ntimers < MAX_TIMERS; p += lstrlenW(p) + 1) {
                struct ctimer *t = &g_timers[g_ntimers];
                int n = 0;
                memset(t, 0, sizeof(*t));
                if (swscanf(p, L"%lld|%n", &t->total_ms, &n) >= 1 && n && t->total_ms > 0) {
                    lstrcpynW(t->name, p + n, ARRAYSIZE(t->name)); t->left_ms = t->total_ms; g_ntimers++;
                }
            }
        }
        {
            DWORD page = 0; cb = sizeof(page);
            if (!RegQueryValueExW(k, L"Page", NULL, NULL, (BYTE *)&page, &cb) && page < P_COUNT) g_page = page;
        }
        RegCloseKey(k);
    } else {
        /* a first start: an alarm, a timer, as Windows' app comes */
        struct alarm *a = &g_alarms[g_nalarms++];
        memset(a, 0, sizeof(*a));
        a->hour = 7; a->minute = 0; a->days = 0x1f; a->snooze_min = 10; a->fired_key = -1;
        lstrcpyW(a->name, L"Good morning");
        g_timers[0].total_ms = g_timers[0].left_ms = 5 * 60 * 1000;
        lstrcpyW(g_timers[0].name, L"Timer (1)");
        g_ntimers = 1;
    }
}

/* ---- the sound: a two-tone chime of our own, synthesized ------------------------------------------ */
static void make_chime(void)
{
    const int rate = 22050, ms = 1600;
    int samples = rate * ms / 1000, i, size = 44 + samples * 2;
    short *pcm;
    char *w = calloc(1, size);
    if (!w) return;
    memcpy(w, "RIFF", 4); *(DWORD *)(w + 4) = size - 8; memcpy(w + 8, "WAVEfmt ", 8);
    *(DWORD *)(w + 16) = 16; *(WORD *)(w + 20) = 1; *(WORD *)(w + 22) = 1; *(DWORD *)(w + 24) = rate;
    *(DWORD *)(w + 28) = rate * 2; *(WORD *)(w + 32) = 2; *(WORD *)(w + 34) = 16;
    memcpy(w + 36, "data", 4); *(DWORD *)(w + 40) = samples * 2;
    pcm = (short *)(w + 44);
    for (i = 0; i < samples; i++) {
        double t = (double)i / rate, v = 0;
        /* two soft notes (E6, B5) with a decay, then a rest */
        if (t < 0.45) v = sin(2 * 3.14159265 * 1318.5 * t) * exp(-t * 5);
        else if (t < 1.0) v = sin(2 * 3.14159265 * 987.8 * (t - 0.45)) * exp(-(t - 0.45) * 5);
        pcm[i] = (short)(v * 9000);
    }
    g_chime = w; g_chime_len = size;
}

static void chime(BOOL on)
{
    if (on && g_chime) PlaySoundA(g_chime, NULL, SND_MEMORY | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
    else PlaySoundA(NULL, NULL, 0);
}

/* ---- notifications -------------------------------------------------------------------------------- */
enum { TK_ALARM = 1, TK_TIMER = 2 };
#define TOAST_W 364
#define TOAST_H 150
static void write_dump(void);
static void relayout(void);

static void place_toasts(void)
{
    RECT work, bar;
    HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
    int i, bottom;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    bottom = work.bottom;
    if (tray && GetWindowRect(tray, &bar) && bar.top > work.top + 100 && bar.top < bottom) bottom = bar.top;
    for (i = 0; i < g_ntoasts; i++)
        SetWindowPos(g_toasts[i].hwnd, HWND_TOPMOST, work.right - S(TOAST_W) - S(12), bottom - (i + 1) * (S(TOAST_H) + S(8)) - S(4),
                     S(TOAST_W), S(TOAST_H), SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static void toast_buttons(const struct toast *t, RECT *snooze, RECT *dismiss)
{
    int w = S(TOAST_W), h = S(TOAST_H), bw = t->kind == TK_ALARM ? (w - S(40)) / 2 : w - S(32);
    SetRect(dismiss, w - S(16) - bw, h - S(48), w - S(16), h - S(16));
    SetRect(snooze, S(16), h - S(48), S(16) + bw, h - S(16));
}

static void close_toast(int i)
{
    HWND h;
    if (i < 0 || i >= g_ntoasts) return;
    h = g_toasts[i].hwnd;
    memmove(&g_toasts[i], &g_toasts[i + 1], (g_ntoasts - i - 1) * sizeof(g_toasts[0]));
    g_ntoasts--;
    DestroyWindow(h);
    if (!g_ntoasts) chime(FALSE);
    place_toasts();
    write_dump();
}

static int toast_index(HWND h)
{
    int i;
    for (i = 0; i < g_ntoasts; i++) if (g_toasts[i].hwnd == h) return i;
    return -1;
}

static void snooze(int alarm)
{
    struct alarm *a;
    if (alarm < 0 || alarm >= g_nalarms) return;
    a = &g_alarms[alarm];
    a->snoozed_until = GetTickCount64() + (ULONGLONG)(a->snooze_min > 0 ? a->snooze_min : 10) * 60000;
}

static LRESULT CALLBACK toast_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int i = toast_index(hwnd);
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r, a, b, t;
        HBRUSH br;
        GetClientRect(hwnd, &r);
        br = CreateSolidBrush(RGB(0x2B, 0x28, 0x33)); FillRect(dc, &r, br); DeleteObject(br);
        SetRect(&a, 0, 0, S(4), r.bottom);
        br = CreateSolidBrush(C_ACCENT); FillRect(dc, &a, br); DeleteObject(br);
        if (i >= 0) {
            struct toast *tt = &g_toasts[i];
            SetBkMode(dc, TRANSPARENT);
            SelectObject(dc, g_f_small); SetTextColor(dc, RGB(0xC8, 0xC2, 0xD4));
            SetRect(&t, S(16), S(10), r.right - S(16), S(30));
            DrawTextW(dc, L"Alarms & Clock", -1, &t, DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, g_f_head); SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
            SetRect(&t, S(16), S(32), r.right - S(16), S(58));
            DrawTextW(dc, tt->title, -1, &t, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, g_f_body); SetTextColor(dc, RGB(0xE8, 0xE4, 0xF0));
            SetRect(&t, S(16), S(58), r.right - S(16), S(80));
            DrawTextW(dc, tt->body, -1, &t, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SetRect(&t, S(16), S(78), r.right - S(16), S(98));
            SetTextColor(dc, RGB(0xC8, 0xC2, 0xD4));
            DrawTextW(dc, tt->sub, -1, &t, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            toast_buttons(tt, &a, &b);
            br = CreateSolidBrush(RGB(0x45, 0x40, 0x52));
            if (tt->kind == TK_ALARM) { FillRect(dc, &a, br); SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF)); DrawTextW(dc, L"Snooze", -1, &a, DT_CENTER | DT_VCENTER | DT_SINGLELINE); }
            DeleteObject(br);
            br = CreateSolidBrush(C_ACCENT); FillRect(dc, &b, br); DeleteObject(br);
            SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
            DrawTextW(dc, L"Dismiss", -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONUP:
        if (i >= 0) {
            RECT a, b;
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            toast_buttons(&g_toasts[i], &a, &b);
            if (PtInRect(&b, pt)) {
                /* an "only once" alarm is done when it is dismissed (a snoozed one goes off again) */
                if (g_toasts[i].kind == TK_ALARM && g_toasts[i].index < g_nalarms && !g_alarms[g_toasts[i].index].days) {
                    g_alarms[g_toasts[i].index].on = FALSE;
                    save_all();
                    relayout();
                }
                close_toast(i);
            }
            else if (g_toasts[i].kind == TK_ALARM && PtInRect(&a, pt)) { snooze(g_toasts[i].index); close_toast(i); }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void notify(int kind, int index, const WCHAR *title, const WCHAR *body, const WCHAR *sub)
{
    struct toast *t;
    HWND owner = g_wnd;
    if (g_ntoasts >= MAX_TOASTS) close_toast(0);
    t = &g_toasts[g_ntoasts];
    memset(t, 0, sizeof(*t));
    t->kind = kind; t->index = index;
    lstrcpynW(t->title, title, ARRAYSIZE(t->title));
    lstrcpynW(t->body, body, ARRAYSIZE(t->body));
    lstrcpynW(t->sub, sub, ARRAYSIZE(t->sub));
    /* owned by the (maybe hidden) main window: no taskbar button, never takes the focus */
    t->hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"SgClockToast", title, WS_POPUP,
                              0, 0, S(TOAST_W), S(TOAST_H), owner, NULL, g_inst, NULL);
    g_ntoasts++;
    place_toasts();
    chime(TRUE);
    write_dump();
}

/* ---- the clock's heartbeat ---------------------------------------------------------------------- */
static void check_alarms(void)
{
    SYSTEMTIME now;
    int i, key, dow;
    GetLocalTime(&now);
    dow = (now.wDayOfWeek + 6) % 7;             /* Monday is 0 */
    key = now.wDay * 1440 + now.wHour * 60 + now.wMinute;
    for (i = 0; i < g_nalarms; i++) {
        struct alarm *a = &g_alarms[i];
        WCHAR t[32], body[96];
        BOOL due = FALSE;
        if (!a->on) continue;
        if (a->snoozed_until && GetTickCount64() >= a->snoozed_until) { a->snoozed_until = 0; due = TRUE; }
        else if (a->hour == now.wHour && a->minute == now.wMinute && a->fired_key != key && (!a->days || (a->days & (1 << dow))))
            due = TRUE;
        if (!due) continue;
        a->fired_key = key;
        fmt_clock(a->hour, a->minute, t, ARRAYSIZE(t));
        _snwprintf(body, ARRAYSIZE(body), L"%ls", a->name[0] ? a->name : L"Alarm");
        notify(TK_ALARM, i, L"Alarm", body, t);
    }
}

static void tick_timers(void)
{
    int i;
    for (i = 0; i < g_ntimers; i++) {
        struct ctimer *t = &g_timers[i];
        LONGLONG left;
        if (!t->running) continue;
        left = (LONGLONG)t->left_at_start - (LONGLONG)(GetTickCount64() - t->started);
#ifdef SG_MUTANT_NOTIMERFIRE
        if (left < 1) left = 1;
#endif
        if (left <= 0) {
            WCHAR total[32];
            t->left_ms = t->total_ms;       /* ready to go again, as Windows resets it */
            t->running = FALSE;
            fmt_hms(t->total_ms, total, ARRAYSIZE(total), FALSE);
            notify(TK_TIMER, i, L"Timer", t->name[0] ? t->name : L"Timer", L"Time's up!");
            (void)total;
        } else t->left_ms = left;
    }
}

static BOOL busy(void)
{
    int i;
    for (i = 0; i < g_nalarms; i++) if (g_alarms[i].on) return TRUE;
    for (i = 0; i < g_ntimers; i++) if (g_timers[i].running) return TRUE;
    return g_ntoasts > 0;
}

/* ---- the window's layout: every clickable thing is a button in this table ------------------------- */
enum {
    B_PIVOT = 1, B_ADD, B_ALARM_CARD, B_ALARM_TOGGLE, B_CITY_CARD, B_CITY_REMOVE, B_TIMER_START, B_TIMER_RESET, B_TIMER_REMOVE,
    B_SW_START, B_SW_LAP, B_SW_RESET, B_TIMER_CARD,
};
struct btn { RECT r; int kind, index; WCHAR label[64]; };
static struct btn g_btns[MAX_BTNS];
static int g_nbtns;

static struct btn *add_btn(int kind, int index, int l, int t, int r, int b, const WCHAR *label)
{
    struct btn *x;
    if (g_nbtns >= MAX_BTNS) return NULL;
    x = &g_btns[g_nbtns++];
    SetRect(&x->r, l, t, r, b);
    x->kind = kind; x->index = index;
    lstrcpynW(x->label, label ? label : L"", ARRAYSIZE(x->label));
    return x;
}

#define PIVOT_H 56
#define BAR_H 56

static void layout(void)
{
    RECT c;
    int i, x, y, w, cw, cols, col;
    GetClientRect(g_wnd, &c);
    g_nbtns = 0;
    /* the pivot */
    {
        HDC dc = GetDC(g_wnd);
        x = S(24);
        SelectObject(dc, g_f_pivot_on);
        for (i = 0; i < P_COUNT; i++) {
            SIZE sz;
            GetTextExtentPoint32W(dc, PAGE_NAMES[i], lstrlenW(PAGE_NAMES[i]), &sz);
            add_btn(B_PIVOT, i, x, S(8), x + sz.cx + S(8), S(PIVOT_H), PAGE_NAMES[i]);
            x += sz.cx + S(28);
        }
        ReleaseDC(g_wnd, dc);
    }
    y = S(PIVOT_H) + S(16) - g_scroll;
    w = c.right - S(48);
    switch (g_page) {
    case P_ALARM:
        cw = S(300); cols = w / (cw + S(12)); if (cols < 1) cols = 1;
        for (i = 0; i < g_nalarms; i++) {
            int cx = S(24) + (i % cols) * (cw + S(12)), cy = y + (i / cols) * S(148);
            add_btn(B_ALARM_CARD, i, cx, cy, cx + cw, cy + S(136), g_alarms[i].name);
            add_btn(B_ALARM_TOGGLE, i, cx + cw - S(64), cy + S(20), cx + cw - S(16), cy + S(44), g_alarms[i].on ? L"On" : L"Off");
        }
        add_btn(B_ADD, 0, c.right - S(72), c.bottom - S(BAR_H) + S(8), c.right - S(24), c.bottom - S(8), L"Add an alarm");
        break;
    case P_WORLD:
        for (i = 0; i <= g_ncities; i++) {
            int cy = y + i * S(84);
            add_btn(B_CITY_CARD, i - 1, S(24), cy, S(24) + w, cy + S(76), i ? g_cities[i - 1].name : L"Local time");
            if (i) add_btn(B_CITY_REMOVE, i - 1, S(24) + w - S(44), cy + S(22), S(24) + w - S(12), cy + S(54), L"Remove");
        }
        add_btn(B_ADD, 0, c.right - S(72), c.bottom - S(BAR_H) + S(8), c.right - S(24), c.bottom - S(8), L"Add a city");
        break;
    case P_TIMER:
        cw = S(300); cols = w / (cw + S(12)); if (cols < 1) cols = 1;
        for (i = 0; i < g_ntimers; i++) {
            int cx = S(24) + (i % cols) * (cw + S(12)), cy = y + (i / cols) * S(232);
            col = cx;
            add_btn(B_TIMER_CARD, i, cx, cy, cx + cw, cy + S(220), g_timers[i].name);
            add_btn(B_TIMER_START, i, col + cw / 2 - S(56), cy + S(160), col + cw / 2 - S(8), cy + S(208), g_timers[i].running ? L"Pause" : L"Start");
            add_btn(B_TIMER_RESET, i, col + cw / 2 + S(8), cy + S(160), col + cw / 2 + S(56), cy + S(208), L"Reset");
            add_btn(B_TIMER_REMOVE, i, cx + cw - S(40), cy + S(8), cx + cw - S(8), cy + S(40), L"Remove");
        }
        add_btn(B_ADD, 0, c.right - S(72), c.bottom - S(BAR_H) + S(8), c.right - S(24), c.bottom - S(8), L"Add a timer");
        break;
    case P_STOPWATCH: {
        int cx = c.right / 2, by = S(PIVOT_H) + S(170);
        add_btn(B_SW_RESET, 0, cx - S(112), by, cx - S(64), by + S(48), L"Reset");
        add_btn(B_SW_START, 0, cx - S(28), by - S(4), cx + S(28), by + S(52), g_sw_started ? L"Pause" : L"Start");
        add_btn(B_SW_LAP, 0, cx + S(64), by, cx + S(112), by + S(48), L"Lap");
        break;
    }
    default: break;
    }
}

static void relayout(void) { layout(); InvalidateRect(g_wnd, NULL, FALSE); write_dump(); }

/* ---- drawing ------------------------------------------------------------------------------------------ */
static void fill(HDC dc, int l, int t, int r, int b, COLORREF c)
{
    RECT x = { l, t, r, b };
    HBRUSH br = CreateSolidBrush(c);
    FillRect(dc, &x, br);
    DeleteObject(br);
}

static void text(HDC dc, HFONT f, COLORREF c, int l, int t, int r, int b, const WCHAR *s, UINT fmt)
{
    RECT x = { l, t, r, b };
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &x, fmt | DT_NOPREFIX);
}

static void round_fill(HDC dc, RECT r, int radius, COLORREF c, COLORREF edge)
{
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pen);
}

static void toggle(HDC dc, RECT r, BOOL on)
{
    int h = S(20), y = (r.top + r.bottom - h) / 2, kx;
    RECT p = { r.left, y, r.left + S(44), y + h };
    HBRUSH br = CreateSolidBrush(on ? C_ACCENT : C_CARD);
    HPEN pen = CreatePen(PS_SOLID, S(2) > 1 ? S(2) : 1, on ? C_ACCENT : RGB(0x33, 0x33, 0x33));
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, p.left, p.top, p.right, p.bottom, h, h);
    SelectObject(dc, GetStockObject(NULL_PEN));
    DeleteObject(br);
    br = CreateSolidBrush(on ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x33, 0x33, 0x33));
    SelectObject(dc, br);
    kx = on ? p.right - S(15) : p.left + S(5);
    Ellipse(dc, kx, p.top + S(5), kx + S(10) + 1, p.top + S(15) + 1);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pen);
}

/* a glyph drawn with lines: + x > || > (play) and a lap flag */
static void glyph(HDC dc, const WCHAR *what, RECT r, COLORREF c)
{
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, a = S(8);
    HPEN pen = CreatePen(PS_SOLID, S(2) > 1 ? S(2) : 1, c);
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
    if (!lstrcmpW(what, L"+")) { MoveToEx(dc, cx - a, cy, NULL); LineTo(dc, cx + a + 1, cy); MoveToEx(dc, cx, cy - a, NULL); LineTo(dc, cx, cy + a + 1); }
    else if (!lstrcmpW(what, L"x")) { a = S(6); MoveToEx(dc, cx - a, cy - a, NULL); LineTo(dc, cx + a + 1, cy + a + 1); MoveToEx(dc, cx + a, cy - a, NULL); LineTo(dc, cx - a - 1, cy + a + 1); }
    else if (!lstrcmpW(what, L"play")) { POINT p[3] = { { cx - S(5), cy - S(8) }, { cx - S(5), cy + S(8) }, { cx + S(8), cy } }; Polygon(dc, p, 3); }
    else if (!lstrcmpW(what, L"pause")) { RECT a1 = { cx - S(6), cy - S(8), cx - S(2), cy + S(8) }, a2 = { cx + S(2), cy - S(8), cx + S(6), cy + S(8) }; FillRect(dc, &a1, br); FillRect(dc, &a2, br); }
    else if (!lstrcmpW(what, L"reset")) {
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        Arc(dc, cx - S(8), cy - S(8), cx + S(8), cy + S(8), cx - S(8), cy - S(2), cx - S(2), cy - S(8));
        MoveToEx(dc, cx - S(8), cy - S(8), NULL); LineTo(dc, cx - S(8), cy - S(2)); LineTo(dc, cx - S(2), cy - S(2));
    } else if (!lstrcmpW(what, L"lap")) {
        MoveToEx(dc, cx - S(6), cy + S(9), NULL); LineTo(dc, cx - S(6), cy - S(9));
        { POINT p[4] = { { cx - S(6), cy - S(9) }, { cx + S(8), cy - S(6) }, { cx - S(6), cy - S(1) }, { cx - S(6), cy - S(9) } }; Polygon(dc, p, 4); }
    }
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen); DeleteObject(br);
}

static void circle_button(HDC dc, RECT r, BOOL accent, BOOL hot, const WCHAR *g)
{
    HBRUSH br = CreateSolidBrush(accent ? (hot ? C_ACCENT_HOT : C_ACCENT) : (hot ? RGB(0xE6, 0xE2, 0xEC) : RGB(0xEC, 0xEA, 0xF0)));
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br);
    glyph(dc, g, r, accent ? RGB(0xFF, 0xFF, 0xFF) : C_TEXT);
}

/* the world clock: a zone's time now */
static BOOL zone_time(const WCHAR *key, SYSTEMTIME *out, LONG *bias_min)
{
    DYNAMIC_TIME_ZONE_INFORMATION dtz;
    SYSTEMTIME utc;
    DWORD i;
    GetSystemTime(&utc);
    for (i = 0; !EnumDynamicTimeZoneInformation(i, &dtz); i++) {
        TIME_ZONE_INFORMATION tzi;
        if (lstrcmpiW(dtz.TimeZoneKeyName, key)) continue;
        /* this year's rules (Wine has no SystemTimeToTzSpecificLocalTimeEx) */
        if (!GetTimeZoneInformationForYear(utc.wYear, &dtz, &tzi) || !SystemTimeToTzSpecificLocalTime(&tzi, &utc, out))
            return FALSE;
        if (bias_min) {
            FILETIME a, b;
            ULARGE_INTEGER ua, ub;
            SystemTimeToFileTime(out, &a); SystemTimeToFileTime(&utc, &b);
            ua.LowPart = a.dwLowDateTime; ua.HighPart = a.dwHighDateTime;
            ub.LowPart = b.dwLowDateTime; ub.HighPart = b.dwHighDateTime;
            *bias_min = (LONG)(((LONGLONG)ua.QuadPart - (LONGLONG)ub.QuadPart) / 600000000LL);
        }
        return TRUE;
    }
    return FALSE;
}

static void relation(const SYSTEMTIME *there, LONG bias_there, WCHAR *out, int cch)
{
    SYSTEMTIME here;
    TIME_ZONE_INFORMATION tzi;
    LONG local_bias, diff;
    const WCHAR *day;
    DWORD r = GetTimeZoneInformation(&tzi);
    GetLocalTime(&here);
    local_bias = -(tzi.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? tzi.DaylightBias : r == TIME_ZONE_ID_STANDARD ? tzi.StandardBias : 0));
    diff = bias_there - local_bias;
    day = there->wDay == here.wDay ? L"Today" : (there->wDay > here.wDay && there->wMonth == here.wMonth) || there->wMonth > here.wMonth ||
          there->wYear > here.wYear ? L"Tomorrow" : L"Yesterday";
    if (!diff) _snwprintf(out, cch, L"%ls, same time", day);
    else if (diff % 60) _snwprintf(out, cch, L"%ls, %ld hr %ld min %ls", day, labs(diff) / 60, labs(diff) % 60, diff > 0 ? L"ahead" : L"behind");
    else _snwprintf(out, cch, L"%ls, %ld hour%ls %ls", day, labs(diff) / 60, labs(diff) == 60 ? L"" : L"s", diff > 0 ? L"ahead" : L"behind");
}

static void paint(HDC dc, RECT *c)
{
    int i;
    WCHAR buf[128], buf2[128];
    SetBkMode(dc, TRANSPARENT);
    fill(dc, 0, 0, c->right, c->bottom, C_BG);
    for (i = 0; i < g_nbtns; i++) {
        struct btn *b = &g_btns[i];
        BOOL hot = i == g_hot;
        switch (b->kind) {
        case B_PIVOT:
            text(dc, b->index == (int)g_page ? g_f_pivot_on : g_f_pivot, b->index == (int)g_page ? C_TEXT : C_TEXT2,
                 b->r.left, b->r.top, b->r.right + S(20), b->r.bottom - S(8), b->label, DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
            if (b->index == (int)g_page) fill(dc, b->r.left, b->r.bottom - S(4), b->r.right - S(8), b->r.bottom - S(1), C_ACCENT);
            break;
        case B_ALARM_CARD: {
            struct alarm *a = &g_alarms[b->index];
            round_fill(dc, b->r, S(8), hot ? C_CARD_HOT : C_CARD, C_EDGE);
            fmt_clock(a->hour, a->minute, buf, ARRAYSIZE(buf));
            text(dc, g_f_big, a->on ? C_TEXT : C_DIM, b->r.left + S(20), b->r.top + S(12), b->r.right - S(80), b->r.top + S(64), buf, DT_SINGLELINE | DT_LEFT);
            text(dc, g_f_body, a->on ? C_TEXT : C_DIM, b->r.left + S(20), b->r.top + S(66), b->r.right - S(20), b->r.top + S(88),
                 a->name[0] ? a->name : L"Alarm", DT_SINGLELINE | DT_END_ELLIPSIS);
            days_text(a->days, buf, ARRAYSIZE(buf));
            if (a->snoozed_until) _snwprintf(buf2, ARRAYSIZE(buf2), L"%ls  \x2022  Snoozed", buf); else lstrcpyW(buf2, buf);
            text(dc, g_f_small, C_TEXT2, b->r.left + S(20), b->r.top + S(92), b->r.right - S(20), b->r.top + S(112), buf2, DT_SINGLELINE | DT_END_ELLIPSIS);
            break;
        }
        case B_ALARM_TOGGLE: toggle(dc, b->r, g_alarms[b->index].on); break;
        case B_CITY_CARD: {
            SYSTEMTIME st;
            LONG bias = 0;
            round_fill(dc, b->r, S(8), hot ? C_CARD_HOT : C_CARD, C_EDGE);
            if (b->index < 0) {
                GetLocalTime(&st);
                lstrcpyW(buf2, L"Local time");
                GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &st, NULL, buf, ARRAYSIZE(buf));
            } else {
                if (!zone_time(g_cities[b->index].key, &st, &bias)) GetLocalTime(&st);
                lstrcpynW(buf2, g_cities[b->index].name, ARRAYSIZE(buf2));
                relation(&st, bias, buf, ARRAYSIZE(buf));
            }
            text(dc, g_f_head, C_TEXT, b->r.left + S(20), b->r.top + S(12), b->r.right - S(220), b->r.top + S(40), buf2, DT_SINGLELINE | DT_END_ELLIPSIS);
            text(dc, g_f_small, C_TEXT2, b->r.left + S(20), b->r.top + S(42), b->r.right - S(220), b->r.top + S(64), buf, DT_SINGLELINE | DT_END_ELLIPSIS);
            GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, buf, ARRAYSIZE(buf));
            text(dc, g_f_big, C_TEXT, b->r.right - S(240), b->r.top + S(10), b->r.right - S(64), b->r.bottom - S(10), buf, DT_SINGLELINE | DT_RIGHT | DT_VCENTER);
            break;
        }
        case B_CITY_REMOVE: case B_TIMER_REMOVE:
            if (hot) round_fill(dc, b->r, S(6), RGB(0xEC, 0xEA, 0xF0), RGB(0xEC, 0xEA, 0xF0));
            glyph(dc, L"x", b->r, C_TEXT2);
            break;
        case B_TIMER_CARD: {
            struct ctimer *t = &g_timers[b->index];
            RECT ring = { (b->r.left + b->r.right) / 2 - S(62), b->r.top + S(20), (b->r.left + b->r.right) / 2 + S(62), b->r.top + S(144) };
            double frac = t->total_ms ? (double)t->left_ms / t->total_ms : 0;
            HPEN pen;
            HGDIOBJ op;
            round_fill(dc, b->r, S(8), C_CARD, C_EDGE);
            text(dc, g_f_small, C_TEXT2, b->r.left + S(16), b->r.top + S(12), b->r.right - S(48), b->r.top + S(32),
                 t->name[0] ? t->name : L"Timer", DT_SINGLELINE | DT_END_ELLIPSIS);
            /* the ring: the time left, in the accent */
            pen = CreatePen(PS_SOLID, S(4), RGB(0xE3, 0xE0, 0xE8));
            op = SelectObject(dc, pen); SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, ring.left, ring.top, ring.right, ring.bottom);
            SelectObject(dc, op); DeleteObject(pen);
            if (frac > 0.001) {
                double a0 = 3.14159265 / 2, a1 = a0 + frac * 2 * 3.14159265;
                int cx = (ring.left + ring.right) / 2, cy = (ring.top + ring.bottom) / 2, rr = (ring.right - ring.left) / 2;
                pen = CreatePen(PS_SOLID, S(4), C_ACCENT);
                op = SelectObject(dc, pen);
                if (frac >= 0.999) Ellipse(dc, ring.left, ring.top, ring.right, ring.bottom);
                else {
                    SetArcDirection(dc, AD_CLOCKWISE);
                    Arc(dc, ring.left, ring.top, ring.right, ring.bottom, cx + (int)(rr * cos(a1)), cy - (int)(rr * sin(a1)),
                        cx + (int)(rr * cos(a0)), cy - (int)(rr * sin(a0)));
                }
                SelectObject(dc, op); DeleteObject(pen);
            }
            fmt_hms(t->left_ms + (t->running ? 999 : 0), buf, ARRAYSIZE(buf), FALSE);
            text(dc, g_f_head, C_TEXT, ring.left, ring.top, ring.right, ring.bottom, buf, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
            break;
        }
        case B_TIMER_START: circle_button(dc, b->r, TRUE, hot, g_timers[b->index].running ? L"pause" : L"play"); break;
        case B_TIMER_RESET: circle_button(dc, b->r, FALSE, hot, L"reset"); break;
        case B_SW_START: circle_button(dc, b->r, TRUE, hot, g_sw_started ? L"pause" : L"play"); break;
        case B_SW_RESET: circle_button(dc, b->r, FALSE, hot, L"reset"); break;
        case B_SW_LAP: circle_button(dc, b->r, FALSE, hot, L"lap"); break;
        case B_ADD: {
            RECT r = b->r;
            if (hot) round_fill(dc, r, S(6), RGB(0xE6, 0xE2, 0xEC), RGB(0xE6, 0xE2, 0xEC));
            glyph(dc, L"+", r, C_TEXT);
            break;
        }
        }
    }
    if (g_page == P_STOPWATCH) {
        LONGLONG e = sw_elapsed();
        int y = S(PIVOT_H) + S(50), k;
        fmt_hms(e, buf, ARRAYSIZE(buf), TRUE);
        text(dc, g_f_huge, C_TEXT, 0, y, c->right, y + S(100), buf, DT_SINGLELINE | DT_CENTER);
        y = S(PIVOT_H) + S(250);
        if (g_nlaps) {
            text(dc, g_f_small, C_TEXT2, c->right / 2 - S(200), y, c->right / 2 - S(80), y + S(20), L"Laps", DT_SINGLELINE);
            text(dc, g_f_small, C_TEXT2, c->right / 2 - S(80), y, c->right / 2 + S(60), y + S(20), L"Time", DT_SINGLELINE | DT_RIGHT);
            text(dc, g_f_small, C_TEXT2, c->right / 2 + S(60), y, c->right / 2 + S(200), y + S(20), L"Total", DT_SINGLELINE | DT_RIGHT);
            y += S(26);
            for (k = g_nlaps - 1; k >= 0 && y < c->bottom; k--) {
                LONGLONG prev = k ? g_laps[k - 1] : 0;
                _snwprintf(buf, ARRAYSIZE(buf), L"%d", k + 1);
                text(dc, g_f_body, C_TEXT, c->right / 2 - S(200), y, c->right / 2 - S(80), y + S(24), buf, DT_SINGLELINE);
                fmt_hms(g_laps[k] - prev, buf, ARRAYSIZE(buf), TRUE);
                text(dc, g_f_body, C_TEXT, c->right / 2 - S(120), y, c->right / 2 + S(60), y + S(24), buf, DT_SINGLELINE | DT_RIGHT);
                fmt_hms(g_laps[k], buf, ARRAYSIZE(buf), TRUE);
                text(dc, g_f_body, C_TEXT2, c->right / 2 + S(20), y, c->right / 2 + S(200), y + S(24), buf, DT_SINGLELINE | DT_RIGHT);
                y += S(28);
            }
        }
    }
    if (g_page == P_ALARM && busy())
        text(dc, g_f_small, C_TEXT2, S(24), c->bottom - S(BAR_H), c->right - S(100), c->bottom - S(8),
             L"Notifications will only show if the PC is awake.", DT_SINGLELINE | DT_VCENTER);
    fill(dc, 0, S(PIVOT_H) + S(2), c->right, S(PIVOT_H) + S(3), C_EDGE);
}

/* ---- the dialogs: an alarm, a timer, a city -------------------------------------------------------- */
enum { D_ALARM = 1, D_TIMER, D_CITY };
static HWND g_dlg;
static int g_dlg_kind, g_dlg_index;
#define ID_NAME 100
#define ID_H 101
#define ID_M 102
#define ID_S 103
#define ID_DAY0 110
#define ID_SNOOZE 120
#define ID_SEARCH 130
#define ID_LIST 131
#define ID_SAVE 140
#define ID_DELETE 141
#define ID_CANCEL 142

/* the owner is enabled before the dialog goes, or Windows activates another program's window */
static void close_dialog(void)
{
    if (!g_dlg) return;
    EnableWindow(g_wnd, TRUE);
    SetActiveWindow(g_wnd);
    DestroyWindow(g_dlg);
}

static int g_zone_count;
static WCHAR g_zone_keys[256][128], g_zone_names[256][160];

static void load_zones(void)
{
    DYNAMIC_TIME_ZONE_INFORMATION dtz;
    DWORD i;
    WCHAR sub[300];
    if (g_zone_count) return;
    for (i = 0; !EnumDynamicTimeZoneInformation(i, &dtz) && g_zone_count < 256; i++) {
        WCHAR disp[160] = L"";
        DWORD cb = sizeof(disp);
        _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\%ls", dtz.TimeZoneKeyName);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, sub, L"Display", RRF_RT_REG_SZ, NULL, disp, &cb)) lstrcpynW(disp, dtz.TimeZoneKeyName, 160);
        lstrcpynW(g_zone_keys[g_zone_count], dtz.TimeZoneKeyName, 128);
        lstrcpynW(g_zone_names[g_zone_count], disp, 160);
        g_zone_count++;
    }
}

/* "(UTC+09:00) Osaka, Sapporo, Tokyo" and the word searched for -> "Tokyo, Osaka, Sapporo..." : the city asked for first */
static void city_name(const WCHAR *display, const WCHAR *query, WCHAR *out, int cch)
{
    const WCHAR *p = wcschr(display, L')');
    WCHAR list[160], *tok, *ctx, lower[160], q[64];
    p = p ? p + 1 : display;
    while (*p == L' ') p++;
    lstrcpynW(list, p, ARRAYSIZE(list));
    lstrcpynW(q, query ? query : L"", ARRAYSIZE(q)); CharLowerW(q);
    for (tok = wcstok_s(list, L",", &ctx); tok; tok = wcstok_s(NULL, L",", &ctx)) {
        while (*tok == L' ') tok++;
        lstrcpynW(lower, tok, ARRAYSIZE(lower)); CharLowerW(lower);
        if (q[0] && wcsstr(lower, q)) { lstrcpynW(out, tok, cch); return; }
    }
    lstrcpynW(out, p, cch);
    if (wcschr(out, L',')) *wcschr(out, L',') = 0;
}

static void fill_zone_list(void)
{
    WCHAR q[64], lower[160];
    HWND list = GetDlgItem(g_dlg, ID_LIST), search = GetDlgItem(g_dlg, ID_SEARCH);
    int i;
    GetWindowTextW(search, q, ARRAYSIZE(q));
    CharLowerW(q);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_zone_count; i++) {
        lstrcpynW(lower, g_zone_names[i], ARRAYSIZE(lower));
        CharLowerW(lower);
        if (q[0] && !wcsstr(lower, q)) continue;
        SendMessageW(list, LB_SETITEMDATA, SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)g_zone_names[i]), i);
    }
    SendMessageW(list, LB_SETCURSEL, 0, 0);
}

static void dialog_save(void)
{
    WCHAR b[64];
    if (g_dlg_kind == D_ALARM) {
        struct alarm *a;
        int h, m, i, days = 0;
        GetDlgItemTextW(g_dlg, ID_H, b, 64); h = _wtoi(b);
        GetDlgItemTextW(g_dlg, ID_M, b, 64); m = _wtoi(b);
        if (h < 0 || h > 23 || m < 0 || m > 59) { MessageBoxW(g_dlg, L"Type the hour (0-23) and the minutes (0-59).", L"Alarm", MB_OK); return; }
        if (g_dlg_index < 0) {
            if (g_nalarms >= MAX_ALARMS) return;
            g_dlg_index = g_nalarms++;
            memset(&g_alarms[g_dlg_index], 0, sizeof(g_alarms[0]));
            g_alarms[g_dlg_index].fired_key = -1;
        }
        a = &g_alarms[g_dlg_index];
        a->hour = h; a->minute = m; a->on = TRUE; a->snoozed_until = 0; a->fired_key = -1;
        GetDlgItemTextW(g_dlg, ID_NAME, a->name, ARRAYSIZE(a->name));
        for (i = 0; i < 7; i++) if (IsDlgButtonChecked(g_dlg, ID_DAY0 + i)) days |= 1 << i;
        a->days = days;
        {
            static const int snz[] = { 5, 10, 20, 30, 60 };
            int s = (int)SendDlgItemMessageW(g_dlg, ID_SNOOZE, CB_GETCURSEL, 0, 0);
            a->snooze_min = s >= 0 && s < 5 ? snz[s] : 10;
        }
    } else if (g_dlg_kind == D_TIMER) {
        struct ctimer *t;
        LONGLONG ms;
        int h, m, s;
        GetDlgItemTextW(g_dlg, ID_H, b, 64); h = _wtoi(b);
        GetDlgItemTextW(g_dlg, ID_M, b, 64); m = _wtoi(b);
        GetDlgItemTextW(g_dlg, ID_S, b, 64); s = _wtoi(b);
        ms = ((LONGLONG)h * 3600 + m * 60 + s) * 1000;
        if (ms <= 0 || h > 99 || m > 59 || s > 59) { MessageBoxW(g_dlg, L"Type a time of at least one second.", L"Timer", MB_OK); return; }
        if (g_ntimers >= MAX_TIMERS) return;
        t = &g_timers[g_ntimers++];
        memset(t, 0, sizeof(*t));
        t->total_ms = t->left_ms = ms;
        GetDlgItemTextW(g_dlg, ID_NAME, t->name, ARRAYSIZE(t->name));
        if (!t->name[0]) _snwprintf(t->name, ARRAYSIZE(t->name), L"Timer (%d)", g_ntimers);
    } else if (g_dlg_kind == D_CITY) {
        HWND list = GetDlgItem(g_dlg, ID_LIST);
        int sel = (int)SendMessageW(list, LB_GETCURSEL, 0, 0), z;
        WCHAR q[64];
        if (sel < 0 || g_ncities >= MAX_CITIES) return;
        z = (int)SendMessageW(list, LB_GETITEMDATA, sel, 0);
        GetDlgItemTextW(g_dlg, ID_SEARCH, q, ARRAYSIZE(q));
        lstrcpynW(g_cities[g_ncities].key, g_zone_keys[z], 128);
        city_name(g_zone_names[z], q, g_cities[g_ncities].name, 128);
        g_ncities++;
    }
    save_all();
    close_dialog();
    relayout();
}

static LRESULT CALLBACK dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SAVE: case IDOK: dialog_save(); return 0;
        case ID_CANCEL: case IDCANCEL: close_dialog(); return 0;
        case ID_DELETE:
            if (g_dlg_kind == D_ALARM && g_dlg_index >= 0 && g_dlg_index < g_nalarms) {
                memmove(&g_alarms[g_dlg_index], &g_alarms[g_dlg_index + 1], (g_nalarms - g_dlg_index - 1) * sizeof(g_alarms[0]));
                g_nalarms--;
                save_all();
            }
            close_dialog();
            relayout();
            return 0;
        case ID_SEARCH: if (HIWORD(wp) == EN_CHANGE) { fill_zone_list(); write_dump(); } return 0;
        case ID_LIST: if (HIWORD(wp) == LBN_DBLCLK) dialog_save(); return 0;
        }
        break;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN:
        SetBkColor((HDC)wp, C_CARD);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_CLOSE: close_dialog(); return 0;
    case WM_DESTROY: g_dlg = NULL; EnableWindow(g_wnd, TRUE); write_dump(); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND dctl(const WCHAR *cls, const WCHAR *txt, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_dlg, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_f_body, TRUE);
    return c;
}

static void open_dialog(int kind, int index)
{
    static const WCHAR *const days[7] = { L"Mo", L"Tu", L"We", L"Th", L"Fr", L"Sa", L"Su" };
    RECT wr;
    int w = S(420), h, x = S(20), y = S(20), i;
    WCHAR b[32];
    const WCHAR *title = kind == D_ALARM ? (index < 0 ? L"New alarm" : L"Edit alarm") : kind == D_TIMER ? L"New timer" : L"Add a new location";
    close_dialog();
    h = kind == D_ALARM ? S(400) : kind == D_TIMER ? S(260) : S(420);
    GetWindowRect(g_wnd, &wr);
    g_dlg_kind = kind; g_dlg_index = index;
    g_dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"SgClockDialog", title, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            (wr.left + wr.right - w) / 2, wr.top + S(60), w, h, g_wnd, NULL, g_inst, NULL);
    if (kind == D_ALARM || kind == D_TIMER) {
        const struct alarm *a = kind == D_ALARM && index >= 0 ? &g_alarms[index] : NULL;
        int n = kind == D_TIMER ? 3 : 2;
        const WCHAR *const labels[3] = { L"Hours", L"Minutes", L"Seconds" };
        for (i = 0; i < n; i++) {
            dctl(L"STATIC", labels[i], 0, x + i * S(120), y, S(100), S(20), -1);
            if (kind == D_ALARM) _snwprintf(b, ARRAYSIZE(b), L"%02d", a ? (i ? a->minute : a->hour) : (i ? 0 : 7));
            else lstrcpyW(b, i == 1 ? L"05" : L"00");
            dctl(L"EDIT", b, WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_CENTER, x + i * S(120), y + S(22), S(100), S(40), ID_H + i);
            SendDlgItemMessageW(g_dlg, ID_H + i, WM_SETFONT, (WPARAM)g_f_head, TRUE);
        }
        y += S(76);
        dctl(L"STATIC", kind == D_ALARM ? L"Alarm name" : L"Timer name", 0, x, y, S(200), S(20), -1);
        dctl(L"EDIT", a ? a->name : kind == D_ALARM ? L"Alarm" : L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, x, y + S(22), w - 2 * x - S(8), S(28), ID_NAME);
        y += S(64);
        if (kind == D_ALARM) {
            dctl(L"STATIC", L"Repeats", 0, x, y, S(200), S(20), -1);
            for (i = 0; i < 7; i++) {
                HWND c = dctl(L"BUTTON", days[i], WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE, x + i * S(52), y + S(22), S(46), S(30), ID_DAY0 + i);
                if (a && (a->days & (1 << i))) SendMessageW(c, BM_SETCHECK, BST_CHECKED, 0);
            }
            y += S(64);
            dctl(L"STATIC", L"Snooze time", 0, x, y, S(200), S(20), -1);
            {
                static const WCHAR *const snz[] = { L"5 minutes", L"10 minutes", L"20 minutes", L"30 minutes", L"1 hour" };
                static const int mins[] = { 5, 10, 20, 30, 60 };
                HWND c = dctl(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y + S(22), S(180), S(200), ID_SNOOZE);
                int sel = 1;
                for (i = 0; i < 5; i++) { SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)snz[i]); if (a && a->snooze_min == mins[i]) sel = i; }
                SendMessageW(c, CB_SETCURSEL, sel, 0);
            }
            y += S(64);
        }
    } else {
        load_zones();
        dctl(L"STATIC", L"Enter a location", 0, x, y, S(300), S(20), -1);
        dctl(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, x, y + S(22), w - 2 * x - S(8), S(28), ID_SEARCH);
        y += S(58);
        dctl(L"LISTBOX", L"", WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, x, y, w - 2 * x - S(8), S(230), ID_LIST);
        y += S(242);
        fill_zone_list();
    }
    dctl(L"BUTTON", kind == D_CITY ? L"Add" : L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON, w - S(236), h - S(84), S(100), S(32), ID_SAVE);
    dctl(L"BUTTON", L"Cancel", WS_TABSTOP, w - S(128), h - S(84), S(100), S(32), ID_CANCEL);
    if (kind == D_ALARM && index >= 0) dctl(L"BUTTON", L"Delete", WS_TABSTOP, x, h - S(84), S(100), S(32), ID_DELETE);
    EnableWindow(g_wnd, FALSE);
    ShowWindow(g_dlg, SW_SHOW);
    SetFocus(GetDlgItem(g_dlg, kind == D_CITY ? ID_SEARCH : ID_H));
    write_dump();
}

/* ---- the dump ---------------------------------------------------------------------------------------- */
static void dump_point(FILE *f, const char *what, HWND h, RECT r, const WCHAR *label)
{
    POINT p = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    char buf[256];
    ClientToScreen(h, &p);
    WideCharToMultiByte(CP_UTF8, 0, label ? label : L"", -1, buf, sizeof(buf), NULL, NULL);
    fprintf(f, "%s at=%ld,%ld: %s\n", what, p.x, p.y, buf);
}

static BOOL CALLBACK dump_dlg_child(HWND c, LPARAM lp)
{
    FILE *f = (FILE *)lp;
    RECT r;
    WCHAR text[160];
    char buf[400];
    POINT p;
    GetWindowRect(c, &r);
    GetWindowTextW(c, text, ARRAYSIZE(text));
    if (GetDlgCtrlID(c) == ID_LIST) {
        int sel = (int)SendMessageW(c, LB_GETCURSEL, 0, 0);
        if (sel >= 0) SendMessageW(c, LB_GETTEXT, sel, (LPARAM)text);
    }
    p.x = (r.left + r.right) / 2; p.y = (r.top + r.bottom) / 2;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, buf, sizeof(buf), NULL, NULL);
    fprintf(f, "dlgctl %d at=%ld,%ld: %s\n", GetDlgCtrlID(c), p.x, p.y, buf);
    return TRUE;
}

static void write_dump(void)
{
    static WCHAR path[MAX_PATH];
    static int have = -1;
    WCHAR tmp[MAX_PATH + 8], buf[128];
    char a[256];
    FILE *f;
    int i;
    if (have < 0) have = GetEnvironmentVariableW(L"SG_CLOCK_DUMP", path, MAX_PATH) > 0;
    if (!have || !g_wnd) return;
    _snwprintf(tmp, ARRAYSIZE(tmp), L"%ls.tmp", path);
    if (!(f = _wfopen(tmp, L"wb"))) return;
    fprintf(f, "visible %d\npage %s\n", IsWindowVisible(g_wnd) ? 1 : 0, PAGE_KEYS[g_page]);
    for (i = 0; i < g_nbtns; i++) {
        static const char *const kinds[] = { "", "pivot", "add", "alarm", "alarmtoggle", "city", "cityremove", "timerstart",
                                             "timerreset", "timerremove", "swstart", "swlap", "swreset", "timer" };
        char what[32];
        snprintf(what, sizeof(what), "btn %s %d", kinds[g_btns[i].kind], g_btns[i].index);
        dump_point(f, what, g_wnd, g_btns[i].r, g_btns[i].label);
    }
    for (i = 0; i < g_nalarms; i++) {
        WideCharToMultiByte(CP_UTF8, 0, g_alarms[i].name, -1, a, sizeof(a), NULL, NULL);
        fprintf(f, "alarm %d %02d:%02d on=%d days=%d snoozed=%d: %s\n", i, g_alarms[i].hour, g_alarms[i].minute, g_alarms[i].on,
                g_alarms[i].days, g_alarms[i].snoozed_until ? 1 : 0, a);
    }
    for (i = 0; i < g_ncities; i++) {
        SYSTEMTIME st;
        LONG bias = 0;
        if (!zone_time(g_cities[i].key, &st, &bias)) memset(&st, 0, sizeof(st));
        WideCharToMultiByte(CP_UTF8, 0, g_cities[i].name, -1, a, sizeof(a), NULL, NULL);
        fprintf(f, "city %d %02d:%02d utc%+ld: %s\n", i, st.wHour, st.wMinute, bias, a);
    }
    for (i = 0; i < g_ntimers; i++) {
        WideCharToMultiByte(CP_UTF8, 0, g_timers[i].name, -1, a, sizeof(a), NULL, NULL);
        fprintf(f, "timer %d total=%lld left=%lld running=%d: %s\n", i, g_timers[i].total_ms, g_timers[i].left_ms, g_timers[i].running, a);
    }
    fmt_hms(sw_elapsed(), buf, ARRAYSIZE(buf), TRUE);
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, a, sizeof(a), NULL, NULL);
    fprintf(f, "stopwatch %lld running=%d laps=%d shown=%s\n", sw_elapsed(), g_sw_started ? 1 : 0, g_nlaps, a);
    for (i = 0; i < g_ntoasts; i++) {
        RECT s, d;
        char t[128], b[256], sub[128];
        WideCharToMultiByte(CP_UTF8, 0, g_toasts[i].title, -1, t, sizeof(t), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, g_toasts[i].body, -1, b, sizeof(b), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, g_toasts[i].sub, -1, sub, sizeof(sub), NULL, NULL);
        fprintf(f, "toast %d %s | %s | %s\n", i, t, b, sub);
        toast_buttons(&g_toasts[i], &s, &d);
        if (g_toasts[i].kind == TK_ALARM) dump_point(f, "toastbtn snooze", g_toasts[i].hwnd, s, L"Snooze");
        dump_point(f, "toastbtn dismiss", g_toasts[i].hwnd, d, L"Dismiss");
    }
    if (g_dlg) {
        GetWindowTextW(g_dlg, buf, ARRAYSIZE(buf));
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, a, sizeof(a), NULL, NULL);
        fprintf(f, "dialog %s\n", a);
        EnumChildWindows(g_dlg, dump_dlg_child, (LPARAM)f);
    }
    fclose(f);
    MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
}

/* ---- the window ---------------------------------------------------------------------------------------- */
static int hit(int x, int y)
{
    POINT pt = { x, y };
    int i;
    /* the smaller things on top of a card first */
    for (i = g_nbtns - 1; i >= 0; i--) if (PtInRect(&g_btns[i].r, pt)) return i;
    return -1;
}

static void set_page(enum page p)
{
    g_page = p;
    g_scroll = 0;
    {
        HKEY k;
        DWORD v = p;
        if (!RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) {
            RegSetValueExW(k, L"Page", 0, REG_DWORD, (BYTE *)&v, sizeof(v));
            RegCloseKey(k);
        }
    }
    relayout();
}

static void click(int i)
{
    struct btn *b;
    if (i < 0 || i >= g_nbtns) return;
    b = &g_btns[i];
    switch (b->kind) {
    case B_PIVOT: set_page((enum page)b->index); return;
    case B_ADD: open_dialog(g_page == P_ALARM ? D_ALARM : g_page == P_TIMER ? D_TIMER : D_CITY, -1); return;
    case B_ALARM_CARD: open_dialog(D_ALARM, b->index); return;
    case B_ALARM_TOGGLE:
        g_alarms[b->index].on = !g_alarms[b->index].on;
        g_alarms[b->index].snoozed_until = 0;
        g_alarms[b->index].fired_key = -1;
        save_all();
        break;
    case B_CITY_REMOVE:
        memmove(&g_cities[b->index], &g_cities[b->index + 1], (g_ncities - b->index - 1) * sizeof(g_cities[0]));
        g_ncities--;
        save_all();
        break;
    case B_TIMER_START: {
        struct ctimer *t = &g_timers[b->index];
        if (t->running) { t->running = FALSE; }
        else { t->running = TRUE; t->started = GetTickCount64(); t->left_at_start = t->left_ms > 0 ? t->left_ms : t->total_ms; }
        break;
    }
    case B_TIMER_RESET: g_timers[b->index].running = FALSE; g_timers[b->index].left_ms = g_timers[b->index].total_ms; break;
    case B_TIMER_REMOVE:
        memmove(&g_timers[b->index], &g_timers[b->index + 1], (g_ntimers - b->index - 1) * sizeof(g_timers[0]));
        g_ntimers--;
        save_all();
        break;
    case B_SW_START:
        if (g_sw_started) { g_sw_base = sw_elapsed(); g_sw_started = 0; }
        else g_sw_started = GetTickCount64();
        break;
    case B_SW_LAP: if (g_nlaps < MAX_LAPS && (g_sw_started || g_sw_base)) g_laps[g_nlaps++] = sw_elapsed(); break;
    case B_SW_RESET: g_sw_base = 0; g_sw_started = 0; g_nlaps = 0; break;
    default: return;
    }
    relayout();
}

static void show_main(void)
{
    g_background = FALSE;
    ShowWindow(g_wnd, IsIconic(g_wnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_wnd);
    write_dump();
}

#define COPYDATA_PAGE 0x53474350
static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE: relayout(); return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO *mm = (MINMAXINFO *)lp; mm->ptMinTrackSize.x = S(420); mm->ptMinTrackSize.y = S(420); return 0; }
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
        paint(mem, &r);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int h = hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h != g_hot) {
            TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
            g_hot = h; TrackMouseEvent(&t); InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE: g_hot = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONUP: click(hit(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))); return 0;
    case WM_MOUSEWHEEL:
        if (g_page != P_STOPWATCH) { g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) * S(60) / WHEEL_DELTA; if (g_scroll < 0) g_scroll = 0; relayout(); }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_SPACE && g_page == P_STOPWATCH) {
            int i;
            for (i = 0; i < g_nbtns; i++) if (g_btns[i].kind == B_SW_START) click(i);
        } else if (wp == 'L' && g_page == P_STOPWATCH) {
            int i;
            for (i = 0; i < g_nbtns; i++) if (g_btns[i].kind == B_SW_LAP) click(i);
        } else if (wp == VK_TAB && GetKeyState(VK_CONTROL) < 0) set_page((enum page)((g_page + (GetKeyState(VK_SHIFT) < 0 ? P_COUNT - 1 : 1)) % P_COUNT));
        return 0;
    case WM_TIMER: {
        static int last_sec = -1;
        SYSTEMTIME now;
        GetLocalTime(&now);
        if (wp == TIMER_TICK) {
            tick_timers();
            if (now.wSecond != last_sec) { last_sec = now.wSecond; check_alarms(); }
            if (IsWindowVisible(hwnd) && (g_page == P_TIMER || g_page == P_WORLD)) InvalidateRect(hwnd, NULL, FALSE);
            write_dump();
            if (g_background && !busy() && !IsWindowVisible(hwnd)) DestroyWindow(hwnd);
        } else if (wp == TIMER_FAST && g_page == P_STOPWATCH && g_sw_started && IsWindowVisible(hwnd)) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_COPYDATA: {
        COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp;
        if (cd->dwData == COPYDATA_PAGE && cd->cbData == sizeof(int)) {
            int p = *(int *)cd->lpData;
            if (p >= 0 && p < P_COUNT) set_page((enum page)p);
            if (p != -2) show_main();
            return TRUE;
        }
        return FALSE;
    }
    case WM_CLOSE:
        /* alarms and timers go on: the window goes, the program stays while they need it */
        if (busy()) { ShowWindow(hwnd, SW_HIDE); g_background = TRUE; write_dump(); return 0; }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HFONT font(int pt10, int weight)
{
    return CreateFontW(-MulDiv(pt10, g_dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

static int page_from_arg(const WCHAR *a)
{
    int i;
    if (!_wcsnicmp(a, L"ms-clock:", 9)) a += 9;
    for (i = 0; i < P_COUNT; i++) {
        WCHAR w[32];
        MultiByteToWideChar(CP_ACP, 0, PAGE_KEYS[i], -1, w, 32);
        if (!_wcsicmp(a, w) || (!_wcsnicmp(a, w, lstrlenW(w)) && (a[lstrlenW(w)] == L'?' || a[lstrlenW(w)] == L'/'))) return i;
    }
    if (!_wcsicmp(a, L"alarms")) return P_ALARM;
    if (!_wcsicmp(a, L"world") || !_wcsicmp(a, L"worldclocks")) return P_WORLD;
    return -1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    WNDCLASSW wc = { 0 };
    int argc = 0, i, want = -1;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    MSG msg;
    HDC dc;
    HANDLE mutex;
    (void)prev; (void)cmdline;

    for (i = 1; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/background") || !_wcsicmp(argv[i], L"-background")) g_background = TRUE;
        else if (page_from_arg(argv[i]) >= 0) want = page_from_arg(argv[i]);
    }
    /* one instance: a second start hands over its page (or just shows the window) */
    mutex = CreateMutexW(NULL, FALSE, L"Local\\StainedGlassAlarmsAndClock");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = NULL;
        for (i = 0; i < 50 && !(other = FindWindowW(L"SgClockWindow", NULL)); i++) Sleep(100);
        if (other) {
            int p = g_background ? -2 : want;
            COPYDATASTRUCT cd = { COPYDATA_PAGE, sizeof(int), &p };
            DWORD_PTR r;
            AllowSetForegroundWindow(ASFW_ANY);
            SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &r);
        }
        return 0;
    }
    (void)mutex;

    g_inst = inst;
    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    if (g_dpi < 96) g_dpi = 96;
    g_f_body = font(105, FW_NORMAL);
    g_f_small = font(90, FW_NORMAL);
    g_f_head = font(140, FW_SEMIBOLD);
    g_f_big = font(260, FW_LIGHT);
    g_f_huge = font(480, FW_LIGHT);
    g_f_pivot = font(140, FW_NORMAL);
    g_f_pivot_on = font(140, FW_SEMIBOLD);
    (void)g_f_glyph;
    make_chime();
    load_all();
    if (want >= 0) g_page = want;

    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpfnWndProc = toast_proc; wc.lpszClassName = L"SgClockToast";
    RegisterClassW(&wc);
    wc.lpfnWndProc = dlg_proc; wc.lpszClassName = L"SgClockDialog"; wc.hbrBackground = GetStockObject(WHITE_BRUSH);
    RegisterClassW(&wc);
    wc.hbrBackground = NULL;
    wc.lpfnWndProc = main_proc; wc.lpszClassName = L"SgClockWindow";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(0, L"SgClockWindow", L"Alarms & Clock", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            S(760), S(600), NULL, NULL, inst, NULL);
    if (!g_wnd) return 1;
    layout();
    if (!g_background) ShowWindow(g_wnd, show ? show : SW_SHOWNORMAL);
    SetTimer(g_wnd, TIMER_TICK, 250, NULL);
    SetTimer(g_wnd, TIMER_FAST, 40, NULL);
    update_run_key();
    write_dump();
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (g_dlg && IsDialogMessageW(g_dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
