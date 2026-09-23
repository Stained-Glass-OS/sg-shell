# sg-shell

The Stained Glass OS shell: Windows 10-style panels docked to Wine's `explorer`
through the AppBar protocol.

Wine's `explorer` keeps owning the shell and its taskbar -- `Shell_TrayWnd`,
the tray protocol, taskbar buttons and position -- because that is what makes
applications behave. The taskbar's Windows 10 look is applied by upgrading that
bar in place (in `wine-sg`). This repo is for the surfaces explorer does not
have: a Start menu, a notification centre, a search panel. See
[ADR 0007](https://github.com/Stained-Glass-OS/stained-glass/blob/main/docs/decisions/0007-shell-strategy.md).

## License

**AGPL-3.0-or-later.** The panels are separate programs speaking a documented
protocol, so they carry no Wine code; the AppBar boundary is a licence boundary
too. Changes to explorer itself live in `wine-sg` under LGPL.
