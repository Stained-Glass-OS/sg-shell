/* sg-svc-test: a real Windows service for the gates (sg-shell's
 * Services console, test/services-check.sh): it starts, stops, pauses and continues when told to.
 * Its own name comes from the SCM. `sg-svc-test.exe info NAME` prints the
 * service's STATE, PID and START type (Wine's sc has no qc/queryex). Install with
 *   sc create NAME binPath= "C:\...\sg-svc-test.exe" DisplayName= "..."
 *
 * Copyright 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>

static SERVICE_STATUS_HANDLE handle;
static SERVICE_STATUS status;
static HANDLE stop_event;

static void set_state(DWORD state)
{
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwControlsAccepted = state == SERVICE_START_PENDING ? 0 :
        SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PAUSE_CONTINUE | SERVICE_ACCEPT_SHUTDOWN;
    status.dwWin32ExitCode = NO_ERROR;
    SetServiceStatus(handle, &status);
}

static DWORD WINAPI control(DWORD code, DWORD type, void *data, void *ctx)
{
    switch (code)
    {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        set_state(SERVICE_STOP_PENDING);
        SetEvent(stop_event);
        return NO_ERROR;
    case SERVICE_CONTROL_PAUSE:
        set_state(SERVICE_PAUSED);
        return NO_ERROR;
    case SERVICE_CONTROL_CONTINUE:
        set_state(SERVICE_RUNNING);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        SetServiceStatus(handle, &status);
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI service_main(DWORD argc, WCHAR **argv)
{
    stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    handle = RegisterServiceCtrlHandlerExW(argc ? argv[0] : L"", control, NULL);
    if (!handle) return;
    set_state(SERVICE_START_PENDING);
    set_state(SERVICE_RUNNING);
    WaitForSingleObject(stop_event, INFINITE);
    set_state(SERVICE_STOPPED);
}

#include <stdio.h>

static int info(const WCHAR *name)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), h;
    SERVICE_STATUS_PROCESS st;
    BYTE buf[8192];
    DWORD need;
    if (!scm || !(h = OpenServiceW(scm, name, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG)))
    {
        printf("ERROR %lu\n", GetLastError());
        return 1;
    }
    if (QueryServiceStatusEx(h, SC_STATUS_PROCESS_INFO, (BYTE *)&st, sizeof(st), &need))
        printf("STATE %lu\nPID %lu\n", st.dwCurrentState, st.dwProcessId);
    if (QueryServiceConfigW(h, (QUERY_SERVICE_CONFIGW *)buf, sizeof(buf), &need))
        printf("START %lu\n", ((QUERY_SERVICE_CONFIGW *)buf)->dwStartType);
    return 0;
}

int wmain(int argc, WCHAR **argv)
{
    SERVICE_TABLE_ENTRYW table[] = { { (WCHAR *)L"", service_main }, { NULL, NULL } };
    if (argc == 3 && !lstrcmpW(argv[1], L"info")) return info(argv[2]);
    return StartServiceCtrlDispatcherW(table) ? 0 : 1;
}
