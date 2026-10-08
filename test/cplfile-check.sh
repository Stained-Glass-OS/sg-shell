#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# A Control Panel item opens with "open" (defaults/42-sg-cplfile.reg): Run's
# "sysdm.cpl" and `start sysdm.cpl` ask for "open" by name, a .cpl had only
# cplopen, and they said "There is no program configured to open this type of
# file" (QA 2026-10-02). With a stand-in Control Panel (App Paths control.exe,
# wine-sg's control-probe), `start sysdm.cpl` must reach it asking for the
# System page. Needs wine-sg with sysdm.cpl (10.0-278, SG_WINE_DIR) and its
# test/control-probe.c (SG_CONTROL_PROBE_C); skips (77) without them.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
REG="${SG_CPLFILE_REG:-$HERE/defaults/42-sg-cplfile.reg}"
PROBE="${SG_CONTROL_PROBE_C:-$HERE/../wine-sg/test/control-probe.c}"
RC=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$WINE_DIR/lib/wine/x86_64-windows/sysdm.cpl" ] || { echo "SKIP: no wine-sg with sysdm.cpl at $WINE_DIR"; exit 77; }
[ -f "$PROBE" ] || { echo "SKIP: no control-probe.c at $PROBE"; exit 77; }
command -v xvfb-run >/dev/null || { echo "SKIP: needs xvfb-run"; exit 77; }
T=$(mktemp -d /var/tmp/sg-cplfile-check.XXXXXX)
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" PATH="$WINE_DIR/bin:$PATH"
trap 'wineserver -k 2>/dev/null; rm -rf "$T"' EXIT INT TERM
wine wineboot -i >/dev/null 2>&1; wineserver -w
x86_64-w64-mingw32-gcc -municode -O2 -o "$WINEPREFIX/drive_c/control-probe.exe" "$PROBE" || { fail "probe did not build"; exit 1; }
[ -f "$REG" ] && cp "$REG" "$WINEPREFIX/drive_c/c.reg" && wine reg import 'C:\c.reg' >/dev/null 2>&1
wine reg add 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\control.exe' /ve /d 'C:\control-probe.exe' /f >/dev/null 2>&1
wineserver -w
timeout 120 xvfb-run -a sh -c 'wine start sysdm.cpl >/dev/null 2>&1; sleep 4'
# System Properties: "control sysdm.cpl" (wine-sg 10.0-278; before, /name
# Microsoft.System, which is Settings > About now, as in Windows 10)
grep -q 'cmdline=.*sysdm\.cpl' "$WINEPREFIX/drive_c/standin.log" 2>/dev/null \
    && pass "start sysdm.cpl (Run's sysdm.cpl) opens the Control Panel's System Properties" \
    || fail "start sysdm.cpl: $(tr -d '\r' < "$WINEPREFIX/drive_c/standin.log" 2>/dev/null)"
[ "$RC" = 0 ] && echo "cplfile-check: PASS" || echo "cplfile-check: FAIL"
exit "$RC"
