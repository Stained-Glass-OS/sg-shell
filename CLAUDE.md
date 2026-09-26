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
| `sg-start` | **The Start menu**, Windows 10-class, our own drawing: a rail (menu button that opens it with labels; the user with an initial avatar, Documents, Pictures, Settings, Power: Lock / Sign out / Sleep and Hibernate (when logind allows) / Restart / Shut down, NoClose and StartMenuLogoff honoured), every app from the user's and common Start Menu (recursive, `.lnk`/`.url`, own icons, letter headers, "Recently added"), pinned tiles (`HKCU\Software\Stained Glass\Start\Pinned`), type-to-search over apps and Control Panel settings, dark owner-drawn context menu (Pin/Unpin, Run as administrator = `runas`, Open file location, Uninstall), keyboard (arrows, Tab, Enter, Escape). Explorer's Start button and the Windows key toggle it (`SgStartPanel`, `WM_USER+10`); Wine's menu is the fallback. See "The Start menu" below. |
| `sg-mstsc` | **Remote Desktop Connection** -- the outbound half of RDP. A Windows dialog (and mstsc's command line: `.rdp` files, `/v:`, `/f`, `/w:`/`/h:`) that starts FreeRDP's native `sdl-freerdp3` through Wine's `\\?\unix\` path. `mstsc` resolves to it via App Paths (`defaults/60-sg-remote-desktop.reg`), and sg-start lists it. |
| `sg-control` | **Control Panel** -- see "The Control Panel" below. `control.exe` resolves to it via App Paths (`defaults/61-sg-control-panel.reg`); sg-start lists it. |
| `sg-settings` | **Settings** (Windows 10's, Win+I, `SystemSettings.exe`, every `ms-settings:` URI) -- the Control Panel's program in the Settings frame, sharing its pages' logic. System, Devices, Network & Internet, Personalization, Apps, Accounts, Time & Language, Ease of Access, Privacy, Update & Security. `defaults/65-sg-settings.reg`; sg-start lists it and its rail's Settings opens it. See "Settings" below. |
| `sg-terminal` | **Terminal** (Windows Terminal, `wt.exe`, Win+X > Terminal): tabs of shells on pseudo consoles -- PowerShell 7, Command Prompt, Git Bash and Windows PowerShell when present -- split into panes (Alt+Shift+Plus/Minus/D, Alt+arrows, Alt+Shift+arrows), with a VT screen of our own, 16/256/RGB colours, scrollback, search (Ctrl+Shift+F), mouse selection, copy/paste, zoom, full screen, and `wt`'s command line (`-p`, `-d`, `--title`, `new-tab`, `split-pane`, `move-focus`, `focus-tab`, `;`). `defaults/66-sg-terminal.reg`; sg-start lists it. See "Terminal" below. |
| `sg-clock` | **Alarms & Clock** (`ms-clock:`): alarms (repeat days, snooze, a notification with a chime of our own), world clock, timers, stopwatch with laps; keeps running for its alarms and timers when closed, and starts with the session while an alarm is on. `defaults/67-sg-clock.reg`; sg-start lists it. See "Alarms & Clock" below. |
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
| `sg-magnify` | **Magnifier** (`magnify.exe`, Win+Plus / Win+Minus / Win+Esc): Windows 10's toolbar (zoom out, the zoom level, zoom in, Views, Settings, Help) and its three views -- full screen (click-through, the point under the pointer shown under the pointer), lens and docked (an AppBar across the top) -- following the pointer, the keyboard focus or the text cursor; Ctrl+Alt+F/L/D, Ctrl+Alt+wheel, Ctrl+Alt+I inverts. Settings in `HKCU\Software\Microsoft\ScreenMagnifier`; Settings > Ease of Access > Magnifier. `defaults/82-sg-magnify.reg`; sg-start lists it. See "Magnifier" below. |
| `sg-osk` | **On-Screen Keyboard** (`osk.exe`, Win+Ctrl+O): Windows 10's dark keyboard -- the full layout, sticky Shift/Ctrl/Alt/Win, Caps Lock lit from the keyboard's state, Fn (F1-F12), the navigation keys, the numeric key pad, Mv Up/Mv Dn, Dock (an AppBar across the bottom), Fade, Options (click sound, hover to type). Real key presses (SendInput with scan codes); it never takes the focus. `defaults/83-sg-osk.reg`; sg-start lists it; Settings > Ease of Access > Keyboard turns it on. See "On-Screen Keyboard" below. |
| `sg-fontview` | **The font viewer and the Fonts folder** (`fontview.exe`, `control fonts`, `shell:fonts`, %WINDIR%\Fonts). A font file (.ttf .otf .ttc .fon) shows its own names read from the file, version, kind, the alphabet and a sample line at 12-72 pt, with Print, Install (for you) and Install for all users (elevated), a face picker for collections. The Fonts folder: a tile per family drawn in its font, search, details (styles, where installed, files), Preview, Delete, Install new font, dropped files. Per-user fonts where Windows 10 keeps them (and in `~/.local/share/fonts` for Linux programs), all users' through the elevated copy and sg-admind. `defaults/81-sg-fontview.reg`; wine-sg 0183 gives the launcher, the associations, per-user font loading and `shell:` URLs. See "Fonts (sg-fontview)" below. |
| `sg-wordpad` | **WordPad** (`wordpad.exe`, `write.exe`) -- Windows 10's WordPad in our own drawing: ribbon (File menu; Home: Clipboard, Font -- face and size boxes, grow/shrink, bold/italic/underline/strike, sub/superscript, highlight and text colour --, Paragraph -- indents, lists (bullets, numbers, letters, Roman), line spacing, alignment, Paragraph and Tabs dialogs --, Insert -- picture, date and time --, Editing -- Find, Replace, Select all; View: zoom, ruler, status bar, word wrap, units), a ruler with draggable indents and tab stops, a status-bar zoom slider, Page Setup, Print and Print preview (RichEdit's EM_FORMATRANGE, wine-sg 0184). Opens and saves RTF, .docx and .odt (our own readers and writers) and text. `defaults/68-sg-wordpad.reg`; wine-sg 0180 hands Wine's wordpad.exe/write.exe over; sg-start lists it. See "WordPad" below. |
| `sg-mmc` | **The administrative consoles**: `services.msc`, `eventvwr.msc` (and `eventvwr.exe`, as `sg-eventvwr64.exe`), `devmgmt.msc`, `diskmgmt.msc`, `compmgmt.msc` -- our own MMC-style host (console tree, result pane, Actions pane, toolbar, Action menu) and the snap-ins in it. `mmc.exe` resolves to it via App Paths (`defaults/79-sg-admin-tools.reg`); wine-sg 0142 gives the `.msc` files, `mmc.exe`/`eventvwr.exe` launchers, the Start menu's Administrative Tools and 0145 Win+X; the Control Panel has an Administrative Tools page. See "The administrative consoles" below. |
| `sg-pdf` | **PDF Viewer** -- `.pdf` opens out of the box (Windows opens PDFs in Edge, which we do not ship). One continuous scroll of every page, zoom (Ctrl+wheel, Ctrl+Plus/Minus, the zoom menu), fit width/fit page (Ctrl+\\), rotate (Ctrl+] / Ctrl+[), the page box (Ctrl+G), a sidebar of thumbnails or the document's bookmarks, find with every hit highlighted (Ctrl+F, F3), select text by dragging and Ctrl+C, links (inside the document and to the web), print (Ctrl+P, the Print verb), password-protected documents. Pages are rendered by Debian's poppler in sg-session's `sg-pdf`, through its bridge. `.pdf` via `defaults/80-sg-pdf.reg`; sg-start lists it; Settings > Default apps has a PDF viewer row. See "PDF Viewer" below. |
| `sg-browser` | **Get a web browser** -- we ship no browser (Edge is Microsoft's). Web links (`http`, `https`) and `.htm`/`.html` open it until the user has one: none installed -- it offers Firefox, Chrome and Brave, downloads the maker's installer as the winget community repository describes it, checks its SHA-256, installs it silently (or through winget when the user has it), makes it the default and opens the link; one installed -- the link opens there; several -- "How do you want to open this?". The user's choice (UserChoice, as Settings > Default apps writes it) is honoured on every link. Internet Explorer (Wine's, Gecko) is offered for simple pages. `defaults/81-sg-browser.reg`; sg-start lists it. See "Get a web browser" below. |
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

## Light and dark mode

Settings > Personalization > Colors keeps Windows' two modes
(`HKCU\...\Themes\Personalize`): `AppsUseLightTheme` for programs,
`SystemUsesLightTheme` for the shell, announced with `WM_SETTINGCHANGE
"ImmersiveColorSet"`. wine-sg (0160-0163) switches the visual style and the
system colours; our own drawing follows through `src/sg-mode.h`
(`sg_apps_dark()`, `sg_system_dark()`, `sg_mode_changed()`,
`sg_mode_title()` -- the title bar through DWMWA_USE_IMMERSIVE_DARK_MODE).

- **The pattern in the apps** (Calculator, Task Manager, Photos, Media Player,
  Alarms & Clock, Snipping Tool, Paint's chrome, Character Map, Compressed
  folders, the consoles): each palette macro is `(sgm_dark ? dark : light)`;
  `sgm_dark` is read before the main window is made, and `sgm_follow(hwnd)` at
  the top of the main window procedure re-reads it on ImmersiveColorSet, sets
  the title bar and repaints everything. Multi-file apps declare it in their
  header (`taskmgr.h`, `paint.h`, `mmc.h`) and define it in `main.c`.
- **Things that bite:** caches drawn in a colour (Photos' and Paint's glyph
  bitmaps) must be keyed by the mode; `WM_CTLCOLOR*` must not return
  `WHITE_BRUSH` (use `SetDCBrushColor` + `DC_BRUSH`); **list and tree views keep
  the colours they were created with** -- set them (`LVM_SETBKCOLOR`,
  `LVM_SETTEXTBKCOLOR`, `LVM_SETTEXTCOLOR`, `TVM_SET*`) at creation and on
  the change (zip, consoles).
- Settings/Control Panel (`g_pal`, `pal_apply`), Start and the network flyout
  (the Windows mode) have their own palettes. Terminal, WordPad, Magnifier,
  the On-Screen Keyboard, Sticky Notes' note colours and the voice-typing bar
  keep their own looks.
- **Gate: `test/appmode-check.sh`** (needs a wine-sg with 0162; `SG_WINE`):
  each app opened light, the mode switched as Settings does and back -- the
  mean brightness of its own drawing and of its title bar must go dark and
  come back, live. Screenshots `build/appmode-<app>-{light,dark,back}.png`.

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
- **Lock screen publishes the choice** (`sg-settingsctl lockscreen picture
  <unix path>` after a tile or Browse, `lockscreen signin yes|no` for the
  switch; the path through `wine_get_unix_file_name`): the lock screen is
  drawn by the machine account, which reads only what sg-settingsctl
  published and sg-lockd checked (sg-session CLAUDE.md, "The lock screen's
  picture and clock"). The registry value stays for the page itself.
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
- **Modes and the taskbar.** Settings and the Control Panel follow the app
  mode (AppsUseLightTheme): `control.h`'s `COL_*` are the `g_pal` palette,
  `pal_apply()` picks light or dark, remakes the brushes, rebuilds the page
  and sets the title bar (`sg_mode_title`) on `WM_SETTINGCHANGE
  "ImmersiveColorSet"`; wine-sg 0160-0163 turn the controls and the system
  colours dark. Personalization > Taskbar writes position (`Software\Stained
  Glass\Taskbar Position`, ABE_*), auto-hide, small buttons, combining
  (`TaskbarGlomLevel`), Task View, search (`Search\SearchboxTaskbarMode`) and
  alignment and announces every change with "TraySettings" (wine-sg 0164
  applies them); Start's page adds "Show most used apps"
  (`Start_TrackProgs`) and "Show suggestions occasionally"
  (`ContentDeliveryManager SubscribedContent-338388Enabled`). The gate's
  Taskbar check: the stored location/search/combining show, the Task View
  switch writes `ShowTaskViewButton`.
- **Not yet:** night light, resolution and
  Power & sleep need the Stained Glass compositor (sg-compositor 0.2.0+sg5
  has wlr-output-power-management for wlopm); multiple displays are not arranged; no Windows Hello,
  Family, Gaming, Phone or Search categories.

## Terminal (sg-terminal)

`src/terminal/`: `vt.c` is the screen -- a VT/xterm parser over a grid of
cells with a 9001-line scrollback ring (plain C, no Windows headers, so
`test/terminal-vt-test.c` runs it natively); `main.c` is the window: the tab
strip, the view, profiles, keys, the pseudo consoles and the dump. The icon is
drawn by `gen-icon.py` at build time.

- **Each tab is a pseudo console**: `CreatePseudoConsole` with two pipes,
  `CreateProcess` with `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE`, a reader thread
  posting the output to the window, a waiter posting the exit. Wine's
  conhost turns the console API into VT text; **three wine-sg patches make
  that work**: 0131 (a pseudo console's programs get its handles -- without
  it cmd and PowerShell write nowhere -- and `ResizePseudoConsole` resizes),
  0132 (conhost interprets the VT sequences programs write -- without it
  PowerShell 7's PSReadLine shows escape codes), 0133 (`wt.exe` in system32,
  Win+X > Terminal).
- **Panes**: a tab is a tree (`struct node`: a leaf is a pane, a split
  puts two subtrees side by side -- `SPLIT_V`, Windows Terminal's
  "vertical" -- or one above the other, with the first child's share in per
  mille). Each pane has its own window (`SgTerminalView`, the pane in
  `GWLP_USERDATA`), screen, pseudo console and program, so a pane's size
  change is its own `ResizePseudoConsole`. Alt+Shift+Plus splits to the
  right, Alt+Shift+Minus downwards (the default profile), Alt+Shift+D
  duplicates the focused pane (profile, command, folder) along its longer
  side; Alt+arrows move the focus to the nearest pane that way (a move with
  nowhere to go leaves the key to the shell, as Windows Terminal does);
  Alt+Shift+arrows move the divider of the nearest split that runs that way
  by 5%; Ctrl+Shift+W closes the pane and its sibling takes the space (the
  tab goes with its last pane); a graceful exit closes only its pane. The
  focused pane has an accent frame in the gap. `wt split-pane`/`sp` (`-H`
  below, `-V` right, `-s` share, `-D` duplicate, `-p`, `-d`, a command),
  `move-focus`/`mf` (left/right/up/down), `focus-tab`/`ft -t N`.
- **Search** (Ctrl+Shift+F, or the menu): a box at the pane's top right
  (`SgTerminalFind`: an edit, match case, up, down, close). As you type,
  every match in the screen and the whole scrollback is highlighted and
  the nearest one up from the bottom of what is shown becomes current and
  is scrolled to; Enter goes up (older), Shift+Enter down, both wrapping;
  Alt+C toggles match case; Escape closes it and gives the keys back to the
  shell. Matches are found on the cells (a wide character is one letter);
  a line is addressed absolutely (0 = the oldest scrollback line).
- **Keys go as VT**: arrows, Home/End, Insert/Delete, PgUp/PgDn and F1-F12
  with xterm's modifier parameters (conhost's input parser reads them),
  Backspace as DEL, Alt+key as ESC+key; the terminal's own shortcuts (see the
  header of `main.c`) never reach the shell -- their queued `WM_CHAR` is
  removed.
- **Profiles** are found on the machine -- PowerShell 7 (App Paths `pwsh.exe`,
  the PATH, `%ProgramFiles%\PowerShell\7`), Command Prompt (`%ComSpec%`),
  Git Bash (`%ProgramFiles%\Git\bin\bash.exe --login -i`), Windows
  PowerShell -- and merged with the user's own from settings.json (below).
  The default is PowerShell when present, until settings.json names another.
  A command line given without `-p` takes the found profile that runs the
  same program. A shell starts in its profile's startingDirectory, else
  `%USERPROFILE%`, with `WT_SESSION` set.
- **Look**: our own colour scheme ("Stained Glass Night"), the first of
  Cascadia Mono / Cascadia Code (Debian's fonts-cascadia-code, in the image)
  / Consolas / DejaVu Sans Mono, a bar cursor, bold drawn bright as Windows
  Terminal does. A graceful exit (0) closes the tab; any other shows the code
  and Enter restarts, Ctrl+D closes.
- **Things that bit.** A pseudo console's programs had no standard handles
  under Wine (see 0131). `wine reg add /d` and dash's echo eat `\b`, `\e`,
  `\7` in Windows paths: the gate prints with printf and copies PowerShell
  into `Program Files` (through a symbolic link .NET looks for its
  assemblies beside the link). `WINEDLLOVERRIDES=mscoree=` kills PowerShell 7
  -- only for creating the prefix. Wine has no `mode.com`: the gate's
  `test/sg-terminal-probe.c` reports the console's size and writes colours.
- **`SG_TERMINAL_DUMP=<file>`**: the window, the focused pane's grid size,
  the font, the profiles, each tab (screen centre, profile, alive, exit
  code, pane count, title), the + and menu buttons, the focused pane's
  origin, the active tab's panes (`pane N id= at= rect= size= profile=
  alive= focus=`) and every pane's non-empty rows (`prow ID Y: ...`), then
  the focused pane's id, cursor, scroll, rows and the colour of each cell
  (`fg N: ...`), the selection, and the search (`search open= case=
  matches= current= line= col= viewrow= needle=`, the box's and buttons'
  screen centres).
- **Gate: `test/terminal-check.sh`** (display :121; needs a wine-sg with
  0131-0133, `SG_WINE_DIR`): the screen's unit test (25 checks, native),
  `wt.exe` through App Paths opening Command Prompt, typed commands and their
  output, a program's own SGR 91 and SetConsoleTextAttribute colours, Ctrl+
  Shift+1 opening PowerShell 7 in a second tab (6*7 = 42, Write-Host in red,
  the tab named PowerShell), switching tabs by clicking, a mouse selection
  copied with Ctrl+Shift+C and Ctrl+V pasting (xclip), F11 resizing the
  pseudo console (the program sees the new size), `exit` closing its tab,
  `wt CMD ; nt -p ... --title ...`, and `wt.exe` typed in cmd (system32's
  launcher). Screenshots `build/terminal-*.png`. Mutants `-DSG_MUTANT_NOSGR`
  (no colours) and `-DSG_MUTANT_NORESIZE` (`SG_TERMINAL_EXE=`) turn it red,
  and stock wine-sg shows no prompt at all.
- **Gate: `test/terminal-panes-check.sh`** (display :141, `SG_TERMINAL_PANES_DPY`; `SG_WINE_DIR`):
  Alt+Shift+Plus gives a second Command Prompt of its own; typed text and
  its output only in the focused pane; each pane's program (the size
  probe) sees its own pane's size, the first one resized by the split;
  Alt+Left/Up/Right move the focus; Alt+Shift+Minus splits downwards;
  Alt+Shift+Right moves the divider and the program sees the new size;
  Ctrl+Shift+W and `exit` close one pane each, the rest filling the space;
  clicking focuses; Alt+Shift+D duplicates; search: two matches of a word
  scrolled 100+ lines into the scrollback, scrolled to, the current one's
  pixels in the highlight colour, Enter/Shift+Enter between them, match
  case (cmd's `AMD64`), Escape giving the keys back; `wt ... ; sp -V ; sp -H
  ; mf left` making three panes in one tab. 36 checks. Screenshots
  `build/terminal-panes-*.png`. Mutants `-DSG_MUTANT_PANEINPUT` (keys to the
  tab's first pane), `-DSG_MUTANT_NOSEARCH` (the screen only) and
  `-DSG_MUTANT_NORESIZE` turn it red.
- **settings.json** (`wtsettings.c` over `json.c`, a JSONC document model
  that keeps every key in order): `%LOCALAPPDATA%\Microsoft\Windows
  Terminal\settings.json`, Windows Terminal's unpackaged place
  (`SG_TERMINAL_SETTINGS` names another); with none, the packaged Windows
  Terminal's (`Packages\Microsoft.WindowsTerminal_8wekyb3d8bbwe\LocalState`)
  is taken over; with neither, one is made -- from the old
  `HKCU\Software\Stained Glass\Terminal` DefaultProfile/FontFace/FontSize
  when they exist (the registry is no longer written). Read: defaultProfile
  (GUID or name), profiles.defaults and profiles.list (or a plain list):
  guid, name, commandline, startingDirectory (`%VAR%`, `~`), icon (.ico, or
  an exe/dll with `,index`: drawn in the tab), colorScheme (a name or
  {dark,light}), font.face/font.size (or fontFace/fontSize), hidden; schemes
  (the 16 colours, background, foreground, cursorColor, selectionBackground;
  Stained Glass Night, our default, and Campbell are built in); actions and
  keybindings in both of Windows Terminal's forms (command+keys, or actions
  with ids and keybindings naming them), `unbound`/null taking a key away --
  newTab (profile, index), closePane, closeTab, next/prevTab, switchToTab,
  splitPane (split, profile, splitMode duplicate), duplicateTab, moveFocus,
  resizePane, find, copy, paste, openSettings, toggleFullscreen,
  adjustFontSize, resetFontSize, openNewTabDropdown, scrollUp/Down. The user's
  keys are looked up before the built-in ones.
- **The machine's profiles are dynamic profiles**: fixed GUIDs (Windows
  Terminal's for PowerShell, Command Prompt and Windows PowerShell, Git for
  Windows' for Git Bash), written into the list as `guid/hidden/name/source:
  "Stained Glass"` and run with the command line found (a `commandline` in
  the file wins). A dynamic profile of a generator we lack (Azure, WSL...)
  is left out. The file is rewritten only when something is added (those
  entries, a missing GUID) or Settings saves -- everything else in it stays,
  known or not, but comments are lost then. An invalid file is never
  overwritten: the built-in settings are used and Settings shows why.
- **Each pane has its profile's scheme and font** (a font cache per face and
  size; the zoom shifts all of them); the default profile's font sizes the
  window. `WT_PROFILE_ID` is the profile's GUID. Hidden profiles are not in
  the + menu nor Ctrl+Shift+N, but `wt -p NAME` opens them. The file is
  re-read within a second of a change (panes keep their profiles by GUID);
  Settings (Ctrl+,) writes defaultProfile and profiles.defaults.font and has
  "Open JSON file".
- **Dividers are dragged with the mouse** (the gap between panes belongs to
  the main window: `divider_at`, a size cursor, capture); both panes'
  pseudo consoles are resized as the divider moves.
- **Gate: `test/terminal-settings-check.sh`** (display :143,
  `SG_TERMINAL_SETTINGS_DPY`; `SG_WINE_DIR`): the registry migration; a JSONC
  file with a user profile (commandline, startingDirectory, scheme -- the
  pane's pixel --, font), a hidden one, a foreign dynamic one, unknown keys
  and a GUID-less profile (GUID written back, unknown keys kept); the menu;
  ctrl+alt+g as newTab of a profile, ctrl+shift+t unbound; Settings' font
  size saved into the file with the rest kept, the defaults' pane taking it
  and the profile with its own size keeping its own; an edit of the file
  picked up; `wt -p` of the hidden profile; a vertical and a horizontal
  X
  Mutants `-DSG_MUTANT_NOUNKNOWN` (drops keys on save),
  `-DSG_MUTANT_NODRAG` and `-DSG_MUTANT_NOUSERPROFILE` turn it red.
- **Not yet:** per-profile settings beyond those above (padding, cursor
  shape, opacity, backgroundImage...), `profiles.defaults` colorScheme only
  as a default name, PNG icons, a Settings UI for profiles (edit the JSON),
  comments kept on rewrite, custom title bar tabs (the tabs are under a
  normal caption), bracketed paste, mouse reporting to programs, Unicode
  combining marks.

## Alarms & Clock (sg-clock)

`src/clock/main.c`, one owner-drawn window (every clickable thing is a
rectangle in one table, as in Calculator), four pages under a pivot: Alarm,
World Clock, Timer, Stopwatch. Dialogs (New alarm, New timer, Add a new
location) are plain controls. The icon is drawn by `gen-icon.py`.

- **Notifications** are our own toast windows (topmost, tool windows that
  never take the focus, owned by the main window so no taskbar button),
  stacked above `Shell_TrayWnd`: Alarm (Snooze, Dismiss) and Timer (Dismiss),
  with a two-note chime synthesized into a WAV in memory (`PlaySound`
  `SND_MEMORY | SND_LOOP`) until dismissed. An "only once" alarm is turned off
  when dismissed, not when it rings, so a snoozed one rings again.
- **It keeps going when closed**: with an alarm on or a timer running,
  closing hides the window; with an alarm on, `HKCU\...\Run\Stained Glass
  Clock` starts it with the session (`/background`). One instance (mutex
  `Local\StainedGlassAlarmsAndClock`); a second start hands over its page.
- **World clock**: time zones from `EnumDynamicTimeZoneInformation` and their
  `Display` names; the city is the one searched for ("tokyo" -> Tokyo).
  **Wine's `SystemTimeToTzSpecificLocalTimeEx` is a stub** (it raised
  EXCEPTION_WINE_STUB): the time is `GetTimeZoneInformationForYear` +
  `SystemTimeToTzSpecificLocalTime`.
- **A dialog's owner is enabled before the dialog goes**: enabled in
  `WM_DESTROY`, Windows (and Wine) had already activated another program's
  window (Notepad came to the front and ate the next click).
- Kept in `HKCU\Software\Stained Glass\Clock` (`Alarms`, `Cities`,
  `Timers` as REG_MULTI_SZ, `Page`).
- **`SG_CLOCK_DUMP=<file>`**: the page, every button's screen centre, alarms,
  cities with their time and offset, timers, the stopwatch, notifications
  and their buttons, an open dialog's controls.
- **Gate: `test/clock-check.sh`** (display :122): `ms-clock:stopwatch`; the
  stopwatch runs, laps, pauses (stays), resets; a 3-second timer made in the
  dialog fires its notification, Dismiss; World Clock finds Tokyo at the
  system's Tokyo time; an alarm for the next minute goes off, Snooze snoozes
  it; the Run entry; closing hides and keeps it running; everything kept
  after a restart. Screenshots `build/clock-*.png`. Mutants
  `-DSG_MUTANT_NOTIMERFIRE` and `-DSG_MUTANT_STOPWATCH` (`SG_CLOCK_EXE=`)
  turn it red.
- **Not yet:** Focus sessions, alarm sounds to choose, the lock screen's
  alarm, waking the PC from sleep.

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
- **Sleep and Hibernate** are in the power menu only when logind says this
  session may (`sg-settingsctl sleep-caps`: `CanSuspend`/`CanHibernate`
  "yes"; polkit's "challenge" is no -- no agent runs to ask, and a remote
  or inactive session is refused), asked at start-up and again, on a
  thread, after every power menu. Choosing one runs `sg-settingsctl sleep
  suspend|hibernate`, which locks the session first (Windows asks for the
  password on waking), then `systemctl suspend|hibernate`. `SG_SETTINGSCTL`
  names a stand-in; the gate's answers yes, then no. NoClose takes them
  away with Restart and Shut down. Mutants: Sleep always shown, Sleep
  asking nothing -- both fail `start-check.sh`.
- **Settings > Personalization > Start and the modes** (read at every
  opening): Start is dark or light by the Windows mode
  (`SystemUsesLightTheme`, `src/sg-mode.h`; tiles keep white on the
  accent); "Most used" -- the five apps this user starts most from Start,
  counted in `Start\Usage` while `Start_TrackProgs` is on (read in one pass
  per opening); "Suggested" -- one bundled app never started, another each
  day, nothing from outside the machine; "Show app list" off leaves rail
  and tiles, the list appearing for a search or the menu button ("All
  apps"); "more tiles" is four columns; "full screen" all of the screen but
  the taskbar. **Placement follows the taskbar's edge** from
  `ABM_GETTASKBARPOS` (asked at start-up and after "TraySettings" or a
  display change -- a round trip at each opening was measurable); a 1 px
  answer (an older shell) means the bottom. `WM_USER+10` with wparam 1 (the
  taskbar's search) opens without toggling.
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
  menu and the registry, the rail, the power menu and `NoClose`, the Windows
  mode (dark, then light), Most used, Suggested and turning both off, app
  list off (388 px, and the list for a search), more tiles (812 px), full
  screen, the search entry, click-away; with `SG_TASKBAR_POSITIONS=1` (a
  wine-sg with 0164) Start below a top taskbar and beside a right one.
  Opening speed under 200 ms fails under a heavily loaded machine (load
  50+: 250-500 ms for the old and new build alike, all in the list scan).
  Screenshots: `build/start-{open,light,search,context,power,rail,nolist,fullscreen}.png`. xdotool's
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
- **What is being said shows in the bar** (the engine's `PARTIAL`): grey
  italics where "Listening..." was, the bar widened to 600 px and still
  centred, a long one losing its start ("...the end"). It is never typed;
  the utterance's final `TEXT` clears it and is typed once. Any state but
  listening clears it too.
- **Spoken commands** come as `CMD delete` (as many Backspaces as the last
  text had characters -- a line break was one Enter -- only if the window
  it went to is still in front, and only within this bar's session) and
  `CMD undo` (Ctrl+Z to the program in front). "Stop listening" is the
  engine's own.
- **Language** (Control Panel: English, Deutsch, Francais, Espanol, or
  detect automatically) goes with each start request (`"language"`), with
  `"partials": true`; the engine applies that language's spoken punctuation,
  fillers and commands.
- **Settings** are `HKCU\Software\Stained Glass\Speech`, read at every
  start, so Control Panel changes apply at once.
- **Control Panel > Speech Recognition** (`src/control/speech.c`; `control
  /name Microsoft.SpeechRecognition`, the bar's gear): on/off -- turning it on
  runs `sg-dictate --download` (sg-speechd downloads the model as root) and
  shows progress from its status file; the microphone (from `sg-dictate
  --mics --out`), "Test microphone" (`sg-dictate --meter`, a level in a temp
  file); hold-to-talk and its key; continuous dictation, automatic and spoken
  punctuation, filler words, numbers, typing or pasting, language; privacy
  and the model's CC BY 4.0 attribution. The model counts as installed when
  the sg-speech-model-parakeet package's copy
  (`/usr/share/stained-glass-speech/...`, `SG_SPEECH_PACKAGED_DIR`) or
  sg-speechd's download has its `.verified` stamp. Wine gives a Windows program no
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
  **Partials and commands**: the stand-in engine "speaks" three partials
  2.5 s apart: the bar's dump (`SG_DICTATE_DUMP`, a Windows path; state,
  rectangle, partial text, last length) shows each while Notepad stays
  unchanged, the bar is 600 px and centred, its pixels have grey text; the
  request carries `"language": "de-DE"` and `"partials": true`; the final
  is typed once and the partial cleared; then `CMD delete` takes exactly its
  30 characters back. With the real engine: a long sentence's partial
  results appear before anything is typed (the WAV starts with 25 s of
  silence -- under load 57 the model took 17 s to load and a sentence spoken
  meanwhile finished before any partial could be shown), then the sentence
  is typed; and German speech with Language = de-DE types "Komma"/"Punkt"
  as marks. `SG_DICTATE_DPY` picks the display, `SG_DICTATE_EXE` a mutant:
  `-DSG_MUTANT_PARTIAL_TYPED` (partials typed) fails 5 checks; the engine
  with its German table removed (`SG_DICTATE_SCRIPT`) fails the German one.

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
  includes the taskbar. With wine-sg 0164 it answers the real edge, and the
  flyout opens at the tray's corner of a top, left or right taskbar. The
  flyout and the tray icon's glyph follow the Windows mode (light flyout,
  black glyph on a light taskbar). It is owned by the (never shown) tray window, so it
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
  not written (ZIP64 is read only). A multi-selection "Compress to ZIP file"
  gets one zip per item (a static verb) -- Send to makes one zip of them all.
- **Send to > Compressed (zipped) Folder**: sg-session plants an empty
  `Compressed (zipped) Folder.ZFSendToTarget` in each profile's SendTo, as
  Windows' Default profile has, and wine-sg's shell32 (0156) runs that type's
  `shell\sendto\command` with every file sent (`%*`); `75-sg-zip.reg`
  registers it as `/create %*`. Gate: wine-sg's `test/explorer2-gate.sh`
  (`SGZIP=` this build's sg-zip64.exe).
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
  **The ProgID gives itself no name** (empty default, no FriendlyTypeName):
  one ProgID serves every image type, so a name would make every picture's
  Type read "Image"; without one File Explorer says "PNG File", "JPG File",
  as Windows 10 does (wine-sg 0152). The same holds for sg-media's ProgIDs.
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

## Magnifier (sg-magnify)

`src/magnify/main.c`. The toolbar is the program's window (class
`SgMagnifier`, what explorer's keys look for: wine-sg 0181 puts
`magnify.exe` in system32, 0182's explorer posts `WM_COMMAND` 0x101/0x102
for Win+Plus/Win+Minus and `WM_CLOSE` for Win+Esc, and runs `magnify.exe`
when none is open); the view is a second, topmost `WS_EX_NOACTIVATE` window.
One instance (mutex `Local\StainedGlassMagnifier`; a second start hands its
command line over by `WM_COPYDATA`: `/fullscreen`, `/lens`, `/docked`,
`/zoomin`, `/zoomout`, `/zoom:N`, `/close`, `/reload`).

- **Where the picture comes from.** The docked view never covers what it
  shows, so it copies the screen DC. The lens and full-screen views do cover
  it: a screen copy would copy the view itself and feed back (each frame the
  last one magnified again -- the `-DSG_MUTANT_FEEDBACK` build shows it).
  They compose the picture instead: the desktop window, then every other
  process's visible top-level window bottom to top, each from its own
  surface with `PrintWindow(PW_RENDERFULLCONTENT)` -- wine-sg 0076 makes that
  work across processes -- with layered windows' alpha and colour key; our
  own windows are never in it. A hung window keeps its last picture
  (`IsHungAppWindow`). **Wine's `PaintDesktop` into a memory DC draws
  nothing**: the desktop comes from `PrintWindow(GetDesktopWindow())`. No
  compositor help is needed; the same code runs under Xwayland in a session.
- **Full screen is click-through** (`WS_EX_LAYERED | WS_EX_TRANSPARENT`) and
  shows the screen from `p * (1 - 1/zoom)`, so the point under the pointer is
  drawn under the pointer and a click lands on what is seen. The pointer is
  drawn magnified (`GetCursorInfo`). At 100% the view hides.
- **Docked is an AppBar** at the top (a quarter of the screen): wine-sg 0182
  makes explorer take an AppBar's space off the work area, and gives it back
  when it goes.
- **Follows** the pointer when it moves, else the text cursor
  (`GetGUIThreadInfo`'s caret) or a newly focused control. Ctrl+Alt+wheel is
  a `WH_MOUSE_LL` hook (the wheel is swallowed only with Ctrl+Alt held).
- Settings are Windows' `HKCU\Software\Microsoft\ScreenMagnifier`
  (`Magnification`, `ZoomIncrement`, `MagnificationMode` 1 full / 2 docked /
  3 lens, `Invert`, `FollowMouse`/`FollowFocus`/`FollowCaret`, `LensWidth`,
  `LensHeight`, `DockedHeight`). **Settings > Ease of Access > Magnifier**
  (`ms-settings:easeofaccess-magnifier`, `set_misc.c`): on/off, zoom level
  and increment, start after sign-in (HKCU Run), invert, view, follow;
  a running Magnifier is told `/reload`.
- **`SG_MAGNIFY_DUMP=<file>`**: mode, zoom, view and source rectangles, the
  pointer, the view's styles, the toolbar's buttons' screen centres, frames;
  `CLOSED` when it exits.
- **Gate: `test/magnify-check.sh`** (display :171; `SG_WINE_DIR` may be a
  build tree -- the Windows-key and work-area checks need wine-sg
  0181/0182, and are skipped with a note on a Wine without `magnify.exe`):
  a window of 8-pixel colour blocks (`test/sg-a11y-probe.c pattern`) and a
  reference screenshot; Win+Plus starts full screen at 200%; every sampled
  view pixel must equal the reference at SOURCE + offset/zoom (the toolbar
  and pointer left out): full screen at 200% and 300%, the source origin
  p(1-1/zoom), a click passing through; Win+Plus/Win+Minus, Ctrl+Alt+wheel,
  the toolbar's +; lens twice a second apart (no feedback); docked and the
  work area; Ctrl+Alt+I (inverted pixels); the setting kept; Win+Esc closes
  and the work area comes back. 21 checks. Mutants `-DSG_MUTANT_SCALE` and
  `-DSG_MUTANT_FEEDBACK` (`SG_MAGNIFY_EXE=`) fail 6 and 4+ of them.
  Screenshots `build/magnify-*.png`.

## On-Screen Keyboard (sg-osk)

`src/osk/main.c`, window class `OSKMainClass` (what programs and explorer's
Win+Ctrl+O look for). One instance (`Local\StainedGlassOnScreenKeyboard`).

- **It never takes the focus**: `WS_EX_NOACTIVATE | WS_EX_TOPMOST`,
  `MA_NOACTIVATE`, and **its own title bar** -- moved and sized with the mouse
  captured and `SWP_NOACTIVATE`, because Wine's move loop (an `HTCAPTION`
  hit) makes the window the foreground window first. Minimize
  (`SW_SHOWMINNOACTIVE`) and close are ours too.
- **Keys are real key presses**: `SendInput` with the virtual key and its scan
  code (`MapVirtualKey`), extended keys flagged; Shift/Ctrl/Alt/Win latch
  (accent colour) and are held round the next key, then let go; Caps Lock is
  a real press and its light is `GetKeyState(VK_CAPITAL)` (a physical Caps
  Lock shows too). Labels follow Shift, Caps and Fn.
- Layout in key units (the main block 15 wide) scaled to the window: Nav's
  keys, Options/Help, the right column (Nav, Mv Up, Mv Dn, Dock, Fade), the
  numeric key pad (Options). Dock is an AppBar across the bottom (the work
  area shrinks, wine-sg 0182); Fade makes it 43% opaque while the pointer is
  elsewhere; hover typing (Options) presses the key the pointer rests on.
  Glyph keys (Backspace, Windows, arrows) are drawn, not text: the UI font
  has no U+232B/U+229E.
- Settings: Windows' `HKCU\Software\Microsoft\Osk` (`WindowLeft/Top/Width/
  Height`, `ShowNavigationKeys`, `ShowNumPad`, `ClickSound`, `Mode`,
  `HoverPeriod`, `Dock`, `Fade`). Settings > Ease of Access > Keyboard has
  "Use the On-Screen Keyboard".
- **`SG_OSK_DUMP=<file>`**: window, styles, latched modifiers, Caps, Fn,
  Nav, key pad, dock, fade/alpha, and every key's screen centre, lit state
  and label, rewritten on every change (and on `WM_MOVE`); `CLOSED` at exit.
- **Gate: `test/osk-check.sh`** (display :172; `SG_WINE_DIR` may be a build
  tree; Win+Ctrl+O and the work area need wine-sg 0181/0182): Notepad in
  front, Win+Ctrl+O opens it (topmost, NOACTIVATE); clicking Shift h e l l o
  , space Shift w o r l d Shift 1 types "Hello, World!" into Notepad
  (`WM_GETTEXT`), Shift lets go, labels follow Shift; Caps Lock lit and
  capitals; Fn's F1..F12; Ctrl+A then Backspace; dragging the title bar, Mv
  Up, Dock (the work area) and undock; Notepad keeping the focus at every
  step; Win+Ctrl+O closes it; its place kept. 28 checks. Mutants
  `-DSG_MUTANT_ACTIVATE` (no NOACTIVATE: Notepad loses the focus, nothing is
  typed) and `-DSG_MUTANT_STICKY` (Shift never lets go: "HELLO< world!")
  (`SG_OSK_EXE=`) turn it red. Screenshots `build/osk-*.png`.

## Fonts (sg-fontview)

`src/fontview/`: `fontinfo.c` reads font files (plain C, no Windows headers,
so `test/fontinfo-dump.c` runs it natively): the sfnt table directory, the
`name` table (Windows English records first, then any Windows, Unicode, Mac
Roman), `OS/2` weight/italic (`head` macStyle without one), which outlines
(glyf/CFF), OpenType Layout (GSUB/GPOS), DSIG; each face of a `ttcf`
collection; a `.fon`'s NE `RT_FONT` resources and FNT headers. **Every offset
is checked against the buffer** -- font files come from downloads and mail.
`fontlib.c` installs, removes and lists; `main.c` is the viewer and the
command line; `folder.c` the Fonts folder. Icons are drawn by `gen-icon.py`.

- **The viewer loads the file privately** (`AddFontResourceEx(FR_PRIVATE)`)
  and names it from the file itself, not from GDI; the samples are drawn with
  the name table's family, OS/2 weight and italic. Windows' title ("Name
  (TrueType)"), a face picker instead of Windows' Previous/Next for a `.ttc`.
- **Where fonts go.** For you: `%LOCALAPPDATA%\Microsoft\Windows\Fonts\<file>`
  and `HKCU\Software\Microsoft\Windows NT\CurrentVersion\Fonts` "Full name
  (TrueType)" = the full path (Windows 10 1809; **Wine read only HKLM's key**
  -- wine-sg 0183 makes win32u load HKCU's), plus a copy in
  `~/.local/share/fonts/stained-glass` (or `$XDG_DATA_HOME/fonts/...`; the
  Unix home is Wine's `WINEHOMEDIR`) and `fc-cache` for Linux programs. For
  all users (an elevated token -- SYSTEM, ADR 0012 -- or `runas` of
  ourselves, whose exit code is the answer): `%WINDIR%\Fonts\<file>`, the
  HKLM value = the file name, and sg-admind's `font-install FILE` copies it
  from the machine prefix's `windows/Fonts` (SYSTEM's own regular file, a
  font's magic, 64 MB at most, never through a link) to
  `/usr/local/share/fonts/stained-glass` (`font-remove` takes it away). Then
  `AddFontResource` and `WM_FONTCHANGE`, as Windows' installer does; a
  collection's value is "A & B (TrueType)".
- **Delete** removes a person's own family (value, both copies,
  `RemoveFontResource` so this session's font cache forgets it); a family
  installed for all users needs the elevated copy; Linux's and Wine's own
  fonts are "a system font and can't be deleted". Wine keeps each process's
  font list, so the Fonts folder hides what it deleted itself, and ignores
  its own `WM_FONTCHANGE` while busy -- **the broadcast reaches our own window
  in the middle of the delete**, and reloading there freed the family being
  deleted (a crash the gate caught).
- **Command line**: `FILE`, `/p FILE`, `/install [/allusers] [/quiet] FILE`,
  `/uninstall [/allusers] [/quiet] FILE|NAME` (exit code 0 or the Windows
  error; `/quiet` never elevates, so a standard user gets 5), `/family NAME`,
  `/folder`, `--families OUT` (every family GDI enumerates, for gates).
  `SG_FONTVIEW_DUMP=<file>`: the viewer's names, type, `gdi_face` (what GDI
  selected), install state, buttons' and sample lines' screen positions; the
  folder's counts, search, tiles with centres, selection and its files; the
  result of a headless install. `SG_FONTVIEW_YES=1` answers its questions.
- The Control Panel: "Fonts" in All Items and Appearance and
  Personalization, `control fonts` / `Microsoft.Fonts` (`open_fonts_folder`:
  our sibling `sg-fontview64.exe /folder`, else `fontview.exe`).
- **Gate: `test/fontview-check.sh`** (display :181, a SHARED prefix: this
  user is SYSTEM, `sgconf` a standard user; fontTools makes the fonts from
  DejaVu at run time): the reader against fontTools on a TrueType font, a
  two-face collection, DejaVu Sans Bold, an OpenType/CFF font, and a `.fon`
  (Wine's `coure.fon`), and non-fonts refused; as the standard user the
  viewer (through the .ttf association) shows the file's names, GDI draws in
  the private font, the 72 pt line is several times the 12 pt one's height;
  Install (a click): the HKCU value, the file, `fc-list` with the user's
  gate HOME, a new process's families; `control fonts` lists it, search
  narrows to it, its tile selects it ("For you", its file), Delete removes
  value, files and every trace; Install for all users refused for the
  standard user (5, nothing written); as SYSTEM for all users: %WINDIR%\Fonts,
  HKLM = the file name, sg-admind (test mode, its own spool) makes the Linux
  copy, the standard user sees it, the folder says "For all users";
  uninstalled again. 27 checks. Mutants `-DSG_MUTANT_NOREG` (no registry
  value), `-DSG_MUTANT_NAMEID` (family and full name swapped;
  `SG_FONTINFO_CFLAGS` for the native reader) and `-DSG_MUTANT_ANYONE`
  (anyone may install for all users) turn it red. Screenshots
  `build/fontview-*.png`.
- **In the gate's shared prefix a standard user can write `%WINDIR%\Fonts`**
  (the harness makes the whole prefix group-writable); the program's own
  check and the HKLM key's DACL are what refuse. The image's prefix
  permissions are sg-session's.
- **Not yet:** Font settings (hide fonts by language, "Show/Hide"), the
  font's designer/licence page, variable-font axes, a Details view of the
  folder, drag out of the folder, printing tested only as far as the dialog
  (no printer).

## WordPad (sg-wordpad)

`src/wordpad/`: `main.c` (window, commands, files, the dump), `ribbon.c`
(the ribbon and the status bar's zoom, one item list for layout, drawing,
clicks and the dump, as Paint's; the font name and size are real combo
boxes placed by it), `ruler.c`, `glyphs.c` (our own pictures, drawn at 4x and
box-filtered), `docmodel.c` (a document as paragraphs of runs; RTF to and
from it), `ooxml.c` (.docx), `odf.c` (.odt), `xml.c` (a small XML reader),
`picture.c` (WIC), `print.c` (printing, preview, Page Setup), `dialogs.c`
(Date and Time, Paragraph, Tabs, Find/Replace). The icon is drawn by
`gen-icon.py`. The text is a RichEdit 4.1 control (msftedit).

- **Files.** RTF is RichEdit's own reader and writer. **.docx and .odt are
  our own readers and writers** (ECMA-376, OASIS ODF), over `src/zip/zipcore.c`
  (its `zw_add_mem` is new -- ODF's `mimetype` must be first and stored):
  reading makes the model and hands RichEdit RTF; saving streams RichEdit's
  RTF out and parses it into the model. Paragraphs (alignment, indents,
  spacing, line height, tab stops, bullets and numbering), runs (bold,
  italic, underline, strike, super/subscript, size, font, colour,
  highlight), styles followed through basedOn / parent-style-name,
  pictures (PNG parts; RichEdit gets them as `\dibitmap`), tables
  flattened. Text: UTF-16 by BOM, UTF-8 when valid, else ANSI; saves
  "Text Document" as UTF-8 and "Unicode Text Document" as UTF-16 after
  Windows' Text-Only warning. A typed extension decides the format.
- **Windows' interface kept**: class `WordPadClass`, title "<name> -
  WordPad", Calibri 11 / 1.15 lines / 10 pt after for a new document (the
  first installed of Calibri, Carlito, Segoe UI, Inter...; text files in the
  first monospace), `/p` and `/pt`, unquoted paths with spaces,
  `Applets\Wordpad\Options` in HKCU, Ctrl+B/I/U/E/L/R/J, Ctrl+Shift+>/<,
  Ctrl+=, Ctrl+Shift+L, Ctrl+1/2/5, F12, Ctrl+wheel zoom.
- **Printing is RichEdit's EM_FORMATRANGE**, which Wine lacked: **wine-sg
  0184** implements it (and fixes `\dibitmap` colours, PNG/JPEG pictures,
  bitmaps dropped on save, `\li`/`\fi` swapped by the writer, and zoomed
  letters overlapping). `SG_WORDPAD_PRINT_EMF=<dir>` prints each page as
  `page<N>.emf` (and `pages.txt`) instead, for the gate.
- **wordpad.exe / write.exe**: App Paths (`defaults/68-sg-wordpad.reg`,
  which also takes .docx/.odt and points rtffile at us); **wine-sg 0180**
  makes Wine's wordpad.exe (which write.exe and the rtffile/wrifile
  associations start) hand off to it. sg-start lists it.
- **`SG_WORDPAD_DUMP=<file>`**: title, path, format, dirty, zoom, ruler,
  wrap, units, selection, length, the selection's character and paragraph
  format, `advance10` (EM_POSFROMCHAR over ten characters), the edit
  control's and text's screen place, every ribbon item's screen rectangle
  and state, the ruler's markers, the status bar's zoom buttons, the print
  preview's page and buttons, the last message box.
- **Gate: `test/wordpad-check.sh`** (display :197, `SG_WINE_DIR` an install
  or a build tree): 58 checks -- `wordpad.exe` via App Paths, write.exe and
  Wine's wordpad.exe handing off (0180); Bold, Center, Start a list, the
  size box, Date and time, a ruler drag, all in the saved RTF; Find; zoom
  (View, status bar) with the text's advance scaling (0184); Save/Don't
  Save; a .docx made by `test/wordpad-mkdocs.py` shown with its colours and
  picture (pixels) and saved back as .docx (document.xml and the PNG part
  checked by Python); the .odt likewise; a picture through RTF (0184); UTF-16
  and UTF-8 text; `/p` of 120 lines on 3 pages inside the margins (EMF played
  by `test/sg-wordpad-probe.c`); Print preview. Screenshots
  `build/wordpad-*.png`. Mutants `-DSG_MUTANT_NOBOLD` and
  `-DSG_MUTANT_DOCXRPR` (`SG_WORDPAD_EXE=`) turn it red, and so does a
  wine-sg without 0180/0184.
- **Not yet:** tables (read flattened, never written), Paint drawings and
  OLE objects, "Send in email", headers/footers and page numbers in print,
  .doc (Word 97) files, spelling.

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
(both directions), Security (read-only: the service's own DACL from
`QueryServiceObjectSecurity` -- who may do what, and the SDDL an
administrator changes with `sc sdset`, wine-sg 0185). **The console decides nothing**: each step opens the
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
  exactly what `sg-sysinfo units` does, with no verbs; Properties >
  Security shows the DACL (a wine-sg without 0185 shows an empty one and
  fails). 19 checks. Mutants (`-DSG_MUTANT_NOSTART`, `-DSG_MUTANT_STATUS`,
  via `SG_MMC_EXE`) turn it red. Screenshots `build/services-*.png`.

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
warning), its category opens by itself and a banner says so. **Disable
device / Enable device** (a PCI or USB device: Actions pane, Action and
context menus; Windows' "Disabling this device will cause it to stop
functioning" question) are sg-sysinfod's `device-disable`/`device-enable`
(administrators only; the controller of the system disk is refused); a
disabled device gets the down-arrow picture (`IC_DISABLED`), its category
opens, and General says "This device is disabled. (Code 22)". Otherwise
Linux binds drivers, sg-drivers installs third-party ones.

- **Gate: `test/devmgmt-check.sh`**: on this machine through the real
  sg-sysinfo, every display and network adapter sg-sysinfo reports is under
  Display adapters / Network adapters with its driver, SetupAPI devices are
  there, and the display adapter's Properties show its kernel module; then a
  made-up machine (the gate writes a sysfs for `SG_SYSFS`, a `pci.ids`, and
  stand-ins for `SG_DRIVERS`/`SG_DPKG_QUERY`): a VM's bochs display and
  virtio network adapter with their drivers, an NVIDIA card with no driver
  (marked, visible, the banner, Properties "Code 28" and `nvidia-driver`),
  and Devices by connection; then Disable/Enable through a real sg-sysinfod
  on the gate's socket over the made-up sysfs: a standard user refused (the
  device untouched), an administrator disabling the virtio adapter
  (`driver_override` = sg-disabled, unbound), Code 22 in Properties, Enable
  device clearing it. 21 checks. Mutants `-DSG_MUTANT_NOLINUX` (only
  SetupAPI), `-DSG_MUTANT_NOWARN` (no-driver ignored) and
  `-DSG_MUTANT_NODISABLED` (Code 22 ignored: 3 fail) turn it red.
  Screenshots `build/devmgmt-*.png`.

### Disk Management (diskmgmt.msc)

`src/mmc/disks.c`: Windows' layout -- the volume list above (Volume, Layout,
Type, File System, Status, Capacity, Free Space, % Free) and the graphical
view below (our own drawing: per disk a label box -- Disk N, Basic or
Removable, size, Online; CD-ROM N, DVD, No Media -- and a bar of its
partitions and unallocated space in disk order, each at least 70 px and the
rest shared by size; a coloured header, blue for system volumes, black for
unallocated; the selection hatched). Clicking either selects both. Names are
Windows': "LABEL (X:)", "Local Disk (C:)", "(Disk 0 partition 1)"; statuses
"Healthy (EFI System Partition)", "Healthy (System, Page File, Primary
Partition)", "Healthy, not mounted (...)". From `sg-sysinfo disks` (lsblk,
and the prefix's drive letters -- C: is the volume holding `drive_c`).

- **Changes are sg-sysinfod's**: Change Drive Letter and Paths (assign D:-Y:,
  or remove), Mount/Unmount, Format (label, NTFS/exFAT/FAT32/ext4, after
  Windows' "Formatting this volume will erase all data on it" warning) are
  requests `letter`/`mount`/`unmount`/`format`; the service allows only an
  administrator and refuses the system disk, a mounted or lettered volume
  for format, C:/Z:, ... The console greys what cannot apply (Format on a
  system or mounted volume) and shows the service's refusal in Windows' words
  ("You need to be an administrator to change disks and volumes").
- **New Simple Volume** (on unallocated space: size, file system, label,
  drive letter -- then mounted and lettered), **Delete Volume** (after
  Windows' warning), **Extend Volume** (into the unallocated space right
  after it) and **Shrink Volume** (the amount; sg-sysinfod's `resize-info`
  gives the limits) are sg-sysinfod's `create`/`delete`/`resize`; offered
  only off the system disk, on unmounted, unlettered volumes; resizing only
  NTFS and ext4. Unallocated space has no list row, so the graph reports it
  through `frame_custom_selection` (the Actions pane then names it
  "Unallocated"). **SMART** (`sg-sysinfo smart`): a failing disk's box says
  "Online (Errors)" in red with a banner; a disk's box double-clicked (or its
  Properties) shows health, temperature, hours and sector counts.
- **Gate: `test/diskmgmt-check.sh`**: this machine's disks against `lsblk`
  (every disk with its exact size, every partition a volume, C: the volume
  holding drive_c); a made-up machine (a stand-in `SG_LSBLK`): the data
  disk's size, 8 GiB unallocated between and after its volumes in disk order,
  drawn to scale, Format offered only for the unmounted non-system volume;
  then changes through a real sg-sysinfod the gate serves itself
  (`systemd-socket-activate -a ... --serve`; **it passes on only the `-E`
  variables**), first as a standard user (`SG_WINE_GROUP` is the user's
  group, `SG_ADMIN_GROUP` not): Change Drive Letter refused with the
  administrator message and no link made; then as an administrator: PHOTOS
  gets D: (a `dosdevices/d:` link to its mount point, shown "PHOTOS (D:)"),
  and Format as exFAT with a label runs the stand-in `mkfs.exfat -L gatevol
  /dev/sdb1` after the warning; SMART through a stand-in smartctl (the
  failing disk, the banner, its Properties); then **for real on a loop
  device only** (passwordless sudo; sg-sysinfod as root on the gate's socket
  with `SG_SYSINFO_DISKS` naming only it): two New Simple Volumes (ext4,
  one with a letter under the gate's media directory), Shrink by 50 MB,
  Extend by 30 MB (e2fsck clean), Delete. 34 checks. Mutants
  `-DSG_MUTANT_SCALE` (not to scale), `-DSG_MUTANT_FSNAME` (always NTFS) and
  `-DSG_MUTANT_NONEW` (no New Simple Volume: 11 fail) turn it red.
  Screenshots `build/diskmgmt-*.png`.

### Computer Management (compmgmt.msc), Local Users and Groups, Shared Folders

`main.c`'s `build_console()` puts the consoles under Windows' tree: Computer
Management (Local) > System Tools (Event Viewer, Shared Folders, Local Users
and Groups, Device Manager), Storage (Disk Management), Services and
Applications (Services, Stained Glass System Services). A folder node's
result pane lists its children in the console's order (not sorted). The
same program answers `lusrmgr` and `fsmgmt` as console names (wine-sg 0186
writes `lusrmgr.msc` and `fsmgmt.msc`).

- `src/mmc/users.c`, **read-only**: Users (`sg-sysinfo users`: name, full
  name, description, Windows session, administrator; the SYSTEM account
  `sgsystem` with the Stained Glass picture) and Groups (`sg-sysinfo groups`:
  `sg-admins` shown as Administrators, `sgwine` as Users, with members and
  the Linux group). The node's one verb opens Control Panel > User Accounts
  (`control userpasswords2`), where accounts are changed (sg-admind).
  Shared Folders: Shares (`sg-sysinfo shares`: Samba's shares with the
  folder as a Windows path -- `wine_get_dos_file_name` --, type, comment,
  access; "Samba is not installed" without it), Sessions and Open Files (an
  administrator's, through sg-sysinfod).
- **Gate: `test/compmgmt-check.sh`**: the tree in Windows' order; Services,
  Device Manager, Disk Management and Event Viewer's Application log working
  inside it; Users and Groups over a made-up passwd/group
  (`SG_PASSWD_FILE`/`SG_GROUP_FILE`): alice a Windows user, bob an
  administrator, sgsystem the SYSTEM account, no actions; Administrators
  (sg-admins, bob) and Users (sgwine); Shares from a stand-in testparm
  (`SG_TESTPARM`) with the Windows path, and "Samba is not installed" with
  none. 17 checks. `-DSG_MUTANT_WINNAME` (Linux group names) turns it red.
  **Expanding a tree item rewrites the dump** (TVN_ITEMEXPANDED), or a gate
  reads the children's positions before they exist.

### System Information (msinfo32.exe)

`src/mmc/msinfo.c`, the same host as `sg-msinfo3264.exe` (App Paths
`msinfo32.exe`; Wine's own `system32\msinfo32.exe` hands off to it, wine-sg
0142), without the toolbar and Actions pane, as Windows' msinfo32 has none.
System Summary (OS name and the Windows version Wine reports, Wine's version,
kernel, maker/model, processor with cores and threads, BIOS and board, BIOS
mode, Secure Boot, directories, boot device, locale, user, time zone, memory,
virtualization), Hardware Resources (a pointer to Device Manager: Linux
assigns them), Components (Display with the Windows display modes, Sound,
Input, Network with Windows' adapters and addresses, Storage Disks and
Drives, USB), Software Environment (System Drivers = the kernel modules
bound to devices, Environment Variables, Running Tasks, Services, Startup
Programs). Hardware facts from `sg-sysinfo system|devices|disks`.
`msinfo32 /report FILE` writes every category as UTF-16 text and exits --
**only once the file is written**: the copy started first creates an event,
re-launches itself through the bridge with the report's full path and
`--report-done <event>`, and waits for it (a Windows process cannot wait on
the native bridge; wine-sg 0186 makes system32's msinfo32 wait for us).

- **Gate: `test/msinfo-check.sh`**: `msinfo32.exe` through App Paths; OS
  Name is sg-sysinfo's; Processor is /proc/cpuinfo's model and thread count;
  Installed Physical Memory is MemTotal; x64-based PC; the computer's name;
  Components > Display lists sg-sysinfo's display adapters; `/report`
  writes all 14 categories, and returns only when it has (a relative name,
  and system32's msinfo32 with 0186). `-DSG_MUTANT_MEM` and
  `-DSG_MUTANT_NOWAIT` (3 fail) turn it red.

### Disk Cleanup (cleanmgr.exe)

`src/mmc/cleanmgr.c`, the host as `sg-cleanmgr64.exe` (App Paths
`cleanmgr.exe`, wine-sg 0142's launcher), a dialog like Windows' "Disk Cleanup
for (C:)": "You can use Disk Cleanup to free up to N of disk space", the list
with ticks and sizes, the total, the description, "Clean up system files"
(itself again with `runas` and `/system`), OK and "Are you sure you want to
permanently delete these files?". Windows-side categories are deleted by the
program itself: Downloaded Program Files, Temporary Internet Files,
Temporary files (**only files not changed for a week**, as Windows). The
user's Linux side is `sg-sysinfo cleanup`/`clean` (Recycle Bin = the XDG
trash Wine uses, Thumbnails, Wine's downloads); `/system` asks sg-sysinfod
for `cleanup-system`/`clean-system` (update packages, old logs, the archived
journal, error reports; an administrator only). `/d X` picks the drive.

- **Gate: `test/cleanmgr-check.sh`**: `cleanmgr.exe` through App Paths, the
  sizes of a planted week-old and a new temporary file (only the old one
  counted), thumbnails and a trashed file; Windows' default ticks; ticking
  with the keyboard, OK, Yes: the old temporary file, the thumbnails and the
  trash go, today's temporary file stays. `-DSG_MUTANT_AGE` turns it red.

### Resource Monitor (resmon.exe)

`src/mmc/resmon.c`, the host as `sg-resmon64.exe` (App Paths `resmon.exe`,
wine-sg 0142's launcher), its own window: tabs Overview (CPU, Disk, Network,
Memory sections), CPU, Memory, Disk (with Storage: the drive letters' space),
Network (processes, TCP Connections, Listening Ports), and four graphs on the
right (CPU from `GetSystemTimes`, disk as the processes' summed I/O, network
from `GetIfTable` without loopback, memory load). **The processes are the
whole Linux machine's** (`sg-sysinfo processes`/`connections` once a second;
Wine's programs appear under their .exe names): CPU is the share of *all*
processors over the last second, as Windows counts it (one busy core of 12
is 8%), Average CPU over a minute; disk bytes per second from the kernel's
per-process counters (only where this user may read them); a connection
whose owner this user cannot see is "(not this user's)".

- **Gate: `test/resmon-check.sh`**: the gate's own loads, Python under
  names of their own (`cp /usr/bin/python3` -- the real interpreter, not a
  pyenv shim -- to `sg-rm-*`): a spinning process at one processor's share,
  a 300 MB working set, a writer syncing ~19 MB/s, a listener on 47123;
  each shows on its tab; Storage lists C:. `-DSG_MUTANT_CPU` turns it red.


## PDF Viewer (sg-pdf)

`src/pdf/`: `main.c` (window, toolbar, commands, the command line, the
dump), `view.c` (the continuous scroll: layout, painting, zoom, rotation,
selection, search, links), `bridge.c` (the pipe to poppler and the render
thread), `side.c` (thumbnails and the bookmarks tree), `print.c`. The icon is
drawn by `gen-icon.py` at build time; `pdf.rc` carries it, a common-controls 6
manifest, the password prompt and the version information (FileDescription
"PDF Viewer", which Settings > Default apps shows).

- **Poppler does the PDF, we do the window.** sg-session's `sg-pdf` (Debian's
  poppler through GObject introspection, cairo) opens the document, renders a
  page to BGRA at any scale and quarter turn, and answers each page's text
  with a box per UTF-16 unit, its links, the outline and search hits -- all
  in top-left page points; the protocol is in sg-session's CLAUDE.md. Wine
  gives a Windows program no pipe to a native one, so the viewer re-launches
  itself as `sg-pdf --bridge wine <itself> --bridged <args>` (as sg-dictate
  does); `SG_PDF` names another sg-pdf. Without it the window says what is
  missing rather than failing silently.
- **Rendering is off the UI thread.** The view asks, at each paint, for the
  visible pages (and the next) at the current scale and rotation; the render
  thread reads the pixels straight into a DIB section and posts it back.
  While a new zoom renders, the old bitmap is stretched in its place. Pages
  far from the view drop their bitmaps. One critical section keeps each
  request and its answer together on the shared pipe (the UI thread asks for
  text, links and search itself -- small answers).
- **Highlights are multiplied into the page** (`PatBlt` with DPa): the
  selection light blue, search hits yellow, the current hit orange, and the
  text under them stays black.
- **Selection is by character boxes**: a caret position is the nearest box's
  near or far side (above the first line: the start; below the last: the
  end); a drag can cross pages; Ctrl+A loads every page's text. Copied text
  has `\r\n` line ends.
- **Print** renders each page at the printer's resolution (at most 300 dpi),
  turns a page whose shape is the paper's other way round, fits and centres
  it; `/p <file>` is Explorer's Print verb. No printer in the gate's prefix,
  so printing is not gated yet (`SG_PDF_PRINT_TO` exists for when it is).
- **Things that bit.** cairo (the gate's PDF maker) places every page's links
  by the *last* page's height, so a landscape page at the end moved page 1's
  links -- the gate's document is all Letter. The render queue's lock must
  exist before the first paint (it did not, and the first window hung).
  `pkill -f` on a pattern that appears in your own command line kills your
  shell: the gate uses `taskkill`.
- **`SG_PDF_DUMP=<file>`** (a Windows path) is rewritten after every paint
  and change: file, title, bridged, pages, current page, zoom, fit, rotation,
  sidebar, bookmarks, hits and the current hit's screen rectangle, the
  selection's length, each visible page's screen rectangle and whether its
  bitmap is current, its first word's screen rectangle, its links, the
  thumbnails, the tree's items, the toolbar's buttons and boxes, the focus,
  an error.
- **Gate: `test/pdf-check.sh`** (Xvfb picks its display; the PDF is made at
  test time by `test/mkpdf.py` with cairo; `SG_PDF_HELPER` or a sibling
  `../sg-session/bin/sg-pdf` or `/usr/bin/sg-pdf`): a PDF in a folder with a
  space opened through its association (`wine start`), 4 pages, title,
  bookmarks, page 1's word in ink where poppler puts it; Page Down to page 2,
  its word and purple bar on the screen where the layout puts them; find
  "zebra" (2 hits, the current one orange, F3 to page 3, Escape); a mouse
  drag over page 2's word and Ctrl+C gives exactly "Zebra" (xclip); Ctrl+A
  copies all four pages in order; zoom in/out, Ctrl+0; rotate; page 1's link
  to page 3 and its web link; the bookmarks and thumbnails going to their
  pages; **double-clicking the .pdf in an Explorer window** (the icon found
  by its purple band in the screenshot); a qpdf-encrypted PDF asking for its
  password; a damaged file; no sg-pdf. 32 checks. Mutants
  `-DSG_MUTANT_FIRSTPAGE` (every page renders page 1), `-DSG_MUTANT_COPY`
  (the copy starts one character late) and `-DSG_MUTANT_NOHITS` (hits not
  drawn), run with `SG_PDF_EXE=`, each turn it red. Screenshots
  `build/pdf-*.png`.
- **Not yet:** forms, annotations and highlighting of our own, two-page view,
  presentation mode, a printing gate, remembering the last page per file.

## Get a web browser (sg-browser)

`src/browser/`: `main.c` (the routing, the two windows, the default
browser), `fetch.c` (the source, the download, SHA-256, running the
installer, winget), `manifest.c` (plain C, no Windows headers: reading winget
installer manifests -- a small, regular subset of YAML -- and choosing the
installer; unit-tested natively). The icon is drawn by `gen-icon.py`.

- **Licensing.** We never ship or redistribute a browser. Install fetches
  the maker's own installer (Mozilla's CDN, Google's, Brave's GitHub
  releases) at the user's request; which file, its SHA-256 and how to run it
  silently come from github.com/microsoft/winget-pkgs (MIT-licensed data):
  `ListUrl` + `m/Mozilla/Firefox` (GitHub's contents API: the version
  folders; the newest by winget's order, sub-packages like `ESR` and `de`
  skipped), then `RawUrl` + `.../<version>/<Id>.installer.yaml` (else the
  singleton `<Id>.yaml`). `HKLM\Software\Stained Glass\Web Browsers`
  `ListUrl`/`RawUrl` point at a mirror (an organisation's; the gate's);
  `SG_BROWSER_LIST_URL`/`SG_BROWSER_RAW_URL` for tests. **A download whose
  SHA-256 is not the manifest's is deleted and never run**; a manifest with
  no SHA-256 is refused. The installer chosen: x64 first, the user's locale,
  machine scope for an administrator and user scope otherwise; run silently
  by type (msi/wix through msiexec `/quiet /norestart`, nullsoft `/S`, inno,
  burn, exe with the manifest's switches), through `ShellExecuteEx` and
  `runas` when it needs elevation (the consent prompt). With winget (the
  user's own: PATH, App Paths, `SG_BROWSER_WINGET`) it is `winget install
  --id <Id> -e --silent --accept-package-agreements
  --accept-source-agreements --disable-interactivity` instead.
- **Offers** are `HKLM\...\Web Browsers\Offers\NN` (`Id`, `Name`,
  `Publisher`, `Description`, `Colour`) -- Firefox, Chrome, Brave by
  default; each has a letter badge in a colour, never a logo.
- **Installed** means registered under `Software\Clients\StartMenuInternet`
  (HKCU, then HKLM; Wine's IEXPLORE.EXE excluded), with the program on
  disk. **The default** is set as Settings > Default apps sets it: the user's
  `Classes\http`, `https` (a copy of the browser's URL ProgID), `.htm`,
  `.html`, and `UrlAssociations\{http,https}\UserChoice\ProgId`.
- **Wine's HKEY_CLASSES_ROOT is the machine's classes only** -- the user's
  `Software\Classes` is not merged in, as Windows does (a user key never
  shadows the machine's; measured: a value in both reads as the machine's).
  So the user's copies above are not what ShellExecute sees: every web link
  still arrives here, and **this program honours UserChoice** (opens the
  chosen browser without a window). Class lookups here, and in Settings >
  Default apps (`src/control/set_apps.c`), read the user's classes first (a
  per-user browser registers there); Default apps shows UserChoice / the
  user's class as the current choice. For
  other file types the user's choice still does not take effect -- a
  wine-sg change (a merged HKCR) is the real fix.
- **`SG_BROWSER_DUMP=<file>`**: the mode (get, choose, none), the link, the
  method (winget, manifest), the default, the installed browsers, each
  offer's state and message, every clickable thing's screen centre, the
  package (id, version, type, URL) and the installer's arguments, what was
  opened.
- **Gate: `test/browser-check.sh`** (Xvfb picks its display): the manifest
  reader's native unit test (`test/browser-manifest-test.c`, 23 checks:
  shapes of the real Firefox, Chrome and Brave manifests); a
  winget-pkgs-shaped source served by `python3 -m http.server` with
  stand-ins built at test time (`test/sg-browser-fake.c`: an installer that
  registers a browser, the browser, winget). With no browser a link opens
  Get a web browser naming it; a tampered download is refused and never run;
  Install takes 1.10.0 over 1.9.0 and the x64 installer, runs it with the
  manifest's switches, the browser becomes the default (UserChoice and the
  user's http class) and the link opens in it; the next link and an .html
  file open there without a window; reset, one browser: opens there and is
  the default again; two: "How do you want to open this?", the one picked
  opens it and with Always becomes the default; winget: `winget install --id
  ... -e --silent ...`; Internet Explorer is offered; Settings > Default
  apps shows the installed browser as the web browser. 29 checks.
  `SG_BROWSER_ONLINE=1` also installs the real Mozilla Firefox from GitHub's
  manifests and Mozilla's CDN. Mutants `-DSG_MUTANT_NOHASH` (no SHA-256
  check) and `-DSG_MUTANT_NODEFAULT` (no default set), via
  `SG_BROWSER_EXE=`, turn it red. Screenshots `build/browser-*.png`.
- sg-session's first-run setup (OOBE) installs Firefox for everyone from
  Mozilla's "latest" link (`lib/sg-oobe-browser`); this program is the
  per-user path afterwards, and the one that checks a manifest's SHA-256.
