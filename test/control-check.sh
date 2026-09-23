#!/bin/sh
# Gate for the Control Panel (sg-control): it reads the machine's real state
# (via --dump, deterministic and headless) and its window appears and paints.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
CTL="$HERE/build/sg-control64.exe"
RC=0; DPY=90; T=$(mktemp -d); chmod 755 "$T"; XP=""
export HOME="$T"
# shellcheck disable=SC2317
cleanup() { pkill -f 'sg-control64' 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null; rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

[ -x "$WINE_DIR/bin/wine" ] && [ -f "$CTL" ] || { echo "SKIP: wine-sg or sg-control not built"; exit 77; }
export WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w

# 1. --dump reports real state.
out=$(wine "$CTL" --dump 2>/dev/null </dev/null | tr -d '\r')
echo "$out" | grep -q '^edition=Stained Glass OS$' && pass "reports the edition" || fail "edition wrong: $(echo "$out" | grep '^edition=')"
comp=$(echo "$out" | sed -n 's/^computer=//p')
[ -n "$comp" ] && [ "$comp" != "(unknown)" ] && pass "reports the computer name ($comp)" || fail "no computer name"
usr=$(echo "$out" | sed -n 's/^user=//p')
[ -n "$usr" ] && [ "$usr" != "(unknown)" ] && pass "reports the signed-in user ($usr)" || fail "no user name"
echo "$out" | grep -q '^arch=' && pass "reports the system type" || fail "no system type"
echo "$out" | grep -q '^policies=[0-9]' && pass "reports the policy count" || fail "no policy count"
echo "$out" | grep -qE '^osbuild=.+' && pass "reports the OS build" || fail "no OS build"
echo "$out" | grep -qE '^cpu=.+' && pass "reports the processor" || fail "no processor"
echo "$out" | grep -qE '^ram=[0-9].* GB' && pass "reports installed RAM ($(echo "$out" | sed -n 's/^ram=//p'))" || fail "no RAM"
echo "$out" | grep -qE '^programs=[0-9]+' && pass "reports installed program count" || fail "no program count"

# 2. the window appears (needs an X server; skip that half if none).
if command -v Xvfb >/dev/null && command -v xdotool >/dev/null; then
    rm -f "/tmp/.X${DPY}-lock"
    Xvfb ":$DPY" -screen 0 1280x800x24 -ac >/dev/null 2>&1 & XP=$!; sleep 2
    export DISPLAY=":$DPY"
    wine "$CTL" >/dev/null 2>&1 &
    W=""; _w=0
    while [ $_w -lt 20 ]; do W=$(xdotool search --name 'Control Panel' 2>/dev/null | head -1); [ -n "$W" ] && break; sleep 1; _w=$((_w+1)); done
    [ -n "$W" ] && pass "the Control Panel window appears" || fail "no Control Panel window"
else
    echo "info  no X server; skipped the window-appears check"
fi

echo
if [ "$RC" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
