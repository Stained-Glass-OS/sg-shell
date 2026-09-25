/* sg-netflyout -- the taskbar's network icon and its flyout.
 *
 * A notification-area icon (Shell_NotifyIcon, so explorer's tray shows it like
 * any program's) that says how this computer is connected. Clicking it opens
 * the Windows 10-style network flyout: the wired connection, the Wi-Fi
 * networks in range with their signal and security, Connect (with "Connect
 * automatically"), the network security key prompt, Disconnect, the Wi-Fi
 * button, and "Network & Internet settings". Joining a network is for anyone
 * signed in; sg-netd (see sg-netclient.h) does it, and says no to what a
 * standard user may not do.
 *
 *   sg-netflyout [--open [--expand SSIDHEX [--prompt]]]
 *   sg-netflyout --dump                          what the flyout would list (stderr)
 *   sg-netflyout --connect SSIDHEX [--password-file PATH] [--no-autoconnect]
 *                                                what Connect does (for gates)
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include "sg-netclient.h"
#include "sg-mode.h"

#define FLY_W 360
#define ROW_H 60
#define EXP_H 64
#define EXP_H_KEY 96
#define FOOT_H 132
#define MAX_NETS 48
#define MAX_WIRED 8
#define MAX_LIST_H 420
#define WM_TRAY (WM_APP + 1)
#define ID_KEY 100

/* the flyout and the icon follow the Windows mode (SystemUsesLightTheme,
 * sg-mode.h), as the taskbar they belong to does */
struct fly_palette {
    COLORREF bg, hover, open, text, subtle, link, button, button_hot, error, line, off, field;
    DWORD glyph;                /* the tray icon's colour (premultiplied: white, or black) */
};
static const struct fly_palette fly_dark = {
    RGB(0x1F, 0x1F, 0x1F), RGB(0x33, 0x33, 0x33), RGB(0x2B, 0x2B, 0x2B), RGB(0xFF, 0xFF, 0xFF),
    RGB(0xA8, 0xA8, 0xA8), RGB(0xB9, 0x8C, 0xF0), RGB(0x44, 0x44, 0x44), RGB(0x55, 0x55, 0x55),
    RGB(0xFF, 0x99, 0xA4), RGB(0x3A, 0x3A, 0x3A), RGB(0x60, 0x60, 0x60), RGB(0x10, 0x10, 0x10), 0xFFFFFF,
};
static const struct fly_palette fly_light = {
    RGB(0xF2, 0xF2, 0xF2), RGB(0xDA, 0xDA, 0xDA), RGB(0xE6, 0xE6, 0xE6), RGB(0x00, 0x00, 0x00),
    RGB(0x5A, 0x5A, 0x5A), RGB(0x5F, 0x24, 0x96), RGB(0xCC, 0xCC, 0xCC), RGB(0xB8, 0xB8, 0xB8),
    RGB(0xC4, 0x2B, 0x1C), RGB(0xCC, 0xCC, 0xCC), RGB(0xB0, 0xB0, 0xB0), RGB(0xFF, 0xFF, 0xFF), 0x000000,
};
static const struct fly_palette *g_pal = &fly_dark;
static void load_mode(void) { g_pal = sg_system_dark() ? &fly_dark : &fly_light; }
#define COL_BG      (g_pal->bg)
#define COL_HOVER   (g_pal->hover)
#define COL_OPEN    (g_pal->open)
#define COL_TEXT    (g_pal->text)
#define COL_SUBTLE  (g_pal->subtle)
#define COL_ACCENT  RGB(0x7B, 0x2F, 0xBE)
#define COL_LINK    (g_pal->link)
#define COL_BUTTON  (g_pal->button)
#define COL_ERROR   (g_pal->error)
#define COL_LINE    (g_pal->line)

struct wnet {
    char hex[72];
    WCHAR name[64];
    int signal;
    char security[16];
    BOOL inuse, saved;
};
struct wired { WCHAR name[32]; BOOL connected; };

static struct wnet g_nets[MAX_NETS];
static struct wired g_wired[MAX_WIRED];
static int g_nnets, g_nwired, g_scroll;
static BOOL g_has_wifi, g_radio_on = TRUE, g_busy, g_autoconnect = TRUE;
static char g_wifi_dev[32];
static HWND g_tray_wnd, g_fly, g_key;
static HFONT g_font, g_font_small, g_font_title;
static UINT g_taskbar_created;
static NOTIFYICONDATAW g_nid;

enum { EXP_IDLE, EXP_KEY, EXP_BUSY, EXP_ERROR };
static int g_exp = -1, g_exp_state = EXP_IDLE, g_hot = -1, g_hot_part;
static WCHAR g_exp_msg[256];

/* --- the data ---------------------------------------------------------------------- */

static void refresh(BOOL pumped, BOOL rescan)
{
    struct net_reply r;
    const char *ad[] = { "adapters" };
    const char *sc[] = { "wifi", "scan", "--rescan" };
    const char *ra[] = { "wifi", "radio", "status" };
    char keep[72] = "";
    int i, eth = 0;

    if (g_exp >= 0 && g_exp < g_nnets) snprintf(keep, sizeof(keep), "%s", g_nets[g_exp].hex);
    pumped ? net_request_pumped(1, ad, NULL, &r) : net_request_argv(1, ad, NULL, &r);
    g_nwired = 0;
    g_has_wifi = FALSE;
    for (i = 0; r.ok && i < r.nlines; i++)
    {
        if (strncmp(r.lines[i], "ADAPTER ", 8)) continue;
        {
            const char *type = net_field(&r, i, "TYPE", 0), *state = net_field(&r, i, "STATE", 0);
            if (type && !strcmp(type, "wifi"))
            {
                g_has_wifi = TRUE;
                snprintf(g_wifi_dev, sizeof(g_wifi_dev), "%s", r.lines[i] + 8);
            }
            else if (type && !strcmp(type, "ethernet") && g_nwired < MAX_WIRED)
            {
                struct wired *w = &g_wired[g_nwired++];
                eth++;
                if (eth > 1) _snwprintf(w->name, 32, L"Ethernet %d", eth);
                else wcscpy(w->name, L"Ethernet");
                w->connected = state && !strcmp(state, "connected");
            }
        }
    }
    net_reply_free(&r);

    g_nnets = 0;
    if (g_has_wifi)
    {
        pumped ? net_request_pumped(rescan ? 3 : 2, sc, NULL, &r) : net_request_argv(rescan ? 3 : 2, sc, NULL, &r);
        for (i = 0; r.ok && i < r.nlines && g_nnets < MAX_NETS; i++)
        {
            /* WIFI signal \t security \t in-use \t saved \t hex \t name */
            char *f[6], *p, buf[512];
            int k = 0;
            if (strncmp(r.lines[i], "WIFI ", 5)) continue;
            snprintf(buf, sizeof(buf), "%s", r.lines[i] + 5);
            for (p = buf, f[k++] = p; *p && k < 6; p++) if (*p == '\t') { *p = 0; f[k++] = p + 1; }
            if (k < 6) continue;
            {
                struct wnet *n = &g_nets[g_nnets++];
                n->signal = atoi(f[0]);
                snprintf(n->security, sizeof(n->security), "%s", f[1]);
                n->inuse = !strcmp(f[2], "yes");
                n->saved = !strcmp(f[3], "yes");
                snprintf(n->hex, sizeof(n->hex), "%s", f[4]);
                utf8_to_w(f[5], n->name, 64);
            }
        }
        net_reply_free(&r);
        /* the connected network first, as Windows lists it */
        for (i = 1; i < g_nnets; i++)
            if (g_nets[i].inuse) { struct wnet t = g_nets[i]; memmove(&g_nets[1], &g_nets[0], i * sizeof(t)); g_nets[0] = t; break; }
        pumped ? net_request_pumped(3, ra, NULL, &r) : net_request_argv(3, ra, NULL, &r);
        {
            const char *v = net_field(&r, 0, "RADIO", 0);
            g_radio_on = !v || strcmp(v, "disabled") != 0;
        }
        net_reply_free(&r);
    }
    g_exp = -1;
    for (i = 0; keep[0] && i < g_nnets; i++) if (!strcmp(g_nets[i].hex, keep)) g_exp = i;
    if (g_exp < 0) g_exp_state = EXP_IDLE;
}

static int connected_net(void)
{
    int i;
    for (i = 0; i < g_nnets; i++) if (g_nets[i].inuse) return i;
    return -1;
}

static BOOL any_wired(void)
{
    int i;
    for (i = 0; i < g_nwired; i++) if (g_wired[i].connected) return TRUE;
    return FALSE;
}

static BOOL secured(const struct wnet *n) { return strcmp(n->security, "open") != 0; }

/* What Connect does. key may be NULL (open network, or a saved one). */
static int do_connect(const char *hex, const char *key, BOOL autoconnect, WCHAR *err, int cch, BOOL pumped)
{
    const char *argv[8];
    struct net_reply r;
    int argc = 0;
    argv[argc++] = "wifi"; argv[argc++] = "connect"; argv[argc++] = "--ssid-hex"; argv[argc++] = hex;
    if (!autoconnect) argv[argc++] = "--no-autoconnect";
    if (key) argv[argc++] = "--password-stdin";
    pumped ? net_request_pumped(argc, argv, key, &r) : net_request_argv(argc, argv, key, &r);
    net_reply_free(&r);
    if (r.ok) return 0;
    net_error_text(&r, err, cch);
    return !strcmp(r.kind, "auth") ? 4 : !strcmp(r.kind, "denied") ? 3 : 1;
}

/* --- the tray icon, drawn here --------------------------------------------------------- */

static HICON make_icon(int size)
{
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB } };
    DWORD *px;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0), mask, old;
    ICONINFO ii = { TRUE };
    HICON icon;
    int i, s = size, cn = connected_net(), bars = cn >= 0 ? (g_nets[cn].signal + 12) / 25 : 0;
    BOOL wifi_up = g_has_wifi && g_radio_on && cn >= 0, wired_up = any_wired();
    old = SelectObject(dc, color);
    memset(px, 0, size * size * 4);
    SetBkMode(dc, TRANSPARENT);
    if (wifi_up || (!wired_up && g_has_wifi))
    {
        /* the Wi-Fi fan: bands lit to the signal strength */
        for (i = 3; i >= 0; i--)
        {
            int rad = s * (i + 1) / 4 - 1;
            BYTE v = wifi_up && i < (bars < 1 ? 1 : bars) ? 0xFF : 0x55;
            HBRUSH b = CreateSolidBrush(RGB(v, v, v)), ob = SelectObject(dc, b);
            HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
            Pie(dc, s / 2 - rad, s - 2 - rad, s / 2 + rad + 1, s - 2 + rad + 1, s / 2 + rad, s - 2 - rad, s / 2 - rad, s - 2 - rad);
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b);
            if (i > 0)
            {
                HBRUSH k = CreateSolidBrush(RGB(0, 0, 0)), ok = SelectObject(dc, k);
                int in = rad - (s >= 32 ? 3 : 1);
                op = SelectObject(dc, GetStockObject(NULL_PEN));
                Pie(dc, s / 2 - in, s - 2 - in, s / 2 + in + 1, s - 2 + in + 1, s / 2 + in, s - 2 - in, s / 2 - in, s - 2 - in);
                SelectObject(dc, ok); SelectObject(dc, op); DeleteObject(k);
            }
        }
    }
    else
    {
        /* a monitor with its cable: the wired connection */
        BYTE v = wired_up ? 0xFF : 0x80;
        HPEN pen = CreatePen(PS_SOLID, s >= 32 ? 2 : 1, RGB(v, v, v)), op = SelectObject(dc, pen);
        HBRUSH ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Rectangle(dc, s / 8, s / 8, s - s / 8, s * 5 / 8);
        MoveToEx(dc, s / 2, s * 5 / 8, NULL); LineTo(dc, s / 2, s * 13 / 16);
        MoveToEx(dc, s / 4, s * 13 / 16, NULL); LineTo(dc, s * 3 / 4, s * 13 / 16);
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
    }
    if (!wifi_up && !wired_up)
    {
        /* not connected: a small cross */
        HPEN pen = CreatePen(PS_SOLID, s >= 32 ? 3 : 2, RGB(0xFF, 0xFF, 0xFF)), op = SelectObject(dc, pen);
        MoveToEx(dc, s * 10 / 16, s * 10 / 16, NULL); LineTo(dc, s - 1, s - 1);
        MoveToEx(dc, s - 1, s * 10 / 16, NULL); LineTo(dc, s * 10 / 16, s - 1);
        SelectObject(dc, op); DeleteObject(pen);
    }
    GdiFlush();
    for (i = 0; i < size * size; i++)
    {
        BYTE a = (BYTE)(px[i] & 0xFF);
        px[i] = a ? ((DWORD)a << 24) | (g_pal->glyph ? 0x00FFFFFF : 0) : 0;
    }
    SelectObject(dc, old);
    mask = CreateBitmap(size, size, 1, 1, NULL);
    ii.hbmColor = color;
    ii.hbmMask = mask;
    icon = CreateIconIndirect(&ii);
    DeleteObject(color); DeleteObject(mask); DeleteDC(dc);
    return icon;
}

static void tray_update(BOOL add)
{
    int cn = connected_net();
    HICON old = g_nid.hIcon;
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_tray_wnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = make_icon(GetSystemMetrics(SM_CXSMICON));
    if (cn >= 0) _snwprintf(g_nid.szTip, 128, L"%ls\nInternet access", g_nets[cn].name);
    else if (any_wired()) _snwprintf(g_nid.szTip, 128, L"Network\nInternet access");
    else _snwprintf(g_nid.szTip, 128, L"Not connected - No connections are available");
    g_nid.szTip[127] = 0;
    Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &g_nid);
    if (old) DestroyIcon(old);
}

/* --- the flyout ---------------------------------------------------------------------------- */

static int list_top(void) { return 8; }
static int exp_h(void) { return g_exp_state == EXP_KEY ? EXP_H_KEY : EXP_H; }

static int row_y(int i)
{
    int y = list_top() - g_scroll + g_nwired * ROW_H, k;
    for (k = 0; k < i; k++) y += ROW_H + (k == g_exp ? exp_h() : 0);
    return y;
}

static int list_height(void)
{
    int h = g_nwired * ROW_H + g_nnets * ROW_H + (g_exp >= 0 ? exp_h() : 0);
    if (!g_nwired && !g_nnets) h = ROW_H;
    return h;
}

static int fly_height(void)
{
    int h = list_height();
    if (h > MAX_LIST_H) h = MAX_LIST_H;
    return list_top() + h + FOOT_H;
}

static void place_flyout(void)
{
    APPBARDATA abd = { sizeof(abd) };
    HWND bar = FindWindowW(L"Shell_TrayWnd", NULL);
    RECT work, tb;
    int h = fly_height(), x, y;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    x = work.right - FLY_W;
    y = work.bottom - h;
    /* Above the taskbar. Its own window says where it is: under Wine the work
     * area can include the taskbar, and ABM_GETTASKBARPOS can answer an empty
     * rectangle. */
    if (!(bar && GetWindowRect(bar, &tb)) && SHAppBarMessage(ABM_GETTASKBARPOS, &abd)) tb = abd.rc;
    else if (!bar) SetRectEmpty(&tb);
    if (tb.bottom > tb.top && tb.top > work.top + 100)
    {
        x = tb.right - FLY_W;
        y = tb.top - h;
    }
    /* a taskbar on another edge (Settings > Taskbar): beside its tray corner */
    abd.uEdge = ABE_BOTTOM;
    if (SHAppBarMessage(ABM_GETTASKBARPOS, &abd) && abd.rc.bottom - abd.rc.top > 1)
    {
        tb = abd.rc;
        switch (abd.uEdge)
        {
        case ABE_TOP:   x = tb.right - FLY_W; y = tb.bottom; break;
        case ABE_LEFT:  x = tb.right; y = tb.bottom - h; break;
        case ABE_RIGHT: x = tb.left - FLY_W; y = tb.bottom - h; break;
        default:        x = tb.right - FLY_W; y = tb.top - h; break;
        }
    }
    SetWindowPos(g_fly, HWND_TOPMOST, x, y, FLY_W, h, SWP_NOACTIVATE);
}

static void draw_wifi_glyph(HDC dc, int x, int y, int signal, BOOL lit)
{
    int i, bars = (signal + 12) / 25;
    for (i = 0; i < 4; i++)
    {
        int rad = 5 + i * 5;
        COLORREF c = lit && i < (bars < 1 ? 1 : bars) ? COL_TEXT : g_pal->off;
        HPEN pen = CreatePen(PS_SOLID, 2, c), op = SelectObject(dc, pen);
        if (i == 0)
        {
            HBRUSH b = CreateSolidBrush(c), ob = SelectObject(dc, b);
            Ellipse(dc, x + 12, y + 20, x + 17, y + 25);
            SelectObject(dc, ob); DeleteObject(b);
        }
        else Arc(dc, x + 14 - rad, y + 22 - rad, x + 15 + rad, y + 23 + rad, x + 15 + rad, y + 22 - rad, x + 14 - rad, y + 22 - rad);
        SelectObject(dc, op); DeleteObject(pen);
    }
}

static void draw_lock(HDC dc, int x, int y)
{
    HPEN pen = CreatePen(PS_SOLID, 1, COL_TEXT), op = SelectObject(dc, pen);
    HBRUSH b = CreateSolidBrush(COL_TEXT), ob = SelectObject(dc, b);
    Rectangle(dc, x, y + 4, x + 7, y + 9);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Arc(dc, x + 1, y, x + 7, y + 8, x + 7, y + 4, x + 1, y + 4);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen); DeleteObject(b);
}

static void draw_monitor(HDC dc, int x, int y, BOOL lit)
{
    HPEN pen = CreatePen(PS_SOLID, 2, lit ? COL_TEXT : COL_SUBTLE), op = SelectObject(dc, pen);
    HBRUSH ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, x + 3, y + 4, x + 27, y + 20);
    MoveToEx(dc, x + 15, y + 20, NULL); LineTo(dc, x + 15, y + 25);
    MoveToEx(dc, x + 9, y + 26, NULL); LineTo(dc, x + 22, y + 26);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
}

/* A flat button; returns its rectangle. */
static RECT button_rect(int right, int y, int w) { RECT r = { right - w, y, right, y + 30 }; return r; }

static void draw_button(HDC dc, RECT r, const WCHAR *text, BOOL accent, BOOL hot)
{
    HBRUSH b = CreateSolidBrush(accent ? (hot ? RGB(0x8E, 0x45, 0xD0) : COL_ACCENT) : (hot ? g_pal->button_hot : COL_BUTTON));
    FillRect(dc, &r, b);
    DeleteObject(b);
    SetTextColor(dc, COL_TEXT);
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static RECT check_rect(int y) { RECT r = { 60, y, 250, y + 22 }; return r; }

enum { PART_ROW = 1, PART_PRIMARY, PART_SECONDARY, PART_CHECK, PART_SETTINGS, PART_WIFI_TILE };

/* The expanded row's parts. With the key prompt: the prompt line, the key
 * field (g_key, a real edit control), then Next and Cancel side by side. */
static RECT key_rect(int i) { int y0 = row_y(i) + ROW_H; RECT r = { 60, y0 + 16, FLY_W - 16, y0 + 42 }; return r; }

static void expanded_parts(int i, RECT *primary, RECT *secondary, RECT *check)
{
    int y0 = row_y(i) + ROW_H;
    *check = check_rect(y0 + 2);
    if (g_exp_state == EXP_KEY)
    {
        int w = (FLY_W - 16 - 60 - 8) / 2;
        SetRect(primary, 60, y0 + 50, 60 + w, y0 + 80);
        SetRect(secondary, FLY_W - 16 - w, y0 + 50, FLY_W - 16, y0 + 80);
    }
    else
    {
        *primary = button_rect(FLY_W - 16, y0 + 26, 130);
        SetRectEmpty(secondary);
    }
}

static RECT settings_rect(void) { RECT r = { 16, 0, FLY_W - 16, 0 }; int top = fly_height() - FOOT_H + 12; r.top = top; r.bottom = top + 20; return r; }
static RECT wifi_tile_rect(void) { int top = fly_height() - FOOT_H + 56; RECT r = { 12, top, 12 + 104, top + 64 }; return r; }

static void on_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps), dc;
    RECT c, r, clip;
    HBITMAP bmp, oldbmp;
    HBRUSH b;
    int i, cn = connected_net(), list_h = list_height();

    GetClientRect(hwnd, &c);
    dc = CreateCompatibleDC(wdc);
    bmp = CreateCompatibleBitmap(wdc, c.right, c.bottom);
    oldbmp = SelectObject(dc, bmp);
    b = CreateSolidBrush(COL_BG); FillRect(dc, &c, b); DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);
    if (list_h > MAX_LIST_H) list_h = MAX_LIST_H;
    SetRect(&clip, 0, list_top(), c.right, list_top() + list_h);
    IntersectClipRect(dc, clip.left, clip.top, clip.right, clip.bottom);

    for (i = 0; i < g_nwired; i++)
    {
        int y = list_top() - g_scroll + i * ROW_H;
        RECT t = { 60, y + 12, FLY_W - 16, y + 32 };
        draw_monitor(dc, 16, y + 14, g_wired[i].connected);
        SelectObject(dc, g_font); SetTextColor(dc, COL_TEXT);
        DrawTextW(dc, g_wired[i].name, -1, &t, DT_SINGLELINE | DT_NOPREFIX);
        OffsetRect(&t, 0, 20);
        SelectObject(dc, g_font_small); SetTextColor(dc, COL_SUBTLE);
        DrawTextW(dc, g_wired[i].connected ? L"Connected" : L"Not connected", -1, &t, DT_SINGLELINE | DT_NOPREFIX);
    }
    if (!g_nwired && !g_nnets)
    {
        RECT t = { 60, list_top() + 12, FLY_W - 16, list_top() + 32 };
        draw_monitor(dc, 16, list_top() + 14, FALSE);
        SelectObject(dc, g_font); SetTextColor(dc, COL_TEXT);
        DrawTextW(dc, g_has_wifi && !g_radio_on ? L"Wi-Fi is turned off" : L"Not connected", -1, &t, DT_SINGLELINE);
        OffsetRect(&t, 0, 20);
        SelectObject(dc, g_font_small); SetTextColor(dc, COL_SUBTLE);
        DrawTextW(dc, L"No connections are available", -1, &t, DT_SINGLELINE);
    }
    for (i = 0; i < g_nnets; i++)
    {
        struct wnet *n = &g_nets[i];
        int y = row_y(i);
        RECT row = { 0, y, FLY_W, y + ROW_H + (i == g_exp ? exp_h() : 0) };
        RECT t = { 60, y + 11, FLY_W - 16, y + 31 };
        WCHAR sub[64];
        if (i == g_exp || (i == g_hot && g_hot_part == PART_ROW))
        {
            b = CreateSolidBrush(i == g_exp ? COL_OPEN : COL_HOVER); FillRect(dc, &row, b); DeleteObject(b);
        }
        draw_wifi_glyph(dc, 16, y + 12, n->signal, TRUE);
        if (secured(n))
        {
            RECT ko = { 32, y + 28, 43, y + 41 };
            HBRUSH kb = CreateSolidBrush(i == g_exp ? COL_OPEN : (i == g_hot && g_hot_part == PART_ROW) ? COL_HOVER : COL_BG);
            FillRect(dc, &ko, kb); DeleteObject(kb);
            draw_lock(dc, 34, y + 30);
        }
        SelectObject(dc, g_font); SetTextColor(dc, COL_TEXT);
        DrawTextW(dc, n->name, -1, &t, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        OffsetRect(&t, 0, 20);
        if (n->inuse) _snwprintf(sub, 64, secured(n) ? L"Connected, secured" : L"Connected");
        else _snwprintf(sub, 64, secured(n) ? L"Secured" : L"Open");
        SelectObject(dc, g_font_small); SetTextColor(dc, COL_SUBTLE);
        DrawTextW(dc, sub, -1, &t, DT_SINGLELINE | DT_NOPREFIX);
        if (i == g_exp)
        {
            RECT pr, sr, ck;
            expanded_parts(i, &pr, &sr, &ck);
            if (g_exp_state == EXP_KEY)
            {
                RECT lbl = { 60, y + ROW_H - 4, FLY_W - 16, y + ROW_H + 14 };
                SetTextColor(dc, g_exp_msg[0] ? COL_ERROR : COL_SUBTLE);
                DrawTextW(dc, g_exp_msg[0] ? g_exp_msg : L"Enter the network security key", -1, &lbl,
                          DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
                SelectObject(dc, g_font);
                draw_button(dc, pr, L"Next", TRUE, i == g_hot && g_hot_part == PART_PRIMARY);
                draw_button(dc, sr, L"Cancel", FALSE, i == g_hot && g_hot_part == PART_SECONDARY);
            }
            else if (g_exp_state == EXP_BUSY)
            {
                RECT lbl = { 60, y + ROW_H + 4, FLY_W - 16, y + ROW_H + 24 };
                SetTextColor(dc, COL_SUBTLE);
                DrawTextW(dc, g_exp_msg, -1, &lbl, DT_SINGLELINE | DT_NOPREFIX);
            }
            else
            {
                if (!n->inuse)
                {
                    RECT box = { ck.left, ck.top + 3, ck.left + 16, ck.top + 19 }, lbl = ck;
                    HPEN pen = CreatePen(PS_SOLID, 1, COL_TEXT), op = SelectObject(dc, pen);
                    HBRUSH fill = CreateSolidBrush(g_autoconnect ? COL_ACCENT : COL_BG), ofb = SelectObject(dc, fill);
                    Rectangle(dc, box.left, box.top, box.right, box.bottom);
                    if (g_autoconnect)
                    {
                        MoveToEx(dc, box.left + 3, box.top + 8, NULL); LineTo(dc, box.left + 7, box.top + 12);
                        LineTo(dc, box.left + 13, box.top + 4);
                    }
                    SelectObject(dc, op); SelectObject(dc, ofb); DeleteObject(pen); DeleteObject(fill);
                    lbl.left += 24;
                    SetTextColor(dc, COL_TEXT);
                    DrawTextW(dc, L"Connect automatically", -1, &lbl, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                }
                if (g_exp_state == EXP_ERROR && g_exp_msg[0])
                {
                    RECT lbl = { 60, pr.top - 2, pr.left - 8, pr.bottom + 2 };
                    SetTextColor(dc, COL_ERROR);
                    DrawTextW(dc, g_exp_msg, -1, &lbl, DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
                }
                SelectObject(dc, g_font);
                draw_button(dc, pr, n->inuse ? L"Disconnect" : L"Connect", !n->inuse, i == g_hot && g_hot_part == PART_PRIMARY);
            }
        }
    }
    SelectClipRgn(dc, NULL);

    /* the footer: settings link and the Wi-Fi button */
    r = c; r.top = fly_height() - FOOT_H; r.bottom = r.top + 1;
    b = CreateSolidBrush(COL_LINE); FillRect(dc, &r, b); DeleteObject(b);
    r = settings_rect();
    SelectObject(dc, g_font); SetTextColor(dc, COL_LINK);
    DrawTextW(dc, L"Network & Internet settings", -1, &r, DT_SINGLELINE | DT_NOPREFIX);
    OffsetRect(&r, 0, 20);
    SelectObject(dc, g_font_small); SetTextColor(dc, COL_SUBTLE);
    DrawTextW(dc, L"Change settings, such as making a connection metered.", -1, &r, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (g_has_wifi)
    {
        RECT t = wifi_tile_rect(), l;
        b = CreateSolidBrush(g_radio_on ? COL_ACCENT : COL_BUTTON); FillRect(dc, &t, b); DeleteObject(b);
        draw_wifi_glyph(dc, t.left + 6, t.top + 2, 100, TRUE);
        l = t; l.left += 8; l.top += 38;
        SelectObject(dc, g_font_small); SetTextColor(dc, COL_TEXT);
        DrawTextW(dc, L"Wi-Fi", -1, &l, DT_SINGLELINE | DT_NOPREFIX);
    }
    (void)cn;
    BitBlt(wdc, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static void hit(int x, int y, int *row, int *part)
{
    POINT p = { x, y };
    RECT r = settings_rect(), t = wifi_tile_rect();
    int i, list_h = list_height();
    *row = -1; *part = 0;
    r.bottom += 20;
    if (PtInRect(&r, p)) { *part = PART_SETTINGS; return; }
    if (g_has_wifi && PtInRect(&t, p)) { *part = PART_WIFI_TILE; return; }
    if (list_h > MAX_LIST_H) list_h = MAX_LIST_H;
    if (y < list_top() || y >= list_top() + list_h) return;
    for (i = 0; i < g_nnets; i++)
    {
        int ry = row_y(i);
        if (y >= ry && y < ry + ROW_H + (i == g_exp ? exp_h() : 0))
        {
            *row = i;
            *part = PART_ROW;
            if (i == g_exp && y >= ry + ROW_H)
            {
                RECT pr, sr, ck;
                expanded_parts(i, &pr, &sr, &ck);
                if (PtInRect(&pr, p)) *part = PART_PRIMARY;
                else if (PtInRect(&sr, p)) *part = PART_SECONDARY;
                else if (PtInRect(&ck, p) && g_exp_state != EXP_KEY) *part = PART_CHECK;
                else *part = 0;
            }
            return;
        }
    }
}

static void relayout(void)
{
    if (g_exp >= 0 && g_exp_state == EXP_KEY)
    {
        RECT k = key_rect(g_exp);
        SetWindowPos(g_key, HWND_TOP, k.left, k.top, k.right - k.left, k.bottom - k.top, SWP_SHOWWINDOW);
    }
    else ShowWindow(g_key, SW_HIDE);
    place_flyout();
    InvalidateRect(g_fly, NULL, FALSE);
}

static void start_key_prompt(const WCHAR *msg)
{
    g_exp_state = EXP_KEY;
    _snwprintf(g_exp_msg, 256, L"%ls", msg ? msg : L"");
    SetWindowTextW(g_key, L"");
    relayout();
    SetFocus(g_key);
}

static void end_key_prompt(void)
{
    ShowWindow(g_key, SW_HIDE);
    SetWindowTextW(g_key, L"");
}

static void connect_selected(const char *key)
{
    struct wnet n = g_nets[g_exp];
    WCHAR err[256];
    int rc;
    end_key_prompt();
    g_exp_state = EXP_BUSY;
    _snwprintf(g_exp_msg, 256, secured(&n) ? L"Checking network requirements..." : L"Connecting...");
    relayout();
    UpdateWindow(g_fly);
    g_busy = TRUE;
    rc = do_connect(n.hex, key, g_autoconnect, err, 256, TRUE);
    g_busy = FALSE;
    if (rc == 4)
    {
        refresh(TRUE, FALSE);
        for (g_exp = 0; g_exp < g_nnets && strcmp(g_nets[g_exp].hex, n.hex); g_exp++) ;
        if (g_exp >= g_nnets) { g_exp = -1; relayout(); return; }
        start_key_prompt(err);
        return;
    }
    refresh(TRUE, FALSE);
    if (rc)
    {
        for (g_exp = 0; g_exp < g_nnets && strcmp(g_nets[g_exp].hex, n.hex); g_exp++) ;
        if (g_exp >= g_nnets) g_exp = -1;
        g_exp_state = EXP_ERROR;
        _snwprintf(g_exp_msg, 256, L"%ls", err);
    }
    else g_exp_state = EXP_IDLE;
    tray_update(FALSE);
    relayout();
}

static void primary_action(void)
{
    struct wnet *n;
    if (g_exp < 0 || g_busy) return;
    n = &g_nets[g_exp];
    if (g_exp_state == EXP_KEY)
    {
        WCHAR key[128];
        char *k;
        GetWindowTextW(g_key, key, 128);
        k = w_to_utf8(key);
        SecureZeroMemory(key, sizeof(key));
        connect_selected(k);
        if (k) { SecureZeroMemory(k, strlen(k)); free(k); }
        return;
    }
    if (n->inuse)
    {
        const char *argv[] = { "wifi", "disconnect", g_wifi_dev };
        struct net_reply r;
        g_busy = TRUE;
        net_request_pumped(3, argv, NULL, &r);
        g_busy = FALSE;
        net_reply_free(&r);
        refresh(TRUE, FALSE);
        tray_update(FALSE);
        relayout();
        return;
    }
    if (secured(n) && !n->saved) { start_key_prompt(NULL); return; }
    connect_selected(NULL);
}

static void open_settings(void)
{
    WCHAR self[MAX_PATH], *slash;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    if ((slash = wcsrchr(self, '\\'))) wcscpy(slash + 1, L"sg-ncpa64.exe");
    if (CreateProcessW(self, NULL, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    ShowWindow(g_fly, SW_HIDE);
}

static void show_flyout(void)
{
    load_mode();
    g_exp = -1;
    g_exp_state = EXP_IDLE;
    g_scroll = 0;
    end_key_prompt();
    refresh(TRUE, FALSE);
    place_flyout();
    ShowWindow(g_fly, SW_SHOW);
    SetForegroundWindow(g_fly);
    InvalidateRect(g_fly, NULL, FALSE);
    /* then look again, properly: a rescan takes a few seconds */
    if (g_has_wifi && !g_busy)
    {
        g_busy = TRUE;
        refresh(TRUE, TRUE);
        g_busy = FALSE;
        tray_update(FALSE);
        relayout();
    }
}

static LRESULT CALLBACK fly_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT: on_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && !g_busy && GetParent((HWND)lp) != hwnd && (HWND)lp != g_key) ShowWindow(hwnd, SW_HIDE);
        return 0;
    case WM_CTLCOLOREDIT:
    {
        static HBRUSH b;
        static COLORREF made = CLR_INVALID;
        if (made != g_pal->field) { if (b) DeleteObject(b); b = CreateSolidBrush(made = g_pal->field); }
        SetTextColor((HDC)wp, COL_TEXT);
        SetBkColor((HDC)wp, g_pal->field);
        return (LRESULT)b;
    }
    case WM_MOUSEMOVE:
    {
        int row, part;
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        hit((short)LOWORD(lp), (short)HIWORD(lp), &row, &part);
        if (row != g_hot || part != g_hot_part) { g_hot = row; g_hot_part = part; InvalidateRect(hwnd, NULL, FALSE); }
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: g_hot = -1; g_hot_part = 0; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_MOUSEWHEEL:
    {
        int max = list_height() - MAX_LIST_H;
        if (max <= 0) return 0;
        g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / 2;
        if (g_scroll < 0) g_scroll = 0;
        if (g_scroll > max) g_scroll = max;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        int row, part;
        if (g_busy) return 0;
        hit((short)LOWORD(lp), (short)HIWORD(lp), &row, &part);
        if (part == PART_SETTINGS) open_settings();
        else if (part == PART_WIFI_TILE)
        {
            const char *argv[] = { "wifi", "radio", g_radio_on ? "off" : "on" };
            struct net_reply r;
            g_busy = TRUE;
            net_request_pumped(3, argv, NULL, &r);
            g_busy = FALSE;
            net_reply_free(&r);
            refresh(TRUE, FALSE);
            tray_update(FALSE);
            relayout();
        }
        else if (part == PART_ROW && row != g_exp)
        {
            end_key_prompt();
            g_exp = row;
            g_exp_state = EXP_IDLE;
            g_exp_msg[0] = 0;
            g_autoconnect = TRUE;
            relayout();
        }
        else if (part == PART_CHECK) { g_autoconnect = !g_autoconnect; InvalidateRect(hwnd, NULL, FALSE); }
        else if (part == PART_PRIMARY) primary_action();
        else if (part == PART_SECONDARY) { end_key_prompt(); g_exp_state = EXP_IDLE; g_exp_msg[0] = 0; relayout(); }
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbar_created && g_taskbar_created) { tray_update(TRUE); return 0; }
    if (sg_mode_changed(msg, lp))
    {
        /* the Windows mode may have changed: the icon and the flyout follow */
        load_mode();
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
            else show_flyout();
        }
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
        {
            HMENU m = CreatePopupMenu();
            POINT p;
            int cmd;
            AppendMenuW(m, MF_STRING, 1, L"Open Network && Internet settings");
            GetCursorPos(&p);
            SetForegroundWindow(hwnd);
            cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, hwnd, NULL);
            DestroyMenu(m);
            if (cmd == 1) open_settings();
        }
        return 0;
    case WM_TIMER:
        if (!g_busy && !IsWindowVisible(g_fly))
        {
            g_busy = TRUE;
            refresh(TRUE, FALSE);
            g_busy = FALSE;
            tray_update(FALSE);
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --- non-interactive paths, for the gates -------------------------------------------------- */

static int dump(void)
{
    int i, cn;
    refresh(FALSE, TRUE);
    for (i = 0; i < g_nwired; i++)
    {
        char *n = w_to_utf8(g_wired[i].name);
        net_report("WIRED %s|%s\n", n ? n : "", g_wired[i].connected ? "connected" : "not connected");
        free(n);
    }
    net_report("WIFI-ADAPTER %s\nRADIO %s\n", g_has_wifi ? g_wifi_dev : "none", g_radio_on ? "on" : "off");
    for (i = 0; i < g_nnets; i++)
    {
        char *n = w_to_utf8(g_nets[i].name);
        net_report("NET %s|%s|%d|%s|%s|%s\n", g_nets[i].hex, n ? n : "", g_nets[i].signal, g_nets[i].security,
                   g_nets[i].inuse ? (secured(&g_nets[i]) ? "Connected, secured" : "Connected")
                                   : (secured(&g_nets[i]) ? "Secured" : "Open"),
                   g_nets[i].saved ? "saved" : "new");
        free(n);
    }
    cn = connected_net();
    tray_update(FALSE);
    {
        char *tip = w_to_utf8(g_nid.szTip);
        for (i = 0; tip && tip[i]; i++) if (tip[i] == '\n') tip[i] = '/';
        net_report("TIP %s\n", tip ? tip : "");
        free(tip);
    }
    (void)cn;
    return 0;
}

static int connect_cli(int argc, WCHAR **argv, int i)
{
    char hex[72], key[256] = "";
    BOOL autoconnect = TRUE, have_key = FALSE;
    WCHAR err[256];
    int k, rc;
    if (i >= argc) { net_report("RESULT ERROR usage\n"); return 2; }
    WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, hex, sizeof(hex), NULL, NULL);
    for (k = i + 1; k < argc; k++)
    {
        if (!wcscmp(argv[k], L"--no-autoconnect")) autoconnect = FALSE;
        else if (!wcscmp(argv[k], L"--password-file") && k + 1 < argc)
        {
            HANDLE f = CreateFileW(argv[++k], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            DWORD got = 0;
            if (f == INVALID_HANDLE_VALUE) { net_report("RESULT ERROR cannot read the key file\n"); return 2; }
            ReadFile(f, key, sizeof(key) - 1, &got, NULL);
            CloseHandle(f);
            key[got] = 0;
            key[strcspn(key, "\r\n")] = 0;
            have_key = TRUE;
        }
    }
    rc = do_connect(hex, have_key ? key : NULL, autoconnect, err, 256, FALSE);
    SecureZeroMemory(key, sizeof(key));
    if (rc) { char *e = w_to_utf8(err); net_report("RESULT ERROR %d %s\n", rc, e ? e : ""); free(e); }
    else net_report("RESULT OK\n");
    return rc;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, WCHAR *cmdline, int show)
{
    WNDCLASSW wc = { 0 };
    WCHAR **argv;
    int argc, i;
    BOOL open = FALSE, prompt = FALSE;
    char expand[72] = "";
    MSG msg;
    (void)prev; (void)cmdline; (void)show;

    load_mode();

    if (!net_init(GetCommandLineW()))
    {
        /* One network icon per session. */
        if (FindWindowW(L"SgNetworkTray", NULL) && !wcsstr(GetCommandLineW(), L"--")) return 0;
        net_relaunch(net_args_after_program(GetCommandLineW()));
        return 0;
    }
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; i < argc; i++)
    {
        if (!wcscmp(argv[i], L"--dump")) return dump();
        if (!wcscmp(argv[i], L"--connect")) return connect_cli(argc, argv, i + 1);
        if (!wcscmp(argv[i], L"--open")) open = TRUE;
        if (!wcscmp(argv[i], L"--prompt")) prompt = TRUE;
        if (!wcscmp(argv[i], L"--expand") && i + 1 < argc)
            WideCharToMultiByte(CP_UTF8, 0, argv[++i], -1, expand, sizeof(expand), NULL, NULL);
    }

    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_small = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_title = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    wc.lpfnWndProc = tray_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SgNetworkTray";
    RegisterClassW(&wc);
    g_tray_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Network", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);

    wc.lpfnWndProc = fly_proc;
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.lpszClassName = L"SgNetworkFlyout";
    RegisterClassW(&wc);
    /* Owned by the (never shown) tray window: no taskbar button of its own. */
    g_fly = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Network", WS_POPUP,
                            0, 0, FLY_W, 300, g_tray_wnd, NULL, inst, NULL);
    g_key = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL, 60, 0, FLY_W - 76, 22,
                            g_fly, (HMENU)(INT_PTR)ID_KEY, inst, NULL);
    SendMessageW(g_key, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_key, EM_LIMITTEXT, 127, 0);

    refresh(FALSE, FALSE);
    tray_update(TRUE);
    SetTimer(g_tray_wnd, 1, 15000, NULL);
    if (open)
    {
        show_flyout();
        for (i = 0; expand[0] && i < g_nnets; i++)
            if (!strcmp(g_nets[i].hex, expand))
            {
                g_exp = i; g_exp_state = EXP_IDLE;
                relayout();
                if (prompt) start_key_prompt(NULL);
            }
    }
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        if (msg.hwnd == g_key && msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) { primary_action(); continue; }
        if (msg.hwnd == g_key && msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE)
        {
            end_key_prompt(); g_exp_state = EXP_IDLE; relayout(); continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
