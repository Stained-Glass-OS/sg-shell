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
#include <setupapi.h>
#include <tlhelp32.h>

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
    /* winget ids: letters, digits, . - _ and + (Notepad++.Notepad++) */
    for (p = id; *p; p++)
#ifdef SG_MUTANT_NOPLUS
        if (!iswalnum(*p) && *p != '.' && *p != '-' && *p != '_') return FALSE;
#else
        if (!iswalnum(*p) && *p != '.' && *p != '-' && *p != '_' && *p != '+') return FALSE;
#endif
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

static HINTERNET open_url_once(const WCHAR *url, const WCHAR *headers, DWORD *status, WCHAR *err, int cch)
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

/* GitHub answers 429 (too many requests) to a busy address -- Python 3.13's manifest in a walk of the
 * whole Store -- and a mirror 503 while it is busy: asked again after the Retry-After it names (else
 * 2, 4 s; never more than 20 s), up to three times in all, as winget does */
static HINTERNET open_url(const WCHAR *url, const WCHAR *headers, DWORD *status, WCHAR *err, int cch)
{
    HINTERNET h;
    int attempt;
    for (attempt = 0;; attempt++) {
        DWORD wait = 0, len = sizeof(wait);
        if (!(h = open_url_once(url, headers, status, err, cch))) return NULL;
#ifndef SG_MUTANT_NO429RETRY
        if ((*status != 429 && *status != 503) || attempt == 2) return h;
#else
        return h;
#endif
        if (!HttpQueryInfoW(h, HTTP_QUERY_RETRY_AFTER | HTTP_QUERY_FLAG_NUMBER, &wait, &len, NULL) || !wait) wait = 2u << attempt;
        if (wait > 20) wait = 20;
        InternetCloseHandle(h);
        Sleep(wait * 1000);
    }
}

static void status_err(WCHAR *err, int cch, const WCHAR *what, DWORD status)
{
    if (status == 429) seterr(err, cch, L"%ls is getting too many requests just now (429). Try again in a few minutes.", what);
    else seterr(err, cch, L"%ls answered %lu.", what, status);
}

/* the whole answer, NUL-terminated; NULL on failure. 404 is reported in *status. */
static char *http_get(const WCHAR *url, DWORD *status, WCHAR *err, int cch)
{
    HINTERNET h = open_url(url, L"Accept: application/vnd.github+json, text/plain, */*\r\n", status, err, cch);
    char *buf = NULL;
    DWORD n = 0, cap = 0, got;
    if (!h) return NULL;
    if (*status != 200) { InternetCloseHandle(h); status_err(err, cch, url, *status); return NULL; }
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
    MultiByteToWideChar(CP_UTF8, 0, best->nested, -1, p->nested, 32);
    MultiByteToWideChar(CP_UTF8, 0, best->nested_file, -1, p->nested_file, MAX_PATH);
    MultiByteToWideChar(CP_UTF8, 0, best->custom, -1, p->custom, 512);
    MultiByteToWideChar(CP_UTF8, 0, best->deps, -1, p->deps, 512);
    p->elevate = !_stricmp(best->elevation, "elevationRequired");
    free(list);
    if (!p->type[0]) { seterr(err, cch, L"%ls %ls does not say how to install it.", id, p->version); return FALSE; }
    return TRUE;
}

/* What a package says about itself, for SG Store's details page: its
 * newest version's locale manifest -- the user's language, else en-US, else
 * an older package's single file -- as text (free it), and that version. */
char *winget_locale_manifest(const WCHAR *id, WCHAR *version, int vcch, WCHAR *err, int cch)
{
    WCHAR base[1024], path[512], url[2048], locale[LOCALE_NAME_MAX_LENGTH] = L"en-US";
    const WCHAR *tries[3];
    char *json, *yaml = NULL, ver[64];
    DWORD status = 0;
    int k;
    if (!id_path(id, path, 512)) { seterr(err, cch, L"'%ls' is not a package name.", id); return NULL; }
    source_url(L"ListUrl", DEFAULT_LIST, base, 1024);
    swprintf(url, 2048, L"%ls%ls", base, path);
    if (!(json = http_get(url, &status, err, cch))) return NULL;
    k = mf_newest_version(json, ver, sizeof(ver));
    free(json);
    if (!k) { seterr(err, cch, L"No version of %ls was found.", id); return NULL; }
    MultiByteToWideChar(CP_UTF8, 0, ver, -1, version, vcch);
    source_url(L"RawUrl", DEFAULT_RAW, base, 1024);
    GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
    tries[0] = locale; tries[1] = L"en-US"; tries[2] = NULL;
    for (k = 0; k < 2 && !yaml; k++) {
        if (k && !lstrcmpiW(tries[0], tries[1])) break;
        swprintf(url, 2048, L"%ls%ls/%ls/%ls.locale.%ls.yaml", base, path, version, id, tries[k]);
        yaml = http_get(url, &status, err, cch);
    }
    if (!yaml) {
        swprintf(url, 2048, L"%ls%ls/%ls/%ls.yaml", base, path, version, id);
        yaml = http_get(url, &status, err, cch);
    }
    if (yaml) err[0] = 0;
    return yaml;
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
    if (status != 200) { InternetCloseHandle(h); status_err(err, cch, L"The download", status); return FALSE; }
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
    if (!p->nohash && memcmp(digest, p->sha256, 32)) {
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

/* Downloads a Store or browser that was ended mid-way left behind (a Store
 * closed during a 164 MB Chrome download kept the part): the folders more than
 * an hour old under %TEMP%, emptied of what can be deleted. A file still open
 * -- a download going on, an installer running from it -- cannot be, and its
 * folder stays. Returns how many folders went. */
int pkg_sweep_downloads(void)
{
    WCHAR tmp[MAX_PATH], pat[MAX_PATH], dir[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd, ff;
    HANDLE find, files;
    FILETIME now;
    ULARGE_INTEGER n, c;
    int gone = 0;
#ifdef SG_MUTANT_NOSWEEP
    return 0;
#endif
    if (!GetTempPathW(MAX_PATH, tmp)) return 0;
    GetSystemTimeAsFileTime(&now);
    n.LowPart = now.dwLowDateTime; n.HighPart = now.dwHighDateTime;
    swprintf(pat, MAX_PATH, L"%lsStainedGlass-Browser-*", tmp);
    if ((find = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        c.LowPart = fd.ftLastWriteTime.dwLowDateTime; c.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        if (n.QuadPart < c.QuadPart + 36000000000ULL) continue;   /* written to within the hour: maybe in use */
        swprintf(dir, MAX_PATH, L"%ls%ls", tmp, fd.cFileName);
        swprintf(pat, MAX_PATH, L"%ls\\*", dir);
        if ((files = FindFirstFileW(pat, &ff)) != INVALID_HANDLE_VALUE) {
            do {
                if (ff.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                swprintf(path, MAX_PATH, L"%ls\\%ls", dir, ff.cFileName);
                DeleteFileW(path);
            } while (FindNextFileW(files, &ff));
            FindClose(files);
        }
        if (RemoveDirectoryW(dir)) gone++;
    } while (FindNextFileW(find, &fd));
    FindClose(find);
    return gone;
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

BOOL (*g_runas_hook)(const WCHAR *file, const WCHAR *args, DWORD *code);

/* the processes the last run_wait's installer started, and theirs, while it
 * ran: an installer that hands its work to another process and exits is still
 * at it (pkg_install gives them to the package) */
static DWORD g_run_tree[64];
static int g_run_ntree;
/* run as an administrator through the consent prompt (runas, or its manifest
 * asking for one), the installer is the elevation broker's child, not ours (its parent id is 0; ours is the relay,
 * rundll32 SgElevate, which ends with it): a process of no parent that
 * appears while it runs is one of its roots. g_run_pre: what ran before. */
static BOOL g_run_orphans;
static DWORD g_run_pre[2048];
static int g_run_npre;

static void run_pre_snapshot(void)
{
    PROCESSENTRY32W pe = { sizeof(pe) };
    HANDLE snap;
    g_run_npre = 0;
    if ((snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)) == INVALID_HANDLE_VALUE) return;
    if (Process32FirstW(snap, &pe)) do
        if (g_run_npre < (int)ARRAYSIZE(g_run_pre)) g_run_pre[g_run_npre++] = pe.th32ProcessID;
    while (Process32NextW(snap, &pe));
    CloseHandle(snap);
}

static BOOL run_orphan_root(const PROCESSENTRY32W *pe)
{
    int i;
#ifdef SG_MUTANT_NOELEVATEDTREE
    return FALSE;
#endif
    if (!g_run_orphans || pe->th32ParentProcessID) return FALSE;
    if (!lstrcmpiW(pe->szExeFile, L"sg-consent64.exe") || !lstrcmpiW(pe->szExeFile, L"sg-store64.exe")) return FALSE;
    for (i = 0; i < g_run_npre; i++) if (g_run_pre[i] == pe->th32ProcessID) return FALSE;
    return TRUE;
}

static FILETIME g_run_since;

/* now, less two seconds of clock granularity: what a process tree started
 * now is born after */
void pkg_tree_start(FILETIME *since)
{
    ULARGE_INTEGER t;
    GetSystemTimeAsFileTime(since);
    t.LowPart = since->dwLowDateTime; t.HighPart = since->dwHighDateTime;
    t.QuadPart -= 2 * 10000000ULL;
    since->dwLowDateTime = t.LowPart; since->dwHighDateTime = t.HighPart;
}

/* A process whose parent id is one of a tree's is that process's child only
 * if it was born after the tree started: ids are reused, and a program
 * running since before (the gate's earlier stand-ins, on qa4 anything long
 * lived) whose parent's id the uninstaller happened to get held SG Store's
 * wait for good. SG_STORE_FAKE_CHILD names a process to take as the tree's
 * child whatever its parent (the gate's). One that cannot be asked is taken. */
BOOL pkg_born_since(DWORD pid, const FILETIME *since)
{
    FILETIME created, x1, x2, x3;
    HANDLE h;
    BOOL ok;
#ifdef SG_MUTANT_PIDREUSE
    return TRUE;
#endif
    if (!(h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid))) return TRUE;
    ok = GetProcessTimes(h, &created, &x1, &x2, &x3);
    CloseHandle(h);
    return !ok || CompareFileTime(&created, since) >= 0;
}

static BOOL fake_child(DWORD pid)
{
    WCHAR v[16];
    return GetEnvironmentVariableW(L"SG_STORE_FAKE_CHILD", v, ARRAYSIZE(v)) && (DWORD)_wtoi(v) == pid;
}

static void run_tree_grow(void)
{
    PROCESSENTRY32W pe = { sizeof(pe) };
    HANDLE snap;
    BOOL grew;
    int i;
    if ((snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)) == INVALID_HANDLE_VALUE) return;
    do {
        grew = FALSE;
        if (Process32FirstW(snap, &pe)) do {
            BOOL known = FALSE, parent = FALSE;
            for (i = 0; i < g_run_ntree; i++) {
                if (g_run_tree[i] == pe.th32ProcessID) known = TRUE;
                if (g_run_tree[i] == pe.th32ParentProcessID) parent = TRUE;
            }
            if (fake_child(pe.th32ProcessID)) parent = TRUE;
            if (parent && !pkg_born_since(pe.th32ProcessID, &g_run_since)) parent = FALSE;
            if (!known && (parent || run_orphan_root(&pe)) && g_run_ntree < (int)ARRAYSIZE(g_run_tree)) { g_run_tree[g_run_ntree++] = pe.th32ProcessID; grew = TRUE; }
        } while (Process32NextW(snap, &pe));
    } while (grew);
    CloseHandle(snap);
}

/* the installer an administrator's helper ran for run_wait (SG Store's
 * batch): its process, for its descendants to be found */
void pkg_note_installer(DWORD pid)
{
    pkg_tree_start(&g_run_since);
    g_run_ntree = 0;
    g_run_tree[g_run_ntree++] = pid;
}

/* the last installer's process tree, for the caller to wait on (SG Store) */
void pkg_take_tree(package_t *p)
{
    if (g_run_ntree) run_tree_grow();   /* what it started at its end too; a parent's id stays its creator's */
    memcpy(p->tree, g_run_tree, g_run_ntree * sizeof(DWORD));
    p->ntree = g_run_ntree;
    p->since = g_run_since;
}

static BOOL run_wait(const WCHAR *file, const WCHAR *args, BOOL admin, DWORD *code, BOOL *elevated, WCHAR *err, int cch)
{
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    int attempt;
    g_run_orphans = FALSE;
    pkg_tree_start(&g_run_since);
    run_pre_snapshot();
    for (attempt = admin ? 1 : 0; attempt < 2; attempt++) {
        if (attempt && g_runas_hook) {   /* the store's batch: its one administrator's helper */
            g_run_ntree = 0;             /* the helper names the installer's process (pkg_note_installer) */
            if (!g_runas_hook(file, args, code)) {
                seterr(err, cch, L"The installer was not run: an administrator did not allow it.");
                return FALSE;
            }
            if (elevated) *elevated = TRUE;
            return TRUE;
        }
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = attempt ? L"runas" : NULL;
        sei.lpFile = file;
        sei.lpParameters = args;
        sei.nShow = SW_SHOWNORMAL;
        /* a program whose manifest asks for an administrator is elevated by
         * the consent prompt without runas too (DYMO Label's setup.exe) */
        if (ShellExecuteExW(&sei)) { if (elevated) *elevated = attempt != 0; g_run_orphans = TRUE; break; }
        /* an installer that must run as an administrator: ask (the consent prompt) */
        if (GetLastError() != ERROR_ELEVATION_REQUIRED && GetLastError() != ERROR_ACCESS_DENIED) attempt = 2;
    }
    if (!sei.hProcess) {
        DWORD e = GetLastError();
#ifndef SG_MUTANT_NOCONSENTMSG
        /* the consent prompt said no, or went unanswered (wine-sg 0981:
         * ShellExecuteEx fails with ERROR_CANCELLED, as on Windows) */
        if (e == ERROR_CANCELLED) seterr(err, cch, L"Nothing was installed: an administrator's consent was not given.");
        else
#endif
        seterr(err, cch, L"The installer could not be started (error %lu).", e);
        return FALSE;
    }
    g_run_ntree = 0;
    g_run_tree[g_run_ntree++] = GetProcessId(sei.hProcess);
    while (WaitForSingleObject(sei.hProcess, 250) == WAIT_TIMEOUT) run_tree_grow();
    GetExitCodeProcess(sei.hProcess, code);
    CloseHandle(sei.hProcess);
    return TRUE;
}

/* An IExpress package of MSIs (SQL Server Compact's runtime: its x86 and
 * x64 MSIs, both needed on a 64-bit system, x86 first). Run quietly it
 * installs neither here; unpacked (/Q /T:dir /C), the MSIs install one after
 * the other as an administrator, under one consent. */
static BOOL install_iexpress_msi(package_t *p, WCHAR *err, int cch)
{
    WCHAR dir[MAX_PATH], args[1024], cmd[2048], sys[MAX_PATH], path[MAX_PATH], *slash;
    WCHAR names[8][MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE find;
    DWORD code = 1;
    int n = 0, i, pass;
    BOOL ok = FALSE;

    lstrcpynW(dir, p->file, MAX_PATH);
    if (!(slash = wcsrchr(dir, '\\'))) return FALSE;
    lstrcpyW(slash, L"\\unpacked");
    CreateDirectoryW(dir, NULL);
    swprintf(args, ARRAYSIZE(args), L"/Q /T:\"%ls\" /C", dir);
    if (!run_wait(p->file, args, FALSE, &code, NULL, err, cch)) return FALSE;
    swprintf(path, MAX_PATH, L"%ls\\*.msi", dir);
    if ((find = FindFirstFileW(path, &fd)) != INVALID_HANDLE_VALUE) {
        do if (n < (int)ARRAYSIZE(names)) lstrcpynW(names[n++], fd.cFileName, MAX_PATH);
        while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    if (code || !n) {
        seterr(err, cch, L"The package could not be unpacked (code %ld).", (long)code);
        goto done;
    }
    /* msiexec's path has no spaces, so cmd keeps the quotes that follow */
    GetSystemDirectoryW(sys, MAX_PATH);
    cmd[0] = 0;
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < n; i++) {
            WCHAR lower[MAX_PATH];
            lstrcpynW(lower, names[i], MAX_PATH);
            _wcslwr(lower);
            if ((wcsstr(lower, L"x86") != NULL) != !pass) continue;
            if (cmd[0]) lstrcatW(cmd, L" && ");
            swprintf(cmd + lstrlenW(cmd), ARRAYSIZE(cmd) - lstrlenW(cmd),
                     L"%ls\\msiexec.exe /i \"%ls\\%ls\" /quiet /norestart", sys, dir, names[i]);
        }
    swprintf(args, ARRAYSIZE(args), L"/c %ls", cmd);
    lstrcpynW(p->command, args, ARRAYSIZE(p->command));
    swprintf(path, MAX_PATH, L"%ls\\cmd.exe", sys);
    if (!run_wait(path, args, TRUE, &code, &p->elevated, err, cch)) goto done;
    p->exit_code = code;
    if (code != 0 && code != 3010 && code != 1641) seterr(err, cch, L"The installer stopped with code %ld.", (long)code);
    else ok = TRUE;
done:
    swprintf(path, MAX_PATH, L"%ls\\*", dir);
    if ((find = FindFirstFileW(path, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            swprintf(path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW(path);
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir);
    return ok;
}

/* One file out of a cabinet a self-extracting package carries (Microsoft's
 * own Windows XP Service Pack 3 holds the Visual Basic 6 runtime, which the
 * programs that need it no longer find anywhere else): the cabinet from its
 * first "MSCF" on is copied out, the file taken from it (and from the
 * one-file cabinet it may be compressed in, as .dl_ files are). */
struct cab_pick { const WCHAR *want; const WCHAR *dir; WCHAR out[MAX_PATH]; BOOL found; };

static UINT CALLBACK cab_cb(void *ctx, UINT note, UINT_PTR p1, UINT_PTR p2)
{
    struct cab_pick *pick = ctx;
    (void)p2;
    if (note == SPFILENOTIFY_FILEINCABINET) {
        FILE_IN_CABINET_INFO_W *info = (FILE_IN_CABINET_INFO_W *)p1;
        const WCHAR *name = info->NameInCabinet, *base = wcsrchr(name, '\\'), *wbase = wcsrchr(pick->want, '\\');
        /* by its path in the cabinet, or by its name alone */
        if (!_wcsicmp(name, pick->want) || !_wcsicmp(base ? base + 1 : name, wbase ? wbase + 1 : pick->want)) {
            swprintf(info->FullTargetName, MAX_PATH, L"%ls\\%ls", pick->dir, base ? base + 1 : name);
            lstrcpynW(pick->out, info->FullTargetName, MAX_PATH);
            pick->found = TRUE;
            return FILEOP_DOIT;
        }
        return FILEOP_SKIP;
    }
    return NO_ERROR;
}

static BOOL cab_take(const WCHAR *cab, const WCHAR *want, const WCHAR *dir, WCHAR *out)
{
    struct cab_pick pick = { want, dir, {0}, FALSE };
    if (!SetupIterateCabinetW(cab, 0, cab_cb, &pick) || !pick.found) return FALSE;
    lstrcpynW(out, pick.out, MAX_PATH);
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

/* the embedded cabinet, from its signature to the end, into a file of its own */
static BOOL cab_slice(const WCHAR *exe, const WCHAR *cab)
{
    static BYTE buf[1 << 20];
    HANDLE in = CreateFileW(exe, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL), out;
    DWORD got, put, i;
    LONGLONG at = -1, pos = 0;
    BOOL ok = FALSE;
    if (in == INVALID_HANDLE_VALUE) return FALSE;
    while (at < 0 && ReadFile(in, buf, sizeof(buf), &got, NULL) && got >= 4) {
        for (i = 0; i + 4 <= got; i++)
            /* "MSCF", then four reserved zero bytes */
            if (buf[i] == 'M' && buf[i + 1] == 'S' && buf[i + 2] == 'C' && buf[i + 3] == 'F' &&
                i + 8 <= got && !buf[i + 4] && !buf[i + 5] && !buf[i + 6] && !buf[i + 7]) { at = pos + i; break; }
        if (at >= 0) break;
        pos += got - 8;
        SetFilePointer(in, (LONG)(pos & 0xffffffff), ((LONG *)&pos) + 1, FILE_BEGIN);
    }
    if (at < 0) { CloseHandle(in); return FALSE; }
    SetFilePointer(in, (LONG)(at & 0xffffffff), ((LONG *)&at) + 1, FILE_BEGIN);
    out = CreateFileW(cab, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (out != INVALID_HANDLE_VALUE) {
        ok = TRUE;
        while (ReadFile(in, buf, sizeof(buf), &got, NULL) && got)
            if (!WriteFile(out, buf, got, &put, NULL) || put != got) { ok = FALSE; break; }
        CloseHandle(out);
    }
    CloseHandle(in);
    return ok;
}

/* "cab-dll": Silent is IN-CABINET|NAME|DISPLAY NAME|VERSION -- the DLL put
 * in the 32-bit system folder and registered, and an Uninstall entry
 * written so Apps & features and the store see it installed, as an
 * administrator under one consent. */
static BOOL install_cab_dll(package_t *p, WCHAR *err, int cch)
{
    WCHAR spec[1024], *f[4] = {0}, *q, dir[MAX_PATH], cab[MAX_PATH], packed[MAX_PATH], dll[MAX_PATH], final[MAX_PATH];
    WCHAR wow[MAX_PATH], sys[MAX_PATH], args[4096], path[MAX_PATH], *slash;
    DWORD code = 1;
    int n = 0;
    BOOL ok = FALSE;

    lstrcpynW(spec, p->silent, ARRAYSIZE(spec));
    for (q = spec; n < 4; n++) { f[n] = q; if (!(q = wcschr(q, '|'))) { n++; break; } *q++ = 0; }
    if (n < 4) { seterr(err, cch, L"%ls: the package's description is incomplete.", p->id); return FALSE; }
    lstrcpynW(dir, p->file, MAX_PATH);
    if (!(slash = wcsrchr(dir, '\\'))) return FALSE;
    lstrcpyW(slash, L"\\unpacked");
    CreateDirectoryW(dir, NULL);
    swprintf(cab, MAX_PATH, L"%ls\\package.cab", dir);
    if (!cab_slice(p->file, cab) || !cab_take(cab, f[0], dir, packed)) {
        seterr(err, cch, L"%ls was not found in the download.", f[0]);
        goto done;
    }
    /* compressed in a one-file cabinet of its own (a .dl_) */
    swprintf(dll, MAX_PATH, L"%ls\\%ls", dir, f[1]);
    if (!cab_take(packed, f[1], dir, final) && !CopyFileW(packed, dll, FALSE)) {
        seterr(err, cch, L"%ls could not be unpacked.", f[1]);
        goto done;
    }
    if (!GetSystemWow64DirectoryW(wow, MAX_PATH)) GetSystemDirectoryW(wow, MAX_PATH);
    GetSystemDirectoryW(sys, MAX_PATH);
    swprintf(args, ARRAYSIZE(args),
             L"/c copy /y \"%ls\" \"%ls\\%ls\" >nul && (\"%ls\\regsvr32.exe\" /s \"%ls\\%ls\" & cd .) && "
             L"reg add \"HKLM\\Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SG-%ls\" /v DisplayName /d \"%ls\" /f >nul && "
             L"reg add \"HKLM\\Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SG-%ls\" /v DisplayVersion /d \"%ls\" /f >nul && "
             L"reg add \"HKLM\\Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SG-%ls\" /v Publisher /d Microsoft /f >nul && "
             L"reg add \"HKLM\\Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SG-%ls\" /v UninstallString /d "
             L"\"cmd.exe /c del /f \\\"%ls\\%ls\\\" & reg delete HKLM\\Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SG-%ls /f\" /f >nul",
             dll, wow, f[1], wow, wow, f[1],
             f[1], f[2], f[1], f[3], f[1], f[1], wow, f[1], f[1]);
    lstrcpynW(p->command, args, ARRAYSIZE(p->command));
    swprintf(path, MAX_PATH, L"%ls\\cmd.exe", sys);
    if (!run_wait(path, args, TRUE, &code, &p->elevated, err, cch)) goto done;
    p->exit_code = code;
    if (code) seterr(err, cch, L"%ls could not be put in place (code %ld).", f[1], (long)code);
    else ok = TRUE;
done:
    {
        WIN32_FIND_DATAW fd;
        HANDLE find;
        swprintf(path, MAX_PATH, L"%ls\\*", dir);
        if ((find = FindFirstFileW(path, &fd)) != INVALID_HANDLE_VALUE) {
            do {
                swprintf(path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW(path);
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        RemoveDirectoryW(dir);
    }
    return ok;
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
#ifndef SG_MUTANT_NOIEXPRESS
    else if (!_wcsicmp(t, L"iexpress-msi")) return install_iexpress_msi(p, err, cch);
#endif
#ifndef SG_MUTANT_NOCABDLL
    else if (!_wcsicmp(t, L"cab-dll")) return install_cab_dll(p, err, cch);
#endif
    else { seterr(err, cch, L"%ls installers (%ls) can't be run here yet.", t, p->id); return FALSE; }
#ifndef SG_MUTANT_NOCUSTOM
    /* winget passes the Custom switches in every mode: Opera's installer
     * (/allusers=0 or 1) cannot choose a folder without them ("The path is
     * invalid"), and stopped with code 103 */
    if (p->custom[0] && lstrlenW(args) + 1 + lstrlenW(p->custom) < 1024) {
        if (args[0]) lstrcatW(args, L" ");
        lstrcatW(args, p->custom);
    }
#endif
#ifndef SG_MUTANT_NOINNOSCOPE
    /* winget tells an Inno Setup installer the scope of the manifest's entry: /ALLUSERS for a machine
     * one, /CURRENTUSER for a user one. Without it RawTherapee's and GIMP's (which can install
     * either way) chose a per-user install, ran as the user, and still wrote to Program Files:
     * "Setup was unable to create the directory", code 4 or 5. A machine install asks for an
     * administrator first, as Inno's own elevation would. */
    if (!_wcsicmp(t, L"inno") && lstrlenW(args) + 14 < 1024) {
        if (!_wcsicmp(p->scope, L"machine")) { lstrcatW(args, L" /ALLUSERS"); p->elevate = TRUE; }
        else if (!_wcsicmp(p->scope, L"user")) lstrcatW(args, L" /CURRENTUSER");
    }
#endif
    lstrcpynW(p->command, args, 1024);
    /* An MSI for all users (Edge's, Chrome's) needs an administrator: on
     * Windows the Installer service elevates it itself, here there is no such
     * service, and as the user msiexec stopped with 1627. Ask first -- and
     * for an installer whose manifest says elevationRequired (foobar2000's,
     * Mp3tag's): run as the user it put its files in Program Files but could
     * not write its Uninstall entry, so it was never seen as installed. */
#ifdef SG_MUTANT_NOELEVATE
    p->elevate = FALSE;
#endif
#ifndef SG_MUTANT_NOINVOKER
    /* An installer for one person (the entry's scope "user": Kdenlive's NSIS,
     * /CurrentUser) runs as that person. Its manifest may still ask for the
     * highest level (NSIS "highest"), and an administrator's elevation here is
     * another account, sgsystem: it installed into sgsystem's profile, out of
     * the person's reach. __COMPAT_LAYER=RunAsInvoker, as on Windows (wine-sg 5958). */
    if (file != msi && !p->elevate && !_wcsicmp(p->scope, L"user")) {
        BOOL ok;
        SetEnvironmentVariableW(L"__COMPAT_LAYER", L"RunAsInvoker");
        ok = run_wait(file, args, FALSE, &code, &p->elevated, err, cch);
        SetEnvironmentVariableW(L"__COMPAT_LAYER", NULL);
        if (!ok) return FALSE;
    } else
#endif
    if (!run_wait(file, args, file == msi || p->elevate, &code, &p->elevated, err, cch)) return FALSE;
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
