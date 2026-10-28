/* roots-probe: the machine's Root store, for test/roots-check.sh
 *   roots-probe list           each root whose name starts "SG Gate": "ROOT <name> uses=<OIDs or none>"
 *   roots-probe add DER        add DER to the machine's Root store as an administrator would (no uses) */
#include <windows.h>
#include <wincrypt.h>
#include <stdio.h>

int wmain(int argc, WCHAR **argv)
{
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, L"Root");
    PCCERT_CONTEXT cert = NULL;

    if (!store) { printf("ERROR store\n"); return 1; }
    if (argc > 2 && !wcscmp(argv[1], L"add")) {
        HANDLE f = CreateFileW(argv[2], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        BYTE buf[8192];
        DWORD len = 0;
        if (f == INVALID_HANDLE_VALUE || !ReadFile(f, buf, sizeof(buf), &len, NULL)) { printf("ERROR read\n"); return 1; }
        CloseHandle(f);
        printf(CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, buf, len, CERT_STORE_ADD_NEW, NULL) ? "ADDED\n" : "ERROR add\n");
        CertCloseStore(store, 0);
        return 0;
    }
    while ((cert = CertEnumCertificatesInStore(store, cert))) {
        char name[256];
        BYTE prop[512];
        DWORD size = sizeof(prop), i;
        CERT_ENHKEY_USAGE *eku;
        CertGetNameStringA(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, sizeof(name));
        if (strncmp(name, "SG Gate", 7)) continue;
        printf("ROOT %s uses=", name);
        if (CertGetCertificateContextProperty(cert, CERT_ENHKEY_USAGE_PROP_ID, prop, &size) &&
            CryptDecodeObjectEx(X509_ASN_ENCODING, X509_ENHANCED_KEY_USAGE, prop, size, CRYPT_DECODE_ALLOC_FLAG, NULL, &eku, &size)) {
            for (i = 0; i < eku->cUsageIdentifier; i++) printf("%s%s", i ? "," : "", eku->rgpszUsageIdentifier[i]);
            LocalFree(eku);
        } else printf("none");
        printf("\n");
    }
    CertCloseStore(store, 0);
    return 0;
}
