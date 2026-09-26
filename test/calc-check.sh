#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for Calculator (sg-calc): calc.exe through App Paths on a shell desktop,
# driven on the X keyboard and mouse (xdotool), read back from the program's
# own dump (SG_CALC_DUMP, written after every paint) and from screenshots.
#
#   - Standard: 12+30= is 42 typed and clicked, = repeats, immediate execution
#     (2+3*4 = 20), percent, divide by zero, memory, Ctrl+C / Ctrl+V
#   - Scientific (Alt+2): precedence (2+3*4 = 14), parentheses, square root,
#     x^2, sin 30 in degrees, 2nd
#   - Programmer (Alt+3): HEX input, FF AND 0F = F, the readouts, NOT in BYTE
#   - the calculator: URI opening it in a mode
#
# Screenshots: build/calc-*.png. Needs wine-sg, Xvfb, xdotool, ImageMagick;
# skips (77) without them. SG_CALC_EXE runs another build (the mutation test).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_CALC_EXE:-$HERE/build/sg-calc64.exe}"
OUT="$HERE/build"
RC=0; DPY=110; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-calc-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
cp "$EXE" "$T/sg-calc64.exe"
winexe=$(wine winepath -w "$T/sg-calc64.exe" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
# The shipped colours and fonts, then our App Paths/URI file pointed at this build.
for f in "$HERE/theme/50-sg-colors.reg" "$HERE/theme/52-sg-fonts.reg"; do
    wine reg import "$(wine winepath -w "$f" | tr -d '\r')" >/dev/null 2>&1
done
esc=$(printf '%s' "$winexe" | sed 's/\\/\\\\\\\\/g')
sed "s|Z:\\\\\\\\usr\\\\\\\\libexec\\\\\\\\stained-glass\\\\\\\\shell\\\\\\\\sg-calc64.exe|$esc|g" \
    "$HERE/defaults/70-sg-calc.reg" > "$T/calc.reg"
wine reg import "$(wine winepath -w "$T/calc.reg" | tr -d '\r')" >/dev/null 2>&1
wineserver -w
if wine reg query 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\calc.exe' 2>/dev/null | tr -d '\r' | grep -qF "$winexe"
then pass "App Paths calc.exe names this build"; else fail "App Paths calc.exe was not registered"; fi

WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

DUMP="$T/calc.dump"
export SG_CALC_DUMP="$(wine winepath -w "$DUMP" | tr -d '\r')"
wine start calc.exe >/dev/null 2>&1
i=0; while [ ! -s "$DUMP" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
[ -s "$DUMP" ] && pass "calc.exe starts (through App Paths)" || { fail "calc.exe did not start"; exit 1; }
sleep 1

D() { sed -n "s/^$1 //p" "$DUMP" | head -1; }
# wait until the dump's $1 line is $2 (up to ~5 s)
want() {
    i=0
    while [ "$(D "$1")" != "$2" ] && [ $i -lt 25 ]; do sleep 0.2; i=$((i + 1)); done
    if [ "$(D "$1")" = "$2" ]; then pass "$3: $1 = $2"; else fail "$3: $1 is '$(D "$1")', want '$2'"; fi
}
btn() { sed -n "s/^BTN $1 //p" "$DUMP" | head -1; }
click() {
    b=$(btn "$1")
    [ -n "$b" ] || { fail "no '$1' key on screen"; return; }
    set -- $b
    xdotool mousemove $(( $1 + $3 / 2 )) $(( $2 + $4 / 2 )) click 1
    sleep 0.25
}
key() { xdotool key --delay 60 "$@"; sleep 0.3; }
typ() { xdotool type --delay 80 "$1"; sleep 0.3; }
shot() { import -window root "$OUT/calc-$1.png" 2>/dev/null; }

# focus: click the result area
set -- $(D CLIENT)
xdotool mousemove $(( $1 + $3 / 2 )) $(( $2 + 80 )) click 1; sleep 0.5
want MODE Standard "starts in Standard"

# --- Standard -----------------------------------------------------------------------------
typ "12+30"; key Return
want DISPLAY 42 "12+30= typed"
want EXPR "12 + 30 =" "the expression line"
shot standard
key Return
want DISPLAY 72 "= again repeats +30"
key Escape
want DISPLAY 0 "Esc clears"
for b in 1 2 add 3 0 eq; do click $b; done
want DISPLAY 42 "12+30= clicked"
click clear
typ "2+3*4="
want DISPLAY 20 "Standard executes immediately (2+3*4)"
key Escape; typ "50+10%"
want DISPLAY 5 "percent of the left operand"
key Return
want DISPLAY 55 "50+10% ="
key Escape; typ "1/0="
want ERROR "Cannot divide by zero" "divide by zero"
shot error
key Escape; typ "1234567"
want SHOWN "1,234,567" "digit grouping"
key BackSpace
want DISPLAY 123456 "Backspace"
key ctrl+m
want MEMORY "1 123456" "MS (Ctrl+M)"
key Escape; typ "7"; click madd
want MEMORY "1 123463" "M+"
key Escape; click mr
want DISPLAY 123463 "MR"
typ "9"; key F9
want DISPLAY -9 "F9 negates the entry"
key Escape; typ "3*7="; key ctrl+c; sleep 0.5
c=$(xclip -o -selection clipboard 2>/dev/null)
[ "$c" = 21 ] && pass "Ctrl+C copies 21" || fail "Ctrl+C copied '$c'"
printf '98765.5' | xclip -i -selection clipboard 2>/dev/null; sleep 0.5
key ctrl+v
want DISPLAY 98765.5 "Ctrl+V pastes"
want HISTORY 6 "history keeps each calculation (not the error)"

# --- Scientific ---------------------------------------------------------------------------
key alt+2
want MODE Scientific "Alt+2"
typ "2+3*4="
want DISPLAY 14 "Scientific honours precedence (2+3*4)"
key Escape; typ "(2+3)*4="
want DISPLAY 20 "parentheses"
key Escape; typ "81"; click sqrt
want DISPLAY 9 "square root of 81"
want EXPR "" "a unary result waits for an operator"
typ "q"
want DISPLAY 81 "x^2 (q)"
key Escape; typ "30"; click sin
want DISPLAY 0.5 "sin 30 degrees"
click 2nd
[ -n "$(btn asin)" ] && pass "2nd shows sin^-1" || fail "2nd did not change the keys"
key Escape; typ "2^10="
want DISPLAY 1024 "x^y"
key Escape; typ "5!"
want DISPLAY 120 "5!"
shot scientific

# --- Programmer ---------------------------------------------------------------------------
key alt+3
want MODE Programmer "Alt+3"
click hex
want RADIX "16 BITS 64" "HEX selected"
typ "ff"
want DEC 255 "ff in HEX is 255 in DEC"
want BIN 11111111 "and 11111111 in BIN"
typ "&0f"; key Return
want DISPLAY F "FF AND 0F"
key Escape
for b in f f and 0 f eq; do click $b; done
want DISPLAY F "FF AND 0F clicked"
[ "$(btn 9 | awk '{print $5}')" = 1 ] && pass "9 is enabled in HEX" || fail "9 disabled in HEX"
click bin
[ "$(btn 2 | awk '{print $5}')" = 0 ] && pass "2 is disabled in BIN" || fail "2 enabled in BIN"
click hex; key Escape
click word; click word; click word
want RADIX "16 BITS 8" "QWORD -> DWORD -> WORD -> BYTE"
typ "f0"; click not
want DISPLAY F "NOT F0 in a BYTE"
want DEC 15 "its DEC readout"
typ "1<4="
want DISPLAY 10 "1 Lsh 4"
shot programmer
click word
key alt+1
want MODE Standard "Alt+1"
sleep 0.5

# the screenshot is really drawn: the = key's accent purple is on screen
set -- $(btn eq)
if [ $# -ge 4 ]; then
    px=$(import -window root -crop 1x1+$(( $1 + 4 ))+$(( $2 + 4 )) txt:- 2>/dev/null | sed -n '2p')
    case "$px" in *"(112,48,192)"*|*"#7030C0"*) pass "the = key is painted in the accent colour";;
                  *) fail "the = key's pixel is $px";; esac
fi

# --- the calculator: URI --------------------------------------------------------------------
DUMP2="$T/calc2.dump"
SG_CALC_DUMP="$(wine winepath -w "$DUMP2" | tr -d '\r')" wine start calculator:scientific >/dev/null 2>&1
i=0; while [ ! -s "$DUMP2" ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
if grep -q '^MODE Scientific' "$DUMP2" 2>/dev/null; then pass "calculator:scientific opens it in Scientific"
else fail "calculator: URI did not open Scientific ($(sed -n 's/^MODE //p' "$DUMP2" 2>/dev/null))"; fi
sleep 1; shot uri

echo "calc-check: $([ $RC = 0 ] && echo PASS || echo FAIL)"
exit $RC
