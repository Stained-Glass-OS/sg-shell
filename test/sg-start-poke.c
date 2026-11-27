#include <windows.h>
/* the gate's hands: no argument toggles Start as the Start button does;
 * "search" opens it as the taskbar's search does (wparam 1);
 * "tray" announces changed taskbar settings, as Settings does.
 * Start is the person's Windows key, search their click on the taskbar's search button (SendInput, input
 * as xdotool's is): since wine-sg 9930's foreground lock a program the person did not click cannot bring
 * Start forward; the shell hands Start that right on the person's key or click. Without a taskbar (a bare
 * Wine): the message to Start, as before. */
static BOOL click_tray_child( UINT id )
{
    HWND tray = FindWindowW( L"Shell_TrayWnd", NULL ), b;
    INPUT in[2] = { { INPUT_MOUSE }, { INPUT_MOUSE } };
    POINT old;
    RECT r;

    for (b = tray ? GetWindow( tray, GW_CHILD ) : NULL; b; b = GetWindow( b, GW_HWNDNEXT ))
    {
        WCHAR cls[32];
        if (!IsWindowVisible( b ) || (UINT)GetDlgCtrlID( b ) != id) continue;
        if (!GetClassNameW( b, cls, ARRAYSIZE(cls) ) || lstrcmpiW( cls, L"Button" )) continue;
        break;
    }
    if (!b) return FALSE;
    GetWindowRect( b, &r );
    GetCursorPos( &old );
    SetCursorPos( (r.left + r.right) / 2, (r.top + r.bottom) / 2 );
    Sleep( 100 );
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput( 2, in, sizeof(INPUT) );
    Sleep( 300 );
    SetCursorPos( old.x, old.y );
    return TRUE;
}

/* the Windows key alone, as the person presses it: the shell opens (or closes) Start */
static BOOL win_key( void )
{
    INPUT in[2] = { { INPUT_KEYBOARD }, { INPUT_KEYBOARD } };
    if (!FindWindowW( L"Shell_TrayWnd", NULL )) return FALSE;
    in[0].ki.wVk = in[1].ki.wVk = VK_LWIN;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    return SendInput( 2, in, sizeof(INPUT) ) == 2;
}

int WINAPI wWinMain(HINSTANCE a,HINSTANCE b,PWSTR c,int d){
    BOOL search = c && !lstrcmpW(c, L"search");
    HWND w;
    (void)a;(void)b;(void)d;
    if (c && !lstrcmpW(c, L"tray")) {
        DWORD_PTR r;
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 3000, &r);
        return 0;
    }
    w=FindWindowW(L"SgStartPanel",NULL);
    if (!w) return 1;
    if (search ? click_tray_child( 0x5345 /* the search button */ ) : win_key()) return 0;
    PostMessageW(w, WM_USER+10, search ? 1 : 0, 0);
    return 0;
}
