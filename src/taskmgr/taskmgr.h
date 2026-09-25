/* sg-taskmgr -- Task Manager, Windows 10-style: shared declarations.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_TASKMGR_H
#define SG_TASKMGR_H

#define WIN32_LEAN_AND_MEAN
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <winternl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <winsvc.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "../sg-mode.h"
extern BOOL sgm_dark;               /* the app mode (sg-mode.h), in main.c */
void sgm_follow(HWND hwnd);

/* ---- the palette (Stained Glass Light) ---------------------------------- */
#define C_BG (sgm_dark ? RGB(32,32,32) : RGB(255, 255, 255))
#define C_SURFACE (sgm_dark ? RGB(43,43,43) : RGB(243, 243, 243))
#define C_LINE (sgm_dark ? RGB(60,60,60) : RGB(222, 222, 222))
#define C_TEXT (sgm_dark ? RGB(255,255,255) : RGB(0, 0, 0))
#define C_SUBTLE (sgm_dark ? RGB(168,168,168) : RGB(96, 96, 96))
#define C_ACCENT    RGB(112, 48, 192)
#define C_ACCENT_LT (sgm_dark ? RGB(74,46,110) : RGB(231, 222, 246))   /* selection */
#define C_HOVER (sgm_dark ? RGB(52,44,64) : RGB(242, 237, 250))
#define C_GROUP (sgm_dark ? RGB(190,150,240) : RGB(80, 30, 150))

extern int g_dpi;
#define S(v) MulDiv((v), g_dpi, 96)
HFONT make_font(int pt10, int weight);
extern HFONT g_font, g_font_bold, g_font_small, g_font_big, g_font_head;

/* ---- sampled data ------------------------------------------------------- */
enum { GRP_APPS, GRP_BACKGROUND, GRP_WINDOWS, GRP_COUNT };

typedef struct proc
{
    DWORD pid, ppid, threads, handles, session;
    LONGLONG created;
    ULONGLONG cpu_time;         /* user + kernel, 100 ns */
    double cpu;                 /* percent of the machine, over the last interval */
    SIZE_T ws;                  /* working set, bytes */
    ULONGLONG io;               /* read + write bytes */
    double disk;                /* MB/s */
    int group;
    HWND win;                   /* its main window, for an app */
    WCHAR name[64];             /* image name, e.g. notepad.exe */
    WCHAR path[MAX_PATH];
    WCHAR desc[128];            /* FileDescription, else the name */
    WCHAR company[96];
    WCHAR user[64];
    WCHAR arch[8];
    WCHAR title[128];           /* its main window's title */
    HICON icon;
    BOOL seen;
} proc_t;

typedef struct perf
{
    double cpu;                 /* percent */
    ULONGLONG mem_total, mem_avail, commit, commit_limit;
    DWORD procs, threads, handles;
    ULONGLONG uptime_ms;
    DWORD mhz, ncpu;
    WCHAR cpu_name[96];
    double cpu_hist[60], mem_hist[60];
    /* the first connected network adapter */
    BOOL net;
    WCHAR net_name[64], net_kind[16];
    double send_kbps, recv_kbps, net_hist[60], net_max;
} perf_t;

extern proc_t *g_procs;
extern int g_nprocs;
extern perf_t g_perf;

void sample(void);
proc_t *find_proc(DWORD pid);
BOOL end_process(DWORD pid, BOOL gracefully);
BOOL end_tree(DWORD pid);
void open_location(const WCHAR *path);
HICON generic_icon(void);
HICON load_small_icon(const WCHAR *path);
void fmt_mem(ULONGLONG bytes, WCHAR *out, int cch);   /* "12.3 MB" */

/* ---- the list (our own control) ---------------------------------------- */
#define GRID_MAXCOL 8
#define GRID_CELL   128

typedef struct gcol
{
    const WCHAR *name;
    int width;                  /* at 96 dpi */
    BOOL right;                 /* numbers are right-aligned */
    BOOL heat;                  /* shaded by value */
    BOOL numeric;               /* sorts by num[] */
    WCHAR total[24];            /* the line above the name ("12%") */
} gcol_t;

typedef struct grow
{
    WCHAR text[GRID_MAXCOL][GRID_CELL];
    double num[GRID_MAXCOL];    /* sort key; heat is num/heatmax */
    int group;                  /* -1 when the grid has no groups */
    BOOL header;                /* a group header row ("Apps (3)") */
    HICON icon;
    UINT_PTR key;               /* stable identity across refreshes */
    WCHAR skey[64];             /* ...or a string one (services, startup) */
} grow_t;

typedef struct grid
{
    HWND hwnd;
    gcol_t cols[GRID_MAXCOL];
    int ncol;
    grow_t *rows;
    int nrows, cap;
    int top;                    /* first visible row */
    int sel;                    /* index into rows, -1 none */
    UINT_PTR sel_key;
    WCHAR sel_skey[64];
    int sortcol;
    BOOL sortdesc;
    BOOL noheader;
    double heatmax[GRID_MAXCOL];
    int hover, hover_head;
} grid_t;

/* notifications to the parent: WM_APP_GRID, wParam = code, lParam = grid_t* */
#define WM_APP_GRID (WM_APP + 1)
enum { GN_SELCHANGE = 1, GN_RCLICK, GN_DBLCLK, GN_DELETE, GN_SORT };

void grid_register(HINSTANCE inst);
HWND grid_create(grid_t *g, HWND parent, int id);
void grid_begin(grid_t *g);
grow_t *grid_add(grid_t *g);
void grid_end(grid_t *g);           /* sorts, keeps the selection, repaints */
grow_t *grid_selected(grid_t *g);
int grid_row_height(void);
int grid_header_height(grid_t *g);
BOOL grid_row_rect(grid_t *g, int i, RECT *out);   /* client rect, FALSE if not visible */

#endif
