#include <windows.h>
int WINAPI wWinMain(HINSTANCE a,HINSTANCE b,PWSTR c,int d){
    (void)a;(void)b;(void)c;(void)d;
    HWND w=FindWindowW(L"SgStartPanel",NULL);
    if(w) PostMessageW(w, WM_USER+10, 0, 0);
    return w?0:1;
}
