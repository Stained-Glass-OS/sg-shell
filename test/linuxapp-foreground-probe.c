/* linuxapp-foreground-check.sh's helpers.
 *   probe start LAUNCHER DESKTOP  a window, "Gate Start", standing in for Start: clicked, it runs
 *                                 LAUNCHER --run DESKTOP, gives the foreground back to the window
 *                                 that had it (Notepad) and hides, as Start closes
 *   probe fg                      the foreground window: "fg CLASS|TITLE"
 */
#include <windows.h>
#include <stdio.h>

static WCHAR *g_cmd;
static HWND g_prev;

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_LBUTTONUP && g_cmd)
    {
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        if (CreateProcessW(NULL, g_cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        {
            CloseHandle(pi.hThread);
            WaitForSingleObject(pi.hProcess, 10000);
            CloseHandle(pi.hProcess);
        }
        if (g_prev) SetForegroundWindow(g_prev);
        ShowWindow(hwnd, SW_HIDE);
        g_cmd = NULL;
        return 0;
    }
    if (msg == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int wmain(int argc, WCHAR **argv)
{
    if (argc >= 4 && !lstrcmpW(argv[1], L"start"))
    {
        WNDCLASSW wc = { 0 };
        MSG msg;
        HWND hwnd;
        static WCHAR cmd[2048];
        _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" --run \"%ls\"", argv[2], argv[3]);
        g_cmd = cmd;
        g_prev = GetForegroundWindow();
        wc.lpfnWndProc = proc;
        wc.lpszClassName = L"GateStart";
        wc.hbrBackground = GetStockObject(GRAY_BRUSH);
        RegisterClassW(&wc);
        hwnd = CreateWindowExW(WS_EX_TOPMOST, L"GateStart", L"Gate Start", WS_POPUP | WS_VISIBLE, 0, 360, 300, 300, 0, 0, 0, 0);
        SetForegroundWindow(hwnd);
        while (GetMessageW(&msg, 0, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        return 0;
    }
    if (argc >= 2 && !lstrcmpW(argv[1], L"fg"))
    {
        WCHAR cls[64] = L"", title[128] = L"";
        HWND fg = GetForegroundWindow();
        GetClassNameW(fg, cls, 64);
        GetWindowTextW(fg, title, 128);
        printf("fg %ls|%ls\n", cls, title);
        return 0;
    }
    return 1;
}
