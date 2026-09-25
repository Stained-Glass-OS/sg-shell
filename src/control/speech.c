/* sg-control -- Speech Recognition: voice typing's settings.
 *
 * Voice typing is sg-dictate (the toolbar, Win+H) over sg-session's engine:
 * Parakeet on this computer's CPU. Its speech model is downloaded once per
 * computer by sg-speechd when someone first turns voice typing on; this page
 * starts that (`sg-dictate --download`) and shows its progress from the
 * status file sg-speechd keeps. "Test microphone" runs `sg-dictate --meter`,
 * which writes only a level to a file of ours. Wine gives a Windows program
 * no pipes to a native one, so files are the way back.
 *
 * Settings, HKCU\Software\Stained Glass\Speech (the toolbar reads them each
 * time it starts listening):
 *   Enabled, Continuous, AutoPunctuation, SpokenPunctuation, RemoveFillers,
 *   FormatNumbers, HoldToTalk (DWORD 0/1), HoldKey (DWORD, a virtual key),
 *   InsertMethod (DWORD, 0 typing, 1 the clipboard), Microphone (SZ, a
 *   PipeWire source; empty for the default), Language (SZ, en-US, de-DE,
 *   fr-FR, es-ES or auto: the language of spoken punctuation, commands and
 *   filler words; the model itself recognises 25 languages either way).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"
#include <shellapi.h>

#define SPEECH_KEY L"Software\\Stained Glass\\Speech"
#define MODEL_BYTES 672807563ULL

enum {
    CMD_ENABLED = CMD_PAGE_FIRST + 1, CMD_RETRY, CMD_MIC, CMD_TEST, CMD_HOLD, CMD_HOLDKEY,
    CMD_CONTINUOUS, CMD_AUTOPUNCT, CMD_SPOKEN, CMD_FILLERS, CMD_NUMBERS,
    CMD_TYPE, CMD_PASTE, CMD_LANG, CMD_TRY,
};

static const struct { const WCHAR *name; DWORD vk; } HOLD_KEYS[] = {
    { L"Right Ctrl", VK_RCONTROL }, { L"Right Alt", VK_RMENU }, { L"Right Shift", VK_RSHIFT },
    { L"Scroll Lock", VK_SCROLL }, { L"Pause", VK_PAUSE }, { L"F12", VK_F12 },
};
static const struct { const WCHAR *name, *code; } LANGS[] = {
    { L"English (United States)", L"en-US" },
    { L"Deutsch (Deutschland)", L"de-DE" },
    { L"Fran\x00e7ais (France)", L"fr-FR" },
    { L"Espa\x00f1ol (Espa\x00f1a)", L"es-ES" },
    { L"Detect automatically (25 European languages)", L"auto" },
};

#define MAX_MICS 32
static WCHAR g_mic_name[MAX_MICS][256], g_mic_desc[MAX_MICS][256];
static int g_nmics = -1;
static WCHAR g_default_mic[256];
static HWND g_status, g_progress, g_meter, g_test_btn, g_retry;
static BOOL g_testing, g_downloading;
static char g_meter_file[MAX_PATH];
static DWORD g_test_started;

/* ---- the machine's side ------------------------------------------------------------ */

static const char *speech_dir(void)
{
    static char dir[MAX_PATH];
    if (!dir[0] && !GetEnvironmentVariableA("SG_SPEECH_DIR", dir, MAX_PATH))
        lstrcpyA(dir, "/var/lib/stained-glass-speech");
    return dir;
}

enum model_state { MS_MISSING, MS_INSTALLED, MS_DOWNLOADING, MS_FAILED };

static enum model_state model_state(ULONGLONG *done, ULONGLONG *total, WCHAR *msg, int cch)
{
    char path[MAX_PATH + 64], *text, *line, *next;
    enum model_state st = MS_MISSING;
    *done = 0; *total = MODEL_BYTES;
    if (msg) msg[0] = 0;
    /* the sg-speech-model-parakeet package's copy, then sg-speechd's download */
    if (!GetEnvironmentVariableA("SG_SPEECH_PACKAGED_DIR", path, MAX_PATH))
        lstrcpyA(path, "/usr/share/stained-glass-speech");
    lstrcatA(path, "/parakeet-tdt-0.6b-v3-int8/.verified");
    if (unix_path_exists(path)) { *done = *total; return MS_INSTALLED; }
    _snprintf(path, sizeof(path), "%s/parakeet-tdt-0.6b-v3-int8/.verified", speech_dir());
    if (unix_path_exists(path)) { *done = *total; return MS_INSTALLED; }
    _snprintf(path, sizeof(path), "%s/status", speech_dir());
    if (!(text = read_unix_file(path, NULL))) return MS_MISSING;
    for (line = text; line && *line; line = next) {
        if ((next = strchr(line, '\n'))) *next++ = 0;
        if (!strncmp(line, "STATE ", 6)) {
            if (!strcmp(line + 6, "downloading")) st = MS_DOWNLOADING;
            else if (!strcmp(line + 6, "failed")) st = MS_FAILED;
            else if (!strcmp(line + 6, "installed")) st = MS_INSTALLED;
        }
        else if (!strncmp(line, "DONE ", 5)) *done = _strtoui64(line + 5, NULL, 10);
        else if (!strncmp(line, "TOTAL ", 6)) *total = _strtoui64(line + 6, NULL, 10);
        else if (!strncmp(line, "MESSAGE ", 8) && msg) MultiByteToWideChar(CP_UTF8, 0, line + 8, -1, msg, cch);
    }
    free(text);
    if (!*total) *total = MODEL_BYTES;
    return st;
}

/* Run sg-dictate with arguments (a native program: no pipes, no exit code). */
static BOOL run_engine(const WCHAR *args)
{
    WCHAR unix_path[MAX_PATH] = L"/usr/bin/sg-dictate", cmd[2048], *p;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    GetEnvironmentVariableW(L"SG_DICTATE", unix_path, MAX_PATH);
    _snwprintf(cmd, ARRAYSIZE(cmd), L"\\\\?\\unix%ls %ls", unix_path, args);
    cmd[ARRAYSIZE(cmd) - 1] = 0;
    for (p = cmd + 8; *p && *p != L' '; p++) if (*p == L'/') *p = L'\\';
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return FALSE;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}

/* A file of ours in %TEMP%, and its Unix name for the engine. */
static BOOL temp_file(const WCHAR *name, WCHAR *dos, char *unix_out, int cch)
{
    static char *(CDECL *to_unix)(const WCHAR *);
    WCHAR dir[MAX_PATH];
    char *u;
    if (!to_unix) to_unix = (void *)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "wine_get_unix_file_name");
    if (!to_unix || !GetTempPathW(MAX_PATH, dir)) return FALSE;
    _snwprintf(dos, MAX_PATH, L"%ls%ls", dir, name);
    dos[MAX_PATH - 1] = 0;
    CloseHandle(CreateFileW(dos, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
    if (!(u = to_unix(dos))) return FALSE;
    lstrcpynA(unix_out, u, cch);
    HeapFree(GetProcessHeap(), 0, u);
    return TRUE;
}

static char *read_dos_file(const WCHAR *path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    char *buf;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(h, NULL);
    if (size > 1 << 20 || !(buf = malloc(size + 1))) { CloseHandle(h); return NULL; }
    ReadFile(h, buf, size, &got, NULL);
    CloseHandle(h);
    buf[got] = 0;
    return buf;
}

/* The microphones, from `sg-dictate --mics` (asked once per visit to the page). */
static void load_mics(void)
{
    WCHAR dos[MAX_PATH], args[MAX_PATH + 64];
    char unix_path[MAX_PATH], *text = NULL, *line, *next;
    int i;
    g_nmics = 0;
    g_default_mic[0] = 0;
    if (!temp_file(L"sg-speech-mics.txt", dos, unix_path, MAX_PATH)) return;
    DeleteFileW(dos);
    _snwprintf(args, ARRAYSIZE(args), L"--mics --out %S", unix_path);
    if (!run_engine(args)) return;
    for (i = 0; i < 50 && !(text = read_dos_file(dos)); i++) Sleep(100);   /* up to 5 s */
    if (!text) return;
    for (line = text; line && *line; line = next) {
        if ((next = strchr(line, '\n'))) *next++ = 0;
        if (!strncmp(line, "MIC ", 4) && g_nmics < MAX_MICS) {
            char *tab = strchr(line + 4, '\t');
            if (tab) *tab++ = 0;
            MultiByteToWideChar(CP_UTF8, 0, line + 4, -1, g_mic_name[g_nmics], 256);
            MultiByteToWideChar(CP_UTF8, 0, tab ? tab : line + 4, -1, g_mic_desc[g_nmics], 256);
            g_nmics++;
        }
        else if (!strncmp(line, "DEFAULT ", 8)) MultiByteToWideChar(CP_UTF8, 0, line + 8, -1, g_default_mic, 256);
    }
    free(text);
    DeleteFileW(dos);
}

static DWORD setting(const WCHAR *name, DWORD def) { return reg_dword(HKEY_CURRENT_USER, SPEECH_KEY, name, def); }
static void set_setting(const WCHAR *name, DWORD v) { reg_set_dword(HKEY_CURRENT_USER, SPEECH_KEY, name, v); }

static const WCHAR *hold_key_name(DWORD vk)
{
    size_t i;
    for (i = 0; i < ARRAYSIZE(HOLD_KEYS); i++) if (HOLD_KEYS[i].vk == vk) return HOLD_KEYS[i].name;
    return L"Right Ctrl";
}

/* Tell the toolbar (running or not) the settings changed. */
static void tell_toolbar(const WCHAR *verb)
{
    ShellExecuteW(NULL, NULL, L"sg-dictate.exe", verb, NULL, SW_SHOWNOACTIVATE);
}

/* ---- the page ------------------------------------------------------------------------- */

static void status_text(WCHAR *out, int cch, BOOL *show_progress, BOOL *show_retry)
{
    ULONGLONG done, total;
    WCHAR size[32], got[32], msg[256];
    enum model_state st = model_state(&done, &total, msg, ARRAYSIZE(msg));
    format_size(total, size, ARRAYSIZE(size));
    *show_progress = *show_retry = FALSE;
    g_downloading = st == MS_DOWNLOADING;
    switch (st) {
    case MS_INSTALLED:
        _snwprintf(out, cch, L"The speech model is installed on this computer (%ls).", size);
        break;
    case MS_DOWNLOADING:
        format_size(done, got, ARRAYSIZE(got));
        _snwprintf(out, cch, L"Downloading the speech model\x2026 %ls of %ls", got, size);
        *show_progress = TRUE;
        break;
    case MS_FAILED:
        _snwprintf(out, cch, L"The speech model couldn't be downloaded: %ls", msg[0] ? msg : L"unknown error");
        *show_retry = TRUE;
        break;
    default:
        if (setting(L"Enabled", 0))
        {
            _snwprintf(out, cch, L"Voice typing needs its speech model, a one-time download of %ls.", size);
            *show_retry = TRUE;
        }
        else
            _snwprintf(out, cch, L"Turning voice typing on downloads its speech model once (%ls). "
                                 L"It stays on this computer, for everyone who uses it.", size);
    }
    out[cch - 1] = 0;
}

static void update_status(void)
{
    WCHAR text[400];
    BOOL progress, retry;
    ULONGLONG done, total;
    status_text(text, ARRAYSIZE(text), &progress, &retry);
    if (g_status) SetWindowTextW(g_status, text);
    if (g_progress) {
        model_state(&done, &total, NULL, 0);
        SendMessageW(g_progress, PBM_SETPOS, total ? (WPARAM)(done * 1000 / total) : 0, 0);
        ShowWindow(g_progress, progress ? SW_SHOWNA : SW_HIDE);
    }
    if (g_retry) ShowWindow(g_retry, retry ? SW_SHOWNA : SW_HIDE);
}

static HWND check(const WCHAR *label, int x, int y, int w, int id, DWORD value)
{
    HWND c = pg_control(L"BUTTON", label, WS_TABSTOP | BS_AUTOCHECKBOX, x, y, w, S(22), id);
    SendMessageW(c, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0);
    return c;
}

static int section(int x, int y, int w, const WCHAR *title)
{
    pg_text(x, y, w, S(24), g_font_cat, COL_TITLE, title, DT_SINGLELINE);
    pg_rule(x, y + S(26), w);
    return y + S(38);
}

void build_speech(void)
{
    static const WCHAR *const labels[] = { L"Set up voice typing", L"Set up a microphone", NULL,
                                           L"See also", L"Hardware and Sound" };
    static const int ids[] = { CMD_TRY, CMD_TEST, 0, -1, NAV(PG_CAT_HW) };
    int x = pg_left_pane(labels, ids, ARRAYSIZE(labels)) + S(36), y = S(24), w = pg_width() - x - S(40), i;
    WCHAR mic[256] = L"", lang[16] = L"en-US";
    HWND c;
    DWORD holdkey = setting(L"HoldKey", VK_RCONTROL);

    g_testing = FALSE;
    reg_sz(HKEY_CURRENT_USER, SPEECH_KEY, L"Microphone", mic, ARRAYSIZE(mic));
    reg_sz(HKEY_CURRENT_USER, SPEECH_KEY, L"Language", lang, ARRAYSIZE(lang));
    if (g_nmics < 0) load_mics();

    pg_title(x, y, L"Speech Recognition");
    y += S(44);
    pg_icon(x, y, S(48), IC_SPEECH);
    i = pg_para(x + S(64), y, w - S(64), g_font_body, COL_TEXT,
                 L"Dictate text anywhere you can type. Press the Windows logo key + H to start or stop, "
                 L"or select the microphone on the voice typing bar. Say \x201C" L"comma\x201D, \x201Cperiod\x201D, "
                 L"\x201Cquestion mark\x201D or \x201Cnew line\x201D for punctuation, and \x201C" L"delete that\x201D, "
                 L"\x201Cundo that\x201D or \x201Cstop listening\x201D to take something back or stop.");
    y += max(i, S(48)) + S(24);

    y = section(x, y, w, L"Voice typing");
    check(L"Turn on voice typing", x + S(16), y, w - S(16), CMD_ENABLED, setting(L"Enabled", 0));
    y += S(30);
    g_status = pg_control(L"STATIC", L"", SS_NOPREFIX, x + S(16), y, w - S(16), S(38), -1);
    y += S(40);
    g_progress = pg_control(PROGRESS_CLASSW, L"", 0, x + S(16), y, S(320), S(14), -1);
    SendMessageW(g_progress, PBM_SETRANGE32, 0, 1000);
    g_retry = pg_control(L"BUTTON", L"Download", WS_TABSTOP | BS_PUSHBUTTON, x + S(16), y - S(6), S(120), S(28), CMD_RETRY);
    update_status();
    y += S(36);

    y = section(x, y, w, L"Microphone");
    pg_text(x + S(16), y + S(4), S(120), S(20), g_font_body, COL_TEXT, L"Microphone:", DT_SINGLELINE);
    c = pg_control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x + S(140), y, S(360), S(240), CMD_MIC);
    SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)L"Default microphone");
    SendMessageW(c, CB_SETCURSEL, 0, 0);
    for (i = 0; i < g_nmics; i++) {
        WCHAR label[300];
        _snwprintf(label, ARRAYSIZE(label), L"%ls%ls", g_mic_desc[i],
                   !wcscmp(g_mic_name[i], g_default_mic) ? L" (default)" : L"");
        label[ARRAYSIZE(label) - 1] = 0;
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)label);
        if (!wcscmp(g_mic_name[i], mic)) SendMessageW(c, CB_SETCURSEL, i + 1, 0);
    }
    y += S(38);
    g_test_btn = pg_button(L"Test microphone", x + S(16), y, S(150), CMD_TEST);
    g_meter = pg_control(PROGRESS_CLASSW, L"", 0, x + S(180), y + S(7), S(320), S(14), -1);
    SendMessageW(g_meter, PBM_SETRANGE32, 0, 100);
    y += S(34);
    pg_text(x + S(16), y, w - S(16), S(20), g_font_body, COL_SUBTLE,
            g_nmics > 0 ? L"Speak, and the bar should move." : L"No microphone was found. Plug one in, then come back.",
            DT_SINGLELINE | DT_END_ELLIPSIS);
    y += S(36);

    y = section(x, y, w, L"Starting and stopping");
    pg_text(x + S(16), y, w - S(16), S(20), g_font_body, COL_TEXT,
            L"Windows logo key + H starts voice typing, and stops it again.", DT_SINGLELINE | DT_END_ELLIPSIS);
    y += S(30);
    check(L"Hold a key to talk:", x + S(16), y, S(200), CMD_HOLD, setting(L"HoldToTalk", 0));
    c = pg_control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x + S(220), y - S(2), S(160), S(200), CMD_HOLDKEY);
    for (i = 0; i < (int)ARRAYSIZE(HOLD_KEYS); i++) {
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)HOLD_KEYS[i].name);
        if (HOLD_KEYS[i].vk == holdkey) SendMessageW(c, CB_SETCURSEL, i, 0);
    }
    y += S(30);
    pg_text(x + S(36), y, w - S(36), S(20), g_font_body, COL_SUBTLE,
            L"Hold it down while you speak, and let go when you're done.", DT_SINGLELINE | DT_END_ELLIPSIS);
    y += S(36);

    y = section(x, y, w, L"Dictation");
    check(L"Keep listening until I stop it (continuous dictation)", x + S(16), y, w - S(16), CMD_CONTINUOUS, setting(L"Continuous", 1));
    y += S(28);
    check(L"Add punctuation automatically", x + S(16), y, w - S(16), CMD_AUTOPUNCT, setting(L"AutoPunctuation", 1));
    y += S(28);
    check(L"Type punctuation I say (\x201C" L"comma\x201D, \x201Cperiod\x201D, \x201Cnew line\x201D)", x + S(16), y, w - S(16),
          CMD_SPOKEN, setting(L"SpokenPunctuation", 1));
    y += S(28);
    check(L"Leave out filler words (\x201Cum\x201D, \x201Cuh\x201D)", x + S(16), y, w - S(16), CMD_FILLERS, setting(L"RemoveFillers", 1));
    y += S(28);
    check(L"Write numbers as digits (\x201Ctwenty three\x201D becomes 23)", x + S(16), y, w - S(16), CMD_NUMBERS, setting(L"FormatNumbers", 1));
    y += S(36);
    pg_text(x + S(16), y + S(4), S(120), S(20), g_font_body, COL_TEXT, L"Language:", DT_SINGLELINE);
    c = pg_control(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x + S(140), y, S(360), S(200), CMD_LANG);
    for (i = 0; i < (int)ARRAYSIZE(LANGS); i++) {
        SendMessageW(c, CB_ADDSTRING, 0, (LPARAM)LANGS[i].name);
        if (!_wcsicmp(LANGS[i].code, lang)) SendMessageW(c, CB_SETCURSEL, i, 0);
    }
    y += S(34);
    pg_text(x + S(16), y, w - S(16), S(20), g_font_body, COL_SUBTLE,
            L"Punctuation and commands you say, and filler words, are recognized in English, German, French and Spanish.",
            DT_SINGLELINE | DT_END_ELLIPSIS);
    y += S(30);
    pg_text(x + S(16), y, w - S(16), S(20), g_font_body, COL_TEXT, L"Put the words into programs by:", DT_SINGLELINE);
    y += S(26);
    c = pg_control(L"BUTTON", L"Typing them (works in the most programs)", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON,
                   x + S(32), y, w - S(32), S(22), CMD_TYPE);
    SendMessageW(c, BM_SETCHECK, setting(L"InsertMethod", 0) != 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    y += S(26);
    c = pg_control(L"BUTTON", L"Pasting them (quicker for long text; your clipboard is put back)", WS_TABSTOP | BS_AUTORADIOBUTTON,
                   x + S(32), y, w - S(32), S(22), CMD_PASTE);
    SendMessageW(c, BM_SETCHECK, setting(L"InsertMethod", 0) == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    y += S(40);

    y = section(x, y, w, L"Privacy");
    y += pg_para(x + S(16), y, w - S(16), g_font_body, COL_TEXT,
                 L"Speech is recognized on this computer. Nothing you say is recorded, kept or sent anywhere, "
                 L"and the microphone is only open while the voice typing bar says \x201CListening\x2026\x201D.");
    y += S(16);
    pg_para(x + S(16), y, w - S(16), g_font_small, COL_SUBTLE,
            L"Speech recognition model: NVIDIA Parakeet TDT 0.6B v3, licensed under CC BY 4.0. "
            L"Voice activity detection: Silero VAD, MIT License.");
    pg_timer(200);
}

static void stop_test(void)
{
    WCHAR dos[MAX_PATH];
    char stop[MAX_PATH + 8];
    if (!g_testing) return;
    g_testing = FALSE;
    _snprintf(stop, sizeof(stop), "%s.stop", g_meter_file);
    unix_to_dos(stop, dos, MAX_PATH);
    CloseHandle(CreateFileW(dos, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
    if (g_test_btn) SetWindowTextW(g_test_btn, L"Test microphone");
    if (g_meter) SendMessageW(g_meter, PBM_SETPOS, 0, 0);
}

static void start_test(void)
{
    WCHAR dos[MAX_PATH], args[1024], mic[256] = L"";
    reg_sz(HKEY_CURRENT_USER, SPEECH_KEY, L"Microphone", mic, ARRAYSIZE(mic));
    if (!temp_file(L"sg-speech-level.txt", dos, g_meter_file, MAX_PATH)) return;
    if (mic[0])
        _snwprintf(args, ARRAYSIZE(args), L"--meter \"%S\" --seconds 30 --device \"%ls\"", g_meter_file, mic);
    else
        _snwprintf(args, ARRAYSIZE(args), L"--meter \"%S\" --seconds 30", g_meter_file);
    args[ARRAYSIZE(args) - 1] = 0;
    if (!run_engine(args)) return;
    g_testing = TRUE;
    g_test_started = GetTickCount();
    SetWindowTextW(g_test_btn, L"Stop test");
}

static void download(void)
{
    run_engine(L"--download");
    g_downloading = TRUE;
    update_status();
}

BOOL cmd_speech(int id, int code, HWND ctl)
{
    BOOL on = SendMessageW(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int sel = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
    ULONGLONG done, total;
    switch (id) {
    case CMD_TRY:
        if (GetDlgItem(g_page, CMD_ENABLED)) SetFocus(GetDlgItem(g_page, CMD_ENABLED));
        return TRUE;
    case CMD_ENABLED:
        if (code != BN_CLICKED) return TRUE;
        set_setting(L"Enabled", on);
        if (on && model_state(&done, &total, NULL, 0) != MS_INSTALLED) download();
        update_status();
        return TRUE;
    case CMD_RETRY: download(); return TRUE;
    case CMD_MIC:
        if (code != CBN_SELCHANGE) return TRUE;
        reg_set_sz(HKEY_CURRENT_USER, SPEECH_KEY, L"Microphone", sel > 0 && sel <= g_nmics ? g_mic_name[sel - 1] : L"");
        if (g_testing) { stop_test(); start_test(); }
        return TRUE;
    case CMD_TEST:
        if (g_testing) stop_test(); else start_test();
        return TRUE;
    case CMD_HOLD:
        set_setting(L"HoldToTalk", on);
        /* The listener: start it now (it also starts with every session), or
         * tell it to go. */
        tell_toolbar(on ? L"/background" : L"/reload");
        return TRUE;
    case CMD_HOLDKEY:
        if (code == CBN_SELCHANGE && sel >= 0 && sel < (int)ARRAYSIZE(HOLD_KEYS)) {
            set_setting(L"HoldKey", HOLD_KEYS[sel].vk);
            tell_toolbar(L"/reload");
        }
        return TRUE;
    case CMD_CONTINUOUS: set_setting(L"Continuous", on); return TRUE;
    case CMD_AUTOPUNCT: set_setting(L"AutoPunctuation", on); return TRUE;
    case CMD_SPOKEN: set_setting(L"SpokenPunctuation", on); return TRUE;
    case CMD_FILLERS: set_setting(L"RemoveFillers", on); return TRUE;
    case CMD_NUMBERS: set_setting(L"FormatNumbers", on); return TRUE;
    case CMD_TYPE: set_setting(L"InsertMethod", 0); return TRUE;
    case CMD_PASTE: set_setting(L"InsertMethod", 1); return TRUE;
    case CMD_LANG:
        if (code == CBN_SELCHANGE && sel >= 0 && sel < (int)ARRAYSIZE(LANGS))
            reg_set_sz(HKEY_CURRENT_USER, SPEECH_KEY, L"Language", LANGS[sel].code);
        return TRUE;
    }
    return FALSE;
}

void timer_speech(void)
{
    if (g_downloading || (g_retry && IsWindowVisible(g_retry))) update_status();
    if (g_testing) {
        WCHAR dos[MAX_PATH];
        char *text;
        unix_to_dos(g_meter_file, dos, MAX_PATH);
        if ((text = read_dos_file(dos))) {
            if (!strncmp(text, "LEVEL ", 6)) SendMessageW(g_meter, PBM_SETPOS, atoi(text + 6), 0);
            else if (!strncmp(text, "DONE", 4) || !strncmp(text, "ERROR", 5)) {
                g_testing = FALSE;
                SetWindowTextW(g_test_btn, L"Test microphone");
                SendMessageW(g_meter, PBM_SETPOS, 0, 0);
            }
            free(text);
        }
        if (GetTickCount() - g_test_started > 31000) stop_test();
    }
}

void dump_speech(void)
{
    ULONGLONG done, total;
    WCHAR mic[256] = L"", lang[16] = L"en-US", msg[256], size[32];
    enum model_state st = model_state(&done, &total, msg, ARRAYSIZE(msg));
    static const WCHAR *const states[] = { L"missing", L"installed", L"downloading", L"failed" };
    reg_sz(HKEY_CURRENT_USER, SPEECH_KEY, L"Microphone", mic, ARRAYSIZE(mic));
    reg_sz(HKEY_CURRENT_USER, SPEECH_KEY, L"Language", lang, ARRAYSIZE(lang));
    format_size(total, size, ARRAYSIZE(size));
    wprintf(L"speech.enabled=%ls\n", setting(L"Enabled", 0) ? L"on" : L"off");
    wprintf(L"speech.model=%ls\n", states[st]);
    if (st == MS_DOWNLOADING) wprintf(L"speech.model.progress=%llu/%llu\n", done, total);
    if (st == MS_FAILED) wprintf(L"speech.model.error=%ls\n", msg);
    wprintf(L"speech.model.size=%ls\n", size);
    wprintf(L"speech.microphone=%ls\n", mic[0] ? mic : L"default");
    wprintf(L"speech.shortcut=Win+H\n");
    wprintf(L"speech.hold_to_talk=%ls\n", setting(L"HoldToTalk", 0) ? L"on" : L"off");
    wprintf(L"speech.hold_key=%ls\n", hold_key_name(setting(L"HoldKey", VK_RCONTROL)));
    wprintf(L"speech.continuous=%ls\n", setting(L"Continuous", 1) ? L"on" : L"off");
    wprintf(L"speech.auto_punctuation=%ls\n", setting(L"AutoPunctuation", 1) ? L"on" : L"off");
    wprintf(L"speech.spoken_punctuation=%ls\n", setting(L"SpokenPunctuation", 1) ? L"on" : L"off");
    wprintf(L"speech.remove_fillers=%ls\n", setting(L"RemoveFillers", 1) ? L"on" : L"off");
    wprintf(L"speech.format_numbers=%ls\n", setting(L"FormatNumbers", 1) ? L"on" : L"off");
    wprintf(L"speech.insert=%ls\n", setting(L"InsertMethod", 0) == 1 ? L"paste" : L"type");
    wprintf(L"speech.language=%ls\n", lang);
}
