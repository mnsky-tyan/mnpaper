# mnPaper for Linux (Wayland)

The same warm paper texture over your whole screen, as a native Wayland
client. One binary, no runtime dependencies beyond libwayland, no GPU, no
telemetry.

```
make          # builds ./mnpaper
./mnpaper     # paper over every monitor
```

## What works today

- Paper texture over every monitor, on the compositor's **overlay layer**:
  above panels and fullscreen windows.
- Fully **click-through**: every click, hover and keystroke reaches the app
  underneath. The overlay has an empty input region, so the compositor never
  routes input to it.
- **Ctrl-free CLI** for everything (see below): on/off/toggle, live parameter
  changes, status.
- Same texture engine as Windows: identical noise, identical parameters,
  identical defaults. `--dump-tex` produces the same paper the overlay shows.
- HiDPI: renders at the compositor's integer scale (a scale-2 output gets a
  scale-2 buffer).
- Hotplug: monitors appearing, disappearing or changing scale are picked up
  live.
- **Tray icon** (StatusNotifierItem + DBusMenu, no toolkit): left-click
  toggles, scroll steps the strength, and the menu carries the strength and
  warmth presets, autostart, the settings file and Quit.

## What is not ported yet

Each of these is its own piece of work, listed so the gaps are explicit
rather than surprising:

| Feature | Why it is missing |
|---|---|
| **E-ink mode** | Needs screen capture. KWin implements neither `wlr-screencopy` nor `ext-image-copy-capture`, so the backend has to be KDE's PipeWire screencast. `--eink` is refused with a log line and the mode stays paper. |
| Settings GUI | No toolkit linked yet. All settings are reachable via `--set`, the tray menu, or the config file (plain text). |
| Global hotkeys | Needs `ext-global-shortcuts-v1` (KWin has it); not wired up. |
| ~~Autostart~~ | **Done.** Writes `~/.config/autostart/mnpaper.desktop`; see below. |
| Self-update | The Windows feed pins an `.exe`; a Linux release feed would need its own artifact and pin. |

## Command line

```
mnpaper                          run the overlay in the foreground
mnpaper --on | --off | --toggle  show / hide / flip the texture
mnpaper --paper                  paper mode (the only mode for now)
mnpaper --eink                   refused on Wayland, see the table above
mnpaper --set k=v [--set k=v]    change parameters live (or at startup)
mnpaper --status                 print the running instance's state
mnpaper --quit                   stop the running instance
mnpaper --dump-tex FILE W H [bg] headless: bake the texture to a 24bpp BMP
mnpaper --log FILE               append diagnostics to FILE as well as stderr
```

Keys accepted by `--set` (same names and bounds as the Windows registry
values): `master`, `mode`, `intensity` (0-40), `warmth` (0-100), `grain`
(2-12), `fibre` (0-100), `blotch` (0-100), `shades`, `contrast`, `dither`,
`share`, `tbmode`, `autostart`, `autoupd`, `panelgap` (0-512).
`autostart` is applied to the `.desktop` file rather than stored in the
config, and is read back from it.
Values are digits only and must fit an int; unknown keys, malformed and
out-of-range values are refused and logged, never silently zeroed.

```bash
./mnpaper --set warmth=80 --set intensity=40   # warmer, stronger
./mnpaper --set warmth=20                      # cooler
./mnpaper --dump-tex paper.bmp 1920 1080       # see a setting before applying
```

## Tray icon

The daemon exposes `org.kde.StatusNotifierItem` plus a `com.canonical.dbusmenu`
menu over the session bus - no GTK or Qt, just libsystemd's sd-bus. It appears
in the system tray as a small procedurally-drawn paper swatch.

- **Left-click** toggles the paper, exactly like `--toggle`.
- **Scroll** on the icon steps through the strength presets.
- **Right-click menu**: Show paper, Strength (Subtle/Soft/Medium/Strong),
  Warmth (Cool/Neutral/Warm/Aged), Start at login, Edit settings file..., Quit.

Everything funnels through the same `SetMaster`/`ApplySettings` path the CLI
uses, so the menu checkmarks, `--status` and the screen can never disagree.
If the session has no StatusNotifierWatcher (bare wlroots, some GNOME
setups), the tray fails soft with a log line and the CLI keeps working.

### Keeping the panel and tray clickable

A layer-shell overlay is always stacked *above* the panel by the compositor
(KWin maps the overlay layer over the dock layer, and a client cannot be
placed between them), so "do not cover the taskbar" has to mean "do not paint
that strip". Two settings control it:

- `tbmode=0` (the default, matching Windows' TB_NOT_COVER): the bottom
  `panelgap` logical pixels are left uncovered, so a floating panel - and the
  tray icon in it - stays visible and clickable.
- `tbmode=1` (TB_COVER): the veil covers the whole screen including that
  strip, like the Windows mode of the same name.

`panelgap` (0-512, default 96) is how tall that uncovered strip is. Set it to
your panel height: KDE's default floating panel is 72-96 logical pixels, GNOME's
is about 48. Setting `panelgap=0` covers the screen edge to edge.

```bash
./mnpaper --set panelgap=48      # a 48px strip for a GNOME-style panel
./mnpaper --set panelgap=0       # cover everything, panels included
```

Note that a *docked* panel reserves its own space via a positive exclusive
zone and is handled by the compositor without any gap; the gap only matters
for floating panels.

## Autostart (start with the session)

```bash
mnpaper --set autostart=1     # write the XDG autostart entry
mnpaper --set autostart=0     # remove it
```

This is the Linux counterpart of the Windows Run-key entry. It writes
`$XDG_CONFIG_HOME/autostart/mnpaper.desktop`, which KDE, GNOME and every
XDG-compliant desktop launch at login. Points worth knowing:

- **The `.desktop` file is the truth**, exactly like the Windows Run key:
  `autostart` is not stored in `settings.conf`, it is derived from the file on
  every load, so a stored copy can never diverge from reality.
- Removing the entry is guarded by `X-Mnpaper-Exe=`: only an entry naming
  **this** executable is ever removed, so a second copy at another path
  cannot disable the installed one. Enabling writes this executable's path
  into the single `mnpaper.desktop` slot, replacing any existing entry (the
  Windows Run key behaves the same way).
- The `Exec=` value is quoted the way the Desktop Entry spec requires, so a
  path that contains a space still launches at login.
- Install the binary somewhere stable first (`~/.local/bin/mnpaper`); an
  autostart entry pointing into a build directory breaks the moment you
  `make clean`.
- At login the entry can fire before the compositor socket exists. The daemon
  retries the connection for about 20 seconds before giving up, so the
  texture still appears when the session is slow to start.

## Settings file

`$XDG_CONFIG_HOME/mnpaper/settings.conf` (normally
`~/.config/mnpaper/settings.conf`), written atomically on every change.
It is the Linux counterpart of `HKEY_CURRENT_USER\Software\mnPaper`, with
the same key names. The file is canonical: every key is always written, so
it diffs cleanly and survives partial edits.

## How it works

- **`zwlr_layer_shell_v1`** puts a surface on the `overlay` layer, anchored to
  all four edges with exclusive zone `-1` (so panels never shrink it). This is
  the Wayland equivalent of `WS_EX_TOPMOST` + a fullscreen borderless window.
- **Autostart** is an XDG `.desktop` file, applied on every settings change
  (idempotent), and read back at load as the source of truth.
- **`wl_shm` + ARGB8888** is premultiplied BGRA in memory: byte-for-byte the
  format the Windows build paints into its top-down DIBs. That is why the
  generator could be lifted across unchanged.
- **Two buffers, one mapping.** Each output has a shm pool holding a texture
  half and an all-zero transparent half. Showing and hiding is a buffer swap,
  never an unmap: KWin sends no new `configure` to an unmapped layer surface,
  so an unmapped overlay could never come back. Swapping also makes a
  hide/show cycle free (no re-bake).
- **Bakes are gated on `wl_buffer.release`.** The compositor owns a buffer's
  pixels from commit until release; writing into a buffer it may still be
  scanning is a use-after-free in the compositor. Every commit is counted
  per half, and a half is neither written nor attached again until its
  release arrives - a bake that arrives early is parked and re-queued on the
  release event.
- **One bake per real change.** A generation counter advances only when
  texture parameters change, so toggling, `share` or `tbmode` never triggers
  a re-bake. In-flight duplicate bakes are suppressed.
- **Second instance = unix socket** at `$XDG_RUNTIME_DIR/mnpaper.sock`
  (text protocol), replacing Windows' `FindWindow` + `WM_COPYDATA`. A stale
  socket left by a killed instance is detected and reclaimed.

## Resource cost (measured on this machine)

A 2880x1800 desktop at scale 2 (3600x2250 buffer):

| | |
|---|---|
| Idle CPU | **0 ticks per 10 s** (no repaints when nothing changes) |
| One full bake | **1.7 s** single-threaded, off the UI thread |
| RAM, steady | **~95 MB** = 62 MB shm (two buffers at 3600x2250) + ~31 MB process |
| Threads | 2 (UI + one bake worker) |

The 62 MB is double what Windows needs because of the transparent half that
keeps the surface mappable, and because KWin reports integer scale 2 for this
fractional-scale (1.6) panel, so the buffer is larger than the physical panel.
Both are fixable later (fractional-scale protocol, smaller transparent half);
neither affects correctness.

## Requirements

- A compositor implementing `zwlr_layer_shell_v1` and `wl_shm`: KWin (KDE
  Plasma 5.21+), sway, Hyprland, labwc, river, wayfire.
  **GNOME/Mutter does not implement layer-shell**, so the overlay cannot
  exist there; the program says so and exits instead of half-working.
- libwayland-client at runtime; `wayland-scanner` only to regenerate the
  vendored protocol code.

## Building

```bash
make            # ./mnpaper
make test       # headless core tests, no display needed
make clean
```

The vendored protocol code (`protocols/`) is committed, so a normal build
needs no wayland-protocols package. `xdg-shell` is linked because
`wlr-layer-shell` references its `xdg_popup` symbols.

## Testing

`make test` runs the headless suite: defaults, clamping, `--set` grammar,
determinism, premultiplied-alpha validity, odd sizes, cancellation, and
e-ink normalization. It mirrors the spirit of the Windows hidden
suites for the parts this port ships.

Live checks that found real bugs during the port (worth re-running after
touching the buffer or layer-surface code):

```bash
./mnpaper --log /tmp/m.log &       # start
./mnpaper --set warmth=80          # expect exactly one new "baked" line
./mnpaper --off; ./mnpaper --on    # expect NO new "baked" line
./mnpaper --toggle                 # rapid toggles: still no re-bakes
./mnpaper --quit                   # socket and shm must disappear
```

## Known limitations

- The overlay is above everything on the overlay layer, including lock
  screens that use it; most lockers run on their own layer and are unaffected.
- No capture exclusion. Windows' `WDA_EXCLUDEFROMCAPTURE` has no portable
  Wayland equivalent; `share` is stored but changes nothing on Wayland.
- `tbmode` is a Windows taskbar concept: on Wayland it decides whether the
  veil also covers the bottom `panelgap` pixels (see the tray section). It
  cannot move the surface off the panel, because a layer-shell overlay is
  always stacked above the panel by the compositor.
- The texture cannot cover the secure login screen (before a session exists).
- The default texture at `intensity=30` is subtle: on dark content it shifts
  fewer than 8 levels per channel, which is intended.
