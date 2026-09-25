#!/bin/sh
# Gate for the On-Screen Keyboard (sg-osk, osk.exe): Notepad has the focus,
# Win+Ctrl+O on the X keyboard opens the keyboard (through explorer and
# system32's osk.exe, wine-sg 0181/0182), and the X mouse clicks its keys
# (their screen centres from SG_OSK_DUMP). Notepad must keep the focus the
# whole time and get exactly what was clicked.
#
#   - the window: visible, topmost, WS_EX_NOACTIVATE; nothing over it
#   - Shift, h, e, l, l, o, comma, space, Shift, w, o, r, l, d, Shift, 1
#     types "Hello, World!" (Shift is sticky and lets go after one key; the
#     labels follow it)
#   - Caps Lock lights, capitals come out, and goes off again; Fn shows F1
#   - Ctrl latched + a selects all, and Backspace clears it
#   - the title bar drags the window, Mv Up moves it to the top, Dock fits it
#     across the bottom and takes that space from the work area (wine-sg
#     0182), undocking gives it back -- Notepad in front throughout
#   - Win+Ctrl+O again closes it; its place is kept in HKCU\Software\Microsoft\Osk
#
# On a Wine without 0181/0182 the keyboard is started directly and the
# Windows-key and work-area checks are skipped (the gate says so).
# Screenshots: build/osk-*.png. SG_OSK_EXE runs another build (mutants:
# -DSG_MUTANT_ACTIVATE, -DSG_MUTANT_STICKY); SG_WINE_DIR another Wine (a
# build tree works). Display :172.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_OSK_EXE:-$HERE/build/sg-osk64.exe}"
OUT="$HERE/build"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
RC=0; DPY="${SG_OSK_DPY:-172}"; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
if [ -x "$WINE_DIR/bin/wine" ]; then WBIN="$WINE_DIR/bin"; WSERVER="$WINE_DIR/bin/wineserver"
elif [ -x "$WINE_DIR/wine" ]; then WBIN="$WINE_DIR"; WSERVER="$WINE_DIR/server/wineserver"
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi
[ -f "$EXE" ] || { echo "SKIP: $EXE missing (make build)"; exit 77; }

T=$(mktemp -d /var/tmp/sg-osk-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WSERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

"$MINGW" -O2 -municode -o "$T/probe.exe" "$HERE/test/sg-a11y-probe.c" -luser32 -lgdi32 || { fail "probe did not build"; exit 1; }

Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WBIN:$(dirname "$WSERVER"):$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
cp "$T/probe.exe" "$WINEPREFIX/drive_c/probe.exe"
cp "$EXE" "$WINEPREFIX/drive_c/sg-osk64.exe"
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
# App Paths as defaults/81-sg-osk.reg has it, pointing at this build.
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\osk.exe' /ve /d 'C:\sg-osk64.exe'
wineserver -w
P() { wine 'C:\probe.exe' "$@" 2>/dev/null | tr -d '\r'; }
DUMP="$WINEPREFIX/drive_c/osk.txt"
export SG_OSK_DUMP='C:\osk.txt'
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
wine notepad >/dev/null 2>&1 &
i=0; while [ "$(P text Notepad)" = NOWINDOW ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
P activate Notepad; sleep 1
[ "$(P foreground)" = Notepad ] && pass "Notepad has the focus" || fail "Notepad is not in front: $(P foreground)"

val() { sed -n "s/^$1 //p" "$DUMP" 2>/dev/null | tr -d '\r' | head -1; }
keyfield() { sed -n "s/^KEY $1 //p" "$DUMP" 2>/dev/null | tr -d '\r' | head -1 | cut -d' ' -f"$2"; }
click() {
    for k in "$@"; do
        x=$(keyfield "$k" 1); y=$(keyfield "$k" 2)
        [ -n "$x" ] || { fail "no key $k in the dump"; continue; }
        xdotool mousemove "$x" "$y" click 1; sleep 0.3
    done
}
wait_text() { i=0; while [ "$(P text Notepad)" != "$1" ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done; [ "$(P text Notepad)" = "$1" ]; }
focus_kept() { f=$(P foreground); [ "$f" = Notepad ] && pass "Notepad keeps the focus ($1)" || fail "the focus went to $f ($1)"; }

HAVE_KEYS=0
[ -f "$WINEPREFIX/drive_c/windows/system32/osk.exe" ] && HAVE_KEYS=1
if [ $HAVE_KEYS = 1 ]; then
    [ -f "$WINEPREFIX/drive_c/windows/syswow64/osk.exe" ] && pass "osk.exe in system32 and syswow64 (0181)" || fail "osk.exe missing from syswow64"
    xdotool key super+ctrl+o
else
    echo "NOTE  this Wine has no osk.exe (wine-sg 0181/0182): started directly, Windows-key checks skipped"
    wine 'C:\sg-osk64.exe' >/dev/null 2>&1 &
fi
i=0; while [ "$(val VISIBLE)" != 1 ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
[ "$(val VISIBLE)" = 1 ] && pass "the On-Screen Keyboard is open$([ $HAVE_KEYS = 1 ] && echo ' (Win+Ctrl+O)')" || fail "no keyboard window"
sleep 1
EX=$(val EXSTYLE)
[ $(( 0x$EX & 0x08000000 )) -ne 0 ] && [ $(( 0x$EX & 0x8 )) -ne 0 ] && pass "it is topmost and WS_EX_NOACTIVATE ($EX)" \
    || fail "styles $EX"
focus_kept "after it opened"
import -window root "$OUT/osk-open.png"

# --- Hello, World! -------------------------------------------------------------------------
click shift
[ "$(val LATCHED)" = 1 ] && [ "$(keyfield h 4)" = H ] && [ "$(keyfield 1 4)" = '!' ] \
    && pass "Shift latches and the labels follow it (H, !)" || fail "after Shift: latched $(val LATCHED), h=$(keyfield h 4), 1=$(keyfield 1 4)"
import -window root "$OUT/osk-shift.png"
click h e l l o comma space shift w o r l d shift 1
if wait_text "Hello, World!"; then pass "clicked keys type \"Hello, World!\" into Notepad"
else fail "Notepad has \"$(P text Notepad)\""; fi
[ "$(val LATCHED)" = 0 ] && pass "Shift let go after one key" || fail "still latched: $(val LATCHED)"
focus_kept "after typing"
P info OSKMainClass > "$T/info"
grep -q '^TOPMOST 1' "$T/info" && grep -q '^ABOVE (none)' "$T/info" && pass "nothing is over the keyboard" \
    || fail "over the keyboard: $(tr '\n' ' ' < "$T/info")"

# --- Caps Lock and Fn -----------------------------------------------------------------------------
click caps; sleep 0.5
[ "$(val CAPS)" = 1 ] && [ "$(keyfield caps 3)" = 1 ] && [ "$(keyfield a 4)" = A ] \
    && pass "Caps Lock is on, its key lit, letters shown as capitals" || fail "Caps: $(val CAPS), lit $(keyfield caps 3), a=$(keyfield a 4)"
import -window root "$OUT/osk-caps.png"
click space a b; wait_text "Hello, World! AB" && pass "with Caps Lock letters come out as capitals" || fail "Notepad has \"$(P text Notepad)\""
click caps; sleep 0.5
[ "$(val CAPS)" = 0 ] && [ "$(keyfield a 4)" = a ] && pass "Caps Lock goes off again" || fail "Caps still $(val CAPS)"
click fn
[ "$(keyfield 1 4)" = F1 ] && [ "$(keyfield equals 4)" = F12 ] && pass "Fn shows F1-F12 on the number row" || fail "Fn: 1=$(keyfield 1 4)"
click fn

# --- Ctrl+A, Backspace ----------------------------------------------------------------------
click ctrl a backspace
wait_text "" && pass "Ctrl (latched) + A selects all, Backspace deletes it" || fail "Notepad has \"$(P text Notepad)\""
focus_kept "after Ctrl+A"

# --- moving and docking --------------------------------------------------------------------------------
set -- $(val WINDOW) - - - -; L0=$1; T0=$2
set -- $(val TITLE) 0 0; xdotool mousemove "$1" "$2"; xdotool mousedown 1; sleep 0.2
xdotool mousemove $(( $1 - 60 )) $(( $2 - 40 )); sleep 0.2; xdotool mousemove $(( $1 - 120 )) $(( $2 - 80 )); sleep 0.3; xdotool mouseup 1; sleep 0.5
set -- $(val WINDOW) - - - -
[ "$1" = $((L0 - 120)) ] && [ "$2" = $((T0 - 80)) ] && pass "dragging the title bar moves it ($L0,$T0 -> $1,$2)" || fail "after the drag: $1,$2 (was $L0,$T0)"
focus_kept "after moving it"
click mvup; sleep 0.5
set -- $(val WINDOW) - - - -; [ "$2" = 0 ] && pass "Mv Up moves it to the top of the screen" || fail "after Mv Up: top $2"
WB=$(P workarea | cut -d' ' -f4)   # the bottom of the work area: above the taskbar
click dock; sleep 1
set -- $(val WINDOW) - - - -
if [ "$1" = 0 ] && [ "$3" = 1024 ] && [ "$4" = "$WB" ] && [ "$(val DOCK)" = 1 ]; then pass "Dock fits it across the bottom ($1 $2 $3 $4)"
else fail "docked window $1 $2 $3 $4, work area bottom $WB"; fi
if [ $HAVE_KEYS = 1 ]; then
    [ "$(P workarea)" = "0 0 1024 $2" ] && pass "the docked keyboard's space comes off the work area" || fail "work area $(P workarea), keyboard top $2"
fi
import -window root "$OUT/osk-docked.png"
click dock; sleep 1
[ "$(val DOCK)" = 0 ] && pass "Dock again undocks it" || fail "still docked"
[ $HAVE_KEYS = 1 ] && { [ "$(P workarea)" = "0 0 1024 728" ] && pass "the work area is given back" || fail "work area $(P workarea)"; }
focus_kept "after docking"
click space o k; wait_text " ok" && pass "it still types after all that" || fail "Notepad has \"$(P text Notepad)\""

# --- closing ------------------------------------------------------------------------------------------------
if [ $HAVE_KEYS = 1 ]; then
    xdotool key super+ctrl+o
else
    set -- $(val CLOSE); xdotool mousemove "$1" "$2" click 1
fi
i=0; while [ "$(head -1 "$DUMP" | tr -d '\r')" != CLOSED ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
[ "$(head -1 "$DUMP" | tr -d '\r')" = CLOSED ] && pass "$([ $HAVE_KEYS = 1 ] && echo 'Win+Ctrl+O' || echo 'the close button') closes it" \
    || fail "it did not close"
Q=$(wine reg query 'HKCU\Software\Microsoft\Osk' /v WindowTop 2>/dev/null | tr -d '\r' | awk '/WindowTop/ {print $3}')
[ "$Q" = 0x0 ] && pass "its place is kept (HKCU\\Software\\Microsoft\\Osk WindowTop 0)" || fail "WindowTop $Q"
import -window root "$OUT/osk-closed.png"

[ $RC = 0 ] && echo "osk-check: PASS" || echo "osk-check: FAIL"
exit $RC
