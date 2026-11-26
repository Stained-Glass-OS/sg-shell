/* emoji-target -- for test/emoji-check.sh: a window with an edit box that has
 * the focus and the caret, in front, for up to two minutes. Every half second
 * it writes the box's text (UTF-8) and where its caret is on the screen to
 * C:\target.txt. "emoji-target linux" makes a window of the class explorer
 * frames Linux programs in (SgLinuxWindow) instead.
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <windows.h>
#include <stdio.h>

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcW(h, m, w, l); }

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    WNDCLASSW wc = { 0 };
    HWND hwnd, edit = NULL;
    MSG msg;
    BOOL linux_frame = cmd && !lstrcmpW(cmd, L"linux");
    DWORD end = GetTickCount() + 120000, next = 0;
    (void)prev; (void)show;
    wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = linux_frame ? L"SgLinuxWindow" : L"EmojiTarget";
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, L"Emoji target", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           100, 100, 600, 300, 0, 0, inst, 0);
    if (!linux_frame)
    {
        edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 20, 20, 400, 28, hwnd, 0, inst, 0);
        SetFocus(edit);
    }
    SetForegroundWindow(hwnd);
    if (edit) SetFocus(edit);
    while (GetTickCount() < end)
    {
        while (PeekMessageW(&msg, 0, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (GetTickCount() >= next)
        {
            WCHAR text[256] = L"";
            char utf8[1024];
            POINT caret = { 0, 0 };
            FILE *f;
            if (edit)
            {
                GetWindowTextW(edit, text, 256);
                GetCaretPos(&caret);
                ClientToScreen(edit, &caret);
            }
            WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL);
            if ((f = _wfopen(L"C:\\target.tmp", L"wb")))
            {
                fprintf(f, "caret %ld,%ld\ntext %s\n", caret.x, caret.y, utf8);
                fclose(f);
                MoveFileExW(L"C:\\target.tmp", L"C:\\target.txt", MOVEFILE_REPLACE_EXISTING);
            }
            next = GetTickCount() + 500;
        }
        Sleep(30);
    }
    return 0;
}
