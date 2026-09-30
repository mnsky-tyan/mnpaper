# Slider latency regression

`slider_latency.c` includes the native app and runs its actual `DlgProc`
slider handler, asynchronous renderer and `UpdateLayeredWindow` presentation.
All strip and settings windows stay hidden; the host is message-only. It
never injects input, changes the foreground window or switches the user's
screen mode. Settings writes use a unique temporary HKCU test key, which is
deleted at the end. The user's existing mnPaper process can stay running.

Build from a VS2022 Enterprise x64 Native Tools shell at the paper root:

```bat
cl /nologo /O2 /W3 tests\slider_latency.c /Feslider_latency.exe
```

Launch the console test minimized and wait for the **process to exit** before
reading the report. Exit 0 and `RESULT 0 failure(s)` indicate success. The test
checks rendered texture pixels and successful layered-window uploads, not
just numeric slider labels. It does not claim to replace a human visual check
of the live desktop.

Covered:

- 2880x1800 preview changes pixels within 250ms and produces multiple frames.
- The newest pending request survives a busy renderer and reaches every monitor.
- The full-quality final pixels exactly match the final settings.
- A new drag cancels full-resolution refinement instead of waiting for it.
- Closing before the autosave timer fires saves and refines the final change.
- Taskbar masking uploads without resynthesizing or corrupting the texture.
- Odd dimensions, transparent zero strength and premultiplied-alpha validity.
- Two-monitor rendering after a 200-request burst.
- The settings-window share checkbox (id 113) persists `share` and flips the
  real `WDA_EXCLUDEFROMCAPTURE` affinity on the hidden overlay strip.
- Mode radios (id 114/115): SetMode morphs the open dialog in place (rows
  swap, radios reflect, share checkbox greys in e-ink) and persists `mode`.
- The Texture-on checkbox (id 118) reflects the current master state on open;
  unchecking it runs the real handler in the HIDE direction only (master=0),
  the show direction is never fired through the handler (so no strips are
  painted), and state is restored by save.
- The Start-with-Windows checkbox (id 119) persists autostart and drives the
  isolated Run key via ApplyAutostart: checking it gains the mnPaper entry,
  unchecking removes it - the real HKCU Run key is never touched.
- The ? button (id 116) opens one shared help window; in headless mode it is
  created but never shown, re-click reuses it, closing clears it.
- Version parsing/comparison for the update check as pure functions, plus
  button 117 existence. The live WinHTTP check is deliberately NOT exercised
  in tests (real network + a real MessageBox would disturb the working user).
- Preview structure metrics: grain measured with fibre and blotch off keeps
  alpha-std above 2 over the whole trackbar range (grain 2-12), and its
  pixel-level detail falls as the grain coarsens - the full build does the
  same, and the unscaled 8px preview ignored the grain slider entirely.
  Fibre 0->100 / blotch 0->100 add clearly more structure than their zero
  points.
- Test windows never become visible.

`update_help_capture.c` is the companion hidden regression for the release
paths: it drives the update-check result handler through the real message-only
host window (so a late result still lands after the settings dialog is closed,
and the version string is released exactly once), the help window's edit child
sized to the parent client rect with a reachable scrollbar, and the
capture-exclusion precedence (e-ink stays excluded from captures even with
`--no-exclude` or `share=1`). It also clicks the real E-ink radio to cover
the confirmation path: declining snaps the radio straight back to Paper, and
the way back to Paper asks nothing. It parks its test strip outside every
virtual screen and intercepts each `MessageBoxW` with a thread-local CBT hook,
so validation paints nothing on the working desktop and starts no browser.

Build it in a debug configuration to enable the heap-leak assertions
(`_CrtMemDifference` is a no-op in release builds):

```bat
cl /nologo /Od /W3 /D_DEBUG /MDd tests\update_help_capture.c /Fetests\update_help_capture.exe
```

The live WinHTTP check is still deliberately NOT clicked by either test: it
performs a real network request and its result opens a real box.

Before the repair, the same slider regression recorded a 2469ms first update,
one frame, a lost final request and an incorrect final bitmap. The repaired
runs recorded 46-125ms first updates and 31-125ms preview request-to-upload latency.
Timing varies with machine load; retain measured results rather than inferring
performance from changing labels or process launch time.
