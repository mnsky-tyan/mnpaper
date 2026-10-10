/* Independent verification of the three audit fixes, driven from the real
 * production code (mnPaper.c is #included), windowless, isolated registry. */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define CINTERFACE
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* mnPaper.c defines its own WINVER/UNICODE/CINTERFACE and a wWinMain; include
   it with a renamed entry point so this verifier owns main() instead. */
#define wWinMain mnPaper_unused_wWinMain
#include "../mnPaper.c"
#undef wWinMain

/* shared full-byte hash (tests/fnv64.h); PixelHash8 kept as the local name */
#include "fnv64.h"
#define PixelHash8 PixelHashAll

static int fails, checks;
static void Check(int c, const char *what) {
    checks++;
    printf("  %s  %s\n", c ? "PASS" : "FAIL", what);
    if (!c) fails++;
}

int main(void) {
    RECT rc;
    WCHAR scratch[128], runkey[160];
    printf("VERIFIER STARTED\n"); fflush(stdout);
    ULONGLONG t0, t1;
    HKEY k;
    LONG r;

    g_test_headless = 1;   /* same safety pin as the other suites */
    /* isolated scratch hive, PID-suffixed like the other suites: never the
     * captain's live settings, and REG_KEY is redirected too, so even a
     * SaveSettings lands in the scratch tree. The Run key gets the same
     * PID suffix so two concurrent runs (or a crashed one) cannot collide. */
    swprintf(scratch, 128, L"Software\\mnPaperVerifyFixes-%lu",
             (unsigned long)GetCurrentProcessId());
    r = RegCreateKeyExW(HKEY_CURRENT_USER, scratch, 0, NULL,
                        0, KEY_ALL_ACCESS, NULL, &k, NULL);
    if (r == ERROR_SUCCESS) RegCloseKey(k);
    REG_KEY = scratch;
    swprintf(runkey, 160, L"Software\\mnPaperVerifyFixes-Run-%lu",
             (unsigned long)GetCurrentProcessId());
    lstrcpynW(g_run_key, runkey, 160);
    g_s = (SETTINGS){ .master = 1, .mode = MODE_PAPER, .intensity = 40,
                      .warmth = 0, .grain = 4, .fibre = 40, .blotch = 26,
                      .shades = 4, .contrast = 50, .dither = 75,
                      .autostart = 0, .share = 0, .autoupd = 1 };

    printf("FIX 1 (F1): a body of exactly the cap must not be refused\n");
    /* FeedFits is the one cap decision HttpGetToMem makes per chunk; driving
     * it directly replaces the old block here, which re-implemented the
     * arithmetic in the test and compared a variable to what it had just
     * assigned it (the 2026-10-03 review's "tautology" finding). */
    {
        const DWORD max_bytes = 8u * 1024u * 1024u;
        Check(FeedFits(0, 10, max_bytes) &&
              FeedFits(max_bytes - 10, 10, max_bytes),
              "a chunk ending exactly at the cap is accepted (complete, legitimate)");
        Check(!FeedFits(max_bytes - 9, 10, max_bytes),
              "a chunk that would cross the cap by one byte is refused");
        Check(FeedFits(0, max_bytes, max_bytes) &&
              !FeedFits(1, max_bytes, max_bytes),
              "the cap boundary is the stop line, not one byte past it");
    }
    printf("\nFIX 2 (B1): showing the overlay must not run the full-screen build\n");
    /* The production sequence is what RepaintAll does: PaperWorkerStop FIRST
     * (parking g_stop_thread at 1), then the show path. The old guard here
     * called SeedPaperPreview as a bare function with no worker state, so the
     * production sequence never occurred in the harness and the suite stayed
     * green while the show path cancelled its own preview every time and fell
     * back to the full uncancellable build on the UI thread (found by the
     * 2026-10-03 review, present since 2.7.7). */
    {
        unsigned gen;
        int w = 1200, h = 800;
        OVL ov;
        volatile LONG epoch;
        unsigned char *px;
        ULONGLONG armed;
        memset(&ov, 0, sizeof ov);
        ov.w = w; ov.h = h; ov.idx = 0;
        ov.bits = malloc((size_t)w * h * 4);
        ov.mem = NULL;                                  /* no real upload target */
        px = (unsigned char *)ov.bits;
        if (!px) { Check(0, "scratch texture allocated"); goto done; }
        memset(px, 0, (size_t)w * h * 4);               /* known background */
        armed = PixelHash8(px, (size_t)w * h * 4);

        /* the exact state RepaintAll hands the show path: worker stopped */
        InterlockedExchange(&g_stop_thread, 1);
        gen = g_pgen;
        t0 = GetTickCount64();
        SeedPaperPreview(&ov);                          /* what ShowOverlay now calls */
        t1 = GetTickCount64();
        Check(PixelHash8(px, (size_t)w * h * 4) != armed,
              "with the worker stopped, the show path still renders (no silent full-build fallback)");
        printf("  METRIC preview paint %llums for 1200x800\n",
               (unsigned long long)(t1 - t0));
        Check(t1 - t0 < 1500,
              "the show path's paint is nowhere near a full build (< 1.5s budget, "
              "generous on purpose - correctness is the hash check above)");
        Check(g_pgen == gen, "the show path queues no render generation of its own");

        /* the cancel contract itself. The stop flag is cleared FIRST: with
         * it parked at 1, PaperCancelled's "stop_flag || epoch moved" || is
         * forced true by the flag alone, so the epoch half is unexercised
         * and could regress to a no-op without this suite noticing
         * (2026-10-04 review). The uncancellable check above still proved
         * NULL-epoch completes while the flag was parked. */
        Check(BuildPaperPreview(px, w, h, &g_s, NULL, 0) == 1,
              "an uncancellable preview completes even with the stop flag parked");
        InterlockedExchange(&g_stop_thread, 0);
        epoch = 41;
        Check(BuildPaperPreview(px, w, h, &g_s, &epoch, 7) == 0,
              "a preview whose generation moved on cancels (epoch half of the cancel, flag clear)");
        Check(BuildPaperPreview(px, w, h, &g_s, &epoch, 41) == 1,
              "an unmoved generation completes (epoch == expected, flag clear)");

        /* the full build, for comparison, is the expensive one */
        t0 = GetTickCount64();
        BuildPaper(&ov);
        t1 = GetTickCount64();
        printf("  (full build of the same monitor took %llums - that is what the UI thread used to do)\n",
               (unsigned long long)(t1 - t0));
        free(ov.bits);
    }
    printf("\nFIX 3 (X1/X2/X5): one hole rule, one owner rule\n");
    {
        OVL ov;
        int w = 200, h = 200, x, y;
        unsigned char *px;
        memset(&ov, 0, sizeof ov);
        ov.w = w; ov.h = h; ov.idx = 0;
        px = malloc((size_t)w * h * 4);
        ov.bits = px;
        if (!px) { Check(0, "FIX 3 scratch allocated"); goto done; }
        for (x = 0; x < w * h; x++) { px[4*x] = 10; px[4*x+1] = 20; px[4*x+2] = 30; px[4*x+3] = 255; }
        ULONGLONG before;
        unsigned char *saved = NULL;
        size_t saved_len = 0;
        int rx0=0, ry0=0, rx1=0, ry1=0;
        g_hole_on[0] = 1;
        SetRect(&g_hole[0], 0, 100, 200, 200);
        before = PixelHash8(px, (size_t)w * h * 4);
        ClearHoleAlpha(&ov, &saved, &saved_len, &rx0, &ry0, &rx1, &ry1);
        Check(px[(size_t)150 * w * 4 + 50 * 4 + 3] == 0, "ClearHoleAlpha zeroes alpha inside the hole");
        Check(px[(size_t)50 * w * 4 + 50 * 4 + 3] == 255, "ClearHoleAlpha leaves alpha outside the hole untouched");
        Check(saved != NULL && saved_len > 0, "ClearHoleAlpha saves the pixels it overwrote");
        RestoreHoleAlpha(&ov, saved, saved_len, rx0, ry0, rx1, ry1);
        Check(PixelHash8(px, (size_t)w * h * 4) == before,
              "RestoreHoleAlpha puts the texture back byte-for-byte (no permanent band)");
        /* the restore put the punched pixel's own alpha back (the hash check
         * above already proves it for every byte; this names the property in
         * the bug report's own terms) */
        Check(px[(size_t)150 * w * 4 + 50 * 4 + 3] == 255,
              "RestoreHoleAlpha leaves the punched pixel back at its own alpha");
        free(px);
    }
    {
        /* the owner rule: a dead handle must come back as NULL, not be used */
        HWND dead = (HWND)(INT_PTR)0x1234;   /* never a live window here */
        g_dlg = dead;
        Check(LiveDlg() == NULL, "LiveDlg() refuses a stale dialog handle");
        g_dlg = NULL;
        Check(LiveDlg() == NULL, "LiveDlg() is NULL with no dialog");
    }
done:
    /* leave no scratch behind: the 2026-10-03 review caught this suite
     * leaking its key into the captain's real hive on every run, and the
     * allocation-failure bail-outs above must not either */
    RegDeleteTreeW(HKEY_CURRENT_USER, scratch);
    RegDeleteTreeW(HKEY_CURRENT_USER, runkey);

    printf("\nRESULT %d failure(s) across %d checks\n", fails, checks);
    return fails ? 1 : 0;
}
