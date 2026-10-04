/*
 * A program that starts others reads the environment variables again when
 * they change, as Windows' shell does: the Environment Variables dialog (and
 * setx, installers) broadcast WM_SETTINGCHANGE "Environment", and what is
 * started after that gets the new values -- not only after signing in again
 * (David 2026-10-04: Claude Code asked for PATH to be changed, and a
 * PowerShell started again from Start or as a Terminal tab still had the old
 * one). The taskbar does this in explorer (wine-sg 0749); Start and Terminal
 * start programs from their own processes.
 *
 * Only what the registry defines is set or removed: the session's own
 * variables (DISPLAY, XDG_RUNTIME_DIR, SG_...) stay. userenv is looked up
 * rather than linked.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_ENVRELOAD_H
#define SG_ENVRELOAD_H

#include <windows.h>
#include <stdlib.h>

/* TRUE for WM_SETTINGCHANGE's "Environment" */
static inline BOOL sg_env_changed(UINT msg, LPARAM lp)
{
    return msg == WM_SETTINGCHANGE && lp && !lstrcmpiW((const WCHAR *)lp, L"Environment");
}

static inline void sg_env_reload(void)
{
#ifndef SG_MUTANT_NO_ENV_RELOAD
    static BOOL (WINAPI *create)(void **, HANDLE, BOOL);
    static BOOL (WINAPI *destroy)(void *);
    static WCHAR *names;   /* what the registry defined last time, '\0'-separated */
    WCHAR *block = NULL, *p, *now, *out, *n, *m;
    HANDLE token;
    size_t len = 1;

    if (!create) {
        HMODULE ue = LoadLibraryW(L"userenv.dll");
        if (ue) {
            create = (void *)GetProcAddress(ue, "CreateEnvironmentBlock");
            destroy = (void *)GetProcAddress(ue, "DestroyEnvironmentBlock");
        }
        if (!create || !destroy) { create = NULL; return; }
    }
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_IMPERSONATE, &token)) return;
    if (!create((void **)&block, token, FALSE)) block = NULL;
    CloseHandle(token);
    if (!block) return;
    for (p = block; *p; p += lstrlenW(p) + 1) len += lstrlenW(p) + 1;
    if (!(now = out = malloc(len * sizeof(WCHAR)))) { destroy(block); return; }
    for (p = block; *p; p += lstrlenW(p) + 1) {
        WCHAR *eq = wcschr(p + 1, '=');
        if (!eq || *p == '=') continue;
        memcpy(out, p, (eq - p) * sizeof(WCHAR));
        out += eq - p;
        *out++ = 0;
        *eq = 0;
        SetEnvironmentVariableW(p, eq + 1);
        *eq = '=';
    }
    *out = 0;
    /* what the registry defined before and does not now: removed */
    for (n = names; n && *n; n += lstrlenW(n) + 1) {
        BOOL still = FALSE;
        for (m = now; *m; m += lstrlenW(m) + 1) if (!lstrcmpiW(m, n)) { still = TRUE; break; }
        if (!still) SetEnvironmentVariableW(n, NULL);
    }
    destroy(block);
    free(names);
    names = now;
#endif
}

#endif
