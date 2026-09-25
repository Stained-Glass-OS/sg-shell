/* Probe for the Magnifier and On-Screen Keyboard gates (test/magnify-check.sh,
 * test/osk-check.sh).
 *
 *   probe pattern X Y W H      a window at X,Y (W x H) painted with 8-pixel
 *                              blocks of different colours; runs until killed.
 *                              Its title is "Pattern N", N the clicks it got.
 *   probe title CLASS          the title of CLASS's window
 *   probe text CLASS           the text of CLASS's edit control (WM_GETTEXT), as UTF-8
 *   probe foreground           the class of the foreground window
 *   probe activate CLASS       bring CLASS to the foreground
 *   probe info CLASS           VISIBLE 0|1, RECT l t r b, EXSTYLE hex of CLASS's window,
 *                              TOPMOST 0|1, ABOVE <class of the nearest visible window above it>
 *   probe caps                 1 if Caps Lock is on (the keyboard's toggle state)
 *   probe workarea             the work area: l t r b
 *   probe windows              every visible top-level window, top to bottom:
 *                              class, rect, exstyle, layered attributes
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int clicks;

static LRESULT CALLBACK pattern_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cr;
        int x, y;
        GetClientRect(hwnd, &cr);
        for (y = 0; y < cr.bottom; y += 8)
            for (x = 0; x < cr.right; x += 8)
            {
                int bx = x / 8, by = y / 8;
                RECT r = { x, y, x + 8, y + 8 };
                HBRUSH b = CreateSolidBrush(RGB((bx * 29 + by * 7) & 255, (by * 41 + bx * 3) & 255, ((bx ^ by) * 23 + 40) & 255));
                FillRect(dc, &r, b);
                DeleteObject(b);
            }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
    {
        WCHAR t[64];
        swprintf(t, 64, L"Pattern %d", ++clicks);
        SetWindowTextW(hwnd, t);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void put_utf8(const WCHAR *w)
{
    static char buf[65536];
    int i, n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), NULL, NULL);
    for (i = 0; i < n - 1; i++)
    {
        if (buf[i] == '\r') continue;
        if (buf[i] == '\n') fputs("\\n", stdout); else putchar(buf[i]);
    }
    putchar('\n');
}

int wmain(int argc, WCHAR **argv)
{
    if (argc >= 6 && !wcscmp(argv[1], L"pattern"))
    {
        WNDCLASSW wc = { 0 };
        MSG msg;
        HWND w;
        wc.lpfnWndProc = pattern_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"SgPattern";
        RegisterClassW(&wc);
        w = CreateWindowExW(0, L"SgPattern", L"Pattern 0", WS_POPUP | WS_VISIBLE,
                            _wtoi(argv[2]), _wtoi(argv[3]), _wtoi(argv[4]), _wtoi(argv[5]), NULL, NULL, wc.hInstance, NULL);
        (void)w;
        while (GetMessageW(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        return 0;
    }
    if (argc >= 3 && !wcscmp(argv[1], L"title"))
    {
        WCHAR t[256];
        HWND w = FindWindowW(argv[2], NULL);
        if (!w) { puts("NOWINDOW"); return 1; }
        GetWindowTextW(w, t, 256);
        put_utf8(t);
        return 0;
    }
    if (argc >= 3 && !wcscmp(argv[1], L"text"))
    {
        static WCHAR text[32768];
        HWND top = FindWindowW(argv[2], NULL), edit;
        if (!top) { puts("NOWINDOW"); return 1; }
        edit = FindWindowExW(top, NULL, L"Edit", NULL);
        if (!edit) edit = FindWindowExW(top, NULL, L"SgNotepadEditor", NULL);
        if (!edit)
        {
            /* Notepad's editor may sit deeper (tabs): the first child that answers */
            HWND c = GetWindow(top, GW_CHILD);
            for (; c; c = GetWindow(c, GW_HWNDNEXT))
            {
                WCHAR cls[64];
                GetClassNameW(c, cls, 64);
                if (wcsstr(cls, L"Editor") || !_wcsicmp(cls, L"Edit")) { edit = c; break; }
            }
        }
        if (!edit) edit = top;
        SendMessageW(edit, WM_GETTEXT, ARRAYSIZE(text), (LPARAM)text);
        put_utf8(text);
        return 0;
    }
    if (argc >= 2 && !wcscmp(argv[1], L"foreground"))
    {
        WCHAR cls[128] = L"(none)";
        HWND w = GetForegroundWindow();
        if (w) GetClassNameW(w, cls, 128);
        put_utf8(cls);
        return 0;
    }
    if (argc >= 3 && !wcscmp(argv[1], L"activate"))
    {
        HWND w = FindWindowW(argv[2], NULL);
        if (!w) { puts("NOWINDOW"); return 1; }
        SetForegroundWindow(w);
        return 0;
    }
    if (argc >= 3 && !wcscmp(argv[1], L"info"))
    {
        HWND w = FindWindowW(argv[2], NULL), a;
        RECT r;
        WCHAR cls[128] = L"(none)";
        if (!w) { puts("VISIBLE 0"); return 1; }
        GetWindowRect(w, &r);
        printf("VISIBLE %d\nRECT %ld %ld %ld %ld\nEXSTYLE %08lx\nTOPMOST %d\n", IsWindowVisible(w) != 0,
               r.left, r.top, r.right, r.bottom, (unsigned long)GetWindowLongW(w, GWL_EXSTYLE),
               (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
        for (a = GetWindow(w, GW_HWNDPREV); a; a = GetWindow(a, GW_HWNDPREV))
        {
            RECT ar, is;
            GetWindowRect(a, &ar);
            if (IsWindowVisible(a) && IntersectRect(&is, &ar, &r)) { GetClassNameW(a, cls, 128); break; }
        }
        printf("ABOVE ");
        put_utf8(cls);
        return 0;
    }
    if (argc >= 2 && !wcscmp(argv[1], L"caps"))
    {
        printf("%d\n", GetKeyState(VK_CAPITAL) & 1);
        return 0;
    }
    if (argc >= 2 && !wcscmp(argv[1], L"workarea"))
    {
        RECT r;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &r, 0);
        printf("%ld %ld %ld %ld\n", r.left, r.top, r.right, r.bottom);
        return 0;
    }
    if (argc >= 2 && !wcscmp(argv[1], L"windows"))
    {
        HWND w;
        for (w = GetTopWindow(NULL); w; w = GetWindow(w, GW_HWNDNEXT))
        {
            WCHAR cls[128];
            RECT r;
            BYTE a = 0; COLORREF k = 0; DWORD f = 0;
            if (!IsWindowVisible(w)) continue;
            GetClassNameW(w, cls, 128);
            GetWindowRect(w, &r);
            if (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_LAYERED) GetLayeredWindowAttributes(w, &k, &a, &f);
            printf("%ls %ld %ld %ld %ld ex=%08lx lwa=%lu alpha=%u key=%06lx\n", cls, r.left, r.top, r.right, r.bottom,
                   (unsigned long)GetWindowLongW(w, GWL_EXSTYLE), (unsigned long)f, a, (unsigned long)k);
        }
        return 0;
    }
    puts("usage");
    return 2;
}
