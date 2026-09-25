/* sg-fontview -- the Fonts folder (Control Panel > Appearance and
 * Personalization > Fonts; control fonts, shell:fonts, %WINDIR%\Fonts).
 *
 * Windows 10's Fonts view in the Control Panel's frame: the address, a left
 * pane (Control Panel Home, Install new font, Find a character), "Preview,
 * delete, or show and hide the fonts installed on your computer", a command
 * bar (Preview, Delete, Install new font), a search box, a tile per family --
 * a page with "Abg" drawn in the family's own font, stacked when it has
 * several styles -- and a details pane for the selection (styles, where it
 * is installed, its files).
 *
 * A person's own fonts are theirs to delete; fonts installed for all users
 * need an administrator (the elevated copy does it); Linux's and Wine's own
 * fonts are the system's. Files dropped on the window are installed for the
 * person. Enter/double-click previews (fontview), Delete deletes.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "fontview.h"
#include <shellapi.h>
#include <commdlg.h>
#include <shlwapi.h>

enum { ID_SEARCH = 200, ID_PREVIEW, ID_DELETE, ID_INSTALLNEW, ID_HOME, ID_CHARMAP, ID_GRID };

static struct {
    HWND hwnd, grid, search, preview, del, installnew, home, charmap;
    struct family *fam;
    int nfam;
    int *shown, nshown;         /* indexes into fam after the search */
    int sel;                    /* index into shown, -1 */
    int scroll, cols, rows_h;
    HFONT *tile_fonts;          /* per family, made when first drawn */
    HFONT ui, ui_small, title, details_big;
    WCHAR filter[128];
    /* families deleted here: GDI in this process may still list a font it
     * had loaded (Wine keeps a process's font list), the folder must not */
    WCHAR gone[16][LF_FACESIZE];
    int ngone;
    BOOL busy;                  /* installing or deleting: our own WM_FONTCHANGE waits */
    WCHAR status[512];
} F;

#define TILE_W S(112)
#define TILE_H S(118)
#define HEADER_H S(112)
#define DETAILS_H S(96)
#define LEFT_W S(200)

/* ---- the list ---------------------------------------------------------------------------- */
static void apply_filter(void)
{
    int i;
    free(F.shown);
    F.shown = malloc((F.nfam ? F.nfam : 1) * sizeof(int));
    F.nshown = 0;
    for (i = 0; i < F.nfam; i++) {
        int g;
        BOOL gone = FALSE;
        for (g = 0; g < F.ngone; g++) if (!lstrcmpiW(F.gone[g], F.fam[i].name)) gone = TRUE;
        if (gone) continue;
        if (!F.filter[0] || StrStrIW(F.fam[i].name, F.filter)) F.shown[F.nshown++] = i;
    }
    if (F.sel >= F.nshown) F.sel = F.nshown ? 0 : -1;
    F.scroll = 0;
}

static void reload(void)
{
    WCHAR keep[LF_FACESIZE] = L"";
    int i;
    if (F.sel >= 0 && F.sel < F.nshown) lstrcpynW(keep, F.fam[F.shown[F.sel]].name, LF_FACESIZE);
    if (F.tile_fonts) {
        for (i = 0; i < F.nfam; i++) if (F.tile_fonts[i]) DeleteObject(F.tile_fonts[i]);
        free(F.tile_fonts);
    }
    if (F.fam) families_free(F.fam, F.nfam);
    F.nfam = families_load(&F.fam);
    F.tile_fonts = calloc(F.nfam ? F.nfam : 1, sizeof(HFONT));
    F.sel = -1;
    apply_filter();
    for (i = 0; keep[0] && i < F.nshown; i++)
        if (!lstrcmpiW(F.fam[F.shown[i]].name, keep)) F.sel = i;
    if (F.sel >= 0) family_files(&F.fam[F.shown[F.sel]]);
}

static struct family *selected(void)
{
    return F.sel >= 0 && F.sel < F.nshown ? &F.fam[F.shown[F.sel]] : NULL;
}

/* ---- the dump ------------------------------------------------------------------------------- */
static void folder_dump(void)
{
    FILE *f;
    struct family *s = selected();
    RECT gr;
    POINT o = { 0, 0 };
    int i;
    if (F.preview) {
        EnableWindow(F.preview, s != NULL);
        EnableWindow(F.del, s != NULL);
    }
    if (!g_dump_path[0] || !(f = _wfopen(g_dump_path, L"w"))) return;
    fprintf(f, "window=folder\ntitle=Fonts\nfamilies=%d\nshown=%d\nfilter=%ls\nstatus=%ls\n", F.nfam, F.nshown, F.filter, F.status);
    GetClientRect(F.grid, &gr);
    ClientToScreen(F.grid, &o);
    fprintf(f, "grid=%ld,%ld,%ld,%ld\ncols=%d\nscroll=%d\n", o.x, o.y, o.x + gr.right, o.y + gr.bottom, F.cols, F.scroll);
    {
        RECT r;
        HWND hs[] = { F.search, F.preview, F.del, F.installnew, F.home, F.charmap };
        const char *ns[] = { "search", "preview", "delete", "installnew", "home", "charmap" };
        for (i = 0; i < 6; i++) {
            GetWindowRect(hs[i], &r);
            fprintf(f, "%s=%ld,%ld,%d\n", ns[i], (r.left + r.right) / 2, (r.top + r.bottom) / 2, IsWindowEnabled(hs[i]) ? 1 : 0);
        }
    }
    if (s) {
        fprintf(f, "selected=%ls\nsel_styles=%d\nsel_scope=%d\nsel_files=%d\n", s->name, s->styles, s->scope, s->nfiles);
        for (i = 0; i < s->nfiles; i++) fprintf(f, "sel_file=%d\t%ls\t%ls\n", s->files[i].scope, s->files[i].regname, s->files[i].path);
    } else fprintf(f, "selected=\n");
    for (i = 0; i < F.nshown; i++) {
        int col = F.cols ? i % F.cols : 0, row = F.cols ? i / F.cols : i;
        int x = o.x + S(12) + col * TILE_W + TILE_W / 2, y = o.y + S(8) + row * TILE_H - F.scroll + TILE_H / 2;
        if (y < o.y || y > o.y + gr.bottom) x = y = -1;
        fprintf(f, "tile=%d,%d\t%ls\n", x, y, F.fam[F.shown[i]].name);
    }
    fclose(f);
}

/* ---- the grid --------------------------------------------------------------------------------- */
static HFONT tile_font(int i)
{
    if (!F.tile_fonts[i]) {
        LOGFONTW lf = { 0 };
        lf.lfHeight = -S(26);
        lf.lfCharSet = F.fam[i].charset;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lstrcpynW(lf.lfFaceName, F.fam[i].name, LF_FACESIZE);
        F.tile_fonts[i] = CreateFontIndirectW(&lf);
    }
    return F.tile_fonts[i];
}

static void grid_metrics(void)
{
    RECT r;
    GetClientRect(F.grid, &r);
    F.cols = (r.right - S(24)) / TILE_W;
    if (F.cols < 1) F.cols = 1;
    F.rows_h = ((F.nshown + F.cols - 1) / F.cols) * TILE_H + S(16);
}

static void grid_scrollbar(void)
{
    RECT r;
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    grid_metrics();
    GetClientRect(F.grid, &r);
    if (F.scroll > F.rows_h - r.bottom) F.scroll = F.rows_h - r.bottom;
    if (F.scroll < 0) F.scroll = 0;
    si.nMax = F.rows_h;
    si.nPage = r.bottom + 1;
    si.nPos = F.scroll;
    SetScrollInfo(F.grid, SB_VERT, &si, TRUE);
}

static void ensure_visible(void)
{
    RECT r;
    int top;
    if (F.sel < 0) return;
    GetClientRect(F.grid, &r);
    top = S(8) + (F.sel / F.cols) * TILE_H;
    if (top < F.scroll) F.scroll = top - S(8);
    else if (top + TILE_H > F.scroll + r.bottom) F.scroll = top + TILE_H - r.bottom + S(8);
    grid_scrollbar();
}

static void draw_page(HDC dc, int x, int y, int w, int h)
{
    POINT pts[5];
    int fold = w / 4;
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xA0, 0xA0, 0xA8)), op;
    HBRUSH br = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF)), ob;
    pts[0].x = x; pts[0].y = y;
    pts[1].x = x + w - fold; pts[1].y = y;
    pts[2].x = x + w; pts[2].y = y + fold;
    pts[3].x = x + w; pts[3].y = y + h;
    pts[4].x = x; pts[4].y = y + h;
    op = SelectObject(dc, pen);
    ob = SelectObject(dc, br);
    Polygon(dc, pts, 5);
    MoveToEx(dc, x + w - fold, y, NULL);
    LineTo(dc, x + w - fold, y + fold);
    LineTo(dc, x + w, y + fold);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(pen);
    DeleteObject(br);
}

static void paint_grid(HDC dc, RECT *rc)
{
    int i;
    FillRect(dc, rc, GetStockObject(WHITE_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < F.nshown; i++) {
        int fi = F.shown[i], col = i % F.cols, row = i / F.cols;
        int x = S(12) + col * TILE_W, y = S(8) + row * TILE_H - F.scroll;
        int pw = S(58), ph = S(66), px = x + (TILE_W - pw) / 2, py = y + S(6);
        RECT t;
        if (y + TILE_H < 0 || y > rc->bottom) continue;
        if (i == F.sel) {
            HBRUSH b = CreateSolidBrush(COL_SELECT);
            RECT s = { x + S(2), y, x + TILE_W - S(2), y + TILE_H - S(4) };
            FillRect(dc, &s, b);
            DeleteObject(b);
        }
        if (F.fam[fi].styles > 1) {
            draw_page(dc, px + S(8), py - S(4), pw, ph);
            draw_page(dc, px + S(4), py - S(2), pw, ph);
        }
        draw_page(dc, px, py, pw, ph);
        SelectObject(dc, tile_font(fi));
        SetTextColor(dc, COL_TEXT);
        SetRect(&t, px, py + S(14), px + pw, py + ph - S(6));
        DrawTextW(dc, F.fam[fi].symbol ? L"\xF041\xF062\xF067" : L"Abg", -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, F.ui_small);
        SetTextColor(dc, COL_TEXT);
        SetRect(&t, x + S(4), py + ph + S(6), x + TILE_W - S(4), y + TILE_H - S(4));
        DrawTextW(dc, F.fam[fi].name, -1, &t, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    if (!F.nshown) {
        RECT t = *rc;
        SelectObject(dc, F.ui);
        SetTextColor(dc, COL_SUBTLE);
        t.top += S(24);
        DrawTextW(dc, L"No items match your search.", -1, &t, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

static void select_index(int i)
{
    if (i < 0 || i >= F.nshown) return;
    F.sel = i;
    family_files(&F.fam[F.shown[i]]);
    ensure_visible();
    InvalidateRect(F.grid, NULL, FALSE);
    InvalidateRect(F.hwnd, NULL, TRUE);
    EnableWindow(F.preview, TRUE);
    EnableWindow(F.del, TRUE);
    folder_dump();
}

static void preview_selected(void)
{
    struct family *s = selected();
    WCHAR self[MAX_PATH], args[MAX_PATH + 32];
    if (!s) return;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    if (s->nfiles) _snwprintf(args, ARRAYSIZE(args), L"\"%ls\"", s->files[0].path);
    else _snwprintf(args, ARRAYSIZE(args), L"/family \"%ls\"", s->name);
    ShellExecuteW(F.hwnd, NULL, self, args, NULL, SW_SHOWNORMAL);
}

static void delete_selected(void)
{
    struct family *sel = selected(), copy, *s = &copy;
    WCHAR msg[512], q[512], args[MAX_PATH + 64];
    int i, user = 0, machine = 0;
    DWORD err = 0, code;
    if (!sel) return;
    /* our own copy: the uninstall broadcasts WM_FONTCHANGE, and a reload frees the list */
    copy = *sel;
    copy.files = malloc((sel->nfiles ? sel->nfiles : 1) * sizeof(*copy.files));
    if (!copy.files) return;
    memcpy(copy.files, sel->files, sel->nfiles * sizeof(*copy.files));
    for (i = 0; i < s->nfiles; i++) {
        if (s->files[i].scope == SCOPE_USER) user++;
        else if (s->files[i].scope == SCOPE_MACHINE) machine++;
    }
    if (!user && !machine) {
        _snwprintf(msg, ARRAYSIZE(msg), L"%ls is a system font and can't be deleted.", s->name);
        lstrcpynW(F.status, msg, ARRAYSIZE(F.status));
        folder_dump();
        free(copy.files);
        message_box(F.hwnd, msg, TRUE);
        return;
    }
    _snwprintf(q, ARRAYSIZE(q), L"Are you sure you want to permanently delete this font family?\n\n%ls", s->name);
    if (!GetEnvironmentVariableW(L"SG_FONTVIEW_YES", args, 4) &&
        MessageBoxW(F.hwnd, q, L"Delete Font", MB_YESNO | MB_ICONWARNING) != IDYES) { free(copy.files); return; }
    msg[0] = 0;
    F.busy = TRUE;
    for (i = 0; i < s->nfiles && !err; i++) {
        if (s->files[i].scope == SCOPE_USER)
            err = font_uninstall(s->files[i].path, SCOPE_USER, msg, ARRAYSIZE(msg));
        else if (s->files[i].scope == SCOPE_MACHINE) {
            if (font_is_elevated()) err = font_uninstall(s->files[i].path, SCOPE_MACHINE, msg, ARRAYSIZE(msg));
            else {
                _snwprintf(args, ARRAYSIZE(args), L"/uninstall /allusers \"%ls\"", s->files[i].path);
                if (!run_self(F.hwnd, args, TRUE, &code)) code = ERROR_ACCESS_DENIED;
                err = code;
                if (err) lstrcpyW(msg, L"You need to be an administrator to delete fonts installed for all users.");
            }
        }
    }
    if (!err) {
        _snwprintf(msg, ARRAYSIZE(msg), L"%ls was deleted.", s->name);
        if (F.ngone < (int)ARRAYSIZE(F.gone)) lstrcpynW(F.gone[F.ngone++], s->name, LF_FACESIZE);
    }
    F.busy = FALSE;
    free(copy.files);
    lstrcpynW(F.status, msg, ARRAYSIZE(F.status));
    reload();
    InvalidateRect(F.hwnd, NULL, TRUE);
    InvalidateRect(F.grid, NULL, FALSE);
    grid_scrollbar();
    folder_dump();
    if (err) message_box(F.hwnd, msg, TRUE);
}

static void install_files(const WCHAR *const *paths, int n)
{
    WCHAR msg[512];
    int i, done = 0;
    DWORD err = 0;
    msg[0] = 0;
    F.busy = TRUE;
    for (i = 0; i < n; i++) {
        struct font_file ff;
        if (font_read(paths[i], &ff) && (font_installed_scope(&ff) & SCOPE_USER)) {
            WCHAR q[MAX_PATH + 128];
            _snwprintf(q, ARRAYSIZE(q), L"The font %ls is already installed. Do you want to replace it?", PathFindFileNameW(paths[i]));
            if (!GetEnvironmentVariableW(L"SG_FONTVIEW_YES", msg, 4) &&
                MessageBoxW(F.hwnd, q, L"Install Font", MB_YESNO | MB_ICONQUESTION) != IDYES) continue;
        }
        if (!(err = font_install(paths[i], SCOPE_USER, msg, ARRAYSIZE(msg)))) {
            int j, g;
            done++;
            /* installed again: no longer gone */
            for (j = 0; j < ff.info.nfaces; j++) {
                WCHAR fam[128];
                utf8_to_w(ff.info.faces[j].family, fam, ARRAYSIZE(fam));
                for (g = 0; g < F.ngone; g++)
                    if (!lstrcmpiW(F.gone[g], fam)) lstrcpynW(F.gone[g], F.gone[--F.ngone], LF_FACESIZE), g--;
            }
        }
        else break;
    }
    F.busy = FALSE;
    lstrcpynW(F.status, msg, ARRAYSIZE(F.status));
    reload();
    InvalidateRect(F.hwnd, NULL, TRUE);
    InvalidateRect(F.grid, NULL, FALSE);
    grid_scrollbar();
    folder_dump();
    if (err) message_box(F.hwnd, msg, TRUE);
    (void)done;
}

static void install_new(void)
{
    WCHAR buf[8192] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    const WCHAR *paths[64];
    WCHAR full[64][MAX_PATH];
    int n = 0;
    ofn.hwndOwner = F.hwnd;
    ofn.lpstrFilter = L"Font files (*.ttf;*.otf;*.ttc;*.fon)\0*.ttf;*.otf;*.ttc;*.fon\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = ARRAYSIZE(buf);
    ofn.lpstrTitle = L"Install new font";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return;
    if (!buf[lstrlenW(buf) + 1]) paths[n++] = buf;         /* one file */
    else {
        const WCHAR *p = buf + lstrlenW(buf) + 1;
        while (*p && n < 64) {
            _snwprintf(full[n], MAX_PATH, L"%ls\\%ls", buf, p);
            paths[n] = full[n];
            n++;
            p += lstrlenW(p) + 1;
        }
    }
    install_files(paths, n);
}

static LRESULT CALLBACK grid_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT r;
        HDC dc = BeginPaint(hwnd, &ps), mem = CreateCompatibleDC(dc);
        HBITMAP bmp, ob;
        GetClientRect(hwnd, &r);
        bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
        ob = SelectObject(mem, bmp);
        paint_grid(mem, &r);
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        grid_scrollbar();
        folder_dump();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        int x = (short)LOWORD(lp) - S(12), y = (short)HIWORD(lp) - S(8) + F.scroll, i;
        SetFocus(hwnd);
        if (x < 0 || y < 0 || x / TILE_W >= F.cols) return 0;
        i = (y / TILE_H) * F.cols + x / TILE_W;
        if (i < F.nshown) {
            select_index(i);
            if (msg == WM_LBUTTONDBLCLK) preview_selected();
        }
        return 0;
    }
    case WM_MOUSEWHEEL:
        F.scroll -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * TILE_H / 2;
        grid_scrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        folder_dump();
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        RECT r;
        GetScrollInfo(hwnd, SB_VERT, &si);
        GetClientRect(hwnd, &r);
        switch (LOWORD(wp)) {
        case SB_LINEUP: F.scroll -= TILE_H / 2; break;
        case SB_LINEDOWN: F.scroll += TILE_H / 2; break;
        case SB_PAGEUP: F.scroll -= r.bottom; break;
        case SB_PAGEDOWN: F.scroll += r.bottom; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: F.scroll = si.nTrackPos; break;
        }
        grid_scrollbar();
        InvalidateRect(hwnd, NULL, FALSE);
        folder_dump();
        return 0;
    }
    case WM_GETDLGCODE: {
        const MSG *m = (const MSG *)lp;
        return DLGC_WANTARROWS | DLGC_WANTCHARS | (m && m->message == WM_KEYDOWN && m->wParam != VK_TAB ? DLGC_WANTMESSAGE : 0);
    }
    case WM_KEYDOWN: {
        int s = F.sel < 0 ? 0 : F.sel;
        switch (wp) {
        case VK_LEFT: select_index(s - 1); return 0;
        case VK_RIGHT: select_index(F.sel < 0 ? 0 : s + 1); return 0;
        case VK_UP: select_index(s - F.cols); return 0;
        case VK_DOWN: select_index(F.sel < 0 ? 0 : s + F.cols); return 0;
        case VK_HOME: select_index(0); return 0;
        case VK_END: select_index(F.nshown - 1); return 0;
        case VK_DELETE: delete_selected(); return 0;
        }
        break;
    }
    case WM_CHAR:
        if (wp == '\r') { preview_selected(); return 0; }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hwnd, NULL, FALSE);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- the frame --------------------------------------------------------------------------- */
static void layout(void)
{
    RECT r;
    int x = LEFT_W + S(20);
    GetClientRect(F.hwnd, &r);
    MoveWindow(F.search, r.right - S(232), S(10), S(220), S(26), TRUE);
    MoveWindow(F.home, S(16), S(56), LEFT_W - S(24), S(22), TRUE);
    MoveWindow(F.charmap, S(16), S(84), LEFT_W - S(24), S(22), TRUE);
    MoveWindow(F.preview, x, HEADER_H - S(36), S(90), S(28), TRUE);
    MoveWindow(F.del, x + S(98), HEADER_H - S(36), S(90), S(28), TRUE);
    MoveWindow(F.installnew, x + S(196), HEADER_H - S(36), S(130), S(28), TRUE);
    MoveWindow(F.grid, LEFT_W, HEADER_H, r.right - LEFT_W, r.bottom - HEADER_H - DETAILS_H, TRUE);
}

static void paint_frame(HDC dc)
{
    RECT r, t;
    struct family *s = selected();
    HBRUSH pane = CreateSolidBrush(COL_BAR);
    WCHAR buf[1024];
    GetClientRect(F.hwnd, &r);
    FillRect(dc, &r, GetStockObject(WHITE_BRUSH));
    SetRect(&t, 0, 0, LEFT_W, r.bottom - DETAILS_H);
    FillRect(dc, &t, pane);
    SetBkMode(dc, TRANSPARENT);
    /* the address */
    SelectObject(dc, F.ui);
    SetTextColor(dc, COL_SUBTLE);
    SetRect(&t, S(16), S(12), r.right - S(250), S(36));
    DrawTextW(dc, L"Control Panel  \x203A  Appearance and Personalization  \x203A  Fonts", -1, &t, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    /* the heading */
    SelectObject(dc, F.title);
    SetTextColor(dc, COL_TITLE);
    SetRect(&t, LEFT_W + S(20), S(46), r.right - S(12), S(74));
    DrawTextW(dc, L"Preview, delete, or show and hide the fonts installed on your computer", -1, &t,
              DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    /* the details pane */
    SetRect(&t, 0, r.bottom - DETAILS_H, r.right, r.bottom);
    FillRect(dc, &t, pane);
    if (s) {
        LOGFONTW lf = { 0 };
        HFONT big;
        const WCHAR *where = s->scope == SCOPE_MACHINE ? L"For all users" : s->scope == SCOPE_USER ? L"For you" : L"With the system";
        int i, o;
        lf.lfHeight = -S(24);
        lf.lfCharSet = s->charset;
        lstrcpynW(lf.lfFaceName, s->name, LF_FACESIZE);
        big = CreateFontIndirectW(&lf);
        SelectObject(dc, big);
        SetTextColor(dc, COL_TEXT);
        TextOutW(dc, S(24), r.bottom - DETAILS_H + S(12), s->symbol ? L"\xF041\xF062\xF067" : L"Abg", 3);
        SelectObject(dc, F.details_big);
        TextOutW(dc, S(120), r.bottom - DETAILS_H + S(10), s->name, lstrlenW(s->name));
        SelectObject(dc, F.ui);
        SetTextColor(dc, COL_SUBTLE);
        _snwprintf(buf, ARRAYSIZE(buf), L"Font styles: %d     Installed: %ls     Type: %ls", s->styles, where,
                   s->raster ? L"Raster" : s->truetype ? L"TrueType / OpenType" : L"Vector");
        TextOutW(dc, S(120), r.bottom - DETAILS_H + S(40), buf, lstrlenW(buf));
        o = _snwprintf(buf, ARRAYSIZE(buf), L"Files: ");
        for (i = 0; i < s->nfiles && o < (int)ARRAYSIZE(buf) - 2; i++)
            o += _snwprintf(buf + o, ARRAYSIZE(buf) - o, L"%ls%ls", i ? L"; " : L"", s->files[i].path);
        if (!s->nfiles) _snwprintf(buf + o, ARRAYSIZE(buf) - o, L"(provided by the system)");
        buf[ARRAYSIZE(buf) - 1] = 0;
        SetRect(&t, S(120), r.bottom - DETAILS_H + S(60), r.right - S(12), r.bottom - S(4));
        DrawTextW(dc, buf, -1, &t, DT_NOPREFIX | DT_END_ELLIPSIS | DT_SINGLELINE);
        DeleteObject(big);
    } else {
        SelectObject(dc, F.ui);
        SetTextColor(dc, COL_SUBTLE);
        _snwprintf(buf, ARRAYSIZE(buf), L"%d items", F.nshown);
        TextOutW(dc, S(24), r.bottom - DETAILS_H + S(16), buf, lstrlenW(buf));
    }
    if (F.status[0]) {
        SelectObject(dc, F.ui_small);
        SetTextColor(dc, COL_ACCENT);
        SetRect(&t, LEFT_W + S(20), S(76), r.right - S(12), HEADER_H - S(40));
        DrawTextW(dc, F.status, -1, &t, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    DeleteObject(pane);
}

static WNDPROC g_search_proc;
static LRESULT CALLBACK search_sub(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { SetWindowTextW(hwnd, L""); return 0; }
    if (msg == WM_KEYDOWN && (wp == VK_DOWN || wp == VK_RETURN)) { SetFocus(F.grid); if (F.sel < 0) select_index(0); return 0; }
    if (msg == WM_CHAR && (wp == '\r' || wp == 27)) return 0;
    return CallWindowProcW(g_search_proc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK folder_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        layout();
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint_frame(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC: {
        static HBRUSH bar;
        if (!bar) bar = CreateSolidBrush(COL_BAR);
        SetBkColor((HDC)wp, COL_BAR);
        SetTextColor((HDC)wp, RGB(0x00, 0x66, 0xCC));
        return (LRESULT)bar;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SEARCH:
            if (HIWORD(wp) == EN_CHANGE) {
                GetWindowTextW(F.search, F.filter, ARRAYSIZE(F.filter));
                apply_filter();
                if (F.sel >= 0) family_files(&F.fam[F.shown[F.sel]]);
                grid_scrollbar();
                InvalidateRect(F.grid, NULL, FALSE);
                InvalidateRect(hwnd, NULL, FALSE);
                folder_dump();
            }
            return 0;
        case ID_PREVIEW: preview_selected(); return 0;
        case ID_DELETE: delete_selected(); return 0;
        case ID_INSTALLNEW: install_new(); return 0;
        case ID_HOME: ShellExecuteW(hwnd, NULL, L"control.exe", NULL, NULL, SW_SHOWNORMAL); return 0;
        case ID_CHARMAP: ShellExecuteW(hwnd, NULL, L"charmap.exe", NULL, NULL, SW_SHOWNORMAL); return 0;
        }
        break;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0), i;
        WCHAR (*names)[MAX_PATH] = calloc(n ? n : 1, sizeof(*names));
        const WCHAR **paths = calloc(n ? n : 1, sizeof(*paths));
        for (i = 0; names && paths && i < n; i++) { DragQueryFileW(drop, i, names[i], MAX_PATH); paths[i] = names[i]; }
        DragFinish(drop);
        if (names && paths) install_files(paths, (int)n);
        free(names);
        free(paths);
        return 0;
    }
    case WM_FONTCHANGE:
        if (F.busy) return 0;           /* our own change: reloaded when it is done */
        reload();
        grid_scrollbar();
        InvalidateRect(F.grid, NULL, FALSE);
        InvalidateRect(hwnd, NULL, TRUE);
        folder_dump();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F5) { SendMessageW(hwnd, WM_FONTCHANGE, 0, 0); return 0; }
        if (wp == 'F' && GetKeyState(VK_CONTROL) < 0) { SetFocus(F.search); return 0; }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND button(const WCHAR *text, int id, DWORD style)
{
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 10, 10,
                             F.hwnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)F.ui, TRUE);
    return b;
}

static HWND link(const WCHAR *text, int id)
{
    HWND b = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_NOTIFY | SS_LEFT, 0, 0, 10, 10,
                             F.hwnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)F.ui, TRUE);
    return b;
}

int folder_main(int show)
{
    WNDCLASSW wc = { 0 };
    MSG msg;

    wc.lpfnWndProc = folder_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(2));
    wc.lpszClassName = L"SgFontsFolder";
    RegisterClassW(&wc);
    wc.lpfnWndProc = grid_proc;
    wc.hIcon = NULL;
    wc.style = CS_DBLCLKS;
    wc.lpszClassName = L"SgFontsGrid";
    RegisterClassW(&wc);

    F.ui = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    F.ui_small = CreateFontW(-S(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    F.title = CreateFontW(-S(17), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    F.details_big = CreateFontW(-S(16), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    F.sel = -1;
    reload();

    F.hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, L"SgFontsFolder", L"Fonts", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, S(940), S(640), NULL, NULL, g_inst, NULL);
    F.search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               0, 0, 10, 10, F.hwnd, (HMENU)ID_SEARCH, g_inst, NULL);
    SendMessageW(F.search, WM_SETFONT, (WPARAM)F.ui, TRUE);
    SendMessageW(F.search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search Fonts");
    g_search_proc = (WNDPROC)SetWindowLongPtrW(F.search, GWLP_WNDPROC, (LONG_PTR)search_sub);
    F.home = link(L"Control Panel Home", ID_HOME);
    F.charmap = link(L"Find a character", ID_CHARMAP);
    F.preview = button(L"Preview", ID_PREVIEW, BS_PUSHBUTTON);
    F.del = button(L"Delete", ID_DELETE, BS_PUSHBUTTON);
    F.installnew = button(L"Install new font...", ID_INSTALLNEW, BS_PUSHBUTTON);
    EnableWindow(F.preview, F.sel >= 0);
    EnableWindow(F.del, F.sel >= 0);
    F.grid = CreateWindowExW(0, L"SgFontsGrid", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP, 0, 0, 10, 10,
                             F.hwnd, (HMENU)ID_GRID, g_inst, NULL);
    layout();
    grid_scrollbar();
    ShowWindow(F.hwnd, show);
    UpdateWindow(F.hwnd);
    SetFocus(F.grid);
    folder_dump();
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(F.hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
