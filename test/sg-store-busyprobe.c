/* sg-store-busyprobe: for store-busy-check.sh. Finds SG Store's window and,
 * for SECONDS, asks it every 100 ms for a message round trip (WM_NULL) and a
 * repaint, and prints the longest wait:
 *   sg-store-busyprobe.exe SECONDS   ->  "BUSY maxms N slow K of M"
 * (slow: a round trip over 500 ms). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, WCHAR **argv)
{
    HWND w = NULL;
    DWORD secs = argc > 1 ? (DWORD)_wtoi(argv[1]) : 5, start, maxms = 0, n = 0, slow = 0;
    int i;
    for (i = 0; i < 100 && !(w = FindWindowW(NULL, L"SG Store")); i++) Sleep(100);
    if (!w) { printf("BUSY nowindow\n"); return 1; }
    for (start = GetTickCount(); GetTickCount() - start < secs * 1000; Sleep(100)) {
        DWORD t0 = GetTickCount(), dt;
        DWORD_PTR r;
        RedrawWindow(w, NULL, NULL, RDW_INVALIDATE);
        if (!SendMessageTimeoutW(w, WM_NULL, 0, 0, SMTO_BLOCK, 5000, &r)) dt = 5000;
        else dt = GetTickCount() - t0;
        if (dt > maxms) maxms = dt;
        if (dt > 500) slow++;
        n++;
    }
    printf("BUSY maxms %lu slow %lu of %lu\n", maxms, slow, n);
    return 0;
}
