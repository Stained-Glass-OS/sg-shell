/* sg-notify -- the notification centre: the taskbar's notification icon and
 * its panel, as Windows 10's action centre.
 *
 * Every toast an application shows (or sends only to the centre) and every
 * balloon a program shows from its notification-area icon is kept by
 * wine-sg (patch 0815) in the user's history,
 * HKCU\Software\Stained Glass\Notifications\History\<number> (App, Title,
 * Body, Launch, Protocol, Time), the newest 50, and this program is told
 * ("SgNotificationsChanged"). Its icon says whether there are new ones and
 * how many; clicking it, or Win+A (explorer runs "sg-notify.exe /toggle"),
 * opens a panel at the right of the screen listing them, newest first, with
 * the program, title, text and time of each. A notification's x dismisses
 * it, "Clear all" dismisses them all. Clicking one hands it to the program
 * that sent it, as Windows' action centre does, and it goes: wine-sg's
 * windows.ui (0819) SgActivateNotification gives it to the program's COM
 * activator (COM starting the program if need be), or to the running
 * program's toast (its Activated event) or tray icon (NIN_BALLOONUSERCLICK),
 * or brings the program to the front or starts it; one that opens a link (a
 * toast with a protocol launch) opens it. Opening the panel marks them seen
 * (Notifications\Seen).
 *
 * It also answers for runtimes a program needs and this system does not
 * have: wine-sg's loader (0817) names a missing DLL the store offers
 * (Store\Runtimes: msvbvm60.dll -> the Visual Basic 6 runtime) in
 * HKCU\Software\Stained Glass\Runtimes\Missing, and this program asks,
 * once a session for each, whether to install it -- Yes opens SG Store on
 * its page (sg-store --page), where Install is one touch away.
 *
 *   sg-notify              the icon (sg-session starts it with the shell)
 *   sg-notify /toggle      open or close the panel (of the running one)
 *   sg-notify --dump       what the panel would list (stderr)
 *   SG_NOTIFY_DUMP=FILE    what the panel shows, written after every paint
 *                          (the gate)
 *
 * The panel ends in Windows 10's quick action tiles (Network, All settings,
 * Night light, Bluetooth, Project, Screen snip; see qa_paint).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "sg-mode.h"
#include "sg-smooth.h"
#include "sg-round.h"
#include "sg-dpi.h"
#include "sg-flyout.h"

#define WM_TRAY   (WM_APP + 1)
#define WM_TOGGLE (WM_APP + 2)
#define HISTORY_KEY L"Software\\Stained Glass\\Notifications\\History"
#define NOTIFY_KEY  L"Software\\Stained Glass\\Notifications"
#define MAX_ENTRIES 64
#define MISSING_KEY  L"Software\\Stained Glass\\Runtimes\\Missing"
#define RUNTIMES_KEY L"Software\\Stained Glass\\Store\\Runtimes"
#define WM_MISSING (WM_APP + 3)
static void ask_missing(void);
static DWORD WINAPI watch_missing(void *arg);

struct entry
{
    DWORD seq;
    WCHAR app[128], title[256], body[1024], launch[512];
    BOOL protocol;
    ULONGLONG time;
    RECT rect, close;        /* where it is drawn (panel coordinates), its x */
};

static HWND g_tray, g_fly;
static NOTIFYICONDATAW g_nid;
static HFONT g_font, g_font_bold, g_font_small, g_font_head;
static UINT g_taskbar_created, g_changed;
static struct entry g_entries[MAX_ENTRIES];
static int g_count, g_hot = -1, g_scroll, g_s8 = 8, g_dpi = 96;
static BOOL g_clear_hot, g_hot_close;
static RECT g_clear_rect;
static const WCHAR *g_dump;

/* the screen's scale, as the title bars, the taskbar and Start have it: the
 * looks' (Scale8) at the display scale (per-monitor v2, sg-dpi.h: drawn by
 * us at it, not scaled by Wine -- the same size, crisp) */
#ifndef SG_MUTANT_NOTIFY_DPI_IGNORED
static int U(int v) { return (v * g_s8 * g_dpi + 384) / 768; }
#else
static int U(int v) { return (v * g_s8 + 4) / 8; }
#endif

static DWORD reg_dword(const WCHAR *key, const WCHAR *name, DWORD def)
{
    DWORD v = def, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, NULL, &v, &size)) return def;
    return v;
}

static void reg_set(const WCHAR *key, const WCHAR *name, DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
    RegCloseKey(k);
}

/* --- the history ------------------------------------------------------------------ */

static void read_sz(HKEY k, const WCHAR *name, WCHAR *out, DWORD cch)
{
    DWORD size = cch * sizeof(WCHAR);
    out[0] = 0;
    if (RegGetValueW(k, NULL, name, RRF_RT_REG_SZ, NULL, out, &size)) out[0] = 0;
}

static int by_newest(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;
    return x->seq < y->seq ? 1 : x->seq > y->seq ? -1 : 0;
}

static void load_history(void)
{
    HKEY key, item;
    WCHAR name[32];
    DWORD i, len;

    g_count = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, HISTORY_KEY, 0, KEY_READ, &key)) return;
    for (i = 0; g_count < MAX_ENTRIES; i++)
    {
        struct entry *e = &g_entries[g_count];
        DWORD size = sizeof(e->time), proto = 0, psize = sizeof(proto);
        len = ARRAYSIZE(name);
        if (RegEnumKeyExW(key, i, name, &len, NULL, NULL, NULL, NULL)) break;
        if (RegOpenKeyExW(key, name, 0, KEY_READ, &item)) continue;
        memset(e, 0, sizeof(*e));
        e->seq = wcstoul(name, NULL, 10);
        read_sz(item, L"App", e->app, ARRAYSIZE(e->app));
        read_sz(item, L"Title", e->title, ARRAYSIZE(e->title));
        read_sz(item, L"Body", e->body, ARRAYSIZE(e->body));
        read_sz(item, L"Launch", e->launch, ARRAYSIZE(e->launch));
        RegGetValueW(item, NULL, L"Time", RRF_RT_REG_QWORD, NULL, &e->time, &size);
        RegGetValueW(item, NULL, L"Protocol", RRF_RT_REG_DWORD, NULL, &proto, &psize);
        e->protocol = proto != 0;
        RegCloseKey(item);
        g_count++;
    }
    RegCloseKey(key);
    qsort(g_entries, g_count, sizeof(g_entries[0]), by_newest);
}

static int unread(void)
{
    DWORD seen = reg_dword(NOTIFY_KEY, L"Seen", 0);
    int i, n = 0;
    for (i = 0; i < g_count; i++) if (g_entries[i].seq >= seen) n++;
    return n;
}

static void mark_seen(void)
{
    reg_set(NOTIFY_KEY, L"Seen", reg_dword(HISTORY_KEY, L"Next", 0));
}

static void dismiss(DWORD seq)
{
    WCHAR path[128];
    _snwprintf(path, ARRAYSIZE(path), L"%ls\\%08lu", HISTORY_KEY, (unsigned long)seq);
    path[ARRAYSIZE(path) - 1] = 0;
    if (!RegDeleteKeyW(HKEY_CURRENT_USER, path)) return;
    /* an entry named without the leading zeros (sg-restart-notice before
     * 0.1.0-278 wrote "0"): Clear all left it there for good */
    _snwprintf(path, ARRAYSIZE(path), L"%ls\\%lu", HISTORY_KEY, (unsigned long)seq);
    path[ARRAYSIZE(path) - 1] = 0;
    RegDeleteKeyW(HKEY_CURRENT_USER, path);
}

static void clear_all(void)
{
    int i;
    load_history();
    for (i = 0; i < g_count; i++) dismiss(g_entries[i].seq);
    g_count = 0;
}

static void when(const struct entry *e, WCHAR *out, int cch)
{
    FILETIME utc, local;
    SYSTEMTIME st, now;
    utc.dwLowDateTime = (DWORD)e->time;
    utc.dwHighDateTime = (DWORD)(e->time >> 32);
    out[0] = 0;
    if (!e->time || !FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return;
    GetLocalTime(&now);
    if (st.wYear == now.wYear && st.wMonth == now.wMonth && st.wDay == now.wDay)
        GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, out, cch);
    else
        GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, out, cch);
}

/* --- the icon: a speech bubble, filled when there are new ones --------------------- */

/* sg-smooth: drawn on a canvas four times larger, reduced to soft-edged alpha */
static HICON make_icon(int size, int fresh)
{
    enum { K = 4 };
    int S = size * K, x, y;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), S, -S, 1, 32, BI_RGB } };
    BITMAPINFO bo = { { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB } };
    DWORD *big, *px;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP canvas = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&big, NULL, 0), color, mask, old;
    ICONINFO ii = { TRUE };
    HICON icon;
    int pw = S / 14;
    HPEN pen = CreatePen(PS_SOLID, pw, RGB(0xFF, 0xFF, 0xFF)), op;
    HBRUSH white = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF)), ob;
    POINT tail[3] = { { S * 5 / 8, S * 11 / 16 }, { S * 13 / 16, S * 15 / 16 }, { S * 13 / 16, S * 11 / 16 } };
    COLORREF fg = sg_system_dark() ? 0x00FFFFFF : 0;

    old = SelectObject(dc, canvas);
    memset(big, 0, S * S * 4);
    op = SelectObject(dc, pen);
    ob = SelectObject(dc, fresh ? (HGDIOBJ)white : GetStockObject(NULL_BRUSH));
    Rectangle(dc, S / 8, S / 6, S - S / 8, S * 11 / 16);
    SelectObject(dc, white);
    Polygon(dc, tail, 3);
    if (!fresh)
    {
        /* three lines of text in the empty bubble */
        for (y = 0; y < 3; y++)
        {
            RECT l = { S / 4, S / 4 + y * S / 8, S - S / 4, S / 4 + y * S / 8 + pw };
            FillRect(dc, &l, white);
        }
    }
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(pen); DeleteObject(white);
    GdiFlush();

    color = CreateDIBSection(dc, &bo, DIB_RGB_COLORS, (void **)&px, NULL, 0);
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++)
        {
            int sum = 0, u, v;
            BYTE a;
            for (v = 0; v < K; v++) for (u = 0; u < K; u++) sum += big[(y * K + v) * S + x * K + u] & 0xFF;
            a = (BYTE)(sum / (K * K));
            px[y * size + x] = a ? ((DWORD)a << 24) | fg : 0;
        }
    SelectObject(dc, old);
    DeleteObject(canvas);
    mask = CreateBitmap(size, size, 1, 1, NULL);
    ii.hbmColor = color;
    ii.hbmMask = mask;
    icon = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask); DeleteDC(dc);
    return icon;
}

static void tray_update(BOOL add)
{
    HICON old = g_nid.hIcon;
    int n;

    load_history();
    n = unread();
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_tray;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = make_icon(U(16), n);
    if (n == 1) lstrcpyW(g_nid.szTip, L"1 new notification");
    else if (n) _snwprintf(g_nid.szTip, ARRAYSIZE(g_nid.szTip), L"%d new notifications", n);
    else lstrcpyW(g_nid.szTip, L"No new notifications");
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_nid);
    if (old) DestroyIcon(old);
}

/* --- a click on a notification ------------------------------------------------------ */

struct activation
{
    DWORD seq;
    WCHAR launch[512];
    BOOL link;
};

static DWORD g_activated = (DWORD)-1;
static HRESULT g_activated_hr;

/* in a thread of its own: COM may take a while to start the program */
static DWORD WINAPI activate_thread(void *arg)
{
    typedef HRESULT (WINAPI *activate_fn)(DWORD, const WCHAR *);
    struct activation *a = arg;
    HMODULE ui = LoadLibraryW(L"windows.ui.dll");
    activate_fn activate = ui ? (activate_fn)(void (*)(void))GetProcAddress(ui, "SgActivateNotification") : NULL;
    HRESULT hr = E_NOTIMPL;

#ifndef SG_MUTANT_NOTIFY_NO_ACTIVATE
    if (activate) hr = activate(a->seq, NULL);
    else if (a->link)
        /* an older wine-sg: a link at least */
        hr = (INT_PTR)ShellExecuteW(NULL, NULL, a->launch, NULL, NULL, SW_SHOWNORMAL) > 32 ? S_OK : E_FAIL;
#endif
    g_activated_hr = hr;
    g_activated = a->seq;
    /* it goes from the centre, as on Windows */
    if (activate || a->link) dismiss(a->seq);
    PostMessageW(g_tray, g_changed, 0, 0);
    free(a);
    return 0;
}

static void activate_entry(const struct entry *e)
{
    struct activation *a = calloc(1, sizeof(*a));
    HANDLE thread;
    if (!a) return;
    a->seq = e->seq;
    a->link = e->protocol && e->launch[0];
    lstrcpynW(a->launch, e->launch, ARRAYSIZE(a->launch));
    if ((thread = CreateThread(NULL, 0, activate_thread, a, 0, NULL))) CloseHandle(thread);
    else free(a);
}

/* --- the panel --------------------------------------------------------------------- */

/* ---- quick actions ------------------------------------------------------
 * Windows 10's action centre ends in quick action tiles: Network, All
 * settings, Night light, Bluetooth, Project, Screen snip (each where this
 * computer has it: Bluetooth with an adapter, Night light where the
 * compositor can tint). A toggle's tile is in the accent while on. The state
 * and the toggles go through sg-settingsctl (nightlight on|off, bluetooth
 * power on|off), read on a thread when the panel opens; the others open what
 * Windows' open: the network flyout, Settings, the Project pane, screen
 * snipping. */
enum { QA_NETWORK, QA_SETTINGS, QA_NIGHT, QA_BLUETOOTH, QA_PROJECT, QA_SNIP, QA_COUNT };
static const WCHAR *const qa_names[QA_COUNT] = { L"Network", L"All settings", L"Night light", L"Bluetooth",
                                                 L"Project", L"Screen snip" };
static RECT g_qa[QA_COUNT];
static int g_qa_hot = -1, g_qa_top;
static volatile LONG g_night = -1, g_bt = -1;   /* -1: not on this computer (or not known yet), 0 off, 1 on */
#define WM_QA_STATE (WM_APP + 4)

/* sg-settingsctl ARGS: its answer (UTF-8, caller frees), as Settings' ctl_run */
static char *qa_ctl(const WCHAR *args)
{
    static LONG seq;
    char *(CDECL *to_unix)(const WCHAR *) = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    WCHAR tool[MAX_PATH] = L"/usr/bin/sg-settingsctl", dir[MAX_PATH], dos[MAX_PATH], cmd[1024], *p;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    char *unix_out, *text = NULL;
    DWORD waited = 0, size, got = 0;
    HANDLE h;

    GetEnvironmentVariableW(L"SG_SETTINGSCTL", tool, MAX_PATH);
    if (!to_unix || !GetTempPathW(MAX_PATH, dir)) return NULL;
    _snwprintf(dos, MAX_PATH, L"%lssg-notify-%lu-%ld.txt", dir, GetCurrentProcessId(), InterlockedIncrement(&seq));
    dos[MAX_PATH - 1] = 0;
    CloseHandle(CreateFileW(dos, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
    if (!(unix_out = to_unix(dos))) return NULL;
    DeleteFileW(dos);
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\\\\?\\unix%ls %ls --out %S", tool, args, unix_out);
    cmd[ARRAYSIZE(cmd) - 1] = 0;
    HeapFree(GetProcessHeap(), 0, unix_out);
    for (p = cmd + 8; *p && *p != L' '; p++) if (*p == L'/') *p = L'\\';
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return NULL;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    while (GetFileAttributesW(dos) == INVALID_FILE_ATTRIBUTES && waited < 8000) { Sleep(40); waited += 40; }
    h = CreateFileW(dos, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(h, NULL);
    if (size < 65536 && (text = malloc(size + 1))) { ReadFile(h, text, size, &got, NULL); text[got] = 0; }
    CloseHandle(h);
    DeleteFileW(dos);
    return text;
}

static DWORD WINAPI qa_read_state(void *arg)
{
    char *a;
    LONG night = -1, bt = -1;
    (void)arg;
    if ((a = qa_ctl(L"nightlight")))
    {
        /* NIGHTLIGHT on|off <TAB> temp <TAB> available ... */
        char *l = strstr(a, "NIGHTLIGHT ");
        if (l && strstr(l, "\tyes")) night = !strncmp(l + 11, "on", 2);
        free(a);
    }
    if ((a = qa_ctl(L"bluetooth")))
    {
        if (strstr(a, "BLUETOOTH yes")) bt = strstr(a, "POWERED yes") != NULL;
        free(a);
    }
    InterlockedExchange(&g_night, night);
    InterlockedExchange(&g_bt, bt);
    PostMessageW(g_fly, WM_QA_STATE, 0, 0);
    return 0;
}

static BOOL qa_shown(int i)
{
    if (i == QA_NIGHT) return g_night >= 0;
    if (i == QA_BLUETOOTH) return g_bt >= 0;
    return TRUE;
}

static int qa_on(int i)
{
    if (i == QA_NIGHT) return g_night == 1;
    if (i == QA_BLUETOOTH) return g_bt == 1;
    return 0;
}

/* each tile's picture: plain strokes, as the action centre's glyphs */
static void qa_glyph(HDC dc, int i, int x, int y, int sz, COLORREF col, COLORREF back)
{
    HPEN pen = CreatePen(PS_SOLID, U(8) >= 12 ? 2 : 1, col), op = SelectObject(dc, pen);
    HBRUSH ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int k, m = sz / 2;

    switch (i)
    {
    case QA_NETWORK:   /* a screen on a stand (a wired network) */
        sg_line(dc, x + 1, y + 2, x + sz - 1, y + 2);
        sg_line(dc, x + sz - 1, y + 2, x + sz - 1, y + sz * 2 / 3);
        sg_line(dc, x + sz - 1, y + sz * 2 / 3, x + 1, y + sz * 2 / 3);
        sg_line(dc, x + 1, y + sz * 2 / 3, x + 1, y + 2);
        sg_line(dc, x + m, y + sz * 2 / 3, x + m, y + sz - 2);
        sg_line(dc, x + m - sz / 4, y + sz - 2, x + m + sz / 4, y + sz - 2);
        break;
    case QA_SETTINGS:  /* a gear: a ring and its teeth */
        sg_ellipse(dc, x + sz / 4, y + sz / 4, x + sz - sz / 4, y + sz - sz / 4);
        for (k = 0; k < 8; k++)
        {
            double a = k * 3.14159265 / 4;
            int x0 = x + m + (int)(cos(a) * sz * 0.30), y0 = y + m + (int)(sin(a) * sz * 0.30);
            int x1 = x + m + (int)(cos(a) * sz * 0.46), y1 = y + m + (int)(sin(a) * sz * 0.46);
            sg_line(dc, x0, y0, x1, y1);
        }
        break;
    case QA_NIGHT:     /* a crescent moon: the moon, less a disc of the tile's colour */
    {
        HBRUSH fg = CreateSolidBrush(col), bk = CreateSolidBrush(back);
        HPEN none = GetStockObject(NULL_PEN);
        SelectObject(dc, none);
        SelectObject(dc, fg);
        sg_ellipse(dc, x + 1, y + 1, x + sz - 1, y + sz - 1);
        SelectObject(dc, bk);
        sg_ellipse(dc, x + sz * 2 / 5, y - sz / 8, x + sz + sz / 4, y + sz * 3 / 4);
        SelectObject(dc, pen);
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        DeleteObject(fg);
        DeleteObject(bk);
        break;
    }
    case QA_BLUETOOTH: /* the rune */
        sg_line(dc, x + m, y + 1, x + m, y + sz - 1);
        sg_line(dc, x + m, y + 1, x + sz * 3 / 4, y + sz / 4);
        sg_line(dc, x + sz * 3 / 4, y + sz / 4, x + sz / 4, y + sz * 3 / 4);
        sg_line(dc, x + m, y + sz - 1, x + sz * 3 / 4, y + sz * 3 / 4);
        sg_line(dc, x + sz * 3 / 4, y + sz * 3 / 4, x + sz / 4, y + sz / 4);
        break;
    case QA_PROJECT:   /* two screens */
        Rectangle(dc, x, y + 2, x + sz * 3 / 5, y + sz * 3 / 5);
        Rectangle(dc, x + sz * 2 / 5, y + sz * 2 / 5, x + sz, y + sz - 2);
        break;
    default:           /* a snip: a dashed frame */
        for (k = 0; k < sz; k += 4)
        {
            sg_line(dc, x + k, y + 1, x + min(k + 2, sz), y + 1);
            sg_line(dc, x + k, y + sz - 1, x + min(k + 2, sz), y + sz - 1);
            sg_line(dc, x + 1, y + k, x + 1, y + min(k + 2, sz));
            sg_line(dc, x + sz - 1, y + k, x + sz - 1, y + min(k + 2, sz));
        }
        break;
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

/* the tiles, four to a row, at the foot of the panel; g_qa_top is where they start */
static void qa_layout(int w, int h)
{
    int cols = 4, gap = U(2), pad = U(12), tw = (w - 2 * pad - (cols - 1) * gap) / cols, th = U(58), n = 0, i, rows;

    for (i = 0; i < QA_COUNT; i++) if (qa_shown(i)) n++;
    rows = (n + cols - 1) / cols;
    g_qa_top = h - pad - rows * th - (rows - 1) * gap - U(8);
    n = 0;
    for (i = 0; i < QA_COUNT; i++)
    {
        int r, c;
        if (!qa_shown(i)) { SetRectEmpty(&g_qa[i]); continue; }
        r = n / cols; c = n % cols; n++;
        SetRect(&g_qa[i], pad + c * (tw + gap), g_qa_top + U(8) + r * (th + gap),
                pad + c * (tw + gap) + tw, g_qa_top + U(8) + r * (th + gap) + th);
    }
}

static void qa_paint(HDC dc, int w, int h, COLORREF bg, COLORREF card, COLORREF card_hot, COLORREF text)
{
    COLORREF accent = sg_accent();
    RECT r;
    HBRUSH b;
    int i;

    qa_layout(w, h);
    SetRect(&r, 0, g_qa_top, w, h);
    b = CreateSolidBrush(bg);
    FillRect(dc, &r, b);
    DeleteObject(b);
    SelectObject(dc, g_font_small);
    for (i = 0; i < QA_COUNT; i++)
    {
        RECT t;
        int on = qa_on(i);
        if (IsRectEmpty(&g_qa[i])) continue;
        b = CreateSolidBrush(on ? accent : i == g_qa_hot ? card_hot : card);
        FillRect(dc, &g_qa[i], b);
        DeleteObject(b);
        qa_glyph(dc, i, g_qa[i].left + U(10), g_qa[i].top + U(8), U(16), on ? RGB(0xFF, 0xFF, 0xFF) : text,
                 on ? accent : i == g_qa_hot ? card_hot : card);
        SetTextColor(dc, on ? RGB(0xFF, 0xFF, 0xFF) : text);
        SetRect(&t, g_qa[i].left + U(10), g_qa[i].bottom - U(24), g_qa[i].right - U(4), g_qa[i].bottom - U(4));
        DrawTextW(dc, qa_names[i], -1, &t, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

static int qa_at(POINT pt)
{
    int i;
    for (i = 0; i < QA_COUNT; i++) if (!IsRectEmpty(&g_qa[i]) && PtInRect(&g_qa[i], pt)) return i;
    return -1;
}

static void show_panel(BOOL show);

static DWORD WINAPI qa_toggle_thread(void *arg)
{
    int i = (int)(INT_PTR)arg & 0xff, on = ((int)(INT_PTR)arg >> 8) & 1;
#ifdef SG_MUTANT_QA_NO_TOGGLE
    return 0;
#endif
    free(qa_ctl(i == QA_NIGHT ? (on ? L"nightlight on" : L"nightlight off")
                              : (on ? L"bluetooth power on" : L"bluetooth power off")));
    qa_read_state(NULL);
    return 0;
}

static void qa_click(int i)
{
    HWND net;
    switch (i)
    {
    case QA_NIGHT:
    case QA_BLUETOOTH:
    {
        /* turned at once on the tile, as Windows' (the answer follows) */
        int on = !qa_on(i);
        InterlockedExchange(i == QA_NIGHT ? &g_night : &g_bt, on);
        InvalidateRect(g_fly, NULL, FALSE);
        CloseHandle(CreateThread(NULL, 0, qa_toggle_thread, (void *)(INT_PTR)(i | (on << 8)), 0, NULL));
        return;
    }
    case QA_NETWORK:
        show_panel(FALSE);
        AllowSetForegroundWindow(ASFW_ANY);
        if ((net = FindWindowW(L"SgNetworkTray", NULL))) PostMessageW(net, WM_APP + 1, 1, WM_LBUTTONUP);
        else ShellExecuteW(NULL, NULL, L"ms-settings:network-status", NULL, NULL, SW_SHOWNORMAL);
        return;
    case QA_SETTINGS:
        show_panel(FALSE);
        AllowSetForegroundWindow(ASFW_ANY);
        ShellExecuteW(NULL, NULL, L"ms-settings:", NULL, NULL, SW_SHOWNORMAL);
        return;
    case QA_PROJECT:
        show_panel(FALSE);
        AllowSetForegroundWindow(ASFW_ANY);
        ShellExecuteW(NULL, NULL, L"ms-settings-displays-topology:projection", NULL, NULL, SW_SHOWNORMAL);
        return;
    case QA_SNIP:
        show_panel(FALSE);
        AllowSetForegroundWindow(ASFW_ANY);
        ShellExecuteW(NULL, NULL, L"snippingtool.exe", L"/clip", NULL, SW_SHOWNORMAL);
        return;
    }
}

static void dump(void)
{
    FILE *f;
    RECT wr;
    int i;
    char a[1200];
    if (!g_dump || !(f = _wfopen(g_dump, L"wb"))) return;
    GetWindowRect(g_fly, &wr);
    fprintf(f, "visible=%d\nrect=%ld,%ld,%ld,%ld\ncount=%d\nunread=%d\n", IsWindowVisible(g_fly), wr.left, wr.top,
            wr.right, wr.bottom, g_count, unread());
    fprintf(f, "clear=%ld,%ld,%ld,%ld\n", g_clear_rect.left, g_clear_rect.top, g_clear_rect.right, g_clear_rect.bottom);
    if (g_activated != (DWORD)-1) fprintf(f, "activated=%lu hr=%08lx\n", (unsigned long)g_activated, (unsigned long)g_activated_hr);
    for (i = 0; i < QA_COUNT; i++)
        if (!IsRectEmpty(&g_qa[i]))
        {
            WideCharToMultiByte(CP_UTF8, 0, qa_names[i], -1, a, sizeof(a), NULL, NULL);
            fprintf(f, "tile %ld,%ld,%ld,%ld on=%d %s\n", g_qa[i].left, g_qa[i].top, g_qa[i].right, g_qa[i].bottom, qa_on(i), a);
        }
    for (i = 0; i < g_count; i++)
    {
        struct entry *e = &g_entries[i];
        WideCharToMultiByte(CP_UTF8, 0, e->title, -1, a, sizeof(a), NULL, NULL);
        fprintf(f, "entry %lu %ld,%ld,%ld,%ld close=%ld,%ld,%ld,%ld %s\n", (unsigned long)e->seq, e->rect.left, e->rect.top,
                e->rect.right, e->rect.bottom, e->close.left, e->close.top, e->close.right, e->close.bottom, a);
    }
    fclose(f);
}

static void fly_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps), dc;
    BOOL dark = sg_system_dark();
    COLORREF bg = dark ? RGB(0x1F, 0x1F, 0x1F) : RGB(0xF2, 0xF2, 0xF2), card = dark ? RGB(0x2B, 0x2B, 0x2B) : RGB(0xFF, 0xFF, 0xFF),
             card_hot = dark ? RGB(0x36, 0x36, 0x36) : RGB(0xF8, 0xF8, 0xF8),
             text = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0), subtle = dark ? RGB(0xA8, 0xA8, 0xA8) : RGB(0x5A, 0x5A, 0x5A),
             link = dark ? sg_accent_shade(RGB(0xB9, 0x8C, 0xF0)) : sg_accent();
    HBITMAP buf, oldbuf;
    HBRUSH b;
    RECT c, r;
    int y, i, w;
    WCHAR t[64];

    GetClientRect(hwnd, &c);
    w = c.right;
    dc = CreateCompatibleDC(wdc);
    buf = CreateCompatibleBitmap(wdc, c.right, c.bottom);
    oldbuf = SelectObject(dc, buf);
    b = CreateSolidBrush(bg);
    FillRect(dc, &c, b);
    DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);

    /* the heading, and Clear all */
    SelectObject(dc, g_font_head);
    SetTextColor(dc, text);
    SetRect(&r, U(20), U(14), w - U(120), U(50));
    DrawTextW(dc, L"Notifications", -1, &r, DT_SINGLELINE | DT_VCENTER);
    SetRectEmpty(&g_clear_rect);
    if (g_count)
    {
        LOGFONTW lf;
        HFONT ul;
        SIZE sz;
        GetObjectW(g_font, sizeof(lf), &lf);
        lf.lfUnderline = g_clear_hot;
        ul = CreateFontIndirectW(&lf);
        SelectObject(dc, ul);
        GetTextExtentPoint32W(dc, L"Clear all", 9, &sz);
        SetRect(&g_clear_rect, w - U(20) - sz.cx, U(14), w - U(20), U(50));
        SetTextColor(dc, link);
        DrawTextW(dc, L"Clear all", -1, &g_clear_rect, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        SelectObject(dc, g_font);
        DeleteObject(ul);
    }

    /* the notifications, newest first */
    y = U(60) - g_scroll;
    if (!g_count)
    {
        SelectObject(dc, g_font);
        SetTextColor(dc, subtle);
        SetRect(&r, U(20), U(70), w - U(20), U(110));
        DrawTextW(dc, L"No new notifications", -1, &r, DT_SINGLELINE | DT_VCENTER);
    }
    for (i = 0; i < g_count; i++)
    {
        struct entry *e = &g_entries[i];
        RECT body, head, title, cr;
        int bh = 0, top = y, pad = U(12);

        /* how tall: the body wraps, up to four lines */
        if (e->body[0])
        {
            SelectObject(dc, g_font);
            SetRect(&body, U(12) + pad, 0, w - U(12) - pad, 0);
            DrawTextW(dc, e->body, -1, &body, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
            bh = min(body.bottom - body.top, U(19) * 4);
        }
        SetRect(&e->rect, U(12), top, w - U(12), top + pad + U(18) + U(4) + (e->title[0] ? U(22) : 0) + bh + pad);
        b = CreateSolidBrush(i == g_hot ? card_hot : card);
        FillRect(dc, &e->rect, b);
        DeleteObject(b);

        /* the program, the time */
        SelectObject(dc, g_font_small);
        SetTextColor(dc, subtle);
        SetRect(&head, e->rect.left + pad, top + pad, e->rect.right - pad - U(24), top + pad + U(18));
        when(e, t, ARRAYSIZE(t));
        DrawTextW(dc, e->app[0] ? e->app : L"A program", -1, &head, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        DrawTextW(dc, t, -1, &head, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        /* its x, under the pointer */
        SetRect(&e->close, e->rect.right - pad - U(16), top + pad, e->rect.right - pad + U(2), top + pad + U(18));
        if (i == g_hot)
        {
            HPEN pen = CreatePen(PS_SOLID, U(8) >= 12 ? 2 : 1, g_hot_close ? text : subtle), op = SelectObject(dc, pen);
            int cx = (e->close.left + e->close.right) / 2, cy = (e->close.top + e->close.bottom) / 2, d = U(5);
            sg_line(dc, cx - d, cy - d, cx + d + 1, cy + d + 1);
            sg_line(dc, cx + d, cy - d, cx - d - 1, cy + d + 1);
            SelectObject(dc, op);
            DeleteObject(pen);
        }
        y = top + pad + U(18) + U(4);
        if (e->title[0])
        {
            SelectObject(dc, g_font_bold);
            SetTextColor(dc, text);
            SetRect(&title, e->rect.left + pad, y, e->rect.right - pad, y + U(22));
            DrawTextW(dc, e->title, -1, &title, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            y += U(22);
        }
        if (bh)
        {
            SelectObject(dc, g_font);
            SetTextColor(dc, text);
            SetRect(&cr, e->rect.left + pad, y, e->rect.right - pad, y + bh);
            DrawTextW(dc, e->body, -1, &cr, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX | DT_EDITCONTROL);
        }
        y = e->rect.bottom + U(8);
    }
#ifndef SG_MUTANT_QA_NONE
    qa_paint(dc, w, c.bottom, bg, card, card_hot, text);
#endif
    BitBlt(wdc, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldbuf);
    DeleteObject(buf);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
    dump();
}

static void show_panel(BOOL show)
{
    RECT work;
    if (!show)
    {
        ShowWindow(g_fly, SW_HIDE);
        dump();
        return;
    }
    g_s8 = (int)reg_dword(L"Software\\Stained Glass\\Style", L"Scale8", 8);
    if (g_s8 < 8) g_s8 = 8;
    if (g_s8 > 24) g_s8 = 24;
    load_history();
    g_hot = -1;
    g_qa_hot = -1;
    g_scroll = 0;
    CloseHandle(CreateThread(NULL, 0, qa_read_state, NULL, 0, NULL));
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    /* the right of the screen, the work area's height, as Windows 10's */
    SetWindowPos(g_fly, HWND_TOPMOST, work.right - U(396), work.top, U(396), work.bottom - work.top, SWP_NOACTIVATE);
    ShowWindow(g_fly, SW_SHOW);
    SetForegroundWindow(g_fly);
    InvalidateRect(g_fly, NULL, FALSE);
    mark_seen();
    tray_update(FALSE);
}

static int entry_at(POINT pt, BOOL *on_close)
{
    int i;
    for (i = 0; i < g_count; i++)
        if (PtInRect(&g_entries[i].rect, pt))
        {
            if (on_close) *on_close = PtInRect(&g_entries[i].close, pt);
            return i;
        }
    if (on_close) *on_close = FALSE;
    return -1;
}

static void make_fonts(void);

static LRESULT CALLBACK fly_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT: fly_paint(hwnd); return 0;
#ifndef SG_MUTANT_NOTIFY_DPI_IGNORED
    case WM_DPICHANGED:
    {
        /* a new display scale: the panel and its text at it, at the screen's
         * right again; the taskbar's icon too */
        HFONT old[4] = { g_font, g_font_small, g_font_bold, g_font_head };
        int i;
        g_dpi = (int)sg_dpi_new(wp);
        make_fonts();
        for (i = 0; i < 4; i++) DeleteObject(old[i]);
        if (IsWindowVisible(hwnd)) show_panel(TRUE);
        tray_update(FALSE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
#endif
    case WM_ERASEBKGND: return 1;
    case WM_QA_STATE: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
        {
            if (IsWindowVisible(hwnd)) sg_flyout_hidden(g_tray, 1);
            show_panel(FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) show_panel(FALSE);
        return 0;
    case WM_MOUSEWHEEL:
    {
        RECT c;
        int most = 0;
        GetClientRect(hwnd, &c);
        if (g_count) most = max(0, g_entries[g_count - 1].rect.bottom + g_scroll + U(12) - c.bottom);
        g_scroll = max(0, min(most, g_scroll - GET_WHEEL_DELTA_WPARAM(wp) * U(60) / WHEEL_DELTA));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        BOOL close, clear = PtInRect(&g_clear_rect, pt);
        int qa = qa_at(pt), hot = qa >= 0 || pt.y >= g_qa_top ? -1 : entry_at(pt, &close);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        if (hot < 0) close = FALSE;
        if (hot != g_hot || close != g_hot_close || clear != g_clear_hot || qa != g_qa_hot)
        {
            g_hot = hot; g_hot_close = close; g_clear_hot = clear; g_qa_hot = qa;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        SetCursor(LoadCursorW(NULL, (const WCHAR *)(clear || (hot >= 0 && !close) ? IDC_HAND : IDC_ARROW)));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hot != -1 || g_clear_hot || g_qa_hot != -1) { g_hot = -1; g_qa_hot = -1; g_clear_hot = FALSE; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        BOOL close;
        int i;
        if ((i = qa_at(pt)) >= 0)
        {
            qa_click(i);
            return 0;
        }
        if (pt.y >= g_qa_top) return 0;
        if (PtInRect(&g_clear_rect, pt) && g_count)
        {
#ifndef SG_MUTANT_NOTIFY_NO_CLEAR
            clear_all();
#endif
            g_hot = -1;
            tray_update(FALSE);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if ((i = entry_at(pt, &close)) < 0) return 0;
        if (!close)
        {
            /* handed to its program (or its link opened), and it goes */
            activate_entry(&g_entries[i]);
            show_panel(FALSE);
            return 0;
        }
        if (close)
        {
            dismiss(g_entries[i].seq);
            load_history();
            g_hot = -1;
            tray_update(FALSE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbar_created && g_taskbar_created) { tray_update(TRUE); return 0; }
    if (msg == g_changed && g_changed)
    {
        /* a notification came (wine-sg 0815): the icon says so; an open
         * panel lists it at once, as seen */
        if (IsWindowVisible(g_fly))
        {
            load_history();
            mark_seen();
            InvalidateRect(g_fly, NULL, FALSE);
        }
        tray_update(FALSE);
        dump();
        return 0;
    }
    if (sg_mode_changed(msg, lp))
    {
        tray_update(FALSE);
        if (IsWindowVisible(g_fly)) InvalidateRect(g_fly, NULL, FALSE);
        return 0;
    }
    switch (msg)
    {
    case WM_TOGGLE:
        show_panel(!IsWindowVisible(g_fly));
        return 0;
    case WM_MISSING:
        ask_missing();
        return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP)
        {
            /* a second click on the icon closes it (sg-flyout.h) */
            if (IsWindowVisible(g_fly)) show_panel(FALSE);
            else if (sg_flyout_may_open()) show_panel(TRUE);
        }
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
        {
            HMENU m = CreatePopupMenu();
            POINT p;
            UINT cmd;
            AppendMenuW(m, MF_STRING, 1, L"Open notification centre");
            AppendMenuW(m, MF_STRING | (g_count ? 0 : MF_GRAYED), 2, L"Clear all notifications");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, 3, L"Notification settings");
            GetCursorPos(&p);
            SetForegroundWindow(hwnd);
            cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, hwnd, NULL);
            DestroyMenu(m);
            if (cmd == 1) show_panel(TRUE);
            else if (cmd == 2) { clear_all(); tray_update(FALSE); }
            else if (cmd == 3) ShellExecuteW(NULL, NULL, L"ms-settings:notifications", NULL, NULL, SW_SHOWNORMAL);
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --- runtimes a program needs ------------------------------------------------------ */

static DWORD WINAPI watch_missing(void *arg)
{
    HKEY key;
    HANDLE ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    (void)arg;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, MISSING_KEY, 0, NULL, 0, KEY_READ | KEY_NOTIFY, NULL, &key, NULL)) return 0;
    PostMessageW(g_tray, WM_MISSING, 0, 0);   /* any named before this started */
    for (;;)
    {
        if (RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, ev, TRUE)) break;
        WaitForSingleObject(ev, INFINITE);
        PostMessageW(g_tray, WM_MISSING, 0, 0);
    }
    RegCloseKey(key);
    return 0;
}

static void ask_missing(void)
{
    static WCHAR asked[16][64];
    static int nasked;
    HKEY key;
    WCHAR dll[64], program[MAX_PATH], ord[32], name[128], store_key[160], text[512];
    DWORD i, n, cb, type;
    int k;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, MISSING_KEY, 0, KEY_READ | KEY_SET_VALUE, &key)) return;
    for (i = 0; ; )
    {
        BOOL seen = FALSE;
        n = ARRAYSIZE(dll); cb = sizeof(program);
        if (RegEnumValueW(key, i, dll, &n, NULL, &type, (BYTE *)program, &cb)) break;
        if (type != REG_SZ) program[0] = 0;
        RegDeleteValueW(key, dll);   /* the next enumeration starts again at 0 */
        CharLowerW(dll);
        for (k = 0; k < nasked; k++) if (!lstrcmpW(asked[k], dll)) seen = TRUE;
        cb = sizeof(ord);
        if (seen || RegGetValueW(HKEY_LOCAL_MACHINE, RUNTIMES_KEY, dll, RRF_RT_REG_SZ, NULL, ord, &cb)) continue;
        if (nasked < (int)ARRAYSIZE(asked)) lstrcpynW(asked[nasked++], dll, 64);
        _snwprintf(store_key, ARRAYSIZE(store_key), L"Software\\Stained Glass\\Store\\Apps\\%ls", ord);
        store_key[ARRAYSIZE(store_key) - 1] = 0;
        cb = sizeof(name);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, store_key, L"Name", RRF_RT_REG_SZ, NULL, name, &cb)) lstrcpynW(name, dll, 128);
        _snwprintf(text, ARRAYSIZE(text), L"%ls needs the %ls (%ls), which is not installed.\n\n"
                   L"Install it from SG Store?", program[0] ? program : L"A program", name, dll);
        text[ARRAYSIZE(text) - 1] = 0;
#ifndef SG_MUTANT_NO_RUNTIME_ASK
        if (MessageBoxW(NULL, text, L"A runtime is missing", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) == IDYES)
        {
            WCHAR args[64];
            _snwprintf(args, ARRAYSIZE(args), L"--page %ls", ord);
            args[ARRAYSIZE(args) - 1] = 0;
            ShellExecuteW(NULL, NULL, L"sg-store.exe", args, NULL, SW_SHOWNORMAL);
        }
#endif
    }
    RegCloseKey(key);
}

static HFONT make_font(int height, int weight)
{
    return CreateFontW(-height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void make_fonts(void)
{
    g_font = make_font(U(14), FW_NORMAL);
    g_font_small = make_font(U(12), FW_NORMAL);
    g_font_bold = make_font(U(14), FW_SEMIBOLD);
    g_font_head = make_font(U(20), FW_NORMAL);
}

/* the foreground lock (wine-sg 9930): the running one may come forward on this request, made on the
 * person's key or click -- as Windows programs hand it on (AllowSetForegroundWindow) */
static void pass_foreground(HWND to)
{
    DWORD pid;
#ifdef SG_MUTANT_HANDOFF_NO_GRANT
    return;
#endif
    if (to && GetWindowThreadProcessId(to, &pid)) AllowSetForegroundWindow(pid);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    static WCHAR dump_path[MAX_PATH];
    WNDCLASSW wc = { 0 };
    HWND running;
    MSG msg;

    (void)prev; (void)show;
    if (cmdline && wcsstr(cmdline, L"--dump"))
    {
        int i;
        char a[1500];
        load_history();
        fprintf(stderr, "NOTIFICATIONS %d unread %d\n", g_count, unread());
        for (i = 0; i < g_count; i++)
        {
            WCHAR line[1500];
            _snwprintf(line, ARRAYSIZE(line), L"%lu|%ls|%ls|%ls", (unsigned long)g_entries[i].seq, g_entries[i].app,
                       g_entries[i].title, g_entries[i].body);
            line[ARRAYSIZE(line) - 1] = 0;
            WideCharToMultiByte(CP_UTF8, 0, line, -1, a, sizeof(a), NULL, NULL);
            fprintf(stderr, "ENTRY %s\n", a);
        }
        return 0;
    }
    /* one per session: a second (Win+A) asks the first */
    if ((running = FindWindowW(L"SgNotifyTray", NULL)))
    {
        pass_foreground(running);
        if (cmdline && wcsstr(cmdline, L"/toggle")) PostMessageW(running, WM_TOGGLE, 0, 0);
        return 0;
    }
    if (GetEnvironmentVariableW(L"SG_NOTIFY_DUMP", dump_path, MAX_PATH)) g_dump = dump_path;
    sg_dpi_init();      /* the display scale, a new one too (WM_DPICHANGED) */
    g_s8 = (int)reg_dword(L"Software\\Stained Glass\\Style", L"Scale8", 8);
    if (g_s8 < 8) g_s8 = 8;
    if (g_s8 > 24) g_s8 = 24;
    {
        HDC dc = GetDC(NULL);
        g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(NULL, dc);
        if (g_dpi < 96) g_dpi = 96;
    }
    make_fonts();
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g_changed = RegisterWindowMessageW(L"SgNotificationsChanged");

    wc.lpfnWndProc = tray_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SgNotifyTray";
    RegisterClassW(&wc);
    g_tray = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Notifications", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    wc.lpfnWndProc = fly_proc;
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.lpszClassName = L"SgNotifyPanel";
    RegisterClassW(&wc);
    /* owned by the (never shown) tray window: no taskbar button of its own */
    g_fly = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Notifications", WS_POPUP,
                            0, 0, U(396), 400, g_tray, NULL, inst, NULL);
    tray_update(TRUE);
    CloseHandle(CreateThread(NULL, 0, watch_missing, NULL, 0, NULL));
    if (cmdline && wcsstr(cmdline, L"/toggle")) show_panel(TRUE);
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
