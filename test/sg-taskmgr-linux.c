/* taskmgr-check.sh's Linux window: a stand-in as the taskbar keeps one for
 * each Linux program's window (hidden, class SgLinuxWindow, its title),
 * which writes C:\linuxclosed.txt when asked to close and C:\linuxrestored.txt
 * when restored -- the taskbar's would close the Linux window or bring it
 * forward (wine-sg 0759).
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <windows.h>
#include <stdio.h>

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SYSCOMMAND && (wp & 0xfff0) == SC_RESTORE) {   /* brought forward: Switch to */
        FILE *f = fopen("C:\\linuxrestored.txt", "w");
        if (f) { fputs("restored\n", f); fclose(f); }
        return 0;
    }
    if (msg == WM_CLOSE) {
        FILE *f = fopen("C:\\linuxclosed.txt", "w");
        if (f) { fputs("closed\n", f); fclose(f); }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int wmain(void)
{
    WNDCLASSW wc = { 0 };
    MSG m;
    wc.lpfnWndProc = proc;
    wc.lpszClassName = L"SgLinuxWindow";
    RegisterClassW(&wc);
    CreateWindowExW(WS_EX_TOOLWINDOW, L"SgLinuxWindow", L"Linux Test App", WS_POPUP, 0, 0, 0, 0, 0, 0, 0, 0);
    while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    return 0;
}
