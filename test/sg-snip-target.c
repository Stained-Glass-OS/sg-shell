/* Gate helper for test/snip-check.sh: a window at a known place whose client
 * area is four known colours (quadrants), so a snip of it can be checked
 * pixel by pixel.  sg-snip-target X Y W H
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <stdlib.h>

static const COLORREF Q[4] = { RGB(220, 20, 60), RGB(30, 160, 40), RGB(20, 60, 220), RGB(250, 200, 0) };

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; RECT rc, q; int i;
        HDC dc = BeginPaint(h, &ps);
        GetClientRect(h, &rc);
        for (i = 0; i < 4; i++) {
            HBRUSH b = CreateSolidBrush(Q[i]);
            q.left = (i & 1) ? rc.right / 2 : 0; q.right = (i & 1) ? rc.right : rc.right / 2;
            q.top = (i & 2) ? rc.bottom / 2 : 0; q.bottom = (i & 2) ? rc.bottom : rc.bottom / 2;
            FillRect(dc, &q, b); DeleteObject(b);
        }
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    WNDCLASSW wc = { 0 };
    MSG msg; int x = 100, y = 100, w = 400, hgt = 300, argc; HWND h;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    (void)prev; (void)cmd; (void)show;
    if (argc >= 5) { x = _wtoi(argv[1]); y = _wtoi(argv[2]); w = _wtoi(argv[3]); hgt = _wtoi(argv[4]); }
    wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = L"SgSnipTarget";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    /* a popup: its client area is exactly the rectangle given */
    h = CreateWindowExW(0, L"SgSnipTarget", L"Snip target", WS_POPUP | WS_VISIBLE, x, y, w, hgt, NULL, NULL, inst, NULL);
    (void)h;
    while (GetMessageW(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    return 0;
}
