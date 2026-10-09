/* Windowless regression for the settings dialog: every "settings changed,
 * refresh the window" path must land on the same push, and that push must
 * leave every control matching g_s. Hidden target only - the dialog is
 * created without WS_VISIBLE and sized 402x402 off any interaction, so
 * nothing paints on the user's desktop.
 */
#define wWinMain MnPaperEntry
#include "../mnPaper.c"
#undef wWinMain

static int failures;
static void Check(int ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static int DlgCheck(HWND dlg, int id, int want) {
    return SendMessageW(GetDlgItem(dlg, id), BM_GETCHECK, 0, 0) == want;
}

int main(void) {
    WNDCLASSW wc = {0};
    HWND dlg;
    int i;
    struct { int id; int *slot; } bars[] = {
        { 100, &g_s.intensity }, { 101, &g_s.warmth }, { 102, &g_s.grain },
        { 103, &g_s.fibre }, { 104, &g_s.blotch }, { 105, &g_s.shades },
        { 106, &g_s.contrast }, { 107, &g_s.dither },
    };

    g_test_headless = 1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitCommonControls();

    /* Isolated scratch hive, PID-suffixed under this suite's own name. The PID
     * is what separates runs (suites are separate processes, so two different
     * suites never shared a key even when they shared a name); naming each
     * suite's key is for attributing a key a crashed run left behind, not for
     * collision prevention - 2026-10-09 review. This suite only
     * pushes settings into controls today, but any future click-driven check
     * (e.g. checkbox 119) runs SaveSettings -> ApplyAutostart, and without the
     * redirect that would write the REAL Run key with this test exe's path - a
     * persistent autostart change surviving the run (2026-10-04 review). */
    {
        WCHAR scratch[128], run[160];
        swprintf(scratch, 128, L"Software\\mnPaper-dlgpush-%lu", GetCurrentProcessId());
        swprintf(run, 160, L"%s\\Run", scratch);
        REG_KEY = scratch; lstrcpynW(g_run_key, run, 160);
    }

    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpfnWndProc = HostProc; wc.lpszClassName = L"MnPaperPushHost";
    RegisterClassW(&wc);
    g_host = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, NULL, wc.hInstance, NULL);
    Check(IsWindow(g_host), "message-only host window created");
    wc.lpfnWndProc = OvlProc; wc.lpszClassName = L"MnPaperOverlay";
    RegisterClassW(&wc);
    wc.lpfnWndProc = DlgProc; wc.lpszClassName = SET_CLASS;
    RegisterClassW(&wc);
    dlg = CreateWindowExW(0, SET_CLASS, L"", WS_OVERLAPPED, 0, 0, 402, 402,
                          NULL, NULL, wc.hInstance, NULL);
    FitClient(dlg, 402, 402);
    Check(IsWindow(dlg) && !IsWindowVisible(dlg), "settings target stays hidden");
    g_dlg = dlg;

    /* WM_CREATE ends with the single push, so a freshly created dialog
     * already shows the live settings without any per-control boot code. */
    for (i = 0; i < 8; i++)
        Check((int)SendMessageW(GetDlgItem(dlg, bars[i].id), TBM_GETPOS, 0, 0) == *bars[i].slot,
              "bar control shows the live setting after creation");
    Check(DlgCheck(dlg, 118, g_s.master ? BST_CHECKED : BST_UNCHECKED),
          "texture-on checkbox matches master after creation");
    Check(DlgCheck(dlg, 119, g_s.autostart ? BST_CHECKED : BST_UNCHECKED),
          "autostart checkbox matches the setting after creation");
    Check(DlgCheck(dlg, 120, g_s.autoupd ? BST_CHECKED : BST_UNCHECKED),
          "auto-update checkbox matches the setting after creation");
    Check(DlgCheck(dlg, 113, g_s.share ? BST_CHECKED : BST_UNCHECKED),
          "share checkbox matches the setting after creation");
    Check(IsDlgButtonChecked(dlg, g_s.mode == MODE_PAPER ? 114 : 115) == BST_CHECKED,
          "the active mode radio is checked after creation");
    /* 114 is a radio button, not a trackbar: the old "bars readable" check
     * sent TBM_GETPOS to it, got 0 back, and could never fail (2026-10-03
     * review). Read a bar this block has not touched yet. */
    Check((int)SendMessageW(GetDlgItem(dlg, 107), TBM_GETPOS, 0, 0) == g_s.dither,
          "an untouched bar reads back its pushed position");

    /* A remote change (tray menu, CLI sync, mode switch) calls the same push:
     * every control must follow, including ones the old per-site code missed. */
    g_s.intensity = 23; g_s.warmth = 77; g_s.grain = 6; g_s.fibre = 31;
    g_s.blotch = 12;  g_s.shades = 5;   g_s.contrast = 41; g_s.dither = 62;
    g_s.master = 0; g_s.autostart = 0; g_s.autoupd = 0; g_s.share = 1;
    DialogPushSettings(dlg);
    for (i = 0; i < 8; i++)
        Check((int)SendMessageW(GetDlgItem(dlg, bars[i].id), TBM_GETPOS, 0, 0) == *bars[i].slot,
              "bar follows the changed setting through the one push");
    Check(!DlgCheck(dlg, 118, BST_CHECKED), "master off is pushed to its checkbox");
    Check(!DlgCheck(dlg, 119, BST_CHECKED), "autostart off is pushed to its checkbox");
    Check(!DlgCheck(dlg, 120, BST_CHECKED), "auto-update off is pushed to its checkbox");
    Check(DlgCheck(dlg, 113, BST_CHECKED), "share on is pushed to its checkbox");

    /* The numeric readout beside each bar must show THAT bar's value. Nothing
     * else asserts the label-to-bar pairing: UpdateVals' own ids[] table,
     * WM_CREATE's g_val[k]/MkTrack(100+k) pairing, DlgSyncBars' field order and
     * ReadBarsToSettings' field order all have to agree, and a drift in ids[]
     * alone would silently swap two labels with every other check still green
     * (2026-10-08 review). Bars carry distinct values here (23, 77, 6, 31, 12,
     * 5, 41, 62), so a swap cannot pass by coincidence. */
    {
        static const int row_ids[8] = { 100, 101, 102, 103, 104, 105, 106, 107 };
        int k, bad = 0;
        for (k = 0; k < 8; k++) {
            WCHAR label[16];
            int pos = (int)SendMessageW(GetDlgItem(dlg, row_ids[k]), TBM_GETPOS, 0, 0);
            label[0] = 0;
            if (!g_val[k]) { bad++; continue; }
            GetWindowTextW(g_val[k], label, 16);
            if (_wtoi(label) != pos) bad++;
        }
        Check(bad == 0, "each numeric readout shows its own bar's value, not a neighbour's");
    }

    /* E-ink mode grays the share box: that mode is always capture-excluded. */
    g_s.mode = MODE_EINK;
    DialogPushSettings(dlg);
    Check(IsDlgButtonChecked(dlg, 115) == BST_CHECKED, "e-ink radio selected by the push");
    Check(IsWindowEnabled(GetDlgItem(dlg, 113)) == 0, "share box grayed in e-ink mode");
    Check(DlgCheck(dlg, 113, BST_CHECKED), "share state kept while grayed, not reset");
    {   /* the e-ink rows are the ones shown in e-ink mode */
        LONG ex = GetWindowLongW(g_tb_shades, GWL_STYLE);
        Check((ex & WS_VISIBLE) != 0, "e-ink bars visible in e-ink mode");
    }

    /* The other mode flips back, and the push restores the enabled share box. */
    g_s.mode = MODE_PAPER; g_s.master = 1;
    DialogPushSettings(dlg);
    Check(IsDlgButtonChecked(dlg, 114) == BST_CHECKED, "paper radio selected by the push");
    Check(IsWindowEnabled(GetDlgItem(dlg, 113)) == 1, "share box enabled in paper mode");
    Check(DlgCheck(dlg, 118, BST_CHECKED), "master on restored in its checkbox");

    /* A dead dialog must be a no-op, not a crash. The guard the push leans
     * on is LiveDlg's IsWindow test, so assert THAT, then push: a regression
     * that removed the guard dies here loudly. */
    g_dlg = dlg;
    DestroyWindow(dlg);
    Check(LiveDlg() == NULL, "a destroyed dialog is dead to LiveDlg (the push's own guard)");
    DialogPushSettings(dlg);
    Check(!IsWindow(dlg), "the push left the dead dialog dead (no resurrection, no crash)");
    /* TaskbarCreated re-add: the branch the round-11 tray fix added, until
     * now unpinned. wWinMain never runs here, so the suite stores the same
     * registered value the real init would (RegisterWindowMessageW returns
     * one atom per string per session), posts the message, and asserts the
     * branch logs - headless runs suppress the real Shell_NotifyIconW call
     * (no tray icon may appear for a test process) but must still take the
     * branch and say so. */
    {
        UINT tb = RegisterWindowMessageW(L"TaskbarCreated");
        FILE *f = fopen("dialog_push_tb.log", "w");
        char buf[256];
        Check(tb != 0 && f != NULL, "TaskbarCreated registered and the log file opened");
        if (tb && f) {
            MSG m;
            g_msg_taskbar = tb;
            g_log = f;
            PostMessageW(g_host, tb, 0, 0);
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
            g_log = NULL;
            fflush(f); fclose(f);
            f = fopen("dialog_push_tb.log", "r");
            memset(buf, 0, sizeof buf);
            if (f) { fread(buf, 1, sizeof buf - 1, f); fclose(f); }
            DeleteFileA("dialog_push_tb.log");   /* no residue */
            Check(strstr(buf, "TaskbarCreated") != NULL,
                  "a TaskbarCreated message takes the re-add branch (logged in headless runs)");
        }
    }

    DestroyWindow(g_host);
    {   /* leave no scratch behind */
        WCHAR scratch[128];
        swprintf(scratch, 128, L"Software\\mnPaper-dlgpush-%lu", GetCurrentProcessId());
        RegDeleteTreeW(HKEY_CURRENT_USER, scratch);
    }

    printf("RESULT %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
