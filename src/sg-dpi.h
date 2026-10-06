/* sg-dpi.h -- our programs at the display scale, and at a new one while they
 * run (David 2026-10-06: "top of the line DPI scaling across the board").
 *
 * A program calls sg_dpi_init() first thing: it is per-monitor v2 aware, so
 * it draws itself at the display scale (not Wine's scaled picture of a 100%
 * window, soft at 175%), and when Settings > Display > Scale changes while
 * it runs it hears WM_DPICHANGED (wine-sg 0890) and lays itself out again;
 * Wine draws its menus, message boxes and dialogs at the new scale too
 * (wine-sg 1121). Where per-monitor awareness is not there, it is aware of
 * the system DPI, as before.
 *
 * On WM_DPICHANGED a program sets its DPI (HIWORD(wParam)), makes its fonts
 * again, hands them to its controls (sg_dpi_refont), moves controls it placed
 * once (sg_dpi_scale_children), takes the suggested rectangle
 * (sg_dpi_apply_rect), lays itself out and repaints.
 *
 * SG_MUTANT_DPI_SYSTEM_ONLY: aware of the system DPI only, as before (the
 * hidpi-live-check gate fails: no WM_DPICHANGED, Wine scales the picture).
 */
#ifndef SG_DPI_H
#define SG_DPI_H

#include <windows.h>

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

/* per-monitor v2, or aware of the system DPI */
static inline void sg_dpi_init(void)
{
#ifndef SG_MUTANT_DPI_SYSTEM_ONLY
    typedef BOOL (WINAPI *ctx_fn)(HANDLE);
    ctx_fn set_ctx = (ctx_fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
    if (set_ctx && set_ctx((HANDLE)-4 /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */)) return;
#endif
    SetProcessDPIAware();
}

/* the DPI HWND is drawn at (the screen's when there is no window yet) */
static inline UINT sg_dpi_for(HWND hwnd)
{
    typedef UINT (WINAPI *dpi_fn)(HWND);
    dpi_fn for_window = (dpi_fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT dpi = hwnd && for_window ? for_window(hwnd) : 0;
    if (!dpi)
    {
        HDC dc = GetDC(NULL);
        dpi = GetDeviceCaps(dc, LOGPIXELSY);
        ReleaseDC(NULL, dc);
    }
    return dpi < 96 ? 96 : dpi;
}

/* WM_DPICHANGED's DPI */
static inline UINT sg_dpi_new(WPARAM wp)
{
    UINT dpi = HIWORD(wp);
    return dpi < 96 ? 96 : dpi;
}

/* the window at the rectangle WM_DPICHANGED suggests (LPARAM) */
static inline void sg_dpi_apply_rect(HWND hwnd, LPARAM lp)
{
    const RECT *r = (const RECT *)lp;
    if (r) SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                        SWP_NOZORDER | SWP_NOACTIVATE);
}

/* a font as large at TO as FONT is at FROM (its owner deletes it) */
static inline HFONT sg_dpi_scale_font(HFONT font, UINT from, UINT to)
{
    LOGFONTW lf;
    if (!font || !GetObjectW(font, sizeof(lf), &lf)) return NULL;
    lf.lfHeight = MulDiv(lf.lfHeight, (int)to, (int)from);
    lf.lfWidth = MulDiv(lf.lfWidth, (int)to, (int)from);
    return CreateFontIndirectW(&lf);
}

struct sg_dpi_refont_ctx { const HFONT *old_fonts, *new_fonts; int n; };

static BOOL CALLBACK sg_dpi_refont_proc(HWND child, LPARAM lp)
{
    const struct sg_dpi_refont_ctx *ctx = (const struct sg_dpi_refont_ctx *)lp;
    HFONT font = (HFONT)SendMessageW(child, WM_GETFONT, 0, 0);
    int i;
    for (i = 0; font && i < ctx->n; i++)
        if (font == ctx->old_fonts[i] && ctx->new_fonts[i])
        {
            SendMessageW(child, WM_SETFONT, (WPARAM)ctx->new_fonts[i], TRUE);
            break;
        }
    return TRUE;
}

/* every window under PARENT whose font is OLD_FONTS[i] gets NEW_FONTS[i]
 * (made again at the new DPI; the program deletes the old ones after) */
static inline void sg_dpi_refont(HWND parent, const HFONT *old_fonts, const HFONT *new_fonts, int n)
{
    struct sg_dpi_refont_ctx ctx;
    ctx.old_fonts = old_fonts;
    ctx.new_fonts = new_fonts;
    ctx.n = n;
    if (parent) EnumChildWindows(parent, sg_dpi_refont_proc, (LPARAM)&ctx);
}

/* PARENT's own children (not theirs: a control places its parts itself)
 * moved and sized for TO, as they were for FROM -- for controls a program
 * placed once; one that lays itself out on WM_SIZE does that instead */
static inline void sg_dpi_scale_children(HWND parent, UINT from, UINT to)
{
    HWND child;
    if (!parent || !from || from == to) return;
    for (child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
    {
        RECT r;
        GetWindowRect(child, &r);
        MapWindowPoints(NULL, parent, (POINT *)&r, 2);
        SetWindowPos(child, NULL, MulDiv(r.left, (int)to, (int)from), MulDiv(r.top, (int)to, (int)from),
                     MulDiv(r.right - r.left, (int)to, (int)from), MulDiv(r.bottom - r.top, (int)to, (int)from),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/* a list view's columns as wide at TO as they were at FROM */
static inline void sg_dpi_scale_columns(HWND list, UINT from, UINT to)
{
    /* LVM_GETHEADER, HDM_GETITEMCOUNT, LVM_GETCOLUMNWIDTH, LVM_SETCOLUMNWIDTH
     * (commctrl.h's, without needing it here) */
    HWND header = (HWND)SendMessageW(list, 0x1000 + 31, 0, 0);
    int i, n = header ? (int)SendMessageW(header, 0x1200, 0, 0) : 0;
    if (!from || from == to) return;
    for (i = 0; i < n; i++)
        SendMessageW(list, 0x1000 + 30, i, MulDiv((int)SendMessageW(list, 0x1000 + 29, i, 0), (int)to, (int)from));
}

struct sg_dpi_columns_ctx { UINT from, to; };

static BOOL CALLBACK sg_dpi_columns_proc(HWND child, LPARAM lp)
{
    const struct sg_dpi_columns_ctx *ctx = (const struct sg_dpi_columns_ctx *)lp;
    WCHAR cls[32];
    if (GetClassNameW(child, cls, 32) && !lstrcmpiW(cls, L"SysListView32")) sg_dpi_scale_columns(child, ctx->from, ctx->to);
    return TRUE;
}

/* every list view under PARENT: its columns at the new scale */
static inline void sg_dpi_scale_all_columns(HWND parent, UINT from, UINT to)
{
    struct sg_dpi_columns_ctx ctx;
    ctx.from = from;
    ctx.to = to;
    if (parent && from && from != to) EnumChildWindows(parent, sg_dpi_columns_proc, (LPARAM)&ctx);
}

#endif /* SG_DPI_H */
