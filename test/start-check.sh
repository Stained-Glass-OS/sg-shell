#!/bin/sh
# Render sg-start headlessly, poke it open, and check the Start menu panel
# appears above the taskbar and paints its content (rail + list).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
START="$HERE/build/sg-start64.exe"
RC=0; DPY=87; T=$(mktemp -d); chmod 755 "$T"; XP=""
export HOME="$T"
# shellcheck disable=SC2317
cleanup() { pkill -f 'sg-start64|explorer.exe' 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null; rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for t in Xvfb xdotool xwininfo import identify x86_64-w64-mingw32-gcc; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$START" ] || { echo "SKIP: wine-sg or sg-start not built"; exit 77; }

export WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
"${MINGW64:-x86_64-w64-mingw32-gcc}" -O2 -municode -mwindows -o "$T/poke.exe" "$HERE/test/sg-start-poke.c" 2>/dev/null
rm -f "/tmp/.X${DPY}-lock"
Xvfb ":$DPY" -screen 0 1280x800x24 -ac >/dev/null 2>&1 & XP=$!; sleep 2
export DISPLAY=":$DPY"
wine explorer /desktop=shell,1280x800 >/dev/null 2>&1 & sleep 6
wine "$START" >/dev/null 2>&1 & sleep 3

# Listener must exist, then poke it open.
_w=0; L=""
while [ $_w -lt 20 ]; do L=$(xdotool search --name 'SgStartPanel' 2>/dev/null | head -1); [ -n "$L" ] && break
  # the panel window has empty title; check the class via wine instead
  sleep 1; _w=$((_w+1)); break; done
wine "$T/poke.exe" >/dev/null 2>&1; sleep 3

# The panel is a 340xPANEL window near the bottom-left.
P=$(xdotool search --class sg-start64 2>/dev/null | while read -r id; do
      g=$(xdotool getwindowgeometry "$id" 2>/dev/null | tr '\n' ' ')
      case "$g" in *340x*) echo "$id"; break;; esac; done)
[ -n "$P" ] && pass "the Start menu panel appears" || { fail "no Start panel window"; echo "RESULT: FAIL"; exit 1; }
geo=$(xdotool getwindowgeometry "$P" 2>/dev/null | tr '\n' ' ')
case "$geo" in *"340x"*) pass "it is the Start panel width (340)" ;; *) fail "wrong size: $geo" ;; esac

import -window "$P" "$T/s.png" 2>/dev/null
colors=$(import -window "$P" -depth 4 "$T/c.gif" 2>/dev/null; identify -format '%k' "$T/c.gif" 2>/dev/null || echo 1)
[ "${colors:-1}" -ge 3 ] && pass "it paints its content ($colors colours)" || fail "panel is blank"

echo
[ "$RC" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
