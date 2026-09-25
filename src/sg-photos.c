/* sg-photos: Photos, the Stained Glass OS image viewer.
 *
 * A Windows 10 Photos-class viewer, our own design in the Stained Glass Light
 * palette. It opens the file it is given (Windows' command line: a path, or
 * an ms-photos:viewer?fileName=... URI) through WIC, so it reads whatever
 * this system's WIC decodes -- PNG, JPEG, BMP, GIF (animated), TIFF, ICO and
 * more -- and honours a JPEG's or TIFF's EXIF orientation.
 *
 *   - fit to the window (never enlarging a small picture), zoom (Ctrl+wheel
 *     at the pointer, + and -, Ctrl+0 fit, Ctrl+1 actual size, double-click),
 *     pan by dragging when zoomed in
 *   - the folder's pictures in Explorer's name order (StrCmpLogicalW): Left
 *     and Right, the wheel, Home and End, arrows on the picture
 *   - rotate (Ctrl+R), Save (Ctrl+S writes the rotation back), Save a copy
 *   - delete to the Recycle Bin (Delete, after asking)
 *   - slideshow (F5; Esc or a click stops it), full screen (F11)
 *   - file information (Alt+Enter), copy (Ctrl+C: the picture and the
 *     file), Edit with Paint (Ctrl+E: mspaint.exe), Set as background (the
 *     Control Panel), Open (Ctrl+O), dropping a file on the window
 *
 * SG_PHOTOS_DUMP=<file> writes what is shown after every paint, for gates.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define APP_NAME L"Photos"
#define CLASS_NAME L"SgPhotosWindow"

/* colours: Stained Glass Light */
#define C_TOOLBAR  RGB(0xFF, 0xFF, 0xFF)
#define C_LINE     RGB(0xE5, 0xE5, 0xE5)
#define C_CANVAS   RGB(0xF3, 0xF3, 0xF3)
#define C_TEXT     RGB(0x1F, 0x1F, 0x1F)
#define C_SUBTEXT  RGB(0x60, 0x60, 0x60)
#define C_HOVER    RGB(0xEB, 0xEB, 0xEB)
#define C_PRESS    RGB(0xDD, 0xDD, 0xDD)
#define C_ACCENT   RGB(112, 48, 192)
#define C_ACTIVE   RGB(0xEE, 0xE6, 0xF8)   /* a toggled button: the accent, faint */

enum { B_OPEN, B_ZOOMIN, B_ZOOMOUT, B_ACTUAL, B_ROTATE, B_DELETE, B_EDIT, B_SLIDESHOW, B_INFO,
       B_FULLSCREEN, B_MORE, B_COUNT };
enum { G_OPEN, G_ZOOMIN, G_ZOOMOUT, G_ACTUAL, G_FIT, G_ROTATE, G_DELETE, G_EDIT, G_SLIDESHOW, G_INFO,
       G_FULLSCREEN, G_UNFULL, G_MORE, G_LEFT, G_RIGHT, G_COUNT };
enum { M_OPEN = 100, M_SAVE, M_SAVECOPY, M_COPY, M_LOCATION, M_BACKGROUND, M_INFO, M_SLIDESHOW, M_EDIT,
       M_DELETE, M_ROTATE };
#define T_ANIM 1
#define T_SLIDE 2
#define T_PILL 3

static const WCHAR *const TIPS[B_COUNT] = {
    L"Open (Ctrl+O)", L"Zoom in (Ctrl+Plus)", L"Zoom out (Ctrl+Minus)", L"Actual size (Ctrl+1)",
    L"Rotate (Ctrl+R)", L"Delete (Delete)", L"Edit with Paint (Ctrl+E)", L"Slideshow (F5)",
    L"File information (Alt+Enter)", L"Full screen (F11)", L"See more" };

typedef struct { BYTE *px; UINT delay; } frame_t;

static struct {
    WCHAR path[MAX_PATH];
    frame_t *frames;
    UINT nframes, frame;
    UINT w, h;              /* as shown (after orientation and rotation) */
    int rotation;           /* the user's, degrees clockwise: 0 90 180 270 */
    int exif;               /* the file's EXIF orientation, 1..8 */
    WCHAR taken[64];        /* EXIF date taken, if any */
    WCHAR kind[64];         /* "PNG file" */
    BOOL error;
} img;

static HWND g_hwnd, g_tip;
static UINT g_dpi = 96;
static HFONT g_font, g_font_bold, g_font_title, g_font_big;
static IWICImagingFactory *g_wic;
static WCHAR **g_list;                  /* the folder's pictures */
static int g_count, g_index = -1;
static WCHAR g_exts[2048] = L"";        /* ";.png;.jpg;..." what WIC decodes */
static BOOL g_fit = TRUE, g_info, g_full, g_slideshow, g_nav_hover, g_dragging, g_tracking;
static double g_scale = 1, g_ox, g_oy;  /* image top-left in canvas coordinates */
static POINT g_drag_at;
static double g_drag_ox, g_drag_oy;
static RECT g_buttons[B_COUNT], g_nav_left, g_nav_right, g_open_button;
static int g_hot = -1, g_pressed = -1, g_nav_hot;
static WINDOWPLACEMENT g_placement = { sizeof(WINDOWPLACEMENT) };
static DWORD g_pill_until;
static HBITMAP g_glyphs[G_COUNT];
static int g_glyph_size;
static WCHAR g_dump[MAX_PATH];
static HBITMAP g_scaled;                /* the picture reduced for a scale below 1 */
static UINT g_scaled_w, g_scaled_h, g_scaled_frame;
static int g_scaled_gen, g_gen;

static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

static HFONT make_font(int pt10, int weight)
{
    return CreateFontW(-MulDiv(pt10, (int)g_dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void make_fonts(void)
{
    if (g_font) { DeleteObject(g_font); DeleteObject(g_font_bold); DeleteObject(g_font_title); DeleteObject(g_font_big); }
    g_font = make_font(90, FW_NORMAL);
    g_font_bold = make_font(90, FW_SEMIBOLD);
    g_font_title = make_font(110, FW_SEMIBOLD);
    g_font_big = make_font(150, FW_NORMAL);
}

/* ---------------------------------------------------------------- glyphs --
 * Drawn with GDI at four times their size, black on white, then reduced to
 * an alpha mask and blended in the text colour: smooth, and no artwork. */
#define SS 4
static int g_gn;
static int P(double v) { return (int)(v * g_gn / 24.0 + 0.5); }

static HPEN pen(int w10)
{
    LOGBRUSH lb = { BS_SOLID, RGB(0, 0, 0), 0 };
    int w = w10 * g_gn / 240;
    return ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, w > 0 ? w : 1, &lb, 0, NULL);
}

static void seg(HDC dc, double x0, double y0, double x1, double y1)
{
    MoveToEx(dc, P(x0), P(y0), NULL);
    LineTo(dc, P(x1), P(y1));
}

static void draw_glyph(HDC dc, int g)
{
    HGDIOBJ op = SelectObject(dc, pen(17)), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    switch (g)
    {
    case G_OPEN:
        MoveToEx(dc, P(3), P(19), NULL); LineTo(dc, P(3), P(5)); LineTo(dc, P(9), P(5)); LineTo(dc, P(11), P(7));
        LineTo(dc, P(19), P(7)); LineTo(dc, P(19), P(10));
        MoveToEx(dc, P(3), P(19), NULL); LineTo(dc, P(6), P(10)); LineTo(dc, P(22), P(10)); LineTo(dc, P(19), P(19));
        LineTo(dc, P(3), P(19));
        break;
    case G_ZOOMIN: case G_ZOOMOUT:
        Ellipse(dc, P(3), P(3), P(17), P(17));
        seg(dc, 15, 15, 21, 21);
        seg(dc, 7, 10, 13, 10);
        if (g == G_ZOOMIN) seg(dc, 10, 7, 10, 13);
        break;
    case G_ACTUAL:
        /* "1:1" */
        seg(dc, 4, 8, 6, 6); seg(dc, 6, 6, 6, 18);
        seg(dc, 17, 8, 19, 6); seg(dc, 19, 6, 19, 18);
        { HBRUSH b = (HBRUSH)GetStockObject(BLACK_BRUSH); HGDIOBJ o = SelectObject(dc, b);
          Ellipse(dc, P(11), P(8.5), P(13.2), P(10.7)); Ellipse(dc, P(11), P(14), P(13.2), P(16.2)); SelectObject(dc, o); }
        break;
    case G_FIT:
        seg(dc, 3, 8, 3, 3); seg(dc, 3, 3, 8, 3);
        seg(dc, 16, 3, 21, 3); seg(dc, 21, 3, 21, 8);
        seg(dc, 21, 16, 21, 21); seg(dc, 21, 21, 16, 21);
        seg(dc, 8, 21, 3, 21); seg(dc, 3, 21, 3, 16);
        Rectangle(dc, P(7), P(8), P(17), P(16));
        break;
    case G_ROTATE:
        /* three quarters of a circle, counter-clockwise from the top round
         * to the right, with the head at the top: turning clockwise */
        Arc(dc, P(4), P(4), P(20), P(20), P(12), P(4), P(20), P(12));
        seg(dc, 13, 4, 10, 1.5); seg(dc, 13, 4, 10, 6.5);
        break;
    case G_DELETE:
        seg(dc, 3, 6, 21, 6); seg(dc, 9, 6, 9, 3); seg(dc, 9, 3, 15, 3); seg(dc, 15, 3, 15, 6);
        MoveToEx(dc, P(5), P(6), NULL); LineTo(dc, P(6.5), P(21)); LineTo(dc, P(17.5), P(21)); LineTo(dc, P(19), P(6));
        seg(dc, 10, 10, 10, 17); seg(dc, 14, 10, 14, 17);
        break;
    case G_EDIT:
        MoveToEx(dc, P(4), P(20), NULL); LineTo(dc, P(5), P(15)); LineTo(dc, P(16), P(4)); LineTo(dc, P(20), P(8));
        LineTo(dc, P(9), P(19)); LineTo(dc, P(4), P(20));
        seg(dc, 13.5, 6.5, 17.5, 10.5);
        break;
    case G_SLIDESHOW:
        Rectangle(dc, P(2.5), P(4), P(21.5), P(18));
        { POINT t[3] = { { P(10), P(7.5) }, { P(15.5), P(11) }, { P(10), P(14.5) } };
          HGDIOBJ o = SelectObject(dc, GetStockObject(BLACK_BRUSH)); Polygon(dc, t, 3); SelectObject(dc, o); }
        seg(dc, 8, 21, 16, 21);
        break;
    case G_INFO:
        Ellipse(dc, P(2.5), P(2.5), P(21.5), P(21.5));
        seg(dc, 12, 11, 12, 17);
        { HGDIOBJ o = SelectObject(dc, GetStockObject(BLACK_BRUSH)); Ellipse(dc, P(10.8), P(6.3), P(13.2), P(8.7)); SelectObject(dc, o); }
        break;
    case G_FULLSCREEN:
        seg(dc, 3, 3, 9, 9); seg(dc, 3, 3, 3, 8); seg(dc, 3, 3, 8, 3);
        seg(dc, 21, 3, 15, 9); seg(dc, 21, 3, 21, 8); seg(dc, 21, 3, 16, 3);
        seg(dc, 3, 21, 9, 15); seg(dc, 3, 21, 3, 16); seg(dc, 3, 21, 8, 21);
        seg(dc, 21, 21, 15, 15); seg(dc, 21, 21, 21, 16); seg(dc, 21, 21, 16, 21);
        break;
    case G_UNFULL:
        seg(dc, 3, 3, 9, 9); seg(dc, 9, 9, 9, 4); seg(dc, 9, 9, 4, 9);
        seg(dc, 21, 3, 15, 9); seg(dc, 15, 9, 15, 4); seg(dc, 15, 9, 20, 9);
        seg(dc, 3, 21, 9, 15); seg(dc, 9, 15, 9, 20); seg(dc, 9, 15, 4, 15);
        seg(dc, 21, 21, 15, 15); seg(dc, 15, 15, 15, 20); seg(dc, 15, 15, 20, 15);
        break;
    case G_MORE:
        { HGDIOBJ o = SelectObject(dc, GetStockObject(BLACK_BRUSH));
          Ellipse(dc, P(3.5), P(10.5), P(6.5), P(13.5)); Ellipse(dc, P(10.5), P(10.5), P(13.5), P(13.5));
          Ellipse(dc, P(17.5), P(10.5), P(20.5), P(13.5)); SelectObject(dc, o); }
        break;
    case G_LEFT: seg(dc, 15, 4, 8, 12); seg(dc, 8, 12, 15, 20); break;
    case G_RIGHT: seg(dc, 9, 4, 16, 12); seg(dc, 16, 12, 9, 20); break;
    }
    DeleteObject(SelectObject(dc, op));
    SelectObject(dc, ob);
}

/* a premultiplied BGRA bitmap of glyph g in colour c, size x size pixels */
static HBITMAP render_glyph(int g, int size, COLORREF c)
{
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), 0, 0, 1, 32, BI_RGB } };
    int big = size * SS, x, y, i, j;
    BYTE *bits, *out;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP bb, res;
    bi.bmiHeader.biWidth = big; bi.bmiHeader.biHeight = -big;
    bb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    SelectObject(dc, bb);
    PatBlt(dc, 0, 0, big, big, WHITENESS);
    g_gn = big;
    draw_glyph(dc, g);
    GdiFlush();
    bi.bmiHeader.biWidth = size; bi.bmiHeader.biHeight = -size;
    res = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&out, NULL, 0);
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++)
        {
            unsigned sum = 0, a;
            for (j = 0; j < SS; j++)
                for (i = 0; i < SS; i++)
                    sum += 255 - bits[((y * SS + j) * big + x * SS + i) * 4 + 1];
            a = sum / (SS * SS);
            out[(y * size + x) * 4 + 0] = (BYTE)(GetBValue(c) * a / 255);
            out[(y * size + x) * 4 + 1] = (BYTE)(GetGValue(c) * a / 255);
            out[(y * size + x) * 4 + 2] = (BYTE)(GetRValue(c) * a / 255);
            out[(y * size + x) * 4 + 3] = (BYTE)a;
        }
    DeleteDC(dc);
    DeleteObject(bb);
    return res;
}

static void blit_glyph(HDC dc, int g, int x, int y)
{
    int size = S(20);
    HDC mem;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    if (g_glyph_size != size)
    {
        int i;
        for (i = 0; i < G_COUNT; i++) if (g_glyphs[i]) { DeleteObject(g_glyphs[i]); g_glyphs[i] = NULL; }
        g_glyph_size = size;
    }
    if (!g_glyphs[g]) g_glyphs[g] = render_glyph(g, size, C_TEXT);
    mem = CreateCompatibleDC(dc);
    SelectObject(mem, g_glyphs[g]);
    AlphaBlend(dc, x, y, size, size, mem, 0, 0, size, size, bf);
    DeleteDC(mem);
}

/* ------------------------------------------------------------- pictures -- */

static void free_image(void)
{
    UINT i;
    for (i = 0; i < img.nframes; i++) free(img.frames[i].px);
    free(img.frames);
    img.frames = NULL;
    img.nframes = img.frame = 0;
    img.w = img.h = 0;
    img.rotation = 0;
    img.exif = 1;
    img.taken[0] = img.kind[0] = 0;
    img.error = FALSE;
    g_gen++;
    KillTimer(g_hwnd, T_ANIM);
}

/* EXIF orientation o (1..8) applied to a w x h BGRA buffer; *w, *h updated */
static BYTE *orient(BYTE *src, UINT *w, UINT *h, int o)
{
    UINT sw = *w, sh = *h, dw, dh, x, y;
    BYTE *dst;
    DWORD *s = (DWORD *)src, *d;
    if (o <= 1 || o > 8) return src;
    dw = o >= 5 ? sh : sw;
    dh = o >= 5 ? sw : sh;
    if (!(dst = malloc((size_t)dw * dh * 4))) return src;
    d = (DWORD *)dst;
    for (y = 0; y < sh; y++)
        for (x = 0; x < sw; x++)
        {
            UINT nx, ny;
            switch (o)
            {
            case 2: nx = sw - 1 - x; ny = y; break;              /* mirror */
            case 3: nx = sw - 1 - x; ny = sh - 1 - y; break;     /* 180 */
            case 4: nx = x; ny = sh - 1 - y; break;              /* flip */
            case 5: nx = y; ny = x; break;                       /* transpose */
            case 6: nx = sh - 1 - y; ny = x; break;              /* 90 clockwise */
            case 7: nx = sh - 1 - y; ny = sw - 1 - x; break;     /* transverse */
            default: nx = y; ny = sw - 1 - x; break;             /* 8: 270 clockwise */
            }
            d[(size_t)ny * dw + nx] = s[(size_t)y * sw + x];
        }
    free(src);
    *w = dw; *h = dh;
    return dst;
}

static BOOL meta_uint(IWICMetadataQueryReader *q, const WCHAR *name, UINT *out)
{
    PROPVARIANT v;
    BOOL ok = FALSE;
    if (!q) return FALSE;
    PropVariantInit(&v);
    if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(q, name, &v)))
    {
        ok = TRUE;
        switch (v.vt)
        {
        case VT_UI1: *out = v.bVal; break;
        case VT_UI2: *out = v.uiVal; break;
        case VT_UI4: *out = v.ulVal; break;
        case VT_I2: *out = (UINT)v.iVal; break;
        case VT_I4: *out = (UINT)v.lVal; break;
        default: ok = FALSE;
        }
    }
    PropVariantClear(&v);
    return ok;
}

static BYTE *frame_pixels(IWICBitmapFrameDecode *f, UINT *w, UINT *h)
{
    IWICBitmapSource *conv = NULL;
    BYTE *px = NULL;
    if (FAILED(IWICBitmapFrameDecode_GetSize(f, w, h)) || !*w || !*h || *w > 32768 || *h > 32768) return NULL;
    if (FAILED(WICConvertBitmapSource(&GUID_WICPixelFormat32bppPBGRA, (IWICBitmapSource *)f, &conv))) return NULL;
    if ((px = malloc((size_t)*w * *h * 4)) &&
        FAILED(IWICBitmapSource_CopyPixels(conv, NULL, *w * 4, *w * *h * 4, px)))
    { free(px); px = NULL; }
    IWICBitmapSource_Release(conv);
    return px;
}

static void load_gif(IWICBitmapDecoder *dec, UINT n)
{
    IWICMetadataQueryReader *q = NULL;
    UINT sw = 0, sh = 0, i;
    BYTE *canvas, *saved = NULL;
    size_t total = 0;
    if (SUCCEEDED(IWICBitmapDecoder_GetMetadataQueryReader(dec, &q)))
    {
        meta_uint(q, L"/logscrdesc/Width", &sw);
        meta_uint(q, L"/logscrdesc/Height", &sh);
        IWICMetadataQueryReader_Release(q);
    }
    if (n > 500) n = 500;
    if (!sw || !sh)
    {
        IWICBitmapFrameDecode *f;
        if (FAILED(IWICBitmapDecoder_GetFrame(dec, 0, &f))) return;
        IWICBitmapFrameDecode_GetSize(f, &sw, &sh);
        IWICBitmapFrameDecode_Release(f);
    }
    if (!sw || !sh || sw > 16384 || sh > 16384) return;
    if (!(canvas = calloc((size_t)sw * sh, 4)) || !(img.frames = calloc(n, sizeof(frame_t)))) { free(canvas); return; }
    img.w = sw; img.h = sh;
    for (i = 0; i < n; i++)
    {
        IWICBitmapFrameDecode *f;
        UINT fw, fh, left = 0, top = 0, delay = 10, disposal = 0, x, y;
        BYTE *px;
        if (FAILED(IWICBitmapDecoder_GetFrame(dec, i, &f))) break;
        if (SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader(f, &q)))
        {
            meta_uint(q, L"/imgdesc/Left", &left);
            meta_uint(q, L"/imgdesc/Top", &top);
            meta_uint(q, L"/grctlext/Delay", &delay);
            meta_uint(q, L"/grctlext/Disposal", &disposal);
            IWICMetadataQueryReader_Release(q);
        }
        px = frame_pixels(f, &fw, &fh);
        IWICBitmapFrameDecode_Release(f);
        if (!px) break;
        if (disposal == 3) { free(saved); if ((saved = malloc((size_t)sw * sh * 4))) memcpy(saved, canvas, (size_t)sw * sh * 4); }
        for (y = 0; y < fh && top + y < sh; y++)
            for (x = 0; x < fw && left + x < sw; x++)
            {
                DWORD p = ((DWORD *)px)[(size_t)y * fw + x];
                if (p >> 24) ((DWORD *)canvas)[(size_t)(top + y) * sw + left + x] = p;
            }
        free(px);
        total += (size_t)sw * sh * 4;
        if (total > 512u << 20 || !(img.frames[i].px = malloc((size_t)sw * sh * 4))) break;
        memcpy(img.frames[i].px, canvas, (size_t)sw * sh * 4);
        img.frames[i].delay = delay < 2 ? 100 : delay * 10;
        img.nframes = i + 1;
        if (disposal == 2)
            for (y = 0; y < fh && top + y < sh; y++)
                memset(canvas + ((size_t)(top + y) * sw + left) * 4, 0, (size_t)min(fw, sw - left) * 4);
        else if (disposal == 3 && saved)
            memcpy(canvas, saved, (size_t)sw * sh * 4);
    }
    free(saved);
    free(canvas);
}

/* The EXIF orientation (and date taken) of a JPEG, read from its APP1
 * segment ourselves: Wine's JPEG decoder exposes no EXIF metadata. */
static unsigned rd16(const BYTE *p, BOOL le) { return le ? p[0] | p[1] << 8 : p[0] << 8 | p[1]; }
static unsigned rd32(const BYTE *p, BOOL le) { return le ? rd16(p, 1) | rd16(p + 2, 1) << 16 : rd16(p, 0) << 16 | rd16(p + 2, 0); }

static void jpeg_exif(const WCHAR *path, int *orientation, WCHAR *taken)
{
    BYTE buf[65536 + 4];
    DWORD got = 0;
    size_t i = 2;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    ReadFile(f, buf, 65536, &got, NULL);
    CloseHandle(f);
    if (got < 4 || buf[0] != 0xFF || buf[1] != 0xD8) return;
    while (i + 4 <= got && buf[i] == 0xFF)
    {
        unsigned marker = buf[i + 1], len = rd16(buf + i + 2, FALSE);
        const BYTE *t;
        size_t tl;
        BOOL le;
        unsigned ifd, n, k, exif_ifd = 0;
        if (marker == 0xDA || len < 2) return;          /* the image data: no EXIF before it */
        if (marker != 0xE1 || i + 2 + len > got || len < 16 || memcmp(buf + i + 4, "Exif\0\0", 6)) { i += 2 + len; continue; }
        t = buf + i + 10; tl = len - 8;
        if (!memcmp(t, "II*\0", 4)) le = TRUE; else if (!memcmp(t, "MM\0*", 4)) le = FALSE; else return;
        ifd = rd32(t + 4, le);
        if (ifd + 2 > tl) return;
        n = rd16(t + ifd, le);
        for (k = 0; k < n && ifd + 2 + (k + 1) * 12 <= tl; k++)
        {
            const BYTE *e = t + ifd + 2 + k * 12;
            unsigned tag = rd16(e, le);
            if (tag == 0x0112) { unsigned o = rd16(e + 8, le); if (o >= 1 && o <= 8) *orientation = (int)o; }
            if (tag == 0x8769) exif_ifd = rd32(e + 8, le);
        }
        if (exif_ifd && exif_ifd + 2 <= tl && taken)
        {
            n = rd16(t + exif_ifd, le);
            for (k = 0; k < n && exif_ifd + 2 + (k + 1) * 12 <= tl; k++)
            {
                const BYTE *e = t + exif_ifd + 2 + k * 12;
                unsigned off = rd32(e + 8, le), cnt = rd32(e + 4, le);
                if (rd16(e, le) == 0x9003 && cnt >= 19 && cnt < 64 && off + cnt <= tl)
                {
                    char d[64];
                    memcpy(d, t + off, cnt); d[cnt - 1] = 0;
                    /* "2024:05:01 10:20:30" -> "2024-05-01 10:20:30" */
                    if (d[4] == ':') d[4] = '-';
                    if (d[7] == ':') d[7] = '-';
                    MultiByteToWideChar(CP_ACP, 0, d, -1, taken, 64);
                }
            }
        }
        return;
    }
}

static void describe_kind(void)
{
    const WCHAR *ext = PathFindExtensionW(img.path);
    int i = 0;
    if (*ext == '.') ext++;
    for (; ext[i] && i < 20; i++) img.kind[i] = towupper(ext[i]);
    lstrcpyW(img.kind + i, L" file");
}

/* Load the picture at path; FALSE (and img.error) if WIC cannot read it */
static BOOL load_image(const WCHAR *path)
{
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *f = NULL;
    IWICMetadataQueryReader *q = NULL;
    GUID container = { 0 };
    UINT n = 0, i, best = 0;
    UINT64 best_area = 0;

    free_image();
    lstrcpynW(img.path, path, MAX_PATH);
    describe_kind();
    if (!g_wic || FAILED(IWICImagingFactory_CreateDecoderFromFilename(g_wic, path, NULL, GENERIC_READ,
                                                                      WICDecodeMetadataCacheOnDemand, &dec)))
    { img.error = TRUE; return FALSE; }
    IWICBitmapDecoder_GetFrameCount(dec, &n);
    if (SUCCEEDED(IWICBitmapDecoder_GetContainerFormat(dec, &container)) &&
        IsEqualGUID(&container, &GUID_ContainerFormatGif) && n > 1)
    {
        load_gif(dec, n);
        IWICBitmapDecoder_Release(dec);
        if (!img.nframes) { img.error = TRUE; return FALSE; }
        return TRUE;
    }
    /* an icon holds several sizes: show the largest */
    for (i = 0; i < n && i < 64; i++)
    {
        UINT w, h;
        if (FAILED(IWICBitmapDecoder_GetFrame(dec, i, &f))) continue;
        if (SUCCEEDED(IWICBitmapFrameDecode_GetSize(f, &w, &h)) && (UINT64)w * h > best_area) { best_area = (UINT64)w * h; best = i; }
        IWICBitmapFrameDecode_Release(f);
        f = NULL;
        if (!IsEqualGUID(&container, &GUID_ContainerFormatIco)) break;
    }
    if (FAILED(IWICBitmapDecoder_GetFrame(dec, best, &f))) { IWICBitmapDecoder_Release(dec); img.error = TRUE; return FALSE; }
    if (!(img.frames = calloc(1, sizeof(frame_t))) || !(img.frames[0].px = frame_pixels(f, &img.w, &img.h)))
    {
        IWICBitmapFrameDecode_Release(f); IWICBitmapDecoder_Release(dec);
        free(img.frames); img.frames = NULL; img.error = TRUE; return FALSE;
    }
    img.nframes = 1;
    if (SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader(f, &q)))
    {
        UINT o = 1;
        PROPVARIANT v;
        if (meta_uint(q, L"/app1/ifd/{ushort=274}", &o) || meta_uint(q, L"/ifd/{ushort=274}", &o)) img.exif = (int)o;
        PropVariantInit(&v);
        if ((SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(q, L"/app1/ifd/exif/{ushort=36867}", &v)) ||
             SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(q, L"/ifd/exif/{ushort=36867}", &v))) && v.vt == VT_LPSTR && v.pszVal)
            MultiByteToWideChar(CP_UTF8, 0, v.pszVal, -1, img.taken, 64);
        PropVariantClear(&v);
        IWICMetadataQueryReader_Release(q);
    }
    IWICBitmapFrameDecode_Release(f);
    IWICBitmapDecoder_Release(dec);
    if (img.exif <= 1 && IsEqualGUID(&container, &GUID_ContainerFormatJpeg)) jpeg_exif(path, &img.exif, img.taken[0] ? NULL : img.taken);
    if (img.exif > 1) img.frames[0].px = orient(img.frames[0].px, &img.w, &img.h, img.exif);
    return TRUE;
}

static void rotate_image(void)
{
    UINT i, w = 0, h = 0;
    if (!img.nframes) return;
    for (i = 0; i < img.nframes; i++)
    {
        w = img.w; h = img.h;
        img.frames[i].px = orient(img.frames[i].px, &w, &h, 6);
    }
    img.w = w; img.h = h;
    img.rotation = (img.rotation + 90) % 360;
    g_gen++;
}

/* ------------------------------------------------------------ the folder -- */

static void add_ext(const WCHAR *e)
{
    WCHAR probe[32];
    if (!*e || lstrlenW(e) > 20) return;
    _snwprintf(probe, 32, L";%ls;", e);
    CharLowerW(probe);
    if (StrStrIW(g_exts, probe)) return;
    if (lstrlenW(g_exts) + lstrlenW(probe) + 1 >= (int)ARRAYSIZE(g_exts)) return;
    if (!*g_exts) lstrcpyW(g_exts, probe);
    else lstrcatW(g_exts, probe + 1);
}

/* what this system's WIC can decode, from its decoders' own extension lists */
static void find_extensions(void)
{
    static const WCHAR *const base[] = { L".png", L".jpg", L".jpeg", L".jpe", L".jfif", L".bmp", L".dib", L".gif",
                                         L".tif", L".tiff", L".ico" };
    IEnumUnknown *en;
    IUnknown *u;
    ULONG got;
    unsigned i;
    for (i = 0; i < ARRAYSIZE(base); i++) add_ext(base[i]);
    if (!g_wic || FAILED(IWICImagingFactory_CreateComponentEnumerator(g_wic, WICDecoder, WICComponentEnumerateDefault, &en))) return;
    while (IEnumUnknown_Next(en, 1, &u, &got) == S_OK && got)
    {
        IWICBitmapCodecInfo *info;
        if (SUCCEEDED(IUnknown_QueryInterface(u, &IID_IWICBitmapCodecInfo, (void **)&info)))
        {
            WCHAR list[256], *p, *c;
            UINT len = 0;
            if (SUCCEEDED(IWICBitmapCodecInfo_GetFileExtensions(info, 256, list, &len)))
                for (p = list; p && *p; p = c ? c + 1 : NULL)
                {
                    if ((c = wcschr(p, ','))) *c = 0;
                    add_ext(p);
                }
            IWICBitmapCodecInfo_Release(info);
        }
        IUnknown_Release(u);
    }
    IEnumUnknown_Release(en);
}

static BOOL is_picture(const WCHAR *name)
{
    WCHAR probe[32];
    const WCHAR *ext = PathFindExtensionW(name);
    if (!*ext || lstrlenW(ext) > 20) return FALSE;
    _snwprintf(probe, 32, L";%ls;", ext);
    return StrStrIW(g_exts, probe) != NULL;
}

static int __cdecl name_order(const void *a, const void *b)
{
    return StrCmpLogicalW(PathFindFileNameW(*(WCHAR *const *)a), PathFindFileNameW(*(WCHAR *const *)b));
}

static void free_list(void)
{
    int i;
    for (i = 0; i < g_count; i++) free(g_list[i]);
    free(g_list);
    g_list = NULL;
    g_count = 0;
    g_index = -1;
}

/* the pictures in path's folder, in Explorer's name order; g_index = path's */
static void scan_folder(const WCHAR *path)
{
    WCHAR dir[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int cap = 0, i;
    free_list();
    lstrcpynW(dir, path, MAX_PATH);
    PathRemoveFileSpecW(dir);
    if (!PathCombineW(pattern, dir, L"*")) return;
    if ((h = FindFirstFileW(pattern, &fd)) != INVALID_HANDLE_VALUE)
    {
        do {
            WCHAR full[MAX_PATH];
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) continue;
            if (!is_picture(fd.cFileName) || !PathCombineW(full, dir, fd.cFileName)) continue;
            if (g_count == cap)
            {
                WCHAR **n = realloc(g_list, (cap = cap ? cap * 2 : 64) * sizeof(*n));
                if (!n) break;
                g_list = n;
            }
            g_list[g_count++] = _wcsdup(full);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (g_count) qsort(g_list, g_count, sizeof(*g_list), name_order);
    for (i = 0; i < g_count; i++)
        if (!lstrcmpiW(PathFindFileNameW(g_list[i]), PathFindFileNameW(path))) { g_index = i; break; }
}

/* ------------------------------------------------------------ the layout -- */

static int toolbar_height(void) { return g_slideshow || g_full ? 0 : S(48); }
static int info_width(void) { return g_info && !g_slideshow ? S(300) : 0; }

static void canvas_rect(RECT *r)
{
    GetClientRect(g_hwnd, r);
    r->top += toolbar_height();
    r->right -= info_width();
    if (r->right < r->left) r->right = r->left;
    if (r->bottom < r->top) r->bottom = r->top;
}

static double fit_scale(void)
{
    RECT c;
    double s;
    int m = g_slideshow ? 0 : S(8);
    canvas_rect(&c);
    if (!img.w || !img.h) return 1;
    s = min((double)(c.right - c.left - 2 * m) / img.w, (double)(c.bottom - c.top - 2 * m) / img.h);
    if (s > 1) s = 1;
    if (s <= 0) s = 0.01;
    return s;
}

/* keep the picture centred where it is smaller than the canvas, and without
 * a gap at an edge where it is larger */
static void clamp_offset(void)
{
    RECT c;
    double cw, ch, iw = img.w * g_scale, ih = img.h * g_scale;
    canvas_rect(&c);
    cw = c.right - c.left; ch = c.bottom - c.top;
    if (iw <= cw) g_ox = (cw - iw) / 2;
    else { if (g_ox > 0) g_ox = 0; if (g_ox + iw < cw) g_ox = cw - iw; }
    if (ih <= ch) g_oy = (ch - ih) / 2;
    else { if (g_oy > 0) g_oy = 0; if (g_oy + ih < ch) g_oy = ch - ih; }
}

static void relayout(void)
{
    if (g_fit) g_scale = fit_scale();
    clamp_offset();
}

static void show_pill(void)
{
    g_pill_until = GetTickCount() + 1500;
    SetTimer(g_hwnd, T_PILL, 1600, NULL);
}

/* zoom to scale s keeping canvas point (px, py) where it is */
static void zoom_to(double s, double px, double py)
{
    double u, v;
    if (!img.nframes) return;
    if (s < 0.02) s = 0.02;
    if (s > 32) s = 32;
    u = (px - g_ox) / g_scale;
    v = (py - g_oy) / g_scale;
    g_fit = FALSE;
    g_scale = s;
    g_ox = px - u * s;
    g_oy = py - v * s;
    clamp_offset();
    show_pill();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void zoom_step(int dir, const POINT *at)
{
    RECT c;
    double px, py, s = g_scale * (dir > 0 ? 1.25 : 0.8);
    canvas_rect(&c);
    if (at) { px = at->x - c.left; py = at->y - c.top; }
    else { px = (c.right - c.left) / 2.0; py = (c.bottom - c.top) / 2.0; }
    /* stop at 100% and at fit on the way through, as Photos does */
    if ((g_scale < 1 && s > 1) || (g_scale > 1 && s < 1)) s = 1;
    zoom_to(s, px, py);
}

static void set_fit(void)
{
    g_fit = TRUE;
    relayout();
    show_pill();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void actual_size(void)
{
    RECT c;
    canvas_rect(&c);
    zoom_to(1, (c.right - c.left) / 2.0, (c.bottom - c.top) / 2.0);
}

/* -------------------------------------------------------------- showing -- */

static void update_title(void)
{
    WCHAR t[MAX_PATH + 32];
    if (img.path[0]) _snwprintf(t, ARRAYSIZE(t), L"%ls - " APP_NAME, PathFindFileNameW(img.path));
    else lstrcpyW(t, APP_NAME);
    t[ARRAYSIZE(t) - 1] = 0;
    SetWindowTextW(g_hwnd, t);
}

static void start_animation(void)
{
    KillTimer(g_hwnd, T_ANIM);
    if (img.nframes > 1 && !IsIconic(g_hwnd)) SetTimer(g_hwnd, T_ANIM, img.frames[img.frame].delay, NULL);
}

static void open_path(const WCHAR *path, BOOL rescan)
{
    WCHAR full[MAX_PATH];
    if (!GetFullPathNameW(path, MAX_PATH, full, NULL)) lstrcpynW(full, path, MAX_PATH);
    load_image(full);
    if (rescan) scan_folder(full);
    else
    {
        int i;
        for (i = 0; i < g_count; i++) if (!lstrcmpiW(g_list[i], full)) { g_index = i; break; }
    }
    g_fit = TRUE;
    relayout();
    update_title();
    start_animation();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void go(int delta, BOOL wrap)
{
    int i = g_index;
    if (g_count < 1) return;
    if (i < 0) i = 0;
    else
    {
        i += delta;
        if (wrap) i = (i % g_count + g_count) % g_count;
        else if (i < 0 || i >= g_count) return;
    }
    if (GetFileAttributesW(g_list[i]) == INVALID_FILE_ATTRIBUTES)
    {
        /* gone since the folder was read: read it again */
        WCHAR keep[MAX_PATH];
        lstrcpynW(keep, img.path, MAX_PATH);
        scan_folder(keep);
        if (g_index < 0 && g_count) g_index = 0;
        return;
    }
    g_index = i;
    open_path(g_list[i], FALSE);
}

static void go_to(int index)
{
    if (index < 0 || index >= g_count) return;
    g_index = index;
    open_path(g_list[index], FALSE);
}

/* the picture's current frame, as a 32-bit DIB section w x h */
static HBITMAP frame_dib(const BYTE *px, UINT w, UINT h)
{
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)h, 1, 32, BI_RGB } };
    void *bits;
    HBITMAP b = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (b) memcpy(bits, px, (size_t)w * h * 4);
    return b;
}

/* the frame reduced to w x h with WIC's Fant scaler (box-like, smooth) */
static HBITMAP reduced_dib(UINT w, UINT h)
{
    IWICBitmap *src = NULL;
    IWICBitmapScaler *sc = NULL;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)h, 1, 32, BI_RGB } };
    void *bits = NULL;
    HBITMAP b = NULL;
    if (!g_wic) return NULL;
    if (FAILED(IWICImagingFactory_CreateBitmapFromMemory(g_wic, img.w, img.h, &GUID_WICPixelFormat32bppPBGRA,
                                                         img.w * 4, img.w * img.h * 4, img.frames[img.frame].px, &src)))
        return NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(g_wic, &sc)) &&
        SUCCEEDED(IWICBitmapScaler_Initialize(sc, (IWICBitmapSource *)src, w, h, WICBitmapInterpolationModeFant)) &&
        (b = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0)) &&
        FAILED(IWICBitmapScaler_CopyPixels(sc, NULL, w * 4, w * h * 4, bits)))
    { DeleteObject(b); b = NULL; }
    if (sc) IWICBitmapScaler_Release(sc);
    IWICBitmap_Release(src);
    return b;
}

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    SetBkColor(dc, c);
    ExtTextOutW(dc, 0, 0, ETO_OPAQUE, r, NULL, 0, NULL);
}

static void round_fill(HDC dc, const RECT *r, int radius, COLORREF c, COLORREF edge)
{
    HBRUSH b = CreateSolidBrush(c);
    HPEN p = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r->left, r->top, r->right, r->bottom, radius, radius);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static RECT g_drawn;    /* where the picture is, client coordinates */

static void paint_picture(HDC dc, const RECT *c)
{
    double iw = img.w * g_scale, ih = img.h * g_scale;
    int x = c->left + (int)floor(g_ox), y = c->top + (int)floor(g_oy);
    int dw = (int)(iw + 0.5), dh = (int)(ih + 0.5);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC mem = CreateCompatibleDC(dc);
    HRGN clip = CreateRectRgnIndirect(c);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    SetRect(&g_drawn, x, y, x + dw, y + dh);
    SelectClipRgn(dc, clip);
    if (g_scale < 0.999)
    {
        if (!g_scaled || g_scaled_w != (UINT)dw || g_scaled_h != (UINT)dh || g_scaled_frame != img.frame || g_scaled_gen != g_gen)
        {
            if (g_scaled) DeleteObject(g_scaled);
            g_scaled = reduced_dib(dw, dh);
            g_scaled_w = dw; g_scaled_h = dh; g_scaled_frame = img.frame; g_scaled_gen = g_gen;
        }
        if (g_scaled)
        {
            SelectObject(mem, g_scaled);
            AlphaBlend(dc, x, y, dw, dh, mem, 0, 0, dw, dh, bf);
        }
    }
    else
    {
        /* enlarged: only the part that is visible, pixel for pixel */
        HBITMAP b = frame_dib(img.frames[img.frame].px, img.w, img.h);
        int sx0 = (int)max(0, floor((c->left - x) / g_scale)), sy0 = (int)max(0, floor((c->top - y) / g_scale));
        int sx1 = (int)min((double)img.w, ceil((c->right - x) / g_scale)), sy1 = (int)min((double)img.h, ceil((c->bottom - y) / g_scale));
        if (b && sx1 > sx0 && sy1 > sy0)
        {
            int ox = x + (int)floor(sx0 * g_scale), oy = y + (int)floor(sy0 * g_scale);
            int ow = (int)floor(sx1 * g_scale) - (int)floor(sx0 * g_scale), oh = (int)floor(sy1 * g_scale) - (int)floor(sy0 * g_scale);
            SelectObject(mem, b);
            if (g_scale > 0.999 && g_scale < 1.001) AlphaBlend(dc, ox, oy, sx1 - sx0, sy1 - sy0, mem, sx0, sy0, sx1 - sx0, sy1 - sy0, bf);
            else AlphaBlend(dc, ox, oy, ow, oh, mem, sx0, sy0, sx1 - sx0, sy1 - sy0, bf);
        }
        DeleteDC(mem);
        mem = CreateCompatibleDC(dc);
        if (b) DeleteObject(b);
    }
    SelectClipRgn(dc, NULL);
    DeleteObject(clip);
    DeleteDC(mem);
}

static void draw_text(HDC dc, HFONT f, COLORREF c, const WCHAR *s, RECT *r, UINT flags)
{
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, r, flags | DT_NOPREFIX);
    SelectObject(dc, of);
}

static void paint_toolbar(HDC dc, const RECT *client)
{
    RECT tb = *client, r;
    int bw = S(40), gap = S(2), x, i, g;
    WCHAR s[MAX_PATH + 64];
    tb.bottom = tb.top + toolbar_height();
    fill(dc, &tb, C_TOOLBAR);
    r = tb; r.top = r.bottom - 1; fill(dc, &r, C_LINE);
    /* buttons from the right */
    x = tb.right - S(8);
    for (i = B_COUNT - 1; i >= 0; i--)
    {
        x -= bw;
        SetRect(&g_buttons[i], x, tb.top + S(4), x + bw, tb.bottom - S(4));
        x -= gap;
        if (i == B_ZOOMIN) x -= S(10);  /* groups: open | zoom | actions | view */
        if (i == B_EDIT) x -= S(10);
        if (i == B_ACTUAL) x -= S(10);
    }
    for (i = 0; i < B_COUNT; i++)
    {
        BOOL active = (i == B_INFO && g_info) || (i == B_FULLSCREEN && g_full);
        BOOL enabled = i == B_OPEN || i == B_MORE || i == B_FULLSCREEN || img.nframes > 0;
        r = g_buttons[i];
        if (enabled && (g_pressed == i && g_hot == i)) round_fill(dc, &r, S(8), C_PRESS, C_PRESS);
        else if (enabled && g_hot == i) round_fill(dc, &r, S(8), C_HOVER, C_HOVER);
        else if (active) round_fill(dc, &r, S(8), C_ACTIVE, C_ACTIVE);
        g = i == B_OPEN ? G_OPEN : i == B_ZOOMIN ? G_ZOOMIN : i == B_ZOOMOUT ? G_ZOOMOUT :
            i == B_ACTUAL ? (g_fit ? G_ACTUAL : G_FIT) : i == B_ROTATE ? G_ROTATE : i == B_DELETE ? G_DELETE :
            i == B_EDIT ? G_EDIT : i == B_SLIDESHOW ? G_SLIDESHOW : i == B_INFO ? G_INFO :
            i == B_FULLSCREEN ? (g_full ? G_UNFULL : G_FULLSCREEN) : G_MORE;
        blit_glyph(dc, g, (r.left + r.right - S(20)) / 2, (r.top + r.bottom - S(20)) / 2);
        if (!enabled)
        {
            /* dim it: wash with the toolbar colour */
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 160, 0 };
            HDC m = CreateCompatibleDC(dc);
            HBITMAP b = CreateCompatibleBitmap(dc, 1, 1);
            SelectObject(m, b);
            SetPixel(m, 0, 0, C_TOOLBAR);
            AlphaBlend(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, m, 0, 0, 1, 1, bf);
            DeleteDC(m); DeleteObject(b);
        }
    }
    /* the file's name, and where it is in the folder */
    r = tb; r.left += S(16); r.right = g_buttons[0].left - S(12);
    if (img.path[0])
    {
        RECT n = r;
        SIZE sz;
        HGDIOBJ of;
        lstrcpynW(s, PathFindFileNameW(img.path), MAX_PATH);
        draw_text(dc, g_font_title, C_TEXT, s, &n, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        of = SelectObject(dc, g_font_title);
        GetTextExtentPoint32W(dc, s, lstrlenW(s), &sz);
        SelectObject(dc, of);
        if (g_count > 1 && g_index >= 0 && r.left + sz.cx + S(16) < r.right)
        {
            n.left = r.left + sz.cx + S(12);
            _snwprintf(s, 64, L"%d of %d", g_index + 1, g_count);
            draw_text(dc, g_font, C_SUBTEXT, s, &n, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        }
    }
    else draw_text(dc, g_font_title, C_TEXT, APP_NAME, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
}

static void format_size(ULONGLONG n, WCHAR *out, int cch)
{
    StrFormatByteSizeW((LONGLONG)n, out, cch);
}

static void paint_info(HDC dc, const RECT *client)
{
    RECT p = *client, r;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    WCHAR v[MAX_PATH + 64], folder[MAX_PATH];
    int y;
    struct { const WCHAR *label; WCHAR value[MAX_PATH + 64]; } rows[7];
    int n = 0, i;

    p.left = p.right - info_width();
    p.top += toolbar_height();
    fill(dc, &p, C_TOOLBAR);
    r = p; r.right = r.left + 1; fill(dc, &r, C_LINE);
    r = p; r.left += S(20); r.right -= S(16); r.top += S(16); r.bottom = r.top + S(28);
    draw_text(dc, g_font_title, C_TEXT, L"File information", &r, DT_SINGLELINE | DT_VCENTER);
    y = r.bottom + S(12);
    if (!img.path[0]) return;

    rows[n].label = L"Name"; lstrcpynW(rows[n++].value, PathFindFileNameW(img.path), MAX_PATH);
    lstrcpynW(folder, img.path, MAX_PATH); PathRemoveFileSpecW(folder);
    rows[n].label = L"Folder"; lstrcpynW(rows[n++].value, folder, MAX_PATH);
    if (img.nframes)
    {
        rows[n].label = L"Dimensions";
        if (img.nframes > 1) _snwprintf(rows[n].value, MAX_PATH, L"%u x %u, %u frames", img.w, img.h, img.nframes);
        else _snwprintf(rows[n].value, MAX_PATH, L"%u x %u", img.w, img.h);
        n++;
    }
    if (GetFileAttributesExW(img.path, GetFileExInfoStandard, &fa))
    {
        SYSTEMTIME st, lt;
        WCHAR d[64] = L"", t[64] = L"";
        format_size(((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow, v, 64);
        rows[n].label = L"Size"; lstrcpyW(rows[n++].value, v);
        FileTimeToSystemTime(&fa.ftLastWriteTime, &st);
        SystemTimeToTzSpecificLocalTime(NULL, &st, &lt);
        GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &lt, NULL, d, 64);
        GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &lt, NULL, t, 64);
        rows[n].label = L"Date modified"; _snwprintf(rows[n++].value, MAX_PATH, L"%ls %ls", d, t);
    }
    if (img.taken[0]) { rows[n].label = L"Date taken"; lstrcpynW(rows[n++].value, img.taken, 64); }
    rows[n].label = L"Type"; lstrcpynW(rows[n++].value, img.kind, 64);

    for (i = 0; i < n; i++)
    {
        RECT l = { p.left + S(20), y, p.right - S(16), y + S(18) }, val;
        draw_text(dc, g_font, C_SUBTEXT, rows[i].label, &l, DT_SINGLELINE);
        val = l; val.top = l.bottom + S(2); val.bottom = val.top + S(200);
        {
            HGDIOBJ of = SelectObject(dc, g_font);
            RECT m = val;
            DrawTextW(dc, rows[i].value, -1, &m, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
            SelectObject(dc, of);
            val.bottom = val.top + (m.bottom - m.top);
        }
        draw_text(dc, g_font, C_TEXT, rows[i].value, &val, DT_WORDBREAK | DT_EDITCONTROL);
        y = val.bottom + S(14);
    }
}

static void paint_nav(HDC dc, const RECT *c)
{
    int w = S(36), h = S(64), cy = (c->top + c->bottom) / 2;
    SetRectEmpty(&g_nav_left); SetRectEmpty(&g_nav_right);
    if (g_count < 2 || g_slideshow || !g_nav_hover) return;
    if (g_index > 0)
    {
        SetRect(&g_nav_left, c->left + S(12), cy - h / 2, c->left + S(12) + w, cy + h / 2);
        round_fill(dc, &g_nav_left, S(8), g_nav_hot == 1 ? C_HOVER : C_TOOLBAR, RGB(0xD4, 0xD4, 0xD4));
        blit_glyph(dc, G_LEFT, g_nav_left.left + (w - S(20)) / 2, cy - S(10));
    }
    if (g_index < g_count - 1)
    {
        SetRect(&g_nav_right, c->right - S(12) - w, cy - h / 2, c->right - S(12), cy + h / 2);
        round_fill(dc, &g_nav_right, S(8), g_nav_hot == 2 ? C_HOVER : C_TOOLBAR, RGB(0xD4, 0xD4, 0xD4));
        blit_glyph(dc, G_RIGHT, g_nav_right.left + (w - S(20)) / 2, cy - S(10));
    }
}

static void paint_pill(HDC dc, const RECT *c)
{
    WCHAR s[32];
    RECT r;
    int w = S(76), h = S(32);
    if (GetTickCount() > g_pill_until || !img.nframes) return;
    _snwprintf(s, 32, L"%d%%", (int)(g_scale * 100 + 0.5));
    SetRect(&r, (c->left + c->right - w) / 2, c->bottom - S(24) - h, (c->left + c->right + w) / 2, c->bottom - S(24));
    round_fill(dc, &r, h, RGB(0x2B, 0x2B, 0x2B), RGB(0x2B, 0x2B, 0x2B));
    draw_text(dc, g_font_bold, RGB(0xFF, 0xFF, 0xFF), s, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
}

static void paint_empty(HDC dc, const RECT *c)
{
    RECT r = *c;
    int cy = (c->top + c->bottom) / 2;
    const WCHAR *msg = img.error ? L"We can't open this file" : L"Open a photo to see it here";
    r.top = cy - S(60); r.bottom = cy - S(20);
    draw_text(dc, g_font_big, C_TEXT, msg, &r, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    if (img.error)
    {
        RECT e = *c; e.top = cy - S(18); e.bottom = cy + S(4);
        draw_text(dc, g_font, C_SUBTEXT, L"It may be damaged, or in a format this PC can't read.", &e, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
    }
    SetRect(&g_open_button, (c->left + c->right) / 2 - S(60), cy + S(14), (c->left + c->right) / 2 + S(60), cy + S(46));
    round_fill(dc, &g_open_button, S(8), g_hot == B_COUNT ? RGB(0x80, 0x44, 0xD0) : C_ACCENT, C_ACCENT);
    draw_text(dc, g_font_bold, RGB(0xFF, 0xFF, 0xFF), L"Open a file", &g_open_button, DT_SINGLELINE | DT_CENTER | DT_VCENTER);
}

static void write_dump(void)
{
    FILE *f;
    RECT wr, c;
    POINT o = { 0, 0 };
    WCHAR title[MAX_PATH + 32];
    char line[4 * MAX_PATH];
    if (!g_dump[0] || !(f = _wfopen(g_dump, L"wb"))) return;
    GetWindowRect(g_hwnd, &wr);
    ClientToScreen(g_hwnd, &o);
    canvas_rect(&c);
    GetWindowTextW(g_hwnd, title, ARRAYSIZE(title));
    WideCharToMultiByte(CP_UTF8, 0, img.path, -1, line, sizeof(line), NULL, NULL);
    fprintf(f, "file %s\n", line);
    WideCharToMultiByte(CP_UTF8, 0, title, -1, line, sizeof(line), NULL, NULL);
    fprintf(f, "title %s\n", line);
    fprintf(f, "index %d of %d\n", g_index, g_count);
    fprintf(f, "image %u %u\n", img.w, img.h);
    fprintf(f, "error %d\n", img.error);
    fprintf(f, "scale %d\n", (int)(g_scale * 100 + 0.5));
    fprintf(f, "fit %d\n", g_fit);
    fprintf(f, "rotation %d\n", img.rotation);
    fprintf(f, "exif %d\n", img.exif);
    fprintf(f, "frames %u frame %u\n", img.nframes, img.frame);
    fprintf(f, "slideshow %d\n", g_slideshow);
    fprintf(f, "fullscreen %d\n", g_full);
    fprintf(f, "info %d\n", g_info);
    fprintf(f, "window %ld %ld %ld %ld\n", wr.left, wr.top, wr.right, wr.bottom);
    fprintf(f, "canvas %ld %ld %ld %ld\n", c.left + o.x, c.top + o.y, c.right + o.x, c.bottom + o.y);
    if (img.nframes)
        fprintf(f, "drawn %ld %ld %ld %ld\n", g_drawn.left + o.x, g_drawn.top + o.y, g_drawn.right + o.x, g_drawn.bottom + o.y);
    fprintf(f, "end\n");
    fclose(f);
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    RECT client, c;
    HDC dc = BeginPaint(hwnd, &ps), mem;
    HBITMAP bb;
    GetClientRect(hwnd, &client);
    if (client.right <= 0 || client.bottom <= 0) { EndPaint(hwnd, &ps); return; }
    mem = CreateCompatibleDC(dc);
    bb = CreateCompatibleBitmap(dc, client.right, client.bottom);
    SelectObject(mem, bb);
    canvas_rect(&c);
    fill(mem, &c, g_slideshow || g_full ? RGB(0, 0, 0) : C_CANVAS);
    SetRectEmpty(&g_open_button);
    if (img.nframes) paint_picture(mem, &c);
    else if (!g_slideshow) paint_empty(mem, &c);
    paint_nav(mem, &c);
    paint_pill(mem, &c);
    if (toolbar_height()) paint_toolbar(mem, &client);
    if (info_width()) paint_info(mem, &client);
    BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    DeleteDC(mem);
    DeleteObject(bb);
    EndPaint(hwnd, &ps);
    write_dump();
}

/* --------------------------------------------------------------- actions -- */

static void update_tips(void)
{
    int i;
    for (i = 0; i < B_COUNT && g_tip; i++)
    {
        TOOLINFOW ti = { sizeof(ti) };
        ti.hwnd = g_hwnd;
        ti.uId = i + 1;
        ti.rect = toolbar_height() ? g_buttons[i] : (RECT){ 0 };
        SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
}

static void set_fullscreen(BOOL on)
{
    DWORD style = GetWindowLongW(g_hwnd, GWL_STYLE);
    if (on == g_full) return;
    if (on)
    {
        MONITORINFO mi = { sizeof(mi) };
        GetWindowPlacement(g_hwnd, &g_placement);
        GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(g_hwnd, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        g_full = TRUE;
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    else
    {
        g_full = FALSE;
        SetWindowLongW(g_hwnd, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_placement);
        SetWindowPos(g_hwnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    relayout();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static BOOL g_full_before_slideshow;

static UINT slide_interval(void)
{
    WCHAR v[16];
    DWORD n = GetEnvironmentVariableW(L"SG_PHOTOS_SLIDE_MS", v, 16);
    UINT ms = n && n < 16 ? (UINT)_wtoi(v) : 0;
    return ms >= 200 ? ms : 3000;
}

static void slideshow(BOOL on)
{
    if (on == g_slideshow || (on && !img.nframes)) return;
    if (on)
    {
        g_full_before_slideshow = g_full;
        set_fullscreen(TRUE);
        g_slideshow = TRUE;
        SetTimer(g_hwnd, T_SLIDE, slide_interval(), NULL);
        while (ShowCursor(FALSE) >= 0) ;
    }
    else
    {
        g_slideshow = FALSE;
        KillTimer(g_hwnd, T_SLIDE);
        while (ShowCursor(TRUE) < 0) ;
        if (!g_full_before_slideshow) set_fullscreen(FALSE);
    }
    g_fit = TRUE;
    relayout();
    update_tips();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void toggle_info(void)
{
    g_info = !g_info;
    relayout();
    update_tips();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void do_rotate(void)
{
    if (!img.nframes) return;
    rotate_image();
    g_fit = TRUE;
    relayout();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void open_dialog(void)
{
    WCHAR file[MAX_PATH] = L"", filter[2400], pats[2200] = L"", *p, *e;
    OPENFILENAMEW ofn = { sizeof(ofn) };
    int n = 0;
    /* "*.png;*.jpg;..." from the extension list */
    for (p = g_exts + 1; *p; p = e + 1)
    {
        if (!(e = wcschr(p, ';'))) break;
        if (n + (e - p) + 3 >= (int)ARRAYSIZE(pats)) break;
        if (n) pats[n++] = ';';
        pats[n++] = '*';
        memcpy(pats + n, p, (e - p) * sizeof(WCHAR));
        n += (int)(e - p);
        pats[n] = 0;
    }
    n = _snwprintf(filter, ARRAYSIZE(filter) - 32, L"Pictures%lc%ls%lc", 0, pats, 0);
    if (n < 0) n = 0;
    memcpy(filter + n, L"All files\0*.*\0\0", 16 * sizeof(WCHAR));
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) open_path(file, TRUE);
}

static void copy_picture(void)
{
    HGLOBAL dib, drop;
    size_t pixels, n;
    UINT x, y;
    if (!img.nframes || !OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    /* the picture: a bottom-up 32-bit DIB, colours un-premultiplied */
    pixels = (size_t)img.w * img.h * 4;
    if ((dib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + pixels)))
    {
        BITMAPINFOHEADER *bh = GlobalLock(dib);
        BYTE *dst = (BYTE *)(bh + 1), *src = img.frames[img.frame].px;
        memset(bh, 0, sizeof(*bh));
        bh->biSize = sizeof(*bh); bh->biWidth = img.w; bh->biHeight = img.h; bh->biPlanes = 1;
        bh->biBitCount = 32; bh->biCompression = BI_RGB; bh->biSizeImage = (DWORD)pixels;
        for (y = 0; y < img.h; y++)
            for (x = 0; x < img.w; x++)
            {
                const BYTE *s = src + ((size_t)y * img.w + x) * 4;
                BYTE *d = dst + ((size_t)(img.h - 1 - y) * img.w + x) * 4;
                BYTE a = s[3];
                d[0] = a ? (BYTE)min(255, s[0] * 255 / a) : 0;
                d[1] = a ? (BYTE)min(255, s[1] * 255 / a) : 0;
                d[2] = a ? (BYTE)min(255, s[2] * 255 / a) : 0;
                d[3] = a;
            }
        GlobalUnlock(dib);
        if (!SetClipboardData(CF_DIB, dib)) GlobalFree(dib);
    }
    /* and the file, as Explorer's copy puts it */
    n = (lstrlenW(img.path) + 2) * sizeof(WCHAR);
    if ((drop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(DROPFILES) + n)))
    {
        DROPFILES *df = GlobalLock(drop);
        df->pFiles = sizeof(DROPFILES);
        df->fWide = TRUE;
        lstrcpyW((WCHAR *)(df + 1), img.path);
        GlobalUnlock(drop);
        if (!SetClipboardData(CF_HDROP, drop)) GlobalFree(drop);
    }
    CloseClipboard();
}

static void quote(WCHAR *out, int cch, const WCHAR *path)
{
    _snwprintf(out, cch, L"\"%ls\"", path);
    out[cch - 1] = 0;
}

static void edit_with_paint(void)
{
    WCHAR arg[MAX_PATH + 4], dir[MAX_PATH];
    if (!img.path[0]) return;
    quote(arg, ARRAYSIZE(arg), img.path);
    lstrcpynW(dir, img.path, MAX_PATH); PathRemoveFileSpecW(dir);
    if ((INT_PTR)ShellExecuteW(g_hwnd, NULL, L"mspaint.exe", arg, dir, SW_SHOWNORMAL) > 32) return;
    if ((INT_PTR)ShellExecuteW(g_hwnd, L"edit", img.path, NULL, dir, SW_SHOWNORMAL) > 32) return;
    MessageBoxW(g_hwnd, L"Paint isn't installed on this PC.", APP_NAME, MB_OK | MB_ICONINFORMATION);
}

static void open_location(void)
{
    WCHAR arg[MAX_PATH + 16];
    if (!img.path[0]) return;
    _snwprintf(arg, ARRAYSIZE(arg), L"/select,\"%ls\"", img.path);
    arg[ARRAYSIZE(arg) - 1] = 0;
    ShellExecuteW(g_hwnd, NULL, L"explorer.exe", arg, NULL, SW_SHOWNORMAL);
}

static void set_background(void)
{
    WCHAR arg[MAX_PATH + 32];
    if (!img.path[0]) return;
    _snwprintf(arg, ARRAYSIZE(arg), L"--set wallpaper \"%ls\" fill", img.path);
    arg[ARRAYSIZE(arg) - 1] = 0;
    ShellExecuteW(g_hwnd, NULL, L"control.exe", arg, NULL, SW_HIDE);
}

static void delete_picture(void)
{
    WCHAR from[MAX_PATH + 2] = { 0 };
    SHFILEOPSTRUCTW op = { 0 };
    int keep = g_index;
    if (!img.path[0] || img.error) return;
    if (MessageBoxW(g_hwnd, L"Are you sure you want to move this file to the Recycle Bin?", L"Delete file",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1) != IDYES) return;
    lstrcpynW(from, img.path, MAX_PATH);
    op.hwnd = g_hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
    if (SHFileOperationW(&op) || op.fAnyOperationsAborted || GetFileAttributesW(from) != INVALID_FILE_ATTRIBUTES)
    {
        MessageBoxW(g_hwnd, L"This file couldn't be deleted.", APP_NAME, MB_OK | MB_ICONERROR);
        return;
    }
    scan_folder(from);
    if (g_count)
    {
        if (keep >= g_count) keep = g_count - 1;
        if (keep < 0) keep = 0;
        go_to(keep);
    }
    else
    {
        free_image();
        img.path[0] = 0;
        update_title();
        InvalidateRect(g_hwnd, NULL, FALSE);
    }
}

/* Save: the rotation written back, in the file's own format; or a copy */
static BOOL save_to(const WCHAR *path)
{
    const WCHAR *ext = PathFindExtensionW(path);
    const GUID *fmt = &GUID_ContainerFormatPng;
    IWICStream *st = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *fe = NULL;
    IPropertyBag2 *bag = NULL;
    WCHAR tmp[MAX_PATH + 8];
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    BOOL ok = FALSE, opaque = TRUE;
    size_t i;
    BYTE *px;
    if (!img.nframes || !g_wic) return FALSE;
    if (!lstrcmpiW(ext, L".jpg") || !lstrcmpiW(ext, L".jpeg") || !lstrcmpiW(ext, L".jpe") || !lstrcmpiW(ext, L".jfif"))
        fmt = &GUID_ContainerFormatJpeg;
    else if (!lstrcmpiW(ext, L".bmp") || !lstrcmpiW(ext, L".dib")) fmt = &GUID_ContainerFormatBmp;
    else if (!lstrcmpiW(ext, L".tif") || !lstrcmpiW(ext, L".tiff")) fmt = &GUID_ContainerFormatTiff;
    else if (!lstrcmpiW(ext, L".gif")) fmt = &GUID_ContainerFormatGif;
    else if (lstrcmpiW(ext, L".png")) return FALSE;
    /* straight alpha for the encoder */
    if (!(px = malloc((size_t)img.w * img.h * 4))) return FALSE;
    memcpy(px, img.frames[img.frame].px, (size_t)img.w * img.h * 4);
    for (i = 0; i < (size_t)img.w * img.h; i++)
    {
        BYTE *p = px + i * 4, a = p[3];
        if (a != 255) opaque = FALSE;
        if (a && a != 255) { p[0] = (BYTE)min(255, p[0] * 255 / a); p[1] = (BYTE)min(255, p[1] * 255 / a); p[2] = (BYTE)min(255, p[2] * 255 / a); }
    }
    _snwprintf(tmp, ARRAYSIZE(tmp), L"%ls.sgtmp", path);
    tmp[ARRAYSIZE(tmp) - 1] = 0;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(g_wic, &st)) &&
        SUCCEEDED(IWICStream_InitializeFromFilename(st, tmp, GENERIC_WRITE)) &&
        SUCCEEDED(IWICImagingFactory_CreateEncoder(g_wic, fmt, NULL, &enc)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &fe, &bag)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Initialize(fe, bag)) &&
        SUCCEEDED(IWICBitmapFrameEncode_SetSize(fe, img.w, img.h)))
    {
        IWICBitmap *src = NULL;
        IWICBitmapSource *conv = NULL;
        if (opaque || fmt == &GUID_ContainerFormatJpeg) pf = GUID_WICPixelFormat24bppBGR;
        IWICBitmapFrameEncode_SetPixelFormat(fe, &pf);
        if (SUCCEEDED(IWICImagingFactory_CreateBitmapFromMemory(g_wic, img.w, img.h, &GUID_WICPixelFormat32bppBGRA,
                                                                img.w * 4, img.w * img.h * 4, px, &src)) &&
            SUCCEEDED(WICConvertBitmapSource(&pf, (IWICBitmapSource *)src, &conv)) &&
            SUCCEEDED(IWICBitmapFrameEncode_WriteSource(fe, conv, NULL)) &&
            SUCCEEDED(IWICBitmapFrameEncode_Commit(fe)) && SUCCEEDED(IWICBitmapEncoder_Commit(enc)))
            ok = TRUE;
        if (conv) IWICBitmapSource_Release(conv);
        if (src) IWICBitmap_Release(src);
    }
    if (bag) IPropertyBag2_Release(bag);
    if (fe) IWICBitmapFrameEncode_Release(fe);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (st) IWICStream_Release(st);
    free(px);
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp);
    return ok;
}

static void save(void)
{
    WCHAR path[MAX_PATH];
    if (!img.nframes || !img.rotation) return;
    if (img.nframes > 1 || img.exif > 1)
    {
        MessageBoxW(g_hwnd, img.nframes > 1 ? L"An animated picture can't be saved rotated. Use Save a copy for a still picture."
                                            : L"This photo's orientation is kept in its camera information. Use Save a copy to save it rotated.",
                    APP_NAME, MB_OK | MB_ICONINFORMATION);
        return;
    }
    lstrcpynW(path, img.path, MAX_PATH);
    if (!save_to(path)) { MessageBoxW(g_hwnd, L"This file couldn't be saved.", APP_NAME, MB_OK | MB_ICONERROR); return; }
    open_path(path, FALSE);
}

static void save_copy(void)
{
    WCHAR file[MAX_PATH];
    OPENFILENAMEW ofn = { sizeof(ofn) };
    if (!img.nframes) return;
    lstrcpynW(file, img.path, MAX_PATH);
    PathRemoveExtensionW(file);
    if (lstrlenW(file) + 12 < MAX_PATH) lstrcatW(file, L" (copy)");
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"PNG\0*.png\0JPEG\0*.jpg;*.jpeg\0BMP\0*.bmp\0TIFF\0*.tif;*.tiff\0\0";
    ofn.lpstrDefExt = L"png";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return;
    if (!save_to(file)) MessageBoxW(g_hwnd, L"This file couldn't be saved.", APP_NAME, MB_OK | MB_ICONERROR);
}

static void more_menu(void)
{
    HMENU m = CreatePopupMenu();
    UINT has = img.nframes ? MF_STRING : MF_STRING | MF_GRAYED;
    POINT pt = { g_buttons[B_MORE].right, g_buttons[B_MORE].bottom };
    int cmd;
    AppendMenuW(m, MF_STRING, M_OPEN, L"&Open...\tCtrl+O");
    AppendMenuW(m, has, M_SAVE, L"&Save\tCtrl+S");
    AppendMenuW(m, has, M_SAVECOPY, L"Save &a copy...\tCtrl+Shift+S");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, has, M_COPY, L"&Copy\tCtrl+C");
    AppendMenuW(m, has, M_ROTATE, L"&Rotate\tCtrl+R");
    AppendMenuW(m, has, M_EDIT, L"&Edit with Paint\tCtrl+E");
    AppendMenuW(m, has, M_DELETE, L"&Delete\tDelete");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, has, M_SLIDESHOW, L"S&lideshow\tF5");
    AppendMenuW(m, has, M_BACKGROUND, L"Set as &background");
    AppendMenuW(m, has, M_LOCATION, L"Open file &location");
    AppendMenuW(m, img.path[0] ? MF_STRING : MF_GRAYED, M_INFO, L"File &information\tAlt+Enter");
    ClientToScreen(g_hwnd, &pt);
    cmd = TrackPopupMenu(m, TPM_RIGHTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
    if (cmd) PostMessageW(g_hwnd, WM_COMMAND, cmd, 0);
}

static void button(int i)
{
    switch (i)
    {
    case B_OPEN: open_dialog(); break;
    case B_ZOOMIN: zoom_step(1, NULL); break;
    case B_ZOOMOUT: zoom_step(-1, NULL); break;
    case B_ACTUAL: if (g_fit) actual_size(); else set_fit(); break;
    case B_ROTATE: do_rotate(); break;
    case B_DELETE: delete_picture(); break;
    case B_EDIT: edit_with_paint(); break;
    case B_SLIDESHOW: slideshow(TRUE); break;
    case B_INFO: toggle_info(); break;
    case B_FULLSCREEN: set_fullscreen(!g_full); update_tips(); break;
    case B_MORE: more_menu(); break;
    }
}

static int hit_button(POINT p)
{
    int i;
    if (!toolbar_height()) return -1;
    for (i = 0; i < B_COUNT; i++) if (PtInRect(&g_buttons[i], p)) return i;
    if (PtInRect(&g_open_button, p)) return B_COUNT;
    return -1;
}

static BOOL key(WPARAM vk)
{
    BOOL ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (g_slideshow)
    {
        if (vk == VK_ESCAPE || vk == VK_F5) { slideshow(FALSE); return TRUE; }
        if (vk == VK_RIGHT || vk == VK_SPACE) { go(1, TRUE); SetTimer(g_hwnd, T_SLIDE, slide_interval(), NULL); return TRUE; }
        if (vk == VK_LEFT) { go(-1, TRUE); SetTimer(g_hwnd, T_SLIDE, slide_interval(), NULL); return TRUE; }
        return TRUE;
    }
    switch (vk)
    {
    case VK_RIGHT: go(1, FALSE); return TRUE;
    case VK_LEFT: go(-1, FALSE); return TRUE;
    case VK_HOME: go_to(0); return TRUE;
    case VK_END: go_to(g_count - 1); return TRUE;
    case VK_ADD: case VK_OEM_PLUS: zoom_step(1, NULL); return TRUE;
    case VK_SUBTRACT: case VK_OEM_MINUS: zoom_step(-1, NULL); return TRUE;
    case '0': case VK_NUMPAD0: if (ctrl) { set_fit(); return TRUE; } break;
    case '1': case VK_NUMPAD1: if (ctrl) { actual_size(); return TRUE; } break;
    case 'R': if (ctrl) { do_rotate(); return TRUE; } break;
    case 'C': if (ctrl) { copy_picture(); return TRUE; } break;
    case 'E': if (ctrl) { edit_with_paint(); return TRUE; } break;
    case 'O': if (ctrl) { open_dialog(); return TRUE; } break;
    case 'S': if (ctrl) { if (shift) save_copy(); else save(); return TRUE; } break;
    case 'I': if (ctrl) { toggle_info(); return TRUE; } break;
    case 'W': if (ctrl) { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return TRUE; } break;
    case VK_DELETE: delete_picture(); return TRUE;
    case VK_F5: slideshow(TRUE); return TRUE;
    case VK_F11: set_fullscreen(!g_full); update_tips(); return TRUE;
    case VK_ESCAPE: if (g_full) { set_fullscreen(FALSE); update_tips(); } return TRUE;
    }
    return FALSE;
}

static void track_leave(void)
{
    TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, g_hwnd, 0 };
    if (!g_tracking) { TrackMouseEvent(&t); g_tracking = TRUE; }
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT p;
    RECT c;
    switch (msg)
    {
    case WM_CREATE:
        g_hwnd = hwnd;
        DragAcceptFiles(hwnd, TRUE);
        return 0;
    case WM_SIZE:
        relayout();
        update_tips();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_DPICHANGED:
    {
        RECT *r = (RECT *)lp;
        g_dpi = HIWORD(wp);
        make_fonts();
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == T_ANIM && img.nframes > 1)
        {
            img.frame = (img.frame + 1) % img.nframes;
            start_animation();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        else if (wp == T_SLIDE) go(1, TRUE);
        else if (wp == T_PILL) { KillTimer(hwnd, T_PILL); InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (msg == WM_SYSKEYDOWN && wp == VK_RETURN) { toggle_info(); return 0; }
        if (msg == WM_SYSKEYDOWN && wp == VK_F10) break;
        if (key(wp)) return 0;
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN) return 0;  /* no beep for Alt+Enter */
        break;
    case WM_MOUSEWHEEL:
        p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        ScreenToClient(hwnd, &p);
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) zoom_step(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1, &p);
        else if (!g_slideshow) go(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1, FALSE);
        return 0;
    case WM_MOUSEMOVE:
    {
        int hot, nav;
        p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        track_leave();
        if (g_dragging)
        {
            g_ox = g_drag_ox + (p.x - g_drag_at.x);
            g_oy = g_drag_oy + (p.y - g_drag_at.y);
            clamp_offset();
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        hot = hit_button(p);
        nav = PtInRect(&g_nav_left, p) ? 1 : PtInRect(&g_nav_right, p) ? 2 : 0;
        canvas_rect(&c);
        if (hot != g_hot || nav != g_nav_hot || PtInRect(&c, p) != g_nav_hover)
        {
            g_hot = hot; g_nav_hot = nav; g_nav_hover = PtInRect(&c, p);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_tracking = FALSE;
        if (g_hot != -1 || g_nav_hover) { g_hot = -1; g_nav_hover = FALSE; g_nav_hot = 0; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN:
        p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        if (g_slideshow) { slideshow(FALSE); return 0; }
        if ((g_pressed = hit_button(p)) >= 0) { SetCapture(hwnd); InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if (PtInRect(&g_nav_left, p)) { go(-1, FALSE); return 0; }
        if (PtInRect(&g_nav_right, p)) { go(1, FALSE); return 0; }
        canvas_rect(&c);
        if (img.nframes && PtInRect(&c, p))
        {
            g_dragging = TRUE;
            g_drag_at = p; g_drag_ox = g_ox; g_drag_oy = g_oy;
            SetCapture(hwnd);
        }
        return 0;
    case WM_LBUTTONUP:
        p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        if (g_pressed >= 0)
        {
            int was = g_pressed;
            g_pressed = -1;
            ReleaseCapture();
            InvalidateRect(hwnd, NULL, FALSE);
            if (hit_button(p) == was) { if (was == B_COUNT) open_dialog(); else button(was); }
            return 0;
        }
        if (g_dragging) { g_dragging = FALSE; ReleaseCapture(); }
        return 0;
    case WM_LBUTTONDBLCLK:
        p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        canvas_rect(&c);
        if (img.nframes && PtInRect(&c, p) && hit_button(p) < 0)
        {
            if (g_fit) zoom_to(max(1.0, g_scale * 2), p.x - c.left, p.y - c.top);
            else set_fit();
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT)
        {
            RECT cr;
            GetCursorPos(&p); ScreenToClient(hwnd, &p);
            canvas_rect(&cr);
            if (g_slideshow) { SetCursor(NULL); return TRUE; }
            if (img.nframes && PtInRect(&cr, p) && !PtInRect(&g_nav_left, p) && !PtInRect(&g_nav_right, p) &&
                (img.w * g_scale > cr.right - cr.left + 1 || img.h * g_scale > cr.bottom - cr.top + 1))
            { SetCursor(LoadCursorW(NULL, IDC_SIZEALL)); return TRUE; }
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_CONTEXTMENU:
        if (img.nframes && !g_slideshow)
        {
            HMENU m = CreatePopupMenu();
            int cmd;
            AppendMenuW(m, MF_STRING, M_COPY, L"&Copy\tCtrl+C");
            AppendMenuW(m, MF_STRING, M_ROTATE, L"&Rotate\tCtrl+R");
            AppendMenuW(m, MF_STRING, M_EDIT, L"&Edit with Paint\tCtrl+E");
            AppendMenuW(m, MF_STRING, M_BACKGROUND, L"Set as &background");
            AppendMenuW(m, MF_STRING, M_LOCATION, L"Open file &location");
            AppendMenuW(m, MF_SEPARATOR, 0, NULL);
            AppendMenuW(m, MF_STRING, M_DELETE, L"&Delete\tDelete");
            AppendMenuW(m, MF_STRING, M_INFO, L"File &information\tAlt+Enter");
            p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
            if (p.x == -1 && p.y == -1) { p.x = p.y = 0; ClientToScreen(hwnd, &p); }
            cmd = TrackPopupMenu(m, TPM_RETURNCMD, p.x, p.y, 0, hwnd, NULL);
            DestroyMenu(m);
            if (cmd) PostMessageW(hwnd, WM_COMMAND, cmd, 0);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case M_OPEN: open_dialog(); break;
        case M_SAVE: save(); break;
        case M_SAVECOPY: save_copy(); break;
        case M_COPY: copy_picture(); break;
        case M_LOCATION: open_location(); break;
        case M_BACKGROUND: set_background(); break;
        case M_INFO: toggle_info(); break;
        case M_SLIDESHOW: slideshow(TRUE); break;
        case M_EDIT: edit_with_paint(); break;
        case M_DELETE: delete_picture(); break;
        case M_ROTATE: do_rotate(); break;
        }
        return 0;
    case WM_DROPFILES:
    {
        WCHAR f[MAX_PATH];
        if (DragQueryFileW((HDROP)wp, 0, f, MAX_PATH)) open_path(f, TRUE);
        DragFinish((HDROP)wp);
        SetForegroundWindow(hwnd);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && g_slideshow) slideshow(FALSE);
        break;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(480);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = S(320);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ms-photos:viewer?fileName=C:%5CUsers%5C... -> the path */
static BOOL from_uri(const WCHAR *uri, WCHAR *out)
{
    const WCHAR *q = StrStrIW(uri, L"fileName=");
    WCHAR tmp[MAX_PATH * 3];
    DWORD n = MAX_PATH;
    const WCHAR *e;
    if (!q) return FALSE;
    q += 9;
    e = wcschr(q, '&');
    lstrcpynW(tmp, q, e && e - q + 1 < (ptrdiff_t)ARRAYSIZE(tmp) ? (int)(e - q + 1) : (int)ARRAYSIZE(tmp));
    return SUCCEEDED(UrlUnescapeW(tmp, out, &n, 0));
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    WCHAR **argv, file[MAX_PATH] = L"";
    int argc, i, w, h;
    MSG msg;
    HACCEL acc = NULL;
    RECT work;
    typedef BOOL (WINAPI *ctx_fn)(HANDLE);
    ctx_fn set_ctx = (ctx_fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    (void)prev; (void)cmd;

    if (set_ctx) set_ctx((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */);
    else SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&g_wic);
    find_extensions();
    GetEnvironmentVariableW(L"SG_PHOTOS_DUMP", g_dump, MAX_PATH);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; i++)
    {
        if (!StrCmpNIW(argv[i], L"ms-photos:", 10)) { from_uri(argv[i], file); continue; }
        if (argv[i][0] == '/' && lstrlenW(argv[i]) < 12 && !wcschr(argv[i] + 1, '/')) continue;  /* switches */
        lstrcpynW(file, argv[i], MAX_PATH);
    }
    if (argv) LocalFree(argv);

    {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
        InitCommonControlsEx(&icc);
    }

    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    {
        HDC sdc = GetDC(NULL);
        g_dpi = GetDeviceCaps(sdc, LOGPIXELSY);
        ReleaseDC(NULL, sdc);
    }
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    w = min(S(1100), (work.right - work.left) * 9 / 10);
    h = min(S(760), (work.bottom - work.top) * 9 / 10);
    g_hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, CLASS_NAME, APP_NAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             work.left + (work.right - work.left - w) / 2, work.top + (work.bottom - work.top - h) / 2,
                             w, h, NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    {
        typedef UINT (WINAPI *dpi_fn)(HWND);
        dpi_fn f = (dpi_fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
        UINT d = f ? f(g_hwnd) : 0;
        if (d) g_dpi = d;
    }
    make_fonts();

    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                            0, 0, 0, 0, g_hwnd, NULL, inst, NULL);
    for (i = 0; g_tip && i < B_COUNT; i++)
    {
        TOOLINFOW ti = { sizeof(ti) };
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = g_hwnd;
        ti.uId = i + 1;
        ti.lpszText = (WCHAR *)TIPS[i];
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    }

    if (file[0]) open_path(file, TRUE);
    ShowWindow(g_hwnd, show == SW_SHOWDEFAULT || show == SW_HIDE ? SW_SHOWNORMAL : show);
    UpdateWindow(g_hwnd);
    {
        /* paint once so tool rectangles exist, then place the tips */
        update_tips();
    }
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        if (acc && TranslateAcceleratorW(g_hwnd, acc, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    free_image();
    free_list();
    if (g_wic) IWICImagingFactory_Release(g_wic);
    CoUninitialize();
    return (int)msg.wParam;
}
