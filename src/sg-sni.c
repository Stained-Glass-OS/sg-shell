/* sg-sni -- Linux programs' notification-area icons in the taskbar.
 *
 * A Linux program shows its tray icon as a StatusNotifierItem on the session
 * bus (Qt, Electron, libayatana-appindicator: Flameshot, KeePassXC,
 * Nextcloud, Telegram). sg-session's sg-sni-bridge is the session's
 * StatusNotifierWatcher and host: it reads each item's icon, tooltip and
 * menu and starts this program, whose standard input and output are its
 * pipes. Each item becomes an icon of ours (Shell_NotifyIcon, as
 * sg-volume's), and what the person does with it goes back:
 *
 *   in   ADD <id>\t<tooltip>\t<icon.png>    add an icon, or change it
 *        MENU <id>\t<items>                 its menu (see menu_build)
 *        DEL <id>                           take it away
 *        SHOWMENU <id>                      open its menu at the pointer (an item
 *                                           that is only a menu, or whose Activate failed)
 *        CLICK <id> left|middle|right       as if clicked (the gate)
 *        PICK <id> <n>                      the menu's n-th item chosen (the gate)
 *   out  READY                              the window is up
 *        ACT <id> <x> <y>                   left click: Activate
 *        SEC <id> <x> <y>                   middle click: SecondaryActivate
 *        CTX <id> <x> <y>                   right click with no menu: ContextMenu
 *        PICK <id> <item>                   a menu item chosen (dbusmenu id)
 *
 * The taskbar coming back (TaskbarCreated) gets every icon again. The end of
 * the input (the bridge gone) ends this program.
 *
 * SG_SNI_DUMP=<file> writes the icons and their menus after every change.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define _WIN32_WINNT 0x0601
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WM_TRAY (WM_APP + 1)
#define WM_LINE (WM_APP + 2)
#define WM_EOF  (WM_APP + 3)
#define MAX_ITEMS 64
#define MAX_MENU 256

struct menu_item { int depth; int id; WCHAR flags[8]; WCHAR label[128]; };

struct item
{
    char id[160];
    UINT uid;
    HICON icon;
    WCHAR tip[128];
    struct menu_item menu[MAX_MENU];
    int nmenu;
    BOOL shown;
};

static struct item g_items[MAX_ITEMS];
static HWND g_wnd;
static UINT g_taskbar_created;
static UINT g_next_uid = 1;
static HANDLE g_out;

static void out(const char *fmt, ...)
{
    char buf[512];
    DWORD done;
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
    buf[n++] = '\n';
    WriteFile(g_out, buf, n, &done, NULL);
}

static struct item *find(const char *id)
{
    int i;
    for (i = 0; i < MAX_ITEMS; i++)
        if (g_items[i].uid && !strcmp(g_items[i].id, id)) return &g_items[i];
    return NULL;
}

static void dump(void)
{
    WCHAR path[MAX_PATH];
    FILE *f;
    int i, j;
    if (!GetEnvironmentVariableW(L"SG_SNI_DUMP", path, MAX_PATH)) return;
    if (!(f = _wfopen(path, L"wb"))) return;
    for (i = 0; i < MAX_ITEMS; i++)
    {
        struct item *it = &g_items[i];
        ICONINFO ii;
        BITMAP bm = { 0 };
        if (!it->uid) continue;
        if (it->icon && GetIconInfo(it->icon, &ii))
        {
            GetObjectW(ii.hbmColor ? ii.hbmColor : ii.hbmMask, sizeof(bm), &bm);
            if (ii.hbmColor) DeleteObject(ii.hbmColor);
            DeleteObject(ii.hbmMask);
        }
        fprintf(f, "item %s shown=%d icon=%ldx%ld tip=%ls\n", it->id, it->shown, bm.bmWidth, bm.bmHeight, it->tip);
        for (j = 0; j < it->nmenu; j++)
            fprintf(f, "menu %s %d %d [%ls] %ls\n", it->id, it->menu[j].depth, it->menu[j].id, it->menu[j].flags, it->menu[j].label);
    }
    fclose(f);
}

/* the PNG the bridge wrote, as an icon of the tray's size (WIC) */
static HICON load_icon(const WCHAR *path)
{
    IWICImagingFactory *fac = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICBitmapScaler *scaler = NULL;
    IWICFormatConverter *conv = NULL;
    HICON icon = NULL;
    int cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
    UINT w = 0, h = 0;

    if (!path[0]) return NULL;
    if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&fac))) return NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(fac, path, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) &&
        SUCCEEDED(IWICBitmapFrameDecode_GetSize(frame, &w, &h)) && w && h &&
        SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(fac, &scaler)) &&
        SUCCEEDED(IWICBitmapScaler_Initialize(scaler, (IWICBitmapSource *)frame, cx, cy, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(fac, &conv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)scaler, &GUID_WICPixelFormat32bppBGRA,
                                                 WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom)))
    {
        BITMAPV5HEADER bi = { sizeof(bi) };
        void *bits = NULL;
        HBITMAP color, mask;
        bi.bV5Width = cx;
        bi.bV5Height = -cy;
        bi.bV5Planes = 1;
        bi.bV5BitCount = 32;
        bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask = 0x00FF0000;
        bi.bV5GreenMask = 0x0000FF00;
        bi.bV5BlueMask = 0x000000FF;
        bi.bV5AlphaMask = 0xFF000000;
        color = CreateDIBSection(NULL, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
        mask = CreateBitmap(cx, cy, 1, 1, NULL);
        if (color && mask && SUCCEEDED(IWICFormatConverter_CopyPixels(conv, NULL, cx * 4, cx * cy * 4, bits)))
        {
            ICONINFO ii = { TRUE, 0, 0, mask, color };
            icon = CreateIconIndirect(&ii);
        }
        if (color) DeleteObject(color);
        if (mask) DeleteObject(mask);
    }
    if (conv) IWICFormatConverter_Release(conv);
    if (scaler) IWICBitmapScaler_Release(scaler);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    IWICImagingFactory_Release(fac);
    return icon;
}

static void show(struct item *it, BOOL add)
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_wnd;
    nid.uID = it->uid;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = it->icon ? it->icon : LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    lstrcpynW(nid.szTip, it->tip, ARRAYSIZE(nid.szTip));
    if (add || !it->shown) it->shown = Shell_NotifyIconW(NIM_ADD, &nid);
    else if (!Shell_NotifyIconW(NIM_MODIFY, &nid)) it->shown = Shell_NotifyIconW(NIM_ADD, &nid);
}

static void hide(struct item *it)
{
    NOTIFYICONDATAW nid = { sizeof(nid) };
    nid.hWnd = g_wnd;
    nid.uID = it->uid;
    if (it->shown) Shell_NotifyIconW(NIM_DELETE, &nid);
    if (it->icon) DestroyIcon(it->icon);
    memset(it, 0, sizeof(*it));
}

static void to_wide(const char *s, WCHAR *w, int cch)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, cch)) w[0] = 0;
    w[cch - 1] = 0;
}

/* the bridge's menu: items separated by \x1e, each depth\x1fid\x1fflags\x1flabel;
 * flags: d disabled, s separator, c checked, r radio, m opens a submenu */
static void menu_parse(struct item *it, char *spec)
{
    char *rec, *next;
    it->nmenu = 0;
    for (rec = spec; rec && *rec && it->nmenu < MAX_MENU; rec = next)
    {
        char *f[4] = { 0 };
        char *p = rec;
        int k;
        struct menu_item *m = &it->menu[it->nmenu];
        if ((next = strchr(rec, '\x1e'))) *next++ = 0;
        for (k = 0; k < 4 && p; k++)
        {
            f[k] = p;
            if ((p = strchr(p, '\x1f'))) *p++ = 0;
        }
        if (!f[3]) continue;
        m->depth = atoi(f[0]);
        m->id = atoi(f[1]);
        to_wide(f[2], m->flags, ARRAYSIZE(m->flags));
        to_wide(f[3], m->label, ARRAYSIZE(m->label));
        it->nmenu++;
    }
}

/* items [*pos, ...) at depth into menu; returns the next index */
static int menu_build(struct item *it, HMENU menu, int pos, int depth)
{
    while (pos < it->nmenu && it->menu[pos].depth >= depth)
    {
        struct menu_item *m = &it->menu[pos];
        UINT flags = MF_STRING;
        if (m->depth > depth) { pos++; continue; }   /* a stray deeper item */
        if (wcschr(m->flags, 's')) { AppendMenuW(menu, MF_SEPARATOR, 0, NULL); pos++; continue; }
        if (wcschr(m->flags, 'd')) flags |= MF_GRAYED;
        if (wcschr(m->flags, 'c')) flags |= MF_CHECKED;
        if (wcschr(m->flags, 'm'))
        {
            HMENU sub = CreatePopupMenu();
            int next = menu_build(it, sub, pos + 1, depth + 1);
            AppendMenuW(menu, flags | MF_POPUP, (UINT_PTR)sub, m->label);
            pos = next;
            continue;
        }
        AppendMenuW(menu, flags, pos + 1, m->label);   /* the command is the item's index + 1 */
        if (wcschr(m->flags, 'r') && wcschr(m->flags, 'c'))
            CheckMenuRadioItem(menu, pos + 1, pos + 1, pos + 1, MF_BYCOMMAND);
        pos++;
    }
    return pos;
}

static void open_menu(struct item *it)
{
    POINT p;
    HMENU menu;
    int cmd;
    if (!it->nmenu) return;
    GetCursorPos(&p);
    menu = CreatePopupMenu();
    menu_build(it, menu, 0, 0);
    SetForegroundWindow(g_wnd);
    cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, p.x, p.y, 0, g_wnd, NULL);
    DestroyMenu(menu);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    if (cmd > 0 && cmd <= it->nmenu) out("PICK %s %d", it->id, it->menu[cmd - 1].id);
}

static void clicked(struct item *it, UINT what)
{
    POINT p;
    GetCursorPos(&p);
    if (what == WM_LBUTTONUP) out("ACT %s %ld %ld", it->id, p.x, p.y);
    else if (what == WM_MBUTTONUP) out("SEC %s %ld %ld", it->id, p.x, p.y);
    else if (!it->nmenu) out("CTX %s %ld %ld", it->id, p.x, p.y);
    else open_menu(it);
}

static void line(char *l)
{
    char *arg = strchr(l, ' ');
    struct item *it;
    if (arg) *arg++ = 0; else arg = l + strlen(l);
    if (!strcmp(l, "ADD"))
    {
        char *tip = strchr(arg, '\t'), *png = NULL;
        WCHAR wpng[MAX_PATH];
        int i;
        if (tip) { *tip++ = 0; if ((png = strchr(tip, '\t'))) *png++ = 0; }
        if (!(it = find(arg)))
        {
            for (i = 0; i < MAX_ITEMS && g_items[i].uid; i++);
            if (i == MAX_ITEMS) return;
            it = &g_items[i];
            lstrcpynA(it->id, arg, sizeof(it->id));
            it->uid = g_next_uid++;
        }
        to_wide(tip ? tip : "", it->tip, ARRAYSIZE(it->tip));
        to_wide(png ? png : "", wpng, ARRAYSIZE(wpng));
        if (wpng[0])
        {
            HICON icon = load_icon(wpng);
            if (icon) { if (it->icon) DestroyIcon(it->icon); it->icon = icon; }
        }
        show(it, FALSE);
    }
    else if (!strcmp(l, "MENU"))
    {
        char *spec = strchr(arg, '\t');
        if (spec) *spec++ = 0;
        if ((it = find(arg)))
        {
            menu_parse(it, spec ? spec : "");
#ifdef SG_MUTANT_SNI_NO_MENU
            it->nmenu = 0;
#endif
        }
    }
    else if (!strcmp(l, "DEL")) { if ((it = find(arg))) hide(it); }
    else if (!strcmp(l, "SHOWMENU")) { if ((it = find(arg))) open_menu(it); }
    else if (!strcmp(l, "CLICK"))
    {
        char *how = strchr(arg, ' ');
        if (how) *how++ = 0;
        if ((it = find(arg)) && how)
        {
            if (!strcmp(how, "left")) clicked(it, WM_LBUTTONUP);
            else if (!strcmp(how, "middle")) clicked(it, WM_MBUTTONUP);
            else if (!it->nmenu) clicked(it, WM_RBUTTONUP);
        }
    }
    else if (!strcmp(l, "PICK"))
    {
        char *n = strchr(arg, ' ');
        int k;
        if (n) *n++ = 0;
        if ((it = find(arg)) && n && (k = atoi(n)) >= 0 && k < it->nmenu)
            out("PICK %s %d", it->id, it->menu[k].id);
    }
    dump();
}

static DWORD WINAPI reader(void *arg)
{
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    static char buf[65536];
    size_t have = 0;
    DWORD got;
    (void)arg;
    while (ReadFile(in, buf + have, sizeof(buf) - 1 - have, &got, NULL) && got)
    {
        char *start = buf, *nl;
        have += got;
        buf[have] = 0;
        while ((nl = memchr(start, '\n', have - (start - buf))))
        {
            size_t n = nl - start;
            char *copy = malloc(n + 1);
            if (n && start[n - 1] == '\r') n--;
            if (copy)
            {
                memcpy(copy, start, n);
                copy[n] = 0;
                PostMessageW(g_wnd, WM_LINE, 0, (LPARAM)copy);
            }
            start = nl + 1;
        }
        have -= start - buf;
        memmove(buf, start, have);
        if (have >= sizeof(buf) - 1) have = 0;   /* a line too long for us: dropped */
    }
    PostMessageW(g_wnd, WM_EOF, 0, 0);
    return 0;
}

static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int i;
    if (msg == g_taskbar_created && msg)
    {
        for (i = 0; i < MAX_ITEMS; i++) if (g_items[i].uid) show(&g_items[i], TRUE);
        dump();
        return 0;
    }
    switch (msg)
    {
    case WM_LINE:
        line((char *)lp);
        free((char *)lp);
        return 0;
    case WM_TRAY:
        for (i = 0; i < MAX_ITEMS; i++)
            if (g_items[i].uid == wp)
            {
                if (lp == WM_LBUTTONUP || lp == WM_MBUTTONUP || lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU)
                    clicked(&g_items[i], lp == WM_CONTEXTMENU ? WM_RBUTTONUP : (UINT)lp);
                break;
            }
        return 0;
    case WM_EOF:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        for (i = 0; i < MAX_ITEMS; i++) if (g_items[i].uid) hide(&g_items[i]);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show_cmd)
{
    WNDCLASSW wc = { 0 };
    MSG msg;
    (void)prev; (void)cmd; (void)show_cmd;
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"SgSniHost";
    RegisterClassW(&wc);
    g_wnd = CreateWindowW(L"SgSniHost", L"Linux notification icons", 0, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    if (!g_wnd) return 1;
    CloseHandle(CreateThread(NULL, 0, reader, NULL, 0, NULL));
    out("READY");
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}
