#!/bin/sh
# shellcheck disable=SC2046,SC2155,SC2086
# Gate for Services (services.msc, sg-mmc), under Xvfb on a shell desktop with
# the X mouse and keyboard, reading what the console shows from SG_MMC_DUMP and
# what the SCM did from `sc`:
#
#   - services.msc opens the console (through wine-sg 0142's .msc file and
#     mmc.exe launcher when the Wine has them; else sg-mmc directly)
#   - a test service installed with `sc create` is listed with its display
#     name, description, blank status, Manual and Local System
#   - selected and started with the toolbar's Start: the SCM says RUNNING and
#     the row says Running; Pause (toolbar) PAUSED; Resume (Actions pane)
#     RUNNING; Restart (toolbar) runs it in a new process; Stop (Actions pane)
#     STOPPED and the row's status is blank again
#   - Properties (Alt+Enter), Startup type Disabled (Alt+U, D), OK: the SCM's
#     start type is DISABLED, the row says Disabled and Start is disabled
#   - Stained Glass System Services lists what sg-sysinfo's `units` reports
#     (through the bridge), read-only
#
# Screenshots: build/services-*.png. Needs wine-sg, Xvfb, xdotool,
# ImageMagick and mingw; skips (77) without them. SG_MMC_EXE tests another
# build (the mutants: -DSG_MUTANT_NOSTART, -DSG_MUTANT_STATUS);
# SG_WINE_DIR another Wine (a build tree works); SG_SYSINFO the sg-sysinfo to
# bridge through (default: ../sg-session/bin/sg-sysinfo, else /usr/bin).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_MMC_EXE:-$HERE/build/sg-mmc64.exe}"
OUT="$HERE/build"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
SYSINFO="${SG_SYSINFO:-$HERE/../sg-session/bin/sg-sysinfo}"
[ -x "$SYSINFO" ] || SYSINFO=/usr/bin/sg-sysinfo
SYSINFO=$(readlink -f "$SYSINFO")
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
if [ -x "$WINE_DIR/bin/wine" ]; then WBIN="$WINE_DIR/bin"; WSERVER="$WINE_DIR/bin/wineserver"
elif [ -x "$WINE_DIR/wine" ]; then WBIN="$WINE_DIR"; WSERVER="$WINE_DIR/server/wineserver"
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi
[ -f "$EXE" ] || { echo "SKIP: $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-services-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WSERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

"$MINGW" -O2 -municode -o "$T/sg-svc-test.exe" "$HERE/test/sg-svc-test.c" -ladvapi32 || { fail "test service did not build"; exit 1; }

Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$T/dpy" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/dpy" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/dpy")" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WBIN:$PATH" SG_SYSINFO="$SYSINFO"
DUMP="$T/dump.txt"
wine wineboot --init >/dev/null 2>&1; "$WSERVER" -w
C="$WINEPREFIX/drive_c"
cp "$T/sg-svc-test.exe" "$C/sg-svc-test.exe"
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
windump=$(wine winepath -w "$DUMP" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x800
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\mmc.exe' /ve /d "$winexe"
for f in "$HERE"/theme/5[0-2]-*.reg; do wine regedit /S "Z:$(printf %s "$f" | tr / '\\')" >/dev/null 2>&1; done
wine sc create SgTestSvc binPath= 'C:\sg-svc-test.exe' DisplayName= 'Stained Glass Test Service' >/dev/null 2>&1
wine sc description SgTestSvc 'A service the gate starts and stops.' >/dev/null 2>&1
"$WSERVER" -w
export SG_MMC_DUMP="$windump"

wine explorer /desktop=shell,1280x800 > "$T/explorer.out" 2>&1 &
sleep 4

d() { [ -f "$DUMP" ] && tr -d '\r' < "$DUMP"; }
wait_dump() { i=0; while ! d | grep -Eq "$1" && [ $i -lt $(( ${2:-10} * 2 )) ]; do sleep 0.5; i=$((i + 1)); done; d | grep -Eq "$1"; }
row() { d | awk -F'\t' -v n="$1" '$1 ~ /^ROW / && $2 == n'; }
row_xy() { row "$1" | awk '{ print $3, $4; exit }'; }
col() { row "$1" | awk -F'\t' -v c="$2" '{ print $(c + 2); exit }'; }
verb_tb() { d | awk -v id="$1" '$1 == "VERB" && $2 == "row" && $3 == id { print $5, $6; exit }'; }
verb_ac() { d | awk -v id="$1" '$1 == "VERB" && $2 == "row" && $3 == id { print $7, $8; exit }'; }
verb_on() { d | awk -v id="$1" '$1 == "VERB" && $2 == "row" && $3 == id { print $4; exit }'; }
tree_xy() { d | awk -v n="$1" '$1 == "TREE" { t = $0; sub(/^TREE [0-9]+ -?[0-9]+ -?[0-9]+ (\* )?/, "", t); if (t == n) { print $3, $4; exit } }'; }
click() { xdotool mousemove "$1" "$2" click 1; sleep 0.8; }
shot() { import -window root "$OUT/services-$1.png" 2>/dev/null; }
info() { wine 'C:\sg-svc-test.exe' info SgTestSvc 2>/dev/null | tr -d '\r' | awk -v k="$1" '$1 == k { print $2 }'; }
scstate() { case "$(info STATE)" in 1) echo STOPPED ;; 2) echo START_PENDING ;; 3) echo STOP_PENDING ;; 4) echo RUNNING ;;
                                    7) echo PAUSED ;; *) echo "?" ;; esac; }
wait_state() { i=0; while [ "$(scstate)" != "$1" ] && [ $i -lt 30 ]; do sleep 0.5; i=$((i + 1)); done; [ "$(scstate)" = "$1" ]; }
svcpid() { info PID; }

# 1. services.msc, as the Run box and cmd name it
if [ -f "$C/windows/system32/services.msc" ]; then
    wine start services.msc >/dev/null 2>&1 &
    how="services.msc (wine-sg 0142: .msc file, mmc.exe launcher, App Paths)"
else
    wine "$winexe" services >/dev/null 2>&1 &
    how="sg-mmc directly (this Wine has no services.msc -- wine-sg 0142 missing)"
fi
if wait_dump '^CONSOLE services' 30 && wait_dump 'Stained Glass Test Service' 10; then pass "the Services console opens: $how"
else fail "the Services console did not open ($how)"; fi
sleep 1; shot open

[ "$(col 'Stained Glass Test Service' 1)" = "A service the gate starts and stops." ] && pass "the test service's description is listed" \
    || fail "description: '$(col 'Stained Glass Test Service' 1)'"
[ "$(col 'Stained Glass Test Service' 2)" = "" ] && [ "$(col 'Stained Glass Test Service' 3)" = "Manual" ] && \
    [ "$(col 'Stained Glass Test Service' 4)" = "Local System" ] && pass "stopped, Manual, Local System" \
    || fail "row: $(row 'Stained Glass Test Service')"
d | grep -q '^BRIDGED 1' && pass "bridged to sg-sysinfo" || fail "not bridged to sg-sysinfo ($SYSINFO)"

# 2. select it; Start on the toolbar
xy=$(row_xy 'Stained Glass Test Service')
# shellcheck disable=SC2086
[ -n "$xy" ] && [ "$xy" != "-1 -1" ] && click $xy
wait_dump "^ROW [0-9]+ [0-9-]+ [0-9-]+ 1	Stained Glass Test Service" 5 && pass "the row selects with a click" || fail "the row did not select"
[ "$(verb_on 1)" = 1 ] && [ "$(verb_on 2)" = 0 ] && pass "Start enabled, Stop disabled for a stopped service" \
    || fail "verbs for a stopped service: start $(verb_on 1) stop $(verb_on 2)"
# shellcheck disable=SC2086
click $(verb_tb 1)
wait_state RUNNING && pass "Start (toolbar): the SCM says RUNNING" || fail "Start: the SCM says $(scstate)"
wait_dump "	Stained Glass Test Service	[^	]*	Running	" 10 && pass "the row says Running" || fail "row after start: $(row 'Stained Glass Test Service')"
shot started

# 3. Pause (toolbar), Resume (Actions pane)
sleep 1
# shellcheck disable=SC2086
click $(verb_tb 3)
wait_state PAUSED && wait_dump "	Stained Glass Test Service	[^	]*	Paused	" 10 && pass "Pause (toolbar): PAUSED, the row says Paused" \
    || fail "Pause: $(scstate); $(row 'Stained Glass Test Service')"
sleep 1
# shellcheck disable=SC2086
click $(verb_ac 4)
wait_state RUNNING && pass "Resume (Actions pane): RUNNING" || fail "Resume: $(scstate)"

# 4. Restart (toolbar): a new process
pid1=$(svcpid)
sleep 1
# shellcheck disable=SC2086
click $(verb_tb 5)
sleep 2
wait_state RUNNING; pid2=$(svcpid)
[ -n "$pid1" ] && [ -n "$pid2" ] && [ "$pid1" != "$pid2" ] && [ "$(scstate)" = RUNNING ] && pass "Restart (toolbar): running again in a new process ($pid1 -> $pid2)" \
    || fail "Restart: $(scstate), pid $pid1 -> $pid2"

# 5. Stop (Actions pane)
sleep 1
# shellcheck disable=SC2086
click $(verb_ac 2)
wait_state STOPPED && pass "Stop (Actions pane): the SCM says STOPPED" || fail "Stop: $(scstate)"
wait_dump "	Stained Glass Test Service	[^	]*		Manual	" 10 && pass "the row's status is blank again" || fail "row after stop: $(row 'Stained Glass Test Service')"
shot stopped

# 6. Properties: Startup type Disabled
xdotool key alt+Return; sleep 2
shot properties
xdotool key alt+u; sleep 0.5; xdotool key d; sleep 0.5; xdotool key Return; sleep 2
[ "$(info START)" = 4 ] && pass "Properties: Startup type Disabled reaches the SCM" || fail "start type: $(info START)"
wait_dump "	Stained Glass Test Service	[^	]*		Disabled	" 5 && pass "the row says Disabled" || fail "row: $(row 'Stained Glass Test Service')"
[ "$(verb_on 1)" = 0 ] && pass "Start is disabled for a disabled service" || fail "Start enabled for a disabled service"

# 7. Stained Glass System Services (sg-sysinfo units, read-only)
want=$("$SYSINFO" units 2>/dev/null | grep -c '^UNIT ')
# shellcheck disable=SC2086
click $(tree_xy 'Stained Glass System Services')
if wait_dump '^NODE Stained Glass System Services' 5 && wait_dump "^ROWS $want\$" 10; then pass "Stained Glass System Services lists sg-sysinfo's $want units"
else fail "system services: $(d | grep -E '^(NODE|ROWS|EMPTY|UNITS)' | tr '\n' ' ') (want $want)"; fi
d | grep -q '^VERB row' && fail "a Linux unit offers actions" || pass "the Linux units are read-only (no actions)"
shot units

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
