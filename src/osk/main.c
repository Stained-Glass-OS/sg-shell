/* sg-osk -- On-Screen Keyboard (osk.exe; Win+Ctrl+O).
 *
 * Windows 10's On-Screen Keyboard, our own drawing: the full keyboard (Esc,
 * the number row -- F1-F12 while Fn is on --, letters, Shift, Caps, Ctrl,
 * Win, Alt, the arrows), the navigation keys (Home, PgUp, End, PgDn, Insert,
 * Pause, PrtScn, ScrLk; shown with Nav), Options and Help, and the right
 * column: Nav, Mv Up, Mv Dn, Dock, Fade. Options: click sound, the
 * navigation keys, the numeric keypad, click or hover to type (and how long
 * to hover).
 *
 * IT NEVER TAKES THE FOCUS. The window is WS_EX_NOACTIVATE and topmost, and
 * answers WM_MOUSEACTIVATE with MA_NOACTIVATE, so the program being typed
 * into keeps the keyboard focus. Its title bar is our own drawing and it is
 * moved and sized by us with the mouse captured (SetWindowPos with
 * SWP_NOACTIVATE): Wine's own move loop (a caption from WM_NCHITTEST) makes
 * the window the foreground window first.
 *
 * KEYS ARE REAL KEY PRESSES. SendInput with the virtual key and its scan code
 * (MapVirtualKey), extended keys flagged, so programs see what a keyboard
 * sends. Shift, Ctrl, Alt and Win are sticky: a click latches one (lit in
 * the accent colour), the next key is sent with the latched ones held, and
 * they let go. Caps Lock is a real Caps Lock press; its light is the
 * keyboard's toggle state (GetKeyState), so a physical Caps Lock shows too.
 * Labels follow Shift and Caps (and Fn).
 *
 * THE LABELS ARE THE KEYBOARD LAYOUT'S. The character keys are physical
 * positions (scan codes); what each types is asked of the layout of the
 * program in front -- GetKeyboardLayout of the foreground window's thread,
 * MapVirtualKeyEx (scan code to virtual key) and ToUnicodeEx with the
 * latched modifiers and Caps Lock -- so a German layout shows QWERTZ, ß, ü,
 * ö, ä and, with AltGr, @ on Q; French shows AZERTY. A key is sent as that
 * layout's virtual key with its own scan code, so the program's layout
 * decides the character, as with a physical keyboard. On a layout with AltGr
 * characters the right Alt is AltGr (sent as left Ctrl + right Alt, as
 * Windows' AltGr is), and an ISO layout gets the key between Shift and Z.
 * The labels are re-read on WM_INPUTLANGCHANGE, when the foreground window
 * changes and every 700 ms (a layout switched on the X server reaches Wine
 * only as a new keymap; Wine's HKL stays the locale's), and redrawn when
 * they differ. Wine: wine-sg 0250 (the national keys' scan codes) and 0251
 * (Ctrl+Alt is AltGr in ToUnicodeEx).
 *
 * Settings: HKCU\Software\Microsoft\Osk -- WindowLeft/Top/Width/Height,
 * ShowNavigationKeys, ShowNumPad, ClickSound, Mode (0 click, 1 hover),
 * HoverPeriod (ms), Dock, Fade.
 *
 * One instance (mutex Local\StainedGlassOnScreenKeyboard, window class
 * OSKMainClass, as programs that look for the On-Screen Keyboard expect);
 * another start shows it. Explorer's Win+Ctrl+O (wine-sg 0182) closes it
 * when it is open.
 *
 * SG_OSK_DUMP=<file> (a Windows path): the window, its styles and every key's
 * screen centre and state, rewritten after every change, for the gate
 * (test/osk-check.sh).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define CLASS_NAME L"OSKMainClass"
#define KEY L"Software\\Microsoft\\Osk"
#define WM_APPBAR (WM_APP + 1)

enum {
    F_MOD = 1,          /* sticky modifier */
    F_EXT = 2,          /* extended key */
    F_NAV = 4,          /* one of the navigation keys (shown with Nav) */
    F_PAD = 8,          /* numeric keypad (shown with the key pad) */
    F_ACT = 16,         /* our own action, not a key */
    F_LETTER = 32,
};
enum { M_SHIFT = 1, M_CTRL = 2, M_ALT = 4, M_WIN = 8, M_ALTGR = 16 };
enum { A_NONE, A_FN, A_NAV, A_UP, A_DOWN, A_DOCK, A_FADE, A_OPTIONS, A_HELP };

struct key {
    const WCHAR *name;          /* the gate's name for it */
    const WCHAR *label, *shifted;
    WORD vk;
    float x, y, w, h;           /* in key units: the main block is 15 wide, 5 high */
    int flags, mod, action;
    const WCHAR *fnlabel;       /* with Fn */
    WORD fnvk;
    RECT rc;                    /* laid out, client coordinates */
    BOOL shown;
    WORD sc;                    /* a character key's scan code (set at start) */
};

#define NAVX 15.25f
#define RCOLX 17.5f
#define PADX 18.9f
static struct key keys[] = {
    /* row 0 */
    { L"esc", L"Esc", NULL, VK_ESCAPE, 0, 0, 1, 1 },
    { L"grave", L"`", L"~", VK_OEM_3, 1, 0, 1, 1 },
    { L"1", L"1", L"!", '1', 2, 0, 1, 1, 0, 0, 0, L"F1", VK_F1 },
    { L"2", L"2", L"@", '2', 3, 0, 1, 1, 0, 0, 0, L"F2", VK_F2 },
    { L"3", L"3", L"#", '3', 4, 0, 1, 1, 0, 0, 0, L"F3", VK_F3 },
    { L"4", L"4", L"$", '4', 5, 0, 1, 1, 0, 0, 0, L"F4", VK_F4 },
    { L"5", L"5", L"%", '5', 6, 0, 1, 1, 0, 0, 0, L"F5", VK_F5 },
    { L"6", L"6", L"^", '6', 7, 0, 1, 1, 0, 0, 0, L"F6", VK_F6 },
    { L"7", L"7", L"&", '7', 8, 0, 1, 1, 0, 0, 0, L"F7", VK_F7 },
    { L"8", L"8", L"*", '8', 9, 0, 1, 1, 0, 0, 0, L"F8", VK_F8 },
    { L"9", L"9", L"(", '9', 10, 0, 1, 1, 0, 0, 0, L"F9", VK_F9 },
    { L"0", L"0", L")", '0', 11, 0, 1, 1, 0, 0, 0, L"F10", VK_F10 },
    { L"minus", L"-", L"_", VK_OEM_MINUS, 12, 0, 1, 1, 0, 0, 0, L"F11", VK_F11 },
    { L"equals", L"=", L"+", VK_OEM_PLUS, 13, 0, 1, 1, 0, 0, 0, L"F12", VK_F12 },
    { L"backspace", L"", NULL, VK_BACK, 14, 0, 1, 1 },
    /* row 1 */
    { L"tab", L"Tab", NULL, VK_TAB, 0, 1, 1.5f, 1 },
    { L"q", L"q", NULL, 'Q', 1.5f, 1, 1, 1, F_LETTER },
    { L"w", L"w", NULL, 'W', 2.5f, 1, 1, 1, F_LETTER },
    { L"e", L"e", NULL, 'E', 3.5f, 1, 1, 1, F_LETTER },
    { L"r", L"r", NULL, 'R', 4.5f, 1, 1, 1, F_LETTER },
    { L"t", L"t", NULL, 'T', 5.5f, 1, 1, 1, F_LETTER },
    { L"y", L"y", NULL, 'Y', 6.5f, 1, 1, 1, F_LETTER },
    { L"u", L"u", NULL, 'U', 7.5f, 1, 1, 1, F_LETTER },
    { L"i", L"i", NULL, 'I', 8.5f, 1, 1, 1, F_LETTER },
    { L"o", L"o", NULL, 'O', 9.5f, 1, 1, 1, F_LETTER },
    { L"p", L"p", NULL, 'P', 10.5f, 1, 1, 1, F_LETTER },
    { L"lbracket", L"[", L"{", VK_OEM_4, 11.5f, 1, 1, 1 },
    { L"rbracket", L"]", L"}", VK_OEM_6, 12.5f, 1, 1, 1 },
    { L"backslash", L"\\", L"|", VK_OEM_5, 13.5f, 1, 0.75f, 1 },
    { L"del", L"Del", NULL, VK_DELETE, 14.25f, 1, 0.75f, 1, F_EXT },
    /* row 2 */
    { L"caps", L"Caps", NULL, VK_CAPITAL, 0, 2, 1.75f, 1 },
    { L"a", L"a", NULL, 'A', 1.75f, 2, 1, 1, F_LETTER },
    { L"s", L"s", NULL, 'S', 2.75f, 2, 1, 1, F_LETTER },
    { L"d", L"d", NULL, 'D', 3.75f, 2, 1, 1, F_LETTER },
    { L"f", L"f", NULL, 'F', 4.75f, 2, 1, 1, F_LETTER },
    { L"g", L"g", NULL, 'G', 5.75f, 2, 1, 1, F_LETTER },
    { L"h", L"h", NULL, 'H', 6.75f, 2, 1, 1, F_LETTER },
    { L"j", L"j", NULL, 'J', 7.75f, 2, 1, 1, F_LETTER },
    { L"k", L"k", NULL, 'K', 8.75f, 2, 1, 1, F_LETTER },
    { L"l", L"l", NULL, 'L', 9.75f, 2, 1, 1, F_LETTER },
    { L"semicolon", L";", L":", VK_OEM_1, 10.75f, 2, 1, 1 },
    { L"quote", L"'", L"\"", VK_OEM_7, 11.75f, 2, 1, 1 },
    { L"enter", L"Enter", NULL, VK_RETURN, 12.75f, 2, 2.25f, 1 },
    /* row 3 */
    { L"shift", L"Shift", NULL, VK_LSHIFT, 0, 3, 2.25f, 1, F_MOD, M_SHIFT },
    { L"oem102", L"\\", L"|", VK_OEM_102, 1.25f, 3, 1, 1 },
    { L"z", L"z", NULL, 'Z', 2.25f, 3, 1, 1, F_LETTER },
    { L"x", L"x", NULL, 'X', 3.25f, 3, 1, 1, F_LETTER },
    { L"c", L"c", NULL, 'C', 4.25f, 3, 1, 1, F_LETTER },
    { L"v", L"v", NULL, 'V', 5.25f, 3, 1, 1, F_LETTER },
    { L"b", L"b", NULL, 'B', 6.25f, 3, 1, 1, F_LETTER },
    { L"n", L"n", NULL, 'N', 7.25f, 3, 1, 1, F_LETTER },
    { L"m", L"m", NULL, 'M', 8.25f, 3, 1, 1, F_LETTER },
    { L"comma", L",", L"<", VK_OEM_COMMA, 9.25f, 3, 1, 1 },
    { L"period", L".", L">", VK_OEM_PERIOD, 10.25f, 3, 1, 1 },
    { L"slash", L"/", L"?", VK_OEM_2, 11.25f, 3, 1, 1 },
    { L"up", L"", NULL, VK_UP, 12.25f, 3, 1, 1, F_EXT },
    { L"rshift", L"Shift", NULL, VK_RSHIFT, 13.25f, 3, 1.75f, 1, F_MOD, M_SHIFT },
    /* row 4 */
    { L"fn", L"Fn", NULL, 0, 0, 4, 1, 1, F_ACT, 0, A_FN },
    { L"ctrl", L"Ctrl", NULL, VK_LCONTROL, 1, 4, 1.25f, 1, F_MOD, M_CTRL },
    { L"win", L"", NULL, VK_LWIN, 2.25f, 4, 1, 1, F_MOD | F_EXT, M_WIN },
    { L"alt", L"Alt", NULL, VK_LMENU, 3.25f, 4, 1.25f, 1, F_MOD, M_ALT },
    { L"space", L"", NULL, VK_SPACE, 4.5f, 4, 5.25f, 1 },
    { L"altgr", L"Alt", NULL, VK_RMENU, 9.75f, 4, 1.25f, 1, F_MOD | F_EXT, M_ALT },
    { L"rctrl", L"Ctrl", NULL, VK_RCONTROL, 11, 4, 1, 1, F_MOD | F_EXT, M_CTRL },
    { L"left", L"", NULL, VK_LEFT, 12, 4, 1, 1, F_EXT },
    { L"down", L"", NULL, VK_DOWN, 13, 4, 1, 1, F_EXT },
    { L"right", L"", NULL, VK_RIGHT, 14, 4, 1, 1, F_EXT },
    /* the navigation keys */
    { L"home", L"Home", NULL, VK_HOME, NAVX, 0, 1, 1, F_EXT | F_NAV },
    { L"pgup", L"PgUp", NULL, VK_PRIOR, NAVX + 1, 0, 1, 1, F_EXT | F_NAV },
    { L"end", L"End", NULL, VK_END, NAVX, 1, 1, 1, F_EXT | F_NAV },
    { L"pgdn", L"PgDn", NULL, VK_NEXT, NAVX + 1, 1, 1, 1, F_EXT | F_NAV },
    { L"insert", L"Insert", NULL, VK_INSERT, NAVX, 2, 1, 1, F_EXT | F_NAV },
    { L"pause", L"Pause", NULL, VK_PAUSE, NAVX + 1, 2, 1, 1, F_NAV },
    { L"prtscn", L"PrtScn", NULL, VK_SNAPSHOT, NAVX, 3, 1, 1, F_EXT | F_NAV },
    { L"scrlk", L"ScrLk", NULL, VK_SCROLL, NAVX + 1, 3, 1, 1, F_NAV },
    { L"options", L"Options", NULL, 0, NAVX, 4, 1, 1, F_ACT, 0, A_OPTIONS },
    { L"help", L"Help", NULL, 0, NAVX + 1, 4, 1, 1, F_ACT, 0, A_HELP },
    /* the right column */
    { L"nav", L"Nav", NULL, 0, RCOLX, 0, 1.15f, 1, F_ACT, 0, A_NAV },
    { L"mvup", L"Mv Up", NULL, 0, RCOLX, 1, 1.15f, 1, F_ACT, 0, A_UP },
    { L"mvdn", L"Mv Dn", NULL, 0, RCOLX, 2, 1.15f, 1, F_ACT, 0, A_DOWN },
    { L"dock", L"Dock", NULL, 0, RCOLX, 3, 1.15f, 1, F_ACT, 0, A_DOCK },
    { L"fade", L"Fade", NULL, 0, RCOLX, 4, 1.15f, 1, F_ACT, 0, A_FADE },
    /* the numeric keypad */
    { L"numlock", L"NumLk", NULL, VK_NUMLOCK, PADX, 0, 1, 1, F_PAD | F_EXT },
    { L"divide", L"/", NULL, VK_DIVIDE, PADX + 1, 0, 1, 1, F_PAD | F_EXT },
    { L"multiply", L"*", NULL, VK_MULTIPLY, PADX + 2, 0, 1, 1, F_PAD },
    { L"subtract", L"-", NULL, VK_SUBTRACT, PADX + 3, 0, 1, 1, F_PAD },
    { L"num7", L"7", NULL, VK_NUMPAD7, PADX, 1, 1, 1, F_PAD },
    { L"num8", L"8", NULL, VK_NUMPAD8, PADX + 1, 1, 1, 1, F_PAD },
    { L"num9", L"9", NULL, VK_NUMPAD9, PADX + 2, 1, 1, 1, F_PAD },
    { L"add", L"+", NULL, VK_ADD, PADX + 3, 1, 1, 2, F_PAD },
    { L"num4", L"4", NULL, VK_NUMPAD4, PADX, 2, 1, 1, F_PAD },
    { L"num5", L"5", NULL, VK_NUMPAD5, PADX + 1, 2, 1, 1, F_PAD },
    { L"num6", L"6", NULL, VK_NUMPAD6, PADX + 2, 2, 1, 1, F_PAD },
    { L"num1", L"1", NULL, VK_NUMPAD1, PADX, 3, 1, 1, F_PAD },
    { L"num2", L"2", NULL, VK_NUMPAD2, PADX + 1, 3, 1, 1, F_PAD },
    { L"num3", L"3", NULL, VK_NUMPAD3, PADX + 2, 3, 1, 1, F_PAD },
    { L"numenter", L"Enter", NULL, VK_RETURN, PADX + 3, 3, 1, 2, F_PAD | F_EXT },
    { L"num0", L"0", NULL, VK_NUMPAD0, PADX, 4, 2, 1, F_PAD },
    { L"decimal", L".", NULL, VK_DECIMAL, PADX + 2, 4, 1, 1, F_PAD },
};
#define NKEYS ((int)ARRAYSIZE(keys))

static HINSTANCE g_inst;
static HWND g_wnd;
static int g_latched;                   /* M_* */
static BOOL g_fn, g_nav = TRUE, g_numpad, g_click_sound, g_hover, g_docked, g_fade;
static DWORD g_hover_ms = 1000;
static int g_pressed = -1, g_hot = -1;  /* key under the mouse button / pointer */
static int g_hover_key = -1;
static DWORD g_sent;                    /* keys sent, for the dump */
static WCHAR g_dump[MAX_PATH];
static HFONT g_font, g_font_small, g_font_title;
static enum { DRAG_NONE, DRAG_MOVE, DRAG_SIZE } g_drag;
static POINT g_drag_origin; static RECT g_drag_rect;
static BYTE g_alpha = 255;
static BOOL g_appbar;
static RECT g_undocked;
static int g_title_h = 30;
static RECT g_btn_min, g_btn_close;

/* the keyboard layout (see the header) */
static HKL g_hkl;
static BOOL g_has_altgr, g_iso;
static WCHAR g_layout_sig[512];
static const struct { const WCHAR *name; WORD sc; } key_scans[] = {
    { L"grave", 0x29 }, { L"1", 0x02 }, { L"2", 0x03 }, { L"3", 0x04 }, { L"4", 0x05 }, { L"5", 0x06 },
    { L"6", 0x07 }, { L"7", 0x08 }, { L"8", 0x09 }, { L"9", 0x0a }, { L"0", 0x0b }, { L"minus", 0x0c },
    { L"equals", 0x0d }, { L"q", 0x10 }, { L"w", 0x11 }, { L"e", 0x12 }, { L"r", 0x13 }, { L"t", 0x14 },
    { L"y", 0x15 }, { L"u", 0x16 }, { L"i", 0x17 }, { L"o", 0x18 }, { L"p", 0x19 }, { L"lbracket", 0x1a },
    { L"rbracket", 0x1b }, { L"backslash", 0x2b }, { L"a", 0x1e }, { L"s", 0x1f }, { L"d", 0x20 },
    { L"f", 0x21 }, { L"g", 0x22 }, { L"h", 0x23 }, { L"j", 0x24 }, { L"k", 0x25 }, { L"l", 0x26 },
    { L"semicolon", 0x27 }, { L"quote", 0x28 }, { L"oem102", 0x56 }, { L"z", 0x2c }, { L"x", 0x2d },
    { L"c", 0x2e }, { L"v", 0x2f }, { L"b", 0x30 }, { L"n", 0x31 }, { L"m", 0x32 }, { L"comma", 0x33 },
    { L"period", 0x34 }, { L"slash", 0x35 },
};

#define TIMER_STATE 1
#define TIMER_HOVER 2

/* ---- colours: Windows 10's dark keyboard, our accent ------------------------------------ */
#define C_BG      RGB(31, 31, 31)
#define C_KEY     RGB(51, 51, 51)
#define C_KEY_HOT RGB(70, 70, 70)
#define C_KEY_DN  RGB(95, 95, 95)
#define C_ACTION  RGB(40, 40, 40)
#define C_TEXT    RGB(255, 255, 255)
#define C_DIM     RGB(170, 170, 170)
#define C_ACCENT  RGB(112, 48, 192)

/* ---- settings ------------------------------------------------------------------------------- */
static DWORD reg_get(const WCHAR *name, DWORD def)
{
    DWORD v = def, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, KEY, name, RRF_RT_REG_DWORD, NULL, &v, &size)) return def;
    return v;
}

static void reg_put(const WCHAR *name, DWORD v)
{
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    RegSetValueExW(k, name, 0, REG_DWORD, (BYTE *)&v, sizeof(v));
    RegCloseKey(k);
}

static void save_settings(void)
{
    reg_put(L"ShowNavigationKeys", g_nav);
    reg_put(L"ShowNumPad", g_numpad);
    reg_put(L"ClickSound", g_click_sound);
    reg_put(L"Mode", g_hover);
    reg_put(L"HoverPeriod", g_hover_ms);
    reg_put(L"Fade", g_fade);
}

static void save_placement(void)
{
    RECT r;
    if (g_docked || !g_wnd || IsIconic(g_wnd)) return;
    GetWindowRect(g_wnd, &r);
    reg_put(L"WindowLeft", r.left);
    reg_put(L"WindowTop", r.top);
    reg_put(L"WindowWidth", r.right - r.left);
    reg_put(L"WindowHeight", r.bottom - r.top);
}

/* ---- state -------------------------------------------------------------------------------- */
static BOOL caps_on(void) { return (GetKeyState(VK_CAPITAL) & 1) != 0; }

static BOOL key_lit(const struct key *k)
{
    if (k->vk == VK_RMENU && g_has_altgr) return (g_latched & M_ALTGR) != 0;
    if (k->flags & F_MOD) return (g_latched & k->mod) != 0;
    if (k->vk == VK_CAPITAL) return caps_on();
    if (k->vk == VK_NUMLOCK) return (GetKeyState(VK_NUMLOCK) & 1) != 0;
    if (k->vk == VK_SCROLL) return (GetKeyState(VK_SCROLL) & 1) != 0;
    switch (k->action)
    {
    case A_FN: return g_fn;
    case A_NAV: return g_nav;
    case A_DOCK: return g_docked;
    case A_FADE: return g_fade;
    }
    return FALSE;
}

/* what a character key types in the layout with these modifiers: 1 a
 * character, -1 a dead key (its accent in out), 0 nothing */
static int layout_char(const struct key *k, BOOL shift, BOOL altgr, BOOL caps, WCHAR *out)
{
    BYTE state[256];
    WCHAR b[8];
    UINT vk;
    int r;
    out[0] = 0;
    if (!k->sc) return 0;
#ifdef SG_MUTANT_USLABELS
    return 0;
#endif
    vk = MapVirtualKeyExW(k->sc, MAPVK_VSC_TO_VK_EX, g_hkl);
    if (!vk) return 0;
    memset(state, 0, sizeof(state));
    if (shift) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
    if (caps) state[VK_CAPITAL] = 0x01;
    if (altgr) state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_MENU] = state[VK_RMENU] = 0x80;
    /* flag 4: the kernel's dead-key state is left alone (Windows 10 1607+) */
    r = ToUnicodeEx(vk, k->sc, state, b, ARRAYSIZE(b), 4, g_hkl);
    if (r == 0 || b[0] < 0x20 || b[0] == 0x7f) return 0;
    out[0] = b[0]; out[1] = 0;
    return r < 0 ? -1 : 1;
}

static const WCHAR *key_label(const struct key *k, WCHAR *buf)
{
    BOOL shift = (g_latched & M_SHIFT) != 0;
    if (g_fn && k->fnlabel) return k->fnlabel;
    if (k->vk == VK_RMENU && g_has_altgr) return L"AltGr";
    if (k->sc)
    {
        BOOL altgr = (g_latched & M_ALTGR) || ((g_latched & (M_CTRL | M_ALT)) == (M_CTRL | M_ALT));
        if (layout_char(k, shift, altgr, caps_on(), buf)) return buf;
        if (altgr) { buf[0] = 0; return buf; }
    }
    if (k->flags & F_LETTER)
    {
        buf[0] = k->label[0]; buf[1] = 0;
        if (shift != caps_on()) buf[0] = towupper(buf[0]);
        return buf;
    }
    if (shift && k->shifted) return k->shifted;
    return k->label;
}

/* ---- the dump ----------------------------------------------------------------------------- */
/* a line of the dump, in UTF-8 (the labels are any character) */
static void dumpf(FILE *f, const WCHAR *fmt, ...)
{
    WCHAR w[1024];
    char u[3072];
    int n;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(w, ARRAYSIZE(w) - 1, fmt, ap);
    va_end(ap);
    w[ARRAYSIZE(w) - 1] = 0;
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u), NULL, NULL);
    if (n > 1) fwrite(u, 1, n - 1, f);
}

static void write_dump(void)
{
    FILE *f;
    RECT wr;
    int i;
    WCHAR buf[4];
    if (!g_dump[0] || !g_wnd) return;
    WCHAR tmp[MAX_PATH + 8];
    _snwprintf(tmp, ARRAYSIZE(tmp), L"%ls.tmp", g_dump);
    tmp[ARRAYSIZE(tmp) - 1] = 0;
    /* written whole, then renamed into place: a reader never sees half of it */
    if (!(f = _wfopen(tmp, L"wb"))) return;
    GetWindowRect(g_wnd, &wr);
    dumpf(f, L"WINDOW %ld %ld %ld %ld\nEXSTYLE %08lx\nVISIBLE %d\n", wr.left, wr.top, wr.right, wr.bottom,
             (unsigned long)GetWindowLongW(g_wnd, GWL_EXSTYLE), IsWindowVisible(g_wnd) != 0);
    dumpf(f, L"LATCHED %d\nCAPS %d\nFN %d\nNAV %d\nNUMPAD %d\nDOCK %d\nFADE %d\nALPHA %d\nHOVER %d %lu\nCLICKSOUND %d\nSENT %lu\n",
             g_latched, caps_on(), g_fn, g_nav, g_numpad, g_docked, g_fade, g_alpha, g_hover,
             (unsigned long)g_hover_ms, g_click_sound, (unsigned long)g_sent);
    dumpf(f, L"LAYOUT %p ALTGR %d ISO %d\n", g_hkl, g_has_altgr, g_iso);
    for (i = 0; i < NKEYS; i++)
    {
        POINT c;
        if (!keys[i].shown) continue;
        c.x = (keys[i].rc.left + keys[i].rc.right) / 2; c.y = (keys[i].rc.top + keys[i].rc.bottom) / 2;
        ClientToScreen(g_wnd, &c);
        dumpf(f, L"KEY %ls %ld %ld %d %ls\n", keys[i].name, c.x, c.y, key_lit(&keys[i]), key_label(&keys[i], buf));
    }
    {
        POINT a = { (g_btn_close.left + g_btn_close.right) / 2, (g_btn_close.top + g_btn_close.bottom) / 2 };
        POINT b = { (g_btn_min.left + g_btn_min.right) / 2, (g_btn_min.top + g_btn_min.bottom) / 2 };
        POINT t = { 60, g_title_h / 2 };
        ClientToScreen(g_wnd, &a); ClientToScreen(g_wnd, &b); ClientToScreen(g_wnd, &t);
        dumpf(f, L"CLOSE %ld %ld\nMINIMIZE %ld %ld\nTITLE %ld %ld\n", a.x, a.y, b.x, b.y, t.x, t.y);
    }
    dumpf(f, L"END\n");
    fclose(f);
    MoveFileExW(tmp, g_dump, MOVEFILE_REPLACE_EXISTING);
}

/* ---- the keyboard layout ------------------------------------------------------------------ */
/* Re-read the layout of the program in front; TRUE when the labels changed. */
static BOOL refresh_layout(void)
{
    WCHAR sig[ARRAYSIZE(g_layout_sig)], c[4];
    int i, n = 0;
    HWND fg = GetForegroundWindow();
    BOOL altgr = FALSE, iso;
    struct key *bs = NULL, *iso_key = NULL;

    g_hkl = GetKeyboardLayout(fg ? GetWindowThreadProcessId(fg, NULL) : 0);
    n = _snwprintf(sig, ARRAYSIZE(sig), L"%p:", g_hkl);
    for (i = 0; i < NKEYS && n < (int)ARRAYSIZE(sig) - 8; i++)
    {
        struct key *k = &keys[i];
        if (!k->sc) continue;
        if (k->sc == 0x2b) bs = k;
        if (k->sc == 0x56) iso_key = k;
        if (layout_char(k, FALSE, FALSE, FALSE, c)) sig[n++] = c[0]; else sig[n++] = ' ';
        if (layout_char(k, TRUE, FALSE, FALSE, c)) sig[n++] = c[0]; else sig[n++] = ' ';
        if (layout_char(k, FALSE, TRUE, FALSE, c) && k->sc != 0x56) { altgr = TRUE; sig[n++] = c[0]; }
    }
    sig[n] = 0;
    /* the key between Shift and Z: ISO layouts (with AltGr, or not a US
     * backslash where the US has it -- British) */
    iso = FALSE;
    if (iso_key && layout_char(iso_key, FALSE, FALSE, FALSE, c))
    {
        WCHAR b[4];
        iso = altgr || !(bs && layout_char(bs, FALSE, FALSE, FALSE, b) && b[0] == '\\');
    }
    if (!wcscmp(sig, g_layout_sig) && altgr == g_has_altgr && iso == g_iso) return FALSE;
    lstrcpynW(g_layout_sig, sig, ARRAYSIZE(g_layout_sig));
    g_has_altgr = altgr;
    if (!altgr) g_latched &= ~M_ALTGR;
    g_iso = iso;
    return TRUE;
}

static void layout(void);
static void redraw(void);
static void layout_changed(void)
{
    if (!refresh_layout()) return;
    if (g_wnd) { layout(); redraw(); }
}

/* ---- layout ------------------------------------------------------------------------------- */
static float units_wide(void)
{
    float w = RCOLX + 1.15f;
    if (g_numpad) w = PADX + 4;
    return w;
}

static void layout(void)
{
    RECT cr;
    float uw, uh;
    int i, pad = 6, gap = 3;
    GetClientRect(g_wnd, &cr);
    SetRect(&g_btn_close, cr.right - 46, 0, cr.right, g_title_h);
    SetRect(&g_btn_min, cr.right - 92, 0, cr.right - 46, g_title_h);
    uw = (cr.right - 2 * pad) / units_wide();
    uh = (cr.bottom - g_title_h - 2 * pad) / 5.0f;
    for (i = 0; i < NKEYS; i++)
    {
        struct key *k = &keys[i];
        k->shown = !((k->flags & F_NAV) && !g_nav) && !((k->flags & F_PAD) && !g_numpad);
        if (k->vk == VK_OEM_102) k->shown = g_iso;
        if (k->vk == VK_LSHIFT) k->w = g_iso ? 1.25f : 2.25f;
        k->rc.left = pad + (int)(k->x * uw + 0.5f);
        k->rc.top = g_title_h + pad + (int)(k->y * uh + 0.5f);
        k->rc.right = pad + (int)((k->x + k->w) * uw + 0.5f) - gap;
        k->rc.bottom = g_title_h + pad + (int)((k->y + k->h) * uh + 0.5f) - gap;
    }
    {
        LOGFONTW lf = { 0 };
        int h = (int)(uh * 0.34f);
        if (h < 10) h = 10;
        if (g_font) DeleteObject(g_font);
        if (g_font_small) DeleteObject(g_font_small);
        lf.lfHeight = -h;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lstrcpyW(lf.lfFaceName, L"Segoe UI");
        g_font = CreateFontIndirectW(&lf);
        lf.lfHeight = -(h * 3 / 4 > 9 ? h * 3 / 4 : 9);
        g_font_small = CreateFontIndirectW(&lf);
    }
}

static int key_at(int x, int y)
{
    POINT p = { x, y };
    int i;
    for (i = 0; i < NKEYS; i++) if (keys[i].shown && PtInRect(&keys[i].rc, p)) return i;
    return -1;
}

/* ---- painting ---------------------------------------------------------------------------- */
/* the keys Windows marks with a picture: Backspace, the Windows key, the arrows */
static BOOL draw_glyph(HDC dc, const struct key *k)
{
    int cx = (k->rc.left + k->rc.right) / 2, cy = (k->rc.top + k->rc.bottom) / 2;
    int s = min(k->rc.right - k->rc.left, k->rc.bottom - k->rc.top) / 5;
    HPEN pen, op;
    HBRUSH br, ob;
    if (s < 3) s = 3;
    pen = CreatePen(PS_SOLID, 1, C_TEXT); br = CreateSolidBrush(C_TEXT);
    op = SelectObject(dc, pen); ob = SelectObject(dc, br);
    if (k->vk == VK_BACK)
    {
        POINT pt[5] = { { cx - 2 * s, cy }, { cx - s, cy - s }, { cx + 2 * s, cy - s }, { cx + 2 * s, cy + s }, { cx - s, cy + s } };
        SelectObject(dc, GetStockObject(NULL_BRUSH));
        Polygon(dc, pt, 5);
        MoveToEx(dc, cx - s / 3, cy - s / 2, NULL); LineTo(dc, cx + s + s / 3, cy + s / 2 + 1);
        MoveToEx(dc, cx + s + s / 3, cy - s / 2, NULL); LineTo(dc, cx - s / 3, cy + s / 2 + 1);
    }
    else if (k->vk == VK_LWIN)
    {
        int g = max(1, s / 4);
        RECT q[4] = { { cx - s, cy - s, cx - g / 2, cy - g / 2 }, { cx + (g + 1) / 2, cy - s, cx + s, cy - g / 2 },
                      { cx - s, cy + (g + 1) / 2, cx - g / 2, cy + s }, { cx + (g + 1) / 2, cy + (g + 1) / 2, cx + s, cy + s } };
        int i;
        for (i = 0; i < 4; i++) FillRect(dc, &q[i], br);
    }
    else if (k->vk == VK_UP || k->vk == VK_DOWN || k->vk == VK_LEFT || k->vk == VK_RIGHT)
    {
        POINT t[3];
        int d = s;
        switch (k->vk)
        {
        case VK_UP:    t[0].x = cx; t[0].y = cy - d; t[1].x = cx - d; t[1].y = cy + d / 2; t[2].x = cx + d; t[2].y = cy + d / 2; break;
        case VK_DOWN:  t[0].x = cx; t[0].y = cy + d; t[1].x = cx - d; t[1].y = cy - d / 2; t[2].x = cx + d; t[2].y = cy - d / 2; break;
        case VK_LEFT:  t[0].x = cx - d; t[0].y = cy; t[1].x = cx + d / 2; t[1].y = cy - d; t[2].x = cx + d / 2; t[2].y = cy + d; break;
        default:       t[0].x = cx + d; t[0].y = cy; t[1].x = cx - d / 2; t[1].y = cy - d; t[2].x = cx - d / 2; t[2].y = cy + d; break;
        }
        Polygon(dc, t, 3);
    }
    else
    {
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen); DeleteObject(br);
        return FALSE;
    }
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(pen); DeleteObject(br);
    return TRUE;
}

static void paint(HDC dc)
{
    RECT cr, r;
    int i;
    WCHAR buf[4];
    HBRUSH bg = CreateSolidBrush(C_BG);
    GetClientRect(g_wnd, &cr);
    FillRect(dc, &cr, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    /* the title bar: our own, so the window never becomes the foreground to be moved */
    SelectObject(dc, g_font_title);
    SetTextColor(dc, C_TEXT);
    SetRect(&r, 12, 0, cr.right - 100, g_title_h);
    {
        HICON ic = LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
        if (ic) { DrawIconEx(dc, 10, (g_title_h - 16) / 2, ic, 16, 16, 0, NULL, DI_NORMAL); DestroyIcon(ic); r.left = 34; }
    }
    DrawTextW(dc, L"On-Screen Keyboard", -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    {
        HPEN pen = CreatePen(PS_SOLID, 1, C_TEXT), op = SelectObject(dc, pen);
        int cx = (g_btn_min.left + g_btn_min.right) / 2, cy = g_title_h / 2;
        MoveToEx(dc, cx - 5, cy, NULL); LineTo(dc, cx + 6, cy);
        cx = (g_btn_close.left + g_btn_close.right) / 2;
        MoveToEx(dc, cx - 5, cy - 5, NULL); LineTo(dc, cx + 6, cy + 6);
        MoveToEx(dc, cx + 5, cy - 5, NULL); LineTo(dc, cx - 6, cy + 6);
        SelectObject(dc, op); DeleteObject(pen);
    }

    for (i = 0; i < NKEYS; i++)
    {
        struct key *k = &keys[i];
        COLORREF fill;
        HBRUSH b;
        const WCHAR *label;
        if (!k->shown) continue;
        fill = (k->flags & F_ACT) ? C_ACTION : C_KEY;
        if (i == g_hot) fill = C_KEY_HOT;
        if (i == g_pressed) fill = C_KEY_DN;
        if (key_lit(k)) fill = C_ACCENT;
        b = CreateSolidBrush(fill);
        FillRect(dc, &k->rc, b);
        DeleteObject(b);
        if (draw_glyph(dc, k)) continue;
        label = key_label(k, buf);
        SelectObject(dc, (wcslen(label) > 2 || (k->flags & F_ACT)) ? g_font_small : g_font);
        SetTextColor(dc, (k->flags & F_ACT) && !key_lit(k) ? C_DIM : C_TEXT);
        r = k->rc;
        if ((k->flags & F_LETTER) || (k->shifted && wcslen(label) == 1))
            DrawTextW(dc, label, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        else
        {
            /* word keys: at the left, as Windows draws them */
            r.left += 6;
            DrawTextW(dc, label, -1, &r, (wcslen(label) > 1 ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }
    /* the sizing grip */
    {
        HPEN pen = CreatePen(PS_SOLID, 1, C_DIM), op = SelectObject(dc, pen);
        int k2;
        for (k2 = 3; k2 <= 9; k2 += 3)
        {
            MoveToEx(dc, cr.right - k2, cr.bottom - 1, NULL); LineTo(dc, cr.right, cr.bottom - k2 - 1);
        }
        SelectObject(dc, op); DeleteObject(pen);
    }
}

static void redraw(void)
{
    InvalidateRect(g_wnd, NULL, FALSE);
    write_dump();
}

/* ---- sending keys ------------------------------------------------------------------------- */
static void add_input_sc(INPUT *in, int *n, WORD vk, WORD sc, BOOL up, BOOL ext)
{
    INPUT *i = &in[(*n)++];
    memset(i, 0, sizeof(*i));
    i->type = INPUT_KEYBOARD;
    i->ki.wVk = vk;
    i->ki.wScan = sc ? sc : (WORD)MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, g_hkl);
    i->ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
}

static void add_input(INPUT *in, int *n, WORD vk, BOOL up, BOOL ext)
{
    add_input_sc(in, n, vk, 0, up, ext);
}

static void click_sound(void)
{
    /* a short tick of our own: 12 ms of a decaying square wave, 8-bit mono 22 kHz */
    static BYTE wav[44 + 264];
    static BOOL made;
    if (!g_click_sound) return;
    if (!made)
    {
        DWORD n = 264, i;
        memcpy(wav, "RIFF", 4); *(DWORD *)(wav + 4) = 36 + n; memcpy(wav + 8, "WAVEfmt ", 8);
        *(DWORD *)(wav + 16) = 16; *(WORD *)(wav + 20) = 1; *(WORD *)(wav + 22) = 1;
        *(DWORD *)(wav + 24) = 22050; *(DWORD *)(wav + 28) = 22050; *(WORD *)(wav + 32) = 1; *(WORD *)(wav + 34) = 8;
        memcpy(wav + 36, "data", 4); *(DWORD *)(wav + 40) = n;
        for (i = 0; i < n; i++)
        {
            int amp = 90 * (int)(n - i) / (int)n;
            wav[44 + i] = (BYTE)(128 + (((i / 6) & 1) ? amp : -amp));
        }
        made = TRUE;
    }
    PlaySoundW((LPCWSTR)wav, NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

static void press_key(int idx)
{
    struct key *k = &keys[idx];
    INPUT in[16];
    int n = 0;
    WORD vk = (g_fn && k->fnvk) ? k->fnvk : k->vk, sc = 0;
    BOOL ext = (k->flags & F_EXT) != 0;

    click_sound();
    if (k->vk == VK_RMENU && g_has_altgr)
    {
        g_latched ^= M_ALTGR;
        redraw();
        return;
    }
    if (k->flags & F_MOD)
    {
        g_latched ^= k->mod;
        redraw();
        return;
    }
    if (k->sc && !(g_fn && k->fnvk))
    {
        /* the layout's key at this position, with its own scan code */
        UINT lvk = MapVirtualKeyExW(k->sc, MAPVK_VSC_TO_VK_EX, g_hkl);
        if (lvk) vk = (WORD)lvk;
        sc = k->sc;
    }
    /* the latched modifiers held round the key */
    if (g_latched & M_CTRL) add_input(in, &n, VK_LCONTROL, FALSE, FALSE);
    if (g_latched & M_ALT) add_input(in, &n, VK_LMENU, FALSE, FALSE);
    if (g_latched & M_WIN) add_input(in, &n, VK_LWIN, FALSE, TRUE);
    if (g_latched & M_ALTGR) { add_input(in, &n, VK_LCONTROL, FALSE, FALSE); add_input(in, &n, VK_RMENU, FALSE, TRUE); }
    if (g_latched & M_SHIFT) add_input(in, &n, VK_LSHIFT, FALSE, FALSE);
    add_input_sc(in, &n, vk, sc, FALSE, ext);
    add_input_sc(in, &n, vk, sc, TRUE, ext);
    if (g_latched & M_SHIFT) add_input(in, &n, VK_LSHIFT, TRUE, FALSE);
    if (g_latched & M_ALTGR) { add_input(in, &n, VK_RMENU, TRUE, TRUE); add_input(in, &n, VK_LCONTROL, TRUE, FALSE); }
    if (g_latched & M_WIN) add_input(in, &n, VK_LWIN, TRUE, TRUE);
    if (g_latched & M_ALT) add_input(in, &n, VK_LMENU, TRUE, FALSE);
    if (g_latched & M_CTRL) add_input(in, &n, VK_LCONTROL, TRUE, FALSE);
    SendInput(n, in, sizeof(INPUT));
    g_sent++;
#ifndef SG_MUTANT_STICKY
    g_latched = 0;
#endif
    redraw();
    /* Caps Lock's light: the toggle state arrives with the input */
    SetTimer(g_wnd, TIMER_STATE, 150, NULL);
}

/* ---- docking, moving, fading --------------------------------------------------------------- */
static void set_alpha(BYTE a)
{
    LONG ex = GetWindowLongW(g_wnd, GWL_EXSTYLE);
    if (a == g_alpha) return;
    g_alpha = a;
    if (a == 255 && (ex & WS_EX_LAYERED)) SetWindowLongW(g_wnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
    else if (a < 255)
    {
        if (!(ex & WS_EX_LAYERED)) SetWindowLongW(g_wnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
        SetLayeredWindowAttributes(g_wnd, 0, a, LWA_ALPHA);
    }
    write_dump();
}

static void move_to(BOOL top)
{
    RECT work, r;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    GetWindowRect(g_wnd, &r);
    SetWindowPos(g_wnd, HWND_TOPMOST, r.left, top ? work.top : work.bottom - (r.bottom - r.top), 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE);
    save_placement();
}

static void set_docked(BOOL on)
{
    APPBARDATA abd = { sizeof(abd) };
    abd.hWnd = g_wnd;
    if (on == g_docked) return;
    if (on)
    {
        RECT work;
        int h;
        GetWindowRect(g_wnd, &g_undocked);
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        h = (int)((work.right - work.left) * 5 / units_wide() * 0.62f) + g_title_h;
        if (h > (work.bottom - work.top) * 2 / 5) h = (work.bottom - work.top) * 2 / 5;
        abd.uCallbackMessage = WM_APPBAR;
        g_appbar = SHAppBarMessage(ABM_NEW, &abd) != 0;
        abd.uEdge = ABE_BOTTOM;
        SetRect(&abd.rc, work.left, work.bottom - h, work.right, work.bottom);
        if (g_appbar)
        {
            SHAppBarMessage(ABM_QUERYPOS, &abd);
            abd.rc.top = abd.rc.bottom - h;
            SHAppBarMessage(ABM_SETPOS, &abd);
        }
        g_docked = TRUE;
        SetWindowPos(g_wnd, HWND_TOPMOST, abd.rc.left, abd.rc.top, abd.rc.right - abd.rc.left, h, SWP_NOACTIVATE);
    }
    else
    {
        if (g_appbar) SHAppBarMessage(ABM_REMOVE, &abd);
        g_appbar = FALSE;
        g_docked = FALSE;
        SetWindowPos(g_wnd, HWND_TOPMOST, g_undocked.left, g_undocked.top, g_undocked.right - g_undocked.left,
                     g_undocked.bottom - g_undocked.top, SWP_NOACTIVATE);
    }
    reg_put(L"Dock", g_docked);
    layout();
    redraw();
}

/* ---- Options ------------------------------------------------------------------------------ */
enum { ID_SOUND = 100, ID_NAVKEYS, ID_NUMPAD, ID_CLICK, ID_HOVER, ID_PERIOD };

static WORD *dlg_align(WORD *p) { return (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3); }

static WORD *dlg_item(WORD *p, DWORD style, short x, short y, short cx, short cy, WORD id, WORD cls, const WCHAR *text)
{
    DLGITEMTEMPLATE *it;
    p = dlg_align(p);
    it = (DLGITEMTEMPLATE *)p;
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p = (WORD *)(it + 1);
    *p++ = 0xFFFF; *p++ = cls;
    wcscpy((WCHAR *)p, text); p += wcslen(text) + 1;
    *p++ = 0;
    return p;
}

static INT_PTR CALLBACK options_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static const DWORD periods[] = { 500, 750, 1000, 1500, 2000, 3000 };
    int i;
    (void)lp;
    switch (msg)
    {
    case WM_INITDIALOG:
        CheckDlgButton(dlg, ID_SOUND, g_click_sound);
        CheckDlgButton(dlg, ID_NAVKEYS, g_nav);
        CheckDlgButton(dlg, ID_NUMPAD, g_numpad);
        CheckRadioButton(dlg, ID_CLICK, ID_HOVER, g_hover ? ID_HOVER : ID_CLICK);
        for (i = 0; i < (int)ARRAYSIZE(periods); i++)
        {
            WCHAR s[16];
            swprintf(s, 16, L"%.2f seconds", periods[i] / 1000.0);
            SendDlgItemMessageW(dlg, ID_PERIOD, CB_ADDSTRING, 0, (LPARAM)s);
            if (periods[i] == g_hover_ms) SendDlgItemMessageW(dlg, ID_PERIOD, CB_SETCURSEL, i, 0);
        }
        if (SendDlgItemMessageW(dlg, ID_PERIOD, CB_GETCURSEL, 0, 0) < 0) SendDlgItemMessageW(dlg, ID_PERIOD, CB_SETCURSEL, 2, 0);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK)
        {
            g_click_sound = IsDlgButtonChecked(dlg, ID_SOUND) == BST_CHECKED;
            g_nav = IsDlgButtonChecked(dlg, ID_NAVKEYS) == BST_CHECKED;
            g_numpad = IsDlgButtonChecked(dlg, ID_NUMPAD) == BST_CHECKED;
            g_hover = IsDlgButtonChecked(dlg, ID_HOVER) == BST_CHECKED;
            i = (int)SendDlgItemMessageW(dlg, ID_PERIOD, CB_GETCURSEL, 0, 0);
            if (i >= 0) g_hover_ms = periods[i];
            save_settings();
            EndDialog(dlg, IDOK);
        }
        else if (LOWORD(wp) == IDCANCEL) EndDialog(dlg, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

static void show_options(void)
{
    static WORD buf[2048];
    WORD *p = buf;
    DLGTEMPLATE *t = (DLGTEMPLATE *)buf;
    BOOL had_numpad = g_numpad, had_nav = g_nav;
    memset(buf, 0, sizeof(buf));
    t->style = DS_MODALFRAME | DS_SETFONT | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    t->cdit = 10;
    t->cx = 230; t->cy = 170;
    p = (WORD *)(t + 1);
    *p++ = 0; *p++ = 0;
    wcscpy((WCHAR *)p, L"Options"); p += 8;
    *p++ = 9; wcscpy((WCHAR *)p, L"Segoe UI"); p += 9;
    p = dlg_item(p, BS_AUTOCHECKBOX | WS_TABSTOP, 10, 8, 210, 12, ID_SOUND, 0x80, L"&Use click sound");
    p = dlg_item(p, BS_AUTOCHECKBOX | WS_TABSTOP, 10, 22, 210, 12, ID_NAVKEYS, 0x80, L"Show &keys to make it easier to move around the screen");
    p = dlg_item(p, BS_AUTOCHECKBOX | WS_TABSTOP, 10, 36, 210, 12, ID_NUMPAD, 0x80, L"Turn on &numeric key pad");
    p = dlg_item(p, 0, 10, 56, 210, 10, 0xFFFF, 0x82, L"To use the On-Screen Keyboard:");
    p = dlg_item(p, BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP, 16, 68, 200, 12, ID_CLICK, 0x80, L"&Click on keys");
    p = dlg_item(p, BS_AUTORADIOBUTTON, 16, 82, 200, 12, ID_HOVER, 0x80, L"&Hover over keys");
    p = dlg_item(p, 0, 28, 98, 90, 10, 0xFFFF, 0x82, L"Hover &duration:");
    p = dlg_item(p, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 120, 96, 90, 80, ID_PERIOD, 0x85, L"");
    p = dlg_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 110, 146, 50, 14, IDOK, 0x80, L"OK");
    p = dlg_item(p, BS_PUSHBUTTON | WS_TABSTOP, 170, 146, 50, 14, IDCANCEL, 0x80, L"Cancel");
    if (DialogBoxIndirectW(g_inst, t, g_wnd, options_proc) == IDOK && (had_numpad != g_numpad || had_nav != g_nav))
    {
        /* the key pad widens the window, as it does on Windows */
        RECT r;
        float before = had_numpad ? PADX + 4 : RCOLX + 1.15f;
        GetWindowRect(g_wnd, &r);
        if (!g_docked)
            SetWindowPos(g_wnd, NULL, 0, 0, (int)((r.right - r.left) * units_wide() / before), r.bottom - r.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
    }
    redraw();
}

static void do_action(int action)
{
    switch (action)
    {
    case A_FN: g_fn = !g_fn; redraw(); break;
    case A_NAV: g_nav = !g_nav; save_settings(); layout(); redraw(); break;
    case A_UP: move_to(TRUE); break;
    case A_DOWN: move_to(FALSE); break;
    case A_DOCK: set_docked(!g_docked); break;
    case A_FADE: g_fade = !g_fade; save_settings(); redraw(); break;
    case A_OPTIONS: show_options(); break;
    case A_HELP:
        MessageBoxW(g_wnd,
            L"Click a key to type it into the program you are using -- the On-Screen Keyboard never takes "
            L"the keyboard away from it.\n\n"
            L"Shift, Ctrl, Alt and the Start key stay down for the next key you click. Fn shows F1-F12 on "
            L"the number keys. Nav shows the navigation keys; Mv Up and Mv Dn move the keyboard to the top or "
            L"bottom of the screen; Dock fits it across the bottom; Fade lets you see through it when the "
            L"pointer is elsewhere.\n\nWindows logo key + Ctrl + O turns the On-Screen Keyboard on or off.",
            L"On-Screen Keyboard", MB_OK | MB_ICONINFORMATION);
        break;
    }
}

static void activate_key(int idx)
{
    if (idx < 0) return;
    if (keys[idx].flags & F_ACT) do_action(keys[idx].action);
    else press_key(idx);
}

/* ---- the window --------------------------------------------------------------------------- */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        g_wnd = hwnd;
        refresh_layout();
        layout();
        SetTimer(hwnd, TIMER_STATE, 250, NULL);
        return 0;
#ifndef SG_MUTANT_ACTIVATE
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
#endif
    case WM_SIZE:
        layout();
        InvalidateRect(hwnd, NULL, FALSE);
        write_dump();
        return 0;
    case WM_MOVE:
        write_dump();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT cr;
        HDC mem;
        HBITMAP bmp, old;
        GetClientRect(hwnd, &cr);
        mem = CreateCompatibleDC(dc);
        bmp = CreateCompatibleBitmap(dc, cr.right, cr.bottom);
        old = SelectObject(mem, bmp);
        paint(mem);
        BitBlt(dc, 0, 0, cr.right, cr.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT cr;
        POINT p = { x, y };
        GetClientRect(hwnd, &cr);
        if (PtInRect(&g_btn_close, p) || PtInRect(&g_btn_min, p)) return 0;
        if (!g_docked && x >= cr.right - 14 && y >= cr.bottom - 14) g_drag = DRAG_SIZE;
        else if (!g_docked && y < g_title_h) g_drag = DRAG_MOVE;
        if (g_drag != DRAG_NONE)
        {
            GetCursorPos(&g_drag_origin);
            GetWindowRect(hwnd, &g_drag_rect);
            SetCapture(hwnd);
            return 0;
        }
        g_pressed = key_at(x, y);
        if (g_pressed >= 0) { SetCapture(hwnd); InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), h;
        if (g_drag != DRAG_NONE)
        {
            POINT pt;
            int dx, dy;
            GetCursorPos(&pt);
            dx = pt.x - g_drag_origin.x; dy = pt.y - g_drag_origin.y;
            if (g_drag == DRAG_MOVE)
                SetWindowPos(hwnd, NULL, g_drag_rect.left + dx, g_drag_rect.top + dy, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            else
                SetWindowPos(hwnd, NULL, 0, 0, max(360, g_drag_rect.right - g_drag_rect.left + dx),
                             max(160, g_drag_rect.bottom - g_drag_rect.top + dy), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        h = key_at(x, y);
        if (h != g_hot)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            g_hot = h;
            InvalidateRect(hwnd, NULL, FALSE);
            if (g_hover)
            {
                g_hover_key = h;
                KillTimer(hwnd, TIMER_HOVER);
                if (h >= 0) SetTimer(hwnd, TIMER_HOVER, g_hover_ms, NULL);
            }
        }
        if (g_fade) set_alpha(255);
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hot = -1;
        KillTimer(hwnd, TIMER_HOVER);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONUP:
    {
        POINT p = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (g_drag != DRAG_NONE)
        {
            g_drag = DRAG_NONE;
            ReleaseCapture();
            save_placement();
            write_dump();
            return 0;
        }
        if (PtInRect(&g_btn_close, p)) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        if (PtInRect(&g_btn_min, p)) { ShowWindow(hwnd, SW_SHOWMINNOACTIVE); return 0; }
        if (g_pressed >= 0)
        {
            int k = g_pressed;
            ReleaseCapture();
            g_pressed = -1;
            if (!g_hover && key_at(p.x, p.y) == k) activate_key(k);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_HOVER)
        {
            KillTimer(hwnd, TIMER_HOVER);
            if (g_hover && g_hover_key >= 0 && g_hover_key == g_hot) activate_key(g_hover_key);
            if (g_hover && g_hot >= 0) SetTimer(hwnd, TIMER_HOVER, g_hover_ms, NULL);
        }
        else if (wp == TIMER_STATE)
        {
            /* Caps Lock and Num Lock may change on the real keyboard too; fade when away */
            static int last_caps = -1;
            int c = caps_on();
            SetTimer(hwnd, TIMER_STATE, 250, NULL);
            static DWORD last_check;
            static HWND last_fg;
            if (c != last_caps) { last_caps = c; redraw(); }
            if (GetForegroundWindow() != last_fg || GetTickCount() - last_check >= 700)
            {
                last_fg = GetForegroundWindow();
                last_check = GetTickCount();
                layout_changed();
            }
            if (g_fade)
            {
                POINT pt; RECT wr;
                GetCursorPos(&pt); GetWindowRect(hwnd, &wr);
                set_alpha(PtInRect(&wr, pt) ? 255 : 110);
            }
            else set_alpha(255);
        }
        return 0;
    case WM_APPBAR:
        return 0;
    case WM_INPUTLANGCHANGE:
        layout_changed();
        break;
    case WM_CLOSE:
        save_placement();
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_appbar)
        {
            APPBARDATA abd = { sizeof(abd) };
            abd.hWnd = hwnd;
            SHAppBarMessage(ABM_REMOVE, &abd);
        }
        if (g_dump[0])
        {
            FILE *f = _wfopen(g_dump, L"w");
            if (f) { fwprintf(f, L"CLOSED\n"); fclose(f); }
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
    WNDCLASSEXW wc = { sizeof(wc) };
    HANDLE mutex;
    MSG msg;
    RECT work;
    int ki, si;

    for (ki = 0; ki < NKEYS; ki++)
        for (si = 0; si < (int)ARRAYSIZE(key_scans); si++)
            if (!wcscmp(keys[ki].name, key_scans[si].name)) keys[ki].sc = key_scans[si].sc;
    int x, y, w, h;
    DWORD n;
    LONG ex = WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_APPWINDOW;
    (void)prev; (void)cmdline; (void)show;

    g_inst = inst;
    n = GetEnvironmentVariableW(L"SG_OSK_DUMP", g_dump, MAX_PATH);
    if (!n || n >= MAX_PATH) g_dump[0] = 0;

    mutex = CreateMutexW(NULL, TRUE, L"Local\\StainedGlassOnScreenKeyboard");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND other = FindWindowW(CLASS_NAME, NULL);
        if (other) ShowWindow(other, IsIconic(other) ? SW_SHOWNOACTIVATE : SW_SHOWNA);
        return 0;
    }

    g_nav = reg_get(L"ShowNavigationKeys", 1) != 0;
    g_numpad = reg_get(L"ShowNumPad", 0) != 0;
    g_click_sound = reg_get(L"ClickSound", 0) != 0;
    g_hover = reg_get(L"Mode", 0) == 1;
    g_hover_ms = reg_get(L"HoverPeriod", 1000);
    if (g_hover_ms < 250 || g_hover_ms > 5000) g_hover_ms = 1000;
    g_fade = reg_get(L"Fade", 0) != 0;

    {
        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        ncm.lfCaptionFont.lfHeight = -13;
        ncm.lfCaptionFont.lfWeight = FW_NORMAL;
        g_font_title = CreateFontIndirectW(&ncm.lfCaptionFont);
    }

    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = CLASS_NAME;
    RegisterClassExW(&wc);

    /* Windows' place: centred across the bottom of the work area, about 70% of its width */
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    w = (work.right - work.left) * 72 / 100;
    if (w < 720) w = min(720, work.right - work.left);
    h = (int)(w / units_wide() * 5 * 0.72f) + g_title_h + 12;
    x = work.left + (work.right - work.left - w) / 2;
    y = work.bottom - h;
    w = reg_get(L"WindowWidth", w); h = reg_get(L"WindowHeight", h);
    x = reg_get(L"WindowLeft", x); y = reg_get(L"WindowTop", y);
    if (x + w > work.right || x < work.left - w / 2 || y < work.top || y + h > work.bottom + h / 2)
    {
        x = work.left + (work.right - work.left - w) / 2;
        y = work.bottom - h;
    }
#ifdef SG_MUTANT_ACTIVATE
    ex &= ~WS_EX_NOACTIVATE;
#endif
    g_wnd = CreateWindowExW(ex, CLASS_NAME, L"On-Screen Keyboard", WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU | WS_CLIPCHILDREN,
                            x, y, w, h, NULL, NULL, inst, NULL);
    if (!g_wnd) return 1;
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    if (reg_get(L"Dock", 0)) set_docked(TRUE);
    write_dump();

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}
