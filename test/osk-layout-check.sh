#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate: the On-Screen Keyboard's labels are the keyboard layout's, and a key
# types what its label shows. Under Xvfb the layout is the X server's keymap,
# switched with setxkbmap while Notepad (in front) and the keyboard run:
#
#   - US: y, z, [ , ; and ' where a US keyboard has them; no ISO key; the right
#     Alt is Alt;
#   - German: the labels follow by themselves (QWERTZ: y and z swapped; ß, ü,
#     ö, ä; the ISO key < between Shift and Y; the right Alt is AltGr, and with
#     it Q shows @ -- wine-sg 0250 gives the national keys their scan codes,
#     0251 makes Ctrl+Alt AltGr in ToUnicodeEx); clicking z-position, ü,
#     Shift+2, AltGr+Q and AltGr+< types "zü\"@|" into Notepad;
#   - French: AZERTY (a on the Q key, w on the Z key, m on the ; key), é on 2;
#     clicking them and AltGr+3 types "aé#";
#   - back to US: the labels and the Alt key are US again.
#
# SG_OSK_EXE runs another build (the mutant -DSG_MUTANT_USLABELS keeps the US
# labels); SG_WINE_DIR another Wine (a build tree works). Display :174.
# Screenshots: build/osk-layout-*.png.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_OSK_EXE:-$HERE/build/sg-osk64.exe}"
OUT="$HERE/build"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
RC=0; DPY="${SG_OSK_LAYOUT_DPY:-174}"; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool setxkbmap import "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
if [ -x "$WINE_DIR/bin/wine" ]; then WBIN="$WINE_DIR/bin"; WSERVER="$WINE_DIR/bin/wineserver"
elif [ -x "$WINE_DIR/wine" ]; then WBIN="$WINE_DIR"; WSERVER="$WINE_DIR/server/wineserver"
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi
[ -f "$EXE" ] || { echo "SKIP: $EXE missing (make build)"; exit 77; }
mkdir -p "$OUT"

T=$(mktemp -d /var/tmp/sg-osk-layout.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WSERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

"$MINGW" -O2 -municode -o "$T/probe.exe" "$HERE/test/sg-a11y-probe.c" -luser32 -lgdi32 || { fail "probe did not build"; exit 1; }

Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
mkdir -p "$T/home" "$T/pfx"
export DISPLAY=":$DPY" HOME="$T/home" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WBIN:$(dirname "$WSERVER"):$PATH"
i=0; while ! xdpyinfo >/dev/null 2>&1 && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
setxkbmap -layout us
DISPLAY= wine wineboot --init >/dev/null 2>&1; wineserver -w
cp "$T/probe.exe" "$WINEPREFIX/drive_c/probe.exe"
cp "$EXE" "$WINEPREFIX/drive_c/sg-osk64.exe"
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
wineserver -w
P() { wine 'C:\probe.exe' "$@" 2>/dev/null | tr -d '\r'; }
DUMP="$WINEPREFIX/drive_c/osk.txt"
export SG_OSK_DUMP='C:\osk.txt'
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
wine notepad >/dev/null 2>&1 &
i=0; while [ "$(P text Notepad)" = NOWINDOW ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
P activate Notepad; sleep 1
wine 'C:\sg-osk64.exe' >/dev/null 2>&1 &
val() { sed -n "s/^$1 //p" "$DUMP" 2>/dev/null | tr -d '\r' | head -1; }
i=0; while [ "$(val VISIBLE)" != 1 ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
[ "$(val VISIBLE)" = 1 ] || { fail "no keyboard window"; exit 1; }
sleep 1
P activate Notepad; sleep 0.5

# the label of key $1 ("" when the key is not shown)
lab() { sed -n "s/^KEY $1 [0-9-]* [0-9-]* [01] //p" "$DUMP" 2>/dev/null | tr -d '\r' | head -1; }
keyfield() { sed -n "s/^KEY $1 //p" "$DUMP" 2>/dev/null | tr -d '\r' | head -1 | cut -d' ' -f"$2"; }
click() {
    for k in "$@"; do
        x=$(keyfield "$k" 1); y=$(keyfield "$k" 2)
        [ -n "$x" ] || { fail "no key $k shown"; continue; }
        xdotool mousemove "$x" "$y" click 1; sleep 0.35
    done
}
wait_text() { i=0; while [ "$(P text Notepad)" != "$1" ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done; [ "$(P text Notepad)" = "$1" ]; }
wait_label() { i=0; while [ "$(lab "$1")" != "$2" ] && [ $i -lt 24 ]; do sleep 0.25; i=$((i + 1)); done; [ "$(lab "$1")" = "$2" ]; }
labels() { for k in "$@"; do printf '%s=%s ' "$k" "$(lab "$k")"; done; }

# --- US ---------------------------------------------------------------------------------------------
[ "$(lab y)" = y ] && [ "$(lab z)" = z ] && [ "$(lab lbracket)" = '[' ] && [ "$(lab semicolon)" = ';' ] && [ "$(lab quote)" = "'" ] \
    && pass "US: y, z, [, ;, ' where a US keyboard has them" || fail "US labels: $(labels y z lbracket semicolon quote)"
[ -z "$(lab oem102)" ] && [ "$(lab altgr)" = Alt ] && pass "US: no ISO key, the right Alt is Alt" || fail "US: oem102='$(lab oem102)' altgr='$(lab altgr)'"
import -window root "$OUT/osk-layout-us.png"

# --- German -------------------------------------------------------------------------------------------
setxkbmap -layout de
if wait_label y z; then pass "German: the labels follow the new layout by themselves (the Y key shows z)"
else fail "German: the Y key shows '$(lab y)'"; fi
[ "$(lab z)" = y ] && [ "$(lab minus)" = 'ß' ] && [ "$(lab lbracket)" = 'ü' ] && [ "$(lab semicolon)" = 'ö' ] && [ "$(lab quote)" = 'ä' ] \
    && pass "German: y, ß, ü, ö, ä on their keys" || fail "German labels: $(labels z minus lbracket semicolon quote)"
[ "$(lab oem102)" = '<' ] && [ "$(lab altgr)" = AltGr ] && pass "German: the ISO key shows <, the right Alt is AltGr" \
    || fail "German: oem102='$(lab oem102)' altgr='$(lab altgr)'"
click shift
[ "$(lab 2)" = '"' ] && [ "$(lab 3)" = '§' ] && pass "German with Shift: \" on 2, § on 3" || fail "German shifted: $(labels 2 3)"
click shift
click altgr
[ "$(lab q)" = '@' ] && [ "$(lab oem102)" = '|' ] && pass "German with AltGr: @ on Q, | on <" || fail "German AltGr: $(labels q oem102)"
import -window root "$OUT/osk-layout-de-altgr.png"
click altgr
click y lbracket shift 2 altgr q altgr oem102
if wait_text 'zü"@|'; then pass "German: clicked keys type what they show (zü\"@|)"
else fail "German: Notepad has \"$(P text Notepad)\""; fi
import -window root "$OUT/osk-layout-de.png"

# --- French -------------------------------------------------------------------------------------------
setxkbmap -layout fr
if wait_label q a; then pass "French: AZERTY (the Q key shows a)"
else fail "French: the Q key shows '$(lab q)'"; fi
[ "$(lab a)" = q ] && [ "$(lab z)" = w ] && [ "$(lab semicolon)" = m ] && [ "$(lab 2)" = 'é' ] \
    && pass "French: q, w, m, é where AZERTY has them" || fail "French labels: $(labels a z semicolon 2)"
click q 2 altgr 3
if wait_text 'zü"@|aé#'; then pass "French: clicked keys type aé# (AltGr+3 is #)"
else fail "French: Notepad has \"$(P text Notepad)\""; fi
import -window root "$OUT/osk-layout-fr.png"

# --- back to US --------------------------------------------------------------------------------------------
setxkbmap -layout us
if wait_label q q && [ "$(lab y)" = y ] && [ -z "$(lab oem102)" ] && [ "$(lab altgr)" = Alt ]; then
    pass "back to US: the labels, the ISO key and the Alt key are US again"
else fail "back to US: $(labels q y oem102 altgr)"; fi
[ "$(P foreground)" = Notepad ] && pass "Notepad kept the focus throughout" || fail "the focus went to $(P foreground)"

[ $RC = 0 ] && echo "osk-layout-check: PASS" || echo "osk-layout-check: FAIL"
exit $RC
