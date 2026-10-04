/* dictate-linux-frame -- for test/dictate-linux-check.sh: a window of the
 * class explorer frames Linux programs in (SgLinuxWindow), in front for a
 * minute, as Firefox for Linux's frame is while it has the focus. */
#include <windows.h>
static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    WNDCLASSW wc = { 0 };
    HWND hwnd;
    MSG msg;
    DWORD end = GetTickCount() + 90000;
    wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"SgLinuxWindow";
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"SgLinuxWindow", L"Mozilla Firefox (Linux)", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           100, 100, 600, 400, 0, 0, inst, 0);
    SetForegroundWindow(hwnd);
    while (GetTickCount() < end)
    {
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        Sleep(50);
    }
    return 0;
}
