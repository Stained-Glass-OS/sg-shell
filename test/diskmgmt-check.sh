#!/bin/sh
# shellcheck disable=SC2046,SC2155,SC2086
# Gate for Disk Management (diskmgmt.msc, sg-mmc), under Xvfb on a shell
# desktop, reading what it shows from SG_MMC_DUMP:
#
#   1. This machine, through the real sg-sysinfo: diskmgmt.msc opens; every
#      disk lsblk lists is a row of the graphical view with lsblk's size, and
#      every partition a volume with its size; C: is the volume holding the
#      prefix's drive_c.
#   2. A made-up machine (a stand-in lsblk for SG_LSBLK): a system disk and a
#      removable data disk with an unmounted ext4 partition, 8 GiB
#      unallocated and a mounted FAT32 "PHOTOS" volume: the unallocated space
#      is drawn to scale between them, the system disk's volumes offer no
#      Format, the unmounted one does.
#   3. Changes go through sg-sysinfod (a socket the gate serves with
#      `systemd-socket-activate`, SG_SYSINFO_SOCKET): as a standard user
#      (SG_WINE_GROUP is this user's group, SG_ADMIN_GROUP one it is not in) Change Drive Letter
#      is refused with the administrator message and no letter appears; as an
#      administrator (SG_ADMIN_GROUP = this user's group) PHOTOS gets D: -- a
#      dosdevices link to its mount point, shown as "PHOTOS (D:)" -- and
#      Format of the spare partition as exFAT with a label runs mkfs.exfat
#      (a stand-in) with that label on that partition after the warning.
#   4. SMART (a stand-in smartctl through sg-sysinfod): the failing data disk
#      says "Online (Errors)", a banner warns, and its Properties give what
#      SMART reports; the healthy one is healthy.
#   5. For real, on a LOOP DEVICE only (needs passwordless sudo; sg-sysinfod
#      as root on the gate's socket with SG_SYSINFO_DISKS naming only that
#      loop device): New Simple Volume in its unallocated space -- ext4 with a
#      label and a drive letter (mounted under the gate's own media directory,
#      the letter in the prefix), then a second one without a letter; Shrink
#      Volume and Extend Volume on the second (lsblk's sizes); Delete Volume.
#
# Screenshots: build/diskmgmt-*.png. Needs wine-sg, Xvfb, xdotool,
# ImageMagick, python3, lsblk, systemd-socket-activate; skips (77) without
# them. SG_MMC_EXE tests another build (mutants: -DSG_MUTANT_SCALE, -DSG_MUTANT_FSNAME, -DSG_MUTANT_NONEW), SG_WINE_DIR
# another Wine, SG_SYSINFO the sg-sysinfo.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_MMC_EXE:-$HERE/build/sg-mmc64.exe}"
OUT="$HERE/build"
SYSINFO="${SG_SYSINFO:-$HERE/../sg-session/bin/sg-sysinfo}"
[ -x "$SYSINFO" ] || SYSINFO=/usr/bin/sg-sysinfo
SYSINFO=$(readlink -f "$SYSINFO")
RC=0; XP=""; SP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import python3 lsblk systemd-socket-activate; do
    command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }
done
if [ -x "$WINE_DIR/bin/wine" ]; then WBIN="$WINE_DIR/bin"; WSERVER="$WINE_DIR/bin/wineserver"
elif [ -x "$WINE_DIR/wine" ]; then WBIN="$WINE_DIR"; WSERVER="$WINE_DIR/server/wineserver"
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi
[ -f "$EXE" ] && [ -x "$SYSINFO" ] || { echo "SKIP: $EXE or sg-sysinfo missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-diskmgmt.XXXXXX); chmod 755 "$T"
LOOP=""
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WSERVER" -k 2>/dev/null
    [ -n "$SP" ] && { kill "$SP" 2>/dev/null; sudo -n kill "$SP" 2>/dev/null; }
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    if [ -n "$LOOP" ]; then
        for m in "$T"/media/*; do [ -d "$m" ] && sudo -n umount "$m" 2>/dev/null; done
        sudo -n losetup -d "$LOOP" 2>/dev/null
    fi
    sudo -n rm -rf "$T" 2>/dev/null || rm -rf "$T"
}
trap cleanup EXIT INT TERM

Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$T/dpy" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/dpy" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/dpy")" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WBIN:$PATH" SG_SYSINFO="$SYSINFO"
DUMP="$T/dump.txt"
wine wineboot --init >/dev/null 2>&1; "$WSERVER" -w
C="$WINEPREFIX/drive_c"
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
windump=$(wine winepath -w "$DUMP" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x800
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\mmc.exe' /ve /d "$winexe"
for f in "$HERE"/theme/5[0-2]-*.reg; do wine regedit /S "Z:$(printf %s "$f" | tr / '\\')" >/dev/null 2>&1; done
"$WSERVER" -w
export SG_MMC_DUMP="$windump"
wine explorer /desktop=shell,1280x800 > "$T/explorer.out" 2>&1 &
sleep 4

d() { [ -f "$DUMP" ] && tr -d '\r' < "$DUMP"; }
wait_dump() { i=0; while ! d | LC_ALL=C grep -aEq "$1" && [ $i -lt $(( ${2:-10} * 2 )) ]; do sleep 0.5; i=$((i + 1)); done; d | LC_ALL=C grep -aEq "$1"; }
click() { [ $# -ge 2 ] || return 0; xdotool mousemove "$1" "$2" click 1; sleep 1; }
shot() { import -window root "$OUT/diskmgmt-$1.png" 2>/dev/null; }
row_xy() { d | awk -F'\t' -v n="$1" '$1 ~ /^ROW / && ($2 == n || index($2, n " (") == 1) { split($1, a, " "); print a[3], a[4]; exit }'; }
link_xy() { d | awk -v n="$1" '$1 == "LINK" && $2 == 0 { t = $0; sub(/^LINK 0 -?[0-9]+ -?[0-9]+ /, "", t); if (t == n) { print $3, $4; exit } }'; }
verb_on() { d | awk -v n="$1" '$1 == "VERB" && $2 == "row" { t = $0; sub(/^VERB row [0-9]+ [01] -?[0-9]+ -?[0-9]+ -?[0-9]+ -?[0-9]+ /, "", t); if (t == n) { print $4; exit } }'; }
open_console() {
    rm -f "$DUMP"
    if [ -f "$C/windows/system32/diskmgmt.msc" ]; then wine start diskmgmt.msc >/dev/null 2>&1 &
    else wine "$winexe" diskmgmt >/dev/null 2>&1 & fi
    wait_dump '^CONSOLE diskmgmt' 30 && wait_dump '^(DISK|EMPTY [A-Za-z])' 20
}
close_console() { wine taskkill /f /im sg-mmc64.exe >/dev/null 2>&1; sleep 1; }

# ---- 1. this machine -------------------------------------------------------------------------
open_console && pass "diskmgmt.msc opens Disk Management$([ -f "$C/windows/system32/diskmgmt.msc" ] || echo ' (sg-mmc directly)')" \
    || fail "Disk Management did not open"
shot machine
lsblk -b -d -n -o NAME,SIZE,TYPE | awk '$3 == "disk" && $2 > 0 && $1 !~ /^(zram|loop|ram)/ { print $1, $2 }' > "$T/disks"
n=0
while read -r name size; do
    n=$((n + 1))
    got=$(d | awk -F'\t' -v n="$name" '$1 ~ /^DISK / && $2 == n { print $3; exit }')
    [ "$got" = "$size" ] && pass "disk $name listed, $size bytes as lsblk says" || fail "disk $name: '$got', lsblk $size"
done < "$T/disks"
[ $n -gt 0 ] || echo "NOTE  lsblk lists no disks here; the made-up machine checks the view"
parts=$(lsblk -b -n -o NAME,TYPE,PKNAME -l | awk 'NR == FNR { disk[$1] = 1; next } $2 == "part" && ($3 in disk)' "$T/disks" - | wc -l)
segs=$(d | grep -c '^SEG [0-9]*	[0-9]*	part	')
[ "$parts" = "$segs" ] && pass "every partition is a volume ($parts)" || fail "partitions: lsblk $parts, shown $segs"
cdev=$(df --output=source "$C" | tail -1 | sed 's#/dev/##')
d | grep -q "^SEG [0-9]*	[0-9]*	part	$cdev	.*	C:\$" && pass "C: is $cdev, the volume holding drive_c" || fail "C: is not $cdev: $(d | grep '	C:$')"
close_console

# ---- 2. a made-up machine -------------------------------------------------------------------------
mkdir -p "$T/media/photos"
cat > "$T/lsblk" <<EOF
#!/usr/bin/python3
import json
G = 1024 ** 3
print(json.dumps({"blockdevices": [
  {"name": "sda", "path": "/dev/sda", "type": "disk", "size": 64 * G, "model": "SYSDISK", "tran": "sata", "rota": False,
   "rm": False, "ro": False, "pttype": "gpt", "mountpoints": [None], "children": [
     {"name": "sda1", "path": "/dev/sda1", "type": "part", "start": 2048, "size": 512 * 1024 ** 2, "fstype": "vfat",
      "parttype": "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "mountpoints": ["/boot/efi"], "fssize": 500, "fsused": 1,
      "fsavail": 499, "partn": 1},
     {"name": "sda2", "path": "/dev/sda2", "type": "part", "start": 1050624, "size": 63 * G, "fstype": "ext4",
      "label": "root", "mountpoints": ["/"], "fssize": 1000, "fsused": 400, "fsavail": 600, "partn": 2}]},
  {"name": "sdb", "path": "/dev/sdb", "type": "disk", "size": 32 * G, "model": "DATA", "tran": "usb", "rota": True,
   "rm": True, "ro": False, "pttype": "dos", "mountpoints": [None], "children": [
     {"name": "sdb1", "path": "/dev/sdb1", "type": "part", "start": 2048, "size": 8 * G, "fstype": "ext4",
      "label": "spare", "mountpoints": [None], "partn": 1},
     {"name": "sdb2", "path": "/dev/sdb2", "type": "part", "start": 2048 + 16 * G // 512, "size": 8 * G,
      "fstype": "vfat", "label": "PHOTOS", "mountpoints": ["$T/media/photos"], "fssize": 8000, "fsused": 10,
      "fsavail": 7990, "partn": 2}]}]}))
EOF
printf '#!/bin/sh\nprintf "%%s\\n" "$*" >> "%s/mkfs.log"\n' "$T" > "$T/mkfs"
chmod 755 "$T/lsblk" "$T/mkfs"
export SG_LSBLK="$T/lsblk" SG_MKFS_EXFAT="$T/mkfs" SG_SYSINFO_SOCKET="$T/sysinfod.sock" SG_ADMIN_GROUP=sg-nobody-here
if open_console; then pass "Disk Management on a made-up machine (SG_LSBLK)"; else fail "the made-up machine did not open"; fi
d | grep -q '^DISK 1	sdb	34359738368	' && pass "the data disk: 32 GiB" || fail "data disk: $(d | grep '^DISK')"
free=$(d | awk -F'\t' '$1 ~ /^SEG / && $2 == 1 && $3 == "free" { print $5; exit }')
[ "$free" = $((8 * 1024 * 1024 * 1024 - 1048576)) ] || [ "$free" = $((8 * 1024 * 1024 * 1024)) ] && pass "8 GiB unallocated between the data disk's volumes" \
    || fail "unallocated: '$free'"
order=$(d | awk -F'\t' '$1 ~ /^SEG / && $2 == 1 { printf "%s ", ($3 == "free" ? "free" : $4) }')
[ "$order" = "sdb1 free sdb2 free " ] && pass "drawn in disk order: sdb1, unallocated, sdb2, unallocated" || fail "order: $order"
w1=$(d | awk -F'\t' '$1 ~ /^SEG / && $4 == "sda1" { print $7 }'); w2=$(d | awk -F'\t' '$1 ~ /^SEG / && $4 == "sda2" { print $7 }')
[ -n "$w1" ] && [ -n "$w2" ] && [ "$w2" -gt $((w1 * 4)) ] && pass "to scale: the 63 GiB volume is drawn far wider than the 512 MiB one ($w2 > $w1)" \
    || fail "widths: sda1 $w1, sda2 $w2"
shot made-up
click $(row_xy 'root')
wait_dump '^VERB row' 5
[ "$(verb_on '&Format...')" = 0 ] && pass "the system disk's volume offers no Format" || fail "system volume Format: $(verb_on '&Format...')"
click $(row_xy 'spare')
sleep 0.5
[ "$(verb_on '&Format...')" = 1 ] && pass "the unmounted spare volume offers Format" || fail "spare Format: $(verb_on '&Format...')"
d | grep -q '^BANNER You are signed in as a standard user' && pass "a standard user sees the banner" || fail "banner: $(d | grep '^BANNER')"

# ---- 3. changes through sg-sysinfod: refused, then allowed -----------------------------------------
serve() {
    [ -n "$SP" ] && kill "$SP" 2>/dev/null; sleep 0.5; rm -f "$SG_SYSINFO_SOCKET"
    # systemd-socket-activate passes on only the environment it is told to (-E)
    SG_ADMIN_GROUP="$1" SG_WINE_GROUP="$(id -gn)" systemd-socket-activate -l "$SG_SYSINFO_SOCKET" --inetd -a \
        -E SG_ADMIN_GROUP -E SG_WINE_GROUP -E SG_LSBLK -E SG_MKFS_EXFAT -E WINEPREFIX -E PATH \
        "$SYSINFO" --serve >/dev/null 2>&1 &
    SP=$!
    i=0; while [ ! -S "$SG_SYSINFO_SOCKET" ] && [ $i -lt 20 ]; do sleep 0.2; i=$((i + 1)); done
}
serve sg-nobody-here
click $(row_xy 'PHOTOS')
click $(link_xy 'Change Drive Letter and Paths...')
sleep 1; shot letter-dialog
xdotool key Return; sleep 2
if wait_dump '^MSG Changing the drive letter could not be done.*administrator.*needs an administrator' 5 && [ ! -e "$WINEPREFIX/dosdevices/d:" ]; then
    pass "a standard user's Change Drive Letter is refused (the administrator message), no letter made"
else fail "standard user's letter: $(d | grep '^MSG') $(ls "$WINEPREFIX/dosdevices")"; fi
shot refused
xdotool key Return; sleep 1
close_console

me=$(id -gn)
export SG_ADMIN_GROUP="$me"
serve "$me"
open_console
d | grep -q '^LINUXADMIN 1' && pass "an administrator (SG_ADMIN_GROUP=$me)" || fail "not an administrator: $(d | grep '^LINUXADMIN')"
click $(row_xy 'PHOTOS')
click $(link_xy 'Change Drive Letter and Paths...')
xdotool key Return; sleep 2
if [ "$(readlink "$WINEPREFIX/dosdevices/d:")" = "$T/media/photos" ]; then pass "PHOTOS gets D: (a dosdevices link to its mount point)"
else fail "d: -> '$(readlink "$WINEPREFIX/dosdevices/d:")'; $(d | grep '^MSG')"; fi
wait_dump '	PHOTOS \(D:\)	' 8 && pass "the volume is shown as PHOTOS (D:)" || fail "volume name: $(d | grep -i photos | head -2)"
shot letter
click $(row_xy 'spare')
click $(link_xy 'Format...')
sleep 1
xdotool key ctrl+a; xdotool type --delay 50 'gatevol'
xdotool key Tab; sleep 0.3; xdotool key e; sleep 0.3
shot format-dialog
xdotool key Return; sleep 1
wait_dump '^MSG Formatting this volume will erase all data' 5 && pass "Format warns that it erases the volume" || fail "no warning: $(d | grep '^MSG')"
xdotool key Return; sleep 3
if grep -qx -- '-L gatevol /dev/sdb1' "$T/mkfs.log" 2>/dev/null; then pass "Format: mkfs.exfat -L gatevol /dev/sdb1"
else fail "mkfs: '$(cat "$T/mkfs.log" 2>/dev/null)'"; fi
close_console

# ---- 4. SMART health through sg-sysinfod ------------------------------------------------------------
cat > "$T/smartctl" <<'EOF2'
#!/usr/bin/python3
import json, sys
if sys.argv[-1] == "/dev/sdb":
    print(json.dumps({"smart_status": {"passed": False}, "temperature": {"current": 51},
                      "ata_smart_attributes": {"table": [{"id": 5, "raw": {"value": 112}}]}}))
    sys.exit(8)
print(json.dumps({"smart_status": {"passed": True}, "temperature": {"current": 33}, "power_on_time": {"hours": 4321}}))
EOF2
chmod 755 "$T/smartctl"
export SG_SMARTCTL="$T/smartctl"
serve() {
    [ -n "$SP" ] && kill "$SP" 2>/dev/null; sleep 0.5; rm -f "$SG_SYSINFO_SOCKET"
    SG_ADMIN_GROUP="$1" SG_WINE_GROUP="$(id -gn)" systemd-socket-activate -l "$SG_SYSINFO_SOCKET" --inetd -a \
        -E SG_ADMIN_GROUP -E SG_WINE_GROUP -E SG_LSBLK -E SG_MKFS_EXFAT -E SG_SMARTCTL -E WINEPREFIX -E PATH \
        "$SYSINFO" --serve >/dev/null 2>&1 &
    SP=$!
    i=0; while [ ! -S "$SG_SYSINFO_SOCKET" ] && [ $i -lt 20 ]; do sleep 0.2; i=$((i + 1)); done
}
serve "$me"
open_console
[ "$(d | awk -F'\t' '$1 ~ /^DISK / && $2 == "sdb" { print $6 }')" = failing ] && [ "$(d | awk -F'\t' '$1 ~ /^DISK / && $2 == "sda" { print $6 }')" = ok ] \
    && pass "SMART: the data disk is failing, the system disk healthy" || fail "health: $(d | grep '^DISK')"
d | grep -q '^BANNER Disk 1 reports (SMART) that it is failing' && pass "a banner warns about the failing disk" || fail "banner: $(d | grep '^BANNER')"
shot smart
xy=$(d | awk -F'\t' '$1 ~ /^DISK / && $2 == "sdb" { print $4 }')
# shellcheck disable=SC2086
[ -n "$xy" ] && xdotool mousemove $xy click --repeat 2 1; sleep 2
if wait_dump '^MSG Disk 1\|.*reports \(SMART\) that it is failing.*.*C\): 51\|Reallocated sectors: 112\|' 5; then pass "the disk's Properties: failing, 51 C, 112 reallocated sectors"
else fail "disk properties: $(d | grep -a -A9 '^MSG')"; fi
shot smart-props
xdotool key Return; sleep 1
close_console
unset SG_SMARTCTL

# ---- 5. for real, on a loop device ------------------------------------------------------------------
if ! sudo -n true 2>/dev/null || [ ! -x /usr/sbin/losetup ]; then
    echo "NOTE  no passwordless sudo: the loop-device part is skipped"
else
    truncate -s 512M "$T/disk.img"
    LOOP=$(sudo -n losetup -f --show "$T/disk.img")
    N=${LOOP#/dev/}
    echo "      test disk: $LOOP"
    unset SG_LSBLK SG_MKFS_EXFAT
    mkdir -p "$T/media"
    export SG_SYSINFO_DISKS="$LOOP" SG_MEDIA_DIR="$T/media"
    [ -n "$SP" ] && kill "$SP" 2>/dev/null; sleep 0.5; rm -f "$SG_SYSINFO_SOCKET"
    sudo -n env SG_ADMIN_GROUP="$me" SG_WINE_GROUP="$me" SG_SYSINFO_DISKS="$LOOP" SG_MEDIA_DIR="$T/media" \
        WINEPREFIX="$WINEPREFIX" systemd-socket-activate -l "$SG_SYSINFO_SOCKET" --inetd -a -E SG_ADMIN_GROUP \
        -E SG_WINE_GROUP -E SG_SYSINFO_DISKS -E SG_MEDIA_DIR -E WINEPREFIX -E PATH "$SYSINFO" --serve >/dev/null 2>&1 &
    SP=$!
    i=0; while [ ! -S "$SG_SYSINFO_SOCKET" ] && [ $i -lt 30 ]; do sleep 0.2; i=$((i + 1)); done
    sudo -n chmod 666 "$SG_SYSINFO_SOCKET"
    open_console
    fxy=$(d | awk -F'\t' -v n="$N" '$1 ~ /^DISK / && $2 == n { k = substr($1, 6) } $1 ~ /^SEG / && $2 == k && $3 == "free" { print $6; exit }')
    [ -n "$fxy" ] && pass "the loop device's unallocated space is shown" || fail "no free space on $N: $(d | grep -E '^(DISK|SEG)')"
    # shellcheck disable=SC2086
    xdotool mousemove $fxy click 1; sleep 1
    [ "$(verb_on 'New &Simple Volume...')" = 1 ] && pass "unallocated space offers New Simple Volume" || fail "New verb: $(d | grep '^VERB')"
    click $(link_xy 'New Simple Volume...')
    sleep 1
    xdotool type --delay 50 '100'; xdotool key Tab; sleep 0.2; xdotool key e e; xdotool key Tab
    xdotool key ctrl+a; xdotool type --delay 50 'GATEONE'
    shot new-volume
    xdotool key Return
    i=0; while [ -z "$(lsblk -n -r -o NAME "$LOOP" | sed 1d)" ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done; sleep 3
    p1=$(lsblk -n -r -o NAME "$LOOP" | sed -n 2p)
    [ -n "$p1" ] && [ "$(lsblk -n -b -o SIZE "/dev/$p1")" = $((100 * 1048576)) ] && [ "$(lsblk -n -o FSTYPE "/dev/$p1")" = ext4 ] \
        && [ "$(lsblk -n -o LABEL "/dev/$p1")" = GATEONE ] && pass "New Simple Volume: $p1, 100 MB ext4 'GATEONE'" \
        || fail "new volume: $(lsblk -b -o NAME,SIZE,FSTYPE,LABEL "$LOOP")"
    lt=$(for l in "$WINEPREFIX"/dosdevices/[d-y]:; do [ -L "$l" ] && readlink "$l"; done | grep "/GATEONE$" | head -1)
    [ -n "$lt" ] && mountpoint -q "$lt" && pass "it is mounted and has a drive letter ($lt)" || fail "no letter/mount: $(ls -l "$WINEPREFIX/dosdevices")"
    wait_dump 'GATEONE \([D-Y]:\)' 10 && pass "shown as GATEONE (X:)" || fail "volume list: $(d | grep -i gate)"
    fxy=$(d | awk -F'\t' -v n="$N" '$1 ~ /^DISK / && $2 == n { k = substr($1, 6) } $1 ~ /^SEG / && $2 == k && $3 == "free" { print $6; exit }')
    # shellcheck disable=SC2086
    xdotool mousemove $fxy click 1; sleep 1
    click $(link_xy 'New Simple Volume...')
    sleep 1
    xdotool type --delay 50 '150'; xdotool key Tab; sleep 0.2; xdotool key e e; xdotool key Tab
    xdotool key ctrl+a; xdotool type --delay 50 'GATETWO'; xdotool key Tab space
    xdotool key Return
    i=0; while [ "$(lsblk -n -r -o NAME "$LOOP" | sed 1d | wc -l)" -lt 2 ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done; sleep 3
    p2=$(lsblk -n -r -o NAME,LABEL "$LOOP" | awk '$2 == "GATETWO" { print $1 }')
    [ -n "$p2" ] && [ "$(lsblk -n -b -o SIZE "/dev/$p2")" = $((150 * 1048576)) ] && [ -z "$(lsblk -n -o MOUNTPOINTS "/dev/$p2")" ] \
        && pass "a second volume without a drive letter: $p2, 150 MB, not mounted" || fail "second: $(lsblk -b -o NAME,SIZE,LABEL,MOUNTPOINTS "$LOOP")"
    shot two-volumes
    wait_dump 'GATETWO' 10
    click $(row_xy 'GATETWO')
    sleep 1
    [ "$(verb_on 'Shrin&k Volume...')" = 1 ] && [ "$(verb_on '&Delete Volume...')" = 1 ] \
        && pass "the unmounted ext4 volume offers Shrink and Delete" || fail "verbs: $(d | grep '^VERB row')"
    click $(link_xy 'Shrink Volume...')
    sleep 3; shot shrink
    xdotool key ctrl+a; xdotool type --delay 50 '50'; xdotool key Return
    i=0; while [ "$(lsblk -n -b -o SIZE "/dev/$p2")" != $((100 * 1048576)) ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
    [ "$(lsblk -n -b -o SIZE "/dev/$p2")" = $((100 * 1048576)) ] && pass "Shrink Volume: $p2 by 50 MB to 100 MB" \
        || fail "shrink: $(lsblk -n -b -o SIZE "/dev/$p2") $(d | grep '^MSG')"
    sleep 2; wait_dump 'GATETWO' 10
    click $(row_xy 'GATETWO'); sleep 1
    [ "$(verb_on 'E&xtend Volume...')" = 1 ] && pass "with space after it, Extend is offered" || fail "extend verb: $(d | grep '^VERB row')"
    click $(link_xy 'Extend Volume...')
    sleep 3; xdotool key ctrl+a; xdotool type --delay 50 '30'; xdotool key Return
    i=0; while [ "$(lsblk -n -b -o SIZE "/dev/$p2")" != $((130 * 1048576)) ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
    [ "$(lsblk -n -b -o SIZE "/dev/$p2")" = $((130 * 1048576)) ] && sudo -n e2fsck -fn "/dev/$p2" >/dev/null 2>&1 \
        && pass "Extend Volume: $p2 by 30 MB to 130 MB, the file system clean" || fail "extend: $(lsblk -n -b -o SIZE "/dev/$p2")"
    sleep 2; wait_dump 'GATETWO' 10
    click $(row_xy 'GATETWO'); sleep 1
    click $(link_xy 'Delete Volume...')
    sleep 1
    wait_dump '^MSG Deleting GATETWO will erase all data' 5 && pass "Delete Volume warns" || fail "delete warning: $(d | grep '^MSG')"
    shot delete
    xdotool key y
    i=0; while [ -e "/dev/$p2" ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
    [ ! -e "/dev/$p2" ] && [ -e "/dev/$p1" ] && pass "Delete Volume: $p2 gone, $p1 kept" || fail "delete: $(lsblk "$LOOP")"
    close_console
fi

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
