/* sg-control -- restore points (sg-session's sg-snapshot): what Settings >
 * Update & Security > Recovery and the Control Panel's Recovery show, and
 * the Control Panel's Recovery page, where an ext4 system drive can be
 * converted to btrfs ("Turn on system restore points").
 *
 * sg-snapshot publishes the facts in /run/stained-glass-snapshot/status
 * (SG_SNAPSHOT_STATUS for the gate): one line each, tab-separated fields,
 * ending with OK. The changes are an administrator's: the elevated copy
 * (/admin restore-point SUBCOMMAND [ID]) files them with sg-admind, which
 * runs sg-snapshot.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "control.h"
#include "restore.h"
#include <shellapi.h>

static void field(const char *line, int i, WCHAR *out, int cch)
{
    const char *p = line, *e;
    out[0] = 0;
    while (i-- > 0) { if (!(p = strchr(p, '\t'))) return; p++; }
    e = strchr(p, '\t');
    if (!e) e = p + strlen(p);
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, p, (int)(e - p), out, cch - 1);
        out[n > 0 ? n : 0] = 0;
    }
}

BOOL rp_read(struct rp_status *s)
{
    char path[400] = "/run/stained-glass-snapshot/status", *text, *line, *next;
    WCHAR env[400];
    DWORD len = 0;
    memset(s, 0, sizeof(*s));
    s->undone = -1;
    s->convert_days = -1;
    if (GetEnvironmentVariableW(L"SG_SNAPSHOT_STATUS", env, ARRAYSIZE(env)))
        WideCharToMultiByte(CP_UTF8, 0, env, -1, path, sizeof(path), NULL, NULL);
    if (!(text = read_unix_file(path, &len))) return FALSE;
    for (line = text; line && *line; line = next) {
        char *k = line, *v;
        if ((next = strchr(line, '\n'))) *next++ = 0;
        if (!(v = strchr(k, ' '))) { if (!strcmp(k, "OK")) s->ok = TRUE; continue; }
        *v++ = 0;
        if (!strcmp(k, "FS")) field(v, 0, s->fs, ARRAYSIZE(s->fs));
        else if (!strcmp(k, "LAYOUT")) s->layout = !strcmp(v, "yes");
        else if (!strcmp(k, "BOOTED")) { if (strcmp(v, "current")) field(v, 0, s->booted, ARRAYSIZE(s->booted)); }
        else if (!strcmp(k, "CONVERT")) { field(v, 0, s->convert, ARRAYSIZE(s->convert)); field(v, 1, s->convert_detail, ARRAYSIZE(s->convert_detail)); }
        else if (!strcmp(k, "PENDING")) { if (!strcmp(v, "rollback")) s->pending_rollback = TRUE; else if (!strcmp(v, "undo")) s->pending_undo = TRUE; }
        else if (!strcmp(k, "SAVED")) s->saved = !strcmp(v, "yes");
        else if (!strcmp(k, "READY")) s->ready = !strcmp(v, "yes");
        else if (!strcmp(k, "WENTBACK")) {
            WCHAR w[16];
            field(v, 1, s->wentback, ARRAYSIZE(s->wentback)); field(v, 2, w, ARRAYSIZE(w));
            s->wentback_store = !lstrcmpW(w, L"store");
        } else if (!strcmp(k, "CONVERT_DEADLINE")) {
            WCHAR d[16];
            field(v, 0, d, ARRAYSIZE(d)); field(v, 1, s->convert_until, ARRAYSIZE(s->convert_until));
#ifndef SG_MUTANT_RP_NO_DAYS
            s->convert_days = _wtoi(d);
#endif
        } else if (!strcmp(k, "KEPT")) {
            WCHAR by[16];
            s->kept = TRUE;
            field(v, 0, s->kept_when, ARRAYSIZE(s->kept_when)); field(v, 1, by, ARRAYSIZE(by));
            s->kept_auto = !lstrcmpW(by, L"auto");
        } else if (!strcmp(k, "PROBLEM") && s->nprob < RP_MAX_PROB) field(v, 0, s->prob[s->nprob++], ARRAYSIZE(s->prob[0]));
        else if (!strcmp(k, "UNDO")) {
            s->has_undo = TRUE;
            field(v, 0, s->undo_id, ARRAYSIZE(s->undo_id)); field(v, 1, s->undo_when, ARRAYSIZE(s->undo_when));
            field(v, 2, s->undo_label, ARRAYSIZE(s->undo_label));
        } else if (!strcmp(k, "UNDONE")) {
            WCHAR ok[8];
            field(v, 1, s->undone_when, ARRAYSIZE(s->undone_when)); field(v, 2, ok, ARRAYSIZE(ok));
            s->undone = !lstrcmpW(ok, L"yes");
        } else if (!strcmp(k, "SNAPSHOT") && s->n < RP_MAX_SNAP) {
            struct rp_snap *p = &s->snap[s->n++];
            WCHAR b[8], pool[16];
            field(v, 0, p->id, ARRAYSIZE(p->id)); field(v, 1, p->when, ARRAYSIZE(p->when));
            field(v, 2, p->kind, ARRAYSIZE(p->kind)); field(v, 3, b, ARRAYSIZE(b));
            field(v, 4, p->label, ARRAYSIZE(p->label)); field(v, 5, pool, ARRAYSIZE(pool));
            p->bootable = !lstrcmpW(b, L"yes");
            p->store = !lstrcmpW(pool, L"store");
        }
    }
    free(text);
    return s->ok;
}

/* how a restore point is named: taken before an update, before a change made
 * in the SG Store (apps are kept apart from the updates), or by hand */
void rp_before_text(BOOL store, const WCHAR *when, WCHAR *out, int cch)
{
#ifdef SG_MUTANT_RP_STORE_TITLE
    store = FALSE;
#endif
    _snwprintf(out, cch, store ? L"the SG Store change of %ls" : L"the update of %ls", when);
    out[cch - 1] = 0;
}

void rp_snap_title(const struct rp_snap *p, WCHAR *out, int cch)
{
    if (!lstrcmpW(p->kind, L"auto")) {
        WCHAR what[64];
        rp_before_text(p->store, p->when, what, ARRAYSIZE(what));
        _snwprintf(out, cch, L"Before %ls", what);
    } else
        _snwprintf(out, cch, L"%ls", p->when);
    out[cch - 1] = 0;
}

/* what is kept: the updates' and the SG Store's restore points apart, so
 * installing apps never pushes out the ones from before an update */
void rp_retention_text(WCHAR *out, int cch)
{
    lstrcpynW(out, L"One is made before every update and before every change made in the SG Store. The last three from before "
              L"updates are kept, and, apart from them, the last two from before SG Store installs.", cch);
}

/* undoing the conversion is offered for 14 days; the days left */
BOOL rp_convert_days_text(const struct rp_status *s, WCHAR *out, int cch)
{
    WCHAR days[48];
    if (s->convert_days < 0) return FALSE;
    if (s->convert_days == 1) lstrcpyW(days, L"1 more day");
    else _snwprintf(days, ARRAYSIZE(days), L"%d more days", s->convert_days);
    days[ARRAYSIZE(days) - 1] = 0;
    _snwprintf(out, cch, L"You can undo the conversion for %ls (until %ls). After that it is kept automatically: the old file "
               L"system is deleted and the space it takes is freed.", days, s->convert_until);
    out[cch - 1] = 0;
    return TRUE;
}

/* a conversion that was kept: by the person, or by itself after 14 days */
BOOL rp_kept_text(const struct rp_status *s, WCHAR *out, int cch)
{
    if (!s->kept || !s->ok) return FALSE;
    if (s->kept_auto)
        _snwprintf(out, cch, L"The system drive was converted to btrfs. The conversion was kept automatically%ls%ls, 14 days "
                   L"after it was made: the old file system was deleted and its space freed.", s->kept_when[0] ? L" on " : L"", s->kept_when);
    else
        _snwprintf(out, cch, L"The system drive was converted to btrfs. You kept the conversion%ls%ls: the old file system was deleted.",
                   s->kept_when[0] ? L" on " : L"", s->kept_when);
    out[cch - 1] = 0;
    return TRUE;
}

/* the conversion is offered: the root file system is ext4 (and nothing
 * else about it is shown otherwise -- David 2026-10-08) */
BOOL rp_convert_offered(const struct rp_status *s)
{
#ifdef SG_MUTANT_RP_CONVERT_ALWAYS
    return TRUE;
#endif
    return s->ok && !lstrcmpW(s->fs, L"ext4");
}

/* a conversion that is done but not yet kept: Keep or Undo */
BOOL rp_convert_unconfirmed(const struct rp_status *s)
{
    return s->ok && !lstrcmpW(s->fs, L"btrfs") && s->saved;
}

/* run the elevated copy's restore-point request; TRUE if it started */
BOOL rp_elevated(const WCHAR *sub, const WCHAR *id)
{
    WCHAR args[128];
    _snwprintf(args, ARRAYSIZE(args), L"/admin restore-point %ls%ls%ls", sub, id && id[0] ? L" " : L"", id ? id : L"");
    args[ARRAYSIZE(args) - 1] = 0;
    return run_elevated(args);
}

/* the elevated copy: /admin restore-point SUB [ID] */
int rp_admin(int argc, WCHAR **argv)
{
    const WCHAR *req[3] = { L"restore-point", argc > 0 ? argv[0] : L"", argc > 1 ? argv[1] : NULL };
    WCHAR msg[512];
    BOOL slow = argc > 0 && (!lstrcmpW(argv[0], L"convert-schedule") || !lstrcmpW(argv[0], L"convert-undo"));
    if (argc < 1) return 2;
    if (!admin_request(req, argc > 1 ? 3 : 2, msg, ARRAYSIZE(msg), slow ? 40 * 60 * 1000 : 10 * 60 * 1000)) {
        message(NULL, L"Recovery", msg, TRUE);
        return 1;
    }
    if (!lstrcmpW(argv[0], L"convert-schedule"))
        message(NULL, L"Recovery", L"The system drive will be converted the next time you restart your PC. Keep it connected "
                L"to power, and do not turn it off while it works.", FALSE);
    else if (!lstrcmpW(argv[0], L"convert-undo"))
        message(NULL, L"Recovery", L"The conversion will be undone the next time you restart your PC. Keep it connected "
                L"to power, and do not turn it off while it works.", FALSE);
    return 0;
}

void rp_restart(const WCHAR *what)
{
    WCHAR q[300];
    _snwprintf(q, ARRAYSIZE(q), L"Restart now %ls? Save your work first.", what);
    q[ARRAYSIZE(q) - 1] = 0;
    if (MessageBoxW(g_main, q, L"Recovery", MB_OKCANCEL | MB_ICONQUESTION) == IDOK)
        ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_FLAG_PLANNED);
}

/* ---- the Control Panel's Recovery page ------------------------------------------------- */
enum { CMD_RP_CONVERT = SHIELD_ID(CMD_PAGE_FIRST + 1), CMD_RP_CONVERT_CANCEL = SHIELD_ID(CMD_PAGE_FIRST + 2),
       CMD_RP_KEEP = SHIELD_ID(CMD_PAGE_FIRST + 3), CMD_RP_UNDO_CONVERT = SHIELD_ID(CMD_PAGE_FIRST + 4),
       CMD_RP_RESTART = CMD_PAGE_FIRST + 5, CMD_RP_SETTINGS = CMD_PAGE_FIRST + 6 };

void build_recovery_cpl(void)
{
    static const WCHAR *const labels[] = { L"Go back to a restore point", NULL, L"See also", L"Updates" };
    static const int ids[] = { CMD_RP_SETTINGS, 0, -1, NAV(PG_UPDATE) };
    struct rp_status s;
    int x = pg_left_pane(labels, ids, ARRAYSIZE(labels)) + S(36), y = S(24), w = pg_width() - x - S(40), i;
    WCHAR line[600];
    rp_read(&s);
    pg_title(x, y, L"Recovery");
    y += S(52);
    if (rp_convert_offered(&s)) {
        BOOL scheduled = !lstrcmpW(s.convert, L"scheduled");
        pg_text(x, y, w, S(24), g_font_cat, COL_TITLE, L"Turn on system restore points (convert the system drive)", DT_SINGLELINE);
        pg_rule(x, y + S(26), w);
        y += S(38);
        y += pg_para(x, y, w, g_font_body, COL_TEXT,
                     L"Stained Glass OS can keep restore points -- copies of the system from before each update, which you can "
                     L"go back to from Settings or the boot menu -- when its system drive uses the btrfs file system. This PC's "
                     L"system drive uses ext4.") + S(10);
        y += pg_para(x, y, w, g_font_body, COL_TEXT,
                     L"Converting the drive keeps your files, your programs and your settings, on the same partition. It "
                     L"happens when your PC restarts and takes from a few minutes to an hour, depending on how much is on the "
                     L"drive. The old file system is kept for 14 days, so the conversion can be undone; after that the conversion is kept "
                     L"automatically and the space the old file system takes is freed.") + S(10);
        y += pg_para(x, y, w, g_font_body, COL_WARN,
                     L"Back up your files first. If the conversion is interrupted -- by a power cut, or by turning the PC "
                     L"off -- the system drive may not start.") + S(14);
        if (scheduled) {
            pg_fill(x, y, w, S(96), COL_PANE);
            pg_icon(x + S(20), y + S(20), S(48), IC_UPDATE);
            pg_text(x + S(86), y + S(16), w - S(106), S(26), g_font_title, COL_TEXT, L"Restart to convert the system drive", DT_SINGLELINE);
            pg_button(L"Restart now", x + S(86), y + S(56), S(130), CMD_RP_RESTART);
            pg_link(x + S(232), y + S(60), L"Cancel the conversion", CMD_RP_CONVERT_CANCEL, LINK_SHIELD);
            y += S(116);
        } else {
            if (!lstrcmpW(s.convert, L"failed") && s.convert_detail[0]) {
                _snwprintf(line, ARRAYSIZE(line), L"The last conversion did not happen: %ls", s.convert_detail);
                y += pg_para(x, y, w, g_font_body, COL_WARN, line) + S(10);
            }
            if (s.ready) {
                pg_icon(x, y, S(20), IC_OK);
                pg_text(x + S(28), y, w - S(28), S(20), g_font_body, COL_TEXT, L"This PC is ready to be converted.", DT_SINGLELINE);
                y += S(30);
            }
            for (i = 0; i < s.nprob; i++) {
                pg_icon(x, y, S(20), IC_WARN);
                y += pg_para(x + S(28), y, w - S(28), g_font_body, COL_TEXT, s.prob[i]) + S(6);
            }
            pg_link(x, y + S(4), L"Convert the system drive...", CMD_RP_CONVERT, LINK_SHIELD);
            y += S(40);
        }
    } else if (rp_convert_unconfirmed(&s)) {
        BOOL undo_scheduled = !lstrcmpW(s.convert, L"undo-scheduled");
        /* the conversion's last step (the layout) failed: the drive works, as
         * plain btrfs, but keeps no restore points -- undo it and try again */
        BOOL half = !lstrcmpW(s.convert, L"failed-layout") || !s.layout;
        pg_text(x, y, w, S(24), g_font_cat, COL_TITLE, half ? L"The conversion did not finish" : L"The system drive was converted",
                DT_SINGLELINE);
        pg_rule(x, y + S(26), w);
        y += S(38);
        if (half) {
            _snwprintf(line, ARRAYSIZE(line), L"The system drive was converted to btrfs, but its last step failed%ls%ls. It works, "
                       L"but keeps no restore points. Undo the conversion to have the ext4 system drive back as it was, then "
                       L"try again.", s.convert_detail[0] ? L": " : L"", s.convert_detail);
            y += pg_para(x, y, w, g_font_body, COL_WARN, line) + S(12);
        } else
        y += pg_para(x, y, w, g_font_body, COL_TEXT,
                     L"The system drive uses btrfs now, and Stained Glass OS keeps restore points. The old file system is still "
                     L"kept, so the conversion can be undone. When everything works as it should, keep the conversion: that "
                     L"frees the space the old file system takes.") + S(10);
        if (rp_convert_days_text(&s, line, ARRAYSIZE(line))) y += pg_para(x, y, w, g_font_body, COL_TEXT, line) + S(12);
        else y += S(2);
        if (undo_scheduled) {
            pg_fill(x, y, w, S(96), COL_PANE);
            pg_icon(x + S(20), y + S(20), S(48), IC_UPDATE);
            pg_text(x + S(86), y + S(16), w - S(106), S(26), g_font_title, COL_TEXT, L"Restart to undo the conversion", DT_SINGLELINE);
            pg_button(L"Restart now", x + S(86), y + S(56), S(130), CMD_RP_RESTART);
            pg_link(x + S(232), y + S(60), L"Cancel", CMD_RP_CONVERT_CANCEL, LINK_SHIELD);
            y += S(116);
        } else {
            if (!half) {
                pg_link(x, y, L"Keep the conversion", CMD_RP_KEEP, LINK_SHIELD);
                y += S(30);
            }
            pg_link(x, y, L"Undo the conversion...", CMD_RP_UNDO_CONVERT, LINK_SHIELD);
            y += S(40);
        }
    } else if (rp_kept_text(&s, line, ARRAYSIZE(line))) {
        pg_text(x, y, w, S(24), g_font_cat, COL_TITLE, L"The system drive was converted", DT_SINGLELINE);
        pg_rule(x, y + S(26), w);
        y += S(38);
        y += pg_para(x, y, w, g_font_body, COL_TEXT, line) + S(14);
    }
    pg_text(x, y, w, S(24), g_font_cat, COL_TITLE, L"Go back to an earlier version", DT_SINGLELINE);
    pg_rule(x, y + S(26), w);
    y += S(38);
    if (s.ok && s.layout) {
        WCHAR keep[300];
        rp_retention_text(keep, ARRAYSIZE(keep));
        _snwprintf(line, ARRAYSIZE(line), s.n == 1 ? L"There is %d restore point. %ls" : L"There are %d restore points. %ls", s.n, keep);
        y += pg_para(x, y, w, g_font_body, COL_TEXT, line) + S(8);
    } else if (s.ok && !lstrcmpW(s.fs, L"ext4"))
        y += pg_para(x, y, w, g_font_body, COL_TEXT, s.has_undo
                     ? L"The last update of Stained Glass OS's own programs can be undone."
                     : L"Without restore points, the last update of Stained Glass OS's own programs can be undone; there is none to undo now.") + S(8);
    pg_link(x, y, L"Open Recovery in Settings", CMD_RP_SETTINGS, 0);
}

BOOL cmd_recovery_cpl(int id, int code, HWND ctl)
{
    struct rp_status s;
    (void)code; (void)ctl;
    switch (id) {
    case CMD_RP_SETTINGS: ShellExecuteW(g_main, NULL, L"ms-settings:recovery", NULL, NULL, SW_SHOWNORMAL); return TRUE;
    case CMD_RP_RESTART:
        rp_read(&s);
        rp_restart(!lstrcmpW(s.convert, L"undo-scheduled") ? L"to undo the conversion" : L"to convert the system drive");
        return TRUE;
    case CMD_RP_CONVERT:
        if (MessageBoxW(g_main, L"Convert the system drive to btrfs the next time you restart?\n\n"
                        L"Have you backed up your files? Keep the PC connected to power, and do not turn it off while it "
                        L"works: an interrupted conversion can leave the system drive unable to start.",
                        L"Turn on system restore points", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
            return TRUE;
        if (rp_elevated(L"convert-schedule", NULL)) refresh_when_back();
        return TRUE;
    case CMD_RP_CONVERT_CANCEL: if (rp_elevated(L"convert-cancel", NULL)) refresh_when_back(); return TRUE;
    case CMD_RP_KEEP:
        if (MessageBoxW(g_main, L"Keep the conversion? The old file system is deleted, and the conversion can no longer be undone.",
                        L"Recovery", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
            return TRUE;
        if (rp_elevated(L"convert-keep", NULL)) refresh_when_back();
        return TRUE;
    case CMD_RP_UNDO_CONVERT:
        if (MessageBoxW(g_main, L"Undo the conversion the next time you restart?\n\n"
                        L"The system drive goes back to exactly what it was before the conversion: everything changed since -- "
                        L"files in your folders, programs, settings -- is lost. Back up what you want to keep first.",
                        L"Recovery", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
            return TRUE;
        if (rp_elevated(L"convert-undo", NULL)) refresh_when_back();
        return TRUE;
    }
    return FALSE;
}

/* --dump recovery: what the pages decide, for the gate */
void dump_recovery(void)
{
    struct rp_status s;
    int i;
    WCHAR text[600];
    BOOL ok = rp_read(&s);
    wprintf(L"recovery.status=%ls\n", ok ? L"yes" : L"no");
    wprintf(L"recovery.fs=%ls\n", s.fs);
    wprintf(L"recovery.convert_offered=%ls\n", rp_convert_offered(&s) ? L"yes" : L"no");
    wprintf(L"recovery.convert_unconfirmed=%ls\n", rp_convert_unconfirmed(&s) ? L"yes" : L"no");
    wprintf(L"recovery.convert=%ls\n", s.convert);
    if (rp_convert_days_text(&s, text, ARRAYSIZE(text))) wprintf(L"recovery.convert_days=%d|%ls\nrecovery.convert_text=%ls\n", s.convert_days, s.convert_until, text);
    if (rp_kept_text(&s, text, ARRAYSIZE(text))) wprintf(L"recovery.kept=%ls|%ls\nrecovery.kept_text=%ls\n", s.kept_when, s.kept_auto ? L"auto" : L"you", text);
    rp_retention_text(text, ARRAYSIZE(text));
    wprintf(L"recovery.retention=%ls\n", text);
    wprintf(L"recovery.ready=%ls\n", s.ready ? L"yes" : L"no");
    for (i = 0; i < s.nprob; i++) wprintf(L"recovery.problem=%ls\n", s.prob[i]);
    wprintf(L"recovery.booted=%ls\n", s.booted[0] ? s.booted : L"current");
    wprintf(L"recovery.pending=%ls\n", s.pending_rollback ? L"rollback" : s.pending_undo ? L"undo" : L"no");
    for (i = 0; i < s.n; i++)
        wprintf(L"recovery.snapshot=%ls|%ls|%ls|%ls|%ls|%ls\n", s.snap[i].id, s.snap[i].when, s.snap[i].kind,
                s.snap[i].bootable ? L"yes" : L"no", s.snap[i].label, s.snap[i].store ? L"store" : L"update");
    if (s.has_undo) wprintf(L"recovery.undo=%ls|%ls|%ls\n", s.undo_id, s.undo_when, s.undo_label);
}
