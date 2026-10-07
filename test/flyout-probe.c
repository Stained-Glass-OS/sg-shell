/* flyout-probe (test/flyout-toggle-check.sh): "rect TRAYCLASS" prints the tray
 * icon's rect (Shell_NotifyIconGetRect, id 1); "vis CLASS" 1 if the window shows */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
int wmain(int argc, WCHAR **argv)
{
    HWND h;
    if (argc < 3) return 2;
    h = FindWindowW(argv[2], NULL);
    if (!lstrcmpW(argv[1], L"vis")) { printf("%d\n", h && IsWindowVisible(h)); return 0; }
    if (h)
    {
        NOTIFYICONIDENTIFIER nii = { sizeof(nii), h, 1 };
        RECT r;
        if (Shell_NotifyIconGetRect(&nii, &r) == S_OK) { printf("%ld %ld %ld %ld\n", r.left, r.top, r.right, r.bottom); return 0; }
    }
    printf("none\n");
    return 1;
}
