/* Stained Glass OS: a flyout opened from its tray icon closes on a second
 * click of that icon, as on Windows.
 *
 * The second click's press makes the taskbar active, so the flyout hides on
 * losing activation -- and the click's release then reached the icon as a
 * click to open it again: it flickered and stayed open (David, 2026-10-07).
 * A guard of 200 ms after hiding caught only quick clicks; a person's click
 * is held longer. So when the flyout hides on deactivation it notes whether
 * the left button is down on its own icon; the tray click that follows leaves
 * it closed, however long the button was held. The note lasts 3 s: a press
 * dragged away and released elsewhere does not block the next real click.
 *
 *   sg_flyout_hidden(owner, id)   where the flyout hides on WA_INACTIVE
 *   sg_flyout_may_open()          in the tray icon's click, before opening
 *
 * Only one flyout is open at a time, and a click elsewhere closes it, even
 * when it never became the active window (a tray click does not always make
 * it so, and then no WA_INACTIVE comes: David, 2026-10-17):
 *   sg_flyout_shown(fly)          after showing it: the others close; a watch
 *   sg_flyout_should_hide(fly, msg, wp)   first in its window procedure
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef SG_FLYOUT_H
#define SG_FLYOUT_H
#include <windows.h>
#include <shellapi.h>

static DWORD sg_flyout_icon_press;   /* GetTickCount of a press on our icon that hid the flyout, 0: none */

static void sg_flyout_hidden(HWND owner, UINT id)
{
    NOTIFYICONIDENTIFIER nii;
    RECT r;
    POINT pt;

    sg_flyout_icon_press = 0;
#ifdef SG_MUTANT_FLYOUT_REOPENS
    return;
#endif
    if (!(GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000)) return;
    memset(&nii, 0, sizeof(nii));
    nii.cbSize = sizeof(nii);
    nii.hWnd = owner;
    nii.uID = id;
    if (GetCursorPos(&pt) && Shell_NotifyIconGetRect(&nii, &r) == S_OK && PtInRect(&r, pt))
        sg_flyout_icon_press = GetTickCount() | 1;
}

static BOOL sg_flyout_may_open(void)
{
    BOOL pressed = sg_flyout_icon_press && GetTickCount() - sg_flyout_icon_press < 3000;
    sg_flyout_icon_press = 0;
    return !pressed;
}

#define SG_FLY_WATCH 0x5f1
static UINT sg_flyout_msg;
static HWND sg_flyout_fg;            /* the active window when it opened */

static inline void sg_flyout_shown(HWND fly)
{
    if (!sg_flyout_msg) sg_flyout_msg = RegisterWindowMessageW(L"SgFlyoutOpened");
    PostMessageW(HWND_BROADCAST, sg_flyout_msg, (WPARAM)fly, 0);
    sg_flyout_fg = GetForegroundWindow();
    SetTimer(fly, SG_FLY_WATCH, 150, NULL);
}

static inline BOOL sg_flyout_ours(HWND w)
{
    DWORD pid = 0;
    WCHAR cls[64];
    if (!w) return TRUE;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    GetClassNameW(w, cls, ARRAYSIZE(cls));
    return !lstrcmpW(cls, L"Shell_TrayWnd");      /* the taskbar: its icons' clicks are handled there */
}

static inline BOOL sg_flyout_should_hide(HWND fly, UINT msg, WPARAM wp)
{
    if (!sg_flyout_msg) sg_flyout_msg = RegisterWindowMessageW(L"SgFlyoutOpened");
#ifdef SG_MUTANT_FLYOUT_STAYS
    return FALSE;
#endif
    if (msg == sg_flyout_msg && msg) return (HWND)wp != fly && IsWindowVisible(fly);
    if (msg == WM_TIMER && wp == SG_FLY_WATCH) {
        HWND fg = GetForegroundWindow();
        POINT pt;
        RECT r;
        if (!IsWindowVisible(fly)) { KillTimer(fly, SG_FLY_WATCH); return FALSE; }
        /* another program's window came to the front */
        if (fg != sg_flyout_fg && !sg_flyout_ours(fg)) return TRUE;
        /* a press outside it and outside the taskbar */
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000 || GetAsyncKeyState(VK_RBUTTON) & 0x8000) && GetCursorPos(&pt)) {
            HWND under = WindowFromPoint(pt);
            GetWindowRect(fly, &r);
            if (!PtInRect(&r, pt) && !sg_flyout_ours(under ? GetAncestor(under, GA_ROOT) : NULL)) return TRUE;
        }
    }
    return FALSE;
}
#endif
