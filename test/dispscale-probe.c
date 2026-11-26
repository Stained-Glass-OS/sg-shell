/* dispscale-probe (from wine-sg test/pmscale-probe.c): each monitor's own scale (wine-sg 9971).
 *   list           MON <n> <left> <top> <right> <bottom> <dpi> [primary]; SYSDPI <dpi>
 *   get N          SCALE <n> <min> <cur> <max>   (DisplayConfigGetDeviceInfo, Windows' type -3)
 *   set N REL      SET <n> <error>, then the list (DisplayConfigSetDeviceInfo, type -4)
 *   move X Y       a per-monitor v2 window at 100,100 moved to X,Y and back: A/B/C <dpi>, DPICHANGED <dpi> */
#include <windows.h>
#include <shellscalingapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dpi_get { DISPLAYCONFIG_DEVICE_INFO_HEADER header; INT32 min, cur, max; };
struct dpi_set { DISPLAYCONFIG_DEVICE_INFO_HEADER header; INT32 rel; };

static HMONITOR mons[8]; static int nmons;
static BOOL CALLBACK collect(HMONITOR m, HDC dc, LPRECT r, LPARAM l) { if (nmons < 8) mons[nmons++] = m; return TRUE; }

static void list(void)
{
    int i;
    for (i = 0; i < nmons; i++)
    {
        MONITORINFOEXW mi = { .cbSize = sizeof(mi) }; UINT x = 0, y = 0;
        GetMonitorInfoW(mons[i], (MONITORINFO *)&mi);
        GetDpiForMonitor(mons[i], MDT_EFFECTIVE_DPI, &x, &y);
        printf("MON %d %ld %ld %ld %ld %u%s\n", i, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right,
               mi.rcMonitor.bottom, x, mi.dwFlags & MONITORINFOF_PRIMARY ? " primary" : "");
    }
    printf("SYSDPI %u\n", GetDpiForSystem());
}

/* the DisplayConfig source of monitor N (by its GDI name) */
static BOOL source_of(int n, LUID *luid, UINT32 *id)
{
    DISPLAYCONFIG_PATH_INFO paths[16]; DISPLAYCONFIG_MODE_INFO modes[32];
    UINT32 np = 16, nm = 32, i;
    MONITORINFOEXW mi = { .cbSize = sizeof(mi) };
    GetMonitorInfoW(mons[n], (MONITORINFO *)&mi);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL)) return FALSE;
    for (i = 0; i < np; i++)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME name = { { DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(name),
                                                    paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id } };
        if (DisplayConfigGetDeviceInfo(&name.header)) continue;
        if (lstrcmpiW(name.viewGdiDeviceName, mi.szDevice)) continue;
        *luid = paths[i].sourceInfo.adapterId; *id = paths[i].sourceInfo.id;
        return TRUE;
    }
    return FALSE;
}

static int changes;
static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_DPICHANGED)
    {
        RECT *r = (RECT *)lp; changes++;
        printf("DPICHANGED %u\n", HIWORD(wp)); fflush(stdout);
        SetWindowPos(h, 0, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}
static void pump(int ms) { DWORD e = GetTickCount() + ms; MSG m; while ((int)(e - GetTickCount()) > 0) { while (PeekMessageW(&m, 0, 0, 0, PM_REMOVE)) DispatchMessageW(&m); Sleep(10); } }

int main(int argc, char **argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    EnumDisplayMonitors(0, 0, collect, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2 || !strcmp(argv[1], "list")) { list(); return 0; }
    if (!strcmp(argv[1], "get") || !strcmp(argv[1], "set"))
    {
        int n = atoi(argv[2]); LUID luid; UINT32 id;
        if (n >= nmons || !source_of(n, &luid, &id)) { printf("NOSOURCE %d\n", n); return 1; }
        if (!strcmp(argv[1], "get"))
        {
            struct dpi_get g = { { (DISPLAYCONFIG_DEVICE_INFO_TYPE)-3, sizeof(g), luid, id } };
            LONG r = DisplayConfigGetDeviceInfo(&g.header);
            if (r) printf("SCALE %d error %ld\n", n, r);
            else printf("SCALE %d %d %d %d\n", n, g.min, g.cur, g.max);
        }
        else
        {
            struct dpi_set s = { { (DISPLAYCONFIG_DEVICE_INFO_TYPE)-4, sizeof(s), luid, id }, atoi(argv[3]) };
            printf("SET %d %ld\n", n, DisplayConfigSetDeviceInfo(&s.header));
            list();
        }
        return 0;
    }
    if (!strcmp(argv[1], "settingchange"))
    {
        DWORD_PTR r;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"WindowMetrics", SMTO_ABORTIFHUNG, 3000, &r);
        return 0;
    }
    if (!strcmp(argv[1], "place") && argc > 4)
    {
        WCHAR cls[64]; HWND w; RECT r;
        MultiByteToWideChar(CP_ACP, 0, argv[2], -1, cls, 64);
        if (!(w = FindWindowW(cls, NULL)) && !(w = FindWindowW(NULL, cls))) { printf("NOWINDOW\n"); return 1; }
        SetWindowPos(w, 0, atoi(argv[3]), atoi(argv[4]), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Sleep(500);
        GetWindowRect(w, &r);
        printf("PLACED %ld %ld %ld %ld dpi %u\n", r.left, r.top, r.right, r.bottom, GetDpiForWindow(w));
        return 0;
    }
    if (!strcmp(argv[1], "move"))
    {
        WNDCLASSW wc = { .lpfnWndProc = proc, .lpszClassName = L"pmscale" };
        HWND w; RECT r;
        RegisterClassW(&wc);
        w = CreateWindowW(L"pmscale", L"pmscale", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 400, 300, 0, 0, 0, 0);
        pump(800);
        printf("A %u\n", GetDpiForWindow(w));
        SetWindowPos(w, 0, atoi(argv[2]), atoi(argv[3]), 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        pump(800);
        GetWindowRect(w, &r);
        printf("B %u %ld %ld changes %d\n", GetDpiForWindow(w), r.right - r.left, r.bottom - r.top, changes);
        SetWindowPos(w, 0, 100, 100, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
        pump(800);
        GetWindowRect(w, &r);
        printf("C %u %ld %ld changes %d\n", GetDpiForWindow(w), r.right - r.left, r.bottom - r.top, changes);
        return 0;
    }
    return 2;
}
