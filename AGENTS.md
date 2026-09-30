# mnPaper - whole-screen paper texture + e-ink overlay (Win32)

Status 2026-09-30: **v2.7.0 release candidate** (`mnPaper.c` + version
metadata in `mnPaper.rc` - bump both together on every release). The settings
window carries: Paper/E-ink mode radios (114/115, e-ink confirms first),
Texture on (118, the master toggle - hide without quitting), Start with
Windows (119, autostart), the share-capture checkbox (113), the ? help
button (116), and Check for updates (117, link-out); SetMode morphs the
dialog in place. Inactive rows hide their labels, controls and values
together. The captain's live preferences (autostart=1, autostart Run key in
place) must never be reset by tests.

**Earlier latency claims were wrong.** A 15ms measurement only timed the
launch of a GUI executable, not its completion. Waiting for the renderer's
process exit measured ~2.2s for 2880x1800. The pre-fix hidden slider regression
measured first changed pixels at 2469ms, one frame, a dropped final request
and the wrong final bitmap. Raising thread priority did not fix rendering
cost. Do not repeat the CPU-starvation diagnosis as established fact.

v2.5 uses an 8px sampling grid with bilinear expansion during dragging, then
exact full-resolution refinement after 400ms idle. QueuePaper retains the
latest immutable snapshot per monitor under an SRW lock; an event wakes the
worker instead of polling. New input cancels full refinement by row; preview
completion is not canceled on every mouse event (avoids starvation). Results
apply monotonically per monitor, with stale full results rejected. Taskbar
holes now mask only the layered upload, restoring the backing pixels afterward;
taskbar animation no longer queues expensive texture rebuilds. Worker stop
joins before cleanup, and display changes stop it before changing overlays.

`tests/slider_latency.c` tests the real hidden slider handler, pixel buffer and
successful UpdateLayeredWindow upload, using an isolated registry key. No mouse
movement, foreground changes, visible test windows, e-ink switching or edits to
the captain's preferences. Repeated repaired runs measured first pixels at
46-125ms, preview request-to-upload at 31-125ms; final full-quality output is
byte-identical to the old renderer. Also tests cancellation, immediate Close
save/refine, taskbar masking, odd sizes/alpha and latest values on two monitors.
These are hidden presentation tests, not a claim of a human desktop eye-check.

**Warmth range widened (captain: "0-100 is too calm").** The old mapping moved
endpoint tints by ~24/255 and never went below neutral, so the slider was
near-invisible (his warmth sat at 100 seeking warmth). New signed swing
through neutral 50: warm 100 = amber (highlight 255,236,192, shadow 157,143,78),
cool 0 = icy (highlight 200,254,255, shadow 105,121,158). Measured mean
blue-minus-red over the composited texture: warm -4, cool +2 (old full range
swung ~1). Tooltip notes the ends are strong on purpose.

Probe helpers live in `probes/`: `zprobe.c` (window
dump), `zorder3.c` (pids/rects/ranks), `zorder4.c` (rank race cadence),
`tborder.c` (auto-hide taskbar reveal watch), `strip.c` (who covers the
bottom strip, by rank), `tbwatch.c` (taskbar topmost bit + rank timeline
through a reveal), `tbtest3.c`/`tbtest4.c`/`tbtest5.c` (veil-geometry
bisection that pinned the width heuristic), `tbfinal2.c` (end-to-end
reveal probe using SendInput), `hooktest.c`/`hooktest2.c` (proof that
SetCursorPos bypasses WH_MOUSE_LL while SendInput traverses it),
`tbtest6.c`..`tbtest10.c` (height-threshold and strip-stack bisection),
`tbstate.c` (taskbar rect/topmost/foreground state + un-wedge click).

## Where things live

- Source: `mnPaper.c` (this directory). PoC: `probes/poc.c`.
- Staged exe: `C:\Users\tyanw\bin\mnPaper.exe` (a release /O2 /W3 build;
  the source now ships a 2.7.0 VERSIONINFO and the app icon via `mnPaper.rc`).
  The DEPLOYED exe may lag this merged source until the post-merge redeploy the
  agent performs (currently a 2.6.0 build with no icon resource), so an agent
  session must not assume the binary on disk carries the 118/119 GUI work or
  the 2.7.0 VERSIONINFO.
  Settings live in `HKCU\Software\mnPaper` (REG_DWORDs); autostart is the
  `mnPaper` value in `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`.
- Tuning samples: `samples/` (paper at intensity 20 and 30, e-ink at 2/4/16
  shades). All produced headlessly with `--dump-tex` / `--dump-eink`.

## Build (MSVC already on this machine)

```
pushd <workdir on C:>
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /W3 mnPaper.c
```

Links via pragmas: user32, gdi32, shell32, advapi32, comctl32, shcore,
d3d11, dxgi, dxguid, ole32.

## CLI (second instance talks to the running one)

```
mnPaper.exe --on | --off | --toggle | --paper | --eink | --settings | --quit
mnPaper.exe --set key=value [key=value ...]     # intensity warmth grain
                                                 # fibre blotch shades
                                                 # contrast dither mode master
                                                 # autostart
mnPaper.exe --log FILE                          # append debug log
mnPaper.exe --no-exclude                        # leave overlay capturable (validation only)
mnPaper.exe --dump-tex FILE w h [bg]            # headless paper render composited over bg
mnPaper.exe --dump-eink FILE w h                # headless e-ink render of a synthetic desktop
```

Defaults: master on, paper mode, intensity 30, warmth 45, grain 4, fibre 40,
blotch 30; e-ink shades 4, contrast 50, dither 75; autostart off.
The seed setting was removed with the New pattern button: the noise math
never consumed it, so rerolls were visually identical (verified by hashing
renders before removal). The stray registry value is inert and was deleted
at deploy time.

## Frozen design decisions (from the grilling rounds)

- Two independent state variables: `master` (Ctrl+Alt+P) and `mode` paper/eink
  (Ctrl+Alt+E also turns master on). Tray has radio items for the mode.
- Paper texture is **bidirectional mottling**: warm-white lifts plus warm-gray
  fibre dims over a small constant warm matte veil. A pure white veil is
  invisible over white content (white-on-white cancels in the composite) - this
  was the single biggest visual bug found during the build.
- E-ink pipeline: capture (DXGI Desktop Duplication, GDI BitBlt fallback with a
  change gate) -> luminance -> shade-count quantization -> optional Bayer 4x4
  ordered dither (strength 0-100%). Pure grayscale; no texture under e-ink.
- All overlay windows get `WDA_EXCLUDEFROMCAPTURE` (never in screen shares).
- Minimal UI: tray + one small settings window (paper: intensity/warmth/grain
  + Advanced disclosure for fibre/blotch; e-ink: shades/contrast/dither).
  The settings window is topmost but only while open; it closes on mode change.
- No cache files, no installer, no update mechanism, no video optimisation in
  e-ink mode (video is best-effort by design).

## Historical settings window v2 (superseded by v2.5 above)

The timings and tooltip/subclass design in this historical section were not
reliable evidence of actual screen updates. Current implementation and measured
regression coverage are described at the top of this file.

Three complaints, three fixes (all verified live by `probes/dlgtest.c`/`probes/cmptest.c`):

1. **Live slider preview.** The old handler sat in a 160ms debounce that
   every WM_HSCROLL reset - during a drag NOTHING was applied - and
   `ApplyPaperResult`'s `d->gen == g_pgen` check discarded every build that
   finished after another request, so even slow drags never showed interim
   values. Now: WM_HSCROLL reads the bars into `g_s` and requests a rebuild
   at most every 70ms (a build takes ~80ms, the worker slot-guard coalesces
   the rest); jobs carry their REQUEST-time gen and results apply
   monotonically (`d->gen >= applied`) instead of exact-match. Effect: each
   finished build lands on screen ~80-150ms behind the thumb (~12fps live
   preview), verified by measuring screen roughness mid-drag. A trailing
   400ms timer only saves the registry.
2. **Tooltips.** One `tooltips_class32` control, `AddTip()` per control
   (TTF_IDISHWND | TTF_SUBCLASS), 250ms initial delay, plain-language
   explanations for every slider/button (what it means + what it changes).
3. **Hold-to-compare.** "Hold to compare" button, subclassed via
   `SetWindowSubclass`: WM_LBUTTONDOWN posts `WM_APP_RAW(1)` (hide all
   strips - raw screen), WM_LBUTTONUP/WM_CAPTURECHANGED posts
   `WM_APP_RAW(0)` (bring the veil back). Purely visual - no setting, no
   registry write, so comparing never disturbs the configuration.

Dialog-test traps: a DPI-aware probe measuring "the screen" must sample a
region AWAY from the settings window (own windows sit above the veil, so a
region under the dialog measures dialog pixels and shows flat values for
veiled/held/released - probes/cmptest.c first "failed" because of exactly this);
and after `--settings` arrives via the CLI channel, give the window a beat
before FindWindowW (races report "not open").

## Historical async build (superseded by v2.5 latest-value queue)

The settings sliders used to block the UI thread with a full 2880x1800
texture rebuild (50-80 ms). Builds now run on a worker thread
(`PaperWorker`) that fills a staging buffer; the UI thread only does a
`memcpy` + `UpdateLayeredWindow` when a fresh result arrives. Requests
coalesce (`RequestPaper`, generation counter `g_pgen`): every timer/tray
strength change repaints through this path. Startup, mode change and
first paint still build synchronously (one 80 ms hit, invisible).

Two more findings from that round:

- The settings window sat UNDER the veil (created topmost but a mascot
  z-order fight re-asserted the veil above it). Dragging sliders through
  the translucent paper felt laggy and unreadable. `Housekeeping` now
  re-asserts the dialog above the veil every tick while it is open.
- Each slider now has a numeric readout (right of the track). The
  `UpdateVals` helper needs the dialog HWND passed in: `WM_CREATE` runs
  inside `CreateWindowExW`, before the global `g_dlg` is assigned, so
  `GetDlgItem(NULL, ...)` silently returns nothing and every readout
  shows 0.
- Paper-mode idle measured 0.4% of one core on the captain's machine.

## Taskbar sinking - ROOT CAUSED AND FIXED with STRIPS (2026-09-29 night)

The captain's auto-hide taskbar "sank" behind maximised windows while mnPaper
ran, and the first fix (the "summon dance": narrow the veil on edge touch via
a mouse hook) was rejected - the right 40% of the paper lifting during every
taskbar interaction was "not acceptable". Final design: **horizontal strips**.

### The shell heuristic (measured, clean session, probes tbtest9/10)

The shell refuses to raise a revealed auto-hide taskbar while a visible
window is fullscreen-ish. On this 2880x1800 screen, single topmost layered
windows: 2880x1800 / x1000 / x900 all SUPPRESS the raise; 2880x800 and
2880x400 never do. The height trigger sits between 800 and 900 (probably
"more than half the monitor height"). Side-by-side windows UNION (two 1440
halves suppress, even with an 8px gap) - the width data from the first
bisection. Stacked full-width windows do NOT union vertically:
2x(2880x900) suppresses (each at the 900 boundary) but 3x(2880x600) and
4x(2880x450) raise reliably (4/4 measured, control 0/2).

### The fix (v2.2)

Each monitor's veil is now `ceil(h/STRIP_H)` windows (`STRIP_H` 450, capped
`MAX_STRIPS` 8), each full monitor width, created at
`(rc.left, rc.top + s*450)` sized `(w, min(450, h - s*450))`. The ONE
full-monitor DIB stays the single texture buffer; `ApplyLayered` presents a
slice per strip via `UpdateLayeredWindow`'s `pptSrc = {0, s*450}` - no extra
DIBs, no builder changes, seams invisible (same texture, adjacent rows).
No window is ever full-width AND tall, so the shell never mistakes the veil
for a fullscreen app: the taskbar raises normally, always, with 100% paper
coverage at all times. The summon dance (hook, narrow/restore, edge zones,
`pw/ph`) is DELETED; the hole-in-the-veil, the add-only topmost re-assert,
the fight pause while revealed, and the `g_tb_state` 0/1/2 machine (with
park unlatch) all remain.

Verified: paper mode 16/16 raises (rank 1-6, topmost, floating over the
maximised terminal); e-ink 16/16; full-screen texture confirmed by capture
(`samples/taskbar_floats_during_summon.png`, `strip_idle.png` era) with no
visible seams; selfcheck 5 style bits per strip + click-through; idle CPU
~1% of one core; staged exe 188,416 bytes, 1,934 lines.

### Session-state traps (cost hours - do not re-discover)

- **A taskbar that owns the FOREGROUND stays revealed and never parks.**
  Mid-experiment the taskbar got focus (the captain clicked it at ~23:00),
  wedged up with the topmost bit set - and every later "raise=1" read was a
  stale bit, poisoning a whole bisection round ("width threshold moved!",
  false). Recovery: click the desktop (or anything else). If raise results
  look too good, CHECK the taskbar is actually parking between trials
  (`probes/tbstate.c`: rect.top back to 899 = parked, fg != Shell_TrayWnd).
- The first width bisection (probes/tbtest3-5, "69% passes / 80% fails") was run in
  a contaminated session; the clean-session width control (2000x1800)
  SUPPRESSED. Do not trust the width numbers across sessions; the height
  result (clean, repeated, control-tested) is the load-bearing one.
- `SetCursorPos` does NOT traverse `WH_MOUSE_LL` hooks (only `SendInput`
  does - probes/hooktest.c/probes/hooktest2.c); LL-hook `pt` is UNCLAMPED (real user
  flicks arrive beyond the monitor rect). Both mattered only to the dead
  dance, but remember them for any future hook work.
- A DPI-unaware probe reads the strips as 1440x225-logical tiles while the
  app logs physical 2880x450 - keep coordinate spaces straight.

## Z-order housekeeping (replaced the first taskbar guard)

The overlays are topmost and full-screen, but the veil must never sink or be
sunk. On every 50 ms tick, `Housekeeping()` walks the top of the topmost
band: our own windows, invisible windows, sub-8x8 helper windows
(`IME`, `MSCTFIME`, `DummyDWMListenerWindow`, `ForegroundStaging`), and
windows parked off the virtual screen (ChatGPT desktop parks at -16000) are
skipped. If a FOREIGN visible on-screen window sits above the veil, the
overlay re-asserts `HWND_TOPMOST` and the taskbar is re-asserted above the
veil, followed by the settings dialog.

Why: the mascot `C:\Users\tyanw\Downloads\Little-Remielle-win\...\小蕾米.exe`
(pid 21980, Qt window class `Qt5152QWindowToolSaveBits`, ex `00080088` =
LAYERED|TOPMOST) re-asserts topmost roughly every 2s and otherwise sits above
the veil; ChatGPT's `CodexComputerUseSwiftOverlay` is the same class of
problem. Measured duty cycle after the fix: the mascot is above at most
~50 ms every ~2s (40-rank probe, probes/zorder4.c).

Taskbar policy (two rounds of captain feedback, final): state-based. Every
tick, for each `Shell_TrayWnd` / `SecondaryTrayWnd`, the on-screen height is
measured: parked as a thin sliver (<= 16 px on screen) it drops UNDER the
veil so the paper covers the screen uniformly to the bottom edge; while
revealed on hover (or docked and always visible) it is re-asserted
`HWND_TOPMOST` so it floats cleanly on the paper and can never sink behind
it. Both states verified live with probes/tborder.c (tray rect parks at `0,899
1440x52` logical, reveals to `0,848 1440x52` when the cursor touches the
edge) and bottom-strip captures.

## Capture sharing toggle (captain's request)

Screenshots and screen shares both honour `WDA_EXCLUDEFROMCAPTURE`, so one
toggle covers them (Zoom/Teams/Capture use the same exclusion). Two UI
surfaces drive the same `share` registry value (default 0 = hidden):
tray item `Texture in shares/screenshots`, and the settings-window checkbox
`Show texture in screenshots and screen shares` (control id 113). Changing
either updates the other live (tray handler re-asserts the checkbox state).
Also settable with `--capture on|off` or `--set share=1|0`.
`ApplyCaptureState()` runs on every repaint and on mode change.

**Mode radios in the settings window (id 114 Paper / 115 E-ink).** Switching
mode from the GUI keeps the dialog OPEN: `SetMode()` now morphs the dialog in
place (DlgSyncBars + DlgLayout + UpdateVals + CheckRadioButton + share
checkbox enable) instead of the old `PostMessageW(WM_CLOSE)`. The share
checkbox is disabled (grey) while in e-ink because e-ink is always
capture-excluded.

**Texture on (118) / Start with Windows (119) checkboxes (captain request,
2026-09-30).** 118 drives `SetMaster` (hide without quitting; the SHOW
direction is never fired by the hidden tests - it would make the strips
visible on the user's desktop; tests verify the hide direction and restore
by state + save). 119 toggles `g_s.autostart` + `SaveSettings`, which applies
the Run key via `ApplyAutostart`. `g_run_key` is a writable buffer (default
`Software\...\Run`) that the hidden regression redirects into its scratch
hive - ApplyAutostart runs on EVERY SaveSettings, so without the redirect a
test save with autostart=0 would delete the captain's real autostart entry.
Checkbox 118 stays truthful on every master change with no per-path re-assert:
`SyncMasterCheckbox` runs from `SetMaster` and `ActivateMode`, and every
master change (Ctrl+Alt+P, Ctrl+Alt+E, CLI --on/--off/--toggle/--paper/--eink,
tray, mode radios) funnels through those two; a CMD_SYNC (`--set master=`)
also re-syncs it (it no longer closes the dialog). 119 is written by the tray
IDM_AUTOSTART and the checkbox handler (both re-assert the control), by
`SetKeyValue`'s autostart arm (`--set autostart=1`, delivered by CMD_SYNC's
re-sync), and by LoadSettings' Run-key mirror (runs before any dialog exists).

**E-ink radio asks first (captain got trapped, 2026-09-30).** A single click
on E-ink flipped his whole screen to opaque greyscale and he found the laptop
unusable and uncapturable (e-ink forces exclusion, so screenshots show
nothing). Now the GUI radio path confirms with a plain-language MessageBox
(defbutton = No) before `ActivateMode(MODE_EINK)`; declining snaps the radio
back to Paper. Paper, the tray items and Ctrl+Alt+E stay instant - the escape
hatch must never be gated. The slider regression calls `SetMode()`
directly, so it never hits the MessageBox.

**Grain preview aliasing (captain: "grain lags behind").** The 8px preview
sampled the true frequencies, so fine grain (8-10px features) aliased into a
near-flat smear: strength/warmth previews looked instant but grain seemed
dead until the full refine landed. `PAPERPARAMS.fs` now scales all noise
coordinates (fs = preview step, 1 for full builds): the preview shows the
same character with step-x larger features, instantly. The refine still lands
the exact texture. Regression measures grain with fibre and blotch off, so
only grain can move the statistic: alpha-std stays above 2 over the whole
trackbar range (grain 2-12) and the pixel-level detail falls as the grain
coarsens - the unscaled 8px preview ignored the grain slider entirely.

**Fibre and blotch now add their own alpha modulation.** They used to only
nudge the noise mix (captain: "don't seem to do much"). `PAPERPARAMS.fspread
/ bspread` add `|field-0.5|*2 * gain * spread` alpha on top, gain 1.3x
(fibre) / 1.2x (blotch) of the strength spread, zero at 0. Measured preview
alpha-std at intensity 40: fibre 4.69 -> 7.22 (0 -> 100), blotch 6.76 -> 9.29.

**Hover tooltips retired; ? help window instead.** All `MnPaperTip` hover
machinery (TipPoll/TipProc/TipPaint/AddTip/g_tipwnd) removed. A circle
"?" button (id 116, WM_DRAWITEM) opens a single help window (`MnPaperHelp`
class, read-only multiline EDIT, owned by the dialog): explains every control
and hotkey in plain language. Re-click brings it to front; g_helpwnd clears
on destroy. `g_test_headless` (test sets it) creates the window without
ShowWindow so hidden tests never flash a window on the desktop.

## Release readiness (2026-09-30)

- **Versioning**: `MNVER_MAJOR/MINOR/PATCH` in mnPaper.c + `mnPaper.rc`
  VERSIONINFO (2.7.0). Bump BOTH on every release; the rc is compiled in via
  `rc /nologo mnPaper.rc` then `cl mnPaper.c mnPaper.res` (cl does not compile
  .rc itself - passing it to cl directly fails with LNK1107).
- **Update check = link-out, never an auto-updater** (deliberate): button 117
  runs `UpdateCheckThread` (WinHTTP, HTTPS-only, default cert validation,
  5-10s timeouts, 4KB read cap), parses a numeric triple from a plain-text
  manifest, compares with `CompareVersion`, and only ever OPENS the built-in
  `PRODUCT_URL` constant. A network-supplied string is never executed or
  navigated to. Hidden regression tests must not click button 117: the live
  path makes a real network request and its result opens a real MessageBox -
  both disturb the working captain. Tests cover ParseVersionTriple and
  CompareVersion as pure functions plus button existence.
- **Before publishing**: point `UPDATE_URL` (version.txt manifest, first line
  = latest version) and `PRODUCT_URL` at the real repo. Placeholders point at
  github.com/mnsky-app/mnpaper (which does not exist yet - the button then
  shows the offline fallback and offers the page anyway).
- **Known accepted gaps**: no code-signing certificate (SmartScreen warning
  until the captain buys one); e-ink shimmer at shades=4 documented, needs
  consent to tune live; update feed not published.
- Repo: github.com/mnsky-tyan/mnpaper (private), default branch
  `main`, validated through the no-mistakes pipeline on feature
  branches.

E-ink flicker (not yet fixed, needs consent to test live): the render math is
deterministic (static `BAYER4` dither), so the shimmer he saw is inherent
4-shade quantization sparkling on every captured frame at shades=4 /
contrast-gain 1.3, re-presenting the full opaque screen per DXGI frame.
Softer defaults (higher shades, lower contrast) would calm it; verifying
requires switching his real screen to e-ink, which he must approve first.

**E-ink mode is ALWAYS capture-excluded, regardless of the toggle or of
`--no-exclude`.** DXGI Desktop Duplication captures the composited desktop
including a non-excluded overlay; feeding the e-ink output back in makes the
image wash out to pure white within ~10s (captain hit exactly this on the
live desktop). There is no legitimate reason to show e-ink raw in a capture,
so the mode hard-forces exclusion.

## Traps learned (do not re-discover)

- **A topmost BIT is not a topmost RANK.** The shell never clears
  WS_EX_TOPMOST from the auto-hide taskbar on park, so after the first
  reveal the bit is permanently set. A raise gated on "bit missing"
  (the original v2.2 add-only guard) then never fires again: the taskbar
  re-reveals at its parked rank BEHIND our topmost strips - the captain
  sees sinking "a few minutes after launch" (exactly one park/reveal
  cycle later). Fix (v2.4): raise the taskbar with
  SetWindowPos(HWND_TOPMOST) UNCONDITIONALLY every tick while revealed
  (state 2). Idempotent, add-only, never demotes; strips still re-assert
  above it once it parks.
- **Never send TBM_* / control messages cross-process to the app's dialog**
  (probes reading/writing trackbar state): v5 comctl32 faults at
  COMCTL32+0x69d0a inside the app's GetMessageW dispatch and the app dies.
  This caused a long false hunt for an app bug. Probe with REAL input only:
  SendInput (absolute 0..65535) + GetWindowRect + GetClassName + screenshots.
  Button clicks via SendInput work; drags work with DOWN, MOVE steps, UP.
- **`--set mode=1` flips the captain's WHOLE SCREEN to greyscale e-ink** while
  he is watching. He complained. Do not live-switch modes for layout checks;
  verify the inactive branch by code inspection or headless dumps.
- **BitBlt/CopyFromScreen cannot see the veil while WDA_EXCLUDEFROMCAPTURE is
  set** (`share=0`, the default, or `--capture off`): screenshots and sampling are blind
  to on/off toggles. Verify strip visibility via IsWindowVisible in the app's
  log, or test with capture exclusion off.
- The settings dialog spawns at CW_USEDEFAULT (cascading positions): never
  click fixed screen coords in probes - locate children via
  EnumChildWindows/GetWindowRect every run.
- **Every settings change saves to `HKCU\Software\mnPaper`.** Test runs
  (`--set`, slider drives in probes) DROVE the captain's live settings:
  warmth 99, fibre 80, intensity 40 were all leftovers from validation.
  Reset the keys to the defaults before handing the app back:
  intensity 30, warmth 45, grain 4, fibre 40, blotch 30,
  shades 4, contrast 50, dither 75, master 1, mode 0, share 0, autostart 1
  (his live value - never write autostart 0, that deletes his Run key).
- `WM_APP + 2` was already taken (`WM_APP_TRAY`); the async build message
  had to become `WM_APP + 3`. Check existing defines before adding a
  message. Tray command ids: 9001-9004, 9005-9006, strength 9007-9010, so
  the share toggle needed 9020.

- **`OUT` is a SAL macro** in the Windows SDK: naming a struct `OUT` makes the
  typedef vanish and every later use of it fails to compile. Use `OUTINFO`.
- `SHAppBarMessageW` is **not exported** by shell32.dll on this build (and not
  in the import lib), so `ABM_GETSTATE`/`ABM_GETTASKBARPOS` cannot be used;
  `StuckRects2\Settings` is empty on Windows 11 26200 too. Taskbar state must
  come from the `Shell_TrayWnd` window rect at runtime.
- With `#define CINTERFACE`, D3D11/DXGI calls need the `this` pointer as the
  first argument and no extra context arg: `dup->lpVtbl->AcquireNextFrame(dup, 0, &fi, &res)`.
- Registry functions need `advapi32.lib` (pragmas cover it).
- WSL-launched `powershell.exe` probes are **desktop-blind**: FindWindow and
  z-order walks from there return nothing even while the app is running.
  Verify through the app's own log, or a C PE launched from bash.
- A crashed/interrupted test run leaves an orphan holding the single-instance
  mutex; a later launch then silently forwards its commands to the orphan and
  exits. Always `Get-Process mnPaper` before concluding a launch failed.
- `start /min /wait "" exe ...` from bash `cmd /c` fails with quoting; use
  `Start-Process -WindowStyle Minimized -Wait -RedirectStandardOutput` from
  the powershell wrapper instead.
- `--set k=v k=v` (multiple pairs after one flag) works only because SetKeyValue
  is fed every `key=value` token after `--set`; a bare `--set` per pair works too.
- A second instance sends EITHER the settings sync OR the command, never both:
  `--set x=1 --on` used to drop the `--on`. The sync (`WM_COPYDATA`,
  SendMessage, synchronous) now goes first and the command is posted after,
  so ordering is guaranteed.
- The single-instance mutex trap: `mnPaper.exe --set k=v` with no instance
  running becomes the FIRST instance - it applies the sets, saves, and keeps
  running its message loop instead of exiting. Kill before launching a
  validation instance.
- Staging a new exe over a running instance fails with "Permission denied"
  (the running process holds the file). `Stop-Process` first.
- `EnumWindows` order is top-of-z-order first, and a `printf` with two
  `Cls()` calls shares ONE static buffer (the second overwrites the first):
  print classes one per line in probes (zorder3.c does it right, zorder2.c
  does not).
- Capturing from WSL PowerShell: `CopyFromScreen(0,0,0,0,(1440,900))` takes
  the top-left PHYSICAL quarter of a 2880x1800 screen. To see the taskbar
  (bottom edge), capture `CopyFromScreen(0,1440,0,0,(1440,360))`.
- Cursor-triggered tests (auto-hide reveal): one `SetCursorPos` to the edge
  may not latch the reveal; wiggle 3-4 positions then hold (mnlive8.ps1 /
  probes/tborder.c pattern).
- The mouse cursor is a hardware sprite drawn above the DWM composite: no
  window, no layered trick, no overlay can cover or texture it. Leave it
  crisp; do not try to "fix" this with a fake cursor.

## Live validation done (desktop unlocked, 2026-09-29 evening round)

- `--selfcheck` on the live desktop: all five style bits present,
  `WindowFromPoint(center)` returns the terminal window under the overlay
  (click-through confirmed), `GetWindowDisplayAffinity` = 17
  (WDA_EXCLUDEFROMCAPTURE) in default mode.
- Veil visible on Terminal, Explorer, Brave, the desktop, the mascot window,
  and the taskbar strip; the desktop and the mascot are covered with no
  un-textured patches (z-order probe: overlay rank 7 vs mascot rank 10).
- Capture toggle: `--capture off` then a screen grab shows the veil
  (mean delta ~2.8 gray levels vs `--capture on`), `--capture on` hides it.
- Taskbar: parked sliver and revealed state both verified; veil never
  withdraws.
- E-ink on the unlocked desktop: DXGI duplication engages
  (`dxgi=1 gdi_only=0` in the log), ~14 fps, ~76% of one core while active.
- The e-ink white-out was reproduced and root-caused (capture feedback, see
  above); it is now impossible in normal running and e-ink hard-excludes.

## Pending for the captain

- Taskbar: live eye-check while USING it - paper must stay full-screen at
  all times now (no lift, no seams) and the taskbar must float. 16/16
  raises verified by probe in both modes.
- Eye-check the tray menu (Strength presets + the new share toggle) and the
  settings window; confirm the paper-texture strength default (30) is where
  he wants it.
- Eye-check e-ink live (shades 4, contrast 50, dither 75) now that it cannot
  white out.
- Brave video and any exclusive-fullscreen game bypass DWM and cannot be
  textured by an overlay; report if he sees that case.

## Validation done (desktop locked, so all headless)

- Build clean at /W3: zero warnings.
- Launch, `--off/--on/--eink/--paper/--settings/--quit`: every command
  delivered and logged; process exits cleanly on quit.
- `--set` for every key lands in the registry; two-instance sync verified.
- Autostart toggle writes/deletes the Run key.
- Paper texture: renders with visible bidirectional mottle over light and
  dark backgrounds (`samples/`); grain scale, warmth, fibre, blotch all
  change the output.
- E-ink pipeline: synthetic desktop renders correctly at 2/4/16 shades with
  Bayer dither (gradients band, rules stay crisp).
- On the locked desktop DXGI duplication finds no outputs and the GDI fallback
  engages without crashing; per-tick logging confirms frame accounting.

## Still owed (needs the captain's unlocked desktop)

1. Hotkey delivery check on the unlocked desktop (Ctrl+Alt+P / Ctrl+Alt+E
   registration succeeds; keybd_event delivery not yet observed live).
2. Settings window layout check (sliders, Advanced disclosure, mode swap).
3. Idle CPU in paper mode on the unlocked desktop (0.26% of one core measured
   during the PoC).
