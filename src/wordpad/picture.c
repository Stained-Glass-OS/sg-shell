/* sg-wordpad -- pictures: decoding files and embedded images through WIC into
 * the DIBs RichEdit reads (\dibitmap0), and encoding pictures as PNG for the
 * .docx and .odt packages (metafiles are played into a bitmap first).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "wordpad.h"

static IWICImagingFactory *factory(void)
{
    static IWICImagingFactory *f;
    if (!f) CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
    return f;
}

/* a WIC source to a packed 24-bit bottom-up DIB (header + bits) */
static BOOL source_to_dib(IWICBitmapSource *src, BYTE **dib, DWORD *dibsize, int *wpx, int *hpx)
{
    IWICFormatConverter *conv = NULL;
    UINT w, h, stride, rowbytes;
    BYTE *buf, *out;
    BITMAPINFOHEADER *bh;
    BOOL ok = FALSE;
    if (FAILED(IWICImagingFactory_CreateFormatConverter(factory(), &conv))) return FALSE;
    if (FAILED(IWICFormatConverter_Initialize(conv, src, &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0,
                                              WICBitmapPaletteTypeCustom))) goto done;
    IWICFormatConverter_GetSize(conv, &w, &h);
    if (!w || !h || w > 20000 || h > 20000) goto done;
    stride = w * 4;
    buf = malloc((size_t)stride * h);
    if (!buf) goto done;
    if (FAILED(IWICFormatConverter_CopyPixels(conv, NULL, stride, stride * h, buf))) { free(buf); goto done; }
    rowbytes = (w * 3 + 3) & ~3u;
    *dibsize = sizeof(BITMAPINFOHEADER) + rowbytes * h;
    out = calloc(1, *dibsize);
    bh = (BITMAPINFOHEADER *)out;
    bh->biSize = sizeof(*bh); bh->biWidth = w; bh->biHeight = h; bh->biPlanes = 1; bh->biBitCount = 24;
    bh->biCompression = BI_RGB; bh->biSizeImage = rowbytes * h;
    bh->biXPelsPerMeter = bh->biYPelsPerMeter = 3780;
    for (UINT y = 0; y < h; y++)
    {
        const BYTE *s = buf + (size_t)y * stride;
        BYTE *d = out + sizeof(*bh) + (size_t)(h - 1 - y) * rowbytes;
        for (UINT x = 0; x < w; x++)
        {
            /* composite on white: transparent parts of PNGs are paper */
            unsigned a = s[x * 4 + 3];
            d[x * 3 + 0] = (BYTE)((s[x * 4 + 0] * a + 255 * (255 - a)) / 255);
            d[x * 3 + 1] = (BYTE)((s[x * 4 + 1] * a + 255 * (255 - a)) / 255);
            d[x * 3 + 2] = (BYTE)((s[x * 4 + 2] * a + 255 * (255 - a)) / 255);
        }
    }
    free(buf);
    *dib = out; *wpx = w; *hpx = h;
    ok = TRUE;
done:
    IWICFormatConverter_Release(conv);
    return ok;
}

static BOOL decoder_to_dib(IWICBitmapDecoder *dec, BYTE **dib, DWORD *dibsize, int *wpx, int *hpx)
{
    IWICBitmapFrameDecode *frame = NULL;
    BOOL ok = FALSE;
    if (SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)))
    {
        ok = source_to_dib((IWICBitmapSource *)frame, dib, dibsize, wpx, hpx);
        IWICBitmapFrameDecode_Release(frame);
    }
    return ok;
}

BOOL pic_load_file(const WCHAR *path, BYTE **dib, DWORD *dibsize, int *wpx, int *hpx)
{
    IWICBitmapDecoder *dec = NULL;
    BOOL ok;
    if (!factory()) return FALSE;
    if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(factory(), path, NULL, GENERIC_READ,
                                                            WICDecodeMetadataCacheOnDemand, &dec))) return FALSE;
    ok = decoder_to_dib(dec, dib, dibsize, wpx, hpx);
    IWICBitmapDecoder_Release(dec);
    return ok;
}

BOOL pic_decode_to_dib(const BYTE *data, DWORD size, BYTE **dib, DWORD *dibsize, int *wpx, int *hpx)
{
    IWICStream *st = NULL;
    IWICBitmapDecoder *dec = NULL;
    BOOL ok = FALSE;
    if (!factory() || FAILED(IWICImagingFactory_CreateStream(factory(), &st))) return FALSE;
    if (SUCCEEDED(IWICStream_InitializeFromMemory(st, (BYTE *)data, size)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(factory(), (IStream *)st, NULL, WICDecodeMetadataCacheOnDemand, &dec)))
    {
        ok = decoder_to_dib(dec, dib, dibsize, wpx, hpx);
        IWICBitmapDecoder_Release(dec);
    }
    IWICStream_Release(st);
    return ok;
}

/* 32-bit BGRA pixels to PNG bytes */
static BOOL encode_png(const BYTE *bgra, int w, int h, BYTE **png, DWORD *pngsize)
{
    IStream *mem = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameDecode *unused = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    IPropertyBag2 *props = NULL;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    HGLOBAL hg;
    BOOL ok = FALSE;
    (void)unused;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &mem))) return FALSE;
    if (FAILED(IWICImagingFactory_CreateEncoder(factory(), &GUID_ContainerFormatPng, NULL, &enc))) goto done;
    if (FAILED(IWICBitmapEncoder_Initialize(enc, mem, WICBitmapEncoderNoCache))) goto done;
    if (FAILED(IWICBitmapEncoder_CreateNewFrame(enc, &frame, &props))) goto done;
    if (FAILED(IWICBitmapFrameEncode_Initialize(frame, props))) goto done;
    IWICBitmapFrameEncode_SetSize(frame, w, h);
    IWICBitmapFrameEncode_SetResolution(frame, 96, 96);
    IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt);
    if (FAILED(IWICBitmapFrameEncode_WritePixels(frame, h, w * 4, w * 4 * h, (BYTE *)bgra))) goto done;
    if (FAILED(IWICBitmapFrameEncode_Commit(frame)) || FAILED(IWICBitmapEncoder_Commit(enc))) goto done;
    if (SUCCEEDED(GetHGlobalFromStream(mem, &hg)))
    {
        STATSTG ss;
        if (SUCCEEDED(IStream_Stat(mem, &ss, STATFLAG_NONAME)))
        {
            void *p = GlobalLock(hg);
            *pngsize = (DWORD)ss.cbSize.QuadPart;
            *png = malloc(*pngsize);
            memcpy(*png, p, *pngsize);
            GlobalUnlock(hg);
            ok = TRUE;
        }
    }
done:
    if (props) IPropertyBag2_Release(props);
    if (frame) IWICBitmapFrameEncode_Release(frame);
    if (enc) IWICBitmapEncoder_Release(enc);
    IStream_Release(mem);
    return ok;
}

BOOL pic_to_png(int type, const BYTE *data, DWORD size, int w_twips, int h_twips, BYTE **png, DWORD *pngsize, int *wpx, int *hpx)
{
    BYTE *dib = NULL;
    DWORD dsz;
    BOOL ok = FALSE;
    if (!factory()) return FALSE;
    if (type == PIC_PNG && size > 24 && !memcmp(data, "\x89PNG", 4))
    {
        *png = malloc(size);
        memcpy(*png, data, size);
        *pngsize = size;
        *wpx = (int)(data[16] << 24 | data[17] << 16 | data[18] << 8 | data[19]);
        *hpx = (int)(data[20] << 24 | data[21] << 16 | data[22] << 8 | data[23]);
        return TRUE;
    }
    if (type == PIC_EMF || type == PIC_WMF)
    {
        /* play the metafile into a bitmap at its shown size (96 dpi) */
        int w = max(1, w_twips / 15), h = max(1, h_twips / 15);
        HENHMETAFILE emf;
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0 } };
        DWORD *px;
        HDC dc;
        HBITMAP bmp;
        HGDIOBJ old;
        RECT r = { 0, 0, w, h };
        if (type == PIC_EMF) emf = SetEnhMetaFileBits(size, data);
        else
        {
            METAFILEPICT mfp = { MM_ANISOTROPIC, w_twips * 254 / 144, h_twips * 254 / 144, NULL };
            emf = SetWinMetaFileBits(size, data, NULL, &mfp);
        }
        if (!emf) return FALSE;
        if (!w_twips || !h_twips)
        {
            ENHMETAHEADER eh;
            GetEnhMetaFileHeader(emf, sizeof(eh), &eh);
            w = max(1, eh.rclBounds.right - eh.rclBounds.left + 1);
            h = max(1, eh.rclBounds.bottom - eh.rclBounds.top + 1);
            bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;
            SetRect(&r, 0, 0, w, h);
        }
        dc = CreateCompatibleDC(NULL);
        bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
        old = SelectObject(dc, bmp);
        FillRect(dc, &r, GetStockObject(WHITE_BRUSH));
        PlayEnhMetaFile(dc, emf, &r);
        GdiFlush();
        for (int i = 0; i < w * h; i++) px[i] |= 0xFF000000;
        ok = encode_png((BYTE *)px, w, h, png, pngsize);
        *wpx = w; *hpx = h;
        SelectObject(dc, old); DeleteObject(bmp); DeleteDC(dc);
        DeleteEnhMetaFile(emf);
        return ok;
    }
    if (type == PIC_DIB) { dib = (BYTE *)data; dsz = size; }
    else if (!pic_decode_to_dib(data, size, &dib, &dsz, wpx, hpx)) return FALSE;
    {
        /* any packed DIB to 32-bit top-down through GDI */
        const BITMAPINFO *bi = (const BITMAPINFO *)dib;
        int w = bi->bmiHeader.biWidth, h = abs(bi->bmiHeader.biHeight);
        unsigned nc = bi->bmiHeader.biClrUsed;
        BITMAPINFO out = { { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0 } };
        DWORD *px;
        HDC dc = CreateCompatibleDC(NULL);
        HBITMAP bmp = CreateDIBSection(NULL, &out, DIB_RGB_COLORS, (void **)&px, NULL, 0);
        const BYTE *bits;
        if (!nc && bi->bmiHeader.biBitCount <= 8) nc = 1u << bi->bmiHeader.biBitCount;
        if (bi->bmiHeader.biCompression == BI_BITFIELDS) nc = 3;
        bits = (const BYTE *)bi + bi->bmiHeader.biSize + nc * sizeof(RGBQUAD);
        if (bmp && w > 0 && h > 0)
        {
            SetDIBits(dc, bmp, 0, h, bits, bi, DIB_RGB_COLORS);
            for (int i = 0; i < w * h; i++) px[i] |= 0xFF000000;
            ok = encode_png((BYTE *)px, w, h, png, pngsize);
            *wpx = w; *hpx = h;
        }
        if (bmp) DeleteObject(bmp);
        DeleteDC(dc);
    }
    if (dib != data) free(dib);
    return ok;
}
