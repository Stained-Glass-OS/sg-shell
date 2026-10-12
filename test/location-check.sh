#!/bin/sh
# The device's location switch (admin/sg-location) against a stand-in
# systemctl: off masks and stops GeoClue, on unmasks it; "apply" keeps a
# recorded choice, and with none follows the shared prefix's registry (the
# first-run setup's choice): Deny is off, Allow or nothing is on. Mutants: the
# hive's Deny not read, off not stopping GeoClue.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT INT TERM
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
TOOL="$HERE/admin/sg-location"
cat > "$T/systemctl" <<EOF
#!/bin/sh
echo "\$*" >> "$T/calls"
EOF
chmod +x "$T/systemctl"
hive() {   # Allow|Deny|none
    {
        printf 'WINE REGISTRY Version 2\n;; All keys relative to \\\\Machine\n\n'
        printf '[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\CapabilityAccessManager\\\\ConsentStore\\\\microphone] 1760000000\n"Value"="Deny"\n\n'
        [ "$1" = none ] || printf '[Software\\\\Microsoft\\\\Windows\\\\CurrentVersion\\\\CapabilityAccessManager\\\\ConsentStore\\\\location] 1760000000\n#time=1dc\n"Value"="%s"\n\n' "$1"
        printf '[Software\\\\Other] 1760000000\n"Value"="Deny"\n'
    } > "$T/system.reg"
}
run() {   # TOOL ARGS... -- prints what systemctl was asked
    t=$1; shift
    : > "$T/calls"
    SG_LOCATION_SETTING="${T:?}/cfg/location" SG_LOCATION_HIVE="$T/system.reg" SG_LOCATION_SYSTEMCTL="$T/systemctl" \
        sh "$t" "$@" >/dev/null 2>&1
    tr '\n' ';' < "$T/calls"
}
check() {   # TOOL -- 0 when every case holds
    t=$1; ok=0
    rm -rf "${T:?}/cfg"; hive Allow
    [ "$(run "$t" off)" = "mask geoclue.service;stop geoclue.service;" ] && [ "$(cat "${T:?}/cfg/location")" = off ] || ok=1
    [ "$(run "$t" on)" = "unmask geoclue.service;" ] && [ "$(cat "${T:?}/cfg/location")" = on ] || ok=1
    echo off > "${T:?}/cfg/location"
    [ "$(run "$t" apply)" = "mask geoclue.service;stop geoclue.service;" ] || ok=1
    rm -rf "${T:?}/cfg"; hive Deny
    [ "$(run "$t" apply)" = "mask geoclue.service;stop geoclue.service;" ] || ok=1
    [ ! -e "${T:?}/cfg/location" ] || ok=1
    hive Allow
    [ "$(run "$t" apply)" = "unmask geoclue.service;" ] || ok=1
    hive none
    [ "$(run "$t" apply)" = "unmask geoclue.service;" ] || ok=1
    rm -f "$T/system.reg"
    [ "$(run "$t" apply)" = "unmask geoclue.service;" ] || ok=1
    run "$t" maybe >/dev/null; [ ! -s "$T/calls" ] || ok=1
    return $ok
}

rm -rf "${T:?}/cfg"; hive Allow
[ "$(run "$TOOL" off)" = "mask geoclue.service;stop geoclue.service;" ] && [ "$(cat "${T:?}/cfg/location")" = off ] \
    && pass "off masks and stops GeoClue, and is recorded" || fail "off: $(cat "$T/calls")"
[ "$(run "$TOOL" on)" = "unmask geoclue.service;" ] && [ "$(cat "${T:?}/cfg/location")" = on ] \
    && pass "on unmasks it" || fail "on: $(cat "$T/calls")"
echo off > "${T:?}/cfg/location"
[ "$(run "$TOOL" apply)" = "mask geoclue.service;stop geoclue.service;" ] && pass "apply keeps a recorded off (the registry says Allow)" || fail "apply recorded"
rm -rf "${T:?}/cfg"; hive Deny
[ "$(run "$TOOL" apply)" = "mask geoclue.service;stop geoclue.service;" ] && pass "never recorded: the setup's Deny in the shared registry is off" || fail "apply Deny: $(cat "$T/calls")"
hive Allow
[ "$(run "$TOOL" apply)" = "unmask geoclue.service;" ] && pass "...its Allow is on" || fail "apply Allow"
hive none
[ "$(run "$TOOL" apply)" = "unmask geoclue.service;" ] && pass "...no switch there (other keys' Deny ignored) is on, as Settings shows it" || fail "apply none"
run "$TOOL" maybe >/dev/null
[ ! -s "$T/calls" ] && pass "anything else changes nothing" || fail "maybe called systemctl"
check "$TOOL" && pass "all cases together" || fail "the cases together"

# the agent GeoClue waits for is the one its configuration allows
id=$(sed -n 's/^AGENT_ID = "\([^"]*\)".*/\1/p' "$HERE/admin/sg-geoclue-agent")
[ -n "$id" ] && grep -qx "whitelist=$id" "$HERE/admin/geoclue-stained-glass.conf" \
    && pass "GeoClue's configuration allows our agent ($id)" || fail "agent id '$id' not in the whitelist"
python3 -c "import ast,sys; ast.parse(open(sys.argv[1]).read())" "$HERE/admin/sg-geoclue-agent" \
    && pass "the agent parses" || fail "the agent does not parse"

# mutants
sed 's/\[Dd\]\[Ee\]\[Nn\]\[Yy\]) v=off/NEVER) v=off/' "$TOOL" > "$T/m1"
check "$T/m1" && fail "MUTANT NODENY survived" || pass "MUTANT NODENY (the setup's Deny not read) is caught"
sed '/" stop geoclue.service/d' "$TOOL" > "$T/m2"
check "$T/m2" && fail "MUTANT NOSTOP survived" || pass "MUTANT NOSTOP (off leaves GeoClue running) is caught"
cmp -s "$T/m1" "$TOOL" && fail "mutant NODENY did not apply"
cmp -s "$T/m2" "$TOOL" && fail "mutant NOSTOP did not apply"
exit $RC
