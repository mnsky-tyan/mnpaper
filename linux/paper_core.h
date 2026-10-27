/* paper_core.h - the portable half of mnPaper.
 *
 * Single-include header: #include it once from one .c file (the Windows
 * build keeps its own copy inside mnPaper.c for now; when the two are
 * unified this header becomes the one source of truth).
 *
 * Everything here runs without a display, a compositor or a GUI: the noise
 * hash, the value noise and fbm, the parameter table and the per-pixel
 * generator, plus the SETTINGS struct with its defaults and its clamp
 * table.  The Linux app and the headless tests drive only this code.
 *
 * Pixel format contract: PaperPixel writes 4 bytes per pixel in memory
 * order B,G,R,A, PREMULTIPLIED (each colour channel is already scaled by
 * alpha).  That is exactly what a top-down 32bpp Windows DIB wants and
 * exactly what wl_shm's ARGB8888 wants on little-endian machines, so the
 * same bytes can be uploaded on either platform unmodified.
 */
#ifndef MNPAPER_PAPER_CORE_H
#define MNPAPER_PAPER_CORE_H

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MODE_PAPER 0
#define MODE_EINK  1

#define TB_NOT_COVER 0  /* raise the bar above the veil: do not cover the taskbar */
#define TB_COVER     1  /* leave the bar under the veil: the paper covers the taskbar */

/* ------------------------------------------------------------ log -------- */

/* Shared logger for the app and the tray module: stderr always, plus the
 * log file once the app has opened one (it assigns paper_log_file).
 * Defined in mnpaper.c (the app); test_core.c does not log. */
extern FILE *paper_log_file;
void PAPERLOG(const char *fmt, ...);

typedef struct {
    int master;
    int mode;
    int intensity;
    int warmth;
    int grain;
    int fibre;
    int blotch;
    int shades;
    int contrast;
    int dither;
    int autostart;
    int share;     /* 1 = the texture is visible in screenshots / screen shares */
    int tbmode;    /* how the veil meets the bottom bar (see TB_* above).
                    * Windows: 0 raises the taskbar above the veil, 1 leaves
                    * it under the paper.  Wayland: an overlay layer surface is
                    * always stacked above the panel, so the modes choose
                    * between the two by way of `panelgap` - 0 leaves the
                    * bottom `panelgap` strip unpainted and asks the compositor
                    * for exclusive zone 0 (with the default panelgap=0 the
                    * paper simply reaches the edge, and a docked panel keeps
                    * its space when the compositor honours that request); 1
                    * covers the whole screen with exclusive zone -1.  Both
                    * platforms obey it (mnpaper.c's ApplyExclusiveZone). */
    int autoupd;   /* 1 = look for a new version about once a day */
    int cfgver;    /* config schema version, written by ConfigWrite on every
                    * save.  A file that carries the key at all was written by
                    * a build that knows it; a file without the line predates
                    * the panelgap default change, which is what LoadSettings
                    * migrates. */
    int panelgap;  /* Wayland: logical pixels left uncovered at the bottom.
                    *
                    * 0 (the default) is right for every panel that either
                    * reserves its own space (zone 0 asks the compositor to
                    * move our surface clear of it, and the protocol leaves
                    * that move to the compositor) or auto-hides (it reserves
                    * nothing, so there is no strip to leave clear and the
                    * paper should reach the screen edge).  A non-zero value
                    * is for a FLOATING panel that reserves nothing but must
                    * stay uncovered - the one case the compositor cannot
                    * infer - and for any panel a compositor leaves under the
                    * paper.  This used to default to 96, which painted a 96px
                    * hole under panels that were not there; a file without a
                    * cfgver key is migrated to 0 once (see LoadSettings). */
} SETTINGS;

/* Same defaults as mnPaper.c's g_s initialiser. Keep the two in step until
 * the settings core is unified. */
static SETTINGS PaperDefaultSettings(void) {
    SETTINGS s;
    memset(&s, 0, sizeof s);
    s.master = 1; s.mode = MODE_PAPER; s.intensity = 30;
    s.warmth = 45; s.grain = 4; s.fibre = 40; s.blotch = 30;
    s.shades = 4; s.contrast = 50; s.dither = 75;
    s.autostart = 0; s.share = 0; s.tbmode = TB_NOT_COVER;
    s.autoupd = 1; s.cfgver = 2; s.panelgap = 0;
    return s;
}

/* Same keys as the Windows SetKeyValue, with a stricter value grammar:
 * digits only, and the value must fit an int.  Unknown keys and malformed
 * values are refused, never silently zero.  A well-formed value outside a
 * key's bounds is accepted here and clamped to the nearest bound by
 * ClampSettingsOf, exactly as on Windows. */
static int SetKeyValue(SETTINGS *s, const char *arg) {
    const char *eq = strchr(arg, '=');
    char key[64];
    int v;
    const char *p;
    if (!eq || eq - arg >= 64) return 0;
    memcpy(key, arg, (size_t)(eq - arg));
    key[eq - arg] = 0;
    /* malformed --set values are refused, never become a silent 0 */
    p = eq + 1;
    if (!*p) return 0;
    for (; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    /* the value must fit an int: atoi's truncation would turn 2^32 into 0
     * and silently pick the opposite clamp endpoint */
    {
        unsigned long n = strtoul(eq + 1, NULL, 10);
        if (n > (unsigned long)INT_MAX) return 0;
        v = (int)n;
    }
    if (!strcasecmp(key, "intensity")) s->intensity = v;
    else if (!strcasecmp(key, "warmth")) s->warmth = v;
    else if (!strcasecmp(key, "grain")) s->grain = v;
    else if (!strcasecmp(key, "fibre")) s->fibre = v;
    else if (!strcasecmp(key, "blotch")) s->blotch = v;
    else if (!strcasecmp(key, "shades")) s->shades = v;
    else if (!strcasecmp(key, "contrast")) s->contrast = v;
    else if (!strcasecmp(key, "dither")) s->dither = v;
    else if (!strcasecmp(key, "mode")) s->mode = v ? MODE_EINK : MODE_PAPER;
    else if (!strcasecmp(key, "autostart")) s->autostart = v ? 1 : 0;
    else if (!strcasecmp(key, "share")) s->share = v ? 1 : 0;
    else if (!strcasecmp(key, "tbmode")) s->tbmode = v ? TB_COVER : TB_NOT_COVER;
    else if (!strcasecmp(key, "panelgap")) s->panelgap = v;
    else if (!strcasecmp(key, "cfgver")) s->cfgver = v;
    else if (!strcasecmp(key, "autoupd")) s->autoupd = v ? 1 : 0;
    else if (!strcasecmp(key, "master")) s->master = v ? 1 : 0;
    else return 0;
    return 1;
}

/* One row per clamped field, bounds next to the field they bound - the same
 * table shape as mnPaper.c's ClampSettingsOf, so a field added to one and
 * forgotten in the other is a visible behavioural drift, not a silent one. */
static void ClampSettingsOf(SETTINGS *s) {
    struct { int *v; int lo, hi; } f[] = {
        { &s->master,    0,   1 }, { &s->mode,      0,   1 },
        { &s->intensity, 0,  40 }, { &s->warmth,    0, 100 },
        { &s->grain,     2,  12 }, { &s->fibre,     0, 100 },
        { &s->blotch,    0, 100 }, { &s->shades,    2,  16 },
        { &s->contrast,  0, 100 }, { &s->dither,    0, 100 },
        { &s->autostart, 0,   1 }, { &s->share,     0,   1 },
        { &s->tbmode,    0,   TB_COVER }, { &s->autoupd,   0,   1 },
        { &s->panelgap,  0,        512 }, { &s->cfgver,   0,   2 },
    };
    int i;
    for (i = 0; i < (int)(sizeof(f) / sizeof(f[0])); i++) {
        if (*f[i].v < f[i].lo) *f[i].v = f[i].lo;
        if (*f[i].v > f[i].hi) *f[i].v = f[i].hi;
    }
}

/* The config file's two halves, kept next to the parser and the migration so
 * the headless suite can drive the real round trip: the app's loader and
 * writer call exactly these, and a test that formats a struct and parses the
 * result back pins the property that matters - a file this build writes is
 * read back unchanged, including the schema stamp. */

/* Writes the canonical file body: every key, always, so the file is
 * diff-friendly.  Returns 0 on success, -1 on a write error. */
static int ConfigFormat(const SETTINGS *s, FILE *f) {
    if (fprintf(f,
            "# mnpaper settings (same keys as the Windows registry values;\n"
            "# autostart is not stored here - the autostart .desktop file is the\n"
            "# truth, exactly as the Windows Run key is)\n"
            "master=%d\nmode=%d\nintensity=%d\nwarmth=%d\ngrain=%d\n"
            "fibre=%d\nblotch=%d\nshades=%d\ncontrast=%d\ndither=%d\n"
            "share=%d\ntbmode=%d\nautoupd=%d\npanelgap=%d\ncfgver=%d\n",
            s->master, s->mode, s->intensity, s->warmth, s->grain,
            s->fibre, s->blotch, s->shades, s->contrast, s->dither,
            s->share, s->tbmode, s->autoupd, s->panelgap, s->cfgver) < 0)
        return -1;
    return 0;
}

/* Applies one line of the file.  `stamped` records whether the file carried
 * the cfgver key AT ALL - that presence, not the value, is what tells a
 * cfgver-aware file from a pre-v2 one, so a file this build wrote can never
 * re-arm the migration.  Returns 1 when the line set a key, 0 when it was a
 * blank line or a comment, and -1 when the key or value was refused. */
static int ConfigParseLine(SETTINGS *s, char *line, int *stamped) {
    char *nl = line + strlen(line);
    if (nl > line && nl[-1] == '\n') nl[-1] = 0;
    if (line[0] == '#' || line[0] == 0) return 0;
    if (!strncasecmp(line, "cfgver=", 7)) *stamped = 1;
    return SetKeyValue(s, line) ? 1 : -1;
}

/* Config schema migration, run once per load.  A file written before cfgver
 * existed carries panelgap=96 because that WAS the default, not because the
 * user chose it - and 96 logical pixels is a bigger hole than most panels,
 * so it showed as an uncovered band at the bottom on any panel that reserves
 * no space of its own (auto-hide, floating).  A cfgver-less file holding
 * exactly that value is the old default, so it becomes the new one; any other
 * value is a real choice and is left alone.
 *
 * `file_had_cfgver` is whether the file carried the key at all, not what the
 * key held: ConfigWrite stamps every file this build saves, so the presence of
 * the key is proof the file came from a cfgver-aware build and the migration
 * is done.  Keying on the value instead would let a stamped file re-arm the
 * migration - 0 is both the clamp floor and a settable value - and silently
 * revert a panelgap the user chose.  Returns 1 when panelgap was migrated. */
static int MigrateSettings(SETTINGS *s, int file_had_cfgver) {
    s->cfgver = 2;
    if (file_had_cfgver || s->panelgap != 96) return 0;
    s->panelgap = 0;
    return 1;
}

/* The only mode this build can represent: e-ink needs screen capture, which
 * the Wayland port does not implement.  Every settings path funnels through
 * here so a refused request can never be persisted, reported or baked as
 * e-ink.  Returns 1 when the request is representable. */
static int NormalizeMode(SETTINGS *s) {
    if (s->mode != MODE_EINK) return 1;
    s->mode = MODE_PAPER;
    return 0;
}

/* ------------------------------------------------------------ the paper --- */

static unsigned lh(int x, int y, int seed) {
    /* unsigned multiplies: same wrapped values as the Windows build's signed
     * expression on any two's-complement machine, but well-defined */
    unsigned h = (unsigned)x * 73856093u ^ (unsigned)y * 19349663u ^
                 (unsigned)seed * 83492791u;
    h = (h ^ (h >> 15)) * 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    return h;
}
static const float R32 = 2.3283064365386963e-10f;

static float vnoise(float x, float y, int p, int seed) {
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0, v00, v10, v01, v11, a, b;
    int xa, ya, xb, yb;
    fx = fx * fx * (3.f - 2.f * fx);
    fy = fy * fy * (3.f - 2.f * fy);
    xa = ((x0 % p) + p) % p; ya = ((y0 % p) + p) % p;
    xb = (xa + 1) % p;         yb = (ya + 1) % p;
    v00 = lh(xa, ya, seed) * R32; v10 = lh(xb, ya, seed) * R32;
    v01 = lh(xa, yb, seed) * R32; v11 = lh(xb, yb, seed) * R32;
    a = v00 + (v10 - v00) * fx;
    b = v01 + (v11 - v01) * fx;
    return a + (b - a) * fy;
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
    return sum / tot;
}

static int NextPow2(int v) {
    int p = 1;
    while (p < v) p <<= 1;
    return p;
}

typedef struct {
    SETTINGS s;
    int pg, pf, pm, wr, wg_r, wb_r, dr, dg, db, bias, spread;
    int fspread, bspread;      /* fibre/blotch own alpha-modulation gain */
    float fs;                  /* coordinate scale: 1 full, ratio on Linux */
} PAPERPARAMS;

/* One fbm is four octaves of value noise weighted 0.533/0.267/0.133/0.067.
 * Averaging shrinks the spread, so the deviation from the midpoint is far
 * narrower than the [0,1] that a naive |2n-1| suggests.  Measured over a full
 * 1800x1125 frame the grain field's folded magnitude has mean 0.216, sd 0.152
 * and max 0.895 (2026-10-27, single-field after the pattern decoupling).  The
 * scale is 1/(3 sigma) so the swing reaches its design range at three sigma
 * and clips only past it; the alpha it buys at intensity 30 is median 38 /
 * p99 68, matching the calibration the user accepted in 2.7.23.  Keep it tied
 * to the noise: if the octave weights or the field count change, re-measure
 * and update this.  Keep it identical to mnPaper.c. */
#define PAPER_MAG_SCALE 2.3f

static PAPERPARAMS PaperParams(int w, float fs, const SETTINGS *sp) {
    PAPERPARAMS p;
    int wb;
    /* Warmth spans cool -> neutral(50) -> aged amber; signed swing, as in
     * mnPaper.c: cool reduces red and raises blue, warm raises red/brown and
     * cuts blue. */
    int tw = sp->warmth * 2 - 100;          /* -100 cool .. +100 warm */
    int cool = tw < 0 ? -tw : 0;
    int warm = tw > 0 ? tw : 0;
    p.s = *sp;
    /* fs is device pixels per sampling unit.  It scales ALL noise frequencies
     * at once, so a coarser grid samples a coarser paper (same character,
     * larger features) instead of aliasing fine grain into a flat smear.  The
     * Linux port passes the buffer/logical ratio here, which is what keeps the
     * texture a property of the surface rather than of the pixel grid: the
     * lattice below is derived from `w` in sampling units, so a 2880px buffer
     * at 1.6x and a 1800px buffer at 1.0x render the SAME pattern.  Sampling
     * raw device pixels instead made identical settings render a completely
     * different texture at the two sizes (measured 2026-10-24: pattern
     * correlation 0.00, ~15 luminance levels apart), which is what the user
     * reported as "the same config looks different depending on the order I
     * set the settings" - the buffer width at each bake was what varied. */
    p.fs = fs > 0.f ? fs : 1.f;
    /* PaperParams is the only place grain is consumed, and both the period
     * below and PaperPixel divide by it, so the clamp has to land in the copy
     * the pixel loop reads: a caller that skipped ClampSettingsOf (this header
     * is the shared engine) must not be able to divide by zero or flood the
     * frame with 0/0 noise inside a bake thread. */
    if (p.s.grain < 1) p.s.grain = 1;
    p.pg = NextPow2(w / p.s.grain + 2);
    p.pf = NextPow2(w / 2 + 2);
    p.pm = NextPow2(w / 90 + 2);
    wb = warm * 44 / 100;
    /* The highlight family used to sit at 236..255, i.e. inside the range of a
     * white page, so the positive half of the noise was invisible on light
     * content: the paper could only darken, and warmth - whose whole job is
     * tinting the highlight branch - had almost nothing to tint.  Pulling the
     * highlights down to 222..244 keeps the cream cast while putting them
     * below any plausible background. */
    p.wr = 244 - cool * 55 / 100;           /* highlight red: cool drops to 189 */
    p.wg_r = 238 + cool / 25 - warm / 7;    /* highlight green */
    p.wb_r = 222 + cool / 5 - wb;           /* highlight blue (255-capped when cool) */
    if (p.wb_r > 255) p.wb_r = 255;
    p.dr = 145 + warm / 8 - cool * 40 / 100;  /* shadow red   */
    p.dg = 133 + warm / 10 - cool / 8;        /* shadow green */
    p.db = 108 - wb + cool / 2;               /* shadow blue  */
    /* Strength is an opacity budget, so it has to be spent on opacity.  The old
     * mapping made `bias + spread == intensity`, i.e. a ceiling of 40/255
     * (15.7%) for a slider that every neighbouring control runs to 100, and the
     * folded `mag` then delivered only 16% of even that.  With the magnitude
     * normalised (see PAPER_MAG_SCALE) the same budget buys a real veil: at the
     * default intensity 30 the median opacity is 15% and the peak 29%, where
     * the old code's *maximum* setting reached 7% median / 18% peak.
     *
     * `bias` stays a floor so the paper never vanishes in the middle of a
     * texture; `spread` is the swing on top of it.  The 6/5 factor keeps the
     * top setting a pleasant 21% median rather than a grey wash - measured
     * against rendered candidates over a white page (2026-10-23).
     * intensity 0 still means fully transparent, which the tests pin. */
    p.bias = sp->intensity / 4;             /* 0..10  */
    p.spread = sp->intensity * 6 / 5;       /* 0..48  */
    /* Fibre and blotch add their own alpha modulation on top of the mix.  The
     * gain is one float expression with a single truncation at the end; over
     * every reachable spread/fibre pair it rounds to exactly what the old
     * `spread * 13 * fibre / 10 / 100` produced, so the visible gain comes
     * from the spread change above, not from this line. */
    p.fspread = (int)(p.spread * 1.3f * sp->fibre / 100.f);   /* up to +1.3x */
    p.bspread = (int)(p.spread * 1.2f * sp->blotch / 100.f);  /* up to +1.2x */
    return p;
}

static void PaperPixel(unsigned char *out, int x, int y, const PAPERPARAMS *p) {
    float u = (float)x / p->fs, v = (float)y / p->fs;
    float grain = fbm(u / (float)p->s.grain, v / (float)p->s.grain, p->pg);
    float fibre = fbm(u / 2.f, v / 12.f, p->pf);
    float macro = fbm(u / 90.f, v / 70.f, p->pm);
    /* The light/dark pattern is the grain field ALONE.  It used to be a
     * weighted mix whose fibre and macro weights were the fibre and blotch
     * SETTINGS, so moving those sliders re-decided which patches are warm
     * highlights and which are shadows - a tenth of the screen flips sides
     * between fibre 40 and 100 at the defaults (measured 2026-10-27), while
     * adjacent steps moved under 2% of pixels.  Half the dial felt dead and
     * the rest felt like a different paper: the user called it "same
     * settings, different output".  Sliders must scale VISIBILITY, never
     * re-weight the pattern: grain sizes the pattern, fibre and blotch are
     * pure alpha overlays on it, and warmth only tints. */
    float d = (grain - 0.5f) * 2.f;
    float mag = d < 0 ? -d : d;
    /* Normalise the folded magnitude to the range the swing is scaled for; see
     * PAPER_MAG_SCALE.  The fibre/blotch terms come from the same narrow
     * distribution and get the same treatment. */
    float magN = mag * PAPER_MAG_SCALE;
    float fibN = fabsf(fibre - 0.5f) * 2.f * PAPER_MAG_SCALE;
    float macN = fabsf(macro - 0.5f) * 2.f * PAPER_MAG_SCALE;
    float a;
    int ai, cr, cg, cb;
    if (magN > 1.f) magN = 1.f;
    if (fibN > 1.f) fibN = 1.f;
    if (macN > 1.f) macN = 1.f;
    /* One accumulation, one rounding: the old code cast each term to int
     * separately, dropping up to a full level per term from a mean alpha of
     * about 10. */
    a = (float)p->bias + magN * (float)p->spread
      + fibN * (float)p->fspread + macN * (float)p->bspread;
    ai = (int)(a + 0.5f);
    cr = d >= 0 ? p->wr : p->dr;
    cg = d >= 0 ? p->wg_r : p->dg;
    cb = d >= 0 ? p->wb_r : p->db;
    if (ai < 0) ai = 0;
    if (ai > 255) ai = 255;
    out[0] = (unsigned char)(cb * ai / 255);
    out[1] = (unsigned char)(cg * ai / 255);
    out[2] = (unsigned char)(cr * ai / 255);
    out[3] = (unsigned char)ai;
}

/* Uncancellable full bake: epoch == NULL means nothing can cancel us (the
 * dump path).  Returns 0 only when a cancel was requested. */
typedef struct { volatile int stop; } PAPERCANCEL;   /* portable stand-in for
                                             * the Windows interlocked flags */
static int PaperCancelledOf(const PAPERCANCEL *c) { return c && c->stop; }

static int BuildPaperFull(unsigned char *px, int w, int h, float fs,
                          const SETTINGS *sp, const PAPERCANCEL *cancel) {
    /* `w`/`h` are buffer pixels; `fs` is device pixels per sampling unit, so
     * the lattice is derived from the surface-local width and the pattern does
     * not change when the buffer scale does. */
    float step = fs > 0.f ? fs : 1.f;
    PAPERPARAMS p = PaperParams((int)((float)w / step + 0.5f), step, sp);
    int x, y;
    for (y = 0; y < h; y++) {
        if (PaperCancelledOf(cancel)) return 0;
        for (x = 0; x < w; x++)
            PaperPixel(px + 4 * ((size_t)y * w + x), x, y, &p);
    }
    return 1;
}

/* The 8px preview sampler (BuildPaperPreview/ExpandPaperRow) is Windows-only
 * today: mnPaper.c defines it for the settings GUI's live slider feedback and
 * the Windows tests/ tree covers it.  It was never carried into this port, so
 * there is nothing unused here to delete and nothing to restore.  When the
 * settings GUI lands, port the sampler and its tests together, or leave both
 * out - never ship the code without the test that covers it. */

#endif /* MNPAPER_PAPER_CORE_H */
