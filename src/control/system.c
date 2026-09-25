/* sg-control -- System: basic information about this computer, and the
 * door to renaming it or joining a domain.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"

enum { CMD_CHANGE = SHIELD_ID(CMD_PAGE_FIRST + 1), CMD_GPRESULT = CMD_PAGE_FIRST + 2 };


int program_count(void);

static int count_values(HKEY root, const WCHAR *sub)
{
    HKEY k;
    DWORD values = 0;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    RegQueryInfoKeyW(k, NULL, NULL, NULL, NULL, NULL, NULL, &values, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    return (int)values;
}

void sys_gather(struct sysfacts *f)
{
    DWORD n;
    SYSTEM_INFO si;
    WCHAR name[128], dom[128];
    memset(f, 0, sizeof(*f));
    lstrcpyW(f->edition, L"Stained Glass OS");
    n = ARRAYSIZE(f->computer);
    if (!GetComputerNameW(f->computer, &n)) lstrcpyW(f->computer, L"(unknown)");
    n = ARRAYSIZE(f->fqdn);
    if (!GetComputerNameExW(ComputerNameDnsFullyQualified, f->fqdn, &n) || !f->fqdn[0]) lstrcpyW(f->fqdn, f->computer);
    current_user(name, ARRAYSIZE(name), dom, ARRAYSIZE(dom));
    if (dom[0]) _snwprintf(f->user, ARRAYSIZE(f->user), L"%ls\\%ls", dom, name); else lstrcpynW(f->user, name, ARRAYSIZE(f->user));
    f->elevated = is_elevated();
    f->admin_account = unix_group_has("sg-admins", name);
    machine_role(f->role, f->realm, f->domain, ARRAYSIZE(f->realm));

    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: lstrcpyW(f->arch, L"64-bit (x64)"); break;
    case PROCESSOR_ARCHITECTURE_INTEL: lstrcpyW(f->arch, L"32-bit (x86)"); break;
    case PROCESSOR_ARCHITECTURE_ARM64: lstrcpyW(f->arch, L"64-bit (ARM64)"); break;
    default: lstrcpyW(f->arch, L"unknown"); break;
    }
    f->policy_count =
        count_values(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer") +
        count_values(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System") +
        count_values(HKEY_LOCAL_MACHINE, L"Software\\Policies");
    lstrcpyW(f->os_build, L"(unknown)");
    {
        WCHAR disp[32] = L"", build[32] = L"";
        reg_sz(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion", L"DisplayVersion", disp, ARRAYSIZE(disp));
        reg_sz(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion", L"CurrentBuild", build, ARRAYSIZE(build));
        if (disp[0] && build[0]) _snwprintf(f->os_build, ARRAYSIZE(f->os_build), L"%ls (build %ls)", disp, build);
        else if (build[0]) _snwprintf(f->os_build, ARRAYSIZE(f->os_build), L"build %ls", build);
    }
    lstrcpyW(f->cpu, L"(unknown)");
    reg_sz(HKEY_LOCAL_MACHINE, L"Hardware\\Description\\System\\CentralProcessor\\0", L"ProcessorNameString", f->cpu, ARRAYSIZE(f->cpu));
    {
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) _snwprintf(f->ram, ARRAYSIZE(f->ram), L"%.1f GB", ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
        else lstrcpyW(f->ram, L"(unknown)");
    }
    f->program_count = program_count();
}

/* a section: a heading, then label/value rows */
static int section(int x, int y, int w, const WCHAR *heading)
{
    pg_text(x, y, w, S(24), g_font_cat, COL_TITLE, heading, DT_SINGLELINE);
    pg_rule(x, y + S(26), w);
    return y + S(36);
}

static int row(int x, int y, int w, const WCHAR *label, const WCHAR *value)
{
    pg_text(x + S(16), y, S(200), S(20), g_font_body, COL_SUBTLE, label, DT_SINGLELINE | DT_END_ELLIPSIS);
    pg_text(x + S(220), y, w - S(220), S(20), g_font_body, COL_TEXT, value, DT_SINGLELINE | DT_END_ELLIPSIS);
    return y + S(24);
}

void build_system(void)
{
    static const WCHAR *const labels[] = { L"Rename this computer", L"Windows Update", L"Group Policy results",
                                           NULL, L"See also", L"User Accounts", L"Programs and Features" };
    static const int ids[] = { CMD_CHANGE, NAV(PG_UPDATE), CMD_GPRESULT, 0, -1, NAV(PG_USERS), NAV(PG_PROGRAMS) };
    struct sysfacts f;
    int x = pg_left_pane(labels, ids, ARRAYSIZE(labels)) + S(36), y = S(24), w = pg_width() - x - S(40);
    WCHAR line[256];

    sys_gather(&f);
    pg_title(x, y, L"View basic information about your computer");
    y += S(48);

    y = section(x, y, w, L"Windows edition");
    pg_icon(x + w - S(96), y - S(6), S(80), IC_SYSTEM);
    y = row(x, y, w - S(110), L"Edition", f.edition);
    y = row(x, y, w - S(110), L"Version", f.os_build);
    pg_text(x + S(16), y, w - S(130), S(20), g_font_small, COL_SUBTLE,
            L"Free software: the programs are licensed under the GPL, LGPL and AGPL.", DT_SINGLELINE | DT_END_ELLIPSIS);
    y += S(40);

    y = section(x, y, w, L"System");
    y = row(x, y, w, L"Processor:", f.cpu);
    y = row(x, y, w, L"Installed memory (RAM):", f.ram);
    y = row(x, y, w, L"System type:", f.arch);
    _snwprintf(line, ARRAYSIZE(line), L"%d installed", f.program_count);
    y = row(x, y, w, L"Programs:", line);
    y += S(16);

    y = section(x, y, w, L"Computer name, domain, and workgroup settings");
    pg_link(x + w - S(140), y, L"Change settings", CMD_CHANGE, LINK_SHIELD);
    y = row(x, y, w - S(160), L"Computer name:", f.computer);
    y = row(x, y, w - S(160), L"Full computer name:", f.fqdn);
    if (!lstrcmpW(f.role, L"member")) y = row(x, y, w - S(160), L"Domain:", f.realm);
    else if (!lstrcmpW(f.role, L"dc")) {
        _snwprintf(line, ARRAYSIZE(line), L"%ls (this computer is its domain controller)", f.realm);
        y = row(x, y, w - S(160), L"Domain:", line);
    } else y = row(x, y, w - S(160), L"Workgroup:", L"WORKGROUP");
    _snwprintf(line, ARRAYSIZE(line), L"%d machine %ls in force", f.policy_count, f.policy_count == 1 ? L"policy" : L"policies");
    y = row(x, y, w, L"Group Policy:", line);
    y += S(16);

    y = section(x, y, w, L"Your account");
    y = row(x, y, w, L"Signed in as:", f.user);
    y = row(x, y, w, L"Account type:", f.admin_account ? L"Administrator" : L"Standard user");
    y = row(x, y, w, L"This session:", f.elevated ? L"Running as administrator" :
            f.admin_account ? L"Standard rights; programs ask before they run as administrator" : L"Standard rights");
}

BOOL cmd_system(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    switch (id) {
    case CMD_CHANGE: if (run_elevated(L"/admin rename")) refresh_when_back(); return TRUE;
    case CMD_GPRESULT: {
        /* gpresult is a console program; show its report in a console */
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        WCHAR cmd[] = L"cmd.exe /k gpresult.exe /r";
        if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NEW_CONSOLE, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
        return TRUE;
    }
    }
    return FALSE;
}

void dump_system(void)
{
    struct sysfacts f;
    sys_gather(&f);
    wprintf(L"edition=%ls\n", f.edition);
    wprintf(L"computer=%ls\n", f.computer);
    wprintf(L"fullname=%ls\n", f.fqdn);
    wprintf(L"membership=%ls\n", !lstrcmpW(f.role, L"member") ? f.realm : !lstrcmpW(f.role, L"dc") ? f.realm : L"WORKGROUP");
    wprintf(L"user=%ls\n", f.user);
    wprintf(L"arch=%ls\n", f.arch);
    wprintf(L"admin=%ls\n", f.elevated ? L"yes" : L"no");
    wprintf(L"accounttype=%ls\n", f.admin_account ? L"administrator" : L"standard");
    wprintf(L"policies=%d\n", f.policy_count);
    wprintf(L"osbuild=%ls\n", f.os_build);
    wprintf(L"cpu=%ls\n", f.cpu);
    wprintf(L"ram=%ls\n", f.ram);
    wprintf(L"programs=%d\n", f.program_count);
}
