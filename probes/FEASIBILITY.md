# Feasibility report: replicating Paperman (whole-screen paper texture)

> **Update 2026-09-29:** the v1 program this report greenlit is built and staged
> at `C:\Users\tyanw\bin\mnPaper.exe`. Build facts, CLI, traps and the remaining
> validation list now live in `AGENTS.md` in this directory. This document is
> kept as the original feasibility evidence.

Date: 2026-09-29. Machine: WSL2 Ubuntu on Windows 11 Home build 26200,
display 1440x900 @ 96 dpi, 32-bit, single monitor.

Note: the screen is physically 2880x1800 at 200% scaling (1440x900 logical);
the PoC ran DPI-unaware so it saw logical pixels.

## Verdict

**Feasible.** The effect Paperman sells is not magic: it is a click-through,
always-on-top layered window that paints a very low-alpha procedural paper
texture over the whole screen. That exact mechanism was built and measured on
this machine as `poc.c` (459 lines, single file, C, zero dependencies). Every
core claim below is backed by a live run, not by reading docs.

## What the PoC actually does

1. Enumerates monitors, records geometry (no windows yet).
2. Captures a baseline screen bitmap (no overlay present).
3. Creates one popup window per monitor with extended styles
   `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST`
   positioned exactly over the monitor rectangle.
4. Generates a procedural paper texture (warm off-white ~ (255,248,235),
   fine 4-octave grain, stretched fibre streaks at ~2x12 texel cells, slow
   macro mottling at ~90 texel cells, all alpha-mixed into a 0..maxAlpha band)
   into a 32-bit top-down DIB and installs it with
   `UpdateLayeredWindow(..., ULW_ALPHA)` for per-pixel alpha.
5. Runs self-checks, captures the composited screen, measures idle cost,
   then destroys its windows and exits.

No screen capture loop, no hooks, no APIs injected into other processes, no
rendering after setup. The texture is painted once and the window manager and
DWM keep it composited for free.

## Measured results (live run, alpha = 30/255 = 11.8%)

```
virtual_screen=1440x900 at (0,0)  depth=32bit  dpi_x=96
monitors=1
[monitor 0] 1440x900 rect=(0,0)-(1440,900)
  exstyle=080800a8 LAYERED=1 TRANSPARENT=1 TOOLWINDOW=1 NOACTIVATE=1 TOPMOST=1
  WindowFromPoint(center)=CASCADIA_HOSTING_WINDOW_CLASS -> fell through (click-through OK)
  taskbar point=MSTaskSwWClass -> taskbar still clickable (OK)
monitor 0: mean|delta|=1.777  max|delta|=20  pixels_changed(>2)=20.74%
idle_cpu=0.26% of one core
working_set=19.31 MiB   texture_memory=5 MiB
```

Second run at alpha = 55/255 = 21.6%: mean|delta|=3.058, max=38,
38.1% of pixels changed; the paper look was clearly visible in captured
images (warm cream tone, visible grain and blotches, text softened slightly).

Interpretation of each measurement:

- **All five style bits confirmed.** The overlay is layered (per-pixel alpha
  texture), transparent (hit-testing skips it), a tool window (never appears
  in Alt-Tab), no-activate (cannot steal focus), and topmost (covers taskbar
  and maximized apps - the capture shows the veil composited over a maximized
  terminal).
- **Click-through proven on the real routing path.** `WindowFromPoint` is the
  same hit-test the system uses to route mouse clicks: at screen center it
  returned the terminal window *underneath* the overlay, and over the taskbar
  it returned the task list, not the overlay. Keyboard input is unaffected
  because the window never activates.
- **The texture genuinely composites and is tunable.** Baseline vs overlay
  captures differ by a controllable mean pixel delta; alpha is the intensity
  knob (11.8% = subtle, 21.6% = obvious paper).
- **Idle cost is effectively zero.** 0.26% of one core over 12s with a static
  texture and a message loop; there is no per-frame rendering. Working set
  19 MiB includes two throwaway 1440x900x24-bit capture bitmaps; the shipping
  footprint is the ~5 MiB texture plus process overhead. A 4K monitor costs
  ~33 MiB for the full-res texture (tiling can trade that down).

One honest probe subtlety: sending `WM_NCHITTEST` to the overlay by hand
returns `HTCLIENT`, not `HTTRANSPARENT`, because `DefWindowProc` does not
itself implement the `WS_EX_TRANSPARENT` style - the window manager simply
skips such windows during hit-testing before anyone asks the wndproc. The
authoritative evidence for click-through is therefore the `WindowFromPoint`
result, which fell through on two different targets (an app and the taskbar).

## Architecture options

| Option | What it is | Pros | Cons |
|---|---|---|---|
| A. Layered window + `UpdateLayeredWindow` (per-pixel alpha) | The PoC and PaperSrc design | Proven, one file, zero deps, ~0 idle CPU, no capture, no latency, survives any app that is a normal window | Static bitmap; ignores exclusive-fullscreen apps (see limits) |
| B. Layered window + `SetLayeredWindowAttributes` + colorkey | Constant alpha, one transparent color | Even simpler | Color-key transparency is all-or-nothing per RGB value: cannot express a noise texture. Rejected. |
| C. DirectComposition visual | Swapchain as a composited visual | Smooth animation if the texture ever needs to move | Extra machinery with no benefit for a static texture. Only revisit if animated grain is wanted. |
| D. Screen capture + reprocess (PaperShade e-ink mode) | Grayscale/2-16 shade/dither pipeline presented as an overlay | Enables true e-ink conversion | Continuous GPU cost, capture latency, fails in exclusive fullscreen anyway, much bigger program. Only needed for an optional e-ink mode later. |
| E. Magnifier API / Windows color filters | System accessibility paths | Officially supported | Cannot inject an arbitrary texture. Rejected. |

**Recommendation: option A**, which is what the PoC already proves.

## Language / toolchain decision

| Option | Pros | Cons |
|---|---|---|
| **C + MSVC cl (chosen)** | MSVC 14.44.35207 and Windows SDK 10.0.26100 are **already installed**; one command builds a 165 KB dependency-free exe; no Nix download; captain wants lightweight native | C is manual but the workload is ~600-900 lines of straightforward Win32 |
| C + MinGW-w64 via nix | Cross-compiles from WSL | Redundant: downloads a whole second toolchain for zero gain |
| Rust (windows-rs) | Memory safety | Needs rustup + a target installed; build on 9p needs `CARGO_TARGET_DIR` tricks; overkill for this size |
| C# | Fast to write | Needs .NET at rest, heavier startup, JIT; against the lightweight preference |
| Electron (PaperSrc style) | - | Explicitly against the captain's preference; hundreds of MB for a texture |

Build recipe used (works today, reproduced twice):

```
pushd <workdir>
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul
cl /nologo /O2 /W3 poc.c user32.lib gdi32.lib shell32.lib psapi.lib
```

Run note: Windows PATH is intentionally not imported into WSL; `cmd.exe` had
no wrapper, so `~/.local/bin/cmd` (execs the absolute path) was added per the
standing rule "a new Windows tool gets a wrapper, not a PATH extension".
PowerShell already had a wrapper and was used to launch the PoC minimized
(`Start-Process -WindowStyle Minimized -Wait`) per the validation hygiene rule.

## Hard limits on "the whole screen, regardless" (be honest here)

1. **True exclusive-fullscreen DirectX games** bypass DWM compositing, so the
   overlay is not presented there. This is architectural: capture-based tools
   (PaperShade) fail there too. Borderless-windowed games are unaffected
   because they are ordinary windows.
2. **Secure desktop**: UAC prompts, Ctrl+Alt+Del, and the lock screen run on a
   separate window station, so no overlay is painted. This is correct and
   desirable behaviour, not a defect.
3. **Multiplane Overlay (MPO)** with certain borderless-fullscreen games can
   flicker or hide overlays occasionally; a known DWM quirk, driver dependent.
4. **Per-monitor DPI**: the PoC ran at 96 dpi unscaled. The shipping app needs
   a PerMonitorV2 manifest and must rebuild textures on `WM_DPICHANGED`,
   `WM_DISPLAYCHANGE`, and monitor hotplug.
5. **Kernel anti-cheat** in some games dislikes always-on-top overlay windows
   (most, including Discord and Xbox Game Bar, coexist fine; nonzero but low
   risk, and only relevant while a game is running).
6. **Text legibility**: the veil softens text slightly by design. Keep the
   default alpha low (~10-12%) so reading is unaffected.

## What v1 (mnPaper) still needs beyond the PoC

- System tray icon + menu (toggle, intensity, warmth, grain scale, exit).
- Intensity/warmth sliders or a hotkey-driven control scheme.
- Persisted settings (registry or small JSON next to the exe).
- Optional start-with-Windows (Run key or Startup task).
- Rebuild texture on monitor/DPI/resolution changes; regenerate all overlays.
- Optional `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` so screen
  recordings/screen shares do not contain the paper texture.
- Texture presets (rice paper / newsprint / warm white / near-neutral).
- Per-monitor coverage verification (the PoC already does this logic).

Realistic size: ~600-900 lines of C in one file, 1-2 focused sessions. The PoC
is already the skeleton: replace the self-check main with a tray/menu message
loop, parameterize the texture builder, add persistence.

## Cost

$0 and zero installs: the toolchain was already on the machine. Runtime cost
measured at ~0.26% of one core and roughly 10-12 MiB without the PoC's capture
bitmaps (5 MiB texture at 1440x900).

## Recommendation if only doing one thing tonight

Ship the PoC as-is behind a tray toggle is not needed yet: the PoC proved the
mechanism. The decision to make next is whether to (a) let me build mnPaper v1
(tray UI + persistence, ~1-2 sessions), (b) keep the PoC as a throwaway demo,
or (c) tune texture aesthetics first (warmth, grain size, fibre density) so v1
starts from a look you approve.
