/* For test/volume-check.sh: the media keys as a program that does not take
 * them passes them on -- WM_APPCOMMAND to DefWindowProc, which goes on to the
 * shell hook windows (sg-volume's) -- Play/Pause, Next, then Browser Back
 * (not a media key: nothing for the player). */
#include <windows.h>
#include <stdio.h>
int wmain(void)
{
    static const int cmds[] = { APPCOMMAND_MEDIA_PLAY_PAUSE, APPCOMMAND_MEDIA_NEXTTRACK,
                                APPCOMMAND_MEDIA_PAUSE, APPCOMMAND_BROWSER_BACKWARD };
    HWND w;
    int i;
    for (i = 0; i < 100 && !FindWindowW(L"SgVolumeTray", NULL); i++) Sleep(100);
    if (!FindWindowW(L"SgVolumeTray", NULL)) { puts("NOVOLUME"); return 1; }
    Sleep(1000);
    w = CreateWindowW(L"STATIC", L"player-less", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, NULL, NULL);
    for (i = 0; i < (int)ARRAYSIZE(cmds); i++)
    {
        DefWindowProcW(w, WM_APPCOMMAND, (WPARAM)w, MAKELPARAM(0, FAPPCOMMAND_KEY | cmds[i]));
        Sleep(700);
    }
    Sleep(2000);
    puts("SENT");
    return 0;
}
