/* sg-browser -- Get a web browser: reading winget installer manifests.
 * Plain C, no Windows headers, so test/browser-manifest-test.c runs it natively.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_BROWSER_MANIFEST_H
#define SG_BROWSER_MANIFEST_H

typedef struct {
    char arch[16], scope[16], locale[24], type[24], url[2048], sha[80], silent[512];
    char nested[24];            /* NestedInstallerType, for a zip that holds the installer */
    char custom[512];
    char elevation[32];         /* ElevationRequirement: elevationRequired = run it as an administrator */           /* InstallerSwitches Custom: passed as well, whatever the mode (Opera's /allusers=0) */
} mf_entry;

#define MF_MAX_ENTRIES 64

/* dot-separated parts, numbers as numbers: <0, 0, >0 */
int mf_version_cmp(const char *a, const char *b);
/* the newest version folder in a GitHub contents listing (JSON); 0 if none */
int mf_newest_version(const char *json, char *out, int cap);
/* an installer manifest: the root's values and each installer (the root's
 * values filled in under it); returns how many installers. text is changed. */
int mf_parse(char *text, mf_entry *root, mf_entry *list, int max);
/* how well we can run an installer type: 2 = directly (msi, wix, nullsoft,
 * inno, burn, exe), 1 = a zip holding one of those (NestedInstallerType),
 * 0 = not at all (portable, msix, a zip of portable files...) */
int mf_type_rank(const char *type, const char *nested);
/* the installer for this PC: one we can run first (mf_type_rank), then
 * 64-bit, the user's locale, the scope an administrator (machine) or a user
 * (user) installs to; -1 if none fits */
int mf_pick(const mf_entry *list, int n, const char *locale, int admin, int is64);
/* 64 hex digits -> 32 bytes; 0 if not valid */
int mf_sha256(const char *hex, unsigned char out[32]);

#endif
