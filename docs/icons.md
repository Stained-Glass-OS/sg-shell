# Icons and drawn glyphs

Every icon, picture strip and glyph Stained Glass OS draws itself: where it
is drawn, how, and at which sizes. Our own artwork only -- nothing here is,
or is traced from, anyone else's icons.

David (2026-10-06): "Review all the icons we create and get them all
rendered at a better resolution." Two rules came out of it:

1. **Icon files carry every size a display scale asks for**: 16, 20, 24,
   32, 40, 48, 64, 96, 128 and 256 px (100% to 250%, small and large icons),
   each frame drawn for its size, not stretched from another.
2. **Nothing round or slanted is drawn with plain GDI on screen.** GDI's
   Ellipse, RoundRect, Polygon, Polyline, Arc, Pie, Chord and slanted lines
   have no antialiasing; on a window they give stepped edges, worse at 150%.
   They are drawn supersampled (four times larger, averaged down).

## 1. Icon files (.ico)

### How they are made: `tools/sgicon.py`

Each icon is a small Python drawing (PIL shapes at coordinates: a vector
drawing, no bitmaps committed). `sgicon.write_ico(path, draw)`:

- `draw(S)` is called once per frame with S = 4 x the frame's size (or one
  1024 px drawing is given), and the result is reduced premultiplied with
  Lanczos, so edges are exact at every size;
- frames of 24 px and under are sharpened slightly (strokes stay crisp
  rather than grey); a drawing may give its own simpler picture for 32 px
  and under (`small=`, used by the Control Panel's icon);
- frames under 256 px are 32-bit DIBs with an AND mask (every reader takes
  them), the 256 px frame is PNG.

`sgicon.write_hicolor(root, name, draw)` writes a Linux icon theme tree
(`<n>x<n>/apps/name.png`, 16, 22, 24, 32, 48, 64, 96, 128, 256, 512 px).

Gate: `test/icon-sizes-check.sh` (`make test-icon-sizes`) runs every
generator the Makefile names and checks every .ico has all ten frames (256
PNG, others DIB, none empty), that every `ICON` an .rc names is generated,
the consoles' strips and SG Office's hicolor tree. Mutant:
`SG_MUTANT_ICON_SIZES=1`.

### The icons

| Icon file | Generator | Program | Drawing |
|---|---|---|---|
| sg-browser.ico | src/browser/gen-icon.py | web browser getter | globe on a purple disc, download badge |
| sg-bugreport.ico | src/bugreport/gen-icon.py | Report a problem | speech bubble with "!" |
| sg-calc.ico | src/sg-calc-icon.py | Calculator | purple calculator |
| sg-charmap.ico | src/charmap/gen-icon.py | Character Map | grid with an "A" |
| sg-clock.ico | src/clock/gen-icon.py | Alarms & Clock | clock face on a tile |
| sg-control.ico | src/control/gen-icon.py | Control Panel | sliders on a panel (simpler at <= 24 px) |
| sg-fontview.ico, sg-fonts.ico | src/fontview/gen-icon.py | Font Viewer, Fonts folder | page with an "A"; folder with a page |
| sg-magnify.ico | src/magnify/gen-icon.py | Magnifier | lens and handle |
| sg-media.ico | src/sg-media-icon.py | Media Player | play triangle on a gradient tile |
| sg-mmc.ico, sg-eventvwr.ico, sg-msinfo32.ico, sg-resmon.ico, sg-cleanmgr.ico | src/mmc/gen-icons.py | the consoles | computer, events, system, scan, disk |
| sg-mstsc.ico | src/sg-mstsc-icon.py | Remote Desktop Connection | two screens and an arrow |
| sg-net.ico | src/sg-net-icon.py | Network Connections | monitor with a globe |
| sg-osk.ico | src/osk/gen-icon.py | On-Screen Keyboard | keyboard |
| sg-paint.ico | src/paint/gen-icon.py | Paint | palette and brush |
| sg-pdf.ico | src/pdf/gen-icon.py | SG PDF | page, band and pen |
| sg-photos.ico | src/sg-photos-icon.py | Photos | picture on a gradient tile |
| sg-rootterm.ico | src/rootterm/gen-icon.py | root console | terminal with "#" |
| sg-settings.ico | src/settings/gen-icon.py | Settings | gear on a tile |
| sg-snip.ico | src/sg-snip-icon.py | Snipping Tool | snipped area in a dashed frame |
| sg-sticky.ico | src/sticky/gen-icon.py | Sticky Notes | note with lines |
| sg-store.ico | src/store/gen-icon.py | SG Store | bag with an arrow |
| sg-taskmgr.ico | src/taskmgr/gen-icon.py | Task Manager | window with a graph |
| sg-terminal.ico | src/terminal/gen-icon.py | Terminal | prompt in a window |
| sg-wordpad.ico | src/wordpad/gen-icon.py | WordPad | page and nib |
| sg-zip.ico | src/zip/gen-icon.py | compressed folders | folder with a zip |
| sg-documents/-spreadsheets/-presentations(.ico, -file.ico) | office/gen-icons.py | SG Office programs and their files | tile with lines/grid/chart; page with the tile |

All have the ten frames. SG Office's program icons are also installed for
Linux in hicolor at 16-512 px (`debian/rules`, sg-office package).

Not files but fonts: `src/sg-icons-font.py` draws Stained Glass Icons, the
caption-button glyphs programs ask Segoe MDL2/Fluent Icons for (a TrueType
outline font: sharp at every size by nature; gate `test-icons-font`).

### Linux apps' icons (Start, taskbar, .desktop files)

`src/linuxapps/sg-linuxapp.c` makes an .ico for each Linux app from the
icon theme: every hicolor PNG size the theme has (256, 128, 96, 64, 48,
32, 24, 22, 16), or, for an app with only an SVG, the SVG drawn by
rsvg-convert at 256, 96, 64, 48, 40, 32, 24, 20 and 16 px (each drawn for
its size). The taskbar (wine-sg 0612) and the .desktop files' icon handler
(0767) use the same .ico. Gate: `test/linuxapps-check.sh` (mutant
`SG_MUTANT_LINUX_ICON_FEW`: the old 48 and 256 only).

### Picture strips (image lists)

| Strip | Drawn by | Sizes | Used |
|---|---|---|---|
| mmc16/20/24/32/40.bmp | src/mmc/gen-icons.py (same drawings as the icons) | 16, 20, 24, 32, 40 | the consoles' tree, list and toolbar; `load_icons` takes the largest no bigger than 16 px at the display scale (125%: 20, 150%: 24, 200%: 32, 250%: 40). Gate: `test/compmgmt-check.sh` (at 150%: 24 px; mutant `SG_MUTANT_MMC_ONE_STRIP`) |

Other programs' toolbar and list pictures are drawn at run time at the
window's scale (section 2), not shipped as bitmaps.

### In wine-sg

| Icon | Where | Sizes |
|---|---|---|
| folder, open folder, document, text file, Notepad, This PC, Desktop, Documents, fixed drive | wine-sg `theme/icons.py` (SVG, line widths per size), rendered by build.sh | 16-256 (ten) |
| IDI_WINLOGO (our mark: a window with no icon) | `theme/icons.py` (0441's drawing) | 16-256 (wine-sg 1241) |
| IDI_APPLICATION (a program without its own icon; was Wine's wine glass) | `theme/icons.py`: a program window under our band | 16-256 (1241) |
| mmc, eventvwr, resmon, cleanmgr launchers | .rc bytes (wine-sg 0463, 1241), from src/mmc/gen-icons.py | 16-256 (1241; were 16-64) |
| the file dialogs' view strip | `theme/icons.py` STRIPS | as comctl32's |
| the Light theme's parts (check boxes, drop-down glyphs ...) | wine-sg 0031 SVGs | one SVG per DPI size |

Gate: wine-sg `test/iconframes-gate.sh` (sources always; the installed
user32 and launchers with `WINE=`). Mutants: `SG_MUTANT_ICON_FRAMES=1`,
the series without 1241.

## 2. Glyphs drawn at run time

### How: `src/sg-smooth.h` (wine-sg: `include/wine/sg_smooth.h`)

Three ways, all drawing four times larger and averaging down:

- **`sg_smooth(dc, x, y, w, h, art, arg)`**: an art callback draws on black
  at 4x; coverage becomes alpha and it is blended in. For art written for
  it (Start's rail glyphs, the user badge, voice typing's toolbar).
- **Drop-ins** with GDI's own arguments, using the pen and brush selected
  in the DC: `sg_ellipse`, `sg_round_rect`, `sg_pie`, `sg_chord` (box: pixel
  edges, so the same pixels as GDI), `sg_polygon`, `sg_polyline`, `sg_arc`,
  `sg_line` (points: pixel centres). What is under the shape is copied up
  first, so any colour blends right, black too. A large rounded rectangle
  keeps GDI's own straight edges; only its corners are supersampled.
- **A region**, for a glyph function making several calls:
  `big = sg_ss_begin(&ss, dc, x, y, w, h, stroke); glyph(big, ...);
  sg_ss_end(&ss);` -- a world transform draws the function's own
  coordinates 4x; `stroke` (its line width) keeps level lines crisp.

Some programs supersample on their own canvas (noted below as "own 4x").

Gates: `test/smooth-shapes-check.sh` (each drop-in and a region against
GDI's own drawing: many colours at the edges, in the same place, straight
edges unchanged; mutant `-DSG_MUTANT_JAGGED`), `test/raw-shapes-check.sh`
(no raw round/slanted GDI call in `src/` unless in supersampled art, or
marked `sg-smooth:` with the reason), and per-program colour counts:
`test/start-check.sh` (rail glyphs, user badge), `test/dictate-check.sh`
(gear, mic, close; `SG_MUTANT_JAGGED_DICTATE`), `test/clock-check.sh`
(round buttons; `SG_MUTANT_JAGGED`).

### sg-shell programs

| Program | Glyphs | Method | Scaled |
|---|---|---|---|
| Start (sg-start.c) | rail glyphs, power, user badge, search magnifier | sg_smooth art | S() |
| Start, older looks | Horizon/Seven tiles, buttons, arrows, picture | drop-ins | S() |
| Voice typing (sg-dictate.c) | round buttons, mic, gear, close | sg_smooth art | g_icon_scale (1.0 until the bar scales its layout) |
| Alarms & Clock | toggles, round buttons, play/pause/reset/lap/+/x, timer ring | drop-ins | S() |
| Calculator | backspace, menu, history, clear-list glyphs | region | S() |
| Control Panel | category/applet icons | own 4x (control/icons.c) | per size |
| Control Panel | back/forward/up arrows; toggles; display preview | region; drop-ins | S() |
| Character Map, Font Viewer | page glyph | drop-in | S() |
| Magnifier | toolbar triangle, cog | drop-ins | fixed px |
| Media Player | transport glyphs | own 4x (glyph_in) | S() |
| Media Player | seek/volume thumbs, play disc, rounded fills | drop-ins | S() |
| Network flyout (sg-netflyout.c) | tray icon | own 4x (make_icon) | per size |
| Network flyout | Wi-Fi bars, lock, tick | drop-ins | fixed px |
| Network Connections (sg-ncpa.c) | adapter pictures, shield | drop-ins | fixed px |
| Notifications (sg-notify.c) | tray icon | own 4x | per size |
| Notifications | close mark | drop-in | U() |
| Volume (sg-volume.c) | tray icon | own 4x | per size |
| Volume | slider thumb | drop-in | fixed |
| Battery (sg-battery.c) | tray icon | own 4x | per size |
| On-Screen Keyboard | key glyphs, title-bar close, size grip | drop-ins | per key / fixed |
| Paint | ribbon glyphs | own 4x (paint/glyphs.c) | S() |
| Paint | ribbon arrows, checks | drop-ins | S() |
| Paint | the picture itself | aliased on purpose (Paint's tools) | -- |
| SG PDF | toolbar glyphs, tool panel glyphs | region | dpx() |
| SG PDF | ellipse being drawn | drop-in | dpx() |
| Photos | toolbar glyphs | own 4x (render_glyph) | per size |
| Photos | rounded buttons | drop-in | S() |
| Restart / Defender notices | restart arrow; shield | region; drop-in | S() |
| Snipping Tool | toolbar glyphs | own 4x | per size |
| Snipping Tool | rounded buttons | drop-in | S() |
| Snipping Tool | ink and free-form outline | as drawn (the user's strokes) | -- |
| SG Store | cards, badges, buttons, chips, checks, arrows | drop-ins | dpx() |
| Sticky Notes | dots | drop-in | S() |
| Task Manager | graph lines, sort arrows, chevron disc, buttons | drop-ins | S() |
| Terminal | rounded box-drawing arcs, tab close, ticks | drop-ins; region | per cell / S() |
| WordPad | ribbon glyphs | own 4x (wordpad/glyphs.c) | S() |
| WordPad | ruler markers, ribbon arrows, checks | drop-ins | S() |
| Compressed folders | back/forward/up arrows | region | S() |
| Standalone taskbar (sg-taskbar.c) | Start mark | drop-in | fixed |

### wine-sg (shell parts drawn in Wine)

| Where | Glyphs | Method |
|---|---|---|
| explorer systray.c (taskbar) | Start mark, search magnifier, Glass orb | sg_smooth art (0856) |
| explorer systray.c | Rounded look's plates, search box, pills; Horizon/Glass button outlines | sgs_round_rect (1240) |
| explorer fileexplorer.c | tabs, tab close mark | drop-ins (1240); toolbar glyphs are GDI+ (antialiased) |
| notepad | tabs | sgs_round_rect (1240) |
| user32, comctl32 combo.c | flat drop-down chevron | region (1240) |
| comctl32 listview.c | group chevron | region (1240) |
| shell32 fileopdlg.c | conflict dialog choice glyphs | region (1240) |
| windows.ui toast.c | toast close mark | sgs_line (1240) |

Gate: wine-sg `test/smoothshapes-gate.sh` (search box, combo chevron, File
Explorer's tab close, Notepad's tab; mutant `SG_MUTANT_JAGGED` per file)
and `test/smoothicons-gate.sh` (0856).

## 3. Elsewhere

- **sg-compositor** (decor.c) draws a Linux program's Horizon and Glass
  caption buttons itself: the close cross by each pixel's share of its two
  strokes, the buttons' and the bar's rounded corners by each pixel's share
  of the outline, 4 x 4 samples (0.2.0+sg42; gate `test/decorsmooth-gate.sh`,
  mutant `SG_MUTANT_JAGGED_DECOR`).
- **sg-session**: `greeter/sg-smooth.h` (a copy of ours) for the login and
  lock screens' account circle and power button, Setup's glyphs and the
  first-run setup's pictures, toggles and arrows; Setup's icon
  (`setup/make-icon.py`, supersampled) at the ten sizes (0.1.0-141; gate
  `test/smooth-glyphs-test.sh`). The stained-glass backdrop behind Setup
  stays hard-edged (supersampling a whole screen costs hundreds of MB).
- The drop-ins and regions draw plain GDI on a DC already in GM_ADVANCED
  (one inside a region), so nesting them is harmless.

## 4. Open

- Some glyphs are sized in fixed pixels rather than by the display scale
  (Magnifier's bar, Network Connections' adapter pictures, the network
  flyout's list glyphs, On-Screen Keyboard's title bar): smooth, but small at
  150% and above until those programs scale their layout.
- No scalable (SVG) Linux icons: the drawings are Python, and hicolor PNGs
  go to 512 px.
