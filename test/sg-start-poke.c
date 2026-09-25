#include <windows.h>
/* the gate's hands: no argument toggles Start as the Start button does;
 * "search" opens it as the taskbar's search does (wparam 1);
 * "tray" announces changed taskbar settings, as Settings does */
int WINAPI wWinMain(HINSTANCE a,HINSTANCE b,PWSTR c,int d){
    (void)a;(void)b;(void)d;
    if (c && !lstrcmpW(c, L"tray")) {
        DWORD_PTR r;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 3000, &r);
        return 0;
    }
    HWND w=FindWindowW(L"SgStartPanel",NULL);
    if(w) PostMessageW(w, WM_USER+10, c && !lstrcmpW(c, L"search") ? 1 : 0, 0);
    return w?0:1;
}
