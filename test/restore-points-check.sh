#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Restore points in Settings > Update & Security > Recovery and the Control
# Panel's Recovery (src/control/restore.c), from what sg-snapshot publishes
# (SG_SNAPSHOT_STATUS: a stand-in status file per case):
#
#   - btrfs: "Go back to the previous version of Stained Glass OS" with the
#     restore points, newest first, each "Before the update of <date>" with
#     what the update changed; one that cannot start says so and has no
#     button; "Go back to this version", answered OK, asks sg-admind (test
#     mode, a stand-in sg-snapshot) for "restore-point rollback ID" -- the
#     elevated half through a real spool;
#   - a restart pending: "Restart required";
#   - running a restore point: "You started an earlier version", Keep this
#     version;
#   - ext4: Undo the last update (Get started), and the way to turn restore
#     points on;
#   - the Control Panel offers "Turn on system restore points (convert the
#     system drive)" ONLY on ext4 (System and Security, and its page with
#     the warning to back up and what stands in the way); on btrfs it does
#     not.
#
#   - SG Store restore points (SNAPSHOT's 6th field "store") are named "Before
#     the SG Store change of <date>", the Settings page says what is kept (the
#     last three before updates and the last two before SG Store installs), and
#     "Get started" never goes back to a Store point;
#   - a conversion waiting to be kept says how many days are left to undo it
#     (CONVERT_DEADLINE) and that it is then kept automatically; one kept by itself
#     (KEPT .. auto) is said so; the offer on ext4 says "kept for 14 days".
#
# Needs wine-sg and Xvfb; skips (77) without. SG_WINE_DIR another Wine.
#   sh test/restore-points-check.sh [--mutant RP_CONVERT_ALWAYS|RP_STORE_TITLE|RP_NO_DAYS|RP_PREVIOUS_STORE]   (must fail)
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
RC=0; DPY="${SG_RP_DPY:-131}"; XP=""; LOOP=""
MUTANT=""
[ "${1:-}" = --mutant ] && MUTANT=${2:-}
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
command -v Xvfb >/dev/null && command -v xdotool >/dev/null || { echo "SKIP: Xvfb or xdotool missing"; exit 77; }
[ -x "$WINE_DIR/bin/wine" ] || { echo "SKIP: wine-sg missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-restore-points-check.XXXXXX)
# shellcheck disable=SC2317
cleanup() {
    [ -n "$LOOP" ] && kill "$LOOP" 2>/dev/null
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

# the programs: this tree's, or a mutant's build (both programs, from the same sources)
SET="$HERE/build/sg-settings64.exe" CTL="$HERE/build/sg-control64.exe"
case "$MUTANT" in
    "") [ -f "$SET" ] && [ -f "$CTL" ] || { echo "SKIP: build/sg-settings64.exe or sg-control64.exe missing"; exit 77; } ;;
    RP_CONVERT_ALWAYS|RP_STORE_TITLE|RP_NO_DAYS|RP_PREVIOUS_STORE)
        B="$T/build"; mkdir -p "$B"
        python3 "$HERE/src/control/gen-icon.py" "$B/sg-control.ico" && python3 "$HERE/src/settings/gen-icon.py" "$B/sg-settings.ico" && \
        x86_64-w64-mingw32-windres -I "$HERE/src/control" -I "$B" "$HERE/src/control/control.rc" -O coff -o "$B/res.o" && \
        x86_64-w64-mingw32-windres -I "$HERE/src/settings" -I "$B" "$HERE/src/settings/settings.rc" -O coff -o "$B/set-res.o" || exit 1
        for pair in "sg-control64.exe res.o" "sg-settings64.exe set-res.o"; do
            # shellcheck disable=SC2086
            set -- $pair
            x86_64-w64-mingw32-gcc -O2 -municode -mwindows -Wall -Wno-missing-field-initializers -DSG_MUTANT_$MUTANT \
                -o "$B/$1" "$HERE"/src/control/*.c "$B/$2" \
                -lcomctl32 -lshell32 -lgdi32 -luser32 -ladvapi32 -lmsimg32 -liphlpapi -lws2_32 -lole32 -luuid -lwindowscodecs \
                -lcomdlg32 -lshlwapi -lwininet -lversion -lwinspool || exit 1
        done
        CTL="$B/sg-control64.exe" SET="$B/sg-settings64.exe" ;;
    *) echo "unknown mutant $MUTANT"; exit 2 ;;
esac

Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=,winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
export SG_SNAPSHOT_STATUS="$T/status"
btrfs_status() {
    printf 'FS btrfs\nLAYOUT yes\nBOOTED %s\nCONVERT none\t\n%sSNAPSHOT 20261008-185501\t2026-10-08 18:55\tauto\tyes\tUpdates: sg-shell 0.1.0-169 to 0.1.0-170\n' "${1:-current}" "${2:-}" > "$T/status"
    printf 'SNAPSHOT 20261007-090000\t2026-10-07 09:00\tauto\tyes\tUpdates: wine-sg 10.0-205 to 10.0-206\n' >> "$T/status"
    printf 'SNAPSHOT 20261001-120000\t2026-10-01 12:00\tauto\tno\tUpdates: linux-image-amd64\nSAVED no\nOK\n' >> "$T/status"
}
ext4_status() {
    printf 'FS ext4\nLAYOUT no\nBOOTED current\nCONVERT none\t\nUNDO 20261008-185501\t2026-10-08 18:55\tUpdates: sg-shell 0.1.0-169 to 0.1.0-170\n' > "$T/status"
    printf 'PROBLEM There is not enough free space on the system drive: 3 GB free, 6 GB needed.\nREADY no\nOK\n' >> "$T/status"
}
show() {   # show EXE ARG WAITFOR: the window's dump once it shows WAITFOR
    rm -f "$T/dump.txt"
    SG_SETTINGS_DUMP=$(wine winepath -w "$T/dump.txt" 2>/dev/null | tr -d '\r') wine "$1" $2 >/dev/null 2>&1 &
    i=0; while [ $i -lt 60 ] && ! grep -q "$3" "$T/dump.txt" 2>/dev/null; do sleep 1; i=$((i + 1)); done
    sleep 1; tr -d '\r' < "$T/dump.txt" 2>/dev/null
}
has() { printf '%s\n' "$out" | grep -qF -- "$1"; }

# --- Settings, btrfs ---------------------------------------------------------------
btrfs_status
out=$(show "$SET" ms-settings:recovery "Restore points"); wineserver -k 2>/dev/null; sleep 1
has "Go back to the previous version of Stained Glass OS" && has "Get started" \
    && pass "Settings > Recovery: Go back to the previous version of Stained Glass OS, Get started" || { fail "no Go back section"; printf '%s\n' "$out" | head -30; }
has "Before the update of 2026-10-08 18:55" && has "Updates: sg-shell 0.1.0-169 to 0.1.0-170" && has "Before the update of 2026-10-07 09:00" \
    && pass "...the restore points, each with what its update changed" || fail "restore points not listed"
[ "$(printf '%s\n' "$out" | grep -c ': Go back to this version$')" = 2 ] && has "cannot be started" \
    && pass "...'Go back to this version' for each that can start; the one that cannot says so" || fail "go back links: $(printf '%s\n' "$out" | grep -c 'Go back to this version')"

# Go back to this version, for real: OK, then the elevated half's request
S="$T/spool"; mkdir -p "$S/requests" "$S/replies" "$T/bin"; chmod 700 "$S/requests"
printf '#!/bin/sh\necho "$*" >> "%s/calls"\necho "PENDING $2"\necho OK\n' "$T" > "$T/bin/sg-snapshot"; chmod 755 "$T/bin/sg-snapshot"
export SG_ADMIN_TEST=1 SG_ADMIN_SPOOL="$S" SG_ADMIN_SYSTEM_UID="$(id -u)" SG_ADMIN_PATH="$T/bin"
( while :; do for f in "$S"/requests/*.req; do [ -e "$f" ] && python3 "$HERE/admin/sg-admind" 2>>"$T/admind.log"; break; done; sleep 0.2; done ) &
LOOP=$!
out=$(show "$SET" ms-settings:recovery "Go back to this version")
at=$(printf '%s\n' "$out" | sed -n 's/.* at=\([0-9]*\),\([0-9]*\): Go back to this version$/\1 \2/p' | head -1)
# shellcheck disable=SC2086  # x y
[ -n "$at" ] && xdotool mousemove $at click 1; sleep 2
W=$(xdotool search --name '^Go back to the previous version$' 2>/dev/null | head -1)
[ -n "$W" ] && pass "Go back to this version asks first" || fail "no question before going back"
[ -n "$W" ] && { xdotool windowactivate --sync "$W" 2>/dev/null; xdotool key --window "$W" Return 2>/dev/null; }
i=0; while [ $i -lt 60 ] && [ ! -s "$T/calls" ]; do sleep 0.5; i=$((i + 1)); done
[ "$(cat "$T/calls" 2>/dev/null)" = "rollback 20261008-185501" ] \
    && pass "...and OK asks sg-admind to go back to that restore point (restore-point rollback ID)" || fail "request: $(cat "$T/calls" 2>&1) $(tail -3 "$T/admind.log" 2>/dev/null)"
kill "$LOOP" 2>/dev/null; LOOP=""; wineserver -k 2>/dev/null; sleep 1

btrfs_status current "PENDING rollback
"
out=$(show "$SET" ms-settings:recovery "Restart required"); wineserver -k 2>/dev/null; sleep 1
has "Restart required" && has "goes back to the earlier version when you restart" && ! has "Go back to this version" \
    && pass "going back pending: Restart required, no other restore point offered" || fail "pending: $(printf '%s\n' "$out" | grep -i restart)"
btrfs_status 20261008-185501
out=$(show "$SET" ms-settings:recovery "You started an earlier version"); wineserver -k 2>/dev/null; sleep 1
has "You started an earlier version" && has "Keep this version" && has "as it was before the update of 2026-10-08 18:55" \
    && pass "running a restore point: You started an earlier version, Keep this version" || fail "booted snapshot not shown"

# --- Settings, ext4 -------------------------------------------------------------------
ext4_status
out=$(show "$SET" ms-settings:recovery "undo the last update"); wineserver -k 2>/dev/null; sleep 1
has "undo the last update of Stained Glass OS's own programs (2026-10-08 18:55)" && has "Get started" && has "Turn on system restore points" \
    && pass "ext4: Undo the last update (Get started), and the way to turn restore points on" || { fail "ext4 page"; printf '%s\n' "$out" | head -40; }

# --- the Control Panel: the conversion only on ext4 ----------------------------------------
TASK="Recovery|Turn on system restore points (convert the system drive)"
d=$(wine "$CTL" --dump items 2>/dev/null | tr -d '\r')
printf '%s\n' "$d" | grep -qF "task=System and Security|$TASK" && wine "$CTL" --dump recovery 2>/dev/null | tr -d '\r' | grep -qx 'recovery.convert_offered=yes' \
    && pass "ext4: System and Security offers 'Turn on system restore points (convert the system drive)'" || fail "ext4: no conversion task: $(printf '%s\n' "$d" | grep task=)"
out=$(show "$CTL" "--page recovery" "Convert the system drive"); wineserver -k 2>/dev/null; sleep 1
has "Turn on system restore points (convert the system drive)" && has "Back up your files first" && has "3 GB free, 6 GB needed" \
    && has "Convert the system drive..." && pass "...its page: what it does, the warning to back up, what stands in the way" || { fail "convert page"; printf '%s\n' "$out" | head -30; }
btrfs_status
d=$(wine "$CTL" --dump items 2>/dev/null | tr -d '\r')
! printf '%s\n' "$d" | grep -qF "$TASK" && printf '%s\n' "$d" | grep -qF "task=System and Security|Recovery|Go back to a restore point" \
    && wine "$CTL" --dump recovery 2>/dev/null | tr -d '\r' | grep -qx 'recovery.convert_offered=no' \
    && pass "btrfs: no conversion offered; Recovery goes back to a restore point" || fail "btrfs: the conversion is offered: $(printf '%s\n' "$d" | grep task=)"

# a conversion whose last step failed: undo it, nothing to keep
printf "FS btrfs\nLAYOUT no\nBOOTED current\nCONVERT failed-layout\tthe system's fstab could not be written\nSAVED yes\nOK\n" > "$T/status"
out=$(show "$CTL" "--page recovery" "Undo the conversion"); wineserver -k 2>/dev/null; sleep 1
has "The conversion did not finish" && has "the system's fstab could not be written" && has "Undo the conversion..." && ! has "Keep the conversion" \
    && pass "a conversion that did not finish: why, and Undo the conversion (nothing to keep)" || { fail "half-converted page"; printf '%s\n' "$out" | head -20; }
printf "FS btrfs\nLAYOUT yes\nBOOTED current\nCONVERT converted\t\nSAVED yes\nOK\n" > "$T/status"
out=$(show "$CTL" "--page recovery" "Keep the conversion"); wineserver -k 2>/dev/null; sleep 1
has "The system drive was converted" && has "Keep the conversion" && has "Undo the conversion..." \
    && pass "converted: Keep the conversion or Undo it" || fail "converted page"

# --- SG Store restore points: named for the Store, kept apart from the updates ------------
store_status() {
    printf 'FS btrfs\nLAYOUT yes\nBOOTED current\nCONVERT none\t\n' > "$T/status"
    printf 'SNAPSHOT 20261009-101500\t2026-10-09 10:15\tauto\tyes\tSG Store: gimp 2.10-1\tstore\n' >> "$T/status"
    printf 'SNAPSHOT 20261008-185501\t2026-10-08 18:55\tauto\tyes\tUpdates: sg-shell 0.1.0-169 to 0.1.0-170\tupdate\n' >> "$T/status"
    printf 'SAVED no\nOK\n' >> "$T/status"
}
store_status
out=$(show "$SET" ms-settings:recovery "Restore points"); wineserver -k 2>/dev/null; sleep 1
has "Before the SG Store change of 2026-10-09 10:15" && has "SG Store: gimp 2.10-1" && has "Before the update of 2026-10-08 18:55" \
    && pass "Settings: a restore point from an SG Store change is 'Before the SG Store change of <date>', the update's 'Before the update of <date>'" \
    || { fail "store restore point not named"; printf '%s\n' "$out" | head -30; }
has "The last three from before updates are kept, and, apart from them, the last two from before SG Store installs" \
    && pass "...and the page says what is kept: the last three before updates, apart, the last two before SG Store installs" || fail "retention text missing"
# Get started goes back to the update's point, though the Store's is newer
wine "$CTL" --dump recovery 2>/dev/null | tr -d '\r' | grep -qF 'recovery.snapshot=20261009-101500|2026-10-09 10:15|auto|yes|SG Store: gimp 2.10-1|store' \
    && pass "the Control Panel's Recovery knows each point's pool (store | update)" || fail "dump lacks the pool"
printf 'FS btrfs\nLAYOUT yes\nBOOTED current\nCONVERT none\t\nSNAPSHOT 20261009-101500\t2026-10-09 10:15\tauto\tyes\tSG Store: gimp 2.10-1\tstore\nSAVED no\nOK\n' > "$T/status"
out=$(show "$SET" ms-settings:recovery "Restore points"); wineserver -k 2>/dev/null; sleep 1
! has "Get started" && has "Go back to this version" \
    && pass "with only an SG Store restore point: no 'Get started' (the previous version is the update's), the point is still listed to go back to" \
    || { fail "Get started offered for a Store point"; printf '%s\n' "$out" | head -30; }
printf 'FS btrfs\nLAYOUT yes\nBOOTED current\nCONVERT none\t\nWENTBACK 20261009-101500\t2026-10-09 10:15\tstore\nOK\n' > "$T/status"
out=$(show "$SET" ms-settings:recovery "went back"); wineserver -k 2>/dev/null; sleep 1
has "went back to the version from before the SG Store change of 2026-10-09 10:15" \
    && pass "after going back to a Store point: said so, by its name" || fail "wentback wording: $(printf '%s\n' "$out" | grep -i 'went back')"

# --- the conversion: 14 days to undo it, then it is kept by itself -----------------------------
printf "FS btrfs\nLAYOUT yes\nBOOTED current\nCONVERT converted\t20261001-120000\nSAVED yes\nCONVERT_DEADLINE 11\t2026-10-23\nOK\n" > "$T/status"
out=$(show "$CTL" "--page recovery" "Keep the conversion"); wineserver -k 2>/dev/null; sleep 1
has "You can undo the conversion for 11 more days (until 2026-10-23). After that it is kept automatically" && has "Keep the conversion" \
    && pass "converted: the page shows the days left (11), the day it is kept by itself, and that the old file system is then deleted" \
    || { fail "days left not shown"; printf '%s\n' "$out" | head -20; }
wine "$CTL" --dump recovery 2>/dev/null | tr -d '\r' | grep -qx 'recovery.convert_days=11|2026-10-23' \
    && pass "...and the dump says so" || fail "dump: convert_days"
printf "FS btrfs\nLAYOUT yes\nBOOTED current\nCONVERT converted\t20261001-120000\nSAVED yes\nCONVERT_DEADLINE 1\t2026-10-10\nOK\n" > "$T/status"
wine "$CTL" --dump recovery 2>/dev/null | tr -d '\r' | grep -qF 'for 1 more day (until 2026-10-10)' \
    && pass "one day left: '1 more day'" || fail "singular"
printf "FS btrfs\nLAYOUT yes\nBOOTED current\nCONVERT kept\t20261015-090000 auto\nSAVED no\nKEPT 2026-10-15 09:00\tauto\nOK\n" > "$T/status"
out=$(show "$CTL" "--page recovery" "kept automatically"); wineserver -k 2>/dev/null; sleep 1
has "The conversion was kept automatically on 2026-10-15 09:00, 14 days after it was made" && ! has "Keep the conversion" && ! has "Undo the conversion" \
    && pass "after 14 days: 'The conversion was kept automatically on <date>' -- nothing left to keep or undo" \
    || { fail "auto-kept page"; printf '%s\n' "$out" | head -20; }
printf "FS btrfs\nLAYOUT yes\nBOOTED current\nCONVERT kept\t20261012-090000\nSAVED no\nKEPT 2026-10-12 09:00\tyou\nOK\n" > "$T/status"
wine "$CTL" --dump recovery 2>/dev/null | tr -d '\r' | grep -qF 'You kept the conversion on 2026-10-12 09:00' \
    && pass "kept by the person: 'You kept the conversion on <date>'" || fail "kept by you"
ext4_status
out=$(show "$CTL" "--page recovery" "Convert the system drive"); wineserver -k 2>/dev/null; sleep 1
has "kept for 14 days" && has "kept automatically" && pass "the offer on ext4 says the old file system is kept for 14 days, then the conversion is kept automatically" \
    || { fail "offer text"; printf '%s\n' "$out" | head -30; }

[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
