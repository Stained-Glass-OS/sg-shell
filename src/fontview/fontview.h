/* sg-fontview -- the font viewer (fontview.exe) and the Fonts folder.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_FONTVIEW_H
#define SG_FONTVIEW_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include "fontinfo.h"

#define COL_BG      RGB(0xFF, 0xFF, 0xFF)
#define COL_TEXT    RGB(0x1A, 0x1A, 0x1A)
#define COL_SUBTLE  RGB(0x60, 0x60, 0x60)
#define COL_RULE    RGB(0xDD, 0xDD, 0xDD)
#define COL_ACCENT  RGB(0x70, 0x30, 0xC0)
#define COL_SELECT  RGB(0xE8, 0xDD, 0xF7)
#define COL_HOT     RGB(0xF3, 0xEE, 0xFB)
#define COL_BAR     RGB(0xF5, 0xF6, 0xF8)
#define COL_TITLE   RGB(0x1E, 0x32, 0x87)

extern HINSTANCE g_inst;
extern int g_dpi;
int S(int dip);

/* ---- a font file, read (fontlib.c) ------------------------------------------------ */
struct font_file {
    WCHAR path[MAX_PATH];
    struct fi_file info;
    DWORD size;
};
BOOL font_read(const WCHAR *path, struct font_file *out);      /* FALSE: not a font */
void utf8_to_w(const char *s, WCHAR *out, int cch);

/* ---- installing (fontlib.c) ---------------------------------------------------------- */
enum { SCOPE_NONE = 0, SCOPE_USER = 1, SCOPE_MACHINE = 2 };
/* The results: 0 done, or a Windows error (ERROR_ACCESS_DENIED for a standard
 * user asking for all users, ERROR_FILE_EXISTS, ERROR_BAD_FORMAT ...);
 * msg says it in words. */
DWORD font_install(const WCHAR *path, int scope, WCHAR *msg, int cch);
DWORD font_uninstall(const WCHAR *path_or_name, int scope, WCHAR *msg, int cch);
/* where a file with this registry name / file name is installed: SCOPE_* bits */
int  font_installed_scope(const struct font_file *f);
BOOL font_is_elevated(void);
/* the per-user fonts folder, %LOCALAPPDATA%\Microsoft\Windows\Fonts */
void font_user_dir(WCHAR *out, int cch);
/* the Linux per-user fonts folder (for fontconfig), as a Windows path */
BOOL font_linux_user_dir(WCHAR *out, int cch);

/* ---- installed families (fontlib.c) --------------------------------------------------- */
struct family_file {
    WCHAR regname[256];     /* the registry value */
    WCHAR path[MAX_PATH];   /* the file, full path */
    int scope;              /* SCOPE_USER, SCOPE_MACHINE, or 0: a system font (Linux, Wine's own) */
};
struct family {
    WCHAR name[LF_FACESIZE];
    int styles;             /* how many faces GDI enumerates */
    BOOL symbol, vertical_hidden;
    struct family_file *files;
    int nfiles;
    int scope;              /* the "widest" file scope; 0 for system fonts */
    BYTE charset;
    BOOL truetype, raster;
};
int  families_load(struct family **out);          /* sorted; families_free after */
void families_free(struct family *f, int n);
void family_files(struct family *f);             /* fills files from the Fonts registry keys */

/* ---- windows ----------------------------------------------------------------------- */
int  viewer_main(const WCHAR *path, const WCHAR *family, int show);
int  folder_main(int show);
BOOL viewer_print(const WCHAR *path, HWND owner, BOOL ask);
void dump_open(void);           /* SG_FONTVIEW_DUMP: rewritten after changes */
extern WCHAR g_dump_path[MAX_PATH];
void message_box(HWND owner, const WCHAR *text, BOOL error);
BOOL run_self(HWND owner, const WCHAR *args, BOOL elevated, DWORD *exit_code);

#endif
