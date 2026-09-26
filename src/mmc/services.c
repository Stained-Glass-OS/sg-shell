/* sg-mmc -- Services (services.msc): the Windows services from the Service
 * Control Manager, and, read-only beside them, the Linux (systemd) services
 * Stained Glass runs on.
 *
 * Everything goes through the SCM's own API with only the access each step
 * needs, so the SCM decides who may do what (wine-sg 0141: a standard user
 * may look, an administrator may start, stop, pause and reconfigure). This
 * console decides nothing; it reports what the SCM says, in Windows' words.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "mmc.h"
#include <string.h>
#include <winsvc.h>
#include <sddl.h>

enum { V_START = 1, V_STOP, V_PAUSE, V_RESUME, V_RESTART };

typedef struct svc
{
    WCHAR name[256];
    WCHAR display[256];
    WCHAR desc[1024];
    WCHAR path[1024];
    WCHAR account[256];
    WCHAR deps[1024];           /* REG_MULTI_SZ-like, '\0' separated */
    DWORD state, accepted, start, type, pid;
    BOOL delayed;
} svc_t;

static svc_t *g_svcs;
static int g_nsvcs;
static WCHAR (*g_keys)[256];    /* stable row keys: index+1 into every name seen */
static int g_nkeys, g_capkeys;

static LPARAM key_for(const WCHAR *name)
{
    int i;
    for (i = 0; i < g_nkeys; i++) if (!_wcsicmp(g_keys[i], name)) return i + 1;
    if (g_nkeys == g_capkeys)
    {
        int cap = g_capkeys ? g_capkeys * 2 : 256;
        void *p = realloc(g_keys, cap * sizeof(*g_keys));
        if (!p) return 0;
        g_keys = p;
        g_capkeys = cap;
    }
    lstrcpynW(g_keys[g_nkeys], name, 256);
    return ++g_nkeys;
}

static svc_t *svc_by_key(LPARAM key)
{
    int i;
    if (key < 1 || key > g_nkeys) return NULL;
    for (i = 0; i < g_nsvcs; i++) if (!_wcsicmp(g_svcs[i].name, g_keys[key - 1])) return &g_svcs[i];
    return NULL;
}

static const WCHAR *state_text(DWORD s)
{
    switch (s)
    {
    case SERVICE_RUNNING: return L"Running";
    case SERVICE_PAUSED: return L"Paused";
    case SERVICE_START_PENDING: return L"Starting";
    case SERVICE_STOP_PENDING: return L"Stopping";
    case SERVICE_PAUSE_PENDING: return L"Pausing";
    case SERVICE_CONTINUE_PENDING: return L"Resuming";
    }
    return L"";
}

static const WCHAR *start_text(const svc_t *s)
{
    switch (s->start)
    {
    case SERVICE_AUTO_START: return s->delayed ? L"Automatic (Delayed Start)" : L"Automatic";
    case SERVICE_DEMAND_START: return L"Manual";
    case SERVICE_DISABLED: return L"Disabled";
    case SERVICE_BOOT_START: return L"Boot";
    case SERVICE_SYSTEM_START: return L"System";
    }
    return L"";
}

static void account_text(const WCHAR *acc, WCHAR *out, int cch)
{
    if (!acc[0] || !_wcsicmp(acc, L"LocalSystem") || !_wcsicmp(acc, L".\\LocalSystem"))
        lstrcpynW(out, L"Local System", cch);
    else if (!_wcsicmp(acc, L"NT AUTHORITY\\LocalService")) lstrcpynW(out, L"Local Service", cch);
    else if (!_wcsicmp(acc, L"NT AUTHORITY\\NetworkService")) lstrcpynW(out, L"Network Service", cch);
    else lstrcpynW(out, acc, cch);
}

/* the service's current configuration; a field the caller may not read stays empty */
static void load_config(SC_HANDLE scm, svc_t *s)
{
    SC_HANDLE h = OpenServiceW(scm, s->name, SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS);
    BYTE buf[8192];
    DWORD need;
    if (!h) return;
    if (QueryServiceConfigW(h, (QUERY_SERVICE_CONFIGW *)buf, sizeof(buf), &need))
    {
        QUERY_SERVICE_CONFIGW *c = (QUERY_SERVICE_CONFIGW *)buf;
        const WCHAR *d;
        WCHAR *o = s->deps;
        s->start = c->dwStartType;
        lstrcpynW(s->path, c->lpBinaryPathName ? c->lpBinaryPathName : L"", ARRAY_SIZE(s->path));
        lstrcpynW(s->account, c->lpServiceStartName ? c->lpServiceStartName : L"", ARRAY_SIZE(s->account));
        for (d = c->lpDependencies; d && *d && o + wcslen(d) + 2 < s->deps + ARRAY_SIZE(s->deps); d += wcslen(d) + 1)
        {
            lstrcpyW(o, d);
            o += wcslen(o) + 1;
        }
        *o = 0;
    }
    if (QueryServiceConfig2W(h, SERVICE_CONFIG_DESCRIPTION, buf, sizeof(buf), &need))
    {
        SERVICE_DESCRIPTIONW *d = (SERVICE_DESCRIPTIONW *)buf;
        lstrcpynW(s->desc, d->lpDescription ? d->lpDescription : L"", ARRAY_SIZE(s->desc));
    }
    if (QueryServiceConfig2W(h, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, buf, sizeof(buf), &need))
        s->delayed = ((SERVICE_DELAYED_AUTO_START_INFO *)buf)->fDelayedAutostart;
    CloseServiceHandle(h);
}

static DWORD load_services(void)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    DWORD need = 0, count = 0, resume = 0, i, err = 0;
    ENUM_SERVICE_STATUS_PROCESSW *e;
    BYTE *buf = NULL;

    free(g_svcs);
    g_svcs = NULL;
    g_nsvcs = 0;
    if (!scm) return GetLastError();
    EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, NULL, 0, &need, &count,
                          &resume, NULL);
    buf = malloc(need + 4096);
    resume = 0;
    if (!buf || !EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buf,
                                       need + 4096, &need, &count, &resume, NULL))
        err = GetLastError();
    else
    {
        e = (ENUM_SERVICE_STATUS_PROCESSW *)buf;
        g_svcs = calloc(count ? count : 1, sizeof(svc_t));
        for (i = 0; g_svcs && i < count; i++)
        {
            svc_t *s = &g_svcs[g_nsvcs++];
            lstrcpynW(s->name, e[i].lpServiceName, ARRAY_SIZE(s->name));
            lstrcpynW(s->display, e[i].lpDisplayName && e[i].lpDisplayName[0] ? e[i].lpDisplayName : e[i].lpServiceName,
                      ARRAY_SIZE(s->display));
            s->state = e[i].ServiceStatusProcess.dwCurrentState;
            s->accepted = e[i].ServiceStatusProcess.dwControlsAccepted;
            s->type = e[i].ServiceStatusProcess.dwServiceType;
            s->pid = e[i].ServiceStatusProcess.dwProcessId;
            load_config(scm, s);
        }
    }
    free(buf);
    CloseServiceHandle(scm);
    return err;
}

static void fill_row(const svc_t *s, int *icon, WCHAR cells[5][512])
{
    lstrcpynW(cells[0], s->display, 512);
    lstrcpynW(cells[1], s->desc, 512);
#ifdef SG_MUTANT_STATUS
    lstrcpynW(cells[2], L"", 512);
#else
    lstrcpynW(cells[2], state_text(s->state), 512);
#endif
    lstrcpynW(cells[3], start_text(s), 512);
    account_text(s->account, cells[4], 512);
    *icon = IC_SERVICE;
}

static void show_services(node_t *n)
{
    static const WCHAR *const cols[] = { L"Name", L"Description", L"Status", L"Startup Type", L"Log On As" };
    static const int widths[] = { 230, 300, 80, 150, 110 };
    DWORD err;
    int i;

    (void)n;
    pane_columns(cols, widths, 5);
    err = load_services();
    pane_begin();
    for (i = 0; i < g_nsvcs; i++)
    {
        WCHAR cells[5][512];
        const WCHAR *p[5];
        int icon, c;
        fill_row(&g_svcs[i], &icon, cells);
        for (c = 0; c < 5; c++) p[c] = cells[c];
        pane_add(key_for(g_svcs[i].name), icon, p);
    }
    pane_end();
    if (err)
    {
        WCHAR msg[256];
        error_text(err, msg, 256);
        pane_empty_text(msg);
        frame_status(L"The service list could not be read: %ls", msg);
    }
    else frame_status(L"%d services", g_nsvcs);
    if (!is_admin())
        frame_banner(L"You are signed in as a standard user. You can view services, but only an administrator "
                     L"can start, stop or change them.");
}

/* refresh one row from the SCM (after an action) */
static void refresh_row(LPARAM key)
{
    svc_t *s = svc_by_key(key);
    SC_HANDLE scm, h;
    SERVICE_STATUS_PROCESS st;
    DWORD need;
    WCHAR cells[5][512];
    int icon, c;
    if (!s) return;
    if (!(scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT))) return;
    if ((h = OpenServiceW(scm, s->name, SERVICE_QUERY_STATUS)))
    {
        if (QueryServiceStatusEx(h, SC_STATUS_PROCESS_INFO, (BYTE *)&st, sizeof(st), &need))
        {
            s->state = st.dwCurrentState;
            s->accepted = st.dwControlsAccepted;
            s->pid = st.dwProcessId;
        }
        CloseServiceHandle(h);
    }
    load_config(scm, s);
    CloseServiceHandle(scm);
    fill_row(s, &icon, cells);
    for (c = 0; c < 5; c++) pane_set(key, c, cells[c]);
    frame_update_verbs();
}

static void svc_verbs(node_t *n, LPARAM key, BOOL have, verbs_t *out)
{
    svc_t *s;
    (void)n;
    if (!have || !(s = svc_by_key(key))) return;
    out->v[out->n++] = (verb_t){ V_START, L"&Start", IC_START,
        s->state == SERVICE_STOPPED && s->start != SERVICE_DISABLED, FALSE };
    out->v[out->n++] = (verb_t){ V_STOP, L"S&top", IC_STOP,
        s->state == SERVICE_RUNNING && (s->accepted & SERVICE_ACCEPT_STOP), FALSE };
    out->v[out->n++] = (verb_t){ V_PAUSE, L"&Pause", IC_PAUSE,
        s->state == SERVICE_RUNNING && (s->accepted & SERVICE_ACCEPT_PAUSE_CONTINUE), FALSE };
    out->v[out->n++] = (verb_t){ V_RESUME, L"Resu&me", -1,
        s->state == SERVICE_PAUSED && (s->accepted & SERVICE_ACCEPT_PAUSE_CONTINUE), FALSE };
    out->v[out->n++] = (verb_t){ V_RESTART, L"R&estart", IC_RESTART,
        s->state == SERVICE_RUNNING && (s->accepted & SERVICE_ACCEPT_STOP), FALSE };
}

/* ---- doing it: Windows' progress box while the service changes state ------------------ */

static HWND g_progress, g_progress_bar;

static void progress_open(const WCHAR *what, const svc_t *s)
{
    WCHAR text[512];
    RECT rc;
    HWND lbl, name;
    GetWindowRect(g_main, &rc);
    g_progress = CreateWindowExW(WS_EX_DLGMODALFRAME, L"#32770", L"Service Control",
                                 WS_POPUP | WS_CAPTION | WS_VISIBLE, (rc.left + rc.right) / 2 - S(200),
                                 (rc.top + rc.bottom) / 2 - S(70), S(400), S(140), g_main, NULL, g_inst, NULL);
    _snwprintf(text, ARRAY_SIZE(text), L"Stained Glass is attempting to %ls the following service on Local Computer...", what);
    lbl = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_NOPREFIX, S(16), S(12), S(360), S(34),
                          g_progress, NULL, g_inst, NULL);
    name = CreateWindowExW(0, L"STATIC", s->display, WS_CHILD | WS_VISIBLE | SS_NOPREFIX, S(16), S(46), S(360), S(18),
                           g_progress, NULL, g_inst, NULL);
    g_progress_bar = CreateWindowExW(0, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE, S(16), S(70), S(360), S(16),
                                     g_progress, NULL, g_inst, NULL);
    SendMessageW(lbl, WM_SETFONT, (WPARAM)g_font, 0);
    SendMessageW(name, WM_SETFONT, (WPARAM)g_font_bold, 0);
    SendMessageW(g_progress_bar, PBM_SETRANGE32, 0, 100);
    EnableWindow(g_main, FALSE);
    frame_status(L"%ls %ls...", what, s->display);
}

static void progress_close(void)
{
    EnableWindow(g_main, TRUE);
    if (g_progress) DestroyWindow(g_progress);
    g_progress = NULL;
    SetForegroundWindow(g_main);
}

static void pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG msg;
    while ((int)(end - GetTickCount()) > 0)
    {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, 50, QS_ALLINPUT);
    }
}

/* wait (with the progress bar) until the service leaves `pending` or 30 s pass */
static DWORD wait_state(SC_HANDLE h, DWORD pending, int base)
{
    SERVICE_STATUS st;
    int i;
    for (i = 0; i < 300; i++)
    {
        if (!QueryServiceStatus(h, &st)) return 0;
        if (st.dwCurrentState != pending) return st.dwCurrentState;
        SendMessageW(g_progress_bar, PBM_SETPOS, base + (i * 3) % (100 - base), 0);
        pump(100);
    }
    return st.dwCurrentState;
}

static void report_failure(const WCHAR *verb, const svc_t *s, DWORD err)
{
    WCHAR msg[256];
    error_text(err, msg, 256);
    if (err == ERROR_ACCESS_DENIED && !is_admin())
        frame_message(MB_OK | MB_ICONERROR, L"Services",
                      L"Stained Glass could not %ls the %ls service on Local Computer.\n\nError %lu: %ls\n\n"
                      L"Only an administrator can %ls services. Run Services as an administrator.",
                      verb, s->display, err, msg, verb);
    else
        frame_message(MB_OK | MB_ICONERROR, L"Services",
                      L"Stained Glass could not %ls the %ls service on Local Computer.\n\nError %lu: %ls",
                      verb, s->display, err, msg);
}

static BOOL do_start(svc_t *s, const WCHAR *params)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), h;
    DWORD err = 0, state;
    const WCHAR *argv[32];
    WCHAR buf[1024], *p;
    int argc = 0;

    if (!scm) { report_failure(L"start", s, GetLastError()); return FALSE; }
    if (!(h = OpenServiceW(scm, s->name, SERVICE_START | SERVICE_QUERY_STATUS)))
    {
        err = GetLastError();
        CloseServiceHandle(scm);
        report_failure(L"start", s, err);
        return FALSE;
    }
    if (params && *params)
    {
        lstrcpynW(buf, params, ARRAY_SIZE(buf));
        for (p = buf; *p && argc < 32; )
        {
            while (*p == ' ' || *p == '\t') *p++ = 0;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
    }
    progress_open(L"start", s);
#ifdef SG_MUTANT_NOSTART
    if (0)
#else
    if (!StartServiceW(h, argc, argc ? argv : NULL))
#endif
        err = GetLastError();
    else if ((state = wait_state(h, SERVICE_START_PENDING, 10)) != SERVICE_RUNNING && state != SERVICE_START_PENDING)
        err = ERROR_SERVICE_REQUEST_TIMEOUT;
    progress_close();
    CloseServiceHandle(h);
    CloseServiceHandle(scm);
    if (err)
    {
        if (err == ERROR_SERVICE_REQUEST_TIMEOUT)
            frame_message(MB_OK | MB_ICONERROR, L"Services",
                          L"The %ls service on Local Computer started and then stopped. Some services stop "
                          L"automatically if they are not in use by other services or programs.", s->display);
        else report_failure(L"start", s, err);
        return FALSE;
    }
    return TRUE;
}

static BOOL do_control(svc_t *s, DWORD code)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), h;
    SERVICE_STATUS st;
    DWORD err = 0, access = code == SERVICE_CONTROL_STOP ? SERVICE_STOP : SERVICE_PAUSE_CONTINUE;
    DWORD pending = code == SERVICE_CONTROL_STOP ? SERVICE_STOP_PENDING :
                    code == SERVICE_CONTROL_PAUSE ? SERVICE_PAUSE_PENDING : SERVICE_CONTINUE_PENDING;
    const WCHAR *verb = code == SERVICE_CONTROL_STOP ? L"stop" : code == SERVICE_CONTROL_PAUSE ? L"pause" : L"resume";

    if (!scm) { report_failure(verb, s, GetLastError()); return FALSE; }
    if (!(h = OpenServiceW(scm, s->name, access | SERVICE_QUERY_STATUS)))
    {
        err = GetLastError();
        CloseServiceHandle(scm);
        report_failure(verb, s, err);
        return FALSE;
    }
    progress_open(verb, s);
    if (!ControlService(h, code, &st)) err = GetLastError();
    else wait_state(h, pending, 10);
    progress_close();
    CloseServiceHandle(h);
    CloseServiceHandle(scm);
    if (err) { report_failure(verb, s, err); return FALSE; }
    return TRUE;
}

static void svc_invoke(node_t *n, LPARAM key, BOOL have, int verb)
{
    svc_t *s = svc_by_key(key);
    (void)n;
    if (!have || !s) return;
    switch (verb)
    {
    case V_START: do_start(s, NULL); break;
    case V_STOP: do_control(s, SERVICE_CONTROL_STOP); break;
    case V_PAUSE: do_control(s, SERVICE_CONTROL_PAUSE); break;
    case V_RESUME: do_control(s, SERVICE_CONTROL_CONTINUE); break;
    case V_RESTART: if (do_control(s, SERVICE_CONTROL_STOP)) { refresh_row(key); do_start(s, NULL); } break;
    }
    refresh_row(key);
    frame_status(L"%ls: %ls", s->display, state_text(s->state)[0] ? state_text(s->state) : L"Stopped");
}

/* ---- Properties ------------------------------------------------------------------------ */

enum
{
    P_NAME = 1100, P_DISPLAY, P_DESC, P_PATH, P_STARTUP, P_STATUS, P_START, P_STOP, P_PAUSE, P_RESUME, P_PARAMS,
    L_SYSTEM = 1200, L_INTERACT, L_THIS, L_ACCOUNT, L_PASS, L_CONFIRM,
    D_ON = 1300, D_BY,
    S_LIST = 1400, S_SDDL,
};

typedef struct props { svc_t *s; LPARAM key; BOOL dirty_general, dirty_logon; } props_t;

static void general_status(HWND dlg, props_t *p)
{
    svc_t *s = p->s;
    SetDlgItemTextW(dlg, P_STATUS, state_text(s->state)[0] ? state_text(s->state) : L"Stopped");
    EnableWindow(GetDlgItem(dlg, P_START), s->state == SERVICE_STOPPED);
    EnableWindow(GetDlgItem(dlg, P_STOP), s->state == SERVICE_RUNNING && (s->accepted & SERVICE_ACCEPT_STOP));
    EnableWindow(GetDlgItem(dlg, P_PAUSE), s->state == SERVICE_RUNNING && (s->accepted & SERVICE_ACCEPT_PAUSE_CONTINUE));
    EnableWindow(GetDlgItem(dlg, P_RESUME), s->state == SERVICE_PAUSED);
}

static const WCHAR *const startup_names[] = { L"Automatic (Delayed Start)", L"Automatic", L"Manual", L"Disabled" };

static INT_PTR CALLBACK general_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    props_t *p = (props_t *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        int i, sel;
        p = (props_t *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)p);
        SetDlgItemTextW(dlg, P_NAME, p->s->name);
        SetDlgItemTextW(dlg, P_DISPLAY, p->s->display);
        SetDlgItemTextW(dlg, P_DESC, p->s->desc);
        SetDlgItemTextW(dlg, P_PATH, p->s->path);
        for (i = 0; i < 4; i++) SendDlgItemMessageW(dlg, P_STARTUP, CB_ADDSTRING, 0, (LPARAM)startup_names[i]);
        sel = p->s->start == SERVICE_AUTO_START ? (p->s->delayed ? 0 : 1) : p->s->start == SERVICE_DISABLED ? 3 : 2;
        SendDlgItemMessageW(dlg, P_STARTUP, CB_SETCURSEL, sel, 0);
        general_status(dlg, p);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case P_STARTUP:
            if (HIWORD(wp) == CBN_SELCHANGE) { p->dirty_general = TRUE; PropSheet_Changed(GetParent(dlg), dlg); }
            return TRUE;
        case P_DISPLAY:
        case P_DESC:
            if (HIWORD(wp) == EN_CHANGE) { p->dirty_general = TRUE; PropSheet_Changed(GetParent(dlg), dlg); }
            return TRUE;
        case P_START:
        {
            WCHAR params[512];
            GetDlgItemTextW(dlg, P_PARAMS, params, ARRAY_SIZE(params));
            do_start(p->s, params);
            refresh_row(p->key);
            general_status(dlg, p);
            return TRUE;
        }
        case P_STOP: do_control(p->s, SERVICE_CONTROL_STOP); refresh_row(p->key); general_status(dlg, p); return TRUE;
        case P_PAUSE: do_control(p->s, SERVICE_CONTROL_PAUSE); refresh_row(p->key); general_status(dlg, p); return TRUE;
        case P_RESUME: do_control(p->s, SERVICE_CONTROL_CONTINUE); refresh_row(p->key); general_status(dlg, p); return TRUE;
        }
        break;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == PSN_APPLY && p->dirty_general)
        {
            SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), h = NULL;
            int sel = (int)SendDlgItemMessageW(dlg, P_STARTUP, CB_GETCURSEL, 0, 0);
            DWORD start = sel <= 1 ? SERVICE_AUTO_START : sel == 3 ? SERVICE_DISABLED : SERVICE_DEMAND_START, err = 0;
            WCHAR display[256], desc[1024];
            SERVICE_DESCRIPTIONW d;
            SERVICE_DELAYED_AUTO_START_INFO del;

            GetDlgItemTextW(dlg, P_DISPLAY, display, ARRAY_SIZE(display));
            GetDlgItemTextW(dlg, P_DESC, desc, ARRAY_SIZE(desc));
            if (!scm || !(h = OpenServiceW(scm, p->s->name, SERVICE_CHANGE_CONFIG))) err = GetLastError();
            else if (!ChangeServiceConfigW(h, SERVICE_NO_CHANGE, start, SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL,
                                           NULL, NULL, wcscmp(display, p->s->display) ? display : NULL))
                err = GetLastError();
            else
            {
                if (wcscmp(desc, p->s->desc))
                {
                    d.lpDescription = desc;
                    ChangeServiceConfig2W(h, SERVICE_CONFIG_DESCRIPTION, &d);
                }
                if (start == SERVICE_AUTO_START)
                {
                    del.fDelayedAutostart = sel == 0;
                    ChangeServiceConfig2W(h, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &del);
                }
            }
            if (h) CloseServiceHandle(h);
            if (scm) CloseServiceHandle(scm);
            if (err)
            {
                WCHAR m[256];
                error_text(err, m, 256);
                frame_message(MB_OK | MB_ICONERROR, L"Services",
                              L"The changes to the %ls service could not be saved.\n\nError %lu: %ls",
                              p->s->display, err, m);
                SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
                return TRUE;
            }
            p->dirty_general = FALSE;
            refresh_row(p->key);
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK logon_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    props_t *p = (props_t *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        const WCHAR *a;
        BOOL system;
        p = (props_t *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)p);
        a = p->s->account;
        system = !a[0] || !_wcsicmp(a, L"LocalSystem") || !_wcsicmp(a, L".\\LocalSystem");
        CheckRadioButton(dlg, L_SYSTEM, L_THIS, system ? L_SYSTEM : L_THIS);
        CheckDlgButton(dlg, L_INTERACT, (p->s->type & SERVICE_INTERACTIVE_PROCESS) ? BST_CHECKED : BST_UNCHECKED);
        if (!system) SetDlgItemTextW(dlg, L_ACCOUNT, a);
        EnableWindow(GetDlgItem(dlg, L_INTERACT), system);
        EnableWindow(GetDlgItem(dlg, L_ACCOUNT), !system);
        EnableWindow(GetDlgItem(dlg, L_PASS), !system);
        EnableWindow(GetDlgItem(dlg, L_CONFIRM), !system);
        if (!system) { SetDlgItemTextW(dlg, L_PASS, L"***************"); SetDlgItemTextW(dlg, L_CONFIRM, L"***************"); }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == L_SYSTEM || LOWORD(wp) == L_THIS)
        {
            BOOL system = IsDlgButtonChecked(dlg, L_SYSTEM) == BST_CHECKED;
            EnableWindow(GetDlgItem(dlg, L_INTERACT), system);
            EnableWindow(GetDlgItem(dlg, L_ACCOUNT), !system);
            EnableWindow(GetDlgItem(dlg, L_PASS), !system);
            EnableWindow(GetDlgItem(dlg, L_CONFIRM), !system);
            p->dirty_logon = TRUE;
            PropSheet_Changed(GetParent(dlg), dlg);
        }
        else if ((LOWORD(wp) == L_ACCOUNT || LOWORD(wp) == L_PASS || LOWORD(wp) == L_CONFIRM) && HIWORD(wp) == EN_CHANGE)
        {
            p->dirty_logon = TRUE;
            PropSheet_Changed(GetParent(dlg), dlg);
        }
        else if (LOWORD(wp) == L_INTERACT) { p->dirty_logon = TRUE; PropSheet_Changed(GetParent(dlg), dlg); }
        break;
    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == PSN_APPLY && p->dirty_logon)
        {
            WCHAR acct[256], pass[256], confirm[256];
            BOOL system = IsDlgButtonChecked(dlg, L_SYSTEM) == BST_CHECKED;
            SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT), h = NULL;
            DWORD err = 0, type = p->s->type & ~SERVICE_INTERACTIVE_PROCESS;
            GetDlgItemTextW(dlg, L_ACCOUNT, acct, 256);
            GetDlgItemTextW(dlg, L_PASS, pass, 256);
            GetDlgItemTextW(dlg, L_CONFIRM, confirm, 256);
            if (!system && wcscmp(pass, confirm))
            {
                frame_message(MB_OK | MB_ICONERROR, L"Services", L"The password and confirmation password that you entered do not match.");
                SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
                return TRUE;
            }
            if (system && IsDlgButtonChecked(dlg, L_INTERACT) == BST_CHECKED) type |= SERVICE_INTERACTIVE_PROCESS;
            if (!scm || !(h = OpenServiceW(scm, p->s->name, SERVICE_CHANGE_CONFIG))) err = GetLastError();
            else if (!ChangeServiceConfigW(h, type, SERVICE_NO_CHANGE, SERVICE_NO_CHANGE, NULL, NULL, NULL, NULL,
                                           system ? L"LocalSystem" : acct,
                                           system ? L"" : (wcscmp(pass, L"***************") ? pass : NULL), NULL))
                err = GetLastError();
            SecureZeroMemory(pass, sizeof(pass));
            SecureZeroMemory(confirm, sizeof(confirm));
            if (h) CloseServiceHandle(h);
            if (scm) CloseServiceHandle(scm);
            if (err)
            {
                WCHAR m[256];
                error_text(err, m, 256);
                frame_message(MB_OK | MB_ICONERROR, L"Services", L"The account for the %ls service could not be changed.\n\nError %lu: %ls",
                              p->s->display, err, m);
                SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_INVALID_NOCHANGEPAGE);
                return TRUE;
            }
            p->dirty_logon = FALSE;
            refresh_row(p->key);
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, PSNRET_NOERROR);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

static INT_PTR CALLBACK deps_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    if (msg == WM_INITDIALOG)
    {
        props_t *p = (props_t *)((PROPSHEETPAGEW *)lp)->lParam;
        const WCHAR *d;
        SC_HANDLE scm, h;
        for (d = p->s->deps; *d; d += wcslen(d) + 1)
            SendDlgItemMessageW(dlg, D_ON, LB_ADDSTRING, 0, (LPARAM)(d[0] == SC_GROUP_IDENTIFIERW ? d + 1 : d));
        if ((scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT)))
        {
            if ((h = OpenServiceW(scm, p->s->name, SERVICE_ENUMERATE_DEPENDENTS)))
            {
                BYTE buf[16384];
                DWORD need, n = 0, i;
                if (EnumDependentServicesW(h, SERVICE_STATE_ALL, (ENUM_SERVICE_STATUSW *)buf, sizeof(buf), &need, &n))
                    for (i = 0; i < n; i++)
                        SendDlgItemMessageW(dlg, D_BY, LB_ADDSTRING, 0, (LPARAM)((ENUM_SERVICE_STATUSW *)buf)[i].lpDisplayName);
                CloseServiceHandle(h);
            }
            CloseServiceHandle(scm);
        }
        if (!SendDlgItemMessageW(dlg, D_ON, LB_GETCOUNT, 0, 0))
            SendDlgItemMessageW(dlg, D_ON, LB_ADDSTRING, 0, (LPARAM)L"<No Dependencies>");
        if (!SendDlgItemMessageW(dlg, D_BY, LB_GETCOUNT, 0, 0))
            SendDlgItemMessageW(dlg, D_BY, LB_ADDSTRING, 0, (LPARAM)L"<No Dependencies>");
        return TRUE;
    }
    return FALSE;
}

/* ---- Security: who may do what with the service (its own descriptor, wine-sg 0185), read-only ---- */

static WCHAR g_last_sddl[1024];

static const WCHAR *rights_text(DWORD m)
{
    if ((m & SERVICE_ALL_ACCESS) == SERVICE_ALL_ACCESS || (m & GENERIC_ALL)) return L"Full control";
    if ((m & (SERVICE_START | SERVICE_STOP)) == (SERVICE_START | SERVICE_STOP)) return L"Start, stop and read";
    if (m & SERVICE_START) return L"Start and read";
    if (m & (SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG | GENERIC_READ)) return L"Read";
    return L"Special permissions";
}

static INT_PTR CALLBACK security_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    (void)wp;
    if (msg == WM_INITDIALOG)
    {
        props_t *p = (props_t *)((PROPSHEETPAGEW *)lp)->lParam;
        SC_HANDLE scm, h = NULL;
        BYTE sdbuf[8192];
        DWORD need = 0;
        BOOL ok = FALSE;
        g_last_sddl[0] = 0;
        if ((scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT)))
        {
            if ((h = OpenServiceW(scm, p->s->name, READ_CONTROL)))
                ok = QueryServiceObjectSecurity(h, DACL_SECURITY_INFORMATION, (PSECURITY_DESCRIPTOR)sdbuf, sizeof(sdbuf), &need);
            if (h) CloseServiceHandle(h);
            CloseServiceHandle(scm);
        }
        if (ok)
        {
            BOOL present = FALSE, def;
            PACL acl = NULL;
            WCHAR *sddl = NULL;
            DWORD i;
            if (ConvertSecurityDescriptorToStringSecurityDescriptorW(sdbuf, SDDL_REVISION_1, DACL_SECURITY_INFORMATION, &sddl, NULL))
            {
                lstrcpynW(g_last_sddl, sddl, ARRAY_SIZE(g_last_sddl));
                SetDlgItemTextW(dlg, S_SDDL, sddl);
                LocalFree(sddl);
            }
            if (GetSecurityDescriptorDacl(sdbuf, &present, &acl, &def) && present && acl)
                for (i = 0; i < acl->AceCount; i++)
                {
                    ACCESS_ALLOWED_ACE *ace;
                    WCHAR name[256], dom[256], line[600];
                    DWORD nl = 256, dl = 256;
                    SID_NAME_USE use;
                    if (!GetAce(acl, i, (void **)&ace)) continue;
                    if (!LookupAccountSidW(NULL, &ace->SidStart, name, &nl, dom, &dl, &use))
                    {
                        WCHAR *str = NULL;
                        ConvertSidToStringSidW(&ace->SidStart, &str);
                        lstrcpynW(name, str ? str : L"?", 256);
                        LocalFree(str);
                        dom[0] = 0;
                    }
                    _snwprintf(line, ARRAY_SIZE(line), L"%ls%ls%ls: %ls %ls", dom, dom[0] ? L"\\" : L"", name,
                               ace->Header.AceType == ACCESS_DENIED_ACE_TYPE ? L"Deny" : L"Allow", rights_text(ace->Mask));
                    SendDlgItemMessageW(dlg, S_LIST, LB_ADDSTRING, 0, (LPARAM)line);
                }
        }
        else
        {
            WCHAR m[256];
            error_text(GetLastError(), m, 256);
            SendDlgItemMessageW(dlg, S_LIST, LB_ADDSTRING, 0, (LPARAM)m);
        }
        frame_dump_later();
        return TRUE;
    }
    return FALSE;
}

static void svc_open(node_t *n, LPARAM key)
{
    static dlgt_t general, logon, deps, security;
    PROPSHEETPAGEW pg[4] = { { 0 } };
    PROPSHEETHEADERW ph = { 0 };
    WCHAR title[300];
    props_t p = { 0 };
    DWORD pstyle = DS_SHELLFONT | WS_CHILD | WS_CAPTION;
    int i;

    (void)n;
    if (!(p.s = svc_by_key(key))) return;
    p.key = key;

    dlg_begin(&general, L"General", pstyle, 252, 218);
    D_LABEL(&general, L"Service name:", 7, 10, 70);
    dlg_item(&general, NULL, ATOM_EDIT, L"", P_NAME, ES_READONLY | ES_AUTOHSCROLL, 80, 9, 165, 10);
    D_LABEL(&general, L"Display name:", 7, 28, 70);
    dlg_item(&general, NULL, ATOM_EDIT, L"", P_DISPLAY, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 80, 26, 165, 13);
    D_LABEL(&general, L"Description:", 7, 46, 70);
    dlg_item(&general, NULL, ATOM_EDIT, L"", P_DESC, WS_BORDER | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
             80, 44, 165, 26);
    D_LABEL(&general, L"Path to executable:", 7, 76, 120);
    D_EDITRO(&general, P_PATH, 7, 87, 238, 12, ES_AUTOHSCROLL | WS_BORDER);
    dlg_item(&general, NULL, ATOM_STATIC, L"Start&up type:", 0xFFFF, SS_LEFT, 7, 107, 70, 8);
    dlg_item(&general, NULL, ATOM_COMBO, L"", P_STARTUP, CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 80, 105, 165, 80);
    D_LABEL(&general, L"Service status:", 7, 128, 70);
    D_VALUE(&general, P_STATUS, 80, 128, 165);
    D_BUTTON(&general, L"&Start", P_START, 7, 142, 56);
    D_BUTTON(&general, L"S&top", P_STOP, 67, 142, 56);
    D_BUTTON(&general, L"&Pause", P_PAUSE, 127, 142, 56);
    D_BUTTON(&general, L"&Resume", P_RESUME, 187, 142, 56);
    dlg_item(&general, NULL, ATOM_STATIC, L"You can specify the start parameters that apply when you start the service from here.",
             0xFFFF, SS_LEFT | SS_NOPREFIX, 7, 162, 238, 16);
    dlg_item(&general, NULL, ATOM_STATIC, L"Start para&meters:", 0xFFFF, SS_LEFT, 7, 184, 70, 8);
    dlg_item(&general, NULL, ATOM_EDIT, L"", P_PARAMS, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 80, 182, 165, 13);

    dlg_begin(&logon, L"Log On", pstyle, 252, 218);
    D_LABEL(&logon, L"Log on as:", 7, 8, 100);
    dlg_item(&logon, NULL, ATOM_BUTTON, L"&Local System account", L_SYSTEM, BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 12, 22, 200, 10);
    dlg_item(&logon, NULL, ATOM_BUTTON, L"Allo&w service to interact with desktop", L_INTERACT, BS_AUTOCHECKBOX | WS_TABSTOP, 24, 36, 200, 10);
    dlg_item(&logon, NULL, ATOM_BUTTON, L"&This account:", L_THIS, BS_AUTORADIOBUTTON | WS_TABSTOP, 12, 56, 70, 10);
    dlg_item(&logon, NULL, ATOM_EDIT, L"", L_ACCOUNT, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | WS_GROUP, 90, 55, 150, 13);
    dlg_item(&logon, NULL, ATOM_STATIC, L"&Password:", 0xFFFF, SS_LEFT, 24, 76, 60, 8);
    dlg_item(&logon, NULL, ATOM_EDIT, L"", L_PASS, WS_BORDER | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 90, 74, 150, 13);
    dlg_item(&logon, NULL, ATOM_STATIC, L"&Confirm password:", 0xFFFF, SS_LEFT, 24, 94, 64, 8);
    dlg_item(&logon, NULL, ATOM_EDIT, L"", L_CONFIRM, WS_BORDER | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL, 90, 92, 150, 13);

    dlg_begin(&deps, L"Dependencies", pstyle, 252, 218);
    D_LABEL(&deps, L"Some services depend on other services, system drivers or load order groups. If a system "
                   L"component is stopped, or is not running properly, dependent services can be affected.", 7, 7, 238);
    D_LABEL(&deps, L"This service depends on the following system components:", 7, 38, 238);
    dlg_item(&deps, NULL, ATOM_LISTBOX, L"", D_ON, WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, 7, 50, 238, 64);
    D_LABEL(&deps, L"The following system components depend on this service:", 7, 122, 238);
    dlg_item(&deps, NULL, ATOM_LISTBOX, L"", D_BY, WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, 7, 134, 238, 64);

    dlg_begin(&security, L"Security", pstyle, 252, 218);
    D_LABEL(&security, L"Group or user names and what they may do with this service:", 7, 7, 238);
    dlg_item(&security, NULL, ATOM_LISTBOX, L"", S_LIST, WS_BORDER | WS_VSCROLL | LBS_NOINTEGRALHEIGHT | LBS_NOSEL, 7, 19, 238, 110);
    D_LABEL(&security, L"The service's security descriptor (SDDL):", 7, 136, 238);
    dlg_item(&security, NULL, ATOM_STATIC, L"", S_SDDL, SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL | WS_BORDER, 7, 148, 238, 36);
    D_LABEL(&security, L"An administrator changes it with sc sdset.", 7, 192, 238);

    {
        dlgt_t *t[4] = { &general, &logon, &deps, &security };
        DLGPROC procs[4] = { general_proc, logon_proc, deps_proc, security_proc };
        for (i = 0; i < 4; i++)
        {
            pg[i].dwSize = sizeof(pg[i]);
            pg[i].dwFlags = PSP_DLGINDIRECT;
            pg[i].hInstance = g_inst;
            pg[i].pResource = t[i]->t;
            pg[i].pfnDlgProc = procs[i];
            pg[i].lParam = (LPARAM)&p;
        }
    }
    _snwprintf(title, ARRAY_SIZE(title), L"%ls Properties (Local Computer)", p.s->display);
    ph.dwSize = sizeof(ph);
    ph.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW * 0 | PSH_NOCONTEXTHELP;
    ph.hwndParent = g_main;
    ph.hInstance = g_inst;
    ph.pszCaption = title;
    ph.nPages = 4;
    ph.ppsp = pg;
    PropertySheetW(&ph);
    g_last_sddl[0] = 0;
    refresh_row(key);
}

static void svc_selchange(node_t *n, LPARAM key, BOOL have)
{
    svc_t *s = svc_by_key(key);
    (void)n;
    if (have && s) frame_status(L"%ls", s->desc[0] ? s->desc : s->display);
}

static void svc_tick(node_t *n)
{
    /* the states change under us (another console, a service that stops by
     * itself): refresh every 5 s, as Windows' console does on F5 */
    static int t;
    LPARAM key;
    (void)n;
    if (++t % 5) return;
    if (pane_selected(&key)) refresh_row(key);
}

static void svc_dump(node_t *n, FILE *f)
{
    int i;
    (void)n;
    for (i = 0; i < g_nsvcs; i++)
        fprintf(f, "SERVICE %ls\t%lu\t%lu\t%lu\t%ls\n", g_svcs[i].name, g_svcs[i].state, g_svcs[i].start,
                (unsigned long)key_for(g_svcs[i].name), g_svcs[i].display);
    if (g_last_sddl[0]) fprintf(f, "SVCSECURITY %ls\n", g_last_sddl);
}

static const snapin_t services_ops = {
    NULL, show_services, svc_verbs, svc_invoke, svc_open, svc_selchange, svc_tick, NULL, NULL, svc_dump
};

/* ---- Stained Glass system services (systemd), read-only ---------------------------------------- */

typedef struct unit
{
    char *id;
    int block;                  /* line index in the reply */
} unit_t;

static sys_reply_t g_units;

static const char *ufield(int block, const char *key)
{
    return sys_field(&g_units, block, key);
}

static void show_units(node_t *n)
{
    static const WCHAR *const cols[] = { L"Name", L"Description", L"Status", L"Startup Type", L"Since" };
    static const int widths[] = { 230, 300, 90, 110, 150 };
    int b;
    (void)n;
    pane_columns(cols, widths, 5);
    sys_free(&g_units);
    sys_request(&g_units, "units", NULL);
    pane_begin();
    for (b = sys_next_block(&g_units, 0, "UNIT"); b >= 0; b = sys_next_block(&g_units, b + 1, "UNIT"))
    {
        WCHAR c[5][512];
        const WCHAR *p[5] = { c[0], c[1], c[2], c[3], c[4] };
        const char *active = ufield(b, "ACTIVE"), *sub = ufield(b, "SUB"), *state = ufield(b, "UNIT-FILE-STATE"),
                   *since = ufield(b, "SINCE");
        utf8_to_w(g_units.lines[b] + 5, c[0], 512);
        utf8_to_w(ufield(b, "DESCRIPTION"), c[1], 512);
        if (active && !strcmp(active, "active")) lstrcpyW(c[2], sub && !strcmp(sub, "running") ? L"Running" :
                                                               sub && !strcmp(sub, "listening") ? L"Listening" :
                                                               sub && !strcmp(sub, "waiting") ? L"Waiting" : L"Active");
        else if (active && !strcmp(active, "failed")) lstrcpyW(c[2], L"Failed");
        else if (active && !strcmp(active, "activating")) lstrcpyW(c[2], L"Starting");
        else lstrcpyW(c[2], L"");
        if (!state) lstrcpyW(c[3], L"");
        else if (!strcmp(state, "enabled") || !strcmp(state, "enabled-runtime")) lstrcpyW(c[3], L"Automatic");
        else if (!strcmp(state, "disabled")) lstrcpyW(c[3], L"Disabled");
        else if (!strcmp(state, "masked")) lstrcpyW(c[3], L"Disabled (masked)");
        else if (!strcmp(state, "static") || !strcmp(state, "indirect")) lstrcpyW(c[3], L"Manual");
        else utf8_to_w(state, c[3], 512);
        c[4][0] = 0;
        if (since && atoll(since) > 0) fmt_time((ULONGLONG)atoll(since), c[4], 512);
        pane_add(b + 1, IC_SGLOGO, p);
    }
    pane_end();
    if (!g_units.ok)
    {
        WCHAR m[512];
        utf8_to_w(g_units.message, m, 512);
        pane_empty_text(m);
        frame_status(L"%ls", m);
    }
    else frame_status(L"The Linux (systemd) services Stained Glass runs on, read-only");
    frame_banner(L"These are the Linux services beneath the system. They are shown read-only: programs manage "
                 L"their services, the system manages these.");
}

static void unit_open(node_t *n, LPARAM key)
{
    int b = (int)key - 1, i;
    WCHAR text[4096] = L"", line[512], id[256];
    (void)n;
    if (b < 0 || b >= g_units.nlines) return;
    for (i = b; i < g_units.nlines && strcmp(g_units.lines[i], "END"); i++)
    {
        utf8_to_w(g_units.lines[i], line, 512);
        if (wcslen(text) + wcslen(line) + 3 < ARRAY_SIZE(text)) { wcscat(text, line); wcscat(text, L"\n"); }
    }
    utf8_to_w(g_units.lines[b] + 5, id, 256);
    frame_message(MB_OK | MB_ICONINFORMATION, id, L"%ls", text);
}

static void unit_dump(node_t *n, FILE *f)
{
    (void)n;
    fprintf(f, "UNITS %d %s\n", g_units.nlines, g_units.ok ? "ok" : g_units.message);
}

static const snapin_t units_ops = {
    NULL, show_units, NULL, NULL, unit_open, NULL, NULL, NULL, NULL, unit_dump
};

node_t *services_create(node_t *parent)
{
    node_t *n = node_add(parent, L"Services (Local)", IC_SERVICES, &services_ops, NULL);
    node_t *u;
    lstrcpyW(n->desc, L"Start, stop and configure services");
    u = node_add(parent, L"Stained Glass System Services", IC_SGLOGO, &units_ops, NULL);
    lstrcpyW(u->desc, L"The Linux services Stained Glass runs on (read-only)");
    return n;
}
