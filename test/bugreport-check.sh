#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Report a problem (src/bugreport): testers right-click a program > "Run with
# a problem report", or open Report a problem from Start. The helpers collect
# the machine's details and run the program with Wine's debug logging; the
# window shows all of it, then copies, saves or sends it to the project's
# website (sg-bugreport-send: public, so ASCII and without the account and
# computer names; only when the tester says so -- no mail any more).
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
    grep -q "^The log's last 60 lines" "$T/run/highlights" && grep -q '^Errors' "$T/run/highlights" \
        && pass "the highlights are made" || fail "no highlights"
    "$(dirname "$WINE")/wineserver" -k 2>/dev/null
else
    echo "info  no wine at $WINE: the run is not tried"
fi

R="$HERE/defaults/87-sg-bugreport.reg"
grep -q 'exefile\\shell\\sgdebugreport\\command' "$R" && grep -q 'lnkfile\\shell\\sgdebugreport\\command' "$R" \
    && [ "$(grep -c '^@="Run with debugging"' "$R")" = 3 ] \
    && pass "programs and shortcuts have 'Run with debugging' (the name Start's menu uses)" || fail "no right-click verb"
grep -q -- '--run \\"%1\\"' "$R" && pass "the verb runs the file under the report" || fail "verb command"
grep -q 'Msi.Package\\shell\\sgdebugreport\\command' "$R" && grep -q 'msiexec.exe\\" /i \\"%1\\"' "$R" \
    && pass "an installer package (.msi) has it too, run by msiexec" || fail "no verb for .msi"
grep -q 'add_beside(L"Report a problem", L"sg-bugreport64.exe")' "$HERE/src/sg-start.c" && pass "Start lists Report a problem" || fail "not in Start"
! grep -qiE 'mailto:|smtp|sendmail|MAPISendMail' "$B/sg-bugreport.c" && grep -q 'MB_YESNO' "$B/sg-bugreport.c" \
    && pass "no mail; Send asks before anything goes" || fail "send path"

# the summary: a crash's place, a .NET exception, not the unwinding noise
mkdir -p "$T/sum"
{
    echo '0024:trace:loaddll:build_module Loaded L"C:\\App\\App.exe" at 00400000: native'
    echo '0024:trace:loaddll:build_module Loaded L"C:\\App\\Lib.dll" at 10000000: native'
    i=0; while [ $i -lt 40 ]; do echo '1.0:04e4:04e8:trace:seh:RtlUnwindEx code=80000026 flags=2 end_frame=0'; i=$((i + 1)); done
    echo '1.1:04e4:04e8:trace:seh:dispatch_exception code=c0000005 (EXCEPTION_ACCESS_VIOLATION) flags=0 addr=10001234'
    echo 'Unhandled Exception:'
    echo 'System.NullReferenceException: Object reference not set to an instance of an object'
    echo '  at App.Cache.Load ()'
    printf '%s\n' "[ERROR] FATAL UNHANDLED EXCEPTION: System.Reflection.TargetInvocationException: Exception has been thrown by the target of an invocation. ---> App.WrappedException: Rethrown exception. See innerexception for details, which is long enough to carry the cause past three hundred characters of text. ---> System.EntryPointNotFoundException: CreateInstalledObjectsInfo assembly:<unknown assembly> type:<unknown type> member:(null)"
} > "$T/sum/log"
echo 255 > "$T/sum/exit"
python3 "$B/sg-debug-summary" "$T/sum"
grep -q 'c0000005 EXCEPTION_ACCESS_VIOLATION.*Lib.dll+0x1234' "$T/sum/highlights" && pass "the summary names the exception and its module" || fail "exception: $(head -8 "$T/sum/highlights")"
grep -q 'System.NullReferenceException' "$T/sum/highlights" && pass "...and the .NET exception" || fail "no .NET exception"
grep -q '^The innermost .NET exception (the cause): System.EntryPointNotFoundException: CreateInstalledObjectsInfo assembly:<unknown assembly> type:<unknown type> member:(null)$' "$T/sum/highlights" \
    && pass "...the innermost of a chain of them, whole (the cause)" || fail "innermost: $(grep -i innermost "$T/sum/highlights")"
grep -q '80000026' "$T/sum/highlights" && fail "the longjmp unwinding is in the summary" || pass "...without the unwinding noise"

# Send: to a stand-in server, ASCII and without the account's name
cat > "$T/server.py" <<'PY'
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        open(sys.argv[1], "wb").write(body)
        self.send_response(201); self.end_headers(); self.wfile.write(b'{"id": "test-1"}')
    def log_message(self, *a): pass
s = http.server.HTTPServer(("127.0.0.1", 0), H)
open(sys.argv[2], "w").write(str(s.server_port))
s.handle_request()
PY
python3 "$T/server.py" "$T/got" "$T/port" & SP=$!
i=0; while [ ! -s "$T/port" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
printf 'Stained Glass OS problem report\nSession: user %s\nCaf\303\251 \342\200\223 done\n' "$(id -un)" > "$T/report.txt"
SG_REPORT_URL="http://127.0.0.1:$(cat "$T/port")/api/report" python3 "$B/sg-bugreport-send" "$T/report.txt" "$T/out"
wait $SP 2>/dev/null
[ "$(cat "$T/out")" = "OK test-1" ] && pass "Send posts the report and reads the answer" || fail "send: $(cat "$T/out" 2>/dev/null)"
LC_ALL=C grep -q '[^[:print:][:space:]]' "$T/got" && fail "non-ASCII was sent" || pass "...as ASCII (Cafe - done)"
grep -q "user <user>" "$T/got" && ! grep -q "$(id -un)" "$T/got" && pass "...without the account's name" || fail "the account's name was sent"
if [ -f "$HERE/build/sg-bugreport64.exe" ] && command -v wrestool >/dev/null; then
    wrestool -l -t 14 "$HERE/build/sg-bugreport64.exe" | grep -q group_icon && pass "it has an icon" || fail "no icon"
fi
grep -q 'build/sg-bugreport64.exe src/bugreport/sg-bugreport-info src/bugreport/sg-debug-run src/bugreport/sg-debug-summary src/bugreport/sg-bugreport-send' "$HERE/debian/rules" \
    && pass "packaged" || fail "not in debian/rules"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
