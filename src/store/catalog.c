/* SG Store -- the catalogue: reading it, detecting installs, checking for
 * updates and installing.
 *
 * The catalogue is data in HKLM\Software\Stained Glass\Store\Apps\NN (shipped
 * as defaults/85-sg-store.reg, so a mirror or Group Policy can replace it). The
 * download/verify/install engine is "Get a web browser"'s (src/browser/fetch.c),
 * compiled into this program: a winget entry resolves through the winget
 * community repository and enforces its SHA-256; a pinned entry enforces the
 * SHA-256 in the catalogue. Our own suites (SG Office) run their own setup
 * program, which does its own pinned-hash download.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "store.h"
#include <shellapi.h>
#include <shlobj.h>
#include "zipcore.h"

const WCHAR *tier_name(int tier)
{
    return tier == TIER_WINDOWS ? L"windows" : tier == TIER_OURS ? L"ours" : L"linux";
}

/* The section an app is listed in: its category, or -- for a native Linux
 * app -- the Linux apps section, which comes after every other, so the
 * Windows build of a program stays the suggested one. */
const WCHAR *app_section(const app_t *a)
{
#ifdef SG_MUTANT_LINUXMIXED
    return a->category;     /* the mutant files Linux apps among the Windows programs */
#else
    return a->tier == TIER_LINUX ? LINUX_SECTION : a->category;
#endif
}

static int parse_tier(const WCHAR *s, int method)
{
    if (!lstrcmpiW(s, L"windows")) return TIER_WINDOWS;
    if (!lstrcmpiW(s, L"ours")) return TIER_OURS;
    if (!lstrcmpiW(s, L"linux")) return TIER_LINUX;
    /* inferred from the method */
    if (method == SRC_OURS_APT) return TIER_OURS;
    if (method == SRC_LINUX_APT) return TIER_LINUX;
    return TIER_WINDOWS;
}

static BOOL hex32(const WCHAR *hex, BYTE out[32])
{
    int i;
    if (lstrlenW(hex) != 64) return FALSE;
    for (i = 0; i < 32; i++) {
        WCHAR b[3] = { hex[2 * i], hex[2 * i + 1], 0 }, *end;
        out[i] = (BYTE)wcstoul(b, &end, 16);
        if (*end) return FALSE;
    }
    return TRUE;
}

/* "pin:<Url>|<Sha256>|<Type>|<Silent>" -- fill the pin_* fields. */
static void parse_pin(app_t *a, const WCHAR *rest)
{
    WCHAR buf[3072], *p = buf, *bar;
    lstrcpynW(buf, rest, ARRAYSIZE(buf));
    if ((bar = wcschr(p, '|'))) *bar = 0;
    lstrcpynW(a->pin_url, p, ARRAYSIZE(a->pin_url));
    if (!bar) return;
    p = bar + 1;
    if ((bar = wcschr(p, '|'))) *bar = 0;
    a->pin_has_sha = hex32(p, a->pin_sha);
    if (!bar) return;
    p = bar + 1;
    if ((bar = wcschr(p, '|'))) *bar = 0;
    lstrcpynW(a->pin_type, p, ARRAYSIZE(a->pin_type));
    if (!bar) return;
    lstrcpynW(a->pin_silent, bar + 1, ARRAYSIZE(a->pin_silent));
}

static void parse_source(app_t *a, const WCHAR *src)
{
    if (!_wcsnicmp(src, L"winget:", 7)) {
        a->method = SRC_WINGET;
        lstrcpynW(a->winget_id, src + 7, ARRAYSIZE(a->winget_id));
    } else if (!_wcsnicmp(src, L"pin:", 4)) {
        a->method = SRC_PIN;
        parse_pin(a, src + 4);
    } else if (!_wcsnicmp(src, L"ours:apt:", 9)) {
        a->method = SRC_OURS_APT;
        lstrcpynW(a->apt_pkg, src + 9, ARRAYSIZE(a->apt_pkg));
    } else if (!_wcsnicmp(src, L"linux:apt:", 10)) {
        a->method = SRC_LINUX_APT;
        lstrcpynW(a->apt_pkg, src + 10, ARRAYSIZE(a->apt_pkg));
    } else {
        a->method = SRC_UNKNOWN;
    }
}

static void reg_str(HKEY k, const WCHAR *name, WCHAR *out, int cch)
{
    DWORD cb = cch * sizeof(WCHAR);
    out[0] = 0;
    RegGetValueW(k, NULL, name, RRF_RT_REG_SZ, NULL, out, &cb);
}

int catalog_load(app_t *apps, int max)
{
    HKEY root, item;
    WCHAR sub[16], tier[24], src[3072];
    DWORD i, n, colour, cb;
    int count = 0;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, STORE_KEY L"\\Apps", 0, KEY_READ, &root)) return 0;
    for (i = 0; n = ARRAYSIZE(sub), count < max && !RegEnumKeyExW(root, i, sub, &n, NULL, NULL, NULL, NULL); i++) {
        app_t *a = &apps[count];
        if (RegOpenKeyExW(root, sub, 0, KEY_READ, &item)) continue;
        memset(a, 0, sizeof(*a));
        lstrcpynW(a->ord, sub, ARRAYSIZE(a->ord));
        reg_str(item, L"Name", a->name, ARRAYSIZE(a->name));
        reg_str(item, L"Publisher", a->publisher, ARRAYSIZE(a->publisher));
        reg_str(item, L"Description", a->desc, ARRAYSIZE(a->desc));
        reg_str(item, L"Category", a->category, ARRAYSIZE(a->category));
        reg_str(item, L"DetectName", a->detect_name, ARRAYSIZE(a->detect_name));
        reg_str(item, L"Run", a->run, ARRAYSIZE(a->run));
        reg_str(item, L"PinVersion", a->pin_version, ARRAYSIZE(a->pin_version));
        reg_str(item, L"Icon", a->icon_url, ARRAYSIZE(a->icon_url));
        reg_str(item, L"Linux", a->linux_ord, ARRAYSIZE(a->linux_ord));
        reg_str(item, L"Prefer", tier, ARRAYSIZE(tier));
        a->prefer_windows = !lstrcmpiW(tier, L"windows");
        a->alt = -1;
        colour = 0x00808080; cb = sizeof(colour);
        RegGetValueW(item, NULL, L"Colour", RRF_RT_REG_DWORD, NULL, &colour, &cb);
        a->colour = colour;
        reg_str(item, L"Source", src, ARRAYSIZE(src));
        parse_source(a, src);
        reg_str(item, L"Tier", tier, ARRAYSIZE(tier));
        a->tier = parse_tier(tier, a->method);
        if (!a->category[0]) lstrcpyW(a->category, L"Apps");
        if (!a->detect_name[0] && a->name[0]) lstrcpynW(a->detect_name, a->name, ARRAYSIZE(a->detect_name));
        RegCloseKey(item);
        count++;
    }
    RegCloseKey(root);
    return count;
}

/* ---- detecting an installed program ---------------------------------------------------------------- */

/* Does an Uninstall DisplayName name this app? patterns is "A|B|!C": the
 * name starts with A or B -- as a whole word, so "Git" is not "GitHub
 * Desktop" -- and does not start with C ("XnView|!XnView MP"). Case is
 * ignored; '*' stands for any run of characters ("Mozilla Firefox*ESR").
 * A pattern that ends in punctuation ("Mozilla Firefox (") needs no word
 * end after it. */
static BOOL glob_word(const WCHAR *s, const WCHAR *p, int pn, WCHAR last)
{
    if (!pn) return !*s || !iswalnum(last) || !iswalnum(*s);
    if (*p == '*') {
        for (;;) {
            if (glob_word(s, p + 1, pn - 1, last)) return TRUE;
            if (!*s) return FALSE;
            last = *s++;
        }
    }
    if (!*s || towlower(*s) != towlower(*p)) return FALSE;
    return glob_word(s + 1, p + 1, pn - 1, *p);
}

static BOOL starts_word(const WCHAR *name, const WCHAR *pat, int n)
{
    return n > 0 && glob_word(name, pat, n, 0);
}

BOOL name_matches(const WCHAR *display, const WCHAR *patterns)
{
    const WCHAR *p = patterns;
    BOOL hit = FALSE;
    if (!display[0] || !patterns[0]) return FALSE;
#ifdef SG_MUTANT_SUBSTRING
    { WCHAR first[128]; int k = 0;
      while (p[k] && p[k] != '|' && k < 127) { first[k] = p[k]; k++; }
      first[k] = 0;
      return StrStrIW(display, first) != NULL; }   /* the old way: anywhere in the name */
#endif
    while (*p) {
        const WCHAR *end = wcschr(p, '|');
        int n = end ? (int)(end - p) : lstrlenW(p);
        if (*p == '!') { if (starts_word(display, p + 1, n - 1)) return FALSE; }
        else if (starts_word(display, p, n)) hit = TRUE;
        p += n;
        if (*p == '|') p++;
    }
    return hit;
}

/* Read the DisplayVersion (and, when icon is given, the DisplayIcon) of the
 * first Uninstall entry whose DisplayName is the app's (HKLM,
 * HKLM\WOW6432Node, HKCU and HKCU\Software\WOW6432Node -- where Wine files a
 * 32-bit installer's per-user entry: Kdenlive's, BleachBit's). */
static BOOL find_installed_entry(const WCHAR *needle, WCHAR *version, int cch, WCHAR *icon, int icch)
{
    static const struct { HKEY root; const WCHAR *path; } roots[] = {
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER,  L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
#ifndef SG_MUTANT_NOWOWCU
        { HKEY_CURRENT_USER,  L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
#endif
    };
    unsigned r;
    version[0] = 0;
    if (!needle[0]) return FALSE;
    for (r = 0; r < ARRAYSIZE(roots); r++) {
        HKEY key, item;
        DWORD i, n;
        WCHAR sub[256], name[256];
        if (RegOpenKeyExW(roots[r].root, roots[r].path, 0, KEY_READ, &key)) continue;
        for (i = 0; n = ARRAYSIZE(sub), !RegEnumKeyExW(key, i, sub, &n, NULL, NULL, NULL, NULL); i++) {
            if (RegOpenKeyExW(key, sub, 0, KEY_READ, &item)) continue;
            reg_str(item, L"DisplayName", name, ARRAYSIZE(name));
            if (name_matches(name, needle)) {
                reg_str(item, L"DisplayVersion", version, cch);
                if (icon) reg_str(item, L"DisplayIcon", icon, icch);
                RegCloseKey(item);
                RegCloseKey(key);
                return TRUE;
            }
            RegCloseKey(item);
        }
        RegCloseKey(key);
    }
    return FALSE;
}

static BOOL find_installed(const WCHAR *needle, WCHAR *version, int cch)
{
    return find_installed_entry(needle, version, cch, NULL, 0);
}

/* Is a Debian package installed (dpkg's status file: a stanza with
 * "Package: <pkg>" and "Status: install ok installed")? Z: is the root
 * file system; SG_DPKG_STATUS names another file for the gate. */
/* dpkg's status file, read again only when it changes (the window looks at
 * every Linux app's state each time it comes forward). The text is held
 * under g_dpkg_lock: dpkg_text() takes it, dpkg_done() gives it back. */
static CRITICAL_SECTION g_dpkg_lock;
static INIT_ONCE g_dpkg_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK dpkg_init(INIT_ONCE *once, void *param, void **ctx)
{
    (void)once; (void)param; (void)ctx;
    InitializeCriticalSection(&g_dpkg_lock);
    return TRUE;
}

static char *dpkg_text(const WCHAR *path)
{
    static char *cached;
    static FILETIME stamp;
    static WCHAR cached_path[MAX_PATH];
    static DWORD size_seen;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    HANDLE h;
    DWORD size, got = 0;
    InitOnceExecuteOnce(&g_dpkg_once, dpkg_init, NULL, NULL);
    EnterCriticalSection(&g_dpkg_lock);
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) { LeaveCriticalSection(&g_dpkg_lock); return NULL; }
    if (cached && !lstrcmpiW(cached_path, path) && !CompareFileTime(&stamp, &fa.ftLastWriteTime) && size_seen == fa.nFileSizeLow)
        return cached;
    free(cached);
    cached = NULL;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        size = GetFileSize(h, NULL);
        if (size != INVALID_FILE_SIZE && size <= (256u << 20) && (cached = malloc(size + 2))) {
            ReadFile(h, cached + 1, size, &got, NULL);
            cached[0] = '\n';       /* every stanza line now follows a '\n' */
            cached[got + 1] = 0;
            stamp = fa.ftLastWriteTime;
            size_seen = fa.nFileSizeLow;
            lstrcpynW(cached_path, path, MAX_PATH);
        }
        CloseHandle(h);
    }
    if (!cached) LeaveCriticalSection(&g_dpkg_lock);
    return cached;
}

static void dpkg_done(void) { LeaveCriticalSection(&g_dpkg_lock); }

BOOL dpkg_installed(const WCHAR *pkg, WCHAR *version, int cch)
{
    WCHAR path[MAX_PATH] = L"Z:\\var\\lib\\dpkg\\status";
    char want[160], *text, *p;
    BOOL found = FALSE;
    version[0] = 0;
    if (!pkg[0]) return FALSE;
    GetEnvironmentVariableW(L"SG_DPKG_STATUS", path, MAX_PATH);
    if (!(text = dpkg_text(path))) return FALSE;
    snprintf(want, sizeof(want), "\nPackage: %ls\n", pkg);
    for (p = text; (p = strstr(p, want)); p++) {
        char *end = strstr(p + 1, "\n\n"), *st, *ver, save = 0;
        if (end) { save = *end; *end = 0; }
        st = strstr(p, "\nStatus: install ok installed\n");
        if (!st && end) st = strstr(p, "\nStatus: install ok installed");   /* the stanza's last line */
        ver = strstr(p, "\nVersion: ");
        if (st) {
            found = TRUE;
            if (ver) {
                char v[128];
                int k = 0;
                ver += 10;
                while (ver[k] && ver[k] != '\n' && k < 127) { v[k] = ver[k]; k++; }
                v[k] = 0;
                MultiByteToWideChar(CP_UTF8, 0, v, -1, version, cch);
            }
        }
        if (end) *end = save;
        if (found) break;
    }
    dpkg_done();
    return found;
}

void app_detect(app_t *a)
{
    a->installed_version[0] = 0;
    a->state = AST_NOT_INSTALLED;
    if (a->method == SRC_LINUX_APT || a->method == SRC_OURS_APT) {
        /* a system package: dpkg says, never a Windows program's name -- the
         * Windows GIMP installed made "GIMP (Linux)" look installed */
#ifdef SG_MUTANT_LINUXBYNAME
        if (find_installed(a->detect_name, a->installed_version, 64)) a->state = AST_INSTALLED;
#else
        if (dpkg_installed(a->apt_pkg, a->installed_version, 64)) a->state = AST_INSTALLED;
#endif
        return;
    }
    if (find_installed(a->detect_name, a->installed_version, 64))
        a->state = AST_INSTALLED;
}

/* ---- opening an installed program ------------------------------------------------------------------
 *
 * Open starts the program itself, as its Start menu entry would: the program
 * its Uninstall entry shows as its icon (DisplayIcon, "C:\...\app.exe,0") when
 * that is the program and not its uninstaller or setup; else its shortcut in
 * Start (all users' or the person's Programs, a folder deep), named as the
 * app is. FALSE: neither -- the store shows Apps & features instead. */

static BOOL not_the_program(const WCHAR *base)
{
    static const WCHAR *const words[] = { L"unins", L"uninst", L"setup", L"install", L"update", L"maintenance" };
    unsigned i;
    for (i = 0; i < ARRAYSIZE(words); i++) if (StrStrIW(base, words[i])) return TRUE;
    return FALSE;
}

static BOOL icon_program(const WCHAR *icon, WCHAR *out, int cch)
{
    WCHAR path[MAX_PATH], *comma, *base;
    int n;
    lstrcpynW(path, icon[0] == '"' ? icon + 1 : icon, MAX_PATH);
    if ((comma = wcsrchr(path, ',')) && comma > wcsrchr(path, '\\')) *comma = 0;
    if ((n = lstrlenW(path)) && path[n - 1] == '"') path[--n] = 0;
    if (n < 5 || _wcsicmp(path + n - 4, L".exe")) return FALSE;
    base = wcsrchr(path, '\\') ? wcsrchr(path, '\\') + 1 : path;
    if (not_the_program(base) || GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return FALSE;
    lstrcpynW(out, path, cch);
    return TRUE;
}

/* the best-named shortcut under dir (and its folders, depth levels down) */
static void find_shortcut(const app_t *a, const WCHAR *dir, int depth, WCHAR *best, int cch, int *best_len)
{
    WCHAR pat[MAX_PATH], base[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        int n = lstrlenW(fd.cFileName);
        if (fd.cFileName[0] == '.') continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && lstrlenW(dir) + n + 2 < MAX_PATH) {
                swprintf(base, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
                find_shortcut(a, base, depth - 1, best, cch, best_len);
            }
            continue;
        }
        if (n < 5 || _wcsicmp(fd.cFileName + n - 4, L".lnk")) continue;
        lstrcpynW(base, fd.cFileName, n - 3);
        if (not_the_program(base)) continue;
        if (!name_matches(base, a->detect_name) && !name_matches(base, a->name)) continue;
        if (*best_len && n >= *best_len) continue;      /* "OpenOffice" before "OpenOffice Calc" */
        if (lstrlenW(dir) + n + 2 >= cch) continue;
        swprintf(best, cch, L"%ls\\%ls", dir, fd.cFileName);
        *best_len = n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

BOOL app_launch_target(const app_t *a, WCHAR *out, int cch)
{
    static const int folders[] = { CSIDL_COMMON_PROGRAMS, CSIDL_PROGRAMS };
    WCHAR version[64], icon[MAX_PATH], dir[MAX_PATH];
    int best_len = 0;
    unsigned i;
    out[0] = 0;
    if (a->tier == TIER_OURS || a->tier == TIER_LINUX) return FALSE;
    if (find_installed_entry(a->detect_name, version, ARRAYSIZE(version), icon, ARRAYSIZE(icon))
        && icon[0] && icon_program(icon, out, cch)) return TRUE;
    for (i = 0; i < ARRAYSIZE(folders); i++)
        if (SUCCEEDED(SHGetFolderPathW(NULL, folders[i], NULL, 0, dir)))
            find_shortcut(a, dir, 1, out, cch, &best_len);
    return out[0] != 0;
}

/* ---- checking for updates -------------------------------------------------------------------------- */

BOOL app_check_update(app_t *a, WCHAR *err, int cch)
{
    a->available_version[0] = 0;
    if (err) err[0] = 0;
    if (a->method == SRC_WINGET) {
        package_t p;
        if (!pkg_resolve(a->winget_id, &p, err, cch)) return FALSE;
        lstrcpynW(a->available_version, p.version, ARRAYSIZE(a->available_version));
    } else if (a->method == SRC_PIN) {
        lstrcpynW(a->available_version, a->pin_version, ARRAYSIZE(a->available_version));
    } else {
        /* apt tiers are update-checked by the OS's own package tools, not
         * the store. */
        return TRUE;
    }
    if (a->state == AST_INSTALLED && a->installed_version[0] && a->available_version[0]) {
        char have[80], want[80];
        WideCharToMultiByte(CP_UTF8, 0, a->installed_version, -1, have, sizeof(have), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, a->available_version, -1, want, sizeof(want), NULL, NULL);
#ifdef SG_MUTANT_NOUPDATE
        (void)have; (void)want;    /* the mutant never notices a newer version */
#else
        if (mf_version_cmp(want, have) > 0) a->state = AST_UPDATE;
#endif
    }
    return TRUE;
}

/* ---- installing (or updating) ---------------------------------------------------------------------- */

/* A zip whose manifest names the installer inside it (NestedInstallerType:
 * Paint.NET's): the zip was checked against its SHA-256 as downloaded; take
 * out the one installer it holds, of that kind, and run it as that kind. */
static BOOL unpack_nested(package_t *p, WCHAR *err, int cch)
{
    zarchive z;
    int i, pick = -1, r;
    BOOL msi = !_wcsicmp(p->nested, L"msi") || !_wcsicmp(p->nested, L"wix");
    const WCHAR *ext = msi ? L".msi" : L".exe";
    WCHAR out[MAX_PATH], *slash;
    BYTE *data;
    size_t len;
    HANDLE h;
    DWORD put;
    if ((r = zip_open(&z, p->file))) { swprintf(err, cch, L"The download is not a readable ZIP file (%ls).", zip_strerror(r)); return FALSE; }
    for (i = 0; i < z.n; i++) {
        int n = lstrlenW(z.e[i].name);
        if (z.e[i].dir || n < 5 || _wcsicmp(z.e[i].name + n - 4, ext)) continue;
        if (pick >= 0) { zip_close(&z); swprintf(err, cch, L"%ls holds more than one installer.", p->id); return FALSE; }
        pick = i;
    }
    if (pick < 0) { zip_close(&z); swprintf(err, cch, L"%ls holds no installer.", p->id); return FALSE; }
    if ((r = zip_read(&z, pick, &data, &len))) { zip_close(&z); swprintf(err, cch, L"The installer could not be unpacked (%ls).", zip_strerror(r)); return FALSE; }
    zip_close(&z);
    /* beside the download, so pkg_cleanup takes both away */
    lstrcpynW(out, p->file, MAX_PATH);
    if ((slash = wcsrchr(out, L'\\'))) slash[1] = 0; else out[0] = 0;
    if (lstrlenW(out) + 10 >= MAX_PATH) { free(data); lstrcpynW(err, L"The installer could not be unpacked.", cch); return FALSE; }
    lstrcatW(out, L"setup");
    lstrcatW(out, ext);
    h = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (h == INVALID_HANDLE_VALUE || !WriteFile(h, data, (DWORD)len, &put, NULL) || put != len) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        free(data);
        swprintf(err, cch, L"The installer could not be unpacked.");
        return FALSE;
    }
    CloseHandle(h);
    free(data);
    DeleteFileW(p->file);
    lstrcpynW(p->file, out, MAX_PATH);
    lstrcpynW(p->type, p->nested, ARRAYSIZE(p->type));
    return TRUE;
}

/* What the system already provides, so a package's dependency on it is met:
 * the Visual C++ runtimes are Wine's own (msvcp140, vcruntime140...), and
 * installing Microsoft's over them would only replace them. */
static BOOL dep_provided(const WCHAR *id)
{
    return !_wcsnicmp(id, L"Microsoft.VCRedist.", 19);
}

#define DEPS_KEY STORE_KEY L"\\Dependencies"

/* Whether a dependency still has to be installed: not if the system
 * provides it, not if SG Store installed it before (recorded), and not if
 * the catalogue's app of that id is detected as installed. */
static BOOL dep_needed(const WCHAR *id)
{
    static app_t apps[MAX_APPS];
    WCHAR key[300];
    HKEY h;
    int n, i;
    if (dep_provided(id)) return FALSE;
    swprintf(key, ARRAYSIZE(key), L"%ls\\%ls", DEPS_KEY, id);
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h) || !RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &h)) {
        RegCloseKey(h);
        return FALSE;
    }
    n = catalog_load(apps, MAX_APPS);
    for (i = 0; i < n; i++)
        if (apps[i].method == SRC_WINGET && !lstrcmpiW(apps[i].winget_id, id)) {
            app_detect(&apps[i]);
            return apps[i].state != AST_INSTALLED && apps[i].state != AST_UPDATE;
        }
    return TRUE;
}

static void dep_record(const WCHAR *id, const WCHAR *version)
{
    WCHAR key[300];
    HKEY h;
    swprintf(key, ARRAYSIZE(key), L"%ls\\%ls", DEPS_KEY, id);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, key, 0, NULL, 0, KEY_WRITE, NULL, &h, NULL) &&
        RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &h, NULL)) return;
    RegSetValueExW(h, L"Version", 0, REG_SZ, (const BYTE *)version, (lstrlenW(version) + 1) * sizeof(WCHAR));
    RegCloseKey(h);
}

static int install_id(const WCHAR *id, int depth, app_t *a, progress_fn progress, void *ctx, volatile LONG *cancel,
                      WCHAR *err, int cch);

/* A package's dependencies (winget's PackageDependencies) first: TortoiseGit
 * needs Git, and its installer failed without it ("can't find executable"). */
static int install_deps(const package_t *p, int depth, progress_fn progress, void *ctx, volatile LONG *cancel,
                        WCHAR *err, int cch)
{
    WCHAR list[512], *tok, *sp;
#ifdef SG_MUTANT_NODEPINSTALL
    return 0;
#endif
    lstrcpynW(list, p->deps, ARRAYSIZE(list));
    for (tok = list; *tok; tok = sp) {
        WCHAR why[400];
        if (*tok == ' ') { sp = tok + 1; continue; }
        if ((sp = wcschr(tok, ' '))) *sp++ = 0;
        else sp = tok + lstrlenW(tok);
        if (!dep_needed(tok)) continue;
        if (depth >= 2) {   /* deeper chains are not followed */
            swprintf(err, cch, L"%ls needs %ls, which needs still more; install %ls first.", p->id, tok, tok);
            return 1;
        }
        if (install_id(tok, depth + 1, NULL, progress, ctx, cancel, why, ARRAYSIZE(why))) {
            swprintf(err, cch, L"%ls needs %ls, which did not install: %ls", p->id, tok, why);
            return 1;
        }
    }
    return 0;
}

static int install_id(const WCHAR *id, int depth, app_t *a, progress_fn progress, void *ctx, volatile LONG *cancel,
                      WCHAR *err, int cch)
{
    package_t p;
    WCHAR winget[MAX_PATH];
    BOOL ok;
    if (!pkg_resolve(id, &p, err, cch)) return 1;
    if (a) lstrcpynW(a->available_version, p.version, ARRAYSIZE(a->available_version));
    if (winget_path(winget, MAX_PATH))   /* winget installs the dependencies itself */
        return winget_install(winget, &p, err, cch) ? 0 : 1;
    if (install_deps(&p, depth, progress, ctx, cancel, err, cch)) return 1;
    if (!pkg_download(&p, progress, ctx, cancel, err, cch)) return 1;
#ifndef SG_MUTANT_NOZIP
    if (!_wcsicmp(p.type, L"zip") && !unpack_nested(&p, err, cch)) { pkg_cleanup(&p); return 1; }
#endif
    ok = pkg_install(&p, err, cch);
    if (a) a->ran_elevated = p.elevated;
    if (ok && depth) dep_record(id, p.version);
    pkg_cleanup(&p);
    return ok ? 0 : 1;
}

static int install_winget(app_t *a, progress_fn progress, void *ctx, volatile LONG *cancel, WCHAR *err, int cch)
{
    return install_id(a->winget_id, 0, a, progress, ctx, cancel, err, cch);
}

static int install_pin(app_t *a, progress_fn progress, void *ctx, volatile LONG *cancel, WCHAR *err, int cch)
{
    package_t p;
    BOOL ok;
    if (!a->pin_has_sha) { lstrcpynW(err, L"This app has no pinned SHA-256, so it was not installed.", cch); return 1; }
    memset(&p, 0, sizeof(p));
    lstrcpynW(p.id, a->name, ARRAYSIZE(p.id));
    lstrcpynW(p.url, a->pin_url, ARRAYSIZE(p.url));
    lstrcpynW(p.type, a->pin_type[0] ? a->pin_type : L"exe", ARRAYSIZE(p.type));
    lstrcpynW(p.silent, a->pin_silent, ARRAYSIZE(p.silent));
    memcpy(p.sha256, a->pin_sha, 32);
    if (!pkg_download(&p, progress, ctx, cancel, err, cch)) return 1;
    ok = pkg_install(&p, err, cch);
    a->ran_elevated = p.elevated;
    pkg_cleanup(&p);
    return ok ? 0 : 1;
}

int app_install(app_t *a, progress_fn progress, void *ctx, volatile LONG *cancel, WCHAR *err, int cch)
{
    err[0] = 0;
    switch (a->method) {
    case SRC_WINGET:     return install_winget(a, progress, ctx, cancel, err, cch);
    case SRC_PIN:        return install_pin(a, progress, ctx, cancel, err, cch);
    case SRC_OURS_APT:
    case SRC_LINUX_APT:
        /* a system package: apt, as root, through the administrator's
         * consent and sg-admind (sysinstall.c); our own (SG Office's) are
         * registered in the Windows side as they install */
        return sys_install_apt(a, err, cch);
    default:
        lstrcpynW(err, L"This app's source is not understood.", cch);
        return 1;
    }
}

/* ---- removing an app ------------------------------------------------------------------------------
 *
 * A Linux app (or one of ours): apt-get remove, through the administrator's
 * consent and sg-admind (sysinstall.c). A Windows program: what its Uninstall
 * entry says removes it, as Apps & features does -- QuietUninstallString if
 * it has one, else UninstallString, an MSI's "MsiExec.exe /I{...}" (repair)
 * turned into /X{...} (remove). It runs as the person, or as an administrator
 * when the uninstaller asks to be (the consent prompt). An NSIS uninstaller
 * copies itself to %TEMP% and returns at once: the entry is watched until it
 * goes, a few minutes at most. */

static BOOL uninstall_command(const WCHAR *needle, WCHAR *cmd, int cch)
{
    static const struct { HKEY root; const WCHAR *path; } roots[] = {
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER,  L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER,  L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
    };
    unsigned r;
    cmd[0] = 0;
    if (!needle[0]) return FALSE;
    for (r = 0; r < ARRAYSIZE(roots) && !cmd[0]; r++) {
        HKEY key, item;
        DWORD i, n;
        WCHAR sub[256], name[256];
        if (RegOpenKeyExW(roots[r].root, roots[r].path, 0, KEY_READ, &key)) continue;
        for (i = 0; !cmd[0] && (n = ARRAYSIZE(sub), !RegEnumKeyExW(key, i, sub, &n, NULL, NULL, NULL, NULL)); i++) {
            if (RegOpenKeyExW(key, sub, 0, KEY_READ, &item)) continue;
            reg_str(item, L"DisplayName", name, ARRAYSIZE(name));
            if (name_matches(name, needle)) {
#ifndef SG_MUTANT_NOQUIET
                reg_str(item, L"QuietUninstallString", cmd, cch);
#endif
                if (!cmd[0]) reg_str(item, L"UninstallString", cmd, cch);
            }
            RegCloseKey(item);
        }
        RegCloseKey(key);
    }
    if (cmd[0]) {   /* MsiExec /I{GUID} repairs; /X{GUID} removes */
        WCHAR *p = StrStrIW(cmd, L"msiexec");
        if (p && (p = StrStrIW(p, L" /I")) && (p[3] == L'{' || p[3] == L' ')) p[2] = L'X';
    }
    return cmd[0] != 0;
}

/* "C:\Program Files\X\unins000.exe" /S -> the program and its arguments (an
 * unquoted path with spaces runs to its .exe) */
static void split_command(const WCHAR *cmd, WCHAR *file, WCHAR *args, int cch)
{
    const WCHAR *end, *exe;
    while (*cmd == L' ') cmd++;
    if (*cmd == L'"' && (end = wcschr(cmd + 1, L'"'))) {
        lstrcpynW(file, cmd + 1, min(cch, (int)(end - cmd)));
        end++;
    } else if ((exe = StrStrIW(cmd, L".exe")) && (exe[4] == 0 || exe[4] == L' ')) {
        lstrcpynW(file, cmd, min(cch, (int)(exe + 4 - cmd) + 1));
        end = exe + 4;
    } else {
        end = cmd + wcscspn(cmd, L" ");
        lstrcpynW(file, cmd, min(cch, (int)(end - cmd) + 1));
    }
    while (*end == L' ') end++;
    lstrcpynW(args, end, cch);
}

/* a runtime (Source type cab-dll: one DLL into the system folder, the
 * Visual Basic 6 runtime): no program to open, removed as an administrator */
BOOL app_is_runtime(const app_t *a)
{
    return a->method == SRC_PIN && !lstrcmpiW(a->pin_type, L"cab-dll");
}

int app_uninstall(app_t *a, WCHAR *err, int cch)
{
    WCHAR cmd[2048], file[MAX_PATH], args[2048], ver[64];
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    DWORD code = 0, start;
    int attempt;
    err[0] = 0;
    if (a->method == SRC_LINUX_APT || a->method == SRC_OURS_APT) return sys_remove_apt(a, err, cch);
    if (!uninstall_command(a->detect_name, cmd, ARRAYSIZE(cmd))) {
        swprintf(err, cch, L"%ls has no uninstaller. Remove it in Settings > Apps.", a->name);
        return 1;
    }
    split_command(cmd, file, args, MAX_PATH);
    /* a runtime the store put in the system folder: only an administrator
     * can take it out (its uninstaller is cmd, which never asks) */
    for (attempt = app_is_runtime(a) ? 1 : 0; attempt < 2; attempt++) {
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = attempt ? L"runas" : NULL;
        sei.lpFile = file;
        sei.lpParameters = args[0] ? args : NULL;
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) break;
        if (GetLastError() != ERROR_ELEVATION_REQUIRED && GetLastError() != ERROR_ACCESS_DENIED) break;
    }
    if (!sei.hProcess) {
        swprintf(err, cch, L"%ls's uninstaller could not be started (error %lu).", a->name, GetLastError());
        return 1;
    }
    WaitForSingleObject(sei.hProcess, INFINITE);
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    /* its entry goes when it is gone; a self-copying uninstaller is still at it */
    for (start = GetTickCount(); find_installed(a->detect_name, ver, ARRAYSIZE(ver)) && GetTickCount() - start < 180000;)
        Sleep(1000);
    if (find_installed(a->detect_name, ver, ARRAYSIZE(ver))) {
        swprintf(err, cch, code ? L"%ls was not removed (its uninstaller stopped with %lu)." : L"%ls was not removed.", a->name, code);
        return 1;
    }
    return 0;
}
