/* sg-emoji -- the emoji panel (Windows' Win+. and Win+;).
 *
 * A small panel at the text cursor of the program in front: a search line,
 * the categories (recently used, smileys, people, animals and nature, food,
 * travel, activities, objects, symbols, flags) and a grid of colour emoji.
 * It never takes the focus (WS_EX_NOACTIVATE), so the program keeps it and a
 * clicked emoji arrives there as typing (SendInput KEYEVENTF_UNICODE), as
 * Windows' panel types it. A Linux program's window gets it through xdotool,
 * as voice typing does (sg-dictate). While the panel is open, letters typed
 * go to its search, as on Windows; the arrow keys move through the emoji,
 * Enter types the one chosen, Esc closes the panel; it closes, too, when
 * another window comes to the front.
 *
 *   sg-emoji [/toggle]   open the panel; if it is open, close it (explorer's
 *                        Win+. and Win+; run this, wine-sg 9412)
 *
 * The emoji and their names are Unicode's (emoji-test.txt, through
 * src/emoji/gen-emoji.py); recently used ones are kept in
 * HKCU\Software\Stained Glass\Emoji, Recent. Colour comes from DirectWrite
 * and Direct2D (Noto Color Emoji, through the system font fallback).
 *
 * SG_EMOJI_DUMP=<file> (gates only) writes the panel's state after every
 * change: where it is, the target, the query, the first results.
 * SG_XDOTOOL: the gate's stand-in for xdotool.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define COBJMACROS
#define INITGUID
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include "sg-dpi.h"

struct emoji_entry { const WCHAR *chars; const char *name; int group; };
#include "emoji-data.h"

#define PANEL_W 360
#define PANEL_H 412
#define PAD 10
#define SEARCH_H 34
#define TABS_H 38
#define COLS 8
#define CELL 42
#define MAX_RECENT 32
#define CAT_RECENT 9
#define WM_KEYCMD (WM_APP + 1)
#define WM_TEST_INSERT (WM_APP + 2)   /* gates: type the result at index wParam */

static const WCHAR *const cat_icons[] = {
    L"\xd83d\xde00", L"\xd83d\xdc4b", L"\xd83d\xdc3b", L"\xd83c\xdf54", L"\xd83d\xde97",
    L"\x26bd", L"\xd83d\xdca1", L"\x2764\xfe0f", L"\xd83c\xdfc1", L"\xd83d\xdd52" };
static const WCHAR *const cat_names[] = {
    L"Smileys & emotion", L"People", L"Animals & nature", L"Food & drink", L"Travel & places",
    L"Activities", L"Objects", L"Symbols", L"Flags", L"Recently used" };
/* the tab order: recently used first, as on Windows */
static const int tab_order[] = { CAT_RECENT, 0, 1, 2, 3, 4, 5, 6, 7, 8 };

static HWND g_wnd, g_target;
static BOOL g_linux;
static ID2D1Factory *g_d2d;
static IDWriteFactory *g_dw;
static ID2D1HwndRenderTarget *g_rt;
static IDWriteTextFormat *g_fmt_emoji, *g_fmt_ui, *g_fmt_tab;
static float g_scale = 1.0f;
static int g_cat = 0, g_scroll, g_hover = -1, g_selected = 0, g_hover_tab = -1;
static WCHAR g_query[64];
static const WCHAR *g_recent[MAX_RECENT];
static WCHAR g_recent_buf[MAX_RECENT][32];
static int g_recent_count;
static int g_results[2048], g_result_count;
static HHOOK g_hook;

/* ---- recently used ---------------------------------------------------- */

static void recent_load(void)
{
    WCHAR buf[MAX_RECENT * 32];
    DWORD size = sizeof(buf) - sizeof(WCHAR) * 2;
    WCHAR *p;
    memset(buf, 0, sizeof(buf));
    g_recent_count = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Stained Glass\\Emoji", L"Recent", RRF_RT_REG_MULTI_SZ, NULL, buf, &size))
        return;
    for (p = buf; *p && g_recent_count < MAX_RECENT; p += wcslen(p) + 1)
    {
        lstrcpynW(g_recent_buf[g_recent_count], p, 32);
        g_recent[g_recent_count] = g_recent_buf[g_recent_count];
        g_recent_count++;
    }
}

static void recent_add(const WCHAR *e)
{
    WCHAR buf[MAX_RECENT * 32 + 1], *p = buf;
    WCHAR keep[MAX_RECENT][32];
    int i, n = 0;
    lstrcpynW(keep[n++], e, 32);
    for (i = 0; i < g_recent_count && n < MAX_RECENT; i++)
        if (wcscmp(g_recent[i], e)) lstrcpynW(keep[n++], g_recent[i], 32);
    for (i = 0; i < n; i++)
    {
        lstrcpynW(g_recent_buf[i], keep[i], 32);
        g_recent[i] = g_recent_buf[i];
        wcscpy(p, keep[i]);
        p += wcslen(p) + 1;
    }
    *p++ = 0;
    g_recent_count = n;
    RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Stained Glass\\Emoji", L"Recent", REG_MULTI_SZ,
                    buf, (DWORD)((p - buf) * sizeof(WCHAR)));
}

/* ---- what the grid shows ---------------------------------------------- */

/* every word of the query starts a word of the name (case-insensitive) */
static BOOL name_matches(const char *name, const WCHAR *query)
{
    const WCHAR *q = query;
    while (*q)
    {
        WCHAR word[32];
        int len = 0, found = 0;
        const char *n;
        while (*q == ' ') q++;
        while (*q && *q != ' ' && len < 31) word[len++] = towlower(*q++);
        word[len] = 0;
        if (!len) break;
        for (n = name; *n && !found; n++)
        {
            int i;
            if (n != name && n[-1] != ' ' && n[-1] != '-' && n[-1] != ':') continue;
            for (i = 0; i < len && n[i] && towlower((unsigned char)n[i]) == word[i]; i++);
            if (i == len) found = 1;
        }
        if (!found) return FALSE;
    }
    return TRUE;
}

static void dump_state(void);

static void update_results(void)
{
    int i;
    g_result_count = 0;
    if (g_query[0])
    {
        for (i = 0; i < (int)ARRAYSIZE(emoji_table) && g_result_count < (int)ARRAYSIZE(g_results); i++)
            if (name_matches(emoji_table[i].name, g_query)) g_results[g_result_count++] = i;
    }
    else if (g_cat == CAT_RECENT)
    {
        for (i = 0; i < g_recent_count; i++) g_results[g_result_count++] = -1 - i;
    }
    else
    {
        for (i = 0; i < (int)ARRAYSIZE(emoji_table); i++)
            if (emoji_table[i].group == g_cat) g_results[g_result_count++] = i;
    }
    g_scroll = 0;
    g_selected = 0;
    dump_state();
}

static const WCHAR *result_chars(int r)
{
    int i = g_results[r];
    return i >= 0 ? emoji_table[i].chars : g_recent[-1 - i];
}

static const char *result_name(int r)
{
    int i = g_results[r], j;
    if (i >= 0) return emoji_table[i].name;
    for (j = 0; j < (int)ARRAYSIZE(emoji_table); j++)
        if (!wcscmp(emoji_table[j].chars, g_recent[-1 - i])) return emoji_table[j].name;
    return "";
}

/* ---- typing it into the program in front ------------------------------ */

static BOOL linux_window(HWND hwnd)
{
    WCHAR cls[32] = L"";
    return hwnd && GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && !lstrcmpW(cls, L"SgLinuxWindow");
}

static void type_linux(const WCHAR *text)
{
    static char *(CDECL *unix_name)(const WCHAR *);
    LONG (WINAPI *spawnvp)(char * const argv[], int wait) =
        (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__wine_unix_spawnvp");
    WCHAR tool[MAX_PATH] = L"";
    char *argv[7] = { NULL }, utf8[64];
    if (!unix_name) unix_name = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    if (GetEnvironmentVariableW(L"SG_XDOTOOL", tool, MAX_PATH) && unix_name) argv[0] = unix_name(tool);
    if (!argv[0]) argv[0] = (char *)"/usr/bin/xdotool";
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), NULL, NULL);
    argv[1] = (char *)"type"; argv[2] = (char *)"--clearmodifiers"; argv[3] = (char *)"--"; argv[4] = utf8;
    if (spawnvp) spawnvp(argv, TRUE);
}

static void type_text(const WCHAR *text)
{
    INPUT in[32];
    int n = 0;
    size_t i;
    for (i = 0; text[i] && n + 2 <= (int)ARRAYSIZE(in); i++)
    {
        memset(&in[n], 0, 2 * sizeof(INPUT));
        in[n].type = in[n + 1].type = INPUT_KEYBOARD;
        in[n].ki.wScan = in[n + 1].ki.wScan = text[i];
        in[n].ki.dwFlags = KEYEVENTF_UNICODE;
        in[n + 1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        n += 2;
    }
    SendInput(n, in, sizeof(INPUT));
}

static void insert(int r)
{
    WCHAR e[32];
    if (r < 0 || r >= g_result_count) return;
    lstrcpynW(e, result_chars(r), ARRAYSIZE(e));
#ifndef SG_MUTANT_EMOJI_NO_LINUX
    if (g_linux) type_linux(e);
    else
#endif
    type_text(e);
    recent_add(e);
    if (g_cat == CAT_RECENT && !g_query[0]) update_results();
    {
        FILE *f;
        WCHAR path[MAX_PATH];
        if (GetEnvironmentVariableW(L"SG_EMOJI_DUMP", path, MAX_PATH) && (f = _wfopen(path, L"a")))
        {
            char u[64];
            WideCharToMultiByte(CP_UTF8, 0, e, -1, u, sizeof(u), NULL, NULL);
            fprintf(f, "INSERTED %s %s\n", g_linux ? "xdotool" : "sendinput", u);
            fclose(f);
        }
    }
}

/* ---- the gate's view ---------------------------------------------------- */

static void dump_state(void)
{
    WCHAR path[MAX_PATH];
    FILE *f;
    RECT rc;
    char q[128];
    int i;
    if (!g_wnd || !GetEnvironmentVariableW(L"SG_EMOJI_DUMP", path, MAX_PATH) || !(f = _wfopen(path, L"a"))) return;
    GetWindowRect(g_wnd, &rc);
    WideCharToMultiByte(CP_UTF8, 0, g_query, -1, q, sizeof(q), NULL, NULL);
    fprintf(f, "STATE at %ld,%ld %ldx%ld target %p linux %d cat %d query \"%s\" results %d first",
            rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, g_target, g_linux, g_cat, q, g_result_count);
    for (i = 0; i < g_result_count && i < 3; i++) fprintf(f, " [%s]", result_name(i));
    fprintf(f, "\n");
    fclose(f);
}

/* ---- drawing ------------------------------------------------------------ */

static float px(float dip) { return dip * g_scale; }

static void create_formats(void)
{
    if (g_fmt_emoji) IDWriteTextFormat_Release(g_fmt_emoji);
    if (g_fmt_ui) IDWriteTextFormat_Release(g_fmt_ui);
    if (g_fmt_tab) IDWriteTextFormat_Release(g_fmt_tab);
    IDWriteFactory_CreateTextFormat(g_dw, L"Segoe UI Emoji", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, px(24), L"en-us", &g_fmt_emoji);
    IDWriteFactory_CreateTextFormat(g_dw, L"Segoe UI Emoji", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, px(18), L"en-us", &g_fmt_tab);
    IDWriteFactory_CreateTextFormat(g_dw, L"Segoe UI", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, px(13), L"en-us", &g_fmt_ui);
    IDWriteTextFormat_SetTextAlignment(g_fmt_emoji, DWRITE_TEXT_ALIGNMENT_CENTER);
    IDWriteTextFormat_SetParagraphAlignment(g_fmt_emoji, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    IDWriteTextFormat_SetTextAlignment(g_fmt_tab, DWRITE_TEXT_ALIGNMENT_CENTER);
    IDWriteTextFormat_SetParagraphAlignment(g_fmt_tab, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    IDWriteTextFormat_SetParagraphAlignment(g_fmt_ui, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

static BOOL dark_mode(void)
{
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size);
    return !light;
}

static D2D1_COLOR_F rgb(DWORD c, float a)
{
    D2D1_COLOR_F f = { ((c >> 16) & 0xff) / 255.0f, ((c >> 8) & 0xff) / 255.0f, (c & 0xff) / 255.0f, a };
    return f;
}

static D2D1_COLOR_F accent(void)
{
    DWORD c = 0, size = sizeof(c);
    /* AccentColor is ABGR */
    if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, NULL, &c, &size))
        return rgb(((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff), 1.0f);
    return rgb(0x8a2be2, 1.0f);
}

static int grid_top(void) { return PAD + SEARCH_H + 6 + TABS_H + 6; }
static int grid_rows_visible(void) { return (PANEL_H - grid_top() - PAD) / CELL; }

static void fill(ID2D1SolidColorBrush *b, D2D1_COLOR_F c, float l, float t, float r, float bt)
{
    D2D1_RECT_F rc = { px(l), px(t), px(r), px(bt) };
    ID2D1SolidColorBrush_SetColor(b, &c);
    ID2D1HwndRenderTarget_FillRectangle(g_rt, &rc, (ID2D1Brush *)b);
}

static void text(ID2D1SolidColorBrush *b, IDWriteTextFormat *fmt, const WCHAR *s, D2D1_COLOR_F c,
                 float l, float t, float r, float bt)
{
    D2D1_RECT_F rc = { px(l), px(t), px(r), px(bt) };
    ID2D1SolidColorBrush_SetColor(b, &c);
    ID2D1HwndRenderTarget_DrawText(g_rt, s, (UINT32)wcslen(s), fmt, &rc, (ID2D1Brush *)b,
                                   D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT,
                                   DWRITE_MEASURING_MODE_NATURAL);
}

static void paint(void)
{
    BOOL dark = dark_mode();
    D2D1_COLOR_F bg = rgb(dark ? 0x2b2b2b : 0xf3f3f3, 1), fg = rgb(dark ? 0xffffff : 0x1b1b1b, 1),
                 dim = rgb(dark ? 0x9a9a9a : 0x6b6b6b, 1), field = rgb(dark ? 0x3a3a3a : 0xffffff, 1),
                 hover = rgb(dark ? 0x454545 : 0xe0e0e0, 1), acc = accent();
    ID2D1SolidColorBrush *b;
    int i, rows, first;
    WCHAR buf[96];

    if (!g_rt)
    {
        RECT rc;
        D2D1_RENDER_TARGET_PROPERTIES props = { D2D1_RENDER_TARGET_TYPE_DEFAULT,
            { DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN }, 96, 96, 0, 0 };
        D2D1_HWND_RENDER_TARGET_PROPERTIES hprops;
        GetClientRect(g_wnd, &rc);
        hprops.hwnd = g_wnd;
        hprops.pixelSize.width = rc.right;
        hprops.pixelSize.height = rc.bottom;
        hprops.presentOptions = D2D1_PRESENT_OPTIONS_NONE;
        if (FAILED(ID2D1Factory_CreateHwndRenderTarget(g_d2d, &props, &hprops, &g_rt))) return;
    }
    ID2D1HwndRenderTarget_CreateSolidColorBrush(g_rt, &fg, NULL, &b);
    ID2D1HwndRenderTarget_BeginDraw(g_rt);
    ID2D1HwndRenderTarget_Clear(g_rt, &bg);

    /* the search line: what was typed, or how to search */
    fill(b, field, PAD, PAD, PANEL_W - PAD - 34, PAD + SEARCH_H);
    fill(b, acc, PAD, PAD + SEARCH_H - 2, PANEL_W - PAD - 34, PAD + SEARCH_H);
    if (g_query[0]) text(b, g_fmt_ui, g_query, fg, PAD + 10, PAD, PANEL_W - PAD - 40, PAD + SEARCH_H);
    else text(b, g_fmt_ui, g_linux ? L"Pick an emoji" : L"Type to search", dim, PAD + 10, PAD, PANEL_W - PAD - 40, PAD + SEARCH_H);
    /* close */
    if (g_hover_tab == 100) fill(b, hover, PANEL_W - PAD - 30, PAD, PANEL_W - PAD, PAD + SEARCH_H);
    {
        /* a drawn cross: the interface font may have no U+2715 */
        float cx = PANEL_W - PAD - 15, cy = PAD + SEARCH_H / 2.0f;
        D2D1_POINT_2F a = { px(cx - 5), px(cy - 5) }, z = { px(cx + 5), px(cy + 5) };
        D2D1_POINT_2F c = { px(cx + 5), px(cy - 5) }, d = { px(cx - 5), px(cy + 5) };
        ID2D1SolidColorBrush_SetColor(b, &fg);
        ID2D1HwndRenderTarget_DrawLine(g_rt, a, z, (ID2D1Brush *)b, px(1.2f), NULL);
        ID2D1HwndRenderTarget_DrawLine(g_rt, c, d, (ID2D1Brush *)b, px(1.2f), NULL);
    }

    /* the categories */
    for (i = 0; i < (int)ARRAYSIZE(tab_order); i++)
    {
        float l = PAD + i * ((PANEL_W - 2 * PAD) / (float)ARRAYSIZE(tab_order)), r = l + (PANEL_W - 2 * PAD) / (float)ARRAYSIZE(tab_order);
        float t = PAD + SEARCH_H + 6;
        if (g_hover_tab == i) fill(b, hover, l, t, r, t + TABS_H);
        text(b, g_fmt_tab, cat_icons[tab_order[i]], fg, l, t, r, t + TABS_H - 3);
        if (!g_query[0] && g_cat == tab_order[i]) fill(b, acc, l + 8, t + TABS_H - 3, r - 8, t + TABS_H);
    }

    /* the heading and the grid */
    rows = grid_rows_visible();
    first = g_scroll * COLS;
    if (g_query[0] && !g_result_count)
        text(b, g_fmt_ui, L"No emoji found", dim, PAD, grid_top(), PANEL_W - PAD, grid_top() + CELL);
    else if (!g_query[0] && g_cat == CAT_RECENT && !g_result_count)
        text(b, g_fmt_ui, L"Emoji you use show up here", dim, PAD, grid_top(), PANEL_W - PAD, grid_top() + CELL);
    for (i = first; i < g_result_count && i < first + rows * COLS; i++)
    {
        int col = (i - first) % COLS, row = (i - first) / COLS;
        float l = PAD + col * CELL + (PANEL_W - 2 * PAD - COLS * CELL) / 2.0f, t = grid_top() + row * CELL;
        if (i == g_hover || i == g_selected) fill(b, i == g_selected ? hover : hover, l, t, l + CELL, t + CELL);
        if (i == g_selected) fill(b, acc, l, t + CELL - 2, l + CELL, t + CELL);
        text(b, g_fmt_emoji, result_chars(i), fg, l, t, l + CELL, t + CELL);
    }
    /* the name of the one under the pointer (or chosen), at the bottom */
    i = g_hover >= 0 ? g_hover : g_selected;
    if (i >= 0 && i < g_result_count)
    {
        MultiByteToWideChar(CP_UTF8, 0, result_name(i), -1, buf, ARRAYSIZE(buf));
        text(b, g_fmt_ui, buf, dim, PAD, PANEL_H - PAD - 16, PANEL_W - PAD, PANEL_H - 2);
    }
    else if (!g_query[0])
        text(b, g_fmt_ui, cat_names[g_cat], dim, PAD, PANEL_H - PAD - 16, PANEL_W - PAD, PANEL_H - 2);
    if (ID2D1HwndRenderTarget_EndDraw(g_rt, NULL, NULL) == (HRESULT)D2DERR_RECREATE_TARGET)
    {
        ID2D1HwndRenderTarget_Release(g_rt);
        g_rt = NULL;
    }
    ID2D1SolidColorBrush_Release(b);
}

/* ---- where things are ---------------------------------------------------- */

static int hit_cell(int x, int y)
{
    float gx = PAD + (PANEL_W - 2 * PAD - COLS * CELL) / 2.0f;
    int col, row, i;
    x = (int)(x / g_scale); y = (int)(y / g_scale);
    if (y < grid_top() || x < gx || x >= gx + COLS * CELL) return -1;
    col = (int)((x - gx) / CELL);
    row = (y - grid_top()) / CELL;
    if (row >= grid_rows_visible()) return -1;
    i = (g_scroll + row) * COLS + col;
    return i < g_result_count ? i : -1;
}

static int hit_tab(int x, int y)
{
    int t = PAD + SEARCH_H + 6;
    x = (int)(x / g_scale); y = (int)(y / g_scale);
    if (y >= PAD && y < PAD + SEARCH_H && x >= PANEL_W - PAD - 30 && x < PANEL_W - PAD) return 100;
    if (y < t || y >= t + TABS_H || x < PAD || x >= PANEL_W - PAD) return -1;
    return (int)((x - PAD) / ((PANEL_W - 2 * PAD) / (float)ARRAYSIZE(tab_order)));
}

static void ensure_visible(void)
{
    int row = g_selected / COLS, rows = grid_rows_visible();
    if (row < g_scroll) g_scroll = row;
    if (row >= g_scroll + rows) g_scroll = row - rows + 1;
}

static void place_panel(void)
{
    GUITHREADINFO gti = { sizeof(gti) };
    RECT anchor, work;
    MONITORINFO mi = { sizeof(mi) };
    HMONITOR mon;
    int w, h, x, y;

    g_target = GetForegroundWindow();
    g_linux = linux_window(g_target);
    if (g_target && GetGUIThreadInfo(GetWindowThreadProcessId(g_target, NULL), &gti) && gti.hwndCaret)
    {
        anchor = gti.rcCaret;
        MapWindowPoints(gti.hwndCaret, NULL, (POINT *)&anchor, 2);
    }
    else if (g_target && GetWindowRect(g_target, &anchor))
    {
        /* no caret to be found (a Linux program, a program that draws its own): the middle of its window */
        anchor.left = anchor.right = (anchor.left + anchor.right) / 2 - PANEL_W / 2;
        anchor.top = anchor.bottom = (anchor.top + anchor.bottom) / 2 - PANEL_H / 2;
    }
    else
    {
        GetCursorPos((POINT *)&anchor);
        anchor.right = anchor.left; anchor.bottom = anchor.top;
    }
    mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(mon, &mi);
    work = mi.rcWork;
    g_scale = sg_dpi_for(NULL) / 96.0f;
    w = (int)(PANEL_W * g_scale); h = (int)(PANEL_H * g_scale);
    x = anchor.left;
    y = anchor.bottom + 4;
    if (y + h > work.bottom) y = anchor.top - h - 4;           /* above the caret when there is no room below */
    if (y < work.top) y = work.top;
    if (x + w > work.right) x = work.right - w;
    if (x < work.left) x = work.left;
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

/* ---- keys: while the panel is open, typing searches ---------------------- */

static LRESULT CALLBACK key_hook(int code, WPARAM wp, LPARAM lp)
{
    const KBDLLHOOKSTRUCT *k = (const KBDLLHOOKSTRUCT *)lp;
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && !(k->flags & LLKHF_INJECTED) &&
        GetAsyncKeyState(VK_CONTROL) >= 0 && GetAsyncKeyState(VK_MENU) >= 0 &&
        GetAsyncKeyState(VK_LWIN) >= 0 && GetAsyncKeyState(VK_RWIN) >= 0)
    {
        BYTE state[256];
        WCHAR ch[4];
        switch (k->vkCode)
        {
        case VK_ESCAPE: case VK_RETURN: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_BACK:
            PostMessageW(g_wnd, WM_KEYCMD, k->vkCode, 0);
            return 1;
        }
        GetKeyboardState(state);
        state[VK_SHIFT] = GetAsyncKeyState(VK_SHIFT) < 0 ? 0x80 : 0;
        if (ToUnicode(k->vkCode, k->scanCode, state, ch, ARRAYSIZE(ch), 4) == 1 && (iswalnum(ch[0]) || ch[0] == ' '))
        {
            PostMessageW(g_wnd, WM_KEYCMD, 0, ch[0]);
            return 1;
        }
    }
    else if (code == HC_ACTION && (wp == WM_KEYUP || wp == WM_SYSKEYUP) && !(k->flags & LLKHF_INJECTED))
    {
        switch (k->vkCode)
        {
        case VK_ESCAPE: case VK_RETURN: case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_BACK:
            return 1;
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static void key_command(WPARAM vk, LPARAM ch)
{
    size_t len = wcslen(g_query);
    if (ch)
    {
        if (len + 1 < ARRAYSIZE(g_query) && !(ch == ' ' && !len)) { g_query[len] = (WCHAR)ch; g_query[len + 1] = 0; update_results(); }
    }
    else switch (vk)
    {
    case VK_ESCAPE:
        if (g_query[0]) { g_query[0] = 0; update_results(); }
        else DestroyWindow(g_wnd);
        return;
    case VK_BACK:
        if (len) { g_query[len - 1] = 0; update_results(); }
        break;
    case VK_RETURN: insert(g_selected); break;
    case VK_LEFT: if (g_selected > 0) g_selected--; break;
    case VK_RIGHT: if (g_selected + 1 < g_result_count) g_selected++; break;
    case VK_UP: if (g_selected >= COLS) g_selected -= COLS; break;
    case VK_DOWN: if (g_selected + COLS < g_result_count) g_selected += COLS; break;
    }
    ensure_visible();
    InvalidateRect(g_wnd, NULL, FALSE);
}

static void CALLBACK foreground_changed(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG obj, LONG child, DWORD thread, DWORD time)
{
    (void)hook; (void)event; (void)obj; (void)child; (void)thread; (void)time;
    /* another window in front: the panel goes, as Windows' does */
    if (hwnd && hwnd != g_wnd && hwnd != g_target && GetAncestor(hwnd, GA_ROOT) != g_wnd) DestroyWindow(g_wnd);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (g_rt)
        {
            D2D1_SIZE_U size = { LOWORD(lp), HIWORD(lp) };
            ID2D1HwndRenderTarget_Resize(g_rt, &size);
        }
        return 0;
    case WM_DPICHANGED:
        g_scale = sg_dpi_new(wp) / 96.0f;
        create_formats();
        sg_dpi_apply_rect(hwnd, lp);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_MOUSEMOVE:
    {
        int c = hit_cell((short)LOWORD(lp), (short)HIWORD(lp)), t = hit_tab((short)LOWORD(lp), (short)HIWORD(lp));
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        if (c != g_hover || t != g_hover_tab) { g_hover = c; g_hover_tab = t; InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = g_hover_tab = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
    {
        int c = hit_cell((short)LOWORD(lp), (short)HIWORD(lp)), t = hit_tab((short)LOWORD(lp), (short)HIWORD(lp));
        if (t == 100) { DestroyWindow(hwnd); return 0; }
        if (t >= 0) { g_query[0] = 0; g_cat = tab_order[t]; update_results(); }
        else if (c >= 0) { g_selected = c; insert(c); }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL:
    {
        int rows = (g_result_count + COLS - 1) / COLS, max_scroll = rows - grid_rows_visible();
        g_scroll -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 2;
        if (g_scroll > max_scroll) g_scroll = max_scroll;
        if (g_scroll < 0) g_scroll = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_KEYCMD:
        key_command(wp, lp);
        return 0;
    case WM_TEST_INSERT:
        insert((int)wp);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* the foreground lock (wine-sg 9930): the running one may come forward on this request, made on the
 * person's key or click -- as Windows programs hand it on (AllowSetForegroundWindow) */
static void pass_foreground(HWND to)
{
    DWORD pid;
#ifdef SG_MUTANT_HANDOFF_NO_GRANT
    return;
#endif
    if (to && GetWindowThreadProcessId(to, &pid)) AllowSetForegroundWindow(pid);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, WCHAR *cmdline, int show)
{
    WNDCLASSW wc = { 0 };
    HWND running;
    MSG msg;
    (void)prev; (void)show;

    /* Win+. again (or /toggle) closes an open panel */
    if ((running = FindWindowW(L"SgEmojiPanel", NULL)))
    {
        pass_foreground(running);   /* closing, it gives the keyboard back to the window it typed into */
        PostMessageW(running, WM_CLOSE, 0, 0);
        return 0;
    }
    (void)cmdline;
    sg_dpi_init();
    CoInitialize(NULL);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL, (void **)&g_d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &IID_IDWriteFactory, (IUnknown **)&g_dw)))
        return 1;
    recent_load();
    /* the recently used ones first, when there are any, as Windows opens it */
    g_cat = g_recent_count ? CAT_RECENT : 0;

    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.lpszClassName = L"SgEmojiPanel";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"SgEmojiPanel", L"Emoji",
                            WS_POPUP | WS_BORDER, 0, 0, PANEL_W, PANEL_H, NULL, NULL, inst, NULL);
    place_panel();
    create_formats();
    update_results();
    /* typing goes to the search only for a Windows program: a Linux one has the keyboard itself */
    if (!g_linux) g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, key_hook, inst, 0);
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, foreground_changed, 0, 0, WINEVENT_OUTOFCONTEXT);
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hook) UnhookWindowsHookEx(g_hook);
    return 0;
}
