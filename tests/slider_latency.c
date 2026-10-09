/* Native regression: hidden settings controls and strip windows only.
 * Exercises the real slider handler, worker, pixel buffer and layered upload.
 * No input injection, foreground changes, real profiles or screen-mode switches.
 */
#define wWinMain MnPaperEntry
#include "../mnPaper.c"
#undef wWinMain

static int failures, applied_count;
static DWORD first_pixel_ms;
static unsigned last_applied_gen;
static ULONGLONG previous_hash;
static DWORD started;
static DWORD preview_ms[128];
static int preview_count;

/* every byte: for equality claims (PixelHash strides 97 and is for change
 * detection only - the 2026-10-03 review flagged one equality claim resting
 * on the sampled hash). PixelHashAll is the shared full-byte hash in
 * tests/fnv64.h, also used by hole_cycle and verify_fixes. */
#include "fnv64.h"   /* full-byte hash: tests/slider_latency.c and fnv64.h sit in tests/ */

static ULONGLONG PixelHash(const unsigned char *p, size_t bytes) {
    ULONGLONG h = 1469598103934665603ULL;
    size_t k;
    for (k = 0; k < bytes; k += 97) h = (h ^ p[k]) * 1099511628211ULL;
    return h;
}
static void Check(int ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}
static LRESULT CALLBACK TestHost(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_APP_PAPER) {
        PaperDone *d = (PaperDone *)l;
        unsigned gen = d->gen;
        int preview = d->preview;
        DWORD queued = d->queued;
        ApplyPaperResult(d);
        if (preview && preview_count < 128)
            preview_ms[preview_count++] = GetTickCount() - queued;
        /* (the old upload-failure counter hung off the g_pupload_ok global,
         * which existed only for it; the property is asserted directly below) */
        last_applied_gen = gen;
        applied_count++;
        if (PixelHash(g_ov[0].bits, (size_t)g_ov[0].w * g_ov[0].h * 4) != previous_hash && !first_pixel_ms)
            first_pixel_ms = GetTickCount() - started;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
/* The hidden test dialog has no WS_VISIBLE on the window itself, so row
 * visibility must be read from the child's own style bit, not the ancestor
 * walk that IsWindowVisible performs. */
static int StyleVisible(HWND w) {
    return (GetWindowLongW(w, GWL_STYLE) & WS_VISIBLE) != 0;
}
static void Pump(DWORD ms) {
    DWORD end = GetTickCount() + ms;
    do {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(1);
    } while ((LONG)(GetTickCount() - end) < 0);
}
static void Slider(int value) {
    SendMessageW(g_tb_intensity, TBM_SETPOS, TRUE, value);
    SendMessageW(g_dlg, WM_HSCROLL, TB_THUMBTRACK, (LPARAM)g_tb_intensity);
}

int main(void) {
    WNDCLASSW wc = {0};
    RECT rc = {0,0,2880,1800};
    WCHAR scratch[128], run[160];
    unsigned char *expected;
    DWORD stop;
    unsigned final_gen;
    int i;
    SETTINGS before = g_s;
    g_test_headless = 1;   /* help + update windows must never become visible in tests */
    /* no g_log here: L() reaches DbgView, and a hardcoded profile path does not belong in a test */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitCommonControls();
    swprintf(scratch, 128, L"Software\\mnPaper-slider-%lu", GetCurrentProcessId());
    swprintf(run, 160, L"%s\\Run", scratch);
    REG_KEY = scratch; lstrcpynW(g_run_key, run, 160);
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpfnWndProc = TestHost; wc.lpszClassName = L"MnPaperTestHost";
    RegisterClassW(&wc);
    g_host = CreateWindowExW(0, wc.lpszClassName, L"", 0,0,0,0,0,HWND_MESSAGE,NULL,wc.hInstance,NULL);
    wc.lpfnWndProc = OvlProc; wc.lpszClassName = L"MnPaperOverlay";
    RegisterClassW(&wc);
    g_n = 1;
    Check(MakeOverlay(&g_ov[0], NULL, &rc), "hidden 2880x1800 strip target created");
    g_ov[0].idx = 0;
    memset(g_ov[0].bits, 0, (size_t)rc.right * rc.bottom * 4);
    previous_hash = PixelHash(g_ov[0].bits, (size_t)rc.right * rc.bottom * 4);
    wc.lpfnWndProc = DlgProc; wc.lpszClassName = SET_CLASS;
    RegisterClassW(&wc);
    g_dlg = CreateWindowExW(0, SET_CLASS, L"", WS_OVERLAPPED, 0, 0, 402, 402,NULL,NULL,wc.hInstance,NULL);
    FitClient(g_dlg, 402, 402);
    Check(g_dlg != NULL && !IsWindowVisible(g_dlg), "settings target stays hidden");
    started = GetTickCount();
    /* A slow enough step stream to mimic dragging without ever moving the cursor. */
    for (i = 0; i < 8; i++) { Slider(8 + i * 3); Pump(60); }
    Slider(37);
    SendMessageW(g_dlg, WM_HSCROLL, TB_ENDTRACK, (LPARAM)g_tb_intensity);
    Pump(450); /* Includes actual trailing timer, isolated registry only. */
    final_gen = g_pgen;
    stop = GetTickCount() + 10000;
    while (last_applied_gen < final_gen && (LONG)(GetTickCount() - stop) < 0) Pump(20);
    printf("METRIC first changed pixel buffer: %lu ms; rendered frames: %d; final request: %u; completed: %u\n",
           first_pixel_ms, applied_count, final_gen, last_applied_gen);
    Check(first_pixel_ms > 0 && first_pixel_ms < 250, "texture pixels change within 250ms, not just numeric readout");
    Check(applied_count >= 3, "multiple texture updates while dragging");
    Check(ApplyLayered(&g_ov[0]) && ApplyLayered(&g_ov[0]),
          "UpdateLayeredWindow accepts the rendered frame (direct calls)");
    /* the counter form died with the g_pupload_ok global; a direct call here
     * proves the same property without a file-scope flag */
    Check(last_applied_gen == final_gen, "last requested value eventually completes without another mouse event");
    printf("METRIC request-to-upload preview latency:");
    for (i = 0; i < preview_count; i++) printf(" %lu", preview_ms[i]);
    printf(" ms\n");
    expected = malloc((size_t)rc.right * rc.bottom * 4);
    BuildPaperInto(expected, rc.right, rc.bottom, &g_s);
    Check(memcmp(expected, g_ov[0].bits, (size_t)rc.right * rc.bottom * 4) == 0,
          "final uploaded bitmap exactly matches full-quality final settings");
    free(expected);

    /* A new drag must preempt, not wait behind, full-quality work. */
    RequestPaper(); Pump(100);
    started = GetTickCount(); first_pixel_ms = 0;
    previous_hash = PixelHash(g_ov[0].bits, (size_t)rc.right * rc.bottom * 4);
    Slider(4); Pump(350);
    Check(first_pixel_ms > 0 && first_pixel_ms < 250,
          "new drag interrupts an in-flight full render within 250ms");
    /* End drag and immediately close, before the 400ms save timer. */
    Slider(19);
    SendMessageW(g_dlg, WM_CLOSE, 0, 0);
    final_gen = g_pgen;
    stop = GetTickCount() + 10000;
    while (g_papplied[0] < final_gen && (LONG)(GetTickCount() - stop) < 0) Pump(20);
    Check(g_papplied[0] == final_gen, "closing immediately still refines and applies the final value");
    {
        HKEY key = NULL;
        int saved = -1;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, scratch, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            saved = GetDword(key, L"intensity", -1); RegCloseKey(key);
        }
        Check(saved == 19, "closing immediately autosaves into isolated test registry");
    }
    /* Taskbar-mask changes must never invoke noise rendering or destroy the
     * backing image. Upload still uses the real hidden layered targets. */
    {
        unsigned gen = g_pgen;
        unsigned char *px = (unsigned char *)g_ov[0].bits;
        int hx = 40, hy = 1700, hw = 80;
        size_t stride = (size_t)rc.right * 4;
        g_hole_on[0] = 1;
        SetRect(&g_hole[0], 0, 1700, 2880, 1800);
        unsigned char *saved = NULL;
        size_t saved_len = 0;
        int rx0=0, ry0=0, rx1=0, ry1=0;
        ULONGLONG armed, armed_all;
        px[(size_t)hy * stride + hx * 4 + 3] = 255;   /* armed before the hole */
        armed = PixelHash(px, (size_t)rc.right * (size_t)rc.bottom * 4);
        armed_all = PixelHashAll(px, (size_t)rc.right * (size_t)rc.bottom * 4);
        /* The hole is a MASK on one upload, not a change to the texture. The
         * upload sees the hole; the master texture must come back untouched,
         * because Housekeeping re-uploads with no rebuild behind it. */
        ClearHoleAlpha(&g_ov[0], &saved, &saved_len, &rx0, &ry0, &rx1, &ry1);
        Check(px[(size_t)hy * stride + hx * 4 + 3] == 0, "taskbar hole zeroes alpha inside the rect");
        Check(px[(size_t)(hy + 40) * stride + (hx + 40) * 4 + 3] == 0, "taskbar hole covers the whole rect");
        RestoreHoleAlpha(&g_ov[0], saved, saved_len, rx0, ry0, rx1, ry1);
        Check(PixelHashAll(px, (size_t)rc.right * (size_t)rc.bottom * 4) == armed_all,
              "the upload leaves the master texture byte-identical (every byte)");
        Check(ApplyLayered(&g_ov[0]), "moving taskbar hole uploads without a texture rebuild");
        Check(g_pgen == gen, "taskbar hole moves without a render generation bump");
        Check(px[(size_t)hy * stride + hx * 4 + 3] != 0, "the texture is not left punched after an upload");
        /* THE REGRESSION (2.7.7): Housekeeping re-uploads when the hole closes
         * or moves with NO rebuild behind it. If the upload leaves the hole
         * punched into the master texture, the transparent band stays after the
         * taskbar parks - a permanent stripe of unrendered screen along the
         * bottom. So: open the hole, upload, close the hole, upload again, and
         * the texture must be exactly as it was before the hole ever opened. */
        {
            ULONGLONG before = PixelHashAll((const unsigned char *)px, (size_t)rc.right * (size_t)rc.bottom * 4);
            g_hole_on[0] = 1;
            SetRect(&g_hole[0], 0, 1700, 2880, 1800);
            ApplyLayered(&g_ov[0]);
            /* ApplyLayered restores the master texture, so the punch is visible
             * only during the upload - which is what keeps the next no-rebuild
             * re-upload correct. What is observable here is that the texture is
             * unchanged by the upload, which the check below pins down. */
            Check(PixelHashAll((const unsigned char *)px, (size_t)rc.right * (size_t)rc.bottom * 4) == before,
                  "an upload with the hole leaves the master texture untouched (every byte)");
            g_hole_on[0] = 0;                       /* taskbar parked: hole closes */
            Check(ApplyLayered(&g_ov[0]), "closing the hole uploads with no rebuild");
            Check(PixelHashAll((const unsigned char *)px, (size_t)rc.right * (size_t)rc.bottom * 4) == before,
                  "closing the hole restores the texture exactly, every byte (no permanent band)");
            Check(px[(size_t)1750 * stride + 200 * 4 + 3] != 0,
                  "the band under a parked taskbar renders paper again");
        }
        g_hole_on[0] = 0;
    }
    /* Small odd sizes cover the grid edge and SIMD scalar tail; all preview
     * bytes remain valid premultiplied alpha. */    {
        unsigned char tiny[31 * 21 * 4];
        int valid = 1;
        Check(BuildPaperPreview(tiny, 31, 21, &g_s, NULL, 0), "odd-sized preview renders");
        for (i = 0; i < 31 * 21; i++)
            if (tiny[4*i] > tiny[4*i+3] || tiny[4*i+1] > tiny[4*i+3] || tiny[4*i+2] > tiny[4*i+3]) valid = 0;
        Check(valid, "preview interpolation retains valid premultiplied alpha");
        g_s.intensity = 0;
        memset(tiny, 0xff, sizeof tiny);
        BuildPaperPreview(tiny, 31, 21, &g_s, NULL, 0);
        valid = 1;
        for (i = 0; i < (int)sizeof tiny; i++) if (tiny[i]) valid = 0;
        Check(valid, "strength zero is fully transparent in preview");
        g_s.intensity = 19;
    }
    for (i = 0; i < g_ov[0].n_strips; i++)
        Check(!IsWindowVisible(g_ov[0].shwnd[i]), "test strip never became visible");
    PaperWorkerStop();
    DestroyOverlay(&g_ov[0]);
    /* One monitor completing must not suppress another monitor's same-gen
     * result. Newest pending snapshots are copied under the real lock. */
    g_n = 2;
    for (i = 0; i < 2; i++) {
        RECT test_rect = {0,0,61,37};
        test_rect.right += i * 18; test_rect.bottom += i * 6;
        Check(MakeOverlay(&g_ov[i], NULL, &test_rect), "hidden multi-monitor test target created");
        g_ov[i].idx = i;
    }
    for (i = 0; i < 200; i++) { g_s.warmth = i % 101; RequestPaperPreview(); }
    g_s.intensity = 38; g_s.warmth = 20; g_s.fibre = 73; g_s.blotch = 44;
    RequestPaper(); final_gen = g_pgen;
    stop = GetTickCount() + 3000;
    while ((g_papplied[0] != final_gen || g_papplied[1] != final_gen) &&
           (LONG)(GetTickCount() - stop) < 0) Pump(10);
    for (i = 0; i < 2; i++) {
        size_t bytes = (size_t)g_ov[i].w * g_ov[i].h * 4;
        expected = malloc(bytes);
        BuildPaperInto(expected, g_ov[i].w, g_ov[i].h, &g_s);
        Check(g_papplied[i] == final_gen && memcmp(expected, g_ov[i].bits, bytes) == 0,
              "each monitor receives the final snapshot after a 200-request burst");
        free(expected);
    }
    /* Each slider changes actual preview pixels across its endpoints. */
    {
        HWND bars[4];
        int mins[4] = {0,2,0,0}, maxs[4] = {100,12,100,100};
        const char *names[4] = {"warmth", "grain", "fibre", "blotch"};
        WNDCLASSW dwc = {0};
        dwc.hInstance = GetModuleHandleW(NULL);
        dwc.lpfnWndProc = DlgProc; dwc.lpszClassName = SET_CLASS;
        RegisterClassW(&dwc);
        g_dlg = CreateWindowExW(0, SET_CLASS, L"", WS_OVERLAPPED, 0, 0, 402, 402,NULL,NULL,dwc.hInstance,NULL);
        FitClient(g_dlg, 402, 402);
        bars[0] = g_tb_warmth; bars[1] = g_tb_grain;
        bars[2] = g_tb_fibre; bars[3] = g_tb_blotch;
        for (i = 0; i < 4; i++) {
            ULONGLONG low, high;
            char message[128];
            SendMessageW(bars[i], TBM_SETPOS, TRUE, mins[i]);
            SendMessageW(g_dlg, WM_HSCROLL, TB_THUMBTRACK, (LPARAM)bars[i]);
            Pump(150); KillTimer(g_dlg, TIMER_DEBOUNCE);
            low = PixelHash(g_ov[0].bits, (size_t)g_ov[0].w * g_ov[0].h * 4);
            SendMessageW(bars[i], TBM_SETPOS, TRUE, maxs[i]);
            SendMessageW(g_dlg, WM_HSCROLL, TB_THUMBTRACK, (LPARAM)bars[i]);
            Pump(150); KillTimer(g_dlg, TIMER_DEBOUNCE);
            high = PixelHash(g_ov[0].bits, (size_t)g_ov[0].w * g_ov[0].h * 4);
            sprintf(message, "%s endpoints change uploaded preview pixels", names[i]);
            Check(low != high && g_papplied[0] == g_platest[0], message);
        }
        /* Warmth must visibly swing hue, not just nibble a channel: over a
         * neutral backdrop, warmth 0 reads cool (blue > red) and warmth 100
         * reads warm (red > blue) by a clear margin in the composited pixel. */
        {
            int w = 320, h = 200, warm_br = 0, cool_br = 0;
            unsigned char *buf = malloc((size_t)w * h * 4);
            SETTINGS saved = g_s;
            g_s.warmth = 100;
            Check(BuildPaperPreview(buf, w, h, &g_s, NULL, 0), "warm preview renders");
            for (i = 0; i < w * h; i++)
                warm_br += (int)buf[4*i] - (int)buf[4*i+2];  /* blue - red */
            g_s.warmth = 0;
            Check(BuildPaperPreview(buf, w, h, &g_s, NULL, 0), "cool preview renders");
            for (i = 0; i < w * h; i++)
                cool_br += (int)buf[4*i] - (int)buf[4*i+2];
            g_s = saved;
            warm_br /= w * h; cool_br /= w * h;
            printf("METRIC warmth swing (blue-red): warm=%d cool=%d\n", warm_br, cool_br);
            Check(warm_br <= -3 && cool_br >= 2 && cool_br - warm_br > 5,
                  "warmth endpoints read distinctly cool and warm");
            free(buf);
        }
    }
    /* Grain, fibre and blotch must be visible in the PREVIEW itself - the
     * old 8px preview sampled the true frequencies on the coarse grid and
     * aliased fine grain into a flat smear, so grain felt seconds behind
     * strength/warmth (captain report 2026-09-30). */
    {
        int w = 640, h = 400;
        unsigned char *buf = malloc((size_t)w * h * 4);
        SETTINGS saved = g_s;
        double sum, sum2, var, std, detail;
        double gstd[11], gdetail[11], m_f0, m_f100, m_b0, m_b100, sd;
        int px, ok, g, render_fail = 0, flat = 0, nonmono = 0;

        /* Two statistics over the preview alpha: its overall spread (std) and
         * its pixel-to-pixel detail (mean neighbour difference). The detail
         * figure is what an 8px grid can lose: bilinear expansion of a grid
         * that sampled the true fine-grain frequencies carries almost no
         * grain response at all. */
        #define PREVIEW_ALPHA_STD(gv, fv, bv, outstd, outdetail) do { \
            g_s.grain = (gv); g_s.fibre = (fv); g_s.blotch = (bv); \
            g_s.intensity = 40; \
            ok = BuildPaperPreview(buf, w, h, &g_s, NULL, 0); \
            if (!ok) render_fail++; \
            sum = sum2 = 0; std = 0; detail = 0; \
            if (ok) { \
                for (px = 0; px < w * h; px++) { double a = buf[4*px+3]; sum += a; sum2 += a*a; } \
                var = sum2 / (w * h) - (sum / (w * h)) * (sum / (w * h)); \
                std = var > 0 ? sqrt(var) : 0; \
                for (px = 0; px < w * h; px += w) { \
                    int k; \
                    for (k = 1; k < w; k++) \
                        detail += fabs((double)buf[4*(px+k)+3] - (double)buf[4*(px+k-1)+3]); \
                } \
                detail /= (double)(w - 1) * h; \
            } \
            outstd = std; outdetail = detail; \
        } while (0)

        /* Grain measured with fibre and blotch off, so neither can lend the
         * statistic structure of its own, across the whole reachable range
         * (ClampSettingsOf caps grain at 12, the trackbar tops out at 12). */
        for (g = 2; g <= 12; g++) {
            PREVIEW_ALPHA_STD(g, 0, 0, gstd[g-2], gdetail[g-2]);
            printf("METRIC grain=%d preview alpha-std %.2f detail %.3f\n",
                   g, gstd[g-2], gdetail[g-2]);
            if (gstd[g-2] <= 2.0) flat = 1;
            if (g > 2 && gdetail[g-2] > gdetail[g-3] + 1e-9) nonmono = 1;
        }
        Check(!render_fail, "preview renders for every structure check");
        Check(!flat, "grain-only preview keeps visible structure at every grain size");
        /* The full-resolution build loses detail as the grain coarsens; the
         * preview has to follow it, which it only does with the frequency
         * scaling. Pre-fix the detail figure ignored the grain slider. */
        Check(!nonmono && gdetail[10] < gdetail[0] * 0.5,
              "preview grain detail falls as grain coarsens (no aliased smear)");

        PREVIEW_ALPHA_STD(6, 0, 47, m_f0, sd);
        printf("METRIC fibre preview std (fibre=0): %.2f\n", m_f0);
        PREVIEW_ALPHA_STD(6, 100, 47, m_f100, sd);
        printf("METRIC fibre preview std (fibre=100): %.2f\n", m_f100);
        Check(m_f100 > m_f0 * 1.15 + 0.5, "fibre=100 adds clearly more structure than fibre=0");

        PREVIEW_ALPHA_STD(6, 100, 0, m_b0, sd);
        printf("METRIC blotch preview std (blotch=0): %.2f\n", m_b0);
        PREVIEW_ALPHA_STD(6, 100, 100, m_b100, sd);
        printf("METRIC blotch preview std (blotch=100): %.2f\n", m_b100);
        Check(m_b100 > m_b0 * 1.15 + 0.5, "blotch=100 adds clearly more structure than blotch=0");

        g_s = saved;
        free(buf);
        #undef PREVIEW_ALPHA_STD
    }
    /* The "?" button opens one shared help window (hidden in headless mode). */
    {
        HWND hbtn = GetDlgItem(g_dlg, 116), h1;
        Check(IsWindow(hbtn), "? help button exists in the settings window");
        SendMessageW(hbtn, BM_CLICK, 0, 0);
        h1 = g_helpwnd;
        Check(h1 && IsWindow(h1) && !IsWindowVisible(h1),
              "? opens the help window (hidden in headless mode)");
        SendMessageW(hbtn, BM_CLICK, 0, 0);
        Check(g_helpwnd == h1, "pressing ? again reuses the same help window");
        Check(h1 && !IsWindowVisible(h1),
              "re-clicking ? reuses it without showing it in headless mode");
        if (h1 && IsWindow(h1)) DestroyWindow(h1);
        Check(g_helpwnd == NULL, "closing the help window clears it");
    }
    /* Update check: pure version parsing/comparison plus the button itself.
     * No live network in tests - the WinHTTP path is exercised by the
     * product, the safety rules (HTTPS only, numeric compare, link-out) are
     * enforced by construction in UpdateCheckThread. */
    {
        int ma, mi, pa;
        HWND hbtn = GetDlgItem(g_dlg, 117);
        Check(IsWindow(hbtn), "Check-for-updates button exists in the settings window");
        Check(ParseVersionTriple("2.6.0", &ma, &mi, &pa) &&
              CompareVersion(ma, mi, pa, 2, 6, 0) == 0,
              "version parse reads its own release string");
        Check(ParseVersionTriple("v2.7.1\n", &ma, &mi, &pa) && ma == 2 && mi == 7 && pa == 1,
              "version parse accepts a v-prefix and trailing newline");
        Check(ParseVersionTriple("10.0.3", &ma, &mi, &pa) && CompareVersion(10, 0, 3, 2, 9, 9) > 0,
              "version compare orders majors before minors");
        Check(CompareVersion(2, 6, 0, 2, 6, 0) == 0 &&
              CompareVersion(2, 5, 9, 2, 6, 0) < 0 &&
              CompareVersion(2, 6, 1, 2, 6, 0) > 0,
              "version compare detects equal, older and newer releases");
        Check(!ParseVersionTriple("", &ma, &mi, &pa) &&
              !ParseVersionTriple("Oops, not a version", &ma, &mi, &pa) &&
              !ParseVersionTriple("2.", &ma, &mi, &pa) &&
              !ParseVersionTriple("2", &ma, &mi, &pa),
              "version parse rejects garbage and incomplete feeds");
        /* FindHash64 is the C half of the release-pin contract - the class of
         * break that shipped 2.7.14 with every suite green - and had no
         * direct coverage at all. A pin is a run of EXACTLY 64 hex chars: a
         * 63-char run extracts nothing and a 65-char run refuses rather than
         * reading its tail. */
        {
            static const char PIN[] = "0123456789abcdef0123456789abcdef"
                                      "0123456789abcdef0123456789abcdef";
            WCHAR got[68];
            char feed[192];
            int ok, ci;
            Check(sizeof PIN - 1 == 64, "the fixture pin is 64 hex chars");
            sprintf(feed, "{\"sha256\": \"%s\", \"size\": 1}", PIN);
            ok = FindHash64(feed, got);
            if (ok) for (ci = 0; ci < 64; ci++) if (got[ci] != (WCHAR)PIN[ci]) { ok = 0; break; }
            Check(ok && got[64] == 0,
                  "the feed's pin is extracted intact from the middle of a JSON-ish body");
            strcpy(feed, PIN); strcat(feed, "\nmore");
            Check(FindHash64(feed, got) == 1, "a pin at the very start is found (terminated by a non-hex byte)");
            strcpy(feed, "pin="); strcat(feed, PIN);
            Check(FindHash64(feed, got) == 1, "a pin at the very end is found (end-of-string terminator)");
            strcpy(feed, PIN); feed[63] = 0;
            Check(FindHash64(feed, got) == 0, "a 63-char run extracts nothing");
            strcpy(feed, PIN); strcat(feed, "0");
            Check(FindHash64(feed, got) == 0, "a 65-char run is rejected, not read as its first/last 64");
            Check(FindHash64("", got) == 0, "an empty body has no pin");
        }
        /* Deliberately NO click on 117 and no WinHTTP call in tests: the live
         * path performs a real network request and its result opens a real
         * MessageBox - both would disturb the working user. The thread flow
         * is reviewed code, not a hidden-window behavior. */
    }
    /* The settings-window share checkbox and mode radios drive real state with
     * the dialog morphing in place. master=0 keeps every strip hidden. */
    {
        DWORD aff = 0;
        int saved_master = g_s.master, saved_share = -1, saved_mode = -1;
        HKEY key = NULL;
        g_s.master = 0;   /* keep hidden strips hidden: RepaintAll only re-affinitizes */
        {
            WCHAR cls[32];
            Check(g_chk_share && IsWindow(g_chk_share) &&
                  GetClassNameW(g_chk_share, cls, 32) == 6 && lstrcmpW(cls, L"Button") == 0,
                  "share checkbox is a live button control in the settings window");
        }
        SendMessageW(g_chk_share, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(g_dlg, WM_COMMAND, MAKELPARAM(113, BN_CLICKED), (LPARAM)g_chk_share);
        Pump(50);
        if (RegOpenKeyExW(HKEY_CURRENT_USER, scratch, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            saved_share = GetDword(key, L"share", -1); RegCloseKey(key);
        }
        Check(saved_share == 1, "checking the share box persists share=1");
        Check(GetWindowDisplayAffinity(g_ov[0].shwnd[0], &aff) && aff == 0,
              "share=1 removes capture exclusion from the overlay");
        SendMessageW(g_chk_share, BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(g_dlg, WM_COMMAND, MAKELPARAM(113, BN_CLICKED), (LPARAM)g_chk_share);
        Pump(50);
        aff = 0;
        Check(GetWindowDisplayAffinity(g_ov[0].shwnd[0], &aff) && aff == WDA_EXCLUDEFROMCAPTURE,
              "share=0 restores capture exclusion");
        /* Mode radios: switch to e-ink and back without closing the dialog. */
        SetMode(MODE_EINK);
        Check(IsWindow(g_dlg) && g_s.mode == MODE_EINK,
              "switching to e-ink keeps the settings dialog open");
        Check(StyleVisible(g_tb_shades) && !StyleVisible(g_tb_intensity),
              "e-ink rows visible and paper rows hidden after the switch");
        Check(IsDlgButtonChecked(g_dlg, 115) == BST_CHECKED,
              "e-ink radio reflects the mode");
        Check(!IsWindowEnabled(g_chk_share),
              "share checkbox disabled in e-ink (always capture-excluded)");
        saved_mode = -1;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, scratch, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            saved_mode = GetDword(key, L"mode", -1); RegCloseKey(key);
        }
        Check(saved_mode == MODE_EINK, "mode change persists to the registry");
        SetMode(MODE_PAPER);
        Check(IsWindow(g_dlg) && g_s.mode == MODE_PAPER,
              "switching back to paper keeps the dialog open");
        Check(StyleVisible(g_tb_intensity) && !StyleVisible(g_tb_shades),
              "paper rows restored after switching back");
        Check(IsDlgButtonChecked(g_dlg, 114) == BST_CHECKED,
              "paper radio reflects the mode");
        Check(IsWindowEnabled(g_chk_share), "share checkbox re-enabled in paper mode");
        g_s.master = saved_master;
        SyncMasterCheckbox();   /* the product's own mirror, same as SetMaster uses */

        /* The sixth round made leaving e-ink release the ring, but that arm of
         * RepaintAll needs the effect ON (a master-off repaint only stops the
         * worker). Both states are set explicitly rather than relying on the
         * suite's default: allocate the ring with the effect off, then let one
         * paper-mode repaint with the effect on release it. */
        g_s.master = 0;
        SetMode(MODE_EINK);              /* mode change only: master is off, so no repaint */
        EinkBuffersRestart();            /* allocate the ring; the worker stays idle */
        Check(g_ecap[0] != NULL && g_eproc_show != NULL && g_ebuild != NULL,
              "the e-ink ring is allocated for the release check");
        g_s.master = 1;
        SetMode(MODE_PAPER);             /* effect on: RepaintAll's else arm frees the ring */
        Check(g_ecap[0] == NULL && g_eproc_show == NULL && g_ebuild == NULL,
              "leaving e-ink with the effect on releases the e-ink ring");
        g_s.master = saved_master;
        RepaintAll();                    /* settle in the suite's own state */
        SyncMasterCheckbox();
    }
    /* Texture-on checkbox (118) and autostart checkbox (119): same handlers
     * as the tray items, fully isolated from the real Run key. */
    {
        HKEY key = NULL;
        int saved_master = g_s.master, saved_auto = g_s.autostart, v;
        DWORD t = 0, sz = 0;
        Check(IsWindow(GetDlgItem(g_dlg, 118)) && IsWindow(GetDlgItem(g_dlg, 119)),
              "texture-on and start-with-windows checkboxes exist");
        Check((int)IsDlgButtonChecked(g_dlg, 118) == (g_s.master ? BST_CHECKED : BST_UNCHECKED) &&
              (int)IsDlgButtonChecked(g_dlg, 119) == (g_s.autostart ? BST_CHECKED : BST_UNCHECKED),
              "new checkboxes reflect current settings on open");
        /* master toggle: only the HIDE direction runs the real handler - the
         * show direction would make the hidden strips visible on the user's
         * desktop. Restore by state + save instead. */
        SendMessageW(GetDlgItem(g_dlg, 118), BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(g_dlg, WM_COMMAND, MAKELPARAM(118, BN_CLICKED), (LPARAM)GetDlgItem(g_dlg, 118));
        Pump(50);
        Check(g_s.master == 0, "unchecking Texture on hides the veil (master=0)");
        g_s.master = saved_master;
        SendMessageW(GetDlgItem(g_dlg, 118), BM_SETCHECK,
                     saved_master ? BST_CHECKED : BST_UNCHECKED, 0);
        /* autostart: verify the isolated Run key gains and loses the value */
        SendMessageW(GetDlgItem(g_dlg, 119), BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(g_dlg, WM_COMMAND, MAKELPARAM(119, BN_CLICKED), (LPARAM)GetDlgItem(g_dlg, 119));
        Pump(50);
        Check(g_s.autostart == 1, "checking Start with Windows persists autostart=1");
        if (RegOpenKeyExW(HKEY_CURRENT_USER, g_run_key, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            WCHAR path[MAX_PATH] = L"";
            sz = sizeof path;
            Check(RegQueryValueExW(key, L"mnPaper", NULL, &t, (BYTE *)path, &sz) == ERROR_SUCCESS,
                  "Run key carries the mnPaper entry when autostart is on");
            RegCloseKey(key);
        } else Check(0, "scratch Run key exists when autostart is on");
        SendMessageW(GetDlgItem(g_dlg, 119), BM_SETCHECK, BST_UNCHECKED, 0);
        SendMessageW(g_dlg, WM_COMMAND, MAKELPARAM(119, BN_CLICKED), (LPARAM)GetDlgItem(g_dlg, 119));
        Pump(50);
        Check(g_s.autostart == 0, "unchecking Start with Windows persists autostart=0");
        v = -1;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, g_run_key, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            v = RegQueryValueExW(key, L"mnPaper", NULL, &t, NULL, &sz) == ERROR_SUCCESS;
            RegCloseKey(key);
        }
        Check(v == 0, "Run key entry removed when autostart is off");
        /* A copy at another path derives autostart=0 from the installed
         * copy's Run entry, so its SaveSettings must not delete that entry;
         * only an entry naming this exe may be removed. */
        if (RegOpenKeyExW(HKEY_CURRENT_USER, g_run_key, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
            WCHAR foreign[MAX_PATH] = L"C:\\Elsewhere\\mnPaper.exe";
            WCHAR mine[MAX_PATH] = L"", seen[MAX_PATH] = L"";
            RegSetValueExW(key, L"mnPaper", 0, REG_SZ, (BYTE *)foreign,
                           (DWORD)((lstrlenW(foreign) + 1) * sizeof(WCHAR)));
            g_s.autostart = 0;
            SaveSettings();
            sz = sizeof seen;
            Check(RegQueryValueExW(key, L"mnPaper", NULL, &t, (BYTE *)seen, &sz) == ERROR_SUCCESS &&
                  _wcsicmp(seen, foreign) == 0,
                  "a copy at another path leaves a foreign Run entry in place");
            GetModuleFileNameW(NULL, mine, MAX_PATH);
            RegSetValueExW(key, L"mnPaper", 0, REG_SZ, (BYTE *)mine,
                           (DWORD)((lstrlenW(mine) + 1) * sizeof(WCHAR)));
            g_s.autostart = 0;
            SaveSettings();
            sz = sizeof seen;
            Check(RegQueryValueExW(key, L"mnPaper", NULL, &t, NULL, &sz) != ERROR_SUCCESS,
                  "an entry naming this exe is removed when autostart is off");
            RegCloseKey(key);
        } else Check(0, "scratch Run key writable for the ownership check");
        g_s.master = saved_master; g_s.autostart = saved_auto;
        SaveSettings();   /* restore isolated-key state to pre-test values */
    }
    PaperWorkerStop();
    Pump(5);
    DestroyWindow(g_dlg);
    for (i = 0; i < g_n; i++) DestroyOverlay(&g_ov[i]);
    DestroyWindow(g_host);
    RegDeleteTreeW(HKEY_CURRENT_USER, scratch);
    g_s = before;
    printf("RESULT %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
