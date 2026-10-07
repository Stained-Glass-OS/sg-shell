/* sg-volume -- the taskbar's volume icon and its flyout.
 *
 * A notification-area icon (Shell_NotifyIcon, as sg-battery's) for the
 * default output: a speaker with one to three waves for the volume, a cross
 * when muted, and a tooltip as Windows words it ("Speakers: 40%"). Clicking
 * it opens a flyout above the taskbar: the output device (click it to choose
 * another), a mute button and a volume slider. The right button offers
 * "Open Sound settings" (Settings > System > Sound).
 *
 * The sound server is PipeWire; sg-settingsctl (sg-session) speaks to it, as
 * for Settings' Sound page: "sound" lists the outputs, "sound volume|mute|
 * default sink NAME [VALUE]" changes them. Those calls run on a worker
 * thread, so a slow sound server never stops the taskbar icon.
 *
 *   sg-volume              the icon (sg-session starts it with the shell)
 *   sg-volume --dump       what it would show (stderr): the gate
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "sg-mode.h"
#include "sg-smooth.h"
#include "sg-round.h"
#include "sg-dpi.h"
#include "sg-flyout.h"

#define FLY_W 360
#define FLY_H 112
#define WM_TRAY  (WM_APP + 1)
#define WM_STATE (WM_APP + 2)   /* the worker read the sound server */
#define MAX_SINKS 16

struct sink { WCHAR name[200], desc[200]; int vol; BOOL def, muted; };

/* what the flyout shows; written only on the window thread */
static struct sink g_sinks[MAX_SINKS];
static int g_nsinks, g_cur = -1;
static BOOL g_known, g_failed;

static HWND g_tray, g_fly;
static DWORD g_fly_hidden;   /* GetTickCount when the flyout last hid on deactivation: a tray click within ~200ms of it is that same click, so it must not reopen (else it flickers instead of closing) */
static NOTIFYICONDATAW g_nid;
static HFONT g_font, g_font_small, g_font_icon;
/* the flyout at the display scale, a new one too (per-monitor v2, sg-dpi.h):
 * its sizes are 100%'s, S() at the DPI it is drawn at */
static int g_dpi = 96;
#define S(x) MulDiv((x), g_dpi, 96)
static UINT g_taskbar_created;
static BOOL g_dragging, g_mute_hot, g_dev_hot;
static int g_drag_vol = -1;

/* --- sg-settingsctl ---------------------------------------------------------------- */

/* Runs sg-settingsctl ARGS and returns its answer (malloc'd) or NULL. The
 * answer lands in a file (--out) that appears whole, as for sg-control. */
static char *ctl_run(const WCHAR *args, DWORD timeout_ms)
{
    static LONG seq;
    static char *(CDECL *to_unix)(const WCHAR *);
    WCHAR tool[MAX_PATH] = L"/usr/bin/sg-settingsctl", dir[MAX_PATH], dos[MAX_PATH], cmd[2048], *p;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    char *unix_out, *text = NULL;
    DWORD waited = 0;
    HANDLE h;

    if (!to_unix) to_unix = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    GetEnvironmentVariableW(L"SG_SETTINGSCTL", tool, MAX_PATH);
    if (!to_unix || !GetTempPathW(MAX_PATH, dir)) return NULL;
    _snwprintf(dos, MAX_PATH, L"%lssg-volume-%lu-%ld.txt", dir, GetCurrentProcessId(), InterlockedIncrement(&seq));
    dos[MAX_PATH - 1] = 0;
    DeleteFileW(dos);
    CloseHandle(CreateFileW(dos, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));     /* so it has a Unix name */
    if (!(unix_out = to_unix(dos))) return NULL;
    DeleteFileW(dos);
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\\\\?\\unix%ls %ls --out %S", tool, args, unix_out);
    cmd[ARRAYSIZE(cmd) - 1] = 0;
    HeapFree(GetProcessHeap(), 0, unix_out);
    for (p = cmd + 8; *p && *p != L' '; p++) if (*p == L'/') *p = L'\\';
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return NULL;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    while (GetFileAttributesW(dos) == INVALID_FILE_ATTRIBUTES && waited < timeout_ms) { Sleep(40); waited += 40; }
    h = CreateFileW(dos, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD size = GetFileSize(h, NULL), got = 0;
        if (size < (1u << 20) && (text = malloc(size + 1))) { ReadFile(h, text, size, &got, NULL); text[got] = 0; }
        CloseHandle(h);
        DeleteFileW(dos);
    }
    return text;
}

/* field I (tab-separated) of a line, UTF-8 to UTF-16 */
static void field(const char *line, int i, WCHAR *out, int cch)
{
    const char *s = line, *e;
    int n;
    while (i-- > 0 && (s = strchr(s, '\t'))) s++;
    if (!s) { out[0] = 0; return; }
    for (e = s; *e && *e != '\t' && *e != '\n' && *e != '\r'; e++) ;
    n = MultiByteToWideChar(CP_UTF8, 0, s, (int)(e - s), out, cch - 1);
    out[n > 0 ? n : 0] = 0;
}

/* "SINK name\tdefault\tvol\tmuted\tdesc" lines into LIST; FALSE if the
 * sound server did not answer */
static BOOL parse_sinks(const char *ans, struct sink *list, int *count)
{
    const char *line = ans;
    *count = 0;
    if (!ans || (!strstr(ans, "\nOK") && strncmp(ans, "OK", 2))) return FALSE;
    while (line && *line)
    {
        if (!strncmp(line, "SINK ", 5) && *count < MAX_SINKS)
        {
            struct sink *s = &list[(*count)++];
            WCHAR f[16];
            field(line + 5, 0, s->name, ARRAYSIZE(s->name));
            field(line + 5, 1, f, ARRAYSIZE(f)); s->def = !lstrcmpW(f, L"yes");
            field(line + 5, 2, f, ARRAYSIZE(f)); s->vol = _wtoi(f);
            field(line + 5, 3, f, ARRAYSIZE(f)); s->muted = !lstrcmpW(f, L"yes");
            field(line + 5, 4, s->desc, ARRAYSIZE(s->desc));
            if (s->vol < 0) s->vol = 0;
        }
        if ((line = strchr(line, '\n'))) line++;
    }
    return TRUE;
}

/* --- the worker: reads, and carries out changes, off the window thread ------------- */

struct change { WCHAR args[512]; };
static CRITICAL_SECTION g_cs;
static struct change g_pending[8];
static int g_npending;
static BOOL g_chime;            /* the chime after the changes (sound chime) */
static HANDLE g_wake;

static void queue_change(const WCHAR *verb, const WCHAR *name, const WCHAR *value)
{
    int i;
    EnterCriticalSection(&g_cs);
    /* a newer volume for the same device replaces the older one */
    for (i = 0; i < g_npending; i++)
        if (!wcsncmp(g_pending[i].args, L"sound volume ", 13) && !wcsncmp(verb, L"volume", 6)) break;
    if (i == g_npending && g_npending < (int)ARRAYSIZE(g_pending)) g_npending++;
    if (i < g_npending)
        _snwprintf(g_pending[i].args, ARRAYSIZE(g_pending[i].args), L"sound %ls sink \"%ls\"%ls%ls",
                   verb, name, value ? L" " : L"", value ? value : L"");
    LeaveCriticalSection(&g_cs);
    SetEvent(g_wake);
}

struct state { struct sink sinks[MAX_SINKS]; int count; BOOL ok; };

static DWORD WINAPI worker(void *arg)
{
    (void)arg;
    for (;;)
    {
        struct change todo[8];
        struct state *st;
        char *ans;
        int n, i;
        BOOL chime;

        WaitForSingleObject(g_wake, 5000);   /* a change, a refresh, or every 5 s */
        EnterCriticalSection(&g_cs);
        n = g_npending;
        memcpy(todo, g_pending, n * sizeof(*todo));
        g_npending = 0;
        chime = g_chime;
        g_chime = FALSE;
        LeaveCriticalSection(&g_cs);
        for (i = 0; i < n; i++) free(ctl_run(todo[i].args, 8000));
        /* at the new volume, so the person hears where it is (David
         * 2026-10-02); one for a burst of wheel steps */
        if (chime) free(ctl_run(L"sound chime", 3000));
        if (!(st = calloc(1, sizeof(*st)))) continue;
        ans = ctl_run(L"sound", 8000);
        st->ok = parse_sinks(ans, st->sinks, &st->count);
        free(ans);
        if (!PostMessageW(g_tray, WM_STATE, 0, (LPARAM)st)) free(st);
    }
    return 0;
}

/* --- the state --------------------------------------------------------------------- */

static int current(void)
{
    int i;
    for (i = 0; i < g_nsinks; i++) if (g_sinks[i].def) return i;
    return g_nsinks ? 0 : -1;
}

static int shown_volume(void)
{
    if (g_drag_vol >= 0) return g_drag_vol;
    return g_cur >= 0 ? min(g_sinks[g_cur].vol, 100) : 0;
}
static BOOL muted(void) { return g_cur >= 0 && g_sinks[g_cur].muted; }

static void describe(WCHAR *out, int cch)
{
    if (!g_known) lstrcpynW(out, L"Volume", cch);
    else if (g_cur < 0) lstrcpynW(out, g_failed ? L"The sound service is not responding" : L"No audio output device is installed", cch);
    else if (muted()) _snwprintf(out, cch, L"%ls: muted", g_sinks[g_cur].desc);
    else _snwprintf(out, cch, L"%ls: %d%%", g_sinks[g_cur].desc, shown_volume());
    out[cch - 1] = 0;
}

/* --- the icon, drawn here at four times the size and scaled down ------------------ */

/* sg-smooth: drawn on a canvas four times larger, reduced to soft-edged alpha */
static HICON make_icon(int size, COLORREF fg)
{
    enum { K = 4 };
    int S = size * K, x, y, waves, vol = shown_volume();
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), S, -S, 1, 32, BI_RGB } };
    BITMAPINFO bo = { { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB } };
    DWORD *big, *px;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP canvas = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&big, NULL, 0), color, mask, old;
    ICONINFO ii = { TRUE };
    HICON icon;
    int pw = max(S / 14, 1);
    HPEN pen = CreatePen(PS_SOLID, pw, RGB(0xFF, 0xFF, 0xFF)), op;
    HBRUSH white = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF)), ob;
    /* the speaker: a box and a cone */
    POINT cone[6] = { { S * 3 / 32, S * 12 / 32 }, { S * 9 / 32, S * 12 / 32 }, { S * 16 / 32, S * 5 / 32 },
                      { S * 16 / 32, S * 27 / 32 }, { S * 9 / 32, S * 20 / 32 }, { S * 3 / 32, S * 20 / 32 } };
    int i;

    old = SelectObject(dc, canvas);
    memset(big, 0, S * S * 4);
    op = SelectObject(dc, pen);
    ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    SelectObject(dc, white);
    Polygon(dc, cone, 6);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (!g_known || g_cur < 0 || muted())
    {
        /* a cross where the waves would be */
        MoveToEx(dc, S * 20 / 32, S * 11 / 32, NULL); LineTo(dc, S * 30 / 32, S * 21 / 32);
        MoveToEx(dc, S * 30 / 32, S * 11 / 32, NULL); LineTo(dc, S * 20 / 32, S * 21 / 32);
    }
    else
    {
        waves = vol == 0 ? 0 : vol < 34 ? 1 : vol < 67 ? 2 : 3;
        for (i = 0; i < waves; i++)
        {
            int r = S * (5 + 5 * i) / 32, cx = S * 15 / 32, cy = S / 2;
            Arc(dc, cx - r, cy - r, cx + r, cy + r, cx + r, cy + r, cx + r, cy - r);
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
            /* premultiplied: the colour scaled by its coverage */
            px[y * size + x] = a ? ((DWORD)a << 24) | (GetRValue(fg) * a / 255) << 16 | (GetGValue(fg) * a / 255) << 8 |
                                   (GetBValue(fg) * a / 255) : 0;
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
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_tray;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    /* white on the dark taskbar, black on the light one */
    g_nid.hIcon = make_icon(GetSystemMetrics(SM_CXSMICON), sg_system_dark() ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0));
    describe(g_nid.szTip, ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_nid);
    if (old) DestroyIcon(old);
}

/* --- the flyout: device, mute button, slider ---------------------------------------- */

static RECT dev_rect(void)    { RECT r = { S(16), S(12), S(FLY_W - 16), S(40) }; return r; }
static RECT mute_rect(void)   { RECT r = { S(12), S(56), S(52), S(96) }; return r; }
static RECT track_rect(void)  { RECT r = { S(64), S(72), S(FLY_W - 64), S(80) }; return r; }
static RECT number_rect(void) { RECT r = { S(FLY_W - 56), S(56), S(FLY_W - 12), S(96) }; return r; }

static int volume_at(int x)
{
    RECT t = track_rect();
    int v = (x - t.left) * 100 / (t.right - t.left);
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

static void fly_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps), mem = CreateCompatibleDC(dc);
    BOOL dark = sg_system_dark();
    COLORREF bg = dark ? RGB(0x1F, 0x1F, 0x1F) : RGB(0xF2, 0xF2, 0xF2), text = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0),
             subtle = dark ? RGB(0xA8, 0xA8, 0xA8) : RGB(0x5A, 0x5A, 0x5A),
             hover = dark ? RGB(0x33, 0x33, 0x33) : RGB(0xE0, 0xE0, 0xE0),
             rail = dark ? RGB(0x5A, 0x5A, 0x5A) : RGB(0xB0, 0xB0, 0xB0),
             accent = dark ? sg_accent_light(40) : sg_accent();
    HBITMAP bmp;
    HBRUSH b;
    RECT c, r, t;
    WCHAR buf[256];
    HICON icon;
    int vol = shown_volume(), x;

    GetClientRect(hwnd, &c);
    bmp = CreateCompatibleBitmap(dc, c.right, c.bottom);
    SelectObject(mem, bmp);
    b = CreateSolidBrush(bg); FillRect(mem, &c, b); DeleteObject(b);
    SetBkMode(mem, TRANSPARENT);

    /* the device */
    r = dev_rect();
    if (g_dev_hot && g_nsinks > 1) { b = CreateSolidBrush(hover); FillRect(mem, &r, b); DeleteObject(b); }
    SelectObject(mem, g_font);
    SetTextColor(mem, text);
    if (g_cur >= 0) _snwprintf(buf, ARRAYSIZE(buf), L"%ls%ls", g_sinks[g_cur].desc, g_nsinks > 1 ? L"  \x2228" : L"");
    else describe(buf, ARRAYSIZE(buf));
    buf[ARRAYSIZE(buf) - 1] = 0;
    r.left += S(8);
    DrawTextW(mem, buf, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    /* the mute button: the speaker as it is */
    r = mute_rect();
    if (g_mute_hot) { b = CreateSolidBrush(hover); FillRect(mem, &r, b); DeleteObject(b); }
    icon = make_icon(S(24), text);
    DrawIconEx(mem, r.left + S(8), r.top + S(8), icon, S(24), S(24), 0, NULL, DI_NORMAL);
    DestroyIcon(icon);

    /* the slider */
    t = track_rect();
    x = t.left + (t.right - t.left) * vol / 100;
    SetRect(&r, t.left, t.top + S(2), t.right, t.bottom - S(2));
    b = CreateSolidBrush(rail); FillRect(mem, &r, b); DeleteObject(b);
    SetRect(&r, t.left, t.top + S(2), x, t.bottom - S(2));
    b = CreateSolidBrush(muted() ? rail : accent); FillRect(mem, &r, b); DeleteObject(b);
    {
        HBRUSH ob;
        HPEN op, edge = CreatePen(PS_SOLID, S(2), bg);
        b = CreateSolidBrush(muted() ? subtle : accent);
        ob = SelectObject(mem, b); op = SelectObject(mem, edge);
        sg_ellipse(mem, x - S(9), (t.top + t.bottom) / 2 - S(9), x + S(10), (t.top + t.bottom) / 2 + S(10));
        SelectObject(mem, ob); SelectObject(mem, op);
        DeleteObject(b); DeleteObject(edge);
    }
    /* the number */
    r = number_rect();
    SelectObject(mem, g_font_icon);
    SetTextColor(mem, g_cur >= 0 ? text : subtle);
    _snwprintf(buf, ARRAYSIZE(buf), L"%d", vol);
    DrawTextW(mem, buf, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);

    BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

static void place_flyout(void)
{
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    SetWindowPos(g_fly, HWND_TOPMOST, work.right - S(FLY_W + 12), work.bottom - S(FLY_H + 12), S(FLY_W), S(FLY_H), SWP_NOACTIVATE);
}

static void set_volume(int v, BOOL send)
{
    WCHAR value[8];
    if (g_cur < 0) return;
    g_sinks[g_cur].vol = v;
    if (send)
    {
#ifndef SG_MUTANT_NO_CHIME
        /* the slider let go, a key, the wheel -- not while it is dragged */
        if (!g_dragging)
        {
            EnterCriticalSection(&g_cs);
            g_chime = TRUE;
            LeaveCriticalSection(&g_cs);
        }
#endif
        _snwprintf(value, ARRAYSIZE(value), L"%d", v);
        queue_change(L"volume", g_sinks[g_cur].name, value);
        /* moving the slider unmutes, as on Windows */
        if (g_sinks[g_cur].muted) { g_sinks[g_cur].muted = FALSE; queue_change(L"mute", g_sinks[g_cur].name, L"no"); }
    }
    tray_update(FALSE);
    InvalidateRect(g_fly, NULL, FALSE);
}

static void toggle_mute(void)
{
    if (g_cur < 0) return;
    g_sinks[g_cur].muted = !g_sinks[g_cur].muted;
    queue_change(L"mute", g_sinks[g_cur].name, g_sinks[g_cur].muted ? L"yes" : L"no");
    tray_update(FALSE);
    InvalidateRect(g_fly, NULL, FALSE);
}

static void choose_device(HWND hwnd)
{
    HMENU m = CreatePopupMenu();
    RECT r = dev_rect();
    POINT p = { r.left, r.bottom };
    int i, cmd;
    for (i = 0; i < g_nsinks; i++)
        AppendMenuW(m, MF_STRING | (i == g_cur ? MF_CHECKED : 0), i + 1, g_sinks[i].desc);
    ClientToScreen(hwnd, &p);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (cmd > 0 && cmd - 1 != g_cur)
    {
        for (i = 0; i < g_nsinks; i++) g_sinks[i].def = (i == cmd - 1);
        g_cur = cmd - 1;
        queue_change(L"default", g_sinks[g_cur].name, NULL);
        tray_update(FALSE);
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

static void make_fonts(void);

static LRESULT CALLBACK fly_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
    RECT r;

    switch (msg)
    {
    case WM_PAINT: fly_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
#ifndef SG_MUTANT_VOLUME_DPI_IGNORED
    case WM_DPICHANGED:
        /* a new display scale: its text, slider and size at it; the tray
         * icon at the new small-icon size */
        g_dpi = (int)sg_dpi_new(wp);
        make_fonts();
        sg_dpi_apply_rect(hwnd, lp);
        place_flyout();
        tray_update(FALSE);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
#endif
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !g_dragging && !IsWindowVisible((HWND)lp)) { ShowWindow(hwnd, SW_HIDE); g_fly_hidden = GetTickCount(); sg_flyout_hidden(g_tray, 1); }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) ShowWindow(hwnd, SW_HIDE);
        else if (wp == VK_LEFT || wp == VK_DOWN) set_volume(max(shown_volume() - 2, 0), TRUE);
        else if (wp == VK_RIGHT || wp == VK_UP) set_volume(min(shown_volume() + 2, 100), TRUE);
        return 0;
    case WM_MOUSEWHEEL:
        set_volume(max(0, min(100, shown_volume() + ((short)HIWORD(wp) > 0 ? 2 : -2))), TRUE);
        return 0;
    case WM_MOUSEMOVE:
    {
        BOOL mh, dh;
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        if (g_dragging) { g_drag_vol = volume_at(pt.x); set_volume(g_drag_vol, FALSE); return 0; }
        r = mute_rect(); mh = PtInRect(&r, pt);
        r = dev_rect(); dh = PtInRect(&r, pt);
        if (mh != g_mute_hot || dh != g_dev_hot) { g_mute_hot = mh; g_dev_hot = dh; InvalidateRect(hwnd, NULL, FALSE); }
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_mute_hot || g_dev_hot) { g_mute_hot = g_dev_hot = FALSE; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN:
        r = track_rect(); InflateRect(&r, S(12), S(14));
        if (PtInRect(&r, pt) && g_cur >= 0)
        {
            g_dragging = TRUE;
            SetCapture(hwnd);
            g_drag_vol = volume_at(pt.x);
            set_volume(g_drag_vol, FALSE);
            SetTimer(hwnd, 1, 150, NULL);   /* the sound follows the thumb */
        }
        return 0;
    case WM_TIMER:
        if (g_dragging && g_drag_vol >= 0 && g_cur >= 0) set_volume(g_drag_vol, TRUE);
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging)
        {
            g_dragging = FALSE;
            KillTimer(hwnd, 1);
            ReleaseCapture();
            set_volume(volume_at(pt.x), TRUE);
            g_drag_vol = -1;
            return 0;
        }
        r = mute_rect();
        if (PtInRect(&r, pt)) { toggle_mute(); return 0; }
        r = dev_rect();
        if (PtInRect(&r, pt) && g_nsinks > 1) choose_device(hwnd);
        return 0;
    case WM_CAPTURECHANGED:
        if (g_dragging && (HWND)lp != hwnd) { g_dragging = FALSE; KillTimer(hwnd, 1); g_drag_vol = -1; }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbar_created && g_taskbar_created) { tray_update(TRUE); return 0; }
    if (sg_mode_changed(msg, lp))
    {
        tray_update(FALSE);
        if (IsWindowVisible(g_fly)) InvalidateRect(g_fly, NULL, FALSE);
        return 0;
    }
    switch (msg)
    {
    case WM_TRAY:
        if (lp == WM_LBUTTONUP)
        {
            if (IsWindowVisible(g_fly)) ShowWindow(g_fly, SW_HIDE);
            else if (!sg_flyout_may_open() || GetTickCount() - g_fly_hidden < 200) { /* this same click just dismissed the flyout: leave it closed (sg-flyout.h) */ }
            else
            {
                place_flyout();
                ShowWindow(g_fly, SW_SHOW);
                SetForegroundWindow(g_fly);
                SetFocus(g_fly);
                InvalidateRect(g_fly, NULL, FALSE);
                SetEvent(g_wake);   /* and look again */
            }
        }
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
        {
            HMENU m = CreatePopupMenu();
            POINT p;
            int cmd;
            AppendMenuW(m, MF_STRING, 1, L"Open Sound settings");
            AppendMenuW(m, MF_STRING | (g_cur < 0 ? MF_GRAYED : 0), 2, muted() ? L"Unmute" : L"Mute");
            GetCursorPos(&p);
            SetForegroundWindow(hwnd);
            cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, hwnd, NULL);
            DestroyMenu(m);
            if (cmd == 1) ShellExecuteW(NULL, NULL, L"ms-settings:sound", NULL, NULL, SW_SHOWNORMAL);
            if (cmd == 2) toggle_mute();
        }
        return 0;
    case WM_STATE:
    {
        struct state *st = (struct state *)lp;
        /* while the thumb moves, the slider is the truth */
        if (!g_dragging)
        {
            g_failed = !st->ok;
            if (st->ok) { memcpy(g_sinks, st->sinks, sizeof(g_sinks)); g_nsinks = st->count; }
            else g_nsinks = 0;
            g_cur = current();
            g_known = TRUE;
            tray_update(FALSE);
            if (IsWindowVisible(g_fly)) InvalidateRect(g_fly, NULL, FALSE);
        }
        free(st);
        return 0;
    }
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HFONT make_font(int height, int weight)
{
    return CreateFontW(-S(height), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void make_fonts(void)
{
    if (g_font) DeleteObject(g_font);
    if (g_font_small) DeleteObject(g_font_small);
    if (g_font_icon) DeleteObject(g_font_icon);
    g_font = make_font(14, FW_NORMAL);
    g_font_small = make_font(12, FW_NORMAL);
    g_font_icon = make_font(20, FW_NORMAL);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSW wc = { 0 };
    MSG msg;

    (void)prev; (void)show;
    if (cmdline && wcsstr(cmdline, L"--dump"))
    {
        char *ans = ctl_run(L"sound", 8000), a[256];
        WCHAR tip[128];
        g_known = TRUE;
        g_failed = !parse_sinks(ans, g_sinks, &g_nsinks);
        free(ans);
        g_cur = current();
        describe(tip, ARRAYSIZE(tip));
        WideCharToMultiByte(CP_UTF8, 0, tip, -1, a, sizeof(a), NULL, NULL);
        if (g_cur < 0) fprintf(stderr, "VOLUME none %s\n", g_failed ? "failed" : "no-device");
        else fprintf(stderr, "VOLUME %d %s %d-outputs\n", shown_volume(), muted() ? "muted" : "on", g_nsinks);
        fprintf(stderr, "TIP %s\n", a);
        return 0;
    }
    if (FindWindowW(L"SgVolumeTray", NULL)) return 0;   /* one per session */

    InitializeCriticalSection(&g_cs);
    g_wake = CreateEventW(NULL, FALSE, TRUE, NULL);   /* read at once */
    sg_dpi_init();      /* the display scale, a new one too (WM_DPICHANGED) */
    g_dpi = (int)sg_dpi_for(NULL);
    make_fonts();
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    wc.lpfnWndProc = tray_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SgVolumeTray";
    RegisterClassW(&wc);
    g_tray = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Volume", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    wc.lpfnWndProc = fly_proc;
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.lpszClassName = L"SgVolumeFlyout";
    RegisterClassW(&wc);
    /* owned by the (never shown) tray window: no taskbar button of its own */
    g_fly = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Volume", WS_POPUP,
                            0, 0, S(FLY_W), S(FLY_H), g_tray, NULL, inst, NULL);
    sg_round_corners(g_fly);
    tray_update(TRUE);
    CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
