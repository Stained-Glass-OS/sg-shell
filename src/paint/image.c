/* sg-paint -- the picture: pixels, undo, fill, transforms, files (WIC) and
 * the clipboard.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "paint.h"

Img g_img;
HDC g_imgdc;
static HBITMAP g_hbm, g_oldbm;

DWORD rgb2px(COLORREF c) { return 0xFF000000u | (GetRValue(c) << 16) | (GetGValue(c) << 8) | GetBValue(c); }
COLORREF px2rgb(DWORD p) { return RGB((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF); }

BOOL img_alloc(Img *im, int w, int h, COLORREF fill)
{
    DWORD v = rgb2px(fill);
    size_t i, n;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    n = (size_t)w * h;
    if (!(im->px = malloc(n * 4))) { im->w = im->h = 0; return FALSE; }
    im->w = w; im->h = h;
    for (i = 0; i < n; i++) im->px[i] = v;
    return TRUE;
}

void img_free(Img *im)
{
    free(im->px);
    im->px = NULL; im->w = im->h = 0;
}

BOOL img_copy(Img *dst, const Img *src)
{
    size_t n = (size_t)src->w * src->h * 4;
    if (!(dst->px = malloc(n ? n : 4))) return FALSE;
    memcpy(dst->px, src->px, n);
    dst->w = src->w; dst->h = src->h;
    return TRUE;
}

static HBITMAP make_dib(int w, int h, DWORD **bits)
{
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0 } };
    return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)bits, NULL, 0);
}

/* take over *im as the picture; *im is left empty */
void img_install(Img *im)
{
    DWORD *bits;
    HBITMAP bm = make_dib(im->w, im->h, &bits);
    if (!bm) { img_free(im); return; }
    memcpy(bits, im->px, (size_t)im->w * im->h * 4);
    if (!g_imgdc) g_imgdc = CreateCompatibleDC(NULL);
    if (g_hbm) { SelectObject(g_imgdc, g_oldbm); DeleteObject(g_hbm); }
    g_oldbm = SelectObject(g_imgdc, bm);
    g_hbm = bm;
    g_img.w = im->w; g_img.h = im->h; g_img.px = bits;
    img_free(im);
}

void img_new(int w, int h)
{
    Img im;
    if (img_alloc(&im, w, h, RGB(255, 255, 255))) img_install(&im);
}

/* ---- undo ---- */
#define UNDO_MAX 40
static Img g_undo[UNDO_MAX], g_redo[UNDO_MAX];
static int g_nundo, g_nredo;

static void stack_push(Img *stack, int *n, Img *im)
{
    if (*n == UNDO_MAX) { img_free(&stack[0]); memmove(stack, stack + 1, (UNDO_MAX - 1) * sizeof(Img)); (*n)--; }
    stack[(*n)++] = *im;
}

void undo_push(void)
{
    Img c;
    int i;
    if (!img_copy(&c, &g_img)) return;
    stack_push(g_undo, &g_nundo, &c);
    for (i = 0; i < g_nredo; i++) img_free(&g_redo[i]);
    g_nredo = 0;
}

void undo_drop_top(void) { if (g_nundo) img_free(&g_undo[--g_nundo]); }
const Img *undo_top(void) { return g_nundo ? &g_undo[g_nundo - 1] : NULL; }
int undo_count(void) { return g_nundo; }
int redo_count(void) { return g_nredo; }

void undo_clear(void)
{
    while (g_nundo) img_free(&g_undo[--g_nundo]);
    while (g_nredo) img_free(&g_redo[--g_nredo]);
}

BOOL do_undo(void)
{
    Img cur, prev;
    if (!g_nundo || !img_copy(&cur, &g_img)) return FALSE;
    prev = g_undo[--g_nundo];
    stack_push(g_redo, &g_nredo, &cur);
    img_install(&prev);
    return TRUE;
}

BOOL do_redo(void)
{
    Img cur, next;
    if (!g_nredo || !img_copy(&cur, &g_img)) return FALSE;
    next = g_redo[--g_nredo];
    stack_push(g_undo, &g_nundo, &cur);
    img_install(&next);
    return TRUE;
}

/* ---- fill: scanline flood fill of the exact colour under the point ---- */
void flood_fill(int x, int y, COLORREF c)
{
    DWORD *p = g_img.px, target, repl = rgb2px(c);
    int w = g_img.w, h = g_img.h, cap = 4096, n = 0;
    POINT *st;
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    target = p[y * w + x] & 0xFFFFFF;
    if (target == (repl & 0xFFFFFF)) return;
    if (!(st = malloc(cap * sizeof(POINT)))) return;
    st[n].x = x; st[n].y = y; n++;
    while (n)
    {
        int lx, rx, i, yy;
        POINT pt = st[--n];
        DWORD *row = p + (size_t)pt.y * w;
        if ((row[pt.x] & 0xFFFFFF) != target) continue;
        lx = rx = pt.x;
        while (lx > 0 && (row[lx - 1] & 0xFFFFFF) == target) lx--;
        while (rx < w - 1 && (row[rx + 1] & 0xFFFFFF) == target) rx++;
        for (i = lx; i <= rx; i++) row[i] = repl;
        for (yy = pt.y - 1; yy <= pt.y + 1; yy += 2)
        {
            DWORD *r2;
            BOOL in = FALSE;
            if (yy < 0 || yy >= h) continue;
            r2 = p + (size_t)yy * w;
            for (i = lx; i <= rx; i++)
            {
                BOOL m = (r2[i] & 0xFFFFFF) == target;
                if (m && !in)
                {
                    if (n == cap) { POINT *g = realloc(st, cap * 2 * sizeof(POINT)); if (!g) { free(st); return; } st = g; cap *= 2; }
                    st[n].x = i; st[n].y = yy; n++;
                }
                in = m;
            }
        }
    }
    free(st);
}

/* ---- transforms (any Img, including a selection) ---- */
void img_rotate(Img *im, int quarter)
{
    Img o;
    int x, y, w = im->w, h = im->h;
    quarter &= 3;
    if (!quarter) return;
    if (quarter == 2) { img_flip(im, TRUE); img_flip(im, FALSE); return; }
    if (!img_alloc(&o, h, w, 0)) return;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
        {
            DWORD v = im->px[(size_t)y * w + x];
            if (quarter == 1) o.px[(size_t)x * h + (h - 1 - y)] = v;       /* right */
            else              o.px[(size_t)(w - 1 - x) * h + y] = v;       /* left */
        }
    img_free(im);
    *im = o;
}

void img_flip(Img *im, BOOL vertical)
{
    int x, y, w = im->w, h = im->h;
    if (vertical)
        for (y = 0; y < h / 2; y++)
            for (x = 0; x < w; x++)
            {
                DWORD t = im->px[(size_t)y * w + x];
                im->px[(size_t)y * w + x] = im->px[(size_t)(h - 1 - y) * w + x];
                im->px[(size_t)(h - 1 - y) * w + x] = t;
            }
    else
        for (y = 0; y < h; y++)
            for (x = 0; x < w / 2; x++)
            {
                DWORD t = im->px[(size_t)y * w + x];
                im->px[(size_t)y * w + x] = im->px[(size_t)y * w + w - 1 - x];
                im->px[(size_t)y * w + w - 1 - x] = t;
            }
}

/* resample with a box filter going down and bilinear going up; keeps the
 * alpha byte (a selection's mask) as a channel of its own */
BOOL img_scale(Img *im, int w, int h)
{
    Img o;
    int x, y;
    if (w < 1 || h < 1 || w > 20000 || h > 20000) return FALSE;
    if (w == im->w && h == im->h) return TRUE;
    if (!img_alloc(&o, w, h, 0)) return FALSE;
    for (y = 0; y < h; y++)
    {
        double sy0 = (double)y * im->h / h, sy1 = (double)(y + 1) * im->h / h;
        for (x = 0; x < w; x++)
        {
            double sx0 = (double)x * im->w / w, sx1 = (double)(x + 1) * im->w / w;
            double acc[4] = { 0 }, tot = 0;
            int i, j, c;
            if (sx1 - sx0 <= 1.0 && sy1 - sy0 <= 1.0)
            {
                /* enlarging: bilinear from the centre */
                double fx = (x + 0.5) * im->w / w - 0.5, fy = (y + 0.5) * im->h / h - 0.5;
                int x0 = (int)floor(fx), y0 = (int)floor(fy);
                double ax = fx - x0, ay = fy - y0;
                for (j = 0; j < 2; j++)
                    for (i = 0; i < 2; i++)
                    {
                        int xx = min(max(x0 + i, 0), im->w - 1), yy = min(max(y0 + j, 0), im->h - 1);
                        double wt = (i ? ax : 1 - ax) * (j ? ay : 1 - ay);
                        DWORD v = im->px[(size_t)yy * im->w + xx];
                        for (c = 0; c < 4; c++) acc[c] += wt * ((v >> (8 * c)) & 0xFF);
                        tot += wt;
                    }
            }
            else
            {
                for (j = (int)sy0; j < (int)ceil(sy1) && j < im->h; j++)
                {
                    double wy = min(sy1, j + 1.0) - max(sy0, (double)j);
                    for (i = (int)sx0; i < (int)ceil(sx1) && i < im->w; i++)
                    {
                        double wt = wy * (min(sx1, i + 1.0) - max(sx0, (double)i));
                        DWORD v = im->px[(size_t)j * im->w + i];
                        if (wt <= 0) continue;
                        for (c = 0; c < 4; c++) acc[c] += wt * ((v >> (8 * c)) & 0xFF);
                        tot += wt;
                    }
                }
            }
            if (tot <= 0) tot = 1;
            o.px[(size_t)y * w + x] = ((DWORD)(acc[3] / tot + 0.5) << 24) | ((DWORD)(acc[2] / tot + 0.5) << 16) |
                                      ((DWORD)(acc[1] / tot + 0.5) << 8) | (DWORD)(acc[0] / tot + 0.5);
        }
    }
    img_free(im);
    *im = o;
    return TRUE;
}

/* skew by whole pixels per row (horizontal) or per column (vertical); the
 * uncovered corners take bg (alpha 0 in a selection: see-through) */
BOOL img_skew(Img *im, int hdeg, int vdeg, COLORREF bg)
{
    DWORD b = rgb2px(bg) & 0xFFFFFF;
    if (hdeg)
    {
        double t = tan(hdeg * M_PI / 180);
        int extra = (int)ceil(fabs(t) * im->h), w = im->w + extra, x, y;
        Img o;
        if (hdeg <= -90 || hdeg >= 90 || !img_alloc(&o, w, im->h, 0)) return FALSE;
        for (x = 0; x < w * im->h; x++) o.px[x] = b;
        for (y = 0; y < im->h; y++)
        {
            int off = (int)floor(t > 0 ? t * (im->h - 1 - y) : -t * y);
            for (x = 0; x < im->w; x++) o.px[(size_t)y * w + x + off] = im->px[(size_t)y * im->w + x];
        }
        img_free(im); *im = o;
    }
    if (vdeg)
    {
        double t = tan(vdeg * M_PI / 180);
        int extra = (int)ceil(fabs(t) * im->w), h = im->h + extra, x, y;
        Img o;
        if (vdeg <= -90 || vdeg >= 90 || !img_alloc(&o, im->w, h, 0)) return FALSE;
        for (x = 0; x < im->w * h; x++) o.px[x] = b;
        for (x = 0; x < im->w; x++)
        {
            int off = (int)floor(t > 0 ? t * (im->w - 1 - x) : -t * x);
            for (y = 0; y < im->h; y++) o.px[(size_t)(y + off) * im->w + x] = im->px[(size_t)y * im->w + x];
        }
        img_free(im); *im = o;
    }
    return TRUE;
}

/* ---- files, through WIC ---- */
static IWICImagingFactory *factory(void)
{
    static IWICImagingFactory *f;
    if (!f) CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
    return f;
}

BOOL img_load(const WCHAR *path, Img *out, WCHAR *err, int cch)
{
    IWICImagingFactory *f = factory();
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    UINT w = 0, h = 0, i;
    HRESULT hr;
    BOOL ok = FALSE;

    out->px = NULL;
    if (!f) { lstrcpynW(err, L"The imaging component is not available.", cch); return FALSE; }
    hr = IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(dec, 0, &frame);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(f, &conv);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppBGRA,
                                                          WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_GetSize(conv, &w, &h);
    if (SUCCEEDED(hr) && (w < 1 || h < 1 || w > 20000 || h > 20000)) hr = E_FAIL;
    if (SUCCEEDED(hr) && img_alloc(out, w, h, 0))
    {
        hr = IWICFormatConverter_CopyPixels(conv, NULL, w * 4, w * h * 4, (BYTE *)out->px);
        if (SUCCEEDED(hr))
        {
            /* what shows through is white, as in Paint */
            for (i = 0; i < w * h; i++)
            {
                DWORD v = out->px[i], a = v >> 24;
                if (a != 255)
                {
                    DWORD r = ((v >> 16) & 0xFF) * a / 255 + (255 - a), g = ((v >> 8) & 0xFF) * a / 255 + (255 - a),
                          b = (v & 0xFF) * a / 255 + (255 - a);
                    out->px[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
                }
            }
            ok = TRUE;
        }
        else img_free(out);
    }
    if (!ok)
        _snwprintf(err, cch, L"Paint cannot read this file.\n\nThis is not a valid bitmap file, or its format is not currently supported. (0x%08lx)", (unsigned long)hr);
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    return ok;
}

static const GUID *container_for(const WCHAR *path, BOOL *gif)
{
    const WCHAR *ext = wcsrchr(path, '.');
    *gif = FALSE;
    if (!ext) return &GUID_ContainerFormatPng;
    if (!lstrcmpiW(ext, L".jpg") || !lstrcmpiW(ext, L".jpeg") || !lstrcmpiW(ext, L".jpe") || !lstrcmpiW(ext, L".jfif"))
        return &GUID_ContainerFormatJpeg;
    if (!lstrcmpiW(ext, L".bmp") || !lstrcmpiW(ext, L".dib")) return &GUID_ContainerFormatBmp;
    if (!lstrcmpiW(ext, L".gif")) { *gif = TRUE; return &GUID_ContainerFormatGif; }
    if (!lstrcmpiW(ext, L".tif") || !lstrcmpiW(ext, L".tiff")) return &GUID_ContainerFormatTiff;
    return &GUID_ContainerFormatPng;
}

BOOL img_save(const WCHAR *path, const Img *im, WCHAR *err, int cch)
{
    IWICImagingFactory *f = factory();
    IWICStream *stream = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *props = NULL;
    IWICPalette *pal = NULL;
    WCHAR tmp[MAX_PATH + 8];
    WICPixelFormatGUID fmt;
    BOOL gif, ok = FALSE;
    const GUID *container = container_for(path, &gif);
    UINT stride, x, y;
    BYTE *buf = NULL;
    HRESULT hr = E_FAIL;

    if (!f) { lstrcpynW(err, L"The imaging component is not available.", cch); return FALSE; }
    _snwprintf(tmp, ARRAYSIZE(tmp), L"%ls.sgtmp", path);
    fmt = gif ? GUID_WICPixelFormat8bppIndexed : GUID_WICPixelFormat24bppBGR;
    stride = gif ? ((im->w + 3) & ~3u) : ((im->w * 3 + 3) & ~3u);
    if (!(buf = calloc(1, (size_t)stride * im->h))) goto done;
    for (y = 0; y < (UINT)im->h; y++)
        for (x = 0; x < (UINT)im->w; x++)
        {
            DWORD v = im->px[(size_t)y * im->w + x];
            BYTE r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
            if (gif) buf[(size_t)y * stride + x] = (BYTE)(((r + 25) / 51) * 36 + ((g + 25) / 51) * 6 + (b + 25) / 51);
            else { BYTE *d = buf + (size_t)y * stride + x * 3; d[0] = b; d[1] = g; d[2] = r; }
        }

    hr = IWICImagingFactory_CreateStream(f, &stream);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(stream, tmp, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(f, container, NULL, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream *)stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &frame, &props);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(frame, props);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(frame, im->w, im->h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt);
    if (SUCCEEDED(hr) && !IsEqualGUID(&fmt, gif ? &GUID_WICPixelFormat8bppIndexed : &GUID_WICPixelFormat24bppBGR)) hr = E_FAIL;
    if (SUCCEEDED(hr) && gif)
    {
        WICColor cols[216];
        int i;
        for (i = 0; i < 216; i++) cols[i] = 0xFF000000u | ((i / 36) * 51 << 16) | (((i / 6) % 6) * 51 << 8) | ((i % 6) * 51);
        hr = IWICImagingFactory_CreatePalette(f, &pal);
        if (SUCCEEDED(hr)) hr = IWICPalette_InitializeCustom(pal, cols, 216);
        if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPalette(frame, pal);
    }
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(frame, im->h, stride, stride * im->h, buf);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    ok = SUCCEEDED(hr);
done:
    if (pal) IWICPalette_Release(pal);
    if (props) IPropertyBag2_Release(props);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (enc) IWICBitmapEncoder_Release(enc);
    if (stream) IWICStream_Release(stream);
    free(buf);
    if (ok && !MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING)) { ok = FALSE; hr = HRESULT_FROM_WIN32(GetLastError()); }
    if (!ok)
    {
        DeleteFileW(tmp);
        _snwprintf(err, cch, L"Paint could not save this file. (0x%08lx)", (unsigned long)hr);
    }
    return ok;
}

/* ---- clipboard: CF_DIB, 24-bit bottom-up, which every program reads ---- */
BOOL clip_put(const Img *im)
{
    UINT stride = (im->w * 3 + 3) & ~3u, x, y;
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + (size_t)stride * im->h);
    BITMAPINFOHEADER *bih;
    BYTE *bits;
    if (!mem) return FALSE;
    bih = GlobalLock(mem);
    memset(bih, 0, sizeof(*bih));
    bih->biSize = sizeof(*bih); bih->biWidth = im->w; bih->biHeight = im->h; bih->biPlanes = 1;
    bih->biBitCount = 24; bih->biCompression = BI_RGB; bih->biSizeImage = stride * im->h;
    bits = (BYTE *)(bih + 1);
    for (y = 0; y < (UINT)im->h; y++)
    {
        BYTE *d = bits + (size_t)(im->h - 1 - y) * stride;
        for (x = 0; x < (UINT)im->w; x++)
        {
            DWORD v = im->px[(size_t)y * im->w + x];
            d[x * 3] = v & 0xFF; d[x * 3 + 1] = (v >> 8) & 0xFF; d[x * 3 + 2] = (v >> 16) & 0xFF;
        }
    }
    GlobalUnlock(mem);
    if (!OpenClipboard(g_main)) { GlobalFree(mem); return FALSE; }
    EmptyClipboard();
    if (!SetClipboardData(CF_DIB, mem)) { GlobalFree(mem); CloseClipboard(); return FALSE; }
    CloseClipboard();
    return TRUE;
}

BOOL clip_get(Img *out)
{
    HGLOBAL mem;
    BITMAPINFO *bi;
    BOOL ok = FALSE;
    out->px = NULL;
    if (!IsClipboardFormatAvailable(CF_DIB) || !OpenClipboard(g_main)) return FALSE;
    if ((mem = GetClipboardData(CF_DIB)) && (bi = GlobalLock(mem)))
    {
        BITMAPINFOHEADER *h = &bi->bmiHeader;
        int w = h->biWidth, ht = abs(h->biHeight);
        DWORD ncol = h->biClrUsed ? h->biClrUsed : (h->biBitCount <= 8 ? 1u << h->biBitCount : 0);
        BYTE *bits = (BYTE *)bi + h->biSize + ncol * sizeof(RGBQUAD);
        if (h->biCompression == BI_BITFIELDS && h->biSize == sizeof(BITMAPINFOHEADER)) bits += 12;
        if (w > 0 && ht > 0 && w <= 20000 && ht <= 20000 && img_alloc(out, w, ht, RGB(255, 255, 255)))
        {
            DWORD *dib;
            HBITMAP bm = make_dib(w, ht, &dib);
            if (bm)
            {
                HDC dc = CreateCompatibleDC(NULL);
                HGDIOBJ old = SelectObject(dc, bm);
                size_t i;
                SetDIBitsToDevice(dc, 0, 0, w, ht, 0, 0, 0, ht, bits, bi, DIB_RGB_COLORS);
                GdiFlush();
                for (i = 0; i < (size_t)w * ht; i++) out->px[i] = dib[i] | 0xFF000000u;
                SelectObject(dc, old); DeleteDC(dc); DeleteObject(bm);
                ok = TRUE;
            }
            else img_free(out);
        }
        GlobalUnlock(mem);
    }
    CloseClipboard();
    return ok;
}
