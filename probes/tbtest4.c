/* tbtest4.c - do two half-width windows together still suppress the raise? */
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

int main(void) {
    HWND p2, p3;
    EnumWindows(E, 0);
    if (!g_tb || !g_veil) { printf("missing window\n"); return 1; }

    /* app veil -> left half; probe window -> right half (near-invisible) */
    SetWindowPos(g_veil, 0, 0, 0, 1440, 1800, SWP_NOACTIVATE | SWP_NOZORDER);
    p2 = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                         WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                         L"STATIC", L"probe2", WS_POPUP,
                         1440, 0, 1440, 1800, NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetLayeredWindowAttributes(p2, 0, 1, LWA_ALPHA);   /* 1/255 alpha: invisible */
    ShowWindow(p2, SW_SHOWNOACTIVATE);
    printf("two halves: app-left 1440 + probe-right 1440\n");

    Park(); Reveal();
    printf("raise with two half windows = %d\n", Top());

    /* also try THREE pieces (1/3 each) for margin */
    p3 = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                         WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                         L"STATIC", L"probe3", WS_POPUP,
                         960, 0, 960, 1800, NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetLayeredWindowAttributes(p3, 0, 1, LWA_ALPHA);
    ShowWindow(p3, SW_SHOWNOACTIVATE);
    SetWindowPos(g_veil, 0, 0, 0, 960, 1800, SWP_NOACTIVATE | SWP_NOZORDER);
    SetWindowPos(p2, 0, 960, 0, 960, 1800, SWP_NOACTIVATE | SWP_NOZORDER);
    Park(); Reveal();
    printf("raise with three third windows = %d\n", Top());

    DestroyWindow(p2); DestroyWindow(p3);
    SetWindowPos(g_veil, 0, 0, 0, 2880, 1800, SWP_NOACTIVATE | SWP_NOZORDER);
    Park();
    printf("cleanup done, raise now: %d\n", Top());
    return 0;
}
