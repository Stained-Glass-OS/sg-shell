/* SG Store -- the window and the command line.
 *
 *   sg-store64.exe                 open the store
 *   sg-store64.exe --list          write the catalogue and each app's state to
 *                                  the dump (headless), then exit
 *   sg-store64.exe --check-updates as --list, but resolve each app's newest
 *                                  available version first
 *   sg-store64.exe --install ID    install (or update) one app by ordinal,
 *                                  name or winget id (headless), then exit
 *   sg-store64.exe --install-elevated ID   the elevated (SYSTEM) copy's part of
 *                                  installing an "ours" app whose programs are
 *                                  a system package (catalog.c); exit code only
 *   sg-store64.exe --deb FILE      "Install a Linux package": what a .deb is,
 *                                  and Install (File Explorer's .deb verb)
 *   --elevated-apt / --elevated-deb   the elevated half (sysinstall.c)
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

static app_t g_apps[MAX_APPS];
static int g_napps;
static WCHAR g_dump[MAX_PATH];
static HWND g_wnd, g_search;
static WNDPROC g_search_proc;
static HFONT g_f_title, g_f_head, g_f_body, g_f_small;
static int g_dpi = 96, g_scroll, g_extent;
static volatile LONG g_cancel;
static int g_busy = -1;                   /* the app being installed, or -1 */
static WCHAR g_query[128];                /* the search box */
static int g_cat;                         /* the chosen category: 0 = all */
static int g_sel = -1;                    /* the selected app (keyboard), or -1 */
static int g_view[MAX_APPS], g_nview;     /* what the list shows, in order */
static int g_view_y[MAX_APPS];
static int g_cols = 1;                     /* the cards' columns, as last drawn */            /* each shown card's top, content coordinates */

#define dpx(x) MulDiv((x), g_dpi, 96)

/* The categories, in the order they are listed; anything else follows them,
 * and the Linux apps come last. */
static const WCHAR *const g_cats[] = {
    L"All", L"Browsers", L"Productivity", L"Graphics", L"Media", L"Development",
    L"Utilities", L"Internet", L"Games", LINUX_SECTION,
};
#define NCATS ((int)ARRAYSIZE(g_cats))

/* hit rectangles, for the mouse and the gate */
enum { H_INSTALL, H_OPEN, H_UPDATE, H_CHECKALL, H_CAT, H_FROMFILE, H_CARD };
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
    default:                return L"failed";
    }
}
static const WCHAR *method_name(int m)
{
    switch (m) {
    case SRC_WINGET:     return L"winget";
    case SRC_PIN:        return L"pin";
    case SRC_OURS_SETUP: return L"ours-setup";
    case SRC_OURS_APT:   return L"ours-apt";
    case SRC_LINUX_APT:  return L"linux-apt";
    default:             return L"unknown";
    }
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
            !(a->tier == TIER_LINUX && !lstrcmpiW(word, L"linux")))
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
    }
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
    dumpf(f, L"focus %ls\n", g_wnd && GetFocus() == g_search ? L"search" : L"list");
    for (i = 0; i < g_nhits; i++) {
        POINT pt = { (g_hits[i].rc.left + g_hits[i].rc.right) / 2, (g_hits[i].rc.top + g_hits[i].rc.bottom) / 2 };
        static const WCHAR *const verbs[] = { L"install", L"open", L"update", L"checkall", L"category", L"fromfile", L"card" };
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
    if (a->method == SRC_OURS_SETUP) { ShellExecuteW(NULL, NULL, L"sg-documents.exe", NULL, NULL, SW_SHOWNORMAL); return; }
    if (a->tier == TIER_LINUX && a->run[0]) { sys_run_linux(a->run); return; }
    /* Apps & features, where every installed app can be launched/removed */
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
}

/* ---- installing on a worker thread ----------------------------------------------------------------- */

static void progress_cb(void *ctx, int stage, ULONGLONG done, ULONGLONG total)
{
    (void)ctx; (void)stage; (void)done; (void)total;
}

static DWORD WINAPI install_worker(void *arg)
{
    app_t *a = arg;
    WCHAR err[512];
    int rc;
    a->state = AST_INSTALLING;
    a->msg[0] = 0;
    if (g_wnd) { InvalidateRect(g_wnd, NULL, FALSE); dump(); }
    rc = app_install(a, progress_cb, NULL, &g_cancel, err, ARRAYSIZE(err));
    if (rc == 0) {
        a->state = AST_DONE;
        app_detect(a);              /* pick up the new installed version */
        if (a->state == AST_NOT_INSTALLED) a->state = AST_DONE;
        lstrcpynW(a->msg, L"Installed.", ARRAYSIZE(a->msg));
    } else {
        a->state = AST_FAILED;
        lstrcpynW(a->msg, err[0] ? err : L"The install failed.", ARRAYSIZE(a->msg));
    }
    g_busy = -1;
    if (g_wnd) { InvalidateRect(g_wnd, NULL, FALSE); dump(); }
    return 0;
}

static void start_install(int i)
{
    if (g_busy >= 0 || i < 0 || i >= g_napps) return;
    g_busy = i;
    CloseHandle(CreateThread(NULL, 0, install_worker, &g_apps[i], 0, NULL));
}

/* the card's main action: Install / Update / Open */
static void activate(int i)
{
    app_t *a;
    if (i < 0 || i >= g_napps) return;
    a = &g_apps[i];
    if (i == g_busy || a->state == AST_INSTALLING) return;
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
    int cardh = dpx(96), pad = dpx(16), badge = dpx(48);
    RECT card = { x, y, x + w, y + cardh }, r;
    BOOL sel = idx == g_sel;
    HBRUSH cb;
    HPEN pen, oldp;
    HBRUSH oldb;
    if (card.bottom < 0 || card.top > client_bottom) return y + cardh + dpx(12);   /* off screen */
    cb = CreateSolidBrush(C_CARD);
    pen = CreatePen(PS_SOLID, sel ? 2 : 1, sel ? C_SEL : C_LINE);
    oldp = SelectObject(dc, pen);
    oldb = SelectObject(dc, cb);
    RoundRect(dc, card.left, card.top, card.right, card.bottom, dpx(8), dpx(8));
    SelectObject(dc, oldb); SelectObject(dc, oldp);
    DeleteObject(cb); DeleteObject(pen);
    add_list_hit(H_CARD, idx, card, client_bottom);

    /* letter badge in the entry's colour (no logos) */
    {
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

    r.left = x + pad + badge + dpx(14); r.right = x + w - dpx(150); r.top = y + dpx(12); r.bottom = y + dpx(36);
    text(dc, g_f_head, C_TEXT, r, a->name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    r.top = r.bottom; r.bottom = y + dpx(54);
    {
        WCHAR sub[300];
        if (a->tier == TIER_LINUX) swprintf(sub, ARRAYSIZE(sub), L"%ls  \x00b7  Linux app", a->publisher);
        else lstrcpynW(sub, a->publisher, ARRAYSIZE(sub));
        text(dc, g_f_small, C_SUB, r, sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    r.top = r.bottom; r.bottom = y + cardh - dpx(8);
    if (a->state == AST_FAILED && a->msg[0])
        text(dc, g_f_small, C_ERR, r, a->msg, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
    else
        text(dc, g_f_body, C_SUB, r, a->desc, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);

    /* the action button, and state text */
    {
        RECT bt = { x + w - dpx(134), y + dpx(30), x + w - pad, y + dpx(30) + dpx(34) };
        if (idx == g_busy || a->state == AST_INSTALLING) {
            text(dc, g_f_body, C_SUB, bt, L"Installing...", DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else if (a->state == AST_UPDATE) {
            button(dc, bt, L"Update", TRUE); add_list_hit(H_UPDATE, idx, bt, client_bottom);
        } else if (a->state == AST_INSTALLED || a->state == AST_DONE) {
            button(dc, bt, L"Open", FALSE); add_list_hit(H_OPEN, idx, bt, client_bottom);
            { RECT s = { x + w - dpx(280), bt.top, x + w - dpx(144), bt.bottom };
              text(dc, g_f_small, C_OK, s, L"Installed", DT_RIGHT | DT_VCENTER | DT_SINGLELINE); }
        } else if (a->state == AST_FAILED) {
            button(dc, bt, L"Try again", TRUE); add_list_hit(H_INSTALL, idx, bt, client_bottom);
        } else {
            button(dc, bt, L"Install", TRUE); add_list_hit(H_INSTALL, idx, bt, client_bottom);
        }
    }
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
                     L"The store suggests Windows programs first; these are for when you prefer a Linux build.",
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
    { RECT sub = { x, dpx(134), x + w, dpx(154) };
      text(dc, g_f_small, C_SUB, sub, L"App and system updates are handled in Settings > Update & Security.", DT_LEFT | DT_VCENTER | DT_SINGLELINE); }

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
    if (cy < header_height()) {
        for (i = 0; i < g_nhits; i++)
            if ((g_hits[i].verb == H_CHECKALL || g_hits[i].verb == H_CAT || g_hits[i].verb == H_FROMFILE) && PtInRect(&g_hits[i].rc, pt))
                { *verb = g_hits[i].verb; *idx = g_hits[i].idx; return 1; }
        return 0;
    }
    for (i = 0; i < g_nhits; i++)       /* buttons before the card they sit on */
        if (g_hits[i].verb != H_CARD && PtInRect(&g_hits[i].rc, pt)) { *verb = g_hits[i].verb; *idx = g_hits[i].idx; return 1; }
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
        if (LOWORD(wp) != WA_INACTIVE && g_sel < 0) SetFocus(g_search);
        return 0;
    case WM_LBUTTONUP: {
        int verb, idx;
        if (hit_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &verb, &idx)) {
            if (verb == H_INSTALL || verb == H_UPDATE) { g_sel = idx; start_install(idx); }
            else if (verb == H_OPEN) { g_sel = idx; open_app(&g_apps[idx]); }
            else if (verb == H_CHECKALL) do_checkall();
            else if (verb == H_FROMFILE) install_from_file();
            else if (verb == H_CAT) set_category(hwnd, idx);
            else if (verb == H_CARD) { g_sel = idx; SetFocus(hwnd); InvalidateRect(hwnd, NULL, FALSE); }
        }
        return 0;
    }
    case WM_KEYDOWN:
        switch (wp) {
        case VK_DOWN: move_selection(hwnd, 1); return 0;
        case VK_UP: move_selection(hwnd, -1); return 0;
        case VK_NEXT: move_selection(hwnd, 5); return 0;
        case VK_PRIOR: move_selection(hwnd, -5); return 0;
        case VK_HOME: g_sel = -1; move_selection(hwnd, 1); return 0;
        case VK_END: g_sel = -1; move_selection(hwnd, -1); return 0;
        case VK_RETURN: case VK_SPACE: activate(g_sel); InvalidateRect(hwnd, NULL, FALSE); return 0;
        case VK_ESCAPE: case VK_TAB: SetFocus(g_search); SendMessageW(g_search, EM_SETSEL, 0, -1); dump(); return 0;
        }
        return 0;
    case WM_CHAR:
        /* typing in the list goes to the search box */
        if (wp >= L' ' && wp != 0x7f && GetFocus() == hwnd) {
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

    for (i = 1; argv && i < argc; i++) {
        if (!lstrcmpiW(argv[i], L"--list")) {
            load_all(FALSE); dump(); LocalFree(argv); return 0;
        }
        if (!lstrcmpiW(argv[i], L"--check-updates")) {
            load_all(TRUE); dump(); LocalFree(argv); return 0;
        }
        if (!lstrcmpiW(argv[i], L"--install-elevated") && i + 1 < argc) {
            int k;
            WCHAR err[512];
            load_all(FALSE);
            k = find_app(argv[i + 1]);
            rc = k < 0 ? 2 : app_install_elevated(&g_apps[k], err, ARRAYSIZE(err));
            LocalFree(argv);
            return rc;
        }
        if (!lstrcmpiW(argv[i], L"--deb") && i + 1 < argc) {
            rc = sys_deb_window(inst, argv[i + 1]);
            LocalFree(argv);
            return rc;
        }
        if (!lstrcmpiW(argv[i], L"--elevated-apt") || !lstrcmpiW(argv[i], L"--elevated-deb")) {
            rc = sys_elevated_main(argc, argv, i);
            LocalFree(argv);
            return rc;
        }
        if (!lstrcmpiW(argv[i], L"--search") && i + 1 < argc) { query = argv[++i]; continue; }
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
