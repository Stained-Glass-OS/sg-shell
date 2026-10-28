#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for the taskbar clock's calendar flyout (src/sg-calendar.c; David 2026-10-28).
#  1. the month's six weeks: Sunday first and Monday first (the locale's first day), a leap February
#  2. --show (the taskbar clock's click) opens it on this month with today, above the taskbar
#     at the screen's right, inside the screen
#  3. Page Down shows the next month, Home this one again
#  4. a second click on the clock closes it, and Esc does too
# Mutants: SG_MUTANT_CALENDAR_SUNDAY (the locale's first day ignored),
#          SG_MUTANT_CALENDAR_NOTOGGLE (a second click leaves it open).
#   SG_WINE_DIR=/opt/wine-sg sh test/calendar-check.sh
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WD="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="$WD/bin/wine"; WINESERVER="$WD/bin/wineserver"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
command -v "$MINGW" >/dev/null || { echo "SKIP: no mingw"; exit 77; }
command -v Xvfb >/dev/null && command -v xdotool >/dev/null || { echo "SKIP: no Xvfb/xdotool"; exit 77; }
[ -x "$WINE" ] || { echo "SKIP: no wine at $WINE"; exit 77; }
T=$(mktemp -d /var/tmp/sg-calendar-check.XXXXXX)
unset DISPLAY WAYLAND_DISPLAY
export HOME="$T/home" WINEPREFIX="$T/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" WINESERVER
mkdir -p "$HOME"
Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/display")"
cleanup() { "$WINESERVER" -k 2>/dev/null; kill $XP 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
CF="-O2 -municode -mwindows -Wall -I$HERE/src"
L="-lshell32 -lgdi32 -luser32 -ladvapi32"
"$MINGW" $CF -o "$T/cal.exe" "$HERE/src/sg-calendar.c" $L || { echo "FAIL  build"; exit 1; }
"$MINGW" $CF -DSG_MUTANT_CALENDAR_SUNDAY -o "$T/mut-sunday.exe" "$HERE/src/sg-calendar.c" $L || exit 1
"$MINGW" $CF -DSG_MUTANT_CALENDAR_NOTOGGLE -o "$T/mut-notoggle.exe" "$HERE/src/sg-calendar.c" $L || exit 1
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1
"$WINESERVER" -w
DUMP="$WINEPREFIX/drive_c/cal.dump"
export SG_CALENDAR_DUMP='C:\cal.dump'
w() { timeout -s KILL 60 "$WINE" "$@" 2>/dev/null | tr -d '\r'; }
v() { { tr -d '\r' < "$DUMP"; } 2>/dev/null | sed -n "s/^$1 //p" | head -1; }
wait_for() { i=0; while [ $i -lt 60 ]; do [ "$(v "$1")" = "$2" ] && return 0; sleep 0.25; i=$((i + 1)); done; return 1; }

# 1. the grids
g=$(SG_CALENDAR_FIRSTDAY=0 w "$T/cal.exe" --dump 2026-10 | sed -n 's/^cells //p')
[ "$g" = "~27 ~28 ~29 ~30 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 ~1 ~2 ~3 ~4 ~5 ~6 ~7" ] \
    && pass "October 2026, Sunday first: Thursday the 1st after September's last four days" || fail "Sunday first: $g"
g=$(SG_CALENDAR_FIRSTDAY=1 w "$T/cal.exe" --dump 2026-10 | sed -n 's/^cells //p' | cut -d' ' -f1-4)
[ "$g" = "~28 ~29 ~30 1" ] && pass "...Monday first (the locale's first day of the week)" || fail "Monday first: $g"
g=$(SG_CALENDAR_FIRSTDAY=1 w "$T/mut-sunday.exe" --dump 2026-10 | sed -n 's/^cells //p' | cut -d' ' -f1-4)
[ "$g" != "~28 ~29 ~30 1" ] && pass "MUTANT CALENDAR_SUNDAY ignores it (gate catches it)" || fail "mutant SUNDAY: $g"
g=$(SG_CALENDAR_FIRSTDAY=0 w "$T/cal.exe" --dump 2024-02 | sed -n 's/^cells //p')
case " $g " in *" 28 29 ~1 "*) pass "February 2024 has its 29th" ;; *) fail "February 2024: $g" ;; esac

# 2. opened by the clock
"$WINE" "$T/cal.exe" --background >/dev/null 2>&1 &
sleep 4
w "$T/cal.exe" --show >/dev/null
if wait_for visible 1; then pass "a click on the clock opens it"; else fail "not shown: $(cat "$DUMP" 2>&1)"; fi
now=$(date +%Y-%m)
[ "$(v month)" = "$now" ] && [ "$(v today)" = "$(date +%Y-%m-%d)" ] && pass "...on this month, with today ($(v today))" \
    || fail "month $(v month) today $(v today)"
set -- $(v rect)
[ "${3:-0}" -le 1280 ] && [ "${4:-0}" -le 800 ] && [ "${1:-0}" -ge 900 ] && [ "${2:-0}" -ge 0 ] \
    && pass "...at the screen's right, above the bottom, inside the screen ($*)" || fail "rect: $*"

# 3. the months
win=$(xdotool search --name '^Calendar$' 2>/dev/null | tail -1)
xdotool windowactivate --sync "$win" >/dev/null 2>&1; xdotool windowfocus --sync "$win" >/dev/null 2>&1
xdotool key --window "$win" Next
next=$(date -d "$(date +%Y-%m-01) +1 month" +%Y-%m)
wait_for month "$next" && pass "Page Down: the next month ($next)" || fail "Page Down: month $(v month)"
xdotool key --window "$win" Home
wait_for month "$now" && pass "Home: this month again" || fail "Home: month $(v month)"

# 4. closing
w "$T/cal.exe" --show >/dev/null
wait_for visible 0 && pass "a second click on the clock closes it" || fail "second click: visible $(v visible)"
w "$T/cal.exe" --show >/dev/null; wait_for visible 1
xdotool windowactivate --sync "$win" >/dev/null 2>&1; xdotool key --window "$win" Escape
wait_for visible 0 && pass "Esc closes it" || fail "Esc: visible $(v visible)"
"$WINESERVER" -k 2>/dev/null; sleep 1

"$WINE" "$T/mut-notoggle.exe" --show >/dev/null 2>&1 &
wait_for visible 1
w "$T/mut-notoggle.exe" --show >/dev/null; sleep 2
[ "$(v visible)" = 1 ] && pass "MUTANT CALENDAR_NOTOGGLE stays open on a second click (gate catches it)" || fail "mutant NOTOGGLE closed"
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
