/* linuxicon-probe PATH -- the shell's icon location for PATH
 * (SHGetFileInfo SHGFI_ICONLOCATION): "PATH,INDEX". For linuxicon-check.sh. */
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
int wmain(int argc, WCHAR **argv)
{
    SHFILEINFOW fi = { 0 };
    if (argc < 2) return 2;
    CoInitialize(NULL);
    if (!SHGetFileInfoW(argv[1], 0, &fi, sizeof(fi), SHGFI_ICONLOCATION)) { printf("failed\n"); return 1; }
    printf("%ls,%d\n", fi.szDisplayName, fi.iIcon);
    return 0;
}
