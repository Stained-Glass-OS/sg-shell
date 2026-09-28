/*
 * Linux Terminal (Administrator), in the Start menu: starts sg-root-terminal,
 * a root shell in a terminal window after the user's password. A Windows
 * program only to be found and started where Windows programs are; the
 * terminal itself is Linux's (xterm).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    static char path[] = "/usr/libexec/stained-glass/shell/sg-root-terminal";
    char *argv[] = { path, NULL };
    LONG (WINAPI *spawnvp)(char * const argv[], int wait);

    (void)inst; (void)prev; (void)cmdline; (void)show;
    spawnvp = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__wine_unix_spawnvp");
    if (!spawnvp || spawnvp(argv, FALSE))
        MessageBoxW(NULL, L"The Linux terminal could not be started.", L"Linux Terminal (Administrator)",
                    MB_OK | MB_ICONERROR);
    return 0;
}
