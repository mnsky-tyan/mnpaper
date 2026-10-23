# mnPaper for Linux (Wayland)

The same warm paper texture over your whole screen, as a native Wayland
client. One binary, no runtime dependencies beyond libwayland and
libsystemd, no GPU, no telemetry.

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
- HiDPI: renders at the compositor's preferred fractional scale
  (`wp_fractional_scale_v1` + `wp_viewport`), so a 160% panel gets a buffer
  of exactly its device pixels instead of a rounded-up integer scale. The
  fractional path needs both protocols; a compositor offering only one, or
  neither, gets the integer `wl_output.scale`.
- Hotplug: monitors appearing, disappearing or changing scale are picked up
  live.
- **Tray icon** (StatusNotifierItem + DBusMenu, no toolkit): left-click
  toggles, scroll steps the strength, and the menu carries the strength,
  warmth, grain, fibre and blotch presets, autostart, the settings file and
  Quit.

## What is not ported yet

Each of these is its own piece of work, listed so the gaps are explicit
rather than surprising:

| Feature | Why it is missing |
|---|---|
| **E-ink mode** | Needs screen capture. KWin implements neither `wlr-screencopy` nor `ext-image-copy-capture`, so the backend has to be KDE's PipeWire screencast. `--eink` is refused with a log line and the mode stays paper. The three e-ink knobs (`shades`, `contrast`, `dither`) are stored but inert until then. |
| **Settings GUI** | No toolkit linked. Every setting is reachable through the tray menu, `--set` or the plain-text config file; what is missing is the Windows dialog's single window with live sliders. A GTK/Qt dialog is its own piece of work and is deliberately not started. |
| **Texture in screenshots (`share`)** | Stored, but nothing acts on it: Wayland has no capture-exclusion protocol here, and a layer surface is not a Plasma window, so `excludeFromCapture` cannot be reached. Unlike the Windows build, the paper is **not** excluded from screen shares on Wayland. |
| Global hotkeys | Needs `ext-global-shortcuts-v1` (KWin has it); not wired up. |
| ~~Autostart~~ | **Done.** Writes `~/.config/autostart/mnpaper.desktop`; see below. |
| Self-update | The Windows feed pins an `.exe`; a Linux release feed would need its own artifact and pin. `autoupd` is stored but inert. |

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
`share`, `tbmode`, `autostart`, `autoupd`, `panelgap` (0-512). `cfgver` is
written by the program for its own migration and is not meant to be set.
`autostart` is applied to the `.desktop` file rather than stored in the
config, and is read back from it.
The keys that change the Linux image are `grain`, `fibre`, `blotch`,
`warmth`, `intensity`, `master`, `mode`, `tbmode` and `panelgap`.  `shades`,
`contrast` and `dither` are e-ink parameters and, like `share`, are stored
but change nothing until e-ink mode is ported (see Known limitations);
`autoupd` is likewise stored but inert, because self-update is not ported
(see What is not ported yet).
Values are digits only and must fit an int.  Unknown keys and malformed
values are refused and logged.  A refused operand names itself, applies
nothing and exits 2, whether or not an instance is already running;
`--dump-tex` is the exception: an unknown or malformed entry there is
logged and the BMP is still written, while a `--set` with no pair at all is
refused on every path, `--dump-tex` included.  Every `--set` on one command
line is applied as a single batch, so no pair is visible before the last
one; a batch beyond 64 pairs is refused, and on the `--dump-tex` path the
excess pairs are ignored like any other bad entry.  An in-range-shaped value
that is outside a key's bounds is not refused: it is accepted and silently
clamped to the nearest bound, so `--set intensity=999` answers
`ok applied 1` and leaves `intensity` at 40.  `--status` prints the values
actually in effect.

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
  Warmth (Cool/Neutral/Warm/Aged), Grain (2..12), Fibre (0..100 in tens),
  Blotch (0..100 in tens), Start at login, Edit settings file..., Quit.

Grain, fibre and blotch are the three texture parameters the menu used to
leave out; each submenu has ten steps spanning the same bounds the clamp
table enforces, and every list contains the setting's default so a fresh
install shows a checked row rather than none. A value set by hand (`--set
fibre=37`, say) matches no preset, so no radio is checked - the honest
answer, and what the strength and warmth submenus already did.

Everything funnels through the same `SetMaster`/`ApplySettings` path the CLI
uses, so the menu checkmarks, `--status` and the screen can never disagree.
If the session has no StatusNotifierWatcher (bare wlroots, some GNOME
setups), the tray logs that and the CLI keeps working: the daemon keeps
watching for the name, so a watcher that shows up later - at login
`plasmashell` often comes up after the daemon - is joined and the icon
appears without restarting it. A watcher that is replaced later (a
`plasmashell` restart or crash) is picked up the same way, so the icon comes
back without restarting the daemon.

### Keeping the panel and tray clickable

A layer-shell overlay is always stacked *above* the panel by the compositor
(KWin maps the overlay layer over the dock layer, and a client cannot be
placed between them), so "do not cover the taskbar" has to mean "do not paint
that strip". Two settings control it:

- `tbmode=0` (the default, matching Windows' TB_NOT_COVER): the bottom
  `panelgap` logical pixels are left uncovered. With the default `panelgap=0`
  that is no strip at all - the paper reaches the bottom edge, and the surface
  asks the compositor for exclusive zone 0, i.e. "move me clear of surfaces
  that reserve space". Whether that moves it is up to the compositor (the
  protocol says its use of the request is implementation-dependent), so a
  panel that ends up under the paper is a `panelgap` case; see below.
- `tbmode=1` (TB_COVER): the veil ignores every other surface's exclusive
  zone (zone -1) and covers the whole screen, like the Windows mode of the
  same name.

`panelgap` (0-512, default **0**) is how tall that uncovered strip is. Zero is
right for a panel that reserves its own space (zone 0 asks the compositor to
move the surface clear of it, and a compositor that honours the request
leaves the panel uncovered) and for an auto-hiding panel (it reserves nothing,
so there is no strip to leave clear and the paper reaches the edge). A
non-zero value is for a **floating** panel that reserves nothing yet must stay
uncovered - the one case the compositor cannot infer - and for any panel a
compositor leaves under the paper. KDE's default floating panel is 72-96
logical pixels, GNOME's about 48.

This default used to be 96, which painted a 96px hole under panels that were
not there. An existing config is migrated once: `cfgver` records the schema,
and a file that does not carry that key at all and holds exactly the old
default 96 becomes 0. Any other stored value is treated as a real choice and
kept, and so is a 96 in a file that already has the key.

```bash
./mnpaper --set panelgap=48      # a 48px strip for a floating panel that needs one
./mnpaper --set panelgap=0       # no extra strip (the default)
```

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
  all four edges, with a tbmode-dependent exclusive zone (`0` by default, `-1`
  with `tbmode=1`). This is the Wayland equivalent of `WS_EX_TOPMOST` + a
  fullscreen borderless window.
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
  release event. A live resize retires the whole old buffer set and frees
  its mapping once the compositor has released both halves.
- **One bake per real change.** A generation counter advances only when
  texture parameters change, so toggling, `share` or `tbmode` never triggers
  a re-bake. In-flight duplicate bakes are suppressed.
- **Second instance = unix socket** at `$XDG_RUNTIME_DIR/mnpaper.sock`
  (text protocol), replacing Windows' `FindWindow` + `WM_COPYDATA`. A stale
  socket left by a killed instance is detected and reclaimed.

## Resource cost (measured on this machine)

A 2880x1800 desktop at 160% (a 2880x1646 buffer with a 96 px bottom panel
strip left clear - `panelgap=96`, the default when this was measured; the
current default 0 keeps the full 2880x1800):

| | |
|---|---|
| Idle CPU | **0 ticks per 10 s** (no repaints when nothing changes) |
| One full bake | **~1.0 s** single-threaded, off the UI thread |
| RAM, steady | **~40 MB** process (idle), **~58 MB** peak during a bake |
| shm | **36 MiB** = two 2880x1646 buffers |
| Threads | 2 (UI + one bake worker) |

The shm is double what the paper alone needs because of the transparent half
that keeps the surface mappable. The buffer is the physical device size: this
panel is 1.6 fractional, and the viewport carries the scale (an earlier build
rounded up to integer scale 2, a 3600x2250 buffer, and cost ~1.7 s per bake).

## Requirements

- A compositor implementing `zwlr_layer_shell_v1` and `wl_shm`: KWin (KDE
  Plasma 5.21+), sway, Hyprland, labwc, river, wayfire.
  **GNOME/Mutter does not implement layer-shell**, so the overlay cannot
  exist there; the program says so and exits instead of half-working.
- libwayland-client and libsystemd (sd-bus) at runtime, plus their development
  headers to build (`libwayland-dev`/`wayland-devel`,
  `libsystemd-dev`/`systemd-devel`); `wayland-scanner` only to regenerate the
  vendored protocol code.
- `dbus-daemon` only to run the test suite: the tray case starts a private
  session bus of its own. It is a test-time dependency, not a runtime one -
  the daemon talks to whatever session bus the desktop provides.

## Building

```bash
make            # ./mnpaper
make test       # headless tests (core + tray), no display needed
make clean
```

The vendored protocol code (`protocols/`) is committed, so a normal build
needs neither a wayland-protocols package nor `wayland-scanner`. `xdg-shell` is
linked because `wlr-layer-shell` references its `xdg_popup` symbols. After
editing a protocol `.xml`, run `make regen` to re-scan it.

## Testing

`make test` runs two binaries, both headless.

The first is the core suite: defaults, clamping, `--set` grammar,
determinism, premultiplied-alpha validity, odd sizes, cancellation, e-ink
normalization, the one-shot panelgap migration, and the config-file round
trip. It mirrors the spirit of the Windows hidden
suites for the parts this port ships. It also spawns the real `mnpaper`
binary to pin the CLI refusals (a malformed or over-long `--set` operand,
a batch past 64 pairs, a dangling `--set`): each child gets a private
`XDG_RUNTIME_DIR`, a private `XDG_CONFIG_HOME` and a nonexistent
compositor, so a regression fails on its own instead of touching the
running session. That is why `make test` builds the daemon as well.

The second is the tray case. It starts a private `dbus-daemon`, runs the
real tray code on it and drives it with the messages a panel sends, so it
never touches the session bus, the live daemon or the desktop. It checks
that the bounds on batched events and property queries hold, that
`Activate`, `Event` and `GetLayout` do what they claim, and it watches the
bus the way a host does: it asserts that a `PropertiesChanged` really
carries the `Status` property, and that `ItemsPropertiesUpdated` arrives
with the body the DBusMenu spec declares (`a(ia{sv})a(ias)`), carrying
every row whose state can move - the texture radios included. Those two
fail silently - a typed consumer such as dbusmenu-qt5 drops a signal with
the wrong element type, and a property change that never leaves the process
leaves a stale icon - so only the wire can catch them. The case skips
itself when `dbus-daemon` is not installed.

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
  veil ignores other surfaces' exclusive zones (see the tray section). It
  cannot move the surface off the panel, because a layer-shell overlay is
  always stacked above the panel by the compositor - an auto-hiding panel is
  therefore drawn *under* the paper when it slides in.
- The texture cannot cover the secure login screen (before a session exists).
- The default texture at `intensity=30` is subtle: on dark content it shifts
  fewer than 8 levels per channel, which is intended.
