#!/bin/sh
# Gate for Photos (sg-photos), the image viewer. On a shell desktop under Xvfb,
# with defaults/73-sg-photos.reg imported (pointing at this build):
#
#   - a .png opened through its association (ShellExecute, as a double-click
#     in Explorer does) shows that picture: the screen's pixels at the
#     picture's centre are its colour, the title is "<file> - Photos"
#   - Right and Left walk the folder in Explorer's name order (img1, img2,
#     img10 -- not img1, img10, img2), with the pixels following
#   - zooming in changes the scale; Ctrl+R rotates (the dimensions swap);
#     Alt+Enter shows the file information; F5 runs a slideshow that
#     advances by itself, Escape stops it
#   - Delete, confirmed, moves the file to the Recycle Bin (gone from the
#     folder, in the trash) and shows the next picture
#   - an animated GIF animates; a JPEG's EXIF orientation is honoured
#   - photos.exe through App Paths, and the ms-photos: URI, open a file
#
# Screenshots: build/photos-*.png. Needs wine-sg, Xvfb, xdotool, ImageMagick
# and python3 with PIL; skips (77) without them. SG_PHOTOS_EXE tests another
# build (mutation testing).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_PHOTOS_EXE:-$HERE/build/sg-photos64.exe}"
OUT="$HERE/build"
RC=0; DPY=""; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import convert python3; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
python3 -c 'import PIL' 2>/dev/null || { echo "SKIP: python3 PIL missing"; exit 77; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-photos-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

# A free display, chosen by the server (a fixed number collides with other gates' servers).
Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
DPY=$(cat "$T/display" 2>/dev/null); [ -n "$DPY" ] || { echo "FAIL  Xvfb did not start"; exit 1; }
# The Recycle Bin is the XDG trash: keep it in the test directory.
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
export XDG_DATA_HOME="$T/xdg" PATH="$WINE_DIR/bin:$PATH"
mkdir -p "$XDG_DATA_HOME"
wine wineboot --init >/dev/null 2>&1; wineserver -w
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
# the package's defaults, pointing at this build
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
python3 - "$HERE/defaults/73-sg-photos.reg" "$winexe" > "$T/photos.reg" <<'EOF2'
import sys
reg = open(sys.argv[1]).read()
esc = lambda p: p.replace("\\", "\\\\")
sys.stdout.write(reg.replace(esc(r"Z:\usr\libexec\stained-glass\shell\sg-photos64.exe"), esc(sys.argv[2])))
EOF2
grep -qF "$(printf '%s' "$winexe" | sed 's/\\/\\\\/g')" "$T/photos.reg" && ! grep -q 'Z:.*usr.*libexec' "$T/photos.reg" \
    || { fail "could not point the defaults at the build"; exit 1; }
wine reg import "$(wine winepath -w "$T/photos.reg" | tr -d '\r')" >/dev/null 2>&1 || fail "reg import failed"
wineserver -w

# --- the pictures ----------------------------------------------------------------------------
PICS="$WINEPREFIX/drive_c/pics"; mkdir -p "$PICS" "$WINEPREFIX/drive_c/anim"
python3 - "$PICS" "$WINEPREFIX/drive_c/anim" <<'EOF'
import sys
from PIL import Image
d, a = sys.argv[1], sys.argv[2]
Image.new("RGB", (400, 200), (220, 30, 30)).save(d + "/img1.png")
Image.new("RGB", (400, 200), (30, 200, 60)).save(d + "/img2.png")
Image.new("RGB", (300, 300), (30, 60, 220)).save(d + "/img10.png")
# a JPEG stored 300x120, EXIF orientation 6 (turn 90 degrees clockwise): shown 120x300
im = Image.new("RGB", (300, 120), (240, 200, 20))
ex = Image.Exif(); ex[274] = 6
im.save(a + "/rotated.jpg", exif=ex.tobytes(), quality=95)
# a large picture, reduced to fit: its left half orange, its right half purple
big = Image.new("RGB", (3000, 1500), (240, 140, 20)); big.paste((112, 48, 192), (1500, 0, 3000, 1500))
big.save(a + "/big.png")
f1 = Image.new("RGB", (120, 120), (255, 0, 0)); f2 = Image.new("RGB", (120, 120), (0, 0, 255))
f1.save(a + "/anim.gif", save_all=True, append_images=[f2], duration=300, loop=0)
EOF

# the gate's explorer must own the desktop before any program starts
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

D="$T/dump"; D2="$T/dump2"; D3="$T/dump3"
wd() { wine winepath -w "$1" 2>/dev/null | tr -d '\r'; }
field() { sed -n "s/^$1 //p" "${2:-$D}" 2>/dev/null | head -1; }
wait_field() {  # name value seconds [dump]
    i=0
    while [ "$(field "$1" "${4:-$D}")" != "$2" ] && [ $i -lt $(( $3 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    [ "$(field "$1" "${4:-$D}")" = "$2" ]
}
shot() { import -window root "$OUT/photos-$1.png" 2>/dev/null; }
# the colour at the centre of the drawn picture, as "r,g,b"
centre() {
    set -- $(field drawn "${2:-$D}")
    [ $# -eq 4 ] || { echo none; return; }
    convert "$OUT/photos-$SHOT.png" -format "%[fx:int(255*p{$(( ($1 + $3) / 2 )),$(( ($2 + $4) / 2 ))}.r)],%[fx:int(255*p{$(( ($1 + $3) / 2 )),$(( ($2 + $4) / 2 ))}.g)],%[fx:int(255*p{$(( ($1 + $3) / 2 )),$(( ($2 + $4) / 2 ))}.b)]" info: 2>/dev/null
}
near() {  # "r,g,b" "r,g,b": each channel within 12
    python3 -c "import sys; a=[int(x) for x in sys.argv[1].split(',')]; b=[int(x) for x in sys.argv[2].split(',')]; sys.exit(0 if all(abs(x-y)<=12 for x,y in zip(a,b)) else 1)" "$1" "$2" 2>/dev/null
}
see() {  # name expected-rgb what
    SHOT=$1; sleep 0.7; shot "$1"
    c=$(centre)
    if near "$c" "$2"; then pass "$3 (centre $c)"; else fail "$3: centre is $c, expected $2"; fi
}

# --- open through the association ------------------------------------------------------------
SG_PHOTOS_DUMP="$(wd "$D")" SG_PHOTOS_SLIDE_MS=1000 wine start 'C:\pics\img2.png' >/dev/null 2>&1 &
if wait_field file 'C:\pics\img2.png' 30; then pass "img2.png opened through its association"
else fail "img2.png did not open through its association (dump: $(field file))"; fi
wait_field title 'img2.png - Photos' 5 && pass "title is 'img2.png - Photos'" || fail "title is '$(field title)'"
[ "$(field index)" = "1 of 3" ] && pass "folder read: index 1 of 3" || fail "folder: '$(field index)'"
[ "$(field image)" = "400 200" ] && pass "image is 400x200" || fail "image is '$(field image)'"
see open "30,200,60" "the picture on screen is img2's green"

# --- next and previous -------------------------------------------------------------------
xdotool key Right
wait_field file 'C:\pics\img10.png' 5 && pass "Right: img10.png (name order: img2 then img10)" || fail "Right: '$(field file)'"
see next "30,60,220" "the picture on screen is img10's blue"
xdotool key Left; wait_field file 'C:\pics\img2.png' 5; xdotool key Left
wait_field file 'C:\pics\img1.png' 5 && pass "Left twice: img1.png" || fail "Left twice: '$(field file)'"
see prev "220,30,30" "the picture on screen is img1's red"
wait_field title 'img1.png - Photos' 3 || fail "title after Left: '$(field title)'"

# --- zoom, rotate, information -------------------------------------------------------------
s0=$(field scale)
xdotool key equal
i=0; while [ "$(field scale)" = "$s0" ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
s1=$(field scale)
[ -n "$s1" ] && [ "$s1" -gt "$s0" ] 2>/dev/null && [ "$(field fit)" = 0 ] && pass "zoom in: scale $s0% -> $s1%" || fail "zoom in: scale $s0 -> $s1"
SHOT=zoom; sleep 0.5; shot zoom
xdotool key minus
i=0; while [ "$(field scale)" = "$s1" ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
[ "$(field scale)" -lt "$s1" ] 2>/dev/null && pass "zoom out: scale $s1% -> $(field scale)%" || fail "zoom out: $(field scale)"
xdotool key ctrl+0
wait_field fit 1 3 && pass "Ctrl+0 fits again" || fail "Ctrl+0: fit $(field fit)"
xdotool key ctrl+r
wait_field image '200 400' 5 && [ "$(field rotation)" = 90 ] && pass "Ctrl+R: rotated 90, shown 200x400" || fail "Ctrl+R: image '$(field image)' rotation '$(field rotation)'"
set -- $(field drawn)
[ $# -eq 4 ] && [ $(( $4 - $2 )) -gt $(( $3 - $1 )) ] && pass "the rotated picture is drawn taller than wide" || fail "drawn after rotate: $*"
SHOT=rotate; sleep 0.5; shot rotate
xdotool key alt+Return
wait_field info 1 3 && pass "Alt+Enter: file information shown" || fail "Alt+Enter: info $(field info)"
SHOT=info; sleep 0.7; shot info
xdotool key alt+Return; wait_field info 0 3 || fail "Alt+Enter again: info $(field info)"

# --- slideshow ------------------------------------------------------------------------------
before=$(field file)
xdotool key F5
wait_field slideshow 1 3 && pass "F5: slideshow on" || fail "F5: slideshow $(field slideshow)"
[ "$(field fullscreen)" = 1 ] && pass "the slideshow is full screen" || fail "slideshow not full screen"
sleep 0.4; SHOT=slideshow; shot slideshow
i=0; while [ "$(field file)" = "$before" ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
[ "$(field file)" != "$before" ] && pass "the slideshow advanced by itself ($before -> $(field file))" || fail "the slideshow did not advance"
xdotool key Escape
wait_field slideshow 0 3 && [ "$(field fullscreen)" = 0 ] && pass "Escape stops the slideshow" || fail "Escape: slideshow $(field slideshow) fullscreen $(field fullscreen)"

# --- delete to the Recycle Bin -----------------------------------------------------------------
xdotool key Home
wait_field file 'C:\pics\img1.png' 5 || fail "Home: '$(field file)'"
xdotool key Delete
sleep 1.5; import -window root "$OUT/photos-delete-ask.png" 2>/dev/null
xdotool key Return
wait_field index '0 of 2' 8 && pass "Delete: the folder has 2 left" || fail "Delete: index '$(field index)'"
[ ! -e "$PICS/img1.png" ] && pass "img1.png is gone from the folder" || fail "img1.png still there"
ls "$XDG_DATA_HOME"/Trash/files 2>/dev/null | grep -q '^img1' && pass "img1.png is in the Recycle Bin" || fail "img1.png not in the trash ($(ls -R "$XDG_DATA_HOME" 2>/dev/null | tr '\n' ' '))"
[ "$(field file)" = 'C:\pics\img2.png' ] && pass "the next picture is shown" || fail "after delete: '$(field file)'"

# --- an animated GIF, EXIF orientation, photos.exe and ms-photos: ---------------------------
SG_PHOTOS_DUMP="$(wd "$D2")" wine start photos.exe 'C:\anim\anim.gif' >/dev/null 2>&1 &
if wait_field file 'C:\anim\anim.gif' 20 "$D2"; then pass "photos.exe (App Paths) opened anim.gif"
else fail "photos.exe did not open anim.gif"; fi
case "$(field frames "$D2")" in "2 frame"*) pass "anim.gif has 2 frames" ;; *) fail "frames: '$(field frames "$D2")'" ;; esac
seen=""
i=0; while [ $i -lt 16 ]; do seen="$seen $(field frames "$D2" | awk '{print $3}')"; sleep 0.2; i=$((i + 1)); done
echo "$seen" | grep -q 0 && echo "$seen" | grep -q 1 && pass "the GIF animates (frames seen:$seen)" || fail "the GIF does not animate:$seen"
xdotool key Right
if wait_field file 'C:\anim\big.png' 5 "$D2"; then
    sc=$(field scale "$D2"); set -- $(field drawn "$D2"); set -- $(field canvas "$D2") "$@"
    [ "$sc" -lt 100 ] 2>/dev/null && [ $(( $7 - $5 )) -le $(( $3 - $1 )) ] && pass "a 3000x1500 picture is reduced to fit ($sc%)" \
        || fail "big.png: scale $sc, canvas/drawn $*"
    SHOT=big; sleep 0.7; shot big
    y=$(( ($6 + $8) / 2 ))
    l=$(convert "$OUT/photos-big.png" -format "%[fx:int(255*p{$(( $5 + ($7 - $5) / 4 )),$y}.r)],%[fx:int(255*p{$(( $5 + ($7 - $5) / 4 )),$y}.g)],%[fx:int(255*p{$(( $5 + ($7 - $5) / 4 )),$y}.b)]" info:)
    r=$(convert "$OUT/photos-big.png" -format "%[fx:int(255*p{$(( $7 - ($7 - $5) / 4 )),$y}.r)],%[fx:int(255*p{$(( $7 - ($7 - $5) / 4 )),$y}.g)],%[fx:int(255*p{$(( $7 - ($7 - $5) / 4 )),$y}.b)]" info:)
    near "$l" "240,140,20" && near "$r" "112,48,192" && pass "the reduced picture's halves are orange and purple ($l / $r)" \
        || fail "reduced picture: left $l right $r"
else fail "Right to big.png: '$(field file "$D2")'"; fi
xdotool key Right
if wait_field file 'C:\anim\rotated.jpg' 5 "$D2"; then
    [ "$(field image "$D2")" = "120 300" ] && pass "EXIF orientation 6: rotated.jpg is shown 120x300" \
        || fail "EXIF orientation: rotated.jpg shown as '$(field image "$D2")' (exif $(field exif "$D2"))"
    SHOT=exif; sleep 0.7; shot exif
else fail "Right in the anim folder: '$(field file "$D2")'"; fi

SG_PHOTOS_DUMP="$(wd "$D3")" wine start 'ms-photos:viewer?fileName=C:%5Cpics%5Cimg10.png' >/dev/null 2>&1 &
wait_field file 'C:\pics\img10.png' 20 "$D3" && pass "ms-photos: URI opened img10.png" || fail "ms-photos: '$(field file "$D3")'"

[ $RC = 0 ] && echo "photos-check: all passed" || echo "photos-check: FAILED"
exit $RC
