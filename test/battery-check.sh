#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for the taskbar's battery icon (sg-battery): what it says in each state
# -- no battery (no icon at all), on battery, charging, fully charged -- read
# from its --dump. Fake batteries are bind-mounted over /sys/class/power_supply
# in a private mount namespace, where wine-sg reads them (needs sudo -n).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_BATTERY_EXE:-$HERE/build/sg-battery64.exe}"
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
sudo -n true 2>/dev/null && command -v unshare >/dev/null || { echo "SKIP: needs sudo -n and unshare"; exit 77; }
T=$(mktemp -d /var/tmp/sg-battery-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() { WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d'
unset DISPLAY
"$WINE_DIR/bin/wine" wineboot -i >/dev/null 2>&1; "$WINE_DIR/bin/wineserver" -k 2>/dev/null
cp "$EXE" "$T/sg-battery64.exe"

mk() { # dir status ac percent
    mkdir -p "$1/BAT0" "$1/AC"
    printf 'Battery\n' > "$1/BAT0/type"; printf '%s\n' "$2" > "$1/BAT0/status"; printf '11400000\n' > "$1/BAT0/voltage_now"
    printf '4385965\n' > "$1/BAT0/charge_full"; printf '%s\n' $(( 4385965 * $4 / 100 + 1 )) > "$1/BAT0/charge_now"
    printf '877192\n' > "$1/BAT0/current_now"
    printf 'Mains\n' > "$1/AC/type"; printf '%s\n' "$3" > "$1/AC/online"
}
dump() { # dir (or none) -> the dump
    if [ "$1" = none ]; then mkdir -p "$T/none"; set -- "$T/none"; fi
    sudo -n unshare -m sh -c "mount --bind '$1' /sys/class/power_supply && exec sudo -n -u '$(id -un)' env HOME='$HOME' \
        WINEPREFIX='$WINEPREFIX' WINEDEBUG=-all '$WINE_DIR/bin/wine' '$T/sg-battery64.exe' --dump" 2>&1 | tr -d '\r' | grep -E '^(BATTERY|TIP)'
    "$WINE_DIR/bin/wineserver" -k 2>/dev/null; sleep 0.5
}
out=$(dump none)
[ "$out" = "BATTERY none" ] && pass "no battery: no icon" || fail "no battery: $out"
mk "$T/on" Discharging 0 63
out=$(dump "$T/on")
echo "$out" | grep -qE '^TIP 3 hr 0[0-9] min \(63%\) remaining$' && pass "on battery: \"3 hr 09 min (63%) remaining\"" || fail "on battery: $out"
mk "$T/chg" Charging 1 63
out=$(dump "$T/chg")
echo "$out" | grep -qx 'TIP 63% available (plugged in, charging)' && pass "charging: \"63% available (plugged in, charging)\"" || fail "charging: $out"
mk "$T/full" Full 1 100
out=$(dump "$T/full")
echo "$out" | grep -qx 'TIP Fully charged (100%)' && pass "full: \"Fully charged (100%)\"" || fail "full: $out"
echo "battery-check: $([ $RC = 0 ] && echo PASS || echo FAIL)"
exit $RC
