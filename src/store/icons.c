/* SG Store -- each app's picture on its card.
 *
 * The catalogue's Icon is where an app's picture is: its maker's or Flathub's
 * web address (https:), or a file (C:\..., Z:\usr\share\...). We ship no
 * maker's artwork: a picture is fetched when the store first shows it and
 * kept in the person's cache (%LOCALAPPDATA%\Stained Glass\Store\Icons, a
 * month); offline, or without an Icon, the card keeps its letter badge.
 * PNG and ICO pictures (the largest of an ICO's), decoded with WIC, scaled
 * down with an area average (Wine's WIC scaler only picks nearest pixels) to
 * a premultiplied 32-bit bitmap that the card draws with AlphaBlend.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define COBJMACROS
#include "store.h"
#include <objbase.h>
#include <wincodec.h>
#include <wininet.h>

#define MAX_ICON_BYTES (1u << 20)
#define CACHE_DAYS 30
#define WORKERS 6

typedef struct {
    WCHAR url[512];
    HBITMAP bmp;            /* set once, when ready */
} icon_ent;

static icon_ent g_ent[MAX_APPS];
static LONG g_nent, g_next, g_pending;
static int g_px;
static HWND g_notify;
static UINT g_msg;

static HINTERNET g_inet;

static unsigned hash(const WCHAR *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned)*s) * 16777619u;
    return h;
}

static BOOL cache_path(const WCHAR *url, WCHAR *out, int cch)
{
    WCHAR dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"SG_STORE_ICON_CACHE", dir, MAX_PATH);
    if (!n || n >= MAX_PATH) {
        if (!(n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) || n >= MAX_PATH - 40) return FALSE;
        lstrcatW(dir, L"\\Stained Glass");
        CreateDirectoryW(dir, NULL);
        lstrcatW(dir, L"\\Store");
        CreateDirectoryW(dir, NULL);
        lstrcatW(dir, L"\\Icons");
    }
    CreateDirectoryW(dir, NULL);
    swprintf(out, cch, L"%ls\\%08x.img", dir, hash(url));
    return TRUE;
}

static BYTE *read_file(const WCHAR *path, DWORD *size)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    BYTE *buf = NULL;
    DWORD n, got = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    n = GetFileSize(h, NULL);
    if (n && n <= MAX_ICON_BYTES && (buf = malloc(n)) && (!ReadFile(h, buf, n, &got, NULL) || got != n)) { free(buf); buf = NULL; }
    CloseHandle(h);
    *size = got;
    return buf;
}

static BOOL fresh(const WCHAR *path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    FILETIME now;
    ULARGE_INTEGER a, b;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) return FALSE;
    GetSystemTimeAsFileTime(&now);
    a.LowPart = fa.ftLastWriteTime.dwLowDateTime; a.HighPart = fa.ftLastWriteTime.dwHighDateTime;
    b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return b.QuadPart < a.QuadPart + (ULONGLONG)CACHE_DAYS * 864000000000ull;
}

static BYTE *download(const WCHAR *url, DWORD *size)
{
    HINTERNET h;
    DWORD st = 0, len = sizeof(st), got, n = 0;
    BYTE *buf;
    if (!g_inet) return NULL;
    if (!(h = InternetOpenUrlW(g_inet, url, L"Accept: image/png, image/x-icon, image/*\r\n", (DWORD)-1,
                               INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_COOKIES, 0)))
        return NULL;
    if (!HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &st, &len, NULL) || st != 200 ||
        !(buf = malloc(MAX_ICON_BYTES))) {
        InternetCloseHandle(h);
        return NULL;
    }
    while (n < MAX_ICON_BYTES && InternetReadFile(h, buf + n, MAX_ICON_BYTES - n, &got) && got) n += got;
    InternetCloseHandle(h);
    if (!n || n >= MAX_ICON_BYTES) { free(buf); return NULL; }
    *size = n;
    return buf;
}

/* the picture's bytes: a file, the cache, or the web (then cached) */
static BYTE *fetch(const WCHAR *url, DWORD *size)
{
    WCHAR cache[MAX_PATH], tmp[MAX_PATH + 8];
    BYTE *buf;
    HANDLE h;
    DWORD put;
    if (_wcsnicmp(url, L"https://", 8) && _wcsnicmp(url, L"http://", 7)) return read_file(url, size);
    if (!cache_path(url, cache, MAX_PATH)) return download(url, size);
    if (fresh(cache) && (buf = read_file(cache, size))) return buf;
    if (!(buf = download(url, size))) return read_file(cache, size);   /* a stale one beats none */
    swprintf(tmp, ARRAYSIZE(tmp), L"%ls.%lu", cache, GetCurrentThreadId());
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        BOOL ok = WriteFile(h, buf, *size, &put, NULL) && put == *size;
        CloseHandle(h);
        if (!ok || !MoveFileExW(tmp, cache, MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp);
    }
    return buf;
}

/* bytes -> premultiplied BGRA, px x px, the picture centred in it */
static HBITMAP decode(const BYTE *data, DWORD size, int px)
{
    IWICImagingFactory *fac = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL, *best = NULL;
    IWICFormatConverter *conv = NULL;
    IStream *stream = NULL;
    UINT frames = 0, i, w = 0, h = 0, bw = 0, bh = 0;
    BYTE *src = NULL;
    HBITMAP bmp = NULL;

    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&fac)))
        return NULL;
    if (!(stream = SHCreateMemStream(data, size))) goto done;
    if (FAILED(IWICImagingFactory_CreateDecoderFromStream(fac, stream, NULL, WICDecodeMetadataCacheOnDemand, &dec))) goto done;
    IWICBitmapDecoder_GetFrameCount(dec, &frames);
    for (i = 0; i < frames && i < 32; i++) {          /* an ICO's largest picture */
        if (FAILED(IWICBitmapDecoder_GetFrame(dec, i, &frame))) continue;
        IWICBitmapFrameDecode_GetSize(frame, &w, &h);
        if (!best || w * h > bw * bh) {
            if (best) IWICBitmapFrameDecode_Release(best);
            best = frame; bw = w; bh = h;
        } else IWICBitmapFrameDecode_Release(frame);
    }
    if (!best || !bw || !bh || bw > 2048 || bh > 2048) goto done;
    if (FAILED(IWICImagingFactory_CreateFormatConverter(fac, &conv)) ||
        FAILED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)best, &GUID_WICPixelFormat32bppPBGRA,
                                              WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)) ||
        !(src = malloc((size_t)bw * bh * 4)) ||
        FAILED(IWICFormatConverter_CopyPixels(conv, NULL, bw * 4, bw * bh * 4, src)))
        goto done;
    {
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), px, -px, 1, 32, BI_RGB } };
        BYTE *dst;
        int big = max(bw, bh), ow = (int)((ULONGLONG)bw * px / big), oh = (int)((ULONGLONG)bh * px / big);
        int ox = (px - ow) / 2, oy = (px - oh) / 2, x, y;
        if (!ow) ow = 1;
        if (!oh) oh = 1;
        if (!(bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&dst, NULL, 0))) goto done;
        memset(dst, 0, (size_t)px * px * 4);
        for (y = 0; y < oh; y++) {
            UINT y0 = (UINT)((ULONGLONG)y * bh / oh), y1 = (UINT)((ULONGLONG)(y + 1) * bh / oh);
            if (y1 <= y0) y1 = y0 + 1;
            for (x = 0; x < ow; x++) {
                UINT x0 = (UINT)((ULONGLONG)x * bw / ow), x1 = (UINT)((ULONGLONG)(x + 1) * bw / ow), sx, sy, cnt = 0;
                ULONG acc[4] = { 0, 0, 0, 0 };
                BYTE *o = dst + ((size_t)(oy + y) * px + ox + x) * 4;
                if (x1 <= x0) x1 = x0 + 1;
                for (sy = y0; sy < y1 && sy < bh; sy++)
                    for (sx = x0; sx < x1 && sx < bw; sx++) {
                        const BYTE *s = src + ((size_t)sy * bw + sx) * 4;
                        acc[0] += s[0]; acc[1] += s[1]; acc[2] += s[2]; acc[3] += s[3]; cnt++;
                    }
                if (cnt) { o[0] = acc[0] / cnt; o[1] = acc[1] / cnt; o[2] = acc[2] / cnt; o[3] = acc[3] / cnt; }
            }
        }
        GdiFlush();
    }
done:
    free(src);
    if (conv) IWICFormatConverter_Release(conv);
    if (best) IWICBitmapFrameDecode_Release(best);
    if (dec) IWICBitmapDecoder_Release(dec);
    if (stream) IStream_Release(stream);
    IWICImagingFactory_Release(fac);
    return bmp;
}

static LONG g_redo;  /* a new size asked for while a pass ran */
static void start_pass(void);

static DWORD WINAPI worker(void *arg)
{
    LONG i;
    (void)arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    while ((i = InterlockedIncrement(&g_next) - 1) < g_nent) {
        DWORD size = 0;
        BYTE *data = fetch(g_ent[i].url, &size);
        if (data) {
            HBITMAP bmp = decode(data, size, g_px);
            if (bmp) g_ent[i].bmp = bmp;   /* (one made at another scale is left: it may be being drawn) */
            free(data);
        }
        if (g_ent[i].bmp && g_notify) PostMessageW(g_notify, g_msg, 0, 0);
    }
    CoUninitialize();
    if (!InterlockedDecrement(&g_pending))
    {
        if (InterlockedExchange(&g_redo, 0)) start_pass();
        else if (g_notify) PostMessageW(g_notify, g_msg, 1, 0);
    }
    return 0;
}

static void start_pass(void)
{
    int i, threads = min(WORKERS, g_nent);
    g_next = 0;
    g_pending = threads;
    for (i = 0; i < threads; i++) CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
}

/* the pictures again at PX (a new display scale: sg-dpi.h), from the cache;
 * the ones drawn now stay until each is replaced */
void icons_rescale(int px)
{
    if (!g_nent || px == g_px) return;
    g_px = px;
    if (g_pending) InterlockedExchange(&g_redo, 1);
    else start_pass();
}

void icons_fetch(const app_t *apps, int n, int px, HWND notify, UINT msg)
{
    int i, k, threads;
    if (g_nent) return;                     /* once per run */
    g_px = px; g_notify = notify; g_msg = msg;
    for (i = 0; i < n && g_nent < MAX_APPS; i++) {
        if (!apps[i].icon_url[0]) continue;
        for (k = 0; k < g_nent; k++) if (!lstrcmpiW(g_ent[k].url, apps[i].icon_url)) break;
        if (k == g_nent) lstrcpynW(g_ent[g_nent++].url, apps[i].icon_url, ARRAYSIZE(g_ent[0].url));
    }
    if (!g_nent) return;
    g_inet = InternetOpenW(L"StainedGlass-Store/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    (void)threads;
    start_pass();
}

/* how many pictures are ready (the dump) */
int icons_ready(void)
{
    int i, n = 0;
    for (i = 0; i < g_nent; i++) if (g_ent[i].bmp) n++;
    return n;
}

HBITMAP icon_for(const app_t *a)
{
    LONG i;
    if (!a->icon_url[0]) return NULL;
    for (i = 0; i < g_nent; i++) if (!lstrcmpiW(g_ent[i].url, a->icon_url)) return g_ent[i].bmp;
    return NULL;
}
