/* sg-browser -- Get a web browser: reading winget installer manifests.
 *
 * Installer manifests (github.com/microsoft/winget-pkgs) are YAML, but a
 * small, regular part of it: "Key: value" at the root, a root
 * InstallerSwitches map, and an Installers list whose items are maps (each
 * may have its own InstallerSwitches, and lists such as InstallModes that we
 * skip). Only the keys we use are kept: Architecture, Scope,
 * InstallerLocale, InstallerType, InstallerUrl, InstallerSha256 and the
 * Silent/SilentWithProgress switches.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "manifest.h"
#include <ctype.h>
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

static void set_field(mf_entry *e, const char *key, const char *val, int switches)
{
    char v[2048];
    copy(v, val, sizeof(v));
    unquote(v);
    if (switches) {
        if (!strcmp(key, "Silent")) copy(e->silent, v, sizeof(e->silent));
        else if (!strcmp(key, "SilentWithProgress") && !e->silent[0]) copy(e->silent, v, sizeof(e->silent));
        return;
    }
    if (!strcmp(key, "Architecture")) copy(e->arch, v, sizeof(e->arch));
    else if (!strcmp(key, "Scope")) copy(e->scope, v, sizeof(e->scope));
    else if (!strcmp(key, "InstallerLocale")) copy(e->locale, v, sizeof(e->locale));
    else if (!strcmp(key, "InstallerType")) copy(e->type, v, sizeof(e->type));
    else if (!strcmp(key, "InstallerUrl")) copy(e->url, v, sizeof(e->url));
    else if (!strcmp(key, "InstallerSha256")) copy(e->sha, v, sizeof(e->sha));
}

int mf_parse(char *text, mf_entry *root, mf_entry *list, int max)
{
    char *line, *next;
    int n = -1, item_indent = -1, in_installers = 0;
    int sw_indent = -1;          /* InstallerSwitches' own indentation, while inside it */
    int sw_root = 0, i;
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
        if (!e->arch[0]) copy(e->arch, root->arch, sizeof(e->arch));
        if (!e->url[0]) copy(e->url, root->url, sizeof(e->url));
        if (!e->sha[0]) copy(e->sha, root->sha, sizeof(e->sha));
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

int mf_pick(const mf_entry *list, int n, const char *locale, int admin, int is64)
{
    int i, best = -1, bestscore = -1;
    for (i = 0; i < n; i++) {
        const mf_entry *e = &list[i];
        int score, ar = arch_rank(e->arch, is64);
        if (!e->url[0] || !ar) continue;
        score = ar * 100;
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
