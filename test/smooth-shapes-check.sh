#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for sg-smooth.h, the shell's soft-edged drawing: each drop-in shape
# (sg_ellipse, sg_round_rect, sg_polygon, sg_polyline, sg_arc, sg_line) and a
# glyph drawn in a region (sg_ss_begin/end) is drawn by test/smooth-shapes-
# probe.c beside GDI's own drawing of the same call, on a memory bitmap:
#
#   - each has many colours along its edges (GDI's: two or three)
#   - each lies where GDI's does (centre of ink within 1.5 px)
#   - a large rounded rectangle's level edge is exactly GDI's (corners only
#     are supersampled: straight edges stay crisp)
#
# Mutant: the probe built with -DSG_MUTANT_JAGGED (every helper plain GDI)
# fails it. Images: build/smooth-shapes.png (GDI left, smooth right, 4x).
# Needs wine-sg and mingw; skips (77) without them.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
command -v "$MINGW" >/dev/null || { echo "SKIP: $MINGW missing"; exit 77; }
if [ -x "$WINE_DIR/bin/wine" ]; then WINE="$WINE_DIR/bin/wine"; WSERVER="$WINE_DIR/bin/wineserver"
elif [ -x "$WINE_DIR/wine" ]; then WINE="$WINE_DIR/wine"; WSERVER="$WINE_DIR/server/wineserver"
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi

T=$(mktemp -d /var/tmp/sg-smooth-shapes.XXXXXX)
# shellcheck disable=SC2317
cleanup() { WINEPREFIX="$T/pfx" "$WSERVER" -k 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
# SG_SMOOTH_CFLAGS: -DSG_MUTANT_JAGGED for the mutant
# shellcheck disable=SC2086
"$MINGW" -O1 ${SG_SMOOTH_CFLAGS:-} -o "$T/probe.exe" "$HERE/test/smooth-shapes-probe.c" -lgdi32 || { fail "the probe did not build"; exit 1; }
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="winemenubuilder.exe=d;mscoree,mshtml="
unset DISPLAY WAYLAND_DISPLAY
"$WINE" "$T/probe.exe" 'C:\shapes.bmp' > "$T/out" 2>/dev/null
tr -d '\r' < "$T/out" > "$T/res"
[ -s "$T/res" ] || { fail "the probe reported nothing"; exit 1; }
for shape in ellipse round_rect polygon polyline arc line region; do
    l=$(grep "^SHAPE $shape " "$T/res")
    c=$(echo "$l" | sed -n 's/.*colours=\([0-9]*\).*/\1/p'); g=$(echo "$l" | sed -n 's/.*gdi=\([0-9]*\).*/\1/p')
    dx=$(echo "$l" | sed -n 's/.*dx=\([-0-9.]*\).*/\1/p'); dy=$(echo "$l" | sed -n 's/.*dy=\([-0-9.]*\).*/\1/p')
    if [ "${c:-0}" -ge 6 ] && [ "${g:-99}" -le 3 ] && awk -v x="${dx:-9}" -v y="${dy:-9}" 'BEGIN { exit !(x < 1.5 && x > -1.5 && y < 1.5 && y > -1.5) }'; then
        pass "$shape: smooth ($c colours, GDI's $g), in place (off by $dx, $dy px)"
    else
        fail "$shape: ${l:-no report} (want 6+ colours, within 1.5 px)"
    fi
done
grep -q '^CRISP 1' "$T/res" && pass "a large rounded rectangle's straight edge is GDI's own (only its corners are supersampled)" \
    || fail "the large rounded rectangle's edge differs from GDI's"
command -v convert >/dev/null && [ -f "$T/pfx/drive_c/shapes.bmp" ] && mkdir -p "$HERE/build" \
    && convert "$T/pfx/drive_c/shapes.bmp" -alpha off -scale 400% "$HERE/build/smooth-shapes.png" 2>/dev/null
[ $RC = 0 ] && echo "smooth-shapes-check: ok" || echo "smooth-shapes-check: FAILED"
exit $RC
