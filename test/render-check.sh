#!/bin/sh
# Render sg-taskbar in a headless X server and check that it docks as an AppBar
# and paints -- i.e. a real bar at the bottom edge, not a blank or missing one.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
BAR="$HERE/build/sg-taskbar64.exe"
RC=0; DPY=86; T=$(mktemp -d); chmod 755 "$T"; XP=""
export HOME="$T"
# shellcheck disable=SC2317
cleanup() { pkill -f sg-taskbar64 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null; rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for t in Xvfb xdotool xwininfo import; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$BAR" ] || { echo "SKIP: wine-sg or the panel not built"; exit 77; }

export WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
rm -f "/tmp/.X${DPY}-lock"
Xvfb ":$DPY" -screen 0 1280x800x24 -ac >/dev/null 2>&1 & XP=$!; sleep 2
export DISPLAY=":$DPY"
wine "$BAR" >/dev/null 2>&1 &
_w=0; W=""
while [ $_w -lt 30 ]; do W=$(xdotool search --name 'sg-taskbar' 2>/dev/null | head -1); [ -n "$W" ] && break; sleep 1; _w=$((_w+1)); done
[ -n "$W" ] && pass "the taskbar window appears" || { fail "no taskbar window"; echo "RESULT: FAIL"; exit 1; }

geo=$(xdotool getwindowgeometry "$W" 2>/dev/null)
case "$geo" in
    *"1280x40"*) pass "it is a full-width bottom strip (1280x40)" ;;
    *) fail "wrong geometry: $(echo "$geo" | tr '\n' ' ')" ;;
esac
case "$geo" in *"760"*) pass "it docked at the bottom edge (y=760)" ;; *) fail "not at the bottom edge" ;; esac

# Paints: capture and check it is not a single flat colour (the Start glyph and
# clock give it at least a few distinct colours).
import -window "$W" "$T/shot.png" 2>/dev/null
colors=$(import -window "$W" -depth 4 "$T/c.gif" 2>/dev/null; identify -format '%k' "$T/c.gif" 2>/dev/null || echo 1)
if [ "${colors:-1}" -ge 2 ]; then pass "the bar paints its content ($colors colours)"; else fail "the bar is blank"; fi

echo
[ "$RC" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
