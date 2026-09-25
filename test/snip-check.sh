#!/bin/sh
# Gate for Snipping Tool (sg-snip). A window of four known colours
# (test/sg-snip-target.c) sits on a shell desktop; `snippingtool.exe /clip`
# (through App Paths, as explorer's Win+Shift+S runs it) freezes the screen and
# xdotool snips with the real X pointer and keyboard. A probe
# (test/sg-snip-probe.c) reads the clipboard back, and python checks pixels:
#
#   - Escape cancels: the clipboard keeps what it had
#   - a rectangle dragged over the window: CF_DIB of exactly the dragged size,
#     every quadrant's colour where it belongs (undimmed), PNG and CF_BITMAP too
#   - the toast opens the editor; a pen stroke; crop, undo; Ctrl+S writes a PNG
#     with the snip's pixels and the stroke, in the Save As dialog
#   - full screen mode, window mode (exactly the window), free-form mode (the
#     shape's box, white outside it)
#   - the Snipping Tool window: New, a snip into the editor
#   - the ms-screenclip: URI starts a screen clip
#
# Screenshots: build/snip-*.png. Needs wine-sg, Xvfb, xdotool, ImageMagick,
# python3 with PIL and mingw; skips (77) without them. SG_WINE_DIR, SG_SNIP_EXE
# (another build, for mutation tests), SG_SNIP_DPY (the X display, 112).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_SNIP_EXE:-$HERE/build/sg-snip64.exe}"
OUT="$HERE/build"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
RC=0; DPY="${SG_SNIP_DPY:-112}"; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import python3 "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
python3 -c 'import PIL' 2>/dev/null || { echo "SKIP: python3 PIL missing"; exit 77; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-snip-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

"$MINGW" -O2 -municode -o "$T/probe.exe" "$HERE/test/sg-snip-probe.c" -lgdi32 -luser32 || { fail "probe did not build"; exit 1; }
"$MINGW" -O2 -municode -mwindows -o "$T/target.exe" "$HERE/test/sg-snip-target.c" -lgdi32 -luser32 -lshell32 || { fail "target did not build"; exit 1; }

Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
cp "$T/probe.exe" "$T/target.exe" "$WINEPREFIX/drive_c/"
cp "$EXE" "$WINEPREFIX/drive_c/sg-snip64.exe"
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
# App Paths and the URI as defaults/72-sg-snip.reg has them, pointing at this build.
sed 's/Z:\\\\usr\\\\libexec\\\\stained-glass\\\\shell\\\\sg-snip64.exe/C:\\\\sg-snip64.exe/g' \
    "$HERE/defaults/72-sg-snip.reg" > "$WINEPREFIX/drive_c/snip.reg"
grep -q 'C:\\\\sg-snip64.exe' "$WINEPREFIX/drive_c/snip.reg" || fail "the defaults file's path was not rewritten"
wine reg import 'C:\snip.reg' >/dev/null 2>&1 || fail "defaults/72-sg-snip.reg does not import"
wineserver -w

DUMP="$T/dump"
export SG_SNIP_DUMP
SG_SNIP_DUMP=$(wine winepath -w "$DUMP" 2>/dev/null | tr -d '\r')
P() { wine 'C:\probe.exe' "$@" 2>/dev/null | tr -d '\r'; }
D() { { tr -d '\r' < "$DUMP"; } 2>/dev/null | sed -n "s/^$1=//p"; }
wait_state() {
    i=0
    while [ "$(D state)" != "$1" ] && [ $i -lt $(( ${2:-20} * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    [ "$(D state)" = "$1" ]
}
shot() { import -window root "$OUT/snip-$1.png" 2>/dev/null; }
centre() { echo "$1" | awk '{ printf "%d %d", ($1 + $3) / 2, ($2 + $4) / 2 }'; }
click() { [ $# -ge 2 ] || { fail "nothing to click (the dump lacks it)"; return; }; xdotool mousemove "$1" "$2"; sleep 0.2; xdotool click 1; }
clip() { rm -f "$DUMP"; wine start "$@" >/dev/null 2>&1; }
# pixel checks on a BMP/PNG: python3 check.py FILE W H "x,y=r,g,b" ...
cat > "$T/check.py" <<'EOF'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w, h = int(sys.argv[2]), int(sys.argv[3])
bad = []
if im.size != (w, h): bad.append("size %dx%d, not %dx%d" % (im.size + (w, h)))
else:
    for spec in sys.argv[4:]:
        xy, rgb = spec.split("=")
        x, y = map(int, xy.split(",")); want = tuple(map(int, rgb.split(",")))
        got = im.getpixel((x, y))
        if max(abs(a - b) for a, b in zip(got, want)) > 3: bad.append("(%d,%d) is %s, not %s" % (x, y, got, want))
print("; ".join(bad) if bad else "OK")
EOF
RED=220,20,60; GREEN=30,160,40; BLUE=20,60,220; YELLOW=250,200,0; WHITE=255,255,255
bmp() { P dib 'C:\clip.bmp'; }
check() { f=$1; shift; python3 "$T/check.py" "$f" "$@"; }
CLIPBMP="$WINEPREFIX/drive_c/clip.bmp"

WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
# the target: client area (200,150)-(600,450), quadrants split at (400,300)
wine 'C:\target.exe' 200 150 400 300 >/dev/null 2>&1 &
i=0; while ! P windows | grep -q SgSnipTarget && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
sleep 1

# --- Escape cancels ------------------------------------------------------------------------------
P settext before-the-snip
clip snippingtool.exe /clip
if wait_state overlay; then pass "snippingtool.exe /clip (App Paths) opens the screen clip"; else fail "no overlay: $(cat "$DUMP" 2>/dev/null)"; fi
sleep 0.5; shot overlay
python3 - "$OUT/snip-overlay.png" <<'EOF' && pass "the frozen screen is shown dimmed" || fail "the overlay is not the dimmed screen"
import sys
from PIL import Image
p = Image.open(sys.argv[1]).convert("RGB").getpixel((250, 200))
sys.exit(0 if abs(p[0] - 110) <= 4 and abs(p[1] - 10) <= 4 and abs(p[2] - 30) <= 4 else 1)
EOF
xdotool key Escape
if wait_state cancelled 10; then pass "Escape cancels"; else fail "Escape did not cancel: $(D state)"; fi
sleep 0.5
[ "$(P text)" = before-the-snip ] && pass "the clipboard is unchanged after Escape" || fail "the clipboard changed after Escape: $(P text)"
[ "$(bmp)" = NODIB ] && pass "no picture on the clipboard after Escape" || fail "a picture reached the clipboard after Escape"

# --- a rectangle ---------------------------------------------------------------------------------
clip snippingtool.exe /clip
wait_state overlay || fail "no overlay for the rectangle snip"
sleep 0.5
xdotool mousemove 250 180; sleep 0.2; xdotool mousedown 1; sleep 0.2
xdotool mousemove 400 300; sleep 0.2; xdotool mousemove 550 420; sleep 0.3
shot dragging
xdotool mouseup 1
if wait_state toast 10; then pass "the rectangle snip ends in the toast"; else fail "no toast after the rectangle: $(D state)"; fi
sz=$(bmp)
[ "$sz" = "300 240" ] && pass "CF_DIB is the dragged size (300x240)" || fail "CF_DIB is '$sz', not 300 240"
r=$(check "$CLIPBMP" 300 240 0,0=$RED 149,0=$RED 150,0=$GREEN 299,0=$GREEN 0,119=$RED 0,120=$BLUE 299,239=$YELLOW 149,239=$BLUE 150,239=$YELLOW)
[ "$r" = OK ] && pass "the snip has every quadrant where it belongs, undimmed" || fail "snip pixels: $r"
fmts=$(P formats | tr '\n' ' ')
case " $fmts " in *" PNG "*) pass "PNG on the clipboard" ;; *) fail "no PNG format: $fmts" ;; esac
case " $fmts " in *" 2 "*) pass "CF_BITMAP on the clipboard" ;; *) fail "no CF_BITMAP: $fmts" ;; esac
sleep 0.5; shot toast
toast=$(D toast)
[ -n "$toast" ] && pass "the toast is shown ($toast)" || fail "no toast rectangle"
# above the taskbar, at the bottom right
echo "$toast" | awk '{ exit !($3 > 900 && $4 <= 728) }' && pass "the toast sits above the taskbar at the right" || fail "the toast is at $toast"

# --- the editor ----------------------------------------------------------------------------------
# shellcheck disable=SC2046
click $(echo "$toast" | awk '{ printf "%d %d", $1 + 60, ($2 + $4) / 2 }')
if wait_state editor 10; then pass "the toast opens the editor"; else fail "the toast did not open the editor: $(D state)"; fi
sleep 1
[ "$(D picture)" = "300 240" ] && pass "the editor shows the 300x240 snip" || fail "editor picture: $(D picture)"
case "$(P foreground)" in SgSnippingTool*) pass "the editor is in front" ;; *) fail "in front: $(P foreground)" ;; esac
cv=$(D canvas); cx=$(echo "$cv" | cut -d' ' -f1); cy=$(echo "$cv" | cut -d' ' -f2)
[ "$(D scale)" = 1.0000 ] || fail "the picture is scaled: $(D scale)"
# a pen stroke (the pen is the default tool) across the red quadrant, row 60
xdotool mousemove $((cx + 20)) $((cy + 60)); sleep 0.2; xdotool mousedown 1; sleep 0.1
xdotool mousemove $((cx + 70)) $((cy + 60)); sleep 0.1; xdotool mousemove $((cx + 120)) $((cy + 60)); sleep 0.2
xdotool mouseup 1; sleep 0.5
[ "$(D strokes)" = 1 ] && pass "a pen stroke is drawn" || fail "strokes: $(D strokes)"
shot editor
# crop: drag the bottom-right handle 100 left and 80 up, Enter
# shellcheck disable=SC2046
click $(centre "$(D button_crop)")
sleep 0.5
[ "$(D cropping)" = 1 ] && pass "crop mode" || fail "crop mode did not start"
xdotool mousemove $((cx + 300)) $((cy + 240)); sleep 0.2; xdotool mousedown 1; sleep 0.1
xdotool mousemove $((cx + 250)) $((cy + 200)); sleep 0.1; xdotool mousemove $((cx + 200)) $((cy + 160)); sleep 0.2
xdotool mouseup 1; sleep 0.3
shot crop
xdotool key Return; sleep 0.8
[ "$(D picture)" = "200 160" ] && pass "crop to 200x160" || fail "after crop: $(D picture)"
xdotool key ctrl+z; sleep 0.8
[ "$(D picture)" = "300 240" ] && [ "$(D strokes)" = 1 ] && pass "undo brings the uncropped picture and its stroke back" \
    || fail "after undo: $(D picture), $(D strokes) strokes"
# Save As, by keyboard, into C:\snip-saved.png
xdotool key ctrl+s; sleep 2
shot save-dialog
xdotool key ctrl+a; xdotool type --delay 40 'c:\snip-saved.png'; sleep 0.3; xdotool key Return
i=0; while [ -z "$(D saved)" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
SAVED="$WINEPREFIX/drive_c/snip-saved.png"
if [ -f "$SAVED" ]; then
    pass "Ctrl+S saved $(D saved)"
    file "$SAVED" | grep -q 'PNG image data, 300 x 240' && pass "it is a 300x240 PNG" || fail "saved file: $(file "$SAVED")"
    r=$(check "$SAVED" 300 240 0,0=$RED 299,0=$GREEN 0,239=$BLUE 299,239=$YELLOW 70,60=0,0,0 70,80=$RED)
    [ "$r" = OK ] && pass "the saved PNG has the snip's pixels and the pen stroke" || fail "saved pixels: $r"
else fail "nothing saved (dump: $(D saved))"; fi
[ -d "$WINEPREFIX/drive_c/users/$USER/Pictures/Screenshots" ] && pass "Pictures\\Screenshots is the default folder" \
    || fail "no Pictures\\Screenshots folder"
case "$(D window_title)" in "snip-saved.png - Snipping Tool") pass "the title names the file" ;; *) fail "title: $(D window_title)" ;; esac
P close SgSnippingTool
i=0; while P windows | grep -q SgSnippingTool && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done

# --- full screen ---------------------------------------------------------------------------------
clip snippingtool.exe /clip
wait_state overlay || fail "no overlay for full screen"
sleep 0.3
# shellcheck disable=SC2046
click $(centre "$(D overlay_button_full)")
wait_state toast 10 || fail "full screen did not snip: $(D state)"
sz=$(bmp)
[ "$sz" = "1024 768" ] && pass "full screen is 1024x768" || fail "full screen snip is '$sz'"
r=$(check "$CLIPBMP" 1024 768 250,200=$RED 550,200=$GREEN 250,400=$BLUE 550,400=$YELLOW)
[ "$r" = OK ] && pass "full screen has the window, undimmed, with no trace of the overlay" || fail "full screen pixels: $r"
# the toast's close button ends it
# shellcheck disable=SC2046
click $(D toast | awk '{ printf "%d %d", $3 - 20, $2 + 18 }')
wait_state done 10 && pass "the toast's close button dismisses it" || fail "toast close: $(D state)"
sleep 0.5

# --- window --------------------------------------------------------------------------------------
clip snippingtool.exe /clip
wait_state overlay || fail "no overlay for window mode"
sleep 0.3
# shellcheck disable=SC2046
click $(centre "$(D overlay_button_window)")
sleep 0.3; xdotool mousemove 300 250; sleep 0.2; xdotool mousemove 310 260; sleep 0.5
shot window-mode
xdotool click 1
wait_state toast 10 || fail "window mode did not snip: $(D state)"
sz=$(bmp)
[ "$sz" = "400 300" ] && pass "window mode snips exactly the window (400x300)" || fail "window snip is '$sz'"
r=$(check "$CLIPBMP" 400 300 0,0=$RED 399,0=$GREEN 0,299=$BLUE 399,299=$YELLOW)
[ "$r" = OK ] && pass "the window snip's pixels" || fail "window pixels: $r"
# shellcheck disable=SC2046
click $(D toast | awk '{ printf "%d %d", $3 - 20, $2 + 18 }'); wait_state done 10; sleep 0.5

# --- free-form -----------------------------------------------------------------------------------
clip snippingtool.exe /clip
wait_state overlay || fail "no overlay for free-form"
sleep 0.3
# shellcheck disable=SC2046
click $(centre "$(D overlay_button_free)")
sleep 0.3
xdotool mousemove 300 160; sleep 0.2; xdotool mousedown 1; sleep 0.1
for pt in "340 195" "380 230" "340 265" "300 300" "260 265" "220 230" "260 195" "300 161"; do
    # shellcheck disable=SC2086
    xdotool mousemove $pt; sleep 0.08
done
shot freeform
xdotool mouseup 1
wait_state toast 10 || fail "free-form did not snip: $(D state)"
sz=$(bmp)
case "$sz" in "16"[01]" 14"[01]) pass "free-form is the shape's box ($sz)" ;; *) fail "free-form snip is '$sz'" ;; esac
w=${sz% *}; h=${sz#* }
r=$(check "$CLIPBMP" "$w" "$h" 0,0=$WHITE $((w - 1)),0=$WHITE 0,$((h - 1))=$WHITE $((w / 2)),$((h / 2))=$RED)
[ "$r" = OK ] && pass "free-form: the shape keeps its pixels, outside it is white" || fail "free-form pixels: $r"
# shellcheck disable=SC2046
click $(D toast | awk '{ printf "%d %d", $3 - 20, $2 + 18 }'); wait_state done 10; sleep 0.5

# --- the Snipping Tool window --------------------------------------------------------------------
rm -f "$DUMP"; wine start snippingtool.exe >/dev/null 2>&1
if wait_state tool; then pass "snippingtool.exe opens the Snipping Tool window"; else fail "no tool window: $(D state)"; fi
sleep 1; shot tool
case "$(D window_title)" in "Snipping Tool") pass "titled Snipping Tool" ;; *) fail "title: $(D window_title)" ;; esac
# shellcheck disable=SC2046
click $(centre "$(D button_new)")
wait_state overlay 10 && pass "New starts a snip" || fail "New: $(D state)"
sleep 0.3
xdotool mousemove 420 320; sleep 0.2; xdotool mousedown 1; sleep 0.1; xdotool mousemove 500 380; sleep 0.2; xdotool mouseup 1
if wait_state editor 10; then pass "a snip from New opens in the editor"; else fail "New's snip: $(D state)"; fi
[ "$(D picture)" = "80 60" ] && [ "$(bmp)" = "80 60" ] && pass "and is on the clipboard (80x60)" || fail "New's snip: $(D picture) / $(bmp)"
r=$(check "$CLIPBMP" 80 60 0,0=$YELLOW 79,59=$YELLOW)
[ "$r" = OK ] && pass "New's snip pixels" || fail "New's snip pixels: $r"
sleep 0.5; shot editor-new
P close SgSnippingTool
i=0; while P windows | grep -q SgSnippingTool && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done

# --- the URI -------------------------------------------------------------------------------------
clip ms-screenclip:
if wait_state overlay; then pass "ms-screenclip: starts a screen clip"; else fail "ms-screenclip: did nothing: $(D state)"; fi
xdotool key Escape; wait_state cancelled 10

[ $RC = 0 ] && echo "snip-check: all passed" || echo "snip-check: FAILED"
exit $RC
