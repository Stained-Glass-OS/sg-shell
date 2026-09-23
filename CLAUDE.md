# sg-shell — the Windows 10-style shell panels

Panels that give Stained Glass OS its appearance, docked to Wine's `explorer`
via the AppBar protocol (`SHAppBarMessage`). See
[ADR 0007](https://github.com/Stained-Glass-OS/stained-glass/blob/main/docs/decisions/0007-shell-strategy.md).

Project brief: [`stained-glass/docs/BRIEF.md`](https://github.com/Stained-Glass-OS/stained-glass/blob/main/docs/BRIEF.md).

## The architecture, and why

Wine's `explorer` **stays the shell process and keeps its taskbar**. It owns
`Shell_TrayWnd`, the tray protocol, the taskbar buttons, the taskbar position
and `ITaskbarList` -- the surface applications depend on. The taskbar's
Windows 10 *appearance* is applied by upgrading that bar in place, in
`wine-sg` (patch 0012): most Windows-compatible, since no protocol path
changes.

**This repo is for the surfaces explorer does not have** -- a Start menu, a
notification centre, a search panel. Each is a program of its own, launched by
the shell and, where it docks, registering as an AppBar. That keeps
applications talking to the real shell while our new UI stays on our side of a
clean, AGPL boundary.

**That boundary is also the licence boundary.** These panels are AGPL and carry
no Wine code. Anything that must change inside `explorer` belongs in `wine-sg`
as an LGPL patch, not here.

## Build, test

```sh
make build   # both architectures into build/, needs mingw
make test    # renders each panel headlessly and checks it docks and paints
```

`make test` is the gate: it runs the panel under a headless Xvfb against
`wine-sg`, and checks the taskbar appears, is a full-width strip on the bottom
edge, and actually paints (more than one colour). It skips (77) without mingw,
wine-sg or an X server.

## Panels

| Panel | State |
|---|---|
| `sg-start` | The Start menu: a persistent listener + a dark Win10 panel above the Start button. Lists programs from the Start Menu folders (plus built-ins), launches them, and has a power / lock / sign-out rail. Explorer's Start button toggles it (`SgStartPanel`, `WM_USER+10`); falls back to Wine's menu if it is not running. |
| `sg-taskbar` | **Superseded.** An early standalone AppBar bar, kept as an AppBar/render reference. The taskbar itself is now upgraded in explorer (`wine-sg` patch 0012), not a separate bar -- David's call: upgrade the bar, do not overlay it. |

## What Wine gives us, and what it does not

- **`SHAppBarMessage` is a real implementation** (`ABM_NEW`, `ABM_QUERYPOS`,
  `ABM_SETPOS`, `ABM_GETTASKBARPOS`), so docking and space reservation work.
- **`uxtheme` loads `.msstyles`**, so a Windows 10 visual style for application
  windows and common controls is a theme file plus a registry setting -- no
  code. That is a separate track from these panels.
- **Explorer's own taskbar is NOT themed.** `Shell_TrayWnd` (`systray.c`) draws
  with plain GDI and never calls `OpenThemeData`, so a `.msstyles` will not
  restyle it. Making the bar look Windows 10 is exactly why the bar is one of
  our panels rather than a theme.

## Coexistence with explorer's taskbar (open)

`explorer /desktop=shell` draws its own 20px taskbar. Ours docks over the same
edge. The plan is to keep `Shell_TrayWnd` alive (applications register tray
icons with it) while hiding its visual, and forward what it holds into our bar.
Until that lands both may be visible; the gate runs the panel standalone so it
tests our bar, not the interaction.

## Start bar alignment

Start is left-aligned. Centering is a settings option, per David -- a config
value the taskbar reads, not a rebuild.
