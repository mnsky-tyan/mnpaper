/* Native regression for the update-check result path, the "?" help window and
 * the capture-exclusion precedence rules. Hidden windows only: the settings
 * dialog, the overlay strips and the help window are created with
 * g_test_headless set, the test strips are parked far outside every virtual
 * screen, and every MessageBoxW the result path raises is intercepted by a
 * thread-local CBT hook before it can be activated, so nothing is ever
 * painted on the user's desktop and no browser is ever launched.
 *
 * No network is touched unless MN_VAL_NETWORK is defined: the worker posts
 * "WM_APP_UPDATE with a result code and a malloc'd version string" to the
 * host window, and that contract is driven here through the real host window
 * and the real UpdateResult handler.
 */
#define wWinMain MnPaperEntry
#include "../mnPaper.c"
#undef wWinMain

#include <crtdbg.h>

static int failures;
static void Check(int ok, const char *what) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

/* ------------------------- MessageBox interception ----------------------- */
static HHOOK g_cbt;
static int   g_box_seen, g_box_answer, g_box_has_no, g_box_has_yes;
static WCHAR g_box_cap[128], g_box_text[1024];

static void CatText(WCHAR *dst, const WCHAR *src) {
    size_t n = wcslen(dst);
    while (*src && n + 4 < 1020) dst[n++] = *src++;
    if (n + 4 < 1020) { dst[n++] = ' '; dst[n++] = '|'; dst[n++] = ' '; }
    dst[n] = 0;
}

static void CaptureBox(HWND h) {
    HWND c = GetWindow(h, GW_CHILD);
    GetWindowTextW(h, g_box_cap, 128);
    g_box_text[0] = 0;
    g_box_has_no = g_box_has_yes = 0;
    while (c) {
        WCHAR buf[512], cls[64];
        GetClassNameW(c, cls, 64);
        if (GetWindowTextW(c, buf, 512) && buf[0]) {
            CatText(g_box_text, buf);
            if (!wcscmp(cls, L"Button")) {
                int j = 0, k;          /* drop the mnemonic ampersand */
                for (k = 0; buf[k]; k++) if (buf[k] != '&') buf[j++] = buf[k];
                buf[j] = 0;
                if (!wcscmp(buf, L"No")) g_box_has_no = 1;
                if (!wcscmp(buf, L"Yes")) g_box_has_yes = 1;
            }
        }
        c = GetWindow(c, GW_HWNDNEXT);
    }
}

static LRESULT CALLBACK CbtProc(int code, WPARAM w, LPARAM l) {
    if (code == HCBT_ACTIVATE) {
        HWND h = (HWND)w;
        WCHAR cls[64];
        GetClassNameW(h, cls, 64);
        if (!wcscmp(cls, L"#32770")) {   /* the modal message box */
            CaptureBox(h);
            g_box_seen++;
            EndDialog(h, g_box_answer);  /* answer without ever showing it */
        }
    }
    return CallNextHookEx(NULL, code, w, l);
}

/* Deliver one worker result through the real host window and wait for the
 * message box it raises (answered, never shown). */
static void Deliver(int result, WPARAM found, int answer, DWORD budget_ms) {
    DWORD start = GetTickCount();
    MSG msg;
    g_box_seen = 0; g_box_answer = answer;
    g_box_cap[0] = g_box_text[0] = 0;
    PostMessageW(g_host, WM_APP_UPDATE, (WPARAM)result, (LPARAM)found);
    for (;;) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (g_box_seen) return;
        if ((LONG)(GetTickCount() - start) < 0 || GetTickCount() - start > budget_ms)
            return;
        Sleep(1);
    }
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

/* Forget the previous box before driving another path that must raise one. */
static void ResetBox(void) { g_box_seen = 0; g_box_cap[0] = g_box_text[0] = 0; }

static int TextHas(const WCHAR *hay, const char *needle) {
    WCHAR w[256];
    MultiByteToWideChar(CP_UTF8, 0, needle, -1, w, 256);
    return wcsstr(hay, w) != NULL;
}

int main(void) {
    WNDCLASSW wc = {0};
    RECT rc = { -32000, -32000, -31200, -31600 };   /* parked off every screen */
    WCHAR scratch[128], run[160];
    SETTINGS before = g_s;
    int i;
    HWND hbtn, h1, edit;
    /* The heap-leak assertions below only mean anything when the CRT debug
     * heap is really instrumented: without _DEBUG the _CrtMem* macros compile
     * to no-ops, so the state structs would never be written and the check
     * would read uninitialised memory. Skip the whole block in release
     * instead (debug recipe: tests/README.md). */
#ifdef _DEBUG
    _CrtMemState m0, m1, md;
#endif

    g_test_headless = 1;   /* help + update windows must never become visible */
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitCommonControls();
    swprintf(scratch, 128, L"Software\\mnPaper-validation-%lu", GetCurrentProcessId());
    swprintf(run, 160, L"%s\\Run", scratch);
    REG_KEY = scratch; lstrcpynW(g_run_key, run, 160);

    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpfnWndProc = HostProc; wc.lpszClassName = L"MnPaperTestHost";
    RegisterClassW(&wc);
    g_host = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, NULL, wc.hInstance, NULL);
    Check(IsWindow(g_host), "message-only host window created for result routing");
    wc.lpfnWndProc = OvlProc; wc.lpszClassName = L"MnPaperOverlay";
    RegisterClassW(&wc);
    g_n = 1;
    Check(MakeOverlay(&g_ov[0], NULL, &rc), "off-screen strip target created");
    g_ov[0].idx = 0;
    {   /* Proof the strips cannot be seen: they are parked outside every
         * virtual screen, so any path that shows them shows nothing. */
        RECT r;
        GetWindowRect(g_ov[0].shwnd[0], &r);
        Check(r.left < GetSystemMetrics(SM_XVIRTUALSCREEN) &&
              r.top < GetSystemMetrics(SM_YVIRTUALSCREEN),
              "test strip is parked outside every visible screen");
    }
    wc.lpfnWndProc = DlgProc; wc.lpszClassName = SET_CLASS;
    RegisterClassW(&wc);
    g_dlg = CreateWindowExW(0, SET_CLASS, L"", WS_OVERLAPPED, 0, 0, 380, 380,
                            NULL, NULL, wc.hInstance, NULL);
    Check(g_dlg != NULL && !IsWindowVisible(g_dlg), "settings target stays hidden");
    Check(IsWindow(GetDlgItem(g_dlg, 116)), "? help button exists (id 116)");
    Check(IsWindow(GetDlgItem(g_dlg, 117)), "check-for-updates button exists (id 117)");

    /* ------------------------- update-check results ---------------------- */
    g_cbt = SetWindowsHookExW(WH_CBT, CbtProc, NULL, GetCurrentThreadId());
    Check(g_cbt != NULL, "message-box trap installed so validation paints nothing");

    Deliver(UPT_SAME, 0, IDOK, 2000);
    Check(g_box_seen == 1, "up-to-date result opens exactly one message box");
    Check(TextHas(g_box_text, "latest version") && TextHas(g_box_cap, "up to date"),
          "up-to-date feed tells the user they are current");
    Check(!g_box_has_no, "an up-to-date user is only informed, not offered a download");

    Deliver(UPT_MALFORMED, 0, IDNO, 2000);
    Check(g_box_seen == 1, "malformed-feed result opens exactly one message box");
    Check(TextHas(g_box_text, "could not be read") && TextHas(g_box_text, "download page"),
          "a feed that answers but states no version says so and offers the page");
    Check(g_box_has_yes && g_box_has_no,
          "the malformed-feed box offers Yes and No, so declining is possible");

    Deliver(UPT_NONE, 0, IDNO, 2000);
    Check(g_box_seen == 1 && TextHas(g_box_text, "Could not reach the update feed"),
          "unreachable feed says so and offers the page");
    Check(g_box_has_yes && g_box_has_no,
          "the unreachable-feed box offers Yes and No, so declining is possible");

    /* A newer version is reported, and the user is offered the page rather
     * than having it forced on them: the box carries Yes and No buttons. */
    {
        WCHAR *found = _wcsdup(L"2.7.0");
        Deliver(UPT_NEW, (WPARAM)found, IDNO, 2000);
        Check(g_box_seen == 1 && TextHas(g_box_text, "2.7.0"),
              "newer version is reported to the user");
        Check(g_box_has_yes && g_box_has_no,
              "the newer-version box offers Yes and No, so declining is possible");
        /* UpdateResult owns the payload and frees it exactly once: nothing to
         * free here, and the heap checkpoints below prove it. */
    }

    /* The result must survive the settings dialog that asked for it: the host
     * window owns the delivery, so a late answer still reaches the user and
     * the version string is released exactly once. */
#ifdef _DEBUG
    _CrtMemCheckpoint(&m0);
    {
        WCHAR *found = _wcsdup(L"9.9.9");
        DestroyWindow(g_dlg);          /* the dialog that clicked the button */
        Pump(20);
        Check(g_dlg == NULL, "settings dialog really closed before the answer");
        Deliver(UPT_NEW, (WPARAM)found, IDNO, 2000);
        Check(g_box_seen == 1 && TextHas(g_box_text, "9.9.9"),
              "late update answer still reaches the user after the dialog closed");
    }
    _CrtMemCheckpoint(&m1);
    _CrtMemDifference(&md, &m0, &m1);
    printf("METRIC heap delta after the delayed result: %ld block(s), %ld byte(s)\n",
           (long)md.lCounts[0], (long)md.lSizes[0]);
    Check(md.lCounts[0] == 0, "no heap block is leaked by the delayed update result");

    _CrtMemCheckpoint(&m0);
    Deliver(UPT_SAME, 0, IDOK, 2000);
    Check(g_box_seen == 1, "a second result still arrives after the dialog closed");
    _CrtMemCheckpoint(&m1);
    _CrtMemDifference(&md, &m0, &m1);
    printf("METRIC heap delta after the second result: %ld block(s), %ld byte(s)\n",
           (long)md.lCounts[0], (long)md.lSizes[0]);
    Check(md.lCounts[0] == 0, "no heap block is leaked by a result without a dialog");
#else
    /* Same paths, no leak instrumentation: still prove the answer survives
     * the dialog closing, just without the heap-difference assertion. */
    {
        WCHAR *found = _wcsdup(L"9.9.9");
        DestroyWindow(g_dlg);          /* the dialog that clicked the button */
        Pump(20);
        Check(g_dlg == NULL, "settings dialog really closed before the answer");
        Deliver(UPT_NEW, (WPARAM)found, IDNO, 2000);
        Check(g_box_seen == 1 && TextHas(g_box_text, "9.9.9"),
              "late update answer still reaches the user after the dialog closed");
    }
    Deliver(UPT_SAME, 0, IDOK, 2000);
    Check(g_box_seen == 1, "a second result still arrives after the dialog closed");
#endif

    /* A fresh dialog can ask again; while a check is in flight the busy guard
     * drops a second click instead of racing a second worker thread. */
    {
        WNDCLASSW dwc = {0};
        dwc.hInstance = GetModuleHandleW(NULL);
        dwc.lpfnWndProc = DlgProc; dwc.lpszClassName = SET_CLASS;
        RegisterClassW(&dwc);
        g_dlg = CreateWindowExW(0, SET_CLASS, L"", WS_OVERLAPPED, 0, 0, 380, 380,
                                NULL, NULL, dwc.hInstance, NULL);
    }
    hbtn = GetDlgItem(g_dlg, 117);
    Check(IsWindow(hbtn), "check-for-updates button exists in a reopened dialog");
    InterlockedExchange(&g_update_busy, 1);   /* stand in for a running check */
    ResetBox();
    SendMessageW(hbtn, BM_CLICK, 0, 0);
    Pump(50);
    Check(g_update_busy == 1 && g_box_seen == 0,
          "a second click while a check runs is dropped, not started");
    InterlockedExchange(&g_update_busy, 0);

    /* ------------------------- "?" help window --------------------------- */
    hbtn = GetDlgItem(g_dlg, 116);
    SendMessageW(hbtn, BM_CLICK, 0, 0);
    h1 = g_helpwnd;
    Check(h1 && IsWindow(h1), "pressing ? creates the help window");
    Check(!IsWindowVisible(h1), "help window stays hidden in a headless run");
    {
        WCHAR cls[64];
        GetClassNameW(h1, cls, 64);
        Check(!wcscmp(cls, L"MnPaperHelp"), "help window uses the MnPaperHelp class");
    }
    edit = GetWindow(h1, GW_CHILD);
    Check(edit != NULL, "help window has an edit child");
    {
        LONG es = GetWindowLongW(edit, GWL_STYLE);
        RECT pclient, erect;
        Check((es & ES_READONLY) != 0, "help text is read-only");
        Check((es & ES_MULTILINE) != 0 && (es & WS_VSCROLL) != 0,
              "help text is multiline with a vertical scrollbar");
        GetClientRect(h1, &pclient);
        GetWindowRect(edit, &erect);
        MapWindowPoints(NULL, h1, (POINT *)&erect, 2);
        printf("METRIC help parent client %ldx%ld, edit %ld,%ld %ldx%ld\n",
               pclient.right, pclient.bottom, erect.left, erect.top,
               erect.right - erect.left, erect.bottom - erect.top);
        Check(erect.left == 0 && erect.top == 0 && erect.right <= pclient.right &&
              erect.bottom <= pclient.bottom,
              "edit is sized to the parent client rect, so the scrollbar is not clipped");
        {
            SCROLLINFO si;
            si.cbSize = sizeof si; si.fMask = SIF_RANGE;
            Check(GetScrollInfo(edit, SB_VERT, &si) && si.nMax > si.nMin,
                  "help text overflows the window, so the scrollbar matters");
        }
        /* WM_SIZE keeps the same relationship after a resize. */
        SetWindowPos(h1, NULL, 0, 0, 620, 700, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        Pump(20);
        GetClientRect(h1, &pclient);
        GetWindowRect(edit, &erect);
        MapWindowPoints(NULL, h1, (POINT *)&erect, 2);
        Check(erect.left == 0 && erect.top == 0 && erect.right <= pclient.right &&
              erect.bottom <= pclient.bottom,
              "resizing the help window resizes the edit to the new client rect");
    }
    SendMessageW(hbtn, BM_CLICK, 0, 0);
    Check(g_helpwnd == h1, "pressing ? again reuses the same help window");
    Check(!IsWindowVisible(h1), "re-clicking ? reuses it without showing it");
    DestroyWindow(h1);
    Check(g_helpwnd == NULL, "closing the help window clears it");

#ifdef MN_VAL_NETWORK
    /* The real button, the real WinHTTP worker, the real feed. Off by default:
     * it performs a live network request and its result raises a real box. */
    SendMessageW(GetDlgItem(g_dlg, 117), BM_CLICK, 0, 0);
    Pump(20000);
    Check(g_update_busy == 0, "a live check releases the busy guard when it finishes");
#endif

    /* --------------------- capture-exclusion precedence ------------------ */
    /* E-ink is always excluded from captures, whatever the flags say: a
     * capture of the e-ink output feeds it back in and white-washes the
     * screen, and no flag may be able to override that. */
    {
        DWORD aff;
        int saved_master = g_s.master, saved_share = g_s.share, saved_mode = g_s.mode;
        g_s.master = 0;
        g_force_capture_show = 1;          /* the --no-exclude validation flag */
        g_s.share = 1;
        Check(CaptureHidden() == 0, "paper mode with --no-exclude and share=1 is capturable");
        SetMode(MODE_EINK);
        Check(CaptureHidden() == 1, "e-ink hides from captures even with --no-exclude");
        ApplyCaptureState(&g_ov[0]);
        aff = 0;
        Check(GetWindowDisplayAffinity(g_ov[0].shwnd[0], &aff) &&
              aff == WDA_EXCLUDEFROMCAPTURE,
              "the real strip window is capture-excluded in e-ink despite the flag");
        g_s.share = 0; g_force_capture_show = 0;
        Check(CaptureHidden() == 1, "e-ink stays excluded with every flag cleared");
        SetMode(MODE_PAPER);
        ApplyCaptureState(&g_ov[0]);
        aff = 1;
        Check(GetWindowDisplayAffinity(g_ov[0].shwnd[0], &aff) &&
              aff == WDA_EXCLUDEFROMCAPTURE,
              "paper mode with share=0 keeps the default exclusion");
        g_s.share = 1;
        ApplyCaptureState(&g_ov[0]);
        aff = 1;
        Check(GetWindowDisplayAffinity(g_ov[0].shwnd[0], &aff) && aff == 0,
              "paper mode with share=1 is capturable again");
        g_force_capture_show = 1;
        Check(CaptureHidden() == 0, "paper mode honours --no-exclude");
        g_force_capture_show = 0;
        g_s.share = saved_share; g_s.mode = saved_mode; g_s.master = saved_master;
    }

    /* The e-ink radio asks first: a stray click must not trap the user in
     * full-screen greyscale. Declining snaps the radio straight back. */
    {
        int saved_master = g_s.master;
        g_s.master = 0;
        g_box_answer = IDNO;
        ResetBox();
        SendMessageW(GetDlgItem(g_dlg, 115), BM_CLICK, 0, 0);
        Check(g_box_seen == 1, "clicking E-ink asks for confirmation first");
        Check(g_s.mode == MODE_PAPER && IsDlgButtonChecked(g_dlg, 114) == BST_CHECKED,
              "declining the e-ink confirmation leaves the mode untouched");
        g_box_answer = IDYES;
        SendMessageW(GetDlgItem(g_dlg, 115), BM_CLICK, 0, 0);
        Check(g_s.mode == MODE_EINK && IsDlgButtonChecked(g_dlg, 115) == BST_CHECKED,
              "accepting the confirmation switches to e-ink in place");
        Check(IsWindow(g_dlg), "the settings dialog stays open across the e-ink switch");
        Check(!IsWindowEnabled(g_chk_share), "share toggle greys out in e-ink");
        {
            DWORD aff = 0;
            Check(GetWindowDisplayAffinity(g_ov[0].shwnd[0], &aff) &&
                  aff == WDA_EXCLUDEFROMCAPTURE,
                  "the e-ink switch keeps the strip capture-excluded on the real window");
        }
        /* Switching back to Paper is the escape hatch: instant, no prompt. */
        g_box_seen = 0;
        SendMessageW(GetDlgItem(g_dlg, 114), BM_CLICK, 0, 0);
        Check(g_box_seen == 0 && g_s.mode == MODE_PAPER,
              "switching back to Paper is instant and asks nothing");
        Check(IsDlgButtonChecked(g_dlg, 114) == BST_CHECKED &&
              IsWindowEnabled(g_chk_share), "paper rows and the share box are restored");
        g_box_answer = IDNO;
        SetMaster(0);          /* the overlay was on for the switch: put it back */
        Pump(20);
        g_s.master = saved_master;
    }

    for (i = 0; i < g_n; i++) Check(!IsWindowVisible(g_ov[0].shwnd[i]),
                                    "test strip never became visible");
    if (g_cbt) UnhookWindowsHookEx(g_cbt);
    DestroyWindow(g_helpwnd);
    DestroyWindow(g_dlg);
    DestroyWindow(g_host);
    RegDeleteTreeW(HKEY_CURRENT_USER, scratch);
    g_s = before;
    printf("RESULT %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
