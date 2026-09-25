/* sg-control -- desktop pictures: decode, fit to the screen, save.
 *
 * Wine's desktop draws only a BMP, tiled or centred. Windows itself keeps a
 * "transcoded" copy of the chosen picture for its desktop to draw; this does
 * the same: the picture is decoded with WIC (JPEG, PNG, BMP, GIF, TIFF),
 * fitted to the screen as the chosen style says (fill, fit, stretch, center,
 * span -- tile keeps the picture's own size), and saved as a BMP the desktop
 * draws 1:1. Resampling is done here (area-averaging down, bilinear up), not
 * left to a scaler that may pick nearest pixels.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define COBJMACROS
#include "control.h"
#include "wallpaper.h"
#include <wincodec.h>

/* decode to top-down BGRA; caller frees */
DWORD *image_load(const WCHAR *path, int *w, int *h)
{
    IWICImagingFactory *f = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapSource *conv = NULL;
    DWORD *px = NULL;
    UINT uw = 0, uh = 0;
    HRESULT hr, init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateDecoderFromFilename(f, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(dec, 0, &frame);
    if (SUCCEEDED(hr)) hr = WICConvertBitmapSource(&GUID_WICPixelFormat32bppBGRA, (IWICBitmapSource *)frame, &conv);
    if (SUCCEEDED(hr)) hr = IWICBitmapSource_GetSize(conv, &uw, &uh);
    if (SUCCEEDED(hr) && uw && uh && uw <= 16384 && uh <= 16384 && (px = malloc((size_t)uw * uh * 4))) {
        WICRect r = { 0, 0, (INT)uw, (INT)uh };
        if (FAILED(IWICBitmapSource_CopyPixels(conv, &r, uw * 4, uw * uh * 4, (BYTE *)px))) { free(px); px = NULL; }
    }
    if (conv) IWICBitmapSource_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (f) IWICImagingFactory_Release(f);
    if (SUCCEEDED(init)) CoUninitialize();
    if (!px) return NULL;
    *w = (int)uw; *h = (int)uh;
    return px;
}

/* Resample the source rectangle (sx, sy, sw, sh) of src (width stride_w)
 * into dst at (dx, dy), size (dw, dh), inside a dst of width dst_w. */
static void resample(const DWORD *src, int src_w, int src_h, double sx, double sy, double sw, double sh,
                     DWORD *dst, int dst_w, int dst_h, int dx, int dy, int dw, int dh)
{
    int x, y;
    double fx = sw / dw, fy = sh / dh;
    for (y = 0; y < dh; y++) {
        int oy = dy + y;
        if (oy < 0 || oy >= dst_h) continue;
        for (x = 0; x < dw; x++) {
            int ox = dx + x;
            double x0 = sx + x * fx, y0 = sy + y * fy;
            unsigned b = 0, g = 0, r = 0, n = 0;
            if (ox < 0 || ox >= dst_w) continue;
            if (fx >= 1.0 || fy >= 1.0) {           /* shrinking: average the box */
                int ix0 = (int)x0, iy0 = (int)y0, ix1 = (int)(x0 + fx), iy1 = (int)(y0 + fy), i, j;
                if (ix1 <= ix0) ix1 = ix0 + 1;
                if (iy1 <= iy0) iy1 = iy0 + 1;
                if (ix1 > src_w) ix1 = src_w;
                if (iy1 > src_h) iy1 = src_h;
                for (j = iy0; j < iy1; j++)
                    for (i = ix0; i < ix1; i++) {
                        DWORD p = src[(size_t)j * src_w + i];
                        b += p & 0xFF; g += (p >> 8) & 0xFF; r += (p >> 16) & 0xFF; n++;
                    }
                if (!n) n = 1;
                dst[(size_t)oy * dst_w + ox] = (r / n) << 16 | (g / n) << 8 | (b / n);
            } else {                                 /* enlarging: bilinear */
                double cx = x0 + fx / 2 - 0.5, cy = y0 + fy / 2 - 0.5, ax, ay;
                int ix, iy, k;
                DWORD p[4];
                double c[3] = { 0, 0, 0 };
                if (cx < 0) cx = 0;
                if (cy < 0) cy = 0;
                ix = (int)cx; iy = (int)cy;
                if (ix >= src_w - 1) ix = src_w - 2 < 0 ? 0 : src_w - 2;
                if (iy >= src_h - 1) iy = src_h - 2 < 0 ? 0 : src_h - 2;
                ax = cx - ix; ay = cy - iy;
                if (ax > 1) ax = 1;
                if (ay > 1) ay = 1;
                p[0] = src[(size_t)iy * src_w + ix];
                p[1] = src[(size_t)iy * src_w + (ix + 1 < src_w ? ix + 1 : ix)];
                p[2] = src[(size_t)(iy + 1 < src_h ? iy + 1 : iy) * src_w + ix];
                p[3] = src[(size_t)(iy + 1 < src_h ? iy + 1 : iy) * src_w + (ix + 1 < src_w ? ix + 1 : ix)];
                for (k = 0; k < 3; k++) {
                    double v0 = (p[0] >> (k * 8)) & 0xFF, v1 = (p[1] >> (k * 8)) & 0xFF;
                    double v2 = (p[2] >> (k * 8)) & 0xFF, v3 = (p[3] >> (k * 8)) & 0xFF;
                    c[k] = (v0 * (1 - ax) + v1 * ax) * (1 - ay) + (v2 * (1 - ax) + v3 * ax) * ay;
                }
                dst[(size_t)oy * dst_w + ox] = (DWORD)(c[2] + 0.5) << 16 | (DWORD)(c[1] + 0.5) << 8 | (DWORD)(c[0] + 0.5);
            }
        }
    }
}

/* Fit the picture into out_w x out_h as the style says; returns the new
 * buffer (caller frees). bg fills what the picture does not cover. */
DWORD *image_fit(const DWORD *src, int sw, int sh, int style, COLORREF bg, int out_w, int out_h)
{
    DWORD *out = malloc((size_t)out_w * out_h * 4), fill = GetRValue(bg) << 16 | GetGValue(bg) << 8 | GetBValue(bg);
    size_t i;
    double s;
    if (!out) return NULL;
    for (i = 0; i < (size_t)out_w * out_h; i++) out[i] = fill;
    switch (style) {
    case WP_STRETCH:
        resample(src, sw, sh, 0, 0, sw, sh, out, out_w, out_h, 0, 0, out_w, out_h);
        break;
    case WP_FIT: {
        int dw, dh;
        s = (double)out_w / sw < (double)out_h / sh ? (double)out_w / sw : (double)out_h / sh;
        dw = (int)(sw * s + 0.5); dh = (int)(sh * s + 0.5);
        resample(src, sw, sh, 0, 0, sw, sh, out, out_w, out_h, (out_w - dw) / 2, (out_h - dh) / 2, dw, dh);
        break;
    }
    case WP_CENTER: {
        /* the picture at its own size, cropped if bigger than the screen */
        int cw = sw < out_w ? sw : out_w, ch = sh < out_h ? sh : out_h;
        resample(src, sw, sh, (sw - cw) / 2.0, (sh - ch) / 2.0, cw, ch, out, out_w, out_h, (out_w - cw) / 2, (out_h - ch) / 2, cw, ch);
        break;
    }
    default: {                                       /* fill, span: cover, crop the overflow */
        double cw, ch;
        s = (double)out_w / sw > (double)out_h / sh ? (double)out_w / sw : (double)out_h / sh;
        cw = out_w / s; ch = out_h / s;
        resample(src, sw, sh, (sw - cw) / 2, (sh - ch) / 2, cw, ch, out, out_w, out_h, 0, 0, out_w, out_h);
        break;
    }
    }
    return out;
}

BOOL image_save_bmp(const WCHAR *path, const DWORD *px, int w, int h)
{
    BITMAPFILEHEADER fh = { 0 };
    BITMAPINFOHEADER ih = { sizeof(ih) };
    int stride = (w * 3 + 3) & ~3, y, x;
    BYTE *row = calloc(1, stride);
    HANDLE f;
    DWORD n;
    BOOL ok = TRUE;
    if (!row) return FALSE;
    ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24; ih.biCompression = BI_RGB;
    ih.biSizeImage = stride * h;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + ih.biSizeImage;
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { free(row); return FALSE; }
    ok = WriteFile(f, &fh, sizeof(fh), &n, NULL) && WriteFile(f, &ih, sizeof(ih), &n, NULL);
    for (y = h - 1; ok && y >= 0; y--) {                 /* bottom-up */
        for (x = 0; x < w; x++) {
            DWORD p = px[(size_t)y * w + x];
            row[x * 3] = p & 0xFF; row[x * 3 + 1] = (p >> 8) & 0xFF; row[x * 3 + 2] = (p >> 16) & 0xFF;
        }
        ok = WriteFile(f, row, stride, &n, NULL);
    }
    CloseHandle(f);
    free(row);
    if (!ok) DeleteFileW(path);
    return ok;
}

/* a small picture for a picker or a preview: fill-cropped, as an HBITMAP */
HBITMAP image_thumbnail(const DWORD *src, int sw, int sh, int style, COLORREF bg, int w, int h)
{
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB } };
    DWORD *bits, *fit = image_fit(src, sw, sh, style == WP_TILE ? WP_FILL : style, bg, w, h);
    HBITMAP b;
    if (!fit) return NULL;
    b = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (b) memcpy(bits, fit, (size_t)w * h * 4);
    free(fit);
    return b;
}
