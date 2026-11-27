/*
 * sg-calendar -- the taskbar clock's calendar flyout (David 2026-10-28: "the taskbar
 * time/date does not come up with a calendar when you click on it"; 2026-11-26: "when
 * you click the time it should bring up a nice calendar just like in Windows").
 *
 * As on Windows 10: a click on a taskbar clock opens, against that bar under its clock
 * (any display's bar, on whichever edge), the time with its seconds, the long date (a
 * link to the date and time settings), and the month: its name, arrows to the month
 * before and after, the days of the week from the locale's first day, and six weeks of
 * days with today in the accent colour and the other months' days dimmed. The month
 * slides in from below or above. A click on the month's name shows the year's months,
 * one on the year's the decade's years; a click on one of those goes back in. The
 * pointer lights the cells' edges near it (Windows 10's reveal), the background is
 * frosted where the desktop's compositor can (wine-sg 0745's __wine_sg_acrylic), and the
 * keyboard moves a focus: the arrows a day (or a month, a year), Page Up / Page Down a
 * month, Home today, Enter / Space chooses, Ctrl+Up shows the larger view, Esc closes.
 *
 * With the Rounded look (Windows 11's, Settings > Personalization) it is Windows 11's
 * flyout instead: round, off the bar's edge, the day's date on top with a chevron that
 * folds the month away (remembered), and round day cells.
 *
 * Esc, a click elsewhere, and a second click on the clock close it.
 *
 *   sg-calendar64.exe --background       the session's copy, hidden (sg-session starts it)
 *   sg-calendar64.exe --show [--bar N]   open it (or, if it is open, close it): the taskbar's
 *                                        clock posts SgCalendarToggle (lparam: its bar) to
 *                                        SgCalendarHost, and starts this only when no copy
 *                                        runs yet (wine-sg 7603, 9950)
 *   sg-calendar64.exe --dump YYYY-MM     the month's grid as text (the gate's)
 *
 * SG_CALENDAR_DUMP=FILE: the shown state after each paint (the gate's).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <math.h>
#include "sg-mode.h"
#include "sg-round.h"
#include "sg-dpi.h"
#include "sg-smooth.h"

/* Windows 10's flyout, at 96 DPI */
#define W10_W 360
#define W10_H 508
#define W10_CELL_W 46
#define W10_CELL_H 40
#define W10_GRID_X 17
#define W10_BODY_Y 178          /* the days of the week, then the six weeks */
/* Windows 11's */
#define W11_W 334
#define W11_HEAD_H 48           /* the date and the chevron: all of it when folded */
#define W11_CELL_W 44
#define W11_CELL_H 42
#define W11_GRID_X 13
#define W11_BODY_Y 104
#define W11_H (W11_BODY_Y + 32 + 6 * W11_CELL_H + 14)
#define W11_MARGIN 12

#define TICK_TIMER 1
#define ANIM_TIMER 2
#define ANIM_MS 200

enum { VIEW_MONTH, VIEW_YEAR, VIEW_DECADE };
enum { HOT_NONE, HOT_DATE, HOT_UP, HOT_DOWN, HOT_SETTINGS, HOT_TITLE, HOT_CHEVRON };

static HWND g_host, g_fly;
static HFONT g_font, g_font_big, g_font_month, g_font_day, g_font_item;
static int g_dpi = 96;
#define S(x) MulDiv((x), g_dpi, 96)
static BOOL g_w11;                          /* Windows 11's look (the Rounded look) */
static BOOL g_folded;                       /* ...its month folded away */
static int g_view = VIEW_MONTH;
static int g_year, g_month;                 /* the month shown (the year's, the decade's: g_year) */
static int g_sel_year, g_sel_month, g_sel_day;  /* the day clicked, 0: none */
static int g_foc_year, g_foc_month, g_foc_day;  /* the keyboard's day (or month, year) */
static BOOL g_kbd;                          /* the keyboard was used: its focus shows */
static int g_hot = -1;                      /* the cell under the pointer, -1: none */
static int g_hot_part;
static POINT g_mouse = { -1000, -1000 };    /* the pointer, for the reveal */
static DWORD g_hidden_tick;                 /* when it hid on losing activation */
static HWND g_hidden_bar;                   /* ...by a press on this bar's clock, NULL: not so */
static HWND g_hidden_from;                  /* ...while shown at this bar */
static HWND g_bar, g_shown_bar;             /* the bar whose clock asked; the one it is shown at */
static int g_edge = ABE_BOTTOM;             /* that bar's edge */
static UINT g_toggle_msg;
static UINT g_acrylic;
static int g_wheel;
/* the slide: the view before it, from where */
static int g_anim_dir;                      /* +1 the new one comes from below, -1 from above, 0: none */
static DWORD g_anim_start;
static int g_prev_view, g_prev_year, g_prev_month;
static int g_slide_frames;                  /* frames drawn mid-slide (the gate's) */

/* ---- the month ------------------------------------------------------------------------------ */

static int days_in(int y, int m)
{
    static const int d[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0)) return 29;
    return d[m - 1];
}

/* 0 Sunday .. 6 Saturday */
static int weekday(int y, int m, int d)
{
    static const int t[] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

/* the locale's first day of the week, 0 Sunday .. 6 Saturday (LOCALE_IFIRSTDAYOFWEEK: 0 Monday) */
static int first_weekday(void)
{
    WCHAR v[4] = L"";
    const WCHAR *forced = _wgetenv(L"SG_CALENDAR_FIRSTDAY");
#ifdef SG_MUTANT_CALENDAR_SUNDAY
    return 0;
#endif
    if (forced && *forced) return _wtoi(forced) % 7;
    if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK, v, ARRAYSIZE(v))) return 0;
    return (_wtoi(v) + 1) % 7;
}

/* cell i (0..41) of month y-m: its date */
static void cell_date(int y, int m, int i, int *cy, int *cm, int *cd)
{
    int lead = (weekday(y, m, 1) - first_weekday() + 7) % 7, d = i - lead + 1;
    *cy = y; *cm = m;
    if (d < 1) {
        if (--*cm < 1) { *cm = 12; --*cy; }
        d += days_in(*cy, *cm);
    } else if (d > days_in(y, m)) {
        d -= days_in(y, m);
        if (++*cm > 12) { *cm = 1; ++*cy; }
    }
    *cd = d;
}

static void today(int *y, int *m, int *d)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    *y = st.wYear; *m = st.wMonth; *d = st.wDay;
}

static int cells_in_view(int view) { return view == VIEW_MONTH ? 42 : 16; }

/* year/decade views: item i's year and month (decade: month 0) */
static void item_of(int view, int year, int i, int *y, int *m)
{
    if (view == VIEW_YEAR) { *y = year + i / 12; *m = i % 12 + 1; }
    else { *y = year - year % 10 + i; *m = 0; }
}

/* ---- where things are ------------------------------------------------------------------------ */

static int fly_w(void) { return S(g_w11 ? W11_W : W10_W); }
static int fly_h(void) { return S(g_w11 ? (g_folded ? W11_HEAD_H : W11_H) : W10_H); }

static RECT rc(int l, int t, int r, int b) { RECT x = { S(l), S(t), S(r), S(b) }; return x; }
static RECT date_rect(void)     { return g_w11 ? rc(16, 0, W11_W - 56, W11_HEAD_H) : rc(24, 72, W10_W - 24, 98); }
static RECT chevron_rect(void)  { return g_w11 ? rc(W11_W - 50, 8, W11_W - 14, 40) : rc(0, 0, 0, 0); }
static RECT title_rect(void)    { return g_w11 ? rc(20, 58, W11_W - 96, 94) : rc(24, 126, W10_W - 100, 158); }
static RECT up_rect(void)       { return g_w11 ? rc(W11_W - 90, 58, W11_W - 54, 94) : rc(W10_W - 92, 126, W10_W - 56, 158); }
static RECT down_rect(void)     { return g_w11 ? rc(W11_W - 50, 58, W11_W - 14, 94) : rc(W10_W - 52, 126, W10_W - 16, 158); }
static RECT settings_rect(void) { return g_w11 ? rc(0, 0, 0, 0) : rc(24, W10_H - 44, 240, W10_H - 16); }

static int body_x(void) { return g_w11 ? W11_GRID_X : W10_GRID_X; }
static int body_y(void) { return g_w11 ? W11_BODY_Y : W10_BODY_Y; }
static int cell_w(void) { return g_w11 ? W11_CELL_W : W10_CELL_W; }
static int cell_h(void) { return g_w11 ? W11_CELL_H : W10_CELL_H; }
static int weekdays_h(void) { return g_w11 ? 32 : 26; }

/* the part that slides: the six weeks (the days of the week stay), or the year's or decade's 4 x 4 */
static RECT slide_rect(int view)
{
    RECT r;
    r.left = S(body_x());
    r.right = S(body_x() + 7 * cell_w());
    r.top = S(body_y() + (view == VIEW_MONTH ? weekdays_h() : 0));
    r.bottom = S(body_y() + weekdays_h() + 6 * cell_h());
    return r;
}

static RECT cell_rect_in(int view, int i, int dy)
{
    RECT r, s = slide_rect(view);
    if (view == VIEW_MONTH) {
        r.left = S(body_x() + (i % 7) * cell_w());
        r.top = S(body_y() + weekdays_h() + (i / 7) * cell_h());
        r.right = r.left + S(cell_w());
        r.bottom = r.top + S(cell_h());
    } else {
        int w = s.right - s.left, h = s.bottom - s.top;
        r.left = s.left + (i % 4) * w / 4;
        r.right = s.left + (i % 4 + 1) * w / 4;
        r.top = s.top + (i / 4) * h / 4;
        r.bottom = s.top + (i / 4 + 1) * h / 4;
    }
    OffsetRect(&r, 0, dy);
    return r;
}
static RECT cell_rect(int i) { return cell_rect_in(g_view, i, 0); }

static int cell_at(POINT pt)
{
    int i;
    if (g_w11 && g_folded) return -1;
    for (i = 0; i < cells_in_view(g_view); i++) { RECT r = cell_rect(i); if (PtInRect(&r, pt)) return i; }
    return -1;
}

static int part_at(POINT pt)
{
    RECT r;
    if (!g_w11 && (r = date_rect(), PtInRect(&r, pt))) return HOT_DATE;
    if (g_w11 && (r = chevron_rect(), PtInRect(&r, pt))) return HOT_CHEVRON;
    if (g_w11 && g_folded) return HOT_NONE;
    if ((r = up_rect(), PtInRect(&r, pt))) return HOT_UP;
    if ((r = down_rect(), PtInRect(&r, pt))) return HOT_DOWN;
    if ((r = settings_rect(), PtInRect(&r, pt))) return HOT_SETTINGS;
    if (g_view != VIEW_DECADE && (r = title_rect(), PtInRect(&r, pt))) return HOT_TITLE;
    return HOT_NONE;
}

/* ---- the look -------------------------------------------------------------------------------- */

static DWORD reg_dword(const WCHAR *key, const WCHAR *name, DWORD def)
{
    DWORD v, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, NULL, &v, &size) ? def : v;
}

/* Windows 11's flyout with the Rounded taskbar (Settings > Personalization > Taskbar style,
 * Taskbar\Look 1), or, where no taskbar look was chosen, the Rounded style */
static BOOL want_w11(void)
{
    DWORD look = reg_dword(L"Software\\Stained Glass\\Taskbar", L"Look", 0xffffffff);
#ifdef SG_MUTANT_CALENDAR_W10_ONLY
    return FALSE;
#endif
    if (look <= 3) return look == 1;
    return reg_dword(L"Software\\Stained Glass\\Style", L"Rounded", 0) != 0;
}

static const WCHAR CAL_KEY[] = L"Software\\Stained Glass\\Calendar";

static void make_fonts(void);

static void create_flyout(void);

static void load_look(void)
{
    BOOL w11 = want_w11();
    g_folded = w11 && reg_dword(CAL_KEY, L"Folded", 0) != 0;
    if (w11 != g_w11) {
        g_w11 = w11;
        make_fonts();
        /* a new window for the other look: the old one kept the Rounded style's corners
         * (wine-sg 0492's region, at its old size) after a switch to Classic */
        if (g_fly && !IsWindowVisible(g_fly)) { DestroyWindow(g_fly); g_fly = NULL; create_flyout(); }
    }
}

struct palette {
    COLORREF bg, text, subtle, line, hot, border, reveal, press;
};

static void palette(struct palette *p, BOOL dark)
{
    if (g_w11) {
        p->bg = dark ? RGB(0x20, 0x20, 0x20) : RGB(0xF3, 0xF3, 0xF3);
        p->line = dark ? RGB(0x34, 0x34, 0x34) : RGB(0xE0, 0xE0, 0xE0);
        p->border = dark ? RGB(0x45, 0x45, 0x45) : RGB(0xC9, 0xC9, 0xC9);
        p->hot = dark ? RGB(0x2D, 0x2D, 0x2D) : RGB(0xE9, 0xE9, 0xE9);
        p->press = dark ? RGB(0x29, 0x29, 0x29) : RGB(0xEE, 0xEE, 0xEE);
    } else {
        p->bg = dark ? RGB(0x1F, 0x1F, 0x1F) : RGB(0xE6, 0xE6, 0xE6);
        p->line = dark ? RGB(0x40, 0x40, 0x40) : RGB(0xC6, 0xC6, 0xC6);
        p->border = dark ? RGB(0x48, 0x48, 0x48) : RGB(0xA8, 0xA8, 0xA8);
        p->hot = dark ? RGB(0x40, 0x40, 0x40) : RGB(0xCC, 0xCC, 0xCC);
        p->press = dark ? RGB(0x55, 0x55, 0x55) : RGB(0xB4, 0xB4, 0xB4);
    }
    p->text = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0, 0, 0);
    p->subtle = dark ? RGB(0x80, 0x80, 0x80) : RGB(0x7A, 0x7A, 0x7A);
    p->reveal = dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00);
}

/* ---- painting -------------------------------------------------------------------------------- */

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, r, b);
    DeleteObject(b);
}

static void frame(HDC dc, const RECT *r, COLORREF c, int w)
{
    RECT e = *r;
    int i;
    HBRUSH b = CreateSolidBrush(c);
    for (i = 0; i < w; i++) {
        FrameRect(dc, &e, b);
        InflateRect(&e, -1, -1);
    }
    DeleteObject(b);
}

static void disc(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c), ob = SelectObject(dc, b);
    HPEN p = CreatePen(PS_SOLID, 1, c), op = SelectObject(dc, p);
    int d = min(r->right - r->left, r->bottom - r->top) - S(4), cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;
    sg_ellipse(dc, cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static void ring(HDC dc, const RECT *r, COLORREF c, int w)
{
    HBRUSH ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HPEN p = CreatePen(PS_INSIDEFRAME, w, c), op = SelectObject(dc, p);
    int d = min(r->right - r->left, r->bottom - r->top) - S(4), cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;
    sg_ellipse(dc, cx - d / 2, cy - d / 2, cx - d / 2 + d, cy - d / 2 + d);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(p);
}

static void text_in(HDC dc, HFONT f, const WCHAR *s, RECT r, COLORREF c, UINT align)
{
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | align);
}

static void link(HDC dc, const WCHAR *text, RECT r, COLORREF c, BOOL hot, UINT align)
{
    LOGFONTW lf;
    HFONT f;
    GetObjectW(g_font, sizeof(lf), &lf);
    lf.lfUnderline = hot;
    f = CreateFontIndirectW(&lf);
    text_in(dc, f, text, r, c, align);
    SelectObject(dc, g_font);
    DeleteObject(f);
}

/* an arrow: up or down, a chevron of two lines */
static void chevron(HDC dc, RECT r, BOOL up, COLORREF c)
{
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, w = S(6), h = S(3);
    HPEN pen = CreatePen(PS_SOLID, S(1) > 1 ? S(1) + 1 : 2, c), old = SelectObject(dc, pen);
    MoveToEx(dc, cx - w, up ? cy + h : cy - h, NULL);
    LineTo(dc, cx, up ? cy - h : cy + h);
    LineTo(dc, cx + w + 1, up ? cy + h + 1 : cy - h - 1);
    SelectObject(dc, old);
    DeleteObject(pen);
}

/* the view's title: the month and year, the year, the decade */
static void view_title(int view, int y, int m, WCHAR *buf, int n)
{
    if (view == VIEW_MONTH) {
        SYSTEMTIME ms = { 0 };
        ms.wYear = (WORD)y; ms.wMonth = (WORD)m; ms.wDay = 1;
        if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_YEARMONTH, &ms, NULL, buf, n, NULL))
            swprintf(buf, n, L"%04d-%02d", y, m);
    } else if (view == VIEW_YEAR) swprintf(buf, n, L"%d", y);
    else swprintf(buf, n, L"%d - %d", y - y % 10, y - y % 10 + 9);
}

static const char *view_name(int v) { return v == VIEW_MONTH ? "month" : v == VIEW_YEAR ? "year" : "decade"; }

/* the slide's state: 0..1000 of the way, eased */
static int anim_progress(void)
{
    DWORD t;
    double p;
    if (!g_anim_dir) return 1000;
    t = GetTickCount() - g_anim_start;
    if (t >= ANIM_MS) return 1000;
    p = (double)t / ANIM_MS;
    p = 1 - (1 - p) * (1 - p) * (1 - p);     /* ease out */
    return (int)(p * 1000);
}

/* one view's cells (the month's days, the year's months, the decade's years), dy px down */
static void paint_cells(HDC dc, const struct palette *p, int view, int y, int m, int dy, BOOL live)
{
    int ty, tm, td, i, n = cells_in_view(view);
    BOOL dark = p->text == RGB(0xFF, 0xFF, 0xFF);
    COLORREF accent = sg_accent();
    WCHAR buf[64];

    today(&ty, &tm, &td);
    for (i = 0; i < n; i++) {
        int cy, cm, cd = 0;
        BOOL is_today, in_scope, selected, focused, hot;
        RECT r = cell_rect_in(view, i, dy), in = r;
        if (view == VIEW_MONTH) {
            cell_date(y, m, i, &cy, &cm, &cd);
            is_today = cy == ty && cm == tm && cd == td;
            in_scope = cm == m;
            selected = g_sel_day && cy == g_sel_year && cm == g_sel_month && cd == g_sel_day;
            focused = cy == g_foc_year && cm == g_foc_month && cd == g_foc_day;
            swprintf(buf, ARRAYSIZE(buf), L"%d", cd);
        } else if (view == VIEW_YEAR) {
            item_of(view, y, i, &cy, &cm);
            is_today = cy == ty && cm == tm;
            in_scope = cy == y;
            selected = FALSE;
            focused = cy == g_foc_year && cm == g_foc_month;
            if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SABBREVMONTHNAME1 + cm - 1, buf, ARRAYSIZE(buf)))
                swprintf(buf, ARRAYSIZE(buf), L"%d", cm);
        } else {
            item_of(view, y, i, &cy, &cm);
            is_today = cy == ty;
            in_scope = cy / 10 == y / 10;
            selected = FALSE;
            focused = cy == g_foc_year;
            swprintf(buf, ARRAYSIZE(buf), L"%d", cy);
        }
        hot = live && i == g_hot;
        focused = focused && live && g_kbd;
        InflateRect(&in, -S(1), -S(1));
        if (g_w11) {
            /* round cells: today filled, the pointer's lightly, the chosen one ringed */
            if (is_today) disc(dc, &in, hot ? sg_accent_light(12) : accent);
            else if (hot) disc(dc, &in, p->hot);
            if (selected && !is_today) ring(dc, &in, accent, S(2));
            if (focused) ring(dc, &in, p->text, S(2));
        } else {
            if (is_today) {
                fill(dc, &in, accent);
                if (selected || hot || focused) frame(dc, &in, dark ? RGB(0, 0, 0) : RGB(0xFF, 0xFF, 0xFF), S(2));
            } else if (selected) frame(dc, &in, accent, S(2));
            if (focused && !is_today) {
                RECT f = in;
                InflateRect(&f, -S(selected ? 2 : 0), -S(selected ? 2 : 0));
                frame(dc, &f, p->text, S(selected ? 1 : 2));
            }
        }
        text_in(dc, view == VIEW_MONTH ? g_font_day : g_font_item, buf, r,
                is_today ? RGB(0xFF, 0xFF, 0xFF) : in_scope ? p->text : p->subtle, DT_CENTER);
    }
}

/* Windows 10's reveal: the cells' edges near the pointer lit, brightest nearest it, the
 * cell under it fully. On the 32-bit top-down bits of the whole flyout. */
static void reveal(DWORD *bits, int stride, int height, const struct palette *p)
{
    int i, n = cells_in_view(g_view), radius = S(96), x, y;
    double peak;
    BYTE rr = GetRValue(p->reveal), rg = GetGValue(p->reveal), rb = GetBValue(p->reveal);
#ifdef SG_MUTANT_CALENDAR_NO_REVEAL
    return;
#endif
    if (g_mouse.x < -500 || (g_w11)) return;
    for (i = 0; i < n; i++) {
        RECT r = cell_rect(i);
        int dx = g_mouse.x < r.left ? r.left - g_mouse.x : g_mouse.x >= r.right ? g_mouse.x - r.right + 1 : 0;
        int dy = g_mouse.y < r.top ? r.top - g_mouse.y : g_mouse.y >= r.bottom ? g_mouse.y - r.bottom + 1 : 0;
        int t = S(2);
        if (dx * dx + dy * dy >= radius * radius) continue;
        InflateRect(&r, -S(1), -S(1));
        peak = i == g_hot ? 0.75 : 0.45;
        for (y = r.top; y < r.bottom; y++) {
            BOOL row_edge = y < r.top + t || y >= r.bottom - t;
            if (y < 0 || y >= height) continue;
            for (x = r.left; x < r.right; x++) {
                double d, a;
                DWORD *px;
                if (!row_edge && x >= r.left + t && x < r.right - t) { x = r.right - t - 1; continue; }
                if (x < 0 || x >= stride) continue;
                d = sqrt((double)(x - g_mouse.x) * (x - g_mouse.x) + (double)(y - g_mouse.y) * (y - g_mouse.y));
                if (d >= radius) continue;
                a = peak * (1 - d / radius);
                if (i == g_hot) a = a < 0.35 ? 0.35 : a;
                px = bits + y * stride + x;
                {
                    BYTE b0 = *px & 0xff, g0 = (*px >> 8) & 0xff, r0 = (*px >> 16) & 0xff;
                    r0 = (BYTE)(r0 + (rr - r0) * a);
                    g0 = (BYTE)(g0 + (rg - g0) * a);
                    b0 = (BYTE)(b0 + (rb - b0) * a);
                    *px = (DWORD)b0 | ((DWORD)g0 << 8) | ((DWORD)r0 << 16);
                }
            }
        }
    }
}

static void dump_state(void);

static void fly_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps), dc;
    BOOL dark = sg_system_dark();
    struct palette p;
    COLORREF linkc = dark ? sg_accent_shade(RGB(0xB9, 0x8C, 0xF0)) : sg_accent();
    RECT c, r;
    WCHAR buf[128];
    SYSTEMTIME st;
    HBITMAP bmp, oldbmp;
    BITMAPINFO bi = { 0 };
    DWORD *bits = NULL;
    int i, first = first_weekday(), prog = anim_progress();

    palette(&p, dark);
    GetClientRect(hwnd, &c);
    /* drawn off screen, then copied: the clock ticks once a second, without a flicker */
    dc = CreateCompatibleDC(wdc);
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = c.right;
    bi.bmiHeader.biHeight = -c.bottom;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
    if (!bmp) bmp = CreateCompatibleBitmap(wdc, c.right, c.bottom), bits = NULL;
    oldbmp = SelectObject(dc, bmp);
    fill(dc, &c, p.bg);
    SetBkMode(dc, TRANSPARENT);
    GetLocalTime(&st);

    if (g_w11) {
        /* the day's date, and the chevron that folds the month away */
        WCHAR fmt[80] = L"dddd, ", md[64] = L"";
        if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SMONTHDAY, md, ARRAYSIZE(md)) || !md[0]) lstrcpyW(md, L"MMMM d");
        lstrcatW(fmt, md);
        if (!GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, fmt, buf, ARRAYSIZE(buf), NULL))
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, NULL, buf, ARRAYSIZE(buf), NULL);
        text_in(dc, g_font_month, buf, date_rect(), p.text, DT_LEFT);
        r = chevron_rect();
        if (g_hot_part == HOT_CHEVRON) { RECT h = r; fill(dc, &h, p.hot); }
        chevron(dc, r, g_folded, p.text);
        if (!g_folded) { SetRect(&r, 0, S(W11_HEAD_H), c.right, S(W11_HEAD_H) + 1); fill(dc, &r, p.line); }
    } else {
        /* the time, with its seconds, and the long date */
        GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, NULL, buf, ARRAYSIZE(buf));
        SetRect(&r, S(22), S(14), S(W10_W - 16), S(70));
        text_in(dc, g_font_big, buf, r, p.text, DT_LEFT);
        GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &st, NULL, buf, ARRAYSIZE(buf), NULL);
        link(dc, buf, date_rect(), linkc, g_hot_part == HOT_DATE, DT_LEFT);
        SetRect(&r, 0, S(112), c.right, S(112) + 1);
        fill(dc, &r, p.line);
    }

    if (!(g_w11 && g_folded)) {
        /* the view's title (a link to the larger view), and its arrows */
        view_title(g_view, g_year, g_month, buf, ARRAYSIZE(buf));
        text_in(dc, g_font_month, buf, title_rect(), g_view != VIEW_DECADE && g_hot_part == HOT_TITLE ? p.subtle : p.text, DT_LEFT);
        r = up_rect();
        if (g_hot_part == HOT_UP) fill(dc, &r, p.hot);
        chevron(dc, r, TRUE, p.text);
        r = down_rect();
        if (g_hot_part == HOT_DOWN) fill(dc, &r, p.hot);
        chevron(dc, r, FALSE, p.text);

        /* the days of the week, from the locale's first */
        if (g_view == VIEW_MONTH) {
            for (i = 0; i < 7; i++) {
                int wd = (first + i) % 7;   /* 0 Sunday */
                WCHAR name[16] = L"";
                /* LOCALE_SSHORTESTDAYNAME1 is Monday ... 7 Sunday */
                if (!GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SSHORTESTDAYNAME1 + (wd + 6) % 7, name, ARRAYSIZE(name)) || !name[0]) {
                    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SABBREVDAYNAME1 + (wd + 6) % 7, name, ARRAYSIZE(name));
                    name[2] = 0;
                }
                r = rc(body_x() + i * cell_w(), body_y(), body_x() + (i + 1) * cell_w(), body_y() + weekdays_h() - 2);
                text_in(dc, g_font_day, name, r, p.text, DT_CENTER);
            }
        }

        /* the cells; while sliding, the view before it too, both inside the slide's part */
        {
            RECT s = slide_rect(g_view);
            HRGN clip = CreateRectRgnIndirect(&s);
            int h = s.bottom - s.top;
            SelectClipRgn(dc, clip);
            if (prog < 1000) {
                int off = MulDiv(h, prog, 1000);
                g_slide_frames++;
                paint_cells(dc, &p, g_prev_view, g_prev_year, g_prev_month, -g_anim_dir * off, FALSE);
                paint_cells(dc, &p, g_view, g_year, g_month, g_anim_dir * (h - off), FALSE);
            } else paint_cells(dc, &p, g_view, g_year, g_month, 0, TRUE);
            SelectClipRgn(dc, NULL);
            DeleteObject(clip);
        }
        if (bits && prog >= 1000) reveal(bits, c.right, c.bottom, &p);
    }

    if (!g_w11) {
        SetRect(&r, 0, S(W10_H - 56), c.right, S(W10_H - 56) + 1);
        fill(dc, &r, p.line);
        link(dc, L"Date and time settings", settings_rect(), linkc, g_hot_part == HOT_SETTINGS, DT_LEFT);
    }
    /* the edge: Windows 10's flyouts a thin grey line (Windows 11's is round, the frame's) */
    frame(dc, &c, p.border, 1);

    BitBlt(wdc, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
    dump_state();
}

/* ---- the views ------------------------------------------------------------------------------- */

static void set_view(int view, int y, int m, int dir)
{
    while (m < 1) { m += 12; y--; }
    while (m > 12) { m -= 12; y++; }
    if (y < 1601) { y = 1601; m = 1; }
    if (y > 9999) { y = 9999; m = 12; }
#ifdef SG_MUTANT_CALENDAR_NO_SLIDE
    dir = 0;
#endif
    if (dir && g_fly && IsWindowVisible(g_fly) && (view != g_view || y != g_year || m != g_month)) {
        g_prev_view = g_view; g_prev_year = g_year; g_prev_month = g_month;
        g_anim_dir = dir;
        g_anim_start = GetTickCount();
        SetTimer(g_fly, ANIM_TIMER, 15, NULL);
    } else g_anim_dir = 0;
    g_view = view; g_year = y; g_month = m;
    g_hot = -1;
    if (g_fly) InvalidateRect(g_fly, NULL, FALSE);
}

static void show_month(int y, int m)
{
    int ny = y, nm = m, dir;
    while (nm < 1) { nm += 12; ny--; }
    while (nm > 12) { nm -= 12; ny++; }
    dir = g_view != VIEW_MONTH ? 0 : ny * 12 + nm > g_year * 12 + g_month ? 1 : ny * 12 + nm < g_year * 12 + g_month ? -1 : 0;
    set_view(VIEW_MONTH, ny, nm, dir);
}

/* the arrows, Page Up / Page Down, the wheel: the view's step back or on */
static void step(int n)
{
    if (g_w11 && g_folded) return;
    if (g_view == VIEW_MONTH) {
        show_month(g_year, g_month + n);
        /* the keyboard's day follows into that month, as on Windows */
        if (g_foc_day) {
            int y = g_year, m = g_month, d = min(g_foc_day, days_in(y, m));
            g_foc_year = y; g_foc_month = m; g_foc_day = d;
        }
    } else if (g_view == VIEW_YEAR) {
        g_foc_year += n;    /* (the focus goes along, as the day's does) */
        set_view(VIEW_YEAR, g_year + n, g_month, n > 0 ? 1 : -1);
    } else {
        g_foc_year += 10 * n;
        set_view(VIEW_DECADE, g_year - g_year % 10 + 10 * n, g_month, n > 0 ? 1 : -1);
    }
}

/* the month's name: its year; the year: its decade (Windows' zoom out) */
static void zoom_out(void)
{
#ifdef SG_MUTANT_CALENDAR_NO_YEARVIEW
    return;
#endif
    if (g_view == VIEW_MONTH) {
        if (!(g_foc_year == g_year && g_foc_month)) { g_foc_year = g_year; g_foc_month = g_month; }
        set_view(VIEW_YEAR, g_year, g_month, 0);
    } else if (g_view == VIEW_YEAR) {
        g_foc_year = g_year;
        set_view(VIEW_DECADE, g_year, g_month, 0);
    }
}

/* a year's month or a decade's year chosen: that one, a view closer */
static void zoom_in(int y, int m)
{
    if (g_view == VIEW_YEAR) {
        g_foc_year = y; g_foc_month = m; g_foc_day = min(max(g_foc_day, 1), days_in(y, m));
        set_view(VIEW_MONTH, y, m, 0);
    } else if (g_view == VIEW_DECADE) {
        g_foc_year = y; if (!g_foc_month) g_foc_month = 1;
        set_view(VIEW_YEAR, y, g_month, 0);
    }
}

static void choose_day(int y, int m, int d)
{
    g_sel_year = y; g_sel_month = m; g_sel_day = d;
    g_foc_year = y; g_foc_month = m; g_foc_day = d;
    /* a day of the month before or after: that month, as on Windows */
    if (m != g_month || y != g_year) show_month(y, m);
    InvalidateRect(g_fly, NULL, FALSE);
}

/* the arrows: the keyboard's day (month, year) dx across, dy down */
static void move_focus(int dx, int dy)
{
#ifdef SG_MUTANT_CALENDAR_ARROWS_DEAD
    return;
#endif
    g_kbd = TRUE;
    if (g_w11 && g_folded) return;
    if (g_view == VIEW_MONTH) {
        SYSTEMTIME st = { 0 };
        FILETIME ft;
        ULARGE_INTEGER u;
        if (!g_foc_day) { int y, m, d; today(&y, &m, &d); g_foc_year = y; g_foc_month = m; g_foc_day = d; }
        st.wYear = (WORD)g_foc_year; st.wMonth = (WORD)g_foc_month; st.wDay = (WORD)g_foc_day;
        SystemTimeToFileTime(&st, &ft);
        u.LowPart = ft.dwLowDateTime; u.HighPart = ft.dwHighDateTime;
        u.QuadPart += (LONGLONG)(dx + 7 * dy) * 864000000000LL;
        ft.dwLowDateTime = u.LowPart; ft.dwHighDateTime = u.HighPart;
        if (!FileTimeToSystemTime(&ft, &st) || st.wYear < 1601 || st.wYear > 9999) return;
        g_foc_year = st.wYear; g_foc_month = st.wMonth; g_foc_day = st.wDay;
        if (g_foc_month != g_month || g_foc_year != g_year) show_month(g_foc_year, g_foc_month);
    } else if (g_view == VIEW_YEAR) {
        int idx = (g_foc_year * 12 + (g_foc_month ? g_foc_month : 1) - 1) + dx + 4 * dy;
        g_foc_year = idx / 12; g_foc_month = idx % 12 + 1;
        if (g_foc_year != g_year) set_view(VIEW_YEAR, g_foc_year, g_month, g_foc_year > g_year ? 1 : -1);
    } else {
        g_foc_year += dx + 4 * dy;
        if (g_foc_year / 10 != g_year / 10) set_view(VIEW_DECADE, g_foc_year, g_month, g_foc_year > g_year ? 1 : -1);
    }
    InvalidateRect(g_fly, NULL, FALSE);
}

static void dump_state(void)
{
    const WCHAR *path = _wgetenv(L"SG_CALENDAR_DUMP");
    FILE *f;
    int ty, tm, td, i, y, m, d;
    WCHAR title[64];
    if (!path || !*path || !(f = _wfopen(path, L"w"))) return;
    today(&ty, &tm, &td);
    fprintf(f, "visible %d\nmonth %04d-%02d\nfirstday %d\ntoday %04d-%02d-%02d\nselected %04d-%02d-%02d\n",
            IsWindowVisible(g_fly) ? 1 : 0, g_year, g_month, first_weekday(), ty, tm, td, g_sel_year, g_sel_month, g_sel_day);
    fprintf(f, "view %s\nfocus %04d-%02d-%02d\nlook %s\nfolded %d\nedge %d\nacrylic %u\nslides %d\ndpi %d\n",
            view_name(g_view), g_foc_year, g_foc_month, g_foc_day, g_w11 ? "w11" : "w10", g_folded, g_edge, g_acrylic,
            g_slide_frames, g_dpi);
    view_title(g_view, g_year, g_month, title, ARRAYSIZE(title));
    fprintf(f, "title %ls\n", title);
    fprintf(f, "cells");
    for (i = 0; i < 42; i++) { cell_date(g_year, g_month, i, &y, &m, &d); fprintf(f, " %s%d", m == g_month ? "" : "~", d); }
    fprintf(f, "\n");
    {
        RECT w;
        GetWindowRect(g_fly, &w);
        fprintf(f, "rect %ld %ld %ld %ld\n", w.left, w.top, w.right, w.bottom);
    }
    fclose(f);
}

/* ---- showing and hiding ---------------------------------------------------------------------- */

static UINT monitor_dpi(HMONITOR mon)
{
    typedef HRESULT (WINAPI *fn)(HMONITOR, int, UINT *, UINT *);
    static fn get;
    static BOOL looked;
    UINT x = 0, y = 0;
    if (!looked) {
        HMODULE sh = LoadLibraryW(L"shcore.dll");
        looked = TRUE;
        if (sh) get = (fn)(void *)GetProcAddress(sh, "GetDpiForMonitor");
    }
    if (get && mon && SUCCEEDED(get(mon, 0 /* MDT_EFFECTIVE_DPI */, &x, &y)) && y >= 96) return y;
    return sg_dpi_for(NULL);
}

/* The bar whose clock was clicked, its display and its edge: the flyout goes against that
 * bar, at the clock's end (Windows 10: flush with the bar and the screen's edge; Windows
 * 11: 12 px off both). The main bar when none was named (an older taskbar). */
static void place_flyout(void)
{
    HWND bar = g_bar && IsWindow(g_bar) ? g_bar : FindWindowW(L"Shell_TrayWnd", NULL);
    MONITORINFO mi = { sizeof(mi) };
    RECT b, mon, work;
    int x, y, w, h, gap;
    UINT dpi;

#ifdef SG_MUTANT_CALENDAR_MAIN_ONLY
    bar = NULL;
#endif
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    SetRect(&mon, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    if (bar && IsWindowVisible(bar) && GetWindowRect(bar, &b) && b.right - b.left > 1 && b.bottom - b.top > 1 &&
        GetMonitorInfoW(MonitorFromRect(&b, MONITOR_DEFAULTTONEAREST), &mi)) {
        mon = mi.rcMonitor;
        work = mi.rcWork;
        if (b.right - b.left >= b.bottom - b.top)
            g_edge = b.top - mon.top < mon.bottom - b.bottom ? ABE_TOP : ABE_BOTTOM;
        else
            g_edge = b.left - mon.left < mon.right - b.right ? ABE_LEFT : ABE_RIGHT;
    } else {
        /* no bar: the work area's bottom right */
        g_edge = ABE_BOTTOM;
        SetRect(&b, work.left, work.bottom, work.right, mon.bottom > work.bottom ? mon.bottom : work.bottom);
        bar = NULL;
    }
    g_shown_bar = bar;
    dpi = monitor_dpi(MonitorFromRect(&b, MONITOR_DEFAULTTONEAREST));
    if ((int)dpi != g_dpi) { g_dpi = (int)dpi; make_fonts(); }
    w = fly_w(); h = fly_h();
    gap = g_w11 ? S(W11_MARGIN) : 0;
    switch (g_edge)
    {
    case ABE_TOP:
        x = min(b.right, mon.right) - w - gap; y = b.bottom + gap; break;
    case ABE_LEFT:
        x = b.right + gap; y = min(b.bottom, mon.bottom) - h - gap; break;
    case ABE_RIGHT:
        x = b.left - w - gap; y = min(b.bottom, mon.bottom) - h - gap; break;
    default:
        x = min(b.right, mon.right) - w - gap; y = b.top - h - gap; break;
    }
    /* inside the display */
    if (x + w > mon.right) x = mon.right - w;
    if (x < mon.left) x = mon.left;
    if (y + h > mon.bottom) y = mon.bottom - h;
    if (y < mon.top) y = mon.top;
    SetWindowPos(g_fly, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
}

/* "Transparency effects" (Personalization > Colors): frosted, as Windows' flyouts are --
 * the desktop's compositor blurs what is below (wine-sg 0745) */
static void set_frost(void)
{
    DWORD on = reg_dword(L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"EnableTransparency", 1);
    g_acrylic = on ? (g_w11 ? 90 : 85) : 0;
#ifdef SG_MUTANT_CALENDAR_NO_FROST
    g_acrylic = 0;
#endif
    if (g_acrylic) SetPropW(g_fly, L"__wine_sg_acrylic", (HANDLE)(UINT_PTR)g_acrylic);
    else RemovePropW(g_fly, L"__wine_sg_acrylic");
}

static void show_flyout(void)
{
    int y, m, d;
    load_look();
    today(&y, &m, &d);
    g_sel_day = 0;
    g_foc_year = y; g_foc_month = m; g_foc_day = d;
    g_kbd = FALSE;
    g_anim_dir = 0;
    g_view = VIEW_MONTH;
    g_mouse.x = g_mouse.y = -1000;
    show_month(y, m);       /* opens on this month, as on Windows */
    set_frost();
    sg_round_corners(g_fly);
    place_flyout();
    SetWindowPos(g_fly, 0, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(g_fly, SW_SHOW);
    SetForegroundWindow(g_fly);
    SetFocus(g_fly);
    SetTimer(g_fly, TICK_TIMER, 1000, NULL);
    InvalidateRect(g_fly, NULL, FALSE);
    dump_state();
}

static void hide_flyout(void)
{
    if (!IsWindowVisible(g_fly)) return;
    ShowWindow(g_fly, SW_HIDE);
    KillTimer(g_fly, TICK_TIMER);
    KillTimer(g_fly, ANIM_TIMER);
    g_anim_dir = 0;
    dump_state();
}

/* The press of a second click on the clock takes the activation from the flyout, which
 * hides; the click's release then asked to open it again. A deactivation by a press on
 * a taskbar's clock (the bar's clock end) leaves it closed for that click from that bar
 * (sg-flyout.h does the same for tray icons). The bar pressed, or NULL. */
static HWND pressed_on_clock(void)
{
    HWND under, root;
    WCHAR cls[64];
    RECT r;
    POINT pt;
    if (!(GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000)) return NULL;
    if (!GetCursorPos(&pt) || !(under = WindowFromPoint(pt)) || !(root = GetAncestor(under, GA_ROOT))) return NULL;
    if (!GetClassNameW(root, cls, ARRAYSIZE(cls)) ||
        (lstrcmpW(cls, L"Shell_TrayWnd") && lstrcmpW(cls, L"Shell_SecondaryTrayWnd"))) return NULL;
    if (!GetWindowRect(root, &r) || !PtInRect(&r, pt)) return NULL;
    if (r.right - r.left > r.bottom - r.top ? pt.x >= r.right - S(120) : pt.y >= r.bottom - S(80)) return root;
    return NULL;
}

static void toggle(HWND bar)
{
    if (IsWindowVisible(g_fly)) {
#ifndef SG_MUTANT_CALENDAR_NOTOGGLE
        /* another display's clock: the calendar goes there (its bar takes no activation) */
        if (bar && g_shown_bar && bar != g_shown_bar && IsWindow(bar)) { g_bar = bar; show_flyout(); return; }
        hide_flyout();
#endif
        return;
    }
#ifndef SG_MUTANT_CALENDAR_REOPENS
    /* (a press on another display's clock: it opens there) */
    if (g_hidden_bar && GetTickCount() - g_hidden_tick < 3000 &&
        (!bar || (bar == g_hidden_bar && (!g_hidden_from || g_hidden_from == bar)))) {
        g_hidden_bar = NULL;
        return;
    }
#endif
    g_hidden_bar = NULL;
    g_bar = bar;
    show_flyout();
}

static void open_settings(void)
{
    hide_flyout();
    if ((INT_PTR)ShellExecuteW(NULL, NULL, L"ms-settings:dateandtime", NULL, NULL, SW_SHOWNORMAL) <= 32)
        ShellExecuteW(NULL, NULL, L"control.exe", L"timedate.cpl", NULL, SW_SHOWNORMAL);
}

static void set_folded(BOOL folded)
{
    HKEY k;
    DWORD v = folded;
    g_folded = folded;
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, CAL_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) {
        RegSetValueExW(k, L"Folded", 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
        RegCloseKey(k);
    }
    g_hot = -1;
    place_flyout();
    InvalidateRect(g_fly, NULL, FALSE);
}

static LRESULT CALLBACK fly_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT: fly_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DPICHANGED:
        g_dpi = (int)sg_dpi_new(wp);
        make_fonts();
        sg_dpi_apply_rect(hwnd, lp);
        place_flyout();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_TIMER:
        if (wp == TICK_TIMER) {
            RECT r = { 0, 0, fly_w(), S(g_w11 ? W11_HEAD_H : 112) };
            InvalidateRect(hwnd, &r, FALSE);
        } else if (wp == ANIM_TIMER) {
            if (anim_progress() >= 1000) { KillTimer(hwnd, ANIM_TIMER); g_anim_dir = 0; }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && IsWindowVisible(hwnd)) {
            g_hidden_bar = pressed_on_clock();
            g_hidden_from = g_shown_bar;
            g_hidden_tick = GetTickCount();
            hide_flyout();
        }
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
    {
        BOOL ctrl = GetKeyState(VK_CONTROL) < 0;
        switch (wp)
        {
        case VK_ESCAPE: hide_flyout(); break;
        case VK_PRIOR: step(-1); break;
        case VK_NEXT: step(1); break;
        case VK_HOME:
        {
            int y, m, d;
            today(&y, &m, &d);
            g_foc_year = y; g_foc_month = m; g_foc_day = d;
            g_kbd = TRUE;
            if (g_view != VIEW_MONTH) set_view(VIEW_MONTH, y, m, 0); else show_month(y, m);
            InvalidateRect(hwnd, NULL, FALSE);
            break;
        }
        case VK_LEFT: move_focus(-1, 0); break;
        case VK_RIGHT: move_focus(1, 0); break;
        case VK_UP:
            if (ctrl) { g_kbd = TRUE; zoom_out(); }
            else move_focus(0, -1);
            break;
        case VK_DOWN:
            if (ctrl) { g_kbd = TRUE; if (g_view == VIEW_YEAR) zoom_in(g_foc_year, g_foc_month ? g_foc_month : 1);
                        else if (g_view == VIEW_DECADE) zoom_in(g_foc_year, 0); }
            else move_focus(0, 1);
            break;
        case VK_RETURN:
        case VK_SPACE:
            g_kbd = TRUE;
            if (g_view == VIEW_MONTH && g_foc_day) choose_day(g_foc_year, g_foc_month, g_foc_day);
            else if (g_view == VIEW_YEAR) zoom_in(g_foc_year, g_foc_month ? g_foc_month : 1);
            else if (g_view == VIEW_DECADE) zoom_in(g_foc_year, 0);
            break;
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        g_wheel += (short)HIWORD(wp);
        while (g_wheel >= WHEEL_DELTA) { g_wheel -= WHEEL_DELTA; step(-1); }
        while (g_wheel <= -WHEEL_DELTA) { g_wheel += WHEEL_DELTA; step(1); }
        return 0;
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int cell = cell_at(pt), part = part_at(pt);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        g_mouse = pt;
        if (cell != g_hot || part != g_hot_part || !g_w11) {
            /* (Windows 10's reveal follows the pointer: the cells near it repaint) */
            g_hot = cell; g_hot_part = part;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        SetCursor(LoadCursorW(NULL, (const WCHAR *)(part == HOT_DATE || part == HOT_SETTINGS ? IDC_HAND : IDC_ARROW)));
        return 0;
    }
    case WM_MOUSELEAVE:
        g_mouse.x = g_mouse.y = -1000;
        g_hot = -1; g_hot_part = HOT_NONE;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int cell = cell_at(pt);
        switch (part_at(pt))
        {
        case HOT_UP: step(-1); return 0;
        case HOT_DOWN: step(1); return 0;
        case HOT_TITLE: zoom_out(); return 0;
        case HOT_CHEVRON: set_folded(!g_folded); return 0;
        case HOT_DATE:
        case HOT_SETTINGS:
            open_settings();
            return 0;
        }
        if (cell >= 0) {
            int y, m, d;
            if (g_view == VIEW_MONTH) {
                cell_date(g_year, g_month, cell, &y, &m, &d);
                choose_day(y, m, d);
            } else {
                item_of(g_view, g_year, cell, &y, &m);
                zoom_in(y, m);
            }
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_toggle_msg && g_toggle_msg) { toggle((HWND)lp); return 0; }
    if (sg_mode_changed(msg, lp)) {
        if (IsWindowVisible(g_fly)) { load_look(); set_frost(); place_flyout(); InvalidateRect(g_fly, NULL, FALSE); }
        return 0;
    }
    if (msg == WM_TIMECHANGE && IsWindowVisible(g_fly)) { InvalidateRect(g_fly, NULL, FALSE); return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HFONT make_font(int height, int weight)
{
    return CreateFontW(-S(height), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void make_fonts(void)
{
    if (g_font) DeleteObject(g_font);
    if (g_font_big) DeleteObject(g_font_big);
    if (g_font_month) DeleteObject(g_font_month);
    if (g_font_day) DeleteObject(g_font_day);
    if (g_font_item) DeleteObject(g_font_item);
    g_font = make_font(14, FW_NORMAL);
    g_font_big = make_font(42, FW_LIGHT);
    g_font_month = make_font(g_w11 ? 14 : 15, FW_SEMIBOLD);
    g_font_day = make_font(g_w11 ? 13 : 14, FW_NORMAL);
    g_font_item = make_font(15, FW_NORMAL);
}

/* owned by the (never shown) host: no taskbar button of its own */
static void create_flyout(void)
{
    g_fly = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"SgCalendarFlyout", L"Calendar", WS_POPUP,
                            0, 0, fly_w(), fly_h(), g_host, NULL, GetModuleHandleW(NULL), NULL);
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

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSW wc = { 0 };
    MSG msg;
    HWND running, bar = NULL;
    BOOL show_now = cmdline && wcsstr(cmdline, L"--show") != NULL;
    const WCHAR *dump = cmdline ? wcsstr(cmdline, L"--dump") : NULL, *bar_arg = cmdline ? wcsstr(cmdline, L"--bar") : NULL;

    (void)prev; (void)show;
    if (dump) {
        /* the month's grid, for the gate: --dump YYYY-MM */
        int y = 0, m = 0, i, cy, cm, cd;
        swscanf(dump + 6, L" %d-%d", &y, &m);
        if (y < 1601 || m < 1 || m > 12) return 2;
        printf("firstday %d\ncells", first_weekday());
        for (i = 0; i < 42; i++) { cell_date(y, m, i, &cy, &cm, &cd); printf(" %s%d", cm == m ? "" : "~", cd); }
        printf("\n");
        return 0;
    }
    if (bar_arg) bar = (HWND)(ULONG_PTR)wcstoul(bar_arg + 5, NULL, 0);
    g_toggle_msg = RegisterWindowMessageW(L"SgCalendarToggle");
    if ((running = FindWindowW(L"SgCalendarHost", NULL))) {   /* one per session: that one opens or closes */
        pass_foreground(running);
        if (show_now) PostMessageW(running, g_toggle_msg, 0, (LPARAM)bar);
        return 0;
    }
    sg_dpi_init();
    g_dpi = (int)sg_dpi_for(NULL);
    g_w11 = want_w11();
    make_fonts();

    wc.lpfnWndProc = host_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SgCalendarHost";
    RegisterClassW(&wc);
    g_host = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Calendar", WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    wc.lpfnWndProc = fly_proc;
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.lpszClassName = L"SgCalendarFlyout";
    RegisterClassW(&wc);
    create_flyout();
    {
        int y, m, d;
        today(&y, &m, &d);
        set_view(VIEW_MONTH, y, m, 0);
    }
    if (show_now) { g_bar = bar; show_flyout(); }
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
