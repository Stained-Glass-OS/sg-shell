/* Drives the taskbar flyout reopen-guard race for the gate.
 *
 * A second click on the tray icon while the flyout is open must CLOSE it, not
 * flicker it shut-then-open. The click's mouse-down deactivates the flyout
 * (WM_ACTIVATE/WA_INACTIVE -> the flyout hides), so by the time the icon's
 * WM_LBUTTONUP is handled the flyout is already hidden; without the guard the
 * handler reopens it. This tool reproduces exactly that order against the
 * running flyout process and prints the flyout's visibility after each step.
 *
 *   sg-fly-poke <TrayClass> <FlyoutClass>
 *   -> prints: shown=<0|1> after=<0|1>
 * With the guard: shown=1 after=0. Without it: shown=1 after=1.
 */
#include <windows.h>
#include <stdio.h>

#define WM_TRAY (WM_APP + 1)

int wmain(int argc, WCHAR **argv)
{
    HWND tray, fly;
    int shown, after, i;

    if (argc < 3) { printf("usage: sg-fly-poke TrayClass FlyoutClass\n"); return 2; }
    for (i = 0, tray = NULL, fly = NULL; i < 50 && (!tray || !fly); i++) {
        if (!tray) tray = FindWindowW(argv[1], NULL);
        if (!fly)  fly  = FindWindowW(argv[2], NULL);
        Sleep(100);
    }
    if (!tray || !fly) { printf("NOWIN tray=%p fly=%p\n", (void *)tray, (void *)fly); return 2; }

    /* open the flyout with a left click on the tray icon */
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONUP);
    Sleep(400);
    shown = IsWindowVisible(fly) ? 1 : 0;

    /* the second click: its mouse-down deactivates the flyout (it hides),
     * then its button-up reaches the tray icon a few ms later */
    PostMessageW(fly, WM_ACTIVATE, WA_INACTIVE, 0);
    Sleep(30);
    PostMessageW(tray, WM_TRAY, 0, WM_LBUTTONUP);
    Sleep(400);
    after = IsWindowVisible(fly) ? 1 : 0;

    printf("shown=%d after=%d\n", shown, after);
    return 0;
}
