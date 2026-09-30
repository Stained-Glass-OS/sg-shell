#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Report a problem (src/bugreport): testers right-click a program > "Run with
# a problem report", or open Report a problem from Start. The helpers collect
# the machine's details and run the program with Wine's debug logging; the
# window shows all of it, then copies, saves or emails it (mailto:, the
# tester's own mail program -- nothing is sent by the tool).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
B="$HERE/src/bugreport"
command -v shellcheck >/dev/null && { shellcheck -s sh "$B/sg-bugreport-info" "$B/sg-debug-run" && pass "the helpers are clean sh" || fail "shellcheck"; }

T=$(mktemp -d /var/tmp/sg-bugreport.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM
sh "$B/sg-bugreport-info" "$T/info.txt"
for k in System Kernel CPU Memory Session Packages; do
    grep -q "^$k:" "$T/info.txt" && pass "the system details name the $k" || fail "no $k line"
done

# a program's run: its exit status, and the log's highlights
WINE="${WINE:-/opt/wine-sg/bin/wine}"
if [ -x "$WINE" ]; then
    export WINEPREFIX="$T/prefix" WINELOADER="$WINE" WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" DISPLAY=
    timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1
    timeout -s KILL 120 sh "$B/sg-debug-run" "$T/run" --cd /tmp cmd.exe /c "exit 3"
    rc=$?
    [ "$rc" = 3 ] && [ "$(cat "$T/run/exit" 2>/dev/null)" = 3 ] && pass "the program's exit status comes back (3)" || fail "exit status $rc / $(cat "$T/run/exit" 2>/dev/null)"
    grep -q 'loaddll' "$T/run/log" && pass "Wine's debug log is written" || fail "no debug log"
    grep -q "^The log's last 60 lines:" "$T/run/highlights" && grep -q '^Errors' "$T/run/highlights" \
        && pass "the highlights are made" || fail "no highlights"
    "$(dirname "$WINE")/wineserver" -k 2>/dev/null
else
    echo "info  no wine at $WINE: the run is not tried"
fi

R="$HERE/defaults/87-sg-bugreport.reg"
grep -q 'exefile\\shell\\sgdebugreport\\command' "$R" && grep -q 'lnkfile\\shell\\sgdebugreport\\command' "$R" \
    && [ "$(grep -c '^@="Run with debugging"' "$R")" = 2 ] \
    && pass "programs and shortcuts have 'Run with debugging' (the name Start's menu uses)" || fail "no right-click verb"
grep -q -- '--run \\"%1\\"' "$R" && pass "the verb runs the file under the report" || fail "verb command"
grep -q 'add_beside(L"Report a problem", L"sg-bugreport64.exe")' "$HERE/src/sg-start.c" && pass "Start lists Report a problem" || fail "not in Start"
grep -q 'mailto:" REPORT_TO' "$B/sg-bugreport.c" && ! grep -qiE 'smtp|sendmail|MAPISendMail' "$B/sg-bugreport.c" \
    && pass "email goes through the tester's mail program (mailto:), never sent by the tool" || fail "email path"
if [ -f "$HERE/build/sg-bugreport64.exe" ] && command -v wrestool >/dev/null; then
    wrestool -l -t 14 "$HERE/build/sg-bugreport64.exe" | grep -q group_icon && pass "it has an icon" || fail "no icon"
fi
grep -q 'build/sg-bugreport64.exe src/bugreport/sg-bugreport-info src/bugreport/sg-debug-run' "$HERE/debian/rules" \
    && pass "packaged" || fail "not in debian/rules"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
