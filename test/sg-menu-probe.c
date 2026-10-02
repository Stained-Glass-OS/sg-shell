/* For test/runas-check.sh: a file's shell item menu, as the desktop and
 * File Explorer get it (IShellFolder::GetUIObjectOf IContextMenu).
 *   menu FILE            each item: VERB<TAB>TEXT
 *   invoke FILE VERB     InvokeCommand on the item with that verb (by its id)
 *   link LNK TARGET      make a shortcut */
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

static IContextMenu *menu_of(const WCHAR *file)
{
    IShellFolder *parent; LPITEMIDLIST pidl; LPCITEMIDLIST child; IContextMenu *cm = NULL;
    if (FAILED(SHParseDisplayName(file, NULL, &pidl, 0, NULL))) return NULL;
    if (SUCCEEDED(SHBindToParent(pidl, &IID_IShellFolder, (void **)&parent, &child))) {
        IShellFolder_GetUIObjectOf(parent, NULL, 1, &child, &IID_IContextMenu, NULL, (void **)&cm);
        IShellFolder_Release(parent);
    }
    CoTaskMemFree(pidl);
    return cm;
}

int wmain(int argc, WCHAR **argv)
{
    IContextMenu *cm;
    CoInitialize(NULL);
    if (argc == 4 && !wcscmp(argv[1], L"link")) {
        IShellLinkW *sl; IPersistFile *pf;
        if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&sl))) return 1;
        IShellLinkW_SetPath(sl, argv[3]);
        IShellLinkW_QueryInterface(sl, &IID_IPersistFile, (void **)&pf);
        return FAILED(IPersistFile_Save(pf, argv[2], TRUE));
    }
    if (argc < 3 || !(cm = menu_of(argv[2]))) { printf("NOMENU\n"); return 1; }
    if (!wcscmp(argv[1], L"menu")) {
        HMENU m = CreatePopupMenu();
        int i, n;
        IContextMenu_QueryContextMenu(cm, m, 0, 1, 0x7fff, CMF_NORMAL);
        n = GetMenuItemCount(m);
        for (i = 0; i < n; i++) {
            WCHAR text[128] = L""; char verb[64] = "";
            UINT id = GetMenuItemID(m, i);
            GetMenuStringW(m, i, text, 128, MF_BYPOSITION);
            if (id && id != (UINT)-1) IContextMenu_GetCommandString(cm, id - 1, GCS_VERBA, NULL, verb, sizeof(verb));
            printf("%s\t%ls\n", verb, text);
        }
        return 0;
    }
    if (argc == 4 && !wcscmp(argv[1], L"invoke")) {
        /* by the item's id, as the desktop does after TrackPopupMenu */
        CMINVOKECOMMANDINFO info = { sizeof(info) };
        HMENU m = CreatePopupMenu();
        char want[64], verb[64];
        int i, n;
        WideCharToMultiByte(CP_ACP, 0, argv[3], -1, want, sizeof(want), NULL, NULL);
        IContextMenu_QueryContextMenu(cm, m, 0, 1, 0x7fff, CMF_NORMAL);
        n = GetMenuItemCount(m);
        for (i = 0; i < n; i++) {
            UINT id = GetMenuItemID(m, i);
            verb[0] = 0;
            if (!id || id == (UINT)-1) continue;
            IContextMenu_GetCommandString(cm, id - 1, GCS_VERBA, NULL, verb, sizeof(verb));
            if (strcmp(verb, want)) continue;
            info.lpVerb = MAKEINTRESOURCEA(id - 1); info.nShow = SW_SHOWNORMAL;
            return FAILED(IContextMenu_InvokeCommand(cm, &info));
        }
        printf("NOVERB\n");
        return 1;
    }
    return 2;
}
