/* sg-terminal-probe: a console program for test/terminal-check.sh.
 *
 *   sg-terminal-probe size    prints size=COLSxROWS (the console's window)
 *   sg-terminal-probe vt      turns on ENABLE_VIRTUAL_TERMINAL_PROCESSING and
 *                             writes "vtred" in SGR 91, then resets
 *   sg-terminal-probe attr    writes "attrgreen" with SetConsoleTextAttribute
 *   sg-terminal-probe boxes   writes "boxes", then a line of 40 U+2500 and a
 *                             rounded box (Claude Code's look), UTF-16
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    DWORD mode = 0, w;
    if (argc < 2) return 2;
    if (!strcmp(argv[1], "size")) {
        if (!GetConsoleScreenBufferInfo(out, &info)) { printf("size=error %lu\n", GetLastError()); return 1; }
        printf("size=%dx%d\n", info.srWindow.Right - info.srWindow.Left + 1, info.srWindow.Bottom - info.srWindow.Top + 1);
        return 0;
    }
    if (!strcmp(argv[1], "vt")) {
        GetConsoleMode(out, &mode);
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        WriteConsoleA(out, "\x1b[91mvtred\x1b[0m\r\n", 16, &w, NULL);
        SetConsoleMode(out, mode);
        return 0;
    }
    if (!strcmp(argv[1], "attr")) {
        GetConsoleScreenBufferInfo(out, &info);
        SetConsoleTextAttribute(out, FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        WriteConsoleA(out, "attrgreen", 9, &w, NULL);
        SetConsoleTextAttribute(out, info.wAttributes);
        WriteConsoleA(out, "\r\n", 2, &w, NULL);
        return 0;
    }
    if (!strcmp(argv[1], "boxes")) {
        WCHAR line[64];
        int i;
        WriteConsoleW(out, L"boxes\r\n", 7, &w, NULL);
        for (i = 0; i < 40; i++) line[i] = 0x2500;
        line[40] = '\r'; line[41] = '\n';
        WriteConsoleW(out, line, 42, &w, NULL);
        WriteConsoleW(out, L"\x256d\x2500\x2500\x256e\r\n\x2570\x2500\x2500\x256f\r\n", 12, &w, NULL);
        return 0;
    }
    return 2;
}
