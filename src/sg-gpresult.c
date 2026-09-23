/* sg-gpresult -- the Stained Glass OS Group Policy result.
 *
 * A console tool, like Windows' `gpresult /r`, that reports which Group Policy
 * settings are actually in force on this machine and for this user. It reads
 * the live registry -- the machine's policy branches in HKLM and the user's in
 * HKCU -- and lists every setting present, so an administrator can confirm
 * what a policy they applied is doing, rather than guess.
 *
 * On Stained Glass the machine policy branch is administrator-owned and read
 * first (wine-sg 0024/0025), so what this reports under COMPUTER SETTINGS is
 * exactly what binds every user. `gpresult.exe` resolves here through App
 * Paths, as it would reach Microsoft's on Windows.
 *
 * It is deliberately read-only and prints to stdout, so the gate can run it
 * headlessly and check the report against policies it planted.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <stdio.h>

/* The policy branches Windows evaluates, relative to a hive root. Everything
 * under these keys is, by definition, policy. */
static const WCHAR *POLICY_ROOTS[] = {
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies",
    L"Software\\Policies",
};

/* Print one value as "name = data (TYPE)", formatting the common types. */
static void print_value(const WCHAR *keypath, const WCHAR *name,
                        DWORD type, const BYTE *data, DWORD len)
{
    wprintf(L"      %ls\\%ls = ", keypath, name[0] ? name : L"(default)");
    switch (type) {
    case REG_DWORD:
        if (len >= sizeof(DWORD)) wprintf(L"%lu (REG_DWORD)\n", *(const DWORD *)data);
        else wprintf(L"(malformed REG_DWORD)\n");
        break;
    case REG_SZ:
    case REG_EXPAND_SZ:
        wprintf(L"%ls (%ls)\n", (const WCHAR *)data,
                type == REG_SZ ? L"REG_SZ" : L"REG_EXPAND_SZ");
        break;
    default:
        wprintf(L"<%lu bytes> (type %lu)\n", len, type);
        break;
    }
}

/* Recursively walk a policy key, printing every value found. Returns the
 * number of values reported. */
static int walk_key(HKEY root, const WCHAR *subkey)
{
    HKEY k;
    DWORD i, nvals, nsubs, name_max, data_max;
    int count = 0;

    if (RegOpenKeyExW(root, subkey, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;

    if (RegQueryInfoKeyW(k, NULL, NULL, NULL, &nsubs, NULL, NULL,
                         &nvals, &name_max, &data_max, NULL, NULL) != ERROR_SUCCESS)
    {
        RegCloseKey(k);
        return 0;
    }

    /* values directly under this key */
    for (i = 0; i < nvals; i++) {
        WCHAR name[512];
        BYTE data[4096];
        DWORD nlen = ARRAYSIZE(name), dlen = sizeof(data), type;
        if (RegEnumValueW(k, i, name, &nlen, NULL, &type, data, &dlen) == ERROR_SUCCESS) {
            print_value(subkey, name, type, data, dlen);
            count++;
        }
    }

    /* recurse into subkeys */
    for (i = 0; i < nsubs; i++) {
        WCHAR sub[256], child[1024];
        DWORD slen = ARRAYSIZE(sub);
        if (RegEnumKeyExW(k, i, sub, &slen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
            continue;
        _snwprintf(child, ARRAYSIZE(child), L"%ls\\%ls", subkey, sub);
        count += walk_key(root, child);
    }

    RegCloseKey(k);
    return count;
}

static void report_scope(const WCHAR *title, HKEY root)
{
    int total = 0;
    int i;
    size_t r;

    wprintf(L"\n%ls\n", title);
    for (i = 0; i < lstrlenW(title); i++) putwchar(L'-');
    putwchar(L'\n');

    wprintf(L"  Applied policies:\n");
    for (r = 0; r < ARRAYSIZE(POLICY_ROOTS); r++)
        total += walk_key(root, POLICY_ROOTS[r]);

    if (!total)
        wprintf(L"    (none in force)\n");
    wprintf(L"  Total settings in force: %d\n", total);
}

static void report_header(void)
{
    WCHAR comp[MAX_COMPUTERNAME_LENGTH + 1];
    DWORD n = ARRAYSIZE(comp);
    WCHAR user[256], dom[256], name[256];
    DWORD ulen;
    HANDLE tok;
    char buf[256];

    wprintf(L"Stained Glass OS Group Policy Result\n");
    wprintf(L"====================================\n");

    if (GetComputerNameW(comp, &n)) wprintf(L"Computer name: %ls\n", comp);

    lstrcpyW(user, L"(unknown)");
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD len = sizeof(buf);
        DWORD nl = ARRAYSIZE(name), dl = ARRAYSIZE(dom);
        SID_NAME_USE use;
        if (GetTokenInformation(tok, TokenUser, buf, len, &len) &&
            LookupAccountSidW(NULL, ((TOKEN_USER *)buf)->User.Sid,
                              name, &nl, dom, &dl, &use))
        {
            if (dl && dom[0]) _snwprintf(user, ARRAYSIZE(user), L"%ls\\%ls", dom, name);
            else lstrcpynW(user, name, ARRAYSIZE(user));
        }
        CloseHandle(tok);
    }
    wprintf(L"User name:     %ls\n", user);
    (void)ulen;
}

int wmain(void)
{
    report_header();
    report_scope(L"COMPUTER SETTINGS", HKEY_LOCAL_MACHINE);
    report_scope(L"USER SETTINGS", HKEY_CURRENT_USER);
    return 0;
}
