/* tbstate.c - report taskbar rect/state; optional un-wedge attempts */
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

static void Snap(const char *tag) {
    RECT rc; LONG ex = GetWindowLongW(g_tb, GWL_EXSTYLE);
    GetWindowRect(g_tb, &rc);
    printf("%-18s rect=(%ld,%ld)-(%ld,%ld) top=%d fg=%d\n", tag,
           rc.left, rc.top, rc.right, rc.bottom,
           (int)((ex & WS_EX_TOPMOST) != 0),
           (int)(GetForegroundWindow() == g_tb));
    fflush(stdout);
}

static void AbsMoveClick(int x, int y) {
    INPUT in[2];
    memset(in, 0, sizeof(in));
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = 0x8001;
    in[0].mi.dx = x * 65535 / 2879; in[0].mi.dy = y * 65535 / 1799;
    in[1] = in[0];
    in[1].mi.dwFlags |= 0x0002;   /* LEFTUP */
    in[0].mi.dwFlags |= 0x0001;   /* LEFTDOWN follows move */
    SendInput(2, in, sizeof(INPUT));
}

int main(int argc, char **argv) {
    EnumWindows(E, 0);
    if (!g_tb) { printf("no tray\n"); return 1; }
    Snap("start");
    if (argc > 1 && argv[1][0] == 'u') {
        SetCursorPos(720, 400);
        Sleep(2500);
        Snap("cursor-away 2.5s");
        AbsMoveClick(720, 300);          /* click the desktop */
        Sleep(1200);
        Snap("after click");
        SetCursorPos(720, 800);
        Sleep(800);
        Snap("end");
    }
    return 0;
}
