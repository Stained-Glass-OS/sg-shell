/*
 * Stained Glass OS -- the Compatibility tab of a program's Properties
 * (right-click an .exe or its shortcut > Properties > Compatibility).
 *
 * A shell extension (IShellExtInit + IShellPropSheetExt) registered for
 * exefile and lnkfile. Every setting is stored where Windows or Wine already
 * looks, so it holds however the program starts -- a double-click, a
 * shortcut, a launcher such as Steam starting a game:
 *
 *   Compatibility mode   HKCU\Software\Wine\AppDefaults\<exe>\Version (Wine's
 *                        per-program version), and the Windows layer name in
 *                        AppCompatFlags\Layers\<full path>, which programs and
 *                        installers read as on Windows
 *   Run as administrator AppCompatFlags\Layers\<full path>: RUNASADMIN
 *                        (wine-sg's CreateProcess asks for consent)
 *   A window of its own  AppDefaults\<exe>\Explorer\Desktop + Explorer\Desktops
 *   Graphics             AppDefaults\<exe>\DllOverrides for d3d9..d3d12, dxgi:
 *                        DXVK/VKD3D-Proton (native) or WineD3D (builtin)
 *   Environment, args    AppDefaults\<exe>\Environment\NAME, \LaunchArgs
 *                        (applied by wine-sg's CreateProcess)
 *
 * Recommended settings come from /usr/share/stained-glass/compat-presets.ini,
 * our own list, one [program.exe] section each.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <prsht.h>
#include <shlwapi.h>
#include <stdio.h>
#include <wchar.h>
#include "resource.h"

/* {5C9B1F3E-7A2D-4E8B-9C61-3A7F2E4D8B10} */
DEFINE_GUID(CLSID_SgCompat, 0x5c9b1f3e, 0x7a2d, 0x4e8b, 0x9c, 0x61, 0x3a, 0x7f, 0x2e, 0x4d, 0x8b, 0x10);

static HINSTANCE g_inst;
static LONG g_locks, g_objects;

static const WCHAR APPDEF[] = L"Software\\Wine\\AppDefaults";
static const WCHAR LAYERS[] = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers";
static const WCHAR PRESETS_DEFAULT[] = L"Z:\\usr\\share\\stained-glass\\compat-presets.ini";
static WCHAR PRESETS[MAX_PATH];

/* SG_COMPAT_PRESETS (a Windows path) replaces the file, for tests. */
static void init_presets(void)
{
    if (!GetEnvironmentVariableW( L"SG_COMPAT_PRESETS", PRESETS, ARRAYSIZE(PRESETS) ) || !PRESETS[0])
        lstrcpyW( PRESETS, PRESETS_DEFAULT );
}

/* Windows versions: the name shown, Wine's Version value, the Layers name. */
static const struct { const WCHAR *label, *wine, *layer; } versions[] = {
    { L"Windows 11",             L"win11", L"WIN10RTM" },
    { L"Windows 10",             L"win10", L"WIN10RTM" },
    { L"Windows 8.1",            L"win81", L"WIN81RTM" },
    { L"Windows 8",              L"win8",  L"WIN8RTM" },
    { L"Windows 7",              L"win7",  L"WIN7RTM" },
    { L"Windows Vista (SP2)",    L"vista", L"VISTASP2" },
    { L"Windows XP (Service Pack 3)", L"winxp", L"WINXPSP3" },
};
static const WCHAR *gfx_labels[] = {
    L"Default (this computer's setting)",
    L"Vulkan (DXVK, VKD3D-Proton)",
    L"OpenGL (WineD3D)",
};
static const WCHAR *d3d_dlls[] = { L"d3d9", L"d3d10core", L"d3d11", L"dxgi", L"d3d12", L"d3d12core" };

struct compat
{
    IShellExtInit IShellExtInit_iface;
    IShellPropSheetExt IShellPropSheetExt_iface;
    LONG ref;
    WCHAR path[MAX_PATH];     /* the program's full path */
    WCHAR exe[MAX_PATH];      /* its file name: the AppDefaults key */
};

/* ---- settings ------------------------------------------------------------ */

struct settings
{
    int version;              /* index into versions[], -1 = off */
    BOOL admin, desktop;
    WCHAR size[32];
    int gfx;                  /* 0 default, 1 vulkan, 2 opengl */
    WCHAR env[64][512];
    int nenv;
    WCHAR args[1024];
};

static HKEY open_app( const WCHAR *exe, const WCHAR *sub, BOOL create )
{
    WCHAR path[MAX_PATH * 2];
    HKEY key = NULL;
    swprintf( path, ARRAYSIZE(path), L"%ls\\%ls%ls%ls", APPDEF, exe, sub ? L"\\" : L"", sub ? sub : L"" );
    if (create) RegCreateKeyExW( HKEY_CURRENT_USER, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL );
    else if (RegOpenKeyExW( HKEY_CURRENT_USER, path, 0, KEY_READ, &key )) key = NULL;
    return key;
}

static BOOL read_sz( HKEY key, const WCHAR *name, WCHAR *out, DWORD chars )
{
    DWORD size = (chars - 1) * sizeof(WCHAR), type;
    if (!key || RegQueryValueExW( key, name, NULL, &type, (BYTE *)out, &size ) || type != REG_SZ) return FALSE;
    out[size / sizeof(WCHAR)] = 0;
    return TRUE;
}

static void read_layers( const WCHAR *path, WCHAR *out, DWORD chars )
{
    HKEY key;
    out[0] = 0;
    if (!RegOpenKeyExW( HKEY_CURRENT_USER, LAYERS, 0, KEY_READ, &key ))
    {
        read_sz( key, path, out, chars );
        RegCloseKey( key );
    }
}

static BOOL has_token( const WCHAR *list, const WCHAR *tok )
{
    size_t n = wcslen( tok );
    const WCHAR *p = list;
    while ((p = StrStrIW( p, tok )))
    {
        if ((p == list || p[-1] == ' ') && (!p[n] || p[n] == ' ')) return TRUE;
        p += n;
    }
    return FALSE;
}

static void load_settings( const struct compat *c, struct settings *s )
{
    WCHAR buf[1024], layers[1024];
    HKEY key;
    DWORD i;

    memset( s, 0, sizeof(*s) );
    s->version = -1;
    lstrcpyW( s->size, L"1280x720" );

    if ((key = open_app( c->exe, NULL, FALSE )))
    {
        if (read_sz( key, L"Version", buf, ARRAYSIZE(buf) ))
            for (i = 0; i < ARRAYSIZE(versions); i++)
                if (!lstrcmpiW( buf, versions[i].wine )) s->version = i;
        read_sz( key, L"LaunchArgs", s->args, ARRAYSIZE(s->args) );
        RegCloseKey( key );
    }
    if ((key = open_app( c->exe, L"Explorer", FALSE )))
    {
        if (read_sz( key, L"Desktop", buf, ARRAYSIZE(buf) ))
        {
            HKEY d;
            s->desktop = TRUE;
            if (!RegOpenKeyExW( HKEY_CURRENT_USER, L"Software\\Wine\\Explorer\\Desktops", 0, KEY_READ, &d ))
            {
                read_sz( d, buf, s->size, ARRAYSIZE(s->size) );
                RegCloseKey( d );
            }
        }
        RegCloseKey( key );
    }
    if ((key = open_app( c->exe, L"DllOverrides", FALSE )))
    {
        if (read_sz( key, L"d3d11", buf, ARRAYSIZE(buf) ))
            s->gfx = !lstrcmpiW( buf, L"builtin" ) ? 2 : 1;
        RegCloseKey( key );
    }
    if ((key = open_app( c->exe, L"Environment", FALSE )))
    {
        WCHAR name[256], value[256];
        for (i = 0; s->nenv < (int)ARRAYSIZE(s->env); i++)
        {
            DWORD nl = ARRAYSIZE(name), vl = sizeof(value) - sizeof(WCHAR), type;
            if (RegEnumValueW( key, i, name, &nl, NULL, &type, (BYTE *)value, &vl )) break;
            if (type != REG_SZ) continue;
            value[vl / sizeof(WCHAR)] = 0;
            swprintf( s->env[s->nenv++], 512, L"%ls=%ls", name, value );
        }
        RegCloseKey( key );
    }
    read_layers( c->path, layers, ARRAYSIZE(layers) );
    s->admin = has_token( layers, L"RUNASADMIN" );
}

static void save_settings( const struct compat *c, const struct settings *s )
{
    WCHAR layers[1024] = L"";
    HKEY key;
    DWORD i;

    if ((key = open_app( c->exe, NULL, TRUE )))
    {
        if (s->version >= 0)
            RegSetValueExW( key, L"Version", 0, REG_SZ, (const BYTE *)versions[s->version].wine,
                            (wcslen( versions[s->version].wine ) + 1) * sizeof(WCHAR) );
        else RegDeleteValueW( key, L"Version" );
        if (s->args[0])
            RegSetValueExW( key, L"LaunchArgs", 0, REG_SZ, (const BYTE *)s->args, (wcslen( s->args ) + 1) * sizeof(WCHAR) );
        else RegDeleteValueW( key, L"LaunchArgs" );
        RegCloseKey( key );
    }

    if (s->desktop)
    {
        HKEY d;
        if ((key = open_app( c->exe, L"Explorer", TRUE )))
        {
            RegSetValueExW( key, L"Desktop", 0, REG_SZ, (const BYTE *)c->exe, (wcslen( c->exe ) + 1) * sizeof(WCHAR) );
            RegCloseKey( key );
        }
        if (!RegCreateKeyExW( HKEY_CURRENT_USER, L"Software\\Wine\\Explorer\\Desktops", 0, NULL, 0,
                              KEY_ALL_ACCESS, NULL, &d, NULL ))
        {
            RegSetValueExW( d, c->exe, 0, REG_SZ, (const BYTE *)s->size, (wcslen( s->size ) + 1) * sizeof(WCHAR) );
            RegCloseKey( d );
        }
    }
    else if ((key = open_app( c->exe, L"Explorer", FALSE )))
    {
        RegDeleteValueW( key, L"Desktop" );
        RegCloseKey( key );
    }

    if ((key = open_app( c->exe, L"DllOverrides", TRUE )))
    {
        for (i = 0; i < ARRAYSIZE(d3d_dlls); i++)
        {
            const WCHAR *v = s->gfx == 1 ? L"native,builtin" : L"builtin";
            if (s->gfx) RegSetValueExW( key, d3d_dlls[i], 0, REG_SZ, (const BYTE *)v, (wcslen( v ) + 1) * sizeof(WCHAR) );
            else RegDeleteValueW( key, d3d_dlls[i] );
        }
        RegCloseKey( key );
    }

    /* the environment is rewritten whole */
    {
        WCHAR path[MAX_PATH * 2];
        swprintf( path, ARRAYSIZE(path), L"%ls\\%ls\\Environment", APPDEF, c->exe );
        RegDeleteTreeW( HKEY_CURRENT_USER, path );
    }
    if (s->nenv && (key = open_app( c->exe, L"Environment", TRUE )))
    {
        for (i = 0; (int)i < s->nenv; i++)
        {
            WCHAR name[512], *eq;
            lstrcpynW( name, s->env[i], ARRAYSIZE(name) );
            if (!(eq = wcschr( name, '=' )) || eq == name) continue;
            *eq = 0;
            RegSetValueExW( key, name, 0, REG_SZ, (const BYTE *)(eq + 1), (wcslen( eq + 1 ) + 1) * sizeof(WCHAR) );
        }
        RegCloseKey( key );
    }

    /* Layers: "~ <version layer> RUNASADMIN", as Windows writes it */
    if (s->version >= 0 || s->admin)
    {
        lstrcpyW( layers, L"~" );
        if (s->version >= 0) { lstrcatW( layers, L" " ); lstrcatW( layers, versions[s->version].layer ); }
        if (s->admin) lstrcatW( layers, L" RUNASADMIN" );
    }
    if (!RegCreateKeyExW( HKEY_CURRENT_USER, LAYERS, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL ))
    {
        if (layers[0]) RegSetValueExW( key, c->path, 0, REG_SZ, (const BYTE *)layers, (wcslen( layers ) + 1) * sizeof(WCHAR) );
        else RegDeleteValueW( key, c->path );
        RegCloseKey( key );
    }
}

/* ---- recommended settings ------------------------------------------------- */

static BOOL load_preset( const struct compat *c, struct settings *s, WCHAR *note, DWORD chars )
{
    WCHAR buf[2048], *p, *next;
    DWORD i;

    if (!GetPrivateProfileStringW( c->exe, L"Note", L"", note, chars, PRESETS )) return FALSE;
    memset( s, 0, sizeof(*s) );
    s->version = -1;
    lstrcpyW( s->size, L"1280x720" );
    if (GetPrivateProfileStringW( c->exe, L"Version", L"", buf, ARRAYSIZE(buf), PRESETS ))
        for (i = 0; i < ARRAYSIZE(versions); i++) if (!lstrcmpiW( buf, versions[i].wine )) s->version = i;
    s->admin = GetPrivateProfileIntW( c->exe, L"Admin", 0, PRESETS );
    if (GetPrivateProfileStringW( c->exe, L"Desktop", L"", buf, ARRAYSIZE(buf), PRESETS ))
    {
        s->desktop = TRUE;
        lstrcpynW( s->size, buf, ARRAYSIZE(s->size) );
    }
    if (GetPrivateProfileStringW( c->exe, L"Graphics", L"", buf, ARRAYSIZE(buf), PRESETS ))
        s->gfx = !lstrcmpiW( buf, L"vulkan" ) ? 1 : !lstrcmpiW( buf, L"opengl" ) ? 2 : 0;
    GetPrivateProfileStringW( c->exe, L"Args", L"", s->args, ARRAYSIZE(s->args), PRESETS );
    if (GetPrivateProfileStringW( c->exe, L"Env", L"", buf, ARRAYSIZE(buf), PRESETS ))
        for (p = buf; p && *p && s->nenv < (int)ARRAYSIZE(s->env); p = next)
        {
            if ((next = wcschr( p, ';' ))) *next++ = 0;
            if (wcschr( p, '=' )) lstrcpynW( s->env[s->nenv++], p, 512 );
        }
    return TRUE;
}

/* ---- the page -------------------------------------------------------------- */

static void to_dialog( HWND dlg, const struct settings *s )
{
    int i;
    CheckDlgButton( dlg, IDC_VER_CHECK, s->version >= 0 ? BST_CHECKED : BST_UNCHECKED );
    SendDlgItemMessageW( dlg, IDC_VER_COMBO, CB_SETCURSEL, s->version >= 0 ? s->version : 4, 0 );
    EnableWindow( GetDlgItem( dlg, IDC_VER_COMBO ), s->version >= 0 );
    CheckDlgButton( dlg, IDC_ADMIN, s->admin ? BST_CHECKED : BST_UNCHECKED );
    CheckDlgButton( dlg, IDC_DESKTOP, s->desktop ? BST_CHECKED : BST_UNCHECKED );
    SetDlgItemTextW( dlg, IDC_DESKTOP_SIZE, s->size );
    EnableWindow( GetDlgItem( dlg, IDC_DESKTOP_SIZE ), s->desktop );
    SendDlgItemMessageW( dlg, IDC_GFX_COMBO, CB_SETCURSEL, s->gfx, 0 );
    SendDlgItemMessageW( dlg, IDC_ENV_LIST, LB_RESETCONTENT, 0, 0 );
    for (i = 0; i < s->nenv; i++) SendDlgItemMessageW( dlg, IDC_ENV_LIST, LB_ADDSTRING, 0, (LPARAM)s->env[i] );
    SetDlgItemTextW( dlg, IDC_ARGS, s->args );
}

static void from_dialog( HWND dlg, struct settings *s )
{
    int i, n;
    memset( s, 0, sizeof(*s) );
    s->version = IsDlgButtonChecked( dlg, IDC_VER_CHECK ) ? (int)SendDlgItemMessageW( dlg, IDC_VER_COMBO, CB_GETCURSEL, 0, 0 ) : -1;
    s->admin = IsDlgButtonChecked( dlg, IDC_ADMIN ) == BST_CHECKED;
    s->desktop = IsDlgButtonChecked( dlg, IDC_DESKTOP ) == BST_CHECKED;
    GetDlgItemTextW( dlg, IDC_DESKTOP_SIZE, s->size, ARRAYSIZE(s->size) );
    if (!s->size[0]) lstrcpyW( s->size, L"1280x720" );
    s->gfx = (int)SendDlgItemMessageW( dlg, IDC_GFX_COMBO, CB_GETCURSEL, 0, 0 );
    if (s->gfx < 0) s->gfx = 0;
    n = (int)SendDlgItemMessageW( dlg, IDC_ENV_LIST, LB_GETCOUNT, 0, 0 );
    for (i = 0; i < n && s->nenv < (int)ARRAYSIZE(s->env); i++)
        if (SendDlgItemMessageW( dlg, IDC_ENV_LIST, LB_GETTEXTLEN, i, 0 ) < 512)
            SendDlgItemMessageW( dlg, IDC_ENV_LIST, LB_GETTEXT, i, (LPARAM)s->env[s->nenv++] );
    GetDlgItemTextW( dlg, IDC_ARGS, s->args, ARRAYSIZE(s->args) );
}

static void changed( HWND dlg ) { PropSheet_Changed( GetParent( dlg ), dlg ); }

static INT_PTR CALLBACK page_proc( HWND dlg, UINT msg, WPARAM wp, LPARAM lp )
{
    struct compat *c = (struct compat *)GetWindowLongPtrW( dlg, DWLP_USER );
    struct settings s;
    WCHAR note[512], text[MAX_PATH + 64];
    DWORD i;

    switch (msg)
    {
    case WM_INITDIALOG:
        c = (struct compat *)((PROPSHEETPAGEW *)lp)->lParam;
        SetWindowLongPtrW( dlg, DWLP_USER, (LONG_PTR)c );
        swprintf( text, ARRAYSIZE(text), L"Program: %ls", c->path );
        SetDlgItemTextW( dlg, IDC_EXE_NAME, text );
        for (i = 0; i < ARRAYSIZE(versions); i++)
            SendDlgItemMessageW( dlg, IDC_VER_COMBO, CB_ADDSTRING, 0, (LPARAM)versions[i].label );
        for (i = 0; i < ARRAYSIZE(gfx_labels); i++)
            SendDlgItemMessageW( dlg, IDC_GFX_COMBO, CB_ADDSTRING, 0, (LPARAM)gfx_labels[i] );
        load_settings( c, &s );
        to_dialog( dlg, &s );
        if (load_preset( c, &s, note, ARRAYSIZE(note) )) SetDlgItemTextW( dlg, IDC_PRESET_TEXT, note );
        else
        {
            SetDlgItemTextW( dlg, IDC_PRESET_TEXT, L"No recommended settings for this program yet." );
            EnableWindow( GetDlgItem( dlg, IDC_PRESET_APPLY ), FALSE );
        }
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD( wp ))
        {
        case IDC_VER_CHECK:
            EnableWindow( GetDlgItem( dlg, IDC_VER_COMBO ), IsDlgButtonChecked( dlg, IDC_VER_CHECK ) == BST_CHECKED );
            changed( dlg );
            break;
        case IDC_DESKTOP:
            EnableWindow( GetDlgItem( dlg, IDC_DESKTOP_SIZE ), IsDlgButtonChecked( dlg, IDC_DESKTOP ) == BST_CHECKED );
            changed( dlg );
            break;
        case IDC_ADMIN:
            changed( dlg );
            break;
        case IDC_VER_COMBO: case IDC_GFX_COMBO:
            if (HIWORD( wp ) == CBN_SELCHANGE) changed( dlg );
            break;
        case IDC_DESKTOP_SIZE: case IDC_ARGS:
            if (HIWORD( wp ) == EN_CHANGE) changed( dlg );
            break;
        case IDC_ENV_ADD:
            GetDlgItemTextW( dlg, IDC_ENV_EDIT, text, ARRAYSIZE(text) );
            if (wcschr( text, '=' ) && text[0] != '=')
            {
                SendDlgItemMessageW( dlg, IDC_ENV_LIST, LB_ADDSTRING, 0, (LPARAM)text );
                SetDlgItemTextW( dlg, IDC_ENV_EDIT, L"" );
                changed( dlg );
            }
            else MessageBoxW( dlg, L"Type a variable as NAME=value, for example DXVK_ASYNC=1.",
                              L"Compatibility", MB_OK | MB_ICONINFORMATION );
            break;
        case IDC_ENV_REMOVE:
        {
            LRESULT sel = SendDlgItemMessageW( dlg, IDC_ENV_LIST, LB_GETCURSEL, 0, 0 );
            if (sel != LB_ERR) { SendDlgItemMessageW( dlg, IDC_ENV_LIST, LB_DELETESTRING, sel, 0 ); changed( dlg ); }
            break;
        }
        case IDC_PRESET_APPLY:
            if (load_preset( c, &s, note, ARRAYSIZE(note) )) { to_dialog( dlg, &s ); changed( dlg ); }
            break;
        }
        break;

    case WM_NOTIFY:
        if (((NMHDR *)lp)->code == PSN_APPLY)
        {
            from_dialog( dlg, &s );
            save_settings( c, &s );
            SetWindowLongPtrW( dlg, DWLP_MSGRESULT, PSNRET_NOERROR );
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ---- COM ------------------------------------------------------------------ */

static inline struct compat *from_init( IShellExtInit *i ) { return CONTAINING_RECORD( i, struct compat, IShellExtInit_iface ); }
static inline struct compat *from_sheet( IShellPropSheetExt *i ) { return CONTAINING_RECORD( i, struct compat, IShellPropSheetExt_iface ); }

static HRESULT WINAPI init_QueryInterface( IShellExtInit *iface, REFIID riid, void **out )
{
    struct compat *c = from_init( iface );
    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IShellExtInit )) *out = &c->IShellExtInit_iface;
    else if (IsEqualIID( riid, &IID_IShellPropSheetExt )) *out = &c->IShellPropSheetExt_iface;
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement( &c->ref );
    return S_OK;
}
static ULONG WINAPI init_AddRef( IShellExtInit *iface ) { return InterlockedIncrement( &from_init( iface )->ref ); }
static ULONG WINAPI init_Release( IShellExtInit *iface )
{
    struct compat *c = from_init( iface );
    ULONG r = InterlockedDecrement( &c->ref );
    if (!r) { HeapFree( GetProcessHeap(), 0, c ); InterlockedDecrement( &g_objects ); }
    return r;
}

/* The selected file: an .exe, or a shortcut's target if it is one. */
static HRESULT WINAPI init_Initialize( IShellExtInit *iface, PCIDLIST_ABSOLUTE folder, IDataObject *data, HKEY progid )
{
    struct compat *c = from_init( iface );
    FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM med;
    WCHAR file[MAX_PATH];
    const WCHAR *ext;
    HRESULT hr = E_FAIL;

    if (!data || FAILED(IDataObject_GetData( data, &fmt, &med ))) return E_FAIL;
    if (DragQueryFileW( med.hGlobal, (UINT)-1, NULL, 0 ) == 1 && DragQueryFileW( med.hGlobal, 0, file, MAX_PATH ))
    {
        ext = PathFindExtensionW( file );
        if (!lstrcmpiW( ext, L".lnk" ))
        {
            IShellLinkW *link;
            IPersistFile *pf;
            if (SUCCEEDED(CoCreateInstance( &CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link )))
            {
                if (SUCCEEDED(IShellLinkW_QueryInterface( link, &IID_IPersistFile, (void **)&pf )))
                {
                    if (SUCCEEDED(IPersistFile_Load( pf, file, STGM_READ )) &&
                        SUCCEEDED(IShellLinkW_GetPath( link, c->path, MAX_PATH, NULL, 0 )) && c->path[0])
                        hr = S_OK;
                    IPersistFile_Release( pf );
                }
                IShellLinkW_Release( link );
            }
            if (hr == S_OK && lstrcmpiW( PathFindExtensionW( c->path ), L".exe" )) hr = E_FAIL;
        }
        else if (!lstrcmpiW( ext, L".exe" ))
        {
            lstrcpynW( c->path, file, MAX_PATH );
            hr = S_OK;
        }
        if (hr == S_OK) lstrcpynW( c->exe, PathFindFileNameW( c->path ), MAX_PATH );
    }
    ReleaseStgMedium( &med );
    return hr;
}

static IShellExtInitVtbl init_vtbl = { init_QueryInterface, init_AddRef, init_Release, init_Initialize };

static HRESULT WINAPI sheet_QueryInterface( IShellPropSheetExt *iface, REFIID riid, void **out )
{ return init_QueryInterface( &from_sheet( iface )->IShellExtInit_iface, riid, out ); }
static ULONG WINAPI sheet_AddRef( IShellPropSheetExt *iface ) { return init_AddRef( &from_sheet( iface )->IShellExtInit_iface ); }
static ULONG WINAPI sheet_Release( IShellPropSheetExt *iface ) { return init_Release( &from_sheet( iface )->IShellExtInit_iface ); }

static UINT CALLBACK page_callback( HWND hwnd, UINT msg, PROPSHEETPAGEW *page )
{
    struct compat *c = (struct compat *)page->lParam;
    if (msg == PSPCB_ADDREF) IShellExtInit_AddRef( &c->IShellExtInit_iface );
    else if (msg == PSPCB_RELEASE) IShellExtInit_Release( &c->IShellExtInit_iface );
    return 1;
}

static HRESULT WINAPI sheet_AddPages( IShellPropSheetExt *iface, LPFNSVADDPROPSHEETPAGE add, LPARAM lparam )
{
    struct compat *c = from_sheet( iface );
    PROPSHEETPAGEW page;

    memset( &page, 0, sizeof(page) );
    page.dwSize = sizeof(page);
    HPROPSHEETPAGE h;

    page.dwFlags = PSP_USECALLBACK;
    page.hInstance = g_inst;
    page.pszTemplate = MAKEINTRESOURCEW( IDD_COMPAT );
    page.pfnDlgProc = page_proc;
    page.lParam = (LPARAM)c;
    page.pfnCallback = page_callback;
    if (!(h = CreatePropertySheetPageW( &page ))) return E_OUTOFMEMORY;
    if (!add( h, lparam )) { DestroyPropertySheetPage( h ); return E_FAIL; }
    return S_OK;
}
static HRESULT WINAPI sheet_ReplacePage( IShellPropSheetExt *iface, EXPPS id, LPFNSVADDPROPSHEETPAGE fn, LPARAM lp )
{ return E_NOTIMPL; }

static IShellPropSheetExtVtbl sheet_vtbl = { sheet_QueryInterface, sheet_AddRef, sheet_Release, sheet_AddPages, sheet_ReplacePage };

static HRESULT WINAPI cf_QueryInterface( IClassFactory *iface, REFIID riid, void **out )
{
    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IClassFactory )) { *out = iface; return S_OK; }
    *out = NULL;
    return E_NOINTERFACE;
}
static ULONG WINAPI cf_AddRef( IClassFactory *iface ) { return 2; }
static ULONG WINAPI cf_Release( IClassFactory *iface ) { return 1; }
static HRESULT WINAPI cf_CreateInstance( IClassFactory *iface, IUnknown *outer, REFIID riid, void **out )
{
    struct compat *c;
    HRESULT hr;
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!(c = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*c) ))) return E_OUTOFMEMORY;
    c->IShellExtInit_iface.lpVtbl = &init_vtbl;
    c->IShellPropSheetExt_iface.lpVtbl = &sheet_vtbl;
    c->ref = 1;
    InterlockedIncrement( &g_objects );
    hr = init_QueryInterface( &c->IShellExtInit_iface, riid, out );
    init_Release( &c->IShellExtInit_iface );
    return hr;
}
static HRESULT WINAPI cf_LockServer( IClassFactory *iface, BOOL lock )
{
    if (lock) InterlockedIncrement( &g_locks ); else InterlockedDecrement( &g_locks );
    return S_OK;
}
static IClassFactoryVtbl cf_vtbl = { cf_QueryInterface, cf_AddRef, cf_Release, cf_CreateInstance, cf_LockServer };
static IClassFactory factory = { &cf_vtbl };

HRESULT WINAPI DllGetClassObject( REFCLSID clsid, REFIID riid, void **out )
{
    if (!IsEqualCLSID( clsid, &CLSID_SgCompat )) { *out = NULL; return CLASS_E_CLASSNOTAVAILABLE; }
    return cf_QueryInterface( &factory, riid, out );
}

HRESULT WINAPI DllCanUnloadNow( void ) { return g_locks || g_objects ? S_FALSE : S_OK; }

BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, void *reserved )
{
    if (reason == DLL_PROCESS_ATTACH) { g_inst = inst; DisableThreadLibraryCalls( inst ); init_presets(); }
    return TRUE;
}
