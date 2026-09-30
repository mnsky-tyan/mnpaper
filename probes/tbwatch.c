/* tbwatch.c - watch the taskbar's topmost bit and rank through a reveal */
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

static void Snap(const char *when) {
    int rk_tb = -1, rk_veil = -1, k = 0;
    LONG ex;
    RECT r;
    HWND h;
    if (!g_tb) { printf("no taskbar window\n"); return; }
    ex = GetWindowLongW(g_tb, GWL_EXSTYLE);
    GetWindowRect(g_tb, &r);
    h = GetTopWindow(NULL);
    while (h) {
        if (h == g_tb) rk_tb = k;
        if (h == g_veil) rk_veil = k;
        k++;
        h = GetWindow(h, GW_HWNDNEXT);
    }
    printf("%-10s tb-rank=%4d veil-rank=%4d tb-ex=%08lx topmost=%d rect.top=%ld %s\n",
           when, rk_tb, rk_veil, ex, (int)((ex & WS_EX_TOPMOST) != 0), r.top,
           (r.top < 848 ? "REVEALED" : "parked"));
    fflush(stdout);
}

int main(void) {
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int i;
    EnumWindows(E, 0);
    if (!g_tb) { printf("no Shell_TrayWnd found\n"); return 1; }
    printf("veil hwnd=%p taskbar hwnd=%p\n", (void*)g_veil, (void*)g_tb);
    Snap("before");
    /* reveal: wiggle then hold on the taskbar band */
    SetCursorPos(sw / 2, sh - 1);  Sleep(150);
    SetCursorPos(sw / 2 - 60, sh - 2); Sleep(100);
    SetCursorPos(sw / 2 + 40, sh - 1); Sleep(100);
    /* hold the cursor ON the bottom sliver: the reveal latches and holds */
    for (i = 0; i < 3; i++) {
        SetCursorPos(sw / 2 + 40, sh - 2); Sleep(80);
        SetCursorPos(sw / 2 - 20, sh - 1); Sleep(80);
        SetCursorPos(sw / 2 + 60, sh - 2); Sleep(80);
    }
    SetCursorPos(sw / 2 + 10, sh - 2);
    for (i = 0; i < 24; i++) {
        Snap("reveal");
        Sleep(150);
    }
    /* leave the strip: taskbar should park and lose topmost */
    SetCursorPos(sw / 2, 400);
    for (i = 0; i < 10; i++) {
        Snap("leave");
        Sleep(200);
    }
    return 0;
}
