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
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
