/*
 * SG Office -- its three programs: sg-documents64.exe, sg-spreadsheets64.exe
 * and sg-presentations64.exe (this file, built three times with
 * SG_NATIVE_KIND set). They are what the Start menu lists, what file types
 * open with and what "Open with" offers; each starts SG Office's editors --
 * the native program /usr/bin/sg-office (package sg-office-editors, based on
 * ONLYOFFICE) -- for its kind of document, or for the files given.
 *
 *     sg-documents64.exe [/p] [FILE...]
 *
 * /p (a type's Print verb) opens the files: the editors print from their
 * own File menu, not from the command line.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef SG_NATIVE_KIND
#define SG_NATIVE_KIND L"documents"
#define SG_TITLE L"SG Office Documents"
#endif

/* SG Office's editors: SG_OFFICE_NATIVE names another (the gate's) */
static BOOL native_program(WCHAR *out, int cch)
{
    if (!GetEnvironmentVariableW(L"SG_OFFICE_NATIVE", out, cch))
        lstrcpynW(out, L"Z:\\usr\\bin\\sg-office", cch);
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

/* A Windows path as the native program reads it: its Unix path (Wine's
 * wine_get_unix_file_name); NULL if unknown. */
static WCHAR *unix_path(const WCHAR *dos)
{
    typedef char *(CDECL *unix_name_fn)(const WCHAR *);
    static unix_name_fn fn;
    WCHAR full[MAX_PATH * 2];
    char *u;
    WCHAR *w;
    int n;
    if (!fn) fn = (unix_name_fn)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    if (!fn || !GetFullPathNameW(dos, ARRAYSIZE(full), full, NULL) || !(u = fn(full))) return NULL;
    n = MultiByteToWideChar(CP_UTF8, 0, u, -1, NULL, 0);
    if ((w = malloc(n * sizeof(WCHAR)))) MultiByteToWideChar(CP_UTF8, 0, u, -1, w, n);
    HeapFree(GetProcessHeap(), 0, u);
    return w;
}

static void append_arg(WCHAR *cmd, size_t cap, const WCHAR *arg)
{
    size_t n = wcslen(cmd);
    if (n + wcslen(arg) + 4 >= cap) return;
    swprintf(cmd + n, cap - n, L" \"%ls\"", arg);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    static WCHAR cmd[32768];
    WCHAR program[MAX_PATH], **argv;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    BOOL files = FALSE;
    int argc, i;

    (void)inst; (void)prev; (void)cmdline; (void)show;
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    if (!native_program(program, MAX_PATH)) {
        MessageBoxW(NULL, L"SG Office's editors are not installed. Install SG Office from SG Store.",
                    SG_TITLE, MB_OK | MB_ICONERROR);
        return 1;
    }
    swprintf(cmd, ARRAYSIZE(cmd), L"sg-office");
    for (i = 1; i < argc; i++) {
        WCHAR *u;
        if (!argv[i][0] || !_wcsicmp(argv[i], L"/p") || !_wcsicmp(argv[i], L"-p")) continue;
        files = TRUE;
        if (wcsstr(argv[i], L"://") || !(u = unix_path(argv[i]))) append_arg(cmd, ARRAYSIZE(cmd), argv[i]);
        else { append_arg(cmd, ARRAYSIZE(cmd), u); free(u); }
    }
    if (!files) { wcscat(cmd, L" --new "); wcscat(cmd, SG_NATIVE_KIND); }
    if (!CreateProcessW(program, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        MessageBoxW(NULL, L"SG Office could not be started.", SG_TITLE, MB_OK | MB_ICONERROR);
        LocalFree(argv);
        return 1;
    }
    /* a Unix program: Wine starts it and returns no handles */
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    LocalFree(argv);
    return 0;
}
