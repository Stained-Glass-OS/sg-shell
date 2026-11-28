/* start-jumplist-check.sh's helper.
 *   start-jumplist-probe link LNK TARGET APPID   a shortcut with its System.AppUserModel.ID
 *   start-jumplist-probe recent APPID FILE       FILE among APPID's recent documents (SHAddToRecentDocs)
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
    if (argc == 5 && !lstrcmpW(argv[1], L"link")) {
        IShellLinkW *link;
        IPersistFile *file;
        IPropertyStore *store;
        PROPVARIANT v;
        if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) return 2;
        IShellLinkW_SetPath(link, argv[3]);
        if (FAILED(IShellLinkW_QueryInterface(link, &iid_store, (void **)&store))) return 3;
        PropVariantInit(&v);
        v.vt = VT_LPWSTR;
        v.pwszVal = argv[4];
        IPropertyStore_SetValue(store, &key_aumid, &v);
        IPropertyStore_Commit(store);
        IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file);
        return FAILED(IPersistFile_Save(file, argv[2], TRUE)) ? 4 : 0;
    }
    if (argc == 4 && !lstrcmpW(argv[1], L"recent")) {
        SHARDAPPIDINFO info;
        IShellItem *item;
        if (FAILED(SHCreateItemFromParsingName(argv[3], NULL, &IID_IShellItem, (void **)&item))) return 5;
        info.psi = item;
        info.pszAppID = argv[2];
        SHAddToRecentDocs(SHARD_APPIDINFO, &info);
        return 0;
    }
    fprintf(stderr, "usage: start-jumplist-probe link LNK TARGET APPID | recent APPID FILE\n");
    return 1;
}
