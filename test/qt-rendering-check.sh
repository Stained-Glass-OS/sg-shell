#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for defaults/44-sg-qt-rendering.reg: Qt 6 programs draw Qt Quick with OpenGL
# (QSG_RHI_BACKEND=opengl), a machine environment variable, so Qt WebEngine's frames need
# no shared Direct3D 11 textures (GOG GALAXY's window was blank white without it).
#  1. imported into a prefix, a program started afterwards has QSG_RHI_BACKEND=opengl
#  2. ...as a machine variable (Session Manager\Environment), not one person's
# Mutant QT_RENDER_UNSET: the same defaults without the value must fail 1.
#   SG_WINE_DIR=/opt/wine-sg sh test/qt-rendering-check.sh
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WD="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="$WD/bin/wine"; WINESERVER="$WD/bin/wineserver"
[ -x "$WINE" ] || { echo "SKIP: no wine at $WINE"; exit 77; }
T=$(mktemp -d /var/tmp/sg-qt-rendering-check.XXXXXX)
unset DISPLAY WAYLAND_DISPLAY QSG_RHI_BACKEND
export HOME="$T/home" WINEPREFIX="$T/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" WINESERVER
mkdir -p "$HOME"
cleanup() { "$WINESERVER" -k 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
w() { timeout -s KILL 120 "$WINE" "$@" 2>/dev/null | tr -d '\r'; }
REG="$HERE/defaults/44-sg-qt-rendering.reg"
[ -f "$REG" ] || { echo "FAIL  no $REG"; exit 1; }
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1
"$WINESERVER" -w

check() { # $1 = the .reg to import; prints the value a new program sees, then the machine key's
    cp "$1" "$T/in.reg"
    w regedit /S 'Z:'"$T/in.reg"
    "$WINESERVER" -w
    echo "seen $(w cmd /c echo %QSG_RHI_BACKEND%)"
    echo "machine $(w reg query 'HKLM\System\CurrentControlSet\Control\Session Manager\Environment' /v QSG_RHI_BACKEND | sed -n 's/.*REG_SZ *//p')"
    echo "user $(w reg query 'HKCU\Environment' /v QSG_RHI_BACKEND | sed -n 's/.*REG_SZ *//p')"
}
v() { printf '%s\n' "$1" | sed -n "s/^$2 //p" | head -1; }

out=$(check "$REG")
[ "$(v "$out" seen)" = opengl ] && pass "a program started after the import has QSG_RHI_BACKEND=opengl" || fail "seen: $out"
[ "$(v "$out" machine)" = opengl ] && [ -z "$(v "$out" user)" ] \
    && pass "...a machine variable (Session Manager\\Environment), no one person's" || fail "where: $out"

# the mutant: the same defaults without the value, in a fresh prefix
"$WINESERVER" -k 2>/dev/null; "$WINESERVER" -w; rm -rf "$WINEPREFIX"
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1
"$WINESERVER" -w
grep -v QSG_RHI_BACKEND "$REG" > "$T/mutant.reg"
out=$(check "$T/mutant.reg")
[ "$(v "$out" seen)" != opengl ] && pass "MUTANT QT_RENDER_UNSET: no OpenGL setting (gate catches it)" || fail "mutant: $out"
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
