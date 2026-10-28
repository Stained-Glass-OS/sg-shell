/*
 * sg-roots -- the root certificates Windows' root program trusts for code signing and time
 * stamping, in the machine's Root store, each only for those uses.
 *
 *   sg-roots --sync BUNDLE.pem     add or keep each root of the bundle, remove the ones this
 *                                  tool added that the bundle no longer has
 *   sg-roots --list                the roots this tool added
 *
 * The bundle (stained-glass data, made from CCADB's public report of Microsoft's root
 * program) holds, before each certificate, "# uses: OID OID" -- its Microsoft EKUs among
 * Code Signing and Time Stamping. Windows marks a root's uses with the certificate's enhanced
 * key usage property; so does this tool, and wine-sg 7602 makes the chain engine honour it:
 * a code signing root is not trusted for a web server.
 *
 * A root already in the store that this tool did not add -- the host's, Wine imported, or
 * one an administrator trusts -- is left as it is: never narrowed, never removed. What this
 * tool added is recorded under HKLM\Software\Stained Glass\Code Signing Roots (its SHA-1).
 * Run as SYSTEM (the machine store is the administrators').
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD_KEY L"Software\\Stained Glass\\Code Signing Roots"
#define MAX_ROOTS 512

typedef struct { BYTE *der; DWORD len; char uses[256]; BYTE sha1[20]; } root_t;

static root_t g_roots[MAX_ROOTS];
static int g_nroots;

static void hex(const BYTE *b, int n, WCHAR *out)
{
    int i;
    for (i = 0; i < n; i++) swprintf(out + 2 * i, 3, L"%02X", b[i]);
}

/* the bundle: "# uses: ..." then a PEM certificate, any number of times */
static BOOL read_bundle(const WCHAR *path)
{
    FILE *f = _wfopen(path, L"rb");
    char line[1024], uses[256] = "", *b64 = NULL;
    size_t blen = 0, bcap = 0;
    BOOL in = FALSE;

    if (!f) return FALSE;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "# uses:", 7)) { lstrcpynA(uses, line + 7 + strspn(line + 7, " "), sizeof(uses)); continue; }
        if (!strcmp(line, "-----BEGIN CERTIFICATE-----")) { in = TRUE; blen = 0; continue; }
        if (!strcmp(line, "-----END CERTIFICATE-----") && in) {
            DWORD len = 0;
            in = FALSE;
            if (g_nroots >= MAX_ROOTS || !uses[0]) { uses[0] = 0; continue; }
            if (!CryptStringToBinaryA(b64, (DWORD)blen, CRYPT_STRING_BASE64, NULL, &len, NULL, NULL)) continue;
            g_roots[g_nroots].der = malloc(len);
            if (!g_roots[g_nroots].der ||
                !CryptStringToBinaryA(b64, (DWORD)blen, CRYPT_STRING_BASE64, g_roots[g_nroots].der, &len, NULL, NULL)) continue;
            g_roots[g_nroots].len = len;
            lstrcpynA(g_roots[g_nroots].uses, uses, sizeof(g_roots[g_nroots].uses));
            {
                DWORD n = 20;
                CryptHashCertificate(0, CALG_SHA1, 0, g_roots[g_nroots].der, len, g_roots[g_nroots].sha1, &n);
            }
            g_nroots++;
            uses[0] = 0;
            continue;
        }
        if (in) {
            size_t n = strlen(line);
            if (blen + n + 1 > bcap) { bcap = (blen + n + 1) * 2; b64 = realloc(b64, bcap); if (!b64) { fclose(f); return FALSE; } }
            memcpy(b64 + blen, line, n);
            blen += n;
            b64[blen] = 0;
        }
    }
    fclose(f);
    free(b64);
    return TRUE;
}

/* the uses, as the enhanced key usage property's encoding */
static BOOL set_uses(PCCERT_CONTEXT cert, const char *uses)
{
    LPSTR oids[8];
    char copy[256], *tok, *ctx = NULL;
    CERT_ENHKEY_USAGE eku = { 0, oids };
    CRYPT_DATA_BLOB blob = { 0, NULL };
    BOOL ok;

    lstrcpynA(copy, uses, sizeof(copy));
    for (tok = strtok_s(copy, " ", &ctx); tok && eku.cUsageIdentifier < 8; tok = strtok_s(NULL, " ", &ctx))
        oids[eku.cUsageIdentifier++] = tok;
#ifdef SG_MUTANT_NOUSES
    return TRUE;
#endif
    if (!CryptEncodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &eku, NULL, &blob.cbData)) return FALSE;
    if (!(blob.pbData = malloc(blob.cbData))) return FALSE;
    ok = CryptEncodeObject(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, &eku, blob.pbData, &blob.cbData) &&
         CertSetCertificateContextProperty(cert, CERT_ENHKEY_USAGE_PROP_ID, 0, &blob);
    free(blob.pbData);
    return ok;
}

static BOOL unhex(const WCHAR *s, BYTE *out)
{
    int i;
    for (i = 0; i < 40; i++) {
        WCHAR c = s[i];
        int v = c >= L'0' && c <= L'9' ? c - L'0' : c >= L'A' && c <= L'F' ? c - L'A' + 10 : c >= L'a' && c <= L'f' ? c - L'a' + 10 : -1;
        if (v < 0) return FALSE;
        if (i % 2) out[i / 2] |= v; else out[i / 2] = v << 4;
    }
    return !s[40];
}

static BOOL recorded(HKEY rec, const WCHAR *sha)
{
    return !RegQueryValueExW(rec, sha, NULL, NULL, NULL, NULL);
}

static int sync(const WCHAR *bundle)
{
    HCERTSTORE store;
    HKEY rec;
    int i, added = 0, kept = 0, foreign = 0, removed = 0, failed = 0;
    DWORD idx, n;
    WCHAR name[64];

    if (!read_bundle(bundle) || !g_nroots) { fwprintf(stderr, L"sg-roots: %ls: no roots\n", bundle); return 2; }
    store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"Root");
    if (!store) { fwprintf(stderr, L"sg-roots: the machine's Root store: error %lu\n", GetLastError()); return 1; }
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, RECORD_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &rec, NULL)) {
        fwprintf(stderr, L"sg-roots: cannot record (an administrator's job)\n");
        CertCloseStore(store, 0);
        return 1;
    }
    for (i = 0; i < g_nroots; i++) {
        CRYPT_HASH_BLOB h = { 20, g_roots[i].sha1 };
        PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &h, NULL);
        WCHAR sha[41];
        hex(g_roots[i].sha1, 20, sha);
        if (cert && !recorded(rec, sha)) {   /* someone else's trust: left as it is */
            foreign++;
            CertFreeCertificateContext(cert);
            continue;
        }
        if (!cert && !CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, g_roots[i].der, g_roots[i].len,
                                                       CERT_STORE_ADD_NEW, &cert)) { failed++; continue; }
        else if (!recorded(rec, sha)) added++;
        else kept++;
        if (!set_uses(cert, g_roots[i].uses)) failed++;
        RegSetValueExW(rec, sha, 0, REG_SZ, (const BYTE *)L"", sizeof(WCHAR));
        CertFreeCertificateContext(cert);
    }
    /* the ones this tool added that the bundle no longer has */
    for (idx = 0; n = ARRAYSIZE(name), !RegEnumValueW(rec, idx, name, &n, NULL, NULL, NULL, NULL);) {
        BYTE sha1[20];
        BOOL in = FALSE;
        if (!unhex(name, sha1)) { idx++; continue; }
        for (i = 0; i < g_nroots && !in; i++) in = !memcmp(g_roots[i].sha1, sha1, 20);
#ifdef SG_MUTANT_NOREMOVE
        in = TRUE;
#endif
        if (in) { idx++; continue; }
        {
            CRYPT_HASH_BLOB h = { 20, sha1 };
            PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &h, NULL);
            if (cert) CertDeleteCertificateFromStore(cert);
        }
        RegDeleteValueW(rec, name);   /* the enumeration goes on at the same index */
        removed++;
    }
    RegCloseKey(rec);
    CertCloseStore(store, 0);
    wprintf(L"sg-roots: %d added, %d kept, %d removed, %d left as they were (not ours), %d failed\n",
            added, kept, removed, foreign, failed);
    return failed ? 1 : 0;
}

int wmain(int argc, WCHAR **argv)
{
    if (argc > 2 && !wcscmp(argv[1], L"--sync")) return sync(argv[2]);
    if (argc > 1 && !wcscmp(argv[1], L"--list")) {
        HKEY rec;
        WCHAR name[64];
        DWORD idx, n;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, RECORD_KEY, 0, KEY_READ, &rec)) return 0;
        for (idx = 0; n = ARRAYSIZE(name), !RegEnumValueW(rec, idx, name, &n, NULL, NULL, NULL, NULL); idx++)
            wprintf(L"%ls\n", name);
        RegCloseKey(rec);
        return 0;
    }
    fwprintf(stderr, L"usage: sg-roots --sync BUNDLE.pem | --list\n");
    return 2;
}
