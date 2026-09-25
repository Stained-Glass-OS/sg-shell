/* sg-control -- facts about the machine that several pages share.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"

void reg_sz(HKEY root, const WCHAR *sub, const WCHAR *val, WCHAR *out, DWORD cch)
{
    HKEY k;
    DWORD type, cb = cch * sizeof(WCHAR);
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    if (RegQueryValueExW(k, val, NULL, &type, (BYTE *)out, &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ))
        out[cch - 1] = 0;
    RegCloseKey(k);
}

DWORD reg_dword(HKEY root, const WCHAR *sub, const WCHAR *val, DWORD def)
{
    HKEY k;
    DWORD type, v = def, cb = sizeof(v);
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return def;
    if (RegQueryValueExW(k, val, NULL, &type, (BYTE *)&v, &cb) != ERROR_SUCCESS || type != REG_DWORD) v = def;
    RegCloseKey(k);
    return v;
}

BOOL reg_set_sz(HKEY root, const WCHAR *sub, const WCHAR *val, const WCHAR *data)
{
    HKEY k;
    LONG r;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return FALSE;
    r = RegSetValueExW(k, val, 0, REG_SZ, (const BYTE *)data, (lstrlenW(data) + 1) * sizeof(WCHAR));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

BOOL reg_set_dword(HKEY root, const WCHAR *sub, const WCHAR *val, DWORD data)
{
    HKEY k;
    LONG r;
    if (RegCreateKeyExW(root, sub, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return FALSE;
    r = RegSetValueExW(k, val, 0, REG_DWORD, (const BYTE *)&data, sizeof(data));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

/* A Unix path as this Windows system names it: through Wine's own mapping
 * (Z: today, \\?\unix\ if Z: is ever removed), not an assumed drive letter. */
void unix_to_dos(const char *unix_path, WCHAR *out, int cch)
{
    typedef WCHAR *(CDECL *fn_t)(const char *);
    static fn_t fn;
    static BOOL looked;
    WCHAR *dos = NULL;
    if (!looked) { fn = (fn_t)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name"); looked = TRUE; }
    if (fn) dos = fn(unix_path);
    if (dos) {
        lstrcpynW(out, dos, cch);
        HeapFree(GetProcessHeap(), 0, dos);
        return;
    }
    {
        int i;
        _snwprintf(out, cch, L"Z:%S", unix_path);
        out[cch - 1] = 0;
        for (i = 0; out[i]; i++) if (out[i] == L'/') out[i] = L'\\';
    }
}

char *read_unix_file(const char *unix_path, DWORD *len)
{
    WCHAR path[MAX_PATH];
    HANDLE h;
    DWORD size, got = 0;
    char *buf;
    unix_to_dos(unix_path, path, MAX_PATH);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 16 * 1024 * 1024) { CloseHandle(h); return NULL; }
    if (!(buf = malloc(size + 1))) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, buf, size, &got, NULL)) got = 0;
    CloseHandle(h);
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

BOOL unix_path_exists(const char *unix_path)
{
    WCHAR path[MAX_PATH];
    unix_to_dos(unix_path, path, MAX_PATH);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

/* /etc/stained-glass/role: role=member|dc, realm=, domain= (sg-domain-join,
 * sg-dc-provision). No file: a workgroup machine. */
void machine_role(WCHAR *role, WCHAR *realm, WCHAR *domain, int cch)
{
    char *text = read_unix_file("/etc/stained-glass/role", NULL), *line, *next;
    role[0] = realm[0] = domain[0] = 0;
    if (!text) return;
    for (line = text; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strncmp(line, "role=", 5)) MultiByteToWideChar(CP_UTF8, 0, line + 5, -1, role, cch);
        else if (!strncmp(line, "realm=", 6)) MultiByteToWideChar(CP_UTF8, 0, line + 6, -1, realm, cch);
        else if (!strncmp(line, "domain=", 7)) MultiByteToWideChar(CP_UTF8, 0, line + 7, -1, domain, cch);
    }
    free(text);
}

/* the signed-in account and the authority that holds it, via the token */
void current_user(WCHAR *name, int cch, WCHAR *domain, int dcch)
{
    HANDLE tok;
    char buf[256];
    DWORD len = sizeof(buf), n = cch, dn = dcch;
    SID_NAME_USE use;
    lstrcpynW(name, L"(unknown)", cch);
    domain[0] = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return;
    if (GetTokenInformation(tok, TokenUser, buf, len, &len))
        LookupAccountSidW(NULL, ((TOKEN_USER *)buf)->User.Sid, name, &n, domain, &dn, &use);
    CloseHandle(tok);
}

/* Is the Unix account user in group (from /etc/group)? Account names are
 * matched case-insensitively, as Windows matches them. */
BOOL unix_group_has(const char *group, const WCHAR *user)
{
    char *text = read_unix_file("/etc/group", NULL), *line, *next, u[256];
    size_t gl = strlen(group);
    BOOL found = FALSE;
    if (!text) return FALSE;
    WideCharToMultiByte(CP_UTF8, 0, user, -1, u, sizeof(u), NULL, NULL);
    for (line = text; line && *line && !found; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strncmp(line, group, gl) && line[gl] == ':') {
            char *m = strrchr(line, ':') + 1, *end;
            for (; m && *m; m = end ? end + 1 : NULL) {
                if ((end = strchr(m, ','))) *end = 0;
                if (!_stricmp(m, u)) { found = TRUE; break; }
            }
        }
    }
    free(text);
    return found;
}

void format_size(ULONGLONG bytes, WCHAR *out, int cch)
{
    if (bytes >= 1024ULL * 1024 * 1024) _snwprintf(out, cch, L"%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024) _snwprintf(out, cch, L"%.1f MB", bytes / (1024.0 * 1024));
    else if (bytes >= 1024) _snwprintf(out, cch, L"%llu KB", bytes / 1024);
    else _snwprintf(out, cch, L"%llu bytes", bytes);
    out[cch - 1] = 0;
}
