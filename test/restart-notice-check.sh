#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015,SC1091  # pass/fail one-liners: both only print
# sg-restart-notice: "Restart to finish updating". From /run/reboot-required
# (here a stand-in) it names what needs the restart, above the tray (bottom
# right of the work area), and keeps it in the notification centre's history
# (sg-notify); "Later" closes it without restarting; "Restart now" restarts
# (here: SG_RESTART_NOTICE_REBOOT's mark). Without reboot-required it shows
# nothing. Screenshot build/restart-notice.png.
#
# Needs wine-sg, Xvfb, xdotool; skips (77) without. SG_NOTICE_EXE runs another
# build: one with SG_MUTANT_RESTART_BUTTON (Restart now does nothing) must fail.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_NOTICE_EXE:-$HERE/build/sg-restart-notice64.exe}"
RC=0; DPY="${SG_NOTICE_DPY:-127}"; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for need in Xvfb xdotool; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
T=$(mktemp -d /var/tmp/sg-restart-notice.XXXXXX)
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM
Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=,winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1
w() { wine winepath -w "$1" 2>/dev/null | tr -d '\r'; }
export SG_RESTART_FILE SG_RESTART_NOTICE_DUMP SG_RESTART_NOTICE_REBOOT
SG_RESTART_FILE=$(w "$T/reboot-required"); SG_RESTART_NOTICE_DUMP=$(w "$T/dump.txt"); SG_RESTART_NOTICE_REBOOT=$(w "$T/rebooted")
waitwin() { i=0; while [ $i -lt 40 ]; do [ -n "$(xdotool search --onlyvisible --name '^Restart to finish$' 2>/dev/null)" ] && return 0; sleep 0.5; i=$((i + 1)); done; return 1; }

# nothing to restart for: nothing shown
wine "$EXE" >/dev/null 2>&1 & sleep 4
[ -z "$(xdotool search --onlyvisible --name '^Restart to finish$' 2>/dev/null)" ] && pass "no reboot-required: nothing shown" || fail "shown without reboot-required"
wineserver -k 2>/dev/null; sleep 1

echo '*** System restart required: Surface touch and pen support ***' > "$T/reboot-required"
wine "$EXE" >/dev/null 2>&1 &
waitwin && pass "the notice shows" || fail "no notice window"
tr -d '\r' < "$T/dump.txt" 2>/dev/null | grep -q "body: Surface touch and pen support was installed and starts working when this PC restarts." \
    && pass "...naming what needs the restart" || fail "body: $(cat "$T/dump.txt" 2>/dev/null)"
W1=$(xdotool search --onlyvisible --name '^Restart to finish$' | head -1)
eval "$(xdotool getwindowgeometry --shell "$W1")"
[ $((X + WIDTH)) -gt 900 ] && [ $((Y + HEIGHT)) -gt 650 ] && pass "...at the bottom right, above the tray ($X,$Y)" || fail "placed at $X,$Y"
wine reg query 'HKCU\Software\Stained Glass\Notifications\History\0' /v Title 2>/dev/null | grep -q 'Restart to finish updating' \
    && pass "...kept in the notification centre's history" || fail "not in the notification history"
command -v import >/dev/null && import -window root "$HERE/build/restart-notice.png" 2>/dev/null
# Later: the right-hand button, near the bottom
xdotool mousemove $((X + WIDTH * 3 / 4)) $((Y + HEIGHT - 20)) click 1; sleep 1.5
xdotool search --onlyvisible --name '^Restart to finish$' 2>/dev/null | grep -qx "$W1" && fail "Later did not close it" || pass "Later closes it"
[ ! -e "$T/rebooted" ] && pass "...without restarting" || fail "Later restarted"
# Restart now: the left-hand button
wine "$EXE" >/dev/null 2>&1 &
waitwin || fail "no second notice"
W2=$(xdotool search --onlyvisible --name '^Restart to finish$' | head -1)
eval "$(xdotool getwindowgeometry --shell "$W2")"
xdotool mousemove $((X + WIDTH / 4)) $((Y + HEIGHT - 20)) click 1
i=0; while [ $i -lt 20 ] && [ ! -e "$T/rebooted" ]; do sleep 0.5; i=$((i + 1)); done
grep -q reboot "$T/rebooted" 2>/dev/null && pass "Restart now restarts" || fail "Restart now did not restart"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
