/* tbtest5.c - how much width inset defeats the suppression? */
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

static HWND MkPiece(int x, int w) {
    HWND p = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                             WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                             L"STATIC", L"probe", WS_POPUP,
                             x, 0, w, 1800, NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetLayeredWindowAttributes(p, 0, 1, LWA_ALPHA);
    ShowWindow(p, SW_SHOWNOACTIVATE);
    return p;
}

static void Trial(const char *name, int w) {
    Park();
    SetWindowPos(g_veil, 0, 0, 0, w, 1800, SWP_NOACTIVATE | SWP_NOZORDER);
    Reveal();
    printf("%-34s -> raise=%d\n", name, Top());
    fflush(stdout);
}

int main(void) {
    HWND g1, g2;
    EnumWindows(E, 0);
    if (!g_tb || !g_veil) { printf("missing window\n"); return 1; }

    Trial("single 2879 wide (1px inset)", 2879);
    Trial("single 2840 wide (40px inset)", 2840);
    Trial("single 2760 wide (120px inset)", 2760);
    Trial("single 2640 wide", 2640);
    Trial("single 2520 wide", 2520);
    Trial("single 2304 wide (80%)", 2304);

    /* gapped pair: left 0..1436, right 1444..2880 (8px gap) */
    SetWindowPos(g_veil, 0, 0, 0, 1436, 1800, SWP_NOACTIVATE | SWP_NOZORDER);
    g1 = MkPiece(1444, 1436);
    Park(); Reveal();
    printf("gapped pair (8px gap at center)      -> raise=%d\n", Top());
    DestroyWindow(g1);

    /* restore */
    SetWindowPos(g_veil, 0, 0, 0, 2880, 1800, SWP_NOACTIVATE | SWP_NOZORDER);
    Park();
    printf("restored, raise now: %d\n", Top());
    return 0;
}
