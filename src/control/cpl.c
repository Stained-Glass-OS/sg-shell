/* sg-control -- hosting .cpl applets: Wine's (Internet Options, Game
 * Controllers, Display) and any a program installs, through the CPlApplet
 * protocol every Control Panel speaks.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"
#include <cpl.h>

/* .cpl files this Control Panel replaces with pages, or lists by name already */
static const WCHAR *const KNOWN[] = {
    L"appwiz.cpl", L"timedate.cpl", L"sysdm.cpl", L"ncpa.cpl", L"nusrmgr.cpl", L"wscui.cpl",
    L"desk.cpl", L"joy.cpl", L"inetcpl.cpl", L"intl.cpl", L"wuaucpl.cpl",
};

static BOOL known(const WCHAR *name)
{
    size_t i;
    for (i = 0; i < ARRAYSIZE(KNOWN); i++) if (!_wcsicmp(name, KNOWN[i])) return TRUE;
    return FALSE;
}

static void cpl_path(const WCHAR *file, WCHAR *out)
{
    if (wcschr(file, L'\\') || wcschr(file, L'/')) { lstrcpynW(out, file, MAX_PATH); return; }
    GetSystemDirectoryW(out, MAX_PATH);
    lstrcatW(out, L"\\");
    lstrcatW(out, file);
}

int cpl_list(struct cpl_item *out, int max)
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    GetSystemDirectoryW(pattern, MAX_PATH);
    lstrcatW(pattern, L"\\*.cpl");
    if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        HMODULE mod;
        APPLET_PROC proc;
        LONG count, i;
        if (known(fd.cFileName)) continue;
        cpl_path(fd.cFileName, path);
        if (!(mod = LoadLibraryW(path))) continue;
        if ((proc = (APPLET_PROC)(void *)GetProcAddress(mod, "CPlApplet")) && proc(NULL, CPL_INIT, 0, 0)) {
            count = proc(NULL, CPL_GETCOUNT, 0, 0);
            for (i = 0; i < count && n < max; i++) {
                NEWCPLINFOW nci;
                CPLINFO ci;
                struct cpl_item *it = &out[n];
                memset(it, 0, sizeof(*it));
                lstrcpynW(it->file, fd.cFileName, MAX_PATH);
                it->index = i;
                memset(&nci, 0, sizeof(nci));
                nci.dwSize = sizeof(nci);
                proc(NULL, CPL_NEWINQUIRE, i, (LPARAM)&nci);
                if (nci.szName[0]) {
                    lstrcpynW(it->name, nci.szName, ARRAYSIZE(it->name));
                    lstrcpynW(it->info, nci.szInfo, ARRAYSIZE(it->info));
                } else {
                    memset(&ci, 0, sizeof(ci));
                    proc(NULL, CPL_INQUIRE, i, (LPARAM)&ci);
                    if (ci.idName) LoadStringW(mod, ci.idName, it->name, ARRAYSIZE(it->name));
                    if (ci.idInfo) LoadStringW(mod, ci.idInfo, it->info, ARRAYSIZE(it->info));
                }
                if (it->name[0]) n++;
            }
            proc(NULL, CPL_EXIT, 0, 0);
        }
        FreeLibrary(mod);
    } while (n < max && FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* an applet runs in a process of its own: its dialogs are modal */
BOOL cpl_open_file(const WCHAR *file, const WCHAR *arg)
{
    WCHAR self[MAX_PATH], cmd[MAX_PATH * 3];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" /cpl \"%ls\"%ls%ls", self, file, arg ? L" " : L"", arg ? arg : L"");
    cmd[ARRAYSIZE(cmd) - 1] = 0;
    if (!CreateProcessW(self, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return FALSE;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

/* Run a .cpl's applet here, as control.exe does: CPL_INIT, find the applet
 * (arg "@n" picks one), CPL_STARTWPARMS or CPL_DBLCLK, then CPL_STOP and
 * CPL_EXIT. Returns -1 if the file is not an applet. */
int cpl_run_inproc(const WCHAR *file, const WCHAR *arg)
{
    WCHAR path[MAX_PATH];
    HMODULE mod;
    APPLET_PROC proc;
    LONG count, index = 0;
    CPLINFO ci;
    HWND owner;
    const WCHAR *params = NULL;

    cpl_path(file, path);
    if (!(mod = LoadLibraryW(path))) return -1;
    if (!(proc = (APPLET_PROC)(void *)GetProcAddress(mod, "CPlApplet"))) { FreeLibrary(mod); return -1; }
    owner = CreateWindowExW(0, L"STATIC", L"Control Panel", WS_POPUP, 0, 0, 0, 0, NULL, NULL, g_inst, NULL);
    if (!proc(owner, CPL_INIT, 0, 0)) { DestroyWindow(owner); FreeLibrary(mod); return -1; }
    count = proc(owner, CPL_GETCOUNT, 0, 0);
    if (arg && arg[0] == L'@') { index = _wtoi(arg + 1); params = wcschr(arg, L','); if (params) params++; }
    else if (arg && arg[0]) params = arg;
    if (index < 0 || index >= count) index = 0;
    memset(&ci, 0, sizeof(ci));
    proc(owner, CPL_INQUIRE, index, (LPARAM)&ci);
    if (!params || !proc(owner, CPL_STARTWPARMSW, index, (LPARAM)params))
        proc(owner, CPL_DBLCLK, index, ci.lData);
    proc(owner, CPL_STOP, index, ci.lData);
    proc(owner, CPL_EXIT, 0, 0);
    DestroyWindow(owner);
    FreeLibrary(mod);
    return 0;
}
