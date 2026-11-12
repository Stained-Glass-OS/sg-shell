/* accentshade-probe: sg-mode.h's accent and its shades (see accentshade-check.sh) */
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include "../src/sg-mode.h"

static int fails;
static void check(BOOL ok, const char *what, COLORREF got)
{
    printf("%s  %s (got %02X%02X%02X)\n", ok ? "PASS" : "FAIL", what, GetRValue(got), GetGValue(got), GetBValue(got));
    if (!ok) fails++;
}

static void set_accent(DWORD rgb)
{
    DWORD abgr = 0xFF000000 | (rgb & 0xFF) << 16 | (rgb & 0xFF00) | (rgb >> 16 & 0xFF);
    HKEY key;
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL)) {
        RegSetValueExW(key, L"AccentColor", 0, REG_DWORD, (BYTE *)&abgr, sizeof(abgr));
        RegCloseKey(key);
    }
}

static BOOL close_to(COLORREF a, COLORREF b)
{
    return abs(GetRValue(a) - GetRValue(b)) <= 2 && abs(GetGValue(a) - GetGValue(b)) <= 2 && abs(GetBValue(a) - GetBValue(b)) <= 2;
}

int wmain(void)
{
    double h, s, v, gh, gs, gv;
    COLORREF c;

    setvbuf(stdout, NULL, _IONBF, 0);
    RegDeleteKeyValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor");
    c = sg_accent();
    check(c == RGB(0x7B, 0x2F, 0xBE), "no accent chosen: Stained Glass purple", c);
    c = sg_accent_shade(RGB(0xE0, 0xDA, 0xEC));
    check(c == RGB(0xE0, 0xDA, 0xEC), "with the default accent a shade is the colour drawn", c);

    set_accent(0x107C41);
    c = sg_accent();
    check(c == RGB(0x10, 0x7C, 0x41), "a new accent is read at once, not up to a second later (the repaint after ImmersiveColorSet)", c);
    c = sg_accent_shade(RGB(0x7B, 0x2F, 0xBE));
    check(close_to(c, RGB(0x10, 0x7C, 0x41)), "the default purple itself becomes the accent", c);
    sg_hsv_of(RGB(0x10, 0x7C, 0x41), &gh, &gs, &gv);
    c = sg_accent_shade(RGB(0xE0, 0xDA, 0xEC));     /* Settings' selected row */
    sg_hsv_of(c, &h, &s, &v);
    check(v >= 0.85 && fabs(h - gh) <= 14, "a pale purple tint becomes a pale tint of the accent (Settings' selected row)", c);
    c = sg_accent_shade(RGB(0x5F, 0x24, 0x96));
    sg_hsv_of(c, &h, &s, &v);
    check(v < gv && fabs(h - gh) <= 14, "a deep purple shade becomes a deeper shade of the accent", c);
    c = sg_accent_shade(RGB(0x00, 0x78, 0xD7));
    check(c == RGB(0x00, 0x78, 0xD7), "another hue (blue) is left as drawn", c);
    c = sg_accent_shade(RGB(0xE5, 0xE5, 0xE5));
    check(c == RGB(0xE5, 0xE5, 0xE5), "a grey is left as drawn", c);
    printf("RESULT: %s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
