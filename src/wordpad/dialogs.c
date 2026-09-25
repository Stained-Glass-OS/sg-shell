/* sg-wordpad -- the dialogs: Date and Time, Paragraph, Tabs, Find and Replace.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "wordpad.h"
#include "resource.h"

UINT g_findmsg;
HWND g_finddlg;
static FINDREPLACEW g_fr;
static WCHAR g_find[256], g_repl[256];

/* ---------------------------------------------------------------- Date and time */

static INT_PTR CALLBACK datetime_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m)
    {
    case WM_INITDIALOG:
    {
        SYSTEMTIME st;
        WCHAR buf[128];
        HWND list = GetDlgItem(h, IDC_DT_LIST);
        static const DWORD dflags[] = { DATE_SHORTDATE, DATE_LONGDATE };
        static const WCHAR *extra[] = { L"M/d/yy", L"MM/dd/yy", L"MM/dd/yyyy", L"yy/MM/dd", L"yyyy-MM-dd", L"dd-MMM-yy",
                                        L"MMMM d, yyyy", L"d MMMM yyyy", L"dddd, d MMMM yyyy", L"MMMM yyyy" };
        GetLocalTime(&st);
        for (int i = 0; i < 2; i++)
            if (GetDateFormatW(LOCALE_USER_DEFAULT, dflags[i], &st, NULL, buf, ARRAYSIZE(buf)) &&
                SendMessageW(list, LB_FINDSTRINGEXACT, -1, (LPARAM)buf) == LB_ERR)
                SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)buf);
        for (int i = 0; i < (int)ARRAYSIZE(extra); i++)
            if (GetDateFormatW(LOCALE_USER_DEFAULT, 0, &st, extra[i], buf, ARRAYSIZE(buf)) &&
                SendMessageW(list, LB_FINDSTRINGEXACT, -1, (LPARAM)buf) == LB_ERR)
                SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)buf);
        if (GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &st, NULL, buf, ARRAYSIZE(buf)))
            SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)buf);
        if (GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &st, L"HH:mm:ss", buf, ARRAYSIZE(buf)) &&
            SendMessageW(list, LB_FINDSTRINGEXACT, -1, (LPARAM)buf) == LB_ERR)
            SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)buf);
        SendMessageW(list, LB_SETCURSEL, 0, 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || (LOWORD(w) == IDC_DT_LIST && HIWORD(w) == LBN_DBLCLK))
        {
            HWND list = GetDlgItem(h, IDC_DT_LIST);
            int i = (int)SendMessageW(list, LB_GETCURSEL, 0, 0);
            if (i >= 0)
            {
                WCHAR buf[128];
                SendMessageW(list, LB_GETTEXT, i, (LPARAM)buf);
                SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)buf);
            }
            EndDialog(h, IDOK);
        }
        else if (LOWORD(w) == IDCANCEL) EndDialog(h, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void dlg_datetime(void)
{
    DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_DATETIME), g_main, datetime_proc);
}

/* ---------------------------------------------------------------- measurements */

static const WCHAR *unit_name(void)
{
    static const WCHAR *u[4] = { L"\"", L"cm", L"pt", L"pi" };
    return u[g_units & 3];
}

static void fmt_len(int twips, WCHAR *out, int cch)
{
    double v;
    switch (g_units)
    {
    case 1: v = twips / 567.0; break;
    case 2: v = twips / 20.0; break;
    case 3: v = twips / 240.0; break;
    default: v = twips / 1440.0; break;
    }
    swprintf(out, cch, L"%.2f%ls", v, unit_name());
    /* 0.50" rather than 0.500000: trim trailing zeros */
    {
        WCHAR *dot = wcschr(out, '.'), *u;
        if (dot)
        {
            u = dot;
            while (*u && (iswdigit(*u) || *u == '.')) u++;
            {
                WCHAR tail[8];
                WCHAR *e = u;
                lstrcpynW(tail, u, 8);
                while (e > dot + 1 && e[-1] == '0') e--;
                if (e == dot + 1) e = dot;
                lstrcpyW(e, tail);
            }
        }
    }
}

/* a length typed in the current units, or with its own ("2cm", "1in", "12pt") */
static BOOL parse_len(const WCHAR *s, int *twips)
{
    WCHAR *end;
    double v = wcstod(s, &end);
    int u = g_units;
    while (*end == ' ') end++;
    if (end == s) return FALSE;
    if (*end == '"' || !_wcsnicmp(end, L"in", 2)) u = 0;
    else if (!_wcsnicmp(end, L"cm", 2)) u = 1;
    else if (!_wcsnicmp(end, L"pt", 2)) u = 2;
    else if (!_wcsnicmp(end, L"pi", 2)) u = 3;
    else if (*end) return FALSE;
    *twips = (int)(v * (u == 1 ? 567.0 : u == 2 ? 20.0 : u == 3 ? 240.0 : 1440.0) + (v < 0 ? -0.5 : 0.5));
    return TRUE;
}

/* ---------------------------------------------------------------- Paragraph */

static INT_PTR CALLBACK para_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    static const WCHAR *spacing[] = { L"1.00", L"1.15", L"1.50", L"2.00" };
    static const int sp20[] = { 20, 23, 30, 40 };
    static const WCHAR *aligns[] = { L"Left", L"Right", L"Center", L"Justify" };
    static const WORD alignv[] = { PFA_LEFT, PFA_RIGHT, PFA_CENTER, PFA_JUSTIFY };
    (void)l;
    switch (m)
    {
    case WM_INITDIALOG:
    {
        PARAFORMAT2 pf;
        WCHAR t[32];
        int cur = 0;
        memset(&pf, 0, sizeof(pf));
        pf.cbSize = sizeof(pf);
        SendMessageW(g_edit, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
        fmt_len(pf.dxStartIndent + pf.dxOffset, t, 32); SetDlgItemTextW(h, IDC_PA_LEFT, t);
        fmt_len(pf.dxRightIndent, t, 32); SetDlgItemTextW(h, IDC_PA_RIGHT, t);
        fmt_len(-pf.dxOffset, t, 32); SetDlgItemTextW(h, IDC_PA_FIRST, t);
        for (int i = 0; i < 4; i++) SendDlgItemMessageW(h, IDC_PA_SPACING, CB_ADDSTRING, 0, (LPARAM)spacing[i]);
        if (pf.bLineSpacingRule == 5) for (int i = 0; i < 4; i++) if (pf.dyLineSpacing == sp20[i]) cur = i;
        if (pf.bLineSpacingRule == 1) cur = 2;
        if (pf.bLineSpacingRule == 2) cur = 3;
        SendDlgItemMessageW(h, IDC_PA_SPACING, CB_SETCURSEL, cur, 0);
        CheckDlgButton(h, IDC_PA_AFTER, pf.dySpaceAfter ? BST_CHECKED : BST_UNCHECKED);
        for (int i = 0; i < 4; i++) SendDlgItemMessageW(h, IDC_PA_ALIGN, CB_ADDSTRING, 0, (LPARAM)aligns[i]);
        for (int i = 0; i < 4; i++) if (pf.wAlignment == alignv[i]) SendDlgItemMessageW(h, IDC_PA_ALIGN, CB_SETCURSEL, i, 0);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK)
        {
            WCHAR t[32];
            int left, right, first;
            PARAFORMAT2 pf;
            GetDlgItemTextW(h, IDC_PA_LEFT, t, 32);
            if (!parse_len(t, &left)) { MessageBoxW(h, L"Enter a valid measurement.", L"WordPad", MB_OK | MB_ICONWARNING); return TRUE; }
            GetDlgItemTextW(h, IDC_PA_RIGHT, t, 32);
            if (!parse_len(t, &right)) { MessageBoxW(h, L"Enter a valid measurement.", L"WordPad", MB_OK | MB_ICONWARNING); return TRUE; }
            GetDlgItemTextW(h, IDC_PA_FIRST, t, 32);
            if (!parse_len(t, &first)) { MessageBoxW(h, L"Enter a valid measurement.", L"WordPad", MB_OK | MB_ICONWARNING); return TRUE; }
            set_indents(max(0, left), first, max(0, right));
            memset(&pf, 0, sizeof(pf));
            pf.cbSize = sizeof(pf);
            pf.dwMask = PFM_LINESPACING | PFM_SPACEAFTER | PFM_ALIGNMENT;
            pf.bLineSpacingRule = 5;
            pf.dyLineSpacing = sp20[max(0, (int)SendDlgItemMessageW(h, IDC_PA_SPACING, CB_GETCURSEL, 0, 0))];
            pf.dySpaceAfter = IsDlgButtonChecked(h, IDC_PA_AFTER) ? 200 : 0;
            pf.wAlignment = alignv[max(0, (int)SendDlgItemMessageW(h, IDC_PA_ALIGN, CB_GETCURSEL, 0, 0))];
            SendMessageW(g_edit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
            refresh_state();
            EndDialog(h, IDOK);
        }
        else if (LOWORD(w) == IDC_PA_TABS) dlg_tabs();
        else if (LOWORD(w) == IDCANCEL) EndDialog(h, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

void dlg_paragraph(void)
{
    DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_PARAGRAPH), g_main, para_proc);
}

/* ---------------------------------------------------------------- Tabs */

static LONG g_tabs[MAX_TAB_STOPS];
static int g_ntabs;

static void tabs_fill(HWND h)
{
    HWND list = GetDlgItem(h, IDC_TB_LIST);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_ntabs; i++)
    {
        WCHAR t[32];
        fmt_len(g_tabs[i], t, 32);
        SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)t);
    }
    EnableWindow(GetDlgItem(h, IDC_TB_CLEARALL), g_ntabs > 0);
}

static INT_PTR CALLBACK tabs_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m)
    {
    case WM_INITDIALOG:
    {
        PARAFORMAT2 pf;
        memset(&pf, 0, sizeof(pf));
        pf.cbSize = sizeof(pf);
        SendMessageW(g_edit, EM_GETPARAFORMAT, 0, (LPARAM)&pf);
        g_ntabs = pf.cTabCount;
        for (int i = 0; i < g_ntabs; i++) g_tabs[i] = pf.rgxTabs[i] & 0xFFFFFF;
        tabs_fill(h);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(w))
        {
        case IDC_TB_SET:
        {
            WCHAR t[32];
            int v, j;
            GetDlgItemTextW(h, IDC_TB_EDIT, t, 32);
            if (!parse_len(t, &v) || v <= 0) { MessageBoxW(h, L"Enter a valid measurement.", L"WordPad", MB_OK | MB_ICONWARNING); return TRUE; }
            for (j = 0; j < g_ntabs; j++) if (g_tabs[j] == v) return TRUE;
            if (g_ntabs >= MAX_TAB_STOPS) return TRUE;
            j = g_ntabs;
            while (j > 0 && g_tabs[j - 1] > v) { g_tabs[j] = g_tabs[j - 1]; j--; }
            g_tabs[j] = v; g_ntabs++;
            tabs_fill(h);
            SetDlgItemTextW(h, IDC_TB_EDIT, L"");
            return TRUE;
        }
        case IDC_TB_CLEAR:
        {
            int i = (int)SendDlgItemMessageW(h, IDC_TB_LIST, LB_GETCURSEL, 0, 0);
            if (i >= 0 && i < g_ntabs) { memmove(g_tabs + i, g_tabs + i + 1, (g_ntabs - i - 1) * sizeof(LONG)); g_ntabs--; tabs_fill(h); }
            return TRUE;
        }
        case IDC_TB_CLEARALL: g_ntabs = 0; tabs_fill(h); return TRUE;
        case IDOK:
        {
            PARAFORMAT2 pf;
            memset(&pf, 0, sizeof(pf));
            pf.cbSize = sizeof(pf);
            pf.dwMask = PFM_TABSTOPS;
            pf.cTabCount = (SHORT)g_ntabs;
            memcpy(pf.rgxTabs, g_tabs, g_ntabs * sizeof(LONG));
            SendMessageW(g_edit, EM_SETPARAFORMAT, 0, (LPARAM)&pf);
            refresh_state();
            EndDialog(h, IDOK);
            return TRUE;
        }
        case IDCANCEL: EndDialog(h, IDCANCEL); return TRUE;
        }
        return TRUE;
    }
    return FALSE;
}

void dlg_tabs(void)
{
    DialogBoxW(g_inst, MAKEINTRESOURCEW(IDD_TABS), GetActiveWindow() ? GetActiveWindow() : g_main, tabs_proc);
}

/* ---------------------------------------------------------------- Find and Replace */

void find_open(BOOL replace)
{
    CHARRANGE cr;
    if (g_finddlg) { DestroyWindow(g_finddlg); g_finddlg = NULL; }
    SendMessageW(g_edit, EM_EXGETSEL, 0, (LPARAM)&cr);
    if (cr.cpMax > cr.cpMin && cr.cpMax - cr.cpMin < (LONG)ARRAYSIZE(g_find))
    {
        TEXTRANGEW tr = { cr, g_find };
        SendMessageW(g_edit, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    }
    memset(&g_fr, 0, sizeof(g_fr));
    g_fr.lStructSize = sizeof(g_fr);
    g_fr.hwndOwner = g_main;
    g_fr.Flags = FR_DOWN | FR_HIDEUPDOWN * 0;
    g_fr.lpstrFindWhat = g_find;
    g_fr.wFindWhatLen = ARRAYSIZE(g_find);
    g_fr.lpstrReplaceWith = g_repl;
    g_fr.wReplaceWithLen = ARRAYSIZE(g_repl);
    g_finddlg = replace ? ReplaceTextW(&g_fr) : FindTextW(&g_fr);
    write_dump();
}

static BOOL find_from(LONG start, BOOL down, DWORD flags)
{
    FINDTEXTEXW ft;
    LONG r;
    GETTEXTLENGTHEX tl = { GTL_NUMCHARS | GTL_PRECISE, 1200 };
    LONG len = (LONG)SendMessageW(g_edit, EM_GETTEXTLENGTHEX, (WPARAM)&tl, 0);
    ft.chrg.cpMin = start;
    ft.chrg.cpMax = down ? len : 0;
    ft.lpstrText = g_find;
    r = (LONG)SendMessageW(g_edit, EM_FINDTEXTEXW, (down ? FR_DOWN : 0) | (flags & (FR_MATCHCASE | FR_WHOLEWORD)), (LPARAM)&ft);
    if (r < 0) return FALSE;
    SendMessageW(g_edit, EM_EXSETSEL, 0, (LPARAM)&ft.chrgText);
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
    return TRUE;
}

static void not_found(void)
{
    WCHAR m[320];
    swprintf(m, ARRAYSIZE(m), L"WordPad has finished searching the document.");
    MessageBoxW(g_finddlg ? g_finddlg : g_main, m, L"WordPad", MB_OK | MB_ICONINFORMATION);
}

void find_next(void)
{
    CHARRANGE cr;
    if (!g_find[0]) { find_open(FALSE); return; }
    SendMessageW(g_edit, EM_EXGETSEL, 0, (LPARAM)&cr);
    if (!find_from((g_fr.Flags & FR_DOWN) || !g_fr.lStructSize ? cr.cpMax : cr.cpMin, (g_fr.Flags & FR_DOWN) || !g_fr.lStructSize, g_fr.Flags))
        not_found();
    write_dump();
}

LRESULT on_find_msg(LPARAM lp)
{
    FINDREPLACEW *fr = (FINDREPLACEW *)lp;
    CHARRANGE cr;
    if (fr->Flags & FR_DIALOGTERM) { g_finddlg = NULL; write_dump(); return 0; }
    SendMessageW(g_edit, EM_EXGETSEL, 0, (LPARAM)&cr);
    if (fr->Flags & FR_FINDNEXT)
    {
        BOOL down = !!(fr->Flags & FR_DOWN);
        if (!find_from(down ? cr.cpMax : cr.cpMin, down, fr->Flags)) not_found();
    }
    else if (fr->Flags & FR_REPLACE)
    {
        /* replace the selection when it is the text, then find the next */
        WCHAR sel[256] = L"";
        if (cr.cpMax > cr.cpMin && cr.cpMax - cr.cpMin < 256)
        {
            TEXTRANGEW tr = { cr, sel };
            SendMessageW(g_edit, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
        }
        if ((fr->Flags & FR_MATCHCASE) ? !wcscmp(sel, g_find) : !lstrcmpiW(sel, g_find))
        {
            SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)g_repl);
            SendMessageW(g_edit, EM_EXGETSEL, 0, (LPARAM)&cr);
        }
        if (!find_from(cr.cpMax, TRUE, fr->Flags)) not_found();
    }
    else if (fr->Flags & FR_REPLACEALL)
    {
        int n = 0;
        SendMessageW(g_edit, EM_SETSEL, 0, 0);
        while (find_from(n ? (SendMessageW(g_edit, EM_EXGETSEL, 0, (LPARAM)&cr), cr.cpMax) : 0, TRUE, fr->Flags) && n < 100000)
        {
            SendMessageW(g_edit, EM_REPLACESEL, TRUE, (LPARAM)g_repl);
            n++;
        }
        if (!n) not_found();
    }
    refresh_state();
    return 0;
}
