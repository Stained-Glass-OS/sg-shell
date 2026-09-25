/* sg-mode.h: Windows' light and dark modes, for Stained Glass programs.
 *
 * Settings > Personalization > Colors keeps two choices, as Windows does, in
 * HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize:
 *   AppsUseLightTheme     programs' windows (default 1: light)
 *   SystemUsesLightTheme  the shell -- taskbar, Start, flyouts (default 0: dark)
 * and announces a change with WM_SETTINGCHANGE "ImmersiveColorSet". wine-sg
 * (0160-0163) switches the visual style and the system colours with the app
 * mode; our own drawing follows with these helpers.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 */
#ifndef SG_MODE_H
#define SG_MODE_H

#include <windows.h>

#define SG_PERSONALIZE_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize"

/* TRUE when the shell (system) or programs (apps) are in dark mode */
static inline BOOL sg_mode_dark(BOOL system)
{
    DWORD v = system ? 0 : 1, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, SG_PERSONALIZE_KEY, system ? L"SystemUsesLightTheme" : L"AppsUseLightTheme",
                     RRF_RT_REG_DWORD, NULL, &v, &size))
        v = system ? 0 : 1;
    return v == 0;
}
#define sg_apps_dark()   sg_mode_dark(FALSE)
#define sg_system_dark() sg_mode_dark(TRUE)

/* a WM_SETTINGCHANGE that means the modes (or the accent) may have changed */
static inline BOOL sg_mode_changed(UINT msg, LPARAM lp)
{
    return msg == WM_SETTINGCHANGE && lp && !lstrcmpW((const WCHAR *)lp, L"ImmersiveColorSet");
}

/* the title bar to match (DWMWA_USE_IMMERSIVE_DARK_MODE), as Windows' own
 * programs ask for it; dwmapi is looked up so no program has to link it */
static inline void sg_mode_title(HWND hwnd, BOOL dark)
{
    static HRESULT (WINAPI *set)(HWND, DWORD, const void *, DWORD);
    static BOOL looked;
    BOOL on = dark;
    if (!looked) {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        looked = TRUE;
        if (dwm) set = (void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    }
    if (set) set(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof(on));
}

#endif
