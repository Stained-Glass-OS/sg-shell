/* sg-control -- Settings > Network & Internet: Status, Wi-Fi, Ethernet, Proxy.
 *
 * What the adapters are is read with GetAdaptersAddresses; changing them is
 * Network Connections' (sg-ncpa) and the taskbar flyout's (sg-netflyout),
 * over sg-netctl -- these pages open them rather than repeat them. The proxy
 * is WinINet's, in Internet Settings, where every Windows program reads it.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"
#include <wininet.h>

enum { CMD_ADAPTERS = CMD_PAGE_FIRST + 1, CMD_SHARING, CMD_FLYOUT, CMD_PROPS_FIRST = CMD_PAGE_FIRST + 50 };

static BOOL run_shell(const WCHAR *exe, const WCHAR *args)
{
    WCHAR path[MAX_PATH], *slash;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR cmd[MAX_PATH + 256];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!(slash = wcsrchr(path, L'\\'))) return FALSE;
    lstrcpyW(slash + 1, exe);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return FALSE;
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\"%ls\" %ls", path, args ? args : L"");
    if (!CreateProcessW(path, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return FALSE;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return TRUE;
}

static int adapter_card(int y, const struct adapter *a)
{
    WCHAR sub[300];
    _snwprintf(sub, ARRAYSIZE(sub), L"%ls%ls%ls", a->up ? (a->internet ? L"Connected" : L"Connected, no Internet") : L"Not connected",
               a->ipv4[0] ? L"   IPv4 " : L"", a->ipv4);
    return st_card(y, IC_G_NETWORK, a->name, sub);
}

static void related(int *y)
{
    *y = st_head(*y, L"Advanced network settings");
    st_link(y, L"Change adapter options", CMD_ADAPTERS);
    st_link(y, L"Network and Sharing Center", CMD_SHARING);
}

void set_build_netstatus(void)
{
    struct adapter ad[16];
    int n = load_adapters(ad, 16), y = st_title(L"Status"), i, inet = 0;
    for (i = 0; i < n; i++) if (ad[i].internet) inet++;
    y = st_head(y, L"Network status");
    y = st_card(y, IC_G_PC, inet ? L"You're connected to the Internet" : L"You're not connected",
                inet ? L"If you have a limited data plan, you can make this network a metered connection."
                     : L"Troubleshoot: check the cable or join a Wi-Fi network from the taskbar.");
    for (i = 0; i < n; i++) if (ad[i].up) y = adapter_card(y, &ad[i]);
    related(&y);
}

static void build_type(const WCHAR *title, const WCHAR *type)
{
    struct adapter ad[16];
    int n = load_adapters(ad, 16), y = st_title(title), i, shown = 0;
    for (i = 0; i < n; i++) {
        if (lstrcmpW(ad[i].type, type)) continue;
        y = adapter_card(y, &ad[i]);
        if (ad[i].up) {
            y = st_row(y, L"IPv4 address", ad[i].ipv4[0] ? ad[i].ipv4 : L"-");
            y = st_row(y, L"Default gateway", ad[i].gateway[0] ? ad[i].gateway : L"-");
            y = st_row(y, L"DNS servers", ad[i].dns[0] ? ad[i].dns : L"-");
            y = st_row(y, L"Physical address (MAC)", ad[i].mac);
            if (ad[i].speed[0]) y = st_row(y, L"Link speed", ad[i].speed);
            y += S(8);
        }
        shown++;
    }
    if (!shown) {
        WCHAR line[128];
        _snwprintf(line, ARRAYSIZE(line), L"This PC has no %ls adapter.", type);
        y = st_para(y, line);
    }
    if (!lstrcmpW(type, L"Wi-Fi")) st_button(&y, L"Show available networks", CMD_FLYOUT);
    related(&y);
}

void set_build_wifi(void) { build_type(L"Wi-Fi", L"Wi-Fi"); }
void set_build_ethernet(void) { build_type(L"Ethernet", L"Ethernet"); }

BOOL set_cmd_net(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    switch (id) {
    case CMD_ADAPTERS:
        if (!open_network_connections()) ShellExecuteW(NULL, NULL, L"control.exe", L"ncpa.cpl", NULL, SW_SHOWNORMAL);
        return TRUE;
    case CMD_SHARING:
        ShellExecuteW(NULL, NULL, L"control.exe", L"/name Microsoft.NetworkAndSharingCenter", NULL, SW_SHOWNORMAL);
        return TRUE;
    case CMD_FLYOUT:
        if (!run_shell(L"sg-netflyout64.exe", L"--open")) st_status(L"The network flyout is not installed.");
        return TRUE;
    }
    return FALSE;
}

/* ---- Proxy --------------------------------------------------------------------------------- */
static const WCHAR INET[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
enum { CMD_AUTODETECT = CMD_PAGE_FIRST + 1, CMD_USESCRIPT, CMD_SCRIPT, CMD_USEPROXY, CMD_SERVER, CMD_BYPASS, CMD_LOCAL, CMD_SAVE };
static HWND g_px[8];

void set_build_proxy(void)
{
    WCHAR server[512] = L"", bypass[1024] = L"", script[512] = L"", *local;
    int y = st_title(L"Proxy");
    reg_sz(HKEY_CURRENT_USER, INET, L"ProxyServer", server, ARRAYSIZE(server));
    reg_sz(HKEY_CURRENT_USER, INET, L"ProxyOverride", bypass, ARRAYSIZE(bypass));
    reg_sz(HKEY_CURRENT_USER, INET, L"AutoConfigURL", script, ARRAYSIZE(script));
    /* "<local>" in the list is the "don't use the proxy for local addresses" box */
    if ((local = wcsstr(bypass, L"<local>"))) {
        WCHAR *end = local + 7;
        if (*end == L';') end++;
        memmove(local, end, (lstrlenW(end) + 1) * sizeof(WCHAR));
        if (local > bypass && local[-1] == L';' && !*local) local[-1] = 0;
    }
    y = st_head(y, L"Automatic proxy setup");
    g_px[0] = st_toggle(&y, L"Automatically detect settings", reg_dword(HKEY_CURRENT_USER, INET, L"AutoDetect", 0) != 0, CMD_AUTODETECT);
    g_px[1] = st_toggle(&y, L"Use setup script", script[0] != 0, CMD_USESCRIPT);
    g_px[2] = st_edit(&y, L"Script address", script, CMD_SCRIPT);
    y = st_head(y, L"Manual proxy setup");
    g_px[3] = st_toggle(&y, L"Use a proxy server", reg_dword(HKEY_CURRENT_USER, INET, L"ProxyEnable", 0) != 0, CMD_USEPROXY);
    g_px[4] = st_edit(&y, L"Address:Port", server, CMD_SERVER);
    g_px[5] = st_edit(&y, L"Use the proxy server except for addresses that start with the following entries. "
                          L"Use semicolons (;) to separate entries.", bypass, CMD_BYPASS);
    st_checkbox(&y, L"Don't use the proxy server for local (intranet) addresses", wcsstr(bypass, L"<local>") != NULL || local != NULL, CMD_LOCAL);
    g_px[6] = GetDlgItem(g_page, CMD_LOCAL);
    st_button(&y, L"Save", CMD_SAVE);
}

static void save_proxy(void)
{
    WCHAR server[512], bypass[1100], script[512];
    GetWindowTextW(g_px[4], server, ARRAYSIZE(server));
    GetWindowTextW(g_px[5], bypass, 1024);
    GetWindowTextW(g_px[2], script, ARRAYSIZE(script));
    if (st_checked(g_px[6])) { if (bypass[0]) lstrcatW(bypass, L";"); lstrcatW(bypass, L"<local>"); }
    reg_set_dword(HKEY_CURRENT_USER, INET, L"ProxyEnable", st_checked(g_px[3]));
    reg_set_sz(HKEY_CURRENT_USER, INET, L"ProxyServer", server);
    reg_set_sz(HKEY_CURRENT_USER, INET, L"ProxyOverride", bypass);
    reg_set_dword(HKEY_CURRENT_USER, INET, L"AutoDetect", st_checked(g_px[0]));
    if (st_checked(g_px[1]) && script[0]) reg_set_sz(HKEY_CURRENT_USER, INET, L"AutoConfigURL", script);
    else {
        HKEY k;
        if (!RegOpenKeyExW(HKEY_CURRENT_USER, INET, 0, KEY_SET_VALUE, &k)) { RegDeleteValueW(k, L"AutoConfigURL"); RegCloseKey(k); }
    }
    /* tell WinINet, as Internet Options does */
    InternetSetOptionW(NULL, INTERNET_OPTION_SETTINGS_CHANGED, NULL, 0);
    InternetSetOptionW(NULL, INTERNET_OPTION_REFRESH, NULL, 0);
}

BOOL set_cmd_proxy(int id, int code, HWND ctl)
{
    (void)ctl;
    switch (id) {
    case CMD_AUTODETECT: case CMD_USESCRIPT: case CMD_USEPROXY: case CMD_LOCAL: save_proxy(); return TRUE;
    case CMD_SAVE: save_proxy(); st_status(L"Saved."); return TRUE;
    case CMD_SCRIPT: case CMD_SERVER: case CMD_BYPASS: (void)code; return TRUE;
    }
    return FALSE;
}
