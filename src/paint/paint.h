/* sg-paint -- Paint for Stained Glass OS: a raster editor in the manner of
 * Windows 10's Paint (mspaint.exe). Our own code and our own drawings.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_PAINT_H
#define SG_PAINT_H

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define ACCENT      RGB(112, 48, 192)
#define ACCENT_HOT  RGB(240, 234, 250)
#define ACCENT_DOWN RGB(226, 212, 246)
#define ACCENT_EDGE RGB(196, 172, 234)
#define RIBBON_BG   RGB(249, 249, 251)
#define WORKSPACE   RGB(214, 211, 224)
#define LINE_GREY   RGB(218, 218, 222)
#define TEXT_GREY   RGB(96, 96, 104)

/* ---- the picture: 32-bit, top-down, alpha ignored (always opaque) ---- */
typedef struct { int w, h; DWORD *px; } Img;

/* tools */
enum { T_SELECT, T_FREESEL, T_PENCIL, T_FILL, T_TEXT, T_ERASER, T_PICKER, T_MAGNIFIER,
       T_BRUSH, T_SHAPE, T_COUNT };
/* brushes */
enum { B_BRUSH, B_CALLI1, B_CALLI2, B_AIRBRUSH, B_MARKER, B_CRAYON, B_COUNT };
/* shapes */
enum { S_LINE, S_RECT, S_ROUNDRECT, S_ELLIPSE, S_TRIANGLE, S_RTRIANGLE, S_DIAMOND, S_PENTAGON,
       S_HEXAGON, S_ARROW_R, S_ARROW_L, S_ARROW_U, S_ARROW_D, S_STAR4, S_STAR5, S_HEART, S_COUNT };

/* commands (ribbon, menus, accelerators) */
enum {
    CMD_NEW = 100, CMD_OPEN, CMD_SAVE, CMD_SAVEAS, CMD_SAVEAS_PNG, CMD_SAVEAS_JPEG, CMD_SAVEAS_BMP,
    CMD_SAVEAS_GIF, CMD_SAVEAS_TIFF, CMD_PROPERTIES, CMD_ABOUT, CMD_EXIT,
    CMD_UNDO, CMD_REDO, CMD_CUT, CMD_COPY, CMD_PASTE, CMD_PASTEFROM, CMD_SELECTALL, CMD_DELETE,
    CMD_INVERTSEL, CMD_TRANSPARENT, CMD_CROP, CMD_RESIZE, CMD_ROT_R, CMD_ROT_L, CMD_ROT_180,
    CMD_FLIP_V, CMD_FLIP_H, CMD_ZOOMIN, CMD_ZOOMOUT, CMD_ZOOM100, CMD_GRID, CMD_STATUSBAR,
    CMD_FULLSCREEN, CMD_EDITCOLORS, CMD_ESCAPE, CMD_FILE_MENU, CMD_SELECT_MENU, CMD_ROTATE_MENU,
    CMD_BRUSH_MENU, CMD_SIZE_MENU, CMD_OUTLINE_MENU, CMD_FILL_MENU, CMD_TEXT_FONT,
    CMD_TEXT_OPAQUE, CMD_TEXT_TRANSPARENT,
    CMD_TAB_HOME, CMD_TAB_VIEW,
    CMD_OUTLINE_NONE, CMD_OUTLINE_SOLID, CMD_FILL_NONE, CMD_FILL_SOLID,
    CMD_TOOL_BASE = 300,                 /* + T_x */
    CMD_BRUSH_BASE = 320,                /* + B_x */
    CMD_SHAPE_BASE = 340,                /* + S_x */
    CMD_SIZE_BASE = 370,                 /* + index 0..3 */
    CMD_COLOR1 = 380, CMD_COLOR2,
    CMD_PALETTE_BASE = 400,              /* + 0..29 */
};

/* the state (main.c) */
extern HINSTANCE g_inst;
extern HWND g_main, g_ribbon, g_canvas, g_status;
extern int g_dpi;
extern HFONT g_font, g_font_small;
extern Img g_img;
extern HDC g_imgdc;                     /* a DC with g_img's DIB selected */
extern int g_tool, g_prev_tool, g_brush, g_shape, g_size_idx;
extern int g_outline_on, g_fill_on;
extern COLORREF g_color1, g_color2, g_palette[30];
extern int g_ncustom;
extern int g_color_sel;                 /* 0: colour 1 is chosen for the palette, 1: colour 2 */
extern int g_zoom;                      /* per mille: 1000 is 100% */
extern int g_grid, g_statusbar_on, g_transparent_sel;
extern int g_dirty;
extern WCHAR g_path[MAX_PATH];
extern int g_tab;                       /* 0 Home, 1 View */
extern int g_text_opaque;
extern LOGFONTW g_text_font;

int S(int v);                           /* scale 96-dpi units */
void set_dirty(void);
void update_title(void);
void layout(void);
void do_command(int cmd);
void set_tool(int tool);
void write_dump(void);
int  tool_size(void);                   /* pixels for the current tool */
extern const int SIZE_PX[4];

/* image.c */
BOOL img_alloc(Img *im, int w, int h, COLORREF fill);
void img_free(Img *im);
BOOL img_copy(Img *dst, const Img *src);
void img_install(Img *im);              /* take over im as the picture (DIB section) */
void img_new(int w, int h);
void undo_push(void);
void undo_drop_top(void);               /* forget the last push (nothing changed) */
const Img *undo_top(void);
void undo_clear(void);
BOOL do_undo(void);
BOOL do_redo(void);
int  undo_count(void);
int  redo_count(void);
DWORD rgb2px(COLORREF c);
COLORREF px2rgb(DWORD p);
void flood_fill(int x, int y, COLORREF c);
void img_rotate(Img *im, int quarter);  /* 1: right 90, 2: 180, 3: left 90 */
void img_flip(Img *im, BOOL vertical);
BOOL img_scale(Img *im, int w, int h);
BOOL img_skew(Img *im, int hdeg, int vdeg, COLORREF bg);
BOOL img_load(const WCHAR *path, Img *out, WCHAR *err, int cch);
BOOL img_save(const WCHAR *path, const Img *im, WCHAR *err, int cch);
BOOL clip_put(const Img *im);
BOOL clip_get(Img *out);

/* canvas.c */
LRESULT CALLBACK canvas_proc(HWND, UINT, WPARAM, LPARAM);
void canvas_changed(BOOL resized);      /* the picture changed: repaint, maybe rescroll */
void canvas_zoom(int zoom, int ax, int ay);   /* anchor in client coords, or -1 */
void sel_commit(void);                  /* paste a floating selection down */
void sel_clear(void);
BOOL sel_active(void);
void sel_rect(RECT *r);
void sel_all(void);
void sel_invert(void);
void sel_delete(void);
BOOL sel_copy(void);
void sel_paste(const Img *im);
void sel_crop(void);
void sel_transform(int op);             /* CMD_ROT_* / CMD_FLIP_* on the selection */
BOOL sel_resize(int w, int h, int hsk, int vsk);
void text_commit(void);
void text_cancel(void);
BOOL text_active(void);
void canvas_cursor_info(int *x, int *y, int *in);
void canvas_escape(void);

/* ribbon.c */
LRESULT CALLBACK ribbon_proc(HWND, UINT, WPARAM, LPARAM);
void ribbon_layout(void);
int  ribbon_height(void);
void ribbon_dump(FILE *f);
LRESULT CALLBACK status_proc(HWND, UINT, WPARAM, LPARAM);
int  status_height(void);

/* glyphs.c */
enum { G_PASTE, G_CUT, G_COPY, G_SELECT, G_FREESEL, G_CROP, G_RESIZE, G_ROTATE, G_PENCIL, G_FILL,
       G_TEXT, G_ERASER, G_PICKER, G_MAGNIFIER, G_BRUSH, G_CALLI1, G_CALLI2, G_AIRBRUSH, G_MARKER,
       G_CRAYON, G_SIZE, G_EDITCOLORS, G_ZOOMIN, G_ZOOMOUT, G_ZOOM100, G_FULLSCREEN, G_OUTLINE, G_FILLSHAPE,
       G_SHAPE_BASE, G_COUNT = G_SHAPE_BASE + S_COUNT };
void glyph_draw(HDC dc, int glyph, int x, int y, int size);
void shape_points(int shape, RECT r, POINT *pts, int *n); /* polygon for the polygonal shapes */

#endif
