#!/bin/sh
# shellcheck disable=SC2046,SC2155,SC2086
# Gate for System Information (msinfo32.exe, sg-msinfo3264.exe), under Xvfb on a
# shell desktop, reading what it shows from SG_MMC_DUMP and checking it against
# this machine:
#
#   - msinfo32.exe (wine-sg 0142's hand-off, App Paths) opens System Summary
#   - OS Name is sg-sysinfo's; Processor names /proc/cpuinfo's model with its
#     core and thread counts; Installed Physical Memory is /proc/meminfo's
#     MemTotal; System Type x64-based PC; System Name the computer's name
#   - Components > Display lists the display adapters sg-sysinfo reports
#   - msinfo32 /report FILE writes every category as text
#
# Screenshots: build/msinfo-*.png. SG_MMC_EXE (mutant: -DSG_MUTANT_MEM),
# SG_WINE_DIR and SG_SYSINFO as the other console gates.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_MMC_EXE:-$HERE/build/sg-msinfo3264.exe}"
OUT="$HERE/build"
SYSINFO="${SG_SYSINFO:-$HERE/../sg-session/bin/sg-sysinfo}"
[ -x "$SYSINFO" ] || SYSINFO=/usr/bin/sg-sysinfo
SYSINFO=$(readlink -f "$SYSINFO")
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import python3 iconv; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
if [ -x "$WINE_DIR/bin/wine" ]; then WBIN="$WINE_DIR/bin"; WSERVER="$WINE_DIR/bin/wineserver"
elif [ -x "$WINE_DIR/wine" ]; then WBIN="$WINE_DIR"; WSERVER="$WINE_DIR/server/wineserver"
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi
[ -f "$EXE" ] && [ -x "$SYSINFO" ] || { echo "SKIP: $EXE or sg-sysinfo missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-msinfo.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WSERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

# the msinfo32 name must reach this build: copy it under its own name
cp "$EXE" "$T/sg-msinfo3264.exe"
Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$T/dpy" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/dpy" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/dpy")" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WBIN:$PATH" SG_SYSINFO="$SYSINFO"
DUMP="$T/dump.txt"
wine wineboot --init >/dev/null 2>&1; "$WSERVER" -w
C="$WINEPREFIX/drive_c"
winexe=$(wine winepath -w "$T/sg-msinfo3264.exe" 2>/dev/null | tr -d '\r')
windump=$(wine winepath -w "$DUMP" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x800
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\msinfo32.exe' /ve /d "$winexe"
for f in "$HERE"/theme/5[0-2]-*.reg; do wine regedit /S "Z:$(printf %s "$f" | tr / '\\')" >/dev/null 2>&1; done
"$WSERVER" -w
export SG_MMC_DUMP="$windump"
wine explorer /desktop=shell,1280x800 > "$T/explorer.out" 2>&1 &
sleep 4

d() { [ -f "$DUMP" ] && tr -d '\r' < "$DUMP"; }
wait_dump() { i=0; while ! d | grep -Eq "$1" && [ $i -lt $(( ${2:-10} * 2 )) ]; do sleep 0.5; i=$((i + 1)); done; d | grep -Eq "$1"; }
val() { d | awk -F'\t' -v n="$1" '$1 ~ /^ROW / && $2 == n { print $3; exit }'; }
tree_xy() { d | awk -v n="$1" '$1 == "TREE" { t = $0; sub(/^TREE [0-9]+ -?[0-9]+ -?[0-9]+ (\* )?/, "", t); if (t == n && $3 >= 0) { print $3, $4; exit } }'; }
click() { [ $# -ge 2 ] || return 0; xdotool mousemove "$1" "$2" click 1; sleep 1; }
shot() { import -window root "$OUT/msinfo-$1.png" 2>/dev/null; }
si() { "$SYSINFO" system 2>/dev/null | sed -n "s/^$1 //p"; }

wine msinfo32.exe >/dev/null 2>&1 & how="msinfo32.exe (Wine's system32 program, handing off to App Paths: wine-sg 0142)"
if wait_dump '^TITLE System Information' 30 && wait_dump '	OS Name	' 10; then pass "System Information opens: $how"
else
    wine "$winexe" >/dev/null 2>&1 &
    wait_dump '	OS Name	' 20 && fail "msinfo32.exe did not reach it (a Wine without 0142?) -- started directly" || fail "System Information did not open"
fi
shot summary
[ "$(val 'OS Name')" = "$(si OS-NAME)" ] && pass "OS Name: $(val 'OS Name')" || fail "OS Name '$(val 'OS Name')' vs '$(si OS-NAME)'"
model=$(sed -n 's/^model name[[:space:]]*: //p' /proc/cpuinfo | head -1)
threads=$(grep -c '^processor' /proc/cpuinfo)
case "$(val Processor)" in "$model, "*" $threads Logical Processor(s)") pass "Processor: $(val Processor)" ;;
                           *) fail "Processor '$(val Processor)' vs $model / $threads threads" ;; esac
mem=$(awk '/^MemTotal/ { print $2 * 1024 }' /proc/meminfo)
want=$(python3 -c "
v=float($mem); u=0
while v >= 1024 and u < 5: v/=1024; u+=1
print(('%.0f' if v >= 100 else '%.1f' if v >= 10 else '%.2f') % v + ' ' + ['bytes','KB','MB','GB','TB','PB'][u])")
[ "$(val 'Installed Physical Memory (RAM)')" = "$want" ] && pass "Installed Physical Memory: $want (/proc/meminfo)" \
    || fail "memory '$(val 'Installed Physical Memory (RAM)')' vs $want"
[ "$(val 'System Type')" = "x64-based PC" ] && pass "System Type: x64-based PC" || fail "System Type '$(val 'System Type')'"
name=$(hostname | cut -d. -f1 | tr '[:lower:]' '[:upper:]')
[ "$(val 'System Name')" = "$name" ] && pass "System Name: $name" || fail "System Name '$(val 'System Name')' vs $name"

click $(tree_xy Components); xdotool key Right; sleep 1
click $(tree_xy Display)
"$SYSINFO" devices 2>/dev/null | awk '/^DEVICE /{n=""; c=""} /^CLASS /{c=$2} /^NAME /{sub(/^NAME /,""); n=$0} /^END$/{ if (c == "display") print n }' > "$T/gpus"
if wait_dump '^NODE Display' 5; then
    ok=1
    while IFS= read -r g; do d | grep -q "	Name	$g\$" || { ok=0; fail "Display: '$g' not listed"; }; done < "$T/gpus"
    [ $ok = 1 ] && pass "Components > Display lists $(wc -l < "$T/gpus") display adapter(s)"
else fail "Display: $(d | grep '^NODE')"; fi
shot display
wine taskkill /f /im sg-msinfo3264.exe >/dev/null 2>&1

wine "$winexe" /report 'C:\report.txt' >/dev/null 2>&1
i=0; while [ ! -s "$C/report.txt" ] && [ $i -lt 30 ]; do sleep 0.5; i=$((i + 1)); done
sleep 1
iconv -f utf-16 -t utf-8 "$C/report.txt" > "$T/report.utf8" 2>/dev/null
if grep -q '^\[System Summary\]' "$T/report.utf8" && grep -q "^OS Name	$(si OS-NAME)" "$T/report.utf8" && \
   grep -q '^\[Software Environment\\Services\]' "$T/report.utf8"; then
    pass "msinfo32 /report writes every category ($(grep -c '^\[' "$T/report.utf8"))"
else fail "report: $(head -5 "$T/report.utf8" | tr '\n' '|')"; fi

# /report returns only once the file is written (scripts read it next), a
# relative name meaning the caller's directory: straight after the command, no waiting
last_of() { iconv -f utf-16 -t utf-8 "$1" 2>/dev/null | grep -c '^\[Software Environment\\Startup Programs\]'; }
(cd "$C" && wine "$winexe" /report 'report2.txt' >/dev/null 2>&1)
[ "$(last_of "$C/report2.txt")" = 1 ] && pass "sg-msinfo /report waits until the report is complete (relative name in the caller's directory)" \
    || fail "sg-msinfo /report returned before the report was written ($(ls -l "$C/report2.txt" 2>&1))"
if [ -f "$C/windows/system32/msinfo32.exe" ] && strings -el "$C/windows/system32/msinfo32.exe" 2>/dev/null | grep -q '^report$'; then
    wine msinfo32 /report 'C:\report3.txt' >/dev/null 2>&1
    [ "$(last_of "$C/report3.txt")" = 1 ] && pass "msinfo32.exe (system32, wine-sg 0186) /report waits too" \
        || fail "system32 msinfo32 /report returned early"
else echo "NOTE  this Wine's msinfo32.exe does not wait (no wine-sg 0186)"; fi

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
