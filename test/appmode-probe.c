/* The app-mode gate's hands (test/appmode-check.sh).
 *
 *   appmode-probe mode light|dark   what Settings does: AppsUseLightTheme, then
 *                                   WM_SETTINGCHANGE "ImmersiveColorSet"
 *   appmode-probe rect CLASS        the class's top-level window: window and
 *                                   client rectangles on the screen
 *   appmode-probe close CLASS       WM_CLOSE to it
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <stdio.h>

int wmain(int argc, WCHAR **argv)
{
    if (argc >= 3 && !lstrcmpW(argv[1], L"mode"))
    {
        DWORD light = !lstrcmpW(argv[2], L"light");
        DWORD_PTR r;
        HKEY key;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                            0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL)) return 1;
        RegSetValueExW(key, L"AppsUseLightTheme", 0, REG_DWORD, (BYTE *)&light, sizeof(light));
        RegCloseKey(key);
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 5000, &r);
        return 0;
    }
    if (argc >= 3 && !lstrcmpW(argv[1], L"rect"))
    {
        HWND w = FindWindowW(argv[2], NULL);
        RECT wr, cr;
        POINT o = { 0, 0 };
        if (!w || !IsWindowVisible(w)) { printf("none\n"); return 1; }
        GetWindowRect(w, &wr);
        GetClientRect(w, &cr);
        ClientToScreen(w, &o);
        printf("%ld %ld %ld %ld %ld %ld %ld %ld\n", wr.left, wr.top, wr.right, wr.bottom,
               o.x, o.y, o.x + cr.right, o.y + cr.bottom);
        return 0;
    }
    if (argc >= 3 && !lstrcmpW(argv[1], L"close"))
    {
        HWND w = FindWindowW(argv[2], NULL);
        if (w) PostMessageW(w, WM_CLOSE, 0, 0);
        return !w;
    }
    fprintf(stderr, "usage: appmode-probe mode light|dark | rect CLASS | close CLASS\n");
    return 2;
}
