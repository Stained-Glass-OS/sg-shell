/* A test process for test/taskmgr-check.sh: burns one CPU until it is ended,
 * so Task Manager has something busy to show and to end. */
#include <windows.h>
int wmain(void)
{
    volatile unsigned long long x = 0;
    for (;;) x++;
    return 0;
}
