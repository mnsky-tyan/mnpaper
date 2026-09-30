/* zorder2.c - find mnPaper overlay + mascot window, try to raise, watch order */
#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")

static HWND g_ov, g_mas;
/* NOTE: one shared static buffer means two Cls() calls in one printf
 * overwrite each other - print one class per line in this family of probes. */
static const char *Cls(HWND h) {
    static char b[128];
    GetClassNameA(h, b, 128);
    return b;
}

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    WCHAR cls[64];
    int visible = IsWindowVisible(h);
    GetClassNameW(h, cls, 64);
    if (lstrcmpiW(cls, L"MnPaperOverlay") == 0) g_ov = h;
    if (lstrcmpiW(cls, L"Qt5152QWindowToolSaveBits") == 0 && visible) {
        RECT r; GetWindowRect(h, &r);
        if (r.left < 500 && r.top > 400) g_mas = h;   /* bottom-left = mascot */
    }
    return TRUE;
}

static void Order(const char *when) {
    HWND h = GetTopWindow(NULL);
    int rank_ov = -1, rank_mas = -1, i = 0;
    while (h) {
        if (h == g_ov) rank_ov = i;
        if (h == g_mas) rank_mas = i;
        i++;
        h = GetWindow(h, GW_HWNDNEXT);
    }
    printf("%-14s rank: overlay=%d mascot=%d  (0 = top, lower number wins)\n", when, rank_ov, rank_mas);
}

int main(void) {
    EnumWindows(EnumProc, 0);
    printf("overlay=%p\nmascot=%p\n", (void*)g_ov, (void*)g_mas);
    if (g_ov) printf("overlay class=%s\n", Cls(g_ov));
    if (g_mas) printf("mascot class=%s\n", Cls(g_mas));
    if (!g_ov || !g_mas) { printf("missing window(s)\n"); return 1; }
    Order("before");
    SetWindowPos(g_ov, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    Order("after raise");
    Sleep(1500); Order("after 1.5s");
    Sleep(1500); Order("after 3s");
    Sleep(2500); Order("after 5.5s");
    return 0;
}
