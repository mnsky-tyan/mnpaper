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

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MODE_PAPER 0
#define MODE_EINK  1

#define TB_NOT_COVER 0  /* raise the bar above the veil: do not cover the taskbar */
#define TB_COVER     1  /* leave the bar under the veil: the paper covers the taskbar */

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
    int tbmode;    /* how the veil meets a revealed taskbar (see TB_* above;
                    * meaningful on Windows; other desktops ignore it) */
    int autoupd;   /* 1 = look for a new version about once a day */
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
    s.autoupd = 1;
    return s;
}

/* Same keys and value grammar as the Windows SetKeyValue: digits only,
 * unknown or malformed entries are refused, never silently zero. */
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
    v = atoi(eq + 1);
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
    };
    int i;
    for (i = 0; i < (int)(sizeof(f) / sizeof(f[0])); i++) {
        if (*f[i].v < f[i].lo) *f[i].v = f[i].lo;
        if (*f[i].v > f[i].hi) *f[i].v = f[i].hi;
    }
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
    float wg, wf, wm, wsum;
    float fs;                  /* coordinate scale: 1 full, step in preview */
} PAPERPARAMS;

static PAPERPARAMS PaperParams(int w, const SETTINGS *sp) {
    PAPERPARAMS p;
    int wb;
    /* Warmth spans cool -> neutral(50) -> aged amber; signed swing, as in
     * mnPaper.c: cool reduces red and raises blue, warm raises red/brown and
     * cuts blue. */
    int tw = sp->warmth * 2 - 100;          /* -100 cool .. +100 warm */
    int cool = tw < 0 ? -tw : 0;
    int warm = tw > 0 ? tw : 0;
    p.s = *sp;
    p.fs = 1.f;   /* full-res default; the preview build raises it to step */
    p.pg = NextPow2(w / sp->grain + 2);
    p.pf = NextPow2(w / 2 + 2);
    p.pm = NextPow2(w / 90 + 2);
    p.wg = 0.45f;
    p.wf = 0.30f * sp->fibre / 100.f;
    p.wm = 0.25f * sp->blotch / 100.f;
    p.wsum = p.wg + p.wf + p.wm;
    wb = warm * 44 / 100;
    p.wr = 255 - cool * 55 / 100;           /* highlight red: cool drops to 200 */
    p.wg_r = 250 + cool / 25 - warm / 7;    /* highlight green */
    p.wb_r = 236 + cool / 5 - wb;           /* highlight blue (255-capped when cool) */
    if (p.wb_r > 255) p.wb_r = 255;
    p.dr = 145 + warm / 8 - cool * 40 / 100;  /* shadow red   */
    p.dg = 133 + warm / 10 - cool / 8;        /* shadow green */
    p.db = 108 - wb + cool / 2;               /* shadow blue  */
    p.bias = sp->intensity / 4; p.spread = sp->intensity * 3 / 4;
    /* Fibre and blotch add their own alpha modulation on top of the mix,
     * scaled with the strength slider: unmistakable endpoints, 0 still off. */
    p.fspread = p.spread * 13 * sp->fibre / 10 / 100;   /* up to +1.3x spread */
    p.bspread = p.spread * 12 * sp->blotch / 10 / 100;  /* up to +1.2x spread */
    return p;
}

static void PaperPixel(unsigned char *out, int x, int y, const PAPERPARAMS *p) {
    /* fs scales ALL noise frequencies at once: the preview grid samples a
     * coarser paper (same character, larger features) instead of aliasing
     * fine grain into a flat smear. */
    float u = (float)x / p->fs, v = (float)y / p->fs;
    float grain = fbm(u / (float)p->s.grain, v / (float)p->s.grain, p->pg);
    float fibre = fbm(u / 2.f, v / 12.f, p->pf);
    float macro = fbm(u / 90.f, v / 70.f, p->pm);
    float n = (grain * p->wg + fibre * p->wf + macro * p->wm) / p->wsum;
    float d = (n - 0.5f) * 2.f;
    float mag = d < 0 ? -d : d;
    int ai = p->bias + (int)(mag * (float)p->spread)
           + (int)((fabsf(fibre - 0.5f) * 2.f) * (float)p->fspread)
           + (int)((fabsf(macro - 0.5f) * 2.f) * (float)p->bspread);
    int cr = d >= 0 ? p->wr : p->dr;
    int cg = d >= 0 ? p->wg_r : p->dg;
    int cb = d >= 0 ? p->wb_r : p->db;
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

static int BuildPaperFull(unsigned char *px, int w, int h, const SETTINGS *sp,
                          const PAPERCANCEL *cancel) {
    PAPERPARAMS p = PaperParams(w, sp);
    int x, y;
    for (y = 0; y < h; y++) {
        if (PaperCancelledOf(cancel)) return 0;
        for (x = 0; x < w; x++)
            PaperPixel(px + 4 * ((size_t)y * w + x), x, y, &p);
    }
    return 1;
}

/* The 8px preview sampler (BuildPaperPreview/ExpandPaperRow) lived here for
 * the settings GUI's live slider feedback.  The GUI is not built, so it was
 * dead code carrying its own test and a `(void)` reference to silence
 * -Wunused-function.  Deleted rather than shipped unused; restore it from git
 * history (commit "fix: codebase review findings") when the GUI lands. */

#endif /* MNPAPER_PAPER_CORE_H */
