/* sg-mmc -- Disk Cleanup (cleanmgr.exe, as sg-cleanmgr64.exe): Windows' dialog
 * -- "You can use Disk Cleanup to free up to N of disk space on (C:)", the
 * list of what can go with a tick and a size each, a description, the total,
 * "Clean up system files", OK -- and its "Are you sure you want to
 * permanently delete these files?".
 *
 * What goes: the Windows side's own (temporary files older than a week,
 * Temporary Internet Files, Downloaded Program Files), the user's Linux side
 * through sg-sysinfo (the Recycle Bin -- Wine's is the XDG trash --,
 * thumbnails, Wine's downloads), and, with "Clean up system files" (an
 * administrator, through sg-sysinfod), downloaded update packages, old logs,
 * the archived journal and error reports.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "mmc.h"
#include <shlobj.h>
#include <string.h>

typedef struct item
{
    WCHAR id[32];
    WCHAR name[128];
    WCHAR desc[512];
    ULONGLONG size;
    BOOL linux_side;            /* sg-sysinfo's category */
    BOOL checked;
    WCHAR dir[MAX_PATH];        /* a Windows-side folder */
    int min_age_days;           /* only files older than this */
} item_t;

static item_t g_items[32];
static int g_nitems;
static BOOL g_system;           /* "Clean up system files" */
static WCHAR g_drive[4] = L"C:";
static HWND g_dlg, g_lv;
static WCHAR g_dump[MAX_PATH], g_msg[512];

enum { CL_LIST = 1900, CL_HEAD, CL_TOTAL, CL_DESC, CL_SYSTEM, CL_VIEW };

/* ---- sizes -------------------------------------------------------------------------------- */

static BOOL old_enough(const WIN32_FIND_DATAW *fd, int days)
{
    FILETIME now;
    ULARGE_INTEGER a, b;
#ifdef SG_MUTANT_AGE
    days = 0;
#endif
    if (days <= 0) return TRUE;
    GetSystemTimeAsFileTime(&now);
    a.LowPart = now.dwLowDateTime; a.HighPart = now.dwHighDateTime;
    b.LowPart = fd->ftLastWriteTime.dwLowDateTime; b.HighPart = fd->ftLastWriteTime.dwHighDateTime;
    return a.QuadPart > b.QuadPart && (a.QuadPart - b.QuadPart) / 10000000ULL > (ULONGLONG)days * 86400;
}

/* the size of what would go (del: and delete it) */
static ULONGLONG walk(const WCHAR *dir, int days, BOOL del)
{
    WCHAR pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    ULONGLONG total = 0;
    _snwprintf(pat, MAX_PATH, L"%ls\\*", dir);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return 0;
    do
    {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        _snwprintf(p, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            total += walk(p, days, del);
            if (del) RemoveDirectoryW(p);     /* only if it is empty now */
            continue;
        }
        if (!old_enough(&fd, days)) continue;
        if (del && !DeleteFileW(p)) continue;
        total += ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

static item_t *add_item(const WCHAR *id, const WCHAR *name, const WCHAR *desc)
{
    item_t *it;
    if (g_nitems >= (int)ARRAY_SIZE(g_items)) return NULL;
    it = &g_items[g_nitems++];
    memset(it, 0, sizeof(*it));
    lstrcpynW(it->id, id, ARRAY_SIZE(it->id));
    lstrcpynW(it->name, name, ARRAY_SIZE(it->name));
    lstrcpynW(it->desc, desc, ARRAY_SIZE(it->desc));
    return it;
}

static void windows_item(const WCHAR *id, const WCHAR *name, const WCHAR *desc, const WCHAR *dir, int days, BOOL checked)
{
    item_t *it = add_item(id, name, desc);
    if (!it) return;
    lstrcpynW(it->dir, dir, MAX_PATH);
    it->min_age_days = days;
    it->size = walk(dir, days, FALSE);
    it->checked = checked;
}

static void load(void)
{
    WCHAR path[MAX_PATH];
    sys_reply_t r;
    int b;
    g_nitems = 0;
    windows_item(L"downloaded-program-files", L"Downloaded Program Files",
                 L"Downloaded Program Files are ActiveX controls and Java applets downloaded automatically from the "
                 L"Internet when you view certain pages. They are temporarily stored in the Downloaded Program Files "
                 L"folder on your hard disk.",
                 (GetWindowsDirectoryW(path, MAX_PATH), wcscat(path, L"\\Downloaded Program Files"), path), 0, TRUE);
    if (SHGetFolderPathW(NULL, CSIDL_INTERNET_CACHE, NULL, 0, path) == S_OK)
        windows_item(L"internet-cache", L"Temporary Internet Files",
                     L"The Temporary Internet Files folder contains webpages stored on your hard disk for quick "
                     L"viewing. Your personalized settings for webpages will be left intact.", path, 0, TRUE);
    if (GetTempPathW(MAX_PATH, path))
    {
        size_t n = wcslen(path);
        if (n && path[n - 1] == '\\') path[n - 1] = 0;
        windows_item(L"temporary-files", L"Temporary files",
                     L"Programs sometimes store temporary information in the TEMP folder. Before a program closes, "
                     L"it usually deletes this information. You can safely delete temporary files that have not "
                     L"been modified in over a week.", path, 7, FALSE);
    }
    sys_request(&r, g_system ? "cleanup-system" : "cleanup", NULL);
    for (b = sys_next_block(&r, 0, "CATEGORY"); b >= 0; b = sys_next_block(&r, b + 1, "CATEGORY"))
    {
        WCHAR id[32], name[128], desc[512];
        const char *sz = sys_field(&r, b, "SIZE");
        item_t *it;
        utf8_to_w(r.lines[b] + 9, id, 32);
        utf8_to_w(sys_field(&r, b, "NAME"), name, 128);
        utf8_to_w(sys_field(&r, b, "DESCRIPTION"), desc, 512);
        if (!(it = add_item(id, name, desc))) break;
        it->linux_side = TRUE;
        it->size = sz ? strtoull(sz, NULL, 10) : 0;
        it->checked = !wcscmp(id, L"thumbnails") || !wcscmp(id, L"update-cache");
    }
    if (!r.ok && g_system)
    {
        WCHAR m[512];
        utf8_to_w(r.message, m, 512);
        _snwprintf(g_msg, ARRAY_SIZE(g_msg), L"%ls", !strcmp(r.kind, "denied") ?
                   L"Only an administrator can clean up system files." : m);
    }
    sys_free(&r);
}

/* ---- the dialog ---------------------------------------------------------------------------- */

static void dump(void)
{
    FILE *f;
    int i;
    if (!g_dump[0] || !(f = _wfopen(g_dump, L"wb"))) return;
    fprintf(f, "TITLE Disk Cleanup\nDRIVE %ls\nSYSTEM %d\nBRIDGED %d\n", g_drive, g_system, sys_bridged());
    for (i = 0; i < g_nitems; i++)
        fprintf(f, "ITEM %d\t%ls\t%ls\t%llu\t%d\n", i, g_items[i].id, g_items[i].name, g_items[i].size,
                g_lv ? (ListView_GetCheckState(g_lv, i) ? 1 : 0) : g_items[i].checked);
    fprintf(f, "MSG %ls\nEND\n", g_msg);
    fclose(f);
}

static void update_total(void)
{
    ULONGLONG total = 0, all = 0;
    WCHAR a[64], t[256];
    int i;
    for (i = 0; i < g_nitems; i++)
    {
        all += g_items[i].size;
        if (ListView_GetCheckState(g_lv, i)) total += g_items[i].size;
    }
    fmt_bytes(total, a, 64);
    SetDlgItemTextW(g_dlg, CL_TOTAL, a);
    fmt_bytes(all, a, 64);
    _snwprintf(t, 256, L"You can use Disk Cleanup to free up to %ls of disk space on (%ls).", a, g_drive);
    SetDlgItemTextW(g_dlg, CL_HEAD, t);
    dump();
}

static void fill(void)
{
    int i;
    LVITEMW it = { 0 };
    ListView_DeleteAllItems(g_lv);
    for (i = 0; i < g_nitems; i++)
    {
        WCHAR sz[64];
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.iSubItem = 0;
        it.pszText = g_items[i].name;
        SendMessageW(g_lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
        fmt_bytes(g_items[i].size, sz, 64);
        it.iSubItem = 1;
        it.pszText = sz;
        SendMessageW(g_lv, LVM_SETITEMTEXTW, i, (LPARAM)&it);
        ListView_SetCheckState(g_lv, i, g_items[i].checked);
    }
    if (g_nitems) ListView_SetItemState(g_lv, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    update_total();
}

static void clean(void)
{
    int i;
    char *ids[32];
    int nids = 0;
    for (i = 0; i < g_nitems; i++)
    {
        if (!ListView_GetCheckState(g_lv, i)) continue;
        if (g_items[i].linux_side) ids[nids++] = w_to_utf8(g_items[i].id);
        else walk(g_items[i].dir, g_items[i].min_age_days, TRUE);
    }
    if (nids)
    {
        const char *argv[34];
        sys_reply_t r;
        argv[0] = g_system ? "clean-system" : "clean";
        for (i = 0; i < nids; i++) argv[i + 1] = ids[i];
        sys_request_argv(&r, nids + 1, argv);
        if (!r.ok)
        {
            WCHAR m[512];
            utf8_to_w(r.message, m, 512);
            _snwprintf(g_msg, ARRAY_SIZE(g_msg), L"Some files could not be deleted: %ls", m);
            dump();
            MessageBoxW(g_dlg, g_msg, L"Disk Cleanup", MB_OK | MB_ICONWARNING);
        }
        sys_free(&r);
        for (i = 0; i < nids; i++) free(ids[i]);
    }
}

static INT_PTR CALLBACK dlg_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        LVCOLUMNW c = { LVCF_WIDTH };
        RECT r;
        WCHAR title[64];
        g_dlg = dlg;
        g_lv = GetDlgItem(dlg, CL_LIST);
        ListView_SetExtendedListViewStyle(g_lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        GetClientRect(g_lv, &r);
        c.cx = r.right - S(100) - GetSystemMetrics(SM_CXVSCROLL);
        SendMessageW(g_lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&c);
        c.cx = S(90);
        SendMessageW(g_lv, LVM_INSERTCOLUMNW, 1, (LPARAM)&c);
        _snwprintf(title, 64, L"Disk Cleanup for (%ls)", g_drive);
        SetWindowTextW(dlg, title);
        SendMessageW(dlg, WM_SETICON, ICON_BIG, (LPARAM)LoadIconW(g_inst, MAKEINTRESOURCEW(1)));
        if (g_system) EnableWindow(GetDlgItem(dlg, CL_SYSTEM), FALSE);
        fill();
        if (g_msg[0]) MessageBoxW(dlg, g_msg, L"Disk Cleanup", MB_OK | MB_ICONWARNING);
        return TRUE;
    }
    case WM_NOTIFY:
        if (((NMHDR *)lp)->idFrom == CL_LIST && ((NMHDR *)lp)->code == LVN_ITEMCHANGED)
        {
            NMLISTVIEW *nm = (NMLISTVIEW *)lp;
            if (nm->uNewState & LVIS_SELECTED && nm->iItem >= 0 && nm->iItem < g_nitems)
                SetDlgItemTextW(dlg, CL_DESC, g_items[nm->iItem].desc);
            update_total();
        }
        break;
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDOK:
            lstrcpyW(g_msg, L"Are you sure you want to permanently delete these files?");
            dump();
            if (MessageBoxW(dlg, g_msg, L"Disk Cleanup", MB_YESNO | MB_ICONQUESTION) != IDYES) { g_msg[0] = 0; dump(); return TRUE; }
            g_msg[0] = 0;
            clean();
            load();
            fill();
            lstrcpyW(g_msg, L"Done");
            dump();
            EndDialog(dlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        case CL_SYSTEM:
            /* as Windows: the same dialog again, elevated, with the system's files */
            {
                WCHAR self[MAX_PATH], args[64];
                SHELLEXECUTEINFOW sei = { sizeof(sei) };
                GetModuleFileNameW(NULL, self, MAX_PATH);
                _snwprintf(args, 64, L"/d %lc /system", g_drive[0]);
                sei.lpVerb = L"runas";
                sei.lpFile = self;
                sei.lpParameters = args;
                sei.nShow = SW_SHOWNORMAL;
                if (ShellExecuteExW(&sei)) EndDialog(dlg, IDCANCEL);
            }
            return TRUE;
        }
        break;
    }
    return FALSE;
}

int cleanmgr_main(const WCHAR *cmdline)
{
    static dlgt_t d;
    int argc, i;
    WCHAR **argv = CommandLineToArgvW(cmdline, &argc);
    for (i = 1; argv && i < argc; i++)
    {
        if ((!_wcsicmp(argv[i], L"/d") || !_wcsicmp(argv[i], L"-d")) && i + 1 < argc)
        {
            g_drive[0] = towupper(argv[++i][0]);
            g_drive[1] = ':';
        }
        else if (!_wcsicmp(argv[i], L"/system")) g_system = TRUE;
    }
    GetEnvironmentVariableW(L"SG_MMC_DUMP", g_dump, MAX_PATH);
    load();
    dlg_begin(&d, L"Disk Cleanup", 0, 250, 250);
    dlg_item(&d, NULL, ATOM_STATIC, L"", CL_HEAD, SS_LEFT | SS_NOPREFIX, 7, 8, 236, 18);
    D_LABEL(&d, L"Files to delete:", 7, 32, 200);
    dlg_item(&d, WC_LISTVIEWW, 0, L"", CL_LIST, LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS |
             WS_BORDER | WS_TABSTOP, 7, 43, 236, 70);
    D_LABEL(&d, L"Total amount of disk space you gain:", 7, 118, 150);
    dlg_item(&d, NULL, ATOM_STATIC, L"", CL_TOTAL, SS_RIGHT | SS_NOPREFIX, 160, 118, 83, 8);
    D_GROUP(&d, L"Description", 7, 132, 236, 72);
    dlg_item(&d, NULL, ATOM_STATIC, L"", CL_DESC, SS_LEFT | SS_NOPREFIX, 14, 145, 222, 54);
    dlg_item(&d, NULL, ATOM_BUTTON, L"Clean up &system files", CL_SYSTEM, BS_PUSHBUTTON | WS_TABSTOP, 7, 210, 100, 14);
    dlg_item(&d, NULL, ATOM_BUTTON, L"OK", IDOK, BS_DEFPUSHBUTTON | WS_TABSTOP, 139, 230, 50, 14);
    D_BUTTON(&d, L"Cancel", IDCANCEL, 193, 230, 50);
    DialogBoxIndirectParamW(g_inst, d.t, NULL, dlg_proc, 0);
    if (argv) LocalFree(argv);
    return 0;
}
