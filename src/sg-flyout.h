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
#endif
