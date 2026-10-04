/* control-icon-check.sh's probe: the icon of the program given, at 16, 32
 * and 48 px -- found, and smooth: ICONSIZE PX FOUND EDGE (EDGE: how many of
 * its pixels are partly transparent, the anti-aliased edge) */
#include <windows.h>
#include <stdio.h>
int wmain(int argc, WCHAR **argv)
{
    static const int sizes[] = { 16, 32, 48 };
    int s;
    if (argc < 2) return 2;
    for (s = 0; s < 3; s++) {
        int px = sizes[s], edge = 0, i;
        HICON icon = NULL;
        UINT id;
        ICONINFO ii;
        BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), px, -px, 1, 32, BI_RGB } };
        DWORD *bits = calloc(px * px, 4);
        HDC dc = GetDC(NULL);
        if (PrivateExtractIconsW(argv[1], 0, px, px, &icon, &id, 1, 0) != 1 || !icon) { printf("ICONSIZE %d 0 0\n", px); continue; }
        if (GetIconInfo(icon, &ii) && ii.hbmColor) {
            GetDIBits(dc, ii.hbmColor, 0, px, bits, &bi, DIB_RGB_COLORS);
            for (i = 0; i < px * px; i++) { BYTE a = bits[i] >> 24; if (a > 0 && a < 255) edge++; }
        }
        printf("ICONSIZE %d 1 %d\n", px, edge);
        ReleaseDC(NULL, dc);
        free(bits);
    }
    return 0;
}
