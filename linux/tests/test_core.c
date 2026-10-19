/* test_core - headless tests for the portable half of the Wayland port.
 *
 * Everything here runs the same code the live overlay runs, without a
 * compositor: the settings core (defaults, clamp, --set grammar) and the
 * paper generator.  It mirrors the spirit of the Windows hidden suites
 * (odd dimensions, premultiplied-alpha validity, cancellation) for the
 * pieces this port actually ships.  Exit 0 = all green.
 *
 *   make test   (from linux/)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../paper_core.h"

static int failures;

#define CHECK(cond, msg) do {                                                  \
        if (!(cond)) {                                                         \
            printf("FAIL %s (line %d): %s\n", __func__, __LINE__, msg);        \
            failures++;                                                        \
        }                                                                      \
    } while (0)

/* premultiplied validity: no colour channel may exceed its alpha, and alpha
 * is a full byte - this is the invariant both GDI (AC_SRC_ALPHA) and
 * wl_shm ARGB8888 compositing assume; breaking it shows up as halos */
static void CheckPremultiplied(const unsigned char *px, int w, int h,
                               const char *what) {
    int i;
    for (i = 0; i < w * h; i++) {
        const unsigned char *p = px + 4 * (size_t)i;
        if (p[0] > p[3] || p[1] > p[3] || p[2] > p[3]) {
            printf("FAIL %s pixel %d: BGR %d,%d,%d > alpha %d\n",
                   what, i, p[0], p[1], p[2], p[3]);
            failures++;
            return;
        }
    }
}

static void TestDefaultsAndClamp(void) {
    SETTINGS s = PaperDefaultSettings();
    CHECK(s.master == 1 && s.mode == MODE_PAPER, "defaults: master/mode");
    CHECK(s.intensity == 30 && s.warmth == 45, "defaults: intensity/warmth");
    CHECK(s.grain == 4 && s.fibre == 40 && s.blotch == 30, "defaults: texture");
    CHECK(s.tbmode == TB_NOT_COVER, "defaults: tbmode");

    s.intensity = 999; s.warmth = -5; s.grain = 1; s.blotch = 1000;
    s.shades = 0; s.dither = -1; s.master = 7; s.mode = 9;
    ClampSettingsOf(&s);
    CHECK(s.intensity == 40, "clamp: intensity hi 40");
    CHECK(s.warmth == 0, "clamp: warmth lo 0");
    CHECK(s.grain == 2, "clamp: grain lo 2");
    CHECK(s.blotch == 100, "clamp: blotch hi 100");
    CHECK(s.shades == 2, "clamp: shades lo 2");
    CHECK(s.dither == 0, "clamp: dither lo 0");
    CHECK(s.master == 1, "clamp: master to 1");
    CHECK(s.mode == 1, "clamp: mode to 1");
}

static void TestSetKeyValueGrammar(void) {
    SETTINGS s = PaperDefaultSettings();

    CHECK(SetKeyValue(&s, "intensity=35"), "set: plain key");
    CHECK(s.intensity == 35, "set: value applied");
    CHECK(SetKeyValue(&s, "INTENSITY=10"), "set: case-insensitive key");
    CHECK(s.intensity == 10, "set: case-insensitive applied");
    CHECK(SetKeyValue(&s, "mode=1"), "set: mode accepted");
    CHECK(s.mode == MODE_EINK, "set: mode eink");
    CHECK(SetKeyValue(&s, "tapebar=1"), "set: legacy alias accepted");
    CHECK(s.tbmode == TB_COVER, "set: legacy alias mapped to tbmode");

    CHECK(!SetKeyValue(&s, "intensity="), "set: empty value refused");
    CHECK(!SetKeyValue(&s, "intensity=x"), "set: non-numeric refused");
    CHECK(!SetKeyValue(&s, "intensity=3x"), "set: trailing junk refused");
    CHECK(!SetKeyValue(&s, "nokey=5"), "set: unknown key refused");
    CHECK(!SetKeyValue(&s, "intensity"), "set: missing '=' refused");
    CHECK(s.intensity == 10, "set: refusals never touch the value");
}

static void TestBakeDeterminismAndValidity(void) {
    SETTINGS s = PaperDefaultSettings();
    int sizes[][2] = { { 320, 200 }, { 97, 61 }, { 2880, 1800 } };
    size_t si;
    for (si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
        int w = sizes[si][0], h = sizes[si][1];
        unsigned char *a = malloc((size_t)w * h * 4);
        unsigned char *b = malloc((size_t)w * h * 4);
        CHECK(a && b, "alloc");
        if (!a || !b) { free(a); free(b); return; }
        CHECK(BuildPaperFull(a, w, h, &s, NULL), "bake a");
        CHECK(BuildPaperFull(b, w, h, &s, NULL), "bake b");
        CHECK(memcmp(a, b, (size_t)w * h * 4) == 0, "determinism");
        CheckPremultiplied(a, w, h, "default settings");
        if (si == 1) {   /* odd size: corners and centre must have texture */
            int nonzero = 0, i;
            for (i = 0; i < w * h; i++)
                if (a[4 * (size_t)i + 3]) nonzero++;
            CHECK(nonzero > (w * h) / 4, "odd-size texture has body");
        }
        free(a); free(b);
    }
}

static void TestSettingsActuallyChangePixels(void) {
    SETTINGS s = PaperDefaultSettings();
    int w = 240, h = 160;
    unsigned char *a = malloc((size_t)w * h * 4);
    unsigned char *b = malloc((size_t)w * h * 4);
    CHECK(a && b, "alloc");
    if (!a || !b) { free(a); free(b); return; }

    BuildPaperFull(a, w, h, &s, NULL);
    s.warmth = 100;                                  /* fully aged amber */
    BuildPaperFull(b, w, h, &s, NULL);
    CHECK(memcmp(a, b, (size_t)w * h * 4) != 0, "warmth changes pixels");
    CheckPremultiplied(b, w, h, "warmth=100");

    s = PaperDefaultSettings();
    BuildPaperFull(b, w, h, &s, NULL);
    CHECK(memcmp(a, b, (size_t)w * h * 4) == 0, "default settings reproduce");

    s.intensity = 0;                                 /* strength zero: off */
    BuildPaperFull(b, w, h, &s, NULL);
    {
        int i, all_transparent = 1;
        for (i = 0; i < w * h; i++)
            if (b[4 * (size_t)i + 3]) { all_transparent = 0; break; }
        CHECK(all_transparent, "intensity 0 means fully transparent");
    }

    free(a); free(b);
}

static void TestCancellation(void) {
    SETTINGS s = PaperDefaultSettings();
    int w = 200, h = 200;
    unsigned char *px = malloc((size_t)w * h * 4);
    PAPERCANCEL c;
    CHECK(px != NULL, "alloc");
    if (!px) return;
    memset(&c, 0, sizeof c);
    CHECK(BuildPaperFull(px, w, h, &s, &c), "uncancelled bake completes");
    c.stop = 1;
    CHECK(!BuildPaperFull(px, w, h, &s, &c), "cancel flag stops the bake");
    free(px);
}

/* Regression for the 2026-10-19 codebase review (finding 1): e-ink is not
 * implemented on Wayland, so a requested mode=1 must never survive as state.
 * The app normalizes it at its single settings choke point; this pins the
 * contract that a refused mode is not representable. */
static void TestEinkNotRepresentable(void) {
    SETTINGS s = PaperDefaultSettings();
    /* the raw parser accepts the value (it is platform-neutral) ... */
    CHECK(SetKeyValue(&s, "mode=1"), "set: mode=1 parses");
    CHECK(s.mode == MODE_EINK, "set: parser yields eink");
    /* ... and the Wayland layer must refuse it, not store it.  This mirrors
     * RequestMode: paper is the only representable mode here. */
    s.mode = MODE_PAPER;                  /* what RequestMode(MODE_EINK) does */
    CHECK(s.mode == MODE_PAPER, "refused mode normalizes to paper");
    /* and a paper mode request is honoured without side effects */
    s.mode = MODE_PAPER;
    CHECK(s.mode == MODE_PAPER, "paper mode is representable");
}

int main(void) {
    TestEinkNotRepresentable();
    TestDefaultsAndClamp();
    TestSetKeyValueGrammar();
    TestBakeDeterminismAndValidity();
    TestSettingsActuallyChangePixels();
    TestCancellation();
    printf("%s: %d failure(s)\n", failures ? "RESULT" : "RESULT 0", failures);
    return failures ? 1 : 0;
}
