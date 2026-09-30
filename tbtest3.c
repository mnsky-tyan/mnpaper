/* tbtest3.c - find the exact veil geometry that suppresses the taskbar raise */
#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")

static HWND g_tb, g_veil;

static BOOL CALLBACK E(HWND h, LPARAM lp) {
    WCHAR cls[64];
    GetClassNameW(h, cls, 64);
    if (lstrcmpiW(cls, L"Shell_TrayWnd") == 0) { g_tb = h; return FALSE; }
    if (lstrcmpiW(cls, L"MnPaperOverlay") == 0) g_veil = h;
    return TRUE;
}

static int Top(void) {
    LONG ex = g_tb ? GetWindowLongW(g_tb, GWL_EXSTYLE) : 0;
    return (int)((ex & WS_EX_TOPMOST) != 0);
}

static void Reveal(void) {
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    SetCursorPos(sw / 2, sh - 1);      Sleep(150);
    SetCursorPos(sw / 2 - 60, sh - 2); Sleep(100);
    SetCursorPos(sw / 2 + 40, sh - 1); Sleep(100);
    SetCursorPos(sw / 2 + 10, sh - 2);
    Sleep(700);
}

static void Park(void) {
    SetCursorPos(GetSystemMetrics(SM_CXSCREEN) / 2, 400);
    Sleep(900);
}

static void Trial(const char *name, int w, int h, int x, int y) {
    Park();
    SetWindowPos(g_veil, 0, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER);
    Reveal();
    printf("%-30s -> raise=%d\n", name, Top());
    fflush(stdout);
}

int main(void) {
    EnumWindows(E, 0);
    if (!g_tb || !g_veil) { printf("missing window\n"); return 1; }
    Trial("height 1700 (bottom 100 gap)", 2880, 1700, 0, 0);
    Trial("height 1550 (bottom 250 gap)", 2880, 1550, 0, 0);
    Trial("height 1400 (bottom 400 gap)", 2880, 1400, 0, 0);
    Trial("height 1000",                 2880, 1000, 0, 0);
    Trial("half width 1440x1800",        1440, 1800, 0, 0);
    Trial("width 2000 x1800",            2000, 1800, 0, 0);
    Trial("width 1600 x1800",            1600, 1800, 0, 0);
    Trial("full 2880x1800 (control)",    2880, 1800, 0, 0);
    /* restore */
    SetWindowPos(g_veil, 0, 0, 0, 2880, 1800, SWP_NOACTIVATE | SWP_NOZORDER);
    Park();
    printf("restored, raise state now: %d\n", Top());
    return 0;
}
