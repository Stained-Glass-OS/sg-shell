#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for Alarms & Clock (sg-clock) on a shell desktop under Xvfb, driven by
# the X mouse and keyboard and read back from its dump (SG_CLOCK_DUMP), the
# registry and screenshots:
#
#   - ms-clock:stopwatch opens it; the stopwatch runs, pauses (and stays
#     paused), takes a lap and resets
#   - a 3-second timer made in the New timer dialog fires: the notification
#     ("Timer", its name, "Time's up!") appears above the taskbar; Dismiss
#   - World Clock: Add a new location, "tokyo" -> Tokyo, at Tokyo's time
#   - an alarm set for the next minute goes off (a notification with Snooze
#     and Dismiss); Snooze snoozes it
#   - closing the window with an alarm on keeps it running (hidden), and it
#     is set to start with the session (HKCU Run /background)
#   - alarms, cities and timers are there after a restart
#
# Screenshots: build/clock-*.png. Needs wine-sg, Xvfb, xdotool, ImageMagick;
# skips (77) without them. SG_CLOCK_EXE runs another build (the mutants).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_CLOCK_EXE:-$HERE/build/sg-clock64.exe}"
OUT="$HERE/build"
RC=0; DPY="${SG_CLOCK_DPY:-122}"; XP=""
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }

for need in Xvfb xdotool import; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-clock-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; [ "${KEEP:-0}" = 1 ] && echo "kept $T" || rm -rf "$T"
}
trap cleanup EXIT INT TERM

Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
cp "$EXE" "$T/sg-clock64.exe"
winexe=$(wine winepath -w "$T/sg-clock64.exe" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
for f in "$HERE/theme/50-sg-colors.reg" "$HERE/theme/52-sg-fonts.reg"; do
    wine reg import "$(wine winepath -w "$f" | tr -d '\r')" >/dev/null 2>&1
done
esc=$(printf '%s' "$winexe" | sed 's/\\/\\\\\\\\/g')
sed "s|Z:\\\\\\\\usr\\\\\\\\libexec\\\\\\\\stained-glass\\\\\\\\shell\\\\\\\\sg-clock64.exe|$esc|g" \
    "$HERE/defaults/67-sg-clock.reg" > "$T/clock.reg"
wine reg import "$(wine winepath -w "$T/clock.reg" | tr -d '\r')" >/dev/null 2>&1
wineserver -w

WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
# a window on the desktop, so it is drawn (and the clock is not its only program)
wine notepad >/dev/null 2>&1 &
sleep 2

DUMP="$T/clock.dump"
export SG_CLOCK_DUMP="$(wine winepath -w "$DUMP" | tr -d '\r')"
D() { sed -n "s/^$1 //p" "$DUMP" 2>/dev/null | head -1; }
at() { grep "^$1 " "$DUMP" 2>/dev/null | head -1 | sed -n 's/.* at=\([0-9-]*\),\([0-9-]*\):.*/\1 \2/p'; }
click() { set -- $(at "$1"); [ $# -eq 2 ] || { fail "nothing to click for '$1'"; return 1; }; xdotool mousemove "$1" "$2" click 1; sleep 0.6; }
wait_grep() { i=0; while ! grep -q "$1" "$DUMP" 2>/dev/null && [ $i -lt $(( ${2:-10} * 4 )) ]; do sleep 0.25; i=$((i + 1)); done; grep -q "$1" "$DUMP" 2>/dev/null; }
shot() { import -window root "$OUT/clock-$1.png" 2>/dev/null; }
sw() { D stopwatch | cut -d' ' -f1; }

wine start ms-clock:stopwatch >/dev/null 2>&1
wait_grep '^page stopwatch' 30 && pass "ms-clock:stopwatch opens the Stopwatch" || { fail "no stopwatch page: $(head -3 "$DUMP" 2>/dev/null)"; exit 1; }
sleep 1

# --- the stopwatch ---------------------------------------------------------------------------------
click 'btn swstart'
sleep 2
a=$(sw); sleep 1; b=$(sw)
[ "${a:-0}" -ge 1500 ] && [ "${b:-0}" -gt "${a:-0}" ] && pass "the stopwatch runs (${a} ms, then ${b} ms)" || fail "stopwatch: $a then $b"
click 'btn swlap'
[ "$(D stopwatch | sed -n 's/.*laps=\([0-9]*\).*/\1/p')" = 1 ] && pass "Lap records a lap" || fail "laps: $(D stopwatch)"
shot stopwatch
click 'btn swstart'
a=$(sw); sleep 1.2; b=$(sw)
[ "$a" = "$b" ] && D stopwatch | grep -q 'running=0' && pass "Pause stops it ($a ms)" || fail "paused: $a then $b"
click 'btn swreset'
[ "$(sw)" = 0 ] && D stopwatch | grep -q 'laps=0' && pass "Reset clears it" || fail "reset: $(D stopwatch)"

# --- a timer that ends ---------------------------------------------------------------------------------
click 'btn pivot 2'
wait_grep '^page timer' 5 && pass "the Timer page" || fail "page: $(D page)"
click 'btn add'
wait_grep '^dialog New timer' 5 && pass "+ opens New timer" || fail "no timer dialog"
set_field() { set -- $(at "dlgctl $1"); xdotool mousemove "$1" "$2" click --repeat 3 1; sleep 0.2; xdotool key ctrl+a; xdotool type --delay 60 "$2"; sleep 0.3; }
f() { at "dlgctl $1"; }
for id in 101 102 103; do set -- $(f $id); xdotool mousemove "$1" "$2" click 1; xdotool key ctrl+a BackSpace; done
set -- $(f 101); xdotool mousemove "$1" "$2" click 1; xdotool type 0
set -- $(f 102); xdotool mousemove "$1" "$2" click 1; xdotool type 0
set -- $(f 103); xdotool mousemove "$1" "$2" click 1; xdotool type 3
set -- $(f 100); xdotool mousemove "$1" "$2" click 1; xdotool key ctrl+a; xdotool type --delay 50 tea
sleep 0.5; shot timer-dialog
click 'dlgctl 140'
n=$(grep -c '^timer ' "$DUMP")
wait_grep '^timer [0-9]* total=3000 .*: tea$' 5 && pass "the timer 'tea', 3 seconds, is on the page" || fail "timers: $(grep '^timer' "$DUMP")"
idx=$(grep '^timer [0-9]* total=3000 .*: tea$' "$DUMP" | cut -d' ' -f2)
click "btn timerstart $idx"
D timer >/dev/null
wait_grep '^timer '"$idx"' .*running=1' 3 && pass "Start runs it" || fail "not running: $(grep '^timer' "$DUMP")"
if wait_grep "^toast [0-9]* Timer | tea | Time's up!" 10; then
    pass "3 seconds later the notification says Time's up for 'tea'"
    sleep 0.5; shot timer-toast
    click 'toastbtn dismiss'
    sleep 0.5
    grep -q '^toast ' "$DUMP" && fail "Dismiss did not close it" || pass "Dismiss closes the notification"
else fail "no timer notification: $(grep '^timer\|^toast' "$DUMP")"; fi

# --- the world clock -----------------------------------------------------------------------------------
click 'btn pivot 1'
wait_grep '^page worldclock' 5 && pass "the World Clock page" || fail "page: $(D page)"
click 'btn add'
wait_grep '^dialog Add a new location' 5 || fail "no location dialog"
set -- $(f 130); xdotool mousemove "$1" "$2" click 1; xdotool type --delay 60 tokyo; sleep 1
grep -q '^dlgctl 131 .*Tokyo' "$DUMP" && pass "searching 'tokyo' finds Tokyo's zone" || fail "zone list: $(grep '^dlgctl 131' "$DUMP")"
click 'dlgctl 140'
if wait_grep '^city [0-9]* .*: Tokyo$' 5; then
    want=$(TZ=Asia/Tokyo date +%H:%M); got=$(grep '^city [0-9]* .*: Tokyo$' "$DUMP" | head -1 | cut -d' ' -f3)
    wmin=$(( $(echo "$want" | cut -c1-2 | sed 's/^0//') * 60 + $(echo "$want" | cut -c4-5 | sed 's/^0//') ))
    gmin=$(( $(echo "$got" | cut -c1-2 | sed 's/^0//') * 60 + $(echo "$got" | cut -c4-5 | sed 's/^0//') ))
    d=$(( wmin - gmin )); [ $d -lt 0 ] && d=$(( -d ))
    [ $d -le 1 ] && pass "Tokyo shows Tokyo's time ($got; the system says $want)" || fail "Tokyo at $got, the system says $want"
else fail "no Tokyo: $(grep '^city' "$DUMP")"; fi
sleep 0.5; shot worldclock

# --- an alarm for the next minute --------------------------------------------------------------------------
click 'btn pivot 0'
wait_grep '^page alarm' 5 && pass "the Alarm page" || fail "page: $(D page)"
[ "$(date +%S | sed 's/^0//')" -gt 50 ] && sleep 12
next=$(date -d '+1 minute' +'%H %M')
click 'btn add'
wait_grep '^dialog New alarm' 5 && pass "+ opens New alarm" || fail "no alarm dialog"
set -- $(f 101); xdotool mousemove "$1" "$2" click 1; xdotool key ctrl+a BackSpace BackSpace; xdotool type "${next% *}"
set -- $(f 102); xdotool mousemove "$1" "$2" click 1; xdotool key ctrl+a BackSpace BackSpace; xdotool type "${next#* }"
set -- $(f 100); xdotool mousemove "$1" "$2" click 1; xdotool key ctrl+a; xdotool type --delay 50 wake
sleep 0.3
click 'dlgctl 140'
wait_grep "^alarm [0-9]* ${next% *}:${next#* } on=1 .*: wake\$" 5 && pass "the alarm 'wake' at ${next% *}:${next#* } is on" || fail "alarms: $(grep '^alarm' "$DUMP")"
shot alarms
if wait_grep '^toast [0-9]* Alarm | wake |' 75; then
    pass "at ${next% *}:${next#* } the alarm goes off"
    sleep 0.5; shot alarm-toast
    click 'toastbtn snooze'
    sleep 0.6
    grep -q '^toast ' "$DUMP" && fail "Snooze did not close it" || pass "Snooze closes the notification"
    grep -q '^alarm [0-9]* .*snoozed=1: wake$' "$DUMP" && pass "...and the alarm is snoozed" || fail "not snoozed: $(grep '^alarm' "$DUMP")"
else fail "the alarm did not go off: $(grep '^alarm\|^toast' "$DUMP")"; fi

# --- closing keeps it running --------------------------------------------------------------------------------
wine reg query 'HKCU\Software\Microsoft\Windows\CurrentVersion\Run' /v 'Stained Glass Clock' 2>/dev/null | grep -q '/background' \
    && pass "with an alarm on, it starts with the session (HKCU Run /background)" || fail "no Run entry"
set -- $(at 'btn pivot 0'); xdotool mousemove "$1" "$2" click 1; sleep 0.3
key_close() { xdotool key alt+F4; sleep 1; }
key_close
wait_grep '^visible 0' 5 && pass "closing the window hides it; the alarms go on" || fail "visible: $(D visible)"
pgrep -f "^[A-Za-z]:.*${T##*/}.sg-clock64.exe" >/dev/null && pass "the program is still running" || fail "the program exited"

# --- after a restart ---------------------------------------------------------------------------------------------
wine taskkill /f /im sg-clock64.exe >/dev/null 2>&1; sleep 2; rm -f "$DUMP"
wine start ms-clock:alarm >/dev/null 2>&1
wait_grep '^page alarm' 30 || fail "no restart"
grep -q '^alarm [0-9]* .*: wake$' "$DUMP" && grep -q '^city [0-9]* .*: Tokyo$' "$DUMP" && grep -q '^timer [0-9]* total=3000 .*: tea$' "$DUMP" \
    && pass "the alarm, the city and the timer are kept" || fail "after a restart: $(grep '^alarm\|^city\|^timer' "$DUMP")"
sleep 0.5; shot restart

[ $RC = 0 ] && echo "clock-check: PASS" || echo "clock-check: FAIL"
exit $RC
