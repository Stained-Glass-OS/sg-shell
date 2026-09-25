/* sg-fontview -- Windows' font viewer (fontview.exe) and the Fonts folder.
 *
 *   fontview.exe FILE               show a font file: its names, version, what
 *                                   kind it is, the alphabet and a sample line
 *                                   at 12..72 points; Print, Install, Install
 *                                   for all users; a face picker for a .ttc
 *   fontview.exe /p FILE            print it on the default printer
 *   fontview.exe /install [/allusers] [/quiet] FILE
 *   fontview.exe /uninstall [/allusers] [/quiet] FILE|NAME
 *                                   (exit code 0, or the Windows error: 5 for
 *                                   a standard user asking for all users)
 *   fontview.exe /family NAME       show an installed family by name
 *   fontview.exe /folder            the Fonts folder (control fonts,
 *                                   shell:fonts, %WINDIR%\Fonts: wine-sg 0183)
 *   fontview.exe --families FILE    every family GDI enumerates, one per line
 *                                   (UTF-8), for gates and scripts
 *
 * The file is loaded with AddFontResourceEx(FR_PRIVATE) -- only this process
 * sees it until it is installed -- and its names are read from the file
 * itself (fontinfo.c), not from GDI, so the viewer shows what the file says.
 * SG_FONTVIEW_DUMP=<file> is rewritten after every change for the gates.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "fontview.h"
#include <shellapi.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <winspool.h>

HINSTANCE g_inst;
int g_dpi = 96;
WCHAR g_dump_path[MAX_PATH];

int S(int dip) { return MulDiv(dip, g_dpi, 96); }

static const int SIZES[] = { 12, 18, 24, 36, 48, 60, 72 };
static const WCHAR ALPHA[3][64] = {
    L"abcdefghijklmnopqrstuvwxyz",
    L"ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    L"1234567890.:,;' \" (!?) +-*/=",
};
static const WCHAR SAMPLE[] = L"The quick brown fox jumps over the lazy dog. 1234567890";

enum { ID_PRINT = 100, ID_INSTALL, ID_INSTALL_ALL, ID_FACE, ID_PANE };

static struct {
    HWND hwnd, pane, print, install, install_all, face_combo;
    struct font_file file;
    BOOL have_file;
    WCHAR family_only[LF_FACESIZE];     /* /family NAME */
    int face;                           /* in a collection */
    int added;                          /* AddFontResourceEx's count */
    int scroll, content_h;
    int scope;                          /* where it is installed */
    WCHAR status[512];
    HFONT ui, ui_bold, ui_big;
    int size_y[ARRAYSIZE(SIZES)];       /* each sample line's top, content coordinates */
    int alpha_y;
    WCHAR gdi_face[LF_FACESIZE];
} v;

void message_box(HWND owner, const WCHAR *text, BOOL error)
{
    FILE *f;
    if (g_dump_path[0] && (f = _wfopen(g_dump_path, L"a"))) { fprintf(f, "message=%ls\n", text); fclose(f); }
    MessageBoxW(owner, text, L"Font viewer", MB_OK | (error ? MB_ICONERROR : MB_ICONINFORMATION));
}

/* ShellExecute ourselves (elevated: "runas", the broker's consent prompt) and wait */
BOOL run_self(HWND owner, const WCHAR *args, BOOL elevated, DWORD *code)
{
    WCHAR self[MAX_PATH];
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    GetModuleFileNameW(NULL, self, MAX_PATH);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpVerb = elevated ? L"runas" : NULL;
    sei.lpFile = self;
    sei.lpParameters = args;
    sei.nShow = SW_SHOWNORMAL;
    *code = ERROR_CANCELLED;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return FALSE;
    for (;;) {
        MSG msg;
        DWORD r = MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    GetExitCodeProcess(sei.hProcess, code);
    CloseHandle(sei.hProcess);
    return TRUE;
}

/* ---- what the viewer shows -------------------------------------------------------- */
static const struct fi_face *cur_face(void)
{
    return v.have_file ? &v.file.info.faces[v.face] : NULL;
}

static void face_name(WCHAR *out, int cch)
{
    const struct fi_face *f = cur_face();
    if (f) utf8_to_w(f->family, out, cch);
    else lstrcpynW(out, v.family_only, cch);
}

static HFONT sample_font(int pixels, int dpi_for)
{
    LOGFONTW lf = { 0 };
    const struct fi_face *f = cur_face();
    (void)dpi_for;
    lf.lfHeight = -pixels;
    lf.lfWeight = f ? f->weight : FW_NORMAL;
    lf.lfItalic = f ? (BYTE)f->italic : 0;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfOutPrecision = f && (f->flags & FI_BITMAP) ? OUT_RASTER_PRECIS : OUT_TT_PRECIS;
    face_name(lf.lfFaceName, LF_FACESIZE);
    return CreateFontIndirectW(&lf);
}

/* draw everything at the given DPI into dc at (x, y - scroll), width w; returns the height.
 * Printing uses the same code at the printer's DPI. */
static int paint_content(HDC dc, int dpi, int x0, int y0, int w, BOOL record)
{
    const struct fi_face *f = cur_face();
    WCHAR buf[1024], name[256];
    int y = y0, i;
    RECT r;
    HFONT old, h;
    HPEN pen;
#define D(n) MulDiv(n, dpi, 96)
    HFONT big = CreateFontW(-D(22), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT body = CreateFontW(-D(13), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT small = CreateFontW(-D(11), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    SetBkMode(dc, TRANSPARENT);
    old = SelectObject(dc, big);
    SetTextColor(dc, COL_TEXT);
    if (f) utf8_to_w(f->full, name, ARRAYSIZE(name));
    else lstrcpynW(name, v.family_only, ARRAYSIZE(name));
    _snwprintf(buf, ARRAYSIZE(buf), L"Font name: %ls", name);
    TextOutW(dc, x0, y, buf, lstrlenW(buf));
    y += D(34);
    SelectObject(dc, body);
    if (f) {
        WCHAR t[256];
        char d8[256];
        utf8_to_w(f->version, t, ARRAYSIZE(t));
        if (t[0]) {
            _snwprintf(buf, ARRAYSIZE(buf), L"Version: %ls", t);
            TextOutW(dc, x0, y, buf, lstrlenW(buf));
            y += D(20);
        }
        fi_describe(f, d8, sizeof(d8));
        utf8_to_w(d8, t, ARRAYSIZE(t));
        if (t[0]) { TextOutW(dc, x0, y, t, lstrlenW(t)); y += D(20); }
        if (f->copyright[0]) {
            utf8_to_w(f->copyright, buf, ARRAYSIZE(buf));
            SelectObject(dc, small);
            SetTextColor(dc, COL_SUBTLE);
            SetRect(&r, x0, y, x0 + w, y + D(200));
            DrawTextW(dc, buf, -1, &r, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            r.right = x0 + w;
            DrawTextW(dc, buf, -1, &r, DT_WORDBREAK | DT_NOPREFIX);
            y = r.bottom + D(4);
            SetTextColor(dc, COL_TEXT);
        }
    }
    y += D(10);
    pen = CreatePen(PS_SOLID, 1, COL_RULE);
    SelectObject(dc, pen);
    MoveToEx(dc, x0, y, NULL); LineTo(dc, x0 + w, y);
    y += D(12);

    /* the alphabet */
    h = sample_font(D(20), dpi);
    SelectObject(dc, h);
    if (record) {
        v.alpha_y = y;
        GetTextFaceW(dc, LF_FACESIZE, v.gdi_face);
    }
    for (i = 0; i < 3; i++) {
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        TextOutW(dc, x0, y, ALPHA[i], lstrlenW(ALPHA[i]));
        y += tm.tmHeight + D(2);
    }
    SelectObject(dc, body);
    DeleteObject(h);
    y += D(10);
    MoveToEx(dc, x0, y, NULL); LineTo(dc, x0 + w, y);
    y += D(12);

    /* the sample line at each size, labelled */
    for (i = 0; i < (int)ARRAYSIZE(SIZES); i++) {
        TEXTMETRICW tm;
        WCHAR label[8];
        h = sample_font(MulDiv(SIZES[i], dpi, 72), dpi);
        SelectObject(dc, h);
        GetTextMetricsW(dc, &tm);
        if (record) v.size_y[i] = y;
        SetRect(&r, x0 + D(32), y, x0 + w, y + tm.tmHeight);
        DrawTextW(dc, SAMPLE, -1, &r, DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, small);
        SetTextColor(dc, COL_SUBTLE);
        _snwprintf(label, ARRAYSIZE(label), L"%d", SIZES[i]);
        TextOutW(dc, x0, y + tm.tmAscent - D(10), label, lstrlenW(label));
        SetTextColor(dc, COL_TEXT);
        DeleteObject(h);
        y += tm.tmHeight + D(8);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
    DeleteObject(big);
    DeleteObject(body);
    DeleteObject(small);
#undef D
    return y - y0 + MulDiv(16, dpi, 96);
}

/* ---- the dump ---------------------------------------------------------------------- */
static void button_centre(FILE *f, const char *name, HWND b)
{
    RECT r;
    if (!b || !IsWindowVisible(b)) { fprintf(f, "%s=-1,-1,0\n", name); return; }
    GetWindowRect(b, &r);
    fprintf(f, "%s=%ld,%ld,%d\n", name, (r.left + r.right) / 2, (r.top + r.bottom) / 2, IsWindowEnabled(b) ? 1 : 0);
}

static void viewer_dump(void)
{
    FILE *f;
    const struct fi_face *x = cur_face();
    WCHAR title[512];
    RECT r;
    POINT o = { 0, 0 };
    int i;
    char d8[256] = "";
    if (!g_dump_path[0] || !(f = _wfopen(g_dump_path, L"w"))) return;
    GetWindowTextW(v.hwnd, title, ARRAYSIZE(title));
    fprintf(f, "window=viewer\ntitle=%ls\n", title);
    if (v.have_file) {
        fprintf(f, "file=%ls\nkind=%d\nfaces=%d\nface=%d\nadded=%d\n", v.file.path, v.file.info.kind,
                v.file.info.nfaces, v.face, v.added);
        fprintf(f, "family=%s\nfull=%s\nsubfamily=%s\nversion=%s\nweight=%d\nitalic=%d\n", x->family, x->full,
                x->subfamily, x->version, x->weight, x->italic);
        fi_describe(x, d8, sizeof(d8));
        fprintf(f, "type=%s\ncopyright=%s\n", d8, x->copyright);
        for (i = 0; i < v.file.info.nfaces; i++) fprintf(f, "face%d=%s\n", i, v.file.info.faces[i].full);
    } else fprintf(f, "family=%ls\n", v.family_only);
    fprintf(f, "gdi_face=%ls\nscope=%d\nstatus=%ls\n", v.gdi_face, v.scope, v.status);
    button_centre(f, "print", v.print);
    button_centre(f, "install", v.install);
    button_centre(f, "install_all", v.install_all);
    button_centre(f, "face_combo", v.face_combo);
    ClientToScreen(v.pane, &o);
    GetClientRect(v.pane, &r);
    fprintf(f, "pane=%ld,%ld,%ld,%ld\nscroll=%d\ncontent_h=%d\n", o.x, o.y, o.x + r.right, o.y + r.bottom, v.scroll, v.content_h);
    fprintf(f, "alpha_y=%d\n", (int)(o.y + v.alpha_y - v.scroll));
    for (i = 0; i < (int)ARRAYSIZE(SIZES); i++) fprintf(f, "size%d=%d\n", SIZES[i], (int)(o.y + v.size_y[i] - v.scroll));
    fclose(f);
}

/* ---- the window --------------------------------------------------------------------- */
static void load_face(void)
{
    WCHAR title[512], name[256], reg[512];
    char reg8[1024];
    const struct fi_face *f = cur_face();
    if (f) {
        fi_registry_name(&v.file.info, reg8, sizeof(reg8));
        utf8_to_w(reg8, reg, ARRAYSIZE(reg));
        utf8_to_w(f->full, name, ARRAYSIZE(name));
        _snwprintf(title, ARRAYSIZE(title), L"%ls (%ls)", name,
                   f->flags & FI_BITMAP ? L"Raster" : (f->flags & FI_PS_OUTLINES) && !(f->flags & FI_TT_OUTLINES) ? L"OpenType" : L"TrueType");
        v.scope = font_installed_scope(&v.file);
    } else {
        lstrcpynW(title, v.family_only, ARRAYSIZE(title));
        v.scope = 0;
    }
    SetWindowTextW(v.hwnd, title);
    v.scroll = 0;
    if (v.install) {
        EnableWindow(v.install, !(v.scope & SCOPE_USER));
        SetWindowTextW(v.install, v.scope & SCOPE_USER ? L"Installed" : L"Install");
        EnableWindow(v.install_all, !(v.scope & SCOPE_MACHINE));
    }
    InvalidateRect(v.pane, NULL, TRUE);
}

static void layout(void)
{
    RECT r;
    int x = S(12), bh = S(28), top = S(10);
    GetClientRect(v.hwnd, &r);
    MoveWindow(v.print, x, top, S(80), bh, TRUE); x += S(88);
    if (v.install) {
        MoveWindow(v.install, x, top, S(96), bh, TRUE); x += S(104);
        MoveWindow(v.install_all, x, top, S(170), bh, TRUE); x += S(178);
    }
    if (v.face_combo) MoveWindow(v.face_combo, x, top + S(2), r.right - x - S(12) > S(160) ? r.right - x - S(12) : S(160), S(300), TRUE);
    MoveWindow(v.pane, 0, S(48), r.right, r.bottom - S(48), TRUE);
}

static void set_scroll(void)
{
    RECT r;
    SCROLLINFO si = { sizeof(si), SIF_ALL };
    GetClientRect(v.pane, &r);
    if (v.scroll > v.content_h - r.bottom) v.scroll = v.content_h - r.bottom;
    if (v.scroll < 0) v.scroll = 0;
    si.nMin = 0;
    si.nMax = v.content_h;
    si.nPage = r.bottom + 1;
    si.nPos = v.scroll;
    SetScrollInfo(v.pane, SB_VERT, &si, TRUE);
}

static void do_install(BOOL all)
{
    WCHAR msg[512], args[MAX_PATH + 64];
    DWORD err;
    if (!all) {
        err = font_install(v.file.path, SCOPE_USER, msg, ARRAYSIZE(msg));
    } else if (font_is_elevated()) {
        err = font_install(v.file.path, SCOPE_MACHINE, msg, ARRAYSIZE(msg));
    } else {
        /* the elevated copy does it; its exit code is the answer */
        _snwprintf(args, ARRAYSIZE(args), L"/install /allusers \"%ls\"", v.file.path);
        if (!run_self(v.hwnd, args, TRUE, &err)) err = ERROR_ACCESS_DENIED;
        if (err == 0) lstrcpyW(msg, L"The font was installed for all users.");
        else lstrcpyW(msg, L"You need to be an administrator to install fonts for all users.");
    }
    lstrcpynW(v.status, msg, ARRAYSIZE(v.status));
    load_face();
    viewer_dump();
    if (err) message_box(v.hwnd, msg, TRUE);
}

static LRESULT CALLBACK pane_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        RECT r;
        HDC dc = BeginPaint(hwnd, &ps), mem;
        HBITMAP bmp, oldb;
        int h;
        GetClientRect(hwnd, &r);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, r.right, r.bottom);
        oldb = SelectObject(mem, bmp);
        FillRect(mem, &r, GetStockObject(WHITE_BRUSH));
        h = paint_content(mem, g_dpi, S(24), S(16) - v.scroll, r.right - S(48), TRUE);
        /* recorded positions are in content coordinates */
        {
            int i;
            v.alpha_y += v.scroll;
            for (i = 0; i < (int)ARRAYSIZE(SIZES); i++) v.size_y[i] += v.scroll;
        }
        BitBlt(dc, 0, 0, r.right, r.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldb);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        if (h + S(16) != v.content_h) { v.content_h = h + S(16); set_scroll(); }
        viewer_dump();
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        set_scroll();
        return 0;
    case WM_MOUSEWHEEL:
        v.scroll -= GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * S(48);
        set_scroll();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_VSCROLL: {
        SCROLLINFO si = { sizeof(si), SIF_ALL };
        RECT r;
        GetScrollInfo(hwnd, SB_VERT, &si);
        GetClientRect(hwnd, &r);
        switch (LOWORD(wp)) {
        case SB_LINEUP: v.scroll -= S(24); break;
        case SB_LINEDOWN: v.scroll += S(24); break;
        case SB_PAGEUP: v.scroll -= r.bottom; break;
        case SB_PAGEDOWN: v.scroll += r.bottom; break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: v.scroll = si.nTrackPos; break;
        case SB_TOP: v.scroll = 0; break;
        case SB_BOTTOM: v.scroll = v.content_h; break;
        }
        set_scroll();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;
    case WM_KEYDOWN:
        switch (wp) {
        case VK_DOWN: SendMessageW(hwnd, WM_VSCROLL, SB_LINEDOWN, 0); return 0;
        case VK_UP: SendMessageW(hwnd, WM_VSCROLL, SB_LINEUP, 0); return 0;
        case VK_NEXT: SendMessageW(hwnd, WM_VSCROLL, SB_PAGEDOWN, 0); return 0;
        case VK_PRIOR: SendMessageW(hwnd, WM_VSCROLL, SB_PAGEUP, 0); return 0;
        case VK_HOME: SendMessageW(hwnd, WM_VSCROLL, SB_TOP, 0); return 0;
        case VK_END: SendMessageW(hwnd, WM_VSCROLL, SB_BOTTOM, 0); return 0;
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK viewer_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        layout();
        return 0;
    case WM_ERASEBKGND: {
        RECT r;
        HBRUSH b = CreateSolidBrush(COL_BAR);
        GetClientRect(hwnd, &r);
        FillRect((HDC)wp, &r, b);
        DeleteObject(b);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, COL_BAR);
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_PRINT:
            viewer_print(v.have_file ? v.file.path : NULL, hwnd, TRUE);
            return 0;
        case ID_INSTALL:
            do_install(FALSE);
            return 0;
        case ID_INSTALL_ALL:
            do_install(TRUE);
            return 0;
        case ID_FACE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int i = (int)SendMessageW(v.face_combo, CB_GETCURSEL, 0, 0);
                if (i >= 0 && i < v.file.info.nfaces) { v.face = i; load_face(); }
            }
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        return SendMessageW(v.pane, msg, wp, lp);
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hwnd); return 0; }
        if (wp == 'P' && GetKeyState(VK_CONTROL) < 0) { viewer_print(v.have_file ? v.file.path : NULL, hwnd, TRUE); return 0; }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND make_button(const WCHAR *text, int id, BOOL shield)
{
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10,
                             v.hwnd, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(b, WM_SETFONT, (WPARAM)v.ui, TRUE);
    if (shield) SendMessageW(b, 0x160C /* BCM_SETSHIELD */, 0, TRUE);
    return b;
}

int viewer_main(const WCHAR *path, const WCHAR *family, int show)
{
    WNDCLASSW wc = { 0 };
    MSG msg;
    int i;

    if (path) {
        if (!font_read(path, &v.file)) {
            WCHAR m[MAX_PATH + 64];
            _snwprintf(m, ARRAYSIZE(m), L"The requested file %ls is not a valid font file.", PathFindFileNameW(path));
            message_box(NULL, m, TRUE);
            return ERROR_BAD_FORMAT;
        }
        v.have_file = TRUE;
#ifndef SG_MUTANT_NOPRIVATE
        v.added = AddFontResourceExW(v.file.path, FR_PRIVATE, 0);
#endif
    } else lstrcpynW(v.family_only, family, LF_FACESIZE);

    wc.lpfnWndProc = viewer_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"SgFontView";
    RegisterClassW(&wc);
    wc.lpfnWndProc = pane_proc;
    wc.hIcon = NULL;
    wc.lpszClassName = L"SgFontViewPane";
    wc.hbrBackground = NULL;
    RegisterClassW(&wc);

    v.ui = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    v.hwnd = CreateWindowExW(0, L"SgFontView", L"", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                             S(760), S(680), NULL, NULL, g_inst, NULL);
    v.print = make_button(L"Print", ID_PRINT, FALSE);
    if (v.have_file) {
        v.install = make_button(L"Install", ID_INSTALL, FALSE);
        v.install_all = make_button(L"Install for all users", ID_INSTALL_ALL, TRUE);
        if (v.file.info.nfaces > 1) {
            v.face_combo = CreateWindowExW(0, L"COMBOBOX", NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                           0, 0, 10, 10, v.hwnd, (HMENU)ID_FACE, g_inst, NULL);
            SendMessageW(v.face_combo, WM_SETFONT, (WPARAM)v.ui, TRUE);
            for (i = 0; i < v.file.info.nfaces; i++) {
                WCHAR n[256];
                utf8_to_w(v.file.info.faces[i].full, n, ARRAYSIZE(n));
                SendMessageW(v.face_combo, CB_ADDSTRING, 0, (LPARAM)n);
            }
            SendMessageW(v.face_combo, CB_SETCURSEL, 0, 0);
        }
    }
    v.pane = CreateWindowExW(0, L"SgFontViewPane", NULL, WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP, 0, 0, 10, 10,
                             v.hwnd, (HMENU)ID_PANE, g_inst, NULL);
    load_face();
    layout();
    ShowWindow(v.hwnd, show);
    UpdateWindow(v.hwnd);
    SetFocus(v.pane);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(v.hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (v.added) RemoveFontResourceExW(v.file.path, FR_PRIVATE, 0);
    return 0;
}

/* ---- printing -------------------------------------------------------------------------- */
BOOL viewer_print(const WCHAR *path, HWND owner, BOOL ask)
{
    HDC dc = NULL;
    DOCINFOW di = { sizeof(di) };
    int dpi, w, margin;
    BOOL ok = FALSE;
    if (path && !v.have_file) {
        if (!font_read(path, &v.file)) return FALSE;
        v.have_file = TRUE;
        AddFontResourceExW(v.file.path, FR_PRIVATE, 0);
    }
    if (ask) {
        PRINTDLGW pd = { sizeof(pd) };
        pd.hwndOwner = owner;
        pd.Flags = PD_RETURNDC | PD_NOSELECTION | PD_NOPAGENUMS;
        if (!PrintDlgW(&pd)) return FALSE;
        dc = pd.hDC;
        if (pd.hDevMode) GlobalFree(pd.hDevMode);
        if (pd.hDevNames) GlobalFree(pd.hDevNames);
    } else {
        WCHAR name[256];
        DWORD n = ARRAYSIZE(name);
        if (GetDefaultPrinterW(name, &n)) dc = CreateDCW(L"WINSPOOL", name, NULL, NULL);
    }
    if (!dc) {
        if (!ask) message_box(owner, L"No printer is installed.", TRUE);
        return FALSE;
    }
    dpi = GetDeviceCaps(dc, LOGPIXELSY);
    margin = dpi / 2;
    w = GetDeviceCaps(dc, HORZRES) - 2 * margin;
    di.lpszDocName = v.have_file ? PathFindFileNameW(v.file.path) : v.family_only;
    if (StartDocW(dc, &di) > 0 && StartPage(dc) > 0) {
        paint_content(dc, dpi, margin, margin, w, FALSE);
        ok = EndPage(dc) > 0 && EndDoc(dc) > 0;
    }
    DeleteDC(dc);
    return ok;
}

/* ---- the command line ---------------------------------------------------------------- */
static int list_families(const WCHAR *out)
{
    struct family *f;
    int n = families_load(&f), i;
    FILE *fp = _wfopen(out, L"w");
    if (!fp) return 1;
    for (i = 0; i < n; i++) {
        char u[LF_FACESIZE * 3];
        WideCharToMultiByte(CP_UTF8, 0, f[i].name, -1, u, sizeof(u), NULL, NULL);
        fprintf(fp, "%s\n", u);
    }
    fclose(fp);
    families_free(f, n);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    int argc, i, scope = SCOPE_USER;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    BOOL quiet = FALSE, install = FALSE, uninstall = FALSE, print = FALSE, folder = FALSE;
    const WCHAR *target = NULL, *family = NULL;
    HDC dc;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES };
    (void)prev; (void)cmd;

    g_inst = inst;
    InitCommonControlsEx(&icc);
    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    if (!GetEnvironmentVariableW(L"SG_FONTVIEW_DUMP", g_dump_path, MAX_PATH)) g_dump_path[0] = 0;

    for (i = 1; i < argc; i++) {
        const WCHAR *a = argv[i];
        if (!lstrcmpiW(a, L"/install") || !lstrcmpiW(a, L"-install")) install = TRUE;
        else if (!lstrcmpiW(a, L"/uninstall") || !lstrcmpiW(a, L"-uninstall")) uninstall = TRUE;
        else if (!lstrcmpiW(a, L"/allusers")) scope = SCOPE_MACHINE;
        else if (!lstrcmpiW(a, L"/quiet") || !lstrcmpiW(a, L"/q")) quiet = TRUE;
        else if (!lstrcmpiW(a, L"/p") || !lstrcmpiW(a, L"/print")) print = TRUE;
        else if (!lstrcmpiW(a, L"/folder")) folder = TRUE;
        else if (!lstrcmpiW(a, L"/family") && i + 1 < argc) family = argv[++i];
        else if (!lstrcmpiW(a, L"--families") && i + 1 < argc) return list_families(argv[++i]);
        else if (!target) target = a;
    }

    if (install || uninstall) {
        WCHAR msg[512], args[MAX_PATH + 64];
        DWORD err;
        if (!target) return ERROR_INVALID_PARAMETER;
        if (scope == SCOPE_MACHINE && !font_is_elevated() && !quiet) {
            /* Windows' "Install for all users": the elevated copy does it */
            _snwprintf(args, ARRAYSIZE(args), L"%ls /allusers \"%ls\"", install ? L"/install" : L"/uninstall", target);
            if (!run_self(NULL, args, TRUE, &err)) err = ERROR_ACCESS_DENIED;
            return (int)err;
        }
        err = install ? font_install(target, scope, msg, ARRAYSIZE(msg)) : font_uninstall(target, scope, msg, ARRAYSIZE(msg));
        if (g_dump_path[0]) {
            FILE *f = _wfopen(g_dump_path, L"w");
            if (f) { fprintf(f, "result=%lu\nmessage=%ls\n", err, msg); fclose(f); }
        }
        if (!quiet) MessageBoxW(NULL, msg, L"Fonts", MB_OK | (err ? MB_ICONERROR : MB_ICONINFORMATION));
        return (int)err;
    }
    if (folder) return folder_main(show);
    if (print) return viewer_print(target, NULL, FALSE) ? 0 : 1;
    if (family) return viewer_main(NULL, family, show);
    if (!target) return folder_main(show);
    return viewer_main(target, NULL, show);
}
