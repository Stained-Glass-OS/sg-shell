/* sg-control -- Settings > Time & Language: Date & time, Region (Speech is
 * the Control Panel's Speech Recognition page, shown in this frame).
 *
 * The clock and the zone are the machine's: changing them is root's, through
 * the elevated copy of this program and sg-admind (timezone, ntp, time).
 * The region and formats are the user's, where Windows keeps them.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "settings.h"

enum {
    CMD_AUTO = CMD_PAGE_FIRST + 1, CMD_ZONE, CMD_CHANGE = SHIELD_ID(CMD_PAGE_FIRST + 3), CMD_DATE_CPL = CMD_PAGE_FIRST + 4,
    CMD_REGION_CPL = CMD_PAGE_FIRST + 5,
};
static WCHAR **g_zones;
static int g_nzones;

static void clock_text(WCHAR *out, int cch)
{
    SYSTEMTIME t;
    WCHAR d[64], h[32];
    GetLocalTime(&t);
    GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &t, NULL, h, ARRAYSIZE(h));
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &t, NULL, d, ARRAYSIZE(d));
    _snwprintf(out, cch, L"%ls   %ls", h, d);
}

void set_build_datetime(void)
{
    struct zone_facts z;
    WCHAR now[128], line[300];
    const WCHAR **items;
    int y = st_title(L"Date & time"), i, sel = -1;
    BOOL ntp;
    HWND c;
    zone_get(&z);
    ntp = z.ntp;
    y = st_head(y, L"Current date and time");
    clock_text(now, ARRAYSIZE(now));
    pg_control(L"STATIC", now, SS_NOPREFIX, st_x(), y, st_w(), S(26), CMD_PAGE_FIRST + 90);
    y += S(40);
    st_toggle(&y, L"Set time automatically", ntp, CMD_AUTO);
    y = st_text(y, L"Set the date and time manually");
    c = st_button(&y, L"Change", CMD_CHANGE);
    EnableWindow(c, !ntp);
    y = st_head(y, L"Time zone");
    if (!g_zones) g_nzones = load_zones(&g_zones);
    items = malloc((g_nzones + 1) * sizeof(*items));
    if (items) {
        for (i = 0; i < g_nzones; i++) { items[i] = g_zones[i]; if (!lstrcmpW(g_zones[i], z.iana)) sel = i; }
        st_combo(&y, NULL, items, g_nzones, sel, CMD_ZONE);
        free(items);
    }
    _snwprintf(line, ARRAYSIZE(line), L"(%ls) %ls", z.offset, z.display);
    y = st_para(y - S(6), line);
    if (z.dst[0]) y = st_para(y, z.dst);
    y = st_head(y, L"Related settings");
    st_link(&y, L"Date, time, and regional formatting", CMD_REGION_CPL);
    st_link(&y, L"Add clocks for different time zones", CMD_DATE_CPL);
    pg_timer(1000);
}

void set_timer_datetime(void)
{
    WCHAR now[128];
    HWND c = GetDlgItem(g_page, CMD_PAGE_FIRST + 90);
    clock_text(now, ARRAYSIZE(now));
    if (c) SetWindowTextW(c, now);
}

BOOL set_cmd_datetime(int id, int code, HWND ctl)
{
    WCHAR args[200];
    switch (id) {
    case CMD_AUTO:
        _snwprintf(args, ARRAYSIZE(args), L"/admin ntp %ls", st_checked(ctl) ? L"on" : L"off");
        if (run_elevated(args)) refresh_when_back(); else refresh_page();
        return TRUE;
    case CMD_CHANGE: if (run_elevated(L"/admin set-time")) refresh_when_back(); return TRUE;
    case CMD_ZONE:
        if (code == CBN_SELCHANGE) {
            int i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < g_nzones) {
                _snwprintf(args, ARRAYSIZE(args), L"/admin set-zone \"%ls\"", g_zones[i]);
                if (run_elevated(args)) refresh_when_back(); else refresh_page();
            }
        }
        return TRUE;
    case CMD_REGION_CPL: navigate(PG_S_REGION); return TRUE;
    case CMD_DATE_CPL: ShellExecuteW(NULL, NULL, L"ms-clock:", NULL, NULL, SW_SHOWNORMAL); return TRUE;
    }
    return FALSE;
}

/* ---- Region -------------------------------------------------------------------------------------- */
static const WCHAR INTL[] = L"Control Panel\\International";
static WCHAR g_locales[800][LOCALE_NAME_MAX_LENGTH], g_locale_names[800][96];
static int g_nlocales;
static GEOID g_geos[300];
static WCHAR g_geo_names[300][64];
static int g_ngeos;
enum { CMD_COUNTRY = CMD_PAGE_FIRST + 1, CMD_FORMAT };

static BOOL CALLBACK add_locale(LPWSTR name, DWORD flags, LPARAM lp)
{
    WCHAR geo[16];
    GEOID id;
    int i;
    (void)flags; (void)lp;
    if (g_nlocales >= (int)ARRAYSIZE(g_locales) || !wcschr(name, L'-') || wcschr(name, L'_')) return TRUE;
    lstrcpynW(g_locales[g_nlocales], name, LOCALE_NAME_MAX_LENGTH);
    if (!GetLocaleInfoEx(name, LOCALE_SLOCALIZEDDISPLAYNAME, g_locale_names[g_nlocales], 96))
        lstrcpynW(g_locale_names[g_nlocales], name, 96);
    g_nlocales++;
    /* the countries are the locales' (Wine's GetGeoInfo has no names) */
    if (GetLocaleInfoEx(name, LOCALE_IGEOID, geo, ARRAYSIZE(geo)) && (id = _wtoi(geo)) > 0 && g_ngeos < (int)ARRAYSIZE(g_geos)) {
        for (i = 0; i < g_ngeos; i++) if (g_geos[i] == id) return TRUE;
        if (!GetLocaleInfoEx(name, LOCALE_SLOCALIZEDCOUNTRYNAME, g_geo_names[g_ngeos], 64) || !g_geo_names[g_ngeos][0]) return TRUE;
        g_geos[g_ngeos++] = id;
    }
    return TRUE;
}

static int cmp_names(const void *a, const void *b) { return lstrcmpiW(*(const WCHAR *const *)a, *(const WCHAR *const *)b); }

void set_build_region(void)
{
    static const WCHAR *litems[800], *gitems[300];
    WCHAR cur[LOCALE_NAME_MAX_LENGTH] = L"", sample[160], buf[64];
    SYSTEMTIME now;
    GEOID geo = GetUserGeoID(GEOCLASS_NATION);
    int y = st_title(L"Region"), i, lsel = -1, gsel = -1;
    if (!g_nlocales) {
        EnumSystemLocalesEx(add_locale, LOCALE_SPECIFICDATA, 0, NULL);
        /* by name, keeping the pair arrays together */
        for (i = 1; i < g_nlocales; i++) {
            int j = i;
            while (j > 0 && lstrcmpiW(g_locale_names[j - 1], g_locale_names[j]) > 0) {
                WCHAR t[96], u[LOCALE_NAME_MAX_LENGTH];
                lstrcpyW(t, g_locale_names[j]); lstrcpyW(g_locale_names[j], g_locale_names[j - 1]); lstrcpyW(g_locale_names[j - 1], t);
                lstrcpyW(u, g_locales[j]); lstrcpyW(g_locales[j], g_locales[j - 1]); lstrcpyW(g_locales[j - 1], u);
                j--;
            }
        }
    }
    if (g_ngeos) {
        for (i = 1; i < g_ngeos; i++) {
            int j = i;
            while (j > 0 && lstrcmpiW(g_geo_names[j - 1], g_geo_names[j]) > 0) {
                WCHAR t[64]; GEOID g;
                lstrcpyW(t, g_geo_names[j]); lstrcpyW(g_geo_names[j], g_geo_names[j - 1]); lstrcpyW(g_geo_names[j - 1], t);
                g = g_geos[j]; g_geos[j] = g_geos[j - 1]; g_geos[j - 1] = g;
                j--;
            }
        }
    }
    (void)cmp_names;
    GetUserDefaultLocaleName(cur, ARRAYSIZE(cur));
    reg_sz(HKEY_CURRENT_USER, INTL, L"LocaleName", cur, ARRAYSIZE(cur));
    for (i = 0; i < g_nlocales; i++) { litems[i] = g_locale_names[i]; if (!lstrcmpiW(g_locales[i], cur)) lsel = i; }
    for (i = 0; i < g_ngeos; i++) { gitems[i] = g_geo_names[i]; if (g_geos[i] == geo) gsel = i; }
    y = st_head(y, L"Country or region");
    st_combo(&y, L"Windows and apps might use your country or region to give you local content", gitems, g_ngeos, gsel, CMD_COUNTRY);
    y = st_head(y, L"Regional format");
    st_combo(&y, L"Windows formats dates and times based on your language and regional preferences.", litems, g_nlocales, lsel, CMD_FORMAT);
    y = st_head(y, L"Regional format data");
    GetLocalTime(&now);
    if (GetDateFormatEx(cur, DATE_SHORTDATE, &now, NULL, buf, ARRAYSIZE(buf), NULL)) { y = st_row(y, L"Short date", buf); }
    if (GetDateFormatEx(cur, DATE_LONGDATE, &now, NULL, sample, ARRAYSIZE(sample), NULL)) { y = st_row(y, L"Long date", sample); }
    if (GetTimeFormatEx(cur, TIME_NOSECONDS, &now, NULL, buf, ARRAYSIZE(buf))) { y = st_row(y, L"Short time", buf); }
    if (GetLocaleInfoEx(cur, LOCALE_SCURRENCY, buf, ARRAYSIZE(buf))) { y = st_row(y, L"Currency", buf); }
    y = st_para(y + S(6), L"Programs pick up a new format when they start again.");
}

BOOL set_cmd_region(int id, int code, HWND ctl)
{
    int i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
    if (code != CBN_SELCHANGE) return id == CMD_COUNTRY || id == CMD_FORMAT;
    if (id == CMD_COUNTRY && i >= 0 && i < g_ngeos) { SetUserGeoID(g_geos[i]); refresh_page(); return TRUE; }
    if (id == CMD_FORMAT && i >= 0 && i < g_nlocales) {
        WCHAR lcid[16];
        DWORD_PTR r;
        _snwprintf(lcid, ARRAYSIZE(lcid), L"%08lx", LocaleNameToLCID(g_locales[i], 0));
        reg_set_sz(HKEY_CURRENT_USER, INTL, L"LocaleName", g_locales[i]);
        reg_set_sz(HKEY_CURRENT_USER, INTL, L"Locale", lcid);
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"intl", SMTO_ABORTIFHUNG, 2000, &r);
        refresh_page();
        return TRUE;
    }
    return FALSE;
}
