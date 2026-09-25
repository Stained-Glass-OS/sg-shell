/* sg-control -- Date and Time: the clock, the time zone and Internet time.
 *
 * The Windows side follows the machine's Unix clock and zone (Wine maps the
 * zone to its Windows name); changing the zone is root's, through sg-admind.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"

enum { CMD_ZONE = SHIELD_ID(CMD_PAGE_FIRST + 1) };

static HWND g_time, g_date;

struct zone_facts { WCHAR key[128], display[256], iana[128], offset[32], dst[256]; BOOL ntp; };

static void zone_facts(struct zone_facts *z)
{
    DYNAMIC_TIME_ZONE_INFORMATION dtz;
    TIME_ZONE_INFORMATION tz;
    WCHAR sub[256];
    SYSTEMTIME now;
    LONG bias;
    DWORD r;
    memset(z, 0, sizeof(*z));
    r = GetDynamicTimeZoneInformation(&dtz);
    lstrcpynW(z->key, dtz.TimeZoneKeyName[0] ? dtz.TimeZoneKeyName : dtz.StandardName, ARRAYSIZE(z->key));
    _snwprintf(sub, ARRAYSIZE(sub), L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\%ls", z->key);
    reg_sz(HKEY_LOCAL_MACHINE, sub, L"Display", z->display, ARRAYSIZE(z->display));
    if (!z->display[0]) lstrcpynW(z->display, z->key, ARRAYSIZE(z->display));
    current_zone(z->iana, ARRAYSIZE(z->iana));
    bias = dtz.Bias + (r == TIME_ZONE_ID_DAYLIGHT ? dtz.DaylightBias : r == TIME_ZONE_ID_STANDARD ? dtz.StandardBias : 0);
    _snwprintf(z->offset, ARRAYSIZE(z->offset), L"UTC%lc%02ld:%02ld", bias <= 0 ? L'+' : L'-', labs(bias) / 60, labs(bias) % 60);
    z->ntp = ntp_enabled();
    GetLocalTime(&now);
    if (GetTimeZoneInformationForYear(now.wYear, &dtz, &tz) && tz.DaylightDate.wMonth) {
        /* the next change this year: a rule (wYear 0) names a weekday of a week */
        SYSTEMTIME *next = r == TIME_ZONE_ID_DAYLIGHT ? &tz.StandardDate : &tz.DaylightDate;
        SYSTEMTIME when = *next;
        WCHAR date[64] = L"", time[32] = L"";
        if (!when.wYear) {
            SYSTEMTIME first = { now.wYear, when.wMonth, 0, 1, 0, 0, 0, 0 };
            FILETIME ft;
            int dow, day;
            SystemTimeToFileTime(&first, &ft); FileTimeToSystemTime(&ft, &first);
            dow = first.wDayOfWeek;
            day = 1 + (when.wDayOfWeek - dow + 7) % 7 + (when.wDay - 1) * 7;
            {
                static const int dim[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
                int max = dim[when.wMonth - 1] + (when.wMonth == 2 && (now.wYear % 4 == 0 && (now.wYear % 100 || now.wYear % 400 == 0)));
                while (day > max) day -= 7;
            }
            when.wYear = now.wYear; when.wDay = (WORD)day;
        }
        GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &when, NULL, date, ARRAYSIZE(date));
        GetTimeFormatW(LOCALE_USER_DEFAULT, TIME_NOSECONDS, &when, NULL, time, ARRAYSIZE(time));
        _snwprintf(z->dst, ARRAYSIZE(z->dst), L"Daylight saving time %ls on %ls at %ls.",
                   r == TIME_ZONE_ID_DAYLIGHT ? L"ends" : L"begins", date, time);
    } else lstrcpyW(z->dst, L"This time zone does not observe daylight saving time.");
    z->dst[ARRAYSIZE(z->dst) - 1] = 0;
}

static void tick(void)
{
    SYSTEMTIME now;
    WCHAR t[64], d[128];
    GetLocalTime(&now);
    GetTimeFormatW(LOCALE_USER_DEFAULT, 0, &now, NULL, t, ARRAYSIZE(t));
    GetDateFormatW(LOCALE_USER_DEFAULT, DATE_LONGDATE, &now, NULL, d, ARRAYSIZE(d));
    if (g_time) SetWindowTextW(g_time, t);
    if (g_date) SetWindowTextW(g_date, d);
}

void build_datetime(void)
{
    static const WCHAR *const labels[] = { L"Change the time zone", NULL, L"See also", L"Clock and Region" };
    static const int ids[] = { CMD_ZONE, 0, -1, NAV(PG_CAT_CLOCK) };
    struct zone_facts z;
    int x = pg_left_pane(labels, ids, ARRAYSIZE(labels)) + S(36), y = S(24), w = pg_width() - x - S(40);
    WCHAR line[400];

    zone_facts(&z);
    pg_title(x, y, L"Date and Time");
    y += S(52);
    pg_icon(x, y, S(72), IC_CLOCK);
    g_time = pg_control(L"STATIC", L"", SS_NOPREFIX, x + S(92), y - S(4), S(360), S(48), -1);
    SendMessageW(g_time, WM_SETFONT, (WPARAM)g_font_big, TRUE);
    g_date = pg_control(L"STATIC", L"", SS_NOPREFIX, x + S(92), y + S(46), S(360), S(22), -1);
    tick();
    pg_timer(1000);
    y += S(100);

    pg_text(x, y, w, S(24), g_font_cat, COL_TITLE, L"Time zone", DT_SINGLELINE);
    pg_rule(x, y + S(26), w);
    y += S(38);
    _snwprintf(line, ARRAYSIZE(line), L"%ls", z.display);
    pg_text(x + S(16), y, w - S(16), S(20), g_font_body, COL_TEXT, line, DT_SINGLELINE | DT_END_ELLIPSIS);
    y += S(24);
    _snwprintf(line, ARRAYSIZE(line), L"%ls%ls%ls   \x2022   currently %ls", z.iana[0] ? L"Zone " : L"", z.iana,
               z.iana[0] ? L"" : L"", z.offset);
    pg_text(x + S(16), y, w - S(16), S(20), g_font_body, COL_SUBTLE, line, DT_SINGLELINE | DT_END_ELLIPSIS);
    y += S(24);
    pg_text(x + S(16), y, w - S(16), S(20), g_font_body, COL_SUBTLE, z.dst, DT_SINGLELINE | DT_END_ELLIPSIS);
    y += S(32);
    pg_link(x + S(12), y, L"Change time zone...", CMD_ZONE, LINK_SHIELD);
    y += S(48);

    pg_text(x, y, w, S(24), g_font_cat, COL_TITLE, L"Internet time", DT_SINGLELINE);
    pg_rule(x, y + S(26), w);
    y += S(38);
    pg_icon(x + S(16), y, S(20), z.ntp ? IC_OK : IC_WARN);
    pg_text(x + S(44), y + S(1), w - S(44), S(20), g_font_body, COL_TEXT,
            z.ntp ? L"This computer is set to synchronize its clock with an Internet time server automatically."
                  : L"This computer is not set to synchronize its clock automatically.", DT_SINGLELINE | DT_END_ELLIPSIS);
    y += S(32);
    pg_link(x + S(12), y, L"Change settings...", CMD_ZONE, LINK_SHIELD);
}

BOOL cmd_datetime(int id, int code, HWND ctl)
{
    (void)code; (void)ctl;
    if (id == CMD_ZONE) { if (run_elevated(L"/admin timezone")) refresh_when_back(); return TRUE; }
    return FALSE;
}

void timer_datetime(void) { tick(); }

void dump_datetime(void)
{
    struct zone_facts z;
    zone_facts(&z);
    wprintf(L"timezone.windows=%ls\n", z.key);
    wprintf(L"timezone.display=%ls\n", z.display);
    wprintf(L"timezone.iana=%ls\n", z.iana);
    wprintf(L"timezone.offset=%ls\n", z.offset);
    wprintf(L"ntp=%ls\n", z.ntp ? L"on" : L"off");
}
