/* sg-browser -- Get a web browser: what its parts share.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_BROWSER_H
#define SG_BROWSER_H

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include "manifest.h"

#define BROWSERS_KEY L"Software\\Stained Glass\\Web Browsers"

enum { STAGE_FIND, STAGE_DOWNLOAD, STAGE_INSTALL, STAGE_DONE, STAGE_FAILED };

typedef struct {
    WCHAR id[128], version[64], url[2048], type[32], scope[16], silent[512], arch[16];
    WCHAR nested[32];           /* a zip's NestedInstallerType (SG Store unpacks those) */
    WCHAR custom[512];          /* the manifest's Custom switches, passed after the silent ones */
    WCHAR deps[512];            /* the packages it needs first (winget Dependencies), space-separated */
    BOOL elevate;               /* ElevationRequirement: elevationRequired -- ask for an administrator first */
    BOOL elevated;              /* the installer was started as an administrator (runas) */
    BYTE sha256[32];
    WCHAR file[MAX_PATH];       /* the download */
    WCHAR command[1024];        /* the installer's arguments, as run */
    ULONGLONG size;
    DWORD exit_code;
} package_t;

typedef void (*progress_fn)(void *ctx, int stage, ULONGLONG done, ULONGLONG total);

BOOL pkg_resolve(const WCHAR *id, package_t *p, WCHAR *err, int cch);
BOOL pkg_download(package_t *p, progress_fn progress, void *ctx, volatile LONG *cancel, WCHAR *err, int cch);
BOOL pkg_install(package_t *p, WCHAR *err, int cch);
/* set by the store while it installs several apps under one consent: an
 * installer that must run as an administrator is run by this instead of
 * "runas" (its exit code to *code); FALSE: it could not be run */
extern BOOL (*g_runas_hook)(const WCHAR *file, const WCHAR *args, DWORD *code);
void pkg_cleanup(package_t *p);
BOOL winget_path(WCHAR *out, int cch);
BOOL winget_install(const WCHAR *winget, package_t *p, WCHAR *err, int cch);

#endif
