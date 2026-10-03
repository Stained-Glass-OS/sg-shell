/* store-check.sh: a process that is not the store asks the store's
 * elevated helper (its pipe, by name) to run something; prints the reply.
 *   sg-store-helperprobe PIPE FILE ARGS
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <windows.h>
#include <stdio.h>
int wmain(int argc, WCHAR **argv)
{
    WCHAR req[2048], reply[64] = L"";
    DWORD got = 0;
    HANDLE p;
    int len;
    if (argc < 4) return 2;
    len = _snwprintf(req, 2048, L"RUN\n%ls\n%ls", argv[2], argv[3]);
    {   /* the pipe may not be up yet, or busy: a few seconds' tries */
        int tries;
        for (tries = 0; tries < 100; tries++) {
            p = CreateFileW(argv[1], GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
            if (p != INVALID_HANDLE_VALUE) break;
            if (GetLastError() == ERROR_PIPE_BUSY) WaitNamedPipeW(argv[1], 200); else Sleep(100);
        }
    }
    if (p == INVALID_HANDLE_VALUE) { printf("noconnect %lu\n", GetLastError()); return 1; }
    WriteFile(p, req, (len + 1) * sizeof(WCHAR), &got, NULL);
    ReadFile(p, reply, sizeof(reply) - 2, &got, NULL);
    reply[got / 2] = 0;
    printf("reply %ls\n", reply);
    CloseHandle(p);
    return 0;
}
