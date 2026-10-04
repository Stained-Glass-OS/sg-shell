/*
 * Linux Terminal, in the Start menu: a terminal window on Linux itself.
 * Started as it is, it is the user's own shell: a Terminal tab of the Linux
 * profile, with Terminal's tabs (David 2026-10-04: PowerShell opened with
 * tabs, the Linux Terminal without); without Terminal, sg-linux-terminal. With
 * --admin -- Start's "Run as administrator" -- it is a root shell after the
 * user's password (sg-root-terminal, sudo -i). A Windows program only to be
 * found and started where Windows programs are; the terminal itself is
 * Linux's (xterm). Not through the elevation broker: its SYSTEM account is
 * not Linux's root, and sudo asks the user who is there.
 * SG_LINUX_TERMINAL_DIR: another folder for the two scripts (the gate).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    char dir[MAX_PATH] = "/usr/libexec/stained-glass/shell", path[MAX_PATH + 32];
    char *argv[] = { path, NULL };
    BOOL admin = cmdline && wcsstr(cmdline, L"--admin") != NULL;
    LONG (WINAPI *spawnvp)(char * const argv[], int wait);
    DWORD n;

    (void)inst; (void)prev; (void)show;
#ifndef SG_MUTANT_LINUX_TERMINAL_NOT_TABBED
    if (!admin)
    {
        WCHAR term[MAX_PATH];
        DWORD cb = sizeof(term);
        if (!RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\wt.exe", NULL,
                          RRF_RT_REG_SZ, NULL, term, &cb) && GetFileAttributesW(term) != INVALID_FILE_ATTRIBUTES &&
            (INT_PTR)ShellExecuteW(NULL, NULL, term, L"-p Linux", NULL, SW_SHOWNORMAL) > 32)
            return 0;
    }
#endif
    n = GetEnvironmentVariableA("SG_LINUX_TERMINAL_DIR", dir, sizeof(dir));
    if (!n || n >= sizeof(dir) || dir[0] != '/') strcpy(dir, "/usr/libexec/stained-glass/shell");
    snprintf(path, sizeof(path), "%s/%s", dir, admin ? "sg-root-terminal" : "sg-linux-terminal");
    spawnvp = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__wine_unix_spawnvp");
    if (!spawnvp || spawnvp(argv, FALSE))
        MessageBoxW(NULL, L"The Linux terminal could not be started.", admin ? L"Linux Terminal (Administrator)" : L"Linux Terminal",
                    MB_OK | MB_ICONERROR);
    return 0;
}
