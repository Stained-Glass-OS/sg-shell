# sg-shell — Windows 10-style panels for Stained Glass OS. See CLAUDE.md.
MINGW64 ?= x86_64-w64-mingw32-gcc
MINGW32 ?= i686-w64-mingw32-gcc
# Not CFLAGS: dpkg-buildpackage exports its own CFLAGS (no -municode),
# which would drop the Unicode entry point and break the link.
SG_CFLAGS := -O2 -municode -mwindows -Wall -Wextra
# Console subsystem: gpresult is a command-line tool whose report must reach the
# console/pipe it is run from, so it links -mconsole, not -mwindows.
SG_CON_CFLAGS := -O2 -municode -mconsole -Wall -Wextra
LIBS     = -lshell32 -lshlwapi -lgdi32 -luser32 -lole32 -luuid
BUILD    = build

PANELS = sg-taskbar sg-start
# The Control Panel is several files (src/control/) and needs more of Windows.
CONTROL_SRC  = $(wildcard src/control/*.c)
CONTROL_LIBS = -lcomctl32 -lshell32 -lgdi32 -luser32 -ladvapi32 -lmsimg32 -liphlpapi -lws2_32 \
               -lole32 -luuid -lwindowscodecs -lcomdlg32 -lshlwapi -lwininet -lversion
# The network programs: sg-netclient.h and a common-controls 6 manifest.
NET_PANELS = sg-ncpa sg-netflyout
WINDRES64 ?= x86_64-w64-mingw32-windres
WINDRES32 ?= i686-w64-mingw32-windres
NET_LIBS = -lcomctl32 -luxtheme $(LIBS)
# Voice typing's toolbar (sg-dictate): its engine is sg-session's sg-dictate.
DICTATE_LIBS = -lshell32 -lgdi32 -luser32
# Compressed (zipped) Folders (sg-zip): our own inflate/deflate, src/zip/.
ZIP_SRC  = $(wildcard src/zip/*.c)
ZIP_LIBS = -lcomctl32 -lshell32 -lshlwapi -lgdi32 -luser32 -lole32 -lcomdlg32
# Media Player (sg-media): DirectShow (quartz, winegstreamer) and its own icon,
# drawn by src/sg-media-icon.py at build time.
MEDIA_LIBS = -lole32 -luuid -lstrmiids -lgdi32 -luser32 -lshell32 -lcomdlg32 -ladvapi32 -lmsimg32
# Calculator (sg-calc): calc.exe. Its icon is drawn at build time (src/sg-calc-icon.py).
CALC_LIBS = -lgdi32 -luser32 -ladvapi32 -lm
# Photos (sg-photos): WIC, and its own icon drawn by src/sg-photos-icon.py at build time.
PHOTOS_LIBS = -lwindowscodecs -lole32 -luuid -lshlwapi -lshell32 -lcomctl32 -lcomdlg32 -lgdi32 -luser32 -lmsimg32
# Task Manager (sg-taskmgr): several files, an icon generated at build time.
TASKMGR_SRC  = $(wildcard src/taskmgr/*.c)
TASKMGR_LIBS = -lntdll -lversion -liphlpapi -ladvapi32 -lcomdlg32 -lshell32 -lgdi32 -luser32 -lole32 -luuid
# Paint (mspaint.exe): several files (src/paint/), WIC, an icon generated at build time.
PAINT_SRC  = $(wildcard src/paint/*.c)
PAINT_LIBS = -lcomdlg32 -lcomctl32 -lshell32 -lgdi32 -luser32 -lmsimg32 -lole32 -luuid -lwindowscodecs
# Sticky Notes (sg-sticky): src/sticky/.
STICKY_LIBS = -lcomctl32 -lshell32 -lgdi32 -luser32 -lole32 -luuid
# Snipping Tool (sg-snip): WIC for saving, its icon drawn at build time.
SNIP_LIBS = -lcomctl32 -lcomdlg32 -lshell32 -lgdi32 -luser32 -lole32 -luuid -lwindowscodecs -lmsimg32
# Character Map (sg-charmap): character names generated from the Unicode
# Character Database (Debian's unicode-data) into build/charmap-names.c.
CHARMAP_LIBS = -lcomctl32 -lgdi32 -luser32 -ladvapi32
UNICODE_DATA ?= /usr/share/unicode/UnicodeData.txt
# Magnifier (magnify.exe) and the On-Screen Keyboard (osk.exe): src/magnify/ and
# src/osk/, their icons drawn at build time.
MAGNIFY_LIBS = -lshell32 -ldwmapi -lmsimg32 -lgdi32 -luser32 -ladvapi32
OSK_LIBS     = -lwinmm -lshell32 -lgdi32 -luser32 -ladvapi32
# The administrative consoles (sg-mmc: services.msc, eventvwr.msc, devmgmt.msc,
# diskmgmt.msc, compmgmt.msc; the same program as sg-eventvwr for eventvwr.exe):
# src/mmc/, pictures drawn at build time by gen-icons.py.
MMC_SRC  = $(wildcard src/mmc/*.c)
MMC_LIBS = -lcomctl32 -lcomdlg32 -luxtheme -lsetupapi -liphlpapi -lws2_32 -lshell32 -ladvapi32 -lgdi32 -luser32 -lole32 -luuid
# Terminal (wt.exe): src/terminal/, its screen (vt.c) also built natively for its unit test.
TERMINAL_SRC  = $(wildcard src/terminal/*.c)
TERMINAL_LIBS = -lshell32 -lgdi32 -luser32 -ladvapi32 -lole32
# Alarms & Clock (sg-clock): src/clock/, its icon drawn at build time.
CLOCK_LIBS = -lwinmm -lshell32 -lgdi32 -luser32 -ladvapi32 -lm
# WordPad (wordpad.exe, write.exe): src/wordpad/, on RichEdit; .docx/.odt through our
# own readers and writers over src/zip/zipcore.c; its icon drawn at build time.
WORDPAD_SRC  = $(wildcard src/wordpad/*.c) src/zip/zipcore.c
WORDPAD_LIBS = -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi -lgdi32 -luser32 -lmsimg32 -lole32 -loleaut32 -luuid \
               -lwindowscodecs -ladvapi32
# SG PDF (sg-pdf, the PDF viewer and editor): src/pdf/, its icon drawn at build
# time; MuPDF is sg-session's sg-pdf, reached through its bridge.
PDF_SRC  = $(wildcard src/pdf/*.c)
PDF_LIBS = -lcomctl32 -lcomdlg32 -lshell32 -lwinspool -lgdi32 -luser32 -ladvapi32 -lole32
# The font viewer (fontview.exe) and the Fonts folder (sg-fontview): src/fontview/,
# its icons drawn at build time.
FONTVIEW_SRC  = $(wildcard src/fontview/*.c)
FONTVIEW_LIBS = -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi -lgdi32 -luser32 -ladvapi32 -lwinspool -lole32
# Get a web browser (sg-browser): src/browser/, WinINet and BCrypt; its
# manifest reader (manifest.c) is plain C, also built natively by its gate.
BROWSER_SRC  = $(wildcard src/browser/*.c)
BROWSER_LIBS = -lsetupapi -lwininet -lbcrypt -lshlwapi -lshell32 -lgdi32 -luser32 -ladvapi32 -lole32
# SG Store (sg-store): src/store/, reusing "Get a web browser"'s download/verify/
# install engine (src/browser/fetch.c + manifest.c); its icon is drawn at build
# time. See docs/decisions/0017-sg-store.md.
STORE_SRC  = src/store/main.c src/store/details.c src/store/catalog.c src/store/sysinstall.c src/store/icons.c src/browser/fetch.c src/browser/manifest.c src/zip/zipcore.c
STORE_LIBS = -lsetupapi -lwininet -lbcrypt -lshlwapi -lshell32 -lgdi32 -luser32 -ladvapi32 -lole32 -luuid -lwindowscodecs -lmsimg32 -lcomdlg32
# Console tools (subsystem console), built the same way but without -mwindows.
CONSOLE_TOOLS = sg-gpresult

.PHONY: all build test clean test-appmode lint
all: build

build:
	@mkdir -p $(BUILD)
	@command -v $(MINGW64) >/dev/null 2>&1 || { echo "SKIP: $(MINGW64) not installed"; exit 0; }
	@for p in $(PANELS); do \
	    $(MINGW64) $(SG_CFLAGS) -o $(BUILD)/$$p'64'.exe src/$$p.c $(LIBS) && echo "built $$p (64-bit)"; \
	    $(MINGW32) $(SG_CFLAGS) -o $(BUILD)/$$p'32'.exe src/$$p.c $(LIBS) && echo "built $$p (32-bit)"; \
	done
	@# Remote Desktop Connection: a panel with its own icon (src/sg-mstsc-icon.py)
	@python3 src/sg-mstsc-icon.py $(BUILD)/sg-mstsc.ico
	@$(WINDRES64) -I src -I $(BUILD) src/sg-mstsc.rc -O coff -o $(BUILD)/sg-mstsc-res64.o
	@$(WINDRES32) -I src -I $(BUILD) src/sg-mstsc.rc -O coff -o $(BUILD)/sg-mstsc-res32.o
	@$(MINGW64) $(SG_CFLAGS) -o $(BUILD)/sg-mstsc64.exe src/sg-mstsc.c $(BUILD)/sg-mstsc-res64.o $(LIBS) && echo "built sg-mstsc (64-bit)"
	@$(MINGW32) $(SG_CFLAGS) -o $(BUILD)/sg-mstsc32.exe src/sg-mstsc.c $(BUILD)/sg-mstsc-res32.o $(LIBS) && echo "built sg-mstsc (32-bit)"
	@# Control Panel, with its own icon (src/control/gen-icon.py: smooth at every size)
	@python3 src/control/gen-icon.py $(BUILD)/sg-control.ico
	@$(WINDRES64) -I src/control -I $(BUILD) src/control/control.rc -O coff -o $(BUILD)/sg-control-res64.o
	@$(WINDRES32) -I src/control -I $(BUILD) src/control/control.rc -O coff -o $(BUILD)/sg-control-res32.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-control64.exe $(CONTROL_SRC) $(BUILD)/sg-control-res64.o $(CONTROL_LIBS) && echo "built sg-control (64-bit)"
	@$(MINGW32) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-control32.exe $(CONTROL_SRC) $(BUILD)/sg-control-res32.o $(CONTROL_LIBS) && echo "built sg-control (32-bit)"
	@# Settings (SystemSettings, ms-settings:) is the Control Panel's program in its own frame
	@# (src/control/settings.c), with its own icon and a common-controls 6 manifest.
	@python3 src/settings/gen-icon.py $(BUILD)/sg-settings.ico
	@$(WINDRES64) -I src/settings -I $(BUILD) src/settings/settings.rc -O coff -o $(BUILD)/sg-settings-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-settings64.exe $(CONTROL_SRC) $(BUILD)/sg-settings-res64.o $(CONTROL_LIBS) \
	    && echo "built sg-settings (64-bit)"
	@python3 src/sg-net-icon.py $(BUILD)/sg-net.ico
	@$(WINDRES64) -I src -I $(BUILD) src/sg-net.rc -O coff -o $(BUILD)/sg-net-res64.o
	@for p in $(NET_PANELS); do \
	    $(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/$$p'64'.exe src/$$p.c $(BUILD)/sg-net-res64.o $(NET_LIBS) \
	        && echo "built $$p (64-bit)"; \
	done
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-battery64.exe src/sg-battery.c \
	    -lshell32 -luser32 -lgdi32 -municode -mwindows && echo "built sg-battery (64-bit)"
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-defender-notice64.exe src/sg-defender-notice.c \
	    -lshell32 -luser32 -lgdi32 -ladvapi32 -municode -mwindows && echo "built sg-defender-notice (64-bit)"
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-volume64.exe src/sg-volume.c \
	    -lshell32 -luser32 -lgdi32 -municode -mwindows && echo "built sg-volume (64-bit)"
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-notify64.exe src/sg-notify.c \
	    -lshell32 -luser32 -lgdi32 -ladvapi32 -municode -mwindows && echo "built sg-notify (64-bit)"
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-dictate64.exe src/sg-dictate.c $(DICTATE_LIBS) \
	    && echo "built sg-dictate (64-bit)"
	@python3 src/zip/gen-icon.py $(BUILD)/sg-zip.ico
	@$(WINDRES64) -I src/zip -I $(BUILD) src/zip/sg-zip.rc -O coff -o $(BUILD)/sg-zip-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-zip64.exe $(ZIP_SRC) $(BUILD)/sg-zip-res64.o $(ZIP_LIBS) \
	    && echo "built sg-zip (64-bit)"
	@python3 src/sg-media-icon.py $(BUILD)/sg-media.ico
	@$(WINDRES64) -I $(BUILD) src/sg-media.rc -O coff -o $(BUILD)/sg-media-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-media64.exe src/sg-media.c $(BUILD)/sg-media-res64.o $(MEDIA_LIBS) \
	    && echo "built sg-media (64-bit)"
	@python3 src/sg-calc-icon.py $(BUILD)/sg-calc.ico
	@$(WINDRES64) -I $(BUILD) src/sg-calc.rc -O coff -o $(BUILD)/sg-calc-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-calc64.exe src/sg-calc.c $(BUILD)/sg-calc-res64.o $(CALC_LIBS) \
	    && echo "built sg-calc (64-bit)"
	@python3 src/sg-photos-icon.py $(BUILD)/sg-photos.ico
	@$(WINDRES64) -I src -I $(BUILD) src/sg-photos.rc -O coff -o $(BUILD)/sg-photos-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-photos64.exe src/sg-photos.c $(BUILD)/sg-photos-res64.o $(PHOTOS_LIBS) \
	    && echo "built sg-photos (64-bit)"
	@python3 src/taskmgr/gen-icon.py $(BUILD)/sg-taskmgr.ico
	@$(WINDRES64) -I src -I $(BUILD) src/taskmgr/taskmgr.rc -O coff -o $(BUILD)/sg-taskmgr-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-taskmgr64.exe $(TASKMGR_SRC) \
	    $(BUILD)/sg-taskmgr-res64.o $(TASKMGR_LIBS) && echo "built sg-taskmgr (64-bit)"
	@python3 src/paint/gen-icon.py $(BUILD)/sg-paint.ico
	@$(WINDRES64) -I src/paint -I $(BUILD) src/paint/paint.rc -O coff -o $(BUILD)/sg-paint-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-paint64.exe $(PAINT_SRC) $(BUILD)/sg-paint-res64.o $(PAINT_LIBS) \
	    && echo "built sg-paint (64-bit)"
	@python3 src/sticky/gen-icon.py $(BUILD)/sg-sticky.ico
	@$(WINDRES64) -I src/sticky -I $(BUILD) src/sticky/sg-sticky.rc -O coff -o $(BUILD)/sg-sticky-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-sticky64.exe src/sticky/sg-sticky.c $(BUILD)/sg-sticky-res64.o $(STICKY_LIBS) \
	    && echo "built sg-sticky (64-bit)"
	@python3 src/sg-snip-icon.py $(BUILD)/sg-snip.ico
	@$(WINDRES64) -I src -I $(BUILD) src/sg-snip.rc -O coff -o $(BUILD)/sg-snip-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-snip64.exe src/sg-snip.c $(BUILD)/sg-snip-res64.o $(SNIP_LIBS) \
	    && echo "built sg-snip (64-bit)"
	@python3 src/charmap/gen-names.py $(UNICODE_DATA) $(BUILD)/charmap-names.c
	@# Report a problem: testers' reports, a program run with debug logging (src/bugreport)
	@python3 src/bugreport/gen-icon.py $(BUILD)/sg-bugreport.ico
	@$(WINDRES64) -I src/bugreport -I $(BUILD) src/bugreport/sg-bugreport.rc -O coff -o $(BUILD)/sg-bugreport-res64.o
	@$(MINGW64) $(SG_CFLAGS) -o $(BUILD)/sg-bugreport64.exe src/bugreport/sg-bugreport.c \
	    $(BUILD)/sg-bugreport-res64.o -lcomdlg32 -lshlwapi -lversion $(LIBS) && echo "built sg-bugreport (64-bit)"
	@# Linux apps in Start: shortcuts for the .desktop apps, and their launcher (src/linuxapps)
	@$(MINGW64) $(SG_CFLAGS) -o $(BUILD)/sg-linuxapp64.exe src/linuxapps/sg-linuxapp.c -lshell32 -lole32 -luuid \
	    && echo "built sg-linuxapp (64-bit)"
	@# Linux Terminal (Administrator): a root shell after the user's password (src/rootterm)
	@python3 src/rootterm/gen-icon.py $(BUILD)/sg-rootterm.ico
	@$(WINDRES64) -I src/rootterm -I $(BUILD) src/rootterm/sg-rootterm.rc -O coff -o $(BUILD)/sg-rootterm-res64.o
	@$(MINGW64) $(SG_CFLAGS) -municode -mwindows -o $(BUILD)/sg-rootterm64.exe src/rootterm/sg-rootterm.c \
	    $(BUILD)/sg-rootterm-res64.o -luser32 -lshell32 -ladvapi32 && echo "built sg-rootterm (64-bit)"
	@python3 src/charmap/gen-icon.py $(BUILD)/sg-charmap.ico
	@$(WINDRES64) -I src/charmap -I $(BUILD) src/charmap/sg-charmap.rc -O coff -o $(BUILD)/sg-charmap-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-charmap64.exe src/charmap/main.c \
	    $(BUILD)/charmap-names.c $(BUILD)/sg-charmap-res64.o $(CHARMAP_LIBS) && echo "built sg-charmap (64-bit)"
	@python3 src/magnify/gen-icon.py $(BUILD)/sg-magnify.ico
	@$(WINDRES64) -I src/magnify -I $(BUILD) src/magnify/magnify.rc -O coff -o $(BUILD)/sg-magnify-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-magnify64.exe src/magnify/main.c \
	    $(BUILD)/sg-magnify-res64.o $(MAGNIFY_LIBS) && echo "built sg-magnify (64-bit)"
	@python3 src/osk/gen-icon.py $(BUILD)/sg-osk.ico
	@$(WINDRES64) -I src/osk -I $(BUILD) src/osk/osk.rc -O coff -o $(BUILD)/sg-osk-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-osk64.exe src/osk/main.c \
	    $(BUILD)/sg-osk-res64.o $(OSK_LIBS) && echo "built sg-osk (64-bit)"
	@python3 src/mmc/gen-icons.py $(BUILD)/mmc16.bmp $(BUILD)/mmc32.bmp $(BUILD)/sg-mmc.ico $(BUILD)/sg-eventvwr.ico $(BUILD)/sg-msinfo32.ico \
	    $(BUILD)/sg-resmon.ico $(BUILD)/sg-cleanmgr.ico
	@$(WINDRES64) -I src/mmc -I $(BUILD) src/mmc/sg-mmc.rc -O coff -o $(BUILD)/sg-mmc-res64.o
	@$(WINDRES64) -I src/mmc -I $(BUILD) src/mmc/sg-eventvwr.rc -O coff -o $(BUILD)/sg-eventvwr-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-mmc64.exe $(MMC_SRC) $(BUILD)/sg-mmc-res64.o $(MMC_LIBS) \
	    && echo "built sg-mmc (64-bit)"
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-eventvwr64.exe $(MMC_SRC) $(BUILD)/sg-eventvwr-res64.o $(MMC_LIBS) \
	    && echo "built sg-eventvwr (64-bit)"
	@$(WINDRES64) -I src/mmc -I $(BUILD) src/mmc/sg-msinfo32.rc -O coff -o $(BUILD)/sg-msinfo32-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-msinfo3264.exe $(MMC_SRC) $(BUILD)/sg-msinfo32-res64.o $(MMC_LIBS) \
	    && echo "built sg-msinfo32 (64-bit)"
	@$(WINDRES64) -I src/mmc -I $(BUILD) src/mmc/sg-cleanmgr.rc -O coff -o $(BUILD)/sg-cleanmgr-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-cleanmgr64.exe $(MMC_SRC) $(BUILD)/sg-cleanmgr-res64.o $(MMC_LIBS) \
	    && echo "built sg-cleanmgr (64-bit)"
	@$(WINDRES64) -I src/mmc -I $(BUILD) src/mmc/sg-resmon.rc -O coff -o $(BUILD)/sg-resmon-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-resmon64.exe $(MMC_SRC) $(BUILD)/sg-resmon-res64.o $(MMC_LIBS) \
	    && echo "built sg-resmon (64-bit)"
	@python3 src/terminal/gen-icon.py $(BUILD)/sg-terminal.ico
	@$(WINDRES64) -I src/terminal -I $(BUILD) src/terminal/terminal.rc -O coff -o $(BUILD)/sg-terminal-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-terminal64.exe $(TERMINAL_SRC) \
	    $(BUILD)/sg-terminal-res64.o $(TERMINAL_LIBS) && echo "built sg-terminal (64-bit)"
	@python3 src/clock/gen-icon.py $(BUILD)/sg-clock.ico
	@$(WINDRES64) -I src/clock -I $(BUILD) src/clock/clock.rc -O coff -o $(BUILD)/sg-clock-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-clock64.exe src/clock/main.c \
	    $(BUILD)/sg-clock-res64.o $(CLOCK_LIBS) && echo "built sg-clock (64-bit)"
	@python3 src/wordpad/gen-icon.py $(BUILD)/sg-wordpad.ico
	@$(WINDRES64) -I src/wordpad -I $(BUILD) src/wordpad/wordpad.rc -O coff -o $(BUILD)/sg-wordpad-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-wordpad64.exe $(WORDPAD_SRC) \
	    $(BUILD)/sg-wordpad-res64.o $(WORDPAD_LIBS) && echo "built sg-wordpad (64-bit)"
	@python3 src/pdf/gen-icon.py $(BUILD)/sg-pdf.ico
	@$(WINDRES64) -I src/pdf -I $(BUILD) src/pdf/pdf.rc -O coff -o $(BUILD)/sg-pdf-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-pdf64.exe $(PDF_SRC) \
	    $(BUILD)/sg-pdf-res64.o $(PDF_LIBS) && echo "built sg-pdf (64-bit)"
	@# The Compatibility tab of a program's Properties: a shell extension DLL
	@# (src/compat), loaded by the Properties dialog for .exe files and shortcuts.
	@$(WINDRES64) -I src/compat src/compat/sgcompat.rc -O coff -o $(BUILD)/sgcompat-res64.o
	@$(MINGW64) -O2 -municode -shared -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
	    -o $(BUILD)/sgcompat64.dll src/compat/sgcompat.c $(BUILD)/sgcompat-res64.o src/compat/sgcompat.def \
	    -lole32 -luuid -lshell32 -lshlwapi -lcomctl32 -ladvapi32 -luser32 && echo "built sgcompat (64-bit)"
	@# A Linux app's .desktop file shows the app's icon: the icon handler of
	@# its type (src/linuxapps/sglinuxicon.c, wine-sg 0767).
	@$(MINGW64) -O2 -municode -shared -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
	    -o $(BUILD)/sglinuxicon64.dll src/linuxapps/sglinuxicon.c src/linuxapps/sglinuxicon.def \
	    -lole32 -luuid -lshell32 -lshlwapi && echo "built sglinuxicon (64-bit)"
	@python3 src/fontview/gen-icon.py $(BUILD)/sg-fontview.ico $(BUILD)/sg-fonts.ico
	@$(WINDRES64) -I src/fontview -I $(BUILD) src/fontview/fontview.rc -O coff -o $(BUILD)/sg-fontview-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-fontview64.exe $(FONTVIEW_SRC) \
	    $(BUILD)/sg-fontview-res64.o $(FONTVIEW_LIBS) && echo "built sg-fontview (64-bit)"
	@python3 src/browser/gen-icon.py $(BUILD)/sg-browser.ico
	@$(WINDRES64) -I src/browser -I $(BUILD) src/browser/browser.rc -O coff -o $(BUILD)/sg-browser-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-browser64.exe $(BROWSER_SRC) \
	    $(BUILD)/sg-browser-res64.o $(BROWSER_LIBS) && echo "built sg-browser (64-bit)"
	@python3 src/store/gen-icon.py $(BUILD)/sg-store.ico
	@$(WINDRES64) -I src/store -I $(BUILD) src/store/store.rc -O coff -o $(BUILD)/sg-store-res64.o
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -Isrc/browser -Isrc/store -Isrc/zip -o $(BUILD)/sg-store64.exe \
	    $(STORE_SRC) $(BUILD)/sg-store-res64.o $(STORE_LIBS) && echo "built sg-store (64-bit)"
	@$(MAKE) --no-print-directory office
	@for p in $(CONSOLE_TOOLS); do \
	    $(MINGW64) $(SG_CON_CFLAGS) -o $(BUILD)/$$p'64'.exe src/$$p.c $(LIBS) && echo "built $$p (64-bit, console)"; \
	    $(MINGW32) $(SG_CON_CFLAGS) -o $(BUILD)/$$p'32'.exe src/$$p.c $(LIBS) && echo "built $$p (32-bit, console)"; \
	done

# SG Office (package sg-office, office/): its three programs (Windows
# programs, each with its icons drawn by office/gen-icons.py) that start SG
# Office's editors, the native program of package sg-office-editors.
OFFICE_KINDS = documents spreadsheets presentations
.PHONY: office test-office
office:
	@mkdir -p $(BUILD)/office
	@python3 office/gen-icons.py $(BUILD)/office/icons
	@for n in $(OFFICE_KINDS); do \
	    t="SG Office $$(echo $$n | sed 's/^./\U&/')"; \
	    $(WINDRES64) -I $(BUILD)/office/icons -DSG_ICON="\\\"sg-$$n.ico\\\"" -DSG_FILE_ICON="\\\"sg-$$n-file.ico\\\"" \
	        -DSG_TITLE_A="\\\"$$t\\\"" office/launcher/launcher.rc -O coff -o $(BUILD)/office/$$n-res64.o && \
	    $(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -DSG_NATIVE_KIND="L\"$$n\"" -DSG_TITLE="L\"$$t\"" \
	        -o $(BUILD)/sg-$${n}64.exe office/launcher/launcher.c $(BUILD)/office/$$n-res64.o -lshell32 -luser32 \
	        && echo "built sg-$$n (64-bit)"; done

# SG Office's gates: the registrations, the programs, Start and Default apps,
# the Store's entry (test/office-check.sh); the programs starting the editors
# under Wine (test/office-native-check.sh).
test-office: office
	@sh test/office-check.sh
	@sh test/office-native-check.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# SG Office's programs starting SG Office's own editors, under Wine
# (test/office-native-check.sh), and the broken builds it must fail.
.PHONY: test-office-native-mutants test-office-mutants
test-office-native-mutants: office
	@sh test/office-native-mutants.sh

# Both of SG Office's gates against the broken builds and sources they must fail.
test-office-mutants: office
	@sh test/office-check-mutants.sh
	@sh test/office-native-mutants.sh

# Our apps follow the app mode, live (needs a wine-sg with dark title bars, 0162:
# SG_WINE=<wine> SG_WINESERVER=<wineserver> for a build tree).
test-appmode: build
	@sh test/appmode-check.sh

# No user-visible "Windows" in our own text (Microsoft's trademark; we must
# never present ourselves as Windows): string literals, .reg data, manifests.
# Technical identifiers pass; anything else needs tools/trademark-allow.txt.
# Every test that runs Wine sources test/scratch-home.sh first (a HOME of its
# own: a prefix links its Desktop, Documents... into HOME).
lint:
	@for f in $$(grep -l WINEPREFIX test/*.sh); do \
	    sed -n 2p "$$f" | grep -q '^\. "$$(dirname "$$0")/scratch-home.sh"$$' || \
	    { echo "$$f: line 2 must be: . \"\$$(dirname \"\$$0\")/scratch-home.sh\""; exit 1; }; done
	@python3 tools/trademark-check.py --allow tools/trademark-allow.txt src defaults theme admin office

# The gate renders each panel headlessly and checks it docks and paints.
test: build
	@sh test/render-check.sh
	@sh test/start-check.sh
	@sh test/taskbar-pins-check.sh
	@sh test/round-check.sh
	@sh test/mstsc-check.sh
	@sh test/rootterm-check.sh
	@sh test/xterm-copypaste-check.sh; rc=$$?; [ $$rc = 0 ] || [ $$rc = 77 ]
	@sh test/linux-terminal-tabs-check.sh; rc=$$?; [ $$rc = 0 ] || [ $$rc = 77 ]
	@sh test/bugreport-check.sh
	@sh test/settings-teardown-check.sh
	@sh test/recovery-check.sh
	@sh test/admind-check.sh
	@sh test/store-catalog-check.sh; rc=$$?; [ $$rc = 0 ] || [ $$rc = 77 ]
	@sh test/updates-page-check.sh; rc=$$?; [ $$rc = 0 ] || [ $$rc = 77 ]
	@sh test/dictate-linux-check.sh; rc=$$?; [ $$rc = 0 ] || [ $$rc = 77 ]
	@sh test/timezone-auto-test.sh
	@sh test/control-check.sh
	@sh test/settings-check.sh
	@sh test/defender-page-check.sh; rc=$$?; [ $$rc = 0 ] || [ $$rc = 77 ]
	@sh test/optionalfeatures-check.sh; rc=$$?; [ $$rc = 0 ] || [ $$rc = 77 ]
	@sh test/power-buttons-check.sh; rc=$$?; [ $$rc = 0 ] || [ $$rc = 77 ]
	@sh test/envreload-check.sh; rc=$$?; [ $$rc = 0 ] || [ $$rc = 77 ]
	@sh test/control-icon-check.sh; rc=$$?; [ $$rc = 0 ] || [ $$rc = 77 ]
	@sh test/defender-notice-check.sh; rc=$$?; [ $$rc = 0 ] || [ $$rc = 77 ]
	@sh test/terminal-check.sh
	@sh test/terminal-handoff-check.sh; rc=$$?; [ $$rc = 0 ] || [ $$rc = 77 ]
	@sh test/terminal-panes-check.sh
	@sh test/terminal-settings-check.sh
	@sh test/clock-check.sh
	@sh test/gpresult-check.sh
	@sh test/net-ui-check.sh
	@sh test/office-check.sh
	@sh test/office-native-check.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# The taskbar's battery icon (sg-battery). Needs sudo -n (a mount namespace for fake batteries).
.PHONY: test-battery
test-battery: build
	@sh test/battery-check.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# The taskbar's volume icon (sg-volume), against a stand-in sg-settingsctl.
# The shell's icon font (caption buttons of Windows 10/11-era programs).
.PHONY: test-icons-font
test-icons-font:
	@sh test/icons-font-check.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

.PHONY: test-volume
test-volume: build
	@sh test/volume-check.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc

# The notification centre (sg-notify): its icon and panel, under Xvfb with the shell.
.PHONY: test-notify
test-notify: build
	@sh test/notify-check.sh; rc=$$?; [ $$rc -eq 77 ] && exit 0 || exit $$rc
	@sh test/dictate-check.sh
	@sh test/zip-check.sh
	@sh test/media-check.sh
	@sh test/calc-check.sh
	@sh test/photos-check.sh
	@sh test/taskmgr-check.sh
	@sh test/paint-check.sh
	@sh test/sticky-check.sh
	@sh test/snip-check.sh
	@sh test/charmap-check.sh
	@sh test/envvars-check.sh
	@sh test/vcruntime-check.sh
	@sh test/cplfile-check.sh
	@sh test/runas-check.sh
	@sh test/compat-check.sh
	@sh test/linuxicon-check.sh
	@sh test/wordpad-check.sh
	@sh test/fontview-check.sh
	@sh test/magnify-check.sh
	@sh test/osk-check.sh
	@sh test/osk-layout-check.sh
	@sh test/services-check.sh
	@sh test/eventvwr-check.sh
	@sh test/devmgmt-check.sh
	@sh test/diskmgmt-check.sh
	@sh test/compmgmt-check.sh
	@sh test/msinfo-check.sh
	@sh test/cleanmgr-check.sh
	@sh test/resmon-check.sh
	@sh test/pdf-check.sh
	@sh test/pdf-editor-check.sh
	@sh test/browser-check.sh
	@sh test/store-check.sh

# SG Store: the catalogue engine (install/verify/update) against a mock winget
# source and a stand-in installer, and the window. test/store-check.sh.
.PHONY: test-store
test-store: build
	@sh test/store-check.sh

# Linux apps in Start: .desktop files -> Programs\Linux apps shortcuts. test/linuxapps-check.sh.
.PHONY: test-linuxapps
test-linuxapps: build
	@sh test/linuxapps-check.sh

clean:
	rm -rf $(BUILD)
	rm -rf debian/sg-shell debian/.debhelper debian/*.substvars debian/files debian/debhelper-build-stamp

deb:
	dpkg-buildpackage -us -uc -b
