/* sg-pdf -- SG PDF: the tools pane (right) and the open tool's bar.
 *
 * With no tool open the pane lists the tools -- Edit PDF, Comment, Fill &
 * Sign, Redact, Organize Pages, Export PDF, Combine Files, Protect -- and a
 * click opens one: its bar appears under the toolbar (its own tools: Add
 * text, Highlight, Sign, Mark area, Rotate ...; the colours for comments;
 * Close) and the pane becomes its properties -- the text format (font,
 * size, colour, bold, italic, alignment) for Edit PDF, the list of comments,
 * the form's fields, what redaction does and the marks so far, the pages
 * picked for organizing. Export, Combine and Protect are dialogs.
 *
 * Everything is our own drawing in the app mode's palette, except the
 * format's standard controls.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"
#include "../sg-smooth.h"
#include <commdlg.h>

HWND g_tbar, g_pane;
static HWND g_list, g_font_cb, g_size_cb, g_color_btn, g_bold_btn, g_italic_btn, g_align_btn[3];
static int g_list_gen = -1, g_list_tool = -1;
static int g_thover = -1, g_tpress = -1, g_phover = -1, g_ppress = -1;
static BOOL g_format_busy;

#define TBAR_H dpx(40)
#define PANE_W dpx(252)

/* ---- glyphs ------------------------------------------------------------------------------------------ */


static void ln(HDC dc, int x1, int y1, int x2, int y2)
{
    MoveToEx(dc, x1, y1, NULL);
    LineTo(dc, x2, y2);
}

/* sg-smooth: drawn in a region (glyph, below) */
static void glyph_raw(HDC dc, int k, int cx, int cy, int s, COLORREF col, COLORREF accent)
{
    HPEN pen = CreatePen(PS_SOLID, max(1, s / 8), col), op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HBRUSH acc = CreateSolidBrush(accent);
    int h = s / 2, q = s / 4;
    RECT r;
    switch (k) {
    case T_SELECT: {
        POINT p[4] = { { cx - q, cy - h }, { cx - q, cy + q + 2 }, { cx, cy + 1 }, { cx + q + 2, cy + 1 } };
        Polygon(dc, p, 4);
        ln(dc, cx - 1, cy + 1, cx + q / 2, cy + h);
        break;
    }
    case T_TEXT: case T_TEXTBOX:
        ln(dc, cx - q - 2, cy - h + 2, cx + q + 2, cy - h + 2);
        ln(dc, cx, cy - h + 2, cx, cy + h - 1);
        ln(dc, cx - q / 2, cy + h - 1, cx + q / 2 + 1, cy + h - 1);
        if (k == T_TEXTBOX) Rectangle(dc, cx - h, cy - h - 1, cx + h + 1, cy + h + 2);
        break;
    case T_IMAGE: case T_REPLACE:
        Rectangle(dc, cx - h, cy - h + 2, cx + h + 1, cy + h - 1);
        ln(dc, cx - h + 2, cy + h - 3, cx - q / 2, cy);
        ln(dc, cx - q / 2, cy, cx + 1, cy + q);
        ln(dc, cx + 1, cy + q, cx + q, cy - 1);
        ln(dc, cx + q, cy - 1, cx + h - 1, cy + h - 3);
        Ellipse(dc, cx + q - 2, cy - h + 4, cx + q + 3, cy - h + 9);
        if (k == T_REPLACE) { ln(dc, cx - h - 2, cy + h + 2, cx + h + 2, cy + h + 2); }
        break;
    case T_DELETE:
        ln(dc, cx - h, cy - h + 3, cx + h + 1, cy - h + 3);
        ln(dc, cx - q / 2, cy - h, cx + q / 2 + 1, cy - h);
        Rectangle(dc, cx - q - 2, cy - h + 3, cx + q + 3, cy + h + 1);
        ln(dc, cx - 2, cy - q + 2, cx - 2, cy + h - 2);
        ln(dc, cx + 2, cy - q + 2, cx + 2, cy + h - 2);
        break;
    case T_NOTE: case T_COMMENT: {
        POINT p[7] = { { cx - h, cy - h + 2 }, { cx + h, cy - h + 2 }, { cx + h, cy + q }, { cx - 1, cy + q },
                       { cx - q, cy + h }, { cx - q, cy + q }, { cx - h, cy + q } };
        Polygon(dc, p, 7);
        ln(dc, cx - q, cy - q + 1, cx + q, cy - q + 1);
        ln(dc, cx - q, cy + 1, cx + q / 2, cy + 1);
        break;
    }
    case T_HIGHLIGHT:
        SetRect(&r, cx - h, cy + q, cx + h + 1, cy + h + 1);
        FillRect(dc, &r, acc);
        ln(dc, cx - q, cy + q - 1, cx + q, cy - h + 1);
        ln(dc, cx - q + 4, cy + q - 1, cx + q + 4, cy - h + 1);
        ln(dc, cx + q, cy - h + 1, cx + q + 4, cy - h + 1);
        break;
    case T_UNDERLINE:
        ln(dc, cx - q, cy - h + 1, cx - q, cy + 1);
        Arc(dc, cx - q, cy - q, cx + q + 1, cy + q / 2 + 3, cx - q, cy, cx + q, cy);
        ln(dc, cx + q, cy - h + 1, cx + q, cy + 1);
        SetRect(&r, cx - h, cy + h - 2, cx + h + 1, cy + h);
        FillRect(dc, &r, acc);
        break;
    case T_STRIKE:
        Arc(dc, cx - q, cy - h, cx + q, cy, cx + q, cy - h + 2, cx - q, cy - q);
        Arc(dc, cx - q, cy, cx + q, cy + h, cx - q, cy + h - 2, cx + q, cy + q);
        SetRect(&r, cx - h, cy - 1, cx + h + 1, cy + 1);
        FillRect(dc, &r, acc);
        break;
    case T_RECT: Rectangle(dc, cx - h, cy - q - 2, cx + h + 1, cy + q + 3); break;
    case T_ELLIPSE: Ellipse(dc, cx - h, cy - q - 2, cx + h + 1, cy + q + 3); break;
    case T_ARROW:
        ln(dc, cx - h, cy + h, cx + h, cy - h);
        ln(dc, cx + h, cy - h, cx + h - q - 1, cy - h);
        ln(dc, cx + h, cy - h, cx + h, cy - h + q + 1);
        break;
    case T_LINE: ln(dc, cx - h, cy + h, cx + h, cy - h); break;
    case T_PEN: {
        POINT p[4] = { { cx - h + 1, cy + h - 1 }, { cx - h + 2, cy + q - 1 }, { cx + q + 1, cy - h + 1 }, { cx + h - 1, cy - q - 1 } };
        Polyline(dc, p, 4);
        ln(dc, cx + h - 1, cy - q - 1, cx - q + 1, cy + h - 2);
        ln(dc, cx - q + 1, cy + h - 2, cx - h + 1, cy + h - 1);
        break;
    }
    case T_SIGN: case T_FILL:
        ln(dc, cx - h, cy + h, cx + h, cy + h);
        ln(dc, cx - h + 2, cy + q, cx + q, cy - h + 2);
        ln(dc, cx + q, cy - h + 2, cx + h - 1, cy - q + 1);
        ln(dc, cx + h - 1, cy - q + 1, cx - q + 3, cy + q + 3);
        ln(dc, cx - q + 3, cy + q + 3, cx - h + 2, cy + q);
        if (k == T_FILL) Rectangle(dc, cx - h, cy - h, cx - 1, cy - q + 1);
        break;
    case T_FLATTEN:
        Rectangle(dc, cx - h, cy - 2, cx + h + 1, cy + q);
        ln(dc, cx - h + 2, cy - q - 1, cx + h - 1, cy - q - 1);
        ln(dc, cx - h + 4, cy - h + 1, cx + h - 3, cy - h + 1);
        ln(dc, cx - h, cy + h, cx + h + 1, cy + h);
        break;
    case T_MARK: case T_REDACT:
        SetRect(&r, cx - h, cy - q, cx + h + 1, cy + q);
        FillRect(dc, &r, GetStockObject(BLACK_BRUSH));
        if (k == T_MARK) {
            HPEN red = CreatePen(PS_SOLID, max(1, s / 10), C_REDMARK);
            HGDIOBJ o2 = SelectObject(dc, red);
            Rectangle(dc, cx - h - 2, cy - q - 2, cx + h + 3, cy + q + 2);
            SelectObject(dc, o2);
            DeleteObject(red);
        } else {
            ln(dc, cx - h, cy - h, cx + h, cy - h);
            ln(dc, cx - h, cy + h, cx + q, cy + h);
        }
        break;
    case T_AREA: {
        LOGBRUSH lb = { BS_SOLID, col, 0 };
        HPEN dash = ExtCreatePen(PS_GEOMETRIC | PS_DOT, 1, &lb, 0, NULL);
        HGDIOBJ o2 = SelectObject(dc, dash);
        Rectangle(dc, cx - h, cy - q - 2, cx + h + 1, cy + q + 3);
        SelectObject(dc, o2);
        DeleteObject(dash);
        ln(dc, cx + q, cy + q, cx + h + 2, cy + h + 2);
        break;
    }
    case T_FIND:
        Ellipse(dc, cx - h, cy - h, cx + q + 1, cy + q + 1);
        ln(dc, cx + q - 1, cy + q - 1, cx + h, cy + h);
        break;
    case T_APPLY:
        ln(dc, cx - h + 1, cy, cx - q + 2, cy + q + 1);
        ln(dc, cx - q + 2, cy + q + 1, cx + h, cy - q - 1);
        break;
    case T_CLEAN:
        Rectangle(dc, cx - q - 1, cy - h, cx + q + 2, cy - q + 3);
        ln(dc, cx, cy - q + 3, cx, cy + q);
        ln(dc, cx - h, cy + h, cx - q, cy + q);
        ln(dc, cx - q, cy + q, cx + q, cy + q);
        ln(dc, cx + q, cy + q, cx + h, cy + h);
        ln(dc, cx - q, cy + h, cx, cy + q + 1);
        break;
    case T_ROTL: case T_ROTR: {
        int rr = q + 2;
        SetArcDirection(dc, k == T_ROTR ? AD_CLOCKWISE : AD_COUNTERCLOCKWISE);
        if (k == T_ROTR) {
            Arc(dc, cx - rr, cy - rr, cx + rr + 1, cy + rr + 1, cx - rr, cy, cx, cy - rr);
            ln(dc, cx, cy - rr, cx - 3, cy - rr - 3); ln(dc, cx, cy - rr, cx - 3, cy - rr + 3);
        } else {
            Arc(dc, cx - rr, cy - rr, cx + rr + 1, cy + rr + 1, cx + rr, cy, cx, cy - rr);
            ln(dc, cx, cy - rr, cx + 3, cy - rr - 3); ln(dc, cx, cy - rr, cx + 3, cy - rr + 3);
        }
        break;
    }
    case T_BLANK: case T_INSERT: case T_EXTRACT: case T_EXPORT: case T_ORGANIZE: case T_EDIT: {
        POINT p[5] = { { cx - q - 2, cy - h }, { cx + q - 1, cy - h }, { cx + q + 3, cy - h + 4 }, { cx + q + 3, cy + h },
                       { cx - q - 2, cy + h } };
        Polygon(dc, p, 5);
        if (k == T_INSERT) { ln(dc, cx - 3, cy + 1, cx + 4, cy + 1); ln(dc, cx, cy - 2, cx, cy + 5); }
        if (k == T_EXTRACT || k == T_EXPORT) { ln(dc, cx - 2, cy + 1, cx + h + 2, cy + 1); ln(dc, cx + h + 2, cy + 1, cx + h - 1, cy - 2); ln(dc, cx + h + 2, cy + 1, cx + h - 1, cy + 4); }
        if (k == T_ORGANIZE) { Rectangle(dc, cx - h - 2, cy - h + 3, cx - q - 1, cy + h - 2); }
        if (k == T_EDIT) { ln(dc, cx - 2, cy + q + 1, cx + h + 1, cy - q); ln(dc, cx - 2, cy + q + 1, cx - 3, cy + q + 3); }
        break;
    }
    case T_SPLIT:
        Rectangle(dc, cx - h, cy - h, cx - 1, cy + h + 1);
        Rectangle(dc, cx + 2, cy - h, cx + h + 2, cy + h + 1);
        break;
    case T_COMBINE:
        Rectangle(dc, cx - h, cy - h, cx + 1, cy + q);
        Rectangle(dc, cx - 1, cy - q, cx + h + 1, cy + h + 1);
        break;
    case T_PROTECT:
        Rectangle(dc, cx - q - 2, cy - 1, cx + q + 3, cy + h + 1);
        Arc(dc, cx - q, cy - h, cx + q + 1, cy + 3, cx + q, cy - 1, cx - q, cy - 1);
        break;
    case T_CLOSE:
        ln(dc, cx - q, cy - q, cx + q + 1, cy + q + 1);
        ln(dc, cx + q, cy - q, cx - q - 1, cy + q + 1);
        break;
    case T_MENU:
        ln(dc, cx - h + 1, cy - q, cx + h, cy - q); ln(dc, cx - h + 1, cy, cx + h, cy); ln(dc, cx - h + 1, cy + q, cx + h, cy + q);
        break;
    case T_HOME: {
        POINT p5[5] = { { cx - h + 2, cy - 1 }, { cx, cy - h + 1 }, { cx + h - 2, cy - 1 }, { cx + h - 2, cy + h - 1 }, { cx - h + 2, cy + h - 1 } };
        Polygon(dc, p5, 5);
        Rectangle(dc, cx - 3, cy + 3, cx + 4, cy + h);
        break;
    }
    case T_UP: ln(dc, cx - q - 1, cy + 2, cx, cy - q + 1); ln(dc, cx, cy - q + 1, cx + q + 1, cy + 2); break;
    case T_DOWN: ln(dc, cx - q - 1, cy - 2, cx, cy + q - 1); ln(dc, cx, cy + q - 1, cx + q + 1, cy - 2); break;
    case T_PLUS: ln(dc, cx - q - 1, cy, cx + q + 2, cy); ln(dc, cx, cy - q - 1, cx, cy + q + 2); break;
    case T_MINUS: ln(dc, cx - q - 1, cy, cx + q + 2, cy); break;
    case T_FITG:
        Rectangle(dc, cx - q, cy - h + 2, cx + q + 1, cy + h - 1);
        ln(dc, cx - h, cy - 1, cx - q - 2, cy - 1); ln(dc, cx + q + 3, cy - 1, cx + h + 1, cy - 1);
        break;
    case T_OPENFILE: {
        POINT p6[6] = { { cx - h, cy - q - 2 }, { cx - q, cy - q - 2 }, { cx - 2, cy - q + 1 }, { cx + h, cy - q + 1 },
                        { cx + h, cy + h - 2 }, { cx - h, cy + h - 2 } };
        Polygon(dc, p6, 6);
        break;
    }
    case T_ALLTOOLS:
        Rectangle(dc, cx - h + 1, cy - h + 1, cx - 1, cy - 1); Rectangle(dc, cx + 2, cy - h + 1, cx + h, cy - 1);
        Rectangle(dc, cx - h + 1, cy + 2, cx - 1, cy + h); Rectangle(dc, cx + 2, cy + 2, cx + h, cy + h);
        break;
    case T_BACK: ln(dc, cx + 2, cy - q - 1, cx - q + 1, cy); ln(dc, cx - q + 1, cy, cx + 2, cy + q + 1); break;
    case T_COMPRESS:
        Rectangle(dc, cx - q - 1, cy - h + 1, cx + q + 2, cy + h);
        ln(dc, cx, cy - q - 2, cx, cy - 1); ln(dc, cx - 3, cy - 4, cx, cy - 1); ln(dc, cx + 3, cy - 4, cx, cy - 1);
        ln(dc, cx, cy + q + 2, cx, cy + 2); ln(dc, cx - 3, cy + 5, cx, cy + 2); ln(dc, cx + 3, cy + 5, cx, cy + 2);
        break;
    case T_CREATE: {
        POINT p5[5] = { { cx - q - 2, cy - h }, { cx + q - 1, cy - h }, { cx + q + 3, cy - h + 4 }, { cx + q + 3, cy + h },
                        { cx - q - 2, cy + h } };
        Polygon(dc, p5, 5);
        ln(dc, cx - 3, cy + 1, cx + 4, cy + 1); ln(dc, cx, cy - 2, cx, cy + 5);
        break;
    }
    case T_FORM: case T_FTEXT: case T_FDATE: case T_FNUM: {
        Rectangle(dc, cx - h, cy - q, cx + h + 1, cy + q + 1);
        if (k == T_FORM) { ln(dc, cx - h, cy - h + 1, cx + h + 1, cy - h + 1); ln(dc, cx - h, cy + h, cx + 1, cy + h); }
        if (k == T_FTEXT) { ln(dc, cx - q, cy - q + 3, cx - q, cy + q - 2); }
        if (k == T_FDATE) { ln(dc, cx - q, cy - q - 2, cx - q, cy - q + 2); ln(dc, cx + q, cy - q - 2, cx + q, cy - q + 2); ln(dc, cx - h + 2, cy, cx + h - 1, cy); }
        if (k == T_FNUM) { ln(dc, cx - q, cy - 2, cx + q, cy - 2); ln(dc, cx - q, cy + 2, cx + q, cy + 2); ln(dc, cx - 2, cy - q + 2, cx - 3, cy + q - 1); ln(dc, cx + 2, cy - q + 2, cx + 1, cy + q - 1); }
        break;
    }
    case T_FCHECK: case T_CHECKMARK:
        if (k == T_FCHECK) Rectangle(dc, cx - q - 2, cy - q - 2, cx + q + 3, cy + q + 3);
        ln(dc, cx - q, cy, cx - 1, cy + q);
        ln(dc, cx - 1, cy + q, cx + q + 1, cy - q);
        break;
    case T_CROSS:
        ln(dc, cx - q, cy - q, cx + q + 1, cy + q + 1);
        ln(dc, cx + q, cy - q, cx - q - 1, cy + q + 1);
        break;
    case T_DOT: case T_FRADIO: {
        HGDIOBJ ob2 = SelectObject(dc, k == T_DOT ? (HGDIOBJ)acc : GetStockObject(NULL_BRUSH));
        if (k == T_FRADIO) { Ellipse(dc, cx - q - 2, cy - q - 2, cx + q + 3, cy + q + 3); SelectObject(dc, acc); Ellipse(dc, cx - 2, cy - 2, cx + 3, cy + 3); }
        else Ellipse(dc, cx - q, cy - q, cx + q + 1, cy + q + 1);
        SelectObject(dc, ob2);
        break;
    }
    case T_FCOMBO: case T_FLIST:
        Rectangle(dc, cx - h, cy - q, cx + h + 1, cy + q + 1);
        if (k == T_FCOMBO) { POINT t3[3] = { { cx + q - 2, cy - 2 }, { cx + q + 4, cy - 2 }, { cx + q + 1, cy + 2 } }; Polygon(dc, t3, 3); }
        else { ln(dc, cx - h + 2, cy - 2, cx + q, cy - 2); ln(dc, cx - h + 2, cy + 2, cx + q, cy + 2); }
        break;
    case T_FSIGN: case T_CERT:
        if (k == T_FSIGN) Rectangle(dc, cx - h, cy - q, cx + h + 1, cy + q + 1);
        ln(dc, cx - q, cy + 2, cx - 2, cy - 3); ln(dc, cx - 2, cy - 3, cx + 1, cy + 3); ln(dc, cx + 1, cy + 3, cx + q, cy - 2);
        if (k == T_CERT) { Ellipse(dc, cx + q - 1, cy + q - 3, cx + h + 2, cy + h); ln(dc, cx - h, cy + h - 1, cx + q - 2, cy + h - 1); }
        break;
    case T_DETECT:
        Rectangle(dc, cx - h, cy - h, cx + 1, cy - 1);
        Ellipse(dc, cx - 1, cy - 1, cx + q + 3, cy + q + 3);
        ln(dc, cx + q + 2, cy + q + 2, cx + h + 1, cy + h + 1);
        break;
    case T_PROPS:
        ln(dc, cx - h, cy - q, cx + h, cy - q); ln(dc, cx - h, cy, cx + h, cy); ln(dc, cx - h, cy + q, cx + h, cy + q);
        Rectangle(dc, cx - q - 2, cy - q - 2, cx - q + 2, cy - q + 2);
        Rectangle(dc, cx + 1, cy + q - 2, cx + 5, cy + q + 2);
        break;
    case T_STAMP:
        Rectangle(dc, cx - h, cy + 1, cx + h + 1, cy + q + 2);
        ln(dc, cx, cy - h + 3, cx, cy + 1);
        Ellipse(dc, cx - 3, cy - h, cx + 4, cy - h + 6);
        ln(dc, cx - h, cy + h, cx + h + 1, cy + h);
        break;
    case T_LINK:
        RoundRect(dc, cx - h, cy - 3, cx + 1, cy + 4, 6, 6);
        RoundRect(dc, cx - 1, cy - 3, cx + h + 1, cy + 4, 6, 6);
        break;
    case T_OCR: case T_SCAN:
        Rectangle(dc, cx - q - 1, cy - q - 1, cx + q + 2, cy + q + 2);
        ln(dc, cx - h, cy - h, cx - q, cy - h); ln(dc, cx - h, cy - h, cx - h, cy - q);
        ln(dc, cx + h, cy - h, cx + q, cy - h); ln(dc, cx + h, cy - h, cx + h, cy - q);
        ln(dc, cx - h, cy + h, cx - q, cy + h); ln(dc, cx - h, cy + h, cx - h, cy + q);
        ln(dc, cx + h, cy + h, cx + q, cy + h); ln(dc, cx + h, cy + h, cx + h, cy + q);
        if (k == T_OCR) { ln(dc, cx - 2, cy - 2, cx + 3, cy - 2); ln(dc, cx, cy - 2, cx, cy + 3); }
        break;
    case T_HEADER: case T_WATERMARK: case T_BATES: case T_REPLACE_PAGE: {
        POINT p5[5] = { { cx - q - 2, cy - h }, { cx + q - 1, cy - h }, { cx + q + 3, cy - h + 4 }, { cx + q + 3, cy + h },
                        { cx - q - 2, cy + h } };
        Polygon(dc, p5, 5);
        if (k == T_HEADER) { ln(dc, cx - q, cy - h + 3, cx + q, cy - h + 3); ln(dc, cx - q, cy + h - 3, cx + q, cy + h - 3); }
        if (k == T_WATERMARK) ln(dc, cx - q, cy + q, cx + q, cy - q);
        if (k == T_BATES) { ln(dc, cx, cy + h - 5, cx + q, cy + h - 5); ln(dc, cx, cy + h - 3, cx + q, cy + h - 3); }
        if (k == T_REPLACE_PAGE) { ln(dc, cx - q, cy, cx + q, cy); ln(dc, cx + q, cy, cx + 1, cy - 3); ln(dc, cx - q, cy + 3, cx + q, cy + 3); ln(dc, cx - q, cy + 3, cx - 1, cy + 6); }
        break;
    }
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    DeleteObject(acc);
}

/* the glyph drawn soft-edged (sg-smooth.h): its lines and shapes four times finer, averaged down */
static void glyph(HDC dc, int k, int cx, int cy, int s, COLORREF col, COLORREF accent)
{
    struct sg_ss ss;
    HDC big = sg_ss_begin(&ss, dc, cx - s, cy - s, 2 * s + 1, 2 * s + 1, max(1, s / 8));
    glyph_raw(big, k, cx, cy, s, col, accent);
    sg_ss_end(&ss);
}

void pdf_glyph(HDC dc, int k, int cx, int cy, int s, COLORREF col, COLORREF accent)
{
    glyph(dc, k, cx, cy, s, col, accent);
}

/* ---- the tool's bar ----------------------------------------------------------------------------------- */

typedef struct { int cmd, glyph; const WCHAR *label, *tip; const char *name; } tdef_t;
#define TD_SEP { 0, 0, NULL, NULL, NULL }

static const tdef_t EDIT_BAR[] = {
    { CMD_SUB + SUB_SELECT, T_SELECT, L"Edit", L"Select, move, resize and edit text and pictures", "select" },
    { CMD_SUB + SUB_ADDTEXT, T_TEXT, L"Add text", L"Click where the text goes", "addtext" },
    { CMD_SUB + SUB_ADDIMAGE, T_IMAGE, L"Add image", L"Choose a picture, then click where it goes", "addimage" },
    { CMD_SUB + SUB_LINK, T_LINK, L"Link", L"Drag a box over what is to be a link", "link" },
    TD_SEP,
    { CMD_HEADFOOT, T_HEADER, NULL, L"Header & footer", "headfoot" },
    { CMD_WATERMARK, T_WATERMARK, NULL, L"Watermark", "watermark" },
    { CMD_BATES, T_BATES, NULL, L"Bates numbering", "bates" },
    TD_SEP,
    { CMD_REPLACEIMAGE, T_REPLACE, L"Replace", L"Replace the selected picture", "replace" },
    { CMD_DELETE, T_DELETE, L"Delete", L"Delete the selected object (Del)", "delete" },
};
static const tdef_t COMMENT_BAR[] = {
    { CMD_SUB + SUB_SELECT, T_SELECT, NULL, L"Select comments", "select" },
    { CMD_SUB + SUB_NOTE, T_NOTE, NULL, L"Sticky note", "note" },
    { CMD_SUB + SUB_HIGHLIGHT, T_HIGHLIGHT, NULL, L"Highlight text", "highlight" },
    { CMD_SUB + SUB_UNDERLINE, T_UNDERLINE, NULL, L"Underline text", "underline" },
    { CMD_SUB + SUB_STRIKE, T_STRIKE, NULL, L"Strikethrough text", "strike" },
    { CMD_SUB + SUB_FREETEXT, T_TEXTBOX, NULL, L"Text box", "textbox" },
    TD_SEP,
    { CMD_SUB + SUB_RECT, T_RECT, NULL, L"Rectangle", "rect" },
    { CMD_SUB + SUB_ELLIPSE, T_ELLIPSE, NULL, L"Oval", "ellipse" },
    { CMD_SUB + SUB_ARROW, T_ARROW, NULL, L"Arrow", "arrow" },
    { CMD_SUB + SUB_LINE, T_LINE, NULL, L"Line", "line" },
    { CMD_SUB + SUB_INK, T_PEN, NULL, L"Draw free form", "ink" },
    { CMD_STAMPS, T_STAMP, NULL, L"Stamp (Approved, Draft, Confidential...)", "stamp" },
    TD_SEP,
    { CMD_DELETE, T_DELETE, NULL, L"Delete the selected comment (Del)", "delete" },
};
static const tdef_t FILL_BAR[] = {
    { CMD_SUB + SUB_SELECT, T_SELECT, L"Fill fields", L"Click a field to fill it", "fill" },
    { CMD_SUB + SUB_FILLTEXT, T_TEXT, L"Add text", L"Type anywhere on the page", "addtext" },
    { CMD_SUB + SUB_MARK_CHECK, T_CHECKMARK, NULL, L"Check mark: click where it goes", "check" },
    { CMD_SUB + SUB_MARK_CROSS, T_CROSS, NULL, L"Cross mark: click where it goes", "cross" },
    { CMD_SUB + SUB_MARK_DOT, T_DOT, NULL, L"Dot: click where it goes", "dot" },
    TD_SEP,
    { CMD_SIGN, T_SIGN, L"Sign", L"Type, draw or choose a signature, then click where it goes", "sign" },
    { CMD_CERTSIGN, T_CERT, L"Certificate", L"Sign with a digital ID (a certificate signature others can check)", "certsign" },
    TD_SEP,
    { CMD_FLATTEN, T_FLATTEN, L"Flatten", L"Make the filled-in values part of the page", "flatten" },
};
static const tdef_t FORM_BAR[] = {
    { CMD_SUB + SUB_SELECT, T_SELECT, NULL, L"Select fields: drag to move, handles to resize, double-click for properties", "select" },
    { CMD_SUB + SUB_F_TEXT, T_FTEXT, NULL, L"Text field", "ftext" },
    { CMD_SUB + SUB_F_DATE, T_FDATE, NULL, L"Date field", "fdate" },
    { CMD_SUB + SUB_F_NUMBER, T_FNUM, NULL, L"Number field", "fnumber" },
    { CMD_SUB + SUB_F_CHECK, T_FCHECK, NULL, L"Check box", "fcheck" },
    { CMD_SUB + SUB_F_RADIO, T_FRADIO, NULL, L"Radio button (buttons of one name are one group)", "fradio" },
    { CMD_SUB + SUB_F_COMBO, T_FCOMBO, NULL, L"Drop-down list", "fcombo" },
    { CMD_SUB + SUB_F_LIST, T_FLIST, NULL, L"List box", "flist" },
    { CMD_SUB + SUB_F_SIGN, T_FSIGN, NULL, L"Signature field", "fsign" },
    TD_SEP,
    { CMD_FORM_DETECT, T_DETECT, L"Auto-detect", L"Find the blanks on the pages and make them fields", "detect" },
    { CMD_FORM_PROPS, T_PROPS, L"Properties", L"The selected field's properties (Enter)", "props" },
    { CMD_FORM_DELETE, T_DELETE, NULL, L"Delete the selected field (Del)", "delete" },
};
static const tdef_t REDACT_BAR[] = {
    { CMD_SUB + SUB_MARKTEXT, T_MARK, L"Mark text & images", L"Drag over text, or drag a box over anything", "marktext" },
    { CMD_SUB + SUB_MARKAREA, T_AREA, L"Mark area", L"Drag a box over an area", "markarea" },
    { CMD_FINDREDACT, T_FIND, L"Find text", L"Find words or patterns and mark them", "findtext" },
    TD_SEP,
    { CMD_APPLYREDACT, T_APPLY, L"Apply", L"Remove what is marked, for good", "apply" },
    { CMD_SANITIZE, T_CLEAN, L"Remove hidden information", L"Metadata, attachments, hidden text and layers, scripts...", "sanitize" },
};
static const tdef_t ORG_BAR[] = {
    { CMD_ORG_ROTL, T_ROTL, NULL, L"Rotate counterclockwise", "rotl" },
    { CMD_ORG_ROTR, T_ROTR, NULL, L"Rotate clockwise", "rotr" },
    { CMD_ORG_DELETE, T_DELETE, NULL, L"Delete pages (Del)", "delete" },
    TD_SEP,
    { CMD_ORG_BLANK, T_BLANK, L"Blank page", L"Insert a blank page after the selection", "blank" },
    { CMD_ORG_INSERT, T_INSERT, L"Insert from file", L"Insert pages of a PDF or a picture after the selection", "insert" },
    { CMD_ORG_EXTRACT, T_EXTRACT, L"Extract", L"Save the selected pages as a new PDF", "extract" },
    { CMD_ORG_SPLIT, T_SPLIT, L"Split", L"Split the document into several files", "split" },
    { CMD_ORG_REPLACE, T_REPLACE_PAGE, L"Replace", L"Replace the selected pages with pages of another PDF", "replace" },
    { CMD_ORG_SCAN, T_SCAN, NULL, L"Insert pages from the scanner after the selection", "scan" },
};

static const tdef_t *bar_defs(int *n)
{
    switch (g.tool) {
    case TOOL_EDIT: *n = sizeof(EDIT_BAR) / sizeof(EDIT_BAR[0]); return EDIT_BAR;
    case TOOL_COMMENT: *n = sizeof(COMMENT_BAR) / sizeof(COMMENT_BAR[0]); return COMMENT_BAR;
    case TOOL_FILL: *n = sizeof(FILL_BAR) / sizeof(FILL_BAR[0]); return FILL_BAR;
    case TOOL_REDACT: *n = sizeof(REDACT_BAR) / sizeof(REDACT_BAR[0]); return REDACT_BAR;
    case TOOL_ORGANIZE: *n = sizeof(ORG_BAR) / sizeof(ORG_BAR[0]); return ORG_BAR;
    case TOOL_FORM: *n = sizeof(FORM_BAR) / sizeof(FORM_BAR[0]); return FORM_BAR;
    }
    *n = 0;
    return NULL;
}

#define MAX_TB 32
static RECT g_tb_rc[MAX_TB];
static int g_tb_cmd[MAX_TB], g_tb_n;
static RECT g_close_rc, g_title_rc;

const WCHAR *tool_name(int tool)
{
    switch (tool) {
    case TOOL_EDIT: return L"Edit PDF";
    case TOOL_COMMENT: return L"Comment";
    case TOOL_FILL: return L"Fill & Sign";
    case TOOL_REDACT: return L"Redact";
    case TOOL_ORGANIZE: return L"Organize Pages";
    case TOOL_FORM: return L"Prepare Form";
    }
    return L"";
}

static BOOL cmd_enabled(int cmd)
{
    BOOL pickobj = g.pick.kind == PICK_OBJ && g.pick.page >= 0 && g.pick.page < g.npages &&
                   g.pick.index < g.pages[g.pick.page].nobjs;
    switch (cmd) {
    case CMD_REPLACEIMAGE: return pickobj && g.pages[g.pick.page].objs[g.pick.index].kind == OBJ_IMAGE;
    case CMD_DELETE: return g.pick.kind != PICK_NONE;
    case CMD_APPLYREDACT: return g.nredact > 0;
    case CMD_FLATTEN: return g.form && g.nfields > 0;
    case CMD_CREATE_FILES: case CMD_CREATE_BLANK: case CMD_CREATE_SCAN: case CMD_MAKEID: return g.bridged;
    case CMD_FORM_PROPS: case CMD_FORM_DELETE: return form_picked() >= 0;
    case CMD_ORG_REPLACE: {
        int k;
        for (k = 0; k < g.npages; k++) if (g.org_sel && g.org_sel[k]) return TRUE;
        return FALSE;
    }
    case CMD_ORG_ROTL: case CMD_ORG_ROTR: case CMD_ORG_DELETE: case CMD_ORG_EXTRACT: {
        int k;
        for (k = 0; k < g.npages; k++) if (g.org_sel && g.org_sel[k]) return TRUE;
        return FALSE;
    }
    }
    return g.npages > 0;
}

static void tbar_layout(HDC dc)
{
    RECT rc;
    int n, i, x, b = dpx(32), top;
    const tdef_t *d = bar_defs(&n);
    SIZE sz;
    GetClientRect(g_tbar, &rc);
    top = (rc.bottom - b) / 2;
    SelectObject(dc, g_font_bold);
    GetTextExtentPoint32W(dc, tool_name(g.tool), lstrlenW(tool_name(g.tool)), &sz);
    SetRect(&g_title_rc, dpx(12), 0, dpx(12) + sz.cx, rc.bottom);
    x = g_title_rc.right + dpx(16);
    SelectObject(dc, g_font_small);
    g_tb_n = 0;
    for (i = 0; i < n && g_tb_n < MAX_TB; i++) {
        int w = b;
        if (!d[i].cmd) { x += dpx(10); continue; }
        if (d[i].label) {
            GetTextExtentPoint32W(dc, d[i].label, lstrlenW(d[i].label), &sz);
            w = b + sz.cx + dpx(8);
        }
        SetRect(&g_tb_rc[g_tb_n], x, top, x + w, top + b);
        g_tb_cmd[g_tb_n++] = d[i].cmd;
        x += w + dpx(2);
    }
    if (g.tool == TOOL_COMMENT) {
        x += dpx(10);
        for (i = 0; i < NCOLORS && g_tb_n < MAX_TB; i++) {
            SetRect(&g_tb_rc[g_tb_n], x, top + dpx(6), x + dpx(20), top + b - dpx(6));
            g_tb_cmd[g_tb_n++] = CMD_COLOR + i;
            x += dpx(24);
        }
    }
    SetRect(&g_close_rc, rc.right - dpx(8) - dpx(76), top, rc.right - dpx(8), top + b);
}

static const tdef_t *def_of(int cmd)
{
    int n, i;
    const tdef_t *d = bar_defs(&n);
    for (i = 0; i < n; i++) if (d[i].cmd == cmd) return &d[i];
    return NULL;
}

static void tbar_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC out = BeginPaint(hwnd, &ps), dc;
    RECT rc, r;
    HBITMAP buf, ob;
    HBRUSH bg = CreateSolidBrush(C_SIDE), hov = CreateSolidBrush(C_HOVER), act = CreateSolidBrush(C_ACTIVE),
           ln_b = CreateSolidBrush(C_LINE), accb = CreateSolidBrush(C_ACCENT);
    int i;
    GetClientRect(hwnd, &rc);
    dc = CreateCompatibleDC(out);
    buf = CreateCompatibleBitmap(out, max(rc.right, 1), max(rc.bottom, 1));
    ob = SelectObject(dc, buf);
    FillRect(dc, &rc, bg);
    SetRect(&r, 0, rc.bottom - 1, rc.right, rc.bottom);
    FillRect(dc, &r, ln_b);
    SetBkMode(dc, TRANSPARENT);
    tbar_layout(dc);
    SelectObject(dc, g_font_bold);
    SetTextColor(dc, C_ACCENT);
    DrawTextW(dc, tool_name(g.tool), -1, &g_title_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, g_font_small);
    for (i = 0; i < g_tb_n; i++) {
        int cmd = g_tb_cmd[i];
        RECT br = g_tb_rc[i];
        if (cmd >= CMD_COLOR && cmd < CMD_COLOR + NCOLORS) {
            HBRUSH sw = CreateSolidBrush(COMMENT_COLORS[cmd - CMD_COLOR]);
            RECT o = br;
            if (g.ccolor == COMMENT_COLORS[cmd - CMD_COLOR]) { InflateRect(&o, dpx(3), dpx(3)); FillRect(dc, &o, accb); }
            else { InflateRect(&o, 1, 1); FillRect(dc, &o, ln_b); }
            FillRect(dc, &br, sw);
            DeleteObject(sw);
            continue;
        } else {
            const tdef_t *d = def_of(cmd);
            BOOL on = cmd >= CMD_SUB && cmd - CMD_SUB == g.sub, en = cmd_enabled(cmd);
            COLORREF col = !en ? C_DISABLED : on ? C_ACCENT : C_TEXT;
            if (on) FillRect(dc, &br, act);
            else if (i == g_thover && en) FillRect(dc, &br, hov);
            glyph(dc, d ? d->glyph : T_NONE, br.left + dpx(16), (br.top + br.bottom) / 2, dpx(16), col,
                  g.tool == TOOL_COMMENT ? g.ccolor : RGB(0xFF, 0xD8, 0x00));
            if (d && d->label) {
                RECT tr = br;
                tr.left += dpx(32);
                SetTextColor(dc, col);
                DrawTextW(dc, d->label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
    }
    if (g_thover == MAX_TB) FillRect(dc, &g_close_rc, hov);
    glyph(dc, T_CLOSE, g_close_rc.left + dpx(14), (g_close_rc.top + g_close_rc.bottom) / 2, dpx(14), C_TEXT, C_ACCENT);
    r = g_close_rc;
    r.left += dpx(28);
    SetTextColor(dc, C_TEXT);
    DrawTextW(dc, L"Close", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    BitBlt(out, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(buf);
    DeleteDC(dc);
    DeleteObject(bg); DeleteObject(hov); DeleteObject(act); DeleteObject(ln_b); DeleteObject(accb);
    EndPaint(hwnd, &ps);
}

static int tbar_hit(POINT pt)
{
    int i;
    if (PtInRect(&g_close_rc, pt)) return MAX_TB;
    for (i = 0; i < g_tb_n; i++) if (PtInRect(&g_tb_rc[i], pt)) return i;
    return -1;
}

static HWND g_ttip;

static LRESULT CALLBACK tbar_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: tbar_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = tbar_hit(pt);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        if (h != g_thover) {
            g_thover = h;
            InvalidateRect(hwnd, NULL, FALSE);
            if (g_ttip) {
                TTTOOLINFOW ti = { sizeof(ti) };
                const tdef_t *d = h >= 0 && h < g_tb_n ? def_of(g_tb_cmd[h]) : NULL;
                ti.hwnd = hwnd;
                ti.uId = 1;
                ti.lpszText = (WCHAR *)(d ? d->tip : h == MAX_TB ? L"Close the tool" : L"");
                if (h >= 0) ti.rect = h == MAX_TB ? g_close_rc : g_tb_rc[h];
                SendMessageW(g_ttip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
                SendMessageW(g_ttip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
            }
        }
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: g_thover = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        g_tpress = tbar_hit(pt);
        if (g_tpress >= 0) SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = tbar_hit(pt), p = g_tpress;
        g_tpress = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (h >= 0 && h == p) {
            if (h == MAX_TB) app_command(CMD_TOOLCLOSE);
            else if ((g_tb_cmd[h] >= CMD_COLOR && g_tb_cmd[h] < CMD_COLOR + NCOLORS) || cmd_enabled(g_tb_cmd[h])) app_command(g_tb_cmd[h]);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the pane ---------------------------------------------------------------------------------------------- */

enum { ROW_HEAD, ROW_TOOL, ROW_BUTTON, ROW_TEXT, ROW_LIST, ROW_FORMAT, ROW_STAT };
typedef struct { int kind, cmd, glyph; const WCHAR *label, *desc; const char *name; RECT rc; } row_t;
#define MAX_ROWS 24
static row_t g_rows[MAX_ROWS];
static int g_nrows;
static WCHAR g_stat[160];

static void add_row(int kind, int cmd, int glyph_k, const WCHAR *label, const WCHAR *desc, const char *name)
{
    row_t *r;
    if (g_nrows >= MAX_ROWS) return;
    r = &g_rows[g_nrows++];
    r->kind = kind; r->cmd = cmd; r->glyph = glyph_k; r->label = label; r->desc = desc; r->name = name;
    SetRectEmpty(&r->rc);
}

static void build_rows(void)
{
    g_nrows = 0;
    switch (g.tool) {
    case TOOL_NONE:
        add_row(ROW_HEAD, 0, 0, L"All tools", NULL, NULL);
        add_row(ROW_TOOL, CMD_TOOL + TOOL_EDIT, T_EDIT, L"Edit PDF", L"Change text and pictures", "edit");
        add_row(ROW_TOOL, CMD_TOOL + TOOL_COMMENT, T_COMMENT, L"Comment", L"Notes, highlights, drawings", "comment");
        add_row(ROW_TOOL, CMD_TOOL + TOOL_FILL, T_FILL, L"Fill & Sign", L"Fill in forms, sign", "fill");
        add_row(ROW_TOOL, CMD_TOOL + TOOL_REDACT, T_REDACT, L"Redact", L"Remove content for good", "redact");
        add_row(ROW_TOOL, CMD_TOOL + TOOL_ORGANIZE, T_ORGANIZE, L"Organize Pages", L"Reorder, rotate, insert, delete", "organize");
        add_row(ROW_TOOL, CMD_TOOL + TOOL_FORM, T_FORM, L"Prepare Form", L"Make a fillable form", "form");
        add_row(ROW_TOOL, CMD_CERTSIGN, T_CERT, L"Certificates", L"Sign with a digital ID", "certificates");
        add_row(ROW_TOOL, CMD_OCR, T_OCR, L"Scan & OCR", L"Recognize text in scans", "ocr");
        add_row(ROW_TOOL, CMD_CREATE_FILES, T_INSERT, L"Create PDF", L"From files, office documents", "create");
        add_row(ROW_TOOL, CMD_EXPORT, T_EXPORT, L"Export PDF", L"Word, text, HTML, pictures", "export");
        add_row(ROW_TOOL, CMD_COMBINE, T_COMBINE, L"Combine Files", L"Several files into one PDF", "combine");
        add_row(ROW_TOOL, CMD_PROTECT, T_PROTECT, L"Protect", L"Passwords and permissions", "protect");
        add_row(ROW_TOOL, CMD_OPTIMIZE, T_FLATTEN, L"Compress", L"Make the file smaller", "optimize");
        break;
    case TOOL_FORM:
        add_row(ROW_BUTTON, CMD_TOOLCLOSE, T_BACK, L"All tools", NULL, "back");
        add_row(ROW_HEAD, 0, 0, L"Fields", NULL, NULL);
        add_row(ROW_STAT, 0, 0, NULL, NULL, NULL);
        add_row(ROW_BUTTON, CMD_FORM_DETECT, T_DETECT, L"Auto-detect form fields", NULL, "detect");
        add_row(ROW_BUTTON, CMD_FORM_PROPS, T_PROPS, L"Field properties...", NULL, "props");
        add_row(ROW_LIST, 0, 0, NULL, NULL, NULL);
        break;
    case TOOL_EDIT:
        add_row(ROW_BUTTON, CMD_TOOLCLOSE, T_BACK, L"All tools", NULL, "back");
        add_row(ROW_HEAD, 0, 0, L"Format", NULL, NULL);
        add_row(ROW_FORMAT, 0, 0, NULL, NULL, NULL);
        add_row(ROW_TEXT, 0, 0, L"Click an object to select it; drag it to move it, drag a corner to resize it. "
                                L"Double-click text to change it: it reflows in its box. Del deletes.", NULL, NULL);
        break;
    case TOOL_COMMENT:
        add_row(ROW_BUTTON, CMD_TOOLCLOSE, T_BACK, L"All tools", NULL, "back");
        add_row(ROW_HEAD, 0, 0, L"Comments", NULL, NULL);
        add_row(ROW_STAT, 0, 0, NULL, NULL, NULL);
        add_row(ROW_LIST, 0, 0, NULL, NULL, NULL);
        break;
    case TOOL_FILL:
        add_row(ROW_BUTTON, CMD_TOOLCLOSE, T_BACK, L"All tools", NULL, "back");
        add_row(ROW_HEAD, 0, 0, L"Fill & Sign", NULL, NULL);
        add_row(ROW_STAT, 0, 0, NULL, NULL, NULL);
        add_row(ROW_BUTTON, CMD_SIGN, T_SIGN, L"Sign yourself...", NULL, "sign");
        add_row(ROW_BUTTON, CMD_CERTSIGN, T_CERT, L"Sign with a certificate...", NULL, "certsign");
        add_row(ROW_BUTTON, CMD_FLATTEN, T_FLATTEN, L"Flatten form", NULL, "flatten");
        add_row(ROW_LIST, 0, 0, NULL, NULL, NULL);
        break;
    case TOOL_REDACT:
        add_row(ROW_BUTTON, CMD_TOOLCLOSE, T_BACK, L"All tools", NULL, "back");
        add_row(ROW_HEAD, 0, 0, L"Redact", NULL, NULL);
        add_row(ROW_STAT, 0, 0, NULL, NULL, NULL);
        add_row(ROW_BUTTON, CMD_FINDREDACT, T_FIND, L"Find text && patterns...", NULL, "findtext");
        add_row(ROW_BUTTON, CMD_APPLYREDACT, T_APPLY, L"Apply redactions", NULL, "apply");
        add_row(ROW_BUTTON, CMD_SANITIZE, T_CLEAN, L"Remove hidden information...", NULL, "sanitize");
        add_row(ROW_TEXT, 0, 0, L"Marks show as red boxes. Apply removes the text, the picture pixels and the "
                                L"drawings under them from the file itself -- not just covered -- and saving writes "
                                L"the file anew, so none of it can be recovered.", NULL, NULL);
        break;
    case TOOL_ORGANIZE:
        add_row(ROW_BUTTON, CMD_TOOLCLOSE, T_BACK, L"All tools", NULL, "back");
        add_row(ROW_HEAD, 0, 0, L"Organize Pages", NULL, NULL);
        add_row(ROW_STAT, 0, 0, NULL, NULL, NULL);
        add_row(ROW_BUTTON, CMD_ORG_BLANK, T_BLANK, L"Insert blank page", NULL, "blank");
        add_row(ROW_BUTTON, CMD_ORG_INSERT, T_INSERT, L"Insert from file...", NULL, "insert");
        add_row(ROW_BUTTON, CMD_ORG_EXTRACT, T_EXTRACT, L"Extract pages...", NULL, "extract");
        add_row(ROW_BUTTON, CMD_ORG_SPLIT, T_SPLIT, L"Split document...", NULL, "split");
        add_row(ROW_BUTTON, CMD_ORG_REPLACE, T_REPLACE_PAGE, L"Replace pages...", NULL, "replace");
        add_row(ROW_BUTTON, CMD_ORG_SCAN, T_SCAN, L"Insert from scanner...", NULL, "scan");
        add_row(ROW_TEXT, 0, 0, L"Click a page to select it (Ctrl and Shift add), drag pages to move them. "
                                L"Del deletes the selected pages.", NULL, NULL);
        break;
    }
}

static int text_height(HDC dc, const WCHAR *t, int w)
{
    RECT r = { 0, 0, w, 0 };
    DrawTextW(dc, t, -1, &r, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    return r.bottom;
}

static void stat_text(void)
{
    int i, n = 0;
    g_stat[0] = 0;
    switch (g.tool) {
    case TOOL_COMMENT:
        n = g_list ? (int)SendMessageW(g_list, LB_GETCOUNT, 0, 0) : 0;
        swprintf(g_stat, 160, n == 1 ? L"1 comment" : L"%d comments", n);
        break;
    case TOOL_FORM:
        swprintf(g_stat, 160, g.nfields == 1 ? L"1 field. Add fields with the bar above, or Auto-detect them."
                                             : L"%d fields. Add fields with the bar above, or Auto-detect them.", g.nfields);
        break;
    case TOOL_FILL:
        if (!g.form || !g.nfields) lstrcpyW(g_stat, L"This document has no form fields. Use Add text and the marks to type on it.");
        else swprintf(g_stat, 160, L"%d field%ls. Click one to fill it in.", g.nfields, g.nfields == 1 ? L"" : L"s");
        break;
    case TOOL_REDACT:
        swprintf(g_stat, 160, g.nredact == 1 ? L"1 mark, not applied yet" : L"%d marks, not applied yet", g.nredact);
        if (!g.nredact) lstrcpyW(g_stat, L"Nothing marked yet.");
        break;
    case TOOL_ORGANIZE:
        for (i = 0; i < g.npages; i++) if (g.org_sel && g.org_sel[i]) n++;
        swprintf(g_stat, 160, L"%d page%ls, %d selected", g.npages, g.npages == 1 ? L"" : L"s", n);
        break;
    }
}

static void pane_layout(HDC dc)
{
    RECT rc;
    int i, y = dpx(12), w, x = dpx(14);
    GetClientRect(g_pane, &rc);
    w = rc.right - 2 * x;
    stat_text();
    for (i = 0; i < g_nrows; i++) {
        row_t *r = &g_rows[i];
        int h = 0;
        switch (r->kind) {
        case ROW_HEAD: h = dpx(34); break;
        case ROW_TOOL: h = dpx(44); break;
        case ROW_BUTTON: h = dpx(36); break;
        case ROW_STAT: SelectObject(dc, g_font_small); h = text_height(dc, g_stat, w) + dpx(10); break;
        case ROW_TEXT: SelectObject(dc, g_font_small); h = text_height(dc, r->label, w) + dpx(14); break;
        case ROW_FORMAT: h = dpx(150); break;
        case ROW_LIST: h = max(dpx(60), rc.bottom - y - dpx(12)); break;
        }
        SetRect(&r->rc, x, y, x + w, y + h);
        if (r->kind == ROW_TOOL) { r->rc.left = 0; r->rc.right = rc.right; }
        y += h + (r->kind == ROW_BUTTON ? dpx(4) : dpx(2));
    }
}

static void pane_place_controls(void)
{
    int i;
    BOOL fmt = FALSE, list = FALSE;
    for (i = 0; i < g_nrows; i++) {
        RECT r = g_rows[i].rc;
        if (g_rows[i].kind == ROW_FORMAT) {
            int x = r.left, y = r.top, w = r.right - r.left, b = dpx(28);
            fmt = TRUE;
            MoveWindow(g_font_cb, x, y, w, dpx(300), TRUE);
            MoveWindow(g_size_cb, x, y + dpx(36), dpx(70), dpx(300), TRUE);
            MoveWindow(g_color_btn, x + dpx(78), y + dpx(36), dpx(40), b, TRUE);
            MoveWindow(g_bold_btn, x + dpx(126), y + dpx(36), b, b, TRUE);
            MoveWindow(g_italic_btn, x + dpx(126) + b + dpx(4), y + dpx(36), b, b, TRUE);
            MoveWindow(g_align_btn[0], x, y + dpx(74), b + dpx(12), b, TRUE);
            MoveWindow(g_align_btn[1], x + b + dpx(16), y + dpx(74), b + dpx(12), b, TRUE);
            MoveWindow(g_align_btn[2], x + 2 * (b + dpx(16)), y + dpx(74), b + dpx(12), b, TRUE);
        }
        if (g_rows[i].kind == ROW_LIST) {
            list = TRUE;
            MoveWindow(g_list, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
        }
    }
    ShowWindow(g_font_cb, fmt ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_size_cb, fmt ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_color_btn, fmt ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_bold_btn, fmt ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_italic_btn, fmt ? SW_SHOWNA : SW_HIDE);
    for (i = 0; i < 3; i++) ShowWindow(g_align_btn[i], fmt ? SW_SHOWNA : SW_HIDE);
    ShowWindow(g_list, list ? SW_SHOWNA : SW_HIDE);
}

static BOOL row_enabled(const row_t *r)
{
    if (r->kind != ROW_TOOL && r->kind != ROW_BUTTON) return FALSE;
    if (r->cmd == CMD_COMBINE || r->cmd == CMD_CREATE_FILES) return g.bridged;
    return cmd_enabled(r->cmd) && g.bridged;
}

static void pane_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC out = BeginPaint(hwnd, &ps), dc;
    RECT rc, r;
    HBITMAP buf, ob;
    HBRUSH bg = CreateSolidBrush(C_PANE), hov = CreateSolidBrush(C_HOVER), ln_b = CreateSolidBrush(C_LINE),
           btn = CreateSolidBrush(C_BAR);
    int i;
    GetClientRect(hwnd, &rc);
    dc = CreateCompatibleDC(out);
    buf = CreateCompatibleBitmap(out, max(rc.right, 1), max(rc.bottom, 1));
    ob = SelectObject(dc, buf);
    FillRect(dc, &rc, bg);
    SetRect(&r, 0, 0, 1, rc.bottom);
    FillRect(dc, &r, ln_b);
    SetBkMode(dc, TRANSPARENT);
    pane_layout(dc);
    for (i = 0; i < g_nrows; i++) {
        row_t *rw = &g_rows[i];
        BOOL en = row_enabled(rw);
        COLORREF col = en || rw->kind == ROW_HEAD || rw->kind == ROW_TEXT || rw->kind == ROW_STAT ? C_TEXT : C_DISABLED;
        r = rw->rc;
        switch (rw->kind) {
        case ROW_HEAD:
            SelectObject(dc, g_font_title);
            SetTextColor(dc, C_TEXT);
            DrawTextW(dc, rw->label, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            break;
        case ROW_TOOL: {
            RECT t = r;
            if (i == g_phover && en) FillRect(dc, &r, hov);
            glyph(dc, rw->glyph, r.left + dpx(30), (r.top + r.bottom) / 2, dpx(22), en ? C_ACCENT : C_DISABLED, C_ACCENT);
            t.left += dpx(56);
            t.bottom = (r.top + r.bottom) / 2 + dpx(2);
            SelectObject(dc, g_font);
            SetTextColor(dc, col);
            DrawTextW(dc, rw->label, -1, &t, DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
            t.top = t.bottom + dpx(1);
            t.bottom = r.bottom;
            SelectObject(dc, g_font_small);
            SetTextColor(dc, C_SUBTEXT);
            DrawTextW(dc, rw->desc, -1, &t, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            break;
        }
        case ROW_BUTTON: {
            RECT t = r, o = r;
            InflateRect(&o, 1, 1);
            FillRect(dc, &o, ln_b);
            FillRect(dc, &r, i == g_phover && en ? hov : btn);
            glyph(dc, rw->glyph, r.left + dpx(18), (r.top + r.bottom) / 2, dpx(16), col, C_ACCENT);
            t.left += dpx(36);
            SelectObject(dc, g_font);
            SetTextColor(dc, col);
            DrawTextW(dc, rw->label, -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            break;
        }
        case ROW_TEXT:
            SelectObject(dc, g_font_small);
            SetTextColor(dc, C_SUBTEXT);
            DrawTextW(dc, rw->label, -1, &r, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            break;
        case ROW_STAT:
            SelectObject(dc, g_font_small);
            SetTextColor(dc, g.tool == TOOL_REDACT && g.nredact ? C_REDMARK : C_SUBTEXT);
            DrawTextW(dc, g_stat, -1, &r, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            break;
        }
    }
    if (g.status[0] && g.tool != TOOL_NONE) {
        RECT sr = { dpx(14), rc.bottom - dpx(56), rc.right - dpx(14), rc.bottom - dpx(8) };
        BOOL list_row = FALSE;
        for (i = 0; i < g_nrows; i++) if (g_rows[i].kind == ROW_LIST) list_row = TRUE;
        if (!list_row) {
            SelectObject(dc, g_font_small);
            SetTextColor(dc, C_SUBTEXT);
            DrawTextW(dc, g.status, -1, &sr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_BOTTOM);
        }
    }
    BitBlt(out, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(buf);
    DeleteDC(dc);
    DeleteObject(bg); DeleteObject(hov); DeleteObject(ln_b); DeleteObject(btn);
    EndPaint(hwnd, &ps);
    pane_place_controls();
}

static int pane_hit(POINT pt)
{
    int i;
    for (i = 0; i < g_nrows; i++)
        if ((g_rows[i].kind == ROW_TOOL || g_rows[i].kind == ROW_BUTTON) && PtInRect(&g_rows[i].rc, pt)) return i;
    return -1;
}

/* ---- the lists (comments, fields) --------------------------------------------------------------------- */

typedef struct { int page, xref; } litem_t;
static litem_t *g_litems;
static int g_nlitems;

static void list_fill(void)
{
    int i;
    if (!g_list) return;
    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_list, LB_RESETCONTENT, 0, 0);
    free(g_litems);
    g_litems = NULL;
    g_nlitems = 0;
    if (g.tool == TOOL_COMMENT) {
        int total = 0, k = 0;
        for (i = 0; i < g.npages; i++) { doc_load_annots(i); total += g.pages[i].nannots; }
        g_litems = total ? calloc(total, sizeof(litem_t)) : NULL;
        for (i = 0; i < g.npages && g_litems; i++) {
            int a;
            for (a = 0; a < g.pages[i].nannots; a++) {
                annot_t *an = &g.pages[i].annots[a];
                WCHAR t[400];
                if (!strcmp(an->type, "Redact")) continue;
                {
                    WCHAR extra[64] = L"";
                    if (an->replies) swprintf(extra, 64, L"  \x2022  %d repl%ls", an->replies, an->replies == 1 ? L"y" : L"ies");
                    if (an->status[0]) { int n2 = lstrlenW(extra); swprintf(extra + n2, 64 - n2, L"  \x2022  %hs", an->status); }
                    swprintf(t, 400, L"Page %d  \x2022  %hs%ls%ls%ls%ls%ls", i + 1, an->type,
                             an->author && an->author[0] ? L"  \x2022  " : L"", an->author ? an->author : L"", extra,
                             an->contents && an->contents[0] ? L"\n" : L"", an->contents ? an->contents : L"");
                }
                SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)t);
                g_litems[k].page = i;
                g_litems[k].xref = an->xref;
                k++;
            }
        }
        g_nlitems = k;
    } else if (g.tool == TOOL_FORM) {
        g_litems = g.nfields ? calloc(g.nfields, sizeof(litem_t)) : NULL;
        for (i = 0; i < g.nfields && g_litems; i++) {
            field_t *f = &g.fields[i];
            WCHAR t[400];
            swprintf(t, 400, L"Page %d  \x2022  %ls%ls%ls\n%ls", f->page + 1, field_kind_name(f),
                     f->calc && f->calc[0] ? L"  \x2022  calculated" : L"", f->flags & FF_REQUIRED ? L"  \x2022  required" : L"",
                     f->name ? f->name : L"(field)");
            SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)t);
            g_litems[i].page = f->page;
            g_litems[i].xref = f->xref;
        }
        g_nlitems = g_litems ? g.nfields : 0;
    } else if (g.tool == TOOL_FILL) {
        g_litems = g.nfields ? calloc(g.nfields, sizeof(litem_t)) : NULL;
        for (i = 0; i < g.nfields && g_litems; i++) {
            field_t *f = &g.fields[i];
            WCHAR t[400];
            const WCHAR *v = f->value ? f->value : L"";
            if (f->type == FLD_CHECK || f->type == FLD_RADIO) v = f->value && !wcscmp(f->value, L"1") ? L"\x2611 on" : L"\x2610 off";
            swprintf(t, 400, L"%ls\n%ls", f->name ? f->name : L"(field)", v);
            SendMessageW(g_list, LB_ADDSTRING, 0, (LPARAM)t);
            g_litems[i].page = f->page;
            g_litems[i].xref = f->xref;
        }
        g_nlitems = g_litems ? g.nfields : 0;
    }
    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, NULL, TRUE);
}

static void list_measure(MEASUREITEMSTRUCT *mi)
{
    mi->itemHeight = dpx(46);
}

static void list_draw(DRAWITEMSTRUCT *di)
{
    WCHAR t[400], *nl;
    RECT r = di->rcItem, a, b;
    HBRUSH bg;
    if (di->itemID == (UINT)-1) return;
    SendMessageW(g_list, LB_GETTEXT, di->itemID, (LPARAM)t);
    bg = CreateSolidBrush(di->itemState & ODS_SELECTED ? C_ACTIVE : C_BAR);
    FillRect(di->hDC, &r, bg);
    DeleteObject(bg);
    if (g.tool == TOOL_COMMENT && (int)di->itemID < g_nlitems) {
        page_t *p = &g.pages[g_litems[di->itemID].page];
        int k;
        for (k = 0; k < p->nannots; k++) if (p->annots[k].xref == g_litems[di->itemID].xref) {
            HBRUSH c = CreateSolidBrush(p->annots[k].color);
            RECT s = { r.left + dpx(4), r.top + dpx(6), r.left + dpx(8), r.bottom - dpx(6) };
            FillRect(di->hDC, &s, c);
            DeleteObject(c);
        }
    }
    SetBkMode(di->hDC, TRANSPARENT);
    nl = wcschr(t, '\n');
    if (nl) *nl++ = 0;
    a = r; a.left += dpx(14); a.right -= dpx(4); a.top += dpx(4); a.bottom = r.top + dpx(22);
    b = a; b.top = a.bottom; b.bottom = r.bottom - dpx(2);
    SelectObject(di->hDC, g_font_small);
    SetTextColor(di->hDC, C_SUBTEXT);
    DrawTextW(di->hDC, t, -1, &a, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (nl) {
        SelectObject(di->hDC, g_font);
        SetTextColor(di->hDC, C_TEXT);
        DrawTextW(di->hDC, nl, -1, &b, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}

static void list_select(int i)
{
    if (i < 0 || i >= g_nlitems) return;
    view_goto_page(g_litems[i].page, 0);
    if (g.tool == TOOL_FORM) {
        int k;
        for (k = 0; k < g.nfields; k++) if (g.fields[k].xref == g_litems[i].xref) {
            form_pick(k);
            view_goto_page(g.fields[k].page, max(0.0f, g.fields[k].box.y1 - 40));
        }
        InvalidateRect(g_view, NULL, FALSE);
        InvalidateRect(g_tbar, NULL, FALSE);
    }
    if (g.tool == TOOL_COMMENT) {
        page_t *p = &g.pages[g_litems[i].page];
        int k;
        doc_load_annots(g_litems[i].page);
        for (k = 0; k < p->nannots; k++) if (p->annots[k].xref == g_litems[i].xref) {
            g.pick.kind = PICK_ANNOT;
            g.pick.page = g_litems[i].page;
            g.pick.index = k;
            view_goto_page(g_litems[i].page, max(0.0f, p->annots[k].box.y1 - 40));
        }
        InvalidateRect(g_view, NULL, FALSE);
        InvalidateRect(g_tbar, NULL, FALSE);
    }
}

/* ---- the format (Edit PDF) --------------------------------------------------------------------------- */

static const WCHAR *const FONTS[] = { L"Helvetica", L"Arial", L"Times New Roman", L"Courier New", L"Calibri", L"Cambria",
                                      L"Liberation Sans", L"Liberation Serif", L"Liberation Mono", L"DejaVu Sans",
                                      L"DejaVu Serif", L"Carlito", L"Caladea" };
static const int SIZES[] = { 6, 7, 8, 9, 10, 11, 12, 14, 16, 18, 20, 24, 28, 32, 36, 48, 72 };

static obj_t *picked_text(void)
{
    if (g.pick.kind != PICK_OBJ || g.pick.page < 0 || g.pick.page >= g.npages) return NULL;
    if (g.pick.index < 0 || g.pick.index >= g.pages[g.pick.page].nobjs) return NULL;
    if (g.pages[g.pick.page].objs[g.pick.index].kind != OBJ_TEXT) return NULL;
    return &g.pages[g.pick.page].objs[g.pick.index];
}

static void format_show(void)
{
    WCHAR t[32];
    int i;
    if (!g_font_cb) return;
    g_format_busy = TRUE;
    SetWindowTextW(g_font_cb, g.fmt_font);
    swprintf(t, 32, L"%g", floor(g.fmt_size * 10 + 0.5) / 10);
    SetWindowTextW(g_size_cb, t);
    SendMessageW(g_bold_btn, BM_SETCHECK, g.fmt_style & 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_italic_btn, BM_SETCHECK, g.fmt_style & 2 ? BST_CHECKED : BST_UNCHECKED, 0);
    for (i = 0; i < 3; i++) SendMessageW(g_align_btn[i], BM_SETCHECK, g.fmt_align == i ? BST_CHECKED : BST_UNCHECKED, 0);
    InvalidateRect(g_color_btn, NULL, FALSE);
    g_format_busy = FALSE;
}

void toolui_format_from_pick(void)
{
    obj_t *o = picked_text();
    if (!o) return;
    lstrcpynW(g.fmt_font, o->font[0] ? o->font : L"Helvetica", 64);
    g.fmt_size = o->size > 0 ? o->size : 12;
    g.fmt_color = o->color;
    g.fmt_style = o->style;
    g.fmt_align = o->align;
    format_show();
}

/* a format control changed: the new-text format, and the picked text block */
static void format_changed(void)
{
    WCHAR t[64];
    obj_t *o;
    char *etext, *efont;
    float size;
    if (g_format_busy) return;
    GetWindowTextW(g_font_cb, t, 64);
    if (t[0]) lstrcpynW(g.fmt_font, t, 64);
    GetWindowTextW(g_size_cb, t, 64);
    size = (float)_wtof(t);
    if (size >= 1 && size <= 500) g.fmt_size = size;
    g.fmt_style = (SendMessageW(g_bold_btn, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0) |
                  (SendMessageW(g_italic_btn, BM_GETCHECK, 0, 0) == BST_CHECKED ? 2 : 0);
    if (!(o = picked_text()) || !o->text) return;
    if (!wcscmp(o->font, g.fmt_font) && fabsf(o->size - g.fmt_size) < 0.05f && o->color == g.fmt_color &&
        o->style == g.fmt_style && o->align == g.fmt_align)
        return;
    etext = esc_utf8(o->text);
    efont = esc_utf8(g.fmt_font);
    if (etext && efont) {
        int page = g.pick.page, index = g.pick.index;
        if (doc_requestf("edittext\t%d\t%d\t-\t%s\tfont=%s\tsize=%.2f\tcolor=%02X%02X%02X\tbold=%d\titalic=%d\talign=%d",
                         page, o->id, etext, efont, g.fmt_size, GetRValue(g.fmt_color), GetGValue(g.fmt_color),
                         GetBValue(g.fmt_color), g.fmt_style & 1, (g.fmt_style >> 1) & 1, g.fmt_align)) {
            /* the block is at the same place: pick it again */
            doc_load_objects(page);
            if (index < g.pages[page].nobjs && g.pages[page].objs[index].kind == OBJ_TEXT) {
                g.pick.kind = PICK_OBJ;
                g.pick.page = page;
                g.pick.index = index;
            }
        }
    }
    free(etext);
    free(efont);
    InvalidateRect(g_view, NULL, FALSE);
}

static void choose_color(void)
{
    static COLORREF custom[16];
    CHOOSECOLORW cc = { sizeof(cc) };
    cc.hwndOwner = g_main;
    cc.rgbResult = g.fmt_color;
    cc.lpCustColors = custom;
    cc.Flags = CC_RGBINIT | CC_FULLOPEN;
    if (!ChooseColorW(&cc)) return;
    g.fmt_color = cc.rgbResult;
    InvalidateRect(g_color_btn, NULL, FALSE);
    format_changed();
}

static LRESULT CALLBACK pane_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    static HBRUSH pb;
    static COLORREF pbc = (COLORREF)-1;
    switch (msg) {
    case WM_PAINT: pane_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_MOUSEMOVE: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = pane_hit(pt);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        if (h != g_phover) { g_phover = h; InvalidateRect(hwnd, NULL, FALSE); }
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: g_phover = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        g_ppress = pane_hit(pt);
        if (g_ppress >= 0) SetCapture(hwnd);
        return 0;
    }
    case WM_LBUTTONUP: {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = pane_hit(pt), p = g_ppress;
        g_ppress = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (h >= 0 && h == p && row_enabled(&g_rows[h])) app_command(g_rows[h].cmd);
        return 0;
    }
    case WM_MEASUREITEM:
        if (((MEASUREITEMSTRUCT *)lp)->CtlType == ODT_LISTBOX) { list_measure((MEASUREITEMSTRUCT *)lp); return TRUE; }
        break;
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        if (di->hwndItem == g_list) { list_draw(di); return TRUE; }
        if (di->hwndItem == g_color_btn) {
            HBRUSH c = CreateSolidBrush(g.fmt_color), fr = CreateSolidBrush(C_LINE);
            RECT r = di->rcItem;
            FillRect(di->hDC, &r, fr);
            InflateRect(&r, -dpx(4), -dpx(4));
            FillRect(di->hDC, &r, c);
            DeleteObject(c);
            DeleteObject(fr);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND: {
        HWND c = (HWND)lp;
        int code = HIWORD(wp);
        if (c == g_list && code == LBN_SELCHANGE) list_select((int)SendMessageW(g_list, LB_GETCURSEL, 0, 0));
        else if (c == g_list && code == LBN_DBLCLK) {
            int k = (int)SendMessageW(g_list, LB_GETCURSEL, 0, 0);
            list_select(k);
            if (g.tool == TOOL_FORM && form_picked() >= 0) form_props(form_picked());
            else if (g.tool == TOOL_COMMENT && g.pick.kind == PICK_ANNOT) comment_thread(g.pick.page, g.pick.index);
        }
        else if (c == g_color_btn && code == BN_CLICKED) choose_color();
        else if ((c == g_bold_btn || c == g_italic_btn) && code == BN_CLICKED) format_changed();
        else if ((c == g_align_btn[0] || c == g_align_btn[1] || c == g_align_btn[2]) && code == BN_CLICKED) {
            g.fmt_align = c == g_align_btn[0] ? 0 : c == g_align_btn[1] ? 1 : 2;
            format_show();
            format_changed();
        } else if ((c == g_font_cb || c == g_size_cb) && (code == CBN_SELCHANGE || code == CBN_KILLFOCUS)) {
            if (code == CBN_SELCHANGE) {
                WCHAR t[64];
                int k = (int)SendMessageW(c, CB_GETCURSEL, 0, 0);
                if (k >= 0) { SendMessageW(c, CB_GETLBTEXT, k, (LPARAM)t); SetWindowTextW(c, t); }
            }
            format_changed();
        }
        return 0;
    }
    case WM_CTLCOLORLISTBOX: case WM_CTLCOLOREDIT: case WM_CTLCOLORBTN: case WM_CTLCOLORSTATIC:
        if (pbc != C_BAR) { if (pb) DeleteObject(pb); pb = CreateSolidBrush(C_BAR); pbc = C_BAR; }
        SetBkColor((HDC)wp, C_BAR);
        SetTextColor((HDC)wp, C_TEXT);
        return (LRESULT)pb;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* the size box: Enter applies */
static WNDPROC g_cb_edit_proc;
static LRESULT CALLBACK cb_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { format_changed(); SetFocus(g_view); return 0; }
    if (msg == WM_CHAR && wp == '\r') return 0;
    return CallWindowProcW(g_cb_edit_proc, hwnd, msg, wp, lp);
}

/* ---- the whole ------------------------------------------------------------------------------------------ */

int toolui_bar_height(void)
{
    return g.tool != TOOL_NONE ? TBAR_H : 0;
}

int toolui_pane_width(void)
{
    return PANE_W;
}

void toolui_layout(void)
{
    app_layout();
}

void toolui_update(void)
{
    build_rows();
    if (g_list && (g.tool == TOOL_COMMENT || g.tool == TOOL_FILL || g.tool == TOOL_FORM) &&
        (g_list_gen != g.generation || g_list_tool != g.tool)) {
        g_list_gen = g.generation;
        g_list_tool = g.tool;
        list_fill();
    }
    if (g.tool == TOOL_EDIT) format_show();
    if (g_tbar) InvalidateRect(g_tbar, NULL, FALSE);
    if (g_pane) InvalidateRect(g_pane, NULL, FALSE);
}

void tool_set(int tool)
{
    int old = g.tool;
    tool_commit_editor();
    tool_cancel();
    if (tool == TOOL_REDACT || tool == TOOL_EDIT || tool == TOOL_COMMENT || tool == TOOL_FILL || tool == TOOL_ORGANIZE ||
        tool == TOOL_FORM) {
        if (!g.npages || !g.bridged) tool = TOOL_NONE;
    }
    g.tool = tool;
    g.sub = tool == TOOL_REDACT ? SUB_MARKTEXT : SUB_SELECT;
    g.pick.kind = PICK_NONE;
    g_list_gen = -1;
    if (tool != TOOL_NONE && !g.pane) g.pane = TRUE;
    if ((old == TOOL_ORGANIZE) != (tool == TOOL_ORGANIZE)) org_show(tool == TOOL_ORGANIZE);
    if (tool == TOOL_FORM && !g.fields_loaded) doc_load_fields();
    if (tool != TOOL_NONE && g.perms != 0xFFFF) {
        unsigned need = tool == TOOL_COMMENT ? 32 : tool == TOOL_FILL ? 256 | 32 : tool == TOOL_ORGANIZE ? 1024 | 8 : 8;
        if (!(g.perms & need)) app_set_status(L"This document is secured: its security does not allow these changes. "
                                              L"Use File > Protect > Enter Permissions Password.");
    }
    app_layout();
    toolui_update();
    InvalidateRect(g_view, NULL, FALSE);
    app_status_changed();
}

void tool_set_sub(int sub)
{
    tool_commit_editor();
    tool_cancel();
    g.sub = sub;
    if (sub == SUB_ADDIMAGE) {
        if (!file_dialog(FALSE, L"Add Image", L"Pictures\0*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.tif;*.tiff\0All files (*.*)\0*.*\0",
                         NULL, g.image_path, MAX_PATH)) {
            g.sub = SUB_SELECT;
            g.image_path[0] = 0;
        }
    }
    if (sub != SUB_SELECT && sub != SUB_MARKTEXT) g.pick.kind = PICK_NONE;
    toolui_update();
    InvalidateRect(g_view, NULL, FALSE);
}

void toolui_register(void)
{
    WNDCLASSW wc = { 0 };
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpfnWndProc = tbar_proc;
    wc.lpszClassName = L"SgPdfToolBar";
    RegisterClassW(&wc);
    wc.lpfnWndProc = pane_proc;
    wc.lpszClassName = L"SgPdfPane";
    RegisterClassW(&wc);
}

void toolui_create(HWND parent)
{
    int i;
    HWND edit;
    TTTOOLINFOW ti = { sizeof(ti) };
    g_tbar = CreateWindowExW(0, L"SgPdfToolBar", NULL, WS_CHILD, 0, 0, 10, 10, parent, NULL, g_inst, NULL);
    g_pane = CreateWindowExW(0, L"SgPdfPane", NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 10, 10, parent, NULL, g_inst, NULL);
    g_ttip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0,
                             parent, NULL, g_inst, NULL);
    ti.uFlags = TTF_SUBCLASS;
    ti.hwnd = g_tbar;
    ti.uId = 1;
    ti.lpszText = L"";
    if (g_ttip) SendMessageW(g_ttip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    g_list = CreateWindowExW(0, L"LISTBOX", NULL, WS_CHILD | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS |
                             LBS_NOINTEGRALHEIGHT, 0, 0, 10, 10, g_pane, NULL, g_inst, NULL);
    g_font_cb = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL,
                                0, 0, 10, 200, g_pane, NULL, g_inst, NULL);
    g_size_cb = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VSCROLL | WS_TABSTOP | CBS_DROPDOWN | CBS_AUTOHSCROLL,
                                0, 0, 10, 200, g_pane, NULL, g_inst, NULL);
    g_color_btn = CreateWindowExW(0, L"BUTTON", L"Colour", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10, g_pane, NULL, g_inst, NULL);
    g_bold_btn = CreateWindowExW(0, L"BUTTON", L"B", WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 10, 10, g_pane, NULL, g_inst, NULL);
    g_italic_btn = CreateWindowExW(0, L"BUTTON", L"I", WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, 0, 10, 10, g_pane, NULL, g_inst, NULL);
    g_align_btn[0] = CreateWindowExW(0, L"BUTTON", L"Left", WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE | WS_GROUP, 0, 0, 10, 10, g_pane, NULL, g_inst, NULL);
    g_align_btn[1] = CreateWindowExW(0, L"BUTTON", L"Centre", WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE, 0, 0, 10, 10, g_pane, NULL, g_inst, NULL);
    g_align_btn[2] = CreateWindowExW(0, L"BUTTON", L"Right", WS_CHILD | WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE, 0, 0, 10, 10, g_pane, NULL, g_inst, NULL);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font_small, FALSE);
    SendMessageW(g_font_cb, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(g_size_cb, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(g_bold_btn, WM_SETFONT, (WPARAM)g_font_bold, FALSE);
    SendMessageW(g_italic_btn, WM_SETFONT, (WPARAM)g_font, FALSE);
    for (i = 0; i < 3; i++) SendMessageW(g_align_btn[i], WM_SETFONT, (WPARAM)g_font_small, FALSE);
    for (i = 0; i < (int)(sizeof(FONTS) / sizeof(FONTS[0])); i++) SendMessageW(g_font_cb, CB_ADDSTRING, 0, (LPARAM)FONTS[i]);
    for (i = 0; i < (int)(sizeof(SIZES) / sizeof(SIZES[0])); i++) {
        WCHAR t[8];
        swprintf(t, 8, L"%d", SIZES[i]);
        SendMessageW(g_size_cb, CB_ADDSTRING, 0, (LPARAM)t);
    }
    edit = FindWindowExW(g_size_cb, NULL, L"EDIT", NULL);
    if (edit) g_cb_edit_proc = (WNDPROC)SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)cb_edit_proc);
    edit = FindWindowExW(g_font_cb, NULL, L"EDIT", NULL);
    if (edit && g_cb_edit_proc) SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)cb_edit_proc);
    build_rows();
}

void toolui_fonts(void)
{
    int i;
    if (!g_list) return;
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font_small, FALSE);
    SendMessageW(g_list, LB_SETITEMHEIGHT, 0, dpx(46));
    SendMessageW(g_font_cb, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(g_size_cb, WM_SETFONT, (WPARAM)g_font, FALSE);
    SendMessageW(g_bold_btn, WM_SETFONT, (WPARAM)g_font_bold, FALSE);
    SendMessageW(g_italic_btn, WM_SETFONT, (WPARAM)g_font, FALSE);
    for (i = 0; i < 3; i++) SendMessageW(g_align_btn[i], WM_SETFONT, (WPARAM)g_font_small, FALSE);
    if (g_tbar) InvalidateRect(g_tbar, NULL, TRUE);
    if (g_pane) InvalidateRect(g_pane, NULL, TRUE);
}

void toolui_dump(FILE *f)
{
    int i;
    char buf[512];
    RECT r;
    fprintf(f, "tool %d %s\n", g.tool, g.tool == TOOL_EDIT ? "edit" : g.tool == TOOL_COMMENT ? "comment" :
            g.tool == TOOL_FILL ? "fill" : g.tool == TOOL_REDACT ? "redact" : g.tool == TOOL_ORGANIZE ? "organize" :
            g.tool == TOOL_FORM ? "form" : "none");
    fprintf(f, "sub %d\n", g.sub);
    fprintf(f, "pane %d\n", g.pane && g_pane && IsWindowVisible(g_pane));
    if (g_tbar && IsWindowVisible(g_tbar)) {
        HDC dc = GetDC(g_tbar);
        tbar_layout(dc);
        ReleaseDC(g_tbar, dc);
        for (i = 0; i < g_tb_n; i++) {
            const tdef_t *d = def_of(g_tb_cmd[i]);
            r = g_tb_rc[i];
            MapWindowPoints(g_tbar, NULL, (POINT *)&r, 2);
            if (g_tb_cmd[i] >= CMD_COLOR && g_tb_cmd[i] < CMD_COLOR + NCOLORS)
                fprintf(f, "tbtn color%d %ld %ld 0 1\n", g_tb_cmd[i] - CMD_COLOR, (r.left + r.right) / 2, (r.top + r.bottom) / 2);
            else if (d)
                fprintf(f, "tbtn %s %ld %ld %d %d\n", d->name, (r.left + r.right) / 2, (r.top + r.bottom) / 2,
                        g_tb_cmd[i] >= CMD_SUB && g_tb_cmd[i] - CMD_SUB == g.sub, cmd_enabled(g_tb_cmd[i]));
        }
        r = g_close_rc;
        MapWindowPoints(g_tbar, NULL, (POINT *)&r, 2);
        fprintf(f, "tbtn close %ld %ld 0 1\n", (r.left + r.right) / 2, (r.top + r.bottom) / 2);
    }
    if (g_pane && IsWindowVisible(g_pane)) {
        HDC dc = GetDC(g_pane);
        pane_layout(dc);
        ReleaseDC(g_pane, dc);
        for (i = 0; i < g_nrows; i++) {
            if (!g_rows[i].name) continue;
            r = g_rows[i].rc;
            MapWindowPoints(g_pane, NULL, (POINT *)&r, 2);
            fprintf(f, "panebtn %s %ld %ld %d\n", g_rows[i].name, (r.left + r.right) / 2, (r.top + r.bottom) / 2, row_enabled(&g_rows[i]));
        }
        to_utf8(g_stat, buf, sizeof(buf));
        fprintf(f, "panestat %s\n", buf);
        if (g_list && IsWindowVisible(g_list)) {
            int n = (int)SendMessageW(g_list, LB_GETCOUNT, 0, 0);
            fprintf(f, "listitems %d\n", n);
            for (i = 0; i < n && i < g_nlitems; i++) {
                WCHAR t[400], *nl;
                RECT ir;
                SendMessageW(g_list, LB_GETTEXT, i, (LPARAM)t);
                for (nl = t; *nl; nl++) if (*nl == '\n') *nl = '|';
                to_utf8(t, buf, sizeof(buf));
                if (SendMessageW(g_list, LB_GETITEMRECT, i, (LPARAM)&ir) != LB_ERR) {
                    MapWindowPoints(g_list, NULL, (POINT *)&ir, 2);
                    fprintf(f, "listitem %d %d %d %ld %ld %s\n", i, g_litems[i].page + 1, g_litems[i].xref,
                            (ir.left + ir.right) / 2, (ir.top + ir.bottom) / 2, buf);
                }
            }
        }
        if (g_font_cb && IsWindowVisible(g_font_cb)) {
            RECT fr;
            to_utf8(g.fmt_font, buf, sizeof(buf));
            fprintf(f, "format %s %.1f %06X %d %d\n", buf, g.fmt_size, (unsigned)g.fmt_color, g.fmt_style, g.fmt_align);
            GetWindowRect(g_size_cb, &fr);
            fprintf(f, "sizebox %ld %ld\n", fr.left + dpx(20), (fr.top + fr.bottom) / 2);
            GetWindowRect(g_bold_btn, &fr);
            fprintf(f, "boldbtn %ld %ld\n", (fr.left + fr.right) / 2, (fr.top + fr.bottom) / 2);
        }
    }
}
