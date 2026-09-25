#!/bin/sh
# Gate for Magnifier (sg-magnify, magnify.exe): on a shell desktop with a
# window of coloured 8-pixel blocks (test/sg-a11y-probe.c "pattern"), every
# view must show exactly the screen's pixels magnified -- each sampled pixel of
# the view is the reference screenshot's pixel at SOURCE + offset / zoom (from
# the dump) -- and the lens and full-screen views must never show themselves
# (they cover what they magnify; copying the screen would feed back).
#
#   - Win+Plus on the X keyboard starts it (full screen, 200%), Win+Plus
#     zooms to 300%, Win+Minus back, Ctrl+Alt+wheel both ways, the toolbar's
#     + button, Win+Esc closes it -- through explorer (wine-sg 0181/0182);
#     on a Wine without them the program is started directly and the keys
#     are skipped (the gate says so)
#   - full screen: pixels at 200% and 300%, the point under the pointer shown
#     under the pointer, and a click passing through to the window below
#   - lens (Ctrl+Alt+L): pixels, twice a second apart (no feedback)
#   - docked (Ctrl+Alt+D): pixels, and the work area giving it the top of the
#     screen (an AppBar; wine-sg 0182), given back when it closes
#   - Ctrl+Alt+I inverts colours; settings kept in
#     HKCU\Software\Microsoft\ScreenMagnifier
#
# Screenshots: build/magnify-*.png. SG_MAGNIFY_EXE runs another build (the
# mutants: -DSG_MUTANT_SCALE, -DSG_MUTANT_FEEDBACK); SG_WINE_DIR another Wine
# (a build tree works). Display :171. Needs Xvfb, xdotool, ImageMagick,
# python3 with PIL and mingw; skips (77) without them.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_MAGNIFY_EXE:-$HERE/build/sg-magnify64.exe}"
OUT="$HERE/build"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
RC=0; DPY="${SG_MAGNIFY_DPY:-171}"; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import python3 "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
python3 -c 'import PIL' 2>/dev/null || { echo "SKIP: python3 PIL missing"; exit 77; }
if [ -x "$WINE_DIR/bin/wine" ]; then WBIN="$WINE_DIR/bin"; WSERVER="$WINE_DIR/bin/wineserver"
elif [ -x "$WINE_DIR/wine" ]; then WBIN="$WINE_DIR"; WSERVER="$WINE_DIR/server/wineserver"
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi
[ -f "$EXE" ] || { echo "SKIP: $EXE missing (make build)"; exit 77; }

T=$(mktemp -d /var/tmp/sg-magnify-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WSERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

"$MINGW" -O2 -municode -o "$T/probe.exe" "$HERE/test/sg-a11y-probe.c" -luser32 -lgdi32 || { fail "probe did not build"; exit 1; }

cat > "$T/magcmp.py" <<'EOF'
# magcmp.py REF MAG DUMP [invert]: sampled view pixels of MAG against REF's
# pixel at SOURCE + offset * 100 / ZOOM; the toolbar and the drawn pointer
# are left out. Prints "MATCH ok n colours k".
import sys
from PIL import Image
ref = Image.open(sys.argv[1]).convert("RGB"); mag = Image.open(sys.argv[2]).convert("RGB")
d = {}
for line in open(sys.argv[3], encoding="utf-8", errors="replace"):
    p = line.split()
    if p: d[p[0]] = p[1:]
inv = len(sys.argv) > 4 and sys.argv[4] == "invert"
zoom = int(d["ZOOM"][0])
vl, vt, vr, vb = map(int, d["VIEW"]); sl, st, sr, sb = map(int, d["SOURCE"])
tl, tt, tr, tb = map(int, d.get("TOOLBAR", ["0", "0", "0", "0"]))
cx, cy = map(int, d["CURSOR"])
dcx = vl + (cx - sl) * zoom // 100; dcy = vt + (cy - st) * zoom // 100
ok = n = 0; cols = set()
for y in range(vt + 4, vb - 4, 5):
    for x in range(vl + 4, vr - 4, 5):
        if tl - 2 <= x < tr + 2 and tt - 2 <= y < tb + 2: continue
        if abs(x - dcx) < 40 * zoom // 100 + 8 and abs(y - dcy) < 40 * zoom // 100 + 8: continue
        sx = sl + (x - vl) * 100 // zoom; sy = st + (y - vt) * 100 // zoom
        if sx >= sr or sy >= sb: continue
        e = ref.getpixel((sx, sy))
        if inv: e = tuple(255 - c for c in e)
        n += 1; cols.add(e)
        if mag.getpixel((x, y)) == e: ok += 1
print("MATCH %d %d colours %d" % (ok, n, len(cols)))
EOF

# --- Wine, a shell desktop and the pattern ----------------------------------------------------------
Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WBIN:$(dirname "$WSERVER"):$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
cp "$T/probe.exe" "$WINEPREFIX/drive_c/probe.exe"
cp "$EXE" "$WINEPREFIX/drive_c/sg-magnify64.exe"
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
# App Paths as defaults/80-sg-magnify.reg has it, pointing at this build.
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\magnify.exe' /ve /d 'C:\sg-magnify64.exe'
wineserver -w
P() { wine 'C:\probe.exe' "$@" 2>/dev/null | tr -d '\r'; }
DUMP="$WINEPREFIX/drive_c/magnify.txt"
export SG_MAGNIFY_DUMP='C:\magnify.txt'
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
wine 'C:\probe.exe' pattern 100 200 400 300 >/dev/null 2>&1 &
i=0; while [ "$(P title SgPattern)" != "Pattern 0" ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
sleep 1
import -window root "$T/ref.png"

val() { sed -n "s/^$1 //p" "$DUMP" 2>/dev/null | tr -d '\r' | head -1; }
wait_val() { # KEY VALUE SECONDS
    i=0; while [ "$(val "$1")" != "$2" ] && [ $i -lt $(( $3 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    [ "$(val "$1")" = "$2" ]
}
# compare NAME [invert]: screenshot and check the view against the reference
compare() {
    sleep 1.5
    import -window root "$OUT/magnify-$1.png"
    set -- $(python3 "$T/magcmp.py" "$T/ref.png" "$OUT/magnify-$1.png" "$DUMP" "${2:-}" 2>/dev/null) - 0 0 - 0
    M_OK=$2; M_N=$3; M_COLS=$5
    [ "$M_N" -gt 200 ] && [ $((M_OK * 1000)) -ge $((M_N * 995)) ] && [ "$M_COLS" -ge 30 ]
}

HAVE_KEYS=0
[ -f "$WINEPREFIX/drive_c/windows/system32/magnify.exe" ] && HAVE_KEYS=1
[ "${SG_MAGNIFY_KEYS:-}" = 0 ] && HAVE_KEYS=0

# --- starting: Win+Plus --------------------------------------------------------------------------
xdotool mousemove 250 330
if [ $HAVE_KEYS = 1 ]; then
    [ -f "$WINEPREFIX/drive_c/windows/syswow64/magnify.exe" ] && pass "magnify.exe in system32 and syswow64 (0181)" \
        || fail "magnify.exe missing from syswow64"
    xdotool key super+equal
else
    echo "NOTE  this Wine has no magnify.exe (wine-sg 0181/0182): started directly, Windows-key checks skipped"
    wine 'C:\sg-magnify64.exe' >/dev/null 2>&1 &
fi
if wait_val MODE full 20 && [ "$(val ZOOM)" = 200 ]; then pass "Magnifier started full screen at 200%"
else fail "Magnifier did not start: $(head -3 "$DUMP" 2>/dev/null | tr '\n' ' ')"; fi
[ $(( 0x$(val VIEWSTYLE) & 0x20 )) -ne 0 ] && [ $(( 0x$(val VIEWSTYLE) & 0x80000 )) -ne 0 ] \
    && pass "the full-screen view is click-through (layered, transparent)" || fail "view styles $(val VIEWSTYLE)"

xdotool mousemove 251 330; sleep 0.3; xdotool mousemove 250 330
if compare full200; then pass "full screen at 200% shows the screen magnified ($M_OK/$M_N, $M_COLS colours)"
else fail "full screen at 200%: $M_OK/$M_N match"; fi
# the point under the pointer is shown under the pointer: origin = p (1 - 1/zoom)
set -- $(val SOURCE) - -
[ "$1" = 125 ] && [ "$2" = 165 ] && pass "the view keeps the pointer's point under the pointer (source at 125,165)" \
    || fail "source origin $1,$2, expected 125,165"
xdotool click 1; sleep 1
[ "$(P title SgPattern)" = "Pattern 1" ] && pass "a click goes through the view to the window under it" \
    || fail "the click did not reach the window: $(P title SgPattern)"

if [ $HAVE_KEYS = 1 ]; then
    xdotool key super+equal
    wait_val ZOOM 300 5 && pass "Win+Plus zooms in to 300%" || fail "Win+Plus: zoom $(val ZOOM)"
else
    xdotool key ctrl+alt+equal; wait_val ZOOM 300 5 || fail "Ctrl+Alt+Plus: zoom $(val ZOOM)"
fi
if compare full300; then pass "full screen at 300% ($M_OK/$M_N)"; else fail "full screen at 300%: $M_OK/$M_N"; fi
if [ $HAVE_KEYS = 1 ]; then
    xdotool key super+minus
    wait_val ZOOM 200 5 && pass "Win+Minus zooms out to 200%" || fail "Win+Minus: zoom $(val ZOOM)"
else
    xdotool key ctrl+alt+minus; wait_val ZOOM 200 5 || fail "Ctrl+Alt+Minus: zoom $(val ZOOM)"
fi
xdotool keydown ctrl alt; xdotool click 4; xdotool keyup alt ctrl
wait_val ZOOM 300 5 && pass "Ctrl+Alt+wheel up zooms in" || fail "Ctrl+Alt+wheel up: zoom $(val ZOOM)"
xdotool keydown ctrl alt; xdotool click 5; xdotool keyup alt ctrl
wait_val ZOOM 200 5 && pass "Ctrl+Alt+wheel down zooms out" || fail "Ctrl+Alt+wheel down: zoom $(val ZOOM)"

# --- lens -------------------------------------------------------------------------------------------
xdotool key ctrl+alt+l
xdotool mousemove 300 350
if wait_val MODE lens 5 && compare lens; then pass "lens (Ctrl+Alt+L) shows the screen under it magnified ($M_OK/$M_N)"
else fail "lens: mode $(val MODE), $M_OK/$M_N"; fi
sleep 1
if compare lens-later; then pass "the lens never shows itself: the same pixels a second later ($M_OK/$M_N)"
else fail "the lens fed back: $M_OK/$M_N a second later"; fi

# --- docked ----------------------------------------------------------------------------------------------
xdotool key ctrl+alt+d
xdotool mousemove 300 400
if wait_val MODE docked 5 && compare docked; then pass "docked (Ctrl+Alt+D) shows the area round the pointer ($M_OK/$M_N)"
else fail "docked: mode $(val MODE), $M_OK/$M_N"; fi
set -- $(val VIEW) - - - -; DOCK_B=$4
if [ $HAVE_KEYS = 1 ]; then
    [ "$(P workarea)" = "0 $DOCK_B 1024 728" ] && pass "the dock takes the top of the work area (0 $DOCK_B 1024 728)" \
        || fail "work area $(P workarea), dock bottom $DOCK_B"
fi

# --- the toolbar's + and invert ------------------------------------------------------------------
set -- $(sed -n 's/^BUTTON plus //p' "$DUMP" | tr -d '\r') 0 0
xdotool mousemove "$1" "$2" click 1
wait_val ZOOM 300 5 && pass "the toolbar's + button zooms in" || fail "toolbar +: zoom $(val ZOOM)"
xdotool mousemove 300 400
xdotool key ctrl+alt+i
if wait_val INVERT 1 5 && compare inverted invert; then pass "Ctrl+Alt+I inverts the colours ($M_OK/$M_N)"
else fail "invert: $(val INVERT), $M_OK/$M_N"; fi
xdotool key ctrl+alt+i; wait_val INVERT 0 5 || true
Q=$(wine reg query 'HKCU\Software\Microsoft\ScreenMagnifier' /v Magnification 2>/dev/null | tr -d '\r' | awk '/Magnification/ {print $3}')
[ "$Q" = 0x12c ] && pass "the zoom is kept in HKCU\\...\\ScreenMagnifier (Magnification 300)" || fail "Magnification value $Q"

# --- Win+Esc ---------------------------------------------------------------------------------------------
if [ $HAVE_KEYS = 1 ]; then
    xdotool key super+Escape
    i=0; while [ "$(head -1 "$DUMP" | tr -d '\r')" != CLOSED ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
    [ "$(head -1 "$DUMP" | tr -d '\r')" = CLOSED ] && pass "Win+Esc closes Magnifier" || fail "Win+Esc did not close it"
    sleep 1
    [ "$(P workarea)" = "0 0 1024 728" ] && pass "the work area is given back" || fail "work area after closing: $(P workarea)"
fi
import -window root "$OUT/magnify-closed.png"

[ $RC = 0 ] && echo "magnify-check: PASS" || echo "magnify-check: FAIL"
exit $RC
