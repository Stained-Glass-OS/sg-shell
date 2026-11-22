/* store-busy-check's slow installer: C:\gate\running while it "installs"
 * (eight seconds), then gone. */
#include <windows.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show)
{
    HANDLE f = CreateFileW(L"C:\\gate\\running", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    (void)inst; (void)prev; (void)cmd; (void)show;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    Sleep(8000);
    DeleteFileW(L"C:\\gate\\running");
    return 0;
}
