/* Probe for the voice typing gate (test/dictate-check.sh).
 *
 *   probe text CLASS         the text of CLASS's edit control (WM_GETTEXT), as UTF-8
 *   probe foreground         the class of the foreground window
 *   probe bar                the dictation bar: VISIBLE 0|1, RECT l t r b, EXSTYLE hex, SCREEN w h
 *   probe clip-set TEXT      put TEXT on the clipboard
 *   probe clip-get           what the clipboard holds
 *   probe activate CLASS     bring CLASS to the foreground
 *   probe click CLASS TEXT   click the button labelled TEXT in CLASS's window
 *   probe check CLASS TEXT   1 if that check box is ticked, else 0
 *   probe progress CLASS     each progress bar's position in CLASS's window
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

static void put_utf8(const WCHAR *w)
{
    char buf[65536];
    int i, n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), NULL, NULL);
    for (i = 0; i < n - 1; i++)
    {
        if (buf[i] == '\r') continue;
        if (buf[i] == '\n') fputs("\\n", stdout); else putchar(buf[i]);
    }
    putchar('\n');
}

struct find { const WCHAR *text; HWND found; };

static BOOL CALLBACK find_child(HWND w, LPARAM lp)
{
    struct find *f = (struct find *)lp;
    WCHAR text[256];
    GetWindowTextW(w, text, 256);
    if (!wcscmp(text, f->text)) { f->found = w; return FALSE; }
    return TRUE;
}

static BOOL CALLBACK print_progress(HWND w, LPARAM lp)
{
    WCHAR cls[64];
    (void)lp;
    GetClassNameW(w, cls, 64);
    if (!_wcsicmp(cls, L"msctls_progress32") && IsWindowVisible(w))
        printf("PROGRESS %d\n", (int)SendMessageW(w, PBM_GETPOS, 0, 0));
    return TRUE;
}

int wmain(int argc, WCHAR **argv)
{
    if (argc >= 4 && (!wcscmp(argv[1], L"click") || !wcscmp(argv[1], L"check")))
    {
        struct find f = { argv[3], NULL };
        HWND top = FindWindowW(argv[2], NULL);
        if (!top) { puts("NOWINDOW"); return 1; }
        EnumChildWindows(top, find_child, (LPARAM)&f);
        if (!f.found) { puts("NOCONTROL"); return 1; }
        if (!wcscmp(argv[1], L"check"))
            printf("%d\n", SendMessageW(f.found, BM_GETCHECK, 0, 0) == BST_CHECKED);
        else
            SendMessageW(f.found, BM_CLICK, 0, 0);
        return 0;
    }
    if (argc >= 3 && !wcscmp(argv[1], L"progress"))
    {
        HWND top = FindWindowW(argv[2], NULL);
        if (!top) { puts("NOWINDOW"); return 1; }
        EnumChildWindows(top, print_progress, 0);
        return 0;
    }
    if (argc >= 3 && !wcscmp(argv[1], L"text"))
    {
        HWND top = FindWindowW(argv[2], NULL), edit;
        static WCHAR text[32768];
        if (!top) { puts("NOWINDOW"); return 1; }
        edit = FindWindowExW(top, NULL, L"Edit", NULL);
        SendMessageW(edit ? edit : top, WM_GETTEXT, 32768, (LPARAM)text);
        put_utf8(text);
        return 0;
    }
    if (argc >= 2 && !wcscmp(argv[1], L"foreground"))
    {
        WCHAR cls[128] = L"(none)";
        HWND fg = GetForegroundWindow();
        if (fg) GetClassNameW(fg, cls, 128);
        put_utf8(cls);
        return 0;
    }
    if (argc >= 2 && !wcscmp(argv[1], L"bar"))
    {
        HWND bar = FindWindowW(L"SgDictateBar", NULL);
        RECT r;
        if (!bar) { puts("VISIBLE 0"); puts("NOWINDOW"); return 0; }
        GetWindowRect(bar, &r);
        printf("VISIBLE %d\nRECT %ld %ld %ld %ld\nEXSTYLE %lx\nSCREEN %d %d\n", IsWindowVisible(bar) ? 1 : 0,
               r.left, r.top, r.right, r.bottom, (unsigned long)GetWindowLongW(bar, GWL_EXSTYLE),
               GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
        return 0;
    }
    if (argc >= 3 && !wcscmp(argv[1], L"clip-set"))
    {
        size_t bytes = (wcslen(argv[2]) + 1) * sizeof(WCHAR);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        memcpy(GlobalLock(mem), argv[2], bytes);
        GlobalUnlock(mem);
        if (!OpenClipboard(NULL)) return 1;
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, mem);
        CloseClipboard();
        return 0;
    }
    if (argc >= 2 && !wcscmp(argv[1], L"clip-get"))
    {
        HANDLE h;
        if (!OpenClipboard(NULL)) return 1;
        h = GetClipboardData(CF_UNICODETEXT);
        if (h) { put_utf8(GlobalLock(h)); GlobalUnlock(h); } else puts("(empty)");
        CloseClipboard();
        return 0;
    }
    if (argc >= 3 && !wcscmp(argv[1], L"activate"))
    {
        HWND w = FindWindowW(argv[2], NULL);
        if (!w) return 1;
        SetForegroundWindow(w);
        return 0;
    }
    fputs("usage: probe text CLASS | foreground | bar | clip-set TEXT | clip-get | activate CLASS\n", stderr);
    return 2;
}
