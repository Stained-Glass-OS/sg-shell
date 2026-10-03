#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Settings > Updates after "Check for updates" that finds nothing: the
# updater says "downloading" when the page is drawn and is done before the
# page's next look (a check with nothing to fetch). The page must come back
# to "You're up to date", not stay on "Preparing the download" (David
# 2026-10-03: until he went away and back). A stand-in sg-settingsctl
# answers "downloading" once after the check, then idle.
#
# Needs wine-sg, Xvfb, xdotool; skips (77) without. SG_SETTINGS_EXE runs
# another build (the mutation test: SG_MUTANT_UPD_TICK_MEMORY).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_SETTINGS_EXE:-$HERE/build/sg-settings64.exe}"
RC=0; DPY="${SG_UPD_DPY:-126}"; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for need in Xvfb xdotool; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
T=$(mktemp -d /var/tmp/sg-updates-page.XXXXXX)
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM
Xvfb ":$DPY" -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=,winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1
mkdir -p "$T/st"
cat > "$T/ctl" <<EOS
#!/bin/sh
out=""; args=""
while [ \$# -gt 0 ]; do case "\$1" in --out) out="\$2"; shift 2 ;; *) args="\$args \$1"; shift ;; esac; done
echo "\${args# }" >> "$T/ctl.log"
set -- \$args
st=\$(cat "$T/st/state" 2>/dev/null)
{
case "\$1 \${2:-}" in
"updates check") echo downloading > "$T/st/state"; touch "$T/st/once"; echo OK ;;
"updates "*) [ "\$st" = downloading ] && echo "DOWNLOADING yes" || echo "DOWNLOADING no"; echo "STAGED no"; echo OK
    [ "\${2:-}" != progress ] && [ -e "$T/st/once" ] && { rm -f "$T/st/once"; echo idle > "$T/st/state"; } ;;
*) echo 'ERROR unsupported stand-in' ;;
esac
} > "\$out.part"; mv "\$out.part" "\$out"
EOS
chmod +x "$T/ctl"
export SG_SETTINGSCTL="$T/ctl"
SG_SETTINGS_DUMP=$(wine winepath -w "$T/dump.txt" | tr -d '\r') wine "$EXE" ms-settings:windowsupdate >/dev/null 2>&1 &
i=0; while [ $i -lt 60 ] && ! grep -q ": Check for updates" "$T/dump.txt" 2>/dev/null; do sleep 0.5; i=$((i + 1)); done
at=$(tr -d '\r' < "$T/dump.txt" | sed -n 's/.* at=\([0-9]*\),\([0-9]*\): Check for updates$/\1 \2/p' | head -1)
[ -n "$at" ] || { fail "no Check for updates button"; exit 1; }
# shellcheck disable=SC2086  # x y
xdotool mousemove $at click 1; sleep 2
xdotool key Return      # the "Checking for updates" message
i=0; while [ $i -lt 20 ] && ! grep -q "Downloading updates" "$T/dump.txt" 2>/dev/null; do sleep 0.5; i=$((i + 1)); done
grep -q "Downloading updates" "$T/dump.txt" && pass "the page showed the check under way" || fail "never showed the download"
sleep 8
tr -d '\r' < "$T/dump.txt" | grep -q "You're up to date" && pass "...and, when it found nothing, comes back to \"You're up to date\" by itself" \
    || fail "still shows: $(tr -d '\r' < "$T/dump.txt" | grep -m1 -E "Downloading|Preparing|up to date")"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
