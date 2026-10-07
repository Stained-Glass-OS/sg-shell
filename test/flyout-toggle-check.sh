#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# A flyout opened from the taskbar closes on a second click of its button, as
# on Windows (David, 2026-10-07: "first click open, 2nd click on the open item
# closes it"). The second click's press made the taskbar active, the flyout
# hid on losing activation, and the release reached the icon as a click to
# open it again -- with a person's click (held longer than 200 ms) Volume and
# Notifications stayed open. src/sg-flyout.h: a press on the flyout's own icon
# that hid it keeps the following click from reopening it.
# Here: Start, Volume and Notifications on a shell desktop, each clicked open
# and then again -- quick clicks and clicks held 0.6 s -- a third click opens
# it again, and with Volume open a click on Notifications' icon opens that one.
#
#   sh test/flyout-toggle-check.sh
#   SG_WINE=<wine> SG_WINESERVER=<wineserver>, SG_VOLUME_EXE=, SG_NOTIFY_EXE=
# Mutant: SG_MUTANT_FLYOUT_REOPENS (make test-flyout-toggle builds and runs it).
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="${SG_WINE:-$WINE_DIR/bin/wine}"
export WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER=$(dirname "$WINE")/server/wineserver
VOLUME="${SG_VOLUME_EXE:-$HERE/build/sg-volume64.exe}"
NOTIFY="${SG_NOTIFY_EXE:-$HERE/build/sg-notify64.exe}"
START="$HERE/build/sg-start64.exe"
RC=0; XP=
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in Xvfb xdotool x86_64-w64-mingw32-gcc; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE" ] || { echo "SKIP: no wine at $WINE"; exit 77; }
for f in "$VOLUME" "$NOTIFY" "$START"; do [ -f "$f" ] || { echo "SKIP: $f not built"; exit 77; }; done
T=$(mktemp -d /var/tmp/sg-flyout-toggle.XXXXXX)
mkdir -p "$T/home" "$T/xdg"; chmod 700 "$T/xdg"
export HOME="$T/home" XDG_RUNTIME_DIR="$T/xdg" WINEPREFIX="$T/pfx" WINEDEBUG=-all
export WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d;winewayland.drv=d"
unset WAYLAND_DISPLAY
trap '"$WINESERVER" -k 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; rm -rf "$T"' EXIT INT TERM
x86_64-w64-mingw32-gcc -O2 -municode -o "$T/probe.exe" "$HERE/test/flyout-probe.c" -lshell32 || { fail "probe did not build"; exit 1; }
W=1280; H=720
Xvfb -displayfd 3 -screen 0 ${W}x${H}x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/display")"
timeout 300 "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w
for r in "$HERE"/theme/*.reg; do "$WINE" regedit /S "$("$WINE" winepath -w "$r" 2>/dev/null)" >/dev/null 2>&1; done
"$WINE" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d ${W}x${H} /f >/dev/null 2>&1
"$WINESERVER" -w
"$WINE" explorer /desktop=shell,${W}x${H} >/dev/null 2>&1 &
i=0; while [ $i -lt 60 ] && ! xdotool search --name 'shell - Wine Desktop' >/dev/null 2>&1; do sleep 0.5; i=$((i + 1)); done
sleep 3
for e in "$START" "$VOLUME" "$NOTIFY"; do "$WINE" "$e" >/dev/null 2>&1 & done
sleep 8
P() { "$WINE" "$T/probe.exe" "$@" 2>/dev/null | tr -d '\r'; }
clk() { xdotool mousemove "$1" "$2" mousedown 1; sleep "$3"; xdotool mouseup 1; sleep 1.5; }
away() { xdotool mousemove 800 150 click 1; sleep 1; }   # the empty desktop: clear of Start (708 px wide) and the Notifications panel (the right edge)
centre() { r=$(P rect "$1"); [ -n "$r" ] && [ "$r" != none ] || return 1; set -- $r; echo "$(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 ))"; }
VP=$(centre SgVolumeTray) || { fail "no Volume icon"; exit 1; }
NP=$(centre SgNotifyTray) || { fail "no Notifications icon"; exit 1; }
for item in "Start:20 $((H - 20)):SgStartWindow" "Volume:$VP:SgVolumeFlyout" "Notifications:$NP:SgNotifyPanel"; do
    n=${item%%:*}; rest=${item#*:}; xy=${rest%%:*}; fly=${rest#*:}
    for hold in 0.05 0.6; do
        # shellcheck disable=SC2086
        clk $xy $hold; a=$(P vis "$fly")
        # shellcheck disable=SC2086
        clk $xy $hold; b=$(P vis "$fly")
        # shellcheck disable=SC2086
        clk $xy $hold; c=$(P vis "$fly")
        away
        [ "$a$b$c" = 101 ] && pass "$n: a click opens it, a second closes it, a third opens it again (held ${hold} s)" \
            || fail "$n, held ${hold} s: open after 1st/2nd/3rd click: $a/$b/$c (want 1/0/1)"
    done
done
# shellcheck disable=SC2086
clk $VP 0.3; # shellcheck disable=SC2086
clk $NP 0.3
[ "$(P vis SgVolumeFlyout)$(P vis SgNotifyPanel)" = 01 ] && pass "with Volume open, a click on Notifications' icon opens Notifications (and Volume closes)" \
    || fail "Volume then Notifications: $(P vis SgVolumeFlyout)/$(P vis SgNotifyPanel) (want 0/1)"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
