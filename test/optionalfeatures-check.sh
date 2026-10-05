#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Settings > Apps & features shows the optional features programs ask for
# (ms-settings:optionalfeatures, Windows' "Turn Windows features on or off"):
# the .NET Framework 3.5 is On when the .NET Framework is registered as
# installed (Wine Mono's NDP\v3.5 Install=1), Off when it is not -- the same
# answer dism.exe, fondue.exe and WMI give (wine-sg 0827-0829). David
# 2026-10-05: Meedio wanted .NET 3.5 and could not tell it was there.
# Read back from Settings' own dump (SG_SETTINGS_DUMP).
#
#   test/optionalfeatures-check.sh   (mutant SG_MUTANT_NO_OPTIONAL_FEATURES:
#   SG_SETTINGS_EXE=<a build with it> fails)
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_SETTINGS_EXE:-$HERE/build/sg-settings64.exe}"
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
command -v Xvfb >/dev/null || { echo "SKIP: Xvfb missing"; exit 77; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
WS="$WINE_DIR/bin/wineserver"; [ -x "$WS" ] || WS="$WINE_DIR/server/wineserver"
T=$(mktemp -d /var/tmp/sg-optfeat.XXXXXX)
cleanup() { WINEPREFIX="$T/pfx" "$WS" -k 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/display")" WINEPREFIX="$T/pfx" WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
W="$WINE_DIR/bin/wine"
"$W" wineboot --init >/dev/null 2>&1; "$WS" -w
cp "$EXE" "$T/pfx/drive_c/sg-settings64.exe"
DUMP="$T/settings.dump"
export SG_SETTINGS_DUMP="$("$W" winepath -w "$DUMP" | tr -d '\r')"
NDP='HKLM\Software\Microsoft\NET Framework Setup\NDP'
show() {   # open ms-settings:optionalfeatures, wait for its dump, close it
    rm -f "$DUMP"
    "$W" 'C:\sg-settings64.exe' ms-settings:optionalfeatures >/dev/null 2>&1 &
    i=0; while ! grep -q '^page ' "$DUMP" 2>/dev/null && [ $i -lt 120 ]; do sleep 0.25; i=$((i + 1)); done
    sleep 1; tr -d '\r' < "$DUMP" 2>/dev/null | sed 's/^\xEF\xBB\xBF//' > "$T/d"
    "$WS" -k 2>/dev/null; "$WS" -w 2>/dev/null
}
"$W" reg add "$NDP\\v3.5" /v Install /t REG_DWORD /d 1 /f >/dev/null 2>&1
"$W" reg delete "$NDP\\v4\\Full" /f >/dev/null 2>&1
show
grep -qx 'page Apps & features' "$T/d" && pass "ms-settings:optionalfeatures opens Apps & features" || fail "page: $(grep '^page' "$T/d")"
grep -qx 'text Optional features' "$T/d" && pass "the page has Optional features" || fail "no Optional features heading"
grep -qx 'text .NET Framework 3.5 (includes .NET 2.0 and 3.0): On' "$T/d" \
    && pass ".NET Framework 3.5 (includes .NET 2.0 and 3.0): On, when NDP v3.5 says installed" || fail "3.5 line: $(grep 'Framework 3.5' "$T/d")"
grep -q '^text .NET Framework 4.8 Advanced Services: Off' "$T/d" \
    && pass "and 4.8 Advanced Services Off without NDP v4 Full" || fail "4.8 line: $(grep 'Framework 4.8' "$T/d")"
"$W" reg delete "$NDP\\v3.5" /f >/dev/null 2>&1
show
grep -q '^text .NET Framework 3.5 (includes .NET 2.0 and 3.0): Off' "$T/d" \
    && pass "without the .NET Framework registered: 3.5 is Off" || fail "3.5 without NDP: $(grep 'Framework 3.5' "$T/d")"
exit $RC
