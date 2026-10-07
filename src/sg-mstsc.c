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
 *     read it from /proc. This program asks for it ("Enter your credentials")
 *     and sg-rdp-connect hands it to the client on its standard input, from a
 *     file in the user's private runtime directory that it removes at once;
 *   - the first connection to a computer asks whether to trust it, as mstsc
 *     does for a certificate it cannot verify; the certificate is then
 *     remembered (FreeRDP's /cert:tofu) and a changed one is refused;
 *   - "Remember me" saves the credentials as mstsc does, in Credential
 *     Manager (a domain credential named TERMSRV/<computer>), which wine-sg
 *     keeps in the person's keyring; the next connection to that computer
 *     uses them without asking. Credential Manager deletes them.
 *
 * `/sg-dry-run` prints the client command line instead of starting it, which
 * is what the gate uses.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors
 */
#include <windows.h>
#include <wincred.h>
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
static BOOL g_first_use;   /* no certificate remembered for this computer yet: trust on first use */

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
    if (g_first_use) append_arg(cmd, cap, L"/cert:tofu");
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

static BOOL launch(const struct conn *c, const WCHAR *client, BOOL dry_run);

static const WCHAR CONNECT_HELPER[] = L"\\\\?\\unix\\usr\\libexec\\stained-glass\\shell\\sg-rdp-connect";

/* A \\?\unix\ path back to its Unix spelling */
static void unix_spelling(const WCHAR *path, WCHAR *out, size_t cap)
{
    WCHAR *p;
    lstrcpynW(out, !wcsncmp(path, L"\\\\?\\unix", 8) ? path + 8 : path, (int)cap);
    for (p = out; *p; p++) if (*p == '\\') *p = '/';
}

/* The password, for sg-rdp-connect: a new file in the user's private runtime
 * directory (0700), which it reads and removes. Returns the file's Unix path. */
static BOOL write_password(const WCHAR *password, WCHAR *unix_path, size_t cap)
{
    WCHAR dir[MAX_PATH], path[MAX_PATH + 64];
    char utf8[4 * MAX_FIELD];
    DWORD n, written;
    HANDLE f;
    int len;

    if (!GetEnvironmentVariableW(L"XDG_RUNTIME_DIR", dir, ARRAYSIZE(dir)) || dir[0] != '/') return FALSE;
    swprintf(unix_path, cap, L"%ls/sg-rdp-%lu-%lu", dir, GetCurrentProcessId(), GetTickCount());
    swprintf(path, ARRAYSIZE(path), L"\\\\?\\unix%ls", unix_path);
    for (n = 8; path[n]; n++) if (path[n] == '/') path[n] = '\\';
    len = WideCharToMultiByte(CP_UTF8, 0, password, -1, utf8, sizeof(utf8), NULL, NULL);
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (f == INVALID_HANDLE_VALUE || len <= 0) { SecureZeroMemory(utf8, sizeof(utf8)); return FALSE; }
    WriteFile(f, utf8, len - 1, &written, NULL);
    CloseHandle(f);
    SecureZeroMemory(utf8, sizeof(utf8));
    return TRUE;
}

static BOOL launch_with(const struct conn *c, const WCHAR *client, BOOL dry_run, const WCHAR *password)
{
    static WCHAR cmd[4096], args[4096];
    WCHAR unix_client[MAX_PATH], pwfile[MAX_PATH + 64];
    const WCHAR *why = NULL;
    STARTUPINFOW si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;

    if (!dry_run && password && password[0] && GetFileAttributesW(CONNECT_HELPER) != INVALID_FILE_ATTRIBUTES)
    {
        unix_spelling(client, unix_client, ARRAYSIZE(unix_client));
        if (build_command(c, unix_client, args, ARRAYSIZE(args), &why) &&
            write_password(password, pwfile, ARRAYSIZE(pwfile)))
        {
            cmd[0] = 0;
            append_arg(cmd, ARRAYSIZE(cmd), CONNECT_HELPER);
            append_arg(cmd, ARRAYSIZE(cmd), pwfile);
            if (wcslen(cmd) + 1 + wcslen(args) < ARRAYSIZE(cmd)) { wcscat(cmd, L" "); wcscat(cmd, args); }
            if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
            {
                CloseHandle(pi.hThread);
                CloseHandle(pi.hProcess);
                return TRUE;
            }
        }
    }
    return launch(c, client, dry_run);
}

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

/* --- the dialogs ------------------------------------------------------------------ */

/* Windows 10's mstsc: a white banner with the program's picture and name over
 * the fields, and the fields on the dialog colour; the credentials asked in a
 * dialog of their own, the certificate question before the first connection. */

enum { ID_COMPUTER = 100, ID_USER, ID_FULL, ID_PASSWORD, ID_REMEMBER, ID_SAVEDNOTE, ID_CONNECT = IDOK, ID_CANCEL = IDCANCEL };

#define BANNER_H 76

static struct conn g_conn;
static const WCHAR *g_client;
static HWND g_computer, g_user, g_full;
static HFONT g_font, g_font_title, g_font_title_light, g_font_heading;
static HICON g_icon;

static HWND add(HWND parent, const WCHAR *cls, const WCHAR *text, DWORD style,
                int x, int y, int w, int h, int id)
{
    HWND c = CreateWindowExW(!lstrcmpW(cls, L"EDIT") ? WS_EX_CLIENTEDGE : 0, cls, text,
                             WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
    return c;
}

static HFONT make_font(int points, int weight)
{
    NONCLIENTMETRICSW ncm = { .cbSize = sizeof(ncm) };
    HDC dc = GetDC(NULL);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = -MulDiv(points, GetDeviceCaps(dc, LOGPIXELSY), 72);
    ncm.lfMessageFont.lfWeight = weight;
    ReleaseDC(NULL, dc);
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

static void paint_banner(HWND hwnd, const WCHAR *line1, const WCHAR *line2)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc, band;
    HBRUSH white = CreateSolidBrush(RGB(255, 255, 255)), rule = CreateSolidBrush(RGB(0xE5, 0xE5, 0xE5));

    GetClientRect(hwnd, &rc);
    band = rc; band.bottom = BANNER_H;
    FillRect(dc, &band, white);
    band.top = BANNER_H; band.bottom = BANNER_H + 1;
    FillRect(dc, &band, rule);
    DeleteObject(white); DeleteObject(rule);
    if (g_icon) DrawIconEx(dc, 18, (BANNER_H - 48) / 2, g_icon, 48, 48, 0, NULL, DI_NORMAL);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_font_title_light);
    SetTextColor(dc, RGB(0x44, 0x44, 0x44));
    TextOutW(dc, 80, 12, line1, lstrlenW(line1));
    SelectObject(dc, g_font_title);
    SetTextColor(dc, RGB(0x1F, 0x1F, 0x1F));
    TextOutW(dc, 80, 32, line2, lstrlenW(line2));
    EndPaint(hwnd, &ps);
}

/* Is a certificate remembered for this computer? FreeRDP keeps the ones it
 * trusted in $XDG_CONFIG_HOME/freerdp/server/<host>_<port>.pem. */
static BOOL certificate_known(const struct conn *c)
{
    WCHAR base[MAX_PATH], path[MAX_PATH * 2], host[MAX_FIELD], *p;

    /* XDG_CONFIG_HOME if set, else the home directory Wine names in
     * WINEHOMEDIR (an NT path, \??\...) plus .config; unknown: ask */
    if (GetEnvironmentVariableW(L"XDG_CONFIG_HOME", base, ARRAYSIZE(base)) && base[0] == '/')
    {
        swprintf(path, ARRAYSIZE(path), L"\\\\?\\unix%ls", base);
        for (p = path + 8; *p; p++) if (*p == '/') *p = '\\';
    }
    else if (GetEnvironmentVariableW(L"WINEHOMEDIR", base, ARRAYSIZE(base)) && !wcsncmp(base, L"\\??\\", 4))
        swprintf(path, ARRAYSIZE(path), L"\\\\?\\%ls\\.config", base + 4);
    else return FALSE;
    lstrcpynW(host, c->host, ARRAYSIZE(host));
    CharLowerW(host);
    swprintf(path + wcslen(path), ARRAYSIZE(path) - wcslen(path), L"\\freerdp\\server\\%ls_%d.pem",
             host, c->port ? c->port : 3389);
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

/* The first connection to a computer: mstsc's warning about a certificate it
 * cannot verify. Yes remembers the certificate (/cert:tofu). */
static BOOL confirm_first_use(HWND owner, const struct conn *c)
{
    WCHAR text[1024];

    if (certificate_known(c)) return TRUE;
    swprintf(text, ARRAYSIZE(text),
             L"The identity of the remote computer cannot be verified. Do you want to connect anyway?\n\n"
             L"This is the first connection to %ls from this computer, and its certificate is not "
             L"from an authority this computer trusts. If you connect, the certificate is remembered, "
             L"and you will be warned if it ever changes.", c->host);
    if (MessageBoxW(owner, text, L"Remote Desktop Connection", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return FALSE;
    g_first_use = TRUE;
    return TRUE;
}

/* --- saved credentials (Credential Manager, as mstsc keeps them) ------------------- */

static void cred_target(const WCHAR *host, WCHAR *out, size_t cch)
{
    swprintf(out, cch, L"TERMSRV/%ls", host);
}

/* The saved credentials for this computer: the user ("DOMAIN\user") and the
 * password. A user name typed for this connection that is not theirs means
 * they are not used. */
static BOOL saved_credentials(const struct conn *c, WCHAR *user, size_t ucch, WCHAR *pw, size_t pcch)
{
    WCHAR target[MAX_FIELD + 16], typed[MAX_FIELD * 2];
    CREDENTIALW *cred;
    DWORD n;
    BOOL ok = FALSE;

    if (!c->host[0]) return FALSE;
    cred_target(c->host, target, ARRAYSIZE(target));
    if (!CredReadW(target, CRED_TYPE_DOMAIN_PASSWORD, 0, &cred)) return FALSE;
    if (c->domain[0]) swprintf(typed, ARRAYSIZE(typed), L"%ls\\%ls", c->domain, c->user);
    else lstrcpynW(typed, c->user, ARRAYSIZE(typed));
    n = cred->CredentialBlobSize / sizeof(WCHAR);
    if (cred->UserName && cred->UserName[0] && (!typed[0] || !lstrcmpiW(typed, cred->UserName)) &&
        cred->CredentialBlob && n && n < pcch)
    {
        lstrcpynW(user, cred->UserName, (int)ucch);
        memcpy(pw, cred->CredentialBlob, n * sizeof(WCHAR));
        pw[n] = 0;
        ok = TRUE;
    }
    if (cred->CredentialBlob) SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    CredFree(cred);
    return ok;
}

static BOOL has_saved_credentials(const WCHAR *host)
{
    WCHAR target[MAX_FIELD + 16];
    CREDENTIALW *cred;
    if (!host[0]) return FALSE;
    cred_target(host, target, ARRAYSIZE(target));
    if (!CredReadW(target, CRED_TYPE_DOMAIN_PASSWORD, 0, &cred)) return FALSE;
    CredFree(cred);
    return TRUE;
}

static BOOL remember_credentials(const struct conn *c, const WCHAR *pw)
{
    WCHAR target[MAX_FIELD + 16], user[MAX_FIELD * 2];
    CREDENTIALW cred = { 0 };

    cred_target(c->host, target, ARRAYSIZE(target));
    if (c->domain[0]) swprintf(user, ARRAYSIZE(user), L"%ls\\%ls", c->domain, c->user);
    else lstrcpynW(user, c->user, ARRAYSIZE(user));
    if (!user[0]) return FALSE;
    cred.Type = CRED_TYPE_DOMAIN_PASSWORD;
    cred.TargetName = target;
    cred.UserName = user;
    cred.CredentialBlob = (BYTE *)pw;
    cred.CredentialBlobSize = (DWORD)(wcslen(pw) * sizeof(WCHAR));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&cred, 0);
}

/* "Enter your credentials": the password, and the user name to change it. */
struct cred_dialog { HWND user, password, remember; WCHAR host[MAX_FIELD]; WCHAR user_text[MAX_FIELD * 2]; WCHAR pw[MAX_FIELD]; BOOL ok, done, keep; };

static LRESULT CALLBACK cred_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    struct cred_dialog *d = (struct cred_dialog *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg)
    {
    case WM_CREATE:
    {
        WCHAR line[MAX_FIELD + 80];
        HWND h;
        d = (struct cred_dialog *)((CREATESTRUCTW *)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
        h = add(hwnd, L"STATIC", L"Enter your credentials", 0, 24, 18, 360, 30, -1);
        SendMessageW(h, WM_SETFONT, (WPARAM)g_font_heading, TRUE);
        swprintf(line, ARRAYSIZE(line), L"These credentials will be used to connect to %ls.", d->host);
        add(hwnd, L"STATIC", line, SS_LEFT, 24, 54, 360, 36, -1);
        add(hwnd, L"STATIC", L"User name", 0, 24, 98, 360, 18, -1);
        d->user = add(hwnd, L"EDIT", d->user_text, WS_TABSTOP | ES_AUTOHSCROLL, 24, 118, 352, 28, ID_USER);
        add(hwnd, L"STATIC", L"Password", 0, 24, 156, 360, 18, -1);
        d->password = add(hwnd, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD, 24, 176, 352, 28, ID_PASSWORD);
        d->remember = add(hwnd, L"BUTTON", L"Remember me", WS_TABSTOP | BS_AUTOCHECKBOX, 24, 214, 200, 22, ID_REMEMBER);
        add(hwnd, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 176, 252, 96, 30, IDOK);
        add(hwnd, L"BUTTON", L"Cancel", WS_TABSTOP, 280, 252, 96, 30, IDCANCEL);
        SetFocus(d->user_text[0] ? d->password : d->user);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, RGB(255, 255, 255));
        return (LRESULT)GetStockObject(WHITE_BRUSH);
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK)
        {
            GetWindowTextW(d->user, d->user_text, ARRAYSIZE(d->user_text));
            GetWindowTextW(d->password, d->pw, ARRAYSIZE(d->pw));
            SetWindowTextW(d->password, L"");
            d->keep = SendMessageW(d->remember, BM_GETCHECK, 0, 0) == BST_CHECKED;
            d->ok = TRUE;
            DestroyWindow(hwnd);
        }
        else if (LOWORD(wp) == IDCANCEL) DestroyWindow(hwnd);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        d->done = TRUE;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* Asks for the password (and lets the user name change). The connection's
 * user and domain are updated; the password goes to pw. */
static BOOL ask_credentials(HWND owner, struct conn *c, WCHAR *pw, size_t cap, BOOL *keep)
{
    static struct cred_dialog d;
    WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
    RECT r = { 0, 0, 400, 300 }, o;
    HWND hwnd;
    MSG msg;
    int x, y;

    memset(&d, 0, sizeof(d));
    lstrcpynW(d.host, c->host, ARRAYSIZE(d.host));
    if (c->domain[0]) swprintf(d.user_text, ARRAYSIZE(d.user_text), L"%ls\\%ls", c->domain, c->user);
    else lstrcpynW(d.user_text, c->user, ARRAYSIZE(d.user_text));

    wc.lpfnWndProc = cred_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = L"SgRemoteDesktopCredentials";
    RegisterClassExW(&wc);
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    if (owner) GetWindowRect(owner, &o);
    else SetRect(&o, 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN));
    x = (o.left + o.right - (r.right - r.left)) / 2;
    y = (o.top + o.bottom - (r.bottom - r.top)) / 2;
    hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, wc.lpszClassName, L"Remote Desktop Connection",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y,
                           r.right - r.left, r.bottom - r.top, owner, NULL, wc.hInstance, &d);
    if (!hwnd) return FALSE;
    if (owner) EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);
    while (!d.done && GetMessageW(&msg, NULL, 0, 0))
    {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (owner) { EnableWindow(owner, TRUE); SetForegroundWindow(owner); }
    if (!d.ok) return FALSE;
    c->user[0] = c->domain[0] = 0;
    if (d.user_text[0]) set_user(c, d.user_text);
    lstrcpynW(pw, d.pw, (int)cap);
    SecureZeroMemory(d.pw, sizeof(d.pw));
    *keep = d.keep;
    return TRUE;
}

/* The certificate question, the credentials, then the client. */
static BOOL connect_to(HWND owner, struct conn *c)
{
    WCHAR pw[MAX_FIELD];
    BOOL ret;

    if (!valid_host(c->host))
    {
        MessageBoxW(owner, L"The computer name is not valid.", L"Remote Desktop Connection", MB_ICONERROR);
        return FALSE;
    }
    /* the gate (SG_MSTSC_TEST=1) checks the command line, not the dialogs */
    if (GetEnvironmentVariableW(L"SG_MSTSC_TEST", pw, ARRAYSIZE(pw)) && !lstrcmpW(pw, L"1"))
        return launch(c, g_client, FALSE);
    if (!confirm_first_use(owner, c)) return FALSE;
    {
        WCHAR user[MAX_FIELD * 2];
        BOOL keep = FALSE;
        /* saved for this computer ("Remember me"): used without asking */
        if (saved_credentials(c, user, ARRAYSIZE(user), pw, ARRAYSIZE(pw)))
        {
            c->user[0] = c->domain[0] = 0;
            set_user(c, user);
        }
        else
        {
            if (!ask_credentials(owner, c, pw, ARRAYSIZE(pw), &keep)) return FALSE;
            if (keep && pw[0] && !remember_credentials(c, pw))
                MessageBoxW(owner, L"Your credentials could not be saved.", L"Remote Desktop Connection", MB_ICONWARNING);
        }
    }
    ret = launch_with(c, g_client, FALSE, pw);
    SecureZeroMemory(pw, sizeof(pw));
    return ret;
}

static const WCHAR ASK_NOTE[] = L"You will be asked for credentials when you connect.";
static const WCHAR SAVED_NOTE[] = L"Saved credentials will be used to connect to this computer. Credential Manager deletes them.";

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        add(hwnd, L"STATIC", L"Computer:", 0, 24, BANNER_H + 24, 90, 20, -1);
        g_computer = add(hwnd, L"EDIT", g_conn.host, WS_TABSTOP | ES_AUTOHSCROLL, 120, BANNER_H + 20, 276, 28, ID_COMPUTER);
        add(hwnd, L"STATIC", L"User name:", 0, 24, BANNER_H + 62, 90, 20, -1);
        g_user = add(hwnd, L"EDIT", g_conn.user, WS_TABSTOP | ES_AUTOHSCROLL, 120, BANNER_H + 58, 276, 28, ID_USER);
        add(hwnd, L"STATIC", has_saved_credentials(g_conn.host) ? SAVED_NOTE : ASK_NOTE,
            0, 120, BANNER_H + 92, 276, 36, ID_SAVEDNOTE);
        g_full = add(hwnd, L"BUTTON", L"Full screen", WS_TABSTOP | BS_AUTOCHECKBOX, 120, BANNER_H + 130, 200, 22, ID_FULL);
        add(hwnd, L"BUTTON", L"Connect", WS_TABSTOP | BS_DEFPUSHBUTTON, 196, BANNER_H + 172, 96, 30, ID_CONNECT);
        add(hwnd, L"BUTTON", L"Cancel", WS_TABSTOP, 300, BANNER_H + 172, 96, 30, ID_CANCEL);
        SetFocus(g_conn.host[0] ? g_user : g_computer);
        return 0;

    case WM_PAINT:
        paint_banner(hwnd, L"Remote Desktop", L"Connection");
        return 0;

    case WM_COMMAND:
        if (LOWORD(wp) == ID_COMPUTER && HIWORD(wp) == EN_CHANGE)
        {
            /* as mstsc: say whether saved credentials will be used */
            WCHAR computer[MAX_FIELD];
            struct conn c = { 0 };
            GetWindowTextW(g_computer, computer, MAX_FIELD);
            set_address(&c, computer);
            SetDlgItemTextW(hwnd, ID_SAVEDNOTE, has_saved_credentials(c.host) ? SAVED_NOTE : ASK_NOTE);
            return 0;
        }
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
            if (connect_to(hwnd, &c))
            {
                WCHAR saved[MAX_FIELD * 2];
                if (c.domain[0]) swprintf(saved, ARRAYSIZE(saved), L"%ls\\%ls", c.domain, c.user);
                else lstrcpynW(saved, c.user, ARRAYSIZE(saved));
                save_last(computer, saved);
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

static void init_look(HINSTANCE inst)
{
    g_font = make_font(9, FW_NORMAL);
    g_font_heading = make_font(15, FW_NORMAL);
    g_font_title_light = make_font(12, FW_LIGHT);
    g_font_title = make_font(18, FW_SEMIBOLD);
    g_icon = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 48, 48, 0);
}

static int run_dialog(HINSTANCE inst)
{
    WNDCLASSEXW wc = { .cbSize = sizeof(wc) };
    RECT r = { 0, 0, 420, BANNER_H + 218 };
    HWND hwnd;
    MSG msg;

    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"SgRemoteDesktop";
    RegisterClassExW(&wc);

    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    hwnd = CreateWindowW(wc.lpszClassName, L"Remote Desktop Connection",
                         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
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

    /* Test only (SG_MSTSC_TEST=1), the saved credentials for the gate:
     * /sg-remember /v:HOST USER PASSWORD saves them as "Remember me" does;
     * /sg-saved /v:HOST prints "SAVED <user> <password length>" or "NONE",
     * as a connection would find them. */
    if (argv && argc > 2 && !lstrcmpW(test_flag, L"1") && !_wcsnicmp(argv[2], L"/v:", 3) &&
        (!lstrcmpiW(argv[1], L"/sg-remember") || !lstrcmpiW(argv[1], L"/sg-saved")))
    {
        WCHAR user[MAX_FIELD * 2], pw[MAX_FIELD];
        char line[MAX_FIELD * 3];
        struct conn c = { 0 };
        DWORD w;
        set_address(&c, argv[2] + 3);
        if (!lstrcmpiW(argv[1], L"/sg-remember"))
        {
            if (argc < 5) return 2;
            set_user(&c, argv[3]);
            return remember_credentials(&c, argv[4]) ? 0 : 1;
        }
        if (argc > 3) set_user(&c, argv[3]);
        if (saved_credentials(&c, user, ARRAYSIZE(user), pw, ARRAYSIZE(pw)))
        {
            char u8[MAX_FIELD * 2];
            WideCharToMultiByte(CP_UTF8, 0, user, -1, u8, sizeof(u8), NULL, NULL);
            snprintf(line, sizeof(line), "SAVED %s %u\n", u8, (unsigned)wcslen(pw));
            SecureZeroMemory(pw, sizeof(pw));
        }
        else snprintf(line, sizeof(line), "NONE\n");
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, (DWORD)strlen(line), &w, NULL);
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

    /* Like mstsc: with a target on the command line, connect straight away
     * (after the certificate question and the credentials). */
    if (dry_run) return launch(&g_conn, g_client, TRUE) ? 0 : 1;
    init_look(inst);
    if (have_target) return connect_to(NULL, &g_conn) ? 0 : 1;

    return run_dialog(inst);
}
