/* sg-kiosk64.exe -- starts the kiosk app (Settings > Accounts > Kiosk)
 * maximized and waits for it: sg-session's sg-kiosk runs this in the kiosk
 * account's session and starts it again whenever it returns.
 *
 *   sg-kiosk64.exe PROGRAM [FOLDER] [ARGUMENTS]
 *
 * PROGRAM is a program or a shortcut (ShellExecuteEx, as Start opens it),
 * opened maximized; every main window it shows is maximized as it appears
 * (a program that ignores the show command it was started with: WPF's
 * windows show themselves "normal"). It returns when the program has ended
 * -- and when a program hands over to another and exits (a launcher such as
 * Squirrel's Update.exe --processStart, or a second copy finding the first),
 * it waits for the windows of programs from the same folder (or below)
 * instead. The exit code is the program's; 1 if it could not be started.
 *
 * SG_KIOSK_POLL (milliseconds, 500) is how often it looks; the gate's.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define MAX_DONE 256
static HWND g_done[MAX_DONE];   /* windows maximized once: the person may restore them */
static int g_ndone;
static WCHAR g_folder[MAX_PATH];   /* the program's own folder: a hand-over stays in it */

/* what a shortcut starts; a program, itself */
static void target_of(const WCHAR *path, WCHAR *out)
{
    const WCHAR *ext = wcsrchr(path, '.');
    IShellLinkW *link;
    IPersistFile *file;
    lstrcpynW(out, path, MAX_PATH);
    if (!ext || lstrcmpiW(ext, L".lnk")) return;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) return;
    if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) {
        WCHAR t[MAX_PATH];
        if (SUCCEEDED(IPersistFile_Load(file, path, STGM_READ)) && SUCCEEDED(IShellLinkW_GetPath(link, t, MAX_PATH, NULL, 0)) && t[0])
            lstrcpynW(out, t, MAX_PATH);
        IPersistFile_Release(file);
    }
    IShellLinkW_Release(link);
}

static BOOL is_main_window(HWND w)
{
    LONG style = GetWindowLongW(w, GWL_STYLE), ex = GetWindowLongW(w, GWL_EXSTYLE);
    return IsWindowVisible(w) && !GetWindow(w, GW_OWNER) && (style & WS_CAPTION) == WS_CAPTION && !(ex & WS_EX_TOOLWINDOW);
}

/* a program from the app's folder (or below) */
static BOOL from_folder(DWORD pid)
{
    WCHAR image[MAX_PATH];
    DWORD n = MAX_PATH;
    HANDLE p;
    BOOL ok = FALSE;
    int len = lstrlenW(g_folder);
    if (!len || !(p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))) return FALSE;
    if (QueryFullProcessImageNameW(p, 0, image, &n))
        ok = !_wcsnicmp(image, g_folder, len) && (image[len] == '\\' || !image[len]);
    CloseHandle(p);
    return ok;
}

struct look { DWORD pid; BOOL any_from_folder; DWORD found_pid; };

static BOOL CALLBACK each_window(HWND w, LPARAM lp)
{
    struct look *l = (struct look *)lp;
    DWORD pid = 0;
    int i;
    if (!is_main_window(w)) return TRUE;
    GetWindowThreadProcessId(w, &pid);
    if (pid != l->pid && !(l->pid == 0 && from_folder(pid))) return TRUE;
    if (l->pid == 0) { l->any_from_folder = TRUE; l->found_pid = pid; }
    for (i = 0; i < g_ndone; i++) if (g_done[i] == w) return TRUE;
#ifndef SG_MUTANT_KIOSK_NOT_MAXIMIZED
    if (!IsZoomed(w)) ShowWindow(w, SW_MAXIMIZE);
    SetForegroundWindow(w);
#endif
    if (g_ndone < MAX_DONE) g_done[g_ndone++] = w;
    return TRUE;
}

/* waits for the process, maximizing its windows as they come */
static DWORD watch(HANDLE process, DWORD pid, DWORD poll)
{
    struct look l = { pid, FALSE, 0 };
    DWORD code = 0;
    while (WaitForSingleObject(process, poll) == WAIT_TIMEOUT) EnumWindows(each_window, (LPARAM)&l);
    GetExitCodeProcess(process, &code);
    return code;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    WCHAR **argv, target[MAX_PATH], poll_s[16], *slash;
    DWORD code = 0, poll = 500;
    int argc, quiet = 0;
    (void)inst; (void)prev; (void)cmdline; (void)show;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc < 2) {
        MessageBoxW(NULL, L"Usage: sg-kiosk64.exe PROGRAM [FOLDER] [ARGUMENTS]", L"Kiosk app", MB_OK | MB_ICONINFORMATION);
        return 2;
    }
    if (GetEnvironmentVariableW(L"SG_KIOSK_POLL", poll_s, ARRAYSIZE(poll_s))) poll = (DWORD)_wtoi(poll_s);
    if (poll < 50) poll = 50;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    target_of(argv[1], target);
    lstrcpynW(g_folder, target, MAX_PATH);
    if ((slash = wcsrchr(g_folder, '\\'))) *slash = 0; else g_folder[0] = 0;

    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
    sei.lpFile = argv[1];
    sei.lpDirectory = argc > 2 && argv[2][0] ? argv[2] : NULL;
    sei.lpParameters = argc > 3 && argv[3][0] ? argv[3] : NULL;
    sei.nShow = SW_SHOWMAXIMIZED;
    if (!ShellExecuteExW(&sei)) {
        fwprintf(stderr, L"sg-kiosk: %ls could not be started (%lu)\n", argv[1], GetLastError());
        return 1;
    }
    if (sei.hProcess) {
        code = watch(sei.hProcess, GetProcessId(sei.hProcess), poll);
        CloseHandle(sei.hProcess);
    }
    /* handed over: the app is whatever from its folder still shows a window */
#ifndef SG_MUTANT_KIOSK_NO_HANDOFF
    for (;;) {
        struct look l = { 0, FALSE, 0 };
        HANDLE p;
        EnumWindows(each_window, (LPARAM)&l);
        if (!l.any_from_folder) {
            /* a moment for a hand-over's window to appear, once */
            if (quiet++ < (int)(3000 / poll)) { Sleep(poll); continue; }
            break;
        }
        quiet = 0;
        if (!(p = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, l.found_pid))) { Sleep(poll); continue; }
        code = watch(p, l.found_pid, poll);
        CloseHandle(p);
    }
#endif
    LocalFree(argv);
    return (int)code;
}
