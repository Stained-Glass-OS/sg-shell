/* sg-control -- Settings > Time & Language > Language: the person's keyboards.
 *
 * Windows keeps a person's keyboards (input methods) per user: added,
 * removed and put in order in Settings, switched with the Start key + Space
 * (and Left Alt + Shift unless turned off), in HKCU\Keyboard Layout\Preload
 * (and Substitutes, for a keyboard of another language than the person's)
 * and Toggle. A change takes effect at once, without signing out.
 *
 * Here the keyboards are the session keymap's layouts (XKB groups), which the
 * compositor owns: sg-session's sg-settingsctl keeps them for the user
 * (~/.config/stained-glass/keyboard, read at every sign-in) and gives them to
 * the compositor at once (KEYMAP); Wine's keyboard driver takes the new
 * layouts from the keymap (wine-sg 9153/9154), and Preload, Substitutes and
 * Toggle are written as Windows writes them, for the programs that read them.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"

/* the keyboards Settings offers: Windows' name and keyboard layout identifier (KLID), and the XKB
 * layout and variant that types as it does */
struct keyboard { const WCHAR *name; DWORD klid; const char *layout, *variant; };
static const struct keyboard KEYBOARDS[] = {
    { L"US", 0x00000409, "us", "" },
    { L"United States-International", 0x00020409, "us", "intl" },
    { L"United States-Dvorak", 0x00010409, "us", "dvorak" },
    { L"United Kingdom", 0x00000809, "gb", "" },
    { L"United Kingdom Extended", 0x00000452, "gb", "extd" },
    { L"Irish", 0x00001809, "ie", "" },
    { L"Canadian French", 0x00001009, "ca", "" },
    { L"Canadian Multilingual Standard", 0x00011009, "ca", "multix" },
    { L"German", 0x00000407, "de", "" },
    { L"Swiss German", 0x00000807, "ch", "" },
    { L"Swiss French", 0x0000100c, "ch", "fr" },
    { L"French", 0x0000040c, "fr", "" },
    { L"Belgian French", 0x0000080c, "be", "" },
    { L"Spanish", 0x0000040a, "es", "" },
    { L"Latin American", 0x0000080a, "latam", "" },
    { L"Italian", 0x00000410, "it", "" },
    { L"Portuguese", 0x00000816, "pt", "" },
    { L"Portuguese (Brazil ABNT)", 0x00000416, "br", "" },
    { L"Dutch", 0x00000413, "nl", "" },
    { L"Swedish", 0x0000041d, "se", "" },
    { L"Norwegian", 0x00000414, "no", "" },
    { L"Danish", 0x00000406, "dk", "" },
    { L"Finnish", 0x0000040b, "fi", "" },
    { L"Icelandic", 0x0000040f, "is", "" },
    { L"Polish (Programmers)", 0x00000415, "pl", "" },
    { L"Czech", 0x00000405, "cz", "" },
    { L"Slovak", 0x0000041b, "sk", "" },
    { L"Hungarian", 0x0000040e, "hu", "" },
    { L"Slovenian", 0x00000424, "si", "" },
    { L"Croatian", 0x0000041a, "hr", "" },
    { L"Estonian", 0x00000425, "ee", "" },
    { L"Latvian", 0x00000426, "lv", "" },
    { L"Lithuanian", 0x00000427, "lt", "" },
    { L"Turkish Q", 0x0000041f, "tr", "" },
    { L"Turkish F", 0x0001041f, "tr", "f" },
    { L"Greek", 0x00000408, "gr", "" },
    { L"Russian", 0x00000419, "ru", "" },
    { L"Ukrainian", 0x00000422, "ua", "" },
    { L"Hebrew", 0x0000040d, "il", "" },
    { L"Arabic (101)", 0x00000401, "ara", "" },
    { L"Japanese", 0x00000411, "jp", "" },
    { L"Korean", 0x00000412, "kr", "" },
};
#define NKEYBOARDS ((int)ARRAYSIZE(KEYBOARDS))
#define MAX_KEYBOARDS 4   /* the groups an XKB keymap has */

enum {
    CMD_ADD = CMD_PAGE_FIRST + 1, CMD_ALTSHIFT, CMD_REGION, CMD_TYPING,
    CMD_UP = CMD_PAGE_FIRST + 20, CMD_DOWN = CMD_PAGE_FIRST + 30, CMD_REMOVE = CMD_PAGE_FIRST + 40,
};

/* the person's keyboards as the session has them now */
static struct { char layout[16], variant[32]; } g_kbd[MAX_KEYBOARDS];
static int g_nkbd, g_current = -1;
static char g_options[256];          /* the keymap's other XKB options, kept as they are */
static BOOL g_altshift;
static int g_offer[NKEYBOARDS + 1];  /* the Add list's entries: KEYBOARDS index, -1 for its prompt */


static int find_keyboard(const char *layout, const char *variant)
{
    int i;
    for (i = 0; i < NKEYBOARDS; i++)
        if (!strcmp(KEYBOARDS[i].layout, layout) && !strcmp(KEYBOARDS[i].variant, variant)) return i;
    return -1;
}

/* field i of a comma-separated list */
static void list_item(const char *list, int i, char *out, int cch)
{
    const char *p = list, *e;
    int n;
    out[0] = 0;
    while (i-- > 0) { if (!(p = strchr(p, ','))) return; p++; }
    e = strchr(p, ',');
    n = e ? (int)(e - p) : (int)strlen(p);
    if (n >= cch) n = cch - 1;
    memcpy(out, p, n);
    out[n] = 0;
}

/* the keyboards from sg-settingsctl keyboard: KEYBOARD layouts \t variants \t options \t own; CURRENT n */
static BOOL load_keyboards(WCHAR *err, int cch)
{
    char *ans, buf[1024], layouts[256] = "", variants[256] = "", options[512] = "", *p;
    BOOL ok;
    int i;
    WCHAR w[512];

    g_nkbd = 0; g_current = -1; g_options[0] = 0; g_altshift = FALSE;
    ans = ctl_run(L"keyboard", &ok, err, cch, 10000);
    if (!ans || !ok || !ctl_line(ans, "KEYBOARD", NULL, buf, sizeof(buf))) { free(ans); return FALSE; }
    if (ctl_field(buf, 0, w, ARRAYSIZE(w))) WideCharToMultiByte(CP_UTF8, 0, w, -1, layouts, sizeof(layouts), NULL, NULL);
    if (ctl_field(buf, 1, w, ARRAYSIZE(w))) WideCharToMultiByte(CP_UTF8, 0, w, -1, variants, sizeof(variants), NULL, NULL);
    if (ctl_field(buf, 2, w, ARRAYSIZE(w))) WideCharToMultiByte(CP_UTF8, 0, w, -1, options, sizeof(options), NULL, NULL);
    if (ctl_line(ans, "CURRENT", NULL, buf, sizeof(buf))) g_current = atoi(buf);
    free(ans);
    for (i = 0; i < MAX_KEYBOARDS; i++) {
        list_item(layouts, i, g_kbd[g_nkbd].layout, sizeof(g_kbd[0].layout));
        if (!g_kbd[g_nkbd].layout[0]) break;
        list_item(variants, i, g_kbd[g_nkbd].variant, sizeof(g_kbd[0].variant));
        g_nkbd++;
    }
    if (!g_nkbd) { strcpy(g_kbd[0].layout, "us"); g_kbd[0].variant[0] = 0; g_nkbd = 1; }
    /* the switching keys are this page's (grp:...): the rest stay as they were */
    for (p = strtok(options, ","); p; p = strtok(NULL, ",")) {
        if (!strncmp(p, "grp:", 4)) { if (!strcmp(p, "grp:alt_shift_toggle")) g_altshift = TRUE; continue; }
#ifdef SG_MUTANT_KBD_DROP_OPTIONS
        continue;
#endif
        if (strlen(g_options) + strlen(p) + 2 >= sizeof(g_options)) break;
        if (g_options[0]) strcat(g_options, ",");
        strcat(g_options, p);
    }
    /* with one keyboard the keymap has no switching keys: the choice is in Toggle, as Windows keeps it */
    if (g_nkbd < 2) {
        WCHAR v[8] = L"";
        reg_sz(HKEY_CURRENT_USER, L"Keyboard Layout\\Toggle", L"Language Hotkey", v, ARRAYSIZE(v));
        g_altshift = !lstrcmpW(v, L"1");
    }
    return TRUE;
}

static void keyboard_name(int i, WCHAR *out, int cch)
{
    int k = find_keyboard(g_kbd[i].layout, g_kbd[i].variant);
    if (k >= 0) lstrcpynW(out, KEYBOARDS[k].name, cch);
    else if (g_kbd[i].variant[0]) _snwprintf(out, cch, L"%hs (%hs)", g_kbd[i].layout, g_kbd[i].variant);
    else _snwprintf(out, cch, L"%hs", g_kbd[i].layout);
}

/* Preload, Substitutes and Toggle, as Windows writes them: Preload's values 1, 2 ... are the keyboards in
 * order -- a keyboard whose layout is the person's language's own by its KLID, else a substitute
 * "d00Nllll" (llll the input language) that Substitutes maps to the keyboard's KLID */
static void write_registry(void)
{
    LANGID lang = LANGIDFROMLCID(GetUserDefaultLCID());
    HKEY pre, sub, tog;
    WCHAR name[16], data[16];
    int i, nsub = 0;

    RegDeleteTreeW(HKEY_CURRENT_USER, L"Keyboard Layout\\Preload");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Keyboard Layout\\Substitutes");
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Keyboard Layout\\Preload", 0, NULL, 0, KEY_SET_VALUE, NULL, &pre, NULL)) return;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Keyboard Layout\\Substitutes", 0, NULL, 0, KEY_SET_VALUE, NULL, &sub, NULL)) {
        RegCloseKey(pre);
        return;
    }
    for (i = 0; i < g_nkbd; i++) {
        int k = find_keyboard(g_kbd[i].layout, g_kbd[i].variant);
        DWORD klid = k >= 0 ? KEYBOARDS[k].klid : lang;
        _snwprintf(name, ARRAYSIZE(name), L"%d", i + 1);
#ifndef SG_MUTANT_KBD_NO_SUBST
        if (klid != lang) {
            WCHAR subst[16];
            _snwprintf(subst, ARRAYSIZE(subst), L"d%03x%04x", ++nsub, lang);
            _snwprintf(data, ARRAYSIZE(data), L"%08lx", (unsigned long)klid);
            RegSetValueExW(sub, subst, 0, REG_SZ, (const BYTE *)data, (lstrlenW(data) + 1) * sizeof(WCHAR));
            RegSetValueExW(pre, name, 0, REG_SZ, (const BYTE *)subst, (lstrlenW(subst) + 1) * sizeof(WCHAR));
            continue;
        }
#endif
        _snwprintf(data, ARRAYSIZE(data), L"%08lx", (unsigned long)klid);
        RegSetValueExW(pre, name, 0, REG_SZ, (const BYTE *)data, (lstrlenW(data) + 1) * sizeof(WCHAR));
    }
    (void)nsub;
    RegCloseKey(pre);
    RegCloseKey(sub);
    /* Toggle: "1" Left Alt + Shift, "3" not assigned */
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, L"Keyboard Layout\\Toggle", 0, NULL, 0, KEY_SET_VALUE, NULL, &tog, NULL)) {
        const WCHAR *v = g_altshift ? L"1" : L"3";
        RegSetValueExW(tog, L"Hotkey", 0, REG_SZ, (const BYTE *)v, 4);
        RegSetValueExW(tog, L"Language Hotkey", 0, REG_SZ, (const BYTE *)v, 4);
        RegCloseKey(tog);
    }
}

/* the keyboards to the session (sg-settingsctl keyboard set) and the registry; the page again */
static void apply_keyboards(void)
{
    WCHAR args[1024], err[256], layouts[128] = L"", variants[256] = L"", options[512];
    char *ans, buf[64];
    BOOL ok, any_variant = FALSE;
    int i;

    for (i = 0; i < g_nkbd; i++) {
        WCHAR one[64];
        _snwprintf(one, ARRAYSIZE(one), L"%ls%hs", i ? L"," : L"", g_kbd[i].layout);
        lstrcatW(layouts, one);
        _snwprintf(one, ARRAYSIZE(one), L"%ls%hs", i ? L"," : L"", g_kbd[i].variant);
        lstrcatW(variants, one);
        if (g_kbd[i].variant[0]) any_variant = TRUE;
    }
    _snwprintf(options, ARRAYSIZE(options), L"%hs", g_options);
    if (g_nkbd > 1) {
        /* the Start key + Space always switches, as on Windows; Left Alt + Shift if chosen */
        if (options[0]) lstrcatW(options, L",");
        lstrcatW(options, L"grp:win_space_toggle");
        if (g_altshift) lstrcatW(options, L",grp:alt_shift_toggle");
    }
    _snwprintf(args, ARRAYSIZE(args), L"keyboard set %ls %ls %ls", layouts, any_variant ? variants : L"-",
               options[0] ? options : L"-");
    ans = ctl_run(args, &ok, err, ARRAYSIZE(err), 15000);
    if (!ok) {
        free(ans);
        st_status(err[0] ? err : L"Your keyboards could not be changed.");
        return;
    }
    write_registry();
    if (ans && ctl_line(ans, "APPLIED", NULL, buf, sizeof(buf)) && !strcmp(buf, "no"))
        st_status(L"Your keyboards change the next time you sign in.");
    else
        st_status(L"");
    free(ans);
}

void set_build_language(void)
{
    static const WCHAR *items[NKEYBOARDS + 1];
    WCHAR err[256] = L"", name[96], sub[96];
    int y = st_title(L"Language"), i, n = 0;
    HWND c;

    if (!load_keyboards(err, ARRAYSIZE(err))) {
        y = st_head(y, L"Keyboards");
        st_para(y, err[0] ? err : L"Keyboard settings are not available right now.");
        return;
    }
    y = st_head(y, L"Keyboards");
    y = st_para(y, g_nkbd > 1 ? L"Press the Start key + Space to switch to your next keyboard. Programs start with the first."
                              : L"Add a keyboard to type in another layout. Press the Start key + Space to switch between them.");
    for (i = 0; i < g_nkbd; i++) {
        int bx = st_x() + st_w() - S(3 * 96 + 10);
        keyboard_name(i, name, ARRAYSIZE(name));
        sub[0] = 0;
        if (i == 0) lstrcpyW(sub, L"Default keyboard");
        if (i == g_current && g_nkbd > 1) lstrcatW(sub, sub[0] ? L" \x2022 Typing now" : L"Typing now");
        pg_fill(st_x(), y, st_w(), S(64), g_pal.card);
        pg_icon(st_x() + S(14), y + S(14), S(36), IC_G_DEVICES);
        pg_text(st_x() + S(64), y + S(10), bx - st_x() - S(72), S(22), g_font_head, COL_TEXT, name, DT_SINGLELINE | DT_END_ELLIPSIS);
        if (sub[0]) pg_text(st_x() + S(64), y + S(34), bx - st_x() - S(72), S(20), g_font_small, COL_SUBTLE, sub, DT_SINGLELINE | DT_END_ELLIPSIS);
        c = pg_control(L"BUTTON", L"Move up", WS_TABSTOP | BS_PUSHBUTTON, bx, y + S(17), S(90), S(30), CMD_UP + i);
        EnableWindow(c, i > 0);
        c = pg_control(L"BUTTON", L"Move down", WS_TABSTOP | BS_PUSHBUTTON, bx + S(96), y + S(17), S(90), S(30), CMD_DOWN + i);
        EnableWindow(c, i < g_nkbd - 1);
        c = pg_control(L"BUTTON", L"Remove", WS_TABSTOP | BS_PUSHBUTTON, bx + S(192), y + S(17), S(90), S(30), CMD_REMOVE + i);
        EnableWindow(c, g_nkbd > 1);
        y += S(72);
    }
    y += S(6);
    /* the keyboards not added yet */
    items[n] = L"Add a keyboard"; g_offer[n++] = -1;
    for (i = 0; i < NKEYBOARDS; i++) {
        int j;
        for (j = 0; j < g_nkbd; j++)
            if (!strcmp(g_kbd[j].layout, KEYBOARDS[i].layout) && !strcmp(g_kbd[j].variant, KEYBOARDS[i].variant)) break;
        if (j < g_nkbd) continue;
        items[n] = KEYBOARDS[i].name; g_offer[n++] = i;
    }
    c = st_combo(&y, NULL, items, n, 0, CMD_ADD);
    EnableWindow(c, g_nkbd < MAX_KEYBOARDS);
    if (g_nkbd >= MAX_KEYBOARDS) y = st_para(y - S(6), L"You can have up to four keyboards. Remove one to add another.");
    y = st_head(y, L"Switching keyboards");
    st_toggle(&y, L"Left Alt + Shift also switches to the next keyboard", g_altshift, CMD_ALTSHIFT);
    y = st_head(y, L"Related settings");
    st_link(&y, L"Region and formats", CMD_REGION);
    st_link(&y, L"Typing", CMD_TYPING);
}

BOOL set_cmd_language(int id, int code, HWND ctl)
{
    int i;
    if (id == CMD_ADD) {
        if (code == CBN_SELCHANGE) {
            int sel = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0), k;
            if (sel > 0 && sel <= NKEYBOARDS && (k = g_offer[sel]) >= 0 && g_nkbd < MAX_KEYBOARDS) {
                strcpy(g_kbd[g_nkbd].layout, KEYBOARDS[k].layout);
                strcpy(g_kbd[g_nkbd].variant, KEYBOARDS[k].variant);
                g_nkbd++;
                apply_keyboards();
            }
        }
        return TRUE;
    }
    if (id == CMD_ALTSHIFT) { g_altshift = st_checked(ctl); apply_keyboards(); return TRUE; }
    if (id == CMD_REGION) { navigate(PG_S_REGION); return TRUE; }
    if (id == CMD_TYPING) { navigate(PG_S_TYPING); return TRUE; }
    if (id >= CMD_UP && id < CMD_UP + MAX_KEYBOARDS && (i = id - CMD_UP) > 0 && i < g_nkbd) {
        char t[sizeof(g_kbd[0])];
        memcpy(t, &g_kbd[i - 1], sizeof(t)); memcpy(&g_kbd[i - 1], &g_kbd[i], sizeof(t)); memcpy(&g_kbd[i], t, sizeof(t));
        apply_keyboards();
        return TRUE;
    }
    if (id >= CMD_DOWN && id < CMD_DOWN + MAX_KEYBOARDS && (i = id - CMD_DOWN) < g_nkbd - 1) {
        char t[sizeof(g_kbd[0])];
        memcpy(t, &g_kbd[i + 1], sizeof(t)); memcpy(&g_kbd[i + 1], &g_kbd[i], sizeof(t)); memcpy(&g_kbd[i], t, sizeof(t));
        apply_keyboards();
        return TRUE;
    }
    if (id >= CMD_REMOVE && id < CMD_REMOVE + MAX_KEYBOARDS && (i = id - CMD_REMOVE) < g_nkbd && g_nkbd > 1) {
        memmove(&g_kbd[i], &g_kbd[i + 1], (g_nkbd - i - 1) * sizeof(g_kbd[0]));
        g_nkbd--;
        apply_keyboards();
        return TRUE;
    }
    return FALSE;
}
