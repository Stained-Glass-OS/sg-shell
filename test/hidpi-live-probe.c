/* hidpi-live-check's eyes and hand (per-monitor aware: it sees the screen's
 * pixels, not Wine's scaled ones):
 *   hidpi-live-probe win CLASS [CHILDCLASS]
 *       the first window of CLASS: "dpi=GetDpiForWindow size=WxH child=WxH"
 *       (its first descendant of CHILDCLASS), or "none"
 *   hidpi-live-probe set DPI
 *       what Settings > Display > Scale does: LogPixels and WM_SETTINGCHANGE
 *       "WindowMetrics" to every window (sg-shell set_system.c scale_write).
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct find { const WCHAR *cls; HWND found; };

static BOOL CALLBACK find_child(HWND hwnd, LPARAM lp)
{
    struct find *f = (struct find *)lp;
    WCHAR cls[128];
    if (GetClassNameW(hwnd, cls, 128) && !lstrcmpiW(cls, f->cls) && IsWindowVisible(hwnd)) { f->found = hwnd; return FALSE; }
    return TRUE;
}

int main(int argc, char **argv)
{
    typedef BOOL (WINAPI *ctx_fn)(HANDLE);
    typedef UINT (WINAPI *dpi_fn)(HWND);
    ctx_fn set_ctx = (ctx_fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    dpi_fn for_window = (dpi_fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");

    if (set_ctx) set_ctx((HANDLE)-4);
    if (argc >= 3 && !strcmp(argv[1], "win")) {
        WCHAR cls[128], child_cls[128];
        struct find f = { child_cls, NULL };
        RECT r = { 0 }, c = { 0 };
        HWND w;
        MultiByteToWideChar(CP_ACP, 0, argv[2], -1, cls, 128);
        if (!(w = FindWindowW(cls, NULL))) { printf("none\n"); return 1; }
        GetWindowRect(w, &r);
        if (argc >= 4) {
            MultiByteToWideChar(CP_ACP, 0, argv[3], -1, child_cls, 128);
            EnumChildWindows(w, find_child, (LPARAM)&f);
            if (f.found) GetWindowRect(f.found, &c);
        }
        printf("dpi=%u size=%ldx%ld child=%ldx%ld\n", for_window ? for_window(w) : 0, r.right - r.left, r.bottom - r.top,
               c.right - c.left, c.bottom - c.top);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "set")) {
        DWORD dpi = atoi(argv[2]);
        DWORD_PTR res;
        HKEY key;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL)) return 1;
        RegSetValueExW(key, L"LogPixels", 0, REG_DWORD, (BYTE *)&dpi, sizeof(dpi));
        RegCloseKey(key);
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"WindowMetrics", SMTO_ABORTIFHUNG, 3000, &res);
        return 0;
    }
    return 2;
}
