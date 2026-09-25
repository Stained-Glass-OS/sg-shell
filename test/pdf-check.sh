#!/bin/sh
# Gate for the PDF Viewer (sg-pdf64.exe) and its Linux half (sg-session's
# sg-pdf, poppler). On a shell desktop under Xvfb, with defaults/80-sg-pdf.reg
# imported (pointing at this build), and a four-page PDF made at test time
# (test/mkpdf.py):
#
#   - a .pdf in a folder with a space opened through its association shows
#     4 pages, the title "<file> - PDF Viewer", its bookmarks, and page 1's
#     first word in dark pixels where poppler says it is
#   - scrolling (Page Down) reaches page 2, whose big word and purple bar are
#     on the screen where the text layout puts them
#   - find: "zebra" has 2 hits, the first on page 2 painted in the current
#     hit's orange; F3 goes to the second, on page 3
#   - a mouse drag over page 2's word selects it and Ctrl+C puts exactly
#     "Zebra" on the clipboard; Ctrl+A, Ctrl+C copies every page's text
#   - zoom in/out and back to fit width; rotate (the page turns landscape)
#   - page 1's link goes to page 3; the bookmarks and thumbnails sidebars
#     go to the page clicked
#   - double-clicking the .pdf in an Explorer window opens it
#   - a password-protected PDF (qpdf, when installed) asks for the password;
#     a damaged file and a missing Linux half say so
#
# Screenshots: build/pdf-*.png. Needs wine-sg, Xvfb, xdotool, xclip,
# ImageMagick, python3 with PIL and cairo, and sg-pdf with poppler's GI
# bindings; skips (77) without them. SG_PDF_EXE tests another build
# (mutation testing), SG_PDF_HELPER another sg-pdf.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_PDF_EXE:-$HERE/build/sg-pdf64.exe}"
OUT="$HERE/build"
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

HELPER="${SG_PDF_HELPER:-}"
if [ -z "$HELPER" ]; then
    for h in "$HERE/../sg-session/bin/sg-pdf" /usr/bin/sg-pdf; do [ -x "$h" ] && { HELPER=$(readlink -f "$h"); break; }; done
fi
for need in Xvfb xdotool xclip import python3; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
python3 -c 'import PIL, cairo' 2>/dev/null || { echo "SKIP: python3 PIL or cairo missing"; exit 77; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
[ -n "$HELPER" ] && [ -x "$HELPER" ] || { echo "SKIP: sg-pdf (sg-session) not found"; exit 77; }
/usr/bin/python3 -c 'import gi; gi.require_version("Poppler", "0.18")' 2>/dev/null || { echo "SKIP: gir1.2-poppler-0.18 missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-pdf-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
DPY=$(cat "$T/display" 2>/dev/null); [ -n "$DPY" ] || { echo "FAIL  Xvfb did not start"; exit 1; }
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH" SG_PDF="$HELPER"
wine wineboot --init >/dev/null 2>&1; wineserver -w
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
python3 - "$HERE/defaults/80-sg-pdf.reg" "$winexe" > "$T/pdf.reg" <<'EOF2'
import sys
reg = open(sys.argv[1]).read()
esc = lambda p: p.replace("\\", "\\\\")
sys.stdout.write(reg.replace(esc(r"Z:\usr\libexec\stained-glass\shell\sg-pdf64.exe"), esc(sys.argv[2])))
EOF2
grep -qF "$(printf '%s' "$winexe" | sed 's/\\/\\\\/g')" "$T/pdf.reg" && ! grep -q 'Z:.*usr.*libexec' "$T/pdf.reg" \
    || { fail "could not point the defaults at the build"; exit 1; }
wine reg import "$(wine winepath -w "$T/pdf.reg" | tr -d '\r')" >/dev/null 2>&1 || fail "reg import failed"
wineserver -w

# --- the documents -------------------------------------------------------------------------
DOCS="$WINEPREFIX/drive_c/my docs"; mkdir -p "$DOCS" "$WINEPREFIX/drive_c/folder" "$WINEPREFIX/drive_c/dumps"
python3 "$HERE/test/mkpdf.py" "$DOCS/gate file.pdf" || { fail "could not make the test PDF"; exit 1; }
cp "$DOCS/gate file.pdf" "$WINEPREFIX/drive_c/folder/report.pdf"
printf 'this is not a PDF\n' > "$DOCS/broken.pdf"
HAVE_QPDF=0
if command -v qpdf >/dev/null && qpdf --encrypt sesame owner 256 -- "$DOCS/gate file.pdf" "$DOCS/locked.pdf" 2>/dev/null; then HAVE_QPDF=1; fi

WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

D="$WINEPREFIX/drive_c/dumps/d1"; D2="$WINEPREFIX/drive_c/dumps/d2"; D3="$WINEPREFIX/drive_c/dumps/d3"
field() { sed -n "s/^$1 //p" "${2:-$D}" 2>/dev/null | head -1; }
wait_field() {  # name value seconds [dump]
    i=0
    while [ "$(field "$1" "${4:-$D}")" != "$2" ] && [ $i -lt $(( $3 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    [ "$(field "$1" "${4:-$D}")" = "$2" ]
}
wait_line() {  # regex seconds [dump]
    i=0
    while ! grep -qE "$1" "${3:-$D}" 2>/dev/null && [ $i -lt $(( $2 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    grep -qE "$1" "${3:-$D}" 2>/dev/null
}
shot() { sleep 0.4; import -window root "$OUT/pdf-$1.png" 2>/dev/null; }
# pixel measures on a screenshot: dark share of a rectangle, colour at a point, share near a colour
dark() { python3 -c "
import sys; from PIL import Image
im = Image.open(sys.argv[1]).convert('L'); x1, y1, x2, y2 = map(int, sys.argv[2:6])
px = [im.getpixel((x, y)) for y in range(max(y1,0), min(y2,im.size[1])) for x in range(max(x1,0), min(x2,im.size[0]))]
print(int(100 * sum(1 for p in px if p < 80) / max(len(px), 1)))" "$@"; }
colour() { python3 -c "
import sys; from PIL import Image
print('%d,%d,%d' % Image.open(sys.argv[1]).convert('RGB').getpixel((int(sys.argv[2]), int(sys.argv[3]))))" "$@"; }
share() { python3 -c "
import sys; from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB'); x1, y1, x2, y2 = map(int, sys.argv[2:6]); c = [int(v) for v in sys.argv[6].split(',')]
px = [im.getpixel((x, y)) for y in range(y1, y2) for x in range(x1, x2)]
print(int(100 * sum(1 for p in px if all(abs(a - b) <= 30 for a, b in zip(p, c))) / max(len(px), 1)))" "$@"; }
near() { python3 -c "import sys; a=[int(x) for x in sys.argv[1].split(',')]; b=[int(x) for x in sys.argv[2].split(',')]; sys.exit(0 if all(abs(x-y)<=20 for x,y in zip(a,b)) else 1)" "$1" "$2" 2>/dev/null; }
wd() { wine winepath -w "$1" 2>/dev/null | tr -d '\r'; }
clip() { xclip -o -selection clipboard 2>/dev/null | tr -d '\r'; }

# --- open through the association ---------------------------------------------------------------
SG_PDF_DUMP="$(wd "$D")" wine start 'C:\my docs\gate file.pdf' >/dev/null 2>&1 &
if wait_field file 'C:\my docs\gate file.pdf' 30; then pass "gate file.pdf opened through its association (a path with spaces)"
else fail "the PDF did not open through its association (dump: '$(field file)')"; fi
wait_field title 'gate file.pdf - PDF Viewer' 5 && pass "title is 'gate file.pdf - PDF Viewer'" || fail "title is '$(field title)'"
[ "$(field bridged)" = 1 ] && pass "connected to poppler (sg-pdf's bridge)" || fail "not bridged"
[ "$(field pages)" = 4 ] && pass "4 pages" || fail "pages: '$(field pages)'"
[ "$(field outline)" = 4 ] && grep -q '^bookmark 1 1 Section One A' "$D" && pass "4 bookmarks, 'Section One A' nested under page 1's" \
    || fail "outline: $(grep -c ^bookmark "$D") bookmarks"
wait_line '^page 1 .* 1$' 15 && pass "page 1 rendered at the current zoom" || fail "page 1 not rendered: $(grep '^page 1' "$D")"
shot open
set -- $(field 'word 1')
if [ $# -ge 5 ] && [ "$5" = Alpha ]; then
    dk=$(dark "$OUT/pdf-open.png" "$1" "$2" "$3" "$4")
    [ "$dk" -ge 15 ] 2>/dev/null && pass "page 1's 'Alpha' is ink on the screen where poppler puts it ($dk% dark)" \
        || fail "page 1's 'Alpha' is not on the screen ($dk% dark at $1,$2-$3,$4)"
else fail "no first word for page 1: '$*'"; fi

# --- scroll to page 2 -----------------------------------------------------------------------------
xdotool key Next; sleep 0.5; xdotool key Next
if wait_field current 2 5; then pass "Page Down twice: page 2 is the current page"
else fail "after Page Down: current page '$(field current)'"; fi
wait_line '^page 2 .* 1$' 15 || fail "page 2 not rendered: $(grep '^page 2' "$D")"
set -- $(field 'word 2')
if [ $# -ge 5 ] && [ "$5" = Zebra ]; then
    shot page2
    dk=$(dark "$OUT/pdf-page2.png" "$1" "$2" "$3" "$4")
    [ "$dk" -ge 25 ] 2>/dev/null && pass "page 2's 'Zebra' is ink on the screen after scrolling ($dk% dark)" \
        || fail "page 2's 'Zebra' is not on the screen ($dk% dark at $1,$2-$3,$4)"
    # the purple bar: 72..540 x 200..260 points on the page
    set -- $(field 'page 2')
    s=$(python3 -c "print(($3 - $1) / 612)")
    bx=$(python3 -c "print(int($1 + 306 * $s))"); by=$(python3 -c "print(int($2 + 230 * $s))")
    c=$(colour "$OUT/pdf-page2.png" "$bx" "$by")
    near "$c" "51,26,115" && pass "page 2's purple bar is on the screen where the page puts it ($c)" \
        || fail "page 2's purple bar: $c at $bx,$by"
else fail "no first word for page 2: '$*'"; fi

# --- find ---------------------------------------------------------------------------------------
xdotool key ctrl+f; sleep 0.3
wait_field focus find 3 || fail "Ctrl+F: focus '$(field focus)'"
xdotool type zebra; xdotool key Return
if wait_field hits 2 8; then pass "find 'zebra': 2 hits (whole document, any case)"; else fail "find: hits '$(field hits)'"; fi
set -- $(field hitrect)
if [ "$(field hit)" = 1 ] && [ "${1:-}" = 2 ]; then
    shot find
    sh=$(share "$OUT/pdf-find.png" "$2" "$3" "$4" "$5" 255,150,50)
    [ "$sh" -ge 25 ] 2>/dev/null && pass "the current hit on page 2 is painted orange ($sh% of it)" || fail "hit not highlighted ($sh% orange)"
else fail "first hit: hit '$(field hit)' rect '$*'"; fi
xdotool key F3
if wait_field hit 2 5 && [ "$(field hitrect | cut -d' ' -f1)" = 3 ] && [ "$(field current)" = 3 ]; then pass "F3: the second hit, on page 3, scrolled to"
else fail "F3: hit '$(field hit)' rect '$(field hitrect)' current '$(field current)'"; fi
xdotool key Escape
wait_field hits 0 3 && pass "Escape clears the search" || fail "Escape: hits '$(field hits)'"

# --- select and copy -----------------------------------------------------------------------------
xdotool key ctrl+g; xdotool type 2; xdotool key Return
wait_field current 2 5 || fail "Ctrl+G 2: current '$(field current)'"
sleep 0.5
set -- $(field 'word 2')
if [ $# -ge 5 ]; then
    y=$(( ($2 + $4) / 2 ))
    xdotool mousemove $(( $1 + 3 )) $y mousedown 1 mousemove $(( $1 + 40 )) $y mousemove $(( $3 - 3 )) $y mouseup 1
    sleep 0.3
    wait_field selected 5 3 && pass "dragging over 'Zebra' selects 5 characters" || fail "drag: selected '$(field selected)'"
    printf 'nothing' | xclip -selection clipboard
    xdotool key ctrl+c; sleep 0.5
    [ "$(clip)" = Zebra ] && pass "Ctrl+C put 'Zebra' on the clipboard" || fail "clipboard is '$(clip)'"
    shot select
fi
xdotool key ctrl+a; sleep 0.3; xdotool key ctrl+c; sleep 0.8
all=$(clip)
case "$all" in *"Alpha document"*"Zebra crossing"*"Omega third page"*"Delta fourth page"*) pass "Ctrl+A, Ctrl+C: every page's text, in order" ;;
    *) fail "select all copied: $(printf '%s' "$all" | head -c 200)" ;; esac
xdotool key Escape

# --- zoom and rotate ------------------------------------------------------------------------------
z0=$(field zoom)
xdotool key ctrl+equal
i=0; while [ "$(field zoom)" = "$z0" ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
[ "$(field zoom)" -gt "$z0" ] 2>/dev/null && [ "$(field fit)" = none ] && pass "Ctrl+Plus: zoom $z0% -> $(field zoom)%" || fail "zoom in: $z0 -> $(field zoom), fit $(field fit)"
xdotool key ctrl+minus ctrl+minus
sleep 0.5
[ "$(field zoom)" -lt "$z0" ] 2>/dev/null && pass "Ctrl+Minus twice: $(field zoom)%" || fail "zoom out: $(field zoom)"
xdotool key ctrl+0
wait_field fit width 3 && [ "$(field zoom)" = "$z0" ] && pass "Ctrl+0: fit width again ($z0%)" || fail "Ctrl+0: fit $(field fit) zoom $(field zoom)"
xdotool key ctrl+bracketright
if wait_field rot 90 3; then
    set -- $(field "page $(field current)")
    [ $# -ge 4 ] && [ $(( $3 - $1 )) -gt $(( $4 - $2 )) ] && pass "Ctrl+]: rotated 90, the page is wider than tall" || fail "rotate: page rect $*"
    wait_line "^page $(field current) .* 1$" 10 || true
    shot rotate
else fail "Ctrl+]: rot '$(field rot)'"; fi
xdotool key ctrl+bracketleft
wait_field rot 0 3 || fail "Ctrl+[: rot '$(field rot)'"

# --- links, bookmarks, thumbnails ---------------------------------------------------------------------
xdotool key ctrl+Home
wait_field current 1 3 || fail "Ctrl+Home: current '$(field current)'"
sleep 0.5
grep -q '^link 1 .* 0 https://example.com/' "$D" && pass "page 1's web link is https://example.com/" || fail "web link missing"
set -- $(grep '^link 1 .* 3 *$' "$D" | head -1)
if [ $# -ge 7 ]; then
    xdotool mousemove $(( ($3 + $5) / 2 )) $(( ($4 + $6) / 2 )) click 1
    wait_field current 3 5 && pass "page 1's link goes to page 3" || fail "link: current '$(field current)'"
else fail "no link to page 3 on page 1"; fi
set -- $(field 'button sidebar')
xdotool mousemove "$1" "$2" click 1
if wait_field side outline 3; then
    pass "the sidebar opens on the bookmarks (the document has some)"
    sleep 0.5; shot outline
    set -- $(grep '^treeitem .* Chapter Two$' "$D" | head -1)
    if [ $# -ge 3 ]; then xdotool mousemove "$2" "$3" click 1; wait_field current 2 5 && pass "bookmark 'Chapter Two' goes to page 2" || fail "bookmark: current '$(field current)'"
    else fail "no 'Chapter Two' in the tree"; fi
else fail "sidebar: '$(field side)'"; fi
set -- $(field 'tab thumbs')
[ $# -ge 2 ] && xdotool mousemove "$1" "$2" click 1
if wait_field side thumbs 3 && wait_line '^thumb 1 .* 1$' 10; then
    pass "thumbnails shown and rendered"
    sleep 0.5; shot thumbs
    set -- $(field 'thumb 4')
    # its top: the rest may be below the window
    if [ $# -ge 4 ]; then xdotool mousemove $(( ($1 + $3) / 2 )) $(( $2 + 12 )) click 1; wait_field current 4 5 && pass "thumbnail 4 goes to page 4" || fail "thumb: current '$(field current)'"
    else fail "thumbnail 4 not on the screen"; fi
else fail "thumbnails: side '$(field side)', $(grep -c '^thumb' "$D") shown"; fi
set -- $(field 'button sidebar'); xdotool mousemove "$1" "$2" click 1
wine taskkill /f /im sg-pdf64.exe >/dev/null 2>&1; sleep 1

# --- Explorer: double-click the .pdf -----------------------------------------------------------------
SG_PDF_DUMP="$(wd "$D2")" wine explorer 'C:\folder' >/dev/null 2>&1 &
sleep 5; shot explorer
xy=$(python3 - "$OUT/pdf-explorer.png" <<'EOF'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB"); W, H = im.size
# the file's icon: our purple band -- a small solid patch (a focus rectangle
# in the accent colour is a hollow square; the taskbar is not searched)
purple = {(x, y) for y in range(H - 60) for x in range(W)
          if all(abs(a - b) <= 40 for a, b in zip(im.getpixel((x, y)), (112, 48, 192)))}
best = None
while purple:
    stack = [purple.pop()]; comp = []
    while stack:
        x, y = stack.pop(); comp.append((x, y))
        for n in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if n in purple: purple.remove(n); stack.append(n)
    xs = [p[0] for p in comp]; ys = [p[1] for p in comp]
    w, h = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
    if 4 <= w <= 40 and 2 <= h <= 12 and len(comp) >= 0.6 * w * h and (best is None or len(comp) > len(best)):
        best = comp
print("%d %d" % (sum(p[0] for p in best) // len(best), sum(p[1] for p in best) // len(best)) if best else "")
EOF
)
if [ -n "$xy" ]; then
    # shellcheck disable=SC2086
    xdotool mousemove $xy click --repeat 2 --delay 80 1
    wait_field file 'C:\folder\report.pdf' 20 "$D2" && pass "double-clicking report.pdf in Explorer opens it in the viewer" \
        || fail "Explorer double-click: dump '$(field file "$D2")'"
    wait_field pages 4 5 "$D2" || fail "Explorer-opened document: pages '$(field pages "$D2")'"
    shot explorer-open
else fail "the PDF's icon is not in the Explorer window"; fi
wine taskkill /f /im sg-pdf64.exe >/dev/null 2>&1; sleep 1

# --- a password, a damaged file, no Linux half ---------------------------------------------------------
if [ $HAVE_QPDF = 1 ]; then
    SG_PDF_DUMP="$(wd "$D3")" wine start 'C:\my docs\locked.pdf' >/dev/null 2>&1 &
    sleep 5; shot password
    xdotool type sesame; xdotool key Return
    wait_field pages 4 15 "$D3" && pass "a password-protected PDF asks, and opens with the password" || fail "password: pages '$(field pages "$D3")' error '$(field error "$D3")'"
    wine taskkill /f /im sg-pdf64.exe >/dev/null 2>&1; sleep 1
else echo "NOTE  qpdf not installed: the password check is skipped"; fi
rm -f "$D3"
SG_PDF_DUMP="$(wd "$D3")" wine start 'C:\my docs\broken.pdf' >/dev/null 2>&1 &
wait_line '^error This file could not be opened' 15 "$D3" && pass "a damaged file says it could not be opened" || fail "damaged: error '$(field error "$D3")'"
shot broken
wine taskkill /f /im sg-pdf64.exe >/dev/null 2>&1; sleep 1; rm -f "$D3"
SG_PDF=/nonexistent/sg-pdf SG_PDF_DUMP="$(wd "$D3")" wine start 'C:\my docs\gate file.pdf' >/dev/null 2>&1 &
wait_line '^error PDF viewing needs its Linux half' 15 "$D3" && [ "$(field bridged "$D3")" = 0 ] && pass "without sg-pdf the viewer says what is missing" \
    || fail "no helper: error '$(field error "$D3")'"
wine taskkill /f /im sg-pdf64.exe >/dev/null 2>&1

[ $RC = 0 ] && echo "pdf-check: all passed" || echo "pdf-check: FAILED"
exit $RC
