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
#include <tlhelp32.h>
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
        reg_str(item, L"Depends", a->depends, ARRAYSIZE(a->depends));
        reg_str(item, L"RegisterDir", a->reg_dir, ARRAYSIZE(a->reg_dir));
        reg_str(item, L"RegisterProgram", a->reg_program, ARRAYSIZE(a->reg_program));
        reg_str(item, L"RegisterUninstall", a->reg_uninstall, ARRAYSIZE(a->reg_uninstall));
        reg_str(item, L"UninstallArgs", a->uninstall_args, ARRAYSIZE(a->uninstall_args));
        reg_str(item, L"InstallArgs", a->install_args, ARRAYSIZE(a->install_args));
        reg_str(item, L"Run", a->run, ARRAYSIZE(a->run));
        reg_str(item, L"DebUrl", a->deb_url, ARRAYSIZE(a->deb_url));
        {
            WCHAR also[8] = L"";
            reg_str(item, L"AlsoInCategory", also, ARRAYSIZE(also));
            a->also_in_category = also[0] == L'1';
        }
        if (_wcsnicmp(a->deb_url, L"https://", 8)) a->deb_url[0] = 0;
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

/* an entry Apps & features does not list: a part of something else
 * (SystemComponent 1, or a ParentKeyName). Python 3.13's bundle installs a
 * dozen MSIs, each with its own hidden entry -- "Python 3.13.9 Executables
 * (64-bit)", "MsiExec.exe /I{...}" -- and the store, taking the first entry
 * named like the app, removed one part and left Python installed. */
static BOOL hidden_entry(HKEY item)
{
    DWORD v = 0, cb = sizeof(v);
#ifdef SG_MUTANT_SHOWPARTS
    return FALSE;
#endif
    if (!RegQueryValueExW(item, L"SystemComponent", NULL, NULL, (BYTE *)&v, &cb) && v) return TRUE;
    return !RegQueryValueExW(item, L"ParentKeyName", NULL, NULL, NULL, NULL);
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
            if (name_matches(name, needle) && !hidden_entry(item)) {
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

/* ---- an installer that writes no Uninstall entry ----------------------------------------------------
 *
 * DOSBox 0.74-3's puts its files and an uninstaller in Program Files and no entry in Apps & features:
 * the store said "done" and never "installed", and offered no Uninstall. Where the catalogue says where
 * such a program goes (RegisterDir, RegisterProgram, RegisterUninstall), the store writes the entry
 * itself, the person's own (HKCU), once the program is there; its uninstaller knows nothing of it, so
 * the store takes it away again once the program is gone. */

static void entry_key(const app_t *a, WCHAR *sub, int cch)
{
    swprintf(sub, cch, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\SgStore-%ls", a->ord);
}

static void set_sz(HKEY key, const WCHAR *name, const WCHAR *value)
{
    RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)value, (lstrlenW(value) + 1) * sizeof(WCHAR));
}

static void register_installed(const app_t *a, const WCHAR *version)
{
    WCHAR dir[MAX_PATH], prog[MAX_PATH], un[256], cmd[MAX_PATH + 300], sub[160], v[64], *args;
    DWORD one = 1;
    HKEY key;
#ifdef SG_MUTANT_NOREGISTER
    return;
#endif
    if (!a->reg_dir[0] || !a->reg_program[0] || find_installed(a->detect_name, v, ARRAYSIZE(v))) return;
    ExpandEnvironmentStringsW(a->reg_dir, dir, MAX_PATH);
    swprintf(prog, MAX_PATH, L"%ls\\%ls", dir, a->reg_program);
    if (GetFileAttributesW(prog) == INVALID_FILE_ATTRIBUTES) return;
    entry_key(a, sub, ARRAYSIZE(sub));
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub, 0, NULL, 0, KEY_WRITE, NULL, &key, NULL)) return;
    set_sz(key, L"DisplayName", a->name);
    if (version && version[0]) set_sz(key, L"DisplayVersion", version);
    set_sz(key, L"Publisher", a->publisher);
    set_sz(key, L"InstallLocation", dir);
    set_sz(key, L"DisplayIcon", prog);
    if (a->reg_uninstall[0]) {
        lstrcpynW(un, a->reg_uninstall, ARRAYSIZE(un));
        if ((args = wcschr(un, L' '))) *args++ = 0;
        swprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\\%ls\"%ls%ls", dir, un, args ? L" " : L"", args ? args : L"");
        set_sz(key, L"UninstallString", cmd);
    }
    RegSetValueExW(key, L"SgStoreEntry", 0, REG_DWORD, (const BYTE *)&one, sizeof(one));
    RegCloseKey(key);
}

static void unregister_gone(const app_t *a)
{
    WCHAR sub[160], prog[MAX_PATH] = L"";
    HKEY key;
    entry_key(a, sub, ARRAYSIZE(sub));
    if (RegOpenKeyExW(HKEY_CURRENT_USER, sub, 0, KEY_READ, &key)) return;
    reg_str(key, L"DisplayIcon", prog, ARRAYSIZE(prog));
    RegCloseKey(key);
    if (prog[0] && GetFileAttributesW(prog) == INVALID_FILE_ATTRIBUTES) RegDeleteKeyW(HKEY_CURRENT_USER, sub);
}

/* Is a Debian package installed (dpkg's status file: a stanza with
 * "Package: <pkg>" and "Status: install ok installed")? Z: is the root
 * file system; SG_DPKG_STATUS names another file for the gate. */
/* dpkg's status file, read again only when it changes (the window looks at
 * every Linux app's state each time it comes forward). The text is held
 * under g_dpkg_lock: dpkg_text() takes it, dpkg_done() gives it back. */
static CRITICAL_SECTION g_dpkg_lock;
static LONG g_dpkg_gen;     /* bumped each time the status file is read again */
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
            g_dpkg_gen++;
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

/* Does an installed part of the system (sg-*, stained-glass*, wine-sg*; not
 * SG Office, which can be removed) need this package -- its sole choice in a
 * Depends or Pre-Depends? apt would take the part away with it, and sg-admind
 * refuses that: the walk 2026-10-21 found the Store offering Uninstall for
 * Thunderbird, which SG Mail depends on. Only direct dependencies are seen.
 * The part's name goes to by. */
static BOOL system_name(const char *n, size_t len)
{
    if (len >= 9 && !strncmp(n, "sg-office", 9) && (len == 9 || n[9] == '-')) return FALSE;
    return (len > 3 && !strncmp(n, "sg-", 3)) || (len >= 13 && !strncmp(n, "stained-glass", 13)) ||
           (len >= 7 && !strncmp(n, "wine-sg", 7));
}

/* is a package installed, by dpkg's whole status text? */
static BOOL status_installed(const char *text, const char *n, size_t len)
{
    const char *p;
    for (p = strstr(text, "\nPackage: "); p; p = strstr(p + 1, "\nPackage: ")) {
        const char *name = p + 10, *end;
        if (strncmp(name, n, len) || name[len] != '\n') continue;
        end = strstr(name, "\n\n");
        p = strstr(name, "\nStatus: install ok installed");
        return p && (!end || p < end);
    }
    return FALSE;
}

/* one Depends line: is pkg its sole choice? An alternative ("firefox-esr |
 * firefox") counts only when none of the others is installed: SG Session
 * needs firefox-esr when firefox is not there, and the Store offered
 * Uninstall for Firefox ESR that sg-admind then refused (qa4 2026-10-25) */
static BOOL field_needs(const char *text, const char *f, const char *end, const char *pkg)
{
    size_t want = strlen(pkg);
    while (f < end) {
        const char *g = f, *ge = memchr(f, ',', end - f);
        BOOL mine = FALSE, other = FALSE;
        if (!ge) ge = end;
        f = ge + 1;
        while (g < ge) {
            const char *n, *ae = memchr(g, '|', ge - g);
            size_t len = 0;
            if (!ae) ae = ge;
            while (g < ae && (*g == ' ' || *g == '\t')) g++;
            for (n = g; n < ae && *n != ' ' && *n != '(' && *n != ':' && *n != '\t'; n++) len++;
            if (len == want && !strncmp(g, pkg, len)) mine = TRUE;
#ifdef SG_MUTANT_ALTSKIP
            else if (len) other = TRUE;
#else
            else if (len && status_installed(text, g, len)) other = TRUE;
#endif
            g = ae + 1;
        }
        if (mine && !other) return TRUE;
    }
    return FALSE;
}

BOOL dpkg_needed_by_system(const WCHAR *pkgw, WCHAR *by, int cch)
{
    WCHAR path[MAX_PATH] = L"Z:\\var\\lib\\dpkg\\status";
    static const char *const fields[] = { "\nDepends: ", "\nPre-Depends: " };
    static struct { char pkg[128]; LONG gen; BOOL found; WCHAR by[128]; } memo[256];   /* under g_dpkg_lock */
    static unsigned memo_next;
    char pkg[128], *text, *p, *next;
    BOOL found = FALSE;
    unsigned i;
    by[0] = 0;
#ifdef SG_MUTANT_NEEDEDBYSYSTEM
    return FALSE;
#endif
    if (!pkgw[0] || !WideCharToMultiByte(CP_UTF8, 0, pkgw, -1, pkg, sizeof(pkg), NULL, NULL)) return FALSE;
    GetEnvironmentVariableW(L"SG_DPKG_STATUS", path, MAX_PATH);
    if (!(text = dpkg_text(path))) return FALSE;
    /* the cards ask on every repaint: the answer is kept until dpkg's file changes */
    for (i = 0; i < ARRAYSIZE(memo) && memo[i].pkg[0]; i++)
        if (memo[i].gen == g_dpkg_gen && !strcmp(memo[i].pkg, pkg)) {
            found = memo[i].found;
            lstrcpynW(by, memo[i].by, cch);
            dpkg_done();
            return found;
        }
    for (p = strstr(text, "\nPackage: "); p && !found; p = next) {
        char *end = strstr(p + 1, "\n\n"), *name = p + 10, save = 0;
        size_t nlen = strcspn(name, "\n");
        unsigned j;
        next = end ? strstr(end, "\nPackage: ") : NULL;
        if (end) { save = end[1]; end[1] = 0; }        /* the stanza, its last line's '\n' kept */
        if (system_name(name, nlen) && strstr(p, "\nStatus: install ok installed\n")) {
            /* the lines, copied: an alternative's check reads the whole text */
            static char lines[ARRAYSIZE(fields)][8192];
            for (j = 0; j < ARRAYSIZE(fields); j++) {
                char *f = strstr(p, fields[j]);
                lines[j][0] = 0;
                if (f) { f += strlen(fields[j]); snprintf(lines[j], sizeof(lines[j]), "%.*s", (int)strcspn(f, "\n"), f); }
            }
            if (end) end[1] = save;
            for (j = 0; j < ARRAYSIZE(fields) && !found; j++)
                found = field_needs(text, lines[j], lines[j] + strlen(lines[j]), pkg);
            if (found) {
                char n[128];
                snprintf(n, sizeof(n), "%.*s", (int)nlen, name);
                MultiByteToWideChar(CP_UTF8, 0, n, -1, by, cch);
            }
            continue;
        }
        if (end) end[1] = save;
    }
    i = memo_next++ % ARRAYSIZE(memo);
    lstrcpynA(memo[i].pkg, pkg, sizeof(memo[i].pkg));
    memo[i].gen = g_dpkg_gen;
    memo[i].found = found;
    lstrcpynW(memo[i].by, by, ARRAYSIZE(memo[i].by));
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
    for (slash = p->nested_file; *slash; slash++) if (*slash == L'/') *slash = L'\\';
    for (i = 0; i < z.n; i++) {
        int n = lstrlenW(z.e[i].name);
        WCHAR name[MAX_PATH], *c;
        if (z.e[i].dir) continue;
        /* the one the manifest names (NestedInstallerFiles), / or \ alike */
        if (p->nested_file[0]) {
            lstrcpynW(name, z.e[i].name, MAX_PATH);
            for (c = name; *c; c++) if (*c == L'/') *c = L'\\';
            if (!_wcsicmp(name, p->nested_file) ||
                (!wcspbrk(p->nested_file, L"\\/") && wcsrchr(name, L'\\') && !_wcsicmp(wcsrchr(name, L'\\') + 1, p->nested_file)))
            { pick = i; break; }
            continue;
        }
        if (n < 5 || _wcsicmp(z.e[i].name + n - 4, ext)) continue;
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
 * provides it; if the catalogue has an app of that id, whether it is
 * detected; otherwise not if SG Store installed it before (recorded). The
 * catalogue's answer comes first: the record outlives an uninstall, and
 * DYMO Connect's setup stopped with code 91 ("you must install .NET") after
 * .NET Desktop Runtime 8, recorded, had been uninstalled. */
static BOOL dep_needed(const WCHAR *id)
{
    static app_t apps[MAX_APPS];
    WCHAR key[300];
    HKEY h;
    int n, i;
    if (dep_provided(id)) return FALSE;
    swprintf(key, ARRAYSIZE(key), L"%ls\\%ls", DEPS_KEY, id);
#ifdef SG_MUTANT_DEPRECORD
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h) || !RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &h)) {
        RegCloseKey(h);
        return FALSE;
    }
#endif
    n = catalog_load(apps, MAX_APPS);
    for (i = 0; i < n; i++)
        if (apps[i].method == SRC_WINGET && !lstrcmpiW(apps[i].winget_id, id)) {
            app_detect(&apps[i]);
            return apps[i].state != AST_INSTALLED && apps[i].state != AST_UPDATE;
        }
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &h) || !RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &h)) {
        RegCloseKey(h);
        return FALSE;
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

static BOOL tree_alive(DWORD *tree, int *n, int max, const FILETIME *since);

/* An installer whose work goes on in a process it started: InstallShield's
 * setup.exe starts a copy of itself from %TEMP% and exits at once, and the
 * store said "Installed." two minutes before DYMO Label 8 had its entry or
 * its shortcuts (qa4 2026-10-22) -- and took its download away. Wait while
 * any of them still runs and the app is not yet installed, half an hour at
 * most (SG_STORE_INSTALL_WAIT, ms: the gate's); one it launches when done
 * (its "Run now") does not hold the store, the app being installed by then. */
static void settle_install(app_t *a, package_t *p)
{
    WCHAR ver[64], v[16];
    DWORD start = GetTickCount(), limit = 30 * 60 * 1000;
#ifdef SG_MUTANT_NOINSTALLTREE
    return;
#endif
    pkg_take_tree(p);
    if (GetEnvironmentVariableW(L"SG_STORE_INSTALL_WAIT", v, ARRAYSIZE(v))) limit = _wtoi(v);
    while (GetTickCount() - start < limit && !find_installed(a->detect_name, ver, ARRAYSIZE(ver)) &&
           tree_alive(p->tree, &p->ntree, ARRAYSIZE(p->tree), &p->since))
        Sleep(1000);
}

static int install_id(const WCHAR *id, int depth, app_t *a, progress_fn progress, void *ctx, volatile LONG *cancel,
                      WCHAR *err, int cch)
{
    package_t p;
    WCHAR winget[MAX_PATH];
    BOOL ok;
    if (!pkg_resolve(id, &p, err, cch)) return 1;
#ifndef SG_MUTANT_NOINSTALLARGS
    if (a && a->install_args[0]) lstrcpynW(p.silent, a->install_args, ARRAYSIZE(p.silent));
#endif
    if (a) lstrcpynW(a->available_version, p.version, ARRAYSIZE(a->available_version));
    if (winget_path(winget, MAX_PATH))   /* winget installs the dependencies itself */
        return winget_install(winget, &p, err, cch) ? 0 : 1;
#ifndef SG_MUTANT_NODEPENDS
    /* and what the catalogue says it needs beside: DYMO Connect's setup skips
     * its .NET 8 prerequisite when silent, and its service then stopped the
     * install waiting on "you must install .NET" (code 91) */
    if (a && a->depends[0] && lstrlenW(p.deps) + 1 + lstrlenW(a->depends) < ARRAYSIZE(p.deps)) {
        if (p.deps[0]) lstrcatW(p.deps, L" ");
        lstrcatW(p.deps, a->depends);
    }
#endif
    if (install_deps(&p, depth, progress, ctx, cancel, err, cch)) return 1;
    if (!pkg_download(&p, progress, ctx, cancel, err, cch)) return 1;
#ifndef SG_MUTANT_NOZIP
    if (!_wcsicmp(p.type, L"zip") && !unpack_nested(&p, err, cch)) { pkg_cleanup(&p); return 1; }
#endif
    ok = pkg_install(&p, err, cch);
    if (ok && a) settle_install(a, &p);
    if (a) a->ran_elevated = p.elevated;
    if (ok && a) register_installed(a, p.version);
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
#ifndef SG_MUTANT_NOPINDEPENDS
    /* what the catalogue says it needs first, as for a winget entry: CCleaner
     * 7's installer only checks for the WebView2 runtime, and ended with
     * 111111 without it */
    lstrcpynW(p.deps, a->depends, ARRAYSIZE(p.deps));
    if (install_deps(&p, 0, progress, ctx, cancel, err, cch)) return 1;
#endif
    if (!pkg_download(&p, progress, ctx, cancel, err, cch)) return 1;
    ok = pkg_install(&p, err, cch);
    if (ok) settle_install(a, &p);
    a->ran_elevated = p.elevated;
    if (ok) register_installed(a, a->pin_version);
    pkg_cleanup(&p);
    return ok ? 0 : 1;
}

/* a Windows install that failed on a nearly full system drive says so: Edge's
 * setup needs over 2 GB for a 170 MB download, crashed when it could not
 * unpack, and the person was told only "The installer stopped with code 91"
 * (qa5 2026-10-22, 1.3 GB free). SG_STORE_FREE_MB: the gate's free space. */
#define NEARLY_FULL_MB 2048
static void disk_hint(WCHAR *err, int cch)
{
    WCHAR dir[MAX_PATH], v[32];
    ULARGE_INTEGER avail;
    ULONGLONG mb;
    int n;
#ifdef SG_MUTANT_NODISKHINT
    return;
#endif
    if (GetEnvironmentVariableW(L"SG_STORE_FREE_MB", v, ARRAYSIZE(v))) mb = _wtoi(v);
    else {
        if (!GetWindowsDirectoryW(dir, ARRAYSIZE(dir)) || lstrlenW(dir) < 3) return;
        dir[3] = 0;                                   /* "C:\" */
        if (!GetDiskFreeSpaceExW(dir, &avail, NULL, NULL)) return;
        mb = avail.QuadPart >> 20;
    }
    if (mb >= NEARLY_FULL_MB) return;
    n = lstrlenW(err);
    swprintf(err + n, cch - n, L"%lsThe system drive is nearly full (%.1f GB free): free some space and try again.",
             n ? L" " : L"", mb / 1024.0);
}

int app_install(app_t *a, progress_fn progress, void *ctx, volatile LONG *cancel, WCHAR *err, int cch)
{
    int rc;
    err[0] = 0;
    switch (a->method) {
    case SRC_WINGET:
    case SRC_PIN:
        rc = a->method == SRC_WINGET ? install_winget(a, progress, ctx, cancel, err, cch)
                                     : install_pin(a, progress, ctx, cancel, err, cch);
        if (rc && !(cancel && *cancel)) disk_hint(err, cch);
        return rc;
    case SRC_OURS_APT:
    case SRC_LINUX_APT:
        /* a system package: apt, as root, through the administrator's
         * consent and sg-admind (sysinstall.c); our own (SG Office's) are
         * registered in the Windows side as they install */
        if (a->deb_url[0]) return sys_install_deb_url(a, progress, ctx, cancel, err, cch);
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

static void split_command(const WCHAR *cmd, WCHAR *file, WCHAR *args, int cch);

/* adds SW to the uninstall command CMD -- before NSIS's "_?=DIR", which must
 * come last: NSIS reads the rest of the line as the folder. foobar2000's
 * entry is "uninstall.exe _?=C:\Program Files\foobar2000"; with /S after
 * it the uninstaller looked in "...\foobar2000 /S", took the entry off the
 * list and removed no file, and its elevated copy was not silent. */
static void add_switch(WCHAR *cmd, int cch, const WCHAR *sw)
{
    WCHAR *tail = NULL, *w = cmd;
#ifndef SG_MUTANT_NSISTAIL
    for (; (w = wcsstr(w, L" _?=")); w++) tail = w;
#endif
    if (!tail) {
        lstrcatW(cmd, L" ");
        lstrcatW(cmd, sw);
        return;
    }
    WCHAR rest[2048];
    lstrcpynW(rest, tail, sizeof(rest) / sizeof(rest[0]));
    *tail = 0;
    lstrcatW(cmd, L" ");
    lstrcatW(cmd, sw);
    if (lstrlenW(cmd) + lstrlenW(rest) < cch) lstrcatW(cmd, rest);
}

/* whether FILE is an NSIS installer or uninstaller: its data carries NSIS's
 * header, "NullsoftInst" (after the program, a few hundred KB in) */
static BOOL is_nsis(const WCHAR *file)
{
    static const char sig[] = "NullsoftInst";
    HANDLE h = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    char *buf;
    DWORD got = 0, i;
    BOOL found = FALSE;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    if ((buf = HeapAlloc(GetProcessHeap(), 0, 4 << 20))) {
        if (ReadFile(h, buf, 4 << 20, &got, NULL) && got >= sizeof(sig))
            for (i = 0; i + sizeof(sig) - 1 <= got && !found; i++)
                found = buf[i] == 'N' && !memcmp(buf + i, sig, sizeof(sig) - 1);
        HeapFree(GetProcessHeap(), 0, buf);
    }
    CloseHandle(h);
    return found;
}

/* where (root and key) the entry it found lives, when root is given */
static BOOL uninstall_command(const WCHAR *needle, const WCHAR *quiet_args, WCHAR *cmd, int cch,
                              HKEY *root, WCHAR *entry, int ecch)
{
    BOOL quiet = FALSE;
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
            if (name_matches(name, needle) && !hidden_entry(item)) {
                if (root) { *root = roots[r].root; swprintf(entry, ecch, L"%ls\\%ls", roots[r].path, sub); }
#ifndef SG_MUTANT_NOQUIET
                reg_str(item, L"QuietUninstallString", cmd, cch);
#endif
                quiet = cmd[0] != 0;
                if (!cmd[0]) reg_str(item, L"UninstallString", cmd, cch);
            }
            RegCloseKey(item);
        }
        RegCloseKey(key);
    }
    if (cmd[0]) {   /* MsiExec /I{GUID} repairs; /X{GUID} removes */
        WCHAR *p = StrStrIW(cmd, L"msiexec");
        if (p && (p = StrStrIW(p, L" /I")) && (p[3] == L'{' || p[3] == L' ')) p[2] = L'X';
        /* and quietly, as winget removes an MSI: DYMO Connect's (an
         * InstallShield-built MSI) ended in 4 s with its full UI and removed
         * nothing; /qn takes it out. A UI level the entry gives is kept. */
#ifndef SG_MUTANT_NOMSIQUIET
        if (!quiet && StrStrIW(cmd, L"msiexec") && StrStrIW(cmd, L" /X") && !StrStrIW(cmd, L" /q") &&
            !StrStrIW(cmd, L" /passive") && lstrlenW(cmd) + 5 < cch) {
            lstrcatW(cmd, L" /qn");
            quiet = TRUE;
        }
#endif
    }
    /* an NSIS uninstaller with no quiet command of its own (Scribus 1.6.6):
     * its window and its "All files ... will be deleted" question waited for
     * a click, and the store said "not removed". /S is NSIS's silent switch,
     * as winget gives it. */
    /* the catalogue's own quiet switch for an uninstaller that has none in
     * its entry and is not NSIS's: Total Commander's tcunin64.exe asked
     * Uninstall/Repair/Cancel and waited; /7 removes it without a word */
#ifndef SG_MUTANT_NOUNINSTARGS
    if (cmd[0] && !quiet && quiet_args && quiet_args[0] && lstrlenW(cmd) + lstrlenW(quiet_args) + 2 < cch) {
        add_switch(cmd, cch, quiet_args);
        quiet = TRUE;
    }
#endif
    if (cmd[0] && !quiet) {
        WCHAR file[MAX_PATH], args[MAX_PATH];
        split_command(cmd, file, args, MAX_PATH);
        /* a Chromium-family uninstaller ("setup.exe --uninstall": Vivaldi,
         * Brave, Chromium): its "Are you sure you want to uninstall?" waited
         * for a click and the store said "not removed"; its own
         * --force-uninstall removes it without asking */
#ifndef SG_MUTANT_NOCHROMIUMFORCE
        if (StrStrIW(args, L"--uninstall") && !StrStrIW(args, L"--force-uninstall") &&
            lstrlenW(cmd) + 20 < cch) {
            lstrcatW(cmd, L" --force-uninstall");
            quiet = TRUE;
        }
#endif
    }
    if (cmd[0] && !quiet) {
        WCHAR file[MAX_PATH], args[MAX_PATH];
        split_command(cmd, file, args, MAX_PATH);
#ifndef SG_MUTANT_NONSISSILENT
        const WCHAR *w = args;
        BOOL has_s = FALSE;
        for (; (w = wcsstr(w, L"/S")); w += 2)   /* the word /S, NSIS's: case counts */
            if ((w == args || w[-1] == L' ') && (!w[2] || w[2] == L' ')) has_s = TRUE;
        if (!has_s && is_nsis(file) && lstrlenW(cmd) + 4 < cch) add_switch(cmd, cch, L"/S");
#endif
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

/* is anything the uninstaller started still running? Opera's (and other
 * launchers') uninstaller exits at once and leaves its window to a child: the
 * store gave up after three minutes and said "was not removed" while the
 * person was still answering it. tree[] holds the process and every
 * descendant seen so far. */
static BOOL tree_alive(DWORD *tree, int *n, int max, const FILETIME *since)
{
    PROCESSENTRY32W pe = { sizeof(pe) };
    BOOL alive = FALSE, grew;
    HANDLE snap;
    int i;
#ifdef SG_MUTANT_NOTREE
    return FALSE;
#endif
    if ((snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)) == INVALID_HANDLE_VALUE) return FALSE;
    do {    /* children first seen now, and theirs */
        grew = FALSE;
        if (Process32FirstW(snap, &pe)) do {
            BOOL known = FALSE, parent = FALSE;
            for (i = 0; i < *n; i++) {
                if (tree[i] == pe.th32ProcessID) known = TRUE;
                if (tree[i] == pe.th32ParentProcessID) parent = TRUE;
            }
            if (!known && !parent && GetEnvironmentVariableW(L"SG_STORE_FAKE_CHILD", NULL, 0)) {
                WCHAR v[16];
                GetEnvironmentVariableW(L"SG_STORE_FAKE_CHILD", v, ARRAYSIZE(v));
                parent = (DWORD)_wtoi(v) == pe.th32ProcessID;
            }
            /* born before the tree started: another program, whose parent's id was reused (pkg_born_since) */
            if (!known && parent && *n < max && pkg_born_since(pe.th32ProcessID, since)) { tree[(*n)++] = pe.th32ProcessID; grew = TRUE; }
        } while (Process32NextW(snap, &pe));
    } while (grew);
    if (Process32FirstW(snap, &pe)) do {
        for (i = 1; i < *n; i++) if (tree[i] == pe.th32ProcessID) alive = TRUE;
    } while (!alive && Process32NextW(snap, &pe));
    CloseHandle(snap);
    return alive;
}

/* the entry's uninstaller is not there at all (Scribus 1.6.6 on qa5: an
 * earlier uninstall stopped half way and took uninst.exe with it): the store
 * said "could not be started (error 2)" and the app stayed installed for
 * good. As Programs and Features does, the person is asked whether to take
 * it off the list (app_forget). */
static BOOL uninstaller_gone(const WCHAR *file)
{
    DWORD e = GetLastError();
#ifdef SG_MUTANT_NOFORGET
    return FALSE;
#endif
    return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) && wcschr(file, L'\\') &&
           GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES;
}

/* take an app whose uninstaller is gone off the list: its Uninstall entry
 * goes (the person's own at once; the machine's as an administrator, by
 * reg.exe). Nothing else is touched. */
int app_forget(app_t *a, WCHAR *err, int cch)
{
    WCHAR cmd[2048], file[MAX_PATH], args[2048], entry[512], params[600];
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    HKEY root = NULL;
    LSTATUS st;
    err[0] = 0;
    if (!uninstall_command(a->detect_name, a->uninstall_args, cmd, ARRAYSIZE(cmd), &root, entry, ARRAYSIZE(entry)) || !root) {
        lstrcpynW(err, L"It is not on the list.", cch);
        return 1;
    }
    split_command(cmd, file, args, MAX_PATH);
    if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES) {   /* its uninstaller is there after all */
        lstrcpynW(err, L"Its uninstaller is on this computer: uninstall it instead.", cch);
        return 1;
    }
    st = RegDeleteTreeW(root, entry);
    if (st == ERROR_SUCCESS) RegDeleteKeyW(root, entry);
    if (st != ERROR_SUCCESS && root == HKEY_LOCAL_MACHINE) {
        DWORD code = 1;
        swprintf(params, ARRAYSIZE(params), L"delete \"HKLM\\%ls\" /f /reg:64", entry);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"runas";
        sei.lpFile = L"reg.exe";
        sei.lpParameters = params;
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei) || !sei.hProcess) {
            lstrcpynW(err, GetLastError() == ERROR_CANCELLED ? L"It was not taken off the list: an administrator's consent was not given."
                                                             : L"It could not be taken off the list.", cch);
            return 1;
        }
        WaitForSingleObject(sei.hProcess, 60000);
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
    }
    app_detect(a);
    if (find_installed(a->detect_name, cmd, ARRAYSIZE(cmd))) {
        lstrcpynW(err, L"It could not be taken off the list.", cch);
        return 1;
    }
    return 0;
}

/* ---- the app is still running ------------------------------------------------------------------
 *
 * Dropbox, running in the person's session when its uninstaller (as an
 * administrator) ran: its "/KillEveryone" did not close it, its files were in
 * use, and 510 MB stayed in Program Files (x86)\Dropbox until a restart --
 * the store said "Uninstalled". Before an uninstaller runs, the store closes
 * the person's own programs that run from the app's folder: asked first
 * (WM_CLOSE), ended after five seconds. The folder is the entry's
 * InstallLocation, else its DisplayIcon's or its uninstaller's; never a
 * system or shared folder, nor one holding them. */

/* a folder of the app's own: not the system's, not a shared one (Program
 * Files, Common Files, a profile, AppData) nor one of their parents */
static BOOL own_folder(const WCHAR *dir)
{
    static const KNOWNFOLDERID *const shared[] = {
        &FOLDERID_Windows, &FOLDERID_ProgramFiles, &FOLDERID_ProgramFilesX86, &FOLDERID_ProgramFilesCommon,
        &FOLDERID_ProgramFilesCommonX86, &FOLDERID_ProgramData, &FOLDERID_Profile, &FOLDERID_LocalAppData,
        &FOLDERID_RoamingAppData, &FOLDERID_UserProgramFiles, &FOLDERID_Desktop, &FOLDERID_Documents,
        &FOLDERID_Downloads, &FOLDERID_UserProfiles, &FOLDERID_Public,
    };
    const WCHAR *last;
    unsigned i;
    int n = lstrlenW(dir), depth = 0;
    if (n < 4 || dir[1] != L':' || dir[2] != L'\\') return FALSE;
    for (i = 3; i < (unsigned)n; i++) if (dir[i] == L'\\' && dir[i - 1] != L'\\') depth++;
    if (depth < 1) return FALSE;                     /* C:\X: too wide */
    last = wcsrchr(dir, L'\\') + 1;
    if (!lstrcmpiW(last, L"Microsoft") || !lstrcmpiW(last, L"Common Files")) return FALSE;
    for (i = 0; i < ARRAYSIZE(shared); i++) {
        WCHAR *k = NULL;
        int kn;
        if (FAILED(SHGetKnownFolderPath(shared[i], 0, NULL, &k)) || !k) continue;
        kn = lstrlenW(k);
        /* it is that folder, holds it, or (the system's) lies in it */
        if ((kn >= n && !_wcsnicmp(k, dir, n) && (k[n] == 0 || k[n] == L'\\')) ||
            (shared[i] == &FOLDERID_Windows && !_wcsnicmp(dir, k, kn) && (dir[kn] == 0 || dir[kn] == L'\\'))) {
            CoTaskMemFree(k);
            return FALSE;
        }
        CoTaskMemFree(k);
    }
    return TRUE;
}

static BOOL CALLBACK close_one(HWND w, LPARAM lp)
{
    const DWORD *pids = (const DWORD *)lp;
    DWORD pid = 0;
    int i;
    GetWindowThreadProcessId(w, &pid);
    for (i = 1; i <= (int)pids[0]; i++)
        if (pids[i] == pid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) PostMessageW(w, WM_CLOSE, 0, 0);
    return TRUE;
}

/* close what runs from dir (the person's own: what the store may open) */
static void close_running(const WCHAR *dir)
{
    PROCESSENTRY32W pe = { sizeof(pe) };
    HANDLE snap, procs[64];
    DWORD pids[65], self = GetCurrentProcessId(), start;
    int n = 0, i, dn = lstrlenW(dir);
#ifdef SG_MUTANT_NOCLOSEAPP
    return;
#endif
    if (!own_folder(dir)) return;
    if ((snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)) == INVALID_HANDLE_VALUE) return;
    if (Process32FirstW(snap, &pe)) do {
        WCHAR img[MAX_PATH];
        DWORD cch = MAX_PATH;
        HANDLE h;
        if (pe.th32ProcessID == self || n >= (int)ARRAYSIZE(procs)) continue;
        if (!(h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID))) continue;
        if (QueryFullProcessImageNameW(h, 0, img, &cch) && cch > (DWORD)dn && !_wcsnicmp(img, dir, dn) && img[dn] == L'\\') {
            procs[n] = h;
            pids[++n] = pe.th32ProcessID;
        } else CloseHandle(h);
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
    if (!n) return;
    pids[0] = n;
    EnumWindows(close_one, (LPARAM)pids);
    for (start = GetTickCount(), i = 0; i < n; i++) {
        DWORD left = GetTickCount() - start < 5000 ? 5000 - (GetTickCount() - start) : 0;
        if (WaitForSingleObject(procs[i], left) == WAIT_TIMEOUT) TerminateProcess(procs[i], 1);
    }
    for (i = 0; i < n; i++) { WaitForSingleObject(procs[i], 3000); CloseHandle(procs[i]); }
}

/* the app's folder, from its entry (root\entry): InstallLocation, else the
 * folder of DisplayIcon, else of its uninstaller (file) */
static void app_folder(HKEY root, const WCHAR *entry, const WCHAR *file, WCHAR *dir, int cch)
{
    WCHAR v[MAX_PATH], *p;
    HKEY key;
    dir[0] = 0;
    if (!RegOpenKeyExW(root, entry, 0, KEY_READ, &key)) {
        DWORD cb = sizeof(v);
        if (!RegGetValueW(key, NULL, L"InstallLocation", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, v, &cb) && v[0])
            lstrcpynW(dir, v, cch);
        cb = sizeof(v);
        if (!dir[0] && !RegGetValueW(key, NULL, L"DisplayIcon", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, v, &cb) && v[0]) {
            if ((p = wcsrchr(v, L','))) *p = 0;                    /* "x.exe,0" */
            p = v + (v[0] == L'"');
            if (p[lstrlenW(p) - 1] == L'"') p[lstrlenW(p) - 1] = 0;
            lstrcpynW(dir, p, cch);
            if ((p = wcsrchr(dir, L'\\'))) *p = 0;
        }
        RegCloseKey(key);
    }
    if (!dir[0] && file[0]) {
        lstrcpynW(dir, file, cch);
        if ((p = wcsrchr(dir, L'\\'))) *p = 0; else dir[0] = 0;
    }
    while ((p = dir + lstrlenW(dir) - 1) >= dir + 3 && *p == L'\\') *p = 0;   /* "C:\X\" */
}

/* still listed, under another part's uninstaller (not cmd's)? DYMO Connect
 * 1.6: an NSIS wrapper's entry (Uninstall.exe) and its MSI's, both "DYMO
 * Connect"; the wrapper's uninstaller took itself away and left the MSI, and
 * the store said "not removed" after three minutes. */
static BOOL another_part(const app_t *a, const WCHAR *cmd)
{
    WCHAR now[2048];
#ifdef SG_MUTANT_NOPARTS
    return FALSE;
#endif
    return uninstall_command(a->detect_name, a->uninstall_args, now, ARRAYSIZE(now), NULL, NULL, 0) && lstrcmpiW(now, cmd);
}

#define UNINSTALL_NEXT 4   /* uninstall_once: removed one part; another is left */

/* An uninstaller of the machine's (its entry in HKLM) that the person cannot
 * change: run as an administrator from the start. NVDA's (NSIS, asInvoker)
 * never asks; run as the person it copied itself to %TEMP%, could delete
 * nothing in Program Files, ended with 0, and the store said "NVDA was not
 * removed" three minutes later. On Windows an administrator's uninstall from
 * Settings would ask; one the person can change (a per-user folder) runs as
 * them, as before. */
static BOOL uninstaller_needs_admin(HKEY root, const WCHAR *file)
{
    HANDLE h;
#ifdef SG_MUTANT_NOMACHINEELEVATE
    return FALSE;
#endif
    if (root != HKEY_LOCAL_MACHINE) return FALSE;
    h = CreateFileW(file, FILE_WRITE_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return FALSE; }
    return GetLastError() == ERROR_ACCESS_DENIED;
}

/* one uninstaller (the first entry named like the app): ran is the
 * command it ran */
static int uninstall_once(app_t *a, WCHAR *err, int cch, WCHAR *ran, int rcch)
{
    DWORD tree[64], grace = 180000;
    FILETIME since;
    int ntree = 0;
    WCHAR cmd[2048], file[MAX_PATH], args[2048], ver[64], entry[512], dir[MAX_PATH];
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    HKEY root = NULL;
    DWORD code = 0, start;
    int attempt;
    err[0] = 0;
    if (!uninstall_command(a->detect_name, a->uninstall_args, cmd, ARRAYSIZE(cmd), &root, entry, ARRAYSIZE(entry))) {
        swprintf(err, cch, L"%ls has no uninstaller. Remove it in Settings > Apps.", a->name);
        return 1;
    }
    lstrcpynW(ran, cmd, rcch);
    split_command(cmd, file, args, MAX_PATH);
    if (root) {
        app_folder(root, entry, file, dir, ARRAYSIZE(dir));
        if (dir[0]) close_running(dir);
    }
    /* a runtime the store put in the system folder: only an administrator
     * can take it out (its uninstaller is cmd, which never asks) */
    pkg_tree_start(&since);
#ifndef SG_MUTANT_NOUNINVOKER
    /* the person's own entry (HKCU): its uninstaller runs as them, whatever
     * its manifest asks (wine-sg 5958). Kdenlive's NSIS uninstaller asks for
     * the highest level; elevated it was sgsystem, found nothing of bob's and
     * the store said "Kdenlive was not removed". */
    if (root == HKEY_CURRENT_USER) SetEnvironmentVariableW(L"__COMPAT_LAYER", L"RunAsInvoker");
#endif
    for (attempt = app_is_runtime(a) || uninstaller_needs_admin(root, file) ? 1 : 0; attempt < 2; attempt++) {
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = attempt ? L"runas" : NULL;
        sei.lpFile = file;
        sei.lpParameters = args[0] ? args : NULL;
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei)) break;
        if (GetLastError() != ERROR_ELEVATION_REQUIRED && GetLastError() != ERROR_ACCESS_DENIED) break;
    }
    SetEnvironmentVariableW(L"__COMPAT_LAYER", NULL);
    if (!sei.hProcess) {
        if (GetLastError() == ERROR_CANCELLED)   /* the consent prompt said no, or went unanswered */
            swprintf(err, cch, L"%ls was not removed: an administrator's consent was not given.", a->name);
        else if (uninstaller_gone(file)) {
            swprintf(err, cch, L"%ls's uninstaller is not on this computer: it may have been uninstalled already.", a->name);
            return UNINSTALL_GONE;
        }
        else swprintf(err, cch, L"%ls's uninstaller could not be started (error %lu).", a->name, GetLastError());
        return 1;
    }
    a->ran_elevated = attempt != 0;
    tree[ntree++] = GetProcessId(sei.hProcess);
    while (WaitForSingleObject(sei.hProcess, 500) == WAIT_TIMEOUT) tree_alive(tree, &ntree, ARRAYSIZE(tree), &since);
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    /* its entry goes when it is gone; a self-copying uninstaller, or one whose
     * window is a child's, is still at it. SG_STORE_UNINSTALL_GRACE (ms): the
     * gate's shorter wait after the last of them */
    if (GetEnvironmentVariableW(L"SG_STORE_UNINSTALL_GRACE", ver, ARRAYSIZE(ver))) grace = _wtoi(ver);
    for (start = GetTickCount(); (unregister_gone(a), find_installed(a->detect_name, ver, ARRAYSIZE(ver)));) {
        BOOL alive = tree_alive(tree, &ntree, ARRAYSIZE(tree), &since);
        if (alive) start = GetTickCount();                  /* 3 minutes after the last of them */
        else if (another_part(a, cmd) || GetTickCount() - start >= grace) break;   /* its part is gone */
        Sleep(1000);
    }
#ifndef SG_MUTANT_NOAFTERWAIT
    /* its entry is gone, but an NSIS uninstaller deletes that before its
     * Start-menu shortcuts: its copy in %TEMP% (Au_.exe) still works. The
     * store stopped watching and the elevated session's end took Au_ with it:
     * Wireshark.lnk and the IrfanView, Mp3tag, Zoom and DYMO folders stayed
     * in the all-users Start menu (qa4). Wait for the rest of it too. */
    if (!find_installed(a->detect_name, ver, ARRAYSIZE(ver)))
        for (start = GetTickCount(); tree_alive(tree, &ntree, ARRAYSIZE(tree), &since) &&
                                     GetTickCount() - start < grace;) Sleep(500);
#endif
    if (find_installed(a->detect_name, ver, ARRAYSIZE(ver))) {
        if (another_part(a, cmd)) return UNINSTALL_NEXT;
        swprintf(err, cch, code ? L"%ls was not removed (its uninstaller stopped with %lu)." : L"%ls was not removed.", a->name, code);
        return 1;
    }
    return 0;
}

/* the app's uninstaller -- and, where it leaves another part listed under
 * the app's name (an MSI under its wrapper), that part's, up to three */
int app_uninstall(app_t *a, WCHAR *err, int cch)
{
    WCHAR ran[2048];
    int round, rc = 1;
    err[0] = 0;
    if (a->method == SRC_LINUX_APT || a->method == SRC_OURS_APT) return sys_remove_apt(a, err, cch);
    for (round = 0; round < 3; round++)
        if ((rc = uninstall_once(a, err, cch, ran, ARRAYSIZE(ran))) != UNINSTALL_NEXT) return rc;
    swprintf(err, cch, L"%ls was not removed.", a->name);
    return 1;
}
