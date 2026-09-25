/* sg-mmc -- Device Manager (devmgmt.msc): the computer's hardware, grouped as
 * Windows groups it (Display adapters, Network adapters, Disk drives, ...),
 * from two places:
 *
 *   - the Linux side (sg-sysinfo devices: sysfs and udev) -- the real
 *     hardware, the kernel driver bound to it and its module's details, and
 *     "no driver" with what sg-drivers would install (NVIDIA and the like);
 *   - SetupAPI -- the devices Wine itself enumerates for Windows programs
 *     (its display adapters, monitors, HID and USB devices). A Wine device
 *     that is a Linux one (same PCI/USB vendor and product) is shown once,
 *     with its Windows instance ID among its properties.
 *
 * The result pane is a tree (View: Devices by type, or by connection), as
 * Windows' Device Manager draws it. Properties: General (type, maker,
 * location, status in Windows' words), Driver (the kernel module: provider,
 * version, file, licence), Details (every property). Nothing is changed:
 * Linux decides which driver binds, sg-drivers installs third-party ones.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "mmc.h"
#include <setupapi.h>
#include <string.h>

typedef struct dev
{
    WCHAR id[128];              /* sg-sysinfo's DEVICE, or the Windows instance ID */
    WCHAR name[256];
    int cat;                    /* index into CATS */
    WCHAR parent[128];
    BOOL nodriver;
    BOOL wine_only;             /* only SetupAPI knows it */
    WCHAR winid[256];           /* Windows instance ID, when Wine has it */
    WCHAR winservice[64];
    int first, last;            /* its lines in the sg-sysinfo reply, or -1 */
    HTREEITEM hti;
} dev_t_;

static const struct cat { const char *cls; const WCHAR *title; int icon; const WCHAR *winclass; } CATS[] = {
    { "sound",   L"Audio inputs and outputs", IC_SOUND, L"AudioEndpoint" },
    { "battery", L"Batteries", IC_BATTERY, L"Battery" },
    { "bluetooth", L"Bluetooth", IC_BLUETOOTH, L"Bluetooth" },
    { "camera",  L"Cameras", IC_CAMERA, L"Camera" },
    { "disk",    L"Disk drives", IC_DISK, L"DiskDrive" },
    { "display", L"Display adapters", IC_DISPLAY, L"Display" },
    { "cdrom",   L"DVD/CD-ROM drives", IC_CDROM, L"CDROM" },
    { "hid",     L"Human Interface Devices", IC_HID, L"HIDClass" },
    { "keyboard", L"Keyboards", IC_KEYBOARD, L"Keyboard" },
    { "mouse",   L"Mice and other pointing devices", IC_MOUSE, L"Mouse" },
    { "monitor", L"Monitors", IC_MONITOR, L"Monitor" },
    { "net",     L"Network adapters", IC_NETWORK, L"Net" },
    { "other",   L"Other devices", IC_DEVICE, L"" },
    { "printer", L"Print queues", IC_PRINTER, L"PrintQueue" },
    { "processor", L"Processors", IC_CPU, L"Processor" },
    { "storage-controller", L"Storage controllers", IC_STORAGE, L"SCSIAdapter" },
    { "system",  L"System devices", IC_SYSTEM, L"System" },
    { "usb",     L"Universal Serial Bus controllers", IC_USB, L"USB" },
};
#define NCATS ((int)ARRAY_SIZE(CATS))
#define CAT_OTHER 12

static sys_reply_t g_rep;
static dev_t_ *g_devs;
static int g_ndevs;
static HWND g_pane, g_dtree;
static BOOL g_by_connection;
static WCHAR g_computer[64];
static HTREEITEM g_root, g_cat_items[NCATS];
enum { V_SCAN = 1, V_BYTYPE, V_BYCONN, V_PROPS };

/* ---- collecting ---------------------------------------------------------------------- */

static const char *field(int from, const char *key) { return sys_field(&g_rep, from, key); }

static int cat_for(const char *cls)
{
    int i;
    for (i = 0; i < NCATS; i++) if (cls && !strcmp(CATS[i].cls, cls)) return i;
    return CAT_OTHER;
}

static dev_t_ *add_dev(void)
{
    dev_t_ *p = realloc(g_devs, (g_ndevs + 1) * sizeof(*g_devs));
    if (!p) return NULL;
    g_devs = p;
    memset(&g_devs[g_ndevs], 0, sizeof(dev_t_));
    g_devs[g_ndevs].first = g_devs[g_ndevs].last = -1;
    return &g_devs[g_ndevs++];
}

static void load_linux(void)
{
    int b;
    sys_free(&g_rep);
    sys_request(&g_rep, "devices", NULL);
    for (b = sys_next_block(&g_rep, 0, "DEVICE"); b >= 0; b = sys_next_block(&g_rep, b + 1, "DEVICE"))
    {
        dev_t_ *d = add_dev();
        const char *v;
        int e;
        if (!d) break;
        utf8_to_w(g_rep.lines[b] + 7, d->id, ARRAY_SIZE(d->id));
        utf8_to_w(field(b, "NAME"), d->name, ARRAY_SIZE(d->name));
        if (!d->name[0]) lstrcpynW(d->name, d->id, ARRAY_SIZE(d->name));
        d->cat = cat_for(field(b, "CLASS"));
        if ((v = field(b, "PARENT"))) utf8_to_w(v, d->parent, ARRAY_SIZE(d->parent));
#ifdef SG_MUTANT_NOWARN
        d->nodriver = FALSE;
#else
        d->nodriver = (v = field(b, "STATUS")) && strcmp(v, "ok");
#endif
        for (e = b; e < g_rep.nlines && strcmp(g_rep.lines[e], "END"); e++) ;
        d->first = b;
        d->last = e;
    }
}

/* the vendor and product from a Windows hardware/instance ID: PCI\VEN_10DE&DEV_2204, USB\VID_046D&PID_C52B */
static BOOL win_ids(const WCHAR *id, unsigned *ven, unsigned *prod)
{
    const WCHAR *v = wcsstr(id, L"VEN_"), *p = wcsstr(id, L"DEV_");
    if (!v || !p) { v = wcsstr(id, L"VID_"); p = wcsstr(id, L"PID_"); }
    if (!v || !p) return FALSE;
    *ven = wcstoul(v + 4, NULL, 16);
    *prod = wcstoul(p + 4, NULL, 16);
    return TRUE;
}

static void load_setupapi(void)
{
    HDEVINFO set = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    SP_DEVINFO_DATA info = { sizeof(info) };
    DWORD i;
    if (set == INVALID_HANDLE_VALUE) return;
    for (i = 0; SetupDiEnumDeviceInfo(set, i, &info); i++)
    {
        WCHAR inst[256] = L"", desc[256] = L"", cls[64] = L"", svc[64] = L"";
        unsigned ven, prod;
        int j, c;
        dev_t_ *d = NULL;
        SetupDiGetDeviceInstanceIdW(set, &info, inst, ARRAY_SIZE(inst), NULL);
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_FRIENDLYNAME, NULL, (BYTE *)desc, sizeof(desc), NULL))
            SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_DEVICEDESC, NULL, (BYTE *)desc, sizeof(desc), NULL);
        SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_CLASS, NULL, (BYTE *)cls, sizeof(cls), NULL);
        SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_SERVICE, NULL, (BYTE *)svc, sizeof(svc), NULL);
        /* the same hardware the Linux side has: one entry */
        if (win_ids(inst, &ven, &prod))
            for (j = 0; j < g_ndevs && !d; j++)
            {
                const char *lv, *lp;
                if (g_devs[j].first < 0 || g_devs[j].wine_only) continue;
                lv = field(g_devs[j].first, "VENDOR-ID");
                lp = field(g_devs[j].first, "PRODUCT-ID");
                if (lv && lp && strtoul(lv, NULL, 16) == ven && strtoul(lp, NULL, 16) == prod && !g_devs[j].winid[0])
                    d = &g_devs[j];
            }
        if (d)
        {
            lstrcpynW(d->winid, inst, ARRAY_SIZE(d->winid));
            lstrcpynW(d->winservice, svc, ARRAY_SIZE(d->winservice));
            continue;
        }
        if (!(d = add_dev())) break;
        d->wine_only = TRUE;
        lstrcpynW(d->id, inst, ARRAY_SIZE(d->id));
        lstrcpynW(d->winid, inst, ARRAY_SIZE(d->winid));
        lstrcpynW(d->winservice, svc, ARRAY_SIZE(d->winservice));
        lstrcpynW(d->name, desc[0] ? desc : inst, ARRAY_SIZE(d->name));
        d->cat = CAT_OTHER;
        for (c = 0; c < NCATS; c++) if (CATS[c].winclass[0] && !_wcsicmp(cls, CATS[c].winclass)) d->cat = c;
        if (!_wcsicmp(cls, L"Image")) d->cat = cat_for("camera");
        if (!_wcsicmp(cls, L"MEDIA")) d->cat = cat_for("sound");
        if (!_wcsicmp(cls, L"Ports") || !_wcsicmp(cls, L"SoftwareDevice") || !_wcsicmp(cls, L"Volume") ||
            !_wcsicmp(cls, L"Volumesnapshot") || !cls[0])
            d->cat = cat_for("system");
    }
    SetupDiDestroyDeviceInfoList(set);
}

static void load(void)
{
    DWORD n = ARRAY_SIZE(g_computer);
    free(g_devs);
    g_devs = NULL;
    g_ndevs = 0;
    GetComputerNameW(g_computer, &n);
#ifndef SG_MUTANT_NOLINUX
    load_linux();
#endif
    load_setupapi();
}

/* ---- the tree ----------------------------------------------------------------------------- */

static HTREEITEM insert(HTREEITEM parent, const WCHAR *text, int icon, LPARAM param, BOOL warn)
{
    TVINSERTSTRUCTW is = { 0 };
    is.hParent = parent;
    is.hInsertAfter = TVI_SORT;
    is.item.mask = TVIF_TEXT | TVIF_IMAGE | TVIF_SELECTEDIMAGE | TVIF_PARAM | TVIF_STATE;
    is.item.pszText = (WCHAR *)text;
    is.item.iImage = is.item.iSelectedImage = icon;
    is.item.lParam = param;
    is.item.stateMask = TVIS_OVERLAYMASK;
    is.item.state = warn ? INDEXTOOVERLAYMASK(1) : 0;
    return (HTREEITEM)SendMessageW(g_dtree, TVM_INSERTITEMW, 0, (LPARAM)&is);
}

static int dev_index(const WCHAR *id)
{
    int i;
    for (i = 0; i < g_ndevs; i++) if (!wcscmp(g_devs[i].id, id)) return i;
    return -1;
}

static HTREEITEM place_by_connection(int i, int depth)
{
    dev_t_ *d = &g_devs[i];
    HTREEITEM parent = g_root;
    int p;
    if (d->hti) return d->hti;
    if (depth < 16 && d->parent[0] && (p = dev_index(d->parent)) >= 0 && p != i) parent = place_by_connection(p, depth + 1);
    return d->hti = insert(parent, d->name, d->nodriver ? IC_NODRIVER : CATS[d->cat].icon, i + 1, d->nodriver);
}

static void fill_tree(void)
{
    int i, c;
    WCHAR title[128];
    SendMessageW(g_dtree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(g_dtree);
    for (i = 0; i < g_ndevs; i++) g_devs[i].hti = NULL;
    lstrcpynW(title, g_computer, ARRAY_SIZE(title));
    g_root = insert(TVI_ROOT, title, IC_COMPUTER, 0, FALSE);
    memset(g_cat_items, 0, sizeof(g_cat_items));
    if (g_by_connection)
        for (i = 0; i < g_ndevs; i++) place_by_connection(i, 0);
    else
        for (c = 0; c < NCATS; c++)
        {
            BOOL any = FALSE, warn = FALSE;
            for (i = 0; i < g_ndevs; i++) if (g_devs[i].cat == c) { any = TRUE; warn |= g_devs[i].nodriver; }
            if (!any) continue;
            g_cat_items[c] = insert(g_root, CATS[c].title, CATS[c].icon, -(c + 1), FALSE);
            for (i = 0; i < g_ndevs; i++)
                if (g_devs[i].cat == c)
                    g_devs[i].hti = insert(g_cat_items[c], g_devs[i].name, g_devs[i].nodriver ? IC_NODRIVER : CATS[c].icon,
                                           i + 1, g_devs[i].nodriver);
            /* Windows opens a category holding a device with a problem */
            if (warn) TreeView_Expand(g_dtree, g_cat_items[c], TVE_EXPAND);
        }
    TreeView_Expand(g_dtree, g_root, TVE_EXPAND);
    SendMessageW(g_dtree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_dtree, NULL, TRUE);
}

static LPARAM selected_key(void)
{
    HTREEITEM h = TreeView_GetSelection(g_dtree);
    TVITEMW it = { TVIF_PARAM, h };
    if (!h || !SendMessageW(g_dtree, TVM_GETITEMW, 0, (LPARAM)&it)) return 0;
    return it.lParam;
}

/* ---- Properties ----------------------------------------------------------------------------- */

static void line_value(dev_t_ *d, const char *key, WCHAR *out, int cch)
{
    const char *v = d->first >= 0 ? field(d->first, key) : NULL;
    utf8_to_w(v ? v : "", out, cch);
}

static void status_text(dev_t_ *d, WCHAR *out, int cch)
{
    WCHAR problem[256], sg[256];
    line_value(d, "PROBLEM", problem, 256);
    line_value(d, "SG-DRIVER", sg, 256);
    if (d->nodriver && sg[0])
        _snwprintf(out, cch, L"The drivers for this device are not installed. (Code 28)\r\n\r\n"
                             L"Stained Glass can install a driver for it: %ls. An administrator installs it with "
                             L"sg-drivers (third-party drivers).", sg);
    else if (d->nodriver)
        _snwprintf(out, cch, L"The drivers for this device are not installed. (Code 28)\r\n\r\n%ls",
                   problem[0] ? problem : L"No Linux driver is bound to this device.");
    else if (sg[0])
        _snwprintf(out, cch, L"This device is working properly.\r\n\r\nA better driver is available: %ls "
                             L"(sg-drivers can install it).", sg);
    else _snwprintf(out, cch, L"This device is working properly.");
    out[cch - 1] = 0;
}

enum { G_ICON = 1700, G_NAME, G_TYPE, G_MFG, G_LOC, G_STATUS, R_PROV, R_VER, R_FILE, R_LIC, R_DESC, R_MOD, R_WIN, X_LIST };
static int g_prop_dev;

static void set(HWND dlg, int id, const WCHAR *s) { SetDlgItemTextW(dlg, id, s && *s ? s : L"Not available"); }

static INT_PTR CALLBACK general_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp; (void)lp;
    if (msg == WM_INITDIALOG)
    {
        dev_t_ *d = &g_devs[g_prop_dev];
        WCHAR v[1024];
        SendDlgItemMessageW(dlg, G_ICON, STM_SETICON, (WPARAM)ImageList_GetIcon(g_icons32, CATS[d->cat].icon, ILD_NORMAL), 0);
        SetDlgItemTextW(dlg, G_NAME, d->name);
        set(dlg, G_TYPE, CATS[d->cat].title);
        line_value(d, "VENDOR", v, 1024);
        set(dlg, G_MFG, d->wine_only ? L"Wine" : v);
        line_value(d, "LOCATION", v, 1024);
        set(dlg, G_LOC, v);
        status_text(d, v, 1024);
        SetDlgItemTextW(dlg, G_STATUS, v);
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK driver_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp; (void)lp;
    if (msg == WM_INITDIALOG)
    {
        dev_t_ *d = &g_devs[g_prop_dev];
        WCHAR v[512], w[512];
        line_value(d, "MODULE-AUTHOR", v, 512);
        set(dlg, R_PROV, v[0] ? v : (d->wine_only ? L"Wine" : L""));
        line_value(d, "MODULE-VERSION", v, 512);
        line_value(d, "MODULE-SRCVERSION", w, 512);
        if (!v[0]) lstrcpyW(v, w);
        set(dlg, R_VER, v);
        line_value(d, "MODULE-FILE", v, 512);
        set(dlg, R_FILE, v);
        line_value(d, "MODULE-LICENSE", v, 512);
        set(dlg, R_LIC, v);
        line_value(d, "MODULE-DESCRIPTION", v, 512);
        set(dlg, R_DESC, v);
        line_value(d, "DRIVER", v, 512);
        line_value(d, "MODULE", w, 512);
        if (v[0] && w[0] && wcscmp(v, w)) { wcscat(v, L" (module "); wcscat(v, w); wcscat(v, L")"); }
        else if (!v[0]) lstrcpyW(v, d->nodriver ? L"(none)" : w);
        set(dlg, R_MOD, v);
        _snwprintf(w, ARRAY_SIZE(w), L"%ls%ls%ls", d->winid, d->winservice[0] ? L"  service: " : L"", d->winservice);
        set(dlg, R_WIN, w);
        return TRUE;
    }
    return FALSE;
}

static INT_PTR CALLBACK details_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp; (void)lp;
    if (msg == WM_INITDIALOG)
    {
        dev_t_ *d = &g_devs[g_prop_dev];
        HWND lv = GetDlgItem(dlg, X_LIST);
        LVCOLUMNW c = { LVCF_TEXT | LVCF_WIDTH };
        int i, row = 0;
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT);
        c.pszText = (WCHAR *)L"Property";
        c.cx = S(140);
        SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&c);
        c.pszText = (WCHAR *)L"Value";
        c.cx = S(360);
        SendMessageW(lv, LVM_INSERTCOLUMNW, 1, (LPARAM)&c);
        if (d->winid[0])
        {
            LVITEMW it = { LVIF_TEXT };
            it.iItem = row++;
            it.pszText = (WCHAR *)L"Windows instance ID";
            SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
            it.iSubItem = 1;
            it.pszText = d->winid;
            SendMessageW(lv, LVM_SETITEMTEXTW, it.iItem, (LPARAM)&it);
        }
        for (i = d->first; i >= 0 && i < d->last; i++)
        {
            WCHAR k[128], v[1024];
            char *sp = strchr(g_rep.lines[i], ' ');
            LVITEMW it = { LVIF_TEXT };
            size_t kl = sp ? (size_t)(sp - g_rep.lines[i]) : strlen(g_rep.lines[i]);
            char key[128];
            if (kl >= sizeof(key)) kl = sizeof(key) - 1;
            memcpy(key, g_rep.lines[i], kl);
            key[kl] = 0;
            utf8_to_w(key, k, 128);
            utf8_to_w(sp ? sp + 1 : "", v, 1024);
            it.iItem = row++;
            it.pszText = k;
            SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
            it.iSubItem = 1;
            it.pszText = v;
            SendMessageW(lv, LVM_SETITEMTEXTW, it.iItem, (LPARAM)&it);
        }
        return TRUE;
    }
    return FALSE;
}

static void props(int i)
{
    static dlgt_t g, r, x;
    PROPSHEETPAGEW pg[3] = { { 0 } };
    PROPSHEETHEADERW ph = { 0 };
    DWORD style = DS_SHELLFONT | WS_CHILD | WS_CAPTION;
    WCHAR title[300];
    int k;
    if (i < 0 || i >= g_ndevs) return;
    g_prop_dev = i;
    dlg_begin(&g, L"General", style, 252, 218);
    dlg_item(&g, NULL, ATOM_STATIC, L"", G_ICON, SS_ICON, 7, 7, 20, 20);
    dlg_item(&g, NULL, ATOM_STATIC, L"", G_NAME, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 40, 12, 205, 10);
    D_LABEL(&g, L"Device type:", 40, 36, 60);
    D_VALUE(&g, G_TYPE, 104, 36, 140);
    D_LABEL(&g, L"Manufacturer:", 40, 50, 60);
    D_VALUE(&g, G_MFG, 104, 50, 140);
    D_LABEL(&g, L"Location:", 40, 64, 60);
    D_VALUE(&g, G_LOC, 104, 64, 140);
    D_GROUP(&g, L"Device status", 7, 84, 238, 124);
    dlg_item(&g, NULL, ATOM_EDIT, L"", G_STATUS, ES_READONLY | ES_MULTILINE | WS_VSCROLL | WS_BORDER, 14, 98, 224, 102);
    dlg_begin(&r, L"Driver", style, 252, 218);
    D_LABEL(&r, L"Driver Provider:", 7, 12, 76);
    D_VALUE(&r, R_PROV, 88, 12, 157);
    D_LABEL(&r, L"Driver Version:", 7, 28, 76);
    D_VALUE(&r, R_VER, 88, 28, 157);
    D_LABEL(&r, L"Kernel driver:", 7, 44, 76);
    D_VALUE(&r, R_MOD, 88, 44, 157);
    D_LABEL(&r, L"Description:", 7, 60, 76);
    D_VALUE(&r, R_DESC, 88, 60, 157);
    D_LABEL(&r, L"License:", 7, 76, 76);
    D_VALUE(&r, R_LIC, 88, 76, 157);
    D_LABEL(&r, L"Driver file:", 7, 92, 76);
    D_EDITRO(&r, R_FILE, 88, 92, 157, 24, ES_MULTILINE);
    D_LABEL(&r, L"Windows device:", 7, 122, 76);
    D_EDITRO(&r, R_WIN, 88, 122, 157, 24, ES_MULTILINE);
    D_LABEL(&r, L"Drivers here are Linux kernel drivers. Third-party ones (NVIDIA, Broadcom Wi-Fi, ...) are "
               L"installed by an administrator through sg-drivers.", 7, 160, 238);
    dlg_begin(&x, L"Details", style, 252, 218);
    dlg_item(&x, WC_LISTVIEWW, 0, L"", X_LIST, LVS_REPORT | LVS_NOSORTHEADER | WS_BORDER | WS_TABSTOP, 7, 7, 238, 204);
    {
        dlgt_t *t[3] = { &g, &r, &x };
        DLGPROC procs[3] = { general_proc, driver_proc, details_proc };
        for (k = 0; k < 3; k++)
        {
            pg[k].dwSize = sizeof(pg[k]);
            pg[k].dwFlags = PSP_DLGINDIRECT;
            pg[k].hInstance = g_inst;
            pg[k].pResource = t[k]->t;
            pg[k].pfnDlgProc = procs[k];
        }
    }
    _snwprintf(title, ARRAY_SIZE(title), L"%ls Properties", g_devs[i].name);
    title[ARRAY_SIZE(title) - 1] = 0;
    ph.dwSize = sizeof(ph);
    ph.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW | PSH_NOCONTEXTHELP;
    ph.hwndParent = g_main;
    ph.hInstance = g_inst;
    ph.pszCaption = title;
    ph.nPages = 3;
    ph.ppsp = pg;
    EnableWindow(g_main, FALSE);
    frame_dump();
    PropertySheetW(&ph);
    EnableWindow(g_main, TRUE);
    frame_dump_later();
}

/* ---- the pane ------------------------------------------------------------------------------- */

static LRESULT CALLBACK pane_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_NOTIFY && ((NMHDR *)lp)->hwndFrom == g_dtree)
    {
        NMHDR *nh = (NMHDR *)lp;
        LPARAM key;
        switch (nh->code)
        {
        case TVN_SELCHANGEDW:
            key = selected_key();
            frame_custom_selection(key, key > 0, key > 0 ? g_devs[key - 1].name : NULL);
            return 0;
        case NM_DBLCLK:
            if ((key = selected_key()) > 0) props((int)key - 1);
            return 0;
        case TVN_KEYDOWN:
            if (((NMTVKEYDOWN *)lp)->wVKey == VK_RETURN && (key = selected_key()) > 0) props((int)key - 1);
            return 0;
        case NM_RCLICK:
        {
            TVHITTESTINFO ht = { 0 };
            HMENU m = CreatePopupMenu();
            int cmd;
            GetCursorPos(&ht.pt);
            ScreenToClient(g_dtree, &ht.pt);
            if (SendMessageW(g_dtree, TVM_HITTEST, 0, (LPARAM)&ht) && ht.hItem) TreeView_SelectItem(g_dtree, ht.hItem);
            AppendMenuW(m, MF_STRING, V_SCAN, L"Scan for hardware &changes");
            if ((key = selected_key()) > 0)
            {
                AppendMenuW(m, MF_SEPARATOR, 0, NULL);
                AppendMenuW(m, MF_STRING, V_PROPS, L"P&roperties");
                SetMenuDefaultItem(m, V_PROPS, FALSE);
            }
            GetCursorPos(&ht.pt);
            cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, ht.pt.x, ht.pt.y, 0, g_main, NULL);
            DestroyMenu(m);
            if (cmd == V_PROPS) props((int)key - 1);
            else if (cmd == V_SCAN) { load(); fill_tree(); frame_dump_later(); }
            return TRUE;
        }
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ensure(void)
{
    WNDCLASSW wc = { 0 };
    if (g_pane) return;
    wc.lpfnWndProc = pane_proc;
    wc.hInstance = g_inst;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"SgDevicePane";
    RegisterClassW(&wc);
    g_pane = CreateWindowExW(0, L"SgDevicePane", NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, g_main, NULL, g_inst, NULL);
    g_dtree = CreateWindowExW(0, WC_TREEVIEWW, NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
                              TVS_LINESATROOT | TVS_SHOWSELALWAYS, 0, 0, 0, 0, g_pane, NULL, g_inst, NULL);
    ImageList_SetOverlayImage(g_icons, IC_NODRIVER, 1);
    TreeView_SetImageList(g_dtree, g_icons, TVSIL_NORMAL);
    SendMessageW(g_dtree, WM_SETFONT, (WPARAM)g_font, 0);
}

static void dev_layout(node_t *n, const RECT *rc)
{
    (void)n;
    ensure();
    MoveWindow(g_pane, rc->left, rc->top, rc->right - rc->left, rc->bottom - rc->top, TRUE);
    MoveWindow(g_dtree, 0, 0, rc->right - rc->left, rc->bottom - rc->top, TRUE);
}

static void dev_show(node_t *n)
{
    int i, bad = 0;
    (void)n;
    ensure();
    pane_show_list(FALSE);
    ShowWindow(g_pane, SW_SHOW);
    load();
    fill_tree();
    SetFocus(g_dtree);
    for (i = 0; i < g_ndevs; i++) bad += g_devs[i].nodriver;
    if (!sys_bridged())
        frame_banner(L"The Linux side is not connected (sg-sysinfo): only the devices Wine knows are shown.");
    else if (bad)
        frame_banner(bad == 1 ? L"One device has no driver. Open it to see what Stained Glass can install."
                              : L"Some devices have no driver. Open them to see what Stained Glass can install.");
    else if (!is_admin())
        frame_banner(L"You are logged on as a standard user. You can view device settings in Device Manager, "
                     L"but you must be logged on as an administrator to make changes.");
    frame_status(L"%d devices", g_ndevs);
    frame_custom_selection(0, FALSE, NULL);
}

static void dev_hide(node_t *n)
{
    (void)n;
    if (g_pane) ShowWindow(g_pane, SW_HIDE);
    pane_show_list(TRUE);
}

static void dev_verbs(node_t *n, LPARAM key, BOOL have, verbs_t *out)
{
    (void)n; (void)key;
    if (have) return;
    out->v[out->n++] = (verb_t){ V_SCAN, L"Scan for hardware &changes", IC_SCAN, TRUE, FALSE };
    out->v[out->n++] = (verb_t){ V_BYTYPE, L"Devices by &type", -1, g_by_connection, TRUE };
    out->v[out->n++] = (verb_t){ V_BYCONN, L"Devices by co&nnection", -1, !g_by_connection, FALSE };
}

static void dev_invoke(node_t *n, LPARAM key, BOOL have, int verb)
{
    (void)n; (void)key; (void)have;
    switch (verb)
    {
    case V_SCAN: load(); fill_tree(); break;
    case V_BYTYPE: g_by_connection = FALSE; fill_tree(); break;
    case V_BYCONN: g_by_connection = TRUE; fill_tree(); break;
    }
    frame_update_verbs();
}

static void dev_open(node_t *n, LPARAM key)
{
    (void)n;
    if (key > 0) props((int)key - 1);
}

static void dev_dump(node_t *n, FILE *f)
{
    int i;
    (void)n;
    fprintf(f, "BYCONNECTION %d\n", g_by_connection);
    for (i = 0; i < g_ndevs; i++)
    {
        RECT r;
        POINT pt = { -1, -1 };
        WCHAR drv[64];
        memcpy(&r, &g_devs[i].hti, sizeof(HTREEITEM));
        if (g_devs[i].hti && SendMessageW(g_dtree, TVM_GETITEMRECT, TRUE, (LPARAM)&r)) screen_center(g_dtree, &r, &pt);
        line_value(&g_devs[i], "DRIVER", drv, 64);
        fprintf(f, "DEV %d\t%ld %ld\t%ls\t%ls\t%ls\t%s\t%ls\t%ls\n", i + 1, pt.x, pt.y, CATS[g_devs[i].cat].title,
                g_devs[i].name, g_devs[i].id, g_devs[i].nodriver ? "nodriver" : g_devs[i].wine_only ? "wine" : "ok",
                drv, g_devs[i].winid);
    }
    for (i = 0; i < NCATS; i++)
        if (g_cat_items[i])
        {
            RECT r;
            POINT pt = { -1, -1 };
            memcpy(&r, &g_cat_items[i], sizeof(HTREEITEM));
            if (SendMessageW(g_dtree, TVM_GETITEMRECT, TRUE, (LPARAM)&r)) screen_center(g_dtree, &r, &pt);
            fprintf(f, "CATEGORY %ld %ld %ls\n", pt.x, pt.y, CATS[i].title);
        }
    if (IsWindowVisible(g_main) && !IsWindowEnabled(g_main) && g_prop_dev < g_ndevs)
    {
        WCHAR st[1024], *p, drv[128];
        status_text(&g_devs[g_prop_dev], st, ARRAY_SIZE(st));
        for (p = st; *p; p++) if (*p == '\r' || *p == '\n') *p = ' ';
        line_value(&g_devs[g_prop_dev], "MODULE", drv, 128);
        fprintf(f, "PROPS %ls\nPROPSTATUS %ls\nPROPMODULE %ls\n", g_devs[g_prop_dev].name, st, drv);
    }
}

static const snapin_t dev_ops = {
    NULL, dev_show, dev_verbs, dev_invoke, dev_open, NULL, NULL, dev_layout, dev_hide, dev_dump
};

node_t *devices_create(node_t *parent)
{
    node_t *n = node_add(parent, L"Device Manager", IC_DEVMGR, &dev_ops, NULL);
    lstrcpyW(n->desc, L"The hardware in this computer and its drivers");
    return n;
}
