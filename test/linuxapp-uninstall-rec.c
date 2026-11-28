/* linuxapp-uninstall-check.sh's stand-in (for sg-store64.exe and for a Linux app's launcher):
 * appends its command line to the file SG_REC names, and exits 0. */
#include <windows.h>
#include <stdio.h>
int wmain(void)
{
    WCHAR path[MAX_PATH];
    FILE *f;
    if (!GetEnvironmentVariableW(L"SG_REC", path, MAX_PATH) || !(f = _wfopen(path, L"a"))) return 1;
    fprintf(f, "%ls\n", GetCommandLineW());
    fclose(f);
    return 0;
}
