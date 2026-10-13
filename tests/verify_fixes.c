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
    printf("\nFIX 3 (X5): the owner rule\n");
    /* The hole-rule half of FIX 3 (X1/X2) died with the hole machinery in
     * 2.7.19: the taskbar modes punch no transparency any more, so there is
     * no save/restore pair left to pin. Its invariant - taskbar state can
     * never alter texture bytes - lives in tests/tbzorder.c now. The owner
     * rule below is unchanged and still load-bearing. */
    {
        /* the owner rule: a dead handle must come back as NULL, not be used */
        HWND dead = (HWND)(INT_PTR)0x1234;   /* never a live window here */
        g_dlg = dead;
        Check(LiveDlg() == NULL, "LiveDlg() refuses a stale dialog handle");
        g_dlg = NULL;
        Check(LiveDlg() == NULL, "LiveDlg() is NULL with no dialog");
    }
    printf("\nFIX (2026-10-11): the --set parser accepts, clamps and refuses\n");
    /* SetKeyValue is the one door every `mnPaper --set key=value` walks
     * through; until now it was exercised only live. Its real contract:
     * keys case-insensitive, values DIGITS ONLY (anything else is refused
     * so a typo cannot become a silent 0), booleans folded v?1:0. Range
     * clamping is mostly ApplySets' job - it runs ClampSettingsOf after the
     * parse, and both steps are driven here on a scratch SETTINGS - but
     * tbmode clamps inside the parser as well, so its bound is asserted
     * below rather than assumed. */
    {
        SETTINGS p = g_s;
        int saw;   /* SetKeyValue returns nonzero for a recognised key */

        saw = SetKeyValue(&p, L"tbmode=1");
        Check(saw && p.tbmode == TB_COVER, "tbmode=1 is accepted and stored");
        saw = SetKeyValue(&p, L"tbmode=0");
        Check(saw && p.tbmode == TB_NOT_COVER, "tbmode=0 sets not cover");
        saw = SetKeyValue(&p, L"tbmode=9");
        Check(saw && p.tbmode == TB_COVER, "tbmode above 1 clamps to cover");
        saw = SetKeyValue(&p, L"tapebar=1");
        Check(saw && p.tbmode == TB_COVER, "legacy tapebar=1 means cover");
        saw = SetKeyValue(&p, L"tapebar=0");
        Check(saw && p.tbmode == TB_NOT_COVER, "legacy tapebar=0 means not cover");

        p.intensity = 1;
        saw = SetKeyValue(&p, L"intensity=999");
        Check(saw && p.intensity == 999, "intensity=999 parses unclamped");
        ClampSettingsOf(&p);
        Check(p.intensity == 40, "ApplySets' clamp pass then caps it at 40");

        p.shades = 4;
        SetKeyValue(&p, L"shades=99");
        ClampSettingsOf(&p);
        Check(p.shades == 16, "shades=99 clamps to the 16-level cap");

        p.intensity = 1;
        Check(!SetKeyValue(&p, L"intensity=-5") && p.intensity == 1,
              "a negative value is refused, not parsed or stored");
        Check(!SetKeyValue(&p, L"intensity=abc") && p.intensity == 1,
              "a non-numeric value is refused, not stored as 0");

        p.master = 1;
        saw = SetKeyValue(&p, L"master=0");
        Check(saw && p.master == 0, "master=0 toggles the master switch");

        p.mode = MODE_PAPER;
        saw = SetKeyValue(&p, L"mode=1");
        Check(saw && p.mode == MODE_EINK, "mode=1 switches to e-ink");
        saw = SetKeyValue(&p, L"MODE=0");
        Check(saw && p.mode == MODE_PAPER, "MODE=0 (any case) switches to paper");

        Check(!SetKeyValue(&p, L"nonsense=7"),
              "an unknown key is refused, not silently stored");
        Check(!SetKeyValue(&p, L"novalue"),
              "a key with no = is refused");
        Check(!SetKeyValue(&p, L"master="),
              "a key with an empty value is refused");
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
