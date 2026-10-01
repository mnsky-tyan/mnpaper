/* zorder4.c - raise overlay, then poll the rank every 250ms for 10s */
#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")

static HWND g_ov, g_mas;

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    WCHAR cls[64];
    GetClassNameW(h, cls, 64);
    if (lstrcmpiW(cls, L"MnPaperOverlay") == 0) g_ov = h;
    if (lstrcmpiW(cls, L"Qt5152QWindowToolSaveBits") == 0 && IsWindowVisible(h)) {
        RECT r; GetWindowRect(h, &r);
        if (r.left < 500 && r.top > 400) g_mas = h;
    }
    return TRUE;
}

static int Rank(HWND target) {
    HWND h = GetTopWindow(NULL);
    int k = 0;
    while (h) {
        if (h == target) return k;
        k++; h = GetWindow(h, GW_HWNDNEXT);
    }
    return -1;
}

int main(void) {
    int i, above = 0, samples = 0, rounds = 0;
    DWORD t0;
    EnumWindows(EnumProc, 0);
    if (!g_ov || !g_mas) { printf("missing: ov=%p mas=%p\n", (void*)g_ov, (void*)g_mas); return 1; }
    SetWindowPos(g_ov, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    t0 = GetTickCount();
    for (i = 0; i < 40; i++) {          /* 40 * 250ms = 10s */
        int ro = Rank(g_ov), rm = Rank(g_mas);
        DWORD dt = GetTickCount() - t0;
        samples++;
        if (rm < ro) {                  /* mascot above the veil */
            above++;
            printf("t=%4lums mascot ABOVE (rank %d vs %d) -> re-raise\n", dt, rm, ro);
            SetWindowPos(g_ov, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            rounds++;
        }
        Sleep(250);
    }
    printf("mascot above in %d/%d samples (%.1f%%), re-raises: %d\n",
           above, samples, 100.0 * above / samples, rounds);
    return 0;
}
