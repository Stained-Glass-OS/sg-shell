#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# The emoji panel (issue #15, Windows' Win+. and Win+;): sg-emoji opens at the
# text cursor of the program in front without taking its focus; letters typed
# search ("grin" finds the grinning face), Enter types the emoji into the
# program, a click on one types it too; what was used comes back as Recently
# used; Esc clears the search and then closes it; a second start closes an
# open panel; into a Linux program's frame (SgLinuxWindow) the emoji goes to
# xdotool. With a colour emoji font installed (fonts-noto-color-emoji), the
# grid is drawn in colour.
#
# Needs wine-sg, Xvfb, xdotool, python3, mingw; skips (77) without.
# SG_EMOJI_EXE: another build (mutant SG_MUTANT_EMOJI_NO_LINUX: the Linux
# program's emoji go to SendInput). SG_EMOJI_DPY: the Xvfb display (96).
# SG_EMOJI_FONT_DIR: a folder with NotoColorEmoji.ttf when the system has none.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_EMOJI_EXE:-$HERE/build/sg-emoji64.exe}"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
RC=0; DPY=${SG_EMOJI_DPY:-96}; XP=""; EP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for need in Xvfb xdotool python3 "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
T=$(mktemp -d /var/tmp/sg-emoji.XXXXXX)
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; [ -n "${KEEP:-}" ] || rm -rf "$T"
}
trap cleanup EXIT INT TERM
"$MINGW" -O2 -municode -mwindows -o "$T/target.exe" "$HERE/test/emoji-target.c" || { fail "target did not build"; exit 1; }
if [ -n "${SG_EMOJI_FONT_DIR:-}" ]; then
    printf '<?xml version="1.0"?>\n<fontconfig><include ignore_missing="yes">/etc/fonts/fonts.conf</include><dir>%s</dir><cachedir>%s/fc</cachedir></fontconfig>\n' \
        "$SG_EMOJI_FONT_DIR" "$T" > "$T/fonts.conf"
    export FONTCONFIG_FILE="$T/fonts.conf"
fi
colour_font=0
fc-list 2>/dev/null | grep -qi 'Noto Color Emoji' && colour_font=1
cat > "$T/xdotool" <<EOS
#!/bin/sh
for a in "\$@"; do printf '[%s]' "\$a"; done >> "$T/xdotool.log"
echo >> "$T/xdotool.log"
EOS
chmod 755 "$T/xdotool"
Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=,winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1
SG_XDOTOOL=$(wine winepath -w "$T/xdotool" 2>/dev/null | tr -d '\r'); export SG_XDOTOOL
SG_EMOJI_DUMP=$(wine winepath -w "$T/dump.txt" 2>/dev/null | tr -d '\r'); export SG_EMOJI_DUMP
cp "$T/target.exe" "$EXE" "$WINEPREFIX/drive_c/"
EMOJI='C:\'$(basename "$EXE")
TT="$WINEPREFIX/drive_c/target.txt"
state() { grep '^STATE' "$T/dump.txt" 2>/dev/null | tail -1; }
wait_for() { i=0; while ! eval "$1" && [ $i -lt "${2:-40}" ]; do sleep 0.5; i=$((i + 1)); done; }
start_panel() { : > "$T/dump.txt"; wine "$EMOJI" /toggle >/dev/null 2>&1 & EP=$!; wait_for '[ -n "$(state)" ]'; sleep 1; }
panel_gone() { i=0; while kill -0 "$EP" 2>/dev/null && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done; ! kill -0 "$EP" 2>/dev/null; }

wine 'C:\target.exe' >/dev/null 2>&1 &
wait_for '[ -s "$TT" ]'
sleep 2
caret=$(sed -n 's/^caret //p' "$TT"); cx=${caret%,*}; cy=${caret#*,}
start_panel
s=$(state)
px=$(echo "$s" | sed -n 's/^STATE at \([-0-9]*\),\([-0-9]*\) .*/\1/p'); py=$(echo "$s" | sed -n 's/^STATE at \([-0-9]*\),\([-0-9]*\) .*/\2/p')
if [ -n "$px" ] && [ "$py" -ge "$cy" ] && [ "$py" -le $((cy + 60)) ] && [ "$px" -ge $((cx - 10)) ] && [ "$px" -le $((cx + 10)) ]; then
    pass "the panel opens at the program's text cursor ($s; caret $caret)"
else fail "where the panel opened: '$s', the caret at $caret"; fi
echo "$s" | grep -q 'target 0*[1-9a-fA-F]' && echo "$s" | grep -q ' linux 0 ' && pass "it knows the program in front, a Windows one" || fail "target: $s"

xdotool type --delay 120 grin; sleep 2
state | grep -q 'query "grin" .* first \[grinning face\]' && pass "typing searches: \"grin\" finds the grinning face first" || fail "search: $(state)"
xdotool key Return; sleep 2
grep -q "^text $(printf '\360\237\230\200')" "$TT" && pass "Enter types the emoji into the program (it kept the focus)" \
    || fail "after Enter the box has: $(sed -n 's/^text //p' "$TT" | od -An -tx1 | head -2)"
if [ "$colour_font" = 1 ] && command -v import >/dev/null; then
    import -display ":$DPY" -window root "$T/shot.png" 2>/dev/null
    n=$(python3 - "$T/shot.png" "$px" "$py" <<'EOF'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
x0, y0 = int(sys.argv[2]), int(sys.argv[3])
n = 0
for y in range(y0 + 94, y0 + 400):
    for x in range(x0 + 10, x0 + 350):
        r, g, b = im.getpixel((x, y))
        if max(r, g, b) - min(r, g, b) > 80: n += 1
print(n)
EOF
)
    [ "${n:-0}" -gt 500 ] && pass "the emoji are drawn in colour ($n coloured pixels in the grid)" || fail "colour: ${n:-0} coloured pixels"
else
    echo "SKIP  colour check: no Noto Color Emoji (fonts-noto-color-emoji, or SG_EMOJI_FONT_DIR) or no ImageMagick import"
fi
xdotool key Escape; sleep 1
state | grep -q 'query ""' && kill -0 "$EP" 2>/dev/null && pass "Esc clears the search, the panel stays" || fail "first Esc: $(state)"
xdotool key Escape
panel_gone && pass "a second Esc closes it" || fail "the panel is still open after Esc"

start_panel
state | grep -q 'cat 9 .* first \[grinning face\]' && pass "it opens on Recently used, with what was typed" || fail "recent: $(state)"
s=$(state)
px=$(echo "$s" | sed -n 's/^STATE at \([-0-9]*\),.*/\1/p'); py=$(echo "$s" | sed -n 's/^STATE at [-0-9]*,\([-0-9]*\) .*/\1/p')
xdotool mousemove $((px + 34)) $((py + 116)) click 1; sleep 2
[ "$(grep -c 'INSERTED sendinput' "$T/dump.txt")" -ge 1 ] && grep -q "^text $(printf '\360\237\230\200\360\237\230\200')" "$TT" \
    && pass "a click on an emoji types it into the program" || fail "click: $(grep INSERTED "$T/dump.txt"); box: $(sed -n 's/^text //p' "$TT" | od -An -tx1 | head -1)"
wine "$EMOJI" /toggle >/dev/null 2>&1
panel_gone && pass "starting it again (Win+. again) closes the open panel" || fail "toggle did not close it"

# a Linux program's frame in front: the emoji goes to xdotool
wineserver -k 2>/dev/null; sleep 1; rm -f "$TT"
wine 'C:\target.exe' linux >/dev/null 2>&1 &
sleep 4
start_panel
s=$(state)
echo "$s" | grep -q ' linux 1 ' && pass "it knows the program in front is a Linux one" || fail "linux target: $s"
px=$(echo "$s" | sed -n 's/^STATE at \([-0-9]*\),.*/\1/p'); py=$(echo "$s" | sed -n 's/^STATE at [-0-9]*,\([-0-9]*\) .*/\1/p')
xdotool mousemove $((px + 34)) $((py + 116)) click 1; sleep 2
grep -qF "[type][--clearmodifiers][--][$(printf '\360\237\230\200')]" "$T/xdotool.log" 2>/dev/null \
    && pass "into a Linux program the emoji goes to xdotool" || fail "xdotool asked: $(cat "$T/xdotool.log" 2>/dev/null); $(grep INSERTED "$T/dump.txt")"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
