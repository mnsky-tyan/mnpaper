/* strip.c - who covers the bottom strip while the taskbar is revealed? */
#include <windows.h>
#include <stdio.h>
#pragma comment(lib, "user32.lib")

static int g_rank;
static RECT g_strip;
static HWND g_tb, g_veil;

static BOOL CALLBACK E(HWND h, LPARAM lp) {
    WCHAR cls[80], title[80] = L"";
    RECT r;
    DWORD pid = 0;
    LONG ex;
    int vis;
    g_rank++;
    ex = GetWindowLongW(h, GWL_EXSTYLE);
    vis = IsWindowVisible(h);
    GetWindowRect(h, &r);
    GetClassNameW(h, cls, 80);
    GetWindowTextW(h, title, 80);
    GetWindowThreadProcessId(h, &pid);
    if (lstrcmpiW(cls, L"Shell_TrayWnd") == 0) g_tb = h;
    if (lstrcmpiW(cls, L"MnPaperOverlay") == 0) g_veil = h;
    /* report anything that intersects the strip and is visible, or the key windows */
    if ((vis && r.right > g_strip.left && r.left < g_strip.right &&
         r.bottom > g_strip.top && r.top < g_strip.bottom) ||
        h == g_tb || h == g_veil) {
        wprintf(L"rank=%3d %s hwnd=%p pid=%lu ex=%08lx vis=%d rect=(%ld,%ld)-(%ld,%ld) cls=%s title=%s\n",
                g_rank,
                (h == g_tb ? L"TASKBAR" : h == g_veil ? L"VEIL" : L"        "),
                (void*)h, pid, ex, vis, r.left, r.top, r.right, r.bottom, cls, title);
    }
    return TRUE;
}

int main(void) {
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    /* strip = revealed taskbar band, full width */
    g_strip.left = 0; g_strip.right = sw; g_strip.top = sh - 104; g_strip.bottom = sh;
    wprintf(L"screen %dx%d, strip=%ld..%ld\n", sw, sh, g_strip.top, g_strip.bottom);
    /* force the reveal first */
    SetCursorPos(sw / 2, sh - 1); Sleep(200);
    SetCursorPos(sw / 2 - 60, sh - 2); Sleep(120);
    SetCursorPos(sw / 2 + 40, sh - 1); Sleep(120);
    SetCursorPos(sw / 2 + 40, sh - 6); Sleep(900);
    wprintf(L"--- z-order walk ---\n");
    g_rank = 0;
    EnumWindows(E, 0);
    if (g_tb) {
        RECT r;
        GetWindowRect(g_tb, &r);
        wprintf(L"taskbar rect=(%ld,%ld)-(%ld,%ld) %s\n", r.left, r.top, r.right, r.bottom,
                r.top < sh - 100 ? L"REVEALED" : L"parked");
    }
    return 0;
}
