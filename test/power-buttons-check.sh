#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# What the power button and closing the lid do (David 2026-10-03: "power
# settings ... what should happen for things like a laptop lid close"):
# Settings > Power & sleep shows systemd-logind's settings from our drop-in
# (SG_LOGIND_DROPIN here), the lid's rows only on a laptop (SG_POWER_LID), and
# a change goes as an administrator to sg-admind's power-buttons (test mode),
# which writes the drop-in -- the other two settings kept. Control Panel's
# Hardware and Sound lists Power Options. Read back from SG_SETTINGS_DUMP.
#
# Needs wine-sg, Xvfb and xdotool; skips (77) without. SG_SETTINGS_EXE runs
# another build; SG_WINE_DIR another Wine.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_SETTINGS_EXE:-$HERE/build/sg-settings64.exe}"
CPL="${SG_CONTROL_EXE:-$HERE/build/sg-control64.exe}"
RC=0; XP=""; LOOP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for c in Xvfb xdotool python3; do command -v "$c" >/dev/null || { echo "SKIP: $c missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-power-buttons.XXXXXX)
# shellcheck disable=SC2317
cleanup() {
    [ -n "$LOOP" ] && kill "$LOOP" 2>/dev/null
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T" "$SG_GATE_HOME"
}
trap cleanup EXIT INT TERM
Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/display")" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1

C="$T/logind/60-stained-glass-power.conf"
mkdir -p "$T/logind"
printf '[Login]\nHandleLidSwitch=hibernate\nHandleLidSwitchExternalPower=ignore\nHandlePowerKey=suspend\n' > "$C"
SG_LOGIND_DROPIN=$(wine winepath -w "$C" 2>/dev/null | tr -d '\r'); export SG_LOGIND_DROPIN
show() {   # show EXE URI MARK: open it, wait for its dump to show MARK, print the dump
    rm -f "$T/dump.txt"
    SG_SETTINGS_DUMP=$(wine winepath -w "$T/dump.txt" 2>/dev/null | tr -d '\r') wine "$1" "$2" >/dev/null 2>&1 &
    i=0; while [ $i -lt 60 ] && ! grep -q "$3" "$T/dump.txt" 2>/dev/null; do sleep 1; i=$((i + 1)); done
    sleep 1; tr -d '\r' < "$T/dump.txt" 2>/dev/null
}

# sg-admind in test mode, on a spool of the gate's (the elevated half inherits these)
S="$T/spool"; mkdir -p "$S/requests" "$S/replies"; chmod 700 "$S/requests"
export SG_ADMIN_TEST=1 SG_ADMIN_SPOOL="$S" SG_ADMIN_SYSTEM_UID="$(id -u)" SG_ADMIN_LOGIND_DROPIN="$C"
export SG_POWER_LID=1
out=$(show "$EXE" ms-settings:powersleep "Power buttons and lid")
printf '%s\n' "$out" | grep -q '^text Power buttons and lid$' && pass "Power & sleep has Power buttons and lid" || { fail "no Power buttons and lid"; printf '%s\n' "$out" | head; }
printf '%s\n' "$out" | grep -q '^text When I close the lid, on battery$' && printf '%s\n' "$out" | grep -q '^text When I close the lid, plugged in$' \
    && pass "on a laptop: the lid, on battery and plugged in" || fail "no lid rows on a laptop"
# the combos after the screen and sleep ones: power button, lid on battery, lid plugged in
vals=$(printf '%s\n' "$out" | grep '^control ComboBox' | sed -n '3,5p' | sed 's/.*: //' | tr '\n' '|')
[ "$vals" = "Sleep|Hibernate|Do nothing|" ] && pass "they show logind's settings from the drop-in (Sleep, Hibernate, Do nothing)" || fail "shown: $vals"

# a change: the power button to Shut down, as an administrator, through sg-admind
( while :; do for f in "$S"/requests/*.req; do [ -e "$f" ] && python3 "$HERE/admin/sg-admind" 2>>"$T/admind.log"; break; done; sleep 0.2; done ) &
LOOP=$!
at=$(printf '%s\n' "$out" | grep '^control ComboBox' | sed -n '3p' | sed 's/.* at=\([0-9]*\),\([0-9]*\).*/\1 \2/')
# shellcheck disable=SC2086  # x y
xdotool mousemove $at click 1; sleep 1; xdotool key End Return
i=0; while [ $i -lt 40 ] && ! grep -q '^HandlePowerKey=poweroff$' "$C"; do sleep 0.5; i=$((i + 1)); done
grep -q '^HandlePowerKey=poweroff$' "$C" && grep -q '^HandleLidSwitch=hibernate$' "$C" && grep -q '^HandleLidSwitchExternalPower=ignore$' "$C" \
    && pass "choosing Shut down writes HandlePowerKey=poweroff (as an administrator); the lid's kept" || fail "after the change: $(tr '\n' ' ' < "$C") $(tail -2 "$T/admind.log" 2>/dev/null)"
kill "$LOOP" 2>/dev/null; LOOP=""; wineserver -k 2>/dev/null; sleep 1

# a desktop: no lid rows
export SG_POWER_LID=0
out=$(show "$EXE" ms-settings:powersleep "Power buttons and lid")
printf '%s\n' "$out" | grep -q 'When I close the lid' && fail "lid rows on a PC without a lid" || pass "no lid, no lid rows (the power button only)"
vals=$(printf '%s\n' "$out" | grep '^control ComboBox' | sed -n '3p' | sed 's/.*: //')
[ "$vals" = "Shut down" ] && pass "the power button shows what was chosen (Shut down)" || fail "power button shows '$vals'"
wineserver -k 2>/dev/null; sleep 1

# Control Panel: Power Options, under Hardware and Sound (where powercfg.cpl goes)
if [ -f "$CPL" ]; then
    items=$(wine "$CPL" --dump items 2>/dev/null | tr -d '\r')
    res=$(wine "$CPL" --resolve powercfg.cpl 2>/dev/null | tr -d '\r')
    printf '%s\n' "$items" | grep -qx 'item=Power Options' && [ "$res" = "page=Hardware and Sound" ] \
        && pass "Control Panel lists Power Options; powercfg.cpl opens Hardware and Sound, where it is" || fail "Control Panel: '$res' $(printf '%s\n' "$items" | grep -i power)"
fi
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
