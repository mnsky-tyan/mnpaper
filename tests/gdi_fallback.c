/* Windowless regression for the GDI capture fallback (used when DXGI desktop
 * duplication is unavailable - remote sessions, secure desktop, some VMs).
 *
 * Two properties, both broken by the version the codebase review found:
 *
 *   1. EXACTNESS - change detection must see a change anywhere on screen.
 *      The old sampled hash read one pixel in 97, so 96/97ths of the screen
 *      was invisible to it: an edit landing between samples left e-ink stale
 *      until an unrelated change happened to hit a sampled pixel. A single
 *      differing pixel at an "unsampled" coordinate must be detected.
 *
 *   2. PERSISTENCE - the DIB and DC must be created once, not per tick. The
 *      poller runs at 20 Hz, so a create/destroy pair per tick was a GDI
 *      alloc plus kernel round-trip every 50 ms.
 *
 * The poller BitBlts the real desktop corner into a small buffer; nothing is
 * shown, no window is created, the user's desktop is only read.
 */
#define wWinMain MnPaperEntry
#include "../mnPaper.c"
#undef wWinMain

static int failures;
static void Check(int ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

int main(void) {
    size_t n;
    HBITMAP bmp_before;
    unsigned char *bits_before;
    size_t probe = ((size_t)1 * 64 + 3) * 4 + 2;   /* row 1, pixel 3, blue */
    unsigned char live;

    unsigned char *dst;
    g_test_headless = 1;
    /* a small capture: the properties under test do not depend on size, and a
     * 64x48 grab is quick anywhere the test runs */
    g_vsx = g_vsy = 0;
    g_vsw = 64; g_vsh = 48;
    dst = (unsigned char *)malloc((size_t)g_vsw * g_vsh * 4);
    Check(dst != NULL, "destination capture buffer allocated");
    if (!dst) { printf("RESULT %d failure(s)\n", failures); return 1; }
    memset(dst, 0x80, (size_t)g_vsw * g_vsh * 4);
    n = (size_t)g_vsw * g_vsh * 4;

    /* first call allocates the DIB/DC/shadow and grabs */
    GdiPoll(dst);                      /* allocation; result is desktop-dependent */
    Check(g_gdi_bmp != NULL && g_gdi_dc != NULL && g_gdi_prev != NULL,
          "grab DIB, DC and previous buffer exist");
    memset(g_gdi_prev, 0xFF, n);    /* force the comparison to differ, on any desktop */
    Check(GdiPoll(dst) == 1, "a grab that differs from the shadow reports a change");
    Check(memcmp(dst, g_gdi_bits, n) == 0, "the grab reaches the caller's buffer");

    /* PERSISTENCE: the same DIB and DC survive later ticks. The old check
     * here demanded a quiet screen - a notification or animated tray glyph in
     * the top-left 64x48 failed it on a working desktop (2026-10-03 review).
     * What the suite owns is the reuse property, so that is what is asserted;
     * a live change between grabs is reported, not failed. */
    bmp_before = g_gdi_bmp; bits_before = g_gdi_bits;
    {
        int r1 = GdiPoll(dst), r2 = GdiPoll(dst);
        if (r1 != 0 || r2 != 0)
            printf("NOTE live screen changed under the test (r1=%d r2=%d); the quiet-screen half is informational\n", r1, r2);
    }
    Check(g_gdi_bmp == bmp_before && g_gdi_bits == bits_before,
          "the DIB is reused across ticks, not recreated each 50 ms");

    /* EXACTNESS: shadow one pixel differently from the live grab - exactly
     * what an edit at a coordinate the old 1-in-97 sample never read looked
     * like. The poller must notice and must update its shadow. */
    live = g_gdi_bits[probe];
    g_gdi_prev[probe] = (unsigned char)(live ^ 0x07);
    Check(GdiPoll(dst) == 1, "a change at an unsampled coordinate is still detected");
    Check(memcmp(g_gdi_prev, g_gdi_bits, n) == 0,
          "the shadow is resynced to the live grab after a detected change");
    Check(GdiPoll(dst) == 0, "the resynced poller stays quiet when nothing moves");

    /* a restart (size change, or e-ink shutdown) rebuilds cleanly */
    GdiResetGrabs();
    Check(g_gdi_bmp == NULL && g_gdi_dc == NULL && g_gdi_prev == NULL,
          "reset releases the DIB, the DC and the shadow buffer");
    Check(GdiPoll(dst) == 1, "the next grab after a reset rebuilds and reports");
    /* seed the fresh shadow to a value a real corner is unlikely to hold, so
     * the "reports again" half does not depend on the desktop's pixels */
    memset(g_gdi_prev, 0x5A, n);
    Check(GdiPoll(dst) == 1 && memcmp(g_gdi_prev, g_gdi_bits, n) == 0,
          "the rebuilt poller re-arms from the live grab, whatever it shows");

    /* the e-ink teardown path releases the same objects twice safely */
    GdiResetGrabs();
    GdiResetGrabs();
    Check(g_gdi_prev == NULL, "double reset is safe");
    GdiResetGrabs();
    free(dst);
    printf("RESULT %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
