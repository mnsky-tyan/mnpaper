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
- The ? button (id 116) opens one shared help window; in headless mode it is
  created but never shown, re-click reuses it, closing clears it.
- Preview structure metrics: grain 8 and 64 both show alpha-std > 2 (no more
  aliased flat smear), and fibre 0->100 / blotch 0->100 add clearly more
  structure than their zero points.
- Test windows never become visible.

Before the repair, the same slider regression recorded a 2469ms first update,
one frame, a lost final request and an incorrect final bitmap. The repaired
runs recorded 46-125ms first updates and 31-125ms preview request-to-upload latency.
Timing varies with machine load; retain measured results rather than inferring
performance from changing labels or process launch time.
