/* tbtest9.c - CLEAN geometry bisection: probe-owned windows only, no app */
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
    if (Top()) {   /* bit can linger: click the desktop to settle it */
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

static void Trial(const char *name, int n, const int *xs, const int *ys,
                  const int *ws, const int *hs) {
    HWND w[8];
    int i, ok = 0;
    for (i = 0; i < n; i++) w[i] = Mk(xs[i], ys[i], ws[i], hs[i]);
    Reveal();
    ok = Top();
    for (i = 0; i < n; i++) DestroyWindow(w[i]);
    printf("%-30s -> raise=%d\n", name, ok);
    fflush(stdout);
    Park();
}

int main(void) {
    int xs[8], ys[8], ws[8], hs[8];
    EnumWindows(E, 0);
    if (!g_tb) { printf("no tray\n"); return 1; }
    Park();
    Reveal(); printf("%-30s -> raise=%d (sanity)\n", "no window", Top()); fflush(stdout);
    Park();

    memset(xs, 0, sizeof(xs)); memset(ys, 0, sizeof(ys));
    ws[0] = 2880; hs[0] = 1800;
    Trial("single 2880x1800", 1, xs, ys, ws, hs);
    ws[0] = 2880; hs[0] = 1000;
    Trial("single 2880x1000", 1, xs, ys, ws, hs);
    ws[0] = 2880; hs[0] = 900;
    Trial("single 2880x900", 1, xs, ys, ws, hs);
    ws[0] = 2880; hs[0] = 800;
    Trial("single 2880x800", 1, xs, ys, ws, hs);
    ws[0] = 2880; hs[0] = 400;
    Trial("single 2880x400", 1, xs, ys, ws, hs);

    /* stacked pairs covering the full screen */
    ws[0] = 2880; hs[0] = 900;  ys[0] = 0;
    ws[1] = 2880; hs[1] = 900;  ys[1] = 900; xs[1] = 0;
    Trial("stacked 2x 2880x900", 2, xs, ys, ws, hs);
    ws[0] = 2880; hs[0] = 600;  ys[0] = 0;
    ws[1] = 2880; hs[1] = 600;  ys[1] = 600;
    ws[2] = 2880; hs[2] = 600;  ys[2] = 1200; xs[2] = 0;
    Trial("stacked 3x 2880x600", 3, xs, ys, ws, hs);

    /* width control to confirm strict mode is on */
    ws[0] = 2000; hs[0] = 1800; ys[0] = 0;
    Trial("single 2000x1800 (width ctl)", 1, xs, ys, ws, hs);
    return 0;
}
