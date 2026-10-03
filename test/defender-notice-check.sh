#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# sg-defender-notice: SG Defender's "threat quarantined" popup. From a notice
# sg-defender left for this person it shows the file, the folder and what
# was found, above the tray (bottom right of the work area); Dismiss closes
# it; a second notice stacks above the first; an id that is not one (a path)
# shows nothing. Screenshot build/defender-notice.png.
#
# Needs wine-sg, Xvfb, xdotool; skips (77) without. SG_NOTICE_EXE runs another
# build (the mutation test); SG_WINE_DIR another Wine.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_NOTICE_EXE:-$HERE/build/sg-defender-notice64.exe}"
RC=0; DPY="${SG_NOTICE_DPY:-124}"; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for need in Xvfb xdotool; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
T=$(mktemp -d /var/tmp/sg-defender-notice.XXXXXX)
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

# a stand-in for Settings: ms-settings: leaves a mark (HKLM: Wine's HKCR is not merged with HKCU's)
OPENED=$(wine winepath -w "$T/opened.txt" 2>/dev/null | tr -d '\r')
wine reg add 'HKLM\Software\Classes\ms-settings' /v 'URL Protocol' /d '' /f >/dev/null 2>&1
wine reg add 'HKLM\Software\Classes\ms-settings\shell\open\command' /ve /d "C:\\windows\\system32\\cmd.exe /c echo %1> $OPENED" /f >/dev/null 2>&1
D="$T/defender"; mkdir -p "$D/notices/$(id -u)"
note() { printf '{"id": "%s", "name": "%s", "folder": "/home/jane/Downloads", "signature": "Win.Test.EICAR_HDB-1", "time": "2026-10-03T09:10:11"}' \
    "$1" "$2" > "$D/notices/$(id -u)/$1.json"; }
note 20261003-091011-abcdef free-game-setup.exe
note 20261003-091500-123456 cracked-tool.exe
export SG_DEFENDER_DIR
SG_DEFENDER_DIR=$(wine winepath -w "$D" 2>/dev/null | tr -d '\r')
SG_DEFENDER_NOTICE_DUMP=$(wine winepath -w "$T/dump.txt" 2>/dev/null | tr -d '\r') wine "$EXE" 20261003-091011-abcdef >/dev/null 2>&1 &
waitwin() { i=0; while [ $i -lt 40 ]; do n=$(xdotool search --onlyvisible --name '^SG Defender$' 2>/dev/null | wc -l); [ "$n" -ge "$1" ] && return 0; sleep 0.5; i=$((i + 1)); done; return 1; }
waitwin 1 && pass "the notice shows" || fail "no notice window"
tr -d '\r' < "$T/dump.txt" 2>/dev/null | grep -q "body: free-game-setup.exe (in Downloads) contained Win.Test.EICAR_HDB-1. It was moved to quarantine" \
    && pass "...naming the file, its folder and what was found" || fail "body: $(cat "$T/dump.txt" 2>/dev/null)"
W1=$(xdotool search --onlyvisible --name '^SG Defender$' | head -1)
eval "$(xdotool getwindowgeometry --shell "$W1")"; X1=$X; Y1=$Y; W1W=$WIDTH; W1H=$HEIGHT
[ $((X1 + W1W)) -gt 900 ] && [ $((Y1 + W1H)) -gt 650 ] && pass "...at the bottom right, above the tray ($X1,$Y1)" || fail "placed at $X1,$Y1"
wine "$EXE" 20261003-091500-123456 >/dev/null 2>&1 &
waitwin 2 && pass "a second notice shows too" || fail "no second notice"
W2=$(xdotool search --onlyvisible --name '^SG Defender$' | grep -vx "$W1" | head -1)
eval "$(xdotool getwindowgeometry --shell "$W2")"
[ "$Y" -lt "$Y1" ] && pass "...stacked above the first" || fail "second at $Y, first at $Y1"
command -v import >/dev/null && import -window root "$HERE/build/defender-notice.png" 2>/dev/null
# Dismiss: the right-hand button, near the bottom
xdotool mousemove $((X1 + W1W * 3 / 4)) $((Y1 + W1H - 20)) click 1; sleep 1.5
xdotool search --onlyvisible --name '^SG Defender$' 2>/dev/null | grep -qx "$W1" && fail "Dismiss did not close it" || pass "Dismiss closes it"
sleep 1; [ ! -e "$T/opened.txt" ] && pass "...without opening Settings" || fail "Dismiss opened Settings"
eval "$(xdotool getwindowgeometry --shell "$W2")"
xdotool mousemove $((X + WIDTH / 4)) $((Y + HEIGHT - 20)) click 1
i=0; while [ $i -lt 40 ] && [ ! -e "$T/opened.txt" ]; do sleep 0.5; i=$((i + 1)); done
grep -q "ms-settings:windowsdefender" "$T/opened.txt" 2>/dev/null && pass "Review opens Settings > Virus & threat protection" \
    || fail "Review: $(cat "$T/opened.txt" 2>/dev/null)"
wineserver -k 2>/dev/null; sleep 1
# a notice planted outside this person's notices, reached by a path for an id
printf '{"name": "planted.exe", "folder": "/x", "signature": "x"}' > "$D/planted.json"
wine "$EXE" '..\..\planted' >/dev/null 2>&1 & sleep 4
[ -z "$(xdotool search --onlyvisible --name '^SG Defender$' 2>/dev/null)" ] && pass "a path is not a notice id: nothing shown" || fail "showed a non-id"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
