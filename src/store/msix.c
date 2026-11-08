/* SG Store -- app packages (MSIX, APPX and their bundles), as winget installs them.
 *
 * A winget manifest whose only installer is a package (InstallerType msix or
 * appx) is installed the way winget does it: PackageManager.AddPackageAsync
 * with the downloaded file, for the person running the store (a package is
 * per user unless SYSTEM adds it). Its frameworks (VCLibs, the Windows App
 * Runtime) must be there already; PackageManager names the missing one in
 * its error text, which the store shows as it is.
 *
 * Such an app is found, opened and removed by its package family name (the
 * catalogue's PackageFamily): it has no Uninstall entry, as on Windows.
 *   found   FindPackagesByUserSecurityIdPackageFamilyName("", family)
 *   opened  shell:AppsFolder\<family>!<Application Id> -- the Id from the
 *           installed package's AppxManifest.xml
 *   removed RemovePackageAsync(full name)
 *
 * The interfaces are declared here as their published ABI lays them out
 * (vtable order and IIDs as in the Windows SDK's metadata).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "store.h"
#include <roapi.h>
#include <winstring.h>
#include <shlwapi.h>
#include <shellapi.h>

typedef struct { HRESULT (WINAPI *QueryInterface)(void *, REFIID, void **); ULONG (WINAPI *AddRef)(void *);
                 ULONG (WINAPI *Release)(void *); void *GetIids, *GetRuntimeClassName, *GetTrustLevel; } insp_vtbl;
typedef struct { insp_vtbl i; HRESULT (WINAPI *ActivateInstance)(void *, void **); } factory_vtbl;
typedef struct { insp_vtbl i; HRESULT (WINAPI *CreateUri)(void *, HSTRING, void **); } uri_factory_vtbl;
typedef struct { insp_vtbl i;
                 HRESULT (WINAPI *AddPackageAsync)(void *, void *, void *, UINT32, void **);
                 void *UpdatePackageAsync;
                 HRESULT (WINAPI *RemovePackageAsync)(void *, HSTRING, void **);
                 void *StagePackageAsync, *RegisterPackageAsync, *FindPackages, *FindPackagesByUserSecurityId,
                      *FindPackagesByNamePublisher, *FindPackagesByUserSecurityIdNamePublisher, *FindUsers,
                      *SetPackageState, *FindPackageByPackageFullName, *CleanupPackageForUserAsync,
                      *FindPackagesByPackageFamilyName;
                 HRESULT (WINAPI *FindPackagesByUserSecurityIdPackageFamilyName)(void *, HSTRING, HSTRING, void **); } manager_vtbl;
typedef struct { insp_vtbl i; void *put_Progress, *get_Progress, *put_Completed, *get_Completed;
                 HRESULT (WINAPI *GetResults)(void *, void **); } operation_vtbl;
typedef struct { insp_vtbl i; void *get_Id; HRESULT (WINAPI *get_Status)(void *, int *);
                 HRESULT (WINAPI *get_ErrorCode)(void *, HRESULT *); } async_info_vtbl;
typedef struct { insp_vtbl i; HRESULT (WINAPI *get_ErrorText)(void *, HSTRING *); void *get_ActivityId;
                 HRESULT (WINAPI *get_ExtendedErrorCode)(void *, HRESULT *); } result_vtbl;
typedef struct { insp_vtbl i; HRESULT (WINAPI *First)(void *, void **); } iterable_vtbl;
typedef struct { insp_vtbl i; HRESULT (WINAPI *get_Current)(void *, void **); HRESULT (WINAPI *get_HasCurrent)(void *, boolean *);
                 HRESULT (WINAPI *MoveNext)(void *, boolean *); } iterator_vtbl;
typedef struct { insp_vtbl i; HRESULT (WINAPI *get_Id)(void *, void **); HRESULT (WINAPI *get_InstalledLocation)(void *, void **); } package_vtbl;
typedef struct { UINT16 major, minor, build, revision; } package_version;
typedef struct { insp_vtbl i; void *get_Name; HRESULT (WINAPI *get_Version)(void *, package_version *);
                 void *get_Architecture, *get_ResourceId, *get_Publisher, *get_PublisherId;
                 HRESULT (WINAPI *get_FullName)(void *, HSTRING *); } package_id_vtbl;
typedef struct { insp_vtbl i; void *RenameAsync, *RenameAsyncOverload, *DeleteAsync, *DeleteAsyncOverload,
                 *GetBasicPropertiesAsync, *get_Name; HRESULT (WINAPI *get_Path)(void *, HSTRING *); } storage_item_vtbl;
typedef struct { const void *vtbl; } obj_t;
#define VT(o, T) ((const T *)((obj_t *)(o))->vtbl)
#define RELEASE(o) do { if (o) VT(o, insp_vtbl)->Release(o); } while (0)

static const GUID iid_activation_factory = {0x00000035,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const GUID iid_async_info = {0x00000036,0,0,{0xc0,0,0,0,0,0,0,0x46}};
static const GUID iid_package_manager = {0x9a7d4b65,0x5e8f,0x4fc7,{0xa2,0xe5,0x7f,0x69,0x25,0xcb,0x8b,0x53}};
static const GUID iid_uri_factory = {0x44a9796f,0x723e,0x4fdf,{0xa2,0x18,0x03,0x3e,0x75,0xb0,0xc0,0x84}};
static const GUID iid_storage_item = {0x4207a996,0xca2f,0x42f7,{0xbd,0xe8,0x8b,0x10,0x45,0x7a,0x7f,0x30}};

static HSTRING hs(const WCHAR *s)
{
    HSTRING h = NULL;
    WindowsCreateString(s, lstrlenW(s), &h);
    return h;
}

static void *activation_factory(const WCHAR *cls)
{
    void *f = NULL;
    HSTRING name = hs(cls);
    RoGetActivationFactory(name, &iid_activation_factory, &f);
    WindowsDeleteString(name);
    return f;
}

static void *package_manager(void)
{
    void *f = activation_factory(L"Windows.Management.Deployment.PackageManager"), *inst = NULL, *pm = NULL;
    if (!f) return NULL;
    if (SUCCEEDED(VT(f, factory_vtbl)->ActivateInstance(f, &inst)) && inst)
        VT(inst, insp_vtbl)->QueryInterface(inst, &iid_package_manager, &pm);
    RELEASE(inst);
    RELEASE(f);
    return pm;
}

/* the store's threads are COM's already or not: either way WinRT is there */
static void ro_init(void)
{
    RoInitialize(RO_INIT_MULTITHREADED);
}

/* wait for a deployment operation (half an hour at most); 0 when it succeeded,
 * else err says what PackageManager said */
static int wait_op(void *op, const WCHAR *what, WCHAR *err, int cch)
{
    void *info = NULL, *result = NULL;
    HRESULT error = S_OK;
    HSTRING text = NULL;
    int status = 0, i;
    if (FAILED(VT(op, insp_vtbl)->QueryInterface(op, &iid_async_info, &info)) || !info) {
        swprintf(err, cch, L"%ls: the package manager gave no answer.", what);
        return 1;
    }
    for (i = 0; i < 30 * 60 * 10; i++) {
        VT(info, async_info_vtbl)->get_Status(info, &status);
        if (status) break;   /* 0 started, 1 completed, 2 canceled, 3 error */
        Sleep(100);
    }
    VT(info, async_info_vtbl)->get_ErrorCode(info, &error);
    RELEASE(info);
    if (status == 1 && SUCCEEDED(error)) return 0;
    if (SUCCEEDED(VT(op, operation_vtbl)->GetResults(op, &result)) && result) {
        VT(result, result_vtbl)->get_ErrorText(result, &text);
        RELEASE(result);
    }
    if (text && WindowsGetStringLen(text))
        swprintf(err, cch, L"%ls (0x%08lx): %ls", what, (unsigned long)error, WindowsGetStringRawBuffer(text, NULL));
    else
        swprintf(err, cch, L"%ls (0x%08lx).", what, (unsigned long)(status ? error : HRESULT_FROM_WIN32(ERROR_TIMEOUT)));
    if (text) WindowsDeleteString(text);
    return 1;
}

BOOL msix_add(const WCHAR *file, WCHAR *err, int cch)
{
    void *pm, *f, *uf = NULL, *uri = NULL, *op = NULL;
    WCHAR url[MAX_PATH * 3];
    DWORD len = ARRAYSIZE(url);
    HSTRING s;
    HRESULT hr;
    int rc = 1;
    ro_init();
    if (!(pm = package_manager())) { swprintf(err, cch, L"App packages can't be installed here (no package manager)."); return FALSE; }
    if (FAILED(UrlCreateFromPathW(file, url, &len, 0))) { swprintf(err, cch, L"The package's path is not usable."); RELEASE(pm); return FALSE; }
    if ((f = activation_factory(L"Windows.Foundation.Uri"))) {
        VT(f, insp_vtbl)->QueryInterface(f, &iid_uri_factory, &uf);
        RELEASE(f);
    }
    s = hs(url);
    if (!uf || FAILED(VT(uf, uri_factory_vtbl)->CreateUri(uf, s, &uri)) || !uri)
        swprintf(err, cch, L"The package's path is not usable.");
    else if (FAILED(hr = VT(pm, manager_vtbl)->AddPackageAsync(pm, uri, NULL, 0, &op)) || !op)
        swprintf(err, cch, L"The package was not installed (0x%08lx).", (unsigned long)hr);
    else
        rc = wait_op(op, L"The package was not installed", err, cch);
    WindowsDeleteString(s);
    RELEASE(op); RELEASE(uri); RELEASE(uf); RELEASE(pm);
    return rc == 0;
}

/* the person's installed package of this family: its full name, version and
 * folder (any may be NULL); FALSE when there is none */
static BOOL find_package(const WCHAR *family, WCHAR *full, int fcch, WCHAR *version, int vcch, WCHAR *dir, int dcch)
{
    void *pm, *list = NULL, *iter = NULL, *pkg = NULL, *id = NULL, *folder = NULL, *item = NULL;
    HSTRING sid = NULL, fam, str;
    boolean has = FALSE;
    BOOL found = FALSE;
    if (!family || !family[0]) return FALSE;
    ro_init();
    if (!(pm = package_manager())) return FALSE;
    fam = hs(family);
    if (SUCCEEDED(VT(pm, manager_vtbl)->FindPackagesByUserSecurityIdPackageFamilyName(pm, sid, fam, &list)) && list &&
        SUCCEEDED(VT(list, iterable_vtbl)->First(list, &iter)) && iter &&
        SUCCEEDED(VT(iter, iterator_vtbl)->get_HasCurrent(iter, &has)) && has &&
        SUCCEEDED(VT(iter, iterator_vtbl)->get_Current(iter, &pkg)) && pkg) {
        found = TRUE;
        if (SUCCEEDED(VT(pkg, package_vtbl)->get_Id(pkg, &id)) && id) {
            package_version v;
            if (full && SUCCEEDED(VT(id, package_id_vtbl)->get_FullName(id, &str))) {
                lstrcpynW(full, WindowsGetStringRawBuffer(str, NULL), fcch);
                WindowsDeleteString(str);
            }
            if (version && SUCCEEDED(VT(id, package_id_vtbl)->get_Version(id, &v)))
                swprintf(version, vcch, L"%u.%u.%u.%u", v.major, v.minor, v.build, v.revision);
        }
        if (dir && SUCCEEDED(VT(pkg, package_vtbl)->get_InstalledLocation(pkg, &folder)) && folder &&
            SUCCEEDED(VT(folder, insp_vtbl)->QueryInterface(folder, &iid_storage_item, &item)) && item &&
            SUCCEEDED(VT(item, storage_item_vtbl)->get_Path(item, &str))) {
            lstrcpynW(dir, WindowsGetStringRawBuffer(str, NULL), dcch);
            WindowsDeleteString(str);
        }
    }
    WindowsDeleteString(fam);
    RELEASE(item); RELEASE(folder); RELEASE(id); RELEASE(pkg); RELEASE(iter); RELEASE(list); RELEASE(pm);
    return found;
}

BOOL msix_installed(const WCHAR *family, WCHAR *version, int cch)
{
    if (version && cch) version[0] = 0;
    return find_package(family, NULL, 0, version, cch, NULL, 0);
}

int msix_remove(const WCHAR *family, const WCHAR *name, WCHAR *err, int cch)
{
    WCHAR full[256] = L"";
    void *pm, *op = NULL;
    HSTRING s;
    HRESULT hr;
    int rc = 1;
    if (!find_package(family, full, ARRAYSIZE(full), NULL, 0, NULL, 0) || !full[0]) return 0;   /* not (any longer) there */
    if (!(pm = package_manager())) { swprintf(err, cch, L"%ls was not removed (no package manager).", name); return 1; }
    s = hs(full);
    if (FAILED(hr = VT(pm, manager_vtbl)->RemovePackageAsync(pm, s, &op)) || !op)
        swprintf(err, cch, L"%ls was not removed (0x%08lx).", name, (unsigned long)hr);
    else
        rc = wait_op(op, L"It was not removed", err, cch);
    WindowsDeleteString(s);
    RELEASE(op); RELEASE(pm);
    return rc;
}

/* the first Application's Id in the installed package's manifest */
static BOOL first_app_id(const WCHAR *dir, WCHAR *out, int cch)
{
    WCHAR path[MAX_PATH];
    HANDLE f;
    DWORD size, got = 0;
    char *xml, *app, *id, *end;
    BOOL ok = FALSE;
    if (swprintf(path, MAX_PATH, L"%ls\\AppxManifest.xml", dir) < 0) return FALSE;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    size = GetFileSize(f, NULL);
    if (size != INVALID_FILE_SIZE && size < 4 * 1024 * 1024 && (xml = malloc(size + 1))) {
        if (ReadFile(f, xml, size, &got, NULL)) {
            xml[got] = 0;
            /* <Application Id="App" ...> under <Applications>; not <Applications> itself */
            for (app = xml; (app = strstr(app, "<Application")); app += 12)
                if (app[12] == ' ' || app[12] == '\t' || app[12] == '\r' || app[12] == '\n') break;
            if (app && (end = strchr(app, '>')) && (id = strstr(app, " Id=\"")) && id < end) {
                char *q = strchr(id + 5, '"');
                if (q && q < end) {
                    *q = 0;
                    ok = MultiByteToWideChar(CP_UTF8, 0, id + 5, -1, out, cch) > 0;
                }
            }
        }
        free(xml);
    }
    CloseHandle(f);
    return ok;
}

/* the app's AppUserModelID ("<family>!<Id>"): what Start's entry starts */
BOOL msix_aumid(const WCHAR *family, WCHAR *out, int cch)
{
    WCHAR dir[MAX_PATH] = L"", id[128];
    if (!find_package(family, NULL, 0, NULL, 0, dir, MAX_PATH) || !dir[0] || !first_app_id(dir, id, ARRAYSIZE(id))) return FALSE;
    return swprintf(out, cch, L"%ls!%ls", family, id) > 0;
}

BOOL msix_open(const WCHAR *family)
{
    WCHAR aumid[400], target[460];
    if (!msix_aumid(family, aumid, ARRAYSIZE(aumid))) return FALSE;
    swprintf(target, ARRAYSIZE(target), L"shell:AppsFolder\\%ls", aumid);
    return (INT_PTR)ShellExecuteW(NULL, NULL, target, NULL, NULL, SW_SHOWNORMAL) > 32;
}
