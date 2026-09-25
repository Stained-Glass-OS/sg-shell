/* sg-control -- Network and Sharing Center: the machine's networks, read
 * from the adapters' live state. Changing an adapter's settings is Network
 * Connections' job (sg-ncpa); this page shows and links.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "control.h"

enum { CMD_ADAPTERS = CMD_PAGE_FIRST + 1, CMD_INET = CMD_PAGE_FIRST + 2 };


static void addr_string(SOCKADDR *sa, WCHAR *out, int cch)
{
    out[0] = 0;
    if (sa->sa_family == AF_INET) InetNtopW(AF_INET, &((SOCKADDR_IN *)sa)->sin_addr, out, cch);
    else if (sa->sa_family == AF_INET6) InetNtopW(AF_INET6, &((SOCKADDR_IN6 *)sa)->sin6_addr, out, cch);
}

static void append(WCHAR *list, int cch, const WCHAR *item)
{
    if (!item[0]) return;
    if (list[0]) wcsncat(list, L", ", cch - lstrlenW(list) - 1);
    wcsncat(list, item, cch - lstrlenW(list) - 1);
}

int load_adapters(struct adapter *out, int max)
{
    ULONG size = 32768;
    IP_ADAPTER_ADDRESSES *buf = NULL, *a;
    int n = 0;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    while (r == ERROR_BUFFER_OVERFLOW) {
        free(buf);
        if (!(buf = malloc(size))) return 0;
        r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS, NULL, buf, &size);
    }
    if (r != ERROR_SUCCESS) { free(buf); return 0; }
    for (a = buf; a && n < max; a = a->Next) {
        struct adapter *d = &out[n];
        IP_ADAPTER_UNICAST_ADDRESS *u;
        IP_ADAPTER_GATEWAY_ADDRESS_LH *g;
        IP_ADAPTER_DNS_SERVER_ADDRESS *s;
        WCHAR t[64];
        ULONG i;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        memset(d, 0, sizeof(*d));
        lstrcpynW(d->name, a->FriendlyName ? a->FriendlyName : L"", ARRAYSIZE(d->name));
        lstrcpynW(d->desc, a->Description ? a->Description : L"", ARRAYSIZE(d->desc));
        lstrcpyW(d->type, a->IfType == IF_TYPE_IEEE80211 ? L"Wi-Fi" : a->IfType == IF_TYPE_ETHERNET_CSMACD ? L"Ethernet"
                          : a->IfType == IF_TYPE_TUNNEL ? L"Tunnel" : a->IfType == IF_TYPE_PPP ? L"Dial-up" : L"Network");
        d->up = a->OperStatus == IfOperStatusUp;
        for (u = a->FirstUnicastAddress; u; u = u->Next) {
            addr_string(u->Address.lpSockaddr, t, ARRAYSIZE(t));
            if (u->Address.lpSockaddr->sa_family == AF_INET) {
                WCHAR w[80];
                _snwprintf(w, ARRAYSIZE(w), L"%ls/%u", t, u->OnLinkPrefixLength);
                append(d->ipv4, ARRAYSIZE(d->ipv4), w);
            } else append(d->ipv6, ARRAYSIZE(d->ipv6), t);
        }
        for (g = a->FirstGatewayAddress; g; g = g->Next) {
            addr_string(g->Address.lpSockaddr, t, ARRAYSIZE(t));
            append(d->gateway, ARRAYSIZE(d->gateway), t);
        }
        for (s = a->FirstDnsServerAddress; s; s = s->Next) {
            addr_string(s->Address.lpSockaddr, t, ARRAYSIZE(t));
            append(d->dns, ARRAYSIZE(d->dns), t);
        }
        for (i = 0; i < a->PhysicalAddressLength && i < 8; i++)
            _snwprintf(d->mac + i * 3, 4, i + 1 < a->PhysicalAddressLength ? L"%02X-" : L"%02X", a->PhysicalAddress[i]);
        if (a->TransmitLinkSpeed && a->TransmitLinkSpeed != (ULONG64)-1) {
            if (a->TransmitLinkSpeed >= 1000000000ULL) _snwprintf(d->speed, ARRAYSIZE(d->speed), L"%.1f Gbps", a->TransmitLinkSpeed / 1e9);
            else _snwprintf(d->speed, ARRAYSIZE(d->speed), L"%.0f Mbps", a->TransmitLinkSpeed / 1e6);
        }
        d->internet = d->up && d->gateway[0];
        n++;
    }
    free(buf);
    return n;
}

static int kv(int x, int y, int w, const WCHAR *k, const WCHAR *v)
{
    if (!v[0]) return y;
    pg_text(x, y, S(150), S(20), g_font_body, COL_SUBTLE, k, DT_SINGLELINE);
    pg_text(x + S(150), y, w - S(150), S(20), g_font_body, COL_TEXT, v, DT_SINGLELINE | DT_END_ELLIPSIS);
    return y + S(22);
}

void build_network(void)
{
    static const WCHAR *const labels[] = { L"Change adapter settings", NULL, L"See also", L"Internet Options", L"System" };
    static const int ids[] = { CMD_ADAPTERS, 0, -1, CMD_INET, NAV(PG_SYSTEM) };
    struct adapter ad[16];
    int x = pg_left_pane(labels, ids, ARRAYSIZE(labels)) + S(36), y = S(24), w = pg_width() - x - S(40), n, i, shown = 0;
    WCHAR host[256], line[300];
    DWORD hn = ARRAYSIZE(host);

    n = load_adapters(ad, ARRAYSIZE(ad));
    pg_title(x, y, L"View your basic network information and set up connections");
    y += S(48);
    if (!GetComputerNameExW(ComputerNameDnsFullyQualified, host, &hn)) lstrcpyW(host, L"(unknown)");
    _snwprintf(line, ARRAYSIZE(line), L"This computer: %ls", host);
    pg_text(x, y, w, S(20), g_font_body, COL_SUBTLE, line, DT_SINGLELINE | DT_END_ELLIPSIS);
    y += S(32);

    pg_text(x, y, w, S(24), g_font_cat, COL_TITLE, L"View your active networks", DT_SINGLELINE);
    pg_rule(x, y + S(26), w);
    y += S(40);
    for (i = 0; i < n; i++) {
        if (!ad[i].up) continue;
        pg_icon(x, y, S(48), IC_NETCENTER);
        pg_text(x + S(64), y, w - S(64), S(22), g_font_head, COL_TEXT, ad[i].name, DT_SINGLELINE | DT_END_ELLIPSIS);
        _snwprintf(line, ARRAYSIZE(line), L"%ls  \x2022  %ls", ad[i].type, ad[i].internet ? L"Access type: Internet" : L"Access type: No Internet access");
        pg_text(x + S(64), y + S(22), w - S(64), S(20), g_font_body, ad[i].internet ? COL_OK : COL_SUBTLE, line, DT_SINGLELINE);
        y += S(50);
        y = kv(x + S(64), y, w - S(64), L"Adapter:", ad[i].desc);
        y = kv(x + S(64), y, w - S(64), L"IPv4 address:", ad[i].ipv4);
        y = kv(x + S(64), y, w - S(64), L"IPv6 address:", ad[i].ipv6);
        y = kv(x + S(64), y, w - S(64), L"Default gateway:", ad[i].gateway);
        y = kv(x + S(64), y, w - S(64), L"DNS servers:", ad[i].dns);
        y = kv(x + S(64), y, w - S(64), L"Physical address:", ad[i].mac);
        y = kv(x + S(64), y, w - S(64), L"Speed:", ad[i].speed);
        y += S(16);
        shown++;
    }
    if (!shown) { pg_text(x, y, w, S(20), g_font_body, COL_SUBTLE, L"You are currently not connected to any networks.", DT_SINGLELINE); y += S(30); }

    for (i = 0, shown = 0; i < n; i++) {
        if (ad[i].up) continue;
        if (!shown++) {
            y += S(8);
            pg_text(x, y, w, S(24), g_font_cat, COL_TITLE, L"Not connected", DT_SINGLELINE);
            pg_rule(x, y + S(26), w);
            y += S(40);
        }
        _snwprintf(line, ARRAYSIZE(line), L"%ls  (%ls, %ls)", ad[i].name, ad[i].type, ad[i].desc);
        pg_text(x + S(16), y, w - S(16), S(20), g_font_body, COL_SUBTLE, line, DT_SINGLELINE | DT_END_ELLIPSIS);
        y += S(24);
    }
    y += S(16);
    pg_link(x, y, L"Change adapter settings", CMD_ADAPTERS, 0);
}

BOOL cmd_network(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    switch (id) {
    case CMD_ADAPTERS:
        if (!open_network_connections())
            message(g_main, L"Network Connections", L"Network Connections is not installed on this computer.", FALSE);
        return TRUE;
    case CMD_INET: cpl_open_file(L"inetcpl.cpl", NULL); return TRUE;
    }
    return FALSE;
}

void dump_network(void)
{
    struct adapter ad[16];
    int n = load_adapters(ad, ARRAYSIZE(ad)), i;
    wprintf(L"network.adapters=%d\n", n);
    for (i = 0; i < n; i++)
        wprintf(L"adapter=%ls|%ls|%ls|%ls|%ls|%ls|%ls\n", ad[i].name, ad[i].type, ad[i].up ? L"up" : L"down",
                ad[i].ipv4, ad[i].gateway, ad[i].dns, ad[i].mac);
}
