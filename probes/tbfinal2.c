/* tbfinal2.c - end-to-end summon test with SendInput (LL-hook-visible) */
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

static struct { HWND t; int n; int found; } g_rc;

static BOOL CALLBACK RankCb(HWND h, LPARAM lp) {
    if (IsWindowVisible(h)) {
        if (h == g_rc.t) { g_rc.found = g_rc.n; return FALSE; }
        g_rc.n++;
    }
    return TRUE;
}

static int Rank(HWND target) {
    g_rc.t = target; g_rc.n = 0; g_rc.found = -1;
    EnumWindows(RankCb, 0);
    return g_rc.found;
}

static void AbsMove(int x, int y) {
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    in.mi.dx = (x * 65535 + 1439) / 2879;
    in.mi.dy = (y * 65535 + 899) / 899;
    SendInput(1, &in, sizeof(INPUT));
}

int main(void) {
    int i;
    EnumWindows(E, 0);
    if (!g_tb || !g_veil) { printf("missing window\n"); return 1; }

    AbsMove(720, 400);
    Sleep(1200);

    for (i = 0; i < 16; i++) {
        LONG ex; RECT tr, vr;
        int dwell = 250 + i * 60;

        AbsMove(720, 1770); Sleep(40);
        AbsMove(720, 1788); Sleep(40);
        AbsMove(720, 1799); Sleep(dwell);

        ex = GetWindowLongW(g_tb, GWL_EXSTYLE);
        GetWindowRect(g_tb, &tr);
        GetWindowRect(g_veil, &vr);
        printf("t=%4dms tb-top=%d tb-rank=%3d tb-y=%3ld veil=%ldx%ld\n",
               dwell, (int)((ex & WS_EX_TOPMOST) != 0), Rank(g_tb),
               tr.top, vr.right - vr.left, vr.bottom - vr.top);
        fflush(stdout);

        AbsMove(720, 400);
        Sleep(1100);
        ex = GetWindowLongW(g_tb, GWL_EXSTYLE);
        GetWindowRect(g_tb, &tr);
        GetWindowRect(g_veil, &vr);
        printf("  parked: tb-top=%d tb-y=%3ld veil=%ldx%ld\n",
               (int)((ex & WS_EX_TOPMOST) != 0), tr.top,
               vr.right - vr.left, vr.bottom - vr.top);
        fflush(stdout);
    }
    return 0;
}
