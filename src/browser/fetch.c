/* sg-browser -- Get a web browser: finding, downloading, checking and
 * installing a browser from its maker.
 *
 * What to download comes from the winget community repository's manifests
 * (github.com/microsoft/winget-pkgs, MIT-licensed data): the newest version's
 * installer manifest names the maker's download URL, its SHA-256 and how to
 * run it silently. We never redistribute a browser: the file comes from the
 * maker at the user's request, and it is run only if its SHA-256 is the
 * manifest's. With winget installed (the user's own), winget does it instead.
 *
 * The repository can be replaced -- an organisation's mirror, or the gate's
 * local copy -- with HKLM\Software\Stained Glass\Web Browsers ListUrl and
 * RawUrl (or SG_BROWSER_LIST_URL / SG_BROWSER_RAW_URL): ListUrl + "m/Mozilla/
 * Firefox" answers a GitHub contents listing (JSON, the version folders),
 * RawUrl + "m/Mozilla/Firefox/<version>/Mozilla.Firefox.installer.yaml" the
 * manifest.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "browser.h"
#include <wininet.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>

#define DEFAULT_LIST L"https://api.github.com/repos/microsoft/winget-pkgs/contents/manifests/"
#define DEFAULT_RAW  L"https://raw.githubusercontent.com/microsoft/winget-pkgs/master/manifests/"
#define MAX_MANIFEST (4 << 20)

static void seterr(WCHAR *err, int cch, const WCHAR *fmt, ...)
{
    va_list ap;
    if (!err) return;
    va_start(ap, fmt);
    vswprintf(err, cch, fmt, ap);
    va_end(ap);
    err[cch - 1] = 0;
}

static void source_url(const WCHAR *name, const WCHAR *def, WCHAR *out, int cch)
{
    WCHAR env[64];
    DWORD cb = cch * sizeof(WCHAR);
    swprintf(env, 64, L"SG_BROWSER_%ls_URL", !wcscmp(name, L"ListUrl") ? L"LIST" : L"RAW");
    if (GetEnvironmentVariableW(env, out, cch) && out[0]) return;
    if (!RegGetValueW(HKEY_LOCAL_MACHINE, BROWSERS_KEY, name, RRF_RT_REG_SZ, NULL, out, &cb) && out[0]) return;
    lstrcpynW(out, def, cch);
}

/* "Mozilla.Firefox" -> "m/Mozilla/Firefox" */
static BOOL id_path(const WCHAR *id, WCHAR *out, int cch)
{
    int n = 0;
    const WCHAR *p;
    if (!id[0] || cch < 4) return FALSE;
    for (p = id; *p; p++)
        if (!iswalnum(*p) && *p != '.' && *p != '-' && *p != '_') return FALSE;
    out[n++] = towlower(id[0]);
    out[n++] = '/';
    for (p = id; *p && n < cch - 1; p++) out[n++] = *p == '.' ? '/' : *p;
    out[n] = 0;
    return !*p;
}

/* ---- HTTP -------------------------------------------------------------------------------------- */

static HINTERNET g_net;

static HINTERNET net(void)
{
    if (!g_net) g_net = InternetOpenW(L"StainedGlass-GetBrowser/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    return g_net;
}

static HINTERNET open_url(const WCHAR *url, const WCHAR *headers, DWORD *status, WCHAR *err, int cch)
{
    HINTERNET h;
    DWORD st = 0, len = sizeof(st);
    if (_wcsnicmp(url, L"https://", 8) && _wcsnicmp(url, L"http://", 7)) {
        seterr(err, cch, L"Only web addresses can be downloaded (%ls).", url);
        return NULL;
    }
    if (!net() || !(h = InternetOpenUrlW(g_net, url, headers, headers ? (DWORD)-1 : 0,
                                         INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0))) {
        seterr(err, cch, L"Can't reach %ls (error %lu). Check your internet connection.", url, GetLastError());
        return NULL;
    }
    if (HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &st, &len, NULL)) *status = st;
    else *status = 200;   /* not HTTP (never: only http and https are opened) */
    return h;
}

/* the whole answer, NUL-terminated; NULL on failure. 404 is reported in *status. */
static char *http_get(const WCHAR *url, DWORD *status, WCHAR *err, int cch)
{
    HINTERNET h = open_url(url, L"Accept: application/vnd.github+json, text/plain, */*\r\n", status, err, cch);
    char *buf = NULL;
    DWORD n = 0, cap = 0, got;
    if (!h) return NULL;
    if (*status != 200) { InternetCloseHandle(h); seterr(err, cch, L"%ls answered %lu.", url, *status); return NULL; }
    for (;;) {
        if (n + 8192 + 1 > cap) {
            char *t;
            cap = cap ? cap * 2 : 65536;
            if (cap > MAX_MANIFEST || !(t = realloc(buf, cap))) { free(buf); InternetCloseHandle(h); seterr(err, cch, L"%ls is too large.", url); return NULL; }
            buf = t;
        }
        if (!InternetReadFile(h, buf + n, 8192, &got)) { free(buf); InternetCloseHandle(h); seterr(err, cch, L"The download from %ls broke off.", url); return NULL; }
        if (!got) break;
        n += got;
    }
    InternetCloseHandle(h);
    if (!buf) buf = calloc(1, 1);
    if (buf) buf[n] = 0;
    return buf;
}

BOOL pkg_resolve(const WCHAR *id, package_t *p, WCHAR *err, int cch)
{
    WCHAR base[1024], path[512], url[2048];
    char *json, *yaml, ver[64], loc8[LOCALE_NAME_MAX_LENGTH];
    mf_entry root, *list = calloc(MF_MAX_ENTRIES, sizeof(mf_entry)), *best;
    DWORD status;
    int n, k;
    WCHAR locale[LOCALE_NAME_MAX_LENGTH] = L"en-US";
    memset(p, 0, sizeof(*p));
    lstrcpynW(p->id, id, 128);
    if (!list) return FALSE;
    if (!id_path(id, path, 512)) { seterr(err, cch, L"'%ls' is not a package name.", id); free(list); return FALSE; }
    source_url(L"ListUrl", DEFAULT_LIST, base, 1024);
    swprintf(url, 2048, L"%ls%ls", base, path);
    if (!(json = http_get(url, &status, err, cch))) { free(list); return FALSE; }
    k = mf_newest_version(json, ver, sizeof(ver));
    free(json);
    if (!k) { free(list); seterr(err, cch, L"No version of %ls was found.", id); return FALSE; }
    MultiByteToWideChar(CP_UTF8, 0, ver, -1, p->version, 64);
    source_url(L"RawUrl", DEFAULT_RAW, base, 1024);
    swprintf(url, 2048, L"%ls%ls/%ls/%ls.installer.yaml", base, path, p->version, id);
    if (!(yaml = http_get(url, &status, err, cch)) && status == 404) {
        /* an older package: one file for everything */
        swprintf(url, 2048, L"%ls%ls/%ls/%ls.yaml", base, path, p->version, id);
        yaml = http_get(url, &status, err, cch);
    }
    if (!yaml) { free(list); return FALSE; }
    n = mf_parse(yaml, &root, list, MF_MAX_ENTRIES);
    free(yaml);
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    WideCharToMultiByte(CP_UTF8, 0, locale, -1, loc8, sizeof(loc8), NULL, NULL);
#ifdef _WIN64
    k = mf_pick(list, n, loc8, IsUserAnAdmin(), 1);
#else
    k = mf_pick(list, n, loc8, IsUserAnAdmin(), 0);
#endif
    if (k < 0) { free(list); seterr(err, cch, L"%ls %ls has no installer for this PC.", id, p->version); return FALSE; }
    best = &list[k];
    if (!mf_sha256(best->sha, p->sha256)) {
        free(list);
        seterr(err, cch, L"%ls %ls has no SHA-256 to check the download with.", id, p->version);
        return FALSE;
    }
    MultiByteToWideChar(CP_UTF8, 0, best->url, -1, p->url, 2048);
    MultiByteToWideChar(CP_UTF8, 0, best->type, -1, p->type, 32);
    MultiByteToWideChar(CP_UTF8, 0, best->scope, -1, p->scope, 16);
    MultiByteToWideChar(CP_UTF8, 0, best->silent, -1, p->silent, 512);
    MultiByteToWideChar(CP_UTF8, 0, best->arch, -1, p->arch, 16);
    free(list);
    if (!p->type[0]) { seterr(err, cch, L"%ls %ls does not say how to install it.", id, p->version); return FALSE; }
    return TRUE;
}

/* ---- download and check ------------------------------------------------------------------------- */

static void file_name_from_url(const WCHAR *url, const WCHAR *type, WCHAR *out, int cch)
{
    const WCHAR *q = wcschr(url, '?'), *end = q ? q : url + wcslen(url), *s = end;
    WCHAR raw[MAX_PATH] = L"", dec[MAX_PATH];
    DWORD n = MAX_PATH;
    int i, k = 0;
    while (s > url && s[-1] != '/') s--;
    lstrcpynW(raw, s, (int)min((size_t)MAX_PATH, (size_t)(end - s) + 1));
    if (UrlUnescapeW(raw, dec, &n, 0) != S_OK) lstrcpynW(dec, raw, MAX_PATH);
    /* only what a file name may hold */
    for (i = 0; dec[i] && k < cch - 8; i++)
        if (iswalnum(dec[i]) || wcschr(L" ._-()+", dec[i])) out[k++] = dec[i];
    out[k] = 0;
    while (k && (out[k - 1] == '.' || out[k - 1] == ' ')) out[--k] = 0;
    if (!k || out[0] == '.') lstrcpyW(out, L"setup"), k = 5;
    if (!wcschr(out, '.')) lstrcatW(out, !_wcsicmp(type, L"msi") || !_wcsicmp(type, L"wix") ? L".msi" : L".exe");
}

BOOL pkg_download(package_t *p, progress_fn progress, void *ctx, volatile LONG *cancel, WCHAR *err, int cch)
{
    WCHAR dir[MAX_PATH], name[MAX_PATH];
    HINTERNET h;
    HANDLE f;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    BYTE buf[65536], digest[32];
    DWORD status = 0, total = 0, len = sizeof(total), got, w;
    ULONGLONG done = 0;
    BOOL ok = FALSE;
    GetTempPathW(MAX_PATH, dir);
    swprintf(dir + wcslen(dir), MAX_PATH - wcslen(dir), L"StainedGlass-Browser-%08lx", GetTickCount() ^ GetCurrentProcessId());
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        seterr(err, cch, L"Can't make a folder for the download (%ls).", dir);
        return FALSE;
    }
    file_name_from_url(p->url, p->type, name, MAX_PATH);
    swprintf(p->file, MAX_PATH, L"%ls\\%ls", dir, name);
    if (!(h = open_url(p->url, NULL, &status, err, cch))) return FALSE;
    if (status != 200) { InternetCloseHandle(h); seterr(err, cch, L"The download answered %lu.", status); return FALSE; }
    if (!HttpQueryInfoW(h, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &total, &len, NULL)) total = 0;
    f = CreateFileW(p->file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { InternetCloseHandle(h); seterr(err, cch, L"Can't write %ls.", p->file); return FALSE; }
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) || BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0)) {
        seterr(err, cch, L"SHA-256 is not available.");
        goto done;
    }
    for (;;) {
        if (cancel && *cancel) { seterr(err, cch, L"Cancelled."); goto done; }
        if (!InternetReadFile(h, buf, sizeof(buf), &got)) { seterr(err, cch, L"The download broke off. Check your internet connection."); goto done; }
        if (!got) break;
        if (!WriteFile(f, buf, got, &w, NULL) || w != got) { seterr(err, cch, L"Can't write %ls (is the disk full?).", p->file); goto done; }
        BCryptHashData(hash, buf, got, 0);
        done += got;
        if (progress) progress(ctx, STAGE_DOWNLOAD, done, total);
    }
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0)) { seterr(err, cch, L"SHA-256 failed."); goto done; }
#ifdef SG_MUTANT_NOHASH
    memcpy(digest, p->sha256, 32);
#endif
    if (memcmp(digest, p->sha256, 32)) {
        seterr(err, cch, L"The download is not the file its maker published (its SHA-256 does not match), so it was not run.");
        goto done;
    }
    p->size = done;
    ok = TRUE;
done:
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    InternetCloseHandle(h);
    if (!ok) { DeleteFileW(p->file); RemoveDirectoryW(dir); p->file[0] = 0; }
    return ok;
}

void pkg_cleanup(package_t *p)
{
    WCHAR dir[MAX_PATH], *slash;
    if (!p->file[0]) return;
    DeleteFileW(p->file);
    lstrcpynW(dir, p->file, MAX_PATH);
    if ((slash = wcsrchr(dir, '\\'))) { *slash = 0; RemoveDirectoryW(dir); }
    p->file[0] = 0;
}

/* ---- running an installer ----------------------------------------------------------------------- */

static BOOL run_wait(const WCHAR *file, const WCHAR *args, DWORD *code, WCHAR *err, int cch)
{
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    int attempt;
    for (attempt = 0; attempt < 2; attempt++) {
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = attempt ? L"runas" : NULL;
        sei.lpFile = file;
        sei.lpParameters = args;
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) break;
        /* an installer that must run as an administrator: ask (the consent prompt) */
        if (GetLastError() != ERROR_ELEVATION_REQUIRED && GetLastError() != ERROR_ACCESS_DENIED) attempt = 2;
    }
    if (!sei.hProcess) { seterr(err, cch, L"The installer could not be started (error %lu).", GetLastError()); return FALSE; }
    WaitForSingleObject(sei.hProcess, INFINITE);
    GetExitCodeProcess(sei.hProcess, code);
    CloseHandle(sei.hProcess);
    return TRUE;
}

BOOL pkg_install(package_t *p, WCHAR *err, int cch)
{
    WCHAR args[1024], msi[MAX_PATH];
    const WCHAR *t = p->type, *file = p->file;
    DWORD code = 1;
    if (!_wcsicmp(t, L"msi") || !_wcsicmp(t, L"wix")) {
        GetSystemDirectoryW(msi, MAX_PATH);
        lstrcatW(msi, L"\\msiexec.exe");
        swprintf(args, 1024, L"/i \"%ls\" %ls", p->file, p->silent[0] ? p->silent : L"/quiet /norestart");
        file = msi;
    } else if (!_wcsicmp(t, L"nullsoft")) lstrcpynW(args, p->silent[0] ? p->silent : L"/S", 1024);
    else if (!_wcsicmp(t, L"inno")) lstrcpynW(args, p->silent[0] ? p->silent : L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /SP-", 1024);
    else if (!_wcsicmp(t, L"burn")) lstrcpynW(args, p->silent[0] ? p->silent : L"/quiet /norestart", 1024);
    else if (!_wcsicmp(t, L"exe")) lstrcpynW(args, p->silent, 1024);
    else { seterr(err, cch, L"%ls installers (%ls) can't be run here yet.", t, p->id); return FALSE; }
    lstrcpynW(p->command, args, 1024);
    if (!run_wait(file, args, &code, err, cch)) return FALSE;
    p->exit_code = code;
    if (code != 0 && code != 3010 && code != 1641) {
        seterr(err, cch, L"The installer stopped with code %ld.", (long)code);
        return FALSE;
    }
    return TRUE;
}

/* ---- winget, when the user has it ---------------------------------------------------------------- */

BOOL winget_path(WCHAR *out, int cch)
{
    DWORD cb = cch * sizeof(WCHAR);
    if (GetEnvironmentVariableW(L"SG_BROWSER_WINGET", out, cch)) return out[0] && GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
    if (SearchPathW(NULL, L"winget.exe", NULL, cch, out, NULL)) return TRUE;
    if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\winget.exe", NULL,
                      RRF_RT_REG_SZ, NULL, out, &cb) && GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return TRUE;
    cb = cch * sizeof(WCHAR);
    if (!RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\winget.exe", NULL,
                      RRF_RT_REG_SZ, NULL, out, &cb) && GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return TRUE;
    out[0] = 0;
    return FALSE;
}

BOOL winget_install(const WCHAR *winget, package_t *p, WCHAR *err, int cch)
{
    WCHAR cmd[2048];
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    swprintf(p->command, 1024, L"install --id %ls -e --silent --accept-package-agreements --accept-source-agreements --disable-interactivity", p->id);
    swprintf(cmd, 2048, L"\"%ls\" %ls", winget, p->command);
    if (!CreateProcessW(winget, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        seterr(err, cch, L"winget could not be started (error %lu).", GetLastError());
        return FALSE;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    p->exit_code = code;
    if (code) { seterr(err, cch, L"winget stopped with code 0x%08lx.", code); return FALSE; }
    return TRUE;
}
