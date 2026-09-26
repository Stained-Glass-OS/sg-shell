/* sg-wordpad -- WordPad for Stained Glass OS (wordpad.exe, write.exe).
 *
 * The window: a ribbon (File, Home, View; ribbon.c), a ruler (ruler.c), the
 * document -- a RichEdit 4.1 control (msftedit) -- and a status bar with the
 * zoom. Files: Rich Text (RTF, RichEdit's own reader and writer), Office Open
 * XML (.docx) and OpenDocument Text (.odt) through our own readers and
 * writers (ooxml.c, odf.c, over docmodel.c), and plain text (UTF-8, UTF-16,
 * ANSI).
 *
 * Compatibility, deliberately: window class "WordPadClass", title
 * "<name> - WordPad", one process per invocation, `/p FILE` prints to the
 * default printer and exits, `/pt FILE PRINTER ...` to a named one, a path
 * with spaces works unquoted, settings in
 * HKCU\Software\Microsoft\Windows\CurrentVersion\Applets\Wordpad\Options.
 *
 * SG_WORDPAD_DUMP=<file> writes the window's state after every change for
 * the gate (see write_dump()).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "wordpad.h"
#include "resource.h"

HINSTANCE g_inst;
HWND g_main, g_ribbon, g_ruler, g_edit, g_status, g_face, g_size;
int g_dpi = 96;
HFONT g_font, g_font_small;
int g_tab;
int g_ruler_on = 1, g_statusbar_on = 1, g_wrap = 2, g_units = 0;
int g_zoom = 100;
COLORREF g_text_color = RGB(192, 0, 0), g_hl_color = RGB(255, 255, 0);
int g_text_auto, g_hl_none;
WCHAR g_path[MAX_PATH];
int g_format = FMT_RTF;
int g_pagew = 12240, g_pageh = 15840;             /* Letter, twips */
RECT g_margins = { 1800, 1440, 1800, 1440 };      /* 1.25", 1", as WordPad */
int g_page_numbers;
WCHAR g_header[256], g_footer[256];

const COLORREF g_pal[NPAL] = {
    RGB(0, 0, 0), RGB(127, 127, 127), RGB(136, 0, 21), RGB(237, 28, 36), RGB(255, 127, 39),
    RGB(255, 242, 0), RGB(34, 177, 76), RGB(0, 162, 232), RGB(63, 72, 204), RGB(112, 48, 192),
    RGB(255, 255, 255), RGB(195, 195, 195), RGB(185, 122, 87), RGB(255, 174, 201), RGB(255, 201, 14),
    RGB(239, 228, 176), RGB(181, 230, 29), RGB(153, 217, 234), RGB(112, 146, 190), RGB(200, 191, 231),
};

static WCHAR g_dump[MAX_PATH];
static HACCEL g_accel;
static WNDPROC g_edit_proc;
static const WCHAR OPTS[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Applets\\Wordpad\\Options";
static const WCHAR *FMT_NAMES[FMT_COUNT + 1] = { L"rtf", L"docx", L"odt", L"txt", L"utxt", L"doc" };
static WCHAR g_msg[512];             /* the last message box's text, for the dump */

int S(int v) { return MulDiv(v, g_dpi, 96); }

/* ---------------------------------------------------------------- settings */

static DWORD opt_get(const WCHAR *name, DWORD def)
{
    DWORD v = def, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, OPTS, name, RRF_RT_REG_DWORD, NULL, &v, &sz)) v = def;
    return v;
}

static void opt_set(const WCHAR *name, DWORD v)
{
    HKEY k;
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, OPTS, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL))
    {
        RegSetValueExW(k, name, 0, REG_DWORD, (BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
}

static void save_settings(void)
{
    WINDOWPLACEMENT wp = { sizeof(wp) };
    opt_set(L"ShowRuler", g_ruler_on);
    opt_set(L"ShowStatusBar", g_statusbar_on);
    opt_set(L"Wrap", g_wrap);
    opt_set(L"Units", g_units);
    opt_set(L"PageWidth", g_pagew);
    opt_set(L"PageHeight", g_pageh);
    opt_set(L"MarginLeft", g_margins.left);
    opt_set(L"MarginTop", g_margins.top);
    opt_set(L"MarginRight", g_margins.right);
    opt_set(L"MarginBottom", g_margins.bottom);
    opt_set(L"PrintPageNumbers", g_page_numbers);
    {
        HKEY k;
        if (!RegCreateKeyExW(HKEY_CURRENT_USER, OPTS, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL))
        {
            RegSetValueExW(k, L"Header", 0, REG_SZ, (BYTE *)g_header, (lstrlenW(g_header) + 1) * sizeof(WCHAR));
            RegSetValueExW(k, L"Footer", 0, REG_SZ, (BYTE *)g_footer, (lstrlenW(g_footer) + 1) * sizeof(WCHAR));
            RegCloseKey(k);
        }
    }
    if (GetWindowPlacement(g_main, &wp) && wp.showCmd != SW_SHOWMINIMIZED)
    {
        HKEY k;
        if (!RegCreateKeyExW(HKEY_CURRENT_USER, OPTS, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL))
        {
            RegSetValueExW(k, L"FrameRect", 0, REG_BINARY, (BYTE *)&wp.rcNormalPosition, sizeof(RECT));
            RegCloseKey(k);
        }
        opt_set(L"Maximized", wp.showCmd == SW_SHOWMAXIMIZED);
    }
}

static void load_settings(void)
{
    g_ruler_on = opt_get(L"ShowRuler", 1) != 0;
    g_statusbar_on = opt_get(L"ShowStatusBar", 1) != 0;
    g_wrap = opt_get(L"Wrap", 2) % 3;
    g_units = opt_get(L"Units", 0) % 4;
    g_pagew = opt_get(L"PageWidth", 12240);
    g_pageh = opt_get(L"PageHeight", 15840);
    g_margins.left = opt_get(L"MarginLeft", 1800);
    g_margins.top = opt_get(L"MarginTop", 1440);
    g_margins.right = opt_get(L"MarginRight", 1800);
    g_margins.bottom = opt_get(L"MarginBottom", 1440);
    g_page_numbers = opt_get(L"PrintPageNumbers", 0) != 0;
    {
        DWORD sz = sizeof(g_header);
        if (RegGetValueW(HKEY_CURRENT_USER, OPTS, L"Header", RRF_RT_REG_SZ, NULL, g_header, &sz)) g_header[0] = 0;
        sz = sizeof(g_footer);
        if (RegGetValueW(HKEY_CURRENT_USER, OPTS, L"Footer", RRF_RT_REG_SZ, NULL, g_footer, &sz)) g_footer[0] = 0;
    }
    if (g_pagew < 1440 || g_pagew > 44640) g_pagew = 12240;
    if (g_pageh < 1440 || g_pageh > 44640) g_pageh = 15840;
}

/* ---------------------------------------------------------------- helpers */

static int msgbox(const WCHAR *text, UINT flags)
{
    lstrcpynW(g_msg, text, ARRAYSIZE(g_msg));
    write_dump();
    return MessageBoxW(g_main, text, L"WordPad", flags);
}

int line_width_twips(void)
{
    return g_pagew - g_margins.left - g_margins.right;
}

/* where the text starts in the edit's client area (the formatting rectangle) */
int edit_left_px(void)
{
    RECT r;
    SendMessageW(g_edit, EM_GETRECT, 0, (LPARAM)&r);
    return r.left;
}

static void apply_wrap(void)
{
    if (g_wrap == 0) SendMessageW(g_edit, EM_SETTARGETDEVICE, 0, 1);
    else if (g_wrap == 1) SendMessageW(g_edit, EM_SETTARGETDEVICE, 0, 0);
    else
    {
        HDC dc = GetDC(g_edit);
        SendMessageW(g_edit, EM_SETTARGETDEVICE, (WPARAM)dc, line_width_twips());
        ReleaseDC(g_edit, dc);
    }
}

static void apply_zoom(void)
{
    if (g_zoom < 10) g_zoom = 10;
    if (g_zoom > 500) g_zoom = 500;
    if (g_zoom == 100) SendMessageW(g_edit, EM_SETZOOM, 0, 0);
    else SendMessageW(g_edit, EM_SETZOOM, g_zoom, 100);
    if (g_ruler) InvalidateRect(g_ruler, NULL, FALSE);
    if (g_status) InvalidateRect(g_status, NULL, FALSE);
}

void set_zoom(int z)
{
    g_zoom = z;
    apply_zoom();
}

static const WCHAR *file_title(void)
{
    const WCHAR *p;
    if (!g_path[0]) return L"Document";
    p = wcsrchr(g_path, '\\');
    return p ? p + 1 : g_path;
}

void update_title(void)
{
    WCHAR t[MAX_PATH + 32];
    wsprintfW(t, L"%s - WordPad", file_title());
    SetWindowTextW(g_main, t);
}

static CHARFORMAT2W get_cf(void)
{
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    SendMessageW(g_edit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    return cf;
}

static PARAFORMAT2 get_pf(void)
{
    PARAFORMAT2 pf;
    memset(&pf, 0, sizeof(pf));
    pf.cbSize = sizeof(pf);
    SendMessageW(g_edit, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
    return pf;
}

static void set_cf(DWORD mask, DWORD effects, const CHARFORMAT2W *src)
{
    CHARFORMAT2W cf;
    if (src) cf = *src;
    else { memset(&cf, 0, sizeof(cf)); cf.cbSize = sizeof(cf); }
    cf.dwMask = mask;
    cf.dwEffects = effects;
    SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    refresh_state();
}

static void set_pf(const PARAFORMAT2 *pf)
{
    SendMessageW(g_edit, EM_SETPARAFORMAT, 0, (LPARAM)pf);
    refresh_state();
}

void set_indents(int left, int first, int right)
{
    PARAFORMAT2 pf, cur = get_pf();
    memset(&pf, 0, sizeof(pf));
    pf.cbSize = sizeof(pf);
    /* RichEdit keeps the first line's indent absolute (dxStartIndent) and the
     * other lines' relative to it (dxOffset); ours is Word's: left for all
     * lines, first relative to left */
    if (left < 0) left = cur.dxStartIndent + cur.dxOffset;
    if (first == INT_MIN) first = -cur.dxOffset;
    if (right < 0) right = cur.dxRightIndent;
    pf.dwMask = PFM_STARTINDENT | PFM_OFFSET | PFM_RIGHTINDENT;
    pf.dxStartIndent = left + first;
    pf.dxOffset = -first;
    pf.dxRightIndent = right;
    set_pf(&pf);
}

/* ---------------------------------------------------------------- the dump */

static void dump_cf(FILE *f)
{
    CHARFORMAT2W cf = get_cf();
    fprintf(f, "font %ls\n", (cf.dwMask & CFM_FACE) ? cf.szFaceName : L"");
    fprintf(f, "size %ld\n", (cf.dwMask & CFM_SIZE) ? cf.yHeight / 20 : -1L);
    fprintf(f, "bold %d\n", (cf.dwMask & CFM_BOLD) ? !!(cf.dwEffects & CFE_BOLD) : -1);
    fprintf(f, "italic %d\n", (cf.dwMask & CFM_ITALIC) ? !!(cf.dwEffects & CFE_ITALIC) : -1);
    fprintf(f, "underline %d\n", (cf.dwMask & CFM_UNDERLINE) ? !!(cf.dwEffects & CFE_UNDERLINE) : -1);
    fprintf(f, "strike %d\n", (cf.dwMask & CFM_STRIKEOUT) ? !!(cf.dwEffects & CFE_STRIKEOUT) : -1);
    fprintf(f, "script %d\n", (cf.dwEffects & CFE_SUPERSCRIPT) ? 1 : (cf.dwEffects & CFE_SUBSCRIPT) ? -1 : 0);
    fprintf(f, "color %06lx\n", (cf.dwEffects & CFE_AUTOCOLOR) ? 0xFFFFFFFFul & 0xFFFFFF : (unsigned long)cf.crTextColor);
    fprintf(f, "autocolor %d\n", !!(cf.dwEffects & CFE_AUTOCOLOR));
    fprintf(f, "backcolor %06lx\n", (cf.dwEffects & CFE_AUTOBACKCOLOR) ? 0xFFFFFFul : (unsigned long)cf.crBackColor);
}

static void dump_pf(FILE *f)
{
    PARAFORMAT2 pf = get_pf();
    fprintf(f, "align %d\n", pf.wAlignment);
    fprintf(f, "numbering %d %d\n", pf.wNumbering, pf.wNumberingStyle);
    fprintf(f, "indent %ld %ld %ld\n", pf.dxStartIndent + pf.dxOffset, -pf.dxOffset, pf.dxRightIndent);
    fprintf(f, "spacing %d %ld %ld %ld\n", pf.bLineSpacingRule, pf.dyLineSpacing, pf.dySpaceBefore, pf.dySpaceAfter);
    fprintf(f, "tabs %d", pf.cTabCount);
    for (int i = 0; i < pf.cTabCount; i++) fprintf(f, " %ld", pf.rgxTabs[i] & 0xFFFFFF);
    fputc('\n', f);
}

void write_dump(void)
{
    FILE *f;
    WCHAR t[MAX_PATH + 32];
    CHARRANGE cr;
    GETTEXTLENGTHEX tl = { GTL_NUMCHARS | GTL_PRECISE, 1200 };
    RECT r;
    POINT o = { 0, 0 };
    if (!g_dump[0] || !g_main) return;
    if (!(f = _wfopen(g_dump, L"w"))) return;
    GetWindowTextW(g_main, t, ARRAYSIZE(t));
    fprintf(f, "title %ls\n", t);
    fprintf(f, "path %ls\n", g_path);
    fprintf(f, "format %ls\n", FMT_NAMES[g_format]);
    fprintf(f, "dirty %d\n", (int)SendMessageW(g_edit, EM_GETMODIFY, 0, 0));
    fprintf(f, "tab %d\n", g_tab);
    fprintf(f, "zoom %d\n", g_zoom);
    fprintf(f, "ruler %d\n", g_ruler_on);
    fprintf(f, "statusbar %d\n", g_statusbar_on);
    fprintf(f, "wrap %d\n", g_wrap);
    fprintf(f, "units %d\n", g_units);
    SendMessageW(g_edit, EM_EXGETSEL, 0, (LPARAM)&cr);
    fprintf(f, "sel %ld %ld\n", cr.cpMin, cr.cpMax);
    fprintf(f, "length %ld\n", (long)SendMessageW(g_edit, EM_GETTEXTLENGTHEX, (WPARAM)&tl, 0));
    fprintf(f, "canundo %d\n", (int)SendMessageW(g_edit, EM_CANUNDO, 0, 0));
    {
        /* how far the text advances over its first ten characters (zoom
         * must scale this with the font) */
        POINTL a = { 0, 0 }, b = { 0, 0 };
        SendMessageW(g_edit, EM_POSFROMCHAR, (WPARAM)&a, 0);
        SendMessageW(g_edit, EM_POSFROMCHAR, (WPARAM)&b, 10);
        fprintf(f, "advance10 %ld\n", b.x - a.x);
    }
    dump_cf(f);
    dump_pf(f);
    GetWindowRect(g_edit, &r);
    fprintf(f, "edit %ld %ld %ld %ld\n", r.left, r.top, r.right, r.bottom);
    ClientToScreen(g_edit, &o);
    fprintf(f, "textleft %ld\n", o.x + edit_left_px());
    GetWindowRect(g_main, &r);
    fprintf(f, "window %ld %ld %ld %ld\n", r.left, r.top, r.right, r.bottom);
    if (IsWindowVisible(g_face))
    {
        GetWindowRect(g_face, &r);
        fprintf(f, "facebox %ld %ld %ld %ld\n", r.left, r.top, r.right, r.bottom);
        GetWindowRect(g_size, &r);
        fprintf(f, "sizebox %ld %ld %ld %ld\n", r.left, r.top, r.right, r.bottom);
    }
    ribbon_dump(f);
    ruler_dump(f);
    status_dump(f);
    {
        extern void preview_dump(FILE *f);
        preview_dump(f);
    }
    fprintf(f, "message %ls\n", g_msg);
    fprintf(f, "finddlg %d\n", g_finddlg && IsWindowVisible(g_finddlg));
    fprintf(f, "end\n");
    fclose(f);
}

/* ---------------------------------------------------------------- the ribbon's state */

void refresh_state(void)
{
    CHARFORMAT2W cf = get_cf();
    WCHAR cur[64], sz[16];
    GetWindowTextW(g_face, cur, ARRAYSIZE(cur));
    if (cf.dwMask & CFM_FACE) { if (wcscmp(cur, cf.szFaceName)) SetWindowTextW(g_face, cf.szFaceName); }
    else if (cur[0]) SetWindowTextW(g_face, L"");
    if (cf.dwMask & CFM_SIZE)
    {
        int tw = cf.yHeight;
        if (tw % 20) swprintf(sz, 16, L"%d.5", tw / 20);
        else swprintf(sz, 16, L"%d", tw / 20);
    }
    else sz[0] = 0;
    GetWindowTextW(g_size, cur, ARRAYSIZE(cur));
    if (wcscmp(cur, sz)) SetWindowTextW(g_size, sz);
    InvalidateRect(g_ribbon, NULL, FALSE);
    if (g_ruler) InvalidateRect(g_ruler, NULL, FALSE);
    if (g_status) InvalidateRect(g_status, NULL, FALSE);
    write_dump();
}

BOOL ribbon_is_on(int cmd)
{
    CHARFORMAT2W cf;
    PARAFORMAT2 pf;
    switch (cmd)
    {
    case CMD_BOLD: cf = get_cf(); return (cf.dwMask & CFM_BOLD) && (cf.dwEffects & CFE_BOLD);
    case CMD_ITALIC: cf = get_cf(); return (cf.dwMask & CFM_ITALIC) && (cf.dwEffects & CFE_ITALIC);
    case CMD_UNDERLINE: cf = get_cf(); return (cf.dwMask & CFM_UNDERLINE) && (cf.dwEffects & CFE_UNDERLINE);
    case CMD_STRIKE: cf = get_cf(); return (cf.dwMask & CFM_STRIKEOUT) && (cf.dwEffects & CFE_STRIKEOUT);
    case CMD_SUBSCRIPT: cf = get_cf(); return !!(cf.dwEffects & CFE_SUBSCRIPT);
    case CMD_SUPERSCRIPT: cf = get_cf(); return !!(cf.dwEffects & CFE_SUPERSCRIPT);
    case CMD_ALIGN_LEFT: pf = get_pf(); return pf.wAlignment == PFA_LEFT;
    case CMD_ALIGN_CENTER: pf = get_pf(); return pf.wAlignment == PFA_CENTER;
    case CMD_ALIGN_RIGHT: pf = get_pf(); return pf.wAlignment == PFA_RIGHT;
    case CMD_ALIGN_JUSTIFY: pf = get_pf(); return pf.wAlignment == PFA_JUSTIFY;
    case CMD_LIST: pf = get_pf(); return pf.wNumbering != 0;
    case CMD_RULER: return g_ruler_on;
    case CMD_STATUSBAR: return g_statusbar_on;
    case CMD_TAB_HOME: return g_tab == 0;
    case CMD_TAB_VIEW: return g_tab == 1;
    }
    return FALSE;
}

/* ---------------------------------------------------------------- files */

typedef struct { const BYTE *p; size_t n, off; } memstream;
typedef struct { Buf b; } outstream;

static DWORD CALLBACK in_cb(DWORD_PTR cookie, BYTE *buf, LONG cb, LONG *got)
{
    memstream *m = (memstream *)cookie;
    size_t left = m->n - m->off;
    if ((size_t)cb > left) cb = (LONG)left;
    memcpy(buf, m->p + m->off, cb);
    m->off += cb;
    *got = cb;
    return 0;
}

static DWORD CALLBACK out_cb(DWORD_PTR cookie, BYTE *buf, LONG cb, LONG *done)
{
    buf_add((Buf *)cookie, buf, cb);
    *done = cb;
    return 0;
}

static void stream_in(const void *data, size_t n, UINT flags)
{
    memstream m = { data, n, 0 };
    EDITSTREAM es = { (DWORD_PTR)&m, 0, in_cb };
    SendMessageW(g_edit, EM_STREAMIN, flags, (LPARAM)&es);
}

static char *stream_out(UINT flags, size_t *len)
{
    Buf b = { 0 };
    EDITSTREAM es = { (DWORD_PTR)&b, 0, out_cb };
    SendMessageW(g_edit, EM_STREAMOUT, flags, (LPARAM)&es);
    buf_add(&b, "\0\0", 2);
    *len = b.n - 2;
    return b.p;
}

static BYTE *read_file(const WCHAR *path, size_t *n)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER sz;
    BYTE *p;
    DWORD got;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart > 0x7FFFFFF0) { CloseHandle(h); return NULL; }
    p = malloc((size_t)sz.QuadPart + 4);
    if (!p || !ReadFile(h, p, (DWORD)sz.QuadPart, &got, NULL) || got != (DWORD)sz.QuadPart)
    { free(p); CloseHandle(h); return NULL; }
    CloseHandle(h);
    memset(p + got, 0, 4);
    *n = got;
    return p;
}

static BOOL write_file(const WCHAR *path, const void *data, size_t n)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD done;
    BOOL ok;
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(h, data, (DWORD)n, &done, NULL) && done == n;
    CloseHandle(h);
    return ok;
}

static BOOL valid_utf8(const BYTE *p, size_t n)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)p, (int)n, NULL, 0) > 0 || !n;
}

static void set_default_format(BOOL plain)
{
    CHARFORMAT2W cf;
    PARAFORMAT2 pf;
    extern const WCHAR *default_face(BOOL mono);
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_CHARSET | CFM_COLOR | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT;
    cf.dwEffects = CFE_AUTOCOLOR;
    cf.yHeight = 11 * 20;
    cf.bCharSet = DEFAULT_CHARSET;
    lstrcpynW(cf.szFaceName, default_face(plain), LF_FACESIZE);
    SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
    memset(&pf, 0, sizeof(pf));
    pf.cbSize = sizeof(pf);
    if (!plain)
    {
        /* WordPad's new document: 1.15 lines, 10 pt after each paragraph */
        pf.dwMask = PFM_LINESPACING | PFM_SPACEAFTER;
        pf.bLineSpacingRule = 5;
        pf.dyLineSpacing = 23;                      /* in 20ths of a line: 1.15 */
        pf.dySpaceAfter = 200;
        SendMessageW(g_edit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    }
}

static void new_document(void)
{
    SetWindowTextW(g_edit, L"");
    set_default_format(FALSE);
    SendMessageW(g_edit, EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(g_edit, EM_SETMODIFY, FALSE, 0);
    g_path[0] = 0;
    g_format = FMT_RTF;
    update_title();
    refresh_state();
}

static int detect_format(const WCHAR *path, const BYTE *p, size_t n)
{
    const WCHAR *ext = wcsrchr(path, '.');
    if (n >= 5 && !memcmp(p, "{\\rtf", 5)) return FMT_RTF;
    if (n >= 8 && !memcmp(p, "\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8)) return FMT_DOC;
    if (n >= 4 && !memcmp(p, "PK\3\4", 4))
    {
        if (ext && !lstrcmpiW(ext, L".odt")) return FMT_ODT;
        if (n > 60 && !memcmp(p + 30, "mimetype", 8)) return FMT_ODT;
        return FMT_DOCX;
    }
    if (n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) return FMT_UTXT;
    return FMT_TXT;
}

/* text bytes to UTF-16 (BOMs, UTF-8 when valid, else the ANSI code page) */
static WCHAR *text_to_wide(const BYTE *p, size_t n)
{
    WCHAR *w;
    int len;
    if (n >= 2 && p[0] == 0xFF && p[1] == 0xFE)
    {
        len = (int)((n - 2) / 2);
        w = malloc((len + 1) * sizeof(WCHAR));
        memcpy(w, p + 2, len * sizeof(WCHAR));
        w[len] = 0;
        return w;
    }
    if (n >= 2 && p[0] == 0xFE && p[1] == 0xFF)
    {
        len = (int)((n - 2) / 2);
        w = malloc((len + 1) * sizeof(WCHAR));
        for (int i = 0; i < len; i++) w[i] = (WCHAR)(p[2 + 2 * i] << 8 | p[3 + 2 * i]);
        w[len] = 0;
        return w;
    }
    {
        UINT cp = CP_ACP;
        if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) { p += 3; n -= 3; cp = CP_UTF8; }
        else if (valid_utf8(p, n)) cp = CP_UTF8;
        len = MultiByteToWideChar(cp, 0, (const char *)p, (int)n, NULL, 0);
        w = malloc((len + 1) * sizeof(WCHAR));
        MultiByteToWideChar(cp, 0, (const char *)p, (int)n, w, len);
        w[len] = 0;
        return w;
    }
}

static BOOL load_doc(const Doc *d)
{
    size_t n;
    char *rtf = doc_to_rtf(d, &n);
    if (!rtf) return FALSE;
    SetWindowTextW(g_edit, L"");
    set_default_format(FALSE);
    stream_in(rtf, n, SF_RTF);
    free(rtf);
    return TRUE;
}

BOOL open_file(const WCHAR *path)
{
    size_t n;
    BYTE *p = read_file(path, &n);
    WCHAR err[256] = L"", full[MAX_PATH];
    int fmt;
    BOOL ok = TRUE;

    if (!p)
    {
        WCHAR m[MAX_PATH + 64];
        swprintf(m, ARRAYSIZE(m), L"Cannot find the %ls file.\n\nPlease verify that the correct path and file name are given.", path);
        msgbox(m, MB_OK | MB_ICONWARNING);
        return FALSE;
    }
    fmt = detect_format(path, p, n);
    SendMessageW(g_edit, WM_SETREDRAW, FALSE, 0);
    if (fmt == FMT_RTF)
    {
        SetWindowTextW(g_edit, L"");
        set_default_format(FALSE);
        stream_in(p, n, SF_RTF);
    }
    else if (fmt == FMT_DOCX || fmt == FMT_ODT || fmt == FMT_DOC)
    {
        Doc d;
        doc_init(&d);
        ok = fmt == FMT_DOCX ? docx_read(path, &d, err, ARRAYSIZE(err)) :
             fmt == FMT_ODT ? odt_read(path, &d, err, ARRAYSIZE(err)) : doc97_read(path, &d, err, ARRAYSIZE(err));
        if (ok) ok = load_doc(&d);
        doc_free(&d);
    }
    else
    {
        WCHAR *w = text_to_wide(p, n);
        SETTEXTEX st = { ST_DEFAULT, 1200 };
        SetWindowTextW(g_edit, L"");
        set_default_format(TRUE);
        SendMessageW(g_edit, EM_SETTEXTEX, (WPARAM)&st, (LPARAM)w);
        free(w);
    }
    free(p);
    SendMessageW(g_edit, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_edit, NULL, TRUE);
    if (!ok)
    {
        WCHAR m[512];
        swprintf(m, ARRAYSIZE(m), L"WordPad cannot open %ls.\n\n%ls", path, err);
        msgbox(m, MB_OK | MB_ICONWARNING);
        new_document();
        return FALSE;
    }
    if (!GetFullPathNameW(path, MAX_PATH, full, NULL)) lstrcpynW(full, path, MAX_PATH);
    lstrcpynW(g_path, full, MAX_PATH);
    g_format = fmt;
    SendMessageW(g_edit, EM_SETSEL, 0, 0);
    SendMessageW(g_edit, EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(g_edit, EM_SETMODIFY, FALSE, 0);
    SHAddToRecentDocs(SHARD_PATHW, g_path);
    update_title();
    refresh_state();
    return TRUE;
}

static BOOL save_to(const WCHAR *path, int fmt)
{
    size_t n;
    char *data;
    WCHAR err[256] = L"";
    BOOL ok = FALSE;

    if (fmt == FMT_RTF)
    {
        data = stream_out(SF_RTF, &n);
        ok = write_file(path, data, n);
        free(data);
    }
    else if (fmt == FMT_DOCX || fmt == FMT_ODT)
    {
        Doc d;
        data = stream_out(SF_RTF, &n);
        doc_init(&d);
        ok = doc_from_rtf(&d, data, n);
        free(data);
        if (ok) ok = fmt == FMT_DOCX ? docx_write(path, &d, err, ARRAYSIZE(err)) : odt_write(path, &d, err, ARRAYSIZE(err));
        doc_free(&d);
    }
    else
    {
        WCHAR *w = (WCHAR *)stream_out(SF_TEXT | SF_UNICODE, &n);
        if (fmt == FMT_UTXT)
        {
            Buf b = { 0 };
            buf_add(&b, "\xFF\xFE", 2);
            buf_add(&b, w, n);
            ok = write_file(path, b.p, b.n);
            free(b.p);
        }
        else
        {
            int len = WideCharToMultiByte(CP_UTF8, 0, w, (int)(n / sizeof(WCHAR)), NULL, 0, NULL, NULL);
            char *u = malloc(len + 1);
            WideCharToMultiByte(CP_UTF8, 0, w, (int)(n / sizeof(WCHAR)), u, len, NULL, NULL);
            ok = write_file(path, u, len);
            free(u);
        }
        free(w);
    }
    if (!ok)
    {
        WCHAR m[512];
        swprintf(m, ARRAYSIZE(m), L"WordPad cannot save %ls.\n\n%ls", path, err[0] ? err : L"Access is denied.");
        msgbox(m, MB_OK | MB_ICONWARNING);
        return FALSE;
    }
    lstrcpynW(g_path, path, MAX_PATH);
    g_format = fmt;
    SendMessageW(g_edit, EM_SETMODIFY, FALSE, 0);
    SHAddToRecentDocs(SHARD_PATHW, g_path);
    update_title();
    write_dump();
    return TRUE;
}

static BOOL confirm_text_format(void)
{
    return msgbox(L"You are about to save the document in a Text-Only format, which will remove all formatting. "
                  L"Are you sure you want to do this?", MB_YESNO | MB_ICONWARNING) == IDYES;
}

static BOOL save_as(int fmt)
{
    static const WCHAR filter[] =
        L"Rich Text Format (RTF)\0*.rtf\0"
        L"Office Open XML Document\0*.docx\0"
        L"OpenDocument Text\0*.odt\0"
        L"Text Document\0*.txt\0"
        L"Unicode Text Document\0*.txt\0"
        L"All Documents\0*.*\0";
    static const WCHAR *exts[] = { L"rtf", L"docx", L"odt", L"txt", L"txt" };
    WCHAR file[MAX_PATH];
    OPENFILENAMEW ofn;
    const WCHAR *dot;

    if (fmt < 0) fmt = g_format;
    lstrcpynW(file, g_path[0] ? file_title() : L"Document", MAX_PATH);
    if ((dot = wcsrchr(file, '.'))) file[dot - file] = 0;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter;
    ofn.nFilterIndex = fmt + 1;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = exts[fmt];
    ofn.lpstrTitle = L"Save As";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return FALSE;
    {
        /* a typed extension of ours decides; otherwise the chosen type */
        const WCHAR *e = wcsrchr(file, '.');
        int byext = -1;
        if (e && !lstrcmpiW(e, L".rtf")) byext = FMT_RTF;
        else if (e && !lstrcmpiW(e, L".docx")) byext = FMT_DOCX;
        else if (e && !lstrcmpiW(e, L".odt")) byext = FMT_ODT;
        else if (e && !lstrcmpiW(e, L".txt")) byext = ofn.nFilterIndex == FMT_UTXT + 1 ? FMT_UTXT : FMT_TXT;
        if (byext >= 0) fmt = byext;
        else if (ofn.nFilterIndex >= 1 && ofn.nFilterIndex <= FMT_COUNT) fmt = ofn.nFilterIndex - 1;
        else fmt = FMT_RTF;
    }
    if ((fmt == FMT_TXT || fmt == FMT_UTXT) && !confirm_text_format()) return FALSE;
    return save_to(file, fmt);
}

static BOOL save(void)
{
    if (!g_path[0]) return save_as(FMT_RTF);
    if (g_format == FMT_DOC) return save_as(FMT_DOCX);      /* WordPad reads Word 97-2003 files, writes .docx */
    return save_to(g_path, g_format);
}

/* Save / Don't Save / Cancel */
static BOOL may_discard(void)
{
    TASKDIALOGCONFIG tc;
    TASKDIALOG_BUTTON b[2] = { { IDYES, L"&Save" }, { IDNO, L"Do&n't Save" } };
    WCHAR m[MAX_PATH + 64];
    int r = IDCANCEL;
    if (!SendMessageW(g_edit, EM_GETMODIFY, 0, 0)) return TRUE;
    swprintf(m, ARRAYSIZE(m), L"Do you want to save changes to %ls?", file_title());
    lstrcpynW(g_msg, m, ARRAYSIZE(g_msg));
    write_dump();
    memset(&tc, 0, sizeof(tc));
    tc.cbSize = sizeof(tc);
    tc.hwndParent = g_main;
    tc.pszWindowTitle = L"WordPad";
    tc.pszMainInstruction = m;
    tc.pButtons = b;
    tc.cButtons = 2;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.nDefaultButton = IDYES;
    if (FAILED(TaskDialogIndirect(&tc, &r, NULL, NULL)))
        r = MessageBoxW(g_main, m, L"WordPad", MB_YESNOCANCEL | MB_ICONWARNING);
    g_msg[0] = 0;
    if (r == IDYES) return save();
    return r == IDNO;
}

static void open_dialog(void)
{
    static const WCHAR filter[] =
        L"All WordPad Documents (*.rtf, *.docx, *.doc, *.odt, *.txt)\0*.rtf;*.docx;*.doc;*.odt;*.txt\0"
        L"Rich Text Format (RTF) (*.rtf)\0*.rtf\0"
        L"Office Open XML Document (*.docx)\0*.docx\0"
        L"Word 97-2003 Document (*.doc)\0*.doc\0"
        L"OpenDocument Text (*.odt)\0*.odt\0"
        L"Text Documents (*.txt)\0*.txt\0"
        L"All Documents (*.*)\0*.*\0";
    WCHAR file[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    if (!may_discard()) return;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Open";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) open_file(file);
}

/* ---------------------------------------------------------------- inserting */

static void insert_picture(void)
{
    static const WCHAR filter[] =
        L"All Picture Files\0*.bmp;*.dib;*.jpg;*.jpeg;*.jpe;*.jfif;*.gif;*.tif;*.tiff;*.png;*.ico\0"
        L"Bitmap Files (*.bmp;*.dib)\0*.bmp;*.dib\0JPEG (*.jpg;*.jpeg;*.jpe;*.jfif)\0*.jpg;*.jpeg;*.jpe;*.jfif\0"
        L"GIF (*.gif)\0*.gif\0TIFF (*.tif;*.tiff)\0*.tif;*.tiff\0PNG (*.png)\0*.png\0All Files (*.*)\0*.*\0";
    WCHAR file[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    BYTE *dib;
    DWORD size;
    int w, h;

    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Select Picture";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    if (!pic_load_file(file, &dib, &size, &w, &h))
    {
        msgbox(L"This is not a valid picture file, or its format is not supported.", MB_OK | MB_ICONWARNING);
        return;
    }
    {
        Buf b = { 0 };
        static const char hex[] = "0123456789abcdef";
        int tw = w * 15, th = h * 15;          /* pixels at 96 dpi to twips */
        /* no wider than the line */
        int lw = line_width_twips();
        if (tw > lw) { th = MulDiv(th, lw, tw); tw = lw; }
        buf_printf(&b, "{\\rtf1{\\pict\\dibitmap0\\picw%d\\pich%d\\picwgoal%d\\pichgoal%d ", w, h, tw, th);
        for (DWORD i = 0; i < size; i++)
        {
            char c[2] = { hex[dib[i] >> 4], hex[dib[i] & 15] };
            buf_add(&b, c, 2);
            if (i % 64 == 63) buf_add(&b, "\n", 1);
        }
        buf_str(&b, "}}");
        stream_in(b.p, b.n, SF_RTF | SFF_SELECTION);
        free(b.p);
        free(dib);
    }
    refresh_state();
}

/* ---------------------------------------------------------------- formatting commands */

static void toggle_effect(DWORD mask, DWORD effect)
{
    CHARFORMAT2W cf = get_cf();
    BOOL on = (cf.dwMask & mask) && (cf.dwEffects & effect);
    set_cf(mask, on ? 0 : effect, NULL);
}

static void toggle_script(DWORD effect)
{
    CHARFORMAT2W cf = get_cf();
    BOOL on = !!(cf.dwEffects & effect);
    set_cf(CFM_SUBSCRIPT | CFM_SUPERSCRIPT, on ? 0 : effect, NULL);
}

static const int SIZES[] = { 8, 9, 10, 11, 12, 14, 16, 18, 20, 22, 24, 26, 28, 36, 48, 72 };

static void grow_font(int dir)
{
    CHARFORMAT2W cf = get_cf(), nf;
    int pt = (cf.dwMask & CFM_SIZE) ? cf.yHeight / 20 : 11, i, n = ARRAYSIZE(SIZES), np = pt;
    if (dir > 0) { np = pt >= 72 ? pt + 10 : pt + 1; for (i = 0; i < n; i++) if (SIZES[i] > pt) { np = SIZES[i]; break; } }
    else { np = pt > 72 ? pt - 10 : 1; for (i = n - 1; i >= 0; i--) if (SIZES[i] < pt) { np = SIZES[i]; break; } }
    if (np < 1) np = 1;
    if (np > 1638) np = 1638;
    memset(&nf, 0, sizeof(nf));
    nf.cbSize = sizeof(nf);
    nf.yHeight = np * 20;
    set_cf(CFM_SIZE, 0, &nf);
}

void apply_font_face(const WCHAR *face)
{
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    lstrcpynW(cf.szFaceName, face, LF_FACESIZE);
    cf.bCharSet = DEFAULT_CHARSET;
    set_cf(CFM_FACE | CFM_CHARSET, 0, &cf);
}

void apply_font_size(const WCHAR *s)
{
    CHARFORMAT2W cf;
    double v = _wtof(s);
    if (v < 1 || v > 1638)
    {
        msgbox(L"The number must be between 1 and 1638.", MB_OK | MB_ICONWARNING);
        refresh_state();
        return;
    }
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.yHeight = (LONG)(v * 2 + 0.5) * 10;
    set_cf(CFM_SIZE, 0, &cf);
}

static void set_color(COLORREF c, BOOL automatic)
{
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.crTextColor = c;
    g_text_color = c; g_text_auto = automatic;
    set_cf(CFM_COLOR, automatic ? CFE_AUTOCOLOR : 0, &cf);
}

static void set_highlight(COLORREF c, BOOL none)
{
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.crBackColor = c;
    g_hl_color = c; g_hl_none = none;
    set_cf(CFM_BACKCOLOR, none ? CFE_AUTOBACKCOLOR : 0, &cf);
}

static void set_align(WORD a)
{
    PARAFORMAT2 pf;
    memset(&pf, 0, sizeof(pf));
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_ALIGNMENT;
    pf.wAlignment = a;
    set_pf(&pf);
}

static void set_list(int style)
{
    static const WORD num[] = { 0, PFN_BULLET, PFN_ARABIC, PFN_LCLETTER, PFN_UCLETTER, PFN_LCROMAN, PFN_UCROMAN };
    PARAFORMAT2 pf, cur = get_pf();
    memset(&pf, 0, sizeof(pf));
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_NUMBERING | PFM_NUMBERINGSTYLE | PFM_NUMBERINGSTART | PFM_NUMBERINGTAB | PFM_STARTINDENT | PFM_OFFSET;
    pf.wNumbering = num[style];
    pf.wNumberingStyle = style >= 2 ? PFNS_PERIOD : 0;
    pf.wNumberingStart = 1;
    pf.wNumberingTab = 360;
    if (style && !cur.wNumbering) { pf.dxStartIndent = cur.dxStartIndent + 360; pf.dxOffset = 360; }
    else if (!style && cur.wNumbering) { pf.dxStartIndent = max(0, cur.dxStartIndent - 360); pf.dxOffset = 0; }
    else { pf.dxStartIndent = cur.dxStartIndent; pf.dxOffset = cur.dxOffset; }
    set_pf(&pf);
}

static void set_spacing(int which)
{
    PARAFORMAT2 pf, cur = get_pf();
    static const int twentieths[4] = { 20, 23, 30, 40 };
    memset(&pf, 0, sizeof(pf));
    pf.cbSize = sizeof(pf);
    if (which == 4)
    {
        pf.dwMask = PFM_SPACEAFTER;
        pf.dySpaceAfter = cur.dySpaceAfter ? 0 : 200;
    }
    else
    {
        pf.dwMask = PFM_LINESPACING;
        pf.bLineSpacingRule = 5;
        pf.dyLineSpacing = twentieths[which];
    }
    set_pf(&pf);
}

static void indent_by(int delta)
{
    PARAFORMAT2 cur = get_pf();
    int left = cur.dxStartIndent + cur.dxOffset + delta;
    if (left < 0) left = 0;
    set_indents(left, INT_MIN, -1);
}

static void popup_at_ribbon(HMENU m, int anchor_cmd)
{
    extern BOOL ribbon_item_rect(int cmd, RECT *r);
    RECT r;
    POINT pt;
    int cmd;
    if (ribbon_item_rect(anchor_cmd, &r)) { pt.x = r.left; pt.y = r.bottom; ClientToScreen(g_ribbon, &pt); }
    else GetCursorPos(&pt);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, g_main, NULL);
    DestroyMenu(m);
    if (cmd) do_command(cmd);
}

static HMENU color_menu(int base, BOOL highlight)
{
    static const WCHAR *names[NPAL] = { L"Black", L"Gray-50%", L"Dark red", L"Red", L"Orange", L"Yellow", L"Green",
        L"Turquoise", L"Indigo", L"Purple", L"White", L"Gray-25%", L"Brown", L"Rose", L"Gold", L"Light yellow",
        L"Lime", L"Light turquoise", L"Blue-gray", L"Lavender" };
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, base + PAL_AUTO, highlight ? L"No color" : L"Automatic");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (int i = 0; i < NPAL; i++) AppendMenuW(m, MF_STRING | ((i % 5 == 0 && i) ? MF_MENUBARBREAK : 0), base + i, names[i]);
    return m;
}

static void file_menu(void)
{
    HMENU m = CreatePopupMenu(), sa = CreatePopupMenu(), pr = CreatePopupMenu();
    AppendMenuW(sa, MF_STRING, CMD_SAVEAS_RTF, L"&Rich Text document");
    AppendMenuW(sa, MF_STRING, CMD_SAVEAS_DOCX, L"Office &Open XML document");
    AppendMenuW(sa, MF_STRING, CMD_SAVEAS_ODT, L"Open&Document text");
    AppendMenuW(sa, MF_STRING, CMD_SAVEAS_TXT, L"&Plain text document");
    AppendMenuW(sa, MF_STRING, CMD_SAVEAS_OTHER, L"Other &formats");
    AppendMenuW(pr, MF_STRING, CMD_PRINT, L"&Print\tCtrl+P");
    AppendMenuW(pr, MF_STRING, CMD_QUICKPRINT, L"&Quick print");
    AppendMenuW(pr, MF_STRING, CMD_PREVIEW, L"Print pre&view");
    AppendMenuW(m, MF_STRING, CMD_NEW, L"&New\tCtrl+N");
    AppendMenuW(m, MF_STRING, CMD_OPEN, L"&Open\tCtrl+O");
    AppendMenuW(m, MF_STRING, CMD_SAVE, L"&Save\tCtrl+S");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sa, L"Save &as");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)pr, L"&Print");
    AppendMenuW(m, MF_STRING, CMD_PAGESETUP, L"Page set&up");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_ABOUT, L"A&bout WordPad");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"E&xit");
    popup_at_ribbon(m, CMD_FILE_MENU);
}

static void about(void)
{
    msgbox(L"WordPad\n\nStained Glass OS\n\nOpens and saves Rich Text (.rtf), Office Open XML (.docx), "
           L"OpenDocument Text (.odt) and plain text documents.\n\nFree software under the GNU Affero General "
           L"Public License, version 3 or later.", MB_OK | MB_ICONINFORMATION);
}

static void font_dialog(void)
{
    CHARFORMAT2W cf = get_cf();
    LOGFONTW lf;
    CHOOSEFONTW c;
    HDC dc = GetDC(g_main);
    memset(&lf, 0, sizeof(lf));
    lstrcpynW(lf.lfFaceName, cf.szFaceName, LF_FACESIZE);
    lf.lfHeight = -MulDiv(cf.yHeight, GetDeviceCaps(dc, LOGPIXELSY), 1440);
    ReleaseDC(g_main, dc);
    lf.lfWeight = (cf.dwEffects & CFE_BOLD) ? FW_BOLD : FW_NORMAL;
    lf.lfItalic = !!(cf.dwEffects & CFE_ITALIC);
    lf.lfUnderline = !!(cf.dwEffects & CFE_UNDERLINE);
    lf.lfStrikeOut = !!(cf.dwEffects & CFE_STRIKEOUT);
    lf.lfCharSet = DEFAULT_CHARSET;
    memset(&c, 0, sizeof(c));
    c.lStructSize = sizeof(c);
    c.hwndOwner = g_main;
    c.lpLogFont = &lf;
    c.Flags = CF_SCREENFONTS | CF_EFFECTS | CF_INITTOLOGFONTSTRUCT | CF_NOSCRIPTSEL;
    c.rgbColors = (cf.dwEffects & CFE_AUTOCOLOR) ? GetSysColor(COLOR_WINDOWTEXT) : cf.crTextColor;
    if (!ChooseFontW(&c)) return;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    lstrcpynW(cf.szFaceName, lf.lfFaceName, LF_FACESIZE);
    cf.yHeight = c.iPointSize * 2;
    cf.crTextColor = c.rgbColors;
    cf.bCharSet = lf.lfCharSet;
    set_cf(CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT | CFM_CHARSET,
           (lf.lfWeight >= FW_BOLD ? CFE_BOLD : 0) | (lf.lfItalic ? CFE_ITALIC : 0) |
           (lf.lfUnderline ? CFE_UNDERLINE : 0) | (lf.lfStrikeOut ? CFE_STRIKEOUT : 0), &cf);
}

/* ---------------------------------------------------------------- commands */

static void set_ruler(int on)
{
    g_ruler_on = on;
    layout();
}

void do_command(int cmd)
{
    switch (cmd)
    {
    case CMD_NEW: if (may_discard()) new_document(); break;
    case CMD_OPEN: open_dialog(); break;
    case CMD_SAVE: save(); break;
    case CMD_SAVEAS: save_as(-1); break;
    case CMD_SAVEAS_RTF: save_as(FMT_RTF); break;
    case CMD_SAVEAS_DOCX: save_as(FMT_DOCX); break;
    case CMD_SAVEAS_ODT: save_as(FMT_ODT); break;
    case CMD_SAVEAS_TXT: save_as(FMT_TXT); break;
    case CMD_SAVEAS_OTHER: save_as(-1); break;
    case CMD_PRINT: print_document(TRUE); break;
    case CMD_QUICKPRINT: print_document(FALSE); break;
    case CMD_PREVIEW: print_preview(); break;
    case CMD_PAGESETUP: page_setup(); apply_wrap(); InvalidateRect(g_ruler, NULL, FALSE); break;
    case CMD_ABOUT: about(); break;
    case CMD_EXIT: PostMessageW(g_main, WM_CLOSE, 0, 0); break;
    case CMD_FILE_MENU: file_menu(); break;
    case CMD_TAB_HOME: g_tab = 0; layout(); break;
    case CMD_TAB_VIEW: g_tab = 1; layout(); break;
    case CMD_UNDO: SendMessageW(g_edit, EM_UNDO, 0, 0); break;
    case CMD_REDO: SendMessageW(g_edit, EM_REDO, 0, 0); break;
    case CMD_CUT: SendMessageW(g_edit, WM_CUT, 0, 0); break;
    case CMD_COPY: SendMessageW(g_edit, WM_COPY, 0, 0); break;
    case CMD_PASTE: SendMessageW(g_edit, WM_PASTE, 0, 0); break;
    case CMD_PASTESPECIAL: SendMessageW(g_edit, EM_PASTESPECIAL, CF_UNICODETEXT, 0); break;
    case CMD_PASTE_MENU:
    {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, CMD_PASTE, L"&Paste");
        AppendMenuW(m, MF_STRING, CMD_PASTESPECIAL, L"Paste &special (text only)");
        popup_at_ribbon(m, CMD_PASTE);
        break;
    }
    case CMD_SELECTALL: SendMessageW(g_edit, EM_SETSEL, 0, -1); break;
    case CMD_FIND: find_open(FALSE); break;
    case CMD_REPLACE: find_open(TRUE); break;
    case CMD_FINDNEXT: find_next(); break;
    case CMD_GROW: grow_font(1); break;
    case CMD_SHRINK: grow_font(-1); break;
#ifndef SG_MUTANT_NOBOLD
    case CMD_BOLD: toggle_effect(CFM_BOLD, CFE_BOLD); break;
#endif
    case CMD_ITALIC: toggle_effect(CFM_ITALIC, CFE_ITALIC); break;
    case CMD_UNDERLINE: toggle_effect(CFM_UNDERLINE, CFE_UNDERLINE); break;
    case CMD_STRIKE: toggle_effect(CFM_STRIKEOUT, CFE_STRIKEOUT); break;
    case CMD_SUBSCRIPT: toggle_script(CFE_SUBSCRIPT); break;
    case CMD_SUPERSCRIPT: toggle_script(CFE_SUPERSCRIPT); break;
    case CMD_COLOR: set_color(g_text_color, g_text_auto); break;
    case CMD_HIGHLIGHT: set_highlight(g_hl_color, g_hl_none); break;
    case CMD_COLOR_MENU: popup_at_ribbon(color_menu(CMD_COLOR_BASE, FALSE), CMD_COLOR); break;
    case CMD_HIGHLIGHT_MENU: popup_at_ribbon(color_menu(CMD_HL_BASE, TRUE), CMD_HIGHLIGHT); break;
    case CMD_FONTDLG: font_dialog(); break;
    case CMD_INDENT_LESS: indent_by(-360); break;
    case CMD_INDENT_MORE: indent_by(360); break;
    case CMD_LIST: set_list(get_pf().wNumbering ? 0 : 1); break;
    case CMD_LIST_MENU:
    {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, CMD_LIST_BASE + 0, L"&None");
        AppendMenuW(m, MF_STRING, CMD_LIST_BASE + 1, L"\x2022  &Bullet");
        AppendMenuW(m, MF_STRING, CMD_LIST_BASE + 2, L"1. 2. 3.  &Numbers");
        AppendMenuW(m, MF_STRING, CMD_LIST_BASE + 3, L"a. b. c.  &Lowercase letters");
        AppendMenuW(m, MF_STRING, CMD_LIST_BASE + 4, L"A. B. C.  &Uppercase letters");
        AppendMenuW(m, MF_STRING, CMD_LIST_BASE + 5, L"i. ii. iii.  Lowercase &Roman numerals");
        AppendMenuW(m, MF_STRING, CMD_LIST_BASE + 6, L"I. II. III.  Uppercase R&oman numerals");
        popup_at_ribbon(m, CMD_LIST);
        break;
    }
    case CMD_SPACING_MENU:
    {
        HMENU m = CreatePopupMenu();
        PARAFORMAT2 pf = get_pf();
        int cur = pf.bLineSpacingRule == 5 ? pf.dyLineSpacing : pf.bLineSpacingRule == 1 ? 30 : pf.bLineSpacingRule == 2 ? 40 : 20;
        AppendMenuW(m, MF_STRING | (cur == 20 ? MF_CHECKED : 0), CMD_SPACING_BASE + 0, L"1.0");
        AppendMenuW(m, MF_STRING | (cur == 23 ? MF_CHECKED : 0), CMD_SPACING_BASE + 1, L"1.15");
        AppendMenuW(m, MF_STRING | (cur == 30 ? MF_CHECKED : 0), CMD_SPACING_BASE + 2, L"1.5");
        AppendMenuW(m, MF_STRING | (cur == 40 ? MF_CHECKED : 0), CMD_SPACING_BASE + 3, L"2.0");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (pf.dySpaceAfter ? MF_CHECKED : 0), CMD_SPACING_BASE + 4, L"Add 10pt space after paragraphs");
        popup_at_ribbon(m, CMD_SPACING_MENU);
        break;
    }
    case CMD_ALIGN_LEFT: set_align(PFA_LEFT); break;
    case CMD_ALIGN_CENTER: set_align(PFA_CENTER); break;
    case CMD_ALIGN_RIGHT: set_align(PFA_RIGHT); break;
    case CMD_ALIGN_JUSTIFY: set_align(PFA_JUSTIFY); break;
    case CMD_PARADLG: dlg_paragraph(); break;
    case CMD_TABSDLG: dlg_tabs(); break;
    case CMD_PICTURE: insert_picture(); break;
    case CMD_DATETIME: dlg_datetime(); break;
    case CMD_TABLE: dlg_table(); break;
    case CMD_ZOOMIN: g_zoom = g_zoom < 100 ? g_zoom + 10 : g_zoom + 50; apply_zoom(); break;
    case CMD_ZOOMOUT: g_zoom = g_zoom <= 100 ? g_zoom - 10 : g_zoom - 50; apply_zoom(); break;
    case CMD_ZOOM100: g_zoom = 100; apply_zoom(); break;
    case CMD_RULER: set_ruler(!g_ruler_on); break;
    case CMD_STATUSBAR: g_statusbar_on = !g_statusbar_on; layout(); break;
    case CMD_WRAP_MENU:
    {
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING | (g_wrap == 0 ? MF_CHECKED : 0), CMD_WRAP_BASE + 0, L"&No wrap");
        AppendMenuW(m, MF_STRING | (g_wrap == 1 ? MF_CHECKED : 0), CMD_WRAP_BASE + 1, L"Wrap to &window");
        AppendMenuW(m, MF_STRING | (g_wrap == 2 ? MF_CHECKED : 0), CMD_WRAP_BASE + 2, L"Wrap to &ruler");
        popup_at_ribbon(m, CMD_WRAP_MENU);
        break;
    }
    case CMD_UNITS_MENU:
    {
        HMENU m = CreatePopupMenu();
        static const WCHAR *u[4] = { L"&Inches", L"&Centimeters", L"&Points", L"P&icas" };
        for (int i = 0; i < 4; i++) AppendMenuW(m, MF_STRING | (g_units == i ? MF_CHECKED : 0), CMD_UNITS_BASE + i, u[i]);
        popup_at_ribbon(m, CMD_UNITS_MENU);
        break;
    }
    case CMD_ESCAPE: break;
    default:
        if (cmd >= CMD_LIST_BASE && cmd < CMD_LIST_BASE + 7) set_list(cmd - CMD_LIST_BASE);
        else if (cmd >= CMD_SPACING_BASE && cmd < CMD_SPACING_BASE + 5) set_spacing(cmd - CMD_SPACING_BASE);
        else if (cmd >= CMD_WRAP_BASE && cmd < CMD_WRAP_BASE + 3) { g_wrap = cmd - CMD_WRAP_BASE; apply_wrap(); }
        else if (cmd >= CMD_UNITS_BASE && cmd < CMD_UNITS_BASE + 4) { g_units = cmd - CMD_UNITS_BASE; InvalidateRect(g_ruler, NULL, FALSE); }
        else if (cmd >= CMD_COLOR_BASE && cmd < CMD_COLOR_BASE + 100)
            set_color(cmd - CMD_COLOR_BASE == PAL_AUTO ? 0 : g_pal[cmd - CMD_COLOR_BASE], cmd - CMD_COLOR_BASE == PAL_AUTO);
        else if (cmd >= CMD_HL_BASE && cmd < CMD_HL_BASE + 100)
            set_highlight(cmd - CMD_HL_BASE == PAL_AUTO ? 0 : g_pal[cmd - CMD_HL_BASE], cmd - CMD_HL_BASE == PAL_AUTO);
        break;
    }
    if (g_edit && cmd != CMD_EXIT && GetFocus() != g_face && GetFocus() != g_size && GetForegroundWindow() == g_main)
        SetFocus(g_edit);
    InvalidateRect(g_ribbon, NULL, FALSE);
    write_dump();
}

/* ---------------------------------------------------------------- layout */

void layout(void)
{
    RECT rc;
    int y = 0, rh, sh, lh;
    if (!g_main) return;
    GetClientRect(g_main, &rc);
    ribbon_layout();
    rh = ribbon_height();
    lh = g_ruler_on ? ruler_height() : 0;
    sh = g_statusbar_on ? status_height() : 0;
    MoveWindow(g_ribbon, 0, y, rc.right, rh, TRUE); y += rh;
    ShowWindow(g_ruler, g_ruler_on ? SW_SHOWNA : SW_HIDE);
    if (lh) { MoveWindow(g_ruler, 0, y, rc.right, lh, TRUE); y += lh; }
    MoveWindow(g_edit, 0, y, rc.right, max(0, rc.bottom - y - sh), TRUE);
    ShowWindow(g_status, g_statusbar_on ? SW_SHOWNA : SW_HIDE);
    if (sh) MoveWindow(g_status, 0, rc.bottom - sh, rc.right, sh, TRUE);
    {
        /* the text starts a little in from the edge, where the ruler's 0 is */
        RECT er;
        GetClientRect(g_edit, &er);
        er.left += S(18); er.top += S(6);
        SendMessageW(g_edit, EM_SETRECT, 0, (LPARAM)&er);
    }
    InvalidateRect(g_ribbon, NULL, FALSE);
    if (g_ruler_on) InvalidateRect(g_ruler, NULL, FALSE);
    write_dump();
}

/* ---------------------------------------------------------------- windows */

static LRESULT CALLBACK edit_sub(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEWHEEL && (GET_KEYSTATE_WPARAM(w) & MK_CONTROL))
    {
        do_command(GET_WHEEL_DELTA_WPARAM(w) > 0 ? CMD_ZOOMIN : CMD_ZOOMOUT);
        return 0;
    }
    if (m == WM_HSCROLL || m == WM_VSCROLL || m == WM_SIZE)
    {
        LRESULT r = CallWindowProcW(g_edit_proc, h, m, w, l);
        if (g_ruler_on) InvalidateRect(g_ruler, NULL, FALSE);
        return r;
    }
    return CallWindowProcW(g_edit_proc, h, m, w, l);
}

static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == g_findmsg && g_findmsg) return on_find_msg(l);
    switch (m)
    {
    case WM_CREATE:
        return 0;
    case WM_SIZE:
        layout();
        return 0;
    case WM_SETFOCUS:
        if (g_edit) SetFocus(g_edit);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE && g_edit) SetFocus(g_edit);
        return 0;
    case WM_COMMAND:
        if ((HWND)l == g_face || (HWND)l == g_size)
        {
            extern void combo_notify(HWND combo, int code);
            combo_notify((HWND)l, HIWORD(w));
            return 0;
        }
        if ((HWND)l == g_edit)
        {
            if (HIWORD(w) == EN_CHANGE) { static int busy; if (!busy) { busy = 1; write_dump(); busy = 0; } }
            return 0;
        }
        do_command(LOWORD(w));
        return 0;
    case WM_NOTIFY:
    {
        NMHDR *nm = (NMHDR *)l;
        if (nm->hwndFrom == g_edit && nm->code == EN_SELCHANGE) refresh_state();
        return 0;
    }
    case WM_DROPFILES:
    {
        WCHAR f[MAX_PATH];
        if (DragQueryFileW((HDROP)w, 0, f, MAX_PATH) && may_discard()) open_file(f);
        DragFinish((HDROP)w);
        return 0;
    }
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)l)->ptMinTrackSize.x = S(400);
        ((MINMAXINFO *)l)->ptMinTrackSize.y = S(300);
        return 0;
    case WM_CLOSE:
        if (!may_discard()) return 0;
        save_settings();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int CALLBACK has_face_cb(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM l)
{
    (void)lf; (void)tm; (void)type;
    *(int *)l = 1;
    return 0;
}

static BOOL has_face(const WCHAR *face)
{
    LOGFONTW lf;
    int found = 0;
    HDC dc = GetDC(NULL);
    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, face, LF_FACESIZE);
    EnumFontFamiliesExW(dc, &lf, has_face_cb, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    return found;
}

/* Windows' WordPad starts in Calibri 11 (Consolas for text files); the first
 * installed of these, then Wine's substitutes */
const WCHAR *default_face(BOOL mono)
{
    static const WCHAR *prop[] = { L"Calibri", L"Carlito", L"Segoe UI", L"Inter", L"Noto Sans", L"DejaVu Sans", L"Tahoma" };
    static const WCHAR *fixed[] = { L"Consolas", L"Cascadia Mono", L"DejaVu Sans Mono", L"Liberation Mono", L"Courier New" };
    static const WCHAR *cache[2];
    const WCHAR **list = mono ? fixed : prop;
    int n = mono ? ARRAYSIZE(fixed) : ARRAYSIZE(prop);
    if (cache[mono]) return cache[mono];
    for (int i = 0; i < n; i++) if (has_face(list[i])) return cache[mono] = list[i];
    return cache[mono] = mono ? L"Courier New" : L"Tahoma";
}

/* the command line: [/p | /pt] [file] [printer ...]; an unquoted path with
 * spaces is taken whole when it names a file */
static void parse_cmdline(WCHAR *file, BOOL *print, WCHAR *printer)
{
    int argc, i = 1;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    file[0] = 0; *print = FALSE; printer[0] = 0;
    if (!argv) return;
    if (i < argc && (!lstrcmpiW(argv[i], L"/p") || !lstrcmpiW(argv[i], L"-p"))) { *print = TRUE; i++; }
    else if (i < argc && (!lstrcmpiW(argv[i], L"/pt") || !lstrcmpiW(argv[i], L"-pt"))) { *print = TRUE; i++; }
    if (i < argc)
    {
        lstrcpynW(file, argv[i], MAX_PATH);
        if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES && i + 1 < argc)
        {
            /* an unquoted path with spaces */
            WCHAR joined[MAX_PATH] = L"";
            for (int j = i; j < argc; j++)
            {
                if (j > i) wcsncat(joined, L" ", MAX_PATH - wcslen(joined) - 1);
                wcsncat(joined, argv[j], MAX_PATH - wcslen(joined) - 1);
                if (GetFileAttributesW(joined) != INVALID_FILE_ATTRIBUTES) { lstrcpynW(file, joined, MAX_PATH); i = j; break; }
            }
        }
        i++;
        if (i < argc) lstrcpynW(printer, argv[i], MAX_PATH);
    }
    LocalFree(argv);
}

extern void print_file_silently(const WCHAR *printer);

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    WNDCLASSEXW wc;
    MSG msg;
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES };
    WCHAR file[MAX_PATH], printer[MAX_PATH];
    BOOL print;
    HDC dc;
    RECT fr;
    DWORD sz = sizeof(fr);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = CW_USEDEFAULT, h = CW_USEDEFAULT;
    (void)prev; (void)cmd;

    g_inst = inst;
    InitCommonControlsEx(&icc);
    OleInitialize(NULL);
    if (!LoadLibraryW(L"msftedit.dll")) LoadLibraryW(L"riched20.dll");
    if (!GetEnvironmentVariableW(L"SG_WORDPAD_DUMP", g_dump, MAX_PATH)) g_dump[0] = 0;
    g_findmsg = RegisterWindowMessageW(FINDMSGSTRINGW);
    dc = GetDC(NULL); g_dpi = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(NULL, dc);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = -S(12);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -S(11);
    g_font_small = CreateFontIndirectW(&ncm.lfMessageFont);
    load_settings();
    parse_cmdline(file, &print, printer);

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = main_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_WORDPAD));
    wc.hIconSm = LoadImageW(inst, MAKEINTRESOURCEW(IDI_WORDPAD), IMAGE_ICON, 16, 16, 0);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"WordPadClass";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = ribbon_proc; wc.lpszClassName = L"SgWordPadRibbon"; wc.hIcon = wc.hIconSm = NULL;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = ruler_proc; wc.lpszClassName = L"SgWordPadRuler";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = status_proc; wc.lpszClassName = L"SgWordPadStatus";
    RegisterClassExW(&wc);

    if (!RegGetValueW(HKEY_CURRENT_USER, OPTS, L"FrameRect", RRF_RT_REG_BINARY, NULL, &fr, &sz) && sz == sizeof(fr)
        && fr.right - fr.left >= 400 && fr.bottom - fr.top >= 300)
    { x = fr.left; y = fr.top; w = fr.right - fr.left; h = fr.bottom - fr.top; }
    else { w = S(900); h = S(640); }

    g_main = CreateWindowExW(WS_EX_ACCEPTFILES, L"WordPadClass", L"Document - WordPad", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             x, y, w, h, NULL, NULL, inst, NULL);
    g_ribbon = CreateWindowExW(0, L"SgWordPadRibbon", NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, g_main, NULL, inst, NULL);
    g_ruler = CreateWindowExW(0, L"SgWordPadRuler", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g_main, NULL, inst, NULL);
    g_status = CreateWindowExW(0, L"SgWordPadStatus", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g_main, NULL, inst, NULL);
    g_edit = CreateWindowExW(0, MSFTEDIT_CLASS, NULL,
                             WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_NOHIDESEL |
                             ES_WANTRETURN | ES_SELECTIONBAR, 0, 0, 0, 0, g_main, (HMENU)1, inst, NULL);
    if (!g_edit)
        g_edit = CreateWindowExW(0, RICHEDIT_CLASSW, NULL,
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_NOHIDESEL |
                                 ES_WANTRETURN | ES_SELECTIONBAR, 0, 0, 0, 0, g_main, (HMENU)1, inst, NULL);
    SendMessageW(g_edit, EM_SETEVENTMASK, 0, ENM_SELCHANGE | ENM_CHANGE);
    SendMessageW(g_edit, EM_SETTEXTMODE, TM_RICHTEXT | TM_MULTILEVELUNDO | TM_MULTICODEPAGE, 0);
    SendMessageW(g_edit, EM_EXLIMITTEXT, 0, 0x7FFFFFF0);
    SendMessageW(g_edit, EM_SETUNDOLIMIT, 1000, 0);
    SendMessageW(g_edit, EM_AUTOURLDETECT, FALSE, 0);
    g_edit_proc = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)edit_sub);
    {
        extern void ribbon_create_controls(void);
        ribbon_create_controls();
    }
    new_document();
    apply_wrap();
    layout();

    if (print && file[0])
    {
        if (open_file(file)) print_file_silently(printer[0] ? printer : NULL);
        return 0;
    }
    ShowWindow(g_main, opt_get(L"Maximized", 0) && show == SW_SHOWNORMAL ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(g_main);
    if (file[0])
    {
        WCHAR full[MAX_PATH];
        /* "document" opens document.rtf as Windows' WordPad does */
        if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES && !wcschr(file, '.'))
        {
            swprintf(full, MAX_PATH, L"%ls.rtf", file);
            if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) lstrcpynW(file, full, MAX_PATH);
        }
        open_file(file);
    }
    SetFocus(g_edit);
    write_dump();

    {
        static const ACCEL acc[] = {
            { FCONTROL | FVIRTKEY, 'N', CMD_NEW }, { FCONTROL | FVIRTKEY, 'O', CMD_OPEN }, { FCONTROL | FVIRTKEY, 'S', CMD_SAVE },
            { FVIRTKEY, VK_F12, CMD_SAVEAS }, { FCONTROL | FVIRTKEY, 'P', CMD_PRINT },
            { FCONTROL | FVIRTKEY, 'B', CMD_BOLD }, { FCONTROL | FVIRTKEY, 'I', CMD_ITALIC }, { FCONTROL | FVIRTKEY, 'U', CMD_UNDERLINE },
            { FCONTROL | FVIRTKEY, 'E', CMD_ALIGN_CENTER }, { FCONTROL | FVIRTKEY, 'L', CMD_ALIGN_LEFT },
            { FCONTROL | FVIRTKEY, 'R', CMD_ALIGN_RIGHT }, { FCONTROL | FVIRTKEY, 'J', CMD_ALIGN_JUSTIFY },
            { FCONTROL | FSHIFT | FVIRTKEY, VK_OEM_PERIOD, CMD_GROW }, { FCONTROL | FSHIFT | FVIRTKEY, VK_OEM_COMMA, CMD_SHRINK },
            { FCONTROL | FVIRTKEY, VK_OEM_PLUS, CMD_SUBSCRIPT }, { FCONTROL | FSHIFT | FVIRTKEY, VK_OEM_PLUS, CMD_SUPERSCRIPT },
            { FCONTROL | FVIRTKEY, 'F', CMD_FIND }, { FCONTROL | FVIRTKEY, 'H', CMD_REPLACE }, { FVIRTKEY, VK_F3, CMD_FINDNEXT },
            { FCONTROL | FVIRTKEY, 'A', CMD_SELECTALL }, { FCONTROL | FSHIFT | FVIRTKEY, 'L', CMD_LIST },
            { FCONTROL | FVIRTKEY, '1', CMD_SPACING_BASE + 0 }, { FCONTROL | FVIRTKEY, '2', CMD_SPACING_BASE + 3 },
            { FCONTROL | FVIRTKEY, '5', CMD_SPACING_BASE + 2 },
            { FCONTROL | FVIRTKEY, 'Y', CMD_REDO },
        };
        g_accel = CreateAcceleratorTableW((ACCEL *)acc, ARRAYSIZE(acc));
    }
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        if (g_finddlg && IsDialogMessageW(g_finddlg, &msg)) continue;
        if (GetAncestor(msg.hwnd, GA_ROOT) == g_main && GetParent(GetParent(msg.hwnd)) != g_ribbon &&
            TranslateAcceleratorW(g_main, g_accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    OleUninitialize();
    return (int)msg.wParam;
}
