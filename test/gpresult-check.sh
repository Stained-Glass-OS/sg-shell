#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for the Group Policy result tool (sg-gpresult): it reports the machine
# and user policy that is actually in force, read from the live registry. To
# prove it reads real policy rather than printing a canned report, the gate
# plants a known machine policy value and a known user policy value and requires
# both to appear -- and requires a key it did NOT set to be absent.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
GP="$HERE/build/sg-gpresult64.exe"
RC=0; T=$(mktemp -d); chmod 755 "$T"
export HOME="$T"
# shellcheck disable=SC2317
cleanup() { WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

[ -x "$WINE_DIR/bin/wine" ] && [ -f "$GP" ] || { echo "SKIP: wine-sg or sg-gpresult not built"; exit 77; }
export WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w

# Plant a machine policy (HKLM) and a user policy (HKCU), plus a non-policy key
# that must never be reported.
wine reg add 'HKLM\Software\Microsoft\Windows\CurrentVersion\Policies\System' \
    /v EnableLUA /t REG_DWORD /d 1 /f >/dev/null 2>&1
wine reg add 'HKCU\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer' \
    /v NoDrives /t REG_DWORD /d 4 /f >/dev/null 2>&1
wine reg add 'HKLM\Software\SGNotAPolicy' \
    /v Decoy /t REG_SZ /d nope /f >/dev/null 2>&1

out=$(wine "$GP" 2>/dev/null </dev/null | tr -d '\r')

echo "$out" | grep -q '^Computer name: ' && pass "reports the computer name" || fail "no computer name line"
echo "$out" | grep -q '^User name: ' && pass "reports the user name" || fail "no user name line"
echo "$out" | grep -q 'COMPUTER SETTINGS' && pass "has a COMPUTER SETTINGS section" || fail "no COMPUTER SETTINGS section"
echo "$out" | grep -q 'USER SETTINGS' && pass "has a USER SETTINGS section" || fail "no USER SETTINGS section"

# teeth: the planted machine and user policies must appear...
echo "$out" | grep -q 'System\\EnableLUA = 1 (REG_DWORD)' \
    && pass "reports the planted machine policy (EnableLUA)" || fail "planted machine policy missing"
echo "$out" | grep -q 'Explorer\\NoDrives = 4 (REG_DWORD)' \
    && pass "reports the planted user policy (NoDrives)" || fail "planted user policy missing"
# ...and a non-policy key must NOT.
if echo "$out" | grep -q 'SGNotAPolicy'; then fail "reported a non-policy key (SGNotAPolicy)"; else pass "ignores non-policy keys"; fi

# The section that has a setting must not also say "(none in force)".
comp_block=$(echo "$out" | awk '/COMPUTER SETTINGS/{f=1} /USER SETTINGS/{f=0} f')
if echo "$comp_block" | grep -q '(none in force)'; then fail "COMPUTER SETTINGS wrongly says none in force"; else pass "COMPUTER SETTINGS counts its settings"; fi

echo
if [ "$RC" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
