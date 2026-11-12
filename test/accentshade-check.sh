#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate: sg-mode.h's accent and its shades. The shell's programs draw what was
# drawn in the default accent's shades (Stained Glass purple: a selection's
# tint, a hover's, a dark mode's lighter purple) with sg_accent_shade(): the
# same shade of the accent chosen now -- the purple itself becomes the
# accent, a pale tint stays pale, a deep shade deep, other hues and greys are
# left alone. sg_accent() reads a new accent at once (the key is watched): the
# repaint right after "ImmersiveColorSet" must not draw the old one.
#
# Mutants (compiled into the probe): SG_MUTANT_SHADE_FIXED, SG_MUTANT_ACCENT_THROTTLED.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE="${SG_WINE:-${SG_WINE_DIR:-/opt/wine-sg}/bin/wine}"
export WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "SKIP: mingw-w64 missing"; exit 77; }
[ -x "$WINE" ] || { echo "SKIP: no wine-sg at $WINE"; exit 77; }
T=$(mktemp -d /var/tmp/sg-accentshade.XXXXXX)
cleanup() { WINEPREFIX="$T/pfx" "$WINESERVER" -k 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
for m in "" SHADE_FIXED ACCENT_THROTTLED; do
    x86_64-w64-mingw32-gcc -O2 -municode ${m:+-DSG_MUTANT_$m} -o "$T/probe${m:+-$m}.exe" "$HERE/test/accentshade-probe.c" -ladvapi32 -lm ||
        { fail "probe ${m:-real} did not build"; exit 1; }
done
export WINEPREFIX="$T/pfx" WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all DISPLAY=
"$WINE" wineboot --init >/dev/null 2>&1; "$WINESERVER" -w
"$WINE" "$T/probe.exe" 2>/dev/null | tr -d '\r' > "$T/real.txt"
cat "$T/real.txt"
grep -q '^RESULT: PASS' "$T/real.txt" || RC=1
for m in SHADE_FIXED ACCENT_THROTTLED; do
    "$WINE" "$T/probe-$m.exe" 2>/dev/null | tr -d '\r' > "$T/$m.txt"
    grep -q '^RESULT: FAIL' "$T/$m.txt" && pass "MUTANT $m fails it (gate catches it)" || fail "MUTANT $m not detected"
done
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
