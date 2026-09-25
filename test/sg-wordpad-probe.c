/* sg-wordpad-probe -- helpers for test/wordpad-check.sh.
 *
 *   emf2bmp IN.emf OUT.bmp      play a printed page into a 24-bit BMP (96 dpi)
 *   activate TITLE              bring a window to the front
 *   fg                          print the foreground window's title
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <stdio.h>

static int emf2bmp(const WCHAR *in, const WCHAR *out)
{
    HENHMETAFILE emf = GetEnhMetaFileW(in);
    ENHMETAHEADER eh;
    int w, h;
    BITMAPINFO bi;
    void *bits;
    HDC dc;
    HBITMAP bmp;
    RECT r;
    BITMAPFILEHEADER fh;
    FILE *f;
    if (!emf) { puts("NOEMF"); return 1; }
    GetEnhMetaFileHeader(emf, sizeof(eh), &eh);
    /* the frame is in .01 mm */
    w = MulDiv(eh.rclFrame.right - eh.rclFrame.left, 96, 2540);
    h = MulDiv(eh.rclFrame.bottom - eh.rclFrame.top, 96, 2540);
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 24;
    dc = CreateCompatibleDC(NULL);
    bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    SelectObject(dc, bmp);
    SetRect(&r, 0, 0, w, h);
    FillRect(dc, &r, GetStockObject(WHITE_BRUSH));
    PlayEnhMetaFile(dc, emf, &r);
    GdiFlush();
    memset(&fh, 0, sizeof(fh));
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + ((w * 3 + 3) & ~3) * h;
    if (!(f = _wfopen(out, L"wb"))) return 1;
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
    fwrite(bits, ((w * 3 + 3) & ~3) * h, 1, f);
    fclose(f);
    printf("OK %d %d\n", w, h);
    return 0;
}

int wmain(int argc, WCHAR **argv)
{
    if (argc >= 4 && !wcscmp(argv[1], L"emf2bmp")) return emf2bmp(argv[2], argv[3]);
    if (argc >= 3 && !wcscmp(argv[1], L"activate"))
    {
        HWND w = FindWindowW(NULL, argv[2]);
        if (!w) { puts("NOWINDOW"); return 1; }
        SetForegroundWindow(w);
        puts("OK");
        return 0;
    }
    if (argc >= 2 && !wcscmp(argv[1], L"fg"))
    {
        WCHAR t[256] = L"";
        GetWindowTextW(GetForegroundWindow(), t, 256);
        printf("%ls\n", t);
        return 0;
    }
    return 2;
}
