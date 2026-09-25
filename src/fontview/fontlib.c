/* sg-fontview -- installing and removing fonts, and what is installed.
 *
 * Where Windows keeps fonts, so Windows programs (and Wine itself, with
 * wine-sg 0183) find them:
 *
 *   for one person   %LOCALAPPDATA%\Microsoft\Windows\Fonts\<file>, and
 *                    HKCU\Software\Microsoft\Windows NT\CurrentVersion\Fonts
 *                    "<Full name> (TrueType)" = the file's full path
 *                    (Windows 10 1809's per-user fonts: no administrator)
 *   for everyone     %WINDIR%\Fonts\<file>, and the same value under HKLM =
 *                    the file name (an administrator: the elevated token)
 *
 * and where Linux programs find them (fontconfig): a copy in the person's
 * ~/.local/share/fonts/stained-glass, or -- for everyone -- sg-admind copies
 * the file from the machine's %WINDIR%\Fonts into
 * /usr/local/share/fonts/stained-glass (verbs font-install / font-remove).
 * Then AddFontResource and WM_FONTCHANGE, as Windows' installer does.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "fontview.h"
#include <shlobj.h>
#include <shlwapi.h>

static const WCHAR FONTS_KEY[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";
#define MAX_FONT_FILE (64u << 20)

void utf8_to_w(const char *s, WCHAR *out, int cch)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cch)) out[0] = 0;
    out[cch - 1] = 0;
}

BOOL font_read(const WCHAR *path, struct font_file *out)
{
    HANDLE h;
    LARGE_INTEGER size;
    BYTE *buf;
    DWORD got = 0;
    BOOL ok = FALSE;

    memset(out, 0, sizeof(*out));
    GetFullPathNameW(path, MAX_PATH, out->path, NULL);
    h = CreateFileW(out->path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= MAX_FONT_FILE &&
        (buf = malloc((size_t)size.QuadPart))) {
        if (ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) && got == (DWORD)size.QuadPart)
            ok = !fi_parse(buf, got, &out->info);
        free(buf);
        out->size = got;
    }
    CloseHandle(h);
    return ok;
}

BOOL font_is_elevated(void)
{
    HANDLE tok;
    char buf[256];
    DWORD len = sizeof(buf);
    BOOL sys = FALSE, admin = FALSE;
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD n = sizeof(sid);
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenUser, buf, len, &len))
            sys = IsWellKnownSid(((TOKEN_USER *)buf)->User.Sid, WinLocalSystemSid);
        CloseHandle(tok);
    }
    if (CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, sid, &n)) CheckTokenMembership(NULL, sid, &admin);
#ifdef SG_MUTANT_ANYONE
    return TRUE;
#endif
    return sys || admin;
}

void font_user_dir(WCHAR *out, int cch)
{
    WCHAR base[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, base)))
        GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    _snwprintf(out, cch, L"%ls\\Microsoft\\Windows\\Fonts", base);
    out[cch - 1] = 0;
}

static void machine_dir(WCHAR *out, int cch)
{
    WCHAR win[MAX_PATH];
    GetWindowsDirectoryW(win, MAX_PATH);
    _snwprintf(out, cch, L"%ls\\Fonts", win);
    out[cch - 1] = 0;
}

typedef WCHAR *(CDECL *dosname_t)(const char *);

/* an NT path Wine gives us ("\??\Z:\home\x", "\??\unix\home\x") as a Win32 one */
static void nt_to_dos(const WCHAR *nt, WCHAR *out, int cch)
{
    if (!wcsncmp(nt, L"\\??\\unix\\", 9)) _snwprintf(out, cch, L"\\\\?\\unix\\%ls", nt + 9);
    else if (!wcsncmp(nt, L"\\??\\", 4)) lstrcpynW(out, nt + 4, cch);
    else lstrcpynW(out, nt, cch);
    out[cch - 1] = 0;
}

BOOL font_linux_user_dir(WCHAR *out, int cch)
{
    WCHAR env[MAX_PATH], home[MAX_PATH];
    char unix_path[MAX_PATH * 3];
    dosname_t dosname = (dosname_t)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name");

    out[0] = 0;
    /* $XDG_DATA_HOME/fonts if the session sets it (a Unix path) */
    if (GetEnvironmentVariableW(L"XDG_DATA_HOME", env, MAX_PATH) && env[0] == '/' && dosname) {
        WCHAR *dos;
        WideCharToMultiByte(CP_UTF8, 0, env, -1, unix_path, sizeof(unix_path), NULL, NULL);
        if ((dos = dosname(unix_path))) {
            _snwprintf(out, cch, L"%ls\\fonts\\stained-glass", dos);
            HeapFree(GetProcessHeap(), 0, dos);
            out[cch - 1] = 0;
            return TRUE;
        }
    }
    /* else ~/.local/share/fonts; Wine names the Unix home WINEHOMEDIR */
    if (!GetEnvironmentVariableW(L"WINEHOMEDIR", env, MAX_PATH)) return FALSE;
    nt_to_dos(env, home, MAX_PATH);
    _snwprintf(out, cch, L"%ls\\.local\\share\\fonts\\stained-glass", home);
    out[cch - 1] = 0;
    return TRUE;
}

static const WCHAR *base_name(const WCHAR *p)
{
    const WCHAR *s = wcsrchr(p, '\\'), *t = wcsrchr(p, '/');
    if (t > s) s = t;
    return s ? s + 1 : p;
}

/* fc-cache on the Linux folder, so Linux programs see the change; not waited for long */
static void refresh_fontconfig(const WCHAR *dos_dir)
{
    typedef char *(CDECL *unixname_t)(const WCHAR *);
    unixname_t unixname = (unixname_t)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    WCHAR cmd[MAX_PATH * 2 + 64], tool[MAX_PATH];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    char *u;
    WCHAR uw[MAX_PATH];
    if (!unixname || !(u = unixname(dos_dir))) return;
    utf8_to_w(u, uw, MAX_PATH);
    HeapFree(GetProcessHeap(), 0, u);
    if (!GetEnvironmentVariableW(L"SG_FC_CACHE", tool, MAX_PATH)) lstrcpyW(tool, L"\\\\?\\unix\\usr\\bin\\fc-cache");
    if (GetFileAttributesW(tool) == INVALID_FILE_ATTRIBUTES) return;
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" \"%ls\"", tool, uw);
    cmd[ARRAYSIZE(cmd) - 1] = 0;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (CreateProcessW(tool, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static void broadcast_change(void)
{
    DWORD_PTR r;
    SendMessageTimeoutW(HWND_BROADCAST, WM_FONTCHANGE, 0, 0, SMTO_ABORTIFHUNG, 2000, &r);
}

/* ---- sg-admind (the Control Panel's spool; see sg-shell admin/sg-admind) -------------- */
static void spool_dir(const char *sub, WCHAR *out, int cch)
{
    char unix_path[512];
    WCHAR env[400];
    char base[400] = "/run/stained-glass-admin";
    dosname_t dosname = (dosname_t)(void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_dos_file_name");
    WCHAR *dos;
    if (GetEnvironmentVariableW(L"SG_ADMIN_SPOOL", env, ARRAYSIZE(env)))
        WideCharToMultiByte(CP_UTF8, 0, env, -1, base, sizeof(base), NULL, NULL);
    _snprintf(unix_path, sizeof(unix_path), "%s/%s", base, sub);
    unix_path[sizeof(unix_path) - 1] = 0;
    out[0] = 0;
    if (dosname && (dos = dosname(unix_path))) { lstrcpynW(out, dos, cch); HeapFree(GetProcessHeap(), 0, dos); }
}

/* a request with the verb and one argument; TRUE for OK */
static BOOL admin_request(const WCHAR *verb, const WCHAR *arg, WCHAR *msg, int cch)
{
    WCHAR dir[MAX_PATH], tmp[MAX_PATH], req[MAX_PATH], rep[MAX_PATH], id[40];
    char body[1024], reply[1024];
    BYTE rnd[16];
    HANDLE h;
    DWORD n, start;
    int i, len;
    typedef BOOLEAN (WINAPI *gen_t)(PVOID, ULONG);
    gen_t gen = (gen_t)(void *)GetProcAddress(LoadLibraryW(L"advapi32.dll"), "SystemFunction036");

    msg[0] = 0;
    if (!gen || !gen(rnd, sizeof(rnd))) return FALSE;
    for (i = 0; i < 16; i++) _snwprintf(id + i * 2, 3, L"%02x", rnd[i]);
    if (wcspbrk(arg, L"\r\n")) return FALSE;
    len = WideCharToMultiByte(CP_UTF8, 0, verb, -1, body, sizeof(body), NULL, NULL);
    if (!len) return FALSE;
    body[len - 1] = '\n';
    n = WideCharToMultiByte(CP_UTF8, 0, arg, -1, body + len, sizeof(body) - len, NULL, NULL);
    if (!n) return FALSE;
    len += n - 1;
    body[len++] = '\n';

    spool_dir("requests", dir, MAX_PATH);
    if (!dir[0]) return FALSE;
    _snwprintf(tmp, MAX_PATH, L"%ls\\.%ls", dir, id);
    _snwprintf(req, MAX_PATH, L"%ls\\%ls.req", dir, id);
    spool_dir("replies", dir, MAX_PATH);
    _snwprintf(rep, MAX_PATH, L"%ls\\%ls.rep", dir, id);
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { lstrcpynW(msg, L"The administration service is not available.", cch); return FALSE; }
    WriteFile(h, body, (DWORD)len, &n, NULL);
    CloseHandle(h);
    if (!MoveFileExW(tmp, req, 0)) { DeleteFileW(tmp); return FALSE; }
    start = GetTickCount();
    while (GetFileAttributesW(rep) == INVALID_FILE_ATTRIBUTES) {
        if (GetTickCount() - start > 30000) {
            DeleteFileW(req);
            lstrcpynW(msg, L"The administration service did not answer.", cch);
            return FALSE;
        }
        Sleep(100);
    }
    Sleep(50);
    h = CreateFileW(rep, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    n = 0;
    if (h != INVALID_HANDLE_VALUE) { ReadFile(h, reply, sizeof(reply) - 1, &n, NULL); CloseHandle(h); }
    reply[n] = 0;
    DeleteFileW(rep);
    if (!strncmp(reply, "OK", 2)) return TRUE;
    if (!strncmp(reply, "FAILED ", 7)) {
        char *e = strchr(reply + 7, '\n');
        if (e) *e = 0;
        MultiByteToWideChar(CP_UTF8, 0, reply + 7, -1, msg, cch);
    }
    return FALSE;
}

/* ---- the registry ------------------------------------------------------------------ */
static HKEY open_fonts(int scope, REGSAM access)
{
    HKEY k = NULL;
    if (scope == SCOPE_USER) {
        if (access & KEY_SET_VALUE) RegCreateKeyExW(HKEY_CURRENT_USER, FONTS_KEY, 0, NULL, 0, access, NULL, &k, NULL);
        else RegOpenKeyExW(HKEY_CURRENT_USER, FONTS_KEY, 0, access, &k);
    } else if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, FONTS_KEY, 0, access | KEY_WOW64_64KEY, &k))
        k = NULL;
    return k;
}

/* the value in a scope's key naming this file (by value name, full path or file name) */
static BOOL find_value(int scope, const WCHAR *regname, const WCHAR *path, WCHAR *name_out, WCHAR *data_out)
{
    HKEY k = open_fonts(scope, KEY_READ);
    WCHAR name[512], data[MAX_PATH];
    DWORD i, nn, dn, type;
    BOOL found = FALSE;
    if (!k) return FALSE;
    for (i = 0; !found; i++) {
        nn = ARRAYSIZE(name);
        dn = sizeof(data) - sizeof(WCHAR);
        if (RegEnumValueW(k, i, name, &nn, NULL, &type, (BYTE *)data, &dn)) break;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        data[dn / sizeof(WCHAR)] = 0;
        if ((regname && !lstrcmpiW(name, regname)) ||
            (path && (!lstrcmpiW(data, path) || !lstrcmpiW(base_name(data), base_name(path))))) {
            found = TRUE;
            if (name_out) lstrcpynW(name_out, name, 512);
            if (data_out) lstrcpynW(data_out, data, MAX_PATH);
        }
    }
    RegCloseKey(k);
    return found;
}

int font_installed_scope(const struct font_file *f)
{
    char reg8[1024];
    WCHAR reg[512];
    int s = 0;
    fi_registry_name(&f->info, reg8, sizeof(reg8));
    utf8_to_w(reg8, reg, ARRAYSIZE(reg));
    if (find_value(SCOPE_USER, reg, NULL, NULL, NULL)) s |= SCOPE_USER;
    if (find_value(SCOPE_MACHINE, reg, NULL, NULL, NULL)) {
        /* Wine lists every Linux font under HKLM too; only %WINDIR%\Fonts counts as installed */
        WCHAR data[MAX_PATH];
        find_value(SCOPE_MACHINE, reg, NULL, NULL, data);
        if (!wcschr(data, '\\')) s |= SCOPE_MACHINE;
        else {
            WCHAR dir[MAX_PATH];
            machine_dir(dir, MAX_PATH);
            if (!wcsnicmp(data, dir, lstrlenW(dir))) s |= SCOPE_MACHINE;
        }
    }
    return s;
}

DWORD font_install(const WCHAR *path, int scope, WCHAR *msg, int cch)
{
    struct font_file f;
    WCHAR dir[MAX_PATH], dest[MAX_PATH], linux_dir[MAX_PATH], linux_dest[MAX_PATH], reg[512], full[MAX_PATH];
    char reg8[1024];
    const WCHAR *data;
    HKEY k;
    DWORD err;

    msg[0] = 0;
    if (!font_read(path, &f)) {
        _snwprintf(msg, cch, L"%ls is not a valid font file.", base_name(path));
        return ERROR_BAD_FORMAT;
    }
    lstrcpynW(full, f.path, MAX_PATH);
    fi_registry_name(&f.info, reg8, sizeof(reg8));
    utf8_to_w(reg8, reg, ARRAYSIZE(reg));

    if (scope == SCOPE_MACHINE && !font_is_elevated()) {
        lstrcpynW(msg, L"You need to be an administrator to install fonts for all users.", cch);
        return ERROR_ACCESS_DENIED;
    }
    if (scope == SCOPE_MACHINE) machine_dir(dir, MAX_PATH);
    else font_user_dir(dir, MAX_PATH);
    SHCreateDirectoryExW(NULL, dir, NULL);
    _snwprintf(dest, MAX_PATH, L"%ls\\%ls", dir, base_name(full));
    if (lstrcmpiW(dest, full) && !CopyFileW(full, dest, FALSE)) {
        err = GetLastError();
        _snwprintf(msg, cch, err == ERROR_ACCESS_DENIED ? L"You do not have permission to install fonts in %ls."
                                                        : L"The font could not be copied to %ls.", dir);
        return err ? err : ERROR_WRITE_FAULT;
    }
    /* per user: the full path; for everyone: the file name in %WINDIR%\Fonts, as Windows writes them */
    data = scope == SCOPE_MACHINE ? base_name(dest) : dest;
    if (!(k = open_fonts(scope, KEY_SET_VALUE | KEY_READ))) {
        if (scope == SCOPE_MACHINE && (err = RegCreateKeyExW(HKEY_LOCAL_MACHINE, FONTS_KEY, 0, NULL, 0,
                                                             KEY_SET_VALUE | KEY_WOW64_64KEY, NULL, &k, NULL))) k = NULL;
    }
    if (!k) {
        if (lstrcmpiW(dest, full)) DeleteFileW(dest);
        lstrcpynW(msg, L"The font could not be registered.", cch);
        return ERROR_ACCESS_DENIED;
    }
#ifndef SG_MUTANT_NOREG
    err = RegSetValueExW(k, reg, 0, REG_SZ, (const BYTE *)data, (lstrlenW(data) + 1) * sizeof(WCHAR));
#else
    err = 0;
#endif
    RegCloseKey(k);
    if (err) {
        if (lstrcmpiW(dest, full)) DeleteFileW(dest);
        lstrcpynW(msg, L"The font could not be registered.", cch);
        return err;
    }
    AddFontResourceW(dest);

    /* Linux programs (fontconfig) */
    if (scope == SCOPE_USER) {
        if (font_linux_user_dir(linux_dir, MAX_PATH)) {
            SHCreateDirectoryExW(NULL, linux_dir, NULL);
            _snwprintf(linux_dest, MAX_PATH, L"%ls\\%ls", linux_dir, base_name(dest));
            if (CopyFileW(dest, linux_dest, FALSE)) refresh_fontconfig(linux_dir);
        }
    } else {
        WCHAR why[256];
        if (!admin_request(L"font-install", base_name(dest), why, ARRAYSIZE(why)) && why[0])
            _snwprintf(msg, cch, L"Installed for Windows programs. Linux programs will not see it yet: %ls", why);
    }
    broadcast_change();
    if (!msg[0]) _snwprintf(msg, cch, L"%ls was installed%ls.", reg, scope == SCOPE_MACHINE ? L" for all users" : L"");
    return 0;
}

DWORD font_uninstall(const WCHAR *what, int scope, WCHAR *msg, int cch)
{
    WCHAR name[512], data[MAX_PATH], file[MAX_PATH], dir[MAX_PATH], linux_dir[MAX_PATH], linux_file[MAX_PATH];
    HKEY k;
    DWORD err;

    msg[0] = 0;
    if (scope == SCOPE_MACHINE && !font_is_elevated()) {
        lstrcpynW(msg, L"You need to be an administrator to delete fonts installed for all users.", cch);
        return ERROR_ACCESS_DENIED;
    }
    if (!find_value(scope, what, what, name, data)) {
        _snwprintf(msg, cch, L"%ls is not installed%ls.", what, scope == SCOPE_MACHINE ? L" for all users" : L" for you");
        return ERROR_FILE_NOT_FOUND;
    }
    if (scope == SCOPE_MACHINE) machine_dir(dir, MAX_PATH);
    else font_user_dir(dir, MAX_PATH);
    if (!wcschr(data, '\\')) _snwprintf(file, MAX_PATH, L"%ls\\%ls", dir, data);
    else lstrcpynW(file, data, MAX_PATH);

    while (RemoveFontResourceW(file)) ;
    if (!(k = open_fonts(scope, KEY_SET_VALUE))) {
        lstrcpynW(msg, L"You do not have permission to delete this font.", cch);
        return ERROR_ACCESS_DENIED;
    }
    err = RegDeleteValueW(k, name);
    RegCloseKey(k);
    if (err) { lstrcpynW(msg, L"You do not have permission to delete this font.", cch); return err; }
    /* the file only when it is in the fonts folder of that scope (not a file elsewhere it names) */
    if (!wcsnicmp(file, dir, lstrlenW(dir)) && file[lstrlenW(dir)] == '\\') DeleteFileW(file);

    if (scope == SCOPE_USER) {
        if (font_linux_user_dir(linux_dir, MAX_PATH)) {
            _snwprintf(linux_file, MAX_PATH, L"%ls\\%ls", linux_dir, base_name(file));
            while (RemoveFontResourceW(linux_file)) ;
            if (DeleteFileW(linux_file)) refresh_fontconfig(linux_dir);
        }
    } else {
        WCHAR why[256];
        admin_request(L"font-remove", base_name(file), why, ARRAYSIZE(why));
    }
    broadcast_change();
    _snwprintf(msg, cch, L"%ls was deleted.", name);
    return 0;
}

/* ---- families ------------------------------------------------------------------------- */
struct fam_list { struct family *v; int n, cap; };

static int CALLBACK fam_proc(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    struct fam_list *l = (struct fam_list *)lp;
    int i;
    (void)tm;
    if (lf->lfFaceName[0] == '@') return 1;         /* vertical CJK forms */
    for (i = 0; i < l->n; i++) if (!lstrcmpiW(l->v[i].name, lf->lfFaceName)) return 1;
    if (l->n == l->cap) {
        struct family *nv = realloc(l->v, (l->cap = l->cap ? l->cap * 2 : 128) * sizeof(*nv));
        if (!nv) return 0;
        l->v = nv;
    }
    memset(&l->v[l->n], 0, sizeof(l->v[0]));
    lstrcpynW(l->v[l->n].name, lf->lfFaceName, LF_FACESIZE);
    l->v[l->n].charset = lf->lfCharSet;
    l->v[l->n].symbol = lf->lfCharSet == SYMBOL_CHARSET;
    l->v[l->n].truetype = (type & TRUETYPE_FONTTYPE) != 0;
    l->v[l->n].raster = (type & RASTER_FONTTYPE) != 0;
    l->n++;
    return 1;
}

struct style_list { WCHAR names[32][LF_FULLFACESIZE]; int n; };

static int CALLBACK style_proc(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    struct style_list *s = (struct style_list *)lp;
    const ENUMLOGFONTEXW *e = (const ENUMLOGFONTEXW *)lf;
    int i;
    (void)tm; (void)type;
    for (i = 0; i < s->n; i++) if (!lstrcmpiW(s->names[i], e->elfFullName)) return 1;
    if (s->n < 32) lstrcpynW(s->names[s->n++], e->elfFullName, LF_FULLFACESIZE);
    return 1;
}

static int by_name(const void *a, const void *b)
{
    return lstrcmpiW(((const struct family *)a)->name, ((const struct family *)b)->name);
}

int families_load(struct family **out)
{
    struct fam_list l = { 0 };
    LOGFONTW lf = { 0 };
    HDC dc = GetDC(NULL);
    int i;
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(dc, &lf, fam_proc, (LPARAM)&l, 0);
    for (i = 0; i < l.n; i++) {
        struct style_list s;
        s.n = 0;
        lstrcpynW(lf.lfFaceName, l.v[i].name, LF_FACESIZE);
        EnumFontFamiliesExW(dc, &lf, style_proc, (LPARAM)&s, 0);
        l.v[i].styles = s.n ? s.n : 1;
    }
    ReleaseDC(NULL, dc);
    if (l.n) qsort(l.v, l.n, sizeof(l.v[0]), by_name);
    *out = l.v;
    return l.n;
}

void families_free(struct family *f, int n)
{
    int i;
    for (i = 0; i < n; i++) free(f[i].files);
    free(f);
}

/* does a registry value name (without its " (TrueType)") name a face of this family? */
static BOOL name_matches(const WCHAR *regname, const WCHAR *family)
{
    int n = lstrlenW(family);
    const WCHAR *p = regname;
    while (p && *p) {
        if (!wcsnicmp(p, family, n) && (!p[n] || p[n] == ' ' || p[n] == '&' || p[n] == '(')) return TRUE;
        p = wcsstr(p, L" & ");
        if (p) p += 3;
    }
    return FALSE;
}

static void add_file(struct family *f, const WCHAR *regname, const WCHAR *path, int scope)
{
    struct family_file *nv;
    int i;
    for (i = 0; i < f->nfiles; i++) if (!lstrcmpiW(f->files[i].path, path)) return;
    if (!(nv = realloc(f->files, (f->nfiles + 1) * sizeof(*nv)))) return;
    f->files = nv;
    lstrcpynW(nv[f->nfiles].regname, regname, 256);
    lstrcpynW(nv[f->nfiles].path, path, MAX_PATH);
    nv[f->nfiles].scope = scope;
    if (scope > f->scope || !f->nfiles) f->scope = scope;
    f->nfiles++;
}

static void scan_key(struct family *f, int scope)
{
    HKEY k = open_fonts(scope, KEY_READ);
    WCHAR name[512], data[MAX_PATH], path[MAX_PATH], mdir[MAX_PATH], udir[MAX_PATH], ldir[MAX_PATH];
    DWORD i, nn, dn, type;
    if (!k) return;
    machine_dir(mdir, MAX_PATH);
    font_user_dir(udir, MAX_PATH);
    if (!font_linux_user_dir(ldir, MAX_PATH)) ldir[0] = 0;
    for (i = 0; ; i++) {
        struct font_file ff;
        int s, j;
        BOOL same = FALSE;
        nn = ARRAYSIZE(name);
        dn = sizeof(data) - sizeof(WCHAR);
        if (RegEnumValueW(k, i, name, &nn, NULL, &type, (BYTE *)data, &dn)) break;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        data[dn / sizeof(WCHAR)] = 0;
        if (!name_matches(name, f->name)) continue;
        if (!wcschr(data, '\\')) _snwprintf(path, MAX_PATH, L"%ls\\%ls", mdir, data);
        else lstrcpynW(path, data, MAX_PATH);
        /* our own Linux copy of a person's font is not a font of its own */
        if (ldir[0] && !wcsnicmp(path, ldir, lstrlenW(ldir))) continue;
        /* the file must really be this family (the prefix may be another family's) */
        if (!font_read(path, &ff)) continue;
        for (j = 0; j < ff.info.nfaces; j++) {
            WCHAR fam[128];
            utf8_to_w(ff.info.faces[j].family, fam, ARRAYSIZE(fam));
            if (!lstrcmpiW(fam, f->name)) same = TRUE;
        }
        if (!same) continue;
        if (scope == SCOPE_USER) s = SCOPE_USER;
        else if (!wcsnicmp(path, mdir, lstrlenW(mdir))) s = SCOPE_MACHINE;
        else s = 0;
        (void)udir;
        add_file(f, name, path, s);
    }
    RegCloseKey(k);
}

void family_files(struct family *f)
{
    free(f->files);
    f->files = NULL;
    f->nfiles = 0;
    f->scope = 0;
    scan_key(f, SCOPE_USER);
    scan_key(f, SCOPE_MACHINE);
}
