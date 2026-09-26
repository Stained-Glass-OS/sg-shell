#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for Task Manager (sg-taskmgr), under Xvfb on a shell desktop with the
# X keyboard and mouse, reading back what it shows from SG_TASKMGR_DUMP:
#
#   - taskmgr.exe through App Paths opens it; "Fewer details" lists a running
#     app (Wine's clock) and not a background process
#   - Processes: a CPU-burning test process is listed with CPU above zero,
#     under Background processes; clicked and ended with the End task
#     button, it is gone (the process, not just the row)
#   - Details: the process, its PID, user and architecture
#   - Performance: CPU a percentage, memory total within 2% of /proc/meminfo,
#     process count matching the list
#   - Startup: a planted HKCU Run entry is listed Enabled; Disable writes
#     StartupApproved\Run with 03 as its first byte, the row says Disabled
#   - Users: this user; Services: Wine's PlugPlay service, Running
#   - File > Run new task starts a program by name
#
# Screenshots: build/taskmgr-*.png. Needs wine-sg, Xvfb, xdotool, ImageMagick
# and mingw; skips (77) without them. SG_TASKMGR_EXE tests another build (the
# mutation tests use it).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_TASKMGR_EXE:-$HERE/build/sg-taskmgr64.exe}"
OUT="$HERE/build"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
RC=0; DPY="${SG_TASKMGR_DPY:-116}"; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-taskmgr-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

"$MINGW" -O1 -municode -mconsole -o "$T/sgburn.exe" "$HERE/test/sg-taskmgr-burn.c" || { fail "test process did not build"; exit 1; }

Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
DUMP="$T/dump.txt"
wine wineboot --init >/dev/null 2>&1; wineserver -w
cp "$T/sgburn.exe" "$WINEPREFIX/drive_c/sgburn.exe"
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
windump=$(wine winepath -w "$DUMP" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
# App Paths as defaults/76-sg-taskmgr.reg has it, pointing at this build.
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\taskmgr.exe' /ve /d "$winexe"
reg 'HKCU\Software\Microsoft\Windows\CurrentVersion\Run' /v SgTestStartup /d 'C:\sg-test-startup.exe /quiet'
wineserver -w
export SG_TASKMGR_DUMP="$windump"

WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

# dump helpers
d() { cat "$DUMP" 2>/dev/null | tr -d '\r'; }
wait_dump() {   # $1: a grep -E pattern the dump must come to match; $2 seconds
    i=0
    while ! d | grep -Eq "$1" && [ $i -lt $(( ${2:-10} * 2 )) ]; do sleep 0.5; i=$((i + 1)); done
    d | grep -Eq "$1"
}
row_xy() { d | awk -v pat="$1" '$1 == "ROW" && $0 ~ pat { print $3, $4; exit }'; }
click() { xdotool mousemove "$1" "$2" click 1; sleep 0.6; }
shot() { import -window root "$OUT/taskmgr-$1.png" 2>/dev/null; }

wine clock >/dev/null 2>&1 &
wine 'C:\sgburn.exe' >/dev/null 2>&1 &
sleep 2
# Task Manager, fewer details first (as on a first run in Windows)
# taskmgr.exe as the Run box, scripts and the Win+X menu name it. CreateProcess
# and `start` find Wine's own system32\taskmgr.exe before App Paths; wine-sg
# hands that one off to App Paths. Without the hand-off, say so and start ours.
wine start taskmgr.exe >/dev/null 2>&1
if wait_dump '^MODE fewer' 10; then
    pass "taskmgr.exe opens this Task Manager (App Paths, through Wine's taskmgr hand-off)"
else
    echo "NOTE  taskmgr.exe opened Wine's own Task Manager: this wine-sg has no taskmgr hand-off; starting ours directly"
    wine taskkill /im taskmgr.exe /f >/dev/null 2>&1
    wine "$winexe" >/dev/null 2>&1 &
    wait_dump '^MODE fewer' 20 || { fail "Task Manager did not open"; exit 1; }
fi
pass "Task Manager opens with fewer details"
if wait_dump '^FEWER [0-9]+	.*[Cc]lock' 10; then pass "fewer details lists the running app (Clock)"; else fail "fewer details lacks Clock: $(d | grep '^FEWER')"; fi
d | grep -q '^FEWER.*sgburn' && fail "fewer details lists a background process" || pass "fewer details leaves out background processes"
shot fewer

# More details
set -- $(d | awk '$1 == "LINK" { print $2, $3 }')
click "$1" "$2"
if wait_dump '^MODE more' 5 && wait_dump '^TAB 0 Processes' 5; then pass "More details opens on Processes"; else fail "More details did not open: $(d | grep -E '^(MODE|TAB) ')"; fi

# the burner: listed, busy, a background process
if wait_dump '^PROC [0-9]+	1	sgburn.exe	[^	]*	[1-9][0-9.]*	' 15; then
    pass "the busy test process is listed under Background processes with CPU above zero"
else
    fail "the test process is not busy/listed: $(d | grep sgburn)"
fi
BURN_PID=$(d | awk -F'\t' '/^PROC [0-9]+\t.*sgburn.exe/ { split($1, a, " "); print a[2]; exit }')
d | grep -q '^ROW [0-9]* [0-9]* [0-9]* 1 Background processes' && pass "Background processes group row" || fail "no Background processes group row"
d | grep -Eq '^ROW [0-9]* [0-9]* [0-9]* 1 Apps \([0-9]+\)' && pass "Apps group row" || fail "no Apps group row"
d | grep -Eq '^COLUMN 2 CPU	[0-9]+%' && pass "the CPU column's header carries the total" || fail "no CPU total in the header"
shot processes

# Details, before the process is ended
xdotool key ctrl+Tab ctrl+Tab ctrl+Tab ctrl+Tab; sleep 1
if wait_dump '^TAB 4 Details' 5; then pass "Ctrl+Tab reaches Details"; else fail "Ctrl+Tab did not reach Details: $(d | grep '^TAB')"; fi
if d | grep -Eq "^ROW .* 0 sgburn.exe	$BURN_PID	Running	[^	]+	[0-9]+	[0-9]+ K	x64	"; then
    pass "Details: sgburn.exe, PID $BURN_PID, Running, a user, x64"
else
    fail "Details row for the test process wrong: $(d | grep 'ROW.*sgburn')"
fi
shot details
xdotool key ctrl+shift+Tab ctrl+shift+Tab ctrl+shift+Tab ctrl+shift+Tab; sleep 1
wait_dump '^TAB 0 Processes' 5 || fail "Ctrl+Shift+Tab did not come back to Processes"

# End task: click the row, then the button
xy=$(row_xy 'sgburn|0 [^	]*sgburn')
[ -n "$xy" ] || xy=$(d | awk '$1 == "ROW" && /sgburn/ { print $3, $4; exit }')
if [ -n "$xy" ]; then
    # shellcheck disable=SC2086
    click $xy
    wait_dump '^SEL .*sgburn' 5 && pass "clicking the row selects it" || fail "the row was not selected: $(d | grep '^SEL')"
    set -- $(d | awk '$1 == "BUTTON" { print $2, $3 }')
    click "$1" "$2"
    sleep 1.5
    if wait_dump "^ACTION end $BURN_PID ok" 5; then pass "End task reports the process ended"; else fail "End task did not act: $(d | grep '^ACTION')"; fi
    if wine tasklist 2>/dev/null | tr -d '\r' | grep -qi sgburn; then fail "the test process is still running"; else pass "the test process is gone"; fi
    d | grep -q '^PROC.*sgburn' && fail "the ended process is still listed" || pass "the list no longer shows it"
else
    fail "no row for the test process on screen"
fi

# Performance
xdotool key ctrl+Tab; sleep 1.5
wait_dump '^TAB 1 Performance' 5 && pass "Performance tab" || fail "no Performance tab"
perf=$(d | grep '^PERF ')
cpu=$(echo "$perf" | awk '{ print $3 }')
total=$(echo "$perf" | awk '{ for (i = 1; i < NF; i++) if ($i == "MEMTOTAL") print $(i + 1) }')
procs=$(echo "$perf" | awk '{ for (i = 1; i < NF; i++) if ($i == "PROCS") print $(i + 1) }')
real=$(awk '/^MemTotal:/ { print $2 * 1024 }' /proc/meminfo)
if awk -v c="$cpu" 'BEGIN { exit !(c >= 0 && c <= 100) }' && [ -n "$cpu" ]; then pass "CPU utilisation $cpu%"; else fail "CPU utilisation not a percentage: '$cpu'"; fi
if [ -n "$total" ] && awk -v a="$total" -v b="$real" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= b * 0.02) }'; then
    pass "memory total $total matches /proc/meminfo ($real)"
else
    fail "memory total '$total' does not match /proc/meminfo ($real)"
fi
listed=$(d | grep -c '^PROC ')
[ "$procs" = "$listed" ] && [ "$listed" -gt 3 ] && pass "process count $procs matches the list" || fail "process count '$procs' vs $listed listed"
shot performance
xdotool key Down; sleep 1
wait_dump '^PERFSEL 1' 3 && pass "Down selects Memory" || fail "Down did not select Memory"
shot performance-memory

# Startup
xdotool key ctrl+Tab; sleep 1.5
wait_dump '^TAB 2 Startup' 5 && pass "Startup tab" || fail "no Startup tab"
if d | grep -q '^STARTUP 0:SgTestStartup	SgTestStartup	Enabled'; then pass "the planted Run entry is listed, Enabled"; else fail "planted Run entry missing: $(d | grep '^STARTUP')"; fi
shot startup
xy=$(d | awk '$1 == "ROW" && /SgTestStartup/ { print $3, $4; exit }')
if [ -n "$xy" ]; then
    # shellcheck disable=SC2086
    click $xy
    set -- $(d | awk '$1 == "BUTTON" { print $2, $3 }')
    click "$1" "$2"
    sleep 1
    val=$(wine reg query 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run' /v SgTestStartup 2>/dev/null | tr -d '\r' | awk '/REG_BINARY/ { print $3 }')
    case "$val" in
        03*) pass "Disable wrote StartupApproved\\Run = $(echo "$val" | cut -c1-8)..." ;;
        *) fail "StartupApproved\\Run\\SgTestStartup is '$val'" ;;
    esac
    [ "$(echo "$val" | wc -c)" -eq 25 ] && pass "StartupApproved value is 12 bytes" || fail "StartupApproved value is not 12 bytes: $val"
    wait_dump '^STARTUP 0:SgTestStartup	SgTestStartup	Disabled' 5 && pass "the row now says Disabled" || fail "the row does not say Disabled"
    shot startup-disabled
    click "$1" "$2"
    sleep 1
    val=$(wine reg query 'HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run' /v SgTestStartup 2>/dev/null | tr -d '\r' | awk '/REG_BINARY/ { print $3 }')
    case "$val" in 02*) pass "Enable writes 02 back" ;; *) fail "Enable wrote '$val'" ;; esac
else
    fail "no row for the planted startup entry"
fi

# Users, Services
xdotool key ctrl+Tab; sleep 1.5
wait_dump '^TAB 3 Users' 5 && pass "Users tab" || fail "no Users tab"
me=$(d | awk -F'\t' '/^USER / { sub(/^USER /, "", $1); print $1; exit }')
[ -n "$me" ] && pass "Users lists '$me'" || fail "Users lists nobody"
shot users
xdotool key ctrl+Tab ctrl+Tab; sleep 1.5
wait_dump '^TAB 5 Services' 5 && pass "Services tab" || fail "no Services tab"
if d | grep -Eq '^SERVICE PlugPlay	[^	]*	Running	[0-9]+'; then pass "Services lists PlugPlay, Running, with its PID"; else fail "PlugPlay not listed as running: $(d | grep '^SERVICE' | head -5)"; fi
shot services

# File > Run new task
xdotool key alt+f; sleep 0.5; xdotool key r; sleep 1.5
xdotool type --delay 60 "winemine"; sleep 0.3; shot run-dialog; xdotool key Return; sleep 3
if wait_dump '^ACTION run winemine' 5 && wine tasklist 2>/dev/null | tr -d '\r' | grep -qi winemine; then
    pass "File > Run new task started winemine"
else
    fail "Run new task did not start winemine: $(d | grep '^ACTION'); $(wine tasklist 2>/dev/null | grep -ci winemine)"
fi
shot run

exit $RC
