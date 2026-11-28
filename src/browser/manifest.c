/* sg-browser -- Get a web browser: reading winget installer manifests.
 *
 * Installer manifests (github.com/microsoft/winget-pkgs) are YAML, but a
 * small, regular part of it: "Key: value" at the root, a root
 * InstallerSwitches map, and an Installers list whose items are maps (each
 * may have its own InstallerSwitches, and lists such as InstallModes that we
 * skip). Only the keys we use are kept: Architecture, Scope,
 * InstallerLocale, InstallerType, InstallerUrl, InstallerSha256 and the
 * Silent/SilentWithProgress and Custom switches, ElevationRequirement and
 * InstallerSuccessCodes.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "manifest.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void copy(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

int mf_version_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            char *ea, *eb;
            unsigned long long x = strtoull(a, &ea, 10), y = strtoull(b, &eb, 10);
            if (x != y) return x < y ? -1 : 1;
            a = ea;
            b = eb;
        } else {
            int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
            if (!ca) return -1;
            if (!cb) return 1;
            if (ca != cb) {
                /* "1.0" before "1.0.1" and "1.0-beta": the shorter wins no ties */
                if (ca == '.') return 1;
                if (cb == '.') return -1;
                return ca < cb ? -1 : 1;
            }
            a++;
            b++;
        }
    }
    return 0;
}

int mf_newest_version(const char *json, char *out, int cap)
{
    const char *p = json;
    int found = 0;
    out[0] = 0;
    while ((p = strstr(p, "\"name\""))) {
        const char *q = p + 6, *e;
        char v[64];
        p += 6;
        while (*q == ' ' || *q == ':' || *q == '\t' || *q == '\n' || *q == '\r') q++;
        if (*q != '"') continue;
        q++;
        if (!(e = strchr(q, '"')) || e == q || e - q >= (long)sizeof(v)) continue;
        memcpy(v, q, e - q);
        v[e - q] = 0;
        /* version folders only: not sub-packages (ESR, de...) and not files */
        if (!isdigit((unsigned char)v[0]) || strpbrk(v, "/\\") || strstr(v, ".yaml")) continue;
        if (!found || mf_version_cmp(v, out) > 0) { copy(out, v, cap); found = 1; }
    }
    return found;
}

static void unquote(char *s)
{
    size_t n;
    char *c;
    while (*s == ' ') memmove(s, s + 1, strlen(s));
    if (*s != '"' && *s != '\'' && (c = strstr(s, " #"))) *c = 0;
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\t')) s[--n] = 0;
    if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0]) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = 0;
    }
}

/* one exit code (decimal, maybe negative) onto an entry's success list */
static void add_code(mf_entry *e, const char *text)
{
    char *end;
    long v;
    size_t len;
    while (*text == ' ') text++;
    v = strtol(text, &end, 10);
    if (end == text) return;
    len = strlen(e->success);
    if (len + 16 < sizeof(e->success)) snprintf(e->success + len, sizeof(e->success) - len, "%s%ld", len ? " " : "", v);
}

static void set_field(mf_entry *e, const char *key, const char *val, int switches)
{
    char v[2048];
    copy(v, val, sizeof(v));
    unquote(v);
    if (switches) {
        if (!strcmp(key, "Silent")) copy(e->silent, v, sizeof(e->silent));
        else if (!strcmp(key, "SilentWithProgress") && !e->silent[0]) copy(e->silent, v, sizeof(e->silent));
        else if (!strcmp(key, "Custom")) copy(e->custom, v, sizeof(e->custom));
        return;
    }
    if (!strcmp(key, "Architecture")) copy(e->arch, v, sizeof(e->arch));
    else if (!strcmp(key, "Scope")) copy(e->scope, v, sizeof(e->scope));
    else if (!strcmp(key, "InstallerLocale")) copy(e->locale, v, sizeof(e->locale));
    else if (!strcmp(key, "InstallerType")) copy(e->type, v, sizeof(e->type));
    else if (!strcmp(key, "InstallerUrl")) copy(e->url, v, sizeof(e->url));
    else if (!strcmp(key, "InstallerSha256")) copy(e->sha, v, sizeof(e->sha));
    else if (!strcmp(key, "NestedInstallerType")) copy(e->nested, v, sizeof(e->nested));
    else if (!strcmp(key, "ElevationRequirement")) copy(e->elevation, v, sizeof(e->elevation));
    else if (!strcmp(key, "InstallerSuccessCodes")) {      /* the flow form: [1, 2] */
        const char *t = v;
        while (*t) {
            while (*t && *t != '-' && !isdigit((unsigned char)*t)) t++;
            if (!*t) break;
            add_code(e, t);
            t++;
            while (isdigit((unsigned char)*t)) t++;
        }
    }
}

int mf_parse(char *text, mf_entry *root, mf_entry *list, int max)
{
    char *line, *next;
    int n = -1, item_indent = -1, in_installers = 0;
    int sw_indent = -1;          /* InstallerSwitches' own indentation, while inside it */
    int sw_root = 0, i;
    int dep_indent = -1;         /* Dependencies' own indentation, while inside it */
    mf_entry *dep_to = NULL;
    int nf_indent = -1;          /* NestedInstallerFiles' own indentation, while inside it */
    mf_entry *nf_to = NULL;
    int sc_indent = -1;          /* InstallerSuccessCodes' own indentation, while inside it */
    mf_entry *sc_to = NULL;
    int rc_indent = -1;          /* ExpectedReturnCodes' own indentation, while inside it */
    mf_entry *rc_to = NULL;
    long rc_code = 0;
    int rc_have = 0;
    memset(root, 0, sizeof(*root));
    for (line = text; line && *line; line = next) {
        char *colon, key[64], *body;
        int indent = 0, item;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        while (line[indent] == ' ') indent++;
        body = line + indent;
        if (!*body || *body == '#' || *body == '\r') continue;
        item = body[0] == '-' && (body[1] == ' ' || !body[1] || body[1] == '\r');

        /* a switches block ends where the indentation comes back */
        if (sw_indent >= 0 && indent <= sw_indent) sw_indent = -1;
        /* so does a Dependencies block; inside it, only the packages count
         * (- PackageIdentifier: Git.Git) -- not Windows features, libraries
         * or MinimumVersion */
        if (dep_indent >= 0 && indent <= dep_indent) dep_indent = -1;
        /* NestedInstallerFiles: - RelativeFilePath: the installer inside the zip
         * (CutePDF's zip holds two .exe files; the manifest says which) */
        if (nf_indent >= 0 && (indent < nf_indent || (indent == nf_indent && !item))) nf_indent = -1;
        /* InstallerSuccessCodes: - -2147219416 (WebView2's "already installed
         * by a newer one" and the like): exit codes that are not failures */
        if (sc_indent >= 0 && (indent < sc_indent || (indent == sc_indent && !item))) sc_indent = -1;
        if (sc_indent >= 0) {
            if (item) add_code(sc_to, body + 1);
            continue;
        }
        /* ExpectedReturnCodes: - InstallerReturnCode: 1618 / ReturnResponse: installInProgress */
        if (rc_indent >= 0 && (indent < rc_indent || (indent == rc_indent && !item))) rc_indent = -1;
        if (rc_indent >= 0) {
#ifndef SG_MUTANT_NOEXPECTED
            char *k = item ? body + 1 : body, v[64], *end;
            size_t len;
            while (*k == ' ') k++;
            if (item) rc_have = 0;
            if (!strncmp(k, "InstallerReturnCode:", 20)) {
                rc_code = strtol(k + 20, &end, 10);
                rc_have = end != k + 20;
            } else if (!strncmp(k, "ReturnResponse:", 15) && rc_have) {
                copy(v, k + 15, sizeof(v));
                unquote(v);
                len = strlen(rc_to->expected);
                if (v[0] && len + strlen(v) + 24 < sizeof(rc_to->expected))
                    snprintf(rc_to->expected + len, sizeof(rc_to->expected) - len, "%s%ld=%s", len ? " " : "", rc_code, v);
                rc_have = 0;
            }
#endif
            continue;
        }
        if (nf_indent >= 0) {
#ifndef SG_MUTANT_NONESTEDFILE
            char *k = item ? body + 1 : body;
            while (*k == ' ') k++;
            if (!strncmp(k, "RelativeFilePath:", 17) && !nf_to->nested_file[0]) {
                copy(nf_to->nested_file, k + 17, sizeof(nf_to->nested_file));
                unquote(nf_to->nested_file);
            }
#endif
            continue;
        }
        if (dep_indent >= 0) {
#ifndef SG_MUTANT_NODEPS
            char *k = item ? body + 1 : body, v[256];
            size_t len;
            while (*k == ' ') k++;
            if (!strncmp(k, "PackageIdentifier:", 18)) {
                copy(v, k + 18, sizeof(v));
                unquote(v);
                len = strlen(dep_to->deps);
                if (v[0] && len + strlen(v) + 2 < sizeof(dep_to->deps)) {
                    if (len) dep_to->deps[len++] = ' ';
                    strcpy(dep_to->deps + len, v);
                }
            }
#endif
            continue;
        }
        if (indent == 0 && !item) in_installers = 0;

        if (item) {
            if (!in_installers || (item_indent >= 0 && indent != item_indent)) continue;   /* another list */
            item_indent = indent;
            if (n + 1 >= max) break;
            n++;
            memset(&list[n], 0, sizeof(list[n]));
            sw_indent = -1;
            body += 1;
            while (*body == ' ') body++;
            indent = item_indent + 2;
            if (!*body) continue;
        }
        if (!(colon = strchr(body, ':')) || colon - body >= (long)sizeof(key)) continue;
        memcpy(key, body, colon - body);
        key[colon - body] = 0;
        colon++;
        if (sw_indent >= 0) {
            set_field(sw_root ? root : &list[n], key, colon, 1);
            continue;
        }
        while (*colon == ' ') colon++;
        if (!*colon || *colon == '\r') {
            if (indent == 0 && !strcmp(key, "Installers")) { in_installers = 1; item_indent = -1; }
            else if (!strcmp(key, "Dependencies") && (indent == 0 || (in_installers && n >= 0 && indent == item_indent + 2))) {
                dep_indent = indent;
                dep_to = indent == 0 ? root : &list[n];
            }
            else if (!strcmp(key, "InstallerSuccessCodes") && (indent == 0 || (in_installers && n >= 0 && indent == item_indent + 2))) {
                sc_indent = indent;
                sc_to = indent == 0 ? root : &list[n];
            }
            else if (!strcmp(key, "ExpectedReturnCodes") && (indent == 0 || (in_installers && n >= 0 && indent == item_indent + 2))) {
                rc_indent = indent;
                rc_to = indent == 0 ? root : &list[n];
                rc_have = 0;
            }
            else if (!strcmp(key, "NestedInstallerFiles") && (indent == 0 || (in_installers && n >= 0 && indent == item_indent + 2))) {
                nf_indent = indent;
                nf_to = indent == 0 ? root : &list[n];
            }
            else if (!strcmp(key, "InstallerSwitches") && (indent == 0 || (in_installers && n >= 0))) {
                sw_indent = indent;
                sw_root = indent == 0;
            }
            continue;
        }
        if (indent == 0) set_field(root, key, colon, 0);
        else if (in_installers && n >= 0 && indent == item_indent + 2) set_field(&list[n], key, colon, 0);
    }
    for (i = 0; i <= n; i++) {
        mf_entry *e = &list[i];
        if (!e->scope[0]) copy(e->scope, root->scope, sizeof(e->scope));
        if (!e->locale[0]) copy(e->locale, root->locale, sizeof(e->locale));
        if (!e->type[0]) copy(e->type, root->type, sizeof(e->type));
        if (!e->silent[0]) copy(e->silent, root->silent, sizeof(e->silent));
        if (!e->custom[0]) copy(e->custom, root->custom, sizeof(e->custom));
        if (!e->elevation[0]) copy(e->elevation, root->elevation, sizeof(e->elevation));
        if (!e->arch[0]) copy(e->arch, root->arch, sizeof(e->arch));
        if (!e->url[0]) copy(e->url, root->url, sizeof(e->url));
        if (!e->sha[0]) copy(e->sha, root->sha, sizeof(e->sha));
        if (!e->nested[0]) copy(e->nested, root->nested, sizeof(e->nested));
        if (!e->nested_file[0]) copy(e->nested_file, root->nested_file, sizeof(e->nested_file));
        if (!e->deps[0]) copy(e->deps, root->deps, sizeof(e->deps));
        if (!e->success[0]) copy(e->success, root->success, sizeof(e->success));
        if (!e->expected[0]) copy(e->expected, root->expected, sizeof(e->expected));
    }
    return n + 1;
}

static int arch_rank(const char *a, int is64)
{
    if (is64 && !strcasecmp(a, "x64")) return 3;
    if (!strcasecmp(a, "neutral") || !a[0]) return 2;
    if (!strcasecmp(a, "x86")) return 1;
    return 0;   /* arm, arm64 */
}

static int runnable(const char *t)
{
    static const char *const kinds[] = { "msi", "wix", "nullsoft", "inno", "burn", "exe" };
    unsigned i;
    for (i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) if (!strcasecmp(t, kinds[i])) return 1;
    return 0;
}

int mf_type_rank(const char *type, const char *nested)
{
    if (runnable(type)) return 2;
    if (!strcasecmp(type, "zip") && nested && runnable(nested)) return 1;
    if (!strcasecmp(type, "msix") || !strcasecmp(type, "appx")) return 1;   /* PackageManager adds it */
    return 0;
}

int mf_pick(const mf_entry *list, int n, const char *locale, int admin, int is64)
{
    int i, best = -1, bestscore = -1;
    for (i = 0; i < n; i++) {
        const mf_entry *e = &list[i];
        int score, ar = arch_rank(e->arch, is64);
        if (!e->url[0] || !ar) continue;
        /* an installer we can run beats a portable zip for a better arch or
         * scope (Notepad++, Telegram, Heroic list both) */
#ifdef SG_MUTANT_ANYTYPE
        score = ar * 100;
#else
        score = mf_type_rank(e->type, e->nested) * 1000 + ar * 100;
#endif
        if (locale && !strcasecmp(e->locale, locale)) score += 20;
        else if (!strcasecmp(e->locale, "en-US") || !e->locale[0]) score += 10;
        if (!strcasecmp(e->scope, admin ? "machine" : "user")) score += 5;
        else if (!e->scope[0]) score += 3;
        if (score > bestscore) { bestscore = score; best = i; }
    }
    return best;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

int mf_url_names_version(const char *url, const char *version)
{
    char vd[64], pd[2048], parts[64];
    const char *path = strstr(url, "://");
    int n = 0, k = 0, len;
#ifdef SG_MUTANT_URLVER
    return 1;
#endif
    if (!path || !(path = strchr(path + 3, '/')) || !version[0]) return 0;
    if (strstr(path, version)) return 1;
    /* the version less its trailing ".0" parts, digits only: 7.23.0 -> 723 */
    snprintf(parts, sizeof(parts), "%s", version);
    len = (int)strlen(parts);
    while (len >= 2 && parts[len - 1] == '0' && parts[len - 2] == '.') parts[len -= 2] = 0;
    for (k = 0; parts[k] && n < (int)sizeof(vd) - 1; k++) if (isdigit((unsigned char)parts[k])) vd[n++] = parts[k];
    vd[n] = 0;
    if (n < 2) return 0;
    for (k = 0, n = 0; path[k] && n < (int)sizeof(pd) - 1; k++) if (isdigit((unsigned char)path[k])) pd[n++] = path[k];
    pd[n] = 0;
    return strstr(pd, vd) != NULL;
}

int mf_sha256(const char *hex, unsigned char out[32])
{
    int k;
    if (strlen(hex) != 64) return 0;
    for (k = 0; k < 32; k++) {
        int hi = hexval((unsigned char)hex[2 * k]), lo = hexval((unsigned char)hex[2 * k + 1]);
        if (hi < 0 || lo < 0) return 0;
        out[k] = (unsigned char)(hi << 4 | lo);
    }
    return 1;
}

/* ---- what an exit code means ----------------------------------------------------------------------
 *
 * "The installer stopped with code 1618" told the person nothing. As winget does, the manifest's
 * ExpectedReturnCodes say what a package's own codes mean (its ReturnResponse values), and the
 * installer kinds' documented codes say the rest: Windows Installer's (an MSI, a WiX bundle), Inno
 * Setup's and NSIS's. */
static const struct { const char *response, *text; } responses[] = {
    { "packageInUse", "the app is in use: close it and try again" },
    { "packageInUseByApplication", "a program that uses it is running: close it and try again" },
    { "fileInUse", "a file it needs is in use: close the programs using it and try again" },
    { "installInProgress", "another installation is in progress: try again when it has finished" },
    { "missingDependency", "something it needs is not installed" },
    { "diskFull", "the disk is full: free some space and try again" },
    { "insufficientMemory", "there is not enough memory: close some programs and try again" },
    { "invalidParameter", "it did not accept the options it was given" },
    { "noNetwork", "it needs a connection to the internet" },
    { "contactSupport", "it reported an error its maker's support can explain" },
    { "rebootRequiredToFinish", "restart the computer to finish the installation" },
    { "rebootRequiredForInstall", "restart the computer, then install it again" },
    { "rebootInitiated", "it is restarting the computer to finish" },
    { "cancelledByUser", "the installation was cancelled" },
    { "alreadyInstalled", "another version of it is already installed" },
    { "downgrade", "a newer version of it is already installed" },
    { "blockedByPolicy", "an administrator's policy does not allow it" },
    { "systemNotSupported", "it does not support this system" },
};

static const struct { long code; const char *text; } msi_codes[] = {
    { 1601, "the installer service could not be reached" },
    { 1602, "the installation was cancelled" },
    { 1603, "it failed while installing (a fatal error)" },
    { 1618, "another installation is in progress: try again when it has finished" },
    { 1619, "the installation package could not be opened" },
    { 1620, "the installation package could not be opened" },
    { 1625, "an administrator's policy does not allow it" },
    { 1633, "it does not support this system" },
    { 1638, "another version of it is already installed: uninstall that first" },
    { 1639, "it did not accept the options it was given" },
    { 1223, "the installation was cancelled" },
    { -2147023673, "the installation was cancelled" },   /* 0x800704C7, a bundle's ERROR_CANCELLED */
}, inno_codes[] = {
    { 1, "setup could not start" },
    { 2, "the installation was cancelled before it began" },
    { 3, "it failed while preparing to install" },
    { 4, "it failed while installing" },
    { 5, "the installation was cancelled" },
    { 6, "setup was stopped" },
    { 7, "it found something on this computer that stops it from installing" },
    { 8, "restart the computer, then install it again" },
}, nsis_codes[] = {
    { 1, "the installation was cancelled" },
    { 2, "the installer stopped itself" },
};

const char *mf_exit_reason(const char *type, long code, const char *expected)
{
    const char *t = expected;
    unsigned i;
    while (t && *t) {
        char *end, resp[64];
        long c = strtol(t, &end, 10);
        size_t k = 0;
        if (end == t || *end != '=') break;
        t = end + 1;
        while (*t && *t != ' ' && k + 1 < sizeof(resp)) resp[k++] = *t++;
        resp[k] = 0;
        while (*t == ' ') t++;
        if (c != code) continue;
        for (i = 0; i < sizeof(responses) / sizeof(responses[0]); i++)
            if (!strcasecmp(resp, responses[i].response)) return responses[i].text;
    }
#ifdef SG_MUTANT_NOEXITREASON
    return NULL;
#endif
    if (!type) return NULL;
    if (!strcasecmp(type, "msi") || !strcasecmp(type, "wix") || !strcasecmp(type, "burn")) {
        for (i = 0; i < sizeof(msi_codes) / sizeof(msi_codes[0]); i++) if (msi_codes[i].code == code) return msi_codes[i].text;
    } else if (!strcasecmp(type, "inno")) {
        for (i = 0; i < sizeof(inno_codes) / sizeof(inno_codes[0]); i++) if (inno_codes[i].code == code) return inno_codes[i].text;
    } else if (!strcasecmp(type, "nullsoft")) {
        for (i = 0; i < sizeof(nsis_codes) / sizeof(nsis_codes[0]); i++) if (nsis_codes[i].code == code) return nsis_codes[i].text;
    }
    return NULL;
}
