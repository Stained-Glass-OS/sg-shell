/* round-check.sh's probe: a top-level window's DWMWA_WINDOW_CORNER_PREFERENCE.
 *   round-probe CLASS TITLE   prints "pref=N hr=HR", or "none" */
#include <windows.h>
#include <stdio.h>

int wmain(int argc, WCHAR **argv)
{
    HRESULT (WINAPI *get)(HWND, DWORD, void *, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    DWORD pref = 99;
    HWND w;
    HRESULT hr;

    if (argc < 3 || !dwm || !(get = (void *)GetProcAddress(dwm, "DwmGetWindowAttribute"))) return 2;
    if (!(w = FindWindowW(argv[1][0] ? argv[1] : NULL, argv[2]))) { printf("none\n"); return 1; }
    hr = get(w, 33, &pref, sizeof(pref));
    printf("pref=%lu hr=%08lx\n", pref, hr);
    return 0;
}
