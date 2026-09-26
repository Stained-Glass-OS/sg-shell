/* The Compatibility tab (sgcompat.dll), for test/compat-check.sh:
 *   compat-probe tabs PATH     -- open PATH's Properties (Wine's own dialog),
 *                                 print "TAB <name>" for each tab, close it
 *   compat-probe set PATH      -- host the tab, set every setting as a person
 *                                 would, press Apply
 *   compat-probe preset PATH   -- host the tab, press Apply (recommended), Apply
 * SPDX-License-Identifier: AGPL-3.0-or-later */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <prsht.h>
#include <commctrl.h>
#include <stdio.h>
#include "../src/compat/resource.h"

DEFINE_GUID(SG_BHID_SFUIObject, 0x3981e225, 0xf559, 0x11d3, 0x8e, 0x3a, 0x00, 0xc0, 0x4f, 0x68, 0x37, 0xd5);
DEFINE_GUID(SG_BHID_DataObject, 0xb8c0bd9f, 0xed24, 0x455c, 0x83, 0xe6, 0xd5, 0x39, 0x0c, 0x4f, 0xe8, 0xc4);
DEFINE_GUID(CLSID_SgCompat, 0x5c9b1f3e, 0x7a2d, 0x4e8b, 0x9c, 0x61, 0x3a, 0x7f, 0x2e, 0x4d, 0x8b, 0x10);

static HPROPSHEETPAGE pages[4];
static UINT npages;
static BOOL CALLBACK add_page( HPROPSHEETPAGE h, LPARAM lp ) { if (npages < 4) pages[npages++] = h; return TRUE; }

/* What right-click > Properties does in File Explorer: the item's context
 * menu, verb "properties" (Wine's SHObjectProperties is a stub). */
static DWORD WINAPI props_thread( void *path )
{
    IShellItem *item;
    IContextMenu *menu;
    CMINVOKECOMMANDINFO ici = { sizeof(ici) };
    MSG msg;
    CoInitialize( NULL );
    if (FAILED(SHCreateItemFromParsingName( path, NULL, &IID_IShellItem, (void **)&item )) ||
        FAILED(IShellItem_BindToHandler( item, NULL, &SG_BHID_SFUIObject, &IID_IContextMenu, (void **)&menu )))
    { printf( "NOMENU\n" ); fflush( stdout ); return 1; }
    ici.lpVerb = "properties";
    ici.nShow = SW_SHOWNORMAL;
    if (FAILED(IContextMenu_InvokeCommand( menu, &ici ))) { printf( "INVOKEFAILED\n" ); fflush( stdout ); }
    while (GetMessageW( &msg, NULL, 0, 0 )) { TranslateMessage( &msg ); DispatchMessageW( &msg ); }
    return 0;
}

static BOOL CALLBACK find_tab( HWND hwnd, LPARAM lp )
{
    WCHAR cls[64];
    GetClassNameW( hwnd, cls, 64 );
    if (!lstrcmpiW( cls, L"SysTabControl32" )) { *(HWND *)lp = hwnd; return FALSE; }
    return TRUE;
}

static int tabs( const WCHAR *path )
{
    HWND sheet = NULL, tab = NULL;
    WCHAR title[MAX_PATH + 32];
    int i, n;
    CreateThread( NULL, 0, props_thread, (void *)path, 0, NULL );
    for (i = 0; i < 200 && !sheet; i++)
    {
        HWND w = NULL;
        Sleep( 100 );
        while ((w = FindWindowExW( NULL, w, NULL, NULL )))
        {
            GetWindowTextW( w, title, ARRAYSIZE(title) );
            if (wcsstr( title, L"Properties" ) && IsWindowVisible( w )) { sheet = w; break; }
        }
    }
    if (!sheet)
    {
        HWND w = NULL;
        printf( "NOSHEET\n" );
        while ((w = FindWindowExW( NULL, w, NULL, NULL )))
        {
            GetWindowTextW( w, title, ARRAYSIZE(title) );
            if (title[0]) printf( "WINDOW %ls (visible %d)\n", title, IsWindowVisible( w ) );
        }
        return 1;
    }
    EnumChildWindows( sheet, find_tab, (LPARAM)&tab );
    n = tab ? (int)SendMessageW( tab, TCM_GETITEMCOUNT, 0, 0 ) : 0;
    for (i = 0; i < n; i++)
    {
        WCHAR name[64] = L"";
        TCITEMW item = { TCIF_TEXT };
        item.pszText = name; item.cchTextMax = 64;
        SendMessageW( tab, TCM_GETITEMW, i, (LPARAM)&item );
        printf( "TAB %ls\n", name );
    }
    fflush( stdout );
    PostMessageW( sheet, WM_CLOSE, 0, 0 );
    Sleep( 500 );
    return 0;
}

static void click( HWND page, int id ) { SendMessageW( page, WM_COMMAND, MAKEWPARAM( id, BN_CLICKED ), (LPARAM)GetDlgItem( page, id ) ); }
static void pick( HWND page, int id, int sel )
{
    SendDlgItemMessageW( page, id, CB_SETCURSEL, sel, 0 );
    SendMessageW( page, WM_COMMAND, MAKEWPARAM( id, CBN_SELCHANGE ), (LPARAM)GetDlgItem( page, id ) );
}

static int host( const WCHAR *path, BOOL preset )
{
    IShellExtInit *init;
    IShellPropSheetExt *sheet_ext;
    IShellItem *item;
    IDataObject *data;
    PROPSHEETHEADERW psh = { sizeof(psh) };
    HWND sheet, page;
    MSG msg;
    WCHAR text[512];
    HRESULT hr;

    if (FAILED(hr = CoCreateInstance( &CLSID_SgCompat, NULL, CLSCTX_INPROC_SERVER, &IID_IShellExtInit, (void **)&init )))
    { printf( "NOCLASS %08lx\n", hr ); return 1; }
    if (FAILED(SHCreateItemFromParsingName( path, NULL, &IID_IShellItem, (void **)&item )) ||
        FAILED(IShellItem_BindToHandler( item, NULL, &SG_BHID_DataObject, &IID_IDataObject, (void **)&data )))
    { printf( "NODATA\n" ); return 1; }
    if (FAILED(hr = IShellExtInit_Initialize( init, NULL, data, NULL ))) { printf( "INIT %08lx\n", hr ); return 1; }
    IShellExtInit_QueryInterface( init, &IID_IShellPropSheetExt, (void **)&sheet_ext );
    IShellPropSheetExt_AddPages( sheet_ext, add_page, 0 );
    if (!npages) { printf( "NOPAGE\n" ); return 1; }
    psh.dwFlags = PSH_MODELESS | PSH_NOAPPLYNOW;
    psh.pszCaption = L"compat-probe";
    psh.nPages = npages;
    psh.phpage = pages;
    sheet = (HWND)PropertySheetW( &psh );
    page = PropSheet_GetCurrentPageHwnd( sheet );
    GetDlgItemTextW( page, IDC_PRESET_TEXT, text, ARRAYSIZE(text) );
    printf( "PRESET %ls\n", text );
    if (preset) click( page, IDC_PRESET_APPLY );
    else
    {
        CheckDlgButton( page, IDC_VER_CHECK, BST_CHECKED ); click( page, IDC_VER_CHECK );
        pick( page, IDC_VER_COMBO, 4 );                      /* Windows 7 */
        CheckDlgButton( page, IDC_ADMIN, BST_CHECKED ); click( page, IDC_ADMIN );
        CheckDlgButton( page, IDC_DESKTOP, BST_CHECKED ); click( page, IDC_DESKTOP );
        SetDlgItemTextW( page, IDC_DESKTOP_SIZE, L"1024x768" );
        pick( page, IDC_GFX_COMBO, 2 );                      /* OpenGL (WineD3D) */
        SetDlgItemTextW( page, IDC_ENV_EDIT, L"SGTEST_COMPAT=from the tab" ); click( page, IDC_ENV_ADD );
        SetDlgItemTextW( page, IDC_ENV_EDIT, L"DXVK_HUD=fps" ); click( page, IDC_ENV_ADD );
        SetDlgItemTextW( page, IDC_ARGS, L"-dx11 -windowed" );
    }
    if (GetEnvironmentVariableW( L"SG_COMPAT_SHOT", text, 8 ))  /* hold for a screenshot */
    {
        DWORD end = GetTickCount() + 4000;
        while (GetTickCount() < end)
            while (PeekMessageW( &msg, NULL, 0, 0, PM_REMOVE )) { TranslateMessage( &msg ); DispatchMessageW( &msg ); }
    }
    PropSheet_Apply( sheet );
    printf( "APPLIED\n" );
    fflush( stdout );
    DestroyWindow( sheet );
    return 0;
}

int wmain( int argc, WCHAR **argv )
{
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES };
    if (argc < 3) return 2;
    InitCommonControlsEx( &icc );
    CoInitialize( NULL );
    if (!lstrcmpW( argv[1], L"tabs" )) return tabs( argv[2] );
    if (!lstrcmpW( argv[1], L"set" )) return host( argv[2], FALSE );
    if (!lstrcmpW( argv[1], L"preset" )) return host( argv[2], TRUE );
    return 2;
}
