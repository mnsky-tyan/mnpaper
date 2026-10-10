/*
 * mnPaper.c - whole-screen paper texture + e-ink mode for Windows.
 *
 * One portable native exe, zero dependencies, no installer, no cache files.
 *
 *   Paper mode : per-monitor WS_EX_LAYERED click-through overlay painting a
 *                procedural paper texture (grain + fibres + blotches, warm
 *                off-white) via UpdateLayeredWindow per-pixel alpha.
 *   E-ink mode : DXGI Desktop Duplication capture (GDI poll fallback), then
 *                capture -> luminance -> shade quantization -> Bayer dither,
 *                presented through the same click-through overlays.
 *
 * State model (frozen spec):
 *   master : on/off, Ctrl+Alt+P toggles
 *   mode   : paper / e-ink, Ctrl+Alt+E switches (also turns master on)
 * All overlay windows use SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)
 * so the effect never appears in screen shares or captures.
 *
 * Second instance commands: --on --off --toggle --paper --eink --settings
 *                           --quit --set key=value --log FILE --no-exclude
 *                           --capture on|off
 */
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS 1
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define UNICODE
#define _UNICODE
#define CINTERFACE

#include <windows.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <commctrl.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <emmintrin.h>  /* SSE2 byte interpolation on the native x64 build */

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "ole32.lib")

#ifndef DXGI_ERROR_WAIT_TIMEOUT
#define DXGI_ERROR_WAIT_TIMEOUT ((HRESULT)0x887A0027L)
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((DPI_AWARENESS_CONTEXT)-4)
#endif

#define WM_APP_TRAY     (WM_APP + 2)
#define IDM_MASTER      9001
#define IDM_PAPER       9002
#define IDM_EINK        9003
#define IDM_SETTINGS    9004
#define IDM_STRENGTH    9007      /* + index: 10/20/30/40 */
#define IDM_SHARE       9020
#define IDM_AUTOSTART   9005
#define IDM_AUTOUPD     9021
#define IDM_EXIT        9006
#define TIMER_TICK      1
#define TIMER_DEBOUNCE  2
#define TIMER_UPD       4   /* daily update check: 30s in, then hourly so UpdDue gates the fetch */
#define TICK_MS         50
#define DEBOUNCE_MS     400     /* trailing registry save only; the live
                                 * preview itself applies immediately */
#define WM_APP_UPDATE   (WM_APP + 5)   /* update-check thread -> host window */
#define WM_APP_INSTALL  (WM_APP + 6)   /* self-update worker thread -> host window */
#define WM_APP_CMD      (WM_APP + 1)   /* second instance -> first instance: tray command */

/* ------------------------------- version -------------------------------- */
/* The release number lives in version.h, shared with mnPaper.rc, so the number
 * compared against the published feed and the number Windows reads out of the
 * exe's version resource cannot drift apart. They did: 2.7.3 through 2.7.5
 * bumped only the resource, so those builds reported themselves as 2.7.2 and
 * kept offering an update that was already installed. The release NUMBER lives
 * here and nowhere else in code; cutting a release then also pins the built
 * exe's SHA-256 in version.txt (the flow in AGENTS.md, checked by
 * tests/release_guard.py).
 *
 * Before publishing, point UPDATE_URL at a plain-text file whose first line is
 * the latest version ("2.7.6") and whose body carries the 64-hex SHA-256 pin
 * of that release's exe (FindHash64 below: a run of exactly 64 hex chars
 * bounded by non-hex bytes, anywhere in the feed), and PRODUCT_URL at the page
 * users download
 * from (GitHub Releases recommended: free TLS hosting, the release itself is
 * the artifact). The check itself is read-only: it fetches that feed and
 * compares versions, so a hostile or offline feed can at worst show a wrong
 * message. The exe is downloaded only after the user confirms the install
 * prompt, and nothing is written or swapped until its computed SHA-256 matches
 * the published pin - the feed is trusted for a version string, a pin, and for
 * nothing else. */
#include "version.h"
#define UPDATE_URL  L"https://raw.githubusercontent.com/mnsky-tyan/mnpaper/main/version.txt"
#define PRODUCT_URL L"https://github.com/mnsky-tyan/mnpaper/releases"
/* self-update payload: stable redirect URL, not the rate-limited REST API */
#define UPDATE_EXE_URL L"https://github.com/mnsky-tyan/mnpaper/releases/latest/download/mnPaper.exe"
#define UPDATE_MAX_BYTES (8u * 1024u * 1024u)
#define FEED_MAX_BYTES 4096   /* the feed is a tiny text file; anything bigger is refused */

#define MODE_PAPER 0
#define MODE_EINK  1

#define HOTKEY_MASTER 1
#define HOTKEY_EINK   2

/* ------------------------------------------------------------ settings --- */

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
    int autoupd;   /* 1 = look for a new version about once a day (never auto-install) */
} SETTINGS;

static LONG WINAPI CrashDump(EXCEPTION_POINTERS *ep);

/* crash forensics: minidump for offline stack mapping */
static LONG WINAPI CrashDump(EXCEPTION_POINTERS *ep) {
    HANDLE f;
    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH - 19);
    WCHAR *slash;
    path[n] = 0;
    slash = wcsrchr(path, L'\\');
    if (slash) *slash = 0;
    lstrcatW(path, L"\\mnpaper-crash.dmp");
    f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei;
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), f,
                          MiniDumpNormal, &mei, NULL, NULL);
        CloseHandle(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static SETTINGS g_s = { .master = 1, .mode = MODE_PAPER, .intensity = 30,
                        .warmth = 45, .grain = 4, .fibre = 40, .blotch = 30,
                        .shades = 4, .contrast = 50, .dither = 75,
                        .autostart = 0, .share = 0, .autoupd = 1 };
/* Test mode: the hidden regression suites set this before anything runs. It
 * gates every side effect a test must not perform (windows on the working
 * desktop, network, the Run key); declared here because ShowOverlay, the
 * first gate, sits well above the help window that used to own it. */
int g_test_headless = 0;
static int g_hotkey_failed;

static const WCHAR *REG_KEY = L"Software\\mnPaper";
/* The Run key path is a writable buffer so the hidden regression can redirect
 * it into its isolated scratch hive; production always keeps the default. */
static WCHAR g_run_key[160] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static ULONGLONG g_lastupd;          /* FILETIME of the last completed update check */
static int g_upd_fails;              /* consecutive checks that could not read the feed */

static FILE *g_log;
static void L(const char *fmt, ...) {
    va_list ap;
    char buf[512];
    int n;
    va_start(ap, fmt);
    n = _vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    buf[sizeof buf - 1] = 0;
    OutputDebugStringA(buf);   /* visible in DbgView, no log file needed */
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        fprintf(g_log, "\n");
        fflush(g_log);
        va_end(ap);
    }
}

static void ClampSettingsOf(SETTINGS *s) {
    int *vals[] = { &s->master, &s->mode, &s->intensity, &s->warmth,
                    &s->grain, &s->fibre, &s->blotch, &s->shades,
                    &s->contrast, &s->dither, &s->autostart, &s->share,
                    &s->autoupd };
    int mins[]  = { 0, 0, 0, 0, 2, 0, 0, 2, 0, 0, 0, 0, 0 };
    int maxs[]  = { 1, 1, 40, 100, 12, 100, 100, 16, 100, 100, 1, 1, 1 };
    int i;
    for (i = 0; i < 13; i++) {
        if (*vals[i] < mins[i]) *vals[i] = mins[i];
        if (*vals[i] > maxs[i]) *vals[i] = maxs[i];
    }
}

static void ClampSettings(void) {
    ClampSettingsOf(&g_s);
}

static void ApplyAutostart(void) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, g_run_key, 0, NULL, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) {
        L("autostart: cannot open Run key, err %lu", (unsigned long)GetLastError());
        return;
    }
    if (g_s.autostart) {
        WCHAR path[MAX_PATH];
        DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
        if (n > 0 && n < MAX_PATH)
            RegSetValueExW(k, L"mnPaper", 0, REG_SZ, (BYTE *)path, (n + 1) * sizeof(WCHAR));
        else
            /* n == MAX_PATH means GetModuleFileNameW null-truncated: writing
             * it would create a Run entry naming a path that does not exist */
            L("autostart: module path unusable (%lu) - Run entry not written",
              (unsigned long)n);
        /* a transient path-read failure here must not silently disable autostart */
    } else {
        /* Remove the entry only when it is absent or names this exe. A copy at
         * another path derives autostart=0 from the installed copy's entry
         * (LoadSettings), so an unconditional delete would remove an entry
         * that belongs to a different exe. */
        WCHAR path[MAX_PATH] = L"", mine[MAX_PATH] = L"";
        DWORD sz = sizeof(path), t = 0, n;
        n = GetModuleFileNameW(NULL, mine, MAX_PATH);
        /* a truncated path does not name this exe, so ownership cannot be
         * proven: leave the entry alone (same rule as the write half and
         * InstallResult) instead of matching a 259-char prefix written by an
         * older build */
        if (n > 0 && n < MAX_PATH &&
            (RegQueryValueExW(k, L"mnPaper", NULL, &t, (BYTE *)path, &sz) != ERROR_SUCCESS ||
             t != REG_SZ || _wcsicmp(path, mine) == 0))
            RegDeleteValueW(k, L"mnPaper");
    }
    RegCloseKey(k);
}

static void SaveSettings(void) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, L"master",    0, REG_DWORD, (BYTE *)&g_s.master,    sizeof(int));
    RegSetValueExW(k, L"mode",      0, REG_DWORD, (BYTE *)&g_s.mode,      sizeof(int));
    RegSetValueExW(k, L"intensity", 0, REG_DWORD, (BYTE *)&g_s.intensity, sizeof(int));
    RegSetValueExW(k, L"warmth",    0, REG_DWORD, (BYTE *)&g_s.warmth,    sizeof(int));
    RegSetValueExW(k, L"grain",     0, REG_DWORD, (BYTE *)&g_s.grain,     sizeof(int));
    RegSetValueExW(k, L"fibre",     0, REG_DWORD, (BYTE *)&g_s.fibre,     sizeof(int));
    RegSetValueExW(k, L"blotch",    0, REG_DWORD, (BYTE *)&g_s.blotch,    sizeof(int));
    RegSetValueExW(k, L"shades",    0, REG_DWORD, (BYTE *)&g_s.shades,    sizeof(int));
    RegSetValueExW(k, L"contrast",  0, REG_DWORD, (BYTE *)&g_s.contrast,  sizeof(int));
    RegSetValueExW(k, L"dither",    0, REG_DWORD, (BYTE *)&g_s.dither,    sizeof(int));
    /* no "autostart" value here: LoadSettings derives it from the Run key,
     * so a stored copy could only diverge from the truth (2026-10-04 review) */
    RegSetValueExW(k, L"share",     0, REG_DWORD, (BYTE *)&g_s.share,     sizeof(int));
    RegSetValueExW(k, L"autoupd",   0, REG_DWORD, (BYTE *)&g_s.autoupd,   sizeof(int));
    RegSetValueExW(k, L"lastupd",   0, REG_QWORD, (BYTE *)&g_lastupd,    sizeof(g_lastupd));
    RegSetValueExW(k, L"updfails",  0, REG_DWORD, (BYTE *)&g_upd_fails,  sizeof(g_upd_fails));
    RegCloseKey(k);
    ApplyAutostart();
}

static int GetDword(HKEY k, const WCHAR *name, int def) {
    DWORD v = 0, sz = sizeof(v), t = 0;
    if (RegQueryValueExW(k, name, NULL, &t, (BYTE *)&v, &sz) == ERROR_SUCCESS && t == REG_DWORD)
        return (int)v;
    return def;
}

static void LoadSettings(void) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        g_s.master    = GetDword(k, L"master",    g_s.master);
        g_s.mode      = GetDword(k, L"mode",      g_s.mode);
        g_s.intensity = GetDword(k, L"intensity", g_s.intensity);
        g_s.warmth    = GetDword(k, L"warmth",    g_s.warmth);
        g_s.grain     = GetDword(k, L"grain",     g_s.grain);
        g_s.fibre     = GetDword(k, L"fibre",     g_s.fibre);
        g_s.blotch    = GetDword(k, L"blotch",    g_s.blotch);
        g_s.shades    = GetDword(k, L"shades",    g_s.shades);
        g_s.contrast  = GetDword(k, L"contrast",  g_s.contrast);
        g_s.dither    = GetDword(k, L"dither",    g_s.dither);
        g_s.share     = GetDword(k, L"share",     g_s.share);
        g_s.autoupd   = GetDword(k, L"autoupd",   1);   /* default: daily check on */
        {
            DWORD sz = sizeof(ULONGLONG), t = 0;
            ULONGLONG v64 = 0;
            if (RegQueryValueExW(k, L"lastupd", NULL, &t, (BYTE *)&v64, &sz) == ERROR_SUCCESS && t == REG_QWORD)
                g_lastupd = v64;
        }
        g_upd_fails = GetDword(k, L"updfails", 0);
        RegCloseKey(k);
    }
    /* autostart mirrors the Run key so an external edit stays honest */
    if (RegOpenKeyExW(HKEY_CURRENT_USER, g_run_key, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        WCHAR path[MAX_PATH] = L"", mine[MAX_PATH] = L"";
        DWORD sz = sizeof(path), t = 0;
        if (RegQueryValueExW(k, L"mnPaper", NULL, &t, (BYTE *)path, &sz) == ERROR_SUCCESS && t == REG_SZ) {
            GetModuleFileNameW(NULL, mine, MAX_PATH);
            if (_wcsicmp(path, mine) == 0) g_s.autostart = 1;
            else g_s.autostart = 0;
        }
        RegCloseKey(k);
    }
    ClampSettings();
}

/* ---------------------------------------------------------------- noise --- */

static unsigned lh(int x, int y, int seed) {
    unsigned h = (unsigned)(x * 73856093 ^ y * 19349663 ^ seed * 83492791);
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



/* ------------------------------------------------------------- overlays --- */

/* The veil covers every monitor, so a monitor past MAX_MON would get no
 * texture at all. Windows itself tops out well under this, but a silent stop
 * is the failure the 2026-10-09 round fixed for MAX_STRIPS: say which bound
 * was hit and what was dropped. */
#define MAX_MON 16

/* Per monitor, STRIP_H tall each. The cap must exceed any plausible monitor
 * height divided by STRIP_H: at 8 strips it was 3600px, so a 4K display in
 * portrait (2160x3840), a 2880x5120 5K panel or an 8K TV got its bottom band
 * left untextured with no log at all (2026-10-09 review). 32 strips covers
 * 14400px, which every shipping panel is under. STRIP_H stays pinned to its
 * measured value: raising the strip height is what risks the auto-hide
 * taskbar raise (see the 450-vs-1800 measurement below). */
#define MAX_STRIPS 32
#define STRIP_H    450              /* < the shell's ~800-900px fullscreen
                                      * height trigger: a full-width window
                                      * this short never suppresses the
                                      * auto-hide taskbar raise (measured
                                      * 4/4 raises at 450, 0/2 at 1800)     */
typedef struct {
    int      idx;
    HMONITOR mon;
    HWND     hwnd;                 /* = shwnd[0]; kept for single-window use */
    HWND     shwnd[MAX_STRIPS];    /* horizontal strips covering the monitor */
    int      n_strips;
    RECT     rc;
    int      w, h;
    HDC      mem;
    HBITMAP  dib;
    void    *bits;
} OVL;

static OVL  g_ov[MAX_MON];
static int  g_n;
/* Hole-in-the-veil state. Fighting the shell over the taskbar's z-order
 * turned out to be intermittent (the shell re-orders it whenever it feels
 * like it), so instead the veil stops painting over the taskbar entirely:
 * while the taskbar is revealed, the veil pixels there are fully
 * transparent alpha and the real taskbar shows through pristine, no matter
 * which side of the overlay it sits on. */
static RECT g_hole[MAX_MON];
static int  g_hole_on[MAX_MON];
static int  g_hole_dirty;
static HWND g_host;
static int  g_force_capture_show;   /* --no-exclude validation flag, session only */

/* Should the overlay be excluded from screenshots and screen shares?
 * Yes by default so the texture never leaks into a share. Toggleable at
 * runtime. E-ink mode is always excluded: Desktop
 * Duplication would capture the e-ink output itself and feed it back,
 * white-washing the screen within seconds. */
static int CaptureHidden(void) {
    if (g_s.mode == MODE_EINK) return 1;   /* before any flag: no capture feedback */
    if (g_force_capture_show) return 0;
    return g_s.share ? 0 : 1;
}

static void ApplyCaptureState(OVL *ov) {
    int s;
    for (s = 0; s < ov->n_strips; s++) {
        if (!ov->shwnd[s]) continue;
        SetWindowDisplayAffinity(ov->shwnd[s],
                                 CaptureHidden() ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE);
    }
}

/* Capture state is a property of the strip windows alone, so a change to it
 * (the share toggle) needs no texture work: re-applying it must never cost a
 * re-bake, which is what routing this through RepaintAll used to do. */
static void ApplyCaptureAll(void) {
    int i;
    for (i = 0; i < g_n; i++) ApplyCaptureState(&g_ov[i]);
}

/* Lift one overlay's strips to the top of the topmost band. Add-only: never
 * demote (a non-topmost insert-after clears WS_EX_TOPMOST and ping-pongs with
 * the shell - see the unconditional-raise comment in Housekeeping). */
static void RaiseOverlay(OVL *ov) {
    int s;
    for (s = 0; s < ov->n_strips; s++)
        SetWindowPos(ov->shwnd[s], HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

typedef struct {
    HMONITOR mon;
    RECT     rc;
} MONINFO;
static MONINFO g_mi[MAX_MON];
static int     g_nmi;

/* Preview samples the same full-screen coordinate system on an 8px grid.
 * Bilinear expansion preserves colour/strength changes while reducing the
 * costly noise evaluation to 1/64 of the pixels. Full quality follows idle. */
#define PAPER_PREVIEW_SHIFT 3
#define PAPER_PREVIEW_STEP (1 << PAPER_PREVIEW_SHIFT)
static volatile LONG g_stop_thread;
static volatile LONG g_pepoch[MAX_MON];

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
    /* Warmth spans cool -> neutral(50) -> aged amber. The old mapping only
     * moved endpoints by ~24/255 and never went below neutral, so 0-100
     * looked identical on screen (user feedback: "too calm"). Signed swing:
     * cool reduces red and raises blue; warm raises red/brown and cuts blue. */
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
    /* Fibre and blotch used to only nudge the noise mix (early feedback:
     * "they don't seem to do much"). They now add their own alpha modulation on
     * top of the mix, scaled with the strength slider, so the endpoints are
     * unmistakable and 0 still means off. */
    p.fspread = p.spread * 13 * sp->fibre / 10 / 100;   /* up to +1.3x spread */
    p.bspread = p.spread * 12 * sp->blotch / 10 / 100;  /* up to +1.2x spread */
    return p;
}

static void PaperPixel(unsigned char *out, int x, int y, const PAPERPARAMS *p) {
    /* fs scales ALL noise frequencies at once: the 8px preview grid samples
     * a coarser paper (same character, 8x larger features) instead of
     * aliasing fine grain into a flat smear - the old preview made grain
     * changes look dead until the full refine landed (user feedback: "grain
     * lags behind"). */
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

static int PaperCancelled(volatile LONG *epoch, LONG expected) {
    return epoch && (InterlockedCompareExchange(&g_stop_thread, 0, 0) ||
                    InterlockedCompareExchange(epoch, 0, 0) != expected);
}

static int BuildPaperFull(unsigned char *px, int w, int h, const SETTINGS *sp,
                          volatile LONG *epoch, LONG expected) {
    PAPERPARAMS p = PaperParams(w, sp);
    int x, y;
    for (y = 0; y < h; y++) {
        if (PaperCancelled(epoch, expected)) return 0;
        for (x = 0; x < w; x++)
            PaperPixel(px + 4 * ((size_t)y * w + x), x, y, &p);
    }
    return 1;
}

static void ExpandPaperRow(unsigned char *dst, const unsigned char *grid, int w) {
    int x, c;
    for (x = 0; x < w; x++) {
        int fx = x % PAPER_PREVIEW_STEP;
        const unsigned char *a = grid + 4 * (x / PAPER_PREVIEW_STEP);
        for (c = 0; c < 4; c++)
            dst[4 * x + c] = (unsigned char)((a[c] * (PAPER_PREVIEW_STEP - fx) +
                                  a[c + 4] * fx + PAPER_PREVIEW_STEP / 2) / PAPER_PREVIEW_STEP);
    }
}

/* Same cancel contract as BuildPaperFull: epoch == NULL means uncancellable
 * (the show path, which must finish even though RepaintAll has parked the
 * stop flag at 1 while it shows the strips). */
static int BuildPaperPreview(unsigned char *px, int w, int h, const SETTINGS *sp,
                             volatile LONG *epoch, LONG expected) {
    PAPERPARAMS p = PaperParams(w, sp);
    int step = PAPER_PREVIEW_STEP;
    p.fs = (float)step;   /* preview: same character, step-x coarser noise */
    int gw = (w + step - 1) / step + 1, gh = (h + step - 1) / step + 1;
    unsigned char *grid = (unsigned char *)malloc((size_t)gw * gh * 4);
    unsigned char *rows = (unsigned char *)malloc((size_t)w * 8);
    int x, y;
    __m128i zero = _mm_setzero_si128(), round = _mm_set1_epi16(PAPER_PREVIEW_STEP / 2);
    if (!grid || !rows) { free(grid); free(rows); return 0; }
    for (y = 0; y < gh; y++) {
        if (PaperCancelled(epoch, expected)) goto cancelled;
        for (x = 0; x < gw; x++)
            PaperPixel(grid + 4 * ((size_t)y * gw + x), x * step, y * step, &p);
    }
    for (y = 0; y < h; y++) {
        int fy = y % step, bytes = w * 4;
        unsigned char *top = rows, *bot = rows + bytes;
        unsigned char *out = px + (size_t)y * bytes;
        __m128i wa = _mm_set1_epi16((short)(step - fy)), wb = _mm_set1_epi16((short)fy);
        if (PaperCancelled(epoch, expected)) goto cancelled;
        if (!fy) {
            if (y) memcpy(top, bot, bytes);
            else ExpandPaperRow(top, grid, w);
            ExpandPaperRow(bot, grid + (size_t)(y / step + 1) * gw * 4, w);
        }
        /* Interpolate 16 BGRA bytes together; reuse expanded grid rows for
         * eight output rows rather than evaluating horizontal weights per
         * full-resolution pixel. No additional persistent texture cache. */
        for (x = 0; x + 16 <= bytes; x += 16) {
            __m128i a = _mm_loadu_si128((const __m128i *)(top + x));
            __m128i b = _mm_loadu_si128((const __m128i *)(bot + x));
            __m128i lo = _mm_add_epi16(_mm_mullo_epi16(_mm_unpacklo_epi8(a, zero), wa),
                                     _mm_mullo_epi16(_mm_unpacklo_epi8(b, zero), wb));
            __m128i hi = _mm_add_epi16(_mm_mullo_epi16(_mm_unpackhi_epi8(a, zero), wa),
                                     _mm_mullo_epi16(_mm_unpackhi_epi8(b, zero), wb));
            lo = _mm_srli_epi16(_mm_add_epi16(lo, round), PAPER_PREVIEW_SHIFT);
            hi = _mm_srli_epi16(_mm_add_epi16(hi, round), PAPER_PREVIEW_SHIFT);
            _mm_storeu_si128((__m128i *)(out + x), _mm_packus_epi16(lo, hi));
        }
        for (; x < bytes; x++)
            out[x] = (unsigned char)((top[x] * (step - fy) + bot[x] * fy + step / 2) / step);
    }
    free(grid); free(rows);
    return 1;
cancelled:
    free(grid); free(rows);
    return 0;
}

static void BuildPaperInto(unsigned char *px, int w, int h, const SETTINGS *sp) {
    BuildPaperFull(px, w, h, sp, NULL, 0);
}


/* ClipRect is defined with GrowRect in the housekeeping section, below
 * LocalHole - forward-declared here rather than splitting the pair. */
static void ClipRect(RECT *r, const RECT *to);

/* hole rect in overlay-local coordinates, 0 if none */
static void LocalHole(const OVL *ov, int *on, int *x0, int *y0, int *x1, int *y1) {
    RECT full, local;
    *on = 0; *x0 = *y0 = *x1 = *y1 = 0;
    if (ov->idx >= 0 && ov->idx < MAX_MON && g_hole_on[ov->idx]) {
        SetRect(&full, 0, 0, ov->w, ov->h);          /* the overlay's own extent */
        local = g_hole[ov->idx];
        OffsetRect(&local, -ov->rc.left, -ov->rc.top);   /* into overlay-local space */
        ClipRect(&local, &full);
        if (local.right > local.left && local.bottom > local.top) {
            *on = 1;
            *x0 = local.left; *y0 = local.top; *x1 = local.right; *y1 = local.bottom;
        }
    }
}

static void BuildPaper(OVL *ov) {
    BuildPaperInto((unsigned char *)ov->bits, ov->w, ov->h, &g_s);
}

/* mirror one setting into one checkbox; no-op when there is no live control */
static void SetChk(HWND ctl, int on) {
    if (ctl) SendMessageW(ctl, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

/* Zero a rect's alpha in place, RGB left alone: one implementation for every
 * path that presents a monitor's texture, so paper and e-ink cannot drift
 * apart on the row/stride geometry. ULW blends by alpha, so the RGB under a
 * hole is never seen and needs no work. Callers that must not leave the master
 * texture mutated use ClearHoleAlpha/RestoreHoleAlpha below, which save the
 * rect first and punch through this. */
static void PunchHoleAlpha(OVL *ov, int on, int x0, int y0, int x1, int y1) {
    int y;
    if (!ov->bits || !on) return;
    for (y = y0; y < y1; y++)
        memset((unsigned char *)ov->bits + (size_t)y * ov->w * 4 + (size_t)x0 * 4, 0,
               (size_t)(x1 - x0) * 4);
}

/* Save the hole rect's pixels, zero their alpha, and hand the saved block back
 * so the caller can restore it after the upload.
 *
 * The ALPHA is per-pixel noise (PaperPixel derives it from grain, fibre and
 * blotch), so it cannot be guessed back - it has to be saved. That is what
 * makes the restore load-bearing, not a simplification: Housekeeping re-uploads
 * when the hole moves or closes with no rebuild behind it, and a texture left
 * punched would keep a transparent band after the taskbar parked. Forcing a
 * constant alpha back would instead flatten the paper's own transparency. The
 * save is only ever the hole rect (a taskbar strip), and RestoreHoleAlpha
 * frees it. */
static void ClearHoleAlpha(OVL *ov, unsigned char **saved, size_t *saved_len, int *rx0, int *ry0, int *rx1, int *ry1) {
    int on, x0, y0, x1, y1, y;
    size_t rowbytes;
    *saved = NULL;
    *saved_len = 0;
    *rx0 = *ry0 = *rx1 = *ry1 = 0;
    if (!ov->bits) return;
    LocalHole(ov, &on, &x0, &y0, &x1, &y1);
    if (!on) return;
    *rx0 = x0; *ry0 = y0; *rx1 = x1; *ry1 = y1;
    rowbytes = (size_t)(x1 - x0) * 4;
    *saved = (unsigned char *)malloc(rowbytes * (size_t)(y1 - y0));
    if (!*saved) return;                 /* cannot save: leave the texture alone */
    *saved_len = rowbytes * (size_t)(y1 - y0);
    for (y = y0; y < y1; y++)
        memcpy(*saved + rowbytes * (size_t)(y - y0),
               (unsigned char *)ov->bits + (size_t)y * ov->w * 4 + (size_t)x0 * 4,
               rowbytes);
    /* Punch the same rect that was just saved, never a re-read of the live
     * hole: a punch wider than the save would zero alpha on pixels nothing
     * restores, leaving a permanent transparent band. */
    PunchHoleAlpha(ov, on, x0, y0, x1, y1);
}

/* Put the saved pixels back where they came from. Called after the upload, on
 * every path, so the master texture in ov->bits is never left mutated. */
static void RestoreHoleAlpha(OVL *ov, unsigned char *saved, size_t saved_len, int x0, int y0, int x1, int y1) {
    int y;
    size_t rowbytes;
    if (!saved || !ov->bits) return;
    /* Restore by the rect that was actually saved, never by re-reading the
     * live hole: the live state may already differ, but the pixels must
     * come back regardless or the punched alpha stays permanent (that was
     * the 2.7.7 regression). */
    rowbytes = (size_t)(x1 - x0) * 4;
    if (rowbytes * (size_t)(y1 - y0) != saved_len) { free(saved); return; }
    for (y = y0; y < y1; y++) {
        unsigned char *row = (unsigned char *)ov->bits + (size_t)y * ov->w * 4 + (size_t)x0 * 4;
        memcpy(row, saved + rowbytes * (size_t)(y - y0), rowbytes);
    }
    free(saved);
}

/* A monitor can appear that has no baked texture yet. Fill it with a cheap
 * preview here - this runs on the UI thread inside RepaintAll - and let the
 * worker replace it with the exact texture straight after. The old code ran
 * BuildPaperFull directly: a synchronous per-pixel noise build for the whole
 * monitor, every time the effect was switched on, the mode changed or the
 * display changed. That is the freeze the worker exists to prevent, so the
 * show path must never do it.
 *
 * The preview must also be UNCANCELLABLE here, not just cheap: RepaintAll
 * parks g_stop_thread at 1 before showing the strips (its first act is
 * PaperWorkerStop, and the flag only clears when RequestPaper restarts the
 * worker at the end). A stop-flag check on the UI thread would cancel every
 * preview mid-flight, the allocation-failure fallback would fire every
 * single time, and the whole-monitor build would run right back on the UI
 * thread - found by the 2026-10-03 review, present since 2.7.7. Passing
 * epoch = NULL is the same "must complete" contract BuildPaperInto has. */
static void SeedPaperPreview(OVL *ov) {
    if (!ov->bits || ov->w <= 0 || ov->h <= 0) return;
    if (!BuildPaperPreview((unsigned char *)ov->bits, ov->w, ov->h, &g_s, NULL, 0))
        BuildPaperInto((unsigned char *)ov->bits, ov->w, ov->h, &g_s);
}

static int ApplyLayered(OVL *ov) {
    HDC screen = GetDC(NULL);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    unsigned char *saved = NULL;
    size_t saved_len = 0;
    int s, ok = 1;
    int rx0=0, ry0=0, rx1=0, ry1=0;
    /* Mask the taskbar hole out of this upload, then put the master texture
     * back exactly as it was. Housekeeping re-uploads when the hole closes or
     * moves WITHOUT rebuilding, so ov->bits must not be left mutated - that is
     * the regression a no-restore version caused (a permanent transparent band
     * across the bottom of the screen once the taskbar parked). */
    if (g_s.mode == MODE_PAPER)
        ClearHoleAlpha(ov, &saved, &saved_len, &rx0, &ry0, &rx1, &ry1);
    for (s = 0; s < ov->n_strips; s++) {
        SIZE size = { ov->w, min(STRIP_H, ov->h - s * STRIP_H) };
        POINT src = { 0, s * STRIP_H };
        if (!UpdateLayeredWindow(ov->shwnd[s], screen, NULL, &size, ov->mem,
                                 &src, 0, &bf, ULW_ALPHA)) {
            L("layered upload failed: monitor=%d strip=%d error=%lu", ov->idx, s, GetLastError());
            ok = 0;
        }
    }
    RestoreHoleAlpha(ov, saved, saved_len, rx0, ry0, rx1, ry1);
    ReleaseDC(NULL, screen);
    return ok;
}

static int PresentEinkRect(OVL *ov);
static void EinkFreeBuffers(void);   /* RepaintAll releases the ring on mode exit */

static LRESULT CALLBACK OvlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void DestroyOverlay(OVL *ov) {
    int s;
    for (s = 0; s < MAX_STRIPS; s++) {
        if (ov->shwnd[s]) DestroyWindow(ov->shwnd[s]);
        ov->shwnd[s] = NULL;
    }
    ov->hwnd = NULL; ov->mon = NULL;
    if (ov->dib) DeleteObject(ov->dib);
    if (ov->mem) DeleteDC(ov->mem);
    ov->dib = NULL; ov->mem = NULL; ov->bits = NULL;
}

static int MakeOverlay(OVL *ov, HMONITOR mon, const RECT *rc) {
    BITMAPINFO bi;
    HDC screen;
    HINSTANCE hi = GetModuleHandleW(NULL);

    memset(ov, 0, sizeof(*ov));
    ov->mon = mon;
    ov->rc = *rc;
    ov->w = rc->right - rc->left;
    ov->h = rc->bottom - rc->top;
    /* One strip per STRIP_H rows, full width: no window is ever both
     * full-width AND tall, so the shell never mistakes the veil for a
     * fullscreen app and the auto-hide taskbar keeps raising (the whole
     * point of the strip design). */
    for (ov->n_strips = 0; ov->n_strips < MAX_STRIPS; ov->n_strips++) {
        int sy = ov->n_strips * STRIP_H;
        int sh = ov->h - sy;
        if (sh <= 0) break;
        if (sh > STRIP_H) sh = STRIP_H;
        ov->shwnd[ov->n_strips] = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            L"MnPaperOverlay", L"mnPaper", WS_POPUP,
            rc->left, rc->top + sy, ov->w, sh, NULL, NULL, hi, NULL);
        if (!ov->shwnd[ov->n_strips]) {
            int k;
            for (k = 0; k < ov->n_strips; k++) { DestroyWindow(ov->shwnd[k]); ov->shwnd[k] = NULL; }
            ov->n_strips = 0;
            return 0;
        }
    }
    /* The loop above stops at MAX_STRIPS. If the monitor is still taller than
     * the strips cover, say so rather than silently leaving a bare band: this
     * is the geometry a future panel could reach (see MAX_STRIPS). */
    if (ov->n_strips * STRIP_H < ov->h)
        L("overlay: monitor at %ld,%ld is %dpx tall but %d strips cover only %dpx",
          ov->rc.left, ov->rc.top, ov->h, ov->n_strips, ov->n_strips * STRIP_H);
    ov->hwnd = ov->shwnd[0];
    ApplyCaptureState(ov);

    screen = GetDC(NULL);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = ov->w;
    bi.bmiHeader.biHeight = -ov->h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    ov->dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &ov->bits, NULL, 0);
    ov->mem = CreateCompatibleDC(screen);
    SelectObject(ov->mem, ov->dib);
    ReleaseDC(NULL, screen);
    /* The strip failure above tears down and reports; a failed DIB must too,
     * or the monitor still counts as covered and the first texture write goes
     * through a NULL ov->bits (ApplyPaperResult gates on hwnd, not bits) - an
     * access violation on the UI thread instead of one untextured monitor. */
    if (!ov->dib || !ov->mem || !ov->bits) {
        int k;
        if (ov->dib) DeleteObject(ov->dib);
        if (ov->mem) DeleteDC(ov->mem);
        ov->dib = NULL; ov->mem = NULL; ov->bits = NULL;
        for (k = 0; k < ov->n_strips; k++) { DestroyWindow(ov->shwnd[k]); ov->shwnd[k] = NULL; }
        ov->n_strips = 0;
        ov->hwnd = NULL;
        L("overlay: monitor at %ld,%ld %dx%d has no DIB - monitor left untextured",
          ov->rc.left, ov->rc.top, ov->w, ov->h);
        return 0;
    }
    return 1;
}

static void RequestEinkRender(void);

static void ShowOverlay(OVL *ov, int show) {
    int s;
    if (!ov->hwnd) return;
    if (show) {
        /* the hidden regression suites must never put strips on the working
         * desktop, and until 2026-10-03 nothing in the code stopped them:
         * the gate lived only in conventions per test file. The hide branch
         * below stays reachable so tests can still verify teardown. */
        if (g_test_headless) return;
        if (g_s.mode == MODE_PAPER) {
            SeedPaperPreview(ov);   /* cheap; the worker bakes the exact one */
            ApplyLayered(ov);
        } else if (PresentEinkRect(ov)) {
            /* Upload only when a real frame was copied. At a paper->e-ink
             * switch there may be none yet (no published frame, or none
             * publishable), and uploading ov->bits then would put the paper
             * texture on screen inside e-ink mode. */
            ApplyLayered(ov);
        } else if (ov->bits) {
            /* Nothing presentable yet. The strips are already visible from
             * the previous mode, so simply skipping the upload would leave
             * the paper texture on screen until the first e-ink frame lands.
             * Blank the overlay to fully transparent (premultiplied BGRA
             * zero) and upload once; the next published frame repaints it. */
            memset(ov->bits, 0, (size_t)ov->w * ov->h * 4);
            ApplyLayered(ov);
        }
        for (s = 0; s < ov->n_strips; s++)
            ShowWindow(ov->shwnd[s], SW_SHOWNA);
    } else {
        for (s = 0; s < ov->n_strips; s++)
            ShowWindow(ov->shwnd[s], SW_HIDE);
    }
}

static BOOL CALLBACK MonRectCb(HMONITOR hm, HDC hdc, LPRECT rc, LPARAM lp) {
    if (g_nmi >= MAX_MON) {
        L("monitors: stopped at MAX_MON=%d - a further display would get no veil", MAX_MON);
        return FALSE;
    }
    g_mi[g_nmi].mon = hm;
    g_mi[g_nmi].rc = *rc;
    g_nmi++;
    return TRUE;
}

static void SyncOverlays(void) {
    OVL tmp[MAX_MON] = { 0 };   /* the tail slots are memcpy'd too */
    int i, j, n = 0;

    g_nmi = 0;
    EnumDisplayMonitors(NULL, NULL, MonRectCb, 0);

    /* keep windows whose monitor still exists at the same geometry, drop the
     * rest: HMONITOR handles can be reused across a resolution change, and a
     * surviving overlay with a stale rect would seed the veil at the old
     * size (2026-10-05 review) */
    for (i = 0; i < g_n; i++) {
        int alive = 0;
        for (j = 0; j < g_nmi; j++)
            if (g_mi[j].mon == g_ov[i].mon && EqualRect(&g_mi[j].rc, &g_ov[i].rc)) { alive = 1; break; }
        if (alive && n < MAX_MON)
            tmp[n++] = g_ov[i];
        else
            DestroyOverlay(&g_ov[i]);
    }
    /* add monitors that have no window yet */
    for (j = 0; j < g_nmi; j++) {
        int have = 0;
        for (i = 0; i < n; i++)
            if (tmp[i].mon == g_mi[j].mon) { have = 1; break; }
        if (!have && n < MAX_MON) {
            RECT rc = g_mi[j].rc;
            if (MakeOverlay(&tmp[n], g_mi[j].mon, &rc))
                n++;
        }
    }
    for (i = 0; i < n; i++) tmp[i].idx = i;
    memcpy(g_ov, tmp, sizeof(tmp));
    g_n = n;
    L("monitors=%d", g_n);
}


/* ------------------------------------------------ overlay housekeeping ---
 * The overlay is topmost and covers the whole screen, but some apps
 * (Live2D mascots, computer-use overlays) re-assert WS_EX_TOPMOST
 * themselves and end up ABOVE the veil. A periodic
 * SetWindowPos(HWND_TOPMOST) re-claims the top of the topmost band; when
 * the order is already correct the call is a no-op. The whole screen is
 * supposed to stay textured, so the veil never withdraws - not even where the
 * taskbar reveals. */
/* One taskbar slot per monitor, not per screenful: Windows shows a taskbar
 * on every display when that option is on, so this has to track MAX_MON or a
 * 5-monitor desk silently drops taskbars 5..N - no hole punched, no add-only
 * raise, and the veil paints straight over them with nothing logged
 * (2026-10-08 round-10 review). */
#define TB_MAX MAX_MON
static RECT g_tb[TB_MAX];
static HWND g_tbw[TB_MAX];
static int  g_ntb;
static int  g_tb_state[TB_MAX];   /* 0 unknown, 1 parked, 2 revealed */

/* Pills-only hole: on Windows 11 the taskbar WINDOW is a full-width strip
 * while it only DRAWS three floating surfaces (Start button, icon pill,
 * tray pill). Holing the full window rect wipes the paper off the whole
 * bottom band and makes the empty gaps read as a dead strip. Union the
 * visible child surfaces instead - verified live: the children carry real
 * rects (Start, ReBarWindow32 icon area, TrayNotifyWnd tray pill) - and
 * hole only that union (+6px margin), clipped to the band. If the children
 * do not add up (older shells, future changes), fall back to the full
 * band: holing more than needed is safe, holing less is not. */
typedef struct { const WCHAR *want; RECT u; int have; } PillScan;

/* grow *into to the union with *r (PillEnumProc and RefinePillHole used to
 * hand-write this min/max chain twice; 2026-10-04 review) */
static void GrowRect(RECT *into, const RECT *r) {
    if (r->left   < into->left)   into->left   = r->left;
    if (r->top    < into->top)    into->top    = r->top;
    if (r->right  > into->right)  into->right  = r->right;
    if (r->bottom > into->bottom) into->bottom = r->bottom;
}

/* shrink *r to the part of it that lies inside *to. The union direction got
 * GrowRect above; this is its sibling, and four sites were hand-writing it
 * (2026-10-08 round-10 review): LocalHole, RefinePillHole, ComputeHoles and
 * DxgiPoll's on-slot bound. An empty result is left as an inverted rect for
 * the caller to test, never silently clamped to something non-empty. */
static void ClipRect(RECT *r, const RECT *to) {
    if (r->left   < to->left)   r->left   = to->left;
    if (r->top    < to->top)    r->top    = to->top;
    if (r->right  > to->right)  r->right  = to->right;
    if (r->bottom > to->bottom) r->bottom = to->bottom;
}

static BOOL CALLBACK PillEnumProc(HWND hwnd, LPARAM lp) {
    PillScan *s = (PillScan *)lp;
    WCHAR cls[64];
    RECT r;
    GetClassNameW(hwnd, cls, 64);
    if (lstrcmpW(cls, s->want) != 0) return TRUE;
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    if (r.right <= r.left || r.bottom <= r.top) return TRUE;
    if (!s->have) { s->u = r; s->have = 1; }
    else GrowRect(&s->u, &r);
    return TRUE;
}

static void RefinePillHole(HWND taskbar, RECT *hole) {
    static const WCHAR *const pills[] = { L"Start", L"ReBarWindow32", L"TrayNotifyWnd" };
    RECT u;
    int have = 0, k;
    const LONG m = 6;
    u.left = u.right = u.top = u.bottom = 0;
    for (k = 0; k < 3; k++) {
        PillScan ps;
        ps.want = pills[k];
        ps.u.left = ps.u.right = ps.u.top = ps.u.bottom = 0;
        ps.have = 0;
        EnumChildWindows(taskbar, PillEnumProc, (LPARAM)&ps);
        if (!ps.have) continue;
        if (!have) { u = ps.u; have = 1; }
        else GrowRect(&u, &ps.u);
    }
    if (!have) return;                       /* keep the full band */
    u.left -= m; u.top -= m; u.right += m; u.bottom += m;
    ClipRect(&u, hole);                        /* the margin may reach outside */
    if (u.right > u.left && u.bottom > u.top) *hole = u;
}

static int IsTaskbarWnd(HWND h);
static void EinkPresent(void);   /* e-ink arm of the hole-dirty refresh below */

static BOOL CALLBACK TbEnumProc(HWND hwnd, LPARAM lp) {
    RECT r;
    if (!IsWindowVisible(hwnd)) return TRUE;
    /* IsTaskbarWnd also matches SecondaryTrayWnd: taskbars shown on secondary
     * displays (Windows 10 all-displays mode, Windows 11 secondary trays) used
     * to be skipped here, so the veil painted straight over them. */
    if (!IsTaskbarWnd(hwnd)) return TRUE;
    /* The cap is tested only once a real taskbar wants the slot. It used to be
     * the callback's first statement, so on a desk whose table was exactly full
     * the first window enumerated AFTER the fill - any window, taskbar or not -
     * stopped the walk and logged "a further taskbar gets no hole", a claim
     * about a taskbar that was never refused (gate round-10 review; same class
     * as the DXGI caps, which probe past the bound before logging a drop). */
    if (g_ntb >= TB_MAX) {
        L("taskbars: stopped at TB_MAX=%d - a further taskbar gets no hole and stays textured over", TB_MAX);
        return FALSE;
    }
    if (!GetWindowRect(hwnd, &r)) return TRUE;
    if (r.right - r.left <= 0 || r.bottom - r.top <= 0) return TRUE;
    g_tb[g_ntb] = r;
    RefinePillHole(hwnd, &g_tb[g_ntb]);   /* hole the pills, keep the paper on the gaps */
    g_tbw[g_ntb] = hwnd;
    g_ntb++;
    return TRUE;
}

static int IsTaskbarWnd(HWND h) {
    WCHAR cls[64];
    if (!h) return 0;
    GetClassNameW(h, cls, 64);
    return lstrcmpiW(cls, L"Shell_TrayWnd") == 0 ||
           lstrcmpiW(cls, L"SecondaryTrayWnd") == 0;
}

static void CollectTaskbars(void) {
    g_ntb = 0;
    /* state slots at indices >= g_ntb must read as "unknown" after this:
     * Housekeeping's tb_up latch scans the whole array, and a slot left at 2
     * by a since-disappeared taskbar (unplugged while revealed) would pin
     * the latch forever and stop the z-order re-claim walk (2026-10-04
     * review) */
    memset(g_tb_state, 0, sizeof g_tb_state);
    EnumWindows(TbEnumProc, 0);
}

static HWND g_dlg;   /* settings window (LiveDlg below and the UI block both use it) */

/* Is the settings window still alive? It can be closed - or destroyed by a
 * completed self-update - while an update worker is still running, so every
 * call site that needs an owner handle has to ask rather than assume. The rule
 * lives here once instead of as a ternary repeated across the file, because the
 * update path is exactly where a stale handle would do damage. */
static HWND LiveDlg(void) {
    return (g_dlg && IsWindow(g_dlg)) ? g_dlg : NULL;
}
/* Hover tooltips RETIRED 2026-09-30 (user feedback: "the information window
 * is bad"): hover popups replaced by a ? button that opens a help window only
 * when pressed. The comctl32 tooltip had crashed; the own tip popup worked
 * but popped unprompted while dragging. */

/* Is this taskbar band a parked sliver? True for the thin reveal strip of an
 * auto-hidden taskbar, which is short in the direction it hides along: a
 * bottom-docked one leaves a short band, a left- or right-docked one leaves a
 * full-height strip that is a few pixels WIDE. Testing only the height misses
 * the side docks entirely, which classifies a parked side taskbar as revealed:
 * a permanent see-through band down that edge, a pointless topmost raise on
 * every tick, and tb_up latched so the z-order re-claim walk never runs again.
 * The smaller extent is the one the taskbar hides along, so test that. */
static int TbParked(const RECT *on) {
    return min(on->bottom - on->top, on->right - on->left) <= 16;
}

static void ComputeHoles(void) {
    int i, j;
    RECT newh[MAX_MON];
    int  newon[MAX_MON];

    memset(newh, 0, sizeof(newh));
    for (i = 0; i < g_n; i++) newon[i] = 0;
    for (j = 0; j < g_ntb; j++) {
        int mon = -1;
        RECT on;
        for (i = 0; i < g_n; i++)
            if (g_tb[j].left < g_ov[i].rc.right && g_tb[j].right > g_ov[i].rc.left &&
                g_tb[j].top < g_ov[i].rc.bottom && g_tb[j].bottom > g_ov[i].rc.top) {
                mon = i; break;
            }
        if (mon < 0) continue;
        on = g_tb[j];
        ClipRect(&on, &g_ov[mon].rc);            /* the band, inside this monitor */
        if (TbParked(&on)) {                     /* parked sliver          */
            g_tb_state[j] = 1;                    /* unlatch: armed again   */
            continue;
        }
        g_tb_state[j] = 2;
        /* The shell's auto-hide manager is supposed to set WS_EX_TOPMOST on
         * the taskbar when it reveals, but with a topmost full-screen veil
         * present it never does (verified: app stopped -> task reveals at
         * rank 8 topmost=1; app running -> topmost never set, rank 230+,
         * the taskbar sinks behind every window). Re-assert the bit for it,
         * ADD-ONLY: never demote. Demoting is what the first guard attempt
         * did, and a SetWindowPos with a non-topmost insert-after clears
         * WS_EX_TOPMOST, which ping-ponged with the shell and made the
         * sinking intermittent.
         *
         * Raise UNCONDITIONALLY while revealed, not only when the bit is
         * missing: the shell never clears WS_EX_TOPMOST on park, so after
         * the first cycle the bit is permanently set and a bit-gated raise
         * never fires again - the taskbar then re-reveals at its parked
         * rank, BEHIND our topmost strips (sank a few
         * minutes after launch). SetWindowPos(HWND_TOPMOST) is idempotent
         * and lifts the window to the top of the topmost band; repeated
         * raises while revealed are harmless and we still never demote. */
        if (g_tbw[j])
            SetWindowPos(g_tbw[j], HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        if (newon[mon]) UnionRect(&newh[mon], &newh[mon], &on);
        else { newh[mon] = on; newon[mon] = 1; }
    }
    for (i = 0; i < g_n; i++) {
        int changed = newon[i] != g_hole_on[i];
        if (!changed && newon[i] &&
            (newh[i].left != g_hole[i].left || newh[i].top != g_hole[i].top ||
             newh[i].right != g_hole[i].right || newh[i].bottom != g_hole[i].bottom))
            changed = 1;
        if (changed) {
            g_hole[i] = newh[i];
            g_hole_on[i] = newon[i];
            g_hole_dirty = 1;
            L("taskbar hole %d %s rect=(%ld,%ld)-(%ld,%ld) on-screen=%ld",
              i, newon[i] ? "ON" : "off",
              newh[i].left, newh[i].top, newh[i].right, newh[i].bottom,
              newon[i] ? newh[i].bottom - newh[i].top : 0);
        }
    }
}

static void RequestPaper(void);

static void Housekeeping(void) {
    int i, j, walked;
    static int logged;
    HWND h;
    DWORD mypid = GetCurrentProcessId();
    RECT vs;

    /* Walk the top of the band. Re-assert only when a foreign, visible,
       on-screen window (>= 8x8) sits above one of our overlays: mascots
       and computer-use overlays keep pushing themselves up. Skipping our
       own windows keeps the settings dialog above the veil; skipping the
       0x0 / 1x1 IME and DWM helper windows avoids pointless z-order churn. */
    vs.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vs.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vs.right  = vs.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vs.bottom = vs.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    /* While a taskbar is revealed, do not fight foreign windows for the top
       of the band: the shell's auto-hide manager sets WS_EX_TOPMOST on the
       taskbar when it reveals, and a veil re-assert above it within the
       same second makes the shell drop the bit again (observed: taskbar
       stuck at rank ~330, topmost=0, hidden behind every window). The cost
       is a possibly untextured mascot while the taskbar is up. */
    {
        int tb_up = 0;
        for (j = 0; j < g_ntb; j++)      /* only slots this cycle produced */
            if (g_tb_state[j] == 2) { tb_up = 1; break; }
        if (!tb_up) {
            h = GetTopWindow(NULL);
            walked = 0;
            while (h && walked < 24) {     /* cap the walk; ours sits near the top */
                int ours = 0;
                walked++;
                RECT r;
                DWORD pid = 0;
                if (g_ov[0].hwnd == h) break;             /* reached the top overlay */
                for (i = 0; i < g_n; i++) {
                    int s;
                    if (g_ov[i].hwnd == h) { ours = 1; break; }
                    for (s = 0; s < g_ov[i].n_strips; s++)
                        if (g_ov[i].shwnd[s] == h) { ours = 1; break; }
                    if (ours) break;
                }
                if (ours) { h = GetWindow(h, GW_HWNDNEXT); continue; }
                GetWindowThreadProcessId(h, &pid);
                if (pid == mypid) { h = GetWindow(h, GW_HWNDNEXT); continue; }
                if (!IsWindowVisible(h)) { h = GetWindow(h, GW_HWNDNEXT); continue; }
                if (!GetWindowRect(h, &r)) { h = GetWindow(h, GW_HWNDNEXT); continue; }
                if (r.right - r.left < 8 || r.bottom - r.top < 8) {
                    h = GetWindow(h, GW_HWNDNEXT); continue;
                }
                if (r.right <= vs.left || r.left >= vs.right ||
                    r.bottom <= vs.top || r.top >= vs.bottom) {
                    h = GetWindow(h, GW_HWNDNEXT); continue;
                }
                /* a real foreign window is above the veil: take the top back */
                for (i = 0; i < g_n; i++) RaiseOverlay(&g_ov[i]);
                break;
            }
        }
    }

    /* No z-order fight with the taskbar any more: the veil simply stops
       painting over the revealed taskbar (see g_hole). The walk above
       skips taskbars entirely. */
    CollectTaskbars();
    ComputeHoles();
    if (g_hole_dirty) {
        g_hole_dirty = 0;
        if (g_s.master && g_s.mode == MODE_PAPER)
            for (i = 0; i < g_n; i++) ApplyLayered(&g_ov[i]);
        else if (g_s.master && g_s.mode == MODE_EINK)
            /* the punch lives in ov->bits and a static screen publishes no
             * frames, so without this the parked taskbar leaves the last
             * published frame's transparent band on screen indefinitely
             * (2026-10-05 review) */
            EinkPresent();
    }
    /* the settings window floats above the veil (the veil used to swallow
       it, which is what made the sliders feel laggy and unreadable). */
    {
        HWND d = LiveDlg();   /* the veil floats the settings window above itself */
        if (d) SetWindowPos(d, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }

    if (!logged) {
        logged = 1;
        L("taskbars visible: %d", g_ntb);
        for (j = 0; j < g_ntb; j++)
            L("  tray %d: %ld,%ld %ldx%ld", j, g_tb[j].left, g_tb[j].top,
              g_tb[j].right - g_tb[j].left, g_tb[j].bottom - g_tb[j].top);
    }
}

/* ------------------------------------- latest-value paper worker ---
 * UI publishes immutable snapshots under a lock; at most one pending job
 * per monitor is retained. A busy worker never loses the final request.
 * Preview completes without cancellation so dense mouse input cannot
 * starve it. Full refinement cancels by row when a newer request arrives. */
#define WM_APP_PAPER (WM_APP + 3)

typedef struct {
    int idx, w, h, preview, pending;
    unsigned gen;
    LONG epoch;
    DWORD queued;
    SETTINGS s;
} PAPERJOB;

typedef struct PaperDone {
    int idx, w, h, preview;
    unsigned gen;
    DWORD queued, build_ms;
    unsigned char *px;
} PaperDone;

static PAPERJOB g_pjob[MAX_MON];
static SRWLOCK g_plock = SRWLOCK_INIT;
static unsigned g_pgen, g_platest[MAX_MON], g_papplied[MAX_MON];
static HANDLE g_worker, g_pwake;

static void FreePaperDone(PaperDone *d) {
    free(d->px);
    free(d);
}

static DWORD WINAPI PaperWorker(LPVOID unused) {
    int next = 0;
    for (;;) {
        PAPERJOB job;
        PaperDone *d;
        DWORD began;
        int i, found = 0, built;
        if (InterlockedCompareExchange(&g_stop_thread, 0, 0)) break;
        AcquireSRWLockExclusive(&g_plock);
        for (i = 0; i < MAX_MON; i++) {
            int idx = (next + i) % MAX_MON;
            if (g_pjob[idx].pending) {
                job = g_pjob[idx];
                g_pjob[idx].pending = 0;
                next = (idx + 1) % MAX_MON;
                found = 1;
                break;
            }
        }
        ReleaseSRWLockExclusive(&g_plock);
        if (!found) { WaitForSingleObject(g_pwake, INFINITE); continue; }
        d = (PaperDone *)calloc(1, sizeof(*d));
        if (!d) continue;
        d->px = (unsigned char *)malloc((size_t)job.w * job.h * 4);
        if (!d->px) { free(d); continue; }
        began = GetTickCount();
        built = job.preview ? BuildPaperPreview(d->px, job.w, job.h, &job.s,
                                                &g_pepoch[job.idx], job.epoch)
                            : BuildPaperFull(d->px, job.w, job.h, &job.s,
                                             &g_pepoch[job.idx], job.epoch);
        if (!built || InterlockedCompareExchange(&g_stop_thread, 0, 0)) {
            FreePaperDone(d);
            continue;
        }
        d->idx = job.idx; d->w = job.w; d->h = job.h;
        d->gen = job.gen; d->preview = job.preview;
        d->queued = job.queued; d->build_ms = GetTickCount() - began;
        if (!PostMessageW(g_host, WM_APP_PAPER, 0, (LPARAM)d)) FreePaperDone(d);
    }
    return 0;
}

static int PaperWorkerStart(void) {
    if (g_worker) return 1;
    InterlockedExchange(&g_stop_thread, 0);
    g_pwake = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!g_pwake) return 0;
    g_worker = CreateThread(NULL, 0, PaperWorker, NULL, 0, NULL);
    if (!g_worker) { CloseHandle(g_pwake); g_pwake = NULL; return 0; }
    return 1;  /* Normal priority: measured rendering cost, not starvation. */
}

static void PaperWorkerStop(void) {
    int i;
    MSG msg;
    InterlockedExchange(&g_stop_thread, 1);
    if (g_worker) {
        SetEvent(g_pwake);
        /* Row cancellation keeps this short. Never free a live worker's
         * buffers on a timed-out wait (the old 1s timeout was unsafe). */
        WaitForSingleObject(g_worker, INFINITE);
        CloseHandle(g_worker); CloseHandle(g_pwake);
        g_worker = g_pwake = NULL;
    }
    AcquireSRWLockExclusive(&g_plock);
    memset(g_pjob, 0, sizeof(g_pjob));
    ReleaseSRWLockExclusive(&g_plock);
    g_pgen++;
    for (i = 0; i < MAX_MON; i++) {
        InterlockedIncrement(&g_pepoch[i]);
        g_platest[i] = g_papplied[i] = g_pgen;
    }
    if (g_host)
        while (PeekMessageW(&msg, g_host, WM_APP_PAPER, WM_APP_PAPER, PM_REMOVE))
            FreePaperDone((PaperDone *)msg.lParam);
}

static void QueuePaper(int preview) {
    int i;
    if (g_s.mode != MODE_PAPER || !g_s.master) return;
    if (!PaperWorkerStart()) { L("paper worker unavailable: %lu", GetLastError()); return; }
    g_pgen++;
    AcquireSRWLockExclusive(&g_plock);
    for (i = 0; i < g_n; i++) {
        PAPERJOB *job = &g_pjob[i];
        if (!g_ov[i].hwnd) continue;
        job->idx = i; job->w = g_ov[i].w; job->h = g_ov[i].h;
        job->s = g_s; job->gen = g_pgen; job->preview = preview;
        job->epoch = InterlockedIncrement(&g_pepoch[i]);
        job->queued = GetTickCount(); job->pending = 1;
        g_platest[i] = g_pgen;
    }
    ReleaseSRWLockExclusive(&g_plock);
    SetEvent(g_pwake);
}

static void RequestPaper(void) { QueuePaper(0); }
static void RequestPaperPreview(void) { QueuePaper(1); }

static void ApplyPaperResult(PaperDone *d) {
    OVL *ov;
    if (d->idx < 0 || d->idx >= g_n) { FreePaperDone(d); return; }
    ov = &g_ov[d->idx];
    if (ov->hwnd && ov->w == d->w && ov->h == d->h &&
        d->gen >= g_papplied[d->idx] &&
        (d->preview || d->gen == g_platest[d->idx]) &&
        g_s.mode == MODE_PAPER && g_s.master) {
        g_papplied[d->idx] = d->gen;
        memcpy(ov->bits, d->px, (size_t)ov->w * ov->h * 4);
        {
            int ok = ApplyLayered(ov);   /* local: it only feeds the line below */
            L("paper %s monitor=%d gen=%u build=%lums request-to-upload=%lums uploaded=%d",
              d->preview ? "preview" : "full", d->idx, d->gen, d->build_ms,
              GetTickCount() - d->queued, ok);
        }
    }
    FreePaperDone(d);
}

/* The e-ink run state is decided here, before the e-ink section defines it. */
static void EinkWorkerSet(int want);
static int  EinkBuffersReady(void);
static void EinkBuffersRestart(void);
static LONG g_eset_gen;               /* bumped whenever settings change */
static LONG g_egen_done;              /* generation the worker has rendered */

/* Ask for a fresh e-ink frame without touching the overlays: bumps the
 * generation so a static screen still re-renders, and makes sure the ring and
 * its worker are up. Used by the settings sliders (whose changes only need a
 * re-render, not a re-present of the current frame) and by RepaintAll. */
static void RequestEinkRender(void) {
    InterlockedIncrement(&g_eset_gen);
    if (!EinkBuffersReady()) EinkBuffersRestart();
    else EinkWorkerSet(1);
}

static void RepaintAll(void) {
    int i;
    PaperWorkerStop();  /* A mode/master/CLI change invalidates old snapshots. */
    if (g_s.master && g_s.mode == MODE_EINK)
        RequestEinkRender();
    else if (g_s.mode != MODE_EINK)
        /* Leaving e-ink: release the ring buffers instead of only stopping the
         * worker. They are up to ~5 virtual-screen bitmaps (~104 MB on a
         * 2880x1800 desktop, ~166 MB at 4K), the GDI grab pair, and the whole
         * DXGI duplication set besides; an eink->paper switch used to pin all
         * of it until exit. EinkFreeBuffers stops the worker itself, and
         * RequestEinkRender re-allocates on demand, so a round trip just pays
         * two mallocs. */
        EinkFreeBuffers();
    else
        /* E-ink with the effect off: stop the worker but KEEP the ring. Toggling
         * the effect between paper and e-ink (Ctrl+Alt+P) must not free and then
         * immediately rebuild ~104 MB just because master changed. */
        EinkWorkerSet(0);
    for (i = 0; i < g_n; i++) {
        ApplyCaptureState(&g_ov[i]);
        ShowOverlay(&g_ov[i], g_s.master);
    }
    /* The show path only lays down a cheap preview (or the current e-ink
     * frame). Follow it with one full-quality request so what is on screen is
     * always the exact texture, without the UI thread ever doing the heavy
     * per-pixel work. Nothing is queued while the effect is off. */
    if (g_s.master && g_s.mode == MODE_PAPER)
        RequestPaper();
}

/* ----------------------------------------------------------------- e-ink --- */

#define MAX_OUT 8
#define MAX_DXGI_ADAPTERS 8   /* the adapter array bound: keep the cap check, the
                               * probe index and the log on this one constant */

typedef struct {
    IDXGIOutput1 *out;      /* retained for the staleness re-check; released in DxgiShutdown */
    IDXGIOutputDuplication *dup;
    RECT r;                 /* rect inside the virtual-screen capture buffer */
    ID3D11Texture2D *stage;
    int stageW, stageH, stageFmt;
    int swapRB;
    int badfmt;             /* output skipped: format unsupported, logged once */
    int rotated;            /* output skipped: not identity, logged once */
} OUTINFO;

static ID3D11Device        *g_dev;
static ID3D11DeviceContext *g_ctx;
static OUTINFO g_out[MAX_OUT];
static int g_nout;
static int g_vsx, g_vsy, g_vsw, g_vsh;
/* -------------------------------------------------- e-ink ring and worker ---
 *
 * The heavy e-ink step - a per-pixel double-precision conversion of the whole
 * virtual screen - used to run on the UI thread inside the 50 ms tick, so
 * dragging a slider or opening a menu stuttered while e-ink was live. The
 * paper path already solved this with a worker thread; e-ink follows the
 * same shape:
 *
 *   UI thread  capture (DXGI duplication, or the GDI fallback) into a free
 *              ring slot, publish it, keep ticking.
 *   worker     convert the newest slot into the build buffer, publish the
 *              result by swapping the show pointer, post a paint message.
 *   UI thread  present: memcpy the show buffer per monitor, upload the strips.
 *
 * Only the memcpy and the layered upload stay on the UI thread - exactly the
 * work the paper path already does there. Slot states: 0 free, 4 filling
 * (being captured), 1 newest (ready), 2 older (droppable when a newer one
 * arrives), 3 retained (what the worker last rendered, kept so a settings
 * change can re-render it when the screen is static). */
#define EINK_SLOTS 3
#define WM_APP_EINK (WM_APP + 8)

static unsigned char *g_ecap[EINK_SLOTS];
static unsigned char *g_eproc_a, *g_eproc_b;
static unsigned char *g_eproc_show;    /* the UI thread presents this one */
static unsigned char *g_ebuild;        /* the worker renders into this one */
static volatile int g_eproc_valid;     /* 0 until the worker published a real
                                        * frame: EinkEnsureBuffers seeds the
                                        * show buffer with 0xFF, and presenting
                                        * that reads as an opaque white screen
                                        * until the first real frame lands
                                        * (2026-10-04 review) */
static LONG  g_ecap_state[EINK_SLOTS];
static HANDLE g_ework, g_ewake;
static volatile LONG g_estop;
static CRITICAL_SECTION g_ecs;
static int   g_ecs_ready;
static int  g_dxgi = 0;        /* 0 uninit, 1 live; the GDI fallback is g_gdi_only */
static int  g_gdi_only = 0;     /* duplication unavailable: BitBlt fallback */
static int  g_dxgi_retry = 1;   /* 0: the reason is permanent (every output
                                 * rotated), so the 2 s retry would only
                                 * re-arm DXGI to fall back again */
static DWORD g_dxgi_fail_at;
static int g_rot_logged;    /* the rotated-output notice is a once-per-process line */
static int g_cap_logged;    /* likewise for the adapter enumeration cap */
static int g_out_cap_logged;/* likewise for the output enumeration cap */
static int g_dupfail_logged;/* likewise: an output duplication that failed */
static int g_devfail_logged; /* likewise: the D3D11 device could not be created */
static int g_noout_logged;   /* likewise: no output could be duplicated */
static int g_acqfail_logged;/* likewise: a per-frame acquire keeps failing */
static int g_badfmt_logged; /* likewise: an unsupported desktop format */
static int g_fallback_logged;/* likewise: the GDI fallback's reason line */
static int g_recover_logged;/* likewise: duplication recovered after GDI */
static int g_init_logged;   /* likewise: the first successful duplication init */
static void GdiResetGrabs(void);   /* defined with the GDI fallback poller */
static const int BAYER4[4][4] = {
    { 0,  8,  2, 10 },
    { 12, 4, 14,  6 },
    { 3, 11,  1,  9 },
    { 15, 7, 13,  5 }
};

/* Release the whole e-ink buffer set and leave every pointer NULL, so no
 * caller can hand a freed buffer to the worker or to a present. Three sites
 * need exactly this sequence - EinkEnsureBuffers' rebuild path, its
 * partial-allocation failure path, and EinkFreeBuffers - and writing it out
 * three times is the same shape that let the 2026-10-07 rotation fix land in
 * one copy of a three-copy sequence (see EinkFallBackToGdi below). */
static void EinkDropBuffers(void) {
    int i;
    for (i = 0; i < EINK_SLOTS; i++) { free(g_ecap[i]); g_ecap[i] = NULL; }
    free(g_eproc_a); free(g_eproc_b);
    g_eproc_a = g_eproc_b = g_eproc_show = g_ebuild = NULL;
}

static int EinkBuffersReady(void);  /* defined with EinkBuffersRestart, below */
static void EinkEnsureBuffers(void) {
    int i;
    size_t n;
    if (EinkBuffersReady()) return;   /* ring present AND desktop rect current */
    g_eproc_valid = 0;      /* rebuilding: nothing publishable until the
                             * worker's next frame lands */
    EinkDropBuffers();
    g_vsx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vsy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vsw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vsh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g_vsw <= 0 || g_vsh <= 0) return;
    n = (size_t)g_vsw * g_vsh * 4;
    for (i = 0; i < EINK_SLOTS; i++) {
        g_ecap[i] = (unsigned char *)malloc(n);
        if (!g_ecap[i]) { while (--i >= 0) { free(g_ecap[i]); g_ecap[i] = NULL; } return; }
        memset(g_ecap[i], 0x80, n);
    }
    g_eproc_a = (unsigned char *)malloc(n);
    g_eproc_b = (unsigned char *)malloc(n);
    if (!g_eproc_a || !g_eproc_b) { EinkDropBuffers(); return; }
    memset(g_eproc_a, 0xFF, n);
    memset(g_eproc_b, 0xFF, n);
    g_eproc_show = g_eproc_a;
    g_ebuild     = g_eproc_b;
    for (i = 0; i < EINK_SLOTS; i++) g_ecap_state[i] = 0;
    L("buffers %dx%d (3 capture slots, 2 processed)", g_vsw, g_vsh);
}

static void DxgiShutdown(void);     /* defined with DxgiInit, below */
static void EinkFreeBuffers(void) {
    EinkWorkerSet(0);           /* never free a live worker's buffers */
    EinkDropBuffers();
    { int i; for (i = 0; i < EINK_SLOTS; i++) g_ecap_state[i] = 0; }
    GdiResetGrabs();   /* the grab DC/DIB are bound to the thread that polls */
    /* Leaving e-ink releases the capture set too: an open duplication and a
     * desktop-sized staging texture per output would otherwise stay pinned
     * for the rest of the session while nothing reads them. DxgiShutdown is
     * idempotent (EinkShutdownAll may have run it already), and re-entering
     * e-ink re-initializes in EinkTick - unless the GDI-only latch holds the
     * capture (every output rotated: g_dxgi_retry stays permanently 0 by
     * decision, so re-entry stays on the GDI grab). */
    DxgiShutdown();
    g_dxgi = 0;
}

static void EinkShutdownCapture(void);

/* Pure function of (src, dst, w, h): no globals read except the settings,
 * so it is safe to run on the worker thread and in the headless dump. */
static void EinkRender(const unsigned char *src, unsigned char *dst, int w, int h) {
    int x, y;
    int levels = g_s.shades;
    double gain = 0.6 + (g_s.contrast / 100.0) * 1.4;
    double ds = g_s.dither / 100.0;
    double scale = (levels > 1) ? 255.0 / (levels - 1) : 0.0;

    if (w <= 0 || h <= 0 || !src || !dst) return;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            const unsigned char *s = src + 4 * ((size_t)y * w + x);
            unsigned char *d = dst + 4 * ((size_t)y * w + x);
            double lum = 0.299 * s[2] + 0.587 * s[1] + 0.114 * s[0];
            double v = (lum - 128.0) * gain + 128.0;
            double off = (BAYER4[y & 3][x & 3] / 16.0 - 0.5) * ds;
            int lvl, out;
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            lvl = (int)floor(v / 255.0 * (levels - 1) + 0.5 + off);
            if (lvl < 0) lvl = 0;
            if (lvl > levels - 1) lvl = levels - 1;
            out = (int)(lvl * scale + 0.5);
            d[0] = d[1] = d[2] = (unsigned char)out;
            d[3] = 255;
        }
    }
}

/* Returns 1 when a frame was actually copied into ov->bits, 0 when there was
 * nothing presentable (no published frame yet, or a geometry that does not fit
 * the capture buffer). The caller must not upload on 0: ov->bits then still
 * holds whatever the other mode left there.
 *
 * The copy runs under g_ecs. The worker's publish swaps g_eproc_show/g_ebuild
 * under the same lock and then renders into the buffer it just handed back, so
 * without it the worker can start overwriting the very pixels being copied -
 * a torn frame. The worker's own critical sections are microseconds long, so
 * holding it for one rect copy costs nothing real. */
static int PresentEinkRect(OVL *ov) {
    const unsigned char *show;
    int x0, y0, y, got = 0;
    if (!ov->bits || !g_ecs_ready) return 0;
    x0 = ov->rc.left - g_vsx; y0 = ov->rc.top - g_vsy;
    EnterCriticalSection(&g_ecs);
    show = g_eproc_show;
    if (show && g_eproc_valid &&
        x0 >= 0 && y0 >= 0 && x0 + ov->w <= g_vsw && y0 + ov->h <= g_vsh) {
        for (y = 0; y < ov->h; y++)
            memcpy((unsigned char *)ov->bits + (size_t)y * ov->w * 4,
                   show + 4 * ((size_t)(y0 + y) * g_vsw + x0),
                   (size_t)ov->w * 4);
        got = 1;
    }
    LeaveCriticalSection(&g_ecs);
    if (!got) return 0;
    /* Same hole rule as the paper path, punched in place: this buffer is
     * re-copied from g_eproc_show at the top of every present, so the pixels
     * under the hole are disposable. Housekeeping re-presents on every hole
     * change, so a parked taskbar cannot leave a stale band in the upload. */
    { int hon, hx0, hy0, hx1, hy1;
      LocalHole(ov, &hon, &hx0, &hy0, &hx1, &hy1);
      PunchHoleAlpha(ov, hon, hx0, hy0, hx1, hy1); }
    return 1;
}

static void DxgiShutdown(void) {
    int i;
    for (i = 0; i < g_nout; i++) {
        if (g_out[i].out)   { g_out[i].out->lpVtbl->Release(g_out[i].out);   g_out[i].out = NULL; }
        if (g_out[i].stage) { g_out[i].stage->lpVtbl->Release(g_out[i].stage); g_out[i].stage = NULL; }
        if (g_out[i].dup)   { g_out[i].dup->lpVtbl->Release(g_out[i].dup);     g_out[i].dup = NULL; }
    }
    g_nout = 0;
    if (g_ctx) { g_ctx->lpVtbl->Release(g_ctx); g_ctx = NULL; }
    if (g_dev) { g_dev->lpVtbl->Release(g_dev); g_dev = NULL; }
}

/* Every shutdown site wants this exact order: stop the worker first,
 * then free what it was using (2026-10-04 review). */
static void EinkShutdownAll(void) {
    EinkShutdownCapture();
    EinkFreeBuffers();
}

static void EinkShutdownCapture(void) {
    DxgiShutdown();
    g_dxgi = 0;
    g_gdi_only = 0;
}

static int DxgiInit(void) {
    IDXGIFactory1 *factory = NULL;
    IDXGIAdapter1 *adapters[MAX_DXGI_ADAPTERS];
    int nadapt = 0, a, o;

    g_dxgi_fail_at = GetTickCount();
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory)) || !factory)
        return 0;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                 D3D11_SDK_VERSION, &g_dev, NULL, &g_ctx)) || !g_dev) {
        if (!g_devfail_logged) { g_devfail_logged = 1; L("d3d11 device failed"); }
        factory->lpVtbl->Release(factory);
        DxgiShutdown();
        return 0;
    }
    for (a = 0; a < MAX_DXGI_ADAPTERS; a++) {
        HRESULT hr = factory->lpVtbl->EnumAdapters1(factory, a, &adapters[a]);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(hr)) break;
        nadapt++;
    }
    if (nadapt == MAX_DXGI_ADAPTERS) {
        /* nadapt == MAX only says that many adapters enumerated; a further
         * one may or may not exist, so probe one past the cap before claiming
         * anything was dropped. Once per process: the 2 s retry would
         * otherwise re-log. */
        IDXGIAdapter1 *extra = NULL;
        HRESULT xhr = factory->lpVtbl->EnumAdapters1(factory, MAX_DXGI_ADAPTERS, &extra);
        if (xhr == S_OK && extra) {
            if (!g_cap_logged) {
                g_cap_logged = 1;
                L("dxgi: stopped at %d adapters - outputs on a further adapter are never duplicated",
                  MAX_DXGI_ADAPTERS);
            }
            extra->lpVtbl->Release(extra);
        }
    }
    for (a = 0; a < nadapt; a++) {
        IDXGIOutput *out = NULL;
        int cap_hit = 0;
        o = 0;
        while (g_nout < MAX_OUT) {
            HRESULT ehr;
            IDXGIOutput1 *out1 = NULL;
            int stored = 0;
            ehr = adapters[a]->lpVtbl->EnumOutputs(adapters[a], o++, &out);
            if (ehr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(ehr) || !out) break;   /* transient failure: skip the
                                               * adapter, never deref NULL */
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(out->lpVtbl->QueryInterface(out, &IID_IDXGIOutput1, (void **)&out1)) && out1) {
                memset(&g_out[g_nout], 0, sizeof(OUTINFO));
                if (SUCCEEDED(out1->lpVtbl->GetDesc(out1, &desc))) {
                    RECT r = desc.DesktopCoordinates;
                    /* Identity is the only orientation the copy loop below can
                     * express: the duplicated surface follows the rotated
                     * output, so for 90/270 its width and height are swapped
                     * against DesktopCoordinates (the 1:1 row copy would come
                     * out transposed) and 180 flips it. Mark the output and let
                     * EinkTick hand the whole desktop to the GDI grab, which
                     * returns the composed desktop in desktop space and needs no
                     * rotation at all. Logged once per process: DxgiInit is
                     * retried every ~2 s when duplication is unavailable, and a
                     * per-attempt line would flush the log on every cycle. */
                    g_out[g_nout].rotated =
                        (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
                         desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED);
                    if (g_out[g_nout].rotated && !g_rot_logged) {
                        g_rot_logged = 1;
                        L("dxgi: output %d is rotated (%d), using the GDI grab for the whole desktop",
                          g_nout, (int)desc.Rotation);
                    }
                    g_out[g_nout].r.left   = r.left   - g_vsx;
                    g_out[g_nout].r.top    = r.top    - g_vsy;
                    g_out[g_nout].r.right  = r.right  - g_vsx;
                    g_out[g_nout].r.bottom = r.bottom - g_vsy;
                    HRESULT dhr = out1->lpVtbl->DuplicateOutput(out1, (IUnknown *)g_dev, &g_out[g_nout].dup);
                    if (SUCCEEDED(dhr) && g_out[g_nout].dup) {
                        g_out[g_nout].out = out1;   /* the interface reference moves into the slot */
                        g_nout++;
                        stored = 1;
                    } else if (!g_dupfail_logged) {
                        /* The device lives on the default adapter, so every
                         * output of a second adapter fails here; those monitors
                         * would silently keep a seed-gray capture while the
                         * count below reads healthy. Cross-adapter duplication
                         * (one device per adapter) is plan-scale; the log is
                         * the fix of its class. */
                        g_dupfail_logged = 1;
                        L("dxgi: an output could not be duplicated (hr 0x%08lx) - its monitor gets no live capture",
                          (unsigned long)dhr);
                    }
                }
                if (!stored)
                    out1->lpVtbl->Release(out1);   /* not retained: dup failed or desc failed */
            }
            out->lpVtbl->Release(out);
            out = NULL;
            if (g_nout >= MAX_OUT) { cap_hit = 1; break; }
        }
        if (cap_hit && !g_out_cap_logged) {
            /* The count reaching MAX_OUT is not proof an output was dropped:
             * probe for a next output on this adapter and on any adapter not
             * reached yet, which is the whole set the cap made unreachable. */
            int k = a;
            IDXGIOutput *extra = NULL;
            while (k < nadapt) {
                int start = (k == a) ? o : 0;
                if (adapters[k]->lpVtbl->EnumOutputs(adapters[k], start, &extra) == S_OK && extra)
                    break;
                extra = NULL;
                k++;
            }
            if (extra) {
                g_out_cap_logged = 1;
                L("dxgi: stopped at MAX_OUT=%d - an output past the cap is never refreshed", MAX_OUT);
                extra->lpVtbl->Release(extra);
            }
        }
        adapters[a]->lpVtbl->Release(adapters[a]);
    }
    factory->lpVtbl->Release(factory);
    if (g_nout == 0) {
        /* the 2 s retry re-runs DxgiInit forever on machines without
         * duplication (RDP, some VMs): say it once, like the other
         * retry-cycle lines in this family */
        if (!g_noout_logged) { g_noout_logged = 1; L("dxgi: no duplication outputs"); }
        DxgiShutdown();
    } else {
        if (!g_init_logged) {
            g_init_logged = 1;
            L("dxgi: %d duplication output(s)", g_nout);
        }
    }
    return g_nout > 0;
}

/* Is any output actually usable by the copy loop? A rotated one is not (any
 * rotation is handled by the GDI handover before polling), and neither is one
 * whose desktop format is unsupported. When every output is unusable the
 * caller must switch to the GDI grab instead of waiting forever for frames
 * that will never arrive. */
static int DxgiHasUsableOutput(void) {
    int i;
    for (i = 0; i < g_nout; i++)
        if (!g_out[i].rotated && !g_out[i].badfmt) return 1;
    return 0;
}

/* Is any output rotated? The DXGI copy loop only expresses an identity
 * orientation (see DxgiInit), so a single rotated output makes the DXGI grab
 * unusable for the WHOLE desktop: that monitor's region would keep whatever a
 * previous frame left in the shared slot while the rest of the screen updates,
 * i.e. permanently stale pixels on that display. The GDI grab returns the
 * composed desktop (rotation already applied by Windows) for every monitor, so
 * one rotated output hands capture over to it. */
static int DxgiAnyRotated(void) {
    int i;
    for (i = 0; i < g_nout; i++)
        if (g_out[i].rotated) return 1;
    return 0;
}

/* Is a later DxgiInit worth trying? Rotation never changes for a display, so
 * a set of rotated-only outputs is permanent; an unsupported desktop format
 * may come back, and a plain init failure may be transient. Called before the
 * outputs are torn down. */
static int DxgiRetryable(void) {
    int i;
    for (i = 0; i < g_nout; i++)
        if (!g_out[i].rotated) return 1;
    return 0;
}

/* returns 1 when at least one output produced a new frame */
static int DxgiPoll(unsigned char *dst) {
    int i, got = 0;
    if (!dst) return 0;
    for (i = 0; i < g_nout; i++) {
        DXGI_OUTDUPL_FRAME_INFO fi;
        IDXGIResource *res = NULL;
        ID3D11Texture2D *tex = NULL;
        D3D11_TEXTURE2D_DESC td;
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr;
        BYTE *srow, *drow;
        long x, y;
        UINT pitch;

        if (g_out[i].rotated) continue;   /* logged once; GDI covers the desktop */

        hr = g_out[i].dup->lpVtbl->AcquireNextFrame(g_out[i].dup, 0, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_ACCESS_DENIED || hr == DXGI_ERROR_INVALID_CALL) {
            L("dxgi: access lost, re-init later");
            if (res) res->lpVtbl->Release(res);
            EinkShutdownCapture();
            return 0;
        }
        if (FAILED(hr)) {
            if (!g_acqfail_logged) {
                g_acqfail_logged = 1;
                L("dxgi: AcquireNextFrame failed 0x%08lx - that monitor stops refreshing",
                  (unsigned long)hr);
            }
            if (res) res->lpVtbl->Release(res);
            continue;
        }

        if (FAILED(res->lpVtbl->QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex)) || !tex) {
            res->lpVtbl->Release(res);
            g_out[i].dup->lpVtbl->ReleaseFrame(g_out[i].dup);
            continue;
        }
        res->lpVtbl->Release(res);
        tex->lpVtbl->GetDesc(tex, &td);

        if (!g_out[i].stage || g_out[i].stageW != (int)td.Width || g_out[i].stageH != (int)td.Height ||
            g_out[i].stageFmt != (int)td.Format) {
            D3D11_TEXTURE2D_DESC sd;
            if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM && td.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
                /* anything else would memcpy channel-swapped garbage; the
                 * GDI fallback only covers a whole-adapter loss, so this
                 * output goes stale - log it once so it is not silent */
                if (!g_out[i].badfmt) {
                    g_out[i].badfmt = 1;
                    /* per-struct: reset by every DxgiInit's memset, so the
                     * retry cycle would re-log - the process latch stops that */
                    if (!g_badfmt_logged) {
                        g_badfmt_logged = 1;
                        L("dxgi: output %d desktop format %d unsupported, output not refreshed",
                          i, (int)td.Format);
                    }
                }
                tex->lpVtbl->Release(tex);
                g_out[i].dup->lpVtbl->ReleaseFrame(g_out[i].dup);
                continue;
            }
            if (g_out[i].stage) { g_out[i].stage->lpVtbl->Release(g_out[i].stage); g_out[i].stage = NULL; }
            memset(&sd, 0, sizeof sd);
            sd.Width = td.Width; sd.Height = td.Height;
            sd.MipLevels = 1; sd.ArraySize = 1;
            sd.Format = td.Format;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(g_dev->lpVtbl->CreateTexture2D(g_dev, &sd, NULL, &g_out[i].stage))) {
                tex->lpVtbl->Release(tex);
                g_out[i].dup->lpVtbl->ReleaseFrame(g_out[i].dup);
                continue;
            }
            g_out[i].stageW = (int)td.Width;
            g_out[i].stageH = (int)td.Height;
            g_out[i].stageFmt = (int)td.Format;
            g_out[i].swapRB = (td.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
        }
        g_ctx->lpVtbl->CopyResource(g_ctx, (ID3D11Resource *)g_out[i].stage, (ID3D11Resource *)tex);
        if (FAILED(g_ctx->lpVtbl->Map(g_ctx, (ID3D11Resource *)g_out[i].stage, 0, D3D11_MAP_READ, 0, &m))) {
            tex->lpVtbl->Release(tex);
            g_out[i].dup->lpVtbl->ReleaseFrame(g_out[i].dup);
            continue;
        }
        srow = (BYTE *)m.pData;
        /* DesktopCoordinates live in unrotated desktop space while the
         * duplicated texture follows the rotated output, so r and
         * td.Width/Height can disagree - and r can fall partly or wholly
         * outside the ring slot, which is sized g_vsw*g_vsh. Copy only the
         * part that lands inside the slot (the same bound PresentEinkRect
         * enforces on the way back out); an empty intersection means this
         * output has no on-slot pixels and is skipped. */
        {
            RECT c = g_out[i].r;
            RECT slot = { 0, 0, g_vsw, g_vsh };   /* the ring slot's extent */
            long cw, ch;
            ClipRect(&c, &slot);
            cw = c.right - c.left;
            ch = c.bottom - c.top;
            if (cw > (long)td.Width) cw = (long)td.Width;
            if (ch > (long)td.Height) ch = (long)td.Height;
            if (cw > 0 && ch > 0) {
                drow = dst + 4 * ((size_t)c.top * g_vsw + c.left);
                pitch = m.RowPitch;
                for (y = 0; y < ch; y++) {
                    BYTE *s = srow + (size_t)y * pitch;
                    BYTE *d = drow + (size_t)y * g_vsw * 4;
                    if (g_out[i].swapRB) {
                        for (x = 0; x < cw; x++) {
                            BYTE *sp = s + 4 * x, *dp = d + 4 * x;
                            dp[0] = sp[2]; dp[1] = sp[1]; dp[2] = sp[0]; dp[3] = 255;
                        }
                    } else {
                        memcpy(d, s, (size_t)cw * 4);
                    }
                }
            }
        }
        g_ctx->lpVtbl->Unmap(g_ctx, (ID3D11Resource *)g_out[i].stage, 0);
        tex->lpVtbl->Release(tex);
        g_out[i].dup->lpVtbl->ReleaseFrame(g_out[i].dup);
        got = 1;
    }
    return got;
}

/* GDI fallback: grab the desktop at tick rate, skip work when nothing
 * moved.
 *
 * Two corrections over the original, both found by the codebase review:
 *
 * 1. The DIB and its DC are created ONCE and kept: this ran at 20 Hz, so a
 *    create/destroy pair per tick meant a GDI alloc + a kernel round-trip
 *    every 50 ms for the lifetime of e-ink mode. The objects are bound to the
 *    UI thread that creates them, which is the only thread that calls this.
 *
 * 2. Change detection reads EVERY pixel. The sampled hash (one pixel in 97)
 *    could not see the other 96/97ths of the screen: a text edit landing
 *    between sample points never updated the e-ink view until an unrelated
 *    change happened to hit a sampled pixel. memcmp against the previous grab
 *    is exact and runs at memory speed, so the security of the check costs
 *    about as much as the copy it guards. */
static HDC    g_gdi_dc;
static HBITMAP g_gdi_bmp;
static unsigned char *g_gdi_bits;   /* the DIB's pixels, written by BitBlt */
static unsigned char *g_gdi_prev;  /* previous grab, owned by this module */
static int g_gdi_w, g_gdi_h;       /* size the grab set was created for */

static void GdiResetGrabs(void) {
    if (g_gdi_bmp) DeleteObject(g_gdi_bmp);
    if (g_gdi_dc)  DeleteDC(g_gdi_dc);
    free(g_gdi_prev);
    g_gdi_bmp = NULL; g_gdi_dc = NULL; g_gdi_bits = NULL; g_gdi_prev = NULL;
    g_gdi_w = 0; g_gdi_h = 0;
}

static int GdiPoll(unsigned char *dst) {
    HDC screen;
    BITMAPINFO bi;
    size_t n;
    int w = g_vsw, h = g_vsh;

    if (w <= 0 || h <= 0 || !dst) return 0;
    n = (size_t)w * h * 4;
    if (!g_gdi_bmp || !g_gdi_dc || !g_gdi_bits || g_gdi_w != w || g_gdi_h != h) {
        GdiResetGrabs();
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        screen = GetDC(NULL);
        g_gdi_dc = CreateCompatibleDC(screen);
        g_gdi_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&g_gdi_bits, NULL, 0);
        ReleaseDC(NULL, screen);
        if (!g_gdi_bmp || !g_gdi_dc || !g_gdi_bits) { GdiResetGrabs(); return 0; }
        g_gdi_prev = (unsigned char *)malloc(n);
        if (!g_gdi_prev) { GdiResetGrabs(); return 0; }
        SelectObject(g_gdi_dc, g_gdi_bmp);
        g_gdi_w = w; g_gdi_h = h;
        memset(g_gdi_prev, 0, n);   /* force the first comparison to differ */
    }
    screen = GetDC(NULL);
    if (!BitBlt(g_gdi_dc, 0, 0, w, h, screen, g_vsx, g_vsy, SRCCOPY | CAPTUREBLT)) {
        ReleaseDC(NULL, screen);
        return 0;
    }
    ReleaseDC(NULL, screen);
    if (g_gdi_prev && memcmp(g_gdi_bits, g_gdi_prev, n) == 0)
        return 0;                        /* nothing moved: no work */
    memcpy(g_gdi_prev, g_gdi_bits, n);
    memcpy(dst, g_gdi_bits, n);
    return 1;
}

/* Stop or start the conversion worker with the e-ink run state. Buffers are
 * only (re)allocated with the worker stopped, so every call site that can
 * change the buffer set goes through EinkBuffersRestart below. */
static int EinkBuffersReady(void) {
    /* The origin is load-bearing: PresentEinkRect, DxgiInit and GdiPoll all
     * map through g_vsx/g_vsy, so a desktop rearrangement that moved the
     * top-left corner without changing the size would otherwise leave the
     * e-ink desktop showing where the monitors used to be. */
    return g_ecap[0] && g_eproc_show
        && g_vsx == GetSystemMetrics(SM_XVIRTUALSCREEN)
        && g_vsy == GetSystemMetrics(SM_YVIRTUALSCREEN)
        && g_vsw == GetSystemMetrics(SM_CXVIRTUALSCREEN)
        && g_vsh == GetSystemMetrics(SM_CYVIRTUALSCREEN);
}

static DWORD g_ering_retry_at;  /* next allowed e-ink ring allocation attempt */

static int EinkRingBackoff(void) {
    /* plain DWORD compare fails open at wrap (one early retry) - harmless */
    return g_ecap[0] == NULL && GetTickCount() < g_ering_retry_at;
}

static void EinkBuffersRestart(void) {
    static int last_want = -1;
    int had_ring = (g_ecap[0] != NULL);
    int was_ready = EinkBuffersReady();
    int want;
    DWORD now = GetTickCount();
    if (EinkRingBackoff()) return;   /* nothing changed since the last failure */
    EinkWorkerSet(0);
    EinkEnsureBuffers();
    if (g_dxgi && !was_ready)
        /* The ring was just re-anchored to fresh desktop metrics, but
         * DxgiInit mapped every g_out[i].r against the OLD origin: without
         * re-init, a geometry change that never delivered WM_DISPLAYCHANGE
         * leaves DXGI frames landing in the wrong slot regions (the GDI grab
         * re-derives its own origin). Keyed on the rebuild, not on the ring's
         * prior presence: a ring absent while capture is live still leaves
         * stale rects behind when it is finally built. EinkTick re-inits
         * within a tick - but never inside the backoff window, which would
         * pair with this teardown into a 1 Hz create/destroy cycle. */
        EinkShutdownCapture();
    if (!g_ecap[0]) g_ering_retry_at = now + 1000;
    want = (g_ecap[0] && g_eproc_show && g_s.master && g_s.mode == MODE_EINK);
    EinkWorkerSet(want);
    if (want != last_want) {   /* a persistent not-ready state ticks at 20 Hz: say it once */
        L("eink buffers %s (worker=%d)", want ? "armed" : "idle", g_ework ? 1 : 0);
        last_want = want;
    }
}

/* ------------------------------- e-ink worker thread --------------------- */

static DWORD WINAPI EinkWorker(LPVOID unused) {
    (void)unused;
    for (;;) {
        int i, pick = -1;
        unsigned char *src = NULL;
        LONG rendered_gen;
        EnterCriticalSection(&g_ecs);
        for (i = 0; i < EINK_SLOTS; i++)
            if (g_ecap_state[i] == 1) { pick = i; break; }            /* newest */
        if (pick >= 0) {
            src = g_ecap[pick];
            for (i = 0; i < EINK_SLOTS; i++)
                if (g_ecap_state[i] == 2) g_ecap_state[i] = 0;        /* drop older */
        } else if (g_egen_done != g_eset_gen) {                       /* settings moved */
            for (i = 0; i < EINK_SLOTS; i++)
                if (g_ecap_state[i] == 3) { pick = i; src = g_ecap[i]; break; }
        }
        LeaveCriticalSection(&g_ecs);
        if (!src) {
            if (g_estop) break;
            WaitForSingleObject(g_ewake, 100);
            continue;
        }
        /* the heavy step, outside the lock: nobody else touches the build
         * buffer while the worker owns it. Snapshot the generation first: a
         * settings bump landing mid-render must survive the write-back, or
         * the frame built from the old settings is published as up to date
         * and the adjustment is lost until an unrelated capture arrives. */
        rendered_gen = g_eset_gen;
        EinkRender(src, g_ebuild, g_vsw, g_vsh);
        EnterCriticalSection(&g_ecs);
        {   /* publish: what was the build buffer becomes the shown one */
            unsigned char *t = g_eproc_show;
            g_eproc_show = g_ebuild;
            g_ebuild = t;
            g_eproc_valid = 1;
        }
        for (i = 0; i < EINK_SLOTS; i++)
            if (g_ecap_state[i] == 3) g_ecap_state[i] = 0;            /* previous kept */
        if (pick >= 0) g_ecap_state[pick] = 3;                        /* keep this frame */
        g_egen_done = rendered_gen;
        LeaveCriticalSection(&g_ecs);
        if (g_host) PostMessageW(g_host, WM_APP_EINK, 0, 0);
    }
    return 0;
}

static void EinkWorkerSet(int want) {
    if (want && !g_ework) {
        if (!g_ecs_ready) { InitializeCriticalSection(&g_ecs); g_ecs_ready = 1; }
        InterlockedExchange(&g_estop, 0);
        g_ewake = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!g_ewake) { L("eink worker wake event failed: %lu", GetLastError()); return; }
        g_ework = CreateThread(NULL, 0, EinkWorker, NULL, 0, NULL);
        if (!g_ework) {
            CloseHandle(g_ewake); g_ewake = NULL;
            L("eink worker thread failed: %lu", GetLastError());
            return;
        }
        return;
    }
    if (!want && g_ework) {
        int i;
        InterlockedExchange(&g_estop, 1);
        SetEvent(g_ewake);
        /* the render step is coalesced to whole frames; a join never hangs
         * on a partial frame, so a plain wait is safe here */
        WaitForSingleObject(g_ework, INFINITE);
        CloseHandle(g_ework); CloseHandle(g_ewake);
        g_ework = NULL; g_ewake = NULL;
        for (i = 0; i < EINK_SLOTS; i++) g_ecap_state[i] = 0;   /* start clean */
        g_egen_done = g_eset_gen;                    /* nothing to re-render yet */
    }
}

/* Capture slot ownership, UI-thread side only. */
static unsigned char *EinkCapBegin(void) {
    int i;
    unsigned char *p = NULL;
    if (!g_ework || !g_ecs_ready) return NULL;
    EnterCriticalSection(&g_ecs);
    for (i = 0; i < EINK_SLOTS; i++)
        if (g_ecap_state[i] == 0) { g_ecap_state[i] = 4; p = g_ecap[i]; break; }
    LeaveCriticalSection(&g_ecs);
    return p;
}

/* Returns 1 when a fresh frame was published. */
static int EinkCapEnd(int ok, unsigned char *p) {
    int i, published = 0;
    if (!p) return 0;
    EnterCriticalSection(&g_ecs);
    for (i = 0; i < EINK_SLOTS; i++) {
        if (g_ecap[i] != p) continue;
        if (g_ecap_state[i] != 4) break;             /* ownership lost */
        if (ok) {
            int j;
            for (j = 0; j < EINK_SLOTS; j++)
                if (g_ecap_state[j] == 1) g_ecap_state[j] = 2;   /* this one is older */
            g_ecap_state[i] = 1;
            SetEvent(g_ewake);
            published = 1;
        } else {
            g_ecap_state[i] = 0;
        }
        break;
    }
    LeaveCriticalSection(&g_ecs);
    return published;
}

/* Present, on the UI thread: one memcpy per monitor and the upload. */
static void EinkPresent(void) {
    int i;
    for (i = 0; i < g_n; i++)
        if (PresentEinkRect(&g_ov[i]))
            ApplyLayered(&g_ov[i]);   /* never upload a buffer nothing refreshed */
}

/* Abandon DXGI for the GDI grab. One helper, because this exact sequence used
 * to be written out three times in EinkTick and the policy then had to be
 * right in three places: the 2026-10-07 round added the rotation fallback and
 * got it wrong in the one copy it edited (an "every output unusable" test left
 * a laptop panel plus rotated monitor set on DXGI), which is precisely the
 * class a single copy removes.
 *
 * retry is the caller's permanence judgement: rotation never changes for a
 * display, so that fallback is permanent; a lost access or an unsupported
 * format may recover. dst is released here so no caller can forget it. */
static void EinkFallBackToGdi(unsigned char *dst, int retry, const char *why) {
    EinkCapEnd(0, dst);
    g_dxgi_retry = retry;
    EinkShutdownCapture();
    g_gdi_only = 1;
    g_dxgi_fail_at = GetTickCount();
    if (why && !g_fallback_logged) {
        g_fallback_logged = 1;
        L("eink: %s", why);
    }
}

static int DxgiRectsStale(void) {
    int i;
    for (i = 0; i < g_nout; i++) {
        DXGI_OUTPUT_DESC d;
        if (!g_out[i].out || FAILED(g_out[i].out->lpVtbl->GetDesc(g_out[i].out, &d)))
            return 1;   /* the output is gone from the desktop: re-anchor */
        if (d.DesktopCoordinates.left   - g_vsx != g_out[i].r.left
            || d.DesktopCoordinates.top    - g_vsy != g_out[i].r.top
            || d.DesktopCoordinates.right  - g_vsx != g_out[i].r.right
            || d.DesktopCoordinates.bottom - g_vsy != g_out[i].r.bottom)
            return 1;
    }
    return 0;
}

static void EinkTick(void) {
    unsigned char *dst;
    int got = 0;
    static int nf;

    if (!g_s.master || g_s.mode != MODE_EINK) { EinkWorkerSet(0); return; }
    if (g_dxgi == 0) {
        if (g_gdi_only) {
            /* retry duplication occasionally in case the reason is gone */
            if (g_dxgi_retry && GetTickCount() - g_dxgi_fail_at > 2000) {
                if (DxgiInit()) {
                    g_dxgi = 1;
                    g_gdi_only = 0;
                    g_dxgi_retry = 1;
                    if (!g_recover_logged) {
                        g_recover_logged = 1;
                        L("eink: duplication recovered");
                    }
                } else {
                    g_dxgi_fail_at = GetTickCount();
                }
            }
        } else if (EinkRingBackoff()) {
            /* the ring failed to allocate and is backing off: standing up
             * duplication now would pair with the re-anchor shutdown in
             * EinkBuffersRestart into a 1 Hz full create/destroy cycle with
             * its own log flood - wait for the window to expire */
        } else if (DxgiInit()) {
            g_dxgi = 1;
            g_dxgi_retry = 1;
            if (!g_init_logged) {
                g_init_logged = 1;
                L("eink: dxgi duplication live");
            }
        } else {
            g_gdi_only = 1;
            g_dxgi_retry = 1;
            L("eink: duplication unavailable, GDI fallback");
            return;
        }
    }
    if (!EinkBuffersReady()) { EinkBuffersRestart(); return; }
    {
        static int rect_tick;
        if (++rect_tick >= 64) {   /* ~3 s: g_out[].r is mapped once at init, so a
            * pure monitor rearrangement that WM_DISPLAYCHANGE never reports and
            * the virtual-screen metrics cannot see (identical resolutions
            * swapped left/right) would put each output's frames in the other
            * monitor's slot half forever - re-check the anchor cheaply */
            rect_tick = 0;
            if (DxgiRectsStale()) {
                L("eink: desktop arrangement changed, capture re-anchoring");
                EinkShutdownCapture();
                return;   /* next tick re-inits against the new coordinates */
            }
        }
    }
    if (!g_ework) EinkWorkerSet(1);          /* mode/master flipped: arm it */

    dst = EinkCapBegin();
    if (!dst) return;                        /* worker behind: coalesce, skip */
    if (g_dxgi == 1 && DxgiAnyRotated()) {
        /* DXGI cannot express a rotated desktop (see DxgiInit): on a mixed set
         * one rotated output would leave its monitor showing stale pixels
         * while the identity outputs kept updating. Checked before polling, so
         * an identity output producing a frame cannot mask it. Rotation never
         * changes for a display, so this fallback is permanent - no 2 s retry. */
        EinkFallBackToGdi(dst, 0, "rotated output present, GDI grab for the whole desktop");
        return;
    }
    if (g_dxgi == 1) {
        int lost = 0;
        got = DxgiPoll(dst);
        if (g_dxgi == 0) lost = 1;           /* DxgiPoll shut capture down */
        if (lost) {
            /* DxgiPoll already reported the loss; its own shutdown makes
             * EinkFallBackToGdi's call a no-op on an empty output set, so
             * no second line is logged here. */
            EinkFallBackToGdi(dst, 1, NULL);
            return;
        }
        if (got == 0 && !DxgiHasUsableOutput()) {
            /* Every output has an unsupported desktop format, so DXGI will
             * never produce a frame. The GDI grab returns the composed desktop
             * (rotation already applied by Windows), so fall back to it
             * instead of letting e-ink freeze on the last frame. Retryable is
             * 1 here by construction: any rotated output was handled above. */
            EinkFallBackToGdi(dst, DxgiRetryable(),
                              "no usable duplication output, GDI fallback");
            return;
        }
    } else {
        got = GdiPoll(dst);
    }
    if (!EinkCapEnd(got, dst)) return;
    if ((++nf % 25) == 0)
        L("eink frame %d (dxgi=%d gdi_only=%d)", nf, g_dxgi, g_gdi_only);
}


/* headless dump support (validation without touching the screen) */
static void DumpBgra(const char *path, unsigned char *bgra, int w, int h) {
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *f;
    int x, y;
    int stride = ((w * 3 + 3) / 4) * 4;
    unsigned char *row = (unsigned char *)malloc(stride);
    if (!row) return;
    memset(&fh, 0, sizeof fh);
    memset(&ih, 0, sizeof ih);
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + stride * h;
    ih.biSize = sizeof ih;
    ih.biWidth = w;
    ih.biHeight = h;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    ih.biCompression = BI_RGB;
    f = fopen(path, "wb");
    if (!f) { free(row); return; }
    fwrite(&fh, 1, sizeof fh, f);
    fwrite(&ih, 1, sizeof ih, f);
    for (y = h - 1; y >= 0; y--) {          /* BMP is bottom-up */
        for (x = 0; x < w; x++) {
            unsigned char *s = bgra + 4 * ((size_t)y * w + x);
            row[x * 3 + 0] = s[0];
            row[x * 3 + 1] = s[1];
            row[x * 3 + 2] = s[2];
        }
        fwrite(row, 1, stride, f);
    }
    fclose(f);
    free(row);
}

/* composite the low-alpha veil over a stand-in app background (default light
   gray) so the dump shows what the eye actually sees, not the raw bitmap */
static int g_dump_bg = 235;
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

static void DumpPaper(const char *path, int w, int h) {
    OVL ov;
    memset(&ov, 0, sizeof ov);
    ov.w = w; ov.h = h;
    ov.bits = malloc((size_t)w * h * 4);
    if (!ov.bits) return;
    BuildPaper(&ov);
    CompositeOver((unsigned char *)ov.bits, w, h, g_dump_bg);
    DumpBgra(path, (unsigned char *)ov.bits, w, h);
    free(ov.bits);
}

/* synthetic desktop: gradient, blocks, text-like lines -> exercises e-ink */
static void DumpEink(const char *path, int w, int h) {
    int x, y;
    unsigned char *cap = (unsigned char *)malloc((size_t)w * h * 4);
    unsigned char *proc = (unsigned char *)malloc((size_t)w * h * 4);
    if (!cap || !proc) { free(cap); free(proc); return; }
    unsigned char *bgra = cap;
    g_vsw = w; g_vsh = h;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned char *p = bgra + 4 * ((size_t)y * w + x);
            int v = (int)(255.0 * x / w);
            p[2] = (unsigned char)v;
            p[1] = (unsigned char)(255 - v / 2);
            p[0] = 128;
            p[3] = 255;
            if ((y / 24) % 2 == 0 && (x / 8) % 2 == 0) {   /* dither target grid */
                p[2] = p[1] = p[0] = (unsigned char)((x / 8) % 2 ? 230 : 25);
            }
            if (y % 96 < 3 || x % 96 < 3) {                  /* rules */
                p[2] = p[1] = p[0] = 10;
            }
        }
    }
    if (cap && proc) {
        EinkRender(cap, proc, w, h);
        DumpBgra(path, proc, w, h);
    }
    free(cap);
    free(proc);
}



/* ---------------------------------------------------------- settings UI --- */

static HWND g_tb_intensity, g_tb_warmth, g_tb_grain, g_tb_fibre, g_tb_blotch;
static HWND g_tb_shades, g_tb_contrast, g_tb_dither, g_btn_adv;
static HWND g_chk_share;   /* settings-window mirror of the tray share toggle */
static void ActivateMode(int mode);   /* defined in the commands section below */
static HWND g_lb_strength, g_lb_warmth, g_lb_grain, g_lb_fibre, g_lb_blotch;
static HWND g_lb_shades, g_lb_contrast, g_lb_dither;
static int   g_advanced;
static const WCHAR *SET_CLASS = L"MnPaperSettings";
/* ---------------- on-demand help window (replaces hover tips) ------------- */

static HWND g_helpwnd;          /* single "?" help window              */
static void SetMaster(int on);  /* defined in the commands section below */

static const WCHAR HELP_TEXT[] =
    L"mnPaper - how every control works\r\n"
    L"\r\n"
    L"MODE\r\n"
    L"  Paper  - the everyday warm paper texture over everything.\r\n"
    L"  E-ink  - the WHOLE screen becomes greyscale like a Kindle reader. "
    L"Black and white, and it can shimmer while things move. You are asked "
    L"to confirm first. Paper (or Ctrl+Alt+E) always switches back "
    L"instantly.\r\n"
    L"\r\n"
    L"PAPER SLIDERS - applied the moment you move them\r\n"
    L"  Strength - how strongly the texture is drawn over the screen. "
    L"0 = off, 30 = default, 40 = heavy.\r\n"
    L"  Warmth   - colour tint. 0 = cool bluish, 50 = neutral, 100 = aged "
    L"amber. The ends are strong on purpose.\r\n"
    L"  Grain    - size of the speckles. Low = fine film grain, high = "
    L"coarse sand. While dragging you see a coarse preview; the exact "
    L"grain refines right after you stop.\r\n"
    L"  Advanced >>\r\n"
    L"    Fibre  - long stringy streaks running through the paper, like "
    L"real pulp fibres. 0 = smooth, 100 = clearly stringy.\r\n"
    L"    Blotch - large soft patches of uneven tone, like handmade paper. "
    L"0 = uniform, 100 = strong patchy shading.\r\n"
    L"\r\n"
    L"E-INK SLIDERS\r\n"
    L"  Shades   - how many greyscale levels. 2 = stark black and white, "
    L"4 = default, 16 = nearly smooth.\r\n"
    L"  Contrast - distance between light and dark.\r\n"
    L"  Dither   - pixel mixing that fakes extra greyscale levels.\r\n"
    L"\r\n"
    L"OTHER CONTROLS\r\n"
    L"  Texture on - uncheck to hide the texture instantly; mnPaper keeps "
    L"running in the tray (Ctrl+Alt+P does the same).\r\n"
    L"  Start with Windows - launch mnPaper automatically at login.\r\n"
    L"  Check for updates automatically - the app reads a tiny version file "
    L"about once a day and, only if a new version exists, shows a tray note. "
    L"Unchecked, it checks only when you press the button.\r\n"
    L"  Texture in shares/screenshots\r\n"
    L"    Unchecked (default): screenshots and screen shares see the clean "
    L"desktop while you still see the texture. Checked: captures include "
    L"the texture. E-ink is always hidden from captures.\r\n"
    L"  ?        - this window.\r\n"
    L"  Check for updates - compares your version with the published one. "
    L"If a new version exists it offers to download and install it: the "
    L"download is verified against a published fingerprint (SHA-256) before "
    L"anything changes, the swap happens next to the exe, and mnPaper "
    L"restarts into the new version. Your settings are kept. If anything "
    L"fails, nothing is modified and you can always download manually from "
    L"the releases page.\r\n"
    L"  Close    - closes the window. Every change is applied and saved the "
    L"moment you move a slider - there is no Save button.\r\n"
    L"\r\n"
    L"HOTKEYS\r\n"
    L"  Ctrl+Alt+P  - turn the effect on/off.\r\n"
    L"  Ctrl+Alt+E  - switch between Paper and E-ink.\r\n";

static LRESULT CALLBACK HelpProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE: {
        HWND e = GetWindow(h, GW_CHILD);
        RECT rc;
        GetClientRect(h, &rc);
        if (e) MoveWindow(e, 0, 0, rc.right, rc.bottom, TRUE);
        return 0;
    }
    case WM_DESTROY:
        if (h == g_helpwnd) g_helpwnd = NULL;
        break;
    }
    return DefWindowProcW(h, m, wp, lp);
}

/* Every path that makes the help window visible funnels through here:
 * a headless run must never show a window or touch the foreground. */
static void HelpPresent(HWND h) {
    if (g_test_headless) return;
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}

static void OpenHelp(HWND owner) {
    WNDCLASSEXW w;
    RECT rc, cr;
    HWND e;
    HFONT f;
    if (g_helpwnd && IsWindow(g_helpwnd)) {
        HelpPresent(g_helpwnd);
        return;
    }
    memset(&w, 0, sizeof w);
    w.cbSize        = sizeof w;
    w.lpfnWndProc   = HelpProc;
    w.hInstance     = GetModuleHandleW(NULL);
    w.lpszClassName = L"MnPaperHelp";
    w.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    w.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    w.hIcon         = LoadIconW(w.hInstance, MAKEINTRESOURCEW(1));
    w.hIconSm       = w.hIcon;
    RegisterClassExW(&w);   /* re-registration after a close fails harmlessly */
    rc.left = 0; rc.top = 0; rc.right = 470; rc.bottom = 580;
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_helpwnd = CreateWindowExW(0, L"MnPaperHelp", L"mnPaper help",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_helpwnd) return;
    GetClientRect(g_helpwnd, &cr);   /* same rect WM_SIZE keeps the edit on */
    e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", HELP_TEXT,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_LEFT,
        0, 0, cr.right, cr.bottom,
        g_helpwnd, NULL, GetModuleHandleW(NULL), NULL);
    f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(e, WM_SETFONT, (WPARAM)f, TRUE);
    HelpPresent(g_helpwnd);
}

/* ---------------------- update check (link-out, safe) -------------------- */

static volatile LONG g_update_busy;   /* one check at a time */
static int g_upd_manual;              /* does the pending check answer to a click? */
static NOTIFYICONDATAW g_nid;
static UINT g_msg_taskbar;  /* "TaskbarCreated": explorer restarts, re-add */

/* Parse "[v] 3.2.1 ..." -> 1 on success. Rejects empty/garbage feeds. */
static int ParseVersionTriple(const char *s, int *ma, int *mi, int *pa) {
    int n[3] = { 0, 0, 0 }, idx = 0, seen = 0;
    const char *p = s;
    while (*p && (*p == ' ' || *p == '\t' || *p == 'v' || *p == 'V')) p++;
    while (*p) {
        if (*p >= '0' && *p <= '9') {
            seen = 1;
            n[idx] = n[idx] * 10 + (*p - '0');
            if (n[idx] > 9999) return 0;
        } else if (*p == '.') {
            if (!seen) return 0;
            if (++idx > 2) return 0;
            seen = 0;
        } else break;
        p++;
    }
    if (!seen || idx < 1) return 0;          /* need at least major.minor */
    *ma = n[0]; *mi = n[1]; *pa = n[2];
    return 1;
}

static int CompareVersion(int a0, int a1, int a2, int b0, int b1, int b2) {
    if (a0 != b0) return a0 < b0 ? -1 : 1;
    if (a1 != b1) return a1 < b1 ? -1 : 1;
    if (a2 != b2) return a2 < b2 ? -1 : 1;
    return 0;
}

#define UPT_NONE      0   /* unreachable / offline / feed not published */
#define UPT_MALFORMED 1   /* feed answered but stated no version          */
#define UPT_NEW       2   /* newer version available                      */
#define UPT_SAME      3   /* already on the latest version                */

typedef struct {
    int result;
    WCHAR ver[24];    /* version string as published by the feed */
    WCHAR hash[65];   /* 64 hex chars if the feed pins the exe    */
} UpdInfo;

/* Find a 64-hex-char sequence (the exe's SHA-256 pin) in the feed body.
 * Only an exact run of 64 bounded by a non-hex byte is a pin; a longer run
 * is rejected rather than read as the last 64 characters of itself. */
static int FindHash64(const char *s, WCHAR *out) {
    const char *p = s;
    int run = 0, i;
    for (; *p; p++) {
        char c = *p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            if (++run > 64) return 0;
        } else {
            if (run == 64) {
                for (i = 0; i < 64; i++) out[i] = (WCHAR)p[i - 64];
                out[64] = 0;
                return 1;
            }
            run = 0;
        }
    }
    if (run == 64) {
        for (i = 0; i < 64; i++) out[i] = (WCHAR)p[i - 64];
        out[64] = 0;
        return 1;
    }
    return 0;
}

/* SHA-256 -> 64 lowercase hex chars via Windows' own bcrypt (no deps). */
static int Sha256Hex(const unsigned char *data, DWORD len, WCHAR *hex) {
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE h = NULL;
    unsigned char dig[32];
    int i, ok = 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return 0;
    if (BCryptCreateHash(alg, &h, NULL, 0, NULL, 0, 0) == 0) {
        if (BCryptHashData(h, (PUCHAR)data, len, 0) == 0 &&
            BCryptFinishHash(h, dig, sizeof dig, 0) == 0) {
            for (i = 0; i < 32; i++)
                _snwprintf(hex + i * 2, 3, L"%02x", dig[i]);
            hex[64] = 0;
            ok = 1;
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

/* Plain HTTPS GET of a URL into memory (TLS validation are WinHTTP defaults).
 * Reads at most max_bytes and hands the malloc'd buffer back through
 * *out_buf with its length in *out_len; a response that does not fit is
 * refused whole, never read past the allocation, and sets *too_big when it
 * is given, so a caller can tell a size refusal from a transport failure.
 * recv_ms is the receive timeout: short for the version feed, long for the
 * exe download. */
/* The one cap decision HttpGetToMem makes per chunk: WinHTTP hands the body
 * over in pieces, and a piece that does not fit under the cap means the body
 * as a whole is over the cap - it is refused whole, never truncated. Kept as
 * a function so the regression suite can drive the boundary directly. */
static int FeedFits(DWORD total, DWORD got, DWORD max_bytes) {
    return got <= max_bytes - total;
}

static int HttpGetToMem(const WCHAR *url, DWORD max_bytes, DWORD recv_ms,
                        unsigned char **out_buf, DWORD *out_len, int *too_big) {
    WCHAR host[256] = L"", path[512] = L"", ua[32];
    URL_COMPONENTSW uc = { sizeof(uc) };
    HINTERNET ses = NULL, con = NULL, req = NULL;
    unsigned char *buf = NULL, *nb;
    DWORD cap = 0, total = 0, got = 0, status = 0, stlen = sizeof status;
    *out_buf = NULL;
    *out_len = 0;
    if (too_big) *too_big = 0;
    _snwprintf(ua, 32, L"mnPaper/%d.%d.%d", MNVER_MAJOR, MNVER_MINOR, MNVER_PATCH);
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;  uc.dwUrlPathLength = 512;
    if (!WinHttpCrackUrl(url, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS)
        return 0;
    ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 5000, 5000, 5000, recv_ms);
    con = WinHttpConnect(ses, host, uc.nPort, 0);
    if (!con) goto done;
    req = WinHttpOpenRequest(con, L"GET", path, NULL, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!req) goto done;
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(req, NULL))
        goto done;
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &stlen,
                             WINHTTP_NO_HEADER_INDEX) || status != 200)
        goto done;
    while (total < max_bytes) {
        if (!WinHttpQueryDataAvailable(req, &got) || !got) break;
        if (!FeedFits(total, got, max_bytes)) { if (too_big) *too_big = 1; goto done; }   /* more than the cap holds: refuse it whole */
        if (total + got > cap) {
            /* geometric growth: the old "cap = total + got" grew the buffer
             * by one ~8 KB read at a time, so an 8 MB update realloc'd (and
             * copied) roughly a thousand times. */
            cap = cap ? cap * 2 : 262144;
            if (cap < total + got) cap = total + got;
            if (cap > max_bytes) cap = max_bytes;
            nb = (unsigned char *)realloc(buf, cap);
            if (!nb) goto done;
            buf = nb;
        }
        if (!WinHttpReadData(req, buf + total, got, &got)) goto done;
        total += got;
    }
    /* A body that filled the cap exactly is complete and legitimate - the loop
     * above stops the moment total reaches max_bytes. Only a body with bytes
     * still to read is over the cap, and that case is refused inside the loop.
     * The old `total >= max_bytes` check here counted a body of exactly the cap
     * as oversized and threw away a good download. */
    if (buf && total) {
        *out_buf = buf;
        *out_len = total;
        buf = NULL;
    }
done:
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    if (ses) WinHttpCloseHandle(ses);
    free(buf);
    return *out_buf != NULL;
}

/* Worker thread: HTTPS GET of UPDATE_URL (TLS + hostname validation are
 * WinHTTP defaults - no ignored-certification flags anywhere). Reads at most
 * FEED_MAX_BYTES, compares numbers and posts a WM_APP_UPDATE to the host
 * window, which lives for the whole process: the settings dialog that started
 * the check can be closed (and its handle recycled) long before the answer
 * arrives. It NEVER navigates to anything from the feed - the result opens the
 * built-in PRODUCT_URL only. No user data leaves the machine (the request is
 * a bare GET with a product user-agent). */
static DWORD WINAPI UpdateCheckThread(LPVOID param) {
    (void)param;   /* the host window receives the result, not the caller */
    int result = UPT_NONE;
    UpdInfo *u = (UpdInfo *)calloc(1, sizeof *u);
    unsigned char *body = NULL;
    DWORD len = 0;
    int manual = g_upd_manual;
    if (u && HttpGetToMem(UPDATE_URL, FEED_MAX_BYTES, 10000, &body, &len, NULL)) {
        char text[FEED_MAX_BYTES + 1];
        int ma, mi, pa;
        memcpy(text, body, len);   /* the helper hands back at most the cap */
        text[len] = 0;
        free(body);
        if (!ParseVersionTriple(text, &ma, &mi, &pa)) {
            result = UPT_MALFORMED;   /* answered, but stated no version */
        } else if (CompareVersion(ma, mi, pa, MNVER_MAJOR, MNVER_MINOR,
                                  MNVER_PATCH) > 0) {
            result = UPT_NEW;
            _snwprintf(u->ver, 24, L"%d.%d.%d", ma, mi, pa);
            FindHash64(text, u->hash);   /* optional pin; empty = no self-update */
        } else {
            result = UPT_SAME;        /* same or older: already current */
        }
    }
    if (u) u->result = result;   /* a failed worker still posts, with no payload */
    InterlockedExchange(&g_update_busy, 0);   /* the answer is captured: release before it is dispatched */
    PostMessageW(g_host, WM_APP_UPDATE, (WPARAM)manual, (LPARAM)u);
    return 0;
}

static int UpdDue(void);   /* defined below, before first use in the thread path */
static void UpdNote(HWND owner, const WCHAR *title, const WCHAR *text);

/* Both wrappers used to re-implement UpdNote's MessageBoxW call
 * (2026-10-04 review); they only differ in text. */
static void UpdateStartFailed(HWND owner) {
    UpdNote(owner, L"mnPaper - update",
            L"Could not start the update check. Please try again.");
}

static void UpdateInstallStartFailed(HWND owner) {
    UpdNote(owner, L"mnPaper - update",
            L"Could not start the update installation.\n"
            L"Nothing was changed - please try again.");
}

static void UpdateBusyNotice(HWND owner) {
    MessageBoxW(owner, L"An update is already in progress.",
                L"mnPaper - update", MB_OK | MB_ICONINFORMATION);
}

/* manual = the user pressed the button (always runs); auto = the daily lazy
 * check (gated by the autoupd setting and the once-a-day timestamp).
 * The manual path is INTENTIONALLY not gated on g_test_headless: the
 * update_help_capture suite's MN_VAL_NETWORK build clicks 117 for a real
 * check. The only thing standing between a headless click and a network
 * request is the caller pre-setting g_update_busy (load-bearing - see the
 * test); the CAS below is that gate. */
static void StartUpdateCheck(int manual) {
    HANDLE t;
    static DWORD last_manual_tick;
    static int seeded;
    if (manual) {
        DWORD now = GetTickCount();
        if (!seeded) {   /* arm the debounce so the first real click is never dropped */
            last_manual_tick = now - 60000;
            seeded = 1;
        }
        if (now - last_manual_tick < 5000) return;   /* double-click debounce */
        last_manual_tick = now;
    } else {
        if (!g_s.autoupd || g_test_headless) return; /* tests never touch network */
        if (!UpdDue()) return;                       /* checked within the last day */
    }
    if (InterlockedCompareExchange(&g_update_busy, 1, 0) != 0) {
        if (manual)
            UpdateBusyNotice(LiveDlg());
        return;   /* a check is already running */
    }
    g_upd_manual = manual;
    t = CreateThread(NULL, 0, UpdateCheckThread, NULL, 0, NULL);
    if (!t) {
        InterlockedExchange(&g_update_busy, 0);
        if (manual) UpdateStartFailed(LiveDlg());
        return;
    }
    CloseHandle(t);   /* the busy flag clears in the thread */
}

static ULONGLONG NowFT(void) {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

/* the lazy daily cadence: 22h gate so jitter in wall time can never push
 * two checks closer than "about once a day". A check that could not read
 * the feed backs off instead of retrying every hour forever: an hour doubled
 * per consecutive failure (so an hour, two, four ... sixteen), and the same
 * 22h cap, because a backoff beyond a day is no backoff at all. */
static int UpdDue(void) {
    const ULONGLONG hour = 3600ULL * 10000000ULL, day = 22ULL * hour;
    ULONGLONG wait = day;
    if (g_upd_fails > 0) {
        int f = g_upd_fails < 5 ? g_upd_fails : 5;   /* past the cap a bigger shift changes nothing */
        wait = hour << f;
        if (wait > day) wait = day;
    }
    return g_lastupd == 0 || NowFT() - g_lastupd > wait;
}

/* the stamp and the backoff belong together: a check that read the feed
 * starts a fresh day, a check that could not doubles the wait */
static void SaveUpdState(void) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    RegSetValueExW(k, L"lastupd",  0, REG_QWORD, (BYTE *)&g_lastupd,   sizeof(g_lastupd));
    RegSetValueExW(k, L"updfails", 0, REG_DWORD, (BYTE *)&g_upd_fails, sizeof(g_upd_fails));
    RegCloseKey(k);
}

static void SaveLastUpdNow(void) {
    g_lastupd = NowFT();
    g_upd_fails = 0;   /* the feed answered: the daily cadence is honest again */
    SaveUpdState();
}

static void SaveUpdFailed(void) {
    g_lastupd = NowFT();
    if (g_upd_fails < 64) g_upd_fails++;   /* back off, but never grow without bound */
    SaveUpdState();
}

static void UpdBalloon(const WCHAR *ver) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    lstrcpyW(n.szInfoTitle, L"mnPaper update");
    _snwprintf(n.szInfo, 256,
        L"Version %s is available. Open settings and press \"Check for updates\" to install it.", ver);
    n.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

/* Self-update: download the published exe, verify it byte-for-byte against
 * the feed's SHA-256 pin, then swap it in. A running exe cannot be
 * overwritten but it CAN be renamed: current -> .old, new -> current, and
 * the .old is swept on the next start. The fresh process must not race our
 * single-instance mutex, so it is started by a short cmd chain after we are
 * gone. Settings live in the registry and are untouched by any of this.
 * The download and the hash can take minutes on a throttled link, so they
 * run on the host's worker thread exactly like the update check: the same
 * busy flag keeps one network operation at a time, and the outcome arrives
 * as WM_APP_INSTALL. The swap is two local renames - fast - so it stays on
 * the host thread together with every dialog and the relaunch. */
#define UPD_DL_NET      0   /* fetch failed: offline or the release is gone   */
#define UPD_DL_SHORT    1   /* fetched, but incomplete or shorter than a real exe */
#define UPD_DL_MISMATCH 2   /* the fingerprint could not be computed, or it is not the published one */
#define UPD_DL_OK       3   /* fetched and verified: install it               */
#define UPD_DL_CAP      4   /* refused whole: the asset did not fit the cap    */

typedef struct {
    int result;
    unsigned char *buf;      /* the verified bytes; freed by InstallResult */
    DWORD len;
    WCHAR hash[65];          /* the pin this download was checked against   */
} InstInfo;

static DWORD WINAPI SelfUpdateThread(LPVOID param);

static void StartSelfUpdate(const WCHAR *hash_hex, HWND owner) {
    HANDLE t;
    InstInfo *in;
    if (InterlockedCompareExchange(&g_update_busy, 1, 0) != 0) {
        UpdateBusyNotice(owner);
        return;   /* a check, or another install, is already running */
    }
    in = (InstInfo *)calloc(1, sizeof *in);
    if (!in) {
        InterlockedExchange(&g_update_busy, 0);
        UpdateInstallStartFailed(owner);
        return;
    }
    lstrcpynW(in->hash, hash_hex, 65);
    t = CreateThread(NULL, 0, SelfUpdateThread, in, 0, NULL);
    if (!t) {
        free(in);
        InterlockedExchange(&g_update_busy, 0);
        UpdateInstallStartFailed(owner);
        return;
    }
    CloseHandle(t);   /* the busy flag clears in the thread */
}

static DWORD WINAPI SelfUpdateThread(LPVOID param) {
    InstInfo *in = (InstInfo *)param;
    unsigned char *buf = NULL;
    DWORD len = 0;
    WCHAR hex[65];
    int result = UPD_DL_NET;
    int too_big = 0;
    if (HttpGetToMem(UPDATE_EXE_URL, UPDATE_MAX_BYTES, 30000, &buf, &len, &too_big)) {
        if (len < 65536) {
            result = UPD_DL_SHORT;   /* a truncated body: never compared */
        } else if (!Sha256Hex(buf, len, hex) || _wcsnicmp(hex, in->hash, 64)) {
            result = UPD_DL_MISMATCH;   /* never verified, or not the published one */
        } else {
            in->buf = buf;   /* handed to the host thread */
            buf = NULL;      /* from here on the payload owns these bytes */
            in->len = len;
            result = UPD_DL_OK;
        }
    } else if (too_big) {
        result = UPD_DL_CAP;   /* refused whole: the release does not fit */
    }
    free(buf);
    in->result = result;
    InterlockedExchange(&g_update_busy, 0);   /* the answer is captured: release before it is dispatched */
    PostMessageW(g_host, WM_APP_INSTALL, 0, (LPARAM)in);
    return 0;
}

/* A path that has to live inside a batch file, or on a cmd command line,
 * must round-trip through the active code page: the file holds ANSI bytes
 * and an unrepresentable character is silently written as '?'. */
static int BatchSafePath(const WCHAR *p) {
    char a[2 * MAX_PATH + 2];
    WCHAR back[MAX_PATH + 2];
    int n = WideCharToMultiByte(CP_ACP, 0, p, -1, a, sizeof a, NULL, NULL);
    int m;
    if (n <= 1) return 0;
    m = MultiByteToWideChar(CP_ACP, 0, a, n - 1, back, MAX_PATH + 1);
    if (m <= 0) return 0;
    back[m] = 0;   /* an explicit-length conversion is not required to terminate */
    return wcscmp(p, back) == 0;
}

/* Writes the temporary .cmd that starts the new exe once we are gone, and
 * launches it after a short delay. The install path enters the batch file
 * through its short (8.3) name, which is ASCII, so the path is data inside
 * a file instead of a command line to parse. Returns 0 when the chain
 * cannot be built safely. */
static int RelaunchAfterSwap(const WCHAR *exe) {
    WCHAR exe8[MAX_PATH], tdir[MAX_PATH], cmdf[MAX_PATH + 32], cmd8[MAX_PATH + 32];
    WCHAR line[2 * MAX_PATH + 8], cmd[2 * MAX_PATH + 64];
    char ansi[2 * MAX_PATH + 8];
    int i, j = 0, n;
    DWORD wrote;
    HANDLE sf;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    if (!GetShortPathNameW(exe, exe8, MAX_PATH) || !exe8[0] ||
        !BatchSafePath(exe8))
        return 0;
    if (!GetTempPathW(MAX_PATH, tdir) || !tdir[0]) return 0;
    _snwprintf(cmdf, MAX_PATH + 32, L"%smnpaper-upd.cmd", tdir);
    line[j++] = L'@'; line[j++] = L'"';
    for (i = 0; exe8[i] && j < 2 * MAX_PATH; i++) {
        line[j++] = exe8[i];
        if (exe8[i] == L'%') line[j++] = L'%';   /* % is special in a .cmd */
    }
    line[j++] = L'"'; line[j++] = L'\r'; line[j++] = L'\n'; line[j] = 0;
    n = WideCharToMultiByte(CP_ACP, 0, line, -1, ansi, sizeof ansi, NULL, NULL);
    if (n <= 0) return 0;
    sf = CreateFileW(cmdf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, NULL);
    if (sf == INVALID_HANDLE_VALUE) return 0;
    if (!WriteFile(sf, ansi, (DWORD)n - 1, &wrote, NULL) || wrote != (DWORD)n - 1) {
        CloseHandle(sf);
        return 0;
    }
    CloseHandle(sf);
    if (!GetShortPathNameW(cmdf, cmd8, MAX_PATH + 32) || !cmd8[0] ||
        !BatchSafePath(cmd8))
        return 0;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    _snwprintf(cmd, 2 * MAX_PATH + 64, L"/c ping -n 3 127.0.0.1 >nul & \"%s\"", cmd8);
    if (!CreateProcessW(L"C:\\Windows\\System32\\cmd.exe", cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
}

/* Runs on the host thread: every dialog, the write, the swap and the
 * relaunch. Neither rename can fail for the running process (it may rename
 * its own image), but a failure must never leave the user with no exe at
 * all, and never destroy the only verified copy on disk. */
/* One shape for every "nothing was changed" box the updater can raise: same
 * flags, owner asked for at the call site. Text and title vary per failure. */
static void UpdNote(HWND owner, const WCHAR *title, const WCHAR *text) {
    MessageBoxW(owner, text, title, MB_OK | MB_ICONWARNING);
}

/* The two "feed answered but odd - open the page anyway?" prompts. */
static int UpdAskPage(HWND owner, const WCHAR *text) {
    return MessageBoxW(owner, text, L"mnPaper - check for updates",
                       MB_YESNO | MB_ICONWARNING) == IDYES;
}

static void InstallResult(InstInfo *in) {
    WCHAR exe[MAX_PATH], old[MAX_PATH + 8], newf[MAX_PATH + 8];
    unsigned char *buf;
    DWORD len, wrote;
    HANDLE f;
    HWND owner = LiveDlg();
    DWORD exe_len;
    if (!in) return;
    exe_len = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (exe_len == 0 || exe_len >= MAX_PATH) {
        /* same null-truncation rule as ApplyAutostart: a truncated path does
         * not name this exe, so the swap must not touch whatever it does name */
        free(in->buf);
        free(in);
        UpdNote(owner, L"mnPaper - update",
            L"The update could not be applied. Nothing was changed - "
            L"download manually from the releases page.");
        return;
    }
    if (in->result == UPD_DL_MISMATCH) {
        free(in->buf);
        free(in);
        UpdNote(owner, L"mnPaper - update refused",
            L"The downloaded file does not match the published fingerprint.\n"
            L"It was discarded and nothing was changed.");
        return;
    }
    if (in->result == UPD_DL_SHORT) {
        free(in->buf);
        free(in);
        UpdNote(owner, L"mnPaper - update",
            L"The download was incomplete. Nothing was changed - please try again.");
        return;
    }
    if (in->result == UPD_DL_CAP) {
        free(in->buf);
        free(in);
        UpdNote(owner, L"mnPaper - update refused",
            L"The download exceeded the size limit.\n"
            L"Nothing was changed.");
        return;
    }
    if (in->result != UPD_DL_OK) {
        free(in->buf);
        free(in);
        UpdNote(owner, L"mnPaper - update",
            L"The download failed (offline, or the release is not reachable).\n"
            L"Nothing was changed. You can try again later or download manually from the releases page.");
        return;
    }
    buf = in->buf;
    len = in->len;
    free(in);
    lstrcpyW(newf, exe); lstrcpyW(newf + lstrlenW(newf), L".new");
    f = CreateFileW(newf, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE || !WriteFile(f, buf, len, &wrote, NULL) ||
        wrote != len) {
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        DeleteFileW(newf);
        free(buf);
        UpdNote(owner, L"mnPaper - update",
                L"Could not write the update next to the exe. Nothing was changed.");
        return;
    }
    CloseHandle(f);
    free(buf);
    /* no box here yet: the swap has not happened, so nothing may say so */
    lstrcpyW(old, exe); lstrcpyW(old + lstrlenW(old), L".old");
    DeleteFileW(old);   /* best effort: a leftover must never be renamed back */
    if (!MoveFileExW(exe, old, MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(newf);   /* nothing moved: the running exe is untouched */
        UpdNote(owner, L"mnPaper - update",
            L"The update could not be applied (the exe is locked). Nothing was changed - "
            L"try again in a moment, or download manually from the releases page.");
        return;
    }
    if (!MoveFileExW(newf, exe, MOVEFILE_REPLACE_EXISTING)) {
        if (!MoveFileExW(old, exe, MOVEFILE_REPLACE_EXISTING)) {
            /* the undo failed as well: .new is the only verified copy */
            UpdNote(owner, L"mnPaper - update",
                L"The update could not be applied, and the previous exe could not be "
                L"restored either.\nThe new version is saved as mnPaper.exe.new next to "
                L"mnPaper.exe - close mnPaper and rename it over mnPaper.exe, or "
                L"download manually from the releases page.");
            return;
        }
        DeleteFileW(newf);
        UpdNote(owner, L"mnPaper - update",
            L"The update could not be applied (the exe is locked). Nothing was changed - "
            L"try again in a moment, or download manually from the releases page.");
        return;
    }
    if (!RelaunchAfterSwap(exe)) {
        UpdNote(owner, L"mnPaper - update",
            L"The update is installed, but mnPaper could not start itself again.\n"
            L"Please start mnPaper from your shortcut or the Start menu.");
        L("self-update: swapped, but the relaunch failed");
    } else {
        L("self-update: swapped and relaunching");
    }
    { HWND dlg = LiveDlg(); if (dlg) DestroyWindow(dlg); }
    DestroyWindow(g_host);   /* clean shutdown: SaveSettings, tray removal */
}

/* Results land on the host window's thread. Any "open page" action uses ONLY
 * the built-in PRODUCT_URL - never a string received from the network. */
static void UpdateResult(HWND dlg, int manual, UpdInfo *u) {
    int open = 0;
    int result;
    if (dlg && !IsWindow(dlg)) dlg = NULL;
    result = u ? u->result : UPT_NONE;   /* a worker that could not answer posts no payload */
    /* only a check that actually saw the feed consumes the daily slot; one
     * that could not is remembered, so the auto cadence backs off */
    if (result == UPT_SAME || result == UPT_NEW)
        SaveLastUpdNow();
    else
        SaveUpdFailed();
    if (result == UPT_NEW && u->ver[0]) {
        if (manual) {
            if (u->hash[0]) {
                WCHAR msg[220];
                _snwprintf(msg, 220,
                    L"Version %s of mnPaper is available.\n\n"
                    L"Download and install it now? The download is verified against a "
                    L"published fingerprint before anything changes, and mnPaper "
                    L"restarts into the new version.", u->ver);
                if (MessageBoxW(dlg, msg, L"mnPaper - update available",
                                MB_YESNO | MB_ICONINFORMATION) == IDYES)
                    StartSelfUpdate(u->hash, dlg);
            } else {
                WCHAR msg[160];
                _snwprintf(msg, 160,
                    L"Version %s of mnPaper is available.\n\nOpen the download page now?", u->ver);
                open = MessageBoxW(dlg, msg, L"mnPaper - update available",
                                   MB_YESNO | MB_ICONINFORMATION) == IDYES;
            }
        } else {
            UpdBalloon(u->ver);   /* the daily check never opens dialogs */
        }
    } else if (result == UPT_SAME) {
        if (manual)
            MessageBoxW(dlg, L"You are running the latest version of mnPaper.",
                        L"mnPaper - up to date", MB_OK | MB_ICONINFORMATION);
    } else if (result == UPT_MALFORMED) {
        if (manual)
            open = UpdAskPage(dlg,
                L"The update feed answered, but its contents could not be read as a "
                L"version number.\n\nOpen the mnPaper download page anyway?");
    } else {
        if (manual)
            open = UpdAskPage(dlg,
                L"Could not reach the update feed (offline, or the feed is not "
                L"published yet).\n\nOpen the mnPaper download page anyway?");
    }
    free(u);
    if (open)
        ShellExecuteW(dlg, L"open", PRODUCT_URL, NULL, NULL, SW_SHOWNORMAL);
}

static HWND MkTrack(HWND parent, int id, int lo, int hi, int pos, int x, int y, int w, int h) {
    HWND t = CreateWindowExW(0, L"msctls_trackbar32", NULL,
        WS_CHILD | WS_VISIBLE | TBS_HORZ | WS_TABSTOP,
        x, y, w, h, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(t, TBM_SETRANGE, TRUE, MAKELONG(lo, hi));
    SendMessageW(t, TBM_SETPOS, TRUE, pos);
    return t;
}

static HWND MkLabel(HWND parent, const WCHAR *text, int x, int y, int w, int h) {
    return CreateWindowExW(0, L"STATIC", text,
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        x, y, w, h, parent, NULL, GetModuleHandleW(NULL), NULL);
}

static int TbVal(HWND t) {
    return t ? (int)SendMessageW(t, TBM_GETPOS, 0, 0) : 0;
}

/* numeric readout next to each slider: the veil change can be subtle, the
   number always responds. Indexed by ROW ORDER (0..7), not by control id; the
   matching slider ids live in UpdateVals' own table below. */
static HWND g_val[8];

static void UpdateVals(HWND dlg) {
    static const int ids[8] = { 100, 101, 102, 103, 104, 105, 106, 107 };
    WCHAR buf[16];
    int k;
    if (!dlg) dlg = g_dlg;
    if (!dlg) return;
    for (k = 0; k < 8; k++) {
        if (!g_val[k]) continue;
        wsprintfW(buf, L"%d", TbVal(GetDlgItem(dlg, ids[k])));
        SetWindowTextW(g_val[k], buf);
    }
}

static void DlgLayout(void) {
    /* hide/show whole rows: label + trackbar + numeric readout.  Both control
     * sets are created at the same client positions, so a set left visible
     * paints on top of the active one (wrong names and values on screen). */
    int paper = (g_s.mode == MODE_PAPER);
    int adv = paper && g_advanced;
    ShowWindow(g_lb_strength,  paper ? SW_SHOW : SW_HIDE);
    ShowWindow(g_tb_intensity, paper ? SW_SHOW : SW_HIDE);
    ShowWindow(g_val[0],       paper ? SW_SHOW : SW_HIDE);
    ShowWindow(g_lb_warmth,    paper ? SW_SHOW : SW_HIDE);
    ShowWindow(g_tb_warmth,    paper ? SW_SHOW : SW_HIDE);
    ShowWindow(g_val[1],       paper ? SW_SHOW : SW_HIDE);
    ShowWindow(g_lb_grain,     paper ? SW_SHOW : SW_HIDE);
    ShowWindow(g_tb_grain,     paper ? SW_SHOW : SW_HIDE);
    ShowWindow(g_val[2],       paper ? SW_SHOW : SW_HIDE);
    ShowWindow(g_btn_adv,      paper ? SW_SHOW : SW_HIDE);
    ShowWindow(g_lb_fibre,     adv ? SW_SHOW : SW_HIDE);
    ShowWindow(g_tb_fibre,     adv ? SW_SHOW : SW_HIDE);
    ShowWindow(g_val[3],       adv ? SW_SHOW : SW_HIDE);
    ShowWindow(g_lb_blotch,    adv ? SW_SHOW : SW_HIDE);
    ShowWindow(g_tb_blotch,    adv ? SW_SHOW : SW_HIDE);
    ShowWindow(g_val[4],       adv ? SW_SHOW : SW_HIDE);
    ShowWindow(g_lb_shades,    paper ? SW_HIDE : SW_SHOW);
    ShowWindow(g_tb_shades,    paper ? SW_HIDE : SW_SHOW);
    ShowWindow(g_val[5],       paper ? SW_HIDE : SW_SHOW);
    ShowWindow(g_lb_contrast,  paper ? SW_HIDE : SW_SHOW);
    ShowWindow(g_tb_contrast,  paper ? SW_HIDE : SW_SHOW);
    ShowWindow(g_val[6],       paper ? SW_HIDE : SW_SHOW);
    ShowWindow(g_lb_dither,    paper ? SW_HIDE : SW_SHOW);
    ShowWindow(g_tb_dither,    paper ? SW_HIDE : SW_SHOW);
    ShowWindow(g_val[7],       paper ? SW_HIDE : SW_SHOW);
}

static void DlgSyncBars(void) {
    SendMessageW(g_tb_intensity, TBM_SETPOS, TRUE, g_s.intensity);
    SendMessageW(g_tb_warmth,    TBM_SETPOS, TRUE, g_s.warmth);
    SendMessageW(g_tb_grain,     TBM_SETPOS, TRUE, g_s.grain);
    SendMessageW(g_tb_fibre,     TBM_SETPOS, TRUE, g_s.fibre);
    SendMessageW(g_tb_blotch,    TBM_SETPOS, TRUE, g_s.blotch);
    SendMessageW(g_tb_shades,    TBM_SETPOS, TRUE, g_s.shades);
    SendMessageW(g_tb_contrast,  TBM_SETPOS, TRUE, g_s.contrast);
    SendMessageW(g_tb_dither,    TBM_SETPOS, TRUE, g_s.dither);
    DlgLayout();
}

static void ReadBarsToSettings(void) {
    if (g_s.mode == MODE_PAPER) {
        g_s.intensity = TbVal(g_tb_intensity);
        g_s.warmth    = TbVal(g_tb_warmth);
        g_s.grain     = TbVal(g_tb_grain);
        g_s.fibre     = TbVal(g_tb_fibre);
        g_s.blotch    = TbVal(g_tb_blotch);
    } else {
        g_s.shades    = TbVal(g_tb_shades);
        g_s.contrast  = TbVal(g_tb_contrast);
        g_s.dither    = TbVal(g_tb_dither);
    }
    ClampSettings();
}

/* Check the mode radio and keep BOTH radios tab stops. Windows' radio-group
 * management moves the group's single tab stop onto the checked button, so a
 * bare CheckRadioButton would leave the inactive mode radio (E-ink in paper
 * mode) unreachable by Tab; re-assert it on both after every check change. */
static void SyncModeRadios(HWND dlg, int id) {
    HWND a, b;
    if (!dlg || !IsWindow(dlg)) return;
    CheckRadioButton(dlg, 114, 115, id);
    a = GetDlgItem(dlg, 114);
    b = GetDlgItem(dlg, 115);
    if (a) SetWindowLongW(a, GWL_STYLE, GetWindowLongW(a, GWL_STYLE) | WS_TABSTOP);
    if (b) SetWindowLongW(b, GWL_STYLE, GetWindowLongW(b, GWL_STYLE) | WS_TABSTOP);
}

/* The ONE place that pushes g_s into the dialog controls. Every "refresh the
 * window because settings changed" site calls this - WM_CREATE, a mode
 * switch, a tray-menu change and a CLI sync used to each push a different
 * subset, which is how the share checkbox kept its old state after a remote
 * change. Narrow paths stay narrow: when the user clicks a checkbox, the
 * control is the source of truth and only that one control is written back
 * (or nothing is, for the tri-state boxes whose mirror is a no-op). */
static void DialogPushSettings(HWND dlg) {
    if (!dlg || !IsWindow(dlg)) return;
    DlgSyncBars();              /* bar positions from g_s + row visibility */
    UpdateVals(dlg);            /* numeric readouts from the bars */
    SyncModeRadios(dlg, g_s.mode == MODE_PAPER ? 114 : 115);
    if (g_chk_share) {
        EnableWindow(g_chk_share, g_s.mode == MODE_PAPER);   /* e-ink is always capture-excluded */
        SetChk(g_chk_share, g_s.share);
    }
    SetChk(GetDlgItem(dlg, 118), g_s.master);
    SetChk(GetDlgItem(dlg, 119), g_s.autostart);
    SetChk(GetDlgItem(dlg, 120), g_s.autoupd);
}

static LRESULT CALLBACK DlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        int y = 14;
        g_val[0] = MkLabel(hwnd, L"30", 302, y + 4, 44, 20); g_tb_intensity = MkTrack(hwnd, 100, 0, 40,  g_s.intensity, 106, y, 190, 26);
        g_lb_strength = MkLabel(hwnd, L"Strength", 14, y + 4, 92, 20);
        y += 34;
        g_val[1] = MkLabel(hwnd, L"45", 302, y + 4, 44, 20); g_tb_warmth    = MkTrack(hwnd, 101, 0, 100, g_s.warmth, 106, y, 190, 26);
        g_lb_warmth = MkLabel(hwnd, L"Warmth",   14, y + 4, 92, 20);
        y += 34;
        g_val[2] = MkLabel(hwnd, L"4",  302, y + 4, 44, 20); g_tb_grain     = MkTrack(hwnd, 102, 2, 12, g_s.grain, 106, y, 190, 26);
        g_lb_grain = MkLabel(hwnd, L"Grain",    14, y + 4, 92, 20);
        y += 34;
        g_btn_adv = CreateWindowExW(0, L"BUTTON", L"Advanced >>",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 14, y, 180, 24, hwnd, (HMENU)110, GetModuleHandleW(NULL), NULL);
        y += 34;
        g_val[3] = MkLabel(hwnd, L"40", 302, y + 4, 44, 20); g_tb_fibre     = MkTrack(hwnd, 103, 0, 100, g_s.fibre, 106, y, 190, 26);
        g_lb_fibre = MkLabel(hwnd, L"Fibre",    14, y + 4, 92, 20);
        y += 34;
        g_val[4] = MkLabel(hwnd, L"30", 302, y + 4, 44, 20); g_tb_blotch    = MkTrack(hwnd, 104, 0, 100, g_s.blotch, 106, y, 190, 26);
        g_lb_blotch = MkLabel(hwnd, L"Blotch",   14, y + 4, 92, 20);
        g_val[5] = MkLabel(hwnd, L"4",  302, 14 + 4, 44, 20); g_tb_shades    = MkTrack(hwnd, 105, 2, 16, g_s.shades, 106, 14, 190, 26);
        g_lb_shades = MkLabel(hwnd, L"Shades",   14, 14 + 4, 92, 20);
        g_val[6] = MkLabel(hwnd, L"50", 302, 48 + 4, 44, 20); g_tb_contrast  = MkTrack(hwnd, 106, 0, 100, g_s.contrast, 106, 48, 190, 26);
        g_lb_contrast = MkLabel(hwnd, L"Contrast", 14, 48 + 4, 92, 20);
        g_val[7] = MkLabel(hwnd, L"75", 302, 82 + 4, 44, 20); g_tb_dither    = MkTrack(hwnd, 107, 0, 100, g_s.dither, 106, 82, 190, 26);
        g_lb_dither = MkLabel(hwnd, L"Dither",   14, 82 + 4, 92, 20);
        /* Mode radios switch Paper <-> E-ink live; the dialog stays open and
         * morphs (SetMode refreshes rows in place). */
        CreateWindowExW(0, L"BUTTON", L"Paper",
            WS_CHILD | WS_VISIBLE | WS_GROUP | WS_TABSTOP | BS_AUTORADIOBUTTON, 14, 226, 80, 20, hwnd, (HMENU)114,
            GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"E-ink",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, 100, 226, 80, 20, hwnd, (HMENU)115,
            GetModuleHandleW(NULL), NULL);
        /* Texture on/off: hide the veil without quitting the app (same as the
         * tray's master toggle and Ctrl+Alt+P). */
        CreateWindowExW(0, L"BUTTON", L"&Texture on",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 14, 250, 352, 20, hwnd, (HMENU)118,
            GetModuleHandleW(NULL), NULL);
        SetChk(GetDlgItem(hwnd, 118), g_s.master);
        /* Autostart: mirrors the tray's Start-with-Windows item. */
        CreateWindowExW(0, L"BUTTON", L"Start with &Windows",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 14, 272, 352, 20, hwnd, (HMENU)119,
            GetModuleHandleW(NULL), NULL);
        /* Same setting as the tray's share toggle. Grayed in e-ink mode:
         * that mode is always capture-excluded (feedback white-out). */
        g_chk_share = CreateWindowExW(0, L"BUTTON", L"Texture in shares/screenshots",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 14, 294, 352, 20, hwnd, (HMENU)113,
            GetModuleHandleW(NULL), NULL);
        /* daily self-check opt-out; the download itself is always manual */
        CreateWindowExW(0, L"BUTTON", L"Check for updates automatically",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 14, 316, 352, 20, hwnd, (HMENU)120,
            GetModuleHandleW(NULL), NULL);
        /* "?" circle: explanations open only when pressed. Owner-drawn
         * round button, id 116. */
        CreateWindowExW(0, L"BUTTON", L"?",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 14, 338, 26, 24, hwnd, (HMENU)116,
            GetModuleHandleW(NULL), NULL);
        CreateWindowExW(0, L"BUTTON", L"&Close",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 135, 338, 110, 24, hwnd, (HMENU)IDCANCEL, GetModuleHandleW(NULL), NULL);
        /* update check + hash-pinned self-update */
        CreateWindowExW(0, L"BUTTON", L"Check for updates",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 81, 364, 240, 24, hwnd, (HMENU)117,
            GetModuleHandleW(NULL), NULL);
        DialogPushSettings(hwnd);   /* every control reflects g_s: one push, one place */

        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)lp;
        if (wp == 116 && d) {
            RECT rc = d->rcItem;
            HBRUSH bg = CreateSolidBrush(RGB(245, 245, 245));
            HPEN pen = CreatePen(PS_SOLID, 1, RGB(110, 110, 110));
            HGDIOBJ ob = SelectObject(d->hDC, bg), op = SelectObject(d->hDC, pen);
            Ellipse(d->hDC, rc.left, rc.top, rc.right, rc.bottom);
            SelectObject(d->hDC, op);
            SelectObject(d->hDC, ob);
            DeleteObject(pen);
            DeleteObject(bg);
            SetBkMode(d->hDC, TRANSPARENT);
            SetTextColor(d->hDC, RGB(40, 40, 40));
            DrawTextW(d->hDC, L"?", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (d->itemState & ODS_FOCUS)
                DrawFocusRect(d->hDC, &rc);
            return TRUE;
        }
        break;
    }
    case WM_HSCROLL: {
        /* Every change publishes the newest value. The worker coalesces
         * pending snapshots, rather than dropping updates while busy. */
        ReadBarsToSettings();
        UpdateVals(hwnd);
        if (g_s.mode == MODE_PAPER && g_s.master)
            RequestPaperPreview();
        else if (g_s.mode == MODE_EINK && g_s.master)
            RequestEinkRender();   /* re-render only; never re-present here */
        else
            RepaintAll();
        SetTimer(hwnd, TIMER_DEBOUNCE, DEBOUNCE_MS, NULL);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_DEBOUNCE) {
            KillTimer(hwnd, TIMER_DEBOUNCE);
            ReadBarsToSettings();
            SaveSettings();
            if (g_s.mode == MODE_PAPER && g_s.master)
                RequestPaper();          /* refine the already-visible preview */
            else if (g_s.mode == MODE_EINK && g_s.master)
                RequestEinkRender();
            else
                RepaintAll();
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == 110) {
            g_advanced = !g_advanced;
            SetWindowTextW(g_btn_adv, g_advanced ? L"<< Advanced" : L"Advanced >>");
            DlgLayout();
        } else if (LOWORD(wp) == 113) {
            g_s.share = IsDlgButtonChecked(hwnd, 113) == BST_CHECKED;
            SaveSettings();
            ApplyCaptureAll();   /* re-applies the capture exclusion live */
            L("share=%d", g_s.share);
        } else if (LOWORD(wp) == 114 || LOWORD(wp) == 115) {
            int mode = (LOWORD(wp) == 114) ? MODE_PAPER : MODE_EINK;
            if (g_s.mode != mode) {
                if (mode == MODE_EINK) {
                    /* E-ink is dramatic and easy to stumble into. Confirm in
                     * plain words first; Paper always switches instantly so
                     * this dialog doubles as the escape hatch. */
                    if (MessageBoxW(hwnd,
                        L"E-ink turns your ENTIRE screen into a black-and-white, Kindle-style reader view. "
                        L"On a normal laptop screen it looks harsh and can shimmer while content moves. "
                        L"It is for occasional reading, not everyday work.\n\n"
                        L"You can always click Paper here (or press Ctrl+Alt+E) to come back instantly.\n\n"
                        L"Switch to E-ink now?",
                        L"mnPaper - about E-ink mode", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                        SyncModeRadios(hwnd, 114);   /* snap back */
                        return 0;
                    }
                }
                ActivateMode(mode);   /* turns the effect on, saves, repaints */
            }
            SyncModeRadios(hwnd, mode == MODE_PAPER ? 114 : 115);
        } else if (LOWORD(wp) == 118) {
            SetMaster(IsDlgButtonChecked(hwnd, 118) == BST_CHECKED);
        } else if (LOWORD(wp) == 119) {
            g_s.autostart = IsDlgButtonChecked(hwnd, 119) == BST_CHECKED;
            SaveSettings();   /* SaveSettings applies the Run key */
            L("autostart=%d", g_s.autostart);
        } else if (LOWORD(wp) == 120) {
            g_s.autoupd = IsDlgButtonChecked(hwnd, 120) == BST_CHECKED;
            SaveSettings();
            L("autoupd=%d", g_s.autoupd);
        } else if (LOWORD(wp) == 116) {
            OpenHelp(hwnd);   /* explanations on demand, never on hover */
        } else if (LOWORD(wp) == 117) {
            StartUpdateCheck(1);
        } else if (LOWORD(wp) == IDCANCEL) {
            SendMessageW(hwnd, WM_TIMER, TIMER_DEBOUNCE, 0);
            DestroyWindow(hwnd);
        }
        return 0;
    case WM_CLOSE:
        SendMessageW(hwnd, WM_TIMER, TIMER_DEBOUNCE, 0);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g_dlg = NULL;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* The controls are laid out inside a 402x402 client rect, so the client has to
 * be exactly that whatever frame the shell hands the window: the frame's real
 * size depends on the display scaling, and AdjustWindowRect does not always
 * predict it (a request for an 804x804 window was measured to produce a
 * 778x733 client at 200%, which would have clipped the bottom row). Measure
 * the frame that was actually applied and correct the window by the gap. */
static void FitClient(HWND h, int cw, int ch) {
    RECT w, c;
    if (!h || !GetWindowRect(h, &w) || !GetClientRect(h, &c)) return;
    SetWindowPos(h, NULL, 0, 0,
                 (w.right - w.left) + (cw - (c.right - c.left)),
                 (w.bottom - w.top) + (ch - (c.bottom - c.top)),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void OpenSettings(void) {
    RECT rc;
    WNDCLASSEXW wc;
    if (g_test_headless) return;   /* the gate's contract: no window on the working desktop */
    L("open settings");
    {
        HWND have = LiveDlg();   /* ask rather than assume: the dialog can die between ticks */
        if (have) {
            SetForegroundWindow(have);
            return;
        }
    }
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = DlgProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = SET_CLASS;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);
    g_advanced = 0;
    rc.left = 0; rc.top = 0; rc.right = 402; rc.bottom = 402;
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    g_dlg = CreateWindowExW(WS_EX_TOPMOST | WS_EX_CONTROLPARENT, SET_CLASS,
        L"mnPaper settings", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (g_dlg) {
        FitClient(g_dlg, 402, 402);   /* the client the controls are laid out in */
        DlgSyncBars();
        ShowWindow(g_dlg, SW_SHOW);
        UpdateWindow(g_dlg);
    }
}

#define CMD_TOGGLE   1
#define CMD_ON       2
#define CMD_OFF      3
#define CMD_PAPER    4
#define CMD_EINK     5
#define CMD_SETTINGS 6
#define CMD_QUIT     7
#define CMD_SYNC     8      /* push parsed settings to the running instance */

/* ------------------------------------------------------------ commands --- */

static void SyncMasterCheckbox(void) {   /* mirror g_s.master into control 118 */
    HWND dlg = LiveDlg();
    if (dlg)
        SetChk(GetDlgItem(dlg, 118), g_s.master);
}

static void SetMaster(int on) {
    on = on ? 1 : 0;
    if (g_s.master == on) return;
    g_s.master = on;
    RepaintAll();
    SaveSettings();
    SyncMasterCheckbox();
    L("master=%d", on);
}

static void SetMode(int mode) {
    if (g_s.mode == mode) return;
    g_s.mode = mode;
    if (mode == MODE_EINK)
        EinkBuffersRestart();
    SaveSettings();
    {
        HWND dlg = LiveDlg();
        if (dlg) {
            /* Morph the dialog in place rather than closing it: re-read bar
             * ranges for the new mode, then re-show rows and values. */
            DialogPushSettings(dlg);
        }
    }
    if (g_s.master)
        RepaintAll();
    L("mode=%s", mode == MODE_PAPER ? "paper" : "eink");
}

static void ActivateMode(int mode) {         /* also turns master on */
    g_s.master = 1;
    SyncMasterCheckbox();
    if (g_s.mode != mode) {
        /* SetMode restarts the e-ink ring when needed, repaints and saves.
         * No eager EinkBuffersRestart here: it used to allocate the whole
         * ring on paper-mode clicks and leave it pinned until exit
         * (2026-10-05 review). */
        SetMode(mode);
        return;
    }
    RepaintAll();           /* unchanged mode: one repaint shows the veil */
    SaveSettings();
}

static const int STRENGTH_STEPS[4] = { 10, 20, 30, 40 };
static const WCHAR *STRENGTH_NAMES[4] = { L"Subtle (10)", L"Soft (20)", L"Medium (30)", L"Strong (40)" };

static void TrayMenu(void) {
    HMENU m = CreatePopupMenu();
    HMENU sub = CreatePopupMenu();
    POINT pt;
    int i;
    for (i = 0; i < 4; i++)
        AppendMenuW(sub, MF_STRING | (g_s.intensity == STRENGTH_STEPS[i] ? MF_CHECKED : 0),
                    IDM_STRENGTH + i, STRENGTH_NAMES[i]);
    AppendMenuW(m, MF_STRING | (g_s.master ? MF_CHECKED : 0), IDM_MASTER, L"Paper effect on");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)sub, L"Strength");
    AppendMenuW(m, MF_STRING | (g_s.mode == MODE_PAPER ? MF_CHECKED : 0), IDM_PAPER, L"Paper texture");
    AppendMenuW(m, MF_STRING | (g_s.mode == MODE_EINK  ? MF_CHECKED : 0), IDM_EINK,  L"E-ink");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (g_s.share ? MF_CHECKED : 0)
                     | (g_s.mode == MODE_EINK ? MF_GRAYED : 0), IDM_SHARE,
                L"Texture in shares/screenshots");   /* e-ink always hides: match the dialog */
    AppendMenuW(m, MF_STRING, IDM_SETTINGS, L"Settings...");
    AppendMenuW(m, MF_STRING | (g_s.autostart ? MF_CHECKED : 0), IDM_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_STRING | (g_s.autoupd ? MF_CHECKED : 0), IDM_AUTOUPD,
                L"Check for updates automatically");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_EXIT, L"Exit");
    GetCursorPos(&pt);
    SetForegroundWindow(g_host);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_host, NULL);
    PostMessageW(g_host, WM_NULL, 0, 0);   /* KB135788: dismiss cleanly if the
                                            * foreground window changed */
    DestroyMenu(sub);
    DestroyMenu(m);
}

/* one-shot self check on a live desktop: styles, click-through, taskbar, guard */

static LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    /* explorer restarts destroy every tray icon; without this the app's only
     * mouse entry point is gone for the rest of the session */
    if (g_msg_taskbar && msg == g_msg_taskbar) {
        if (g_test_headless)
            L("tray: TaskbarCreated re-add suppressed in headless tests");
        else if (!Shell_NotifyIconW(NIM_ADD, &g_nid))
            L("tray: icon re-add after explorer restart failed (err %lu)",
              (unsigned long)GetLastError());
        return 0;
    }
    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_TIMER:
        if (wp == TIMER_UPD) {
            /* hourly wakeup: UpdDue's 22h stamp decides if this one fetches */
            SetTimer(hwnd, TIMER_UPD, 3600000, NULL);
            StartUpdateCheck(0);
        } else if (wp == TIMER_TICK) {
            Housekeeping();
            if (g_s.master && g_s.mode == MODE_EINK)
                EinkTick();
        }
        return 0;
    case WM_HOTKEY:
        if (wp == HOTKEY_MASTER)
            SetMaster(!g_s.master);
        else if (wp == HOTKEY_EINK)
            ActivateMode(g_s.mode == MODE_PAPER ? MODE_EINK : MODE_PAPER);
        return 0;
    case WM_DISPLAYCHANGE:
        L("display change");
        PaperWorkerStop();
        EinkShutdownAll();          /* stops the e-ink worker, frees the ring */
        SyncOverlays();
        RepaintAll();               /* re-arms and ensures the ring for the mode */
        return 0;
    case WM_APP_EINK:
        if (g_s.master && g_s.mode == MODE_EINK) EinkPresent();
        return 0;
    case WM_APP_UPDATE:
        UpdateResult(g_dlg, (int)wp, (UpdInfo *)lp);   /* may outlive the dialog that asked */
        return 0;
    case WM_APP_INSTALL:
        InstallResult((InstInfo *)lp);   /* may outlive the prompt that asked */
        return 0;
    case WM_APP_PAPER:
        ApplyPaperResult((PaperDone *)lp);
        return 0;
    case WM_APP_CMD:
        L("cmd %d", (int)wp);
        switch (wp) {
        case CMD_TOGGLE: SetMaster(!g_s.master); break;
        case CMD_ON:     SetMaster(1); break;
        case CMD_OFF:    SetMaster(0); break;
        case CMD_PAPER:  ActivateMode(MODE_PAPER); break;
        case CMD_EINK:   ActivateMode(MODE_EINK); break;
        case CMD_SETTINGS: OpenSettings(); break;
        case CMD_QUIT:   L("quit"); SaveSettings(); DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_COPYDATA: {
        COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp;
        if (cd && cd->dwData == CMD_SYNC && cd->cbData == sizeof(SETTINGS)) {
            int own_autostart = g_s.autostart;   /* derived from THIS exe's path
             * against the Run key; a sender at another path cannot know it */
            g_s = *(SETTINGS *)cd->lpData;
            if (g_s.autostart < 0) g_s.autostart = own_autostart;
            ClampSettings();
            SaveSettings();
            if (g_s.mode == MODE_EINK)
                EinkBuffersRestart();
            RepaintAll();
            {
                HWND dlg = LiveDlg();
                if (dlg) DialogPushSettings(dlg);
            }
            L("cli sync: master=%d mode=%d", g_s.master, g_s.mode);
        }
        return TRUE;
    }
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP)
            TrayMenu();
        else if (LOWORD(lp) == WM_LBUTTONUP)
            OpenSettings();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_MASTER:
            SetMaster(!g_s.master);
            break;
        case IDM_PAPER:     ActivateMode(MODE_PAPER); break;
        case IDM_EINK:      ActivateMode(MODE_EINK); break;
        case IDM_SETTINGS:  OpenSettings(); break;
        case IDM_STRENGTH + 0: case IDM_STRENGTH + 1:
        case IDM_STRENGTH + 2: case IDM_STRENGTH + 3:
            g_s.intensity = STRENGTH_STEPS[LOWORD(wp) - IDM_STRENGTH];
            {
                HWND dlg = LiveDlg();
                if (dlg) DialogPushSettings(dlg);
            }
            SaveSettings();
            if (g_s.master && g_s.mode == MODE_PAPER)
                RequestPaper();
            else
                RepaintAll();
            L("strength=%d", g_s.intensity);
            break;
        case IDM_SHARE:
            g_s.share = g_s.share ? 0 : 1;
            SaveSettings();
            ApplyCaptureAll();   /* re-applies the capture exclusion state */
            {
                HWND dlg = LiveDlg();
                if (dlg) SetChk(GetDlgItem(dlg, 113), g_s.share);
            }
            L("share=%d", g_s.share);
            break;
        case IDM_AUTOSTART:
            g_s.autostart = g_s.autostart ? 0 : 1;
            SaveSettings();
            {
                HWND dlg = LiveDlg();
                if (dlg) SetChk(GetDlgItem(dlg, 119), g_s.autostart);
            }
            L("autostart=%d", g_s.autostart);
            break;
        case IDM_AUTOUPD:
            g_s.autoupd = g_s.autoupd ? 0 : 1;
            SaveSettings();
            {
                HWND dlg = LiveDlg();
                if (dlg) SetChk(GetDlgItem(dlg, 120), g_s.autoupd);
            }
            L("autoupd=%d", g_s.autoupd);
            break;
        case IDM_EXIT:
            SaveSettings();
            DestroyWindow(hwnd);
            break;
        }
        return 0;
    case WM_DESTROY:
        PaperWorkerStop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --------------------------------------------------------- second instance --- */


static int  g_cmd;
static int  g_has_set;
static int  g_autostart_set;   /* the last ApplySets parsed an autostart entry */
static SETTINGS g_cli_settings;

static WCHAR g_sets[16][80];
static int    g_nsets;
/* owned copies: the parse below used to store pointers INTO the
 * CommandLineToArgvW block and then LocalFree it, so the dump path read
 * freed memory (2026-10-04 review) */
static WCHAR g_dump_tex[MAX_PATH];
static WCHAR g_dump_eink[MAX_PATH];
static int g_dump_w = 960, g_dump_h = 600;

static int SetKeyValue(SETTINGS *s, const WCHAR *arg) {
    const WCHAR *eq = wcschr(arg, L'=');
    WCHAR key[64];
    int v;
    if (!eq || eq - arg >= 64) return 0;
    wcsncpy(key, arg, (size_t)(eq - arg));
    key[eq - arg] = 0;
    {   /* a malformed --set value must be refused, not become a silent 0 */
        const WCHAR *p = eq + 1;
        if (!*p) return 0;
        for (; *p; p++)
            if (*p < L'0' || *p > L'9') return 0;
    }
    v = _wtoi(eq + 1);
    if (!_wcsicmp(key, L"intensity")) s->intensity = v;
    else if (!_wcsicmp(key, L"warmth")) s->warmth = v;
    else if (!_wcsicmp(key, L"grain")) s->grain = v;
    else if (!_wcsicmp(key, L"fibre")) s->fibre = v;
    else if (!_wcsicmp(key, L"blotch")) s->blotch = v;
    else if (!_wcsicmp(key, L"shades")) s->shades = v;
    else if (!_wcsicmp(key, L"contrast")) s->contrast = v;
    else if (!_wcsicmp(key, L"dither")) s->dither = v;
    else if (!_wcsicmp(key, L"mode")) s->mode = v ? MODE_EINK : MODE_PAPER;
    else if (!_wcsicmp(key, L"autostart")) { s->autostart = v ? 1 : 0; g_autostart_set = 1; }
    else if (!_wcsicmp(key, L"share")) s->share = v ? 1 : 0;
    else if (!_wcsicmp(key, L"autoupd")) s->autoupd = v ? 1 : 0;
    else if (!_wcsicmp(key, L"master")) s->master = v ? 1 : 0;
    else return 0;
    g_has_set = 1;
    return 1;
}

static void ApplySets(SETTINGS *s) {
    int i;
    g_autostart_set = 0;
    for (i = 0; i < g_nsets; i++) {
        WCHAR buf[80];
        wcsncpy(buf, g_sets[i], 79);
        buf[79] = 0;
        if (!SetKeyValue(s, buf))
            L("ignoring unknown or malformed --set entry: %S", buf);
    }
    ClampSettingsOf(s);
}

static int SendToRunning(void) {
    HWND h = FindWindowW(L"MnPaperHost", NULL);
    COPYDATASTRUCT cd;
    int tries;
    int ok = 1;
    /* the first instance creates its mutex before its host window, so an
     * immediate command can land in that gap: poll briefly before giving up */
    for (tries = 0; !h && tries < 8; tries++) {
        Sleep(150);
        h = FindWindowW(L"MnPaperHost", NULL);
    }
    if (!h) {
        L("send: host window not found");
        return 0;
    }
    L("send: host %p", (void *)h);
    if (g_has_set) {
        cd.dwData = CMD_SYNC;
        cd.cbData = sizeof(SETTINGS);
        cd.lpData = &g_cli_settings;
        if (!SendMessageTimeoutW(h, WM_COPYDATA, (WPARAM)NULL, (LPARAM)&cd,
                                 SMTO_ABORTIFHUNG, 10000, NULL))
            /* the handler returns TRUE only when it processed us; a wedged
             * host thread (stuck in a driver call) must drop the command and
             * let the exit-2 path report it, not hang the second copy */
            ok = 0;
    }
    if (g_cmd)
        if (!PostMessageW(h, WM_APP_CMD, (WPARAM)g_cmd, 0))
            ok = 0;
    return ok;
}

/* ---------------------------------------------------------------- main --- */

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE prev, LPWSTR cmdline, int show) {
    SetUnhandledExceptionFilter(CrashDump);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW wc;
    MSG msg;
    HANDLE mutex;
    int i;
    int argc = 0;
    LPWSTR *argv;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        for (i = 1; i < argc; i++) {
            if (!_wcsicmp(argv[i], L"--log") && i + 1 < argc) {
                g_log = _wfopen(argv[++i], L"a");
                L("=== mnPaper start ===");
            } else if (!_wcsicmp(argv[i], L"--dump-tex") && i + 3 < argc) {
                wcsncpy(g_dump_tex, argv[++i], MAX_PATH - 1);
                g_dump_w    = _wtoi(argv[++i]);
                g_dump_h    = _wtoi(argv[++i]);
                if (i + 1 < argc && argv[i + 1][0] != L'-')
                    g_dump_bg = _wtoi(argv[++i]);
            } else if (!_wcsicmp(argv[i], L"--dump-eink") && i + 3 < argc) {
                wcsncpy(g_dump_eink, argv[++i], MAX_PATH - 1);
                g_dump_w    = _wtoi(argv[++i]);
                g_dump_h    = _wtoi(argv[++i]);
            } else if (!_wcsicmp(argv[i], L"--no-exclude")) {
                g_force_capture_show = 1;
            } else if (!_wcsicmp(argv[i], L"--capture") && i + 1 < argc) {
                /* friendly alias for --set share=1|0 : texture visible in
                   screenshots and screen shares */
                i++;
                if (g_nsets < 16)
                    wcsncpy(g_sets[g_nsets++],
                            !_wcsicmp(argv[i], L"on") ? L"share=1" : L"share=0", 79);
            } else if (!_wcsicmp(argv[i], L"--toggle")) g_cmd = CMD_TOGGLE;
            else if (!_wcsicmp(argv[i], L"--on"))     g_cmd = CMD_ON;
            else if (!_wcsicmp(argv[i], L"--off"))    g_cmd = CMD_OFF;
            else if (!_wcsicmp(argv[i], L"--paper"))  g_cmd = CMD_PAPER;
            else if (!_wcsicmp(argv[i], L"--eink"))   g_cmd = CMD_EINK;
            else if (!_wcsicmp(argv[i], L"--settings")) g_cmd = CMD_SETTINGS;
            else if (!_wcsicmp(argv[i], L"--quit"))   g_cmd = CMD_QUIT;
            else if (!_wcsicmp(argv[i], L"--set") && i + 1 < argc) {
                i++;
                while (i < argc && g_nsets < 16 && wcschr(argv[i], L'=')) {
                    wcsncpy(g_sets[g_nsets++], argv[i], 79);
                    g_sets[g_nsets - 1][79] = 0;
                    i++;
                }
                i--;
            } else {
                L("ignoring unknown argument: %S", argv[i]);
            }
        }
        LocalFree(argv);
    }

    if (g_dump_tex[0] || g_dump_eink[0]) {
        LoadSettings();
        if (g_nsets)
            ApplySets(&g_s);
        L("dump path: nsets=%d intensity=%d warmth=%d grain=%d fibre=%d blotch=%d",
          g_nsets, g_s.intensity, g_s.warmth, g_s.grain, g_s.fibre, g_s.blotch);
        if (g_dump_tex[0]) {
            char p[512];
            WideCharToMultiByte(CP_UTF8, 0, g_dump_tex, -1, p, sizeof p, NULL, NULL);
            DumpPaper(p, g_dump_w, g_dump_h);
        } else {
            char p[512];
            WideCharToMultiByte(CP_UTF8, 0, g_dump_eink, -1, p, sizeof p, NULL, NULL);
            DumpEink(p, g_dump_w, g_dump_h);
        }
        if (g_log) fclose(g_log);
        return 0;
    }

    /* a completed self-update leaves the previous exe as .old - sweep it */
    {
        WCHAR exe[MAX_PATH], oldp[MAX_PATH + 8];
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        /* a truncated path does not name this exe: deleting "<truncation>.old"
         * could remove a file the truncation happens to name (same rule as
         * InstallResult) - sweep only when the path is provably ours, and let
         * startup continue either way */
        if (n > 0 && n < MAX_PATH) {
            lstrcpyW(oldp, exe); lstrcpyW(oldp + lstrlenW(oldp), L".old");
            DeleteFileW(oldp);   /* best effort; may still be locked right after the swap */
        }
        {   /* the updater's throwaway .cmd also outlives its run */
            WCHAR tdir[MAX_PATH], cmdp[MAX_PATH + 24];
            if (GetTempPathW(MAX_PATH, tdir)) {
                _snwprintf(cmdp, MAX_PATH + 24, L"%smnpaper-upd.cmd", tdir);
                DeleteFileW(cmdp);
            }
        }
    }

    mutex = CreateMutexW(NULL, TRUE, L"mnPaper-single-instance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        LoadSettings();                       /* registry mirrors the live app */
        if (g_nsets) {
            g_cli_settings = g_s;
            ApplySets(&g_cli_settings);
            /* autostart is path-derived on whichever exe reads the registry:
             * a sender at another path cannot know the installed copy's truth,
             * so it is only sent when an entry actually parsed (-1 = keep ours) */
            if (!g_autostart_set)
                g_cli_settings.autostart = -1;
        }
        if (!SendToRunning()) {
            L("send: command not delivered");
            if (g_log) fclose(g_log);
            return 2;   /* nonzero: the caller's command was dropped */
        }
        if (g_log) fclose(g_log);
        return 0;
    }

    LoadSettings();
    if (g_nsets) {                             /* fresh launch with --set */
        ApplySets(&g_s);
        SaveSettings();
    }
    InitCommonControls();
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = HostProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MnPaperHost";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassExW(&wc);

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = OvlProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MnPaperOverlay";
    wc.hbrBackground = NULL;
    RegisterClassExW(&wc);

    g_host = CreateWindowExW(0, L"MnPaperHost", L"mnPaper", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 200, NULL, NULL, hInst, NULL);
    ShowWindow(g_host, SW_HIDE);

    memset(&g_nid, 0, sizeof g_nid);
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_host;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpyW(g_nid.szTip, L"mnPaper");
    g_msg_taskbar = RegisterWindowMessageW(L"TaskbarCreated");
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid))
        /* the handler below re-adds when the shell is ready; at a logon race
         * the shell may simply not exist yet */
        L("tray: icon add failed (err %lu)", (unsigned long)GetLastError());

    if (!RegisterHotKey(g_host, HOTKEY_MASTER, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P'))
        g_hotkey_failed = 1;
    if (!RegisterHotKey(g_host, HOTKEY_EINK, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'E'))
        g_hotkey_failed = 1;
    if (g_hotkey_failed)
        L("hotkey registration failed (another app owns it?)");

    SetTimer(g_host, TIMER_TICK, TICK_MS, NULL);
    /* the daily update check: the host window lives for the whole process, the
     * settings dialog does not */
    SetTimer(g_host, TIMER_UPD, 30000, NULL);

    SyncOverlays();
    if (g_s.mode == MODE_EINK)
        EinkBuffersRestart();
    RepaintAll();
    L("ready master=%d mode=%s monitors=%d strips/mon=%d", g_s.master,
      g_s.mode == MODE_PAPER ? "paper" : "eink", g_n,
      g_n > 0 ? g_ov[0].n_strips : 0);

    while (GetMessageW(&msg, NULL, 0, 0)) {
        {
            HWND d = LiveDlg();
            if (d && IsDialogMessageW(d, &msg))
                continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    for (i = 0; i < g_n; i++)
        DestroyOverlay(&g_ov[i]);
    EinkShutdownAll();
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    SaveSettings();
    if (g_log) fclose(g_log);
    return 0;
}
