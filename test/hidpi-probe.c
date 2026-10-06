/* hidpi-check's eyes, aware of the display scale (so it sees the screen's
 * pixels, not Wine's scaled ones): the screen, the taskbar's rectangle, the
 * system DPI and the title bar's height as a program started now gets them. */
#include <windows.h>
#include <stdio.h>
int main(void)
{
    HWND bar, start;
    RECT r = { 0 };
    int pm;
    HDC dc;
    SetProcessDPIAware();
    dc = GetDC(NULL);
    bar = FindWindowW(L"Shell_TrayWnd", NULL);
    if (bar) GetWindowRect(bar, &r);
    /* Start's panel on the screen, in its pixels */
    /* Start's panel: per-monitor v2 (it sizes itself by the display scale) */
    start = FindWindowW(L"SgStartPanel", NULL);
    pm = start && AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(start), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    printf("screen=%dx%d dpi=%d caption=%d bar=%ld,%ld,%ld,%ld barh=%ld cursor=%d startpm=%d\n", GetSystemMetrics(SM_CXSCREEN),
           GetSystemMetrics(SM_CYSCREEN), GetDeviceCaps(dc, LOGPIXELSY), GetSystemMetrics(SM_CYCAPTION),
           r.left, r.top, r.right, r.bottom, r.bottom - r.top, GetSystemMetrics(SM_CYCURSOR), pm);
    ReleaseDC(NULL, dc);
    return 0;
}
