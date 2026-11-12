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

/* The accent colour Settings > Personalization > Colors chose, as Windows keeps
 * it: HKCU\Software\Microsoft\Windows\DWM AccentColor, 0xAABBGGRR -- its low
 * three bytes are a COLORREF. Read again only once the key has changed (it is
 * asked for while painting): the key is watched, so the repaint that follows
 * "ImmersiveColorSet" -- a change is announced so, as the modes are
 * (sg_mode_changed) -- already has the new one. Without the watch, once a
 * second. */
static inline COLORREF sg_accent(void)
{
    static COLORREF cached = RGB(0x7B, 0x2F, 0xBE);
    static HKEY key;
    static HANDLE changed;
    static DWORD when;
    static BOOL have, watching;
    DWORD now = GetTickCount(), v, size = sizeof(v);
    BOOL read = !have;
#ifndef SG_MUTANT_ACCENT_THROTTLED
    if (!watching && !have) {
        /* created if no accent was ever chosen: a key that is not there cannot be watched */
        if (!RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", 0, NULL, 0, KEY_NOTIFY, NULL, &key, NULL) &&
            (changed = CreateEventW(NULL, FALSE, FALSE, NULL)))
            watching = TRUE;
    }
    if (watching && have && !WaitForSingleObject(changed, 0)) read = TRUE;
    if (watching && read)
        RegNotifyChangeKeyValue(key, FALSE, REG_NOTIFY_CHANGE_LAST_SET, changed, TRUE);
#endif
    if (!watching && now - when > 1000) read = TRUE;
    if (read) {
        if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                          RRF_RT_REG_DWORD, NULL, &v, &size))
            cached = v & 0xFFFFFF;
        else
            cached = RGB(0x7B, 0x2F, 0xBE);
        when = now;
        have = TRUE;
    }
    return cached;
}

/* c mixed towards to by pct percent: the accent's lighter (hover) and darker
 * (pressed) shades */
static inline COLORREF sg_mix(COLORREF c, COLORREF to, int pct)
{
    return RGB(GetRValue(c) + (GetRValue(to) - GetRValue(c)) * pct / 100,
               GetGValue(c) + (GetGValue(to) - GetGValue(c)) * pct / 100,
               GetBValue(c) + (GetBValue(to) - GetBValue(c)) * pct / 100);
}
#define sg_accent_light(pct) sg_mix(sg_accent(), RGB(0xFF, 0xFF, 0xFF), (pct))
#define sg_accent_dark(pct)  sg_mix(sg_accent(), RGB(0, 0, 0), (pct))

/* A colour drawn against the default accent (Stained Glass purple, 0x7B2FBE) --
 * a selection's tint, a hover's, a dark-mode link's lighter purple -- as the
 * same shade of the accent chosen now: its hue becomes the accent's, its
 * saturation and value keep their place relative to the key's (below it they
 * scale, above it the rest of the way to full scales), so a pale tint stays
 * pale and a deep shade deep. Colours of another hue come back unchanged.
 * uxtheme does the same to our visual style (wine-sg 7746). */
static inline void sg_hsv_of(COLORREF c, double *h, double *s, double *v)
{
    double r = GetRValue(c) / 255.0, g = GetGValue(c) / 255.0, b = GetBValue(c) / 255.0;
    double hi = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b), d = hi - lo;
    *v = hi;
    *s = hi > 0 ? d / hi : 0;
    if (d <= 0) *h = 0;
    else if (hi == r) *h = 60 * ((g - b) / d);
    else if (hi == g) *h = 60 * ((b - r) / d + 2);
    else *h = 60 * ((r - g) / d + 4);
    if (*h < 0) *h += 360;
}
static inline double sg_shade_shift(double x, double key, double accent)
{
    return x <= key ? x * accent / key : accent + (x - key) / (1 - key) * (1 - accent);
}
static inline COLORREF sg_accent_shade(COLORREF designed)
{
    const double key_h = 271.88, key_s = 0.7526, key_v = 0.7451;
    COLORREF accent = sg_accent();
    double h, s, v, ah, as, av, dh, c, x, m, hh, r, g, b;
    int i;
#ifdef SG_MUTANT_SHADE_FIXED
    return designed;
#endif
    if (accent == RGB(0x7B, 0x2F, 0xBE)) return designed;
    sg_hsv_of(designed, &h, &s, &v);
    dh = h > key_h ? h - key_h : key_h - h;
    if (dh > 180) dh = 360 - dh;
    if (dh > 22 || s < 0.04 || v < 0.15) return designed;
    sg_hsv_of(accent, &ah, &as, &av);
    s = sg_shade_shift(s, key_s, as); if (s > 1) s = 1;
    v = sg_shade_shift(v, key_v, av); if (v > 1) v = 1;
    c = v * s; m = v - c; hh = ah / 60; i = (int)hh % 6;
    x = hh - 2 * (int)(hh / 2) - 1; if (x < 0) x = -x;
    x = c * (1 - x);
    switch (i) {
    case 0: r = c; g = x; b = 0; break;
    case 1: r = x; g = c; b = 0; break;
    case 2: r = 0; g = c; b = x; break;
    case 3: r = 0; g = x; b = c; break;
    case 4: r = x; g = 0; b = c; break;
    default: r = c; g = 0; b = x; break;
    }
    return RGB((BYTE)((r + m) * 255 + 0.5), (BYTE)((g + m) * 255 + 0.5), (BYTE)((b + m) * 255 + 0.5));
}

#endif
