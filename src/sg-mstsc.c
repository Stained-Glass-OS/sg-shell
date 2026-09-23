/* sg-mstsc: Remote Desktop Connection for Stained Glass OS.
 *
 * The outbound half of "RDP in and out". A Windows program, so it sits where a
 * Windows user looks for it (the Start menu, `mstsc` at a prompt) and takes
 * mstsc's command line: an .rdp file, /v:server[:port], /f, /w: and /h:. The
 * connection itself is made by FreeRDP's native client, started through Wine's
 * \\?\unix\ path the way LockWorkStation() reaches sg-lockctl (wine-sg patch
 * 0010).
 *
 * Security, because .rdp files arrive as email attachments:
 *   - every value becomes its own properly quoted argument, so nothing in a
 *     file can add FreeRDP options -- a smuggled /drive: would share this
 *     machine's disks with the server;
 *   - server and user names are validated against conservative character sets;
 *   - device and drive redirection settings in .rdp files are ignored: this
 *     client redirects the clipboard (mstsc's default) and nothing else;
 *   - the password never appears on a command line, where any local user could
 *     read it from /proc. FreeRDP asks for it in its own dialog.
 *
 * `/sg-dry-run` prints the client command line instead of starting it, which
 * is what the gate uses.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#define MAX_FIELD 256

/* The native client. sdl-freerdp3 has its own dialogs for credentials and
 * certificate trust, which a program started from the desktop needs: it has no
 * terminal to prompt on. Overridable for testing and for other distributions. */
static const WCHAR DEFAULT_CLIENT[] = L"\\\\?\\unix\\usr\\bin\\sdl-freerdp3";

static const WCHAR REG_KEY[] = L"Software\\Stained Glass\\Remote Desktop";

struct conn {
    WCHAR host[MAX_FIELD];      /* server, without port */
    int   port;                 /* 0 = default */
    WCHAR user[MAX_FIELD];
    WCHAR domain[MAX_FIELD];
    int   fullscreen;
    int   width, height;        /* 0 = client default */
};

/* --- validation ------------------------------------------------------------ */

/* Host names, IPv4 and bracket-free IPv6 literals. */
static BOOL valid_host(const WCHAR *s)
{
    if (!*s || *s == '-' || *s == '/' || *s == '+') return FALSE;
    for (; *s; s++)
        if (!(iswalnum(*s) || *s == '.' || *s == '-' || *s == ':' || *s == '_'))
            return FALSE;
    return TRUE;
}

/* User and domain names: printable, no separators that could matter to a
 * command line or to FreeRDP's own parsing. */
static BOOL valid_name(const WCHAR *s)
{
    if (*s == '-' || *s == '/' || *s == '+') return FALSE;
    for (; *s; s++)
        if (*s < 0x20 || *s == '"' || *s == '\\' || *s == ' ' || *s == ',')
            return FALSE;
    return TRUE;
}

/* "DOMAIN\user" into domain and user. Anything else -- a bare name, or a
 * "user@domain" UPN, which FreeRDP accepts as it is -- is the user. */
static void set_user(struct conn *c, const WCHAR *v)
{
    const WCHAR *bs = wcschr(v, '\\');
    if (bs)
    {
        size_t n = bs - v;
        if (n >= MAX_FIELD) n = MAX_FIELD - 1;
        wmemcpy(c->domain, v, n); c->domain[n] = 0;
        lstrcpynW(c->user, bs + 1, MAX_FIELD);
    }
    else lstrcpynW(c->user, v, MAX_FIELD);
}

/* "host", "host:port" or "[v6]:port". */
static void set_address(struct conn *c, const WCHAR *v)
{
    const WCHAR *colon = wcsrchr(v, ':');
    if (v[0] == '[')                            /* [v6] or [v6]:port */
    {
        const WCHAR *close = wcschr(v, ']');
        if (!close) return;
        size_t n = close - v - 1;
        if (n >= MAX_FIELD) n = MAX_FIELD - 1;
        wmemcpy(c->host, v + 1, n); c->host[n] = 0;
        if (close[1] == ':') c->port = _wtoi(close + 2);
        return;
    }
    if (colon && colon == wcschr(v, ':'))       /* exactly one colon: host:port */
    {
        size_t n = colon - v;
        if (n >= MAX_FIELD) n = MAX_FIELD - 1;
        wmemcpy(c->host, v, n); c->host[n] = 0;
        c->port = _wtoi(colon + 1);
    }
    else lstrcpynW(c->host, v, MAX_FIELD);      /* plain host, or a bare v6 */
}

/* --- .rdp files -------------------------------------------------------------- */

/* mstsc writes UTF-16LE with a BOM; hand-written files are usually ANSI or
 * UTF-8. Returns a NUL-terminated wide copy, or NULL. */
static WCHAR *read_text_file(const WCHAR *path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got;
    BYTE *raw;
    WCHAR *text;

    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size > 1 << 20) { CloseHandle(f); return NULL; }
    raw = HeapAlloc(GetProcessHeap(), 0, size + 2);
    if (!raw || !ReadFile(f, raw, size, &got, NULL)) { CloseHandle(f); return NULL; }
    CloseHandle(f);
    raw[got] = raw[got + 1] = 0;

    if (got >= 2 && raw[0] == 0xFF && raw[1] == 0xFE)
    {
        text = HeapAlloc(GetProcessHeap(), 0, got + 2);
        memcpy(text, raw + 2, got - 2);
        text[(got - 2) / 2] = 0;
    }
    else
    {
        const char *s = (const char *)raw;
        UINT cp = CP_ACP;
        int n;
        if (got >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) { s += 3; cp = CP_UTF8; }
        else if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0)) cp = CP_UTF8;
        n = MultiByteToWideChar(cp, 0, s, -1, NULL, 0);
        text = HeapAlloc(GetProcessHeap(), 0, n * sizeof(WCHAR));
        MultiByteToWideChar(cp, 0, s, -1, text, n);
    }
    HeapFree(GetProcessHeap(), 0, raw);
    return text;
}

/* Lines are "name:type:value". Only the keys that say where to connect and how
 * large a window to use are honoured; everything else -- including every
 * redirection setting -- is ignored on purpose. */
static BOOL load_rdp_file(struct conn *c, const WCHAR *path)
{
    WCHAR *text = read_text_file(path), *line, *next;
    if (!text) return FALSE;

    for (line = text; line && *line; line = next)
    {
        WCHAR *name, *type, *value, *p;
        next = wcspbrk(line, L"\r\n");
        if (next) { *next++ = 0; while (*next == '\r' || *next == '\n') next++; }

        name = line;
        if (!(p = wcschr(name, ':'))) continue;
        *p = 0; type = p + 1;
        if (!(p = wcschr(type, ':'))) continue;
        *p = 0; value = p + 1;

        if (!lstrcmpiW(name, L"full address") && !lstrcmpiW(type, L"s")) set_address(c, value);
        else if (!lstrcmpiW(name, L"server port") && !lstrcmpiW(type, L"i")) c->port = _wtoi(value);
        else if (!lstrcmpiW(name, L"username") && !lstrcmpiW(type, L"s")) set_user(c, value);
        else if (!lstrcmpiW(name, L"domain") && !lstrcmpiW(type, L"s")) lstrcpynW(c->domain, value, MAX_FIELD);
        else if (!lstrcmpiW(name, L"screen mode id") && !lstrcmpiW(type, L"i")) c->fullscreen = (_wtoi(value) == 2);
        else if (!lstrcmpiW(name, L"desktopwidth") && !lstrcmpiW(type, L"i")) c->width = _wtoi(value);
        else if (!lstrcmpiW(name, L"desktopheight") && !lstrcmpiW(type, L"i")) c->height = _wtoi(value);
    }
    HeapFree(GetProcessHeap(), 0, text);
    return TRUE;
}

/* --- the client command line ---------------------------------------------------- */

/* Append one argument, quoted by the rules Windows -- and Wine, when it turns
 * a command line back into a native argv -- use to split it again. Inside
 * quotes, backslashes are literal except in front of a quote or the closing
 * quote, where each must be doubled; a literal quote is \". This is the
 * boundary between a hostile .rdp file and FreeRDP's options, so it follows
 * the standard algorithm exactly rather than anything cleverer. */
static void append_arg(WCHAR *cmd, size_t cap, const WCHAR *arg)
{
    size_t len = wcslen(cmd);
    const WCHAR *p;
    unsigned i, slashes;

#define PUT(ch) do { if (len < cap - 1) cmd[len++] = (ch); } while (0)
    if (len) PUT(' ');
    if (*arg && !wcspbrk(arg, L" \t\n\v\""))
    {
        for (p = arg; *p; p++) PUT(*p);
        cmd[len] = 0;
        return;
    }
    PUT('"');
    for (p = arg; ; p++)
    {
        for (slashes = 0; *p == '\\'; p++) slashes++;
        if (!*p)
        {
            for (i = 0; i < slashes * 2; i++) PUT('\\');
            break;
        }
        if (*p == '"')
        {
            for (i = 0; i < slashes * 2 + 1; i++) PUT('\\');
            PUT('"');
        }
        else
        {
            for (i = 0; i < slashes; i++) PUT('\\');
            PUT(*p);
        }
    }
    PUT('"');
    cmd[len] = 0;
#undef PUT
}

static void append_opt(WCHAR *cmd, size_t cap, const WCHAR *opt, const WCHAR *value)
{
    WCHAR buf[MAX_FIELD + 32];
    swprintf(buf, ARRAYSIZE(buf), L"%ls%ls", opt, value);
    append_arg(cmd, cap, buf);
}

/* Returns FALSE, with a reason, when the connection cannot be made safely. */
static BOOL build_command(const struct conn *c, const WCHAR *client, WCHAR *cmd, size_t cap,
                          const WCHAR **why)
{
    WCHAR buf[64];

    if (!valid_host(c->host)) { *why = L"The computer name is not valid."; return FALSE; }
    if (c->user[0] && !valid_name(c->user)) { *why = L"The user name is not valid."; return FALSE; }
    if (c->domain[0] && !valid_name(c->domain)) { *why = L"The domain name is not valid."; return FALSE; }
    if (c->port < 0 || c->port > 65535) { *why = L"The port is not valid."; return FALSE; }

    cmd[0] = 0;
    append_arg(cmd, cap, client);
    if (c->port) { swprintf(buf, ARRAYSIZE(buf), L":%d", c->port); }
    else buf[0] = 0;
    {
        WCHAR addr[MAX_FIELD + 16];
        swprintf(addr, ARRAYSIZE(addr), wcschr(c->host, ':') ? L"[%ls]%ls" : L"%ls%ls", c->host, buf);
        append_opt(cmd, cap, L"/v:", addr);
    }
    if (c->user[0]) append_opt(cmd, cap, L"/u:", c->user);
    if (c->domain[0]) append_opt(cmd, cap, L"/d:", c->domain);
    if (c->fullscreen) append_arg(cmd, cap, L"/f");
    else if (c->width > 0 && c->height > 0)
    {
        swprintf(buf, ARRAYSIZE(buf), L"/size:%dx%d", c->width, c->height);
        append_arg(cmd, cap, buf);
    }
    else append_arg(cmd, cap, L"/dynamic-resolution");
    append_arg(cmd, cap, L"+clipboard");
    return TRUE;
}

/* --- remembering the last connection ------------------------------------------ */

static void load_last(struct conn *c)
{
    HKEY k;
    DWORD n;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &k)) return;
    n = sizeof(c->host);   RegQueryValueExW(k, L"LastComputer", NULL, NULL, (BYTE *)c->host, &n);
    n = sizeof(c->user);   RegQueryValueExW(k, L"LastUser", NULL, NULL, (BYTE *)c->user, &n);
    RegCloseKey(k);
}

static void save_last(const WCHAR *computer, const WCHAR *user)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)) return;
    RegSetValueExW(k, L"LastComputer", 0, REG_SZ, (const BYTE *)computer, (lstrlenW(computer) + 1) * sizeof(WCHAR));
    RegSetValueExW(k, L"LastUser", 0, REG_SZ, (const BYTE *)user, (lstrlenW(user) + 1) * sizeof(WCHAR));
    RegCloseKey(k);
}

/* --- starting the client --------------------------------------------------------- */

static BOOL launch(const struct conn *c, const WCHAR *client, BOOL dry_run)
{
    static WCHAR cmd[4096];
    const WCHAR *why = NULL;
    STARTUPINFOW si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;

    if (!build_command(c, client, cmd, ARRAYSIZE(cmd), &why))
    {
        if (dry_run)
        {
            char out[512] = "REFUSED ";
            DWORD w;
            int n;
            WideCharToMultiByte(CP_UTF8, 0, why, -1, out + 8, sizeof(out) - 10, NULL, NULL);
            n = lstrlenA(out);
            out[n++] = '\n';
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out, n, &w, NULL);
        }
        else MessageBoxW(NULL, why, L"Remote Desktop Connection", MB_ICONERROR);
        return FALSE;
    }

    if (dry_run)
    {
        char out[8192];
        DWORD w;
        int n = WideCharToMultiByte(CP_UTF8, 0, cmd, -1, out, sizeof(out) - 2, NULL, NULL);
        if (n > 0) { out[n - 1] = '\n'; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out, n, &w, NULL); }
        return TRUE;
    }

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
    {
        WCHAR msg[512];
        swprintf(msg, ARRAYSIZE(msg),
                 L"Could not start the Remote Desktop client (error %lu).\n\n%ls",
                 GetLastError(), client);
        MessageBoxW(NULL, msg, L"Remote Desktop Connection", MB_ICONERROR);
        return FALSE;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

/* --- the dialog ------------------------------------------------------------------ */

enum { ID_COMPUTER = 100, ID_USER, ID_FULL, ID_CONNECT = IDOK, ID_CANCEL = IDCANCEL };

static struct conn g_conn;
static const WCHAR *g_client;
static HWND g_computer, g_user, g_full;
static HFONT g_font;

static HWND add(HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style,
                int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(!lstrcmpW(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text,
                             WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        add(hwnd, L"STATIC", L"Computer:", 0, 16, 22, 90, 20, -1);
        g_computer = add(hwnd, L"EDIT", g_conn.host, WS_TABSTOP | ES_AUTOHSCROLL, 110, 18, 250, 24, ID_COMPUTER);
        add(hwnd, L"STATIC", L"User name:", 0, 16, 58, 90, 20, -1);
        g_user = add(hwnd, L"EDIT", g_conn.user, WS_TABSTOP | ES_AUTOHSCROLL, 110, 54, 250, 24, ID_USER);
        g_full = add(hwnd, L"BUTTON", L"Full screen", WS_TABSTOP | BS_AUTOCHECKBOX, 110, 88, 200, 22, ID_FULL);
        add(hwnd, L"STATIC", L"You will be asked for your password when you connect.",
            0, 16, 118, 344, 20, -1);
        add(hwnd, L"BUTTON", L"Connect", WS_TABSTOP | BS_DEFPUSHBUTTON, 188, 150, 84, 28, ID_CONNECT);
        add(hwnd, L"BUTTON", L"Cancel", WS_TABSTOP, 278, 150, 84, 28, ID_CANCEL);
        SetFocus(g_conn.host[0] ? g_user : g_computer);
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_CONNECT)
        {
            WCHAR computer[MAX_FIELD], user[MAX_FIELD];
            struct conn c = g_conn;
            GetWindowTextW(g_computer, computer, MAX_FIELD);
            GetWindowTextW(g_user, user, MAX_FIELD);
            c.host[0] = c.user[0] = c.domain[0] = 0;
            c.port = 0;
            set_address(&c, computer);
            if (user[0]) set_user(&c, user);
            c.fullscreen = SendMessageW(g_full, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (launch(&c, g_client, FALSE))
            {
                save_last(computer, user);
                DestroyWindow(hwnd);
            }
        }
        else if (LOWORD(wp) == ID_CANCEL) DestroyWindow(hwnd);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int run_dialog(HINSTANCE inst)
{
    WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
    NONCLIENTMETRICSW ncm = { .cbSize = sizeof(ncm) };
    RECT r = { 0, 0, 378, 196 };
    HWND hwnd;
    MSG msg;

    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"SgRemoteDesktop";
    RegisterClassExW(&wc);

    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    hwnd = CreateWindowW(wc.lpszClassName, L"Remote Desktop Connection",
                         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                         CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                         NULL, NULL, inst, NULL);
    ShowWindow(hwnd, SW_SHOW);
    while (GetMessageW(&msg, NULL, 0, 0))
    {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

/* --- entry point ------------------------------------------------------------------- */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    int argc, i;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    BOOL dry_run = FALSE, have_target = FALSE;
    static WCHAR client[MAX_PATH];
    DWORD n;

    (void)prev; (void)cmdline; (void)show;

    n = GetEnvironmentVariableW(L"SG_RDP_CLIENT", client, ARRAYSIZE(client));
    g_client = (n && n < ARRAYSIZE(client)) ? client : DEFAULT_CLIENT;

    load_last(&g_conn);

    /* Test only: pass every following argument through append_arg to the
     * client, so the gate can round-trip awkward strings -- spaces, quotes,
     * backslashes before quotes -- through Wine's command-line-to-argv
     * conversion. Real arguments are validated to contain none of those, so
     * without this the quoting would never be exercised. Honoured only when
     * SG_MSTSC_TEST=1: it bypasses validation, though it grants nothing that
     * running the client directly would not. */
    WCHAR test_flag[4] = { 0 };
    GetEnvironmentVariableW(L"SG_MSTSC_TEST", test_flag, ARRAYSIZE(test_flag));
    if (argv && argc > 1 && !lstrcmpiW(argv[1], L"/sg-echo-args") && !lstrcmpW(test_flag, L"1"))
    {
        static WCHAR cmd[4096];
        STARTUPINFOW si = { .cb = sizeof(si) };
        PROCESS_INFORMATION pi;
        cmd[0] = 0;
        append_arg(cmd, ARRAYSIZE(cmd), g_client);
        for (i = 2; i < argc; i++) append_arg(cmd, ARRAYSIZE(cmd), argv[i]);
        if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return 1;
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return 0;
    }

    for (i = 1; argv && i < argc; i++)
    {
        const WCHAR *a = argv[i];
        if (!lstrcmpiW(a, L"/sg-dry-run")) dry_run = TRUE;
        else if (!_wcsnicmp(a, L"/v:", 3))
        {
            g_conn.host[0] = 0; g_conn.port = 0;
            set_address(&g_conn, a + 3);
            have_target = TRUE;
        }
        else if (!lstrcmpiW(a, L"/f")) g_conn.fullscreen = 1;
        else if (!_wcsnicmp(a, L"/w:", 3)) g_conn.width = _wtoi(a + 3);
        else if (!_wcsnicmp(a, L"/h:", 3)) g_conn.height = _wtoi(a + 3);
        else if (a[0] != '/')
        {
            /* An .rdp file: what it says replaces anything remembered. */
            struct conn fresh = { 0 };
            if (load_rdp_file(&fresh, a)) { g_conn = fresh; have_target = g_conn.host[0] != 0; }
            else if (!dry_run)
            {
                MessageBoxW(NULL, L"The connection file could not be read.",
                            L"Remote Desktop Connection", MB_ICONERROR);
                return 1;
            }
            else
            {
                DWORD w;
                WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "REFUSED unreadable file\n", 24, &w, NULL);
                return 1;
            }
        }
        /* other mstsc switches (/admin, /public, /span, /multimon, ...) are
         * accepted and ignored */
    }

    /* Like mstsc: with a target on the command line, connect straight away. */
    if (have_target || dry_run)
        return launch(&g_conn, g_client, dry_run) ? 0 : 1;

    return run_dialog(inst);
}
