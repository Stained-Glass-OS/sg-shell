#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for defaults/44-sg-qt-rendering.reg: Qt 6 programs draw Qt Quick with Direct3D 11,
# as on Windows. sg-shell 235-243 set QSG_RHI_BACKEND=opengl machine-wide (no shared
# Direct3D 11 textures before wine-sg 10.0-807); the file now takes it away again.
#  1. on a machine with the old value, importing the file removes it: a program started
#     afterwards has no QSG_RHI_BACKEND, and Session Manager\Environment has none
#  2. a machine that never had it is left without it (the import succeeds)
# Mutant QT_RENDER_KEPT: the old defaults (the value set) must fail 1.
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

# 1. a machine with sg-shell 235-243's value
printf 'Windows Registry Editor Version 5.00\n\n[HKEY_LOCAL_MACHINE\\System\\CurrentControlSet\\Control\\Session Manager\\Environment]\n"QSG_RHI_BACKEND"="opengl"\n' > "$T/old.reg"
out=$(check "$T/old.reg")
[ "$(v "$out" machine)" = opengl ] || fail "setup: the old value did not land: $out"
out=$(check "$REG")
[ "$(v "$out" seen)" = "%QSG_RHI_BACKEND%" ] && pass "after the import a program started has no QSG_RHI_BACKEND (Qt's own Direct3D 11)" || fail "seen: $out"
[ -z "$(v "$out" machine)" ] && [ -z "$(v "$out" user)" ] \
    && pass "...the machine value is gone (Session Manager\\Environment)" || fail "where: $out"
# 2. imported again (a machine that never had it): still none, no error
out=$(check "$REG")
[ -z "$(v "$out" machine)" ] && pass "a machine without the value stays without it" || fail "again: $out"

# the mutant: the old defaults (the value set)
out=$(check "$T/old.reg")
[ "$(v "$out" seen)" = opengl ] && pass "MUTANT QT_RENDER_KEPT: the override stays (gate catches it)" || fail "mutant: $out"
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
