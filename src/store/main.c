/* SG Store -- the window and the command line.
 *
 *   sg-store64.exe                 open the store
 *   sg-store64.exe --list          write the catalogue and each app's state to
 *                                  the dump (headless), then exit
 *   sg-store64.exe --check-updates as --list, but resolve each app's newest
 *                                  available version first
 *   sg-store64.exe --install ID    install (or update) one app by ordinal,
 *                                  name or winget id (headless), then exit
 *   sg-store64.exe --open ID       Open, as the card's button: the program
 *                                  (headless), then exit
 *   sg-store64.exe --uninstall ID  remove one app (headless), then exit
 *   sg-store64.exe --deb FILE      "Install a Linux package": what a .deb is,
 *                                  and Install (File Explorer's .deb verb)
 *   --elevated-apt / --elevated-apt-remove / --elevated-deb
 *   --install-batch ORD...   install them as Install selected does: one consent
 *   --elevated-helper FILE   that consent's helper (sysinstall.c)
 *                                  the elevated half (sysinstall.c)
 *
 * SG_STORE_DUMP=<file> (a Windows path) receives the catalogue, each app's
 * tier/state/versions, what the window lists in which order and, while the
 * window is open, every clickable thing's screen centre -- for the gate.
 *
 * The window: a search box (it looks in names, makers, descriptions and
 * categories), the categories, then the apps by category -- Windows programs
 * and our own first; native Linux apps in a "Linux apps" section of their
 * own, after all the others. Keyboard: typing searches, Down/Up move through
 * the list, Enter installs or opens, Esc goes back to the search box.
 * OS updates are not handled here.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "store.h"
#include <shellapi.h>
#include <commdlg.h>

app_t g_apps[MAX_APPS];      /* details.c reads them */
int g_napps;
static WCHAR g_dump[MAX_PATH];
HWND g_wnd;
static HWND g_search;
static int g_detail = -1;                 /* the app whose details page is open, or -1: the list */
static int g_list_scroll;                 /* the list's scroll while a details page is open */
static WNDPROC g_search_proc;
static HFONT g_f_title, g_f_head, g_f_body, g_f_small;
static int g_dpi = 96, g_scroll, g_extent;
static volatile LONG g_cancel;
static int g_busy = -1;                   /* the app being installed (or removed), or -1 */
/* What waits its turn: one install (or removal) at a time, the rest queued
 * in the order they were asked for (David 2026-10-01). */
static int g_queue[MAX_APPS], g_qn;
static BOOL g_qremove[MAX_APPS];          /* by app: its turn removes it */
static CRITICAL_SECTION g_qlock;
static volatile LONG g_batch;             /* the queue holds checked apps: one consent (sysinstall.c) */
static WCHAR g_query[128];                /* the search box */
static int g_cat;                         /* the chosen category: 0 = all */
static int g_sel = -1;                    /* the selected app (keyboard), or -1 */
static int g_view[MAX_APPS], g_nview;     /* what the list shows, in order */
static int g_view_y[MAX_APPS];
static int g_cols = 1;                     /* the cards' columns, as last drawn */
#define WM_ICONS (WM_APP + 1)

#define dpx(x) MulDiv((x), g_dpi, 96)

/* The categories, in the order they are listed; anything else follows them,
 * and the Linux apps come last. */
static const WCHAR *const g_cats[] = {
    L"All", L"Browsers", L"Productivity", L"Graphics", L"Media", L"Development",
    L"Utilities", L"Internet", L"Games", LINUX_SECTION,
};
#define NCATS ((int)ARRAYSIZE(g_cats))

/* hit rectangles, for the mouse and the gate */
enum { H_INSTALL, H_OPEN, H_UPDATE, H_CHECKALL, H_CAT, H_FROMFILE, H_CARD, H_BUILD, H_UNINSTALL, H_UNQUEUE, H_ICON,
       H_CHECK, H_INSTALLSEL, H_BACK, H_HOMEPAGE };
typedef struct { int verb, idx; RECT rc; } hit_t;
static hit_t g_hits[MAX_APPS * 3 + 32];
static int g_nhits;

static const WCHAR *state_name(int s)
{
    switch (s) {
    case AST_NOT_INSTALLED: return L"not-installed";
    case AST_INSTALLED:     return L"installed";
    case AST_UPDATE:        return L"update";
    case AST_INSTALLING:    return L"installing";
    case AST_DONE:          return L"done";
    case AST_REMOVING:      return L"removing";
    default:                return L"failed";
    }
}
static const WCHAR *method_name(int m)
{
    switch (m) {
    case SRC_WINGET:     return L"winget";
    case SRC_PIN:        return L"pin";
    case SRC_OURS_APT:   return L"ours-apt";
    case SRC_LINUX_APT:  return L"linux-apt";
    default:             return L"unknown";
    }
}

/* ---- one app, two builds ----------------------------------------------------------------------------
 *
 * A Windows program whose catalogue entry names its Linux build (Linux=Lnn)
 * is one card with a choice of build: the Linux one unless the entry says
 * Prefer=windows (David 2026-10-01: OBS for Linux before OBS for Windows; a
 * browser whose single sign-on wants Windows stays a Windows program). The
 * Linux entry is then no card of its own, so no app is listed twice. */

static void link_pairs(void)
{
    int i, j;
#ifdef SG_MUTANT_NOPAIR
    return;     /* the mutant lists both builds as apps of their own */
#endif
    for (i = 0; i < g_napps; i++) {
        app_t *a = &g_apps[i];
        if (!a->linux_ord[0] || a->tier == TIER_LINUX) continue;
        for (j = 0; j < g_napps; j++)
            if (j != i && g_apps[j].tier == TIER_LINUX && !g_apps[j].is_alt && !lstrcmpiW(g_apps[j].ord, a->linux_ord)) break;
        if (j == g_napps) continue;
        a->alt = j;
        g_apps[j].alt = i;
        g_apps[j].is_alt = TRUE;
        a->use_alt = !a->prefer_windows;
    }
}

static BOOL has_it(const app_t *a)
{
    return a->state == AST_INSTALLED || a->state == AST_UPDATE || a->state == AST_DONE;
}

/* Uninstall is offered for an installed app: a system package (SG Office,
 * a Linux app) through sg-admind's apt-remove, a Windows program through
 * its own uninstaller (catalog.c app_uninstall) */
static BOOL can_uninstall(const app_t *a)
{
#ifdef SG_MUTANT_NOUNINSTALL
    return FALSE;
#endif
    if ((a->method == SRC_OURS_APT || a->method == SRC_LINUX_APT) && !a->apt_pkg[0]) return FALSE;
    return has_it(a);
}

static BOOL in_hand(int i)      /* being installed or removed, or waiting to be */
{
    return i == g_busy || g_apps[i].queued || g_apps[i].state == AST_INSTALLING || g_apps[i].state == AST_REMOVING;
}

/* the entry a card acts on: the build being worked on, else the one that is
 * installed, else the one chosen */
static int card_target(int i)
{
    const app_t *a = &g_apps[i];
    int l = a->alt;
    if (l < 0 || a->is_alt) return i;
    if (in_hand(i)) return i;
    if (in_hand(l)) return l;
    if (has_it(a) != has_it(&g_apps[l])) return has_it(a) ? i : l;
    return a->use_alt ? l : i;
}

/* ---- what the list shows ------------------------------------------------------------------------ */

static int section_rank(const app_t *a)
{
    const WCHAR *s = app_section(a);
    int i;
    for (i = 1; i < NCATS; i++) if (!lstrcmpiW(s, g_cats[i])) return !lstrcmpiW(s, LINUX_SECTION) ? 1000 : i;
    return 500;     /* a category of a mirror's own, before the Linux apps */
}

static BOOL matches_query(const app_t *a, const WCHAR *q)
{
    WCHAR word[128];
    const WCHAR *p = q;
#ifdef SG_MUTANT_NOSEARCH
    return TRUE;    /* the mutant's search box filters nothing */
#endif
    /* every word of the query somewhere in the name, maker, description or category */
    while (*p) {
        int n = 0;
        while (*p == L' ') p++;
        while (*p && *p != L' ' && n < 127) word[n++] = *p++;
        word[n] = 0;
        if (!n) break;
        if (!StrStrIW(a->name, word) && !StrStrIW(a->publisher, word) && !StrStrIW(a->desc, word) &&
            !StrStrIW(a->category, word) && !StrStrIW(app_section(a), word) &&
            !((a->tier == TIER_LINUX || a->alt >= 0) && !lstrcmpiW(word, L"linux")))
            return FALSE;
    }
    return TRUE;
}

static int cmp_view(const void *x, const void *y)
{
    const app_t *a = &g_apps[*(const int *)x], *b = &g_apps[*(const int *)y];
    int ra = section_rank(a), rb = section_rank(b), c;
    if (ra != rb) return ra - rb;
    if ((c = lstrcmpiW(app_section(a), app_section(b)))) return c;
    /* our own suites lead their category; then by name */
    if ((a->tier == TIER_OURS) != (b->tier == TIER_OURS)) return a->tier == TIER_OURS ? -1 : 1;
    return lstrcmpiW(a->name, b->name);
}

/* what the list shows, in order, into view/n: a pure function of the
 * catalogue, the search and the category, so the install thread's dump()
 * never disturbs the list the window is painting */
static void list_view(int *view, int *n)
{
    int i;
    *n = 0;
    for (i = 0; i < g_napps; i++) {
        const app_t *a = &g_apps[i];
        BOOL linux_tab = g_cat > 0 && !lstrcmpiW(g_cats[g_cat], LINUX_SECTION);
        if (a->is_alt) continue;      /* its Windows build's card offers it */
        if (linux_tab && a->alt >= 0) { if (!g_query[0] || matches_query(a, g_query)) view[(*n)++] = i; continue; }
        if (g_cat > 0 && lstrcmpiW(app_section(a), g_cats[g_cat]) && lstrcmpiW(a->category, g_cats[g_cat])) continue;
        if (g_cat > 0 && a->tier == TIER_LINUX && lstrcmpiW(g_cats[g_cat], LINUX_SECTION)) continue;
        if (g_query[0] && !matches_query(a, g_query)) continue;
        view[(*n)++] = i;
    }
    qsort(view, *n, sizeof(view[0]), cmp_view);
}

/* the window's list (the window's thread only); the selection stays only
 * while its app is listed */
static void build_view(void)
{
    int i, keep = g_sel;
    list_view(g_view, &g_nview);
    g_sel = -1;
    for (i = 0; i < g_nview; i++) if (g_view[i] == keep) g_sel = keep;
}

/* ---- the dump ------------------------------------------------------------------------------------- */

static void dumpf(FILE *f, const WCHAR *fmt, ...)
{
    WCHAR buf[2048];
    char u[4096];
    va_list ap;
    va_start(ap, fmt);
    vswprintf(buf, ARRAYSIZE(buf), fmt, ap);
    va_end(ap);
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, u, sizeof(u), NULL, NULL);
    fputs(u, f);
}

static void dump(void)
{
    WCHAR tmp[MAX_PATH + 8];
    FILE *f;
    int i;
    if (!g_dump[0]) return;
    swprintf(tmp, ARRAYSIZE(tmp), L"%ls.tmp", g_dump);
    if (!(f = _wfopen(tmp, L"wb"))) return;
    dumpf(f, L"window %d\n", g_wnd ? 1 : 0);
    dumpf(f, L"columns %d\n", g_cols);
    dumpf(f, L"search %ls\n", g_query);
    dumpf(f, L"category %ls\n", g_cats[g_cat]);
    dumpf(f, L"catalog %d\n", g_napps);
    for (i = 0; i < g_napps; i++) {
        app_t *a = &g_apps[i];
        dumpf(f, L"app %ls tier=%ls method=%ls state=%ls installed=%ls available=%ls category=%ls section=%ls name=%ls\n",
              a->ord, tier_name(a->tier), method_name(a->method), state_name(a->state),
              a->installed_version[0] ? a->installed_version : L"-",
              a->available_version[0] ? a->available_version : L"-",
              a->category, app_section(a), a->name);
        if (a->msg[0]) dumpf(f, L"msg %ls %ls\n", a->ord, a->msg);
        if (a->alt >= 0 && !a->is_alt)
            dumpf(f, L"pair %ls %ls choice=%ls target=%ls\n", a->ord, g_apps[a->alt].ord,
                  a->use_alt ? L"linux" : L"windows", g_apps[card_target(i)].ord);
        if (a->queued) dumpf(f, L"queued %ls %ls\n", a->ord, g_qremove[i] ? L"remove" : L"install");
        if (a->checked) dumpf(f, L"checked %ls\n", a->ord);
    }
    dumpf(f, L"icons %d\n", icons_ready());
    dumpf(f, L"batch %d\n", g_batch);
    /* the list's order: ordinals, first to last */
    {
        int view[MAX_APPS];
        int n;
        list_view(view, &n);
        dumpf(f, L"view %d", n);
        for (i = 0; i < n; i++) dumpf(f, L" %ls", g_apps[view[i]].ord);
    }
    dumpf(f, L"\n");
    dumpf(f, L"selected %ls\n", g_sel >= 0 ? g_apps[g_sel].ord : L"-");
    if (g_detail >= 0) {
        int t = card_target(g_detail);
        details_t *d = details_get(t);
        WCHAR src[1024], first[400];
        int k;
        details_source(&g_apps[t], d, src, ARRAYSIZE(src));
        dumpf(f, L"detail %ls ready=%d\n", g_apps[t].ord, d && d->state == 2);
        dumpf(f, L"detail-source %ls\n", src);
        if (d && d->state == 2) {
            lstrcpynW(first, d->desc, ARRAYSIZE(first));
            for (k = 0; first[k]; k++) if (first[k] == '\n') first[k] = '|';
            dumpf(f, L"detail-desc %ls\n", first);
            dumpf(f, L"detail-desc-length %d\n", lstrlenW(d->desc));
            dumpf(f, L"detail-version %ls\n", d->version[0] ? d->version : L"-");
            dumpf(f, L"detail-homepage %ls\n", d->homepage[0] ? d->homepage : L"-");
            dumpf(f, L"detail-license %ls\n", d->license[0] ? d->license : L"-");
            dumpf(f, L"detail-size %ls\n", d->size[0] ? d->size : L"-");
            if (d->error[0]) dumpf(f, L"detail-error %ls\n", d->error);
        }
    } else dumpf(f, L"detail -\n");
    dumpf(f, L"focus %ls\n", g_wnd && GetFocus() == g_search ? L"search" : L"list");
    for (i = 0; i < g_nhits; i++) {
        POINT pt = { (g_hits[i].rc.left + g_hits[i].rc.right) / 2, (g_hits[i].rc.top + g_hits[i].rc.bottom) / 2 };
        if (g_hits[i].verb == H_CARD) {     /* a point of the card that is no button: its name's row */
            pt.x = g_hits[i].rc.left + (g_hits[i].rc.right - g_hits[i].rc.left) / 4;
            pt.y = g_hits[i].rc.top + dpx(22);
        }
        static const WCHAR *const verbs[] = { L"install", L"open", L"update", L"checkall", L"category", L"fromfile", L"card",
                                              L"build", L"uninstall", L"unqueue", L"icon", L"check", L"installsel",
                                              L"back", L"homepage" };
        if (g_wnd) ClientToScreen(g_wnd, &pt);
        dumpf(f, L"hit %ls %ls %d %d\n", verbs[g_hits[i].verb],
              g_hits[i].verb == H_CAT ? g_cats[g_hits[i].idx] : g_hits[i].idx >= 0 ? g_apps[g_hits[i].idx].ord : L"-",
              pt.x, pt.y);
    }
    if (g_search) {
        RECT r; POINT pt;
        GetWindowRect(g_search, &r);
        pt.x = (r.left + r.right) / 2; pt.y = (r.top + r.bottom) / 2;
        dumpf(f, L"hit search - %d %d\n", pt.x, pt.y);
    }
    fclose(f);
    MoveFileExW(tmp, g_dump, MOVEFILE_REPLACE_EXISTING);
}

/* ---- opening an installed app ---------------------------------------------------------------------- */

static void open_app(app_t *a)
{
    WCHAR target[MAX_PATH], dir[MAX_PATH], *slash;
    if (a->tier == TIER_LINUX && a->run[0]) { sys_run_linux(a->run); return; }
#ifndef SG_MUTANT_NOLAUNCH
    /* our own: its program, by its App Paths name (SG Office: sg-documents.exe) */
    if (a->tier == TIER_OURS && a->run[0]) { ShellExecuteW(NULL, NULL, a->run, NULL, NULL, SW_SHOWNORMAL); return; }
    /* the program itself (or its Start shortcut), started in its own folder */
    if (app_launch_target(a, target, MAX_PATH)) {
        lstrcpynW(dir, target, MAX_PATH);
        if ((slash = wcsrchr(dir, '\\'))) *slash = 0;
        ShellExecuteW(NULL, NULL, target, NULL, dir, SW_SHOWNORMAL);
        return;
    }
#else
    (void)target; (void)dir; (void)slash;
#endif
    /* neither found: Apps & features, where it can be changed or removed */
    ShellExecuteW(NULL, NULL, L"ms-settings:appsfeatures", NULL, NULL, SW_SHOWNORMAL);
}

/* "Install from a file": an installer someone downloaded (.exe, .msi) or a
 * Linux package (.deb) -- opened as File Explorer opens it (the .deb verb is
 * this program's --deb). */
static void install_from_file(void)
{
    WCHAR file[MAX_PATH] = L"";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"Installers and packages (*.exe;*.msi;*.deb)\0*.exe;*.msi;*.deb\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Install from a file";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) ShellExecuteW(g_wnd, NULL, file, NULL, NULL, SW_SHOWNORMAL);
}

/* ---- the whole catalogue --------------------------------------------------------------------------- */

static void load_all(BOOL check_updates)
{
    int i;
    WCHAR err[512];
    g_napps = catalog_load(g_apps, MAX_APPS);
    for (i = 0; i < g_napps; i++) {
        app_detect(&g_apps[i]);
        if (check_updates) app_check_update(&g_apps[i], err, ARRAYSIZE(err));
    }
    link_pairs();
}

/* what is installed now (the window, coming back to it: something may have
 * been removed in Apps & features meanwhile) -- not while the queue works */
static void redetect(void)
{
    int i;
    if (g_busy >= 0) return;
    for (i = 0; i < g_napps; i++)
        if (g_apps[i].state != AST_FAILED && !g_apps[i].queued) app_detect(&g_apps[i]);
}

/* ---- installing on a worker thread ----------------------------------------------------------------- */

static void progress_cb(void *ctx, int stage, ULONGLONG done, ULONGLONG total)
{
    (void)ctx; (void)stage; (void)done; (void)total;
}

/* one turn: install (or update) a, or remove it */
static void run_one(int i, BOOL remove)
{
    app_t *a = &g_apps[i];
    WCHAR err[512];
    int rc;
    a->state = remove ? AST_REMOVING : AST_INSTALLING;
    a->msg[0] = 0;
    a->msg_error = FALSE;
    if (g_wnd) { InvalidateRect(g_wnd, NULL, FALSE); dump(); }
    rc = remove ? app_uninstall(a, err, ARRAYSIZE(err)) : app_install(a, progress_cb, NULL, &g_cancel, err, ARRAYSIZE(err));
    if (remove) {
        app_detect(a);
        if (has_it(a) && !rc) rc = 1, lstrcpynW(err, L"It is still installed.", ARRAYSIZE(err));
        if (rc) { lstrcpynW(a->msg, err[0] ? err : L"It was not removed.", ARRAYSIZE(a->msg)); a->msg_error = TRUE; }
        else lstrcpynW(a->msg, L"Uninstalled.", ARRAYSIZE(a->msg));
    } else if (rc == 0) {
        a->state = AST_DONE;
        app_detect(a);              /* pick up the new installed version */
        if (a->state == AST_NOT_INSTALLED) a->state = AST_DONE;
        lstrcpynW(a->msg, L"Installed.", ARRAYSIZE(a->msg));
    } else {
        a->state = AST_FAILED;
        lstrcpynW(a->msg, err[0] ? err : L"The install failed.", ARRAYSIZE(a->msg));
    }
}

static DWORD WINAPI queue_worker(void *arg)
{
    int i = (int)(INT_PTR)arg;
    for (;;) {
        run_one(i, g_qremove[i]);
        g_qremove[i] = FALSE;
        EnterCriticalSection(&g_qlock);
        if (g_qn) {
            i = g_queue[0];
            memmove(g_queue, g_queue + 1, --g_qn * sizeof(g_queue[0]));
            g_apps[i].queued = FALSE;
            g_busy = i;
        } else g_busy = -1;
        LeaveCriticalSection(&g_qlock);
        if (g_busy < 0 && InterlockedExchange(&g_batch, 0)) sys_batch_end();   /* the batch is done: its helper goes */
        if (g_wnd) { InvalidateRect(g_wnd, NULL, FALSE); dump(); }
        if (g_busy < 0) return 0;
    }
}

/* install (or remove) i now, or when its turn comes */
static void enqueue(int i, BOOL remove)
{
    BOOL now = FALSE;
    if (i < 0 || i >= g_napps || in_hand(i)) return;
    EnterCriticalSection(&g_qlock);
    if (g_busy < 0) { g_busy = i; now = TRUE; }
#ifdef SG_MUTANT_NOQUEUE
    else { LeaveCriticalSection(&g_qlock); return; }    /* the mutant ignores a second click (the old store) */
#else
    else { g_queue[g_qn++] = i; g_apps[i].queued = TRUE; g_apps[i].msg[0] = 0; }
#endif
    g_qremove[i] = remove;
    LeaveCriticalSection(&g_qlock);
    if (now) CloseHandle(CreateThread(NULL, 0, queue_worker, (void *)(INT_PTR)i, 0, NULL));
    if (g_wnd) { InvalidateRect(g_wnd, NULL, FALSE); dump(); }
}

static void start_install(int i) { enqueue(i, FALSE); }

/* ---- several at once (David 2026-10-03: install many apps via a checkbox,
 * the administrator asked once for all of them) ---- */

static BOOL checkable(int idx)
{
    int t = card_target(idx);
    return !has_it(&g_apps[t]) && !in_hand(t);
}

static int count_checked(void)
{
    int i, n = 0;
    for (i = 0; i < g_napps; i++) if (g_apps[i].checked) n++;
    return n;
}

static void toggle_check(int idx)
{
    if (idx < 0 || idx >= g_napps || !checkable(idx)) return;
    g_apps[idx].checked = !g_apps[idx].checked;
    if (g_wnd) { InvalidateRect(g_wnd, NULL, FALSE); dump(); }
}

static void install_selected(void)
{
    int i, n = count_checked();
    if (!n) return;
    /* two or more: what needs an administrator goes to one elevated helper */
    if (n > 1 && !InterlockedExchange(&g_batch, 1)) sys_batch_begin();
    for (i = 0; i < g_napps; i++)
        if (g_apps[i].checked) {
            g_apps[i].checked = FALSE;
            if (checkable(i)) enqueue(card_target(i), FALSE);
        }
    if (g_wnd) { InvalidateRect(g_wnd, NULL, FALSE); dump(); }
}

static void unqueue(int i)
{
    int k;
    EnterCriticalSection(&g_qlock);
    for (k = 0; k < g_qn; k++)
        if (g_queue[k] == i) { memmove(g_queue + k, g_queue + k + 1, (g_qn - k - 1) * sizeof(g_queue[0])); g_qn--; break; }
    g_apps[i].queued = FALSE;
    g_qremove[i] = FALSE;
    LeaveCriticalSection(&g_qlock);
    if (g_wnd) { InvalidateRect(g_wnd, NULL, FALSE); dump(); }
}

static void ask_uninstall(HWND hwnd, int i)
{
    WCHAR q[400], yes[4] = L"";
    const app_t *a = &g_apps[i];
    _snwprintf(q, ARRAYSIZE(q), L"Uninstall %ls?\n\nIts files are removed from this computer. "
               L"Your documents and settings are kept.", a->name);
    q[ARRAYSIZE(q) - 1] = 0;
    /* SG_STORE_YES=1: the gate's answer */
    GetEnvironmentVariableW(L"SG_STORE_YES", yes, ARRAYSIZE(yes));
    if (yes[0] == L'1' || MessageBoxW(hwnd, q, L"SG Store", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
        enqueue(i, TRUE);
}

/* the card's build: Linux or Windows */
static void choose_build(HWND hwnd, int i, POINT pt)
{
    app_t *a = &g_apps[i];
    HMENU m;
    int cmd;
    if (a->alt < 0) return;
    m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (a->use_alt ? MF_CHECKED : 0), 1,
                a->prefer_windows ? L"Linux app" : L"Linux app (recommended)");
    AppendMenuW(m, MF_STRING | (!a->use_alt ? MF_CHECKED : 0), 2,
                a->prefer_windows ? L"Windows program (recommended)" : L"Windows program");
    ClientToScreen(hwnd, &pt);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(m);
    if (cmd) { a->use_alt = cmd == 1; InvalidateRect(hwnd, NULL, FALSE); }
}

/* the card's main action: Install / Update / Open */
static void activate(int i)
{
    app_t *a;
    if (i < 0 || i >= g_napps) return;
    i = card_target(i);
    a = &g_apps[i];
    if (in_hand(i)) return;
    if (a->state == AST_INSTALLED || a->state == AST_DONE) open_app(a);
    else start_install(i);
}

/* ---- drawing --------------------------------------------------------------------------------------- */

#define C_BG      RGB(0xf3, 0xf3, 0xf3)
#define C_CARD    RGB(0xff, 0xff, 0xff)
#define C_LINE    RGB(0xe1, 0xe1, 0xe1)
#define C_TEXT    RGB(0x20, 0x20, 0x20)
#define C_SUB     RGB(0x60, 0x60, 0x60)
#define C_ACCENT  RGB(0x10, 0x7c, 0x41)
#define C_OK      RGB(0x10, 0x7c, 0x41)
#define C_ERR     RGB(0xc4, 0x2b, 0x1c)
#define C_SEL     RGB(0x00, 0x5a, 0x9e)

static int header_height(void) { return dpx(160); }

static void add_hit(int verb, int idx, RECT rc)
{
    if (g_nhits < (int)ARRAYSIZE(g_hits)) { g_hits[g_nhits].verb = verb; g_hits[g_nhits].idx = idx; g_hits[g_nhits].rc = rc; g_nhits++; }
}

/* a hit in the scrolled list, only where it is visible below the header */
static void add_list_hit(int verb, int idx, RECT rc, int client_bottom)
{
    if (rc.top < header_height() || rc.bottom > client_bottom) return;
    add_hit(verb, idx, rc);
}

static void text(HDC dc, HFONT font, COLORREF col, RECT rc, const WCHAR *s, UINT fmt)
{
    HFONT old = SelectObject(dc, font);
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &rc, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void button(HDC dc, RECT rc, const WCHAR *label, BOOL primary)
{
    HBRUSH b = CreateSolidBrush(primary ? C_ACCENT : C_CARD);
    HPEN pen = CreatePen(PS_SOLID, 1, primary ? C_ACCENT : C_LINE), oldp = SelectObject(dc, pen);
    HBRUSH oldb = SelectObject(dc, b);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, dpx(6), dpx(6));
    SelectObject(dc, oldb); SelectObject(dc, oldp);
    DeleteObject(b); DeleteObject(pen);
    text(dc, g_f_body, primary ? RGB(255, 255, 255) : C_TEXT, rc, label, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* one card; returns its bottom in client coordinates */
static int draw_card(HDC dc, int x, int w, int y, int idx, int client_bottom)
{
    app_t *a = &g_apps[idx];
    int t = card_target(idx), cardh = dpx(96), pad = dpx(16), badge = dpx(48), textright;
    app_t *s = &g_apps[t];          /* the build the card acts on */
    RECT card = { x, y, x + w, y + cardh }, r;
    BOOL sel = idx == g_sel, two = FALSE;
    HBRUSH cb;
    HPEN pen, oldp;
    HBRUSH oldb;
    HBITMAP pic;
    if (card.bottom < 0 || card.top > client_bottom) return y + cardh + dpx(12);   /* off screen */
    cb = CreateSolidBrush(C_CARD);
    pen = CreatePen(PS_SOLID, sel ? 2 : 1, sel ? C_SEL : C_LINE);
    oldp = SelectObject(dc, pen);
    oldb = SelectObject(dc, cb);
    RoundRect(dc, card.left, card.top, card.right, card.bottom, dpx(8), dpx(8));
    SelectObject(dc, oldb); SelectObject(dc, oldp);
    DeleteObject(cb); DeleteObject(pen);
    add_list_hit(H_CARD, idx, card, client_bottom);

    /* its picture (icons.c); until it is here, or without one, a letter
     * badge in the entry's colour */
    pic = icon_for(a);
    if (!pic) pic = icon_for(s);
#ifdef SG_MUTANT_NOICON
    pic = NULL;     /* the mutant keeps the letter badges */
#endif
    if (pic) {
        RECT ir = { x + pad, y + pad, x + pad + badge, y + pad + badge };
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP ob = SelectObject(mdc, pic);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(dc, x + pad, y + pad, badge, badge, mdc, 0, 0, badge, badge, bf);
        SelectObject(mdc, ob);
        DeleteDC(mdc);
        add_list_hit(H_ICON, idx, ir, client_bottom);
    } else {
        DWORD c = a->colour;
        HBRUSH bb = CreateSolidBrush(RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff));
        HBRUSH ob = SelectObject(dc, bb);
        HPEN np = SelectObject(dc, GetStockObject(NULL_PEN));
        WCHAR letter[2] = { a->name[0], 0 };
        RECT br = { x + pad, y + pad, x + pad + badge, y + pad + badge };
        RoundRect(dc, br.left, br.top, br.right, br.bottom, dpx(8), dpx(8));
        SelectObject(dc, ob); SelectObject(dc, np);
        DeleteObject(bb);
        text(dc, g_f_head, RGB(255, 255, 255), br, letter, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    /* the buttons, right to left: the main one, then Uninstall or the
     * choice of build; and what is going on */
    {
        RECT bt = { x + w - dpx(134), y + dpx(30), x + w - pad, y + dpx(30) + dpx(34) };
        RECT b2 = { bt.left - dpx(150), bt.top, bt.left - dpx(8), bt.bottom };
        RECT st = { bt.left - dpx(250), y + dpx(8), x + w - pad, y + dpx(28) };
        if (t == g_busy && s->state == AST_REMOVING) {
            text(dc, g_f_body, C_SUB, bt, L"Uninstalling...", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (t == g_busy || s->state == AST_INSTALLING) {
            text(dc, g_f_body, C_SUB, bt, L"Installing...", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (s->queued) {
            button(dc, bt, L"Cancel", FALSE); add_list_hit(H_UNQUEUE, t, bt, client_bottom);
            text(dc, g_f_small, C_SUB, st, g_qremove[t] ? L"Waiting to uninstall" : L"Waiting to install",
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else if (has_it(s)) {
            WCHAR what[64];
            button(dc, bt, s->state == AST_UPDATE ? L"Update" : L"Open", s->state == AST_UPDATE);
            add_list_hit(s->state == AST_UPDATE ? H_UPDATE : H_OPEN, t, bt, client_bottom);
            if (can_uninstall(s)) { button(dc, b2, L"Uninstall", FALSE); add_list_hit(H_UNINSTALL, t, b2, client_bottom); two = TRUE; }
            swprintf(what, ARRAYSIZE(what), L"Installed%ls", a->alt >= 0 ? (s->tier == TIER_LINUX ? L" (Linux app)" : L" (Windows program)") : L"");
            text(dc, g_f_small, C_OK, st, what, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else {
            button(dc, bt, s->state == AST_FAILED ? L"Try again" : L"Install", TRUE);
            add_list_hit(H_INSTALL, t, bt, client_bottom);
            if (a->alt >= 0) {      /* which build: a drop-down */
                WCHAR lab[32];
                POINT tri[3];
                HBRUSH tb = CreateSolidBrush(C_TEXT), otb;
                HPEN np;
                RECT lr = b2;
                swprintf(lab, ARRAYSIZE(lab), L"%ls", a->use_alt ? L"Linux app" : L"Windows program");
                button(dc, b2, L"", FALSE);
                lr.right -= dpx(18);
                text(dc, g_f_body, C_TEXT, lr, lab, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                tri[0].x = b2.right - dpx(20); tri[0].y = (b2.top + b2.bottom) / 2 - dpx(2);
                tri[1].x = b2.right - dpx(12); tri[1].y = tri[0].y;
                tri[2].x = b2.right - dpx(16); tri[2].y = tri[0].y + dpx(4);
                otb = SelectObject(dc, tb); np = SelectObject(dc, GetStockObject(NULL_PEN));
                Polygon(dc, tri, 3);
                SelectObject(dc, otb); SelectObject(dc, np); DeleteObject(tb);
                add_list_hit(H_BUILD, idx, b2, client_bottom);
                two = TRUE;
            }
        }
        textright = two ? b2.left - dpx(10) : bt.left - dpx(10);
    }

    r.left = x + pad + badge + dpx(14); r.right = textright; r.top = y + dpx(12); r.bottom = y + dpx(36);
    if (a->checked && !checkable(idx)) a->checked = FALSE;   /* installed meanwhile */
    if (checkable(idx)) {   /* a box to check it, before its name */
        int sz = dpx(16);
        RECT box = { r.left, (r.top + r.bottom - sz) / 2, r.left + sz, (r.top + r.bottom + sz) / 2 }, hit = box;
        HBRUSH bb = CreateSolidBrush(a->checked ? C_ACCENT : C_CARD);
        HPEN bp = CreatePen(PS_SOLID, 1, a->checked ? C_ACCENT : C_SUB), op = SelectObject(dc, bp);
        HBRUSH ob = SelectObject(dc, bb);
        RoundRect(dc, box.left, box.top, box.right, box.bottom, dpx(3), dpx(3));
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(bb); DeleteObject(bp);
        if (a->checked) {
            HPEN tick = CreatePen(PS_SOLID, max(2, dpx(2)), RGB(255, 255, 255));
            op = SelectObject(dc, tick);
            MoveToEx(dc, box.left + sz * 3 / 16, box.top + sz / 2, NULL);
            LineTo(dc, box.left + sz * 7 / 16, box.top + sz * 3 / 4);
            LineTo(dc, box.left + sz * 13 / 16, box.top + sz / 4);
            SelectObject(dc, op); DeleteObject(tick);
        }
        InflateRect(&hit, dpx(4), dpx(4));
        add_list_hit(H_CHECK, idx, hit, client_bottom);
        r.left += sz + dpx(10);
    }
    text(dc, g_f_head, C_TEXT, r, a->name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    r.top = r.bottom; r.bottom = y + dpx(54);
    {
        WCHAR sub[300];
        if (a->tier == TIER_LINUX) swprintf(sub, ARRAYSIZE(sub), L"%ls  \x00b7  Linux app", a->publisher);
        else if (a->alt >= 0) swprintf(sub, ARRAYSIZE(sub), L"%ls  \x00b7  Linux app or Windows program", a->publisher);
        else lstrcpynW(sub, a->publisher, ARRAYSIZE(sub));
        text(dc, g_f_small, C_SUB, r, sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    r.top = r.bottom; r.bottom = y + cardh - dpx(8);
    if ((s->state == AST_FAILED || s->msg_error) && s->msg[0])
        text(dc, g_f_small, C_ERR, r, s->msg, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    else
        text(dc, g_f_body, C_SUB, r, a->desc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    return y + cardh + dpx(12);
}

/* The content's band across the window, and its columns of cards: one up to
 * about 900 px, two or three wider -- full screen, one 720 px column left
 * most of the screen empty (David). Columns are at most 560 px wide. */
#define CARD_GAP 16
static int content_box(const RECT *rc, int *x, int *w)
{
    int avail = rc->right - dpx(40), cols = avail >= dpx(1400) ? 3 : avail >= dpx(900) ? 2 : 1;
    int colw = min(dpx(560), (avail - (cols - 1) * dpx(CARD_GAP)) / cols);
    if (cols == 1) colw = min(avail, dpx(720));
    *w = cols * colw + (cols - 1) * dpx(CARD_GAP);
    *x = (rc->right - *w) / 2;
    return cols;
}

static void layout_search(HWND hwnd)
{
    RECT rc;
    int x, w;
    GetClientRect(hwnd, &rc);
    content_box(&rc, &x, &w);
    if (g_search) MoveWindow(g_search, x + dpx(70), dpx(66), w - dpx(70), dpx(28), TRUE);
}

/* ---- an app's details page ------------------------------------------------------------------------- */

/* text wrapped in a width: drawn at y, the height it took */
static int wrapped(HDC dc, HFONT font, COLORREF col, int x, int w, int y, const WCHAR *s, int client_bottom)
{
    RECT r = { x, y, x + w, y + 10 };
    HFONT of = SelectObject(dc, font);
    DrawTextW(dc, s, -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    SelectObject(dc, of);
    if (r.bottom >= 0 && r.top <= client_bottom) text(dc, font, col, r, s, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
    return r.bottom - r.top;
}

static int fact(HDC dc, int x, int w, int y, const WCHAR *label, const WCHAR *value, int client_bottom)
{
    int h;
    RECT l = { x, y, x + dpx(150), y + dpx(22) };
    if (!value || !value[0]) return 0;
    text(dc, g_f_body, C_SUB, l, label, DT_LEFT | DT_TOP | DT_SINGLELINE);
    h = wrapped(dc, g_f_body, C_TEXT, x + dpx(160), w - dpx(160), y, value, client_bottom);
    return max(h, dpx(22)) + dpx(6);
}

static void paint_details(HDC dc, const RECT *rc)
{
    int t = card_target(g_detail), x, w, y, head = dpx(64), badge = dpx(72), bx;
    app_t *a = &g_apps[g_detail], *s = &g_apps[t];
    details_t *d = details_get(t);
    BOOL ready = d && d->state == 2;
    WCHAR line[1024];
    HBITMAP pic;

    content_box(rc, &x, &w);
    if (w > dpx(900)) { x += (w - dpx(900)) / 2; w = dpx(900); }
    y = head + dpx(16) - g_scroll;

    /* what it is */
    pic = icon_for(a);
    if (!pic) pic = icon_for(s);
    if (pic) {
        HDC mdc = CreateCompatibleDC(dc);
        HBITMAP ob = SelectObject(mdc, pic);
        BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        BITMAP bm;
        GetObjectW(pic, sizeof(bm), &bm);
        AlphaBlend(dc, x, y, badge, badge, mdc, 0, 0, bm.bmWidth, bm.bmHeight, bf);
        SelectObject(mdc, ob);
        DeleteDC(mdc);
    } else {
        DWORD c = a->colour;
        HBRUSH bb = CreateSolidBrush(RGB((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff)), ob = SelectObject(dc, bb);
        HPEN np = SelectObject(dc, GetStockObject(NULL_PEN));
        WCHAR letter[2] = { a->name[0], 0 };
        RECT br = { x, y, x + badge, y + badge };
        RoundRect(dc, br.left, br.top, br.right, br.bottom, dpx(10), dpx(10));
        SelectObject(dc, ob); SelectObject(dc, np); DeleteObject(bb);
        text(dc, g_f_title, RGB(255, 255, 255), br, letter, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    {
        RECT n = { x + badge + dpx(18), y, x + w, y + dpx(40) };
        text(dc, g_f_title, C_TEXT, n, a->name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        n.top = n.bottom; n.bottom = n.top + dpx(22);
        swprintf(line, ARRAYSIZE(line), L"%ls  \x00b7  %ls  \x00b7  %ls", a->publisher, a->category,
                 s->tier == TIER_LINUX ? L"Linux app" : s->tier == TIER_OURS ? L"Made for Stained Glass OS" : L"Windows program");
        text(dc, g_f_body, C_SUB, n, line, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    y += badge + dpx(14);

    /* what it can do: the card's buttons */
    bx = x;
    {
        RECT bt = { bx, y, bx + dpx(140), y + dpx(34) };
        if (t == g_busy || s->state == AST_INSTALLING || s->state == AST_REMOVING) {
            text(dc, g_f_body, C_SUB, bt, s->state == AST_REMOVING ? L"Uninstalling..." : L"Installing...", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else if (s->queued) {
            button(dc, bt, L"Cancel", FALSE); add_hit(H_UNQUEUE, t, bt);
        } else if (has_it(s)) {
            button(dc, bt, s->state == AST_UPDATE ? L"Update" : L"Open", TRUE);
            add_hit(s->state == AST_UPDATE ? H_UPDATE : H_OPEN, t, bt);
            if (can_uninstall(s)) {
                RECT b2 = { bt.right + dpx(10), bt.top, bt.right + dpx(150), bt.bottom };
                button(dc, b2, L"Uninstall", FALSE); add_hit(H_UNINSTALL, t, b2);
            }
        } else {
            button(dc, bt, s->state == AST_FAILED ? L"Try again" : L"Install", TRUE);
            add_hit(H_INSTALL, t, bt);
            if (a->alt >= 0) {
                RECT b2 = { bt.right + dpx(10), bt.top, bt.right + dpx(190), bt.bottom };
                button(dc, b2, a->use_alt ? L"Linux app  \x25be" : L"Windows program  \x25be", FALSE);
                add_hit(H_BUILD, g_detail, b2);
            }
        }
        y += dpx(48);
    }
    if (s->msg[0]) { y += wrapped(dc, g_f_small, s->msg_error || s->state == AST_FAILED ? C_ERR : C_SUB, x, w, y, s->msg, rc->bottom) + dpx(8); }

    /* where it comes from */
    {
        RECT h = { x, y, x + w, y + dpx(28) };
        text(dc, g_f_head, C_TEXT, h, L"Where it comes from", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += dpx(32);
        details_source(s, ready ? d : NULL, line, ARRAYSIZE(line));
        y += wrapped(dc, g_f_body, C_TEXT, x, w, y, line, rc->bottom) + dpx(18);
    }

    /* what it is, in its maker's words */
    {
        RECT h = { x, y, x + w, y + dpx(28) };
        text(dc, g_f_head, C_TEXT, h, L"About", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += dpx(32);
        if (!ready) y += wrapped(dc, g_f_body, C_SUB, x, w, y, L"Fetching its description...", rc->bottom) + dpx(18);
        else {
            if (d->summary[0] && lstrcmpW(d->summary, d->desc)) y += wrapped(dc, g_f_head, C_TEXT, x, w, y, d->summary, rc->bottom) + dpx(8);
            y += wrapped(dc, g_f_body, C_TEXT, x, w, y, d->desc, rc->bottom) + dpx(18);
        }
    }

    /* the facts */
    {
        RECT h = { x, y, x + w, y + dpx(28) };
        WCHAR id[160] = L"";
        text(dc, g_f_head, C_TEXT, h, L"Details", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += dpx(34);
        if (ready && d->version[0]) y += fact(dc, x, w, y, s->tier == TIER_WINDOWS ? L"Latest version" : L"Version", d->version, rc->bottom);
        else if (s->available_version[0]) y += fact(dc, x, w, y, L"Latest version", s->available_version, rc->bottom);
        if (s->installed_version[0]) y += fact(dc, x, w, y, L"Installed", s->installed_version, rc->bottom);
        if (ready) {
            if (d->homepage[0]) {
                RECT l = { x, y, x + dpx(150), y + dpx(22) }, v = { x + dpx(160), y, x + w, y + dpx(22) };
                HFONT of = SelectObject(dc, g_f_body);
                SIZE sz;
                text(dc, g_f_body, C_SUB, l, L"Website", DT_LEFT | DT_TOP | DT_SINGLELINE);
                GetTextExtentPoint32W(dc, d->homepage, lstrlenW(d->homepage), &sz);
                SelectObject(dc, of);
                v.right = min(v.left + sz.cx, x + w);
                text(dc, g_f_body, C_SEL, v, d->homepage, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
                add_hit(H_HOMEPAGE, t, v);
                y += dpx(28);
            }
            y += fact(dc, x, w, y, L"License", d->license, rc->bottom);
            y += fact(dc, x, w, y, L"Installed size", d->size, rc->bottom);
            y += fact(dc, x, w, y, s->tier == TIER_WINDOWS ? L"Publisher" : L"Maintainer", d->maintainer, rc->bottom);
            if (d->origin[0] && s->tier != TIER_WINDOWS) y += fact(dc, x, w, y, L"Repository", d->origin, rc->bottom);
            if (d->error[0]) y += fact(dc, x, w, y, L"Note", d->error, rc->bottom);
        }
        if (s->method == SRC_WINGET) swprintf(id, ARRAYSIZE(id), L"%ls (winget)", s->winget_id);
        else if (s->apt_pkg[0]) swprintf(id, ARRAYSIZE(id), L"%ls (apt)", s->apt_pkg);
        y += fact(dc, x, w, y, L"Package", id, rc->bottom);
    }
    g_extent = y + g_scroll + dpx(30);

    /* the bar over it: Back */
    {
        RECT hb = { 0, 0, rc->right, head }, bt = { x, dpx(14), x + dpx(110), dpx(48) }, tt;
        HBRUSH bg = CreateSolidBrush(C_BG);
        HPEN pen = CreatePen(PS_SOLID, 1, C_LINE), op;
        FillRect(dc, &hb, bg); DeleteObject(bg);
        op = SelectObject(dc, pen);
        MoveToEx(dc, 0, head - 1, NULL); LineTo(dc, rc->right, head - 1);
        SelectObject(dc, op); DeleteObject(pen);
        button(dc, bt, L"\x2190  Back", FALSE);
        add_hit(H_BACK, -1, bt);
        tt.left = bt.right + dpx(16); tt.top = bt.top; tt.right = x + w; tt.bottom = bt.bottom;
        text(dc, g_f_head, C_TEXT, tt, L"SG Store", DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

static void open_details(HWND hwnd, int i)
{
    if (i < 0 || i >= g_napps) return;
    g_detail = i;
    g_sel = i;
    g_list_scroll = g_scroll;
    g_scroll = 0;
    details_get(card_target(i));
    ShowWindow(g_search, SW_HIDE);
    SetFocus(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void close_details(HWND hwnd)
{
    if (g_detail < 0) return;
    g_detail = -1;
    g_scroll = g_list_scroll;
    ShowWindow(g_search, SW_SHOW);
    SetFocus(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void paint(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(hwnd, &ps), dc;
    RECT rc;
    HBITMAP bmp, oldbmp;
    int x, w, i, top, head = header_height(), cols, colw, col = 0, rowbottom;
    WCHAR cursec[64] = L"";
    GetClientRect(hwnd, &rc);
    dc = CreateCompatibleDC(wdc);
    bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    oldbmp = SelectObject(dc, bmp);
    { HBRUSH bg = CreateSolidBrush(C_BG); FillRect(dc, &rc, bg); DeleteObject(bg); }

    g_nhits = 0;
    if (g_detail >= 0) { paint_details(dc, &rc); goto drawn; }
    g_cols = cols = content_box(&rc, &x, &w);
    colw = (w - (cols - 1) * dpx(CARD_GAP)) / cols;

    /* the list (scrolled), under the header: sections, each a grid of cards */
    build_view();
    top = head + dpx(8) - g_scroll;
    rowbottom = top;
    for (i = 0; i < g_nview; i++) {
        const app_t *a = &g_apps[g_view[i]];
        if (lstrcmpiW(app_section(a), cursec)) {
            if (col) { top = rowbottom; col = 0; }   /* the last row's end */
            RECT ch = { x, top, x + w, top + dpx(26) };
            lstrcpynW(cursec, app_section(a), ARRAYSIZE(cursec));
            text(dc, g_f_head, C_TEXT, ch, cursec, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            top += dpx(30);
            if (!lstrcmpiW(cursec, LINUX_SECTION)) {
                RECT sub = { x, top, x + w, top + dpx(34) };
                text(dc, g_f_small, C_SUB, sub,
                     L"Built for Linux and installed with the system's package manager (an administrator is asked). "
                     L"An app made for both is one card in its category, with a choice of build.",
                     DT_LEFT | DT_TOP | DT_WORDBREAK);
                top += dpx(38);
            }
        }
        g_view_y[i] = top + g_scroll;
        rowbottom = max(rowbottom, draw_card(dc, x + col * (colw + dpx(CARD_GAP)), colw, top, g_view[i], rc.bottom));
        if (++col == cols) { col = 0; top = rowbottom; }
    }
    if (col) top = rowbottom;
    if (!g_nview) {
        RECT e = { x, top + dpx(20), x + w, top + dpx(60) };
        WCHAR msg[256];
        if (g_query[0]) swprintf(msg, ARRAYSIZE(msg), L"No apps match \"%ls\".", g_query);
        else lstrcpynW(msg, L"No apps here.", ARRAYSIZE(msg));
        text(dc, g_f_body, C_SUB, e, msg, DT_CENTER | DT_TOP | DT_SINGLELINE);
        top = e.bottom;
    }
    g_extent = top + g_scroll + dpx(20);

    /* the header, fixed over the list */
    {
        RECT hb = { 0, 0, rc.right, head };
        HBRUSH bg = CreateSolidBrush(C_BG);
        HPEN line = CreatePen(PS_SOLID, 1, C_LINE), op = SelectObject(dc, line);
        FillRect(dc, &hb, bg);
        DeleteObject(bg);
        MoveToEx(dc, 0, head - 1, NULL); LineTo(dc, rc.right, head - 1);
        SelectObject(dc, op); DeleteObject(line);
    }
    { RECT h = { x, dpx(14), x + w - dpx(330), dpx(54) };
      text(dc, g_f_title, C_TEXT, h, L"SG Store", DT_LEFT | DT_VCENTER | DT_SINGLELINE); }
    { RECT bt = { x + w - dpx(150), dpx(18), x + w, dpx(50) };
      button(dc, bt, L"Check for updates", FALSE); add_hit(H_CHECKALL, -1, bt); }
    { RECT bt = { x + w - dpx(310), dpx(18), x + w - dpx(160), dpx(50) };
      button(dc, bt, L"Install from a file...", FALSE); add_hit(H_FROMFILE, -1, bt); }
    { RECT l = { x, dpx(66), x + dpx(66), dpx(94) };
      text(dc, g_f_body, C_TEXT, l, L"Search", DT_LEFT | DT_VCENTER | DT_SINGLELINE); }
    /* the categories */
    {
        int cx = x, cy = dpx(104), k;
        for (k = 0; k < NCATS; k++) {
            SIZE sz;
            RECT chip;
            HFONT of = SelectObject(dc, g_f_small);
            GetTextExtentPoint32W(dc, g_cats[k], lstrlenW(g_cats[k]), &sz);
            SelectObject(dc, of);
            if (cx + sz.cx + dpx(20) > x + w) break;
            chip.left = cx; chip.top = cy; chip.right = cx + sz.cx + dpx(20); chip.bottom = cy + dpx(24);
            {
                HBRUSH b = CreateSolidBrush(k == g_cat ? C_ACCENT : C_CARD);
                HPEN pen = CreatePen(PS_SOLID, 1, k == g_cat ? C_ACCENT : C_LINE), op = SelectObject(dc, pen);
                HBRUSH ob = SelectObject(dc, b);
                RoundRect(dc, chip.left, chip.top, chip.right, chip.bottom, dpx(12), dpx(12));
                SelectObject(dc, ob); SelectObject(dc, op);
                DeleteObject(b); DeleteObject(pen);
            }
            text(dc, g_f_small, k == g_cat ? RGB(255, 255, 255) : C_TEXT, chip, g_cats[k], DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            add_hit(H_CAT, k, chip);
            cx = chip.right + dpx(6);
        }
    }
    {
        int n = count_checked();
        RECT sub = { x, dpx(134), x + w, dpx(154) };
        if (n) {
            WCHAR lab[48];
            RECT bt = { x + w - dpx(190), dpx(128), x + w, dpx(158) };
            swprintf(lab, ARRAYSIZE(lab), L"Install selected (%d)", n);
            button(dc, bt, lab, TRUE);
            add_hit(H_INSTALLSEL, -1, bt);
            sub.right = bt.left - dpx(10);
        }
        text(dc, g_f_small, C_SUB, sub, L"App and system updates are handled in Settings > Update & Security.",
             DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

drawn:
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldbmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    EndPaint(hwnd, &ps);

    {
        SCROLLINFO si = { sizeof(si) };
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0; si.nMax = g_extent; si.nPage = rc.bottom; si.nPos = g_scroll;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
    }
    dump();
}

/* header hits were added last, but they sit over the list: they win */
static int hit_at(int cx, int cy, int *verb, int *idx)
{
    POINT pt = { cx, cy };
    int i;
    if (cy < header_height() && g_detail < 0) {
        for (i = 0; i < g_nhits; i++)
            if ((g_hits[i].verb == H_CHECKALL || g_hits[i].verb == H_CAT || g_hits[i].verb == H_FROMFILE ||
                 g_hits[i].verb == H_INSTALLSEL) && PtInRect(&g_hits[i].rc, pt))
                { *verb = g_hits[i].verb; *idx = g_hits[i].idx; return 1; }
        return 0;
    }
    for (i = 0; i < g_nhits; i++)       /* buttons before the card they sit on */
        if (g_hits[i].verb != H_CARD && g_hits[i].verb != H_ICON && PtInRect(&g_hits[i].rc, pt)) { *verb = g_hits[i].verb; *idx = g_hits[i].idx; return 1; }
    for (i = 0; i < g_nhits; i++)
        if (g_hits[i].verb == H_CARD && PtInRect(&g_hits[i].rc, pt)) { *verb = H_CARD; *idx = g_hits[i].idx; return 1; }
    return 0;
}

static void do_checkall(void)
{
    int i;
    WCHAR err[512];
    for (i = 0; i < g_napps; i++) {
        if (g_apps[i].tier == TIER_LINUX) continue;
        app_check_update(&g_apps[i], err, ARRAYSIZE(err));
    }
    InvalidateRect(g_wnd, NULL, FALSE);
    dump();
}

static void scroll_to(HWND hwnd, int pos)
{
    RECT rc; GetClientRect(hwnd, &rc);
    int max = g_extent - rc.bottom;
    if (max < 0) max = 0;
    if (pos < 0) pos = 0;
    if (pos > max) pos = max;
    if (pos != g_scroll) { g_scroll = pos; InvalidateRect(hwnd, NULL, FALSE); }
}

/* make the selected card visible */
static void reveal(HWND hwnd)
{
    RECT rc;
    int i, y = -1, top, bottom;
    GetClientRect(hwnd, &rc);
    for (i = 0; i < g_nview; i++) if (g_view[i] == g_sel) y = g_view_y[i];
    if (y < 0) return;
    top = y - g_scroll; bottom = top + dpx(96);
    if (top < header_height() + dpx(8)) scroll_to(hwnd, y - header_height() - dpx(40));
    else if (bottom > rc.bottom - dpx(8)) scroll_to(hwnd, y + dpx(96) - rc.bottom + dpx(16));
}

static void move_selection(HWND hwnd, int delta)
{
    int i, pos = -1;
    build_view();
    if (!g_nview) return;
    for (i = 0; i < g_nview; i++) if (g_view[i] == g_sel) pos = i;
    pos = pos < 0 ? (delta >= 0 ? 0 : g_nview - 1) : pos + delta;
    if (pos < 0) pos = 0;
    if (pos >= g_nview) pos = g_nview - 1;
    g_sel = g_view[pos];
    InvalidateRect(hwnd, NULL, FALSE);
    UpdateWindow(hwnd);
    reveal(hwnd);
}

static void set_category(HWND hwnd, int k)
{
    if (k < 0 || k >= NCATS) return;
    g_cat = k;
    g_scroll = 0;
    g_sel = -1;
    InvalidateRect(hwnd, NULL, FALSE);
}

/* the search box: typing filters at once; arrows and Enter drive the list */
static LRESULT CALLBACK search_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN) {
        if (wp == VK_DOWN || wp == VK_UP || wp == VK_NEXT || wp == VK_PRIOR) {
            SetFocus(g_wnd);
            move_selection(g_wnd, wp == VK_UP || wp == VK_PRIOR ? -1 : 1);
            return 0;
        }
        if (wp == VK_RETURN) {
            if (g_sel < 0) { build_view(); if (g_nview) g_sel = g_view[0]; }
            SetFocus(g_wnd);
            InvalidateRect(g_wnd, NULL, FALSE);
            reveal(g_wnd);
            return 0;
        }
        if (wp == VK_ESCAPE) { SetWindowTextW(hwnd, L""); return 0; }
    }
    if (msg == WM_CHAR && (wp == L'\r' || wp == 27)) return 0;   /* no beep */
    return CallWindowProcW(g_search_proc, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                   0, 0, 10, 10, hwnd, (HMENU)100, ((CREATESTRUCTW *)lp)->hInstance, NULL);
        SendMessageW(g_search, WM_SETFONT, (WPARAM)g_f_body, FALSE);
        SendMessageW(g_search, EM_LIMITTEXT, ARRAYSIZE(g_query) - 1, 0);
        g_search_proc = (WNDPROC)SetWindowLongPtrW(g_search, GWLP_WNDPROC, (LONG_PTR)search_proc);
        layout_search(hwnd);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == 100 && HIWORD(wp) == EN_CHANGE) {
            GetWindowTextW(g_search, g_query, ARRAYSIZE(g_query));
            g_scroll = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_SETFOCUS:
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) { redetect(); InvalidateRect(hwnd, NULL, FALSE); }
        if (LOWORD(wp) != WA_INACTIVE && g_sel < 0) SetFocus(g_search);
        return 0;
    case WM_DETAILS:
        if (g_detail >= 0) InvalidateRect(hwnd, NULL, FALSE);
        dump();
        return 0;
    case WM_XBUTTONUP:
        if (GET_XBUTTON_WPARAM(wp) == XBUTTON1) close_details(hwnd);
        return TRUE;
    case WM_ICONS:
        InvalidateRect(hwnd, NULL, FALSE);
        if (wp) dump();
        return 0;
    case WM_LBUTTONUP: {
        int verb, idx;
        if (hit_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &verb, &idx)) {
            if (verb == H_INSTALL || verb == H_UPDATE) start_install(idx);
            else if (verb == H_OPEN) open_app(&g_apps[idx]);
            else if (verb == H_UNINSTALL) ask_uninstall(hwnd, idx);
            else if (verb == H_UNQUEUE) unqueue(idx);
            else if (verb == H_BUILD) { POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) }; choose_build(hwnd, idx, pt); }
            else if (verb == H_CHECKALL) do_checkall();
            else if (verb == H_CHECK) toggle_check(idx);
            else if (verb == H_INSTALLSEL) install_selected();
            else if (verb == H_FROMFILE) install_from_file();
            else if (verb == H_CAT) set_category(hwnd, idx);
#ifndef SG_MUTANT_NODETAILS
            else if (verb == H_CARD || verb == H_ICON) open_details(hwnd, idx);   /* the card, not its buttons: its page */
#else
            else if (verb == H_CARD) { g_sel = idx; SetFocus(hwnd); InvalidateRect(hwnd, NULL, FALSE); }
#endif
            else if (verb == H_BACK) close_details(hwnd);
            else if (verb == H_HOMEPAGE) {
                details_t *d = details_get(idx);
                if (d && (!wcsncmp(d->homepage, L"https://", 8) || !wcsncmp(d->homepage, L"http://", 7)))
                    ShellExecuteW(hwnd, NULL, d->homepage, NULL, NULL, SW_SHOWNORMAL);
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (g_detail >= 0) {
            switch (wp) {
            case VK_ESCAPE: case VK_BACK: case VK_BROWSER_BACK: close_details(hwnd); return 0;
            case VK_RETURN: activate(g_detail); InvalidateRect(hwnd, NULL, FALSE); return 0;
            case VK_DOWN: scroll_to(hwnd, g_scroll + dpx(40)); return 0;
            case VK_UP: scroll_to(hwnd, g_scroll - dpx(40)); return 0;
            case VK_NEXT: scroll_to(hwnd, g_scroll + dpx(300)); return 0;
            case VK_PRIOR: scroll_to(hwnd, g_scroll - dpx(300)); return 0;
            }
            return 0;
        }
        switch (wp) {
        case VK_DOWN: move_selection(hwnd, 1); return 0;
        case VK_UP: move_selection(hwnd, -1); return 0;
        case VK_NEXT: move_selection(hwnd, 5); return 0;
        case VK_PRIOR: move_selection(hwnd, -5); return 0;
        case VK_HOME: g_sel = -1; move_selection(hwnd, 1); return 0;
        case VK_END: g_sel = -1; move_selection(hwnd, -1); return 0;
        case VK_RETURN: activate(g_sel); InvalidateRect(hwnd, NULL, FALSE); return 0;
        case VK_SPACE:   /* as a list of check boxes: Space checks it; one that cannot be checked, as Enter */
            if (g_sel >= 0 && checkable(g_sel)) toggle_check(g_sel);
            else activate(g_sel);
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        case VK_ESCAPE: case VK_TAB: SetFocus(g_search); SendMessageW(g_search, EM_SETSEL, 0, -1); dump(); return 0;
        }
        return 0;
    case WM_CHAR:
        /* typing in the list goes to the search box */
        if (g_detail < 0 && wp >= L' ' && wp != 0x7f && GetFocus() == hwnd) {
            SetFocus(g_search);
            SendMessageW(g_search, EM_SETSEL, -1, -1);
            SendMessageW(g_search, WM_CHAR, wp, lp);
        }
        return 0;
    case WM_MOUSEWHEEL:
        scroll_to(hwnd, g_scroll - GET_WHEEL_DELTA_WPARAM(wp) / 2);
        return 0;
    case WM_VSCROLL: {
        RECT rc; GetClientRect(hwnd, &rc);
        int pos = g_scroll;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= dpx(40); break;
        case SB_LINEDOWN: pos += dpx(40); break;
        case SB_PAGEUP: pos -= rc.bottom - header_height(); break;
        case SB_PAGEDOWN: pos += rc.bottom - header_height(); break;
        case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = HIWORD(wp); break;
        }
        scroll_to(hwnd, pos);
        return 0;
    }
    case WM_SIZE:
        layout_search(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_PAINT:
        paint(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        InterlockedExchange(&g_cancel, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void make_fonts(void)
{
    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    HDC dc = GetDC(NULL);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    ReleaseDC(NULL, dc);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_f_body = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -dpx(11); g_f_small = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -dpx(15); ncm.lfMessageFont.lfWeight = FW_SEMIBOLD; g_f_head = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -dpx(24); ncm.lfMessageFont.lfWeight = FW_SEMIBOLD; g_f_title = CreateFontIndirectW(&ncm.lfMessageFont);
}

static int window(HINSTANCE inst, const WCHAR *query)
{
    WNDCLASSW wc = { 0 };
    MSG m;
    make_fonts();
    load_all(FALSE);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"SgStore";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, L"SG Store",
                            WS_OVERLAPPEDWINDOW | WS_VSCROLL,
                            CW_USEDEFAULT, CW_USEDEFAULT, dpx(820), dpx(680), NULL, NULL, inst, NULL);
    if (!g_wnd) return 1;
    icons_fetch(g_apps, g_napps, dpx(48), g_wnd, WM_ICONS);
    if (query && query[0]) SetWindowTextW(g_search, query);
    ShowWindow(g_wnd, SW_SHOWNORMAL);
    UpdateWindow(g_wnd);
    SetFocus(g_search);
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (m.message == WM_KEYDOWN && m.hwnd == g_search && m.wParam == VK_TAB) { SetFocus(g_wnd); move_selection(g_wnd, 0); continue; }
        if (m.message == WM_KEYDOWN && (m.wParam == 'F' || m.wParam == 'E') && (GetKeyState(VK_CONTROL) & 0x8000)) {
            SetFocus(g_search); SendMessageW(g_search, EM_SETSEL, 0, -1); dump(); continue;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

/* ---- the command line ------------------------------------------------------------------------------ */

static int find_app(const WCHAR *key)
{
    int i;
    for (i = 0; i < g_napps; i++)
        if (!lstrcmpiW(g_apps[i].ord, key) || !lstrcmpiW(g_apps[i].name, key) ||
            (g_apps[i].winget_id[0] && !lstrcmpiW(g_apps[i].winget_id, key)))
            return i;
    return -1;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show)
{
    int argc, i, rc = 0;
    WCHAR **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const WCHAR *query = NULL;
    (void)prev; (void)cmdline; (void)show;
    GetEnvironmentVariableW(L"SG_STORE_DUMP", g_dump, MAX_PATH);
    InitializeCriticalSection(&g_qlock);

    for (i = 1; argv && i < argc; i++) {
        if (!lstrcmpiW(argv[i], L"--list")) {
            load_all(FALSE); dump(); LocalFree(argv); return 0;
        }
        if (!lstrcmpiW(argv[i], L"--check-updates")) {
            load_all(TRUE); dump(); LocalFree(argv); return 0;
        }
        if (!lstrcmpiW(argv[i], L"--deb") && i + 1 < argc) {
            rc = sys_deb_window(inst, argv[i + 1]);
            LocalFree(argv);
            return rc;
        }
        if (!lstrcmpiW(argv[i], L"--elevated-helper") && i + 1 < argc) {
            rc = sys_helper_main(argv[i + 1]);
            LocalFree(argv);
            return rc;
        }
        if (!lstrcmpiW(argv[i], L"--elevated-apt") || !lstrcmpiW(argv[i], L"--elevated-apt-remove") ||
            !lstrcmpiW(argv[i], L"--elevated-deb")) {
            rc = sys_elevated_main(argc, argv, i);
            LocalFree(argv);
            return rc;
        }
        if (!lstrcmpiW(argv[i], L"--open") && i + 1 < argc) {
            int k;
            load_all(FALSE);
            k = find_app(argv[i + 1]);
            if (k >= 0) open_app(&g_apps[k]);
            LocalFree(argv);
            return k < 0 ? 2 : 0;
        }
        if (!lstrcmpiW(argv[i], L"--search") && i + 1 < argc) { query = argv[++i]; continue; }
        if (!lstrcmpiW(argv[i], L"--uninstall") && i + 1 < argc) {
            /* the card's Uninstall, without its question (the gate) */
            int k;
            WCHAR err[512] = L"", tmp[MAX_PATH + 8];
            FILE *f;
            load_all(FALSE);
            k = find_app(argv[i + 1]);
            if (k < 0) { LocalFree(argv); return 2; }
            app_detect(&g_apps[k]);
            if (!can_uninstall(&g_apps[k])) { rc = SYS_FAILED; lstrcpynW(err, L"It cannot be uninstalled here.", ARRAYSIZE(err)); }
            else rc = app_uninstall(&g_apps[k], err, ARRAYSIZE(err));
            app_detect(&g_apps[k]);
            if (!rc && has_it(&g_apps[k])) { rc = 1; lstrcpynW(err, L"It is still installed.", ARRAYSIZE(err)); }
            if (g_dump[0]) {
                swprintf(tmp, ARRAYSIZE(tmp), L"%ls.result", g_dump);
                if ((f = _wfopen(tmp, L"wb"))) {
                    dumpf(f, L"uninstall %ls %ls %d %ls\n", g_apps[k].ord, rc ? L"fail" : L"ok", rc, err);
                    fclose(f);
                }
                dump();
            }
            LocalFree(argv);
            return rc ? 1 : 0;
        }
        if (!lstrcmpiW(argv[i], L"--install-batch") && i + 1 < argc) {
            /* several apps as Install selected does them: under one consent */
            int k, j, fails = 0;
            WCHAR err[512], tmp[MAX_PATH + 8];
            FILE *f = NULL;
            load_all(FALSE);
            if (argc - i - 1 > 1) sys_batch_begin();
            if (g_dump[0]) { swprintf(tmp, ARRAYSIZE(tmp), L"%ls.result", g_dump); f = _wfopen(tmp, L"wb"); }
            for (j = i + 1; j < argc; j++) {
                if ((k = find_app(argv[j])) < 0) { fails++; continue; }
                app_detect(&g_apps[k]);
                rc = app_install(&g_apps[k], progress_cb, NULL, &g_cancel, err, ARRAYSIZE(err));
                if (rc) fails++;
                if (f) { dumpf(f, L"result %ls %ls %d %ls\n", g_apps[k].ord, rc ? L"fail" : L"ok", rc, rc ? err : L"Installed."); fflush(f); }
            }
            sys_batch_end();
            if (f) fclose(f);
            LocalFree(argv);
            return fails ? 1 : 0;
        }
        if (!lstrcmpiW(argv[i], L"--install") && i + 1 < argc) {
            int k;
            WCHAR err[512], tmp[MAX_PATH + 8];
            FILE *f;
            load_all(FALSE);
            k = find_app(argv[i + 1]);
            if (k < 0) { LocalFree(argv); return 2; }
            app_detect(&g_apps[k]);
            rc = app_install(&g_apps[k], progress_cb, NULL, &g_cancel, err, ARRAYSIZE(err));
            g_apps[k].state = rc ? AST_FAILED : AST_DONE;
            lstrcpynW(g_apps[k].msg, rc ? err : L"Installed.", ARRAYSIZE(g_apps[k].msg));
            if (!rc) app_detect(&g_apps[k]);
            if (g_dump[0]) {
                swprintf(tmp, ARRAYSIZE(tmp), L"%ls.result", g_dump);
                if ((f = _wfopen(tmp, L"wb"))) {
                    dumpf(f, L"result %ls %ls %d %ls\n", g_apps[k].ord, rc ? L"fail" : L"ok", rc, g_apps[k].msg);
                    dumpf(f, L"elevated %ls %d\n", g_apps[k].ord, g_apps[k].ran_elevated);
                    fclose(f);
                }
                dump();
            }
            LocalFree(argv);
            return rc;
        }
    }
    rc = window(inst, query);
    if (argv) LocalFree(argv);
    return rc;
}
