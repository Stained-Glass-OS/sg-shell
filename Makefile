# sg-shell — Windows 10-style panels for Stained Glass OS. See CLAUDE.md.
MINGW64 ?= x86_64-w64-mingw32-gcc
MINGW32 ?= i686-w64-mingw32-gcc
# Not CFLAGS: dpkg-buildpackage exports its own CFLAGS (no -municode),
# which would drop the Unicode entry point and break the link.
SG_CFLAGS := -O2 -municode -mwindows -Wall -Wextra
# Console subsystem: gpresult is a command-line tool whose report must reach the
# console/pipe it is run from, so it links -mconsole, not -mwindows.
SG_CON_CFLAGS := -O2 -municode -mconsole -Wall -Wextra
LIBS     = -lshell32 -lgdi32 -luser32 -lole32 -luuid
BUILD    = build

PANELS = sg-taskbar sg-start sg-mstsc
# The Control Panel is several files (src/control/) and needs more of Windows.
CONTROL_SRC  = $(wildcard src/control/*.c)
CONTROL_LIBS = -lcomctl32 -lshell32 -lgdi32 -luser32 -ladvapi32 -lmsimg32 -liphlpapi -lws2_32 \
               -lole32 -luuid -lwindowscodecs -lcomdlg32 -lshlwapi
# The network programs: sg-netclient.h and a common-controls 6 manifest.
NET_PANELS = sg-ncpa sg-netflyout
WINDRES64 ?= x86_64-w64-mingw32-windres
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
# Console tools (subsystem console), built the same way but without -mwindows.
CONSOLE_TOOLS = sg-gpresult

.PHONY: all build test clean
all: build

build:
	@mkdir -p $(BUILD)
	@command -v $(MINGW64) >/dev/null 2>&1 || { echo "SKIP: $(MINGW64) not installed"; exit 0; }
	@for p in $(PANELS); do \
	    $(MINGW64) $(SG_CFLAGS) -o $(BUILD)/$$p'64'.exe src/$$p.c $(LIBS) && echo "built $$p (64-bit)"; \
	    $(MINGW32) $(SG_CFLAGS) -o $(BUILD)/$$p'32'.exe src/$$p.c $(LIBS) && echo "built $$p (32-bit)"; \
	done
	@$(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-control64.exe $(CONTROL_SRC) $(CONTROL_LIBS) && echo "built sg-control (64-bit)"
	@$(MINGW32) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/sg-control32.exe $(CONTROL_SRC) $(CONTROL_LIBS) && echo "built sg-control (32-bit)"
	@$(WINDRES64) -I src src/sg-net.rc -O coff -o $(BUILD)/sg-net-res64.o
	@for p in $(NET_PANELS); do \
	    $(MINGW64) $(SG_CFLAGS) -Wno-missing-field-initializers -o $(BUILD)/$$p'64'.exe src/$$p.c $(BUILD)/sg-net-res64.o $(NET_LIBS) \
	        && echo "built $$p (64-bit)"; \
	done
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
	@for p in $(CONSOLE_TOOLS); do \
	    $(MINGW64) $(SG_CON_CFLAGS) -o $(BUILD)/$$p'64'.exe src/$$p.c $(LIBS) && echo "built $$p (64-bit, console)"; \
	    $(MINGW32) $(SG_CON_CFLAGS) -o $(BUILD)/$$p'32'.exe src/$$p.c $(LIBS) && echo "built $$p (32-bit, console)"; \
	done

# The gate renders each panel headlessly and checks it docks and paints.
test: build
	@sh test/render-check.sh
	@sh test/start-check.sh
	@sh test/mstsc-check.sh
	@sh test/admind-check.sh
	@sh test/control-check.sh
	@sh test/gpresult-check.sh
	@sh test/net-ui-check.sh
	@sh test/dictate-check.sh
	@sh test/zip-check.sh
	@sh test/media-check.sh
	@sh test/calc-check.sh

clean:
	rm -rf $(BUILD)
	rm -rf debian/sg-shell debian/.debhelper debian/*.substvars debian/files debian/debhelper-build-stamp

deb:
	dpkg-buildpackage -us -uc -b
