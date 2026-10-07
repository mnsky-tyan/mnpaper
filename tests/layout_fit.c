/* Layout proof for the settings-dialog captions.
 *
 * A caption control clips its text at its own rectangle: the extra glyphs are
 * simply not painted, and no ellipsis is added. So this test renders a caption
 * twice, once in the real control geometry (the width the dialog uses) and
 * once in a deliberately oversized control where clipping is impossible, and
 * compares the ink extent:
 *
 *   - same ink width in both            -> the whole caption is drawn
 *   - narrower ink in the real control  -> glyphs were cut off
 *
 * Everything happens on hidden windows: the settings dialog is created without
 * WS_VISIBLE, the comparison controls are children without WS_VISIBLE, and the
 * registry is redirected to a scratch hive, so the real settings, autostart
 * entry and desktop are untouched. No network is touched.
 */
#define wWinMain MnPaperEntry
#include "../mnPaper.c"
#undef wWinMain

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INK_MAGIC 0x00FF00FFu          /* untouched-pixel sentinel */
#define REF_W     600                  /* wide enough that clipping is impossible */

static int failures;
static void Check(int ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* --------------------------- window rendering ---------------------------- */

typedef struct { int first, last, magenta, w, h; unsigned *px; } Render;

static int RenderCtrl(HWND ctl, int w, int h, Render *r) {
    HDC mdc = CreateCompatibleDC(NULL);
    BITMAPINFO bi;
    HBITMAP bmp;
    HGDIOBJ old;
    unsigned *dibbits = NULL;
    int x, y;

    memset(r, 0, sizeof *r);
    r->first = r->last = -1;
    if (!mdc) return 0;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;                 /* top-down rows */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&dibbits, NULL, 0);
    if (!bmp || !dibbits) { if (bmp) DeleteObject(bmp); DeleteDC(mdc); return 0; }
    old = SelectObject(mdc, bmp);
    for (y = 0; y < w * h; y++) dibbits[y] = INK_MAGIC;
    SendMessageW(ctl, WM_PRINT, (WPARAM)mdc, PRF_CLIENT | PRF_ERASEBKGND);
    SelectObject(mdc, old);
    r->w = w; r->h = h;

    {   /* ink = any pixel that is neither the sentinel nor the face colour
         * (the face colour is whatever dominates the painted row). */
        unsigned face = 0, cand[64];
        int cnt[64], nc = 0, best = -1, i, n = w * h;
        for (i = 0; i < n; i++) {
            unsigned c = dibbits[i] & 0x00FFFFFFu; int j;
            if (c == (INK_MAGIC & 0x00FFFFFFu)) continue;
            for (j = 0; j < nc; j++) if (cand[j] == c) break;
            if (j < nc) { cnt[j]++; if (cnt[j] > best) { best = cnt[j]; face = c; } }
            else if (nc < 64) { cand[nc] = c; cnt[nc] = 1; nc++; if (best < 1) { best = 1; face = c; } }
        }
        for (x = 0; x < w; x++) {
            int inked = 0;
            for (y = 0; y < h; y++) {
                unsigned c = dibbits[y * w + x] & 0x00FFFFFFu;
                if (c == (INK_MAGIC & 0x00FFFFFFu)) { r->magenta++; continue; }
                if (c != face) inked = 1;
            }
            if (inked) { if (r->first < 0) r->first = x; r->last = x; }
        }
        /* Keep an owned copy: destroying a DIB section frees the bits. */
        r->px = (unsigned *)malloc((size_t)w * h * 4);
        if (r->px) memcpy(r->px, dibbits, (size_t)w * h * 4);
    }
    DeleteObject(bmp);
    DeleteDC(mdc);
    return r->px != NULL;
}

static int InkWidth(const Render *r) { return r->first < 0 ? 0 : r->last - r->first + 1; }

static int WriteBmp(const char *path, const unsigned *px, int w, int h) {
    BITMAPFILEHEADER fh; BITMAPINFOHEADER ih; FILE *f = fopen(path, "wb");
    if (!f || !px) {
        printf("  WARN: BMP not written: %s\n", path);   /* never fail silently */
        if (f) fclose(f);
        return 0;
    }
    memset(&fh, 0, sizeof fh); memset(&ih, 0, sizeof ih);
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + (DWORD)w * h * 4;
    ih.biSize = sizeof ih; ih.biWidth = w; ih.biHeight = -h;
    ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(px, (size_t)w * h * 4, 1, f);
    fclose(f);
    return 1;
}


/* ------------------------------ the dialog ------------------------------- */

static HWND g_dlg;

static HWND MakeCaption(const WCHAR *text, int w, int visible) {
    return CreateWindowExW(0, L"BUTTON", text,
        WS_CHILD | (visible ? WS_VISIBLE : 0) | BS_AUTOCHECKBOX,
        14, 470, w, 20, g_dlg, (HMENU)9999, GetModuleHandleW(NULL), NULL);
}

/* Geometry, bounds and spacing for whatever set of controls is visible: the
 * controls must sit at their design coordinates, every control must fit inside
 * the 402x402 client, and no two visible controls may overlap. */
static void CheckLayout(HWND dlg, const char *mode) {
    RECT c0, all[96];
    int ids[96], n = 0, i, j, outside = 0, overlap = 0;
    HWND c;
    GetClientRect(dlg, &c0);
    printf("\nlayout (%s): client %dx%d (design 402x402)\n",
           mode, (int)c0.right, (int)c0.bottom);
    Check(c0.right == 402 && c0.bottom == 402, "client is exactly the 402x402 design rect");
    {
        RECT r;
        GetWindowRect(GetDlgItem(dlg, 120), &r);
        MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
        printf("  checkbox 120 at %d,%d %dx%d\n",
               (int)r.left, (int)r.top, (int)(r.right - r.left), (int)(r.bottom - r.top));
        Check(r.left == 14 && r.top == 316 && r.right == 366 && r.bottom == 336,
              "checkbox 120 sits at its design position");
        GetWindowRect(GetDlgItem(dlg, 117), &r);
        MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
        Check(r.left == 81 && r.right == 321, "the update button keeps its design width");
    }
    c = GetWindow(dlg, GW_CHILD);
    while (c && n < 96) {
        RECT r;
        if (GetWindowLongW(c, GWL_STYLE) & WS_VISIBLE) {
            GetWindowRect(c, &r);
            MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
            if (r.left < 0 || r.top < 0 || r.right > c0.right || r.bottom > c0.bottom) {
                outside++;
                printf("  OUTSIDE client: id=%d at %d,%d..%d,%d\n",
                       GetDlgCtrlID(c), (int)r.left, (int)r.top, (int)r.right, (int)r.bottom);
            }
            all[n] = r; ids[n] = GetDlgCtrlID(c); n++;
        }
        c = GetWindow(c, GW_HWNDNEXT);
    }
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++) {
            RECT a = all[i], b = all[j];
            int ox = (a.left > b.left ? a.left : b.left);
            int oy = (a.top > b.top ? a.top : b.top);
            int ex = (a.right < b.right ? a.right : b.right);
            int ey = (a.bottom < b.bottom ? a.bottom : b.bottom);
            if (ox < ex && oy < ey) {
                overlap++;
                printf("  OVERLAP: id=%d and id=%d\n", ids[i], ids[j]);
            }
        }
    printf("  %d visible controls, %d outside the client, %d overlapping\n", n, outside, overlap);
    Check(n >= 8, "found the dialog's visible controls");
    Check(outside == 0, "every visible control fits inside the client");
    Check(overlap == 0, "no two visible controls overlap");
}

int main(void) {
    WNDCLASSW wc = {0};
    WCHAR scratch[128], run[160], cap[256];
    static const WCHAR *OLD_CAP = L"Check for updates automatically (about once a day)";
    static const WCHAR *NEW_CAP = L"Check for updates automatically";
    Render rold, rnew, roldref, rnewref;
    HWND hOld, hOldRef, hNewRef, h120;
    int clippedOld, completeNew;
    char tmpdir[MAX_PATH];
    char bmp_old[520], bmp_new[520], bmp_row[520], bmp_dlg[520];

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("mnPaper settings-dialog caption fit check\n");

    g_test_headless = 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitCommonControls();
    swprintf(scratch, 128, L"Software\\mnPaper-layout-%lu", GetCurrentProcessId());
    swprintf(run, 160, L"%s\\Run", scratch);
    REG_KEY = scratch; lstrcpynW(g_run_key, run, 160);

    /* visual-check BMPs land in %TEMP%, never the working directory (the
     * old relative paths littered the repo and WriteBmp failed silently
     * on a read-only cwd; 2026-10-04 review) */
    GetTempPathA(MAX_PATH, tmpdir);
    snprintf(bmp_old, sizeof bmp_old, "%smnpaper_layout_caption_old.bmp", tmpdir);
    snprintf(bmp_new, sizeof bmp_new, "%smnpaper_layout_caption_new.bmp", tmpdir);
    snprintf(bmp_row, sizeof bmp_row, "%smnpaper_layout_row_real.bmp", tmpdir);
    snprintf(bmp_dlg, sizeof bmp_dlg, "%smnpaper_layout_dialog_real.bmp", tmpdir);

    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpfnWndProc = DlgProc; wc.lpszClassName = SET_CLASS;
    RegisterClassW(&wc);

    /* The design geometry is DPI independent by design: the window is small
     * and the fonts are whatever the display gives, so the check that matters
     * is that every caption still renders in full at this display's scaling
     * (audited below) and that the client is exactly the design rect. */
    {   /* same creation path as OpenSettings: caption + system menu, then the
         * client is corrected to the design rect the controls are laid out in */
        RECT wr = { 0, 0, 402, 402 };
        AdjustWindowRect(&wr, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
        g_dlg = CreateWindowExW(0, SET_CLASS, L"", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                0, 0, wr.right - wr.left, wr.bottom - wr.top,
                                NULL, NULL, wc.hInstance, NULL);
    }
    FitClient(g_dlg, 402, 402);
    Check(g_dlg != NULL && !IsWindowVisible(g_dlg), "settings dialog stays hidden");
    h120 = GetDlgItem(g_dlg, 120);
    Check(IsWindow(h120), "auto-update checkbox (id 120) exists");

    GetWindowTextW(h120, cap, 256);
    Check(!wcsstr(cap, L"("), "checkbox 120 caption carries no parenthetical");
    Check(!wcscmp(cap, NEW_CAP), "checkbox 120 caption is exactly the short form");

    hOld    = MakeCaption(OLD_CAP, 352,   0);
    hOldRef = MakeCaption(OLD_CAP, REF_W, 0);
    hNewRef = MakeCaption(NEW_CAP, REF_W, 0);
    Check(IsWindow(hOld) && IsWindow(hOldRef) && IsWindow(hNewRef),
          "comparison controls created as hidden children of the hidden dialog");

    Check(RenderCtrl(hOld,    352,   20, &rold),    "rendered old caption at its real 352px");
    Check(RenderCtrl(hOldRef, REF_W, 20, &roldref), "rendered old caption at 600px (reference)");
    Check(RenderCtrl(h120,    352,   20, &rnew),    "rendered live checkbox 120 at 352px");
    Check(RenderCtrl(hNewRef, REF_W, 20, &rnewref), "rendered short caption at 600px (reference)");

    printf("\ncaption ink extents (pixels):\n");
    printf("  %-44s ctrl_w=%3d  ink=%3d..%3d  width=%3d\n", "old caption @352 (2.7.2 layout)",
           352, rold.first, rold.last, InkWidth(&rold));
    printf("  %-44s ctrl_w=%3d  ink=%3d..%3d  width=%3d\n", "old caption @600 (unclipped)",
           REF_W, roldref.first, roldref.last, InkWidth(&roldref));
    printf("  %-44s ctrl_w=%3d  ink=%3d..%3d  width=%3d\n", "new caption @352 (live control 120)",
           352, rnew.first, rnew.last, InkWidth(&rnew));
    printf("  %-44s ctrl_w=%3d  ink=%3d..%3d  width=%3d\n", "new caption @600 (unclipped)",
           REF_W, rnewref.first, rnewref.last, InkWidth(&rnewref));

    clippedOld  = InkWidth(&rold) < InkWidth(&roldref) - 1;
    completeNew = abs(InkWidth(&rnew) - InkWidth(&rnewref)) <= 1;

    Check(rold.magenta == 0 && rnew.magenta == 0 && roldref.magenta == 0 && rnewref.magenta == 0,
          "all four controls really painted themselves into the scratch DC");
    /* Whether the OLD caption clips at 352px depends on the host's font and
     * scaling - a host font that fits it failed this suite with nothing
     * wrong (2026-10-04 review). It stays a measurement. What IS
     * font-independent, and therefore assertable: the short caption renders
     * strictly narrower than the long one in whatever font the host picked. */
    Check(InkWidth(&rnew) < InkWidth(&rold),
          "the short caption renders strictly narrower than the old one in this host's font");
    printf("  old caption at 352px: %s (ink %d vs unclipped %d)\n",
           clippedOld ? "clipped at this host's font/DPI" : "fits at this host's font/DPI",
           InkWidth(&rold), InkWidth(&roldref));
    Check(completeNew, "NEW caption draws in full at the same 352px (fix proven)");
    Check(rnew.last < 351, "fixed caption no longer runs into the clip edge");
    printf("  new ink %d vs unclipped %d (%s)\n", InkWidth(&rnew), InkWidth(&rnewref),
           completeNew ? "complete" : "still losing glyphs");

    {   /* Text captions only: a push button paints a frame and an owner-drawn
         * button paints its own shape, so their ink always spans the control.
         * For every static/checkbox/radio caption the whole-caption test is
         * the ink width against a twin of the same class and style laid out
         * wider: equal width means no glyph was clipped. */
        HWND c = GetWindow(g_dlg, GW_CHILD);
        int mismatches = 0, audited = 0;
        printf("\ncaption audit (real width vs the same caption laid out at %dpx):\n", REF_W);
        while (c) {
            WCHAR buf[256], cls[64];
            LONG style = GetWindowLongW(c, GWL_STYLE), bst = style & 0xF;
            RECT r;
            int isStatic, isTextBtn, id;
            GetClassNameW(c, cls, 64);
            id = GetDlgCtrlID(c);
            isStatic  = !wcscmp(cls, L"Static");
            isTextBtn = !wcscmp(cls, L"Button") &&
                        (bst == BS_CHECKBOX || bst == BS_AUTOCHECKBOX ||
                         bst == BS_3STATE || bst == BS_AUTO3STATE ||
                         bst == BS_RADIOBUTTON || bst == BS_AUTORADIOBUTTON);
            GetClientRect(c, &r);
            if ((isStatic || isTextBtn) && id != 9999 && r.right > 8 && r.bottom > 0 && r.right <= 700 &&
                GetWindowTextW(c, buf, 256) && buf[0] && (int)wcslen(buf) > 3)
            {
                HWND twin = CreateWindowExW(0, cls, buf, WS_CHILD | bst,
                                            14, 470, REF_W, r.bottom, g_dlg, (HMENU)9998,
                                            GetModuleHandleW(NULL), NULL);
                Render ra = { 0 }, rb = { 0 };
                if (twin && RenderCtrl(c, r.right, r.bottom > 60 ? 60 : (int)r.bottom, &ra) &&
                            RenderCtrl(twin, REF_W, r.bottom > 60 ? 60 : (int)r.bottom, &rb)) {
                    int ca = InkWidth(&ra), cb = InkWidth(&rb), ok = abs(ca - cb) <= 1;
                    printf("  %-4s id=%-4d w=%3d ink=%3d vs %3d  %ls\n",
                           ok ? "ok" : "CUT", id, (int)r.right, ca, cb, buf);
                    audited++;
                    if (!ok) mismatches++;
                }
                /* Outside the if: when the second render fails, the first one's
                 * buffer is still live (a zero-initialised Render frees NULL). */
                free(ra.px); free(rb.px);
                if (twin) DestroyWindow(twin);
            }
            c = GetWindow(c, GW_HWNDNEXT);
        }
        printf("  %d captions audited, %d clipping\n", audited, mismatches);
        Check(audited >= 10, "audited the dialog's text captions");
        Check(mismatches == 0, "no caption in the dialog loses glyphs to clipping");
    }


    {   /* Push buttons: a framed, gradient-filled control cannot be ink-scanned,
         * so measure the caption with the real button font and require a clear
         * margin inside the button after the themed insets. */
        HWND c = GetWindow(g_dlg, GW_CHILD);
        NONCLIENTMETRICSW ncm; HFONT mf; HDC dc = GetDC(g_dlg);
        int bad = 0, n = 0;
        memset(&ncm, 0, sizeof ncm);
        ncm.cbSize = sizeof ncm;
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        mf = CreateFontIndirectW(&ncm.lfMessageFont);
        printf("\npush-button captions (caption width vs button width):\n");
        while (c) {
            WCHAR buf[256], cls[64];
            LONG style = GetWindowLongW(c, GWL_STYLE), bst = style & 0xF;
            RECT r;
            GetClassNameW(c, cls, 64);
            GetClientRect(c, &r);
            if (!wcscmp(cls, L"Button") && (bst == BS_PUSHBUTTON || bst == BS_DEFPUSHBUTTON) &&
                GetWindowTextW(c, buf, 256) && buf[0])
            {
                HFONT f = (HFONT)SendMessageW(c, WM_GETFONT, 0, 0);
                HGDIOBJ oldf; SIZE sz;
                if (!f) f = mf ? mf : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
                oldf = SelectObject(dc, f);
                sz.cx = sz.cy = 0;
                GetTextExtentPoint32W(dc, buf, (int)wcslen(buf), &sz);
                SelectObject(dc, oldf);
                {   /* 8px themed inset each side is the usual budget */
                    int slack = (int)r.right - 16 - (int)sz.cx;
                    printf("  %-5s id=%-4d caption=%3d px  button=%3d px  slack=%3d  %ls\n",
                           slack >= 16 ? "ok" : "TIGHT", GetDlgCtrlID(c), (int)sz.cx, (int)r.right,
                           slack, buf);
                    n++;
                    if (slack < 16) bad++;
                }
            }
            c = GetWindow(c, GW_HWNDNEXT);
        }
        if (mf) DeleteObject(mf);
        if (dc) ReleaseDC(g_dlg, dc);
        printf("  %d push buttons checked, %d with less than 16px slack\n", n, bad);
        Check(n >= 2, "checked the dialog's push buttons");
        Check(bad == 0, "every push-button caption fits its button with a clear margin");
    }

    CheckLayout(g_dlg, "paper mode");
    {   /* the e-ink rows occupy the same slots as the paper rows, so the
         * visibility swap is what keeps them from overlapping: exercise it
         * through the dialog's own layout routine (no mode is applied to the
         * real screen, this only re-runs DlgLayout in the test process). */
        int keep = g_s.mode;
        g_s.mode = MODE_EINK;
        DlgLayout();
        CheckLayout(g_dlg, "e-ink mode");
        g_s.mode = keep;
        DlgLayout();
    }

    /* ---- the release number has one home: version.h, shared with
     * mnPaper.rc. Its mechanical rules (MNVER_STR equals MAJOR.MINOR.PATCH,
     * the published feed matches it, the exe resource reports it) live in
     * tests/release_guard.py, which runs on every push via the pre-push hook;
     * this suite used to re-implement the format check and the two copies
     * could drift (2026-10-03 review, cross-directory duplicate). ---- */

    {   /* pictures of the real row and of both captions, for eyeballing */
        HDC mdc = CreateCompatibleDC(NULL);
        BITMAPINFO bi; HBITMAP bmp; HGDIOBJ old; unsigned *bits = NULL;
        int ok = 0;
        memset(&bi, 0, sizeof bi);
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = 402; bi.bmiHeader.biHeight = -402;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
        if (bmp && bits && mdc) {
            int i;
            for (i = 0; i < 402 * 402; i++) bits[i] = 0x00FFFFFFu;
            old = SelectObject(mdc, bmp);
            SendMessageW(g_dlg, WM_PRINT, (WPARAM)mdc, PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
            SelectObject(mdc, old);
            WriteBmp(bmp_row, bits + 300 * 402, 402, 44);
            WriteBmp(bmp_dlg, bits, 402, 402);
            ok = 1;
        }
        if (bmp) DeleteObject(bmp);
        if (mdc) DeleteDC(mdc);
        Check(ok, "rendered the real dialog for a visual check");
    }
    WriteBmp(bmp_old, rold.px, 352, 20);
    WriteBmp(bmp_new, rnew.px, 352, 20);
    /* The four visual-check BMPs exist for a human to look at, but the standing
     * rule is that nothing stays on the Windows side after the run, and %TEMP%
     * is not an exception: delete them in the same operation. Nothing is printed
     * about them, since they are gone by the time the console is read. */
    DeleteFileA(bmp_old); DeleteFileA(bmp_new);
    DeleteFileA(bmp_row); DeleteFileA(bmp_dlg);

    free(rold.px); free(rnew.px); free(roldref.px); free(rnewref.px);
    RegDeleteTreeW(HKEY_CURRENT_USER, scratch);
    printf("\nRESULT %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
