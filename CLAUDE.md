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
| `sg-settings` | **Settings** (Windows 10's, Win+I, `SystemSettings.exe`, every `ms-settings:` URI) -- the Control Panel's program in the Settings frame, sharing its pages' logic. System, Devices, Network & Internet, Personalization, Apps, Accounts, Time & Language, Ease of Access, Privacy, Update & Security. `defaults/65-sg-settings.reg`; sg-start lists it and its rail's Settings opens it. See "Settings" below. |
| `sg-gpresult` | **Group Policy result.** A console tool, like `gpresult /r`, that reports the machine and user Group Policy actually in force -- every setting under the HKLM and HKCU policy branches, read from the live registry -- so an administrator can confirm what an applied policy does. `gpresult.exe` resolves to it via App Paths (`defaults/62-sg-gpresult.reg`). Gate: `test/gpresult-check.sh` plants a machine and a user policy and requires both (and no non-policy key) in the report. |
| `sg-ncpa` | **Network Connections** (`ncpa.cpl`). A tile per adapter with its status (our own drawn icons), a command bar (Disable/Enable, View status, Change settings -- a UAC shield on the administrator ones for a standard user), and the classic dialogs from in-memory templates: Status (connectivity, media state, SSID, speed, bytes), Network Connection Details (Windows' rows, the lease in local time), Properties (IPv4/IPv6 items; unticking IPv6 disables it), and **Internet Protocol Version 4 (TCP/IPv4) Properties** with the real `SysIPAddress32` fields -- automatic/fixed address and DNS, the class mask filled in on leaving the address, a fixed address forcing fixed DNS, Windows' validation messages. `ncpa.cpl`/`ncpa.exe` resolve to it via App Paths (`defaults/63-sg-network.reg`). `--dump`, `--set-ipv4` and `--open` exist for the gate. |
| `sg-netflyout` | **The taskbar's network icon and flyout.** A notification-area icon (`Shell_NotifyIcon`, drawn at runtime: Wi-Fi bands by signal, a monitor for wired, a cross when not connected) with a tooltip, and the dark Windows 10 flyout above the taskbar: the wired connection, Wi-Fi networks by signal with security and a padlock, Connect with "Connect automatically", the network security key prompt (Next/Cancel, Enter/Escape), "The network security key isn't correct", Disconnect, the Wi-Fi button, "Network & Internet settings" (opens sg-ncpa). sg-session's `sg-run-explorer` starts it with the session. `--dump`, `--connect` and `--open` exist for the gate. |
| `sg-dictate` | **Voice typing** (Win+H). A dark bar at the top centre (mic button with a level ring in the accent colour while listening, "Listening...", a gear for Control Panel > Speech Recognition, close) that never takes the focus; what is said is typed into the program that has it. The engine is sg-session's `sg-dictate` (Parakeet on the CPU). `sg-dictate.exe` resolves via App Paths (`defaults/64-sg-dictate.reg`) for explorer's Win+H. See "Voice typing" below. |
| `sg-zip` | **Compressed (zipped) Folders** -- Wine has no `zipfldr.dll`. Opening a `.zip` (ProgID `CompressedFolder`) browses it read-only like a folder (Name, Type, Compressed size, Password protected, Size, Ratio, Date modified; Enter/double-click opens folders, Backspace/Alt+Up goes up, Back/Forward; a file opens from a temporary copy), "Extract all" / the "Extract All..." verb runs the **Extract Compressed (Zipped) Folders** wizard (destination named like the zip, Browse, "Show extracted files when complete", progress, Replace-or-Skip with "Do this for all conflicts"), and "Compress to ZIP file" on any file or folder makes `<name>.zip` beside it. Our own inflate/deflate. `defaults/75-sg-zip.reg`. See "Compressed folders" below. |
| `sg-media` | **Media Player** -- audio and video, Windows 11 Media Player's layout in the Stained Glass Light palette: an "Open file(s)" bar, the video pane (letterboxed; a drawn album tile for audio), and the transport bar -- seek bar with elapsed/total, now playing ("1 of 3"), Stop, Previous, Play/Pause, Next, mute and volume, full screen. Space/Ctrl+P, Ctrl+S, Ctrl+B/F, Left/Right (5 s, Ctrl 60 s), Up/Down, M/F7, F11/Alt+Enter/double-click, Escape, media keys, the wheel for volume, dropped files. One instance: a second launch hands its files over. DirectShow through winegstreamer. `wmplayer.exe` and 17 audio/video types via `defaults/74-sg-media.reg`. See "Media Player" below. |
| `sg-calc` | **Calculator** (`calc.exe`). Windows 10's Calculator, our own drawing: Standard (immediate execution), Scientific (precedence, parentheses, trigonometry in DEG/RAD/GRAD, 2nd functions, F-E) and Programmer (HEX/DEC/OCT/BIN readouts, QWORD..BYTE, AND/OR/XOR/NOT, Lsh/Rsh, Mod); memory, History, keyboard, copy/paste. `calc.exe` and the `calculator:` URI resolve to it (`defaults/70-sg-calc.reg`); sg-start lists it. See "Calculator" below. |
| `sg-photos` | **Photos** -- the image viewer, Windows 10 Photos' layout in the Stained Glass Light palette: a toolbar (the file's name and "2 of 5"; Open, Zoom in/out, Actual size/Fit, Rotate, Delete, Edit with Paint, Slideshow, File information, Full screen, See more), the picture fitted (never enlarged), arrows on it for the folder's other pictures in Explorer's name order. Reads whatever WIC decodes (animated GIFs animate, the largest icon size, JPEG EXIF orientation). Zoom (Ctrl+wheel at the pointer, +/-, Ctrl+0, Ctrl+1, double-click) and drag to pan; Left/Right/wheel/Home/End; Ctrl+R rotate, Ctrl+S save it rotated, Save a copy; Delete to the Recycle Bin; F5 slideshow, F11 full screen; Alt+Enter file information; Ctrl+C (picture and file); Ctrl+E Paint; Set as background. Images, `photos.exe` and `ms-photos:` via `defaults/73-sg-photos.reg`. See "Photos" below. |
| `sg-taskmgr` | **Task Manager**, Windows 10's. Fewer details (the running apps, End task) and More details: Processes (Apps / Background / Windows processes, CPU, memory, disk shaded by load, totals in the headers, sort, End task, Open file location, Go to details), Performance (CPU, memory, network graphs and figures), Startup (Run keys and Startup folders, Enable/Disable through `StartupApproved`), Users, Details, Services (start/stop/restart). File > Run new task (with administrative privileges = `runas`), Always on top, Update speed. `taskmgr.exe` resolves via App Paths (`defaults/76-sg-taskmgr.reg`); sg-start lists it. See "Task Manager" below. |
| `sg-paint` | **Paint** (`mspaint.exe`), an MS-Paint-class raster editor, our own drawing: Windows 10 Paint's ribbon (File menu; Home: Clipboard, Image -- rectangular/free-form select, select all, invert, delete, transparent selection, Crop, Resize and Skew, Rotate/flip --, Tools -- pencil, fill, text, eraser, colour picker, magnifier --, Brushes (6), Shapes (16, outline/fill), Size, Color 1/Color 2, 20-colour palette + custom row, Edit colors; View: zoom, gridlines, status bar, full screen), a canvas with resize handles, a status bar (cursor, selection, picture size, zoom slider), undo/redo, CF_DIB cut/copy/paste, files through WIC (PNG, JPEG, BMP, GIF, TIFF). App Paths `mspaint.exe` and the pictures' Edit verb: `defaults/71-sg-paint.reg`; sg-start lists it. See "Paint" below. |
| `sg-sticky` | **Sticky Notes.** Borderless notes with a strip in a darker shade (+ new note, ... menu, x close), a RichEdit body (Ctrl+B/I/U, Ctrl+T strikethrough, Ctrl+Shift+L bullets, and a formatting bar), seven colours, Notes list with search, Delete note (asks). One process owns every note; notes save themselves (debounced) to `%LOCALAPPDATA%\Stained Glass\Sticky Notes\<id>.note` and come back at the next start. `stikynot.exe` resolves via App Paths (`defaults/78-sg-sticky.reg`); sg-start lists it. See "Sticky Notes" below. |
| `sg-snip` | **Snipping Tool** (and Snip & Sketch's screen clip). `/clip` -- what explorer's Win+Shift+S and PrtScn run, and the `ms-screenclip:` URI -- freezes the screen, shows it dimmed with the mode bar (rectangle, free-form, window, full screen, close), puts the snip on the clipboard (CF_DIB, PNG; CF_BITMAP synthesized) and shows a "Snip saved to clipboard" toast that opens the editor. Without it: the Snipping Tool window (New, mode, delay 0/3/5/10 s) that grows into the editor -- pen, highlighter, eraser, crop, undo/redo, copy, save as PNG/JPEG/GIF/BMP (WIC) to `Pictures\Screenshots`. `snippingtool.exe` via App Paths (`defaults/72-sg-snip.reg`); sg-start lists it. See "Snipping Tool" below. |
| `sg-charmap` | **Character Map** (`charmap.exe`) -- Wine has none. A font drop-down (every installed family), the font's characters in a 20-column grid (only what `GetFontUnicodeRanges` says it has), a magnified view while the mouse holds a cell or the keyboard moves, "Characters to copy" (in the chosen font) with Select and Copy, and the Advanced view: character set, search by name or `U+XXXX`/`0xXXXX` (a name search narrows the grid; Reset), Go to Unicode. The status bar names the character ("U+00E9: Latin Small Letter E With Acute") and gives Windows' Alt+0nnn keystroke. The font and the view are remembered in `HKCU\Software\Microsoft\CharMap`, as Windows keeps them. `defaults/77-sg-charmap.reg`; sg-start lists it. See "Character Map" below. |
| `sg-mmc` | **The administrative consoles**: `services.msc`, `eventvwr.msc` (and `eventvwr.exe`, as `sg-eventvwr64.exe`), `devmgmt.msc`, `diskmgmt.msc`, `compmgmt.msc` -- our own MMC-style host (console tree, result pane, Actions pane, toolbar, Action menu) and the snap-ins in it. `mmc.exe` resolves to it via App Paths (`defaults/79-sg-admin-tools.reg`); wine-sg 0142 gives the `.msc` files, `mmc.exe`/`eventvwr.exe` launchers, the Start menu's Administrative Tools and 0145 Win+X; the Control Panel has an Administrative Tools page. See "The administrative consoles" below. |
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

## Settings (sg-settings)

`build/sg-settings64.exe` is `src/control/*.c` linked again with its own icon
and a common-controls 6 manifest (`src/settings/`): `main.c` hands over to
`settings_main()` (`settings.c`) when the program is called `sg-settings*` or
`SystemSettings*`, or its first argument is `--settings` or an `ms-settings:`
URI. The headless entry points (`/admin`, `--dump`, `--set`...) serve both,
so the elevated copy of Settings is the Control Panel's elevated dialogs.

- **One page machinery, two frames.** Settings' pages are more entries in
  `g_pages` (`PG_S_*`, `SETTINGS_PAGE_DEFS` in `settings.h`) built with the
  same `pg_*` items and controls; `settings.c` adds the Settings pieces --
  `st_title/st_head/st_toggle/st_combo/st_slider/st_card...`, the switch
  (`SgSetCtl`, `BM_GETCHECK`/`BN_CLICKED`), the navigation pane (`SgSetNav`:
  back, Home, search, the category's pages with the accent bar), Home's
  category tiles, search over titles and keywords. `pg_left_pane` returns no
  pane in Settings, so a Control Panel page (Speech Recognition) shows in the
  Settings frame as it is.
- **Shared, not copied**: Background/Colors/Themes call personalize.c's
  `pers_set_wallpaper/accent/mode` (exported for this); Apps & features is
  programs.c's list and uninstallers (`prog_load/prog_run`); Accounts are
  users.c's accounts and elevated dialogs; About is system.c's facts; Date &
  time is datetime.c's zone; Update is update.c's facts and history;
  Ethernet/Wi-Fi/Status are network.c's adapters and open sg-ncpa and the
  flyout rather than repeat them.
- **The pages by where they keep things**: Windows' own values where Windows
  keeps them (LogPixels, SPI_* for mouse, keyboard, accessibility, the
  notification switches, `StartupApproved` as Task Manager writes it,
  WinINet's proxy with `INTERNET_OPTION_SETTINGS_CHANGED`, the ConsentStore
  for privacy, `HKCU\Software\Classes` for default apps, SetUserGeoID and
  `Control Panel\International` for the region); **sg-session's
  `sg-settingsctl`** for what only the machine has -- sound devices and
  volumes (PipeWire through pactl), Bluetooth (bluez: power, scan, pair,
  connect, remove), the compositor's modes (wlr-randr), night light
  (wlsunset), Power & sleep's timers (swayidle), pending updates (apt); and
  **sg-admind** through the elevated copy for root's: the PC's name (`/admin
  rename-pc`, Windows 10's "Rename your PC"), time zone (`/admin set-zone`),
  time synchronisation (`/admin ntp`), the clock by hand (`/admin set-time`,
  sg-admind's new `time` verb, refused while synchronised), the update check,
  the device-wide privacy switches (`/admin consent CAP on|off`, HKLM).
- **sg-settingsctl answers through a file** (`--out`), as sg-dictate --mics
  does: Wine gives a Windows program no pipes to a native one. `ctl_run`
  polls for the answer, which the helper renames into place whole.
  `SG_SETTINGSCTL` names another helper (the gate's stand-in).
- **Voice typing honours Privacy > Microphone**: with the device's, apps' or
  desktop apps' ConsentStore value `Deny`, `sg-dictate` says so and does not
  listen, and its gear opens `ms-settings:privacy-microphone`.
- **One window**: a second start (another URI, Win+I) hands its page to the
  running window (`WM_COPYDATA` to `SgSettingsWindow`) and exits.
  `--resolve URI` prints the page a URI opens; an unknown name opens Home, as
  on Windows.
- **Things that bit.** Never rebuild a page inside an edit control's own
  `EN_CHANGE` (the rebuild destroys the edit mid-message): post a command and
  rebuild after. A drop-down list's window height is its list's: the page's
  extent counts its edit box only, or every page with a combo scrolls.
  Glyphs are drawn in the accent colour and cached by colour too, or a page
  drawn before an accent change keeps the old one. Wine's `GetGeoInfo` has no
  names: countries come from the locales (`LOCALE_IGEOID`).
- **`SG_SETTINGS_DUMP=<file>`**: after every page is shown or scrolled, the
  window, its rectangle, the page, the category, the accent, the selection
  bar's and the search box's screen points, every text and every control
  (class, id, state, screen centre, text).
- **Gate: `test/settings-check.sh`** (display :120; a stand-in
  sg-settingsctl): the ms-settings: table through `--resolve` (18 URIs);
  `ms-settings:display` through ShellExecute; a second URI going to the
  running window; About showing the device name; Background's picture tile
  making the desktop red (registry and the desktop's pixels); Colors' accent
  tile writing `AccentColor` 0xff417c10 and Settings repainting its
  selection bar in it; Sound's devices and a keyboard-moved slider asking for
  exactly that volume; the microphone switch writing ConsentStore Deny;
  search and Enter; Bluetooth's paired device, Add a device (scan) and Pair;
  Update's pending list; `SystemSettings.exe` through App Paths; Date & time;
  Region; every other page building; with `WINI=1` Win+I on the X keyboard
  (a wine-sg with 0130). Screenshots `build/settings-*.png`. Mutants
  `-DSG_MUTANT_URI` (every URI opens Home) and `-DSG_MUTANT_ACCENT` (the
  neighbouring accent) turn it red, and so does Win+I on a wine-sg without
  0130 (the Control Panel opens).
- **Not yet:** the taskbar does not read Taskbar's switches (alignment,
  small buttons, auto-hide) nor Start's "more tiles"/"full screen"; the lock
  screen does not show the chosen picture; night light, resolution and
  Power & sleep need the Stained Glass compositor (wlopm needs
  wlr-output-power-management, which sg-compositor lacks, so the screen is
  never turned off); multiple displays are not arranged; no Windows Hello,
  Family, Gaming, Phone or Search categories.

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

## Calculator (sg-calc)

`src/sg-calc.c`, one owner-drawn window: every key is a rectangle in a table
rebuilt by `layout()` on each change, so hover, disabled keys (hex digits
outside HEX, MC/MR with nothing stored) and the dump come from one place.

- **One engine for all modes**: an operand stack and an operator stack
  (shunting-yard). Standard gives every operator the same precedence, which
  *is* Windows' immediate execution (2 + 3 x 4 = 20); Scientific and
  Programmer use real precedence and parentheses. `=` again repeats the last
  operator with its right operand; an operator straight after another
  replaces it; `5 + =` is 10, as on Windows.
- **Numbers**: doubles shown to 16 significant digits (exponential outside
  that), trigonometric results rounded to 15 so sin 30 deg is 0.5; Programmer
  keeps 64-bit integers cut to the word size and refuses a digit that would
  not fit.
- **Alt+1/2/3 switch modes**; `WM_SYSCOMMAND SC_KEYMENU` from the keyboard
  is swallowed, or Alt's release entered the (menu-less) menu loop and ate the
  next keys.
- The icon is drawn at build time (`src/sg-calc-icon.py`, PIL) and linked as
  a resource; nothing binary is committed.
- **Gate: `test/calc-check.sh`** (in `make test`): `wine start calc.exe`
  through App Paths on a shell desktop, driven by xdotool keys and clicks
  (key positions from `SG_CALC_DUMP`): 12+30= typed and clicked, repeated =,
  immediate execution, percent, divide by zero, grouping, Backspace, memory,
  F9, Ctrl+C/Ctrl+V (xclip), history; Scientific precedence, parentheses,
  square root, x^2, sin 30, 2nd, x^y, n!; Programmer HEX, FF AND 0F (typed and
  clicked), readouts, BIN disabling digits, BYTE NOT, Lsh; `calculator:scientific`;
  the = key's accent pixel. Screenshots `build/calc-*.png`. Mutants (+ computing
  -, a click pressing the neighbouring key, Standard with precedence) each
  turn it red.

## Task Manager (sg-taskmgr)

`src/taskmgr/`: `data.c` samples (once a second, View > Update speed),
`grid.c` is the list every page shows (our own control: two-line headers with
the total above the name, group rows, heat-shaded cells, the selection kept
across refreshes by PID or name), `main.c` the window, pages, Performance
graphs, Run new task and the dump. The icon is drawn by `gen-icon.py` at build
time. One instance (`Local\StainedGlassTaskManager`); `/startup` opens the
Startup tab; settings in `HKCU\Software\Stained Glass\TaskManager`.

- **What Wine answers.** `NtQuerySystemInformation(SystemProcessInformation)`
  lists every Windows process on the machine's wineserver with real CPU times,
  thread and handle counts and working sets (`PrivatePageCount` and the private
  working set are 0, so Memory is the working set). The Linux processes under
  them are not listed -- they are not Windows processes. `GetSystemTimes` and
  `GlobalMemoryStatusEx` come from `/proc`, so Performance is the whole
  machine's. `GetPerformanceInfo` answers zeros: the counts are summed from the
  process list. Per-process I/O counters are 0 (Disk shows 0.0 MB/s); there is
  no per-process network, so there is no Network column.
- **Names.** A process's name is its `FileDescription`; Wine's builtins carry
  none, so apps go by their window's title and the shell's processes by a
  table in `data.c`.
- **Startup: disabling is recorded as Windows records it** -- a 12-byte
  `StartupApproved\{Run,Run32,StartupFolder}` value, first byte 02 enabled,
  03 disabled (HKCU for per-user entries, HKLM for machine ones) -- and
  wine-sg 0125 (10.0-35) makes wineboot skip the disabled ones (Run, Run32,
  the user's Startup folder), so Disable takes effect at the next sign-in.
- **`taskmgr.exe` is Wine's own first**: CreateProcess and `start` find
  `system32\taskmgr.exe` before App Paths. wine-sg 0121 (10.0-35) makes that
  one hand off to App Paths (as 0072 does for control.exe), and 0123 binds
  Ctrl+Shift+Esc in explorer to `taskmgr.exe`, as is the Win+X menu's Task
  Manager. On a wine-sg without 0121 the gate notes it and starts ours directly.
- **`SG_TASKMGR_DUMP=<file>`** writes, after every refresh, the mode and tab,
  tab/link/button centres in screen coordinates, every process, the
  performance figures, startup entries, users, services, the visible rows of
  the current list with their centres, the selection and the last action.
- **Gate: `test/taskmgr-check.sh`** (Xvfb, a shell desktop, X mouse and keys):
  fewer details lists Clock and not a background process; a CPU-burning test
  process (`test/sg-taskmgr-burn.c`) is listed busy under Background processes
  and in Details (PID, user, x64), and End task (row click, then the button)
  ends the process; Performance's memory total matches `/proc/meminfo` and its
  process count the list; a planted Run entry is listed, Disable writes 03 to
  `StartupApproved\Run` and Enable 02; Users; PlugPlay running in Services;
  File > Run new task starts winemine. Screenshots `build/taskmgr-*.png`.
  Mutants with CPU always 0, an End task that does not terminate, Disable
  writing 02, and no Apps group each turn it red.
## Snipping Tool (sg-snip)

One file, `src/sg-snip.c`; its icon is drawn at build time by
`src/sg-snip-icon.py` (PIL) into `build/`, with a common-controls 6 manifest
(`src/sg-snip.rc`).

- **The screen is frozen first**: `GetDC(NULL)` + `BitBlt` of the virtual
  screen, before anything of ours is shown. Under Wine's shell desktop that
  returns other processes' windows' real pixels (checked), so no per-window
  `PrintWindow` composition is needed. The overlay shows that copy at half
  brightness and the selection at full; what is snipped is cut from the
  frozen copy, never read back from the screen (so the overlay cannot leak
  into it -- the full-screen check proves it).
- **Give the keyboard back before the overlay goes.** Destroying the focused
  topmost overlay left X with no input focus at all under Xvfb, and nothing
  -- not even `SetForegroundWindow` on the editor afterwards -- brought it
  back: the editor looked active and got no keys. The overlay now reactivates
  the window that was in front when the snip began (as Windows does), then
  goes. `SG_MUTANT_NOREFOCUS` turns the gate red.
- **The overlay and toast are owned by a never-shown window**, so neither gets
  a taskbar button (the installed wine-sg still gave the tool-window toast
  one).
- The dump writes with `_wfopen(..., "w")`: text mode, CRLF -- the gate strips
  the `\r`.
- One screen clip at a time (mutex `Local\StainedGlassScreenClip`), so a
  second Win+Shift+S does not stack overlays.
- **Gate: `test/snip-check.sh`** (Xvfb :112, a shell desktop, a four-colour
  target window from `test/sg-snip-target.c`, `test/sg-snip-probe.c` reading
  the clipboard): Escape leaves the clipboard alone; a dragged rectangle is
  exactly the dragged size with every quadrant undimmed, PNG and CF_BITMAP
  present; the toast above the taskbar opens the editor; a pen stroke, crop
  by the corner handle and Enter, undo; Ctrl+S through the Save As dialog
  writes a PNG with the snip and the stroke; full screen, window (exactly the
  window) and free-form (white outside the shape); the tool window's New; the
  `ms-screenclip:` URI. Screenshots `build/snip-*.png`. Mutants
  (`-DSG_MUTANT_OFFSET`, `_ESCAPE_COPIES`, `_NODIB`, `_NOREFOCUS`; run with
  `SG_SNIP_EXE=` and `SG_SNIP_DPY=`) each turn it red.

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

## Media Player (sg-media)

`src/sg-media.c`, one window drawn by us (glyphs at 4x, box-filtered to alpha;
the exe's icon is drawn by `src/sg-media-icon.py` at build time).

- **Playback is DirectShow, decoded by GStreamer.** The graph is built as
  file source -> winegstreamer's own splitter (`CLSID_decodebin_parser`,
  decodebin) -> `Render` on each of its pins; `RenderFile` is only the
  fallback. `RenderFile` picks Wine's native AVI and MPEG splitters first, and
  those know few codecs: an MPEG-4 AVI gave sound and no picture
  (`VFW_S_PARTIAL_RENDER`). `.mpg` (MPEG program streams) plays neither way, so
  it is not associated.
- **It needs GStreamer's plugins, not just its libraries.** With only
  `libgstreamer1.0-0` every file fails (`VFW_E_CANNOT_RENDER`: no typefind, no
  decodebin). sg-image installs `gstreamer1.0-plugins-base`, `-good`, `-ugly`
  (ASF: WMV/WMA) and `gstreamer1.0-libav` (H.264, AAC, WMV, MPEG-4); with just
  those, every associated type was measured playing.
- The video renderer's window is a child of our pane (`IVideoWindow` owner,
  message drain to the main window, so keys and double-clicks reach us).
  Full screen restyles the main window to the monitor and hides the bars.
- Volume is linear 0-100 on screen, `2000*log10(v/100)` hundredths of a dB to
  `IBasicAudio`; it and mute persist in `HKCU\Software\Stained Glass\Media
  Player`. Previous restarts a track more than 3 s in, as players do.
- `SG_MEDIA_DUMP=<file>` writes the state every 200 ms: state, file, duration
  and position, volume, video size and pane rectangle, the seek bar's, Play's
  and Next's screen rectangles, full screen, queue, title, error.
- **Gate: `test/media-check.sh`** (display :214 -- an xvfb-run elsewhere
  grabbed :114). Makes a 20 s WAV, a one-colour H.264 MP4, a VP8 WebM and an
  MPEG-4 AVI; imports the real `.reg`; the WAV opens by association and plays
  (duration, position advancing), Space pauses and resumes, a click at 3/4 of
  the seek bar, Left, Down, M; a second launch plays the MP4 in the same
  window and the pane's pixel is the clip's colour, F11 fills the screen with
  it, Escape; a queue of three and Next twice (WebM, then the AVI with its
  picture). Mutants that ignore pause, ignore seeks, or skip the GStreamer
  splitter (`-DSG_MUTANT_*`) each turn it red. Screenshots in
  `build/media-*.png`.
- **Not yet:** no library/playlist views, no album art or tags, no
  subtitles, no playback speed; `.mpg` and DVDs do not play.

## Photos (sg-photos)

`src/sg-photos.c`, one window drawn by us (toolbar glyphs at 4x, reduced to
alpha; the exe's icon is drawn by `src/sg-photos-icon.py` at build time).

- **Pictures are decoded once into premultiplied BGRA** (every frame of an
  animated GIF composed with its offsets and disposal, up to 500 frames and
  512 MB); rotation turns those buffers. Reduced views go through WIC's Fant
  scaler, cached per size -- `AlphaBlend`'s own reduction drops pixels;
  enlarged ones blit only the visible part.
- **What counts as a picture is what WIC decodes**: its decoders' own
  extension lists (`IWICBitmapCodecInfo::GetFileExtensions`), for the folder
  walk and the Open dialog.
- **Wine's JPEG decoder exposes no EXIF metadata**, so the orientation (and
  date taken) come from the APP1 segment, read by us. Ctrl+S refuses to
  re-encode an EXIF-oriented JPEG or an animation (Save a copy works).
- **Delete is `SHFileOperation` with `FOF_ALLOWUNDO`**: Wine's Recycle Bin
  is the XDG trash. The gate points `XDG_DATA_HOME` at its own directory.
- The associations are HKLM ProgID `StainedGlass.Photos.Image` for .png .jpg
  .jpeg .jpe .jfif .bmp .dib .gif .tif .tiff .ico (Wine's wine.inf registers
  none of them), plus `OpenWithProgids`, `Applications\sg-photos64.exe`,
  App Paths `photos.exe`, and the `ms-photos:` protocol
  (`ms-photos:viewer?fileName=<escaped path>`).
- `SG_PHOTOS_DUMP=<file>` (a Windows path) writes after every paint: file,
  title, index, image size, scale, fit, rotation, EXIF orientation, frames,
  slideshow, full screen, info, and the window, canvas and picture rectangles
  on screen. `SG_PHOTOS_SLIDE_MS` shortens the slideshow's 3 s.
- **Gate: `test/photos-check.sh`** (a display Xvfb picks itself with
  `-displayfd`: fixed numbers collided with other gates' servers). It imports
  the real `.reg`, opens img2.png by association and checks the screen's
  pixel at the picture's centre, Right (img10 after img2: name order) and
  Left twice with the pixels following, zoom in/out/fit, Ctrl+R (dimensions
  swap, drawn taller), Alt+Enter, F5 advancing by itself and Escape,
  Delete-confirm (gone from the folder, in the trash, next shown),
  `photos.exe` via App Paths, a GIF animating, a 3000x1500 picture reduced
  with its halves' colours intact, EXIF orientation 6, and `ms-photos:`.
  Screenshots `build/photos-*.png`. Seen red against mutants: plain
  `lstrcmpi` order, a rotate that does nothing, and no reduced bitmap.
- **Not yet:** no albums/collections, crop or edit tools, printing,
  favourites, or video; rotation of a JPEG is not lossless.
## Paint (sg-paint)

`src/paint/`: `main.c` (window, commands, files, dialogs, the dump),
`ribbon.c` (ribbon and status bar, one item list for layout, drawing, hover,
clicks and the dump), `canvas.c` (zoomed view, tools, selection, text box,
canvas handles), `image.c` (pixels, undo, flood fill, transforms, WIC,
clipboard), `glyphs.c` (the ribbon's pictures, drawn at 4x and box-filtered,
as the Control Panel's icons). The exe's icon is drawn by `gen-icon.py` at
build time (PIL) -- no binary is committed.

- **The picture is one 32-bit top-down DIB section** selected into a memory
  DC, so GDI (shapes, text) and our own loops (pencil, brushes, fill) draw on
  the same pixels. GDI leaves alpha 0; the picture's alpha is ignored
  everywhere, and compared colours are masked to RGB.
- **A selection is lifted on first move** into a floating picture whose
  alpha byte is its mask (free-form shapes, transparent selection), so
  rotate/flip/resize/skew act on it with the same code as on the whole
  picture, and putting it down is one blend.
- **Undo keeps whole copies** (40 steps). A shape being dragged is redrawn
  from the copy undo just took, so there is no separate preview buffer.
- **Save writes 24-bit** (GIF: a fixed 216-colour palette, which WIC's GIF
  encoder needs) to a temporary name and renames, so a failed encode never
  destroys the file.
- **`SG_PAINT_DUMP=<file>`** writes the title, path, tool, colours, sizes,
  undo depth, selection, and the screen rectangles of every ribbon item and
  of the picture's origin, after every change -- the gate clicks from it.
- **Gate: `test/paint-check.sh`** (in `make test`): `mspaint.exe` through App
  Paths on a shell desktop; with the X mouse it picks colours from the
  palette and draws a pencil line, a rectangle, a fill inside it, a line, text
  in a text box, a brush stroke, and a scribble that Ctrl+Z takes back; Ctrl+S
  drives the Save As dialog and the PNG's pixels are checked; a PNG on the
  command line opens, rotates (Ctrl+R) and saves in place; Select all + Copy
  there and Paste in the first window moves pixels over the clipboard;
  Resize 50% and its undo; closing with changes asks, and No leaves the file
  alone; the ribbon paints; the View tab zooms. Windows are brought forward
  with a probe calling `SetForegroundWindow` (no X window manager for
  xdotool). Screenshots `build/paint-*.png`. Mutants whose pencil does not
  draw, whose save writes a blank picture, or whose undo does nothing each
  turn it red (`SG_PAINT_EXE` runs it on another build).
- **Things that bit:** clicking a taskbar button of the window already in
  front minimizes it, and Alt+F4 with the taskbar focused goes to explorer --
  the gate uses the probe and the caption's close button instead.
## Sticky Notes (sg-sticky)

`src/sticky/sg-sticky.c`. Each note is a `WS_POPUP` window we draw ourselves
(strip, buttons, frame, formatting bar) around a RichEdit 4.1 (`msftedit`);
`WM_NCHITTEST` makes the strip a caption and the edges resize borders. The
... menu is a child window laid over the strip: the seven colours, Notes list,
Delete note. Closing a note (x, Ctrl+W) keeps it -- it is in the list and
comes back from there; the process exits once no note and no list is shown.

- **One process.** Mutex `Local\StainedGlassStickyNotes`; a second start
  finds the message-only window `SgStickyHost` (`FindWindowEx(HWND_MESSAGE)`)
  and hands it its command line by `WM_COPYDATA`: nothing (the open notes to
  the front, or the list), `/new`, `/list`, `/quit`.
- **Storage.** One file per note: `StickyNote 1`, `Color=`, `Rect=x,y,w,h`,
  `Open=`, a blank line, then the text as RTF (`EM_STREAMOUT`). Saved 600 ms
  after the last change or move, through a `.tmp` renamed into place, and at
  exit and `WM_ENDSESSION`.
- **Things that bit.** `EM_GETCHARFORMAT` widens `dwMask` to every attribute
  the selection shares; handing that back to `EM_SETCHARFORMAT` with only the
  bold bit in `dwEffects` clears `CFE_AUTOBACKCOLOR` and paints the text on
  black. Keep the mask you asked for. `near` is a macro in `windows.h`.
- **`SG_STICKY_DUMP=<file>`**: every note (text, colour, rectangle, button
  centres on screen), the open menu's swatches and items, the list (items,
  search box), the number of `.note` files; rewritten every 400 ms.
- **Gate: `test/sticky-check.sh`** (display :118): `stikynot.exe` through App
  Paths opens a first note and saves it at once; typed text, Ctrl+B, the ...
  menu's green, a drag of the strip; then `taskkill /f` and a new start: the
  note is back with its text, bold, colour and place, and the screen shows it
  green with the text not on a black block; `/new` handled by the running copy;
  `/list` and its search; Delete note asks and removes only that note's file.
  Screenshots `build/sticky-*.png`. Mutants built with `-DSG_MUTANT_NO_COLOR`
  (the colour not read back) and `-DSG_MUTANT_WIDE_MASK` (the black block)
  (`SG_STICKY_EXE=`) turn it red.

## Character Map (sg-charmap)

`src/charmap/main.c`. The window is a plain top-level window with
`WS_EX_CONTROLPARENT` and `IsDialogMessage`, the grid a control of its own
(`SgCharGrid`, `DLGC_WANTARROWS | DLGC_WANTCHARS`, and Enter), the magnified
view an owned `WS_EX_NOACTIVATE` popup (`SgCharZoom`).

- **Names come from the Unicode Character Database at build time**:
  `gen-names.py` reads Debian's `unicode-data` (`/usr/share/unicode/
  UnicodeData.txt`, `UNICODE_DATA=` to override; a Build-Depends) into
  `build/charmap-names.c` -- the BMP's names in title case as a word list plus
  indices (about 17 000 names). Ideographs, Hangul syllables (the Unicode
  algorithm) and private use are named in code. Nothing generated is
  committed; the data's Unicode License v3 notice is in `debian/copyright`.
- **A label's mnemonic needs `WM_NEXTDLGCTL`**: `IsDialogMessage` sends it to
  the "dialog" for `&Font :` and friends, and only `DefDlgProc` answers it --
  so a non-dialog window must, or Alt+F does nothing. Mnemonics must be
  unique ("C&haracter set" took Alt+H from "Searc&h for").
- **Growing the window leaves the old status bar's pixels** under the
  advanced controls until the whole window is invalidated.
- **Under Xvfb the grid's glyphs render without anti-aliasing** (4 colours);
  the gate measures ink, not a colour count.
- **Gate: `test/charmap-check.sh`** (display :117): `charmap.exe` through
  App Paths on Arial, a font chosen with Alt+F and a letter (and remembered),
  typing a character in the grid selects it, arrows, Enter and Alt+S add to
  "Characters to copy", Alt+C puts exactly that on the clipboard (read by
  `test/sg-charmap-probe.c`), the magnified view while the mouse holds a cell,
  the Advanced view: `u+00e9` selects é with its name and Alt+0233, "omega"
  narrows the grid and Reset restores it, Go to Unicode 20AC is the Euro Sign.
  `+` is typed as Shift held down on the X keyboard (`xdotool type` loses
  Shift). Screenshots `build/charmap-*.png`. Mutants built with
  `-DSG_MUTANT_SELECT` (Select adds the next character) or
  `-DSG_MUTANT_SEARCH` (no `U+` search) via `SG_CHARMAP_EXE=` turn it red.

## The administrative consoles (sg-mmc)

`src/mmc/`: `main.c` is the host -- Windows MMC's layout in our own code: a
menu bar (File, Action -- rebuilt from the verbs on every opening --, View,
Help), a toolbar (Back, Forward, Up, Show/Hide Console Tree, Properties,
Refresh, Export List, Help, then the selection's verbs that have a picture),
the console tree (a TreeView), the result pane (a report ListView, sortable by
any column, or a snap-in's own view) and the Actions pane on the right (our
own drawing: the node's and the selection's verbs as links). A yellow banner
above the result pane carries notices ("You are signed in as a standard
user..."). `util.c` has the sg-sysinfo bridge client, in-memory dialog
templates, formatting and elevation (`runas`). **No MMC snap-in COM**: a
`.msc` file only names the console -- wine-sg 0142 writes ours as an
`MMC_ConsoleFile` with `<StainedGlass Console="services"/>`; any other `.msc`
is recognised by its file name. `sg-eventvwr64.exe` is the same program,
opening Event Viewer by its own name (App Paths values cannot carry
arguments). Pictures: `gen-icons.py` draws a 16px and a 32px strip in
`mmc.h`'s `IC_*` order (the build's check: 56 names = 56 enum entries) and the
program icons, at build time.

- **Snap-in interface** (`snapin_t` in `mmc.h`): `expand` (children, once),
  `show` (columns and rows through `pane_*`, or a custom view), `verbs` (for
  the node, or the selected row), `invoke`, `open` (double-click, Enter,
  Properties), `selchange`, `tick` (once a second), `layout` (a custom view's
  windows), `hide`, `dump`. Row keys are the snap-in's own stable numbers,
  so a refresh keeps the selection.
- **The Linux side** is sg-session's `sg-sysinfo`, through its bridge: the
  console re-launches itself as `sg-sysinfo --bridge wine <itself> --bridged
  ...` at start (as sg-ncpa does with sg-netctl); `SG_SYSINFO` points at
  another copy; `--no-bridge` skips it.
- **`SG_MMC_DUMP=<file>`** (a Windows path) is rewritten after every change:
  title, console, admin/bridged, the selected node, the tree with each item's
  screen centre, the fixed toolbar buttons, columns, every row (key, screen
  centre or -1, selected, cells tab-separated), the verbs (`VERB node|row id
  enabled toolbar-x toolbar-y actions-x actions-y name`), the Actions links,
  banner, status, the last message box's text, and the snap-in's own lines.

### Services (services.msc)

`src/mmc/services.c`. **Services (Local)**: every Win32 service from
`EnumServicesStatusEx` -- Name (display name), Description, Status (blank
when stopped, as Windows 10), Startup Type (Automatic, Automatic (Delayed
Start), Manual, Disabled), Log On As -- with Start, Stop, Pause, Resume and
Restart (toolbar, Actions pane, Action and context menus) and Windows'
"Windows is attempting to start the following service..." progress box
while the state changes; Properties: General (display name, description,
path, startup type, status and its buttons, start parameters), Log On (Local
System + interact with desktop, or an account and password), Dependencies
(both directions). **The console decides nothing**: each step opens the
SCM and the service with only the access it needs, and wine-sg 0141 makes
the SCM refuse a standard user's start/stop/change -- the console shows
Windows' "Windows could not stop the X service on Local Computer. Error 5:
Access is denied." and, for a standard user, a banner. **Stained Glass
System Services**: the Linux units under Windows from `sg-sysinfo units`,
read-only (no verbs; Properties shows the unit's fields).

- **Wine's `sc` has no `qc` or `queryex`**: the gate's test service
  (`test/sg-svc-test.c`, a real start/stop/pause service) answers `info NAME`
  with its state, PID and start type itself.
- **Gate: `test/services-check.sh`** (Xvfb picks its display, a shell
  desktop; `SG_WINE_DIR` may be a build tree): `services.msc` through wine-sg
  0142 (or sg-mmc directly on a Wine without it); a service made with `sc
  create` is listed with its description, stopped, Manual, Local System;
  bridged to sg-sysinfo; the row selects with a click; Start (toolbar) ->
  RUNNING and the row says Running; Pause (toolbar) -> PAUSED; Resume
  (Actions pane); Restart (toolbar) -> a new process; Stop (Actions pane) ->
  STOPPED, blank status; Properties, Alt+U, D, Enter -> start type DISABLED,
  the row says Disabled, Start disabled; Stained Glass System Services lists
  exactly what `sg-sysinfo units` does, with no verbs. 18 checks. Mutants
  (`-DSG_MUTANT_NOSTART`, `-DSG_MUTANT_STATUS`, via `SG_MMC_EXE`) turn it
  red. Screenshots `build/services-*.png`.

### Event Viewer (eventvwr.exe, eventvwr.msc)

`src/mmc/events.c`. The tree: Event Viewer (Local) > Windows Logs
(Application, Security, Setup, System -- those the registry has) and
Applications and Services Logs (every other `HKLM\System\CurrentControlSet\
Services\EventLog` subkey but the service's `Parameters`, and **Stained
Glass**). A log's view is Windows': a header ("Application  Number of events:
N", and the filter), the list (Level, Date and Time, Source, Event ID, Task
Category; newest first) and a preview pane with General (the message, then
Log Name, Source, Logged, Event ID, Task Category, Level, Keywords, User,
Computer) and Details (the record's fields, strings and data in hex). Verbs:
Filter Current Log (levels -- Audit Success/Failure on Security --, Logged
any time/1 h/12 h/24 h/7 d/30 d, sources, IDs with ranges and exclusions),
Clear Filter, Clear Log (save first or not), Save All Events As
(`BackupEventLog`), and per event Copy and Properties (a dialog with
Previous/Next and Copy). `eventvwr /c:<log>` opens that log, `/l:<file>` a
saved one. New events appear by themselves (the count is polled every second).

- **The Windows logs are wine-sg's event log** (0143/0144: the EventLog
  service keeps them and decides who may read them from the caller's token,
  via 0140). A standard user's `OpenEventLog("Security")` fails with 5: the
  list shows "You do not have permission to read the Security log. Access is
  denied." and a banner. Clear is 5 and backup 1314 for a standard user.
- **Messages are formatted as Windows does**: `EventMessageFile` (REG_EXPAND_SZ,
  `;`-separated) of `EventLog\<log>\<source>`, `LoadLibraryEx(AS_DATAFILE)`,
  `FormatMessage(FROM_HMODULE | ARGUMENT_ARRAY)` with the full 32-bit event ID
  and the record's strings (padded to 99 inserts, so a message asking for
  more does not read garbage); categories from `CategoryMessageFile`. With no
  message: Windows' "The description for Event ID N from source S cannot be
  found..." and the strings. The IDs shown are `& 0xFFFF`.
- **The Stained Glass log** is `sg-sysinfo journal --lines 2000` (sg-sysinfod
  filters it to sg-* units and identifiers): Level from the priority (0-2
  Critical, 3 Error, 4 Warning, 5-6 Information, 7 Verbose), Source the
  identifier, Process ID and Unit columns, the message in the preview.
- **Gate: `test/eventvwr-check.sh`** (a SHARED prefix: this user owns it and is
  SYSTEM there, `sgconf` is a standard user through `sudo -u`, `xhost
  +SI:localuser:`): a message DLL built at run time with `windmc`/`windres`
  (`test/sg-evt-msg.mc`) and `test/sg-evt-probe.c` writing events;
  `eventvwr.exe /c:Application` through wine-sg 0142; the registered source's
  event with its formatted text and fields; the unregistered source's
  "cannot be found" text with its strings; an event appearing while open;
  Filter (Warning only; the header says Filtered) and Clear Filter; System's
  6005 "The Event log service was started."; SYSTEM's audit event in Security
  for the owner; the Stained Glass log through the real sg-sysinfo with a
  stand-in `SG_JOURNALCTL` (only sg-* entries); and as the standard user
  Security refused with the banner while Application reads. 22 checks. Stock
  wine-sg fails 14 (the stub event log reads nothing); `-DSG_MUTANT_NOFORMAT`
  (no message files) fails the text checks. Screenshots `build/eventvwr-*.png`.
- **A per-user HKCU**: in the shared-prefix gate the second user needs the
  shell desktop and theme settings in its own hive (`reg add` as that user),
  as a new profile gets them from Default User on the image.

### Device Manager (devmgmt.msc)

`src/mmc/devices.c`, a custom view: a TreeView in the result pane (the
computer, then Windows' categories -- Audio inputs and outputs, Cameras, Disk
drives, Display adapters, Human Interface Devices, Keyboards, Mice, Monitors,
Network adapters, Processors, Storage controllers, System devices, USB
controllers, ...), or by connection (each device under its `PARENT`).
**Two sources**: `sg-sysinfo devices` (the real hardware, the bound kernel
driver and its module's version/file/licence/author, `STATUS nodriver` and
`SG-DRIVER`) and SetupAPI (`SetupDiGetClassDevs(DIGCF_ALLCLASSES |
DIGCF_PRESENT)`: what Wine enumerates for Windows programs -- its display
adapters and monitors, the HID/USB/Bluetooth bus drivers). A Wine device with
a Linux one's PCI/USB vendor and product (`VEN_`/`DEV_`, `VID_`/`PID_`) is
one entry, its Windows instance ID among the properties. Properties: General
(type, maker, location, status in Windows' words -- "This device is working
properly." or "The drivers for this device are not installed. (Code 28)" and
what sg-drivers would install), Driver (the kernel driver and module),
Details (every property). **A device with no driver** gets the warning
picture (Wine's TreeView draws no overlay images, so the icon itself is the
warning), its category opens by itself and a banner says so. Nothing is
changed here: Linux binds drivers, sg-drivers installs third-party ones.

- **Gate: `test/devmgmt-check.sh`**: on this machine through the real
  sg-sysinfo, every display and network adapter sg-sysinfo reports is under
  Display adapters / Network adapters with its driver, SetupAPI devices are
  there, and the display adapter's Properties show its kernel module; then a
  made-up machine (the gate writes a sysfs for `SG_SYSFS`, a `pci.ids`, and
  stand-ins for `SG_DRIVERS`/`SG_DPKG_QUERY`): a VM's bochs display and
  virtio network adapter with their drivers, an NVIDIA card with no driver
  (marked, visible, the banner, Properties "Code 28" and `nvidia-driver`),
  and Devices by connection. 14 checks. Mutants `-DSG_MUTANT_NOLINUX` (only
  SetupAPI) and `-DSG_MUTANT_NOWARN` (no-driver ignored) turn it red.
  Screenshots `build/devmgmt-*.png`.

