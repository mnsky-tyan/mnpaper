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

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/wait.h>

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

/* ------------------------------------------------------- legibility ----- */

/* The 2026-10-23 review found the whole texture suite could not tell a
 * visible paper from an invisible one: the only "the texture responds"
 * assertion was `memcmp(a,b) != 0`, which a one-pixel change satisfies, and
 * the product shipped at a median alpha of 18/255 with warmth moving 0.9% of
 * pixels by 2 luminance levels.  These helpers measure what a person actually
 * sees: the texture composited over a background, in whole luminance levels.
 *
 * A level here is one unit of the 0..255 luminance the panel receives, so a
 * threshold of 2 is the smallest difference that is reliably visible on a
 * mid-tone area - roughly the Weber limit for a flat field.  They are
 * deliberately floors, not targets: they fail an invisible texture without
 * pinning an exact look, so tuning the generator stays possible. */

static double LumOf(const unsigned char *p) {
    return 0.299 * p[2] + 0.587 * p[1] + 0.114 * p[0];
}

/* Fraction of pixels whose luminance differs by at least `levels` between two
 * composited buffers.  Returns 0..100. */
static double PctDifferingBy(const unsigned char *a, const unsigned char *b,
                             int w, int h, double levels) {
    int i, n = w * h, hit = 0;
    for (i = 0; i < n; i++) {
        double d = LumOf(a + 4 * (size_t)i) - LumOf(b + 4 * (size_t)i);
        if (d < 0) d = -d;
        if (d >= levels) hit++;
    }
    return 100.0 * hit / n;
}

/* Alpha percentile: p in 0..100.  alpha=0 everywhere returns 0. */
static int AlphaPercentile(const unsigned char *px, int w, int h, int p) {
    int cnt[256], i, cum = 0, n = w * h;
    memset(cnt, 0, sizeof cnt);
    for (i = 0; i < n; i++) cnt[px[4 * (size_t)i + 3]]++;
    for (i = 0; i < 256; i++) {
        cum += cnt[i];
        if (cum * 100 >= n * p) return i;
    }
    return 255;
}

/* The default and maximum strengths must both put real paper on the screen.
 * The old mapping peaked at 7% median / 18% peak opacity, which is why every
 * slider felt dead; this pins the fix so it cannot silently regress. */
static void TestTextureIsActuallyVisible(void) {
    int w = 400, h = 300;
    unsigned char *px = malloc((size_t)w * h * 4);
    CHECK(px != NULL, "alloc");
    if (!px) return;

    {
        SETTINGS s = PaperDefaultSettings();       /* intensity 30 */
        BuildPaperFull(px, w, h, 1.f, &s, NULL);
        CHECK(AlphaPercentile(px, w, h, 50) >= 25,
              "default strength puts a visible veil on screen (median alpha)");
        CHECK(AlphaPercentile(px, w, h, 99) >= 45,
              "default strength has visible structure (p99 alpha)");
    }
    {
        SETTINGS s = PaperDefaultSettings();
        s.intensity = 40;                          /* the maximum */
        BuildPaperFull(px, w, h, 1.f, &s, NULL);
        CHECK(AlphaPercentile(px, w, h, 50) >= 35,
              "maximum strength is a real veil, not a haze (median alpha)");
        CHECK(AlphaPercentile(px, w, h, 99) >= 65,
              "maximum strength has real contrast (p99 alpha)");
    }
    /* and the texture, not a flat tint, must carry the veil: the generator's
     * own bias is the floor, so the median has to sit far above it.  The bias
     * is read out of PaperParams rather than recomputed here - a duplicated
     * formula would keep passing no matter what the generator did - and the
     * 3x bar is what fails a bias of intensity*3/4 (median 54, floor 22). */
    {
        SETTINGS s = PaperDefaultSettings();
        int med, floor;
        BuildPaperFull(px, w, h, 1.f, &s, NULL);
        med = AlphaPercentile(px, w, h, 50);
        floor = PaperParams(w, 1.f, &s).bias;       /* the generator's floor */
        CHECK(med >= floor * 3,
              "most of the veil is texture, not a flat bias floor");
    }
    free(px);
}

/* Every control that is documented to change the image must change it
 * PERCEPTIBLY, measured after compositing over a background - the old suite
 * accepted a one-pixel difference.  Backgrounds are light, mid and dark
 * because a texture that only works on dark content is the bug that was
 * reported. */
static void TestSettingsArePerceptible(void) {
    int w = 400, h = 300;
    int bgs[3];
    size_t bi;
    unsigned char *a = malloc((size_t)w * h * 4);
    unsigned char *b = malloc((size_t)w * h * 4);
    CHECK(a && b, "alloc");
    if (!a || !b) { free(a); free(b); return; }

    bgs[0] = 245;   /* a light page - where the old build was invisible */
    bgs[1] = 128;   /* mid grey */
    bgs[2] = 20;    /* a dark terminal */

    for (bi = 0; bi < 3; bi++) {
        int bg = bgs[bi];
        double pct;

        /* fibre: the strongest of the three texture controls */
        {
            SETTINGS s = PaperDefaultSettings();
            s.fibre = 0;  BuildPaperFull(a, w, h, 1.f, &s, NULL);
            s.fibre = 100; BuildPaperFull(b, w, h, 1.f, &s, NULL);
            CompositeOver(a, w, h, bg);
            CompositeOver(b, w, h, bg);
            pct = PctDifferingBy(a, b, w, h, 2.0);
            CHECK(pct >= 15.0,
                  "fibre 0 vs 100 is visibly different on every background");
        }
        /* blotch */
        {
            SETTINGS s = PaperDefaultSettings();
            s.blotch = 0;  BuildPaperFull(a, w, h, 1.f, &s, NULL);
            s.blotch = 100; BuildPaperFull(b, w, h, 1.f, &s, NULL);
            CompositeOver(a, w, h, bg);
            CompositeOver(b, w, h, bg);
            pct = PctDifferingBy(a, b, w, h, 2.0);
            CHECK(pct >= 15.0,
                  "blotch 0 vs 100 is visibly different on every background");
        }
        /* grain changes the structure, so it must move luminance too */
        {
            SETTINGS s = PaperDefaultSettings();
            s.grain = 2;  BuildPaperFull(a, w, h, 1.f, &s, NULL);
            s.grain = 12; BuildPaperFull(b, w, h, 1.f, &s, NULL);
            CompositeOver(a, w, h, bg);
            CompositeOver(b, w, h, bg);
            pct = PctDifferingBy(a, b, w, h, 2.0);
            CHECK(pct >= 15.0,
                  "grain 2 vs 12 is visibly different on every background");
        }
        /* warmth is a hue shift, so luminance alone understates it; require a
         * smaller luminance fraction but also assert the chroma swing below */
        {
            SETTINGS s = PaperDefaultSettings();
            s.warmth = 10; BuildPaperFull(a, w, h, 1.f, &s, NULL);
            s.warmth = 95; BuildPaperFull(b, w, h, 1.f, &s, NULL);
            CompositeOver(a, w, h, bg);
            CompositeOver(b, w, h, bg);
            pct = PctDifferingBy(a, b, w, h, 2.0);
            CHECK(pct >= 10.0,
                  "warmth 10 vs 95 is visibly different on every background");
        }
    }

    /* warmth must also move the RED-BLUE separation monotonically across its
     * range: the old mapping moved it 5.7 levels end to end and was quantised
     * by integer division into a staircase. */
    {
        SETTINGS s = PaperDefaultSettings();
        double prev = 0.0;
        int wt, ok = 1;
        double lo = 0.0, hi = 0.0;
        for (wt = 0; wt <= 100; wt += 20) {
            double sum = 0.0;
            int i;
            s.warmth = wt;
            BuildPaperFull(a, w, h, 1.f, &s, NULL);
            CompositeOver(a, w, h, 245);
            for (i = 0; i < w * h; i++)
                sum += (double)a[4 * (size_t)i + 2] - (double)a[4 * (size_t)i + 0];
            sum /= (double)(w * h);
            if (wt == 0) lo = sum;
            if (wt == 100) hi = sum;
            /* never decreases: cool -> neutral -> warm is one direction */
            if (wt > 0 && sum <= prev) ok = 0;
            prev = sum;
        }
        CHECK(ok, "warmth increases R-B monotonically across its range");
        CHECK(hi - lo >= 12.0,
              "warmth spans a visible chroma range, not a staircase");
    }

    free(a); free(b);
}

static void TestDefaultsAndClamp(void) {
    SETTINGS s = PaperDefaultSettings();
    CHECK(s.master == 1 && s.mode == MODE_PAPER, "defaults: master/mode");
    CHECK(s.intensity == 30 && s.warmth == 45, "defaults: intensity/warmth");
    CHECK(s.grain == 4 && s.fibre == 40 && s.blotch == 30, "defaults: texture");
    CHECK(s.tbmode == TB_NOT_COVER, "defaults: tbmode");
    /* The paper reaches the screen edge by default.  A non-zero default is
     * what used to paint an uncovered band at the bottom on every panel that
     * reserves no space of its own (auto-hide, floating): the band is only
     * ever right for a floating panel, which the user must opt into. */
    CHECK(s.panelgap == 0, "defaults: panelgap 0 (paper reaches the edge)");
    CHECK(s.cfgver == 2, "defaults: cfgver is the current schema");

    s.intensity = 999; s.warmth = -5; s.grain = 1; s.blotch = 1000;
    s.shades = 0; s.dither = -1; s.master = 7; s.mode = 9;
    s.tbmode = TB_COVER + 7; s.panelgap = 5000;
    ClampSettingsOf(&s);
    CHECK(s.intensity == 40, "clamp: intensity hi 40");
    CHECK(s.warmth == 0, "clamp: warmth lo 0");
    CHECK(s.grain == 2, "clamp: grain lo 2");
    CHECK(s.blotch == 100, "clamp: blotch hi 100");
    CHECK(s.shades == 2, "clamp: shades lo 2");
    CHECK(s.dither == 0, "clamp: dither lo 0");
    CHECK(s.master == 1, "clamp: master to 1");
    CHECK(s.mode == 1, "clamp: mode to 1");
    /* the bottom strip the veil leaves for a floating panel: its upper bound
     * is what keeps a bad value from reserving a dead band across the screen,
     * and tbmode's bound is what keeps ApplyExclusiveZone choosing a zone */
    CHECK(s.panelgap == 512, "clamp: panelgap hi 512");
    CHECK(s.tbmode == TB_COVER, "clamp: tbmode to TB_COVER");

    s = PaperDefaultSettings();
    s.panelgap = -1;
    ClampSettingsOf(&s);
    CHECK(s.panelgap == 0, "clamp: panelgap lo 0");
}

static void TestSetKeyValueGrammar(void) {
    SETTINGS s = PaperDefaultSettings();

    CHECK(SetKeyValue(&s, "intensity=35"), "set: plain key");
    CHECK(s.intensity == 35, "set: value applied");
    CHECK(SetKeyValue(&s, "INTENSITY=10"), "set: case-insensitive key");
    CHECK(s.intensity == 10, "set: case-insensitive applied");
    CHECK(SetKeyValue(&s, "mode=1"), "set: mode accepted");
    CHECK(s.mode == MODE_EINK, "set: mode eink");
    CHECK(SetKeyValue(&s, "tbmode=1"), "set: tbmode accepted");
    CHECK(s.tbmode == TB_COVER, "set: tbmode mapped");
    CHECK(SetKeyValue(&s, "panelgap=48"), "set: panelgap accepted");
    CHECK(s.panelgap == 48, "set: panelgap applied");
    /* an out-of-range panelgap is a clamp case, not a refusal: the grammar
     * accepts it and ClampSettingsOf brings it to the nearest bound */
    CHECK(SetKeyValue(&s, "panelgap=5000"), "set: oversized panelgap accepted");
    ClampSettingsOf(&s);
    CHECK(s.panelgap == 512, "set: oversized panelgap clamps to 512");
    CHECK(SetKeyValue(&s, "tbmode=9"), "set: oversized tbmode accepted");
    CHECK(s.tbmode == TB_COVER, "set: any nonzero tbmode maps to cover");
    ClampSettingsOf(&s);
    CHECK(s.tbmode == TB_COVER, "set: oversized tbmode stays in range");

    CHECK(!SetKeyValue(&s, "intensity="), "set: empty value refused");
    CHECK(!SetKeyValue(&s, "intensity=x"), "set: non-numeric refused");
    CHECK(!SetKeyValue(&s, "intensity=3x"), "set: trailing junk refused");
    CHECK(!SetKeyValue(&s, "nokey=5"), "set: unknown key refused");
    CHECK(!SetKeyValue(&s, "intensity"), "set: missing '=' refused");
    /* a value that does not fit an int is refused: atoi would truncate it
     * to 0 (or wrap it negative) and the clamp would then silently pick the
     * opposite endpoint instead of the one that was asked for */
    CHECK(!SetKeyValue(&s, "intensity=4294967296"), "set: value beyond int refused");
    CHECK(!SetKeyValue(&s, "warmth=2147483648"), "set: value beyond int refused");
    CHECK(!SetKeyValue(&s, "grain=99999999999"), "set: value beyond int refused");
    CHECK(s.intensity == 10, "set: refusals never touch the value");
    {
        SETTINGS big = PaperDefaultSettings();
        CHECK(SetKeyValue(&big, "intensity=2147483647"), "set: int max accepted");
        CHECK(big.intensity == 2147483647, "set: int max applied");
    }
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
        CHECK(BuildPaperFull(a, w, h, 1.f, &s, NULL), "bake a");
        CHECK(BuildPaperFull(b, w, h, 1.f, &s, NULL), "bake b");
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

    BuildPaperFull(a, w, h, 1.f, &s, NULL);
    s.warmth = 100;                                  /* fully aged amber */
    BuildPaperFull(b, w, h, 1.f, &s, NULL);
    CHECK(memcmp(a, b, (size_t)w * h * 4) != 0, "warmth changes pixels");
    CheckPremultiplied(b, w, h, "warmth=100");

    s = PaperDefaultSettings();
    BuildPaperFull(b, w, h, 1.f, &s, NULL);
    CHECK(memcmp(a, b, (size_t)w * h * 4) == 0, "default settings reproduce");

    s.intensity = 0;                                 /* strength zero: off */
    BuildPaperFull(b, w, h, 1.f, &s, NULL);
    {
        int i, all_transparent = 1;
        for (i = 0; i < w * h; i++)
            if (b[4 * (size_t)i + 3]) { all_transparent = 0; break; }
        CHECK(all_transparent, "intensity 0 means fully transparent");
    }

    free(a); free(b);
}

/* Strength sliders must scale VISIBILITY, never repaint the pattern.  The
 * light/dark structure belongs to the grain field alone; fibre and blotch are
 * pure alpha overlays.  The property that matters: the UN-premultiplied
 * colour of every pixel is identical at any fibre and blotch, so moving those
 * sliders can fade strands and patches in and out but can never re-decide
 * which patches are warm highlights and which are shadows.  Before
 * 2026-10-27 the mix weights WERE the settings: fibre 40 -> 100 flipped a
 * tenth of the screen across the highlight/shadow line while adjacent steps
 * moved under 2% of pixels - the user's "same settings, different output". */
static void TestStrengthSlidersDoNotRepaintPattern(void) {
    SETTINGS s = PaperDefaultSettings();
    int w = 240, h = 160, x, y, flips = 0, compared = 0, colbad = 0;
    PAPERPARAMS pp = PaperParams(w, 1.f, &s);
    int hi = (pp.wr * 299 + pp.wg_r * 587 + pp.wb_r * 114) / 1000;
    int lo = (pp.dr * 299 + pp.dg * 587 + pp.db * 114) / 1000;
    int mid = (hi + lo) / 2;
    unsigned char *a = malloc((size_t)w * h * 4);
    unsigned char *b = malloc((size_t)w * h * 4);
    CHECK(a && b, "alloc");
    if (!a || !b) { free(a); free(b); return; }
    CHECK(hi - lo > 64,
          "family colours sit far enough apart to classify at alpha 8");

    s.fibre = 0; s.blotch = 0;
    BuildPaperFull(a, w, h, 1.f, &s, NULL);
    s.fibre = 100; s.blotch = 100;
    BuildPaperFull(b, w, h, 1.f, &s, NULL);

    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned char *pa = a + 4 * ((size_t)y * w + x);
            unsigned char *pb = b + 4 * ((size_t)y * w + x);
            int c, la, lb;
            if (pa[3] < 8 || pb[3] < 8) continue;   /* too thin to un-multiply */
            /* the branch map: highlight vs shadow, un-premultiplied luminance
             * against the midpoint of the two family colours.  Un-multiply
             * only rounds down and by at most 255/alpha levels, so with the
             * gap checked above, alpha >= 8 cannot cross the midpoint. */
            la = (pa[2] * 299 + pa[1] * 587 + pa[0] * 114) * 255 / (1000 * pa[3]);
            lb = (pb[2] * 299 + pb[1] * 587 + pb[0] * 114) * 255 / (1000 * pb[3]);
            compared++;
            if ((la >= mid) != (lb >= mid)) flips++;
            /* where both are solid, the un-premultiplied colour must also
             * match within quantization (255/100 amplifies by 2.5) */
            if (pa[3] >= 100 && pb[3] >= 100)
                for (c = 0; c < 3; c++) {
                    int ca = (pa[c] * 255 + pa[3] / 2) / pa[3];
                    int cb = (pb[c] * 255 + pb[3] / 2) / pb[3];
                    if (abs(ca - cb) > 4) colbad++;
                }
        }
    CHECK(compared > w * h / 4, "both bakes carry real alpha");
    CHECK(flips == 0, "fibre/blotch must not flip the highlight/shadow map");
    if (flips)
        printf("FAIL %s (line %d): %d of %d pixels flipped colour class\n",
               __func__, __LINE__, flips, compared);
    CHECK(colbad == 0, "solid pixels keep their un-premultiplied colour");
    if (colbad)
        printf("FAIL %s (line %d): %d solid pixels changed colour\n",
               __func__, __LINE__, colbad);
    /* and the overlays must still be real: full fibre/blotch adds visible
     * alpha on top of the bare grain */
    {
        size_t i, n = (size_t)w * h, sum = 0;
        for (i = 0; i < n; i++) sum += b[4*i+3] - a[4*i+3];
        CHECK(sum > 0, "fibre/blotch overlays add alpha");
    }
    free(a); free(b);
}

/* The texture is a property of the surface, not of the pixel grid.  The
 * lattice periods come from the width in SAMPLING units and PaperPixel divides
 * the pixel coordinate by fs, so rendering the same logical surface at two
 * buffer scales must give the same paper.  Before 2026-10-24 the port sampled
 * raw device pixels while deriving the lattice from the device-pixel width, so
 * a 2880px buffer at 1.6x and a 1800px buffer at 1.0x - the same screen - gave
 * completely different textures (measured: 96% of alphas differed by more than
 * one level, max 111).  The user met that as "the same config looks different
 * depending on the order I set the settings", because the buffer width at each
 * bake was what actually varied.
 *
 * The fractional scales are the ones that matter: at an integer scale every
 * buffer pixel lands on an integer logical coordinate, which is exactly the
 * case where the coordinate division and the round(w/fs) lattice derivation
 * cannot disagree.  A 1.6 buffer pixel sits 0.6 of a logical pixel from the
 * next one, so each scale is given as an exact num/den pair and the comparison
 * samples only the logical points both grids hold exactly (x = m*den, whose
 * buffer pixel is m*num). */
static void TestTextureIsScaleInvariant(void) {
    SETTINGS s = PaperDefaultSettings();
    int lw = 300, lh = 200;                 /* the logical surface */
    int i;
    /* 1.0, 2.0, 3.0, 1.25, 1.5, 1.6 - the last is this repo's reference panel */
    int scales[][2] = { {1,1}, {2,1}, {3,1}, {5,4}, {3,2}, {8,5} };
    unsigned char *base = NULL;
    for (i = 0; i < (int)(sizeof scales / sizeof scales[0]); i++) {
        int num = scales[i][0], den = scales[i][1];
        float fs = (float)num / (float)den;
        int bw = lw * num / den, bh = lh * num / den;
        unsigned char *px = malloc((size_t)bw * bh * 4);
        int x, y, diff = 0, maxd = 0;
        CHECK(px != NULL, "alloc");
        if (!px) { free(base); return; }
        CHECK(BuildPaperFull(px, bw, bh, fs, &s, NULL), "bake");
        /* the alpha must actually be there: a scale test comparing two blank
         * frames would pass for the wrong reason */
        {
            int nonzero = 0, k;
            for (k = 0; k < bw * bh; k++) if (px[4 * (size_t)k + 3]) nonzero++;
            CHECK(nonzero > (bw * bh) / 4, "scaled bake has body");
        }
        if (i == 0) { base = px; continue; }
        for (y = 0; y < lh; y += den)
            for (x = 0; x < lw; x += den) {
                int sx = x / den * num, sy = y / den * num;
                int d = abs(base[4 * ((size_t)y * lw + x) + 3] -
                            px[4 * ((size_t)sy * bw + sx) + 3]);
                if (d > maxd) maxd = d;
                if (d > 1) diff++;
            }
        CHECK(diff == 0 && maxd <= 1, "scale invariance: no pixel moves");
        if (diff || maxd > 1)
            printf("FAIL %s (line %d): scale invariance at fs %.2f: "
                   "%d pixels moved, max %d\n",
                   __func__, __LINE__, (double)fs, diff, maxd);
        free(px);
    }
    free(base);
}

static void TestCancellation(void) {
    SETTINGS s = PaperDefaultSettings();
    int w = 200, h = 200;
    unsigned char *px = malloc((size_t)w * h * 4);
    PAPERCANCEL c;
    CHECK(px != NULL, "alloc");
    if (!px) return;
    memset(&c, 0, sizeof c);
    CHECK(BuildPaperFull(px, w, h, 1.f, &s, &c), "uncancelled bake completes");
    c.stop = 1;
    CHECK(!BuildPaperFull(px, w, h, 1.f, &s, &c), "cancel flag stops the bake");
    free(px);
}

/* The engine is documented to survive a caller that skipped ClampSettingsOf:
 * grain 0 must clamp inside PaperParams, where both the noise period and the
 * per-pixel divisor read it. Unclamped it is a zero divisor on the bake thread
 * - a fault on the period, or 0/0 noise in float that lands as a fully
 * transparent bake reporting success. */
static void TestGrainZeroBakes(void) {
    SETTINGS s = PaperDefaultSettings();
    int w = 120, h = 90, i, opaque = 0;
    unsigned char *px = malloc((size_t)w * h * 4);
    CHECK(px != NULL, "alloc");
    if (!px) return;
    s.grain = 0;                    /* below the clamp floor: the guard's case */
    CHECK(BuildPaperFull(px, w, h, 1.f, &s, NULL), "grain 0 bake completes");
    CheckPremultiplied(px, w, h, "grain 0");
    for (i = 0; i < w * h; i++) if (px[4 * (size_t)i + 3]) opaque++;
    CHECK(opaque == w * h, "grain 0 bakes the real veil, not a blank frame");
    free(px);
}

/* e-ink is not implemented on Wayland, so a requested mode=1 must never
 * survive as state.  The app normalizes it at its single settings choke
 * point; this pins the contract that a refused mode is not representable. */
static void TestEinkNotRepresentable(void) {
    SETTINGS s = PaperDefaultSettings();
    /* the raw parser accepts the value (it is platform-neutral) ... */
    CHECK(SetKeyValue(&s, "mode=1"), "set: mode=1 parses");
    CHECK(s.mode == MODE_EINK, "set: parser yields eink");
    /* ... and the Wayland layer must refuse it, not store it: the same
     * NormalizeMode the daemon's RequestMode funnels every settings path
     * through maps it back to paper and reports the refusal. */
    CHECK(!NormalizeMode(&s), "eink request reported unrepresentable");
    CHECK(s.mode == MODE_PAPER, "refused mode normalizes to paper");
    /* and a paper mode request is honoured without side effects */
    CHECK(SetKeyValue(&s, "mode=0"), "set: mode=0 parses");
    CHECK(NormalizeMode(&s), "paper request is representable");
    CHECK(s.mode == MODE_PAPER, "paper mode stays paper");
}

/* The panelgap migration: the only code that can fix an already-installed
 * config, so every branch is pinned here.  The old default (96) is
 * indistinguishable from a deliberate 96 in a file that predates cfgver, so
 * that exact value migrates and every other value is treated as a choice.
 *
 * The migration is keyed on whether the FILE carried a cfgver key, not on the
 * value: ConfigWrite stamps every file this build saves, so a stamped file has
 * already been through this and must never be migrated again - otherwise a
 * panelgap the user chose is reverted on the next start. */
static void TestPanelgapMigration(void) {
    SETTINGS s;

    /* a pre-v2 file: no cfgver line at all, and it holds the old default */
    s = PaperDefaultSettings();
    s.panelgap = 96;
    CHECK(MigrateSettings(&s, 0) == 1, "migrate: the old 96 default is migrated");
    CHECK(s.panelgap == 0, "migrate: 96 becomes 0 (paper reaches the edge)");
    CHECK(s.cfgver == 2, "migrate: the schema version is stamped");

    /* the migration is one-shot: the file this build writes carries the key,
     * so the next start sees it and leaves a 96 alone */
    s.panelgap = 96;                 /* deliberate, post-migration */
    CHECK(MigrateSettings(&s, 1) == 0, "migrate: a stamped file is not touched");
    CHECK(s.panelgap == 96, "migrate: the user's own 96 survives");
    s.cfgver = 0;                    /* even a hand-edited stamp value */
    CHECK(MigrateSettings(&s, 1) == 0,
          "migrate: a stamp of 0 in a stamped file does not re-arm it");
    CHECK(s.panelgap == 96, "migrate: the stamp's value is not what arms it");
    CHECK(s.cfgver == 2, "migrate: the stamp is repaired to the current version");

    /* any other pre-v2 value was a real choice and is preserved */
    s = PaperDefaultSettings();
    s.panelgap = 48;
    CHECK(MigrateSettings(&s, 0) == 0, "migrate: a non-default pre-v2 gap is kept");
    CHECK(s.panelgap == 48, "migrate: the chosen gap is unchanged");
    CHECK(s.cfgver == 2, "migrate: the version is stamped even when unchanged");

    /* a fresh install already carries the current version and the new
     * default, so it has nothing to migrate */
    s = PaperDefaultSettings();
    CHECK(s.cfgver == 2, "migrate: defaults are already current");
    CHECK(MigrateSettings(&s, 1) == 0, "migrate: defaults need no migration");
    CHECK(s.panelgap == 0, "migrate: defaults leave no bottom band");
}

/* Run one CLI invocation through the shell, capturing stdout and stderr;
 * returns the raw wait status (WIFEXITED/WEXITSTATUS apply). */
static int RunCli(const char *cmd, char *out, size_t outsz) {
    FILE *p = popen(cmd, "r");
    size_t got = 0;
    if (!p) return -1;
    while (got + 1 < outsz) {
        size_t n = fread(out + got, 1, outsz - 1 - got, p);
        if (!n) break;
        got += n;
    }
    out[got] = 0;
    return pclose(p);
}

/* The CLI refusal rules live in mnpaper.c's main(), a translation unit this
 * suite cannot link (it is the daemon and needs a compositor), so they are
 * pinned through the real binary: a refused operand names itself on stderr
 * and exits 2.  Every child gets a private XDG_RUNTIME_DIR, a private
 * XDG_CONFIG_HOME and a compositor that is not there, so a refusal that
 * somehow falls through fails on its own instead of reaching the live
 * session or the user's settings. */
static void TestCliRefusals(const char *argv0) {
    char dir[512], bin[600], cmd[4096], out[2048];
    char rt[] = "/tmp/mn-test-rt-XXXXXX", cf[] = "/tmp/mn-test-cf-XXXXXX";
    const char *slash = strrchr(argv0, '/');
    size_t n;
    int st;

    if (slash) {
        n = (size_t)(slash - argv0);
        if (n >= sizeof dir) n = sizeof dir - 1;
        memcpy(dir, argv0, n);
        dir[n] = 0;
    } else {
        strcpy(dir, ".");
    }
    snprintf(bin, sizeof bin, "%s/../mnpaper", dir);
    if (access(bin, X_OK) != 0) {
        CHECK(0, "cli: mnpaper binary not found next to the test tree");
        return;
    }
    if (!mkdtemp(rt) || !mkdtemp(cf)) {
        CHECK(0, "cli: private temp dirs");
        return;
    }
    setenv("XDG_RUNTIME_DIR", rt, 1);
    setenv("XDG_CONFIG_HOME", cf, 1);
    setenv("WAYLAND_DISPLAY", "mn-test-suite-none", 1);
    unsetenv("WAYLAND_SOCKET");

    /* no '=': the operand is refused by name, nothing is applied */
    snprintf(cmd, sizeof cmd, "\"%s\" --set warmth 2>&1", bin);
    st = RunCli(cmd, out, sizeof out);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 2, "cli: missing '=' exits 2");
    CHECK(strstr(out, "refused") && strstr(out, "warmth"),
          "cli: missing '=' names the operand");

    /* unknown key */
    snprintf(cmd, sizeof cmd, "\"%s\" --set bogus=1 2>&1", bin);
    st = RunCli(cmd, out, sizeof out);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 2, "cli: unknown key exits 2");
    CHECK(strstr(out, "refused") && strstr(out, "bogus"),
          "cli: unknown key is named");

    /* --set with no pair at all */
    snprintf(cmd, sizeof cmd, "\"%s\" --set 2>&1", bin);
    st = RunCli(cmd, out, sizeof out);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 2, "cli: bare --set exits 2");
    CHECK(strstr(out, "key=value"), "cli: bare --set says what is missing");

    /* an operand longer than the staging bound: grammar-valid (all digits)
     * and must still be refused before anything is sent */
    {
        char kv[160];
        strcpy(kv, "intensity=");
        for (n = 0; n < 90; n++) strcat(kv, "0");
        snprintf(cmd, sizeof cmd, "\"%s\" --set %s 2>&1", bin, kv);
        st = RunCli(cmd, out, sizeof out);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 2,
              "cli: overlong operand exits 2");
        CHECK(strstr(out, "intensity") && strstr(out, "maximum is 95"),
              "cli: overlong operand names itself and the bound");
    }

    /* a batch beyond MAX_SETS is refused by name, not silently trimmed */
    {
        size_t used = 0;
        int k;
        used += (size_t)snprintf(cmd, sizeof cmd, "\"%s\"", bin);
        for (k = 1; k <= 65; k++) {
            used += (size_t)snprintf(cmd + used, sizeof cmd - used,
                                     " --set warmth=%d", k);
            if (used >= sizeof cmd - 8) break;
        }
        strcat(cmd, " 2>&1");
        st = RunCli(cmd, out, sizeof out);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 2,
              "cli: oversized batch exits 2");
        CHECK(strstr(out, "more than 64") && strstr(out, "warmth=65"),
              "cli: oversized batch names the bound and the first ignored pair");
    }

    /* a --set that got no operand is refused even when other pairs follow */
    snprintf(cmd, sizeof cmd, "\"%s\" --set warmth=45 --set 2>&1", bin);
    st = RunCli(cmd, out, sizeof out);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 2, "cli: dangling --set exits 2");
    CHECK(strstr(out, "key=value"), "cli: dangling --set says what is missing");

    rmdir(rt);
    rmdir(cf);
}

/* The config round trip: what this build writes, it must read back unchanged.
 * The loader used to zero the schema stamp before parsing, so a file this
 * build had just written looked pre-v2 on the next start and the migration
 * silently reverted a panelgap the user had chosen (including one set with
 * --set on a machine that had no settings file).  Formatting a struct with the
 * real writer and parsing it back with the real reader pins the property that
 * broke; the loader's own copy of that sequence is the same two calls. */
static void TestConfigRoundTrip(void) {
    SETTINGS s = PaperDefaultSettings(), back = PaperDefaultSettings();
    char body[2048], pristine[2048], *line, *save = NULL;
    int stamped = 0, loaded = 0;
    FILE *f = tmpfile();
    size_t got;

    CHECK(f != NULL, "config: the temp file opens");
    if (!f) return;

    /* every key the writer emits starts away from its default and in
     * bounds, so a key lost in either direction - dropped by the writer,
     * refused or mis-parsed by the parser - cannot hide behind one;
     * panelgap stays the user-chosen 96 the migration checks build on */
    s.master = 0; s.mode = MODE_EINK; s.intensity = 33; s.warmth = 63;
    s.grain = 7; s.fibre = 55; s.blotch = 66; s.shades = 9;
    s.contrast = 51; s.dither = 76; s.share = 1; s.tbmode = TB_COVER;
    s.autoupd = 0; s.panelgap = 96; s.cfgver = 1;
    CHECK(ConfigFormat(&s, f) == 0, "config: the file body is written");
    rewind(f);
    got = fread(body, 1, sizeof body - 1, f);
    body[got] = 0;
    fclose(f);
    /* strtok_r below writes NULs into body, so keep the pristine bytes
     * for the total round-trip comparison at the end */
    memcpy(pristine, body, got + 1);

    /* the written body is the file contract this build owns: all 15 keys
     * must be in it.  The byte comparison at the end cannot see a key the
     * writer drops - both of its bodies come from the same writer - so the
     * contract is checked on the body itself */
    {
        const char *const keys[15] = {
            "master", "mode", "intensity", "warmth", "grain", "fibre",
            "blotch", "shades", "contrast", "dither", "share", "tbmode",
            "autoupd", "panelgap", "cfgver"
        };
        char needle[16];
        int i;
        for (i = 0; i < 15; i++) {
            snprintf(needle, sizeof needle, "\n%s=", keys[i]);
            if (!strstr(pristine, needle)) {
                printf("FAIL %s: the written body lacks the %s key\n",
                       __func__, keys[i]);
                failures++;
            }
        }
    }

    /* parse it back the way LoadSettings does */
    for (line = strtok_r(body, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (ConfigParseLine(&back, line, &stamped) > 0) loaded++;
    CHECK(stamped, "config: the written file carries the schema stamp");
    CHECK(loaded > 0, "config: the written file yields its keys");
    CHECK(back.panelgap == 96, "config: the chosen panelgap reads back");
    CHECK(back.cfgver == 1, "config: the stamp value reads back");

    /* total round trip: re-serialize the parsed state and require the same
     * bytes.  The key scan above holds the writer to every key; this one
     * holds the parser: no seeded field sits at its default, so a key that
     * is refused, lost or mapped into the wrong field changes the bytes. */
    {
        char body2[2048];
        FILE *f2 = tmpfile();
        size_t got2;
        CHECK(f2 != NULL, "config: the second temp file opens");
        if (!f2) return;
        CHECK(ConfigFormat(&back, f2) == 0, "config: the parsed state re-writes");
        rewind(f2);
        got2 = fread(body2, 1, sizeof body2 - 1, f2);
        body2[got2] = 0;
        fclose(f2);
        CHECK(strcmp(pristine, body2) == 0,
              "config: what this build writes it reads back unchanged");
    }

    /* and the migration leaves it alone, because the file carried the stamp */
    CHECK(MigrateSettings(&back, stamped) == 0,
          "config: a file this build wrote is not migrated");
    CHECK(back.panelgap == 96, "config: the chosen panelgap survives a reload");

    /* a genuine pre-v2 body (no cfgver line) still migrates exactly once */
    back = PaperDefaultSettings();
    stamped = 0;
    {
        char pre[] = "panelgap=96";
        CHECK(ConfigParseLine(&back, pre, &stamped) > 0,
              "config: a pre-v2 line parses");
    }
    CHECK(!stamped, "config: a pre-v2 body carries no stamp");
    CHECK(MigrateSettings(&back, stamped) == 1, "config: a pre-v2 96 migrates");
    CHECK(back.panelgap == 0, "config: the migration yields the new default");

    /* the stamp is presence, not value: even cfgver=0 in the file counts as
     * stamped, so a hand-edited value cannot re-arm the migration */
    back = PaperDefaultSettings();
    back.panelgap = 96;
    stamped = 0;
    {
        char zero[] = "cfgver=0";
        CHECK(ConfigParseLine(&back, zero, &stamped) > 0,
              "config: cfgver=0 parses");
    }
    CHECK(stamped, "config: cfgver=0 still counts as a stamp");
    CHECK(MigrateSettings(&back, stamped) == 0,
          "config: a stamp of 0 does not re-arm the migration");
    CHECK(back.panelgap == 96, "config: the chosen panelgap is kept");
}

int main(int argc, char **argv) {
    (void)argc;
    TestEinkNotRepresentable();
    TestPanelgapMigration();
    TestConfigRoundTrip();
    TestDefaultsAndClamp();
    TestSetKeyValueGrammar();
    TestBakeDeterminismAndValidity();
    TestTextureIsActuallyVisible();
    TestSettingsArePerceptible();
    TestSettingsActuallyChangePixels();
    TestCancellation();
    TestGrainZeroBakes();
    TestTextureIsScaleInvariant();
    TestStrengthSlidersDoNotRepaintPattern();
    TestCliRefusals(argc >= 1 && argv[0] ? argv[0] : "test_core");
    printf("%s: %d failure(s)\n", failures ? "RESULT" : "RESULT 0", failures);
    return failures ? 1 : 0;
}
