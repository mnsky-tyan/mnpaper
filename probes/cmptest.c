#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <commctrl.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static HWND g_tb, g_cmp;

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
    BitBlt(mem, 0, 0, 240, 240, sc, 2000, 1200, SRCCOPY);
    GetDIBits(mem, bm, 0, 240, px, &bi, DIB_RGB_COLORS);
    SelectObject(mem, old);
    DeleteObject(bm); DeleteDC(mem); ReleaseDC(NULL, sc);
    for (y = 8; y < 232; y += 4)
        for (x = 8; x < 232; x += 4) {
            double v = (px[4 * (y * 240 + x)] + px[4 * (y * 240 + x) + 1] +
                        px[4 * (y * 240 + x) + 2]) / 3.0;
            sum += v; sum2 += v * v; n++;
        }
    return n < 2 ? -1 : sqrt(sum2 / n - (sum / n) * (sum / n));
}

int main(void) {
    HWND d = FindWindowW(L"MnPaperSettings", NULL);
    if (!d) { printf("no dlg\n"); return 1; }
    EnumChildWindows(d, ChildCb, 0);
    SendMessageW(g_tb, TBM_SETPOS, TRUE, 30);
    SendMessageW(d, WM_HSCROLL, MAKEWPARAM(TB_ENDTRACK, 0), (LPARAM)g_tb);
    Sleep(1200);
    printf("veiled@30    rough=%.2f\n", TexRough());
    SendMessageW(g_cmp, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
    Sleep(600);
    printf("held-raw     rough=%.2f\n", TexRough());
    SendMessageW(g_cmp, WM_LBUTTONUP, 0, MAKELPARAM(10, 10));
    Sleep(900);
    printf("released@30  rough=%.2f\n", TexRough());
    return 0;
}
