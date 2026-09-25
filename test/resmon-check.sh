#!/bin/sh
# shellcheck disable=SC2046,SC2155,SC2086
# Gate for Resource Monitor (resmon.exe, sg-resmon64.exe), under Xvfb on a shell
# desktop, reading its tabs from SG_MMC_DUMP while test processes of the
# gate's own (named sg-rm-*) load the machine:
#
#   - resmon.exe (wine-sg 0142's launcher, App Paths) opens on Overview with
#     the CPU, Disk, Network and Memory sections and the graphs
#   - CPU: a process spinning on one processor shows about one processor's
#     share of the machine (at least 60% of 100/processors)
#   - Memory: a process holding 300 MB has a working set of at least that
#   - Disk: a process writing and syncing a file shows write bytes per second
#   - Network: a process listening on a TCP port is under Listening Ports with
#     that port
#
# SG_MMC_EXE (mutant: -DSG_MUTANT_CPU), SG_WINE_DIR, SG_SYSINFO as the other
# gates. Screenshots: build/resmon-*.png.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_MMC_EXE:-$HERE/build/sg-resmon64.exe}"
OUT="$HERE/build"
SYSINFO="${SG_SYSINFO:-$HERE/../sg-session/bin/sg-sysinfo}"
[ -x "$SYSINFO" ] || SYSINFO=/usr/bin/sg-sysinfo
SYSINFO=$(readlink -f "$SYSINFO")
RC=0; XP=""; PIDS=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import python3; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
if [ -x "$WINE_DIR/bin/wine" ]; then WBIN="$WINE_DIR/bin"; WSERVER="$WINE_DIR/bin/wineserver"
elif [ -x "$WINE_DIR/wine" ]; then WBIN="$WINE_DIR"; WSERVER="$WINE_DIR/server/wineserver"
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi
[ -f "$EXE" ] && [ -x "$SYSINFO" ] || { echo "SKIP: $EXE or sg-sysinfo missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-resmon.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    WINEPREFIX="$T/pfx" "$WSERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

# the loads: python under names of their own (the kernel's comm)
py=$(readlink -f /usr/bin/python3)   # the interpreter itself, not a pyenv shim
for n in burn mem disk net; do cp "$py" "$T/sg-rm-$n"; done
"$T/sg-rm-burn" -c 'while True: pass' & PIDS="$PIDS $!"
"$T/sg-rm-mem" -c 'import time; b = b"\x01" * (300 * 1024 * 1024); time.sleep(900)' & PIDS="$PIDS $!"
"$T/sg-rm-disk" -c '
import os, time
while True:
    with open("'"$T"'/written", "wb") as f:
        for i in range(8):
            f.write(os.urandom(256 * 1024)); f.flush(); os.fsync(f.fileno())
    time.sleep(0.1)' & PIDS="$PIDS $!"
"$T/sg-rm-net" -c 'import socket, time; s = socket.socket(); s.bind(("127.0.0.1", 47123)); s.listen(); time.sleep(900)' & PIDS="$PIDS $!"

cp "$EXE" "$T/sg-resmon64.exe"
Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$T/dpy" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/dpy" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/dpy")" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WBIN:$PATH" SG_SYSINFO="$SYSINFO"
DUMP="$T/dump.txt"
wine wineboot --init >/dev/null 2>&1; "$WSERVER" -w
winexe=$(wine winepath -w "$T/sg-resmon64.exe" 2>/dev/null | tr -d '\r')
windump=$(wine winepath -w "$DUMP" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x800
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\resmon.exe' /ve /d "$winexe"
for f in "$HERE"/theme/5[0-2]-*.reg; do wine regedit /S "Z:$(printf %s "$f" | tr / '\\')" >/dev/null 2>&1; done
"$WSERVER" -w
export SG_MMC_DUMP="$windump"
wine explorer /desktop=shell,1280x800 > "$T/explorer.out" 2>&1 &
sleep 4

d() { [ -f "$DUMP" ] && tr -d '\r' < "$DUMP"; }
wait_dump() { i=0; while ! d | grep -Eq "$1" && [ $i -lt $(( ${2:-10} * 2 )) ]; do sleep 0.5; i=$((i + 1)); done; d | grep -Eq "$1"; }
col() { d | awk -F'\t' -v s="$1" -v n="$2" -v c="$3" '$1 == "ROW " s && $2 == n { print $(c + 1); exit }'; }
tab() { xy=$(d | awk -v t="$1" '$1 == "TABPOS" && $2 == t { print $3, $4 }'); xdotool mousemove $xy click 1; sleep 1; }
shot() { import -window root "$OUT/resmon-$1.png" 2>/dev/null; }

wine resmon.exe >/dev/null 2>&1 &
wait_dump '^TITLE Resource Monitor' 30 && wait_dump '^SECTION 3 Memory' 5 && pass "resmon.exe opens Resource Monitor on Overview (CPU, Disk, Network, Memory)" \
    || fail "Resource Monitor did not open"
d | grep -q '^BRIDGED 1' && pass "bridged to sg-sysinfo" || fail "not bridged"
sleep 5; shot overview

tab 1
ncpu=$(nproc)
want=$(( 60 / ncpu ))
i=0; while [ $i -lt 20 ]; do c=$(col 0 sg-rm-burn 5); [ -n "$c" ] && [ "$c" -ge "$want" ] 2>/dev/null && break; sleep 0.5; i=$((i + 1)); done
[ -n "$c" ] && [ "$c" -ge "$want" ] 2>/dev/null && pass "CPU: the spinning process uses $c% of the machine (>= $want%, one of $ncpu processors)" \
    || fail "CPU of sg-rm-burn: '$c' (want >= $want)"
shot cpu
tab 2
i=0; while [ $i -lt 20 ]; do m=$(col 0 sg-rm-mem 3); [ -n "$m" ] && break; sleep 0.5; i=$((i + 1)); done
[ -n "$m" ] && [ "$m" -ge 307200 ] 2>/dev/null && pass "Memory: the 300 MB process's working set is $m KB" || fail "working set of sg-rm-mem: '$m'"
tab 3
i=0; while [ $i -lt 20 ]; do w=$(col 0 sg-rm-disk 4); [ -n "$w" ] && [ "$w" -gt 100000 ] 2>/dev/null && break; sleep 0.5; i=$((i + 1)); done
[ -n "$w" ] && [ "$w" -gt 100000 ] 2>/dev/null && pass "Disk: the writing process writes $w B/sec" || fail "writes of sg-rm-disk: '$w'"
d | grep -q '^SECTION 1 Storage' && d | grep -q '^ROW 1	C:	' && pass "Disk: Storage lists C:" || fail "storage: $(d | grep '^ROW 1' | head -2)"
shot disk
tab 4
i=0; while [ $i -lt 20 ]; do p=$(col 2 sg-rm-net 4); [ -n "$p" ] && break; sleep 0.5; i=$((i + 1)); done
[ "$p" = 47123 ] && pass "Network: the listening process is under Listening Ports on 47123" || fail "listening port of sg-rm-net: '$p'"
shot network

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
