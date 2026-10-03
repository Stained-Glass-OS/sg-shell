/* SG Store -- an application store for Stained Glass OS.
 *
 * A catalogue of applications the OS can install on demand. For any app the
 * Windows (wine-sg) build is the ideal path and the default suggestion; our
 * own builds (SG Office, our browser builds) sit beside them; native Linux
 * desktop apps are a hidden last resort, shown only behind an "Advanced"
 * disclosure. The store checks for and installs per-app updates.
 *
 * We never ship or redistribute a third-party or Microsoft binary: the store
 * downloads the maker's installer at the user's request and runs it only if it
 * is exactly the file the maker published (a pinned SHA-256, or the winget
 * community repository's -- the same engine as "Get a web browser",
 * src/browser/fetch.c). Our own builds come from our apt repo / freesoft.page.
 *
 * OS updates are NOT handled here: they keep their own path (Settings >
 * Update & Security). The store never touches the system upgrade.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_STORE_H
#define SG_STORE_H

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
#include "browser.h"      /* package_t, pkg_resolve/download/install, winget_* */

#define STORE_KEY L"Software\\Stained Glass\\Store"

/* Tiers, in the order the UI prefers them. */
enum { TIER_WINDOWS, TIER_OURS, TIER_LINUX };

/* How an entry is installed and update-checked. */
enum { SRC_WINGET, SRC_PIN, SRC_OURS_APT, SRC_LINUX_APT, SRC_UNKNOWN };

/* Per-app state. */
enum { AST_NOT_INSTALLED, AST_INSTALLED, AST_UPDATE, AST_INSTALLING, AST_DONE, AST_FAILED, AST_REMOVING };

typedef struct {
    WCHAR ord[8];                 /* the catalogue ordinal ("01") */
    WCHAR name[128], publisher[128], desc[256], category[64];
    DWORD colour;                 /* 0x00RRGGBB, letter badge -- no logos */
    int   tier;                   /* TIER_* */
    int   method;                 /* SRC_* */
    WCHAR winget_id[128];         /* SRC_WINGET */
    WCHAR apt_pkg[128];           /* SRC_OURS_APT / SRC_LINUX_APT: the system package */
    /* SRC_PIN: a vendor download pinned in the catalogue */
    WCHAR pin_url[2048], pin_type[32], pin_silent[512], pin_version[64];
    BYTE  pin_sha[32];
    BOOL  pin_has_sha;
    /* how to tell it is installed and read its version: a Windows program
     * by the Uninstall entries' DisplayName (detect_name), a Linux one by
     * dpkg's state of apt_pkg -- never the other way round */
    WCHAR detect_name[128];       /* "Name|Other|!Not this" -- see name_matches() */
    WCHAR run[260];               /* what Open starts: SRC_LINUX_APT, a Unix program;
                                   * SRC_OURS_APT, a Windows program (App Paths name) */
    WCHAR icon_url[512];          /* Icon: its picture (https: or a file) -- icons.c */
    /* one app, two builds: a Windows program's Linux = the ordinal of the
     * same app's Linux build. The pair is one card, with a choice of build;
     * Prefer = linux (the default) or windows names the one it suggests. */
    WCHAR linux_ord[8];
    BOOL  prefer_windows;
    int   alt;                    /* the pair's other entry (index), or -1 */
    BOOL  is_alt;                 /* the Linux half of a pair: not a card of its own */
    BOOL  use_alt;                /* the card's choice: the Linux build */
    /* runtime */
    int   state;                  /* AST_* */
    WCHAR installed_version[64];
    WCHAR available_version[64];
    WCHAR msg[256];
    BOOL  msg_error;              /* msg says what went wrong (an uninstall that failed) */
    BOOL  ran_elevated;           /* its installer was started as an administrator */
    BOOL  queued;                 /* waiting for the install before it */
    BOOL  checked;                /* the card is checked: Install selected installs it */
} app_t;

#define MAX_APPS 512   /* the catalogue: 200-odd apps, Linux ones among them */

/* The categories, in the order the store shows them; Linux apps come last,
 * in a section of their own. */
#define LINUX_SECTION L"Linux apps"

/* catalog.c */
BOOL name_matches(const WCHAR *display, const WCHAR *patterns);
BOOL dpkg_installed(const WCHAR *pkg, WCHAR *version, int cch);
int  catalog_load(app_t *apps, int max);         /* reads HKLM Store\Apps\NN; returns count */
void app_detect(app_t *a);                        /* installed? + installed_version; sets state */
BOOL app_launch_target(const app_t *a, WCHAR *out, int cch); /* the program (or its Start shortcut) Open starts */
BOOL app_check_update(app_t *a, WCHAR *err, int cch); /* fills available_version; AST_UPDATE if newer */
const WCHAR *app_section(const app_t *a);         /* its category, or LINUX_SECTION */
int  app_install(app_t *a, progress_fn progress, void *ctx, volatile LONG *cancel, WCHAR *err, int cch);
int  app_uninstall(app_t *a, WCHAR *err, int cch);  /* 0: removed (as its uninstaller or apt says) */
const WCHAR *tier_name(int tier);

/* icons.c: the cards' pictures, fetched on a few threads; msg is posted to
 * notify as each is ready (wParam 1: all done) */
void    icons_fetch(const app_t *apps, int n, int px, HWND notify, UINT msg);
HBITMAP icon_for(const app_t *a);
int     icons_ready(void);

/* sysinstall.c: system packages (Linux apps, .deb files) through the
 * administrator's consent and sg-admind */
enum { SYS_OK = 0, SYS_DENIED = 1, SYS_FAILED = 10, SYS_CANCELLED = 11 };
int  sys_install_apt(const app_t *a, WCHAR *err, int cch);     /* from the store: elevate, wait */
int  sys_remove_apt(const app_t *a, WCHAR *err, int cch);      /* Uninstall: elevate, wait */
int  sys_elevated_main(int argc, WCHAR **argv, int i);          /* --elevated-apt / -apt-remove / -deb */
void sys_batch_begin(void);                                     /* several apps, one consent */
void sys_batch_end(void);
int  sys_helper_main(const WCHAR *file);                        /* --elevated-helper FILE */
int  sys_deb_window(HINSTANCE inst, const WCHAR *file);         /* --deb FILE */
BOOL sys_unix_path(const WCHAR *dos, char *out, int cch);
void sys_run_linux(const WCHAR *unix_path);

#endif
