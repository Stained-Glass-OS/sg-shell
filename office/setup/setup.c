/*
 * SG Office -- Get SG Office (sg-office-setup64.exe).
 *
 * SG Office is LibreOffice, The Document Foundation's office suite, set up
 * to work like the suite people know: Microsoft's file formats by default,
 * Excel's formula syntax, a ribbon, Calibri-sized templates, and the Excel
 * functions its engine lacks (SG Office Functions). We never redistribute
 * LibreOffice: when the user asks for it, this downloads The Document
 * Foundation's own Windows installer, checks it is exactly their published
 * file (SHA-256, pinned in payload/office.ini), installs it silently for all
 * users and puts our settings on top ("the payload", installed by the
 * sg-office package in /usr/share/sg-office/payload, reached through Z:).
 *
 *   sg-office-setup64.exe                  the window: what it is, Install
 *   sg-office-setup64.exe /install [/msi FILE] [/quiet]
 *                                          download (or FILE), check, install, apply
 *   sg-office-setup64.exe /apply           apply the payload again (after an update)
 *   sg-office-setup64.exe /status          exit 0 when installed with the current payload
 *
 * Installing and applying need an administrator: the program runs itself
 * elevated for them (one consent prompt). SG_OFFICE_PAYLOAD may name another
 * payload directory (a gate's).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "setup.h"

static HWND g_main, g_text, g_bar, g_install, g_cancel;
static WCHAR g_msi[MAX_PATH], g_err[1024];
static volatile LONG g_cancelled;
static BOOL g_quiet;

/* ---- the payload's description ------------------------------------------------------------------ */

BOOL payload_dir(WCHAR *out, int cch)
{
    if (GetEnvironmentVariableW(L"SG_OFFICE_PAYLOAD", out, cch) && out[0]) return TRUE;
    lstrcpynW(out, L"Z:\\usr\\share\\sg-office\\payload", cch);
    return TRUE;
}

void payload_value(const WCHAR *key, WCHAR *out, int cch)
{
    WCHAR ini[MAX_PATH];
    payload_dir(ini, MAX_PATH);
    lstrcatW(ini, L"\\office.ini");
    GetPrivateProfileStringW(L"LibreOffice", key, L"", out, cch, ini);
}

/* ---- where LibreOffice is --------------------------------------------------------------------------- */

/* the program folder ("C:\Program Files\LibreOffice\program"), empty when not installed */
BOOL office_program_dir(WCHAR *out, int cch)
{
    DWORD cb = cch * sizeof(WCHAR);
    WCHAR soffice[MAX_PATH + 16];
    out[0] = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"Software\\LibreOffice\\UNO\\InstallPath", NULL,
                     RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, NULL, out, &cb) || !out[0]) {
        ExpandEnvironmentStringsW(L"%ProgramFiles%\\LibreOffice\\program", out, cch);
    }
    swprintf(soffice, ARRAYSIZE(soffice), L"%ls\\soffice.exe", out);
    if (GetFileAttributesW(soffice) == INVALID_FILE_ATTRIBUTES) { out[0] = 0; return FALSE; }
    return TRUE;
}

/* the payload's version, and the one applied to this installation */
static void stamp_path(const WCHAR *program, WCHAR *out, int cch)
{
    swprintf(out, cch, L"%ls\\..\\share\\registry\\sg-office.stamp", program);
}

BOOL payload_current(void)
{
    WCHAR program[MAX_PATH], stamp[MAX_PATH], want[64], have[64] = L"";
    DWORD got = 0;
    HANDLE f;
    char buf[64] = "";
    if (!office_program_dir(program, MAX_PATH)) return FALSE;
    payload_value(L"Payload", want, ARRAYSIZE(want));
    stamp_path(program, stamp, MAX_PATH);
    f = CreateFileW(stamp, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    ReadFile(f, buf, sizeof(buf) - 1, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;
    MultiByteToWideChar(CP_UTF8, 0, buf, -1, have, ARRAYSIZE(have));
    return !lstrcmpW(have, want);
}

/* ---- applying the payload ----------------------------------------------------------------------------- */

static BOOL copy_tree(const WCHAR *from, const WCHAR *to)
{
    WCHAR pat[MAX_PATH], a[MAX_PATH], b[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    BOOL ok = TRUE;
    CreateDirectoryW(to, NULL);
    swprintf(pat, MAX_PATH, L"%ls\\*", from);
    if ((h = FindFirstFileW(pat, &fd)) == INVALID_HANDLE_VALUE) return FALSE;
    do {
        if (!lstrcmpW(fd.cFileName, L".") || !lstrcmpW(fd.cFileName, L"..")) continue;
        swprintf(a, MAX_PATH, L"%ls\\%ls", from, fd.cFileName);
        swprintf(b, MAX_PATH, L"%ls\\%ls", to, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok &= copy_tree(a, b);
        else ok &= CopyFileW(a, b, FALSE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

static void delete_tree(const WCHAR *dir)
{
    WCHAR pat[MAX_PATH], p[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    swprintf(pat, MAX_PATH, L"%ls\\*", dir);
    if ((h = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE) {
        do {
            if (!lstrcmpW(fd.cFileName, L".") || !lstrcmpW(fd.cFileName, L"..")) continue;
            swprintf(p, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(p);
            else { SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL); DeleteFileW(p); }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* a file:/// URL for a Windows path, as LibreOffice's settings hold them */
static void file_url(const WCHAR *path, char *out, int cch)
{
    WCHAR full[MAX_PATH];
    char u8[MAX_PATH * 3];
    int i, n = 0;
    GetFullPathNameW(path, MAX_PATH, full, NULL);
    WideCharToMultiByte(CP_UTF8, 0, full, -1, u8, sizeof(u8), NULL, NULL);
    n = snprintf(out, cch, "file:///");
    for (i = 0; u8[i] && n < cch - 4; i++) {
        unsigned char c = (unsigned char)u8[i];
        if (c == '\\') out[n++] = '/';
        else if (isalnum(c) || strchr("-._~/:", c)) out[n++] = c;
        else n += snprintf(out + n, cch - n, "%%%02X", c);
    }
    out[n] = 0;
}

static char *read_all(const WCHAR *path, DWORD *len)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    char *buf;
    if (f == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(f, NULL);
    if (!(buf = malloc(size + 1))) { CloseHandle(f); return NULL; }
    ReadFile(f, buf, size, &got, NULL);
    CloseHandle(f);
    buf[got] = 0;
    if (len) *len = got;
    return buf;
}

static BOOL write_all(const WCHAR *path, const char *data, DWORD len)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD done = 0;
    BOOL ok;
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(f, data, len, &done, NULL) && done == len;
    CloseHandle(f);
    return ok;
}

/* The payload into the installation: our defaults (sg-office.xcd, with the
 * templates' place filled in), the templates, the SG Office Functions
 * extension (as a bundled one: every user gets it), and a stamp. LibreOffice's
 * own Start menu folder goes: Start lists SG Office's three programs. */
BOOL payload_apply(WCHAR *err, int cch)
{
    WCHAR program[MAX_PATH], share[MAX_PATH], src[MAX_PATH], dst[MAX_PATH], pay[MAX_PATH], version[64];
    char url[MAX_PATH * 3], *xcd, *out, *at, stamp[64];
    DWORD len;
    if (!office_program_dir(program, MAX_PATH)) { swprintf(err, cch, L"LibreOffice is not installed."); return FALSE; }
    payload_dir(pay, MAX_PATH);
    swprintf(share, MAX_PATH, L"%ls\\..\\share", program);

    /* templates */
    swprintf(src, MAX_PATH, L"%ls\\templates", pay);
    swprintf(dst, MAX_PATH, L"%ls\\template\\sg-office", share);
    if (!copy_tree(src, dst)) { swprintf(err, cch, L"Could not copy the templates to %ls.", dst); return FALSE; }

    /* defaults, with the templates' folder as a URL */
    swprintf(src, MAX_PATH, L"%ls\\sg-office.xcd.in", pay);
    if (!(xcd = read_all(src, &len))) { swprintf(err, cch, L"%ls is missing.", src); return FALSE; }
    file_url(dst, url, sizeof(url));
    if (!(out = malloc(len + 16 * sizeof(url)))) { free(xcd); return FALSE; }
    out[0] = 0;
    {
        char *p = xcd;
        while ((at = strstr(p, "file://@TEMPLATEDIR@"))) {
            strncat(out, p, at - p);
            strcat(out, url);
            p = at + strlen("file://@TEMPLATEDIR@");
        }
        strcat(out, p);
    }
    swprintf(dst, MAX_PATH, L"%ls\\registry\\sg-office.xcd", share);
    if (!write_all(dst, out, (DWORD)strlen(out))) {
        swprintf(err, cch, L"Could not write %ls.", dst);
        free(xcd); free(out);
        return FALSE;
    }
    free(xcd); free(out);

    /* the spreadsheet ribbon (Home as Excel's), beside LibreOffice's own */
    swprintf(src, MAX_PATH, L"%ls\\ui\\scalc\\notebookbar_sgoffice.ui", pay);
    swprintf(dst, MAX_PATH, L"%ls\\config\\soffice.cfg\\modules\\scalc\\ui\\notebookbar_sgoffice.ui", share);
    if (!CopyFileW(src, dst, FALSE)) { swprintf(err, cch, L"Could not copy the spreadsheet ribbon to %ls.", dst); return FALSE; }

    /* the functions, as a bundled extension (a fresh copy: files may have gone) */
    swprintf(src, MAX_PATH, L"%ls\\extensions\\sg-office-functions", pay);
    swprintf(dst, MAX_PATH, L"%ls\\extensions\\sg-office-functions", share);
    delete_tree(dst);
    if (!copy_tree(src, dst)) { swprintf(err, cch, L"Could not copy SG Office Functions to %ls.", dst); return FALSE; }

    /* LibreOffice's own Start menu entries: ours stand for them */
    {
        WCHAR menu[MAX_PATH], pat[MAX_PATH], p[MAX_PATH];
        WIN32_FIND_DATAW fd;
        HANDLE h;
        if (SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, 0, menu) == S_OK) {
            swprintf(pat, MAX_PATH, L"%ls\\LibreOffice*", menu);
            if ((h = FindFirstFileW(pat, &fd)) != INVALID_HANDLE_VALUE) {
                do {
                    swprintf(p, MAX_PATH, L"%ls\\%ls", menu, fd.cFileName);
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) delete_tree(p);
                    else DeleteFileW(p);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
    }

    /* the stamp: which payload this installation has */
    payload_value(L"Payload", version, ARRAYSIZE(version));
    WideCharToMultiByte(CP_UTF8, 0, version, -1, stamp, sizeof(stamp), NULL, NULL);
    stamp_path(program, dst, MAX_PATH);
    if (!write_all(dst, stamp, (DWORD)strlen(stamp))) { swprintf(err, cch, L"Could not write %ls.", dst); return FALSE; }
    return TRUE;
}

/* ---- installing ---------------------------------------------------------------------------------------- */

static BOOL is_admin(void)
{
    BOOL admin = FALSE;
    PSID group;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(NULL, group, &admin);
        FreeSid(group);
    }
    return admin;
}

static BOOL run_wait(const WCHAR *file, const WCHAR *args, BOOL elevate, DWORD *code)
{
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = elevate ? L"runas" : NULL;
    sei.lpFile = file;
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return FALSE;
    WaitForSingleObject(sei.hProcess, INFINITE);
    GetExitCodeProcess(sei.hProcess, code);
    CloseHandle(sei.hProcess);
    return TRUE;
}

/* As an administrator: the MSI silently for all users, then the payload. */
static int install_elevated(const WCHAR *msi)
{
    WCHAR sys[MAX_PATH], args[2048], props[1024], log[MAX_PATH];
    DWORD code = 1;
    GetSystemDirectoryW(sys, MAX_PATH);
    lstrcatW(sys, L"\\msiexec.exe");
    payload_value(L"MsiProperties", props, ARRAYSIZE(props));
    GetTempPathW(MAX_PATH, log);
    lstrcatW(log, L"sg-office-install.log");
    swprintf(args, ARRAYSIZE(args), L"/i \"%ls\" /qn /norestart /l*v \"%ls\" %ls", msi, log, props);
    if (!run_wait(sys, args, FALSE, &code)) return 2;
    if (code != 0 && code != 3010 && code != 1641) return (int)(code ? code : 1);
    return payload_apply(g_err, ARRAYSIZE(g_err)) ? 0 : 3;
}

/* Run ourselves as an administrator for VERB (one consent prompt), unless we are one. */
int elevated(const WCHAR *verb, const WCHAR *arg)
{
    WCHAR self[MAX_PATH], args[MAX_PATH + 64];
    DWORD code = 1;
    if (is_admin() || GetEnvironmentVariableW(L"SG_OFFICE_NO_ELEVATE", NULL, 0)) {
        if (!lstrcmpW(verb, L"/install-msi")) return install_elevated(arg);
        return payload_apply(g_err, ARRAYSIZE(g_err)) ? 0 : 3;
    }
    GetModuleFileNameW(NULL, self, MAX_PATH);
    swprintf(args, ARRAYSIZE(args), arg ? L"%ls \"%ls\"" : L"%ls", verb, arg);
    if (!run_wait(self, args, TRUE, &code)) return 1223;   /* ERROR_CANCELLED: the prompt was refused */
    return (int)code;
}

/* ---- downloading ------------------------------------------------------------------------------------------ */

static BOOL hex32(const WCHAR *hex, BYTE out[32])
{
    int i;
    if (lstrlenW(hex) != 64) return FALSE;
    for (i = 0; i < 32; i++) {
        WCHAR b[3] = { hex[2 * i], hex[2 * i + 1], 0 }, *end;
        out[i] = (BYTE)wcstoul(b, &end, 16);
        if (*end) return FALSE;
    }
    return TRUE;
}

/* The SHA-256 of a file (a local installer, or the download as written). */
static BOOL sha256_file(const WCHAR *path, BYTE digest[32])
{
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    static BYTE buf[1 << 16];
    DWORD got;
    BOOL ok = FALSE;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    if (!BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) && !BCryptCreateHash(alg, &hash, NULL, 0, NULL, 0, 0)) {
        while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got) BCryptHashData(hash, buf, got, 0);
        ok = !BCryptFinishHash(hash, digest, 32, 0);
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}

BOOL verify(const WCHAR *file, WCHAR *err, int cch)
{
    WCHAR want_hex[80];
    BYTE want[32], got[32];
    payload_value(L"Sha256", want_hex, ARRAYSIZE(want_hex));
    if (!hex32(want_hex, want)) { swprintf(err, cch, L"office.ini has no valid SHA-256."); return FALSE; }
    if (!sha256_file(file, got)) { swprintf(err, cch, L"Could not read %ls.", file); return FALSE; }
#ifdef SG_MUTANT_NOHASH
    memcpy(got, want, 32);
#endif
    if (memcmp(want, got, 32)) {
        swprintf(err, cch, L"The download is not the file The Document Foundation published (its SHA-256 does not "
                           L"match), so it was not installed.");
        return FALSE;
    }
    return TRUE;
}

static void progress(ULONGLONG done, ULONGLONG total)
{
    if (g_bar && total) PostMessageW(g_bar, PBM_SETPOS, (WPARAM)(done * 1000 / total), 0);
}

BOOL download(WCHAR *out, int cch_out, WCHAR *err, int cch)
{
    WCHAR url[1024], dir[MAX_PATH];
    HINTERNET net, h;
    HANDLE f;
    static BYTE buf[1 << 16];
    DWORD got, w, total = 0, len = sizeof(total), status = 0, slen = sizeof(status);
    ULONGLONG done = 0;
    payload_value(L"Url", url, ARRAYSIZE(url));
    if (_wcsnicmp(url, L"https://", 8)) { swprintf(err, cch, L"office.ini has no https download address."); return FALSE; }
    GetTempPathW(MAX_PATH, dir);
    swprintf(out, cch_out, L"%lsSG-Office-LibreOffice.msi", dir);
    if (!(net = InternetOpenW(L"StainedGlass-SGOffice/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0))) return FALSE;
    if (!(h = InternetOpenUrlW(net, url, NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0))) {
        swprintf(err, cch, L"Can't reach The Document Foundation's download server (error %lu). Check your internet connection.",
                 GetLastError());
        InternetCloseHandle(net);
        return FALSE;
    }
    if (HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &slen, NULL) && status != 200) {
        swprintf(err, cch, L"The download server answered %lu.", status);
        InternetCloseHandle(h); InternetCloseHandle(net);
        return FALSE;
    }
    if (!HttpQueryInfoW(h, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &total, &len, NULL)) total = 0;
    f = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { InternetCloseHandle(h); InternetCloseHandle(net); swprintf(err, cch, L"Can't write %ls.", out); return FALSE; }
    for (;;) {
        if (g_cancelled) { swprintf(err, cch, L"Cancelled."); break; }
        if (!InternetReadFile(h, buf, sizeof(buf), &got)) { swprintf(err, cch, L"The download broke off. Check your internet connection."); got = (DWORD)-1; break; }
        if (!got) break;
        if (!WriteFile(f, buf, got, &w, NULL) || w != got) { swprintf(err, cch, L"Can't write %ls (is the disk full?).", out); got = (DWORD)-1; break; }
        done += got;
        progress(done, total);
    }
    CloseHandle(f);
    InternetCloseHandle(h);
    InternetCloseHandle(net);
    if (g_cancelled || got == (DWORD)-1) { DeleteFileW(out); return FALSE; }
    return TRUE;
}

/* ---- the whole job, on a worker thread when there is a window ------------------------------------------------ */

int do_install(void)
{
    WCHAR file[MAX_PATH];
    BOOL downloaded = FALSE;
    int rc;
    g_err[0] = 0;
    if (g_msi[0]) lstrcpynW(file, g_msi, MAX_PATH);
    else {
        if (g_main) SetWindowTextW(g_text, L"Downloading LibreOffice from The Document Foundation...");
        if (!download(file, MAX_PATH, g_err, ARRAYSIZE(g_err))) return 1;
        downloaded = TRUE;
    }
    if (g_main) SetWindowTextW(g_text, L"Checking the download...");
    if (!verify(file, g_err, ARRAYSIZE(g_err))) { if (downloaded) DeleteFileW(file); return 1; }
    if (g_main) {
        SetWindowTextW(g_text, L"Installing SG Office. This takes a few minutes...");
        SendMessageW(g_bar, PBM_SETMARQUEE, TRUE, 30);
    }
    rc = elevated(L"/install-msi", file);
    if (downloaded) DeleteFileW(file);
    if (rc == 1223) swprintf(g_err, ARRAYSIZE(g_err), L"Installing needs an administrator's permission.");
    else if (rc == 3 && !g_err[0]) swprintf(g_err, ARRAYSIZE(g_err), L"LibreOffice was installed, but SG Office's settings could not be applied.");
    else if (rc && !g_err[0]) swprintf(g_err, ARRAYSIZE(g_err), L"The installer stopped with code %d.", rc);
    return rc;
}

/* ---- the window ---------------------------------------------------------------------------------------------- */

enum { ID_INSTALL = 100, WM_DONE = WM_APP + 1 };

static DWORD WINAPI worker(void *arg)
{
    (void)arg;
    PostMessageW(g_main, WM_DONE, do_install(), 0);
    return 0;
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == ID_INSTALL) {
            EnableWindow(g_install, FALSE);
            ShowWindow(g_bar, SW_SHOW);
            CloseHandle(CreateThread(NULL, 0, worker, NULL, 0, NULL));
        } else if (LOWORD(wp) == IDCANCEL) {
            InterlockedExchange(&g_cancelled, 1);
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_DONE:
        if (wp == 0) {
            SetWindowTextW(g_text, L"SG Office is installed. Find it in Start: SG Office Documents, Spreadsheets and Presentations.");
            SendMessageW(g_bar, PBM_SETMARQUEE, FALSE, 0);
            SendMessageW(g_bar, PBM_SETPOS, 1000, 0);
            SetWindowTextW(g_cancel, L"Close");
            PostQuitMessage(0);
            return 0;
        }
        SetWindowTextW(g_text, g_err);
        ShowWindow(g_bar, SW_HIDE);
        EnableWindow(g_install, TRUE);
        SetWindowTextW(g_install, L"Try again");
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode((HDC)wp, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DESTROY:
        PostQuitMessage(1);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int window(HINSTANCE inst)
{
    WNDCLASSW wc = { 0 };
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    HFONT font, big;
    HWND title;
    MSG m;
    WCHAR version[32], text[1024];
    int dpi, rc = 1;
    HDC dc = GetDC(NULL);
    dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
#define S(x) MulDiv((x), dpi, 96)
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -S(22);
    big = CreateFontIndirectW(&ncm.lfMessageFont);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = L"SgOfficeSetup";
    RegisterClassW(&wc);
    g_main = CreateWindowExW(0, wc.lpszClassName, L"Get SG Office", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                             CW_USEDEFAULT, CW_USEDEFAULT, S(560), S(330), NULL, NULL, inst, NULL);
    payload_value(L"Version", version, ARRAYSIZE(version));
    title = CreateWindowW(L"STATIC", L"SG Office", WS_CHILD | WS_VISIBLE, S(24), S(18), S(500), S(34), g_main, NULL, inst, NULL);
    swprintf(text, ARRAYSIZE(text),
             L"Documents, spreadsheets and presentations, opening and saving Microsoft Office's file formats, "
             L"with Excel's formulas and a familiar ribbon.\n\nSG Office is LibreOffice, the free office suite made by "
             L"The Document Foundation, set up for Stained Glass OS. Install downloads LibreOffice %ls (about 360 MB) "
             L"from The Document Foundation, checks it is exactly the file they published, and installs it for everyone "
             L"on this computer.", version);
    g_text = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, S(24), S(60), S(500), S(150), g_main, NULL, inst, NULL);
    g_bar = CreateWindowW(PROGRESS_CLASSW, NULL, WS_CHILD | PBS_SMOOTH | PBS_MARQUEE, S(24), S(218), S(500), S(16), g_main, NULL, inst, NULL);
    SendMessageW(g_bar, PBM_SETRANGE32, 0, 1000);
    g_install = CreateWindowW(L"BUTTON", L"Install", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              S(314), S(248), S(100), S(30), g_main, (HMENU)ID_INSTALL, inst, NULL);
    g_cancel = CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                             S(424), S(248), S(100), S(30), g_main, (HMENU)IDCANCEL, inst, NULL);
    SendMessageW(title, WM_SETFONT, (WPARAM)big, 0);
    SendMessageW(g_text, WM_SETFONT, (WPARAM)font, 0);
    SendMessageW(g_install, WM_SETFONT, (WPARAM)font, 0);
    SendMessageW(g_cancel, WM_SETFONT, (WPARAM)font, 0);
    ShowWindow(g_main, SW_SHOWNORMAL);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(g_main, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    rc = (int)m.wParam;
    if (rc == 0) {
        /* installed: say so until the user closes the window */
        while (IsWindow(g_main) && GetMessageW(&m, NULL, 0, 0) > 0)
            if (!IsDialogMessageW(g_main, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    return rc;
#undef S
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    WCHAR **argv;
    int argc, i, rc;
    enum { WINDOW, INSTALL, APPLY, STATUS, INSTALL_MSI } mode = WINDOW;
    const WCHAR *msi_arg = NULL;
    (void)prev; (void)cmdline; (void)show;
    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (i = 1; argv && i < argc; i++) {
        if (!_wcsicmp(argv[i], L"/install")) mode = INSTALL;
        else if (!_wcsicmp(argv[i], L"/apply")) mode = APPLY;
        else if (!_wcsicmp(argv[i], L"/status")) mode = STATUS;
        else if (!_wcsicmp(argv[i], L"/quiet")) g_quiet = TRUE;
        else if (!_wcsicmp(argv[i], L"/msi") && i + 1 < argc) lstrcpynW(g_msi, argv[++i], MAX_PATH);
        else if (!_wcsicmp(argv[i], L"/install-msi") && i + 1 < argc) { mode = INSTALL_MSI; msi_arg = argv[++i]; }
    }
    switch (mode) {
    case STATUS:
        return payload_current() ? 0 : 1;
    case APPLY:
        rc = elevated(L"/apply", NULL);
        if (rc && !g_quiet) MessageBoxW(NULL, g_err[0] ? g_err : L"SG Office's settings could not be applied.", L"SG Office", MB_OK | MB_ICONERROR);
        return rc;
    case INSTALL_MSI:   /* the elevated half */
        return install_elevated(msi_arg);
    case INSTALL:
        rc = do_install();
        if (rc && !g_quiet) MessageBoxW(NULL, g_err, L"Get SG Office", MB_OK | MB_ICONERROR);
        else if (rc && g_err[0]) {
            char e[1024];
            WideCharToMultiByte(CP_UTF8, 0, g_err, -1, e, sizeof(e), NULL, NULL);
            fprintf(stderr, "sg-office-setup: %s\n", e);
        }
        return rc;
    default:
        InitCommonControlsEx(&icc);
        return window(inst);
    }
}
