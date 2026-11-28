/* proxy-page-driver: drives Settings > Network & Internet > Proxy (sg-settings, --page
 * network-proxy) through its own controls -- sets "Use a proxy server", the address and the
 * exceptions, clicks Save -- then says what each kind of Windows program now reads:
 *   WINHTTP-IE  WinHttpGetIEProxyConfigForCurrentUser (Chrome, Edge, .NET HttpClient)
 *   WININET     InternetQueryOption PER_CONNECTION_OPTION (WinINet programs, Firefox's system proxy)
 *   PAGE        the page opened again reads the settings back
 * usage: proxy-page-driver SETTINGS.EXE
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#include <windows.h>
#include <wininet.h>
#include <stdio.h>
typedef struct { BOOL fAutoDetect; LPWSTR lpszAutoConfigUrl, lpszProxy, lpszProxyBypass; } IE_CFG;
BOOL WINAPI WinHttpGetIEProxyConfigForCurrentUser(IE_CFG *);
enum { ID_AUTODETECT = 2001, ID_USESCRIPT, ID_SCRIPT, ID_USEPROXY, ID_SERVER, ID_BYPASS, ID_LOCAL, ID_SAVE };
static DWORD g_pid;
static HWND g_top;
static BOOL CALLBACK find_top(HWND w, LPARAM l)
{
    DWORD pid; (void)l;
    GetWindowThreadProcessId(w, &pid);
    if (pid == g_pid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) { g_top = w; return FALSE; }
    return TRUE;
}
struct find { int id; HWND found; };
static BOOL CALLBACK find_id(HWND w, LPARAM l)
{
    struct find *f = (struct find *)l;
    if (GetDlgCtrlID(w) == f->id && IsWindowVisible(w)) { f->found = w; return FALSE; }
    return TRUE;
}
static HWND ctl(int id)
{
    struct find f = { id, NULL };
    EnumChildWindows(g_top, find_id, (LPARAM)&f);
    return f.found;
}
static HANDLE open_page(const char *exe)
{
    char cmd[MAX_PATH + 64];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    int i;
    snprintf(cmd, sizeof(cmd), "\"%s\" --page network-proxy", exe);
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return NULL;
    g_pid = pi.dwProcessId; g_top = NULL;
    for (i = 0; i < 300 && !(g_top && ctl(ID_SAVE)); i++) { Sleep(100); g_top = NULL; EnumWindows(find_top, 0); }
    CloseHandle(pi.hThread);
    if (!g_top || !ctl(ID_SAVE)) { printf("FAIL  the Proxy page did not open\n"); TerminateProcess(pi.hProcess, 1); return NULL; }
    return pi.hProcess;
}
static void close_page(HANDLE p)
{
    PostMessageW(g_top, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(p, 10000)) TerminateProcess(p, 1);
    CloseHandle(p);
}
int main(int argc, char **argv)
{
    IE_CFG ie = {0};
    INTERNET_PER_CONN_OPTION_LISTW list = { sizeof(list) };
    INTERNET_PER_CONN_OPTIONW o[3] = { { INTERNET_PER_CONN_FLAGS }, { INTERNET_PER_CONN_PROXY_SERVER }, { INTERNET_PER_CONN_PROXY_BYPASS } };
    DWORD size = sizeof(list);
    WCHAR text[256];
    HANDLE p;
    int rc = 0;
    if (argc < 2) return 2;
    if (!(p = open_page(argv[1]))) return 1;
    /* a new account: Windows' default is to detect automatically */
    printf("%s  a new account's page detects settings automatically\n",
           SendMessageW(ctl(ID_AUTODETECT), BM_GETCHECK, 0, 0) == BST_CHECKED ? "PASS" : (rc = 1, "FAIL"));
    SendMessageW(ctl(ID_AUTODETECT), BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(ctl(ID_USEPROXY), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(ctl(ID_SERVER), WM_SETTEXT, 0, (LPARAM)L"10.1.2.3:3128");
    SendMessageW(ctl(ID_BYPASS), WM_SETTEXT, 0, (LPARAM)L"*.example.lan");
    SendMessageW(ctl(ID_LOCAL), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(ctl(ID_SAVE), BM_CLICK, 0, 0);
    Sleep(500);
    close_page(p);

    if (WinHttpGetIEProxyConfigForCurrentUser(&ie) && ie.lpszProxy && !wcscmp(ie.lpszProxy, L"10.1.2.3:3128") && !ie.fAutoDetect &&
        ie.lpszProxyBypass && !wcscmp(ie.lpszProxyBypass, L"*.example.lan;<local>"))
        printf("PASS  WINHTTP-IE proxy=%ls bypass=%ls autodetect=%d\n", ie.lpszProxy, ie.lpszProxyBypass, ie.fAutoDetect);
    else { printf("FAIL  WINHTTP-IE proxy=%ls bypass=%ls autodetect=%d\n", ie.lpszProxy ? ie.lpszProxy : L"(none)",
                  ie.lpszProxyBypass ? ie.lpszProxyBypass : L"(none)", ie.fAutoDetect); rc = 1; }
    list.dwOptionCount = 3; list.pOptions = o;
    if (InternetQueryOptionW(NULL, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, &size) &&
        (o[0].Value.dwValue & (PROXY_TYPE_PROXY | PROXY_TYPE_AUTO_DETECT)) == PROXY_TYPE_PROXY &&
        o[1].Value.pszValue && !wcscmp(o[1].Value.pszValue, L"10.1.2.3:3128"))
        printf("PASS  WININET flags=%#lx proxy=%ls\n", o[0].Value.dwValue, o[1].Value.pszValue);
    else { printf("FAIL  WININET flags=%#lx proxy=%ls\n", o[0].Value.dwValue, o[1].Value.pszValue ? o[1].Value.pszValue : L"(none)"); rc = 1; }

    /* turned off again: WinHTTP's programs go direct */
    if (!(p = open_page(argv[1]))) return 1;
    SendMessageW(ctl(ID_SERVER), WM_GETTEXT, 256, (LPARAM)text);  /* another process's edit: WM_GETTEXT */
    if (SendMessageW(ctl(ID_USEPROXY), BM_GETCHECK, 0, 0) == BST_CHECKED && !wcscmp(text, L"10.1.2.3:3128") &&
        SendMessageW(ctl(ID_AUTODETECT), BM_GETCHECK, 0, 0) != BST_CHECKED && SendMessageW(ctl(ID_LOCAL), BM_GETCHECK, 0, 0) == BST_CHECKED)
        printf("PASS  PAGE reads the saved settings back\n");
    else { printf("FAIL  PAGE reads back server=%ls proxy=%ld autodetect=%ld local=%ld\n", text,
                  (long)SendMessageW(ctl(ID_USEPROXY), BM_GETCHECK, 0, 0), (long)SendMessageW(ctl(ID_AUTODETECT), BM_GETCHECK, 0, 0),
                  (long)SendMessageW(ctl(ID_LOCAL), BM_GETCHECK, 0, 0)); rc = 1; }
    SendMessageW(ctl(ID_USEPROXY), BM_SETCHECK, BST_UNCHECKED, 0);
    SendMessageW(ctl(ID_AUTODETECT), BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(ctl(ID_SAVE), BM_CLICK, 0, 0);
    Sleep(500);
    close_page(p);
    memset(&ie, 0, sizeof(ie));
    if (WinHttpGetIEProxyConfigForCurrentUser(&ie) && !ie.lpszProxy && ie.fAutoDetect)
        printf("PASS  WINHTTP-IE after turning the proxy off: none, autodetect\n");
    else { printf("FAIL  WINHTTP-IE after turning it off: proxy=%ls autodetect=%d\n", ie.lpszProxy ? ie.lpszProxy : L"(none)", ie.fAutoDetect); rc = 1; }
    printf("RESULT: %s\n", rc ? "FAIL" : "PASS");
    return rc;
}
