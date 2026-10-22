# Codebase Review - 2026-10-19

Scope: `linux/` (the Wayland port) | Review units: 1 (`linux/` - 4 source files, ~2100 lines; `protocols/` is generated and excluded)
Summary: 9 findings (0 Critical, 1 High, 4 Medium, 4 Low)

Files read in full: `linux/mnpaper.c` (2014), `linux/paper_core.h` (299), `linux/tests/test_core.c` (335), `linux/Makefile` (58), `linux/README.md`, `linux/AGENTS.md`. (Counts are the current tree's; the port has grown since this pass.)

## Findings by Severity

| # | Category | Severity | File(s) | Description | Fix Complexity |
|---|---|---|---|---|---|
| 1 | Consistency | High | `linux/mnpaper.c:899-907`, `linux/mnpaper.c:1005-1011`, `linux/mnpaper.c:1190-1204` | `mode=1` (e-ink) is accepted and persisted even though e-ink is refused. `RequestMode` logs "staying in paper mode" and returns 0, but callers ignore the return: `ApplySettings` keeps `g_s.mode = MODE_EINK` (line 899 only *calls* RequestMode; it does not restore the value), the IPC `apply` path (line 1190) writes `g_s = c->pend` then calls RequestMode without reverting, and `main` (line 1457) does the same at startup. Result: `settings.conf` stores `mode=1`, `--status` reports `mode eink`, and `--dump-tex` reports `mode eink`, while the overlay renders paper. Verified live: after `--set mode=1`, `grep ^mode ~/.config/mnpaper/settings.conf` gives `mode=1` and `--status` prints `mode eink`, yet the screen still shows paper. | Inline |
| 2 | Simplicity & Efficiency | Medium | `linux/mnpaper.c:658-681` | `PresentBake` sets `o->buf_released = 0` before the `if (o->visible)` branch, but only the visible branch commits the buffer. When the output is hidden, no commit happens, so no `wl_buffer.release` will ever arrive and `buf_released` stays 0. A later bake therefore takes the "still in use" early-return at line 664 forever, until the output is shown once. The "armed while hidden" comment at line 657 describes behaviour the code does not deliver. Reaching this needs a texture change while hidden, then a second change while still hidden. | Inline |
| 3 | Simplicity & Efficiency | Medium | `linux/mnpaper.c:495-504` vs `linux/mnpaper.c:525-529` | Duplicated and now contradictory comment blocks: the "(Re)create this output's buffer for logical size..." block at 495-504 sits directly above the `QueueBake`/`BufferRelease` forward declarations and has nothing to do with them, while the same comment is repeated almost verbatim at 525-529 above the real `BufferResize`. The first copy is orphaned by an earlier edit. | Inline |
| 4 | Refactoring Opportunities | Medium | `linux/mnpaper.c:1489-1503` | The `wl_display_prepare_read` retry loop is wrong when `wl_display_dispatch_pending` fails: it sets `g_quit = 1` but then immediately calls `wl_display_prepare_read` again, and on the error path the matching `wl_display_cancel_read` for the *first* failed prepare was never called, leaving the display in a prepared state. It also cannot terminate the loop on a persistent error other than by luck. The loop condition should bail out when `g_quit` is set. | Inline |
| 5 | Simplicity & Efficiency | Medium | `linux/mnpaper.c:783-795` | Double-`munmap` path in `OutTeardown`: `RetireSet` parks the current set (with its `data` pointer) in `o->retired[]`, then `FlushRetired` may free and unmap it, and the following `if (o->data ...) { for (...) if (retired[i].data == o->data) still = 1; if (!still) munmap(...); }` unmaps `o->data` a second time whenever the set was freed inside `FlushRetired` (the loop then finds nothing, `still` stays 0). A double `munmap` on a large region is undefined behaviour. | Inline |
| 6 | DRY | Low | `linux/mnpaper.c:1169-1185` | The client-side flag-to-protocol mapping table (`SendToRunning`) duplicates the server-side command dispatch in `IpcCommand` (lines 1140-1160) and duplicates the CLI flag list in `main` (lines 1394-1408). Three places must agree on `--toggle/--on/--off/--paper/--eink/--settings/--quit/--status`; adding a command means editing all three, and `main`'s `--settings` maps to the magic literal `99` while the IPC layer has no matching constant (unlike `CMD_*` for everything else). | Inline |
| 7 | Consistency | Low | `linux/mnpaper.c:1-50` vs `linux/AGENTS.md`, `linux/README.md` | The file header still lists "autostart wiring" under "Not ported yet", but autostart is now implemented (`ApplyAutostart`, lines 196-290). The header is the first thing a reader sees and is now actively misleading about shipped functionality. | Inline |
| 8 | YAGNI | Low | `mnPaper.c:527-528`, `mnPaper.c:621`, `mnPaper.c:635` plus `tests/slider_latency.c` and `tests/verify_fixes.c` (Windows tree) | The 8px preview sampler (`BuildPaperPreview`, `ExpandPaperRow`, `PAPER_PREVIEW_SHIFT`/`PAPER_PREVIEW_STEP`) is speculative code for a settings GUI that does not exist, and its tests live in the Windows `tests/` tree, which the Linux port does not build. **Correction (2026-10-22): this row was wrong about the Linux tree.** The sampler was never ported to `linux/` - it appears in no commit under `linux/` (`git log --all -S BuildPaperPreview -- linux/` has no hit), so there is no unused code here and no `(void)BuildPaperPreview;` reference silencing `-Wunused-function`. Its claim of a `test_core.c` preview-character test was wrong from the start: `preview` never appears anywhere in `linux/tests/` history. Nothing was deleted and there is nothing to restore. When the settings GUI lands, port the sampler and its tests from `mnPaper.c` together, or leave both out - never ship the code without the test that covers it. | Inline |
| 9 | Consistency | Low | `linux/mnpaper.c:62-71` | Include ordering is not grouped or sorted (`sys/signalfd.h` sits between `pthread.h` and `stdarg.h`; `sys/mman.h` and `sys/socket.h` follow `string.h`). Minor, but this file has no other style debt and `make` with `-Wall -Wextra` is otherwise clean. | Inline |

## Cross-Scope Findings

| # | Category | Severity | File(s) | Description |
|---|---|---|---|---|
| 10 | Cross-Directory DRY | Medium | `linux/paper_core.h:44-105` vs `mnPaper.c:175-240`, `mnPaper.c:3926-3960` | `SETTINGS`, `PaperDefaultSettings`/`g_s`, `ClampSettingsOf`, and `SetKeyValue` now exist in **both** the Windows single file and the Linux header, with the Windows copy authoritative and the Linux copy a hand-maintained mirror. The comment at `paper_core.h:51` ("Keep the two in step until the settings core is unified") admits the risk. Drift is silent: a field added to one and forgotten in the other compiles cleanly on both platforms and only shows up as a behaviour difference. This is the single largest maintenance liability in the port and is a deliberate deferral, not an accident - it needs its own plan (make `paper_core.h` the one source of truth and have `mnPaper.c` include it). |

Note: findings 1-9 are within-scope; finding 10 is the boundary issue. No duplicate findings were raised between passes.

## Clean Areas

- `linux/tests/test_core.c` - assertions are behavioural (determinism, premultiplied-alpha validity, clamp bounds, `--set` grammar refusals, cancellation). No preview-character test exists in this file and none ever did: the preview sampler and its tests exist only in the Windows tree (`mnPaper.c`, `tests/slider_latency.c`, `tests/verify_fixes.c`) and were not carried into the port, so finding 8's reference to a `test_core.c` preview test was mistaken.
- `linux/Makefile` - the vendored protocol code is committed and the scanner
  rules are order-only on the XML, so a plain `make` never invokes
  `wayland-scanner` and needs only the two libraries and their headers;
  `make regen` is the explicit path for a changed XML. `make clean` and a
  from-scratch `make` both verified, the latter with `wayland-scanner` absent
  from `PATH`.
- The buffer-retirement discipline in `RetireSet`/`BufferRelease`/`FlushRetired` is otherwise sound: the `wl_shm` release contract is honoured, and the deliberate "leak rather than unmap live memory" choice on slot exhaustion is correct and documented.
- The single-instance socket probe, stale-socket reclaim, and exit codes (`0`/`1`/`2`) were read and match the documented contract and the Windows behaviour.

## Post-fix addendum (2026-10-19)

All findings above were fixed in the same session. Fixing finding 2 exposed a
**worse underlying bug that the review missed**, now recorded here for honesty:

- `QueueBake` bailed out when `!g_s.master` (`mnpaper.c`, the guard read
  `if (!o->buf || !o->configured || !g_s.master) return;`), so a hidden output
  **never baked at all**. The "armed while hidden" behaviour documented in
  `PresentBake` and in the README therefore never happened: every show cost a
  full re-bake. This is why the original hidden-bake test showed zero bakes
  rather than a parked one. Fixed by dropping the `master` condition and
  letting `RepaintAll` converge hidden outputs too.
- Fixing finding 4 (the prepare-read loop) briefly introduced an EPIPE
  shutdown path by leaving the display prepared on exit; the final loop keeps
  the prepared/not-prepared states strictly accounted for.
- `ApplyAutostart` rewrote and logged "autostart: enabled" on every settings
  change; it now returns early when the on-disk entry already matches.

Verified after the fixes (live, this session): startup = 1 bake; 2 hidden
changes = exactly 2 bakes; show = 0 new bakes; 3 toggles = 0 new bakes; daemon
stays alive; `--set mode=1` now leaves `mode=0` in the config and `--status`
reports `mode paper`.

## What most deserves attention

1. **Finding 1 (High)** - a persisted `mode=1` that the app cannot honour means the on-disk state, `--status`, and the screen can disagree permanently. Cheap to fix, and it makes every future status read unreliable.
2. **Finding 2 (Medium)** - the hidden-output bake path is subtly broken in a way the comments claim works. It needs a specific sequence to hit, so it will not be caught by the normal test recipe.
3. **Finding 10 (Medium, cross-scope)** - the duplicated settings core is the debt that will actually bite; it should get its own plan rather than be patched incrementally.
