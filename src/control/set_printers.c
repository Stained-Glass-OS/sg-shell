/* sg-control -- the printers: Settings > Devices > Printers & scanners, and
 * the Control Panel's Devices and Printers.
 *
 * A printer here is a Windows printer (winspool), as every Windows program
 * sees it: one for each printer CUPS has, printing through the printer
 * maker's Windows driver when it is installed (wine-sg 0924), else through
 * the Linux driver CUPS has for it. The same printer's CUPS queues -- the
 * one Linux programs print to with the Linux driver, and "<printer> (maker's
 * driver)" through which Linux programs use the Windows driver -- are
 * listed under it (sg-settingsctl printers), so one physical printer is one
 * entry. Its status is its queue's (wine-sg 1222: paused, offline, out of
 * paper...).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"
#include <winspool.h>

#define MAX_PRN 48
#define MAX_Q   96
struct printer {
    WCHAR name[256], title[256], driver[256], port[300], status[200], queue[256];
    DWORD bits, jobs;
    BOOL deflt, maker;          /* the default printer; the maker's Windows driver prints it */
};
struct queue { WCHAR name[256], model[256], uri[512], state[32], info[256]; };
static struct printer g_prn[MAX_PRN];
static struct queue g_q[MAX_Q];
static int g_nprn, g_nq, g_sel = -1;
static BOOL g_linux_known;      /* sg-settingsctl answered: the CUPS queues are known */

/* what a printer's status bits say, as the printer list says it */
static void status_text(DWORD bits, DWORD jobs, BOOL deflt, WCHAR *out, int cch)
{
    static const struct { DWORD bit; const WCHAR *text; } words[] = {
        { PRINTER_STATUS_OFFLINE, L"Offline" },
        { PRINTER_STATUS_PAUSED, L"Paused" },
        { PRINTER_STATUS_PAPER_OUT, L"Out of paper" },
        { PRINTER_STATUS_PAPER_JAM, L"Paper jam" },
        { PRINTER_STATUS_PAPER_PROBLEM, L"Paper problem" },
        { PRINTER_STATUS_DOOR_OPEN, L"Door open" },
        { PRINTER_STATUS_NO_TONER, L"Out of toner or ink" },
        { PRINTER_STATUS_TONER_LOW, L"Toner or ink low" },
        { PRINTER_STATUS_OUTPUT_BIN_FULL, L"Output tray full" },
        { PRINTER_STATUS_USER_INTERVENTION, L"Needs attention" },
        { PRINTER_STATUS_ERROR, L"Error" },
        { PRINTER_STATUS_PRINTING, L"Printing" },
        { PRINTER_STATUS_WAITING, L"Connecting" },
    };
    WCHAR s[200] = L"";
    size_t i;
    for (i = 0; i < ARRAYSIZE(words); i++) {
        if (!(bits & words[i].bit)) continue;
        if (s[0]) wcsncat(s, L", ", ARRAYSIZE(s) - wcslen(s) - 1);
        wcsncat(s, words[i].text, ARRAYSIZE(s) - wcslen(s) - 1);
    }
    if (!s[0]) lstrcpyW(s, L"Ready");
    if (jobs) {
        WCHAR j[48];
        _snwprintf(j, ARRAYSIZE(j), jobs == 1 ? L", 1 document waiting" : L", %lu documents waiting", jobs);
        j[ARRAYSIZE(j) - 1] = 0;
        wcsncat(s, j, ARRAYSIZE(s) - wcslen(s) - 1);
    }
    _snwprintf(out, cch, L"%ls%ls", deflt ? L"Default, " : L"", s);
    out[cch - 1] = 0;
}

/* the CUPS queues, from sg-settingsctl (absent on a machine without it: the
 * Windows printers alone are listed) */
static void load_queues(void)
{
    WCHAR err[200];
    BOOL ok = FALSE;
    char *ans, buf[1600];
    const char *pos = NULL;
    g_nq = 0;
    g_linux_known = FALSE;
    if (!(ans = ctl_run(L"printers", &ok, err, ARRAYSIZE(err), 15000))) return;
    g_linux_known = ok;
    while (ok && g_nq < MAX_Q && ctl_line(ans, "QUEUE", &pos, buf, sizeof(buf))) {
        struct queue *q = &g_q[g_nq++];
        ctl_field(buf, 0, q->name, ARRAYSIZE(q->name));
        ctl_field(buf, 1, q->model, ARRAYSIZE(q->model));
        ctl_field(buf, 2, q->uri, ARRAYSIZE(q->uri));
        ctl_field(buf, 3, q->state, ARRAYSIZE(q->state));
        ctl_field(buf, 4, q->info, ARRAYSIZE(q->info));
    }
    free(ans);
}

int printers_load(BOOL with_queues)
{
    DWORD needed = 0, n = 0, i, cch;
    PRINTER_INFO_2W *pi;
    WCHAR def[256] = L"";
    g_nprn = 0;
    cch = ARRAYSIZE(def);
    if (!GetDefaultPrinterW(def, &cch)) def[0] = 0;
    EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, NULL, 0, &needed, &n);
    if (needed && (pi = malloc(needed))) {
        if (EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, NULL, 2, (BYTE *)pi, needed, &needed, &n)) {
            for (i = 0; i < n && g_nprn < MAX_PRN; i++) {
                struct printer *p = &g_prn[g_nprn++];
                WCHAR *c;
                memset(p, 0, sizeof(*p));
                lstrcpynW(p->name, pi[i].pPrinterName ? pi[i].pPrinterName : L"", ARRAYSIZE(p->name));
                lstrcpynW(p->driver, pi[i].pDriverName ? pi[i].pDriverName : L"", ARRAYSIZE(p->driver));
                lstrcpynW(p->port, pi[i].pPortName ? pi[i].pPortName : L"", ARRAYSIZE(p->port));
                /* the name people know it by: CUPS's description ("DYMO
                 * LabelWriter 550"), else the queue's name with spaces */
                if (pi[i].pComment && pi[i].pComment[0]) lstrcpynW(p->title, pi[i].pComment, ARRAYSIZE(p->title));
                else {
                    lstrcpynW(p->title, p->name, ARRAYSIZE(p->title));
                    for (c = p->title; *c; c++) if (*c == L'_') *c = L' ';
                }
                if (!_wcsnicmp(p->port, L"CUPS:", 5)) lstrcpynW(p->queue, p->port + 5, ARRAYSIZE(p->queue));
                /* the maker's driver: our print processor for Windows drivers
                 * (0924), not the PostScript driver made from the queue's PPD */
                p->maker = pi[i].pPrintProcessor && !_wcsicmp(pi[i].pPrintProcessor, L"winprint");
#ifndef SG_MUTANT_PRINTERS_STATUS
                p->bits = pi[i].Status;
#endif
                p->jobs = pi[i].cJobs;
                p->deflt = def[0] && !lstrcmpiW(def, p->name);
                status_text(p->bits, p->jobs, p->deflt, p->status, ARRAYSIZE(p->status));
            }
        }
        free(pi);
    }
    if (with_queues) load_queues();
    return g_nprn;
}

/* the CUPS queues of printer i, for Linux programs: its own queue (the Linux
 * driver) and the one printing through its Windows driver (sgwindrv:/QUEUE) */
static void linux_queues(int i, WCHAR *out, int cch)
{
    WCHAR wd[300];
    int k;
    out[0] = 0;
#ifdef SG_MUTANT_PRINTERS_LINUX
    return;
#endif
    if (!g_prn[i].queue[0]) return;
    _snwprintf(wd, ARRAYSIZE(wd), L"sgwindrv:/%ls", g_prn[i].queue);
    wd[ARRAYSIZE(wd) - 1] = 0;
    for (k = 0; k < g_nq; k++) {
        const struct queue *q = &g_q[k];
        const WCHAR *how;
        WCHAR line[400];
        if (!lstrcmpiW(q->name, g_prn[i].queue)) how = L"the Linux driver";
        else if (!lstrcmpiW(q->uri, wd)) how = L"the maker's Windows driver";
        else continue;
        _snwprintf(line, ARRAYSIZE(line), L"%ls\x201C%ls\x201D (%ls)", out[0] ? L", " : L"",
                   q->info[0] ? q->info : q->name, how);
        line[ARRAYSIZE(line) - 1] = 0;
        wcsncat(out, line, cch - wcslen(out) - 1);
    }
}

static void driver_text(int i, WCHAR *out, int cch)
{
    if (g_prn[i].maker)
        _snwprintf(out, cch, L"%ls, the printer maker's Windows driver", g_prn[i].driver);
    else if (g_prn[i].queue[0])
        _snwprintf(out, cch, L"%ls, the Linux driver (through CUPS)", g_prn[i].driver);
    else
        _snwprintf(out, cch, L"%ls", g_prn[i].driver);
    out[cch - 1] = 0;
}

/* ---- actions both pages offer -------------------------------------------------------------- */
static const WCHAR *set_default(int i)
{
    if (i < 0 || i >= g_nprn) return L"No printer is selected.";
    if (!SetDefaultPrinterW(g_prn[i].name)) return L"The default printer could not be changed.";
    return NULL;
}

static const WCHAR *preferences(int i)
{
    HANDLE h;
    LONG size;
    DEVMODEW *dm;
    const WCHAR *why = NULL;
    if (i < 0 || i >= g_nprn) return L"No printer is selected.";
    if (!OpenPrinterW(g_prn[i].name, &h, NULL)) return L"The printer could not be opened.";
    size = DocumentPropertiesW(g_main, h, g_prn[i].name, NULL, NULL, 0);
    if (size <= 0 || !(dm = malloc(size))) why = L"The printer's driver has no preferences to show.";
    else {
        LONG r = DocumentPropertiesW(g_main, h, g_prn[i].name, dm, NULL, DM_OUT_BUFFER);
        if (r == IDOK) r = DocumentPropertiesW(g_main, h, g_prn[i].name, dm, dm, DM_IN_BUFFER | DM_IN_PROMPT | DM_OUT_BUFFER);
        if (r == IDOK) {
            PRINTER_INFO_9W pi9 = { dm };
            if (!SetPrinterW(h, 9, (BYTE *)&pi9, 0)) why = L"Your preferences could not be saved.";
        } else if (r < 0) why = L"The printer's driver has no preferences to show.";
        free(dm);
    }
    ClosePrinter(h);
    return why;
}

/* a page with the printer's name on it, through its driver: what Windows'
 * "Print a test page" proves. A label is printed along its length (a label
 * printer's page is narrow and long), the text sized to the page. */
static HFONT fit_font(HDC dc, const WCHAR *const *lines, int n, int w, int h)
{
    int size = h / (n + 1), i;
    for (; size > 4; size = size * 9 / 10) {
        HFONT f = CreateFontW(-size, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
        HFONT old = SelectObject(dc, f);
        BOOL fits = TRUE;
        SIZE sz;
        for (i = 0; i < n && fits; i++)
            if (!GetTextExtentPoint32W(dc, lines[i], lstrlenW(lines[i]), &sz) || sz.cx > w * 9 / 10) fits = FALSE;
        SelectObject(dc, old);
        if (fits && size * n * 3 / 2 <= h) return f;
        DeleteObject(f);
    }
    return CreateFontW(-4, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
}

static const WCHAR *test_page(int i)
{
    DOCINFOW di = { sizeof(di), L"Test page" };
    const WCHAR *lines[2] = { L"Stained Glass test page", NULL };
    DEVMODEW *dm = NULL;
    HANDLE h;
    HDC dc;
    HFONT f, old;
    LONG size;
    int w, hgt, ok, k;
    if (i < 0 || i >= g_nprn) return L"No printer is selected.";
    lines[1] = g_prn[i].title;
    if (OpenPrinterW(g_prn[i].name, &h, NULL)) {
        size = DocumentPropertiesW(NULL, h, g_prn[i].name, NULL, NULL, 0);
        if (size > 0 && (dm = malloc(size)) &&
            DocumentPropertiesW(NULL, h, g_prn[i].name, dm, NULL, DM_OUT_BUFFER) != IDOK) { free(dm); dm = NULL; }
        ClosePrinter(h);
    }
    if (dm && (dc = CreateICW(L"WINSPOOL", g_prn[i].name, NULL, dm))) {
        /* a long narrow page: turned, so the text runs along it */
        if (GetDeviceCaps(dc, HORZRES) * 2 < GetDeviceCaps(dc, VERTRES)) {
            dm->dmOrientation = DMORIENT_LANDSCAPE;
            dm->dmFields |= DM_ORIENTATION;
        }
        DeleteDC(dc);
    }
    dc = CreateDCW(L"WINSPOOL", g_prn[i].name, NULL, dm);
    free(dm);
    if (!dc) return L"The printer's driver could not be loaded.";
    w = GetDeviceCaps(dc, HORZRES);
    hgt = GetDeviceCaps(dc, VERTRES);
    f = fit_font(dc, lines, 2, w, hgt);
    ok = StartDocW(dc, &di) > 0 && StartPage(dc) > 0;
    if (ok) {
        TEXTMETRICW tm;
        old = SelectObject(dc, f);
        GetTextMetricsW(dc, &tm);
        for (k = 0; k < 2; k++) {
            RECT r = { 0, hgt / 2 - tm.tmHeight + k * tm.tmHeight, w, hgt / 2 + k * tm.tmHeight };
            DrawTextW(dc, lines[k], -1, &r, DT_CENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        SelectObject(dc, old);
        ok = EndPage(dc) > 0 && EndDoc(dc) > 0;
    }
    DeleteObject(f);
    DeleteDC(dc);
    return ok ? NULL : L"The test page could not be printed.";
}

/* ---- Settings > Devices > Printers & scanners ------------------------------------------------ */
enum { CMD_PRN_REFRESH = CMD_PAGE_FIRST + 1, CMD_PRN_DEFAULT, CMD_PRN_PREFS, CMD_PRN_TEST, CMD_PRN_SEL = CMD_PAGE_FIRST + 100 };

void set_build_printers(void)
{
    int y = st_title(L"Printers & scanners"), i;
    WCHAR line[600];
    printers_load(TRUE);
    if (g_sel >= g_nprn) g_sel = -1;
    y = st_head(y, L"Add printers & scanners");
    y = st_para(y, L"A printer plugged into this PC, or shared on your network, is added by itself when it is "
                   L"turned on. If the printer's maker offers a Windows driver, install it with the maker's setup "
                   L"program: the printer then prints through it, from Windows programs and Linux programs alike.");
    st_button(&y, L"Refresh", CMD_PRN_REFRESH);
    y = st_head(y, L"Printers & scanners");
    if (!g_nprn) y = st_para(y, L"No printers are installed.");
    for (i = 0; i < g_nprn; i++) {
        y = st_card(y, IC_HW, g_prn[i].title, g_prn[i].status);
        if (g_sel != i) {
            pg_link(st_x() + S(64), y - S(12), L"Select", CMD_PRN_SEL + i, 0);
            y += S(16);
            continue;
        }
        _snwprintf(line, ARRAYSIZE(line), L"%ls", g_prn[i].name);
        line[ARRAYSIZE(line) - 1] = 0;
        y = st_row(y, L"Name", line);
        driver_text(i, line, ARRAYSIZE(line));
        y = st_row(y, L"Driver", line);
        if (g_prn[i].queue[0]) {
            linux_queues(i, line, ARRAYSIZE(line));
            if (line[0]) y = st_row(y, L"Linux programs", line);
            else if (g_linux_known) y = st_row(y, L"Linux programs", L"Not shared with Linux programs");
        }
        pg_control(L"BUTTON", L"Set as default", WS_TABSTOP | BS_PUSHBUTTON | (g_prn[i].deflt ? WS_DISABLED : 0),
                   st_x(), y, S(130), S(30), CMD_PRN_DEFAULT);
        pg_control(L"BUTTON", L"Printing preferences", WS_TABSTOP | BS_PUSHBUTTON, st_x() + S(140), y, S(160), S(30), CMD_PRN_PREFS);
        pg_control(L"BUTTON", L"Print a test page", WS_TABSTOP | BS_PUSHBUTTON, st_x() + S(310), y, S(140), S(30), CMD_PRN_TEST);
        y += S(46);
    }
}

BOOL set_cmd_printers(int id, int code, HWND ctl)
{
    const WCHAR *why = NULL;
    (void)code; (void)ctl;
    if (id == CMD_PRN_REFRESH) { refresh_page(); return TRUE; }
    if (id >= CMD_PRN_SEL && id < CMD_PRN_SEL + MAX_PRN) { g_sel = id - CMD_PRN_SEL; refresh_page(); return TRUE; }
    if (id == CMD_PRN_DEFAULT) {
        if ((why = set_default(g_sel))) st_status(why);
        else refresh_page();
        return TRUE;
    }
    if (id == CMD_PRN_PREFS) { if ((why = preferences(g_sel))) st_status(why); return TRUE; }
    if (id == CMD_PRN_TEST) {
        why = test_page(g_sel);
        st_status(why ? why : L"A test page was sent to the printer.");
        return TRUE;
    }
    return FALSE;
}

/* ---- Control Panel > Hardware and Sound > Devices and Printers ------------------------------ */
enum { CMD_DP_SEL = CMD_PAGE_FIRST + 100, CMD_DP_DEFAULT = CMD_PAGE_FIRST + 1, CMD_DP_PREFS, CMD_DP_TEST, CMD_DP_SETTINGS };

void build_printers(void)
{
    static const WCHAR *const labels[] = { L"Printers & scanners settings" };
    static const int ids[] = { CMD_DP_SETTINGS };
    int x0, w = pg_width(), colw = S(150), cols, i, y, rows;
    WCHAR head[64], line[600];
    printers_load(TRUE);
    if (g_sel >= g_nprn) g_sel = -1;
    x0 = pg_left_pane(labels, ids, 1) + S(36);
    pg_title(x0, S(24), L"Devices and Printers");
    _snwprintf(head, ARRAYSIZE(head), L"Printers (%d)", g_nprn);
    head[ARRAYSIZE(head) - 1] = 0;
    pg_text(x0, S(70), S(300), S(22), g_font_head, COL_TEXT, head, DT_SINGLELINE);
    pg_rule(x0, S(96), w - x0 - S(36));
    cols = (w - x0 - S(36)) / colw;
    if (cols < 1) cols = 1;
    for (i = 0; i < g_nprn; i++) {
        int x = x0 + (i % cols) * colw, yy = S(110) + (i / cols) * S(150);
        pg_icon(x + (colw - S(48)) / 2, yy, S(48), IC_HW);
        if (g_prn[i].deflt) pg_icon(x + (colw - S(48)) / 2 + S(34), yy + S(32), S(18), IC_OK);
        pg_link(x + S(4), yy + S(56), g_prn[i].title, CMD_DP_SEL + i, i == g_sel ? LINK_BOLD : 0);
        pg_text(x + S(4), yy + S(80), colw - S(12), S(36), g_font_small, COL_SUBTLE, g_prn[i].status, DT_WORDBREAK);
    }
    rows = (g_nprn + cols - 1) / cols;
    y = S(110) + (rows ? rows : 1) * S(150);
    if (!g_nprn) pg_text(x0, S(110), w - x0 - S(36), S(22), g_font_body, COL_SUBTLE,
                         L"No printers are installed. A printer plugged into this PC is added by itself.", DT_SINGLELINE);
    if (g_sel < 0) return;
    pg_rule(x0, y - S(14), w - x0 - S(36));
    pg_text(x0, y, w - x0 - S(36), S(24), g_font_head, COL_TEXT, g_prn[g_sel].title, DT_SINGLELINE | DT_END_ELLIPSIS);
    y += S(30);
    pg_textf(x0, y, w - x0 - S(36), g_font_body, COL_TEXT, L"Status: %ls", g_prn[g_sel].status);
    y += S(22);
    pg_textf(x0, y, w - x0 - S(36), g_font_body, COL_TEXT, L"Printer name: %ls", g_prn[g_sel].name);
    y += S(22);
    driver_text(g_sel, line, ARRAYSIZE(line));
    pg_textf(x0, y, w - x0 - S(36), g_font_body, COL_TEXT, L"Driver: %ls", line);
    y += S(22);
    if (g_prn[g_sel].queue[0]) {
        linux_queues(g_sel, line, ARRAYSIZE(line));
        if (line[0]) {
            WCHAR full[700];
            _snwprintf(full, ARRAYSIZE(full), L"Linux programs: %ls", line);
            full[ARRAYSIZE(full) - 1] = 0;
            y += pg_para(x0, y, w - x0 - S(36), g_font_body, COL_TEXT, full) + S(4);
        }
    }
    y += S(8);
    if (!g_prn[g_sel].deflt) { pg_link(x0, y, L"Set as default printer", CMD_DP_DEFAULT, 0); y += S(24); }
    pg_link(x0, y, L"Printing preferences", CMD_DP_PREFS, 0); y += S(24);
    pg_link(x0, y, L"Print a test page", CMD_DP_TEST, 0);
}

BOOL cmd_printers(int id, int code, HWND ctl)
{
    const WCHAR *why = NULL;
    (void)code; (void)ctl;
    if (id >= CMD_DP_SEL && id < CMD_DP_SEL + MAX_PRN) { g_sel = id - CMD_DP_SEL; refresh_page(); return TRUE; }
    if (id == CMD_DP_DEFAULT) why = set_default(g_sel);
    else if (id == CMD_DP_PREFS) why = preferences(g_sel);
    else if (id == CMD_DP_TEST) {
        why = test_page(g_sel);
        if (!why) message(g_main, L"Print a test page", L"A test page was sent to the printer.", FALSE);
    } else if (id == CMD_DP_SETTINGS) {
        ShellExecuteW(g_main, NULL, L"ms-settings:printers", NULL, NULL, SW_SHOWNORMAL);
        return TRUE;
    } else return FALSE;
    if (why) message(g_main, L"Devices and Printers", why, TRUE);
    else refresh_page();
    return TRUE;
}

/* the printers as the pages list them, for the gates (--dump printers) */
void dump_printers(void)
{
    int i;
    WCHAR line[600], *c;
    printers_load(TRUE);
    for (i = 0; i < g_nprn; i++) {
        driver_text(i, line, ARRAYSIZE(line));
        wprintf(L"printer=%ls|%ls|%ls|%ls\n", g_prn[i].name, g_prn[i].title, g_prn[i].status, line);
        linux_queues(i, line, ARRAYSIZE(line));
        for (c = line; *c; c++) if (*c == 0x201C || *c == 0x201D) *c = L'"';   /* the console's code page */
        if (line[0]) wprintf(L"linux=%ls|%ls\n", g_prn[i].name, line);
    }
}
