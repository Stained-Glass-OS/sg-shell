/* A program with a text field and a button, for test/touchkbd-check.sh: it
 * does what its command file (C:\touchkbd-cmd.txt) says, a line at a time --
 *   touch-edit     a touch focuses the text field: the desktop's touch time
 *                  and touched window (what wine-sg 1150 stamps for every
 *                  touch going down), then the focus moves to the edit control
 *   touch-button   the same for the button
 *   touch-late     a touch focuses the window itself, which shows a caret
 *                  2 seconds later -- a WPF text box (the focus is the
 *                  program's own window; WPF's empty Win32 caret comes with
 *                  its next drawing), in a program that opens its field late
 *                  (Sonos's search)
 *   linux-front    a window of the class a Linux program's frame has
 *                  (SgLinuxWindow) comes to the front
 *   front          this window to the front again, the text field focused
 *   set NAME N     the touch keyboard's setting NAME (TabletTip\1.7) to N,
 *                  announced as the taskbar's menu does ("TraySettings")
 * -- and writes C:\touchkbd-state.txt: the edit's text, whether this window
 * is in front and has the focus. "touchkbd-probe.exe workarea" writes the
 * work area (as a new process reads it) to C:\touchkbd-work.txt.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static HWND g_wnd, g_edit, g_button;
static long g_done;

static void touch_focus(HWND hwnd)
{
    KillTimer(g_wnd, 2);
    DestroyCaret();
    SetPropW(GetDesktopWindow(), L"__wine_sg_touch_hwnd", g_wnd);
    SetPropW(GetDesktopWindow(), L"__wine_sg_touch_time", (HANDLE)(ULONG_PTR)(GetTickCount() | 1));
    /* a touch on the window brings it to the front, as a click does */
    SetForegroundWindow(g_wnd);
    SetFocus(hwnd);
}

static HWND g_linux;

static LRESULT CALLBACK linux_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* a stand-in for a Linux program's window in its frame (explorer's
 * SgLinuxWindow), brought to the front */
static void linux_front(void)
{
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = linux_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"SgLinuxWindow";
    RegisterClassW(&wc);
    if (!g_linux)
        g_linux = CreateWindowW(L"SgLinuxWindow", L"Linux program", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 600, 40, 300, 200,
                                NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(g_linux);
}

/* the touch keyboard's settings, as Settings and the taskbar's menu write them */
static void set_tip(const WCHAR *name, DWORD value)
{
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\TabletTip\\1.7", name, REG_DWORD, &value, sizeof(value));
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 2000, NULL);
}

static void tick(void)
{
    FILE *f;
    WCHAR text[256];
    char line[64];
    RECT work;
    long n = 0;
    HWND focus = GetFocus();

    if ((f = _wfopen(L"C:\\touchkbd-cmd.txt", L"r")))
    {
        while (fgets(line, sizeof(line), f))
        {
            if (++n <= g_done) continue;
            g_done = n;
            if (!strncmp(line, "touch-edit", 10)) touch_focus(g_edit);
            else if (!strncmp(line, "touch-button", 12)) touch_focus(g_button);
            else if (!strncmp(line, "touch-late", 10)) { touch_focus(g_wnd); SetTimer(g_wnd, 2, 2000, NULL); }
            else if (!strncmp(line, "linux-front", 11)) linux_front();
            else if (!strncmp(line, "front", 5)) { SetForegroundWindow(g_wnd); SetFocus(g_edit); }
            else if (!strncmp(line, "set ", 4))
            {
                WCHAR name[64];
                unsigned long v = 0;
                char a[64];
                if (sscanf(line + 4, "%63s %lu", a, &v) == 2)
                {
                    MultiByteToWideChar(CP_ACP, 0, a, -1, name, 64);
                    set_tip(name, v);
                }
            }
        }
        fclose(f);
    }
    GetWindowTextW(g_edit, text, 256);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if ((f = _wfopen(L"C:\\touchkbd-state.tmp", L"w")))
    {
        WCHAR fgclass[64] = L"none";
        if (GetForegroundWindow()) GetClassNameW(GetForegroundWindow(), fgclass, 64);
        fwprintf(f, L"FG %ls\n", fgclass);
        fwprintf(f, L"TEXT %ls\nFRONT %d\nFOCUS %ls\nWORK %ld %ld %ld %ld\n", text, GetForegroundWindow() == g_wnd,
                 focus == g_edit ? L"edit" : focus == g_button ? L"button" : focus == g_wnd ? L"window" : L"other",
                 work.left, work.top, work.right, work.bottom);
        fclose(f);
        MoveFileExW(L"C:\\touchkbd-state.tmp", L"C:\\touchkbd-state.txt", MOVEFILE_REPLACE_EXISTING);
    }
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_TIMER && wp == 2)
    {
        KillTimer(hwnd, 2);
        CreateCaret(hwnd, NULL, 1, 16);
        SetCaretPos(10, 50);
        ShowCaret(hwnd);
        return 0;
    }
    if (msg == WM_TIMER) { tick(); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    WNDCLASSW wc = { 0 };
    MSG msg;
    (void)prev; (void)show;

    /* "workarea": the work area now, as a new process reads it */
    if (cmd && !wcsncmp(cmd, L"workarea", 8))
    {
        RECT work;
        FILE *f = _wfopen(L"C:\\touchkbd-work.txt", L"w");
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        if (f) { fwprintf(f, L"%ld %ld %ld %ld\n", work.left, work.top, work.right, work.bottom); fclose(f); }
        return 0;
    }

    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TouchKbdProbe";
    RegisterClassW(&wc);
    g_wnd = CreateWindowW(wc.lpszClassName, L"Touch probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 520, 200,
                          NULL, NULL, inst, NULL);
    g_edit = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 10, 10, 300, 28, g_wnd,
                           (HMENU)1, inst, NULL);
    g_button = CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 320, 10, 80, 28, g_wnd,
                             (HMENU)2, inst, NULL);
    SetForegroundWindow(g_wnd);
    SetFocus(g_button);
    SetTimer(g_wnd, 1, 100, NULL);
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
