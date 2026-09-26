/* sg-control -- Administrative Tools: the Windows administrative tools, as
 * Control Panel > System and Security > Administrative Tools lists them.
 * Each opens by its Windows name through ShellExecute (services.msc through
 * mmc.exe, eventvwr.exe, ... -- wine-sg 0142 and sg-shell's consoles), so
 * what opens here is what the Run box, the Start menu's Windows
 * Administrative Tools and Win+X open.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"
#include <shellapi.h>

enum { CMD_TOOL = CMD_PAGE_FIRST + 700 };

static const struct { const WCHAR *name, *file, *desc; } TOOLS[] = {
    { L"Computer Management", L"compmgmt.msc", L"Manage disks, services, devices, event logs, local users and shared folders." },
    { L"Device Manager",      L"devmgmt.msc",  L"View the hardware in this computer and the drivers it uses." },
    { L"Disk Cleanup",        L"cleanmgr.exe", L"Free disk space by removing files you do not need." },
    { L"Disk Management",     L"diskmgmt.msc", L"View and manage disks, partitions and drive letters." },
    { L"Event Viewer",        L"eventvwr.exe", L"View the event logs and the Stained Glass system log." },
    { L"Registry Editor",     L"regedit.exe",  L"View and change the registry." },
    { L"Resource Monitor",    L"resmon.exe",   L"See how programs use the processor, memory, disk and network." },
    { L"Services",            L"services.msc", L"Start, stop and configure services." },
    { L"System Information",  L"msinfo32.exe", L"View this computer's hardware and software." },
};

void build_admintools(void)
{
    const WCHAR *labels[2] = { L"System and Security", L"System" };
    int ids[2] = { NAV(PG_CAT_SYSSEC), NAV(PG_SYSTEM) };
    int x = pg_left_pane(labels, ids, 2) + S(36), y = S(28), w = pg_width(), i;
    pg_title(x, y, L"Administrative Tools");
    y += S(44);
    for (i = 0; i < (int)ARRAYSIZE(TOOLS); i++)
    {
        pg_icon(x, y, S(32), IC_ADMINTOOLS);
        pg_link(x + S(44), y - S(2), TOOLS[i].name, CMD_TOOL + i, 0);
        pg_text(x + S(44), y + S(18), w - x - S(80), S(18), g_font_body, COL_SUBTLE, TOOLS[i].desc,
                DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        y += S(48);
    }
}

BOOL cmd_admintools(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    if (id >= CMD_TOOL && id < CMD_TOOL + (int)ARRAYSIZE(TOOLS))
    {
        if ((INT_PTR)ShellExecuteW(g_main, NULL, TOOLS[id - CMD_TOOL].file, NULL, NULL, SW_SHOWNORMAL) <= 32)
        {
            WCHAR msg[300];
            _snwprintf(msg, ARRAYSIZE(msg), L"Stained Glass cannot find '%ls'.", TOOLS[id - CMD_TOOL].file);
            msg[ARRAYSIZE(msg) - 1] = 0;
            MessageBoxW(g_main, msg, TOOLS[id - CMD_TOOL].name, MB_OK | MB_ICONERROR);
        }
        return TRUE;
    }
    return FALSE;
}

void dump_admintools(void)
{
    int i;
    for (i = 0; i < (int)ARRAYSIZE(TOOLS); i++) wprintf(L"admintool=%ls\t%ls\n", TOOLS[i].name, TOOLS[i].file);
}
