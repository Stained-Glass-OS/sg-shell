# sg-shell — Windows 10-style panels for Stained Glass OS. See CLAUDE.md.
MINGW64 ?= x86_64-w64-mingw32-gcc
MINGW32 ?= i686-w64-mingw32-gcc
# Not CFLAGS: dpkg-buildpackage exports its own CFLAGS (no -municode),
# which would drop the Unicode entry point and break the link.
SG_CFLAGS := -O2 -municode -mwindows -Wall -Wextra
# Console subsystem: gpresult is a command-line tool whose report must reach the
# console/pipe it is run from, so it links -mconsole, not -mwindows.
SG_CON_CFLAGS := -O2 -municode -mconsole -Wall -Wextra
LIBS     = -lshell32 -lgdi32 -luser32
BUILD    = build

PANELS = sg-taskbar sg-start sg-mstsc
# The Control Panel is several files (src/control/) and needs more of Windows.
CONTROL_SRC  = $(wildcard src/control/*.c)
CONTROL_LIBS = -lcomctl32 -lshell32 -lgdi32 -luser32 -ladvapi32 -lmsimg32 -liphlpapi -lws2_32 \
               -lole32 -luuid -lwindowscodecs -lcomdlg32 -lshlwapi
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

clean:
	rm -rf $(BUILD)
	rm -rf debian/sg-shell debian/.debhelper debian/*.substvars debian/files debian/debhelper-build-stamp

deb:
	dpkg-buildpackage -us -uc -b
