/* tray.c - StatusNotifierItem + DBusMenu over sd-bus.  See tray.h. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

#include <systemd/sd-bus.h>

#include "tray.h"

/* The core's logger (declared in paper_core.h, defined in mnpaper.c). */
extern void PAPERLOG(const char *fmt, ...);
#define L PAPERLOG

/* ------------------------------------------------------------------ ids --- */

/* Fixed item ids: the menu is rebuilt from live state on every GetLayout, but
 * ids must be stable across revisions or Plasma's cached rows go stale. */
enum {
    ID_ROOT = 0,
    ID_SHOW = 1,
    ID_SEP1 = 2,
    ID_STRENGTH = 3,
    ID_STRENGTH_BASE = 30,      /* + index 0..3 -> 31..34 */
    ID_WARMTH = 4,
    ID_WARMTH_BASE = 40,        /* + index 0..3 -> 41..44 */
    ID_SEP2 = 5,
    ID_AUTOSTART = 6,
    ID_EDITCFG = 7,
    ID_SEP3 = 8,
    ID_QUIT = 9,
    /* The three texture rows the tray used to be missing: grain, fibre and
     * blotch are consumed by PaperParams, so a preset here changes the
     * baked texture exactly like --set does.  Bases are spaced 20 apart so
     * ten steps each fit without touching any other id. */
    ID_GRAIN = 10,
    ID_GRAIN_BASE = 100,        /* + index 0..9 -> 101..110 */
    ID_FIBRE = 11,
    ID_FIBRE_BASE = 120,        /* + index 0..9 -> 121..130 */
    ID_BLOTCH = 12,
    ID_BLOTCH_BASE = 140,       /* + index 0..9 -> 141..150 */
};

/* Work bound for one D-Bus call.  This menu has 50 items (12 root rows plus
 * the four-, four- and three ten-row submenus) and a real host sends a few
 * dozen ids or events at most, so this is far above any genuine
 * batch while still stopping a hostile flood from stalling the poll loop.
 * The bound is on the WORK, not just on what is answered: reading stops at
 * it, so a caller sending a million ids costs the same as one sending a
 * hundred.  A call over the bound is refused with a D-Bus error and changes
 * nothing: MenuGetGroupProperties sends no reply at all and
 * MenuEventGroup applies no event, so a refused call never leaves a
 * half-done state behind. */
#define MENU_BATCH_MAX 256

/* Bound for every method call this connection makes.  Without one, sd-bus'
 * default is 25 s, and the register call at startup would keep the daemon out
 * of its poll loop for that long whenever the watcher owns the name but is
 * wedged (a login still bringing up plasmashell, a frozen panel).  5 s is
 * generous for a healthy watcher (measured 1-2 ms to register against a live
 * one) and short enough that the overlay reaches its loop, the CLI and the
 * signal handling promptly. */
#define TRAY_CALL_TIMEOUT_US (5u * 1000000u)

/* The watcher name is what we re-register against: plasmashell hosts it, and
 * a panel restart or crash replaces it with an empty list. */
#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_MATCH \
    "type='signal',sender='org.freedesktop.DBus'," \
    "interface='org.freedesktop.DBus',member='NameOwnerChanged'," \
    "path='/org/freedesktop/DBus',arg0='" WATCHER_NAME "'"

static const char *STRENGTH_NAMES[4] = { "Subtle (10)", "Soft (20)",
                                         "Medium (30)", "Strong (40)" };
static const int STRENGTH_STEPS[4] = { 10, 20, 30, 40 };
static const char *WARMTH_NAMES[4] = { "Cool (10)", "Neutral (45)",
                                       "Warm (75)", "Aged (95)" };
static const int WARMTH_STEPS[4] = { 10, 45, 75, 95 };

/* Ten steps for each texture row, spanning the same bounds ClampSettingsOf
 * enforces.  Every list contains the setting's default, so a fresh install
 * shows a checked row instead of none.  Grain is an integer period in pixels
 * (2..12); fibre and blotch are 0..100 weights, so they share one table. */
#define TEX_STEPS 10
static const int GRAIN_STEPS[TEX_STEPS] =
    { 2, 3, 4, 5, 6, 7, 8, 9, 10, 12 };
static const char *GRAIN_NAMES[TEX_STEPS] =
    { "2", "3", "4", "5", "6", "7", "8", "9", "10", "12" };
static const int TENS_STEPS[TEX_STEPS] =
    { 0, 10, 20, 30, 40, 50, 60, 70, 80, 100 };
static const char *TENS_NAMES[TEX_STEPS] =
    { "0", "10", "20", "30", "40", "50", "60", "70", "80", "100" };

static const struct TRAYCB *cb;
static sd_bus *bus;
static char service[96];        /* well-known name requested on the bus */
static uint32_t revision = 1;   /* menu revision: bumped on every change */

/* ------------------------------------------------------------- the icon --- */

/* The mnPaper tray icon: the mn-brand artwork (mnpaper.png, 2048x2048)
 * downscaled to 24x24. Same artwork as the app icon (mnPaper.ico); this
 * table is that render baked in as pixels so the tray still ships no
 * binary asset. Regenerate from
 * /home/tyan/mnsky/mnbrand/icons/mnpaper.png (never from the 300x300 control
 * copy, and never hand-edit: the artwork changes in the mn-brand pipeline).
 *
 * D-Bus ARGB32 is big-endian per pixel: bytes are A, R, G, B. */
static const unsigned char ICON_PIX[24 * 24 * 4] = {
    255,239,236,232, 255,239,236,232, 255,238,235,231, 255,239,237,233, 255,240,237,233, 255,240,237,233, 255,240,237,233, 255,240,237,233, 255,240,237,233, 255,240,237,233, 255,240,237,233, 255,240,237,233, 255,240,237,233, 255,240,237,233, 255,240,237,233, 255,240,237,233, 255,240,237,233, 255,240,237,233, 255,240,237,233, 255,240,237,233, 255,239,236,232, 255,238,235,231, 255,239,236,232, 255,239,236,232,
    255,239,236,232, 255,239,236,232, 255,244,241,238, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,241, 255,243,241,237, 255,238,235,231, 255,239,236,232,
    255,238,235,231, 255,244,241,238, 255,248,246,244, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,248,247,244, 255,243,240,237, 255,238,235,231,
    255,239,236,232, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,247,245,242, 255,246,244,241, 255,239,235,231,
    255,239,236,232, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,242, 255,247,245,242, 255,246,245,241, 255,247,245,241, 255,247,245,241, 255,247,245,241, 255,239,236,232,
    255,239,236,232, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,242, 255,243,242,238, 255,243,242,238, 255,247,244,242, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,238,236,231,
    255,239,236,232, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,245,243,240, 255,245,243,240, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,248,245,243, 255,238,239,230, 255,227,233,217, 255,226,232,216, 255,236,238,228, 255,245,244,240, 255,246,244,241, 255,246,244,241, 255,238,236,231,
    255,239,236,232, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,245,243,240, 255,245,242,239, 255,254,252,249, 255,250,248,245, 255,246,243,240, 255,246,244,241, 255,246,244,241, 255,246,244,241, 255,245,243,240, 255,246,244,241, 255,246,244,241, 255,235,238,227, 255,227,234,217, 255,230,236,219, 255,235,241,224, 255,229,236,218, 255,238,239,230, 255,247,245,243, 255,246,244,240, 255,238,236,231,
    255,239,236,232, 255,246,244,241, 255,246,244,241, 255,244,241,238, 255,251,249,246, 255,254,252,249, 255,211,209,206, 255,227,225,222, 255,247,245,242, 255,243,241,238, 255,246,244,241, 255,245,243,240, 255,250,248,245, 255,247,245,243, 255,238,239,231, 255,226,232,215, 255,229,235,219, 255,220,226,211, 255,191,195,182, 255,211,216,201, 255,235,238,227, 255,247,245,242, 255,246,244,240, 255,238,236,231,
    255,239,236,232, 255,246,244,240, 255,244,242,238, 255,255,255,252, 255,225,224,221, 255,159,157,154, 255,146,144,141, 255,158,156,153, 255,255,255,255, 255,255,254,250, 255,246,244,240, 255,253,251,247, 255,229,227,223, 255,248,246,242, 255,243,246,235, 255,227,233,217, 255,227,233,217, 255,158,160,151, 255,175,178,167, 255,153,154,145, 255,216,221,207, 255,245,245,238, 255,246,244,240, 255,238,235,231,
    255,239,236,232, 255,245,243,239, 255,253,252,249, 255,191,190,187, 255,146,143,140, 255,229,229,227, 255,195,193,189, 255,133,131,128, 255,148,146,143, 255,167,165,162, 255,243,241,237, 255,194,192,188, 255,97,94,92, 255,151,149,145, 255,156,158,148, 255,236,242,226, 255,238,245,228, 255,178,182,170, 255,167,170,159, 255,162,164,154, 255,216,222,205, 255,241,243,233, 255,247,244,241, 255,238,235,231,
    255,237,234,230, 255,251,250,248, 255,182,180,177, 255,165,163,160, 255,253,253,251, 255,236,234,231, 255,79,76,74, 255,171,169,166, 255,113,111,108, 255,126,123,121, 255,147,145,142, 255,156,154,151, 255,104,101,99, 255,232,232,229, 255,186,185,179, 255,144,145,136, 255,185,189,176, 255,186,190,178, 255,194,197,185, 255,211,216,201, 255,232,237,223, 255,244,243,238, 255,246,244,240, 255,238,235,231,
    255,239,236,232, 255,245,243,241, 255,228,226,223, 255,251,249,246, 255,253,252,250, 255,197,195,192, 255,176,174,171, 255,252,250,248, 255,227,225,222, 255,243,241,238, 255,221,219,216, 255,235,233,230, 255,228,225,222, 255,253,252,249, 255,253,251,248, 255,218,218,210, 255,184,185,175, 255,208,212,198, 255,235,241,224, 255,234,238,225, 255,243,242,238, 255,246,243,241, 255,245,243,240, 255,238,235,231,
    255,239,236,232, 255,245,243,240, 255,249,247,244, 255,244,242,239, 255,244,242,239, 255,249,247,244, 255,255,253,250, 255,244,242,239, 255,249,247,244, 255,246,244,241, 255,251,249,246, 255,247,245,242, 255,249,247,244, 255,243,241,238, 255,243,241,239, 255,250,248,245, 255,255,253,249, 255,245,245,238, 255,235,236,227, 255,243,241,237, 255,246,244,241, 255,245,243,240, 255,245,243,240, 255,238,235,231,
    255,239,236,232, 255,245,243,239, 255,244,242,239, 255,245,243,239, 255,245,243,239, 255,244,242,239, 255,243,241,238, 255,244,242,239, 255,244,242,239, 255,244,242,239, 255,244,242,238, 255,245,243,239, 255,243,242,238, 255,245,243,239, 255,245,243,239, 255,244,242,239, 255,242,240,237, 255,245,243,240, 255,246,243,241, 255,246,244,240, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,238,235,231,
    255,239,236,232, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,246,244,240, 255,245,243,238, 255,244,242,238, 255,247,245,241, 255,245,243,239, 255,246,243,239, 255,246,244,240, 255,244,242,237, 255,247,245,241, 255,246,244,240, 255,245,243,239, 255,244,242,238, 255,251,249,245, 255,245,243,238, 255,246,244,240, 255,243,241,236, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,238,235,231,
    255,239,236,232, 255,245,243,239, 255,244,242,238, 255,250,248,244, 255,223,221,217, 255,166,163,160, 255,174,171,168, 255,183,181,178, 255,172,170,166, 255,185,183,180, 255,204,206,198, 255,199,201,192, 255,206,207,199, 255,200,203,193, 255,201,203,194, 255,200,203,194, 255,194,197,187, 255,206,207,199, 255,188,192,181, 255,217,218,211, 255,249,247,243, 255,244,242,238, 255,245,243,239, 255,238,235,231,
    255,239,236,232, 255,245,243,239, 255,244,242,238, 255,251,250,246, 255,217,215,211, 255,170,168,165, 255,182,180,176, 255,162,159,156, 255,177,174,171, 255,179,177,174, 255,187,189,179, 255,202,204,195, 255,180,184,173, 255,178,183,171, 255,185,189,178, 255,205,207,198, 255,169,175,161, 255,210,212,203, 255,192,196,185, 255,238,237,232, 255,247,245,241, 255,245,243,239, 255,245,243,239, 255,238,235,231,
    255,239,236,232, 255,245,243,239, 255,245,243,239, 255,245,244,240, 255,241,239,235, 255,235,233,229, 255,236,234,231, 255,234,232,229, 255,235,233,229, 255,241,238,235, 255,192,195,186, 255,228,228,222, 255,242,240,236, 255,238,237,232, 255,191,194,184, 255,233,232,227, 255,247,245,242, 255,234,233,228, 255,239,238,233, 255,243,241,237, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,238,235,231,
    255,239,236,232, 255,244,242,239, 255,244,242,239, 255,244,242,239, 255,245,243,240, 255,247,245,241, 255,246,244,241, 255,247,245,241, 255,246,244,241, 255,246,244,241, 255,242,240,236, 255,245,243,240, 255,246,243,240, 255,246,244,241, 255,242,240,236, 255,245,243,240, 255,244,242,239, 255,247,244,241, 255,246,243,240, 255,244,242,239, 255,244,242,239, 255,244,242,239, 255,244,242,239, 255,238,235,231,
    255,237,234,230, 255,243,241,237, 255,245,243,239, 255,244,242,238, 255,244,242,238, 255,243,241,237, 255,243,241,237, 255,243,241,237, 255,243,241,237, 255,243,241,237, 255,245,242,239, 255,244,242,238, 255,243,241,237, 255,243,241,237, 255,245,243,239, 255,244,242,238, 255,244,242,237, 255,243,241,237, 255,243,241,237, 255,244,242,238, 255,244,242,238, 255,245,243,239, 255,242,240,236, 255,237,234,230,
    255,238,235,231, 255,237,234,230, 255,244,242,238, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,245,243,239, 255,244,242,237, 255,237,234,230, 255,238,235,231,
    255,239,236,232, 255,238,234,230, 255,236,233,228, 255,239,236,232, 255,239,237,233, 255,239,237,233, 255,239,237,233, 255,239,237,233, 255,239,237,233, 255,239,237,233, 255,239,237,233, 255,239,237,233, 255,239,237,233, 255,239,237,233, 255,239,237,233, 255,239,237,233, 255,239,237,233, 255,239,237,233, 255,239,237,233, 255,239,237,233, 255,238,236,232, 255,236,233,228, 255,238,235,231, 255,239,236,232,
    255,239,236,232, 255,239,236,232, 255,239,236,232, 255,237,234,229, 255,236,233,229, 255,236,233,229, 255,236,233,229, 255,236,233,229, 255,236,233,229, 255,236,233,229, 255,236,233,229, 255,236,233,229, 255,236,233,229, 255,236,233,229, 255,236,233,229, 255,236,233,229, 255,236,233,229, 255,236,233,229, 255,236,233,229, 255,236,233,229, 255,237,234,230, 255,239,236,232, 255,239,236,232, 255,239,236,232,
};


/* ----------------------------------------------------------- menu model --- */

static int StrengthIndex(void) {
    int i, v = cb->query_intensity();
    for (i = 0; i < 4; i++) if (STRENGTH_STEPS[i] == v) return i;
    return -1;
}
static int WarmthIndex(void) {
    int i, v = cb->query_warmth();
    for (i = 0; i < 4; i++) if (WARMTH_STEPS[i] == v) return i;
    return -1;
}
/* -1 means "the current value is not one of the presets" (a hand-edited
 * config, or a value set with --set): then no radio is checked, which is the
 * honest answer and is what the existing two submenus already do. */
static int StepIndex(const int *steps, int v) {
    int i;
    for (i = 0; i < TEX_STEPS; i++) if (steps[i] == v) return i;
    return -1;
}
static int GrainIndex(void)  { return StepIndex(GRAIN_STEPS, cb->query_grain()); }
static int FibreIndex(void)  { return StepIndex(TENS_STEPS, cb->query_fibre()); }
static int BlotchIndex(void) { return StepIndex(TENS_STEPS, cb->query_blotch()); }

/* One authority for "is this id a radio row, which row is it, which row of its
 * submenu is checked, what value does it set, what does it say, and which
 * callback applies it".  Returns 1 and fills the out-params when id is a radio
 * row; *cur is -1 when the current value matches no preset, so no row of that
 * submenu is checked.  The layout (labels, toggle-type, toggle-state), the
 * click handler and the incremental ItemsPropertiesUpdated push all read this,
 * so a row whose state can move in one channel can never be missing from the
 * other, and a new submenu only has to be added here. */
static int RadioRow(int32_t id, int *row, int *cur, int *step,
                    const char **name, void (**set)(int)) {
    int si = id - ID_STRENGTH_BASE - 1;
    int wi = id - ID_WARMTH_BASE - 1;
    int gi = id - ID_GRAIN_BASE - 1;
    int fi = id - ID_FIBRE_BASE - 1;
    int bi = id - ID_BLOTCH_BASE - 1;
    if (si >= 0 && si < 4) {
        *row = si; *cur = StrengthIndex();
        *step = STRENGTH_STEPS[si]; *name = STRENGTH_NAMES[si];
        *set = cb->set_intensity; return 1;
    }
    if (wi >= 0 && wi < 4) {
        *row = wi; *cur = WarmthIndex();
        *step = WARMTH_STEPS[wi]; *name = WARMTH_NAMES[wi];
        *set = cb->set_warmth; return 1;
    }
    if (gi >= 0 && gi < TEX_STEPS) {
        *row = gi; *cur = GrainIndex();
        *step = GRAIN_STEPS[gi]; *name = GRAIN_NAMES[gi];
        *set = cb->set_grain; return 1;
    }
    if (fi >= 0 && fi < TEX_STEPS) {
        *row = fi; *cur = FibreIndex();
        *step = TENS_STEPS[fi]; *name = TENS_NAMES[fi];
        *set = cb->set_fibre; return 1;
    }
    if (bi >= 0 && bi < TEX_STEPS) {
        *row = bi; *cur = BlotchIndex();
        *step = TENS_STEPS[bi]; *name = TENS_NAMES[bi];
        *set = cb->set_blotch; return 1;
    }
    return 0;
}

static int MenuChildCount(int id) {
    switch (id) {
    case ID_ROOT: return 12;
    case ID_STRENGTH: return 4;
    case ID_WARMTH: return 4;
    case ID_GRAIN: return TEX_STEPS;
    case ID_FIBRE: return TEX_STEPS;
    case ID_BLOTCH: return TEX_STEPS;
    default: return 0;
    }
}
static int MenuChild(int id, int idx) {
    switch (id) {
    case ID_ROOT: {
        static const int root[12] = { ID_SHOW, ID_SEP1, ID_STRENGTH, ID_WARMTH,
                                      ID_GRAIN, ID_FIBRE, ID_BLOTCH,
                                      ID_SEP2, ID_AUTOSTART, ID_EDITCFG,
                                      ID_SEP3, ID_QUIT };
        return root[idx];
    }
    case ID_STRENGTH: return ID_STRENGTH_BASE + 1 + idx;
    case ID_WARMTH: return ID_WARMTH_BASE + 1 + idx;
    case ID_GRAIN: return ID_GRAIN_BASE + 1 + idx;
    case ID_FIBRE: return ID_FIBRE_BASE + 1 + idx;
    case ID_BLOTCH: return ID_BLOTCH_BASE + 1 + idx;
    default: return -1;
    }
}

static int MenuIsSeparator(int id) {
    return id == ID_SEP1 || id == ID_SEP2 || id == ID_SEP3;
}

/* The model above is the only authority on which ids exist: every method that
 * takes an id must agree with it, or the tray publishes items that no layout
 * ever contained. */
static int MenuIdValid(int32_t id) {
    int i, j;
    if (id == ID_ROOT) return 1;
    for (i = 0; i < MenuChildCount(ID_ROOT); i++) {
        int sub = MenuChild(ID_ROOT, i);
        if (sub == id) return 1;
        for (j = 0; j < MenuChildCount(sub); j++)
            if (MenuChild(sub, j) == id) return 1;
    }
    return 0;
}

/* sd_bus_message_append returns 0 on success; PropAppend reports 1 appended. */
static int Appended(int r) { return r < 0 ? r : 1; }

/* One "{sv}" property.  Returns 1 appended, 0 skipped (the property does not
 * apply to this id), negative errno when the append failed.  The DBusMenu spec
 * lets a server return more properties than the caller asked for, so the
 * propertyNames filters are not implemented. */
static int PropAppend(sd_bus_message *m, int32_t id, const char *key) {
    if (!strcmp(key, "type"))
        return Appended(sd_bus_message_append(m, "{sv}", "type", "s",
                                 MenuIsSeparator(id) ? "separator" : "standard"));
    if (!strcmp(key, "label")) {
        const char *label = NULL;
        switch (id) {
        case ID_SHOW: label = "Show paper"; break;
        case ID_STRENGTH: label = "Strength"; break;
        case ID_WARMTH: label = "Warmth"; break;
        case ID_GRAIN: label = "Grain"; break;
        case ID_FIBRE: label = "Fibre"; break;
        case ID_BLOTCH: label = "Blotch"; break;
        case ID_AUTOSTART: label = "Start at login"; break;
        case ID_EDITCFG: label = "Edit settings file..."; break;
        case ID_QUIT: label = "Quit"; break;
        default: {
            int row, cur, step;
            const char *name = NULL;
            void (*set)(int);
            if (RadioRow(id, &row, &cur, &step, &name, &set)) label = name;
            break;
        }
        }
        if (!label) return 0;
        return Appended(sd_bus_message_append(m, "{sv}", "label", "s", label));
    }
    if (!strcmp(key, "enabled") || !strcmp(key, "visible")) {
        if (MenuIsSeparator(id)) return 0;
        return Appended(sd_bus_message_append(m, "{sv}", key, "b", 1));
    }
    if (!strcmp(key, "toggle-type")) {
        const char *t = NULL;
        int row, cur, step;
        const char *name;
        void (*set)(int);
        if (id == ID_SHOW || id == ID_AUTOSTART) t = "checkmark";
        else if (RadioRow(id, &row, &cur, &step, &name, &set)) t = "radio";
        if (!t) return 0;
        return Appended(sd_bus_message_append(m, "{sv}", "toggle-type", "s", t));
    }
    if (!strcmp(key, "toggle-state")) {
        int on, row, cur, step;
        const char *name;
        void (*set)(int);
        if (id == ID_SHOW) on = cb->query_master() ? 1 : 0;
        else if (id == ID_AUTOSTART) on = cb->query_autostart() ? 1 : 0;
        else if (RadioRow(id, &row, &cur, &step, &name, &set) && cur >= 0)
            on = (cur == row);
        else return 0;   /* not a toggle row, or no preset matches at all */
        return Appended(sd_bus_message_append(m, "{sv}", "toggle-state", "i", on));
    }
    if (!strcmp(key, "children-display")) {
        if (!MenuChildCount(id)) return 0;
        return Appended(sd_bus_message_append(m, "{sv}", "children-display", "s",
                                              "submenu"));
    }
    return 0;
}

/* The "a{sv}" property map of one item. */
static int MenuAppendProps(sd_bus_message *m, int32_t id) {
    static const char *keys[] = { "type", "label", "enabled", "visible",
                                  "toggle-type", "toggle-state",
                                  "children-display" };
    int r, i;
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r < 0) return r;
    for (i = 0; i < (int)(sizeof keys / sizeof keys[0]); i++) {
        r = PropAppend(m, id, keys[i]);
        if (r < 0) return r;
    }
    return sd_bus_message_close_container(m);
}

/* Serialize one item as a "(ia{sv}av)".  wrap=1 embeds it in a VARIANT (what
 * children inside the "av" array need); wrap=0 appends the bare struct (what
 * the top-level GetLayout reply wants - the spec has no variant there).  At
 * the depth limit (depth == 0) children appear as empty variants, which the
 * spec allows and Plasma renders as stubs on the next fetch. */
static int MenuAppendItem(sd_bus_message *m, int id, int depth, int wrap) {
    int r, i, n = MenuChildCount(id);
    if (wrap) {
        r = sd_bus_message_open_container(m, SD_BUS_TYPE_VARIANT, "(ia{sv}av)");
        if (r < 0) return r;
    }
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "ia{sv}av");
    if (r < 0) return r;
    r = sd_bus_message_append(m, "i", id);
    if (r < 0) return r;
    r = MenuAppendProps(m, id);
    if (r < 0) return r;

    r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "v");
    if (r < 0) return r;
    if (n > 0 && depth != 0) {
        for (i = 0; i < n; i++) {
            r = MenuAppendItem(m, MenuChild(id, i), depth > 0 ? depth - 1 : -1, 1);
            if (r < 0) return r;
        }
    }
    r = sd_bus_message_close_container(m);
    if (r < 0) return r;
    r = sd_bus_message_close_container(m);
    if (r < 0) return r;
    if (wrap) {
        r = sd_bus_message_close_container(m);
        if (r < 0) return r;
    }
    return r;
}

/* ------------------------------------------------------ DBusMenu methods -- */

static int MenuGetLayout(sd_bus_message *msg, void *ud, sd_bus_error *err) {
    int parent, depth, r;
    sd_bus_message *reply;
    (void)ud;

    r = sd_bus_message_read(msg, "ii", &parent, &depth);
    if (r <= 0)
        return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                                 "malformed GetLayout body");
    /* The trailing propertyNames ("as") is deliberately ignored: every item
     * ships its full property set, which the spec allows.  It is not read at
     * all - a caller could put a million names there, and reading them would
     * cost the poll loop time for nothing. */
    if (!MenuIdValid(parent))
        return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                                 "unknown menu id %d", parent);

    r = sd_bus_message_new_method_return(msg, &reply);
    if (r < 0) return r;

    r = sd_bus_message_append(reply, "u", revision);
    if (r >= 0) r = MenuAppendItem(reply, parent, depth, 0);
    if (r >= 0) r = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    return r < 0 ? r : 1;
}

static int MenuGetGroupProperties(sd_bus_message *msg, void *ud,
                                  sd_bus_error *err) {
    sd_bus_message *reply = NULL;
    int r, n = 0, in_array = 0;
    (void)ud;
    /* Ids are streamed straight into the reply, so the request never has to
     * be buffered.  The property-name filter is ignored on purpose: we always
     * return everything we have, which the spec allows.  Reading stops at the
     * bound, so a caller sending a million ids costs the same work as one
     * sending a hundred. */
    r = sd_bus_message_enter_container(msg, SD_BUS_TYPE_ARRAY, "i");
    if (r < 0) return r;
    in_array = r > 0;    /* 0 means an empty array: no container was entered */
    r = sd_bus_message_new_method_return(msg, &reply);
    if (r < 0) { if (in_array) sd_bus_message_exit_container(msg); return r; }
    r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "(ia{sv})");
    for (; r >= 0 && n <= MENU_BATCH_MAX;) {
        int32_t id;
        r = sd_bus_message_read(msg, "i", &id);
        if (r <= 0) break;
        n++;
        /* one id past the bound is enough to know the call is over the
         * bound; reading the rest of the array is what must not happen */
        if (n > MENU_BATCH_MAX) break;
        if (!MenuIdValid(id)) continue;       /* never publish an unknown id */
        r = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT, "ia{sv}");
        if (r < 0) break;
        r = sd_bus_message_append(reply, "i", id);
        if (r < 0) break;
        r = MenuAppendProps(reply, id);
        if (r < 0) break;
        r = sd_bus_message_close_container(reply);
        if (r < 0) break;
    }
    if (in_array) sd_bus_message_exit_container(msg);
    /* The caller sent more ids than the bound allows: the loop stopped
     * reading, so refuse the whole call rather than answering a prefix as if
     * it were everything. */
    if (n > MENU_BATCH_MAX) {
        L("tray: GetGroupProperties asked for more than %d ids - refusing",
          MENU_BATCH_MAX);
        sd_bus_message_unref(reply);
        return sd_bus_error_setf(err, SD_BUS_ERROR_LIMITS_EXCEEDED,
                                 "more than %d ids in one call", MENU_BATCH_MAX);
    }
    if (r >= 0) r = sd_bus_message_close_container(reply);
    if (r >= 0) r = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    return r < 0 ? r : 1;
}

static int MenuAboutToShow(sd_bus_message *msg, void *ud, sd_bus_error *err) {
    int32_t id;
    sd_bus_message *reply;
    int r;
    (void)ud;
    r = sd_bus_message_read(msg, "i", &id);
    if (r <= 0)
        return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                                 "malformed AboutToShow body");
    r = sd_bus_message_new_method_return(msg, &reply);
    if (r < 0) return r;
    /* We push layout changes with LayoutUpdated, so nothing to flush here.
     * An id we do not know needs no update either, so the answer is false. */
    r = sd_bus_message_append(reply, "b", 0);
    if (r >= 0) r = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    return r < 0 ? r : 1;
}

/* One activation, shared by Event and EventGroup: the two entry points must
 * map exactly the same ids, or a batched click silently does nothing.
 * Returns 0 when the id is not in the model, so the caller can report it. */
static int MenuActivate(int32_t id, const char *event) {
    int row, cur, step;
    const char *name;
    void (*set)(int);
    if (!MenuIdValid(id)) return 0;
    if (strcmp(event, "clicked")) return 1;
    switch (id) {
    case ID_SHOW: cb->toggle_show(); break;
    case ID_AUTOSTART: cb->toggle_autostart(); break;
    case ID_EDITCFG: cb->edit_settings(); break;
    case ID_QUIT: cb->quit(); break;
    default:
        if (RadioRow(id, &row, &cur, &step, &name, &set)) set(step);
        break;
    }
    return 1;
}

static int MenuEvent(sd_bus_message *msg, void *ud, sd_bus_error *err) {
    int32_t id;
    const char *event;
    int r, rr;
    (void)ud;

    r = sd_bus_message_read(msg, "is", &id, &event);
    if (r <= 0)
        return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                                 "malformed Event body");
    r = sd_bus_message_skip(msg, "vu");   /* data is a variant of any type */
    if (r < 0)
        return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                                 "malformed Event body");
    MenuActivate(id, event);
    /* empty result: send the reply ourselves (see SniActivate) */
    rr = sd_bus_reply_method_return(msg, NULL);
    if (rr < 0) return rr;
    return 1;
}

static int MenuEventGroup(sd_bus_message *msg, void *ud, sd_bus_error *err) {
    /* The events are collected before any of them is applied: a refused call
     * must not have changed anything, and whether the call is over the bound
     * is only known once the array has been read.  The buffer is fixed size,
     * so the work stays bounded - at most MENU_BATCH_MAX+1 events are read,
     * and one past the bound is enough to refuse the call. */
    struct { int32_t id; const char *event; } ev[MENU_BATCH_MAX + 1];
    sd_bus_message *reply = NULL;
    int r, seen = 0, overflow = 0, malformed = 0, i;
    (void)ud;
    /* Plasma sends batched events only for keyboard activation; every one of
     * them has to be applied, not just the first.  An id the model does not
     * have is reported in idErrors.  A call that carried more than the bound
     * is refused with a D-Bus error, and none of the events it carried are
     * applied. */
    r = sd_bus_message_new_method_return(msg, &reply);
    if (r < 0) return r;
    r = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "i");
    if (r < 0) { sd_bus_message_unref(reply); return r; }

    r = sd_bus_message_enter_container(msg, SD_BUS_TYPE_ARRAY, "(isvu)");
    if (r < 0)
        malformed = 1;
    else if (r > 0) {
        for (;;) {
            int32_t id;
            const char *event;
            int bad = 0;
            r = sd_bus_message_enter_container(msg, SD_BUS_TYPE_STRUCT, "isvu");
            if (r < 0) { malformed = 1; break; }
            if (r == 0) break;       /* the array ends here */
            r = sd_bus_message_read(msg, "is", &id, &event);
            if (r <= 0)
                bad = 1;
            else {
                r = sd_bus_message_skip(msg, "vu");
                if (r < 0) bad = 1;
            }
            r = sd_bus_message_exit_container(msg);
            if (r < 0) bad = 1;
            if (bad) { malformed = 1; break; }
            ev[seen].id = id;
            ev[seen].event = event;
            seen++;
            if (seen > MENU_BATCH_MAX) {
                /* one event past the bound: the call is refused below and
                 * none of the events are applied */
                overflow = 1;
                break;
            }
        }
        sd_bus_message_exit_container(msg);
    }

    if (malformed) {
        sd_bus_message_unref(reply);
        return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                                 "malformed EventGroup body");
    }
    if (overflow) {
        /* The caller's events past the bound were never read, so the call is
         * refused instead of answered: a host must never read a truncated
         * batch as "everything else was applied".  Nothing is applied - the
         * events were collected, not run. */
        sd_bus_message_unref(reply);
        L("tray: EventGroup refused - more than %d events in one call, "
          "nothing applied", MENU_BATCH_MAX);
        return sd_bus_error_setf(err, SD_BUS_ERROR_LIMITS_EXCEEDED,
                                 "more than %d events in one call", MENU_BATCH_MAX);
    }
    for (i = 0; i < seen; i++) {
        if (!MenuActivate(ev[i].id, ev[i].event)) {
            /* unapplied: an id the model does not have */
            int ar = sd_bus_message_append(reply, "i", ev[i].id);
            if (ar < 0) { sd_bus_message_unref(reply); return ar; }
        }
    }
    r = sd_bus_message_close_container(reply);
    if (r >= 0) r = sd_bus_send(NULL, reply, NULL);
    sd_bus_message_unref(reply);
    return r < 0 ? r : 1;
}

/* --------------------------------------------------- StatusNotifierItem --- */

static int SniGetStr(sd_bus *b, const char *path, const char *iface,
                     const char *prop, sd_bus_message *m, void *ud,
                     sd_bus_error *err) {
    const char *v = "";
    (void)b; (void)path; (void)iface; (void)ud; (void)err;
    if (!strcmp(prop, "Id")) v = "mnpaper";
    else if (!strcmp(prop, "Category")) v = "ApplicationStatus";
    else if (!strcmp(prop, "Title")) v = "mnPaper";
    else if (!strcmp(prop, "Status")) v = cb->query_master() ? "Active" : "Passive";
    else if (!strcmp(prop, "IconName")) v = "";
    return sd_bus_message_append(m, "s", v);
}

static int MenuGetStatus(sd_bus *b, const char *path, const char *iface,
                         const char *prop, sd_bus_message *m, void *ud,
                         sd_bus_error *err) {
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    /* DBusMenu's Status is "normal" or "notice"; Active/Passive is the SNI
     * vocabulary and belongs to the other interface's getter. */
    return sd_bus_message_append(m, "s", "normal");
}

static int SniGetMenuPath(sd_bus *b, const char *path, const char *iface,
                          const char *prop, sd_bus_message *m, void *ud,
                          sd_bus_error *err) {
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    /* Menu is an OBJECT PATH ("o"), not a string - a type mismatch here
     * fails every GetAll, which makes the tray shell retry in a loop. */
    return sd_bus_message_append(m, "o", "/StatusNotifierItem");
}

static int SniGetIconPixmap(sd_bus *b, const char *path, const char *iface,
                            const char *prop, sd_bus_message *m, void *ud,
                            sd_bus_error *err) {
    int r;
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;

    /* IconPixmap is a(iiay): an ARRAY of (w, h, bytes) icons - one element
     * here.  sd-bus enforces the container walk against the signature, so
     * the containers must follow the type exactly. */
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "(iiay)");
    if (r < 0) return r;
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "iiay");
    if (r < 0) return r;
    r = sd_bus_message_append(m, "ii", 24, 24);
    if (r < 0) return r;
    r = sd_bus_message_append_array(m, 'y', ICON_PIX, sizeof ICON_PIX);
    if (r < 0) return r;
    r = sd_bus_message_close_container(m);
    if (r < 0) return r;
    return sd_bus_message_close_container(m);
}

static int SniGetBool(sd_bus *b, const char *path, const char *iface,
                      const char *prop, sd_bus_message *m, void *ud,
                      sd_bus_error *err) {
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    /* ItemIsMenu=0: the item has a primary action, so a left click is sent as
     * Activate and the menu opens on right click. */
    return sd_bus_message_append(m, "b", 0);
}

static int SniGetInt(sd_bus *b, const char *path, const char *iface,
                     const char *prop, sd_bus_message *m, void *ud,
                     sd_bus_error *err) {
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    return sd_bus_message_append(m, "i", 0);
}

static int SniGetUint(sd_bus *b, const char *path, const char *iface,
                      const char *prop, sd_bus_message *m, void *ud,
                      sd_bus_error *err) {
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    /* DBusMenu Version is "u"; appending "i" mismatches the vtable type. */
    return sd_bus_message_append(m, "u", 3);
}

static int SniGetToolTip(sd_bus *b, const char *path, const char *iface,
                         const char *prop, sd_bus_message *m, void *ud,
                         sd_bus_error *err) {
    int r;
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;

    /* ToolTip is (s a(iiay) s s): the pixmap sits in an array like above. */
    r = sd_bus_message_open_container(m, 'r', "sa(iiay)ss");
    if (r < 0) return r;
    r = sd_bus_message_append(m, "s", "");
    if (r < 0) return r;
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_ARRAY, "(iiay)");
    if (r < 0) return r;
    r = sd_bus_message_open_container(m, SD_BUS_TYPE_STRUCT, "iiay");
    if (r < 0) return r;
    r = sd_bus_message_append(m, "ii", 24, 24);
    if (r < 0) return r;
    r = sd_bus_message_append_array(m, 'y', ICON_PIX, sizeof ICON_PIX);
    if (r < 0) return r;
    r = sd_bus_message_close_container(m);
    if (r < 0) return r;
    r = sd_bus_message_close_container(m);
    if (r < 0) return r;
    r = sd_bus_message_append(m, "ss", "mnPaper",
                              cb->query_master()
                                ? "A soft paper texture over your screen"
                                : "Paper hidden - click Show paper");
    if (r < 0) return r;
    return sd_bus_message_close_container(m);
}

static int SniActivate(sd_bus_message *msg, void *ud, sd_bus_error *err) {
    int r;
    (void)ud; (void)err;
    cb->toggle_show();
    /* sd-bus vtable convention: a method handler that returns 0 means "no
     * matching object" (the caller gets an UnknownMethod error), and a
     * positive return means "I will send the reply myself".  These methods
     * have an empty result, so send that empty reply and return 1. */
    r = sd_bus_reply_method_return(msg, NULL);
    if (r < 0) return r;
    return 1;
}

static int SniScroll(sd_bus_message *msg, void *ud, sd_bus_error *err) {
    int32_t delta;
    const char *orient;
    int r;
    (void)ud;
    r = sd_bus_message_read(msg, "is", &delta, &orient);
    if (r <= 0)
        return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS,
                                 "malformed Scroll body");
    /* Scroll up/down steps through the strength presets, like a volume icon. */
    {
        int i = StrengthIndex(), v;
        if (i < 0) i = 2;
        i += delta < 0 ? -1 : 1;
        if (i < 0) i = 0;
        if (i > 3) i = 3;
        v = STRENGTH_STEPS[i];
        if (v != cb->query_intensity()) cb->set_intensity(v);
    }
    {   /* empty result: send the reply ourselves (see SniActivate) */
        int rr = sd_bus_reply_method_return(msg, NULL);
        if (rr < 0) return rr;
    }
    return 1;
}

static const sd_bus_vtable sni_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Id", "s", SniGetStr, 0, 0),
    SD_BUS_PROPERTY("Category", "s", SniGetStr, 0, 0),
    SD_BUS_PROPERTY("Title", "s", SniGetStr, 0, 0),
    SD_BUS_PROPERTY("Status", "s", SniGetStr, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("WindowId", "i", SniGetInt, 0, 0),
    SD_BUS_PROPERTY("IconName", "s", SniGetStr, 0, 0),
    SD_BUS_PROPERTY("IconPixmap", "a(iiay)", SniGetIconPixmap, 0, 0),
    SD_BUS_PROPERTY("ToolTip", "(sa(iiay)ss)", SniGetToolTip, 0,
                    SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("Menu", "o", SniGetMenuPath, 0, 0),
    SD_BUS_PROPERTY("ItemIsMenu", "b", SniGetBool, 0, 0),
    SD_BUS_METHOD("Activate", "ii", "", SniActivate, 0),
    SD_BUS_METHOD("Scroll", "is", "", SniScroll, 0),
    SD_BUS_VTABLE_END,
};

static const sd_bus_vtable menu_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Version", "u", SniGetUint, 0, 0),   /* DBusMenu v3 */
    SD_BUS_PROPERTY("Status", "s", MenuGetStatus, 0, 0),
    SD_BUS_METHOD("GetLayout", "iias", "u(ia{sv}av)", MenuGetLayout, 0),
    SD_BUS_METHOD("GetGroupProperties", "aias", "a(ia{sv})",
                  MenuGetGroupProperties, 0),
    SD_BUS_METHOD("AboutToShow", "i", "b", MenuAboutToShow, 0),
    SD_BUS_METHOD("Event", "isvu", "", MenuEvent, 0),
    SD_BUS_METHOD("EventGroup", "a(isvu)", "ai", MenuEventGroup, 0),
    SD_BUS_VTABLE_END,
};

/* -------------------------------------------------------------- lifecycle -- */

/* Whether the watcher knows us.  The registration is attempted once at
 * startup and again whenever the watcher's owner changes; until one of those
 * attempts goes through the icon is simply not there. */
static int registered;

/* Register with the watcher without waiting for the reply: a wedged watcher
 * must never hold the poll loop, and the reply is only reported. */
static int TrayRegisterCb(sd_bus_message *m, void *ud, sd_bus_error *ret_error) {
    const sd_bus_error *e;
    (void)ud; (void)ret_error;
    e = sd_bus_message_get_error(m);
    if (e) {
        L("tray: registration did not go through (%s) - the icon stays hidden "
          "until a watcher answers",
          e->message ? e->message : e->name);
        return 0;
    }
    if (registered)
        L("tray: re-registered as %s", service);
    else
        L("tray: registered as %s", service);
    registered = 1;
    return 0;
}

static void TrayRegisterAsync(void) {
    int r;
    if (!bus) return;
    r = sd_bus_call_method_async(bus, NULL, WATCHER_NAME,
                                 "/StatusNotifierWatcher", WATCHER_NAME,
                                 "RegisterStatusNotifierItem", TrayRegisterCb,
                                 NULL, "s", service);
    if (r < 0)
        L("tray: re-register send failed (%s)", strerror(-r));
}

/* org.freedesktop.DBus.NameOwnerChanged for the watcher name.  A panel
 * restart or a plasmashell crash replaces the watcher with one holding an
 * empty item list, and the icon is only recovered by registering again - what
 * KDE's own KStatusNotifierItem does on a watcher owner change.  A watcher
 * that only shows up after the daemon started (at login plasmashell often
 * comes up later) is joined the same way.  The match rule pins arg0 to the
 * watcher name, so only the owner pair is of interest: a vanishing owner
 * needs no action, a new one is a watcher to join. */
static int TrayWatcherChanged(sd_bus_message *m, void *ud, sd_bus_error *ret_error) {
    const char *name, *old_owner, *new_owner;
    int r;
    (void)ud; (void)ret_error;
    r = sd_bus_message_read(m, "sss", &name, &old_owner, &new_owner);
    if (r < 0) {
        L("tray: unreadable NameOwnerChanged (%s) - ignoring", strerror(-r));
        return 0;
    }
    if (!new_owner || !*new_owner) return 0;
    TrayRegisterAsync();
    return 0;
}

/* Reported when the NameOwnerChanged rule could not be installed: the tray
 * works, but a later watcher restart will not be picked up.  A callback here
 * is not optional - with NULL, sd-bus' own default terminates the whole bus
 * connection when installing the match fails, which would take the tray down
 * instead of only losing the restart recovery. */
static int TrayMatchFailed(sd_bus_message *m, void *ud, sd_bus_error *ret_error) {
    const sd_bus_error *e;
    (void)ud; (void)ret_error;
    e = sd_bus_message_get_error(m);
    if (e)
        L("tray: cannot watch %s for restarts (%s) - the icon will not come "
          "back on its own if the panel is restarted, CLI still works",
          WATCHER_NAME, e->message ? e->message : e->name);
    return 0;
}

int tray_start(const struct TRAYCB *c) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r;

    cb = c;
    if (!cb) return -1;

    r = sd_bus_open_user(&bus);
    if (r < 0) {
        L("tray: no session bus (%s) - tray disabled, CLI still works",
          strerror(-r));
        return -1;
    }

    /* Bound every method call this connection makes, in particular the
     * registration below: sd-bus' 25 s default would keep the daemon out of
     * its poll loop for that long whenever the watcher owns the name but is
     * wedged, leaving the CLI unanswered and a stop or logout sitting in the
     * blocked signalfd. */
    r = sd_bus_set_method_call_timeout(bus, TRAY_CALL_TIMEOUT_US);
    if (r < 0) goto fail;

    r = sd_bus_add_object_vtable(bus, NULL, "/StatusNotifierItem",
                                 "org.kde.StatusNotifierItem",
                                 sni_vtable, NULL);
    if (r < 0) goto fail;
    r = sd_bus_add_object_vtable(bus, NULL, "/StatusNotifierItem",
                                 "com.canonical.dbusmenu",
                                 menu_vtable, NULL);
    if (r < 0) goto fail;

    snprintf(service, sizeof service, "org.kde.StatusNotifierItem-%d-1",
             (int)getpid());
    r = sd_bus_request_name(bus, service, SD_BUS_NAME_REPLACE_EXISTING);
    if (r < 0) goto fail;

    signal(SIGCHLD, SIG_IGN);   /* the Edit-config child is fire and forget */

    /* Watch the watcher: the registration below is tried once at startup,
     * and this signal is what picks up a watcher that appears later or
     * replaces the one we registered with - a panel restart would otherwise
     * leave the icon gone until mnpaper itself is restarted.  Async on
     * purpose - the synchronous sd_bus_add_match sends its own AddMatch
     * call and waits on sd-bus' built-in 25 s, which no connection setting
     * shortens (measured: 25.03 s against a wedged bus), and a match that
     * cannot be installed is a lost restart recovery, not a reason to drop
     * the tray the user already has. */
    r = sd_bus_add_match_async(bus, NULL, WATCHER_MATCH, TrayWatcherChanged,
                               TrayMatchFailed, NULL);
    if (r < 0)
        L("tray: cannot watch %s for restarts (%s) - tray still works",
          WATCHER_NAME, strerror(-r));

    r = sd_bus_call_method(bus, WATCHER_NAME,
                           "/StatusNotifierWatcher",
                           WATCHER_NAME,
                           "RegisterStatusNotifierItem", &err, &reply, "s",
                           service);
    if (r < 0) {
        /* No watcher on this session yet - or one that is wedged.  The bus,
         * the name, the vtables and the match stay up on purpose: a watcher
         * that shows up later is joined through the NameOwnerChanged signal,
         * so the icon appears whenever the panel gets there.  The CLI works
         * either way - the tray is an enhancement, never a dependency. */
        L("tray: no tray watcher answered (%s) - waiting for one to appear, "
          "CLI still works", err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return 0;
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    registered = 1;

    L("tray: registered as %s", service);
    return 0;

fail:
    L("tray: registration failed (%s) - tray disabled, CLI still works",
      strerror(-r));
    sd_bus_error_free(&err);
    sd_bus_unref(bus);
    bus = NULL;
    return -1;
}

/* Streams one "(ia{sv})" entry carrying a row's toggle-state into the
 * ItemsPropertiesUpdated signal. */
static int AppendToggle(sd_bus_message *sig, int32_t id, int on) {
    int r = sd_bus_message_open_container(sig, SD_BUS_TYPE_STRUCT, "ia{sv}");
    if (r >= 0) r = sd_bus_message_append(sig, "i", id);
    if (r >= 0) r = sd_bus_message_open_container(sig, SD_BUS_TYPE_ARRAY, "{sv}");
    if (r >= 0) r = sd_bus_message_append(sig, "{sv}", "toggle-state", "i", on);
    if (r >= 0) r = sd_bus_message_close_container(sig);
    if (r >= 0) r = sd_bus_message_close_container(sig);
    return r;
}

void tray_state_changed(void) {
    sd_bus_message *sig = NULL;
    int r;
    if (!bus) return;
    revision++;

    r = sd_bus_emit_properties_changed(bus, "/StatusNotifierItem",
                                       "org.kde.StatusNotifierItem", "Status",
                                       "ToolTip", NULL);
    if (r < 0) L("tray: Status PropertiesChanged failed (%s)", strerror(-r));

    /* ItemsPropertiesUpdated: the toggles and radios whose state can move. */
    r = sd_bus_message_new_signal(bus, &sig, "/StatusNotifierItem",
                                  "com.canonical.dbusmenu",
                                  "ItemsPropertiesUpdated");
    if (r < 0) {
        L("tray: ItemsPropertiesUpdated failed (%s)", strerror(-r));
        return;
    }
    {
        /* Every row whose state can move: the two checkmarks, plus every radio
         * row of every submenu, walked from the model so a submenu added later
         * cannot be left out of the push channel.  An index of -1 means the
         * current value matches no preset, and then that submenu contributes
         * nothing - the layout reports no checked row either.  The entries are
         * streamed into the signal, so there is no hand-counted array size to
         * keep in step with the menu. */
        static const int roots[] = { ID_STRENGTH, ID_WARMTH, ID_GRAIN,
                                     ID_FIBRE, ID_BLOTCH };
        int i, j;
        r = sd_bus_message_open_container(sig, SD_BUS_TYPE_ARRAY, "(ia{sv})");
        if (r >= 0)
            r = AppendToggle(sig, ID_SHOW, cb->query_master() ? 1 : 0);
        if (r >= 0)
            r = AppendToggle(sig, ID_AUTOSTART, cb->query_autostart() ? 1 : 0);
        for (i = 0; r >= 0 && i < (int)(sizeof roots / sizeof roots[0]); i++) {
            int m = MenuChildCount(roots[i]);
            for (j = 0; r >= 0 && j < m; j++) {
                int32_t id = MenuChild(roots[i], j);
                int row, cur, step;
                const char *name;
                void (*set)(int);
                if (!RadioRow(id, &row, &cur, &step, &name, &set) || cur < 0)
                    continue;
                r = AppendToggle(sig, id, cur == row);
            }
        }
        if (r >= 0) r = sd_bus_message_close_container(sig);
    }
    /* removedProps is a(ias) per the DBusMenu spec; this menu never removes a
     * property, so the array stays empty. */
    if (r >= 0) r = sd_bus_message_open_container(sig, SD_BUS_TYPE_ARRAY, "(ias)");
    if (r >= 0) r = sd_bus_message_close_container(sig);
    if (r >= 0) r = sd_bus_send(NULL, sig, NULL);
    sd_bus_message_unref(sig);
    if (r < 0) L("tray: ItemsPropertiesUpdated failed (%s)", strerror(-r));

    r = sd_bus_emit_signal(bus, "/StatusNotifierItem", "com.canonical.dbusmenu",
                           "LayoutUpdated", "ui", revision, 0);
    if (r < 0) L("tray: LayoutUpdated failed (%s)", strerror(-r));
}

int tray_fd(void) {
    if (!bus) return -1;
    return sd_bus_get_fd(bus);
}

short tray_events(void) {
    if (!bus) return 0;
    return (short)sd_bus_get_events(bus);
}

int tray_timeout_ms(void) {
    uint64_t t;
    uint64_t now;
    struct timespec ts;
    if (!bus) return -1;
    if (sd_bus_get_timeout(bus, &t) < 0 || t == UINT64_MAX) return -1;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now = (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000u;
    if (t <= now) return 0;
    if ((t - now) / 1000ull > 1000000ull) return -1;
    /* Round UP: sd_bus_get_timeout(3) requires it, and a truncating divide
     * turns any remaining fraction of a millisecond into 0, which makes the
     * poll loop spin poll(...,0) -> tray_process() until the deadline
     * actually passes. */
    return (int)((t - now + 999ull) / 1000ull);
}

void tray_process(void) {
    int r;
    if (!bus) return;
    for (;;) {
        r = sd_bus_process(bus, NULL);
        if (r < 0) {
            /* A dropped session bus (logout) is not fatal for the overlay. */
            if (r == -ENOTCONN || r == -ECONNRESET || r == -EPIPE) {
                L("tray: session bus gone - tray disabled, overlay keeps running");
                sd_bus_unref(bus);
                bus = NULL;
            }
            return;
        }
        if (r == 0) break;
    }
}
