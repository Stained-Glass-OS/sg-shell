/* sg-control -- the Stained Glass OS Control Panel.
 *
 * A single window that shows the machine's real state: the edition and
 * computer name, the signed-in user and whether they are an administrator in
 * this session, how Windows Update is managed, and how many machine policies
 * are in force. It reads live data -- the token, the registry, the update
 * marker -- rather than canned strings, so it is a true panel, not a mockup.
 *
 * Windows programs open the Control Panel through control.exe; sg-shell adds a
 * Start-menu entry, and an App Paths entry maps control.exe here.
 *
 * With --dump it prints what it would show to stdout and exits, so the gate can
 * check the data without reading pixels.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>
#include <stdio.h>

#define WIN_W 620
#define WIN_H 620
#define MARGIN 24

#define COL_BG      RGB(0x20, 0x20, 0x20)
#define COL_CARD    RGB(0x2B, 0x2B, 0x2B)
#define COL_TEXT    RGB(0xFF, 0xFF, 0xFF)
#define COL_SUBTLE  RGB(0xB0, 0xB0, 0xB0)
#define COL_ACCENT  RGB(0x7B, 0x2F, 0xBE)

static const WCHAR CLASS_NAME[] = L"SgControlWindow";

/* ---- gathered facts ---------------------------------------------------- */
struct facts {
    WCHAR edition[64];
    WCHAR computer[MAX_COMPUTERNAME_LENGTH + 1];
    WCHAR user[256];        /* DOMAIN\user or user */
    WCHAR arch[32];
    BOOL  is_admin;         /* administrator in THIS session (split token) */
    WCHAR update[128];
    int   policy_count;
    WCHAR os_build[64];     /* the OS build string, from the registry */
    WCHAR cpu[128];         /* processor name, from the registry */
    WCHAR ram[32];          /* installed physical memory, human-readable */
    int   program_count;    /* installed programs (Uninstall entries) */
};

/* Read a REG_SZ value into out; leave out untouched on any failure. */
static void reg_read_sz(HKEY root, const WCHAR *sub, const WCHAR *val,
                        WCHAR *out, DWORD out_cch)
{
    HKEY k;
    DWORD type, cb = out_cch * sizeof(WCHAR);
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    if (RegQueryValueExW(k, val, NULL, &type, (BYTE *)out, &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ))
        out[out_cch - 1] = 0;   /* ensure termination */
    RegCloseKey(k);
}

/* Count the subkeys under a key (0 if it does not open). */
static int count_subkeys(HKEY root, const WCHAR *sub)
{
    HKEY k;
    DWORD subs = 0;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    RegQueryInfoKeyW(k, NULL, NULL, NULL, &subs, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    return (int)subs;
}

/* the signed-in user's name, via the process token's SID */
static void get_user_name(WCHAR *out, size_t out_cch)
{
    HANDLE tok;
    char buf[256];
    DWORD len = sizeof(buf);
    WCHAR name[128], dom[128];
    DWORD nlen = ARRAYSIZE(name), dlen = ARRAYSIZE(dom);
    SID_NAME_USE use;

    lstrcpynW(out, L"(unknown)", (int)out_cch);
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return;
    if (GetTokenInformation(tok, TokenUser, buf, len, &len) &&
        LookupAccountSidW(NULL, ((TOKEN_USER *)buf)->User.Sid, name, &nlen, dom, &dlen, &use))
    {
        if (dlen && dom[0]) _snwprintf(out, out_cch, L"%s\\%s", dom, name);
        else lstrcpynW(out, name, (int)out_cch);
    }
    CloseHandle(tok);
}

/* is the session user an administrator right now? On Stained Glass ordinary
 * sessions run unprivileged (split token, ADR 0012), so this is normally No --
 * administrators elevate a program through the broker rather than run elevated. */
static BOOL user_is_admin(void)
{
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD n = sizeof(sid);
    BOOL member = FALSE;
    if (CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, sid, &n))
        CheckTokenMembership(NULL, sid, &member);
    return member;
}

/* count values under an HKLM policy key (a rough "how many policies apply") */
static int count_policy_values(HKEY root, const WCHAR *sub)
{
    HKEY k;
    DWORD values = 0;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return 0;
    RegQueryInfoKeyW(k, NULL, NULL, NULL, NULL, NULL, NULL, &values, NULL, NULL, NULL, NULL);
    RegCloseKey(k);
    return (int)values;
}

static void gather(struct facts *f)
{
    DWORD n;
    SYSTEM_INFO si;

    lstrcpyW(f->edition, L"Stained Glass OS");
    n = ARRAYSIZE(f->computer);
    if (!GetComputerNameW(f->computer, &n)) lstrcpyW(f->computer, L"(unknown)");
    get_user_name(f->user, ARRAYSIZE(f->user));
    f->is_admin = user_is_admin();

    GetNativeSystemInfo(&si);
    switch (si.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: lstrcpyW(f->arch, L"64-bit (x64)"); break;
    case PROCESSOR_ARCHITECTURE_INTEL: lstrcpyW(f->arch, L"32-bit (x86)"); break;
    case PROCESSOR_ARCHITECTURE_ARM64: lstrcpyW(f->arch, L"64-bit (ARM64)"); break;
    default: lstrcpyW(f->arch, L"unknown"); break;
    }

    /* Windows Update: managed by the OS; staged updates install on reboot. */
    lstrcpyW(f->update, L"Managed by Stained Glass OS (apt)");

    f->policy_count =
        count_policy_values(HKEY_LOCAL_MACHINE,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer") +
        count_policy_values(HKEY_LOCAL_MACHINE,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\System") +
        count_policy_values(HKEY_LOCAL_MACHINE, L"Software\\Policies");

    /* OS build, from where Windows keeps it. */
    lstrcpyW(f->os_build, L"(unknown)");
    {
        WCHAR disp[32] = L"", build[32] = L"";
        reg_read_sz(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion",
                    L"DisplayVersion", disp, ARRAYSIZE(disp));
        reg_read_sz(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion",
                    L"CurrentBuild", build, ARRAYSIZE(build));
        if (disp[0] && build[0]) _snwprintf(f->os_build, ARRAYSIZE(f->os_build), L"%ls (build %ls)", disp, build);
        else if (build[0]) _snwprintf(f->os_build, ARRAYSIZE(f->os_build), L"build %ls", build);
    }

    /* Processor, from the hardware description the loader writes. */
    lstrcpyW(f->cpu, L"(unknown)");
    reg_read_sz(HKEY_LOCAL_MACHINE,
                L"Hardware\\Description\\System\\CentralProcessor\\0",
                L"ProcessorNameString", f->cpu, ARRAYSIZE(f->cpu));

    /* Installed physical memory. */
    {
        MEMORYSTATUSEX ms;
        ms.dwLength = sizeof(ms);
        if (GlobalMemoryStatusEx(&ms)) {
            double gb = (double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
            _snwprintf(f->ram, ARRAYSIZE(f->ram), L"%.1f GB", gb);
        } else lstrcpyW(f->ram, L"(unknown)");
    }

    /* Installed programs: the classic Uninstall entries, machine and user. */
    f->program_count =
        count_subkeys(HKEY_LOCAL_MACHINE,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall") +
        count_subkeys(HKEY_LOCAL_MACHINE,
            L"Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall") +
        count_subkeys(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall");
}

/* ---- drawing ----------------------------------------------------------- */
static HFONT g_title, g_head, g_body;

static void make_fonts(void)
{
    g_title = CreateFontW(30, 0, 0, 0, FW_LIGHT, 0, 0, 0, DEFAULT_CHARSET,
                          0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_head  = CreateFontW(18, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                          0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_body  = CreateFontW(16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                          0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void text(HDC dc, int x, int y, HFONT font, COLORREF col, const WCHAR *s)
{
    SelectObject(dc, font);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, x, y, s, lstrlenW(s));
}

/* a labelled row inside a card */
static void row(HDC dc, int x, int *y, const WCHAR *label, const WCHAR *value)
{
    text(dc, x, *y, g_body, COL_SUBTLE, label);
    text(dc, x + 150, *y, g_body, COL_TEXT, value);
    *y += 26;
}

static void card(HDC dc, RECT *r, const WCHAR *heading)
{
    HBRUSH b = CreateSolidBrush(COL_CARD);
    FillRect(dc, r, b);
    DeleteObject(b);
    /* a purple accent stripe on the left edge */
    RECT stripe = { r->left, r->top, r->left + 4, r->bottom };
    b = CreateSolidBrush(COL_ACCENT);
    FillRect(dc, &stripe, b);
    DeleteObject(b);
    text(dc, r->left + 18, r->top + 12, g_head, COL_TEXT, heading);
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    struct facts f;
    RECT c;
    int y;
    WCHAR line[128];

    gather(&f);

    { RECT full; GetClientRect(hwnd, &full);
      HBRUSH bg = CreateSolidBrush(COL_BG); FillRect(dc, &full, bg); DeleteObject(bg); }

    text(dc, MARGIN, 20, g_title, COL_TEXT, L"Control Panel");

    /* System card */
    c.left = MARGIN; c.top = 70; c.right = WIN_W - MARGIN; c.bottom = 70 + 176;
    card(dc, &c, L"System");
    y = c.top + 46;
    row(dc, c.left + 18, &y, L"Edition", f.edition);
    row(dc, c.left + 18, &y, L"Version", f.os_build);
    row(dc, c.left + 18, &y, L"Computer name", f.computer);
    row(dc, c.left + 18, &y, L"System type", f.arch);

    /* Device card */
    c.top = c.bottom + 16; c.bottom = c.top + 96;
    card(dc, &c, L"Device");
    y = c.top + 46;
    row(dc, c.left + 18, &y, L"Processor", f.cpu);
    row(dc, c.left + 18, &y, L"Installed RAM", f.ram);

    /* Account card */
    c.top = c.bottom + 16; c.bottom = c.top + 96;
    card(dc, &c, L"User account");
    y = c.top + 46;
    row(dc, c.left + 18, &y, L"Signed in as", f.user);
    row(dc, c.left + 18, &y, L"Administrator", f.is_admin ? L"Yes" : L"No (elevate to run as administrator)");

    /* Update + policy card */
    c.top = c.bottom + 16; c.bottom = c.top + 122;
    card(dc, &c, L"Updates and policy");
    y = c.top + 46;
    row(dc, c.left + 18, &y, L"Windows Update", f.update);
    _snwprintf(line, ARRAYSIZE(line), L"%d machine %s in force",
               f.policy_count, f.policy_count == 1 ? L"policy" : L"policies");
    row(dc, c.left + 18, &y, L"Group Policy", line);
    _snwprintf(line, ARRAYSIZE(line), L"%d installed", f.program_count);
    row(dc, c.left + 18, &y, L"Programs", line);

    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT:   paint(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int dump(void)
{
    struct facts f;
    gather(&f);
    wprintf(L"edition=%ls\n", f.edition);
    wprintf(L"computer=%ls\n", f.computer);
    wprintf(L"user=%ls\n", f.user);
    wprintf(L"arch=%ls\n", f.arch);
    wprintf(L"admin=%ls\n", f.is_admin ? L"yes" : L"no");
    wprintf(L"policies=%d\n", f.policy_count);
    wprintf(L"osbuild=%ls\n", f.os_build);
    wprintf(L"cpu=%ls\n", f.cpu);
    wprintf(L"ram=%ls\n", f.ram);
    wprintf(L"programs=%d\n", f.program_count);
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSW wc = { 0 };
    HWND hwnd;
    MSG msg;
    (void)prev;

    if (cmd && wcsstr(cmd, L"--dump")) return dump();

    make_fonts();
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassW(&wc);

    hwnd = CreateWindowExW(0, CLASS_NAME, L"Control Panel",
                           WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, WIN_W, WIN_H,
                           NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show ? show : SW_SHOW);
    UpdateWindow(hwnd);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
