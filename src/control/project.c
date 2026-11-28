/* Stained Glass OS: the Project pane (Win+P), as Windows 10's: PC screen only,
 * Duplicate, Extend, Second screen only, at the main display's right edge.
 * Windows opens it for ms-settings-displays-topology:projection (wine-sg 9068's
 * Win+P runs that); the choice goes to the compositor through sg-settingsctl
 * (display project ...), as Settings > Display's "Multiple displays" does.
 * Up/Down (or Win+P again) move the highlight, Enter or a click chooses, Esc or a
 * click elsewhere closes it.
 *
 *   sg-settings64.exe ms-settings-displays-topology:projection
 *   sg-settings64.exe /project-dump      what the pane would show (tests)
 *   sg-settings64.exe /project-choose N  choose as a click on choice N would (tests)
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include "control.h"
#include "settings.h"
#include "../sg-dpi.h"
#include "../sg-mode.h"
#include <stdio.h>

#define PJ_CLASS L"SgProjectPane"
#define PJ_NEXT (WM_APP + 1)

enum { PJ_PC, PJ_DUP, PJ_EXTEND, PJ_SECOND, PJ_COUNT };
static const WCHAR *pj_names[PJ_COUNT] = { L"PC screen only", L"Duplicate", L"Extend", L"Second screen only" };

static char pj_pc[64], pj_second[64];   /* the two displays "PC screen" and "second screen" name */
static int pj_ndisps, pj_current = -1, pj_sel;
static UINT pj_dpi = 96;
static HFONT pj_title_font, pj_font;
static COLORREF pj_accent;
static BOOL pj_dark;

static int PS(int v) { return MulDiv(v, pj_dpi, 96); }

/* the displays and the present choice, from sg-settingsctl display: PLACE name x y w h on main,
 * PROJECT extend | duplicate | only<TAB>name */
static void pj_load(void)
{
    char *ans, buf[512], names[16][64], main_name[64] = "", project[80] = "";
    const char *pos = NULL;
    BOOL ok, on[16];
    int i;

    pj_ndisps = 0;
    pj_pc[0] = pj_second[0] = 0;
    pj_current = -1;
    /* (tests: SG_SETTINGSCTL names a stand-in) */
    ans = ctl_run(L"display", &ok, NULL, 0, 6000);
    if (!ans) return;
    while (pj_ndisps < 16 && ctl_line(ans, "PLACE", &pos, buf, sizeof(buf))) {
        WCHAR f[64];
        ctl_field(buf, 0, f, ARRAYSIZE(f));
        WideCharToMultiByte(CP_UTF8, 0, f, -1, names[pj_ndisps], sizeof(names[0]), NULL, NULL);
        ctl_field(buf, 5, f, ARRAYSIZE(f)); on[pj_ndisps] = !lstrcmpW(f, L"yes");
        ctl_field(buf, 6, f, ARRAYSIZE(f));
        if (!lstrcmpW(f, L"yes")) lstrcpynA(main_name, names[pj_ndisps], sizeof(main_name));
        pj_ndisps++;
    }
    if (ctl_line(ans, "PROJECT", NULL, buf, sizeof(buf))) lstrcpynA(project, buf, sizeof(project));
    free(ans);
    if (!pj_ndisps) return;
    /* the PC's own screen: the built-in panel, else the main display, else the first */
    for (i = 0; i < pj_ndisps && !pj_pc[0]; i++)
        if (!strncmp(names[i], "eDP", 3) || !strncmp(names[i], "LVDS", 4) || !strncmp(names[i], "DSI", 3))
            lstrcpynA(pj_pc, names[i], sizeof(pj_pc));
    if (!pj_pc[0]) lstrcpynA(pj_pc, main_name[0] ? main_name : names[0], sizeof(pj_pc));
    for (i = 0; i < pj_ndisps && !pj_second[0]; i++)
        if (strcmp(names[i], pj_pc)) lstrcpynA(pj_second, names[i], sizeof(pj_second));
    if (!strncmp(project, "duplicate", 9)) pj_current = PJ_DUP;
    else if (!strncmp(project, "only\t", 5)) pj_current = !strcmp(project + 5, pj_pc) ? PJ_PC : PJ_SECOND;
    else if (pj_ndisps < 2) pj_current = PJ_PC;
    else {
        /* extend, or one of two turned off: that is "only" the other */
        int n_on = 0, last = -1;
        for (i = 0; i < pj_ndisps; i++) if (on[i]) { n_on++; last = i; }
        if (n_on == 1) pj_current = !strcmp(names[last], pj_pc) ? PJ_PC : PJ_SECOND;
        else pj_current = PJ_EXTEND;
    }
}

static BOOL pj_apply(int which)
{
    WCHAR args[160];
#ifndef SG_MUTANT_PROJECT_NO_APPLY
    WCHAR err[256];
#endif
    BOOL ok = FALSE;

    if (which == pj_current) return TRUE;
    if (pj_ndisps < 2) return FALSE;   /* one display: nothing to choose, as Windows */
    switch (which) {
    case PJ_PC:     _snwprintf(args, ARRAYSIZE(args), L"display project only %S", pj_pc); break;
    case PJ_SECOND: _snwprintf(args, ARRAYSIZE(args), L"display project only %S", pj_second); break;
    case PJ_DUP:    lstrcpyW(args, L"display project duplicate"); break;
    default:        lstrcpyW(args, L"display project extend"); break;
    }
    args[ARRAYSIZE(args) - 1] = 0;
#ifndef SG_MUTANT_PROJECT_NO_APPLY
    free(ctl_run(args, &ok, err, ARRAYSIZE(err), 15000));
#endif
    if (ok) pj_current = which;
    return ok;
}

/* the item's place in the pane: the title, then one row per choice */
static RECT pj_row(HWND hwnd, int i)
{
    RECT rc, r;
    GetClientRect(hwnd, &rc);
    SetRect(&r, PS(12), PS(72) + i * PS(76), rc.right - PS(12), PS(72) + i * PS(76) + PS(68));
    return r;
}

/* each choice's picture: one or two screens, as Windows draws them */
static void pj_icon(HDC hdc, const RECT *r, int which, COLORREF fg)
{
    HPEN pen = CreatePen(PS_SOLID, PS(2) > 1 ? PS(2) : 1, fg), oldp = SelectObject(hdc, pen);
    HBRUSH oldb = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    int cx = r->left + PS(34), cy = (r->top + r->bottom) / 2, w = PS(22), h = PS(16);

    switch (which) {
    case PJ_PC:
        Rectangle(hdc, cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
        break;
    case PJ_DUP:
        Rectangle(hdc, cx - w / 2 - PS(4), cy - h / 2 - PS(3), cx + w / 2 - PS(4), cy + h / 2 - PS(3));
        Rectangle(hdc, cx - w / 2 + PS(4), cy - h / 2 + PS(3), cx + w / 2 + PS(4), cy + h / 2 + PS(3));
        break;
    case PJ_EXTEND:
        Rectangle(hdc, cx - w, cy - h / 2, cx, cy + h / 2);
        Rectangle(hdc, cx + PS(2), cy - h / 2, cx + w + PS(2), cy + h / 2);
        break;
    default: {
        HPEN dash = CreatePen(PS_DOT, 1, fg);
        SelectObject(hdc, dash);
        Rectangle(hdc, cx - w, cy - h / 2, cx, cy + h / 2);
        SelectObject(hdc, pen);
        DeleteObject(dash);
        Rectangle(hdc, cx + PS(2), cy - h / 2, cx + w + PS(2), cy + h / 2);
        break;
    }
    }
    SelectObject(hdc, oldb);
    SelectObject(hdc, oldp);
    DeleteObject(pen);
}

static void pj_paint(HWND hwnd)
{
    COLORREF bg = pj_dark ? RGB(0x1F, 0x1F, 0x1F) : RGB(0xE6, 0xE6, 0xE6);
    COLORREF fg = pj_dark ? RGB(0xFF, 0xFF, 0xFF) : RGB(0x00, 0x00, 0x00);
    COLORREF hot = pj_dark ? RGB(0x3A, 0x3A, 0x3A) : RGB(0xCC, 0xCC, 0xCC);
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    HBRUSH b;
    RECT rc, r;
    int i;

    GetClientRect(hwnd, &rc);
    b = CreateSolidBrush(bg); FillRect(hdc, &rc, b); DeleteObject(b);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    SelectObject(hdc, pj_title_font);
    SetRect(&r, PS(24), PS(20), rc.right - PS(12), PS(60));
    DrawTextW(hdc, L"Project", -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    SelectObject(hdc, pj_font);
    for (i = 0; i < PJ_COUNT; i++) {
        r = pj_row(hwnd, i);
        if (i == pj_current) {
            b = CreateSolidBrush(pj_accent); FillRect(hdc, &r, b); DeleteObject(b);
            SetTextColor(hdc, RGB(0xFF, 0xFF, 0xFF));
        } else if (i == pj_sel) {
            b = CreateSolidBrush(hot); FillRect(hdc, &r, b); DeleteObject(b);
            SetTextColor(hdc, fg);
        } else SetTextColor(hdc, fg);
        if (i == pj_sel) {
            /* the keyboard's place: a frame, as Windows' focus rectangle there */
            HBRUSH f = CreateSolidBrush(i == pj_current ? RGB(0xFF, 0xFF, 0xFF) : fg);
            FrameRect(hdc, &r, f);
            DeleteObject(f);
        }
        pj_icon(hdc, &r, i, i == pj_current ? RGB(0xFF, 0xFF, 0xFF) : fg);
        r.left += PS(72);
        DrawTextW(hdc, pj_names[i], -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    }
    EndPaint(hwnd, &ps);
}

static void pj_choose(HWND hwnd, int which)
{
    pj_sel = which;
    InvalidateRect(hwnd, NULL, FALSE);
    UpdateWindow(hwnd);
    if (pj_apply(which)) DestroyWindow(hwnd);
    else InvalidateRect(hwnd, NULL, FALSE);
}

static LRESULT CALLBACK pj_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: pj_paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) DestroyWindow(hwnd);
        return 0;
    case PJ_NEXT:   /* Win+P again */
        pj_sel = (pj_sel + 1) % PJ_COUNT;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        else if (wp == VK_DOWN || wp == VK_TAB) { pj_sel = (pj_sel + 1) % PJ_COUNT; InvalidateRect(hwnd, NULL, FALSE); }
        else if (wp == VK_UP) { pj_sel = (pj_sel + PJ_COUNT - 1) % PJ_COUNT; InvalidateRect(hwnd, NULL, FALSE); }
        else if (wp == VK_RETURN || wp == VK_SPACE) pj_choose(hwnd, pj_sel);
        return 0;
    case WM_LBUTTONUP: {
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        int i;
        for (i = 0; i < PJ_COUNT; i++) {
            RECT r = pj_row(hwnd, i);
            if (PtInRect(&r, pt)) { pj_choose(hwnd, i); break; }
        }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int project_dump(void)
{
    int i;
    pj_load();
    printf("displays=%d pc=%s second=%s current=%d\n", pj_ndisps, pj_pc, pj_second, pj_current);
    for (i = 0; i < PJ_COUNT; i++) printf("choice %d %ls%s\n", i, pj_names[i], i == pj_current ? " (current)" : "");
    fflush(stdout);
    return 0;
}

int project_choose(int which)
{
    pj_load();
    if (which < 0 || which >= PJ_COUNT) return 2;
    return pj_apply(which) ? 0 : 1;
}

int project_main(void)
{
    WNDCLASSW wc = { 0 };
    MONITORINFO mi = { sizeof(mi) };
    HWND hwnd, other;
    MSG msg;
    int w;

    /* one pane: Win+P while it is open moves its highlight */
    if ((other = FindWindowW(PJ_CLASS, NULL))) {
        PostMessageW(other, PJ_NEXT, 0, 0);
        return 0;
    }
    sg_dpi_init();
    pj_dpi = sg_dpi_for(NULL);
    pj_dark = sg_mode_dark(TRUE);
    pj_accent = reg_dword(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", 0xFFC03070) & 0xFFFFFF;
    pj_title_font = CreateFontW(-PS(20), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    pj_font = CreateFontW(-PS(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    pj_load();
    pj_sel = pj_current >= 0 ? pj_current : PJ_EXTEND;

    wc.lpfnWndProc = pj_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = PJ_CLASS;
    RegisterClassW(&wc);
    /* at the main display's right edge, over its work area, as Windows 10's */
    GetMonitorInfoW(MonitorFromPoint((POINT){ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    w = PS(360);
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, PJ_CLASS, L"Project", WS_POPUP,
                           mi.rcWork.right - w, mi.rcWork.top, w, mi.rcWork.bottom - mi.rcWork.top,
                           NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
