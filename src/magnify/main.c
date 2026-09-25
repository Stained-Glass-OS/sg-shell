/* sg-magnify -- Magnifier (magnify.exe; Win+Plus, Win+Minus, Win+Esc).
 *
 * Windows 10's Magnifier, our own code: a small toolbar (zoom out, the zoom
 * level, zoom in, Views, Settings, Help) and one of three views:
 *
 *   Full screen (Ctrl+Alt+F)  the whole screen magnified, following the
 *                             pointer, the keyboard focus or the text cursor
 *   Lens        (Ctrl+Alt+L)  a rectangle around the pointer, magnified
 *   Docked      (Ctrl+Alt+D)  a band across the top of the screen (an AppBar,
 *                             so windows make room) showing the area around
 *                             the pointer
 *
 * Ctrl+Alt+wheel zooms, Ctrl+Alt+I inverts colours, Ctrl+Alt+Minus/Plus
 * zoom too; explorer's Win+Plus starts Magnifier (or zooms in), Win+Minus
 * zooms out and Win+Esc closes it (wine-sg 0182: it posts IDM_ZOOMIN,
 * IDM_ZOOMOUT or WM_CLOSE to our window, class SgMagnifier).
 *
 * WHERE THE PICTURE COMES FROM. The docked view shows a part of the screen it
 * never covers, so it copies from the screen DC. The lens and the full-screen
 * view cover what they magnify: copying the screen would copy the view itself
 * and feed back (each frame the previous frame magnified again). They build
 * the picture themselves instead -- the desktop, then every other process's
 * visible top-level window, bottom to top, each from its own surface with
 * PrintWindow(PW_RENDERFULLCONTENT) (wine-sg 0076 makes that work across
 * processes) -- so our own windows are never in it. A window whose program
 * is not answering keeps its last picture.
 *
 * The full-screen view is click-through (WS_EX_LAYERED | WS_EX_TRANSPARENT):
 * the pointer and the clicks stay where they are, and the view is placed so
 * that the point under the pointer is shown under the pointer -- the origin
 * of what is shown is p * (1 - 1/zoom) -- so what you click is what you see.
 *
 * Settings: HKCU\Software\Microsoft\ScreenMagnifier (Magnification, the
 * percentage; ZoomIncrement; MagnificationMode 1 full screen, 2 docked,
 * 3 lens; Invert; FollowMouse, FollowFocus, FollowCaret; LensWidth and
 * LensHeight, percentages of the screen; DockedHeight, a percentage).
 *
 * Command line: /fullscreen, /lens, /docked, /zoomin, /zoomout, /close,
 * /zoom:NNN, /reload (read the settings again). One instance: another
 * hands its command line over (WM_COPYDATA).
 *
 * SG_MAGNIFY_DUMP=<file> (a Windows path) is rewritten as the state changes,
 * for the gate (test/magnify-check.sh).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define CLASS_BAR  L"SgMagnifier"
#define CLASS_VIEW L"SgMagnifierView"
#define KEY L"Software\\Microsoft\\ScreenMagnifier"

enum { MODE_FULL = 1, MODE_DOCKED = 2, MODE_LENS = 3 };
enum { IDM_ZOOMIN = 0x101, IDM_ZOOMOUT, IDM_FULL, IDM_DOCKED, IDM_LENS, IDM_INVERT, IDM_SETTINGS, IDM_HELP,
       IDM_FOLLOW_MOUSE, IDM_FOLLOW_FOCUS, IDM_FOLLOW_CARET };
enum { HK_FULL = 1, HK_LENS, HK_DOCKED, HK_INVERT, HK_IN, HK_OUT, HK_WIN_IN, HK_WIN_IN2, HK_WIN_OUT, HK_WIN_OUT2, HK_WIN_ESC };
#define TIMER_FRAME 1
#define WM_APPBAR (WM_APP + 1)

static HINSTANCE g_inst;
static HWND g_bar, g_view;
static int g_mode = MODE_FULL, g_zoom = 200, g_inc = 100, g_invert;
static int g_follow_mouse = 1, g_follow_focus = 1, g_follow_caret = 1;
static int g_lens_w = 40, g_lens_h = 30, g_dock_h = 25;
static int g_sw, g_sh;                          /* the screen */
static POINT g_center;                          /* what the view follows */
static POINT g_last_mouse = { -1, -1 };
static RECT g_last_caret; static HWND g_last_focus;
static RECT g_src, g_viewrc;                    /* the source rectangle and where it is shown */
static BOOL g_docked_appbar;
static HHOOK g_mouse_hook;
static DWORD g_frames;
static WCHAR g_dump[MAX_PATH];
static HFONT g_font, g_font_big;
static int g_hot = -1;                          /* the toolbar button under the mouse */

/* ---- settings ------------------------------------------------------------------------------ */
static DWORD reg_get(const WCHAR *name, DWORD def)
{
    DWORD v = def, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, KEY, name, RRF_RT_REG_DWORD, NULL, &v, &size)) return def;
    return v;
}

static void reg_put(const WCHAR *name, DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    RegSetValueExW(k, name, 0, REG_DWORD, (BYTE *)&v, sizeof(v));
    RegCloseKey(k);
}

static void load_settings(void)
{
    g_zoom = reg_get(L"Magnification", 200);
    g_inc = reg_get(L"ZoomIncrement", 100);
    g_mode = reg_get(L"MagnificationMode", MODE_FULL);
    g_invert = reg_get(L"Invert", 0) != 0;
    g_follow_mouse = reg_get(L"FollowMouse", 1) != 0;
    g_follow_focus = reg_get(L"FollowFocus", 1) != 0;
    g_follow_caret = reg_get(L"FollowCaret", 1) != 0;
    g_lens_w = reg_get(L"LensWidth", 40);
    g_lens_h = reg_get(L"LensHeight", 30);
    g_dock_h = reg_get(L"DockedHeight", 25);
    if (g_mode < MODE_FULL || g_mode > MODE_LENS) g_mode = MODE_FULL;
    if (g_zoom < 100) g_zoom = 100;
    if (g_zoom > 1600) g_zoom = 1600;
    if (g_inc != 25 && g_inc != 50 && g_inc != 100 && g_inc != 150 && g_inc != 200 && g_inc != 400) g_inc = 100;
    if (g_lens_w < 10 || g_lens_w > 100) g_lens_w = 40;
    if (g_lens_h < 10 || g_lens_h > 100) g_lens_h = 30;
    if (g_dock_h < 10 || g_dock_h > 50) g_dock_h = 25;
}

/* ---- the dump ------------------------------------------------------------------------------ */
static const WCHAR *mode_name(int m)
{
    return m == MODE_LENS ? L"lens" : m == MODE_DOCKED ? L"docked" : L"full";
}

static RECT g_btn[6];   /* toolbar buttons: minus, level, plus, views, settings, help */
static const WCHAR *const btn_names[6] = { L"minus", L"level", L"plus", L"views", L"settings", L"help" };

static void write_dump(void)
{
    FILE *f;
    RECT br;
    POINT pt;
    int i;
    if (!g_dump[0]) return;
    WCHAR tmp[MAX_PATH + 8];
    _snwprintf(tmp, ARRAYSIZE(tmp), L"%ls.tmp", g_dump);
    tmp[ARRAYSIZE(tmp) - 1] = 0;
    /* written whole, then renamed into place: a reader never sees half of it */
    if (!(f = _wfopen(tmp, L"w"))) return;
    fwprintf(f, L"MODE %ls\nZOOM %d\nINCREMENT %d\nINVERT %d\n", mode_name(g_mode), g_zoom, g_inc, g_invert);
    fwprintf(f, L"SCREEN %d %d\n", g_sw, g_sh);
    fwprintf(f, L"VIEW %ld %ld %ld %ld\n", g_viewrc.left, g_viewrc.top, g_viewrc.right, g_viewrc.bottom);
    fwprintf(f, L"SOURCE %ld %ld %ld %ld\n", g_src.left, g_src.top, g_src.right, g_src.bottom);
    fwprintf(f, L"CENTER %ld %ld\n", g_center.x, g_center.y);
    GetCursorPos(&pt);
    fwprintf(f, L"CURSOR %ld %ld\n", pt.x, pt.y);
    if (g_view) fwprintf(f, L"VIEWSTYLE %08lx\n", (unsigned long)GetWindowLongW(g_view, GWL_EXSTYLE));
    if (g_bar)
    {
        GetWindowRect(g_bar, &br);
        fwprintf(f, L"TOOLBAR %ld %ld %ld %ld\n", br.left, br.top, br.right, br.bottom);
        for (i = 0; i < 6; i++)
        {
            POINT c = { (g_btn[i].left + g_btn[i].right) / 2, (g_btn[i].top + g_btn[i].bottom) / 2 };
            ClientToScreen(g_bar, &c);
            fwprintf(f, L"BUTTON %ls %ld %ld\n", btn_names[i], c.x, c.y);
        }
    }
    fwprintf(f, L"FRAMES %lu\nEND\n", (unsigned long)g_frames);
    fclose(f);
    MoveFileExW(tmp, g_dump, MOVEFILE_REPLACE_EXISTING);
}

/* ---- composing the screen without ourselves ---------------------------------------------- */
struct wincache { HWND hwnd; int w, h; HDC dc; HBITMAP bmp, old; BOOL seen; };
static struct wincache g_cache[96];
static int g_ncache;

static struct wincache *cache_for(HWND hwnd, int w, int h)
{
    struct wincache *c = NULL;
    int i;
    for (i = 0; i < g_ncache; i++) if (g_cache[i].hwnd == hwnd) { c = &g_cache[i]; break; }
    if (c && (c->w != w || c->h != h))
    {
        SelectObject(c->dc, c->old); DeleteObject(c->bmp); DeleteDC(c->dc);
        c->dc = NULL;
    }
    if (!c)
    {
        if (g_ncache == ARRAYSIZE(g_cache)) return NULL;
        c = &g_cache[g_ncache++];
        memset(c, 0, sizeof(*c));
        c->hwnd = hwnd;
    }
    if (!c->dc)
    {
        HDC screen = GetDC(NULL);
        c->dc = CreateCompatibleDC(screen);
        c->bmp = CreateCompatibleBitmap(screen, w, h);
        c->old = SelectObject(c->dc, c->bmp);
        ReleaseDC(NULL, screen);
        c->w = w; c->h = h;
        PatBlt(c->dc, 0, 0, w, h, BLACKNESS);
    }
    c->seen = TRUE;
    return c;
}

static void cache_sweep(void)
{
    int i;
    for (i = 0; i < g_ncache; )
    {
        if (!g_cache[i].seen)
        {
            SelectObject(g_cache[i].dc, g_cache[i].old); DeleteObject(g_cache[i].bmp); DeleteDC(g_cache[i].dc);
            g_cache[i] = g_cache[--g_ncache];
            continue;
        }
        g_cache[i++].seen = FALSE;
    }
}

struct winlist { HWND h[512]; int n; };

static BOOL CALLBACK collect(HWND hwnd, LPARAM lp)
{
    struct winlist *l = (struct winlist *)lp;
    if (l->n < (int)ARRAYSIZE(l->h)) l->h[l->n++] = hwnd;
    return TRUE;
}

static HDC g_cdc; static HBITMAP g_cbmp, g_cold; static int g_cw, g_ch;

static void ensure_compose(int w, int h)
{
    HDC screen;
    if (g_cdc && g_cw >= w && g_ch >= h) return;
    if (g_cdc) { SelectObject(g_cdc, g_cold); DeleteObject(g_cbmp); DeleteDC(g_cdc); }
    screen = GetDC(NULL);
    g_cdc = CreateCompatibleDC(screen);
    g_cw = max(w, g_cw); g_ch = max(h, g_ch);
    g_cbmp = CreateCompatibleBitmap(screen, g_cw, g_ch);
    g_cold = SelectObject(g_cdc, g_cbmp);
    ReleaseDC(NULL, screen);
}

/* the picture of src (screen coordinates) into g_cdc at 0,0 */
static void capture(const RECT *src, BOOL compose)
{
    int w = src->right - src->left, h = src->bottom - src->top, i;
    struct winlist *l;
    struct wincache *c;
    DWORD self = GetCurrentProcessId();

    ensure_compose(w, h);
#ifdef SG_MUTANT_FEEDBACK
    compose = FALSE;
#endif
    if (!compose)
    {
        HDC screen = GetDC(NULL);
        BitBlt(g_cdc, 0, 0, w, h, screen, src->left, src->top, SRCCOPY);
        ReleaseDC(NULL, screen);
        return;
    }
    /* the desktop (its colour, wallpaper and icons, from its own surface), then the windows over it */
    {
        HWND desk = GetDesktopWindow();
        RECT dr;
        GetWindowRect(desk, &dr);
        if ((c = cache_for(desk, dr.right - dr.left, dr.bottom - dr.top)) && PrintWindow(desk, c->dc, PW_RENDERFULLCONTENT))
            BitBlt(g_cdc, 0, 0, w, h, c->dc, src->left - dr.left, src->top - dr.top, SRCCOPY);
        else
        {
            RECT r = { 0, 0, w, h };
            FillRect(g_cdc, &r, GetSysColorBrush(COLOR_DESKTOP));
        }
    }
    if (!(l = HeapAlloc(GetProcessHeap(), 0, sizeof(*l)))) return;
    l->n = 0;
    EnumWindows(collect, (LPARAM)l);
    for (i = l->n - 1; i >= 0; i--)            /* bottom to top */
    {
        HWND hwnd = l->h[i];
        RECT wr, is;
        DWORD pid = 0, cloaked = 0;
        BYTE alpha = 255; COLORREF key = 0; DWORD lwa = 0;
        LONG ex;

        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) continue;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == self) continue;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) continue;
        GetWindowRect(hwnd, &wr);
        if (wr.right <= wr.left || wr.bottom <= wr.top) continue;
        if (!IntersectRect(&is, &wr, src)) continue;
        if (!(c = cache_for(hwnd, wr.right - wr.left, wr.bottom - wr.top))) continue;
        if (!IsHungAppWindow(hwnd))
            PrintWindow(hwnd, c->dc, PW_RENDERFULLCONTENT);
        ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_LAYERED) GetLayeredWindowAttributes(hwnd, &key, &alpha, &lwa);
        if ((lwa & LWA_ALPHA) && alpha < 255)
        {
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, 0 };
            GdiAlphaBlend(g_cdc, is.left - src->left, is.top - src->top, is.right - is.left, is.bottom - is.top,
                          c->dc, is.left - wr.left, is.top - wr.top, is.right - is.left, is.bottom - is.top, bf);
        }
        else if (lwa & LWA_COLORKEY)
            GdiTransparentBlt(g_cdc, is.left - src->left, is.top - src->top, is.right - is.left, is.bottom - is.top,
                              c->dc, is.left - wr.left, is.top - wr.top, is.right - is.left, is.bottom - is.top, key);
        else
            BitBlt(g_cdc, is.left - src->left, is.top - src->top, is.right - is.left, is.bottom - is.top,
                   c->dc, is.left - wr.left, is.top - wr.top, SRCCOPY);
    }
    HeapFree(GetProcessHeap(), 0, l);
    cache_sweep();
}

/* ---- geometry ----------------------------------------------------------------------------- */
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int scaled(int n) { return MulDiv(n, 100, g_zoom); }  /* screen pixels shown in n view pixels */

static void compute_rects(void)
{
    POINT p = g_center;
    int sw, sh;
    switch (g_mode)
    {
    case MODE_FULL:
        g_viewrc.left = 0; g_viewrc.top = 0; g_viewrc.right = g_sw; g_viewrc.bottom = g_sh;
        sw = scaled(g_sw); sh = scaled(g_sh);
        /* the point under the pointer stays under the pointer: origin = p (1 - 1/zoom) */
        g_src.left = clampi(p.x - MulDiv(p.x, 100, g_zoom), 0, g_sw - sw);
        g_src.top = clampi(p.y - MulDiv(p.y, 100, g_zoom), 0, g_sh - sh);
        break;
    case MODE_LENS:
    {
        int lw = MulDiv(g_sw, g_lens_w, 100), lh = MulDiv(g_sh, g_lens_h, 100);
        g_viewrc.left = clampi(p.x - lw / 2, 0, g_sw - lw);
        g_viewrc.top = clampi(p.y - lh / 2, 0, g_sh - lh);
        g_viewrc.right = g_viewrc.left + lw; g_viewrc.bottom = g_viewrc.top + lh;
        sw = scaled(lw); sh = scaled(lh);
        g_src.left = clampi(p.x - sw / 2, 0, g_sw - sw);
        g_src.top = clampi(p.y - sh / 2, 0, g_sh - sh);
        break;
    }
    default: /* docked */
    {
        int dh = MulDiv(g_sh, g_dock_h, 100);
        g_viewrc.left = 0; g_viewrc.top = 0; g_viewrc.right = g_sw; g_viewrc.bottom = dh;
        sw = scaled(g_sw); sh = scaled(dh);
        if (sh > g_sh - dh) sh = g_sh - dh;
        g_src.left = clampi(p.x - sw / 2, 0, g_sw - sw);
        g_src.top = clampi(p.y - sh / 2, dh, g_sh - sh);
        break;
    }
    }
    g_src.right = g_src.left + sw; g_src.bottom = g_src.top + sh;
}

/* what to follow: the pointer when it moved, else the text cursor or the focus when they did */
static void track(void)
{
    POINT pt;
    GUITHREADINFO gti = { sizeof(gti) };
    GetCursorPos(&pt);
    if (pt.x != g_last_mouse.x || pt.y != g_last_mouse.y)
    {
        g_last_mouse = pt;
        if (g_follow_mouse)
        {
            /* in the docked view, the pointer over the dock itself moves nothing */
            if (!(g_mode == MODE_DOCKED && pt.y < g_viewrc.bottom && g_viewrc.bottom > 0)) g_center = pt;
        }
        return;
    }
    if (!GetGUIThreadInfo(0, &gti)) return;
    if (g_follow_caret && gti.hwndCaret)
    {
        RECT r = gti.rcCaret;
        MapWindowPoints(gti.hwndCaret, NULL, (POINT *)&r, 2);
        if (!EqualRect(&r, &g_last_caret))
        {
            g_last_caret = r;
            g_center.x = r.left; g_center.y = (r.top + r.bottom) / 2;
            return;
        }
    }
    if (g_follow_focus && gti.hwndFocus && gti.hwndFocus != g_last_focus)
    {
        RECT r;
        DWORD pid = 0;
        g_last_focus = gti.hwndFocus;
        GetWindowThreadProcessId(gti.hwndFocus, &pid);
        if (pid != GetCurrentProcessId() && GetWindowRect(gti.hwndFocus, &r))
        {
            g_center.x = (r.left + r.right) / 2; g_center.y = (r.top + r.bottom) / 2;
        }
    }
}

/* ---- the view ----------------------------------------------------------------------------- */
static void appbar(BOOL on)
{
    APPBARDATA abd = { sizeof(abd) };
    abd.hWnd = g_view;
    if (on && !g_docked_appbar)
    {
        abd.uCallbackMessage = WM_APPBAR;
        SHAppBarMessage(ABM_NEW, &abd);
        abd.uEdge = ABE_TOP;
        SetRect(&abd.rc, 0, 0, g_sw, MulDiv(g_sh, g_dock_h, 100));
        SHAppBarMessage(ABM_QUERYPOS, &abd);
        abd.rc.bottom = abd.rc.top + MulDiv(g_sh, g_dock_h, 100);
        SHAppBarMessage(ABM_SETPOS, &abd);
        g_docked_appbar = TRUE;
    }
    else if (!on && g_docked_appbar)
    {
        SHAppBarMessage(ABM_REMOVE, &abd);
        g_docked_appbar = FALSE;
    }
}

static void place_view(void)
{
    static RECT last;
    static unsigned ticks;
    LONG ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    if (!g_view) return;
    /* full screen and the lens let the pointer through to what is under them */
    if (g_mode != MODE_DOCKED) ex |= WS_EX_LAYERED | WS_EX_TRANSPARENT;
    if ((LONG)GetWindowLongW(g_view, GWL_EXSTYLE) != ex)
    {
        SetWindowLongW(g_view, GWL_EXSTYLE, ex);
        if (ex & WS_EX_LAYERED) SetLayeredWindowAttributes(g_view, 0, 255, LWA_ALPHA);
    }
    appbar(g_mode == MODE_DOCKED);
    compute_rects();
    if (g_mode == MODE_FULL && g_zoom <= 100)
    {
        ShowWindow(g_view, SW_HIDE);
        SetRectEmpty(&last);
        return;
    }
    /* moved, or twice a second: another topmost window (the taskbar) may have come up over us */
    if (!EqualRect(&last, &g_viewrc) || !IsWindowVisible(g_view) || ++ticks % 15 == 0)
    {
        last = g_viewrc;
        SetWindowPos(g_view, HWND_TOPMOST, g_viewrc.left, g_viewrc.top, g_viewrc.right - g_viewrc.left,
                     g_viewrc.bottom - g_viewrc.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        /* the toolbar stays above the view */
        if (g_bar) SetWindowPos(g_bar, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    }
}

static void draw_cursor(HDC dc)
{
    CURSORINFO ci = { sizeof(ci) };
    ICONINFO ii;
    int cx, cy, x, y;
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;
    if (!PtInRect(&g_src, ci.ptScreenPos)) return;
    if (!GetIconInfo(ci.hCursor, &ii)) return;
    cx = MulDiv(GetSystemMetrics(SM_CXCURSOR), g_zoom, 100);
    cy = MulDiv(GetSystemMetrics(SM_CYCURSOR), g_zoom, 100);
    x = MulDiv(ci.ptScreenPos.x - g_src.left, g_zoom, 100) - MulDiv(ii.xHotspot, g_zoom, 100);
    y = MulDiv(ci.ptScreenPos.y - g_src.top, g_zoom, 100) - MulDiv(ii.yHotspot, g_zoom, 100);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    DrawIconEx(dc, x, y, ci.hCursor, cx, cy, 0, NULL, DI_NORMAL);
}

static void paint_view(HDC dc)
{
    int sw = g_src.right - g_src.left, sh = g_src.bottom - g_src.top;
    int dw = MulDiv(sw, g_zoom, 100), dh = MulDiv(sh, g_zoom, 100);
    RECT cr;
#ifdef SG_MUTANT_SCALE
    dw = MulDiv(sw, g_zoom + 50, 100); dh = MulDiv(sh, g_zoom + 50, 100);
#endif
    GetClientRect(g_view, &cr);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchBlt(dc, 0, 0, dw, dh, g_cdc, 0, 0, sw, sh, SRCCOPY);
    draw_cursor(dc);
    if (g_invert) PatBlt(dc, 0, 0, cr.right, cr.bottom, DSTINVERT);
    if (g_mode != MODE_FULL)
    {
        /* a frame in the accent colour, as Windows draws round the lens and under the dock */
        HBRUSH b = CreateSolidBrush(RGB(112, 48, 192));
        if (g_mode == MODE_LENS)
            FrameRect(dc, &cr, b), InflateRect(&cr, -1, -1), FrameRect(dc, &cr, b);
        else
        {
            RECT r = { 0, cr.bottom - 3, cr.right, cr.bottom };
            FillRect(dc, &r, b);
        }
        DeleteObject(b);
    }
}

static void frame(void)
{
    track();
    place_view();
    if (!IsWindowVisible(g_view)) { write_dump(); return; }
    capture(&g_src, g_mode != MODE_DOCKED);
    {
        HDC dc = GetDC(g_view);
        paint_view(dc);
        ReleaseDC(g_view, dc);
    }
    g_frames++;
    write_dump();
}

static LRESULT CALLBACK view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return g_mode == MODE_DOCKED ? HTCLIENT : HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_cdc) paint_view(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_APPBAR:
        if (wp == ABN_POSCHANGED && g_docked_appbar) { appbar(FALSE); appbar(TRUE); }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- zoom and views ----------------------------------------------------------------------- */
static void set_zoom(int z)
{
    g_zoom = clampi(z, 100, 1600);
    reg_put(L"Magnification", g_zoom);
    if (g_bar) InvalidateRect(g_bar, NULL, TRUE);
    frame();
}

static void set_mode(int m)
{
    g_mode = m;
    reg_put(L"MagnificationMode", m);
    /* a view that starts at 100% would show nothing: start at one step */
    if (g_zoom <= 100) g_zoom = 100 + g_inc;
    if (g_bar) InvalidateRect(g_bar, NULL, TRUE);
    frame();
}

static void do_command(int id)
{
    switch (id)
    {
    case IDM_ZOOMIN: set_zoom(g_zoom + g_inc); break;
    case IDM_ZOOMOUT: set_zoom(g_zoom - g_inc); break;
    case IDM_FULL: set_mode(MODE_FULL); break;
    case IDM_DOCKED: set_mode(MODE_DOCKED); break;
    case IDM_LENS: set_mode(MODE_LENS); break;
    case IDM_INVERT: g_invert = !g_invert; reg_put(L"Invert", g_invert); frame(); break;
    case IDM_FOLLOW_MOUSE: g_follow_mouse = !g_follow_mouse; reg_put(L"FollowMouse", g_follow_mouse); break;
    case IDM_FOLLOW_FOCUS: g_follow_focus = !g_follow_focus; reg_put(L"FollowFocus", g_follow_focus); break;
    case IDM_FOLLOW_CARET: g_follow_caret = !g_follow_caret; reg_put(L"FollowCaret", g_follow_caret); break;
    case IDM_SETTINGS: ShellExecuteW(NULL, NULL, L"ms-settings:easeofaccess-magnifier", NULL, NULL, SW_SHOWNORMAL); break;
    case IDM_HELP:
        MessageBoxW(g_bar,
            L"Magnifier makes part or all of the screen bigger.\n\n"
            L"Windows logo key + Plus sign\tZoom in (starts Magnifier)\n"
            L"Windows logo key + Minus sign\tZoom out\n"
            L"Windows logo key + Esc\tClose Magnifier\n"
            L"Ctrl + Alt + mouse wheel\tZoom in or out\n"
            L"Ctrl + Alt + F\tFull screen view\n"
            L"Ctrl + Alt + L\tLens view\n"
            L"Ctrl + Alt + D\tDocked view\n"
            L"Ctrl + Alt + I\tInvert colours",
            L"Magnifier", MB_OK | MB_ICONINFORMATION);
        break;
    }
    write_dump();
}

static void views_menu(void)
{
    HMENU m = CreatePopupMenu();
    POINT p = { g_btn[3].left, g_btn[3].bottom };
    AppendMenuW(m, MF_STRING | (g_mode == MODE_FULL ? MF_CHECKED : 0), IDM_FULL, L"Full screen\tCtrl+Alt+F");
    AppendMenuW(m, MF_STRING | (g_mode == MODE_DOCKED ? MF_CHECKED : 0), IDM_DOCKED, L"Docked\tCtrl+Alt+D");
    AppendMenuW(m, MF_STRING | (g_mode == MODE_LENS ? MF_CHECKED : 0), IDM_LENS, L"Lens\tCtrl+Alt+L");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_invert ? MF_CHECKED : 0), IDM_INVERT, L"Invert colours\tCtrl+Alt+I");
    AppendMenuW(m, MF_STRING | (g_follow_mouse ? MF_CHECKED : 0), IDM_FOLLOW_MOUSE, L"Follow the mouse pointer");
    AppendMenuW(m, MF_STRING | (g_follow_focus ? MF_CHECKED : 0), IDM_FOLLOW_FOCUS, L"Follow the keyboard focus");
    AppendMenuW(m, MF_STRING | (g_follow_caret ? MF_CHECKED : 0), IDM_FOLLOW_CARET, L"Follow the text cursor");
    ClientToScreen(g_bar, &p);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, 0, g_bar, NULL);
    DestroyMenu(m);
}

/* ---- the toolbar ------------------------------------------------------------------------- */
#define BAR_W 340
#define BAR_H 56

static void layout_bar(void)
{
    int x = 8, y = 8, h = 40;
    SetRect(&g_btn[0], x, y, x + 40, y + h); x += 40;
    SetRect(&g_btn[1], x, y, x + 64, y + h); x += 64;
    SetRect(&g_btn[2], x, y, x + 40, y + h); x += 48;
    SetRect(&g_btn[3], x, y, x + 88, y + h); x += 92;
    SetRect(&g_btn[4], x, y, x + 40, y + h); x += 40;
    SetRect(&g_btn[5], x, y, x + 40, y + h);
}

static void paint_bar(HWND hwnd, HDC dc)
{
    RECT cr;
    int i;
    WCHAR level[16];
    COLORREF ink = RGB(32, 32, 36);
    HBRUSH bg = CreateSolidBrush(RGB(243, 242, 246)), hot = CreateSolidBrush(RGB(226, 220, 238));
    GetClientRect(hwnd, &cr);
    FillRect(dc, &cr, bg);
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < 6; i++)
    {
        RECT r = g_btn[i];
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, 2, ink), op;
        if (i == g_hot && i != 1) FillRect(dc, &r, hot);
        op = SelectObject(dc, pen);
        SetTextColor(dc, ink);
        switch (i)
        {
        case 0: MoveToEx(dc, cx - 7, cy, NULL); LineTo(dc, cx + 8, cy); break;
        case 2: MoveToEx(dc, cx - 7, cy, NULL); LineTo(dc, cx + 8, cy);
                MoveToEx(dc, cx, cy - 7, NULL); LineTo(dc, cx, cy + 8); break;
        case 1:
            swprintf(level, 16, L"%d%%", g_zoom);
            SelectObject(dc, g_font_big);
            DrawTextW(dc, level, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            break;
        case 3:
        {
            RECT t = r;
            POINT tri[3];
            HBRUSH ib = CreateSolidBrush(ink), obr;
            SelectObject(dc, g_font);
            t.right -= 14;
            DrawTextW(dc, L"Views", -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            tri[0].x = r.right - 18; tri[0].y = cy - 2; tri[1].x = r.right - 10; tri[1].y = cy - 2;
            tri[2].x = r.right - 14; tri[2].y = cy + 2;
            obr = SelectObject(dc, ib);
            Polygon(dc, tri, 3);
            SelectObject(dc, obr); DeleteObject(ib);
            break;
        }
        case 4:
        {
            /* a cog: a ring and eight teeth */
            int k;
            HBRUSH ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, cx - 6, cy - 6, cx + 7, cy + 7);
            Ellipse(dc, cx - 2, cy - 2, cx + 3, cy + 3);
            for (k = 0; k < 8; k++)
            {
                static const int dx[8] = { 0, 6, 9, 6, 0, -6, -9, -6 }, dy[8] = { -9, -6, 0, 6, 9, 6, 0, -6 };
                MoveToEx(dc, cx + dx[k] * 2 / 3, cy + dy[k] * 2 / 3, NULL); LineTo(dc, cx + dx[k], cy + dy[k]);
            }
            SelectObject(dc, ob);
            break;
        }
        case 5:
            SelectObject(dc, g_font_big);
            DrawTextW(dc, L"?", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        SelectObject(dc, op); DeleteObject(pen);
    }
    DeleteObject(bg); DeleteObject(hot);
}

static int hit_button(int x, int y)
{
    POINT p = { x, y };
    int i;
    for (i = 0; i < 6; i++) if (PtInRect(&g_btn[i], p)) return i;
    return -1;
}

static void handle_cmdline(const WCHAR *cmd, BOOL first)
{
    int n = 0;
    WCHAR **argv = CommandLineToArgvW(cmd, &n);
    int i;
    BOOL any = FALSE;
    for (i = 1; argv && i < n; i++)
    {
        const WCHAR *a = argv[i];
        if (*a == '/' || *a == '-') a++;
        any = TRUE;
        if (!_wcsicmp(a, L"fullscreen")) set_mode(MODE_FULL);
        else if (!_wcsicmp(a, L"lens")) set_mode(MODE_LENS);
        else if (!_wcsicmp(a, L"docked")) set_mode(MODE_DOCKED);
        else if (!_wcsicmp(a, L"zoomin")) do_command(IDM_ZOOMIN);
        else if (!_wcsicmp(a, L"zoomout")) do_command(IDM_ZOOMOUT);
        else if (!_wcsicmp(a, L"close")) PostMessageW(g_bar, WM_CLOSE, 0, 0);
        else if (!_wcsnicmp(a, L"zoom:", 5)) set_zoom(_wtoi(a + 5));
        else if (!_wcsicmp(a, L"reload"))
        {
            /* Settings > Ease of Access > Magnifier changed something */
            load_settings();
            InvalidateRect(g_bar, NULL, TRUE);
            frame();
        }
    }
    /* started again with nothing to do: bring the toolbar back */
    if (!any && !first) { ShowWindow(g_bar, SW_RESTORE); SetForegroundWindow(g_bar); }
    if (argv) LocalFree(argv);
}

static LRESULT CALLBACK mouse_ll(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && wp == WM_MOUSEWHEEL &&
        (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000))
    {
        MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
        short delta = (short)HIWORD(m->mouseData);
        PostMessageW(g_bar, WM_COMMAND, delta > 0 ? IDM_ZOOMIN : IDM_ZOOMOUT, 0);
        return 1;   /* the wheel was Magnifier's */
    }
    return CallNextHookEx(g_mouse_hook, code, wp, lp);
}

static LRESULT CALLBACK bar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        layout_bar();
        SetTimer(hwnd, TIMER_FRAME, 33, NULL);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_FRAME) frame();
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_bar(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        int h = hit_button(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (h != g_hot)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            g_hot = h;
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hot = -1; InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
        switch (hit_button(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
        {
        case 0: do_command(IDM_ZOOMOUT); break;
        case 2: do_command(IDM_ZOOMIN); break;
        case 3: views_menu(); break;
        case 4: do_command(IDM_SETTINGS); break;
        case 5: do_command(IDM_HELP); break;
        }
        return 0;
    case WM_COMMAND:
        do_command(LOWORD(wp));
        return 0;
    case WM_HOTKEY:
        switch (wp)
        {
        case HK_FULL: do_command(IDM_FULL); break;
        case HK_LENS: do_command(IDM_LENS); break;
        case HK_DOCKED: do_command(IDM_DOCKED); break;
        case HK_INVERT: do_command(IDM_INVERT); break;
        case HK_IN: case HK_WIN_IN: case HK_WIN_IN2: do_command(IDM_ZOOMIN); break;
        case HK_OUT: case HK_WIN_OUT: case HK_WIN_OUT2: do_command(IDM_ZOOMOUT); break;
        case HK_WIN_ESC: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        }
        return 0;
    case WM_COPYDATA:
    {
        COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp;
        if (cd->dwData == 0x4D41474E /* 'MAGN' */ && cd->cbData >= sizeof(WCHAR))
        {
            WCHAR *s = HeapAlloc(GetProcessHeap(), 0, cd->cbData + sizeof(WCHAR));
            if (s)
            {
                memcpy(s, cd->lpData, cd->cbData);
                s[cd->cbData / sizeof(WCHAR)] = 0;
                handle_cmdline(s, FALSE);
                HeapFree(GetProcessHeap(), 0, s);
            }
        }
        return TRUE;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_FRAME);
        if (g_view) { appbar(FALSE); DestroyWindow(g_view); g_view = NULL; }
        if (g_mouse_hook) UnhookWindowsHookEx(g_mouse_hook);
        if (g_dump[0])
        {
            FILE *f = _wfopen(g_dump, L"w");
            if (f) { fwprintf(f, L"CLOSED\n"); fclose(f); }
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    HANDLE mutex;
    MSG msg;
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    DWORD n;
    (void)prev; (void)cmdline; (void)show;

    g_inst = inst;
    n = GetEnvironmentVariableW(L"SG_MAGNIFY_DUMP", g_dump, MAX_PATH);
    if (!n || n >= MAX_PATH) g_dump[0] = 0;

    /* one Magnifier: another start hands its command line to the running one */
    mutex = CreateMutexW(NULL, TRUE, L"Local\\StainedGlassMagnifier");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND other = NULL;
        int i;
        for (i = 0; i < 50 && !(other = FindWindowW(CLASS_BAR, NULL)); i++) Sleep(100);
        if (other)
        {
            const WCHAR *cl = GetCommandLineW();
            COPYDATASTRUCT cd = { 0x4D41474E, (DWORD)((wcslen(cl) + 1) * sizeof(WCHAR)), (void *)cl };
            DWORD_PTR r;
            SendMessageTimeoutW(other, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &r);
        }
        return 0;
    }

    load_settings();
    g_sw = GetSystemMetrics(SM_CXSCREEN);
    g_sh = GetSystemMetrics(SM_CYSCREEN);
    GetCursorPos(&g_center);

    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = -15;
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -18;
    g_font_big = CreateFontIndirectW(&ncm.lfMessageFont);

    wc.lpfnWndProc = bar_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = CLASS_BAR;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = view_proc;
    wc.lpszClassName = CLASS_VIEW;
    wc.hbrBackground = NULL;
    RegisterClassExW(&wc);

    g_view = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, CLASS_VIEW, L"Magnifier view",
                             WS_POPUP, 0, 0, 1, 1, NULL, NULL, inst, NULL);
    {
        RECT r = { 0, 0, BAR_W, BAR_H };
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRectEx(&r, style, FALSE, WS_EX_TOPMOST);
        g_bar = CreateWindowExW(WS_EX_TOPMOST, CLASS_BAR, L"Magnifier", style,
                                g_sw - (r.right - r.left) - 24, g_sh / 8, r.right - r.left, r.bottom - r.top,
                                NULL, NULL, inst, NULL);
    }
    if (!g_bar) return 1;

    RegisterHotKey(g_bar, HK_FULL, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'F');
    RegisterHotKey(g_bar, HK_LENS, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'L');
    RegisterHotKey(g_bar, HK_DOCKED, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D');
    RegisterHotKey(g_bar, HK_INVERT, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'I');
    RegisterHotKey(g_bar, HK_IN, MOD_CONTROL | MOD_ALT, VK_OEM_PLUS);
    RegisterHotKey(g_bar, HK_OUT, MOD_CONTROL | MOD_ALT, VK_OEM_MINUS);
    /* explorer has these where it is the shell (wine-sg 0182); elsewhere they are ours */
    RegisterHotKey(g_bar, HK_WIN_IN, MOD_WIN, VK_OEM_PLUS);
    RegisterHotKey(g_bar, HK_WIN_IN2, MOD_WIN, VK_ADD);
    RegisterHotKey(g_bar, HK_WIN_OUT, MOD_WIN, VK_OEM_MINUS);
    RegisterHotKey(g_bar, HK_WIN_OUT2, MOD_WIN, VK_SUBTRACT);
    RegisterHotKey(g_bar, HK_WIN_ESC, MOD_WIN | MOD_NOREPEAT, VK_ESCAPE);
    g_mouse_hook = SetWindowsHookExW(WH_MOUSE_LL, mouse_ll, inst, 0);

    ShowWindow(g_bar, SW_SHOWNOACTIVATE);
    handle_cmdline(GetCommandLineW(), TRUE);
    frame();

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}
