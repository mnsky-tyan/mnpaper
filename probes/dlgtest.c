/* dlgtest.c - verify live drag preview + hold-to-compare in the real app */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <math.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static HWND g_dlg, g_tb, g_cmp;

static BOOL CALLBACK ChildCb(HWND h, LPARAM lp) {
    WCHAR cls[64];
    GetClassNameW(h, cls, 64);
    if (!g_tb && lstrcmpiW(cls, L"msctls_trackbar32") == 0) g_tb = h;
    if (!g_cmp && lstrcmpiW(cls, L"Button") == 0) {
        WCHAR txt[64];
        GetWindowTextW(h, txt, 64);
        if (wcsstr(txt, L"compare")) g_cmp = h;
    }
    return TRUE;
}

/* stdev of a 240x240 screen region (physical, top-left area) */
static double TexRough(void) {
    HDC sc = GetDC(NULL), mem = CreateCompatibleDC(sc);
    HBITMAP bm, old;
    BITMAPINFO bi;
    static unsigned char px[240 * 240 * 4];
    double sum = 0, sum2 = 0;
    int x, y, n = 0;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 240; bi.bmiHeader.biHeight = -240;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bm = CreateCompatibleBitmap(sc, 240, 240);
    old = SelectObject(mem, bm);
    BitBlt(mem, 0, 0, 240, 240, sc, 100, 100, SRCCOPY);
    GetDIBits(mem, bm, 0, 240, px, &bi, DIB_RGB_COLORS);
    SelectObject(mem, old);
    DeleteObject(bm); DeleteDC(mem); ReleaseDC(NULL, sc);
    for (y = 8; y < 232; y += 4)
        for (x = 8; x < 232; x += 4) {
            double v = (px[4 * (y * 240 + x)] + px[4 * (y * 240 + x) + 1] +
                        px[4 * (y * 240 + x) + 2]) / 3.0;
            sum += v; sum2 += v * v; n++;
        }
    if (n < 2) return -1;
    return sqrt(sum2 / n - (sum / n) * (sum / n));
}

int main(void) {
    int step;
    double r0, r[6];
    SetProcessDpiAwarenessContext((DPI_AWARENESS_CONTEXT)-4);
    g_dlg = FindWindowW(L"MnPaperSettings", NULL);
    if (!g_dlg) { printf("settings window not open\n"); return 1; }
    EnumChildWindows(g_dlg, ChildCb, 0);
    if (!g_tb || !g_cmp) { printf("controls not found tb=%p cmp=%p\n", (void*)g_tb, (void*)g_cmp); return 1; }
    printf("dialog=%p trackbar=%p compare=%p\n", (void*)g_dlg, (void*)g_tb, (void*)g_cmp);

    /* park at 30, measure, then DRAG down to 6 with 100ms steps
     * (no pause >= 400ms anywhere: the old debounce would never fire) */
    SendMessageW(g_tb, TBM_SETPOS, TRUE, 30);
    SendMessageW(g_dlg, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, 0), (LPARAM)g_tb);
    Sleep(900);
    r0 = TexRough();
    printf("parked@30 rough=%.2f\n", r0);
    for (step = 0; step < 6; step++) {
        int pos = 30 - step * 4;      /* 30,26,22,18,14,10 */
        SendMessageW(g_tb, TBM_SETPOS, TRUE, pos);
        SendMessageW(g_dlg, WM_HSCROLL, MAKEWPARAM(TB_THUMBTRACK, 0), (LPARAM)g_tb);
        Sleep(100);                   /* mid-drag, no release yet */
        r[step] = TexRough();
    }
    printf("drag 30->10 roughness per step: %.2f %.2f %.2f %.2f %.2f %.2f\n",
           r[0], r[1], r[2], r[3], r[4], r[5]);
    /* release */
    SendMessageW(g_tb, TBM_SETPOS, TRUE, 10);
    SendMessageW(g_dlg, WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, 0), (LPARAM)g_tb);
    Sleep(600);
    printf("released@10 rough=%.2f\n", TexRough());

    /* compare button: hold = raw screen (less rough), release = back */
    SendMessageW(g_cmp, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
    Sleep(500);
    printf("compare-held rough=%.2f\n", TexRough());
    SendMessageW(g_cmp, WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
    Sleep(700);
    printf("compare-released rough=%.2f\n", TexRough());

    /* restore slider */
    SendMessageW(g_tb, TBM_SETPOS, TRUE, 30);
    SendMessageW(g_dlg, WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, 0), (LPARAM)g_tb);
    return 0;
}
