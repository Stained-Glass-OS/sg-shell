/* linuxapp-identity-check.sh's helper: what the shell reads of a Linux app's
 * identity (sg-linuxapp, wine-sg 9511).
 *
 *   linuxapp-identity-probe appid LNK      the shortcut's System.AppUserModel.ID
 *   linuxapp-identity-probe jump APPID     the app's jump list: kind<TAB>title a line
 *                                          (shell32's SGJumpList*, as the taskbar reads it)
 *   linuxapp-identity-probe invoke APPID N runs the jump list's entry N
 */
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <propsys.h>
#include <stdio.h>

static const PROPERTYKEY key_aumid = {{0x9f4c2855, 0x9f79, 0x4b39, {0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3}}, 5};
static const IID iid_store = {0x886d8eeb, 0x8cf2, 0x4446, {0x8d, 0x02, 0xcd, 0xba, 0x1d, 0xbd, 0xcf, 0x99}};

int wmain(int argc, WCHAR **argv)
{
    CoInitialize(NULL);
    if (argc == 3 && !lstrcmpW(argv[1], L"appid")) {
        IShellLinkW *link;
        IPersistFile *file;
        IPropertyStore *store;
        PROPVARIANT v;
        if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) return 2;
        IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file);
        if (FAILED(IPersistFile_Load(file, argv[2], STGM_READ))) return 3;
        if (FAILED(IShellLinkW_QueryInterface(link, &iid_store, (void **)&store))) return 4;
        PropVariantInit(&v);
        if (SUCCEEDED(IPropertyStore_GetValue(store, &key_aumid, &v)) && v.vt == VT_LPWSTR) printf("%ls\n", v.pwszVal);
        return 0;
    }
    if (argc >= 3 && (!lstrcmpW(argv[1], L"jump") || !lstrcmpW(argv[1], L"invoke"))) {
        HMODULE shell32 = LoadLibraryW(L"shell32.dll");
        HRESULT (WINAPI *load)(const WCHAR *, const WCHAR *, void **) = (void *)GetProcAddress(shell32, "SGJumpListLoad");
        UINT (WINAPI *count)(void *) = (void *)GetProcAddress(shell32, "SGJumpListCount");
        BOOL (WINAPI *entry)(void *, UINT, int *, WCHAR *, UINT) = (void *)GetProcAddress(shell32, "SGJumpListEntry");
        HRESULT (WINAPI *invoke)(void *, UINT, HWND) = (void *)GetProcAddress(shell32, "SGJumpListInvoke");
        void *view = NULL;
        UINT i, n;
        if (!load || !count || !entry || !invoke) return 5;
        if (FAILED(load(argv[2], NULL, &view)) || !view) return 6;
        n = count(view);
        if (argc == 4) return FAILED(invoke(view, _wtoi(argv[3]), NULL)) ? 7 : 0;
        for (i = 0; i < n; i++) {
            WCHAR title[MAX_PATH];
            int kind = -1;
            if (entry(view, i, &kind, title, MAX_PATH)) printf("%d\t%ls\n", kind, title);
        }
        return 0;
    }
    fprintf(stderr, "usage: linuxapp-identity-probe appid LNK | jump APPID | invoke APPID N\n");
    return 1;
}
