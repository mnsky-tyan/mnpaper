/* Tall-monitor strip coverage (2026-10-09 review, finding 1).
 *
 * The overlay is built as a stack of horizontal strips, one per STRIP_H rows.
 * The stack used to stop at MAX_STRIPS == 8, i.e. 8 x 450 = 3600px of monitor
 * height; anything taller (a 4K panel in portrait, a 5K panel, an 8K TV) got
 * its bottom band left with no paper over it, silently.
 *
 * This suite drives the REAL MakeOverlay against a set of tall rects and asks
 * the created windows themselves where they are: it walks ov.shwnd[] with
 * GetWindowRect, rebuilds the union of the strip rectangles and checks that
 * union equals the monitor rectangle, top edge to bottom edge, every pixel.
 * Nothing here re-implements the strip arithmetic; the windows are the
 * product's own output.
 *
 * The last case drives a monitor taller than MAX_STRIPS * STRIP_H can cover
 * to prove the boundary is real (the loop genuinely has a cap) and is not
 * silently overrun.
 *
 * The strips MakeOverlay creates carry no WS_VISIBLE and nothing here ever
 * shows one - that structural fact, not the case rects, is what keeps the
 * run invisible (several cases sit at {0,0} on the visible desktop); same
 * hidden-target pattern as the other suites. The registry is left alone
 * (no settings are written). */
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

/* Does the union of ov's real strip windows cover [rc.top, rc.bottom) on the
 * full width, with no gap and no stray window? */
static int StripsCover(OVL *ov, const RECT *rc) {
    int expect_top = rc->top;
    int s;
    if (ov->n_strips <= 0) return 0;
    for (s = 0; s < ov->n_strips; s++) {
        RECT wr;
        int w, h;
        if (!ov->shwnd[s] || !IsWindow(ov->shwnd[s])) return 0;
        if (!GetWindowRect(ov->shwnd[s], &wr)) return 0;
        w = wr.right - wr.left;
        h = wr.bottom - wr.top;
        if (wr.left != rc->left || w != rc->right - rc->left) return 0;
        if (wr.top != expect_top) return 0;        /* contiguous, no gap/overlap */
        if (h <= 0 || h > STRIP_H) return 0;
        expect_top = wr.bottom;
    }
    return expect_top == rc->bottom;               /* reaches the bottom edge */
}

static void RunCase(const char *label, int w, int h, int must_cover) {
    OVL ov;
    RECT rc = { 0, 0, w, h };
    char msg[256];
    MakeOverlay(&ov, NULL, &rc);
    snprintf(msg, sizeof msg, "%s (%dx%d): %d strips, %dpx covered, %dpx tall",
             label, w, h, ov.n_strips, ov.n_strips * STRIP_H, h);
    printf("  .. %s\n", msg);
    if (must_cover) {
        snprintf(msg, sizeof msg, "%s (%dx%d) is fully covered by real strip windows",
                 label, w, h);
        Check(StripsCover(&ov, &rc), msg);
    } else {
        snprintf(msg, sizeof msg, "%s (%dx%d) exceeds the cap, so it is NOT fully covered",
                 label, w, h);
        Check(!StripsCover(&ov, &rc), msg);
        Check(ov.n_strips == MAX_STRIPS, "  .. and the strip count hit MAX_STRIPS");
    }
    DestroyOverlay(&ov);
}

/* Run MakeOverlay on a monitor past the cap with the log pointed at a file
 * and confirm it names the geometry and the covered height. This is the half
 * of the fix that turns an unforeseen panel from silent into diagnosable. */
static void CheckShortfallLogged(void) {
    OVL ov;
    RECT rc = { 12, 34, 1452, 34 + 16000 };
    FILE *f = fopen("tall_coverage.log", "w");
    char buf[512];
    if (!f) { Check(0, "shortfall log file could be opened"); return; }
    g_log = f;
    MakeOverlay(&ov, NULL, &rc);
    g_log = NULL;
    if (f) { fflush(f); fclose(f); }
    f = fopen("tall_coverage.log", "r");
    memset(buf, 0, sizeof buf);
    if (f) { fread(buf, 1, sizeof buf - 1, f); fclose(f); }
    DeleteFileA("tall_coverage.log");   /* no residue: the log was scratch */
    printf("  .. log: %s\n", buf[0] ? buf : "(empty)");
    Check(strstr(buf, "monitor at 12,34") != NULL, "shortfall log names the monitor rect");
    Check(strstr(buf, "16000px tall") != NULL, "shortfall log names the real monitor height");
    Check(strstr(buf, "32 strips cover only 14400px") != NULL,
          "shortfall log names the strips and the height they reach");
    DestroyOverlay(&ov);
}

int main(void) {
    WNDCLASSW wc = {0};

    g_test_headless = 1;   /* windows must never become visible in tests */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitCommonControls();
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpfnWndProc = OvlProc; wc.lpszClassName = L"MnPaperOverlay";
    RegisterClassW(&wc);

    printf("tall-monitor strip coverage (STRIP_H=%d, MAX_STRIPS=%d, cap=%dpx)\n",
           STRIP_H, MAX_STRIPS, STRIP_H * MAX_STRIPS);
    printf("  .. reference desktop the older suites used (2880x1800) must still cover\n");

    /* The failure the review reported: 3840px tall is 8*450=3600px of strips. */
    RunCase("4K portrait", 2160, 3840, 1);
    RunCase("5K portrait", 2880, 5120, 1);
    RunCase("8K TV", 7680, 4320, 1);
    RunCase("very tall panel", 1440, 12000, 1);
    /* Boundary that is intentionally beyond the cap: the guard must not lie. */
    RunCase("beyond cap", 1440, 16000, 0);
    /* ...and the shortfall must be diagnosable, not silent: MakeOverlay's L()
     * must report the exact rect and the covered height into the log. */
    CheckShortfallLogged();
    /* The old reference desktop stays covered (no regression). */
    RunCase("reference 2880x1800", 2880, 1800, 1);

    printf("RESULT %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
