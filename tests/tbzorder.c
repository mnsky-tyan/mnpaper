/* The two taskbar modes are pure z-order as of 2.7.19: Not cover raises a
 * revealed taskbar above the veil; Cover leaves it beneath (the veil's
 * strips are click-through, so the bar keeps working under the paper). No
 * transparency is punched in either mode - the hole era is gone - so the
 * texture bytes must be identical whatever the bar does. This suite drives
 * the real classifier, the real z-order pass on invisible off-screen fake
 * taskbars, and the upload path, on a fixed 2880x1800 geometry (the
 * reference desktop this suite was written against - nothing here queries
 * the live monitors). Windowless: no window is ever shown. */
#define _CRT_SECURE_NO_WARNINGS 1
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define wWinMain mnPaper_unused_wWinMain
#include "../mnPaper.c"
#undef wWinMain

static int fails, checks;
static void Check(int c, const char *what) {
    checks++;
    printf("  %s  %s\n", c ? "PASS" : "FAIL", what);
    if (!c) fails++;
}
/* shared full-byte hash (tests/fnv64.h); H kept as the file's short name */
#include "fnv64.h"
#define H PixelHashAll

/* rank of a window in the top-level z-order (0 = top), -1 if not found */
static int Zrank(HWND target) {
    HWND h = GetTopWindow(NULL);
    int n = 0;
    while (h && n < 4096) {
        if (h == target) return n;
        n++;
        h = GetWindow(h, GW_HWNDNEXT);
    }
    return -1;
}

static int IsTopmost(HWND h) {
    return (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

int main(void) {
    RECT mon = { 0, 0, 2880, 1800 };          /* fixed reference geometry */
    int w = mon.right - mon.left, h = mon.bottom - mon.top;
    int band_y = 1800 - 48;                    /* inside the taskbar band */
    OVL ov;
    unsigned char *px;
    unsigned long long clean;
    int i;

    g_test_headless = 1;   /* same safety pin as the other suites */
    {
        /* one registration for every section below: the z-order pair, the cap
         * probe and the five-fake chain all need a process-local
         * Shell_TrayWnd class (what IsTaskbarWnd matches). Re-registering in
         * a second scope fails with ERROR_CLASS_ALREADY_EXISTS and would
         * silently disable those sections. */
        WNDCLASSW fc = {0};   /* zeroed: an uninitialised hbrBackground is a garbage brush the fake windows paint with */
        fc.hInstance = GetModuleHandleW(NULL);
        fc.lpfnWndProc = DefWindowProcW;
        fc.lpszClassName = L"Shell_TrayWnd";
        RegisterClassW(&fc);
    }
    printf("taskbar z-order modes on a %dx%d monitor (fixed reference geometry)\n", w, h);
    memset(&ov, 0, sizeof ov);
    ov.idx = 0; ov.w = w; ov.h = h; ov.rc = mon;
    px = (unsigned char *)malloc((size_t)w * h * 4);
    if (!px) { printf("RESULT 1 failure(s)\n"); return 1; }
    ov.bits = px;
    g_s = (SETTINGS){ .master = 1, .mode = MODE_PAPER, .intensity = 40,
                      .warmth = 80, .grain = 4, .fibre = 40, .blotch = 26,
                      .shades = 4, .contrast = 50, .dither = 75,
                      .autostart = 0, .share = 0, .tbmode = TB_NOT_COVER, .autoupd = 1 };
    BuildPaper(&ov);
    clean = H(px, (size_t)w * h * 4);
    Check(px[(size_t)band_y * w * 4 + 1400 * 4 + 3] != 0,
          "the band carries real paper alpha (the texture is never cut open)");

    /* --------------- the parked/revealed classifier ------------------
     * TbParked (2026-10-06 review): an auto-hidden taskbar leaves a short
     * band when docked to the bottom, but a full-height few-pixel-WIDE
     * strip when docked left or right. The old height-only test read the
     * side docks as revealed, which misclassified a parked side taskbar as
     * revealed: a pointless topmost raise on every tick and the Not cover
     * walk skipped for as long as it stays misclassified. Drive the
     * classifier directly on all three geometries plus the threshold. */
    {
        RECT r;
        SetRect(&r, mon.left, 1752, mon.right, mon.bottom);
        Check(!TbParked(&r), "a revealed bottom taskbar band is not parked");
        SetRect(&r, 0, 0, 6, 1800);
        Check(TbParked(&r), "a left-docked sliver is parked (full height, 6px wide)");
        SetRect(&r, 2874, 0, 2880, 1800);
        Check(TbParked(&r), "a right-docked sliver is parked (full height, 6px wide)");
        SetRect(&r, 0, 1784, 2880, 1800);
        Check(TbParked(&r), "a 16px bottom sliver counts as parked (the stated threshold)");
    }

    /* ClassifyTaskbars fills g_tb_state from the collected rects against
     * the overlay table: 2 revealed, 1 parked, 0 when the taskbar matches
     * no overlay at all (a monitor without a veil must not invent a
     * classification for a bar it never clips). */
    {
        g_n = 1;
        g_ov[0].rc = mon;
        g_ov[0].idx = 0;
        g_ntb = 1;

        SetRect(&g_tb[0], mon.left, 1752, mon.right, mon.bottom);   /* revealed bottom band */
        ClassifyTaskbars();
        Check(g_tb_state[0] == 2, "a revealed bottom band classifies as revealed");

        SetRect(&g_tb[0], 0, 0, 6, 1800);                           /* parked, docked LEFT */
        ClassifyTaskbars();
        Check(g_tb_state[0] == 1, "a left-docked parked sliver classifies as parked");

        SetRect(&g_tb[0], 2874, 0, 2880, 1800);                     /* parked, docked RIGHT */
        ClassifyTaskbars();
        Check(g_tb_state[0] == 1, "a right-docked parked sliver classifies as parked");

        SetRect(&g_tb[0], mon.right + 400, mon.top, mon.right + 500, mon.bottom);
        g_tb_state[0] = 0;   /* what CollectTaskbars' memset leaves: unknown */
        ClassifyTaskbars();
        Check(g_tb_state[0] == 0,
              "a taskbar on a monitor without a veil stays unclassified (0)");

        g_ntb = 0;
    }

    /* --------------- the two taskbar modes are z-order only ----------
     * Every sub-check drives the REAL production pass with the state
     * Housekeeping would latch, on a FRESH pair of real (invisible,
     * off-screen) windows: a fake taskbar created first, a reference window
     * created after it (so the reference starts above). The raise sets the
     * topmost bit permanently - nothing in production ever demotes - so a
     * pair that was already raised can never again show a non-raise; each
     * sub-check needs its own never-touched pair. Not cover: a revealed bar
     * is raised and gets the topmost bit, a parked bar is left alone.
     * Cover: nothing is raised either way - the veil's strips are the ones
     * that sit above the bar. */
    {
        WNDCLASSW rc_ = {0};
        rc_.hInstance = GetModuleHandleW(NULL);
        rc_.lpfnWndProc = DefWindowProcW;
        rc_.lpszClassName = L"MnTbZRef";
        if (RegisterClassW(&rc_)) {
            HWND fake, ref;
            g_n = 1; g_ov[0].rc = mon; g_ntb = 1;
            g_s.tbmode = TB_NOT_COVER;

            /* Not cover, revealed: the raise must put the fake above the
             * reference and set its topmost bit */
            fake = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"Shell_TrayWnd",
                                    L"t", WS_POPUP, -32000, -32000, 30, 30,
                                    NULL, NULL, rc_.hInstance, NULL);
            ref = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"MnTbZRef",
                                  L"r", WS_POPUP, -31900, -31900, 30, 30,
                                  NULL, NULL, rc_.hInstance, NULL);
            Check(fake && ref, "fake taskbar and reference windows created (invisible, off-screen)");
            if (fake && ref) {
                Check(Zrank(ref) < Zrank(fake), "the reference starts above the fake (arrangement sane)");
                g_tbw[0] = fake; g_tb_state[0] = 2;   /* revealed, as Housekeeping latches */
                TaskbarZorder();
                Check(Zrank(fake) >= 0 && Zrank(fake) < Zrank(ref),
                      "Not cover raises a revealed taskbar above later windows");
                Check(IsTopmost(fake), "Not cover sets the raised bar's topmost bit");
                Check((GetWindowLongW(ref, GWL_EXSTYLE) & WS_EX_TOPMOST) == 0,
                      "the raise touches only the taskbar window, never bystanders");
            }
            if (fake) DestroyWindow(fake);
            if (ref) DestroyWindow(ref);

            /* Not cover, parked: a fresh pair, nothing raised. A parked
             * sliver must be left exactly where it is - no bit, no move. */
            fake = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"Shell_TrayWnd",
                                    L"t", WS_POPUP, -32000, -32000, 30, 30,
                                    NULL, NULL, rc_.hInstance, NULL);
            ref = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"MnTbZRef",
                                  L"r", WS_POPUP, -31900, -31900, 30, 30,
                                  NULL, NULL, rc_.hInstance, NULL);
            if (fake && ref) {
                g_tbw[0] = fake; g_tb_state[0] = 1;   /* parked sliver */
                TaskbarZorder();
                Check(Zrank(ref) < Zrank(fake),
                      "Not cover leaves a parked sliver where it is (no move)");
                Check(!IsTopmost(fake), "Not cover never raises a parked sliver");
            } else {
                Check(0, "parked-sliver pair could be created");
            }
            if (fake) DestroyWindow(fake);
            if (ref) DestroyWindow(ref);

            /* Cover, revealed: a fresh pair, nothing raised. The bar stays
             * where the shell keeps it - under the veil, above nothing new. */
            fake = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"Shell_TrayWnd",
                                    L"t", WS_POPUP, -32000, -32000, 30, 30,
                                    NULL, NULL, rc_.hInstance, NULL);
            ref = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"MnTbZRef",
                                  L"r", WS_POPUP, -31900, -31900, 30, 30,
                                  NULL, NULL, rc_.hInstance, NULL);
            if (fake && ref) {
                g_s.tbmode = TB_COVER;
                g_tbw[0] = fake; g_tb_state[0] = 2;   /* revealed, as Housekeeping latches */
                TaskbarZorder();
                Check(Zrank(ref) < Zrank(fake),
                      "Cover raises nothing: the bar stays where the shell keeps it");
                Check(!IsTopmost(fake), "Cover sets no topmost bit on the bar");
            } else {
                Check(0, "Cover pair could be created");
            }
            if (fake) DestroyWindow(fake);
            if (ref) DestroyWindow(ref);
            g_ntb = 0;   /* leave no synthesized taskbar behind */
            g_s.tbmode = TB_NOT_COVER;
        } else {
            Check(0, "the reference window class could be registered");
        }
    }

    /* --------------- no mode ever touches the texture ----------------
     * The 2.7.19 contract: the modes differ in z-order only, so an upload
     * around a latched revealed taskbar leaves the master texture
     * byte-identical in BOTH modes, and the band keeps full paper alpha
     * (the punched hole that showed the desktop's white frame is gone).
     * This scratch OVL has no strips, so ApplyLayered's upload loop no-ops
     * (it cannot fail) and the byte-identity hashes are the real checks -
     * exactly the path where the 2.7.7 regression lived. */
    {
        g_n = 1;
        g_ov[0].rc = mon;
        g_ov[0].idx = 0;
        g_ntb = 1;
        g_tbw[0] = NULL;          /* no taskbar window: the raise is skipped */
        g_tb_state[0] = 2;        /* revealed, latched as Housekeeping would */

        g_s.tbmode = TB_NOT_COVER;
        Check(ApplyLayered(&ov), "Not cover: the upload for a revealed taskbar succeeds");
        Check(H(px, (size_t)w * h * 4) == clean,
              "Not cover: the upload leaves the master texture byte-identical");
        Check(px[(size_t)band_y * w * 4 + 1400 * 4 + 3] != 0,
              "Not cover: the band still carries paper alpha (nothing is punched)");

        g_s.tbmode = TB_COVER;
        Check(ApplyLayered(&ov), "Cover: the upload succeeds");
        Check(H(px, (size_t)w * h * 4) == clean,
              "Cover: the upload leaves the master texture byte-identical");
        Check(px[(size_t)band_y * w * 4 + 1400 * 4 + 3] != 0,
              "Cover: the band still carries paper alpha (nothing is punched)");

        /* and the cycle is repeatable: 20 reveal/park round trips through
         * both modes leave no residue of any kind */
        for (i = 0; i < 20; i++) {
            g_tb_state[0] = 2; g_s.tbmode = (i & 1) ? TB_COVER : TB_NOT_COVER;
            ApplyLayered(&ov);
            g_tb_state[0] = 1; ApplyLayered(&ov);
        }
        Check(H(px, (size_t)w * h * 4) == clean,
              "20 reveal/park cycles across both modes leave the texture untouched");

        g_ntb = 0;                /* leave no synthesized taskbar behind */
    }

    /* ------------- taskbar slots cover every monitor ----------------
     * TB_MAX used to be 4 while MAX_MON was 16, so a desk with 5+ displays
     * each showing a taskbar silently dropped taskbars 5..N: TbEnumProc
     * stopped enumerating, no raise happened, and the veil painted straight
     * over them with nothing in the log (2026-10-08 round-10 review). The
     * capacity fix is TB_MAX = MAX_MON; what is asserted here is the half
     * that can regress quietly - the cap is not below the monitor cap, and
     * hitting the cap is reported rather than silent. */
    Check(TB_MAX >= MAX_MON,
          "taskbar slots cover every monitor the veil can have (TB_MAX >= MAX_MON)");

    /* ------------- the cap, end to end, with its log line -------------
     * Real production chain on real windows: fake taskbars whose class the
     * product recognises -> TbEnumProc -> the cap. The fakes are parked
     * outside every virtual screen and are destroyed before the suite
     * returns, so nothing paints for the user. */
    {
        HWND fake;
        char buf[512];
        BOOL r;
        fake = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"Shell_TrayWnd",
                               L"t", WS_POPUP | WS_VISIBLE, -32000, -32000, 30, 30,
                               NULL, NULL, GetModuleHandleW(NULL), NULL);
        Check(fake != NULL, "a fake taskbar window could be created");
        if (fake) {
            /* the table exactly full: TbEnumProc's only FALSE path is the cap,
             * and since the cap is tested after the visibility and taskbar
             * filters, a FALSE here additionally proves fake passed them as a
             * real taskbar (the product logs a refusal only for a taskbar it
             * actually refuses) */
            g_ntb = TB_MAX;
            {
                FILE *f = fopen("tbzorder_tape.log", "w");
                Check(f != NULL, "cap log file could be opened");
                if (f) {
                    g_log = f;
                    r = TbEnumProc(fake, 0);
                    g_log = NULL;
                    fflush(f); fclose(f);
                    f = fopen("tbzorder_tape.log", "r");
                    memset(buf, 0, sizeof buf);
                    if (f) { fread(buf, 1, sizeof buf - 1, f); fclose(f); }
                    DeleteFileA("tbzorder_tape.log");   /* no residue */
                    printf("  .. log: %s\n", buf[0] ? buf : "(empty)");
                    Check(r == FALSE, "a taskbar past the cap stops the enumeration (nothing is stored)");
                    Check(g_ntb == TB_MAX, "the cap is not overrun: no slot is written past the end");
                    Check(buf[0] != '\0',
                          "hitting the taskbar cap writes a log line (a dropped taskbar is diagnosable)");
                    Check(strstr(buf, "TB_MAX") != NULL,
                          "the cap log identifies the bound it stopped at (TB_MAX)");
                    /* the wording is contractual, not incidental: the ninth-round
                     * lesson is that a silent cap is the bug, so the line must say
                     * what is lost, not just which bound it stopped at */
                    Check(strstr(buf, "no slot") != NULL,
                          "the cap log says what is lost, not just that a bound was reached");
                }
            }
            /* a full table must not make ordinary windows trigger the claim:
             * the cap sits behind the taskbar filters now, so a non-taskbar
             * window enumerated on a full table passes through with no FALSE
             * and no log (it used to be the first statement, so ANY window
             * after the fill logged the refusal) */
            {
                WNDCLASSW pc = {0};
                HWND plain;
                char pbuf[512];
                BOOL pr;
                pc.hInstance = GetModuleHandleW(NULL);
                pc.lpfnWndProc = DefWindowProcW;
                pc.lpszClassName = L"MnTbZPlainWnd";
                plain = RegisterClassW(&pc)
                      ? CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"MnTbZPlainWnd",
                                        L"p", WS_POPUP | WS_VISIBLE, -31900, -31900, 20, 20,
                                        NULL, NULL, pc.hInstance, NULL)
                      : NULL;
                Check(plain != NULL, "a plain non-taskbar window could be created");
                g_ntb = TB_MAX;   /* table full: the case the old order got wrong */
                if (plain) {
                    FILE *pf = fopen("tbzorder_tape.log", "w");
                    if (pf) {
                        g_log = pf;
                        pr = TbEnumProc(plain, 0);
                        g_log = NULL;
                        fflush(pf); fclose(pf);
                        pf = fopen("tbzorder_tape.log", "r");
                        memset(pbuf, 0, sizeof pbuf);
                        if (pf) { fread(pbuf, 1, sizeof pbuf - 1, pf); fclose(pf); }
                        DeleteFileA("tbzorder_tape.log");   /* no residue */
                        Check(pr == TRUE,
                              "a non-taskbar window on a full table is passed through, not refused");
                        Check(pbuf[0] == '\0',
                              "no cap line is logged for a window that is not a taskbar");
                    }
                    DestroyWindow(plain);
                }
                g_ntb = 0;
            }
            DestroyWindow(fake);
        }
    }

    /* ------------- the real enumeration chain on five fakes --------------
     * Drives the REAL production walk: EnumWindows -> TbEnumProc ->
     * CollectTaskbars -> ClassifyTaskbars. Five fakes is one past the old
     * TB_MAX of 4, so with that value back the fifth never enters g_tb and
     * this fails on the count - exactly the reported symptom (taskbars 5..N
     * textured over, nothing logged) rather than on a proxy for it. The
     * fakes are visible-but-parked outside every virtual screen (the walk's
     * IsWindowVisible filter must pass them) and are destroyed before the
     * suite returns, so nothing paints for the user. No z-order pass runs
     * here: TaskbarZorder is skipped, so nothing is raised anywhere. */
    {
        HWND fake[5];
        int made = 0, k, found = 0;
        RECT mon5 = { -32000, -32000, -31000, -31000 };   /* covers the fakes */
        for (k = 0; k < 5; k++) {
            fake[k] = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                      L"Shell_TrayWnd", L"t",
                                      WS_POPUP | WS_VISIBLE,
                                      -32000 + k * 40, -32000, 30, 30,
                                      NULL, NULL, GetModuleHandleW(NULL), NULL);
            if (fake[k]) made++;
        }
        if (made == 5) {
            g_n = 1;
            g_ov[0].rc = mon5; g_ov[0].idx = 0;
            CollectTaskbars();
            ClassifyTaskbars();
            for (k = 0; k < 5; k++) {
                int j;
                for (j = 0; j < g_ntb; j++)
                    if (g_tb[j].left == -32000 + k * 40 && g_tb[j].top == -32000) {
                        found++;
                        if (g_tb_state[j] != 2)
                            printf("  .. fake %d classified %d, expected 2 (revealed)\n",
                                   k, g_tb_state[j]);
                        break;
                    }
            }
            printf("  .. %d of 5 fake taskbars collected (g_ntb=%d)\n", found, g_ntb);
            Check(found == 5,
                  "all five taskbars survive enumeration (the old TB_MAX=4 dropped the fifth)");
            {
                int revealed_ok = 1;
                for (k = 0; k < g_ntb; k++)
                    if (g_tb[k].top == -32000 && g_tb[k].bottom <= -31930 &&
                        g_tb_state[k] != 2) revealed_ok = 0;
                Check(revealed_ok,
                      "each collected fake classifies as revealed (30x30 band, not a sliver)");
            }
        } else {
            printf("  NOTE could not create five fake taskbar windows (%d made) - "
                   "the end-to-end half of this check is skipped\n", made);
        }
        for (k = 0; k < 5; k++) if (k < made && fake[k]) DestroyWindow(fake[k]);
        g_n = 1;
        g_ov[0].rc = mon;          /* restore the suite's reference geometry */
        g_ntb = 0;                /* leave no synthesized taskbar behind */
    }

    /* ------------- the monitor cap is logged, like its siblings -------------
     * MonRectCb is the one round-10 "silent cap" that no suite pinned:
     * TB_MAX's log wording is asserted above, MAX_STRIPS's in tall_coverage.
     * It never dereferences the monitor handle, so it drives headlessly the
     * same way TbEnumProc does. */
    {
        FILE *f = fopen("tbzorder_moncap.log", "w");
        RECT r = { 100, 100, 200, 200 };
        int saved_nmi = g_nmi, r1, r2;
        char buf[512];
        Check(f != NULL, "moncap log file could be opened");
        if (f) {
            g_log = f;
            g_nmi = MAX_MON;
            r1 = MonRectCb(NULL, NULL, &r, 0);   /* table full: cap hits */
            g_nmi = MAX_MON - 1;
            r2 = MonRectCb(NULL, NULL, &r, 0);   /* one slot free: stored */
            g_log = NULL;
            fflush(f); fclose(f);
            f = fopen("tbzorder_moncap.log", "r");
            memset(buf, 0, sizeof buf);
            if (f) { fread(buf, 1, sizeof buf - 1, f); fclose(f); }
            DeleteFileA("tbzorder_moncap.log");   /* no residue */
            Check(r1 == FALSE, "a monitor past the cap stops the enumeration");
            Check(r2 == TRUE && g_nmi == MAX_MON,
                  "a monitor with one slot free is stored (the cap is not off by one)");
            Check(strstr(buf, "MAX_MON") != NULL && strstr(buf, "no veil") != NULL,
                  "hitting the monitor cap logs what is lost (the veil), like the sibling caps");
        }
        g_nmi = saved_nmi;
    }

    free(px);
    printf("\nRESULT %d failure(s) across %d checks\n", fails, checks);
    return fails ? 1 : 0;
}
