#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Settings > Display > Scale is the selected display's own scale (wine-sg 9971),
# as on Windows: each display keeps its own (PerMonitorSettings), read and set
# with DisplayConfigGetDeviceInfo / DisplayConfigSetDeviceInfo (Windows' types
# -3 and -4). One scale (LogPixels) was every display's: a 1080p display beside
# a 1440p main one was 125% too, and unplugging the main one turned every
# window to 100% (GitHub report, ledger i0621). Under Xvfb (one display):
#   1. the session's scale 100% (LogPixels 96, automatic), the display's own
#      set to 150% (+2 from its recommended 100%): Settings shows 150% selected
#   2. LogPixels was not touched by the display's own scale
#
#   SG_WINE_DIR=<wine-sg root with 9971> sh test/dispscale-check.sh
#   Mutant (sg-control): SG_MUTANT_SCALE_ONE_FOR_ALL fails 1.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="$WINE_DIR/bin/wine"
export WINESERVER="$WINE_DIR/bin/wineserver"
EXE="${SG_SETTINGS_EXE:-$HERE/build/sg-settings64.exe}"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in Xvfb "$MINGW"; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or sg-settings missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-dispscale.XXXXXX)
# shellcheck disable=SC2317
cleanup() { "$WINESERVER" -k 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; [ -n "${KEEP:-}" ] && echo "kept $T" || rm -rf "$T"; }
trap cleanup EXIT INT TERM
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" XDG_CONFIG_HOME="$T/config"
mkdir -p "$T/run" "$T/config"; export XDG_RUNTIME_DIR="$T/run" SG_LOCK_CONTROL=/nonexistent
"$MINGW" -O2 -o "$T/probe.exe" "$HERE/test/dispscale-probe.c" -lshcore -luser32 || { fail "the probe did not build"; exit 1; }
cp "$EXE" "$T/sg-settings64.exe"
Xvfb -displayfd 3 -screen 0 1920x1080x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
DISPLAY=":$(cat "$T/display")"; export DISPLAY
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w
"$WINE" reg add 'HKCU\Control Panel\Desktop' /v LogPixels /t REG_DWORD /d 96 /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Software\Stained Glass\Display' /v AutoLogPixels /t REG_DWORD /d 96 /f >/dev/null 2>&1
"$WINESERVER" -w
cd "$T" || exit 1
S=$("$WINE" probe.exe set 0 2 2>/dev/null | tr -d '\r')
echo "$S" | grep -q '^SET 0 0$' || { echo "SKIP: this Wine has no display scale of each display (wine-sg 9971): $(echo $S)"; exit 77; }

export SG_SETTINGS_DUMP="$("$WINE" winepath -w "$T/sdump" 2>/dev/null | tr -d '\r')"
"$WINE" "$T/sg-settings64.exe" ms-settings:display >/dev/null 2>&1 &
i=0; while [ $i -lt 40 ] && ! grep -q '^page Display' "$T/sdump" 2>/dev/null; do sleep 0.5; i=$((i + 1)); done
sleep 1
tr -d '\r' < "$T/sdump" 2>/dev/null | grep -q 'ComboBox.*: 150%$' \
    && pass "Settings > Display > Scale: the display's own 150%, selected" \
    || fail "Settings' scale: want the display's own 150%, got $(tr -d '\r' < "$T/sdump" 2>/dev/null | grep ComboBox | head -1)"
lp=$("$WINE" reg query 'HKCU\Control Panel\Desktop' /v LogPixels 2>/dev/null | tr -d '\r' | awk '$1 == "LogPixels" {print $3}')
[ "$lp" = 0x60 ] && pass "the session's scale (LogPixels) stays 100%" || fail "LogPixels: want 0x60, got '$lp'"
exit $RC
