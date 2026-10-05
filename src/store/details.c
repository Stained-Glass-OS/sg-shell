/* SG Store -- an app's details page: what it is and where it comes from.
 *
 * David 2026-10-04: clicking an app, not its Install button or its box,
 * should say more -- where it comes from, and the whole description. The
 * first time an app's page is opened its details are fetched on a thread:
 *
 *   a Linux app or one of ours: the system's package lists (sg-appinfo,
 *     apt-cache) -- the long description, homepage, size, maintainer, and
 *     the repository apt gets it from;
 *   a Windows program from winget: the winget community repository's locale
 *     manifest -- Description, publisher's site, licence;
 *   a pinned download: the catalogue's own entry (where, which version).
 *
 * WM_DETAILS tells the window when they are in.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "store.h"

extern app_t g_apps[];
extern int g_napps;
extern HWND g_wnd;

static details_t *g_details[MAX_APPS];

/* ---- apt: sg-appinfo's lines ------------------------------------------- */

static void line_field(const char *text, const char *key, WCHAR *out, int cch)
{
    size_t kl = strlen(key);
    const char *p = text;
    out[0] = 0;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        if (!strncmp(p, key, kl) && p[kl] == ' ') {
            int n = (int)((nl ? nl : p + strlen(p)) - (p + kl + 1));
            if (n > 0) {
                n = MultiByteToWideChar(CP_UTF8, 0, p + kl + 1, n, out, cch - 1);
                out[max(n, 0)] = 0;
            }
            if (!lstrcmpW(out, L"-")) out[0] = 0;
            return;
        }
        p = nl ? nl + 1 : NULL;
    }
}

static void from_appinfo(details_t *d, const char *text)
{
    const char *p = text;
    int n = 0;
    if (!strstr(text, "\nOK") && strncmp(text, "OK", 2)) {
        line_field(text, "ERROR", d->error, ARRAYSIZE(d->error));
        if (!d->error[0]) lstrcpynW(d->error, L"The package lists do not have it.", ARRAYSIZE(d->error));
        return;
    }
    line_field(text, "VERSION", d->version, ARRAYSIZE(d->version));
    line_field(text, "INSTALLED", d->installed, ARRAYSIZE(d->installed));
    line_field(text, "SUMMARY", d->summary, ARRAYSIZE(d->summary));
    line_field(text, "HOMEPAGE", d->homepage, ARRAYSIZE(d->homepage));
    line_field(text, "SECTION", d->section, ARRAYSIZE(d->section));
    line_field(text, "MAINTAINER", d->maintainer, ARRAYSIZE(d->maintainer));
    line_field(text, "ORIGIN", d->origin, ARRAYSIZE(d->origin));
    {
        WCHAR kb[32];
        line_field(text, "SIZE", kb, ARRAYSIZE(kb));
        if (kb[0]) {
            ULONGLONG k = _wcstoui64(kb, NULL, 10);
            if (k >= 1024 * 1024) swprintf(d->size, ARRAYSIZE(d->size), L"%.1f GB", k / 1048576.0);
            else if (k >= 1024) swprintf(d->size, ARRAYSIZE(d->size), L"%.1f MB", k / 1024.0);
            else swprintf(d->size, ARRAYSIZE(d->size), L"%llu KB", k);
        }
    }
    /* DESC lines: the long description, a line each; an empty one between paragraphs */
    while (p && *p) {
        const char *nl = strchr(p, '\n'), *end = nl ? nl : p + strlen(p);
        if (!strncmp(p, "DESC", 4) && (p[4] == ' ' || p + 4 == end)) {
            const char *s = p[4] == ' ' ? p + 5 : p + 4;
            int left = ARRAYSIZE(d->desc) - n - 4, k;
            if (left <= 0) break;
            if (s == end) { if (n && d->desc[n - 1] != '\n') { d->desc[n++] = '\n'; d->desc[n++] = '\n'; } }
            else {
                if (n && d->desc[n - 1] != '\n') d->desc[n++] = ' ';
                k = MultiByteToWideChar(CP_UTF8, 0, s, (int)(end - s), d->desc + n, left);
                n += max(k, 0);
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    d->desc[n] = 0;
}

/* ---- winget: a locale manifest's fields --------------------------------- */

/* KEY's value: a plain or quoted scalar, or a block (| or >) of the lines
 * indented under it */
static void yaml_get(const char *y, const char *key, WCHAR *out, int cch)
{
    size_t kl = strlen(key);
    const char *p = y;
    char *buf;
    int n = 0;
    out[0] = 0;
    if (!(buf = malloc(65536))) return;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        if (!strncmp(p, key, kl) && p[kl] == ':') {
            const char *v = p + kl + 1, *end = nl ? nl : p + strlen(p);
            while (v < end && (*v == ' ' || *v == '\t')) v++;
            if (v < end && (*v == '|' || *v == '>')) {
                BOOL fold = *v == '>';
                int indent = -1, blanks = 0;
                const char *q = nl ? nl + 1 : NULL;
                while (q && *q) {
                    const char *qn = strchr(q, '\n'), *qe = qn ? qn : q + strlen(q), *t = q;
                    int ind = 0;
                    while (t < qe && *t == ' ') { t++; ind++; }
                    if (t < qe && *t == '\r') qe = t;
                    if (t == qe || (qe > t && qe[-1] == '\r' && qe - 1 == t)) {      /* an empty line: a paragraph */
                        blanks++;
                    } else {
                        if (indent < 0) indent = ind;
                        if (ind < indent || indent == 0) break;
                        /* lines: a newline between them (|) or a space (>); each empty line, a newline more */
                        if (n && n < 65500) {
                            if (!fold || !blanks) buf[n++] = fold ? ' ' : '\n';
                            while (blanks-- > 0) buf[n++] = '\n';
                        }
                        blanks = 0;
                        while (t < qe && qe[-1] == '\r') qe--;
                        if (n + (qe - t) < 65530) { memcpy(buf + n, t, qe - t); n += (int)(qe - t); }
                    }
                    q = qn ? qn + 1 : NULL;
                }
            } else {
                while (end > v && (end[-1] == '\r' || end[-1] == ' ')) end--;
                if (end - v >= 2 && (*v == '"' || *v == '\'') && end[-1] == *v) {
                    char qc = *v;
                    for (v++, end--; v < end && n < 65530; v++) {
                        if (qc == '"' && *v == '\\' && v + 1 < end) { v++; buf[n++] = *v == 'n' ? '\n' : *v; }
                        else buf[n++] = *v;
                    }
                } else if (end > v) { memcpy(buf, v, min(end - v, 65530)); n = (int)min(end - v, 65530); }
            }
            break;
        }
        p = nl ? nl + 1 : NULL;
    }
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == ' ')) n--;
    if (n) {
        int k = MultiByteToWideChar(CP_UTF8, 0, buf, n, out, cch - 1);
        out[max(k, 0)] = 0;
    }
    free(buf);
}

static void from_winget(details_t *d, const app_t *a)
{
    WCHAR err[256] = L"", url[512];
    char *y = winget_locale_manifest(a->winget_id, d->version, ARRAYSIZE(d->version), err, ARRAYSIZE(err));
    if (!y) { lstrcpynW(d->error, err[0] ? err : L"Its description could not be fetched.", ARRAYSIZE(d->error)); return; }
    yaml_get(y, "ShortDescription", d->summary, ARRAYSIZE(d->summary));
    yaml_get(y, "Description", d->desc, ARRAYSIZE(d->desc));
    yaml_get(y, "PackageUrl", d->homepage, ARRAYSIZE(d->homepage));
    if (!d->homepage[0]) yaml_get(y, "PublisherUrl", d->homepage, ARRAYSIZE(d->homepage));
    yaml_get(y, "License", d->license, ARRAYSIZE(d->license));
    yaml_get(y, "Publisher", d->maintainer, ARRAYSIZE(d->maintainer));
    yaml_get(y, "PublisherUrl", url, ARRAYSIZE(url));
    if (url[0]) lstrcpynW(d->origin, url, ARRAYSIZE(d->origin));
    free(y);
}

/* ---- the fetch ---------------------------------------------------------- */

static DWORD WINAPI fetch_thread(void *arg)
{
    int i = (int)(INT_PTR)arg;
    const app_t *a = &g_apps[i];
    details_t *d = g_details[i];
    char *text;
    switch (a->method) {
    case SRC_LINUX_APT: case SRC_OURS_APT:
        if ((text = sys_appinfo(a->apt_pkg))) { from_appinfo(d, text); free(text); }
        else lstrcpynW(d->error, L"The package lists could not be read.", ARRAYSIZE(d->error));
        break;
    case SRC_WINGET:
        from_winget(d, a);
        break;
    case SRC_PIN:
        lstrcpynW(d->version, a->pin_version, ARRAYSIZE(d->version));
        {
            /* no manifest to read: the catalogue says what licence it comes
             * under and where its maker describes it */
            WCHAR key[128];
            DWORD cb;
            swprintf(key, ARRAYSIZE(key), L"%ls\\Apps\\%ls", STORE_KEY, a->ord);
            cb = sizeof(d->license);
            RegGetValueW(HKEY_LOCAL_MACHINE, key, L"License", RRF_RT_REG_SZ | RRF_ZEROONFAILURE, NULL, d->license, &cb);
            cb = sizeof(d->homepage);
            RegGetValueW(HKEY_LOCAL_MACHINE, key, L"Homepage", RRF_RT_REG_SZ | RRF_ZEROONFAILURE, NULL, d->homepage, &cb);
            cb = sizeof(d->desc);   /* the whole of it (the card's copy is cut short) */
            RegGetValueW(HKEY_LOCAL_MACHINE, key, L"Description", RRF_RT_REG_SZ | RRF_ZEROONFAILURE, NULL, d->desc, &cb);
        }
        break;
    }
    if (!d->desc[0] && d->summary[0]) lstrcpynW(d->desc, d->summary, ARRAYSIZE(d->desc));
    if (!d->desc[0]) lstrcpynW(d->desc, a->desc, ARRAYSIZE(d->desc));
    InterlockedExchange(&d->state, 2);
    if (g_wnd) PostMessageW(g_wnd, WM_DETAILS, i, 0);
    return 0;
}

details_t *details_get(int i)
{
    details_t *d;
    HANDLE t;
    if (i < 0 || i >= g_napps) return NULL;
    if (!(d = g_details[i])) {
        if (!(d = calloc(1, sizeof(*d)))) return NULL;
        g_details[i] = d;
    }
    if (InterlockedCompareExchange(&d->state, 1, 0) == 0) {
        if ((t = CreateThread(NULL, 0, fetch_thread, (void *)(INT_PTR)i, 0, NULL))) CloseHandle(t);
        else d->state = 2;
    }
    return d;
}

/* the host of a URL: "https://deb.debian.org/debian" -> "deb.debian.org" */
static void url_host(const WCHAR *url, WCHAR *out, int cch)
{
    const WCHAR *s = wcsstr(url, L"://"), *e;
    s = s ? s + 3 : url;
    for (e = s; *e && *e != '/' && *e != ' '; e++) ;
    lstrcpynW(out, s, min(cch, (int)(e - s) + 1));
}

/* Where it comes from, in a sentence */
void details_source(const app_t *a, const details_t *d, WCHAR *out, int cch)
{
    WCHAR host[256] = L"";
    switch (a->method) {
    case SRC_WINGET:
        swprintf(out, cch, L"A Windows program, downloaded from its maker's own site as the winget community "
                 L"repository lists it (%ls), and installed only if the file is exactly the one listed there (SHA-256).",
                 a->winget_id);
        break;
    case SRC_PIN:
        url_host(a->pin_url, host, ARRAYSIZE(host));
        swprintf(out, cch, L"A Windows program, downloaded from %ls, and installed only if the file is exactly "
                 L"the one this store expects (SHA-256).", host[0] ? host : L"its maker's site");
        break;
    case SRC_OURS_APT:
        swprintf(out, cch, L"Made for Stained Glass OS: the package %ls from the Stained Glass repository%ls%ls%ls, "
                 L"kept up to date with the system's updates.", a->apt_pkg,
                 d && d->origin[0] ? L" (" : L"", d && d->origin[0] ? d->origin : L"", d && d->origin[0] ? L")" : L"");
        break;
    case SRC_LINUX_APT:
        if (d && d->origin[0]) url_host(d->origin, host, ARRAYSIZE(host));
        swprintf(out, cch, L"A Linux app: the package %ls%ls%ls%ls, installed with the system's package manager and "
                 L"kept up to date with the system's updates.", a->apt_pkg,
                 host[0] ? L" from " : L"", host[0] ? host : L"",
                 wcsstr(host, L"debian.org") ? L" (Debian's own repository)" : L"");
        break;
    default:
        lstrcpynW(out, L"Where it comes from is not known.", cch);
    }
}
