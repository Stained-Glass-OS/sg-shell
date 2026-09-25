/* Unit test for Get a web browser's manifest reader (src/browser/manifest.c),
 * built and run natively by test/browser-check.sh: version order, the newest
 * version in a GitHub listing, installer manifests shaped like the real
 * Firefox (nullsoft, root switches), Chrome (wix, quoted product codes) and
 * Brave (per-installer scope, switches and InstallModes lists) ones, and
 * which installer a PC gets.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdio.h>
#include <string.h>
#include "../src/browser/manifest.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("PASS  "); else { printf("FAIL  "); fails++; } printf(__VA_ARGS__); printf("\n"); } while (0)

static const char FIREFOX[] =
    "# Created with YamlCreate.ps1\n"
    "# yaml-language-server: $schema=https://aka.ms/winget-manifest.installer.1.12.0.schema.json\n"
    "\n"
    "PackageIdentifier: Mozilla.Firefox\n"
    "PackageVersion: 156.0.1\n"
    "InstallerType: nullsoft\n"
    "Scope: machine\n"
    "InstallerSwitches:\n"
    "  Silent: /S /PreventRebootRequired=true\n"
    "  SilentWithProgress: /S /PreventRebootRequired=true\n"
    "  InstallLocation: /InstallDirectoryPath=\"<INSTALLPATH>\"\n"
    "UpgradeBehavior: install\n"
    "Protocols:\n"
    "- http\n"
    "- https\n"
    "FileExtensions:\n"
    "- htm\n"
    "- html\n"
    "ProductCode: Mozilla Firefox\n"
    "Installers:\n"
    "- Architecture: x86\n"
    "  InstallerUrl: https://example.org/win32/Firefox%20Setup%20156.0.1.exe\n"
    "  InstallerSha256: A9C641B3E5DBD8B8AB545F3E18CA2C43D740669A8A03E60076CA58A3AC38A84C\n"
    "- Architecture: x64\n"
    "  InstallerUrl: https://example.org/win64/Firefox%20Setup%20156.0.1.exe\n"
    "  InstallerSha256: E69FF3FB4C585F88C947A673DEDCFA6C346A5092881A6E6B618005821030BC67\n"
    "- Architecture: arm64\n"
    "  InstallerUrl: https://example.org/win64-aarch64/Firefox%20Setup%20156.0.1.exe\n"
    "  InstallerSha256: 1CEF5F572D33D6413D1CF44F4B8FB8DDC7A8FC0DACB08C74E7144B2B7F978DD1\n"
    "ManifestType: installer\n"
    "ManifestVersion: 1.12.0\n";

static const char CHROME[] =
    "PackageIdentifier: Google.Chrome\r\n"
    "InstallerType: wix\r\n"
    "Scope: machine\r\n"
    "AppsAndFeaturesEntries:\r\n"
    "- UpgradeCode: '{C1DFDF69-5945-32F2-A35E-EE94C99C7CF4}'\r\n"
    "ElevationRequirement: elevatesSelf\r\n"
    "Installers:\r\n"
    "- Architecture: x86\r\n"
    "  InstallerUrl: https://example.org/chrome.msi\r\n"
    "  InstallerSha256: D3C5D9B99F9C5B6AF88BA68E1063D576E889E218723ADCAEC687D147675BDD13\r\n"
    "  ProductCode: '{0D45BE17-B109-3E7E-B04D-4039339A4F2E}'\r\n"
    "- Architecture: x64\r\n"
    "  InstallerUrl: \"https://example.org/chrome64.msi\"\r\n"
    "  InstallerSha256: 40de51d92ebbc3d2e9b434526ec6f937bf9a62df6de7ca385dd02d0648379051 # lower case\r\n"
    "  ProductCode: '{C8B3A0BF-0F75-32C9-B05B-BE6FE9F5ED90}'\r\n"
    "ManifestType: installer\r\n";

static const char BRAVE[] =
    "PackageIdentifier: Brave.Brave\n"
    "InstallerType: exe\n"
    "ExpectedReturnCodes:\n"
    "- InstallerReturnCode: -2147219440\n"
    "  ReturnResponse: cancelledByUser\n"
    "Installers:\n"
    "  - Architecture: x64\n"
    "    Scope: user\n"
    "    InstallerUrl: https://example.org/BraveSilentSetup.exe\n"
    "    InstallerSha256: 9D8BABEE2F409A8ED11AA162DA2EC30A72BDAA8D2EF2A3BAB19DAA09A8C543E2\n"
    "    InstallModes:\n"
    "    - silent\n"
    "  - Architecture: x64\n"
    "    Scope: machine\n"
    "    InstallerUrl: https://example.org/BraveSetup.exe\n"
    "    InstallerSha256: 825237818B6800270CF3BE49B5C692992F088B6AB7EACD44F46E1745952E615D\n"
    "    InstallModes:\n"
    "    - interactive\n"
    "    - silent\n"
    "    InstallerSwitches:\n"
    "      Silent: /silent /install\n"
    "      SilentWithProgress: /silent /install\n"
    "    ElevationRequirement: elevationRequired\n"
    "ManifestType: installer\n";

static const char LISTING[] =
    "[{\"name\": \"1.9.0\", \"type\": \"dir\"}, {\"name\": \"ESR\", \"type\": \"dir\"},"
    " {\"name\": \"1.10.0\", \"type\": \"dir\"}, {\"name\":\"1.10.0-beta\",\"type\":\"dir\"},"
    " {\"name\": \"de\", \"type\": \"dir\"}, {\"name\": \"1.2.0\", \"type\": \"dir\"}]";

int main(void)
{
    static char buf[16384];
    mf_entry root, list[MF_MAX_ENTRIES];
    char v[64];
    unsigned char sha[32];
    int n, k;

    CHECK(mf_version_cmp("1.10.0", "1.9.0") > 0, "1.10.0 is newer than 1.9.0");
    CHECK(mf_version_cmp("156.0.1", "156.0") > 0, "156.0.1 is newer than 156.0");
    CHECK(mf_version_cmp("2.0", "2.0") == 0, "2.0 is 2.0");
    mf_newest_version(LISTING, v, sizeof(v));
    CHECK(!strncmp(v, "1.10.0", 6), "the newest version folder is 1.10.x, not ESR or de (%s)", v);
    CHECK(!mf_newest_version("[{\"name\": \"ESR\"}]", v, sizeof(v)), "no version folder: none");

    strcpy(buf, FIREFOX);
    n = mf_parse(buf, &root, list, MF_MAX_ENTRIES);
    CHECK(n == 3, "Firefox: 3 installers (%d)", n);
    CHECK(!strcmp(root.type, "nullsoft") && !strcmp(list[1].type, "nullsoft"), "Firefox: nullsoft, inherited by each installer");
    CHECK(!strcmp(list[1].silent, "/S /PreventRebootRequired=true"), "Firefox: the root's Silent switch (%s)", list[1].silent);
    CHECK(!strcmp(list[1].scope, "machine"), "Firefox: machine scope");
    k = mf_pick(list, n, "en-US", 1, 1);
    CHECK(k == 1 && strstr(list[k].url, "/win64/"), "Firefox: a 64-bit PC gets the x64 installer (%d)", k);
    CHECK(mf_pick(list, n, "en-US", 1, 0) == 0, "Firefox: a 32-bit one gets x86");
    CHECK(mf_sha256(list[1].sha, sha) && sha[0] == 0xE6 && sha[31] == 0x67, "Firefox: the SHA-256 reads");

    strcpy(buf, CHROME);
    n = mf_parse(buf, &root, list, MF_MAX_ENTRIES);
    CHECK(n == 2, "Chrome: 2 installers (CRLF lines; AppsAndFeaturesEntries is not one) (%d)", n);
    k = mf_pick(list, n, "de-DE", 1, 1);
    CHECK(k == 1 && !strcmp(list[k].url, "https://example.org/chrome64.msi"), "Chrome: x64, its quoted URL unquoted (%s)", k >= 0 ? list[k].url : "-");
    CHECK(k == 1 && mf_sha256(list[k].sha, sha) && sha[0] == 0x40, "Chrome: a lower-case SHA-256 with a comment after it");
    CHECK(!strcmp(list[1].type, "wix"), "Chrome: wix");

    strcpy(buf, BRAVE);
    n = mf_parse(buf, &root, list, MF_MAX_ENTRIES);
    CHECK(n == 2, "Brave: 2 installers (indented list, InstallModes skipped) (%d)", n);
    CHECK(!strcmp(list[0].scope, "user") && !list[0].silent[0], "Brave: the user installer has no switches");
    CHECK(!strcmp(list[1].silent, "/silent /install") && !strcmp(list[1].scope, "machine"), "Brave: the machine installer's own switches (%s)", list[1].silent);
    CHECK(mf_pick(list, n, "en-US", 0, 1) == 0, "Brave: a standard user gets the per-user installer");
    CHECK(mf_pick(list, n, "en-US", 1, 1) == 1, "Brave: an administrator gets the machine-wide one");

    CHECK(!mf_sha256("abc", sha), "a short SHA-256 is refused");
    CHECK(!mf_sha256("ZZ5D9B99F9C5B6AF88BA68E1063D576E889E218723ADCAEC687D147675BDD13", sha), "a SHA-256 with non-hex digits is refused");
    printf("browser-manifest-test: %s\n", fails ? "FAILED" : "all passed");
    return fails ? 1 : 0;
}
