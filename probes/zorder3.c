/* zorder3.c - clean dump: mnPaper processes, overlay/mascot windows, ranks */
#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")

static HWND g_ov[8], g_mas;
static int g_nov;

static void Cls(HWND h, char *out, int cap) {
    GetClassNameA(h, out, cap);
}

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    WCHAR cls[64];
    char buf[128];
    RECT r;
    DWORD pid = 0;
    GetClassNameW(h, cls, 64);
    GetWindowThreadProcessId(h, &pid);
    if (lstrcmpiW(cls, L"MnPaperOverlay") == 0 && g_nov < 8)
        g_ov[g_nov++] = h;
    if (lstrcmpiW(cls, L"Qt5152QWindowToolSaveBits") == 0 && IsWindowVisible(h)) {
        GetWindowRect(h, &r);
        if (r.left < 500 && r.top > 400 && !g_mas) g_mas = h;
    }
    (void)buf; (void)Cls;
    return TRUE;
}

int main(void) {
    int i;
    char buf[128];
    EnumWindows(EnumProc, 0);
    printf("-- overlay windows: %d --\n", g_nov);
    for (i = 0; i < g_nov; i++) {
        RECT r; DWORD pid = 0; LONG ex;
        GetWindowRect(g_ov[i], &r);
        GetWindowThreadProcessId(g_ov[i], &pid);
        ex = GetWindowLongW(g_ov[i], GWL_EXSTYLE);
        Cls(g_ov[i], buf, 128);
        printf("  hwnd=%p pid=%lu ex=%08lx cls=%s rect=(%ld,%ld)-(%ld,%ld)\n",
               (void*)g_ov[i], pid, ex, buf, r.left, r.top, r.right, r.bottom);
    }
    if (g_mas) {
        RECT r; DWORD pid = 0; LONG ex;
        GetWindowRect(g_mas, &r);
        GetWindowThreadProcessId(g_mas, &pid);
        ex = GetWindowLongW(g_mas, GWL_EXSTYLE);
        Cls(g_mas, buf, 128);
        printf("-- mascot --\n  hwnd=%p pid=%lu ex=%08lx cls=%s rect=(%ld,%ld)-(%ld,%ld)\n",
               (void*)g_mas, pid, ex, buf, r.left, r.top, r.right, r.bottom);
    }
    /* ranks */
    printf("-- ranks --\n");
    for (i = 0; i < g_nov; i++) {
        HWND h = GetTopWindow(NULL);
        int rank = -1, k = 0;
        while (h) {
            if (h == g_ov[i]) { rank = k; break; }
            k++; h = GetWindow(h, GW_HWNDNEXT);
        }
        printf("  overlay %d rank=%d\n", i, rank);
    }
    if (g_mas) {
        HWND h = GetTopWindow(NULL);
        int rank = -1, k = 0;
        while (h) {
            if (h == g_mas) { rank = k; break; }
            k++; h = GetWindow(h, GW_HWNDNEXT);
        }
        printf("  mascot rank=%d\n", rank);
    }
    printf("-- first 6 in z-order --\n");
    {
        HWND h = GetTopWindow(NULL);
        int k;
        for (k = 0; k < 6 && h; k++, h = GetWindow(h, GW_HWNDNEXT)) {
            RECT r; DWORD pid = 0;
            GetWindowRect(h, &r);
            GetWindowThreadProcessId(h, &pid);
            Cls(h, buf, 128);
            printf("  %d hwnd=%p pid=%lu vis=%d rect=(%ld,%ld)-(%ld,%ld) cls=%s\n",
                   k, (void*)h, pid, IsWindowVisible(h), r.left, r.top, r.right, r.bottom, buf);
        }
    }
    return 0;
}
