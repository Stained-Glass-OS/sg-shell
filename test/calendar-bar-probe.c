/* calendar-bar-probe (test/calendar-check.sh): a stand-in taskbar -- Shell_SecondaryTrayWnd, or the
 * class given -- at L T R B. It asks the calendar to open at it (SgCalendarToggle, lparam: the bar,
 * as wine-sg 9950's clocks do), then stays 60 s. --noclick: only the bar. */
#include <windows.h>
#include <stdlib.h>
static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_PAINT) { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT r; GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(RGB(0x20,0x20,0x24)); FillRect(dc, &r, b); DeleteObject(b); EndPaint(h, &ps); return 0; }
    return DefWindowProcW(h, m, w, l);
}
int wmain(int argc, WCHAR **argv)
{
    WNDCLASSW wc = { 0 };
    HWND bar, host;
    MSG msg;
    int l, t, r, b;
    const WCHAR *cls = argc > 5 ? argv[5] : L"Shell_SecondaryTrayWnd";
    if (argc < 5) return 2;
    l = _wtoi(argv[1]); t = _wtoi(argv[2]); r = _wtoi(argv[3]); b = _wtoi(argv[4]);
    wc.lpfnWndProc = proc; wc.lpszClassName = cls; wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    bar = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, cls, L"", WS_POPUP | WS_VISIBLE, l, t, r - l, b - t, NULL, NULL, NULL, NULL);
    UpdateWindow(bar);
    Sleep(300);
    /* (the calendar's copy may still be starting) */
    for (l = 0; l < 100 && !(host = FindWindowW(L"SgCalendarHost", NULL)); l++) Sleep(100);
    if (host && !(argc > 6 && !wcscmp(argv[6], L"--noclick")))
        PostMessageW(host, RegisterWindowMessageW(L"SgCalendarToggle"), 0, (LPARAM)bar);
    SetTimer(NULL, 0, 60000, NULL);
    while (GetMessageW(&msg, NULL, 0, 0)) { if (msg.message == WM_TIMER) break; DispatchMessageW(&msg); }
    return 0;
}
