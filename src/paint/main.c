/* sg-paint -- Paint for Stained Glass OS (mspaint.exe).
 *
 * One window: the ribbon (ribbon.c) over the canvas (canvas.c) over the
 * status bar. The picture is a 32-bit DIB (image.c); files go through WIC,
 * copy and paste through CF_DIB. Command line: mspaint.exe [file].
 *
 * SG_PAINT_DUMP=<file> writes the state (tool, colours, picture size, title,
 * the ribbon's and the canvas's screen rectangles) after every change, for
 * the gate (test/paint-check.sh).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "paint.h"
/* Stained Glass: the app mode picks the chrome's palette (paint.h); switched live */
BOOL sgm_dark;
void sgm_follow(HWND hwnd)
{
    BOOL dark = sg_apps_dark();
    if (dark == sgm_dark) return;
    sgm_dark = dark;
    sg_mode_title(hwnd, dark);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}
#include "resource.h"

HINSTANCE g_inst;
HWND g_main, g_ribbon, g_canvas, g_status;
int g_dpi = 96;
HFONT g_font, g_font_small;
int g_tool = T_PENCIL, g_prev_tool = T_PENCIL, g_brush = B_BRUSH, g_shape = S_LINE, g_size_idx = 0;
int g_outline_on = 1, g_fill_on = 0;
COLORREF g_color1 = RGB(0, 0, 0), g_color2 = RGB(255, 255, 255);
COLORREF g_palette[30] = {
    RGB(0, 0, 0), RGB(127, 127, 127), RGB(136, 0, 21), RGB(237, 28, 36), RGB(255, 127, 39),
    RGB(255, 242, 0), RGB(34, 177, 76), RGB(0, 162, 232), RGB(63, 72, 204), RGB(112, 48, 192),
    RGB(255, 255, 255), RGB(195, 195, 195), RGB(185, 122, 87), RGB(255, 174, 201), RGB(255, 201, 14),
    RGB(239, 228, 176), RGB(181, 230, 29), RGB(153, 217, 234), RGB(112, 146, 190), RGB(200, 191, 231),
};
int g_ncustom;
int g_color_sel;
int g_zoom = 1000;
int g_grid, g_statusbar_on = 1, g_transparent_sel;
int g_dirty;
WCHAR g_path[MAX_PATH];
int g_tab;
int g_text_opaque;
LOGFONTW g_text_font;
const int SIZE_PX[4] = { 1, 3, 5, 8 };
static const WCHAR *g_dump_path;
static HACCEL g_accel;
static WCHAR g_status_msg[128];

int S(int v) { return MulDiv(v, g_dpi, 96); }

int tool_size(void)
{
    static const int pencil[4] = { 1, 2, 3, 4 }, eraser[4] = { 4, 6, 8, 10 };
    if (g_tool == T_PENCIL) return pencil[g_size_idx];
    if (g_tool == T_ERASER) return eraser[g_size_idx];
    return SIZE_PX[g_size_idx];
}

static const WCHAR *file_name(void)
{
    const WCHAR *s;
    if (!g_path[0]) return L"Untitled";
    s = wcsrchr(g_path, '\\');
    return s ? s + 1 : g_path;
}

void update_title(void)
{
    WCHAR t[MAX_PATH + 32];
    _snwprintf(t, ARRAYSIZE(t), L"%ls - Paint", file_name());
    SetWindowTextW(g_main, t);
}

void set_dirty(void) { g_dirty = 1; }

void set_tool(int tool)
{
    if (tool == g_tool) return;
    text_commit();
    if (g_tool == T_SELECT || g_tool == T_FREESEL)
        if (tool != T_SELECT && tool != T_FREESEL) sel_commit();
    g_prev_tool = g_tool;
    g_tool = tool;
    ribbon_layout();
    InvalidateRect(g_canvas, NULL, FALSE);
    write_dump();
}

void layout(void)
{
    RECT rc;
    int rh = ribbon_height(), sh = g_statusbar_on ? status_height() : 0;
    GetClientRect(g_main, &rc);
    MoveWindow(g_ribbon, 0, 0, rc.right, rh, TRUE);
    MoveWindow(g_canvas, 0, rh, rc.right, max(0, rc.bottom - rh - sh), TRUE);
    ShowWindow(g_status, g_statusbar_on ? SW_SHOW : SW_HIDE);
    MoveWindow(g_status, 0, rc.bottom - sh, rc.right, sh, TRUE);
}

void canvas_dump(FILE *f);

void write_dump(void)
{
    FILE *f;
    WCHAR tmp[MAX_PATH + 8];
    WCHAR title[MAX_PATH + 32];
    RECT r;
    int x, y, in;
    if (!g_dump_path) return;
    _snwprintf(tmp, ARRAYSIZE(tmp), L"%ls.tmp", g_dump_path);
    if (!(f = _wfopen(tmp, L"wb"))) return;
    GetWindowTextW(g_main, title, ARRAYSIZE(title));
    fprintf(f, "title %ls\n", title);
    fprintf(f, "path %ls\n", g_path);
    fprintf(f, "tool %d\nbrush %d\nshape %d\nsize %d\n", g_tool, g_brush, g_shape, tool_size());
    fprintf(f, "color1 %06lx\ncolor2 %06lx\n", (unsigned long)g_color1, (unsigned long)g_color2);
    fprintf(f, "image %d %d\nzoom %d\ndirty %d\nundo %d\nredo %d\n", g_img.w, g_img.h, g_zoom, g_dirty, undo_count(), redo_count());
    fprintf(f, "outline %d\nfill %d\ntab %d\ngrid %d\nstatusbar %d\ntext %d\n", g_outline_on, g_fill_on, g_tab, g_grid, g_statusbar_on, text_active());
    if (sel_active()) { sel_rect(&r); fprintf(f, "selection %ld %ld %ld %ld\n", r.left, r.top, r.right, r.bottom); }
    else fprintf(f, "selection none\n");
    canvas_cursor_info(&x, &y, &in);
    fprintf(f, "cursor %d %d %d\n", x, y, in);
    fprintf(f, "status %ls\n", g_status_msg);
    GetWindowRect(g_main, &r);
    fprintf(f, "window %ld %ld %ld %ld\n", r.left, r.top, r.right, r.bottom);
    canvas_dump(f);
    ribbon_dump(f);
    fprintf(f, "end\n");
    fclose(f);
    MoveFileExW(tmp, g_dump_path, MOVEFILE_REPLACE_EXISTING);
}

/* ---- files ---- */
static const WCHAR OPEN_FILTER[] =
    L"All Picture Files\0*.bmp;*.dib;*.jpg;*.jpeg;*.jpe;*.jfif;*.gif;*.tif;*.tiff;*.png;*.ico\0"
    L"Bitmap Files (*.bmp;*.dib)\0*.bmp;*.dib\0JPEG (*.jpg;*.jpeg;*.jpe;*.jfif)\0*.jpg;*.jpeg;*.jpe;*.jfif\0"
    L"GIF (*.gif)\0*.gif\0TIFF (*.tif;*.tiff)\0*.tif;*.tiff\0PNG (*.png)\0*.png\0ICO (*.ico)\0*.ico\0All Files (*.*)\0*.*\0";
static const WCHAR SAVE_FILTER[] =
    L"PNG (*.png)\0*.png\0JPEG (*.jpg;*.jpeg;*.jpe;*.jfif)\0*.jpg\0"
    L"24-bit Bitmap (*.bmp;*.dib)\0*.bmp\0GIF (*.gif)\0*.gif\0TIFF (*.tif;*.tiff)\0*.tif\0";
static const WCHAR *SAVE_EXT[] = { L"png", L"jpg", L"bmp", L"gif", L"tif" };

static void pictures_dir(WCHAR *out)
{
    out[0] = 0;
    if (!SHGetSpecialFolderPathW(NULL, out, CSIDL_MYPICTURES, TRUE)) out[0] = 0;
}

static BOOL open_path(const WCHAR *path)
{
    Img im;
    WCHAR err[512];
    if (!img_load(path, &im, err, ARRAYSIZE(err)))
    {
        MessageBoxW(g_main, err, L"Paint", MB_OK | MB_ICONWARNING);
        return FALSE;
    }
    text_cancel();
    sel_clear();
    img_install(&im);
    undo_clear();
    GetFullPathNameW(path, MAX_PATH, g_path, NULL);
    g_dirty = 0;
    update_title();
    canvas_changed(TRUE);
    write_dump();
    return TRUE;
}

/* fmt: -1 keeps the current type (or PNG), 0..4 preselects one */
static BOOL save_as(int fmt)
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR name[MAX_PATH], dir[MAX_PATH], err[256], *dot;
    int i;
    pictures_dir(dir);
    lstrcpynW(name, g_path[0] ? file_name() : L"Untitled", MAX_PATH);
    if (fmt < 0)
    {
        fmt = 0;
        if ((dot = wcsrchr(name, '.')))
            for (i = 0; i < 5; i++)
                if (!lstrcmpiW(dot + 1, SAVE_EXT[i]) || (i == 1 && !lstrcmpiW(dot + 1, L"jpeg")) || (i == 4 && !lstrcmpiW(dot + 1, L"tiff")))
                    fmt = i;
    }
    if ((dot = wcsrchr(name, '.'))) *dot = 0;
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = SAVE_FILTER;
    ofn.nFilterIndex = fmt + 1;
    ofn.lpstrFile = name;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = g_path[0] ? NULL : (dir[0] ? dir : NULL);
    ofn.lpstrTitle = L"Save As";
    ofn.lpstrDefExt = SAVE_EXT[fmt];
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return FALSE;
    /* the type the user chose decides the extension, if they typed none */
    if (!wcsrchr(name + ofn.nFileOffset, '.') && ofn.nFilterIndex >= 1 && ofn.nFilterIndex <= 5 &&
        lstrlenW(name) + 5 < MAX_PATH)
    {
        lstrcatW(name, L".");
        lstrcatW(name, SAVE_EXT[ofn.nFilterIndex - 1]);
    }
    if (!img_save(name, &g_img, err, ARRAYSIZE(err)))
    {
        MessageBoxW(g_main, err, L"Paint", MB_OK | MB_ICONWARNING);
        return FALSE;
    }
    lstrcpynW(g_path, name, MAX_PATH);
    g_dirty = 0;
    update_title();
    write_dump();
    return TRUE;
}

static BOOL save(void)
{
    WCHAR err[256];
    text_commit();
    sel_commit();
    if (!g_path[0]) return save_as(-1);
    if (!img_save(g_path, &g_img, err, ARRAYSIZE(err)))
    {
        MessageBoxW(g_main, err, L"Paint", MB_OK | MB_ICONWARNING);
        return save_as(-1);
    }
    g_dirty = 0;
    update_title();
    write_dump();
    return TRUE;
}

/* TRUE when it is fine to throw the picture away */
static BOOL confirm_discard(void)
{
    WCHAR msg[MAX_PATH + 64];
    text_commit();
    if (!g_dirty) return TRUE;
    _snwprintf(msg, ARRAYSIZE(msg), L"Do you want to save changes to %ls?", file_name());
    switch (MessageBoxW(g_main, msg, L"Paint", MB_YESNOCANCEL))
    {
    case IDYES: return save();
    case IDNO: return TRUE;
    default: return FALSE;
    }
}

static void file_new(void)
{
    RECT wa;
    if (!confirm_discard()) return;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    text_cancel();
    sel_clear();
    img_new(max(320, (wa.right - wa.left) * 3 / 5), max(240, (wa.bottom - wa.top) * 3 / 5));
    undo_clear();
    g_path[0] = 0;
    g_dirty = 0;
    update_title();
    canvas_changed(TRUE);
}

static void file_open(void)
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR name[MAX_PATH] = L"", dir[MAX_PATH];
    if (!confirm_discard()) return;
    pictures_dir(dir);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = OPEN_FILTER;
    ofn.lpstrFile = name;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = dir[0] ? dir : NULL;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) open_path(name);
}

static void paste_from(void)
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    WCHAR name[MAX_PATH] = L"", err[512];
    Img im;
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = OPEN_FILTER;
    ofn.lpstrFile = name;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Paste From";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    if (!img_load(name, &im, err, ARRAYSIZE(err))) { MessageBoxW(g_main, err, L"Paint", MB_OK | MB_ICONWARNING); return; }
    sel_paste(&im);
    img_free(&im);
}

/* ---- whole-picture changes ---- */
static void whole(int op)
{
    Img c;
    if (!img_copy(&c, &g_img)) return;
    undo_push();
    switch (op)
    {
    case CMD_ROT_R: img_rotate(&c, 1); break;
    case CMD_ROT_L: img_rotate(&c, 3); break;
    case CMD_ROT_180: img_rotate(&c, 2); break;
    case CMD_FLIP_V: img_flip(&c, TRUE); break;
    case CMD_FLIP_H: img_flip(&c, FALSE); break;
    }
    img_install(&c);
    set_dirty();
    canvas_changed(TRUE);
}

void canvas_set_size(int w, int h)
{
    Img n;
    int y;
    if (w == g_img.w && h == g_img.h) return;
    if (w < 1 || h < 1 || w > 20000 || h > 20000) return;
    if (!img_alloc(&n, w, h, g_color2)) return;
    undo_push();
    for (y = 0; y < min(h, g_img.h); y++) memcpy(n.px + (size_t)y * w, g_img.px + (size_t)y * g_img.w, min(w, g_img.w) * 4);
    img_install(&n);
    set_dirty();
    canvas_changed(TRUE);
}

/* ---- dialogs ---- */
static int dlg_int(HWND dlg, int id)
{
    BOOL ok;
    int v = (int)GetDlgItemInt(dlg, id, &ok, TRUE);
    return ok ? v : -1;
}

static int g_rs_pixels, g_rs_lock = 1, g_rs_updating;

static void rs_fill(HWND dlg)
{
    RECT r;
    int w = g_img.w, h = g_img.h;
    if (sel_active()) { sel_rect(&r); w = r.right - r.left; h = r.bottom - r.top; }
    g_rs_updating = 1;
    SetDlgItemInt(dlg, IDC_RS_H, g_rs_pixels ? w : 100, TRUE);
    SetDlgItemInt(dlg, IDC_RS_V, g_rs_pixels ? h : 100, TRUE);
    g_rs_updating = 0;
}

static INT_PTR CALLBACK resize_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    RECT r;
    int w = g_img.w, h = g_img.h;
    (void)lp;
    if (sel_active()) { sel_rect(&r); w = r.right - r.left; h = r.bottom - r.top; }
    switch (msg)
    {
    case WM_INITDIALOG:
        CheckRadioButton(dlg, IDC_RS_PERCENT, IDC_RS_PIXELS, g_rs_pixels ? IDC_RS_PIXELS : IDC_RS_PERCENT);
        CheckDlgButton(dlg, IDC_RS_LOCK, g_rs_lock ? BST_CHECKED : BST_UNCHECKED);
        rs_fill(dlg);
        SetDlgItemInt(dlg, IDC_SK_H, 0, TRUE);
        SetDlgItemInt(dlg, IDC_SK_V, 0, TRUE);
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDC_RS_PERCENT: case IDC_RS_PIXELS:
            g_rs_pixels = LOWORD(wp) == IDC_RS_PIXELS;
            rs_fill(dlg);
            return TRUE;
        case IDC_RS_LOCK:
            g_rs_lock = IsDlgButtonChecked(dlg, IDC_RS_LOCK) == BST_CHECKED;
            return TRUE;
        case IDC_RS_H: case IDC_RS_V:
            if (HIWORD(wp) == EN_CHANGE && g_rs_lock && !g_rs_updating)
            {
                int v = dlg_int(dlg, LOWORD(wp));
                if (v > 0)
                {
                    int o = g_rs_pixels ? (LOWORD(wp) == IDC_RS_H ? MulDiv(v, h, w) : MulDiv(v, w, h)) : v;
                    g_rs_updating = 1;
                    SetDlgItemInt(dlg, LOWORD(wp) == IDC_RS_H ? IDC_RS_V : IDC_RS_H, max(1, o), TRUE);
                    g_rs_updating = 0;
                }
            }
            return TRUE;
        case IDOK:
        {
            int hv = dlg_int(dlg, IDC_RS_H), vv = dlg_int(dlg, IDC_RS_V), hs = dlg_int(dlg, IDC_SK_H), vs = dlg_int(dlg, IDC_SK_V);
            int nw = g_rs_pixels ? hv : MulDiv(w, hv, 100), nh = g_rs_pixels ? vv : MulDiv(h, vv, 100);
            if (hv < 1 || vv < 1 || nw < 1 || nh < 1 || nw > 20000 || nh > 20000 || (!g_rs_pixels && (hv > 500 || vv > 500)))
            {
                MessageBoxW(dlg, g_rs_pixels ? L"Enter a whole number between 1 and 20000." : L"Enter a whole number between 1 and 500.",
                            L"Paint", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            if (hs < -89 || hs > 89 || vs < -89 || vs > 89)
            {
                MessageBoxW(dlg, L"Enter a whole number between -89 and 89.", L"Paint", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            EndDialog(dlg, IDOK);
            if (sel_active()) sel_resize(nw, nh, hs, vs);
            else
            {
                Img c;
                if (!img_copy(&c, &g_img)) return TRUE;
                undo_push();
                img_scale(&c, nw, nh);
                if (hs || vs) img_skew(&c, hs, vs, g_color2);
                img_install(&c);
                set_dirty();
                canvas_changed(TRUE);
            }
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK props_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        WCHAR s[MAX_PATH + 64];
        SetDlgItemInt(dlg, IDC_PR_W, g_img.w, FALSE);
        SetDlgItemInt(dlg, IDC_PR_H, g_img.h, FALSE);
        _snwprintf(s, ARRAYSIZE(s), L"File: %ls", g_path[0] ? g_path : L"Not saved");
        SetDlgItemTextW(dlg, IDC_PR_FILE, s);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK)
        {
            int w = dlg_int(dlg, IDC_PR_W), h = dlg_int(dlg, IDC_PR_H);
            if (w < 1 || h < 1 || w > 20000 || h > 20000)
            {
                MessageBoxW(dlg, L"Enter a whole number between 1 and 20000.", L"Paint", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            EndDialog(dlg, IDOK);
            text_commit();
            sel_commit();
            canvas_set_size(w, h);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static void edit_colors(void)
{
    static COLORREF custom[16];
    CHOOSECOLORW cc = { sizeof(cc) };
    int i;
    for (i = 0; i < 10 && i < g_ncustom; i++) custom[i] = g_palette[20 + i];
    cc.hwndOwner = g_main;
    cc.rgbResult = g_color_sel ? g_color2 : g_color1;
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (!ChooseColorW(&cc)) return;
    if (g_color_sel) g_color2 = cc.rgbResult; else g_color1 = cc.rgbResult;
    /* the newest custom colour first, as Paint's third row */
    memmove(g_palette + 21, g_palette + 20, 9 * sizeof(COLORREF));
    g_palette[20] = cc.rgbResult;
    if (g_ncustom < 10) g_ncustom++;
    InvalidateRect(g_ribbon, NULL, FALSE);
}

/* ---- full screen: the picture alone, until a click or a key ---- */
static LRESULT CALLBACK full_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        RECT rc;
        HDC dc = BeginPaint(hwnd, &ps);
        int w = g_img.w, h = g_img.h, x, y;
        GetClientRect(hwnd, &rc);
        if (w > rc.right || h > rc.bottom)
        {
            double s = min((double)rc.right / w, (double)rc.bottom / h);
            w = (int)(w * s); h = (int)(h * s);
        }
        x = (rc.right - w) / 2; y = (rc.bottom - h) / 2;
        FillRect(dc, &rc, GetStockObject(BLACK_BRUSH));
        SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, NULL);
        StretchBlt(dc, x, y, w, h, g_imgdc, 0, 0, g_img.w, g_img.h, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_KEYDOWN: case WM_LBUTTONUP: case WM_RBUTTONUP:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void full_screen(void)
{
    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = full_proc; wc.hInstance = g_inst; wc.lpszClassName = L"SgPaintFullScreen";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    text_commit();
    CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"Paint", WS_POPUP | WS_VISIBLE, 0, 0,
                    GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN), g_main, NULL, g_inst, NULL);
}

static const int ZOOMS[] = { 125, 250, 500, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000 };

int zoom_step(int dir)
{
    int i;
    if (dir > 0) { for (i = 0; i < (int)ARRAYSIZE(ZOOMS); i++) if (ZOOMS[i] > g_zoom) return ZOOMS[i]; return ZOOMS[ARRAYSIZE(ZOOMS) - 1]; }
    for (i = ARRAYSIZE(ZOOMS) - 1; i >= 0; i--) if (ZOOMS[i] < g_zoom) return ZOOMS[i];
    return ZOOMS[0];
}

static void about(void)
{
    MessageBoxW(g_main, L"Paint\n\nPart of Stained Glass OS.\nFree software under the GNU AGPL, version 3 or later.",
                L"About Paint", MB_OK | MB_ICONINFORMATION);
}

void do_command(int cmd)
{
    Img im;
    if (cmd >= CMD_TOOL_BASE && cmd < CMD_TOOL_BASE + T_COUNT) { set_tool(cmd - CMD_TOOL_BASE); return; }
    if (cmd >= CMD_BRUSH_BASE && cmd < CMD_BRUSH_BASE + B_COUNT) { g_brush = cmd - CMD_BRUSH_BASE; set_tool(T_BRUSH); ribbon_layout(); write_dump(); return; }
    if (cmd >= CMD_SHAPE_BASE && cmd < CMD_SHAPE_BASE + S_COUNT) { g_shape = cmd - CMD_SHAPE_BASE; set_tool(T_SHAPE); ribbon_layout(); write_dump(); return; }
    if (cmd >= CMD_SIZE_BASE && cmd < CMD_SIZE_BASE + 4) { g_size_idx = cmd - CMD_SIZE_BASE; ribbon_layout(); write_dump(); return; }
    switch (cmd)
    {
    case CMD_NEW: file_new(); break;
    case CMD_OPEN: file_open(); break;
    case CMD_SAVE: save(); break;
    case CMD_SAVEAS: text_commit(); sel_commit(); save_as(-1); break;
    case CMD_SAVEAS_PNG: case CMD_SAVEAS_JPEG: case CMD_SAVEAS_BMP: case CMD_SAVEAS_GIF: case CMD_SAVEAS_TIFF:
        text_commit(); sel_commit(); save_as(cmd - CMD_SAVEAS_PNG); break;
    case CMD_PROPERTIES: DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_PROPS), g_main, props_proc); break;
    case CMD_ABOUT: about(); break;
    case CMD_EXIT: PostMessageW(g_main, WM_CLOSE, 0, 0); break;
    case CMD_UNDO:
        if (text_active()) { text_cancel(); break; }
        sel_clear();
        if (do_undo()) { set_dirty(); canvas_changed(TRUE); }
        break;
    case CMD_REDO:
        text_commit(); sel_clear();
        if (do_redo()) { set_dirty(); canvas_changed(TRUE); }
        break;
    case CMD_CUT: if (sel_copy()) sel_delete(); break;
    case CMD_COPY: sel_copy(); break;
    case CMD_PASTE:
        text_commit();
        if (clip_get(&im)) { sel_paste(&im); img_free(&im); }
        break;
    case CMD_PASTEFROM: text_commit(); paste_from(); break;
    case CMD_SELECTALL: text_commit(); sel_all(); break;
    case CMD_INVERTSEL: text_commit(); sel_invert(); break;
    case CMD_DELETE: sel_delete(); break;
    case CMD_TRANSPARENT: g_transparent_sel = !g_transparent_sel; break;
    case CMD_CROP: text_commit(); sel_crop(); break;
    case CMD_RESIZE: text_commit(); DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_RESIZE), g_main, resize_proc); break;
    case CMD_ROT_R: case CMD_ROT_L: case CMD_ROT_180: case CMD_FLIP_V: case CMD_FLIP_H:
        text_commit();
        if (sel_active()) sel_transform(cmd); else whole(cmd);
        break;
    case CMD_ZOOMIN: canvas_zoom(zoom_step(1), -1, -1); break;
    case CMD_ZOOMOUT: canvas_zoom(zoom_step(-1), -1, -1); break;
    case CMD_ZOOM100: canvas_zoom(1000, -1, -1); break;
    case CMD_GRID: g_grid = !g_grid; InvalidateRect(g_canvas, NULL, FALSE); break;
    case CMD_STATUSBAR: g_statusbar_on = !g_statusbar_on; layout(); break;
    case CMD_FULLSCREEN: full_screen(); break;
    case CMD_EDITCOLORS: edit_colors(); break;
    case CMD_ESCAPE: canvas_escape(); break;
    case CMD_TAB_HOME: g_tab = 0; ribbon_layout(); break;
    case CMD_TAB_VIEW: g_tab = 1; ribbon_layout(); break;
    case CMD_OUTLINE_NONE: g_outline_on = 0; if (!g_fill_on) g_fill_on = 0; break;
    case CMD_OUTLINE_SOLID: g_outline_on = 1; break;
    case CMD_FILL_NONE: g_fill_on = 0; break;
    case CMD_FILL_SOLID: g_fill_on = 1; break;
    case CMD_TEXT_OPAQUE: g_text_opaque = 1; text_commit(); break;
    case CMD_TEXT_TRANSPARENT: g_text_opaque = 0; break;
    case CMD_TEXT_FONT:
    {
        CHOOSEFONTW cf = { sizeof(cf) };
        LOGFONTW lf = g_text_font;
        cf.hwndOwner = g_main; cf.lpLogFont = &lf; cf.Flags = CF_INITTOLOGFONTSTRUCT | CF_SCREENFONTS | CF_EFFECTS;
        cf.rgbColors = g_color1;
        if (ChooseFontW(&cf)) { g_text_font = lf; g_color1 = cf.rgbColors; }
        break;
    }
    }
    InvalidateRect(g_ribbon, NULL, FALSE);
    InvalidateRect(g_status, NULL, FALSE);
    write_dump();
}

void status_message(const WCHAR *s) { lstrcpynW(g_status_msg, s, ARRAYSIZE(g_status_msg)); }

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (sg_mode_changed(msg, lp)) sgm_follow(hwnd);
    switch (msg)
    {
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(400); ((MINMAXINFO *)lp)->ptMinTrackSize.y = S(300); return 0;
    case WM_COMMAND:
        if (!lp || HIWORD(wp) == 0) { do_command(LOWORD(wp)); return 0; }
        break;
    case WM_CLOSE:
        if (confirm_discard()) DestroyWindow(hwnd);
        return 0;
    case WM_TIMER: write_dump(); return 0;
    case WM_SETFOCUS: SetFocus(g_canvas); return 0;
    case WM_DPICHANGED:
    {
        RECT *r = (RECT *)lp;
        g_dpi = HIWORD(wp);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ribbon_layout();
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HACCEL make_accel(void)
{
    ACCEL a[] = {
        { FCONTROL | FVIRTKEY, 'N', CMD_NEW }, { FCONTROL | FVIRTKEY, 'O', CMD_OPEN }, { FCONTROL | FVIRTKEY, 'S', CMD_SAVE },
        { FVIRTKEY, VK_F12, CMD_SAVEAS }, { FCONTROL | FVIRTKEY, 'Z', CMD_UNDO }, { FCONTROL | FVIRTKEY, 'Y', CMD_REDO },
        { FCONTROL | FVIRTKEY, 'X', CMD_CUT }, { FCONTROL | FVIRTKEY, 'C', CMD_COPY }, { FCONTROL | FVIRTKEY, 'V', CMD_PASTE },
        { FCONTROL | FVIRTKEY, 'A', CMD_SELECTALL }, { FVIRTKEY, VK_DELETE, CMD_DELETE },
        { FCONTROL | FVIRTKEY, 'E', CMD_PROPERTIES }, { FCONTROL | FVIRTKEY, 'W', CMD_RESIZE },
        { FCONTROL | FSHIFT | FVIRTKEY, 'X', CMD_CROP }, { FCONTROL | FVIRTKEY, 'G', CMD_GRID },
        { FCONTROL | FVIRTKEY, VK_PRIOR, CMD_ZOOMIN }, { FCONTROL | FVIRTKEY, VK_NEXT, CMD_ZOOMOUT },
        { FCONTROL | FVIRTKEY, VK_ADD, CMD_ZOOMIN }, { FCONTROL | FVIRTKEY, VK_SUBTRACT, CMD_ZOOMOUT },
        { FVIRTKEY, VK_F11, CMD_FULLSCREEN }, { FVIRTKEY, VK_ESCAPE, CMD_ESCAPE },
        { FCONTROL | FVIRTKEY, 'R', CMD_ROT_R },
    };
    return CreateAcceleratorTableW(a, ARRAYSIZE(a));
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    MSG m;
    int argc, i;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const WCHAR *file = NULL;
    HDC sdc;
    (void)prev; (void)cmdline;

    g_inst = inst;
    SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    sdc = GetDC(NULL); g_dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(NULL, sdc);
    if (g_dpi < 96) g_dpi = 96;
    {
        static WCHAR dump[MAX_PATH];
        if (GetEnvironmentVariableW(L"SG_PAINT_DUMP", dump, MAX_PATH) && dump[0]) g_dump_path = dump;
    }
    for (i = 1; argv && i < argc; i++)
        if (argv[i][0] != '/' || wcschr(argv[i], '\\') || wcschr(argv[i] + 1, '/')) { file = argv[i]; break; }

    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_small = CreateFontW(-MulDiv(8, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    memset(&g_text_font, 0, sizeof(g_text_font));
    lstrcpyW(g_text_font.lfFaceName, L"Segoe UI");
    g_text_font.lfHeight = -MulDiv(11, 96, 72);
    g_text_font.lfWeight = FW_NORMAL;
    g_text_font.lfQuality = ANTIALIASED_QUALITY;

    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_PAINT));
    wc.hIconSm = LoadImageW(inst, MAKEINTRESOURCEW(IDI_PAINT), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpfnWndProc = main_proc; wc.lpszClassName = L"SgPaintMain";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);
    wc.hIcon = wc.hIconSm = NULL; wc.hbrBackground = NULL;
    wc.lpfnWndProc = ribbon_proc; wc.lpszClassName = L"SgPaintRibbon"; RegisterClassExW(&wc);
    wc.lpfnWndProc = status_proc; wc.lpszClassName = L"SgPaintStatus"; RegisterClassExW(&wc);
    wc.lpfnWndProc = canvas_proc; wc.lpszClassName = L"SgPaintCanvas"; wc.style = CS_DBLCLKS; wc.hCursor = NULL; RegisterClassExW(&wc);

    file_new();
    {
        /* wide enough for the whole ribbon, as far as the screen allows */
        RECT wa;
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        sgm_dark = sg_apps_dark();
        g_main = CreateWindowExW(0, L"SgPaintMain", L"Untitled - Paint", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                 CW_USEDEFAULT, CW_USEDEFAULT, min(S(1000), (int)(wa.right - wa.left)),
                                 min(S(700), (int)(wa.bottom - wa.top)), NULL, NULL, inst, NULL);
        if (g_main) sg_mode_title(g_main, sgm_dark);
    }
    g_ribbon = CreateWindowExW(0, L"SgPaintRibbon", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g_main, NULL, inst, NULL);
    g_canvas = CreateWindowExW(0, L"SgPaintCanvas", NULL, WS_CHILD | WS_VISIBLE | WS_HSCROLL | WS_VSCROLL | WS_CLIPCHILDREN,
                               0, 0, 0, 0, g_main, NULL, inst, NULL);
    g_status = CreateWindowExW(0, L"SgPaintStatus", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g_main, NULL, inst, NULL);
    ribbon_layout();
    layout();
    update_title();
    if (file) open_path(file);
    canvas_changed(TRUE);
    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    SetFocus(g_canvas);
    g_accel = make_accel();
    if (g_dump_path) SetTimer(g_main, 1, 300, NULL);
    write_dump();
    if (argv) LocalFree(argv);

    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        /* the text box keeps its own keys (Ctrl+A, Ctrl+C, Delete ...) */
        HWND focus = GetFocus();
        BOOL in_edit = focus && GetParent(focus) == g_canvas;
        HWND top = GetAncestor(m.hwnd, GA_ROOT);
        if (top == g_main && !in_edit && TranslateAcceleratorW(g_main, g_accel, &m)) continue;
        if (in_edit && m.message == WM_KEYDOWN && m.wParam == VK_ESCAPE) { text_commit(); continue; }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    CoUninitialize();
    return 0;
}
