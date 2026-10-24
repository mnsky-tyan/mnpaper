/* mnpaper - Linux (Wayland) port of mnPaper.
 *
 * Scope of this port (v1, Wayland only - there is no X11 fallback by design):
 *
 *   The paper texture is one layer-shell surface per output, placed on the
 *   OVERLAY layer (above panels and fullscreen windows - the closest thing
 *   Wayland has to Windows' WS_EX_TOPMOST), anchored to all four edges.  The
 *   exclusive zone and the bottom margin follow tbmode/panelgap (see
 *   ApplyExclusiveZone): by default the paper reaches every screen edge, and
 *   a floating panel that needs a clear strip opts into one with `panelgap`.  The input
 *   region is empty, so every click, hover and keystroke falls through to
 *   whatever is underneath - the Wayland equivalent of WS_EX_TRANSPARENT.
 *
 *   The texture itself is plain software rendering into wl_shm buffers.
 *   wl_shm's ARGB8888 is premultiplied BGRA in memory - byte-for-byte the
 *   format the Windows build paints into its top-down DIBs - so the
 *   generator (paper_core.h) is shared verbatim.
 *
 * Not ported yet (each is its own piece of work, listed here so the gaps
 * are explicit):
 *   - e-ink mode: needs screen capture.  KWin (this desktop) implements
 *     neither wlr-screencopy nor ext-image-copy-capture, so the capture
 *     backend would be KDE's PipeWire screencast.  Requests for e-ink are
 *     refused with a log line; mode stays paper.
 *   - settings GUI, global hotkeys (ext-global-shortcuts-v1), update feed,
 *     and capture exclusion (`share`: a layer surface is not a Plasma window,
 *     so KWin's excludeFromCapture is unreachable - the paper DOES appear in
 *     screen shares on Wayland).  Everything the GUI would control is
 *     reachable through the CLI and the tray's submenus.
 *   Autostart IS implemented (an XDG .desktop file; see ApplyAutostart),
 *   and so is the tray icon (StatusNotifierItem + DBusMenu, see tray.c).
 *
 * CLI (mirrors the Windows flags where it makes sense):
 *   mnpaper                 start the overlay daemon (foreground)
 *   mnpaper --on|--off|--toggle|--paper|--eink|--quit
 *                           command a running instance
 *   mnpaper --set k=v [--set k=v ...] [--on]
 *                           push settings to a running instance, or start
 *                           fresh with them when none is running
 *   mnpaper --status        print the running instance's state
 *   mnpaper --dump-tex out.bmp W H [bg]
 *                           headless: bake a texture to a 24bpp BMP
 *                           (composite over `bg` gray, default 235), no
 *                           display needed - the test harness uses this
 *   mnpaper --log FILE      append diagnostics to FILE as well as stderr
 *
 * Second instance: a unix socket at $XDG_RUNTIME_DIR/mnpaper.sock replaces
 * the Windows FindWindow/WM_COPYDATA dance.  Text lines in, text lines out.
 *
 * Settings: $XDG_CONFIG_HOME/mnpaper/settings.conf (default
 * ~/.config/...), same keys and bounds as the Windows registry values,
 * written atomically on every applied change.
 */

#define _GNU_SOURCE            /* pipe2 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <wayland-client.h>

#include "protocols/wlr-layer-shell-unstable-v1.h"
#include "protocols/fractional-scale-v1.h"
#include "protocols/viewporter.h"
#include "paper_core.h"
#include "tray.h"
#include "../version.h"

#define APP_VERSION MNVER_STR "-wayland"

/* The veil covers every output, so an output past this count would get no
 * texture at all - same cap and same policy as mnPaper.c's MAX_MON. */
#define MAX_MON 16

/* Which half of an output's buffer set its surface currently shows. */
#define HALF_NONE  0        /* nothing attached yet */
#define HALF_TEX   1        /* the texture half */
#define HALF_CLEAR 2        /* the transparent half */

/* One --set batch bound, shared by the fresh-start path and the client
 * request builder, so the same command line behaves the same whether or not
 * an instance is already running. */
#define MAX_SETS 64

/* ------------------------------------------------------------------ log --- */

/* The logger the core declares: stderr plus the log file.  The tray module
 * and the CLI paths both funnel through this one implementation. */
FILE *paper_log_file;
void PAPERLOG(const char *fmt, ...) {
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", buf);
    if (paper_log_file) {
        fprintf(paper_log_file, "%s\n", buf);
        fflush(paper_log_file);
    }
}
#define g_log paper_log_file
#define L PAPERLOG

static double NowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ------------------------------------------------------------- settings --- */

static SETTINGS g_s;
static int g_settings_gen = 1;   /* bumped on every applied change: results
                                  * baked from an older generation are dropped */

static const char *ConfigDir(void) {
    static char dir[512];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg)
        snprintf(dir, sizeof dir, "%s/mnpaper", xdg);
    else {
        const char *home = getenv("HOME");
        if (!home || !*home) return NULL;
        snprintf(dir, sizeof dir, "%s/.config/mnpaper", home);
    }
    return dir;
}

static const char *ConfigPath(void) {
    static char path[600];
    const char *dir = ConfigDir();
    if (!dir) return NULL;
    snprintf(path, sizeof path, "%s/settings.conf", dir);
    return path;
}

static int ConfigWrite(const SETTINGS *s) {
    const char *dir = ConfigDir(), *path = ConfigPath();
    char tmp[700];
    FILE *f;
    if (!dir || !path) return 0;
    if (mkdir(dir, 0755) && errno != EEXIST) return 0;
    snprintf(tmp, sizeof tmp, "%s/settings.conf.tmp.%d", dir, (int)getpid());
    f = fopen(tmp, "w");
    if (!f) return 0;
    if (ConfigFormat(s, f) < 0) { fclose(f); unlink(tmp); return 0; }
    if (fclose(f)) { unlink(tmp); return 0; }
    if (rename(tmp, path)) { unlink(tmp); return 0; }
    return 1;
}

static void AutostartSyncFromDisk(void);   /* defined with the autostart code */

static void LoadSettings(void) {
    FILE *f;
    char line[256];
    int loaded = 0, stamped = 0, migrated;
    const char *path;
    g_s = PaperDefaultSettings();
    path = ConfigPath();
    if (!path) return;
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        int r = ConfigParseLine(&g_s, line, &stamped);
        if (r < 0)
            L("config: ignoring unknown or malformed line: %s", line);
        else if (r > 0) loaded++;
    }
    fclose(f);
    migrated = MigrateSettings(&g_s, stamped);
    if (migrated)
        L("config: panelgap migrated 96 -> 0 (pre-v2 default; the paper now "
          "reaches the bottom edge - set panelgap again if a floating panel "
          "needs the strip)");
    ClampSettingsOf(&g_s);
    NormalizeMode(&g_s);       /* a stored mode=1 is never representable */
    /* Written back immediately, not just at exit: a migration that only lives
     * in memory would run again on every start (and re-log every time), an
     * unstamped file would keep a hand-edited 96 migratable, and a crash
     * before the clean shutdown would lose the migration.  What lands on disk
     * is already clamped and normalized. */
    if ((migrated || !stamped) && !ConfigWrite(&g_s))
        L("warning: could not persist the settings to %s", path);
    AutostartSyncFromDisk();   /* the .desktop file overrides any stored value */
    if (loaded) L("config: loaded %d keys from %s", loaded, path);
}

/* ------------------------------------------------------------ autostart --- */

/* Windows writes an entry under HKCU\...\Run and, crucially, never stores
 * `autostart` in its settings: LoadSettings derives it from the Run entry, so
 * a stored copy could only diverge from the truth.  The Linux counterpart is
 * an XDG autostart file, and it follows the same rule: the .desktop file IS
 * the truth, and the config key is a request that is applied to it.
 *
 * Written for the current user only, under $XDG_CONFIG_HOME/autostart, which
 * KDE, GNOME and every other XDG-compliant desktop reads at login. */

static const char *AutostartPath(void) {
    static char path[600];
    const char *dir = ConfigDir();          /* <config>/mnpaper */
    char base[512];
    char *slash;
    if (!dir) return NULL;
    snprintf(base, sizeof base, "%s", dir);
    slash = strrchr(base, '/');             /* strip the trailing /mnpaper */
    if (!slash) return NULL;
    *slash = 0;
    snprintf(path, sizeof path, "%s/autostart/mnpaper.desktop", base);
    return path;
}

static char *ExePath(void) {
    static char path[4096];
    static int known;
    if (!known) {
        ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
        if (n <= 0) return NULL;
        path[n] = 0;
        /* Replacing the binary under a running daemon makes Linux report
         * "<path> (deleted)" here.  Writing that literal into the autostart
         * .desktop poisons it: the entry then names a file that does not
         * exist, and the next boot starts nothing (measured live - the
         * poisoned entry also flipped `autostart` back to 0, because the
         * ownership comparison no longer matched).  Strip the suffix and
         * require the path to exist: an exe that is gone for good must yield
         * NULL, so the callers keep their "cannot determine this executable's
         * path - entry not touched" fail-soft instead of writing a broken
         * Exec line.
         *
         * The link is read once, because /proc/self/exe also follows a plain
         * rename of the running binary (no suffix), and that new name is not
         * this install's identity: the entry and every ownership check are
         * keyed on the path this process was started from. */
        if (n > 10 && !memcmp(path + n - 10, " (deleted)", 10)) path[n - 10] = 0;
        known = 1;
    }
    if (access(path, F_OK) != 0) return NULL;
    return path;
}

static void DesktopQuote(const char *s, char *out, size_t outsz);

/* Parse the on-disk entry: *ours is set when it names THIS executable (the
 * same ownership rule as RunEntryMatchesExe, so a second copy of the binary
 * at another path cannot touch the installed one's autostart) and *exec
 * when its Exec line equals the quoting this build writes.  The lines are
 * read whole: the Exec line of a long install path is longer than any
 * fixed buffer would hold. */
static void AutostartEntryState(const char *file, const char *exe,
                                int *ours, int *exec) {
    FILE *f = fopen(file, "r");
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    char quoted[8300];
    DesktopQuote(exe, quoted, sizeof quoted);
    *ours = 0;
    *exec = 0;
    if (!f) return;
    while ((n = getline(&line, &cap, f)) > 0) {
        if (line[n - 1] == '\n') line[n - 1] = 0;
        if (!strncmp(line, "X-Mnpaper-Exe=", 14) && !strcmp(line + 14, exe))
            *ours = 1;
        else if (!strncmp(line, "Exec=", 5) && !strcmp(line + 5, quoted))
            *exec = 1;
    }
    free(line);
    fclose(f);
}

/* Quote a path for the Exec value of a desktop entry.  The spec splits the
 * value on whitespace, so an unquoted path with a space would be exec'd as
 * two words; inside double quotes it reserves ", `, $ and \, and a literal
 * percent sign is written as %%. */
static void DesktopQuote(const char *s, char *out, size_t outsz) {
    size_t o = 0;
    if (outsz < 4) { if (outsz) out[0] = 0; return; }
    out[o++] = '"';
    for (; *s && o + 3 < outsz; s++) {
        if (*s == '%') { out[o++] = '%'; out[o++] = '%'; continue; }
        if (*s == '"' || *s == '`' || *s == '$' || *s == '\\') out[o++] = '\\';
        out[o++] = *s;
    }
    out[o++] = '"';
    out[o] = 0;
}

/* Apply the requested autostart state to the desktop entry.
 * Returns 1 when the on-disk truth matches the request. */
static int ApplyAutostart(void) {
    const char *path = AutostartPath();
    char *exe = ExePath();
    if (!path || !exe) {
        L("autostart: cannot determine %s - entry not touched",
          !path ? "the config directory" : "this executable's path");
        return 0;
    }
    if (g_s.autostart) {
        char dir[600], tmp[700];
        char quoted[8300];
        FILE *f;
        /* already enabled, ours and written the way this build writes it:
         * nothing to write, nothing to log.  This runs on every settings
         * change, so an unconditional rewrite spammed one "autostart:
         * enabled" line per --set. */
        {
            int ours, exec_ok;
            AutostartEntryState(path, exe, &ours, &exec_ok);
            if (ours && exec_ok) return 1;
        }
        snprintf(dir, sizeof dir, "%s", path);
        {
            char *slash = strrchr(dir, '/');
            if (slash) *slash = 0;
        }
        if (mkdir(dir, 0755) && errno != EEXIST) {
            L("autostart: cannot create %s (errno %d)", dir, errno);
            return 0;
        }
        snprintf(tmp, sizeof tmp, "%s.tmp.%d", path, (int)getpid());
        f = fopen(tmp, "w");
        if (!f) {
            L("autostart: cannot write %s (errno %d)", tmp, errno);
            return 0;
        }
        DesktopQuote(exe, quoted, sizeof quoted);
        fprintf(f,
            "[Desktop Entry]\n"
            "Type=Application\n"
            "Name=mnPaper\n"
            "Comment=A soft paper texture over your whole screen\n"
            "Exec=%s\n"
            "Terminal=false\n"
            "Hidden=false\n"
            "X-GNOME-Autostart-enabled=true\n"
            "X-KDE-autostart-enabled=true\n"
            "X-Mnpaper-Exe=%s\n",
            quoted, exe);
        if (fclose(f) || rename(tmp, path)) {
            unlink(tmp);
            L("autostart: could not install %s (errno %d)", path, errno);
            return 0;
        }
        L("autostart: enabled (%s)", path);
        return 1;
    }
    /* disabling: remove only an entry that names this executable */
    {
        int ours, exec_ok;
        AutostartEntryState(path, exe, &ours, &exec_ok);
        if (access(path, F_OK) == 0 && ours) {
            if (unlink(path)) {
                L("autostart: cannot remove %s (errno %d)", path, errno);
                return 0;
            }
            L("autostart: disabled (%s removed)", path);
        }
    }
    return 1;
}

/* The on-disk entry decides `autostart`, exactly like the Windows Run key:
 * no stored copy can diverge from the truth. */
static void AutostartSyncFromDisk(void) {
    const char *path = AutostartPath();
    char *exe = ExePath();
    int ours, exec_quoted;
    if (!path || !exe) return;
    AutostartEntryState(path, exe, &ours, &exec_quoted);
    g_s.autostart = ours;
}

/* ----------------------------------------------------------- wayland app --- */

struct OUT;

static struct wl_display *g_dpy;
static struct wl_registry *g_reg;
static struct wl_compositor *g_comp;
static int g_comp_version;
static struct wl_shm *g_shm;
static struct zwlr_layer_shell_v1 *g_shell;
static struct wp_fractional_scale_manager_v1 *g_fsm;
static struct wp_viewporter *g_vp;
static int g_sock_fd = -1;         /* command socket listen fd */
static int g_sig_fd = -1;          /* signalfd for SIGINT/SIGTERM */

static struct OUT *g_ov[MAX_MON];
static int g_nov;

static volatile sig_atomic_t g_quit;

/* ---- bake worker: one thread, one job at a time, results over a pipe ----
 * Same shape as the Windows build's PaperWorker/PostMessage(WM_APP_PAPER):
 * the UI thread never runs the noise loops, the worker never touches the
 * compositor.  A settings change does not abort an in-flight bake (the
 * Windows build cancels row-wise); the stale result is simply dropped at
 * present time by its generation number, at the cost of one wasted bake. */

struct JOB { int used; int idx, w, h, gen; float fs; SETTINGS s; };
static struct JOB g_job[MAX_MON];
static int g_baking[MAX_MON];          /* gen the worker is baking now, 0 = idle */

static pthread_mutex_t g_wlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_wake  = PTHREAD_COND_INITIALIZER;
static pthread_t       g_worker;
static int             g_worker_on;
static PAPERCANCEL     g_wcancel;        /* live stop flag the worker re-reads
                                        * every row; WorkerStop raises it */

static int g_pipe[2] = { -1, -1 };     /* worker -> event loop "done" ping */

struct RESULT {
    int idx, w, h, gen;
    double build_ms;
    unsigned char *px;                 /* worker-malloc'd; main thread copies
                                        * into the surface buffer and frees */
};
#define RESULTQ 32
static struct RESULT g_results[RESULTQ];
static int g_nresults;

static void WorkerKick(void) { pthread_cond_broadcast(&g_wake); }

/* Return an output's in-flight marker to idle - but only while it still
 * names the job that just finished: the worker may already be baking a
 * newer generation for that output, and wiping its marker would let
 * QueueBake enqueue a duplicate of the bake in flight.  Shared by every
 * path that ends a bake, so a job that delivers no result cannot strand
 * the marker either. */
static void BakeIdle(int idx, int gen) {
    pthread_mutex_lock(&g_wlock);
    if (g_baking[idx] == gen) g_baking[idx] = 0;
    pthread_mutex_unlock(&g_wlock);
}

static void *WorkerMain(void *unused) {
    (void)unused;
    pthread_mutex_lock(&g_wlock);
    for (;;) {
        struct JOB job;
        int i = -1, found = 0;
        while (!g_wcancel.stop) {
            int k;
            for (k = 0; k < MAX_MON; k++)
                if (g_job[k].used) break;
            if (k < MAX_MON) { i = k; found = 1; break; }
            pthread_cond_wait(&g_wake, &g_wlock);
        }
        if (!found) break;              /* stopping */
        job = g_job[i];
        memset(&g_job[i], 0, sizeof g_job[i]);
        g_baking[i] = job.gen;
        pthread_mutex_unlock(&g_wlock);

        {
            size_t bytes = (size_t)job.w * job.h * 4;
            unsigned char *px = malloc(bytes ? bytes : 4);
            double began = NowMs();
            int built = px && BuildPaperFull(px, job.w, job.h, job.fs, &job.s, &g_wcancel);
            if (built) {
                struct RESULT r;
                r.idx = job.idx; r.w = job.w; r.h = job.h; r.gen = job.gen;
                r.build_ms = NowMs() - began;
                r.px = px;
                pthread_mutex_lock(&g_wlock);
                if (g_nresults < RESULTQ) {
                    g_results[g_nresults++] = r;
                    r.px = NULL;
                }
                pthread_mutex_unlock(&g_wlock);
                if (r.px) {                 /* queue full: this should not
                                             * happen at RESULTQ 32 */
                    L("worker: result queue full, dropped bake for output %d", r.idx);
                    free(r.px);
                    BakeIdle(job.idx, job.gen);
                } else if (write(g_pipe[1], "r", 1) < 0 && errno != EAGAIN) {
                    /* the loop always drains; a full pipe just means the
                     * main thread is momentarily busy */
                }
            } else {
                if (!px)
                    L("worker: out of memory baking output %d (%dx%d) - bake dropped",
                      job.idx, job.w, job.h);
                free(px);
                BakeIdle(job.idx, job.gen);
            }
        }
        pthread_mutex_lock(&g_wlock);
    }
    pthread_mutex_unlock(&g_wlock);
    return NULL;
}

static void WorkerStop(void) {
    pthread_mutex_lock(&g_wlock);
    g_wcancel.stop = 1;
    pthread_cond_broadcast(&g_wake);
    pthread_mutex_unlock(&g_wlock);
    if (g_worker_on) pthread_join(g_worker, NULL);
    g_worker_on = 0;
}

/* ------------------------------- shm buffers ------------------------------ */

static int ShmFdFor(size_t bytes) {
    char name[64];
    int fd;
    snprintf(name, sizeof name, "/mnpaper-%d", (int)getpid());
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(name);      /* the fd keeps the object alive */
    if (fd >= 0 && ftruncate(fd, (off_t)bytes)) { close(fd); return -1; }
    return fd;
}

/* ---------------------------------- outputs ------------------------------- */

/* One retired buffer set (texture + transparent halves over one mapping).
 * Freed only after the compositor released BOTH halves and a frame callback
 * proved it: that is the earliest moment unmapping is safe. */
struct RETIRED {
    struct wl_buffer *buf, *hbuf;
    struct wl_shm_pool *pool;
    int fd;
    void *data;
    size_t bytes;
    int rel_buf, rel_hbuf;
};

struct OUT {
    int idx;                            /* slot in g_ov */
    uint32_t global_name;
    struct wl_output *wl;
    int wl_gone;                        /* output global removed: never reuse */
    struct wl_surface *sfc;
    struct zwlr_layer_surface_v1 *ls;
    struct wp_fractional_scale_v1 *fs;   /* fractional scale for this surface */
    struct wp_viewport *vp;               /* buffer px <-> surface-local px */
    int fs120;                            /* preferred scale in 120ths, 0 = none */
    int lw, lh;                         /* logical size from configure */
    int scale;                          /* integer scale the output reports */
    int bscale;                         /* buffer scale the set was built for */
    int w, h;                           /* buffer pixels = BufferGeometry result */
    int configured;
    /* Layer-shell mapping state.  attached = which half the surface
     * currently shows (HALF_NONE until the first attach).  The surface
     * itself is NEVER unmapped: KWin sends no further configure for an
     * unmapped layer surface, so a remap would never be licensed (this cost
     * two live sessions to find).  Hiding is a swap to the transparent half,
     * which keeps the surface mapped. */
    int attached;
    int baked_gen;                      /* generation the texture half holds, 0 = none */
    /* Buffer state: ONE shm pool holding TWO buffers over ONE mapping -
     * [ texture | transparent ].  Hiding never unmaps the surface: KWin
     * sends no new configure for an unmapped layer surface, so a remap
     * would never be licensed again (that failure mode cost two live
     * sessions).  Hide/show is therefore a pure buffer swap: the texture
     * stays in the mapping, so re-showing costs zero bakes. */
    int fd;
    void *data;                         /* mapping: texture half, then transparent half */
    size_t map_bytes;                   /* both halves together        */
    struct wl_shm_pool *pool;
    struct wl_buffer *buf;              /* texture  */
    struct wl_buffer *hbuf;             /* transparent */
    int buf_pending, hbuf_pending;      /* commits the compositor still owns:
                                         * a half may be handed out again only
                                         * when its count is back to zero */
    struct wl_callback *frame_cb;       /* the one frame callback in flight,
                                         * or NULL.  Its listener data is this
                                         * OUT, so the OUT is freed only after
                                         * it fired or was cancelled. */
    struct RETIRED retired[4];
    int nretired;
    int teardown;                       /* global removed: this OUT is off
                                         * g_ov and is freed only once the
                                         * compositor released its last
                                         * buffer set (the buffers' listener
                                         * data points here) */
    /* wl_output state: the resolution in PIXELS comes from the current
     * mode; the geometry event's physical size is in MILLIMETRES and is not
     * a resolution, so it is not stored here */
    int out_w, out_h;
    char make[64], model[64];
};

static void FlushRetired(struct OUT *o) {
    int i, k = 0;
    for (i = 0; i < o->nretired; i++) {
        struct RETIRED *r = &o->retired[i];
        if (r->rel_buf && r->rel_hbuf) {
            wl_buffer_destroy(r->buf);
            wl_buffer_destroy(r->hbuf);
            wl_shm_pool_destroy(r->pool);
            /* our own fd: libwayland closed the dup it forwarded, not this one */
            if (r->fd >= 0) close(r->fd);
            if (r->data && r->data != MAP_FAILED) munmap(r->data, r->bytes);
        } else {
            o->retired[k++] = *r;        /* still in use by the compositor */
        }
    }
    o->nretired = k;
    /* A torn-down OUT is freed only once nothing can still reach it: every
     * retired set released and no frame callback still holding its address.
     * A callback whose surface died may never fire, so that output is parked
     * for process exit - the same bargain its buffers get when the
     * compositor keeps them. */
    if (o->teardown && !o->nretired && !o->frame_cb) free(o);
}

/* Park a whole buffer set for later release.  Never freed here: the
 * compositor may still be scanning it (wl_shm contract), and unmapping a
 * page it reads is a use-after-free in the compositor, not in us.
 * Returns 0 when the set could not be tracked: its buffers keep this OUT as
 * listener data, so the caller must then keep the OUT alive. */
static int RetireSet(struct OUT *o, struct wl_buffer *buf, struct wl_buffer *hbuf,
                     struct wl_shm_pool *pool, int fd, void *data, size_t bytes,
                     int rel_buf, int rel_hbuf) {
    struct RETIRED *r;
    if (!buf) {
        if (fd >= 0) close(fd);
        return 1;
    }
    if (o->nretired >= (int)(sizeof o->retired / sizeof o->retired[0])) {
        L("output %d: retired-buffer slots exhausted - leaking one buffer set "
          "rather than unmapping memory the compositor may still read", o->idx);
        return 0;
    }
    r = &o->retired[o->nretired++];
    r->buf = buf; r->hbuf = hbuf; r->pool = pool; r->fd = fd;
    r->data = data; r->bytes = bytes;
    r->rel_buf = rel_buf; r->rel_hbuf = rel_hbuf;
    return 1;
}

static const struct wl_callback_listener CbListener;

/* Hand the current buffer set to the retired list and empty the slot: the
 * one move every set replacement (resize, surface rebuild, teardown) uses.
 * A half the compositor still owns waits for its release before the mapping
 * can be unmapped; when the set cannot be tracked (retired slots
 * exhausted), its proxies are destroyed but the mapping is never freed with
 * the compositor possibly still reading it. */
static void RetireCurrentSet(struct OUT *o) {
    FlushRetired(o);
    if (!RetireSet(o, o->buf, o->hbuf, o->pool, o->fd, o->data, o->map_bytes,
                   o->buf_pending ? 0 : 1, o->hbuf_pending ? 0 : 1)) {
        if (o->buf) wl_buffer_destroy(o->buf);
        if (o->hbuf) wl_buffer_destroy(o->hbuf);
        if (o->pool) wl_shm_pool_destroy(o->pool);
    }
    o->buf = o->hbuf = NULL;
    o->pool = NULL;
    o->data = NULL;
    o->fd = -1;
    o->buf_pending = o->hbuf_pending = 0;
}

/* A retired set is freed only once the compositor proved it no longer scans
 * it: the release events set rel_buf/rel_hbuf and FlushRetired runs on every
 * release or frame callback.  PresentBake used to be the only place that
 * requested a callback, so a resize that never presents (a scale change, a
 * tbmode margin change, any configure with no bake behind it) parked the old
 * mapping until an unrelated event called FlushRetired - on an idle desktop,
 * process exit.  Call this on every LIVE resize path (never during teardown:
 * the surface proxy may already be destroyed): it frees a set whose halves
 * came back before the retire and arms one callback for the rest. */
static void RetireSweep(struct OUT *o) {
    FlushRetired(o);
    if (o->nretired > 0 && !o->frame_cb && o->sfc) {
        o->frame_cb = wl_surface_frame(o->sfc);
        wl_callback_add_listener(o->frame_cb, &CbListener, o);
        wl_surface_commit(o->sfc);
    }
}

static void QueueBake(struct OUT *o);
static void BufferRelease(void *data, struct wl_buffer *buf);
static void CancelFrame(struct OUT *o);
static void AttachCurrentBuffer(struct OUT *o);

static const struct wl_buffer_listener BufferListener = { BufferRelease };

/* wl_shm: the compositor owns the pixels from the commit that handed a
 * buffer out until this event.  The pending counters gate the next write
 * into a half and the next attach of it, and a bake that had to wait for a
 * release is re-queued here. */
static void BufferRelease(void *data, struct wl_buffer *buf) {
    struct OUT *o = data;
    int i;
    if (buf == o->buf) {
        if (o->buf_pending > 0) o->buf_pending--;
        if (o->baked_gen != g_settings_gen) QueueBake(o);
    } else if (buf == o->hbuf) {
        if (o->hbuf_pending > 0) o->hbuf_pending--;
    } else {
        for (i = 0; i < o->nretired; i++) {
            if (o->retired[i].buf == buf) { o->retired[i].rel_buf = 1; break; }
            if (o->retired[i].hbuf == buf) { o->retired[i].rel_hbuf = 1; break; }
        }
    }
    /* the half that came back may be the one master wants right now */
    AttachCurrentBuffer(o);
    /* A torn-down output has no surface left to deliver the frame callback
     * that normally proves a frame elapsed, so its release events are the
     * only remaining signal - and the event that frees the OUT itself. */
    if (o->teardown) FlushRetired(o);
}

/* ---- buffer geometry ----------------------------------------------------
 *
 * A fractional-scaled output (KWin offers 160% here) is the interesting
 * case.  The compositor's surface-local size stays logical (1800x1029) but
 * the screen area it covers is device pixels (2880x1646.4).  With only the
 * integer wl_output.scale (2) the buffer would hold 3600x2058 px - 1.25x
 * more than the panel has - and the compositor would have to resample that
 * oversized buffer down onto the surface: softer paper and 56% more pixels
 * to bake.
 *
 * The fractional-scale protocol fixes that: the compositor states the scale
 * it wants (in 120ths), the buffer then holds one pixel per screen pixel,
 * and a viewport maps that buffer onto the surface-local size.  The paper is
 * rendered 1:1.
 *
 * This is a sharpness and cost fix, NOT a fix for the black frame: the frame
 * was the ShapeCorners effect stroking our surface as if it were a normal
 * window, and it was cured by the layer-surface scope (see
 * LayerSurfaceInit).  A 1648 px buffer framed and a 1646 px one did not, but
 * that was coincidence - the frame tracked a buffer swap, not the height. */
static int UseFractionalScale(struct OUT *o) {
    /* The viewport that maps the buffer onto the surface-local size is the
     * viewporter extension; without it the fractional path cannot be
     * expressed, so the integer path stays. */
    return o->fs120 > 0 && g_vp != NULL;
}

/* Buffer pixels and buffer scale for a surface of lw x lh surface-local px. */
static void BufferGeometry(struct OUT *o, int lw, int lh, int scale,
                            int *bscale, int *bw, int *bh) {
    if (UseFractionalScale(o)) {
        *bscale = 1;                      /* the viewport carries the scale */
        /* the protocol asks for "rounded halfway away from zero" */
        *bw = (int)(((long long)lw * o->fs120 + 60) / 120);
        *bh = (int)(((long long)lh * o->fs120 + 60) / 120);
    } else {
        *bscale = scale;
        *bw = lw * scale;
        *bh = lh * scale;
    }
}

/* The viewport and the buffer scale are double-buffered surface state: the
 * compositor applies them with the next commit.  They are re-sent with every
 * attach, not only when the buffer is rebuilt, so the mapping a buffer is
 * presented with is always stated by the same commit that presents it -
 * nothing about the paper's geometry then depends on state left over from an
 * earlier commit. */
static void ApplySurfaceScale(struct OUT *o) {
    if (!o->buf) return;
    if (UseFractionalScale(o)) {
        if (!o->vp) {              /* surviving integer set: state its scale */
            if (g_comp_version >= 3)
                wl_surface_set_buffer_scale(o->sfc, o->bscale);
            return;
        }
        wp_viewport_set_source(o->vp, 0, 0,
                               wl_fixed_from_int(o->w), wl_fixed_from_int(o->h));
        wp_viewport_set_destination(o->vp, o->lw, o->lh);
        wl_surface_set_buffer_scale(o->sfc, 1);
    } else if (g_comp_version >= 3) {
        wl_surface_set_buffer_scale(o->sfc, o->bscale);
    }
}

/* (Re)create this output's buffer set for logical size lw x lh at scale.
 * No-op when nothing changed.  The fd stays open: libwayland forwards it to
 * the compositor on the next flush, so it is only closed when the pool dies
 * (see RetireSet/OutTeardown).
 *
 * The fd stays ours: wl_shm_create_pool does not adopt it.  libwayland dups
 * the descriptor it is handed (wl_closure_marshal) and closes that copy once
 * the request has been sent, so the number recorded here is still the one
 * this process opened - and still the one to close when the set is retired. */
static void BufferResize(struct OUT *o, int lw, int lh, int scale) {
    size_t bytes;
    int fd = -1;
    int bscale, bw, bh;
    void *data;
    struct wl_shm_pool *pool;
    struct wl_buffer *buf, *hbuf;

    BufferGeometry(o, lw, lh, scale, &bscale, &bw, &bh);

    if (o->buf && o->hbuf && o->lw == lw && o->lh == lh &&
        o->bscale == bscale && o->w == bw && o->h == bh) return;

    bytes = (size_t)bw * bh * 4;
    fd = ShmFdFor(bytes * 2);
    if (fd < 0) {
        L("output %d: shm allocation for %dx%d failed - output left untextured",
          o->idx, bw, bh);
        return;
    }
    data = mmap(NULL, bytes * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        L("output %d: mmap failed - output left untextured", o->idx);
        return;
    }
    memset((char *)data + bytes, 0, bytes);   /* the transparent half */
    pool = wl_shm_create_pool(g_shm, fd, (int32_t)(bytes * 2));
    buf  = wl_shm_pool_create_buffer(pool, 0, bw, bh,
                                     bw * 4, WL_SHM_FORMAT_ARGB8888);
    hbuf = wl_shm_pool_create_buffer(pool, (int32_t)bytes, bw, bh,
                                     bw * 4, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(buf, &BufferListener, o);
    wl_buffer_add_listener(hbuf, &BufferListener, o);
    /* the whole old set (both halves share one mapping) is retired together */
    RetireCurrentSet(o);
    RetireSweep(o);             /* live surface: free it, or arm a callback */
    o->fd = fd;
    o->data = data;
    o->map_bytes = bytes * 2;
    o->pool = pool; o->buf = buf; o->hbuf = hbuf;
    o->buf_pending = o->hbuf_pending = 0;   /* new buffers: never busy */
    o->attached = HALF_NONE;                /* the old half is retired */
    o->lw = lw; o->lh = lh;
    o->w = bw; o->h = bh; o->bscale = bscale;
    o->baked_gen = 0;                   /* a fresh texture half holds nothing */

    if (UseFractionalScale(o) && !o->vp)
        o->vp = wp_viewporter_get_viewport(g_vp, o->sfc);
    ApplySurfaceScale(o);
}

/* Make the surface show the half master asks for, right now.  The surface
 * stays MAPPED in both cases (see the buffer-state comment).
 *
 * A half the compositor still owns may not be handed out again: the
 * protocol allows reuse only after wl_buffer.release.  The swap therefore
 * waits for that event, which retries it.  A half that is already attached
 * is left alone: attaching it again would be a second commit of a buffer
 * the compositor may still be reading. */
static void AttachCurrentBuffer(struct OUT *o) {
    int want = g_s.master ? HALF_TEX : HALF_CLEAR;
    struct wl_buffer *buf;
    int *pending;
    if (!o->buf || !o->configured) return;
    if (o->attached == want) return;          /* already showing it */
    buf = want == HALF_TEX ? o->buf : o->hbuf;
    pending = want == HALF_TEX ? &o->buf_pending : &o->hbuf_pending;
    if (*pending > 0) return;                 /* still owned: retry on release */
    o->attached = want;
    (*pending)++;
    ApplySurfaceScale(o);          /* the mapping travels with the buffer */
    wl_surface_attach(o->sfc, buf, 0, 0);
    if (g_comp_version >= 4)
        wl_surface_damage_buffer(o->sfc, 0, 0, o->w, o->h);
    else
        wl_surface_damage(o->sfc, 0, 0, o->lw, o->lh);
    wl_surface_commit(o->sfc);
}

/* Queue one bake for this output's current buffer geometry (deduped). */
static void QueueBake(struct OUT *o) {
    /* Neither visible nor master is required: a bake landing while hidden
     * fills the texture half without attaching it, so the next show is
     * instant.  Requiring master here made the "armed while hidden" promise
     * false - hidden outputs simply never baked. */
    if (!o->buf || !o->configured) return;
    if (o->baked_gen == g_settings_gen) return;   /* this exact paper is ready */
    pthread_mutex_lock(&g_wlock);
    if (g_job[o->idx].used || g_baking[o->idx] == g_settings_gen) {
        pthread_mutex_unlock(&g_wlock);   /* already queued, or in flight */
        return;
    }
    g_job[o->idx].used = 1;
    g_job[o->idx].idx = o->idx;
    g_job[o->idx].w = o->w;
    g_job[o->idx].h = o->h;
    g_job[o->idx].gen = g_settings_gen;
    /* device pixels per surface-local pixel: the noise is sampled in logical
     * units so the texture is identical at every buffer scale (see
     * PaperParams in paper_core.h). */
    g_job[o->idx].fs = o->lw > 0 ? (float)o->w / (float)o->lw : 1.f;
    g_job[o->idx].s = g_s;
    pthread_mutex_unlock(&g_wlock);
    WorkerKick();
}

/* Resize this output's set for the given surface-local size, then present
 * the wanted half and queue a bake.  Every listener that rebuilds a set
 * shares this tail: a rebuilt set must never be left unattached, because an
 * unmapped layer surface gets no further configure.  When the allocation
 * fails nothing changes - the surviving set (or none) stays as it was. */
static void RescaleAndPresent(struct OUT *o, int lw, int lh) {
    BufferResize(o, lw, lh, o->scale);
    if (!o->buf) return;
    AttachCurrentBuffer(o);
    QueueBake(o);
}

/* Frame callback: the frame that used the previous buffer set is on screen,
 * which (with both release events) is the earliest safe moment to free. */
static void FrameDone(void *data, struct wl_callback *cb, uint32_t t) {
    struct OUT *o = data;
    (void)t;
    wl_callback_destroy(cb);
    if (o->frame_cb == cb) o->frame_cb = NULL;
    FlushRetired(o);
}

static const struct wl_callback_listener CbListener = { FrameDone };

/* Copy a finished bake into the texture half and, when that half is the
 * visible one, hand it to the compositor.  A hidden output gets its texture
 * armed so the next show costs nothing. */
static void PresentBake(struct OUT *o, const unsigned char *px) {
    /* The copy is safe whenever the previous commit that handed this buffer
     * out has come back released; if it has not, the compositor may still be
     * scanning it, so the bake waits for the release event.  One frame of
     * latency, never a torn or stolen buffer. */
    if (o->buf_pending > 0) {
        L("output %d: texture half still in use by the compositor - "
          "bake parked until its release arrives", o->idx);
        return;                          /* caller re-queues on release */
    }
    memcpy(o->data, px, (size_t)o->w * o->h * 4);
    o->baked_gen = g_settings_gen;
    /* Only a commit hands the buffer to the compositor, so only a commit can
     * owe a release.  Clearing the token while hidden would wait forever for
     * an event that never comes and wedge every later bake. */
    if (o->attached == HALF_TEX) {
        o->buf_pending++;                /* until the compositor returns it */
        ApplySurfaceScale(o);          /* the mapping travels with the buffer */
        wl_surface_attach(o->sfc, o->buf, 0, 0);
        if (g_comp_version >= 4)
            wl_surface_damage_buffer(o->sfc, 0, 0, o->w, o->h);
        else
            wl_surface_damage(o->sfc, 0, 0, o->lw, o->lh);
        /* one tracked callback at a time: it only proves that a frame
         * elapsed before retired memory is unmapped, so a present that
         * arrives while one is pending reuses it */
        if (!o->frame_cb) {
            o->frame_cb = wl_surface_frame(o->sfc);
            wl_callback_add_listener(o->frame_cb, &CbListener, o);
        }
        wl_surface_commit(o->sfc);
    }
}

/* Empty input region: the compositor never routes pointer or keyboard
 * events to us, everything falls through to the windows underneath. */
static void ApplyClickThrough(struct OUT *o) {
    struct wl_region *r = wl_compositor_create_region(g_comp);
    wl_region_add(r, 0, 0, 0, 0);       /* zero-area: an empty region */
    wl_surface_set_input_region(o->sfc, r);
    wl_region_destroy(r);
}

/* tbmode picks the exclusive zone; the panelgap margin below picks the strip
 * the veil leaves unpainted (none by default).  tbmode=TB_NOT_COVER (the
 * default) sets zone 0: the surface claims no exclusive space of its own and
 * asks the compositor to move it away from surfaces that do reserve space (a
 * docked panel), so the paper still reaches the edges under a docked panel's
 * reserved area but never covers it.  tbmode=TB_COVER sets -1: the veil
 * ignores every other surface's exclusive zone and reaches the screen edges
 * unconditionally - the paper covers the taskbar, exactly like the Windows
 * mode of the same name.  The size of a floating panel, the one case the
 * compositor cannot know, is the user's `panelgap`. */
static void ApplyExclusiveZone(struct OUT *o) {
    if (!o->ls) return;
    zwlr_layer_surface_v1_set_exclusive_zone(o->ls,
        g_s.tbmode == TB_COVER ? -1 : 0);
    /* The paper is an overlay layer surface, and KWin stacks every overlay
     * above the panel (DockLayer) - no client can be placed between them.
     * "Do not cover the taskbar" therefore has to mean "do not paint that
     * strip": a bottom margin shrinks the surface to end above the panel so
     * the floating panel and its tray stay visible and clickable. */
    if (g_s.tbmode == TB_NOT_COVER && g_s.panelgap > 0)
        zwlr_layer_surface_v1_set_margin(o->ls, 0, 0, (int32_t)g_s.panelgap, 0);
    else
        zwlr_layer_surface_v1_set_margin(o->ls, 0, 0, 0, 0);
}

static void LayerSurfaceInit(struct OUT *o);

/* The compositor states the scale it wants for this surface, in 120ths
 * (160% = 192).  A change re-shapes the buffer and re-bakes the paper at
 * the new size. */
static void FracPreferred(void *data, struct wp_fractional_scale_v1 *fs,
                          uint32_t scale) {
    struct OUT *o = data;
    (void)fs;
    if ((int)scale == o->fs120) return;
    o->fs120 = (int)scale;
    L("output %d: fractional scale %u/120 (%u%%) - paper at device pixels",
      o->idx, scale, scale * 100 / 120);
    if (!o->configured || !o->buf) return;
    RescaleAndPresent(o, o->lw, o->lh);
}

static const struct wp_fractional_scale_v1_listener FracListener = {
    FracPreferred
};

/* Ask the compositor for this surface's preferred scale.  One object per
 * surface, so it is only created once - but the manager global may arrive
 * after the surface does, so this is called from both places. */
static void OutAttachFractionalScale(struct OUT *o) {
    if (!g_fsm || !o->sfc || o->fs) return;
    o->fs = wp_fractional_scale_manager_v1_get_fractional_scale(g_fsm, o->sfc);
    if (!o->fs) return;
    wp_fractional_scale_v1_add_listener(o->fs, &FracListener, o);
}

/* Drop the per-surface scale object and the viewport with the surface they
 * belong to. */
static void OutDropSurfaceExtras(struct OUT *o) {
    if (o->fs) { wp_fractional_scale_v1_destroy(o->fs); o->fs = NULL; }
    if (o->vp) { wp_viewport_destroy(o->vp); o->vp = NULL; }
    o->fs120 = 0;
}

static void LayerConfigure(void *data, struct zwlr_layer_surface_v1 *ls,
                           uint32_t serial, uint32_t w, uint32_t h) {
    struct OUT *o = data;
    (void)ls;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    o->configured = 1;
    RescaleAndPresent(o, (int)w, (int)h);
}

static void LayerClosed(void *data, struct zwlr_layer_surface_v1 *ls) {
    struct OUT *o = data;
    (void)ls;
    /* the layer surface object is dead per protocol: rebuild the whole
     * surface stack on the same wl_output (unless the output itself left) */
    L("output %d: layer surface closed by compositor, rebuilding", o->idx);
    o->configured = 0;
    o->attached = HALF_NONE;
    o->baked_gen = 0;
    CancelFrame(o);              /* the destroyed surface never delivers done */
    RetireCurrentSet(o);         /* commits on the dead surface are retired
                                  * like any other set replacement */
    if (o->wl_gone) return;
    if (o->ls) { zwlr_layer_surface_v1_destroy(o->ls); o->ls = NULL; }
    OutDropSurfaceExtras(o);
    if (o->sfc) { wl_surface_destroy(o->sfc); o->sfc = NULL; }
    LayerSurfaceInit(o);
}

static const struct zwlr_layer_surface_v1_listener LayerListener = {
    LayerConfigure, LayerClosed,
};

static void OutputGeometry(void *data, struct wl_output *wl, int32_t x, int32_t y,
                           int32_t pw, int32_t ph, int32_t subpixel,
                           const char *make, const char *model,
                           int32_t transform) {
    struct OUT *o = data;
    (void)wl; (void)x; (void)y; (void)pw; (void)ph; (void)subpixel; (void)transform;
    snprintf(o->make, sizeof o->make, "%s", make ? make : "?");
    snprintf(o->model, sizeof o->model, "%s", model ? model : "?");
}

static void OutputMode(void *data, struct wl_output *wl, uint32_t flags,
                       int32_t w, int32_t h, int32_t refresh) {
    struct OUT *o = data;
    (void)wl; (void)refresh;
    if (flags & WL_OUTPUT_MODE_CURRENT) { o->out_w = w; o->out_h = h; }
}

static void OutputDone(void *data, struct wl_output *wl) {
    struct OUT *o = data;
    (void)wl;
    /* scale can change under us (display settings); keep the buffer in step.
     * The swap leaves the surface with nothing attached, so re-attach the
     * wanted half right away - a release of the retired old buffer is not
     * guaranteed to arrive (the surface stops being presented, not replaced). */
    if (o->configured && o->buf)
        RescaleAndPresent(o, o->lw, o->lh);
}

static void OutputScale(void *data, struct wl_output *wl, int32_t scale) {
    struct OUT *o = data;
    (void)wl;
    if (scale < 1) scale = 1;
    o->scale = (int)scale;
}

static void OutputName(void *data, struct wl_output *wl, const char *name) {
    (void)data; (void)wl; (void)name;
}

static void OutputDescription(void *data, struct wl_output *wl,
                              const char *description) {
    (void)data; (void)wl; (void)description;
}

static const struct wl_output_listener OutputListener = {
    OutputGeometry, OutputMode, OutputDone, OutputScale, OutputName,
    OutputDescription,
};

/* Drop the pending frame callback, if any.  A destroyed surface never
 * delivers done, so waiting for it would stall the retirement flush. */
static void CancelFrame(struct OUT *o) {
    if (!o->frame_cb) return;
    wl_callback_destroy(o->frame_cb);
    o->frame_cb = NULL;
}

static void OutTeardown(struct OUT *o) {
    if (!o) return;
    if (o->ls) zwlr_layer_surface_v1_destroy(o->ls);
    OutDropSurfaceExtras(o);
    if (o->sfc) wl_surface_destroy(o->sfc);
    CancelFrame(o);
    if (o->wl) wl_output_destroy(o->wl);
    /* the current set joins the retired list and is freed only once the
     * compositor released both halves (never unmap memory a live compositor
     * reads).  The OUT itself must outlive those releases: the buffers'
     * listener data points here. */
    RetireCurrentSet(o);
    o->configured = 0;
    o->attached = HALF_NONE;
    o->baked_gen = 0;
    pthread_mutex_lock(&g_wlock);
    memset(&g_job[o->idx], 0, sizeof g_job[o->idx]);
    g_baking[o->idx] = 0;
    pthread_mutex_unlock(&g_wlock);
    o->teardown = 1;
    FlushRetired(o);              /* frees o itself once nothing is outstanding */
}

/* build the wl_surface + layer surface for an existing OUT (used both by
 * OutCreate and by the closed/rebuild path).  Refuses to build anything
 * before the globals it needs are bound: wl_output is advertised before
 * wl_compositor, wl_shm and zwlr_layer_shell_v1 in some registry orders, and
 * a missing layer-shell is a hard error, not a NULL dereference. */
static void LayerSurfaceInit(struct OUT *o) {
    if (!g_comp || !g_shm || !g_shell || o->ls || o->wl_gone) return;
    o->sfc = wl_compositor_create_surface(g_comp);
    OutAttachFractionalScale(o);
    ApplyClickThrough(o);
    /* The scope is not cosmetic: KWin maps it to a WindowType and defaults
     * an unknown scope to WindowType::Normal (see scopeToType in KWin's
     * layershellv1window.cpp).  A Normal window is what desktop effects
     * manage, and a popular corner-rounding effect
     * (kwin4_effect_shapecorners, InactiveOutlineThickness=1,
     * InactiveOutlineColor=#000000) then strokes its one pixel black
     * inactive outline around this surface: the paper came back framed by a
     * black rounded-rect border after every hide/show buffer swap.  The
     * paper is not a user window - it is a persistent screen overlay - so it
     * must not present itself as Normal.  Measured on KWin 6.6.6: with the
     * Normal default the frame appeared after each toggle and survived a
     * re-bake; with this scope four consecutive toggles stayed clean. */
    o->ls = zwlr_layer_shell_v1_get_layer_surface(g_shell, o->sfc, o->wl,
                                                  ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
                                                  "on-screen-display");
    zwlr_layer_surface_v1_set_anchor(o->ls, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    ApplyExclusiveZone(o);              /* tbmode decides panel coverage */
    zwlr_layer_surface_v1_set_keyboard_interactivity(o->ls,
                                     ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(o->ls, &LayerListener, o);
    wl_surface_commit(o->sfc);          /* triggers the configure */
}

/* Create every output that is still waiting for its globals.  Called once
 * after the registry enumeration has bound everything: an output announced
 * before wl_compositor or layer-shell simply waits here instead of
 * dereferencing a NULL proxy. */
static void OutSurfaceInitAll(void) {
    int i;
    for (i = 0; i < MAX_MON; i++)
        if (g_ov[i]) LayerSurfaceInit(g_ov[i]);
}

static void OutCreate(struct wl_output *wl, uint32_t name) {
    struct OUT *o;
    int slot = -1, i;

    for (i = 0; i < MAX_MON; i++)
        if (!g_ov[i]) { slot = i; break; }
    if (slot < 0) {
        L("output global %u dropped: %d outputs already covered (MAX_MON)", name, MAX_MON);
        return;
    }
    o = calloc(1, sizeof *o);
    if (!o) return;
    o->idx = slot;
    o->wl = wl;
    o->global_name = name;
    o->fd = -1;
    o->scale = o->bscale = 1;
    g_ov[slot] = o;
    if (slot >= g_nov) g_nov = slot + 1;

    wl_output_add_listener(wl, &OutputListener, o);

    LayerSurfaceInit(o);                /* no-op until the globals are bound */
    L("output %d: global %u registered", slot, name);
}

static void RegistryGlobal(void *data, struct wl_registry *reg, uint32_t name,
                           const char *iface, uint32_t version) {
    (void)data;
    if (strcmp(iface, wl_compositor_interface.name) == 0) {
        g_comp_version = (int)version;
        g_comp = wl_registry_bind(reg, name, &wl_compositor_interface,
                                  version < 6 ? version : 6);
    } else if (strcmp(iface, wl_shm_interface.name) == 0) {
        g_shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        g_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface,
                                   version < 5 ? version : 5);
    } else if (strcmp(iface, wp_fractional_scale_manager_v1_interface.name) == 0) {
        g_fsm = wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1);
        /* the manager may arrive after the outputs did: give the surfaces
         * that already exist their scale object now */
        for (int i = 0; i < MAX_MON; i++)
            if (g_ov[i]) OutAttachFractionalScale(g_ov[i]);
    } else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
        g_vp = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        struct wl_output *wl = wl_registry_bind(reg, name, &wl_output_interface,
                                                version < 4 ? version : 4);
        if (wl) OutCreate(wl, name);
    }
}

static void RegistryRemove(void *data, struct wl_registry *reg, uint32_t name) {
    int i;
    (void)data; (void)reg;
    for (i = 0; i < MAX_MON; i++) {
        struct OUT *o = g_ov[i];
        if (o && o->global_name == name) {
            L("output %d: gone (global %u)", i, name);
            o->wl_gone = 1;
            g_ov[i] = NULL;
            OutTeardown(o);
        }
    }
}

static const struct wl_registry_listener RegistryListener = {
    RegistryGlobal, RegistryRemove,
};

/* -------------------------------------------------- apply / state change --- */

static void RepaintAll(void) {
    int i;
    for (i = 0; i < MAX_MON; i++) {
        struct OUT *o = g_ov[i];
        if (!o || !o->configured || !o->buf) continue;
        /* tbmode may have moved the surface off (or onto) the panel; the
         * zone only takes effect in a commit, and AttachCurrentBuffer alone
         * would return early when the wanted half is already showing. */
        if (o->ls) {
            ApplyExclusiveZone(o);
            wl_surface_commit(o->sfc);
        }
        AttachCurrentBuffer(o);             /* show or hide: the wanted half */
        /* bake regardless of visibility: a hidden output keeps its texture
         * armed so showing it later costs no bake */
        if (o->baked_gen != g_settings_gen) QueueBake(o);
    }
}

/* e-ink is not implemented on Wayland (it needs screen capture this
 * compositor path does not provide).  The mode is therefore NOT a state we
 * can hold: reporting `mode eink` while painting paper made --status, the
 * config file and the screen disagree permanently, because the refused value
 * was still persisted.  Normalization lives in paper_core.h (NormalizeMode)
 * so the headless suite pins the same contract this daemon relies on. */
static int RequestMode(int mode) {
    g_s.mode = mode;
    if (NormalizeMode(&g_s)) return 1;
    L("e-ink mode is not implemented on Wayland yet: it needs screen "
      "capture and this compositor path is not built (staying in paper mode)");
    return 0;
}

static void ApplySettings(int save) {
    static int last_tex[5];              /* texture fields at the last gen bump */
    int i, dirty;
    int tex[5];

    ClampSettingsOf(&g_s);
    /* refused modes are normalized to paper BEFORE the rest of the function
     * (and before the config write) can observe them */
    RequestMode(g_s.mode);

    /* The comparison is between EFFECTIVE values: a clamped request (say
     * warmth=150) must not look like a change, or it would buy a re-bake of
     * a byte-identical texture. */
    tex[0] = g_s.intensity; tex[1] = g_s.warmth; tex[2] = g_s.grain;
    tex[3] = g_s.fibre; tex[4] = g_s.blotch;

    /* Only texture content invalidates the generation.  A toggle, share or
     * tray change must NOT cost a re-bake (Windows behaves the same way:
     * toggling only shows/hides the strips over the same DIB). */
    dirty = memcmp(tex, last_tex, sizeof tex) != 0;
    if (dirty) {
        memcpy(last_tex, tex, sizeof tex);
        g_settings_gen++;
        pthread_mutex_lock(&g_wlock);
        /* re-stamp queued jobs with the new generation and settings: one
         * bake per change, never a stale one */
        for (i = 0; i < MAX_MON; i++)
            if (g_job[i].used) {
                g_job[i].gen = g_settings_gen;
                g_job[i].s = g_s;
            }
        pthread_mutex_unlock(&g_wlock);
    }

    ApplyAutostart();       /* idempotent; the .desktop file is the truth */
    RepaintAll();
    if (save && !ConfigWrite(&g_s))
        L("warning: could not write settings to %s", ConfigPath() ? ConfigPath() : "(unset)");
    tray_state_changed();   /* keep the menu checkmarks in step (no-op if off) */
}

/* --------------------------------------------------------------- commands -- */

static void SetMaster(int on) {
    if (g_s.master == on) return;
    g_s.master = on;
    ApplySettings(1);
}

static void SetMode(int mode) {
    if (mode == MODE_EINK) {
        RequestMode(MODE_EINK);
        return;
    }
    if (g_s.mode == MODE_PAPER && g_s.master) return;
    g_s.mode = MODE_PAPER;
    g_s.master = 1;
    ApplySettings(1);
}

/* --------------------------------------------------------------- tray ----- */

/* The tray mirrors the same state the CLI drives; every menu action lands in
 * the SetMaster/ApplySettings paths, so one choke point keeps the screen,
 * the config and the menu checkmarks in agreement. */
static void MenuToggleShow(void)      { SetMaster(!g_s.master); }
static void MenuSetIntensity(int v)   { g_s.intensity = v; ApplySettings(1); }
static void MenuSetWarmth(int v)      { g_s.warmth = v; ApplySettings(1); }
static void MenuSetGrain(int v)       { g_s.grain = v; ApplySettings(1); }
static void MenuSetFibre(int v)       { g_s.fibre = v; ApplySettings(1); }
static void MenuSetBlotch(int v)      { g_s.blotch = v; ApplySettings(1); }
static void MenuToggleAutostart(void) { g_s.autostart = !g_s.autostart; ApplySettings(1); }
static void MenuQuit(void)            { g_quit = 1; }
static void MenuEditConfig(void) {
    const char *path = ConfigPath();
    pid_t pid = fork();
    if (pid == 0) {
        sigset_t none;
        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        setsid();
        execlp("xdg-open", "xdg-open", path ? path : "", (char *)NULL);
        _exit(127);
    }
    /* fire and forget: SIGCHLD is ignored while the tray runs, no zombie */
}
static int MenuQueryMaster(void)      { return g_s.master; }
static int MenuQueryIntensity(void)   { return g_s.intensity; }
static int MenuQueryWarmth(void)      { return g_s.warmth; }
static int MenuQueryGrain(void)       { return g_s.grain; }
static int MenuQueryFibre(void)       { return g_s.fibre; }
static int MenuQueryBlotch(void)      { return g_s.blotch; }
static int MenuQueryAutostart(void)   { return g_s.autostart; }

static const struct TRAYCB g_traycb = {
    MenuToggleShow, MenuSetIntensity, MenuSetWarmth,
    MenuSetGrain, MenuSetFibre, MenuSetBlotch, MenuToggleAutostart,
    MenuEditConfig, MenuQuit,
    MenuQueryMaster, MenuQueryIntensity, MenuQueryWarmth,
    MenuQueryGrain, MenuQueryFibre, MenuQueryBlotch, MenuQueryAutostart,
};

/* --------------------------------------------------------------- ipc ------- */

#define CMD_TOGGLE 1
#define CMD_ON     2
#define CMD_OFF    3
#define CMD_PAPER  4
#define CMD_EINK   5
#define CMD_QUIT   6

static const char *SockPath(void) {
    static char path[200];
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) return NULL;
    snprintf(path, sizeof path, "%s/mnpaper.sock", rt);
    return path;
}

static int SockStart(void) {
    struct sockaddr_un addr;
    const char *path = SockPath();
    int fd;
    if (!path) { L("XDG_RUNTIME_DIR is not set; cannot create the command socket"); return -1; }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        L("command socket: socket() failed (errno %d)", errno);
        return -1;
    }
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr)) {
        if (errno != EADDRINUSE) {
            L("command socket: cannot bind %s (errno %d)", path, errno);
            close(fd);
            return -1;
        }
        /* in use: is a live instance serving on it, or is it stale? */
        {
            int c = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            int alive = 0;
            if (c >= 0) {
                alive = (connect(c, (struct sockaddr *)&addr, sizeof addr) == 0);
                close(c);
            }
            if (alive) { close(fd); return -2; }
            /* stale: nobody accepts on it - steal the path.  A second live
             * instance racing us here is impossible in practice: it would
             * have answered the probe above. */
            unlink(path);
            if (bind(fd, (struct sockaddr *)&addr, sizeof addr)) {
                close(fd);
                return -2;
            }
        }
    }
    if (listen(fd, 4)) {
        L("command socket: listen failed (errno %d)", errno);
        close(fd);
        return -1;
    }
    return fd;
}

/* The one place the command flags live.  The CLI parser, the client-side
 * request builder and the IPC command names all read this table, so a new
 * command cannot be added to one and forgotten in the others. */
#define CMD_STATUS   98     /* not an IPC command: answered by StatusLine */

static const struct { const char *flag, *proto; int cmd; } CMD_TABLE[] = {
    { "--toggle",   "toggle",   CMD_TOGGLE   },
    { "--on",       "on",       CMD_ON       },
    { "--off",      "off",      CMD_OFF      },
    { "--paper",    "paper",    CMD_PAPER    },
    { "--eink",     "eink",     CMD_EINK     },
    { "--quit",     "quit",     CMD_QUIT     },
    { "--status",   "status",   CMD_STATUS   },
};
#define CMD_TABLE_N ((int)(sizeof CMD_TABLE / sizeof CMD_TABLE[0]))

/* One staged key/value pair of an IPC batch.  A key is at most 63 chars and
 * a value has to fit an int, so a pair never comes near this bound. */
#define STAGE_MAX 96

/* one connected client: parse lines, mutate state, answer text.  The fd is
 * nonblocking; the client drops the connection after its request and we
 * close ours when the reply is written or the peer goes away.
 *
 * A batch stages the pairs it was given; 'apply' writes exactly those keys
 * onto the settings as they are at that moment, so a change another client
 * made in between is not reverted. */
struct IPC {
    int fd;
    int pidx;                   /* this client's slot in the poll array, -1
                                 * when the current poll does not cover it */
    char buf[4096];
    size_t len;
    int sets;
    char staged[MAX_SETS][STAGE_MAX];
};

static void IpcSend(struct IPC *c, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) {
        if ((size_t)n >= sizeof buf) {
            n = (int)sizeof buf - 1;
            buf[n - 1] = '\n';       /* a truncated reply must stay one line */
        }
        {
            ssize_t ignored = write(c->fd, buf, (size_t)n);
            (void)ignored;
        }
    }
}

static void StatusLine(struct IPC *c) {
    int i, live = 0;
    IpcSend(c, "version %s\n", APP_VERSION);
    IpcSend(c, "master %d\n", g_s.master);
    IpcSend(c, "mode %s\n", g_s.mode == MODE_PAPER ? "paper" : "eink");
    IpcSend(c, "intensity %d\nwarmth %d\ngrain %d\nfibre %d\nblotch %d\n",
            g_s.intensity, g_s.warmth, g_s.grain, g_s.fibre, g_s.blotch);
    IpcSend(c, "share %d\ntbmode %d\nautostart %d\nautoupd %d\n",
            g_s.share, g_s.tbmode, g_s.autostart, g_s.autoupd);
    IpcSend(c, "panelgap %d\n", g_s.panelgap);
    for (i = 0; i < MAX_MON; i++) {
        struct OUT *o = g_ov[i];
        if (!o) continue;
        live++;
        IpcSend(c, "output %s %s %dx%d scale %d configured %d\n",
                o->make, o->model, o->out_w, o->out_h, o->scale, o->configured);
    }
    IpcSend(c, "outputs %d\n", live);
    IpcSend(c, ".\n");
}

static void IpcCommand(struct IPC *c, char *line) {
    while (*line == ' ' || *line == '\t') line++;
    if (!strcmp(line, "status")) { StatusLine(c); return; }
    if (!strcmp(line, "quit")) {
        IpcSend(c, "ok quit\n");
        g_quit = 1;
        return;
    }
    if (!strcmp(line, "toggle")) { SetMaster(!g_s.master); IpcSend(c, "ok master %d\n", g_s.master); return; }
    if (!strcmp(line, "on"))     { SetMaster(1); IpcSend(c, "ok master %d\n", g_s.master); return; }
    if (!strcmp(line, "off"))    { SetMaster(0); IpcSend(c, "ok master %d\n", g_s.master); return; }
    if (!strcmp(line, "paper"))  { SetMode(MODE_PAPER); IpcSend(c, "ok mode paper\n"); return; }
    if (!strcmp(line, "eink"))   {
        if (!RequestMode(MODE_EINK)) { IpcSend(c, "err eink-not-implemented-on-wayland\n"); return; }
        IpcSend(c, "ok mode eink\n");
        return;
    }
    if (!strncmp(line, "set ", 4)) {
        const char *kv = line + 4;
        SETTINGS probe = g_s;
        if (c->sets >= MAX_SETS) {
            IpcSend(c, "err too-many-sets\n");
            return;
        }
        if (strlen(kv) >= STAGE_MAX) {
            IpcSend(c, "err bad-set %s\n", kv);
            return;
        }
        /* the grammar is the settings core's: an unknown key or a malformed
         * value is refused here, not silently applied */
        if (!SetKeyValue(&probe, kv)) {
            IpcSend(c, "err bad-set %s\n", kv);
            return;
        }
        memcpy(c->staged[c->sets], kv, strlen(kv) + 1);
        c->sets++;
        return;
    }
    if (!strcmp(line, "apply")) {
        if (c->sets) {
            int k;
            /* only the staged keys are written: every other field keeps the
             * value it has right now, not the one the batch started from */
            for (k = 0; k < c->sets; k++)
                SetKeyValue(&g_s, c->staged[k]);
            ApplySettings(1);           /* normalizes a refused mode=1 */
            IpcSend(c, "ok applied %d\n", c->sets);
        } else {
            IpcSend(c, "ok nothing-to-apply\n");
        }
        c->sets = 0;                   /* the batch is consumed, applied or not */
        return;
    }
    IpcSend(c, "err unknown %s\n", line);
}

static int IpcReadable(struct IPC *c) {   /* returns 0 when closed */
    for (;;) {
        ssize_t n;
        /* a line must fit the buffer: a client that sends more than that
         * without ending the line is told, not dropped without a word */
        if (c->len >= sizeof c->buf - 1) {
            IpcSend(c, "err overflow\n");
            L("ipc: client line longer than the %d-byte buffer - dropped",
              (int)sizeof c->buf - 1);
            goto bye;
        }
        n = read(c->fd, c->buf + c->len, sizeof c->buf - 1 - c->len);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
            goto bye;
        }
        c->len += (size_t)n;
        c->buf[c->len] = 0;
        {
            char *p = c->buf, *nl;
            while ((nl = strchr(p, '\n'))) {
                *nl = 0;
                IpcCommand(c, p);
                p = nl + 1;
            }
            memmove(c->buf, p, strlen(p) + 1);
            c->len = strlen(c->buf);
        }
    }
bye:
    close(c->fd);
    free(c);
    return 0;
}

/* accept loop over the listen socket; each client gets its own IPC struct
 * which the caller polls later */
static struct IPC *g_ipc[64];
#define IPC_MAX ((int)(sizeof g_ipc / sizeof g_ipc[0]))

static void SockAccept(void) {
    for (;;) {
        struct IPC *c;
        int slot = -1, i;
        int fd = accept4(g_sock_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) return;
        for (i = 0; i < IPC_MAX; i++)
            if (!g_ipc[i]) { slot = i; break; }
        if (slot < 0) { close(fd); return; }   /* table full: refuse politely */
        c = calloc(1, sizeof *c);
        if (!c) { close(fd); continue; }
        c->fd = fd;
        c->pidx = -1;   /* accepted after the poll array was filled: the next
                         * poll places it, this wakeup must not read it */
        g_ipc[slot] = c;
    }
}

/* client side: connect, send the command line as protocol lines, print the
 * reply; returns 0 delivered, 2 nobody home, 3 request refused.
 *
 * The --set pairs arrive parsed and validated (main owns that grammar and
 * the staging bounds), so the request carries them as one batch: every pair
 * is staged and a single apply commits them, exactly like the fresh-start
 * path does.  The command flags follow the batch, so a --set pair is never
 * applied after the command it was given with. */
static int SendToRunning(int argc, char **argv, char **sets, int nsets) {
    struct sockaddr_un addr;
    const char *path = SockPath();
    int fd, i, k;
    /* one request line is "set " + the pair + '\n', and a pair is staged
     * only when it is shorter than STAGE_MAX, so a full batch plus the
     * trailing apply always fits */
    char req[MAX_SETS * (STAGE_MAX + sizeof("set ")) + 8];
    size_t len = 0;
    int dropped = 0;
    if (!path) { L("XDG_RUNTIME_DIR is not set; cannot find the command socket"); return 2; }
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 2;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr)) {
        close(fd);
        return 2;                            /* nobody is running */
    }
    for (k = 0; k < nsets; k++)
        len += (size_t)snprintf(req + len, sizeof req - len, "set %s\n", sets[k]);
    if (nsets) {
        memcpy(req + len, "apply\n", 6);
        len += 6;
    }
    for (i = 1; i < argc; i++) {
        for (k = 0; k < CMD_TABLE_N; k++)
            if (!strcmp(argv[i], CMD_TABLE[k].flag)) {
                int n;
                if (len + 1 >= sizeof req) { dropped++; break; }
                n = snprintf(req + len, sizeof req - len, "%s\n",
                             CMD_TABLE[k].proto);
                if (n > 0 && (size_t)n < sizeof req - len) len += (size_t)n;
                else dropped++;
                break;
            }
    }
    if (dropped) {
        L("--request refused: %d item(s) do not fit the %zu-byte request "
          "buffer", dropped, sizeof req);
        close(fd);
        return 3;
    }
    if (len) {
        ssize_t ignored = write(fd, req, len);
        (void)ignored;
        shutdown(fd, SHUT_WR);
        for (;;) {
            struct pollfd pf = { fd, POLLIN, 0 };
            char rbuf[512];
            ssize_t n;
            int pr = poll(&pf, 1, 5000);
            if (pr < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (pr == 0) { L("no reply within 5 s from the running instance"); break; }
            n = read(fd, rbuf, sizeof rbuf);
            if (n <= 0) break;
            fwrite(rbuf, 1, (size_t)n, stdout);
            fflush(stdout);
        }
    }
    close(fd);
    return 0;
}

/* ------------------------------------------------------------- dump path --- */

/* BMP with the exact layout of the Windows DumpBgra: 14-byte file header,
 * 40-byte info header, bottom-up 24bpp rows padded to 4 bytes. */
static int DumpBmp(const char *path, const unsigned char *bgra, int w, int h) {
    int stride = ((w * 3 + 3) / 4) * 4;
    unsigned char *row = calloc(1, (size_t)stride);
    FILE *f = fopen(path, "wb");
    unsigned char hdr[54];
    uint32_t filesize = 54 + (uint32_t)stride * (uint32_t)h;
    int x, y, ok = 1;
    if (!row || !f) { free(row); if (f) fclose(f); return 0; }
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (unsigned char)(filesize);       hdr[3] = (unsigned char)(filesize >> 8);
    hdr[4] = (unsigned char)(filesize >> 16); hdr[5] = (unsigned char)(filesize >> 24);
    hdr[10] = 54;                              /* bits offset */
    hdr[14] = 40;                              /* info header size */
    hdr[18] = (unsigned char)(w);  hdr[19] = (unsigned char)(w >> 8);
    hdr[20] = (unsigned char)(w >> 16); hdr[21] = (unsigned char)(w >> 24);
    hdr[22] = (unsigned char)(h);  hdr[23] = (unsigned char)(h >> 8);
    hdr[24] = (unsigned char)(h >> 16); hdr[25] = (unsigned char)(h >> 24);
    hdr[26] = 1;                               /* planes */
    hdr[28] = 24;                              /* bpp */
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = h - 1; y >= 0; y--) {
        for (x = 0; x < w; x++) {
            const unsigned char *s = bgra + 4 * ((size_t)y * w + x);
            row[x * 3 + 0] = s[0];
            row[x * 3 + 1] = s[1];
            row[x * 3 + 2] = s[2];
        }
        if (fwrite(row, 1, (size_t)stride, f) != (size_t)stride) { ok = 0; break; }
    }
    fclose(f);
    free(row);
    return ok;
}

static void CompositeOver(unsigned char *bgra, int w, int h, int bg) {
    int i;
    for (i = 0; i < w * h; i++) {
        unsigned char *p = bgra + 4 * (size_t)i;
        int a = p[3];
        p[0] = (unsigned char)(p[0] + bg * (255 - a) / 255);
        p[1] = (unsigned char)(p[1] + bg * (255 - a) / 255);
        p[2] = (unsigned char)(p[2] + bg * (255 - a) / 255);
        p[3] = 255;
    }
}

/* -------------------------------------------------- worker results pump --- */

static void WorkerResultsReady(void) {
    char drain[256];
    while (read(g_pipe[0], drain, sizeof drain) > 0)
        ;
    for (;;) {
        struct RESULT r;
        struct OUT *o;
        int stale = 1;
        pthread_mutex_lock(&g_wlock);
        if (!g_nresults) { pthread_mutex_unlock(&g_wlock); break; }
        r = g_results[--g_nresults];
        pthread_mutex_unlock(&g_wlock);
        o = (r.idx >= 0 && r.idx < MAX_MON) ? g_ov[r.idx] : NULL;
        if (o && o->configured && o->buf &&
            r.gen == g_settings_gen && r.w == o->w && r.h == o->h) {
            stale = 0;
            PresentBake(o, r.px);
            L("output %d: baked %dx%d in %.0f ms%s", r.idx, r.w, r.h,
              r.build_ms, o->attached == HALF_TEX ? "" : " (hidden, armed)");
        }
        if (r.idx >= 0 && r.idx < MAX_MON)
            BakeIdle(r.idx, r.gen);
        if (stale) {
            /* A settings change or resize beat this bake.  Never a silent
             * gap: re-queue deduped so the output converges. */
            L("output %d: stale bake dropped (gen %d vs %d) - re-queued",
              r.idx, r.gen, g_settings_gen);
            if (o) QueueBake(o);
        }
        free(r.px);
    }
}

/* ------------------------------------------------------------------ main --- */

static void Usage(void) {
    fprintf(stderr,
        "mnpaper %s - a paper texture over your whole screen (Wayland)\n\n"
        "  mnpaper                 run the overlay (foreground daemon)\n"
        "  mnpaper --on|--off|--toggle\n"
        "  mnpaper --paper|--eink  e-ink is refused on Wayland for now\n"
        "  mnpaper --set k=v ...   intensity|warmth|grain|fibre|blotch|share|master|...\n"
        "  mnpaper --status\n"
        "  mnpaper --quit\n"
        "  mnpaper --dump-tex out.bmp W H [bg]   headless texture render\n"
        "  mnpaper --log FILE\n",
        APP_VERSION);
}

int main(int argc, char **argv) {
    int i;
    int cmd = 0;
    int set_seen = 0;
    int bare_set = 0;
    int set_extra = 0;
    const char *set_extra_first = NULL;
    char *sets[MAX_SETS];
    int nsets = 0;
    char dump_tex[512] = "";
    int dump_w = 960, dump_h = 600, dump_bg = 235;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log") && i + 1 < argc) {
            int fd = open(argv[++i], O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
            if (fd >= 0) {
                g_log = fdopen(fd, "a");
                if (!g_log) close(fd);
            }
            L("=== mnpaper (wayland) start ===");
        } else if (!strcmp(argv[i], "--dump-tex")) {
            if (i + 3 >= argc) {
                L("--dump-tex needs an output file, a width and a height");
                Usage();
                return 2;
            }
            snprintf(dump_tex, sizeof dump_tex, "%s", argv[++i]);
            dump_w = atoi(argv[++i]);
            dump_h = atoi(argv[++i]);
            if (i + 1 < argc && argv[i + 1][0] != '-') dump_bg = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--set")) {
            set_seen = 1;
            if (i + 1 >= argc || argv[i + 1][0] == '-') bare_set = 1;
            while (i + 1 < argc && argv[i + 1][0] != '-' && nsets < MAX_SETS)
                sets[nsets++] = argv[++i];
            while (i + 1 < argc && argv[i + 1][0] != '-') {
                if (!set_extra) set_extra_first = argv[i + 1];
                set_extra++;
                L("--set batch full (max %d): ignoring %s", MAX_SETS, argv[i + 1]);
                i++;
            }
        } else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            Usage();
            return 0;
        } else {
            int k, mapped = 0;
            for (k = 0; k < CMD_TABLE_N; k++)
                if (!strcmp(argv[i], CMD_TABLE[k].flag)) {
                    cmd = CMD_TABLE[k].cmd;
                    mapped = 1;
                    break;
                }
            if (!mapped) L("ignoring unknown argument: %s", argv[i]);
        }
    }

    /* ---- --set grammar, checked once at the boundary every path shares ----
     * A malformed operand - no '=', an unknown key, a value that is not
     * digits, one longer than the staging bound - and a batch beyond
     * MAX_SETS are refused by name and nothing is applied, so the same
     * command line behaves the same whether an instance is already running
     * or not (see MAX_SETS).  --dump-tex is the documented exception: it
     * logs the bad entry and still renders, inside its own branch below. */
    if (set_seen && (!nsets || bare_set)) {
        L("--set needs at least one key=value pair (for example "
          "--set warmth=45)");
        return 2;
    }
    if (!dump_tex[0]) {
        SETTINGS probe = PaperDefaultSettings();
        int k;
        for (k = 0; k < nsets; k++) {
            if (strlen(sets[k]) >= STAGE_MAX) {
                L("--set refused: %s is %d characters (the maximum is %d)",
                  sets[k], (int)strlen(sets[k]), STAGE_MAX - 1);
                return 2;
            }
            if (!SetKeyValue(&probe, sets[k])) {
                L("--set refused: %s is not a key=value pair", sets[k]);
                return 2;
            }
        }
        if (set_extra) {
            L("--set refused: more than %d key=value pairs (first ignored: %s)",
              MAX_SETS, set_extra_first);
            return 2;
        }
    }

    /* ---- headless texture dump: no display, no compositor ---- */
    if (dump_tex[0]) {
        unsigned char *px;
        LoadSettings();
        if (nsets) {
            int k;
            for (k = 0; k < nsets; k++)
                if (!SetKeyValue(&g_s, sets[k]))
                    L("ignoring unknown or malformed --set entry: %s", sets[k]);
            ClampSettingsOf(&g_s);
        }
        RequestMode(g_s.mode);
        if (dump_w <= 0 || dump_h <= 0 || dump_w > 16384 || dump_h > 16384) {
            L("dump size out of range: %dx%d", dump_w, dump_h);
            return 2;
        }
        if (dump_bg < 0 || dump_bg > 255) {
            L("dump bg out of range: %d (expected 0-255)", dump_bg);
            return 2;
        }
        px = malloc((size_t)dump_w * dump_h * 4);
        if (!px) { L("out of memory"); return 2; }
        BuildPaperFull(px, dump_w, dump_h, 1.f, &g_s, NULL);
        CompositeOver(px, dump_w, dump_h, dump_bg);
        if (!DumpBmp(dump_tex, px, dump_w, dump_h)) L("dump write failed: %s", dump_tex);
        else L("dump: %s (%dx%d, bg %d, mode %s)", dump_tex, dump_w, dump_h, dump_bg,
               "paper");   /* e-ink is not implemented; never claim otherwise */
        free(px);
        return 0;
    }

    /* ---- command a running instance, or fall through and start ---- */
    if (cmd || nsets) {
        int rc = SendToRunning(argc, argv, sets, nsets);
        if (rc == 0) return 0;
        if (rc == 3) return 2;      /* refused before anything was sent */
        if (cmd && cmd != CMD_ON && cmd != CMD_PAPER) {
            /* a command with nobody to hear it is a dropped command */
            L("no running mnpaper instance; start one with: mnpaper");
            return 2;
        }
    }

    LoadSettings();
    if (nsets) {       /* validated above: none of these can fail */
        int k;
        for (k = 0; k < nsets; k++) SetKeyValue(&g_s, sets[k]);
        ClampSettingsOf(&g_s);
    }
    /* a visibility command on the fresh path means what it means against a
     * running instance: start shown, in paper mode */
    if (cmd == CMD_ON) g_s.master = 1;
    else if (cmd == CMD_PAPER) { g_s.mode = MODE_PAPER; g_s.master = 1; }

    /* ---- become the daemon ---- */
    /* At login the autostart entry can fire before the compositor's socket
     * exists.  Without this retry the texture would silently never appear
     * and there would be nothing running to ask.  Bounded: ~20 s, then the
     * honest error below.  A human-started run usually connects on the
     * first try and pays nothing. */
    {
        int tries;
        for (tries = 0; !(g_dpy = wl_display_connect(NULL)) && tries < 40; tries++) {
            if (!tries)
                L("compositor not reachable yet - waiting (autostart race)");
            struct timespec ts = { 0, 500 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    if (!g_dpy) {
        L("cannot connect to a Wayland compositor (is WAYLAND_DISPLAY set?)");
        return 1;
    }
    g_reg = wl_display_get_registry(g_dpy);
    wl_registry_add_listener(g_reg, &RegistryListener, NULL);
    wl_display_roundtrip(g_dpy);
    if (!g_comp || !g_shm) {
        L("compositor is missing %s - nothing to draw with",
          !g_comp ? "wl_compositor" : "wl_shm");
        return 1;
    }
    if (!g_shell) {
        L("compositor does not advertise zwlr_layer_shell_v1: an overlay over "
          "your windows is impossible there (KWin, sway, Hyprland and other "
          "wlroots compositors work; GNOME/Mutter does not)");
        return 1;
    }
    /* outputs announced before the globals above waited in OutCreate; build
     * their surfaces now that every global they need is bound */
    OutSurfaceInitAll();
    wl_display_roundtrip(g_dpy);            /* outputs finish their geometry */
    if (!g_nov) {
        L("no outputs found - nothing to cover");
        return 1;
    }

    if (pipe2(g_pipe, O_CLOEXEC | O_NONBLOCK)) {
        L("pipe failed");
        return 1;
    }

    /* ---- command socket: claim it after the display is up, so a command
     * sent to us in the first milliseconds finds a live instance ---- */
    {
        int sfd = SockStart();
        if (sfd == -2) {
            L("another mnpaper instance is already serving %s", SockPath());
            return 2;
        }
        if (sfd < 0)
            L("warning: no command socket - the overlay runs, but --on/--off/--set/--status cannot reach it");
        g_sock_fd = sfd;
    }

    signal(SIGPIPE, SIG_IGN);
    /* SIGINT/SIGTERM through a signalfd: no async handler, the poll loop
     * below wakes up like any other fd and we exit cleanly */
    {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        sigprocmask(SIG_BLOCK, &mask, NULL);
        g_sig_fd = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);
        if (g_sig_fd < 0) {
            L("signalfd failed");
            return 1;
        }
    }

    ApplySettings(1);   /* stamps every queued job with the final generation */

    /* worker starts last: any job queued by an early configure is baked at
     * the current generation, so startup costs exactly one bake per output */
    g_worker_on = (pthread_create(&g_worker, NULL, WorkerMain, NULL) == 0);
    if (!g_worker_on) {
        L("could not start the bake worker");
        return 1;
    }

    L("mnpaper %s ready: master=%d mode=%s outputs=%d", APP_VERSION,
      g_s.master, g_s.mode == MODE_PAPER ? "paper" : "eink", g_nov);

    tray_start(&g_traycb);   /* optional; fails soft on sessions without a tray */

    /* ---- main loop: one poll over the display, the worker pipe, the
     * command socket and its clients, and the signalfd.  The display side
     * uses the prepare-read/flush dance so a busy queue can never starve
     * the other fds (the pattern wl_client docs recommend for clients). */
    while (!g_quit) {
        struct pollfd pfd[5 + IPC_MAX];   /* display, pipe, signalfd, socket,
                                           * tray, plus one slot per client */
        int n = 0, widx, pipeidx, sigidx, sockidx;
        int pr, rc;
        int i;

        /* prepare_read fails only while events are already queued: drain them
         * and retry.  Bail out on a real error - retrying a dead display
         * would spin forever.  Whatever happens, we leave this loop with the
         * display in exactly one of two states: prepared, or not prepared at
         * all (a failed dispatch_pending left nothing prepared, so no cancel
         * is owed).  Mixing the two is a protocol error, so the loop never
         * exits "prepared but unaccounted". */
        pr = -1;
        while (pr != 0) {
            pr = wl_display_prepare_read(g_dpy);
            if (pr == 0) break;
            if (wl_display_dispatch_pending(g_dpy) < 0) {
                if (errno != EINTR) {
                    L("wayland dispatch failed (errno %d) - exiting", errno);
                    g_quit = 1;
                }
                break;                   /* nothing prepared: no cancel owed */
            }
            if (g_quit) break;           /* not prepared here either */
        }
        if (pr != 0) break;              /* nothing prepared: leave the loop */
        wl_display_flush(g_dpy);

        widx = n;   pfd[n].fd = wl_display_get_fd(g_dpy);
        pfd[n].events = POLLIN; pfd[n++].revents = 0;
        pipeidx = n; pfd[n].fd = g_pipe[0];
        pfd[n].events = POLLIN; pfd[n++].revents = 0;
        sigidx = n; pfd[n].fd = g_sig_fd;
        pfd[n].events = POLLIN; pfd[n++].revents = 0;
        sockidx = -1;
        if (g_sock_fd >= 0) {
            sockidx = n; pfd[n].fd = g_sock_fd;
            pfd[n].events = POLLIN; pfd[n++].revents = 0;
        }
        if (tray_fd() >= 0) {
            pfd[n].fd = tray_fd();
            pfd[n].events = tray_events(); pfd[n++].revents = 0;
        }
        for (i = 0; i < IPC_MAX; i++)
            if (g_ipc[i]) g_ipc[i]->pidx = -1;
        for (i = 0; i < IPC_MAX && n < (int)(sizeof pfd / sizeof pfd[0]); i++)
            if (g_ipc[i]) {
                g_ipc[i]->pidx = n;   /* readiness is looked up through this,
                                       * never by counting positions */
                pfd[n].fd = g_ipc[i]->fd;
                pfd[n].events = POLLIN;
                pfd[n++].revents = 0;
            }

        rc = poll(pfd, (nfds_t)n, tray_timeout_ms());
        if (rc < 0 && errno != EINTR) {
            wl_display_cancel_read(g_dpy);
            L("poll failed (errno %d) - exiting", errno);
            break;
        }
        if (pfd[widx].revents & POLLIN) {
            if (wl_display_read_events(g_dpy) < 0 && errno != EINTR) {
                L("wayland read failed (errno %d) - exiting", errno);
                g_quit = 1;
            }
            if (wl_display_dispatch_pending(g_dpy) < 0 && errno != EINTR) {
                L("wayland dispatch failed (errno %d) - exiting", errno);
                g_quit = 1;
            }
        } else {
            wl_display_cancel_read(g_dpy);
        }

        if (pfd[pipeidx].revents & POLLIN) WorkerResultsReady();
        if (pfd[sigidx].revents & POLLIN) {
            struct signalfd_siginfo si;
            while (read(g_sig_fd, &si, sizeof si) == sizeof si)
                L("signal %u: shutting down", si.ssi_signo);
            g_quit = 1;
        }
        if (sockidx >= 0 && (pfd[sockidx].revents & POLLIN)) SockAccept();
        tray_process();   /* tray fd, dbus timeouts and menu actions */
        for (i = 0; i < IPC_MAX; i++) {
            if (!g_ipc[i] || g_ipc[i]->pidx < 0) continue;
            if (pfd[g_ipc[i]->pidx].revents & (POLLIN | POLLHUP | POLLERR))
                if (!IpcReadable(g_ipc[i])) {
                    g_ipc[i] = NULL;   /* freed inside IpcReadable */
                }
        }
    }

    L("shutting down");
    WorkerStop();
    for (i = 0; i < MAX_MON; i++) {
        if (g_ov[i]) { OutTeardown(g_ov[i]); g_ov[i] = NULL; }
    }
    ConfigWrite(&g_s);
    if (g_dpy) wl_display_disconnect(g_dpy);
    if (g_sock_fd >= 0) close(g_sock_fd);
    if (g_sig_fd >= 0) close(g_sig_fd);
    {
        const char *path = SockPath();
        if (path) unlink(path);
    }
    if (g_log) fclose(g_log);
    return 0;
}
