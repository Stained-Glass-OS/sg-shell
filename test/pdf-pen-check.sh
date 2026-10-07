#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for SG PDF's pen ink (src/pdf/interact.c tool_pointer, with sg-session's
# sg-pdf drawing it): a real pen stroke -- sg-compositor's test build
# (-Dtest-tablet=true) feeds a pen through its real input path, Xwayland makes
# it an X tablet, wine-sg (1000/1150) gives SG PDF its WM_POINTER messages
# with GetPointerPenInfo's pressure. The pen taps Comment's Draw and draws
# left to right pressing harder and harder; the pen taps Save. The saved file
# (read with MuPDF from outside SG PDF): a standard Ink annotation whose
# widths follow the pressure (/SGInkWidths, smallest to largest at least
# 1:3), drawn thin at the light end and thick at the firm end.
#
#   WINE=<wine-sg build>/wine sh test/pdf-pen-check.sh [--mutants]
#   SG_COMPOSITOR_SRC=<sg-compositor checkout> (default: beside this repo)
# Mutant (must fail it): SG_MUTANT_PEN_NO_PRESSURE (every point half pressure).
# Skips (77) with a Wine that lists no pen to programs (wine-sg 10.0-197+), or without
# Xwayland, meson, ninja, python3-pymupdf, sg-pdf, or the compositor source.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE="${WINE:-/opt/wine-sg/bin/wine}"
WINESERVER="${WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
EXE="${SG_PDF_EXE:-$HERE/build/sg-pdf64.exe}"
SRC="${SG_COMPOSITOR_SRC:-$HERE/../sg-compositor}"
[ -d "$SRC" ] || SRC="${SG_REAL_HOME:-$HOME}/Stained-Glass-OS/sg-compositor"
PY=/usr/bin/python3
RC=0; CP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

if [ "${1:-}" = --mutants ]; then
    MINGW64=${MINGW64:-x86_64-w64-mingw32-gcc}; WINDRES64=${WINDRES64:-x86_64-w64-mingw32-windres}
    M=$(mktemp -d /var/tmp/sg-pdf-pen-mut.XXXXXX); mrc=0
    "$WINDRES64" -I "$HERE/src/pdf" -I "$HERE/build" "$HERE/src/pdf/pdf.rc" -O coff -o "$M/res.o" || { echo "FAIL  resources"; exit 1; }
    for m in PEN_NO_PRESSURE; do
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
for need in Xwayland meson ninja xdpyinfo; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
$PY -c 'import pymupdf' 2>/dev/null || { echo "SKIP: python3-pymupdf missing"; exit 77; }
[ -x "$WINE" ] && [ -f "$EXE" ] || { echo "SKIP: no wine at $WINE or no $EXE"; exit 77; }
[ -n "$HELPER" ] && [ -x "$HELPER" ] || { echo "SKIP: sg-pdf (sg-session) not found"; exit 77; }
grep -q '"pressure %lf"' "$SRC/seat.c" 2>/dev/null || { echo "SKIP: no sg-compositor 0.2.0+sg39 or later at $SRC"; exit 77; }
printf 'quit\n' | "$HELPER" --serve >/dev/null 2>&1
grep -q 'pressure' "$(dirname "$HELPER")/../pdf/sgpdf.py" 2>/dev/null || grep -q 'SGInkWidths' /usr/lib/stained-glass/pdf/sgpdf.py 2>/dev/null \
    || { echo "SKIP: $HELPER draws no pen pressure (sg-session too old)"; exit 77; }

T=$(mktemp -d /var/tmp/sg-pdf-pen.XXXXXX); chmod 755 "$T"
stop() { [ -n "$CP" ] && kill -9 "$CP" 2>/dev/null; wait "$CP" 2>/dev/null; CP=""; }
# shellcheck disable=SC2317
cleanup() { WINEPREFIX="$T/pfx" "$WINESERVER" -k 2>/dev/null; stop; rm -rf "$T"; }
trap cleanup EXIT INT TERM

meson setup "$T/comp" "$SRC" -Dtest-tablet=true -Dman-pages=disabled --buildtype=release >"$T/meson.log" 2>&1 &&
    ninja -C "$T/comp" >"$T/ninja.log" 2>&1 || { tail -20 "$T/meson.log" "$T/ninja.log"; echo "SKIP: cannot build the test compositor"; exit 77; }
mkfifo "$T/pen"
unset DISPLAY WAYLAND_DISPLAY
SG_TEST_TABLET_FIFO="$T/pen" WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$T/comp/sg-compositor" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$DISPLAY > $T/disp; exec sleep 900" >"$T/comp.log" 2>&1 &
CP=$!
i=0; while [ ! -s "$T/disp" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
DISPLAY=$(cat "$T/disp" 2>/dev/null); export DISPLAY
[ -n "$DISPLAY" ] && [ "$DISPLAY" != ":0" ] || { echo "FAIL  no display from the compositor"; cat "$T/comp.log"; exit 1; }
sleep 1
set -- $(xdpyinfo 2>/dev/null | sed -n 's/.*dimensions: *\([0-9]*\)x\([0-9]*\) pixels.*/\1 \2/p')
SW=${1:-1280}; SH=${2:-720}
echo "      screen ${SW}x$SH"

export WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all WINESERVER
export SG_PDF="$HELPER" SG_PDF_QUIET=1
# SG PDF starts again under its Linux half, which runs "wine": this one
PATH="$(dirname "$WINE"):$PATH"; export PATH
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w
DOCS="$WINEPREFIX/drive_c/docs"; mkdir -p "$DOCS" "$WINEPREFIX/drive_c/dumps"
$PY -c 'import sys, pymupdf as f; d = f.open(); d.new_page(width=612, height=792); d.save(sys.argv[1])' "$DOCS/pen.pdf"

D="$WINEPREFIX/drive_c/dumps/pen"
field() { sed -n "s/^$1 //p" "$D" 2>/dev/null | head -1; }
wait_field() {
    i=0
    while [ "$(field "$1")" != "$2" ] && [ $i -lt $(( $3 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    [ "$(field "$1")" = "$2" ]
}
wd() { "$WINE" winepath -w "$1" 2>/dev/null | tr -d '\r'; }
# the pen, a line a frame (a real device's events come in frames of their own)
feed() { for l in "$@"; do printf '%s\n' "$l" > "$T/pen"; sleep 0.12; done; }
nx() { $PY -c "print('%.5f' % (($1) / $SW))"; }
ny() { $PY -c "print('%.5f' % (($1) / $SH))"; }
tap() {  # X Y (screen)
    x=$(nx "$1"); y=$(ny "$2")
    feed "in $x $y" "move $x $y" "pressure 0.5" down up out
    sleep 1
}
page_xy() {  # X Y (points) -> screen x y on page 1
    set -- "$1" "$2" $(field "page 1") 0 0 0 0
    $PY -c "l,t,r,b=$3,$4,$5,$6; s=(r-l)/612.0; print(int(l+$1*s), int(t+$2*s))"
}

# Xwayland makes the pen's X device when the pen first comes over one of its
# windows, and a Windows program reads the pens when it starts (wine-sg 1150
# also takes a pen that appears later, where the X server says so): SG PDF
# is started, the pen brought over it once, and SG PDF started again.
start_pdf() {
    rm -f "$D"
    SG_PDF_DUMP="$(wd "$D")" timeout -s KILL 600 "$WINE" "$EXE" "$(wd "$DOCS/pen.pdf")" >"$T/app.log" 2>&1 &
    wait_field bridged 1 40 || fail "SG PDF did not start"
    wait_field pages 1 30 || fail "the document did not open: pages '$(field pages)'"
}
start_pdf
set -- $(field "page 1")
[ $# -ge 4 ] && feed "in $(nx $(( ($1 + $3) / 2 ))) $(ny $(( ($2 + $4) / 2 )))" "move 0.5 0.5" out
sleep 1
"$WINE" taskkill /f /im sg-pdf64.exe >/dev/null 2>&1; sleep 2
start_pdf
sleep 2
[ "$(field pointerdevices)" != 0 ] || { echo "SKIP: $WINE lists no pen to programs (wine-sg 10.0-197 or later needed)"; exit 77; }
# Comment's Draw, from the quick tools rail, with the pen
set -- $(grep '^railbtn draw ' "$D" | head -1)
if [ $# -ge 4 ]; then tap "$3" "$4"; else fail "no Draw on the rail: $(grep '^railbtn' "$D" | head -3)"; fi
wait_field tool '2 comment' 5 && pass "the pen taps Draw (Comment)" || fail "tool '$(field tool)' after tapping Draw"

# left to right, pressing harder and harder
set -- $(page_xy 100 300); x0=$1; y0=$2
set -- $(page_xy 500 300); x1=$1
lines="in $(nx "$x0") $(ny "$y0")|move $(nx "$x0") $(ny "$y0")|pressure 0.05|down"
k=1; while [ $k -le 20 ]; do
    p=$($PY -c "print('%.3f' % (0.05 + 0.95 * $k / 20))")
    x=$(( x0 + (x1 - x0) * k / 20 ))
    lines="$lines|pressure $p|move $(nx "$x") $(ny "$y0")"
    k=$((k + 1))
done
lines="$lines|up|out"
IFS='|'; set -- $lines; unset IFS
feed "$@"
wait_field listitems 1 10 && pass "the stroke is a comment" || fail "no comment after the stroke: listitems '$(field listitems)'"
set -- $(grep '^button save ' "$D" | head -1)
if [ $# -ge 4 ]; then tap "$3" "$4"; else fail "no Save button"; fi
wait_field dirty 0 15 && pass "saved (the pen taps Save)" || fail "not saved: dirty '$(field dirty)'"

R=$($PY - "$DOCS/pen.pdf" <<'PYEOF'
import re, sys
import pymupdf as fitz
d = fitz.open(sys.argv[1])
p = d[0]
a = p.first_annot
if not a or a.type[0] != fitz.PDF_ANNOT_INK:
    print("none 0 0 0 0"); sys.exit()
t, v = d.xref_get_key(a.xref, "SGInkWidths")
ws = [float(x) for x in re.findall(r"[\d.]+", v)] if t == "array" else []
r = a.rect
pix = p.get_pixmap(dpi=144)
def thick(xpt):
    X = int(xpt * 2)
    return sum(1 for y in range(int(r.y0 * 2) - 4, int(r.y1 * 2) + 4)
               if 0 <= y < pix.height and min(pix.pixel(X, y)[:3]) < 200)
x0, x1 = r.x0 + 0.15 * r.width, r.x1 - 0.15 * r.width
print("ink %d %.2f %.2f %d %d" % (len(ws), min(ws) if ws else 0, max(ws) if ws else 0, thick(x0), thick(x1)))
PYEOF
)
echo "      saved: $R"
set -- $R
[ "$1" = ink ] && pass "a standard Ink annotation" || fail "no Ink annotation in the saved file"
[ "${2:-0}" -ge 5 ] && $PY -c "import sys; sys.exit(0 if float('${3:-0}') > 0 and float('${4:-0}') >= 3 * float('${3:-0}') else 1)" \
    && pass "its widths follow the pen's pressure (${3:-?} to ${4:-?} pt over $2 points)" \
    || fail "widths: ${2:-0} points, ${3:-?} to ${4:-?} pt"
[ "${5:-0}" -ge 1 ] && [ "${6:-0}" -ge $(( ${5:-0} * 2 )) ] && pass "drawn thin where the pen pressed lightly, thick where hard (${5:-?} px, ${6:-?} px)" \
    || fail "drawn ${5:-?} px and ${6:-?} px"

"$WINESERVER" -k 2>/dev/null
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
