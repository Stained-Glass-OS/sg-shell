#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for Character Map (sg-charmap), driven like a person on the X keyboard
# and mouse, on a shell desktop under Xvfb:
#
#   - charmap.exe through App Paths opens "Character Map" on Arial with the
#     font's characters, "U+0041: Latin Capital Letter A" in the status bar
#   - choosing a font with the keyboard (Alt+F, a letter) loads that font's
#     characters and remembers it (HKCU\Software\Microsoft\CharMap\Font)
#   - in the grid, typing a character selects it, arrows move, Enter and
#     Alt+S (Select) add to "Characters to copy", Alt+C (Copy) puts exactly
#     that text on the clipboard (read back by a probe)
#   - the magnified view shows while the mouse holds a cell
#   - Advanced view: searching "U+00E9" selects é (and the status bar names
#     it, with its Alt+0233 keystroke); searching "omega" by name narrows the
#     grid, Reset brings everything back; "Go to Unicode" 20AC selects the
#     Euro sign
#
# Screenshots: build/charmap-*.png. Needs wine-sg, Xvfb, xdotool, ImageMagick
# and mingw; skips (77) without them. SG_CHARMAP_EXE runs another build (the
# mutation test).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_CHARMAP_EXE:-$HERE/build/sg-charmap64.exe}"
OUT="$HERE/build"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
RC=0; DPY=117; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-charmap-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

"$MINGW" -O2 -municode -o "$T/probe.exe" "$HERE/test/sg-charmap-probe.c" -luser32 || { fail "probe did not build"; exit 1; }

Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
cp "$T/probe.exe" "$WINEPREFIX/drive_c/probe.exe"
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
for r in "$HERE"/theme/*.reg; do wine reg import "$(wine winepath -w "$r" | tr -d '\r')" >/dev/null 2>&1; done
# App Paths as defaults/77-sg-charmap.reg has it, pointing at this build.
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\charmap.exe' /ve /d "$(wine winepath -w "$EXE" | tr -d '\r')"
wineserver -w
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

D="$T/dump.txt"
export SG_CHARMAP_DUMP="$D"
v() { sed -n "s/^$1=//p" "$D" 2>/dev/null | head -1; }
K() { xdotool key "$@"; sleep 0.8; }
clip() { wine 'C:\probe.exe' clip 2>/dev/null | tr -d '\r'; }
shot() { import -window root "$OUT/charmap-$1.png" 2>/dev/null; }
mkdir -p "$OUT"

wine 'C:\probe.exe' set before
wine start charmap.exe >/dev/null 2>&1
i=0; while [ "$(v font)" = "" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
sleep 2
shot open
[ "$(v font)" = Arial ] && pass "charmap.exe (App Paths) opens Character Map on Arial" || fail "font at start: '$(v font)'"
n=$(v count | cut -d/ -f1)
[ "${n:-0}" -gt 200 ] && pass "the grid holds the font's characters ($n)" || fail "count: $(v count)"
[ "$(v status0)" = "U+0041: Latin Capital Letter A" ] && pass "status bar: $(v status0)" || fail "status bar: '$(v status0)'"

# a font by the keyboard: Alt+F to the list, a letter picks the first font with it
arial_count=$(v count)
K alt+f
K t
font=$(v font)
case "$font" in T*|t*) pass "Alt+F, t chooses a font by the keyboard ($font, $(v count) characters)" ;; *) fail "font after Alt+F t: '$font'" ;; esac
[ "$(wine reg query 'HKCU\Software\Microsoft\CharMap' /v Font 2>/dev/null | tr -d '\r' | sed -n 's/.*REG_SZ *//p')" = "$font" ] \
    && pass "and remembers it in HKCU\\Software\\Microsoft\\CharMap" || fail "Font not remembered"
shot font
# back to Arial (the same way) for known characters
i=0; while [ "$(v font)" != Arial ] && [ $i -lt 12 ]; do xdotool key a; sleep 0.6; i=$((i + 1)); done
[ "$(v font)" = Arial ] && [ "$(v count)" = "$arial_count" ] || fail "could not return to Arial: $(v font)"

# into the grid: click the first cell, type a character, move with arrows
g=$(v grid)
gx=$(echo "$g" | cut -d, -f1); gy=$(echo "$g" | cut -d, -f2)
xdotool mousemove $((gx + 12)) $((gy + 12)) click 1; sleep 0.8
[ "$(v focus)" = grid ] && [ "$(v sel)" = U+0020 ] && pass "clicking the first cell selects the space" || fail "after click: focus=$(v focus) sel=$(v sel)"
K a
[ "$(v sel)" = U+0061 ] && pass "typing a in the grid selects it" || fail "typed a: sel=$(v sel)"
K Right Right
[ "$(v sel)" = U+0063 ] && [ "$(v zoom)" = 1 ] && pass "Right twice moves to c, magnified" || fail "arrows: sel=$(v sel) zoom=$(v zoom)"
K Return
K Right
K alt+s
[ "$(v text)" = cd ] && pass "Enter and Alt+S (Select) add c and d to Characters to copy" || fail "characters to copy: '$(v text)' ($(v textcp))"
[ "$(v copy_enabled)" = 1 ] && pass "Copy is enabled once there is text" || fail "Copy not enabled"
K alt+c
[ "$(clip)" = "U+0063 U+0064" ] && pass "Alt+C (Copy) puts exactly 'cd' on the clipboard" || fail "clipboard after Copy: $(clip)"

# the magnified view while the mouse holds a cell
sc=$(v selcell)
xdotool mousemove "$(echo "$sc" | cut -d, -f1)" "$(echo "$sc" | cut -d, -f2)" mousedown 1; sleep 0.8
[ "$(v zoom)" = 1 ] && pass "holding the mouse on a cell magnifies it" || fail "no magnified view: zoom=$(v zoom)"
shot zoom
xdotool mouseup 1; sleep 0.6
[ "$(v zoom)" = 0 ] && pass "and letting go hides it" || fail "magnified view stayed"

# Advanced view: search by code and by name, Go to Unicode
K alt+v
[ "$(v advanced)" = 1 ] && pass "Alt+V opens the advanced view" || fail "advanced=$(v advanced)"
K alt+h
# '+' is Shift+=: held down on the X keyboard (xdotool type loses Shift under Xvfb)
xdotool type u; xdotool keydown shift key equal keyup shift; xdotool type --delay 80 00e9; sleep 0.3
K Return
[ "$(v sel)" = U+00E9 ] && pass "searching u+00e9 selects é" || fail "search u+00e9: sel=$(v sel) (search box '$(v search)')"
[ "$(v status0)" = "U+00E9: Latin Small Letter E With Acute" ] && [ "$(v status1)" = "Keystroke: Alt+0233" ] \
    && pass "status bar: $(v status0) | $(v status1)" || fail "status for é: '$(v status0)' | '$(v status1)'"
shot search-code
K alt+h
xdotool type --delay 80 'omega'; sleep 0.3
K Return
n=$(v count | cut -d/ -f1)
if [ "$(v filtered)" = 1 ] && [ "${n:-0}" -ge 1 ] && [ "${n:-0}" -lt 100 ] && v name | grep -qi omega; then
    pass "searching omega by name narrows the grid to $n characters ($(v name))"
else fail "name search: filtered=$(v filtered) count=$(v count) name=$(v name)"; fi
shot search-name
K alt+r
[ "$(v filtered)" = 0 ] && [ "$(v count)" = "$arial_count" ] && pass "Reset shows every character again" || fail "reset: filtered=$(v filtered) count=$(v count)"
K alt+g
xdotool type --delay 80 '20ac'; sleep 0.8
[ "$(v sel)" = U+20AC ] && [ "$(v name)" = "Euro Sign" ] && pass "Go to Unicode 20ac selects the Euro Sign" || fail "go to 20ac: sel=$(v sel) name=$(v name)"
shot advanced

# the screenshot is the window, not a blank: the grid's area is mostly white
# with ink in it (glyphs, the grid lines and the selected cell)
mean=$(convert "$OUT/charmap-advanced.png" -crop "$(( $(echo "$g" | cut -d, -f3) - gx ))x$(( $(echo "$g" | cut -d, -f4) - gy ))+$gx+$gy" +repage \
       -colorspace Gray -format '%[fx:int(mean*1000)]' info: 2>/dev/null)
[ "${mean:-1000}" -gt 600 ] && [ "${mean:-1000}" -lt 950 ] && pass "the grid paints glyphs (mean grey $mean/1000)" || fail "grid looks blank or dark (mean grey $mean/1000 in $g)"

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
