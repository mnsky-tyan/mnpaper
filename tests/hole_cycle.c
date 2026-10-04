/* Reproduces the reported symptom end to end at a fixed 2880x1800 geometry
 * (the reference desktop this suite was written against - nothing here
 * queries the live monitors): reveal the taskbar (hole armed), upload, park
 * it (hole closed), upload again with no rebuild, and assert the bottom band
 * renders paper again. Windowless: only the texture math is used. */
#define _CRT_SECURE_NO_WARNINGS 1
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define wWinMain mnPaper_unused_wWinMain
#include "../mnPaper.c"
#undef wWinMain

static int fails, checks;
static void Check(int c, const char *what) {
    checks++;
    printf("  %s  %s\n", c ? "PASS" : "FAIL", what);
    if (!c) fails++;
}
static unsigned long long H(const unsigned char *p, size_t n) {
    unsigned long long h = 1469598103934665603ULL; size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

int main(void) {
    RECT mon = { 0, 0, 2880, 1800 };          /* fixed reference geometry */
    int w = mon.right - mon.left, h = mon.bottom - mon.top;
    OVL ov;
    unsigned char *px;
    unsigned long long clean, revealed;
    int band_y = 1800 - 48;                    /* inside the taskbar band */
    int i;

    g_test_headless = 1;   /* same safety pin as the other suites */
    printf("taskbar hole cycle on a %dx%d monitor (fixed reference geometry)\n", w, h);
    memset(&ov, 0, sizeof ov);
    ov.idx = 0; ov.w = w; ov.h = h; ov.rc = mon;
    px = (unsigned char *)malloc((size_t)w * h * 4);
    if (!px) { printf("RESULT 1 failure(s)\n"); return 1; }
    ov.bits = px;
    g_s = (SETTINGS){ .master = 1, .mode = MODE_PAPER, .intensity = 40,
                      .warmth = 80, .grain = 4, .fibre = 40, .blotch = 26,
                      .shades = 4, .contrast = 50, .dither = 75,
                      .autostart = 0, .share = 0, .autoupd = 1 };
    BuildPaper(&ov);
    clean = H(px, (size_t)w * h * 4);
    Check(px[(size_t)band_y * w * 4 + 1400 * 4 + 3] != 0,
          "with the taskbar parked the band carries real paper alpha");

    /* taskbar revealed: the hole opens over the bottom band */
    g_hole_on[0] = 1;
    SetRect(&g_hole[0], mon.left, 1752, mon.right, mon.bottom);
    /* this scratch OVL has no strips, so ApplyLayered runs the hole
     * save/punch/restore around a no-op upload (it cannot fail - the
     * return value is not assertable here; the byte-identity hashes below
     * are the real checks). That path is exactly what the 2.7.8 regression
     * lived in. */
    ApplyLayered(&ov);
    revealed = H(px, (size_t)w * h * 4);
    Check(revealed == clean, "the upload for a revealed taskbar changes no master pixel");

    /* taskbar parks: the hole closes, Housekeeping re-uploads with NO rebuild */
    g_hole_on[0] = 0;
    ApplyLayered(&ov);
    Check(H(px, (size_t)w * h * 4) == clean,
          "after the taskbar parks the texture is byte-identical to the clean one");
    {
        int holes = 0;
        for (i = 0; i < 40; i++) {
            int y = mon.bottom - 2 - i, x = 200 + i * 30;
            if (px[(size_t)y * w * 4 + x * 4 + 3] == 0) holes++;
        }
        Check(holes == 0, "no zero-alpha pixel is left in the parked-taskbar band");
    }
    /* and the cycle is repeatable: 20 reveal/park round trips leave no residue */
    for (i = 0; i < 20; i++) {
        g_hole_on[0] = 1; ApplyLayered(&ov);
        g_hole_on[0] = 0; ApplyLayered(&ov);
    }
    Check(H(px, (size_t)w * h * 4) == clean, "20 reveal/park cycles leave the texture untouched");
    free(px);
    printf("\nRESULT %d failure(s) across %d checks\n", fails, checks);
    return fails ? 1 : 0;
}
