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
#include <ctype.h>

/* mnPaper.c defines its own WINVER/UNICODE/CINTERFACE and a wWinMain; include
   it with a renamed entry point so this verifier owns main() instead. */
#define wWinMain mnPaper_unused_wWinMain
#include "../mnPaper.c"
#undef wWinMain

static char *ReadWholeFile(const char *path, size_t *len_out) {
    FILE *f = fopen(path, "rb");
    long sz;
    char *b;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    b = (char *)malloc((size_t)sz + 1);
    if (!b) { fclose(f); return NULL; }
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    b[sz] = 0;
    fclose(f);
    if (len_out) *len_out = (size_t)sz;
    return b;
}

static unsigned long long PixelHash8(const unsigned char *p, size_t bytes) {
    unsigned long long h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < bytes; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static size_t CountOccurrences(const char *hay, const char *needle) {
    size_t n = 0, l = strlen(needle);
    const char *p = hay;
    while ((p = strstr(p, needle)) != NULL) { n++; p += l; }
    return n;
}

static int fails, checks;
static void Check(int c, const char *what) {
    checks++;
    printf("  %s  %s\n", c ? "PASS" : "FAIL", what);
    if (!c) fails++;
}

/* One guarded read of the product source. The 2026-10-03 review caught two
 * scans here that PASSED VACUOUSLY when the test was started from any
 * directory other than the paper root: fopen failed, the needles were never
 * searched, and !found passed. Reading once, through this helper, with a
 * hard length check, makes that state impossible. */
static char *g_src;
static size_t g_src_len;
static char *ReadProductSource(void) {
    if (!g_src) {
        g_src = ReadWholeFile("mnPaper.c", &g_src_len);
        Check(g_src != NULL && g_src_len > 100000,
              "the product source was read whole (scans are not vacuous)");
    }
    return g_src;
}
/* whitespace-normalized needle match: a re-indent must not disarm a guard */
static int SourceHas(const char *needle) {
    char *src = ReadProductSource();
    char *hay, *q;
    const char *p;
    int found;
    if (!src) return 0;
    hay = (char *)malloc(strlen(src) + 1);
    q = hay;
    for (p = src; *p; p++)
        if (!isspace((unsigned char)*p)) *q++ = *p;
    *q = 0;
    {
        char *n2 = (char *)malloc(strlen(needle) + 1), *w = n2;
        for (p = needle; *p; p++)
            if (!isspace((unsigned char)*p)) *w++ = *p;
        *w = 0;
        found = strstr(hay, n2) != NULL;
        free(n2);
    }
    free(hay);
    return found;
}

int main(void) {
    RECT rc;
    WCHAR scratch[128];
    printf("VERIFIER STARTED\n"); fflush(stdout);
    ULONGLONG t0, t1;
    HKEY k;
    LONG r;

    g_test_headless = 1;   /* same safety pin as the other suites */
    /* isolated scratch hive, PID-suffixed like the other suites: never the
     * captain's live settings, and REG_KEY is redirected too, so even a
     * SaveSettings lands in the scratch tree */
    swprintf(scratch, 128, L"Software\\mnPaperVerifyFixes-%lu",
             (unsigned long)GetCurrentProcessId());
    r = RegCreateKeyExW(HKEY_CURRENT_USER, scratch, 0, NULL,
                        0, KEY_ALL_ACCESS, NULL, &k, NULL);
    if (r == ERROR_SUCCESS) RegCloseKey(k);
    REG_KEY = scratch;
    wcscpy(g_run_key, L"Software\\mnPaperVerifyFixes-Run");
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
    /* and the same the other way: the code no longer has the >= check */
    Check(!SourceHas("if (total >= max_bytes)"),
          "the double-counting 'total >= max_bytes' refusal is gone");

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
        if (!px) { Check(0, "scratch texture allocated"); return 1; }
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
        Check(t1 - t0 < 60, "the show path's paint is cheap (< 60ms for 1200x800)");
        Check(g_pgen == gen, "the show path queues no render generation of its own");

        /* the cancel contract itself: NULL epoch is uncancellable, a moved
         * epoch cancels - the worker's preemption still has to work */
        Check(BuildPaperPreview(px, w, h, &g_s, NULL, 0) == 1,
              "an uncancellable preview completes even with the stop flag parked");
        epoch = 41;
        Check(BuildPaperPreview(px, w, h, &g_s, &epoch, 7) == 0,
              "a preview whose generation moved on cancels (worker preemption preserved)");
        InterlockedExchange(&g_stop_thread, 0);

        /* the full build, for comparison, is the expensive one */
        t0 = GetTickCount64();
        BuildPaper(&ov);
        t1 = GetTickCount64();
        printf("  (full build of the same monitor took %llums - that is what the UI thread used to do)\n",
               (unsigned long long)(t1 - t0));
        free(ov.bits);
    }
    /* the show path renders the preview helper, never the full build */
    Check(SourceHas("MODE_PAPER) SeedPaperPreview(ov);"),
          "ShowOverlay lays down SeedPaperPreview, not BuildPaper");
    Check(!SourceHas("MODE_PAPER) BuildPaper(ov);"),
          "ShowOverlay no longer calls BuildPaper (the synchronous full build)");

    printf("\nFIX 3 (X1/X2/X5): one hole rule, one owner rule\n");
    {
        OVL ov;
        int w = 200, h = 200, x, y;
        unsigned char *px;
        memset(&ov, 0, sizeof ov);
        ov.w = w; ov.h = h; ov.idx = 0;
        px = malloc((size_t)w * h * 4);
        ov.bits = px;
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
        /* and the property the bug violated: an upload must never leave the
         * master texture mutated, because Housekeeping re-uploads without a
         * rebuild when the taskbar parks */
        Check(px[(size_t)150 * w * 4 + 50 * 4 + 3] == 255,
              "the master texture keeps its own alpha after a hole upload");        free(px);
    }
    {
        /* the owner rule: a dead handle must come back as NULL, not be used */
        HWND dead = (HWND)(INT_PTR)0x1234;   /* never a live window here */
        g_dlg = dead;
        Check(LiveDlg() == NULL, "LiveDlg() refuses a stale dialog handle");
        g_dlg = NULL;
        Check(LiveDlg() == NULL, "LiveDlg() is NULL with no dialog");
    }
    Check(ReadProductSource() &&
          (int)CountOccurrences(ReadProductSource(),
                                "(g_dlg && IsWindow(g_dlg)) ? g_dlg : NULL") == 1,
          "exactly one owner ternary remains (inside LiveDlg)");

    /* leave no scratch behind: the 2026-10-03 review caught this suite
     * leaking its key into the captain's real hive on every run */
    RegDeleteTreeW(HKEY_CURRENT_USER, scratch);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\mnPaperVerifyFixes-Run");
    free(g_src);

    printf("\nRESULT %d failure(s) across %d checks\n", fails, checks);
    return fails ? 1 : 0;
}
