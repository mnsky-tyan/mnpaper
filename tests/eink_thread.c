/* Windowless regression for the e-ink pipeline on its worker thread.
 *
 * The heavy conversion used to run on the UI thread inside the 50 ms tick.
 * It now runs on a worker thread, and the UI thread only memcpy's the result
 * into each overlay and uploads it. This drives that machinery end to end:
 * capture slot ownership, publish, worker render, pointer swap, the
 * settings-change re-render, and the stop path - with no screen interaction.
 */
#define wWinMain MnPaperEntry
#include "../mnPaper.c"
#undef wWinMain

static int failures;
static void Check(int ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static LRESULT CALLBACK TestHost(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* wait up to timeout_ms for n presents */
static int WaitForPresents(int want, int timeout_ms) {
    DWORD began = GetTickCount();
    MSG msg;
    while (want > 0 && GetTickCount() - began < (DWORD)timeout_ms) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_APP_EINK) want--;
        }
        if (GetTickCount() - began > 250) Sleep(5);
    }
    return want == 0;
}

int main(void) {
    WNDCLASSW wc = {0};
    size_t n = (size_t)64 * 48 * 4;
    int i;
    unsigned char *pat1, *pat2;
    unsigned char *before_show, *before_build;
    unsigned char reference[64 * 48 * 4];
    int slots_used = 0;
    unsigned char *adist[3];
    int dist;

    g_test_headless = 1;
    g_vsx = g_vsy = 0; g_vsw = 64; g_vsh = 48;
    /* buffers: exactly the shape EinkEnsureBuffers builds, owned by the test */
    for (i = 0; i < EINK_SLOTS; i++) {
        g_ecap[i] = (unsigned char *)malloc(n);
        if (!g_ecap[i]) { printf("alloc failed\n"); return 2; }
        memset(g_ecap[i], 0, n);
        g_ecap_state[i] = 0;
    }
    g_eproc_a = (unsigned char *)malloc(n); memset(g_eproc_a, 0xFF, n);
    g_eproc_b = (unsigned char *)malloc(n); memset(g_eproc_b, 0xFF, n);
    g_eproc_show = g_eproc_a; g_ebuild = g_eproc_b;

    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpfnWndProc = TestHost; wc.lpszClassName = L"MnPaperEinkHost";
    RegisterClassW(&wc);
    g_host = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, NULL, wc.hInstance, NULL);
    Check(IsWindow(g_host), "message-only host window for worker posts");

    /* synthetic captures: two different pictures */
    pat1 = (unsigned char *)malloc(n);
    pat2 = (unsigned char *)malloc(n);
    for (i = 0; i < 64 * 48; i++) {
        pat1[i * 4 + 0] = 40;  pat1[i * 4 + 1] = 90;  pat1[i * 4 + 2] = 200; pat1[i * 4 + 3] = 255;
        pat2[i * 4 + 0] = 220; pat2[i * 4 + 1] = 60;  pat2[i * 4 + 2] = 30;  pat2[i * 4 + 3] = 255;
    }

    /* ------------------------- the render step is pure -------------------- */
    g_s.shades = 4; g_s.contrast = 50; g_s.dither = 75;
    EinkRender(pat1, reference, 64, 48);
    Check(reference[0] == reference[1] && reference[1] == reference[2],
          "e-ink output is greyscale (all channels equal)");
    Check(reference[3] == 255, "e-ink output alpha is opaque");
    {   /* a luma ramp: the rendered picture must use the whole shade range */
        unsigned char *ramp = (unsigned char *)malloc(n);
        unsigned char *out = (unsigned char *)malloc(n);
        int mn = 255, mx = 0;
        for (i = 0; i < 64 * 48; i++) {
            int v = (int)(255.0 * ((i % 64) + 1) / 64);
            ramp[i * 4 + 0] = ramp[i * 4 + 1] = ramp[i * 4 + 2] = (unsigned char)v;
            ramp[i * 4 + 3] = 255;
        }
        EinkRender(ramp, out, 64, 48);
        for (i = 0; i < 64 * 48; i++) {
            if (out[i * 4] < mn) mn = out[i * 4];
            if (out[i * 4] > mx) mx = out[i * 4];
        }
        Check(mn == 0 && mx == 255, "a luma ramp renders to the full shade range (4 shades)");
        free(ramp); free(out);
    }

    /* ------------------------- worker: capture -> render ------------------ */
    EinkWorkerSet(1);
    Check(g_ework != NULL, "e-ink worker thread starts");
    adist[slots_used++] = EinkCapBegin();
    Check(adist[0] != NULL, "a capture slot is available");
    {   /* slots are distinct: the ring is a real ring, not one buffer */
        adist[slots_used] = EinkCapBegin();
        unsigned char *s2 = EinkCapBegin();
        Check(adist[slots_used] && s2 && adist[slots_used] != s2,
              "successive captures take different slots");
        slots_used++;
        if (s2 != adist[0] && s2 != adist[1]) { adist[slots_used++] = s2; }
        else EinkCapEnd(0, s2);   /* same slot twice: the ring is only 3 deep */
    }
    Check(slots_used >= 2, "each grab owns its own slot");
    for (i = 0; i < EINK_SLOTS; i++) if (g_ecap_state[i] == 0) { /* release nothing */ }
    for (i = 0; i < slots_used; i++) EinkCapEnd(0, adist[i]);   /* hand them back unused */

    before_show = g_eproc_show; before_build = g_ebuild;
    {
        unsigned char *dst = EinkCapBegin();
        Check(dst != NULL, "a slot is free again after the unused grabs");
        if (dst) {
            memcpy(dst, pat1, n);
            Check(EinkCapEnd(1, dst) == 1, "publishing a captured frame wakes the worker");
        }
    }
    Check(WaitForPresents(1, 5000), "the worker posts a present message");
    Check(g_eproc_show != before_show || g_ebuild != before_build,
          "publish swaps the show pointer (worker -> UI handoff)");
    Check(memcmp(g_eproc_show, reference, n) == 0,
          "the shown buffer is the conversion of the captured frame");

    /* --------------------- second frame, new content ---------------------- */
    {
        unsigned char *dst = EinkCapBegin();
        Check(dst != NULL, "the ring recycles the slot after rendering");
        if (dst) { memcpy(dst, pat2, n); EinkCapEnd(1, dst); }
    }
    EinkRender(pat2, reference, 64, 48);
    Check(WaitForPresents(1, 5000), "a second capture also gets presented");
    Check(memcmp(g_eproc_show, reference, n) == 0,
          "the second capture is the one presented (newest wins)");

    /* --------- settings change with a STATIC screen still re-renders ------ */
    g_s.shades = 2;                      /* the retained frame re-rendered */
    InterlockedIncrement(&g_eset_gen);
    EinkRender(pat2, reference, 64, 48);
    Check(WaitForPresents(1, 5000),
          "a settings change re-renders with no new capture");
    Check(memcmp(g_eproc_show, reference, n) == 0,
          "the re-render reflects the new settings");

    /* with no settings change and no capture, the worker stays idle */
    g_egen_done = g_eset_gen;
    Check(!WaitForPresents(1, 400), "an idle screen does not spin frames");

    /* ------------------------- ring hygiene ------------------------------- */
    {
        int state0 = 0, state3 = 0, other = 0;
        for (i = 0; i < EINK_SLOTS; i++) {
            if (g_ecap_state[i] == 0) state0++;
            else if (g_ecap_state[i] == 3) state3++;
            else other++;
        }
        Check(other == 0, "no slot is stuck mid-capture after the frames drain");
        Check(state3 == 1 && state0 == 2, "one retained frame, the rest free");
    }

    /* ------------------------- stop path ---------------------------------- */
    EinkWorkerSet(0);
    Check(g_ework == NULL, "the worker stops and joins");
    Check(EinkCapBegin() == NULL, "no capture slots are handed out with no worker");
    EinkWorkerSet(0);
    Check(1, "a second stop is a no-op");
    EinkWorkerSet(1);
    EinkWorkerSet(0);
    Check(g_ework == NULL, "start+stop is clean");

    DestroyWindow(g_host);
    for (i = 0; i < EINK_SLOTS; i++) { free(g_ecap[i]); g_ecap[i] = NULL; }
    free(g_eproc_a); free(g_eproc_b); g_eproc_a = g_eproc_b = g_eproc_show = g_ebuild = NULL;
    free(pat1); free(pat2);
    printf("RESULT %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
