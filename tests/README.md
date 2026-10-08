# Test suites

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

## Known flake (documented 2026-10-02)

`slider_latency.c`'s "last requested value eventually completes without another
mouse event" can fail on a busy machine: the run still shows the final render in
flight (e.g. 11 requested, 10 completed) and the assertion waits for it to land.
Measured over 16 runs of the same code: 15 clean, 1 flaky. It is a timing
threshold, not a logic fault, and the hole code is dormant during that section.
Re-run before treating it as a regression.

## Build recipe (Windows host)

Above is the debug recipe for the leak-checked suite. The practical build for
all suites on a Windows box with VS2022 (any edition) is:

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\<Edition>\VC\Auxiliary\Build\vcvars64.bat"
rc /nologo mnPaper.rc
cl /nologo /O2 /W3 mnPaper.c mnPaper.res /FemnPaper.exe
for %%f in (layout_fit slider_latency verify_fixes hole_cycle dialog_push gdi_fallback eink_thread tall_coverage) do ^
  cl /nologo /O2 /W3 tests\%%f.c /Fe%%f.exe
cl /nologo /O2 /W3 /MTd /D_DEBUG tests\update_help_capture.c /Fetest_extra.exe
```

One source file, two recipes: the block above this one builds the same
`update_help_capture.c` as `tests\update_help_capture.exe` with `/Od /D_DEBUG /MDd`,
the line here builds it as `test_extra.exe` with `/O2 /MTd /D_DEBUG` - either
link works, and the leak assertions only exist because of `/D_DEBUG`, so a
build without that define silently drops them.

Every suite is in the repository (`git ls-files tests/` is the truth): the
block above builds all of them from a fresh clone (the loop builds eight, and
the final `cl` line builds `update_help_capture.c` as `test_extra.exe`). The
only gitignored file under tests/ is `release_guard.py`.

Run every exe twice; `slider_latency.c` a third time (measured 1-in-16 flake
above). Each is idempotent and leaves no windows or scratch files behind.

If a freshly built test exe will not start or vanishes between build and
run, check Windows Defender before suspecting the build: its ML heuristics
intermittently quarantine these unsigned exes (`Behavior:Win32/Execution.A!ml`,
seen 2026-10-04 on a hole_cycle build). `Get-MpThreatDetection` shows it;
rebuild and run in one go, or exclude the temp directory.

### What each suite asserts

- `slider_latency.c` - drag latency end to end (request-to-upload budget with
  METRIC lines), the final bitmap, worker preemption, close-before-timer, the
  taskbar hole cycle (hole moves/parks with no texture rebuild, master
  byte-identical - every byte), odd preview sizes, warmth/grain/fibre/blotch
  structure, help window, update-feed parsing, and the share/mode/texture-on/
  autostart controls against a PID-suffixed scratch hive.
- `eink_thread.c` - the e-ink worker machinery headlessly: pure render
  properties, capture-slot ownership (the ring is 3 deep), publish, worker
  render, pointer swap, newest-wins, settings re-render on a static screen,
  idle quiescence, ring hygiene, stop/restart, that `PresentEinkRect` waits
  while the worker holds the swap lock (then copies a real frame), and the
  `DxgiHasUsableOutput`/`DxgiRetryable`/`DxgiAnyRotated` combination table over
  plain, rotated-only, unsupported-format-only, mixed (identity plus rotated)
  and all-rotated output sets. The mixed row is the one the rotation policy
  exists for: `DxgiAnyRotated` must force the whole-desktop GDI grab even
  though a usable identity output is present.
- `gdi_fallback.c` - the GDI capture fallback: DIB/DC persistence across
  ticks (no per-tick reallocation), exact whole-buffer change detection (an
  edit at any coordinate is seen, not one pixel in 97), shadow resync, clean
  rebuild after a reset.
- `hole_cycle.c` - the taskbar hole over a fixed geometry (2880x1800 monitor,
  48px band): reveal/park cycles produce the right hole rect, the master
  texture comes back byte-identical every time (full-byte hash), 20 round
  trips leave no residue, and `ComputeHoles`' parked/revealed decision is
  driven directly for a bottom band, a left-docked sliver and a right-docked
  sliver (the side docks a height-only test used to misread as revealed).
- `dialog_push.c` - one `DialogPushSettings` call drives every control from
  the settings struct (sliders, checkboxes, the mode radio pair), each numeric
  readout is asserted to show its own bar's value (the label-to-bar pairing no
  other suite checks), and a destroyed dialog is refused rather than pushed to.
- `layout_fit.c` - the settings dialog at 402x402: every caption renders in
  full at this display's scaling (ink width against a reference render), the
  design positions hold, controls do not overlap, push buttons keep their
  slack, and the real dialog is rendered for a visual check (the scratch BMPs
  are deleted before the suite returns).
- `verify_fixes.c` - the extracted feed-size cap (`FeedFits`) boundary, the
  show path never falling back to a full synchronous build, the hole
  save/restore being byte-identical, and the `LiveDlg` owner rule.
- `tall_coverage.c` - the strip stack over tall monitors: it drives the real
  `MakeOverlay` on 4K-portrait, 5K-portrait, 8K and very-tall rects and walks
  each created strip window's own `GetWindowRect` to prove the union covers the
  monitor edge to edge with no gap, no overlap and no oversize strip (the old
  `MAX_STRIPS` of 8 left the bottom 240px of a 3840px-tall monitor untextured,
  silently). It also drives a monitor past the cap to prove the boundary is
  real rather than silently overrun, and checks the shortfall is logged with the
  monitor's rect, its height and the height the strips reach. Its scratch log
  file is deleted before the suite returns.
- `update_help_capture.c` - the update feed's version/hash parsing and the
  help/update UI in a real (headless) window: the help window's contents, the
  manual update path with its message box intercepted by a CBT hook (both the
  decline path and the accept path, which switches to e-ink in place), and
  that the suite leaves no window visible behind it.
