/* sg-mmc -- Event Viewer (eventvwr.msc, eventvwr.exe): the Windows logs from the
 * event log service (wine-sg 0143/0144: OpenEventLog, ReadEventLog, the
 * service keeps the logs and decides who may read them), and a read-only
 * "Stained Glass" log -- the systemd journal of Stained Glass's own services,
 * from sg-session's sg-sysinfo.
 *
 * A log's view is Windows': a header line (the log, the number of events, the
 * filter), the list (Level, Date and Time, Source, Event ID, Task Category)
 * and a preview pane below with General (the message, then the fields) and
 * Details (the inserted strings and the event's data). The message is
 * formatted as Windows does: the source's EventMessageFile (HKLM\System\
 * CurrentControlSet\Services\EventLog\<log>\<source>), FormatMessage with the
 * event's strings; with none, Windows' "The description for Event ID ...
 * cannot be found" and the strings.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "mmc.h"
#include <sddl.h>
#include <string.h>

#define EVT_KEY L"System\\CurrentControlSet\\Services\\EventLog"
#define MAX_EVENTS 20000

enum { LV_CRITICAL = 1, LV_ERROR, LV_WARNING, LV_INFO, LV_VERBOSE, LV_AUDIT_OK, LV_AUDIT_FAIL };
enum { V_FILTER = 1, V_CLEARFILTER, V_CLEARLOG, V_SAVEAS, V_EVENTPROPS, V_COPY, V_OPENSAVED };

typedef struct evt
{
    DWORD record;
    ULONGLONG time;             /* seconds since 1970 */
    int level;
    DWORD id;                   /* full 32-bit event ID */
    WORD category;
    WCHAR source[128];
    WCHAR computer[128];
    WCHAR user[160];
    WCHAR unit[128];            /* the journal: unit */
    DWORD pid;                  /* the journal: process */
    WCHAR *message;             /* formatted when first needed */
    WCHAR **strings;
    int nstrings;
    BYTE *data;
    DWORD ndata;
} evt_t;

typedef struct logview
{
    WCHAR name[128];            /* Application, System, Security, ... */
    BOOL journal;               /* the Stained Glass log */
    WCHAR backup[MAX_PATH];     /* an opened saved log */
    evt_t *ev;
    int n;
    DWORD err;                  /* why it could not be read */
    DWORD count, oldest;        /* to notice new events */
    /* the filter */
    BOOL filtered;
    BOOL levels[8];
    int hours;                  /* 0 = any time */
    WCHAR sources[256];
    WCHAR ids[256];
} logview_t;

static HWND g_header, g_tabs, g_preview;
static node_t *g_shown;
static int g_tab;               /* 0 General, 1 Details */

/* ---- reading a Windows log ------------------------------------------------------------- */

static void free_events(logview_t *lv)
{
    int i, j;
    for (i = 0; i < lv->n; i++)
    {
        free(lv->ev[i].message);
        for (j = 0; j < lv->ev[i].nstrings; j++) free(lv->ev[i].strings[j]);
        free(lv->ev[i].strings);
        free(lv->ev[i].data);
    }
    free(lv->ev);
    lv->ev = NULL;
    lv->n = 0;
}

static int level_of(WORD type)
{
    switch (type)
    {
    case EVENTLOG_ERROR_TYPE: return LV_ERROR;
    case EVENTLOG_WARNING_TYPE: return LV_WARNING;
    case EVENTLOG_AUDIT_SUCCESS: return LV_AUDIT_OK;
    case EVENTLOG_AUDIT_FAILURE: return LV_AUDIT_FAIL;
    }
    return LV_INFO;
}

static const WCHAR *level_text(int l)
{
    switch (l)
    {
    case LV_CRITICAL: return L"Critical";
    case LV_ERROR: return L"Error";
    case LV_WARNING: return L"Warning";
    case LV_VERBOSE: return L"Verbose";
    case LV_AUDIT_OK: return L"Audit Success";
    case LV_AUDIT_FAIL: return L"Audit Failure";
    }
    return L"Information";
}

static int level_icon(int l)
{
    switch (l)
    {
    case LV_CRITICAL: case LV_ERROR: return IC_ERROR;
    case LV_WARNING: return IC_WARNING;
    case LV_AUDIT_OK: return IC_AUDIT_OK;
    case LV_AUDIT_FAIL: return IC_AUDIT_FAIL;
    }
    return IC_INFO;
}

static void sid_name(PSID sid, WCHAR *out, int cch)
{
    WCHAR name[128], dom[128], *s;
    DWORD nn = 128, dn = 128;
    SID_NAME_USE use;
    if (LookupAccountSidW(NULL, sid, name, &nn, dom, &dn, &use))
    {
        if (dom[0]) _snwprintf(out, cch, L"%ls\\%ls", dom, name);
        else lstrcpynW(out, name, cch);
    }
    else if (ConvertSidToStringSidW(sid, &s))
    {
        lstrcpynW(out, s, cch);
        LocalFree(s);
    }
    else lstrcpynW(out, L"N/A", cch);
    out[cch - 1] = 0;
}

static DWORD read_log(logview_t *lv)
{
    HANDLE h;
    BYTE *buf;
    DWORD size = 0x10000, got, need, err = 0;

    free_events(lv);
    lv->err = 0;
    h = lv->backup[0] ? OpenBackupEventLogW(NULL, lv->backup) : OpenEventLogW(NULL, lv->name);
    if (!h) return lv->err = GetLastError();
    GetNumberOfEventLogRecords(h, &lv->count);
    GetOldestEventLogRecord(h, &lv->oldest);
    lv->ev = calloc(lv->count + 16 < MAX_EVENTS ? lv->count + 16 : MAX_EVENTS, sizeof(evt_t));
    buf = malloc(size);
    while (lv->ev && buf && lv->n < MAX_EVENTS && (int)lv->n < (int)lv->count + 16)
    {
        BYTE *p;
        if (!ReadEventLogW(h, EVENTLOG_SEQUENTIAL_READ | EVENTLOG_BACKWARDS_READ, 0, buf, size, &got, &need))
        {
            err = GetLastError();
            if (err == ERROR_INSUFFICIENT_BUFFER && need > size)
            {
                BYTE *nb = realloc(buf, need);
                if (!nb) break;
                buf = nb;
                size = need;
                err = 0;
                continue;
            }
            if (err == ERROR_HANDLE_EOF) err = 0;
            break;
        }
        for (p = buf; p < buf + got && lv->n < MAX_EVENTS && (int)lv->n < (int)lv->count + 16; )
        {
            EVENTLOGRECORD *r = (EVENTLOGRECORD *)p;
            evt_t *e = &lv->ev[lv->n++];
            const WCHAR *src = (const WCHAR *)(r + 1), *str;
            int i;
            if (r->Length < sizeof(*r)) { p = buf + got; break; }
            e->record = r->RecordNumber;
            e->time = r->TimeGenerated;
            e->level = level_of(r->EventType);
            e->id = r->EventID;
            e->category = r->EventCategory;
            lstrcpynW(e->source, src, ARRAY_SIZE(e->source));
            lstrcpynW(e->computer, src + wcslen(src) + 1, ARRAY_SIZE(e->computer));
            if (r->UserSidLength) sid_name((PSID)(p + r->UserSidOffset), e->user, ARRAY_SIZE(e->user));
            else lstrcpyW(e->user, L"N/A");
            e->nstrings = r->NumStrings;
            e->strings = calloc(r->NumStrings ? r->NumStrings : 1, sizeof(WCHAR *));
            str = (const WCHAR *)(p + r->StringOffset);
            for (i = 0; i < r->NumStrings && e->strings; i++)
            {
                e->strings[i] = _wcsdup(str);
                str += wcslen(str) + 1;
            }
            if (r->DataLength && (e->data = malloc(r->DataLength)))
            {
                memcpy(e->data, p + r->DataOffset, r->DataLength);
                e->ndata = r->DataLength;
            }
            p += r->Length;
        }
    }
    free(buf);
    CloseEventLog(h);
    return lv->err = err;
}

/* ---- messages ---------------------------------------------------------------------------- */

static BOOL message_from(const WCHAR *files, DWORD id, WCHAR **strings, int nstrings, WCHAR **out)
{
    WCHAR list[1024], *f, *next;
    DWORD_PTR args[100];
    int i;
    for (i = 0; i < 100; i++) args[i] = (DWORD_PTR)(i < nstrings && strings[i] ? strings[i] : L"");
    ExpandEnvironmentStringsW(files, list, ARRAY_SIZE(list));
    for (f = list; f && *f; f = next)
    {
        HMODULE mod;
        WCHAR *text = NULL;
        if ((next = wcschr(f, ';'))) *next++ = 0;
        while (*f == ' ') f++;
        if (!*f) continue;
        if (!(mod = LoadLibraryExW(f, NULL, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE))) continue;
        if (FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_ARGUMENT_ARRAY | FORMAT_MESSAGE_ALLOCATE_BUFFER,
                           mod, id, 0, (WCHAR *)&text, 0, (va_list *)args) && text)
        {
            size_t n = wcslen(text);
            while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) text[--n] = 0;
            *out = _wcsdup(text);
            LocalFree(text);
            FreeLibrary(mod);
            return TRUE;
        }
        FreeLibrary(mod);
    }
    return FALSE;
}

static void source_value(const logview_t *lv, const WCHAR *source, const WCHAR *value, WCHAR *out, DWORD cch)
{
    WCHAR key[512];
    DWORD size = cch * sizeof(WCHAR);
    out[0] = 0;
    _snwprintf(key, ARRAY_SIZE(key), EVT_KEY L"\\%ls\\%ls", lv->name, source);
    key[ARRAY_SIZE(key) - 1] = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, NULL, out, &size))
        out[0] = 0;
}

static const WCHAR *event_message(logview_t *lv, evt_t *e)
{
    WCHAR files[1024], *m = NULL;
    size_t len, i;
    if (e->message) return e->message;
#ifdef SG_MUTANT_NOFORMAT
    if (0)
#else
    if (!lv->journal)
#endif
    {
        source_value(lv, e->source, L"EventMessageFile", files, ARRAY_SIZE(files));
        if (files[0] && message_from(files, e->id, e->strings, e->nstrings, &m)) return e->message = m;
    }
    /* Windows' words when the source's messages are not here */
    len = 512;
    for (i = 0; i < (size_t)e->nstrings; i++) len += wcslen(e->strings[i]) + 2;
    if (!(m = malloc(len * sizeof(WCHAR)))) return L"";
    _snwprintf(m, len, L"The description for Event ID %lu from source %ls cannot be found. Either the component "
                       L"that raises this event is not installed on your local computer or the installation is "
                       L"corrupted. You can install or repair the component on the local computer.\r\n\r\n"
                       L"The following information was included with the event:%ls",
               e->id & 0xFFFF, e->source, e->nstrings ? L"\r\n\r\n" : L" (none)");
    for (i = 0; i < (size_t)e->nstrings; i++)
    {
        wcscat(m, e->strings[i]);
        if (i + 1 < (size_t)e->nstrings) wcscat(m, L"\r\n");
    }
    return e->message = m;
}

static void category_text(logview_t *lv, evt_t *e, WCHAR *out, int cch)
{
    WCHAR files[1024], *m = NULL;
    if (lv->journal) { lstrcpynW(out, e->unit, cch); return; }
    if (!e->category) { lstrcpynW(out, L"None", cch); return; }
    source_value(lv, e->source, L"CategoryMessageFile", files, ARRAY_SIZE(files));
    if (files[0] && message_from(files, e->category, NULL, 0, &m))
    {
        lstrcpynW(out, m, cch);
        free(m);
        return;
    }
    _snwprintf(out, cch, L"(%u)", e->category);
    out[cch - 1] = 0;
}

/* ---- the Stained Glass log (the journal) -------------------------------------------------- */

static void unescape(const char *s, WCHAR *out, int cch)
{
    char *u = malloc(strlen(s) + 1), *d = u;
    if (!u) { out[0] = 0; return; }
    for (; *s; s++)
    {
        if (*s == '\\' && s[1])
        {
            s++;
            *d++ = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : *s;
        }
        else *d++ = *s;
    }
    *d = 0;
    utf8_to_w(u, out, cch);
    free(u);
}

static DWORD read_journal(logview_t *lv)
{
    sys_reply_t r;
    int i;
    free_events(lv);
    lv->err = 0;
    sys_request(&r, "journal", "--lines", "2000", NULL);
    if (!r.ok)
    {
        sys_free(&r);
        return lv->err = !strcmp(r.kind, "denied") ? ERROR_ACCESS_DENIED : ERROR_NOT_SUPPORTED;
    }
    lv->ev = calloc(r.nlines ? r.nlines : 1, sizeof(evt_t));
    for (i = 0; lv->ev && i < r.nlines; i++)
    {
        char *f[8], *line = r.lines[i], *p;
        int k = 0;
        evt_t *e;
        WCHAR msg[8192];
        if (strncmp(line, "E\t", 2)) continue;
        for (p = line; k < 8; )
        {
            f[k++] = p;
            if (!(p = strchr(p, '\t'))) break;
            *p++ = 0;
        }
        if (k < 8) continue;
        e = &lv->ev[lv->n++];
        e->record = lv->n;
        e->time = strtoull(f[1], NULL, 10) / 1000000ULL;
        switch (atoi(f[2]))
        {
        case 0: case 1: case 2: e->level = LV_CRITICAL; break;
        case 3: e->level = LV_ERROR; break;
        case 4: e->level = LV_WARNING; break;
        case 7: e->level = LV_VERBOSE; break;
        default: e->level = LV_INFO; break;
        }
        unescape(f[3], e->unit, ARRAY_SIZE(e->unit));
        unescape(f[4], e->source, ARRAY_SIZE(e->source));
        e->pid = strtoul(f[5], NULL, 10);
        unescape(f[7], msg, ARRAY_SIZE(msg));
        e->message = _wcsdup(msg);
        lstrcpyW(e->user, L"N/A");
        GetComputerNameW(e->computer, &(DWORD){ ARRAY_SIZE(e->computer) });
    }
    lv->count = lv->n;
    sys_free(&r);
    return 0;
}

/* ---- the filter ------------------------------------------------------------------------------ */

static BOOL in_list(const WCHAR *list, const WCHAR *word)
{
    WCHAR buf[256], *t, *ctx;
    if (!list[0]) return TRUE;
    lstrcpynW(buf, list, ARRAY_SIZE(buf));
    for (t = wcstok_s(buf, L",;", &ctx); t; t = wcstok_s(NULL, L",;", &ctx))
    {
        while (*t == ' ') t++;
        while (*t && t[wcslen(t) - 1] == ' ') t[wcslen(t) - 1] = 0;
        if (!_wcsicmp(t, word)) return TRUE;
    }
    return FALSE;
}

/* "1,3,5-99,-76": numbers and ranges, a leading minus excludes */
static BOOL id_matches(const WCHAR *spec, DWORD id)
{
    WCHAR buf[256], *t, *ctx;
    BOOL any_include = FALSE, inc = FALSE;
    if (!spec[0]) return TRUE;
    lstrcpynW(buf, spec, ARRAY_SIZE(buf));
    for (t = wcstok_s(buf, L", ", &ctx); t; t = wcstok_s(NULL, L", ", &ctx))
    {
        BOOL ex = *t == '-';
        WCHAR *dash;
        DWORD a, b;
        if (ex) t++;
        a = wcstoul(t, &dash, 10);
        b = *dash == '-' ? wcstoul(dash + 1, NULL, 10) : a;
        if (ex) { if (id >= a && id <= b) return FALSE; }
        else { any_include = TRUE; if (id >= a && id <= b) inc = TRUE; }
    }
    return !any_include || inc;
}

static BOOL passes(const logview_t *lv, const evt_t *e)
{
    BOOL any = FALSE;
    int i;
    if (!lv->filtered) return TRUE;
    for (i = 1; i < 8; i++) any |= lv->levels[i];
    if (any && !lv->levels[e->level]) return FALSE;
    if (lv->hours)
    {
        FILETIME ft;
        ULONGLONG now;
        GetSystemTimeAsFileTime(&ft);
        now = (((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) - 116444736000000000ULL) / 10000000ULL;
        if (e->time + (ULONGLONG)lv->hours * 3600 < now) return FALSE;
    }
    if (!in_list(lv->sources, e->source)) return FALSE;
    if (!id_matches(lv->ids, lv->journal ? e->pid : (e->id & 0xFFFF))) return FALSE;
    return TRUE;
}

/* ---- the view ------------------------------------------------------------------------------ */

static void fill_list(node_t *n)
{
    logview_t *lv = n->data;
    int i, shown = 0;
    WCHAR head[512];
    pane_begin();
    for (i = 0; i < lv->n; i++)
    {
        evt_t *e = &lv->ev[i];
        WCHAR when[64], id[32], cat[128];
        const WCHAR *cells[5];
        if (!passes(lv, e)) continue;
        fmt_time(e->time, when, 64);
        if (lv->journal) _snwprintf(id, 32, L"%lu", e->pid);
        else _snwprintf(id, 32, L"%lu", e->id & 0xFFFF);
        category_text(lv, e, cat, ARRAY_SIZE(cat));
        cells[0] = level_text(e->level);
        cells[1] = when;
        cells[2] = e->source;
        cells[3] = id;
        cells[4] = cat;
        pane_add(e->record, level_icon(e->level), cells);
        shown++;
    }
    pane_end();
    if (lv->err)
    {
        WCHAR why[256], msg[512];
        error_text(lv->err, why, 256);
        if (lv->err == ERROR_ACCESS_DENIED)
            _snwprintf(msg, ARRAY_SIZE(msg), L"You do not have permission to read the %ls log. %ls", lv->name, why);
        else _snwprintf(msg, ARRAY_SIZE(msg), L"The %ls log could not be read. %ls", lv->name, why);
        msg[ARRAY_SIZE(msg) - 1] = 0;
        pane_empty_text(msg);
        frame_status(L"%ls", msg);
        if (lv->err == ERROR_ACCESS_DENIED && !is_admin())
            frame_banner(L"Only an administrator can read this log. Run Event Viewer as an administrator.");
    }
    else pane_empty_text(L"There are no items to show in this view.");
    if (lv->filtered)
        _snwprintf(head, ARRAY_SIZE(head), L"%ls    Number of events: %d    Filtered: showing %d of %d",
                   lv->name, lv->n, shown, lv->n);
    else _snwprintf(head, ARRAY_SIZE(head), L"%ls    Number of events: %d", lv->name, lv->n);
    head[ARRAY_SIZE(head) - 1] = 0;
    SetWindowTextW(g_header, head);
    if (!lv->err) frame_status(L"%ls: %d events%ls", lv->name, lv->n, lv->filtered ? L" (filtered)" : L"");
}

static evt_t *find_event(logview_t *lv, LPARAM key)
{
    int i;
    for (i = 0; i < lv->n; i++) if (lv->ev[i].record == (DWORD)key) return &lv->ev[i];
    return NULL;
}

static void event_text(logview_t *lv, evt_t *e, BOOL details, WCHAR *out, size_t cch)
{
    WCHAR when[64], cat[128];
    size_t n;
    int i;
    fmt_time(e->time, when, 64);
    category_text(lv, e, cat, ARRAY_SIZE(cat));
    if (!details)
    {
        if (lv->journal)
            _snwprintf(out, cch, L"%ls\r\n\r\nLog Name:\t%ls\r\nSource:\t%ls\r\nUnit:\t%ls\r\nLogged:\t%ls\r\n"
                                 L"Level:\t%ls\r\nProcess ID:\t%lu\r\nComputer:\t%ls",
                       event_message(lv, e), lv->name, e->source, e->unit, when, level_text(e->level), e->pid, e->computer);
        else
            _snwprintf(out, cch, L"%ls\r\n\r\nLog Name:\t%ls\r\nSource:\t%ls\r\nLogged:\t%ls\r\nEvent ID:\t%lu\r\n"
                                 L"Task Category:\t%ls\r\nLevel:\t%ls\r\nKeywords:\t%ls\r\nUser:\t%ls\r\nComputer:\t%ls",
                       event_message(lv, e), lv->name, e->source, when, e->id & 0xFFFF, cat, level_text(e->level),
                       e->level == LV_AUDIT_OK ? L"Audit Success" : e->level == LV_AUDIT_FAIL ? L"Audit Failure" : L"Classic",
                       e->user, e->computer);
        out[cch - 1] = 0;
        return;
    }
    _snwprintf(out, cch, L"System\r\n  Provider [Name]: %ls\r\n  EventID: %lu\r\n  Qualifiers: %lu\r\n  Level: %ls\r\n"
                         L"  Task: %u\r\n  TimeCreated: %ls\r\n  EventRecordID: %lu\r\n  Channel: %ls\r\n  Computer: %ls\r\n"
                         L"  Security [UserID]: %ls\r\n\r\nEventData\r\n",
               e->source, e->id & 0xFFFF, e->id >> 16, level_text(e->level), e->category, when, e->record, lv->name,
               e->computer, e->user);
    out[cch - 1] = 0;
    for (i = 0; i < e->nstrings; i++)
    {
        n = wcslen(out);
        _snwprintf(out + n, cch - n, L"  %ls\r\n", e->strings[i]);
    }
    if (e->ndata)
    {
        DWORD k;
        n = wcslen(out);
        _snwprintf(out + n, cch - n, L"\r\nBinary data:\r\n  ");
        for (k = 0; k < e->ndata && wcslen(out) + 8 < cch; k++)
        {
            n = wcslen(out);
            _snwprintf(out + n, cch - n, L"%02X%ls", e->data[k], (k % 16) == 15 ? L"\r\n  " : L" ");
        }
    }
    out[cch - 1] = 0;
}

static void show_preview(node_t *n, LPARAM key, BOOL have)
{
    logview_t *lv = n->data;
    evt_t *e;
    static WCHAR text[32768];
    if (!have || !(e = find_event(lv, key))) { SetWindowTextW(g_preview, L""); return; }
    event_text(lv, e, g_tab == 1, text, ARRAY_SIZE(text));
    SetWindowTextW(g_preview, text);
}

static LRESULT CALLBACK tabs_sub(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref)
{
    (void)id; (void)ref;
    if (msg == WM_CTLCOLORSTATIC && (HWND)lp == g_preview)
    {
        SetBkColor((HDC)wp, C_BG);
        SetTextColor((HDC)wp, C_TEXT);
        SetDCBrushColor((HDC)wp, C_BG);
        return (LRESULT)GetStockObject(DC_BRUSH);
    }
    if (msg == WM_NOTIFY && ((NMHDR *)lp)->code == TCN_SELCHANGE)
    {
        LPARAM key;
        g_tab = TabCtrl_GetCurSel(g_tabs);
        if (g_shown) show_preview(g_shown, pane_selected(&key) ? key : 0, pane_selected(&key));
        return 0;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

static void ensure_windows(void)
{
    TCITEMW ti = { TCIF_TEXT };
    if (g_header) return;
    g_header = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX,
                               0, 0, 0, 0, g_main, NULL, g_inst, NULL);
    g_tabs = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, g_main, NULL, g_inst, NULL);
    ti.pszText = (WCHAR *)L"General";
    SendMessageW(g_tabs, TCM_INSERTITEMW, 0, (LPARAM)&ti);
    ti.pszText = (WCHAR *)L"Details";
    SendMessageW(g_tabs, TCM_INSERTITEMW, 1, (LPARAM)&ti);
    g_preview = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL, 0, 0, 0, 0, g_tabs, NULL, g_inst, NULL);
    SendMessageW(g_header, WM_SETFONT, (WPARAM)g_font_bold, 0);
    SendMessageW(g_tabs, WM_SETFONT, (WPARAM)g_font, 0);
    SendMessageW(g_preview, WM_SETFONT, (WPARAM)g_font, 0);
    {
        int stops[1] = { 60 };
        SendMessageW(g_preview, EM_SETTABSTOPS, 1, (LPARAM)stops);
    }
    SetWindowSubclass(g_tabs, tabs_sub, 1, 0);
}

static void log_layout(node_t *n, const RECT *rc)
{
    RECT list = *rc, t;
    int hh = S(26), ph = (rc->bottom - rc->top) * 42 / 100;
    (void)n;
    ensure_windows();
    MoveWindow(g_header, rc->left + S(8), rc->top, rc->right - rc->left - S(8), hh, TRUE);
    list.top += hh;
    list.bottom -= ph;
    pane_set_list_rect(&list);
    MoveWindow(g_tabs, rc->left + S(2), list.bottom + S(4), rc->right - rc->left - S(4), ph - S(6), TRUE);
    GetClientRect(g_tabs, &t);
    TabCtrl_AdjustRect(g_tabs, FALSE, &t);
    MoveWindow(g_preview, t.left, t.top, t.right - t.left, t.bottom - t.top, TRUE);
}

static void log_show(node_t *n)
{
    static const WCHAR *const cols[] = { L"Level", L"Date and Time", L"Source", L"Event ID", L"Task Category" };
    static const WCHAR *const jcols[] = { L"Level", L"Date and Time", L"Source", L"Process ID", L"Unit" };
    static const int widths[] = { 110, 170, 190, 80, 170 };
    logview_t *lv = n->data;
    ensure_windows();
    g_shown = n;
    ShowWindow(g_header, SW_SHOW);
    ShowWindow(g_tabs, SW_SHOW);
    ShowWindow(g_preview, SW_SHOW);
    pane_columns(lv->journal ? jcols : cols, widths, 5);
    pane_numeric(3);
    pane_sort(-1, FALSE);       /* newest first, as read */
    if (lv->journal) read_journal(lv);
    else read_log(lv);
    fill_list(n);
    SetWindowTextW(g_preview, L"");
}

static void log_hide(node_t *n)
{
    (void)n;
    g_shown = NULL;
    if (!g_header) return;
    ShowWindow(g_header, SW_HIDE);
    ShowWindow(g_tabs, SW_HIDE);
    ShowWindow(g_preview, SW_HIDE);
}

static void log_selchange(node_t *n, LPARAM key, BOOL have)
{
    show_preview(n, key, have);
}

/* new events: look once a second, reload when the log changed (Windows would
 * offer "new events available"; re-reading is cheap) */
static void log_tick(node_t *n)
{
    logview_t *lv = n->data;
    HANDLE h;
    DWORD count = 0, oldest = 0;
    static int t;
    if (lv->journal) { if (++t % 5 == 0) { read_journal(lv); fill_list(n); } return; }
    if (lv->backup[0] || lv->err) return;
    if (!(h = OpenEventLogW(NULL, lv->name))) return;
    GetNumberOfEventLogRecords(h, &count);
    GetOldestEventLogRecord(h, &oldest);
    CloseEventLog(h);
    if (count != lv->count || oldest != lv->oldest)
    {
        read_log(lv);
        fill_list(n);
    }
}

/* ---- dialogs -------------------------------------------------------------------------------- */

enum { F_CRIT = 1500, F_WARN, F_VERB, F_ERR, F_INFO, F_AOK, F_AFAIL, F_TIME, F_SOURCES, F_IDS, F_CLEAR };
static const struct { const WCHAR *name; int hours; } RANGES[] = {
    { L"Any time", 0 }, { L"Last hour", 1 }, { L"Last 12 hours", 12 }, { L"Last 24 hours", 24 },
    { L"Last 7 days", 168 }, { L"Last 30 days", 720 } };

static INT_PTR CALLBACK filter_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    logview_t *lv = (logview_t *)GetWindowLongPtrW(dlg, DWLP_USER);
    int i;
    switch (msg)
    {
    case WM_INITDIALOG:
        lv = (logview_t *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        for (i = 0; i < (int)ARRAY_SIZE(RANGES); i++)
        {
            SendDlgItemMessageW(dlg, F_TIME, CB_ADDSTRING, 0, (LPARAM)RANGES[i].name);
            if (RANGES[i].hours == lv->hours) SendDlgItemMessageW(dlg, F_TIME, CB_SETCURSEL, i, 0);
        }
        CheckDlgButton(dlg, F_CRIT, lv->levels[LV_CRITICAL]);
        CheckDlgButton(dlg, F_WARN, lv->levels[LV_WARNING]);
        CheckDlgButton(dlg, F_VERB, lv->levels[LV_VERBOSE]);
        CheckDlgButton(dlg, F_ERR, lv->levels[LV_ERROR]);
        CheckDlgButton(dlg, F_INFO, lv->levels[LV_INFO]);
        CheckDlgButton(dlg, F_AOK, lv->levels[LV_AUDIT_OK]);
        CheckDlgButton(dlg, F_AFAIL, lv->levels[LV_AUDIT_FAIL]);
        SetDlgItemTextW(dlg, F_SOURCES, lv->sources);
        SetDlgItemTextW(dlg, F_IDS, lv->ids);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK)
        {
            lv->levels[LV_CRITICAL] = IsDlgButtonChecked(dlg, F_CRIT);
            lv->levels[LV_WARNING] = IsDlgButtonChecked(dlg, F_WARN);
            lv->levels[LV_VERBOSE] = IsDlgButtonChecked(dlg, F_VERB);
            lv->levels[LV_ERROR] = IsDlgButtonChecked(dlg, F_ERR);
            lv->levels[LV_INFO] = IsDlgButtonChecked(dlg, F_INFO);
            lv->levels[LV_AUDIT_OK] = IsDlgButtonChecked(dlg, F_AOK);
            lv->levels[LV_AUDIT_FAIL] = IsDlgButtonChecked(dlg, F_AFAIL);
            i = (int)SendDlgItemMessageW(dlg, F_TIME, CB_GETCURSEL, 0, 0);
            lv->hours = i >= 0 && i < (int)ARRAY_SIZE(RANGES) ? RANGES[i].hours : 0;
            GetDlgItemTextW(dlg, F_SOURCES, lv->sources, ARRAY_SIZE(lv->sources));
            GetDlgItemTextW(dlg, F_IDS, lv->ids, ARRAY_SIZE(lv->ids));
            lv->filtered = TRUE;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == F_CLEAR)
        {
            for (i = F_CRIT; i <= F_AFAIL; i++) CheckDlgButton(dlg, i, BST_UNCHECKED);
            SendDlgItemMessageW(dlg, F_TIME, CB_SETCURSEL, 0, 0);
            SetDlgItemTextW(dlg, F_SOURCES, L"");
            SetDlgItemTextW(dlg, F_IDS, L"");
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

static void filter_dialog(node_t *n)
{
    static dlgt_t d;
    logview_t *lv = n->data;
    dlg_begin(&d, L"Filter Current Log", 0, 290, 196);
    dlg_item(&d, NULL, ATOM_STATIC, L"&Logged:", 0xFFFF, SS_LEFT, 8, 10, 60, 8);
    dlg_item(&d, NULL, ATOM_COMBO, L"", F_TIME, CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 80, 8, 200, 90);
    D_LABEL(&d, L"Event level:", 8, 32, 60);
    dlg_item(&d, NULL, ATOM_BUTTON, L"C&ritical", F_CRIT, BS_AUTOCHECKBOX | WS_TABSTOP, 80, 32, 64, 10);
    dlg_item(&d, NULL, ATOM_BUTTON, L"&Warning", F_WARN, BS_AUTOCHECKBOX | WS_TABSTOP, 148, 32, 64, 10);
    dlg_item(&d, NULL, ATOM_BUTTON, L"&Verbose", F_VERB, BS_AUTOCHECKBOX | WS_TABSTOP, 216, 32, 64, 10);
    dlg_item(&d, NULL, ATOM_BUTTON, L"&Error", F_ERR, BS_AUTOCHECKBOX | WS_TABSTOP, 80, 46, 64, 10);
    dlg_item(&d, NULL, ATOM_BUTTON, L"&Information", F_INFO, BS_AUTOCHECKBOX | WS_TABSTOP, 148, 46, 64, 10);
    if (!_wcsicmp(lv->name, L"Security"))
    {
        dlg_item(&d, NULL, ATOM_BUTTON, L"Audit &Success", F_AOK, BS_AUTOCHECKBOX | WS_TABSTOP, 80, 60, 64, 10);
        dlg_item(&d, NULL, ATOM_BUTTON, L"Audit &Failure", F_AFAIL, BS_AUTOCHECKBOX | WS_TABSTOP, 148, 60, 64, 10);
    }
    dlg_item(&d, NULL, ATOM_STATIC, L"Event s&ources:", 0xFFFF, SS_LEFT, 8, 82, 70, 8);
    dlg_item(&d, NULL, ATOM_EDIT, L"", F_SOURCES, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 80, 80, 200, 13);
    D_LABEL(&d, L"Separate sources with commas.", 80, 96, 200);
    dlg_item(&d, NULL, ATOM_STATIC, lv->journal ? L"Process I&Ds:" : L"Event I&Ds:", 0xFFFF, SS_LEFT, 8, 114, 70, 8);
    dlg_item(&d, NULL, ATOM_EDIT, L"", F_IDS, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 80, 112, 200, 13);
    dlg_item(&d, NULL, ATOM_STATIC, L"Numbers and ranges separated by commas; a minus sign first excludes: 1,3,5-99,-76",
             0xFFFF, SS_LEFT | SS_NOPREFIX, 80, 128, 200, 18);
    D_BUTTON(&d, L"Clear", F_CLEAR, 8, 172, 50);
    dlg_item(&d, NULL, ATOM_BUTTON, L"OK", IDOK, BS_DEFPUSHBUTTON | WS_TABSTOP, 176, 172, 50, 14);
    D_BUTTON(&d, L"Cancel", IDCANCEL, 232, 172, 50);
    if (DialogBoxIndirectParamW(g_inst, d.t, g_main, filter_proc, (LPARAM)lv) == IDOK) fill_list(n);
    frame_update_verbs();
}

enum { E_TEXT = 1600, E_PREV, E_NEXT, E_COPY };
typedef struct evdlg { node_t *n; LPARAM key; } evdlg_t;

static void evdlg_fill(HWND dlg, evdlg_t *d)
{
    static WCHAR text[32768];
    logview_t *lv = d->n->data;
    evt_t *e = find_event(lv, d->key);
    WCHAR title[128];
    if (!e) return;
    event_text(lv, e, FALSE, text, ARRAY_SIZE(text));
    SetDlgItemTextW(dlg, E_TEXT, text);
    _snwprintf(title, ARRAY_SIZE(title), L"Event %lu, %ls", lv->journal ? e->pid : (e->id & 0xFFFF), e->source);
    title[ARRAY_SIZE(title) - 1] = 0;
    SetWindowTextW(dlg, title);
}

static void copy_text(HWND owner, const WCHAR *text)
{
    size_t n = (wcslen(text) + 1) * sizeof(WCHAR);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n);
    if (!g) return;
    memcpy(GlobalLock(g), text, n);
    GlobalUnlock(g);
    if (OpenClipboard(owner))
    {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, g);
        CloseClipboard();
    }
    else GlobalFree(g);
}

static INT_PTR CALLBACK event_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    evdlg_t *d = (evdlg_t *)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg)
    {
    case WM_INITDIALOG:
    {
        int stops[1] = { 60 };
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SendDlgItemMessageW(dlg, E_TEXT, EM_SETTABSTOPS, 1, (LPARAM)stops);
        evdlg_fill(dlg, (evdlg_t *)lp);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp))
        {
        case IDCANCEL: case IDOK: EndDialog(dlg, 0); return TRUE;
        case E_PREV: case E_NEXT:
        {
            HWND list = pane_list();
            int i = ListView_GetNextItem(list, -1, LVNI_SELECTED);
            i += LOWORD(wp) == E_PREV ? -1 : 1;
            if (i >= 0 && i < ListView_GetItemCount(list))
            {
                LVITEMW it = { LVIF_PARAM };
                it.iItem = i;
                SendMessageW(list, LVM_GETITEMW, 0, (LPARAM)&it);
                pane_select_key(it.lParam);
                d->key = it.lParam;
                evdlg_fill(dlg, d);
            }
            return TRUE;
        }
        case E_COPY:
        {
            static WCHAR text[32768];
            GetDlgItemTextW(dlg, E_TEXT, text, ARRAY_SIZE(text));
            copy_text(dlg, text);
            return TRUE;
        }
        }
        break;
    }
    return FALSE;
}

static void event_dialog(node_t *n, LPARAM key)
{
    static dlgt_t t;
    evdlg_t d = { n, key };
    dlg_begin(&t, L"Event Properties", DS_SHELLFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU,
              330, 240);
    dlg_item(&t, NULL, ATOM_EDIT, L"", E_TEXT, WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_TABSTOP,
             7, 7, 316, 204);
    D_BUTTON(&t, L"&Copy", E_COPY, 7, 218, 50);
    D_BUTTON(&t, L"&Previous", E_PREV, 150, 218, 54);
    D_BUTTON(&t, L"&Next", E_NEXT, 208, 218, 54);
    dlg_item(&t, NULL, ATOM_BUTTON, L"Close", IDOK, BS_DEFPUSHBUTTON | WS_TABSTOP, 272, 218, 50, 14);
    DialogBoxIndirectParamW(g_inst, t.t, g_main, event_proc, (LPARAM)&d);
}

static BOOL save_path(const WCHAR *title, const WCHAR *def, WCHAR *path)
{
    OPENFILENAMEW ofn = { sizeof(ofn) };
    lstrcpynW(path, def, MAX_PATH);
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = L"Event Files (*.evt)\0*.evt\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"evt";
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    return GetSaveFileNameW(&ofn);
}

static void clear_log(node_t *n)
{
    logview_t *lv = n->data;
    WCHAR path[MAX_PATH], why[256];
    HANDLE h;
    int r = frame_message(MB_YESNOCANCEL | MB_ICONQUESTION, L"Event Viewer",
                          L"Do you want to save \"%ls\" before clearing it?\n\nYes: Save and Clear.  No: Clear.", lv->name);
    BOOL ok;
    if (r == IDCANCEL) return;
    if (r == IDYES && !save_path(L"Save As", lv->name, path)) return;
    if (!(h = OpenEventLogW(NULL, lv->name)))
    {
        error_text(GetLastError(), why, 256);
        frame_message(MB_OK | MB_ICONERROR, L"Event Viewer", L"The %ls log could not be cleared.\n\n%ls", lv->name, why);
        return;
    }
    ok = ClearEventLogW(h, r == IDYES ? path : NULL);
    if (!ok)
    {
        DWORD err = GetLastError();
        error_text(err, why, 256);
        frame_message(MB_OK | MB_ICONERROR, L"Event Viewer", L"The %ls log could not be cleared.\n\nError %lu: %ls%ls",
                      lv->name, err, why, err == ERROR_ACCESS_DENIED && !is_admin() ?
                      L"\n\nOnly an administrator can clear logs." : L"");
    }
    CloseEventLog(h);
    read_log(lv);
    fill_list(n);
}

static void save_log(node_t *n)
{
    logview_t *lv = n->data;
    WCHAR path[MAX_PATH], why[256];
    HANDLE h;
    if (!save_path(L"Save All Events As", lv->name, path)) return;
    if (!(h = OpenEventLogW(NULL, lv->name)) || !BackupEventLogW(h, path))
    {
        DWORD err = GetLastError();
        error_text(err, why, 256);
        frame_message(MB_OK | MB_ICONERROR, L"Event Viewer", L"The events could not be saved.\n\nError %lu: %ls", err, why);
    }
    if (h) CloseEventLog(h);
}

/* ---- verbs --------------------------------------------------------------------------------- */

static void log_verbs(node_t *n, LPARAM key, BOOL have, verbs_t *out)
{
    logview_t *lv = n->data;
    (void)key;
    if (have)
    {
        out->v[out->n++] = (verb_t){ V_COPY, L"&Copy", -1, TRUE, FALSE };
        return;
    }
    out->v[out->n++] = (verb_t){ V_FILTER, L"&Filter Current Log...", IC_FILTER, TRUE, FALSE };
    out->v[out->n++] = (verb_t){ V_CLEARFILTER, L"Clear Filte&r", -1, lv->filtered, FALSE };
    if (!lv->journal && !lv->backup[0])
    {
        out->v[out->n++] = (verb_t){ V_CLEARLOG, L"&Clear Log...", IC_CLEAR, !lv->err, TRUE };
        out->v[out->n++] = (verb_t){ V_SAVEAS, L"&Save All Events As...", -1, !lv->err, FALSE };
    }
}

static void log_invoke(node_t *n, LPARAM key, BOOL have, int verb)
{
    logview_t *lv = n->data;
    evt_t *e;
    static WCHAR text[32768];
    switch (verb)
    {
    case V_FILTER: filter_dialog(n); break;
    case V_CLEARFILTER: lv->filtered = FALSE; memset(lv->levels, 0, sizeof(lv->levels)); lv->hours = 0;
        lv->sources[0] = lv->ids[0] = 0; fill_list(n); break;
    case V_CLEARLOG: clear_log(n); break;
    case V_SAVEAS: save_log(n); break;
    case V_EVENTPROPS: if (have) event_dialog(n, key); break;
    case V_COPY:
        if (have && (e = find_event(lv, key)))
        {
            event_text(lv, e, FALSE, text, ARRAY_SIZE(text));
            copy_text(g_main, text);
        }
        break;
    }
}

static void log_open(node_t *n, LPARAM key) { event_dialog(n, key); }

static void log_dump(node_t *n, FILE *f)
{
    logview_t *lv = n->data;
    LPARAM key;
    WCHAR head[512], *p;
    static WCHAR pv[32768];
    GetWindowTextW(g_header, head, ARRAY_SIZE(head));
    fprintf(f, "LOG %ls\nLOGERR %lu\nHEADER %ls\nFILTERED %d\nEVENTS %d\n", lv->name, lv->err, head, lv->filtered, lv->n);
    if (pane_selected(&key))
    {
        GetWindowTextW(g_preview, pv, ARRAY_SIZE(pv));
        for (p = pv; *p; p++) if (*p == '\r') *p = ' '; else if (*p == '\n') *p = '|';
        fprintf(f, "PREVIEW %ls\n", pv);
    }
}

static const snapin_t log_ops = {
    NULL, log_show, log_verbs, log_invoke, log_open, log_selchange, log_tick, log_layout, log_hide, log_dump
};

/* ---- the tree -------------------------------------------------------------------------------- */

static node_t *add_log(node_t *parent, const WCHAR *name, const WCHAR *title, BOOL journal)
{
    logview_t *lv = calloc(1, sizeof(*lv));
    node_t *n;
    lstrcpynW(lv->name, name, ARRAY_SIZE(lv->name));
    lv->journal = journal;
    n = node_add(parent, title ? title : name, journal ? IC_SGLOGO : IC_LOG, &log_ops, lv);
    return n;
}

static const WCHAR *const WINDOWS_LOGS[] = { L"Application", L"Security", L"Setup", L"System" };

static void services_logs_expand(node_t *n)
{
    HKEY key;
    WCHAR name[128];
    DWORD i, len;
    int j;
    if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, EVT_KEY, 0, KEY_ENUMERATE_SUB_KEYS, &key))
    {
        for (i = 0; len = ARRAY_SIZE(name), !RegEnumKeyExW(key, i, name, &len, NULL, NULL, NULL, NULL); i++)
        {
            BOOL windows = FALSE;
            for (j = 0; j < (int)ARRAY_SIZE(WINDOWS_LOGS); j++) if (!_wcsicmp(name, WINDOWS_LOGS[j])) windows = TRUE;
            /* the service's own settings, not a log */
            if (!_wcsicmp(name, L"Parameters")) continue;
            if (!windows) add_log(n, name, NULL, FALSE);
        }
        RegCloseKey(key);
    }
    add_log(n, L"Stained Glass", NULL, TRUE);
}

static void windows_logs_expand(node_t *n)
{
    HKEY key;
    int j;
    for (j = 0; j < (int)ARRAY_SIZE(WINDOWS_LOGS); j++)
    {
        WCHAR path[256];
        _snwprintf(path, ARRAY_SIZE(path), EVT_KEY L"\\%ls", WINDOWS_LOGS[j]);
        if (!RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_QUERY_VALUE, &key))
        {
            RegCloseKey(key);
            add_log(n, WINDOWS_LOGS[j], NULL, FALSE);
        }
    }
}

static const snapin_t windows_logs_ops = { windows_logs_expand };
static const snapin_t services_logs_ops = { services_logs_expand };

/* eventvwr /c:<log> (Windows' "channel" switch) opens that log */
static node_t *find_child(node_t *n, const WCHAR *title)
{
    node_t *c, *r;
    for (c = n->child; c; c = c->next)
    {
        if (!_wcsicmp(c->title, title)) return c;
        if (!c->expanded && c->ops && c->ops->expand) { c->ops->expand(c); c->expanded = TRUE; }
        if ((r = find_child(c, title))) return r;
    }
    return NULL;
}

node_t *events_create(node_t *parent)
{
    node_t *root = node_add(parent, L"Event Viewer (Local)", IC_EVENTS, NULL, NULL), *w, *a, *want;
    int argc, i;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    lstrcpyW(root->desc, L"The system's event logs and the Stained Glass system log");
    w = node_add(root, L"System Logs", IC_FOLDER, &windows_logs_ops, NULL);
    lstrcpyW(w->desc, L"Application, Security and System");
    a = node_add(root, L"Applications and Services Logs", IC_FOLDER, &services_logs_ops, NULL);
    lstrcpyW(a->desc, L"Other logs, and Stained Glass: the journal of its system services");
    windows_logs_expand(w);
    w->expanded = TRUE;
    services_logs_expand(a);
    a->expanded = TRUE;
    root->expanded = TRUE;
    for (i = 1; argv && i < argc; i++)
        if ((argv[i][0] == '/' || argv[i][0] == '-') && (argv[i][1] == 'c' || argv[i][1] == 'C') && argv[i][2] == ':')
        {
            if ((want = find_child(root, argv[i] + 3))) frame_set_initial(want);
        }
        else if ((argv[i][0] == '/' || argv[i][0] == '-') && (argv[i][1] == 'l' || argv[i][1] == 'L') && argv[i][2] == ':')
        {
            /* a saved log file */
            logview_t *lv;
            node_t *s = add_log(a, L"Saved Log", NULL, FALSE);
            lv = s->data;
            lstrcpynW(lv->backup, argv[i] + 3, MAX_PATH);
            frame_set_initial(s);
        }
    if (argv) LocalFree(argv);
    return root;
}
