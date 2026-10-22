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

static void TestDefaultsAndClamp(void) {
    SETTINGS s = PaperDefaultSettings();
    CHECK(s.master == 1 && s.mode == MODE_PAPER, "defaults: master/mode");
    CHECK(s.intensity == 30 && s.warmth == 45, "defaults: intensity/warmth");
    CHECK(s.grain == 4 && s.fibre == 40 && s.blotch == 30, "defaults: texture");
    CHECK(s.tbmode == TB_NOT_COVER, "defaults: tbmode");
    CHECK(s.panelgap == 96, "defaults: panelgap 96");

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

int main(int argc, char **argv) {
    (void)argc;
    TestEinkNotRepresentable();
    TestDefaultsAndClamp();
    TestSetKeyValueGrammar();
    TestBakeDeterminismAndValidity();
    TestSettingsActuallyChangePixels();
    TestCancellation();
    TestCliRefusals(argc >= 1 && argv[0] ? argv[0] : "test_core");
    printf("%s: %d failure(s)\n", failures ? "RESULT" : "RESULT 0", failures);
    return failures ? 1 : 0;
}
