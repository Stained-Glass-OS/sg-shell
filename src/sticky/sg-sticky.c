/* sg-sticky -- Sticky Notes for Stained Glass OS.
 *
 * Each note is a small borderless window: a strip along the top in a darker
 * shade of the note's colour (+ new note, ... menu, x close), a rich text body
 * (RichEdit: Ctrl+B/I/U, Ctrl+T strikethrough, Ctrl+Shift+L bullets) and a
 * formatting bar. The ... menu holds the colours, "Notes list" and "Delete
 * note". One process owns every note: a second start hands its command line to
 * the first (WM_COPYDATA to the message-only window SgStickyHost) and exits.
 *
 * Notes save themselves (debounced) to %LOCALAPPDATA%\Stained Glass\Sticky
 * Notes\<id>.note -- a short header (colour, position, open) and the text as
 * RTF -- and come back at the next start.
 *
 *   sg-sticky            the open notes (a new one if there are none)
 *   sg-sticky /new       a new note
 *   sg-sticky /list      the notes list
 *   sg-sticky /quit      save everything and exit (tests, sign-out)
 *
 * SG_STICKY_DUMP=<file>: the notes, their buttons and the list, rewritten
 * twice a second, for the gate (test/sticky-check.sh).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <windowsx.h>
#include <richedit.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define HOST_CLASS  L"SgStickyHost"
#define NOTE_CLASS  L"SgStickyNote"
#define MENU_CLASS  L"SgStickyMenu"
#define LIST_CLASS  L"SgStickyList"
#define MUTEX_NAME  L"Local\\StainedGlassStickyNotes"
#define APP_TITLE   L"Sticky Notes"
#define MAX_NOTES   512

static const struct { const WCHAR *name, *label; COLORREF body, strip, text, button; } COLORS[] = {
    { L"yellow",   L"Yellow",   RGB(0xFF, 0xF8, 0xC9), RGB(0xFA, 0xEB, 0x96), RGB(0x20, 0x20, 0x20), RGB(0xEF, 0xDA, 0x6E) },
    { L"green",    L"Green",    RGB(0xE3, 0xF6, 0xDD), RGB(0xC6, 0xEB, 0xBC), RGB(0x20, 0x20, 0x20), RGB(0xA9, 0xDB, 0x9C) },
    { L"pink",     L"Pink",     RGB(0xFF, 0xE3, 0xF0), RGB(0xFA, 0xC8, 0xDF), RGB(0x20, 0x20, 0x20), RGB(0xF0, 0xA9, 0xCA) },
    { L"purple",   L"Purple",   RGB(0xEF, 0xE4, 0xFB), RGB(0xDD, 0xCA, 0xF6), RGB(0x20, 0x20, 0x20), RGB(0xC7, 0xA8, 0xEE) },
    { L"blue",     L"Blue",     RGB(0xE0, 0xEF, 0xFC), RGB(0xC6, 0xE1, 0xF8), RGB(0x20, 0x20, 0x20), RGB(0xA3, 0xCC, 0xF0) },
    { L"grey",     L"Grey",     RGB(0xF2, 0xF1, 0xF0), RGB(0xE0, 0xDE, 0xDC), RGB(0x20, 0x20, 0x20), RGB(0xC9, 0xC6, 0xC3) },
    { L"charcoal", L"Charcoal", RGB(0x5B, 0x5B, 0x5E), RGB(0x46, 0x46, 0x49), RGB(0xF4, 0xF4, 0xF4), RGB(0x6C, 0x6C, 0x70) },
};
#define NCOLORS ((int)(sizeof(COLORS) / sizeof(COLORS[0])))
static const COLORREF ACCENT = RGB(112, 48, 192);

enum { BTN_NONE, BTN_NEW, BTN_MORE, BTN_CLOSE, BTN_BOLD, BTN_ITALIC, BTN_UNDER, BTN_STRIKE, BTN_BULLET, BTN_COUNT };
enum { MI_LIST = 100, MI_DELETE };

typedef struct note {
    WCHAR id[64];
    int color;
    RECT rc;                     /* on screen */
    BOOL open;
    FILETIME modified;
    char *rtf;                   /* loaded, until the window takes it */
    HWND hwnd, edit, menu;
    int hot, pressed;
    BOOL tracking;
} note;

static note *g_notes[MAX_NOTES];
static int g_n;
static HWND g_host, g_list, g_listbox, g_search, g_listnew;
static HFONT g_font_body, g_font_ui, g_font_ui_bold, g_font_title, g_font_glyph, g_font_small;
static int g_dpi = 96;
static WCHAR g_dir[MAX_PATH], g_dump[MAX_PATH];
static HINSTANCE g_inst;
static BOOL g_loading, g_quitting;
static WNDPROC g_edit_proc;

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HFONT make_font(int pt10, int weight, BOOL italic, BOOL under, BOOL strike, const WCHAR *face)
{
    return CreateFontW(-MulDiv(pt10, g_dpi, 720), 0, 0, 0, weight, italic, under, strike, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, face ? face : L"Segoe UI");
}

/* ---- storage --------------------------------------------------------------------------------- */

static void notes_dir(void)
{
    WCHAR base[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, base))) GetTempPathW(MAX_PATH, base);
    _snwprintf(g_dir, MAX_PATH, L"%ls\\Stained Glass\\Sticky Notes", base);
    g_dir[MAX_PATH - 1] = 0;
    SHCreateDirectoryExW(NULL, g_dir, NULL);
}

static void note_path(const note *n, WCHAR *out)
{
    _snwprintf(out, MAX_PATH, L"%ls\\%ls.note", g_dir, n->id);
    out[MAX_PATH - 1] = 0;
}

typedef struct { char *buf; size_t len, cap; } membuf;

static DWORD CALLBACK stream_out(DWORD_PTR cookie, BYTE *data, LONG cb, LONG *done)
{
    membuf *m = (membuf *)cookie;
    if (m->len + cb + 1 > m->cap)
    {
        size_t cap = (m->len + cb + 1) * 2;
        char *nb = realloc(m->buf, cap);
        if (!nb) return 1;
        m->buf = nb; m->cap = cap;
    }
    memcpy(m->buf + m->len, data, cb);
    m->len += cb;
    m->buf[m->len] = 0;
    *done = cb;
    return 0;
}

typedef struct { const char *p; size_t left; } instream;

static DWORD CALLBACK stream_in(DWORD_PTR cookie, BYTE *data, LONG cb, LONG *done)
{
    instream *s = (instream *)cookie;
    LONG n = (LONG)min((size_t)cb, s->left);
    memcpy(data, s->p, n);
    s->p += n; s->left -= n;
    *done = n;
    return 0;
}

static char *note_rtf(const note *n)
{
    membuf m = { 0 };
    EDITSTREAM es = { (DWORD_PTR)&m, 0, stream_out };
    if (!n->edit) return n->rtf ? _strdup(n->rtf) : _strdup("");
    SendMessageW(n->edit, EM_STREAMOUT, SF_RTF, (LPARAM)&es);
    return m.buf ? m.buf : _strdup("");
}

static void note_text(const note *n, WCHAR *out, int cch)
{
    GETTEXTEX gt = { (DWORD)(cch * sizeof(WCHAR)), GT_DEFAULT, 1200, NULL, NULL };
    out[0] = 0;
    if (n->edit) SendMessageW(n->edit, EM_GETTEXTEX, (WPARAM)&gt, (LPARAM)out);
    out[cch - 1] = 0;
}

static void save_note(note *n)
{
    WCHAR path[MAX_PATH], tmp[MAX_PATH + 8];
    char head[256];
    char *rtf;
    HANDLE f;
    DWORD w;
    int len;

    if (n->hwnd && !IsIconic(n->hwnd)) GetWindowRect(n->hwnd, &n->rc);
    note_path(n, path);
    _snwprintf(tmp, MAX_PATH + 8, L"%ls.tmp", path);
    rtf = note_rtf(n);
    len = _snprintf(head, sizeof(head), "StickyNote 1\r\nColor=%ls\r\nRect=%ld,%ld,%ld,%ld\r\nOpen=%d\r\n\r\n",
                    COLORS[n->color].name, n->rc.left, n->rc.top, n->rc.right - n->rc.left,
                    n->rc.bottom - n->rc.top, n->open ? 1 : 0);
    f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE)
    {
        WriteFile(f, head, len, &w, NULL);
        WriteFile(f, rtf, (DWORD)strlen(rtf), &w, NULL);
        CloseHandle(f);
        MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
    }
    free(rtf);
    GetSystemTimeAsFileTime(&n->modified);
}

static note *load_note(const WCHAR *path, const WCHAR *id)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got;
    char *buf, *p, *body;
    note *n;
    int x, y, w, h, i;

    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size > 64 * 1024 * 1024 || !(buf = malloc(size + 1))) { CloseHandle(f); return NULL; }
    if (!ReadFile(f, buf, size, &got, NULL)) got = 0;
    buf[got] = 0;
    n = calloc(1, sizeof(*n));
    GetFileTime(f, NULL, NULL, &n->modified);
    CloseHandle(f);
    if (strncmp(buf, "StickyNote 1", 12)) { free(buf); free(n); return NULL; }
    lstrcpynW(n->id, id, 64);
    n->open = TRUE;
    SetRect(&n->rc, 100, 100, 100 + S(300), 100 + S(300));
    body = strstr(buf, "\r\n\r\n");
    if (body) { *body = 0; body += 4; } else body = buf + got;
    for (p = buf; p && *p; p = strstr(p, "\r\n") ? strstr(p, "\r\n") + 2 : NULL)
    {
#ifndef SG_MUTANT_NO_COLOR
        if (!strncmp(p, "Color=", 6))
            for (i = 0; i < NCOLORS; i++)
            {
                char name[32];
                _snprintf(name, sizeof(name), "%ls", COLORS[i].name);
                if (!strncmp(p + 6, name, strlen(name)) && (p[6 + strlen(name)] == '\r' || !p[6 + strlen(name)])) n->color = i;
            }
#endif
        if (!strncmp(p, "Rect=", 5) && sscanf(p + 5, "%d,%d,%d,%d", &x, &y, &w, &h) == 4 && w > 0 && h > 0)
            SetRect(&n->rc, x, y, x + w, y + h);
        if (!strncmp(p, "Open=", 5)) n->open = p[5] != '0';
    }
    n->rtf = _strdup(body);
    free(buf);
    return n;
}

static void load_notes(void)
{
    WCHAR pat[MAX_PATH], path[MAX_PATH], id[64], *dot;
    WIN32_FIND_DATAW fd;
    HANDLE h;

    _snwprintf(pat, MAX_PATH, L"%ls\\*.note", g_dir);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do
    {
        note *n;
        if (g_n >= MAX_NOTES) break;
        lstrcpynW(id, fd.cFileName, 64);
        if ((dot = wcsrchr(id, '.'))) *dot = 0;
        _snwprintf(path, MAX_PATH, L"%ls\\%ls", g_dir, fd.cFileName);
        if ((n = load_note(path, id))) g_notes[g_n++] = n;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* ---- the dump, for the gate ---------------------------------------------------------------------- */

static void dump_pt(FILE *f, const char *name, HWND hwnd, int x, int y)
{
    POINT pt = { x, y };
    ClientToScreen(hwnd, &pt);
    fprintf(f, " %s=%ld,%ld", name, pt.x, pt.y);
}

static void button_rect(const note *n, int b, RECT *r);

static void write_dump(void)
{
    WCHAR tmp[MAX_PATH + 8], text[4096];
    FILE *f;
    int i, j, files = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (!g_dump[0]) return;
    _snwprintf(tmp, MAX_PATH + 8, L"%ls.tmp", g_dump);
    if (!(f = _wfopen(tmp, L"wb"))) return;
    for (i = 0; i < g_n; i++)
    {
        note *n = g_notes[i];
        RECT rc;
        char *u;
        int len;
        GetWindowRect(n->hwnd, &rc);
        note_text(n, text, 4096);
        for (j = 0; text[j]; j++) if (text[j] == '\r' || text[j] == '\n') text[j] = '|';
        len = WideCharToMultiByte(CP_UTF8, 0, text, -1, NULL, 0, NULL, NULL);
        u = malloc(len);
        WideCharToMultiByte(CP_UTF8, 0, text, -1, u, len, NULL, NULL);
        fprintf(f, "NOTE %d id=%ls color=%ls visible=%d rect=%ld,%ld,%ld,%ld active=%d text=%s\n", i, n->id,
                COLORS[n->color].name, IsWindowVisible(n->hwnd) ? 1 : 0, rc.left, rc.top, rc.right, rc.bottom,
                GetForegroundWindow() == n->hwnd ? 1 : 0, u);
        free(u);
        fprintf(f, "BUTTONS %d", i);
        {
            static const char *names[] = { "", "new", "more", "close", "bold", "italic", "under", "strike", "bullet" };
            for (j = BTN_NEW; j < BTN_COUNT; j++)
            {
                RECT b;
                button_rect(n, j, &b);
                dump_pt(f, names[j], n->hwnd, (b.left + b.right) / 2, (b.top + b.bottom) / 2);
            }
        }
        fprintf(f, "\n");
        if (n->menu && IsWindowVisible(n->menu))
        {
            RECT mc;
            int sw;
            GetClientRect(n->menu, &mc);
            sw = mc.right / NCOLORS;
            fprintf(f, "MENU %d", i);
            for (j = 0; j < NCOLORS; j++)
            {
                char nm[16];
                _snprintf(nm, sizeof(nm), "%ls", COLORS[j].name);
                dump_pt(f, nm, n->menu, j * sw + sw / 2, S(22));
            }
            dump_pt(f, "list", n->menu, mc.right / 2, S(44) + S(20));
            dump_pt(f, "delete", n->menu, mc.right / 2, S(44) + S(40) + S(20));
            fprintf(f, "\n");
        }
    }
    if (g_list)
    {
        WCHAR q[128] = L"";
        RECT rc;
        GetWindowTextW(g_search, q, 128);
        GetWindowRect(g_list, &rc);
        fprintf(f, "LIST visible=%d items=%d rect=%ld,%ld,%ld,%ld search=%ls", IsWindowVisible(g_list) ? 1 : 0,
                (int)SendMessageW(g_listbox, LB_GETCOUNT, 0, 0), rc.left, rc.top, rc.right, rc.bottom, q);
        GetWindowRect(g_search, &rc);
        fprintf(f, " searchbox=%ld,%ld", (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
        GetWindowRect(g_listbox, &rc);
        fprintf(f, " first=%ld,%ld", (rc.left + rc.right) / 2, rc.top + S(42));
        fprintf(f, "\n");
    }
    else fprintf(f, "LIST visible=0 items=0\n");
    {
        WCHAR pat[MAX_PATH];
        _snwprintf(pat, MAX_PATH, L"%ls\\*.note", g_dir);
        if ((h = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE)
        {
            do files++; while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    fprintf(f, "FILES %d dir=%ls\nPID %lu\nEND\n", files, g_dir, GetCurrentProcessId());
    fclose(f);
    MoveFileExW(tmp, g_dump, MOVEFILE_REPLACE_EXISTING);
}

/* ---- layout and drawing ------------------------------------------------------------------------- */

static int strip_h(void) { return S(32); }
static int bar_h(void) { return S(36); }

static void button_rect(const note *n, int b, RECT *r)
{
    RECT c;
    int sh = strip_h(), bh = bar_h(), w;
    GetClientRect(n->hwnd, &c);
    w = c.right;
    SetRectEmpty(r);
    switch (b)
    {
    case BTN_NEW:   SetRect(r, 0, 0, S(40), sh); break;
    case BTN_MORE:  SetRect(r, w - S(80), 0, w - S(40), sh); break;
    case BTN_CLOSE: SetRect(r, w - S(40), 0, w, sh); break;
    default:
        if (b >= BTN_BOLD && b <= BTN_BULLET)
        {
            int x = S(6) + (b - BTN_BOLD) * S(34);
            SetRect(r, x, c.bottom - bh + S(3), x + S(32), c.bottom - S(3));
        }
    }
}

static int button_at(const note *n, POINT pt)
{
    int b;
    for (b = BTN_NEW; b < BTN_COUNT; b++)
    {
        RECT r;
        button_rect(n, b, &r);
        if (PtInRect(&r, pt)) return b;
    }
    return BTN_NONE;
}

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, r, b);
    DeleteObject(b);
}

static void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF c, int w)
{
    HPEN p = CreatePen(PS_SOLID, w, c), old = SelectObject(dc, p);
    MoveToEx(dc, x1, y1, NULL);
    LineTo(dc, x2, y2);
    DeleteObject(SelectObject(dc, old));
}

static void dot(HDC dc, int x, int y, int r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c), ob = SelectObject(dc, b);
    HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, x - r, y - r, x + r + 1, y + r + 1);
    SelectObject(dc, op);
    DeleteObject(SelectObject(dc, ob));
}

/* the current selection's effects, for the bar's pressed look */
static DWORD sel_effects(const note *n, BOOL *bullets)
{
    CHARFORMAT2W cf = { 0 };
    PARAFORMAT2 pf = { 0 };
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_BOLD | CFM_ITALIC | CFM_UNDERLINE | CFM_STRIKEOUT;
    SendMessageW(n->edit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_NUMBERING;
    SendMessageW(n->edit, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
    *bullets = pf.wNumbering == PFN_BULLET;
    return cf.dwEffects & cf.dwMask;
}

static void paint_note(note *n, HDC dc)
{
    RECT c, r;
    int b, sh = strip_h(), bh = bar_h();
    COLORREF body = COLORS[n->color].body, strip = COLORS[n->color].strip, ink = COLORS[n->color].text;
    BOOL bullets = FALSE;
    DWORD fx = n->edit ? sel_effects(n, &bullets) : 0;
    HFONT old;

    GetClientRect(n->hwnd, &c);
    SetRect(&r, 0, 0, c.right, sh);
    fill(dc, &r, strip);
    SetRect(&r, 0, sh, c.right, c.bottom);
    fill(dc, &r, body);
    SetBkMode(dc, TRANSPARENT);
    for (b = BTN_NEW; b < BTN_COUNT; b++)
    {
        int cx, cy;
        BOOL on = (b == BTN_BOLD && (fx & CFE_BOLD)) || (b == BTN_ITALIC && (fx & CFE_ITALIC)) ||
                  (b == BTN_UNDER && (fx & CFE_UNDERLINE)) || (b == BTN_STRIKE && (fx & CFE_STRIKEOUT)) ||
                  (b == BTN_BULLET && bullets);
        button_rect(n, b, &r);
        if (n->hot == b || on) fill(dc, &r, n->pressed == b ? COLORS[n->color].button : (on ? COLORS[n->color].button : strip));
        if (b == BTN_CLOSE && n->hot == b) fill(dc, &r, RGB(0xC4, 0x2B, 0x1C));
        cx = (r.left + r.right) / 2; cy = (r.top + r.bottom) / 2;
        switch (b)
        {
        case BTN_NEW:
            line(dc, cx - S(6), cy, cx + S(7), cy, ink, max(1, S(1)));
            line(dc, cx, cy - S(6), cx, cy + S(7), ink, max(1, S(1)));
            break;
        case BTN_MORE:
            dot(dc, cx - S(6), cy, max(1, S(1)), ink); dot(dc, cx, cy, max(1, S(1)), ink); dot(dc, cx + S(6), cy, max(1, S(1)), ink);
            break;
        case BTN_CLOSE:
        {
            COLORREF k = n->hot == b ? RGB(255, 255, 255) : ink;
            line(dc, cx - S(5), cy - S(5), cx + S(6), cy + S(6), k, max(1, S(1)));
            line(dc, cx + S(5), cy - S(5), cx - S(6), cy + S(6), k, max(1, S(1)));
            break;
        }
        case BTN_BULLET:
        {
            int k;
            for (k = -1; k <= 1; k++)
            {
                dot(dc, cx - S(7), cy + k * S(5), max(1, S(1)), ink);
                line(dc, cx - S(3), cy + k * S(5), cx + S(8), cy + k * S(5), ink, 1);
            }
            break;
        }
        case BTN_BOLD: case BTN_ITALIC: case BTN_UNDER: case BTN_STRIKE:
        {
            HFONT f = make_font(110, b == BTN_BOLD ? FW_BOLD : FW_NORMAL, b == BTN_ITALIC, b == BTN_UNDER, b == BTN_STRIKE,
                                b == BTN_ITALIC ? L"Times New Roman" : NULL);
            static const WCHAR *glyph[] = { L"B", L"I", L"U", L"ab" };
            old = SelectObject(dc, f);
            SetTextColor(dc, ink);
            DrawTextW(dc, glyph[b - BTN_BOLD], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DeleteObject(SelectObject(dc, old));
            break;
        }
        }
    }
    /* a hairline above the bar, and a frame: the note has no border of its own */
    line(dc, S(8), c.bottom - bh, c.right - S(8), c.bottom - bh, COLORS[n->color].button, 1);
    {
        HBRUSH fb = CreateSolidBrush(COLORS[n->color].button);
        FrameRect(dc, &c, fb);
        DeleteObject(fb);
    }
}

/* ---- notes ------------------------------------------------------------------------------------ */

static void refresh_list(void);
static void show_list(void);
static void maybe_exit(void);
static note *new_note(note *beside);

static note *note_of(HWND hwnd) { return (note *)GetWindowLongPtrW(hwnd, GWLP_USERDATA); }

static void apply_colour(note *n)
{
    CHARFORMAT2W cf = { 0 };
    SendMessageW(n->edit, EM_SETBKGNDCOLOR, 0, COLORS[n->color].body);
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    if (n->color == 6) cf.crTextColor = COLORS[n->color].text;
    else cf.dwEffects = CFE_AUTOCOLOR;
    SendMessageW(n->edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    InvalidateRect(n->hwnd, NULL, TRUE);
    if (n->menu) InvalidateRect(n->menu, NULL, TRUE);
}

static void touch(note *n)
{
    if (g_loading) return;
    SetTimer(n->hwnd, 1, 600, NULL);   /* the debounced save */
}

static void toggle_effect(note *n, DWORD effect)
{
    CHARFORMAT2W cf = { 0 };
    cf.cbSize = sizeof(cf);
    cf.dwMask = effect == CFE_BOLD ? CFM_BOLD : effect == CFE_ITALIC ? CFM_ITALIC :
                effect == CFE_UNDERLINE ? CFM_UNDERLINE : CFM_STRIKEOUT;
    DWORD mask = cf.dwMask;
    SendMessageW(n->edit, EM_GETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    /* the query widens dwMask to every attribute the selection shares; set back
     * with that mask and the automatic background goes, painting it black */
#ifndef SG_MUTANT_WIDE_MASK
    cf.dwMask = mask;
#endif
    cf.dwEffects = (cf.dwEffects & effect) ? 0 : effect;
    SendMessageW(n->edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    InvalidateRect(n->hwnd, NULL, FALSE);
    touch(n);
}

static void toggle_bullets(note *n)
{
    PARAFORMAT2 pf = { 0 };
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_NUMBERING;
    SendMessageW(n->edit, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
    pf.dwMask = PFM_NUMBERING | PFM_OFFSET;
    pf.wNumbering = pf.wNumbering == PFN_BULLET ? 0 : PFN_BULLET;
    pf.dxOffset = pf.wNumbering ? 300 : 0;
    SendMessageW(n->edit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
    InvalidateRect(n->hwnd, NULL, FALSE);
    touch(n);
}

static void close_menu(note *n)
{
    if (n->menu) { DestroyWindow(n->menu); n->menu = NULL; }
}

static void open_menu(note *n)
{
    RECT c;
    if (n->menu) { close_menu(n); return; }
    GetClientRect(n->hwnd, &c);
    n->menu = CreateWindowExW(0, MENU_CLASS, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, c.right,
                              S(44) + 2 * S(40) + S(6), n->hwnd, NULL, g_inst, n);
    SetWindowPos(n->menu, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
}

static void remove_note(note *n)
{
    int i;
    for (i = 0; i < g_n; i++)
        if (g_notes[i] == n)
        {
            memmove(g_notes + i, g_notes + i + 1, (g_n - i - 1) * sizeof(*g_notes));
            g_n--;
            break;
        }
    free(n->rtf);
    free(n);
}

static void delete_note(note *n, HWND owner)
{
    WCHAR path[MAX_PATH];
    if (MessageBoxW(owner, L"Delete this note?\n\nIt will be gone for good.", APP_TITLE,
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON1) != IDYES) return;
    KillTimer(n->hwnd, 1);
    note_path(n, path);
    DeleteFileW(path);
    SetWindowLongPtrW(n->hwnd, GWLP_USERDATA, 0);
    DestroyWindow(n->hwnd);
    remove_note(n);
    refresh_list();
    maybe_exit();
}

static void show_note(note *n)
{
    n->open = TRUE;
    ShowWindow(n->hwnd, IsIconic(n->hwnd) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(n->hwnd);
    SetFocus(n->edit);
    save_note(n);
}

static LRESULT CALLBACK edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    note *n = note_of(GetParent(hwnd));
    if (n && msg == WM_KEYDOWN && GetKeyState(VK_CONTROL) < 0)
    {
        BOOL shift = GetKeyState(VK_SHIFT) < 0;
        switch (wp)
        {
        case 'B': toggle_effect(n, CFE_BOLD); return 0;
        case 'I': toggle_effect(n, CFE_ITALIC); return 0;
        case 'U': toggle_effect(n, CFE_UNDERLINE); return 0;
        case 'T': toggle_effect(n, CFE_STRIKEOUT); return 0;
        case 'L': if (shift) { toggle_bullets(n); return 0; } break;
        case 'N': new_note(n); return 0;
        case 'D': delete_note(n, n->hwnd); return 0;
        case 'W': PostMessageW(n->hwnd, WM_CLOSE, 0, 0); return 0;
        }
    }
    if (n && msg == WM_CHAR && GetKeyState(VK_CONTROL) < 0 && wp < 32) return 0;   /* no stray control characters */
    if (n && msg == WM_KEYDOWN && wp == VK_ESCAPE && n->menu) { close_menu(n); return 0; }
    if (n && msg == WM_LBUTTONDOWN) close_menu(n);
    if (n && (msg == WM_KEYUP || msg == WM_LBUTTONUP)) InvalidateRect(n->hwnd, NULL, FALSE);
    return CallWindowProcW(g_edit_proc, hwnd, msg, wp, lp);
}

static void layout_note(note *n)
{
    RECT c;
    GetClientRect(n->hwnd, &c);
    if (n->edit) MoveWindow(n->edit, S(12), strip_h() + S(6), max(0, c.right - S(24)), max(0, c.bottom - strip_h() - bar_h() - S(8)), TRUE);
    if (n->menu) MoveWindow(n->menu, 0, 0, c.right, S(44) + 2 * S(40) + S(6), TRUE);
}

static LRESULT CALLBACK note_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    note *n = note_of(hwnd);
    switch (msg)
    {
    case WM_NCCREATE:
        n = (note *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)n);
        n->hwnd = hwnd;
        break;
    case WM_CREATE:
        n->edit = CreateWindowExW(0, MSFTEDIT_CLASS, L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL |
                                  WS_VSCROLL | ES_NOHIDESEL, 0, 0, 10, 10, hwnd, (HMENU)1, g_inst, NULL);
        if (!n->edit)
            n->edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                      0, 0, 10, 10, hwnd, (HMENU)1, g_inst, NULL);
        SendMessageW(n->edit, WM_SETFONT, (WPARAM)g_font_body, 0);
        SendMessageW(n->edit, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);
        SendMessageW(n->edit, EM_SETTEXTMODE, TM_RICHTEXT, 0);
        if (n->rtf && n->rtf[0])
        {
            instream s = { n->rtf, strlen(n->rtf) };
            EDITSTREAM es = { (DWORD_PTR)&s, 0, stream_in };
            SendMessageW(n->edit, EM_STREAMIN, SF_RTF, (LPARAM)&es);
        }
        free(n->rtf); n->rtf = NULL;
        g_edit_proc = (WNDPROC)SetWindowLongPtrW(n->edit, GWLP_WNDPROC, (LONG_PTR)edit_proc);
        apply_colour(n);
        SendMessageW(n->edit, EM_SETMODIFY, FALSE, 0);
        layout_note(n);
        return 0;
    case WM_SIZE: layout_note(n); InvalidateRect(hwnd, NULL, TRUE); break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps), mem;
        RECT c;
        HBITMAP bm, ob;
        GetClientRect(hwnd, &c);
        mem = CreateCompatibleDC(dc);
        bm = CreateCompatibleBitmap(dc, max(1, c.right), max(1, c.bottom));
        ob = SelectObject(mem, bm);
        paint_note(n, mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        DeleteObject(SelectObject(mem, ob));
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCHITTEST:
    {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        RECT w;
        int e = S(6);
        GetWindowRect(hwnd, &w);
        if (!IsZoomed(hwnd))
        {
            BOOL l = pt.x < w.left + e, r = pt.x >= w.right - e, t = pt.y < w.top + e / 2, b = pt.y >= w.bottom - e;
            if (t && l) return HTTOPLEFT;
            if (t && r) return HTTOPRIGHT;
            if (b && l) return HTBOTTOMLEFT;
            if (b && r) return HTBOTTOMRIGHT;
            if (l) return HTLEFT;
            if (r) return HTRIGHT;
            if (b) return HTBOTTOM;
            if (t) return HTTOP;
        }
        ScreenToClient(hwnd, &pt);
        if (pt.y < strip_h() && button_at(n, pt) == BTN_NONE) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCLBUTTONDOWN:
        close_menu(n);
        break;
    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int hot = button_at(n, pt);
        if (!n->tracking)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            n->tracking = TRUE;
        }
        if (hot != n->hot) { n->hot = hot; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        n->tracking = FALSE;
        if (n->hot) { n->hot = BTN_NONE; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        n->pressed = button_at(n, pt);
        if (n->pressed != BTN_MORE) close_menu(n);
        if (n->pressed) SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int b = button_at(n, pt), was = n->pressed;
        n->pressed = BTN_NONE;
        ReleaseCapture();
        InvalidateRect(hwnd, NULL, FALSE);
        if (!was || b != was) return 0;
        switch (b)
        {
        case BTN_NEW: new_note(n); break;
        case BTN_MORE: open_menu(n); break;
        case BTN_CLOSE: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case BTN_BOLD: toggle_effect(n, CFE_BOLD); SetFocus(n->edit); break;
        case BTN_ITALIC: toggle_effect(n, CFE_ITALIC); SetFocus(n->edit); break;
        case BTN_UNDER: toggle_effect(n, CFE_UNDERLINE); SetFocus(n->edit); break;
        case BTN_STRIKE: toggle_effect(n, CFE_STRIKEOUT); SetFocus(n->edit); break;
        case BTN_BULLET: toggle_bullets(n); SetFocus(n->edit); break;
        }
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == 1 && HIWORD(wp) == EN_CHANGE) { touch(n); refresh_list(); }
        return 0;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == EN_SELCHANGE) InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) close_menu(n);
        else if (n && n->edit) SetFocus(n->edit);
        return 0;
    case WM_SETFOCUS:
        if (n->edit) SetFocus(n->edit);
        return 0;
    case WM_EXITSIZEMOVE:
    case WM_MOVE:
        if (n) touch(n);
        break;
    case WM_TIMER:
        if (wp == 1) { KillTimer(hwnd, 1); save_note(n); }
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(220);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = S(160);
        return 0;
    case WM_CLOSE:
        /* closing a note keeps it: it is in the list, and comes back from there */
        close_menu(n);
        n->open = FALSE;
        KillTimer(hwnd, 1);
        ShowWindow(hwnd, SW_HIDE);
        save_note(n);
        refresh_list();
        maybe_exit();
        return 0;
    case WM_DESTROY:
        if (n) { KillTimer(hwnd, 1); n->hwnd = NULL; n->edit = NULL; }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* the ... menu: the colours, Notes list, Delete note */
static LRESULT CALLBACK menu_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    note *n = (note *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
        break;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        SetPropW(hwnd, L"hot", (HANDLE)(INT_PTR)(GET_Y_LPARAM(lp) + 1));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSELEAVE:
        RemovePropW(hwnd, L"hot");
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT c, r;
        int i, sw, hy = (int)(INT_PTR)GetPropW(hwnd, L"hot") - 1;
        BOOL dark = n->color == 6;
        COLORREF bg = dark ? RGB(0x2B, 0x2B, 0x2D) : RGB(0xFB, 0xFB, 0xFB), fg = dark ? RGB(0xF4, 0xF4, 0xF4) : RGB(0x1A, 0x1A, 0x1A);
        static const WCHAR *items[] = { L"Notes list", L"Delete note" };
        GetClientRect(hwnd, &c);
        fill(dc, &c, bg);
        sw = c.right / NCOLORS;
        for (i = 0; i < NCOLORS; i++)
        {
            SetRect(&r, i * sw, 0, (i + 1) * sw, S(44));
            fill(dc, &r, COLORS[i].strip);
            if (i == n->color)
            {
                /* a tick on the chosen colour */
                int cx = (r.left + r.right) / 2, cy = S(22);
                COLORREF k = COLORS[i].text;
                line(dc, cx - S(6), cy, cx - S(2), cy + S(4), k, max(2, S(2)));
                line(dc, cx - S(2), cy + S(4), cx + S(6), cy - S(4), k, max(2, S(2)));
            }
        }
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_font_ui);
        SetTextColor(dc, fg);
        for (i = 0; i < 2; i++)
        {
            int y = S(44) + S(3) + i * S(40);
            SetRect(&r, 0, y, c.right, y + S(40));
            if (hy >= r.top && hy < r.bottom) fill(dc, &r, dark ? RGB(0x3C, 0x3C, 0x3F) : RGB(0xEA, 0xEA, 0xEA));
            /* the icons: a list, a bin */
            if (i == 0)
            {
                int k;
                for (k = 0; k < 3; k++) line(dc, S(16), y + S(14) + k * S(6), S(30), y + S(14) + k * S(6), fg, 1);
            }
            else
            {
                line(dc, S(15), y + S(13), S(31), y + S(13), fg, 1);
                line(dc, S(17), y + S(13), S(18), y + S(28), fg, 1);
                line(dc, S(29), y + S(13), S(28), y + S(28), fg, 1);
                line(dc, S(18), y + S(28), S(28), y + S(28), fg, 1);
                line(dc, S(20), y + S(10), S(26), y + S(10), fg, 1);
            }
            r.left = S(44);
            DrawTextW(dc, items[i], -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT c;
        HWND parent = GetParent(hwnd);
        GetClientRect(hwnd, &c);
        if (y < S(44))
        {
            int i = min(NCOLORS - 1, x / max(1, c.right / NCOLORS));
            n->color = i;
            close_menu(n);
            apply_colour(n);
            touch(n);
            refresh_list();
        }
        else if (y < S(44) + S(3) + S(40)) { close_menu(n); show_list(); }
        else { close_menu(n); delete_note(n, parent); }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static note *new_note(note *beside)
{
    note *n;
    GUID g;
    RECT work;
    int w = S(300), h = S(300), x, y;

    if (g_n >= MAX_NOTES) return NULL;
    n = calloc(1, sizeof(*n));
    CoCreateGuid(&g);
    _snwprintf(n->id, 64, L"%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3, g.Data4[0],
               g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    n->open = TRUE;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (beside && beside->hwnd)
    {
        RECT r;
        GetWindowRect(beside->hwnd, &r);
        x = r.right + S(12); y = r.top;
        if (x + w > work.right) x = r.left - w - S(12);
        if (x < work.left) { x = r.left + S(24); y = r.top + S(24); }
    }
    else
    {
        x = work.right - w - S(40) - (g_n % 5) * S(24);
        y = work.top + S(40) + (g_n % 5) * S(24);
    }
    if (y + h > work.bottom) y = max(work.top, work.bottom - h);
    SetRect(&n->rc, x, y, x + w, y + h);
    g_notes[g_n++] = n;
    CreateWindowExW(0, NOTE_CLASS, APP_TITLE, WS_POPUP | WS_CLIPCHILDREN, x, y, w, h, NULL, NULL, g_inst, n);
    save_note(n);
    show_note(n);
    refresh_list();
    return n;
}

/* ---- the notes list ---------------------------------------------------------------------------- */

static BOOL matches(const note *n, const WCHAR *q)
{
    WCHAR text[4096], lt[4096], lq[128];
    if (!q[0]) return TRUE;
    note_text(n, text, 4096);
    lstrcpynW(lt, text, 4096); CharLowerW(lt);
    lstrcpynW(lq, q, 128); CharLowerW(lq);
    return wcsstr(lt, lq) != NULL;
}

static int cmp_modified(const void *a, const void *b)
{
    const note *x = *(note *const *)a, *y = *(note *const *)b;
    return -CompareFileTime(&x->modified, &y->modified);
}

static void refresh_list(void)
{
    WCHAR q[128] = L"";
    note *sorted[MAX_NOTES];
    int i;
    if (!g_list || g_loading) return;
    GetWindowTextW(g_search, q, 128);
    memcpy(sorted, g_notes, g_n * sizeof(*sorted));
    qsort(sorted, g_n, sizeof(*sorted), cmp_modified);
    SendMessageW(g_listbox, WM_SETREDRAW, FALSE, 0);
    SendMessageW(g_listbox, LB_RESETCONTENT, 0, 0);
    for (i = 0; i < g_n; i++)
        if (matches(sorted[i], q))
        {
            int k = (int)SendMessageW(g_listbox, LB_ADDSTRING, 0, (LPARAM)L"");
            SendMessageW(g_listbox, LB_SETITEMDATA, k, (LPARAM)sorted[i]);
        }
    SendMessageW(g_listbox, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_listbox, NULL, TRUE);
}

static BOOL is_note(const note *p)
{
    int i;
    for (i = 0; i < g_n; i++) if (g_notes[i] == p) return TRUE;
    return FALSE;
}

static void draw_card(const DRAWITEMSTRUCT *di)
{
    note *n = (note *)di->itemData;
    RECT r = di->rcItem, card, t;
    WCHAR text[4096], when[64];
    SYSTEMTIME st, now;
    FILETIME lf;
    HDC dc = di->hDC;
    fill(dc, &r, RGB(0xF3, 0xF3, 0xF3));
    if (di->itemID == (UINT)-1 || !is_note(n)) return;
    SetRect(&card, r.left + S(12), r.top + S(4), r.right - S(12), r.bottom - S(4));
    fill(dc, &card, (di->itemState & ODS_SELECTED) ? RGB(0xEC, 0xE4, 0xF8) : RGB(0xFF, 0xFF, 0xFF));
    SetRect(&t, card.left, card.top, card.right, card.top + S(4));
    fill(dc, &t, COLORS[n->color].button);
    if (di->itemState & ODS_FOCUS)
    {
        SetRect(&t, card.left, card.top, card.left + S(3), card.bottom);
        fill(dc, &t, ACCENT);
    }
    FileTimeToLocalFileTime(&n->modified, &lf);
    FileTimeToSystemTime(&lf, &st);
    GetLocalTime(&now);
    if (st.wYear == now.wYear && st.wMonth == now.wMonth && st.wDay == now.wDay)
        GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, when, 64);
    else GetDateFormatW(LOCALE_USER_DEFAULT, 0, &st, L"MMM d", when, 64);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font_small);
    SetTextColor(dc, RGB(0x60, 0x60, 0x60));
    SetRect(&t, card.left + S(12), card.top + S(8), card.right - S(12), card.top + S(26));
    DrawTextW(dc, when, -1, &t, DT_RIGHT | DT_SINGLELINE);
    note_text(n, text, 4096);
    SelectObject(dc, g_font_ui);
    SetTextColor(dc, text[0] ? RGB(0x1A, 0x1A, 0x1A) : RGB(0x80, 0x80, 0x80));
    SetRect(&t, card.left + S(12), card.top + S(24), card.right - S(12), card.bottom - S(6));
    DrawTextW(dc, text[0] ? text : L"Empty note", -1, &t, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_EDITCONTROL | DT_NOPREFIX);
    if (!n->open)
    {
        SelectObject(dc, g_font_small);
        SetTextColor(dc, RGB(0x80, 0x80, 0x80));
        SetRect(&t, card.left + S(12), card.top + S(8), card.right - S(80), card.top + S(26));
        DrawTextW(dc, L"Closed", -1, &t, DT_LEFT | DT_SINGLELINE);
    }
}

static void open_selected(void)
{
    int k = (int)SendMessageW(g_listbox, LB_GETCURSEL, 0, 0);
    note *n;
    if (k < 0) return;
    n = (note *)SendMessageW(g_listbox, LB_GETITEMDATA, k, 0);
    if (is_note(n)) { show_note(n); refresh_list(); }
}

static void layout_list(void)
{
    RECT c;
    GetClientRect(g_list, &c);
    MoveWindow(g_listnew, c.right - S(56), S(12), S(40), S(36), TRUE);
    MoveWindow(g_search, S(16), S(58), c.right - S(32), S(30), TRUE);
    MoveWindow(g_listbox, 0, S(98), c.right, max(0, c.bottom - S(98)), TRUE);
}

static LRESULT CALLBACK list_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SIZE: layout_list(); InvalidateRect(hwnd, NULL, TRUE); return 0;
    case WM_ERASEBKGND:
    {
        RECT c;
        GetClientRect(hwnd, &c);
        fill((HDC)wp, &c, RGB(0xF3, 0xF3, 0xF3));
        return 1;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        r.left = S(16); r.top = S(12); r.bottom = S(48);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, g_font_title);
        SetTextColor(dc, RGB(0x1A, 0x1A, 0x1A));
        DrawTextW(dc, APP_TITLE, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MEASUREITEM:
        ((MEASUREITEMSTRUCT *)lp)->itemHeight = S(96);
        return TRUE;
    case WM_DRAWITEM:
    {
        const DRAWITEMSTRUCT *di = (const DRAWITEMSTRUCT *)lp;
        if (di->CtlID == 3) draw_card(di);
        else if (di->CtlID == 2)
        {
            RECT r = di->rcItem;
            BOOL hot = (di->itemState & ODS_SELECTED) != 0;
            fill(di->hDC, &r, hot ? RGB(0xE0, 0xE0, 0xE0) : RGB(0xF3, 0xF3, 0xF3));
            line(di->hDC, (r.left + r.right) / 2 - S(7), (r.top + r.bottom) / 2, (r.left + r.right) / 2 + S(8), (r.top + r.bottom) / 2, ACCENT, max(2, S(2)));
            line(di->hDC, (r.left + r.right) / 2, (r.top + r.bottom) / 2 - S(7), (r.left + r.right) / 2, (r.top + r.bottom) / 2 + S(8), ACCENT, max(2, S(2)));
        }
        return TRUE;
    }
    case WM_CTLCOLORLISTBOX:
    {
        static HBRUSH grey;
        if (!grey) grey = CreateSolidBrush(RGB(0xF3, 0xF3, 0xF3));
        return (LRESULT)grey;
    }
    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wp, RGB(255, 255, 255));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_COMMAND:
        if (LOWORD(wp) == 2) new_note(NULL);
        else if (LOWORD(wp) == 4 && HIWORD(wp) == EN_CHANGE) refresh_list();
        else if (LOWORD(wp) == 3 && HIWORD(wp) == LBN_DBLCLK) open_selected();
        return 0;
    case WM_VKEYTOITEM:
        if (LOWORD(wp) == VK_RETURN) { open_selected(); return -2; }
        if (LOWORD(wp) == VK_DELETE)
        {
            int k = (int)SendMessageW(g_listbox, LB_GETCURSEL, 0, 0);
            if (k >= 0) delete_note((note *)SendMessageW(g_listbox, LB_GETITEMDATA, k, 0), hwnd);
            return -2;
        }
        return -1;
    case WM_CONTEXTMENU:
        if ((HWND)wp == g_listbox)
        {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }, cp = pt;
            int k, cmd;
            HMENU m;
            ScreenToClient(g_listbox, &cp);
            k = LOWORD(SendMessageW(g_listbox, LB_ITEMFROMPOINT, 0, MAKELPARAM(cp.x, cp.y)));
            if (k >= (int)SendMessageW(g_listbox, LB_GETCOUNT, 0, 0)) return 0;
            SendMessageW(g_listbox, LB_SETCURSEL, k, 0);
            m = CreatePopupMenu();
            AppendMenuW(m, MF_STRING, 1, L"&Open note");
            AppendMenuW(m, MF_STRING, 2, L"&Delete note");
            cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
            DestroyMenu(m);
            if (cmd == 1) open_selected();
            if (cmd == 2) delete_note((note *)SendMessageW(g_listbox, LB_GETITEMDATA, k, 0), hwnd);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        maybe_exit();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void show_list(void)
{
    if (!g_list)
    {
        RECT work;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        g_list = CreateWindowExW(0, LIST_CLASS, APP_TITLE, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                 work.left + S(40), work.top + S(40), S(360), min(S(560), work.bottom - work.top - S(80)),
                                 NULL, NULL, g_inst, NULL);
        g_listnew = CreateWindowExW(0, L"BUTTON", L"New note", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10,
                                    g_list, (HMENU)2, g_inst, NULL);
        g_search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                   0, 0, 10, 10, g_list, (HMENU)4, g_inst, NULL);
        SendMessageW(g_search, WM_SETFONT, (WPARAM)g_font_ui, 0);
        SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search...");
        g_listbox = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_OWNERDRAWFIXED |
                                    LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_WANTKEYBOARDINPUT,
                                    0, 0, 10, 10, g_list, (HMENU)3, g_inst, NULL);
        layout_list();
    }
    refresh_list();
    ShowWindow(g_list, IsIconic(g_list) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(g_list);
    SetFocus(g_search);
}

static void save_all(void)
{
    int i;
    for (i = 0; i < g_n; i++) if (g_notes[i]->hwnd) { KillTimer(g_notes[i]->hwnd, 1); save_note(g_notes[i]); }
}

static void maybe_exit(void)
{
    int i;
    if (g_list && IsWindowVisible(g_list)) return;
    for (i = 0; i < g_n; i++) if (IsWindowVisible(g_notes[i]->hwnd)) return;
    save_all();
    g_quitting = TRUE;
    PostQuitMessage(0);
}

/* ---- one instance ------------------------------------------------------------------------------ */

static void run_command(const WCHAR *cmd)
{
    int i, shown = 0;
    while (*cmd == ' ') cmd++;
    if (!_wcsnicmp(cmd, L"/new", 4) || !_wcsnicmp(cmd, L"-new", 4)) { new_note(NULL); return; }
    if (!_wcsnicmp(cmd, L"/list", 5)) { show_list(); return; }
    if (!_wcsnicmp(cmd, L"/quit", 5)) { save_all(); g_quitting = TRUE; PostQuitMessage(0); return; }
    /* started again: the open notes to the front, or the list when none is open */
    for (i = 0; i < g_n; i++)
        if (g_notes[i]->open) { ShowWindow(g_notes[i]->hwnd, SW_SHOWNOACTIVATE); SetForegroundWindow(g_notes[i]->hwnd); shown++; }
    if (!shown) { if (g_n) show_list(); else new_note(NULL); }
}

static LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_COPYDATA)
    {
        const COPYDATASTRUCT *cd = (const COPYDATASTRUCT *)lp;
        WCHAR cmd[256];
        int len = (int)min(cd->cbData / sizeof(WCHAR), 255);
        if (cd->dwData != 0x5354) return FALSE;
        memcpy(cmd, cd->lpData, len * sizeof(WCHAR));
        cmd[len] = 0;
        run_command(cmd);
        return TRUE;
    }
    if (msg == WM_TIMER) { write_dump(); return 0; }
    if (msg == WM_ENDSESSION && wp) save_all();
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    HANDLE mutex;
    WNDCLASSW wc = { 0 };
    MSG m;
    HDC sdc;
    int i, visible = 0;
    (void)prev; (void)show;

    g_inst = inst;
    mutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        /* hand the command line to the running copy */
        int tries;
        for (tries = 0; tries < 50; tries++)
        {
            HWND host = FindWindowExW(HWND_MESSAGE, NULL, HOST_CLASS, NULL);
            if (host)
            {
                COPYDATASTRUCT cd = { 0x5354, (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR)), cmd };
                DWORD_PTR r;
                AllowSetForegroundWindow(ASFW_ANY);
                SendMessageTimeoutW(host, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &r);
                return 0;
            }
            Sleep(100);
        }
        return 1;
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    LoadLibraryW(L"msftedit.dll");
    {
        INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&icc);
    }
    sdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);
    g_font_body = make_font(110, FW_NORMAL, 0, 0, 0, NULL);
    g_font_ui = make_font(100, FW_NORMAL, 0, 0, 0, NULL);
    g_font_ui_bold = make_font(100, FW_SEMIBOLD, 0, 0, 0, NULL);
    g_font_small = make_font(85, FW_NORMAL, 0, 0, 0, NULL);
    g_font_title = make_font(160, FW_SEMIBOLD, 0, 0, 0, NULL);
    g_font_glyph = make_font(110, FW_BOLD, 0, 0, 0, NULL);
    if (!GetEnvironmentVariableW(L"SG_STICKY_DUMP", g_dump, MAX_PATH)) g_dump[0] = 0;

    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpfnWndProc = host_proc; wc.lpszClassName = HOST_CLASS; RegisterClassW(&wc);
    wc.lpfnWndProc = note_proc; wc.lpszClassName = NOTE_CLASS; wc.style = CS_DROPSHADOW; RegisterClassW(&wc);
    wc.style = 0;
    wc.lpfnWndProc = menu_proc; wc.lpszClassName = MENU_CLASS; RegisterClassW(&wc);
    wc.lpfnWndProc = list_proc; wc.lpszClassName = LIST_CLASS; RegisterClassW(&wc);
    g_host = CreateWindowExW(0, HOST_CLASS, NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
    if (g_dump[0]) SetTimer(g_host, 1, 400, NULL);

    notes_dir();
    g_loading = TRUE;
    load_notes();
    for (i = 0; i < g_n; i++)
    {
        note *n = g_notes[i];
        RECT r = n->rc;
        CreateWindowExW(0, NOTE_CLASS, APP_TITLE, WS_POPUP | WS_CLIPCHILDREN, r.left, r.top, r.right - r.left,
                        r.bottom - r.top, NULL, NULL, inst, n);
        if (n->open) { ShowWindow(n->hwnd, SW_SHOWNOACTIVATE); visible++; }
    }
    g_loading = FALSE;
    while (*cmd == ' ') cmd++;
    if (*cmd) run_command(cmd);
    else if (!visible) { if (g_n) show_list(); else new_note(NULL); }
    else for (i = g_n - 1; i >= 0; i--) if (g_notes[i]->open) { SetForegroundWindow(g_notes[i]->hwnd); break; }

    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        if (g_list && IsDialogMessageW(g_list, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    save_all();
    write_dump();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
