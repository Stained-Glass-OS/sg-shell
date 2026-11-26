#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Settings > Update & Security > Virus & threat protection (SG Defender), as
# Windows Security's page: ms-settings:windowsdefender opens it; it shows
# sg-defender's status.json (on, how many checked, the definitions), the
# scans (Quick, Full, Custom: sg-defender-scan, run as this person; the last
# ones from notices/<uid>/scans.json, the buttons off while one runs),
# Protection history (what was found in this person's files, how: on
# download, at start, a script; Restore and Delete for what was kept),
# Real-time protection (sg-admind's defender-realtime: off for a while), SG
# Defender on/off (sg-admind's defender), and Exclusions (an elevated window:
# sg-admind's defender-exclusion). Read back from SG_SETTINGS_DUMP; buttons
# clicked with xdotool; sg-admind in test mode carries out the requests.
#
# Needs wine-sg and Xvfb; skips (77) without. SG_SETTINGS_EXE runs another
# build (the mutation test); SG_WINE_DIR another Wine.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_SETTINGS_EXE:-$HERE/build/sg-settings64.exe}"
RC=0; DPY="${SG_DEFENDER_DPY:-123}"; XP=""; LOOP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
command -v Xvfb >/dev/null || { echo "SKIP: Xvfb missing"; exit 77; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-defender-page.XXXXXX)
# shellcheck disable=SC2317
cleanup() {
    [ -n "$LOOP" ] && kill "$LOOP" 2>/dev/null
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM
Xvfb ":$DPY" -screen 0 1024x900x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=,winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1

# what sg-defender leaves: its status, two detections in this person's notices, their scans
D="$T/defender"; N="$D/notices/$(id -u)"; mkdir -p "$N"
cat > "$D/status.json" <<'J'
{
 "enabled": true,
 "realtime": true,
 "realtime_back": "",
 "engine": "ClamAV 1.4.3/27780/Fri Oct  3 08:24:01 2026",
 "scanned": 42,
 "found": 2,
 "since": "2026-10-03T08:00:00"
}
J
printf '{"id": "20261003-091011-abcdef", "name": "free-game-setup.exe", "folder": "/home/jane/Downloads", "signature": "Win.Test.EICAR_HDB-1", "time": "2026-10-03T09:10:11", "how": "image", "action": "quarantined"}' \
    > "$N/x1.json"
printf '{"id": "20261003-091500-fedcba", "name": "PowerShell_C:\\\\x.ps1 script.ps1", "folder": "", "signature": "Eicar-Test-Signature", "time": "2026-10-03T09:15:00", "how": "amsi", "action": "blocked"}' \
    > "$N/x2.json"
cat > "$N/scans.json" <<'J'
[
 {
  "id": "20261125-113837-186ac7",
  "type": "quick",
  "state": "done",
  "start": "2026-11-25T11:38:37",
  "end": "2026-11-25T11:38:40",
  "files": 6,
  "skipped": 0,
  "found": 5,
  "threats": [
   {"id": "20261125-113838-aaaaaa", "name": "Updater.exe", "folder": "C:/x", "signature": "Eicar-Test-Signature", "removed": true}
  ]
 }
]
J
export SG_DEFENDER_DIR
SG_DEFENDER_DIR=$(wine winepath -w "$D" 2>/dev/null | tr -d '\r')
# the scan tool: records how it was run, and says a quick scan is running
cat > "$T/scan-tool" <<EOF
#!/bin/sh
echo "\$*" >> "$T/scan-args"
printf '[{"id": "20261125-120000-111111", "type": "%s", "state": "running", "start": "2026-11-25T12:00:00", "end": "", "files": 17, "skipped": 0, "found": 0, "threats": []}]' "\$1" > "$N/scans.json"
echo "STARTED 20261125-120000-111111"
EOF
chmod +x "$T/scan-tool"
export SG_DEFENDER_SCAN_TOOL="$T/scan-tool"
# sg-admind, in test mode, carrying out what the elevated half asks
S="$T/spool"; mkdir -p "$S/requests" "$S/replies" "$D/quarantine" "$T/dl" "$T/run"; chmod 700 "$S/requests"
export SG_ADMIN_TEST=1 SG_ADMIN_SPOOL="$S" SG_ADMIN_SYSTEM_UID="$(id -u)" SG_ADMIN_DEFENDER_STATE="$D" \
    SG_ADMIN_DEFENDER_RUNTIME="$T/run" SG_ADMIN_DEFENDER_EXCLUSIONS="$T/exclusions" SG_ADMIN_DEFENDER_CONF="$T/defender.conf"
( while :; do for f in "$S"/requests/*.req; do [ -e "$f" ] && python3 "$HERE/admin/sg-admind" 2>>"$T/admind.log"; break; done; sleep 0.2; done ) &
LOOP=$!

open_page() {   # wait for the page's dump, until it shows MARK
    rm -f "$T/dump.txt"
    SG_SETTINGS_DUMP=$(wine winepath -w "$T/dump.txt" 2>/dev/null | tr -d '\r') wine "$EXE" ms-settings:windowsdefender >/dev/null 2>&1 &
    i=0; while [ $i -lt 60 ] && ! grep -q "${1:-Protection history}" "$T/dump.txt" 2>/dev/null; do sleep 1; i=$((i + 1)); done
    sleep 1
}
dump() { tr -d '\r' < "$T/dump.txt" 2>/dev/null; }
click() {   # the control whose dump line ends with ": TEXT"
    at=$(dump | grep ": $1\$" | sed -n 's/.* at=\([0-9]*\),\([0-9]*\).*/\1 \2/p' | head -1)
    [ -n "$at" ] || { echo "      (no control '$1')"; return 1; }
    # shellcheck disable=SC2086  # x y
    xdotool mousemove $at click 1
}
wait_dump() {   # until the page (dumped again after each rebuild) shows PATTERN
    i=0; while [ $i -lt 30 ] && ! dump | grep -q "$1"; do sleep 0.5; i=$((i + 1)); done
    dump | grep -q "$1"
}
stop() { wineserver -k 2>/dev/null; sleep 1; }

open_page
out=$(dump)
printf '%s\n' "$out" | grep -q "Virus & threat protection" && pass "ms-settings:windowsdefender opens Virus & threat protection" \
    || { fail "the page did not open"; printf '%s\n' "$out" | head -20; }
printf '%s\n' "$out" | grep -q "On. 42 programs checked" && pass "it reads sg-defender's status (on, 42 checked)" || fail "no status"
printf '%s\n' "$out" | grep -q "Definitions: ClamAV 1.4.3/27780" && pass "...and the definitions in use" || fail "no definitions"
printf '%s\n' "$out" | grep -q "Quick scan, 2026-11-25 11:38: 6 files scanned, 5 threats found." && pass "the last scan, from this person's scans" \
    || fail "no scan history: $(printf '%s\n' "$out" | grep -i scan | head -5)"
printf '%s\n' "$out" | grep -q "free-game-setup.exe -- Win.Test.EICAR_HDB-1 (stopped when it started" \
    && pass "Protection history: a program stopped at its start" || fail "the detection is not listed"
printf '%s\n' "$out" | grep -q "PowerShell.* -- Eicar-Test-Signature (script stopped" \
    && pass "...and script text AMSI stopped" || fail "the AMSI detection is not listed"
[ "$(printf '%s\n' "$out" | grep -c ': Restore$')" = 1 ] && printf '%s\n' "$out" | grep -q ": Delete$" \
    && pass "...Restore and Delete for what was kept, not for script text" || fail "Restore/Delete: $(printf '%s\n' "$out" | grep -ci ': restore$')"
printf '%s\n' "$out" | grep -q "state=1 .*: Real-time protection$" && pass "Real-time protection shows on" || fail "the real-time switch is not on"
printf '%s\n' "$out" | grep -q "state=1 .*: SG Defender$" && pass "...SG Defender too" || fail "the SG Defender switch is not on"
printf '%s\n' "$out" | grep -q "Button .*: Add or remove exclusions$" && pass "Exclusions has its button" || fail "no exclusions button"
for b in "Quick scan" "Full scan" "Custom scan"; do
    printf '%s\n' "$out" | grep -q "Button .*[0-9]: $b\$" || { fail "no enabled $b button"; }
done

# Quick scan: sg-defender-scan quick, as this person; the page follows the running scan
command -v xdotool >/dev/null || { echo "SKIP  the clicks: no xdotool"; stop; exit $RC; }
click "Quick scan"
i=0; while [ $i -lt 20 ] && [ ! -s "$T/scan-args" ]; do sleep 0.5; i=$((i + 1)); done
[ "$(cat "$T/scan-args" 2>/dev/null)" = quick ] && pass "Quick scan runs sg-defender-scan quick" || fail "scan tool: $(cat "$T/scan-args" 2>&1)"
wait_dump "Quick scan running (started 2026-11-25 12:00): 17 files scanned so far" && pass "...and the page shows it running" \
    || fail "no running scan: $(dump | grep -i scan | head -3)"
dump | grep -q "disabled: Full scan" && pass "...with the scan buttons off while it runs" || fail "buttons not off"
printf '[{"id": "20261125-120000-111111", "type": "quick", "state": "done", "start": "2026-11-25T12:00:00", "end": "2026-11-25T12:01:00", "files": 20, "skipped": 0, "found": 1, "threats": []}]' > "$N/scans.json"
wait_dump "Quick scan, 2026-11-25 12:00: 20 files scanned, 1 threat found." && pass "...until it is done (the page updates by itself)" \
    || fail "the finished scan not shown: $(dump | grep -i "scan," | head -3)"

# Restore, for real: the warning answered Yes, sg-admind puts the file back
QID=20261003-091011-abcdef
printf 'MZ quarantined' > "$D/quarantine/$QID.bin"
printf '{"id": "%s", "path": "%s/dl/free-game-setup.exe", "uid": %s, "signature": "Win.Test.EICAR_HDB-1", "sha256": "%s", "mode": 420}' \
    "$QID" "$T" "$(id -u)" "$(printf 'MZ quarantined' | sha256sum | cut -d' ' -f1)" > "$D/quarantine/$QID.json"
click Restore; sleep 2
W=$(xdotool search --name '^Virus & threat protection$' 2>/dev/null | head -1)
[ -n "$W" ] && pass "Restore warns first" || fail "no warning before restoring"
[ -n "$W" ] && { xdotool windowactivate --sync "$W" 2>/dev/null; xdotool key --window "$W" y 2>/dev/null; }
i=0; while [ $i -lt 40 ] && [ ! -e "$T/dl/free-game-setup.exe" ]; do sleep 0.5; i=$((i + 1)); done
[ "$(cat "$T/dl/free-game-setup.exe" 2>/dev/null)" = "MZ quarantined" ] && [ ! -e "$D/quarantine/$QID.bin" ] \
    && pass "...and Yes puts the file back where it was (through sg-admind)" || fail "not restored: $(ls "$T/dl") $(tail -3 "$T/admind.log" 2>/dev/null)"
stop

# Real-time protection off: the switch asks sg-admind (elevated), off for a while
open_page
click "Real-time protection"
i=0; while [ $i -lt 40 ] && [ ! -e "$T/run/realtime-off" ]; do sleep 0.5; i=$((i + 1)); done
left=$(( $(cat "$T/run/realtime-off" 2>/dev/null || echo 0) - $(date +%s) ))
[ "$left" -gt 0 ] && [ "$left" -le 14400 ] && pass "the Real-time protection switch turns it off, for a while ($left s)" \
    || fail "real-time off: $left s; $(tail -3 "$T/admind.log" 2>/dev/null)"
stop
sed -i 's/"realtime": true/"realtime": false/; s/"realtime_back": ""/"realtime_back": "2026-11-25T15:00"/' "$D/status.json"
open_page
dump | grep -q "Real-time protection is off until 15:00: programs are not scanned when they start" \
    && pass "off, the page says until when" || fail "the off state not shown: $(dump | grep -i real-time | head -3)"
dump | grep -q "state=0 .*: Real-time protection$" && pass "...and its switch is off" || fail "the real-time switch is not off"
stop

# Exclusions: the elevated window lists them (as paths of this PC) and saves what changed
# (typed as a Linux path in lower case: xdotool's typing into Wine under Xvfb loses Shift)
NEWDIR=/var/tmp/sg-defender-page-new-$$
mkdir -p "$T/keep-old" "$NEWDIR"
echo "$T/keep-old" > "$T/exclusions"
export SG_DEFENDER_EXCLUSIONS_FILE
SG_DEFENDER_EXCLUSIONS_FILE=$(wine winepath -w "$T/exclusions" 2>/dev/null | tr -d '\r')
wine "$EXE" /admin defender-exclusions >/dev/null 2>&1 &
i=0; W=""; while [ $i -lt 40 ] && [ -z "$W" ]; do W=$(xdotool search --name '^Exclusions$' 2>/dev/null | head -1); sleep 0.5; i=$((i + 1)); done
if [ -n "$W" ]; then
    pass "Add or remove exclusions opens its window"
    xdotool windowactivate --sync "$W" 2>/dev/null; sleep 0.5
    xdotool key --window "$W" ctrl+End; xdotool type --window "$W" --delay 20 "$NEWDIR"
    # Save: the form's default button, bottom right beside Cancel (a multi-line edit keeps Tab and Return)
    eval "$(xdotool getwindowgeometry --shell "$W")"
    xdotool mousemove $((X + WIDTH - 181)) $((Y + HEIGHT - 38)) click 1
    i=0; while [ $i -lt 40 ] && ! grep -qx "$NEWDIR" "$T/exclusions" 2>/dev/null; do sleep 0.5; i=$((i + 1)); done
    rmdir "$NEWDIR"
    grep -qx "$NEWDIR" "$T/exclusions" && grep -qx "$T/keep-old" "$T/exclusions" \
        && pass "...a folder typed in is added, the others (shown as C:/Z: paths) kept" || fail "exclusions: $(cat "$T/exclusions"; tail -3 "$T/admind.log")"
else fail "no Exclusions window"; fi
stop

# SG Defender turned off
sed -i 's/"enabled": true/"enabled": false/' "$D/status.json"; rm -f "$N"/x*.json "$N/scans.json"
open_page
dump | grep -q "Off: programs and downloads are not scanned" && pass "turned off, it says so" || fail "off not shown"
dump | grep -q "state=0 .*: SG Defender$" && pass "...and its switch off" || fail "the switch is not off"
dump | grep -q "Nothing has been found in your files" && pass "...and nothing found when nothing was" || fail "empty list wrong"
dump | grep -q "disabled: Quick scan" && pass "...and no scans to start" || fail "scan buttons on while off"
stop

[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
