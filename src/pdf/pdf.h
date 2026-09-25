/* sg-pdf -- PDF Viewer: what its parts share.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_PDF_H
#define SG_PDF_H

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <wctype.h>

#define APP_NAME L"PDF Viewer"

/* colours: Stained Glass Light */
#define C_BAR      RGB(0xFF, 0xFF, 0xFF)
#define C_LINE     RGB(0xE0, 0xE0, 0xE0)
#define C_CANVAS   RGB(0xE9, 0xE9, 0xEC)
#define C_SIDE     RGB(0xF3, 0xF3, 0xF3)
#define C_TEXT     RGB(0x1F, 0x1F, 0x1F)
#define C_SUBTEXT  RGB(0x60, 0x60, 0x60)
#define C_HOVER    RGB(0xEB, 0xEB, 0xEB)
#define C_PRESS    RGB(0xDD, 0xDD, 0xDD)
#define C_ACCENT   RGB(112, 48, 192)
#define C_ACTIVE   RGB(0xEE, 0xE6, 0xF8)
#define C_SHADOW   RGB(0xC8, 0xC8, 0xCC)
/* highlights are multiplied into the page (DPa), so the text stays black */
#define C_SELECT   RGB(0xB5, 0xD5, 0xFF)
#define C_HIT      RGB(0xFF, 0xEB, 0x3B)
#define C_HITCUR   RGB(0xFF, 0x96, 0x32)

#define WM_APP_RENDERED  (WM_APP + 1)
#define WM_APP_GONE      (WM_APP + 2)

typedef struct { float x1, y1, x2, y2; } frect;

typedef struct {
    frect box;              /* points, top-left origin, page unturned */
    int page;               /* goto: 0-based; -1 for a URI */
    float top;
    WCHAR *uri;
} link_t;

typedef struct {
    double w, h;            /* points */
    /* the rendered page (UI thread only) */
    HBITMAP bmp;
    int bw, bh, brot;
    double bscale;
    HBITMAP thumb;
    int tw, th, trot;
    /* text and links, loaded when first needed */
    BOOL text_loaded, links_loaded;
    WCHAR *text;
    frect *boxes;
    int ntext;
    link_t *links;
    int nlinks;
    /* layout, in document pixels at the current zoom */
    int x, y, dw, dh;
} page_t;

typedef struct { int depth, page; float top; WCHAR *title; } outline_t;
typedef struct { int page; frect box; } hit_t;
typedef struct { int page, pos; } caret_t;

enum { FIT_NONE, FIT_WIDTH, FIT_PAGE };
enum { SIDE_NONE, SIDE_THUMBS, SIDE_OUTLINE };

typedef struct {
    /* the document */
    WCHAR path[MAX_PATH];       /* Windows path */
    WCHAR name[MAX_PATH];
    WCHAR doc_title[256];
    page_t *pages;
    int npages;
    int generation;             /* bumped for each document opened */
    outline_t *outline;
    int noutline;
    WCHAR error[256];
    /* the view */
    double zoom;                /* 1.0 = 100% */
    int fit;
    int rot;                    /* degrees clockwise */
    int sx, sy;                 /* scroll position */
    int docw, doch;             /* laid-out size */
    int current;                /* the page in the middle of the view */
    int side;
    /* search */
    WCHAR needle[256];
    hit_t *hits;
    int nhits, hit;             /* hit: the current one, -1 none */
    BOOL searched;
    /* selection */
    caret_t sel_a, sel_b;
    BOOL selecting, has_sel;
    /* bridge */
    BOOL bridged;
    UINT dpi;
} app_t;

extern app_t g;
extern HWND g_main, g_view, g_bar, g_side, g_tree;
extern HINSTANCE g_inst;
extern HFONT g_font, g_font_small, g_font_bold;

/* main.c */
int dpx(int px);
void app_update_title(void);
void app_layout(void);
void app_dump(void);
BOOL app_open(const WCHAR *path);
void app_status_changed(void);
void app_command(int cmd);

/* bridge.c */
BOOL br_start(void);
int br_request(const char *line, char *head, int headcap, BYTE **payload, DWORD *len);
BOOL br_request_into_dib(const char *line, HBITMAP *out, int *w, int *h);
const char *br_field(const char *head, const char *key, char *buf, int cap);
void render_init(void);
void render_start_thread(void);
LRESULT bridge_on_rendered(LPARAM lp);
void render_want(int page, double scale, int rot, BOOL thumb);
void render_clear_wants(BOOL thumbs);
void to_utf8(const WCHAR *w, char *out, int cap);
WCHAR *from_utf8(const char *s, int len);

/* view.c */
void view_register(void);
void view_relayout(BOOL keep_anchor);
void view_scroll_to(int x, int y);
void view_goto_page(int page, float top);
void view_page_rect(int page, RECT *rc);     /* client coordinates */
double view_scale(void);                     /* pixels a point */
void view_set_zoom(double zoom, int fit, POINT *anchor);
void view_zoom_step(int dir);
void view_find(const WCHAR *needle, int dir);
void view_find_clear(void);
void view_copy(void);
void view_select_all(void);
void view_page_to_client(int page, float x, float y, POINT *pt);
void view_box_to_client(int page, const frect *b, RECT *rc);
BOOL page_load_text(int page);
BOOL page_load_links(int page);
void view_rendered(int page, BOOL thumb, double scale, int rot, int gen, HBITMAP bmp, int w, int h);
void view_free_far(void);

/* side.c */
void side_register(void);
HWND side_create(HWND parent);
void side_set_mode(int mode);
void side_update(void);
void side_load_outline(void);
void side_ensure_visible(int page);

BOOL side_thumb_rect(int i, RECT *out);
BOOL side_tab_center(int k, POINT *pt);

/* print.c */
void print_document(void);

/* toolbar (main.c) */
#define CMD_OPEN 100
#define CMD_PRINT 101
#define CMD_ZOOMIN 102
#define CMD_ZOOMOUT 103
#define CMD_FITWIDTH 104
#define CMD_FITPAGE 105
#define CMD_ROTATE 106
#define CMD_ROTATE_LEFT 107
#define CMD_SIDEBAR 108
#define CMD_FIND 109
#define CMD_FINDNEXT 110
#define CMD_FINDPREV 111
#define CMD_COPY 112
#define CMD_SELECTALL 113
#define CMD_GOTOPAGE 114
#define CMD_ACTUAL 115
#define CMD_OUTLINE 116
#define CMD_THUMBS 117
#define CMD_FIRST 118
#define CMD_LAST 119
#define CMD_PROPERTIES 120

#endif
