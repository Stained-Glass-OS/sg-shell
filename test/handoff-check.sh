#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# The foreground lock (wine-sg 9930): a second instance that hands its request to the running one (Win+A's
# "sg-notify /toggle", the clock's "sg-calendar --show") lets that one come forward, as Windows programs do
# (AllowSetForegroundWindow) -- the second instance was started on the person's click, the running one was
# not, and without the grant its panel stayed behind the window in front.
# Here, on a shell desktop with Wine's foreground lock: the running sg-notify and sg-calendar, started
# from outside; a launcher window (as the taskbar is) whose click runs the second instance; after the click
# the panel, then the calendar, is the foreground window.
#   sh test/handoff-check.sh    SG_WINE=<wine with the lock> SG_WINESERVER=, SG_NOTIFY_EXE=, SG_CALENDAR_EXE=
# Mutant: SG_MUTANT_HANDOFF_NO_GRANT (make test-handoff builds and runs it).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="${SG_WINE:-$WINE_DIR/bin/wine}"
export WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER=$(dirname "$WINE")/server/wineserver
NOTIFY="${SG_NOTIFY_EXE:-$HERE/build/sg-notify64.exe}"
CALENDAR="${SG_CALENDAR_EXE:-$HERE/build/sg-calendar64.exe}"
RC=0; XP=
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
unset DISPLAY XAUTHORITY
for t in Xvfb xdotool x86_64-w64-mingw32-gcc; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE" ] || { echo "SKIP: no wine at $WINE"; exit 77; }
for f in "$NOTIFY" "$CALENDAR"; do [ -f "$f" ] || { echo "SKIP: $f not built"; exit 77; }; done
T=$(mktemp -d /var/tmp/sg-handoff.XXXXXX)
mkdir -p "$T/home" "$T/xdg"; chmod 700 "$T/xdg"
export HOME="$T/home" XDG_RUNTIME_DIR="$T/xdg" WINEPREFIX="$T/pfx" WINEDEBUG=-all
export WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d;winewayland.drv=d"
unset WAYLAND_DISPLAY
trap '"$WINESERVER" -k 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; rm -rf "$T"' EXIT INT TERM
x86_64-w64-mingw32-gcc -O2 -municode -o "$T/probe.exe" "$HERE/test/handoff-probe.c" || { fail "probe did not build"; exit 1; }
W=1280; H=720
Xvfb -displayfd 3 -screen 0 ${W}x${H}x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/display")"
timeout 300 "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w
"$WINE" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d ${W}x${H} /f >/dev/null 2>&1
"$WINESERVER" -w
C="$WINEPREFIX/drive_c"
cp "$NOTIFY" "$C/notify.exe"; cp "$CALENDAR" "$C/calendar.exe"; cp "$T/probe.exe" "$C/"
cd "$C" || exit 1
"$WINE" explorer /desktop=shell,${W}x${H} >/dev/null 2>&1 &
i=0; while [ $i -lt 60 ] && ! xdotool search --name 'shell - Wine Desktop' >/dev/null 2>&1; do sleep 0.5; i=$((i + 1)); done
sleep 3
"$WINE" notify.exe >/dev/null 2>&1 &
"$WINE" calendar.exe >/dev/null 2>&1 &
sleep 4
"$WINE" probe.exe launcher NotifyLauncher 'C:\notify.exe /toggle' 0 >/dev/null 2>&1 &
sleep 2
"$WINE" probe.exe launcher CalendarLauncher 'C:\calendar.exe --show' 1 >/dev/null 2>&1 &
sleep 3
P() { "$WINE" probe.exe "$@" 2>/dev/null | tr -d '\r'; }
# the person clicks the first launcher: their input is there, not with the panels' programs
xdotool mousemove 190 110 click 1; sleep 4
f=$(P fg)
[ "$(P lock)" = 1 ] || { echo "SKIP: this Wine has no foreground lock (wine-sg 9930)"; exit 77; }
[ "$f" = SgNotifyPanel ] && pass "Win+A's second sg-notify lets the running one's panel come forward ($f)" \
    || fail "after the second sg-notify the foreground is $f (want SgNotifyPanel)"
xdotool mousemove 190 220 click 1; sleep 4
f=$(P fg)
[ "$f" = SgCalendarFlyout ] && pass "the clock's second sg-calendar lets the running one's calendar come forward ($f)" \
    || fail "after the second sg-calendar the foreground is $f (want SgCalendarFlyout)"
exit $RC
