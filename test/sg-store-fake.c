/* SG Store's gate stand-ins, one program by name and switches:
 *
 *   winget.exe install --id X ...   a stand-in winget: logs its arguments to
 *                                   C:\gate\winget.log and runs %SG_FAKE_SETUP% /S
 *   <any> ARGS                      an installer: logs its arguments to
 *                                   C:\gate\setup.log, copies itself into
 *                                   C:\Program Files\%SG_FAKE_DIR% and writes an
 *                                   Uninstall entry (%SG_FAKE_KEY%: DisplayName
 *                                   %SG_FAKE_DISPLAY%, DisplayVersion
 *                                   %SG_FAKE_VERSION%) so SG Store detects it,
 *                                   whose DisplayIcon is app.exe -- or, with
 *                                   %SG_FAKE_LINK%, an uninstaller, and
 *                                   shortcuts in all users' Start menu: the
 *                                   program's and "Uninstall <name>"
 *   app.exe ARGS                    the installed program: logs its arguments
 *                                   to C:\gate\launch.log (Open started it)
 *   uninst.exe /key=K ARGS          its uninstaller (the entry's UninstallString
 *                                   /ui, QuietUninstallString /quiet): logs its
 *                                   arguments to C:\gate\uninstall.log and
 *                                   deletes the Uninstall entry K
 *   <any> /Q /T:"DIR" /C              an IExpress package, unpacking: copies
 *                                   the files of %SG_FAKE_MSIS% into DIR
 * %SG_FAKE_SLEEP% (ms): the installer takes that long (the store's queue).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
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

static void shortcut(const WCHAR *lnk, const WCHAR *target, const WCHAR *args)
{
    IShellLinkW *sl;
    IPersistFile *pf;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return;
    sl->lpVtbl->SetPath(sl, target);
    sl->lpVtbl->SetArguments(sl, args);
    if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        pf->lpVtbl->Save(pf, lnk, TRUE);
        pf->lpVtbl->Release(pf);
    }
    sl->lpVtbl->Release(sl);
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

    if (!_wcsicmp(base, L"app.exe")) {
        logline(L"C:\\gate\\launch.log", args);
        return 0;
    }

    if (!_wcsicmp(base, L"uninst.exe")) {
        const WCHAR *k = wcsstr(args, L"/key=");
        logline(L"C:\\gate\\uninstall.log", args);
        if (k) {
            WCHAR name[128];
            int n = 0;
            for (k += 5; *k && *k != L' ' && n < 127; k++) name[n++] = *k;
            name[n] = 0;
            swprintf(sub, ARRAYSIZE(sub), L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%ls", name);
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, sub);
        }
        return 0;
    }

    logline(L"C:\\gate\\setup.log", args);
    {
        const WCHAR *t = wcsstr(args, L"/T:");
        WCHAR from[MAX_PATH], to[MAX_PATH], pat[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE find;
        int n = 0;
        if (t && wcsstr(args, L" /C") && GetEnvironmentVariableW(L"SG_FAKE_MSIS", from, MAX_PATH)) {
            for (t += 3; *t == L'"'; t++);
            while (*t && *t != L'"' && n < MAX_PATH - 1) to[n++] = *t++;
            to[n] = 0;
            swprintf(pat, MAX_PATH, L"%ls\\*", from);
            if ((find = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return 4;
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                swprintf(a, MAX_PATH, L"%ls\\%ls", from, fd.cFileName);
                swprintf(b, MAX_PATH, L"%ls\\%ls", to, fd.cFileName);
                CopyFileW(a, b, FALSE);
            } while (FindNextFileW(find, &fd));
            FindClose(find);
            return 0;
        }
    }
    {
        WCHAR ms[16];
        if (GetEnvironmentVariableW(L"SG_FAKE_SLEEP", ms, ARRAYSIZE(ms))) Sleep(_wtoi(ms));
    }
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
    {
        WCHAR un[MAX_PATH], cmd[MAX_PATH + 160];
        swprintf(un, MAX_PATH, L"%ls\\uninst.exe", dest);
        CopyFileW(self, un, FALSE);
        swprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" /key=%ls /ui", un, key);
        sz(HKEY_LOCAL_MACHINE, sub, L"UninstallString", cmd);
        swprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" /key=%ls /quiet", un, key);
        sz(HKEY_LOCAL_MACHINE, sub, L"QuietUninstallString", cmd);
    }
    if (GetEnvironmentVariableW(L"SG_FAKE_LINK", NULL, 0)) {
        WCHAR programs[MAX_PATH], lnk[MAX_PATH], icon[MAX_PATH];
        swprintf(icon, MAX_PATH, L"%ls\\uninst.exe,0", dest);
        sz(HKEY_LOCAL_MACHINE, sub, L"DisplayIcon", icon);
        CoInitialize(NULL);
        SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, 0, programs);
        swprintf(lnk, MAX_PATH, L"%ls\\Uninstall %ls.lnk", programs, display);
        shortcut(lnk, exe, L"/uninstall");
        swprintf(lnk, MAX_PATH, L"%ls\\%ls.lnk", programs, display);
        shortcut(lnk, exe, L"/from-shortcut");
        CoUninitialize();
    } else {
        WCHAR icon[MAX_PATH];
        swprintf(icon, MAX_PATH, L"%ls,0", exe);
        sz(HKEY_LOCAL_MACHINE, sub, L"DisplayIcon", icon);
    }
    return 0;
}
