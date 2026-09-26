/* sg-terminal -- settings.json, kept as Windows Terminal keeps it.
 *
 * The file is %LOCALAPPDATA%\Microsoft\Windows Terminal\settings.json (where
 * Windows Terminal's unpackaged build keeps it; SG_TERMINAL_SETTINGS names
 * another). With none there, the packaged Windows Terminal's
 * (...\Packages\Microsoft.WindowsTerminal_8wekyb3d8bbwe\LocalState\) is read
 * instead, and written to ours; with neither, one is made -- from the old
 * HKCU\Software\Stained Glass\Terminal values (DefaultProfile, FontFace,
 * FontSize) when they exist.
 *
 * What is read (Windows Terminal's schema): defaultProfile (a GUID or a
 * name); profiles.defaults and profiles.list (or profiles as a plain list)
 * with guid, name, commandline, startingDirectory, icon, colorScheme (a name,
 * or {dark, light}: dark), font.face/font.size (or the older
 * fontFace/fontSize), hidden, source; schemes; actions and keybindings (the
 * older one-list form -- {command, keys} -- and the newer split one --
 * actions with ids, keybindings naming them), with "unbound" and null
 * taking a key away. Everything else in the file is kept as it is: the
 * document is rewritten only when something is added (the profiles found on
 * this machine, missing GUIDs) or the Settings dialog saves, and keeps every
 * key it had, known or not. Comments are lost on such a rewrite.
 *
 * Profiles found on the machine (PowerShell 7, Command Prompt, Git Bash,
 * Windows PowerShell) have fixed GUIDs and appear in the file as Windows
 * Terminal's own dynamic profiles do: guid, name, source, hidden -- their
 * command line is the one found; a file's "commandline" overrides it.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "wtsettings.h"
#include "json.h"
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REG_KEY L"Software\\Stained Glass\\Terminal"
#define SOURCE L"Stained Glass"

static jv *g_doc;

/* ---- helpers ------------------------------------------------------------------------------------- */
static char *utf8(const WCHAR *w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *s = malloc(n > 0 ? n : 1);
    if (!s) return NULL;
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    else s[0] = 0;
    return s;
}

static void wide(const char *s, WCHAR *out, int cch)
{
    if (!s) { out[0] = 0; return; }
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cch)) out[0] = 0;
    out[cch - 1] = 0;
}

static jv *jstrw(const WCHAR *w)
{
    char *s = utf8(w);
    jv *v = jstr(s ? s : "");
    free(s);
    return v;
}

static const char *get_str(const jv *o, const char *key) { return jgets(o, key); }

static BOOL parse_colour(const char *s, uint32_t *out)
{
    unsigned v;
    if (!s || s[0] != '#') return FALSE;
    if (strlen(s) == 7 && sscanf(s + 1, "%6x", &v) == 1) { *out = v; return TRUE; }
    if (strlen(s) == 4 && sscanf(s + 1, "%3x", &v) == 1) {
        *out = ((v >> 8) & 15) * 0x110000 + ((v >> 4) & 15) * 0x1100 + (v & 15) * 0x11;
        return TRUE;
    }
    return FALSE;
}

static void default_path(WCHAR *out, BOOL packaged)
{
    WCHAR base[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) &&
        FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) base[0] = 0;
    if (packaged)
        _snwprintf(out, MAX_PATH, L"%ls\\Packages\\Microsoft.WindowsTerminal_8wekyb3d8bbwe\\LocalState\\settings.json", base);
    else
        _snwprintf(out, MAX_PATH, L"%ls\\Microsoft\\Windows Terminal\\settings.json", base);
    out[MAX_PATH - 1] = 0;
}

static char *read_file(const WCHAR *path, size_t *len)
{
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD size, got = 0;
    char *buf;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size > 16 * 1024 * 1024 || !(buf = malloc(size + 1))) { CloseHandle(h); return NULL; }
    if (!ReadFile(h, buf, size, &got, NULL) || got != size) { free(buf); CloseHandle(h); return NULL; }
    CloseHandle(h);
    buf[size] = 0;
    *len = size;
    return buf;
}

static void make_dirs(const WCHAR *path)
{
    WCHAR dir[MAX_PATH], *p;
    lstrcpynW(dir, path, MAX_PATH);
    if ((p = wcsrchr(dir, L'\\'))) *p = 0;
    for (p = dir + 3; *p; p++) if (*p == L'\\') { *p = 0; CreateDirectoryW(dir, NULL); *p = L'\\'; }
    CreateDirectoryW(dir, NULL);
}

/* the document, written whole to a temporary file and renamed into place */
static BOOL write_doc(const WCHAR *path)
{
    WCHAR tmp[MAX_PATH + 8];
    char *text;
    HANDLE h;
    DWORD done;
    BOOL ok;
    if (!g_doc || !(text = json_write(g_doc))) return FALSE;
    make_dirs(path);
    _snwprintf(tmp, ARRAYSIZE(tmp), L"%ls.new", path);
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { free(text); return FALSE; }
    ok = WriteFile(h, text, (DWORD)strlen(text), &done, NULL) && done == strlen(text);
    CloseHandle(h);
    free(text);
    if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp);
    return ok;
}

/* a GUID of our own for a profile the file has without one */
static void new_guid(WCHAR *out)
{
    GUID g;
    if (FAILED(CoCreateGuid(&g))) memset(&g, 0x5a, sizeof(g));
    _snwprintf(out, 40, L"{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
               g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

/* ---- keys -------------------------------------------------------------------------------------------- */
static BOOL parse_keys(const char *s, int *mods, UINT *vk)
{
    char buf[64], *part, *save;
    lstrcpynA(buf, s, sizeof(buf));
    CharLowerA(buf);
    *mods = 0; *vk = 0;
    for (part = strtok_s(buf, "+", &save); part; part = strtok_s(NULL, "+", &save)) {
        static const struct { const char *name; UINT vk; } names[] = {
            { "tab", VK_TAB }, { "enter", VK_RETURN }, { "space", VK_SPACE }, { "esc", VK_ESCAPE }, { "escape", VK_ESCAPE },
            { "backspace", VK_BACK }, { "delete", VK_DELETE }, { "insert", VK_INSERT }, { "home", VK_HOME }, { "end", VK_END },
            { "pgup", VK_PRIOR }, { "pageup", VK_PRIOR }, { "pgdn", VK_NEXT }, { "pagedown", VK_NEXT }, { "up", VK_UP },
            { "down", VK_DOWN }, { "left", VK_LEFT }, { "right", VK_RIGHT }, { "plus", VK_OEM_PLUS }, { "minus", VK_OEM_MINUS },
            { "=", VK_OEM_PLUS }, { "-", VK_OEM_MINUS }, { ",", VK_OEM_COMMA }, { ".", VK_OEM_PERIOD }, { "/", VK_OEM_2 },
            { ";", VK_OEM_1 }, { "`", VK_OEM_3 }, { "[", VK_OEM_4 }, { "]", VK_OEM_6 }, { "\\", VK_OEM_5 }, { "'", VK_OEM_7 },
            { "numpad_plus", VK_ADD }, { "numpad_minus", VK_SUBTRACT },
        };
        size_t i;
        if (!strcmp(part, "ctrl")) { *mods |= TS_CTRL; continue; }
        if (!strcmp(part, "shift")) { *mods |= TS_SHIFT; continue; }
        if (!strcmp(part, "alt")) { *mods |= TS_ALT; continue; }
        if (!part[0]) { *vk = VK_OEM_PLUS; continue; }     /* "ctrl++" */
        for (i = 0; i < ARRAYSIZE(names); i++) if (!strcmp(part, names[i].name)) { *vk = names[i].vk; break; }
        if (i < ARRAYSIZE(names)) continue;
        if (part[0] == 'f' && part[1] >= '1' && part[1] <= '9') {
            int n = atoi(part + 1);
            if (n >= 1 && n <= 24) { *vk = VK_F1 + n - 1; continue; }
        }
        if (!part[1] && ((part[0] >= 'a' && part[0] <= 'z') || (part[0] >= '0' && part[0] <= '9'))) {
            *vk = (UINT)(part[0] >= 'a' ? part[0] - 'a' + 'A' : part[0]);
            continue;
        }
        return FALSE;
    }
    return *vk != 0;
}

static BOOL dir_of(const char *s, int *dx, int *dy)
{
    *dx = *dy = 0;
    if (!s) return FALSE;
    if (!_stricmp(s, "left")) *dx = -1;
    else if (!_stricmp(s, "right")) *dx = 1;
    else if (!_stricmp(s, "up")) *dy = -1;
    else if (!_stricmp(s, "down")) *dy = 1;
    else return FALSE;
    return TRUE;
}

/* a command -- "name" or {"action": "name", ...} -- into a */
static BOOL parse_command(const jv *cmd, struct ts_action *a)
{
    static const struct { const char *name; int action; } simple[] = {
        { "unbound", A_UNBOUND }, { "newTab", A_NEWTAB }, { "closePane", A_CLOSEPANE }, { "closeTab", A_CLOSETAB },
        { "nextTab", A_NEXTTAB }, { "prevTab", A_PREVTAB }, { "switchToTab", A_SWITCHTAB }, { "splitPane", A_SPLIT },
        { "duplicateTab", A_DUPLICATE }, { "moveFocus", A_MOVEFOCUS }, { "resizePane", A_RESIZE }, { "find", A_FIND },
        { "copy", A_COPY }, { "paste", A_PASTE }, { "openSettings", A_SETTINGS }, { "toggleFullscreen", A_FULLSCREEN },
        { "adjustFontSize", A_FONTUP }, { "resetFontSize", A_FONTRESET }, { "openNewTabDropdown", A_MENU },
        { "scrollUp", A_SCROLLUP }, { "scrollDown", A_SCROLLDOWN },
    };
    const char *name = NULL;
    size_t i;
    a->index = -1;
    if (!cmd || cmd->type == J_NULL) { a->action = A_UNBOUND; return TRUE; }
    if (cmd->type == J_STR) name = cmd->s;
    else if (cmd->type == J_OBJ) name = get_str(cmd, "action");
    if (!name) return FALSE;
    for (i = 0; i < ARRAYSIZE(simple); i++) if (!_stricmp(name, simple[i].name)) break;
    if (i == ARRAYSIZE(simple)) return FALSE;
    a->action = simple[i].action;
    if (cmd->type != J_OBJ) {
        if (a->action == A_SPLIT) a->split = 0;
        if (a->action == A_FONTUP) a->dx = 1;
        return a->action != A_MOVEFOCUS && a->action != A_RESIZE && a->action != A_SWITCHTAB;
    }
    wide(get_str(cmd, "profile"), a->profile, ARRAYSIZE(a->profile));
    if (jget(cmd, "index")) a->index = (int)jgetn(cmd, "index", -1);
    if (a->action == A_MOVEFOCUS || a->action == A_RESIZE) return dir_of(get_str(cmd, "direction"), &a->dx, &a->dy);
    if (a->action == A_FONTUP) {
        a->dx = (int)jgetn(cmd, "delta", 1);
        if (a->dx < 0) a->action = A_FONTDOWN;
    }
    if (a->action == A_SPLIT) {
        const char *sp = get_str(cmd, "split"), *mode = get_str(cmd, "splitMode");
        a->split = !sp || !_stricmp(sp, "auto") ? 0 : (!_stricmp(sp, "right") || !_stricmp(sp, "vertical") || !_stricmp(sp, "left")) ? 1 : 2;
        if (mode && !_stricmp(mode, "duplicate")) a->action = A_DUPLICATE;
    }
    return TRUE;
}

static void add_action(struct ts_settings *s, const char *keys, const struct ts_action *a)
{
    struct ts_action k = *a;
    int i;
    if (!keys || !parse_keys(keys, &k.mods, &k.vk)) return;
    /* a later binding of the same keys replaces an earlier one, as in Windows Terminal */
    for (i = 0; i < s->nactions; i++)
        if (s->actions[i].mods == k.mods && s->actions[i].vk == k.vk) { s->actions[i] = k; return; }
    if (s->nactions < TS_MAX_ACTIONS) s->actions[s->nactions++] = k;
}

static void bind_keys(struct ts_settings *s, const jv *keys, const struct ts_action *a)
{
    int i;
    if (!keys) return;
    if (keys->type == J_STR) add_action(s, keys->s, a);
    else if (keys->type == J_ARR) for (i = 0; i < keys->n; i++) if (keys->items[i]->type == J_STR) add_action(s, keys->items[i]->s, a);
}

static void read_actions(struct ts_settings *s)
{
    const char *lists[] = { "keybindings", "actions" };
    size_t l;
    int i;
    /* the older form: every entry has its command and keys */
    for (l = 0; l < 2; l++) {
        jv *arr = jget(g_doc, lists[l]);
        if (!arr || arr->type != J_ARR) continue;
        for (i = 0; i < arr->n; i++) {
            const jv *e = arr->items[i];
            struct ts_action a = { 0 };
            if (e->type != J_OBJ || !jget(e, "keys") || !jget(e, "command")) continue;
            if (parse_command(jget(e, "command"), &a)) bind_keys(s, jget(e, "keys"), &a);
        }
    }
    /* the newer form: keybindings name an action's id (null: take the key away) */
    {
        jv *kb = jget(g_doc, "keybindings"), *acts = jget(g_doc, "actions");
        if (kb && kb->type == J_ARR)
            for (i = 0; i < kb->n; i++) {
                const jv *e = kb->items[i], *id;
                struct ts_action a = { 0 };
                int j;
                if (e->type != J_OBJ || jget(e, "command") || !(id = jget(e, "id"))) continue;
                if (id->type == J_NULL) { a.action = A_UNBOUND; bind_keys(s, jget(e, "keys"), &a); continue; }
                if (id->type != J_STR || !acts || acts->type != J_ARR) continue;
                for (j = 0; j < acts->n; j++) {
                    const char *aid = get_str(acts->items[j], "id");
                    if (aid && !strcmp(aid, id->s) && parse_command(jget(acts->items[j], "command"), &a)) {
                        bind_keys(s, jget(e, "keys"), &a);
                        break;
                    }
                }
            }
    }
}

/* ---- schemes --------------------------------------------------------------------------------------- */
static void builtin_schemes(struct ts_settings *s)
{
    static const struct ts_scheme night = { L"Stained Glass Night",
        { 0x2A2635, 0xE0556A, 0x4FC98E, 0xE7C564, 0x5A8DF0, 0xB27CF0, 0x45C6D1, 0xD4CFDF,
          0x6E6880, 0xFF7A8C, 0x72E6AB, 0xFFE08A, 0x82ABFF, 0xCFA0FF, 0x6FE3EC, 0xFFFFFF },
        0x1D1A26, 0xE8E4F0, 0xC9A7FF, 0x55427E };
    /* the classic Windows console palette, which Windows Terminal calls Campbell */
    static const struct ts_scheme campbell = { L"Campbell",
        { 0x0C0C0C, 0xC50F1F, 0x13A10E, 0xC19C00, 0x0037DA, 0x881798, 0x3A96DD, 0xCCCCCC,
          0x767676, 0xE74856, 0x16C60C, 0xF9F1A5, 0x3B78FF, 0xB4009E, 0x61D6D6, 0xF2F2F2 },
        0x0C0C0C, 0xCCCCCC, 0xFFFFFF, 0x4A4A6A };
    s->schemes[0] = night;
    s->schemes[1] = campbell;
    s->nschemes = 2;
}

static void read_schemes(struct ts_settings *s)
{
    static const char *const names[16] = { "black", "red", "green", "yellow", "blue", "purple", "cyan", "white",
        "brightBlack", "brightRed", "brightGreen", "brightYellow", "brightBlue", "brightPurple", "brightCyan", "brightWhite" };
    jv *arr = jget(g_doc, "schemes");
    int i, k;
    if (!arr || arr->type != J_ARR) return;
    for (i = 0; i < arr->n; i++) {
        const jv *e = arr->items[i];
        struct ts_scheme sc = s->schemes[0], *dst = NULL;
        const char *name = get_str(e, "name");
        if (!name) continue;
        wide(name, sc.name, ARRAYSIZE(sc.name));
        for (k = 0; k < 16; k++) parse_colour(get_str(e, names[k]), &sc.table[k]);
        parse_colour(get_str(e, "background"), &sc.bg);
        parse_colour(get_str(e, "foreground"), &sc.fg);
        if (!parse_colour(get_str(e, "cursorColor"), &sc.cursor)) sc.cursor = sc.fg;
        parse_colour(get_str(e, "selectionBackground"), &sc.sel);
        for (k = 0; k < s->nschemes; k++) if (!lstrcmpiW(s->schemes[k].name, sc.name)) dst = &s->schemes[k];
        if (!dst && s->nschemes < TS_MAX_SCHEMES) dst = &s->schemes[s->nschemes++];
        if (dst) *dst = sc;
    }
}

const struct ts_scheme *ts_scheme(const struct ts_settings *s, const WCHAR *name)
{
    int i;
    if (name && name[0]) for (i = 0; i < s->nschemes; i++) if (!lstrcmpiW(s->schemes[i].name, name)) return &s->schemes[i];
    if (s->scheme[0]) for (i = 0; i < s->nschemes; i++) if (!lstrcmpiW(s->schemes[i].name, s->scheme)) return &s->schemes[i];
    return &s->schemes[0];
}

/* ---- profiles -------------------------------------------------------------------------------------- */
static void read_font(const jv *o, WCHAR *face, int *size)
{
    jv *f = jget(o, "font");
    const char *fc = f ? get_str(f, "face") : NULL;
    double sz = f ? jgetn(f, "size", 0) : 0;
    if (!fc) fc = get_str(o, "fontFace");
    if (!sz) sz = jgetn(o, "fontSize", 0);
    if (fc) wide(fc, face, LF_FACESIZE);
    if (sz >= 4 && sz <= 128) *size = (int)(sz + 0.5);
}

static void read_scheme_name(const jv *o, WCHAR *out, int cch)
{
    jv *c = jget(o, "colorScheme");
    if (!c) return;
    if (c->type == J_STR) wide(c->s, out, cch);
    else if (c->type == J_OBJ) wide(get_str(c, "dark"), out, cch);
}

/* the profiles object: {"defaults": {...}, "list": [...]}; a plain list is made one */
static jv *profiles_obj(void)
{
    jv *p = jget(g_doc, "profiles");
    if (p && p->type == J_ARR) {
        jv *o = jobj(), *list = jarr();
        int i;
        for (i = 0; i < p->n; i++) { jpush(list, p->items[i]); p->items[i] = NULL; }
        p->n = 0;
        jset(o, "defaults", jobj());
        jset(o, "list", list);
        jset(g_doc, "profiles", o);     /* frees the emptied array */
        p = o;
    }
    if (!p || p->type != J_OBJ) { p = jobj(); jset(g_doc, "profiles", p); }
    if (!jget(p, "defaults")) jset(p, "defaults", jobj());
    if (!jget(p, "list") || jget(p, "list")->type != J_ARR) jset(p, "list", jarr());
    return p;
}

/* a document for the first run: what the registry had, and a schema line as Windows Terminal writes */
static void new_doc(struct ts_settings *s, const struct ts_profile *det, int nd)
{
    WCHAR def[64] = L"", face[LF_FACESIZE] = L"";
    DWORD cb = sizeof(def), size = 0;
    jv *prof, *defaults;
    int i;
    g_doc = jobj();
    jset(g_doc, "$help", jstr("https://aka.ms/terminal-documentation"));
    jset(g_doc, "$schema", jstr("https://aka.ms/terminal-profiles-schema"));
    prof = profiles_obj();
    defaults = jget(prof, "defaults");
    if (!RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"DefaultProfile", RRF_RT_REG_SZ, NULL, def, &cb) && def[0])
        for (i = 0; i < nd; i++) if (!lstrcmpiW(det[i].name, def)) { jset(g_doc, "defaultProfile", jstrw(det[i].guid)); s->migrated = TRUE; }
    cb = sizeof(face);
    if (!RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"FontFace", RRF_RT_REG_SZ, NULL, face, &cb) && face[0]) {
        jv *f = jobj();
        jset(f, "face", jstrw(face));
        cb = sizeof(size);
        if (!RegGetValueW(HKEY_CURRENT_USER, REG_KEY, L"FontSize", RRF_RT_REG_DWORD, NULL, &size, &cb) && size >= 6 && size <= 72)
            jset(f, "size", jnum(size));
        jset(defaults, "font", f);
        s->migrated = TRUE;
    }
    if (!jget(g_doc, "defaultProfile") && nd) jset(g_doc, "defaultProfile", jstrw(det[0].guid));
    jset(g_doc, "schemes", jarr());
    jset(g_doc, "actions", jarr());
    s->created = TRUE;
}

void ts_load(struct ts_settings *s, const struct ts_profile *det, int nd)
{
    WCHAR packaged[MAX_PATH];
    char *text = NULL;
    size_t len = 0;
    BOOL dirty = FALSE, writable = TRUE;
    jv *prof, *list, *defaults;
    int i, j;

    memset(s, 0, sizeof(*s));
    builtin_schemes(s);
    if (!GetEnvironmentVariableW(L"SG_TERMINAL_SETTINGS", s->path, MAX_PATH)) default_path(s->path, FALSE);
    json_free(g_doc);
    g_doc = NULL;
    if ((text = read_file(s->path, &len))) {
        if (!(g_doc = json_parse(text, len))) {
            _snwprintf(s->error, ARRAYSIZE(s->error), L"%ls is not valid JSON: the built-in settings are used until it is fixed.", s->path);
            writable = FALSE;       /* never overwrite what the user is editing */
        }
    } else {
        default_path(packaged, TRUE);
        if ((text = read_file(packaged, &len)) && (g_doc = json_parse(text, len))) dirty = TRUE;    /* Windows Terminal's, taken over */
    }
    free(text);
    if (g_doc && g_doc->type != J_OBJ) {
        json_free(g_doc); g_doc = NULL;
        _snwprintf(s->error, ARRAYSIZE(s->error), L"%ls is not a settings object.", s->path);
        writable = FALSE;
    }
    if (!g_doc) { new_doc(s, det, nd); dirty = writable; }

    prof = profiles_obj();
    defaults = jget(prof, "defaults");
    list = jget(prof, "list");
    read_font(defaults, s->face, &s->size);
    read_scheme_name(defaults, s->scheme, ARRAYSIZE(s->scheme));

    /* the profiles found on this machine, in the file as dynamic profiles */
    for (i = 0; i < nd; i++) {
        char *g = utf8(det[i].guid);
        for (j = 0; j < list->n; j++) {
            const char *eg = get_str(list->items[j], "guid");
            if (eg && g && !_stricmp(eg, g)) break;
        }
        if (j == list->n) {
            jv *e = jobj();
            jset(e, "guid", jstrw(det[i].guid));
            jset(e, "hidden", jbool(0));
            jset(e, "name", jstrw(det[i].name));
            jset(e, "source", jstrw(SOURCE));
            jpush(list, e);
            dirty = TRUE;
        }
        free(g);
    }

    /* the list, in its order */
    for (j = 0; j < list->n && s->np < TS_MAX_PROFILES; j++) {
        jv *e = list->items[j];
        struct ts_profile *p = &s->p[s->np];
        const char *src, *cmd;
        const struct ts_profile *d = NULL;
        if (e->type != J_OBJ) continue;
        memset(p, 0, sizeof(*p));
        if (!get_str(e, "guid")) {
            new_guid(p->guid);
            jset(e, "guid", jstrw(p->guid));
            dirty = TRUE;
        } else wide(get_str(e, "guid"), p->guid, ARRAYSIZE(p->guid));
        for (i = 0; i < nd; i++) if (!lstrcmpiW(det[i].guid, p->guid)) d = &det[i];
        src = get_str(e, "source");
        cmd = get_str(e, "commandline");
        /* a dynamic profile whose generator is not here (Azure Cloud Shell, WSL, ...) or is gone */
        if (!d && !cmd && src) continue;
        if (d) *p = *d;
        p->detected = d != NULL;
        p->user = !d;
        if (get_str(e, "name")) wide(get_str(e, "name"), p->name, ARRAYSIZE(p->name));
        if (!p->name[0]) lstrcpyW(p->name, L"Profile");
        if (cmd) wide(cmd, p->cmd, ARRAYSIZE(p->cmd));
        if (!p->cmd[0]) lstrcpyW(p->cmd, L"cmd.exe");
        if (get_str(e, "startingDirectory")) wide(get_str(e, "startingDirectory"), p->dir, ARRAYSIZE(p->dir));
        if (get_str(e, "icon")) wide(get_str(e, "icon"), p->icon, ARRAYSIZE(p->icon));
        read_scheme_name(e, p->scheme, ARRAYSIZE(p->scheme));
        read_font(e, p->face, &p->size);
        p->hidden = jgetb(e, "hidden", FALSE);
#ifdef SG_MUTANT_NOUSERPROFILE
        if (p->user) continue;
#endif
        if (!d) {
            /* a badge for a profile of the user's own: its first letter on a colour of its name */
            unsigned h = 0;
            WCHAR *c;
            for (c = p->name; *c; c++) h = h * 31 + *c;
            p->letter = (WCHAR)(ULONG_PTR)CharUpperW((WCHAR *)(ULONG_PTR)p->name[0]);
            p->colour = RGB(0x40 + (h & 0x7f), 0x40 + ((h >> 8) & 0x7f), 0x40 + ((h >> 16) & 0x7f));
        }
        s->np++;
    }
    if (get_str(g_doc, "defaultProfile")) wide(get_str(g_doc, "defaultProfile"), s->def, ARRAYSIZE(s->def));
    read_schemes(s);
    read_actions(s);
    if (dirty && writable) write_doc(s->path);
}

BOOL ts_save_prefs(struct ts_settings *s, const WCHAR *default_guid, const WCHAR *face, int size)
{
    jv *prof, *defaults, *font;
    if (!g_doc || s->error[0]) return FALSE;
    prof = profiles_obj();
    defaults = jget(prof, "defaults");
    if (default_guid && default_guid[0]) jset(g_doc, "defaultProfile", jstrw(default_guid));
    font = jget(defaults, "font");
    if (!font || font->type != J_OBJ) { font = jobj(); jset(defaults, "font", font); }
    if (face && face[0]) jset(font, "face", jstrw(face));
    if (size >= 6 && size <= 72) jset(font, "size", jnum(size));
    if (default_guid) lstrcpynW(s->def, default_guid, ARRAYSIZE(s->def));
    if (face && face[0]) lstrcpynW(s->face, face, ARRAYSIZE(s->face));
    if (size >= 6 && size <= 72) s->size = size;
#ifdef SG_MUTANT_NOUNKNOWN
    {
        /* keep only what we know: loses the user's own keys */
        jv *keep = jobj();
        const char *known[] = { "$schema", "defaultProfile", "profiles", "schemes", "actions" };
        size_t k;
        for (k = 0; k < ARRAYSIZE(known); k++) {
            int i;
            for (i = 0; i < g_doc->n; i++)
                if (!strcmp(g_doc->keys[i], known[k])) { jset(keep, known[k], g_doc->items[i]); g_doc->items[i] = jnull(); }
        }
        json_free(g_doc);
        g_doc = keep;
    }
#endif
    return write_doc(s->path);
}

ULONGLONG ts_file_stamp(const struct ts_settings *s)
{
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(s->path, GetFileExInfoStandard, &fa)) return 0;
    return ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime) ^ fa.nFileSizeLow;
}
