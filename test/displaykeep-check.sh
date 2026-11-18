#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Settings > Display: "Keep these display settings?" after a new refresh
# rate, as Windows asks it (David 2026-11-18: while it counted, the page's
# controls jumped between two places and could not be used), against a
# stand-in sg-settingsctl:
#   - the rate is set (display mode), and a dialog of its own, "Display
#     Settings", asks; the page is not rebuilt while it counts.
#     Mutant SG_MUTANT_KEEP_REBUILDS_PAGE (the page rebuilt every second).
#   - Esc reverts (display mode back to the old rate) and closes it.
#   - Enter keeps (Keep changes): it closes and nothing is reverted.
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

T=$(mktemp -d /var/tmp/sg-displaykeep-check.XXXXXX)
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T" "$SG_GATE_HOME"
}
trap cleanup EXIT INT TERM

# shellcheck disable=SC2086
"$MINGW" -O2 -municode -mwindows -Wno-missing-field-initializers -DSG_MUTANT_KEEP_REBUILDS_PAGE \
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

# the native half: answers as sg-settingsctl does, keeps the mode set
CTL="$T/sg-settingsctl"; LOG="$T/ctl.log"; : > "$LOG"; echo 1920x1080@60 > "$T/mode"
cat > "$CTL" <<EOS
#!/bin/sh
out=""; args=""
while [ \$# -gt 0 ]; do
    case "\$1" in --out) out="\$2"; shift 2 ;; *) args="\$args \$1"; shift ;; esac
done
echo "\${args# }" >> "$LOG"
set -- \$args
cur=\$(cat "$T/mode")
{
case "\$*" in
display) printf 'OUTPUT eDP-1\t%s\t1.0\tBuilt-in Display\n' "\$cur"
         for m in 1920x1080@60 1920x1080@144; do
             c=no; [ "\$m" = "\$cur" ] && c=yes; p=no; [ "\$m" = 1920x1080@60 ] && p=yes
             printf 'MODE eDP-1\t%s\t%s\t%s\n' "\$m" "\$c" "\$p"
         done
         printf 'TRANSFORM eDP-1\tnormal\nOK\n' ;;
"display mode eDP-1 "*) echo "\$4" > "$T/mode"; echo OK ;;
rotation) printf 'ROTATION no\tno\nOK\n' ;;
nightlight) printf 'NIGHTLIGHT off\t4000\tyes\tno\nOK\n' ;;
*) echo 'ERROR invalid usage' ;;
esac
} > "\$out.part"
mv "\$out.part" "\$out"
EOS
chmod 755 "$CTL"
export SG_SETTINGSCTL="$CTL"

show() {   # the Display page's dump, once it shows $1
    rm -f "$T/dump.txt"
    wineserver -k 2>/dev/null; sleep 0.5
    echo 1920x1080@60 > "$T/mode"; : > "$LOG"
    SG_SETTINGS_DUMP=$(winpath "$T/dump.txt") wine "$EXE" ms-settings:display >/dev/null 2>&1 &
    i=0; while [ $i -lt 60 ] && ! grep -q "$1" "$T/dump.txt" 2>/dev/null; do sleep 1; i=$((i + 1)); done
    sleep 1; tr -d '\r' < "$T/dump.txt" 2>/dev/null
}
waitfor() { i=0; while [ $i -lt 40 ] && ! sh -c "$1" 2>/dev/null; do sleep 0.5; i=$((i + 1)); done; sh -c "$1" 2>/dev/null; }
at() { printf '%s\n' "$1" | grep "$2" | head -1 | sed 's/.* at=\([0-9]*\),\([0-9]*\).*/\1 \2/'; }
soon() { i=0; while [ $i -lt 6 ] && ! sh -c "$1" 2>/dev/null; do sleep 0.5; i=$((i + 1)); done; sh -c "$1" 2>/dev/null; }   # within 3 s: before the countdown could
focusdlg() {   # a click on the dialog's text, as a person brings it forward
    eval "$(xdotool getwindowgeometry --shell "$1" 2>/dev/null)"
    xdotool mousemove $((X + 60)) $((Y + 30)) click 1; sleep 0.5
}
dialog() { xdotool search --name '^Display Settings$' 2>/dev/null | head -1; }
choose144() {   # Refresh rate: 144 Hz, from the list
    out=$(show "Refresh rate")
    xy=$(at "$out" '^control ComboBox .*: 60 Hz$')
    [ -n "$xy" ] || { fail "no Refresh rate list at 60 Hz: $(printf '%s\n' "$out" | grep ComboBox)"; return 1; }
    for _try in 1 2 3; do
        # shellcheck disable=SC2086
        xdotool mousemove $xy click 1; sleep 1.5; xdotool key End   # the rate applies as it is selected; a Return would answer the dialog
        waitfor "grep -qx 'display mode eDP-1 1920x1080@144' '$LOG'" >/dev/null && break
        xdotool key Escape; sleep 1
    done
    grep -qx 'display mode eDP-1 1920x1080@144' "$LOG"
}
steady() {   # the page's dump is not rewritten over 3 s (a rebuild writes it)
    waitfor "test -n \"\$(xdotool search --name '^Display Settings\$' 2>/dev/null)\"" >/dev/null
    sleep 1.5; a=$(stat -c %y "$T/dump.txt"); sleep 3; b=$(stat -c %y "$T/dump.txt"); [ "$a" = "$b" ]
}

EXE="$HERE/build/sg-settings64.exe"
# 1. a new rate: its own dialog, the page left as it is
if choose144; then
    pass "144 Hz chosen: sg-settingsctl display mode eDP-1 1920x1080@144"
    [ -n "$(waitfor "test -n \"\$(xdotool search --name '^Display Settings\$' 2>/dev/null)\"" >/dev/null; dialog)" ] \
        && pass "...\"Keep these display settings?\" asks in a dialog of its own (Display Settings)" || fail "no Display Settings dialog"
    steady && pass "...and the page is not rebuilt while it counts" || fail "the page was rebuilt while the dialog counted"
    # 2. Esc reverts
    d=$(dialog); [ -n "$d" ] && focusdlg "$d"; xdotool key Escape
    soon "grep -qx 'display mode eDP-1 1920x1080@60' '$LOG'" >/dev/null && pass "Esc reverts: display mode eDP-1 1920x1080@60" \
        || fail "Esc: sg-settingsctl heard $(tr '\n' ' ' < "$LOG")"
    waitfor "test -z \"\$(xdotool search --name '^Display Settings\$' 2>/dev/null)\"" >/dev/null && pass "...and the dialog is gone" || fail "the dialog stayed after Esc"
else fail "144 Hz could not be chosen: $(tr '\n' ' ' < "$LOG")"; fi

# 3. Enter keeps
if choose144; then
    waitfor "test -n \"\$(xdotool search --name '^Display Settings\$' 2>/dev/null)\"" >/dev/null
    d=$(dialog); [ -n "$d" ] && focusdlg "$d"; xdotool key Return
    soon "test -z \"\$(xdotool search --name '^Display Settings\$' 2>/dev/null)\"" >/dev/null && sleep 2 \
        && ! grep -qx 'display mode eDP-1 1920x1080@60' "$LOG" && pass "Enter keeps the new rate: the dialog closes, nothing reverted" \
        || fail "Enter: dialog $(dialog), sg-settingsctl heard $(tr '\n' ' ' < "$LOG")"
else fail "144 Hz could not be chosen the second time"; fi

# 4. the mutant: the page rebuilt every second
EXE="$T/mut-settings.exe"
if choose144; then
    steady && fail "MUTANT KEEP_REBUILDS_PAGE not caught" || pass "MUTANT KEEP_REBUILDS_PAGE (the page rebuilt every second) is caught"
else fail "mutant: 144 Hz could not be chosen"; fi

[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
