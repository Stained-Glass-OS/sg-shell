/* The browser gate's stand-ins, one program by name and switches:
 *
 *   winget.exe install --id X ...   a stand-in winget: logs its arguments to
 *                                   C:\gate\winget.log and runs %SG_FAKE_SETUP% /S
 *   <any> /S [...]                  an installer: logs its arguments to
 *                                   C:\gate\setup.log, copies itself to
 *                                   C:\Program Files\Fake Browser\fakebrowser.exe and
 *                                   registers it as a browser (StartMenuInternet,
 *                                   Capabilities, FakeBrowserURL/FakeBrowserHTML)
 *   <any> ARGS                      the browser: appends its command line to
 *                                   C:\gate\browser.log
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static void logline(const WCHAR *file, const WCHAR *text)
{
    char u[8192];
    FILE *f;
    CreateDirectoryW(L"C:\\gate", NULL);
    if (!(f = _wfopen(file, L"ab"))) return;
    WideCharToMultiByte(CP_UTF8, 0, text, -1, u, sizeof(u), NULL, NULL);
    fprintf(f, "%s\n", u);
    fclose(f);
}

static void sz(HKEY root, const WCHAR *sub, const WCHAR *name, const WCHAR *val)
{
    HKEY k;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)val, (DWORD)(wcslen(val) + 1) * sizeof(WCHAR));
    RegCloseKey(k);
}

int wmain(int argc, WCHAR **argv)
{
    WCHAR self[MAX_PATH], *base, *args = GetCommandLineW();
    int i, silent = 0;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    base = wcsrchr(self, '\\') ? wcsrchr(self, '\\') + 1 : self;
    if (!_wcsicmp(base, L"winget.exe")) {
        WCHAR setup[MAX_PATH], cmd[MAX_PATH + 16];
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        DWORD code = 1;
        logline(L"C:\\gate\\winget.log", args);
        if (!GetEnvironmentVariableW(L"SG_FAKE_SETUP", setup, MAX_PATH)) return 2;
        swprintf(cmd, MAX_PATH + 16, L"\"%ls\" /S", setup);
        if (!CreateProcessW(setup, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 3;
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        return (int)code;
    }
    for (i = 1; i < argc; i++) if (!wcscmp(argv[i], L"/S")) silent = 1;
    if (silent) {
        WCHAR dir[MAX_PATH], exe[MAX_PATH], cmd[MAX_PATH + 32];
        logline(L"C:\\gate\\setup.log", args);
        ExpandEnvironmentStringsW(L"%ProgramFiles%\\Fake Browser", dir, MAX_PATH);
        CreateDirectoryW(dir, NULL);
        swprintf(exe, MAX_PATH, L"%ls\\fakebrowser.exe", dir);
        if (!CopyFileW(self, exe, FALSE)) return 4;
        swprintf(cmd, MAX_PATH + 32, L"\"%ls\"", exe);
        sz(HKEY_LOCAL_MACHINE, L"Software\\Clients\\StartMenuInternet\\FakeBrowser", NULL, L"Fake Browser");
        sz(HKEY_LOCAL_MACHINE, L"Software\\Clients\\StartMenuInternet\\FakeBrowser\\shell\\open\\command", NULL, cmd);
        sz(HKEY_LOCAL_MACHINE, L"Software\\Clients\\StartMenuInternet\\FakeBrowser\\Capabilities\\URLAssociations", L"http", L"FakeBrowserURL");
        sz(HKEY_LOCAL_MACHINE, L"Software\\Clients\\StartMenuInternet\\FakeBrowser\\Capabilities\\URLAssociations", L"https", L"FakeBrowserURL");
        sz(HKEY_LOCAL_MACHINE, L"Software\\Clients\\StartMenuInternet\\FakeBrowser\\Capabilities\\FileAssociations", L".html", L"FakeBrowserHTML");
        swprintf(cmd, MAX_PATH + 32, L"\"%ls\" -url \"%%1\"", exe);
        sz(HKEY_LOCAL_MACHINE, L"Software\\Classes\\FakeBrowserURL", NULL, L"Fake Browser URL");
        sz(HKEY_LOCAL_MACHINE, L"Software\\Classes\\FakeBrowserURL", L"URL Protocol", L"");
        sz(HKEY_LOCAL_MACHINE, L"Software\\Classes\\FakeBrowserURL\\shell\\open\\command", NULL, cmd);
        sz(HKEY_LOCAL_MACHINE, L"Software\\Classes\\FakeBrowserHTML", NULL, L"Fake Browser HTML Document");
        sz(HKEY_LOCAL_MACHINE, L"Software\\Classes\\FakeBrowserHTML\\shell\\open\\command", NULL, cmd);
        sz(HKEY_LOCAL_MACHINE, L"Software\\Classes\\.html\\OpenWithProgids", L"FakeBrowserHTML", L"");
        return 0;
    }
    logline(L"C:\\gate\\browser.log", args);
    return 0;
}
