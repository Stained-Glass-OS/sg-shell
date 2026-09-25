/* sg-wordpad -- WordPad for Stained Glass OS: a rich-text word processor in
 * the manner of Windows 10's WordPad (wordpad.exe, write.exe), on RichEdit.
 * Our own code and our own drawings.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_WORDPAD_H
#define SG_WORDPAD_H

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <cderr.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#define ACCENT      RGB(112, 48, 192)
#define ACCENT_HOT  RGB(240, 234, 250)
#define ACCENT_DOWN RGB(226, 212, 246)
#define ACCENT_EDGE RGB(196, 172, 234)
#define RIBBON_BG   RGB(249, 249, 251)
#define WORKSPACE   RGB(228, 226, 234)
#define LINE_GREY   RGB(218, 218, 222)
#define TEXT_GREY   RGB(96, 96, 104)

/* commands (ribbon, menus, accelerators) */
enum {
    CMD_NEW = 100, CMD_OPEN, CMD_SAVE, CMD_SAVEAS, CMD_SAVEAS_RTF, CMD_SAVEAS_DOCX, CMD_SAVEAS_ODT,
    CMD_SAVEAS_TXT, CMD_SAVEAS_OTHER, CMD_PRINT, CMD_QUICKPRINT, CMD_PREVIEW, CMD_PAGESETUP,
    CMD_ABOUT, CMD_EXIT, CMD_FILE_MENU, CMD_TAB_HOME, CMD_TAB_VIEW,
    CMD_UNDO, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_PASTESPECIAL, CMD_PASTE_MENU,
    CMD_SELECTALL, CMD_FIND, CMD_REPLACE, CMD_FINDNEXT,
    CMD_FONTFACE, CMD_FONTSIZE, CMD_GROW, CMD_SHRINK, CMD_BOLD, CMD_ITALIC, CMD_UNDERLINE, CMD_STRIKE,
    CMD_SUBSCRIPT, CMD_SUPERSCRIPT, CMD_HIGHLIGHT, CMD_HIGHLIGHT_MENU, CMD_COLOR, CMD_COLOR_MENU,
    CMD_FONTDLG,
    CMD_INDENT_LESS, CMD_INDENT_MORE, CMD_LIST, CMD_LIST_MENU, CMD_SPACING_MENU,
    CMD_ALIGN_LEFT, CMD_ALIGN_CENTER, CMD_ALIGN_RIGHT, CMD_ALIGN_JUSTIFY, CMD_PARADLG, CMD_TABSDLG,
    CMD_PICTURE, CMD_DATETIME,
    CMD_ZOOMIN, CMD_ZOOMOUT, CMD_ZOOM100, CMD_RULER, CMD_STATUSBAR, CMD_WRAP_MENU, CMD_UNITS_MENU,
    CMD_ESCAPE,
    CMD_LIST_BASE = 300,        /* + 0 none, 1 bullet, 2 1., 3 a., 4 A., 5 i., 6 I. */
    CMD_SPACING_BASE = 320,     /* + 0 1.0, 1 1.15, 2 1.5, 3 2.0, 4 add 10pt after */
    CMD_WRAP_BASE = 330,        /* + 0 no wrap, 1 window, 2 ruler */
    CMD_UNITS_BASE = 340,       /* + 0 inches, 1 cm, 2 points, 3 picas */
    CMD_COLOR_BASE = 400,       /* + palette index; CMD_COLOR_BASE + 99 automatic / no colour */
    CMD_HL_BASE = 500,
};
#define PAL_AUTO 99
#define NPAL 20
extern const COLORREF g_pal[NPAL];

/* file formats */
enum { FMT_RTF, FMT_DOCX, FMT_ODT, FMT_TXT, FMT_UTXT, FMT_COUNT };

/* state (main.c) */
extern HINSTANCE g_inst;
extern HWND g_main, g_ribbon, g_ruler, g_edit, g_status, g_face, g_size;
extern int g_dpi;
extern HFONT g_font, g_font_small;
extern int g_tab;                       /* 0 Home, 1 View */
extern int g_ruler_on, g_statusbar_on, g_wrap, g_units;
extern int g_zoom;                      /* percent */
extern COLORREF g_text_color, g_hl_color;
extern int g_text_auto, g_hl_none;
extern WCHAR g_path[MAX_PATH];
extern int g_format;
extern int g_pagew, g_pageh;            /* twips */
extern RECT g_margins;                  /* twips */

int  S(int v);
void layout(void);
void do_command(int cmd);
void write_dump(void);
void update_title(void);
void refresh_state(void);               /* the ribbon's toggles follow the selection */
int  line_width_twips(void);
int  edit_left_px(void);                /* where the text starts in the edit's client area */
void set_indents(int left, int first, int right);   /* twips; -1 = keep */

/* ribbon.c */
LRESULT CALLBACK ribbon_proc(HWND, UINT, WPARAM, LPARAM);
void ribbon_layout(void);
int  ribbon_height(void);
void ribbon_dump(FILE *f);
BOOL ribbon_is_on(int cmd);
LRESULT CALLBACK status_proc(HWND, UINT, WPARAM, LPARAM);
int  status_height(void);
void status_dump(FILE *f);

/* ruler.c */
LRESULT CALLBACK ruler_proc(HWND, UINT, WPARAM, LPARAM);
int  ruler_height(void);
void ruler_dump(FILE *f);
int  twips_to_px(int twips);
int  px_to_twips(int px);

/* glyphs.c */
enum { G_PASTE, G_CUT, G_COPY, G_GROW, G_SHRINK, G_BOLD, G_ITALIC, G_UNDERLINE, G_STRIKE, G_SUB, G_SUPER,
       G_HIGHLIGHT, G_COLOR, G_INDENT_LESS, G_INDENT_MORE, G_LIST, G_SPACING, G_ALEFT, G_ACENTER, G_ARIGHT,
       G_AJUSTIFY, G_PARA, G_PICTURE, G_DATETIME, G_FIND, G_REPLACE, G_SELECTALL, G_ZOOMIN, G_ZOOMOUT,
       G_ZOOM100, G_RULER, G_STATUS, G_WRAP, G_UNITS, G_LAUNCHER, G_COUNT };
void glyph_draw(HDC dc, int glyph, int x, int y, int size);

/* docmodel.c: a document as paragraphs of runs -- what the DOCX and ODT
 * readers make and writers take, and what RTF converts to and from. */
typedef struct {
    WCHAR font[64];
    int hps;                            /* half-points */
    int bold, italic, underline, strike, script;    /* script: 1 super, -1 sub */
    int has_color; COLORREF color;
    int has_hl; COLORREF hl;
} CharProps;

enum { PIC_NONE, PIC_EMF, PIC_WMF, PIC_DIB, PIC_PNG, PIC_JPEG };
typedef struct {
    WCHAR *text; int len;               /* a run of text; '\t' tab, '\v' line break */
    CharProps cp;
    int pic;                            /* PIC_x: a picture instead of text */
    BYTE *data; DWORD size;
    int w, h;                           /* its shown size, twips */
} Run;

enum { AL_LEFT, AL_CENTER, AL_RIGHT, AL_JUSTIFY };
enum { LS_NONE, LS_BULLET, LS_DECIMAL, LS_LALPHA, LS_UALPHA, LS_LROMAN, LS_UROMAN };
typedef struct {
    int align, left, right, first;      /* twips; first is relative to left */
    int before, after;                  /* twips */
    int line;                           /* 240ths of a line: 240 single, 276 1.15 */
    int list;                           /* LS_x */
    int ntabs, tabs[32];
    Run *runs; int nruns, cap;
} Para;

typedef struct { Para *p; int n, cap; } Doc;

void doc_init(Doc *d);
void doc_free(Doc *d);
Para *doc_add_para(Doc *d);
Run *para_add_run(Para *p);
void para_add_text(Para *p, const CharProps *cp, const WCHAR *s, int n);
void cp_default(CharProps *cp);
/* RTF (as RichEdit writes it, and what other programs write) to the model */
BOOL doc_from_rtf(Doc *d, const char *rtf, size_t len);
/* the model to RTF, for EM_STREAMIN; free() the result */
char *doc_to_rtf(const Doc *d, size_t *len);

/* growable byte buffer */
typedef struct { char *p; size_t n, cap; } Buf;
void buf_add(Buf *b, const void *s, size_t n);
void buf_str(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);
void buf_xml(Buf *b, const WCHAR *s, int n);  /* UTF-8, XML-escaped */

/* ooxml.c, odf.c */
BOOL docx_read(const WCHAR *path, Doc *d, WCHAR *err, int cch);
BOOL docx_write(const WCHAR *path, const Doc *d, WCHAR *err, int cch);
BOOL odt_read(const WCHAR *path, Doc *d, WCHAR *err, int cch);
BOOL odt_write(const WCHAR *path, const Doc *d, WCHAR *err, int cch);

/* xml.c: a small non-validating XML reader */
typedef struct XNode {
    char *name;                         /* element name with its prefix, or NULL for text */
    char **attr; int nattr;             /* name, value pairs (unescaped, UTF-8) */
    char *text;                         /* text nodes */
    struct XNode **kids; int nkids;
} XNode;
XNode *xml_parse(const char *s, size_t n);
void xml_free(XNode *x);
const char *xml_attr(const XNode *x, const char *name);     /* also matches any prefix: "w:val" or "val" */
const char *xml_local(const char *name);                     /* the part after ':' */
XNode *xml_child(const XNode *x, const char *local);

/* picture.c */
BOOL pic_load_file(const WCHAR *path, BYTE **dib, DWORD *dibsize, int *wpx, int *hpx);
BOOL pic_to_png(int type, const BYTE *data, DWORD size, int w_twips, int h_twips, BYTE **png, DWORD *pngsize,
                int *wpx, int *hpx);
BOOL pic_decode_to_dib(const BYTE *data, DWORD size, BYTE **dib, DWORD *dibsize, int *wpx, int *hpx);

/* print.c */
void print_document(BOOL dialog);
void print_preview(void);
void page_setup(void);

/* dialogs.c */
void dlg_datetime(void);
void dlg_paragraph(void);
void dlg_tabs(void);
void find_open(BOOL replace);
void find_next(void);
extern UINT g_findmsg;
LRESULT on_find_msg(LPARAM lp);
extern HWND g_finddlg;

#endif
