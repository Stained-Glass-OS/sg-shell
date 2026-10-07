#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for SG PDF's Full Screen Mode (View > Full Screen Mode, F11, Ctrl+L;
# src/pdf/present.c), driven like a person on the X keyboard and mouse, on a
# shell desktop under Xvfb:
#
#   F11 shows the page in view alone, over the whole screen (the taskbar
#   too), as large as the screen holds it, centred on black; Right, Space,
#   Page Down and a click go forward, Left, Page Up and a right click back,
#   Home and End to the first and the last, never past them; Esc ends it
#   and the window shows the page the presentation was on; Ctrl+L starts it
#   too and ends it again.
#
# Mutants (each must fail it): SG_MUTANT_PRESENT_WINDOWED (a window, not the
# screen), SG_MUTANT_PRESENT_STUCK (the forward keys do nothing).
#   sh test/pdf-present-check.sh [--mutants]
# SG_PDF_EXE tests another build, SG_PDF_HELPER another sg-pdf. Skips (77)
# without wine-sg, Xvfb, xdotool, ImageMagick, python3-pymupdf.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_PDF_EXE:-$HERE/build/sg-pdf64.exe}"
PY=/usr/bin/python3
OUT="$HERE/build"
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

if [ "${1:-}" = --mutants ]; then
    MINGW64=${MINGW64:-x86_64-w64-mingw32-gcc}; WINDRES64=${WINDRES64:-x86_64-w64-mingw32-windres}
    M=$(mktemp -d /var/tmp/sg-pdf-present-mut.XXXXXX); mrc=0
    "$WINDRES64" -I "$HERE/src/pdf" -I "$HERE/build" "$HERE/src/pdf/pdf.rc" -O coff -o "$M/res.o" || { echo "FAIL  resources"; exit 1; }
    for m in PRESENT_WINDOWED PRESENT_STUCK; do
        # shellcheck disable=SC2046
        "$MINGW64" -O2 -municode -mwindows -Wno-missing-field-initializers -DSG_MUTANT_$m -o "$M/$m.exe" \
            "$HERE"/src/pdf/*.c "$M/res.o" -lcomctl32 -lcomdlg32 -lshell32 -lwinspool -lgdi32 -luser32 -ladvapi32 -lole32 \
            || { echo "FAIL  mutant $m did not build"; mrc=1; continue; }
        if SG_PDF_EXE="$M/$m.exe" sh "$0" > "$M/$m.log" 2>&1; then echo "FAIL  mutant $m survived"; mrc=1
        else echo "PASS  mutant $m killed ($(grep -c '^FAIL' "$M/$m.log") checks failed)"; fi
    done
    rm -rf "$M"; exit $mrc
fi

HELPER="${SG_PDF_HELPER:-}"
if [ -z "$HELPER" ]; then
    for h in "$HERE/../sg-session/bin/sg-pdf" /usr/bin/sg-pdf; do [ -x "$h" ] && { HELPER=$(readlink -f "$h"); break; }; done
fi
for need in Xvfb xdotool import; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
$PY -c 'import pymupdf' 2>/dev/null || { echo "SKIP: python3-pymupdf missing"; exit 77; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
[ -n "$HELPER" ] && [ -x "$HELPER" ] || { echo "SKIP: sg-pdf (sg-session) not found"; exit 77; }

T=$(mktemp -d /var/tmp/sg-pdf-present.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

Xvfb -displayfd 3 -screen 0 1280x900x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
DPY=$(cat "$T/display" 2>/dev/null); [ -n "$DPY" ] || { echo "FAIL  Xvfb did not start"; exit 1; }
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH" SG_PDF="$HELPER" SG_PDF_QUIET=1
wine wineboot --init >/dev/null 2>&1; wineserver -w
wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
wine reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x900 /f >/dev/null 2>&1
wineserver -w
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1280x900 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

# five pages, each a different colour band and its number
DOCS="$WINEPREFIX/drive_c/docs"; mkdir -p "$DOCS" "$WINEPREFIX/drive_c/dumps"
$PY - "$DOCS/slides.pdf" <<'PYEOF'
import sys
import pymupdf as fitz
d = fitz.open()
for i in range(5):
    p = d.new_page(width=792, height=612)          # landscape slides
    p.draw_rect(p.rect, color=None, fill=(1, 1, 1))
    p.draw_rect(fitz.Rect(0, 0, 792, 90), color=None, fill=(0.1, 0.3, 0.8))
    p.insert_text((60, 330), "Slide %d" % (i + 1), fontsize=72)
d.save(sys.argv[1])
PYEOF

D="$WINEPREFIX/drive_c/dumps/present"
field() { sed -n "s/^$1 //p" "$D" 2>/dev/null | head -1; }
wait_field() {
    i=0
    while [ "$(field "$1")" != "$2" ] && [ $i -lt $(( $3 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    [ "$(field "$1")" = "$2" ]
}
wd() { wine winepath -w "$1" 2>/dev/null | tr -d '\r'; }
SG_PDF_DUMP="$(wd "$D")" wine "$EXE" "$(wd "$DOCS/slides.pdf")" >/dev/null 2>&1 &
wait_field bridged 1 30 || fail "SG PDF did not start"
wait_field pages 5 30 || fail "the slides did not open: pages '$(field pages)'"
sleep 2
# the view on page 2
xdotool key ctrl+g; sleep 0.5; xdotool type 2; xdotool key Return; sleep 1
wait_field current 2 5 || fail "go to page 2: current '$(field current)'"

# what the screen shows: the share of black, and the white page's box
look() {
    import -window root "$T/s.png" 2>/dev/null
    $PY - "$T/s.png" <<'PYEOF'
import sys
import pymupdf as fitz
p = fitz.Pixmap(sys.argv[1])
w, h = p.width, p.height
black = white = 0
xs, ys = [], []
for y in range(0, h, 4):
    for x in range(0, w, 4):
        c = p.pixel(x, y)[:3]
        if max(c) < 16:
            black += 1
        elif min(c) > 240:
            white += 1
            xs.append(x); ys.append(y)
n = (w // 4 + (w % 4 > 0)) * (h // 4 + (h % 4 > 0))
box = (min(xs), min(ys), max(xs), max(ys)) if xs else (0, 0, 0, 0)
print("black=%d white=%d box=%d,%d,%d,%d" % (100 * black // n, 100 * white // n, *box))
PYEOF
}

xdotool key F11
if wait_field present '1 2' 5; then pass "F11: Full Screen Mode, on the page in view (2)"
else fail "F11: present '$(field present)'"; fi
sleep 1.5
L=$(look); echo "      screen: $L"
set -- $(echo "$L" | sed 's/[a-z]*=//g; s/,/ /g')
blk=$1; wht=$2; bx0=$3; by0=$4; bx1=$5; by1=$6
[ "$blk" -ge 3 ] && [ "$wht" -ge 50 ] && pass "the page alone on black ($blk% black, $wht% page)" \
    || fail "not the page on black: $L"
# a 792x612 page on 1280x900: as tall as the screen (1164x900), centred left to right
[ "$by1" -ge 884 ] && [ $(( bx1 - bx0 )) -ge 1140 ] && [ $(( (bx0 + bx1) / 2 )) -ge 630 ] && [ $(( (bx0 + bx1) / 2 )) -le 650 ] \
    && pass "as large as the screen holds it, centred (page at $bx0,$by0-$bx1,$by1)" \
    || fail "page box $bx0,$by0-$bx1,$by1 on 1280x900"
cp "$T/s.png" "$OUT/pdf-present.png" 2>/dev/null

xdotool key Right; wait_field present '1 3' 3 && pass "Right: the next page" || fail "Right: '$(field present)'"
xdotool key space; wait_field present '1 4' 3 && pass "Space: the next" || fail "Space: '$(field present)'"
xdotool key Next; wait_field present '1 5' 3 && pass "Page Down: the next" || fail "Page Down: '$(field present)'"
xdotool key Right; sleep 0.8; [ "$(field present)" = "1 5" ] && pass "never past the last page" || fail "past the end: '$(field present)'"
xdotool key Left; wait_field present '1 4' 3 && pass "Left: the page before" || fail "Left: '$(field present)'"
xdotool key Prior; wait_field present '1 3' 3 && pass "Page Up: before" || fail "Page Up: '$(field present)'"
xdotool mousemove 640 450 click 1; wait_field present '1 4' 3 && pass "a click: the next page" || fail "click: '$(field present)'"
xdotool click 3; wait_field present '1 3' 3 && pass "a right click: the page before" || fail "right click: '$(field present)'"
xdotool key Home; wait_field present '1 1' 3 && pass "Home: the first page" || fail "Home: '$(field present)'"
xdotool key Left; sleep 0.8; [ "$(field present)" = "1 1" ] && pass "never before the first" || fail "before the first: '$(field present)'"
xdotool key End; wait_field present '1 5' 3 && pass "End: the last page" || fail "End: '$(field present)'"
xdotool key Left
wait_field present '1 4' 3
xdotool key Escape
wait_field present '0 0' 3 && pass "Esc ends it" || fail "Esc: '$(field present)'"
wait_field current 4 5 && pass "and the window shows the page it was on (4)" || fail "after Esc: current '$(field current)'"
sleep 1
L=$(look); set -- $(echo "$L" | sed 's/[a-z]*=//g; s/,/ /g')
[ "$1" -lt 3 ] && pass "the window is back (no black screen: $1%)" || fail "after Esc the screen is $1% black"
xdotool key ctrl+l; wait_field present '1 4' 5 && pass "Ctrl+L starts it too" || fail "Ctrl+L: '$(field present)'"
xdotool key ctrl+l; wait_field present '0 0' 5 && pass "and Ctrl+L ends it" || fail "Ctrl+L again: '$(field present)'"

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
