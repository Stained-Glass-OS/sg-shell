/* sg-control -- Settings > System: Display, Sound, Notifications & actions,
 * Power & sleep, Storage, Multitasking, About.
 *
 * Where Windows keeps a setting, it is kept there (LogPixels, the
 * notification switches, the snap switch); what only the machine can do --
 * the compositor's outputs and colour temperature, the sound server, the
 * idle timers -- goes through sg-settingsctl, and renaming the PC through
 * the Control Panel's elevated dialogs and sg-admind.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"
#include <shlobj.h>

static const WCHAR DESKTOP[] = L"Control Panel\\Desktop";

/* ---- Display ------------------------------------------------------------------------------- */
enum {
    CMD_NIGHT = CMD_PAGE_FIRST + 1, CMD_NIGHT_STRENGTH, CMD_SCALE, CMD_RES, CMD_OUTPUT, CMD_KEEP, CMD_REVERT,
    CMD_ADVANCED, CMD_ORIENT, CMD_RLOCK, CMD_RATE, CMD_IDENTIFY, CMD_APPLY_LAYOUT, CMD_MAKEMAIN, CMD_PROJECT,
    CMD_ARRANGE,
};

/* ---- several displays (David 2026-11-12: "many monitors, more than two even, be able to
 * adjust where they are"): each one where the compositor has it (sg-settingsctl display:
 * PLACE), dragged in the box as on Windows and applied with Apply; the main display; extend /
 * duplicate / only one; the resolution, refresh rate and orientation of the one selected ---- */
struct disp {
    char name[64];
    WCHAR desc[128];
    int x, y, w, h;             /* in the layout (dragged: not applied yet) */
    BOOL on, main;
};
static struct disp g_disps[16];
static int g_ndisps, g_sel;
static BOOL g_layout_dirty;     /* dragged, not applied: kept over the page being built again */
static char g_project[80];      /* extend, duplicate, only<TAB>NAME */

static BOOL span_mode(void)
{
    /* the desktop over every monitor (wine-sg 7749): Wine follows the monitors itself */
    WCHAR v[8] = L"";
    DWORD size = sizeof(v);
    return !RegGetValueW(HKEY_CURRENT_USER, L"Software\\Wine\\Explorer", L"HostMonitors", RRF_RT_REG_SZ, NULL, v, &size) &&
           (v[0] == 'Y' || v[0] == 'y' || v[0] == '1');
}

/* names in their natural order: DP-2 before DP-10 -- the displays' numbers */
static int natcmp(const char *a, const char *b)
{
    while (*a && *b) {
        if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
            long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
            if (x != y) return x < y ? -1 : 1;
            continue;
        }
        if (*a != *b) return (unsigned char)*a - (unsigned char)*b;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static int disp_order(const void *a, const void *b)
{
    return natcmp(((const struct disp *)a)->name, ((const struct disp *)b)->name);
}

static void load_displays(const char *ans)
{
    const char *pos = NULL;
    char buf[512], keep[64] = "";
    struct disp old[16];
    int nold = g_ndisps, i;

    if (g_sel >= 0 && g_sel < g_ndisps) lstrcpynA(keep, g_disps[g_sel].name, sizeof(keep));
    memcpy(old, g_disps, sizeof(old));
    g_ndisps = 0;
    while (g_ndisps < (int)ARRAYSIZE(g_disps) && ctl_line(ans, "PLACE", &pos, buf, sizeof(buf))) {
        struct disp *d = &g_disps[g_ndisps];
        WCHAR f[64];
        ctl_field(buf, 0, f, ARRAYSIZE(f));
        WideCharToMultiByte(CP_UTF8, 0, f, -1, d->name, sizeof(d->name), NULL, NULL);
        ctl_field(buf, 1, f, ARRAYSIZE(f)); d->x = _wtoi(f);
        ctl_field(buf, 2, f, ARRAYSIZE(f)); d->y = _wtoi(f);
        ctl_field(buf, 3, f, ARRAYSIZE(f)); d->w = _wtoi(f);
        ctl_field(buf, 4, f, ARRAYSIZE(f)); d->h = _wtoi(f);
        ctl_field(buf, 5, f, ARRAYSIZE(f)); d->on = !lstrcmpW(f, L"yes");
        ctl_field(buf, 6, f, ARRAYSIZE(f)); d->main = !lstrcmpW(f, L"yes");
        if (d->w <= 0 || d->h <= 0) { d->w = 1280; d->h = 720; }
        d->desc[0] = 0;
        g_ndisps++;
    }
    qsort(g_disps, g_ndisps, sizeof(g_disps[0]), disp_order);
    /* each one's description (OUTPUT name<TAB>mode<TAB>scale<TAB>description) */
    pos = NULL;
    while (ctl_line(ans, "OUTPUT", &pos, buf, sizeof(buf))) {
        WCHAR f[64];
        char name[64];
        ctl_field(buf, 0, f, ARRAYSIZE(f));
        WideCharToMultiByte(CP_UTF8, 0, f, -1, name, sizeof(name), NULL, NULL);
        for (i = 0; i < g_ndisps; i++)
            if (!strcmp(g_disps[i].name, name)) ctl_field(buf, 3, g_disps[i].desc, ARRAYSIZE(g_disps[i].desc));
    }
    g_project[0] = 0;
    if (ctl_line(ans, "PROJECT", NULL, buf, sizeof(buf))) lstrcpynA(g_project, buf, sizeof(g_project));
    /* dragged and not applied: those places stay */
    if (g_layout_dirty)
        for (i = 0; i < g_ndisps; i++) {
            int j;
            for (j = 0; j < nold; j++)
                if (!strcmp(old[j].name, g_disps[i].name)) { g_disps[i].x = old[j].x; g_disps[i].y = old[j].y; }
        }
    g_sel = -1;
    for (i = 0; i < g_ndisps; i++) if (keep[0] && !strcmp(g_disps[i].name, keep)) g_sel = i;
    if (g_sel < 0) for (i = 0; i < g_ndisps; i++) if (g_disps[i].main) g_sel = i;
    if (g_sel < 0 && g_ndisps) g_sel = 0;
}

/* the box the displays are drawn in, and dragged */
static double g_ar_scale;
static int g_ar_ox, g_ar_oy, g_drag = -1, g_drag_dx, g_drag_dy;

static void arrange_geometry(const RECT *rc)
{
    int minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN, i;
    double sx, sy;
    for (i = 0; i < g_ndisps; i++) {
        minx = min(minx, g_disps[i].x); miny = min(miny, g_disps[i].y);
        maxx = max(maxx, g_disps[i].x + g_disps[i].w); maxy = max(maxy, g_disps[i].y + g_disps[i].h);
    }
    if (!g_ndisps) return;
    /* room around them to drag one past the others' edges */
    sx = (rc->right - S(40)) / (double)(maxx - minx + (maxx - minx) / 3);
    sy = (rc->bottom - S(30)) / (double)(maxy - miny + (maxy - miny) / 3);
    g_ar_scale = min(sx, sy);
    g_ar_ox = (rc->right - (int)((maxx - minx) * g_ar_scale)) / 2 - (int)(minx * g_ar_scale);
    g_ar_oy = (rc->bottom - (int)((maxy - miny) * g_ar_scale)) / 2 - (int)(miny * g_ar_scale);
}

static RECT disp_box(int i)
{
    RECT r;
    r.left = g_ar_ox + (int)(g_disps[i].x * g_ar_scale);
    r.top = g_ar_oy + (int)(g_disps[i].y * g_ar_scale);
    r.right = g_ar_ox + (int)((g_disps[i].x + g_disps[i].w) * g_ar_scale);
    r.bottom = g_ar_oy + (int)((g_disps[i].y + g_disps[i].h) * g_ar_scale);
    InflateRect(&r, -1, -1);
    return r;
}

static BOOL disp_overlaps(int i, int x, int y)
{
    int j;
    for (j = 0; j < g_ndisps; j++) {
        if (j == i || !g_disps[j].on) continue;
        if (x < g_disps[j].x + g_disps[j].w && g_disps[j].x < x + g_disps[i].w &&
            y < g_disps[j].y + g_disps[j].h && g_disps[j].y < y + g_disps[i].h) return TRUE;
    }
    return FALSE;
}

/* a display let go: against the nearest edge of another, not over any, as
 * Windows puts it (a display not touching the others has nowhere to be) */
static void disp_snap(int i)
{
    struct disp *d = &g_disps[i];
    int j, bx = d->x, by = d->y;
    long long best = -1;
#ifdef SG_MUTANT_ARRANGE_NO_SNAP
    return;
#endif
    for (j = 0; j < g_ndisps; j++) {
        struct disp *o = &g_disps[j];
        int cand[4][2], k;
        if (j == i || !o->on) continue;
        cand[0][0] = o->x - d->w;  cand[0][1] = max(o->y - d->h + 1, min(d->y, o->y + o->h - 1));
        cand[1][0] = o->x + o->w;  cand[1][1] = cand[0][1];
        cand[2][1] = o->y - d->h;  cand[2][0] = max(o->x - d->w + 1, min(d->x, o->x + o->w - 1));
        cand[3][1] = o->y + o->h;  cand[3][0] = cand[2][0];
        for (k = 0; k < 4; k++) {
            long long dx = cand[k][0] - d->x, dy = cand[k][1] - d->y, dist = dx * dx + dy * dy;
            /* lined up with the other's edge when it is close (tops, bottoms, lefts, rights) */
            if (k < 2) {
                if (abs(cand[k][1] - o->y) < d->h / 10) cand[k][1] = o->y;
                else if (abs(cand[k][1] + d->h - o->y - o->h) < d->h / 10) cand[k][1] = o->y + o->h - d->h;
            } else {
                if (abs(cand[k][0] - o->x) < d->w / 10) cand[k][0] = o->x;
                else if (abs(cand[k][0] + d->w - o->x - o->w) < d->w / 10) cand[k][0] = o->x + o->w - d->w;
            }
            if (disp_overlaps(i, cand[k][0], cand[k][1])) continue;
            if (best < 0 || dist < best) { best = dist; bx = cand[k][0]; by = cand[k][1]; }
        }
    }
    if (best >= 0) { d->x = bx; d->y = by; }
}

static LRESULT CALLBACK arrange_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
    RECT rc;
    int i;
    GetClientRect(hwnd, &rc);
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps), mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom), oldbmp = SelectObject(mem, bmp);
        HBRUSH b = CreateSolidBrush(COL_PANE);
        HFONT big = CreateFontW(-S(28), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT oldf;
        FillRect(mem, &rc, b);
        DeleteObject(b);
        if (g_drag < 0) arrange_geometry(&rc);
        oldf = SelectObject(mem, big);
        SetBkMode(mem, TRANSPARENT);
        for (i = 0; i < g_ndisps; i++) {
            RECT r = disp_box(i);
            WCHAR num[8];
            BOOL sel = i == g_sel;
            COLORREF fill = sel ? st_accent() : (g_dark ? RGB(0x5A, 0x5A, 0x5A) : RGB(0xB4, 0xB4, 0xB4));
            if (!g_disps[i].on) fill = g_dark ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xDC, 0xDC, 0xDC);
            b = CreateSolidBrush(fill);
            FillRect(mem, &r, b);
            DeleteObject(b);
            if (sel) { b = CreateSolidBrush(COL_TEXT); FrameRect(mem, &r, b); DeleteObject(b); }
            _snwprintf(num, ARRAYSIZE(num), L"%d", i + 1);
            SetTextColor(mem, sel ? RGB(0xFF, 0xFF, 0xFF) : COL_TEXT);
            DrawTextW(mem, num, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(mem, oldf);
        DeleteObject(big);
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldbmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        for (i = g_ndisps - 1; i >= 0; i--) {
            RECT r = disp_box(i);
            if (!PtInRect(&r, pt)) continue;
            g_sel = i;
            if (g_ndisps > 1 && g_disps[i].on && strncmp(g_project, "duplicate", 9)) {
                g_drag = i;
                g_drag_dx = pt.x - r.left;
                g_drag_dy = pt.y - r.top;
                SetCapture(hwnd);
            }
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        return 0;
    case WM_MOUSEMOVE:
        if (g_drag >= 0 && g_ar_scale > 0) {
            g_disps[g_drag].x = (int)((pt.x - g_drag_dx - g_ar_ox) / g_ar_scale);
            g_disps[g_drag].y = (int)((pt.y - g_drag_dy - g_ar_oy) / g_ar_scale);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_drag >= 0) {
            int was_x = g_disps[g_drag].x, was_y = g_disps[g_drag].y;
            ReleaseCapture();
            disp_snap(g_drag);
            (void)was_x; (void)was_y;
            g_drag = -1;
            g_layout_dirty = TRUE;
        }
        /* the page again: the selected display's resolution, orientation,
         * main display, and Apply for the new places */
        PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(CMD_ARRANGE, 0), (LPARAM)hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Identify: each display's number on it, for three seconds */
static LRESULT CALLBACK ident_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT rc;
        WCHAR num[8];
        HDC hdc = BeginPaint(hwnd, &ps);
        HFONT f = CreateFontW(-S(110), 0, 0, 0, FW_LIGHT, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
        HFONT old = SelectObject(hdc, f);
        HBRUSH b = CreateSolidBrush(RGB(0x20, 0x20, 0x20));
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, b);
        DeleteObject(b);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0xFF, 0xFF, 0xFF));
        _snwprintf(num, ARRAYSIZE(num), L"%d", (int)GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        DrawTextW(hdc, num, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old);
        DeleteObject(f);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_TIMER:
    case WM_LBUTTONDOWN:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void identify_displays(void)
{
    static BOOL reg;
    int i, mx = 0, my = 0;
    if (!reg) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = ident_proc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpszClassName = L"SgDisplayIdentify";
        reg = RegisterClassW(&wc) != 0;
    }
    /* the layout's places, from the main display's: Windows' screen
     * coordinates have the main display at 0,0 (as Wine has them) */
    for (i = 0; i < g_ndisps; i++) if (g_disps[i].main) { mx = g_disps[i].x; my = g_disps[i].y; }
    for (i = 0; i < g_ndisps; i++) {
        HWND w;
        if (!g_disps[i].on) continue;
        w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"SgDisplayIdentify", L"", WS_POPUP,
                            g_disps[i].x - mx + S(32), g_disps[i].y - my + g_disps[i].h - S(232), S(160), S(200),
                            NULL, NULL, g_inst, NULL);
        if (!w) continue;
        SetWindowLongPtrW(w, GWLP_USERDATA, i + 1);
        ShowWindow(w, SW_SHOWNOACTIVATE);
        SetTimer(w, 1, 3000, NULL);
    }
}

struct mode { int w, h; double hz; BOOL pref; WCHAR text[48]; char spec[48]; };
static struct mode g_modes[64];
static int g_nmodes, g_cur_mode = -1;
static BOOL g_modes_ctl;                    /* the compositor's modes, through sg-settingsctl */
static char g_output[64];
static WCHAR g_output_desc[128];
/* the output's orientation (the compositor's transform): Landscape, Portrait,
 * Landscape (flipped), Portrait (flipped) -- Portrait is the PC turned
 * clockwise, its left side up, as iio-sensor-proxy and GNOME have it */
static const char *const TRANSFORMS[] = { "normal", "90", "180", "270" };
static int g_orient;
static int g_revert_left;                   /* > 0: "keep these display settings?" is counting */
static int g_res_mode[64], g_nres;          /* each resolution's mode (its recommended one, else its first) */
static int g_rate_mode[64], g_nrate;        /* the refresh rates of the resolution in use */
static WCHAR g_rate_text[64][24];
static char g_prev_spec[48];
static DEVMODEW g_prev_dm;

static void load_modes(void)
{
    BOOL ok;
    WCHAR err[256];
    char *ans = ctl_run(L"display", &ok, err, ARRAYSIZE(err), 6000), buf[512];
    const char *pos = NULL;
    g_nmodes = 0; g_cur_mode = -1; g_modes_ctl = FALSE; g_output[0] = 0; g_output_desc[0] = 0;
    if (ans && ok) load_displays(ans);
    else g_ndisps = 0;
    if (ans && ok && ctl_line(ans, "OUTPUT", &pos, buf, sizeof(buf))) {
        char *tab = strchr(buf, '\t');
        if (tab) *tab = 0;
        lstrcpynA(g_output, buf, sizeof(g_output));
        if (tab) { *tab = '\t'; ctl_field(buf, 3, g_output_desc, ARRAYSIZE(g_output_desc)); }
        /* the display selected in the box, not the first */
        if (g_sel >= 0 && g_sel < g_ndisps) {
            lstrcpynA(g_output, g_disps[g_sel].name, sizeof(g_output));
            lstrcpynW(g_output_desc, g_disps[g_sel].desc, ARRAYSIZE(g_output_desc));
        }
        pos = NULL;
        while (g_nmodes < (int)ARRAYSIZE(g_modes) && ctl_line(ans, "MODE", &pos, buf, sizeof(buf))) {
            WCHAR name[64], spec[48], cur[8], pref[8];
            struct mode *m = &g_modes[g_nmodes];
            double hz = 0;
            ctl_field(buf, 0, name, ARRAYSIZE(name));
            ctl_field(buf, 1, spec, ARRAYSIZE(spec));
            ctl_field(buf, 2, cur, ARRAYSIZE(cur));
            ctl_field(buf, 3, pref, ARRAYSIZE(pref));
            if (lstrcmpA(g_output, "") && WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), NULL, NULL) && strcmp(buf, g_output)) continue;
            if (swscanf(spec, L"%dx%d@%lf", &m->w, &m->h, &hz) < 2) continue;
            m->hz = hz;
            m->pref = !lstrcmpW(pref, L"yes");
            WideCharToMultiByte(CP_UTF8, 0, spec, -1, m->spec, sizeof(m->spec), NULL, NULL);
            _snwprintf(m->text, ARRAYSIZE(m->text), L"%d \x00D7 %d%ls%ls", m->w, m->h,
                       hz > 0 ? L"" : L"", !lstrcmpW(pref, L"yes") ? L" (Recommended)" : L"");
            if (hz > 0) {
                WCHAR hzs[24];
                _snwprintf(hzs, ARRAYSIZE(hzs), L", %.0f Hz", hz);
                wcsncat(m->text, hzs, ARRAYSIZE(m->text) - lstrlenW(m->text) - 1);
            }
            if (!lstrcmpW(cur, L"yes")) g_cur_mode = g_nmodes;
            g_nmodes++;
        }
        g_modes_ctl = g_nmodes > 0;
        g_orient = 0;
        pos = NULL;
        while (ctl_line(ans, "TRANSFORM", &pos, buf, sizeof(buf))) {
            WCHAR name[64], t[8];
            char tn[8];
            int k;
            ctl_field(buf, 0, name, ARRAYSIZE(name));
            ctl_field(buf, 1, t, ARRAYSIZE(t));
            WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), NULL, NULL);
            if (strcmp(buf, g_output)) continue;
            WideCharToMultiByte(CP_UTF8, 0, t, -1, tn, sizeof(tn), NULL, NULL);
            for (k = 0; k < (int)ARRAYSIZE(TRANSFORMS); k++) if (!strcmp(tn, TRANSFORMS[k])) g_orient = k;
        }
    }
    free(ans);
    if (g_modes_ctl) return;
    /* Wine's own list: the display driver's modes */
    {
        DEVMODEW dm = { .dmSize = sizeof(dm) }, cur = { .dmSize = sizeof(cur) };
        DWORD i;
        EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &cur);
        for (i = 0; EnumDisplaySettingsW(NULL, i, &dm) && g_nmodes < (int)ARRAYSIZE(g_modes); i++) {
            int j, dup = 0;
            if (dm.dmBitsPerPel && dm.dmBitsPerPel < 24) continue;
            for (j = 0; j < g_nmodes; j++) if (g_modes[j].w == (int)dm.dmPelsWidth && g_modes[j].h == (int)dm.dmPelsHeight) dup = 1;
            if (dup) continue;
            g_modes[g_nmodes].w = dm.dmPelsWidth; g_modes[g_nmodes].h = dm.dmPelsHeight;
            _snwprintf(g_modes[g_nmodes].text, ARRAYSIZE(g_modes[0].text), L"%d \x00D7 %d", (int)dm.dmPelsWidth, (int)dm.dmPelsHeight);
            g_nmodes++;
        }
        for (i = 0; i < (DWORD)g_nmodes; i++)
            if (g_modes[i].w == (int)cur.dmPelsWidth && g_modes[i].h == (int)cur.dmPelsHeight) g_cur_mode = i;
        if (g_cur_mode < 0 && cur.dmPelsWidth && g_nmodes < (int)ARRAYSIZE(g_modes)) {
            g_modes[g_nmodes].w = cur.dmPelsWidth; g_modes[g_nmodes].h = cur.dmPelsHeight;
            _snwprintf(g_modes[g_nmodes].text, ARRAYSIZE(g_modes[0].text), L"%d \x00D7 %d", (int)cur.dmPelsWidth, (int)cur.dmPelsHeight);
            g_cur_mode = g_nmodes++;
        }
    }
}

/* Windows' steps; the last three only where they are recommended or below
 * (a 4K screen and larger), so a 1080p screen's list is as it was */
static const int SCALES[] = { 100, 125, 150, 175, 200, 225, 250, 300, 350, 400 };

void set_build_display(void)
{
    const WCHAR *items[64];
    WCHAR scales[ARRAYSIZE(SCALES)][32];
    const WCHAR *sitems[ARRAYSIZE(SCALES)];
    int y = st_title(L"Display"), i, dpi, sel = 0, rec = scale_recommended(), nscales = 0;
    BOOL ok, night = FALSE, avail = FALSE;
    int temp = 4000;
    char *ans, buf[256];
    HWND c;

    /* the displays, to select one and rearrange them (two or more) */
    load_modes();
    if (g_ndisps > 1) {
        static BOOL reg;
        HWND box;
        if (!reg) {
            WNDCLASSW wc = { 0 };
            wc.lpfnWndProc = arrange_proc;
            wc.hInstance = g_inst;
            wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
            wc.lpszClassName = L"SgDisplayArrange";
            reg = RegisterClassW(&wc) != 0;
        }
        y = st_head(y, L"Rearrange your displays");
        y = st_para(y, L"Select a display below to change the settings for it. Press and hold (or select) a display, "
                       L"then drag to rearrange it.");
        box = pg_control(L"SgDisplayArrange", L"", WS_TABSTOP, st_x(), y, min(st_w(), S(560)), S(200), CMD_ARRANGE);
        (void)box;
        y += S(212);
        pg_control(L"BUTTON", L"Identify", WS_TABSTOP | BS_PUSHBUTTON, st_x(), y, S(110), S(32), CMD_IDENTIFY);
        c = pg_control(L"BUTTON", L"Apply", WS_TABSTOP | BS_PUSHBUTTON, st_x() + S(122), y, S(110), S(32), CMD_APPLY_LAYOUT);
        EnableWindow(c, g_layout_dirty);
        y += S(46);
    }

    if (g_revert_left > 0) {
        WCHAR line[160];
        y = st_head(y, L"Keep these display settings?");
        _snwprintf(line, ARRAYSIZE(line), L"Reverting to previous display settings in %d seconds.", g_revert_left);
        pg_text(st_x(), y, st_w(), S(22), g_font_body, COL_TEXT, line, DT_SINGLELINE);
        y += S(34);
        st_button(&y, L"Keep changes", CMD_KEEP);
        y -= S(46);
        pg_control(L"BUTTON", L"Revert", WS_TABSTOP | BS_PUSHBUTTON, st_x() + S(150), y, S(120), S(32), CMD_REVERT);
        y += S(56);
        pg_timer(1000);
    }

    /* Night light: the compositor's colour temperature */
    y = st_head(y, L"Brightness and color");
    ans = ctl_run(L"nightlight", &ok, NULL, 0, 5000);
    if (ans && ok && ctl_line(ans, "NIGHTLIGHT", NULL, buf, sizeof(buf))) {
        WCHAR f[16];
        ctl_field(buf, 0, f, ARRAYSIZE(f)); night = !lstrcmpW(f, L"on");
        ctl_field(buf, 1, f, ARRAYSIZE(f)); temp = _wtoi(f);
        ctl_field(buf, 2, f, ARRAYSIZE(f)); avail = !lstrcmpW(f, L"yes");
    }
    free(ans);
    c = st_toggle(&y, L"Night light", night, CMD_NIGHT);
    EnableWindow(c, avail);
    if (!avail) y = st_para(y - S(8), L"Night light needs the Stained Glass compositor, and is not available in this session.");
    else {
        c = st_slider(&y, L"Strength", 0, 100, (6500 - temp) * 100 / 4600, CMD_NIGHT_STRENGTH);
        y = st_para(y - S(6), L"Night light shows warmer colors to help you sleep.");
    }

    /* Scale: LogPixels, which Windows programs read at start */
    y = st_head(y, L"Scale and layout");
    dpi = (int)reg_dword(HKEY_CURRENT_USER, DESKTOP, L"LogPixels", g_dpi);
    /* "(Recommended)": the scale that makes everything the share of this
     * screen it has on a 1080p screen at 100% (scale_for_screen) */
    for (i = 0; i < (int)ARRAYSIZE(SCALES) && (SCALES[i] <= 250 || SCALES[i] <= rec); i++) {
        _snwprintf(scales[i], ARRAYSIZE(scales[i]), L"%d%%%ls", SCALES[i], SCALES[i] == rec ? L" (Recommended)" : L"");
        sitems[i] = scales[i];
        if (MulDiv(SCALES[i], 96, 100) == dpi) sel = i;
        nscales++;
    }
    st_combo(&y, L"Change the size of text, apps, and other items", sitems, nscales, sel, CMD_SCALE);

    if (g_modes_ctl) {
        /* the resolutions, each once, and the refresh rates of the one in use
         * (David 2026-11-12: displays at different refresh rates -- each its own) */
        WCHAR rtext[ARRAYSIZE(g_modes)][32];
        const WCHAR *ritems[ARRAYSIZE(g_modes)];
        int nres = 0, nrate = 0, rsel = -1, csel = -1, j;
        for (i = 0; i < g_nmodes; i++) {
            for (j = 0; j < nres; j++)
                if (g_modes[g_res_mode[j]].w == g_modes[i].w && g_modes[g_res_mode[j]].h == g_modes[i].h) break;
            if (j < nres) { if (g_modes[i].pref) g_res_mode[j] = i; continue; }
            g_res_mode[nres++] = i;
        }
        for (i = 0; i < nres; i++) {
            struct mode *m = &g_modes[g_res_mode[i]];
            BOOL rec = FALSE;
            for (j = 0; j < g_nmodes; j++) if (g_modes[j].pref && g_modes[j].w == m->w && g_modes[j].h == m->h) rec = TRUE;
            _snwprintf(rtext[i], ARRAYSIZE(rtext[i]), L"%d \x00D7 %d%ls", m->w, m->h, rec ? L" (Recommended)" : L"");
            ritems[i] = rtext[i];
            if (g_cur_mode >= 0 && m->w == g_modes[g_cur_mode].w && m->h == g_modes[g_cur_mode].h) rsel = i;
        }
        g_nres = nres;
        c = st_combo(&y, L"Display resolution", ritems, nres, rsel, CMD_RES);
        for (i = 0; i < g_nmodes && g_cur_mode >= 0; i++) {
            if (g_modes[i].w != g_modes[g_cur_mode].w || g_modes[i].h != g_modes[g_cur_mode].h || g_modes[i].hz <= 0) continue;
            _snwprintf(g_rate_text[nrate], ARRAYSIZE(g_rate_text[0]), L"%.3g Hz", g_modes[i].hz);
            if (g_modes[i].hz >= 100) _snwprintf(g_rate_text[nrate], ARRAYSIZE(g_rate_text[0]), L"%.0f Hz", g_modes[i].hz);
            items[nrate] = g_rate_text[nrate];
            if (i == g_cur_mode) csel = nrate;
            g_rate_mode[nrate++] = i;
        }
        g_nrate = nrate;
        if (nrate) {
            c = st_combo(&y, L"Refresh rate", items, nrate, csel, CMD_RATE);
            if (nrate < 2) EnableWindow(c, FALSE);
        }
    } else {
        for (i = 0; i < g_nmodes; i++) items[i] = g_modes[i].text;
        c = st_combo(&y, L"Display resolution", items, g_nmodes, g_cur_mode, CMD_RES);
        if (!g_nmodes) EnableWindow(c, FALSE);
    }
    if (g_modes_ctl && g_output_desc[0]) {
        WCHAR line[200];
        _snwprintf(line, ARRAYSIZE(line), L"Display: %ls", g_output_desc);
        y = st_para(y - S(6), line);
    }
    /* orientation, and the rotation lock where the PC has an accelerometer
     * (David 2026-10-07: a tablet turns its screen, or not) */
    {
        static const WCHAR *ORIENTS[] = { L"Landscape", L"Portrait", L"Landscape (flipped)", L"Portrait (flipped)" };
        BOOL sensor = FALSE, locked = FALSE;
        HWND oc;
        ans = g_modes_ctl ? ctl_run(L"rotation", &ok, NULL, 0, 5000) : NULL;
        if (ans && ok && ctl_line(ans, "ROTATION", NULL, buf, sizeof(buf))) {
            WCHAR f[8];
            ctl_field(buf, 0, f, ARRAYSIZE(f)); locked = !lstrcmpW(f, L"yes");
            ctl_field(buf, 1, f, ARRAYSIZE(f)); sensor = !lstrcmpW(f, L"yes");
        }
        free(ans);
        oc = st_combo(&y, L"Display orientation", ORIENTS, ARRAYSIZE(ORIENTS), g_orient, CMD_ORIENT);
        if (!g_modes_ctl) EnableWindow(oc, FALSE);
        if (sensor) {
            st_toggle(&y, L"Rotation lock", locked, CMD_RLOCK);
#ifndef SG_MUTANT_ROTATION_ORIENT_WHILE_TURNING
            if (!locked) EnableWindow(oc, FALSE);
#endif
            y = st_para(y - S(8), locked ? L"The screen stays as it is when you turn this PC."
                                         : L"The screen turns as you turn this PC. Turn on rotation lock to keep it as it is.");
        }
    }
    y = st_head(y, L"Multiple displays");
    if (g_ndisps > 1) {
        WCHAR ptext[2 + ARRAYSIZE(g_disps)][40];
        const WCHAR *pitems[2 + ARRAYSIZE(g_disps)];
        int np = 0, psel = 1;
        lstrcpyW(ptext[np], L"Duplicate these displays"); pitems[np] = ptext[np]; np++;
        lstrcpyW(ptext[np], L"Extend these displays"); pitems[np] = ptext[np]; np++;
        if (!strncmp(g_project, "duplicate", 9)) psel = 0;
        for (i = 0; i < g_ndisps; i++) {
            _snwprintf(ptext[np], ARRAYSIZE(ptext[np]), L"Show only on %d", i + 1);
            pitems[np] = ptext[np];
            if (!strncmp(g_project, "only\t", 5) && !strcmp(g_project + 5, g_disps[i].name)) psel = np;
            np++;
        }
        st_combo(&y, L"Multiple displays", pitems, np, psel, CMD_PROJECT);
        if (g_sel >= 0 && g_sel < g_ndisps) {
            st_checkbox(&y, L"Make this my main display", g_disps[g_sel].main, CMD_MAKEMAIN);
            if (g_disps[g_sel].main) EnableWindow(GetDlgItem(g_page, CMD_MAKEMAIN), FALSE);
        }
        y = st_para(y, L"The taskbar is on the main display; Settings > Personalization > Taskbar shows it on all displays.");
    } else
        y = st_para(y, L"Older displays might not always connect automatically. Connect another display and it is added "
                       L"to the right of this one.");
    st_link(&y, L"Advanced display settings", CMD_ADVANCED);
}

/* The compositor's output changed size: Wine's desktop -- the shell, sized
 * when the session started -- takes the new size too. It stayed at the old
 * one, a smaller picture in the middle of the screen. */
void desktop_follow(int w, int h)
{
    DEVMODEW dm = { .dmSize = sizeof(dm) };
    int tries;
    if (w <= 0 || h <= 0) return;
    /* over every monitor Wine follows the compositor's outputs itself: one
     * mode the size of this one would make the main display that size */
    if (span_mode()) return;
    dm.dmPelsWidth = w; dm.dmPelsHeight = h;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    /* Wine learns the screen's new size a moment later (the X server's
     * RandR event): until then the size is not among its modes */
    for (tries = 0; tries < 30; tries++) {
        if (ChangeDisplaySettingsExW(NULL, &dm, NULL, CDS_UPDATEREGISTRY, NULL) == DISP_CHANGE_SUCCESSFUL) return;
        Sleep(200);
    }
}

static BOOL apply_mode(int i, BOOL remember)
{
    WCHAR args[128], err[256];
    BOOL ok;
    if (i < 0 || i >= g_nmodes) return FALSE;
    if (g_modes_ctl) {
        char *ans;
        if (remember && g_cur_mode >= 0) lstrcpyA(g_prev_spec, g_modes[g_cur_mode].spec);
        _snwprintf(args, ARRAYSIZE(args), L"display mode %S %S", g_output, g_modes[i].spec);
        ans = ctl_run(args, &ok, err, ARRAYSIZE(err), 10000);
        free(ans);
        if (!ok) { st_status(err[0] ? err : L"The display did not accept that resolution."); return FALSE; }
        desktop_follow(g_modes[i].w, g_modes[i].h);
        return TRUE;
    } else {
        DEVMODEW dm = { .dmSize = sizeof(dm) };
        if (remember) { g_prev_dm.dmSize = sizeof(g_prev_dm); EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &g_prev_dm); }
        dm.dmPelsWidth = g_modes[i].w; dm.dmPelsHeight = g_modes[i].h;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        if (ChangeDisplaySettingsExW(NULL, &dm, NULL, CDS_UPDATEREGISTRY, NULL) != DISP_CHANGE_SUCCESSFUL) {
            st_status(L"The display did not accept that resolution.");
            return FALSE;
        }
        return TRUE;
    }
}

static void revert_mode(void)
{
    g_revert_left = 0;
    KillTimer(g_page, 1);
    if (g_modes_ctl && g_prev_spec[0]) {
        WCHAR args[128];
        BOOL ok;
        _snwprintf(args, ARRAYSIZE(args), L"display mode %S %S", g_output, g_prev_spec);
        free(ctl_run(args, &ok, NULL, 0, 10000));
        if (ok) { int w = 0, h = 0; if (sscanf(g_prev_spec, "%dx%d", &w, &h) == 2) desktop_follow(w, h); }
    } else if (!g_modes_ctl && g_prev_dm.dmPelsWidth) {
        g_prev_dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
        ChangeDisplaySettingsExW(NULL, &g_prev_dm, NULL, CDS_UPDATEREGISTRY, NULL);
    }
    refresh_page();
}

void set_timer_display(void)
{
    if (g_revert_left <= 0) { KillTimer(g_page, 1); return; }
    if (--g_revert_left <= 0) revert_mode();
    else refresh_page();
}

BOOL set_cmd_display(int id, int code, HWND ctl)
{
    WCHAR args[64];
    BOOL ok;
    switch (id) {
    case CMD_NIGHT:
        _snwprintf(args, ARRAYSIZE(args), L"nightlight %ls", st_checked(ctl) ? L"on" : L"off");
        free(ctl_run(args, &ok, NULL, 0, 5000));
        refresh_page();
        return TRUE;
    case CMD_NIGHT_STRENGTH:
        if (code == (PG_SCROLL_CODE | TB_ENDTRACK)) {
            int pos = (int)SendMessageW(ctl, TBM_GETPOS, 0, 0);
            _snwprintf(args, ARRAYSIZE(args), L"nightlight --temp %d", 6500 - pos * 46);
            free(ctl_run(args, &ok, NULL, 0, 5000));
        }
        return TRUE;
    case CMD_SCALE:
        if (code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < (int)ARRAYSIZE(SCALES) && SCALES[i] != scale_current()) {
                scale_set(SCALES[i]);
                st_status(L"Some apps won't respond to scaling changes until you close and open them again.");
            }
        }
        return TRUE;
    case CMD_RES:
        if (code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (g_modes_ctl) {
                /* that resolution at the rate in use when it has it, else its recommended one */
                int j, m;
                if (i < 0 || i >= g_nres) return TRUE;
                m = g_res_mode[i];
                for (j = 0; j < g_nmodes && g_cur_mode >= 0; j++)
                    if (g_modes[j].w == g_modes[m].w && g_modes[j].h == g_modes[m].h &&
                        (int)(g_modes[j].hz + 0.5) == (int)(g_modes[g_cur_mode].hz + 0.5)) m = j;
                i = m;
            }
            if (i != g_cur_mode && apply_mode(i, TRUE)) { g_revert_left = 15; refresh_page(); }
        }
        return TRUE;
    case CMD_RATE:
        if (code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < g_nrate && g_rate_mode[i] != g_cur_mode && apply_mode(g_rate_mode[i], TRUE)) {
                g_revert_left = 15;
                refresh_page();
            }
        }
        return TRUE;
    case CMD_ARRANGE:
        refresh_page();
        return TRUE;
    case CMD_IDENTIFY:
        identify_displays();
        return TRUE;
    case CMD_APPLY_LAYOUT: {
        WCHAR a[1024], err[256];
        int i, n;
        n = _snwprintf(a, ARRAYSIZE(a), L"display layout");
        for (i = 0; i < g_ndisps && n > 0 && n < (int)ARRAYSIZE(a) - 100; i++)
            if (g_disps[i].on) n += _snwprintf(a + n, ARRAYSIZE(a) - n, L" %S %d %d", g_disps[i].name, g_disps[i].x, g_disps[i].y);
        free(ctl_run(a, &ok, err, ARRAYSIZE(err), 10000));
        if (!ok) st_status(err[0] ? err : L"The displays could not be rearranged.");
        g_layout_dirty = FALSE;
        refresh_page();
        return TRUE;
    }
    case CMD_MAKEMAIN:
        if (g_sel >= 0 && g_sel < g_ndisps && st_checked(ctl)) {
            WCHAR err[256];
            _snwprintf(args, ARRAYSIZE(args), L"display primary %S", g_disps[g_sel].name);
            free(ctl_run(args, &ok, err, ARRAYSIZE(err), 10000));
            if (!ok) st_status(err[0] ? err : L"That display could not be made the main display.");
            refresh_page();
        }
        return TRUE;
    case CMD_PROJECT:
        if (code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            WCHAR err[256];
            if (i == 0) lstrcpyW(args, L"display project duplicate");
            else if (i == 1) lstrcpyW(args, L"display project extend");
            else if (i - 2 < g_ndisps) _snwprintf(args, ARRAYSIZE(args), L"display project only %S", g_disps[i - 2].name);
            else return TRUE;
            g_layout_dirty = FALSE;
            free(ctl_run(args, &ok, err, ARRAYSIZE(err), 10000));
            if (!ok) st_status(err[0] ? err : L"The displays could not be changed.");
            refresh_page();
        }
        return TRUE;
    case CMD_ORIENT:
        if (code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            WCHAR err[256];
            if (i < 0 || i >= (int)ARRAYSIZE(TRANSFORMS) || i == g_orient || !g_output[0]) return TRUE;
            _snwprintf(args, ARRAYSIZE(args), L"display transform %S %S", g_output, TRANSFORMS[i]);
            free(ctl_run(args, &ok, err, ARRAYSIZE(err), 10000));
            if (!ok) { st_status(err[0] ? err : L"The display could not be turned."); return TRUE; }
            /* the desktop takes the turned size at once (sg-session follows too) */
            if (g_cur_mode >= 0) {
                int w = g_modes[g_cur_mode].w, h = g_modes[g_cur_mode].h;
                if (i % 2) desktop_follow(h, w); else desktop_follow(w, h);
            }
            refresh_page();
        }
        return TRUE;
    case CMD_RLOCK:
        _snwprintf(args, ARRAYSIZE(args), L"rotation lock %ls", st_checked(ctl) ? L"yes" : L"no");
        free(ctl_run(args, &ok, NULL, 0, 10000));
        refresh_page();
        return TRUE;
    case CMD_KEEP: g_revert_left = 0; KillTimer(g_page, 1); refresh_page(); return TRUE;
    case CMD_REVERT: revert_mode(); return TRUE;
    case CMD_ADVANCED: cpl_open_file(L"desk.cpl", NULL); return TRUE;
    }
    return FALSE;
}

/* ---- the display scale ------------------------------------------------------------------
 * LogPixels, which Windows programs read at their start (Wine scales the
 * windows of those that do not scale themselves); the taskbar and Start
 * follow it at once, told by WM_SETTINGCHANGE "WindowMetrics" (wine-sg 0832).
 *
 * Until the user picks a scale, it is the recommended one for the screen,
 * set at sign-in and when the screen changes size (sg-session); a pick
 * that is not the recommended one is kept (Software\Stained Glass\Display
 * ScaleChosen), picking the recommended one again makes it automatic.
 * AutoLogPixels is the value the automatic scale last wrote: a LogPixels
 * other than it and 96 was chosen before this existed, and is kept too.
 * The same rule, in sh, is sg-session's sg_auto_scale (before the shell
 * starts). */
#define DISPLAY_KEY L"Software\\Stained Glass\\Display"

/* The recommended scale for a WxH screen: whatever is on it takes the share
 * of the screen it takes on a 1080p screen at 100% (David 2026-10-05: "the
 * taskbar will take up the same % of screen as on a 1080p screen") -- the
 * shorter side over 1080, to the nearest of Windows' 25% steps, never below
 * 100% (1080p and smaller stay as they are), at most 400%. 1440p 125%,
 * 1824 (a Surface Pro 7) 175%, 2160 (4K) 200%. Windows weighs the panel's
 * physical size too; the screen's height alone is the rule here, so a
 * screen looks the same at every size of panel. */
int scale_for_screen(int w, int h)
{
    int s = w < h ? w : h, q;
#ifdef SG_MUTANT_SCALE_RECOMMEND_100
    return 100;
#endif
    if (s <= 0) return 100;
    q = (s * 4 + 540) / 1080;
    return (q < 4 ? 4 : q > 16 ? 16 : q) * 25;
}

/* the screen's size in pixels: the mode in use (the desktop's), not the
 * one a program that Wine scales is shown */
static void screen_size(int *w, int *h)
{
    DEVMODEW dm = { .dmSize = sizeof(dm) };
    WCHAR fake[32];
    *w = GetSystemMetrics(SM_CXSCREEN); *h = GetSystemMetrics(SM_CYSCREEN);
    if (EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth >= 200 && dm.dmPelsHeight >= 200) {
        *w = (int)dm.dmPelsWidth; *h = (int)dm.dmPelsHeight;
    }
    /* the gates' stand-in for a screen their X server does not have */
    if (GetEnvironmentVariableW(L"SG_FAKE_SCREEN", fake, ARRAYSIZE(fake))) swscanf(fake, L"%dx%d", w, h);
}

int scale_recommended(void)
{
    int w, h;
    screen_size(&w, &h);
    return scale_for_screen(w, h);
}

/* the scale in use, in percent (LogPixels; 100 where there is none) */
int scale_current(void)
{
    DWORD dpi = reg_dword(HKEY_CURRENT_USER, DESKTOP, L"LogPixels", 96);
    return MulDiv(dpi < 96 ? 96 : (int)dpi, 100, 96);
}

static void scale_write(int dpi)
{
    DWORD_PTR r;
    WCHAR args[32];
    BOOL ok;
    char *out;
    reg_set_dword(HKEY_CURRENT_USER, DESKTOP, L"LogPixels", dpi);
#ifndef SG_MUTANT_SCALE_NO_BROADCAST
    /* every window: Wine lays the running programs out at the new scale
     * as it delivers this (wine-sg 0890), the taskbar and Start follow */
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"WindowMetrics", SMTO_ABORTIFHUNG, 2000, &r);
#else
    (void)r;
#endif
#ifndef SG_MUTANT_SCALE_LINUX_NOT_TOLD
    /* and the Linux programs, while they run: sg-session's sg-display-scale
     * (the session's XSETTINGS manager: GTK, Qt, the pointer) */
    _snwprintf(args, ARRAYSIZE(args), L"display-scale %d", MulDiv(dpi, 100, 96));
    args[ARRAYSIZE(args) - 1] = 0;
    if ((out = ctl_run(args, &ok, NULL, 0, 3000))) free(out);
#else
    (void)args; (void)ok; (void)out;
#endif
}

/* Settings' list and sg-control --set scale PERCENT: the user's pick */
const WCHAR *scale_set(int percent)
{
    int dpi = MulDiv(percent, 96, 100);
    if (percent < 100 || percent > 500) return L"a scale is 100 to 500 percent";
#ifndef SG_MUTANT_SCALE_NO_STICK
    /* kept from now on, unless it is the recommended one: then the scale is
     * automatic again, and follows the screen */
    if (percent == scale_recommended()) {
        reg_set_dword(HKEY_CURRENT_USER, DISPLAY_KEY, L"ScaleChosen", 0);
        reg_set_dword(HKEY_CURRENT_USER, DISPLAY_KEY, L"AutoLogPixels", dpi);
    } else reg_set_dword(HKEY_CURRENT_USER, DISPLAY_KEY, L"ScaleChosen", 1);
#endif
    scale_write(dpi);
    look_rescale();   /* the title bars: their share of the screen at the new scale */
    return NULL;
}

/* the automatic scale (sign-in, the screen's size changed, --set scale
 * auto): the recommended one, unless the user chose one. TRUE when it
 * changed the scale. */
BOOL scale_auto(void)
{
    int lp = (int)reg_dword(HKEY_CURRENT_USER, DESKTOP, L"LogPixels", 0);
    int was_auto = (int)reg_dword(HKEY_CURRENT_USER, DISPLAY_KEY, L"AutoLogPixels", 0);
    int want = MulDiv(scale_recommended(), 96, 100);
#ifndef SG_MUTANT_SCALE_NO_STICK
    if (reg_dword(HKEY_CURRENT_USER, DISPLAY_KEY, L"ScaleChosen", 0)) return FALSE;
    if (lp && lp != 96 && lp != was_auto) {
        /* a scale picked before the automatic one existed: the user's */
        reg_set_dword(HKEY_CURRENT_USER, DISPLAY_KEY, L"ScaleChosen", 1);
        return FALSE;
    }
#endif
    (void)was_auto;
    if ((lp ? lp : 96) == want) return FALSE;
    reg_set_dword(HKEY_CURRENT_USER, DISPLAY_KEY, L"AutoLogPixels", want);
    scale_write(want);
    return TRUE;
}

/* --set scale auto: back to the recommended scale, which follows the screen */
const WCHAR *scale_set_auto(void)
{
    reg_set_dword(HKEY_CURRENT_USER, DISPLAY_KEY, L"ScaleChosen", 0);
    if (scale_auto()) look_rescale();
    return NULL;
}

/* ---- Sound --------------------------------------------------------------------------------- */
enum { CMD_OUT = CMD_PAGE_FIRST + 1, CMD_OUT_VOL, CMD_IN, CMD_IN_VOL, CMD_MUTE, CMD_SPEECH, CMD_MICPRIV, CMD_MICTEST,
       CMD_DEVSOUNDS };
static HWND g_mictest, g_miclevel;

struct sdev { WCHAR name[200], desc[200]; int vol; BOOL def, muted; };
static struct sdev g_sinks[24], g_sources[24];
static int g_nsinks, g_nsources;

/* the devices the page shows, as one string: a microphone or headset plugged
 * in or out while the page is open shows at once (David, 2026-10-07: a new
 * microphone worked but was not in the list) */
static WCHAR g_sound_sig[4096];
static int g_sound_ticks;

static void sound_sig_of(const char *ans, WCHAR *sig, int cch)
{
    const char *p = ans;
    int n = 0;
    sig[0] = 0;
    while (p && *p && n < cch - 2) {
        if (!strncmp(p, "SINK ", 5) || !strncmp(p, "SOURCE ", 7)) {
            const char *q = p;
            while (*q && *q != '\t' && *q != '\n' && n < cch - 2) sig[n++] = *q++;
            sig[n++] = '|';
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    sig[n] = 0;
}

static void load_sound(WCHAR *err, int cch, BOOL *ok)
{
    char *ans = ctl_run(L"sound", ok, err, cch, 8000), buf[1024];
    const char *pos = NULL;
    g_nsinks = g_nsources = 0;
    g_sound_ticks = 0;
    if (!ans) { *ok = FALSE; return; }
    sound_sig_of(ans, g_sound_sig, ARRAYSIZE(g_sound_sig));
    while (ctl_line(ans, "SINK", &pos, buf, sizeof(buf)) && g_nsinks < (int)ARRAYSIZE(g_sinks)) {
        struct sdev *d = &g_sinks[g_nsinks++];
        WCHAR f[16];
        ctl_field(buf, 0, d->name, ARRAYSIZE(d->name));
        ctl_field(buf, 1, f, ARRAYSIZE(f)); d->def = !lstrcmpW(f, L"yes");
        ctl_field(buf, 2, f, ARRAYSIZE(f)); d->vol = _wtoi(f);
        ctl_field(buf, 3, f, ARRAYSIZE(f)); d->muted = !lstrcmpW(f, L"yes");
        ctl_field(buf, 4, d->desc, ARRAYSIZE(d->desc));
    }
    pos = NULL;
    while (ctl_line(ans, "SOURCE", &pos, buf, sizeof(buf)) && g_nsources < (int)ARRAYSIZE(g_sources)) {
        struct sdev *d = &g_sources[g_nsources++];
        WCHAR f[16];
        ctl_field(buf, 0, d->name, ARRAYSIZE(d->name));
        ctl_field(buf, 1, f, ARRAYSIZE(f)); d->def = !lstrcmpW(f, L"yes");
        ctl_field(buf, 2, f, ARRAYSIZE(f)); d->vol = _wtoi(f);
        ctl_field(buf, 3, f, ARRAYSIZE(f)); d->muted = !lstrcmpW(f, L"yes");
        ctl_field(buf, 4, d->desc, ARRAYSIZE(d->desc));
    }
    free(ans);
}

static int default_of(const struct sdev *d, int n)
{
    int i;
    for (i = 0; i < n; i++) if (d[i].def) return i;
    return n ? 0 : -1;
}

void set_build_sound(void)
{
    const WCHAR *items[24];
    WCHAR err[256] = L"";
    BOOL ok;
    int y = st_title(L"Sound"), i, d;
    HWND c;
    load_sound(err, ARRAYSIZE(err), &ok);
    if (!ok) {
        WCHAR line[400];
        _snwprintf(line, ARRAYSIZE(line), L"Sound settings are not available right now. %ls", err);
        y = st_para(y, line);
    }
    y = st_head(y, L"Output");
    for (i = 0; i < g_nsinks; i++) items[i] = g_sinks[i].desc;
    d = default_of(g_sinks, g_nsinks);
    c = st_combo(&y, L"Choose your output device", items, g_nsinks, d, CMD_OUT);
    if (!g_nsinks) { EnableWindow(c, FALSE); y = st_para(y - S(6), L"No output devices found."); }
    c = st_slider(&y, L"Master volume", 0, 100, d >= 0 ? min(g_sinks[d].vol, 100) : 0, CMD_OUT_VOL);
    EnableWindow(c, d >= 0);
    if (d >= 0) st_checkbox(&y, L"Mute", g_sinks[d].muted, CMD_MUTE);

    y = st_head(y, L"Input");
    for (i = 0; i < g_nsources; i++) items[i] = g_sources[i].desc;
    d = default_of(g_sources, g_nsources);
    c = st_combo(&y, L"Choose your input device", items, g_nsources, d, CMD_IN);
    if (!g_nsources) { EnableWindow(c, FALSE); y = st_para(y - S(6), L"No input devices found."); }
    c = st_slider(&y, L"Input volume", 0, 100, d >= 0 ? min(g_sources[d].vol, 100) : 0, CMD_IN_VOL);
    EnableWindow(c, d >= 0);
    /* Test your microphone: the chosen input's level while you speak, as
     * Speech Recognition's (sg-dictate --meter) -- David 2026-10-02 */
    mic_meter_stop();
    g_mictest = st_button(&y, L"Test your microphone", CMD_MICTEST);
    g_miclevel = pg_control(PROGRESS_CLASSW, L"", 0, st_x() + S(200), y - S(46) + S(11), S(300), S(10), -1);
    EnableWindow(g_mictest, d >= 0);
    pg_timer(200);
    if (!privacy_mic_allowed())
        y = st_para(y, L"Microphone access for this device is off, so apps and voice typing cannot use it.");

    /* the sounds a USB device makes coming and going (sg-session's device-sounds;
     * David 2026-10-07) */
    {
        char *ans, buf[64];
        BOOL on = TRUE, known = FALSE;
        ans = ctl_run(L"device-sounds", &ok, NULL, 0, 5000);
        if (ans && ok && ctl_line(ans, "DEVICESOUNDS", NULL, buf, sizeof(buf))) { known = TRUE; on = !strcmp(buf, "on"); }
        free(ans);
        if (known) {
            y = st_head(y, L"Device sounds");
            st_toggle(&y, L"Play a sound when a device connects or disconnects", on, CMD_DEVSOUNDS);
        }
    }

    y = st_head(y, L"Related settings");
    st_link(&y, L"Microphone privacy settings", CMD_MICPRIV);
    st_link(&y, L"Speech recognition", CMD_SPEECH);
}

static void sound_set(const WCHAR *verb, const WCHAR *kind, const WCHAR *name, const WCHAR *value)
{
    WCHAR args[600], err[256];
    BOOL ok;
    _snwprintf(args, ARRAYSIZE(args), L"sound %ls %ls \"%ls\"%ls%ls", verb, kind, name, value ? L" " : L"", value ? value : L"");
    free(ctl_run(args, &ok, err, ARRAYSIZE(err), 8000));
    if (!ok) st_status(err[0] ? err : L"The sound setting could not be changed.");
}

BOOL set_cmd_sound(int id, int code, HWND ctl)
{
    WCHAR v[16];
    int i, d;
    switch (id) {
    case CMD_OUT: case CMD_IN:
        if (code == CBN_SELCHANGE) {
            i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (id == CMD_OUT && i >= 0 && i < g_nsinks) sound_set(L"default", L"sink", g_sinks[i].name, NULL);
            if (id == CMD_IN && i >= 0 && i < g_nsources) sound_set(L"default", L"source", g_sources[i].name, NULL);
            refresh_page();
        }
        return TRUE;
    case CMD_OUT_VOL: case CMD_IN_VOL:
        if (code == (PG_SCROLL_CODE | TB_ENDTRACK)) {
            _snwprintf(v, ARRAYSIZE(v), L"%d", (int)SendMessageW(ctl, TBM_GETPOS, 0, 0));
            if (id == CMD_OUT_VOL && (d = default_of(g_sinks, g_nsinks)) >= 0) {
                BOOL ok;
                sound_set(L"volume", L"sink", g_sinks[d].name, v);
                free(ctl_run(L"sound chime", &ok, NULL, 0, 3000));   /* at the new volume */
            }
            if (id == CMD_IN_VOL && (d = default_of(g_sources, g_nsources)) >= 0) sound_set(L"volume", L"source", g_sources[d].name, v);
        }
        return TRUE;
    case CMD_MUTE:
        if ((d = default_of(g_sinks, g_nsinks)) >= 0) sound_set(L"mute", L"sink", g_sinks[d].name, st_checked(ctl) ? L"yes" : L"no");
        return TRUE;
    case CMD_DEVSOUNDS: {
        BOOL ok;
        free(ctl_run(st_checked(ctl) ? L"device-sounds on" : L"device-sounds off", &ok, NULL, 0, 5000));
        if (!ok) st_status(L"The device sounds could not be changed.");
        return TRUE;
    }
    case CMD_SPEECH: navigate(PG_SPEECH); return TRUE;
    case CMD_MICPRIV: navigate(PG_S_PRIV_MIC); return TRUE;
    case CMD_MICTEST:
        if (mic_meter_poll() >= 0) {
            mic_meter_stop();
            SetWindowTextW(g_mictest, L"Test your microphone");
            SendMessageW(g_miclevel, PBM_SETPOS, 0, 0);
        } else if ((d = default_of(g_sources, g_nsources)) >= 0 && mic_meter_start(g_sources[d].name))
            SetWindowTextW(g_mictest, L"Stop test");
        return TRUE;
    }
    return FALSE;
}

void set_timer_sound(void)
{
    static BOOL was;
    int lv = mic_meter_poll();
    if (!g_mictest || !IsWindow(g_mictest)) return;
#ifndef SG_MUTANT_SOUND_LIST_STATIC
    /* every 3 s, not while the microphone is being tested: the devices again */
    if (lv < 0 && ++g_sound_ticks >= 15) {
        BOOL ok;
        WCHAR err[64], sig[ARRAYSIZE(g_sound_sig)];
        char *ans = ctl_run(L"sound", &ok, err, ARRAYSIZE(err), 3000);
        g_sound_ticks = 0;
        if (ans) {
            sound_sig_of(ans, sig, ARRAYSIZE(sig));
            free(ans);
            if (lstrcmpW(sig, g_sound_sig)) { refresh_page(); return; }
        }
    }
#endif
    if (lv >= 0) { SendMessageW(g_miclevel, PBM_SETPOS, lv, 0); was = TRUE; }
    else if (was) {
        was = FALSE;
        SetWindowTextW(g_mictest, L"Test your microphone");
        SendMessageW(g_miclevel, PBM_SETPOS, 0, 0);
    }
}

/* ---- Notifications & actions -------------------------------------------------------------- */
static const WCHAR PUSH[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications";
static const WCHAR NOTIFY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings";
static const WCHAR SG_NOTIFY[] = L"Software\\Stained Glass\\Notifications";
enum { CMD_TOASTS = CMD_PAGE_FIRST + 1, CMD_LOCKTOASTS, CMD_SOUNDS, CMD_FOCUS, CMD_TIPS };

void set_build_notify(void)
{
    static const WCHAR *const focus[] = { L"Off", L"Priority only", L"Alarms only" };
    int y = st_title(L"Notifications & actions");
    y = st_head(y, L"Notifications");
    st_toggle(&y, L"Get notifications from apps and other senders",
              reg_dword(HKEY_CURRENT_USER, PUSH, L"ToastEnabled", 1) != 0, CMD_TOASTS);
    st_checkbox(&y, L"Show notifications on the lock screen",
                reg_dword(HKEY_CURRENT_USER, NOTIFY, L"NOC_GLOBAL_SETTING_ALLOW_TOASTS_ABOVE_LOCK", 1) != 0, CMD_LOCKTOASTS);
    st_checkbox(&y, L"Allow notifications to play sounds",
                reg_dword(HKEY_CURRENT_USER, NOTIFY, L"NOC_GLOBAL_SETTING_ALLOW_NOTIFICATION_SOUND", 1) != 0, CMD_SOUNDS);
    st_checkbox(&y, L"Get tips, tricks, and suggestions as you use Stained Glass",
                reg_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager",
                          L"SubscribedContent-338389Enabled", 0) != 0, CMD_TIPS);
    y += S(8);
    y = st_head(y, L"Focus assist");
    st_combo(&y, L"Choose which notifications you see and hear", focus, 3,
             (int)reg_dword(HKEY_CURRENT_USER, SG_NOTIFY, L"FocusAssist", 0) % 3, CMD_FOCUS);
}

BOOL set_cmd_notify(int id, int code, HWND ctl)
{
    switch (id) {
    case CMD_TOASTS: reg_set_dword(HKEY_CURRENT_USER, PUSH, L"ToastEnabled", st_checked(ctl)); return TRUE;
    case CMD_LOCKTOASTS: reg_set_dword(HKEY_CURRENT_USER, NOTIFY, L"NOC_GLOBAL_SETTING_ALLOW_TOASTS_ABOVE_LOCK", st_checked(ctl)); return TRUE;
    case CMD_SOUNDS: reg_set_dword(HKEY_CURRENT_USER, NOTIFY, L"NOC_GLOBAL_SETTING_ALLOW_NOTIFICATION_SOUND", st_checked(ctl)); return TRUE;
    case CMD_TIPS:
        reg_set_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\ContentDeliveryManager",
                      L"SubscribedContent-338389Enabled", st_checked(ctl));
        return TRUE;
    case CMD_FOCUS:
        if (code == CBN_SELCHANGE) reg_set_dword(HKEY_CURRENT_USER, SG_NOTIFY, L"FocusAssist", (DWORD)SendMessageW(ctl, CB_GETCURSEL, 0, 0));
        return TRUE;
    }
    return FALSE;
}

/* ---- Power & sleep --------------------------------------------------------------------------- */
enum { CMD_SCREEN = CMD_PAGE_FIRST + 1, CMD_SLEEP, CMD_POWEROPTS, CMD_LID, CMD_LID_AC, CMD_POWERKEY };

/* What the power button and closing the lid do (David 2026-10-03): systemd-
 * logind's settings, for the whole machine, kept in our own drop-in (sg-admind
 * power-buttons, as an administrator). Unset, logind's own defaults: the lid
 * sleeps, plugged in as on battery; the power button shuts down. */
static const WCHAR *const PB_ACTION[] = { L"ignore", L"suspend", L"hibernate", L"poweroff" };
static const WCHAR *const PB_LABEL[] = { L"Do nothing", L"Sleep", L"Hibernate", L"Shut down" };
static int g_pb[3];     /* lid on battery, lid plugged in, power button: PB_ACTION indexes */

static int pb_index(const char *v)
{
    int i;
    for (i = 0; i < (int)ARRAYSIZE(PB_ACTION); i++) {
        char a[16];
        WideCharToMultiByte(CP_UTF8, 0, PB_ACTION[i], -1, a, sizeof(a), NULL, NULL);
        if (!strcmp(v, a)) return i;
    }
    return -1;
}

static void pb_load(void)
{
    WCHAR path[MAX_PATH] = L"Z:\\etc\\systemd\\logind.conf.d\\60-stained-glass-power.conf";
    char buf[1024], *l, *e;
    DWORD n = 0;
    HANDLE h;
    BOOL ac_set = FALSE;
    g_pb[0] = 1; g_pb[1] = -1; g_pb[2] = 3;
    GetEnvironmentVariableW(L"SG_LOGIND_DROPIN", path, MAX_PATH);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { if (!ReadFile(h, buf, sizeof(buf) - 1, &n, NULL)) n = 0; CloseHandle(h); }
    buf[n] = 0;
    for (l = buf; l && *l; l = e ? e + 1 : NULL) {
        int i;
        if ((e = strchr(l, '\n'))) *e = 0;
        if (e && e > l && e[-1] == '\r') e[-1] = 0;
        if (!strncmp(l, "HandleLidSwitch=", 16) && (i = pb_index(l + 16)) >= 0) g_pb[0] = i;
        else if (!strncmp(l, "HandleLidSwitchExternalPower=", 29) && (i = pb_index(l + 29)) >= 0) { g_pb[1] = i; ac_set = TRUE; }
        else if (!strncmp(l, "HandlePowerKey=", 15) && (i = pb_index(l + 15)) >= 0) g_pb[2] = i;
    }
    if (!ac_set) g_pb[1] = g_pb[0];
}

/* a laptop: a battery, or a lid (the rows for the lid only then) */
static BOOL has_lid(void)
{
    SYSTEM_POWER_STATUS ps;
    WCHAR v[8];
    if (GetEnvironmentVariableW(L"SG_POWER_LID", v, ARRAYSIZE(v))) return v[0] == L'1';
    if (GetFileAttributesW(L"Z:\\proc\\acpi\\button\\lid") != INVALID_FILE_ATTRIBUTES) return TRUE;
    return GetSystemPowerStatus(&ps) && !(ps.BatteryFlag & 128) && ps.BatteryFlag != 255;
}
static const int MINUTES[] = { 1, 2, 3, 5, 10, 15, 20, 25, 30, 45, 60, 120, 180, 240, 300, 0 };

static int minutes_index(int m)
{
    int i;
    for (i = 0; i < (int)ARRAYSIZE(MINUTES); i++) if (MINUTES[i] == m) return i;
    return ARRAYSIZE(MINUTES) - 1;
}

void set_build_power(void)
{
    WCHAR labels[ARRAYSIZE(MINUTES)][24];
    const WCHAR *items[ARRAYSIZE(MINUTES)];
    char *ans, buf[128];
    BOOL ok;
    WCHAR err[256] = L"";
    int y = st_title(L"Power & sleep"), i, screen = 10, sleep = 0;
    BOOL avail = FALSE;
    HWND a, b;
    for (i = 0; i < (int)ARRAYSIZE(MINUTES); i++) {
        if (!MINUTES[i]) lstrcpyW(labels[i], L"Never");
        else if (MINUTES[i] >= 60) _snwprintf(labels[i], ARRAYSIZE(labels[i]), L"%d hour%ls", MINUTES[i] / 60, MINUTES[i] >= 120 ? L"s" : L"");
        else _snwprintf(labels[i], ARRAYSIZE(labels[i]), L"%d minute%ls", MINUTES[i], MINUTES[i] > 1 ? L"s" : L"");
        items[i] = labels[i];
    }
    ans = ctl_run(L"power", &ok, err, ARRAYSIZE(err), 5000);
    if (ans && ok && ctl_line(ans, "POWER", NULL, buf, sizeof(buf))) {
        WCHAR f[16];
        ctl_field(buf, 0, f, ARRAYSIZE(f)); screen = _wtoi(f);
        ctl_field(buf, 1, f, ARRAYSIZE(f)); sleep = _wtoi(f);
        ctl_field(buf, 2, f, ARRAYSIZE(f)); avail = !lstrcmpW(f, L"yes");
    }
    free(ans);
    y = st_head(y, L"Screen");
    a = st_combo(&y, L"When plugged in, turn off after", items, ARRAYSIZE(MINUTES), minutes_index(screen), CMD_SCREEN);
    y = st_head(y, L"Sleep");
    b = st_combo(&y, L"When plugged in, PC goes to sleep after", items, ARRAYSIZE(MINUTES), minutes_index(sleep), CMD_SLEEP);
    if (!ok) { EnableWindow(a, FALSE); EnableWindow(b, FALSE); }
    if (!avail)
        y = st_para(y, ok ? L"These take effect in a Stained Glass session: they are saved, and applied the next time you sign in."
                          : L"Power settings are not available right now.");
#ifndef SG_MUTANT_NO_POWER_BUTTONS
    y = st_head(y, L"Power buttons and lid");
#endif
    pb_load();
    st_combo(&y, L"When I press the power button", PB_LABEL, ARRAYSIZE(PB_LABEL), g_pb[2], CMD_POWERKEY);
    if (has_lid()) {
        st_combo(&y, L"When I close the lid, on battery", PB_LABEL, ARRAYSIZE(PB_LABEL), g_pb[0], CMD_LID);
        st_combo(&y, L"When I close the lid, plugged in", PB_LABEL, ARRAYSIZE(PB_LABEL), g_pb[1], CMD_LID_AC);
    }
    y = st_para(y, L"These are for everyone who uses this PC, so changing them needs an administrator.");
    y = st_head(y, L"Save energy and battery life");
    y = st_para(y, L"Set shorter times above to save energy. The screen turns off first; the PC sleeps later.");
}

BOOL set_cmd_power(int id, int code, HWND ctl)
{
    WCHAR args[128], err[256];
    BOOL ok;
    int i;
    if ((id == CMD_LID || id == CMD_LID_AC || id == CMD_POWERKEY) && code == CBN_SELCHANGE) {
        int v[3] = { g_pb[0], g_pb[1], g_pb[2] };
        i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
        if (i < 0 || i >= (int)ARRAYSIZE(PB_ACTION)) return TRUE;
        v[id == CMD_LID ? 0 : id == CMD_LID_AC ? 1 : 2] = i;
        _snwprintf(args, ARRAYSIZE(args), L"/admin power-buttons %ls %ls %ls", PB_ACTION[v[0]], PB_ACTION[v[1]], PB_ACTION[v[2]]);
        if (run_elevated(args)) refresh_when_back(); else refresh_page();
        return TRUE;
    }
    if ((id != CMD_SCREEN && id != CMD_SLEEP) || code != CBN_SELCHANGE) return id == CMD_POWEROPTS || id == CMD_LID || id == CMD_LID_AC || id == CMD_POWERKEY;
    i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
    if (i < 0 || i >= (int)ARRAYSIZE(MINUTES)) return TRUE;
    _snwprintf(args, ARRAYSIZE(args), L"power %ls %d", id == CMD_SCREEN ? L"--screen" : L"--sleep", MINUTES[i]);
    free(ctl_run(args, &ok, err, ARRAYSIZE(err), 5000));
    if (!ok) st_status(err[0] ? err : L"The power setting could not be saved.");
    return TRUE;
}

/* ---- Storage --------------------------------------------------------------------------------- */
enum { CMD_SENSE = CMD_PAGE_FIRST + 1, CMD_CLEAN, CMD_OPEN_FIRST = CMD_PAGE_FIRST + 10 };
static const WCHAR SENSE[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\StorageSense\\Parameters\\StoragePolicy";

static ULONGLONG dir_size(const WCHAR *dir, int depth, int *budget)
{
    WCHAR pattern[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    ULONGLONG total = 0;
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if (--*budget <= 0) break;
        if (!lstrcmpW(fd.cFileName, L".") || !lstrcmpW(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 12 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                _snwprintf(sub, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
                total += dir_size(sub, depth + 1, budget);
            }
        } else total += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

static const struct { int csidl; const WCHAR *name; } FOLDERS[] = {
    { CSIDL_PERSONAL, L"Documents" }, { CSIDL_MYPICTURES, L"Pictures" }, { CSIDL_MYMUSIC, L"Music" },
    { CSIDL_MYVIDEO, L"Videos" }, { CSIDL_DESKTOPDIRECTORY, L"Desktop" },
};

static int clean_temp(const WCHAR *dir, ULONGLONG *freed)
{
    WCHAR pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!lstrcmpW(fd.cFileName, L".") || !lstrcmpW(fd.cFileName, L"..")) continue;
        _snwprintf(path, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            n += clean_temp(path, freed);
            RemoveDirectoryW(path);
        } else if (DeleteFileW(path)) { n++; *freed += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

void set_build_storage(void)
{
    WCHAR drives[128], *d, line[256], a[32], b[32], dir[MAX_PATH];
    int y = st_title(L"Storage"), i, budget;
    st_toggle(&y, L"Storage Sense", reg_dword(HKEY_CURRENT_USER, SENSE, L"01", 0) != 0, CMD_SENSE);
    y = st_para(y - S(8), L"Storage Sense can automatically free up space by getting rid of files you don't need, like "
                          L"temporary files.");
    y = st_head(y, L"Local Disk");
    GetLogicalDriveStringsW(ARRAYSIZE(drives), drives);
    for (d = drives; *d; d += lstrlenW(d) + 1) {
        ULARGE_INTEGER avail, total, free_;
        UINT type = GetDriveTypeW(d);
        if (type != DRIVE_FIXED) continue;
        if (!GetDiskFreeSpaceExW(d, &avail, &total, &free_) || !total.QuadPart) continue;
        format_size(total.QuadPart - free_.QuadPart, a, ARRAYSIZE(a));
        format_size(total.QuadPart, b, ARRAYSIZE(b));
        _snwprintf(line, ARRAYSIZE(line), L"%ls  %ls used of %ls", d, a, b);
        pg_text(st_x(), y, st_w(), S(22), g_font_body, COL_TEXT, line, DT_SINGLELINE | DT_END_ELLIPSIS);
        y += S(26);
        pg_fill(st_x(), y, S(400), S(10), g_dark ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xE0, 0xE0, 0xE0));
        pg_fill(st_x(), y, (int)(S(400) * ((double)(total.QuadPart - free_.QuadPart) / total.QuadPart)), S(10), st_accent());
        y += S(24);
    }
    y = st_head(y, L"This is how your storage is used");
    for (i = 0; i < (int)ARRAYSIZE(FOLDERS); i++) {
        if (FAILED(SHGetFolderPathW(NULL, FOLDERS[i].csidl, NULL, 0, dir))) continue;
        budget = 20000;
        format_size(dir_size(dir, 0, &budget), a, ARRAYSIZE(a));
        y = st_row(y, FOLDERS[i].name, a);
    }
    GetTempPathW(MAX_PATH, dir);
    budget = 20000;
    format_size(dir_size(dir, 0, &budget), a, ARRAYSIZE(a));
    y = st_row(y, L"Temporary files", a);
    y += S(8);
    st_button(&y, L"Remove temporary files", CMD_CLEAN);
}

BOOL set_cmd_storage(int id, int code, HWND ctl)
{
    (void)code;
    if (id == CMD_SENSE) { reg_set_dword(HKEY_CURRENT_USER, SENSE, L"01", st_checked(ctl)); return TRUE; }
    if (id == CMD_CLEAN) {
        WCHAR dir[MAX_PATH], msg[128], size[32];
        ULONGLONG freed = 0;
        int n;
        GetTempPathW(MAX_PATH, dir);
        if (lstrlenW(dir) > 3 && dir[lstrlenW(dir) - 1] == L'\\') dir[lstrlenW(dir) - 1] = 0;
        n = clean_temp(dir, &freed);
        format_size(freed, size, ARRAYSIZE(size));
        _snwprintf(msg, ARRAYSIZE(msg), L"Removed %d temporary files (%ls). Files in use were left alone.", n, size);
        st_status(msg);
        return TRUE;
    }
    return FALSE;
}

/* ---- Multitasking ------------------------------------------------------------------------------- */
static const WCHAR ADVANCED[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
enum { CMD_SNAP = CMD_PAGE_FIRST + 1, CMD_VD_TASKBAR, CMD_VD_ALTTAB };

void set_build_multitask(void)
{
    static const WCHAR *const scope[] = { L"Only the desktop I'm using", L"All desktops" };
    WCHAR v[8] = L"1";
    int y = st_title(L"Multitasking");
    y = st_head(y, L"Snap windows");
    reg_sz(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WindowArrangementActive", v, ARRAYSIZE(v));
    st_toggle(&y, L"Arrange windows automatically by dragging them to the sides or corners of the screen", v[0] != L'0', CMD_SNAP);
    y = st_head(y, L"Virtual desktops");
    st_combo(&y, L"On the taskbar, show windows that are open on", scope, 2,
             reg_dword(HKEY_CURRENT_USER, ADVANCED, L"VirtualDesktopTaskbarFilter", 1) ? 0 : 1, CMD_VD_TASKBAR);
    st_combo(&y, L"Pressing Alt+Tab shows windows that are open on", scope, 2,
             reg_dword(HKEY_CURRENT_USER, ADVANCED, L"VirtualDesktopAltTabFilter", 1) ? 0 : 1, CMD_VD_ALTTAB);
    y = st_para(y, L"Win+Tab opens Task View, where you can add desktops. Win+Ctrl+Left and Right switch between them.");
}

BOOL set_cmd_multitask(int id, int code, HWND ctl)
{
    switch (id) {
    case CMD_SNAP:
        reg_set_sz(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WindowArrangementActive", st_checked(ctl) ? L"1" : L"0");
        SystemParametersInfoW(SPI_SETWINARRANGING, st_checked(ctl), NULL, SPIF_SENDCHANGE);
        return TRUE;
    case CMD_VD_TASKBAR: case CMD_VD_ALTTAB:
        if (code == CBN_SELCHANGE)
            reg_set_dword(HKEY_CURRENT_USER, ADVANCED, id == CMD_VD_TASKBAR ? L"VirtualDesktopTaskbarFilter" : L"VirtualDesktopAltTabFilter",
                          SendMessageW(ctl, CB_GETCURSEL, 0, 0) == 0);
        return TRUE;
    }
    return FALSE;
}

/* ---- About ------------------------------------------------------------------------------------ */
enum { CMD_RENAME = SHIELD_ID(CMD_PAGE_FIRST + 1), CMD_RENAME_ADV = SHIELD_ID(CMD_PAGE_FIRST + 2), CMD_COPY = CMD_PAGE_FIRST + 3,
       CMD_COPY2 = CMD_PAGE_FIRST + 4 , CMD_ENVVARS_PAGE = CMD_PAGE_FIRST + 10, CMD_ENVVARS_OPEN = CMD_PAGE_FIRST + 11};

static void about_text(WCHAR *out, int cch, BOOL windows)
{
    struct sysfacts f;
    WCHAR guid[64] = L"";
    sys_gather(&f);
    reg_sz(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Cryptography", L"MachineGuid", guid, ARRAYSIZE(guid));
    if (!windows)
        _snwprintf(out, cch, L"Device name\t%ls\r\nProcessor\t%ls\r\nInstalled RAM\t%ls\r\nDevice ID\t%ls\r\nSystem type\t%ls\r\n",
                   f.computer, f.cpu, f.ram, guid[0] ? guid : L"-", f.arch);
    else
        _snwprintf(out, cch, L"Edition\t%ls\r\nVersion\t%ls\r\n", f.edition, f.os_build);
    out[cch - 1] = 0;
}

void set_build_about(void)
{
    struct sysfacts f;
    WCHAR guid[64] = L"", line[256];
    const char *(CDECL *wine_version)(void) = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
    int y = st_title(L"About");
    sys_gather(&f);
    reg_sz(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Cryptography", L"MachineGuid", guid, ARRAYSIZE(guid));
    y = st_card(y, IC_G_PC, f.computer, f.admin_account ? L"Your PC, and you are an administrator" : L"Your PC");
    y = st_head(y, L"Device specifications");
    y = st_row(y, L"Device name", f.computer);
    y = st_row(y, L"Full device name", f.fqdn);
    y = st_row(y, L"Processor", f.cpu);
    y = st_row(y, L"Installed RAM", f.ram);
    y = st_row(y, L"Device ID", guid[0] ? guid : L"-");
    y = st_row(y, L"System type", f.arch);
    if (f.role[0]) y = st_row(y, L"Domain", f.realm);
    y += S(8);
    st_button(&y, L"Copy", CMD_COPY);
    st_button(&y, L"Rename this PC", CMD_RENAME);
    y = st_head(y, L"Stained Glass specifications");
    y = st_row(y, L"Edition", f.edition);
    y = st_row(y, L"Version", f.os_build);
    if (wine_version) { _snwprintf(line, ARRAYSIZE(line), L"Wine %S (wine-sg)", wine_version()); y = st_row(y, L"Compatibility", line); }
    y += S(8);
    st_button(&y, L"Copy", CMD_COPY2);
    y = st_para(y, L"Stained Glass OS is free software: its programs are licensed under the GPL, LGPL and AGPL.");
    y = st_head(y, L"Related settings");
    st_link(&y, L"Rename this PC (advanced)", CMD_RENAME_ADV);
    st_link(&y, L"Environment variables", CMD_ENVVARS_PAGE);
}

BOOL set_cmd_about(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    switch (id) {
    case CMD_RENAME: if (run_elevated(L"/admin rename-pc")) refresh_when_back(); return TRUE;
    case CMD_RENAME_ADV: if (run_elevated(L"/admin rename")) refresh_when_back(); return TRUE;
    case CMD_ENVVARS_PAGE: navigate(PG_S_ENVVARS); return TRUE;
    case CMD_COPY: case CMD_COPY2: {
        WCHAR text[1024];
        HGLOBAL h;
        about_text(text, ARRAYSIZE(text), id == CMD_COPY2);
        if (OpenClipboard(g_main)) {
            EmptyClipboard();
            if ((h = GlobalAlloc(GMEM_MOVEABLE, (lstrlenW(text) + 1) * sizeof(WCHAR)))) {
                lstrcpyW(GlobalLock(h), text);
                GlobalUnlock(h);
                SetClipboardData(CF_UNICODETEXT, h);
            }
            CloseClipboard();
        }
        return TRUE;
    }
    }
    return FALSE;
}

/* Environment variables: what they are, the person's Path, and the dialog
 * (envvars.c) -- found by "environment" and "path" in Settings' search */
static void envvars_open(void)
{
    WCHAR self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    ShellExecuteW(g_main, NULL, self, L"/envvars", NULL, SW_SHOWNORMAL);
}

void set_build_envvars(void)
{
    WCHAR path[4096] = L"", line[1100];
    const WCHAR *p, *e;
    DWORD cb = sizeof(path);
    int y = st_title(L"Environment variables");
    y = st_para(y, L"Programs read settings from environment variables: Path lists the folders searched for "
                   L"programs you start by name (a command line, a script). Your variables are yours; system "
                   L"variables apply to everyone and need an administrator to change.");
    st_button(&y, L"Edit environment variables...", CMD_ENVVARS_OPEN);
    y = st_head(y, L"Your Path");
    if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"Path", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
                     NULL, path, &cb) || !path[0])
        y = st_para(y, L"You have no folders of your own in Path.");
    else
        for (p = path; p && *p; p = e ? e + 1 : NULL) {
            int len;
            e = wcschr(p, L';');
            len = e ? (int)(e - p) : lstrlenW(p);
            if (!len) continue;
            if (len > 1000) len = 1000;
            memcpy(line, p, len * sizeof(WCHAR)); line[len] = 0;
            y = st_para(y, line);
        }
    y = st_para(y, L"Programs started after a change get it; ones already running keep what they had.");
}

BOOL set_cmd_envvars(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    if (id == CMD_ENVVARS_OPEN) { envvars_open(); refresh_when_back(); return TRUE; }
    return FALSE;
}
