/* sg-store-height BOTTOM -- for test/store-check.sh: SG Store's window
 * (class SgStore) sized so that its client area ends at screen row BOTTOM;
 * prints the client area's bottom as it is then.
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>

int wmain(int argc, WCHAR **argv)
{
    HWND h = FindWindowW(L"SgStore", NULL);
    RECT wr, cr;
    POINT pt = { 0, 0 };
    if (argc < 2 || !h) return 1;
    GetWindowRect(h, &wr);
    GetClientRect(h, &cr);
    ClientToScreen(h, &pt);
    /* the frame below the client area stays as it is */
    SetWindowPos(h, 0, 0, 0, wr.right - wr.left,
                 _wtoi(argv[1]) - wr.top + (wr.bottom - (pt.y + cr.bottom)), SWP_NOMOVE | SWP_NOZORDER);
    Sleep(500);
    GetClientRect(h, &cr);
    pt.x = pt.y = 0;
    ClientToScreen(h, &pt);
    printf("bottom %ld\n", pt.y + cr.bottom);
    return 0;
}
