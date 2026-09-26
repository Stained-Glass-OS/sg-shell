/* sg-snip -- Snipping Tool (and Snip & Sketch's screen clip) for Stained Glass OS.
 *
 *   sg-snip64.exe                 the Snipping Tool window: New, mode, delay
 *   sg-snip64.exe /clip           screen clip (Win+Shift+S, PrtScn, ms-screenclip:)
 *   sg-snip64.exe ms-screenclip:  the same, as the URI protocol hands it over
 *   sg-snip64.exe FILE            the editor, with that picture
 *
 * A screen clip first freezes the screen (a copy of the whole virtual screen,
 * taken before anything of ours is shown), then shows it dimmed in a topmost
 * window with the mode bar at the top centre: rectangle, free-form, window,
 * full screen, close. What is selected goes to the clipboard -- CF_DIB and a
 * "PNG" format; CF_BITMAP is synthesized from the DIB by the system -- and a
 * toast, "Snip saved to clipboard", opens it in the editor when clicked.
 *
 * The editor is the Snipping Tool window grown round the picture: pen,
 * highlighter, eraser (whole strokes, as Windows' does), crop, undo and redo,
 * copy, and save as PNG, JPEG, GIF or BMP through WIC, by default as
 * "Screenshot <date> <time>.png" in Pictures\Screenshots.
 *
 * SG_SNIP_DUMP=<file> writes the state after each change (UTF-8, key=value),
 * for the gate: what is shown, where its buttons are, the last result.
 *
 * Everything drawn here is our own: the glyphs are GDI shapes drawn at four
 * times their size and box-filtered, as the Control Panel's icons are.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commdlg.h>
#include <commctrl.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
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

#define ACCENT      RGB(112, 48, 192)
#define ACCENT_DARK RGB(90, 36, 160)
#define ACCENT_SOFT (sgm_dark ? RGB(62,46,86) : RGB(236, 226, 248))
#define SURFACE (sgm_dark ? RGB(32,32,32) : RGB(243, 243, 243))
#define BAR (sgm_dark ? RGB(43,43,43) : RGB(249, 249, 249))
#define LINE (sgm_dark ? RGB(64,64,64) : RGB(224, 224, 224))
#define COL_TEXT (sgm_dark ? RGB(255,255,255) : RGB(26, 26, 26))
#define COL_DIM (sgm_dark ? RGB(168,168,168) : RGB(96, 96, 96))
#define HOVER (sgm_dark ? RGB(58,58,58) : RGB(234, 234, 234))

#define MAIN_CLASS    L"SgSnippingTool"
#define OVERLAY_CLASS L"SgScreenClip"
#define TOAST_CLASS   L"SgSnipToast"
#define APP_NAME      L"Snipping Tool"

enum { M_RECT, M_FREE, M_WINDOW, M_FULL, M_COUNT };
static const WCHAR *const MODE_NAMES[M_COUNT] = { L"Rectangle", L"Free-form", L"Window", L"Full screen" };
static const WCHAR *const MODE_KEYS[M_COUNT]  = { L"rect", L"free", L"window", L"full" };

/* glyphs */
enum { G_RECT, G_FREE, G_WINDOW, G_FULL, G_CLOSE, G_NEW, G_CHEVRON, G_PEN, G_HIGHLIGHTER, G_ERASER,
       G_CROP, G_UNDO, G_REDO, G_SAVE, G_COPY, G_CLOCK, G_CHECK };

static HINSTANCE g_inst;
static int g_dpi = 96;
static HFONT g_font, g_font_bold, g_font_small;
static const WCHAR *g_dump_path;
static HICON g_icon, g_icon_small;
static HWND g_owner;                   /* never shown: owns the overlay and toast, so neither gets a taskbar button */

static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* ---------------------------------------------------------------------------------------------
 * pictures: 32 bpp top-down DIB sections
 */
typedef struct { int w, h; DWORD *px; HBITMAP bmp; } IMG;

static IMG *img_new(int w, int h)
{
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0 }, { { 0, 0, 0, 0 } } };
    IMG *im;
    void *bits;
    if (w <= 0 || h <= 0 || !(im = calloc(1, sizeof(*im)))) return NULL;
    if (!(im->bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0))) { free(im); return NULL; }
    im->w = w; im->h = h; im->px = bits;
    return im;
}

static void img_free(IMG *im) { if (im) { DeleteObject(im->bmp); free(im); } }

static IMG *img_crop(const IMG *src, RECT r)
{
    IMG *im;
    int y;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > src->w) r.right = src->w;
    if (r.bottom > src->h) r.bottom = src->h;
    if (!(im = img_new(r.right - r.left, r.bottom - r.top))) return NULL;
    GdiFlush();
    for (y = 0; y < im->h; y++)
        memcpy(im->px + (size_t)y * im->w, src->px + (size_t)(y + r.top) * src->w + r.left, (size_t)im->w * 4);
    return im;
}

static IMG *img_dup(const IMG *src) { RECT r = { 0, 0, src->w, src->h }; return img_crop(src, r); }

/* The whole virtual screen, as it is now. */
static IMG *capture_screen(POINT *origin)
{
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    HDC screen, mem;
    IMG *im;
    if (w <= 0 || h <= 0) { x = y = 0; w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN); }
    if (!(im = img_new(w, h))) return NULL;
    screen = GetDC(NULL);
    mem = CreateCompatibleDC(screen);
    SelectObject(mem, im->bmp);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    GdiFlush();
    origin->x = x; origin->y = y;
    return im;
}

/* ---------------------------------------------------------------------------------------------
 * glyphs: drawn black on white at SS times their size, box-filtered to coverage, blended in colour
 */
#define SS 4
static int g_gn;                                   /* the big canvas's size */
static int GP(double v) { return (int)floor(v * g_gn / 24.0 + 0.5); }

static HPEN gpen(double w)
{
    LOGBRUSH lb = { BS_SOLID, RGB(0, 0, 0), 0 };
    int pw = GP(w);
    return ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, pw > 0 ? pw : 1, &lb, 0, NULL);
}

static void gpoly(HDC dc, double w, const double *xy, int n, BOOL closed)
{
    POINT p[24];
    HPEN pen = gpen(w);
    HGDIOBJ op = SelectObject(dc, pen);
    int i;
    for (i = 0; i < n && i < 23; i++) { p[i].x = GP(xy[2 * i]); p[i].y = GP(xy[2 * i + 1]); }
    if (closed) p[i++] = p[0];
    Polyline(dc, p, i);
    SelectObject(dc, op); DeleteObject(pen);
}

static void gline(HDC dc, double w, double x1, double y1, double x2, double y2)
{
    double xy[4] = { x1, y1, x2, y2 };
    gpoly(dc, w, xy, 2, FALSE);
}

static void gfill(HDC dc, COLORREF c, const double *xy, int n)
{
    POINT p[24];
    HBRUSH br = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, GetStockObject(NULL_PEN));
    int i;
    for (i = 0; i < n && i < 24; i++) { p[i].x = GP(xy[2 * i]); p[i].y = GP(xy[2 * i + 1]); }
    Polygon(dc, p, i);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br);
}

static void grect(HDC dc, double w, double l, double t, double r, double b)
{
    double xy[8] = { l, t, r, t, r, b, l, b };
    gpoly(dc, w, xy, 4, TRUE);
}

static void gfillrect(HDC dc, COLORREF c, double l, double t, double r, double b)
{
    double xy[8] = { l, t, r, t, r, b, l, b };
    gfill(dc, c, xy, 4);
}

static void gellipse(HDC dc, double w, BOOL fill, double l, double t, double r, double b)
{
    HPEN pen = gpen(w);
    HGDIOBJ op = SelectObject(dc, fill ? GetStockObject(NULL_PEN) : pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(fill ? BLACK_BRUSH : NULL_BRUSH));
    Ellipse(dc, GP(l), GP(t), GP(r), GP(b));
    SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
}

static void garc(HDC dc, double w, double l, double t, double r, double b, double x1, double y1, double x2, double y2)
{
    HPEN pen = gpen(w);
    HGDIOBJ op = SelectObject(dc, pen);
    SetArcDirection(dc, AD_CLOCKWISE);
    Arc(dc, GP(l), GP(t), GP(r), GP(b), GP(x1), GP(y1), GP(x2), GP(y2));
    SelectObject(dc, op); DeleteObject(pen);
}

static void glyph_shapes(HDC dc, int id)
{
    const double W = 1.6;
    switch (id)
    {
    case G_RECT:
        grect(dc, W, 3.5, 5.5, 20.5, 18.5);
        gfillrect(dc, 0, 2, 4, 5.5, 7.5); gfillrect(dc, 0, 18.5, 4, 22, 7.5);
        gfillrect(dc, 0, 2, 16.5, 5.5, 20); gfillrect(dc, 0, 18.5, 16.5, 22, 20);
        break;
    case G_FREE: {
        double xy[] = { 5, 9, 9, 4.5, 15, 5, 20, 9, 18, 14, 20.5, 19, 13, 19.5, 6, 18, 3.5, 13 };
        gpoly(dc, W, xy, 9, TRUE);
        break; }
    case G_WINDOW:
        grect(dc, W, 3, 5, 21, 19);
        gfillrect(dc, 0, 3, 5, 21, 8.5);
        break;
    case G_FULL:
        grect(dc, W, 2.5, 4, 21.5, 16.5);
        gline(dc, W, 12, 16.5, 12, 20); gline(dc, W, 8, 20, 16, 20);
        gfillrect(dc, 0, 5.5, 7, 18.5, 13.5);
        break;
    case G_CLOSE:
        gline(dc, W, 6.5, 6.5, 17.5, 17.5); gline(dc, W, 17.5, 6.5, 6.5, 17.5);
        break;
    case G_NEW:
        gline(dc, 2.0, 12, 5, 12, 19); gline(dc, 2.0, 5, 12, 19, 12);
        break;
    case G_CHEVRON: {
        double xy[] = { 6.5, 9.5, 12, 15, 17.5, 9.5 };
        gpoly(dc, W, xy, 3, FALSE);
        break; }
    case G_PEN: {
        double body[] = { 7, 14, 16, 5, 19, 8, 10, 17 };
        double nib[] = { 7, 14, 10, 17, 4.5, 19.5 };
        gpoly(dc, W, body, 4, TRUE);
        gfill(dc, 0, nib, 3);
        break; }
    case G_HIGHLIGHTER: {
        double body[] = { 6, 12, 14, 4, 20, 10, 12, 18 };
        double tip[] = { 6, 12, 12, 18, 9, 19, 5, 15 };
        gpoly(dc, W, body, 4, TRUE);
        gfill(dc, 0, tip, 4);
        break; }
    case G_ERASER: {
        double body[] = { 3.5, 15, 12, 6.5, 19.5, 14, 13.5, 20, 8.5, 20 };
        double half[] = { 3.5, 15, 7.75, 10.75, 15.25, 18.25, 13.5, 20, 8.5, 20 };
        gpoly(dc, W, body, 5, TRUE);
        gfill(dc, 0, half, 5);
        gline(dc, W, 13.5, 20, 21, 20);
        break; }
    case G_CROP: {
        double a[] = { 7, 3, 7, 17, 21, 17 }, b[] = { 3, 7, 17, 7, 17, 21 };
        gpoly(dc, W, a, 3, FALSE); gpoly(dc, W, b, 3, FALSE);
        break; }
    case G_UNDO: case G_REDO: {
        double head[] = { 9, 5.5, 5, 9.5, 9, 13.5 };
        gpoly(dc, W, head, 3, FALSE);
        gline(dc, W, 5, 9.5, 14, 9.5);
        garc(dc, W, 9, 9.5, 20, 20.5, 14, 9.5, 14, 20.5);
        gline(dc, W, 14, 20.5, 8, 20.5);
        break; }
    case G_SAVE: {
        double body[] = { 4, 4, 17, 4, 20, 7, 20, 20, 4, 20 };
        gpoly(dc, W, body, 5, TRUE);
        gfillrect(dc, 0, 8, 4, 15, 9);
        grect(dc, W, 7.5, 13.5, 16.5, 20);
        break; }
    case G_COPY:
        grect(dc, W, 8.5, 3.5, 20, 16);
        gfillrect(dc, RGB(255, 255, 255), 3.2, 7.2, 16.3, 21.3);
        grect(dc, W, 4, 8, 15.5, 20.5);
        break;
    case G_CLOCK:
        gellipse(dc, W, FALSE, 4, 4, 20, 20);
        gline(dc, W, 12, 7.5, 12, 12); gline(dc, W, 12, 12, 15, 14);
        break;
    case G_CHECK: {
        double xy[] = { 5, 12.5, 10, 17.5, 19.5, 7 };
        gpoly(dc, 2.0, xy, 3, FALSE);
        break; }
    }
}

/* Draw glyph id, size px square at (x, y) on dc (a memory DC with a bitmap selected), in colour c. */
static void draw_glyph(HDC dc, int id, int x, int y, int size, COLORREF c)
{
    int big = size * SS, i, j, k, l;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), big, -big, 1, 32, BI_RGB, 0, 0, 0, 0, 0 }, { { 0, 0, 0, 0 } } };
    BITMAPINFO si = { { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB, 0, 0, 0, 0, 0 }, { { 0, 0, 0, 0 } } };
    DWORD *bp, *sp;
    HBITMAP bb, sb;
    HDC bdc = CreateCompatibleDC(dc), sdc = CreateCompatibleDC(dc);
    RECT all = { 0, 0, big, big };

    bb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bp, NULL, 0);
    sb = CreateDIBSection(dc, &si, DIB_RGB_COLORS, (void **)&sp, NULL, 0);
    if (!bb || !sb) goto done;
    SelectObject(bdc, bb);
    SelectObject(sdc, sb);
    FillRect(bdc, &all, GetStockObject(WHITE_BRUSH));
    g_gn = big;
    glyph_shapes(bdc, id == G_REDO ? G_UNDO : id);      /* redo is undo, mirrored below */
    BitBlt(sdc, 0, 0, size, size, dc, x, y, SRCCOPY);
    GdiFlush();
    for (j = 0; j < size; j++)
        for (i = 0; i < size; i++)
        {
            int cov = 0, a;
            DWORD d;
            for (l = 0; l < SS; l++)
                for (k = 0; k < SS; k++)
                    cov += 255 - (bp[(j * SS + l) * big + (id == G_REDO ? big - 1 - (i * SS + k) : i * SS + k)] & 0xff);
            a = cov / (SS * SS);
            if (!a) continue;
            d = sp[j * size + i];
#define MIX(sh, cv) ((((d >> (sh)) & 0xff) * (255 - a) + (cv) * a) / 255)
            sp[j * size + i] = (MIX(16, GetRValue(c)) << 16) | (MIX(8, GetGValue(c)) << 8) | MIX(0, GetBValue(c));
#undef MIX
        }
    BitBlt(dc, x, y, size, size, sdc, 0, 0, SRCCOPY);
done:
    DeleteDC(bdc); DeleteDC(sdc);
    if (bb) DeleteObject(bb);
    if (sb) DeleteObject(sb);
}

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, r, NULL, 0, NULL);
}

static void round_fill(HDC dc, const RECT *r, COLORREF c, COLORREF border, int radius)
{
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, radius, radius);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(br); DeleteObject(pen);
}

static void text(HDC dc, HFONT f, COLORREF c, const WCHAR *s, RECT *r, UINT flags)
{
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, r, flags | DT_NOPREFIX);
    SelectObject(dc, of);
}

/* ---------------------------------------------------------------------------------------------
 * the dump, for the gate
 */
static WCHAR g_state[32] = L"starting";
static int g_last_w, g_last_h;
static WCHAR g_saved[MAX_PATH];
static void dump(void);

static void set_state(const WCHAR *s) { lstrcpynW(g_state, s, 32); dump(); }

/* ---------------------------------------------------------------------------------------------
 * saving, the clipboard
 */
static const GUID *container_for(const WCHAR *path)
{
    const WCHAR *ext = wcsrchr(path, '.');
    if (ext && (!lstrcmpiW(ext, L".jpg") || !lstrcmpiW(ext, L".jpeg") || !lstrcmpiW(ext, L".jpe"))) return &GUID_ContainerFormatJpeg;
    if (ext && !lstrcmpiW(ext, L".gif")) return &GUID_ContainerFormatGif;
    if (ext && (!lstrcmpiW(ext, L".bmp") || !lstrcmpiW(ext, L".dib"))) return &GUID_ContainerFormatBmp;
    return &GUID_ContainerFormatPng;
}

/* Encode im into stream as container fmt. */
static HRESULT encode(const IMG *im, IStream *stream, const GUID *fmt)
{
    IWICImagingFactory *wic = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *props = NULL;
    IWICBitmap *src = NULL;
    IWICPalette *pal = NULL;
    IWICFormatConverter *conv = NULL;
    WICPixelFormatGUID pf = GUID_WICPixelFormat24bppBGR;
    BYTE *rgb = NULL;
    UINT stride = ((UINT)im->w * 3 + 3) & ~3u;
    HRESULT hr;
    int x, y;

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic);
    if (FAILED(hr)) return hr;
    if (!(rgb = malloc((size_t)stride * im->h))) { hr = E_OUTOFMEMORY; goto done; }
    GdiFlush();
    for (y = 0; y < im->h; y++)
        for (x = 0; x < im->w; x++)
        {
            DWORD p = im->px[(size_t)y * im->w + x];
            BYTE *d = rgb + (size_t)y * stride + x * 3;
            d[0] = p & 0xff; d[1] = (p >> 8) & 0xff; d[2] = (p >> 16) & 0xff;
        }
    if (FAILED(hr = IWICImagingFactory_CreateEncoder(wic, fmt, NULL, &enc))) goto done;
    if (FAILED(hr = IWICBitmapEncoder_Initialize(enc, stream, WICBitmapEncoderNoCache))) goto done;
    if (FAILED(hr = IWICBitmapEncoder_CreateNewFrame(enc, &frame, &props))) goto done;
    if (FAILED(hr = IWICBitmapFrameEncode_Initialize(frame, props))) goto done;
    if (FAILED(hr = IWICBitmapFrameEncode_SetSize(frame, im->w, im->h))) goto done;
    if (IsEqualGUID(fmt, &GUID_ContainerFormatGif))
    {
        /* GIF is palettized: a palette made from the picture, and a converter to it */
        WICRect all = { 0, 0, im->w, im->h };
        pf = GUID_WICPixelFormat8bppIndexed;
        if (FAILED(hr = IWICImagingFactory_CreateBitmapFromMemory(wic, im->w, im->h, &GUID_WICPixelFormat24bppBGR,
                                                                  stride, stride * im->h, rgb, &src))) goto done;
        if (FAILED(hr = IWICImagingFactory_CreatePalette(wic, &pal))) goto done;
        if (FAILED(hr = IWICPalette_InitializeFromBitmap(pal, (IWICBitmapSource *)src, 256, FALSE))) goto done;
        if (FAILED(hr = IWICImagingFactory_CreateFormatConverter(wic, &conv))) goto done;
        if (FAILED(hr = IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)src, &GUID_WICPixelFormat8bppIndexed,
                                                       WICBitmapDitherTypeNone, pal, 0.0, WICBitmapPaletteTypeCustom))) goto done;
        if (FAILED(hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &pf))) goto done;
        IWICBitmapFrameEncode_SetPalette(frame, pal);
        if (FAILED(hr = IWICBitmapFrameEncode_WriteSource(frame, (IWICBitmapSource *)conv, &all))) goto done;
    }
    else
    {
        if (FAILED(hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &pf))) goto done;
        if (!IsEqualGUID(&pf, &GUID_WICPixelFormat24bppBGR)) { hr = E_FAIL; goto done; }
        if (FAILED(hr = IWICBitmapFrameEncode_WritePixels(frame, im->h, stride, stride * im->h, rgb))) goto done;
    }
    if (FAILED(hr = IWICBitmapFrameEncode_Commit(frame))) goto done;
    hr = IWICBitmapEncoder_Commit(enc);
done:
    if (conv) IWICFormatConverter_Release(conv);
    if (pal) IWICPalette_Release(pal);
    if (src) IWICBitmap_Release(src);
    if (props) IPropertyBag2_Release(props);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (enc) IWICBitmapEncoder_Release(enc);
    IWICImagingFactory_Release(wic);
    free(rgb);
    return hr;
}

static HRESULT save_file(const IMG *im, const WCHAR *path)
{
    IWICImagingFactory *wic = NULL;
    IWICStream *stream = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic);
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = IWICImagingFactory_CreateStream(wic, &stream)) &&
        SUCCEEDED(hr = IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE)))
        hr = encode(im, (IStream *)stream, container_for(path));
    if (stream) IWICStream_Release(stream);
    IWICImagingFactory_Release(wic);
    if (FAILED(hr)) DeleteFileW(path);
    return hr;
}

/* Load a picture file (anything WIC reads) as 32 bpp. */
static IMG *load_file(const WCHAR *path)
{
    IWICImagingFactory *wic = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    IMG *im = NULL;
    UINT w, h;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic))) return NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(wic, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(wic, &conv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGR,
                                                 WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(IWICFormatConverter_GetSize(conv, &w, &h)) && (im = img_new(w, h)))
    {
        if (FAILED(IWICFormatConverter_CopyPixels(conv, NULL, w * 4, w * h * 4, (BYTE *)im->px))) { img_free(im); im = NULL; }
    }
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    IWICImagingFactory_Release(wic);
    return im;
}

static BOOL to_clipboard(HWND owner, const IMG *im)
{
    UINT stride = ((UINT)im->w * 3 + 3) & ~3u;
    SIZE_T size = sizeof(BITMAPINFOHEADER) + (SIZE_T)stride * im->h;
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, size), png = NULL;
    IStream *stream = NULL;
    BITMAPINFOHEADER *bi;
    BYTE *bits;
    int x, y, i;
    BOOL ok = FALSE;

    if (!dib || !(bi = GlobalLock(dib))) return FALSE;
    memset(bi, 0, sizeof(*bi));
    bi->biSize = sizeof(*bi); bi->biWidth = im->w; bi->biHeight = im->h;    /* bottom-up, as CF_DIB usually is */
    bi->biPlanes = 1; bi->biBitCount = 24; bi->biCompression = BI_RGB; bi->biSizeImage = stride * im->h;
    bits = (BYTE *)(bi + 1);
    GdiFlush();
    for (y = 0; y < im->h; y++)
    {
        BYTE *d = bits + (size_t)(im->h - 1 - y) * stride;
        for (x = 0; x < im->w; x++)
        {
            DWORD p = im->px[(size_t)y * im->w + x];
            d[x * 3] = p & 0xff; d[x * 3 + 1] = (p >> 8) & 0xff; d[x * 3 + 2] = (p >> 16) & 0xff;
        }
    }
    GlobalUnlock(dib);

    /* PNG as well: what browsers, chat programs and Office prefer */
    if (SUCCEEDED(CreateStreamOnHGlobal(NULL, FALSE, &stream)))
    {
        if (SUCCEEDED(encode(im, stream, &GUID_ContainerFormatPng))) GetHGlobalFromStream(stream, &png);
        IStream_Release(stream);
    }

    for (i = 0; i < 50 && !OpenClipboard(owner); i++) Sleep(50);
    if (i < 50)
    {
        EmptyClipboard();
#ifdef SG_MUTANT_NODIB
        GlobalFree(dib);
#else
        ok = SetClipboardData(CF_DIB, dib) != NULL;
#endif
        if (png && SetClipboardData(RegisterClipboardFormatW(L"PNG"), png)) png = NULL;
        CloseClipboard();
    }
    if (!ok) GlobalFree(dib);
    if (png) GlobalFree(png);
    return ok;
}

/* ---------------------------------------------------------------------------------------------
 * the editor's document: a base picture and ink strokes over it, with undo
 */
enum { T_NONE, T_PEN, T_HIGHLIGHTER, T_ERASER, T_CROP };
typedef struct { int tool; COLORREF color; int width; int n, cap; POINT *pts; } STROKE;
typedef struct { IMG *base; int n; STROKE **strokes; } SNAP;

static SNAP *g_hist;
static int g_nhist, g_cur = -1;
static IMG *g_comp;                     /* base with the strokes drawn: what is shown, copied, saved */
static STROKE *g_live;                  /* the stroke being drawn */
static BOOL g_dirty;

static SNAP *cur(void) { return g_cur >= 0 ? &g_hist[g_cur] : NULL; }

static void push_snap(IMG *base, STROKE **strokes, int n)
{
    SNAP *s;
    g_nhist = g_cur + 1;                /* what could be redone is gone */
    g_hist = realloc(g_hist, (g_nhist + 1) * sizeof(*g_hist));
    s = &g_hist[g_nhist++];
    s->base = base; s->n = n;
    s->strokes = n ? malloc(n * sizeof(*s->strokes)) : NULL;
    if (n) memcpy(s->strokes, strokes, n * sizeof(*s->strokes));
    g_cur = g_nhist - 1;
}

static void draw_stroke_gdi(HDC dc, const STROKE *st, COLORREF c)
{
    LOGBRUSH lb = { BS_SOLID, c, 0 };
    HPEN pen = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, st->width, &lb, 0, NULL);
    HGDIOBJ op = SelectObject(dc, pen);
    if (st->n == 1)
    {
        HBRUSH br = CreateSolidBrush(c);
        HGDIOBJ ob = SelectObject(dc, br), on = SelectObject(dc, GetStockObject(NULL_PEN));
        int r = st->width / 2 + 1;
        Ellipse(dc, st->pts[0].x - r, st->pts[0].y - r, st->pts[0].x + r + 1, st->pts[0].y + r + 1);
        SelectObject(dc, ob); SelectObject(dc, on); DeleteObject(br);
    }
    else Polyline(dc, st->pts, st->n);
    SelectObject(dc, op); DeleteObject(pen);
}

/* A highlighter stroke darkens what is under it to its colour (a per-channel minimum), once,
 * however often it crosses itself. */
static void apply_highlighter(IMG *im, HDC imdc, const STROKE *st)
{
    IMG *mask = img_new(im->w, im->h);
    HDC mdc;
    RECT box = { im->w, im->h, 0, 0 };
    int i, x, y, pad = st->width;
    if (!mask) return;
    mdc = CreateCompatibleDC(imdc);
    SelectObject(mdc, mask->bmp);
    memset(mask->px, 0, (size_t)mask->w * mask->h * 4);
    draw_stroke_gdi(mdc, st, RGB(255, 255, 255));
    GdiFlush();
    for (i = 0; i < st->n; i++)
    {
        box.left = min(box.left, st->pts[i].x - pad); box.top = min(box.top, st->pts[i].y - pad);
        box.right = max(box.right, st->pts[i].x + pad); box.bottom = max(box.bottom, st->pts[i].y + pad);
    }
    box.left = max(box.left, 0); box.top = max(box.top, 0);
    box.right = min(box.right, im->w); box.bottom = min(box.bottom, im->h);
    for (y = box.top; y < box.bottom; y++)
        for (x = box.left; x < box.right; x++)
            if (mask->px[(size_t)y * im->w + x] & 0xff)
            {
                DWORD *p = &im->px[(size_t)y * im->w + x];
                DWORD r = min((*p >> 16) & 0xff, GetRValue(st->color)), g = min((*p >> 8) & 0xff, GetGValue(st->color));
                DWORD b = min(*p & 0xff, GetBValue(st->color));
                *p = (r << 16) | (g << 8) | b;
            }
    DeleteDC(mdc);
    img_free(mask);
}

static void render_stroke(IMG *im, HDC dc, const STROKE *st)
{
    if (st->tool == T_HIGHLIGHTER) apply_highlighter(im, dc, st);
    else draw_stroke_gdi(dc, st, st->color);
}

static void rebuild_comp(void)
{
    SNAP *s = cur();
    HDC dc;
    int i;
    if (!s) return;
    if (!g_comp || g_comp->w != s->base->w || g_comp->h != s->base->h)
    {
        img_free(g_comp);
        g_comp = img_new(s->base->w, s->base->h);
        if (!g_comp) return;
    }
    GdiFlush();
    memcpy(g_comp->px, s->base->px, (size_t)g_comp->w * g_comp->h * 4);
    dc = CreateCompatibleDC(NULL);
    SelectObject(dc, g_comp->bmp);
    for (i = 0; i < s->n; i++) render_stroke(g_comp, dc, s->strokes[i]);
    if (g_live) render_stroke(g_comp, dc, g_live);
    DeleteDC(dc);
    GdiFlush();
}

static void doc_open(IMG *im)
{
    g_nhist = 0; g_cur = -1;            /* earlier snips' pictures are kept; they are small and few */
    push_snap(im, NULL, 0);
    g_dirty = FALSE;
    rebuild_comp();
}

/* the distance from p to segment a-b, squared */
static double seg_dist2(POINT p, POINT a, POINT b)
{
    double dx = b.x - a.x, dy = b.y - a.y, t = 0, x, y;
    if (dx || dy) t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / (dx * dx + dy * dy);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    x = a.x + t * dx - p.x; y = a.y + t * dy - p.y;
    return x * x + y * y;
}

/* The eraser takes the topmost stroke under it away. */
static BOOL erase_at(POINT p, int radius)
{
    SNAP *s = cur();
    int i, j;
    if (!s) return FALSE;
    for (i = s->n - 1; i >= 0; i--)
    {
        STROKE *st = s->strokes[i];
        double lim = st->width / 2.0 + radius;
        for (j = 0; j < st->n; j++)
            if (seg_dist2(p, st->pts[j], st->pts[j + 1 < st->n ? j + 1 : j]) <= lim * lim)
            {
                STROKE **rest = malloc(s->n * sizeof(*rest));
                int k, m = 0;
                for (k = 0; k < s->n; k++) if (k != i) rest[m++] = s->strokes[k];
                push_snap(s->base, rest, m);
                free(rest);
                return TRUE;
            }
    }
    return FALSE;
}

/* ---------------------------------------------------------------------------------------------
 * the screen clip overlay
 */
typedef void (*snip_done_fn)(IMG *result);     /* NULL: cancelled */

static struct
{
    HWND hwnd;
    IMG *cap, *dim;
    POINT origin;
    int mode;
    BOOL dragging, have_sel;
    POINT start, now;
    POINT *free_pts; int nfree, capfree;
    RECT *wins; int nwins; int hover;
    RECT buttons[5];                   /* the four modes and close, client coordinates */
    HWND before;                       /* what was in front when the snip began: it gets the keyboard back */
    int hot;
    snip_done_fn done;
} ov;

static void ov_layout(void)
{
    int bw = S(44), gap = S(4), n = 5, total = n * bw + (n - 1) * gap + S(12), i;
    int x = (ov.cap->w - total) / 2 + S(6), y = S(8) + S(6);
    for (i = 0; i < n; i++)
    {
        SetRect(&ov.buttons[i], x, y, x + bw, y + S(40));
        x += bw + gap;
    }
}

static RECT ov_bar_rect(void)
{
    RECT r = { ov.buttons[0].left - S(6), ov.buttons[0].top - S(6), ov.buttons[4].right + S(6), ov.buttons[4].bottom + S(6) };
    return r;
}

static void norm_rect(RECT *r, POINT a, POINT b)
{
    r->left = min(a.x, b.x); r->right = max(a.x, b.x);
    r->top = min(a.y, b.y); r->bottom = max(a.y, b.y);
}

static BOOL CALLBACK collect_window(HWND h, LPARAM l)
{
    RECT r;
    DWORD cloaked = 0;
    (void)l;
    if (h == ov.hwnd || !IsWindowVisible(h) || IsIconic(h)) return TRUE;
    if (!GetWindowRect(h, &r) || r.right - r.left < 8 || r.bottom - r.top < 8) return TRUE;
    {
        typedef HRESULT (WINAPI *dwm_fn)(HWND, DWORD, void *, DWORD);
        static dwm_fn fn; static BOOL looked;
        if (!looked) { HMODULE m = LoadLibraryW(L"dwmapi.dll"); fn = m ? (dwm_fn)(void *)GetProcAddress(m, "DwmGetWindowAttribute") : NULL; looked = TRUE; }
        if (fn && SUCCEEDED(fn(h, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
    }
    OffsetRect(&r, -ov.origin.x, -ov.origin.y);
    ov.wins = realloc(ov.wins, (ov.nwins + 1) * sizeof(*ov.wins));
    ov.wins[ov.nwins++] = r;
    return TRUE;
}

static int window_at(POINT p)
{
    int i;
    for (i = 0; i < ov.nwins; i++) if (PtInRect(&ov.wins[i], p)) return i;
    return -1;
}

static void ov_paint(HDC hdc)
{
    HDC mem = CreateCompatibleDC(hdc), src = CreateCompatibleDC(hdc);
    HBITMAP back = CreateCompatibleBitmap(hdc, ov.cap->w, ov.cap->h);
    RECT bar, sel;
    int i;

    SelectObject(mem, back);
    SelectObject(src, ov.dim->bmp);
    BitBlt(mem, 0, 0, ov.cap->w, ov.cap->h, src, 0, 0, SRCCOPY);
    SelectObject(src, ov.cap->bmp);

    /* what is being selected shows at full brightness */
    if (ov.mode == M_RECT && ov.dragging)
    {
        norm_rect(&sel, ov.start, ov.now);
        BitBlt(mem, sel.left, sel.top, sel.right - sel.left, sel.bottom - sel.top, src, sel.left, sel.top, SRCCOPY);
        {
            HGDIOBJ op = SelectObject(mem, GetStockObject(WHITE_PEN)), ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
            Rectangle(mem, sel.left - 1, sel.top - 1, sel.right + 1, sel.bottom + 1);
            SelectObject(mem, op); SelectObject(mem, ob);
        }
    }
    else if (ov.mode == M_FREE && ov.dragging && ov.nfree > 1)
    {
        HRGN rgn = CreatePolygonRgn(ov.free_pts, ov.nfree, WINDING);
        SelectClipRgn(mem, rgn);
        BitBlt(mem, 0, 0, ov.cap->w, ov.cap->h, src, 0, 0, SRCCOPY);
        SelectClipRgn(mem, NULL);
        DeleteObject(rgn);
        {
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
            HGDIOBJ op = SelectObject(mem, pen);
            Polyline(mem, ov.free_pts, ov.nfree);
            SelectObject(mem, op); DeleteObject(pen);
        }
    }
    else if (ov.mode == M_WINDOW && ov.hover >= 0)
    {
        RECT w = ov.wins[ov.hover];
        HPEN pen = CreatePen(PS_SOLID, S(2), ACCENT);
        HGDIOBJ op, ob;
        BitBlt(mem, w.left, w.top, w.right - w.left, w.bottom - w.top, src, w.left, w.top, SRCCOPY);
        op = SelectObject(mem, pen); ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
        Rectangle(mem, w.left, w.top, w.right, w.bottom);
        SelectObject(mem, op); SelectObject(mem, ob); DeleteObject(pen);
    }

    /* the mode bar, hidden while dragging */
    if (!ov.dragging)
    {
        bar = ov_bar_rect();
        round_fill(mem, &bar, (sgm_dark ? RGB(43, 43, 43) : RGB(255, 255, 255)), LINE, S(8));
        for (i = 0; i < 5; i++)
        {
            RECT b = ov.buttons[i];
            int gs = S(20);
            BOOL on = i < 4 && i == ov.mode;
            if (on) round_fill(mem, &b, ACCENT_SOFT, ACCENT_SOFT, S(6));
            else if (i == ov.hot) round_fill(mem, &b, HOVER, HOVER, S(6));
            if (i == 4)
            {
                /* a separator before close */
                RECT sep = { b.left - S(3), b.top + S(8), b.left - S(2), b.bottom - S(8) };
                fill(mem, &sep, LINE);
            }
            draw_glyph(mem, i == 4 ? G_CLOSE : (i == M_RECT ? G_RECT : i == M_FREE ? G_FREE : i == M_WINDOW ? G_WINDOW : G_FULL),
                       (b.left + b.right - gs) / 2, (b.top + b.bottom - gs) / 2, gs, on ? ACCENT : COL_TEXT);
        }
    }
    BitBlt(hdc, 0, 0, ov.cap->w, ov.cap->h, mem, 0, 0, SRCCOPY);
    DeleteDC(src); DeleteDC(mem); DeleteObject(back);
}

static void ov_finish(IMG *result)
{
    snip_done_fn done = ov.done;
    HWND h = ov.hwnd;
    ov.hwnd = NULL;
    /* Give the keyboard back before the overlay goes: under Wine the focused window's going leaves
     * no window with the keyboard at all, and nothing -- not even activating ours later -- brings
     * it back. On Windows, too, the program the snip interrupted is in front again afterwards. */
#ifndef SG_MUTANT_NOREFOCUS
    if (ov.before && IsWindow(ov.before) && IsWindowVisible(ov.before)) SetForegroundWindow(ov.before);
#endif
    ShowWindow(h, SW_HIDE);
    DestroyWindow(h);
    img_free(ov.cap); img_free(ov.dim);
    ov.cap = ov.dim = NULL;
    free(ov.free_pts); ov.free_pts = NULL; ov.nfree = ov.capfree = 0;
    free(ov.wins); ov.wins = NULL; ov.nwins = 0;
    if (done) done(result);
}

static void ov_select_rect(RECT r)
{
    IMG *res;
#ifdef SG_MUTANT_OFFSET
    OffsetRect(&r, 1, 1);
#endif
    if (r.right - r.left < 1 || r.bottom - r.top < 1) return;
    res = img_crop(ov.cap, r);
    ov_finish(res);
}

static void ov_select_free(void)
{
    RECT box = { ov.cap->w, ov.cap->h, 0, 0 };
    HRGN rgn;
    IMG *res;
    int i, x, y;
    if (ov.nfree < 3) return;
    for (i = 0; i < ov.nfree; i++)
    {
        box.left = min(box.left, ov.free_pts[i].x); box.top = min(box.top, ov.free_pts[i].y);
        box.right = max(box.right, ov.free_pts[i].x + 1); box.bottom = max(box.bottom, ov.free_pts[i].y + 1);
    }
    if (box.right - box.left < 2 || box.bottom - box.top < 2) return;
    rgn = CreatePolygonRgn(ov.free_pts, ov.nfree, WINDING);
    if (!(res = img_crop(ov.cap, box))) { DeleteObject(rgn); return; }
    /* outside the shape is white, as a free-form snip is on Windows */
    for (y = 0; y < res->h; y++)
        for (x = 0; x < res->w; x++)
            if (!PtInRegion(rgn, box.left + x, box.top + y)) res->px[(size_t)y * res->w + x] = 0xffffff;
    DeleteObject(rgn);
    ov_finish(res);
}

static void dump_overlay(FILE *f);

static LRESULT CALLBACK ov_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    int i;
    switch (m)
    {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (ov.cap) ov_paint(dc);
        EndPaint(h, &ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT)
        {
            POINT c;
            GetCursorPos(&c); ScreenToClient(h, &c);
            for (i = 0; i < 5 && !ov.dragging; i++) if (PtInRect(&ov.buttons[i], c)) { SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_ARROW)); return TRUE; }
            SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_CROSS));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        for (i = 0; i < 5; i++)
            if (PtInRect(&ov.buttons[i], p))
            {
                if (i == 4) { ov_finish(NULL); return 0; }
                if (i == M_FULL) { RECT all = { 0, 0, ov.cap->w, ov.cap->h }; ov.mode = M_FULL; ov_select_rect(all); return 0; }
                ov.mode = i; ov.hover = -1;
                InvalidateRect(h, NULL, FALSE);
                dump();
                return 0;
            }
        if (ov.mode == M_WINDOW)
        {
            int k = window_at(p);
            if (k >= 0) { RECT r = ov.wins[k], all = { 0, 0, ov.cap->w, ov.cap->h }; IntersectRect(&r, &r, &all); ov_select_rect(r); }
            return 0;
        }
        SetCapture(h);
        ov.dragging = TRUE;
        ov.start = ov.now = p;
        ov.nfree = 0;
        if (ov.mode == M_FREE)
        {
            ov.capfree = 256;
            ov.free_pts = realloc(ov.free_pts, ov.capfree * sizeof(POINT));
            ov.free_pts[ov.nfree++] = p;
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE:
        if (ov.dragging)
        {
            ov.now = p;
            if (ov.mode == M_FREE)
            {
                if (ov.nfree == ov.capfree) { ov.capfree *= 2; ov.free_pts = realloc(ov.free_pts, ov.capfree * sizeof(POINT)); }
                ov.free_pts[ov.nfree++] = p;
            }
            InvalidateRect(h, NULL, FALSE);
        }
        else
        {
            int hot = -1;
            for (i = 0; i < 5; i++) if (PtInRect(&ov.buttons[i], p)) hot = i;
            if (hot != ov.hot) { ov.hot = hot; InvalidateRect(h, NULL, FALSE); }
            if (ov.mode == M_WINDOW)
            {
                int k = hot >= 0 ? -1 : window_at(p);
                if (k != ov.hover) { ov.hover = k; InvalidateRect(h, NULL, FALSE); }
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (!ov.dragging) return 0;
        ReleaseCapture();
        ov.dragging = FALSE;
        ov.now = p;
        if (ov.mode == M_RECT)
        {
            RECT r;
            norm_rect(&r, ov.start, ov.now);
            if (r.right - r.left >= 1 && r.bottom - r.top >= 1) { ov_select_rect(r); return 0; }
        }
        else if (ov.mode == M_FREE) { ov_select_free(); if (!ov.hwnd) return 0; }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE)
        {
#ifdef SG_MUTANT_ESCAPE_COPIES
            RECT all = { 0, 0, ov.cap->w, ov.cap->h };
            ov_select_rect(all);
#else
            ov_finish(NULL);
#endif
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) SetFocus(h);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* Freeze the screen and let the user pick part of it; done() gets the result (or NULL). */
static BOOL start_overlay(int mode, snip_done_fn done)
{
    IMG *cap;
    int i, n;
    if (ov.hwnd) return FALSE;
    ov.before = GetForegroundWindow();
    if (!(cap = capture_screen(&ov.origin))) return FALSE;
    ov.cap = cap;
    ov.done = done;
    ov.mode = mode;
    ov.hot = ov.hover = -1;
    ov.dragging = FALSE;
    if (mode == M_FULL)
    {
        ov.cap = NULL;
        done(cap);
        return TRUE;
    }
    /* the dimmed copy the overlay shows */
    if (!(ov.dim = img_dup(cap))) { img_free(cap); ov.cap = NULL; return FALSE; }
    n = cap->w * cap->h;
    for (i = 0; i < n; i++)
    {
        DWORD p = ov.dim->px[i];
        ov.dim->px[i] = ((((p >> 16) & 0xff) * 50 / 100) << 16) | ((((p >> 8) & 0xff) * 50 / 100) << 8) | ((p & 0xff) * 50 / 100);
    }
    ov_layout();
    ov.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, OVERLAY_CLASS, L"Screen snip", WS_POPUP,
                              ov.origin.x, ov.origin.y, cap->w, cap->h, g_owner, NULL, g_inst, NULL);
    /* the windows window mode can pick, topmost first, as they were when the screen froze */
    ov.nwins = 0;
    EnumWindows(collect_window, 0);
    ShowWindow(ov.hwnd, SW_SHOW);
    UpdateWindow(ov.hwnd);
    SetForegroundWindow(ov.hwnd);
    SetFocus(ov.hwnd);
    set_state(L"overlay");
    return TRUE;
}

/* ---------------------------------------------------------------------------------------------
 * the Snipping Tool window and editor
 */
enum { B_NEW, B_MODE, B_DELAY, B_PEN, B_HIGHLIGHTER, B_ERASER, B_CROP, B_UNDO, B_REDO, B_SAVE, B_COPY,
       B_APPLY, B_CANCEL, B_COUNT };
static const WCHAR *const BTN_KEYS[B_COUNT] = { L"new", L"mode", L"delay", L"pen", L"highlighter", L"eraser", L"crop",
                                                L"undo", L"redo", L"save", L"copy", L"apply", L"cancel" };
static const WCHAR *const BTN_TIPS[B_COUNT] = { L"New snip (Ctrl+N)", L"Snipping mode", L"Delay", L"Ballpoint pen",
                                                L"Highlighter", L"Eraser", L"Image crop", L"Undo (Ctrl+Z)",
                                                L"Redo (Ctrl+Y)", L"Save as (Ctrl+S)", L"Copy (Ctrl+C)", L"Apply", L"Cancel" };

static const COLORREF PEN_COLORS[] = { RGB(0, 0, 0), RGB(232, 17, 35), RGB(0, 120, 215), RGB(16, 124, 16), ACCENT, RGB(255, 255, 255) };
static const WCHAR *const PEN_COLOR_NAMES[] = { L"Black", L"Red", L"Blue", L"Green", L"Purple", L"White" };
static const COLORREF HL_COLORS[] = { RGB(255, 240, 0), RGB(80, 230, 80), RGB(255, 120, 200), RGB(80, 200, 255) };
static const WCHAR *const HL_COLOR_NAMES[] = { L"Yellow", L"Green", L"Pink", L"Aqua" };

static struct
{
    HWND hwnd;
    int mode, delay;                   /* the next snip's */
    int tool;
    int pen_color, hl_color;
    RECT btn[B_COUNT]; BOOL shown[B_COUNT];
    int hot, pressed;
    RECT canvas;                        /* where the picture is drawn, client coordinates */
    double scale;
    BOOL cropping; RECT crop; int crop_drag; POINT crop_from; RECT crop_orig;
    BOOL inking;
    HWND tip;
} ed;

static BOOL g_clip_only;               /* started with /clip: no window until the toast is clicked */
static void toast_show(IMG *im);
static void editor_show_picture(void);

static BOOL has_doc(void) { return cur() != NULL; }

static void ed_layout(void)
{
    RECT c;
    int h = S(48), bw = S(40), x, i, y = S(4);
    GetClientRect(ed.hwnd, &c);
    memset(ed.shown, 0, sizeof(ed.shown));
    /* left: New (a wide accent button), mode, delay */
    SetRect(&ed.btn[B_NEW], S(8), y, S(8) + S(84), y + bw); ed.shown[B_NEW] = TRUE;
    SetRect(&ed.btn[B_MODE], ed.btn[B_NEW].right + S(8), y, ed.btn[B_NEW].right + S(8) + S(56), y + bw); ed.shown[B_MODE] = TRUE;
    SetRect(&ed.btn[B_DELAY], ed.btn[B_MODE].right + S(4), y, ed.btn[B_MODE].right + S(4) + S(56), y + bw); ed.shown[B_DELAY] = TRUE;
    if (has_doc())
    {
        /* right: copy, save; centre: the ink tools, crop, undo, redo */
        x = c.right - S(8) - bw;
        SetRect(&ed.btn[B_COPY], x, y, x + bw, y + bw); ed.shown[B_COPY] = TRUE;
        x -= bw + S(2);
        SetRect(&ed.btn[B_SAVE], x, y, x + bw, y + bw); ed.shown[B_SAVE] = TRUE;
        if (ed.cropping)
        {
            /* cropping: the crop tool, Apply and Cancel in the middle, nothing else to press */
            int total = bw + S(12) + S(96) + S(4) + S(96), left = ed.btn[B_DELAY].right + S(16);
            x = max(left, (c.right - total) / 2);
            SetRect(&ed.btn[B_CROP], x, y, x + bw, y + bw); ed.shown[B_CROP] = TRUE;
            x += bw + S(12);
            SetRect(&ed.btn[B_APPLY], x, y, x + S(96), y + bw); ed.shown[B_APPLY] = TRUE;
            x += S(96) + S(4);
            SetRect(&ed.btn[B_CANCEL], x, y, x + S(96), y + bw); ed.shown[B_CANCEL] = TRUE;
            ed.shown[B_SAVE] = ed.shown[B_COPY] = FALSE;
        }
        else
        {
            int n = 6, total = n * bw + (n - 1) * S(2) + S(12);
            int left = ed.btn[B_DELAY].right + S(16), right = ed.btn[B_SAVE].left - S(16);
            x = (c.right - total) / 2;
            if (x < left) x = left;
            if (x + total > right) x = max(left, right - total);
            for (i = B_PEN; i <= B_REDO; i++)
            {
                SetRect(&ed.btn[i], x, y, x + bw, y + bw); ed.shown[i] = TRUE;
                x += bw + S(2) + (i == B_CROP ? S(12) : 0);
            }
        }
    }
    /* the picture, fitted (never enlarged) and centred below the bar */
    SetRect(&ed.canvas, 0, h, c.right, c.bottom);
    if (has_doc())
    {
        SNAP *s = cur();
        int aw = c.right - S(32), ah = c.bottom - h - S(32), w, hh;
        double sc = 1.0;
        if (aw < 1) aw = 1;
        if (ah < 1) ah = 1;
        if (s->base->w > aw) sc = (double)aw / s->base->w;
        if (s->base->h * sc > ah) sc = (double)ah / s->base->h;
        w = (int)(s->base->w * sc + 0.5); hh = (int)(s->base->h * sc + 0.5);
        ed.scale = sc;
        SetRect(&ed.canvas, (c.right - w) / 2, h + (c.bottom - h - hh) / 2, (c.right - w) / 2 + w, h + (c.bottom - h - hh) / 2 + hh);
    }
    if (ed.tip)
    {
        TOOLINFOW ti = { sizeof(ti) };
        ti.hwnd = ed.hwnd;
        for (i = 0; i < B_COUNT; i++)
        {
            ti.uId = i + 1;
            SendMessageW(ed.tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
            if (!ed.shown[i]) continue;
            ti.uFlags = TTF_SUBCLASS; ti.rect = ed.btn[i]; ti.lpszText = (WCHAR *)BTN_TIPS[i];
            SendMessageW(ed.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
    }
}

static POINT to_image(POINT p)
{
    POINT r;
    r.x = (int)floor((p.x - ed.canvas.left) / ed.scale);
    r.y = (int)floor((p.y - ed.canvas.top) / ed.scale);
    return r;
}

static POINT to_client(POINT p)
{
    POINT r = { ed.canvas.left + (int)floor(p.x * ed.scale + 0.5), ed.canvas.top + (int)floor(p.y * ed.scale + 0.5) };
    return r;
}

static void ed_button(HDC dc, int i)
{
    RECT b = ed.btn[i];
    BOOL active = (i == B_PEN && ed.tool == T_PEN) || (i == B_HIGHLIGHTER && ed.tool == T_HIGHLIGHTER) ||
                  (i == B_ERASER && ed.tool == T_ERASER) || (i == B_CROP && ed.cropping);
    BOOL disabled = (i == B_UNDO && g_cur <= 0) || (i == B_REDO && g_cur >= g_nhist - 1);
    COLORREF ink = disabled ? (sgm_dark ? RGB(100, 100, 100) : RGB(170, 170, 170)) : active ? (sgm_dark ? RGB(179, 139, 235) : ACCENT) : COL_TEXT;
    int gs = S(20), gy = (b.top + b.bottom - gs) / 2;

    if (i == B_NEW || i == B_APPLY)
    {
        RECT t = b;
        round_fill(dc, &b, ed.hot == i ? ACCENT_DARK : ACCENT, ed.hot == i ? ACCENT_DARK : ACCENT, S(8));
        draw_glyph(dc, i == B_NEW ? G_NEW : G_CHECK, b.left + S(10), gy, gs, RGB(255, 255, 255));
        t.left += S(10) + gs + S(6);
        text(dc, g_font_bold, RGB(255, 255, 255), i == B_NEW ? L"New" : L"Apply", &t, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        return;
    }
    if (active) round_fill(dc, &b, ACCENT_SOFT, ACCENT_SOFT, S(8));
    else if (ed.hot == i && !disabled) round_fill(dc, &b, HOVER, HOVER, S(8));
    switch (i)
    {
    case B_MODE: {
        static const int g[M_COUNT] = { G_RECT, G_FREE, G_WINDOW, G_FULL };
        draw_glyph(dc, g[ed.mode], b.left + S(6), gy, gs, ink);
        draw_glyph(dc, G_CHEVRON, b.right - S(6) - S(14), (b.top + b.bottom - S(14)) / 2, S(14), COL_DIM);
        break; }
    case B_DELAY: {
        WCHAR n[8];
        RECT t = b;
        draw_glyph(dc, G_CLOCK, b.left + S(6), gy, gs, ink);
        if (ed.delay)
        {
            swprintf(n, 8, L"%d", ed.delay);
            t.left = b.left + S(6) + gs; t.right = b.right - S(14) - S(4);
            text(dc, g_font_small, COL_TEXT, n, &t, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        }
        draw_glyph(dc, G_CHEVRON, b.right - S(4) - S(14), (b.top + b.bottom - S(14)) / 2, S(14), COL_DIM);
        break; }
    case B_CANCEL: {
        RECT t = b;
        round_fill(dc, &b, ed.hot == i ? HOVER : (sgm_dark ? RGB(43, 43, 43) : RGB(255, 255, 255)), LINE, S(8));
        draw_glyph(dc, G_CLOSE, b.left + S(10), gy, gs, COL_TEXT);
        t.left += S(10) + gs + S(6);
        text(dc, g_font, COL_TEXT, L"Cancel", &t, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        break; }
    default: {
        static const int g[B_COUNT] = { 0, 0, 0, G_PEN, G_HIGHLIGHTER, G_ERASER, G_CROP, G_UNDO, G_REDO, G_SAVE, G_COPY };
        draw_glyph(dc, g[i], (b.left + b.right - gs) / 2, gy - (i == B_PEN || i == B_HIGHLIGHTER ? S(3) : 0), gs, ink);
        if (i == B_PEN || i == B_HIGHLIGHTER)
        {
            /* the ink's colour, under the tool */
            COLORREF c = i == B_PEN ? PEN_COLORS[ed.pen_color] : HL_COLORS[ed.hl_color];
            RECT sw = { b.left + S(11), b.bottom - S(9), b.right - S(11), b.bottom - S(5) };
            round_fill(dc, &sw, c, c == RGB(255, 255, 255) ? LINE : c, S(3));
        }
        break; }
    }
}

static int handle_pos(int k, int lo, int hi, BOOL y);
static void ed_paint(HDC hdc)
{
    RECT c, bar, sep;
    HDC mem;
    HBITMAP back;
    int i;
    GetClientRect(ed.hwnd, &c);
    if (c.right <= 0 || c.bottom <= 0) return;
    mem = CreateCompatibleDC(hdc);
    back = CreateCompatibleBitmap(hdc, c.right, c.bottom);
    SelectObject(mem, back);
    fill(mem, &c, SURFACE);
    SetRect(&bar, 0, 0, c.right, S(48));
    fill(mem, &bar, BAR);
    SetRect(&sep, 0, S(48) - 1, c.right, S(48));
    fill(mem, &sep, LINE);
    for (i = 0; i < B_COUNT; i++) if (ed.shown[i]) ed_button(mem, i);

    if (has_doc() && g_comp)
    {
        HDC src = CreateCompatibleDC(hdc);
        RECT fr = ed.canvas;
        InflateRect(&fr, 1, 1);
        fill(mem, &fr, LINE);
        SelectObject(src, g_comp->bmp);
        if (ed.scale >= 0.999)
            BitBlt(mem, ed.canvas.left, ed.canvas.top, g_comp->w, g_comp->h, src, 0, 0, SRCCOPY);
        else
        {
            SetStretchBltMode(mem, HALFTONE);
            SetBrushOrgEx(mem, 0, 0, NULL);
            StretchBlt(mem, ed.canvas.left, ed.canvas.top, ed.canvas.right - ed.canvas.left, ed.canvas.bottom - ed.canvas.top,
                       src, 0, 0, g_comp->w, g_comp->h, SRCCOPY);
        }
        DeleteDC(src);
        if (ed.cropping)
        {
            /* outside the crop darkened, a frame and handles round it */
            POINT a = { ed.crop.left, ed.crop.top }, b = { ed.crop.right, ed.crop.bottom };
            RECT cr;
            HRGN all = CreateRectRgnIndirect(&ed.canvas), in;
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 120, 0 };
            HDC shade = CreateCompatibleDC(hdc);
            HBITMAP sb = CreateCompatibleBitmap(hdc, 1, 1);
            RECT one = { 0, 0, 1, 1 };
            int hs = S(4), k;
            a = to_client(a); b = to_client(b);
            SetRect(&cr, a.x, a.y, b.x, b.y);
            in = CreateRectRgnIndirect(&cr);
            CombineRgn(all, all, in, RGN_DIFF);
            SelectObject(shade, sb);
            fill(shade, &one, RGB(0, 0, 0));
            SelectClipRgn(mem, all);
            AlphaBlend(mem, ed.canvas.left, ed.canvas.top, ed.canvas.right - ed.canvas.left, ed.canvas.bottom - ed.canvas.top,
                       shade, 0, 0, 1, 1, bf);
            SelectClipRgn(mem, NULL);
            DeleteDC(shade); DeleteObject(sb); DeleteObject(all); DeleteObject(in);
            {
                HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
                HGDIOBJ op = SelectObject(mem, pen), ob = SelectObject(mem, GetStockObject(NULL_BRUSH));
                Rectangle(mem, cr.left, cr.top, cr.right, cr.bottom);
                SelectObject(mem, op); SelectObject(mem, ob); DeleteObject(pen);
            }
            for (k = 0; k < 8; k++)
            {
                int hx = handle_pos(k, cr.left, cr.right, FALSE), hy = handle_pos(k, cr.top, cr.bottom, TRUE);
                RECT hr;
                SetRect(&hr, hx - hs, hy - hs, hx + hs, hy + hs);
                round_fill(mem, &hr, RGB(255, 255, 255), ACCENT, S(3));
            }
        }
    }
    else
    {
        RECT t = { S(24), S(48) + S(14), c.right - S(24), c.bottom - S(8) };
        text(mem, g_font, COL_DIM, L"Choose a mode, then select New to snip part of your screen.\n"
                                    L"Press the Start key + Shift + S to start a snip from anywhere.", &t, DT_LEFT | DT_WORDBREAK);
    }
    BitBlt(hdc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
    DeleteDC(mem); DeleteObject(back);
}

static void ed_title(void)
{
    WCHAR t[MAX_PATH + 32];
    if (g_saved[0] && has_doc())
    {
        const WCHAR *n = wcsrchr(g_saved, '\\');
        swprintf(t, MAX_PATH + 32, L"%ls%ls - %ls", g_dirty ? L"*" : L"", n ? n + 1 : g_saved, APP_NAME);
    }
    else lstrcpyW(t, APP_NAME);
    SetWindowTextW(ed.hwnd, t);
}

static void ed_refresh(void)
{
    ed_layout();
    InvalidateRect(ed.hwnd, NULL, FALSE);
    ed_title();
    dump();
}

/* the window's size for the picture (or the compact bar without one) */
static void ed_fit_window(void)
{
    RECT work, want;
    int cw, ch;
    DWORD style = GetWindowLongW(ed.hwnd, GWL_STYLE), ex = GetWindowLongW(ed.hwnd, GWL_EXSTYLE);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (has_doc())
    {
        SNAP *s = cur();
        cw = max(S(640), s->base->w + S(32));
        ch = max(S(420), s->base->h + S(48) + S(32));
        cw = min(cw, (work.right - work.left) * 9 / 10);
        ch = min(ch, (work.bottom - work.top) * 9 / 10);
    }
    else { cw = S(560); ch = S(48) + S(76); }
    SetRect(&want, 0, 0, cw, ch);
    AdjustWindowRectEx(&want, style, FALSE, ex);
    cw = want.right - want.left; ch = want.bottom - want.top;
    SetWindowPos(ed.hwnd, NULL, work.left + (work.right - work.left - cw) / 2, work.top + (work.bottom - work.top - ch) / 2,
                 cw, ch, SWP_NOZORDER);
}

static void make_default_name(WCHAR *path, int cch)
{
    WCHAR *pics = NULL, dir[MAX_PATH];
    SYSTEMTIME t;
    GetLocalTime(&t);
    dir[0] = 0;
    if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Pictures, KF_FLAG_CREATE, NULL, &pics)))
    {
        swprintf(dir, MAX_PATH, L"%ls\\Screenshots", pics);
        CoTaskMemFree(pics);
        CreateDirectoryW(dir, NULL);
    }
    swprintf(path, cch, L"%ls%lsScreenshot %04d-%02d-%02d %02d%02d%02d.png", dir, dir[0] ? L"\\" : L"",
             t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

static void ed_save(void)
{
    WCHAR path[MAX_PATH], dir[MAX_PATH], *slash;
    OPENFILENAMEW ofn = { sizeof(ofn) };
    HRESULT hr;
    if (!has_doc() || !g_comp) return;
    make_default_name(path, MAX_PATH);
    lstrcpyW(dir, path);
    if ((slash = wcsrchr(dir, '\\'))) { *slash = 0; lstrcpyW(path, slash + 1); }
    ofn.hwndOwner = ed.hwnd;
    ofn.lpstrFilter = L"PNG (*.png)\0*.png\0JPEG (*.jpg)\0*.jpg;*.jpeg\0GIF (*.gif)\0*.gif\0BMP (*.bmp)\0*.bmp\0";
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir[0] && slash ? dir : NULL;
    ofn.lpstrDefExt = L"png";
    ofn.lpstrTitle = L"Save As";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return;
    /* the type chosen in the list decides when the name has no known extension */
    {
        const WCHAR *ext = wcsrchr(path, '.'), *slash2 = wcsrchr(path, '\\');
        static const WCHAR *const exts[] = { L".png", L".jpg", L".gif", L".bmp" };
        if ((!ext || (slash2 && ext < slash2)) && ofn.nFilterIndex >= 1 && ofn.nFilterIndex <= 4 &&
            wcslen(path) + 5 < MAX_PATH)
            lstrcatW(path, exts[ofn.nFilterIndex - 1]);
    }
    hr = save_file(g_comp, path);
    if (FAILED(hr))
    {
        WCHAR msg[MAX_PATH + 64];
        swprintf(msg, MAX_PATH + 64, L"The picture could not be saved to %ls (error 0x%08lx).", path, (unsigned long)hr);
        MessageBoxW(ed.hwnd, msg, APP_NAME, MB_ICONERROR);
        return;
    }
    lstrcpynW(g_saved, path, MAX_PATH);
    g_dirty = FALSE;
    ed_refresh();
}

static void ed_copy(void)
{
    if (has_doc() && g_comp) to_clipboard(ed.hwnd, g_comp);
}

static void ed_undo(int d)
{
    int n = g_cur + d;
    if (n < 0 || n >= g_nhist) return;
    g_cur = n;
    g_dirty = TRUE;
    rebuild_comp();
    ed_refresh();
}

static void ed_crop_begin(void)
{
    SNAP *s = cur();
    if (!s) return;
    ed.cropping = TRUE;
    SetRect(&ed.crop, 0, 0, s->base->w, s->base->h);
    ed.crop_drag = -1;
    ed_refresh();
}

static void ed_crop_end(BOOL apply)
{
    ed.cropping = FALSE;
    if (apply && g_comp && ed.crop.right - ed.crop.left >= 1 && ed.crop.bottom - ed.crop.top >= 1)
    {
        IMG *im = img_crop(g_comp, ed.crop);
        if (im)
        {
            push_snap(im, NULL, 0);
            g_dirty = TRUE;
            rebuild_comp();
            ed_fit_window();
        }
    }
    ed_refresh();
}

/* the crop frame's handle under p: 0-7 as painted, 8 inside, -1 nothing */
static const signed char HANDLE_X[8] = { 0, 1, 2, 0, 2, 0, 1, 2 }, HANDLE_Y[8] = { 0, 0, 0, 1, 1, 2, 2, 2 };
static int handle_pos(int k, int lo, int hi, BOOL y) { int v = y ? HANDLE_Y[k] : HANDLE_X[k]; return v == 0 ? lo : v == 1 ? (lo + hi) / 2 : hi; }

static int crop_hit(POINT p)
{
    POINT a = { ed.crop.left, ed.crop.top }, b = { ed.crop.right, ed.crop.bottom };
    int k, r = S(8);
    a = to_client(a); b = to_client(b);
    for (k = 0; k < 8; k++)
    {
        int hx = handle_pos(k, a.x, b.x, FALSE), hy = handle_pos(k, a.y, b.y, TRUE);
        if (abs(p.x - hx) <= r && abs(p.y - hy) <= r) return k;
    }
    if (p.x > a.x && p.x < b.x && p.y > a.y && p.y < b.y) return 8;
    return -1;
}

static void crop_drag_to(POINT p)
{
    SNAP *s = cur();
    POINT q = to_image(p);
    RECT r = ed.crop_orig;
    int k = ed.crop_drag;
    if (!s) return;
    q.x = max(0, min(s->base->w, q.x)); q.y = max(0, min(s->base->h, q.y));
    if (k == 8)
    {
        POINT f = to_image(ed.crop_from);
        int dx = q.x - f.x, dy = q.y - f.y;
        dx = max(-r.left, min(s->base->w - r.right, dx));
        dy = max(-r.top, min(s->base->h - r.bottom, dy));
        OffsetRect(&r, dx, dy);
    }
    else if (k == 9) norm_rect(&r, to_image(ed.crop_from), q);   /* a new frame, dragged out */
    else
    {
        if (k == 0 || k == 3 || k == 5) r.left = min(q.x, r.right - 1);
        if (k == 2 || k == 4 || k == 7) r.right = max(q.x, r.left + 1);
        if (k == 0 || k == 1 || k == 2) r.top = min(q.y, r.bottom - 1);
        if (k == 5 || k == 6 || k == 7) r.bottom = max(q.y, r.top + 1);
    }
    r.left = max(0, r.left); r.top = max(0, r.top);
    r.right = min(s->base->w, r.right); r.bottom = min(s->base->h, r.bottom);
    ed.crop = r;
    InvalidateRect(ed.hwnd, NULL, FALSE);
}

static void new_snip(void);

static void popup_menu(int which)
{
    HMENU m = CreatePopupMenu();
    POINT p = { ed.btn[which].left, ed.btn[which].bottom };
    int i, cmd;
    if (which == B_MODE)
        for (i = 0; i < M_COUNT; i++) AppendMenuW(m, MF_STRING | (i == ed.mode ? MF_CHECKED : 0), 100 + i, MODE_NAMES[i]);
    else if (which == B_DELAY)
    {
        static const int d[] = { 0, 3, 5, 10 };
        for (i = 0; i < 4; i++)
        {
            WCHAR s[32];
            if (d[i]) swprintf(s, 32, L"Snip in %d seconds", d[i]); else lstrcpyW(s, L"No delay");
            AppendMenuW(m, MF_STRING | (d[i] == ed.delay ? MF_CHECKED : 0), 200 + d[i], s);
        }
    }
    else if (which == B_PEN)
        for (i = 0; i < (int)ARRAYSIZE(PEN_COLORS); i++)
            AppendMenuW(m, MF_STRING | (i == ed.pen_color ? MF_CHECKED : 0), 300 + i, PEN_COLOR_NAMES[i]);
    else
        for (i = 0; i < (int)ARRAYSIZE(HL_COLORS); i++)
            AppendMenuW(m, MF_STRING | (i == ed.hl_color ? MF_CHECKED : 0), 400 + i, HL_COLOR_NAMES[i]);
    ClientToScreen(ed.hwnd, &p);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, 0, ed.hwnd, NULL);
    DestroyMenu(m);
    if (cmd >= 100 && cmd < 100 + M_COUNT) ed.mode = cmd - 100;
    else if (cmd >= 200 && cmd <= 210) ed.delay = cmd - 200;
    else if (cmd >= 300 && cmd < 400) ed.pen_color = cmd - 300;
    else if (cmd >= 400 && cmd < 500) ed.hl_color = cmd - 400;
    ed.hot = -1;
    ed_refresh();
}

static void ed_click(int i)
{
    switch (i)
    {
    case B_NEW: new_snip(); break;
    case B_MODE: case B_DELAY: popup_menu(i); break;
    case B_PEN: case B_HIGHLIGHTER: {
        int t = i == B_PEN ? T_PEN : T_HIGHLIGHTER;
        if (ed.cropping) ed_crop_end(FALSE);
        if (ed.tool == t) popup_menu(i);        /* the tool again: its colours */
        else { ed.tool = t; ed_refresh(); }
        break; }
    case B_ERASER:
        if (ed.cropping) ed_crop_end(FALSE);
        ed.tool = ed.tool == T_ERASER ? T_NONE : T_ERASER;
        ed_refresh();
        break;
    case B_CROP: if (ed.cropping) ed_crop_end(FALSE); else ed_crop_begin(); break;
    case B_UNDO: ed_undo(-1); break;
    case B_REDO: ed_undo(1); break;
    case B_SAVE: ed_save(); break;
    case B_COPY: ed_copy(); break;
    case B_APPLY: ed_crop_end(TRUE); break;
    case B_CANCEL: ed_crop_end(FALSE); break;
    }
}

static int btn_at(POINT p)
{
    int i;
    for (i = 0; i < B_COUNT; i++) if (ed.shown[i] && PtInRect(&ed.btn[i], p)) return i;
    return -1;
}

static void ink_add(POINT p)
{
    if (!g_live) return;
    if (g_live->n && g_live->pts[g_live->n - 1].x == p.x && g_live->pts[g_live->n - 1].y == p.y) return;
    if (g_live->n == g_live->cap) { g_live->cap *= 2; g_live->pts = realloc(g_live->pts, g_live->cap * sizeof(POINT)); }
    g_live->pts[g_live->n++] = p;
}

static LRESULT CALLBACK ed_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (sg_mode_changed(m, l)) sgm_follow(h);
    POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    switch (m)
    {
    case WM_CREATE:
        ed.hwnd = h;
        ed.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                                 0, 0, 0, 0, h, NULL, g_inst, NULL);
        return 0;
    case WM_SIZE: ed_layout(); InvalidateRect(h, NULL, FALSE); return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)l;
        mm->ptMinTrackSize.x = S(520); mm->ptMinTrackSize.y = S(150);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        ed_paint(dc);
        EndPaint(h, &ps);
        return 0; }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && has_doc())
        {
            POINT c;
            GetCursorPos(&c); ScreenToClient(h, &c);
            if (btn_at(c) < 0 && PtInRect(&ed.canvas, c) && (ed.tool != T_NONE || ed.cropping))
            {
                int k = ed.cropping ? crop_hit(c) : -1;
                LPCWSTR cur = ed.cropping ? (k == 8 ? IDC_SIZEALL : k == 0 || k == 7 ? IDC_SIZENWSE : k == 2 || k == 5 ? IDC_SIZENESW :
                                             k == 1 || k == 6 ? IDC_SIZENS : k == 3 || k == 4 ? IDC_SIZEWE : IDC_CROSS) : IDC_CROSS;
                SetCursor(LoadCursorW(NULL, cur));
                return TRUE;
            }
        }
        break;
    case WM_MOUSEMOVE: {
        int hot = btn_at(p);
        if (hot != ed.hot) { ed.hot = hot; InvalidateRect(h, NULL, FALSE); }
        if (hot >= 0) { TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 }; TrackMouseEvent(&tme); }
        if (ed.cropping && ed.crop_drag >= 0) crop_drag_to(p);
        else if (ed.inking && g_live) { ink_add(to_image(p)); rebuild_comp(); InvalidateRect(h, &ed.canvas, FALSE); }
        else if (ed.inking && ed.tool == T_ERASER && erase_at(to_image(p), (int)(S(6) / ed.scale)))
        {
            g_dirty = TRUE; rebuild_comp(); ed_refresh();
        }
        return 0; }
    case WM_MOUSELEAVE: if (ed.hot >= 0) { ed.hot = -1; InvalidateRect(h, NULL, FALSE); } return 0;
    case WM_LBUTTONDOWN: {
        int b = btn_at(p);
        SetFocus(h);
        if (b >= 0) { ed.pressed = b; return 0; }
        if (!has_doc()) return 0;
        if (ed.cropping)
        {
            int k = crop_hit(p);
            if (k < 0 && !PtInRect(&ed.canvas, p)) return 0;
            ed.crop_drag = k < 0 ? 9 : k;
            ed.crop_from = p; ed.crop_orig = ed.crop;
            SetCapture(h);
            return 0;
        }
        if (!PtInRect(&ed.canvas, p)) return 0;
        if (ed.tool == T_PEN || ed.tool == T_HIGHLIGHTER)
        {
            g_live = calloc(1, sizeof(*g_live));
            g_live->tool = ed.tool;
            g_live->color = ed.tool == T_PEN ? PEN_COLORS[ed.pen_color] : HL_COLORS[ed.hl_color];
            g_live->width = ed.tool == T_PEN ? max(2, (int)(S(3) / ed.scale)) : max(8, (int)(S(16) / ed.scale));
            g_live->cap = 64; g_live->pts = malloc(64 * sizeof(POINT));
            ink_add(to_image(p));
            ed.inking = TRUE;
            SetCapture(h);
            rebuild_comp();
            InvalidateRect(h, &ed.canvas, FALSE);
        }
        else if (ed.tool == T_ERASER)
        {
            ed.inking = TRUE;
            SetCapture(h);
            if (erase_at(to_image(p), (int)(S(6) / ed.scale))) { g_dirty = TRUE; rebuild_comp(); ed_refresh(); }
        }
        return 0; }
    case WM_LBUTTONUP: {
        int b = btn_at(p), pressed = ed.pressed;
        ed.pressed = -1;
        if (GetCapture() == h) ReleaseCapture();
        if (ed.cropping && ed.crop_drag >= 0) { crop_drag_to(p); ed.crop_drag = -1; dump(); return 0; }
        if (ed.inking)
        {
            ed.inking = FALSE;
            if (g_live)
            {
                SNAP *s = cur();
                STROKE **all = malloc((s->n + 1) * sizeof(*all));
                if (s->n) memcpy(all, s->strokes, s->n * sizeof(*all));
                all[s->n] = g_live;
                g_live = NULL;
                push_snap(s->base, all, s->n + 1);
                free(all);
                g_dirty = TRUE;
                rebuild_comp();
                ed_refresh();
            }
            return 0;
        }
        if (b >= 0 && b == pressed) ed_click(b);
        return 0; }
    case WM_KEYDOWN: {
        BOOL ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl && w == 'N') new_snip();
        else if (ctrl && w == 'S') ed_save();
        else if (ctrl && w == 'C') ed_copy();
        else if (ctrl && w == 'Z') ed_undo(-1);
        else if (ctrl && w == 'Y') ed_undo(1);
        else if (ed.cropping && w == VK_RETURN) ed_crop_end(TRUE);
        else if (ed.cropping && w == VK_ESCAPE) ed_crop_end(FALSE);
        return 0; }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void editor_create(void)
{
    if (ed.hwnd) return;
    ed.hot = ed.pressed = -1;
    ed.tool = T_PEN;
    sgm_dark = sg_apps_dark();
    CreateWindowExW(0, MAIN_CLASS, APP_NAME, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(560), S(160),
                    NULL, NULL, g_inst, NULL);
    if (ed.hwnd) sg_mode_title(ed.hwnd, sgm_dark);
    SendMessageW(ed.hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_icon);
    SendMessageW(ed.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_icon_small);
}

static void editor_show_picture(void)
{
    editor_create();
    ed.cropping = FALSE;
    g_saved[0] = 0;
    ed_fit_window();
    ed_layout();
    ShowWindow(ed.hwnd, SW_SHOW);
    SetForegroundWindow(ed.hwnd);
    InvalidateRect(ed.hwnd, NULL, FALSE);
    UpdateWindow(ed.hwnd);
    set_state(L"editor");
    ed_title();
}

/* a snip from the window's New: the clipboard gets it too, as Windows' does */
static void snip_into_editor(IMG *res)
{
    if (!res)
    {
        if (ed.hwnd) { ShowWindow(ed.hwnd, SW_SHOW); SetForegroundWindow(ed.hwnd); }
        set_state(has_doc() ? L"editor" : L"tool");
        return;
    }
    g_last_w = res->w; g_last_h = res->h;
    to_clipboard(ed.hwnd, res);
    doc_open(res);
    editor_show_picture();
}

static void snip_to_clipboard(IMG *res)
{
    if (!res) { set_state(L"cancelled"); PostQuitMessage(0); return; }
    g_last_w = res->w; g_last_h = res->h;
    to_clipboard(NULL, res);
    doc_open(res);
    toast_show(res);
}

#define TIMER_DELAY 1
static void new_snip(void)
{
    if (ov.hwnd) return;
    if (ed.hwnd) ShowWindow(ed.hwnd, SW_HIDE);
    set_state(L"waiting");
    /* the delay, or a moment for the windows under ours to repaint */
    SetTimer(ed.hwnd, TIMER_DELAY, ed.delay ? ed.delay * 1000 : 250, NULL);
}

/* ---------------------------------------------------------------------------------------------
 * the toast
 */
static struct { HWND hwnd; IMG *thumb_src; BOOL hot; } toast;
#define TIMER_TOAST 2

static void toast_paint(HDC hdc)
{
    RECT c, r, t;
    HDC mem;
    HBITMAP back;
    int gs = S(16), tw, th;
    GetClientRect(toast.hwnd, &c);
    mem = CreateCompatibleDC(hdc);
    back = CreateCompatibleBitmap(hdc, c.right, c.bottom);
    SelectObject(mem, back);
    fill(mem, &c, toast.hot ? (sgm_dark ? RGB(58, 58, 58) : RGB(236, 236, 236)) : (sgm_dark ? RGB(43, 43, 43) : RGB(248, 248, 248)));
    r = c; r.right = r.left + 1; fill(mem, &r, LINE);
    r = c; r.bottom = r.top + 1; fill(mem, &r, LINE);
    r = c; r.left = r.right - 1; fill(mem, &r, LINE);
    r = c; r.top = r.bottom - 1; fill(mem, &r, LINE);
    /* the app's name with its mark */
    DrawIconEx(mem, S(12), S(10), g_icon_small, gs, gs, 0, NULL, DI_NORMAL);
    SetRect(&t, S(12) + gs + S(8), S(8), c.right - S(40), S(8) + S(20));
    text(mem, g_font_small, COL_DIM, APP_NAME, &t, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    draw_glyph(mem, G_CLOSE, c.right - S(12) - gs, S(10), gs, COL_DIM);
    /* the picture, small, on the right */
    tw = S(96); th = S(64);
    if (toast.thumb_src)
    {
        double sc = min((double)tw / toast.thumb_src->w, (double)th / toast.thumb_src->h);
        int w = max(1, (int)(toast.thumb_src->w * sc)), h = max(1, (int)(toast.thumb_src->h * sc));
        HDC src = CreateCompatibleDC(hdc);
        RECT fr;
        int x = c.right - S(12) - tw + (tw - w) / 2, y = S(36) + (th - h) / 2;
        SetRect(&fr, x - 1, y - 1, x + w + 1, y + h + 1);
        fill(mem, &fr, LINE);
        SelectObject(src, toast.thumb_src->bmp);
        SetStretchBltMode(mem, HALFTONE);
        SetBrushOrgEx(mem, 0, 0, NULL);
        StretchBlt(mem, x, y, w, h, src, 0, 0, toast.thumb_src->w, toast.thumb_src->h, SRCCOPY);
        DeleteDC(src);
    }
    SetRect(&t, S(12), S(36), c.right - S(24) - tw, S(36) + S(22));
    text(mem, g_font_bold, COL_TEXT, L"Snip saved to clipboard", &t, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
    SetRect(&t, S(12), S(58), c.right - S(24) - tw, c.bottom - S(8));
    text(mem, g_font_small, COL_DIM, L"Select here to mark up and share the image", &t, DT_LEFT | DT_WORDBREAK);
    BitBlt(hdc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
    DeleteDC(mem); DeleteObject(back);
}

static void toast_close(BOOL open_editor)
{
    HWND h = toast.hwnd;
    if (!h) return;
    toast.hwnd = NULL;
    KillTimer(h, TIMER_TOAST);
    DestroyWindow(h);
    if (open_editor) editor_show_picture();
    else if (!ed.hwnd || !IsWindowVisible(ed.hwnd)) { set_state(L"done"); PostQuitMessage(0); }
}

static LRESULT CALLBACK toast_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    POINT p = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    switch (m)
    {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        toast_paint(dc);
        EndPaint(h, &ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE:
        if (!toast.hot)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            toast.hot = TRUE; InvalidateRect(h, NULL, FALSE);
            KillTimer(h, TIMER_TOAST);          /* it stays while the pointer is on it */
        }
        return 0;
    case WM_MOUSELEAVE:
        toast.hot = FALSE; InvalidateRect(h, NULL, FALSE);
        SetTimer(h, TIMER_TOAST, 4000, NULL);
        return 0;
    case WM_LBUTTONUP: {
        RECT c, x;
        GetClientRect(h, &c);
        SetRect(&x, c.right - S(36), 0, c.right, S(36));
        toast_close(!PtInRect(&x, p));
        return 0; }
    case WM_TIMER: if (w == TIMER_TOAST) toast_close(FALSE); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void toast_show(IMG *im)
{
    RECT work, tray;
    HWND tw = FindWindowW(L"Shell_TrayWnd", NULL);
    int w = S(364), h = S(112);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    /* the work area may include the taskbar; stay above it */
    if (tw && IsWindowVisible(tw) && GetWindowRect(tw, &tray) && tray.top > work.top && tray.top < work.bottom &&
        tray.right - tray.left > tray.bottom - tray.top)
        work.bottom = tray.top;
    toast.thumb_src = im;
    toast.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, TOAST_CLASS, L"Snip saved to clipboard",
                                 WS_POPUP, work.right - w - S(16), work.bottom - h - S(16), w, h, g_owner, NULL, g_inst, NULL);
    ShowWindow(toast.hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(toast.hwnd);
    SetTimer(toast.hwnd, TIMER_TOAST, 7000, NULL);
    set_state(L"toast");
}

/* ---------------------------------------------------------------------------------------------
 * the dump
 */
static void put_rect(FILE *f, const char *key, const WCHAR *name, HWND h, RECT r)
{
    POINT a = { r.left, r.top }, b = { r.right, r.bottom };
    if (h) { ClientToScreen(h, &a); ClientToScreen(h, &b); }
    fprintf(f, "%s%s%ls=%ld %ld %ld %ld\n", key, name ? "_" : "", name ? name : L"", a.x, a.y, b.x, b.y);
}

static void dump_overlay(FILE *f)
{
    int i;
    fprintf(f, "overlay_mode=%ls\n", MODE_KEYS[ov.mode]);
    for (i = 0; i < 5; i++) put_rect(f, "overlay_button", i == 4 ? L"close" : MODE_KEYS[i], ov.hwnd, ov.buttons[i]);
}

static void dump(void)
{
    FILE *f;
    WCHAR tmp[MAX_PATH + 8];
    int i;
    if (!g_dump_path) return;
    swprintf(tmp, MAX_PATH + 8, L"%ls.tmp", g_dump_path);
    if (!(f = _wfopen(tmp, L"w"))) return;
    fprintf(f, "state=%ls\n", g_state);
    fprintf(f, "result=%d %d\n", g_last_w, g_last_h);
    if (ov.hwnd) dump_overlay(f);
    if (toast.hwnd) { RECT r; GetWindowRect(toast.hwnd, &r); put_rect(f, "toast", NULL, NULL, r); }
    if (ed.hwnd && IsWindowVisible(ed.hwnd))
    {
        WCHAR title[MAX_PATH + 32];
        GetWindowTextW(ed.hwnd, title, MAX_PATH + 32);
        fprintf(f, "window_title=%ls\n", title);
        fprintf(f, "mode=%ls\ndelay=%d\n", MODE_KEYS[ed.mode], ed.delay);
        fprintf(f, "tool=%d\ncropping=%d\n", ed.tool, ed.cropping);
        if (has_doc()) fprintf(f, "picture=%d %d\nstrokes=%d\nhistory=%d/%d\n", cur()->base->w, cur()->base->h, cur()->n, g_cur, g_nhist);
        put_rect(f, "canvas", NULL, ed.hwnd, ed.canvas);
        fprintf(f, "scale=%.4f\n", ed.scale);
        for (i = 0; i < B_COUNT; i++) if (ed.shown[i]) put_rect(f, "button", BTN_KEYS[i], ed.hwnd, ed.btn[i]);
    }
    if (g_saved[0]) fprintf(f, "saved=%ls\n", g_saved);
    fclose(f);
    MoveFileExW(tmp, g_dump_path, MOVEFILE_REPLACE_EXISTING);
}

/* ---------------------------------------------------------------------------------------------
 * start-up
 */

static HFONT make_font(int pt10, int weight)
{
    return CreateFontW(-MulDiv(pt10, g_dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static BOOL is_clip_arg(const WCHAR *a)
{
    return !lstrcmpiW(a, L"/clip") || !lstrcmpiW(a, L"-clip") || !_wcsnicmp(a, L"ms-screenclip:", 14) ||
           !_wcsnicmp(a, L"ms-screensketch:", 16);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    static WCHAR dump_path[MAX_PATH];
    WNDCLASSW wc = { 0 };
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
    HDC screen;
    MSG msg;
    int argc, i, mode = M_RECT;
    LPWSTR *argv;
    const WCHAR *file = NULL;
    HANDLE once = NULL;
    sgm_dark = sg_apps_dark();   /* the toast and the clip bar too */

    (void)prev; (void)cmdline; (void)show;
    g_inst = inst;
    SetProcessDPIAware();                /* the screen is captured in real pixels */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    InitCommonControlsEx(&icc);
    screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    if (g_dpi < 96) g_dpi = 96;
    if (GetEnvironmentVariableW(L"SG_SNIP_DUMP", dump_path, MAX_PATH)) g_dump_path = dump_path;
    g_font = make_font(100, FW_NORMAL);
    g_font_bold = make_font(100, FW_SEMIBOLD);
    g_font_small = make_font(90, FW_NORMAL);
    g_icon = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    g_icon_small = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);

    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpfnWndProc = ed_proc; wc.lpszClassName = MAIN_CLASS; wc.hIcon = g_icon;
    RegisterClassW(&wc);
    wc.lpfnWndProc = ov_proc; wc.lpszClassName = OVERLAY_CLASS; wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_CROSS);
    RegisterClassW(&wc);
    wc.lpfnWndProc = toast_proc; wc.lpszClassName = TOAST_CLASS; wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
    RegisterClassW(&wc);

    g_owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", APP_NAME, WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; i++)
    {
        if (is_clip_arg(argv[i])) g_clip_only = TRUE;
        else if (!_wcsnicmp(argv[i], L"/mode:", 6))
        {
            int k;
            for (k = 0; k < M_COUNT; k++) if (!lstrcmpiW(argv[i] + 6, MODE_KEYS[k])) mode = k;
        }
        else if (argv[i][0] != '/') file = argv[i];
    }

    if (g_clip_only)
    {
        /* one screen clip at a time: Win+Shift+S pressed twice does not stack overlays */
        once = CreateMutexW(NULL, TRUE, L"Local\\StainedGlassScreenClip");
        if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
        if (!start_overlay(mode, snip_to_clipboard)) return 1;
    }
    else
    {
        editor_create();
        ed.mode = mode;
        if (file)
        {
            IMG *im = load_file(file);
            if (!im)
            {
                WCHAR msg2[MAX_PATH + 64];
                swprintf(msg2, MAX_PATH + 64, L"%ls could not be opened as a picture.", file);
                MessageBoxW(NULL, msg2, APP_NAME, MB_ICONERROR);
            }
            else { doc_open(im); editor_show_picture(); lstrcpynW(g_saved, file, MAX_PATH); ed_title(); dump(); }
        }
        if (!has_doc())
        {
            ed_fit_window();
            ed_layout();
            ShowWindow(ed.hwnd, SW_SHOW);
            UpdateWindow(ed.hwnd);
            set_state(L"tool");
        }
    }

    while (GetMessageW(&msg, NULL, 0, 0))
    {
        if (msg.message == WM_TIMER && msg.hwnd == ed.hwnd && msg.wParam == TIMER_DELAY)
        {
            KillTimer(ed.hwnd, TIMER_DELAY);
            if (!start_overlay(ed.mode, snip_into_editor)) snip_into_editor(NULL);
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (once) CloseHandle(once);
    return 0;
}
