#!/bin/sh
# Set time zone automatically (admin/sg-timezone-auto) against a stand-in for
# freesoft.page's /api/timezone: a new PC (UTC, never set) gets its zone; a
# zone someone chose is left alone unless the setting is on; "off" is off; a
# zone this PC does not have, or a path, is refused; offline does nothing.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
T=$(mktemp -d); SP=; RC=0
trap '[ -n "$SP" ] && kill "$SP" 2>/dev/null; rm -rf "$T"' EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
cat > "$T/server.py" <<'PY'
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        z = open(sys.argv[1]).read().strip()
        self.send_response(200); self.end_headers()
        self.wfile.write(('{"timezone": "%s", "country": "US"}' % z).encode())
    def log_message(self, *a): pass
s = http.server.HTTPServer(("127.0.0.1", 0), H)
open(sys.argv[2], "w").write(str(s.server_port))
s.serve_forever()
PY
echo America/Los_Angeles > "$T/zone"
python3 "$T/server.py" "$T/zone" "$T/port" & SP=$!
i=0; while [ ! -s "$T/port" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
printf '#!/bin/sh\necho "$1" > %s/set\n' "$T" > "$T/setzone"; chmod +x "$T/setzone"
run() {   # current-zone [setting]
    echo "$1" > "$T/current"; rm -f "$T/set" "$T/setting"
    [ $# -gt 1 ] && echo "$2" > "$T/setting"
    SG_TZ_URL="http://127.0.0.1:$(cat "$T/port")/api/timezone" SG_TZ_SETTING="$T/setting" SG_TZ_CURRENT="$T/current" \
        SG_TZ_SET="$T/setset" python3 "$HERE/admin/sg-timezone-auto" >/dev/null 2>&1
    cat "$T/set" 2>/dev/null
}
cp "$T/setzone" "$T/setset"
[ "$(run UTC)" = America/Los_Angeles ] && pass "a new PC (UTC, never set) gets its zone" || fail "UTC: $(run UTC)"
[ -z "$(run Europe/Berlin)" ] && pass "a zone someone chose is left alone" || fail "a chosen zone was changed"
[ "$(run Europe/Berlin on)" = America/Los_Angeles ] && pass "...unless the setting is on" || fail "on did not set"
[ -z "$(run UTC off)" ] && pass "off is off" || fail "off set a zone"
[ -z "$(run America/Los_Angeles on)" ] && pass "the same zone is not set again" || fail "set the same zone"
echo 'Mars/Olympus' > "$T/zone"; [ -z "$(run UTC on)" ] && pass "a zone this PC does not have is refused" || fail "a made-up zone was set"
echo '../../etc/passwd' > "$T/zone"; [ -z "$(run UTC on)" ] && pass "a path is refused" || fail "a path was set"
kill "$SP"; SP=; echo America/Denver > "$T/zone"
[ -z "$(run UTC on)" ] && pass "offline: nothing changes" || fail "offline set a zone"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
