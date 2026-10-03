#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Settings > Update & Security > Virus & threat protection (SG Defender):
# ms-settings:windowsdefender opens it; it shows sg-defender's status.json
# (on, how many checked, the definitions) and what was quarantined from this
# person's files (notices/<uid>/), and its switch asks sg-admind for
# "defender on|off" (test/admind-check.sh). Read back from SG_SETTINGS_DUMP.
#
# Needs wine-sg and Xvfb; skips (77) without. SG_SETTINGS_EXE runs another
# build (the mutation test); SG_WINE_DIR another Wine.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_SETTINGS_EXE:-$HERE/build/sg-settings64.exe}"
RC=0; DPY="${SG_DEFENDER_DPY:-123}"; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
command -v Xvfb >/dev/null || { echo "SKIP: Xvfb missing"; exit 77; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-defender-page.XXXXXX)
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

# what sg-defender leaves: its status, and one detection in this person's notices
D="$T/defender"; mkdir -p "$D/notices/$(id -u)"
cat > "$D/status.json" <<'J'
{
 "enabled": true,
 "engine": "ClamAV 1.4.3/27780/Fri Oct  3 08:24:01 2026",
 "scanned": 42,
 "found": 1,
 "since": "2026-10-03T08:00:00"
}
J
printf '{"id": "x1", "name": "free-game-setup.exe", "folder": "/home/jane/Downloads", "signature": "Win.Test.EICAR_HDB-1", "time": "2026-10-03T09:10:11"}' \
    > "$D/notices/$(id -u)/x1.json"
export SG_DEFENDER_DIR
SG_DEFENDER_DIR=$(wine winepath -w "$D" 2>/dev/null | tr -d '\r')

show() {   # show URI: open Settings there, wait for its dump, print it
    rm -f "$T/dump.txt"
    SG_SETTINGS_DUMP=$(wine winepath -w "$T/dump.txt" 2>/dev/null | tr -d '\r') wine "$EXE" "$1" >/dev/null 2>&1 &
    i=0; while [ $i -lt 60 ] && ! grep -q "Found and quarantined" "$T/dump.txt" 2>/dev/null; do sleep 1; i=$((i + 1)); done
    sleep 1; cat "$T/dump.txt" 2>/dev/null | tr -d '\r'
    wineserver -k 2>/dev/null; sleep 1
}
out=$(show ms-settings:windowsdefender)
printf '%s\n' "$out" | grep -q "Virus & threat protection" && pass "ms-settings:windowsdefender opens Virus & threat protection" \
    || { fail "the page did not open"; printf '%s\n' "$out" | head -20; }
printf '%s\n' "$out" | grep -q "On. 42 programs checked" && pass "it reads sg-defender's status (on, 42 checked)" || fail "no status"
printf '%s\n' "$out" | grep -q "Definitions: ClamAV 1.4.3/27780" && pass "...and the definitions in use" || fail "no definitions"
printf '%s\n' "$out" | grep -q "free-game-setup.exe -- Win.Test.EICAR_HDB-1" && pass "lists what was quarantined from this person's files" \
    || fail "the detection is not listed"
printf '%s\n' "$out" | grep -q "state=1 .*: Scan downloaded programs" && pass "the switch shows it on" || fail "the switch is not on"

sed -i 's/"enabled": true/"enabled": false/' "$D/status.json"; rm -f "$D/notices/$(id -u)/x1.json"
out=$(show ms-settings:windowsdefender)
printf '%s\n' "$out" | grep -q "Off: downloaded programs are not scanned" && pass "turned off, it says so" || fail "off not shown"
printf '%s\n' "$out" | grep -q "state=0 .*: Scan downloaded programs" && pass "...and its switch off" || fail "the switch is not off"
printf '%s\n' "$out" | grep -q "Nothing has been found in your files" && pass "...and nothing found when nothing was" || fail "empty list wrong"

[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
