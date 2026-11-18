#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015,SC1091  # pass/fail one-liners: both only print
# sg-bootspace-notice (sg-restart-notice.c built with SG_BOOTSPACE): "An
# update could not be installed". From sg-boot-space's notice (here a
# stand-in for /var/lib/stained-glass/notices/boot-space) it names the kernel
# left out and the one the PC keeps starting, above the tray, kept in the
# notification centre's history; "Dismiss" closes it; "Settings" opens
# Settings > Update (here: SG_RESTART_NOTICE_REBOOT's mark). Without the
# notice it shows nothing. Screenshot build/bootspace-notice.png.
#
# Needs wine-sg, Xvfb, xdotool; skips (77) without. SG_NOTICE_EXE runs another
# build: one with SG_MUTANT_RESTART_BUTTON (Settings does nothing) must fail.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_NOTICE_EXE:-$HERE/build/sg-bootspace-notice64.exe}"
RC=0; DPY="${SG_NOTICE_DPY:-127}"; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for need in Xvfb xdotool; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
T=$(mktemp -d /var/tmp/sg-bootspace-notice.XXXXXX)
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
export SG_BOOTSPACE_FILE SG_RESTART_NOTICE_DUMP SG_RESTART_NOTICE_REBOOT
SG_BOOTSPACE_FILE=$(w "$T/boot-space"); SG_RESTART_NOTICE_DUMP=$(w "$T/dump.txt"); SG_RESTART_NOTICE_REBOOT=$(w "$T/rebooted")
waitwin() { i=0; while [ $i -lt 40 ]; do [ -n "$(xdotool search --onlyvisible --name '^Update could not be installed$' 2>/dev/null)" ] && return 0; sleep 0.5; i=$((i + 1)); done; return 1; }

# no notice: nothing shown
wine "$EXE" >/dev/null 2>&1 & sleep 4
[ -z "$(xdotool search --onlyvisible --name '^Update could not be installed$' 2>/dev/null)" ] && pass "no notice: nothing shown" || fail "shown without the notice"
wineserver -k 2>/dev/null; sleep 1

printf 'kernel=6.12.118+deb13-amd64\nrunning=6.12.117+deb13-amd64\nneed=1\nfree=0\n' > "$T/boot-space"
wine "$EXE" >/dev/null 2>&1 &
waitwin && pass "the notice shows" || fail "no notice window"
tr -d '\r' < "$T/dump.txt" 2>/dev/null | grep -q "body: There is not enough room on the boot partition for the new Linux kernel 6.12.118+deb13-amd64. This PC keeps starting with Linux 6.12.117+deb13-amd64, and tries again at each start." \
    && pass "...naming the kernel left out and the one still started" || fail "body: $(cat "$T/dump.txt" 2>/dev/null)"
tr -d '\r' < "$T/dump.txt" 2>/dev/null | grep -q "buttons: Settings, Dismiss" && pass "...with Settings and Dismiss" || fail "buttons: $(cat "$T/dump.txt" 2>/dev/null)"
W1=$(xdotool search --onlyvisible --name '^Update could not be installed$' | head -1)
eval "$(xdotool getwindowgeometry --shell "$W1")"
[ $((X + WIDTH)) -gt 900 ] && [ $((Y + HEIGHT)) -gt 650 ] && pass "...at the bottom right, above the tray ($X,$Y)" || fail "placed at $X,$Y"
wine reg query 'HKCU\Software\Stained Glass\Notifications\History\00000000' /v Title 2>/dev/null | grep -q 'An update could not be installed' \
    && pass "...kept in the notification centre's history" || fail "not in the notification history"
command -v import >/dev/null && import -window root "$HERE/build/bootspace-notice.png" 2>/dev/null
# Dismiss: the right-hand button, near the bottom
xdotool mousemove $((X + WIDTH * 3 / 4)) $((Y + HEIGHT - 20)) click 1; sleep 1.5
xdotool search --onlyvisible --name '^Update could not be installed$' 2>/dev/null | grep -qx "$W1" && fail "Dismiss did not close it" || pass "Dismiss closes it"
[ ! -e "$T/rebooted" ] && pass "...without opening Settings" || fail "Dismiss opened Settings"
# Settings: the left-hand button
wine "$EXE" >/dev/null 2>&1 &
waitwin || fail "no second notice"
W2=$(xdotool search --onlyvisible --name '^Update could not be installed$' | head -1)
eval "$(xdotool getwindowgeometry --shell "$W2")"
xdotool mousemove $((X + WIDTH / 4)) $((Y + HEIGHT - 20)) click 1
i=0; while [ $i -lt 20 ] && [ ! -e "$T/rebooted" ]; do sleep 0.5; i=$((i + 1)); done
grep -q settings "$T/rebooted" 2>/dev/null && pass "Settings opens Settings > Update" || fail "Settings did nothing"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
