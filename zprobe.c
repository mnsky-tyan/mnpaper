/* zprobe.c - enumerate top-level windows in z-order with class/exstyle/rect */
#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")

static const char *Cls(HWND h) {
    static char b[256];
    GetClassNameA(h, b, 256);
    return b;
}

static BOOL CALLBACK EnumProc(HWND h, LPARAM lp) {
    RECT r;
    char title[256] = "";
    DWORD pid = 0;
    int visible = IsWindowVisible(h);
    if (!GetWindowRect(h, &r)) return TRUE;
    GetWindowTextA(h, title, 256);
    GetWindowThreadProcessId(h, &pid);
    if (!visible && !title[0]) return TRUE;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    printf("hwnd=%p pid=%lu vis=%d ex=%08lx cls=%-32s rect=(%ld,%ld)-(%ld,%ld) %ldx%ld title=%s\n",
           (void*)h, pid, visible, ex, Cls(h),
           r.left, r.top, r.right, r.bottom,
           r.right - r.left, r.bottom - r.top, title);
    return TRUE;
}

int main(void) {
    HWND me = GetConsoleWindow();
    printf("probe window: %p\n", (void*)me);
    EnumWindows(EnumProc, 0);
    /* where is the probe in z-order relative to everything? */
    HWND top = GetTopWindow(NULL);
    printf("top window: %p\n", (void*)top);
    return 0;
}
