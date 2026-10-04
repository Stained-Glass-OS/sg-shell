#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Control Panel's icon (David 2026-10-04: it "looks like it needs some anti
# aliasing or to be higher quality" -- Start drew a gear of one-pixel lines,
# and the program had no icon): sg-control64.exe carries its own, smooth at
# 16, 32 and 48 px (partly transparent edge pixels). SG_CONTROL_EXE tests
# another build (mutant: one linked without src/control/control.rc).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_CONTROL_EXE:-$HERE/build/sg-control64.exe}"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
command -v "$MINGW" >/dev/null || { echo "SKIP: $MINGW missing"; exit 77; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
T=$(mktemp -d /var/tmp/sg-control-icon.XXXXXX)
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' DISPLAY=
export PATH="$WINE_DIR/bin:$PATH"
trap 'wineserver -k 2>/dev/null; rm -rf "$T" "$SG_GATE_HOME"' EXIT INT TERM
"$MINGW" -O2 -municode -o "$T/probe.exe" "$HERE/test/control-icon-probe.c" -luser32 -lgdi32 || { fail "probe did not build"; exit 1; }
wine wineboot -i >/dev/null 2>&1
cp "$EXE" "$WINEPREFIX/drive_c/sg-control64.exe"
out=$(wine "$T/probe.exe" 'C:\sg-control64.exe' 2>/dev/null | tr -d '\r')
for px in 16 32 48; do
    line=$(printf '%s\n' "$out" | grep "^ICONSIZE $px ")
    # shellcheck disable=SC2086  # the probe's fields
    set -- $line
    [ "${3:-0}" = 1 ] && [ "${4:-0}" -ge $((px / 2)) ] && pass "a ${px} px icon of its own, with a smooth edge ($4 partly transparent pixels)" \
        || fail "${px} px: '${line:-none}'"
done
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
