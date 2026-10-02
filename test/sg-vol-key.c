/* For test/volume-check.sh: an arrow key on the volume flyout (posted to it,
 * shown or not), as the person's Up key -- the volume goes up and chimes. */
#include <windows.h>
#include <stdio.h>
int wmain(void)
{
    HWND fly = NULL;
    int i;
    for (i = 0; i < 100 && !(fly = FindWindowW(L"SgVolumeFlyout", NULL)); i++) Sleep(100);
    if (!fly) { puts("NOFLYOUT"); return 1; }
    Sleep(1500);   /* the worker's first answer: the outputs */
    PostMessageW(fly, WM_KEYDOWN, VK_UP, 0);
    Sleep(2500);
    puts("POSTED");
    return 0;
}
