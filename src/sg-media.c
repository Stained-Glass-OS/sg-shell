/* sg-media -- Media Player: audio and video, Windows 11's Media Player in the
 * Stained Glass Light palette.
 *
 * Playback is DirectShow (Wine's quartz), decoded by GStreamer through
 * winegstreamer. The graph is built as source -> winegstreamer's own splitter
 * (decodebin) -> whatever renders each stream, because Wine's native AVI and
 * MPEG splitters know few codecs; RenderFile is the fallback. The video
 * renderer's window is a child of our video pane.
 *
 * Command line: files to queue and play (wmplayer's /play, /open, /prefetch
 * and other switches are ignored). One instance: a second hands its files to
 * the first (WM_COPYDATA) and exits, as Media Player does.
 *
 * SG_MEDIA_DUMP=<file> writes the player's state after every tick, for gates.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define COBJMACROS
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <commdlg.h>
#include <dshow.h>
#include <math.h>
#include <stdio.h>
#include <wchar.h>
#include <initguid.h>
#include "sg-mode.h"
/* Stained Glass: the app mode (Settings > Colors, AppsUseLightTheme) picks
 * the palette; WM_SETTINGCHANGE "ImmersiveColorSet" switches it live */
BOOL sgm_dark;
void sgm_follow(HWND hwnd)
{
    BOOL dark = sg_apps_dark();
    if (dark == sgm_dark) return;
    sgm_dark = dark;
    sg_mode_title(hwnd, dark);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

/* winegstreamer's "GStreamer splitter filter" (decodebin) */
DEFINE_GUID(CLSID_SgDecodebinParser, 0xf9d8d64e, 0xa144, 0x47dc, 0x8e, 0xe0, 0xf5, 0x34, 0x98, 0x37, 0x2c, 0x29);

#define CLASS_MAIN  L"SgMediaPlayer"
#define CLASS_VIDEO L"SgMediaVideo"
#define APP_NAME    L"Media Player"
#define SETTINGS    L"Software\\Stained Glass\\Media Player"
#define WM_GRAPHNOTIFY (WM_APP + 1)
#define TIMER_TICK 1
#define COPYDATA_FILES 0x53474d31 /* 'SGM1' */

#define RGBc(r, g, b) RGB(r, g, b)
/* the palette follows the app mode (sg-mode.h): Media Player's light, or dark */
#define C_BG         (sgm_dark ? RGB(0x20, 0x20, 0x20) : RGB(0xF3, 0xF3, 0xF3))
#define C_BAR        (sgm_dark ? RGB(0x2B, 0x2B, 0x2B) : RGB(0xFB, 0xFB, 0xFB))
#define C_LINE       (sgm_dark ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xE5, 0xE5, 0xE5))
#define C_TEXT       (sgm_dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x1A, 0x1A, 0x1A))
#define C_SUB        (sgm_dark ? RGB(0xA8, 0xA8, 0xA8) : RGB(0x5F, 0x5F, 0x5F))
#define C_HOVER      (sgm_dark ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xEA, 0xEA, 0xEA))
#define C_PRESS      (sgm_dark ? RGB(0x48, 0x48, 0x48) : RGB(0xDD, 0xDD, 0xDD))
#define C_TRACK      (sgm_dark ? RGB(0x60, 0x60, 0x60) : RGB(0xC4, 0xC4, 0xC4))
#define C_ACCENT     RGB(112, 48, 192)
#define C_ACCENT_HOT RGB(128, 64, 208)
#define C_WHITE      RGB(0xFF, 0xFF, 0xFF)
#define C_DIM        (sgm_dark ? RGB(0x6E, 0x6E, 0x6E) : RGB(0xA0, 0xA0, 0xA0))

enum { ST_CLOSED, ST_STOPPED, ST_PLAYING, ST_PAUSED };
static const WCHAR *const STATE_NAMES[] = { L"Closed", L"Stopped", L"Playing", L"Paused" };

enum { H_NONE, H_OPEN, H_SEEK, H_PREV, H_PLAY, H_NEXT, H_STOP, H_MUTE, H_VOL, H_FULL, H_COUNT };

static HINSTANCE g_inst;
static HWND g_main, g_video;
static int g_dpi = 96;
static HFONT g_font, g_font_small, g_font_title, g_font_big;

/* the graph */
static IGraphBuilder *g_graph;
static IMediaControl *g_control;
static IMediaSeeking *g_seek;
static IMediaEventEx *g_events;
static IBasicAudio *g_audio;
static IVideoWindow *g_vw;
static IBasicVideo *g_bv;
static BOOL g_has_video;
static long g_vid_w, g_vid_h;
static int g_state = ST_CLOSED;
static LONGLONG g_duration, g_position;
static WCHAR g_error[512];

/* the queue */
static WCHAR **g_queue;
static int g_nqueue, g_current = -1;

/* UI state */
static RECT g_hit[H_COUNT], g_content, g_bar, g_topbar;
static int g_hot = H_NONE, g_pressed = H_NONE;
static BOOL g_dragging_seek, g_dragging_vol;
static LONGLONG g_drag_pos;
static int g_volume = 80;          /* 0..100 */
static BOOL g_muted;
static BOOL g_fullscreen;
static WINDOWPLACEMENT g_placement = { sizeof(WINDOWPLACEMENT) };
static LONG g_saved_style;
static WCHAR g_dump_path[MAX_PATH];

static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* ---- helpers --------------------------------------------------------------------------- */

static const WCHAR *base_name(const WCHAR *path)
{
    const WCHAR *a = wcsrchr(path, '\\'), *b = wcsrchr(path, '/');
    if (b > a) a = b;
    return a ? a + 1 : path;
}

static void fmt_time(LONGLONG t100ns, WCHAR *out, int cch)
{
    LONGLONG s = t100ns / 10000000;
    if (s < 0) s = 0;
    if (s >= 3600) _snwprintf(out, cch, L"%d:%02d:%02d", (int)(s / 3600), (int)(s / 60 % 60), (int)(s % 60));
    else _snwprintf(out, cch, L"%d:%02d", (int)(s / 60), (int)(s % 60));
    out[cch - 1] = 0;
}

static void load_settings(void)
{
    HKEY k;
    DWORD v, cb = sizeof(v);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS, 0, KEY_READ, &k)) return;
    if (!RegQueryValueExW(k, L"Volume", NULL, NULL, (BYTE *)&v, &cb) && v <= 100) g_volume = v;
    cb = sizeof(v);
    if (!RegQueryValueExW(k, L"Muted", NULL, NULL, (BYTE *)&v, &cb)) g_muted = !!v;
    RegCloseKey(k);
}

static void save_settings(void)
{
    HKEY k;
    DWORD v;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    v = g_volume; RegSetValueExW(k, L"Volume", 0, REG_DWORD, (BYTE *)&v, sizeof(v));
    v = g_muted;  RegSetValueExW(k, L"Muted", 0, REG_DWORD, (BYTE *)&v, sizeof(v));
    RegCloseKey(k);
}

/* ---- the dump ---------------------------------------------------------------------------- */

static void put_utf8(FILE *f, const WCHAR *w)
{
    char buf[2048];
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), NULL, NULL);
    if (n > 0) fputs(buf, f);
}

static void screen_rect(const RECT *r, RECT *out)
{
    POINT a = { r->left, r->top }, b = { r->right, r->bottom };
    ClientToScreen(g_main, &a); ClientToScreen(g_main, &b);
    SetRect(out, a.x, a.y, b.x, b.y);
}

static void write_dump(void)
{
    WCHAR tmp[MAX_PATH + 8], line[1200], title[512];
    RECT r, vr;
    FILE *f;
    if (!g_dump_path[0]) return;
    _snwprintf(tmp, ARRAYSIZE(tmp), L"%s.tmp", g_dump_path);
    if (!(f = _wfopen(tmp, L"wb"))) return;
    GetWindowTextW(g_main, title, ARRAYSIZE(title));
    _snwprintf(line, ARRAYSIZE(line), L"STATE %s\nFILE %s\nDURATION_MS %lld\nPOSITION_MS %lld\nVOLUME %d\nMUTED %d\n",
               STATE_NAMES[g_state], g_current >= 0 ? g_queue[g_current] : L"", g_duration / 10000,
               g_position / 10000, g_volume, g_muted);
    put_utf8(f, line);
    GetWindowRect(g_video, &vr);
    _snwprintf(line, ARRAYSIZE(line), L"VIDEO %d %ld %ld\nVIDEORECT %ld %ld %ld %ld\nVIDEOSHOWN %d\n", g_has_video,
               g_vid_w, g_vid_h, vr.left, vr.top, vr.right, vr.bottom, IsWindowVisible(g_video));
    put_utf8(f, line);
    screen_rect(&g_hit[H_SEEK], &r);
    _snwprintf(line, ARRAYSIZE(line), L"SEEKRECT %ld %ld %ld %ld\n", r.left, r.top, r.right, r.bottom);
    put_utf8(f, line);
    screen_rect(&g_hit[H_PLAY], &r);
    _snwprintf(line, ARRAYSIZE(line), L"PLAYRECT %ld %ld %ld %ld\n", r.left, r.top, r.right, r.bottom);
    put_utf8(f, line);
    screen_rect(&g_hit[H_NEXT], &r);
    _snwprintf(line, ARRAYSIZE(line), L"NEXTRECT %ld %ld %ld %ld\n", r.left, r.top, r.right, r.bottom);
    put_utf8(f, line);
    _snwprintf(line, ARRAYSIZE(line), L"FULLSCREEN %d\nQUEUE %d %d\nTITLE %s\nERROR %s\nEND\n", g_fullscreen,
               g_current, g_nqueue, title, g_error);
    put_utf8(f, line);
    fclose(f);
    MoveFileExW(tmp, g_dump_path, MOVEFILE_REPLACE_EXISTING);
}

/* ---- anti-aliased glyphs: drawn at 4x with GDI, box-filtered to alpha ---------------- */

typedef void (*shape_fn)(HDC dc, int w, int h, void *ctx);

static void draw_glyph(HDC dst, const RECT *r, COLORREF color, shape_fn fn, void *ctx)
{
    int w = r->right - r->left, h = r->bottom - r->top, W = w * 4, H = h * 4, x, y, i, j;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), 0, 0, 1, 32, BI_RGB } };
    BYTE *big, *small;
    HBITMAP hb, hs, ob;
    HDC mdc;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    if (w <= 0 || h <= 0) return;
    mdc = CreateCompatibleDC(dst);
    bi.bmiHeader.biWidth = W; bi.bmiHeader.biHeight = -H;
    hb = CreateDIBSection(dst, &bi, DIB_RGB_COLORS, (void **)&big, NULL, 0);
    if (!hb) { DeleteDC(mdc); return; }
    ob = SelectObject(mdc, hb);
    SelectObject(mdc, GetStockObject(WHITE_BRUSH));
    SelectObject(mdc, GetStockObject(NULL_PEN));
    fn(mdc, W, H, ctx);
    GdiFlush();
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
    hs = CreateDIBSection(dst, &bi, DIB_RGB_COLORS, (void **)&small, NULL, 0);
    if (hs)
    {
        for (y = 0; y < h; y++)
            for (x = 0; x < w; x++)
            {
                int sum = 0;
                BYTE *px = small + (y * w + x) * 4;
                for (j = 0; j < 4; j++)
                    for (i = 0; i < 4; i++) sum += big[((y * 4 + j) * W + x * 4 + i) * 4 + 1];
                sum /= 16;
                px[0] = GetBValue(color) * sum / 255;
                px[1] = GetGValue(color) * sum / 255;
                px[2] = GetRValue(color) * sum / 255;
                px[3] = sum;
            }
        SelectObject(mdc, hs);
        AlphaBlend(dst, r->left, r->top, w, h, mdc, 0, 0, w, h, bf);
        DeleteObject(hs);
    }
    SelectObject(mdc, ob);
    DeleteObject(hb);
    DeleteDC(mdc);
}

static void poly(HDC dc, const double *pts, int n, int w, int h)
{
    POINT p[16];
    int i;
    for (i = 0; i < n && i < 16; i++) { p[i].x = (LONG)(pts[2 * i] * w); p[i].y = (LONG)(pts[2 * i + 1] * h); }
    Polygon(dc, p, n);
}

static void rectf(HDC dc, double l, double t, double r, double b, int w, int h)
{
    Rectangle(dc, (int)(l * w), (int)(t * h), (int)(r * w) + 1, (int)(b * h) + 1);
}

static void shape_play(HDC dc, int w, int h, void *c)
{ static const double p[] = { .34, .24, .34, .76, .76, .5 }; (void)c; poly(dc, p, 3, w, h); }
static void shape_pause(HDC dc, int w, int h, void *c)
{ (void)c; rectf(dc, .3, .24, .43, .76, w, h); rectf(dc, .57, .24, .7, .76, w, h); }
static void shape_stop(HDC dc, int w, int h, void *c)
{ (void)c; RoundRect(dc, (int)(.3 * w), (int)(.3 * h), (int)(.7 * w), (int)(.7 * h), w / 10, h / 10); }
static void shape_prev(HDC dc, int w, int h, void *c)
{ static const double p[] = { .7, .28, .7, .72, .36, .5 }; (void)c; poly(dc, p, 3, w, h); rectf(dc, .28, .28, .34, .72, w, h); }
static void shape_next(HDC dc, int w, int h, void *c)
{ static const double p[] = { .3, .28, .3, .72, .64, .5 }; (void)c; poly(dc, p, 3, w, h); rectf(dc, .66, .28, .72, .72, w, h); }

static void arc_stroke(HDC dc, int cx, int cy, int rad, int thick, int w)
{
    /* a right-facing arc: an annulus sector drawn as two ellipses with the left half masked */
    HRGN outer = CreateEllipticRgn(cx - rad - thick, cy - rad - thick, cx + rad + thick, cy + rad + thick);
    HRGN inner = CreateEllipticRgn(cx - rad, cy - rad, cx + rad, cy + rad);
    HRGN half = CreateRectRgn(cx + thick, 0, w, cy * 2);
    CombineRgn(outer, outer, inner, RGN_DIFF);
    CombineRgn(outer, outer, half, RGN_AND);
    FillRgn(dc, outer, GetStockObject(WHITE_BRUSH));
    DeleteObject(outer); DeleteObject(inner); DeleteObject(half);
}

static void shape_speaker(HDC dc, int w, int h, void *c)
{
    static const double p[] = { .14, .4, .28, .4, .46, .24, .46, .76, .28, .6, .14, .6 };
    int level = *(int *)c; /* -1 muted, 0..3 waves */
    poly(dc, p, 6, w, h);
    if (level < 0)
    {
        HPEN pen = CreatePen(PS_SOLID, w / 14, RGB(255, 255, 255)), op = SelectObject(dc, pen);
        MoveToEx(dc, (int)(.58 * w), (int)(.38 * h), NULL); LineTo(dc, (int)(.82 * w), (int)(.62 * h));
        MoveToEx(dc, (int)(.82 * w), (int)(.38 * h), NULL); LineTo(dc, (int)(.58 * w), (int)(.62 * h));
        SelectObject(dc, op); DeleteObject(pen);
        return;
    }
    if (level >= 1) arc_stroke(dc, (int)(.46 * w), h / 2, (int)(.12 * w), w / 18, w);
    if (level >= 2) arc_stroke(dc, (int)(.46 * w), h / 2, (int)(.24 * w), w / 18, w);
    if (level >= 3) arc_stroke(dc, (int)(.46 * w), h / 2, (int)(.36 * w), w / 18, w);
}

static void shape_full(HDC dc, int w, int h, void *c)
{
    int t = w / 14, in = *(int *)c; /* 1: exit full screen (corners point inward) */
    double a = .26, b = .74, l = .16;
    (void)in;
    Rectangle(dc, (int)(a * w), (int)(a * h), (int)((a + l) * w), (int)(a * h) + t);
    Rectangle(dc, (int)(a * w), (int)(a * h), (int)(a * w) + t, (int)((a + l) * h));
    Rectangle(dc, (int)((b - l) * w), (int)(a * h), (int)(b * w), (int)(a * h) + t);
    Rectangle(dc, (int)(b * w) - t, (int)(a * h), (int)(b * w), (int)((a + l) * h));
    Rectangle(dc, (int)(a * w), (int)(b * h) - t, (int)((a + l) * w), (int)(b * h));
    Rectangle(dc, (int)(a * w), (int)((b - l) * h), (int)(a * w) + t, (int)(b * h));
    Rectangle(dc, (int)((b - l) * w), (int)(b * h) - t, (int)(b * w), (int)(b * h));
    Rectangle(dc, (int)(b * w) - t, (int)((b - l) * h), (int)(b * w), (int)(b * h));
}

static void shape_folder(HDC dc, int w, int h, void *c)
{
    static const double p[] = { .14, .3, .4, .3, .48, .38, .86, .38, .86, .74, .14, .74 };
    (void)c; poly(dc, p, 6, w, h);
}

static void shape_note(HDC dc, int w, int h, void *c)
{
    (void)c;
    Ellipse(dc, (int)(.26 * w), (int)(.6 * h), (int)(.46 * w), (int)(.76 * h));
    Ellipse(dc, (int)(.56 * w), (int)(.52 * h), (int)(.76 * w), (int)(.68 * h));
    rectf(dc, .41, .26, .46, .68, w, h);
    rectf(dc, .71, .18, .76, .6, w, h);
    {
        static const double p[] = { .41, .26, .76, .18, .76, .28, .41, .36 };
        poly(dc, p, 4, w, h);
    }
}

static void shape_film(HDC dc, int w, int h, void *c)
{
    int i;
    HRGN r = CreateRoundRectRgn((int)(.18 * w), (int)(.24 * h), (int)(.82 * w), (int)(.76 * h), w / 12, h / 12), hole;
    (void)c;
    for (i = 0; i < 5; i++)
    {
        int x = (int)((.24 + i * .12) * w);
        hole = CreateRectRgn(x, (int)(.28 * h), x + w / 20, (int)(.33 * h)); CombineRgn(r, r, hole, RGN_DIFF); DeleteObject(hole);
        hole = CreateRectRgn(x, (int)(.67 * h), x + w / 20, (int)(.72 * h)); CombineRgn(r, r, hole, RGN_DIFF); DeleteObject(hole);
    }
    hole = CreateRectRgn((int)(.22 * w), (int)(.37 * h), (int)(.78 * w), (int)(.63 * h));
    CombineRgn(r, r, hole, RGN_DIFF); DeleteObject(hole);
    FillRgn(dc, r, GetStockObject(WHITE_BRUSH));
    DeleteObject(r);
}

/* ---- layout and painting ------------------------------------------------------------- */

static void layout(void)
{
    RECT c;
    int cx, y, bar_h = S(100), top_h = S(44);
    GetClientRect(g_main, &c);
    if (g_fullscreen)
    {
        g_content = c;
        SetRectEmpty(&g_bar); SetRectEmpty(&g_topbar);
        memset(g_hit, 0, sizeof(g_hit));
    }
    else
    {
        SetRect(&g_topbar, 0, 0, c.right, top_h);
        SetRect(&g_bar, 0, max(top_h, c.bottom - bar_h), c.right, c.bottom);
        SetRect(&g_content, 0, top_h, c.right, g_bar.top);
        SetRect(&g_hit[H_OPEN], S(8), S(6), S(8) + S(112), top_h - S(6));
        y = g_bar.top + S(10);
        SetRect(&g_hit[H_SEEK], S(64), y, c.right - S(64), y + S(24));
        y = g_bar.top + S(44);
        cx = c.right / 2;
        SetRect(&g_hit[H_PLAY], cx - S(24), y, cx + S(24), y + S(48));
        SetRect(&g_hit[H_PREV], cx - S(24) - S(8) - S(40), y + S(4), cx - S(32), y + S(44));
        SetRect(&g_hit[H_STOP], g_hit[H_PREV].left - S(4) - S(40), y + S(4), g_hit[H_PREV].left - S(4), y + S(44));
        SetRect(&g_hit[H_NEXT], cx + S(32), y + S(4), cx + S(32) + S(40), y + S(44));
        SetRect(&g_hit[H_FULL], c.right - S(12) - S(40), y + S(4), c.right - S(12), y + S(44));
        SetRect(&g_hit[H_VOL], g_hit[H_FULL].left - S(8) - S(100), y + S(4), g_hit[H_FULL].left - S(8), y + S(44));
        SetRect(&g_hit[H_MUTE], g_hit[H_VOL].left - S(4) - S(40), y + S(4), g_hit[H_VOL].left - S(4), y + S(44));
    }
    if (g_video)
        MoveWindow(g_video, g_content.left, g_content.top, g_content.right - g_content.left,
                   g_content.bottom - g_content.top, TRUE);
}

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    SetDCBrushColor(dc, c);
    FillRect(dc, r, GetStockObject(DC_BRUSH));
}

static void round_fill(HDC dc, const RECT *r, int rad, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c), ob = SelectObject(dc, b);
    HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, r->left, r->top, r->right + 1, r->bottom + 1, rad, rad);
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(b);
}

static void button_bg(HDC dc, int id)
{
    if (g_pressed == id && g_hot == id) round_fill(dc, &g_hit[id], S(8), C_PRESS);
    else if (g_hot == id) round_fill(dc, &g_hit[id], S(8), C_HOVER);
}

static void glyph_in(HDC dc, const RECT *r, int size, COLORREF col, shape_fn fn, void *ctx)
{
    RECT g;
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;
    SetRect(&g, cx - size / 2, cy - size / 2, cx - size / 2 + size, cy - size / 2 + size);
    draw_glyph(dc, &g, col, fn, ctx);
}

static LONGLONG shown_position(void) { return g_dragging_seek ? g_drag_pos : g_position; }

static void paint_content(HDC dc)
{
    RECT r = g_content, tile, t;
    const WCHAR *name;
    WCHAR title[MAX_PATH];
    int size;

    fill(dc, &r, C_BG);
    if (g_has_video && !g_error[0]) return;   /* the video pane covers it */

    size = min(S(200), min(r.right - r.left, r.bottom - r.top) - S(100));
    if (size < S(48)) size = S(48);
    SetRect(&tile, (r.left + r.right - size) / 2, r.top + (r.bottom - r.top - size - S(70)) / 2, 0, 0);
    tile.right = tile.left + size; tile.bottom = tile.top + size;
    if (tile.top < r.top + S(8)) OffsetRect(&tile, 0, r.top + S(8) - tile.top);
    {
        /* a gradient tile in the accent colours: our own album art stand-in */
        TRIVERTEX v[2] = { { tile.left, tile.top, 0x8a00, 0x3c00, 0xd800, 0xff00 },
                           { tile.right, tile.bottom, 0x4b00, 0x1c00, 0x7a00, 0xff00 } };
        GRADIENT_RECT gr = { 0, 1 };
        HRGN clip = CreateRoundRectRgn(tile.left, tile.top, tile.right + 1, tile.bottom + 1, S(16), S(16));
        SelectClipRgn(dc, clip);
        GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
        SelectClipRgn(dc, NULL);
        DeleteObject(clip);
    }
    draw_glyph(dc, &tile, C_WHITE, g_current >= 0 || !g_error[0] ? shape_note : shape_film, NULL);

    SetBkMode(dc, TRANSPARENT);
    t = r; t.top = tile.bottom + S(16); t.bottom = t.top + S(36);
    SelectObject(dc, g_font_big);
    SetTextColor(dc, C_TEXT);
    if (g_current >= 0)
    {
        WCHAR *dot;
        lstrcpynW(title, base_name(g_queue[g_current]), MAX_PATH);
        if ((dot = wcsrchr(title, '.'))) *dot = 0;
        name = title;
    }
    else name = L"Play something";
    DrawTextW(dc, name, -1, &t, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    t.top = t.bottom; t.bottom = t.top + S(40); t.left += S(24); t.right -= S(24);
    SelectObject(dc, g_font);
    SetTextColor(dc, g_error[0] ? RGB(0xC4, 0x2B, 0x1C) : C_SUB);
    DrawTextW(dc, g_error[0] ? g_error : g_current >= 0 ? L"" : L"Open a music or video file, or drop one here.",
              -1, &t, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
}

static void paint_topbar(HDC dc)
{
    RECT r = g_topbar, t;
    fill(dc, &r, C_BG);
    button_bg(dc, H_OPEN);
    t = g_hit[H_OPEN]; t.right = t.left + S(36);
    glyph_in(dc, &t, S(24), C_ACCENT, shape_folder, NULL);
    t.left = t.right; t.right = g_hit[H_OPEN].right;
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font);
    SetTextColor(dc, C_TEXT);
    DrawTextW(dc, L"Open file(s)", -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

static void paint_bar(HDC dc)
{
    RECT r = g_bar, t, track, fillr;
    WCHAR buf[64];
    LONGLONG pos = shown_position();
    int thumb_x, vol_x, level, full = 0;
    BOOL enabled = g_state != ST_CLOSED;
    COLORREF glyph = enabled ? C_TEXT : C_DIM;

    fill(dc, &r, C_BAR);
    t = r; t.bottom = t.top + 1;
    fill(dc, &t, C_LINE);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font_small);
    SetTextColor(dc, C_SUB);

    /* seek row: elapsed | track | total */
    fmt_time(pos, buf, 64);
    SetRect(&t, S(8), g_hit[H_SEEK].top, g_hit[H_SEEK].left - S(8), g_hit[H_SEEK].bottom);
    DrawTextW(dc, buf, -1, &t, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    fmt_time(g_duration, buf, 64);
    SetRect(&t, g_hit[H_SEEK].right + S(8), g_hit[H_SEEK].top, r.right - S(8), g_hit[H_SEEK].bottom);
    DrawTextW(dc, buf, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    track = g_hit[H_SEEK];
    InflateRect(&track, -S(8), 0);
    track.top = (track.top + track.bottom) / 2 - S(2); track.bottom = track.top + S(4);
    round_fill(dc, &track, S(4), C_TRACK);
    thumb_x = track.left + (g_duration > 0 ? (int)((track.right - track.left) * (double)pos / (double)g_duration) : 0);
    thumb_x = max(track.left, min(track.right, thumb_x));
    fillr = track; fillr.right = thumb_x;
    if (fillr.right > fillr.left) round_fill(dc, &fillr, S(4), C_ACCENT);
    if (enabled)
    {
        RECT th = { thumb_x - S(9), (track.top + track.bottom) / 2 - S(9), thumb_x + S(9), (track.top + track.bottom) / 2 + S(9) };
        HBRUSH wb = CreateSolidBrush(C_WHITE), ob;
        HPEN pen = CreatePen(PS_SOLID, 1, C_LINE), op;
        ob = SelectObject(dc, wb); op = SelectObject(dc, pen);
        Ellipse(dc, th.left, th.top, th.right, th.bottom);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(wb); DeleteObject(pen);
        InflateRect(&th, -S(4), -S(4));
        {
            HBRUSH ab = CreateSolidBrush(g_hot == H_SEEK || g_dragging_seek ? C_ACCENT_HOT : C_ACCENT);
            ob = SelectObject(dc, ab); op = SelectObject(dc, GetStockObject(NULL_PEN));
            Ellipse(dc, th.left, th.top, th.right + 1, th.bottom + 1);
            SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(ab);
        }
    }

    /* now playing, on the left */
    if (g_current >= 0 && r.right > S(640))
    {
        RECT tile = { S(16), g_hit[H_PLAY].top, S(16) + S(48), g_hit[H_PLAY].top + S(48) };
        WCHAR name[MAX_PATH], *dot;
        round_fill(dc, &tile, S(8), C_ACCENT);
        draw_glyph(dc, &tile, C_WHITE, g_has_video ? shape_film : shape_note, NULL);
        lstrcpynW(name, base_name(g_queue[g_current]), MAX_PATH);
        if ((dot = wcsrchr(name, '.'))) *dot = 0;
        SetRect(&t, tile.right + S(12), tile.top + S(4), g_hit[H_STOP].left - S(12), tile.top + S(26));
        SelectObject(dc, g_font_title);
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, name, -1, &t, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        OffsetRect(&t, 0, S(20));
        SelectObject(dc, g_font_small);
        SetTextColor(dc, C_SUB);
        _snwprintf(buf, 64, L"%d of %d", g_current + 1, g_nqueue);
        DrawTextW(dc, buf, -1, &t, DT_LEFT | DT_SINGLELINE);
    }

    /* transport */
    button_bg(dc, H_STOP); glyph_in(dc, &g_hit[H_STOP], S(32), glyph, shape_stop, NULL);
    button_bg(dc, H_PREV); glyph_in(dc, &g_hit[H_PREV], S(32), g_nqueue > 0 ? C_TEXT : C_DIM, shape_prev, NULL);
    button_bg(dc, H_NEXT); glyph_in(dc, &g_hit[H_NEXT], S(32), g_current + 1 < g_nqueue ? C_TEXT : C_DIM, shape_next, NULL);
    {
        HBRUSH b = CreateSolidBrush(!enabled ? C_DIM : g_hot == H_PLAY ? C_ACCENT_HOT : C_ACCENT), ob = SelectObject(dc, b);
        HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, g_hit[H_PLAY].left, g_hit[H_PLAY].top, g_hit[H_PLAY].right + 1, g_hit[H_PLAY].bottom + 1);
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(b);
        glyph_in(dc, &g_hit[H_PLAY], S(40), C_WHITE, g_state == ST_PLAYING ? shape_pause : shape_play, NULL);
    }

    /* volume */
    level = g_muted || !g_volume ? -1 : g_volume < 34 ? 1 : g_volume < 67 ? 2 : 3;
    button_bg(dc, H_MUTE); glyph_in(dc, &g_hit[H_MUTE], S(32), C_TEXT, shape_speaker, &level);
    track = g_hit[H_VOL];
    InflateRect(&track, -S(8), 0);
    track.top = (track.top + track.bottom) / 2 - S(2); track.bottom = track.top + S(4);
    round_fill(dc, &track, S(4), C_TRACK);
    vol_x = track.left + (track.right - track.left) * g_volume / 100;
    fillr = track; fillr.right = vol_x;
    if (fillr.right > fillr.left) round_fill(dc, &fillr, S(4), g_muted ? C_DIM : C_ACCENT);
    {
        RECT th = { vol_x - S(7), (track.top + track.bottom) / 2 - S(7), vol_x + S(7), (track.top + track.bottom) / 2 + S(7) };
        HBRUSH ab = CreateSolidBrush(g_muted ? C_DIM : C_ACCENT), ob = SelectObject(dc, ab);
        HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, th.left, th.top, th.right + 1, th.bottom + 1);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(ab);
    }
    button_bg(dc, H_FULL); glyph_in(dc, &g_hit[H_FULL], S(32), g_has_video ? C_TEXT : C_DIM, shape_full, &full);
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps), mem;
    RECT c;
    HBITMAP bmp, old;
    GetClientRect(hwnd, &c);
    mem = CreateCompatibleDC(dc);
    bmp = CreateCompatibleBitmap(dc, max(1, c.right), max(1, c.bottom));
    old = SelectObject(mem, bmp);
    paint_content(mem);
    if (!g_fullscreen) { paint_topbar(mem); paint_bar(mem); }
    BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

/* ---- playback -------------------------------------------------------------------------- */

static void update_title(void)
{
    WCHAR t[MAX_PATH + 32];
    if (g_current >= 0) _snwprintf(t, ARRAYSIZE(t), L"%s - " APP_NAME, base_name(g_queue[g_current]));
    else lstrcpyW(t, APP_NAME);
    t[ARRAYSIZE(t) - 1] = 0;
    SetWindowTextW(g_main, t);
}

static void apply_volume(void)
{
    long db;
    if (!g_audio) return;
    if (g_muted || g_volume <= 0) db = -10000;
    else db = (long)(2000.0 * log10(g_volume / 100.0));   /* amplitude -> hundredths of a decibel */
    if (db < -10000) db = -10000;
    IBasicAudio_put_Volume(g_audio, db);
}

static void place_video(void)
{
    RECT c;
    long w, h, x, y;
    if (!g_vw || !g_has_video) return;
    GetClientRect(g_video, &c);
    w = c.right; h = c.bottom;
    if (g_vid_w > 0 && g_vid_h > 0)
    {
        if (w * g_vid_h > h * g_vid_w) w = h * g_vid_w / g_vid_h;   /* letterbox, keep the aspect */
        else h = w * g_vid_h / g_vid_w;
    }
    x = (c.right - w) / 2; y = (c.bottom - h) / 2;
    IVideoWindow_SetWindowPosition(g_vw, x, y, w, h);
}

static void close_media(void)
{
    if (g_control) IMediaControl_Stop(g_control);
    if (g_vw)
    {
        IVideoWindow_put_Visible(g_vw, OAFALSE);
        IVideoWindow_put_Owner(g_vw, 0);
        IVideoWindow_put_MessageDrain(g_vw, 0);
    }
    if (g_events) { IMediaEventEx_SetNotifyWindow(g_events, 0, 0, 0); IMediaEventEx_Release(g_events); g_events = NULL; }
    if (g_vw) { IVideoWindow_Release(g_vw); g_vw = NULL; }
    if (g_bv) { IBasicVideo_Release(g_bv); g_bv = NULL; }
    if (g_audio) { IBasicAudio_Release(g_audio); g_audio = NULL; }
    if (g_seek) { IMediaSeeking_Release(g_seek); g_seek = NULL; }
    if (g_control) { IMediaControl_Release(g_control); g_control = NULL; }
    if (g_graph) { IGraphBuilder_Release(g_graph); g_graph = NULL; }
    g_has_video = FALSE;
    g_vid_w = g_vid_h = 0;
    g_duration = g_position = 0;
    g_state = ST_CLOSED;
    ShowWindow(g_video, SW_HIDE);
}

/* source -> winegstreamer's decodebin splitter -> render each stream */
static HRESULT build_with_gstreamer(const WCHAR *path, int *streams)
{
    IBaseFilter *src = NULL, *split = NULL;
    IEnumPins *e = NULL;
    IPin *pin, *out = NULL, *in = NULL;
    HRESULT hr;
    *streams = 0;
    if (FAILED(hr = IGraphBuilder_AddSourceFilter(g_graph, path, L"Source", &src))) return hr;
    hr = CoCreateInstance(&CLSID_SgDecodebinParser, NULL, CLSCTX_INPROC_SERVER, &IID_IBaseFilter, (void **)&split);
    if (SUCCEEDED(hr)) hr = IGraphBuilder_AddFilter(g_graph, split, L"Splitter");
    if (SUCCEEDED(hr) && SUCCEEDED(IBaseFilter_EnumPins(src, &e)))
    {
        while (!out && IEnumPins_Next(e, 1, &pin, NULL) == S_OK)
        {
            PIN_DIRECTION d;
            if (SUCCEEDED(IPin_QueryDirection(pin, &d)) && d == PINDIR_OUTPUT) out = pin;
            else IPin_Release(pin);
        }
        IEnumPins_Release(e); e = NULL;
    }
    if (SUCCEEDED(hr) && SUCCEEDED(IBaseFilter_EnumPins(split, &e)))
    {
        while (!in && IEnumPins_Next(e, 1, &pin, NULL) == S_OK)
        {
            PIN_DIRECTION d;
            if (SUCCEEDED(IPin_QueryDirection(pin, &d)) && d == PINDIR_INPUT) in = pin;
            else IPin_Release(pin);
        }
        IEnumPins_Release(e); e = NULL;
    }
    if (SUCCEEDED(hr)) hr = out && in ? IGraphBuilder_ConnectDirect(g_graph, out, in, NULL) : E_FAIL;
    if (SUCCEEDED(hr) && SUCCEEDED(IBaseFilter_EnumPins(split, &e)))
    {
        while (IEnumPins_Next(e, 1, &pin, NULL) == S_OK)
        {
            PIN_DIRECTION d;
            if (SUCCEEDED(IPin_QueryDirection(pin, &d)) && d == PINDIR_OUTPUT &&
                SUCCEEDED(IGraphBuilder_Render(g_graph, pin)))
                (*streams)++;
            IPin_Release(pin);
        }
        IEnumPins_Release(e);
        if (!*streams) hr = VFW_E_CANNOT_RENDER;
    }
    if (out) IPin_Release(out);
    if (in) IPin_Release(in);
    if (split) IBaseFilter_Release(split);
    if (src) IBaseFilter_Release(src);
    return hr;
}

static HRESULT new_graph(void)
{
    HRESULT hr = CoCreateInstance(&CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, &IID_IGraphBuilder, (void **)&g_graph);
    if (FAILED(hr)) return hr;
    IGraphBuilder_QueryInterface(g_graph, &IID_IMediaControl, (void **)&g_control);
    IGraphBuilder_QueryInterface(g_graph, &IID_IMediaSeeking, (void **)&g_seek);
    IGraphBuilder_QueryInterface(g_graph, &IID_IMediaEventEx, (void **)&g_events);
    IGraphBuilder_QueryInterface(g_graph, &IID_IBasicAudio, (void **)&g_audio);
    IGraphBuilder_QueryInterface(g_graph, &IID_IVideoWindow, (void **)&g_vw);
    IGraphBuilder_QueryInterface(g_graph, &IID_IBasicVideo, (void **)&g_bv);
    return g_control && g_seek ? S_OK : E_NOINTERFACE;
}

static BOOL open_current(BOOL play)
{
    const WCHAR *path;
    HRESULT hr;
    int streams = 0;

    close_media();
    g_error[0] = 0;
    if (g_current < 0 || g_current >= g_nqueue) { update_title(); InvalidateRect(g_main, NULL, FALSE); return FALSE; }
    path = g_queue[g_current];
    update_title();

    if (SUCCEEDED(hr = new_graph()))
    {
#ifndef SG_MUTANT_NO_GSTREAMER_SPLITTER
        hr = build_with_gstreamer(path, &streams);
#else
        hr = E_FAIL;
#endif
        if (FAILED(hr))
        {
            /* Wine's own splitters, for anything decodebin will not take */
            close_media();
            if (SUCCEEDED(hr = new_graph())) hr = IGraphBuilder_RenderFile(g_graph, path, NULL);
        }
    }
    if (FAILED(hr))
    {
        close_media();
        _snwprintf(g_error, ARRAYSIZE(g_error),
                   L"Can't play this file. The file type isn't supported, or the file is damaged. (0x%08lx)", hr);
        InvalidateRect(g_main, NULL, FALSE);
        write_dump();
        return FALSE;
    }

    IMediaSeeking_SetTimeFormat(g_seek, &TIME_FORMAT_MEDIA_TIME);
    IMediaSeeking_GetDuration(g_seek, &g_duration);
    if (g_events) IMediaEventEx_SetNotifyWindow(g_events, (OAHWND)g_main, WM_GRAPHNOTIFY, 0);
    if (g_bv && SUCCEEDED(IBasicVideo_get_SourceWidth(g_bv, &g_vid_w)) &&
        SUCCEEDED(IBasicVideo_get_SourceHeight(g_bv, &g_vid_h)) && g_vid_w > 0 && g_vid_h > 0)
        g_has_video = TRUE;
    if (g_has_video)
    {
        ShowWindow(g_video, SW_SHOW);
        IVideoWindow_put_Owner(g_vw, (OAHWND)g_video);
        IVideoWindow_put_WindowStyle(g_vw, WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
        IVideoWindow_put_MessageDrain(g_vw, (OAHWND)g_main);
        place_video();
        IVideoWindow_put_Visible(g_vw, OATRUE);
    }
    else ShowWindow(g_video, SW_HIDE);
    apply_volume();
    g_state = ST_STOPPED;
    if (play && SUCCEEDED(IMediaControl_Run(g_control))) g_state = ST_PLAYING;
    else if (!play) { IMediaControl_Pause(g_control); g_state = ST_PAUSED; }
    InvalidateRect(g_main, NULL, FALSE);
    write_dump();
    return TRUE;
}

static void play_pause(void)
{
    if (g_state == ST_CLOSED)
    {
        if (g_nqueue) { if (g_current < 0) g_current = 0; open_current(TRUE); }
        return;
    }
    if (g_state == ST_PLAYING)
    {
#ifndef SG_MUTANT_PAUSE_IGNORED
        if (SUCCEEDED(IMediaControl_Pause(g_control))) g_state = ST_PAUSED;
#endif
    }
    else if (SUCCEEDED(IMediaControl_Run(g_control))) g_state = ST_PLAYING;
    InvalidateRect(g_main, NULL, FALSE);
}

static void stop(void)
{
    LONGLONG zero = 0;
    if (g_state == ST_CLOSED) return;
    IMediaControl_Stop(g_control);
    IMediaSeeking_SetPositions(g_seek, &zero, AM_SEEKING_AbsolutePositioning, NULL, AM_SEEKING_NoPositioning);
    g_position = 0;
    g_state = ST_STOPPED;
    InvalidateRect(g_main, NULL, FALSE);
}

static void seek_to(LONGLONG t)
{
    if (g_state == ST_CLOSED || !g_seek) return;
    if (t < 0) t = 0;
    if (g_duration > 0 && t > g_duration) t = g_duration;
#ifdef SG_MUTANT_SEEK_IGNORED
    return;
#endif
    IMediaSeeking_SetPositions(g_seek, &t, AM_SEEKING_AbsolutePositioning, NULL, AM_SEEKING_NoPositioning);
    g_position = t;
    InvalidateRect(g_main, NULL, FALSE);
}

static void go(int delta)
{
    int n = g_current + delta;
    if (delta < 0 && g_position > 30000000) { seek_to(0); return; }   /* Previous restarts a track 3 s in */
    if (n < 0 || n >= g_nqueue) return;
    g_current = n;
    open_current(TRUE);
}

static void set_queue(WCHAR **files, int n)
{
    int i;
    for (i = 0; i < g_nqueue; i++) free(g_queue[i]);
    free(g_queue);
    g_queue = NULL; g_nqueue = 0; g_current = -1;
    if (n <= 0) return;
    g_queue = calloc(n, sizeof(*g_queue));
    for (i = 0; i < n; i++)
    {
        WCHAR full[MAX_PATH];
        if (!GetFullPathNameW(files[i], MAX_PATH, full, NULL)) lstrcpynW(full, files[i], MAX_PATH);
        g_queue[g_nqueue++] = _wcsdup(full);
    }
    g_current = 0;
}

/* files from a command line; wmplayer's switches (/play, /open, /prefetch:N ...) are ignored */
static int files_from_cmdline(const WCHAR *cmdline, BOOL skip_first, WCHAR ***out)
{
    int argc = 0, i, n = 0;
    WCHAR **argv = cmdline && *cmdline ? CommandLineToArgvW(cmdline, &argc) : NULL;
    WCHAR **files = argc ? calloc(argc, sizeof(*files)) : NULL;
    for (i = skip_first ? 1 : 0; i < argc; i++)
    {
        if (argv[i][0] == '/' && !wcschr(argv[i] + 1, '/') && GetFileAttributesW(argv[i]) == INVALID_FILE_ATTRIBUTES)
            continue;
        if (argv[i][0] == '-' && argv[i][1] == '-') continue;
        files[n++] = _wcsdup(argv[i]);
    }
    if (argv) LocalFree(argv);
    *out = files;
    return n;
}

static void open_files(WCHAR **files, int n)
{
    if (n <= 0) return;
    set_queue(files, n);
    open_current(TRUE);
}

static void open_dialog(void)
{
    static WCHAR buf[32768];
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR *files[256], *p, dir[MAX_PATH], full[MAX_PATH * 2];
    int n = 0;
    buf[0] = 0;
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"Media files\0*.mp3;*.wav;*.ogg;*.oga;*.opus;*.flac;*.m4a;*.aac;*.wma;*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.ogv;*.wmv;*.avi\0"
                      L"Music\0*.mp3;*.wav;*.ogg;*.oga;*.opus;*.flac;*.m4a;*.aac;*.wma\0"
                      L"Videos\0*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.ogv;*.wmv;*.avi\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = ARRAYSIZE(buf);
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;
    p = buf + lstrlenW(buf) + 1;
    if (!*p) { files[n++] = _wcsdup(buf); }
    else
    {
        lstrcpynW(dir, buf, MAX_PATH);
        for (; *p && n < 256; p += lstrlenW(p) + 1)
        {
            _snwprintf(full, ARRAYSIZE(full), L"%s\\%s", dir, p);
            full[ARRAYSIZE(full) - 1] = 0;
            files[n++] = _wcsdup(full);
        }
    }
    open_files(files, n);
    while (n--) free(files[n]);
}

static void set_fullscreen(BOOL on)
{
    if (on == g_fullscreen) return;
    if (on)
    {
        MONITORINFO mi = { sizeof(mi) };
        if (!g_has_video) return;
        GetWindowPlacement(g_main, &g_placement);
        g_saved_style = GetWindowLongW(g_main, GWL_STYLE);
        GetMonitorInfoW(MonitorFromWindow(g_main, MONITOR_DEFAULTTONEAREST), &mi);
        g_fullscreen = TRUE;
        SetWindowLongW(g_main, GWL_STYLE, g_saved_style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(g_main, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
    else
    {
        g_fullscreen = FALSE;
        SetWindowLongW(g_main, GWL_STYLE, g_saved_style);
        SetWindowPlacement(g_main, &g_placement);
        SetWindowPos(g_main, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    layout();
    place_video();
    InvalidateRect(g_main, NULL, FALSE);
}

static void set_volume(int v)
{
    g_volume = max(0, min(100, v));
    if (g_volume) g_muted = FALSE;
    apply_volume();
    save_settings();
    InvalidateRect(g_main, &g_bar, FALSE);
}

static void handle_graph_events(void)
{
    long code;
    LONG_PTR p1, p2;
    if (!g_events) return;
    while (g_events && SUCCEEDED(IMediaEventEx_GetEvent(g_events, &code, &p1, &p2, 0)))
    {
        IMediaEventEx_FreeEventParams(g_events, code, p1, p2);
        if (code == EC_COMPLETE)
        {
            if (g_current + 1 < g_nqueue) { g_current++; open_current(TRUE); return; }
            stop();
        }
        else if (code == EC_ERRORABORT || code == EC_USERABORT)
        {
            stop();
            lstrcpyW(g_error, L"Playback stopped because of an error.");
        }
    }
}

static LONGLONG pos_from_x(int x)
{
    RECT track = g_hit[H_SEEK];
    InflateRect(&track, -S(8), 0);
    if (track.right <= track.left || g_duration <= 0) return 0;
    x = max(track.left, min(track.right, x));
    return (LONGLONG)((double)(x - track.left) / (track.right - track.left) * g_duration);
}

static int vol_from_x(int x)
{
    RECT track = g_hit[H_VOL];
    InflateRect(&track, -S(8), 0);
    if (track.right <= track.left) return g_volume;
    return MulDiv(max(0, min(track.right - track.left, x - track.left)), 100, track.right - track.left);
}

static int hit_test(int x, int y)
{
    POINT pt = { x, y };
    int i;
    for (i = 1; i < H_COUNT; i++) if (PtInRect(&g_hit[i], pt)) return i;
    return H_NONE;
}

static void click(int id)
{
    switch (id)
    {
    case H_OPEN: open_dialog(); break;
    case H_PLAY: play_pause(); break;
    case H_STOP: stop(); break;
    case H_PREV: go(-1); break;
    case H_NEXT: go(1); break;
    case H_MUTE: g_muted = !g_muted; apply_volume(); save_settings(); break;
    case H_FULL: set_fullscreen(!g_fullscreen); break;
    }
    InvalidateRect(g_main, NULL, FALSE);
}

static BOOL key(WPARAM vk)
{
    BOOL ctrl = GetKeyState(VK_CONTROL) < 0;
    switch (vk)
    {
    case VK_SPACE: case VK_MEDIA_PLAY_PAUSE: play_pause(); return TRUE;
    case 'P': if (ctrl) { play_pause(); return TRUE; } return FALSE;
    case 'S': if (ctrl) { stop(); return TRUE; } return FALSE;
    case VK_MEDIA_STOP: stop(); return TRUE;
    case 'B': if (ctrl) { go(-1); return TRUE; } return FALSE;
    case 'F': if (ctrl) { go(1); return TRUE; } return FALSE;
    case VK_MEDIA_PREV_TRACK: go(-1); return TRUE;
    case VK_MEDIA_NEXT_TRACK: go(1); return TRUE;
    case 'O': if (ctrl) { open_dialog(); return TRUE; } return FALSE;
    case VK_LEFT: seek_to(g_position - (ctrl ? 600000000LL : 50000000LL)); return TRUE;
    case VK_RIGHT: seek_to(g_position + (ctrl ? 600000000LL : 50000000LL)); return TRUE;
    case VK_UP: set_volume(g_volume + 5); return TRUE;
    case VK_DOWN: set_volume(g_volume - 5); return TRUE;
    case 'M': case VK_F7: case VK_VOLUME_MUTE: g_muted = !g_muted; apply_volume(); save_settings(); InvalidateRect(g_main, NULL, FALSE); return TRUE;
    case VK_F11: set_fullscreen(!g_fullscreen); return TRUE;
    case VK_RETURN: if (GetKeyState(VK_MENU) < 0) { set_fullscreen(!g_fullscreen); return TRUE; } return FALSE;
    case VK_ESCAPE: if (g_fullscreen) { set_fullscreen(FALSE); return TRUE; } return FALSE;
    }
    return FALSE;
}

static LRESULT CALLBACK video_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_ERASEBKGND:
    {
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect((HDC)wp, &r, GetStockObject(BLACK_BRUSH));
        return 1;
    }
    case WM_SIZE: place_video(); return 0;
    case WM_LBUTTONDBLCLK: set_fullscreen(!g_fullscreen); return 0;
    case WM_LBUTTONDOWN: SetFocus(g_main); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (sg_mode_changed(msg, lp)) sgm_follow(hwnd);
    switch (msg)
    {
    case WM_CREATE:
        g_main = hwnd;
        g_video = CreateWindowExW(0, CLASS_VIDEO, NULL, WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                                  hwnd, NULL, g_inst, NULL);
        DragAcceptFiles(hwnd, TRUE);
        SetTimer(hwnd, TIMER_TICK, 200, NULL);
        return 0;
    case WM_SIZE:
        layout();
        place_video();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(500);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = S(320);
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint(hwnd); return 0;
    case WM_TIMER:
        if (g_seek && g_state != ST_CLOSED)
        {
            LONGLONG p;
            if (SUCCEEDED(IMediaSeeking_GetCurrentPosition(g_seek, &p))) g_position = p;
            if (!g_duration) IMediaSeeking_GetDuration(g_seek, &g_duration);
            if (!g_fullscreen) InvalidateRect(hwnd, &g_bar, FALSE);
        }
        write_dump();
        return 0;
    case WM_GRAPHNOTIFY: handle_graph_events(); return 0;
    case WM_MOUSEMOVE:
    {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), h = hit_test(x, y);
        if (g_dragging_seek) { g_drag_pos = pos_from_x(x); InvalidateRect(hwnd, &g_bar, FALSE); return 0; }
        if (g_dragging_vol) { set_volume(vol_from_x(x)); return 0; }
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
        if (g_hot != H_NONE) { g_hot = H_NONE; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN:
    {
        int x = GET_X_LPARAM(lp), h = hit_test(x, GET_Y_LPARAM(lp));
        SetFocus(hwnd);
        if (h == H_SEEK && g_state != ST_CLOSED) { g_dragging_seek = TRUE; g_drag_pos = pos_from_x(x); SetCapture(hwnd); }
        else if (h == H_VOL) { g_dragging_vol = TRUE; SetCapture(hwnd); set_volume(vol_from_x(x)); }
        else if (h != H_NONE) { g_pressed = h; SetCapture(hwnd); }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        int h = hit_test(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        ReleaseCapture();
        if (g_dragging_seek) { g_dragging_seek = FALSE; seek_to(pos_from_x(GET_X_LPARAM(lp))); }
        else if (g_dragging_vol) g_dragging_vol = FALSE;
        else if (g_pressed != H_NONE) { int p = g_pressed; g_pressed = H_NONE; if (p == h) click(h); }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
    {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (PtInRect(&g_content, pt) && g_has_video) set_fullscreen(!g_fullscreen);
        else if (hit_test(pt.x, pt.y) != H_NONE) SendMessageW(hwnd, WM_LBUTTONDOWN, wp, lp);
        return 0;
    }
    case WM_MOUSEWHEEL:
        set_volume(g_volume + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 5 : -5));
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (key(wp)) return 0;
        break;
    case WM_APPCOMMAND:
        switch (GET_APPCOMMAND_LPARAM(lp))
        {
        case APPCOMMAND_MEDIA_PLAY_PAUSE: play_pause(); return TRUE;
        case APPCOMMAND_MEDIA_STOP: stop(); return TRUE;
        case APPCOMMAND_MEDIA_NEXTTRACK: go(1); return TRUE;
        case APPCOMMAND_MEDIA_PREVIOUSTRACK: go(-1); return TRUE;
        }
        break;
    case WM_DROPFILES:
    {
        HDROP drop = (HDROP)wp;
        UINT i, n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        WCHAR **files = calloc(n ? n : 1, sizeof(*files)), path[MAX_PATH];
        int k = 0;
        for (i = 0; i < n; i++) if (DragQueryFileW(drop, i, path, MAX_PATH)) files[k++] = _wcsdup(path);
        DragFinish(drop);
        open_files(files, k);
        while (k--) free(files[k]);
        free(files);
        SetForegroundWindow(hwnd);
        return 0;
    }
    case WM_COPYDATA:
    {
        COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp;
        if (cd->dwData == COPYDATA_FILES && cd->cbData >= sizeof(WCHAR))
        {
            WCHAR *cmd = malloc(cd->cbData + sizeof(WCHAR)), **files;
            int n;
            memcpy(cmd, cd->lpData, cd->cbData);
            cmd[cd->cbData / sizeof(WCHAR)] = 0;
            n = files_from_cmdline(cmd, TRUE, &files);
            open_files(files, n);
            while (n--) free(files[n]);
            free(files);
            free(cmd);
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            return TRUE;
        }
        return FALSE;
    }
    case WM_DESTROY:
        close_media();
        KillTimer(hwnd, TIMER_TICK);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HFONT make_font(int pt10, int weight)
{
    return CreateFontW(-MulDiv(pt10, g_dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    HWND other;
    HDC screen;
    MSG msg;
    WCHAR **files;
    int n;
    HANDLE mutex;

    (void)prev; (void)cmdline;
    g_inst = inst;
    SetProcessDPIAware();

    /* one Media Player: a second hands its files over and leaves */
    mutex = CreateMutexW(NULL, FALSE, L"Local\\StainedGlassMediaPlayer");
    if (GetLastError() == ERROR_ALREADY_EXISTS && !GetEnvironmentVariableW(L"SG_MEDIA_NEW_WINDOW", NULL, 0))
    {
        int i;
        for (i = 0; i < 50 && !(other = FindWindowW(CLASS_MAIN, NULL)); i++) Sleep(100);
        if (other)
        {
            const WCHAR *cl = GetCommandLineW();
            COPYDATASTRUCT cd = { COPYDATA_FILES, (DWORD)((lstrlenW(cl) + 1) * sizeof(WCHAR)), (void *)cl };
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid);
            SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cd);
            return 0;
        }
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (!GetEnvironmentVariableW(L"SG_MEDIA_DUMP", g_dump_path, MAX_PATH)) g_dump_path[0] = 0;
    screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    g_font = make_font(100, FW_NORMAL);
    g_font_small = make_font(90, FW_NORMAL);
    g_font_title = make_font(105, FW_SEMIBOLD);
    g_font_big = make_font(200, FW_SEMIBOLD);
    load_settings();

    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                            GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = CLASS_MAIN;
    wc.style = CS_DBLCLKS;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = video_proc;
    wc.lpszClassName = CLASS_VIDEO;
    wc.hIcon = wc.hIconSm = NULL;
    wc.hbrBackground = GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    sgm_dark = sg_apps_dark();
    CreateWindowExW(WS_EX_ACCEPTFILES, CLASS_MAIN, APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                    CW_USEDEFAULT, S(860), S(600), NULL, NULL, inst, NULL);
    if (!g_main) return 1;
    sg_mode_title(g_main, sgm_dark);
    update_title();
    ShowWindow(g_main, show);
    UpdateWindow(g_main);

    n = files_from_cmdline(GetCommandLineW(), TRUE, &files);
    open_files(files, n);
    while (n-- > 0) free(files[n]);
    free(files);
    write_dump();

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    save_settings();
    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return 0;
}
