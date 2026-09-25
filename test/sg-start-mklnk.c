/* sg-start-mklnk: make Start Menu shortcuts for the Start gate.
 *   sg-start-mklnk DIR NAME TARGET          DIR\NAME.lnk -> TARGET
 *   sg-start-mklnk DIR PREFIX TARGET COUNT  DIR\PREFIX NNN.lnk, COUNT of them */
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>

static int make(const WCHAR *dir, const WCHAR *name, const WCHAR *target)
{
    IShellLinkW *link; IPersistFile *file; WCHAR path[MAX_PATH]; HRESULT hr;
    SHCreateDirectoryExW(NULL, dir, NULL);
    swprintf(path, MAX_PATH, L"%ls\\%ls.lnk", dir, name);
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) return 1;
    IShellLinkW_SetPath(link, target);
    IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file);
    hr = IPersistFile_Save(file, path, TRUE);
    IPersistFile_Release(file); IShellLinkW_Release(link);
    return FAILED(hr);
}

int wmain(int argc, WCHAR **argv)
{
    int i, n, bad = 0;
    WCHAR name[128];
    if (argc < 4) return 2;
    CoInitialize(NULL);
    if (argc == 4) return make(argv[1], argv[2], argv[3]);
    n = _wtoi(argv[4]);
    for (i = 0; i < n; i++) { swprintf(name, 128, L"%ls %03d", argv[2], i); bad |= make(argv[1], name, argv[3]); }
    return bad;
}
