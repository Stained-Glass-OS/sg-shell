/* sg-start: the Stained Glass OS Start menu.
 *
 * A Windows 10-style Start menu, and a new surface that Wine's explorer does
 * not provide (ADR 0007). It runs alongside the session, keeps a hidden
 * listener window, and shows its panel when the shell's Start button is
 * pressed -- explorer posts SG_START_TOGGLE to the "SgStartPanel" window
 * (wine-sg patch). Click away and it hides.
 *
 * The panel: a dark column above the Start button, a power/lock/sign-out rail
 * down the left, and an alphabetical list of programs read from the Start Menu
 * folders, each launchable. It reuses the project's stained-glass palette.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 */
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

#define SG_START_TOGGLE (WM_USER + 10)

#define PANEL_W    340
#define PANEL_H    480
#define RAIL_W     48
#define ROW_H      36
#define TASKBAR_H  40

#define COL_PANEL   RGB(0x25, 0x25, 0x25)
#define COL_RAIL    RGB(0x1B, 0x1B, 0x1B)
#define COL_HOVER   RGB(0x3A, 0x3A, 0x3A)
#define COL_TEXT    RGB(0xFF, 0xFF, 0xFF)
#define COL_SUBTLE  RGB(0xB0, 0xB0, 0xB0)
#define COL_ACCENT  RGB(0x7B, 0x2F, 0xBE)
#define COL_DIM     RGB(0x55, 0x55, 0x55)  /* a policy-disabled glyph */

static const WCHAR LISTENER_CLASS[] = L"SgStartPanel";
static const WCHAR PANEL_CLASS[]    = L"SgStartWindow";

struct entry {
    WCHAR name[128];
    WCHAR path[MAX_PATH];   /* the .lnk or executable to launch */
};

static struct entry *g_entries;
static int g_count, g_cap;
static int g_scroll;         /* first visible row */
static int g_hot = -1;       /* hovered row, or -1 */
static int g_rail_hot = -1;  /* hovered rail button, or -1 */
static HWND g_panel;
static HFONT g_font, g_font_small, g_icon_font;
static HBRUSH g_panel_bg;

/* Rail buttons, bottom to top. */
enum { RAIL_POWER, RAIL_LOCK, RAIL_SIGNOUT, RAIL_COUNT };

static int entry_cmp(const void *a, const void *b)
{
    return lstrcmpiW(((const struct entry *)a)->name, ((const struct entry *)b)->name);
}

static void add_entry(const WCHAR *name, const WCHAR *path)
{
    int i;
    for (i = 0; i < g_count; i++)
        if (!lstrcmpiW(g_entries[i].name, name)) return;   /* de-dup by name */
    if (g_count == g_cap)
    {
        int cap = g_cap ? g_cap * 2 : 64;
        struct entry *n = realloc(g_entries, cap * sizeof(*n));
        if (!n) return;
        g_entries = n; g_cap = cap;
    }
    lstrcpynW(g_entries[g_count].name, name, 128);
    lstrcpynW(g_entries[g_count].path, path, MAX_PATH);
    g_count++;
}

/* Read the .lnk files under one Start Menu\Programs tree. */
static void scan_programs(int csidl)
{
    WCHAR base[MAX_PATH], pattern[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;

    if (!SHGetSpecialFolderPathW(NULL, base, csidl, FALSE)) return;
    /* One level of subfolders plus the top level is enough for Phase 0. */
    lstrcpynW(pattern, base, MAX_PATH);
    lstrcatW(pattern, L"\\*");
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        WCHAR full[MAX_PATH];
        if (fd.cFileName[0] == '.') continue;
        _snwprintf(full, MAX_PATH, L"%s\\%s", base, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            WCHAR sub[MAX_PATH];
            WIN32_FIND_DATAW sfd;
            HANDLE sh;
            _snwprintf(sub, MAX_PATH, L"%s\\*.lnk", full);
            sh = FindFirstFileW(sub, &sfd);
            if (sh != INVALID_HANDLE_VALUE)
            {
                do {
                    WCHAR lp[MAX_PATH], nm[128];
                    _snwprintf(lp, MAX_PATH, L"%s\\%s", full, sfd.cFileName);
                    lstrcpynW(nm, sfd.cFileName, 128);
                    { WCHAR *dot = wcsrchr(nm, '.'); if (dot) *dot = 0; }
                    add_entry(nm, lp);
                } while (FindNextFileW(sh, &sfd));
                FindClose(sh);
            }
        }
        else
        {
            WCHAR *ext = wcsrchr(fd.cFileName, '.');
            if (ext && !lstrcmpiW(ext, L".lnk"))
            {
                WCHAR nm[128];
                lstrcpynW(nm, fd.cFileName, 128);
                { WCHAR *dot = wcsrchr(nm, '.'); if (dot) *dot = 0; }
                add_entry(nm, full);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void build_list(void)
{
    g_count = 0;
    scan_programs(CSIDL_COMMON_PROGRAMS);
    scan_programs(CSIDL_PROGRAMS);
    /* A couple of built-ins so the menu is never empty. */
    add_entry(L"Notepad", L"notepad.exe");
    add_entry(L"File Explorer", L"explorer.exe");
    /* Remote Desktop Connection, when sg-mstsc is installed beside us --
     * found next to our own executable, so no install path is hardcoded. */
    {
        static const WCHAR mstsc[] = L"sg-mstsc64.exe";
        WCHAR self[MAX_PATH], *slash;
        DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
        if (n && n < MAX_PATH && (slash = wcsrchr(self, '\\')) &&
            (size_t)(slash + 1 - self) + ARRAYSIZE(mstsc) <= MAX_PATH)
        {
            lstrcpyW(slash + 1, mstsc);
            if (GetFileAttributesW(self) != INVALID_FILE_ATTRIBUTES)
                add_entry(L"Remote Desktop Connection", self);
        }
    }
    qsort(g_entries, g_count, sizeof(*g_entries), entry_cmp);
}

static int visible_rows(void) { return (PANEL_H - 8) / ROW_H; }

static void launch(const WCHAR *path)
{
    ShellExecuteW(NULL, L"open", path, NULL, NULL, SW_SHOWNORMAL);
}

static void run_native_lock(void)
{
    /* Windows LockWorkStation, which wine-sg routes to the compositor. */
    LockWorkStation();
}

/* Is a power-rail action forbidden by machine policy (Group Policy)? sg-start
 * honours the same restrictions Windows' shell does: NoClose disables shut
 * down, StartMenuLogoff removes sign-out. SHRestricted reads the machine
 * policy from HKLM first (wine-sg 0025), which an administrator sets and a user
 * cannot override. */
static BOOL rail_blocked(int which)
{
    switch (which)
    {
    case RAIL_POWER:   return SHRestricted(REST_NOCLOSE) != 0;
    case RAIL_SIGNOUT: return SHRestricted(REST_STARTMENULOGOFF) != 0;
    default:           return FALSE;
    }
}

static void rail_action(int which)
{
    if (rail_blocked(which)) return;   /* forbidden by policy */
    switch (which)
    {
    case RAIL_LOCK:    run_native_lock(); break;
    case RAIL_SIGNOUT: ExitWindowsEx(EWX_LOGOFF, 0); break;
    case RAIL_POWER:   ExitWindowsEx(EWX_SHUTDOWN, 0); break;
    }
}

/* ---- drawing ----------------------------------------------------------- */

static RECT rail_button_rect(int i)
{
    /* Stacked from the bottom of the rail. */
    RECT r = { 0, PANEL_H - (i + 1) * RAIL_W, RAIL_W, PANEL_H - i * RAIL_W };
    return r;
}

static void draw_rail_glyph(HDC dc, RECT r, int which, BOOL hot)
{
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    BOOL blocked = rail_blocked(which);
    HPEN pen = CreatePen(PS_SOLID, 2, blocked ? COL_DIM : (hot ? COL_TEXT : COL_SUBTLE));
    if (blocked) hot = FALSE;   /* never draw a disabled control as hot */
    HPEN old = SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    switch (which)
    {
    case RAIL_POWER:   /* a power ring with a top stem */
        Ellipse(dc, cx - 7, cy - 6, cx + 7, cy + 8);
        { HPEN mask = CreatePen(PS_SOLID, 3, hot ? COL_HOVER : COL_RAIL);
          SelectObject(dc, mask);
          MoveToEx(dc, cx, cy - 9, NULL); LineTo(dc, cx, cy + 1);
          SelectObject(dc, pen); DeleteObject(mask); }
        MoveToEx(dc, cx, cy - 9, NULL); LineTo(dc, cx, cy);
        break;
    case RAIL_LOCK:    /* a padlock: body + shackle */
        RoundRect(dc, cx - 7, cy, cx + 7, cy + 11, 3, 3);
        Arc(dc, cx - 5, cy - 8, cx + 5, cy + 4, cx - 5, cy + 1, cx + 5, cy + 1);
        break;
    case RAIL_SIGNOUT: /* a person */
        Ellipse(dc, cx - 4, cy - 8, cx + 4, cy);
        Arc(dc, cx - 8, cy, cx + 8, cy + 16, cx + 8, cy + 4, cx - 8, cy + 4);
        break;
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

static void on_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rail = { 0, 0, RAIL_W, PANEL_H };
    HBRUSH br = CreateSolidBrush(COL_RAIL);
    int rows = visible_rows(), i;

    /* The panel body is the class background (COL_PANEL); draw the rail and the
     * program list on top of it. Direct drawing -- no back buffer -- because
     * the panel is small and repaints are rare. */
    FillRect(dc, &rail, br);
    DeleteObject(br);

    for (i = 0; i < RAIL_COUNT; i++)
    {
        RECT rb = rail_button_rect(i);
        if (g_rail_hot == i) { HBRUSH h = CreateSolidBrush(COL_HOVER); FillRect(dc, &rb, h); DeleteObject(h); }
        draw_rail_glyph(dc, rb, i, g_rail_hot == i);
    }

    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font);
    for (i = 0; i < rows && g_scroll + i < g_count; i++)
    {
        int idx = g_scroll + i;
        RECT row = { RAIL_W, 4 + i * ROW_H, PANEL_W, 4 + (i + 1) * ROW_H };
        RECT text = row; text.left += 14;
        if (idx == g_hot) { HBRUSH h = CreateSolidBrush(COL_HOVER); FillRect(dc, &row, h); DeleteObject(h); }
        SetTextColor(dc, COL_TEXT);
        DrawTextW(dc, g_entries[idx].name, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    EndPaint(hwnd, &ps);
}

static void position_panel(HWND hwnd)
{
    int sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, sh - TASKBAR_H - PANEL_H, PANEL_W, PANEL_H,
                 SWP_NOACTIVATE);
}

static void show_panel(HWND hwnd, BOOL show)
{
    if (show)
    {
        build_list();
        g_scroll = 0;
        g_hot = g_rail_hot = -1;
        position_panel(hwnd);
        ShowWindow(hwnd, SW_SHOW);
        SetForegroundWindow(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
    }
    else ShowWindow(hwnd, SW_HIDE);
}

static int row_at(int y)
{
    int i = (y - 4) / ROW_H;
    if (i < 0 || i >= visible_rows() || g_scroll + i >= g_count) return -1;
    return g_scroll + i;
}

static int rail_at(int x, int y)
{
    int i;
    if (x >= RAIL_W) return -1;
    for (i = 0; i < RAIL_COUNT; i++)
    {
        RECT r = rail_button_rect(i);
        if (y >= r.top && y < r.bottom) return i;
    }
    return -1;
}

static LRESULT CALLBACK panel_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT: on_paint(hwnd); return 0;
    case WM_MOUSEMOVE:
    {
        int x = LOWORD(lp), y = HIWORD(lp);
        int row = (x >= RAIL_W) ? row_at(y) : -1;
        int rail = rail_at(x, y);
        if (row != g_hot || rail != g_rail_hot)
        {
            g_hot = row; g_rail_hot = rail;
            InvalidateRect(hwnd, NULL, FALSE);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hot = g_rail_hot = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_MOUSEWHEEL:
    {
        int delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -1 : 1;
        int maxs = g_count - visible_rows();
        if (maxs < 0) maxs = 0;
        g_scroll += delta * 3;
        if (g_scroll < 0) g_scroll = 0;
        if (g_scroll > maxs) g_scroll = maxs;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        int x = LOWORD(lp), y = HIWORD(lp);
        int rail = rail_at(x, y);
        if (rail >= 0) { show_panel(hwnd, FALSE); rail_action(rail); return 0; }
        int row = (x >= RAIL_W) ? row_at(y) : -1;
        if (row >= 0) { WCHAR p[MAX_PATH]; lstrcpynW(p, g_entries[row].path, MAX_PATH); show_panel(hwnd, FALSE); launch(p); }
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) show_panel(hwnd, FALSE);   /* click-away closes */
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) show_panel(hwnd, FALSE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* The hidden listener explorer posts SG_START_TOGGLE to. */
static LRESULT CALLBACK listener_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == SG_START_TOGGLE)
    {
        show_panel(g_panel, !IsWindowVisible(g_panel));
        return 0;
    }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSW lc = { 0 }, pc = { 0 };
    HWND listener;
    MSG msg;

    (void)prev; (void)cmd; (void)show;
    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                         CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_small = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                               CLEARTYPE_QUALITY, 0, L"Segoe UI");

    lc.lpfnWndProc = listener_proc; lc.hInstance = inst; lc.lpszClassName = LISTENER_CLASS;
    RegisterClassW(&lc);
    g_panel_bg = CreateSolidBrush(COL_PANEL);
    pc.lpfnWndProc = panel_proc; pc.hInstance = inst; pc.lpszClassName = PANEL_CLASS;
    pc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    pc.hbrBackground = g_panel_bg;
    RegisterClassW(&pc);

    listener = CreateWindowW(LISTENER_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);
    g_panel = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, PANEL_CLASS, L"",
                              WS_POPUP, 0, 0, PANEL_W, PANEL_H, NULL, NULL, inst, NULL);
    (void)listener;

    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
