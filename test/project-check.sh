#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# The Project pane (src/control/project.c), Win+P as Windows has it: Windows opens it for
# ms-settings-displays-topology:projection (wine-sg 9068's Win+P runs that), with PC screen
# only, Duplicate, Extend and Second screen only, and the choice goes to the compositor
# through sg-settingsctl (display project ...), as Settings > Display's "Multiple displays".
#
#   - the protocol is registered (defaults/65-sg-settings.reg)
#   - a laptop panel (eDP-1) and a monitor (HDMI-A-1), extended: Extend is the current choice,
#     the panel is "PC screen", the monitor "second screen"
#   - each choice asks sg-settingsctl for it (a stand-in records the calls):
#     PC screen only -> display project only eDP-1, Duplicate -> duplicate, Second screen only
#     -> only HDMI-A-1; the current one (Extend) asks nothing
#   - the pane opens for the URI at the main display's right edge, and Escape closes it
#
# Needs wine-sg, Xvfb, xdotool, mingw-w64. Mutant: -DSG_MUTANT_PROJECT_NO_APPLY.
#   SG_SETTINGS_EXE runs another build; SG_WINE_DIR another Wine.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_SETTINGS_EXE:-$HERE/build/sg-settings64.exe}"
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for need in Xvfb xdotool; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-project-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    sg_kill_prefix "$T/pfx" 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

# the protocol, as the defaults register it
grep -q '^\[HKEY_LOCAL_MACHINE\\Software\\Classes\\ms-settings-displays-topology\\shell\\open\\command\]' "$HERE/defaults/65-sg-settings.reg" \
    && pass "ms-settings-displays-topology: is registered to Settings (Win+P's URI)" \
    || fail "ms-settings-displays-topology: is not registered in defaults/65-sg-settings.reg"

N=$(( 700 + $$ % 200 ))
while [ -e "/tmp/.X$N-lock" ] || [ -e "/tmp/.X11-unix/X$N" ]; do N=$((N + 1)); done
Xvfb ":$N" -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
i=0; while [ ! -e "/tmp/.X11-unix/X$N" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$N" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDEBUG=-all \
    WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w

# the native half: two displays extended, the calls recorded
CTL="$T/sg-settingsctl"; LOG="$T/ctl.log"; : > "$LOG"
cat > "$CTL" <<EOS
#!/bin/sh
out=""; args=""
while [ \$# -gt 0 ]; do
    case "\$1" in --out) out="\$2"; shift 2 ;; *) args="\$args \$1"; shift ;; esac
done
echo "\${args# }" >> "$LOG"
case "\${args# }" in
display) printf 'PLACE eDP-1\t0\t0\t1920\t1200\tyes\tno\nPLACE HDMI-A-1\t1920\t0\t2560\t1440\tyes\tyes\nPROJECT extend\nOK\n' ;;
"display project "*) echo OK ;;
*) echo 'ERROR invalid usage' ;;
esac > "\$out.part"
mv "\$out.part" "\$out"
EOS
chmod 755 "$CTL"
export SG_SETTINGSCTL="$CTL"

out=$(wine "$EXE" /project-dump 2>/dev/null | tr -d '\r')
echo "$out" | sed 's/^/      /'
echo "$out" | grep -q '^displays=2 pc=eDP-1 second=HDMI-A-1 current=2$' \
    && pass "a laptop panel and a monitor, extended: Extend is the current choice; the panel is the PC screen" \
    || fail "the pane's view: $(echo "$out" | head -1)"
for c in "0 display project only eDP-1" "1 display project duplicate" "3 display project only HDMI-A-1"; do
    n=${c%% *}; want=${c#* }
    : > "$LOG"
    wine "$EXE" /project-choose "$n" >/dev/null 2>&1
    grep -qx "$want" "$LOG" && pass "choice $n asks sg-settingsctl: $want" || fail "choice $n: $(tr '\n' '|' < "$LOG") (want $want)"
done
: > "$LOG"
wine "$EXE" /project-choose 2 >/dev/null 2>&1
grep -q 'project' "$LOG" && fail "the current choice (Extend) asked again: $(tr '\n' '|' < "$LOG")" \
    || pass "the current choice (Extend) asks nothing"

# the pane, opened for the URI: at the main display's right edge; Escape closes it
wine "$EXE" ms-settings-displays-topology:projection >/dev/null 2>&1 &
i=0; while [ $i -lt 40 ] && [ -z "$(xdotool search --name '^Project$' 2>/dev/null | head -1)" ]; do sleep 0.5; i=$((i + 1)); done
w=$(xdotool search --name '^Project$' 2>/dev/null | head -1)
if [ -n "$w" ]; then
    eval "$(xdotool getwindowgeometry --shell "$w" 2>/dev/null)"
    [ "$((X + WIDTH))" = 1280 ] && pass "the pane opens at the main display's right edge (x=$X w=$WIDTH)" \
        || fail "the pane's place: x=$X w=$WIDTH (screen 1280 wide)"
    [ -n "${SHOT:-}" ] && import -window root "$SHOT" 2>/dev/null
    xdotool windowfocus --sync "$w" 2>/dev/null; xdotool key Escape; sleep 1.5
    [ -z "$(xdotool search --name '^Project$' 2>/dev/null | head -1)" ] && pass "Escape closes it" || fail "Escape left the pane open"
else
    fail "no Project pane for ms-settings-displays-topology:projection"
fi
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
