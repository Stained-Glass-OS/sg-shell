#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate: programs that draw symbols by the name "Segoe UI Symbol" -- Windows' symbol font, not ours to
# ship -- get them from Symbola (fonts-symbola, in the image), as theme/52-sg-fonts.reg names it for
# GDI, GDI+ and DirectWrite. WinDirStat 2.9 paints its toolbar's Resume, Refresh and Search icons as
# GDI+ paths of U+25B6, U+21BB and U+2315 in that family: with no font of that name GDI+ found no
# family, nothing was painted, and the icons were black squares (grey when disabled).
#
# Mutant: SG_MUTANT_NO_SYMBOL_FONT=1 imports the .reg without the "Segoe UI Symbol" lines.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE="${SG_WINE:-${SG_WINE_DIR:-/opt/wine-sg}/bin/wine}"
export WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
REG="$HERE/theme/52-sg-fonts.reg"
n=$(grep -c '^"Segoe UI Symbol"="Symbola"' "$REG")
[ "$n" = 2 ] && pass "52-sg-fonts.reg names Symbola for \"Segoe UI Symbol\" (FontSubstitutes and Replacements)" ||
    fail "\"Segoe UI Symbol\" mappings in 52-sg-fonts.reg: $n of 2"
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "SKIP: mingw-w64 missing"; exit 77; }
[ -x "$WINE" ] || { echo "SKIP: no wine-sg at $WINE"; exit 77; }
fc-list 2>/dev/null | grep -q ': Symbola:' || { echo "SKIP: Symbola (fonts-symbola) is not installed here"; exit 77; }
T=$(mktemp -d /var/tmp/sg-symbolfont.XXXXXX)
export WINEPREFIX="$T/pfx" WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all DISPLAY=
cleanup() { "$WINESERVER" -k 2>/dev/null; sg_kill_prefix "$WINEPREFIX"; rm -rf "$T"; }
trap cleanup EXIT INT TERM
x86_64-w64-mingw32-gcc -O2 -o "$T/probe.exe" "$HERE/test/symbolfont-probe.c" -lgdiplus -lgdi32 ||
    { fail "probe did not build"; exit 1; }
"$WINE" wineboot --init >/dev/null 2>&1; "$WINESERVER" -w
if [ "${SG_MUTANT_NO_SYMBOL_FONT:-}" = 1 ]; then
    grep -v '^"Segoe UI Symbol"=' "$REG" > "$T/fonts.reg"
else
    cp "$REG" "$T/fonts.reg"
fi
"$WINE" regedit /s "$("$WINE" winepath -w "$T/fonts.reg" 2>/dev/null)" >/dev/null 2>&1; "$WINESERVER" -w
timeout 120 "$WINE" "$T/probe.exe" 2>/dev/null | tr -d '\r' > "$T/out.txt"
cat "$T/out.txt"
grep -q '^RESULT: PASS' "$T/out.txt" || RC=1
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
