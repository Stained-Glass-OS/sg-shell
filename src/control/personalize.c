/* sg-control -- Personalization: desktop background, accent color, and
 * light or dark mode.
 *
 * Everything is kept where Windows keeps it, so any program -- and the
 * shell's own theme -- reads the same settings:
 *
 *   HKCU\Control Panel\Desktop       Wallpaper, WallpaperStyle, TileWallpaper
 *                                    (through SPI_SETDESKWALLPAPER)
 *   HKCU\Control Panel\Colors        Background (and SetSysColors)
 *   ...\Explorer\Wallpapers          BackgroundHistoryPath0-4, BackgroundType
 *   HKCU\Software\Microsoft\Internet Explorer\Desktop\General  WallpaperSource
 *   HKCU\Software\Microsoft\Windows\DWM          AccentColor, ColorizationColor
 *   ...\Explorer\Accent              AccentColorMenu, StartColorMenu, AccentPalette
 *   ...\Themes\Personalize           AppsUseLightTheme, SystemUsesLightTheme
 *
 * and each change is announced as Windows announces it (WM_SETTINGCHANGE
 * "ImmersiveColorSet", WM_DWMCOLORIZATIONCOLORCHANGED, WM_SYSCOLORCHANGE).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"
#include "wallpaper.h"
#include <commdlg.h>
#include <shlobj.h>

#ifndef WM_DWMCOLORIZATIONCOLORCHANGED
#define WM_DWMCOLORIZATIONCOLORCHANGED 0x0320
#endif

static const WCHAR DESKTOP[] = L"Control Panel\\Desktop";
static const WCHAR COLORS[] = L"Control Panel\\Colors";
/* the wallpapers sg-shell ships (theme/wallpapers), through Wine's Z: drive */
static const WCHAR SG_WALLPAPERS[] = L"Z:\\usr\\share\\stained-glass\\wallpapers";
static const WCHAR WALLPAPERS[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Wallpapers";
static const WCHAR IE_DESKTOP[] = L"Software\\Microsoft\\Internet Explorer\\Desktop\\General";
static const WCHAR DWM[] = L"Software\\Microsoft\\Windows\\DWM";
static const WCHAR ACCENT[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent";
static const WCHAR PERSONALIZE[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

const WCHAR *const PERS_FIT_NAMES[WP_COUNT] = { L"Fill", L"Fit", L"Stretch", L"Tile", L"Center", L"Span" };
static const WCHAR *const FIT_KEYS[WP_COUNT] = { L"fill", L"fit", L"stretch", L"tile", L"center", L"span" };
/* WallpaperStyle, TileWallpaper for each fit */
static const WCHAR *const FIT_STYLE[WP_COUNT] = { L"10", L"6", L"2", L"0", L"0", L"22" };
static const WCHAR *const FIT_TILE[WP_COUNT] = { L"0", L"0", L"0", L"1", L"0", L"0" };

/* our own palettes (colours are not anyone's artwork) */
const COLORREF PERS_ACCENTS[20] = {
    RGB(0x7B, 0x2F, 0xBE), RGB(0x9B, 0x3C, 0xC9), RGB(0x5A, 0x3F, 0xC0), RGB(0x3A, 0x55, 0xD6),
    RGB(0x00, 0x67, 0xC0), RGB(0x00, 0x82, 0xB4), RGB(0x03, 0x83, 0x87), RGB(0x1F, 0x9E, 0x8E),
    RGB(0x10, 0x7C, 0x41), RGB(0x49, 0x8A, 0x0E), RGB(0x76, 0x76, 0x00), RGB(0xB8, 0x86, 0x00),
    RGB(0xCA, 0x50, 0x10), RGB(0xD1, 0x34, 0x38), RGB(0xC2, 0x18, 0x5B), RGB(0xB1, 0x46, 0xC2),
    RGB(0x86, 0x4A, 0x8C), RGB(0x68, 0x76, 0x8A), RGB(0x51, 0x5C, 0x6B), RGB(0x4C, 0x4A, 0x48),
};
const COLORREF PERS_BACKGROUNDS[12] = {
    RGB(0x24, 0x70, 0x94), RGB(0x1B, 0x3A, 0x5C), RGB(0x2D, 0x1B, 0x4E), RGB(0x4A, 0x20, 0x60),
    RGB(0x0E, 0x4D, 0x4A), RGB(0x1E, 0x55, 0x2E), RGB(0x5C, 0x3A, 0x10), RGB(0x6B, 0x1F, 0x1F),
    RGB(0x10, 0x10, 0x10), RGB(0x3A, 0x3A, 0x3A), RGB(0x6E, 0x6E, 0x6E), RGB(0xC8, 0xC8, 0xC8),
};


void pers_read(struct pstate *s)
{
    WCHAR style[16] = L"10", tile[16] = L"0", bg[32] = L"";
    DWORD accent;
    int i;
    memset(s, 0, sizeof(*s));
    reg_sz(HKEY_CURRENT_USER, DESKTOP, L"Wallpaper", s->wallpaper, MAX_PATH);
    reg_sz(HKEY_CURRENT_USER, DESKTOP, L"WallpaperStyle", style, ARRAYSIZE(style));
    reg_sz(HKEY_CURRENT_USER, DESKTOP, L"TileWallpaper", tile, ARRAYSIZE(tile));
    reg_sz(HKEY_CURRENT_USER, WALLPAPERS, L"BackgroundHistoryPath0", s->source, MAX_PATH);
    if (!s->source[0]) reg_sz(HKEY_CURRENT_USER, IE_DESKTOP, L"WallpaperSource", s->source, MAX_PATH);
    if (!s->source[0]) lstrcpynW(s->source, s->wallpaper, MAX_PATH);
    s->style = WP_FILL;
    if (!lstrcmpW(tile, L"1")) s->style = WP_TILE;
    else for (i = 0; i < WP_COUNT; i++) if (i != WP_TILE && !lstrcmpW(style, FIT_STYLE[i])) { s->style = i; break; }
    s->solid = !s->wallpaper[0] || reg_dword(HKEY_CURRENT_USER, WALLPAPERS, L"BackgroundType", 0) == 1;
    reg_sz(HKEY_CURRENT_USER, COLORS, L"Background", bg, ARRAYSIZE(bg));
    {
        int r, g, b;
        if (swscanf(bg, L"%d %d %d", &r, &g, &b) == 3) s->background = RGB(r, g, b);
        else s->background = GetSysColor(COLOR_BACKGROUND);
    }
    /* AccentColor is ABGR: its low three bytes are a COLORREF */
    accent = reg_dword(HKEY_CURRENT_USER, DWM, L"AccentColor", 0xFFBE2F7B);
    s->accent = accent & 0xFFFFFF;
    s->apps_light = reg_dword(HKEY_CURRENT_USER, PERSONALIZE, L"AppsUseLightTheme", 1) != 0;
    s->system_light = reg_dword(HKEY_CURRENT_USER, PERSONALIZE, L"SystemUsesLightTheme", 0) != 0;
}

static void announce(const WCHAR *what)
{
    DWORD_PTR r;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)what, SMTO_ABORTIFHUNG, 2000, &r);
}

/* ---- applying ------------------------------------------------------------------------ */
static BOOL transcoded_path(WCHAR *out)
{
    WCHAR dir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, dir))) return FALSE;
    _snwprintf(out, MAX_PATH, L"%ls\\Microsoft\\Windows\\Themes", dir);
    out[MAX_PATH - 1] = 0;
    SHCreateDirectoryExW(NULL, out, NULL);
    lstrcatW(out, L"\\TranscodedWallpaper");
    return TRUE;
}

static void push_history(const WCHAR *src)
{
    WCHAR prev[5][MAX_PATH], name[40];
    int i, n = 0;
    for (i = 0; i < 5; i++) {
        prev[i][0] = 0;
        _snwprintf(name, ARRAYSIZE(name), L"BackgroundHistoryPath%d", i);
        reg_sz(HKEY_CURRENT_USER, WALLPAPERS, name, prev[i], MAX_PATH);
    }
    _snwprintf(name, ARRAYSIZE(name), L"BackgroundHistoryPath%d", n++);
    reg_set_sz(HKEY_CURRENT_USER, WALLPAPERS, name, src);
    for (i = 0; i < 5 && n < 5; i++) {
        if (!prev[i][0] || !lstrcmpiW(prev[i], src)) continue;
        _snwprintf(name, ARRAYSIZE(name), L"BackgroundHistoryPath%d", n++);
        reg_set_sz(HKEY_CURRENT_USER, WALLPAPERS, name, prev[i]);
    }
}

/* Wine's desktop, reloading its picture, paints over the windows on it and
 * nothing repaints them. Once it has had a moment to redraw, ask every
 * window to repaint. (Harmless where the desktop behaves.) */
static BOOL CALLBACK repaint_one(HWND hwnd, LPARAM lp)
{
    (void)lp;
    if (IsWindowVisible(hwnd)) RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    return TRUE;
}

static void repaint_all(void)
{
    Sleep(400);
    EnumWindows(repaint_one, 0);
}

/* Set the desktop picture. Returns 0, or a reason. */
const WCHAR *pers_set_wallpaper(const WCHAR *src, int style)
{
    struct pstate s;
    WCHAR out[MAX_PATH], full[MAX_PATH];
    DWORD *img, *fit = NULL;
    int w, h, ow, oh;
    BOOL ok;
    if (style < 0 || style >= WP_COUNT) return L"unknown fit";
    if (!GetFullPathNameW(src, MAX_PATH, full, NULL)) lstrcpynW(full, src, MAX_PATH);
    if (!(img = image_load(full, &w, &h))) return L"the picture could not be read";
    pers_read(&s);
    if (style == WP_SPAN) { ow = GetSystemMetrics(SM_CXVIRTUALSCREEN); oh = GetSystemMetrics(SM_CYVIRTUALSCREEN); }
    else { ow = GetSystemMetrics(SM_CXSCREEN); oh = GetSystemMetrics(SM_CYSCREEN); }
    if (ow <= 0 || oh <= 0) { ow = 1920; oh = 1080; }
    /* tiled: the picture at its own size; otherwise fitted to the screen */
    if (style == WP_TILE) { ow = w; oh = h; fit = img; img = NULL; }
    else fit = image_fit(img, w, h, style, s.background, ow, oh);
    free(img);
    if (!fit) return L"out of memory";
    ok = transcoded_path(out) && image_save_bmp(out, fit, ow, oh);
    free(fit);
    if (!ok) return L"the picture could not be saved";
    reg_set_sz(HKEY_CURRENT_USER, DESKTOP, L"WallpaperStyle", FIT_STYLE[style]);
    reg_set_sz(HKEY_CURRENT_USER, DESKTOP, L"TileWallpaper", FIT_TILE[style]);
    WriteProfileStringW(L"desktop", L"TileWallpaper", FIT_TILE[style]);
    reg_set_sz(HKEY_CURRENT_USER, IE_DESKTOP, L"WallpaperSource", full);
    reg_set_dword(HKEY_CURRENT_USER, WALLPAPERS, L"BackgroundType", 0);
    push_history(full);
    if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, out, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
        return L"the desktop did not accept the picture";
    repaint_all();
    return NULL;
}

const WCHAR *pers_set_background(COLORREF c)
{
    WCHAR v[32];
    INT el = COLOR_BACKGROUND;
    _snwprintf(v, ARRAYSIZE(v), L"%d %d %d", GetRValue(c), GetGValue(c), GetBValue(c));
    reg_set_sz(HKEY_CURRENT_USER, COLORS, L"Background", v);
    reg_set_dword(HKEY_CURRENT_USER, WALLPAPERS, L"BackgroundType", 1);
    SetSysColors(1, &el, &c);
    if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, (void *)L"", SPIF_UPDATEINIFILE | SPIF_SENDCHANGE))
        return L"the desktop did not accept the change";
    repaint_all();
    return NULL;
}

static COLORREF mix(COLORREF a, COLORREF b, double t)
{
    return RGB((int)(GetRValue(a) * (1 - t) + GetRValue(b) * t), (int)(GetGValue(a) * (1 - t) + GetGValue(b) * t),
               (int)(GetBValue(a) * (1 - t) + GetBValue(b) * t));
}

static DWORD abgr(COLORREF c) { return 0xFF000000 | (DWORD)c; }
static DWORD argb(COLORREF c, BYTE a) { return (DWORD)a << 24 | GetRValue(c) << 16 | GetGValue(c) << 8 | GetBValue(c); }

const WCHAR *pers_set_accent(COLORREF c)
{
    BYTE palette[32];
    COLORREF shades[8];
    HKEY k;
    int i;
    DWORD colorization = argb(c, 0xC4);
    shades[0] = mix(c, RGB(255, 255, 255), 0.6); shades[1] = mix(c, RGB(255, 255, 255), 0.4);
    shades[2] = mix(c, RGB(255, 255, 255), 0.2); shades[3] = c;
    shades[4] = mix(c, RGB(0, 0, 0), 0.2);       shades[5] = mix(c, RGB(0, 0, 0), 0.4);
    shades[6] = mix(c, RGB(0, 0, 0), 0.6);       shades[7] = mix(c, RGB(0x80, 0x80, 0x80), 0.5);
    for (i = 0; i < 8; i++) {           /* RGBA, one colour per four bytes */
        palette[i * 4] = GetRValue(shades[i]); palette[i * 4 + 1] = GetGValue(shades[i]);
        palette[i * 4 + 2] = GetBValue(shades[i]); palette[i * 4 + 3] = 0xFF;
    }
    if (!reg_set_dword(HKEY_CURRENT_USER, DWM, L"AccentColor", abgr(c))) return L"the setting could not be saved";
    reg_set_dword(HKEY_CURRENT_USER, DWM, L"ColorizationColor", colorization);
    reg_set_dword(HKEY_CURRENT_USER, DWM, L"ColorizationAfterglow", colorization);
    reg_set_dword(HKEY_CURRENT_USER, ACCENT, L"AccentColorMenu", abgr(c));
    reg_set_dword(HKEY_CURRENT_USER, ACCENT, L"StartColorMenu", abgr(shades[5]));
    if (RegCreateKeyExW(HKEY_CURRENT_USER, ACCENT, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, L"AccentPalette", 0, REG_BINARY, palette, sizeof(palette));
        RegCloseKey(k);
    }
    announce(L"ImmersiveColorSet");
    PostMessageW(HWND_BROADCAST, WM_DWMCOLORIZATIONCOLORCHANGED, colorization, TRUE);
    return NULL;
}

const WCHAR *pers_set_mode(BOOL apps, BOOL light)
{
    if (!reg_set_dword(HKEY_CURRENT_USER, PERSONALIZE, apps ? L"AppsUseLightTheme" : L"SystemUsesLightTheme", light ? 1 : 0))
        return L"the setting could not be saved";
    announce(L"ImmersiveColorSet");
    return NULL;
}

/* ---- tiles: colour swatches and picture thumbnails ---------------------------------- */
struct tile { COLORREF color; HBITMAP bmp; BOOL sel, hot; };

static LRESULT CALLBACK tile_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct tile *t = (struct tile *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        if (!(t = calloc(1, sizeof(*t)))) return FALSE;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)t);
        break;
    case WM_NCDESTROY: free(t); SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0); break;
    case TILE_SETCOLOR: t->color = (COLORREF)wp; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case TILE_SETBITMAP: t->bmp = (HBITMAP)wp; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case TILE_SETSEL: t->sel = (BOOL)wp; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_SETCURSOR: SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND)); return TRUE;
    case WM_MOUSEMOVE:
        if (!t->hot) { TRACKMOUSEEVENT e = { sizeof(e), TME_LEAVE, hwnd, 0 }; t->hot = TRUE; TrackMouseEvent(&e); InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_MOUSELEAVE: t->hot = FALSE; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN: SetFocus(hwnd); return 0;
    case WM_LBUTTONUP:
        PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), (LPARAM)hwnd);
        return 0;
    case WM_GETDLGCODE:
        if (lp && ((MSG *)lp)->message == WM_KEYDOWN && ((MSG *)lp)->wParam == VK_RETURN) return DLGC_WANTMESSAGE;
        return DLGC_WANTCHARS;
    case WM_KEYDOWN:
        if (wp == VK_RETURN) { PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), (LPARAM)hwnd); return 0; }
        break;
    case WM_CHAR:
        if (wp == ' ') { PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(hwnd), BN_CLICKED), (LPARAM)hwnd); return 0; }
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(hwnd, NULL, FALSE); break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r, in;
        HBRUSH b;
        GetClientRect(hwnd, &r);
        b = CreateSolidBrush(t->sel ? COL_TEXT : t->hot ? RGB(0x99, 0x99, 0x99) : COL_BG);
        FillRect(dc, &r, b); DeleteObject(b);
        in = r; InflateRect(&in, -S(2), -S(2));
        b = CreateSolidBrush(COL_BG); FillRect(dc, &in, b); DeleteObject(b);
        InflateRect(&in, -S(2), -S(2));
        if (t->bmp) {
            HDC mem = CreateCompatibleDC(dc);
            BITMAP bm;
            GetObjectW(t->bmp, sizeof(bm), &bm);
            SelectObject(mem, t->bmp);
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchBlt(dc, in.left, in.top, in.right - in.left, in.bottom - in.top, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            DeleteDC(mem);
        } else { b = CreateSolidBrush(t->color); FillRect(dc, &in, b); DeleteObject(b); }
        if (GetFocus() == hwnd && g_kbd_cues) DrawFocusRect(dc, &r);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the preview: the desktop as the settings would make it ------------------------ */
HBITMAP g_preview_pic;
static struct pstate g_state;

static LRESULT CALLBACK preview_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r, scr, bar, win, cap;
        HBRUSH b;
        struct pstate st;
        pers_read(&st);
        GetClientRect(hwnd, &r);
        b = CreateSolidBrush(COL_BG); FillRect(dc, &r, b); DeleteObject(b);
        b = CreateSolidBrush(RGB(0x2E, 0x38, 0x48));
        { HGDIOBJ o = SelectObject(dc, b), p = SelectObject(dc, GetStockObject(NULL_PEN));
          RoundRect(dc, 0, 0, r.right, r.bottom - S(18), S(10), S(10));
          Rectangle(dc, r.right / 2 - S(14), r.bottom - S(20), r.right / 2 + S(14), r.bottom - S(6));
          RoundRect(dc, r.right / 2 - S(50), r.bottom - S(8), r.right / 2 + S(50), r.bottom, S(6), S(6));
          SelectObject(dc, o); SelectObject(dc, p); }
        DeleteObject(b);
        SetRect(&scr, S(8), S(8), r.right - S(8), r.bottom - S(26));
        if (!st.solid && g_preview_pic) {
            HDC mem = CreateCompatibleDC(dc);
            BITMAP bm;
            GetObjectW(g_preview_pic, sizeof(bm), &bm);
            SelectObject(mem, g_preview_pic);
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchBlt(dc, scr.left, scr.top, scr.right - scr.left, scr.bottom - scr.top, mem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            DeleteDC(mem);
        } else { b = CreateSolidBrush(st.background); FillRect(dc, &scr, b); DeleteObject(b); }
        /* a window in the apps' mode */
        SetRect(&win, scr.left + (scr.right - scr.left) / 5, scr.top + (scr.bottom - scr.top) / 6,
                scr.right - (scr.right - scr.left) / 4, scr.bottom - (scr.bottom - scr.top) / 3);
        b = CreateSolidBrush(st.apps_light ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x2B, 0x2B, 0x2B)); FillRect(dc, &win, b); DeleteObject(b);
        cap = win; cap.bottom = cap.top + S(10);
        b = CreateSolidBrush(st.accent); FillRect(dc, &cap, b); DeleteObject(b);
        {
            int i;
            for (i = 0; i < 3; i++) {
                RECT l = { win.left + S(8), cap.bottom + S(8) + i * S(9), win.right - S(8) - i * S(18), cap.bottom + S(12) + i * S(9) };
                b = CreateSolidBrush(st.apps_light ? RGB(0xD0, 0xD0, 0xD0) : RGB(0x55, 0x55, 0x55)); FillRect(dc, &l, b); DeleteObject(b);
            }
        }
        /* the taskbar in the Windows mode, its start button in the accent */
        SetRect(&bar, scr.left, scr.bottom - S(12), scr.right, scr.bottom);
        b = CreateSolidBrush(st.system_light ? RGB(0xEE, 0xEE, 0xEE) : RGB(0x20, 0x20, 0x20)); FillRect(dc, &bar, b); DeleteObject(b);
        bar.right = bar.left + S(14);
        b = CreateSolidBrush(st.accent); FillRect(dc, &bar, b); DeleteObject(b);
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the pictures on offer ---------------------------------------------------------------- */
struct thumb { WCHAR path[MAX_PATH]; HBITMAP bmp; };
static struct thumb g_thumbs[16];
static int g_nthumbs;

HBITMAP pers_thumb(const WCHAR *path, int w, int h)
{
    int i, iw, ih;
    DWORD *img;
    HBITMAP b;
    for (i = 0; i < g_nthumbs; i++) if (!lstrcmpiW(g_thumbs[i].path, path)) return g_thumbs[i].bmp;
    if (!(img = image_load(path, &iw, &ih))) return NULL;
    b = image_thumbnail(img, iw, ih, WP_FILL, 0, w, h);
    free(img);
    if (g_nthumbs < (int)ARRAYSIZE(g_thumbs)) { lstrcpynW(g_thumbs[g_nthumbs].path, path, MAX_PATH); g_thumbs[g_nthumbs++].bmp = b; }
    return b;
}

static int find_pictures(WCHAR (*out)[MAX_PATH], int max, const WCHAR *dir, int depth)
{
    WCHAR pattern[MAX_PATH], sub[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", dir);
    if ((h = FindFirstFileW(pattern, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do {
        const WCHAR *ext = wcsrchr(fd.cFileName, L'.');
        if (fd.cFileName[0] == L'.') continue;
        _snwprintf(sub, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < 2) n += find_pictures(out + n, max - n, sub, depth + 1);
        } else if (ext && (!_wcsicmp(ext, L".jpg") || !_wcsicmp(ext, L".jpeg") || !_wcsicmp(ext, L".png") || !_wcsicmp(ext, L".bmp")))
            lstrcpynW(out[n++], sub, MAX_PATH);
    } while (n < max && FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* the pictures Personalization offers: Windows' wallpaper folder, then ours */
int pers_pictures(WCHAR (*out)[MAX_PATH], int max)
{
    WCHAR windir[MAX_PATH], dir[MAX_PATH];
    int n;
    GetWindowsDirectoryW(windir, MAX_PATH);
    _snwprintf(dir, MAX_PATH, L"%ls\\Web\\Wallpaper", windir);
    n = find_pictures(out, max, dir, 0);
    return n + find_pictures(out + n, max - n, SG_WALLPAPERS, 0);
}

static WCHAR g_pics[12][MAX_PATH];
static int g_npics;

enum {
    CMD_BGTYPE = CMD_PAGE_FIRST + 1, CMD_FIT, CMD_BROWSE, CMD_APPS_LIGHT, CMD_APPS_DARK, CMD_SYS_LIGHT, CMD_SYS_DARK,
    CMD_DISPLAY, CMD_GO_BG, CMD_GO_COLORS,
    CMD_PIC_FIRST = CMD_PAGE_FIRST + 100, CMD_BG_FIRST = CMD_PAGE_FIRST + 200, CMD_ACC_FIRST = CMD_PAGE_FIRST + 300,
};
static int g_colors_y;

void pers_register_classes(void)
{
    static BOOL done;
    WNDCLASSW wc = { 0 };
    if (done) return;
    wc.hInstance = g_inst;
    wc.lpfnWndProc = tile_proc; wc.lpszClassName = L"SgCplTile"; wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
    RegisterClassW(&wc);
    wc.lpfnWndProc = preview_proc; wc.lpszClassName = L"SgCplPreview"; wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    done = TRUE;
}

static int heading(int x, int y, int w, const WCHAR *s)
{
    pg_text(x, y, w, S(24), g_font_cat, COL_TITLE, s, DT_SINGLELINE);
    return y + S(34);
}

void build_personalize(void)
{
    static const WCHAR *const labels[] = { L"Desktop background", L"Colors", L"Display settings", NULL, L"See also", L"System" };
    static const int ids[] = { CMD_GO_BG, CMD_GO_COLORS, CMD_DISPLAY, 0, -1, NAV(PG_SYSTEM) };
    int x = pg_left_pane(labels, ids, ARRAYSIZE(labels)) + S(36), y = S(24), w = pg_width() - x - S(40), i;
    WCHAR windir[MAX_PATH], dir[MAX_PATH];
    HWND c;

    pers_register_classes();
    pers_read(&g_state);
    pg_title(x, y, L"Personalize your computer");
    y += S(48);

    /* the preview */
    if (!g_state.solid && g_state.source[0]) g_preview_pic = pers_thumb(g_state.source, S(192), S(108));
    else g_preview_pic = NULL;
    pg_control(L"SgCplPreview", L"", 0, x, y, S(300), S(200), -1);
    pg_para(x + S(324), y + S(8), w - S(324), g_font_body, COL_SUBTLE,
            L"Choose a picture or a color for your desktop, an accent color, and whether windows and the taskbar are light or dark. "
            L"Changes take effect immediately.");
    y += S(220);

    /* background */
    y = heading(x, y, w, L"Background");
    c = pg_control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, S(220), S(200), CMD_BGTYPE);
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Picture");
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Solid color");
    SendMessageW(c, CB_SETCURSEL, g_state.solid ? 1 : 0, 0);
    y += S(40);
    if (!g_state.solid) {
        int cols, tw = S(136), th = S(80);
        GetWindowsDirectoryW(windir, MAX_PATH);
        _snwprintf(dir, MAX_PATH, L"%ls\\Web\\Wallpaper", windir);
        g_npics = find_pictures(g_pics, ARRAYSIZE(g_pics), dir, 0);
        /* and the system's own (sg-shell's theme/wallpapers) */
        g_npics += find_pictures(g_pics + g_npics, ARRAYSIZE(g_pics) - g_npics, SG_WALLPAPERS, 0);
        pg_text(x, y, w, S(20), g_font_body, COL_TEXT, L"Choose your picture", DT_SINGLELINE);
        y += S(26);
        cols = w / (tw + S(10));
        if (cols < 1) cols = 1;
        for (i = 0; i < g_npics; i++) {
            HWND t = pg_control(L"SgCplTile", L"", WS_TABSTOP, x + (i % cols) * (tw + S(10)), y + (i / cols) * (th + S(10)), tw, th, CMD_PIC_FIRST + i);
            HBITMAP b = pers_thumb(g_pics[i], S(128), S(72));
            wchar_t *name = wcsrchr(g_pics[i], L'\\');
            SetWindowTextW(t, name ? name + 1 : g_pics[i]);
            if (b) SendMessageW(t, TILE_SETBITMAP, (WPARAM)b, 0);
            else SendMessageW(t, TILE_SETCOLOR, RGB(0xDD, 0xDD, 0xDD), 0);
            SendMessageW(t, TILE_SETSEL, !lstrcmpiW(g_pics[i], g_state.source), 0);
        }
        if (!g_npics) pg_text(x, y, w, S(20), g_font_body, COL_SUBTLE, L"No pictures were found in the Windows wallpaper folder.", DT_SINGLELINE);
        y += g_npics ? ((g_npics + cols - 1) / cols) * (th + S(10)) : S(24);
        pg_button(L"Browse...", x, y + S(4), S(110), CMD_BROWSE);
        y += S(48);
        pg_text(x, y, w, S(20), g_font_body, COL_TEXT, L"Choose a fit", DT_SINGLELINE);
        y += S(24);
        c = pg_control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, S(220), S(240), CMD_FIT);
        for (i = 0; i < WP_COUNT; i++) SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)PERS_FIT_NAMES[i]);
        SendMessageW(c, CB_SETCURSEL, g_state.style, 0);
        y += S(48);
    } else {
        pg_text(x, y, w, S(20), g_font_body, COL_TEXT, L"Choose your background color", DT_SINGLELINE);
        y += S(26);
        for (i = 0; i < (int)ARRAYSIZE(PERS_BACKGROUNDS); i++) {
            HWND t = pg_control(L"SgCplTile", L"", WS_TABSTOP, x + (i % 12) * S(48), y + (i / 12) * S(48), S(44), S(44), CMD_BG_FIRST + i);
            SendMessageW(t, TILE_SETCOLOR, PERS_BACKGROUNDS[i], 0);
            SendMessageW(t, TILE_SETSEL, PERS_BACKGROUNDS[i] == g_state.background, 0);
        }
        y += ((ARRAYSIZE(PERS_BACKGROUNDS) + 11) / 12) * S(48) + S(24);
    }

    /* colours */
    g_colors_y = y;
    y = heading(x, y, w, L"Accent color");
    for (i = 0; i < (int)ARRAYSIZE(PERS_ACCENTS); i++) {
        HWND t = pg_control(L"SgCplTile", L"", WS_TABSTOP, x + (i % 10) * S(48), y + (i / 10) * S(48), S(44), S(44), CMD_ACC_FIRST + i);
        SendMessageW(t, TILE_SETCOLOR, PERS_ACCENTS[i], 0);
        SendMessageW(t, TILE_SETSEL, PERS_ACCENTS[i] == g_state.accent, 0);
    }
    y += ((ARRAYSIZE(PERS_ACCENTS) + 9) / 10) * S(48) + S(24);

    y = heading(x, y, w, L"Choose your default app mode");
    c = pg_control(L"BUTTON", L"Light", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, x, y, S(120), S(24), CMD_APPS_LIGHT);
    SendMessageW(c, BM_SETCHECK, g_state.apps_light ? BST_CHECKED : BST_UNCHECKED, 0);
    c = pg_control(L"BUTTON", L"Dark", WS_TABSTOP | BS_AUTORADIOBUTTON, x + S(130), y, S(120), S(24), CMD_APPS_DARK);
    SendMessageW(c, BM_SETCHECK, g_state.apps_light ? BST_UNCHECKED : BST_CHECKED, 0);
    y += S(44);
    y = heading(x, y, w, L"Choose your default Windows mode");
    c = pg_control(L"BUTTON", L"Light", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON, x, y, S(120), S(24), CMD_SYS_LIGHT);
    SendMessageW(c, BM_SETCHECK, g_state.system_light ? BST_CHECKED : BST_UNCHECKED, 0);
    c = pg_control(L"BUTTON", L"Dark", WS_TABSTOP | BS_AUTORADIOBUTTON, x + S(130), y, S(120), S(24), CMD_SYS_DARK);
    SendMessageW(c, BM_SETCHECK, g_state.system_light ? BST_UNCHECKED : BST_CHECKED, 0);
    y += S(40);
}

static void failed(const WCHAR *why)
{
    WCHAR msg[256];
    if (!why) { refresh_page(); return; }
    _snwprintf(msg, ARRAYSIZE(msg), L"The change could not be made: %ls.", why);
    message(g_main, L"Personalization", msg, TRUE);
}

BOOL cmd_personalize(int id, int code, HWND ctl)
{
    (void)ctl;
    if (id >= CMD_PIC_FIRST && id < CMD_PIC_FIRST + g_npics) {
        HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
        const WCHAR *r = pers_set_wallpaper(g_pics[id - CMD_PIC_FIRST], g_state.style);
        SetCursor(old);
        failed(r);
        return TRUE;
    }
    if (id >= CMD_BG_FIRST && id < CMD_BG_FIRST + (int)ARRAYSIZE(PERS_BACKGROUNDS)) { failed(pers_set_background(PERS_BACKGROUNDS[id - CMD_BG_FIRST])); return TRUE; }
    if (id >= CMD_ACC_FIRST && id < CMD_ACC_FIRST + (int)ARRAYSIZE(PERS_ACCENTS)) { failed(pers_set_accent(PERS_ACCENTS[id - CMD_ACC_FIRST])); return TRUE; }
    switch (id) {
    case CMD_BGTYPE:
        if (code == CBN_SELCHANGE) {
            LRESULT sel = SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (sel == 1 && !g_state.solid) failed(pers_set_background(g_state.background));
            else if (sel == 0 && g_state.solid) {
                /* back to a picture: the last one, or the first on offer */
                WCHAR windir[MAX_PATH], dir[MAX_PATH];
                const WCHAR *pic = NULL;
                WCHAR hist[MAX_PATH] = L"";
                reg_sz(HKEY_CURRENT_USER, WALLPAPERS, L"BackgroundHistoryPath0", hist, MAX_PATH);
                if (hist[0] && GetFileAttributesW(hist) != INVALID_FILE_ATTRIBUTES) pic = hist;
                else {
                    GetWindowsDirectoryW(windir, MAX_PATH);
                    _snwprintf(dir, MAX_PATH, L"%ls\\Web\\Wallpaper", windir);
                    g_npics = find_pictures(g_pics, ARRAYSIZE(g_pics), dir, 0);
                    g_npics += find_pictures(g_pics + g_npics, ARRAYSIZE(g_pics) - g_npics, SG_WALLPAPERS, 0);
                    if (g_npics) pic = g_pics[0];
                }
                if (pic) failed(pers_set_wallpaper(pic, g_state.style));
                else message(g_main, L"Personalization", L"There is no picture to show. Use Browse to choose one.", FALSE);
            }
        }
        return TRUE;
    case CMD_FIT:
        if (code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < WP_COUNT && g_state.source[0]) failed(pers_set_wallpaper(g_state.source, sel));
        }
        return TRUE;
    case CMD_BROWSE: {
        WCHAR file[MAX_PATH] = L"", start[MAX_PATH];
        OPENFILENAMEW ofn = { sizeof(ofn) };
        SHGetFolderPathW(NULL, CSIDL_MYPICTURES, NULL, 0, start);
        ofn.hwndOwner = g_main;
        ofn.lpstrFilter = L"Pictures (*.jpg; *.jpeg; *.png; *.bmp; *.gif; *.tif)\0*.jpg;*.jpeg;*.png;*.bmp;*.gif;*.tif;*.tiff\0All files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = start;
        ofn.lpstrTitle = L"Choose a picture";
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
        if (GetOpenFileNameW(&ofn)) failed(pers_set_wallpaper(file, g_state.style));
        return TRUE;
    }
    case CMD_APPS_LIGHT: case CMD_APPS_DARK:
        if ((id == CMD_APPS_LIGHT) != g_state.apps_light) failed(pers_set_mode(TRUE, id == CMD_APPS_LIGHT));
        return TRUE;
    case CMD_SYS_LIGHT: case CMD_SYS_DARK:
        if ((id == CMD_SYS_LIGHT) != g_state.system_light) failed(pers_set_mode(FALSE, id == CMD_SYS_LIGHT));
        return TRUE;
    case CMD_DISPLAY: cpl_open_file(L"desk.cpl", NULL); return TRUE;
    case CMD_GO_BG: page_scroll_to(0); return TRUE;
    case CMD_GO_COLORS: page_scroll_to(g_colors_y); return TRUE;
    }
    return FALSE;
}

LRESULT notify_personalize(NMHDR *nm) { (void)nm; return 0; }

/* --set KIND VALUE...: the same changes without the window, for the gate and scripts */
static BOOL parse_rgb(const WCHAR *s, COLORREF *c)
{
    unsigned v;
    if (s[0] == L'#') s++;
    if (lstrlenW(s) != 6 || swscanf(s, L"%6x", &v) != 1) return FALSE;
    *c = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return TRUE;
}

int personalize_set(int argc, WCHAR **argv)
{
    const WCHAR *why = L"usage: --set wallpaper PATH [fill|fit|stretch|tile|center|span] | background RRGGBB | "
                       L"accent RRGGBB | mode apps|system light|dark";
    COLORREF c;
    int i;
    if (argc >= 2 && !lstrcmpW(argv[0], L"wallpaper")) {
        int style = WP_FILL;
        if (argc >= 3) { style = -1; for (i = 0; i < WP_COUNT; i++) if (!lstrcmpiW(argv[2], FIT_KEYS[i])) style = i; }
        why = style < 0 ? L"unknown fit" : pers_set_wallpaper(argv[1], style);
    } else if (argc == 2 && !lstrcmpW(argv[0], L"background") && parse_rgb(argv[1], &c)) why = pers_set_background(c);
    else if (argc == 2 && !lstrcmpW(argv[0], L"accent") && parse_rgb(argv[1], &c)) why = pers_set_accent(c);
    else if (argc == 3 && !lstrcmpW(argv[0], L"mode") && (!lstrcmpW(argv[1], L"apps") || !lstrcmpW(argv[1], L"system")) &&
             (!lstrcmpW(argv[2], L"light") || !lstrcmpW(argv[2], L"dark")))
        why = pers_set_mode(!lstrcmpW(argv[1], L"apps"), !lstrcmpW(argv[2], L"light"));
    if (why) { wprintf(L"FAILED %ls\n", why); fflush(stdout); return 1; }
    wprintf(L"OK\n");
    fflush(stdout);
    return 0;
}

void dump_personalize(void)
{
    struct pstate s;
    pers_read(&s);
    wprintf(L"wallpaper=%ls\n", s.wallpaper);
    wprintf(L"wallpaper.source=%ls\n", s.source);
    wprintf(L"wallpaper.fit=%ls\n", FIT_KEYS[s.style]);
    wprintf(L"background.type=%ls\n", s.solid ? L"solid" : L"picture");
    wprintf(L"background.color=%02X%02X%02X\n", GetRValue(s.background), GetGValue(s.background), GetBValue(s.background));
    wprintf(L"accent=%02X%02X%02X\n", GetRValue(s.accent), GetGValue(s.accent), GetBValue(s.accent));
    wprintf(L"mode.apps=%ls\n", s.apps_light ? L"light" : L"dark");
    wprintf(L"mode.system=%ls\n", s.system_light ? L"light" : L"dark");
}
