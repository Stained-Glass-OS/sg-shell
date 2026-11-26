#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for the taskbar clock's calendar flyout (src/sg-calendar.c; David 2026-10-28, 2026-11-26:
# "when you click the time it should bring up a nice calendar just like in Windows").
#  1. the month's six weeks: Sunday first and Monday first (the locale's first day), a leap February
#  2. --show (a click on the clock) opens it on this month with today; a second click and Esc close it
#  3. against the bar whose clock was clicked (wine-sg 9950 names it): bottom, top, left, right, flush
#     with the bar and the screen's edge; another bar's click moves it there
#  4. the month's name: the year's months, the year's: the decade's; a year, then a month, back in
#  5. the keyboard: the arrows move a day / a week, Page Down a month (sliding), Home today,
#     Ctrl+Up the year, Enter back in
#  6. Windows 10's reveal: the cells' edges near the pointer lit; frosted (and not, without
#     "Transparency effects")
#  7. the Rounded taskbar: Windows 11's flyout, 12 px off the bar and the edge; its chevron folds
#     the month away, remembered
# Mutants: SG_MUTANT_CALENDAR_SUNDAY, _NOTOGGLE, _MAIN_ONLY, _NO_YEARVIEW, _ARROWS_DEAD, _NO_SLIDE,
#          _NO_REVEAL, _NO_FROST, _W10_ONLY (each fails its part).
#   SG_WINE_DIR=/opt/wine-sg sh test/calendar-check.sh
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WD="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="$WD/bin/wine"; WINESERVER="$WD/bin/wineserver"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
command -v "$MINGW" >/dev/null || { echo "SKIP: no mingw"; exit 77; }
for t in Xvfb xdotool import convert; do command -v $t >/dev/null || { echo "SKIP: no $t"; exit 77; }; done
[ -x "$WINE" ] || { echo "SKIP: no wine at $WINE"; exit 77; }
T=$(mktemp -d /var/tmp/sg-calendar-check.XXXXXX)
unset DISPLAY WAYLAND_DISPLAY
export HOME="$T/home" WINEPREFIX="$T/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" WINESERVER
mkdir -p "$HOME"
Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/display")"
cleanup() { "$WINESERVER" -k 2>/dev/null; kill $XP 2>/dev/null; [ -n "${KEEP:-}" ] && echo "kept $T" || rm -rf "$T"; }
trap cleanup EXIT INT TERM
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
CF="-O2 -municode -mwindows -Wall -I$HERE/src"
L="-lshell32 -lgdi32 -luser32 -ladvapi32"
"$MINGW" $CF -o "$T/cal.exe" "$HERE/src/sg-calendar.c" $L || { echo "FAIL  build"; exit 1; }
for m in SUNDAY NOTOGGLE MAIN_ONLY NO_YEARVIEW ARROWS_DEAD NO_SLIDE NO_REVEAL NO_FROST W10_ONLY; do
    "$MINGW" $CF -DSG_MUTANT_CALENDAR_$m -o "$T/mut-$m.exe" "$HERE/src/sg-calendar.c" $L || exit 1
done
"$MINGW" -O2 -municode -mwindows -o "$T/bar.exe" "$HERE/test/calendar-bar-probe.c" -luser32 -lgdi32 || exit 1
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1
"$WINESERVER" -w
DUMP="$WINEPREFIX/drive_c/cal.dump"
export SG_CALENDAR_DUMP='C:\cal.dump'
w() { timeout -s KILL 60 "$WINE" "$@" 2>/dev/null | tr -d '\r'; }
v() { { tr -d '\r' < "$DUMP"; } 2>/dev/null | sed -n "s/^$1 //p" | head -1; }
wait_for() { i=0; while [ $i -lt 60 ]; do [ "$(v "$1")" = "$2" ] && return 0; sleep 0.25; i=$((i + 1)); done; return 1; }
reg() { "$WINE" reg add "$1" /v "$2" /t REG_DWORD /d "$3" /f >/dev/null 2>&1; }
TB='HKCU\Software\Stained Glass\Taskbar'
PERS='HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize'
# a copy of EXE running hidden, as the session keeps one
start() { "$WINESERVER" -k 2>/dev/null; sleep 0.5; rm -f "$DUMP"; "$WINE" "$1" --background >/dev/null 2>&1 & sleep 3; }
# a bar at L T R B whose clock is clicked
bar() { "$WINE" "$T/bar.exe" "$@" >/dev/null 2>&1 & sleep 1.5; }
win() { xdotool search --onlyvisible --name '^Calendar$' 2>/dev/null | tail -1; }
key() { k=$(win); xdotool windowactivate --sync "$k" >/dev/null 2>&1; xdotool windowfocus --sync "$k" >/dev/null 2>&1;
        xdotool key --window "$k" "$@"; sleep 0.6; }
# a click at X Y of the flyout (its own coordinates)
fclick() { wait_for visible 1; set -- $(v rect) "$1" "$2"; [ $# = 6 ] || return 0; xdotool mousemove $(($1 + $5)) $(($2 + $6)); sleep 0.3; xdotool click 1; sleep 0.6; }
rect() { v rect | tr ' ' ,; }
d() { date -d "$1" +%Y-%m-%d; }

# ---- the scenarios: each prints what it saw (one line), for the build and its mutants ----
# 3. the bars: bottom, then top, left and right (each moves it), then a click without a bar closes it
sc_place() {
    start "$1"
    bar 0 760 1280 800; wait_for visible 1; b=$(rect)
    bar 0 0 1280 40; sleep 0.5; t=$(rect)
    bar 0 0 48 800; sleep 0.5; l=$(rect)
    bar 1232 0 1280 800; sleep 0.5; r=$(rect)
    echo "bottom=$b top=$t left=$l right=$r"
}
# 4. year / decade views
sc_views() {
    start "$1"
    bar 0 760 1280 800; wait_for visible 1
    fclick 60 142; a="$(v view):$(v title)"
    fclick 60 142; b="$(v view):$(v title)"
    fclick 137 211; c="$(v view):$(v title)"      # the decade's second: 2021
    fclick 218 211; e="$(v view):$(v month)"      # the year's third: March
    echo "$a|$b|$c|$e"
}
# 5. the keyboard
sc_keys() {
    start "$1"
    bar 0 760 1280 800; wait_for visible 1
    key Right; a=$(v focus)
    key Down; b=$(v focus)
    key Next; c="$(v month)/$(v focus)"
    sleep 0.5; s=$(v slides)
    key Home; e="$(v month)/$(v focus)"
    key ctrl+Up; f=$(v view)
    key Return; g=$(v view)
    echo "$a $b $c $e $f $g slides=$s"
}
# 6. the reveal: a neighbour's edge lit by the pointer over a cell; the frost, with and without
sc_reveal() {
    start "$1"
    bar 0 760 1280 800; wait_for visible 1
    set -- $(v rect)
    # the pointer in the middle of the third week's Tuesday (cell 16); cell 17's left edge, 25 px away
    xdotool mousemove $(($1 + 17 + 46 * 2 + 23)) $(($2 + 204 + 80 + 20)); sleep 0.8
    import -window root "$T/reveal.png" 2>/dev/null
    edge=$(convert "$T/reveal.png" -format '%[fx:int(255*p{'$(($1 + 17 + 46 * 3 + 1))','$(($2 + 204 + 80 + 20))'}.r)]' info: 2>/dev/null)
    far=$(convert "$T/reveal.png" -format '%[fx:int(255*p{'$(($1 + 17 + 46 * 6 + 1))','$(($2 + 204 + 200 + 20))'}.r)]' info: 2>/dev/null)
    echo "edge=$edge far=$far"
}
sc_frost() {
    start "$1"
    bar 0 760 1280 800; wait_for visible 1; a=$(v acrylic)
    reg "$PERS" EnableTransparency 0
    w "$1" --show; wait_for visible 0; bar 0 760 1280 800; wait_for visible 1; b=$(v acrylic)
    reg "$PERS" EnableTransparency 1
    echo "$a/$b"
}
# 7. the Rounded taskbar
sc_w11() {
    reg "$TB" Look 1
    start "$1"
    bar 0 760 1280 800; wait_for visible 1; a="$(v look) $(rect)"
    fclick 300 24; b="folded=$(v folded) $(rect)"
    f=$("$WINE" reg query 'HKCU\Software\Stained Glass\Calendar' /v Folded 2>/dev/null | tr -d '\r' | awk '/Folded/ {print $3}')
    fclick 300 24; c="folded=$(v folded)"
    reg "$TB" Look 0
    echo "$a | $b reg=$f | $c"
}

# 1. the grids
g=$(SG_CALENDAR_FIRSTDAY=0 w "$T/cal.exe" --dump 2026-10 | sed -n 's/^cells //p')
[ "$g" = "~27 ~28 ~29 ~30 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 ~1 ~2 ~3 ~4 ~5 ~6 ~7" ] \
    && pass "October 2026, Sunday first: Thursday the 1st after September's last four days" || fail "Sunday first: $g"
g=$(SG_CALENDAR_FIRSTDAY=1 w "$T/cal.exe" --dump 2026-10 | sed -n 's/^cells //p' | cut -d' ' -f1-4)
[ "$g" = "~28 ~29 ~30 1" ] && pass "...Monday first (the locale's first day of the week)" || fail "Monday first: $g"
g=$(SG_CALENDAR_FIRSTDAY=1 w "$T/mut-SUNDAY.exe" --dump 2026-10 | sed -n 's/^cells //p' | cut -d' ' -f1-4)
[ "$g" != "~28 ~29 ~30 1" ] && pass "MUTANT CALENDAR_SUNDAY ignores it (gate catches it)" || fail "mutant SUNDAY: $g"
g=$(SG_CALENDAR_FIRSTDAY=0 w "$T/cal.exe" --dump 2024-02 | sed -n 's/^cells //p')
case " $g " in *" 28 29 ~1 "*) pass "February 2024 has its 29th" ;; *) fail "February 2024: $g" ;; esac

# 2. opened by the clock (an older taskbar's: no bar named -- the main one, here none: the work area's corner)
start "$T/cal.exe"
w "$T/cal.exe" --show >/dev/null
if wait_for visible 1; then pass "a click on the clock opens it"; else fail "not shown: $(cat "$DUMP" 2>&1)"; fi
now=$(date +%Y-%m)
[ "$(v month)" = "$now" ] && [ "$(v today)" = "$(date +%Y-%m-%d)" ] && [ "$(v view)" = month ] &&
    pass "...on this month, with today ($(v today))" || fail "month $(v month) today $(v today) view $(v view)"
[ "$(rect)" = "920,292,1280,800" ] && pass "...in the screen's bottom right corner, flush, without a taskbar ($(rect))" || fail "rect: $(rect)"
[ "$(v look)" = w10 ] && pass "...Windows 10's flyout (the Classic taskbar)" || fail "look: $(v look)"
w "$T/cal.exe" --show >/dev/null
wait_for visible 0 && pass "a second click on the clock closes it" || fail "second click: visible $(v visible)"
w "$T/cal.exe" --show >/dev/null; wait_for visible 1
key Escape
wait_for visible 0 && pass "Esc closes it" || fail "Esc: visible $(v visible)"
start "$T/mut-NOTOGGLE.exe"
w "$T/mut-NOTOGGLE.exe" --show >/dev/null; wait_for visible 1
w "$T/mut-NOTOGGLE.exe" --show >/dev/null; sleep 2
[ "$(v visible)" = 1 ] && pass "MUTANT CALENDAR_NOTOGGLE stays open on a second click (gate catches it)" || fail "mutant NOTOGGLE closed"

# 3. the bars
want="bottom=920,252,1280,760 top=920,40,1280,548 left=48,292,408,800 right=872,292,1232,800"
got=$(sc_place "$T/cal.exe")
[ "$got" = "$want" ] && pass "against the clicked bar, flush: bottom, top, left, right; each bar's click moves it ($got)" ||
    fail "bars: $got (want $want)"
[ "$(v visible)" = 1 ] && [ "$(v edge)" = 2 ] && pass "...and it stays open when another bar's clock is clicked" || fail "after moving: visible $(v visible) edge $(v edge)"
got=$(sc_place "$T/mut-MAIN_ONLY.exe")
[ "$got" != "$want" ] && pass "MUTANT CALENDAR_MAIN_ONLY opens at the main corner whatever bar (gate catches it)" || fail "mutant MAIN_ONLY: $got"

# 4. the views
y=$(date +%Y); dec=$((y - y % 10))
want="year:$y|decade:$dec - $((dec + 9))|year:$((dec + 1))|month:$((dec + 1))-03"
got=$(sc_views "$T/cal.exe")
[ "$got" = "$want" ] && pass "the month's name: the year's months; the year's: the decade; a year, then March, back in ($got)" ||
    fail "views: $got (want $want)"
got=$(sc_views "$T/mut-NO_YEARVIEW.exe")
[ "$got" != "$want" ] && pass "MUTANT CALENDAR_NO_YEARVIEW (gate catches it)" || fail "mutant NO_YEARVIEW: $got"

# 5. the keyboard
t=$(date +%Y-%m-%d)
got=$(sc_keys "$T/cal.exe")
set -- $got - - - - - - -
[ "$1" = "$(d "$t +1 day")" ] && [ "$2" = "$(d "$t +8 days")" ] && pass "Right: tomorrow, Down: a week on ($1 $2)" || fail "arrows: $got"
[ "${3%/*}" = "$(date -d "$(date -d "$(d "$t +8 days")" +%Y-%m-01) +1 month" +%Y-%m)" ] &&
    pass "Page Down: the month after the focus's ($3)" || fail "Page Down: $3"
[ "$4" = "$now/$t" ] && pass "Home: this month, today ($4)" || fail "Home: $4"
[ "$5" = year ] && [ "$6" = month ] && pass "Ctrl+Up: the year's months; Enter: back in" || fail "Ctrl+Up/Enter: $5 $6"
s=${7#slides=}
[ "${s:-0}" -ge 3 ] && pass "...the month slides in ($s frames)" || fail "no slide: $7"
got=$(sc_keys "$T/mut-ARROWS_DEAD.exe"); set -- $got - -
[ "$1" != "$(d "$t +1 day")" ] && pass "MUTANT CALENDAR_ARROWS_DEAD (gate catches it)" || fail "mutant ARROWS_DEAD: $got"
got=$(sc_keys "$T/mut-NO_SLIDE.exe"); s=$(echo "$got" | sed -n 's/.*slides=//p')
[ "${s:-0}" -lt 3 ] && pass "MUTANT CALENDAR_NO_SLIDE (gate catches it: $s frames)" || fail "mutant NO_SLIDE: $got"

# 6. the reveal and the frost
got=$(sc_reveal "$T/cal.exe"); set -- $(echo "$got" | tr '=' ' ')
[ "${2:-0}" -ge 70 ] && [ "${4:-255}" -le 40 ] && pass "the reveal: a neighbour's edge lit near the pointer, not far off ($got)" ||
    fail "reveal: $got"
got=$(sc_reveal "$T/mut-NO_REVEAL.exe"); set -- $(echo "$got" | tr '=' ' ')
[ "${2:-0}" -lt 70 ] && pass "MUTANT CALENDAR_NO_REVEAL (gate catches it: $got)" || fail "mutant NO_REVEAL: $got"
got=$(sc_frost "$T/cal.exe")
[ "$got" = "85/0" ] && pass "frosted (85), and not without Transparency effects ($got)" || fail "frost: $got"
got=$(sc_frost "$T/mut-NO_FROST.exe")
[ "$got" != "85/0" ] && pass "MUTANT CALENDAR_NO_FROST (gate catches it: $got)" || fail "mutant NO_FROST: $got"

# 7. the Rounded taskbar
want="w11 934,346,1268,748 | folded=1 934,700,1268,748 reg=0x1 | folded=0"
got=$(sc_w11 "$T/cal.exe")
[ "$got" = "$want" ] && pass "the Rounded taskbar: Windows 11's flyout, 12 px off; the chevron folds it, remembered ($got)" ||
    fail "Rounded: $got (want $want)"
got=$(sc_w11 "$T/mut-W10_ONLY.exe")
[ "$got" != "$want" ] && pass "MUTANT CALENDAR_W10_ONLY (gate catches it)" || fail "mutant W10_ONLY: $got"

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
