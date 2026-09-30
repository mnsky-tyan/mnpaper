#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")
static DWORD g_pid;
static BOOL CALLBACK Cb(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == g_pid) {
        WCHAR cls[64], txt[80];
        GetClassNameW(h, cls, 64); GetWindowTextW(h, txt, 80);
        printf("%p %-26S %-30S vis=%d\n", (void*)h, cls, txt, (int)IsWindowVisible(h));
    }
    return TRUE;
}
int main(int argc, char **argv) {
    DWORD pid = (DWORD)atoi(argv[1]);
    g_pid = pid;
    EnumWindows(Cb, 0);
    return 0;
}
