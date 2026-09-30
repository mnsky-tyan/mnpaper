/* tborder.c - wiggle the cursor at the bottom edge, watch the Shell_TrayWnd rect */
#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")

static HWND g_tb;

static BOOL CALLBACK E(HWND h, LPARAM lp) {
    WCHAR c[64];
    GetClassNameW(h, c, 64);
    if (lstrcmpiW(c, L"Shell_TrayWnd") == 0) { g_tb = h; return FALSE; }
    return TRUE;
}

int main(void) {
    int i;
    RECT r;
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    printf("screen %dx%d\n", sw, sh);
    EnumWindows(E, 0);
    if (!g_tb) { printf("no Shell_TrayWnd\n"); return 1; }
    for (i = 0; i < 24; i++) {
        GetWindowRect(g_tb, &r);
        printf("t=%2ds tray rect=(%ld,%ld)-(%ld,%ld) %ldx%ld  %s\n", i / 4,
               r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top,
               r.top < sh - 100 ? "REVEALED" : "parked");
        fflush(stdout);
        if (i == 1) SetCursorPos(sw / 2, sh - 1);          /* touch the edge */
        else if (i == 3) SetCursorPos(sw / 2 - 40, sh - 2); /* wiggle */
        else if (i == 5) SetCursorPos(sw / 2 + 60, sh - 1);
        else if (i == 7) SetCursorPos(sw / 2 + 60, sh - 6); /* hover, stay */
        Sleep(250);
    }
    return 0;
}
