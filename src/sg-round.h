/*
 * A flyout asks for round corners (DWMWA_WINDOW_CORNER_PREFERENCE,
 * DWMWCP_ROUND), which the Rounded style gives it (wine-sg 0497), as newer
 * Windows rounds its flyouts; in the Classic style it keeps square corners.
 * dwmapi is looked up rather than linked: a program keeps running where it
 * is missing.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_ROUND_H
#define SG_ROUND_H

#include <windows.h>

static inline void sg_round_corners(HWND hwnd)
{
    static HRESULT (WINAPI *set)(HWND, DWORD, const void *, DWORD);
    static BOOL looked;
    DWORD pref = 2;   /* DWMWCP_ROUND */

    if (!looked)
    {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        looked = TRUE;
        if (dwm) set = (void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    }
    if (set && hwnd) set(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &pref, sizeof(pref));
}

#endif
