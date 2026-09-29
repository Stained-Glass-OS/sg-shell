/* SG Store's gate stand-ins, one program by name and switches:
 *
 *   winget.exe install --id X ...   a stand-in winget: logs its arguments to
 *                                   C:\gate\winget.log and runs %SG_FAKE_SETUP% /S
 *   <any> ARGS                      an installer: logs its arguments to
 *                                   C:\gate\setup.log, copies itself into
 *                                   C:\Program Files\%SG_FAKE_DIR% and writes an
 *                                   Uninstall entry (%SG_FAKE_KEY%: DisplayName
 *                                   %SG_FAKE_DISPLAY%, DisplayVersion
 *                                   %SG_FAKE_VERSION%) so SG Store detects it.
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

static void env(const WCHAR *name, const WCHAR *def, WCHAR *out, int cch)
{
    if (!GetEnvironmentVariableW(name, out, cch) || !out[0]) lstrcpynW(out, def, cch);
}

int wmain(int argc, WCHAR **argv)
{
    WCHAR self[MAX_PATH], *base, *args = GetCommandLineW();
    WCHAR key[128], display[128], version[64], dir[128];
    WCHAR sub[512], pf[MAX_PATH], dest[MAX_PATH], exe[MAX_PATH];
    (void)argc; (void)argv;
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

    logline(L"C:\\gate\\setup.log", args);
    env(L"SG_FAKE_KEY", L"FakeApp", key, ARRAYSIZE(key));
    env(L"SG_FAKE_DISPLAY", L"Fake App", display, ARRAYSIZE(display));
    env(L"SG_FAKE_VERSION", L"1.10.0", version, ARRAYSIZE(version));
    env(L"SG_FAKE_DIR", L"Fake App", dir, ARRAYSIZE(dir));

    ExpandEnvironmentStringsW(L"%ProgramFiles%", pf, MAX_PATH);
    swprintf(dest, MAX_PATH, L"%ls\\%ls", pf, dir);
    CreateDirectoryW(dest, NULL);
    swprintf(exe, MAX_PATH, L"%ls\\app.exe", dest);
    CopyFileW(self, exe, FALSE);

    swprintf(sub, ARRAYSIZE(sub), L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%ls", key);
    sz(HKEY_LOCAL_MACHINE, sub, L"DisplayName", display);
    sz(HKEY_LOCAL_MACHINE, sub, L"DisplayVersion", version);
    sz(HKEY_LOCAL_MACHINE, sub, L"Publisher", L"The gate");
    return 0;
}
