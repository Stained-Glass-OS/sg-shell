/* sg-control -- Environment Variables: the person's own variables and the
 * machine's, as Windows' dialog of that name has them (System Properties >
 * Advanced). New, Edit and Delete; a Path-like value is edited as a list of
 * its entries. OK writes them and tells the session (WM_SETTINGCHANGE
 * "Environment"): the shell reads them again, and programs started after get
 * them (wine-sg 0749).
 *
 * The person's variables are HKCU\Environment; the machine's,
 * HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment, which
 * only an administrator may change: "Edit system variables" opens this
 * dialog again, elevated (sg-control /admin envvars-system), for those alone.
 *
 * David 2026-10-01: Claude Code asked for PATH to be changed, and nothing in
 * Settings or Control Panel was found by "Environment".
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"
#include <shlobj.h>

#define ENV_USER_KEY L"Environment"
#define ENV_SYS_KEY  L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"
#define MAX_VARS 256
#define VAL_CCH  32767

struct envvar { WCHAR name[256]; WCHAR *value; DWORD type; };
struct envlist { struct envvar v[MAX_VARS]; int n; BOOL changed; HKEY root; const WCHAR *key; HWND lv; };

enum { ID_U_LIST = 100, ID_U_NEW, ID_U_EDIT, ID_U_DEL, ID_S_LIST, ID_S_NEW, ID_S_EDIT, ID_S_DEL, ID_S_ELEVATE };

static struct envlist g_user, g_sys;
static BOOL g_system_only, g_env_done, g_env_ok;

static void list_free(struct envlist *l)
{
    int i;
    for (i = 0; i < l->n; i++) free(l->v[i].value);
    l->n = 0;
}

static int cmp_var(const void *a, const void *b)
{
    return lstrcmpiW(((const struct envvar *)a)->name, ((const struct envvar *)b)->name);
}

static void list_load(struct envlist *l)
{
    HKEY k;
    DWORD i;
    list_free(l);
    l->changed = FALSE;
    if (RegOpenKeyExW(l->root, l->key, 0, KEY_READ, &k)) return;
    for (i = 0; l->n < MAX_VARS; i++) {
        struct envvar *e = &l->v[l->n];
        DWORD ncch = ARRAYSIZE(e->name), cb = 0, type;
        if (RegEnumValueW(k, i, e->name, &ncch, NULL, &type, NULL, &cb)) break;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        if (!(e->value = calloc(1, cb + sizeof(WCHAR)))) continue;
        RegQueryValueExW(k, e->name, NULL, NULL, (BYTE *)e->value, &cb);
        e->type = type;
        l->n++;
    }
    RegCloseKey(k);
    qsort(l->v, l->n, sizeof(l->v[0]), cmp_var);
}

/* what OK writes: every variable, and the ones removed deleted */
static BOOL list_save(struct envlist *l)
{
    HKEY k;
    DWORD i, n;
    WCHAR name[256];
    int j;
    if (!l->changed) return TRUE;
    if (RegCreateKeyExW(l->root, l->key, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL, &k, NULL)) return FALSE;
    for (i = 0; ; ) {
        BOOL keep = FALSE;
        DWORD type;
        n = ARRAYSIZE(name);
        if (RegEnumValueW(k, i, name, &n, NULL, &type, NULL, NULL)) break;
        if (type != REG_SZ && type != REG_EXPAND_SZ) { i++; continue; }
        for (j = 0; j < l->n; j++) if (!lstrcmpiW(l->v[j].name, name)) keep = TRUE;
        if (!keep) RegDeleteValueW(k, name);    /* the next one moves to index i */
        else i++;
    }
    for (j = 0; j < l->n; j++) {
        struct envvar *e = &l->v[j];
        /* a value naming another variable (%USERPROFILE%) is expanded when used */
        DWORD type = wcschr(e->value, L'%') ? REG_EXPAND_SZ : (e->type ? e->type : REG_SZ);
        RegSetValueExW(k, e->name, 0, type, (const BYTE *)e->value, (lstrlenW(e->value) + 1) * sizeof(WCHAR));
    }
    RegCloseKey(k);
    l->changed = FALSE;
    return TRUE;
}

static void list_fill(struct envlist *l)
{
    int i, sel = (int)SendMessageW(l->lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    LVITEMW it;
    SendMessageW(l->lv, LVM_DELETEALLITEMS, 0, 0);
    for (i = 0; i < l->n; i++) {
        memset(&it, 0, sizeof(it));
        it.mask = LVIF_TEXT; it.iItem = i; it.pszText = l->v[i].name;
        SendMessageW(l->lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
        it.iSubItem = 1; it.pszText = l->v[i].value;
        SendMessageW(l->lv, LVM_SETITEMTEXTW, i, (LPARAM)&it);
    }
    if (sel >= l->n) sel = l->n - 1;
    if (sel >= 0) {
        memset(&it, 0, sizeof(it));
        it.stateMask = it.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(l->lv, LVM_SETITEMSTATE, sel, (LPARAM)&it);
    }
}

/* the row of that name selected (the one just added or edited), shown */
static void list_select(struct envlist *l, const WCHAR *name)
{
    LVITEMW it;
    int i;
    for (i = 0; i < l->n; i++) {
        if (lstrcmpiW(l->v[i].name, name)) continue;
        memset(&it, 0, sizeof(it));
        it.stateMask = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(l->lv, LVM_SETITEMSTATE, (WPARAM)-1, (LPARAM)&it);
        it.state = LVIS_SELECTED | LVIS_FOCUSED;
        SendMessageW(l->lv, LVM_SETITEMSTATE, i, (LPARAM)&it);
        SendMessageW(l->lv, LVM_ENSUREVISIBLE, i, FALSE);
        return;
    }
}

/* Path, PATHEXT, PSModulePath -- and any value that is a list of folders --
 * is edited one entry at a time */
static BOOL is_list(const WCHAR *name, const WCHAR *value)
{
    static const WCHAR *const names[] = { L"Path", L"PATHEXT", L"PSModulePath", L"CLASSPATH", L"PYTHONPATH" };
    size_t i;
    for (i = 0; i < ARRAYSIZE(names); i++) if (!lstrcmpiW(name, names[i])) return TRUE;
    return value && wcschr(value, L';') && (wcschr(value, L'\\') || wcschr(value, L'/'));
}

/* ---- the list editor: one entry a line ---------------------------------------------------- */

enum { ID_L_LIST = 200, ID_L_NEW, ID_L_EDIT, ID_L_BROWSE, ID_L_DEL, ID_L_UP, ID_L_DOWN, ID_L_TEXT };
static BOOL g_l_done, g_l_ok;

static void l_join(HWND lb, WCHAR *out, int cch)
{
    int i, n = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    WCHAR item[1024];
    out[0] = 0;
    for (i = 0; i < n; i++) {
        SendMessageW(lb, LB_GETTEXT, i, (LPARAM)item);
        if (!item[0]) continue;
        if (out[0] && lstrlenW(out) + 1 < cch) lstrcatW(out, L";");
        if (lstrlenW(out) + lstrlenW(item) < cch) lstrcatW(out, item);
    }
}

static void l_split(HWND lb, const WCHAR *value)
{
    WCHAR item[1024];
    const WCHAR *p = value, *e;
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    while (p && *p) {
        int len;
        e = wcschr(p, L';');
        len = e ? (int)(e - p) : lstrlenW(p);
        if (len >= (int)ARRAYSIZE(item)) len = ARRAYSIZE(item) - 1;
        memcpy(item, p, len * sizeof(WCHAR)); item[len] = 0;
        if (item[0]) SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)item);
        p = e ? e + 1 : NULL;
    }
}

static BOOL browse_folder(HWND owner, WCHAR *out)
{
    BROWSEINFOW bi = { 0 };
    PIDLIST_ABSOLUTE pidl;
    bi.hwndOwner = owner;
    bi.lpszTitle = L"Choose a folder";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (!(pidl = SHBrowseForFolderW(&bi))) return FALSE;
    SHGetPathFromIDListW(pidl, out);
    CoTaskMemFree(pidl);
    return out[0] != 0;
}

static void l_select(HWND lb, int i)
{
    SendMessageW(lb, LB_SETCURSEL, i, 0);
}

static LRESULT CALLBACK l_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    HWND lb = GetDlgItem(hwnd, ID_L_LIST);
    int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0), n = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    WCHAR item[1024];
    switch (msg) {
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_L_NEW: case ID_L_EDIT: {
            struct form_field f[] = { { L"Entry:", item, ARRAYSIZE(item), FF_TEXT } };
            BOOL edit = LOWORD(wp) == ID_L_EDIT;
            if (edit && sel < 0) return 0;
            item[0] = 0;
            if (edit) SendMessageW(lb, LB_GETTEXT, sel, (LPARAM)item);
            if (run_form(hwnd, edit ? L"Edit entry" : L"New entry", NULL, f, 1, L"OK", FALSE) && item[0]) {
                if (edit) SendMessageW(lb, LB_DELETESTRING, sel, 0);
                else sel = sel < 0 ? n : sel + 1;
                SendMessageW(lb, LB_INSERTSTRING, sel, (LPARAM)item);
                l_select(lb, sel);
            }
            return 0;
        }
        case ID_L_BROWSE:
            item[0] = 0;
            if (browse_folder(hwnd, item)) {
                sel = sel < 0 ? n : sel + 1;
                SendMessageW(lb, LB_INSERTSTRING, sel, (LPARAM)item);
                l_select(lb, sel);
            }
            return 0;
        case ID_L_DEL:
            if (sel >= 0) { SendMessageW(lb, LB_DELETESTRING, sel, 0); l_select(lb, sel < n - 1 ? sel : sel - 1); }
            return 0;
        case ID_L_UP: case ID_L_DOWN: {
            int to = LOWORD(wp) == ID_L_UP ? sel - 1 : sel + 1;
            if (sel < 0 || to < 0 || to >= n) return 0;
            SendMessageW(lb, LB_GETTEXT, sel, (LPARAM)item);
            SendMessageW(lb, LB_DELETESTRING, sel, 0);
            SendMessageW(lb, LB_INSERTSTRING, to, (LPARAM)item);
            l_select(lb, to);
            return 0;
        }
        case ID_L_TEXT: {
            static WCHAR text[VAL_CCH];
            struct form_field f[] = { { L"Variable value:", text, ARRAYSIZE(text), FF_TEXT } };
            l_join(lb, text, ARRAYSIZE(text));
            if (run_form(hwnd, L"Edit text", NULL, f, 1, L"OK", FALSE)) l_split(lb, text);
            return 0;
        }
        case ID_L_LIST:
            if (HIWORD(wp) == LBN_DBLCLK) PostMessageW(hwnd, WM_COMMAND, ID_L_EDIT, 0);
            return 0;
        case IDOK: g_l_ok = TRUE; g_l_done = TRUE; return 0;
        case IDCANCEL: g_l_done = TRUE; return 0;
        }
        break;
    case WM_CLOSE: g_l_done = TRUE; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* the entries of a list-like value; the value is replaced on OK */
static BOOL edit_list(HWND owner, const WCHAR *name, WCHAR *value, int cch)
{
    static BOOL registered;
    WCHAR title[300];
    int w = S(560), h = S(470), bx = w - S(24) - S(130), i;
    static const struct { const WCHAR *label; int id; } buttons[] = {
        { L"&New", ID_L_NEW }, { L"&Edit", ID_L_EDIT }, { L"&Browse...", ID_L_BROWSE }, { L"&Delete", ID_L_DEL },
        { L"Move &Up", ID_L_UP }, { L"Move D&own", ID_L_DOWN }, { L"Edit &text...", ID_L_TEXT },
    };
    HWND hwnd, lb, c;
    MSG msg;
    if (!registered) {
        WNDCLASSW wc = { 0 };
        wc.lpfnWndProc = l_proc; wc.hInstance = g_inst; wc.lpszClassName = L"SgEnvList";
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
        RegisterClassW(&wc);
        registered = TRUE;
    }
    _snwprintf(title, ARRAYSIZE(title), L"Edit environment variable %ls", name);
    title[ARRAYSIZE(title) - 1] = 0;
    hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"SgEnvList", title, WS_POPUP | WS_CAPTION | WS_SYSMENU,
                           CW_USEDEFAULT, CW_USEDEFAULT, w, h, owner, NULL, g_inst, NULL);
    if (!hwnd) return FALSE;
    lb = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                         LBS_NOINTEGRALHEIGHT, S(16), S(16), bx - S(32), h - S(110), hwnd, (HMENU)ID_L_LIST, g_inst, NULL);
    SendMessageW(lb, WM_SETFONT, (WPARAM)g_font_body, TRUE);
    for (i = 0; i < (int)ARRAYSIZE(buttons); i++) {
        c = CreateWindowW(L"BUTTON", buttons[i].label, WS_CHILD | WS_VISIBLE | WS_TABSTOP, bx, S(16) + i * S(36), S(130), S(28),
                          hwnd, (HMENU)(INT_PTR)buttons[i].id, g_inst, NULL);
        SendMessageW(c, WM_SETFONT, (WPARAM)g_font_body, TRUE);
    }
    c = CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, w - S(250), h - S(84), S(100), S(28),
                      hwnd, (HMENU)IDOK, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font_body, TRUE);
    c = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, w - S(140), h - S(84), S(100), S(28),
                      hwnd, (HMENU)IDCANCEL, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font_body, TRUE);
    l_split(lb, value);
    l_select(lb, 0);
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(lb);
    g_l_done = g_l_ok = FALSE;
    while (!g_l_done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_l_ok) l_join(lb, value, cch);
    if (owner) EnableWindow(owner, TRUE);
    DestroyWindow(hwnd);
    if (owner) SetForegroundWindow(owner);
    return g_l_ok;
}

/* ---- the dialog --------------------------------------------------------------------------- */

static BOOL valid_name(const WCHAR *name)
{
    return name[0] && !wcschr(name, L'=') && !wcschr(name, L'\\') && lstrlenW(name) < 255;
}

/* New or Edit on one of the lists (index -1: new) */
static void edit_var(HWND owner, struct envlist *l, int index)
{
    static WCHAR value[VAL_CCH];
    WCHAR name[256] = L"";
    BOOL ok;
    if (l->n >= MAX_VARS && index < 0) return;
    value[0] = 0;
    if (index >= 0) { lstrcpynW(name, l->v[index].name, ARRAYSIZE(name)); lstrcpynW(value, l->v[index].value, ARRAYSIZE(value)); }
    if (index >= 0 && is_list(name, value)) ok = edit_list(owner, name, value, ARRAYSIZE(value));
    else {
        struct form_field f[] = {
            { L"Variable name:", name, ARRAYSIZE(name), FF_TEXT },
            { L"Variable value:", value, ARRAYSIZE(value), FF_TEXT },
        };
        for (;;) {
            ok = run_form(owner, index < 0 ? (l == &g_user ? L"New User Variable" : L"New System Variable")
                                             : (l == &g_user ? L"Edit User Variable" : L"Edit System Variable"),
                          NULL, f, 2, L"OK", FALSE);
            if (!ok || valid_name(name)) break;
            message(owner, L"Environment Variables", L"A variable's name cannot be empty or contain = or \\.", TRUE);
        }
    }
    if (!ok) return;
    if (index < 0) {
        int i;
        for (i = 0; i < l->n; i++) if (!lstrcmpiW(l->v[i].name, name)) index = i;   /* that name already: replaced */
        if (index < 0) { index = l->n++; l->v[index].value = NULL; l->v[index].type = 0; }
    }
    lstrcpynW(l->v[index].name, name, ARRAYSIZE(l->v[index].name));
    free(l->v[index].value);
    l->v[index].value = _wcsdup(value);
    if (!l->v[index].value) l->v[index].value = _wcsdup(L"");
    qsort(l->v, l->n, sizeof(l->v[0]), cmp_var);
    l->changed = TRUE;
    list_fill(l);
    list_select(l, name);
    SetFocus(l->lv);
}

static void delete_var(struct envlist *l)
{
    int sel = (int)SendMessageW(l->lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
    if (sel < 0 || sel >= l->n) return;
    free(l->v[sel].value);
    memmove(&l->v[sel], &l->v[sel + 1], (l->n - sel - 1) * sizeof(l->v[0]));
    l->n--;
    l->changed = TRUE;
    list_fill(l);
}

static LRESULT CALLBACK env_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND: {
        struct envlist *l = LOWORD(wp) >= ID_S_LIST && LOWORD(wp) <= ID_S_DEL ? &g_sys : &g_user;
        switch (LOWORD(wp)) {
        case ID_U_NEW: case ID_S_NEW: edit_var(hwnd, l, -1); return 0;
        case ID_U_EDIT: case ID_S_EDIT: {
            /* nothing selected: nothing to edit (not a new one) */
            int sel = (int)SendMessageW(l->lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
            if (sel >= 0 && sel < l->n) edit_var(hwnd, l, sel);
            return 0;
        }
        case ID_U_DEL: case ID_S_DEL: delete_var(l); return 0;
        case ID_S_ELEVATE:
            /* an administrator's own dialog for the machine's variables;
             * this one shows them again when it is back in front */
            run_elevated(L"/admin envvars-system");
            return 0;
        case IDOK: g_env_ok = TRUE; g_env_done = TRUE; return 0;
        case IDCANCEL: g_env_done = TRUE; return 0;
        }
        break;
    }
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->code == NM_DBLCLK && (nm->idFrom == ID_U_LIST || (nm->idFrom == ID_S_LIST && g_system_only)))
            PostMessageW(hwnd, WM_COMMAND, nm->idFrom == ID_U_LIST ? ID_U_EDIT : ID_S_EDIT, 0);
        break;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && !g_system_only && g_sys.lv && !g_sys.changed) { list_load(&g_sys); list_fill(&g_sys); }
        break;
    case WM_ELEVATED_DONE:
        if (!g_system_only) { list_load(&g_sys); list_fill(&g_sys); }
        return 0;
    case WM_CLOSE: g_env_done = TRUE; return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND make_list(HWND parent, int x, int y, int w, int h, int id, struct envlist *l)
{
    LVCOLUMNW col = { 0 };
    HWND lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL, WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                              LVS_SINGLESEL | LVS_SHOWSELALWAYS, x, y, w, h, parent, (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(lv, WM_SETFONT, (WPARAM)g_font_body, TRUE);
    SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, LVS_EX_FULLROWSELECT, LVS_EX_FULLROWSELECT);
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = (WCHAR *)L"Variable"; col.cx = S(170);
    SendMessageW(lv, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
    col.pszText = (WCHAR *)L"Value"; col.cx = w - S(170) - S(24);
    SendMessageW(lv, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
    l->lv = lv;
    return lv;
}

static void add_label(HWND parent, const WCHAR *text, int x, int y, int w)
{
    HWND c = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_NOPREFIX, x, y, w, S(20), parent, NULL, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font_body, TRUE);
}

static void add_button(HWND parent, const WCHAR *text, int x, int y, int w, int id, DWORD style)
{
    HWND c = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, x, y, w, S(28), parent,
                           (HMENU)(INT_PTR)id, g_inst, NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font_body, TRUE);
}

/* the dialog; system_only: the elevated one, for the machine's variables alone */
int envvars_main(BOOL system_only)
{
    WNDCLASSW wc = { 0 };
    WCHAR user[128] = L"", label[200];
    DWORD n = ARRAYSIZE(user);
    int w = S(660), x = S(16), lw = w - S(48), y = S(14), lh = S(170), bw = S(110);
    HWND hwnd;
    MSG msg;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };

    InitCommonControlsEx(&icc);
    CoInitialize(NULL);     /* the folder chooser */
    g_system_only = system_only;
    g_user.root = HKEY_CURRENT_USER; g_user.key = ENV_USER_KEY; g_user.lv = NULL;
    g_sys.root = HKEY_LOCAL_MACHINE; g_sys.key = ENV_SYS_KEY; g_sys.lv = NULL;
    wc.lpfnWndProc = env_proc; wc.hInstance = g_inst; wc.lpszClassName = L"SgEnvVars";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    RegisterClassW(&wc);
    hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"SgEnvVars",
                           system_only ? L"System Variables" : L"Environment Variables",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, w,
                           system_only ? S(330) : S(560), NULL, NULL, g_inst, NULL);
    if (!hwnd) return 1;
    if (!system_only) {
        GetUserNameW(user, &n);
        _snwprintf(label, ARRAYSIZE(label), L"User variables for %ls", user);
        label[ARRAYSIZE(label) - 1] = 0;
        add_label(hwnd, label, x, y, lw);
        y += S(24);
        make_list(hwnd, x, y, lw, lh, ID_U_LIST, &g_user);
        y += lh + S(8);
        add_button(hwnd, L"&New...", w - S(32) - 3 * bw - S(16), y, bw, ID_U_NEW, 0);
        add_button(hwnd, L"&Edit...", w - S(32) - 2 * bw - S(8), y, bw, ID_U_EDIT, 0);
        add_button(hwnd, L"&Delete", w - S(32) - bw, y, bw, ID_U_DEL, 0);
        y += S(46);
    }
    add_label(hwnd, L"System variables", x, y, lw);
    y += S(24);
    make_list(hwnd, x, y, lw, lh, ID_S_LIST, &g_sys);
    y += lh + S(8);
    if (system_only) {
        add_button(hwnd, L"Ne&w...", w - S(32) - 3 * bw - S(16), y, bw, ID_S_NEW, 0);
        add_button(hwnd, L"Ed&it...", w - S(32) - 2 * bw - S(8), y, bw, ID_S_EDIT, 0);
        add_button(hwnd, L"De&lete", w - S(32) - bw, y, bw, ID_S_DEL, 0);
    } else {
        add_button(hwnd, L"Edit system variables...", w - S(32) - S(230), y, S(230), ID_S_ELEVATE, 0);
        SendMessageW(GetDlgItem(hwnd, ID_S_ELEVATE), BCM_SETSHIELD, 0, TRUE);
    }
    y += S(50);
    add_button(hwnd, L"OK", w - S(32) - 2 * bw - S(8), y, bw, IDOK, BS_DEFPUSHBUTTON);
    add_button(hwnd, L"Cancel", w - S(32) - bw, y, bw, IDCANCEL, 0);
    {
        RECT r = { 0, 0, w, y + S(44) };
        AdjustWindowRectEx(&r, WS_OVERLAPPED | WS_CAPTION, FALSE, WS_EX_DLGMODALFRAME);
        SetWindowPos(hwnd, NULL, 0, 0, w, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    }
    if (!system_only) { list_load(&g_user); list_fill(&g_user); }
    list_load(&g_sys); list_fill(&g_sys);
    ShowWindow(hwnd, SW_SHOW);
    SetFocus(system_only ? g_sys.lv : g_user.lv);
    g_env_done = g_env_ok = FALSE;
    while (!g_env_done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_env_ok) {
        BOOL ok = TRUE, any = g_user.changed || g_sys.changed;
        if (!system_only) ok = list_save(&g_user);
        if (system_only) ok = list_save(&g_sys) && ok;
        if (!ok) message(hwnd, L"Environment Variables", L"The variables could not be saved.", TRUE);
        if (any) {
            DWORD_PTR r;
            /* the shell, and programs that listen, read them again */
            SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 5000, &r);
        }
    }
    DestroyWindow(hwnd);
    list_free(&g_user);
    list_free(&g_sys);
    return g_env_ok ? 0 : 1;
}
