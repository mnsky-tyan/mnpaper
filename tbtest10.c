/* tbtest10.c - stability check: 4x450 strips vs controls, repeated */
#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")

static HWND g_tb;

static BOOL CALLBACK E(HWND h, LPARAM lp) {
    WCHAR cls[64];
    GetClassNameW(h, cls, 64);
    if (lstrcmpiW(cls, L"Shell_TrayWnd") == 0) { g_tb = h; return FALSE; }
    return TRUE;
}

static int Top(void) {
    LONG ex = g_tb ? GetWindowLongW(g_tb, GWL_EXSTYLE) : 0;
    return (int)((ex & WS_EX_TOPMOST) != 0);
}

static void Reveal(void) {
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    SetCursorPos(sw / 2, sh - 40); Sleep(40);
    SetCursorPos(sw / 2, sh - 15); Sleep(40);
    SetCursorPos(sw / 2, sh - 1);  Sleep(700);
}

static void Park(void) {
    SetCursorPos(GetSystemMetrics(SM_CXSCREEN) / 2, 400);
    Sleep(1100);
    if (Top()) {
        INPUT in[2];
        memset(in, 0, sizeof(in));
        in[0].type = INPUT_MOUSE; in[1].type = INPUT_MOUSE;
        in[0].mi.dwFlags = 0x8001; in[0].mi.dx = 720*65535/2879; in[0].mi.dy = 300*65535/1799;
        in[1].mi.dwFlags = 0x8001 | 0x0002;
        SendInput(2, in, sizeof(INPUT));
        Sleep(900);
    }
}

static HWND Mk(int x, int y, int w, int h) {
    HWND p = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
                             WS_EX_NOACTIVATE | WS_EX_TOPMOST,
                             L"STATIC", L"t", WS_POPUP,
                             x, y, w, h, NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetLayeredWindowAttributes(p, 0, 1, LWA_ALPHA);
    ShowWindow(p, SW_SHOWNOACTIVATE);
    return p;
}

int main(void) {
    HWND s[8];
    int i, ok;
    EnumWindows(E, 0);
    if (!g_tb) { printf("no tray\n"); return 1; }

    for (i = 0; i < 4; i++) {
        Park();
        s[0] = Mk(0, 0, 2880, 450); s[1] = Mk(0, 450, 2880, 450);
        s[2] = Mk(0, 900, 2880, 450); s[3] = Mk(0, 1350, 2880, 450);
        Reveal();
        ok = Top();
        DestroyWindow(s[0]); DestroyWindow(s[1]);
        DestroyWindow(s[2]); DestroyWindow(s[3]);
        printf("4x450 strips -> raise=%d\n", ok);
        fflush(stdout);
    }
    for (i = 0; i < 2; i++) {
        Park();
        s[0] = Mk(0, 0, 2880, 1800);
        Reveal();
        ok = Top();
        DestroyWindow(s[0]);
        printf("control single full -> raise=%d\n", ok);
        fflush(stdout);
    }
    return 0;
}
