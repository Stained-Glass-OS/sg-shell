/* sg-terminal -- settings.json, as Windows Terminal keeps it (see wtsettings.c).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_WTSETTINGS_H
#define SG_WTSETTINGS_H
#include <windows.h>
#include <stdint.h>

#define TS_MAX_PROFILES 32
#define TS_MAX_SCHEMES  24
#define TS_MAX_ACTIONS  96

struct ts_profile {
    WCHAR guid[40], name[64], cmd[1088], dir[MAX_PATH], icon[MAX_PATH], scheme[64], face[LF_FACESIZE];
    int size;                   /* points; 0: the default */
    BOOL hidden, detected, user;
    WCHAR letter;               /* the tab badge when there is no icon */
    COLORREF colour;
};

struct ts_scheme {
    WCHAR name[64];
    uint32_t table[16], bg, fg, cursor, sel;
};

enum { A_NONE, A_UNBOUND, A_NEWTAB, A_CLOSEPANE, A_CLOSETAB, A_NEXTTAB, A_PREVTAB, A_SWITCHTAB, A_SPLIT, A_DUPLICATE,
       A_MOVEFOCUS, A_RESIZE, A_FIND, A_COPY, A_PASTE, A_SETTINGS, A_FULLSCREEN, A_FONTUP, A_FONTDOWN, A_FONTRESET,
       A_MENU, A_SCROLLUP, A_SCROLLDOWN };

#define TS_CTRL  1
#define TS_SHIFT 2
#define TS_ALT   4

struct ts_action {
    int mods;
    UINT vk;
    int action;
    WCHAR profile[64];          /* newTab / splitPane: a profile's name or GUID */
    int index;                  /* newTab / switchToTab: -1 none */
    int dx, dy;                 /* moveFocus / resizePane */
    int split;                  /* splitPane: 1 right (side by side), 2 down, 0 auto */
};

struct ts_settings {
    WCHAR path[MAX_PATH];       /* the settings.json in use */
    WCHAR error[256];           /* why the file could not be read ("" when it was) */
    WCHAR def[64];              /* defaultProfile: a GUID or a name */
    WCHAR face[LF_FACESIZE];    /* profiles.defaults.font.face ("" none) */
    int size;                   /* profiles.defaults.font.size (0 none) */
    WCHAR scheme[64];           /* profiles.defaults.colorScheme */
    struct ts_profile p[TS_MAX_PROFILES];
    int np;
    struct ts_scheme schemes[TS_MAX_SCHEMES];
    int nschemes;
    struct ts_action actions[TS_MAX_ACTIONS];
    int nactions;
    BOOL migrated, created;     /* this load wrote a new file (from the old registry settings) */
};

/* Reads settings.json (creating it on first run, from HKCU\Software\Stained Glass\Terminal
 * when that exists), merges the profiles found on the machine (detected[]: their GUIDs,
 * names, command lines) into it -- writing the file back when that added any -- and
 * fills s: the profiles in the file's order, built-in and the file's schemes, actions. */
void ts_load(struct ts_settings *s, const struct ts_profile *detected, int ndetected);

/* Saves the Settings dialog's choices into the file, keeping everything else in it. */
BOOL ts_save_prefs(struct ts_settings *s, const WCHAR *default_guid, const WCHAR *face, int size);

/* The file's last write time, to notice edits (0 when there is none). */
ULONGLONG ts_file_stamp(const struct ts_settings *s);

const struct ts_scheme *ts_scheme(const struct ts_settings *s, const WCHAR *name);

#endif
