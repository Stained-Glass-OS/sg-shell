/* sg-touchkbd -- the touch keyboard (TabTip.exe).
 *
 * The keyboard a tablet shows when a text field is touched: across the
 * bottom of the work area (above the taskbar, over the programs), large keys, our own drawing in the shell's light or
 * dark mode and the accent colour. It is not the On-Screen Keyboard (sg-osk,
 * osk.exe, the full keyboard of Ease of Access): letters, a page of numbers
 * and symbols and one of more symbols, Shift (once; twice for Caps Lock),
 * Ctrl (sticky, for Ctrl+C, Ctrl+V...), Backspace (repeats while held),
 * Enter, Tab, the space bar, the arrow keys, and a key that hides it.
 *
 * WHEN IT SHOWS ITSELF. A text field focused by a touch, with no hardware
 * keyboard attached (a Surface without its Type Cover), shows it, as a tablet
 * does; a touch that moves the focus off text fields hides it again (if it
 * showed itself). The touch: wine-sg 1150 stamps the desktop window's
 * __wine_sg_touch_time with the time of every touch going down, and
 * __wine_sg_touch_hwnd with the window it went down on; checked four times a
 * second, the focus looked at in that window's thread. A text field: the focus is an edit control (its class
 * names Edit or RichEdit, a .NET TextBox's too) that is not read-only, or
 * its thread shows a caret. A hardware keyboard: an input device of the
 * kernel's (/proc/bus/input/devices) that repeats keys and has letters and
 * a space bar -- a Surface's power and volume buttons are not one.
 * HKCU\Software\Microsoft\TabletTip\1.7 EnableDesktopModeAutoInvoke = 1
 * shows it with a keyboard attached too (Windows' setting of that name).
 *
 * THE TASKBAR BUTTON. In the notification area, on a computer with a touch
 * screen or pen (GetSystemMetrics SM_DIGITIZER, wine-sg 1150), or when
 * TipbandDesiredVisibility = 1 in that key; a click shows or hides the
 * keyboard. Started at sign-in (/background, the machine's Run key in
 * defaults/90-sg-touchkbd.reg): without a touch screen and the setting, it
 * exits at once.
 *
 * IT NEVER TAKES THE FOCUS: WS_EX_NOACTIVATE, MA_NOACTIVATE, and touches
 * taken as pointer messages (no mouse made of them, so nothing activates
 * it). Keys are real key presses (SendInput): a character the layout of the
 * program in front has a key for is that key (with Shift when it needs it),
 * any other is sent as the character itself (KEYEVENTF_UNICODE).
 *
 * Command line: /show, /hide, /toggle (a running keyboard does it), no
 * argument: /show (programs start TabTip.exe to show the keyboard),
 * /background: the taskbar button and the automatic keyboard only. One
 * instance (Local\StainedGlassTouchKeyboard; the window class is Windows'
 * touch keyboard's, IPTip_Main_Window, which programs look for).
 *
 * For the gate (test/touchkbd-check.sh): SG_TOUCHKBD_DUMP=<file> (a Windows
 * path) gets the window's state and every key's screen centre after each
 * change; SG_TOUCHKBD_DEVICES=<file> reads that as the input devices;
 * SG_TOUCHKBD_TOUCH=1 has a touch screen.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../sg-mode.h"
#include "../sg-smooth.h"
#include "../sg-dpi.h"

#define CLASS_NAME L"IPTip_Main_Window"
#define TIP_KEY L"Software\\Microsoft\\TabletTip\\1.7"
#define WM_TRAY (WM_APP + 1)
#define WM_COMMAND_LINE (WM_APP + 2)
enum { CMD_SHOW = 1, CMD_HIDE, CMD_TOGGLE };
enum { T_POLL = 1, T_REPEAT, T_FOCUS };

enum { K_CHAR, K_BACK, K_ENTER, K_SHIFT, K_TAB, K_SPACE, K_LEFT, K_RIGHT, K_PAGE, K_MORE, K_CTRL, K_HIDE };

struct key {
    int kind;
    WCHAR ch;           /* K_CHAR: what it types */
    float w;            /* in key widths */
    const WCHAR *label; /* the special keys' */
    int page;           /* K_PAGE, K_MORE: the page it goes to */
    RECT rc;            /* laid out, client coordinates */
};

#define ROWS 4
#define MAXKEYS 16
static struct key pages[3][ROWS][MAXKEYS];
static int page_len[3][ROWS];

static HINSTANCE g_inst;
static HWND g_wnd;
static int g_page;
static int g_shift;          /* 0 off, 1 the next letter, 2 Caps Lock */
static DWORD g_shift_time;
static BOOL g_ctrl;
static BOOL g_shown, g_auto;  /* shown; shown by itself */
static BOOL g_tray;
static UINT g_taskbar_created;
static NOTIFYICONDATAW g_nid;
static WCHAR g_dump[MAX_PATH], g_devices[MAX_PATH];
static HFONT g_font, g_font_small;
static DWORD g_last_touch, g_hide_time, g_touch_seen;
static BOOL g_force_touch;
/* /notray (with /background): no taskbar button -- the sign-in screen and the
 * first-run setup have no taskbar, and Wine stood the icon in a little tray
 * window of its own, an empty box on the Surface's sign-in screen (David,
 * 2026-10-07). The automatic keyboard still shows for a text field. */
static BOOL g_notray;

/* the pointers holding keys down: pointer id (0: the mouse) -> key */
static struct { UINT32 id; struct key *key; BOOL used; } g_down[10];
static struct key *g_repeat_key;

static void add_key(int page, int row, int kind, WCHAR ch, float w, const WCHAR *label, int target)
{
    struct key *k = &pages[page][row][page_len[page][row]++];
    k->kind = kind;
    k->ch = ch;
    k->w = w;
    k->label = label;
    k->page = target;
}

static void add_chars(int page, int row, const WCHAR *chars)
{
    for (; *chars; chars++) add_key(page, row, K_CHAR, *chars, 1, NULL, 0);
}

static void bottom_row(int page)
{
    add_key(page, 3, K_PAGE, 0, 1.5f, page ? L"abc" : L"&123", page ? 0 : 1);
    add_key(page, 3, K_CTRL, 0, 1.25f, L"Ctrl", 0);
    add_key(page, 3, K_SPACE, ' ', 5.5f, NULL, 0);
    add_key(page, 3, K_LEFT, 0, 1, NULL, 0);
    add_key(page, 3, K_RIGHT, 0, 1, NULL, 0);
    add_key(page, 3, K_HIDE, 0, 1.25f, NULL, 0);
}

static void build_pages(void)
{
    /* letters */
    add_chars(0, 0, L"qwertyuiop");
    add_key(0, 0, K_BACK, 0, 1.5f, NULL, 0);
    add_key(0, 1, K_TAB, 0, 1, L"Tab", 0);
    add_chars(0, 1, L"asdfghjkl");
    add_key(0, 1, K_ENTER, 0, 1.5f, NULL, 0);
    add_key(0, 2, K_SHIFT, 0, 1.25f, NULL, 0);
    add_chars(0, 2, L"zxcvbnm,.");
    add_key(0, 2, K_SHIFT, 0, 1.25f, NULL, 0);
    bottom_row(0);
    /* numbers and symbols */
    add_chars(1, 0, L"1234567890");
    add_key(1, 0, K_BACK, 0, 1.5f, NULL, 0);
    add_chars(1, 1, L"@#$%&*-+()");
    add_key(1, 1, K_ENTER, 0, 1.5f, NULL, 0);
    add_key(1, 2, K_MORE, 0, 1.25f, L"1/2", 2);
    add_chars(1, 2, L"!\":;/?'_=");
    add_key(1, 2, K_TAB, 0, 1.25f, L"Tab", 0);
    bottom_row(1);
    /* more symbols */
    add_chars(2, 0, L"[]{}|\\^~`<");
    add_key(2, 0, K_BACK, 0, 1.5f, NULL, 0);
    add_chars(2, 1, L"\x20ac\x00a3\x00a5\x00b0\x00a7\x2022\x2026\x2014\x00ab\x00bb");
    add_key(2, 1, K_ENTER, 0, 1.5f, NULL, 0);
    add_key(2, 2, K_MORE, 0, 1.25f, L"2/2", 1);
    add_chars(2, 2, L">,.!?\x00bf\x00a1\x00b1\x00d7");
    add_key(2, 2, K_TAB, 0, 1.25f, L"Tab", 0);
    bottom_row(2);
    pages[2][3][0].label = L"abc";
    pages[1][3][0].page = 0;
    pages[2][3][0].page = 0;
}

static DWORD reg_get(const WCHAR *name, DWORD def)
{
    DWORD v = def, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, TIP_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &size)) v = def;
    return v;
}

/* the display scale it is drawn at (per-monitor v2, sg-dpi.h: a new one too) */
static float dpi_scale(void)
{
    return sg_dpi_for(g_wnd) / 96.0f;
}

/* ---- what is attached --------------------------------------------------- */

static BOOL has_touch_screen(void)
{
    if (g_force_touch) return TRUE;
    return (GetSystemMetrics(SM_DIGITIZER) & (NID_INTEGRATED_TOUCH | NID_EXTERNAL_TOUCH | NID_INTEGRATED_PEN |
                                             NID_EXTERNAL_PEN)) != 0;
}

/* bit N of a /proc/bus/input/devices bitmap ("B: KEY=..."): words of 64 bits,
 * the highest first */
static BOOL bitmap_has(const char *bits, int n)
{
    const char *words[64];
    int count = 0, word = n / 64;
    const char *p = bits;
    unsigned long long v;

    while (*p && count < 64)
    {
        while (*p == ' ') p++;
        if (!*p || *p == '\n') break;
        words[count++] = p;
        while (*p && *p != ' ' && *p != '\n') p++;
    }
    if (word >= count) return FALSE;
    v = strtoull(words[count - 1 - word], NULL, 16);
    return (v >> (n % 64)) & 1;
}

/* a keyboard with letters is attached */
static BOOL has_keyboard(void)
{
    HANDLE file = CreateFileW(g_devices, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    static char buf[256 * 1024];
    DWORD got, total = 0;
    BOOL found = FALSE, rep = FALSE, letters = FALSE;
    char *line, *next;

#ifdef SG_MUTANT_TOUCHKBD_NO_KEYBOARD_CHECK
    return FALSE;
#endif
    if (file == INVALID_HANDLE_VALUE) return TRUE;  /* cannot tell: as a laptop */
    while (total < sizeof(buf) - 1 && ReadFile(file, buf + total, sizeof(buf) - 1 - total, &got, NULL) && got)
        total += got;
    CloseHandle(file);
    buf[total] = 0;
    for (line = buf; line && *line && !found; line = next)
    {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!*line)
        {
            /* the end of a device */
            found = rep && letters;
            rep = letters = FALSE;
        }
        else if (!strncmp(line, "B: EV=", 6))
        {
            unsigned long ev = strtoul(line + 6, NULL, 16);
            rep = (ev & (1ul << 1)) && (ev & (1ul << 20));   /* EV_KEY, EV_REP */
        }
        else if (!strncmp(line, "B: KEY=", 7))
            letters = bitmap_has(line + 7, 30) && bitmap_has(line + 7, 44) && bitmap_has(line + 7, 57);  /* A, Z, space */
    }
    return found || (rep && letters);
}

/* ---- the layout ----------------------------------------------------------- */

static int g_w, g_h, g_gap;

static void layout(void)
{
    RECT rc;
    int r, i, top, row_h;
    float units = 0, unit;

    GetClientRect(g_wnd, &rc);
    g_w = rc.right;
    g_h = rc.bottom;
    g_gap = max(3, (int)(4 * dpi_scale()));
    for (r = 0; r < ROWS; r++)
    {
        float u = 0;
        for (i = 0; i < page_len[0][r]; i++) u += pages[0][r][i].w;
        if (u > units) units = u;
    }
    unit = (g_w - g_gap * 4) / units;
    if (unit > g_h / 3.2f) unit = g_h / 3.2f;   /* a very wide screen: not stretched */
    row_h = (g_h - g_gap * (ROWS + 1)) / ROWS;
    for (int p = 0; p < 3; p++)
        for (r = 0; r < ROWS; r++)
        {
            float u = 0, x;
            for (i = 0; i < page_len[p][r]; i++) u += pages[p][r][i].w;
            x = (g_w - u * unit) / 2;
            top = g_gap + r * (row_h + g_gap);
            for (i = 0; i < page_len[p][r]; i++)
            {
                struct key *k = &pages[p][r][i];
                SetRect(&k->rc, (int)x + g_gap / 2, top, (int)(x + k->w * unit) - g_gap / 2, top + row_h);
                x += k->w * unit;
            }
        }
    if (g_font) DeleteObject(g_font);
    if (g_font_small) DeleteObject(g_font_small);
    {
        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfMessageFont.lfHeight = -(row_h * 42 / 100);
        ncm.lfMessageFont.lfWeight = FW_NORMAL;
        ncm.lfMessageFont.lfQuality = CLEARTYPE_QUALITY;
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        ncm.lfMessageFont.lfHeight = -(row_h * 28 / 100);
        g_font_small = CreateFontIndirectW(&ncm.lfMessageFont);
    }
}

static struct key *key_at(POINT pt)
{
    int r, i;
    for (r = 0; r < ROWS; r++)
        for (i = 0; i < page_len[g_page][r]; i++)
        {
            RECT rc = pages[g_page][r][i].rc;
            InflateRect(&rc, g_gap / 2 + 1, g_gap / 2 + 1);
            if (PtInRect(&rc, pt)) return &pages[g_page][r][i];
        }
    return NULL;
}

static const WCHAR *key_name(const struct key *k, WCHAR *buf)
{
    switch (k->kind)
    {
    case K_CHAR:
        if (k->ch >= 'a' && k->ch <= 'z') swprintf(buf, 8, L"%lc", k->ch);
        else swprintf(buf, 8, L"u%04x", k->ch);
        return buf;
    case K_BACK: return L"backspace";
    case K_ENTER: return L"enter";
    case K_SHIFT: return L"shift";
    case K_TAB: return L"tab";
    case K_SPACE: return L"space";
    case K_LEFT: return L"left";
    case K_RIGHT: return L"right";
    case K_PAGE: return L"page";
    case K_MORE: return L"more";
    case K_CTRL: return L"ctrl";
    case K_HIDE: return L"hide";
    }
    return L"?";
}

static void write_dump(void)
{
    FILE *f;
    RECT wr;
    WCHAR name[8];
    int r, i;

    if (!g_dump[0] || !(f = _wfopen(g_dump, L"w"))) return;
    GetWindowRect(g_wnd, &wr);
    fwprintf(f, L"WINDOW %d %ld %ld %ld %ld\n", g_shown && IsWindowVisible(g_wnd), wr.left, wr.top,
             wr.right - wr.left, wr.bottom - wr.top);
    fwprintf(f, L"STATE page=%d shift=%d ctrl=%d auto=%d tray=%d\n", g_page, g_shift, g_ctrl, g_auto, g_tray);
    for (r = 0; r < ROWS; r++)
        for (i = 0; i < page_len[g_page][r]; i++)
        {
            const struct key *k = &pages[g_page][r][i];
            if (k->kind == K_SHIFT && i) continue;   /* the right Shift: the left one's name */
            fwprintf(f, L"KEY %ls %ld %ld\n", key_name(k, name), wr.left + (k->rc.left + k->rc.right) / 2,
                     wr.top + (k->rc.top + k->rc.bottom) / 2);
        }
    fclose(f);
}

/* ---- drawing -------------------------------------------------------------- */

struct art { int kind; COLORREF color; };

/* the special keys' pictures, drawn large and smoothed down (sg-smooth.h) */
static void draw_art(HDC dc, int w, int h, const void *arg)
{
    const struct art *a = arg;
    int s = min(w, h), cx = w / 2, cy = h / 2, t = max(SG_SS, s / 14);
    HPEN pen = CreatePen(PS_SOLID, t, a->color), op = SelectObject(dc, pen);
    HBRUSH ob = SelectObject(dc, GetStockObject(NULL_BRUSH));

    switch (a->kind)
    {
    case K_BACK:
    {
        POINT p[5] = { { cx - s * 5 / 16, cy }, { cx - s / 8, cy - s / 5 }, { cx + s * 5 / 16, cy - s / 5 },
                       { cx + s * 5 / 16, cy + s / 5 }, { cx - s / 8, cy + s / 5 } };
        Polygon(dc, p, 5);
        MoveToEx(dc, cx - s / 32, cy - s / 12, NULL); LineTo(dc, cx + s * 5 / 32, cy + s / 12);
        MoveToEx(dc, cx + s * 5 / 32, cy - s / 12, NULL); LineTo(dc, cx - s / 32, cy + s / 12);
        break;
    }
    case K_ENTER:
        MoveToEx(dc, cx + s / 4, cy - s / 5, NULL); LineTo(dc, cx + s / 4, cy + s / 10); LineTo(dc, cx - s / 4, cy + s / 10);
        MoveToEx(dc, cx - s / 8, cy - s / 40, NULL); LineTo(dc, cx - s / 4, cy + s / 10); LineTo(dc, cx - s / 8, cy + s * 9 / 40);
        break;
    case K_SHIFT:
    {
        POINT p[7] = { { cx, cy - s / 4 }, { cx + s / 5, cy }, { cx + s / 11, cy }, { cx + s / 11, cy + s / 5 },
                       { cx - s / 11, cy + s / 5 }, { cx - s / 11, cy }, { cx - s / 5, cy } };
        Polygon(dc, p, 7);
        break;
    }
    case K_LEFT:
        MoveToEx(dc, cx + s / 12, cy - s / 7, NULL); LineTo(dc, cx - s / 12, cy); LineTo(dc, cx + s / 12, cy + s / 7);
        break;
    case K_RIGHT:
        MoveToEx(dc, cx - s / 12, cy - s / 7, NULL); LineTo(dc, cx + s / 12, cy); LineTo(dc, cx - s / 12, cy + s / 7);
        break;
    case K_HIDE:
    {
        /* a keyboard, and a chevron down under it */
        int kw = s * 9 / 20, kh = s / 4, i;
        Rectangle(dc, cx - kw, cy - kh, cx + kw, cy + kh / 3);
        for (i = 0; i < 4; i++)
        {
            int x = cx - kw + (2 * kw) * (i + 1) / 5;
            MoveToEx(dc, x, cy - kh * 2 / 3, NULL); LineTo(dc, x + t, cy - kh * 2 / 3);
        }
        MoveToEx(dc, cx - kw / 2, cy - kh / 8, NULL); LineTo(dc, cx + kw / 2, cy - kh / 8);
        MoveToEx(dc, cx - s / 10, cy + kh * 2 / 3, NULL); LineTo(dc, cx, cy + kh); LineTo(dc, cx + s / 10, cy + kh * 2 / 3);
        break;
    }
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

static BOOL key_down(const struct key *k)
{
    int i;
    for (i = 0; i < (int)ARRAYSIZE(g_down); i++) if (g_down[i].used && g_down[i].key == k) return TRUE;
    return FALSE;
}

static void paint(HDC hdc)
{
    BOOL dark = sg_system_dark();
    COLORREF bg = dark ? RGB(32, 32, 32) : RGB(228, 228, 228);
    COLORREF key = dark ? RGB(58, 58, 58) : RGB(253, 253, 253);
    COLORREF special = dark ? RGB(43, 43, 43) : RGB(240, 240, 240);
    COLORREF text = dark ? RGB(255, 255, 255) : RGB(20, 20, 20);
    COLORREF accent = sg_accent();
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, g_w, g_h), ob = SelectObject(dc, bmp);
    HBRUSH b = CreateSolidBrush(bg);
    RECT all = { 0, 0, g_w, g_h };
    int r, i, radius = max(4, (int)(5 * dpi_scale()));

    FillRect(dc, &all, b);
    DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);
    for (r = 0; r < ROWS; r++)
        for (i = 0; i < page_len[g_page][r]; i++)
        {
            struct key *k = &pages[g_page][r][i];
            BOOL lit = (k->kind == K_SHIFT && g_shift) || (k->kind == K_CTRL && g_ctrl);
            COLORREF face = k->kind == K_CHAR || k->kind == K_SPACE ? key : special;
            COLORREF ink = text;
            HBRUSH fb;
            HPEN np = SelectObject(dc, GetStockObject(NULL_PEN));
            WCHAR label[4];
            const WCHAR *s = k->label;

            if (key_down(k)) face = dark ? RGB(96, 96, 96) : RGB(200, 200, 200);
            if (lit || (key_down(k) && k->kind == K_ENTER))
            {
                face = key_down(k) ? sg_accent_dark(20) : accent;
                ink = RGB(255, 255, 255);
            }
            fb = CreateSolidBrush(face);
            SelectObject(dc, fb);
            sg_round_rect(dc, k->rc.left, k->rc.top, k->rc.right + 1, k->rc.bottom + 1, radius * 2, radius * 2);
            SelectObject(dc, np);
            DeleteObject(fb);

            if (k->kind == K_CHAR)
            {
                WCHAR c = k->ch;
                if (g_shift && c >= 'a' && c <= 'z') c = towupper(c);
                label[0] = c; label[1] = 0;
                s = label;
            }
            SetTextColor(dc, ink);
            if (s)
            {
                RECT tr = k->rc;
                SelectObject(dc, k->kind == K_CHAR ? g_font : g_font_small);
                DrawTextW(dc, s, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
            else if (k->kind != K_SPACE)
            {
                struct art a = { k->kind, sg_smooth_colour(ink) };
                int w = k->rc.right - k->rc.left, h = k->rc.bottom - k->rc.top, side = min(w, h);
                sg_smooth(dc, k->rc.left + (w - side) / 2, k->rc.top + (h - side) / 2, side, side, draw_art, &a);
                if (k->kind == K_SHIFT && g_shift == 2)
                {
                    /* Caps Lock: a bar under the arrow */
                    RECT bar = { k->rc.left + w / 2 - side / 8, k->rc.bottom - side / 5, k->rc.left + w / 2 + side / 8,
                                 k->rc.bottom - side / 5 + max(2, side / 24) };
                    HBRUSH ib = CreateSolidBrush(ink);
                    FillRect(dc, &bar, ib);
                    DeleteObject(ib);
                }
            }
        }
    BitBlt(hdc, 0, 0, g_w, g_h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
}

/* ---- typing --------------------------------------------------------------- */

/* A Linux program's window in front (in its Wine frame, explorer's
 * SgLinuxWindow) has the X focus itself: SendInput reaches Wine's windows
 * only, so its keys go through sg-session's sg-xtype (XTEST on the session's
 * X server) instead. Without sg-xtype, SendInput as for any window. */
static WCHAR g_xtype[MAX_PATH];

static BOOL linux_in_front(void)
{
    HWND fg = GetForegroundWindow();
    WCHAR cls[32];
#ifdef SG_MUTANT_TOUCHKBD_NO_XTYPE
    return FALSE;
#endif
    return fg && GetClassNameW(fg, cls, ARRAYSIZE(cls)) && !wcscmp(cls, L"SgLinuxWindow");
}

static void find_xtype(void)
{
    WCHAR *(CDECL *to_dos)(const char *) = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name");
    char unix_path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("SG_XTYPE", unix_path, MAX_PATH);
    WCHAR *dos;

    if (!n || n >= MAX_PATH) lstrcpyA(unix_path, "/usr/libexec/stained-glass/sg-xtype");
    if (!to_dos || !(dos = to_dos(unix_path))) return;
    if (GetFileAttributesW(dos) != INVALID_FILE_ATTRIBUTES) lstrcpynW(g_xtype, dos, MAX_PATH);
    HeapFree(GetProcessHeap(), 0, dos);
}

/* TOKEN (sg-xtype's: "u:0061", "k:BackSpace") to the Linux program in front,
 * with Ctrl if latched; FALSE when it cannot */
static BOOL xtype(const WCHAR *token)
{
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR cmd[MAX_PATH + 64];

    if (!g_xtype[0] || !linux_in_front()) return FALSE;
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" %ls%ls", g_xtype, g_ctrl ? L"ctrl+" : L"", token);
    cmd[ARRAYSIZE(cmd) - 1] = 0;
    if (!CreateProcessW(g_xtype, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return FALSE;
    WaitForSingleObject(pi.hProcess, 2000);   /* keys in order */
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

static const WCHAR *vk_keysym(WORD vk)
{
    switch (vk)
    {
    case VK_BACK: return L"k:BackSpace";
    case VK_RETURN: return L"k:Return";
    case VK_TAB: return L"k:Tab";
    case VK_LEFT: return L"k:Left";
    case VK_RIGHT: return L"k:Right";
    }
    return NULL;
}

static void send_vk(WORD vk, BOOL up, BOOL extended)
{
    INPUT in = { INPUT_KEYBOARD };
    in.ki.wVk = vk;
    in.ki.wScan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (extended ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &in, sizeof(in));
}

static void tap_vk(WORD vk, BOOL extended)
{
    WCHAR token[16];
    if (vk >= 'A' && vk <= 'Z') _snwprintf(token, ARRAYSIZE(token), L"u:%04x", vk - 'A' + 'a');
    else if (vk_keysym(vk)) lstrcpyW(token, vk_keysym(vk));
    else token[0] = 0;
    if (token[0] && xtype(token)) return;
    if (g_ctrl) send_vk(VK_CONTROL, FALSE, FALSE);
    send_vk(vk, FALSE, extended);
    send_vk(vk, TRUE, extended);
    if (g_ctrl) send_vk(VK_CONTROL, TRUE, FALSE);
}

static void type_char(WCHAR c)
{
    HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), NULL));
    SHORT vks = VkKeyScanExW(c, layout);
    BOOL shift = vks != -1 && (HIBYTE(vks) & 1);
    WCHAR token[16];

    _snwprintf(token, ARRAYSIZE(token), L"u:%04x", c);
    if (xtype(token)) return;

    if (vks != -1 && !(HIBYTE(vks) & 6))
    {
        /* the layout's own key, as a hardware keyboard sends it */
        if (shift) send_vk(VK_SHIFT, FALSE, FALSE);
        tap_vk(LOBYTE(vks), FALSE);
        if (shift) send_vk(VK_SHIFT, TRUE, FALSE);
    }
    else
    {
        INPUT in[2] = { { INPUT_KEYBOARD }, { INPUT_KEYBOARD } };
        in[0].ki.wScan = in[1].ki.wScan = c;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }
}

static void show_keyboard(BOOL show, BOOL automatic);

static void press(struct key *k)
{
    switch (k->kind)
    {
    case K_CHAR:
    case K_SPACE:
    {
        WCHAR c = k->ch;
        if (g_shift && c >= 'a' && c <= 'z') c = towupper(c);
        if (g_ctrl && c >= 'a' && c <= 'z') tap_vk(towupper(c), FALSE);
        else type_char(c);
        if (g_shift == 1) g_shift = 0;
        g_ctrl = FALSE;
        break;
    }
    case K_BACK: tap_vk(VK_BACK, FALSE); break;
    case K_ENTER: tap_vk(VK_RETURN, FALSE); g_ctrl = FALSE; break;
    case K_TAB: tap_vk(VK_TAB, FALSE); g_ctrl = FALSE; break;
    case K_LEFT: tap_vk(VK_LEFT, TRUE); break;
    case K_RIGHT: tap_vk(VK_RIGHT, TRUE); break;
    case K_SHIFT:
        if (g_shift == 1 && GetTickCount() - g_shift_time < 450) g_shift = 2;   /* twice: Caps Lock */
        else g_shift = g_shift ? 0 : 1;
        g_shift_time = GetTickCount();
        break;
    case K_CTRL: g_ctrl = !g_ctrl; break;
    case K_PAGE:
    case K_MORE: g_page = k->page; break;
    case K_HIDE:
        g_hide_time = GetTickCount();
        show_keyboard(FALSE, FALSE);
        return;
    }
    InvalidateRect(g_wnd, NULL, FALSE);
    write_dump();
}

static void pointer_down(UINT32 id, POINT pt)
{
    struct key *k = key_at(pt);
    int i, free_slot = -1;

    for (i = 0; i < (int)ARRAYSIZE(g_down); i++)
    {
        if (g_down[i].used && g_down[i].id == id) { free_slot = i; break; }
        if (!g_down[i].used && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0 || !k) return;
    g_down[free_slot].used = TRUE;
    g_down[free_slot].id = id;
    g_down[free_slot].key = k;
    if (k->kind == K_BACK)
    {
        /* at once, and again while held */
        press(k);
        g_repeat_key = k;
        SetTimer(g_wnd, T_REPEAT, 450, NULL);
    }
    InvalidateRect(g_wnd, NULL, FALSE);
}

static void pointer_move(UINT32 id, POINT pt)
{
    int i;
    for (i = 0; i < (int)ARRAYSIZE(g_down); i++)
        if (g_down[i].used && g_down[i].id == id)
        {
            struct key *k = key_at(pt);
            /* sliding to another key: that one types (not Backspace, which already did) */
            if (k && k != g_down[i].key && g_down[i].key->kind != K_BACK)
            {
                g_down[i].key = k;
                InvalidateRect(g_wnd, NULL, FALSE);
            }
        }
}

static void pointer_up(UINT32 id)
{
    int i;
    for (i = 0; i < (int)ARRAYSIZE(g_down); i++)
        if (g_down[i].used && g_down[i].id == id)
        {
            struct key *k = g_down[i].key;
            g_down[i].used = FALSE;
            if (k == g_repeat_key)
            {
                KillTimer(g_wnd, T_REPEAT);
                g_repeat_key = NULL;
            }
            else press(k);
            InvalidateRect(g_wnd, NULL, FALSE);
        }
}

/* ---- showing --------------------------------------------------------------- */

static void place(void)
{
    MONITORINFO mi = { sizeof(mi) };
    HWND fg = GetForegroundWindow();
    float s = dpi_scale();
    int h;

    /* across the bottom of the work area, over the programs (no space taken
     * from the work area: that resizes every maximized program each time it
     * shows, and tells every window of it) */
    GetMonitorInfoW(MonitorFromWindow(fg ? fg : g_wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    h = (mi.rcWork.bottom - mi.rcWork.top) * 38 / 100;
    h = max(h, (int)(200 * s));
    h = min(h, (int)(360 * s));
    SetWindowPos(g_wnd, HWND_TOPMOST, mi.rcWork.left, mi.rcWork.bottom - h, mi.rcWork.right - mi.rcWork.left, h,
                 SWP_NOACTIVATE);
    layout();
}

static void show_keyboard(BOOL show, BOOL automatic)
{
    if (show)
    {
        if (!g_shown) g_page = 0;
        g_shown = TRUE;
        g_auto = automatic;
        place();
        ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        InvalidateRect(g_wnd, NULL, FALSE);
    }
    else if (g_shown)
    {
        g_shown = FALSE;
        g_auto = FALSE;
        g_ctrl = FALSE;
        if (g_shift == 1) g_shift = 0;
        ShowWindow(g_wnd, SW_HIDE);
    }
    write_dump();
}

/* the focus is a text field */
static BOOL text_field(HWND *focus)
{
    GUITHREADINFO gti = { sizeof(gti) };
    HWND fg = GetForegroundWindow(), touched = GetPropW(GetDesktopWindow(), L"__wine_sg_touch_hwnd");
    DWORD thread = 0;
    WCHAR cls[64];

    *focus = NULL;
    /* the touched window's thread (wine-sg 1150 names it), else the one in front */
    if (touched && IsWindow(touched) && touched != g_wnd) thread = GetWindowThreadProcessId(touched, NULL);
    if (!thread && fg) thread = GetWindowThreadProcessId(fg, NULL);
    if (!thread || !GetGUIThreadInfo(thread, &gti) || !gti.hwndFocus) return FALSE;
    *focus = gti.hwndFocus;
    if (gti.hwndFocus == g_wnd) return FALSE;
    if (GetClassNameW(gti.hwndFocus, cls, ARRAYSIZE(cls)))
    {
        WCHAR *p;
        for (p = cls; *p; p++) *p = towlower(*p);
        if (wcsstr(cls, L"edit") || wcsstr(cls, L"textbox"))
            return !(GetWindowLongW(gti.hwndFocus, GWL_STYLE) & ES_READONLY);
    }
    return gti.hwndCaret != NULL;
}

/* the window FOCUS is in fills its screen (a full-screen program, the login
 * screen) */
static BOOL full_screen(HWND focus)
{
    HWND top = GetAncestor(focus, GA_ROOT);
    MONITORINFO mi = { sizeof(mi) };
    RECT rc;

    if (!top || !GetWindowRect(top, &rc) || !GetMonitorInfoW(MonitorFromWindow(top, MONITOR_DEFAULTTONEAREST), &mi))
        return FALSE;
    return rc.left <= mi.rcMonitor.left && rc.top <= mi.rcMonitor.top && rc.right >= mi.rcMonitor.right &&
           rc.bottom >= mi.rcMonitor.bottom;
}

/* Settings > Devices > Typing (sg-control): "Show the touch keyboard when not
 * in tablet mode and there's no keyboard attached" is Windows' value
 * EnableDesktopModeAutoInvoke -- here there is no tablet mode, so it is the
 * switch for showing itself at all (on unless turned off); "Automatically
 * show the touch keyboard in windowed apps when there's no keyboard
 * attached" is ours, AutoInvokeInWindowedApps (on unless turned off): off,
 * it shows itself only for full-screen programs and the screens before
 * sign-in. Never with a hardware keyboard attached. */
static BOOL auto_wanted(HWND focus)
{
    if (!reg_get(L"EnableDesktopModeAutoInvoke", 1)) return FALSE;
    if (!reg_get(L"AutoInvokeInWindowedApps", 1) && !full_screen(focus)) return FALSE;
    return !has_keyboard();
}

/* a touch went down: show the keyboard for a text field it focused, hide
 * one that showed itself when it focused something else */
static void check_focus(void)
{
    HWND focus;
    BOOL text = text_field(&focus);

#ifdef SG_MUTANT_TOUCHKBD_NO_AUTOSHOW
    return;
#endif
    if (text && !g_shown)
    {
        if (!auto_wanted(focus)) return;
        show_keyboard(TRUE, TRUE);
    }
    else if (!text && g_shown && g_auto) show_keyboard(FALSE, FALSE);
}

static void poll_touch(void)
{
    DWORD stamp = (DWORD)(ULONG_PTR)GetPropW(GetDesktopWindow(), L"__wine_sg_touch_time");

    /* a keyboard attached meanwhile (a Type Cover clicked on): the keyboard
     * that showed itself goes, every two seconds looked at */
#ifndef SG_MUTANT_TOUCHKBD_NO_LIVE_KEYBOARD
    {
        static int ticks;
        if (g_shown && g_auto && ++ticks % 8 == 0 && has_keyboard()) show_keyboard(FALSE, FALSE);
    }
#endif

    if (!stamp || stamp == g_last_touch) return;
    g_last_touch = stamp;
    /* not a touch on the keyboard's own hide key */
    if ((int)(stamp - g_hide_time) <= 0) return;
    g_touch_seen = GetTickCount();
    /* the focus moves as the touch lifts: looked at a little later, twice */
    SetTimer(g_wnd, T_FOCUS, 250, NULL);
}

/* ---- the taskbar button ------------------------------------------------------ */

struct icon_art { COLORREF color; };

static void draw_icon_art(HDC dc, int w, int h, const void *arg)
{
    const struct icon_art *a = arg;
    int t = max(SG_SS, w / 14), i, j;
    HPEN pen = CreatePen(PS_SOLID, t, a->color), op = SelectObject(dc, pen);
    HBRUSH brush = CreateSolidBrush(a->color), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RECT body = { w / 16, h * 4 / 16, w - w / 16, h * 13 / 16 };

    RoundRect(dc, body.left, body.top, body.right, body.bottom, w / 6, w / 6);
    for (j = 0; j < 2; j++)
        for (i = 0; i < 5; i++)
        {
            RECT k = { body.left + w * (2 + i * 5) / 32, body.top + h * (2 + j * 3) / 16,
                       body.left + w * (4 + i * 5) / 32, body.top + h * (4 + j * 3) / 16 };
            FillRect(dc, &k, brush);
        }
    {
        RECT space = { body.left + w * 7 / 32, body.top + h * 8 / 16, body.right - w * 7 / 32, body.top + h * 10 / 16 };
        FillRect(dc, &space, brush);
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    DeleteObject(brush);
}

static HICON make_icon(int size)
{
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB } };
    struct icon_art a = { sg_system_dark() ? RGB(255, 255, 255) : RGB(16, 16, 16) };
    ICONINFO ii = { TRUE };
    DWORD *px;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0), old, mask;
    HICON icon;

    old = SelectObject(dc, color);
    memset(px, 0, size * size * 4);
    sg_smooth(dc, 0, 0, size, size, draw_icon_art, &a);
    GdiFlush();
    SelectObject(dc, old);
    mask = CreateBitmap(size, size, 1, 1, NULL);
    ii.hbmColor = color;
    ii.hbmMask = mask;
    icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    DeleteDC(dc);
    return icon;
}

/* "Show touch keyboard button" (the taskbar's menu): Windows' value
 * TipbandDesiredVisibility; not set, on with a touch screen or pen */
static BOOL tray_wanted(void)
{
#ifndef SG_MUTANT_TOUCHKBD_TRAY_ANYWAY
    if (g_notray) return FALSE;
#endif
    return reg_get(L"TipbandDesiredVisibility", has_touch_screen()) != 0;
}

static void tray_add(void)
{
    HICON old = g_nid.hIcon;
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = make_icon(GetSystemMetrics(SM_CXSMICON));
    lstrcpynW(g_nid.szTip, L"Touch keyboard", ARRAYSIZE(g_nid.szTip));
    g_tray = Shell_NotifyIconW(NIM_ADD, &g_nid) || Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    if (old) DestroyIcon(old);
}

/* ---- the window ------------------------------------------------------------- */

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbar_created && g_taskbar_created)
    {
        if (g_tray) tray_add();
        return 0;
    }
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_POINTERACTIVATE:
        return PA_NOACTIVATE;
    case WM_POINTERDOWN:
    case WM_POINTERUPDATE:
    case WM_POINTERUP:
    {
        /* touches and the pen: taken here, so no mouse is made of them */
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        UINT32 id = GET_POINTERID_WPARAM(wp);
        ScreenToClient(hwnd, &pt);
        if (msg == WM_POINTERDOWN) pointer_down(id, pt);
        else if (msg == WM_POINTERUPDATE) pointer_move(id, pt);
        else pointer_up(id);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        /* a pen's or a touch's mouse: its pointer messages came first */
        if ((GetMessageExtraInfo() & 0xffffff00) == 0xff515700) return 0;
        if (msg == WM_LBUTTONDOWN) { SetCapture(hwnd); pointer_down(0, pt); }
        else if (msg == WM_MOUSEMOVE) { if (wp & MK_LBUTTON) pointer_move(0, pt); }
        else { ReleaseCapture(); pointer_up(0); }
        return 0;
    }
    case WM_TIMER:
        if (wp == T_POLL) poll_touch();
        else if (wp == T_REPEAT)
        {
            if (g_repeat_key && key_down(g_repeat_key)) press(g_repeat_key);
            SetTimer(hwnd, T_REPEAT, 60, NULL);
        }
        else if (wp == T_FOCUS)
        {
            check_focus();
            /* once more a little later: a program focusing its field late */
            if (GetTickCount() - g_touch_seen < 600) SetTimer(hwnd, T_FOCUS, 400, NULL);
            else KillTimer(hwnd, T_FOCUS);
        }
        return 0;
    case WM_TRAY:
        if (lp == WM_LBUTTONUP || lp == NIN_SELECT || lp == NIN_KEYSELECT)
        {
            if (g_shown) g_hide_time = GetTickCount();
            show_keyboard(!g_shown, FALSE);
        }
        return 0;
    case WM_COMMAND_LINE:
        if (wp == CMD_SHOW) show_keyboard(TRUE, FALSE);
        else if (wp == CMD_HIDE) show_keyboard(FALSE, FALSE);
        else if (wp == CMD_TOGGLE) show_keyboard(!g_shown, FALSE);
        return 0;
    case WM_DISPLAYCHANGE:
        if (g_shown) place();
        return 0;
    case WM_SETTINGCHANGE:
        if (sg_mode_changed(msg, lp))
        {
            if (g_tray) tray_add();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        else if (wp == SPI_SETWORKAREA && g_shown) { place(); write_dump(); }
        else if (lp && !lstrcmpW((const WCHAR *)lp, L"TraySettings"))
        {
            /* the button turned on or off from the taskbar's menu */
            if (tray_wanted() && !g_tray) tray_add();
            else if (!tray_wanted() && g_tray)
            {
                Shell_NotifyIconW(NIM_DELETE, &g_nid);
                g_tray = FALSE;
            }
            write_dump();
        }
        return 0;
    case WM_SIZE:
        layout();
        write_dump();
        return 0;
#ifndef SG_MUTANT_TOUCHKBD_DPI_IGNORED
    case WM_DPICHANGED:
        /* a new display scale: its keys, gaps and height at it */
        if (g_shown) place();
        else { sg_dpi_apply_rect(hwnd, lp); layout(); }
        InvalidateRect(hwnd, NULL, FALSE);
        write_dump();
        return 0;
#endif
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (g_tray) Shell_NotifyIconW(NIM_DELETE, &g_nid);
        show_keyboard(FALSE, FALSE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int parse_command(const WCHAR *cmdline, BOOL *background)
{
    *background = FALSE;
    if (!cmdline) return CMD_SHOW;
    while (*cmdline == ' ') cmdline++;
    if (!_wcsnicmp(cmdline, L"/background", 11)) {
        *background = TRUE;
        if (wcsstr(cmdline + 11, L"/notray")) g_notray = TRUE;
        return 0;
    }
    if (!_wcsnicmp(cmdline, L"/hide", 5)) return CMD_HIDE;
    if (!_wcsnicmp(cmdline, L"/toggle", 7)) return CMD_TOGGLE;
    return CMD_SHOW;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    BOOL background;
    int command = parse_command(cmdline, &background);
    HANDLE mutex;
    DWORD n;
    MSG msg;
    (void)prev; (void)show;

    g_inst = inst;
#ifndef SG_MUTANT_TOUCHKBD_DPI_IGNORED
    sg_dpi_init();      /* the display scale, a new one too (WM_DPICHANGED) */
#endif
    n = GetEnvironmentVariableW(L"SG_TOUCHKBD_DUMP", g_dump, MAX_PATH);
    if (!n || n >= MAX_PATH) g_dump[0] = 0;
    n = GetEnvironmentVariableW(L"SG_TOUCHKBD_DEVICES", g_devices, MAX_PATH);
    if (!n || n >= MAX_PATH) lstrcpyW(g_devices, L"\\\\?\\unix\\proc\\bus\\input\\devices");
    g_force_touch = GetEnvironmentVariableW(L"SG_TOUCHKBD_TOUCH", NULL, 0) != 0;

    mutex = CreateMutexW(NULL, TRUE, L"Local\\StainedGlassTouchKeyboard");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND other;
        int i;
        /* the running keyboard does it (it may still be starting) */
        for (i = 0; i < 50 && !(other = FindWindowW(CLASS_NAME, NULL)); i++) Sleep(100);
        if (other && command) PostMessageW(other, WM_COMMAND_LINE, command, 0);
        return 0;
    }
    if (background && !has_touch_screen() && !tray_wanted()) return 0;

    build_pages();
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, CLASS_NAME, L"Touch keyboard",
                            WS_POPUP, 0, 0, 800, 300, NULL, NULL, inst, NULL);
    if (!g_wnd) return 1;
    layout();
    find_xtype();
    if (tray_wanted()) tray_add();
    if (has_touch_screen()) SetTimer(g_wnd, T_POLL, 250, NULL);
    if (command) show_keyboard(command != CMD_HIDE, FALSE);
    write_dump();

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}
