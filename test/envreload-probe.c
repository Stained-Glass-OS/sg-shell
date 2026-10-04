/* envreload-check.sh's probe.
 *   envreload-probe broadcast       WM_SETTINGCHANGE "Environment", as the
 *                                   Environment Variables dialog sends it
 *   envreload-probe get EXE VAR     VAR in the running EXE's environment
 *                                   (read from its process parameters), or
 *                                   "(unset)"; "(no process)" without one
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <stdio.h>

static BOOL read_env(DWORD pid, const WCHAR *var)
{
    NTSTATUS (WINAPI *query)(HANDLE, PROCESSINFOCLASS, void *, ULONG, ULONG *) =
        (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess");
    PROCESS_BASIC_INFORMATION pbi;
    PEB peb;
    char params[0x100];   /* RTL_USER_PROCESS_PARAMETERS: winternl.h's ends before Environment */
    static WCHAR env[1 << 20];
    void *envp;
    SIZE_T got = 0, n = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    size_t vl = wcslen(var);
    const WCHAR *p;

    if (!h || query(h, ProcessBasicInformation, &pbi, sizeof(pbi), NULL) ||
        !ReadProcessMemory(h, pbi.PebBaseAddress, &peb, sizeof(peb), NULL) ||
        !ReadProcessMemory(h, peb.ProcessParameters, params, sizeof(params), NULL)) return FALSE;
    /* the Environment pointer: after CommandLine in the parameters (offset 0x80 on x64) */
    memcpy(&envp, params + 0x80, sizeof(envp));
    /* a page at a time, up to the block's end ("\0\0") */
    while (n < ARRAYSIZE(env) - 2048) {
        if (!ReadProcessMemory(h, (char *)envp + n * sizeof(WCHAR), env + n, 4096, &got) || !got) break;
        n += got / sizeof(WCHAR);
        env[n] = env[n + 1] = 0;
        if (n >= 2) { SIZE_T i; for (i = 1; i < n; i++) if (!env[i] && !env[i - 1]) goto done; }
    }
done:
    CloseHandle(h);
    for (p = env; *p; p += wcslen(p) + 1)
        if (!_wcsnicmp(p, var, vl) && p[vl] == '=') { wprintf(L"%ls\n", p + vl + 1); return TRUE; }
    wprintf(L"(unset)\n");
    return TRUE;
}

int wmain(int argc, WCHAR **argv)
{
    if (argc >= 2 && !lstrcmpW(argv[1], L"broadcast")) {
        DWORD_PTR r;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, &r);
        return 0;
    }
    if (argc >= 4 && !lstrcmpW(argv[1], L"get")) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe = { sizeof(pe) };
        if (snap != INVALID_HANDLE_VALUE && Process32FirstW(snap, &pe))
            do if (!lstrcmpiW(pe.szExeFile, argv[2]) && read_env(pe.th32ProcessID, argv[3])) return 0;
            while (Process32NextW(snap, &pe));
        wprintf(L"(no process)\n");
        return 1;
    }
    return 2;
}
