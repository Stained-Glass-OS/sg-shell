/* sg-evt-probe: writes events for the Event Viewer gate (test/eventvwr-check.sh).
 *
 *   sg-evt-probe register LOG SOURCE MSGDLL   the source's EventMessageFile
 *   sg-evt-probe report LOG SOURCE TYPE ID STRING...
 *                                             ReportEvent (TYPE: error, warning,
 *                                             info, success, failure); prints RESULT
 *   sg-evt-probe open LOG                     OpenEventLog: RESULT <error> and the
 *                                             record count
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <stdio.h>

int wmain(int argc, WCHAR **argv)
{
    if (argc == 5 && !lstrcmpW(argv[1], L"register"))
    {
        WCHAR key[512];
        HKEY h;
        DWORD types = 7;
        LONG r;
        _snwprintf(key, 512, L"System\\CurrentControlSet\\Services\\EventLog\\%ls\\%ls", argv[2], argv[3]);
        r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &h, NULL);
        if (!r)
        {
            r = RegSetValueExW(h, L"EventMessageFile", 0, REG_EXPAND_SZ, (BYTE *)argv[4], (lstrlenW(argv[4]) + 1) * 2);
            RegSetValueExW(h, L"TypesSupported", 0, REG_DWORD, (BYTE *)&types, 4);
            RegCloseKey(h);
        }
        printf("RESULT %ld\n", r);
        return 0;
    }
    if (argc >= 6 && !lstrcmpW(argv[1], L"report"))
    {
        HANDLE h = RegisterEventSourceW(NULL, argv[3]);
        WORD type = !lstrcmpW(argv[4], L"error") ? EVENTLOG_ERROR_TYPE : !lstrcmpW(argv[4], L"warning") ?
                    EVENTLOG_WARNING_TYPE : !lstrcmpW(argv[4], L"success") ? EVENTLOG_AUDIT_SUCCESS :
                    !lstrcmpW(argv[4], L"failure") ? EVENTLOG_AUDIT_FAILURE : EVENTLOG_INFORMATION_TYPE;
        const WCHAR *strings[16];
        int n = 0, i;
        BOOL ok;
        for (i = 6; i < argc && n < 16; i++) strings[n++] = argv[i];
        if (!h) { printf("RESULT %lu\n", GetLastError()); return 0; }
        ok = ReportEventW(h, type, 0, wcstoul(argv[5], NULL, 10), NULL, n, 0, strings, NULL);
        printf("RESULT %lu\n", ok ? 0 : GetLastError());
        DeregisterEventSource(h);
        return 0;
    }
    if (argc == 3 && !lstrcmpW(argv[1], L"open"))
    {
        HANDLE h = OpenEventLogW(NULL, argv[2]);
        DWORD n = 0;
        if (!h) { printf("RESULT %lu\n", GetLastError()); return 0; }
        GetNumberOfEventLogRecords(h, &n);
        printf("RESULT 0\nCOUNT %lu\n", n);
        CloseEventLog(h);
        return 0;
    }
    return 2;
}
