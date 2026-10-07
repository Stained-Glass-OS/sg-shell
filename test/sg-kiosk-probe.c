/* The kiosk gate's stand-in app (test/kiosk-check.sh).
 *
 *   sg-kiosk-probe.exe MARKFILE [SECONDS]   a window shown "normal" (as a WPF
 *                      program shows its own); after a second it appends
 *                      "zoomed=0|1 cwd=..." to MARKFILE, and exits with code
 *                      7 after SECONDS (2)
 *   sg-kiosk-probe.exe --handoff MARKFILE   a launcher: starts a copy of
 *                      itself (the app) and exits 0 at once
 *   sg-kiosk-probe.exe --mklnk LNK TARGET ARGS DIR   a shortcut, for Start
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>

static WCHAR g_mark[MAX_PATH];
static int g_seconds = 2, g_ticks;

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_TIMER) {
        if (++g_ticks == 1) {
            WCHAR cwd[MAX_PATH] = L"";
            FILE *f = _wfopen(g_mark, L"a");
            GetCurrentDirectoryW(MAX_PATH, cwd);
            if (f) { fwprintf(f, L"zoomed=%d cwd=%ls\n", IsZoomed(w) ? 1 : 0, cwd); fclose(f); }
        }
        if (g_ticks >= g_seconds) PostQuitMessage(7);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(7); return 0; }
    return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    WNDCLASSW wc = { 0 };
    WCHAR **argv;
    MSG msg;
    HWND w;
    int argc;
    (void)prev; (void)cmdline; (void)show;
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc >= 3 && !lstrcmpW(argv[1], L"--handoff")) {
        WCHAR self[MAX_PATH], line[MAX_PATH * 3];
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        GetModuleFileNameW(NULL, self, MAX_PATH);
        _snwprintf(line, ARRAYSIZE(line), L"\"%ls\" \"%ls\" 4", self, argv[2]);
        if (CreateProcessW(self, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        return 0;
    }
    if (argc >= 6 && !lstrcmpW(argv[1], L"--mklnk")) {
        IShellLinkW *link; IPersistFile *file; HRESULT hr = E_FAIL;
        WCHAR dir[MAX_PATH], *slash;
        lstrcpynW(dir, argv[2], MAX_PATH);
        if ((slash = wcsrchr(dir, '\\'))) { *slash = 0; SHCreateDirectoryExW(NULL, dir, NULL); }
        CoInitialize(NULL);
        if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) return 1;
        IShellLinkW_SetPath(link, argv[3]);
        if (argv[4][0]) IShellLinkW_SetArguments(link, argv[4]);
        if (argv[5][0]) IShellLinkW_SetWorkingDirectory(link, argv[5]);
        if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
            hr = IPersistFile_Save(file, argv[2], TRUE);
            IPersistFile_Release(file);
        }
        IShellLinkW_Release(link);
        return FAILED(hr);
    }
    if (argc < 2) return 2;
    lstrcpynW(g_mark, argv[1], MAX_PATH);
    if (argc >= 3) g_seconds = _wtoi(argv[2]);
    wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"SgKioskProbe";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    w = CreateWindowW(L"SgKioskProbe", L"Kiosk probe", WS_OVERLAPPEDWINDOW, 100, 100, 400, 300, NULL, NULL, inst, NULL);
    /* as WPF does: the show command it was started with is not used */
    ShowWindow(w, SW_HIDE);
    ShowWindow(w, SW_SHOWNORMAL);
    SetTimer(w, 1, 1000, NULL);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return (int)msg.wParam;
}
