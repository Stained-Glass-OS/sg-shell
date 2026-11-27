/* handoff-probe: for test/handoff-check.sh (the foreground lock, wine-sg 9930).
 *   handoff-probe launcher NAME COMMAND   a window titled NAME at (40, 40 + 120 * its place); a click in it
 *                                         runs COMMAND (as the person's click on a taskbar icon does)
 *   handoff-probe fg                      the foreground window's class
 *   handoff-probe lock                    "1" when this Wine has the foreground lock: this process, started
 *                                         from outside while the person works elsewhere, may not grant it
 */
#include <windows.h>
#include <stdio.h>

static WCHAR *command;

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_LBUTTONUP && command)
    {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        WCHAR buf[1024];
        lstrcpynW(buf, command, ARRAYSIZE(buf));
        if (CreateProcessW(NULL, buf, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        return 0;
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int wmain(int argc, WCHAR **argv)
{
    if (argc >= 5 && !lstrcmpW(argv[1], L"launcher"))
    {
        WNDCLASSW wc = { 0 };
        HWND hwnd;
        MSG msg;
        command = argv[3];
        wc.lpfnWndProc = proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"HandoffLauncher";
        RegisterClassW(&wc);
        hwnd = CreateWindowW(L"HandoffLauncher", argv[2], WS_OVERLAPPEDWINDOW, 40, 40 + 120 * _wtoi(argv[4]), 300, 100,
                             NULL, NULL, wc.hInstance, NULL);
        ShowWindow(hwnd, SW_SHOWNORMAL);
        while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        return 0;
    }
    if (argc >= 2 && !lstrcmpW(argv[1], L"fg"))
    {
        WCHAR cls[128] = L"none";
        HWND fg = GetForegroundWindow();
        if (fg) GetClassNameW(fg, cls, ARRAYSIZE(cls));
        wprintf(L"%ls\n", cls);
        return 0;
    }
    if (argc >= 2 && !lstrcmpW(argv[1], L"lock"))
    {
        wprintf(L"%d\n", !AllowSetForegroundWindow(ASFW_ANY));
        return 0;
    }
    return 1;
}
