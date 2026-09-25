/* sg-charmap-probe: the clipboard, for test/charmap-check.sh.
 *
 *   sg-charmap-probe clip        the clipboard's text as "U+XXXX U+XXXX ..." (or EMPTY)
 *   sg-charmap-probe set TEXT    put TEXT on the clipboard
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <stdio.h>

int wmain(int argc, WCHAR **argv)
{
    if (argc >= 2 && !lstrcmpW(argv[1], L"clip"))
    {
        HANDLE h;
        const WCHAR *t;
        int i;
        if (!OpenClipboard(NULL)) { printf("NOCLIP\n"); return 1; }
        if (!(h = GetClipboardData(CF_UNICODETEXT)) || !(t = GlobalLock(h)) || !t[0]) printf("EMPTY\n");
        else
        {
            for (i = 0; t[i]; i++) printf("%sU+%04X", i ? " " : "", t[i]);
            printf("\n");
            GlobalUnlock(h);
        }
        CloseClipboard();
        return 0;
    }
    if (argc >= 3 && !lstrcmpW(argv[1], L"set"))
    {
        size_t n = (lstrlenW(argv[2]) + 1) * sizeof(WCHAR);
        HGLOBAL m = GlobalAlloc(GMEM_MOVEABLE, n);
        memcpy(GlobalLock(m), argv[2], n);
        GlobalUnlock(m);
        if (!OpenClipboard(NULL)) return 1;
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, m);
        CloseClipboard();
        return 0;
    }
    return 2;
}
