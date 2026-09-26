#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for Sticky Notes (sg-sticky): started through App Paths (stikynot.exe)
# on a shell desktop, a first note appears; text typed on the X keyboard lands
# in it, Ctrl+B makes it bold, the ... menu turns it green, dragging its strip
# moves it. The process is then killed -- no chance to save on the way out --
# and started again: the note must come back with the same text, colour,
# bold and place (the debounced save already wrote it). A second start is
# handed to the first (one process); /new adds a note, /list opens the notes
# list, whose search filters; "Delete note" asks, and removes the note's file.
#
# Screenshots: build/sticky-{note,menu,restored,list}.png. Needs wine-sg, Xvfb,
# xdotool, ImageMagick and python3; skips (77) without them. SG_STICKY_EXE runs
# another build (the mutation test does).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_STICKY_EXE:-$HERE/build/sg-sticky64.exe}"
OUT="$HERE/build"
RC=0; DPY=118; XP=""
pass() { printf "PASS  %s\n" "$*"; }
fail() { printf "FAIL  %s\n" "$*"; RC=1; }

for need in Xvfb xdotool import python3; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-sticky-check.XXXXXX); chmod 755 "$T"
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
C="$WINEPREFIX/drive_c"
cp "$EXE" "$C/sg-sticky64.exe"
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\stikynot.exe' /ve /d 'C:\sg-sticky64.exe'
wineserver -w
DUMP="$T/dump.txt"
export SG_STICKY_DUMP="$(wine winepath -w "$DUMP" 2>/dev/null | tr -d '\r')"
NOTES="$C/users/$(id -un)/AppData/Local/Stained Glass/Sticky Notes"

WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

# dump helpers
field() { sed -n "s/.*[ ]$2=\([^ ]*\).*/\1/p" "$DUMP" 2>/dev/null | sed -n "$1p"; }
noteline() { grep "^NOTE $1 " "$DUMP" 2>/dev/null; }
nfield() { noteline "$1" | sed -n "s/.* $2=\([^ ]*\).*/\1/p"; }
ntext() { noteline "$1" | sed -n 's/.* text=//p'; }
btn() { grep "^BUTTONS $1 " "$DUMP" | sed -n "s/.* $2=\([0-9-]*\),\([0-9-]*\).*/\1 \2/p"; }
menupt() { grep "^MENU $1 " "$DUMP" | sed -n "s/.* $2=\([0-9-]*\),\([0-9-]*\).*/\1 \2/p"; }
listf() { grep '^LIST ' "$DUMP" | sed -n "s/.* $1=\([^ ]*\).*/\1/p"; }
files() { sed -n 's/^FILES \([0-9]*\).*/\1/p' "$DUMP"; }
wait_for() { # $1 = shell condition, $2 = seconds
    i=0; while ! eval "$1" && [ $i -lt $(( $2 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done; eval "$1"
}
click() { xdotool mousemove "$1" "$2" click 1; sleep 0.6; }
shot() { import -window root "$OUT/sticky-$1.png" 2>/dev/null; }
count_procs() { wine tasklist 2>/dev/null | tr -d '\r' | grep -ci '^sg-sticky64'; }

# --- first start, through App Paths: a new note -----------------------------------------------
wine start stikynot.exe >/dev/null 2>&1
wait_for '[ "$(nfield 0 visible)" = 1 ]' 30 && pass "stikynot.exe (App Paths) opens a first note" || fail "no note: $(cat "$DUMP" 2>/dev/null)"
wait_for '[ "$(files)" = 1 ]' 10 && pass "the note is saved at once (one .note file)" || fail "files: $(files)"
[ -d "$NOTES" ] && pass "under %LOCALAPPDATA%\\Stained Glass\\Sticky Notes" || fail "no $NOTES ($(grep ^FILES "$DUMP"))"

rect=$(nfield 0 rect); IFS=, read -r l t r b <<EOF
$rect
EOF
click $(( (l + r) / 2 )) $(( (t + b) / 2 ))
xdotool type --delay 60 "hello sticky notes"; sleep 1
wait_for '[ "$(ntext 0)" = "hello sticky notes" ]' 5 && pass "typed text lands in the note" || fail "text: '$(ntext 0)'"
xdotool key ctrl+a; sleep 0.3; xdotool key ctrl+b; sleep 0.3; xdotool key End; sleep 1.5
shot note

# --- the ... menu: green ----------------------------------------------------------------------
set -- $(btn 0 more); click "$1" "$2"
wait_for 'grep -q "^MENU 0 " "$DUMP"' 5 && pass "... opens the note's menu" || fail "no menu"
shot menu
set -- $(menupt 0 green); click "$1" "$2"
wait_for '[ "$(nfield 0 color)" = green ]' 5 && pass "choosing green turns the note green" || fail "colour: $(nfield 0 color)"
grep -q "^MENU 0 " "$DUMP" && fail "the menu stayed open" || pass "and closes the menu"

# --- drag the strip: the note moves ---------------------------------------------------------
set -- $(btn 0 new); sx=$(( $1 + 60 )); sy=$2
xdotool mousemove "$sx" "$sy" mousedown 1; sleep 0.3
xdotool mousemove $(( sx + 60 )) $(( sy + 40 )); sleep 0.3
xdotool mousemove $(( sx - 150 )) $(( sy + 90 )); sleep 0.5; xdotool mouseup 1; sleep 1
moved=$(nfield 0 rect); IFS=, read -r ml mt mr mb <<EOF
$moved
EOF
[ $(( ml - l )) = -150 ] && [ $(( mt - t )) = 90 ] && pass "dragging the strip moves the note (-150,+90)" || fail "moved from $rect to $moved"
[ $(( mr - ml )) = $(( r - l )) ] && pass "and keeps its size" || fail "size changed: $rect -> $moved"
sleep 2    # past the debounced save
F=$(ls "$NOTES"/*.note 2>/dev/null | head -1)
grep -q '^Color=green' "$F" 2>/dev/null && pass "the file has the colour" || fail "file: $(head -5 "$F" 2>/dev/null)"
grep -q 'hello sticky notes' "$F" && grep -q '\\b' "$F" && pass "and the text, bold (RTF)" || fail "file body: $(tail -c 300 "$F")"

# --- killed, started again: the same note ---------------------------------------------------
wine taskkill /f /im sg-sticky64.exe >/dev/null 2>&1; sleep 2
rm -f "$DUMP"
wine start stikynot.exe >/dev/null 2>&1
wait_for '[ "$(nfield 0 visible)" = 1 ]' 30 && pass "started again, the note is back" || fail "no note after restart"
[ "$(ntext 0)" = "hello sticky notes" ] && pass "with its text" || fail "restored text: '$(ntext 0)'"
[ "$(nfield 0 color)" = green ] && pass "its colour (green)" || fail "restored colour: $(nfield 0 color)"
[ "$(nfield 0 rect)" = "$moved" ] && pass "and its place ($moved)" || fail "restored rect: $(nfield 0 rect), want $moved"
[ "$(files)" = 1 ] && pass "no note was added on the way (one file)" || fail "files after restart: $(files)"
sleep 1; shot restored
python3 - "$OUT/sticky-restored.png" "$moved" <<'EOF' && pass "the screen shows a green note there" || fail "screen colour at the note is not green"
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
l, t, r, b = map(int, sys.argv[2].split(","))
px = im.getpixel((l + 4, b - 10))          # the body, below the text, by the bar
g = im.getpixel((r - 8, b - 50))
ok = lambda p: p[1] > p[0] + 10 and p[1] > p[2] + 10
sys.exit(0 if ok(px) or ok(g) else 1)
EOF
python3 - "$OUT/sticky-restored.png" "$moved" <<'EOF' && pass "the bold text is drawn on the note, not on a black block" || fail "the text line is mostly black (a lost background)"
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
l, t, r, b = map(int, sys.argv[2].split(","))
box = [im.getpixel((x, y)) for x in range(l + 14, l + 110) for y in range(t + 36, t + 52)]
dark = sum(1 for p in box if sum(p) < 150)
sys.exit(0 if dark < len(box) * 0.45 else 1)
EOF

# --- one process; /new; the list --------------------------------------------------------------
wine start stikynot.exe /new >/dev/null 2>&1
wait_for '[ "$(nfield 1 visible)" = 1 ]' 15 && pass "stikynot /new adds a second note" || fail "no second note"
[ "$(count_procs)" = 1 ] && pass "handled by the running copy (one process)" || fail "processes: $(count_procs)"
wait_for '[ "$(files)" = 2 ]' 5 && pass "two .note files" || fail "files: $(files)"
xdotool type --delay 60 "second one"; sleep 1.5
wine start stikynot.exe /list >/dev/null 2>&1
wait_for '[ "$(listf visible)" = 1 ]' 10 && pass "/list opens the notes list" || fail "no list"
[ "$(listf items)" = 2 ] && pass "listing both notes" || fail "list items: $(listf items)"
set -- $(grep '^LIST ' "$DUMP" | sed -n 's/.* searchbox=\([0-9-]*\),\([0-9-]*\).*/\1 \2/p'); click "$1" "$2"
xdotool type --delay 60 "hello"; sleep 1.2
[ "$(listf items)" = 1 ] && pass "searching 'hello' leaves one" || fail "search items: $(listf items) ($(listf search))"
shot list
xdotool key ctrl+a BackSpace; sleep 1
[ "$(listf items)" = 2 ] && pass "clearing the search shows both again" || fail "items after clearing: $(listf items)"

# --- delete: asked, then the file goes ------------------------------------------------------
second=$(noteline 1 | sed -n 's/.* id=\([^ ]*\).*/\1/p')
wine start stikynot.exe /new >/dev/null 2>&1; sleep 0.1   # (a no-op for the check below: a third note)
wait_for '[ "$(files)" = 3 ]' 10
set -- $(btn 1 more); click "$1" "$2"
wait_for 'grep -q "^MENU 1 " "$DUMP"' 5
set -- $(menupt 1 delete); click "$1" "$2"
sleep 1; xdotool key Return; sleep 1.5
[ ! -f "$NOTES/$second.note" ] && pass "Delete note (confirmed) removes the note's file" || fail "file still there: $second"
[ "$(files)" = 2 ] && pass "two notes left" || fail "files after delete: $(files)"
[ -f "$F" ] && pass "the first note's file is untouched" || fail "first note's file gone"

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
