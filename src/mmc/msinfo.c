/* sg-mmc -- System Information (msinfo32.exe, as sg-msinfo3264.exe): Windows'
 * System Information -- a category tree (System Summary, Hardware Resources,
 * Components, Software Environment) and Item/Value lists -- on the same
 * console host, without the toolbar and Actions pane (msinfo32 has none).
 *
 * The machine's facts come from sg-sysinfo (DMI, /proc, lsblk, sysfs); the
 * Windows side's from Windows itself (the version Wine reports, directories,
 * drives, display modes, environment, running tasks, services, startup
 * programs). `msinfo32 /report FILE` writes every category as text and exits,
 * as Windows' does.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "mmc.h"
#include <iphlpapi.h>
#include <tlhelp32.h>
#include <winsvc.h>
#include <string.h>

typedef void (*fill_fn)(void (*add)(const WCHAR *item, const WCHAR *value));

static sys_reply_t g_sys, g_dev, g_dsk;
static BOOL g_loaded;

static void load(void)
{
    if (g_loaded) return;
    g_loaded = TRUE;
    sys_request(&g_sys, "system", NULL);
    sys_request(&g_dev, "devices", NULL);
    sys_request(&g_dsk, "disks", NULL);
}

static void sysv(const char *key, WCHAR *out, int cch)
{
    const char *v = sys_field(&g_sys, 0, key);
    utf8_to_w(v ? v : "", out, cch);
}

/* ---- System Summary ------------------------------------------------------------------------ */

static void summary(void (*add)(const WCHAR *, const WCHAR *))
{
    WCHAR v[512], a[256], b[256], c[256];
    OSVERSIONINFOEXW os = { sizeof(os) };
    MEMORYSTATUSEX ms = { sizeof(ms) };
    DWORD n;
    const char *(CDECL *wine_version)(void) = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "wine_get_version");
    LONG (WINAPI *rtl_version)(OSVERSIONINFOEXW *) = (void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");

    sysv("OS-NAME", v, 512);
    add(L"OS Name", v[0] ? v : L"Stained Glass OS");
    if (rtl_version) rtl_version(&os);
    else GetVersionExW((OSVERSIONINFOW *)&os);
    _snwprintf(v, 512, L"%lu.%lu.%lu Build %lu", os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, os.dwBuildNumber);
    add(L"Version", v);
    add(L"OS Manufacturer", L"Stained Glass OS");
    if (wine_version) { utf8_to_w(wine_version(), a, 256); _snwprintf(v, 512, L"Wine %ls", a); add(L"Windows Compatibility", v); }
    sysv("KERNEL", v, 512);
    add(L"Kernel", v);
    n = 256; GetComputerNameW(a, &n);
    add(L"System Name", a);
    sysv("MANUFACTURER", v, 512); add(L"System Manufacturer", v);
    sysv("MODEL", v, 512); add(L"System Model", v);
    sysv("SYSTEM-TYPE", v, 512); add(L"System Type", v[0] ? v : L"x64-based PC");
    sysv("CPU", a, 256); sysv("CPU-MHZ", b, 256); sysv("CPU-CORES", c, 256);
    {
        WCHAR t[64];
        sysv("CPU-THREADS", t, 64);
        _snwprintf(v, 512, L"%ls, %ls Mhz, %ls Core(s), %ls Logical Processor(s)", a, b, c, t);
    }
    add(L"Processor", v);
    sysv("BIOS-VENDOR", a, 256); sysv("BIOS-VERSION", b, 256); sysv("BIOS-DATE", c, 256);
    _snwprintf(v, 512, L"%ls %ls, %ls", a, b, c);
    add(L"BIOS Version/Date", v);
    sysv("BOOT-MODE", v, 512); add(L"BIOS Mode", v);
    sysv("BOARD-VENDOR", v, 512); add(L"BaseBoard Manufacturer", v);
    sysv("BOARD-NAME", v, 512); add(L"BaseBoard Product", v);
    sysv("BOARD-VERSION", v, 512); add(L"BaseBoard Version", v);
    sysv("SECURE-BOOT", a, 256);
    add(L"Secure Boot State", !wcscmp(a, L"on") ? L"On" : !wcscmp(a, L"off") ? L"Off" : L"Unsupported");
    GetWindowsDirectoryW(v, 512); add(L"Windows Directory", v);
    GetSystemDirectoryW(v, 512); add(L"System Directory", v);
    sysv("BOOT-DEVICE", v, 512); add(L"Boot Device", v);
    sysv("LOCALE", v, 512); add(L"Locale", v);
    n = 256; GetUserNameW(a, &n);
    sysv("DOMAIN", b, 256);
    _snwprintf(v, 512, L"%ls\\%ls", b[0] ? b : L".", a);
    add(L"User Name", v);
    sysv("TIMEZONE", v, 512); add(L"Time Zone", v);
    {
        const char *t = sys_field(&g_sys, 0, "MEMORY-TOTAL"), *av = sys_field(&g_sys, 0, "MEMORY-AVAILABLE"),
                   *sw = sys_field(&g_sys, 0, "SWAP-TOTAL");
        ULONGLONG total = t ? strtoull(t, NULL, 10) : 0;
        GlobalMemoryStatusEx(&ms);
        if (!total) total = ms.ullTotalPhys;
#ifdef SG_MUTANT_MEM
        total /= 1000;
#endif
        fmt_bytes(total, v, 512); add(L"Installed Physical Memory (RAM)", v);
        fmt_bytes(total, v, 512); add(L"Total Physical Memory", v);
        fmt_bytes(av ? strtoull(av, NULL, 10) : ms.ullAvailPhys, v, 512); add(L"Available Physical Memory", v);
        fmt_bytes(ms.ullTotalPageFile, v, 512); add(L"Total Virtual Memory", v);
        fmt_bytes(ms.ullAvailPageFile, v, 512); add(L"Available Virtual Memory", v);
        fmt_bytes(sw ? strtoull(sw, NULL, 10) : 0, v, 512); add(L"Page File Space", v);
    }
    sysv("VIRTUALIZATION", v, 512);
    add(L"Virtualization", !wcscmp(v, L"none") || !v[0] ? L"None (running directly on the hardware)" : v);
}

/* ---- Components ----------------------------------------------------------------------------- */

static void devices_of(const char *cls, void (*add)(const WCHAR *, const WCHAR *))
{
    int b, n = 0;
    for (b = sys_next_block(&g_dev, 0, "DEVICE"); b >= 0; b = sys_next_block(&g_dev, b + 1, "DEVICE"))
    {
        const char *c = sys_field(&g_dev, b, "CLASS"), *keys[] = { "VENDOR", "DRIVER", "MODULE", "MODULE-VERSION",
                                                                     "LOCATION", "STATUS" };
        static const WCHAR *const names[] = { L"Manufacturer", L"Driver", L"Kernel Module", L"Driver Version",
                                               L"Location", L"Status" };
        WCHAR name[256], v[512];
        int k;
        if (!c || strcmp(c, cls)) continue;
        if (n++) add(L"", L"");
        utf8_to_w(sys_field(&g_dev, b, "NAME"), name, 256);
        add(L"Name", name);
        for (k = 0; k < 6; k++)
        {
            const char *s = sys_field(&g_dev, b, keys[k]);
            if (!s) continue;
            utf8_to_w(s, v, 512);
            add(names[k], k == 5 ? (!strcmp(s, "ok") ? L"OK" : L"No driver") : v);
        }
    }
    if (!n) add(L"No devices of this kind", L"");
}

static void display(void (*add)(const WCHAR *, const WCHAR *))
{
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    DEVMODEW dm = { 0 };
    WCHAR v[256];
    DWORD i;
    devices_of("display", add);
    for (i = 0; EnumDisplayDevicesW(NULL, i, &dd, 0); i++)
    {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        add(L"", L"");
        add(L"Windows Adapter", dd.DeviceString);
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
        {
            _snwprintf(v, 256, L"%lu x %lu x %lu hertz", dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
            add(L"Resolution", v);
            _snwprintf(v, 256, L"%lu Bits/Pixel", dm.dmBitsPerPel);
            add(L"Bits/Pixel", v);
        }
    }
}

static void network(void (*add)(const WCHAR *, const WCHAR *))
{
    ULONG size = 0;
    IP_ADAPTER_ADDRESSES *aa, *a;
    devices_of("net", add);
    GetAdaptersAddresses(AF_UNSPEC, 0, NULL, NULL, &size);
    if (!size || !(aa = malloc(size))) return;
    if (!GetAdaptersAddresses(AF_UNSPEC, 0, NULL, aa, &size))
        for (a = aa; a; a = a->Next)
        {
            IP_ADAPTER_UNICAST_ADDRESS *u;
            WCHAR mac[64] = L"", ip[64];
            ULONG k;
            if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            add(L"", L"");
            add(L"Windows Adapter", a->Description);
            for (k = 0; k < a->PhysicalAddressLength && k < 8; k++)
                _snwprintf(mac + wcslen(mac), 4, k ? L":%02X" : L"%02X", a->PhysicalAddress[k]);
            if (mac[0]) add(L"MAC Address", mac);
            for (u = a->FirstUnicastAddress; u; u = u->Next)
            {
                DWORD len = ARRAY_SIZE(ip);
                if (!WSAAddressToStringW(u->Address.lpSockaddr, u->Address.iSockaddrLength, NULL, ip, &len))
                    add(L"IP Address", ip);
            }
        }
    free(aa);
}

static void disks(void (*add)(const WCHAR *, const WCHAR *))
{
    int b, n = 0;
    for (b = sys_next_block(&g_dsk, 0, "DISK"); b >= 0; b = sys_next_block(&g_dsk, b + 1, "DISK"))
    {
        WCHAR v[256];
        const char *s;
        if (b && strcmp(g_dsk.lines[b - 1], "END") && strncmp(g_dsk.lines[b - 1], "MAP ", 4)) continue;
        if (n++) add(L"", L"");
        utf8_to_w(g_dsk.lines[b] + 5, v, 256);
        add(L"Name", v);
        if ((s = sys_field(&g_dsk, b, "MODEL"))) { utf8_to_w(s, v, 256); add(L"Model", v); }
        if ((s = sys_field(&g_dsk, b, "TRANSPORT"))) { utf8_to_w(s, v, 256); add(L"Interface Type", v); }
        if ((s = sys_field(&g_dsk, b, "SIZE"))) { fmt_bytes(strtoull(s, NULL, 10), v, 256); add(L"Size", v); }
        if ((s = sys_field(&g_dsk, b, "PTTYPE"))) { utf8_to_w(s, v, 256); add(L"Partition Style", v); }
        if ((s = sys_field(&g_dsk, b, "SERIAL"))) { utf8_to_w(s, v, 256); add(L"Serial Number", v); }
    }
    if (!n) add(L"No disks were found", L"");
}

static void drives(void (*add)(const WCHAR *, const WCHAR *))
{
    DWORD mask = GetLogicalDrives();
    int d, n = 0;
    for (d = 0; d < 26; d++)
    {
        WCHAR root[4] = { 'A' + d, ':', '\\', 0 }, label[128] = L"", fs[32] = L"", v[128];
        ULARGE_INTEGER avail, total, freeb;
        if (!(mask & (1u << d))) continue;
        if (n++) add(L"", L"");
        root[2] = 0;
        add(L"Drive", root);
        root[2] = '\\';
        GetVolumeInformationW(root, label, 128, NULL, NULL, NULL, fs, 32);
        add(L"Description", GetDriveTypeW(root) == DRIVE_REMOTE ? L"Network Connection" :
                            GetDriveTypeW(root) == DRIVE_CDROM ? L"CD-ROM Disc" : L"Local Fixed Disk");
        if (label[0]) add(L"Volume Name", label);
        if (fs[0]) add(L"File System", fs);
        if (GetDiskFreeSpaceExW(root, &avail, &total, &freeb))
        {
            fmt_bytes(total.QuadPart, v, 128); add(L"Size", v);
            fmt_bytes(freeb.QuadPart, v, 128); add(L"Free Space", v);
        }
    }
}

static void sound(void (*add)(const WCHAR *, const WCHAR *)) { devices_of("sound", add); }
static void usb(void (*add)(const WCHAR *, const WCHAR *)) { devices_of("usb", add); }
static void keyboard(void (*add)(const WCHAR *, const WCHAR *)) { devices_of("keyboard", add); }
static void mouse(void (*add)(const WCHAR *, const WCHAR *)) { devices_of("mouse", add); }

/* ---- Software Environment -------------------------------------------------------------------- */

static void environment(void (*add)(const WCHAR *, const WCHAR *))
{
    WCHAR *env = GetEnvironmentStringsW(), *p;
    for (p = env; p && *p; p += wcslen(p) + 1)
    {
        WCHAR name[256], *eq = wcschr(p + 1, '=');
        if (!eq) continue;
        lstrcpynW(name, p, (int)min((size_t)(eq - p + 1), ARRAY_SIZE(name)));
        add(name, eq + 1);
    }
    FreeEnvironmentStringsW(env);
}

static void tasks(void (*add)(const WCHAR *, const WCHAR *))
{
    PROCESSENTRY32W pe = { sizeof(pe) };
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    if (Process32FirstW(snap, &pe))
        do
        {
            WCHAR v[64];
            _snwprintf(v, 64, L"Process ID %lu", pe.th32ProcessID);
            add(pe.szExeFile, v);
        } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
}

static void services(void (*add)(const WCHAR *, const WCHAR *))
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    DWORD need = 0, count = 0, resume = 0, i;
    ENUM_SERVICE_STATUS_PROCESSW *e;
    if (!scm) return;
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, NULL, 0, &need, &count, &resume, NULL);
    if ((e = malloc(need + 1024)))
    {
        resume = 0;
        if (EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, (BYTE *)e, need + 1024,
                                  &need, &count, &resume, NULL))
            for (i = 0; i < count; i++)
                add(e[i].lpDisplayName, e[i].ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING ? L"Running" : L"Stopped");
        free(e);
    }
    CloseServiceHandle(scm);
}

static void startup(void (*add)(const WCHAR *, const WCHAR *))
{
    static const struct { HKEY root; const WCHAR *where; } keys[] = {
        { HKEY_LOCAL_MACHINE, L"HKLM" }, { HKEY_CURRENT_USER, L"HKCU" } };
    int k;
    for (k = 0; k < 2; k++)
    {
        HKEY h;
        WCHAR name[256], data[1024];
        DWORD i, nl, dl, type;
        if (RegOpenKeyExW(keys[k].root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &h)) continue;
        for (i = 0; nl = ARRAY_SIZE(name), dl = sizeof(data),
             !RegEnumValueW(h, i, name, &nl, NULL, &type, (BYTE *)data, &dl); i++)
            if (type == REG_SZ || type == REG_EXPAND_SZ) add(name, data);
        RegCloseKey(h);
    }
}

static void drivers(void (*add)(const WCHAR *, const WCHAR *))
{
    int b;
    char seen[8192] = " ";
    for (b = sys_next_block(&g_dev, 0, "DEVICE"); b >= 0; b = sys_next_block(&g_dev, b + 1, "DEVICE"))
    {
        const char *m = sys_field(&g_dev, b, "MODULE"), *d = sys_field(&g_dev, b, "MODULE-DESCRIPTION");
        char key[160];
        WCHAR a[128], v[256];
        if (!m) continue;
        snprintf(key, sizeof(key), " %s ", m);
        if (strstr(seen, key) || strlen(seen) + strlen(key) + 1 >= sizeof(seen)) continue;
        strcat(seen, key + 1);
        utf8_to_w(m, a, 128);
        utf8_to_w(d ? d : "", v, 256);
        add(a, v);
    }
}

/* ---- the categories --------------------------------------------------------------------------- */

typedef struct cat { const WCHAR *path; fill_fn fill; } cat_t;
static const cat_t CATS[] = {
    { L"System Summary", summary },
    { L"Components\\Display", display },
    { L"Components\\Sound Device", sound },
    { L"Components\\Input\\Keyboard", keyboard },
    { L"Components\\Input\\Pointing Device", mouse },
    { L"Components\\Network\\Adapter", network },
    { L"Components\\Storage\\Disks", disks },
    { L"Components\\Storage\\Drives", drives },
    { L"Components\\USB", usb },
    { L"Software Environment\\System Drivers", drivers },
    { L"Software Environment\\Environment Variables", environment },
    { L"Software Environment\\Running Tasks", tasks },
    { L"Software Environment\\Services", services },
    { L"Software Environment\\Startup Programs", startup },
};

static int g_row;
static void add_row(const WCHAR *item, const WCHAR *value)
{
    const WCHAR *cells[2] = { item, value };
    pane_add(++g_row, -1, cells);
}

static void cat_show(node_t *n)
{
    static const WCHAR *const cols[] = { L"Item", L"Value" };
    static const int widths[] = { 260, 560 };
    load();
    pane_columns(cols, widths, 2);
    pane_sort(-1, FALSE);
    pane_begin();
    g_row = 0;
    ((const cat_t *)n->data)->fill(add_row);
    pane_end();
    frame_status(L"%ls", ((const cat_t *)n->data)->path);
}

static const snapin_t cat_ops = { NULL, cat_show };

static node_t *find_or_add(node_t *parent, const WCHAR *name)
{
    node_t *c;
    for (c = parent ? parent->child : NULL; c; c = c->next) if (!wcscmp(c->title, name)) return c;
    return node_add(parent, name, IC_FOLDER, NULL, NULL);
}

node_t *msinfo_create(void)
{
    node_t *root = node_add(NULL, L"System Summary", IC_COMPUTER, &cat_ops, (void *)&CATS[0]);
    node_t *hw = node_add(root, L"Hardware Resources", IC_FOLDER, NULL, NULL);
    int i;
    (void)hw;
    for (i = 1; i < (int)ARRAY_SIZE(CATS); i++)
    {
        WCHAR path[256], *part, *ctx, *next;
        node_t *at = root;
        lstrcpynW(path, CATS[i].path, 256);
        for (part = wcstok_s(path, L"\\", &ctx); part; part = next)
        {
            next = wcstok_s(NULL, L"\\", &ctx);
            if (next) at = find_or_add(at, part);
            else node_add(at, part, IC_LOG, &cat_ops, (void *)&CATS[i]);
        }
    }
    for (i = 0; i < 1; i++) root->expanded = TRUE;
    lstrcpyW(hw->desc, L"Linux assigns the hardware's resources; see Device Manager");
    return root;
}

/* msinfo32 /report FILE: every category, as text */
static FILE *g_rf;
static void report_row(const WCHAR *item, const WCHAR *value)
{
    fwprintf(g_rf, L"%ls\t%ls\n", item, value);
}

int msinfo_report(const WCHAR *path)
{
    int i;
    if (!(g_rf = _wfopen(path, L"w, ccs=UTF-16LE"))) return 1;
    load();
    fwprintf(g_rf, L"System Information report\n\n");
    for (i = 0; i < (int)ARRAY_SIZE(CATS); i++)
    {
        fwprintf(g_rf, L"[%ls]\n\nItem\tValue\n", CATS[i].path);
        CATS[i].fill(report_row);
        fwprintf(g_rf, L"\n");
    }
    fclose(g_rf);
    return 0;
}
