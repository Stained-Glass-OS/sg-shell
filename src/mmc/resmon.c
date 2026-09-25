/* sg-mmc -- Resource Monitor (resmon.exe, as sg-resmon64.exe): Windows'
 * Resource Monitor -- tabs Overview, CPU, Memory, Disk and Network; on each,
 * sections of process lists on the left and graphs on the right -- over the
 * whole machine: every Linux process (Wine's programs among them, by their
 * .exe names), from sg-sysinfo processes and connections, sampled once a
 * second. CPU is a process's share of all processors over the last second,
 * Average CPU over the last minute; disk is bytes read and written per second
 * (what the kernel counted for the process, when it lets this user read it);
 * network is the TCP connections and listening ports each process holds.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <winsock2.h>
#include "mmc.h"
#include <iphlpapi.h>
#include <string.h>

#define MAXP 4096
#define HIST 60

typedef struct proc
{
    DWORD pid;
    WCHAR name[64];
    WCHAR user[32];
    DWORD threads;
    ULONGLONG ticks, rss, rbytes, wbytes;
    double cpu, avg, rps, wps;
    double cpu_hist[HIST];
    int conns, listens;
    BOOL seen, io;
} proc_t;

typedef struct conn { WCHAR proto[8], local[64], remote[64], state[16]; DWORD pid; BOOL have_pid; } conn_t;

static proc_t g_p[MAXP];
static int g_np;
static conn_t *g_c;
static int g_nc;
static DWORD g_tick_prev;
static double g_clk = 100, g_ncpu = 1;
static double g_cpu_hist[HIST], g_disk_hist[HIST], g_net_hist[HIST], g_mem_hist[HIST];
static double g_cpu_now, g_disk_now, g_net_now, g_mem_now;
static ULONGLONG g_idle_prev, g_kern_prev, g_user_prev, g_net_prev;
static int g_tab;
static HWND g_wnd, g_tabs, g_lists[4], g_heads[4], g_graphs;
static int g_nlists;
static WCHAR g_dump[MAX_PATH];
enum { T_OVERVIEW, T_CPU, T_MEMORY, T_DISK, T_NETWORK };

/* ---- sampling ------------------------------------------------------------------------------- */

static proc_t *find(DWORD pid)
{
    int i;
    for (i = 0; i < g_np; i++) if (g_p[i].pid == pid) return &g_p[i];
    return NULL;
}

static ULONGLONG ft64(FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

static void push(double *h, double v)
{
    memmove(h, h + 1, (HIST - 1) * sizeof(double));
    h[HIST - 1] = v;
}

static void sample(void)
{
    sys_reply_t r;
    DWORD now = GetTickCount();
    double dt = g_tick_prev ? (now - g_tick_prev) / 1000.0 : 0;
    int b, i;
    SYSTEM_INFO si;
    FILETIME idle, kern, user;
    MEMORYSTATUSEX ms = { sizeof(ms) };
    double disk = 0;

    GetSystemInfo(&si);
    g_ncpu = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
    for (i = 0; i < g_np; i++) g_p[i].seen = FALSE;
    sys_request(&r, "processes", NULL);
    for (b = sys_next_block(&r, 0, "PROCESS"); b >= 0; b = sys_next_block(&r, b + 1, "PROCESS"))
    {
        DWORD pid = strtoul(r.lines[b] + 8, NULL, 10);
        proc_t *p = find(pid);
        const char *v;
        ULONGLONG ticks, rb, wb;
        BOOL fresh = FALSE;
        if (!p)
        {
            if (g_np >= MAXP) continue;
            p = &g_p[g_np++];
            memset(p, 0, sizeof(*p));
            p->pid = pid;
            fresh = TRUE;
        }
        p->seen = TRUE;
        utf8_to_w(sys_field(&r, b, "NAME"), p->name, ARRAY_SIZE(p->name));
        utf8_to_w(sys_field(&r, b, "USER"), p->user, ARRAY_SIZE(p->user));
        if ((v = sys_field(&r, b, "CLOCK-TICKS"))) g_clk = atof(v) > 0 ? atof(v) : 100;
        p->threads = (v = sys_field(&r, b, "THREADS")) ? strtoul(v, NULL, 10) : 0;
        p->rss = (v = sys_field(&r, b, "RSS")) ? strtoull(v, NULL, 10) : 0;
        ticks = (v = sys_field(&r, b, "CPU-TICKS")) ? strtoull(v, NULL, 10) : 0;
        p->io = (v = sys_field(&r, b, "READ-BYTES")) != NULL;
        rb = v ? strtoull(v, NULL, 10) : 0;
        wb = (v = sys_field(&r, b, "WRITE-BYTES")) ? strtoull(v, NULL, 10) : 0;
        if (!fresh && dt > 0)
        {
            p->cpu = (ticks >= p->ticks ? ticks - p->ticks : 0) / g_clk / dt / g_ncpu * 100.0;
            if (p->cpu > 100) p->cpu = 100;
#ifdef SG_MUTANT_CPU
            p->cpu = 0;
#endif
            p->rps = rb >= p->rbytes ? (rb - p->rbytes) / dt : 0;
            p->wps = wb >= p->wbytes ? (wb - p->wbytes) / dt : 0;
        }
        p->ticks = ticks;
        p->rbytes = rb;
        p->wbytes = wb;
        push(p->cpu_hist, p->cpu);
        {
            double s = 0;
            int k;
            for (k = 0; k < HIST; k++) s += p->cpu_hist[k];
            p->avg = s / HIST;
        }
        disk += p->rps + p->wps;
        p->conns = p->listens = 0;
    }
    sys_free(&r);
    /* drop the processes that ended */
    for (i = 0; i < g_np; )
        if (!g_p[i].seen) g_p[i] = g_p[--g_np];
        else i++;

    free(g_c);
    g_c = NULL;
    g_nc = 0;
    sys_request(&r, "connections", NULL);
    g_c = calloc(r.nlines ? r.nlines : 1, sizeof(conn_t));
    for (b = 0; g_c && b < r.nlines; b++)
    {
        char *f[8], *line = r.lines[b], *s;
        int k = 0;
        conn_t *c;
        proc_t *p;
        if (strncmp(line, "CONN\t", 5)) continue;
        for (s = line; k < 8; )
        {
            f[k++] = s;
            if (!(s = strchr(s, '\t'))) break;
            *s++ = 0;
        }
        if (k < 7) continue;
        c = &g_c[g_nc++];
        utf8_to_w(f[1], c->proto, 8);
        utf8_to_w(f[2], c->local, 64);
        utf8_to_w(f[3], c->remote, 64);
        utf8_to_w(f[4], c->state, 16);
        c->have_pid = strcmp(f[5], "-") != 0;
        c->pid = c->have_pid ? strtoul(f[5], NULL, 10) : 0;
        if (c->have_pid && (p = find(c->pid)))
        {
            if (!wcscmp(c->state, L"LISTEN")) p->listens++;
            else p->conns++;
        }
    }
    sys_free(&r);

    /* the machine */
    if (GetSystemTimes(&idle, &kern, &user))
    {
        ULONGLONG i2 = ft64(idle), k2 = ft64(kern), u2 = ft64(user);
        if (g_kern_prev)
        {
            ULONGLONG total = (k2 - g_kern_prev) + (u2 - g_user_prev), id = i2 - g_idle_prev;
            g_cpu_now = total ? (double)(total - id) * 100.0 / total : 0;
        }
        g_idle_prev = i2; g_kern_prev = k2; g_user_prev = u2;
    }
    GlobalMemoryStatusEx(&ms);
    g_mem_now = ms.dwMemoryLoad;
    g_disk_now = disk;
    {
        /* network: the bytes through the machine's adapters, not loopback */
        static BYTE buf[128 * 1024];
        ULONG size = sizeof(buf);
        MIB_IFTABLE *t = (MIB_IFTABLE *)buf;
        if (!GetIfTable(t, &size, FALSE))
        {
            ULONGLONG total = 0;
            DWORD j;
            for (j = 0; j < t->dwNumEntries; j++)
                if (t->table[j].dwType != IF_TYPE_SOFTWARE_LOOPBACK)
                    total += (ULONGLONG)t->table[j].dwInOctets + t->table[j].dwOutOctets;
            if (g_net_prev && dt > 0 && total >= g_net_prev) g_net_now = (total - g_net_prev) * 8.0 / dt;
            g_net_prev = total;
        }
    }
    push(g_cpu_hist, g_cpu_now);
    push(g_disk_hist, g_disk_now);
    push(g_net_hist, g_net_now);
    push(g_mem_hist, g_mem_now);
    g_tick_prev = now;
}

/* ---- the lists ------------------------------------------------------------------------------ */

typedef struct col { const WCHAR *name; int width; BOOL right; } col_t;

static void set_cols(HWND lv, const col_t *cols, int n)
{
    int i;
    while (ListView_DeleteColumn(lv, 0)) ;
    for (i = 0; i < n; i++)
    {
        LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH | LVCF_FMT };
        c.pszText = (WCHAR *)cols[i].name;
        c.cx = S(cols[i].width);
        c.fmt = cols[i].right ? LVCFMT_RIGHT : LVCFMT_LEFT;
        SendMessageW(lv, LVM_INSERTCOLUMNW, i, (LPARAM)&c);
    }
}

static void row(HWND lv, int i, LPARAM key, const WCHAR *const *cells, int n)
{
    LVITEMW it = { LVIF_TEXT | LVIF_PARAM };
    int k, cnt = ListView_GetItemCount(lv);
    if (i < cnt)
    {
        it.mask = LVIF_PARAM;
        it.iItem = i;
        it.lParam = key;
        SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&it);
        for (k = 0; k < n; k++)
        {
            LVITEMW s = { 0 };
            WCHAR old[256];
            ListView_GetItemText(lv, i, k, old, 256);
            if (!wcscmp(old, cells[k])) continue;
            s.iSubItem = k;
            s.pszText = (WCHAR *)cells[k];
            SendMessageW(lv, LVM_SETITEMTEXTW, i, (LPARAM)&s);
        }
        return;
    }
    it.iItem = i;
    it.pszText = (WCHAR *)cells[0];
    it.lParam = key;
    SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
    for (k = 1; k < n; k++)
    {
        LVITEMW s = { 0 };
        s.iSubItem = k;
        s.pszText = (WCHAR *)cells[k];
        SendMessageW(lv, LVM_SETITEMTEXTW, i, (LPARAM)&s);
    }
}

static void trim(HWND lv, int n) { while (ListView_GetItemCount(lv) > n) ListView_DeleteItem(lv, n); }

static int by_cpu(const void *a, const void *b)
{
    const proc_t *x = *(proc_t *const *)a, *y = *(proc_t *const *)b;
    return x->cpu < y->cpu ? 1 : x->cpu > y->cpu ? -1 : x->avg < y->avg ? 1 : x->avg > y->avg ? -1 : 0;
}
static int by_mem(const void *a, const void *b)
{
    const proc_t *x = *(proc_t *const *)a, *y = *(proc_t *const *)b;
    return x->rss < y->rss ? 1 : x->rss > y->rss ? -1 : 0;
}
static int by_disk(const void *a, const void *b)
{
    const proc_t *x = *(proc_t *const *)a, *y = *(proc_t *const *)b;
    double dx = x->rps + x->wps, dy = y->rps + y->wps;
    return dx < dy ? 1 : dx > dy ? -1 : 0;
}
static int by_net(const void *a, const void *b)
{
    const proc_t *x = *(proc_t *const *)a, *y = *(proc_t *const *)b;
    int nx = x->conns + x->listens, ny = y->conns + y->listens;
    return nx < ny ? 1 : nx > ny ? -1 : 0;
}

static void fmt_rate(double bps, WCHAR *out, int cch)
{
    _snwprintf(out, cch, L"%.0f", bps);
}

static void fill_cpu(HWND lv)
{
    static proc_t *order[MAXP];
    int i, n = 0;
    for (i = 0; i < g_np; i++) order[n++] = &g_p[i];
    qsort(order, n, sizeof(order[0]), by_cpu);
    for (i = 0; i < n; i++)
    {
        WCHAR pid[16], thr[16], cpu[16], avg[16];
        const WCHAR *c[6] = { order[i]->name, pid, order[i]->user, thr, cpu, avg };
        _snwprintf(pid, 16, L"%lu", order[i]->pid);
        _snwprintf(thr, 16, L"%lu", order[i]->threads);
        _snwprintf(cpu, 16, L"%.0f", order[i]->cpu);
        _snwprintf(avg, 16, L"%.2f", order[i]->avg);
        row(lv, i, order[i]->pid, c, 6);
    }
    trim(lv, n);
}

static void fill_mem(HWND lv)
{
    static proc_t *order[MAXP];
    int i, n = 0;
    for (i = 0; i < g_np; i++) if (g_p[i].rss) order[n++] = &g_p[i];
    qsort(order, n, sizeof(order[0]), by_mem);
    for (i = 0; i < n; i++)
    {
        WCHAR pid[16], ws[32];
        const WCHAR *c[3] = { order[i]->name, pid, ws };
        _snwprintf(pid, 16, L"%lu", order[i]->pid);
        _snwprintf(ws, 32, L"%llu", order[i]->rss / 1024);
        row(lv, i, order[i]->pid, c, 3);
    }
    trim(lv, n);
}

static void fill_disk(HWND lv)
{
    static proc_t *order[MAXP];
    int i, n = 0;
    for (i = 0; i < g_np; i++) if (g_p[i].rps + g_p[i].wps > 0) order[n++] = &g_p[i];
    qsort(order, n, sizeof(order[0]), by_disk);
    for (i = 0; i < n; i++)
    {
        WCHAR pid[16], rd[32], wr[32], tot[32];
        const WCHAR *c[5] = { order[i]->name, pid, rd, wr, tot };
        _snwprintf(pid, 16, L"%lu", order[i]->pid);
        fmt_rate(order[i]->rps, rd, 32);
        fmt_rate(order[i]->wps, wr, 32);
        fmt_rate(order[i]->rps + order[i]->wps, tot, 32);
        row(lv, i, order[i]->pid, c, 5);
    }
    trim(lv, n);
}

static void fill_net_procs(HWND lv)
{
    static proc_t *order[MAXP];
    int i, n = 0;
    for (i = 0; i < g_np; i++) if (g_p[i].conns + g_p[i].listens) order[n++] = &g_p[i];
    qsort(order, n, sizeof(order[0]), by_net);
    for (i = 0; i < n; i++)
    {
        WCHAR pid[16], cn[16], ls[16];
        const WCHAR *c[4] = { order[i]->name, pid, cn, ls };
        _snwprintf(pid, 16, L"%lu", order[i]->pid);
        _snwprintf(cn, 16, L"%d", order[i]->conns);
        _snwprintf(ls, 16, L"%d", order[i]->listens);
        row(lv, i, order[i]->pid, c, 4);
    }
    trim(lv, n);
}

static void split_addr(const WCHAR *a, WCHAR *host, WCHAR *port)
{
    const WCHAR *colon = wcsrchr(a, ':');
    if (!colon) { lstrcpynW(host, a, 64); port[0] = 0; return; }
    lstrcpynW(host, a, (int)min(64, colon - a + 1));
    lstrcpynW(port, colon + 1, 16);
}

static void fill_conns(HWND lv, BOOL listening)
{
    int i, n = 0;
    for (i = 0; i < g_nc; i++)
    {
        conn_t *c = &g_c[i];
        proc_t *p;
        WCHAR pid[16], lh[64], lp[16], rh[64], rp[16];
        const WCHAR *img;
        if (listening != !wcscmp(c->state, L"LISTEN") || c->proto[0] != 't') continue;
        p = c->have_pid ? find(c->pid) : NULL;
        img = p ? p->name : L"(not this user's)";
        if (c->have_pid) _snwprintf(pid, 16, L"%lu", c->pid); else lstrcpyW(pid, L"");
        split_addr(c->local, lh, lp);
        split_addr(c->remote, rh, rp);
        if (listening)
        {
            const WCHAR *cells[5] = { img, pid, lh, lp, c->proto };
            row(lv, n++, c->pid, cells, 5);
        }
        else
        {
            const WCHAR *cells[6] = { img, pid, lh, lp, rh, rp };
            row(lv, n++, c->pid, cells, 6);
        }
    }
    trim(lv, n);
}

static void fill_storage(HWND lv)
{
    DWORD mask = GetLogicalDrives();
    int d, n = 0;
    for (d = 0; d < 26; d++)
    {
        WCHAR root[4] = { 'A' + d, ':', '\\', 0 }, name[4] = { 'A' + d, ':', 0 }, av[32], tot[32], pct[16];
        ULARGE_INTEGER a, t, f;
        if (!(mask & (1u << d)) || !GetDiskFreeSpaceExW(root, &a, &t, &f)) continue;
        _snwprintf(av, 32, L"%llu", a.QuadPart / (1024 * 1024));
        _snwprintf(tot, 32, L"%llu", t.QuadPart / (1024 * 1024));
        _snwprintf(pct, 16, L"%llu", t.QuadPart ? (t.QuadPart - f.QuadPart) * 100 / t.QuadPart : 0);
        {
            const WCHAR *cells[4] = { name, pct, av, tot };
            row(lv, n++, d, cells, 4);
        }
    }
    trim(lv, n);
}

/* ---- tabs and sections ---------------------------------------------------------------------- */

typedef struct section { const WCHAR *title; const col_t *cols; int ncols; void (*fill)(HWND); } section_t;

static const col_t CPU_COLS[] = { { L"Image", 180 }, { L"PID", 60, TRUE }, { L"User", 90 }, { L"Threads", 60, TRUE },
                                  { L"CPU", 50, TRUE }, { L"Average CPU", 90, TRUE } };
static const col_t MEM_COLS[] = { { L"Image", 180 }, { L"PID", 60, TRUE }, { L"Working Set (KB)", 130, TRUE } };
static const col_t DISK_COLS[] = { { L"Image", 180 }, { L"PID", 60, TRUE }, { L"Read (B/sec)", 100, TRUE },
                                   { L"Write (B/sec)", 100, TRUE }, { L"Total (B/sec)", 100, TRUE } };
static const col_t NETP_COLS[] = { { L"Image", 180 }, { L"PID", 60, TRUE }, { L"TCP Connections", 120, TRUE },
                                   { L"Listening Ports", 110, TRUE } };
static const col_t TCP_COLS[] = { { L"Image", 160 }, { L"PID", 60, TRUE }, { L"Local Address", 120 },
                                  { L"Local Port", 70, TRUE }, { L"Remote Address", 120 }, { L"Remote Port", 80, TRUE } };
static const col_t LISTEN_COLS[] = { { L"Image", 160 }, { L"PID", 60, TRUE }, { L"Address", 140 }, { L"Port", 60, TRUE },
                                     { L"Protocol", 70 } };
static const col_t STORE_COLS[] = { { L"Logical Disk", 100 }, { L"Active Time (%)", 110, TRUE },
                                    { L"Available Space (MB)", 150, TRUE }, { L"Total Space (MB)", 130, TRUE } };

static void fill_tcp(HWND lv) { fill_conns(lv, FALSE); }
static void fill_listen(HWND lv) { fill_conns(lv, TRUE); }

static const section_t TAB_SECTIONS[5][4] = {
    { { L"CPU", CPU_COLS, 6, fill_cpu }, { L"Disk", DISK_COLS, 5, fill_disk }, { L"Network", NETP_COLS, 4, fill_net_procs },
      { L"Memory", MEM_COLS, 3, fill_mem } },
    { { L"Processes", CPU_COLS, 6, fill_cpu } },
    { { L"Processes", MEM_COLS, 3, fill_mem } },
    { { L"Processes with Disk Activity", DISK_COLS, 5, fill_disk }, { L"Storage", STORE_COLS, 4, fill_storage } },
    { { L"Processes with Network Activity", NETP_COLS, 4, fill_net_procs }, { L"TCP Connections", TCP_COLS, 6, fill_tcp },
      { L"Listening Ports", LISTEN_COLS, 5, fill_listen } },
};

static int nsections(int tab) { int n = 0; while (n < 4 && TAB_SECTIONS[tab][n].title) n++; return n; }

static void refresh_lists(void)
{
    int i;
    for (i = 0; i < g_nlists; i++)
    {
        SendMessageW(g_lists[i], WM_SETREDRAW, FALSE, 0);
        TAB_SECTIONS[g_tab][i].fill(g_lists[i]);
        SendMessageW(g_lists[i], WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_lists[i], NULL, FALSE);
    }
    InvalidateRect(g_graphs, NULL, FALSE);
}

static void layout(void)
{
    RECT rc, t;
    int gw = S(270), top, h, i, hh = S(26);
    GetClientRect(g_wnd, &rc);
    MoveWindow(g_tabs, S(4), S(4), rc.right - S(8), rc.bottom - S(8), TRUE);
    GetClientRect(g_tabs, &t);
    TabCtrl_AdjustRect(g_tabs, FALSE, &t);
    MapWindowPoints(g_tabs, g_wnd, (POINT *)&t, 2);
    MoveWindow(g_graphs, t.right - gw, t.top, gw, t.bottom - t.top, TRUE);
    top = t.top;
    h = g_nlists ? (t.bottom - t.top) / g_nlists : 0;
    for (i = 0; i < g_nlists; i++)
    {
        MoveWindow(g_heads[i], t.left, top + i * h, t.right - gw - t.left - S(6), hh, TRUE);
        MoveWindow(g_lists[i], t.left, top + i * h + hh, t.right - gw - t.left - S(6), h - hh - S(4), TRUE);
    }
}

static void show_tab(int tab)
{
    int i;
    g_tab = tab;
    g_nlists = nsections(tab);
    for (i = 0; i < 4; i++)
    {
        ShowWindow(g_lists[i], i < g_nlists ? SW_SHOW : SW_HIDE);
        ShowWindow(g_heads[i], i < g_nlists ? SW_SHOW : SW_HIDE);
        if (i < g_nlists)
        {
            ListView_DeleteAllItems(g_lists[i]);
            set_cols(g_lists[i], TAB_SECTIONS[tab][i].cols, TAB_SECTIONS[tab][i].ncols);
            SetWindowTextW(g_heads[i], TAB_SECTIONS[tab][i].title);
        }
    }
    layout();
    refresh_lists();
}

/* ---- graphs ----------------------------------------------------------------------------------- */

static void draw_graph(HDC dc, RECT r, const WCHAR *title, const WCHAR *now, const double *h, double max, COLORREF col)
{
    HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0)), frame = CreateSolidBrush(RGB(60, 60, 60));
    HPEN grid = CreatePen(PS_SOLID, 1, RGB(0, 70, 0)), pen = CreatePen(PS_SOLID, 2, col);
    RECT g = r, t = r;
    POINT pts[HIST];
    int i;
    t.bottom = t.top + S(18);
    SelectObject(dc, g_font_bold);
    SetTextColor(dc, C_TEXT);
    DrawTextW(dc, title, -1, &t, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX);
    SelectObject(dc, g_font);
    DrawTextW(dc, now, -1, &t, DT_SINGLELINE | DT_RIGHT | DT_NOPREFIX);
    g.top += S(20);
    FillRect(dc, &g, bg);
    FrameRect(dc, &g, frame);
    SelectObject(dc, grid);
    for (i = 1; i < 4; i++)
    {
        MoveToEx(dc, g.left, g.top + (g.bottom - g.top) * i / 4, NULL);
        LineTo(dc, g.right, g.top + (g.bottom - g.top) * i / 4);
    }
    if (max <= 0) max = 1;
    for (i = 0; i < HIST; i++)
    {
        double v = h[i] / max;
        if (v > 1) v = 1;
        pts[i].x = g.left + (g.right - g.left - 1) * i / (HIST - 1);
        pts[i].y = g.bottom - 2 - (int)((g.bottom - g.top - 4) * v);
    }
    SelectObject(dc, pen);
    Polyline(dc, pts, HIST);
    DeleteObject(bg); DeleteObject(frame); DeleteObject(grid); DeleteObject(pen);
}

static double peak(const double *h) { double m = 0; int i; for (i = 0; i < HIST; i++) if (h[i] > m) m = h[i]; return m; }

static LRESULT CALLBACK graphs_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_PAINT)
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc, r;
        WCHAR now[64];
        int gh;
        HBRUSH bg = CreateSolidBrush(C_BG);
        GetClientRect(h, &rc);
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        gh = (rc.bottom - S(8)) / 4;
        r = rc; r.left += S(6); r.right -= S(6);
        r.top = S(4); r.bottom = r.top + gh - S(8);
        _snwprintf(now, 64, L"%.0f%% CPU Usage", g_cpu_now);
        draw_graph(dc, r, L"CPU", now, g_cpu_hist, 100, RGB(50, 200, 50));
        r.top += gh; r.bottom += gh;
        _snwprintf(now, 64, L"%.0f KB/sec", g_disk_now / 1024);
        draw_graph(dc, r, L"Disk", now, g_disk_hist, max(peak(g_disk_hist), 1024 * 1024), RGB(80, 150, 255));
        r.top += gh; r.bottom += gh;
        _snwprintf(now, 64, L"%.0f Kbps", g_net_now / 1000);
        draw_graph(dc, r, L"Network", now, g_net_hist, max(peak(g_net_hist), 1000000), RGB(240, 180, 20));
        r.top += gh; r.bottom += gh;
        _snwprintf(now, 64, L"%.0f%% Used Physical Memory", g_mem_now);
        draw_graph(dc, r, L"Memory", now, g_mem_hist, 100, RGB(180, 110, 240));
        EndPaint(h, &ps);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- the dump --------------------------------------------------------------------------------- */

static void dump(void)
{
    FILE *f;
    int i, r, c;
    WCHAR tmp[MAX_PATH + 8];
    if (!g_dump[0]) return;
    _snwprintf(tmp, ARRAY_SIZE(tmp), L"%ls.tmp", g_dump);
    if (!(f = _wfopen(tmp, L"wb"))) return;
    fprintf(f, "TITLE Resource Monitor\nBRIDGED %d\nTAB %d\nCPU %.1f\nMEM %.1f\nDISK %.0f\nPROCS %d\nCONNS %d\n", sys_bridged(),
            g_tab, g_cpu_now, g_mem_now, g_disk_now, g_np, g_nc);
    for (i = 0; i < 5; i++)
    {
        RECT tr;
        POINT pt;
        TabCtrl_GetItemRect(g_tabs, i, &tr);
        screen_center(g_tabs, &tr, &pt);
        fprintf(f, "TABPOS %d %ld %ld\n", i, pt.x, pt.y);
    }
    for (i = 0; i < g_nlists; i++)
    {
        int rows = ListView_GetItemCount(g_lists[i]), cols = TAB_SECTIONS[g_tab][i].ncols;
        fprintf(f, "SECTION %d %ls\n", i, TAB_SECTIONS[g_tab][i].title);
        for (r = 0; r < rows && r < 400; r++)
        {
            fprintf(f, "ROW %d", i);
            for (c = 0; c < cols; c++)
            {
                WCHAR t[256];
                ListView_GetItemText(g_lists[i], r, c, t, 256);
                fprintf(f, "\t%ls", t);
            }
            fputc('\n', f);
        }
    }
    fprintf(f, "END\n");
    fclose(f);
    MoveFileExW(tmp, g_dump, MOVEFILE_REPLACE_EXISTING);
}

/* ---- the window --------------------------------------------------------------------------------- */

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (sg_mode_changed(msg, lp)) sgm_follow(h);
    switch (msg)
    {
    case WM_SIZE: layout(); return 0;
    case WM_TIMER:
        sample();
        refresh_lists();
        dump();
        return 0;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->hwndFrom == g_tabs && ((NMHDR *)lp)->code == TCN_SELCHANGE)
        {
            show_tab(TabCtrl_GetCurSel(g_tabs));
            dump();
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == 1) DestroyWindow(h);
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, C_SURFACE);
        SetTextColor((HDC)wp, C_HEAD);
        {
            SetDCBrushColor((HDC)wp, C_SURFACE);
            return (LRESULT)GetStockObject(DC_BRUSH);
        }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int resmon_main(void)
{
    WNDCLASSW wc = { 0 };
    static const WCHAR *const names[] = { L"Overview", L"CPU", L"Memory", L"Disk", L"Network" };
    HMENU bar = CreateMenu(), file = CreatePopupMenu();
    RECT work;
    MSG msg;
    int i;

    GetEnvironmentVariableW(L"SG_MMC_DUMP", g_dump, MAX_PATH);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = g_inst;
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"SgResourceMonitor";
    RegisterClassW(&wc);
    wc.lpfnWndProc = graphs_proc;
    wc.hIcon = NULL;
    wc.lpszClassName = L"SgResmonGraphs";
    RegisterClassW(&wc);
    AppendMenuW(file, MF_STRING, 1, L"E&xit");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    sgm_dark = sg_apps_dark();
    g_wnd = CreateWindowExW(0, L"SgResourceMonitor", L"Resource Monitor", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            work.left + S(30), work.top + S(20), min(S(1180), work.right - work.left - S(60)),
                            min(S(720), work.bottom - work.top - S(40)), NULL, bar, g_inst, NULL);
    if (g_wnd) sg_mode_title(g_wnd, sgm_dark);
    g_main = g_wnd;
    g_tabs = CreateWindowExW(0, WC_TABCONTROLW, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, g_wnd, NULL,
                             g_inst, NULL);
    SendMessageW(g_tabs, WM_SETFONT, (WPARAM)g_font, 0);
    for (i = 0; i < 5; i++)
    {
        TCITEMW ti = { TCIF_TEXT };
        ti.pszText = (WCHAR *)names[i];
        SendMessageW(g_tabs, TCM_INSERTITEMW, i, (LPARAM)&ti);
    }
    for (i = 0; i < 4; i++)
    {
        g_heads[i] = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX, 0, 0, 0, 0,
                                     g_wnd, NULL, g_inst, NULL);
        SendMessageW(g_heads[i], WM_SETFONT, (WPARAM)g_font_bold, 0);
        g_lists[i] = CreateWindowExW(0, WC_LISTVIEWW, NULL, WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SHOWSELALWAYS |
                                     LVS_SINGLESEL, 0, 0, 0, 0, g_wnd, NULL, g_inst, NULL);
        ListView_SetExtendedListViewStyle(g_lists[i], LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        SendMessageW(g_lists[i], WM_SETFONT, (WPARAM)g_font, 0);
        SetWindowPos(g_heads[i], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(g_lists[i], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    g_graphs = CreateWindowExW(0, L"SgResmonGraphs", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, g_wnd, NULL, g_inst, NULL);
    SetWindowPos(g_graphs, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    sample();
    show_tab(T_OVERVIEW);
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    SetTimer(g_wnd, 1, 1000, NULL);
    dump();
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
