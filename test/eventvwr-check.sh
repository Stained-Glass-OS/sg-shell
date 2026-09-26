#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2046,SC2155,SC2086
# Gate for Event Viewer (eventvwr.exe / eventvwr.msc, sg-mmc), under Xvfb on a
# shell desktop of a SHARED prefix -- this user owns it (SYSTEM there) and the
# second Unix user SG_OTHER (default sgconf) is a standard user -- reading
# what the console shows from SG_MMC_DUMP:
#
#   - eventvwr.exe /c:Application (wine-sg 0142's launcher, App Paths) opens
#     the Application log
#   - an event written with ReportEvent from a source with a message file is
#     listed (Information, source, ID 1000) and its preview is the message
#     text with the event's strings inserted; one from a source with no
#     message file gets Windows' "The description for Event ID ... cannot be
#     found" and its strings
#   - an event reported while the log is open appears by itself
#   - Filter Current Log (Actions pane), Warning only: only warnings, the
#     header says Filtered; Clear Filter restores them
#   - System holds the event log service's own "started" event (6005), text
#     from its message table
#   - Security: an audit event SYSTEM wrote is listed for the owner; for the
#     standard user the log is refused ("You do not have permission to read
#     the Security log") while Application still reads
#   - the Stained Glass log lists the journal (sg-sysinfo, a stand-in
#     journalctl) with its messages
#
# Needs wine-sg with the event log (0143/0144) and 0140, Xvfb, xdotool,
# ImageMagick, mingw (windmc, windres) and passwordless `sudo -u $SG_OTHER`;
# skips (77) without them. SG_MMC_EXE tests another build (mutant:
# -DSG_MUTANT_NOFORMAT); SG_WINE_DIR another Wine (a build tree works).
# Screenshots: build/eventvwr-*.png.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_MMC_EXE:-$HERE/build/sg-eventvwr64.exe}"
OUT="$HERE/build"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
PFXT="${MINGW%-gcc}"
SYSINFO="${SG_SYSINFO:-$HERE/../sg-session/bin/sg-sysinfo}"
[ -x "$SYSINFO" ] || SYSINFO=/usr/bin/sg-sysinfo
SYSINFO=$(readlink -f "$SYSINFO")
SG_OTHER=${SG_OTHER:-sgconf}
SG_GROUP=${SG_GROUP:-sgconfgrp}
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool xhost import "$MINGW" "$PFXT-windmc" "$PFXT-windres"; do
    command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }
done
if [ -x "$WINE_DIR/bin/wine" ]; then WBIN="$WINE_DIR/bin"; WSERVER="$WINE_DIR/bin/wineserver"
elif [ -x "$WINE_DIR/wine" ]; then WBIN="$WINE_DIR"; WSERVER="$WINE_DIR/server/wineserver"
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi
[ -f "$EXE" ] || { echo "SKIP: $EXE missing"; exit 77; }
id "$SG_OTHER" >/dev/null 2>&1 && sudo -n -u "$SG_OTHER" true 2>/dev/null || { echo "SKIP: no $SG_OTHER or sudo"; exit 77; }

T=$(mktemp -d /var/tmp/sg-eventvwr-check.XXXXXX); chmod 755 "$T"
mkdir "$T/out"; chmod 777 "$T/out"
# shellcheck disable=SC2317
cleanup() {
    set +e
    WINEPREFIX="$T/pfx" "$WSERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    sleep 1
    sudo -n rm -rf "$T"
}
trap cleanup EXIT INT TERM

# the gate's message file, and the probe that writes events
( cd "$T" && "$PFXT-windmc" -U -h . -r . "$HERE/test/sg-evt-msg.mc" && "$PFXT-windres" sg-evt-msg.rc -O coff -o msg.o &&
  "$MINGW" -shared -nostdlib -Wl,-e,0 -o sg-evt-msg.dll msg.o ) >/dev/null || { fail "message file did not build"; exit 1; }
"$MINGW" -O2 -municode -o "$T/sg-evt-probe.exe" "$HERE/test/sg-evt-probe.c" || { fail "probe did not build"; exit 1; }
chmod 755 "$T"/*.exe "$T"/*.dll

# a stand-in journalctl for the Stained Glass log
cat > "$T/journalctl" <<'EOF'
#!/bin/sh
printf '%s\n' '{"__REALTIME_TIMESTAMP":"1790000000000000","PRIORITY":"3","_SYSTEMD_UNIT":"sg-netd@1.service","SYSLOG_IDENTIFIER":"sg-netd","_PID":"4242","__CURSOR":"c1","MESSAGE":"sg-netd: the gate journal line"}'
printf '%s\n' '{"__REALTIME_TIMESTAMP":"1789999999000000","PRIORITY":"6","_SYSTEMD_UNIT":"sshd.service","SYSLOG_IDENTIFIER":"sshd","_PID":"99","__CURSOR":"c0","MESSAGE":"not ours"}'
EOF
chmod 755 "$T/journalctl"

Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$T/dpy" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/dpy" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/dpy")"
xhost "+SI:localuser:$SG_OTHER" >/dev/null 2>&1

PFX="$T/pfx"
mkdir "$PFX"; chgrp "$SG_GROUP" "$PFX"; chmod 2770 "$PFX"; touch "$PFX/.sg-system-prefix"
export WINEPREFIX="$PFX" WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all PATH="$WBIN:$PATH"
export SG_SYSINFO="$SYSINFO" SG_JOURNALCTL="$T/journalctl"
sg "$SG_GROUP" -c "umask 002; '$WSERVER' -p"
sg "$SG_GROUP" -c "umask 002; wine wineboot -i" >/dev/null 2>&1
chmod -R g+rwX "$PFX" 2>/dev/null
C="$PFX/drive_c"
cp "$T/sg-evt-msg.dll" "$T/sg-evt-probe.exe" "$C/"
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x800
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\eventvwr.exe' /ve /d "$winexe"
for f in "$HERE"/theme/5[0-2]-*.reg; do wine regedit /S "Z:$(printf %s "$f" | tr / '\\')" >/dev/null 2>&1; done
other() {
    sudo -n -u "$SG_OTHER" env DISPLAY="$DISPLAY" WINEPREFIX="$PFX" WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=' \
        HOME=/var/tmp PATH="$PATH" SG_SYSINFO="$SYSINFO" SG_JOURNALCTL="$T/journalctl" "$@"
}
other wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
other wine reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x800 /f >/dev/null 2>&1
for f in "$HERE"/theme/5[0-2]-*.reg; do other wine regedit /S "Z:$(printf %s "$f" | tr / '\\')" >/dev/null 2>&1; done

P() { wine 'C:\sg-evt-probe.exe' "$@" 2>/dev/null | tr -d '\r' | sed -n 's/^RESULT //p'; }
[ "$(P register Application SgGate 'C:\sg-evt-msg.dll')" = 0 ] || fail "registering the gate's source"
[ "$(P report Application SgGate info 1000 'the gate' 'Thursday')" = 0 ] && pass "ReportEvent (Application, a registered source)" \
    || fail "ReportEvent failed: is this a wine-sg with the event log (0143/0144)?"
P report Application SgNoMsg warning 42 'string one' 'string two' >/dev/null
P register Security SgGateAudit 'C:\sg-evt-msg.dll' >/dev/null
[ "$(P report Security SgGateAudit success 4624 'alice')" = 0 ] && pass "SYSTEM (the prefix owner) writes a Security event" \
    || fail "SYSTEM could not write to Security"

wine explorer /desktop=shell,1280x800 > "$T/explorer.out" 2>&1 &
sleep 5

DUMP="$T/out/dump.txt"
windump=$(wine winepath -w "$DUMP" 2>/dev/null | tr -d '\r')
d() { [ -f "$DUMP" ] && tr -d '\r' < "$DUMP"; }
wait_dump() { i=0; while ! d | grep -Eq "$1" && [ $i -lt $(( ${2:-10} * 2 )) ]; do sleep 0.5; i=$((i + 1)); done; d | grep -Eq "$1"; }
row_xy() { d | awk -F'\t' -v s="$1" '$1 ~ /^ROW / && $4 == s { split($1, a, " "); print a[3], a[4]; exit }'; }
tree_xy() { d | awk -v n="$1" '$1 == "TREE" { t = $0; sub(/^TREE [0-9]+ -?[0-9]+ -?[0-9]+ (\* )?/, "", t); if (t == n) { print $3, $4; exit } }'; }
link_xy() { d | awk -v n="$1" '$1 == "LINK" && $2 == 0 { t = $0; sub(/^LINK 0 -?[0-9]+ -?[0-9]+ /, "", t); if (t == n) { print $3, $4; exit } }'; }
click() { [ $# -ge 2 ] || return 0; xdotool mousemove "$1" "$2" click 1; sleep 1; }
shot() { import -window root "$OUT/eventvwr-$1.png" 2>/dev/null; }

# 1. eventvwr.exe /c:Application
if [ -f "$C/windows/system32/eventvwr.exe" ]; then
    SG_MMC_DUMP="$windump" wine eventvwr.exe /c:Application >/dev/null 2>&1 &
    how="eventvwr.exe (wine-sg 0142's launcher, App Paths)"
else
    SG_MMC_DUMP="$windump" wine "$winexe" /c:Application >/dev/null 2>&1 &
    how="sg-eventvwr directly (no eventvwr.exe in this Wine)"
fi
wait_dump '^NODE Application' 30 && wait_dump '	SgGate	1000	' 10 && pass "Event Viewer opens on Application: $how" \
    || fail "Application log: $(d | grep -E '^(NODE|LOG|ROWS|EMPTY)' | tr '\n' ' ')"
d | grep -q '^ROW [0-9]* [0-9-]* [0-9-]* [01]	Information	[^	]*	SgGate	1000	None' && pass "the event is listed: Information, SgGate, 1000" \
    || fail "SgGate's row: $(d | grep 'SgGate')"
click $(row_xy SgGate)
wait_dump '^PREVIEW The Event Viewer gate wrote this event: the gate happened on Thursday\.' 5 \
    && pass "its text: the source's message with the event's strings inserted" || fail "preview: $(d | grep '^PREVIEW')"
d | grep -q '^PREVIEW.*Event ID:	1000 .*Level:	Information' && pass "the preview's fields (Event ID, Level)" || fail "preview fields"
shot application
click $(row_xy SgNoMsg)
wait_dump '^PREVIEW The description for Event ID 42 from source SgNoMsg cannot be found.*string one.*string two' 5 \
    && pass "a source without a message file: Windows' 'cannot be found' text and the strings" || fail "SgNoMsg preview: $(d | grep '^PREVIEW')"

# 2. a new event appears by itself
P report Application SgGate error 1001 'the live check' >/dev/null
wait_dump '	Error	[^	]*	SgGate	1001	' 8 && pass "an event reported while the log is open appears by itself" || fail "the new event did not appear"

# 3. Filter Current Log: Warning only
click $(link_xy 'Filter Current Log...')
sleep 1; shot filter
xdotool key alt+w; sleep 0.3; xdotool key Return; sleep 1.5
if wait_dump '^FILTERED 1' 5 && wait_dump '^ROWS 1$' 5 && d | grep -q '	Warning	[^	]*	SgNoMsg	42	'; then
    pass "Filter Current Log, Warning only: just the warning"
else fail "filter: $(d | grep -E '^(FILTERED|ROWS|HEADER)' | tr '\n' ' ')"; fi
d | grep -q '^HEADER Application.*Filtered: showing 1 of' && pass "the header says Filtered" || fail "header: $(d | grep '^HEADER')"
click $(link_xy 'Clear Filter')
wait_dump '^FILTERED 0' 5 && wait_dump '^ROWS [3-9]' 5 && pass "Clear Filter shows every event again" || fail "clear filter"

# 4. System: the event log service's own start event
click $(tree_xy System)
if wait_dump '^NODE System' 5 && wait_dump '	EventLog	6005	' 8; then
    click $(row_xy EventLog)
    wait_dump '^PREVIEW The Event log service was started' 5 && pass "System: EventLog 6005, 'The Event log service was started.'" \
        || fail "6005's text: $(d | grep '^PREVIEW')"
else fail "System: $(d | grep -E '^(NODE|ROWS|EMPTY)' | tr '\n' ' ')"; fi

# 5. Security for the owner
click $(tree_xy Security)
wait_dump '^NODE Security' 5 && wait_dump '	Audit Success	[^	]*	SgGateAudit	4624	' 8 && pass "Security lists SYSTEM's audit event for the owner" \
    || fail "owner's Security: $(d | grep -E '^(NODE|LOGERR|ROWS|EMPTY)' | tr '\n' ' ')"
shot security-admin

# 6. the Stained Glass log (journal through sg-sysinfo)
click $(tree_xy 'Stained Glass')
if wait_dump '^NODE Stained Glass' 5 && wait_dump '	Error	[^	]*	sg-netd	4242	' 10; then
    pass "Stained Glass lists the journal's sg-* entry"
    click $(row_xy sg-netd)
    wait_dump '^PREVIEW sg-netd: the gate journal line' 5 && pass "its message in the preview" || fail "journal preview: $(d | grep '^PREVIEW')"
else fail "Stained Glass log: $(d | grep -E '^(NODE|BRIDGED|ROWS|EMPTY)' | tr '\n' ' ')"; fi
d | grep -q 'not ours' && fail "a non-Stained Glass journal entry is shown" || pass "only Stained Glass's own journal entries"
shot journal
wine taskkill /f /im sg-eventvwr64.exe >/dev/null 2>&1
sleep 1

# 7. the standard user: Security refused, Application readable
ODUMP="$T/out/other.txt"
winodump=$(wine winepath -w "$ODUMP" 2>/dev/null | tr -d '\r')
other env SG_MMC_DUMP="$winodump" wine "$winexe" /c:Security >/dev/null 2>&1 &
i=0; while ! { [ -f "$ODUMP" ] && tr -d '\r' < "$ODUMP" | grep -q '^NODE Security'; } && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
sleep 2
od() { [ -f "$ODUMP" ] && tr -d '\r' < "$ODUMP"; }
od | grep -q '^ADMIN 0' && pass "the second user is a standard user" || fail "the second user: $(od | grep '^ADMIN')"
if od | grep -q '^LOGERR 5' && od | grep -q '^EMPTY You do not have permission to read the Security log' && ! od | grep -q 'SgGateAudit'; then
    pass "Security is refused to the standard user (Access is denied), no events shown"
else fail "standard user's Security: $(od | grep -E '^(NODE|LOGERR|ROWS|EMPTY)' | tr '\n' ' ')"; fi
od | grep -q '^BANNER Only an administrator can read this log' && pass "the banner says why" || fail "banner: $(od | grep '^BANNER')"
shot security-standard
xy=$(od | awk -v n=Application '$1 == "TREE" { t = $0; sub(/^TREE [0-9]+ -?[0-9]+ -?[0-9]+ (\* )?/, "", t); if (t == n) { print $3, $4; exit } }')
click $xy
i=0; while ! od | grep -q '	SgGate	1000	' && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
od | grep -q '	SgGate	1000	' && pass "the standard user reads Application" || fail "standard user's Application: $(od | grep -E '^(NODE|LOGERR|ROWS|EMPTY)' | tr '\n' ' ')"

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
