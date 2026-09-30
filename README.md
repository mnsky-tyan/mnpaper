# mnPaper

A soft, generated paper texture that settles over your whole screen - like
working on paper instead of glass. mnPaper is a single portable Windows
executable: no installer, no account, no telemetry.

## What it does

- Lays a warm, procedurally generated paper texture over every monitor.
  Your cursor, clicks and typing pass straight through it.
- **E-ink mode** turns the entire screen into a greyscale, Kindle-style
  reader view (shades / contrast / dither). It asks before switching and is
  always hidden from screen captures.
- Everything applies the moment you move a slider: coarse feedback while you
  drag, full refinement right after you stop.
- The texture is **hidden from screenshots and screen shares by default** -
  other people see the clean desktop, you still see paper. One checkbox
  includes it when you want that.
- Autosaves every setting the moment it changes. There is no Save button.

## Requirements

- Windows 10 (version 2004+) or Windows 11, x64.
- No admin rights, no runtime dependencies.

## Install and run

1. Copy `mnPaper.exe` anywhere in your user profile (for example
   `C:\Users\you\bin`).
2. Run it. A tray icon appears; the texture fades in.
3. Optional: tick **Start with Windows** in the settings window, or run
   `mnPaper.exe --set autostart=1`.

Uninstalling: quit from the tray, delete the exe, and remove the
`HKEY_CURRENT_USER\Software\mnPaper` registry key (settings) plus the
`mnPaper` value under `HKCU\...\CurrentVersion\Run` if you enabled autostart.

## Using it

- **Settings window**: tray icon right-click, or run `mnPaper.exe --settings`.
- **Ctrl+Alt+P** - show / hide the texture (the app keeps running in the tray).
- **Ctrl+Alt+E** - switch between Paper and E-ink mode.
- **Texture on** (checkbox) - hide the texture without quitting.
- **Show texture in screenshots and screen shares** - off by default.
- **Start with Windows** - launch at login.
- **Check for updates** - compares your version with the published one and
  offers to open the download page. It never downloads or installs anything.

### Command line

```
mnPaper.exe --settings
mnPaper.exe --set <setting>=<value>   (intensity, warmth, grain, fibre,
                                       blotch, shades, contrast, dither,
                                       master, mode, share, autostart)
mnPaper.exe --capture on|off          (texture in captures)
mnPaper.exe --on | --off | --toggle
mnPaper.exe --quit
mnPaper.exe --log <file>              (diagnostics; nothing is logged by default)
```

## Privacy

- No telemetry, no analytics, no accounts.
- The only outbound network request mnPaper ever makes is the **manual**
  "Check for updates" button (a plain HTTPS GET of a version text file).
  Everything else stays on your machine.
- Settings live in `HKEY_CURRENT_USER\Software\mnPaper`.

## Building from source

- Visual Studio 2022 (any edition), x64 Native Tools command prompt.
- No third-party dependencies.

```bat
rc /nologo mnPaper.rc
cl /nologo /O2 /W3 mnPaper.c mnPaper.res /FemnPaper.exe
```

Hidden regression suite (windowless, never injects input, uses an isolated
registry hive - safe to run while working):

```bat
cl /nologo /O2 /W3 tests\slider_latency.c /Feslider_latency.exe
slider_latency.exe        REM exit 0 and "RESULT 0 failure(s)" = pass
```

See `tests/README.md` for what the suite covers.

## Known limitations

- The texture cannot cover the secure desktop (Ctrl+Alt+Delete, UAC prompts,
  the login screen): Windows forbids drawing there by design.
- The Start menu, search and notification center run in shell z-order bands
  ordinary apps cannot enter, so they stay untextured.
- An auto-hidden taskbar is covered by the paper while it is parked, and
  floats above the paper when you reveal it. The revealed taskbar itself is
  not textured: mnPaper steps aside for it, because forcing the always-on-top
  paper over a revealed taskbar makes Windows drop the taskbar's own topmost
  state.
- E-ink mode can shimmer on content-heavy screens at low shade counts.
- The exe is not code-signed yet: the first run may show a SmartScreen
  warning ("More info" -> "Run anyway").

## Icon

`mnPaper.ico` is generated from choice 7 of the captain's icon sheet
(`icon_7th_preview.png` keeps the 820px master): layered paper waves with
rounded-corner transparency, packed at 16-256 px.

## Status

Version 2.7.0. Internal engineering notes live in `AGENTS.md`; historical
screen-probing experiments are parked under `probes/`.
