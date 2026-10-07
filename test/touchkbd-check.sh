#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for the touch keyboard (sg-touchkbd, TabTip.exe). A program with a text
# field and a button (test/touchkbd-probe.c) is in front; the keyboard runs as
# at sign-in (/background) on a "touch screen" (SG_TOUCHKBD_TOUCH=1: Xvfb has
# none) with the input devices of a Surface without its Type Cover
# (SG_TOUCHKBD_DEVICES: power and volume buttons only).
#
#   - started in the background: its button in the notification area, hidden
#   - a touch focusing the text field shows it across the bottom, above the
#     taskbar and over the programs (the work area theirs still), never
#     taking the focus
#   - its keys type: Shift once, h, e, l, l, o -> "Hello"; Backspace; the
#     number page and 1; the space bar
#   - a touch moving the focus to the button hides it again
#   - with a hardware keyboard attached, a touch on the text field leaves it
#     hidden; /toggle shows it anyway and its hide key hides it
#
#   - its settings: the taskbar's "Show touch keyboard button", Settings'
#     "Show the touch keyboard when not in tablet mode..." and "...in windowed
#     apps..."; a keyboard attached while shown hides it; a Linux program's
#     window in front gets its keys through sg-xtype (a stand-in here)
#
# SG_TOUCHKBD_EXE runs another build (mutants: -DSG_MUTANT_TOUCHKBD_NO_AUTOSHOW,
# -DSG_MUTANT_TOUCHKBD_NO_KEYBOARD_CHECK, -DSG_MUTANT_TOUCHKBD_NO_LIVE_KEYBOARD,
# -DSG_MUTANT_TOUCHKBD_NO_XTYPE);
# SG_WINE_DIR another Wine (a build tree works). Display :176. Screenshot:
# build/touchkbd-shown.png.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_TOUCHKBD_EXE:-$HERE/build/sg-touchkbd64.exe}"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
RC=0; DPY="${SG_TOUCHKBD_DPY:-176}"; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
if [ -x "$WINE_DIR/bin/wine" ]; then WBIN="$WINE_DIR/bin"; WSERVER="$WINE_DIR/bin/wineserver"
elif [ -x "$WINE_DIR/wine" ]; then WBIN="$WINE_DIR"; WSERVER="$WINE_DIR/server/wineserver"
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi
[ -f "$EXE" ] || { echo "SKIP: $EXE missing (make build)"; exit 77; }
[ -e "/tmp/.X${DPY}-lock" ] && { echo "SKIP: display :$DPY is taken"; exit 77; }

T=$(mktemp -d /var/tmp/sg-touchkbd-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WSERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; [ -n "${TOUCHKBD_KEEP:-}" ] && echo "kept $T" || rm -rf "$T"
}
trap cleanup EXIT INT TERM

"$MINGW" -O2 -municode -mwindows -o "$T/probe.exe" "$HERE/test/touchkbd-probe.c" -luser32 -lgdi32 || { fail "probe did not build"; exit 1; }

unset DISPLAY XAUTHORITY WAYLAND_DISPLAY
Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
sleep 1
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" WINEDEBUG=-all
export PATH="$WBIN:$(dirname "$WSERVER"):$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
C="$WINEPREFIX/drive_c"
cp "$T/probe.exe" "$C/probe.exe"
cp "$EXE" "$C/sg-touchkbd64.exe"
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
wineserver -w
# a Surface without its Type Cover: its buttons (power, volume) are no keyboard
cat > "$C/no-keyboard.txt" <<'EOF'
I: Bus=0019 Vendor=0000 Product=0001 Version=0000
N: Name="Power Button"
H: Handlers=kbd event0
B: PROP=0
B: EV=3
B: KEY=10000000000000 0

I: Bus=0018 Vendor=045e Product=0922 Version=0001
N: Name="Surface Pro 7 Buttons"
H: Handlers=kbd event5
B: PROP=0
B: EV=100013
B: KEY=1c000000000000 0 0 0

I: Bus=0018 Vendor=045e Product=099f Version=0100
N: Name="IPTS Touch"
H: Handlers=event7
B: PROP=2
B: EV=b
B: KEY=400 0 0 0 0 0
B: ABS=6e18000 3
EOF
# the Type Cover attached: a keyboard that repeats keys, with letters
cat "$C/no-keyboard.txt" > "$C/keyboard.txt"
cat >> "$C/keyboard.txt" <<'EOF'

I: Bus=0003 Vendor=045e Product=09c0 Version=0111
N: Name="Microsoft Surface Type Cover Keyboard"
H: Handlers=sysrq kbd leds event9
B: PROP=0
B: EV=120013
B: KEY=1000000000007 ff9f207ac14057ff febeffdfffefffff fffffffffffffffe
B: MSC=10
B: LED=1f
EOF
cp "$C/no-keyboard.txt" "$C/devices.txt"
# a stand-in for sg-session's sg-xtype (XTEST into a Linux program's window):
# it writes what it was asked to type
printf '#!/bin/sh\necho "$*" >> "%s/xtype.log"\n' "$C" > "$T/fake-xtype"
chmod +x "$T/fake-xtype"
export SG_XTYPE="$T/fake-xtype"
export SG_TOUCHKBD_TOUCH=1 SG_TOUCHKBD_DEVICES='C:\devices.txt' SG_TOUCHKBD_DUMP='C:\touchkbd.txt'
DUMP="$C/touchkbd.txt"; STATE="$C/touchkbd-state.txt"

wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
sleep 2
wine 'C:\probe.exe' >/dev/null 2>&1 &
i=0; while [ ! -s "$STATE" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
wine 'C:\sg-touchkbd64.exe' /background >/dev/null 2>&1 &
i=0; while [ ! -s "$DUMP" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
sleep 1

val() { sed -n "s/^$1 //p" "$DUMP" 2>/dev/null | tr -d '\r' | head -1; }
state() { sed -n "s/^$1 //p" "$STATE" 2>/dev/null | tr -d '\r' | head -1; }
shown() { val WINDOW | cut -d' ' -f1; }
key() { sed -n "s/^KEY $1 //p" "$DUMP" 2>/dev/null | tr -d '\r' | head -1; }
cmd() { echo "$1" >> "$C/touchkbd-cmd.txt"; sleep 1.5; [ -n "${TOUCHKBD_KEEP:-}" ] && { _i=0; while [ $_i -lt 40 ] && [ "$(shown)" != 1 ]; do sleep 0.25; _i=$((_i+1)); done; echo "waited $_i quarter seconds more"; }; [ -n "${TOUCHKBD_KEEP:-}" ] && { echo "after $1:"; tr -d '\r' < "$DUMP" | head -2; tr -d '\r' < "$STATE"; }; }
tap() {
    for k in "$@"; do
        set -- $(key "$k")
        [ -n "${1:-}" ] || { fail "no key $k in the dump"; continue; }
        xdotool mousemove "$1" "$2" click 1; sleep 0.3
    done
    sleep 0.5
}

grep -q 'tray=1' "$DUMP" && pass "started at sign-in: its button is in the notification area" || fail "no taskbar button: $(val STATE)"
[ "$(shown)" = 0 ] && pass "and the keyboard is hidden" || fail "shown at start: $(val WINDOW)"

cmd touch-edit
command -v import >/dev/null && import -window root "$HERE/build/touchkbd-touched.png" 2>/dev/null
[ "$(shown)" = 1 ] && pass "a touch focusing the text field shows it ($(val STATE))" || fail "the text field's touch did not show it: $(val WINDOW)"
set -- $(val WINDOW); ky=$3; kh=$5
[ "${ky:-0}" -gt 384 ] && [ $(( ky + kh )) -le 768 ] && pass "across the bottom ($(val WINDOW))" || fail "its place: $(val WINDOW)"
work() { rm -f "$C/touchkbd-work.txt"; wine 'C:\probe.exe' workarea >/dev/null 2>&1; tr -d '\r' < "$C/touchkbd-work.txt" 2>/dev/null; }
# over the programs: it takes nothing from the work area (that would resize
# every maximized program each time it shows)
WA=$(work); set -- $WA
[ "${4:-0}" -gt "$ky" ] && [ "$(( ky + kh ))" -le "${4:-0}" ] \
    && pass "above the taskbar, over the programs: the work area is theirs still ($WA)" \
    || fail "the work area: $WA, keyboard $ky+$kh"
tap shift h e l l o
[ "$(state TEXT)" = Hello ] && pass "its keys type: Shift once, h e l l o -> Hello" || fail "typed: '$(state TEXT)'"
[ "$(state FRONT)" = 1 ] && [ "$(state FOCUS)" = edit ] && pass "the program keeps the focus" \
    || fail "the focus moved: front $(state FRONT) ($(state FG)), focus $(state FOCUS)"
command -v import >/dev/null && import -window root "$HERE/build/touchkbd-shown.png" 2>/dev/null
tap backspace
[ "$(state TEXT)" = Hell ] && pass "Backspace" || fail "after Backspace: '$(state TEXT)'"
tap page
tap u0031 page space
[ "$(state TEXT)" = "Hell1 " ] && pass "the number page types 1; back to letters; the space bar" || fail "typed: '$(state TEXT)'"

cmd touch-button
[ "$(shown)" = 0 ] && pass "a touch moving the focus off the text field hides it" || fail "still shown on the button: $(val WINDOW)"

cp "$C/keyboard.txt" "$C/devices.txt"
cmd touch-edit
[ "$(shown)" = 0 ] && pass "with a hardware keyboard attached a touch on the text field leaves it hidden" \
    || fail "shown with a keyboard attached: $(val WINDOW)"
wine 'C:\sg-touchkbd64.exe' /toggle >/dev/null 2>&1; sleep 1.5
[ "$(shown)" = 1 ] && pass "/toggle (the taskbar button's action) shows it anyway" || fail "/toggle: $(val WINDOW)"
tap hide
[ "$(shown)" = 0 ] && pass "its hide key hides it" || fail "the hide key: $(val WINDOW)"


# --- its settings (Settings > Devices > Typing; the taskbar's menu) ---
cp "$C/no-keyboard.txt" "$C/devices.txt"
cmd "set TipbandDesiredVisibility 0"
grep -q 'tray=0' "$DUMP" && pass "\"Show touch keyboard button\" off: the button leaves the notification area" \
    || fail "the button stays: $(val STATE)"
cmd "set TipbandDesiredVisibility 1"
grep -q 'tray=1' "$DUMP" && pass "and on again: it comes back" || fail "the button did not come back: $(val STATE)"
cmd "set EnableDesktopModeAutoInvoke 0"
cmd touch-button
cmd touch-edit
[ "$(shown)" = 0 ] && pass "\"Show the touch keyboard when not in tablet mode...\" off: a touched text field leaves it hidden" \
    || fail "shown with the setting off: $(val WINDOW)"
cmd "set EnableDesktopModeAutoInvoke 1"
cmd "set AutoInvokeInWindowedApps 0"
cmd touch-button
cmd touch-edit
[ "$(shown)" = 0 ] && pass "\"...in windowed apps\" off: not for a program in a window" || fail "shown for a windowed program: $(val WINDOW)"
cmd "set AutoInvokeInWindowedApps 1"
cmd touch-button
cmd touch-edit
[ "$(shown)" = 1 ] && pass "both on: shown again" || fail "not shown with both on: $(val WINDOW)"

# --- a keyboard attached while it is shown (a Type Cover clicked on) ---
cp "$C/keyboard.txt" "$C/devices.txt"
i=0; while [ "$(shown)" = 1 ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
[ "$(shown)" = 0 ] && pass "a keyboard attached hides the keyboard that showed itself (in $((i / 2)) s)" \
    || fail "still shown with a keyboard attached: $(val WINDOW)"
cp "$C/no-keyboard.txt" "$C/devices.txt"

# --- a Linux program in front: its keys through sg-xtype ---
wine 'C:\sg-touchkbd64.exe' /show >/dev/null 2>&1; sleep 1.5
cmd linux-front
tap h i
tap backspace
tap shift a
x=$(tr -d '\r' < "$C/xtype.log" 2>/dev/null | tr '\n' ' ')
[ "$x" = "u:0068 u:0069 k:BackSpace u:0041 " ] && pass "a Linux program's window in front: its keys go through sg-xtype ($x)" \
    || fail "sg-xtype was asked: '$x'"
cmd front
before=$(state TEXT)
tap z
[ "$(state TEXT)" = "${before}z" ] && pass "and a Wine program's in front again: SendInput as before" \
    || fail "back in the Wine program: '$(state TEXT)' after '$before'"

# --- the sign-in screen and setup: /background /notray -- no taskbar there,
# so no button (Wine stood it in an empty tray window of its own: a white box
# on the Surface's sign-in screen, David 2026-10-07) ---
wine taskkill /f /im sg-touchkbd64.exe >/dev/null 2>&1; sleep 2
rm -f "$DUMP"
wine 'C:\sg-touchkbd64.exe' /background /notray >/dev/null 2>&1 &
i=0; while [ ! -s "$DUMP" ] && [ $i -lt 30 ]; do sleep 0.5; i=$((i + 1)); done
grep -q 'tray=0' "$DUMP" && pass "started with /notray (sign-in, setup): no button in a notification area that is not there" \
    || fail "/notray: $(val STATE)"
[ "$(shown)" = 0 ] && pass "and hidden until a text field is touched" || fail "/notray: shown at start: $(val WINDOW)"

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
