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
| `sg-start` | **The Start menu**, Windows 10-class, our own drawing: a rail (menu button that opens it with labels; the user with an initial avatar, Documents, Pictures, Settings, Power: Lock / Sign out / Restart / Shut down, NoClose and StartMenuLogoff honoured), every app from the user's and common Start Menu (recursive, `.lnk`/`.url`, own icons, letter headers, "Recently added"), pinned tiles (`HKCU\Software\Stained Glass\Start\Pinned`), type-to-search over apps and Control Panel settings, dark owner-drawn context menu (Pin/Unpin, Run as administrator = `runas`, Open file location, Uninstall), keyboard (arrows, Tab, Enter, Escape). Explorer's Start button and the Windows key toggle it (`SgStartPanel`, `WM_USER+10`); Wine's menu is the fallback. See "The Start menu" below. |
| `sg-mstsc` | **Remote Desktop Connection** -- the outbound half of RDP. A Windows dialog (and mstsc's command line: `.rdp` files, `/v:`, `/f`, `/w:`/`/h:`) that starts FreeRDP's native `sdl-freerdp3` through Wine's `\\?\unix\` path. `mstsc` resolves to it via App Paths (`defaults/60-sg-remote-desktop.reg`), and sg-start lists it. |
| `sg-control` | **Control Panel** -- see "The Control Panel" below. `control.exe` resolves to it via App Paths (`defaults/61-sg-control-panel.reg`); sg-start lists it. |
| `sg-gpresult` | **Group Policy result.** A console tool, like `gpresult /r`, that reports the machine and user Group Policy actually in force -- every setting under the HKLM and HKCU policy branches, read from the live registry -- so an administrator can confirm what an applied policy does. `gpresult.exe` resolves to it via App Paths (`defaults/62-sg-gpresult.reg`). Gate: `test/gpresult-check.sh` plants a machine and a user policy and requires both (and no non-policy key) in the report. |
| `sg-ncpa` | **Network Connections** (`ncpa.cpl`). A tile per adapter with its status (our own drawn icons), a command bar (Disable/Enable, View status, Change settings -- a UAC shield on the administrator ones for a standard user), and the classic dialogs from in-memory templates: Status (connectivity, media state, SSID, speed, bytes), Network Connection Details (Windows' rows, the lease in local time), Properties (IPv4/IPv6 items; unticking IPv6 disables it), and **Internet Protocol Version 4 (TCP/IPv4) Properties** with the real `SysIPAddress32` fields -- automatic/fixed address and DNS, the class mask filled in on leaving the address, a fixed address forcing fixed DNS, Windows' validation messages. `ncpa.cpl`/`ncpa.exe` resolve to it via App Paths (`defaults/63-sg-network.reg`). `--dump`, `--set-ipv4` and `--open` exist for the gate. |
| `sg-netflyout` | **The taskbar's network icon and flyout.** A notification-area icon (`Shell_NotifyIcon`, drawn at runtime: Wi-Fi bands by signal, a monitor for wired, a cross when not connected) with a tooltip, and the dark Windows 10 flyout above the taskbar: the wired connection, Wi-Fi networks by signal with security and a padlock, Connect with "Connect automatically", the network security key prompt (Next/Cancel, Enter/Escape), "The network security key isn't correct", Disconnect, the Wi-Fi button, "Network & Internet settings" (opens sg-ncpa). sg-session's `sg-run-explorer` starts it with the session. `--dump`, `--connect` and `--open` exist for the gate. |
| `sg-dictate` | **Voice typing** (Win+H). A dark bar at the top centre (mic button with a level ring in the accent colour while listening, "Listening...", a gear for Control Panel > Speech Recognition, close) that never takes the focus; what is said is typed into the program that has it. The engine is sg-session's `sg-dictate` (Parakeet on the CPU). `sg-dictate.exe` resolves via App Paths (`defaults/64-sg-dictate.reg`) for explorer's Win+H. See "Voice typing" below. |
| `sg-zip` | **Compressed (zipped) Folders** -- Wine has no `zipfldr.dll`. Opening a `.zip` (ProgID `CompressedFolder`) browses it read-only like a folder (Name, Type, Compressed size, Password protected, Size, Ratio, Date modified; Enter/double-click opens folders, Backspace/Alt+Up goes up, Back/Forward; a file opens from a temporary copy), "Extract all" / the "Extract All..." verb runs the **Extract Compressed (Zipped) Folders** wizard (destination named like the zip, Browse, "Show extracted files when complete", progress, Replace-or-Skip with "Do this for all conflicts"), and "Compress to ZIP file" on any file or folder makes `<name>.zip` beside it. Our own inflate/deflate. `defaults/75-sg-zip.reg`. See "Compressed folders" below. |
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

## The Start menu (sg-start)

One file, `src/sg-start.c`. The list is rebuilt on every opening (a scan of
the two Programs trees) but icons are cached by path and the first build
happens at start-up, so it opens in well under 200 ms with 200 apps.

- **Icons: `ExtractIconEx`, not `SHGetFileInfo`.** Wine's `SHGetFileInfo`
  gives every program the generic icon; a shortcut's icon location (or its
  target) goes through `ExtractIconEx`. **Wine's `SHGetStockIconInfo`
  answers `S_OK` with no icon**: the fallbacks are `SHGetFileInfo` of the
  Windows folder (File Explorer -- Wine's explorer carries no icon) and of a
  `program.exe` by attributes. Settings and the Control Panel get a drawn
  gear on the accent colour. COM is initialised for `IShellLink`.
- **"Recently added"** is what was created after this user's Start menu
  first ran (`FirstRun` in the Start key) and within a week -- otherwise a
  fresh profile would call everything recent.
- **Menus are owner-drawn** (dark), so their mnemonics come through
  `WM_MENUCHAR`, and the dump takes their labels from our own table.
- **`SG_START_DUMP=<file>`** writes what is shown after every paint (UTF-8,
  no BOM -- `ccs=UTF-8` writes one and broke the first key's match): the
  window rectangle, the rows, tiles, selection, search, open menu and its
  items, the last launch, and how long the last opening took.
- **Gate: `test/start-check.sh`** (in `make test`; `SG_WINE`/`SG_WINESERVER`
  for another Wine build). It plants shortcuts (one three folders deep, 200
  fillers, one after start-up) with `test/sg-start-mklnk.c`, joins the shell
  desktop, and drives the menu with xdotool: placement, speed, apps, icons,
  headers, Recently added, default tiles, arrows/Tab, search (apps and
  settings), Escape twice, Enter launching, pin/unpin through the context
  menu and the registry, the rail, the power menu and `NoClose`, click-away.
  Screenshots: `build/start-{open,search,context,power,rail}.png`. xdotool's
  window geometry is not the panel's in a Wine desktop -- the gate reads
  the rectangle from the dump.

## Voice typing (sg-dictate)

`src/sg-dictate.c`. Explorer's Win+H (wine-sg 0092) runs `sg-dictate.exe
/toggle`: open the bar and listen, or stop and close it. It re-launches
itself through the engine's bridge (`sg-dictate --bridge wine <itself>
--bridged`, `SG_DICTATE` overrides the engine's path, as `SG_NETCTL` does);
the protocol is in sg-session's CLAUDE.md. One instance: a second hands its
command to the first (`WM_COPYDATA`, class `SgDictateBar`, mutex
`Local\StainedGlassVoiceTyping`) and exits.

- **It never takes the focus**: `WS_EX_NOACTIVATE` (and `MA_NOACTIVATE`,
  `SWP_NOACTIVATE`), topmost, a tool window owned by a hidden window (no
  taskbar button under any taskbar rule). Text goes to whatever has the
  focus: `SendInput` with `KEYEVENTF_UNICODE` (a line break is the Enter
  key), or, if the user chose it, the clipboard and Ctrl+V -- the clipboard's
  text is put back afterwards.
- **Hold-to-talk**: with `HoldToTalk` on, `sg-dictate /background` (started
  with the session by sg-session's `sg-run-explorer`; exits at once if the
  setting is off) keeps a `WH_KEYBOARD_LL` hook. The key (default Right Ctrl)
  held alone for 250 ms opens the bar and listens; released, it stops, and
  the bar goes once the text is in. With another key it is a shortcut and
  nothing happens; the key is never swallowed. The model is unloaded after 5
  minutes unused. `/reload` tells a running instance the settings changed.
- **Settings** are `HKCU\Software\Stained Glass\Speech`, read at every
  start, so Control Panel changes apply at once.
- **Control Panel > Speech Recognition** (`src/control/speech.c`; `control
  /name Microsoft.SpeechRecognition`, the bar's gear): on/off -- turning it on
  runs `sg-dictate --download` (sg-speechd downloads the model as root) and
  shows progress from its status file; the microphone (from `sg-dictate
  --mics --out`), "Test microphone" (`sg-dictate --meter`, a level in a temp
  file); hold-to-talk and its key; continuous dictation, automatic and spoken
  punctuation, filler words, numbers, typing or pasting, language; privacy
  and the model's CC BY 4.0 attribution. Wine gives a Windows program no
  pipes to a native one, so the engine answers through files.
  `--dump speech` for gates.
- **Gate: `test/dictate-check.sh`** (Xvfb, a shell desktop, Notepad in
  front): `/toggle` through App Paths, a stand-in engine's words typed into
  Notepad and read back with `WM_GETTEXT`, the bar's styles and place, Notepad
  keeping the focus throughout, the settings in the start request, `/toggle`
  closing it, pasting with the clipboard given back, "off", hold-to-talk on
  the X keyboard (and Right Ctrl+C left alone), the Control Panel page (the
  dump, the download's progress, the level meter, writing a setting), and --
  when a model is at hand -- the real engine typing spoken words. `WINH=1`
  presses Win+H on the X keyboard too (a wine-sg with 0092). Screenshots in
  `build/dictate-*.png`. Mutants that drop `WS_EX_NOACTIVATE`, stop typing,
  skip giving back the clipboard or the chord guard each turn it red.

## Start bar alignment

Start is left-aligned. Centering is a settings option, per David -- a config
value the taskbar reads, not a rebuild.

## The network programs: sg-ncpa and sg-netflyout

They show and change network settings only through **sg-netctl**
(sg-session; NetworkManager behind it). Wine has no AF_UNIX and gives a native
program started from a GUI process no stdin or stdout, so each re-launches
itself as `sg-netctl --bridge wine <itself> --bridged ...` and talks JSON lines
on the pipes it is given (`src/sg-netclient.h`; requests run on a worker
thread while the window keeps pumping messages). **They decide nothing**:
sg-netd lets a standard user join Wi-Fi and refuses them adapter settings,
and the programs say so in Windows' words. They carry a common-controls 6
manifest (`src/sg-net.rc`) for the visual style and `BCM_SETSHIELD`; tab pages
call `EnableThemeDialogTexture`, or every label paints a grey box on the tab.

- **The flyout places itself from `Shell_TrayWnd`'s rectangle.** Under Wine
  `ABM_GETTASKBARPOS` answered TRUE with an empty rectangle, and the work area
  includes the taskbar. It is owned by the (never shown) tray window, so it
  gets no taskbar button.
- **`GetNumberFormat` prints 0 as nothing** with `LeadingZero = 0`.
- **Gate: `test/net-ui-check.sh`** runs both programs under Wine against the
  real sg-netctl bridge and sg-netd with a stand-in nmcli: the tiles and every
  Details row, the IPv4 dialog's OK as exactly one `nmcli connection modify`
  (and refused for a standard user, and for a bad mask or an off-subnet
  gateway before anything is asked), the flyout's list, Connect with the key in
  the saved profile and on no command line, a wrong key, and the windows
  through the programs' own re-launch -- with screenshots in
  `build/net-ui-*.png`. **The test prefix must put programs on the shell's
  desktop** (`HKCU\Software\Wine\Explorer\Desktop=shell`, as
  sg-run-explorer does), or every window is a bare X window outside it with no
  caption. The real NetworkManager, radio and DHCP are sg-image's `make net-test`.

## Compressed folders (sg-zip)

`src/zip/`: `zipcore.c` is the format, written from RFC 1951 and PKWARE's
APPNOTE -- inflate (stored, fixed and dynamic blocks), deflate (LZ77 over hash
chains, each block the cheapest of dynamic Huffman, fixed or stored; about
zlib's size on text), CRC-32, the reader (ZIP64 sizes and offsets, UTF-8 or
CP437 names) and the writer (UTF-8 flag only when needed; a `.part` file
renamed into place, so a failure leaves no half zip). **No zlib, nothing
bundled.** `main.c` is the command line, the browse window and the wizard.

- **Archives are hostile input.** `zip_safe_path` refuses a name that is
  absolute, has a `:` (a drive or an NTFS stream), or has a component made
  only of dots and spaces (`..`, and `...`, which Windows trims to it); the
  entry is logged `REFUSED` and never written. Every entry is checked against
  its CRC before the file is created, and inflate never writes past the
  declared size (an entry that lies about its size fails, it does not overrun).
- **Headless**: `/extract <zip> <dir> [/overwrite|/skip] [/quiet] [/log f]`,
  `/create <items...> [/out zip]`, `/list <zip> /log f`; exit 1 when anything
  failed or was refused. `SG_ZIP_DUMP=<file>` writes what the windows show;
  `SG_ZIP_NO_SHOW=1` stops them opening Explorer afterwards.
- **Wine's `SHGetFileInfo` type name is ".ext file"**; we show Windows'
  "EXT File". The Replace-or-Skip prompt is a `TaskDialogIndirect` (hence
  the common-controls 6 manifest, `src/zip/sg-zip.manifest`).
- **Not yet:** password-protected (ZipCrypto/AES) entries are listed but not
  extracted; no drag-out or copy from the browse window; entries over 4 GB are
  not written (ZIP64 is read only); no "Send to > Compressed (zipped) folder"
  shortcut (that is a per-user SendTo item, sg-session's to plant). A
  multi-selection "Compress to ZIP file" gets one zip per item (a static verb).
- **Gate: `test/zip-check.sh`** (display :115): Python-made zips (deflate at
  level 9, stored, nested, unicode names, an empty file and folder) extract
  byte-identically with their dates; `/skip`, `/overwrite`; seven zip-slip
  names refused with nothing written outside; a damaged stored entry caught by
  its CRC; our zips pass Python's `testzip`, content equality and `unzip -t`
  and round-trip; `<name>.zip`, then `(2)`; the browse window through the
  `.zip` association, keyboard navigation, the wizard typed into and its
  Replace prompt. Screenshots `build/zip-*.png`. Mutants built with
  `-DSG_MUTANT_NOCRC` or `-DSG_MUTANT_TRAVERSAL` (`SG_ZIP_EXE=`) turn it red.

