/* hidpi-check's eyes, aware of the display scale (so it sees the screen's
 * pixels, not Wine's scaled ones): the screen, the taskbar's rectangle, the
 * system DPI and the title bar's height as a program started now gets them. */
#include <windows.h>
#include <stdio.h>
int main(void)
{
    HWND bar;
    RECT r = { 0 };
    HDC dc;
    SetProcessDPIAware();
    dc = GetDC(NULL);
    bar = FindWindowW(L"Shell_TrayWnd", NULL);
    if (bar) GetWindowRect(bar, &r);
    printf("screen=%dx%d dpi=%d caption=%d bar=%ld,%ld,%ld,%ld barh=%ld cursor=%d\n", GetSystemMetrics(SM_CXSCREEN),
           GetSystemMetrics(SM_CYSCREEN), GetDeviceCaps(dc, LOGPIXELSY), GetSystemMetrics(SM_CYCAPTION),
           r.left, r.top, r.right, r.bottom, r.bottom - r.top, GetSystemMetrics(SM_CYCURSOR));
    ReleaseDC(NULL, dc);
    return 0;
}
