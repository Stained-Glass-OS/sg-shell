/* sg-pdf -- SG PDF: Prepare Form -- making a fillable form.
 *
 * The tool shows every field as a box with its name; its bar adds fields of
 * each kind (text, date, number, check box, radio button, drop-down, list
 * box, signature) where a box is dragged, or a box of the kind's usual size
 * where the page is clicked; Auto-detect makes fields of the blanks the
 * pages suggest (underscores, rules, empty boxes and table cells, small
 * squares). A field is picked with a click, dragged to move it, resized by
 * its handles, deleted with Del, nudged with the arrow keys; a double-click
 * (or Enter, or Properties) opens its properties: name, tooltip, required,
 * read-only, multi-line, font size, alignment, default value, format
 * (number, currency, percent, date, ZIP code, phone, social security
 * number), calculation (sum, product, average, minimum, maximum of fields,
 * or a formula like "Qty * Price"), choices, export value.
 *
 * sg-pdf (sgpdf_forms.py) makes the fields as standard AcroForm fields with
 * the standard format and calculation scripts, so other readers fill and
 * compute them the same way.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"
#include "resource.h"

enum { FD_NONE, FD_ARMED, FD_MOVE, FD_RESIZE, FD_NEW };

static struct {
    int mode, page, handle, index;
    float x0, y0, x1, y1;
    frect orig;
    POINT down;
} F;

static const char *const KIND_OF_SUB[] = { "text", "date", "number", "checkbox", "radio", "combo", "list", "signature" };

const WCHAR *field_kind_name(const field_t *f)
{
    if (f->type == FLD_TEXT && f->fmt && !wcsncmp(f->fmt, L"date", 4)) return L"Date";
    if (f->type == FLD_TEXT && f->fmt && (!wcsncmp(f->fmt, L"number", 6) || !wcsncmp(f->fmt, L"percent", 7))) return L"Number";
    switch (f->type) {
    case FLD_TEXT: return L"Text";
    case FLD_CHECK: return L"Check box";
    case FLD_RADIO: return L"Radio button";
    case FLD_COMBO: return L"Drop-down";
    case FLD_LIST: return L"List box";
    case FLD_BUTTON: return L"Button";
    case FLD_SIGNATURE: return L"Signature";
    }
    return L"Field";
}

int form_picked(void)
{
    if (g.pick.kind != PICK_FIELD || g.pick.index < 0 || g.pick.index >= g.nfields) return -1;
    return g.pick.index;
}

void form_pick(int index)
{
    if (index < 0 || index >= g.nfields) { g.pick.kind = PICK_NONE; return; }
    g.pick.kind = PICK_FIELD;
    g.pick.page = g.fields[index].page;
    g.pick.index = index;
}

static int index_of_xref(int xref)
{
    int i;
    for (i = 0; i < g.nfields; i++) if (g.fields[i].xref == xref) return i;
    return -1;
}

static void to_client(int page, const frect *b, RECT *r)
{
    view_box_to_client(page, b, r);
}

static frect norm(float x0, float y0, float x1, float y1)
{
    frect r = { min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1) };
    return r;
}

static int field_at(int page, POINT pt)
{
    int i, best = -1;
    double best_area = 1e30;
    for (i = 0; i < g.nfields; i++) {
        RECT r;
        double a;
        if (g.fields[i].page != page) continue;
        to_client(page, &g.fields[i].box, &r);
        InflateRect(&r, dpx(2), dpx(2));
        if (!PtInRect(&r, pt)) continue;
        a = (double)(r.right - r.left) * (r.bottom - r.top);
        if (a < best_area) { best_area = a; best = i; }
    }
    return best;
}

static void handle_rects(const RECT *r, RECT h[8])
{
    int s = dpx(4), cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2, i;
    POINT c[8] = { { r->left, r->top }, { cx, r->top }, { r->right, r->top }, { r->right, cy },
                   { r->right, r->bottom }, { cx, r->bottom }, { r->left, r->bottom }, { r->left, cy } };
    for (i = 0; i < 8; i++) SetRect(&h[i], c[i].x - s, c[i].y - s, c[i].x + s + 1, c[i].y + s + 1);
}

static int handle_at(POINT pt)
{
    RECT r, h[8];
    int i, k = form_picked();
    if (k < 0) return -1;
    to_client(g.fields[k].page, &g.fields[k].box, &r);
    handle_rects(&r, h);
    for (i = 0; i < 8; i++) { InflateRect(&h[i], dpx(2), dpx(2)); if (PtInRect(&h[i], pt)) return i; }
    return -1;
}

static frect dragged(void)
{
    frect b = F.orig;
    float dx = F.x1 - F.x0, dy = F.y1 - F.y0;
    if (F.mode == FD_MOVE) { b.x1 += dx; b.x2 += dx; b.y1 += dy; b.y2 += dy; return b; }
    switch (F.handle) {
    case 0: b.x1 += dx; b.y1 += dy; break;
    case 1: b.y1 += dy; break;
    case 2: b.x2 += dx; b.y1 += dy; break;
    case 3: b.x2 += dx; break;
    case 4: b.x2 += dx; b.y2 += dy; break;
    case 5: b.y2 += dy; break;
    case 6: b.x1 += dx; b.y2 += dy; break;
    case 7: b.x1 += dx; break;
    }
    return norm(b.x1, b.y1, b.x2, b.y2);
}

static BOOL request_move(int index, frect b)
{
    int xref = g.fields[index].xref, page = g.fields[index].page;
    if (b.x2 - b.x1 < 4 || b.y2 - b.y1 < 4) return FALSE;
    if (!doc_requestf("movefield\t%d\t%d\t%.2f %.2f %.2f %.2f", page, xref, b.x1, b.y1, b.x2, b.y2)) return FALSE;
    form_pick(index_of_xref(xref));
    return TRUE;
}

/* a new field where the box was drawn (a click: the kind's usual size) */
static void add_field(int page, frect r, BOOL click)
{
    int k = g.sub - SUB_F_TEXT;
    char num[32];
    float w = (float)g.pages[page].w, h = (float)g.pages[page].h;
    if (k < 0 || k > SUB_F_SIGN - SUB_F_TEXT) return;
    if (click) {
        float fw = 160, fh = 20;
        switch (g.sub) {
        case SUB_F_CHECK: case SUB_F_RADIO: fw = fh = 14; break;
        case SUB_F_DATE: case SUB_F_NUMBER: fw = 100; break;
        case SUB_F_LIST: fw = 140; fh = 60; break;
        case SUB_F_SIGN: fw = 200; fh = 40; break;
        }
        if (g.sub == SUB_F_CHECK || g.sub == SUB_F_RADIO) { r.x1 -= fw / 2; r.y1 -= fh / 2; }
        r.x2 = r.x1 + fw;
        r.y2 = r.y1 + fh;
    }
    if (r.x1 < 0) { r.x2 -= r.x1; r.x1 = 0; }
    if (r.y1 < 0) { r.y2 -= r.y1; r.y1 = 0; }
    if (r.x2 > w) { r.x1 -= r.x2 - w; r.x2 = w; }
    if (r.y2 > h) { r.y1 -= r.y2 - h; r.y2 = h; }
#ifndef SG_MUTANT_FORMADD
    if (doc_requestf("addfield\t%d\t%s\t%.2f %.2f %.2f %.2f", page, KIND_OF_SUB[k], r.x1, r.y1, r.x2, r.y2)) {
#else
    if (0) {
#endif
        if (br_field(g_last_head, "xref", num, sizeof(num))) form_pick(index_of_xref(atoi(num)));
        app_set_status(L"Field added. Double-click it (or press Enter) for its properties.");
    }
    g.sub = SUB_SELECT;
    toolui_update();
}

void form_command(int cmd)
{
    int k = form_picked();
    switch (cmd) {
    case CMD_FORM_DETECT: {
        char num[32];
        int before = g.nfields;
        if (doc_request("formdetect\t\tadd=1")) {
            int found = br_field(g_last_head, "found", num, sizeof(num)) ? atoi(num) : 0;
            if (!found) app_set_status(L"No blanks to make fields of were found. Add fields with the tools above.");
            else app_set_status(L"%d field%ls added from the blanks on the pages.", g.nfields - before,
                                g.nfields - before == 1 ? L"" : L"s");
        }
        break;
    }
    case CMD_FORM_PROPS: if (k >= 0) form_props(k); break;
    case CMD_FORM_DELETE:
        if (k >= 0) {
            int page = g.fields[k].page, xref = g.fields[k].xref;
            g.pick.kind = PICK_NONE;
            doc_requestf("delfield\t%d\t%d", page, xref);
        }
        break;
    case CMD_FORM_CLEARALL: {
        int i;
        if (!g.nfields) break;
        if (MessageBoxW(g_main, L"Delete every field of the form?", L"Prepare Form", MB_OKCANCEL | MB_ICONQUESTION) != IDOK) break;
        g.pick.kind = PICK_NONE;
        for (i = 0; g.nfields > 0 && i < 10000; i++)
            if (!doc_requestf("delfield\t%d\t%d", g.fields[g.nfields - 1].page, g.fields[g.nfields - 1].xref)) break;
        break;
    }
    }
    toolui_update();
    InvalidateRect(g_view, NULL, FALSE);
}

BOOL form_mouse(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    int page, k;
    float x, y;
    (void)wp;
    if (msg == WM_LBUTTONDOWN) {
        SetFocus(hwnd);
        F.mode = FD_NONE;
        F.down = pt;
        page = view_page_at(pt, FALSE);
        if (page < 0) {
            if (g.pick.kind != PICK_NONE) { g.pick.kind = PICK_NONE; toolui_update(); InvalidateRect(hwnd, NULL, FALSE); }
            return TRUE;
        }
        view_client_to_page(page, pt.x, pt.y, &x, &y);
        F.page = page;
        F.x0 = F.x1 = x;
        F.y0 = F.y1 = y;
        SetCapture(hwnd);
        if (g.sub >= SUB_F_TEXT && g.sub <= SUB_F_SIGN) { F.mode = FD_NEW; return TRUE; }
        if ((k = handle_at(pt)) >= 0) {
            F.mode = FD_RESIZE;
            F.handle = k;
            F.index = form_picked();
            F.orig = g.fields[F.index].box;
            F.page = g.fields[F.index].page;
            return TRUE;
        }
        k = field_at(page, pt);
        if (k >= 0) {
            form_pick(k);
            F.index = k;
            F.orig = g.fields[k].box;
            F.mode = FD_ARMED;
        } else g.pick.kind = PICK_NONE;
        toolui_update();
        InvalidateRect(hwnd, NULL, FALSE);
        return TRUE;
    }
    if (msg == WM_MOUSEMOVE) {
        if (F.mode == FD_NONE || GetCapture() != hwnd) { form_setcursor(pt); return TRUE; }
        view_client_to_page(F.page, pt.x, pt.y, &x, &y);
        F.x1 = max(0.0f, min(x, (float)g.pages[F.page].w));
        F.y1 = max(0.0f, min(y, (float)g.pages[F.page].h));
        if (F.mode == FD_ARMED && abs(pt.x - F.down.x) + abs(pt.y - F.down.y) > dpx(3)) F.mode = FD_MOVE;
        InvalidateRect(hwnd, NULL, FALSE);
        return TRUE;
    }
    if (msg == WM_LBUTTONUP) {
        int mode = F.mode;
        BOOL small = abs(pt.x - F.down.x) + abs(pt.y - F.down.y) <= dpx(3);
        F.mode = FD_NONE;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (mode == FD_NEW) {
            frect r = norm(F.x0, F.y0, F.x1, F.y1);
            add_field(F.page, r, small || r.x2 - r.x1 < 6 || r.y2 - r.y1 < 6);
        } else if (mode == FD_MOVE || mode == FD_RESIZE) {
            F.mode = mode;
            {
                frect b = dragged();
                F.mode = FD_NONE;
                request_move(F.index, b);
            }
        }
        InvalidateRect(hwnd, NULL, FALSE);
        toolui_update();
        app_status_changed();
        return TRUE;
    }
    if (msg == WM_LBUTTONDBLCLK) {
        page = view_page_at(pt, FALSE);
        if (page >= 0 && (k = field_at(page, pt)) >= 0) {
            F.mode = FD_NONE;
            if (GetCapture() == hwnd) ReleaseCapture();
            form_pick(k);
            form_props(k);
        }
        return TRUE;
    }
    if (msg == WM_RBUTTONUP) {
        HMENU m;
        int cmd;
        page = view_page_at(pt, FALSE);
        if (page < 0 || (k = field_at(page, pt)) < 0) return TRUE;
        form_pick(k);
        InvalidateRect(hwnd, NULL, FALSE);
        m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, 1, L"&Properties...");
        AppendMenuW(m, MF_STRING, 2, L"&Delete");
        ClientToScreen(hwnd, &pt);
        cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, NULL);
        DestroyMenu(m);
        if (cmd == 1) form_props(k);
        else if (cmd == 2) form_command(CMD_FORM_DELETE);
        return TRUE;
    }
    return FALSE;
}

BOOL form_setcursor(POINT pt)
{
    int page = view_page_at(pt, FALSE), h;
    LPCWSTR c = (LPCWSTR)IDC_ARROW;
    if (g.sub >= SUB_F_TEXT && g.sub <= SUB_F_SIGN) c = page >= 0 ? (LPCWSTR)IDC_CROSS : (LPCWSTR)IDC_ARROW;
    else if ((h = handle_at(pt)) >= 0)
        c = (LPCWSTR)(h == 0 || h == 4 ? IDC_SIZENWSE : h == 2 || h == 6 ? IDC_SIZENESW : h == 1 || h == 5 ? IDC_SIZENS : IDC_SIZEWE);
    else if (page >= 0 && field_at(page, pt) >= 0) c = (LPCWSTR)IDC_SIZEALL;
    SetCursor(LoadCursorW(NULL, c));
    return TRUE;
}

BOOL form_key(WPARAM vk)
{
    int k = form_picked();
    if (vk == VK_ESCAPE) {
        if (F.mode != FD_NONE && GetCapture() == g_view) ReleaseCapture();
        F.mode = FD_NONE;
        g.sub = SUB_SELECT;
        g.pick.kind = PICK_NONE;
        toolui_update();
        InvalidateRect(g_view, NULL, FALSE);
        return TRUE;
    }
    if (k < 0) return FALSE;
    if (vk == VK_DELETE) { form_command(CMD_FORM_DELETE); return TRUE; }
    if (vk == VK_RETURN) { form_props(k); return TRUE; }
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) {
        float d = GetKeyState(VK_SHIFT) < 0 ? 10.0f : 1.0f;
        frect b = g.fields[k].box;
        if (vk == VK_LEFT) { b.x1 -= d; b.x2 -= d; }
        if (vk == VK_RIGHT) { b.x1 += d; b.x2 += d; }
        if (vk == VK_UP) { b.y1 -= d; b.y2 -= d; }
        if (vk == VK_DOWN) { b.y1 += d; b.y2 += d; }
        request_move(k, b);
        return TRUE;
    }
    return FALSE;
}

/* ---- drawing ----------------------------------------------------------------------------------------- */

static void frame(HDC dc, RECT r, COLORREF c, int style, int width)
{
    LOGBRUSH lb = { BS_SOLID, c, 0 };
    HPEN pen = style == PS_SOLID ? CreatePen(PS_SOLID, width, c) : ExtCreatePen(PS_GEOMETRIC | style, width, &lb, 0, NULL);
    HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

void form_paint(HDC dc)
{
    int i, picked = form_picked();
    RECT vr;
    COLORREF blue = RGB(0x1E, 0x64, 0xD8), tagbg = RGB(0xD6, 0xE6, 0xFF);
    HBRUSH tag = CreateSolidBrush(tagbg), fill = CreateSolidBrush(RGB(0xE8, 0xF0, 0xFF)), acc = CreateSolidBrush(blue);
    HFONT of = SelectObject(dc, g_font_small);
    GetClientRect(g_view, &vr);
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < g.nfields; i++) {
        field_t *f = &g.fields[i];
        RECT r, t, x;
        WCHAR label[128];
        SIZE sz;
        to_client(f->page, &f->box, &r);
        if (r.bottom < 0 || r.top > vr.bottom) continue;
        {
            HGDIOBJ o = SelectObject(dc, fill);
            PatBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0x00A000C9 /* DPa */);
            SelectObject(dc, o);
        }
        frame(dc, r, f->flags & FF_REQUIRED ? RGB(0xD0, 0x20, 0x20) : blue, PS_SOLID, i == picked ? max(2, dpx(2)) : 1);
        swprintf(label, 128, L"%ls%ls", f->name ? f->name : L"", f->flags & FF_REQUIRED ? L" *" : L"");
        GetTextExtentPoint32W(dc, label, lstrlenW(label), &sz);
        /* the name inside the field, at its left (over the page's own words outside it, never) */
        SetRect(&t, r.left + 1, r.top + 1, min(r.right - 1, r.left + sz.cx + dpx(9)), min(r.bottom - 1, r.top + sz.cy + dpx(2)));
        if (r.bottom - r.top > (sz.cy + dpx(2)) * 2) OffsetRect(&t, 0, 0);
        else OffsetRect(&t, 0, max(0, ((r.bottom - r.top) - (t.bottom - t.top)) / 2 - 1));
        if (f->type == FLD_CHECK || f->type == FLD_RADIO) {
            /* a small box's name goes beside it */
            SetRect(&t, r.right + dpx(3), (r.top + r.bottom - sz.cy) / 2 - dpx(1), r.right + dpx(11) + sz.cx,
                    (r.top + r.bottom + sz.cy) / 2 + dpx(1));
        }
        FillRect(dc, &t, i == picked ? acc : tag);
        SetTextColor(dc, i == picked ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x10, 0x30, 0x70));
        x = t;
        x.left += dpx(4);
        DrawTextW(dc, label, -1, &x, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (i == picked) {
            RECT h[8];
            int j;
            handle_rects(&r, h);
            for (j = 0; j < 8; j++) { FillRect(dc, &h[j], acc); InflateRect(&h[j], -1, -1); FillRect(dc, &h[j], GetStockObject(WHITE_BRUSH)); }
        }
    }
    if (F.mode == FD_MOVE || F.mode == FD_RESIZE) {
        frect b = dragged();
        RECT r;
        to_client(g.fields[F.index].page, &b, &r);
        frame(dc, r, blue, PS_DASH, 1);
    } else if (F.mode == FD_NEW) {
        frect b = norm(F.x0, F.y0, F.x1, F.y1);
        RECT r;
        to_client(F.page, &b, &r);
        frame(dc, r, blue, PS_DASH, 1);
    }
    SelectObject(dc, of);
    DeleteObject(tag);
    DeleteObject(fill);
    DeleteObject(acc);
}

void form_dump(FILE *f)
{
    int i;
    for (i = 0; i < g.nfields; i++) {
        RECT r;
        char name[256], fmt[64], calc[256];
        field_t *fl = &g.fields[i];
        to_client(fl->page, &fl->box, &r);
        MapWindowPoints(g_view, NULL, (POINT *)&r, 2);
        to_utf8(fl->name ? fl->name : L"", name, sizeof(name));
        to_utf8(fl->fmt ? fl->fmt : L"", fmt, sizeof(fmt));
        to_utf8(fl->calc ? fl->calc : L"", calc, sizeof(calc));
        fprintf(f, "formfield %d %d %d %ld %ld %ld %ld %d fmt=%s calc=%s name=%s\n", fl->page + 1, fl->xref, fl->type, r.left,
                r.top, r.right, r.bottom, fl->flags, fmt[0] ? fmt : "-", calc[0] ? calc : "-", name);
    }
    fprintf(f, "formpick %d\n", form_picked());
}

/* ---- the Properties dialog ---------------------------------------------------------------------------------- */

static const WCHAR *const FMT_LABELS[] = { L"None", L"Number", L"Currency", L"Percent", L"Date", L"ZIP code", L"ZIP+4",
                                           L"Phone number", L"Social security number" };
static const WCHAR *const DATE_PATTERNS[] = { L"mm/dd/yyyy", L"dd/mm/yyyy", L"yyyy-mm-dd", L"m/d/yyyy", L"d-mmm-yyyy",
                                              L"mmmm d, yyyy", L"mm/yy", L"dd.mm.yyyy" };
static const WCHAR *const CALC_LABELS[] = { L"None", L"Sum (+)", L"Product (\x00d7)", L"Average", L"Minimum", L"Maximum",
                                            L"Formula" };
static const char *const CALC_KEYS[] = { "none", "sum", "product", "avg", "min", "max", "expr" };

static int g_pi;            /* the field the dialog edits */

static void show(HWND dlg, int id, BOOL on)
{
    ShowWindow(GetDlgItem(dlg, id), on ? SW_SHOW : SW_HIDE);
}

static void props_enable(HWND dlg)
{
    field_t *f = &g.fields[g_pi];
    BOOL text = f->type == FLD_TEXT, choice = f->type == FLD_COMBO || f->type == FLD_LIST,
         box = f->type == FLD_CHECK || f->type == FLD_RADIO;
    int fmt = (int)SendDlgItemMessageW(dlg, IDC_FP_FORMAT, CB_GETCURSEL, 0, 0);
    int calc = (int)SendDlgItemMessageW(dlg, IDC_FP_CALC, CB_GETCURSEL, 0, 0);
    int id;
    for (id = IDC_FP_FMTLABEL; id <= IDC_FP_CALCPICK; id++) show(dlg, id, text);
    show(dlg, IDC_FP_MULTI, text);
    show(dlg, IDC_FP_DEFLABEL, text || choice);
    show(dlg, IDC_FP_DEFAULT, text || choice);
    show(dlg, IDC_FP_OPTLABEL, choice);
    show(dlg, IDC_FP_OPTIONS, choice);
    show(dlg, IDC_FP_EXPLABEL, box);
    show(dlg, IDC_FP_EXPORT, box);
    if (text) {
        show(dlg, IDC_FP_DECLABEL, fmt >= 1 && fmt <= 3);
        show(dlg, IDC_FP_DECIMALS, fmt >= 1 && fmt <= 3);
        show(dlg, IDC_FP_CURLABEL, fmt == 2);
        show(dlg, IDC_FP_CURRENCY, fmt == 2);
        show(dlg, IDC_FP_DATELABEL, fmt == 4);
        show(dlg, IDC_FP_DATEPAT, fmt == 4);
        show(dlg, IDC_FP_CALCFIELDS, calc > 0);
        show(dlg, IDC_FP_CALCPICK, calc > 0 && calc < 6);
        SetDlgItemTextW(dlg, IDC_FP_CALCHINT, calc == 6 ? L"Formula, e.g. Qty * Price + Tax (field names, + - * / and brackets):"
                                             : calc > 0 ? L"Of the fields (names, separated by commas):" : L"");
    }
}

/* the fields to calculate from, picked in a list */
static INT_PTR CALLBACK pick_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static WCHAR *buf;
    switch (msg) {
    case WM_INITDIALOG: {
        int i;
        buf = (WCHAR *)lp;
        for (i = 0; i < g.nfields; i++) {
            int k;
            BOOL dup = FALSE;
            if (i == g_pi || g.fields[i].type != FLD_TEXT || !g.fields[i].name) continue;
            for (k = 0; k < i; k++) if (g.fields[k].name && !wcscmp(g.fields[k].name, g.fields[i].name)) dup = TRUE;
            if (dup) continue;
            k = (int)SendDlgItemMessageW(dlg, IDC_PK_LIST, LB_ADDSTRING, 0, (LPARAM)g.fields[i].name);
            /* already named: selected */
            {
                WCHAR t[1024], *s, *ctx = NULL;
                lstrcpynW(t, buf, 1024);
                for (s = wcstok_s(t, L",", &ctx); s; s = wcstok_s(NULL, L",", &ctx)) {
                    while (*s == ' ') s++;
                    {
                        WCHAR *e = s + wcslen(s);
                        while (e > s && e[-1] == ' ') *--e = 0;
                    }
                    if (!wcscmp(s, g.fields[i].name)) SendDlgItemMessageW(dlg, IDC_PK_LIST, LB_SETSEL, TRUE, k);
                }
            }
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            int n = (int)SendDlgItemMessageW(dlg, IDC_PK_LIST, LB_GETCOUNT, 0, 0), i, len = 0;
            buf[0] = 0;
            for (i = 0; i < n; i++) {
                WCHAR t[256];
                if (SendDlgItemMessageW(dlg, IDC_PK_LIST, LB_GETSEL, i, 0) <= 0) continue;
                SendDlgItemMessageW(dlg, IDC_PK_LIST, LB_GETTEXT, i, (LPARAM)t);
                len += swprintf(buf + len, 1024 - len, L"%ls%ls", len ? L", " : L"", t);
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static void props_init(HWND dlg)
{
    field_t *f = &g.fields[g_pi];
    WCHAR t[256];
    int i, fmt = 0, dec = 2, calc = 0;
    const WCHAR *cur = L"$", *datepat = L"mm/dd/yyyy", *calcrest = L"";
    swprintf(t, 256, L"%ls Field Properties", field_kind_name(f));
    SetWindowTextW(dlg, t);
    SetDlgItemTextW(dlg, IDC_FP_NAME, f->name ? f->name : L"");
    SetDlgItemTextW(dlg, IDC_FP_TOOLTIP, f->tooltip ? f->tooltip : L"");
    CheckDlgButton(dlg, IDC_FP_REQUIRED, f->flags & FF_REQUIRED ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_FP_READONLY, f->flags & FF_READONLY ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_FP_MULTI, f->flags & FF_MULTILINE ? BST_CHECKED : BST_UNCHECKED);
    SendDlgItemMessageW(dlg, IDC_FP_FONTSIZE, CB_ADDSTRING, 0, (LPARAM)L"Auto");
    for (i = 6; i <= 24; i += (i < 12 ? 1 : 2)) { swprintf(t, 16, L"%d", i); SendDlgItemMessageW(dlg, IDC_FP_FONTSIZE, CB_ADDSTRING, 0, (LPARAM)t); }
    if (f->fontsize > 0) swprintf(t, 16, L"%g", f->fontsize); else lstrcpyW(t, L"Auto");
    SetDlgItemTextW(dlg, IDC_FP_FONTSIZE, t);
    SendDlgItemMessageW(dlg, IDC_FP_ALIGN, CB_ADDSTRING, 0, (LPARAM)L"Left");
    SendDlgItemMessageW(dlg, IDC_FP_ALIGN, CB_ADDSTRING, 0, (LPARAM)L"Center");
    SendDlgItemMessageW(dlg, IDC_FP_ALIGN, CB_ADDSTRING, 0, (LPARAM)L"Right");
    SendDlgItemMessageW(dlg, IDC_FP_ALIGN, CB_SETCURSEL, 0, 0);
    for (i = 0; i < 9; i++) SendDlgItemMessageW(dlg, IDC_FP_FORMAT, CB_ADDSTRING, 0, (LPARAM)FMT_LABELS[i]);
    for (i = 0; i <= 4; i++) { swprintf(t, 16, L"%d", i); SendDlgItemMessageW(dlg, IDC_FP_DECIMALS, CB_ADDSTRING, 0, (LPARAM)t); }
    for (i = 0; i < 8; i++) SendDlgItemMessageW(dlg, IDC_FP_DATEPAT, CB_ADDSTRING, 0, (LPARAM)DATE_PATTERNS[i]);
    for (i = 0; i < 7; i++) SendDlgItemMessageW(dlg, IDC_FP_CALC, CB_ADDSTRING, 0, (LPARAM)CALC_LABELS[i]);
    /* the format: "number:2:$:0", "percent:1", "date:pattern", "zip", ... */
    if (f->fmt && f->fmt[0]) {
        static WCHAR copy[128];
        WCHAR *a, *b2;
        lstrcpynW(copy, f->fmt, 128);
        a = wcschr(copy, ':');
        if (a) *a++ = 0;
        if (!wcscmp(copy, L"number")) {
            b2 = a ? wcschr(a, ':') : NULL;
            if (b2) *b2++ = 0;
            dec = a ? _wtoi(a) : 2;
            if (b2) { WCHAR *c3 = wcschr(b2, ':'); if (c3) *c3 = 0; }
            fmt = b2 && b2[0] ? 2 : 1;
            if (b2 && b2[0]) cur = b2;
        } else if (!wcscmp(copy, L"percent")) { fmt = 3; dec = a ? _wtoi(a) : 0; }
        else if (!wcscmp(copy, L"date")) { fmt = 4; if (a && a[0]) datepat = a; }
        else if (!wcscmp(copy, L"zip")) fmt = 5;
        else if (!wcscmp(copy, L"zip4")) fmt = 6;
        else if (!wcscmp(copy, L"phone")) fmt = 7;
        else if (!wcscmp(copy, L"ssn")) fmt = 8;
    }
    SendDlgItemMessageW(dlg, IDC_FP_FORMAT, CB_SETCURSEL, fmt, 0);
    SendDlgItemMessageW(dlg, IDC_FP_DECIMALS, CB_SETCURSEL, max(0, min(4, dec)), 0);
    SetDlgItemTextW(dlg, IDC_FP_CURRENCY, cur);
    SetDlgItemTextW(dlg, IDC_FP_DATEPAT, datepat);
    if (f->calc && f->calc[0]) {
        const WCHAR *colon = wcschr(f->calc, ':');
        for (i = 1; i < 7; i++) {
            WCHAR key[16];
            swprintf(key, 16, L"%hs", CALC_KEYS[i]);
            if (colon && (size_t)(colon - f->calc) == wcslen(key) && !wcsncmp(f->calc, key, wcslen(key))) calc = i;
        }
        if (colon) calcrest = colon + 1;
    }
    SendDlgItemMessageW(dlg, IDC_FP_CALC, CB_SETCURSEL, calc, 0);
    if (calc > 0 && calc < 6) {
        /* "a,b,c" shown as "a, b, c" */
        WCHAR shown[1024];
        int k = 0;
        for (i = 0; calcrest[i] && k < 1020; i++) { shown[k++] = calcrest[i]; if (calcrest[i] == ',') shown[k++] = ' '; }
        shown[k] = 0;
        SetDlgItemTextW(dlg, IDC_FP_CALCFIELDS, shown);
    } else SetDlgItemTextW(dlg, IDC_FP_CALCFIELDS, calcrest);
    if (f->options) {
        /* one a line in the box */
        size_t n = wcslen(f->options), k = 0, j;
        WCHAR *o = calloc(n * 2 + 1, sizeof(WCHAR));
        for (j = 0; o && j < n; j++) { if (f->options[j] == '\n') o[k++] = '\r'; o[k++] = f->options[j]; }
        if (o) { SetDlgItemTextW(dlg, IDC_FP_OPTIONS, o); free(o); }
    }
    if (f->type == FLD_CHECK || f->type == FLD_RADIO) SetDlgItemTextW(dlg, IDC_FP_EXPORT, L"");
    props_enable(dlg);
}

static char *dlg_esc(HWND dlg, int id)
{
    int n = GetWindowTextLengthW(GetDlgItem(dlg, id)), i, k = 0;
    WCHAR *t = calloc(n + 1, sizeof(WCHAR));
    char *e;
    if (!t) return NULL;
    GetDlgItemTextW(dlg, id, t, n + 1);
    for (i = 0; t[i]; i++) if (t[i] != '\r') t[k++] = t[i];
    t[k] = 0;
    e = esc_utf8(t);
    free(t);
    return e;
}

static BOOL props_apply(HWND dlg)
{
    field_t *f = &g.fields[g_pi];
    char *name = dlg_esc(dlg, IDC_FP_NAME), *tip = dlg_esc(dlg, IDC_FP_TOOLTIP), *line = NULL, fmt[160] = "", calc[1200] = "";
    char size[16], *opts = NULL, *defv = NULL, *exp = NULL;
    WCHAR t[1024];
    int fmtk = (int)SendDlgItemMessageW(dlg, IDC_FP_FORMAT, CB_GETCURSEL, 0, 0);
    int calck = (int)SendDlgItemMessageW(dlg, IDC_FP_CALC, CB_GETCURSEL, 0, 0);
    int dec = (int)SendDlgItemMessageW(dlg, IDC_FP_DECIMALS, CB_GETCURSEL, 0, 0), len;
    BOOL ok;
    if (!name || !name[0]) {
        MessageBoxW(dlg, L"A field needs a name.", L"Prepare Form", MB_OK | MB_ICONWARNING);
        free(name); free(tip);
        return FALSE;
    }
    GetDlgItemTextW(dlg, IDC_FP_FONTSIZE, t, 16);
    snprintf(size, sizeof(size), "%g", _wtof(t) > 0 ? _wtof(t) : 0.0);
    if (f->type == FLD_TEXT) {
        char cur[32], pat[64];
        GetDlgItemTextW(dlg, IDC_FP_CURRENCY, t, 32);
        to_utf8(t, cur, sizeof(cur));
        GetDlgItemTextW(dlg, IDC_FP_DATEPAT, t, 64);
        to_utf8(t, pat, sizeof(pat));
        if (dec < 0) dec = 2;
        switch (fmtk) {
        case 1: snprintf(fmt, sizeof(fmt), "number:%d", dec); break;
        case 2: snprintf(fmt, sizeof(fmt), "number:%d:%s", dec, cur[0] ? cur : "$"); break;
        case 3: snprintf(fmt, sizeof(fmt), "percent:%d", dec); break;
        case 4: snprintf(fmt, sizeof(fmt), "date:%s", pat[0] ? pat : "mm/dd/yyyy"); break;
        case 5: lstrcpyA(fmt, "zip"); break;
        case 6: lstrcpyA(fmt, "zip4"); break;
        case 7: lstrcpyA(fmt, "phone"); break;
        case 8: lstrcpyA(fmt, "ssn"); break;
        default: lstrcpyA(fmt, "none");
        }
#ifdef SG_MUTANT_FORMPROPS
        calck = 0;
#endif
        if (calck > 0 && calck < 7) {
            char *fields = dlg_esc(dlg, IDC_FP_CALCFIELDS);
            if (calck < 6 && fields) {
                /* "a, b" -> "a,b" */
                char *s = fields, *d = fields;
                for (; *s; s++) if (!(*s == ' ' && (d == fields || d[-1] == ','))) *d++ = *s;
                *d = 0;
                while (d > fields && (d[-1] == ' ' || d[-1] == ',')) *--d = 0;
            }
            snprintf(calc, sizeof(calc), "%s:%s", CALC_KEYS[calck], fields ? fields : "");
            free(fields);
        } else lstrcpyA(calc, "none");
        defv = dlg_esc(dlg, IDC_FP_DEFAULT);
    } else if (f->type == FLD_COMBO || f->type == FLD_LIST) {
        opts = dlg_esc(dlg, IDC_FP_OPTIONS);
        defv = dlg_esc(dlg, IDC_FP_DEFAULT);
    } else if (f->type == FLD_CHECK || f->type == FLD_RADIO) {
        exp = dlg_esc(dlg, IDC_FP_EXPORT);
    }
    len = _scprintf("x") + 4096 + (int)strlen(name) + (tip ? (int)strlen(tip) : 0) + (opts ? (int)strlen(opts) : 0) +
          (defv ? (int)strlen(defv) : 0) + (int)strlen(calc);
    line = malloc(len);
    if (!line) { free(name); free(tip); free(opts); free(defv); free(exp); return FALSE; }
    len = snprintf(line, len, "setfieldprops\t%d\t%d\tname=%s\ttooltip=%s\trequired=%d\treadonly=%d\tfontsize=%s\talign=%d",
                   f->page, f->xref, name, tip ? tip : "", IsDlgButtonChecked(dlg, IDC_FP_REQUIRED) == BST_CHECKED,
                   IsDlgButtonChecked(dlg, IDC_FP_READONLY) == BST_CHECKED, size,
                   (int)max(0, SendDlgItemMessageW(dlg, IDC_FP_ALIGN, CB_GETCURSEL, 0, 0)));
    if (f->type == FLD_TEXT)
        len += sprintf(line + len, "\tmultiline=%d\tformat=%s\tcalc=%s\tdefault=%s",
                       IsDlgButtonChecked(dlg, IDC_FP_MULTI) == BST_CHECKED, fmt, calc, defv ? defv : "");
    if (opts) len += sprintf(line + len, "\toptions=%s\tdefault=%s", opts, defv ? defv : "");
    if (exp && exp[0]) len += sprintf(line + len, "\texport=%s", exp);
    {
        int xref = f->xref;
        ok = doc_request(line);
        if (ok) form_pick(index_of_xref(xref));
    }
    free(line); free(name); free(tip); free(opts); free(defv); free(exp);
    return ok;
}

static INT_PTR CALLBACK props_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg) {
    case WM_INITDIALOG: props_init(dlg); return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_FP_FORMAT: case IDC_FP_CALC:
            if (HIWORD(wp) == CBN_SELCHANGE) props_enable(dlg);
            break;
        case IDC_FP_CALCPICK: {
            WCHAR buf[1024];
            GetDlgItemTextW(dlg, IDC_FP_CALCFIELDS, buf, 1024);
            if (DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_PICKFIELDS), dlg, pick_proc, (LPARAM)buf) == IDOK)
                SetDlgItemTextW(dlg, IDC_FP_CALCFIELDS, buf);
            break;
        }
        case IDOK: if (props_apply(dlg)) EndDialog(dlg, IDOK); return TRUE;
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

BOOL form_props(int index)
{
    if (index < 0 || index >= g.nfields) return FALSE;
    g_pi = index;
    return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_FIELDPROPS), g_main, props_proc, 0) == IDOK;
}
