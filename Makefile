# sg-shell — Windows 10-style panels for Stained Glass OS. See CLAUDE.md.
MINGW64 ?= x86_64-w64-mingw32-gcc
MINGW32 ?= i686-w64-mingw32-gcc
CFLAGS  ?= -O2 -municode -mwindows -Wall -Wextra
LIBS     = -lshell32 -lgdi32 -luser32
BUILD    = build

PANELS = sg-taskbar sg-start

.PHONY: all build test clean
all: build

build:
	@mkdir -p $(BUILD)
	@command -v $(MINGW64) >/dev/null 2>&1 || { echo "SKIP: $(MINGW64) not installed"; exit 0; }
	@for p in $(PANELS); do \
	    $(MINGW64) $(CFLAGS) -o $(BUILD)/$$p'64'.exe src/$$p.c $(LIBS) && echo "built $$p (64-bit)"; \
	    $(MINGW32) $(CFLAGS) -o $(BUILD)/$$p'32'.exe src/$$p.c $(LIBS) && echo "built $$p (32-bit)"; \
	done

# The gate renders each panel headlessly and checks it docks and paints.
test: build
	@sh test/render-check.sh
	@sh test/start-check.sh

clean:
	rm -rf $(BUILD)
