/* sg-ncpa -- Network Connections (ncpa.cpl) for Stained Glass OS.
 *
 * The window administrators expect: one tile per adapter with its status, and
 * behind it the classic dialogs -- Status, Network Connection Details,
 * Properties, and "Internet Protocol Version 4 (TCP/IPv4) Properties" with
 * "Obtain an IP address automatically" / "Use the following IP address". The
 * data and every change go through sg-netctl (see sg-netclient.h), which lets
 * only administrators change an adapter's settings; this program decides
 * nothing and says so plainly when sg-netd refuses.
 *
 *   sg-ncpa [--open status|details|props|ipv4|ipv6 DEVICE]
 *   sg-ncpa --dump                       what the window would show (stderr)
 *   sg-ncpa --set-ipv4 DEVICE auto|static IP MASK GATEWAY DNS1 DNS2
 *                                        what the IPv4 dialog's OK does, with
 *                                        "-" for an empty field (for gates)
 *
 * `control ncpa.cpl` and `ncpa.cpl` open it (defaults/63-sg-network.reg).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <stdio.h>
#include "sg-netclient.h"

#define MAX_ADAPTERS 16
#define TILE_W 300
#define TILE_H 76
#define BAR_H 34

#define COL_BG       RGB(0xFF, 0xFF, 0xFF)
#define COL_BAR      RGB(0xF5, 0xF6, 0xF7)
#define COL_BAR_LINE RGB(0xE5, 0xE5, 0xE5)
#define COL_TEXT     RGB(0x00, 0x00, 0x00)
#define COL_SUBTLE   RGB(0x6D, 0x6D, 0x6D)
#define COL_LINK     RGB(0x00, 0x66, 0xCC)
#define COL_SEL      RGB(0xCC, 0xE8, 0xFF)
#define COL_SEL_LINE RGB(0x99, 0xD1, 0xFF)
#define COL_HOT      RGB(0xE5, 0xF3, 0xFF)

struct adapter {
    char dev[32], type[24], state[24], mac[32], conn[128], uuid[64], driver[64];
    char v4method[16], v6method[16], v4gw[48], v6gw[64], suffix[128];
    char v4addr[8][48], v6addr[8][64], v4dns[4][48], v6dns[4][64];
    int nv4, nv6, nv4dns, nv6dns, dns_auto;
    char dhcp_server[48], ssid[128];
    long long obtained, expires, speed, rx, tx;
    WCHAR name[64];         /* "Ethernet", "Ethernet 2", "Wi-Fi" */
};

static struct adapter g_ad[MAX_ADAPTERS];
static int g_nad, g_sel = -1, g_hot = -1, g_hot_cmd = -1;
static BOOL g_is_admin, g_refreshing;
static HWND g_main;
static HFONT g_font, g_font_bold, g_font_bar;
static WCHAR g_status[256];

/* --- the data ------------------------------------------------------------------- */

static void copy_field(char *dst, size_t cap, const char *src)
{
    snprintf(dst, cap, "%s", src ? src : "");
}

static int parse_adapters(const struct net_reply *r, struct adapter *out, int max)
{
    int i, n = 0, eth = 0, wifi = 0;
    struct adapter *a = NULL;
    for (i = 0; i < r->nlines; i++)
    {
        const char *l = r->lines[i], *v = strchr(l, ' ');
        char key[32];
        size_t kl;
        if (!v) { if (!strcmp(l, "END")) a = NULL; continue; }
        kl = (size_t)(v - l);
        if (kl >= sizeof(key)) continue;
        memcpy(key, l, kl); key[kl] = 0; v++;
        if (!strcmp(key, "ADAPTER"))
        {
            if (n >= max) break;
            a = &out[n++];
            memset(a, 0, sizeof(*a));
            a->dns_auto = 1;
            copy_field(a->dev, sizeof(a->dev), v);
            continue;
        }
        if (!a) continue;
        if (!strcmp(key, "TYPE")) copy_field(a->type, sizeof(a->type), v);
        else if (!strcmp(key, "STATE")) copy_field(a->state, sizeof(a->state), v);
        else if (!strcmp(key, "MAC")) copy_field(a->mac, sizeof(a->mac), v);
        else if (!strcmp(key, "CONNECTION")) copy_field(a->conn, sizeof(a->conn), v);
        else if (!strcmp(key, "CONNECTION-UUID")) copy_field(a->uuid, sizeof(a->uuid), v);
        else if (!strcmp(key, "DRIVER")) copy_field(a->driver, sizeof(a->driver), v);
        else if (!strcmp(key, "IPV4-METHOD")) copy_field(a->v4method, sizeof(a->v4method), v);
        else if (!strcmp(key, "IPV6-METHOD")) copy_field(a->v6method, sizeof(a->v6method), v);
        else if (!strcmp(key, "IPV4-DNS-AUTO")) a->dns_auto = strcmp(v, "no") != 0;
        else if (!strcmp(key, "IPV4-ADDRESS") && a->nv4 < 8) copy_field(a->v4addr[a->nv4++], 48, v);
        else if (!strcmp(key, "IPV6-ADDRESS") && a->nv6 < 8) copy_field(a->v6addr[a->nv6++], 64, v);
        else if (!strcmp(key, "IPV4-DNS") && a->nv4dns < 4) copy_field(a->v4dns[a->nv4dns++], 48, v);
        else if (!strcmp(key, "IPV6-DNS") && a->nv6dns < 4) copy_field(a->v6dns[a->nv6dns++], 64, v);
        else if (!strcmp(key, "IPV4-GATEWAY")) copy_field(a->v4gw, sizeof(a->v4gw), v);
        else if (!strcmp(key, "IPV6-GATEWAY")) copy_field(a->v6gw, sizeof(a->v6gw), v);
        else if (!strcmp(key, "DNS-SUFFIX")) copy_field(a->suffix, sizeof(a->suffix), v);
        else if (!strcmp(key, "DHCP4-SERVER")) copy_field(a->dhcp_server, sizeof(a->dhcp_server), v);
        else if (!strcmp(key, "DHCP4-OBTAINED")) a->obtained = _atoi64(v);
        else if (!strcmp(key, "DHCP4-EXPIRES")) a->expires = _atoi64(v);
        else if (!strcmp(key, "SPEED")) a->speed = _atoi64(v);
        else if (!strcmp(key, "RX-BYTES")) a->rx = _atoi64(v);
        else if (!strcmp(key, "TX-BYTES")) a->tx = _atoi64(v);
        else if (!strcmp(key, "SSID")) copy_field(a->ssid, sizeof(a->ssid), v);
    }
    /* Windows' names: Ethernet, Ethernet 2, ..., Wi-Fi, Wi-Fi 2 */
    for (i = 0; i < n; i++)
    {
        struct adapter *x = &out[i];
        if (!strcmp(x->type, "wifi"))
            wifi++ ? _snwprintf(x->name, 64, L"Wi-Fi %d", wifi) : _snwprintf(x->name, 64, L"Wi-Fi");
        else if (!strcmp(x->type, "ethernet"))
            eth++ ? _snwprintf(x->name, 64, L"Ethernet %d", eth) : _snwprintf(x->name, 64, L"Ethernet");
        else
        {
            WCHAR t[24];
            utf8_to_w(x->dev, t, 24);
            _snwprintf(x->name, 64, L"%ls", t);
        }
        x->name[63] = 0;
    }
    return n;
}

static BOOL is_connected(const struct adapter *a) { return !strcmp(a->state, "connected"); }
static BOOL is_wifi(const struct adapter *a) { return !strcmp(a->type, "wifi"); }

static void status_text(const struct adapter *a, WCHAR *out, int cch)
{
    WCHAR t[128];
    if (is_connected(a))
    {
        if (is_wifi(a) && a->ssid[0]) { utf8_to_w(a->ssid, t, 128); _snwprintf(out, cch, L"%ls", t); }
        else if (a->suffix[0]) { utf8_to_w(a->suffix, t, 128); _snwprintf(out, cch, L"%ls", t); }
        else _snwprintf(out, cch, L"Network");
    }
    else if (!strcmp(a->state, "connecting")) _snwprintf(out, cch, L"Identifying...");
    else if (!strcmp(a->state, "unavailable"))
        _snwprintf(out, cch, is_wifi(a) ? L"Not connected" : L"Network cable unplugged");
    else if (!strcmp(a->state, "unmanaged")) _snwprintf(out, cch, L"Managed outside Stained Glass");
    else _snwprintf(out, cch, L"Not connected");
    out[cch - 1] = 0;
}

static void desc_text(const struct adapter *a, WCHAR *out, int cch)
{
    WCHAR drv[64], dev[32];
    utf8_to_w(a->driver[0] ? a->driver : "Network", drv, 64);
    utf8_to_w(a->dev, dev, 32);
    _snwprintf(out, cch, L"%ls %ls adapter (%ls)", drv, is_wifi(a) ? L"wireless" : L"Ethernet", dev);
    out[cch - 1] = 0;
}

static int load_adapters(BOOL pumped)
{
    struct net_reply r;
    const char *argv[] = { "adapters" };
    if (pumped) net_request_pumped(1, argv, NULL, &r);
    else net_request_argv(1, argv, NULL, &r);
    if (r.ok) g_nad = parse_adapters(&r, g_ad, MAX_ADAPTERS);
    else utf8_to_w(r.message, g_status, 256);
    net_reply_free(&r);
    return r.ok;
}

static void load_whoami(void)
{
    struct net_reply r;
    const char *v;
    net_request(&r, NULL, "whoami", NULL);
    v = net_field(&r, 0, "ADMIN", 0);
    g_is_admin = v && !strcmp(v, "yes");
    net_reply_free(&r);
}

static int find_adapter(const char *dev)
{
    int i;
    for (i = 0; i < g_nad; i++) if (!strcmp(g_ad[i].dev, dev)) return i;
    return -1;
}

/* --- addresses ------------------------------------------------------------------ */

static void ip4_str(DWORD ip, char *out, size_t cap)
{
    snprintf(out, cap, "%lu.%lu.%lu.%lu", (ip >> 24) & 255, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

static BOOL parse_ip4(const char *s, DWORD *ip)
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return FALSE;
    *ip = (a << 24) | (b << 16) | (c << 8) | d;
    return TRUE;
}

/* The prefix length of a subnet mask; -1 if it is not a mask. */
static int mask_prefix(DWORD mask)
{
    int p = 0;
    while (p < 32 && (mask & (0x80000000u >> p))) p++;
    if (p < 32 && (mask << p)) return -1;
    return p;
}

static DWORD prefix_mask(int p) { return p <= 0 ? 0 : p >= 32 ? 0xFFFFFFFFu : 0xFFFFFFFFu << (32 - p); }

/* What the IPv4 dialog's OK does. Returns 0 on success, else an error in err
 * (and the sg-netctl exit kind: 2 invalid, 3 denied, 1 other). ip/mask/gw/dns
 * of 0 are empty fields. */
static int apply_ipv4(const char *dev, BOOL automatic, BOOL dns_auto, DWORD ip, DWORD mask, DWORD gw,
                      DWORD dns1, DWORD dns2, WCHAR *err, int cch, BOOL pumped)
{
    const char *argv[12];
    char addr[48], gws[24], dns[64], d1[24], d2[24];
    struct net_reply r;
    int argc = 0, prefix;

    dns[0] = 0;
    if (dns1) { ip4_str(dns1, d1, sizeof(d1)); strcat(dns, d1); }
    if (dns2) { ip4_str(dns2, d2, sizeof(d2)); if (dns[0]) strcat(dns, ","); strcat(dns, d2); }
    argv[argc++] = "ipv4";
    argv[argc++] = dev;
    if (automatic)
    {
        argv[argc++] = "dhcp";
        if (!dns_auto && dns[0]) { argv[argc++] = "--dns"; argv[argc++] = dns; }
    }
    else
    {
        if (!ip)
        {
            _snwprintf(err, cch, L"The adapter requires at least one IP address. Please enter one.");
            return 2;
        }
        if (!mask)
        {
            _snwprintf(err, cch, L"You must enter a subnet mask.");
            return 2;
        }
        if ((prefix = mask_prefix(mask)) < 1)
        {
            _snwprintf(err, cch, L"The subnet mask entered is not valid. Please enter a valid mask.");
            return 2;
        }
        if (gw && ((gw & mask) != (ip & mask)))
        {
            _snwprintf(err, cch, L"The default gateway is not on the same network segment (subnet) that is "
                                 L"defined by the IP address and subnet mask.");
            return 2;
        }
        ip4_str(ip, addr, sizeof(addr));
        snprintf(addr + strlen(addr), sizeof(addr) - strlen(addr), "/%d", prefix);
        argv[argc++] = "static";
        argv[argc++] = addr;
        if (gw) { ip4_str(gw, gws, sizeof(gws)); argv[argc++] = "--gateway"; argv[argc++] = gws; }
        if (dns[0]) { argv[argc++] = "--dns"; argv[argc++] = dns; }
    }
    if (pumped) net_request_pumped(argc, argv, NULL, &r);
    else net_request_argv(argc, argv, NULL, &r);
    net_reply_free(&r);
    if (r.ok) return 0;
    net_error_text(&r, err, cch);
    return !strcmp(r.kind, "denied") ? 3 : !strcmp(r.kind, "invalid") ? 2 : 1;
}

/* --- dialogs, built in memory ------------------------------------------------------- */

struct dlg {
    WORD buf[8192];
    WORD *p;
    DLGTEMPLATE *t;
};

static void dlg_str(struct dlg *d, const WCHAR *s)
{
    while (*s) *d->p++ = *s++;
    *d->p++ = 0;
}

static void dlg_begin(struct dlg *d, const WCHAR *title, short cx, short cy)
{
    memset(d, 0, sizeof(*d));
    d->t = (DLGTEMPLATE *)d->buf;
    d->t->style = DS_SHELLFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    d->t->cx = cx;
    d->t->cy = cy;
    d->p = (WORD *)(d->t + 1);
    *d->p++ = 0;            /* no menu */
    *d->p++ = 0;            /* the default class */
    dlg_str(d, title);
    *d->p++ = 8;            /* point size */
    dlg_str(d, L"MS Shell Dlg 2");
}

/* cls: a class name, or one of the predefined atoms below. */
#define ATOM_BUTTON 0x80
#define ATOM_EDIT   0x81
#define ATOM_STATIC 0x82
static void dlg_item(struct dlg *d, const WCHAR *cls, WORD atom, const WCHAR *text, WORD id, DWORD style,
                     short x, short y, short cx, short cy)
{
    DLGITEMTEMPLATE *it;
    d->p = (WORD *)(((ULONG_PTR)d->p + 3) & ~(ULONG_PTR)3);
    it = (DLGITEMTEMPLATE *)d->p;
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy;
    it->id = id;
    d->p = (WORD *)(it + 1);
    if (cls) dlg_str(d, cls);
    else { *d->p++ = 0xFFFF; *d->p++ = atom; }
    dlg_str(d, text ? text : L"");
    *d->p++ = 0;            /* no creation data */
    d->t->cdit++;
}

#define LABEL(d, text, x, y, cx) dlg_item(d, NULL, ATOM_STATIC, text, 0xFFFF, SS_LEFT, x, y, cx, 8)
#define VALUE(d, id, x, y, cx) dlg_item(d, NULL, ATOM_STATIC, L"", id, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, x, y, cx, 8)
#define BUTTON(d, text, id, x, y, cx) dlg_item(d, NULL, ATOM_BUTTON, text, id, BS_PUSHBUTTON | WS_TABSTOP, x, y, cx, 14)
#define GROUP(d, text, x, y, cx, cy) dlg_item(d, NULL, ATOM_BUTTON, text, 0xFFFF, BS_GROUPBOX, x, y, cx, cy)
#define RADIO(d, text, id, x, y, cx, grp) dlg_item(d, NULL, ATOM_BUTTON, text, id, BS_RADIOBUTTON | WS_TABSTOP | ((grp) ? WS_GROUP : 0), x, y, cx, 10)
#define IPBOX(d, id, x, y) dlg_item(d, WC_IPADDRESSW, 0, L"", id, WS_TABSTOP, x, y, 95, 13)
#define EDIT(d, id, x, y, cx) dlg_item(d, NULL, ATOM_EDIT, L"", id, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, x, y, cx, 13)

/* --- the IPv4 dialog ------------------------------------------------------------------ */

enum { ID_V4_AUTO = 101, ID_V4_STATIC, ID_DNS_AUTO, ID_DNS_STATIC, ID_V4_IP = 110, ID_V4_MASK, ID_V4_GW,
       ID_V4_DNS1, ID_V4_DNS2, ID_NOTE = 120, ID_TAB = 130 };

static void v4_enable(HWND dlg)
{
    BOOL fixed = IsDlgButtonChecked(dlg, ID_V4_STATIC) == BST_CHECKED;
    BOOL dns_fixed;
    int id;
    if (fixed)
    {
        /* A fixed address needs fixed DNS servers, as on Windows. */
        CheckRadioButton(dlg, ID_DNS_AUTO, ID_DNS_STATIC, ID_DNS_STATIC);
    }
    EnableWindow(GetDlgItem(dlg, ID_DNS_AUTO), !fixed);
    dns_fixed = IsDlgButtonChecked(dlg, ID_DNS_STATIC) == BST_CHECKED;
    for (id = ID_V4_IP; id <= ID_V4_GW; id++) EnableWindow(GetDlgItem(dlg, id), fixed);
    for (id = ID_V4_DNS1; id <= ID_V4_DNS2; id++) EnableWindow(GetDlgItem(dlg, id), dns_fixed);
}

static void ipbox_set(HWND dlg, int id, const char *text)
{
    DWORD ip;
    char buf[64];
    char *slash;
    snprintf(buf, sizeof(buf), "%s", text ? text : "");
    if ((slash = strchr(buf, '/'))) *slash = 0;
    if (buf[0] && parse_ip4(buf, &ip)) SendDlgItemMessageW(dlg, id, IPM_SETADDRESS, 0, ip);
    else SendDlgItemMessageW(dlg, id, IPM_CLEARADDRESS, 0, 0);
}

static DWORD ipbox_get(HWND dlg, int id)
{
    DWORD ip = 0;
    if (SendDlgItemMessageW(dlg, id, IPM_ISBLANK, 0, 0)) return 0;
    SendDlgItemMessageW(dlg, id, IPM_GETADDRESS, 0, (LPARAM)&ip);
    return ip;
}

static INT_PTR CALLBACK ipv4_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    struct adapter *a = (struct adapter *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        TCITEMW tab = { TCIF_TEXT };
        BOOL automatic;
        a = (struct adapter *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        tab.pszText = (WCHAR *)L"General";
        SendDlgItemMessageW(dlg, ID_TAB, TCM_INSERTITEMW, 0, (LPARAM)&tab);
        EnableThemeDialogTexture(dlg, ETDT_ENABLETAB);
        automatic = strcmp(a->v4method, "manual") != 0;
        CheckRadioButton(dlg, ID_V4_AUTO, ID_V4_STATIC, automatic ? ID_V4_AUTO : ID_V4_STATIC);
        CheckRadioButton(dlg, ID_DNS_AUTO, ID_DNS_STATIC, a->dns_auto && automatic ? ID_DNS_AUTO : ID_DNS_STATIC);
        if (!automatic && a->nv4)
        {
            char *slash = strchr(a->v4addr[0], '/');
            ipbox_set(dlg, ID_V4_IP, a->v4addr[0]);
            if (slash) SendDlgItemMessageW(dlg, ID_V4_MASK, IPM_SETADDRESS, 0, prefix_mask(atoi(slash + 1)));
            ipbox_set(dlg, ID_V4_GW, a->v4gw);
        }
        if (!(a->dns_auto && automatic))
        {
            ipbox_set(dlg, ID_V4_DNS1, a->nv4dns > 0 ? a->v4dns[0] : "");
            ipbox_set(dlg, ID_V4_DNS2, a->nv4dns > 1 ? a->v4dns[1] : "");
        }
        if (!g_is_admin)
        {
            SetDlgItemTextW(dlg, ID_NOTE, L"Changing these settings requires an administrator.");
            SendDlgItemMessageW(dlg, IDOK, BCM_SETSHIELD, 0, TRUE);
        }
        v4_enable(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_V4_AUTO: case ID_V4_STATIC:
            CheckRadioButton(dlg, ID_V4_AUTO, ID_V4_STATIC, LOWORD(wp));
            v4_enable(dlg);
            return TRUE;
        case ID_DNS_AUTO: case ID_DNS_STATIC:
            CheckRadioButton(dlg, ID_DNS_AUTO, ID_DNS_STATIC, LOWORD(wp));
            v4_enable(dlg);
            return TRUE;
        case ID_V4_IP:
            /* Leaving the address with no mask fills in its class's mask, as Windows does. */
            if (HIWORD(wp) == EN_KILLFOCUS && SendDlgItemMessageW(dlg, ID_V4_MASK, IPM_ISBLANK, 0, 0))
            {
                DWORD ip = ipbox_get(dlg, ID_V4_IP), first = ip >> 24;
                if (ip && first < 224)
                    SendDlgItemMessageW(dlg, ID_V4_MASK, IPM_SETADDRESS, 0,
                                        first < 128 ? 0xFF000000u : first < 192 ? 0xFFFF0000u : 0xFFFFFF00u);
            }
            return TRUE;
        case IDOK:
        {
            WCHAR err[512];
            HCURSOR old;
            int rc;
            if (!a) { EndDialog(dlg, IDCANCEL); return TRUE; }
            SetDlgItemTextW(dlg, ID_NOTE, L"Applying the settings...");
            EnableWindow(GetDlgItem(dlg, IDOK), FALSE);
            old = SetCursor(LoadCursorW(NULL, (const WCHAR *)IDC_WAIT));
            rc = apply_ipv4(a->dev, IsDlgButtonChecked(dlg, ID_V4_AUTO) == BST_CHECKED,
                            IsDlgButtonChecked(dlg, ID_DNS_AUTO) == BST_CHECKED,
                            ipbox_get(dlg, ID_V4_IP), ipbox_get(dlg, ID_V4_MASK), ipbox_get(dlg, ID_V4_GW),
                            ipbox_get(dlg, ID_V4_DNS1), ipbox_get(dlg, ID_V4_DNS2), err, 512, TRUE);
            SetCursor(old);
            EnableWindow(GetDlgItem(dlg, IDOK), TRUE);
            SetDlgItemTextW(dlg, ID_NOTE, g_is_admin ? L"" : L"Changing these settings requires an administrator.");
            if (rc)
            {
                MessageBoxW(dlg, err, L"Microsoft TCP/IP", MB_OK | (rc == 3 ? MB_ICONWARNING : MB_ICONERROR));
                return TRUE;
            }
            EndDialog(dlg, IDOK);
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

static INT_PTR show_ipv4(HWND owner, struct adapter *a)
{
    static struct dlg d;
    dlg_begin(&d, L"Internet Protocol Version 4 (TCP/IPv4) Properties", 263, 276);
    dlg_item(&d, WC_TABCONTROLW, 0, L"", ID_TAB, WS_CLIPSIBLINGS, 6, 6, 251, 243);
    dlg_item(&d, NULL, ATOM_STATIC, L"You can get IP settings assigned automatically if your network supports "
             L"this capability. Otherwise, you need to ask your network administrator for the appropriate IP "
             L"settings.", 0xFFFF, SS_LEFT, 14, 26, 235, 26);
    RADIO(&d, L"&Obtain an IP address automatically", ID_V4_AUTO, 14, 57, 200, TRUE);
    GROUP(&d, L"", 13, 72, 236, 63);
    RADIO(&d, L"U&se the following IP address:", ID_V4_STATIC, 19, 71, 125, FALSE);
    LABEL(&d, L"&IP address:", 21, 88, 100);
    IPBOX(&d, ID_V4_IP, 146, 86);
    LABEL(&d, L"S&ubnet mask:", 21, 104, 100);
    IPBOX(&d, ID_V4_MASK, 146, 102);
    LABEL(&d, L"&Default gateway:", 21, 120, 100);
    IPBOX(&d, ID_V4_GW, 146, 118);
    RADIO(&d, L"O&btain DNS server address automatically", ID_DNS_AUTO, 14, 143, 200, TRUE);
    GROUP(&d, L"", 13, 158, 236, 47);
    RADIO(&d, L"Us&e the following DNS server addresses:", ID_DNS_STATIC, 19, 157, 150, FALSE);
    LABEL(&d, L"&Preferred DNS server:", 21, 174, 110);
    IPBOX(&d, ID_V4_DNS1, 146, 172);
    LABEL(&d, L"&Alternate DNS server:", 21, 190, 110);
    IPBOX(&d, ID_V4_DNS2, 146, 188);
    dlg_item(&d, NULL, ATOM_STATIC, L"", ID_NOTE, SS_LEFT, 14, 214, 235, 20);
    dlg_item(&d, NULL, ATOM_BUTTON, L"OK", IDOK, BS_DEFPUSHBUTTON | WS_TABSTOP, 152, 255, 50, 14);
    BUTTON(&d, L"Cancel", IDCANCEL, 207, 255, 50);
    return DialogBoxIndirectParamW(GetModuleHandleW(NULL), d.t, owner, ipv4_proc, (LPARAM)a);
}

/* --- the IPv6 dialog -------------------------------------------------------------- */

enum { ID_V6_AUTO = 201, ID_V6_STATIC, ID_V6_DNS_AUTO, ID_V6_DNS_STATIC, ID_V6_IP = 210, ID_V6_PREFIX,
       ID_V6_GW, ID_V6_DNS1, ID_V6_DNS2 };

static void v6_enable(HWND dlg)
{
    BOOL fixed = IsDlgButtonChecked(dlg, ID_V6_STATIC) == BST_CHECKED;
    int id;
    if (fixed) CheckRadioButton(dlg, ID_V6_DNS_AUTO, ID_V6_DNS_STATIC, ID_V6_DNS_STATIC);
    EnableWindow(GetDlgItem(dlg, ID_V6_DNS_AUTO), !fixed);
    for (id = ID_V6_IP; id <= ID_V6_GW; id++) EnableWindow(GetDlgItem(dlg, id), fixed);
    for (id = ID_V6_DNS1; id <= ID_V6_DNS2; id++)
        EnableWindow(GetDlgItem(dlg, id), IsDlgButtonChecked(dlg, ID_V6_DNS_STATIC) == BST_CHECKED);
}

static char *dlg_text_utf8(HWND dlg, int id)
{
    WCHAR w[128];
    GetDlgItemTextW(dlg, id, w, 128);
    return w_to_utf8(w);
}

static INT_PTR CALLBACK ipv6_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    struct adapter *a = (struct adapter *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        TCITEMW tab = { TCIF_TEXT };
        BOOL automatic;
        WCHAR w[80];
        int i;
        a = (struct adapter *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        tab.pszText = (WCHAR *)L"General";
        SendDlgItemMessageW(dlg, ID_TAB, TCM_INSERTITEMW, 0, (LPARAM)&tab);
        EnableThemeDialogTexture(dlg, ETDT_ENABLETAB);
        automatic = strcmp(a->v6method, "manual") != 0;
        CheckRadioButton(dlg, ID_V6_AUTO, ID_V6_STATIC, automatic ? ID_V6_AUTO : ID_V6_STATIC);
        CheckRadioButton(dlg, ID_V6_DNS_AUTO, ID_V6_DNS_STATIC, automatic ? ID_V6_DNS_AUTO : ID_V6_DNS_STATIC);
        for (i = 0; i < a->nv6 && !automatic; i++)
        {
            char buf[64], *slash;
            if (!strncmp(a->v6addr[i], "fe80:", 5)) continue;
            snprintf(buf, sizeof(buf), "%s", a->v6addr[i]);
            if ((slash = strchr(buf, '/'))) { *slash = 0; utf8_to_w(slash + 1, w, 80); SetDlgItemTextW(dlg, ID_V6_PREFIX, w); }
            utf8_to_w(buf, w, 80); SetDlgItemTextW(dlg, ID_V6_IP, w);
            break;
        }
        if (!automatic) { utf8_to_w(a->v6gw, w, 80); SetDlgItemTextW(dlg, ID_V6_GW, w); }
        if (!g_is_admin)
        {
            SetDlgItemTextW(dlg, ID_NOTE, L"Changing these settings requires an administrator.");
            SendDlgItemMessageW(dlg, IDOK, BCM_SETSHIELD, 0, TRUE);
        }
        v6_enable(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case ID_V6_AUTO: case ID_V6_STATIC:
            CheckRadioButton(dlg, ID_V6_AUTO, ID_V6_STATIC, LOWORD(wp)); v6_enable(dlg); return TRUE;
        case ID_V6_DNS_AUTO: case ID_V6_DNS_STATIC:
            CheckRadioButton(dlg, ID_V6_DNS_AUTO, ID_V6_DNS_STATIC, LOWORD(wp)); v6_enable(dlg); return TRUE;
        case IDOK:
        {
            const char *argv[10];
            char *ip = dlg_text_utf8(dlg, ID_V6_IP), *pfx = dlg_text_utf8(dlg, ID_V6_PREFIX);
            char *gw = dlg_text_utf8(dlg, ID_V6_GW), *d1 = dlg_text_utf8(dlg, ID_V6_DNS1);
            char *d2 = dlg_text_utf8(dlg, ID_V6_DNS2), addr[96], dns[160];
            struct net_reply r;
            int argc = 0;
            WCHAR err[512];
            dns[0] = 0;
            if (d1 && d1[0]) strcat(dns, d1);
            if (d2 && d2[0]) { if (dns[0]) strcat(dns, ","); strcat(dns, d2); }
            argv[argc++] = "ipv6"; argv[argc++] = a->dev;
            if (IsDlgButtonChecked(dlg, ID_V6_AUTO) == BST_CHECKED)
            {
                argv[argc++] = "auto";
                if (IsDlgButtonChecked(dlg, ID_V6_DNS_STATIC) == BST_CHECKED && dns[0]) { argv[argc++] = "--dns"; argv[argc++] = dns; }
            }
            else
            {
                snprintf(addr, sizeof(addr), "%s/%s", ip ? ip : "", pfx && pfx[0] ? pfx : "64");
                argv[argc++] = "static"; argv[argc++] = addr;
                if (gw && gw[0]) { argv[argc++] = "--gateway"; argv[argc++] = gw; }
                if (dns[0]) { argv[argc++] = "--dns"; argv[argc++] = dns; }
            }
            net_request_pumped(argc, argv, NULL, &r);
            free(ip); free(pfx); free(gw); free(d1); free(d2);
            net_reply_free(&r);
            if (!r.ok)
            {
                net_error_text(&r, err, 512);
                MessageBoxW(dlg, err, L"Microsoft TCP/IP", MB_OK | MB_ICONERROR);
                return TRUE;
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

static INT_PTR show_ipv6(HWND owner, struct adapter *a)
{
    static struct dlg d;
    dlg_begin(&d, L"Internet Protocol Version 6 (TCP/IPv6) Properties", 263, 276);
    dlg_item(&d, WC_TABCONTROLW, 0, L"", ID_TAB, WS_CLIPSIBLINGS, 6, 6, 251, 243);
    dlg_item(&d, NULL, ATOM_STATIC, L"You can get IPv6 settings assigned automatically if your network supports "
             L"this capability. Otherwise, you need to ask your network administrator for the appropriate IPv6 "
             L"settings.", 0xFFFF, SS_LEFT, 14, 26, 235, 26);
    RADIO(&d, L"&Obtain an IPv6 address automatically", ID_V6_AUTO, 14, 57, 200, TRUE);
    GROUP(&d, L"", 13, 72, 236, 63);
    RADIO(&d, L"U&se the following IPv6 address:", ID_V6_STATIC, 19, 71, 130, FALSE);
    LABEL(&d, L"&IPv6 address:", 21, 88, 100);
    EDIT(&d, ID_V6_IP, 120, 86, 121);
    LABEL(&d, L"S&ubnet prefix length:", 21, 104, 100);
    EDIT(&d, ID_V6_PREFIX, 120, 102, 30);
    LABEL(&d, L"&Default gateway:", 21, 120, 100);
    EDIT(&d, ID_V6_GW, 120, 118, 121);
    RADIO(&d, L"O&btain DNS server address automatically", ID_V6_DNS_AUTO, 14, 143, 200, TRUE);
    GROUP(&d, L"", 13, 158, 236, 47);
    RADIO(&d, L"Us&e the following DNS server addresses:", ID_V6_DNS_STATIC, 19, 157, 150, FALSE);
    LABEL(&d, L"&Preferred DNS server:", 21, 174, 100);
    EDIT(&d, ID_V6_DNS1, 120, 172, 121);
    LABEL(&d, L"&Alternate DNS server:", 21, 190, 100);
    EDIT(&d, ID_V6_DNS2, 120, 188, 121);
    dlg_item(&d, NULL, ATOM_STATIC, L"", ID_NOTE, SS_LEFT, 14, 214, 235, 20);
    dlg_item(&d, NULL, ATOM_BUTTON, L"OK", IDOK, BS_DEFPUSHBUTTON | WS_TABSTOP, 152, 255, 50, 14);
    BUTTON(&d, L"Cancel", IDCANCEL, 207, 255, 50);
    return DialogBoxIndirectParamW(GetModuleHandleW(NULL), d.t, owner, ipv6_proc, (LPARAM)a);
}

/* --- Network Connection Details -------------------------------------------------------- */

static void fmt_time(long long t, WCHAR *out, int cch)
{
    FILETIME ft;
    SYSTEMTIME st, lt;
    WCHAR d[64], tm[32];
    ULONGLONG v = ((ULONGLONG)t + 11644473600ULL) * 10000000ULL;
    if (t <= 0) { out[0] = 0; return; }
    ft.dwLowDateTime = (DWORD)v;
    ft.dwHighDateTime = (DWORD)(v >> 32);
    FileTimeToSystemTime(&ft, &st);
    SystemTimeToTzSpecificLocalTime(NULL, &st, &lt);
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &lt, NULL, d, 64);
    GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &lt, NULL, tm, 32);
    _snwprintf(out, cch, L"%ls %ls", d, tm);
    out[cch - 1] = 0;
}

/* The rows of "Network Connection Details", as Windows orders them. */
typedef void (*row_fn)(void *ctx, const WCHAR *prop, const WCHAR *value);

static void detail_rows(const struct adapter *a, row_fn row, void *ctx)
{
    WCHAR v[256], desc[128];
    int i;
    BOOL dhcp = strcmp(a->v4method, "manual") != 0;
    utf8_to_w(a->suffix, v, 256); row(ctx, L"Connection-specific DNS Suffix", v);
    desc_text(a, desc, 128); row(ctx, L"Description", desc);
    utf8_to_w(a->mac, v, 256);
    for (i = 0; v[i]; i++) { if (v[i] == ':') v[i] = '-'; else v[i] = towupper(v[i]); }
    row(ctx, L"Physical Address", v);
    row(ctx, L"DHCP Enabled", dhcp ? L"Yes" : L"No");
    for (i = 0; i < a->nv4; i++)
    {
        char buf[48], *slash;
        DWORD m;
        snprintf(buf, sizeof(buf), "%s", a->v4addr[i]);
        slash = strchr(buf, '/');
        m = prefix_mask(slash ? atoi(slash + 1) : 32);
        if (slash) *slash = 0;
        utf8_to_w(buf, v, 256); row(ctx, L"IPv4 Address", v);
        _snwprintf(v, 256, L"%lu.%lu.%lu.%lu", (m >> 24) & 255, (m >> 16) & 255, (m >> 8) & 255, m & 255);
        row(ctx, L"IPv4 Subnet Mask", v);
    }
    if (dhcp && a->obtained) { fmt_time(a->obtained, v, 256); row(ctx, L"Lease Obtained", v); }
    if (dhcp && a->expires) { fmt_time(a->expires, v, 256); row(ctx, L"Lease Expires", v); }
    utf8_to_w(a->v4gw, v, 256); row(ctx, L"IPv4 Default Gateway", v);
    if (dhcp) { utf8_to_w(a->dhcp_server, v, 256); row(ctx, L"IPv4 DHCP Server", v); }
    for (i = 0; i < a->nv4dns; i++) { utf8_to_w(a->v4dns[i], v, 256); row(ctx, i ? L"" : L"IPv4 DNS Servers", v); }
    if (!a->nv4dns) row(ctx, L"IPv4 DNS Servers", L"");
    for (i = 0; i < a->nv6; i++)
    {
        char buf[64], *slash;
        snprintf(buf, sizeof(buf), "%s", a->v6addr[i]);
        if ((slash = strchr(buf, '/'))) *slash = 0;
        utf8_to_w(buf, v, 256);
        row(ctx, strncmp(buf, "fe80:", 5) ? L"IPv6 Address" : L"Link-local IPv6 Address", v);
    }
    utf8_to_w(a->v6gw, v, 256); row(ctx, L"IPv6 Default Gateway", v);
    for (i = 0; i < a->nv6dns; i++) { utf8_to_w(a->v6dns[i], v, 256); row(ctx, i ? L"" : L"IPv6 DNS Server", v); }
}

static void lv_row(void *ctx, const WCHAR *prop, const WCHAR *value)
{
    HWND lv = ctx;
    LVITEMW it = { LVIF_TEXT };
    it.iItem = (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0);
    it.pszText = (WCHAR *)prop;
    it.iItem = (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
    it.iSubItem = 1;
    it.pszText = (WCHAR *)value;
    SendMessageW(lv, LVM_SETITEMTEXTW, it.iItem, (LPARAM)&it);
}

static INT_PTR CALLBACK details_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_INITDIALOG)
    {
        HWND lv = GetDlgItem(dlg, 300);
        LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH };
        c.cx = 170; c.pszText = (WCHAR *)L"Property";
        SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&c);
        c.cx = 230; c.pszText = (WCHAR *)L"Value";
        SendMessageW(lv, LVM_INSERTCOLUMNW, 1, (LPARAM)&c);
        SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
        detail_rows((struct adapter *)lp, lv_row, lv);
        return TRUE;
    }
    if (msg == WM_COMMAND && (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDOK)) { EndDialog(dlg, IDOK); return TRUE; }
    return FALSE;
}

static void show_details(HWND owner, struct adapter *a)
{
    static struct dlg d;
    dlg_begin(&d, L"Network Connection Details", 290, 200);
    LABEL(&d, L"Network Connection &Details:", 7, 7, 200);
    dlg_item(&d, WC_LISTVIEWW, 0, L"", 300, LVS_REPORT | LVS_NOSORTHEADER | WS_BORDER | WS_TABSTOP, 7, 18, 276, 154);
    dlg_item(&d, NULL, ATOM_BUTTON, L"&Close", IDCANCEL, BS_DEFPUSHBUTTON | WS_TABSTOP, 233, 179, 50, 14);
    DialogBoxIndirectParamW(GetModuleHandleW(NULL), d.t, owner, details_proc, (LPARAM)a);
}

/* --- Properties ------------------------------------------------------------------------------ */

static INT_PTR CALLBACK props_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    struct adapter *a = (struct adapter *)GetWindowLongPtrW(dlg, DWLP_USER);
    HWND lv = GetDlgItem(dlg, 400);
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        TCITEMW tab = { TCIF_TEXT };
        LVCOLUMNW c = { LVCF_WIDTH };
        LVITEMW it = { LVIF_TEXT };
        WCHAR desc[128];
        a = (struct adapter *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        tab.pszText = (WCHAR *)L"Networking";
        SendDlgItemMessageW(dlg, ID_TAB, TCM_INSERTITEMW, 0, (LPARAM)&tab);
        EnableThemeDialogTexture(dlg, ETDT_ENABLETAB);
        desc_text(a, desc, 128);
        SetDlgItemTextW(dlg, 401, desc);
        SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT,
                     LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        c.cx = 330;
        SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&c);
        it.pszText = (WCHAR *)L"Internet Protocol Version 4 (TCP/IPv4)";
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iItem = 1;
        it.pszText = (WCHAR *)L"Internet Protocol Version 6 (TCP/IPv6)";
        SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
        ListView_SetCheckState(lv, 0, TRUE);
        ListView_SetCheckState(lv, 1, strcmp(a->v6method, "disabled") != 0);
        ListView_SetItemState(lv, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SetDlgItemTextW(dlg, 402, L"Transmission Control Protocol/Internet Protocol. The default wide area "
                                  L"network protocol that provides communication across diverse interconnected networks.");
        if (!g_is_admin) SendDlgItemMessageW(dlg, IDOK, BCM_SETSHIELD, 0, TRUE);
        return TRUE;
    }
    case WM_NOTIFY:
    {
        NMHDR *h = (NMHDR *)lp;
        if (h->idFrom == 400 && h->code == LVN_ITEMCHANGED)
        {
            NMLISTVIEW *n = (NMLISTVIEW *)lp;
            /* IPv4 cannot be turned off here. */
            if (n->iItem == 0 && (n->uChanged & LVIF_STATE) && !ListView_GetCheckState(lv, 0))
                ListView_SetCheckState(lv, 0, TRUE);
            if (n->uNewState & LVIS_SELECTED)
                SetDlgItemTextW(dlg, 402, n->iItem == 0
                    ? L"Transmission Control Protocol/Internet Protocol. The default wide area network protocol "
                      L"that provides communication across diverse interconnected networks."
                    : L"TCP/IP version 6. The latest version of the internet protocol that provides communication "
                      L"across diverse interconnected networks.");
        }
        else if (h->idFrom == 400 && h->code == NM_DBLCLK)
            PostMessageW(dlg, WM_COMMAND, 403, 0);
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case 403:
        {
            int i = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
            if (i == 0) show_ipv4(dlg, a);
            else if (i == 1) show_ipv6(dlg, a);
            return TRUE;
        }
        case IDOK:
        {
            BOOL v6 = ListView_GetCheckState(lv, 1);
            BOOL was = strcmp(a->v6method, "disabled") != 0;
            if (v6 != was)
            {
                const char *argv[] = { "ipv6", a->dev, v6 ? "auto" : "disabled" };
                struct net_reply r;
                WCHAR err[512];
                net_request_pumped(3, argv, NULL, &r);
                net_reply_free(&r);
                if (!r.ok)
                {
                    net_error_text(&r, err, 512);
                    MessageBoxW(dlg, err, L"Network Connections", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
        }
        break;
    }
    return FALSE;
}

static void show_props(HWND owner, struct adapter *a)
{
    static struct dlg d;
    WCHAR title[128];
    _snwprintf(title, 128, L"%ls Properties", a->name);
    dlg_begin(&d, title, 240, 264);
    dlg_item(&d, WC_TABCONTROLW, 0, L"", ID_TAB, WS_CLIPSIBLINGS, 6, 6, 228, 230);
    LABEL(&d, L"Connect using:", 13, 26, 200);
    dlg_item(&d, NULL, ATOM_EDIT, L"", 401, WS_BORDER | ES_READONLY | ES_AUTOHSCROLL, 13, 37, 214, 14);
    LABEL(&d, L"This c&onnection uses the following items:", 13, 60, 200);
    dlg_item(&d, WC_LISTVIEWW, 0, L"", 400, LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
             WS_BORDER | WS_TABSTOP, 13, 71, 214, 70);
    dlg_item(&d, NULL, ATOM_BUTTON, L"I&nstall...", 404, BS_PUSHBUTTON | WS_DISABLED, 13, 146, 68, 14);
    dlg_item(&d, NULL, ATOM_BUTTON, L"&Uninstall", 405, BS_PUSHBUTTON | WS_DISABLED, 86, 146, 68, 14);
    BUTTON(&d, L"P&roperties", 403, 159, 146, 68);
    GROUP(&d, L"Description", 13, 166, 214, 60);
    dlg_item(&d, NULL, ATOM_STATIC, L"", 402, SS_LEFT, 20, 178, 200, 44);
    dlg_item(&d, NULL, ATOM_BUTTON, L"OK", IDOK, BS_DEFPUSHBUTTON | WS_TABSTOP, 129, 243, 50, 14);
    BUTTON(&d, L"Cancel", IDCANCEL, 184, 243, 50);
    DialogBoxIndirectParamW(GetModuleHandleW(NULL), d.t, owner, props_proc, (LPARAM)a);
}

/* --- Status ------------------------------------------------------------------------------ */

static void fmt_bytes(long long n, WCHAR *out, int cch)
{
    WCHAR num[64], raw[32];
    NUMBERFMTW f = { 0, 1, 3, (WCHAR *)L".", (WCHAR *)L",", 0 };  /* LeadingZero, or 0 prints as nothing */
    _snwprintf(raw, 32, L"%I64d", n);
    if (!GetNumberFormatW(LOCALE_USER_DEFAULT, 0, raw, &f, num, 64)) wcscpy(num, raw);
    _snwprintf(out, cch, L"%ls", num);
    out[cch - 1] = 0;
}

static void status_fill(HWND dlg, struct adapter *a)
{
    WCHAR v[128];
    BOOL up = is_connected(a);
    SetDlgItemTextW(dlg, 501, up && a->v4gw[0] ? L"Internet" : up && a->nv4 ? L"No Internet access" : L"No network access");
    SetDlgItemTextW(dlg, 502, up && a->v6gw[0] ? L"Internet" : L"No network access");
    SetDlgItemTextW(dlg, 503, up ? L"Enabled" : L"Disabled");
    utf8_to_w(a->ssid, v, 128);
    SetDlgItemTextW(dlg, 504, is_wifi(a) ? v : L"");
    if (a->speed >= 1000) _snwprintf(v, 128, L"%.1f Gbps", a->speed / 1000.0);
    else if (a->speed > 0) _snwprintf(v, 128, L"%I64d Mbps", a->speed);
    else wcscpy(v, L"");
    SetDlgItemTextW(dlg, 505, v);
    fmt_bytes(a->tx, v, 128); SetDlgItemTextW(dlg, 506, v);
    fmt_bytes(a->rx, v, 128); SetDlgItemTextW(dlg, 507, v);
    SetDlgItemTextW(dlg, 509, up ? L"&Disable" : L"&Enable");
    if (!g_is_admin) SendDlgItemMessageW(dlg, 509, BCM_SETSHIELD, 0, TRUE);
}

static void refresh_selected(struct adapter *a)
{
    char dev[32];
    int i;
    snprintf(dev, sizeof(dev), "%s", a->dev);
    load_adapters(TRUE);
    if ((i = find_adapter(dev)) >= 0 && &g_ad[i] != a) *a = g_ad[i];
    else if (i >= 0) *a = g_ad[i];
}

static BOOL set_enabled(HWND owner, struct adapter *a, BOOL enable)
{
    const char *argv[] = { enable ? "enable" : "disable", a->dev };
    struct net_reply r;
    WCHAR err[512];
    net_request_pumped(2, argv, NULL, &r);
    net_reply_free(&r);
    if (r.ok) return TRUE;
    net_error_text(&r, err, 512);
    MessageBoxW(owner, err, L"Network Connections", MB_OK | MB_ICONWARNING);
    return FALSE;
}

static INT_PTR CALLBACK status_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    struct adapter *a = (struct adapter *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        TCITEMW tab = { TCIF_TEXT };
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        tab.pszText = (WCHAR *)L"General";
        SendDlgItemMessageW(dlg, ID_TAB, TCM_INSERTITEMW, 0, (LPARAM)&tab);
        EnableThemeDialogTexture(dlg, ETDT_ENABLETAB);
        status_fill(dlg, (struct adapter *)lp);
        SetTimer(dlg, 1, 3000, NULL);
        return TRUE;
    }
    case WM_TIMER:
        if (!g_refreshing && a)
        {
            g_refreshing = TRUE;
            refresh_selected(a);
            status_fill(dlg, a);
            g_refreshing = FALSE;
        }
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case 508: show_details(dlg, a); return TRUE;
        case 510: show_props(dlg, a); refresh_selected(a); status_fill(dlg, a); return TRUE;
        case 509:
            if (set_enabled(dlg, a, !is_connected(a))) { refresh_selected(a); status_fill(dlg, a); }
            return TRUE;
        case IDCANCEL: case IDOK: KillTimer(dlg, 1); EndDialog(dlg, IDOK); return TRUE;
        }
        break;
    }
    return FALSE;
}

static void show_status(HWND owner, struct adapter *a)
{
    static struct dlg d;
    WCHAR title[128];
    _snwprintf(title, 128, L"%ls Status", a->name);
    dlg_begin(&d, title, 230, 236);
    dlg_item(&d, WC_TABCONTROLW, 0, L"", ID_TAB, WS_CLIPSIBLINGS, 6, 6, 218, 204);
    GROUP(&d, L"Connection", 13, 24, 204, 98);
    LABEL(&d, L"IPv4 Connectivity:", 20, 37, 90);   VALUE(&d, 501, 115, 37, 98);
    LABEL(&d, L"IPv6 Connectivity:", 20, 49, 90);   VALUE(&d, 502, 115, 49, 98);
    LABEL(&d, L"Media State:", 20, 61, 90);         VALUE(&d, 503, 115, 61, 98);
    LABEL(&d, L"SSID:", 20, 73, 90);                VALUE(&d, 504, 115, 73, 98);
    LABEL(&d, L"Speed:", 20, 85, 90);               VALUE(&d, 505, 115, 85, 98);
    BUTTON(&d, L"D&etails...", 508, 20, 101, 60);
    GROUP(&d, L"Activity", 13, 126, 204, 56);
    LABEL(&d, L"Sent", 100, 139, 50);
    LABEL(&d, L"Received", 165, 139, 50);
    LABEL(&d, L"Bytes:", 20, 156, 40);
    VALUE(&d, 506, 80, 156, 65);
    VALUE(&d, 507, 150, 156, 65);
    BUTTON(&d, L"P&roperties", 510, 13, 188, 60);
    BUTTON(&d, L"&Disable", 509, 78, 188, 60);
    dlg_item(&d, NULL, ATOM_BUTTON, L"&Close", IDCANCEL, BS_DEFPUSHBUTTON | WS_TABSTOP, 174, 216, 50, 14);
    DialogBoxIndirectParamW(GetModuleHandleW(NULL), d.t, owner, status_proc, (LPARAM)a);
}

/* --- the main window ------------------------------------------------------------------------ */

enum { CMD_TOGGLE, CMD_STATUS, CMD_CHANGE, CMD_COUNT };

static void cmd_text(int c, WCHAR *out, int cch)
{
    const struct adapter *a = g_sel >= 0 ? &g_ad[g_sel] : NULL;
    switch (c)
    {
    case CMD_TOGGLE: _snwprintf(out, cch, a && !is_connected(a) ? L"Enable this network device" : L"Disable this network device"); break;
    case CMD_STATUS: _snwprintf(out, cch, L"View status of this connection"); break;
    default: _snwprintf(out, cch, L"Change settings of this connection"); break;
    }
}

static RECT cmd_rect(HDC dc, int c)
{
    RECT r = { 12, 0, 12, BAR_H };
    int i;
    for (i = 0; i <= c; i++)
    {
        WCHAR t[64];
        SIZE sz;
        cmd_text(i, t, 64);
        GetTextExtentPoint32W(dc, t, lstrlenW(t), &sz);
        r.left = r.right + (i ? 18 : 0) + (i == CMD_STATUS ? 0 : 0);
        r.right = r.left + sz.cx + 4 + (i == CMD_TOGGLE || i == CMD_CHANGE ? (g_is_admin ? 0 : 20) : 0);
    }
    return r;
}

static RECT tile_rect(HWND hwnd, int i)
{
    RECT c, r;
    int cols;
    GetClientRect(hwnd, &c);
    cols = (c.right - 24) / (TILE_W + 12);
    if (cols < 1) cols = 1;
    r.left = 12 + (i % cols) * (TILE_W + 12);
    r.top = BAR_H + 12 + (i / cols) * (TILE_H + 8);
    r.right = r.left + TILE_W;
    r.bottom = r.top + TILE_H;
    return r;
}

/* Our own adapter pictures: a network card with its cable, or Wi-Fi arcs;
 * a red cross when it is not connected, grey when disabled. */
static void draw_adapter_icon(HDC dc, int x, int y, const struct adapter *a)
{
    BOOL up = is_connected(a), off = !strcmp(a->state, "disconnected");
    COLORREF body = off ? RGB(0xA0, 0xA0, 0xA0) : RGB(0x3C, 0x6E, 0xB4);
    HPEN pen = CreatePen(PS_SOLID, 2, body), oldp = SelectObject(dc, pen);
    HBRUSH br = CreateSolidBrush(off ? RGB(0xE8, 0xE8, 0xE8) : RGB(0xD6, 0xE8, 0xFA)), oldb = SelectObject(dc, br);
    if (is_wifi(a))
    {
        int i;
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        for (i = 0; i < 3; i++)
        {
            int rad = 10 + i * 9;
            Arc(dc, x + 24 - rad, y + 38 - rad, x + 24 + rad, y + 38 + rad, x + 24 + rad, y + 38 - rad, x + 24 - rad, y + 38 - rad);
        }
        SelectObject(dc, br);
        Ellipse(dc, x + 20, y + 34, x + 28, y + 42);
    }
    else
    {
        RoundRect(dc, x + 6, y + 10, x + 42, y + 34, 4, 4);   /* the card */
        Rectangle(dc, x + 12, y + 34, x + 18, y + 40);        /* its connector */
        MoveToEx(dc, x + 15, y + 40, NULL);
        LineTo(dc, x + 15, y + 46);                           /* the cable */
        Rectangle(dc, x + 26, y + 16, x + 36, y + 24);        /* the chip */
    }
    SelectObject(dc, oldp); SelectObject(dc, oldb);
    DeleteObject(pen); DeleteObject(br);
    if (!up)
    {
        HBRUSH red = CreateSolidBrush(RGB(0xE8, 0x11, 0x23));
        HPEN white = CreatePen(PS_SOLID, 2, RGB(0xFF, 0xFF, 0xFF));
        oldb = SelectObject(dc, red);
        oldp = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, x + 28, y + 30, x + 46, y + 48);
        SelectObject(dc, white);
        MoveToEx(dc, x + 33, y + 35, NULL); LineTo(dc, x + 41, y + 43);
        MoveToEx(dc, x + 41, y + 35, NULL); LineTo(dc, x + 33, y + 43);
        SelectObject(dc, oldb); SelectObject(dc, oldp);
        DeleteObject(red); DeleteObject(white);
    }
}

static void draw_shield(HDC dc, int x, int y)
{
    POINT p[5] = { { x + 6, y }, { x + 12, y + 2 }, { x + 11, y + 9 }, { x + 6, y + 13 }, { x + 1, y + 9 } };
    HBRUSH b = CreateSolidBrush(RGB(0x1E, 0x6F, 0xD9)), ob = SelectObject(dc, b);
    HPEN op = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, p, 5);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b);
    p[0].x = x + 12; p[3].x = x + 6;
}

static void on_paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps), dc;
    RECT c, r;
    HBITMAP bmp, oldbmp;
    HBRUSH b;
    int i;

    GetClientRect(hwnd, &c);
    dc = CreateCompatibleDC(wdc);
    bmp = CreateCompatibleBitmap(wdc, c.right, c.bottom);
    oldbmp = SelectObject(dc, bmp);
    b = CreateSolidBrush(COL_BG); FillRect(dc, &c, b); DeleteObject(b);
    SetBkMode(dc, TRANSPARENT);

    /* the command bar */
    r = c; r.bottom = BAR_H;
    b = CreateSolidBrush(COL_BAR); FillRect(dc, &r, b); DeleteObject(b);
    r.top = BAR_H - 1;
    b = CreateSolidBrush(COL_BAR_LINE); FillRect(dc, &r, b); DeleteObject(b);
    SelectObject(dc, g_font_bar);
    if (g_sel >= 0)
    {
        for (i = 0; i < CMD_COUNT; i++)
        {
            WCHAR t[64];
            RECT cr = cmd_rect(dc, i);
            cmd_text(i, t, 64);
            if (i == g_hot_cmd)
            {
                RECT h = cr; InflateRect(&h, 6, -5);
                b = CreateSolidBrush(COL_HOT); FillRect(dc, &h, b); DeleteObject(b);
            }
            if (!g_is_admin && (i == CMD_TOGGLE || i == CMD_CHANGE))
            {
                draw_shield(dc, cr.left, (BAR_H - 14) / 2);
                cr.left += 20;
            }
            SetTextColor(dc, COL_TEXT);
            DrawTextW(dc, t, -1, &cr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        }
    }
    else
    {
        RECT tr = { 12, 0, c.right, BAR_H };
        SetTextColor(dc, COL_SUBTLE);
        DrawTextW(dc, g_nad ? L"Select a connection to see what you can do with it."
                            : g_status[0] ? g_status : L"Looking for network adapters...",
                  -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    for (i = 0; i < g_nad; i++)
    {
        struct adapter *a = &g_ad[i];
        WCHAR st[128], desc[128];
        RECT tr = tile_rect(hwnd, i), t;
        if (i == g_sel || i == g_hot)
        {
            HPEN pen = CreatePen(PS_SOLID, 1, i == g_sel ? COL_SEL_LINE : COL_HOT);
            HBRUSH fill = CreateSolidBrush(i == g_sel ? COL_SEL : COL_HOT);
            HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, fill);
            Rectangle(dc, tr.left, tr.top, tr.right, tr.bottom);
            SelectObject(dc, op); SelectObject(dc, ob);
            DeleteObject(pen); DeleteObject(fill);
        }
        draw_adapter_icon(dc, tr.left + 10, tr.top + 14, a);
        status_text(a, st, 128);
        desc_text(a, desc, 128);
        t = tr; t.left += 70; t.top += 12; t.right -= 8;
        SelectObject(dc, g_font_bold); SetTextColor(dc, COL_TEXT);
        DrawTextW(dc, a->name, -1, &t, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        t.top += 19;
        SelectObject(dc, g_font); SetTextColor(dc, COL_SUBTLE);
        DrawTextW(dc, st, -1, &t, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        t.top += 17;
        DrawTextW(dc, desc, -1, &t, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    BitBlt(wdc, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);
}

static int hit_tile(HWND hwnd, int x, int y)
{
    POINT p = { x, y };
    int i;
    for (i = 0; i < g_nad; i++)
    {
        RECT r = tile_rect(hwnd, i);
        if (PtInRect(&r, p)) return i;
    }
    return -1;
}

static int hit_cmd(HWND hwnd, int x, int y)
{
    POINT p = { x, y };
    HDC dc;
    int i, hit = -1;
    if (g_sel < 0 || y >= BAR_H) return -1;
    dc = GetDC(hwnd);
    SelectObject(dc, g_font_bar);
    for (i = 0; i < CMD_COUNT && hit < 0; i++)
    {
        RECT r = cmd_rect(dc, i);
        if (PtInRect(&r, p)) hit = i;
    }
    ReleaseDC(hwnd, dc);
    return hit;
}

static void run_cmd(HWND hwnd, int c)
{
    struct adapter a;
    if (g_sel < 0) return;
    a = g_ad[g_sel];
    if (c == CMD_STATUS) show_status(hwnd, &a);
    else if (c == CMD_CHANGE) show_props(hwnd, &a);
    else if (c == CMD_TOGGLE) set_enabled(hwnd, &a, !is_connected(&a));
    load_adapters(TRUE);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void context_menu(HWND hwnd, int x, int y)
{
    HMENU m = CreatePopupMenu();
    POINT p = { x, y };
    int cmd;
    const struct adapter *a = &g_ad[g_sel];
    AppendMenuW(m, MF_STRING, 1, is_connected(a) ? L"Disa&ble" : L"Ena&ble");
    AppendMenuW(m, MF_STRING, 2, L"Stat&us");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, 3, L"P&roperties");
    SetMenuDefaultItem(m, 2, FALSE);
    ClientToScreen(hwnd, &p);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (cmd == 1) run_cmd(hwnd, CMD_TOGGLE);
    else if (cmd == 2) run_cmd(hwnd, CMD_STATUS);
    else if (cmd == 3) run_cmd(hwnd, CMD_CHANGE);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT: on_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_TIMER:
        if (!g_refreshing && IsWindowEnabled(hwnd))
        {
            char dev[32] = "";
            g_refreshing = TRUE;
            if (g_sel >= 0) snprintf(dev, sizeof(dev), "%s", g_ad[g_sel].dev);
            load_adapters(TRUE);
            g_sel = dev[0] ? find_adapter(dev) : -1;
            g_refreshing = FALSE;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
    {
        int t = hit_tile(hwnd, (short)LOWORD(lp), (short)HIWORD(lp));
        int c = hit_cmd(hwnd, (short)LOWORD(lp), (short)HIWORD(lp));
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        if (t != g_hot || c != g_hot_cmd) { g_hot = t; g_hot_cmd = c; InvalidateRect(hwnd, NULL, FALSE); }
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE: g_hot = g_hot_cmd = -1; InvalidateRect(hwnd, NULL, FALSE); return 0;
    case WM_LBUTTONDOWN:
    {
        int c = hit_cmd(hwnd, (short)LOWORD(lp), (short)HIWORD(lp));
        if (c >= 0) { run_cmd(hwnd, c); return 0; }
        g_sel = hit_tile(hwnd, (short)LOWORD(lp), (short)HIWORD(lp));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if ((g_sel = hit_tile(hwnd, (short)LOWORD(lp), (short)HIWORD(lp))) >= 0) run_cmd(hwnd, CMD_STATUS);
        return 0;
    case WM_RBUTTONUP:
        if ((g_sel = hit_tile(hwnd, (short)LOWORD(lp), (short)HIWORD(lp))) >= 0)
        {
            InvalidateRect(hwnd, NULL, FALSE);
            context_menu(hwnd, (short)LOWORD(lp), (short)HIWORD(lp));
        }
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_F5) { SendMessageW(hwnd, WM_TIMER, 1, 0); return 0; }
        if ((wp == VK_RIGHT || wp == VK_DOWN) && g_nad) { g_sel = (g_sel + 1) % g_nad; InvalidateRect(hwnd, NULL, FALSE); }
        if ((wp == VK_LEFT || wp == VK_UP) && g_nad) { g_sel = g_sel <= 0 ? g_nad - 1 : g_sel - 1; InvalidateRect(hwnd, NULL, FALSE); }
        if (wp == VK_RETURN && g_sel >= 0) run_cmd(hwnd, CMD_STATUS);
        if (wp == VK_APPS && g_sel >= 0) { RECT r = tile_rect(hwnd, g_sel); context_menu(hwnd, r.left + 20, r.bottom - 10); }
        return 0;
    case WM_APP:   /* --open: a dialog straight away */
    {
        int i = (int)lp;
        struct adapter a;
        if (i < 0 || i >= g_nad) return 0;
        a = g_ad[i];
        g_sel = i;
        InvalidateRect(hwnd, NULL, FALSE);
        switch (wp)
        {
        case 1: show_status(hwnd, &a); break;
        case 2: show_details(hwnd, &a); break;
        case 3: show_props(hwnd, &a); break;
        case 4: show_ipv4(hwnd, &a); break;
        case 5: show_ipv6(hwnd, &a); break;
        }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --- non-interactive paths, for the gates -------------------------------------------------- */

static void dump_row(void *ctx, const WCHAR *prop, const WCHAR *value)
{
    char *p = w_to_utf8(prop), *v = w_to_utf8(value);
    net_report("DETAIL %s|%s|%s\n", (const char *)ctx, p ? p : "", v ? v : "");
    free(p); free(v);
}

static int dump(void)
{
    int i;
    if (!load_adapters(FALSE)) { char *m = w_to_utf8(g_status); net_report("ERROR %s\n", m ? m : ""); free(m); return 1; }
    load_whoami();
    net_report("ADMIN %s\n", g_is_admin ? "yes" : "no");
    for (i = 0; i < g_nad; i++)
    {
        WCHAR st[128], desc[128];
        char *n, *s, *d;
        status_text(&g_ad[i], st, 128);
        desc_text(&g_ad[i], desc, 128);
        n = w_to_utf8(g_ad[i].name); s = w_to_utf8(st); d = w_to_utf8(desc);
        net_report("TILE %s|%s|%s|%s\n", g_ad[i].dev, n, s, d);
        free(n); free(s); free(d);
        detail_rows(&g_ad[i], dump_row, g_ad[i].dev);
    }
    return 0;
}

static DWORD arg_ip(const WCHAR *s, BOOL *ok)
{
    char buf[64];
    DWORD ip = 0;
    WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, sizeof(buf), NULL, NULL);
    if (!strcmp(buf, "-")) return 0;
    if (!parse_ip4(buf, &ip)) *ok = FALSE;
    return ip;
}

static int set_ipv4_cli(int argc, WCHAR **argv, int i)
{
    char dev[32];
    WCHAR err[512];
    BOOL ok = TRUE, automatic;
    DWORD ip = 0, mask = 0, gw = 0, d1 = 0, d2 = 0;
    int rc;
    if (i + 2 > argc) { net_report("RESULT ERROR usage\n"); return 2; }
    WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, dev, sizeof(dev), NULL, NULL);
    automatic = !wcscmp(argv[i + 1], L"auto");
    if (!automatic)
    {
        if (i + 7 > argc) { net_report("RESULT ERROR usage\n"); return 2; }
        ip = arg_ip(argv[i + 2], &ok); mask = arg_ip(argv[i + 3], &ok); gw = arg_ip(argv[i + 4], &ok);
        d1 = arg_ip(argv[i + 5], &ok); d2 = arg_ip(argv[i + 6], &ok);
    }
    else if (i + 4 <= argc)
    {
        d1 = arg_ip(argv[i + 2], &ok); d2 = arg_ip(argv[i + 3], &ok);
    }
    if (!ok) { net_report("RESULT ERROR not an IPv4 address\n"); return 2; }
    load_whoami();
    rc = apply_ipv4(dev, automatic, automatic && !d1 && !d2, ip, mask, gw, d1, d2, err, 512, FALSE);
    if (rc) { char *e = w_to_utf8(err); net_report("RESULT ERROR %d %s\n", rc, e ? e : ""); free(e); }
    else net_report("RESULT OK\n");
    return rc;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, WCHAR *cmdline, int show)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_INTERNET_CLASSES | ICC_TAB_CLASSES |
                                 ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
    WNDCLASSW wc = { 0 };
    WCHAR **argv;
    int argc, i, open_what = 0;
    char open_dev[32] = "";
    MSG msg;
    (void)prev; (void)cmdline;

    if (!net_init(GetCommandLineW()))
    {
        if (!net_relaunch(net_args_after_program(GetCommandLineW())))
            MessageBoxW(NULL, L"The network settings service could not be started.", L"Network Connections",
                        MB_OK | MB_ICONERROR);
        return 0;
    }
    InitCommonControlsEx(&icc);
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; i < argc; i++)
    {
        if (!wcscmp(argv[i], L"--dump")) return dump();
        if (!wcscmp(argv[i], L"--set-ipv4")) return set_ipv4_cli(argc, argv, i + 1);
        if (!wcscmp(argv[i], L"--open") && i + 2 < argc)
        {
            static const WCHAR *names[] = { L"status", L"details", L"props", L"ipv4", L"ipv6" };
            int k;
            for (k = 0; k < 5; k++) if (!wcscmp(argv[i + 1], names[k])) open_what = k + 1;
            WideCharToMultiByte(CP_UTF8, 0, argv[i + 2], -1, open_dev, sizeof(open_dev), NULL, NULL);
        }
    }

    g_font = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_bold = CreateFontW(-13, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_font_bar = CreateFontW(-12, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    load_whoami();
    load_adapters(FALSE);

    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (const WCHAR *)IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"SgNetworkConnections";
    RegisterClassW(&wc);
    g_main = CreateWindowExW(0, wc.lpszClassName, L"Network Connections", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 700, 420, NULL, NULL, inst, NULL);
    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    SetTimer(g_main, 1, 5000, NULL);
    if (open_what) PostMessageW(g_main, WM_APP, open_what, find_adapter(open_dev));
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        if (msg.message == WM_KEYDOWN && msg.hwnd == g_main) { SendMessageW(g_main, WM_KEYDOWN, msg.wParam, msg.lParam); continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
