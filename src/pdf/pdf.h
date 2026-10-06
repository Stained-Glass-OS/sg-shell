/* sg-pdf -- SG PDF: what its parts share.
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

#define APP_NAME L"SG PDF"

/* colours: Stained Glass Light and Dark (the app mode, sg-mode.h) */
typedef struct {
    COLORREF bar, line, canvas, side, text, subtext, hover, press, accent, active, shadow, pane, edit, disabled;
} palette_t;
extern palette_t P;
#define C_BAR      (P.bar)
#define C_LINE     (P.line)
#define C_CANVAS   (P.canvas)
#define C_SIDE     (P.side)
#define C_TEXT     (P.text)
#define C_SUBTEXT  (P.subtext)
#define C_HOVER    (P.hover)
#define C_PRESS    (P.press)
#define C_ACCENT   (P.accent)
#define C_ACTIVE   (P.active)
#define C_SHADOW   (P.shadow)
#define C_PANE     (P.pane)
#define C_EDITBG   (P.edit)
#define C_DISABLED (P.disabled)
/* highlights are multiplied into the page (DPa), so the text stays black */
#define C_SELECT   RGB(0xB5, 0xD5, 0xFF)
#define C_HIT      RGB(0xFF, 0xEB, 0x3B)
#define C_HITCUR   RGB(0xFF, 0x96, 0x32)
#define C_FIELD    RGB(0xDD, 0xE6, 0xFF)
#define C_REDMARK  RGB(0xD0, 0x10, 0x10)

#define WM_APP_RENDERED  (WM_APP + 1)
#define WM_APP_GONE      (WM_APP + 2)

typedef struct { float x1, y1, x2, y2; } frect;

typedef struct {
    frect box;              /* points, top-left origin, page unturned */
    int page;               /* goto: 0-based; -1 for a URI */
    float top;
    WCHAR *uri;
} link_t;

/* Edit PDF: a page's objects, as sg-pdf's "objects" reports them */
enum { OBJ_TEXT, OBJ_IMAGE, OBJ_PATH };
typedef struct {
    int kind, id;
    frect box;
    WCHAR font[64];
    float size, lh;
    COLORREF color;
    int style;              /* 1 bold, 2 italic */
    int align;
    int xref;
    WCHAR *text;
} obj_t;

/* Comment: a page's annotations */
typedef struct {
    int xref;
    char type[16];          /* Highlight, Text, FreeText, Square, ... Redact */
    frect box;
    COLORREF color;
    WCHAR *author, *contents;
    int replies;            /* replies to it (status changes not counted) */
    char status[16];        /* its review status: Accepted, Rejected, ... or "" */
} annot_t;

/* Fill & Sign: the document's form fields */
enum { FLD_TEXT, FLD_CHECK, FLD_RADIO, FLD_COMBO, FLD_LIST, FLD_BUTTON, FLD_SIGNATURE, FLD_OTHER };
typedef struct {
    int page, xref, type, flags;
    frect box;
    float fontsize;
    WCHAR *name, *value, *options;  /* options: '\n' between them */
    WCHAR *fmt, *calc, *tooltip;    /* "number:2:$", "sum:a,b", ... (sgpdf_forms) */
} field_t;
#define FF_READONLY 1
#define FF_REQUIRED 2
#define FF_MULTILINE 4096

/* certificate signatures: the document's signature fields and their state */
enum { SIG_UNSIGNED, SIG_VALID, SIG_UNKNOWN, SIG_INVALID };
typedef struct {
    int page, xref, state;
    BOOL covers;            /* the signature covers the whole file */
    frect box;
    WCHAR *name, *signer, *time, *reason, *detail;
} sig_t;

/* attachments */
typedef struct { WCHAR *key, *file, *desc; DWORD size; } attach_t;

typedef struct {
    double w, h;            /* points */
    /* the rendered page (UI thread only) */
    HBITMAP bmp;
    int bw, bh, brot;
    double bscale;
    HBITMAP thumb;
    int tw, th, trot;
    /* a page too large to draw whole at this zoom: the part in view, sharp (the whole page is
     * drawn smaller under it) */
    HBITMAP tile;
    int tlx, tly, tlw, tlh, tlrot, tlgen;
    double tlscale;
    BOOL stale, tstale;     /* drawn before the last change: shown until redrawn */
    /* text and links, loaded when first needed */
    BOOL text_loaded, links_loaded;
    WCHAR *text;
    frect *boxes;
    int ntext;
    link_t *links;
    int nlinks;
    /* the editor's view of the page, loaded when first needed */
    BOOL objs_loaded, annots_loaded;
    obj_t *objs;
    int nobjs;
    annot_t *annots;
    int nannots;
    /* layout, in document pixels at the current zoom */
    int x, y, dw, dh;
} page_t;

typedef struct { int depth, page; float top; WCHAR *title; } outline_t;
typedef struct { int page; frect box; } hit_t;
typedef struct { int page, pos; } caret_t;

enum { FIT_NONE, FIT_WIDTH, FIT_PAGE };
enum { SIDE_NONE, SIDE_THUMBS, SIDE_OUTLINE, SIDE_ATTACH, SIDE_SIGS };
/* page display: one column scrolling (the default), two pages side by side scrolling, and the page
 * (or the two pages) at a time */
enum { LAYOUT_CONT, LAYOUT_TWOCONT, LAYOUT_SINGLE, LAYOUT_TWO };

/* the tools (the right-hand pane) and what each does with the mouse */
enum { TOOL_NONE, TOOL_EDIT, TOOL_COMMENT, TOOL_FILL, TOOL_REDACT, TOOL_ORGANIZE, TOOL_FORM, TOOL_COUNT };
enum {
    SUB_SELECT = 0,
    SUB_ADDTEXT, SUB_ADDIMAGE,                                          /* Edit PDF */
    SUB_NOTE, SUB_HIGHLIGHT, SUB_UNDERLINE, SUB_STRIKE, SUB_FREETEXT,   /* Comment */
    SUB_RECT, SUB_ELLIPSE, SUB_ARROW, SUB_LINE, SUB_INK,
    SUB_FILLTEXT, SUB_SIGN,                                             /* Fill & Sign */
    SUB_MARKTEXT, SUB_MARKAREA,                                         /* Redact */
    SUB_STAMP, SUB_LINK,                                                /* Comment, Edit PDF */
    SUB_MARK_CHECK, SUB_MARK_CROSS, SUB_MARK_DOT, SUB_CERTSIGN,         /* Fill & Sign */
    SUB_F_TEXT, SUB_F_DATE, SUB_F_NUMBER, SUB_F_CHECK, SUB_F_RADIO,     /* Prepare Form */
    SUB_F_COMBO, SUB_F_LIST, SUB_F_SIGN,
    SUB_LAST = SUB_F_SIGN
};

/* what is picked (Edit PDF: an object; Comment and Redact: an annotation) */
enum { PICK_NONE, PICK_OBJ, PICK_ANNOT, PICK_FIELD };
typedef struct { int kind, page, index; } pick_t;

typedef struct {
    /* the document */
    WCHAR path[MAX_PATH];       /* Windows path */
    WCHAR name[MAX_PATH];
    WCHAR doc_title[256];
    page_t *pages;
    int npages;
    int generation;             /* bumped for each document opened, and each change */
    outline_t *outline;
    int noutline;
    WCHAR error[256];
    /* the document's state, as sg-pdf reports it after each change */
    int undo, redo;
    BOOL dirty, encrypted, form;
    unsigned perms;
    char protect[16];
    int nredact;
    field_t *fields;
    int nfields;
    BOOL fields_loaded;
    sig_t *sigs;
    int nsigs;
    BOOL sigs_loaded;
    attach_t *attach;
    int nattach;
    BOOL attach_loaded;
    BOOL untitled;              /* made here (Create PDF), not saved yet */
    /* the view */
    double zoom;                /* 1.0 = 100% */
    int fit;
    int rot;                    /* degrees clockwise */
    int sx, sy;                 /* scroll position */
    int docw, doch;             /* laid-out size */
    int current;                /* the page in the middle of the view */
    int side;
    BOOL pane;                  /* the tools pane on the right */
    int layout;                 /* LAYOUT_x */
    BOOL cover;                 /* two pages: the first page alone, as a book's cover */
    BOOL night;                 /* the pages drawn dark (their colours inverted) */
    BOOL reading;               /* Read Out Loud is speaking */
    BOOL home;                  /* the Home tab is in front (it is anyway with no document) */
    BOOL menu_open;             /* the Menu is down (the dump says so) */
    /* search */
    WCHAR needle[256];
    hit_t *hits;
    int nhits, hit;             /* hit: the current one, -1 none */
    BOOL searched;
    /* selection */
    caret_t sel_a, sel_b;
    BOOL selecting, has_sel;
    /* the tools */
    int tool, sub;
    pick_t pick;
    WCHAR fmt_font[64];         /* Edit PDF / Add text: the format for new text */
    float fmt_size;
    COLORREF fmt_color;
    int fmt_style, fmt_align;
    COLORREF ccolor;            /* Comment: the colour */
    int sig_kind;               /* Fill & Sign: the signature to place (0 none, 1 ink, 2 text, 3 image) */
    WCHAR *sig_data;
    WCHAR image_path[MAX_PATH]; /* Edit PDF: the picture to place */
    int stamp;                  /* Comment: the stamp to place (STAMP_NAMES) */
    /* Fill & Sign with a certificate: the digital ID and what the signature says */
    WCHAR cert_file[MAX_PATH], cert_pw[128], cert_reason[128], cert_location[128];
    int cert_field;             /* the empty signature field to sign (xref), 0: where the page is clicked */
    WCHAR status[256];          /* the last message (an edit refused ...) */
    /* organize */
    BOOL *org_sel;
    int org_anchor;
    /* bridge */
    BOOL bridged;
    UINT dpi;
    BOOL dark;
} app_t;

extern app_t g;
extern HWND g_main, g_view, g_bar, g_side, g_tree, g_tbar, g_pane, g_org;
extern HINSTANCE g_inst;
extern HFONT g_font, g_font_small, g_font_bold, g_font_title;

/* main.c */
int dpx(int px);
void app_update_title(void);
void app_layout(void);
void app_dump(void);
BOOL app_open(const WCHAR *path);
void app_status_changed(void);
void app_command(int cmd);
void app_set_status(const WCHAR *fmt, ...);
char *unix_path(const WCHAR *path);
void app_apply_mode(void);
BOOL app_can(unsigned perm);

/* bridge.c */
BOOL br_start(void);
int br_request(const char *line, char *head, int headcap, BYTE **payload, DWORD *len);
BOOL br_request_into_dib(const char *line, HBITMAP *out, int *w, int *h, int *x, int *y);
const char *br_field(const char *head, const char *key, char *buf, int cap);
void render_init(void);
void render_start_thread(void);
LRESULT bridge_on_rendered(LPARAM lp);
void render_want(int page, double scale, int rot, BOOL thumb);
void render_want_tile(int page, double scale, int rot, int x, int y, int w, int h);
#define TILE_PIXELS 4000000     /* a page bitmap larger than this is drawn in tiles */
void render_clear_wants(BOOL thumbs);
void to_utf8(const WCHAR *w, char *out, int cap);
WCHAR *from_utf8(const char *s, int len);
char *esc_utf8(const WCHAR *w);         /* UTF-8 with \t \n \r \\ escaped; free it */
WCHAR *unesc_utf8(const char *s, int len);

/* doc.c: requests that change the document, and what they change */
extern char g_last_head[1024];
BOOL doc_request(const char *line);     /* TRUE: done; FALSE: refused (g.status says why) */
BOOL doc_requestf(const char *fmt, ...);
void doc_apply_state(const char *head, const BYTE *data, DWORD len);
void doc_free_page_cache(page_t *p);
BOOL doc_load_objects(int page);
BOOL doc_load_annots(int page);
BOOL doc_load_fields(void);
void doc_free_fields(void);
BOOL doc_save(BOOL save_as);
BOOL doc_close_prompt(void);           /* FALSE: the user cancelled */
void doc_undo(int dir);
void doc_pages_spec(char *out, int cap, const int *pages, int n);

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
void view_client_to_page(int page, int cx, int cy, float *x, float *y);
int view_page_at(POINT pt, BOOL clamp);      /* the page under a client point, -1 none */
void view_box_to_client(int page, const frect *b, RECT *rc);
BOOL page_load_text(int page);
BOOL page_load_links(int page);
void view_rendered(int page, BOOL thumb, double scale, int rot, int gen, HBITMAP bmp, int w, int h);
BOOL view_page_ready(int i);
void view_rendered_tile(int page, double scale, int rot, int gen, HBITMAP bmp, int x, int y, int w, int h);
void view_free_far(void);
int view_selection_rects(int page, frect *out, int cap);  /* the selection on a page, one rect a line */
BOOL view_has_selection(void);
BOOL view_over_text(POINT pt);
void view_clear_selection(void);
void view_set_layout(int layout, BOOL cover);

/* side.c */
void side_register(void);
HWND side_create(HWND parent);
void side_set_mode(int mode);
void side_update(void);
void side_load_outline(void);
void side_ensure_visible(int page);
BOOL side_thumb_rect(int i, RECT *out);
BOOL side_tab_center(int k, POINT *pt);
void side_apply_mode(void);
void side_fonts(void);
void side_dump(FILE *f);

/* print.c */
void print_document(void);

/* the glyphs drawn for buttons, cards and the rail (toolui.c, soft-edged with sg-smooth.h) */
enum {
    T_NONE, T_SELECT, T_TEXT, T_IMAGE, T_DELETE, T_REPLACE, T_NOTE, T_HIGHLIGHT, T_UNDERLINE, T_STRIKE, T_TEXTBOX,
    T_RECT, T_ELLIPSE, T_ARROW, T_LINE, T_PEN, T_SIGN, T_FLATTEN, T_MARK, T_AREA, T_FIND, T_APPLY, T_CLEAN, T_ROTL,
    T_ROTR, T_BLANK, T_INSERT, T_EXTRACT, T_SPLIT, T_CLOSE, T_EDIT, T_COMMENT, T_FILL, T_REDACT, T_ORGANIZE,
    T_EXPORT, T_COMBINE, T_PROTECT, T_FORM, T_FTEXT, T_FDATE, T_FNUM, T_FCHECK, T_FRADIO, T_FCOMBO, T_FLIST, T_FSIGN,
    T_DETECT, T_PROPS, T_STAMP, T_LINK, T_CHECKMARK, T_CROSS, T_DOT, T_CERT, T_OCR, T_HEADER, T_WATERMARK, T_BATES,
    T_REPLACE_PAGE, T_SCAN, T_MENU, T_HOME, T_UP, T_DOWN, T_PLUS, T_MINUS, T_FITG, T_OPENFILE, T_ALLTOOLS, T_BACK,
    T_COMPRESS, T_CREATE,
};
void pdf_glyph(HDC dc, int k, int cx, int cy, int s, COLORREF col, COLORREF accent);

/* toolui.c: the tools pane and each tool's bar */
void toolui_register(void);
void toolui_create(HWND parent);
void tool_set(int tool);
void tool_set_sub(int sub);
void toolui_update(void);                /* the state changed: enable, check, refresh lists */
void toolui_layout(void);
int toolui_bar_height(void);
int toolui_pane_width(void);
void toolui_dump(FILE *f);
void toolui_format_from_pick(void);
const WCHAR *tool_name(int tool);
void toolui_fonts(void);

/* interact.c: the mouse and the keyboard on the pages, by tool */
BOOL tool_mouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
BOOL tool_key(WPARAM vk);
BOOL tool_setcursor(POINT pt);
void tool_paint(HDC dc);
void tool_cancel(void);
void tool_dump(FILE *f);
BOOL tool_editor_open(void);
void tool_commit_editor(void);
void tool_delete_pick(void);
void tool_after_change(void);
void fields_paint(HDC dc, int page);
void comment_thread(int page, int index);

/* dialogs.c */
BOOL dlg_text(HWND owner, const WCHAR *title, const WCHAR *prompt, WCHAR *buf, int cap, BOOL multiline);
BOOL dlg_signature(HWND owner);
void dlg_protect(void);
void dlg_export_as(int kind);        /* 0: ask; 1 docx, 2 txt, 3 png, 4 jpeg, 5 html */
void dlg_combine(void);
void dlg_properties(void);
void dlg_find_redact(void);
void dlg_sanitize(BOOL after_apply);
void dlg_split(void);
BOOL dlg_permissions_password(void);
BOOL file_dialog(BOOL save, const WCHAR *title, const WCHAR *filter, const WCHAR *defext, WCHAR *out, int cap);
BOOL file_dialog_multi(const WCHAR *title, const WCHAR *filter, WCHAR ***files, int *n);

/* form.c: Prepare Form (making a form's fields) */
BOOL form_mouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
BOOL form_key(WPARAM vk);
BOOL form_setcursor(POINT pt);
void form_paint(HDC dc);
void form_dump(FILE *f);
void form_command(int cmd);
BOOL form_props(int index);           /* the field's Properties dialog */
void form_pick(int index);
int form_picked(void);                /* the picked field's index, -1 none */
const WCHAR *field_kind_name(const field_t *f);

/* sign.c: certificate signatures */
BOOL dlg_certsign(int field_xref);    /* choose the digital ID; TRUE: click where it goes (or the field is signed) */
BOOL sign_at(int page, frect r);      /* sign in a new box; asks where to save */
BOOL doc_load_sigs(void);
void doc_free_sigs(void);
void dlg_makeid(HWND owner, WCHAR *out_path, int cap);
void sig_banner(WCHAR *out, int cap, int *level);   /* "Signed and all signatures are valid." ... */
void sig_show(int index);             /* a signature's details */
extern HWND g_sigbar;                 /* the line over the pages: "Signed and all signatures are valid." */
void sigbar_create(HWND parent);
int sigbar_height(void);
void sigbar_update(void);

/* create.c: Create PDF, Recognize Text, headers and footers, watermarks, Bates numbers, optimize,
 * attachments, Read Out Loud */
void create_command(int cmd);
BOOL app_adopt(const char *head, const BYTE *data, DWORD len, const WCHAR *name);
BOOL doc_load_attach(void);
void doc_free_attach(void);
void attach_save(int index);
void attach_open(int index);
void attach_delete(int index);
void read_aloud(int page, BOOL to_end);
void read_stop(void);
void link_create(int page, frect box);
extern const WCHAR *const STAMP_LABELS[];
extern const char *const STAMP_NAMES[];
#define NSTAMPS 14

/* home.c: the tab strip and Menu, Home (tools gallery, recent files), the quick tools rail, the floating page
 * controls */
extern HWND g_tabs, g_home, g_rail, g_float;
void frame_create(HWND parent);
void float_create(HWND parent);
void frame_update(void);
void frame_dump(FILE *f);
BOOL home_shown(void);
void home_switch(BOOL home);
void home_card(int index);
void recent_add(const WCHAR *path);
void menu_popup(int index);
void rail_command(int index);
int tabbar_height(void);
int rail_width(void);
int float_width(void);
int float_height(void);
void shortcuts_help(void);

/* organize.c */
void org_register(void);
HWND org_create(HWND parent);
void org_show(BOOL on);
void org_update(void);
void org_dump(FILE *f);
void org_command(int cmd);
int org_selected(int *out, int cap);

/* commands (the menu, the bars, the keyboard) */
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
#define CMD_SAVE 121
#define CMD_SAVEAS 122
#define CMD_UNDO 123
#define CMD_REDO 124
#define CMD_CLOSE 125
#define CMD_EXIT 126
#define CMD_PANE 127
#define CMD_ABOUT 128
#define CMD_NEXTPAGE 129
#define CMD_PREVPAGE 130
/* tools: CMD_TOOL + TOOL_x */
#define CMD_TOOL 140
#define CMD_EXPORT 150
#define CMD_EXPORT_DOCX 151
#define CMD_EXPORT_TXT 152
#define CMD_EXPORT_PNG 153
#define CMD_EXPORT_JPEG 154
#define CMD_EXPORT_HTML 155
#define CMD_COMBINE 156
#define CMD_PROTECT 157
#define CMD_UNPROTECT 158
#define CMD_UNLOCK 159
#define CMD_SANITIZE 160
#define CMD_FINDREDACT 161
#define CMD_APPLYREDACT 162
#define CMD_FLATTEN 163
#define CMD_SIGN 164
#define CMD_DELETE 165
#define CMD_REPLACEIMAGE 166
#define CMD_TOOLCLOSE 167
/* subtools: CMD_SUB + SUB_x (to CMD_SUB + SUB_LAST, below CMD_COLOR) */
#define CMD_SUB 170
/* comment colours: CMD_COLOR + index */
#define CMD_COLOR 220
#define NCOLORS 6
/* Create PDF and the whole-document tools */
#define CMD_CREATE_BLANK 230
#define CMD_CREATE_FILES 231
#define CMD_CREATE_SCAN 232
#define CMD_OCR 233
#define CMD_HEADFOOT 234
#define CMD_WATERMARK 235
#define CMD_BATES 236
#define CMD_PAGENUM 237
#define CMD_OPTIMIZE 238
#define CMD_FORM_DETECT 239
#define CMD_FORM_PROPS 240
#define CMD_FORM_RESET 241
#define CMD_CERTSIGN 242
#define CMD_VALIDATE 243
#define CMD_MAKEID 244
#define CMD_ATTACHMENTS 245
#define CMD_SIGNATURES 246
#define CMD_LAYOUT_CONT 247
#define CMD_LAYOUT_TWOCONT 248
#define CMD_LAYOUT_SINGLE 249
#define CMD_LAYOUT_TWO 250
#define CMD_COVER 251
#define CMD_NIGHT 252
#define CMD_READ_PAGE 253
#define CMD_READ_DOC 254
#define CMD_READ_STOP 255
#define CMD_ADDATTACH 256
#define CMD_STAMPS 257
#define CMD_FORM_DELETE 258
#define CMD_FULLSCREEN 259
#define CMD_FORM_CLEARALL 260
#define CMD_SHORTCUTS 261
#define CMD_HOMETAB 262
#define CMD_PROPSBAR 263
/* organize */
#define CMD_ORG_ROTL 280
#define CMD_ORG_ROTR 281
#define CMD_ORG_DELETE 282
#define CMD_ORG_BLANK 283
#define CMD_ORG_INSERT 284
#define CMD_ORG_EXTRACT 285
#define CMD_ORG_SPLIT 286
#define CMD_ORG_SELALL 287
#define CMD_ORG_REPLACE 288
#define CMD_ORG_SCAN 289
extern const COLORREF COMMENT_COLORS[NCOLORS];

#endif
