#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Settings > Display: the screen's orientation and the rotation lock (David
# 2026-10-07: "a tablet can auto rotate, or disable auto rotate in the
# settings"), against a stand-in sg-settingsctl (sg-session's: display
# transform, rotation [lock]):
#   - a PC with an accelerometer, rotation lock off: the orientation list is
#     greyed (the screen turns with the PC) and says so; Rotation lock is off.
#     Mutant SG_MUTANT_ROTATION_ORIENT_WHILE_TURNING (the list stays usable).
#   - Rotation lock on: sg-settingsctl rotation lock yes; the list usable;
#     Portrait chosen: display transform eDP-1 90, and the page shows it.
#   - no accelerometer: no Rotation lock, the list usable.
#
# Needs wine-sg, Xvfb, xdotool and mingw; skips (77) without.
# SG_WINE_DIR another Wine (bin/wine, bin/wineserver).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for c in Xvfb xdotool "$MINGW"; do command -v "$c" >/dev/null || { echo "SKIP: $c missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$HERE/build/sg-settings64.exe" ] && [ -f "$HERE/build/sg-settings-res64.o" ] \
    || { echo "SKIP: wine-sg or build/sg-settings64.exe missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-rotation-check.XXXXXX)
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T" "$SG_GATE_HOME"
}
trap cleanup EXIT INT TERM

# shellcheck disable=SC2086
"$MINGW" -O2 -municode -mwindows -Wno-missing-field-initializers -DSG_MUTANT_ROTATION_ORIENT_WHILE_TURNING \
    -o "$T/mut-settings.exe" "$HERE"/src/control/*.c "$HERE/build/sg-settings-res64.o" \
    -lcomctl32 -lshell32 -lgdi32 -luser32 -ladvapi32 -lmsimg32 -liphlpapi -lws2_32 \
    -lole32 -luuid -lwindowscodecs -lcomdlg32 -lshlwapi -lwininet -lversion -lwinspool 2>"$T/mut-build.log" \
    || { fail "the mutant does not build: $(tail -1 "$T/mut-build.log")"; exit 1; }

Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
DISPLAY=":$(cat "$T/display")"
export DISPLAY WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDEBUG=-all \
    WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
winpath() { wine winepath -w "$1" 2>/dev/null | tr -d '\r'; }

# the native half: answers as sg-settingsctl does, keeps the transform and the lock
CTL="$T/sg-settingsctl"; LOG="$T/ctl.log"; : > "$LOG"
cat > "$CTL" <<EOS
#!/bin/sh
out=""; args=""
while [ \$# -gt 0 ]; do
    case "\$1" in --out) out="\$2"; shift 2 ;; *) args="\$args \$1"; shift ;; esac
done
echo "\${args# }" >> "$LOG"
set -- \$args
{
case "\$*" in
display) printf 'OUTPUT eDP-1\t1920x1080@60\t1.0\tBuilt-in Display\nMODE eDP-1\t1920x1080@60\tyes\tyes\nTRANSFORM eDP-1\t%s\nOK\n' "\$(cat "$T/transform" 2>/dev/null || echo normal)" ;;
"display transform eDP-1 "*) echo "\$4" > "$T/transform"; echo OK ;;
rotation) printf 'ROTATION %s\t%s\nOK\n' "\$(cat "$T/lock" 2>/dev/null || echo no)" "\$(cat "$T/sensor" 2>/dev/null || echo yes)" ;;
"rotation lock "*) echo "\$3" > "$T/lock"; echo OK ;;
nightlight) printf 'NIGHTLIGHT off\t4000\tyes\tno\nOK\n' ;;
*) echo 'ERROR invalid usage' ;;
esac
} > "\$out.part"
mv "\$out.part" "\$out"
EOS
chmod 755 "$CTL"
export SG_SETTINGSCTL="$CTL"

EXE="$HERE/build/sg-settings64.exe"
show() {   # the Display page's dump, once it shows $1
    rm -f "$T/dump.txt"
    wineserver -k 2>/dev/null; sleep 0.5
    SG_SETTINGS_DUMP=$(winpath "$T/dump.txt") wine "$EXE" ms-settings:display >/dev/null 2>&1 &
    i=0; while [ $i -lt 60 ] && ! grep -q "$1" "$T/dump.txt" 2>/dev/null; do sleep 1; i=$((i + 1)); done
    sleep 1; tr -d '\r' < "$T/dump.txt" 2>/dev/null
}
waitfor() { i=0; while [ $i -lt 40 ] && ! sh -c "$1" 2>/dev/null; do sleep 0.5; i=$((i + 1)); done; sh -c "$1" 2>/dev/null; }
at() { printf '%s\n' "$1" | grep "$2" | head -1 | sed 's/.* at=\([0-9]*\),\([0-9]*\).*/\1 \2/'; }
orient() { printf '%s\n' "$1" | grep '^control ComboBox .*: \(Landscape\|Portrait\)' | head -1; }

# 1. turning with the PC
out=$(show "Rotation lock")
o=$(orient "$out")
case "$o" in *" disabled: Landscape") pass "accelerometer, unlocked: Display orientation Landscape, greyed -- the screen turns with the PC" ;;
    *) fail "unlocked: the orientation list is '$o' (want greyed, Landscape)" ;; esac
printf '%s\n' "$out" | grep -q '^control SgSetCtl .*state=0.*: Rotation lock$\|^control Button .*state=0.*: Rotation lock$' \
    && printf '%s\n' "$out" | grep -q '^text The screen turns as you turn this PC' \
    && pass "...Rotation lock is off, and the page says the screen turns" || fail "unlocked: $(printf '%s\n' "$out" | grep -i 'rotation\|turns')"
EXE="$T/mut-settings.exe"
o=$(orient "$(show "Rotation lock")")
case "$o" in *disabled*) fail "MUTANT ROTATION_ORIENT_WHILE_TURNING not caught ($o)" ;;
    *) pass "MUTANT ROTATION_ORIENT_WHILE_TURNING (the list usable while it turns) is caught" ;; esac
EXE="$HERE/build/sg-settings64.exe"

# 2. the lock on, then Portrait
out=$(show "Rotation lock")
xy=$(at "$out" ': Rotation lock$')
# shellcheck disable=SC2086
[ -n "$xy" ] && xdotool mousemove $xy click 1
waitfor "grep -qx 'rotation lock yes' '$LOG'" >/dev/null && pass "Rotation lock on: sg-settingsctl rotation lock yes" || fail "lock: $(cat "$LOG")"
waitfor "grep -q 'text The screen stays as it is' '$T/dump.txt'" >/dev/null
out=$(tr -d '\r' < "$T/dump.txt")
o=$(orient "$out")
case "$o" in *disabled*|"") fail "locked: the orientation list is '$o' (want usable)" ;; *) pass "...the orientation list can be used" ;; esac
xy=$(at "$out" '^control ComboBox .*: Landscape$')
for _try in 1 2 3; do   # the drop-down can miss a click while the page settles
    # shellcheck disable=SC2086
    xdotool mousemove $xy click 1; sleep 1.5; xdotool key Home Down Return
    waitfor "grep -qx 'display transform eDP-1 90' '$LOG'" >/dev/null && break
    xdotool key Escape; sleep 1
done
grep -qx 'display transform eDP-1 90' "$LOG" && pass "Portrait: sg-settingsctl display transform eDP-1 90" || fail "portrait: $(cat "$LOG")"
waitfor "tr -d '\\r' < '$T/dump.txt' | grep -q '^control ComboBox .*: Portrait$'" >/dev/null \
    && pass "...and the page shows Portrait" || fail "after: $(orient "$(tr -d '\r' < "$T/dump.txt")")"

# 3. no accelerometer
echo no > "$T/sensor"; echo normal > "$T/transform"; echo no > "$T/lock"
out=$(show "Display orientation")
o=$(orient "$out")
case "$o" in *disabled*|"") fail "no accelerometer: the list is '$o'" ;;
    *) printf '%s\n' "$out" | grep -q 'Rotation lock' && fail "no accelerometer, yet a Rotation lock" \
           || pass "no accelerometer: no Rotation lock, the orientation list can be used" ;; esac

[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
