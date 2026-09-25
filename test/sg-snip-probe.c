/* Gate helper for test/snip-check.sh: reads and sets the clipboard, and the screen.
 *   probe formats            -- the clipboard's formats, one per line
 *   probe dib FILE.bmp       -- CF_DIB written as a .bmp (prints W H, or NODIB)
 *   probe settext TEXT       -- the clipboard becomes TEXT
 *   probe text               -- CF_UNICODETEXT (or NOTEXT)
 *   probe screen FILE.bmp    -- GetDC(NULL) captured to a .bmp
 *   probe windows            -- visible top-level windows: class, title, rect
 *   probe close CLASS        -- WM_CLOSE to every visible window of that class
 *   probe foreground         -- the foreground window's class and title
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <stdio.h>

static int open_clip(void)
{
    int i;
    for (i = 0; i < 50; i++) { if (OpenClipboard(NULL)) return 1; Sleep(100); }
    return 0;
}

static void write_bmp(const wchar_t *file, const BITMAPINFOHEADER *bi, SIZE_T size)
{
    BITMAPFILEHEADER fh = { 0 };
    DWORD off = sizeof(fh) + bi->biSize + (bi->biCompression == BI_BITFIELDS ? 12 : 0) + bi->biClrUsed * 4;
    FILE *f = _wfopen(file, L"wb");
    if (!f) return;
    fh.bfType = 0x4d42; fh.bfSize = sizeof(fh) + (DWORD)size; fh.bfOffBits = off;
    fwrite(&fh, sizeof(fh), 1, f); fwrite(bi, size, 1, f); fclose(f);
}

static BOOL CALLBACK list(HWND h, LPARAM l)
{
    WCHAR cls[128], title[256]; RECT r;
    (void)l;
    if (!IsWindowVisible(h)) return TRUE;
    GetClassNameW(h, cls, 128); GetWindowTextW(h, title, 256); GetWindowRect(h, &r);
    printf("%ls\t%ls\t%ld %ld %ld %ld\n", cls, title, r.left, r.top, r.right, r.bottom);
    return TRUE;
}

static const wchar_t *close_cls;
static BOOL CALLBACK closer(HWND h, LPARAM l)
{
    WCHAR cls[128]; (void)l;
    GetClassNameW(h, cls, 128);
    if (!lstrcmpiW(cls, close_cls)) PostMessageW(h, WM_CLOSE, 0, 0);
    return TRUE;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) return 2;
    if (!lstrcmpW(argv[1], L"formats")) {
        UINT f = 0; WCHAR name[128];
        if (!open_clip()) return 1;
        while ((f = EnumClipboardFormats(f))) {
            if (!GetClipboardFormatNameW(f, name, 128)) swprintf(name, 128, L"%u", f);
            printf("%ls\n", name);
        }
        CloseClipboard(); return 0;
    }
    if (!lstrcmpW(argv[1], L"dib") && argc > 2) {
        HANDLE h; BITMAPINFOHEADER *bi;
        if (!open_clip()) return 1;
        if (!(h = GetClipboardData(CF_DIB)) || !(bi = GlobalLock(h))) { printf("NODIB\n"); CloseClipboard(); return 0; }
        write_bmp(argv[2], bi, GlobalSize(h));
        printf("%ld %ld\n", bi->biWidth, bi->biHeight < 0 ? -bi->biHeight : bi->biHeight);
        GlobalUnlock(h); CloseClipboard(); return 0;
    }
    if (!lstrcmpW(argv[1], L"settext") && argc > 2) {
        SIZE_T n = (wcslen(argv[2]) + 1) * sizeof(WCHAR);
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
        memcpy(GlobalLock(g), argv[2], n); GlobalUnlock(g);
        if (!open_clip()) return 1;
        EmptyClipboard(); SetClipboardData(CF_UNICODETEXT, g); CloseClipboard(); return 0;
    }
    if (!lstrcmpW(argv[1], L"text")) {
        HANDLE h; WCHAR *t;
        if (!open_clip()) return 1;
        if ((h = GetClipboardData(CF_UNICODETEXT)) && (t = GlobalLock(h))) { printf("%ls\n", t); GlobalUnlock(h); }
        else printf("NOTEXT\n");
        CloseClipboard(); return 0;
    }
    if (!lstrcmpW(argv[1], L"screen") && argc > 2) {
        HDC s = GetDC(NULL), m = CreateCompatibleDC(s);
        int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
        BITMAPINFOHEADER *bi = calloc(1, sizeof(*bi) + (SIZE_T)w * h * 4);
        HBITMAP b = CreateCompatibleBitmap(s, w, h);
        SelectObject(m, b); BitBlt(m, 0, 0, w, h, s, 0, 0, SRCCOPY);
        bi->biSize = sizeof(*bi); bi->biWidth = w; bi->biHeight = -h; bi->biPlanes = 1; bi->biBitCount = 32;
        GetDIBits(m, b, 0, h, bi + 1, (BITMAPINFO *)bi, DIB_RGB_COLORS);
        write_bmp(argv[2], bi, sizeof(*bi) + (SIZE_T)w * h * 4);
        printf("%d %d\n", w, h); return 0;
    }
    if (!lstrcmpW(argv[1], L"foreground")) {
        WCHAR cls[128] = L"", title[256] = L""; HWND h = GetForegroundWindow();
        if (h) { GetClassNameW(h, cls, 128); GetWindowTextW(h, title, 256); }
        printf("%ls\t%ls\n", cls, title); return 0;
    }
    if (!lstrcmpW(argv[1], L"windows")) { EnumWindows(list, 0); return 0; }
    if (!lstrcmpW(argv[1], L"close") && argc > 2) { close_cls = argv[2]; EnumWindows(closer, 0); return 0; }
    return 2;
}
