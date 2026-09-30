/* hooktest2.c - do SendInput-injected moves traverse WH_MOUSE_LL? */
#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")

static HHOOK g_h;
static volatile LONG g_moves, g_edge;

static LRESULT CALLBACK HookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == WM_MOUSEMOVE) {
        MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
        InterlockedIncrement(&g_moves);
        if (m->pt.y >= 1700) InterlockedIncrement(&g_edge);
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static void AbsMove(int x, int y) {   /* physical, absolute */
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    in.mi.dx = (x * 65535 + 1439) / 2879;   /* 0..65535 across 2880 */
    in.mi.dy = (y * 65535 + 899) / 899;     /* 0..65535 across 1800  */
    SendInput(1, &in, sizeof(INPUT));
}

int main(void) {
    MSG msg;
    int i;
    SetProcessDpiAwarenessContext((DPI_AWARENESS_CONTEXT)-4);
    g_h = SetWindowsHookExW(WH_MOUSE_LL, HookProc, NULL, 0);
    printf("hook installed; SendInput wiggling near bottom edge...\n");
    for (i = 0; i < 6; i++) {
        AbsMove(600 + i * 40, 1798);
        Sleep(60);
        AbsMove(600 + i * 40, 1785);
        Sleep(60);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    printf("moves=%ld edge(y>=1700)=%ld\n", g_moves, g_edge);
    UnhookWindowsHookEx(g_h);
    return 0;
}
