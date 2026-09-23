# sg-shell

The Stained Glass OS shell: Windows 10-style panels docked to Wine's `explorer`
through the AppBar protocol.

Not a replacement for the shell process. Wine's `explorer` keeps owning
`Shell_TrayWnd`, the tray protocol and AppBar registration -- the surface
applications actually talk to -- because that is what makes applications behave.
These panels dock alongside it as any third-party taskbar would, and supply the
Windows 10 appearance. See
[ADR 0007](https://github.com/Stained-Glass-OS/stained-glass/blob/main/docs/decisions/0007-shell-strategy.md).

## License

**AGPL-3.0-or-later.** The panels are separate programs speaking a documented
protocol, so they carry no Wine code; the AppBar boundary is a licence boundary
too. Changes to explorer itself live in `wine-sg` under LGPL.
