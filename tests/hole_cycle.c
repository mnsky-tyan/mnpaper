/* Reproduces the reported symptom end to end at a fixed 2880x1800 geometry
 * (the reference desktop this suite was written against - nothing here
 * queries the live monitors): reveal the taskbar (hole armed), upload, park
 * it (hole closed), upload again with no rebuild, and assert the bottom band
 * renders paper again. Windowless: only the texture math is used. */
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

int main(void) {
    RECT mon = { 0, 0, 2880, 1800 };          /* fixed reference geometry */
    int w = mon.right - mon.left, h = mon.bottom - mon.top;
    OVL ov;
    unsigned char *px;
    unsigned long long clean, revealed;
    int band_y = 1800 - 48;                    /* inside the taskbar band */
    int i;

    g_test_headless = 1;   /* same safety pin as the other suites */
    printf("taskbar hole cycle on a %dx%d monitor (fixed reference geometry)\n", w, h);
    memset(&ov, 0, sizeof ov);
    ov.idx = 0; ov.w = w; ov.h = h; ov.rc = mon;
    px = (unsigned char *)malloc((size_t)w * h * 4);
    if (!px) { printf("RESULT 1 failure(s)\n"); return 1; }
    ov.bits = px;
    g_s = (SETTINGS){ .master = 1, .mode = MODE_PAPER, .intensity = 40,
                      .warmth = 80, .grain = 4, .fibre = 40, .blotch = 26,
                      .shades = 4, .contrast = 50, .dither = 75,
                      .autostart = 0, .share = 0, .autoupd = 1 };
    BuildPaper(&ov);
    clean = H(px, (size_t)w * h * 4);
    Check(px[(size_t)band_y * w * 4 + 1400 * 4 + 3] != 0,
          "with the taskbar parked the band carries real paper alpha");

    /* taskbar revealed: the hole opens over the bottom band */
    g_hole_on[0] = 1;
    SetRect(&g_hole[0], mon.left, 1752, mon.right, mon.bottom);
    /* this scratch OVL has no strips, so ApplyLayered runs the hole
     * save/punch/restore around a no-op upload (it cannot fail - the
     * return value is not assertable here; the byte-identity hashes below
     * are the real checks). That path is exactly where the regression that
     * 2.7.7 introduced and 2.7.8 fixed lived. */
    ApplyLayered(&ov);
    revealed = H(px, (size_t)w * h * 4);
    Check(revealed == clean, "the upload for a revealed taskbar changes no master pixel");

    /* taskbar parks: the hole closes, Housekeeping re-uploads with NO rebuild */
    g_hole_on[0] = 0;
    ApplyLayered(&ov);
    Check(H(px, (size_t)w * h * 4) == clean,
          "after the taskbar parks the texture is byte-identical to the clean one");
    {
        int holes = 0;
        for (i = 0; i < 40; i++) {
            int y = mon.bottom - 2 - i, x = 200 + i * 30;
            if (px[(size_t)y * w * 4 + x * 4 + 3] == 0) holes++;
        }
        Check(holes == 0, "no zero-alpha pixel is left in the parked-taskbar band");
    }
    /* and the cycle is repeatable: 20 reveal/park round trips leave no residue */
    for (i = 0; i < 20; i++) {
        g_hole_on[0] = 1; ApplyLayered(&ov);
        g_hole_on[0] = 0; ApplyLayered(&ov);
    }
    Check(H(px, (size_t)w * h * 4) == clean, "20 reveal/park cycles leave the texture untouched");

    /* ComputeHoles' parked/revealed decision (TbParked, 2026-10-06 review): an
     * auto-hidden taskbar leaves a short band when docked to the bottom, but a
     * full-height few-pixel-WIDE strip when docked left or right. The old
     * height-only test read the side docks as revealed, which left a permanent
     * see-through band down that edge and latched the z-order walk off. Drive
     * the classifier directly on all three geometries. */
    {
        g_n = 1;
        g_ov[0].rc = mon;
        g_ntb = 1;
        g_tbw[0] = NULL;          /* no taskbar window: the raise is skipped */
        g_hole_on[0] = 0; g_hole_dirty = 0;

        SetRect(&g_tb[0], mon.left, 1752, mon.right, mon.bottom);   /* revealed bottom band */
        ComputeHoles();
        Check(g_tb_state[0] == 2 && g_hole_on[0] == 1 &&
              g_hole[0].bottom == 1800 && g_hole[0].top == 1752,
              "a revealed bottom taskbar band opens the hole");

        SetRect(&g_tb[0], 0, 0, 6, 1800);                            /* parked, docked LEFT */
        ComputeHoles();
        Check(g_tb_state[0] == 1 && g_hole_on[0] == 0,
              "a left-docked parked sliver is parked, never a hole (full height, 6px wide)");

        SetRect(&g_tb[0], 2874, 0, 2880, 1800);                      /* parked, docked RIGHT */
        ComputeHoles();
        Check(g_tb_state[0] == 1 && g_hole_on[0] == 0,
              "a right-docked parked sliver is parked (full height, 6px wide)");

        SetRect(&g_tb[0], 0, 1784, 2880, 1800);                      /* 16px: the parked threshold */
        ComputeHoles();
        Check(g_tb_state[0] == 1 && g_hole_on[0] == 0,
              "a 16px bottom sliver counts as parked (the stated threshold)");

        g_ntb = 0;                /* leave no synthesized taskbar behind */
        g_hole_on[0] = 0;
    }

    /* --------------- clipping at the hole/hole's-bounds boundary -----------
     * Round-10 review extracted the four hand-written "clip a RECT into a
     * bound" chains into ClipRect(). The equivalence that matters is not the
     * helper's source but what the two pure-logic call sites now do with an
     * overhanging rect, so drive them: an off-screen taskbar band through
     * ComputeHoles (clip into the monitor), and an off-screen hole through
     * LocalHole / ClearHoleAlpha (clip into the overlay). Both are the real
     * production functions, and both fail if ClipRect's semantics drift from
     * the clamp chains it replaced. */
    {
        RECT band = { mon.left - 500, mon.top - 300, mon.right + 700, mon.bottom + 400 };
        unsigned char *saved = NULL;
        size_t saved_len = 0;
        int rx0, ry0, rx1, ry1, x, y, clipped_ok;

        g_n = 1;
        g_ov[0].rc = mon;
        g_ntb = 1;
        g_tbw[0] = NULL;
        g_hole_on[0] = 0; g_hole_dirty = 0;
        g_tb[0] = band;                       /* overhangs the monitor on all four sides */
        ComputeHoles();
        Check(g_ov[0].rc.left == 0 && g_ov[0].rc.right == 2880 &&
              g_ov[0].rc.top == 0 && g_ov[0].rc.bottom == 1800,
              "the boundary case really does overhang the reference monitor");
        Check(g_hole_on[0] == 1 &&
              g_hole[0].left == mon.left && g_hole[0].top == mon.top &&
              g_hole[0].right == mon.right && g_hole[0].bottom == mon.bottom,
              "an off-screen taskbar band opens a hole clipped to the monitor");

        /* the same overhang through the overlay-local path: a hole wider and
         * taller than the strip must punch only the intersection and then put
         * the master texture back untouched. */
        g_hole_on[0] = 1;
        SetRect(&g_hole[0], mon.left - 400, mon.top - 200,
                            mon.right + 250, mon.bottom + 150);
        ClearHoleAlpha(&ov, &saved, &saved_len, &rx0, &ry0, &rx1, &ry1);
        Check(rx0 == 0 && ry0 == 0 && rx1 == w && ry1 == h,
              "LocalHole clips an off-screen hole into the overlay extent");
        clipped_ok = 1;
        for (y = 0; y < h; y += 37)
            for (x = 0; x < w; x += 41) {
                unsigned char a = px[((size_t)y * w + x) * 4 + 3];
                if (a != 0) clipped_ok = 0;   /* every on-overlay pixel is punched */
            }
        Check(clipped_ok,
              "every pixel inside the clipped hole has its alpha punched");
        RestoreHoleAlpha(&ov, saved, saved_len, rx0, ry0, rx1, ry1);
        /* RestoreHoleAlpha frees saved itself (by the saved rect's length). */
        Check(H(px, (size_t)w * h * 4) == clean,
              "after the overhanging hole closes the master texture is restored byte for byte");

        /* a hole wholly off the overlay must be a no-op, not an inverted rect
         * punched at the wrong edge (the failure ClipRect explicitly avoids
         * by leaving an empty result inverted rather than clamping it) */
        g_hole_on[0] = 1;
        SetRect(&g_hole[0], mon.right + 10, mon.bottom + 10,
                            mon.right + 90, mon.bottom + 90);
        ClearHoleAlpha(&ov, &saved, &saved_len, &rx0, &ry0, &rx1, &ry1);
        Check(saved == NULL && rx0 == 0 && ry0 == 0 && rx1 == 0 && ry1 == 0,
              "a hole entirely off the overlay punches nothing (no inverted rect)");
        Check(H(px, (size_t)w * h * 4) == clean,
              "the wholly-off-screen hole leaves every master pixel alone");

        g_ntb = 0;
        g_hole_on[0] = 0;
    }

    /* --------------------- taskbar slots cover every monitor ----------------
     * TB_MAX used to be 4 while MAX_MON was 16, so a desk with 5+ displays
     * each showing a taskbar silently dropped taskbars 5..N: TbEnumProc
     * stopped enumerating, ComputeHoles never saw them, no hole was punched
     * and no raise happened, so the veil painted straight over them with
     * nothing in the log (2026-10-08 round-10 review). The capacity fix is
     * TB_MAX = MAX_MON; what is asserted here is the half that can regress
     * quietly - that the cap is not below the monitor cap, and that hitting
     * the cap is reported rather than silent. */
    Check(TB_MAX >= MAX_MON,
          "taskbar slots cover every monitor the veil can have (TB_MAX >= MAX_MON)");
    /* ------------------- the symptom itself, end to end -------------------
     * The constant above is a static invariant and the gate review (round 10)
     * was right that it exercises no code path. This is the runtime half, and
     * it drives the REAL production chain: windows whose class the product
     * recognises as a taskbar -> EnumWindows(TbEnumProc) -> CollectTaskbars ->
     * ComputeHoles -> a hole per taskbar. Five fakes is one past the old
     * TB_MAX of 4, so with that value back the fifth and sixth never enter
     * g_tb and this fails on the count, which is exactly the reported symptom
     * (taskbars 5..N textured over, nothing logged) rather than on a proxy
     * for it. The fakes are parked outside every virtual screen and are
     * destroyed before the suite returns, so nothing paints for the user. */
    {
        WNDCLASSW fc = {0};   /* zeroed: an uninitialised hbrBackground is a garbage brush the fake windows paint with */
        HWND fake[5];
        int made = 0, k, found = 0;
        RECT mon5 = { -32000, -32000, -31000, -31000 };   /* covers the fakes */
        fc.hInstance = GetModuleHandleW(NULL);
        fc.lpfnWndProc = DefWindowProcW;
        fc.lpszClassName = L"Shell_TrayWnd";   /* what IsTaskbarWnd matches */
        if (RegisterClassW(&fc)) {
            for (k = 0; k < 5; k++) {
                fake[k] = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                          L"Shell_TrayWnd", L"t",
                                          WS_POPUP | WS_VISIBLE,
                                          -32000 + k * 40, -32000, 30, 30,
                                          NULL, NULL, fc.hInstance, NULL);
                if (fake[k]) made++;
            }
        }
        if (made == 5) {
            FILE *f = fopen("hole_cycle_tbcap.log", "w");
            char buf[512];
            BOOL r;
            g_test_headless = 1;
            /* the table exactly full: the cap is the first thing TbEnumProc
             * tests, so a FALSE here can only have come from the cap */
            g_ntb = TB_MAX;
            Check(f != NULL, "cap log file could be opened");
            if (f) {
                g_log = f;
                r = TbEnumProc(fake[0], 0);
                g_log = NULL;
                fflush(f); fclose(f);
                f = fopen("hole_cycle_tbcap.log", "r");
                memset(buf, 0, sizeof buf);
                if (f) { fread(buf, 1, sizeof buf - 1, f); fclose(f); }
                DeleteFileA("hole_cycle_tbcap.log");   /* no residue */
                printf("  .. log: %s\n", buf[0] ? buf : "(empty)");
                Check(r == FALSE, "a taskbar past the cap stops the enumeration (nothing is stored)");
                Check(g_ntb == TB_MAX, "the cap is not overrun: no slot is written past the end");
                Check(buf[0] != '\0',
                      "hitting the taskbar cap writes a log line (a dropped taskbar is diagnosable)");
                Check(strstr(buf, "TB_MAX") != NULL,
                      "the cap log identifies the bound it stopped at (TB_MAX)");
            }
            /* now the real chain, with five taskbars on one monitor */
            g_n = 1;
            g_ov[0].rc = mon5; g_ov[0].idx = 0;
            g_hole_on[0] = 0; g_hole_dirty = 0;
            CollectTaskbars();
            ComputeHoles();
            for (k = 0; k < 5; k++) {
                int j;
                for (j = 0; j < g_ntb; j++)
                    if (g_tb[j].left == -32000 + k * 40 && g_tb[j].top == -32000) { found++; break; }
            }
            printf("  .. %d of 5 fake taskbars collected (g_ntb=%d), hole_on=%d\n",
                   found, g_ntb, g_hole_on[0]);
            Check(found == 5,
                  "all five taskbars survive enumeration (the old TB_MAX=4 dropped the fifth)");
            Check(g_hole_on[0] == 1,
                  "the taskbars past the old cap get their hole punched, not painted over");
            for (k = 0; k < 5; k++) if (fake[k]) DestroyWindow(fake[k]);
        } else {
            printf("  NOTE could not create five fake taskbar windows (%d made) - "
                   "the end-to-end half of this check is skipped\n", made);
            for (k = 0; k < 5; k++) if (k < made && fake[k]) DestroyWindow(fake[k]);
        }
        g_n = 1;
        g_ov[0].rc = mon;          /* restore the suite's reference geometry */
        g_ntb = 0;                /* leave no synthesized taskbar behind */
        g_hole_on[0] = 0;
    }

    free(px);
    printf("\nRESULT %d failure(s) across %d checks\n", fails, checks);
    return fails ? 1 : 0;
}
