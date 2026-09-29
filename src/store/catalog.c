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

const WCHAR *tier_name(int tier)
{
    return tier == TIER_WINDOWS ? L"windows" : tier == TIER_OURS ? L"ours" : L"linux";
}

BOOL app_shown_by_default(const app_t *a)
{
#ifdef SG_MUTANT_SHOWLINUX
    (void)a; return TRUE;   /* the mutant leaks the Linux tier into the default view */
#else
    return a->tier != TIER_LINUX;
#endif
}

static int parse_tier(const WCHAR *s, int method)
{
    if (!lstrcmpiW(s, L"windows")) return TIER_WINDOWS;
    if (!lstrcmpiW(s, L"ours")) return TIER_OURS;
    if (!lstrcmpiW(s, L"linux")) return TIER_LINUX;
    /* inferred from the method */
    if (method == SRC_OURS_SETUP || method == SRC_OURS_APT) return TIER_OURS;
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
    } else if (!_wcsnicmp(src, L"ours:setup:", 11)) {
        a->method = SRC_OURS_SETUP;
        lstrcpynW(a->setup_exe, src + 11, ARRAYSIZE(a->setup_exe));
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
        reg_str(item, L"PinVersion", a->pin_version, ARRAYSIZE(a->pin_version));
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

/* Read the DisplayVersion of the first Uninstall entry whose DisplayName holds
 * the app's detect name (HKLM, HKLM\WOW6432Node and HKCU). */
static BOOL find_installed(const WCHAR *needle, WCHAR *version, int cch)
{
    static const struct { HKEY root; const WCHAR *path; } roots[] = {
        { HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_LOCAL_MACHINE, L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
        { HKEY_CURRENT_USER,  L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall" },
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
            if (name[0] && StrStrIW(name, needle)) {
                reg_str(item, L"DisplayVersion", version, cch);
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

/* The path an App Paths name resolves to (e.g. sg-office-setup.exe). */
static BOOL app_path(const WCHAR *exe, WCHAR *out, int cch)
{
    WCHAR key[256];
    DWORD cb = cch * sizeof(WCHAR);
    out[0] = 0;
    swprintf(key, ARRAYSIZE(key), L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%ls", exe);
    if (!RegGetValueW(HKEY_LOCAL_MACHINE, key, NULL, RRF_RT_REG_SZ, NULL, out, &cb) && out[0]) return TRUE;
    cb = cch * sizeof(WCHAR);
    if (!RegGetValueW(HKEY_CURRENT_USER, key, NULL, RRF_RT_REG_SZ, NULL, out, &cb) && out[0]) return TRUE;
    return FALSE;
}

static DWORD run_status(const WCHAR *file, const WCHAR *args)
{
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    DWORD code = (DWORD)-1;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpFile = file;
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return code;
    WaitForSingleObject(sei.hProcess, INFINITE);
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code;
}

void app_detect(app_t *a)
{
    a->installed_version[0] = 0;
    a->state = AST_NOT_INSTALLED;
    if (a->method == SRC_OURS_SETUP) {
        WCHAR exe[MAX_PATH];
        if (app_path(a->setup_exe, exe, MAX_PATH)) {
            DWORD code = run_status(exe, L"/status");
            /* /status: 0 = installed with the current payload; non-zero = not,
             * or an older payload (an update). (DWORD)-1 = could not run. */
            if (code == 0) a->state = AST_INSTALLED;
            else if (code != (DWORD)-1 && find_installed(a->detect_name, a->installed_version, 64))
                a->state = AST_UPDATE;
        }
        return;
    }
    if (find_installed(a->detect_name, a->installed_version, 64))
        a->state = AST_INSTALLED;
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
        /* ours:setup update is decided by app_detect (/status); apt tiers are
         * update-checked by the OS's own package tools, not the store. */
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

static int install_winget(app_t *a, progress_fn progress, void *ctx, volatile LONG *cancel, WCHAR *err, int cch)
{
    package_t p;
    WCHAR winget[MAX_PATH];
    BOOL ok;
    if (!pkg_resolve(a->winget_id, &p, err, cch)) return 1;
    lstrcpynW(a->available_version, p.version, ARRAYSIZE(a->available_version));
    if (winget_path(winget, MAX_PATH))
        return winget_install(winget, &p, err, cch) ? 0 : 1;
    if (!pkg_download(&p, progress, ctx, cancel, err, cch)) return 1;
    ok = pkg_install(&p, err, cch);
    pkg_cleanup(&p);
    return ok ? 0 : 1;
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
    pkg_cleanup(&p);
    return ok ? 0 : 1;
}

static int install_ours_setup(app_t *a, WCHAR *err, int cch)
{
    WCHAR exe[MAX_PATH];
    DWORD code;
    if (!app_path(a->setup_exe, exe, MAX_PATH)) { swprintf(err, cch, L"%ls is not available.", a->setup_exe); return 1; }
    /* the setup program downloads its own pinned installer, self-elevates and
     * installs silently (SG Office's sg-office-setup /install). */
    code = run_status(exe, L"/install /quiet");
    if (code == 0) return 0;
    if (code == (DWORD)-1) { swprintf(err, cch, L"%ls could not be started.", a->setup_exe); return 1; }
    swprintf(err, cch, L"%ls stopped with code %lu.", a->name, code);
    return (int)code;
}

int app_install(app_t *a, progress_fn progress, void *ctx, volatile LONG *cancel, WCHAR *err, int cch)
{
    err[0] = 0;
    switch (a->method) {
    case SRC_WINGET:     return install_winget(a, progress, ctx, cancel, err, cch);
    case SRC_PIN:        return install_pin(a, progress, ctx, cancel, err, cch);
    case SRC_OURS_SETUP: return install_ours_setup(a, err, cch);
    case SRC_OURS_APT:
    case SRC_LINUX_APT:
        /* A Windows program has no pipe to native apt; the sg-session apt
         * bridge is a tracked follow-up (ADR 0017). For now the card explains
         * the app is installed from Settings. */
        lstrcpynW(err, L"Install this from Settings > Apps (it is a system package).", cch);
        return 1;
    default:
        lstrcpynW(err, L"This app's source is not understood.", cch);
        return 1;
    }
}
