/* front-probe TITLE: bring the top-level window titled TITLE forward as a person does -- a click on its
 * caption where it shows, else on its taskbar button (SendInput, input as xdotool's is). Since wine-sg 9930's foreground lock a program the
 * person did not click may not bring another program's window forward; gates that start a program from
 * outside after typing elsewhere bring its window forward this way. Without a button of its own: Alt+Tab. */
#include <windows.h>
#include <stdio.h>

static void click( int x, int y )
{
    INPUT in[2] = { { INPUT_MOUSE }, { INPUT_MOUSE } };
    POINT old;
    GetCursorPos( &old );
    SetCursorPos( x, y );
    Sleep( 100 );
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput( 2, in, sizeof(INPUT) );
    Sleep( 300 );
    SetCursorPos( old.x, old.y );
}

static const WCHAR *want;
static HWND found;

static BOOL CALLBACK find_visible( HWND hwnd, LPARAM lp )
{
    WCHAR t[256];
    if (!IsWindowVisible( hwnd ) || !GetWindowTextW( hwnd, t, 256 ) || lstrcmpW( t, want )) return TRUE;
    found = hwnd;
    return FALSE;
}

int wmain( int argc, WCHAR **argv )
{
    HWND w = NULL, tray = FindWindowW( L"Shell_TrayWnd", NULL ), b;
    int i;

    if (argc < 2) return 2;
    want = argv[1];
    for (i = 0; i < 50 && !w; i++)
    {
        found = NULL;
        EnumWindows( find_visible, 0 );
        if (!(w = found)) Sleep( 100 );
    }
    if (w && GetAncestor( w, GA_ROOTOWNER )) w = GetAncestor( w, GA_ROOTOWNER );   /* the taskbar's window */
    if (!w) { puts( "NOWINDOW" ); return 1; }
    if (GetAncestor( GetForegroundWindow(), GA_ROOTOWNER ) == w) { puts( "FRONT" ); return 0; }
    /* a click on a part of its caption that shows (clear of its buttons), as a person clicks a window */
    {
        HWND vis = found ? found : w;
        RECT r;
        int x, y;
        GetWindowRect( vis, &r );
        y = r.top + GetSystemMetrics( SM_CYFRAME ) + GetSystemMetrics( SM_CYCAPTION ) / 2;
        for (x = r.right - 150; x > r.left + 30; x -= 20)
        {
            POINT pt = { x, y };
            if (GetAncestor( WindowFromPoint( pt ), GA_ROOT ) != vis) continue;
            click( x, y );
            Sleep( 500 );
            if (GetAncestor( GetForegroundWindow(), GA_ROOTOWNER ) == w || GetForegroundWindow() == vis)
            {
                puts( "CAPTION" );
                return 0;
            }
            break;
        }
    }
    for (b = tray ? GetWindow( tray, GW_CHILD ) : NULL; b; b = GetWindow( b, GW_HWNDNEXT ))
        if ((HWND)(ULONG_PTR)GetDlgCtrlID( b ) == w && IsWindowVisible( b )) break;
    if (b)
    {
        RECT r;
        GetWindowRect( b, &r );
        click( (r.left + r.right) / 2, (r.top + r.bottom) / 2 );
        puts( "CLICKED" );
    }
    else
    {
        /* no button of its own (combined with its program's others): Alt+Tab to it, as a person does,
         * one more Tab each try */
        int n;
        for (n = 1; n <= 8 && GetAncestor( GetForegroundWindow(), GA_ROOTOWNER ) != w; n++)
        {
            INPUT k = { INPUT_KEYBOARD };
            int t;
            k.ki.wVk = VK_MENU; SendInput( 1, &k, sizeof(k) ); Sleep( 150 );
            for (t = 0; t < n; t++)
            {
                k.ki.wVk = VK_TAB; k.ki.dwFlags = 0; SendInput( 1, &k, sizeof(k) ); Sleep( 60 );
                k.ki.dwFlags = KEYEVENTF_KEYUP; SendInput( 1, &k, sizeof(k) ); Sleep( 150 );
            }
            k.ki.wVk = VK_MENU; k.ki.dwFlags = KEYEVENTF_KEYUP; SendInput( 1, &k, sizeof(k) );
            Sleep( 800 );
        }
        puts( GetAncestor( GetForegroundWindow(), GA_ROOTOWNER ) == w ? "ALTTAB" : "NOTFRONT" );
    }
    return 0;
}
