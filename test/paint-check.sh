#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for Paint (sg-paint, mspaint.exe): on a shell desktop under Xvfb,
# `mspaint.exe` (found through App Paths, as ShellExecute and the Run box
# find it) opens "Untitled - Paint"; the gate draws with the X mouse -- a
# pencil line, a rectangle, a fill inside it, a line, a scribble taken back
# with Ctrl+Z -- saves with Ctrl+S through the Save As dialog, and reads the
# PNG's pixels. A PNG given on the command line opens, rotates (Ctrl+R) and
# saves back over itself; Select all + Copy there and Paste in the first
# window moves pixels through the clipboard (CF_DIB); closing with unsaved
# changes asks, and "No" leaves the file alone.
#
# The ribbon's and canvas's screen rectangles come from SG_PAINT_DUMP.
# Screenshots: build/paint-*.png. Needs wine-sg, Xvfb, xdotool, ImageMagick
# and python3 with PIL; skips (77) without them. SG_PAINT_EXE tests another
# build (the mutation test).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_PAINT_EXE:-$HERE/build/sg-paint64.exe}"
OUT="$HERE/build"
RC=0; DPY=111; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
for need in Xvfb xdotool import python3 "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
python3 -c 'import PIL' 2>/dev/null || { echo "SKIP: python3-pil missing"; exit 77; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-paint-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
mkdir -p "$T/home"
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all HOME="$T/home"
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
cat > "$T/probe.c" <<'EOF'
#include <windows.h>
#include <stdio.h>
int wmain(int argc, WCHAR **argv)
{
    if (argc >= 3 && !wcscmp(argv[1], L"activate"))
    {
        HWND w = FindWindowW(NULL, argv[2]);
        if (!w) { puts("NOWINDOW"); return 1; }
        SetForegroundWindow(w);
        puts("OK");
        return 0;
    }
    return 2;
}
EOF
"$MINGW" -O2 -municode -o "$WINEPREFIX/drive_c/probe.exe" "$T/probe.c" -luser32 || { echo "FAIL  probe did not build"; exit 1; }
for r in "$HERE"/theme/5*.reg; do wine regedit /S "$(wine winepath -w "$r" 2>/dev/null | tr -d '\r')" >/dev/null 2>&1; done
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
# App Paths as defaults/71-sg-paint.reg has it, pointing at this build.
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\mspaint.exe' /ve /d "$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')"
wineserver -w
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

shot() { import -window root "$OUT/paint-$1.png" 2>/dev/null; }
D1="$T/dump1"; D2="$T/dump2"
# value of key $2 in dump $1
dv() { sed -n "s/^$2 //p" "$1" 2>/dev/null | head -1 | tr -d '\r'; }
wait_dump() {       # dump key value [seconds]
    i=0; while [ "$(dv "$1" "$2")" != "$3" ] && [ $i -lt $(( ${4:-15} * 2 )) ]; do sleep 0.5; i=$((i + 1)); done
    [ "$(dv "$1" "$2")" = "$3" ]
}
# the centre of the ribbon item for command $2
item() { awk -v c="$2" '$1 == "item" && $2 == c { printf "%d %d\n", ($3 + $5) / 2, ($4 + $6) / 2; exit }' "$1"; }
click_item() { set -- $(item "$1" "$2"); [ $# -eq 2 ] || return 1; xdotool mousemove "$1" "$2" click 1; sleep 0.4; }
click_label() {
    set -- $(awk -v l="$2" '$1 == "item" { n = $7; for (i = 8; i <= NF; i++) n = n " " $i; if (n == l) { printf "%d %d\n", ($3 + $5) / 2, ($4 + $6) / 2; exit } }' "$1")
    [ $# -eq 2 ] || return 1; xdotool mousemove "$1" "$2" click 1; sleep 0.4; }
# bring a Paint window to the front by its title (a probe: there is no X
# window manager here for xdotool to ask)
front() { wine 'C:\probe.exe' activate "$2 - Paint" >/dev/null 2>&1; sleep 0.8; }
# screen position of picture pixel x,y (100% zoom)
at() { awk -v x="$2" -v y="$3" '$1 == "canvas" { printf "%d %d\n", $2 + x, $3 + y; exit }' "$1"; }
drag() {           # dump x1 y1 x2 y2 [button]
    set -- "$1" $(at "$1" "$2" "$3") $(at "$1" "$4" "$5") "${6:-1}"
    xdotool mousemove "$2" "$3" sleep 0.2 mousedown "$6" sleep 0.2 mousemove $(( ($2 + $4) / 2 )) $(( ($3 + $5) / 2 )) \
        sleep 0.2 mousemove "$4" "$5" sleep 0.2 mouseup "$6"
    sleep 0.5
}
clickat() { set -- $(at "$1" "$2" "$3"); xdotool mousemove "$1" "$2" click 1; sleep 0.5; }
# palette cells: 0 black, 3 red, 6 green, 7 turquoise, 9 purple
PAL=400; T_PENCIL=302; T_FILL=303; S_LINE=340; S_RECT=341

# --- 1. mspaint.exe through App Paths -------------------------------------------------------
SG_PAINT_DUMP="$D1" wine start mspaint.exe >/dev/null 2>&1
if wait_dump "$D1" title "Untitled - Paint" 30; then pass "mspaint.exe (App Paths) opens \"Untitled - Paint\""
else fail "no Paint window: $(dv "$D1" title)"; shot fail-start; exit 1; fi
sleep 2
[ "$(dv "$D1" tool)" = 2 ] && pass "the pencil is the tool at start" || fail "tool at start is $(dv "$D1" tool)"
[ "$(dv "$D1" image | awk '{print ($1 >= 320 && $2 >= 240)}')" = 1 ] && pass "a blank picture: $(dv "$D1" image)" || fail "picture size $(dv "$D1" image)"
shot start

# --- 2. drawing -----------------------------------------------------------------------------
click_item "$D1" $((PAL + 3)); click_item "$D1" $T_PENCIL
[ "$(dv "$D1" color1)" = 241ced ] && pass "palette: red is colour 1" || fail "colour 1 is $(dv "$D1" color1), wanted 241ced"
drag "$D1" 10 20 110 20
[ "$(dv "$D1" dirty)" = 1 ] && pass "drawing marks the picture changed" || fail "not dirty after drawing"
click_item "$D1" $PAL; click_item "$D1" $S_RECT
[ "$(dv "$D1" tool)" = 9 ] && [ "$(dv "$D1" shape)" = 1 ] && pass "the rectangle shape is chosen" || fail "shape tool $(dv "$D1" tool)/$(dv "$D1" shape)"
drag "$D1" 150 50 250 150
click_item "$D1" $((PAL + 7)); click_item "$D1" $T_FILL
clickat "$D1" 200 100
click_item "$D1" $((PAL + 6)); click_item "$D1" $S_LINE
drag "$D1" 10 200 200 200
# a scribble, taken back
click_item "$D1" $((PAL + 9)); click_item "$D1" $T_PENCIL
u0=$(dv "$D1" undo)
drag "$D1" 300 250 350 250
xdotool key ctrl+z; sleep 0.7
[ "$(dv "$D1" undo)" -lt "$(( u0 + 1 ))" ] 2>/dev/null && [ "$(dv "$D1" redo)" = 1 ] && pass "Ctrl+Z takes the last stroke back" \
    || fail "undo: $(dv "$D1" undo) steps, redo $(dv "$D1" redo) (before $u0)"
# text: a box dragged out, typed into, put down by a click outside it
click_item "$D1" $PAL; click_item "$D1" 304
drag "$D1" 300 380 470 430
[ "$(dv "$D1" text)" = 1 ] && pass "the text tool opens a text box" || fail "no text box: text=$(dv "$D1" text)"
xdotool type --delay 60 "hello"; sleep 0.5
shot text
clickat "$D1" 550 300
[ "$(dv "$D1" text)" = 0 ] && pass "a click outside puts the text down" || fail "text box still open"
# the brush: the big Brushes button's upper half
set -- $(item "$D1" 320); xdotool mousemove "$1" $(( $2 - 15 )) click 1; sleep 0.4
[ "$(dv "$D1" tool)" = 8 ] && pass "Brushes chooses the brush" || fail "brush tool: $(dv "$D1" tool)"
click_item "$D1" $((PAL + 3))
drag "$D1" 300 440 400 440
shot drawn

# --- 3. save as PNG through the dialog ------------------------------------------------------
xdotool key ctrl+s; sleep 2
shot saveas
DLG=$(xdotool search --name '^Save As$' 2>/dev/null | head -1)
[ -n "$DLG" ] && pass "Ctrl+S on an untitled picture asks where (Save As)" || fail "no Save As dialog"
xdotool type --delay 80 gate1; sleep 0.3; xdotool key Return
if wait_dump "$D1" title "gate1.png - Paint" 15; then pass "saved: the title is \"gate1.png - Paint\""
else fail "title after saving: $(dv "$D1" title)"; shot fail-save; fi
PNG=$(wine winepath -u "$(dv "$D1" path)" 2>/dev/null | tr -d '\r')
[ "$(dv "$D1" dirty)" = 0 ] && pass "saving clears the changed mark" || fail "still dirty after saving"
check_png() {   # file checks...: python asserts, prints FAIL lines
    python3 - "$@" <<'EOF'
import sys
from PIL import Image
path, checks = sys.argv[1], sys.argv[2:]
im = Image.open(path).convert("RGB")
print("SIZE %dx%d %s" % (im.width, im.height, im.format if hasattr(im, "format") else ""))
for c in checks:
    x, y, want, what = c.split(":", 3)
    got = "%02x%02x%02x" % im.getpixel((int(x), int(y)))
    print(("PASS  " if got == want else "FAIL  ") + "%s at %s,%s is %s%s" % (what, x, y, got, "" if got == want else " (wanted %s)" % want))
EOF
}
if [ -f "$PNG" ]; then
    [ "$(head -c 8 "$PNG" | od -An -tx1 | tr -d ' \n')" = 89504e470d0a1a0a ] && pass "the file is a PNG" || fail "not a PNG"
    check_png "$PNG" \
        "10:20:ed1c24:the pencil line's start" "60:20:ed1c24:the pencil line" "110:20:ed1c24:the pencil line's end" \
        "60:21:ffffff:below the 1px pencil line" "150:100:000000:the rectangle's left edge" "250:100:000000:the rectangle's right edge" \
        "200:50:000000:the rectangle's top edge" "200:100:00a2e8:the fill inside the rectangle" "160:140:00a2e8:the fill's corner" \
        "260:100:ffffff:outside the rectangle" "100:200:22b14c:the green line" "320:250:ffffff:where the undone scribble was" \
        "5:5:ffffff:the background" "350:440:ed1c24:the brush stroke" > "$T/png1.txt"
    cat "$T/png1.txt"; grep -q '^FAIL' "$T/png1.txt" && RC=1
    n=$(python3 -c '
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
print(sum(1 for x in range(300, 470) for y in range(380, 430) if sum(im.getpixel((x, y))) < 600))' "$PNG")
    [ "$n" -gt 30 ] && pass "the typed text is in the picture ($n dark pixels)" || fail "no text in the picture ($n dark pixels)"
    w=$(sed -n 's/^SIZE \([0-9]*\)x\([0-9]*\).*/\1 \2/p' "$T/png1.txt")
    [ "$w" = "$(dv "$D1" image)" ] && pass "the PNG is the picture's size ($w)" || fail "PNG size $w vs picture $(dv "$D1" image)"
else fail "no saved file at '$PNG'"; fi

# --- 4. a PNG from the command line: open, rotate, save back ---------------------------------
python3 -c '
from PIL import Image
im = Image.new("RGB", (64, 48), (255, 255, 255))
for x in range(64):
    for y in range(48):
        if x < 16: im.putpixel((x, y), (200, 0, 0))
        elif y < 8: im.putpixel((x, y), (0, 0, 200))
im.save("'"$T"'/in.png")'
SG_PAINT_DUMP="$D2" wine start mspaint.exe "$(wine winepath -w "$T/in.png" 2>/dev/null | tr -d '\r')" >/dev/null 2>&1
if wait_dump "$D2" title "in.png - Paint" 30; then pass "a file on the command line opens: \"in.png - Paint\""
else fail "command-line file: title $(dv "$D2" title)"; fi
[ "$(dv "$D2" image)" = "64 48" ] && pass "its size is 64 x 48" || fail "opened size $(dv "$D2" image)"
sleep 1
xdotool key ctrl+r; sleep 0.5
[ "$(dv "$D2" image)" = "48 64" ] && pass "Ctrl+R rotates it right" || fail "after rotating: $(dv "$D2" image)"
xdotool key ctrl+s; sleep 1.5
wait_dump "$D2" dirty 0 5 && pass "Ctrl+S saves an opened file in place, without asking" || fail "not saved in place"
shot opened
# rotated right: the left red band (x<16) becomes the top rows; the top blue band (y<8) the right-hand columns
check_png "$T/in.png" "20:5:c80000:red band on top after rotating right" "44:40:0000c8:blue band on the right" "10:40:ffffff:white below" > "$T/png2.txt"
cat "$T/png2.txt"; grep -q '^FAIL' "$T/png2.txt" && RC=1
grep -q '^SIZE 48x64' "$T/png2.txt" && pass "the saved file is 48 x 64" || fail "saved file $(head -1 "$T/png2.txt")"

# --- 5. the clipboard: Select all + Copy here, Paste in the first window --------------------
xdotool key ctrl+a; sleep 0.4; xdotool key ctrl+c; sleep 0.5
[ "$(dv "$D2" selection)" = "0 0 48 64" ] && pass "Ctrl+A selects the whole picture" || fail "selection $(dv "$D2" selection)"
front "$D1" gate1.png
xdotool key ctrl+v; sleep 0.7
[ "$(dv "$D1" selection)" = "0 0 48 64" ] && [ "$(dv "$D1" floating)" = 1 ] && pass "Ctrl+V pastes it as a floating selection" \
    || fail "after pasting: selection $(dv "$D1" selection) floating $(dv "$D1" floating)"
shot pasted
xdotool key Escape; sleep 0.3; xdotool key ctrl+s; sleep 1.5
wait_dump "$D1" dirty 0 5 || fail "second save did not finish"
check_png "$PNG" "20:5:c80000:pasted red" "44:40:0000c8:pasted blue" "10:40:ffffff:pasted white" "60:20:ed1c24:the old line beside the paste" > "$T/png3.txt"
cat "$T/png3.txt"; grep -q '^FAIL' "$T/png3.txt" && RC=1

# --- 5b. Resize and Skew (Ctrl+W): 50%, aspect ratio kept; Ctrl+Z puts it back ---------------
xdotool key ctrl+w; sleep 1.5
shot resize
xdotool key alt+h; sleep 0.3; xdotool key ctrl+a; xdotool type --delay 80 50; sleep 0.3; xdotool key Return; sleep 1
[ "$(dv "$D1" image)" = "307 230" ] && pass "Resize 50% halves the picture" || fail "after resizing: $(dv "$D1" image)"
xdotool key ctrl+z; sleep 0.7
[ "$(dv "$D1" image)" = "614 460" ] && pass "Ctrl+Z undoes the resize" || fail "after undoing the resize: $(dv "$D1" image)"

# --- 6. closing with unsaved changes asks ---------------------------------------------------
front "$D2" in.png
click_item "$D2" $T_PENCIL
drag "$D2" 5 30 40 30
sum=$(md5sum < "$T/in.png")
# the caption's close button (the window's top-right corner, from the dump)
set -- $(dv "$D2" window); xdotool mousemove $(( $3 - 18 )) $(( $2 + 18 )) click 1; sleep 1.5
shot close-prompt
MB=$(xdotool search --name '^Paint$' 2>/dev/null | head -1)
[ -n "$MB" ] && pass "closing with changes asks to save" || fail "no save prompt on close"
xdotool key n; sleep 1.5
xdotool search --name '^in.png - Paint$' >/dev/null 2>&1 && fail "Paint still open after Don't Save" || pass "\"No\" closes without saving"
[ "$(md5sum < "$T/in.png")" = "$sum" ] && pass "the file is untouched" || fail "the file changed"

# --- 7. the look: the ribbon paints, the View tab --------------------------------------------
front "$D1" gate1.png
colours=$(import -window root -crop 900x110+0+0 txt:- 2>/dev/null | awk 'NR>1{print $3}' | sort -u | wc -l)
[ "$colours" -gt 20 ] && pass "the ribbon is drawn ($colours colours)" || fail "the ribbon looks blank ($colours colours)"
click_label "$D1" View
[ "$(dv "$D1" tab)" = 1 ] && pass "the View tab opens" || fail "View tab: $(dv "$D1" tab)"
click_label "$D1" "Zoom in"
[ "$(dv "$D1" zoom)" = 2000 ] && pass "Zoom in goes to 200%" || fail "zoom $(dv "$D1" zoom)"
shot view

echo "screenshots: $OUT/paint-*.png"
[ $RC = 0 ] && echo "paint-check: all passed" || echo "paint-check: FAILED"
exit $RC
