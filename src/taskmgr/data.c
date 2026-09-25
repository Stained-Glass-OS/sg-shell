/* sg-taskmgr -- what the machine is doing: processes, CPU, memory, network.
 *
 * One sample a refresh. The process list is NtQuerySystemInformation's
 * (under Wine, every Windows process on this machine's wineserver -- the
 * Linux processes beneath them are not Windows processes and are not
 * listed); CPU and memory for the whole machine are GetSystemTimes' and
 * GlobalMemoryStatusEx's, which Wine answers from /proc, so the Performance
 * page is the real machine's. What does not change while a process lives
 * (its path, description, user, architecture, icon) is looked up once.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <winsock2.h>
#include <ws2ipdef.h>
#include "taskmgr.h"
#include <netioapi.h>
#include <iphlpapi.h>

/* SYSTEM_PROCESS_INFORMATION as Windows lays it out (winternl.h's is partial) */
typedef struct
{
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime, UserTime, KernelTime;
    UNICODE_STRING ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId, InheritedFromUniqueProcessId;
    ULONG HandleCount, SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize, VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize, WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage, QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage, PeakPagefileUsage, PrivatePageCount;
    LARGE_INTEGER ReadOperationCount, WriteOperationCount, OtherOperationCount;
    LARGE_INTEGER ReadTransferCount, WriteTransferCount, OtherTransferCount;
} SG_SPI;

proc_t *g_procs;
int g_nprocs;
static int g_cap;
perf_t g_perf;

static ULONGLONG g_last_tick, g_last_idle, g_last_total;

/* The shell's own processes: Windows 10 lists these as "Windows processes". */
static const WCHAR *const windows_procs[] = {
    L"services.exe", L"winedevice.exe", L"svchost.exe", L"plugplay.exe", L"rpcss.exe", L"explorer.exe",
    L"wineboot.exe", L"winemenubuilder.exe", L"conhost.exe", L"csrss.exe", L"lsass.exe", L"smss.exe",
    L"wininit.exe", L"winlogon.exe", L"spoolsv.exe", L"sg-start64.exe", L"sg-netflyout64.exe",
    L"sg-dictate64.exe", L"sg-start32.exe", L"dllhost.exe", L"rundll32.exe",
};

/* Names the shell's own programs go by (they carry no version resource) */
static const struct { const WCHAR *exe, *name; } friendly[] = {
    { L"sg-start64.exe", L"Start" },
    { L"sg-netflyout64.exe", L"Network" },
    { L"sg-dictate64.exe", L"Voice typing" },
    { L"sg-control64.exe", L"Control Panel" },
    { L"sg-ncpa64.exe", L"Network Connections" },
    { L"sg-mstsc64.exe", L"Remote Desktop Connection" },
    { L"sg-taskmgr64.exe", L"Task Manager" },
    { L"explorer.exe", L"File Explorer" },
    { L"services.exe", L"Services and Controller app" },
    { L"svchost.exe", L"Service Host" },
    { L"winedevice.exe", L"Device driver host" },
    { L"plugplay.exe", L"Plug and Play" },
    { L"rpcss.exe", L"Remote Procedure Call" },
    { L"conhost.exe", L"Console Window Host" },
};

void fmt_mem(ULONGLONG b, WCHAR *out, int cch)
{
    double mb = b / 1048576.0;
    if (mb >= 1024.0 * 10) _snwprintf(out, cch, L"%.1f GB", mb / 1024.0);
    else _snwprintf(out, cch, L"%.1f MB", mb);
    out[cch - 1] = 0;
}

proc_t *find_proc(DWORD pid)
{
    int i;
    for (i = 0; i < g_nprocs; i++) if (g_procs[i].pid == pid) return &g_procs[i];
    return NULL;
}

static void version_string(const BYTE *ver, const WCHAR *what, WCHAR *out, int cch)
{
    struct { WORD lang, cp; } *tr;
    UINT n;
    WCHAR q[96];
    WCHAR *s;
    static const DWORD fallbacks[] = { 0x040904B0, 0x040904E4, 0x04090000 };
    int i;

    if (VerQueryValueW(ver, L"\\VarFileInfo\\Translation", (void **)&tr, &n) && n >= sizeof(*tr))
    {
        _snwprintf(q, 96, L"\\StringFileInfo\\%04x%04x\\%s", tr->lang, tr->cp, what);
        if (VerQueryValueW(ver, q, (void **)&s, &n) && n > 1 && *s) { lstrcpynW(out, s, cch); return; }
    }
    for (i = 0; i < 3; i++)
    {
        _snwprintf(q, 96, L"\\StringFileInfo\\%08lx\\%s", fallbacks[i], what);
        if (VerQueryValueW(ver, q, (void **)&s, &n) && n > 1 && *s) { lstrcpynW(out, s, cch); return; }
    }
}

static void describe(proc_t *p)
{
    DWORD size, dummy;
    BYTE *ver;
    int i;

    for (i = 0; i < (int)ARRAYSIZE(friendly); i++)
        if (!lstrcmpiW(p->name, friendly[i].exe)) { lstrcpynW(p->desc, friendly[i].name, 128); break; }
    if (!p->path[0] || !(size = GetFileVersionInfoSizeW(p->path, &dummy))) goto done;
    if (!(ver = malloc(size))) goto done;
    if (GetFileVersionInfoW(p->path, 0, size, ver))
    {
        if (!p->desc[0]) version_string(ver, L"FileDescription", p->desc, 128);
        version_string(ver, L"CompanyName", p->company, 96);
    }
    free(ver);
done:
    if (!p->desc[0]) lstrcpynW(p->desc, p->name, 128);
}

static void who(proc_t *p, HANDLE h)
{
    HANDLE tok;
    BYTE buf[256];
    DWORD len, nn = 64, dn = 64;
    WCHAR dom[64];
    SID_NAME_USE use;
    if (!OpenProcessToken(h, TOKEN_QUERY, &tok)) return;
    if (GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &len))
        if (!LookupAccountSidW(NULL, ((TOKEN_USER *)buf)->User.Sid, p->user, &nn, dom, &dn, &use)) p->user[0] = 0;
    CloseHandle(tok);
}

HICON load_small_icon(const WCHAR *path)
{
    HICON small = NULL;
    if (path && path[0] && ExtractIconExW(path, 0, NULL, &small, 1) >= 1 && small) return small;
    return generic_icon();
}

/* Details that do not change while a process lives */
static void look_up(proc_t *p)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p->pid);
    BOOL wow = FALSE;
    lstrcpyW(p->arch, L"x64");
    if (h)
    {
        DWORD n = MAX_PATH;
        if (!QueryFullProcessImageNameW(h, 0, p->path, &n)) p->path[0] = 0;
        who(p, h);
        if (IsWow64Process(h, &wow) && wow) lstrcpyW(p->arch, L"x86");
        CloseHandle(h);
    }
    describe(p);
    p->icon = load_small_icon(p->path);
}

/* each process's main window: the first visible, unowned, captioned one */
static BOOL CALLBACK find_windows(HWND w, LPARAM lp)
{
    DWORD pid;
    proc_t *p;
    WCHAR cls[64], title[128];
    (void)lp;
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    if (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    if (!GetWindowTextW(w, title, 128)) return TRUE;
    GetClassNameW(w, cls, 64);
    if (!lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"Shell_TrayWnd") || !lstrcmpW(cls, L"WorkerW")) return TRUE;
    GetWindowThreadProcessId(w, &pid);
    if (!(p = find_proc(pid)) || p->win) return TRUE;
    p->win = w;
    lstrcpynW(p->title, title, 128);
    return TRUE;
}

static void sample_network(double secs)
{
    static ULONGLONG last_in, last_out;
    static NET_LUID last_luid;
    MIB_IF_TABLE2 *t = NULL;
    ULONG i;
    BOOL found = FALSE;

    if (GetIfTable2(&t) != NO_ERROR || !t) { g_perf.net = FALSE; return; }
    for (i = 0; i < t->NumEntries; i++)
    {
        MIB_IF_ROW2 *r = &t->Table[i];
        if (r->Type == IF_TYPE_SOFTWARE_LOOPBACK || r->OperStatus != IfOperStatusUp) continue;
        if (!r->InterfaceAndOperStatusFlags.HardwareInterface && r->Type != IF_TYPE_ETHERNET_CSMACD
            && r->Type != IF_TYPE_IEEE80211) continue;
        found = TRUE;
        lstrcpynW(g_perf.net_kind, r->Type == IF_TYPE_IEEE80211 ? L"Wi-Fi" : L"Ethernet", 16);
        lstrcpynW(g_perf.net_name, r->Alias[0] ? r->Alias : r->Description, 64);
        if (last_luid.Value == r->InterfaceLuid.Value && secs > 0)
        {
            g_perf.recv_kbps = (r->InOctets - last_in) * 8.0 / 1000.0 / secs;
            g_perf.send_kbps = (r->OutOctets - last_out) * 8.0 / 1000.0 / secs;
        }
        last_in = r->InOctets; last_out = r->OutOctets; last_luid = r->InterfaceLuid;
        break;
    }
    FreeMibTable(t);
    g_perf.net = found;
}

static void push(double *hist, double v)
{
    memmove(hist, hist + 1, 59 * sizeof(double));
    hist[59] = v;
}

void sample(void)
{
    static BYTE *buf;
    static ULONG bufsize = 1 << 18;
    static BOOL once;
    ULONG len = 0;
    NTSTATUS st;
    SG_SPI *s;
    ULONGLONG now = GetTickCount64(), idle, total;
    double secs = g_last_tick ? (now - g_last_tick) / 1000.0 : 0;
    FILETIME fi, fk, fu;
    MEMORYSTATUSEX ms = { sizeof(ms) };
    int i;

    if (!once)
    {
        SYSTEM_INFO si;
        HKEY k;
        DWORD sz;
        once = TRUE;
        GetNativeSystemInfo(&si);
        g_perf.ncpu = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1;
        if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &k))
        {
            sz = sizeof(g_perf.mhz);
            RegQueryValueExW(k, L"~MHz", NULL, NULL, (BYTE *)&g_perf.mhz, &sz);
            sz = sizeof(g_perf.cpu_name) - sizeof(WCHAR);
            RegQueryValueExW(k, L"ProcessorNameString", NULL, NULL, (BYTE *)g_perf.cpu_name, &sz);
            RegCloseKey(k);
        }
        if (!g_perf.cpu_name[0]) lstrcpyW(g_perf.cpu_name, L"Processor");
        /* trim the padding some CPUs put in front of their name */
        { WCHAR *c = g_perf.cpu_name; while (*c == ' ') c++; memmove(g_perf.cpu_name, c, (lstrlenW(c) + 1) * sizeof(WCHAR)); }
    }

    for (;;)
    {
        if (!buf && !(buf = malloc(bufsize))) return;
        st = NtQuerySystemInformation(SystemProcessInformation, buf, bufsize, &len);
        if (st != (NTSTATUS)0xC0000004L /* STATUS_INFO_LENGTH_MISMATCH */) break;
        free(buf); buf = NULL; bufsize *= 2;
    }
    if (st) return;

    for (i = 0; i < g_nprocs; i++) g_procs[i].seen = FALSE;
    g_perf.procs = g_perf.threads = g_perf.handles = 0;
    for (s = (SG_SPI *)buf;; s = (SG_SPI *)((BYTE *)s + s->NextEntryOffset))
    {
        DWORD pid = (DWORD)(ULONG_PTR)s->UniqueProcessId;
        proc_t *p = find_proc(pid);
        ULONGLONG cpu = s->UserTime.QuadPart + s->KernelTime.QuadPart;
        ULONGLONG io = s->ReadTransferCount.QuadPart + s->WriteTransferCount.QuadPart;

        if (pid == 0 && !s->ImageName.Length) goto next;   /* the idle process, if Wine ever lists one */
        if (p && p->created != s->CreateTime.QuadPart) { if (p->icon && p->icon != generic_icon()) DestroyIcon(p->icon); memset(p, 0, sizeof(*p)); p->pid = pid; p = NULL; }
        if (!p)
        {
            if (g_nprocs == g_cap)
            {
                proc_t *n = realloc(g_procs, (g_cap ? g_cap * 2 : 64) * sizeof(proc_t));
                if (!n) goto next;
                g_procs = n; g_cap = g_cap ? g_cap * 2 : 64;
            }
            p = &g_procs[g_nprocs++];
            memset(p, 0, sizeof(*p));
            p->pid = pid;
            p->created = s->CreateTime.QuadPart;
            lstrcpynW(p->name, s->ImageName.Buffer ? s->ImageName.Buffer : L"System",
                      min((int)s->ImageName.Length / 2 + 1, 64));
            if (!s->ImageName.Length) lstrcpyW(p->name, L"System");
            p->cpu_time = cpu;
            p->io = io;
            look_up(p);
        }
        else
        {
            /* a process's CPU time over the interval, as a share of every CPU */
            p->cpu = secs > 0 ? (double)(cpu - p->cpu_time) / (secs * 1e7 * g_perf.ncpu) * 100.0 : 0;
            if (p->cpu < 0) p->cpu = 0;
            if (p->cpu > 100) p->cpu = 100;
            p->disk = secs > 0 ? (io - p->io) / 1048576.0 / secs : 0;
            p->cpu_time = cpu;
            p->io = io;
        }
        p->seen = TRUE;
        p->ppid = (DWORD)(ULONG_PTR)s->InheritedFromUniqueProcessId;
        p->threads = s->NumberOfThreads;
        p->handles = s->HandleCount;
        p->session = s->SessionId;
        p->ws = s->WorkingSetPrivateSize.QuadPart ? (SIZE_T)s->WorkingSetPrivateSize.QuadPart : s->WorkingSetSize;
        p->win = NULL;
        p->title[0] = 0;
        g_perf.procs++;
        g_perf.threads += s->NumberOfThreads;
        g_perf.handles += s->HandleCount;
next:
        if (!s->NextEntryOffset) break;
    }
    /* drop what has gone */
    for (i = 0; i < g_nprocs;)
    {
        if (g_procs[i].seen) { i++; continue; }
        if (g_procs[i].icon && g_procs[i].icon != generic_icon()) DestroyIcon(g_procs[i].icon);
        g_procs[i] = g_procs[--g_nprocs];
    }

    EnumWindows(find_windows, 0);
    for (i = 0; i < g_nprocs; i++)
    {
        proc_t *p = &g_procs[i];
        int j;
        p->group = p->win ? GRP_APPS : GRP_BACKGROUND;
        /* an app with no description goes by its window's title, as Windows names it */
        if (p->win && !lstrcmpiW(p->desc, p->name) && p->title[0]) lstrcpynW(p->desc, p->title, 128);
        if (p->group == GRP_BACKGROUND)
            for (j = 0; j < (int)ARRAYSIZE(windows_procs); j++)
                if (!lstrcmpiW(p->name, windows_procs[j])) { p->group = GRP_WINDOWS; break; }
    }

    /* the machine */
    if (GetSystemTimes(&fi, &fk, &fu))
    {
        idle = ((ULONGLONG)fi.dwHighDateTime << 32) | fi.dwLowDateTime;
        total = (((ULONGLONG)fk.dwHighDateTime << 32) | fk.dwLowDateTime) +
                (((ULONGLONG)fu.dwHighDateTime << 32) | fu.dwLowDateTime);   /* kernel time includes idle */
        if (g_last_total && total > g_last_total)
            g_perf.cpu = 100.0 * (1.0 - (double)(idle - g_last_idle) / (double)(total - g_last_total));
        if (g_perf.cpu < 0) g_perf.cpu = 0;
        if (g_perf.cpu > 100) g_perf.cpu = 100;
        g_last_idle = idle; g_last_total = total;
    }
    if (GlobalMemoryStatusEx(&ms))
    {
        g_perf.mem_total = ms.ullTotalPhys;
        g_perf.mem_avail = ms.ullAvailPhys;
        g_perf.commit_limit = ms.ullTotalPageFile;
        g_perf.commit = ms.ullTotalPageFile - ms.ullAvailPageFile;
    }
    g_perf.uptime_ms = now;
    sample_network(secs);
    if (g_last_tick)
    {
        double net = g_perf.send_kbps + g_perf.recv_kbps;
        push(g_perf.cpu_hist, g_perf.cpu);
        push(g_perf.mem_hist, g_perf.mem_total ? 100.0 * (g_perf.mem_total - g_perf.mem_avail) / g_perf.mem_total : 0);
        push(g_perf.net_hist, net);
        g_perf.net_max = 100;
        for (i = 0; i < 60; i++) if (g_perf.net_hist[i] > g_perf.net_max) g_perf.net_max = g_perf.net_hist[i];
    }
    g_last_tick = now;
}

/* ---- ending processes --------------------------------------------------- */

static BOOL CALLBACK close_windows(HWND w, LPARAM pid)
{
    DWORD owner;
    GetWindowThreadProcessId(w, &owner);
    if (owner == (DWORD)pid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) PostMessageW(w, WM_CLOSE, 0, 0);
    return TRUE;
}

typedef struct { DWORD pid; } ender_t;

static DWORD WINAPI ender(void *arg)
{
    DWORD pid = ((ender_t *)arg)->pid;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    free(arg);
    if (!h) return 0;
    /* as Windows' End task: ask the app to close, and end it if it does not */
    EnumWindows(close_windows, pid);
    if (WaitForSingleObject(h, 3000) == WAIT_TIMEOUT) TerminateProcess(h, 1);
    CloseHandle(h);
    return 0;
}

BOOL end_process(DWORD pid, BOOL gracefully)
{
    proc_t *p = find_proc(pid);
    HANDLE h;
    BOOL ok;
    if (gracefully && p && p->win)
    {
        ender_t *e = malloc(sizeof(*e));
        HANDLE t;
        if (!e) return FALSE;
        e->pid = pid;
        if ((t = CreateThread(NULL, 0, ender, e, 0, NULL))) { CloseHandle(t); return TRUE; }
        free(e);
    }
    if (!(h = OpenProcess(PROCESS_TERMINATE, FALSE, pid))) return FALSE;
    ok = TerminateProcess(h, 1);
    CloseHandle(h);
    return ok;
}

BOOL end_tree(DWORD pid)
{
    int i;
    BOOL ok = TRUE;
    for (i = 0; i < g_nprocs; i++)
        if (g_procs[i].ppid == pid && g_procs[i].pid != pid && g_procs[i].pid != GetCurrentProcessId())
            ok &= end_tree(g_procs[i].pid);
    return end_process(pid, FALSE) && ok;
}

void open_location(const WCHAR *path)
{
    WCHAR args[MAX_PATH + 16];
    if (!path || !path[0]) return;
    _snwprintf(args, ARRAYSIZE(args), L"/select,\"%s\"", path);
    args[ARRAYSIZE(args) - 1] = 0;
    ShellExecuteW(NULL, NULL, L"explorer.exe", args, NULL, SW_SHOWNORMAL);
}
