#!/bin/sh
# shellcheck disable=SC2046,SC2155,SC2086
# Gate for Disk Cleanup (cleanmgr.exe, sg-cleanmgr64.exe), under Xvfb on a shell
# desktop, reading the dialog from SG_MMC_DUMP and checking what is deleted:
#
#   - cleanmgr.exe (wine-sg 0142's launcher, App Paths) opens "Disk Cleanup
#     for (C:)" with Windows' categories and the user's Linux ones (through
#     sg-sysinfo): the sizes are what is there -- the thumbnails, the Recycle
#     Bin (the XDG trash), and of the temporary files only those older than a
#     week
#   - ticking Temporary files and Recycle Bin, OK, and Yes to "Are you sure
#     you want to permanently delete these files?" deletes the week-old
#     temporary file and not today's, empties the Recycle Bin and the
#     thumbnails (ticked by default), and leaves what was not ticked
#
# SG_MMC_EXE (mutant: -DSG_MUTANT_AGE), SG_WINE_DIR, SG_SYSINFO as the other
# gates. Screenshots: build/cleanmgr-*.png.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_MMC_EXE:-$HERE/build/sg-cleanmgr64.exe}"
OUT="$HERE/build"
SYSINFO="${SG_SYSINFO:-$HERE/../sg-session/bin/sg-sysinfo}"
[ -x "$SYSINFO" ] || SYSINFO=/usr/bin/sg-sysinfo
SYSINFO=$(readlink -f "$SYSINFO")
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import python3; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
if [ -x "$WINE_DIR/bin/wine" ]; then WBIN="$WINE_DIR/bin"; WSERVER="$WINE_DIR/bin/wineserver"
elif [ -x "$WINE_DIR/wine" ]; then WBIN="$WINE_DIR"; WSERVER="$WINE_DIR/server/wineserver"
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi
[ -f "$EXE" ] && [ -x "$SYSINFO" ] || { echo "SKIP: $EXE or sg-sysinfo missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-cleanmgr.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WSERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

cp "$EXE" "$T/sg-cleanmgr64.exe"
Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$T/dpy" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/dpy" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/dpy")" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WBIN:$PATH" SG_SYSINFO="$SYSINFO" XDG_CACHE_HOME="$T/home/.cache" XDG_DATA_HOME="$T/home/.local/share"
DUMP="$T/dump.txt"
wine wineboot --init >/dev/null 2>&1; "$WSERVER" -w
winexe=$(wine winepath -w "$T/sg-cleanmgr64.exe" 2>/dev/null | tr -d '\r')
windump=$(wine winepath -w "$DUMP" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x800
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\cleanmgr.exe' /ve /d "$winexe"
for f in "$HERE"/theme/5[0-2]-*.reg; do wine regedit /S "Z:$(printf %s "$f" | tr / '\\')" >/dev/null 2>&1; done
"$WSERVER" -w

# what there is to clean
temp=$(wine cmd /c echo %TEMP% 2>/dev/null | tr -d '\r')
utemp=$(wine winepath -u "$temp" 2>/dev/null | tr -d '\r')
mkdir -p "$utemp" "$XDG_CACHE_HOME/thumbnails/normal" "$XDG_DATA_HOME/Trash/files" "$XDG_DATA_HOME/Trash/info"
head -c 100000 /dev/urandom > "$utemp/sg-old.tmp"; touch -d '10 days ago' "$utemp/sg-old.tmp"
head -c 70000 /dev/urandom > "$utemp/sg-new.tmp"
head -c 300000 /dev/urandom > "$XDG_CACHE_HOME/thumbnails/normal/a.png"
head -c 200000 /dev/urandom > "$XDG_DATA_HOME/Trash/files/deleted.txt"
printf '[Trash Info]\nPath=/home/x/deleted.txt\nDeletionDate=2026-09-01T10:00:00\n' > "$XDG_DATA_HOME/Trash/info/deleted.txt.trashinfo"
old_other=$(find "$utemp" -type f ! -name 'sg-*.tmp' -mtime +7 -printf '%s\n' 2>/dev/null | awk '{ s += $1 } END { print s + 0 }')

export SG_MMC_DUMP="$windump"
wine explorer /desktop=shell,1280x800 > "$T/explorer.out" 2>&1 &
sleep 4
d() { [ -f "$DUMP" ] && tr -d '\r' < "$DUMP"; }
wait_dump() { i=0; while ! d | grep -Eq "$1" && [ $i -lt $(( ${2:-10} * 2 )) ]; do sleep 0.5; i=$((i + 1)); done; d | grep -Eq "$1"; }
item() { d | awk -F'\t' -v id="$1" '$1 ~ /^ITEM / && $2 == id { print $'"$2"'; exit }'; }
idx() { d | awk -F'\t' -v id="$1" '$1 ~ /^ITEM / && $2 == id { split($1, a, " "); print a[2]; exit }'; }
shot() { import -window root "$OUT/cleanmgr-$1.png" 2>/dev/null; }

wine cleanmgr.exe >/dev/null 2>&1 &
wait_dump '^ITEM ' 30 && pass "cleanmgr.exe opens Disk Cleanup for (C:)" || fail "Disk Cleanup did not open"
d | grep -q '^BRIDGED 1' && pass "bridged to sg-sysinfo" || fail "not bridged"
sleep 1; shot open
[ "$(item temporary-files 4)" = $((100000 + old_other)) ] && pass "Temporary files: only what is older than a week ($(item temporary-files 4) bytes)" \
    || fail "temporary files: $(item temporary-files 4), want $((100000 + old_other))"
[ "$(item thumbnails 4)" -ge 300000 ] 2>/dev/null && pass "Thumbnails: $(item thumbnails 4) bytes" || fail "thumbnails: $(item thumbnails 4)"
[ "$(item recycle-bin 4)" -ge 200000 ] 2>/dev/null && pass "Recycle Bin: $(item recycle-bin 4) bytes" || fail "recycle bin: $(item recycle-bin 4)"
[ "$(item thumbnails 5)" = 1 ] && [ "$(item temporary-files 5)" = 0 ] && pass "Windows' defaults: Thumbnails ticked, Temporary files not" \
    || fail "defaults: thumbnails $(item thumbnails 5), temporary $(item temporary-files 5)"

# tick Temporary files and Recycle Bin with the keyboard (Down to the row, Space)
pos=0
for id in temporary-files recycle-bin; do
    to=$(idx $id)
    while [ $pos -lt "$to" ]; do xdotool key Down; pos=$((pos + 1)); sleep 0.2; done
    xdotool key space; sleep 0.4
done
wait_dump "^ITEM [0-9]+	recycle-bin	.*	1\$" 5 && [ "$(item temporary-files 5)" = 1 ] && pass "Temporary files and Recycle Bin ticked" \
    || fail "ticks: $(d | grep '^ITEM' | tr '\n' ' ')"
shot ticked
xdotool key Return; sleep 1.5
wait_dump '^MSG Are you sure you want to permanently delete these files' 5 && pass "it asks before deleting" || fail "no question: $(d | grep '^MSG')"
shot confirm
xdotool key Return
wait_dump '^MSG Done' 20 && pass "cleaned" || fail "not done: $(d | grep '^MSG')"
[ ! -e "$utemp/sg-old.tmp" ] && pass "the week-old temporary file is deleted" || fail "sg-old.tmp still there"
[ -e "$utemp/sg-new.tmp" ] && pass "today's temporary file is kept" || fail "sg-new.tmp was deleted"
[ ! -e "$XDG_CACHE_HOME/thumbnails/normal/a.png" ] && pass "the thumbnails are deleted" || fail "thumbnail still there"
[ ! -e "$XDG_DATA_HOME/Trash/files/deleted.txt" ] && pass "the Recycle Bin is emptied" || fail "trash still holds deleted.txt"

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
