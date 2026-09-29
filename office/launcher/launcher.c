/*
 * SG Office -- its three programs: sg-documents64.exe, sg-spreadsheets64.exe
 * and sg-presentations64.exe (this file, built three times with SG_KIND
 * set). They are what the Start menu lists, what file types open with and
 * what "Open with" offers; each starts LibreOffice's Windows program
 * (soffice.exe, installed by Get SG Office) for its kind of document, or
 * for the files given.
 *
 *     sg-documents64.exe [/p] [FILE...]      /p prints the files instead
 *
 * Not installed yet: it runs Get SG Office, and carries on once it is.
 * Installed with older SG Office settings (an sg-office update): it has them
 * applied first (one administrator prompt).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef SG_KIND
#define SG_KIND L"--writer"
#define SG_TITLE L"SG Office Documents"
#endif

static BOOL office_program_dir(WCHAR *out, int cch)
{
    DWORD cb = cch * sizeof(WCHAR);
    WCHAR soffice[MAX_PATH + 16];
    out[0] = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\LibreOffice\\UNO\\InstallPath", NULL,
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, out, &cb) || !out[0])
        ExpandEnvironmentStringsW(L"%ProgramFiles%\\LibreOffice\\program", out, cch);
    swprintf(soffice, ARRAYSIZE(soffice), L"%ls\\soffice.exe", out);
    return GetFileAttributesW(soffice) != INVALID_FILE_ATTRIBUTES;
}

/* Get SG Office, beside us; its exit code */
static DWORD setup(const WCHAR *args)
{
    WCHAR self[MAX_PATH], *slash;
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    DWORD code = 1;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    if ((slash = wcsrchr(self, '\\'))) lstrcpyW(slash + 1, L"sg-office-setup64.exe");
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpFile = self;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return 1;
    WaitForSingleObject(sei.hProcess, INFINITE);
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code;
}

static void append_arg(WCHAR *cmd, size_t cap, const WCHAR *arg)
{
    size_t n = wcslen(cmd);
    if (n + wcslen(arg) + 4 >= cap) return;
    swprintf(cmd + n, cap - n, L" \"%ls\"", arg);
}

static BOOL ends_with(const WCHAR *s, const WCHAR *tail)
{
    size_t a = wcslen(s), b = wcslen(tail);
    return a >= b && !_wcsicmp(s + a - b, tail);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    static WCHAR cmd[32768];
    WCHAR program[MAX_PATH], soffice[MAX_PATH + 16], **argv;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    BOOL print = FALSE, files = FALSE, show_mode = FALSE;
    int argc, i;

    (void)inst; (void)prev; (void)cmdline; (void)show;
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    if (!office_program_dir(program, MAX_PATH)) {
        if (setup(L"") || !office_program_dir(program, MAX_PATH)) return 1;   /* not installed (the user said no) */
    } else if (setup(L"/status") != 0) {
        setup(L"/apply");     /* an older payload: bring it up to date; carry on either way */
    }
    swprintf(soffice, ARRAYSIZE(soffice), L"%ls\\soffice.exe", program);
    for (i = 1; i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/p") || !_wcsicmp(argv[i], L"-p")) print = TRUE;
        else if (argv[i][0]) {
            files = TRUE;
            if (ends_with(argv[i], L".ppsx") || ends_with(argv[i], L".pps") || ends_with(argv[i], L".ppsm")) show_mode = TRUE;
        }
    }
    swprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" --nologo", soffice);
    if (print) wcscat(cmd, L" -p");
    else if (show_mode) wcscat(cmd, L" --show");
    else if (!files) { wcscat(cmd, L" "); wcscat(cmd, SG_KIND); }
    for (i = 1; i < argc; i++) {
        WCHAR full[MAX_PATH * 2];
        if (!_wcsicmp(argv[i], L"/p") || !_wcsicmp(argv[i], L"-p") || !argv[i][0]) continue;
        /* a relative path is the caller's; soffice.exe runs in its own folder */
        if (!wcsstr(argv[i], L"://") && GetFullPathNameW(argv[i], ARRAYSIZE(full), full, NULL)) append_arg(cmd, ARRAYSIZE(cmd), full);
        else append_arg(cmd, ARRAYSIZE(cmd), argv[i]);
    }
    if (!CreateProcessW(soffice, cmd, NULL, NULL, FALSE, 0, NULL, program, &si, &pi)) {
        MessageBoxW(NULL, L"SG Office could not be started.", SG_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    LocalFree(argv);
    return 0;
}
