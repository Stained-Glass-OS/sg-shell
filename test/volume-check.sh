#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for the taskbar's volume icon (sg-volume): what it says for each state
# of the sound server -- an output at 40%, muted, none at all, the server not
# answering -- read from its --dump. A stand-in sg-settingsctl answers as the
# real one does (SINK lines, then OK).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_VOLUME_EXE:-$HERE/build/sg-volume64.exe}"
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
T=$(mktemp -d /var/tmp/sg-volume-check.XXXXXX)
# shellcheck disable=SC2317
cleanup() { WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d'
unset DISPLAY
"$WINE_DIR/bin/wine" wineboot -i >/dev/null 2>&1
cp "$EXE" "$T/sg-volume64.exe"
cat > "$T/settingsctl" <<EOF
#!/bin/sh
# the last two arguments are --out FILE
for a; do out=\$a; done
mode=\$(cat "$T/mode")
{
    printf '%s\n' "\$*" >> "$T/calls"
    case \$mode in
        one)   printf 'SINK alsa_output.pci.analog-stereo\tyes\t40\tno\tSpeakers (Built-in Audio)\nSOURCE alsa_input.pci\tyes\t100\tno\tMicrophone\nOK\n' ;;
        two)   printf 'SINK hdmi\tno\t70\tno\tHDMI Output\nSINK speakers\tyes\t25\tyes\tSpeakers (Built-in Audio)\nOK\n' ;;
        none)  printf 'OK\n' ;;
        down)  printf 'ERROR failed pactl: Connection refused\n' ;;
    esac
} > "\$out.tmp" && mv "\$out.tmp" "\$out"
EOF
chmod 755 "$T/settingsctl"
dump() { echo "$1" > "$T/mode"; SG_SETTINGSCTL="$T/settingsctl" "$WINE_DIR/bin/wine" "$T/sg-volume64.exe" --dump 2>&1 | tr -d '\r' | grep -E '^(VOLUME|TIP)'; }

out=$(dump one); echo "$out" | sed 's/^/      /'
[ "$out" = "VOLUME 40 on 1-outputs
TIP Speakers (Built-in Audio): 40%" ] && pass "an output at 40%: the icon says so, as Windows words it" || fail "one: $out"
out=$(dump two); echo "$out" | sed 's/^/      /'
[ "$out" = "VOLUME 25 muted 2-outputs
TIP Speakers (Built-in Audio): muted" ] && pass "two outputs: the default one is shown, and muted" || fail "two: $out"
out=$(dump none); echo "$out" | sed 's/^/      /'
[ "$out" = "VOLUME none no-device
TIP No audio output device is installed" ] && pass "no output: Windows' words for it" || fail "none: $out"
out=$(dump down); echo "$out" | sed 's/^/      /'
[ "$out" = "VOLUME none failed
TIP The sound service is not responding" ] && pass "the sound server down: said, not a silent blank" || fail "down: $out"
grep -q '^sound --out ' "$T/calls" && pass "it asks sg-settingsctl, as Settings' Sound page does" || fail "calls: $(cat "$T/calls")"

# --- a second click on the icon closes the flyout, not flickers it (David 2026-09-29) ----------
# The click's mouse-down deactivates the flyout (it hides) before the icon's
# button-up arrives; without the reopen guard the icon reopened it.
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
if command -v Xvfb >/dev/null && command -v "$MINGW" >/dev/null; then
    "$MINGW" -O2 -municode -o "$T/fly-poke.exe" "$HERE/test/sg-fly-poke.c" 2>/dev/null
    # the --dump runs above left a wineserver whose desktop has no display; start clean
    "$WINE_DIR/bin/wineserver" -k 2>/dev/null; "$WINE_DIR/bin/wineserver" -w 2>/dev/null
    Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
    i=0; while [ ! -s "$T/display" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    echo one > "$T/mode"
    DISPLAY=":$(cat "$T/display")" SG_SETTINGSCTL="$T/settingsctl" "$WINE_DIR/bin/wine" "$T/sg-volume64.exe" >"$T/vol.log" 2>&1 & VP=$!
    out=$(WINEPREFIX="$T/pfx" DISPLAY=":$(cat "$T/display")" "$WINE_DIR/bin/wine" "$T/fly-poke.exe" SgVolumeTray SgVolumeFlyout 2>/dev/null | tr -d '\r')
    kill "$VP" "$XP" 2>/dev/null
    [ "$out" = "shown=1 after=0" ] && pass "a second click on the icon closes the flyout (no flicker reopen)" \
        || { fail "flyout second click: $out (want shown=1 after=0)"; sed 's/^/      vol: /' "$T/vol.log" | head -5; }
    # --- the chime: a volume change from the flyout (a key, the wheel, the
    # slider let go) plays it, after the change (David 2026-10-02; mutant
    # NO_CHIME)
    "$MINGW" -O2 -municode -o "$T/vol-key.exe" "$HERE/test/sg-vol-key.c" 2>/dev/null
    Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display2" >/dev/null 2>&1 & XP=$!
    i=0; while [ ! -s "$T/display2" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    : > "$T/calls"
    DISPLAY=":$(cat "$T/display2")" SG_SETTINGSCTL="$T/settingsctl" "$WINE_DIR/bin/wine" "$T/sg-volume64.exe" >"$T/vol.log" 2>&1 & VP=$!
    WINEPREFIX="$T/pfx" DISPLAY=":$(cat "$T/display2")" "$WINE_DIR/bin/wine" "$T/vol-key.exe" >/dev/null 2>&1
    kill "$VP" "$XP" 2>/dev/null
    v=$(grep -n '^sound volume sink' "$T/calls" | head -1 | cut -d: -f1)
    c=$(grep -n '^sound chime' "$T/calls" | head -1 | cut -d: -f1)
    [ -n "$v" ] && [ -n "$c" ] && [ "$c" -gt "$v" ] && pass "a volume change from the flyout chimes, after the change" \
        || fail "no chime after the change: $(tr '\n' '|' < "$T/calls")"
else
    echo "SKIP  flyout second-click and chime (no Xvfb or mingw)"
fi
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
