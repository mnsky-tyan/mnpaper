/* hooktest.c - does a WH_MOUSE_LL hook installed from this thread see
 * injected + real mouse moves while pumping messages? */
#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")

static HHOOK g_h;
static volatile LONG g_moves, g_events, g_edge;

static LRESULT CALLBACK HookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        InterlockedIncrement(&g_events);
        if (wp == WM_MOUSEMOVE) {
            MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
            InterlockedIncrement(&g_moves);
            if (m->pt.y >= 1700) InterlockedIncrement(&g_edge);
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

int main(void) {
    MSG msg;
    int i;
    SetProcessDpiAwarenessContext((DPI_AWARENESS_CONTEXT)-4);
    g_h = SetWindowsHookExW(WH_MOUSE_LL, HookProc, NULL, 0);
    if (!g_h) { printf("hook failed: %lu\n", GetLastError()); return 1; }
    printf("hook installed; wiggling cursor (bottom edge, injected)...\n");
    for (i = 0; i < 6; i++) {
        SetCursorPos(600 + i * 40, 1798);
        Sleep(60);
        SetCursorPos(600 + i * 40, 1785);
        Sleep(60);
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    printf("events=%ld moves=%ld edge(y>=1700)=%ld\n", g_events, g_moves, g_edge);
    UnhookWindowsHookEx(g_h);
    return 0;
}
