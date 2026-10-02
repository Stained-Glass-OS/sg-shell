/* sglinuxicon64.dll -- a Linux app's .desktop file shows the app's icon.
 *
 * The icon handler (shellex\IconHandler) of SG.LinuxApp, the .desktop
 * files' type: the shell asks it for each file's icon (wine-sg 0767). Steam
 * puts steam.desktop on the desktop at its first start; it showed the
 * type's icon -- the Store's -- not Steam's (David 2026-10-02).
 *
 * The icon is the one sg-linuxapp made for the app (sync_apps: the theme's
 * PNGs, or its SVG, as an .ico) in %LOCALAPPDATA%\Stained Glass\Linux app
 * icons: named after the app's id (the .desktop file's name), or after its
 * Icon= name. None made: the type's icon, as before.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 David Hamner and the Stained Glass OS contributors */
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <shlobj.h>
#include <shlguid.h>
#include <shlwapi.h>
#include <stdio.h>

/* {0FADADA1-718E-488D-A815-66A920A012EF} */
DEFINE_GUID(CLSID_SgLinuxIcon, 0x0fadada1, 0x718e, 0x488d, 0xa8, 0x15, 0x66, 0xa9, 0x20, 0xa0, 0x12, 0xef);

static LONG g_objects, g_locks;

/* IExtractIconW as COM lays it out: mingw's C declaration leaves IUnknown's
 * methods out of its vtable */
struct extract;
struct extract_vtbl {
    HRESULT (WINAPI *QueryInterface)( struct extract *iface, REFIID riid, void **out );
    ULONG (WINAPI *AddRef)( struct extract *iface );
    ULONG (WINAPI *Release)( struct extract *iface );
    HRESULT (WINAPI *GetIconLocation)( struct extract *iface, UINT flags, LPWSTR file, UINT len, int *index, UINT *out_flags );
    HRESULT (WINAPI *Extract)( struct extract *iface, LPCWSTR file, UINT index, HICON *large, HICON *small, UINT sizes );
};
struct extract { const struct extract_vtbl *lpVtbl; };

struct icon {
    IPersistFile IPersistFile_iface;
    struct extract IExtractIconW_iface;
    LONG ref;
    WCHAR path[MAX_PATH];
};

static struct icon *from_file( IPersistFile *iface ) { return CONTAINING_RECORD( iface, struct icon, IPersistFile_iface ); }
static struct icon *from_extract( struct extract *iface ) { return CONTAINING_RECORD( iface, struct icon, IExtractIconW_iface ); }

static HRESULT query( struct icon *ic, REFIID riid, void **out )
{
    if (IsEqualIID( riid, &IID_IUnknown ) || IsEqualIID( riid, &IID_IPersist ) || IsEqualIID( riid, &IID_IPersistFile ))
        *out = &ic->IPersistFile_iface;
    else if (IsEqualIID( riid, &IID_IExtractIconW )) *out = &ic->IExtractIconW_iface;
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement( &ic->ref );
    return S_OK;
}

static ULONG release( struct icon *ic )
{
    ULONG ref = InterlockedDecrement( &ic->ref );
    if (!ref) { HeapFree( GetProcessHeap(), 0, ic ); InterlockedDecrement( &g_objects ); }
    return ref;
}

/* ---- IPersistFile: the .desktop file --------------------------------- */

static HRESULT WINAPI pf_QueryInterface( IPersistFile *iface, REFIID riid, void **out ) { return query( from_file( iface ), riid, out ); }
static ULONG WINAPI pf_AddRef( IPersistFile *iface ) { return InterlockedIncrement( &from_file( iface )->ref ); }
static ULONG WINAPI pf_Release( IPersistFile *iface ) { return release( from_file( iface ) ); }
static HRESULT WINAPI pf_GetClassID( IPersistFile *iface, CLSID *clsid ) { *clsid = CLSID_SgLinuxIcon; return S_OK; }
static HRESULT WINAPI pf_IsDirty( IPersistFile *iface ) { return S_FALSE; }
static HRESULT WINAPI pf_Load( IPersistFile *iface, LPCOLESTR name, DWORD mode )
{
    lstrcpynW( from_file( iface )->path, name, MAX_PATH );
    return S_OK;
}
static HRESULT WINAPI pf_Save( IPersistFile *iface, LPCOLESTR name, BOOL remember ) { return E_NOTIMPL; }
static HRESULT WINAPI pf_SaveCompleted( IPersistFile *iface, LPCOLESTR name ) { return E_NOTIMPL; }
static HRESULT WINAPI pf_GetCurFile( IPersistFile *iface, LPOLESTR *name ) { return E_NOTIMPL; }

static IPersistFileVtbl pf_vtbl = {
    pf_QueryInterface, pf_AddRef, pf_Release, pf_GetClassID, pf_IsDirty, pf_Load, pf_Save, pf_SaveCompleted, pf_GetCurFile
};

/* ---- IExtractIconW: the app's icon ------------------------------------ */

/* the Icon= of a .desktop file's [Desktop Entry]: a name, or a path whose
 * name (without folder and extension) is tried */
static BOOL icon_name( const WCHAR *path, WCHAR *out, int len )
{
    char line[1024], name[512] = "";
    BOOL entry = FALSE;
    FILE *f = _wfopen( path, L"rb" );
    char *p, *slash, *dot;

    if (!f) return FALSE;
    while (fgets( line, sizeof(line), f ))
    {
        line[strcspn( line, "\r\n" )] = 0;
        if (line[0] == '[') { entry = !strcmp( line, "[Desktop Entry]" ); continue; }
        if (entry && !strncmp( line, "Icon=", 5 )) { lstrcpynA( name, line + 5, sizeof(name) ); break; }
    }
    fclose( f );
    p = name;
    while (*p == ' ') p++;
    if ((slash = strrchr( p, '/' ))) p = slash + 1;
    if ((dot = strrchr( p, '.' )) && (!_stricmp( dot, ".png" ) || !_stricmp( dot, ".svg" ) || !_stricmp( dot, ".xpm" ) || !_stricmp( dot, ".ico" )))
        *dot = 0;
    if (!*p || strpbrk( p, "\\:*?\"<>|" )) return FALSE;
    MultiByteToWideChar( CP_UTF8, 0, p, -1, out, len );
    return TRUE;
}

static BOOL cached( const WCHAR *name, WCHAR *ico, UINT len )
{
    WCHAR dir[MAX_PATH];
    if (!name[0] || !SHGetSpecialFolderPathW( NULL, dir, CSIDL_LOCAL_APPDATA, FALSE )) return FALSE;
    _snwprintf( ico, len, L"%ls\\Stained Glass\\Linux app icons\\%ls.ico", dir, name );
    ico[len - 1] = 0;
    return GetFileAttributesW( ico ) != INVALID_FILE_ATTRIBUTES;
}

static HRESULT WINAPI ei_QueryInterface( struct extract *iface, REFIID riid, void **out ) { return query( from_extract( iface ), riid, out ); }
static ULONG WINAPI ei_AddRef( struct extract *iface ) { return InterlockedIncrement( &from_extract( iface )->ref ); }
static ULONG WINAPI ei_Release( struct extract *iface ) { return release( from_extract( iface ) ); }

static HRESULT WINAPI ei_GetIconLocation( struct extract *iface, UINT flags, LPWSTR file, UINT len, int *index, UINT *out_flags )
{
    struct icon *ic = from_extract( iface );
    WCHAR id[MAX_PATH], name[512];

    *out_flags = GIL_PERINSTANCE;
    *index = 0;
    /* the app's id: the file's name (steam.desktop: steam) */
    lstrcpynW( id, PathFindFileNameW( ic->path ), MAX_PATH );
    PathRemoveExtensionW( id );
#ifndef SG_MUTANT_LINUXICON_NONE
    if (cached( id, file, len )) return S_OK;
    if (icon_name( ic->path, name, ARRAYSIZE(name) ) && cached( name, file, len )) return S_OK;
#endif
    file[0] = 0;
    return S_FALSE;   /* the type's icon */
}

static HRESULT WINAPI ei_Extract( struct extract *iface, LPCWSTR file, UINT index, HICON *large, HICON *small, UINT sizes )
{
    return S_FALSE;   /* the shell takes it from the .ico */
}

static const struct extract_vtbl ei_vtbl = { ei_QueryInterface, ei_AddRef, ei_Release, ei_GetIconLocation, ei_Extract };

/* ---- the class factory ------------------------------------------------- */

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
    struct icon *ic;
    HRESULT hr;
    *out = NULL;
    if (outer) return CLASS_E_NOAGGREGATION;
    if (!(ic = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*ic) ))) return E_OUTOFMEMORY;
    ic->IPersistFile_iface.lpVtbl = &pf_vtbl;
    ic->IExtractIconW_iface.lpVtbl = &ei_vtbl;
    ic->ref = 1;
    InterlockedIncrement( &g_objects );
    hr = query( ic, riid, out );
    release( ic );
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
    if (!IsEqualCLSID( clsid, &CLSID_SgLinuxIcon )) { *out = NULL; return CLASS_E_CLASSNOTAVAILABLE; }
    return IClassFactory_QueryInterface( &factory, riid, out );
}

HRESULT WINAPI DllCanUnloadNow( void ) { return g_locks || g_objects ? S_FALSE : S_OK; }
