/* sg-taskmgr -- Task Manager, as Windows 10 has it.
 *
 * "Fewer details": the running apps and End task. "More details": the
 * tabs -- Processes (apps, background and Windows processes, with CPU,
 * memory and disk shaded by how busy they are), Performance (CPU, memory,
 * network graphs), Startup (Run keys and Startup folders, enabled and
 * disabled through StartupApproved as Windows keeps it), Users, Details and
 * Services. Everything is re-read once a second (View > Update speed).
 *
 * taskmgr.exe reaches this through App Paths (defaults/76-sg-taskmgr.reg)
 * and wine-sg's hand-off in Wine's own taskmgr. `/startup` (or `/0
 * /startup`) opens the Startup tab. SG_TASKMGR_DUMP=<file> writes what is
 * shown, with screen positions, after every refresh -- for the gate.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "taskmgr.h"
/* Stained Glass: the app mode picks the palette (taskmgr.h); switched live */
BOOL sgm_dark;
void sgm_follow(HWND hwnd)
{
    BOOL dark = sg_apps_dark();
    if (dark == sgm_dark) return;
    sgm_dark = dark;
    sg_mode_title(hwnd, dark);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}
#include <commdlg.h>

#define MAIN_CLASS L"SgTaskManagerWindow"
#define PERF_CLASS L"SgTaskmgrPerf"
#define SETTINGS_KEY L"Software\\Stained Glass\\TaskManager"
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define APPROVED_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved"

int g_dpi = 96;
HFONT g_font, g_font_bold, g_font_small, g_font_big, g_font_head, g_font_value;

enum { TAB_PROCESSES, TAB_PERFORMANCE, TAB_STARTUP, TAB_USERS, TAB_DETAILS, TAB_SERVICES, TAB_COUNT };
static const WCHAR *const tab_names[TAB_COUNT] = { L"Processes", L"Performance", L"Startup", L"Users", L"Details", L"Services" };

enum {
    IDM_RUN = 100, IDM_EXIT, IDM_TOPMOST, IDM_REFRESH, IDM_SPEED_HIGH, IDM_SPEED_NORMAL, IDM_SPEED_LOW,
    IDM_SPEED_PAUSED, IDM_END, IDM_END_TREE, IDM_LOCATION, IDM_DETAILS, IDM_SVC_START, IDM_SVC_STOP,
    IDM_SVC_RESTART, IDM_STARTUP_TOGGLE, IDM_TOGGLE_MODE, IDM_NEXT_TAB, IDM_PREV_TAB, IDM_ACTION,
};

static HWND g_main, g_perf_wnd;
static BOOL g_more;                 /* More details */
static int g_tab;
static int g_speed = IDM_SPEED_NORMAL;
static BOOL g_topmost;
static HMENU g_menu;
static grid_t g_grids[TAB_COUNT], g_fewer;
static int g_perf_sel;              /* 0 CPU, 1 memory, 2 network */
static RECT g_tab_rc[TAB_COUNT], g_link_rc, g_button_rc;
static int g_hot_tab = -1, g_hot_link, g_hot_button;
static WCHAR g_dump_path[MAX_PATH];
static WCHAR g_last_action[128];

/* ---- startup entries ---------------------------------------------------- */
enum { SRC_HKCU_RUN, SRC_HKLM_RUN, SRC_HKLM_RUN32, SRC_USER_FOLDER, SRC_COMMON_FOLDER };
typedef struct { WCHAR value[128], cmd[MAX_PATH], path[MAX_PATH], name[128], publisher[96]; int src; BOOL enabled; } startup_t;
static startup_t g_startup[128];
static int g_nstartup;

HFONT make_font(int pt10, int weight)
{
    return CreateFontW(-MulDiv(pt10, g_dpi, 720), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                       CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, r, b);
    DeleteObject(b);
}

/* ---- icons, drawn ------------------------------------------------------- */

/* A 32-bit icon from a drawing function, alpha from a supersampled canvas */
static HICON draw_icon(int size, void (*draw)(HDC, int))
{
    const int ss = 4, n = size * ss;
    BITMAPINFO bi = { { sizeof(BITMAPINFOHEADER), n, -n, 1, 32, BI_RGB, 0, 0, 0, 0, 0 }, { { 0, 0, 0, 0 } } };
    BITMAPINFO bo = { { sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB, 0, 0, 0, 0, 0 }, { { 0, 0, 0, 0 } } };
    DWORD *big, *out;
    HDC dc = CreateCompatibleDC(NULL);
    HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&big, NULL, 0), ho, mask;
    ICONINFO ii = { TRUE, 0, 0, NULL, NULL };
    HICON icon;
    int x, y, i, j;
    HGDIOBJ old = SelectObject(dc, hb);
    for (i = 0; i < n * n; i++) big[i] = 0xFF00FF;                     /* key colour */
    draw(dc, n);
    GdiFlush();
    ho = CreateDIBSection(dc, &bo, DIB_RGB_COLORS, (void **)&out, NULL, 0);
    for (y = 0; y < size; y++)
        for (x = 0; x < size; x++)
        {
            unsigned r = 0, g = 0, b = 0, a = 0;
            for (j = 0; j < ss; j++)
                for (i = 0; i < ss; i++)
                {
                    DWORD p = big[(y * ss + j) * n + x * ss + i];
                    if ((p & 0xFFFFFF) == 0xFF00FF) continue;
                    r += (p >> 16) & 0xFF; g += (p >> 8) & 0xFF; b += p & 0xFF; a++;
                }
            out[y * size + x] = a ? ((a * 255 / (ss * ss)) << 24) | ((r / (ss * ss)) << 16) | ((g / (ss * ss)) << 8) | (b / (ss * ss)) : 0;
        }
    SelectObject(dc, old);
    mask = CreateBitmap(size, size, 1, 1, NULL);
    ii.hbmColor = ho; ii.hbmMask = mask;
    icon = CreateIconIndirect(&ii);
    DeleteObject(mask); DeleteObject(ho); DeleteObject(hb); DeleteDC(dc);
    return icon;
}

static void draw_generic(HDC dc, int n)
{
    HBRUSH frame = CreateSolidBrush(RGB(120, 120, 120)), body = CreateSolidBrush(RGB(255, 255, 255)),
           bar = CreateSolidBrush(C_ACCENT);
    RECT r = { n / 16, n * 3 / 16, n - n / 16, n - n * 2 / 16 };
    FillRect(dc, &r, frame);
    InflateRect(&r, -n / 16, -n / 16);
    FillRect(dc, &r, body);
    r.bottom = r.top + n * 3 / 16;
    FillRect(dc, &r, bar);
    DeleteObject(frame); DeleteObject(body); DeleteObject(bar);
}

HICON generic_icon(void)
{
    static HICON icon;
    if (!icon) icon = draw_icon(S(16), draw_generic);
    return icon;
}

/* ---- settings ----------------------------------------------------------- */

static DWORD setting(const WCHAR *name, DWORD def)
{
    DWORD v = def, cb = sizeof(v), type;
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_READ, &k)) return def;
    if (RegQueryValueExW(k, name, NULL, &type, (BYTE *)&v, &cb) || type != REG_DWORD) v = def;
    RegCloseKey(k);
    return v;
}

static void save_setting(const WCHAR *name, DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    RegSetValueExW(k, name, 0, REG_DWORD, (BYTE *)&v, sizeof(v));
    RegCloseKey(k);
}

/* ---- the Startup tab's data --------------------------------------------- */

/* the program a command line runs: a quoted path, or up to ".exe " */
static void command_path(const WCHAR *cmd, WCHAR *out, int cch)
{
    const WCHAR *e;
    WCHAR expanded[MAX_PATH];
    out[0] = 0;
    ExpandEnvironmentStringsW(cmd, expanded, MAX_PATH);
    cmd = expanded;
    while (*cmd == ' ') cmd++;
    if (*cmd == '"')
    {
        if ((e = wcschr(cmd + 1, '"'))) lstrcpynW(out, cmd + 1, (int)min(cch, e - cmd));
        return;
    }
    {
        WCHAR low[MAX_PATH];
        WCHAR *x;
        lstrcpynW(low, cmd, MAX_PATH);
        CharLowerW(low);
        if ((x = wcsstr(low, L".exe"))) lstrcpynW(out, cmd, (int)min(cch, x - low + 5));
        else if ((e = wcschr(cmd, ' '))) lstrcpynW(out, cmd, (int)min(cch, e - cmd + 1));
        else lstrcpynW(out, cmd, cch);
    }
}

static void file_strings(const WCHAR *path, WCHAR *desc, int dcch, WCHAR *company, int ccch)
{
    DWORD size, dummy;
    BYTE *ver;
    struct { WORD lang, cp; } *tr;
    UINT n;
    WCHAR q[96], *s;
    if (!path[0] || !(size = GetFileVersionInfoSizeW(path, &dummy)) || !(ver = malloc(size))) return;
    if (GetFileVersionInfoW(path, 0, size, ver) && VerQueryValueW(ver, L"\\VarFileInfo\\Translation", (void **)&tr, &n) && n >= sizeof(*tr))
    {
        _snwprintf(q, 96, L"\\StringFileInfo\\%04x%04x\\FileDescription", tr->lang, tr->cp);
        if (VerQueryValueW(ver, q, (void **)&s, &n) && n > 1 && *s) lstrcpynW(desc, s, dcch);
        _snwprintf(q, 96, L"\\StringFileInfo\\%04x%04x\\CompanyName", tr->lang, tr->cp);
        if (VerQueryValueW(ver, q, (void **)&s, &n) && n > 1 && *s) lstrcpynW(company, s, ccch);
    }
    free(ver);
}

static HKEY approved_root(int src) { return src == SRC_HKCU_RUN || src == SRC_USER_FOLDER ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE; }
static const WCHAR *approved_sub(int src)
{
    return src == SRC_HKLM_RUN32 ? APPROVED_KEY L"\\Run32"
         : src == SRC_USER_FOLDER || src == SRC_COMMON_FOLDER ? APPROVED_KEY L"\\StartupFolder"
         : APPROVED_KEY L"\\Run";
}

/* StartupApproved: a 12-byte value, its first byte even when enabled, odd when
 * disabled (Windows writes 02 and 03), then when it was changed. Absent is enabled. */
static BOOL approved(int src, const WCHAR *value)
{
    BYTE b[12];
    DWORD cb = sizeof(b), type;
    HKEY k;
    BOOL on = TRUE;
    if (RegOpenKeyExW(approved_root(src), approved_sub(src), 0, KEY_READ, &k)) return TRUE;
    if (!RegQueryValueExW(k, value, NULL, &type, b, &cb) && type == REG_BINARY && cb >= 1) on = !(b[0] & 1);
    RegCloseKey(k);
    return on;
}

static BOOL set_approved(int src, const WCHAR *value, BOOL on)
{
    BYTE b[12] = { 0 };
    HKEY k;
    LONG r;
    b[0] = on ? 0x02 : 0x03;
    if (!on) { FILETIME ft; GetSystemTimeAsFileTime(&ft); memcpy(b + 4, &ft, sizeof(ft)); }
    if (RegCreateKeyExW(approved_root(src), approved_sub(src), 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return FALSE;
    r = RegSetValueExW(k, value, 0, REG_BINARY, b, sizeof(b));
    RegCloseKey(k);
    return !r;
}

static void add_startup(int src, const WCHAR *value, const WCHAR *cmd)
{
    startup_t *s;
    WCHAR *dot;
    if (g_nstartup >= (int)ARRAYSIZE(g_startup)) return;
    s = &g_startup[g_nstartup++];
    memset(s, 0, sizeof(*s));
    s->src = src;
    lstrcpynW(s->value, value, 128);
    lstrcpynW(s->cmd, cmd, MAX_PATH);
    if (src == SRC_USER_FOLDER || src == SRC_COMMON_FOLDER) lstrcpynW(s->path, cmd, MAX_PATH);
    else command_path(cmd, s->path, MAX_PATH);
    file_strings(s->path, s->name, 128, s->publisher, 96);
    if (!s->name[0])
    {
        lstrcpynW(s->name, value, 128);
        if ((src == SRC_USER_FOLDER || src == SRC_COMMON_FOLDER) && (dot = wcsrchr(s->name, '.'))) *dot = 0;
    }
    s->enabled = approved(src, value);
}

static void read_run(HKEY root, const WCHAR *sub, REGSAM view, int src)
{
    HKEY k;
    DWORD i, nn, cb, type;
    WCHAR name[128], data[MAX_PATH];
    if (RegOpenKeyExW(root, sub, 0, KEY_READ | view, &k)) return;
    for (i = 0;; i++)
    {
        nn = 128; cb = sizeof(data) - sizeof(WCHAR);
        if (RegEnumValueW(k, i, name, &nn, NULL, &type, (BYTE *)data, &cb)) break;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        data[cb / sizeof(WCHAR)] = 0;
        add_startup(src, name, data);
    }
    RegCloseKey(k);
}

static void read_folder(int csidl, int src)
{
    WCHAR dir[MAX_PATH], pat[MAX_PATH], full[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!SHGetSpecialFolderPathW(NULL, dir, csidl, FALSE)) return;
    _snwprintf(pat, MAX_PATH, L"%s\\*", dir);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || !lstrcmpiW(fd.cFileName, L"desktop.ini")) continue;
        _snwprintf(full, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        add_startup(src, fd.cFileName, full);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void read_startup(void)
{
    g_nstartup = 0;
    read_run(HKEY_CURRENT_USER, RUN_KEY, 0, SRC_HKCU_RUN);
    read_run(HKEY_LOCAL_MACHINE, RUN_KEY, KEY_WOW64_64KEY, SRC_HKLM_RUN);
    read_run(HKEY_LOCAL_MACHINE, RUN_KEY, KEY_WOW64_32KEY, SRC_HKLM_RUN32);
    read_folder(CSIDL_STARTUP, SRC_USER_FOLDER);
    read_folder(CSIDL_COMMON_STARTUP, SRC_COMMON_FOLDER);
}

static startup_t *selected_startup(void)
{
    grow_t *r = grid_selected(&g_grids[TAB_STARTUP]);
    int i;
    if (!r) return NULL;
    for (i = 0; i < g_nstartup; i++)
    {
        WCHAR key[64];
        _snwprintf(key, 64, L"%d:%s", g_startup[i].src, g_startup[i].value);
        key[63] = 0;
        if (!lstrcmpW(key, r->skey)) return &g_startup[i];
    }
    return NULL;
}

/* ---- the pages' rows ----------------------------------------------------- */

static void set_col(grid_t *g, int c, const WCHAR *name, int width, BOOL right, BOOL heat, BOOL numeric)
{
    g->cols[c].name = name; g->cols[c].width = width; g->cols[c].right = right;
    g->cols[c].heat = heat; g->cols[c].numeric = numeric;
    if (c >= g->ncol) g->ncol = c + 1;
}

static void setup_grids(void)
{
    grid_t *g;
    g = &g_grids[TAB_PROCESSES];
    set_col(g, 0, L"Name", 200, 0, 0, 0); set_col(g, 1, L"Status", 110, 0, 0, 0);
    set_col(g, 2, L"CPU", 76, 1, 1, 1); set_col(g, 3, L"Memory", 90, 1, 1, 1); set_col(g, 4, L"Disk", 80, 1, 1, 1);
    g->sortcol = -1;
    g = &g_grids[TAB_STARTUP];
    set_col(g, 0, L"Name", 200, 0, 0, 0); set_col(g, 1, L"Publisher", 170, 0, 0, 0);
    set_col(g, 2, L"Status", 90, 0, 0, 0); set_col(g, 3, L"Startup impact", 110, 0, 0, 0);
    g = &g_grids[TAB_USERS];
    set_col(g, 0, L"User", 200, 0, 0, 0); set_col(g, 1, L"Status", 110, 0, 0, 0);
    set_col(g, 2, L"CPU", 76, 1, 1, 1); set_col(g, 3, L"Memory", 90, 1, 1, 1);
    g = &g_grids[TAB_DETAILS];
    set_col(g, 0, L"Name", 150, 0, 0, 0); set_col(g, 1, L"PID", 56, 1, 0, 1); set_col(g, 2, L"Status", 90, 0, 0, 0);
    set_col(g, 3, L"User name", 90, 0, 0, 0); set_col(g, 4, L"CPU", 44, 1, 0, 1); set_col(g, 5, L"Memory", 84, 1, 0, 1);
    set_col(g, 6, L"Architecture", 84, 0, 0, 0); set_col(g, 7, L"Description", 170, 0, 0, 0);
    g = &g_grids[TAB_SERVICES];
    set_col(g, 0, L"Name", 140, 0, 0, 0); set_col(g, 1, L"PID", 56, 1, 0, 1);
    set_col(g, 2, L"Description", 250, 0, 0, 0); set_col(g, 3, L"Status", 90, 0, 0, 0);
    g_fewer.noheader = TRUE;
    set_col(&g_fewer, 0, L"Name", 200, 0, 0, 0);
    g_fewer.sortcol = 0;
}

static const WCHAR *proc_status(proc_t *p)
{
    return p->win && IsHungAppWindow(p->win) ? L"Not responding" : L"";
}

static void fill_processes(void)
{
    grid_t *g = &g_grids[TAB_PROCESSES];
    int counts[GRP_COUNT] = { 0 }, i;
    double disk = 0;
    static const WCHAR *const group_names[GRP_COUNT] = { L"Apps", L"Background processes", L"System processes" };

    grid_begin(g);
    for (i = 0; i < g_nprocs; i++)
    {
        proc_t *p = &g_procs[i];
        grow_t *r = grid_add(g);
        if (!r) break;
        counts[p->group]++;
        r->group = p->group;
        r->key = p->pid;
        r->icon = p->icon;
        lstrcpynW(r->text[0], p->desc, GRID_CELL);
        lstrcpynW(r->text[1], proc_status(p), GRID_CELL);
        _snwprintf(r->text[2], GRID_CELL, L"%.1f%%", p->cpu);
        fmt_mem(p->ws, r->text[3], GRID_CELL);
        _snwprintf(r->text[4], GRID_CELL, L"%.1f MB/s", p->disk);
        r->num[2] = p->cpu; r->num[3] = (double)p->ws; r->num[4] = p->disk;
        disk += p->disk;
    }
    for (i = 0; i < GRP_COUNT; i++)
    {
        grow_t *r;
        if (!counts[i] || !(r = grid_add(g))) continue;
        r->group = i; r->header = TRUE;
        _snwprintf(r->text[0], GRID_CELL, L"%s (%d)", group_names[i], counts[i]);
    }
    _snwprintf(g->cols[2].total, 24, L"%.0f%%", g_perf.cpu);
    _snwprintf(g->cols[3].total, 24, L"%.0f%%", g_perf.mem_total ? 100.0 * (g_perf.mem_total - g_perf.mem_avail) / g_perf.mem_total : 0);
    _snwprintf(g->cols[4].total, 24, L"%.1f MB/s", disk);
    g->heatmax[2] = 100; g->heatmax[3] = (double)g_perf.mem_total / 8; g->heatmax[4] = 50;
    grid_end(g);
}

static void fill_details(void)
{
    grid_t *g = &g_grids[TAB_DETAILS];
    int i;
    grid_begin(g);
    for (i = 0; i < g_nprocs; i++)
    {
        proc_t *p = &g_procs[i];
        grow_t *r = grid_add(g);
        WCHAR mem[32];
        if (!r) break;
        r->key = p->pid;
        r->icon = p->icon;
        lstrcpynW(r->text[0], p->name, GRID_CELL);
        _snwprintf(r->text[1], GRID_CELL, L"%lu", p->pid);
        lstrcpynW(r->text[2], proc_status(p)[0] ? proc_status(p) : L"Running", GRID_CELL);
        lstrcpynW(r->text[3], p->user, GRID_CELL);
        _snwprintf(r->text[4], GRID_CELL, L"%02.0f", p->cpu);
        _snwprintf(mem, 32, L"%lu K", (unsigned long)(p->ws / 1024));
        lstrcpynW(r->text[5], mem, GRID_CELL);
        lstrcpynW(r->text[6], p->arch, GRID_CELL);
        lstrcpynW(r->text[7], p->desc, GRID_CELL);
        r->num[1] = p->pid; r->num[4] = p->cpu; r->num[5] = (double)p->ws;
    }
    grid_end(g);
}

static void fill_users(void)
{
    grid_t *g = &g_grids[TAB_USERS];
    int i, j;
    grid_begin(g);
    for (i = 0; i < g_nprocs; i++)
    {
        const WCHAR *u = g_procs[i].user[0] ? g_procs[i].user : L"SYSTEM";
        grow_t *r = NULL;
        for (j = 0; j < g->nrows; j++) if (!lstrcmpiW(g->rows[j].skey, u)) { r = &g->rows[j]; break; }
        if (!r)
        {
            if (!(r = grid_add(g))) break;
            lstrcpynW(r->skey, u, 64);
            _snwprintf(r->text[0], GRID_CELL, L"%s (%d)", u, 0);
        }
        r->num[1]++;
        r->num[2] += g_procs[i].cpu;
        r->num[3] += (double)g_procs[i].ws;
    }
    for (j = 0; j < g->nrows; j++)
    {
        grow_t *r = &g->rows[j];
        _snwprintf(r->text[0], GRID_CELL, L"%s (%d)", r->skey, (int)r->num[1]);
        _snwprintf(r->text[2], GRID_CELL, L"%.1f%%", r->num[2]);
        fmt_mem((ULONGLONG)r->num[3], r->text[3], GRID_CELL);
        r->icon = generic_icon();
    }
    _snwprintf(g->cols[2].total, 24, L"%.0f%%", g_perf.cpu);
    _snwprintf(g->cols[3].total, 24, L"%.0f%%", g_perf.mem_total ? 100.0 * (g_perf.mem_total - g_perf.mem_avail) / g_perf.mem_total : 0);
    g->heatmax[2] = 100; g->heatmax[3] = (double)g_perf.mem_total / 4;
    grid_end(g);
}

static void fill_startup(void)
{
    grid_t *g = &g_grids[TAB_STARTUP];
    int i;
    read_startup();
    grid_begin(g);
    for (i = 0; i < g_nstartup; i++)
    {
        startup_t *s = &g_startup[i];
        grow_t *r = grid_add(g);
        if (!r) break;
        _snwprintf(r->skey, 64, L"%d:%s", s->src, s->value);
        r->skey[63] = 0;
        r->icon = load_small_icon(s->path[0] && GetFileAttributesW(s->path) != INVALID_FILE_ATTRIBUTES ? s->path : NULL);
        lstrcpynW(r->text[0], s->name, GRID_CELL);
        lstrcpynW(r->text[1], s->publisher, GRID_CELL);
        lstrcpynW(r->text[2], s->enabled ? L"Enabled" : L"Disabled", GRID_CELL);
        lstrcpynW(r->text[3], L"Not measured", GRID_CELL);
    }
    grid_end(g);
    /* the icons are made anew each time; the old ones went with the rows */
}

static const WCHAR *service_state(DWORD s)
{
    switch (s)
    {
    case SERVICE_RUNNING: return L"Running";
    case SERVICE_STOPPED: return L"Stopped";
    case SERVICE_START_PENDING: return L"Starting";
    case SERVICE_STOP_PENDING: return L"Stopping";
    case SERVICE_PAUSED: return L"Paused";
    default: return L"";
    }
}

static void fill_services(void)
{
    grid_t *g = &g_grids[TAB_SERVICES];
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    BYTE *buf = NULL;
    DWORD need = 0, n = 0, resume = 0, i;
    grid_begin(g);
    if (scm)
    {
        EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, NULL, 0, &need, &n, &resume, NULL);
        if (need && (buf = malloc(need)))
        {
            resume = 0;
            if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buf, need, &need, &n, &resume, NULL))
            {
                ENUM_SERVICE_STATUS_PROCESSW *s = (ENUM_SERVICE_STATUS_PROCESSW *)buf;
                for (i = 0; i < n; i++)
                {
                    grow_t *r = grid_add(g);
                    if (!r) break;
                    lstrcpynW(r->skey, s[i].lpServiceName, 64);
                    lstrcpynW(r->text[0], s[i].lpServiceName, GRID_CELL);
                    if (s[i].ServiceStatusProcess.dwProcessId)
                        _snwprintf(r->text[1], GRID_CELL, L"%lu", s[i].ServiceStatusProcess.dwProcessId);
                    r->num[1] = s[i].ServiceStatusProcess.dwProcessId;
                    lstrcpynW(r->text[2], s[i].lpDisplayName ? s[i].lpDisplayName : L"", GRID_CELL);
                    lstrcpynW(r->text[3], service_state(s[i].ServiceStatusProcess.dwCurrentState), GRID_CELL);
                }
            }
            free(buf);
        }
        CloseServiceHandle(scm);
    }
    grid_end(g);
}

static void fill_fewer(void)
{
    int i;
    grid_begin(&g_fewer);
    for (i = 0; i < g_nprocs; i++)
    {
        proc_t *p = &g_procs[i];
        grow_t *r;
        if (p->group != GRP_APPS || !(r = grid_add(&g_fewer))) continue;
        r->key = p->pid;
        r->icon = p->icon;
        if (proc_status(p)[0]) _snwprintf(r->text[0], GRID_CELL, L"%s (%s)", p->desc, proc_status(p));
        else lstrcpynW(r->text[0], p->desc, GRID_CELL);
    }
    grid_end(&g_fewer);
}

/* ---- the dump, for the gate ---------------------------------------------- */

static void dump_line(FILE *f, const WCHAR *fmt, ...)
{
    WCHAR w[1024];
    char u[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(w, 1024, fmt, ap);
    va_end(ap);
    w[1023] = 0;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), NULL, NULL);
    fputs(u, f);
    fputc('\n', f);
}

static void center(HWND w, const RECT *r, POINT *p)
{
    p->x = (r->left + r->right) / 2; p->y = (r->top + r->bottom) / 2;
    ClientToScreen(w, p);
}

static void dump(void)
{
    WCHAR tmp[MAX_PATH + 8];
    FILE *f;
    RECT wr;
    POINT pt;
    int i, c;
    grid_t *g = g_more ? (g_tab == TAB_PERFORMANCE ? NULL : &g_grids[g_tab]) : &g_fewer;

    if (!g_dump_path[0]) return;
    _snwprintf(tmp, ARRAYSIZE(tmp), L"%s.tmp", g_dump_path);
    if (!(f = _wfopen(tmp, L"wb"))) return;
    GetWindowRect(g_main, &wr);
    dump_line(f, L"WINDOW %ld %ld %ld %ld", wr.left, wr.top, wr.right, wr.bottom);
    dump_line(f, L"MODE %s", g_more ? L"more" : L"fewer");
    dump_line(f, L"TAB %d %s", g_tab, tab_names[g_tab]);
    dump_line(f, L"SPEED %d", g_speed - IDM_SPEED_HIGH);
    if (g_more)
        for (i = 0; i < TAB_COUNT; i++) { center(g_main, &g_tab_rc[i], &pt); dump_line(f, L"TABRECT %d %s %ld %ld", i, tab_names[i], pt.x, pt.y); }
    center(g_main, &g_link_rc, &pt);
    dump_line(f, L"LINK %ld %ld", pt.x, pt.y);
    if (!IsRectEmpty(&g_button_rc)) { center(g_main, &g_button_rc, &pt); dump_line(f, L"BUTTON %ld %ld", pt.x, pt.y); }
    for (i = 0; i < g_nprocs; i++)
    {
        proc_t *p = &g_procs[i];
        dump_line(f, L"PROC %lu\t%d\t%s\t%s\t%.2f\t%lu\t%s\t%s", p->pid, p->group, p->name, p->desc, p->cpu,
                  (unsigned long)(p->ws / 1024), p->user, p->arch);
    }
    dump_line(f, L"PERF CPU %.1f PROCS %lu THREADS %lu HANDLES %lu MEMTOTAL %llu MEMAVAIL %llu COMMIT %llu LIMIT %llu UPTIME %llu NCPU %lu MHZ %lu",
              g_perf.cpu, g_perf.procs, g_perf.threads, g_perf.handles, g_perf.mem_total, g_perf.mem_avail,
              g_perf.commit, g_perf.commit_limit, g_perf.uptime_ms, g_perf.ncpu, g_perf.mhz);
    dump_line(f, L"PERFSEL %d", g_perf_sel);
    if (g_perf.net) dump_line(f, L"NET %s\t%s\t%.1f\t%.1f", g_perf.net_kind, g_perf.net_name, g_perf.send_kbps, g_perf.recv_kbps);
    for (i = 0; i < g_nstartup; i++)
        dump_line(f, L"STARTUP %d:%s\t%s\t%s\t%s", g_startup[i].src, g_startup[i].value, g_startup[i].name,
                  g_startup[i].enabled ? L"Enabled" : L"Disabled", g_startup[i].publisher);
    for (i = 0; i < g_grids[TAB_USERS].nrows; i++)
        dump_line(f, L"USER %s\t%s\t%s", g_grids[TAB_USERS].rows[i].skey, g_grids[TAB_USERS].rows[i].text[2], g_grids[TAB_USERS].rows[i].text[3]);
    for (i = 0; i < g_grids[TAB_SERVICES].nrows; i++)
        dump_line(f, L"SERVICE %s\t%s\t%s\t%s", g_grids[TAB_SERVICES].rows[i].text[0], g_grids[TAB_SERVICES].rows[i].text[2],
                  g_grids[TAB_SERVICES].rows[i].text[3], g_grids[TAB_SERVICES].rows[i].text[1]);
    for (i = 0; i < g_fewer.nrows; i++) dump_line(f, L"FEWER %lu\t%s", (unsigned long)g_fewer.rows[i].key, g_fewer.rows[i].text[0]);
    if (g)
    {
        WCHAR line[900];
        for (i = 0; i < g->ncol; i++) dump_line(f, L"COLUMN %d %s\t%s", i, g->cols[i].name, g->cols[i].total);
        for (i = g->top; i < g->nrows; i++)
        {
            RECT r;
            if (!grid_row_rect(g, i, &r)) break;
            center(g->hwnd, &r, &pt);
            line[0] = 0;
            for (c = 0; c < g->ncol; c++)
            {
                if (c) wcscat(line, L"\t");
                wcsncat(line, g->rows[i].text[c], 100);
            }
            dump_line(f, L"ROW %d %ld %ld %d %s", i, pt.x, pt.y, g->rows[i].header, line);
        }
        if (grid_selected(g)) dump_line(f, L"SEL %s", grid_selected(g)->text[0]);
    }
    dump_line(f, L"ACTION %s", g_last_action);
    dump_line(f, L"END");
    fclose(f);
    MoveFileExW(tmp, g_dump_path, MOVEFILE_REPLACE_EXISTING);
}

/* ---- refresh ------------------------------------------------------------- */

static void refresh(void)
{
    sample();
    fill_processes();
    fill_details();
    fill_users();
    fill_startup();
    fill_services();
    fill_fewer();
    if (g_perf_wnd) InvalidateRect(g_perf_wnd, NULL, FALSE);
    InvalidateRect(g_main, &g_button_rc, FALSE);
    dump();
}

static void set_speed(int id)
{
    static const int ms[] = { 500, 1000, 4000, 0 };
    g_speed = id;
    KillTimer(g_main, 1);
    if (ms[id - IDM_SPEED_HIGH]) SetTimer(g_main, 1, ms[id - IDM_SPEED_HIGH], NULL);
    CheckMenuRadioItem(g_menu, IDM_SPEED_HIGH, IDM_SPEED_PAUSED, id, MF_BYCOMMAND);
    save_setting(L"UpdateSpeed", id - IDM_SPEED_HIGH);
}

/* ---- the Performance tab -------------------------------------------------- */

static void fmt_uptime(ULONGLONG ms, WCHAR *out, int cch)
{
    ULONGLONG s = ms / 1000;
    _snwprintf(out, cch, L"%llu:%02llu:%02llu:%02llu", s / 86400, s / 3600 % 24, s / 60 % 60, s % 60);
}

static void fmt_gb(ULONGLONG b, WCHAR *out, int cch) { _snwprintf(out, cch, L"%.1f GB", b / 1073741824.0); }

static void fmt_rate(double kbps, WCHAR *out, int cch)
{
    if (kbps >= 1000) _snwprintf(out, cch, L"%.1f Mbps", kbps / 1000);
    else _snwprintf(out, cch, L"%.0f Kbps", kbps);
}

static void graph(HDC dc, RECT r, const double *hist, double max, BOOL big)
{
    HPEN grid_pen = CreatePen(PS_SOLID, 1, sgm_dark ? RGB(70, 52, 100) : RGB(226, 214, 244)), line_pen = CreatePen(PS_SOLID, big ? S(1) : 1, C_ACCENT),
         border = CreatePen(PS_SOLID, 1, C_ACCENT);
    HBRUSH shade = CreateSolidBrush(sgm_dark ? RGB(52, 40, 72) : RGB(243, 237, 251));
    POINT pts[62];
    int i, w = r.right - r.left, h = r.bottom - r.top;
    HGDIOBJ op, ob;
    fill(dc, &r, C_BG);
    op = SelectObject(dc, grid_pen);
    if (big)
    {
        for (i = 1; i < 10; i++) { MoveToEx(dc, r.left, r.top + h * i / 10, NULL); LineTo(dc, r.right, r.top + h * i / 10); }
        for (i = 1; i < 20; i++) { MoveToEx(dc, r.left + w * i / 20, r.top, NULL); LineTo(dc, r.left + w * i / 20, r.bottom); }
    }
    for (i = 0; i < 60; i++)
    {
        double v = max > 0 ? hist[i] / max : 0;
        if (v > 1) v = 1;
        pts[i].x = r.left + w * i / 59;
        pts[i].y = r.bottom - 1 - (int)(v * (h - 2));
    }
    pts[60].x = r.right - 1; pts[60].y = r.bottom - 1;
    pts[61].x = r.left; pts[61].y = r.bottom - 1;
    SelectObject(dc, GetStockObject(NULL_PEN));
    ob = SelectObject(dc, shade);
    Polygon(dc, pts, 62);
    SelectObject(dc, line_pen);
    Polyline(dc, pts, 60);
    SelectObject(dc, border);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, r.left, r.top, r.right, r.bottom);
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(grid_pen); DeleteObject(line_pen); DeleteObject(border); DeleteObject(shade);
}

static void text_at(HDC dc, HFONT f, COLORREF c, int x, int y, int w, UINT al, const WCHAR *s)
{
    RECT r = { x, y, x + w, y + S(40) };
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s, -1, &r, al | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

static int perf_items(void) { return g_perf.net ? 3 : 2; }

static void stat(HDC dc, int x, int y, const WCHAR *label, const WCHAR *value)
{
    text_at(dc, g_font_small, C_SUBTLE, x, y, S(160), DT_LEFT, label);
    text_at(dc, g_font_value, C_TEXT, x, y + S(16), S(160), DT_LEFT, value);
}

static void perf_paint(HDC dc, RECT *rc)
{
    int left = S(220), i, item_h = S(64), gx, gy, gw, gh;
    WCHAR a[64], b[64], c[64];
    double mem_pct = g_perf.mem_total ? 100.0 * (g_perf.mem_total - g_perf.mem_avail) / g_perf.mem_total : 0;
    RECT r;

    fill(dc, rc, C_BG);
    SetBkMode(dc, TRANSPARENT);
    /* the list on the left */
    for (i = 0; i < perf_items(); i++)
    {
        const double *hist = i == 0 ? g_perf.cpu_hist : i == 1 ? g_perf.mem_hist : g_perf.net_hist;
        double max = i == 2 ? g_perf.net_max : 100;
        RECT item = { 0, S(8) + i * item_h, left - S(8), S(8) + (i + 1) * item_h }, spark;
        if (i == g_perf_sel) fill(dc, &item, C_ACCENT_LT);
        SetRect(&spark, S(10), item.top + S(8), S(76), item.bottom - S(8));
        graph(dc, spark, hist, max, FALSE);
        if (i == 0) { lstrcpyW(a, L"CPU"); _snwprintf(b, 64, L"%.0f%%  %.2f GHz", g_perf.cpu, g_perf.mhz / 1000.0); }
        else if (i == 1)
        {
            lstrcpyW(a, L"Memory");
            _snwprintf(b, 64, L"%.1f/%.1f GB (%.0f%%)", (g_perf.mem_total - g_perf.mem_avail) / 1073741824.0,
                       g_perf.mem_total / 1073741824.0, mem_pct);
        }
        else
        {
            WCHAR s[24], rr[24];
            lstrcpyW(a, g_perf.net_kind);
            fmt_rate(g_perf.send_kbps, s, 24); fmt_rate(g_perf.recv_kbps, rr, 24);
            _snwprintf(b, 64, L"S: %s  R: %s", s, rr);
        }
        text_at(dc, g_font_head, C_TEXT, S(86), item.top + S(12), left - S(96), DT_LEFT, a);
        text_at(dc, g_font_small, C_SUBTLE, S(86), item.top + S(34), left - S(96), DT_LEFT, b);
    }
    r = *rc; r.left = left - 1; r.right = left;
    fill(dc, &r, C_LINE);

    /* the selected one, large */
    gx = left + S(20); gw = rc->right - gx - S(20);
    gy = S(64); gh = max(S(80), rc->bottom - gy - S(170));
    if (g_perf_sel == 0)
    {
        text_at(dc, g_font_big, C_TEXT, gx, S(8), gw, DT_LEFT, L"CPU");
        text_at(dc, g_font, C_TEXT, gx, S(16), gw, DT_RIGHT, g_perf.cpu_name);
        text_at(dc, g_font_small, C_SUBTLE, gx, gy - S(18), gw, DT_LEFT, L"% Utilization");
        text_at(dc, g_font_small, C_SUBTLE, gx, gy - S(18), gw, DT_RIGHT, L"100%");
    }
    else if (g_perf_sel == 1)
    {
        fmt_gb(g_perf.mem_total, a, 64);
        text_at(dc, g_font_big, C_TEXT, gx, S(8), gw, DT_LEFT, L"Memory");
        text_at(dc, g_font, C_TEXT, gx, S(16), gw, DT_RIGHT, a);
        text_at(dc, g_font_small, C_SUBTLE, gx, gy - S(18), gw, DT_LEFT, L"Memory usage");
        text_at(dc, g_font_small, C_SUBTLE, gx, gy - S(18), gw, DT_RIGHT, a);
    }
    else
    {
        fmt_rate(g_perf.net_max, a, 64);
        text_at(dc, g_font_big, C_TEXT, gx, S(8), gw, DT_LEFT, g_perf.net_kind);
        text_at(dc, g_font, C_TEXT, gx, S(16), gw, DT_RIGHT, g_perf.net_name);
        text_at(dc, g_font_small, C_SUBTLE, gx, gy - S(18), gw, DT_LEFT, L"Throughput");
        text_at(dc, g_font_small, C_SUBTLE, gx, gy - S(18), gw, DT_RIGHT, a);
    }
    SetRect(&r, gx, gy, gx + gw, gy + gh);
    graph(dc, r, g_perf_sel == 0 ? g_perf.cpu_hist : g_perf_sel == 1 ? g_perf.mem_hist : g_perf.net_hist,
          g_perf_sel == 2 ? g_perf.net_max : 100, TRUE);
    text_at(dc, g_font_small, C_SUBTLE, gx, gy + gh + S(2), gw, DT_LEFT, L"60 seconds");
    text_at(dc, g_font_small, C_SUBTLE, gx, gy + gh + S(2), gw, DT_RIGHT, L"0");

    gy += gh + S(28);
    if (g_perf_sel == 0)
    {
        _snwprintf(a, 64, L"%.0f%%", g_perf.cpu); stat(dc, gx, gy, L"Utilization", a);
        _snwprintf(a, 64, L"%.2f GHz", g_perf.mhz / 1000.0); stat(dc, gx + S(110), gy, L"Speed", a);
        _snwprintf(a, 64, L"%lu", g_perf.procs); stat(dc, gx, gy + S(48), L"Processes", a);
        _snwprintf(a, 64, L"%lu", g_perf.threads); stat(dc, gx + S(90), gy + S(48), L"Threads", a);
        _snwprintf(a, 64, L"%lu", g_perf.handles); stat(dc, gx + S(170), gy + S(48), L"Handles", a);
        fmt_uptime(g_perf.uptime_ms, a, 64); stat(dc, gx, gy + S(96), L"Up time", a);
        _snwprintf(a, 64, L"%.2f GHz", g_perf.mhz / 1000.0);
        _snwprintf(b, 64, L"%lu", g_perf.ncpu);
        text_at(dc, g_font_small, C_SUBTLE, gx + S(290), gy, S(120), DT_LEFT, L"Base speed:");
        text_at(dc, g_font_small, C_TEXT, gx + S(420), gy, S(120), DT_LEFT, a);
        text_at(dc, g_font_small, C_SUBTLE, gx + S(290), gy + S(20), S(120), DT_LEFT, L"Logical processors:");
        text_at(dc, g_font_small, C_TEXT, gx + S(420), gy + S(20), S(120), DT_LEFT, b);
    }
    else if (g_perf_sel == 1)
    {
        fmt_gb(g_perf.mem_total - g_perf.mem_avail, a, 64); stat(dc, gx, gy, L"In use", a);
        fmt_gb(g_perf.mem_avail, a, 64); stat(dc, gx + S(130), gy, L"Available", a);
        fmt_gb(g_perf.commit, b, 64); fmt_gb(g_perf.commit_limit, c, 64);
        _snwprintf(a, 64, L"%s/%s", b, c); a[lstrlenW(a)] = 0;
        stat(dc, gx, gy + S(48), L"Committed", a);
        fmt_gb(g_perf.mem_total, a, 64); stat(dc, gx + S(260), gy, L"Total", a);
    }
    else
    {
        fmt_rate(g_perf.send_kbps, a, 64); stat(dc, gx, gy, L"Send", a);
        fmt_rate(g_perf.recv_kbps, a, 64); stat(dc, gx + S(130), gy, L"Receive", a);
        text_at(dc, g_font_small, C_SUBTLE, gx + S(290), gy, S(120), DT_LEFT, L"Adapter name:");
        text_at(dc, g_font_small, C_TEXT, gx + S(400), gy, S(200), DT_LEFT, g_perf.net_name);
        text_at(dc, g_font_small, C_SUBTLE, gx + S(290), gy + S(20), S(120), DT_LEFT, L"Connection type:");
        text_at(dc, g_font_small, C_TEXT, gx + S(400), gy + S(20), S(200), DT_LEFT, g_perf.net_kind);
    }
}

static LRESULT CALLBACK perf_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps), mem = CreateCompatibleDC(dc);
        RECT rc;
        HBITMAP bmp;
        HGDIOBJ old;
        GetClientRect(hwnd, &rc);
        bmp = CreateCompatibleBitmap(dc, max(rc.right, 1), max(rc.bottom, 1));
        old = SelectObject(mem, bmp);
        perf_paint(mem, &rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        int x = (short)LOWORD(lp), y = (short)HIWORD(lp), i = (y - S(8)) / S(64);
        SetFocus(hwnd);
        if (x < S(212) && y >= S(8) && i < perf_items()) { g_perf_sel = i; InvalidateRect(hwnd, NULL, FALSE); dump(); }
        return 0;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_KEYDOWN:
        if (wp == VK_UP && g_perf_sel > 0) g_perf_sel--;
        else if (wp == VK_DOWN && g_perf_sel < perf_items() - 1) g_perf_sel++;
        else break;
        InvalidateRect(hwnd, NULL, FALSE);
        dump();
        return 0;
    case WM_SIZE: InvalidateRect(hwnd, NULL, FALSE); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---- Run new task ------------------------------------------------------- */

struct dlg { WORD buf[4096]; WORD *p; DLGTEMPLATE *t; };

static void dlg_str(struct dlg *d, const WCHAR *s) { while (*s) *d->p++ = *s++; *d->p++ = 0; }

static void dlg_item(struct dlg *d, WORD atom, const WCHAR *text, WORD id, DWORD style, short x, short y, short cx, short cy)
{
    DLGITEMTEMPLATE *it;
    d->p = (WORD *)(((ULONG_PTR)d->p + 3) & ~(ULONG_PTR)3);
    it = (DLGITEMTEMPLATE *)d->p;
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    d->p = (WORD *)(it + 1);
    *d->p++ = 0xFFFF; *d->p++ = atom;
    dlg_str(d, text);
    *d->p++ = 0;
    d->t->cdit++;
}

enum { ID_RUN_EDIT = 201, ID_RUN_ADMIN, ID_RUN_BROWSE };

static BOOL run_task(HWND owner, const WCHAR *text, BOOL admin)
{
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    WCHAR file[MAX_PATH], msg[MAX_PATH + 128];
    const WCHAR *args = NULL, *e;
    while (*text == ' ') text++;
    if (*text == '"' && (e = wcschr(text + 1, '"')))
    {
        lstrcpynW(file, text + 1, (int)min(MAX_PATH, e - text));
        args = e + 1;
    }
    else if (GetFileAttributesW(text) != INVALID_FILE_ATTRIBUTES || !(e = wcschr(text, ' ')))
        lstrcpynW(file, text, MAX_PATH);
    else
    {
        lstrcpynW(file, text, (int)min(MAX_PATH, e - text + 1));
        args = e + 1;
    }
    while (args && *args == ' ') args++;
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpVerb = admin ? L"runas" : NULL;
    sei.lpFile = file;
    sei.lpParameters = args && *args ? args : NULL;
    sei.nShow = SW_SHOWNORMAL;
    _snwprintf(g_last_action, 128, L"run %s%s", admin ? L"admin " : L"", file);
    if (ShellExecuteExW(&sei)) return TRUE;
    _snwprintf(msg, ARRAYSIZE(msg), L"Stained Glass cannot find '%s'. Make sure you typed the name correctly, and then try again.", file);
    MessageBoxW(owner, msg, L"Create new task", MB_ICONERROR | MB_OK);
    return FALSE;
}

static INT_PTR CALLBACK run_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)lp;
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        HWND ok = GetDlgItem(dlg, IDOK);
        EnableWindow(ok, FALSE);
        SetFocus(GetDlgItem(dlg, ID_RUN_EDIT));
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_RUN_EDIT:
            if (HIWORD(wp) == EN_CHANGE) EnableWindow(GetDlgItem(dlg, IDOK), GetWindowTextLengthW(GetDlgItem(dlg, ID_RUN_EDIT)) > 0);
            return TRUE;
        case ID_RUN_BROWSE:
        {
            WCHAR file[MAX_PATH] = L"";
            OPENFILENAMEW ofn = { sizeof(ofn) };
            ofn.hwndOwner = dlg;
            ofn.lpstrFilter = L"Programs\0*.exe;*.pif;*.com;*.bat;*.cmd\0All files\0*.*\0";
            ofn.lpstrFile = file; ofn.nMaxFile = MAX_PATH;
            ofn.lpstrTitle = L"Browse";
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
            if (GetOpenFileNameW(&ofn))
            {
                WCHAR q[MAX_PATH + 2];
                _snwprintf(q, ARRAYSIZE(q), wcschr(file, ' ') ? L"\"%s\"" : L"%s", file);
                SetDlgItemTextW(dlg, ID_RUN_EDIT, q);
            }
            return TRUE;
        }
        case IDOK:
        {
            WCHAR text[1024];
            GetDlgItemTextW(dlg, ID_RUN_EDIT, text, 1024);
            if (text[0] && run_task(dlg, text, IsDlgButtonChecked(dlg, ID_RUN_ADMIN) == BST_CHECKED)) EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static void run_dialog(HWND owner)
{
    struct dlg d;
    memset(&d, 0, sizeof(d));
    d.t = (DLGTEMPLATE *)d.buf;
    d.t->style = DS_SHELLFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d.t->cx = 250; d.t->cy = 112;
    d.p = (WORD *)(d.t + 1);
    *d.p++ = 0; *d.p++ = 0;
    dlg_str(&d, L"Create new task");
    *d.p++ = 9;
    dlg_str(&d, L"Segoe UI");
    dlg_item(&d, 0x82, L"Type the name of a program, folder, document, or Internet resource, and Stained Glass will open it for you.",
             0xFFFF, SS_LEFT, 10, 10, 230, 20);
    dlg_item(&d, 0x82, L"&Open:", 0xFFFF, SS_LEFT, 10, 38, 26, 10);
    dlg_item(&d, 0x81, L"", ID_RUN_EDIT, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 40, 36, 200, 13);
    dlg_item(&d, 0x80, L"Create this task with administrative privileges.", ID_RUN_ADMIN,
             BS_AUTOCHECKBOX | WS_TABSTOP, 40, 56, 200, 10);
    dlg_item(&d, 0x80, L"OK", IDOK, BS_DEFPUSHBUTTON | WS_TABSTOP, 80, 90, 50, 14);
    dlg_item(&d, 0x80, L"Cancel", IDCANCEL, BS_PUSHBUTTON | WS_TABSTOP, 135, 90, 50, 14);
    dlg_item(&d, 0x80, L"&Browse...", ID_RUN_BROWSE, BS_PUSHBUTTON | WS_TABSTOP, 190, 90, 50, 14);
    DialogBoxIndirectParamW(GetModuleHandleW(NULL), d.t, owner, run_proc, 0);
}

/* ---- actions ------------------------------------------------------------- */

static grid_t *current_grid(void)
{
    if (!g_more) return &g_fewer;
    return g_tab == TAB_PERFORMANCE ? NULL : &g_grids[g_tab];
}

static const WCHAR *action_label(void)
{
    startup_t *s;
    if (!g_more) return L"End task";
    switch (g_tab)
    {
    case TAB_PROCESSES: case TAB_DETAILS: return L"End task";
    case TAB_STARTUP: return (s = selected_startup()) && !s->enabled ? L"Enable" : L"Disable";
    default: return NULL;
    }
}

static BOOL action_enabled(void)
{
    grid_t *g = current_grid();
    return g && grid_selected(g) && action_label();
}

static void end_selected(BOOL tree)
{
    grid_t *g = current_grid();
    grow_t *r = g ? grid_selected(g) : NULL;
    DWORD pid;
    BOOL ok;
    if (!r || !r->key) return;
    pid = (DWORD)r->key;
    if (pid == GetCurrentProcessId()) { PostMessageW(g_main, WM_CLOSE, 0, 0); return; }
    ok = tree ? end_tree(pid) : end_process(pid, g == &g_fewer || g_tab == TAB_PROCESSES);
    _snwprintf(g_last_action, 128, L"end %lu %s", pid, ok ? L"ok" : L"failed");
    if (!ok)
    {
        WCHAR msg[256];
        _snwprintf(msg, 256, L"The operation could not be completed.\n\nAccess is denied.");
        MessageBoxW(g_main, msg, L"Task Manager", MB_ICONERROR | MB_OK);
    }
    Sleep(150);
    refresh();
}

static void toggle_startup(void)
{
    startup_t *s = selected_startup();
    if (!s) return;
    if (!set_approved(s->src, s->value, !s->enabled))
    {
        MessageBoxW(g_main, L"You need administrator rights to change this item.", L"Task Manager", MB_ICONWARNING | MB_OK);
        return;
    }
    _snwprintf(g_last_action, 128, L"%s %s", s->enabled ? L"disable" : L"enable", s->value);
    refresh();
}

static void do_action(void)
{
    if (!action_enabled()) return;
    if (g_more && g_tab == TAB_STARTUP) toggle_startup();
    else end_selected(FALSE);
}

static void service_control(int id)
{
    grow_t *r = grid_selected(&g_grids[TAB_SERVICES]);
    SC_HANDLE scm, svc;
    SERVICE_STATUS st;
    BOOL ok = FALSE;
    if (!r || !(scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT))) return;
    if ((svc = OpenServiceW(scm, r->skey, SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS)))
    {
        if (id == IDM_SVC_STOP || id == IDM_SVC_RESTART)
        {
            int i;
            ok = ControlService(svc, SERVICE_CONTROL_STOP, &st);
            for (i = 0; i < 30 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; i++) Sleep(100);
        }
        if (id == IDM_SVC_START || id == IDM_SVC_RESTART) ok = StartServiceW(svc, 0, NULL);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    _snwprintf(g_last_action, 128, L"service %s %d %s", r->skey, id - IDM_SVC_START, ok ? L"ok" : L"failed");
    if (!ok) MessageBoxW(g_main, L"The operation could not be completed.", L"Task Manager", MB_ICONERROR | MB_OK);
    refresh();
}

static void context_menu(grid_t *g)
{
    HMENU m = CreatePopupMenu();
    POINT pt;
    grow_t *r = grid_selected(g);
    proc_t *p = r && r->key ? find_proc((DWORD)r->key) : NULL;
    startup_t *s;
    int cmd;
    if (!r) return;
    if (g == &g_grids[TAB_SERVICES])
    {
        BOOL running = !lstrcmpW(r->text[3], L"Running");
        AppendMenuW(m, MF_STRING | (running ? MF_GRAYED : 0), IDM_SVC_START, L"&Start");
        AppendMenuW(m, MF_STRING | (running ? 0 : MF_GRAYED), IDM_SVC_STOP, L"S&top");
        AppendMenuW(m, MF_STRING | (running ? 0 : MF_GRAYED), IDM_SVC_RESTART, L"&Restart");
    }
    else if (g == &g_grids[TAB_STARTUP])
    {
        s = selected_startup();
        AppendMenuW(m, MF_STRING, IDM_STARTUP_TOGGLE, s && !s->enabled ? L"&Enable" : L"&Disable");
        AppendMenuW(m, MF_STRING | (s && s->path[0] ? 0 : MF_GRAYED), IDM_LOCATION, L"&Open file location");
    }
    else if (g == &g_grids[TAB_USERS]) { DestroyMenu(m); return; }
    else
    {
        AppendMenuW(m, MF_STRING, IDM_END, L"&End task");
        if (g == &g_grids[TAB_DETAILS]) AppendMenuW(m, MF_STRING, IDM_END_TREE, L"End process &tree");
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (p && p->path[0] ? 0 : MF_GRAYED), IDM_LOCATION, L"&Open file location");
        if (g == &g_grids[TAB_PROCESSES]) AppendMenuW(m, MF_STRING, IDM_DETAILS, L"&Go to details");
    }
    GetCursorPos(&pt);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, NULL);
    DestroyMenu(m);
    if (cmd) SendMessageW(g_main, WM_COMMAND, cmd, 0);
}

/* ---- the window ---------------------------------------------------------- */

static int tab_height(void) { return g_more ? S(34) : 0; }
static int bar_height(void) { return S(50); }

static void layout(void)
{
    RECT rc;
    int i, x = S(4), th = tab_height(), bh = bar_height();
    HDC dc = GetDC(g_main);
    HGDIOBJ of = SelectObject(dc, g_font);
    GetClientRect(g_main, &rc);
    for (i = 0; i < TAB_COUNT; i++)
    {
        SIZE sz;
        GetTextExtentPoint32W(dc, tab_names[i], lstrlenW(tab_names[i]), &sz);
        SetRect(&g_tab_rc[i], x, 0, x + sz.cx + S(20), th);
        x += sz.cx + S(20);
    }
    {
        const WCHAR *link = g_more ? L"Fewer details" : L"More details";
        SIZE sz;
        GetTextExtentPoint32W(dc, link, lstrlenW(link), &sz);
        SetRect(&g_link_rc, S(10), rc.bottom - bh + S(12), S(10) + S(26) + sz.cx, rc.bottom - S(12));
    }
    SelectObject(dc, of);
    ReleaseDC(g_main, dc);
    if (action_label()) SetRect(&g_button_rc, rc.right - S(110), rc.bottom - bh + S(10), rc.right - S(12), rc.bottom - S(10));
    else SetRectEmpty(&g_button_rc);

    for (i = 0; i < TAB_COUNT; i++)
        if (g_grids[i].hwnd)
        {
            BOOL show = g_more && g_tab == i;
            if (show) MoveWindow(g_grids[i].hwnd, 0, th, rc.right, rc.bottom - th - bh, TRUE);
            ShowWindow(g_grids[i].hwnd, show ? SW_SHOW : SW_HIDE);
        }
    if (g_more && g_tab == TAB_PERFORMANCE) MoveWindow(g_perf_wnd, 0, th, rc.right, rc.bottom - th - bh, TRUE);
    ShowWindow(g_perf_wnd, g_more && g_tab == TAB_PERFORMANCE ? SW_SHOW : SW_HIDE);
    if (!g_more) MoveWindow(g_fewer.hwnd, 0, 0, rc.right, rc.bottom - bh, TRUE);
    ShowWindow(g_fewer.hwnd, g_more ? SW_HIDE : SW_SHOW);
    InvalidateRect(g_main, NULL, TRUE);
}

static void select_tab(int t)
{
    g_tab = (t + TAB_COUNT) % TAB_COUNT;
    save_setting(L"Tab", g_tab);
    layout();
    if (g_more)
    {
        if (g_tab == TAB_PERFORMANCE) SetFocus(g_perf_wnd);
        else SetFocus(g_grids[g_tab].hwnd);
    }
    dump();
}

static void set_mode(BOOL more)
{
    RECT wr;
    int w = more ? S(760) : S(380), h = more ? S(680) : S(400);
    g_more = more;
    save_setting(L"Details", more);
    SetMenu(g_main, more ? g_menu : NULL);
    GetWindowRect(g_main, &wr);
    SetWindowPos(g_main, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
    layout();
    SetFocus(more ? (g_tab == TAB_PERFORMANCE ? g_perf_wnd : g_grids[g_tab].hwnd) : g_fewer.hwnd);
    dump();
}

static void paint_main(HDC dc, RECT *rc)
{
    RECT r;
    int i, bh = bar_height();
    SetBkMode(dc, TRANSPARENT);
    if (g_more)
    {
        r = *rc; r.bottom = tab_height();
        fill(dc, &r, C_BG);
        for (i = 0; i < TAB_COUNT; i++)
        {
            RECT t = g_tab_rc[i];
            if (i == g_hot_tab && i != g_tab) fill(dc, &t, C_HOVER);
            SelectObject(dc, g_font);
            SetTextColor(dc, i == g_tab ? C_TEXT : C_SUBTLE);
            DrawTextW(dc, tab_names[i], -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (i == g_tab) { t.top = t.bottom - S(3); t.left += S(6); t.right -= S(6); fill(dc, &t, C_ACCENT); }
        }
        r.top = r.bottom - 1;
        fill(dc, &r, C_LINE);
    }
    /* the bar along the bottom */
    r = *rc; r.top = rc->bottom - bh;
    fill(dc, &r, C_SURFACE);
    r.bottom = r.top + 1;
    fill(dc, &r, C_LINE);
    {
        /* the details link, with a chevron in a circle */
        RECT t = g_link_rc;
        int cx = t.left + S(9), cy = (t.top + t.bottom) / 2, rad = S(8), a = S(3);
        HPEN pen = CreatePen(PS_SOLID, max(1, S(1)), g_hot_link ? C_ACCENT : C_SUBTLE);
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
        Ellipse(dc, cx - rad, cy - rad, cx + rad + 1, cy + rad + 1);
        if (g_more) { MoveToEx(dc, cx - a, cy + a / 2 + 1, NULL); LineTo(dc, cx, cy - a / 2); LineTo(dc, cx + a + 1, cy + a / 2 + 2); }
        else { MoveToEx(dc, cx - a, cy - a / 2, NULL); LineTo(dc, cx, cy + a / 2 + 1); LineTo(dc, cx + a + 1, cy - a / 2 - 1); }
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen);
        t.left += S(24);
        SelectObject(dc, g_font);
        SetTextColor(dc, g_hot_link ? C_ACCENT : C_TEXT);
        DrawTextW(dc, g_more ? L"Fewer details" : L"More details", -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    if (!IsRectEmpty(&g_button_rc))
    {
        BOOL en = action_enabled();
        RECT b = g_button_rc;
        HPEN pen = CreatePen(PS_SOLID, 1, en ? (g_hot_button ? C_ACCENT : (sgm_dark ? RGB(90, 90, 90) : RGB(190, 190, 190))) : (sgm_dark ? RGB(60, 60, 60) : RGB(220, 220, 220)));
        HBRUSH br = CreateSolidBrush(en && g_hot_button ? C_HOVER : (sgm_dark ? RGB(51, 51, 51) : RGB(253, 253, 253)));
        HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, br);
        RoundRect(dc, b.left, b.top, b.right, b.bottom, S(4), S(4));
        SelectObject(dc, op); SelectObject(dc, ob); DeleteObject(pen); DeleteObject(br);
        SelectObject(dc, g_font);
        SetTextColor(dc, en ? C_TEXT : (sgm_dark ? RGB(110, 110, 110) : RGB(160, 160, 160)));
        DrawTextW(dc, action_label(), -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

static HMENU build_menu(void)
{
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), opt = CreatePopupMenu(), view = CreatePopupMenu(), speed = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_RUN, L"&Run new task");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(opt, MF_STRING, IDM_TOPMOST, L"&Always on top");
    AppendMenuW(speed, MF_STRING, IDM_SPEED_HIGH, L"&High");
    AppendMenuW(speed, MF_STRING, IDM_SPEED_NORMAL, L"&Normal");
    AppendMenuW(speed, MF_STRING, IDM_SPEED_LOW, L"&Low");
    AppendMenuW(speed, MF_STRING, IDM_SPEED_PAUSED, L"&Paused");
    AppendMenuW(view, MF_STRING, IDM_REFRESH, L"&Refresh now\tF5");
    AppendMenuW(view, MF_POPUP, (UINT_PTR)speed, L"&Update speed");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)opt, L"&Options");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");
    return bar;
}

static void set_topmost(BOOL on)
{
    g_topmost = on;
    CheckMenuItem(g_menu, IDM_TOPMOST, on ? MF_CHECKED : MF_UNCHECKED);
    SetWindowPos(g_main, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    save_setting(L"AlwaysOnTop", on);
}

static LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (sg_mode_changed(msg, lp)) sgm_follow(hwnd);
    switch (msg)
    {
    case WM_CREATE:
    {
        int i;
        g_main = hwnd;
        for (i = 0; i < TAB_COUNT; i++) if (i != TAB_PERFORMANCE) grid_create(&g_grids[i], hwnd, 300 + i);
        grid_create(&g_fewer, hwnd, 399);
        g_perf_wnd = CreateWindowExW(0, PERF_CLASS, L"", WS_CHILD | WS_TABSTOP, 0, 0, 10, 10, hwnd, NULL, GetModuleHandleW(NULL), NULL);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        paint_main(dc, &rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE: layout(); return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = S(320);
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = S(300);
        return 0;
    case WM_TIMER: refresh(); return 0;
    case WM_MOUSEMOVE:
    {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int hot = -1, i, link = PtInRect(&g_link_rc, pt), button = PtInRect(&g_button_rc, pt);
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        if (g_more) for (i = 0; i < TAB_COUNT; i++) if (PtInRect(&g_tab_rc[i], pt)) hot = i;
        if (hot != g_hot_tab || link != g_hot_link || button != g_hot_button)
        {
            g_hot_tab = hot; g_hot_link = link; g_hot_button = button;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        SetCursor(LoadCursorW(NULL, (LPCWSTR)(link ? IDC_HAND : IDC_ARROW)));
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hot_tab = -1; g_hot_link = g_hot_button = 0;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
    {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int i;
        if (PtInRect(&g_link_rc, pt)) { set_mode(!g_more); return 0; }
        if (PtInRect(&g_button_rc, pt)) { do_action(); return 0; }
        if (g_more) for (i = 0; i < TAB_COUNT; i++) if (PtInRect(&g_tab_rc[i], pt)) { select_tab(i); return 0; }
        return 0;
    }
    case WM_APP_GRID:
    {
        grid_t *g = (grid_t *)lp;
        switch (wp)
        {
        case GN_SELCHANGE: InvalidateRect(hwnd, &g_button_rc, FALSE); dump(); break;
        case GN_RCLICK: context_menu(g); break;
        case GN_DELETE:
            if (g == &g_grids[TAB_STARTUP]) toggle_startup();
            else if (g != &g_grids[TAB_SERVICES] && g != &g_grids[TAB_USERS]) end_selected(FALSE);
            break;
        case GN_DBLCLK:
            if (g == &g_fewer) { grow_t *r = grid_selected(g); proc_t *p = r ? find_proc((DWORD)r->key) : NULL;
                                 if (p && p->win) { if (IsIconic(p->win)) ShowWindow(p->win, SW_RESTORE); SetForegroundWindow(p->win); } }
            break;
        case GN_SORT: refresh(); break;
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDM_RUN: run_dialog(hwnd); return 0;
        case IDM_EXIT: DestroyWindow(hwnd); return 0;
        case IDM_TOPMOST: set_topmost(!g_topmost); return 0;
        case IDM_REFRESH: refresh(); return 0;
        case IDM_SPEED_HIGH: case IDM_SPEED_NORMAL: case IDM_SPEED_LOW: case IDM_SPEED_PAUSED: set_speed(LOWORD(wp)); return 0;
        case IDM_END: end_selected(FALSE); return 0;
        case IDM_END_TREE: end_selected(TRUE); return 0;
        case IDM_LOCATION:
        {
            grid_t *g = current_grid();
            grow_t *r = g ? grid_selected(g) : NULL;
            proc_t *p;
            startup_t *s;
            if (g == &g_grids[TAB_STARTUP]) { if ((s = selected_startup())) open_location(s->path); }
            else if (r && (p = find_proc((DWORD)r->key))) open_location(p->path);
            return 0;
        }
        case IDM_DETAILS:
        {
            grow_t *r = grid_selected(&g_grids[TAB_PROCESSES]);
            if (r) { g_grids[TAB_DETAILS].sel_key = r->key; g_grids[TAB_DETAILS].sel_skey[0] = 0; g_grids[TAB_DETAILS].sel = -1; }
            select_tab(TAB_DETAILS);
            refresh();
            return 0;
        }
        case IDM_SVC_START: case IDM_SVC_STOP: case IDM_SVC_RESTART: service_control(LOWORD(wp)); return 0;
        case IDM_STARTUP_TOGGLE: toggle_startup(); return 0;
        case IDM_TOGGLE_MODE: set_mode(!g_more); return 0;
        case IDM_NEXT_TAB: if (g_more) select_tab(g_tab + 1); return 0;
        case IDM_PREV_TAB: if (g_more) select_tab(g_tab - 1); return 0;
        case IDM_ACTION: do_action(); return 0;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSW wc = { 0 };
    HDC screen;
    MSG msg;
    HWND existing;
    HACCEL accel;
    ACCEL keys[] = {
        { FVIRTKEY | FALT, 'D', IDM_TOGGLE_MODE }, { FVIRTKEY | FALT, 'E', IDM_ACTION },
        { FVIRTKEY | FCONTROL, VK_TAB, IDM_NEXT_TAB }, { FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, IDM_PREV_TAB },
        { FVIRTKEY, VK_F5, IDM_REFRESH },
    };
    int start_tab;
    (void)prev;

    /* one Task Manager: a second brings the first forward */
    CreateMutexW(NULL, FALSE, L"Local\\StainedGlassTaskManager");
    if (GetLastError() == ERROR_ALREADY_EXISTS && (existing = FindWindowW(MAIN_CLASS, NULL)))
    {
        if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        return 0;
    }

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    SetProcessDPIAware();
    screen = GetDC(NULL);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY);
    ReleaseDC(NULL, screen);
    if (g_dpi < 96) g_dpi = 96;
    GetEnvironmentVariableW(L"SG_TASKMGR_DUMP", g_dump_path, MAX_PATH);

    g_font = make_font(90, FW_NORMAL);
    g_font_small = make_font(85, FW_NORMAL);
    g_font_bold = make_font(90, FW_SEMIBOLD);
    g_font_head = make_font(110, FW_NORMAL);
    g_font_value = make_font(140, FW_NORMAL);
    g_font_big = make_font(200, FW_NORMAL);

    grid_register(inst);
    wc.lpfnWndProc = perf_proc; wc.hInstance = inst; wc.lpszClassName = PERF_CLASS;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&wc);
    wc.lpfnWndProc = main_proc; wc.lpszClassName = MAIN_CLASS;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);

    setup_grids();
    g_menu = build_menu();
    g_more = setting(L"Details", 0) != 0;
    start_tab = setting(L"Tab", TAB_PROCESSES);
    if (start_tab < 0 || start_tab >= TAB_COUNT) start_tab = TAB_PROCESSES;
    /* taskmgr /0 /startup (what "Open Task Manager" in Settings runs) */
    if (cmd && wcsstr(cmd, L"/startup")) { g_more = TRUE; start_tab = TAB_STARTUP; }
    g_tab = start_tab;

    sgm_dark = sg_apps_dark();
    CreateWindowExW(0, MAIN_CLASS, L"Task Manager", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                    g_more ? S(760) : S(380), g_more ? S(680) : S(400), NULL, g_more ? g_menu : NULL, inst, NULL);
    if (!g_main) return 1;
    sg_mode_title(g_main, sgm_dark);
    {
        int sp = setting(L"UpdateSpeed", 1);
        set_speed(IDM_SPEED_HIGH + (sp >= 0 && sp <= 3 ? sp : 1));
    }
    if (setting(L"AlwaysOnTop", 0)) set_topmost(TRUE);
    refresh();
    layout();
    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    select_tab(g_tab);
    if (!g_more) SetFocus(g_fewer.hwnd);
    /* a second sample straight away, so CPU figures are not a second late */
    Sleep(250);
    refresh();

    accel = CreateAcceleratorTableW(keys, ARRAYSIZE(keys));
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        if (TranslateAcceleratorW(g_main, accel, &msg)) continue;
        if (IsDialogMessageW(g_main, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
