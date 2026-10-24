/* For test/media-smtc-check.sh: a media key as the window in front gets it --
 * WM_APPCOMMAND sent to the first window of a class. Prints what the window
 * returned (TRUE: it took the key).
 *   sg-appcommand CLASS play-pause|play|pause|stop|next|previous */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>
int wmain(int argc, WCHAR **argv)
{
    static const struct { const WCHAR *name; int cmd; } cmds[] = {
        { L"play-pause", APPCOMMAND_MEDIA_PLAY_PAUSE }, { L"play", APPCOMMAND_MEDIA_PLAY },
        { L"pause", APPCOMMAND_MEDIA_PAUSE }, { L"stop", APPCOMMAND_MEDIA_STOP },
        { L"next", APPCOMMAND_MEDIA_NEXTTRACK }, { L"previous", APPCOMMAND_MEDIA_PREVIOUSTRACK },
    };
    HWND w;
    int i;
    if (argc != 3 || !(w = FindWindowW(argv[1], NULL))) { puts("NOWINDOW"); return 1; }
    for (i = 0; i < (int)ARRAYSIZE(cmds); i++)
        if (!wcscmp(argv[2], cmds[i].name))
        {
            LRESULT r = SendMessageW(w, WM_APPCOMMAND, (WPARAM)w, MAKELPARAM(0, FAPPCOMMAND_KEY | cmds[i].cmd));
            printf("RESULT %ld\n", (long)r);
            return 0;
        }
    puts("NOCOMMAND");
    return 1;
}
