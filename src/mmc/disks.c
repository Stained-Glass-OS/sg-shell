/* sg-mmc -- Disk Management (diskmgmt.msc): the disks and volumes, as Windows
 * lays them out -- the volume list above, and below, each disk as a row: its
 * label box (Disk 0, Basic, size, Online) and a bar of its partitions and
 * unallocated space, drawn to scale.
 *
 * The disks are Linux's (sg-sysinfo disks: lsblk, and which of them the Wine
 * prefix maps to drive letters). Changes -- a drive letter, mounting,
 * formatting -- are requests to sg-sysinfod, which lets only an
 * administrator make them and refuses anything on the system disk or in use
 * (see sg-session); the console only asks and reports the answer. So are New
 * Simple Volume (in unallocated space), Delete Volume, Extend and Shrink Volume
 * (NTFS and ext4, as Windows resizes NTFS only), and each disk's health
 * (SMART, `sg-sysinfo smart`): the label box says "Online (Errors)" for a
 * failing disk, and the disk's Properties show what SMART reports.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "mmc.h"
#include <string.h>

typedef struct seg
{
    int kind;                   /* 0 partition, 1 unallocated */
    int line;                   /* its block in the reply */
    int disk;                   /* index into g_disks */
    ULONGLONG start, size;
    RECT rc;                    /* on the graphical pane */
} seg_t;

typedef struct disk
{
    int line;
    int number;
    ULONGLONG size;
    BOOL cdrom, system, removable;
    RECT label, bar;
} disk_t;

static sys_reply_t g_rep, g_smart;
static disk_t g_disks[64];
static int g_ndisks;
static seg_t g_segs[512];
static int g_nsegs;
static HWND g_graph;
static int g_selseg = -1;
static BOOL g_admin;
enum { V_RESCAN = 1, V_OPEN, V_LETTER, V_FORMAT, V_MOUNT, V_UNMOUNT, V_PROPS, V_NEW, V_DELETE, V_EXTEND, V_SHRINK };
/* keys: a volume or unallocated space is its segment + 1; a disk's label box DISK_KEY + its index */
#define DISK_KEY 10000
static int g_seldisk = -1;

static const char *f(int line, const char *key) { return sys_field(&g_rep, line, key); }

static ULONGLONG num(int line, const char *key)
{
    const char *v = f(line, key);
    return v ? strtoull(v, NULL, 10) : 0;
}

static BOOL has_flag(int line, const char *flag)
{
    const char *v = f(line, "FLAGS");
    char buf[128], *t, *ctx;
    if (!v) return FALSE;
    lstrcpynA(buf, v, sizeof(buf));
    for (t = strtok_s(buf, " ", &ctx); t; t = strtok_s(NULL, " ", &ctx)) if (!strcmp(t, flag)) return TRUE;
    return FALSE;
}

/* a disk's SMART health: "ok", "failing", "unsupported", "unknown" (or "" when not asked) */
static const char *disk_health(int d, int *block)
{
    int b;
    const char *name = g_rep.lines[g_disks[d].line] + 5;
    for (b = sys_next_block(&g_smart, 0, "SMART"); b >= 0; b = sys_next_block(&g_smart, b + 1, "SMART"))
        if (!strcmp(g_smart.lines[b] + 6, name))
        {
            const char *h = sys_field(&g_smart, b, "HEALTH");
            if (block) *block = b;
            return h ? h : "unknown";
        }
    if (block) *block = -1;
    return "";
}

static BOOL resizable_fs(int line)
{
    const char *v = sys_field(&g_rep, line, "FSTYPE");
    return v && (!strcmp(v, "ntfs") || !strcmp(v, "ntfs3") || !strcmp(v, "ext4") || !strcmp(v, "ext3") ||
                 !strcmp(v, "ext2"));
}

/* the unallocated space right after segment s on its disk, in bytes */
static ULONGLONG free_after(int s)
{
    int i;
    ULONGLONG end = g_segs[s].start + g_segs[s].size;
    for (i = 0; i < g_nsegs; i++)
        if (g_segs[i].kind && g_segs[i].disk == g_segs[s].disk && g_segs[i].start >= end &&
            g_segs[i].start - end < 1024 * 1024)
            return g_segs[i].size;
    return 0;
}

/* the first LETTER of a partition, "" if none */
static void letter_of(int line, WCHAR *out, int cch)
{
    const char *v = f(line, "LETTER");
    utf8_to_w(v ? v : "", out, cch);
}

static void fs_name(int line, WCHAR *out, int cch)
{
    const char *v = f(line, "FSTYPE");
    if (!v) { out[0] = 0; return; }
    if (!strcmp(v, "vfat")) lstrcpynW(out, L"FAT32", cch);
    else if (!strcmp(v, "exfat")) lstrcpynW(out, L"exFAT", cch);
    else if (!strcmp(v, "ntfs") || !strcmp(v, "ntfs3")) lstrcpynW(out, L"NTFS", cch);
    else if (!strcmp(v, "swap")) lstrcpynW(out, L"Linux swap", cch);
    else if (!strcmp(v, "crypto_LUKS")) lstrcpynW(out, L"Encrypted (LUKS)", cch);
    else utf8_to_w(v, out, cch);
}

static void volume_name(int line, WCHAR *out, int cch)
{
    WCHAR label[128], letter[8];
    const char *l = f(line, "LABEL");
    utf8_to_w(l ? l : "", label, 128);
    letter_of(line, letter, 8);
    if (letter[0]) _snwprintf(out, cch, L"%ls (%ls)", label[0] ? label : L"Local Disk", letter);
    else if (label[0]) lstrcpynW(out, label, cch);
    else
    {
        int d = 0, i;
        const char *disk = f(line, "DISK");
        for (i = 0; i < g_ndisks; i++) if (disk && !strcmp(g_rep.lines[g_disks[i].line] + 5, disk)) d = g_disks[i].number;
        _snwprintf(out, cch, L"(Disk %d partition %llu)", d, num(line, "NUMBER"));
    }
    out[cch - 1] = 0;
}

static void status_text(int line, WCHAR *out, int cch)
{
    WCHAR parts[128] = L"";
    BOOL mounted = f(line, "MOUNT") != NULL;
    if (has_flag(line, "esp")) wcscat(parts, L"EFI System Partition");
    else
    {
        if (has_flag(line, "system")) wcscat(parts, L"System, ");
        if (has_flag(line, "boot")) wcscat(parts, L"Boot, ");
        if (has_flag(line, "swap")) wcscat(parts, L"Page File, ");
        wcscat(parts, L"Primary Partition");
    }
    _snwprintf(out, cch, L"%ls (%ls)", mounted ? L"Healthy" : L"Healthy, not mounted", parts);
    out[cch - 1] = 0;
}

/* a block's first line (DISK/PART/FREE), not a key inside one (a PART's "DISK sda") */
static BOOL block_start(int i)
{
    return i == 0 || !strcmp(g_rep.lines[i - 1], "END") || !strncmp(g_rep.lines[i - 1], "MAP ", 4);
}

static void load(void)
{
    int b, i;
    sys_reply_t who;
    sys_free(&g_rep);
    g_ndisks = g_nsegs = 0;
    g_selseg = -1;
    g_seldisk = -1;
    sys_request(&g_rep, "disks", NULL);
    for (i = 0; i < g_rep.nlines && g_ndisks < (int)ARRAY_SIZE(g_disks); i++)
        if (!strncmp(g_rep.lines[i], "DISK ", 5) && block_start(i))
        {
            disk_t *d = &g_disks[g_ndisks];
            const char *t = f(i, "TYPE");
            memset(d, 0, sizeof(*d));
            d->line = i;
            d->size = num(i, "SIZE");
            d->cdrom = t && !strcmp(t, "cdrom");
            d->system = f(i, "SYSTEM") && !strcmp(f(i, "SYSTEM"), "yes");
            d->removable = f(i, "REMOVABLE") && !strcmp(f(i, "REMOVABLE"), "yes");
            d->number = g_ndisks;
            g_ndisks++;
        }
    for (b = 0; b < g_rep.nlines && g_nsegs < (int)ARRAY_SIZE(g_segs); b++)
    {
        BOOL part = !strncmp(g_rep.lines[b], "PART ", 5), fr = !strncmp(g_rep.lines[b], "FREE ", 5);
        const char *disk;
        seg_t *s;
        if ((!part && !fr) || !block_start(b)) continue;
        disk = part ? f(b, "DISK") : g_rep.lines[b] + 5;
        s = &g_segs[g_nsegs];
        memset(s, 0, sizeof(*s));
        s->kind = fr;
        s->line = b;
        s->disk = -1;
        for (i = 0; i < g_ndisks; i++) if (disk && !strcmp(g_rep.lines[g_disks[i].line] + 5, disk)) s->disk = i;
        if (s->disk < 0) continue;
        s->start = num(b, "START");
        s->size = num(b, "SIZE");
        g_nsegs++;
    }
    /* each disk's pieces in the order they lie on it */
    for (b = 1; b < g_nsegs; b++)
    {
        seg_t t = g_segs[b];
        for (i = b; i > 0 && (g_segs[i - 1].disk > t.disk || (g_segs[i - 1].disk == t.disk && g_segs[i - 1].start > t.start)); i--)
            g_segs[i] = g_segs[i - 1];
        g_segs[i] = t;
    }
    sys_free(&g_smart);
    sys_request(&g_smart, "smart", NULL);
    sys_request(&who, "whoami", NULL);
    g_admin = who.ok && sys_field(&who, 0, "ADMIN") && !strcmp(sys_field(&who, 0, "ADMIN"), "yes");
    sys_free(&who);
}

/* ---- the volume list ---------------------------------------------------------------------- */

static void fill_list(void)
{
    static const WCHAR *const cols[] = { L"Volume", L"Layout", L"Type", L"File System", L"Status", L"Capacity",
                                         L"Free Space", L"% Free" };
    static const int widths[] = { 190, 60, 60, 90, 260, 90, 90, 60 };
    int i;
    pane_columns(cols, widths, 8);
    pane_numeric(7);
    pane_begin();
    for (i = 0; i < g_nsegs; i++)
    {
        WCHAR c[8][256];
        const WCHAR *p[8];
        int k, line = g_segs[i].line;
        ULONGLONG fsize, favail;
        if (g_segs[i].kind) continue;
        volume_name(line, c[0], 256);
        lstrcpyW(c[1], L"Simple");
        lstrcpyW(c[2], L"Basic");
        fs_name(line, c[3], 256);
        status_text(line, c[4], 256);
        fmt_bytes(g_segs[i].size, c[5], 256);
        fsize = num(line, "FSSIZE");
        favail = num(line, "FSAVAIL");
        if (fsize) { fmt_bytes(favail, c[6], 256); _snwprintf(c[7], 256, L"%d %%", (int)(favail * 100 / fsize)); }
        else { c[6][0] = 0; c[7][0] = 0; }
        for (k = 0; k < 8; k++) p[k] = c[k];
        pane_add(i + 1, IC_DISK, p);
    }
    pane_end();
}

/* ---- the graphical view -------------------------------------------------------------------- */

#define ROW_H  S(78)
#define LABEL_W S(128)

static void layout_graph(void)
{
    RECT rc;
    int i, y = S(8), s;
    GetClientRect(g_graph, &rc);
    for (i = 0; i < g_ndisks; i++)
    {
        disk_t *d = &g_disks[i];
        int x0, w, n = 0, minw = S(70), avail, x;
        ULONGLONG total = 0;
        SetRect(&d->label, S(8), y, S(8) + LABEL_W, y + ROW_H);
        SetRect(&d->bar, d->label.right + S(2), y, rc.right - S(8), y + ROW_H);
        for (s = 0; s < g_nsegs; s++) if (g_segs[s].disk == i) { n++; total += g_segs[s].size; }
        x0 = d->bar.left;
        w = d->bar.right - d->bar.left;
        avail = w - n * minw;
        if (avail < 0) avail = 0;
#ifdef SG_MUTANT_SCALE
        avail = 0;
#endif
        x = x0;
        for (s = 0; s < g_nsegs; s++)
        {
            seg_t *g = &g_segs[s];
            int sw;
            if (g->disk != i) continue;
            sw = minw + (total ? (int)((double)avail * g->size / total) : 0);
            SetRect(&g->rc, x, y, x + sw, y + ROW_H);
            x += sw;
        }
        if (n && x < d->bar.right)
            for (s = g_nsegs - 1; s >= 0; s--) if (g_segs[s].disk == i) { g_segs[s].rc.right = d->bar.right; break; }
        y += ROW_H + S(10);
    }
}

static void draw_text_lines(HDC dc, RECT r, const WCHAR *a, const WCHAR *b, const WCHAR *c, BOOL bold)
{
    int lh = S(16);
    r.left += S(6);
    r.right -= S(4);
    r.top += S(10);
    SelectObject(dc, bold ? g_font_bold : g_font);
    DrawTextW(dc, a, -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    r.top += lh;
    SelectObject(dc, g_font);
    DrawTextW(dc, b, -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    r.top += lh;
    DrawTextW(dc, c, -1, &r, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void paint_graph(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT rc;
    int i, s;
    HBRUSH bg = CreateSolidBrush(C_BG), lb = CreateSolidBrush(C_SURFACE), line = CreateSolidBrush(C_LINE),
           head = CreateSolidBrush(C_ACCENT), free_b = CreateSolidBrush(RGB(40, 40, 40)),
           sys_b = CreateSolidBrush(RGB(60, 90, 160)), sel = CreateSolidBrush(C_ACCENT_LT);
    GetClientRect(h, &rc);
    FillRect(dc, &rc, bg);
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < g_ndisks; i++)
    {
        disk_t *d = &g_disks[i];
        WCHAR a[64], b[64], c[64], size[32];
        FillRect(dc, &d->label, i == g_seldisk ? sel : lb);
        FrameRect(dc, &d->label, line);
        fmt_bytes(d->size, size, 32);
        if (d->cdrom)
        {
            _snwprintf(a, 64, L"CD-ROM %d", i);
            lstrcpyW(b, L"DVD");
            lstrcpyW(c, d->size ? size : L"No Media");
            ImageList_Draw(g_icons, IC_CDROM, dc, d->label.left + S(6), d->label.top + S(10), ILD_NORMAL);
        }
        else
        {
            _snwprintf(a, 64, L"Disk %d", i);
            _snwprintf(b, 64, d->removable ? L"Removable" : L"Basic");
            _snwprintf(c, 64, L"%ls", size);
            ImageList_Draw(g_icons, IC_DISK, dc, d->label.left + S(6), d->label.top + S(10), ILD_NORMAL);
        }
        {
            RECT t = d->label;
            t.left += S(24);
            SetTextColor(dc, C_TEXT);
            draw_text_lines(dc, t, a, b, c, TRUE);
            t.top += S(58);
            t.left += S(6);
            SelectObject(dc, g_font);
            if (!d->cdrom && !strcmp(disk_health(i, NULL), "failing"))
            {
                SetTextColor(dc, RGB(196, 43, 28));
                DrawTextW(dc, L"Online (Errors)", -1, &t, DT_SINGLELINE | DT_NOPREFIX);
                SetTextColor(dc, C_TEXT);
            }
            else DrawTextW(dc, L"Online", -1, &t, DT_SINGLELINE | DT_NOPREFIX);
        }
        for (s = 0; s < g_nsegs; s++)
        {
            seg_t *g = &g_segs[s];
            RECT r = g->rc, hd = g->rc;
            WCHAR n1[256], n2[128], n3[256], fs[64];
            if (g->disk != i) continue;
            if (s == g_selseg) FillRect(dc, &r, sel);
            FrameRect(dc, &r, line);
            hd.bottom = hd.top + S(8);
            FillRect(dc, &hd, g->kind ? free_b : has_flag(g->line, "system") ? sys_b : head);
            fmt_bytes(g->size, size, 32);
            SetTextColor(dc, C_TEXT);
            if (g->kind)
            {
                draw_text_lines(dc, r, size, L"Unallocated", L"", FALSE);
                continue;
            }
            volume_name(g->line, n1, 256);
            fs_name(g->line, fs, 64);
            _snwprintf(n2, 128, L"%ls %ls", size, fs);
            status_text(g->line, n3, 256);
            draw_text_lines(dc, r, n1, n2, n3, TRUE);
            if (s == g_selseg)
            {
                HPEN pen = CreatePen(PS_DOT, 1, C_TEXT);
                HGDIOBJ op = SelectObject(dc, pen), ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, r.left + 2, r.top + S(10), r.right - 2, r.bottom - 2);
                SelectObject(dc, op);
                SelectObject(dc, ob);
                DeleteObject(pen);
            }
        }
    }
    if (!g_ndisks)
    {
        SetTextColor(dc, C_SUBTLE);
        SelectObject(dc, g_font);
        rc.top += S(20);
        DrawTextW(dc, sys_bridged() ? L"No disks were found." : L"The Linux side is not connected (sg-sysinfo).", -1, &rc,
                  DT_CENTER | DT_SINGLELINE);
    }
    DeleteObject(bg); DeleteObject(lb); DeleteObject(line); DeleteObject(head); DeleteObject(free_b);
    DeleteObject(sys_b); DeleteObject(sel);
    EndPaint(h, &ps);
}

static void select_seg(int s, BOOL from_list)
{
    g_selseg = s;
    g_seldisk = -1;
    InvalidateRect(g_graph, NULL, FALSE);
    if (!from_list)
    {
        if (s >= 0 && !g_segs[s].kind) pane_select_key(s + 1);
        else ListView_SetItemState(pane_list(), -1, 0, LVIS_SELECTED);
    }
    /* unallocated space has no row in the list: its verbs (New Simple Volume) come
     * from the graph's own selection */
    if (s >= 0 && g_segs[s].kind) frame_custom_selection(s + 1, TRUE, L"Unallocated");
    else frame_update_verbs();
}

static void select_disk(int d)
{
    WCHAR name[32];
    g_selseg = -1;
    g_seldisk = d;
    InvalidateRect(g_graph, NULL, FALSE);
    ListView_SetItemState(pane_list(), -1, 0, LVIS_SELECTED);
    _snwprintf(name, 32, L"Disk %d", d);
    frame_custom_selection(DISK_KEY + d, TRUE, name);
}

static void verbs_menu(int s);
static void disk_open(node_t *n, LPARAM key);

static LRESULT CALLBACK graph_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT: paint_graph(h); return 0;
    case WM_SIZE: layout_graph(); InvalidateRect(h, NULL, FALSE); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_LBUTTONDBLCLK:
    {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int s, hit = -1, d, dhit = -1;
        for (s = 0; s < g_nsegs; s++) if (PtInRect(&g_segs[s].rc, pt)) hit = s;
        for (d = 0; d < g_ndisks; d++) if (PtInRect(&g_disks[d].label, pt)) dhit = d;
        SetFocus(h);
        if (dhit >= 0)
        {
            select_disk(dhit);
            if (msg == WM_RBUTTONUP) verbs_menu(DISK_KEY + dhit - 1);
            if (msg == WM_LBUTTONDBLCLK) node_current()->ops->open(node_current(), DISK_KEY + dhit);
            return 0;
        }
        select_seg(hit, FALSE);
        if (msg == WM_RBUTTONUP && hit >= 0) verbs_menu(hit);
        if (msg == WM_LBUTTONDBLCLK && hit >= 0 && !g_segs[hit].kind) node_current()->ops->open(node_current(), hit + 1);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- doing things ---------------------------------------------------------------------------- */

static void part_path(int s, char *out, size_t cch)
{
    const char *p = g_rep.lines[g_segs[s].line] + 5;
    snprintf(out, cch, "%s", p);
}

static BOOL request_report(const WCHAR *what, int argc, const char *const *argv)
{
    sys_reply_t r;
    sys_request_argv(&r, argc, argv);
    if (!r.ok)
    {
        WCHAR m[512];
        utf8_to_w(r.message, m, 512);
        if (!strcmp(r.kind, "denied"))
            frame_message(MB_OK | MB_ICONERROR, L"Disk Management",
                          L"%ls could not be done.\n\nYou need to be an administrator to change disks and volumes. "
                          L"Run Disk Management as an administrator.\n\n(%ls)", what, m);
        else frame_message(MB_OK | MB_ICONERROR, L"Disk Management", L"%ls could not be done.\n\n%ls", what, m);
        sys_free(&r);
        return FALSE;
    }
    sys_free(&r);
    return TRUE;
}

enum { L_LETTER = 1800, L_REMOVE, L_ASSIGN, F_LABEL = 1810, F_FS, F_QUICK };

static INT_PTR CALLBACK letter_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    WCHAR *cur = (WCHAR *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        DWORD used = GetLogicalDrives();
        int c, sel = -1;
        WCHAR l[4];
        cur = (WCHAR *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        for (c = 'D'; c <= 'Y'; c++)
        {
            if ((used & (1u << (c - 'A'))) && !(cur[0] == c)) continue;
            _snwprintf(l, 4, L"%lc:", c);
            SendDlgItemMessageW(dlg, L_LETTER, CB_ADDSTRING, 0, (LPARAM)l);
            if (cur[0] == c) sel = (int)SendDlgItemMessageW(dlg, L_LETTER, CB_GETCOUNT, 0, 0) - 1;
        }
        SendDlgItemMessageW(dlg, L_LETTER, CB_SETCURSEL, sel >= 0 ? sel : 0, 0);
        CheckRadioButton(dlg, L_ASSIGN, L_REMOVE, L_ASSIGN);
        EnableWindow(GetDlgItem(dlg, L_REMOVE), cur[0] != 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK)
        {
            if (IsDlgButtonChecked(dlg, L_REMOVE)) lstrcpyW(cur, L"none");
            else GetDlgItemTextW(dlg, L_LETTER, cur, 8);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static void change_letter(int s)
{
    static dlgt_t d;
    WCHAR cur[16], title[300], name[256];
    char part[64], *l;
    const char *argv[3];
    letter_of(g_segs[s].line, cur, 16);
    volume_name(g_segs[s].line, name, 256);
    _snwprintf(title, ARRAY_SIZE(title), L"Change Drive Letter and Paths for %ls", name);
    dlg_begin(&d, title, 0, 250, 96);
    D_LABEL(&d, L"Allow access to this volume by using the following drive letter:", 7, 8, 236);
    dlg_item(&d, NULL, ATOM_BUTTON, L"&Assign the following drive letter:", L_ASSIGN,
             BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 12, 26, 140, 10);
    dlg_item(&d, NULL, ATOM_COMBO, L"", L_LETTER, CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 160, 24, 50, 120);
    dlg_item(&d, NULL, ATOM_BUTTON, L"&Remove the drive letter", L_REMOVE, BS_AUTORADIOBUTTON | WS_TABSTOP, 12, 44, 140, 10);
    dlg_item(&d, NULL, ATOM_BUTTON, L"OK", IDOK, BS_DEFPUSHBUTTON | WS_TABSTOP, 136, 74, 50, 14);
    D_BUTTON(&d, L"Cancel", IDCANCEL, 192, 74, 50);
    if (DialogBoxIndirectParamW(g_inst, d.t, g_main, letter_proc, (LPARAM)cur) != IDOK) return;
    part_path(s, part, sizeof(part));
    l = w_to_utf8(cur);
    argv[0] = "letter";
    argv[1] = part;
    argv[2] = l;
    request_report(L"Changing the drive letter", 3, argv);
    free(l);
    node_select(node_current());
}

typedef struct fmt { WCHAR label[64]; WCHAR fs[16]; } fmt_t;

static INT_PTR CALLBACK format_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    fmt_t *ft = (fmt_t *)GetWindowLongPtrW(dlg, DWLP_USER);
    static const WCHAR *const fss[] = { L"NTFS", L"exFAT", L"FAT32", L"ext4" };
    int i;
    switch (msg)
    {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        ft = (fmt_t *)lp;
        SetDlgItemTextW(dlg, F_LABEL, ft->label);
        for (i = 0; i < 4; i++) SendDlgItemMessageW(dlg, F_FS, CB_ADDSTRING, 0, (LPARAM)fss[i]);
        SendDlgItemMessageW(dlg, F_FS, CB_SETCURSEL, 0, 0);
        CheckDlgButton(dlg, F_QUICK, BST_CHECKED);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK)
        {
            GetDlgItemTextW(dlg, F_LABEL, ft->label, ARRAY_SIZE(ft->label));
            i = (int)SendDlgItemMessageW(dlg, F_FS, CB_GETCURSEL, 0, 0);
#ifdef SG_MUTANT_FSNAME
            i = 0;
#endif
            lstrcpyW(ft->fs, fss[i >= 0 && i < 4 ? i : 0]);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static void format_volume(int s)
{
    static dlgt_t d;
    fmt_t ft = { L"", L"" };
    WCHAR name[256], title[300];
    const char *lab = f(g_segs[s].line, "LABEL");
    char part[64], *label, fs[16];
    const char *argv[5];
    int argc = 0;
    volume_name(g_segs[s].line, name, 256);
    utf8_to_w(lab ? lab : "", ft.label, 64);
    _snwprintf(title, ARRAY_SIZE(title), L"Format %ls", name);
    dlg_begin(&d, title, 0, 220, 104);
    dlg_item(&d, NULL, ATOM_STATIC, L"&Volume label:", 0xFFFF, SS_LEFT, 7, 10, 70, 8);
    dlg_item(&d, NULL, ATOM_EDIT, L"", F_LABEL, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 80, 8, 132, 13);
    dlg_item(&d, NULL, ATOM_STATIC, L"&File system:", 0xFFFF, SS_LEFT, 7, 30, 70, 8);
    dlg_item(&d, NULL, ATOM_COMBO, L"", F_FS, CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 80, 28, 132, 80);
    dlg_item(&d, NULL, ATOM_BUTTON, L"&Perform a quick format", F_QUICK, BS_AUTOCHECKBOX | WS_DISABLED, 7, 52, 200, 10);
    dlg_item(&d, NULL, ATOM_BUTTON, L"OK", IDOK, BS_DEFPUSHBUTTON | WS_TABSTOP, 106, 82, 50, 14);
    D_BUTTON(&d, L"Cancel", IDCANCEL, 162, 82, 50);
    if (DialogBoxIndirectParamW(g_inst, d.t, g_main, format_proc, (LPARAM)&ft) != IDOK) return;
    if (frame_message(MB_OKCANCEL | MB_ICONWARNING, L"Format",
                      L"Formatting this volume will erase all data on it. Back up any data you want to keep before "
                      L"formatting. Do you want to continue?") != IDOK)
        return;
    part_path(s, part, sizeof(part));
    WideCharToMultiByte(CP_UTF8, 0, ft.fs, -1, fs, sizeof(fs), NULL, NULL);
    CharLowerA(fs);
    label = w_to_utf8(ft.label);
    argv[argc++] = "format";
    argv[argc++] = part;
    argv[argc++] = fs;
    if (label && *label) { argv[argc++] = "--label"; argv[argc++] = label; }
    request_report(L"Formatting", argc, argv);
    free(label);
    node_select(node_current());
}

static void mount_volume(int s, BOOL mount)
{
    char part[64];
    const char *argv[2] = { mount ? "mount" : "unmount", part };
    part_path(s, part, sizeof(part));
    request_report(mount ? L"Mounting the volume" : L"Unmounting the volume", 2, argv);
    node_select(node_current());
}

static void properties(int s)
{
    WCHAR text[4096] = L"", line[512];
    int i;
    for (i = g_segs[s].line; i < g_rep.nlines && strcmp(g_rep.lines[i], "END"); i++)
    {
        utf8_to_w(g_rep.lines[i], line, 512);
        if (wcslen(text) + wcslen(line) + 3 < ARRAY_SIZE(text)) { wcscat(text, line); wcscat(text, L"\n"); }
    }
    volume_name(g_segs[s].line, line, 512);
    frame_message(MB_OK | MB_ICONINFORMATION, line, L"%ls", text);
}

/* ---- New Simple Volume, Delete, Extend, Shrink ----------------------------------------------- */

#define MB (1024ull * 1024ull)
enum { N_SIZE = 1830, N_MAX, N_FS, N_LABEL, N_LETTERON, N_LETTER, N_QUICK, R_AMOUNT = 1840, R_AFTER };

typedef struct newvol
{
    ULONGLONG max_mb, size_mb;
    WCHAR fs[16], label[64], letter[8];
} newvol_t;

static INT_PTR CALLBACK newvol_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    newvol_t *nv = (newvol_t *)GetWindowLongPtrW(dlg, DWLP_USER);
    static const WCHAR *const fss[] = { L"NTFS", L"exFAT", L"FAT32", L"ext4" };
    WCHAR t[64];
    int i;
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        DWORD used = GetLogicalDrives();
        int c;
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        nv = (newvol_t *)lp;
        _snwprintf(t, 64, L"%llu", nv->max_mb);
        SetDlgItemTextW(dlg, N_MAX, t);
        SetDlgItemTextW(dlg, N_SIZE, t);
        for (i = 0; i < 4; i++) SendDlgItemMessageW(dlg, N_FS, CB_ADDSTRING, 0, (LPARAM)fss[i]);
        SendDlgItemMessageW(dlg, N_FS, CB_SETCURSEL, 0, 0);
        SetDlgItemTextW(dlg, N_LABEL, L"New Volume");
        for (c = 'D'; c <= 'Y'; c++)
        {
            if (used & (1u << (c - 'A'))) continue;
            _snwprintf(t, 4, L"%lc:", c);
            SendDlgItemMessageW(dlg, N_LETTER, CB_ADDSTRING, 0, (LPARAM)t);
        }
        SendDlgItemMessageW(dlg, N_LETTER, CB_SETCURSEL, 0, 0);
        CheckDlgButton(dlg, N_LETTERON, BST_CHECKED);
        CheckDlgButton(dlg, N_QUICK, BST_CHECKED);
        SendDlgItemMessageW(dlg, N_SIZE, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, N_SIZE));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == N_LETTERON)
            EnableWindow(GetDlgItem(dlg, N_LETTER), IsDlgButtonChecked(dlg, N_LETTERON) == BST_CHECKED);
        if (LOWORD(wp) == IDOK)
        {
            GetDlgItemTextW(dlg, N_SIZE, t, 64);
            nv->size_mb = wcstoull(t, NULL, 10);
            if (nv->size_mb < 8 || nv->size_mb > nv->max_mb)
            {
                WCHAR m[160];
                _snwprintf(m, 160, L"The size must be between 8 MB and %llu MB.", nv->max_mb);
                frame_message(MB_OK | MB_ICONWARNING, L"New Simple Volume", L"%ls", m);
                return TRUE;
            }
            i = (int)SendDlgItemMessageW(dlg, N_FS, CB_GETCURSEL, 0, 0);
            lstrcpyW(nv->fs, fss[i >= 0 && i < 4 ? i : 0]);
            GetDlgItemTextW(dlg, N_LABEL, nv->label, ARRAY_SIZE(nv->label));
            nv->letter[0] = 0;
            if (IsDlgButtonChecked(dlg, N_LETTERON) == BST_CHECKED) GetDlgItemTextW(dlg, N_LETTER, nv->letter, 8);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

/* the name of the partition a request made, from its "CREATED <part> <fs>" line */
static BOOL reply_word(const sys_reply_t *r, const char *head, char *out, size_t cch)
{
    int i;
    size_t n = strlen(head);
    for (i = 0; i < r->nlines; i++)
        if (!strncmp(r->lines[i], head, n) && r->lines[i][n] == ' ')
        {
            const char *w = r->lines[i] + n + 1, *e = strchr(w, ' ');
            size_t len = e ? (size_t)(e - w) : strlen(w);
            if (len >= cch) len = cch - 1;
            memcpy(out, w, len);
            out[len] = 0;
            return TRUE;
        }
    return FALSE;
}

static BOOL request_reply(const WCHAR *what, int argc, const char *const *argv, sys_reply_t *r)
{
    sys_request_argv(r, argc, argv);
    if (!r->ok)
    {
        WCHAR m[512];
        utf8_to_w(r->message, m, 512);
        if (!strcmp(r->kind, "denied") && strstr(r->message, "administrator"))
            frame_message(MB_OK | MB_ICONERROR, L"Disk Management",
                          L"%ls could not be done.\n\nYou need to be an administrator to change disks and volumes. "
                          L"Run Disk Management as an administrator.\n\n(%ls)", what, m);
        else frame_message(MB_OK | MB_ICONERROR, L"Disk Management", L"%ls could not be done.\n\n%ls", what, m);
        return FALSE;
    }
    return TRUE;
}

static void new_volume(int s)
{
    static dlgt_t d;
    newvol_t nv;
    sys_reply_t r;
    char disk[64], start[32], size[32], fs[16], part[64], *label, *letter;
    const char *argv[9];
    int argc = 0;
    memset(&nv, 0, sizeof(nv));
    nv.max_mb = g_segs[s].size / MB;
    dlg_begin(&d, L"New Simple Volume", 0, 260, 158);
    D_LABEL(&d, L"Maximum disk space in MB:", 7, 10, 130);
    D_VALUE(&d, N_MAX, 150, 10, 100);
    D_LABEL(&d, L"Minimum disk space in MB:", 7, 24, 130);
    D_LABEL(&d, L"8", 150, 24, 100);
    dlg_item(&d, NULL, ATOM_STATIC, L"Simple volume &size in MB:", 0xFFFF, SS_LEFT, 7, 42, 130, 8);
    dlg_item(&d, NULL, ATOM_EDIT, L"", N_SIZE, WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 150, 40, 80, 13);
    dlg_item(&d, NULL, ATOM_STATIC, L"&File system:", 0xFFFF, SS_LEFT, 7, 62, 130, 8);
    dlg_item(&d, NULL, ATOM_COMBO, L"", N_FS, CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 150, 60, 100, 80);
    dlg_item(&d, NULL, ATOM_STATIC, L"&Volume label:", 0xFFFF, SS_LEFT, 7, 82, 130, 8);
    dlg_item(&d, NULL, ATOM_EDIT, L"", N_LABEL, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 150, 80, 100, 13);
    dlg_item(&d, NULL, ATOM_BUTTON, L"&Assign the following drive letter:", N_LETTERON,
             BS_AUTOCHECKBOX | WS_TABSTOP, 7, 102, 140, 10);
    dlg_item(&d, NULL, ATOM_COMBO, L"", N_LETTER, CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 150, 100, 50, 120);
    dlg_item(&d, NULL, ATOM_BUTTON, L"Perform a quick format", N_QUICK, BS_AUTOCHECKBOX | WS_DISABLED, 7, 118, 140, 10);
    dlg_item(&d, NULL, ATOM_BUTTON, L"OK", IDOK, BS_DEFPUSHBUTTON | WS_TABSTOP, 146, 138, 50, 14);
    D_BUTTON(&d, L"Cancel", IDCANCEL, 202, 138, 50);
    if (DialogBoxIndirectParamW(g_inst, d.t, g_main, newvol_proc, (LPARAM)&nv) != IDOK) return;
    snprintf(disk, sizeof(disk), "%s", g_rep.lines[g_disks[g_segs[s].disk].line] + 5);
    snprintf(start, sizeof(start), "%llu", g_segs[s].start);
    snprintf(size, sizeof(size), "%llu", nv.size_mb == g_segs[s].size / MB ? g_segs[s].size : nv.size_mb * MB);
    WideCharToMultiByte(CP_UTF8, 0, nv.fs, -1, fs, sizeof(fs), NULL, NULL);
    CharLowerA(fs);
    label = w_to_utf8(nv.label);
    argv[argc++] = "create";
    argv[argc++] = disk;
    argv[argc++] = start;
    argv[argc++] = size;
    argv[argc++] = "--fs";
    argv[argc++] = fs;
    if (label && *label) { argv[argc++] = "--label"; argv[argc++] = label; }
    frame_status(L"Formatting...");
    if (request_reply(L"Creating the volume", argc, argv, &r) && nv.letter[0] &&
        reply_word(&r, "CREATED", part, sizeof(part)))
    {
        /* as the wizard does: the new volume opens under a drive letter */
        const char *mv[2] = { "mount", part };
        sys_reply_t m;
        if (request_reply(L"Mounting the new volume", 2, mv, &m))
        {
            const char *lv[3] = { "letter", part, NULL };
            sys_reply_t l;
            letter = w_to_utf8(nv.letter);
            lv[2] = letter;
            request_reply(L"Assigning the drive letter", 3, lv, &l);
            sys_free(&l);
            free(letter);
        }
        sys_free(&m);
    }
    sys_free(&r);
    free(label);
    node_select(node_current());
}

static void delete_volume(int s)
{
    char part[64];
    const char *argv[2] = { "delete", part };
    WCHAR name[256];
    sys_reply_t r;
    volume_name(g_segs[s].line, name, 256);
    if (frame_message(MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2, L"Delete simple volume",
                      L"Deleting %ls will erase all data on it. Back up any data you want to keep before deleting "
                      L"it. Do you want to continue?", name) != IDYES)
        return;
    part_path(s, part, sizeof(part));
    request_reply(L"Deleting the volume", 2, argv, &r);
    sys_free(&r);
    node_select(node_current());
}

typedef struct rsz { BOOL shrink; ULONGLONG size, min, max, amount_mb; } rsz_t;

static INT_PTR CALLBACK resize_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    rsz_t *rz = (rsz_t *)GetWindowLongPtrW(dlg, DWLP_USER);
    WCHAR t[64];
    ULONGLONG avail;
    switch (msg)
    {
    case WM_INITDIALOG:
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        rz = (rsz_t *)lp;
        avail = rz->shrink ? (rz->size - rz->min) / MB : (rz->max - rz->size) / MB;
        _snwprintf(t, 64, L"%llu", avail);
        SetDlgItemTextW(dlg, R_AMOUNT, t);
        SendDlgItemMessageW(dlg, R_AMOUNT, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(dlg, R_AMOUNT));
        /* the total after */
        /* fall through */
    case WM_USER + 1:
        GetDlgItemTextW(dlg, R_AMOUNT, t, 64);
        avail = wcstoull(t, NULL, 10);
        _snwprintf(t, 64, L"%llu", rz->shrink ? (rz->size / MB > avail ? rz->size / MB - avail : 0) : rz->size / MB + avail);
        SetDlgItemTextW(dlg, R_AFTER, t);
        return msg == WM_INITDIALOG ? FALSE : TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == R_AMOUNT && HIWORD(wp) == EN_CHANGE) SendMessageW(dlg, WM_USER + 1, 0, 0);
        if (LOWORD(wp) == IDOK)
        {
            GetDlgItemTextW(dlg, R_AMOUNT, t, 64);
            rz->amount_mb = wcstoull(t, NULL, 10);
            avail = rz->shrink ? (rz->size - rz->min) / MB : (rz->max - rz->size) / MB;
            if (!rz->amount_mb || rz->amount_mb > avail)
            {
                WCHAR m[160];
                _snwprintf(m, 160, L"Enter an amount between 1 MB and %llu MB.", avail);
                frame_message(MB_OK | MB_ICONWARNING, rz->shrink ? L"Shrink" : L"Extend Volume", L"%ls", m);
                return TRUE;
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static void resize_volume(int s, BOOL shrink)
{
    static dlgt_t d;
    char part[64], size[32];
    const char *argv[3] = { "resize-info", part, NULL };
    WCHAR name[256], title[300], t[64];
    sys_reply_t r;
    rsz_t rz;
    ULONGLONG newsize;
    memset(&rz, 0, sizeof(rz));
    rz.shrink = shrink;
    part_path(s, part, sizeof(part));
    volume_name(g_segs[s].line, name, 256);
    if (shrink) frame_status(L"Querying the volume for available shrink space, please wait...");
    if (!request_reply(shrink ? L"Shrinking the volume" : L"Extending the volume", 2, argv, &r)) { sys_free(&r); return; }
    rz.size = strtoull(sys_field(&r, 0, "SIZE") ? sys_field(&r, 0, "SIZE") : "0", NULL, 10);
    rz.min = strtoull(sys_field(&r, 0, "MIN-SIZE") ? sys_field(&r, 0, "MIN-SIZE") : "0", NULL, 10);
    rz.max = strtoull(sys_field(&r, 0, "MAX-SIZE") ? sys_field(&r, 0, "MAX-SIZE") : "0", NULL, 10);
    sys_free(&r);
    if (shrink && rz.min + MB > rz.size)
    {
        frame_message(MB_OK | MB_ICONINFORMATION, L"Shrink", L"%ls cannot be shrunk: its files need all of it.", name);
        return;
    }
    if (!shrink && rz.max < rz.size + MB)
    {
        frame_message(MB_OK | MB_ICONINFORMATION, L"Extend Volume",
                      L"There is no unallocated space right after %ls to extend it into.", name);
        return;
    }
    _snwprintf(title, ARRAY_SIZE(title), shrink ? L"Shrink %ls" : L"Extend Volume %ls", name);
    dlg_begin(&d, title, 0, 250, 96);
    _snwprintf(t, 64, L"%llu", rz.size / MB);
    D_LABEL(&d, shrink ? L"Total size before shrink in MB:" : L"Total volume size in MB:", 7, 10, 150);
    D_LABEL(&d, t, 170, 10, 70);
    dlg_item(&d, NULL, ATOM_STATIC, shrink ? L"Enter the amount of space to &shrink in MB:"
                                           : L"Select the amount of &space in MB:", 0xFFFF, SS_LEFT, 7, 30, 160, 8);
    dlg_item(&d, NULL, ATOM_EDIT, L"", R_AMOUNT, WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_AUTOHSCROLL, 170, 28, 70, 13);
    D_LABEL(&d, shrink ? L"Total size after shrink in MB:" : L"Total volume size after in MB:", 7, 50, 150);
    D_VALUE(&d, R_AFTER, 170, 50, 70);
    dlg_item(&d, NULL, ATOM_BUTTON, shrink ? L"S&hrink" : L"OK", IDOK, BS_DEFPUSHBUTTON | WS_TABSTOP, 136, 74, 50, 14);
    D_BUTTON(&d, L"Cancel", IDCANCEL, 192, 74, 50);
    if (DialogBoxIndirectParamW(g_inst, d.t, g_main, resize_proc, (LPARAM)&rz) != IDOK) return;
    newsize = shrink ? rz.size - rz.amount_mb * MB : rz.size + rz.amount_mb * MB;
    if (!shrink && newsize > rz.max) newsize = rz.max;
    snprintf(size, sizeof(size), "%llu", newsize);
    argv[0] = "resize";
    argv[2] = size;
    request_reply(shrink ? L"Shrinking the volume" : L"Extending the volume", 3, argv, &r);
    sys_free(&r);
    node_select(node_current());
}

/* a disk's Properties: what it is, and its health as SMART reports it */
static void disk_properties(int dsk)
{
    WCHAR text[2048], v[256], size[32];
    const char *h;
    int b, i;
    static const struct { const char *key; const WCHAR *name; } rows[] = {
        { "TEMPERATURE", L"Temperature (\u00b0C)" }, { "POWER-ON-HOURS", L"Power-on hours" },
        { "REALLOCATED", L"Reallocated sectors" }, { "PENDING", L"Pending sectors" },
        { "UNCORRECTABLE", L"Uncorrectable sectors" }, { "WEAR", L"Wear (% used)" },
        { "MEDIA-ERRORS", L"Media errors" },
    };
    h = disk_health(dsk, &b);
    fmt_bytes(g_disks[dsk].size, size, 32);
    utf8_to_w(f(g_disks[dsk].line, "MODEL") ? f(g_disks[dsk].line, "MODEL") : "", v, 256);
    _snwprintf(text, ARRAY_SIZE(text), L"Disk %d\n%ls\n%ls, %ls\n\nDevice status: %ls\n", dsk, v, size,
               g_disks[dsk].removable ? L"Removable" : L"Basic",
               !strcmp(h, "failing") ? L"Stained Glass detected a hard disk problem: the disk reports (SMART) that it is "
                                       L"failing. Back up your files now to prevent information loss."
               : !strcmp(h, "ok") ? L"This device is working properly (SMART: healthy)."
               : !strcmp(h, "unsupported") ? L"This device is working properly (it does not report its health)."
               : L"This device is working properly (its health is not known).");
    for (i = 0; b >= 0 && i < (int)ARRAY_SIZE(rows); i++)
    {
        const char *x = sys_field(&g_smart, b, rows[i].key);
        size_t n = wcslen(text);
        if (!x) continue;
        utf8_to_w(x, v, 256);
        _snwprintf(text + n, ARRAY_SIZE(text) - n, L"%ls: %ls\n", rows[i].name, v);
    }
    text[ARRAY_SIZE(text) - 1] = 0;
    frame_message(MB_OK | (!strcmp(h, "failing") ? MB_ICONWARNING : MB_ICONINFORMATION), L"Disk Properties", L"%ls", text);
}

/* ---- the snap-in ------------------------------------------------------------------------------ */

static int seg_of_key(LPARAM key) { return key >= 1 && key <= g_nsegs ? (int)key - 1 : -1; }

static void disk_verbs(node_t *n, LPARAM key, BOOL have, verbs_t *out)
{
    int s = seg_of_key(key);
    BOOL mounted, sysvol, sysdisk;
    WCHAR letter[8];
    (void)n;
    if (have && key >= DISK_KEY && key < DISK_KEY + g_ndisks) return;   /* Properties (open) only */
    if (!have || s < 0)
    {
        if (!have) out->v[out->n++] = (verb_t){ V_RESCAN, L"Rescan Dis&ks", IC_REFRESH, TRUE, FALSE };
        return;
    }
    sysdisk = g_disks[g_segs[s].disk].system;
    if (g_segs[s].kind)
    {
#ifndef SG_MUTANT_NONEW
        out->v[out->n++] = (verb_t){ V_NEW, L"New &Simple Volume...", -1, !sysdisk && !g_disks[g_segs[s].disk].cdrom, FALSE };
#endif
        return;
    }
    mounted = f(g_segs[s].line, "MOUNT") != NULL;
    sysvol = has_flag(g_segs[s].line, "system") || has_flag(g_segs[s].line, "esp") || has_flag(g_segs[s].line, "swap");
    letter_of(g_segs[s].line, letter, 8);
    out->v[out->n++] = (verb_t){ V_OPEN, L"&Open", -1, letter[0] != 0, FALSE };
    out->v[out->n++] = (verb_t){ V_LETTER, L"Change Drive &Letter and Paths...", -1, mounted && !sysvol, TRUE };
    out->v[out->n++] = (verb_t){ V_FORMAT, L"&Format...", -1, !mounted && !sysvol && !g_disks[g_segs[s].disk].system, FALSE };
    if (mounted) out->v[out->n++] = (verb_t){ V_UNMOUNT, L"&Unmount", -1, !sysvol, FALSE };
    else out->v[out->n++] = (verb_t){ V_MOUNT, L"&Mount", -1, !sysvol, FALSE };
    out->v[out->n++] = (verb_t){ V_EXTEND, L"E&xtend Volume...", -1,
                                 !mounted && !sysvol && !sysdisk && resizable_fs(g_segs[s].line) && free_after(s) >= 1024 * 1024,
                                 TRUE };
    out->v[out->n++] = (verb_t){ V_SHRINK, L"Shrin&k Volume...", -1,
                                 !mounted && !sysvol && !sysdisk && resizable_fs(g_segs[s].line), FALSE };
    out->v[out->n++] = (verb_t){ V_DELETE, L"&Delete Volume...", -1, !mounted && !letter[0] && !sysvol && !sysdisk, FALSE };
}

static void disk_invoke(node_t *n, LPARAM key, BOOL have, int verb)
{
    int s = seg_of_key(key);
    WCHAR letter[8], path[8];
    (void)n; (void)have;
    switch (verb)
    {
    case V_RESCAN: node_select(node_current()); return;
    case V_OPEN:
        letter_of(g_segs[s].line, letter, 8);
        _snwprintf(path, 8, L"%ls\\", letter);
        ShellExecuteW(g_main, NULL, path, NULL, NULL, SW_SHOWNORMAL);
        return;
    case V_LETTER: change_letter(s); return;
    case V_FORMAT: format_volume(s); return;
    case V_MOUNT: mount_volume(s, TRUE); return;
    case V_UNMOUNT: mount_volume(s, FALSE); return;
    case V_NEW: new_volume(s); return;
    case V_DELETE: delete_volume(s); return;
    case V_EXTEND: resize_volume(s, FALSE); return;
    case V_SHRINK: resize_volume(s, TRUE); return;
    }
}

static void verbs_menu(int s)
{
    verbs_t vs = { 0 };
    HMENU m = CreatePopupMenu();
    POINT pt;
    int i, cmd;
    disk_verbs(node_current(), s + 1, TRUE, &vs);  /* a disk's box passes DISK_KEY + d - 1 */
    for (i = 0; i < vs.n; i++)
    {
        if (vs.v[i].separator_before) AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING | (vs.v[i].enabled ? 0 : MF_GRAYED), 100 + i, vs.v[i].name);
    }
    if (s >= DISK_KEY - 1 || !g_segs[s].kind)
    {
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
        AppendMenuW(m, MF_STRING, 99, L"P&roperties");
    }
    GetCursorPos(&pt);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, NULL);
    DestroyMenu(m);
    if (cmd == 99) disk_open(node_current(), s + 1);
    else if (cmd >= 100) disk_invoke(node_current(), s + 1, TRUE, vs.v[cmd - 100].id);
}

static void disk_open(node_t *n, LPARAM key)
{
    int s = seg_of_key(key);
    (void)n;
    if (key >= DISK_KEY && key < DISK_KEY + g_ndisks) disk_properties((int)key - DISK_KEY);
    else if (s >= 0 && !g_segs[s].kind) properties(s);
}

static void disk_selchange(node_t *n, LPARAM key, BOOL have)
{
    (void)n;
    if (have) { g_selseg = seg_of_key(key); InvalidateRect(g_graph, NULL, FALSE); }
}

static void ensure(void)
{
    WNDCLASSW wc = { 0 };
    if (g_graph) return;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = graph_proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"SgDiskGraph";
    RegisterClassW(&wc);
    g_graph = CreateWindowExW(WS_EX_CLIENTEDGE, L"SgDiskGraph", NULL, WS_CHILD | WS_VSCROLL * 0, 0, 0, 0, 0, g_main,
                              NULL, g_inst, NULL);
}

static void disk_layout(node_t *n, const RECT *rc)
{
    RECT list = *rc;
    int gh = (rc->bottom - rc->top) * 58 / 100;
    (void)n;
    ensure();
    list.bottom -= gh;
    pane_set_list_rect(&list);
    MoveWindow(g_graph, rc->left, list.bottom + S(3), rc->right - rc->left, gh - S(3), TRUE);
    layout_graph();
}

static void disk_show(node_t *n)
{
    (void)n;
    ensure();
    ShowWindow(g_graph, SW_SHOW);
    load();
    fill_list();
    layout_graph();
    InvalidateRect(g_graph, NULL, TRUE);
    if (!g_rep.ok)
    {
        WCHAR m[512];
        utf8_to_w(g_rep.message, m, 512);
        pane_empty_text(m[0] ? m : L"The disks could not be read.");
    }
    {
        int i;
        for (i = 0; i < g_ndisks; i++)
            if (!strcmp(disk_health(i, NULL), "failing"))
            {
                WCHAR b[200];
                _snwprintf(b, 200, L"Disk %d reports (SMART) that it is failing. Back up its files now.", i);
                frame_banner(b);
                break;
            }
    }
    if (!g_admin && sys_bridged())
        frame_banner(L"You are signed in as a standard user. You can see the disks; changing them needs an administrator.");
    frame_status(L"%d disks, %d volumes", g_ndisks, ListView_GetItemCount(pane_list()));
}

static void disk_hide(node_t *n)
{
    (void)n;
    if (g_graph) ShowWindow(g_graph, SW_HIDE);
}

static void disk_dump(node_t *n, FILE *fp)
{
    int i;
    (void)n;
    fprintf(fp, "LINUXADMIN %d\nSELSEG %d\nSELDISK %d\n", g_admin, g_selseg, g_seldisk);
    for (i = 0; i < g_ndisks; i++)
    {
        POINT pt;
        const char *model = f(g_disks[i].line, "MODEL");
        screen_center(g_graph, &g_disks[i].label, &pt);
        fprintf(fp, "DISK %d\t%s\t%llu\t%ld %ld\t%s\t%s\n", i, g_rep.lines[g_disks[i].line] + 5, g_disks[i].size, pt.x, pt.y,
                model ? model : "", disk_health(i, NULL));
    }
    for (i = 0; i < g_nsegs; i++)
    {
        POINT pt;
        WCHAR name[256] = L"", letter[8] = L"";
        screen_center(g_graph, &g_segs[i].rc, &pt);
        if (!g_segs[i].kind) { volume_name(g_segs[i].line, name, 256); letter_of(g_segs[i].line, letter, 8); }
        fprintf(fp, "SEG %d\t%d\t%s\t%s\t%llu\t%ld %ld\t%ld\t%ls\t%ls\n", i + 1, g_segs[i].disk,
                g_segs[i].kind ? "free" : "part", g_segs[i].kind ? "" : g_rep.lines[g_segs[i].line] + 5,
                g_segs[i].size, pt.x, pt.y, g_segs[i].rc.right - g_segs[i].rc.left, name, letter);
    }
}

static const snapin_t disk_ops = {
    NULL, disk_show, disk_verbs, disk_invoke, disk_open, disk_selchange, NULL, disk_layout, disk_hide, disk_dump
};

node_t *disks_create(node_t *parent)
{
    node_t *n = node_add(parent, L"Disk Management", IC_DISKMGMT, &disk_ops, NULL);
    lstrcpyW(n->desc, L"The disks and volumes of this computer");
    return n;
}
