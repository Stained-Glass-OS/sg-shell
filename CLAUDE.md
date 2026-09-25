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
| `sg-mstsc` | **Remote Desktop Connection** -- the outbound half of RDP. A Windows dialog (and mstsc's command line: `.rdp` files, `/v:`, `/f`, `/w:`/`/h:`) that starts FreeRDP's native `sdl-freerdp3` through Wine's `\\?\unix\` path. `mstsc` resolves to it via App Paths (`defaults/60-sg-remote-desktop.reg`), and sg-start lists it. |
| `sg-control` | **Control Panel** -- see "The Control Panel" below. `control.exe` resolves to it via App Paths (`defaults/61-sg-control-panel.reg`); sg-start lists it. |
| `sg-gpresult` | **Group Policy result.** A console tool, like `gpresult /r`, that reports the machine and user Group Policy actually in force -- every setting under the HKLM and HKCU policy branches, read from the live registry -- so an administrator can confirm what an applied policy does. `gpresult.exe` resolves to it via App Paths (`defaults/62-sg-gpresult.reg`). Gate: `test/gpresult-check.sh` plants a machine and a user policy and requires both (and no non-policy key) in the report. |
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

## sg-mstsc: .rdp files are untrusted input

They arrive as email attachments, so sg-mstsc treats them as hostile:

- **Every value is its own quoted argument** to FreeRDP, and server and user
  names are validated to conservative character sets -- a `/drive:` smuggled in
  through an address or user name would share this machine's disks with the
  server. The gate tries exactly that, and expects a refusal.
- **Redirection settings in .rdp files are ignored.** It redirects the clipboard
  (mstsc's default) and nothing else.
- **The password is never on a command line**, where any local user could read
  it from `/proc`: `sdl-freerdp3` prompts in its own dialog, which is also why it
  is the SDL client and not `xfreerdp3` (no terminal to prompt on).

`test/mstsc-check.sh` round-trips awkward strings through `append_arg` and
Wine's command-line-to-argv conversion into a fake native client. It has been
seen to fail: a mutant that never quotes fails it. (The first version of the
test did not -- every real argument is space-free, and CreateProcess resolves an
unquoted path by trying each space-separated prefix -- hence `/sg-echo-args`,
honoured only with `SG_MSTSC_TEST=1`.)

## The Control Panel (sg-control)

`src/control/`: one window, a navigation bar (back, forward, up, a breadcrumb
whose segments navigate, search) over a **page rebuilt from live state on
every visit**. A page is painted items plus real child controls; links are
windows of their own (`SgCplLink`), so Tab, Enter and `IsDialogMessage` reach
everything. Icons are drawn in code at 4x and box-filtered to alpha
(`icons.c`) -- no borrowed artwork, smooth without GDI+.

| File | Applet |
|---|---|
| `home.c` | category view, category pages, All Items + search |
| `system.c` | System; "Change settings" -> `/admin rename` |
| `programs.c` | Programs and Features (Uninstall keys, both views + HKCU) |
| `users.c` | User Accounts, Manage Accounts |
| `datetime.c` | Date and Time |
| `personalize.c`, `wallpaper.c` | Personalization |
| `update.c` | Windows Update (staged updates, apt history) |
| `network.c` | Network and Sharing Center (read-only; adapters are sg-ncpa's) |
| `cpl.c` | hosting .cpl applets through `CPlApplet` |
| `admin.c` | elevation, the spool client, the elevated dialogs |

**Everything is kept where Windows keeps it**, so programs and the shell's
theme read the same settings (Personalization's header comment lists the
values). `control.exe` arguments -- `.cpl` names, keywords, `/name
Microsoft.*` -- map in `main.c`'s `TARGETS`; `ncpa.cpl` goes to `sg-ncpa` when
it is installed. Headless entry points for gates and scripts: `--dump
[section]`, `--set wallpaper|background|accent|mode ...`, `--uninstall NAME`,
`--resolve ARG`, `/admin-do VERB ...` (password on stdin).

### Administration: sg-admind

The elevated token is SYSTEM, unprivileged on the Unix side (ADR 0012), and
renaming the computer, joining a domain, managing accounts and setting the
zone are root's. The Control Panel runs itself with ShellExecute `runas` (the
broker's consent prompt); the elevated copy files a request in
`/run/stained-glass-admin/requests` (sgsystem 0700: only SYSTEM can) and
`sg-admind.path` starts `admin/sg-admind` as root. It reads each request
without following links, requires SYSTEM as its owner, acts only on its fixed
verbs with sg-install's validation, refuses to remove or demote the last
administrator or remove a signed-in account, and answers in `replies/`
(root:sgsystem 0750). Passwords travel in the request file and on the tools'
stdin, never argv, and are never logged. **This makes SYSTEM able to do these
root operations** -- as on Windows, where LocalSystem outranks an
administrator; the list is the boundary.

Why a spool: **Wine gives a Windows program no `AF_UNIX` sockets** (Winsock
returns WSAEAFNOSUPPORT) **and no pipes to a `\\?\unix\` child** (its stdin is
`/dev/null`, its exit code unreadable).

### Things that bit

- **Wine's desktop draws only a BMP, centred or tiled.** Personalization does
  what Windows does with `TranscodedWallpaper`: decode (WIC), fit to the
  screen for the chosen style, save a BMP, point `Wallpaper` at it; the
  original is in `BackgroundHistoryPath0` and `WallpaperSource`.
- **The desktop, reloading its picture, paints over the windows on it** and
  nothing repaints them (a wine-sg bug, not fixed here); the applet asks every
  top-level window to repaint afterwards.
- **A Wine desktop closes when its last user process leaves.** A test that
  runs one `--set` on a fresh `explorer /desktop=shell` kills the desktop;
  keep a program on it.
- **Trixie has no `/etc/timezone`.** The zone is `/etc/localtime`'s target,
  which `GetFinalPathNameByHandleW` resolves.
- **xdotool's capitals reach Wine lower-case under Xvfb** -- type lower case
  in dialog tests.

### Gates

`test/admind-check.sh` runs sg-admind unprivileged against its own spool with
stand-ins for the tools: every operation, and every refusal (bad names,
reserved and system accounts, the last administrator, a signed-in account,
zones outside zoneinfo, a request not owned by SYSTEM, a planted symlink, a
password in the log). `test/control-check.sh` checks each applet's `--dump`
against this machine (accounts vs `getent`, zone vs `/etc/localtime`, apt's
history, `ip addr`, planted Uninstall entries), what `--set` writes (registry
values, the transcoded BMP's pixels), control.exe's argument map, the
elevated path through a real spool, the window (appears, paints, Tab+Enter
navigate), and the "Create an account" dialog driven by the keyboard. Both
have been seen to fail: dropping the owner check, logging passwords, and
writing the accent in the wrong byte order each turn them red.

## Start bar alignment

Start is left-aligned. Centering is a settings option, per David -- a config
value the taskbar reads, not a rebuild.
