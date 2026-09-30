/*
 * poc.c - Paperman feasibility proof-of-concept.
 *
 * Claim under test: a full-screen "paper texture" over everything is what a
 * WS_EX_LAYERED / WS_EX_TRANSPARENT / WS_EX_TOOLWINDOW / WS_EX_NOACTIVATE /
 * WS_EX_TOPMOST popup with a per-pixel-alpha (UpdateLayeredWindow) procedural
 * texture buys us - no screen capture, no hooking, no per-frame work.
 *
 * Self-checks printed to stdout:
 *   - one overlay window per monitor, rect == monitor rect
 *   - layered/transparent/noactivate/toolwindow/topmost style bits
 *   - click-through: WindowFromPoint + WM_NCHITTEST (the hit must fall through
 *     to the window below and DefWindowProc must report HTTRANSPARENT)
 *   - click-through over the taskbar as well
 *   - screen capture before/after -> mean pixel delta proves the texture
 *     actually composites (and how strongly)
 *   - idle CPU while the message loop spins, plus working set
 *
 * Build (MSVC already present on this machine):
 *   cl /nologo /O2 /W3 poc.c
 * Run:
 *   poc.exe [--ms 15000] [--alpha 30] [--out DIR]
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS 1
#include <windows.h>
#include <shellapi.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "psapi.lib")

#define MAX_MON 16

typedef struct {
    int      idx;
    HWND     hwnd;
    RECT     rc;
    int      w, h;
    HDC      mem;
    HBITMAP  dib;
    void    *bits;
} OVERLAY;

static OVERLAY g_ov[MAX_MON];
static int     g_n;

/* ---------------------------------------------------------------- noise --- */

static unsigned lh(int x, int y, int seed) {
    unsigned h = (unsigned)(x * 73856093 ^ y * 19349663 ^ seed * 83492791);
    h = (h ^ (h >> 15)) * 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return h;
}

static const float R32 = 2.3283064365386963e-10f; /* 1/2^32 */

/* value noise on a lattice that wraps every p cells (keeps tiles seamless) */
static float vnoise(float x, float y, int p, int seed) {
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    fx = fx * fx * (3.f - 2.f * fx);
    fy = fy * fy * (3.f - 2.f * fy);
    int xa = ((x0 % p) + p) % p, ya = ((y0 % p) + p) % p;
    int xb = (xa + 1) % p,        yb = (ya + 1) % p;
    float v00 = lh(xa, ya, seed) * R32, v10 = lh(xb, ya, seed) * R32;
    float v01 = lh(xa, yb, seed) * R32, v11 = lh(xb, yb, seed) * R32;
    float a = v00 + (v10 - v00) * fx, b = v01 + (v11 - v01) * fx;
    return a + (b - a) * fy;                       /* 0..1 */
}

static float fbm(float x, float y, int p) {
    float sum = 0.f, amp = 0.5f, tot = 0.f;
    int i;
    for (i = 0; i < 4; i++) {
        sum += amp * vnoise(x, y, p, i * 101 + 7);
        tot += amp;
        amp *= 0.5f;
        x *= 2.03f; y *= 2.01f; p *= 2;
    }
    return sum / tot;                              /* 0..1 */
}

static int NextPow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

/* -------------------------------------------------------------- texture --- */

/* Paper = warm off-white + fine grain + faint stretched fibres + slow
 * macro mottling. Everything lands in a very low alpha band (0..maxAlpha)
 * so the display keeps its content, only loses its digital flatness. */
static void BuildTexture(OVERLAY *ov, int maxAlpha) {
    unsigned char *px = (unsigned char *)ov->bits;
    int w = ov->w, h = ov->h;
    int pg = NextPow2(w / 4 + 2);   /* grain,  ~4 texel cells   */
    int pf = NextPow2(w / 2 + 2);   /* fibres, ~2 texel wide    */
    int pm = NextPow2(w / 90 + 2);  /* macro blotches           */
    int x, y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float u = (float)x, v = (float)y;
            float grain = fbm(u / 4.f,  v / 4.f,  pg);
            float fibre = fbm(u / 2.f,  v / 12.f, pf);
            float macro = fbm(u / 90.f, v / 70.f, pm);
            float a = grain * 0.45f + fibre * 0.30f + macro * 0.25f;
            int   ai = (int)(a * (float)maxAlpha);
            if (ai < 0) ai = 0;
            if (ai > 255) ai = 255;
            {
                unsigned char *p = px + 4 * ((size_t)y * w + x);
                p[0] = (unsigned char)(235 * ai / 255); /* B */
                p[1] = (unsigned char)(248 * ai / 255); /* G */
                p[2] = (unsigned char)(255 * ai / 255); /* R (warm white) */
                p[3] = (unsigned char)ai;               /* A (premultiplied) */
            }
        }
    }
}

static void Apply(OVERLAY *ov) {
    HDC  screen = GetDC(NULL);
    SIZE size   = { ov->w, ov->h };
    POINT src   = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(ov->hwnd, screen, NULL, &size,
                        ov->mem, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* pass 1: geometry only, no windows */
static BOOL CALLBACK MonRect(HMONITOR hm, HDC hdc, LPRECT rc, LPARAM lp) {
    if (g_n >= MAX_MON) return FALSE;
    g_ov[g_n].idx = g_n;
    g_ov[g_n].hwnd = NULL;
    g_ov[g_n].rc = *rc;
    g_ov[g_n].w = rc->right - rc->left;
    g_ov[g_n].h = rc->bottom - rc->top;
    g_n++;
    return TRUE;
}

/* pass 2: real overlay window per monitor, honouring the rect from pass 1 */
static BOOL CALLBACK MonMake(HMONITOR hm, HDC hdc, LPRECT rc, LPARAM lp) {
    static int i;
    OVERLAY *ov;
    if (i >= g_n) return FALSE;
    ov = &g_ov[i++];
    ov->rc = *rc;
    ov->w = rc->right - rc->left;
    ov->h = rc->bottom - rc->top;

    ov->hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
        WS_EX_NOACTIVATE | WS_EX_TOPMOST,
        L"MnPaperOverlay", L"paper-poc", WS_POPUP | WS_VISIBLE,
        rc->left, rc->top, ov->w, ov->h,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!ov->hwnd) return TRUE;

    {
        BITMAPINFO bi;
        HDC screen = GetDC(NULL);
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = ov->w;
        bi.bmiHeader.biHeight = -ov->h;        /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        ov->dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &ov->bits, NULL, 0);
        ov->mem = CreateCompatibleDC(screen);
        SelectObject(ov->mem, ov->dib);
        ReleaseDC(NULL, screen);
    }
    return TRUE;
}

/* ------------------------------------------------------------- bmp dump --- */

static int Stride(int w) { return ((w * 3 + 3) / 4) * 4; }

static void SaveBmp(const char *path, HDC screen, const RECT *r) {
    int w = r->right - r->left, h = r->bottom - r->top;
    int stride = Stride(w);
    BITMAPINFO bi;
    void *bits = NULL;
    HDC mem;
    HBITMAP bmp;
    HGDIOBJ old;
    BITMAPFILEHEADER fh;
    FILE *f;
    unsigned char *buf;

    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;
    mem = CreateCompatibleDC(screen);
    bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, r->left, r->top, SRCCOPY);
    SelectObject(mem, old);
    DeleteDC(mem);

    buf = (unsigned char *)malloc(stride * h);
    memcpy(buf, bits, stride * h);
    DeleteObject(bmp);

    memset(&fh, 0, sizeof fh);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + stride * h;
    f = fopen(path, "wb");
    if (f) {
        fwrite(&fh, 1, sizeof fh, f);
        fwrite(&bi.bmiHeader, 1, sizeof(BITMAPINFOHEADER), f);
        fwrite(buf, 1, stride * h, f);
        fclose(f);
    }
    free(buf);
}

static void DiffBmps(const char *pa, const char *pb) {
    FILE *fa = fopen(pa, "rb"), *fb = fopen(pb, "rb");
    if (!fa || !fb) {
        if (fa) fclose(fa);
        if (fb) fclose(fb);
        printf("(missing capture)\n");
        return;
    }
    {
        BITMAPFILEHEADER fha, fhb;
        BITMAPINFOHEADER iha, ihb;
        int w, h, stride, x, y, n = 0, chg = 0;
        double sum = 0, mx = 0;
        unsigned char *A, *B;
        size_t off;

        fread(&fha, 1, sizeof fha, fa);
        fread(&fhb, 1, sizeof fhb, fb);
        fread(&iha, 1, sizeof iha, fa);
        fread(&ihb, 1, sizeof ihb, fb);
        w = iha.biWidth; h = iha.biHeight; stride = Stride(w);
        off = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fseek(fa, (long)off, SEEK_SET);
        fseek(fb, (long)off, SEEK_SET);
        A = (unsigned char *)malloc(stride * h);
        B = (unsigned char *)malloc(stride * h);
        fread(A, 1, stride * h, fa);
        fread(B, 1, stride * h, fb);
        for (y = 0; y < h; y++) {
            for (x = 0; x < w * 3; x++) {
                int d = abs(A[y * stride + x] - B[y * stride + x]);
                sum += d; n++;
                if (d > 2) chg++;
                if (d > mx) mx = d;
            }
        }
        free(A); free(B);
        printf("mean|delta|=%.3f  max|delta|=%g  pixels_changed(>2)=%d/%d (%.2f%%)\n",
               sum / n, mx, chg, n, 100.0 * chg / n);
    }
    fclose(fa); fclose(fb);
}

/* ------------------------------------------------------------- timing --- */

static double CpuPctSince(ULARGE_INTEGER cpu0, ULARGE_INTEGER wall0) {
    FILETIME fc, fe, fk, fu, fnow;
    ULARGE_INTEGER cpu1, wall1, k, u;

    GetProcessTimes(GetCurrentProcess(), &fc, &fe, &fk, &fu);
    k.LowPart = fk.dwLowDateTime; k.HighPart = fk.dwHighDateTime;
    u.LowPart = fu.dwLowDateTime; u.HighPart = fu.dwHighDateTime;
    cpu1.QuadPart = k.QuadPart + u.QuadPart;

    GetSystemTimeAsFileTime(&fnow);
    wall1.LowPart = fnow.dwLowDateTime; wall1.HighPart = fnow.dwHighDateTime;

    {
        double dt = (double)(wall1.QuadPart - wall0.QuadPart) / 1e7;
        double used = (double)(cpu1.QuadPart - cpu0.QuadPart) / 1e7;
        if (dt <= 0) return 0.0;
        return 100.0 * used / dt;
    }
}

/* ---------------------------------------------------------------- main --- */

int main(int argc, char **argv) {
    int ms = 15000, alpha = 30, i;
    const char *out = ".";
    WNDCLASSW wc;
    HDC screen;
    char path[512];
    FILETIME fc, fe, fk, fu, fnow;
    ULARGE_INTEGER cpu0, wall0;
    PROCESS_MEMORY_COUNTERS pmc;
    MSG msg;
    DWORD start;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ms") && i + 1 < argc) ms = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--alpha") && i + 1 < argc) alpha = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    }

    screen = GetDC(NULL);
    printf("== environment ==\n");
    printf("virtual_screen=%dx%d at (%d,%d)  depth=%dbit  dpi_x=%d\n",
           GetSystemMetrics(SM_CXVIRTUALSCREEN),
           GetSystemMetrics(SM_CYVIRTUALSCREEN),
           GetSystemMetrics(SM_XVIRTUALSCREEN),
           GetSystemMetrics(SM_YVIRTUALSCREEN),
           GetDeviceCaps(screen, BITSPIXEL) * GetDeviceCaps(screen, PLANES),
           GetDeviceCaps(screen, LOGPIXELSX));

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"MnPaperOverlay";
    RegisterClassW(&wc);

    /* pass 1: geometry only */
    EnumDisplayMonitors(NULL, NULL, MonRect, 0);
    printf("monitors=%d\n", g_n);

    /* baseline capture before any overlay exists */
    printf("\n== baseline capture (no overlay) ==\n");
    for (i = 0; i < g_n; i++) {
        sprintf(path, "%s/base_%d.bmp", out, i);
        SaveBmp(path, screen, &g_ov[i].rc);
        printf("wrote %s (%dx%d)\n", path, g_ov[i].w, g_ov[i].h);
    }

    /* pass 2: real overlay windows */
    EnumDisplayMonitors(NULL, NULL, MonMake, 0);
    printf("\n== overlays: max_alpha=%d/255 (%.1f%%) ==\n",
           alpha, 100.0 * alpha / 255.0);

    for (i = 0; i < g_n; i++) {
        OVERLAY *ov = &g_ov[i];
        LONG ex;
        RECT rc;
        POINT c;
        HWND hit;
        WCHAR cls[256] = L"?";
        LRESULT ht;
        HWND taskbar;
        RECT trayrc;

        if (!ov->hwnd) continue;
        BuildTexture(ov, alpha);
        Apply(ov);

        GetWindowRect(ov->hwnd, &rc);
        ex = GetWindowLongW(ov->hwnd, GWL_EXSTYLE);
        printf("\n[monitor %d] %dx%d rect=(%ld,%ld)-(%ld,%ld)\n",
               ov->idx, ov->w, ov->h, rc.left, rc.top, rc.right, rc.bottom);
        printf("  exstyle=%08lx LAYERED=%ld TRANSPARENT=%ld TOOLWINDOW=%ld NOACTIVATE=%ld TOPMOST=%ld\n",
               (unsigned long)ex,
               (long)((ex & WS_EX_LAYERED) != 0),
               (long)((ex & WS_EX_TRANSPARENT) != 0),
               (long)((ex & WS_EX_TOOLWINDOW) != 0),
               (long)((ex & WS_EX_NOACTIVATE) != 0),
               (long)((ex & WS_EX_TOPMOST) != 0));

        c.x = (rc.left + rc.right) / 2;
        c.y = (rc.top + rc.bottom) / 2;
        hit = WindowFromPoint(c);
        if (hit) GetClassNameW(hit, cls, 128);
        printf("  WindowFromPoint(center)=%p class=%S -> %s\n", (void *)hit, cls,
               hit == ov->hwnd ? "HIT OVERLAY (click-through FAILED)"
                               : "fell through (click-through OK)");

        ht = SendMessageW(ov->hwnd, WM_NCHITTEST, 0,
                          MAKELPARAM(c.x - rc.left, c.y - rc.top));
        printf("  WM_NCHITTEST=%ld (HTTRANSPARENT=%d) %s\n", (long)ht,
               HTTRANSPARENT,
               ht == HTTRANSPARENT ? "transparent OK" : "NOT TRANSPARENT");

        taskbar = FindWindowW(L"Shell_TrayWnd", NULL);
        if (taskbar && GetWindowRect(taskbar, &trayrc)) {
            POINT t;
            t.x = (trayrc.left + trayrc.right) / 2;
            t.y = (trayrc.top + trayrc.bottom) / 2;
            hit = WindowFromPoint(t);
            if (hit) GetClassNameW(hit, cls, 128);
            printf("  taskbar point=%p class=%S -> %s\n", (void *)hit, cls,
                   hit == ov->hwnd ? "overlay swallowed taskbar clicks"
                                   : "taskbar still clickable (OK)");
        }

        sprintf(path, "%s/ov_%d.bmp", out, i);
        SaveBmp(path, screen, &rc);
        printf("  wrote %s\n", path);
    }

    printf("\n== composited delta (before vs after) ==\n");
    for (i = 0; i < g_n; i++) {
        char pb[512];
        sprintf(path, "%s/base_%d.bmp", out, i);
        sprintf(pb, "%s/ov_%d.bmp", out, i);
        printf("monitor %d: ", i);
        DiffBmps(path, pb);
    }

    GetProcessTimes(GetCurrentProcess(), &fc, &fe, &fk, &fu);
    cpu0.LowPart = fk.dwLowDateTime; cpu0.HighPart = fk.dwHighDateTime;
    {
        ULARGE_INTEGER u0;
        u0.LowPart = fu.dwLowDateTime; u0.HighPart = fu.dwHighDateTime;
        cpu0.QuadPart += u0.QuadPart;
    }
    GetSystemTimeAsFileTime(&fnow);
    wall0.LowPart = fnow.dwLowDateTime; wall0.HighPart = fnow.dwHighDateTime;

    start = GetTickCount();
    printf("\n== idle %d ms (message loop, texture static) ==\n", ms);
    while (GetTickCount() - start < (DWORD)ms) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(25);
    }
    printf("idle_cpu=%.2f%% of one core\n", CpuPctSince(cpu0, wall0));

    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc))
        printf("working_set=%.2f MiB\n", pmc.WorkingSetSize / 1048576.0);
    printf("texture_memory=%d MiB (bitmaps held by DWM/this proc)\n",
           g_ov[0].w * g_ov[0].h * 4 / 1048576);

    for (i = 0; i < g_n; i++) {
        if (g_ov[i].hwnd) DestroyWindow(g_ov[i].hwnd);
    }
    printf("\nDONE\n");
    return 0;
}
