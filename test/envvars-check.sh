#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Environment Variables (sg-control /envvars, src/control/envvars.c), driven
# like a person on the X keyboard under Xvfb (David 2026-10-01: Claude Code
# asked for PATH to be changed, and nothing in Settings or Control Panel was
# found by "Environment"):
#   - the dialog lists the person's variables (Path among them)
#   - New... adds a variable; Path opens as a list of its entries, and New
#     there adds one; OK writes them to HKCU\Environment
#   - Cancel writes nothing
#   - Settings' search and Control Panel's find it ("environment", "path")
# Needs wine-sg, Xvfb, xdotool; skips (77) without them. SG_CONTROL_EXE runs
# another build (the mutation test).
# shellcheck disable=SC2015  # pass/fail one-liners
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_CONTROL_EXE:-$HERE/build/sg-control64.exe}"
RC=0; XP=""
pass() { printf "PASS  %s\n" "$*"; }
fail() { printf "FAIL  %s\n" "$*"; RC=1; }
for need in Xvfb xdotool; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-envvars-check.XXXXXX)
# shellcheck disable=SC2317
cleanup() { WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
for n in $(seq 120 160); do [ -e "/tmp/.X11-unix/X$n" ] || break; done
Xvfb ":$n" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$n" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
wine reg add 'HKCU\Environment' /v Path /t REG_SZ /d 'C:\a;C:\b' /f >/dev/null 2>&1
wineserver -w
regval() { wine reg query 'HKCU\Environment' /v "$1" 2>/dev/null | tr -d '\r' | sed -n "s/^ *$1 *REG_[A-Z_]* *//p"; }
win() { i=0; while [ $i -lt 60 ]; do w=$(xdotool search --onlyvisible --name "^$1\$" 2>/dev/null | head -1); [ -n "$w" ] && { echo "$w"; return 0; }; sleep 0.25; i=$((i + 1)); done; return 1; }
keys() { sleep 0.4; xdotool key --clearmodifiers "$@"; }
typ() { sleep 0.3; xdotool type --delay 30 "$1"; }

# --- OK: a new variable, and an entry added to Path
wine "$EXE" /envvars >/dev/null 2>&1 & P=$!
W=$(win "Environment Variables") && pass "the dialog opens (sg-control /envvars)" || { fail "no Environment Variables window"; exit 1; }
xdotool windowfocus --sync "$W" 2>/dev/null; xdotool windowactivate "$W" 2>/dev/null
keys alt+n
F=$(win "New User Variable") && pass "New... asks for a name and a value" || fail "no New User Variable form"
xdotool windowfocus "$F" 2>/dev/null; typ SG_GATE_VAR; keys Tab; typ 'hello gate'; keys Return
sleep 0.6; xdotool windowfocus "$W" 2>/dev/null
# Path is the first row (sorted); Edit opens it as a list of entries
keys Home; keys alt+e
L=$(win "Edit environment variable Path") && pass "Edit on Path opens its entries" || fail "no list editor for Path"
xdotool windowfocus "$L" 2>/dev/null; keys End; keys alt+n
E=$(win "New entry") && { xdotool windowfocus "$E" 2>/dev/null; typ 'C:\added'; keys Return; } || fail "no New entry form"
sleep 0.6; xdotool windowfocus "$L" 2>/dev/null; keys Return
sleep 0.6; xdotool windowfocus "$W" 2>/dev/null; keys Return
i=0; while kill -0 $P 2>/dev/null && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
wineserver -w
[ "$(regval SG_GATE_VAR)" = "hello gate" ] && pass "OK writes the new variable to HKCU\\Environment" || fail "SG_GATE_VAR: '$(regval SG_GATE_VAR)'"
[ "$(regval Path)" = 'C:\a;C:\b;C:\added' ] && pass "and Path with the entry added at its end, the others kept" || fail "Path: '$(regval Path)'"

# --- Cancel writes nothing
wine "$EXE" /envvars >/dev/null 2>&1 & P=$!
W=$(win "Environment Variables") || fail "no dialog the second time"
xdotool windowfocus --sync "$W" 2>/dev/null; keys alt+n
F=$(win "New User Variable") && { xdotool windowfocus "$F" 2>/dev/null; typ SG_CANCELLED; keys Tab; typ x; keys Return; }
sleep 0.6; xdotool windowfocus "$W" 2>/dev/null; keys Escape
i=0; while kill -0 $P 2>/dev/null && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
wineserver -w
[ -z "$(regval SG_CANCELLED)" ] && pass "Cancel writes nothing" || fail "Cancel wrote SG_CANCELLED"

# --- found by search: Settings' page and Control Panel's item
grep -q 'PG_S_ENVVARS, L"environment variables path' "$HERE/src/control/settings.c" && grep -q 'L"Environment variables",' "$HERE/src/control/settings.h" \
    && pass "Settings: an Environment variables page, found by \"environment\" and \"path\"" || fail "Settings page or its words"
grep -q 'ENVVARS_A = { L"Environment Variables"' "$HERE/src/control/home.c" && grep -q '&ENVVARS_A' "$HERE/src/control/home.c" \
    && pass "Control Panel: an Environment Variables item in its search" || fail "Control Panel item"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
