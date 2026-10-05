#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# The notification centre (sg-notify): the taskbar's notification icon and
# its panel. wine-sg (0815) keeps every toast and balloon in
# HKCU\Software\Stained Glass\Notifications\History; here three are planted,
# and under Xvfb, with the shell's taskbar:
#   - --dump lists them newest first, all new;
#   - /toggle (Win+A) opens the panel at the right of the screen, the work
#     area's height, listing them newest first, and marks them seen;
#   - a notification's x dismisses it (gone from the history too);
#   - Clear all dismisses the rest;
#   - /toggle again closes it.
#
#   sh test/notify-check.sh        SG_WINE_DIR=<wine-sg>   (mutant SG_MUTANT_NOTIFY_NO_CLEAR)
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="${SG_WINE:-$WINE_DIR/bin/wine}"
WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER=$(dirname "$WINE")/server/wineserver
EXE="${SG_NOTIFY_EXE:-$HERE/build/sg-notify64.exe}"
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in Xvfb xdotool; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
T=$(mktemp -d /var/tmp/sg-notify-check.XXXXXX); XP=
cleanup() { WINEPREFIX="$T/pfx" "$WINESERVER" -k 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' SG_NOTIFY_DUMP="$T/dump"
Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/display")"
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w
cp "$EXE" "$T/sg-notify64.exe"
"$WINE" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
add() {
    k="HKCU\\Software\\Stained Glass\\Notifications\\History\\$1"
    "$WINE" reg add "$k" /v App /d "$2" /f >/dev/null 2>&1
    "$WINE" reg add "$k" /v Title /d "$3" /f >/dev/null 2>&1
    "$WINE" reg add "$k" /v Body /d "$4" /f >/dev/null 2>&1
}
add 00000000 Firefox "New message from Alex" "Are we still on for lunch tomorrow?"
add 00000001 Backup "Backup finished" "Your files were copied."
add 00000002 Thunderbird "3 new emails" ""
"$WINE" reg add 'HKCU\Software\Stained Glass\Notifications\History' /v Next /t REG_DWORD /d 3 /f >/dev/null 2>&1
"$WINESERVER" -w
mkdir -p "$T/run"; export XDG_RUNTIME_DIR="$T/run" SG_LOCK_CONTROL=/nonexistent

out=$("$WINE" "$T/sg-notify64.exe" --dump 2>&1 | tr -d '\r' | grep -E '^(NOTIFICATIONS|ENTRY)')
echo "$out" | sed 's/^/      /'
[ "$(echo "$out" | head -1)" = "NOTIFICATIONS 3 unread 3" ] && [ "$(echo "$out" | sed -n 2p)" = "ENTRY 2|Thunderbird|3 new emails|" ] \
    && pass "--dump: the three, newest first, all new" || fail "--dump: $out"

"$WINE" explorer /desktop=shell,1280x800 >/dev/null 2>&1 &
sleep 8
"$WINE" "$T/sg-notify64.exe" >/dev/null 2>&1 &
sleep 3
d() { cat "$T/dump" 2>/dev/null; }
val() { d | sed -n "s/^$1=//p"; }
"$WINE" "$T/sg-notify64.exe" /toggle >/dev/null 2>&1; sleep 2
set -- $(val rect | tr ',' ' ')
[ "$(val visible)" = 1 ] && [ $# = 4 ] && [ "$3" = 1280 ] && [ "$2" = 0 ] && [ $(( $4 - $2 )) -ge 700 ] \
    && pass "/toggle (Win+A) opens the panel at the right, the work area's height: $(val rect)" || fail "panel: visible $(val visible) rect $(val rect)"
px=$1 py=$2
[ "$(d | grep '^entry' | awk '{print $2}' | tr '\n' ' ')" = "2 1 0 " ] && pass "it lists them newest first" \
    || fail "order: $(d | grep '^entry' | awk '{print $2}' | tr '\n' ' ')"
[ "$(val unread)" = 0 ] && pass "opening it marks them seen" || fail "unread after opening: $(val unread)"

# the middle one's x
set -- $(d | sed -n 's/^entry 1 [^ ]* close=\([0-9,]*\) .*/\1/p' | tr ',' ' ')
if [ $# = 4 ]; then
    cx=$(( px + ($1 + $3) / 2 )) cy=$(( py + ($2 + $4) / 2 ))
    xdotool mousemove $(( cx - 30 )) "$cy"; sleep 0.4; xdotool mousemove "$cx" "$cy"; sleep 0.4; xdotool click 1; sleep 1.2
fi
hist=$("$WINE" reg query 'HKCU\Software\Stained Glass\Notifications\History' 2>/dev/null | tr -d '\r')
[ "$(val count)" = 2 ] && ! printf '%s\n' "$hist" | grep -q '\\00000001$' \
    && pass "a notification's x dismisses it, from the history too" || fail "dismiss: count $(val count); $(printf '%s\n' "$hist" | grep -c '\\0000')"

set -- $(val clear | tr ',' ' ')
[ $# = 4 ] && { xdotool mousemove $(( px + ($1 + $3) / 2 )) $(( py + ($2 + $4) / 2 )); sleep 0.4; xdotool click 1; sleep 1.2; }
hist=$("$WINE" reg query 'HKCU\Software\Stained Glass\Notifications\History' 2>/dev/null | tr -d '\r')
[ "$(val count)" = 0 ] && ! printf '%s\n' "$hist" | grep -q '\\0000' \
    && pass "Clear all dismisses the rest" || fail "clear all: count $(val count); $(printf '%s\n' "$hist" | grep -c '\\0000') left"

"$WINE" "$T/sg-notify64.exe" /toggle >/dev/null 2>&1; sleep 1.5
[ "$(val visible)" = 0 ] && pass "/toggle again closes it" || fail "still open: $(val visible)"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
