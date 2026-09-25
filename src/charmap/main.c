/* sg-charmap -- Character Map, as Windows has it: a font's characters in a
 * grid, a magnified view of the one under the mouse or the keyboard, a
 * "Characters to copy" box with Select and Copy, and the advanced view
 * (character set, search by name or U+ code, go to a code point). The status
 * bar names the character ("U+00E9: Latin Small Letter E With Acute") from a
 * table generated at build time from the Unicode Character Database
 * (gen-names.py).
 *
 * charmap.exe resolves here through App Paths (defaults/77-sg-charmap.reg).
 * SG_CHARMAP_DUMP=<file> writes the state after every change, for the gate.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../sg-mode.h"
/* Stained Glass: the app mode (Settings > Colors, AppsUseLightTheme) picks
 * the palette; WM_SETTINGCHANGE "ImmersiveColorSet" switches it live */
BOOL sgm_dark;
void sgm_follow(HWND hwnd)
{
    BOOL dark = sg_apps_dark();
    if (dark == sgm_dark) return;
    sgm_dark = dark;
    sg_mode_title(hwnd, dark);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

/* the generated name table (build/charmap-names.c) */
extern const char cm_words[];
extern const unsigned int cm_word_off[];
extern const int cm_count, cm_nranges;
extern const unsigned short cm_cp[];
extern const unsigned short cm_range[][3];
extern const unsigned int cm_tok_start[];
extern const unsigned short cm_toks[];

#define COLS 20
#define ROWS 10

enum { ID_FONT = 100, ID_GRID, ID_EDIT, ID_SELECT, ID_COPY, ID_ADV, ID_CHARSET, ID_GROUP,
       ID_SEARCH_EDIT, ID_SEARCH, ID_GOTO, ID_STATUS, ID_L_FONT, ID_L_COPY, ID_L_CHARSET,
       ID_L_GROUP, ID_L_SEARCH, ID_L_GOTO };

static HINSTANCE g_inst;
static HWND g_hwnd, g_font_combo, g_grid, g_zoom, g_edit, g_select, g_copy, g_adv, g_status;
static HWND g_adv_ctl[12];
static int g_nadv;
static HFONT g_ui_font, g_cell_font, g_zoom_font, g_edit_font;
static int g_dpi = 96, g_cell;
static COLORREF g_accent = RGB(112, 48, 192);
static WCHAR g_face[LF_FACESIZE];
static WCHAR *g_all, *g_shown;
static int g_nall, g_nshown, g_sel, g_top;
static BOOL g_filtered, g_advanced, g_zoom_from_mouse;
static const WCHAR *g_dump_path;

#define S(x) MulDiv((x), g_dpi, 96)
static const WCHAR KEY[] = L"Software\\Microsoft\\CharMap";

/* ---- names ------------------------------------------------------------ */

static const char *const JAMO_L[] = { "G", "GG", "N", "D", "DD", "R", "M", "B", "BB", "S", "SS", "", "J",
                                      "JJ", "C", "K", "T", "P", "H" };
static const char *const JAMO_V[] = { "A", "AE", "YA", "YAE", "EO", "E", "YEO", "YE", "O", "WA", "WAE",
                                      "OE", "YO", "U", "WEO", "WE", "WI", "YU", "EU", "YI", "I" };
static const char *const JAMO_T[] = { "", "G", "GG", "GS", "N", "NJ", "NH", "D", "L", "LG", "LM", "LB",
                                      "LS", "LT", "LP", "LH", "M", "B", "BS", "S", "SS", "NG", "J", "C",
                                      "K", "T", "P", "H" };

/* The character's name, or an empty string when it has none. */
static void char_name(unsigned cp, char *out, size_t cch)
{
    int lo = 0, hi = cm_count - 1, i;
    out[0] = 0;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (cm_cp[mid] == cp)
        {
            size_t len = 0;
            unsigned t;
            for (t = cm_tok_start[mid]; t < cm_tok_start[mid + 1]; t++)
            {
                const char *w = cm_words + cm_word_off[cm_toks[t]];
                size_t wl = strlen(w);
                if (len + wl + 2 > cch) break;
                if (len) out[len++] = ' ';
                memcpy(out + len, w, wl);
                len += wl;
            }
            out[len] = 0;
            return;
        }
        if (cm_cp[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    for (i = 0; i < cm_nranges; i++)
    {
        if (cp < cm_range[i][0] || cp > cm_range[i][1]) continue;
        if (cm_range[i][2] == 0) snprintf(out, cch, "CJK Unified Ideograph-%04X", cp);
        else if (cm_range[i][2] == 2) snprintf(out, cch, "Private Use");
        else
        {
            unsigned s = cp - 0xAC00;
            char syl[16];
            size_t k;
            snprintf(syl, sizeof(syl), "%s%s%s", JAMO_L[s / (21 * 28)], JAMO_V[(s % (21 * 28)) / 28], JAMO_T[s % 28]);
            for (k = 1; syl[k]; k++) syl[k] = (char)(syl[k] - 'A' + 'a');
            snprintf(out, cch, "Hangul Syllable %s", syl);
        }
        return;
    }
}

static void char_name_w(unsigned cp, WCHAR *out, int cch)
{
    char a[160];
    char_name(cp, a, sizeof(a));
    MultiByteToWideChar(CP_ACP, 0, a, -1, out, cch);
}

/* ---- the state, for the gate ---------------------------------------- */

static void put_utf8(FILE *f, const WCHAR *s)
{
    char buf[1024];
    WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf), NULL, NULL);
    fputs(buf, f);
}

static void cell_rect(int index, RECT *r);

static void dump(void)
{
    FILE *f;
    WCHAR text[256], name[160];
    RECT gr, cr;
    POINT pt = { 0, 0 };
    int i;
    if (!g_dump_path || !(f = _wfopen(g_dump_path, L"wb"))) return;
    fputs("font=", f); put_utf8(f, g_face); fputc('\n', f);
    fprintf(f, "count=%d/%d\nfiltered=%d\nadvanced=%d\n", g_nshown, g_nall, g_filtered, g_advanced);
    if (g_nshown)
    {
        char_name_w(g_shown[g_sel], name, 160);
        fprintf(f, "sel=U+%04X\nname=", g_shown[g_sel]);
        put_utf8(f, name);
        fputc('\n', f);
    }
    else fputs("sel=none\n", f);
    GetWindowTextW(g_edit, text, 256);
    fputs("text=", f); put_utf8(f, text); fputs("\ntextcp=", f);
    for (i = 0; text[i]; i++) fprintf(f, "%sU+%04X", i ? " " : "", text[i]);
    fputc('\n', f);
    ClientToScreen(g_grid, &pt);
    GetClientRect(g_grid, &gr);
    fprintf(f, "grid=%ld,%ld,%ld,%ld\ncell=%d\ntop=%d\ncols=%d\n", pt.x, pt.y, pt.x + gr.right, pt.y + gr.bottom,
            g_cell, g_top, COLS);
    if (g_nshown)
    {
        cell_rect(g_sel, &cr);
        fprintf(f, "selcell=%ld,%ld\n", pt.x + (cr.left + cr.right) / 2, pt.y + (cr.top + cr.bottom) / 2);
    }
    for (i = 0; i < 2; i++)
    {
        WCHAR st[256] = L"";
        if (LOWORD(SendMessageW(g_status, SB_GETTEXTLENGTHW, i, 0)) < 256) SendMessageW(g_status, SB_GETTEXTW, i, (LPARAM)st);
        fprintf(f, "status%d=", i); put_utf8(f, st); fputc('\n', f);
    }
    GetDlgItemTextW(g_hwnd, ID_SEARCH_EDIT, text, 256);
    fputs("search=", f); put_utf8(f, text); fputc('\n', f);
    fprintf(f, "zoom=%d\ncopy_enabled=%d\n", IsWindowVisible(g_zoom) ? 1 : 0, IsWindowEnabled(g_copy) ? 1 : 0);
    {
        HWND focus = GetFocus();
        WCHAR cls[64] = L"";
        if (focus) GetClassNameW(focus, cls, 64);
        fputs("focus=", f); put_utf8(f, focus == g_grid ? L"grid" : focus == g_edit ? L"edit" : cls); fputc('\n', f);
    }
    fputs("end\n", f);
    fclose(f);
}

/* ---- the font's characters -------------------------------------------- */

static HFONT make_font(const WCHAR *face, int height, int weight)
{
    return CreateFontW(-height, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, face);
}

static BOOL in_list(const WCHAR *list, int n, WCHAR c, int *at)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi)
    {
        int mid = (lo + hi) / 2;
        if (list[mid] == c) { if (at) *at = mid; return TRUE; }
        if (list[mid] < c) lo = mid + 1; else hi = mid - 1;
    }
    return FALSE;
}

static int total_rows(void) { return (g_nshown + COLS - 1) / COLS; }

static void update_scroll(void)
{
    SCROLLINFO si = { sizeof(si), SIF_ALL | SIF_DISABLENOSCROLL };
    int rows = total_rows();
    if (g_top > rows - ROWS) g_top = rows - ROWS;
    if (g_top < 0) g_top = 0;
    si.nMin = 0;
    si.nMax = rows ? rows - 1 : 0;
    si.nPage = ROWS;
    si.nPos = g_top;
    SetScrollInfo(g_grid, SB_VERT, &si, TRUE);
}

static void update_status(void)
{
    WCHAR name[160], text[220], keys[64] = L"";
    unsigned char b;
    WCHAR c;
    if (!g_nshown) { SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)L""); SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)L""); return; }
    c = g_shown[g_sel];
    char_name_w(c, name, 160);
    if (name[0]) swprintf(text, 220, L"U+%04X: %ls", c, name);
    else swprintf(text, 220, L"U+%04X", c);
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)text);
    /* Windows' Alt+0nnn for the characters of the ANSI code page's upper half */
    if (c == ' ') lstrcpyW(keys, L"Keystroke: Spacebar");
    else if (c >= 0x80)
    {
        BOOL lost = FALSE;
        if (WideCharToMultiByte(1252, WC_NO_BEST_FIT_CHARS, &c, 1, (char *)&b, 1, NULL, &lost) == 1 && !lost && b >= 0x80)
            swprintf(keys, 64, L"Keystroke: Alt+0%u", b);
    }
    SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)keys);
}

static void hide_zoom(void) { if (IsWindowVisible(g_zoom)) ShowWindow(g_zoom, SW_HIDE); }

static void show_zoom(void);

static void ensure_visible(void)
{
    int row = g_sel / COLS;
    if (row < g_top) g_top = row;
    if (row >= g_top + ROWS) g_top = row - ROWS + 1;
    update_scroll();
}

static void select_index(int i, BOOL zoom)
{
    if (!g_nshown) return;
    if (i < 0) i = 0;
    if (i >= g_nshown) i = g_nshown - 1;
    g_sel = i;
    ensure_visible();
    InvalidateRect(g_grid, NULL, FALSE);
    update_status();
    if (zoom) show_zoom(); else hide_zoom();
    dump();
}

/* Characters matching the search text (by name), or everything. */
static BOOL apply_filter(const WCHAR *query)
{
    char q[128], name[160];
    int i, n = 0;
    WCHAR keep = g_nshown ? g_shown[g_sel] : 0;
    if (!query || !query[0])
    {
        memcpy(g_shown, g_all, g_nall * sizeof(WCHAR));
        g_nshown = g_nall;
        g_filtered = FALSE;
    }
    else
    {
        WideCharToMultiByte(CP_ACP, 0, query, -1, q, sizeof(q), NULL, NULL);
        CharUpperA(q);
        for (i = 0; i < g_nall; i++)
        {
            char_name(g_all[i], name, sizeof(name));
            CharUpperA(name);
            if (strstr(name, q)) g_shown[n++] = g_all[i];
        }
        if (!n) return FALSE;
        g_nshown = n;
        g_filtered = TRUE;
    }
    g_sel = 0;
    if (keep) in_list(g_shown, g_nshown, keep, &g_sel);
    g_top = 0;
    SetDlgItemTextW(g_hwnd, ID_SEARCH, g_filtered ? L"&Reset" : L"S&earch");
    ensure_visible();
    InvalidateRect(g_grid, NULL, TRUE);
    update_status();
    return TRUE;
}

static void load_font(const WCHAR *face)
{
    HDC dc = CreateCompatibleDC(NULL);
    HFONT probe = make_font(face, 32, FW_NORMAL), old;
    DWORD size;
    GLYPHSET *gs;
    WCHAR keep = g_nshown ? g_shown[g_sel] : 'A';
    LOGFONTW lf;
    DWORD i, j;

    lstrcpynW(g_face, face, LF_FACESIZE);
    old = SelectObject(dc, probe);
    free(g_all); free(g_shown);
    g_all = g_shown = NULL; g_nall = g_nshown = 0;
    if ((size = GetFontUnicodeRanges(dc, NULL)) && (gs = malloc(size)))
    {
        GetFontUnicodeRanges(dc, gs);
        g_all = malloc((gs->cGlyphsSupported + 1) * sizeof(WCHAR));
        g_shown = malloc((gs->cGlyphsSupported + 1) * sizeof(WCHAR));
        for (i = 0; g_all && g_shown && i < gs->cRanges; i++)
            for (j = 0; j < gs->ranges[i].cGlyphs; j++)
            {
                unsigned c = gs->ranges[i].wcLow + j;
                if (c < 0x20 || (c >= 0x7F && c < 0xA0) || (c >= 0xD800 && c < 0xE000) || c > 0xFFFD) continue;
                if (g_nall && g_all[g_nall - 1] >= c) continue;   /* sorted, no repeats */
                g_all[g_nall++] = (WCHAR)c;
            }
        free(gs);
    }
    SelectObject(dc, old);
    DeleteObject(probe);
    DeleteDC(dc);
    if (!g_all) { g_all = malloc(sizeof(WCHAR)); g_shown = malloc(sizeof(WCHAR)); }

    if (g_cell_font) DeleteObject(g_cell_font);
    if (g_zoom_font) DeleteObject(g_zoom_font);
    if (g_edit_font) DeleteObject(g_edit_font);
    g_cell_font = make_font(face, g_cell * 64 / 100, FW_NORMAL);
    g_zoom_font = make_font(face, g_cell * 2 * 72 / 100, FW_NORMAL);
    /* the box of characters to copy shows them in the chosen font, as Windows does */
    GetObjectW(g_ui_font, sizeof(lf), &lf);
    g_edit_font = make_font(face, abs(lf.lfHeight) * 5 / 4, FW_NORMAL);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_edit_font, TRUE);

    g_nshown = 0;
    g_filtered = FALSE;
    memcpy(g_shown, g_all, g_nall * sizeof(WCHAR));
    g_nshown = g_nall;
    SetDlgItemTextW(g_hwnd, ID_SEARCH, L"S&earch");
    SetDlgItemTextW(g_hwnd, ID_SEARCH_EDIT, L"");
    g_sel = 0;
    if (!in_list(g_shown, g_nshown, keep, &g_sel)) g_sel = 0;
    g_top = 0;
    ensure_visible();
    hide_zoom();
    InvalidateRect(g_grid, NULL, TRUE);
    update_status();
    {
        HKEY k;
        if (!RegCreateKeyExW(HKEY_CURRENT_USER, KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL))
        {
            RegSetValueExW(k, L"Font", 0, REG_SZ, (const BYTE *)g_face, (lstrlenW(g_face) + 1) * sizeof(WCHAR));
            RegCloseKey(k);
        }
    }
    dump();
}

/* ---- the grid --------------------------------------------------------- */

static void cell_rect(int index, RECT *r)
{
    int row = index / COLS - g_top, col = index % COLS;
    r->left = col * g_cell;
    r->top = row * g_cell;
    r->right = r->left + g_cell + 1;
    r->bottom = r->top + g_cell + 1;
}

static int cell_at(int x, int y)
{
    int col = x / g_cell, row = y / g_cell + g_top, i;
    if (x < 0 || y < 0 || col >= COLS) return -1;
    i = row * COLS + col;
    return i < g_nshown ? i : -1;
}

static void paint_grid(HWND hwnd, HDC dc)
{
    RECT cr, r;
    /* the cells follow the app mode: white, or dark (sg-mode.h) */
    HBRUSH white = CreateSolidBrush(sgm_dark ? RGB(32, 32, 32) : RGB(255, 255, 255)), sel = CreateSolidBrush(g_accent);
    HPEN line = CreatePen(PS_SOLID, 1, sgm_dark ? RGB(80, 80, 80) : RGB(160, 160, 160)), oldpen;
    HGDIOBJ oldbrush;
    HFONT oldfont;
    int row, col;

    GetClientRect(hwnd, &cr);
    FillRect(dc, &cr, white);
    oldbrush = SelectObject(dc, white);
    oldpen = SelectObject(dc, line);
    oldfont = SelectObject(dc, g_cell_font);
    SetBkMode(dc, TRANSPARENT);
    for (row = 0; row < ROWS; row++)
        for (col = 0; col < COLS; col++)
        {
            int i = (g_top + row) * COLS + col;
            cell_rect(i, &r);
            Rectangle(dc, r.left, r.top, r.right, r.bottom);
            if (i >= g_nshown) continue;
            if (i == g_sel)
            {
                RECT in = { r.left + 1, r.top + 1, r.right - 1, r.bottom - 1 };
                FillRect(dc, &in, sel);
                SetTextColor(dc, RGB(255, 255, 255));
                if (GetFocus() == hwnd)
                {
                    InflateRect(&in, -1, -1);
                    DrawFocusRect(dc, &in);
                }
            }
            else SetTextColor(dc, sgm_dark ? RGB(255, 255, 255) : RGB(0, 0, 0));
            DrawTextW(dc, &g_shown[i], 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    SelectObject(dc, oldfont);
    SelectObject(dc, oldpen);
    SelectObject(dc, oldbrush);
    DeleteObject(line);
    DeleteObject(sel);
    DeleteObject(white);
}

static void show_zoom(void)
{
    RECT r, gr;
    POINT pt = { 0, 0 };
    int z = g_cell * 2 + S(6), x, y;
    if (!g_nshown) return;
    cell_rect(g_sel, &r);
    ClientToScreen(g_grid, &pt);
    GetClientRect(g_grid, &gr);
    x = pt.x + (r.left + r.right) / 2 - z / 2;
    y = pt.y + (r.top + r.bottom) / 2 - z / 2;
    if (x < pt.x) x = pt.x;
    if (y < pt.y) y = pt.y;
    if (x + z > pt.x + gr.right) x = pt.x + gr.right - z;
    if (y + z > pt.y + gr.bottom) y = pt.y + gr.bottom - z;
    SetWindowPos(g_zoom, HWND_TOP, x, y, z, z, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_zoom, NULL, TRUE);
    UpdateWindow(g_zoom);
}

static LRESULT CALLBACK zoom_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        HPEN pen = CreatePen(PS_SOLID, 1, sgm_dark ? RGB(160, 160, 160) : RGB(64, 64, 64));
        HBRUSH bg = CreateSolidBrush(sgm_dark ? RGB(32, 32, 32) : RGB(255, 255, 255));
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, bg), of;
        GetClientRect(hwnd, &r);
        Rectangle(dc, r.left, r.top, r.right, r.bottom);
        if (g_nshown)
        {
            of = SelectObject(dc, g_zoom_font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, sgm_dark ? RGB(255, 255, 255) : RGB(0, 0, 0));
            DrawTextW(dc, &g_shown[g_sel], 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, of);
        }
        SelectObject(dc, ob);
        SelectObject(dc, op);
        DeleteObject(pen);
        DeleteObject(bg);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void append_selected(void)
{
    WCHAR c[2];
    if (!g_nshown) return;
    c[0] = g_shown[g_sel];
    c[1] = 0;
#ifdef SG_MUTANT_SELECT
    c[0] = (WCHAR)(c[0] + 1);
#endif
    SendMessageW(g_edit, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);
    SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)c);
}

static void scroll_to(int top)
{
    g_top = top;
    update_scroll();
    hide_zoom();
    InvalidateRect(g_grid, NULL, FALSE);
    dump();
}

static LRESULT CALLBACK grid_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_GETDLGCODE:
    {
        const MSG *m = (const MSG *)lp;
        if (m && m->message == WM_KEYDOWN && (m->wParam == VK_RETURN || m->wParam == VK_ESCAPE)) return DLGC_WANTMESSAGE;
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps), mem;
        RECT r;
        HBITMAP bmp, old;
        GetClientRect(hwnd, &r);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
        old = SelectObject(mem, bmp);
        paint_grid(hwnd, mem);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        dump();
        return 0;
    }
    case WM_SETFOCUS: case WM_KILLFOCUS:
        if (msg == WM_KILLFOCUS) hide_zoom();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    {
        int i = cell_at((short)LOWORD(lp), (short)HIWORD(lp));
        SetFocus(hwnd);
        if (i >= 0) { SetCapture(hwnd); g_zoom_from_mouse = TRUE; select_index(i, TRUE); }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd)
        {
            int i = cell_at((short)LOWORD(lp), (short)HIWORD(lp));
            if (i >= 0 && i != g_sel) select_index(i, TRUE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) { ReleaseCapture(); hide_zoom(); dump(); }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (cell_at((short)LOWORD(lp), (short)HIWORD(lp)) >= 0) append_selected();
        return 0;
    case WM_MOUSEWHEEL:
        scroll_to(g_top - GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 3);
        return 0;
    case WM_VSCROLL:
    {
        SCROLLINFO si = { sizeof(si), SIF_TRACKPOS };
        int top = g_top;
        GetScrollInfo(hwnd, SB_VERT, &si);
        switch (LOWORD(wp))
        {
        case SB_LINEUP: top--; break;
        case SB_LINEDOWN: top++; break;
        case SB_PAGEUP: top -= ROWS; break;
        case SB_PAGEDOWN: top += ROWS; break;
        case SB_TOP: top = 0; break;
        case SB_BOTTOM: top = total_rows(); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: top = si.nTrackPos; break;
        }
        scroll_to(top);
        return 0;
    }
    case WM_KEYDOWN:
    {
        int i = g_sel;
        BOOL ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (wp)
        {
        case VK_LEFT: i--; break;
        case VK_RIGHT: i++; break;
        case VK_UP: i -= COLS; break;
        case VK_DOWN: i += COLS; break;
        case VK_PRIOR: i -= COLS * ROWS; break;
        case VK_NEXT: i += COLS * ROWS; break;
        case VK_HOME: i = ctrl ? 0 : i - i % COLS; break;
        case VK_END: i = ctrl ? g_nshown - 1 : i - i % COLS + COLS - 1; break;
        case VK_RETURN: append_selected(); hide_zoom(); return 0;
        case VK_ESCAPE: hide_zoom(); dump(); return 0;
        default: return 0;
        }
        if (i < 0) i = (wp == VK_UP || wp == VK_PRIOR) ? g_sel % COLS : 0;
        if (i >= g_nshown) i = g_nshown - 1;
        g_zoom_from_mouse = FALSE;
        select_index(i, TRUE);
        return 0;
    }
    case WM_CHAR:
    {
        int at;
        if (wp == ' ') { append_selected(); return 0; }
        if (wp > ' ' && in_list(g_shown, g_nshown, (WCHAR)wp, &at)) select_index(at, TRUE);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the window ------------------------------------------------------- */

static int CALLBACK enum_font(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM lp)
{
    (void)tm; (void)type; (void)lp;
    if (lf->lfFaceName[0] == '@') return 1;
    if (SendMessageW(g_font_combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)lf->lfFaceName) == CB_ERR)
        SendMessageW(g_font_combo, CB_ADDSTRING, 0, (LPARAM)lf->lfFaceName);
    return 1;
}

static void fill_fonts(void)
{
    LOGFONTW lf = { 0 };
    HDC dc = GetDC(NULL);
    WCHAR want[LF_FACESIZE] = L"";
    DWORD cb = sizeof(want);
    const WCHAR *tries[] = { want, L"Arial", L"Segoe UI", L"Tahoma" };
    int i, at = CB_ERR;
    lf.lfCharSet = DEFAULT_CHARSET;
    EnumFontFamiliesExW(dc, &lf, (FONTENUMPROCW)enum_font, 0, 0);
    ReleaseDC(NULL, dc);
    RegGetValueW(HKEY_CURRENT_USER, KEY, L"Font", RRF_RT_REG_SZ, NULL, want, &cb);
    for (i = 0; i < 4 && at == CB_ERR; i++)
        if (tries[i][0]) at = (int)SendMessageW(g_font_combo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)tries[i]);
    if (at == CB_ERR) at = 0;
    SendMessageW(g_font_combo, CB_SETCURSEL, at, 0);
}

static void current_font(WCHAR *face)
{
    int i = (int)SendMessageW(g_font_combo, CB_GETCURSEL, 0, 0);
    face[0] = 0;
    if (i != CB_ERR && SendMessageW(g_font_combo, CB_GETLBTEXTLEN, i, 0) < LF_FACESIZE)
        SendMessageW(g_font_combo, CB_GETLBTEXT, i, (LPARAM)face);
}

static HWND add(const WCHAR *cls, const WCHAR *text, DWORD style, int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(!lstrcmpW(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             S(x), S(y), S(w), S(h), g_hwnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_ui_font, FALSE);
    return c;
}

static int g_base_h, g_adv_h;

static void set_advanced(BOOL on)
{
    RECT wr, cr;
    int i, sb;
    HKEY k;
    g_advanced = on;
    for (i = 0; i < g_nadv; i++) ShowWindow(g_adv_ctl[i], on ? SW_SHOW : SW_HIDE);
    SendMessageW(g_adv, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    GetWindowRect(g_status, &cr);
    sb = cr.bottom - cr.top;
    wr.left = 0; wr.top = 0; wr.right = 10; wr.bottom = S(on ? g_adv_h : g_base_h) + sb;
    AdjustWindowRectEx(&wr, GetWindowLongW(g_hwnd, GWL_STYLE), FALSE, GetWindowLongW(g_hwnd, GWL_EXSTYLE));
    GetWindowRect(g_hwnd, &cr);
    SetWindowPos(g_hwnd, NULL, 0, 0, cr.right - cr.left, wr.bottom - wr.top, SWP_NOMOVE | SWP_NOZORDER);
    /* the status bar moved: where it was is under the advanced controls now */
    RedrawWindow(g_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL))
    {
        DWORD v = on;
        RegSetValueExW(k, L"Advanced", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
    dump();
}

/* "U+00E9", "u+e9", "0x00E9": a code point; -1 otherwise */
static int parse_code(const WCHAR *s, BOOL bare)
{
    WCHAR *end;
    unsigned long v;
    while (*s == ' ') s++;
#ifdef SG_MUTANT_SEARCH
    if (!bare) return -1;
#endif
    if ((s[0] == 'U' || s[0] == 'u') && s[1] == '+') s += 2;
    else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    else if (!bare) return -1;
    if (!iswxdigit(*s)) return -1;
    v = wcstoul(s, &end, 16);
    while (*end == ' ') end++;
    if (*end || v > 0xFFFF) return -1;
    return (int)v;
}

static BOOL goto_code(int cp)
{
    int at = 0;
    if (cp < 0) return FALSE;
    if (!in_list(g_shown, g_nshown, (WCHAR)cp, &at))
    {
        if (!in_list(g_all, g_nall, (WCHAR)cp, NULL)) return FALSE;
        apply_filter(NULL);
        in_list(g_shown, g_nshown, (WCHAR)cp, &at);
    }
    select_index(at, FALSE);
    return TRUE;
}

static void do_search(void)
{
    WCHAR q[128];
    if (g_filtered)
    {
        apply_filter(NULL);
        SetDlgItemTextW(g_hwnd, ID_SEARCH_EDIT, L"");
        dump();
        return;
    }
    GetDlgItemTextW(g_hwnd, ID_SEARCH_EDIT, q, 128);
    if (!q[0]) return;
    if (goto_code(parse_code(q, FALSE))) return;
    if (!apply_filter(q))
    {
        SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)L"No characters match the search.");
        MessageBeep(MB_ICONASTERISK);
    }
    dump();
}

static void copy_text(void)
{
    WCHAR *text;
    DWORD start = 0, end = 0;
    int len = GetWindowTextLengthW(g_edit);
    HGLOBAL mem;
    if (!len) return;
    text = malloc((len + 1) * sizeof(WCHAR));
    GetWindowTextW(g_edit, text, len + 1);
    SendMessageW(g_edit, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    if (end > start && end <= (DWORD)len) { memmove(text, text + start, (end - start) * sizeof(WCHAR)); text[end - start] = 0; }
    else { start = 0; end = len; }
    if (OpenClipboard(g_hwnd))
    {
        EmptyClipboard();
        if ((mem = GlobalAlloc(GMEM_MOVEABLE, (end - start + 1) * sizeof(WCHAR))))
        {
            memcpy(GlobalLock(mem), text, (end - start + 1) * sizeof(WCHAR));
            GlobalUnlock(mem);
            if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
        }
        CloseClipboard();
    }
    free(text);
    /* Windows selects the characters after copying them */
    SendMessageW(g_edit, EM_SETSEL, start, end);
}

static void create_controls(void)
{
    int gw = COLS * g_cell + 1, gh = ROWS * g_cell + 1, sbw = GetSystemMetrics(SM_CXVSCROLL);
    int gwd = MulDiv(gw + sbw + 2, 96, g_dpi);        /* the grid's width in 96-dpi units */
    int y, x0 = 10, n = 0;
    int parts[2];
    RECT gr;

    add(L"STATIC", L"&Font :", SS_LEFT | SS_CENTERIMAGE, x0, 10, 44, 23, ID_L_FONT);
    g_font_combo = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_SORT | WS_VSCROLL | WS_TABSTOP, x0 + 48, 10, gwd - 48, 300, ID_FONT);
    g_grid = CreateWindowExW(WS_EX_CLIENTEDGE, L"SgCharGrid", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL,
                             S(x0), S(42), gw + sbw + 4, gh + 4, g_hwnd, (HMENU)ID_GRID, g_inst, NULL);
    GetWindowRect(g_grid, &gr);
    y = MulDiv(S(42) + (gr.bottom - gr.top), 96, g_dpi) + 10;
    add(L"STATIC", L"Characters to cop&y :", SS_LEFT | SS_CENTERIMAGE, x0, y, 118, 25, ID_L_COPY);
    g_edit = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, x0 + 120, y, gwd - 120 - 172, 25, ID_EDIT);
    g_select = add(L"BUTTON", L"&Select", BS_PUSHBUTTON | WS_TABSTOP, x0 + gwd - 164, y, 78, 25, ID_SELECT);
    g_copy = add(L"BUTTON", L"&Copy", BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, x0 + gwd - 78, y, 78, 25, ID_COPY);
    y += 34;
    g_adv = add(L"BUTTON", L"Ad&vanced view", BS_AUTOCHECKBOX | WS_TABSTOP, x0, y, 140, 22, ID_ADV);
    g_base_h = y + 30;
    y += 32;
    g_adv_ctl[n++] = add(L"STATIC", L"Charac&ter set :", SS_LEFT | SS_CENTERIMAGE, x0, y, 100, 23, ID_L_CHARSET);
    g_adv_ctl[n] = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, x0 + 104, y, 200, 200, ID_CHARSET);
    SendMessageW(g_adv_ctl[n], CB_ADDSTRING, 0, (LPARAM)L"Unicode");
    SendMessageW(g_adv_ctl[n++], CB_SETCURSEL, 0, 0);
    y += 30;
    g_adv_ctl[n++] = add(L"STATIC", L"Group &by :", SS_LEFT | SS_CENTERIMAGE, x0, y, 100, 23, ID_L_GROUP);
    g_adv_ctl[n] = add(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, x0 + 104, y, 200, 200, ID_GROUP);
    SendMessageW(g_adv_ctl[n], CB_ADDSTRING, 0, (LPARAM)L"All");
    SendMessageW(g_adv_ctl[n++], CB_SETCURSEL, 0, 0);
    y += 30;
    g_adv_ctl[n++] = add(L"STATIC", L"Searc&h for :", SS_LEFT | SS_CENTERIMAGE, x0, y, 100, 25, ID_L_SEARCH);
    g_adv_ctl[n++] = add(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, x0 + 104, y, 200, 25, ID_SEARCH_EDIT);
    g_adv_ctl[n++] = add(L"BUTTON", L"S&earch", BS_PUSHBUTTON | WS_TABSTOP, x0 + 312, y, 78, 25, ID_SEARCH);
    y += 32;
    g_adv_ctl[n++] = add(L"STATIC", L"&Go to Unicode :", SS_LEFT | SS_CENTERIMAGE, x0, y, 100, 25, ID_L_GOTO);
    g_adv_ctl[n++] = add(L"EDIT", L"", ES_AUTOHSCROLL | ES_UPPERCASE | WS_TABSTOP, x0 + 104, y, 80, 25, ID_GOTO);
    SendMessageW(g_adv_ctl[n - 1], EM_LIMITTEXT, 6, 0);
    g_nadv = n;
    g_adv_h = y + 34;

    g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g_hwnd,
                               (HMENU)ID_STATUS, g_inst, NULL);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_ui_font, FALSE);
    parts[0] = S(gwd + 20) - S(150);
    parts[1] = -1;
    SendMessageW(g_status, SB_SETPARTS, 2, (LPARAM)parts);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (sg_mode_changed(msg, lp)) sgm_follow(hwnd);
    switch (msg)
    {
    case WM_SIZE:
        SendMessageW(g_status, WM_SIZE, 0, 0);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) hide_zoom();
        return 0;
    case WM_MOVE:
        hide_zoom();
        return 0;
    case WM_NEXTDLGCTL:
    {
        /* IsDialogMessage sends this for a label's mnemonic (and DefDlgProc
         * would handle it): focus the given control, or the next tab stop */
        HWND to = LOWORD(lp) ? (HWND)wp : GetNextDlgTabItem(hwnd, GetFocus(), wp != 0);
        if (to)
        {
            SetFocus(to);
            if (SendMessageW(to, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL) SendMessageW(to, EM_SETSEL, 0, -1);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_FONT:
            if (HIWORD(wp) == CBN_SELCHANGE)
            {
                WCHAR face[LF_FACESIZE];
                current_font(face);
                if (face[0]) load_font(face);
            }
            return 0;
        case ID_SELECT: append_selected(); return 0;
        case ID_COPY: copy_text(); return 0;
        case ID_EDIT:
            if (HIWORD(wp) == EN_CHANGE) { EnableWindow(g_copy, GetWindowTextLengthW(g_edit) > 0); dump(); }
            return 0;
        case ID_ADV: set_advanced(SendMessageW(g_adv, BM_GETCHECK, 0, 0) == BST_CHECKED); return 0;
        case ID_SEARCH: do_search(); return 0;
        case ID_GOTO:
            if (HIWORD(wp) == EN_CHANGE)
            {
                WCHAR t[16];
                GetDlgItemTextW(hwnd, ID_GOTO, t, 16);
                goto_code(parse_code(t, TRUE));
            }
            return 0;
        case IDOK:
        {
            HWND f = GetFocus();
            if (f == GetDlgItem(hwnd, ID_SEARCH_EDIT)) do_search();
            else if (f == g_edit) copy_text();
            return 0;
        }
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void read_accent(void)
{
    DWORD v, cb = sizeof(v);
    if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                      NULL, &v, &cb))
        g_accent = v & 0xFFFFFF;          /* 0xAABBGGRR: the low three bytes are a COLORREF */
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, WCHAR *cmd, int show)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    WCHAR face[LF_FACESIZE], dump_buf[MAX_PATH];
    DWORD adv = 0, cb = sizeof(adv);
    HDC dc;
    RECT r;
    MSG m;
    (void)prev; (void)cmd;

    g_inst = inst;
    InitCommonControlsEx(&icc);
    if (GetEnvironmentVariableW(L"SG_CHARMAP_DUMP", dump_buf, MAX_PATH)) g_dump_path = dump_buf;
    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    g_cell = S(24);
    read_accent();
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_ui_font = CreateFontIndirectW(&ncm.lfMessageFont);

    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpfnWndProc = main_proc;
    wc.lpszClassName = L"SgCharMap";
    RegisterClassExW(&wc);
    wc.hIcon = NULL;
    wc.hbrBackground = NULL;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = grid_proc;
    wc.lpszClassName = L"SgCharGrid";
    RegisterClassExW(&wc);
    wc.style = CS_SAVEBITS;
    wc.lpfnWndProc = zoom_proc;
    wc.lpszClassName = L"SgCharZoom";
    RegisterClassExW(&wc);

    r.left = r.top = 0;
    r.right = COLS * g_cell + 1 + GetSystemMetrics(SM_CXVSCROLL) + 4 + S(20);
    r.bottom = S(400);
    AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, WS_EX_CONTROLPARENT);
    sgm_dark = sg_apps_dark();
    g_hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"SgCharMap", L"Character Map",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    if (g_hwnd) sg_mode_title(g_hwnd, sgm_dark);
    g_zoom = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"SgCharZoom", L"", WS_POPUP, 0, 0, 10, 10,
                             g_hwnd, NULL, inst, NULL);
    create_controls();
    fill_fonts();
    current_font(face);
    load_font(face[0] ? face : L"Tahoma");
    RegGetValueW(HKEY_CURRENT_USER, KEY, L"Advanced", RRF_RT_REG_DWORD, NULL, &adv, &cb);
    set_advanced(adv != 0);
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    SetFocus(g_grid);

    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        if (IsDialogMessageW(g_hwnd, &m)) { if (m.message == WM_KEYDOWN || m.message == WM_CHAR) dump(); continue; }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}
