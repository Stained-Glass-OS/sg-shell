/* sg-zip -- Compressed (zipped) Folders for Stained Glass OS.
 *
 * Wine has no zipfldr.dll, so a .zip was just a file. This program is what
 * Windows' zipped-folder support does for a person:
 *
 *   sg-zip64.exe <zip>                      browse it, read-only, like a folder
 *   sg-zip64.exe /extractall <zip>          the "Extract Compressed (Zipped)
 *                                           Folders" wizard
 *   sg-zip64.exe /create <file|folder>...   "Send to > Compressed (zipped)
 *                                           folder": <first name>.zip beside them
 *
 * and, headless, for scripts and the gate:
 *
 *   /extract <zip> <folder> [/quiet] [/overwrite|/skip] [/log <file>]
 *   /create ... [/out <zip>] [/quiet] [/log <file>]
 *   /list <zip> /log <file>
 *
 * Exit code 0 when everything went, 1 when any entry failed or was refused.
 * SG_ZIP_DUMP=<file> writes what the windows show (UTF-8), for the gate.
 *
 * Extraction treats the archive as hostile: a name that is absolute, carries
 * a drive or stream (":"), or climbs out with ".." is refused, never written.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "zipcore.h"
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

#define COL_BG (sgm_dark ? RGB(0x20,0x20,0x20) : RGB(0xFF, 0xFF, 0xFF))
#define COL_BAR (sgm_dark ? RGB(0x2B,0x2B,0x2B) : RGB(0xF3, 0xF3, 0xF3))
#define COL_LINE (sgm_dark ? RGB(0x3A,0x3A,0x3A) : RGB(0xE5, 0xE5, 0xE5))
#define COL_TEXT (sgm_dark ? RGB(0xFF,0xFF,0xFF) : RGB(0x1A, 0x1A, 0x1A))
#define COL_SUBTLE (sgm_dark ? RGB(0xA8,0xA8,0xA8) : RGB(0x6D, 0x6D, 0x6D))
#define COL_ACCENT   RGB(112, 48, 192)
#define COL_HOT (sgm_dark ? RGB(0x3B,0x2E,0x4F) : RGB(0xEC, 0xE4, 0xF7))
/* the window's background in the app mode's colour: the DC's own brush */
static HBRUSH bg_brush(HDC dc) { SetDCBrushColor(dc, COL_BG); return GetStockObject(DC_BRUSH); }
/* a list view keeps the colours it was made with: give it the mode's */
static BOOL CALLBACK list_colours(HWND hwnd, LPARAM lp)
{
    WCHAR cls[32];
    (void)lp;
    if (GetClassNameW(hwnd, cls, 32) && !lstrcmpiW(cls, WC_LISTVIEWW))
    {
        SendMessageW(hwnd, LVM_SETBKCOLOR, 0, COL_BG);
        SendMessageW(hwnd, LVM_SETTEXTBKCOLOR, 0, COL_BG);
        SendMessageW(hwnd, LVM_SETTEXTCOLOR, 0, COL_TEXT);
    }
    return TRUE;
}
#define COL_PRESSED (sgm_dark ? RGB(0x4A,0x38,0x66) : RGB(0xDD, 0xCE, 0xF1))

#define WM_APP_PROGRESS (WM_APP + 1)
#define WM_APP_DONE     (WM_APP + 2)
#define WM_APP_ASK      (WM_APP + 3)

enum { ID_EXTRACT = 100, ID_BACK, ID_FWD, ID_UP, ID_LIST, ID_ADDR,
       ID_DEST = 200, ID_BROWSE, ID_SHOW, ID_GO, ID_CANCEL, ID_PROGRESS, ID_STATUS };

static HINSTANCE g_inst;
static int g_dpi = 96;
static HFONT g_font, g_font_title, g_font_glyph;
static WCHAR g_dump[MAX_PATH];

static int S(int v) { return MulDiv(v, g_dpi, 96); }

static HFONT make_font(int pt10, int weight, const WCHAR *face)
{
    return CreateFontW(-MulDiv(pt10, g_dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, face);
}

/* ---- the dump (UTF-8, no BOM) --------------------------------------------------------------- */
static void dumpf(BOOL truncate, const WCHAR *fmt, ...)
{
    WCHAR line[2048];
    char u8[6144];
    va_list ap;
    HANDLE f;
    DWORD n;
    int len;
    if (!g_dump[0]) return;
    va_start(ap, fmt);
    _vsnwprintf(line, 2047, fmt, ap);
    line[2047] = 0;
    va_end(ap);
    len = WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, sizeof(u8), NULL, NULL) - 1;
    if (len < 0) return;
    f = CreateFileW(g_dump, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    truncate ? CREATE_ALWAYS : OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, NULL, FILE_END);
    WriteFile(f, u8, len, &n, NULL);
    WriteFile(f, "\n", 1, &n, NULL);
    CloseHandle(f);
}

/* ---- a log for the headless modes --------------------------------------------------------- */
static HANDLE g_log = INVALID_HANDLE_VALUE;
static void zlog(const WCHAR *fmt, ...)
{
    WCHAR line[2048];
    char u8[6144];
    va_list ap;
    DWORD n;
    int len;
    if (g_log == INVALID_HANDLE_VALUE) return;
    va_start(ap, fmt);
    _vsnwprintf(line, 2047, fmt, ap);
    line[2047] = 0;
    va_end(ap);
    len = WideCharToMultiByte(CP_UTF8, 0, line, -1, u8, sizeof(u8), NULL, NULL) - 1;
    if (len < 0) return;
    WriteFile(g_log, u8, len, &n, NULL);
    WriteFile(g_log, "\n", 1, &n, NULL);
}

static void error_box(HWND owner, const WCHAR *what, const WCHAR *detail)
{
    WCHAR msg[1024];
    _snwprintf(msg, 1023, L"%ls\n\n%ls", what, detail);
    msg[1023] = 0;
    MessageBoxW(owner, msg, L"Compressed (zipped) Folders", MB_OK | MB_ICONERROR);
}

static const WCHAR *base_name(const WCHAR *p)
{
    const WCHAR *s = wcsrchr(p, '\\'), *t = wcsrchr(p, '/');
    if (t > s) s = t;
    return s ? s + 1 : p;
}

/* ---- extraction (the wizard's worker and the headless mode) -------------------------------- */
enum { POLICY_ASK, POLICY_OVERWRITE, POLICY_SKIP };
enum { ASK_REPLACE = 1, ASK_SKIP = 2, ASK_CANCEL = 0 };

typedef struct {
    zarchive *z;
    WCHAR dest[MAX_PATH];
    int policy;
    HWND notify;
    volatile LONG cancel;
    int done, total, written, errors, refused, skipped;
    WCHAR first_error[512];
} xjob;

static void note_error(xjob *j, const WCHAR *name, int e)
{
    j->errors++;
    if (!j->first_error[0]) _snwprintf(j->first_error, 511, L"%ls: %ls", name, zip_strerror(e));
    zlog(L"ERROR\t%ls\t%ls", name, zip_strerror(e));
}

static BOOL make_dirs(const WCHAR *path)
{
    int r = SHCreateDirectoryExW(NULL, path, NULL);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

static int extract_run(xjob *j)
{
    int i;
    j->total = j->z->n;
    if (!make_dirs(j->dest)) { note_error(j, j->dest, ZE_WRITE); return ZE_WRITE; }
    for (i = 0; i < j->z->n; i++)
    {
        zentry *e = &j->z->e[i];
        WCHAR rel[MAX_PATH], full[MAX_PATH], parent[MAX_PATH], *slash;
        BYTE *data;
        size_t len;
        int r;

        if (j->cancel) return ZE_CANCELLED;
        j->done = i;
        if (j->notify) PostMessageW(j->notify, WM_APP_PROGRESS, i, (LPARAM)j->z->n);
        if (!zip_safe_path(e->name, rel, MAX_PATH))
        {
            if (e->name[0] && lstrcmpW(e->name, L"./") && lstrcmpW(e->name, L"/")) { j->refused++; zlog(L"REFUSED\t%ls", e->name); }
            else if (!e->dir) { j->refused++; zlog(L"REFUSED\t%ls", e->name); }
            if (!j->first_error[0] && j->refused)
                _snwprintf(j->first_error, 511, L"%ls: %ls", e->name, zip_strerror(ZE_UNSAFE));
            continue;
        }
        if (_snwprintf(full, MAX_PATH, L"%ls\\%ls", j->dest, rel) < 0) { note_error(j, e->name, ZE_TOOBIG); continue; }
        full[MAX_PATH - 1] = 0;
        if (e->dir) { if (!make_dirs(full)) note_error(j, e->name, ZE_WRITE); continue; }
        lstrcpyW(parent, full);
        if ((slash = wcsrchr(parent, '\\'))) { *slash = 0; if (!make_dirs(parent)) { note_error(j, e->name, ZE_WRITE); continue; } }

        if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES)
        {
            int answer = j->policy == POLICY_OVERWRITE ? ASK_REPLACE : j->policy == POLICY_SKIP ? ASK_SKIP
                       : j->notify ? (int)SendMessageW(j->notify, WM_APP_ASK, i, (LPARAM)full) : ASK_SKIP;
            if (answer == ASK_CANCEL) { j->cancel = 1; return ZE_CANCELLED; }
            if (answer == ASK_SKIP) { j->skipped++; zlog(L"SKIPPED\t%ls", e->name); continue; }
            SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
        }
        if ((r = zip_read(j->z, i, &data, &len))) { note_error(j, e->name, r); continue; }
        {
            HANDLE f = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
            DWORD n = 0;
            BOOL ok = f != INVALID_HANDLE_VALUE;
            size_t off = 0;
            while (ok && off < len)
            {
                DWORD chunk = len - off > 0x10000000 ? 0x10000000 : (DWORD)(len - off);
                ok = WriteFile(f, data + off, chunk, &n, NULL) && n == chunk;
                off += chunk;
            }
            if (ok)
            {
                FILETIME lft, ft;
                if (DosDateTimeToFileTime(e->dosdate, e->dostime, &lft) && LocalFileTimeToFileTime(&lft, &ft))
                    SetFileTime(f, NULL, NULL, &ft);
            }
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            free(data);
            if (!ok) { DeleteFileW(full); note_error(j, e->name, ZE_WRITE); continue; }
        }
        j->written++;
        zlog(L"OK\t%ls", e->name);
    }
    j->done = j->z->n;
    return ZE_OK;
}

/* ---- creating an archive ------------------------------------------------------------------- */
typedef struct { zwriter *w; const WCHAR *out; int files, errors; WCHAR first_error[512]; } cjob;

static void add_tree(cjob *c, const WCHAR *path, const WCHAR *arc)
{
    DWORD attr = GetFileAttributesW(path);
    int r;
    if (attr == INVALID_FILE_ATTRIBUTES) { c->errors++; zlog(L"ERROR\t%ls\tnot found", path); return; }
    if (attr & FILE_ATTRIBUTE_DIRECTORY)
    {
        WCHAR pat[MAX_PATH], sub[MAX_PATH], subarc[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        FILETIME ft = { 0 };
        HANDLE d = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (d != INVALID_HANDLE_VALUE) { GetFileTime(d, NULL, NULL, &ft); CloseHandle(d); }
        if ((r = zw_add_dir(c->w, arc, &ft))) { c->errors++; zlog(L"ERROR\t%ls\t%ls", arc, zip_strerror(r)); return; }
        _snwprintf(pat, MAX_PATH, L"%ls\\*", path);
        if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return;
        do
        {
            if (!lstrcmpW(fd.cFileName, L".") || !lstrcmpW(fd.cFileName, L"..")) continue;
            _snwprintf(sub, MAX_PATH, L"%ls\\%ls", path, fd.cFileName);
            _snwprintf(subarc, MAX_PATH, L"%ls/%ls", arc, fd.cFileName);
            sub[MAX_PATH - 1] = subarc[MAX_PATH - 1] = 0;
            if (!lstrcmpiW(sub, c->out)) continue;          /* never the archive being made */
            add_tree(c, sub, subarc);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return;
    }
    if ((r = zw_add_file(c->w, arc, path)))
    {
        c->errors++;
        if (!c->first_error[0]) _snwprintf(c->first_error, 511, L"%ls: %ls", path, zip_strerror(r));
        zlog(L"ERROR\t%ls\t%ls", arc, zip_strerror(r));
        return;
    }
    c->files++;
    zlog(L"ADDED\t%ls", arc);
}

/* "<first name>.zip" beside the first item, " (2)" and on when taken, as Windows names it */
static void default_zip_name(const WCHAR *first, WCHAR *out)
{
    WCHAR dir[MAX_PATH], stem[MAX_PATH], *slash, *dot;
    DWORD attr = GetFileAttributesW(first);
    int n = 2;
    GetFullPathNameW(first, MAX_PATH, dir, NULL);
    { int l = lstrlenW(dir); while (l > 3 && (dir[l - 1] == '\\' || dir[l - 1] == '/')) dir[--l] = 0; }
    lstrcpyW(stem, base_name(dir));
    if ((slash = wcsrchr(dir, '\\'))) slash[0] = 0;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY) && (dot = wcsrchr(stem, '.')) && dot != stem) *dot = 0;
    _snwprintf(out, MAX_PATH, L"%ls\\%ls.zip", dir, stem);
    while (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES && n < 1000)
        _snwprintf(out, MAX_PATH, L"%ls\\%ls (%d).zip", dir, stem, n++);
}

static int do_create(WCHAR **items, int nitems, const WCHAR *out_opt, BOOL quiet)
{
    WCHAR out[MAX_PATH];
    cjob c = { 0 };
    int i, r;
    if (!nitems) return 1;
    if (out_opt) GetFullPathNameW(out_opt, MAX_PATH, out, NULL);
    else default_zip_name(items[0], out);
    c.out = out;
    if (!(c.w = zw_open(out)))
    {
        if (!quiet) error_box(NULL, L"The Compressed (zipped) Folder could not be created.", out);
        zlog(L"ERROR\t%ls\tcannot create", out);
        return 1;
    }
    for (i = 0; i < nitems; i++)
    {
        WCHAR full[MAX_PATH];
        GetFullPathNameW(items[i], MAX_PATH, full, NULL);
        { int l = lstrlenW(full); while (l > 3 && full[l - 1] == '\\') full[--l] = 0; }
        add_tree(&c, full, base_name(full));
    }
    r = zw_close(c.w, TRUE);
    if (r) { c.errors++; zlog(L"ERROR\t%ls\t%ls", out, zip_strerror(r)); }
    zlog(L"CREATED\t%ls\t%d", out, c.files);
    if (!quiet && (r || c.errors))
        error_box(NULL, L"Some files could not be added to the Compressed (zipped) Folder.",
                  c.first_error[0] ? c.first_error : zip_strerror(r));
    if (!quiet && !r)
    {
        /* show it, selected, as Windows leaves it for a rename */
        WCHAR param[MAX_PATH + 16];
        _snwprintf(param, MAX_PATH + 15, L"/select,\"%ls\"", out);
        param[MAX_PATH + 15] = 0;
        if (GetEnvironmentVariableW(L"SG_ZIP_NO_SHOW", NULL, 0) == 0)
            ShellExecuteW(NULL, NULL, L"explorer.exe", param, NULL, SW_SHOWNORMAL);
    }
    return r || c.errors ? 1 : 0;
}

/* ---- drawn glyphs --------------------------------------------------------------------------- */
static void draw_zip_folder(HDC dc, int x, int y, int size)
{
    /* a folder in amber with a zipper down its middle */
    HBRUSH back = CreateSolidBrush(RGB(0xE8, 0xB0, 0x3C)), front = CreateSolidBrush(RGB(0xF8, 0xCE, 0x62));
    HBRUSH zip = CreateSolidBrush(RGB(0x5A, 0x3A, 0x10));
    RECT r;
    int u = size / 16 ? size / 16 : 1, k;
    SetRect(&r, x + u, y + 2 * u, x + 7 * u, y + 5 * u); FillRect(dc, &r, back);
    SetRect(&r, x + u, y + 4 * u, x + 15 * u, y + 14 * u); FillRect(dc, &r, back);
    SetRect(&r, x + u, y + 5 * u, x + 15 * u, y + 14 * u); FillRect(dc, &r, front);
    for (k = 0; k < 4; k++)
    {
        SetRect(&r, x + 7 * u + (k & 1) * u, y + (5 + 2 * k) * u, x + 8 * u + (k & 1) * u, y + (6 + 2 * k) * u);
        FillRect(dc, &r, zip);
    }
    SetRect(&r, x + 7 * u, y + 12 * u, x + 9 * u, y + 14 * u); FillRect(dc, &r, zip);
    DeleteObject(back); DeleteObject(front); DeleteObject(zip);
}

static void draw_arrow(HDC dc, RECT *r, int dir, COLORREF col)   /* 0 left, 1 right, 2 up */
{
    HPEN pen = CreatePen(PS_SOLID, S(1) + 1, col), old = SelectObject(dc, pen);
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2, a = S(6), b = S(4);
    if (dir == 2)
    {
        MoveToEx(dc, cx, cy + a, NULL); LineTo(dc, cx, cy - a);
        MoveToEx(dc, cx - b, cy - a + b, NULL); LineTo(dc, cx, cy - a); LineTo(dc, cx + b + 1, cy - a + b + 1);
    }
    else
    {
        int s = dir ? 1 : -1;
        MoveToEx(dc, cx - s * a, cy, NULL); LineTo(dc, cx + s * a, cy);
        MoveToEx(dc, cx + s * (a - b), cy - b, NULL); LineTo(dc, cx + s * a, cy); LineTo(dc, cx + s * (a - b - 1), cy + b + 1);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

/* flat owner-drawn buttons: hover and pressed in the accent's tint */
static void draw_flat_button(DRAWITEMSTRUCT *di, BOOL hot)
{
    WCHAR text[128];
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    BOOL pressed = di->itemState & ODS_SELECTED, disabled = di->itemState & ODS_DISABLED;
    HBRUSH b = CreateSolidBrush(pressed ? COL_PRESSED : hot ? COL_HOT : COL_BG);
    FillRect(dc, &r, b);
    DeleteObject(b);
    GetWindowTextW(di->hwndItem, text, 128);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? (sgm_dark ? RGB(0x6E, 0x6E, 0x6E) : RGB(0xA0, 0xA0, 0xA0)) : COL_TEXT);
    SelectObject(dc, g_font);
    switch (di->CtlID)
    {
    case ID_EXTRACT:
        draw_zip_folder(dc, r.left + S(8), (r.top + r.bottom) / 2 - S(10), S(20));
        r.left += S(34);
        DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
        break;
    case ID_BACK: case ID_FWD: case ID_UP:
        draw_arrow(dc, &r, di->CtlID == ID_BACK ? 0 : di->CtlID == ID_FWD ? 1 : 2,
                   disabled ? (sgm_dark ? RGB(0x60, 0x60, 0x60) : RGB(0xC0, 0xC0, 0xC0)) : COL_TEXT);
        break;
    default:
        DrawTextW(dc, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
    }
    if (di->itemState & ODS_FOCUS)
    {
        RECT f = di->rcItem;
        InflateRect(&f, -2, -2);
        DrawFocusRect(dc, &f);
    }
}

static HWND g_hot_button;
static WNDPROC g_button_proc;
static LRESULT CALLBACK hover_button_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEMOVE && g_hot_button != hwnd)
    {
        TRACKMOUSEEVENT t = { sizeof(t), TME_LEAVE, hwnd, 0 };
        HWND old = g_hot_button;
        g_hot_button = hwnd;
        TrackMouseEvent(&t);
        if (old) InvalidateRect(old, NULL, FALSE);
        InvalidateRect(hwnd, NULL, FALSE);
    }
    else if (msg == WM_MOUSELEAVE && g_hot_button == hwnd)
    {
        g_hot_button = NULL;
        InvalidateRect(hwnd, NULL, FALSE);
    }
    return CallWindowProcW(g_button_proc, hwnd, msg, wp, lp);
}

static HWND flat_button(HWND parent, const WCHAR *text, int id)
{
    HWND b = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_inst, NULL);
    g_button_proc = (WNDPROC)SetWindowLongPtrW(b, GWLP_WNDPROC, (LONG_PTR)hover_button_proc);
    return b;
}

/* ---- the wizard: Extract Compressed (Zipped) Folders ---------------------------------------- */
typedef struct {
    HWND hwnd, dest, browse, show, go, cancel, progress, status;
    WCHAR zip[MAX_PATH];
    zarchive z;
    xjob job;
    HANDLE thread;
    BOOL running, finished;
    int ask_all;             /* 0 ask; else the remembered answer */
} wizard;

static void wizard_dump(wizard *w, const WCHAR *state)
{
    WCHAR dest[MAX_PATH];
    GetWindowTextW(w->dest, dest, MAX_PATH);
    dumpf(TRUE, L"WINDOW wizard");
    dumpf(FALSE, L"STATE %ls", state);
    dumpf(FALSE, L"ZIP %ls", w->zip);
    dumpf(FALSE, L"DEST %ls", dest);
    dumpf(FALSE, L"SHOW %d", Button_GetCheck(w->show) == BST_CHECKED);
    dumpf(FALSE, L"DONE %d/%d written=%d errors=%d refused=%d skipped=%d", w->job.done, w->job.total,
          w->job.written, w->job.errors, w->job.refused, w->job.skipped);
}

static void wizard_layout(wizard *w)
{
    RECT rc;
    int W, H, m = S(34), bw = S(88), bh = S(26);
    GetClientRect(w->hwnd, &rc);
    W = rc.right; H = rc.bottom;
    MoveWindow(w->dest, m, S(120), W - 2 * m - bw - S(10), S(24), TRUE);
    MoveWindow(w->browse, W - m - bw, S(119), bw, bh, TRUE);
    MoveWindow(w->show, m, S(156), W - 2 * m, S(22), TRUE);
    MoveWindow(w->progress, m, S(200), W - 2 * m, S(16), TRUE);
    MoveWindow(w->status, m, S(222), W - 2 * m, S(20), TRUE);
    MoveWindow(w->go, W - S(20) - 2 * bw - S(10), H - S(44) + (S(44) - bh) / 2, bw, bh, TRUE);
    MoveWindow(w->cancel, W - S(20) - bw, H - S(44) + (S(44) - bh) / 2, bw, bh, TRUE);
}

static DWORD WINAPI wizard_thread(void *arg)
{
    wizard *w = arg;
    int r = extract_run(&w->job);
    PostMessageW(w->hwnd, WM_APP_DONE, r, 0);
    return 0;
}

static void wizard_start(wizard *w)
{
    WCHAR dest[MAX_PATH], full[MAX_PATH];
    DWORD attr;
    GetWindowTextW(w->dest, dest, MAX_PATH);
    if (!dest[0]) { error_box(w->hwnd, L"Type the name of a folder to extract the files to.", L""); return; }
    GetFullPathNameW(dest, MAX_PATH, full, NULL);
    attr = GetFileAttributesW(full);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY))
    { error_box(w->hwnd, L"The destination is a file, not a folder.", full); return; }
    memset(&w->job, 0, sizeof(w->job));
    w->job.z = &w->z;
    lstrcpyW(w->job.dest, full);
    w->job.policy = POLICY_ASK;
    w->job.notify = w->hwnd;
    w->ask_all = 0;
    w->running = TRUE;
    EnableWindow(w->dest, FALSE); EnableWindow(w->browse, FALSE);
    EnableWindow(w->show, FALSE); EnableWindow(w->go, FALSE);
    ShowWindow(w->progress, SW_SHOW); ShowWindow(w->status, SW_SHOW);
    SendMessageW(w->progress, PBM_SETRANGE32, 0, w->z.n ? w->z.n : 1);
    SetWindowTextW(w->status, L"Extracting...");
    wizard_dump(w, L"extracting");
    w->thread = CreateThread(NULL, 0, wizard_thread, w, 0, NULL);
}

static int wizard_ask(wizard *w, const WCHAR *full)
{
    TASKDIALOGCONFIG tc = { sizeof(tc) };
    TASKDIALOG_BUTTON buttons[] = {
        { ASK_REPLACE + 1000, L"Replace the file in the destination" },
        { ASK_SKIP + 1000,    L"Skip this file" },
    };
    WCHAR main_text[MAX_PATH + 64];
    int button = 0;
    BOOL all = FALSE;
    if (w->ask_all) return w->ask_all;
    _snwprintf(main_text, MAX_PATH + 63, L"The destination already has a file named \"%ls\"", base_name(full));
    main_text[MAX_PATH + 63] = 0;
    tc.hwndParent = w->hwnd;
    tc.hInstance = g_inst;
    tc.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    tc.pszWindowTitle = L"Replace or Skip Files";
    tc.pszMainInstruction = main_text;
    tc.pszContent = full;
    tc.cButtons = 2;
    tc.pButtons = buttons;
    tc.nDefaultButton = ASK_REPLACE + 1000;
    tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    tc.pszVerificationText = L"Do this for all conflicts";
    dumpf(TRUE, L"WINDOW ask");
    dumpf(FALSE, L"FILE %ls", full);
    if (FAILED(TaskDialogIndirect(&tc, &button, NULL, &all)))
    {
        int r = MessageBoxW(w->hwnd, main_text, L"Replace or Skip Files", MB_YESNOCANCEL | MB_ICONQUESTION);
        button = r == IDYES ? ASK_REPLACE + 1000 : r == IDNO ? ASK_SKIP + 1000 : IDCANCEL;
    }
    if (button == IDCANCEL) return ASK_CANCEL;
    if (all) w->ask_all = button - 1000;
    wizard_dump(w, L"extracting");
    return button - 1000;
}

static LRESULT CALLBACK wizard_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CREATE) sg_mode_title(hwnd, sgm_dark);
    if (sg_mode_changed(msg, lp)) sgm_follow(hwnd);
    wizard *w = (wizard *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_CREATE:
    {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        WCHAR dest[MAX_PATH], *dot;
        w = cs->lpCreateParams;
        w->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)w);
        /* default: a folder named like the zip, beside it */
        lstrcpyW(dest, w->zip);
        if ((dot = wcsrchr(dest, '.')) && dot > wcsrchr(dest, '\\')) *dot = 0;
        w->dest = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", dest, WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  0, 0, 10, 10, hwnd, (HMENU)ID_DEST, g_inst, NULL);
        w->browse = CreateWindowW(L"BUTTON", L"B&rowse...", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  0, 0, 10, 10, hwnd, (HMENU)ID_BROWSE, g_inst, NULL);
        w->show = CreateWindowW(L"BUTTON", L"Sh&ow extracted files when complete",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                0, 0, 10, 10, hwnd, (HMENU)ID_SHOW, g_inst, NULL);
        Button_SetCheck(w->show, BST_CHECKED);
        w->progress = CreateWindowW(PROGRESS_CLASSW, NULL, WS_CHILD, 0, 0, 10, 10, hwnd, (HMENU)ID_PROGRESS, g_inst, NULL);
        w->status = CreateWindowW(L"STATIC", L"", WS_CHILD | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 10, 10, hwnd, (HMENU)ID_STATUS, g_inst, NULL);
        w->go = CreateWindowW(L"BUTTON", L"&Extract", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              0, 0, 10, 10, hwnd, (HMENU)ID_GO, g_inst, NULL);
        w->cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  0, 0, 10, 10, hwnd, (HMENU)ID_CANCEL, g_inst, NULL);
        {
            HWND kids[] = { w->dest, w->browse, w->show, w->status, w->go, w->cancel };
            int k;
            for (k = 0; k < 6; k++) SendMessageW(kids[k], WM_SETFONT, (WPARAM)g_font, FALSE);
        }
        wizard_layout(w);
        SetFocus(w->dest);
        SendMessageW(w->dest, EM_SETSEL, 0, -1);
        wizard_dump(w, L"ready");
        return 0;
    }
    case WM_SIZE:
        if (w) wizard_layout(w);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, COL_BG);
        SetTextColor((HDC)wp, COL_TEXT);
        return (LRESULT)bg_brush((HDC)wp);
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, r;
        HBRUSH bar = CreateSolidBrush(COL_BAR), line = CreateSolidBrush(COL_LINE);
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, bg_brush(dc));
        SetRect(&r, 0, rc.bottom - S(44), rc.right, rc.bottom); FillRect(dc, &r, bar);
        SetRect(&r, 0, rc.bottom - S(45), rc.right, rc.bottom - S(44)); FillRect(dc, &r, line);
        SetBkMode(dc, TRANSPARENT);
        draw_zip_folder(dc, S(34), S(22), S(32));
        SelectObject(dc, g_font_title);
        SetTextColor(dc, COL_ACCENT);
        SetRect(&r, S(76), S(22), rc.right - S(20), S(56));
        DrawTextW(dc, L"Select a Destination and Extract Files", -1, &r, DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, g_font);
        SetTextColor(dc, COL_TEXT);
        SetRect(&r, S(34), S(96), rc.right - S(34), S(116));
        DrawTextW(dc, L"Files will be extracted to this folder:", -1, &r, DT_SINGLELINE | DT_VCENTER);
        DeleteObject(bar); DeleteObject(line);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_BROWSE:
        {
            BROWSEINFOW bi = { 0 };
            WCHAR path[MAX_PATH];
            LPITEMIDLIST pidl;
            bi.hwndOwner = hwnd;
            bi.lpszTitle = L"Select a destination.";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            if ((pidl = SHBrowseForFolderW(&bi)))
            {
                if (SHGetPathFromIDListW(pidl, path)) SetWindowTextW(w->dest, path);
                CoTaskMemFree(pidl);
            }
            return 0;
        }
        case ID_GO:
        case IDOK:
            if (!w->running && !w->finished) wizard_start(w);
            return 0;
        case ID_CANCEL:
        case IDCANCEL:
            if (w->running) { w->job.cancel = 1; return 0; }
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_APP_PROGRESS:
    {
        WCHAR s[512];
        const WCHAR *name = (int)wp < w->z.n ? w->z.e[wp].name : L"";
        SendMessageW(w->progress, PBM_SETPOS, wp, 0);
        _snwprintf(s, 511, L"Extracting %d of %d: %ls", (int)wp + 1, (int)lp, name);
        s[511] = 0;
        SetWindowTextW(w->status, s);
        return 0;
    }
    case WM_APP_ASK:
        return wizard_ask(w, (const WCHAR *)lp);
    case WM_APP_DONE:
    {
        BOOL show = Button_GetCheck(w->show) == BST_CHECKED;
        WaitForSingleObject(w->thread, INFINITE);
        CloseHandle(w->thread);
        w->running = FALSE;
        w->finished = TRUE;
        SendMessageW(w->progress, PBM_SETPOS, w->z.n, 0);
        wizard_dump(w, wp == ZE_CANCELLED ? L"cancelled" : L"done");
        if (w->job.errors || w->job.refused)
        {
            WCHAR msg[768];
            _snwprintf(msg, 767, L"%d item(s) could not be extracted.", w->job.errors + w->job.refused);
            msg[767] = 0;
            error_box(hwnd, msg, w->job.first_error);
        }
        if (show && wp != ZE_CANCELLED && GetEnvironmentVariableW(L"SG_ZIP_NO_SHOW", NULL, 0) == 0)
            ShellExecuteW(NULL, NULL, L"explorer.exe", w->job.dest, NULL, SW_SHOWNORMAL);
        DestroyWindow(hwnd);
        return 0;
    }
    case WM_CLOSE:
        if (w->running) { w->job.cancel = 1; return 0; }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int run_wizard(const WCHAR *zip, HWND owner)
{
    static wizard w;
    WNDCLASSW wc = { 0 };
    MSG m;
    int r;
    RECT wr = { 0, 0, S(560), S(330) };
    memset(&w, 0, sizeof(w));
    GetFullPathNameW(zip, MAX_PATH, w.zip, NULL);
    if ((r = zip_open(&w.z, w.zip))) { error_box(owner, zip_strerror(r), w.zip); return 1; }
    wc.lpfnWndProc = wizard_proc;
    wc.hInstance = g_inst;
    wc.lpszClassName = L"SgZipWizard";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    AdjustWindowRectEx(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);
    CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Extract Compressed (Zipped) Folders",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                    CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, owner, NULL, g_inst, &w);
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        if (w.hwnd && IsWindow(w.hwnd) && IsDialogMessageW(w.hwnd, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    zip_close(&w.z);
    return w.job.errors || w.job.refused ? 1 : 0;
}

/* ---- the browse window --------------------------------------------------------------------- */
typedef struct {
    HWND hwnd, list, extract, back, fwd, up, status;
    WCHAR zip[MAX_PATH], folder[MAX_PATH];      /* folder: "" or "a/b/" */
    WCHAR history[32][MAX_PATH];
    int nhist, hpos;
    zarchive z;
    WCHAR folders[512][MAX_PATH / 2];
    int nrows, nfolders;
    HIMAGELIST images;
} browser;

static void fmt_size(ULONGLONG n, WCHAR *out, int cch)
{
    StrFormatKBSizeW((LONGLONG)n, out, cch);
}

static void fmt_date(const zentry *e, WCHAR *out, int cch)
{
    FILETIME ft;
    SYSTEMTIME st;
    WCHAR d[64], t[64];
    out[0] = 0;
    if (!DosDateTimeToFileTime(e->dosdate, e->dostime, &ft) || !FileTimeToSystemTime(&ft, &st)) return;
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, d, 64);
    GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &st, NULL, t, 64);
    _snwprintf(out, cch, L"%ls %ls", d, t);
    out[cch - 1] = 0;
}

static int icon_index(const WCHAR *name, BOOL dir, WCHAR *type, int cch)
{
    SHFILEINFOW sfi = { 0 };
    SHGetFileInfoW(name, dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                   SHGFI_USEFILEATTRIBUTES | SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_TYPENAME);
    if (type)
    {
        if (dir) lstrcpynW(type, L"File folder", cch);
        /* Wine's generic answer is ".ext file"; Windows says "EXT File" */
        else if (sfi.szTypeName[0] && sfi.szTypeName[0] != '.') lstrcpynW(type, sfi.szTypeName, cch);
        else
        {
            const WCHAR *dot = wcsrchr(name, '.');
            if (dot && dot[1])
            {
                int k;
                _snwprintf(type, cch, L"%ls File", dot + 1);
                type[cch - 1] = 0;
                for (k = 0; type[k] && type[k] != ' '; k++) type[k] = towupper(type[k]);
            }
            else lstrcpynW(type, L"File", cch);
        }
    }
    return sfi.iIcon;
}

static void browser_title(browser *b)
{
    WCHAR t[MAX_PATH * 2], addr[MAX_PATH * 2], *p;
    const WCHAR *zipname = base_name(b->zip);
    int k;
    lstrcpyW(addr, zipname);
    for (p = b->folder, k = lstrlenW(addr); *p && k < MAX_PATH * 2 - 4; p++)
    {
        if (p == b->folder) { lstrcpyW(addr + k, L" \x203a "); k += 3; }
        if (*p == '/') { if (p[1]) { lstrcpyW(addr + k, L" \x203a "); k += 3; } }
        else addr[k++] = *p;
        addr[k] = 0;
    }
    SetWindowTextW(GetDlgItem(b->hwnd, ID_ADDR), addr);
    if (b->folder[0])
    {
        lstrcpyW(t, b->folder);
        t[lstrlenW(t) - 1] = 0;
        SetWindowTextW(b->hwnd, base_name(t));
    }
    else SetWindowTextW(b->hwnd, zipname);
}

static void browser_fill(browser *b)
{
    int i, plen = lstrlenW(b->folder);
    WCHAR buf[64];
    SendMessageW(b->list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(b->list);
    b->nrows = 0; b->nfolders = 0;
    dumpf(TRUE, L"WINDOW browse");
    dumpf(FALSE, L"ZIP %ls", b->zip);
    dumpf(FALSE, L"FOLDER %ls", b->folder);
    for (i = 0; i < b->z.n && b->nrows < 4096; i++)
    {
        const WCHAR *name = b->z.e[i].name, *rest, *slash;
        WCHAR child[MAX_PATH / 2], type[80], col[128];
        LVITEMW it = { 0 };
        BOOL isdir;
        int k, row;
        if (_wcsnicmp(name, b->folder, plen)) continue;
        rest = name + plen;
        while (*rest == '/') rest++;
        if (!*rest) continue;
        slash = wcschr(rest, '/');
        isdir = slash != NULL;
        k = isdir ? (int)(slash - rest) : lstrlenW(rest);
        if (k >= MAX_PATH / 2) continue;
        lstrcpynW(child, rest, k + 1);
        if (isdir)
        {
            int f;
            for (f = 0; f < b->nfolders; f++) if (!lstrcmpiW(b->folders[f], child)) break;
            if (f < b->nfolders || b->nfolders == 512) continue;
            lstrcpyW(b->folders[b->nfolders++], child);
            row = -b->nfolders;          /* -1 - folder slot */
        }
        else row = i;
        it.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
        it.iItem = isdir ? b->nfolders - 1 : b->nrows;
        it.pszText = child;
        it.iImage = icon_index(child, isdir, type, 80);
        it.lParam = row;
        it.iItem = ListView_InsertItem(b->list, &it);
        ListView_SetItemText(b->list, it.iItem, 1, type);
        b->nrows++;
        if (isdir)
        {
            dumpf(FALSE, L"ROW %ls\t%ls", child, type);
            continue;
        }
        {
            const zentry *e = &b->z.e[i];
            WCHAR csz[64], usz[64], date[128];
            fmt_size(e->csize, csz, 64);
            fmt_size(e->usize, usz, 64);
            ListView_SetItemText(b->list, it.iItem, 2, csz);
            ListView_SetItemText(b->list, it.iItem, 3, e->flags & 1 ? L"Yes" : L"No");
            ListView_SetItemText(b->list, it.iItem, 4, usz);
            if (e->usize) _snwprintf(col, 128, L"%d%%", (int)(100 - (e->csize * 100 + e->usize / 2) / e->usize));
            else lstrcpyW(col, L"0%");
            if (col[0] == '-') lstrcpyW(col, L"0%");
            ListView_SetItemText(b->list, it.iItem, 5, col);
            fmt_date(e, date, 128);
            ListView_SetItemText(b->list, it.iItem, 6, date);
            dumpf(FALSE, L"ROW %ls\t%ls\t%ls\t%ls\t%ls\t%ls\t%ls", child, type, csz, e->flags & 1 ? L"Yes" : L"No", usz, col, date);
        }
    }
    SendMessageW(b->list, WM_SETREDRAW, TRUE, 0);
    if (b->nrows) ListView_SetItemState(b->list, 0, LVIS_FOCUSED, LVIS_FOCUSED);
    _snwprintf(buf, 64, b->nrows == 1 ? L"%d item" : L"%d items", b->nrows);
    buf[63] = 0;
    SetWindowTextW(b->status, buf);
    EnableWindow(b->back, b->hpos > 0);
    EnableWindow(b->fwd, b->hpos < b->nhist - 1);
    EnableWindow(b->up, b->folder[0] != 0);
    browser_title(b);
}

static void browser_go(browser *b, const WCHAR *folder, BOOL record)
{
    lstrcpynW(b->folder, folder, MAX_PATH);
    if (record)
    {
        if (b->hpos < b->nhist - 1) b->nhist = b->hpos + 1;
        if (b->nhist == 32) { memmove(b->history[0], b->history[1], sizeof(b->history[0]) * 31); b->nhist--; }
        lstrcpyW(b->history[b->nhist], folder);
        b->hpos = b->nhist++;
    }
    browser_fill(b);
}

static void browser_up(browser *b)
{
    WCHAR f[MAX_PATH], *s;
    if (!b->folder[0]) return;
    lstrcpyW(f, b->folder);
    f[lstrlenW(f) - 1] = 0;
    if ((s = wcsrchr(f, '/'))) s[1] = 0;
    else f[0] = 0;
    browser_go(b, f, TRUE);
}

/* a file inside: extracted to a temporary folder and opened, read-only, as Windows does */
static void browser_open_file(browser *b, int i)
{
    WCHAR tmp[MAX_PATH], dir[MAX_PATH], full[MAX_PATH];
    BYTE *data;
    size_t len;
    int r;
    HANDLE f;
    DWORD n;
    if ((r = zip_read(&b->z, i, &data, &len))) { error_box(b->hwnd, zip_strerror(r), b->z.e[i].name); return; }
    GetTempPathW(MAX_PATH, tmp);
    _snwprintf(dir, MAX_PATH, L"%lsTemp1_%ls", tmp, base_name(b->zip));
    dir[MAX_PATH - 1] = 0;
    make_dirs(dir);
    _snwprintf(full, MAX_PATH, L"%ls\\%ls", dir, base_name(b->z.e[i].name));
    full[MAX_PATH - 1] = 0;
    SetFileAttributesW(full, FILE_ATTRIBUTE_NORMAL);
    f = CreateFileW(full, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_READONLY, NULL);
    if (f == INVALID_HANDLE_VALUE || !WriteFile(f, data, (DWORD)len, &n, NULL))
    { if (f != INVALID_HANDLE_VALUE) CloseHandle(f); free(data); error_box(b->hwnd, zip_strerror(ZE_WRITE), full); return; }
    CloseHandle(f);
    free(data);
    dumpf(FALSE, L"OPENED %ls", full);
    ShellExecuteW(b->hwnd, NULL, full, NULL, dir, SW_SHOWNORMAL);
}

static void browser_activate(browser *b)
{
    int sel = ListView_GetNextItem(b->list, -1, LVNI_FOCUSED);
    LVITEMW it = { 0 };
    if (sel < 0) return;
    it.mask = LVIF_PARAM;
    it.iItem = sel;
    ListView_GetItem(b->list, &it);
    if (it.lParam < 0)
    {
        WCHAR f[MAX_PATH];
        _snwprintf(f, MAX_PATH, L"%ls%ls/", b->folder, b->folders[-it.lParam - 1]);
        f[MAX_PATH - 1] = 0;
        browser_go(b, f, TRUE);
    }
    else browser_open_file(b, (int)it.lParam);
}

static void browser_layout(browser *b)
{
    RECT rc;
    int bar = S(44), nav = S(34), st = S(24);
    GetClientRect(b->hwnd, &rc);
    MoveWindow(b->extract, S(8), S(6), S(116), bar - S(12), TRUE);
    MoveWindow(b->back, S(6), bar + S(3), S(28), nav - S(6), TRUE);
    MoveWindow(b->fwd, S(36), bar + S(3), S(28), nav - S(6), TRUE);
    MoveWindow(b->up, S(66), bar + S(3), S(28), nav - S(6), TRUE);
    MoveWindow(GetDlgItem(b->hwnd, ID_ADDR), S(102), bar + S(5), rc.right - S(110), nav - S(10), TRUE);
    MoveWindow(b->list, 0, bar + nav, rc.right, rc.bottom - bar - nav - st, TRUE);
    MoveWindow(b->status, S(10), rc.bottom - st, rc.right - S(20), st, TRUE);
}

static WNDPROC g_list_proc;
static LRESULT CALLBACK list_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    browser *b = (browser *)GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA);
    if (msg == WM_KEYDOWN && b)
    {
        if (wp == VK_RETURN) { browser_activate(b); return 0; }
        if (wp == VK_BACK) { browser_up(b); return 0; }
    }
    if (msg == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    return CallWindowProcW(g_list_proc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK browser_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_CREATE) sg_mode_title(hwnd, sgm_dark);
    if (sg_mode_changed(msg, lp))
    {
        sgm_follow(hwnd);
        EnumChildWindows(hwnd, list_colours, 0);
        InvalidateRect(hwnd, NULL, TRUE);
    }
    browser *b = (browser *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg)
    {
    case WM_CREATE:
    {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        static const struct { const WCHAR *name; int width, fmt; } cols[] = {
            { L"Name", 220, LVCFMT_LEFT }, { L"Type", 110, LVCFMT_LEFT }, { L"Compressed size", 110, LVCFMT_RIGHT },
            { L"Password protected", 120, LVCFMT_LEFT }, { L"Size", 80, LVCFMT_RIGHT }, { L"Ratio", 50, LVCFMT_RIGHT },
            { L"Date modified", 130, LVCFMT_LEFT } };
        SHFILEINFOW sfi;
        HWND addr;
        int k;
        b = cs->lpCreateParams;
        b->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)b);
        b->extract = flat_button(hwnd, L"Extract all", ID_EXTRACT);
        b->back = flat_button(hwnd, L"Back", ID_BACK);
        b->fwd = flat_button(hwnd, L"Forward", ID_FWD);
        b->up = flat_button(hwnd, L"Up", ID_UP);
        addr = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_READONLY | ES_AUTOHSCROLL,
                               0, 0, 10, 10, hwnd, (HMENU)ID_ADDR, g_inst, NULL);
        SendMessageW(addr, WM_SETFONT, (WPARAM)g_font, FALSE);
        b->list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                                  LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS,
                                  0, 0, 10, 10, hwnd, (HMENU)ID_LIST, g_inst, NULL);
        ListView_SetExtendedListViewStyle(b->list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        list_colours(b->list, 0);
        SendMessageW(b->list, WM_SETFONT, (WPARAM)g_font, FALSE);
        b->images = (HIMAGELIST)SHGetFileInfoW(L"C:\\", 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX | SHGFI_SMALLICON);
        if (b->images) ListView_SetImageList(b->list, b->images, LVSIL_SMALL);
        for (k = 0; k < 7; k++)
        {
            LVCOLUMNW c = { 0 };
            c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
            c.pszText = (WCHAR *)cols[k].name;
            c.cx = S(cols[k].width);
            c.fmt = cols[k].fmt;
            ListView_InsertColumn(b->list, k, &c);
        }
        g_list_proc = (WNDPROC)SetWindowLongPtrW(b->list, GWLP_WNDPROC, (LONG_PTR)list_proc);
        b->status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_CENTERIMAGE,
                                  0, 0, 10, 10, hwnd, (HMENU)ID_STATUS, g_inst, NULL);
        SendMessageW(b->status, WM_SETFONT, (WPARAM)g_font, FALSE);
        browser_layout(b);
        browser_go(b, L"", TRUE);
        SetFocus(b->list);
        return 0;
    }
    case WM_SIZE:
        if (b) browser_layout(b);
        return 0;
    case WM_SETFOCUS:
        if (b) SetFocus(b->list);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, GetDlgCtrlID((HWND)lp) == ID_ADDR ? COL_BG : COL_BG);
        SetTextColor((HDC)wp, GetDlgCtrlID((HWND)lp) == ID_STATUS ? COL_SUBTLE : COL_TEXT);
        return (LRESULT)bg_brush((HDC)wp);
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, r;
        HBRUSH line = CreateSolidBrush(COL_LINE);
        GetClientRect(hwnd, &rc);
        FillRect(dc, &rc, bg_brush(dc));
        SetRect(&r, 0, S(44) - 1, rc.right, S(44)); FillRect(dc, &r, line);
        SetRect(&r, 0, rc.bottom - S(24), rc.right, rc.bottom - S(24) + 1); FillRect(dc, &r, line);
        DeleteObject(line);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM:
    {
        DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
        draw_flat_button(di, di->hwndItem == g_hot_button);
        return TRUE;
    }
    case WM_NOTIFY:
    {
        NMHDR *nh = (NMHDR *)lp;
        if (nh->idFrom == ID_LIST && (nh->code == NM_DBLCLK || nh->code == LVN_ITEMACTIVATE)) browser_activate(b);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_EXTRACT:
        {
            WCHAR self[MAX_PATH], args[MAX_PATH + 32];
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi;
            GetModuleFileNameW(NULL, self, MAX_PATH);
            _snwprintf(args, MAX_PATH + 31, L"\"%ls\" /extractall \"%ls\"", self, b->zip);
            args[MAX_PATH + 31] = 0;
            if (CreateProcessW(self, args, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
            { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
            return 0;
        }
        case ID_BACK:
            if (b->hpos > 0) { b->hpos--; browser_go(b, b->history[b->hpos], FALSE); }
            return 0;
        case ID_FWD:
            if (b->hpos < b->nhist - 1) { b->hpos++; browser_go(b, b->history[b->hpos], FALSE); }
            return 0;
        case ID_UP:
            browser_up(b);
            return 0;
        }
        break;
    case WM_XBUTTONUP:
        SendMessageW(hwnd, WM_COMMAND, GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? ID_BACK : ID_FWD, 0);
        return TRUE;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int run_browser(const WCHAR *zip)
{
    static browser b;
    WNDCLASSW wc = { 0 };
    MSG m;
    ACCEL acc[] = { { FALT | FVIRTKEY, VK_UP, ID_UP }, { FALT | FVIRTKEY, VK_LEFT, ID_BACK },
                    { FALT | FVIRTKEY, VK_RIGHT, ID_FWD } };
    HACCEL accel = CreateAcceleratorTableW(acc, 3);
    int r;
    memset(&b, 0, sizeof(b));
    GetFullPathNameW(zip, MAX_PATH, b.zip, NULL);
    if ((r = zip_open(&b.z, b.zip)))
    {
        WCHAR msg[MAX_PATH + 128];
        _snwprintf(msg, MAX_PATH + 127, L"Stained Glass cannot open the folder.\n\nThe Compressed (zipped) Folder '%ls' is invalid.", b.zip);
        msg[MAX_PATH + 127] = 0;
        dumpf(TRUE, L"WINDOW error");
        MessageBoxW(NULL, msg, L"Compressed (zipped) Folders Error", MB_OK | MB_ICONERROR);
        return 1;
    }
    wc.lpfnWndProc = browser_proc;
    wc.hInstance = g_inst;
    wc.lpszClassName = L"SgZipBrowse";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    CreateWindowW(wc.lpszClassName, base_name(b.zip), WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                  CW_USEDEFAULT, CW_USEDEFAULT, S(860), S(520), NULL, NULL, g_inst, &b);
    while (GetMessageW(&m, NULL, 0, 0) > 0)
    {
        if (TranslateAcceleratorW(b.hwnd, accel, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    zip_close(&b.z);
    return 0;
}

/* ---- the command line ----------------------------------------------------------------------- */
int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    WCHAR **argv, *log = NULL, *out = NULL, *items[1024];
    int argc, i, nitems = 0, policy = POLICY_ASK, rc = 0;
    BOOL quiet = FALSE;
    enum { M_BROWSE, M_WIZARD, M_EXTRACT, M_CREATE, M_LIST } mode = M_BROWSE;
    HDC dc;
    (void)prev; (void)cmdline; (void)show;
    sgm_dark = sg_apps_dark();

    g_inst = inst;
    SetProcessDPIAware();
    dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    g_font = make_font(90, FW_NORMAL, L"Segoe UI");
    g_font_title = make_font(120, FW_NORMAL, L"Segoe UI");
    g_font_glyph = make_font(100, FW_NORMAL, L"Segoe UI");
    GetEnvironmentVariableW(L"SG_ZIP_DUMP", g_dump, MAX_PATH);

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; i++)
    {
        WCHAR *a = argv[i];
        if (!lstrcmpiW(a, L"/extractall")) mode = M_WIZARD;
        else if (!lstrcmpiW(a, L"/extract")) mode = M_EXTRACT;
        else if (!lstrcmpiW(a, L"/create")) mode = M_CREATE;
        else if (!lstrcmpiW(a, L"/list")) mode = M_LIST;
        else if (!lstrcmpiW(a, L"/quiet")) quiet = TRUE;
        else if (!lstrcmpiW(a, L"/overwrite")) policy = POLICY_OVERWRITE;
        else if (!lstrcmpiW(a, L"/skip")) policy = POLICY_SKIP;
        else if (!lstrcmpiW(a, L"/log") && i + 1 < argc) log = argv[++i];
        else if (!lstrcmpiW(a, L"/out") && i + 1 < argc) out = argv[++i];
        else if (nitems < 1024) items[nitems++] = a;
    }
    if (log)
        g_log = CreateFileW(log, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);

    switch (mode)
    {
    case M_CREATE:
        rc = do_create(items, nitems, out, quiet);
        break;
    case M_EXTRACT:
    case M_LIST:
    {
        zarchive z;
        int r;
        if (nitems < (mode == M_EXTRACT ? 2 : 1)) { rc = 2; break; }
        if ((r = zip_open(&z, items[0])))
        {
            zlog(L"ERROR\t%ls\t%ls", items[0], zip_strerror(r));
            if (!quiet) error_box(NULL, zip_strerror(r), items[0]);
            rc = 1;
            break;
        }
        if (mode == M_LIST)
        {
            for (i = 0; i < z.n; i++)
                zlog(L"ENTRY\t%ls\t%u\t%I64u\t%I64u\t%08lx", z.e[i].name, z.e[i].method, z.e[i].csize, z.e[i].usize, (unsigned long)z.e[i].crc);
        }
        else
        {
            xjob j;
            memset(&j, 0, sizeof(j));
            j.z = &z;
            GetFullPathNameW(items[1], MAX_PATH, j.dest, NULL);
            j.policy = policy == POLICY_ASK ? POLICY_SKIP : policy;
            extract_run(&j);
            zlog(L"SUMMARY\twritten=%d errors=%d refused=%d skipped=%d", j.written, j.errors, j.refused, j.skipped);
            if (!quiet && (j.errors || j.refused)) error_box(NULL, L"Some files could not be extracted.", j.first_error);
            rc = j.errors || j.refused ? 1 : 0;
        }
        zip_close(&z);
        break;
    }
    case M_WIZARD:
        if (!nitems) { rc = 2; break; }
        rc = run_wizard(items[0], NULL);
        break;
    case M_BROWSE:
        if (!nitems)
        {
            OPENFILENAMEW ofn = { sizeof(ofn) };
            static WCHAR path[MAX_PATH];
            ofn.lpstrFilter = L"Compressed (zipped) Folders (*.zip)\0*.zip\0All files (*.*)\0*.*\0";
            ofn.lpstrFile = path;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            if (!GetOpenFileNameW(&ofn)) break;
            items[nitems++] = path;
        }
        rc = run_browser(items[0]);
        break;
    }
    if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    LocalFree(argv);
    return rc;
}
