#!/bin/sh
# Gate for WordPad (sg-wordpad, wordpad.exe/write.exe): on a shell desktop
# under Xvfb, with the X mouse and keyboard.
#
#  - `wordpad.exe` through App Paths opens "Document - WordPad"; with a
#    wine-sg carrying 0180, Wine's own write.exe and wordpad.exe hand off to it
#    (a CreateProcess of each);
#  - typed text made bold, centred, bulleted and 20 pt from the ribbon (and
#    the font size box), the date inserted, the ruler's left-indent marker
#    dragged an inch; Save As RTF has each of those in the file;
#  - a .docx (made by test/wordpad-mkdocs.py from ECMA-376) opens with its
#    heading, italic/underline/red/highlight runs, centred paragraph, lists
#    and picture -- the picture's red and blue halves on screen (wine-sg 0184
#    fixed RichEdit's \dibitmap reader) -- and saves back as .docx whose
#    document.xml has the formatting and whose PNG part has the picture;
#  - the .odt equivalent opens and saves as .odt (mimetype first and stored,
#    styles for bold/italic, the list, the picture);
#  - an RTF with a picture saves it (wine-sg 0184: RichEdit wrote no bitmap
#    pictures) and reopens with it;
#  - UTF-16 text opens, and saves as UTF-8 after the "Text-Only" warning;
#  - `/p` of a 120-line document prints three pages (EM_FORMATRANGE, wine-sg
#    0184) with the text inside Page Setup's margins (SG_WORDPAD_PRINT_EMF
#    writes the pages as EMF; the probe plays them into bitmaps);
#  - Print preview shows "Page 1 of 3" and pages forward; Find selects a word;
#    View > Zoom in and the status bar zoom; closing with changes asks, and
#    "Don't Save" leaves the file alone.
#
# Screenshots: build/wordpad-*.png. Needs Xvfb, xdotool, ImageMagick, python3
# with PIL, mingw; skips (77) without them. SG_WINE_DIR is an installed Wine
# (bin/wine) or a build tree (./wine, server/wineserver); SG_WORDPAD_EXE tests
# another build (the mutation test); SG_WORDPAD_DPY picks the display.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_WORDPAD_EXE:-$HERE/build/sg-wordpad64.exe}"
OUT="$HERE/build"
RC=0; DPY="${SG_WORDPAD_DPY:-197}"; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
for need in Xvfb xdotool import convert python3 "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
python3 -c 'import PIL' 2>/dev/null || { echo "SKIP: python3-pil missing"; exit 77; }
if [ -x "$WINE_DIR/bin/wine" ]; then WINE="$WINE_DIR/bin/wine"; WINESERVER="$WINE_DIR/bin/wineserver"
elif [ -x "$WINE_DIR/wine" ]; then WINE="$WINE_DIR/wine"; WINESERVER="$WINE_DIR/server/wineserver"
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi
[ -f "$EXE" ] || { echo "SKIP: $EXE missing"; exit 77; }
mkdir -p "$OUT"

T=$(mktemp -d /var/tmp/sg-wordpad-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINESERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

Xvfb ":$DPY" -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
mkdir -p "$T/home" "$T/pfx"
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all HOME="$T/home" WINESERVER
w() { "$WINE" "$@"; }
w wineboot --init >/dev/null 2>&1; "$WINESERVER" -w
C="$WINEPREFIX/drive_c"
"$MINGW" -O2 -municode -o "$C/probe.exe" "$HERE/test/sg-wordpad-probe.c" -lgdi32 -luser32 || { echo "FAIL  probe did not build"; exit 1; }
for r in "$HERE"/theme/5*.reg; do w regedit /S "$(w winepath -w "$r" 2>/dev/null | tr -d '\r')" >/dev/null 2>&1; done
reg() { w reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x800
WEXE=$(w winepath -w "$EXE" 2>/dev/null | tr -d '\r')
# App Paths as defaults/68-sg-wordpad.reg has them, pointing at this build.
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\wordpad.exe' /ve /d "$WEXE"
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\write.exe' /ve /d "$WEXE"
"$WINESERVER" -w
WINEDEBUG=trace+explorer w explorer /desktop=shell,1280x800 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
mkdir -p "$C/t" "$C/pr"
python3 "$HERE/test/wordpad-mkdocs.py" "$C/t"

D="$C/wp.dump"
shot() { import -window root "$OUT/wordpad-$1.png" 2>/dev/null; }
dv() { sed -n "s/^$1 //p" "$D" 2>/dev/null | head -1 | tr -d '\r'; }
wait_dump() {       # key value [seconds]
    i=0; while [ "$(dv "$1")" != "$2" ] && [ $i -lt $(( ${3:-15} * 2 )) ]; do sleep 0.5; i=$((i + 1)); done
    [ "$(dv "$1")" = "$2" ]
}
# the centre of the ribbon item with this label; +dx for the arrow part
item() { tr -d '\r' < "$D" | awk -v l="$1" '$1 == "item" { n = $8; for (i = 9; i <= NF; i++) n = n " " $i; if (n == l) { print int(($3 + $5) / 2), int(($4 + $6) / 2), $5; exit } }'; }
click() { lbl=$1; set -- $(item "$1"); [ $# -eq 3 ] || { echo "  (no item $lbl)"; return 1; }; xdotool mousemove "$1" "$2" click 1; sleep 0.6; }
click_arrow() { set -- $(item "$1"); [ $# -eq 3 ] || return 1; xdotool mousemove $(( $3 - 5 )) "$2" click 1; sleep 0.8; }
running() { pgrep -x 'sg-wordpad64.ex' >/dev/null; }
quit() { pkill -x 'sg-wordpad64.ex'; i=0; while running && [ $i -lt 20 ]; do sleep 0.3; i=$((i + 1)); done; rm -f "$D"; }
start() { quit; SG_WORDPAD_DUMP='C:\wp.dump' w "$@" >/dev/null 2>&1 & }
front() { w 'C:\probe.exe' activate "$1" >/dev/null 2>&1; sleep 0.6; }
saveas() {          # F12, type the Windows path, Enter
    xdotool key F12; sleep 1.5; xdotool type --delay 30 "$1"; xdotool key Return; sleep 2
}

# --- 1. the Windows names --------------------------------------------------------------------
start start wordpad.exe
if wait_dump title "Document - WordPad" 30; then pass "wordpad.exe (App Paths) opens \"Document - WordPad\""
else fail "no WordPad window: '$(dv title)'"; shot fail-start; exit 1; fi
sleep 1.5
shot start
quit
start 'C:\windows\system32\write.exe'
if wait_dump title "Document - WordPad" 20; then pass "system32 write.exe starts our WordPad"
else fail "write.exe did not reach sg-wordpad (a wine-sg without 0180?)"; fi
quit
start 'C:\Program Files\Windows NT\Accessories\wordpad.exe' 'C:\t\in.docx'
if wait_dump title "in.docx - WordPad" 20; then pass "Wine's wordpad.exe hands off with its arguments"
else fail "Wine's wordpad.exe did not hand off: '$(dv title)'"; fi
quit

# --- 2. formatting from the ribbon ------------------------------------------------------------
start "$EXE"
wait_dump title "Document - WordPad" 20 || fail "no window for the editing test"
sleep 1.5
xdotool type --delay 40 "hello world"; sleep 0.5
xdotool key ctrl+a; sleep 0.4
click "Bold"
[ "$(dv bold)" = 1 ] && pass "Bold on the ribbon makes the selection bold" || fail "bold after the ribbon's Bold: $(dv bold)"
click "Center"
[ "$(dv align)" = 3 ] && pass "Center centres the paragraph" || fail "align after Center: $(dv align)"
click "Start a list"
[ "$(dv numbering | cut -d' ' -f1)" = 1 ] && pass "Start a list makes a bulleted paragraph" || fail "numbering: $(dv numbering)"
set -- $(sed -n 's/^sizebox //p' "$D" | tr -d '\r')
xdotool mousemove $(( ($1 + $3) / 2 - 8 )) $(( ($2 + $4) / 2 )) click 1; sleep 0.3
xdotool key ctrl+a; xdotool type "20"; xdotool key Return; sleep 0.8
[ "$(dv size)" = 20 ] && pass "the font size box sets 20 pt" || fail "size after typing 20: $(dv size)"
shot formatted
xdotool key ctrl+End; sleep 0.3
click "Date and time"; sleep 1
xdotool key Return; sleep 0.8
TODAY=$(python3 -c 'import datetime; d = datetime.date.today(); print(f"{d.month}/{d.day}/{d.year}")')
# the ruler: drag the left indent marker (the box) one inch to the right
set -- $(sed -n 's/^ruler_left //p' "$D" | tr -d '\r')
L0=$(dv indent | cut -d' ' -f1)
xdotool mousemove "$1" "$2" sleep 0.2 mousedown 1 sleep 0.2 mousemove $(( $1 + 48 )) "$2" sleep 0.2 mousemove $(( $1 + 96 )) "$2" sleep 0.3 mouseup 1
sleep 0.8
L1=$(dv indent | cut -d' ' -f1)
[ "$L1" -ge $(( L0 + 1400 )) ] 2>/dev/null && [ "$L1" -le $(( L0 + 1480 )) ] && pass "dragging the ruler's left-indent box an inch indents $L0 -> $L1 twips" \
    || fail "ruler drag: left indent $L0 -> $L1"
shot ruler
saveas 'c:\t\typed.rtf'
if [ -f "$C/t/typed.rtf" ]; then
    python3 - "$C/t/typed.rtf" "$TODAY" "$L1" > "$T/rtf.out" <<'EOF'
import re, sys
s = open(sys.argv[1], encoding="latin-1").read()
checks = [("bold", r"\\b[\\ ]"), ("centred", r"\\qc"), ("bulleted", r"\\pnlvlblt"), ("20 pt", r"\\fs40"),
          ("the text", r"hello world"), ("the ruler's indent", r"\\li" + sys.argv[3] + r"(?![0-9])")]
for name, rx in checks:
    print(name, "yes" if re.search(rx, s) else "no")
print("date", "yes" if sys.argv[2] in s else "no")
EOF
    for k in "bold" "centred" "bulleted" "20 pt" "the text" "the ruler's indent" "date"; do
        grep -q "^$k yes" "$T/rtf.out" && pass "the saved RTF has $k" || fail "the saved RTF lacks $k"
    done
else fail "Save As did not write typed.rtf"; fi
[ "$(dv title)" = "typed.rtf - WordPad" ] && [ "$(dv dirty)" = 0 ] && pass "saved: title typed.rtf, not dirty" || fail "after save: '$(dv title)' dirty=$(dv dirty)"
# Find selects the word
xdotool key ctrl+Home; sleep 0.3
xdotool key ctrl+f; sleep 1.2
xdotool type --delay 40 "world"; xdotool key Return; sleep 0.8
xdotool key Escape; sleep 0.5
S=$(dv sel)
[ "$S" = "6 11" ] && pass "Find selects \"world\" (6-11)" || fail "Find: selection $S"
# zoom from the View tab, and back
A100=$(dv advance10)
click "View"; sleep 0.4
click "Zoom in"
[ "$(dv zoom)" = 150 ] && pass "View > Zoom in: 150%" || fail "zoom after Zoom in: $(dv zoom)"
A150=$(dv advance10)
# the text's advance scales with the font (wine-sg 0184: RichEdit kept the
# old size's glyph advances, and zoomed letters overlapped)
[ $(( A150 * 100 )) -ge $(( A100 * 135 )) ] 2>/dev/null && [ $(( A150 * 100 )) -le $(( A100 * 165 )) ] \
    && pass "zoomed text spreads with its font ($A100 -> $A150 px over ten characters)" \
    || fail "zoomed text advance $A100 -> $A150 px (letters overlap)"
shot view
click "100%"
set -- $(sed -n 's/^zoomminus //p' "$D" | tr -d '\r')
xdotool mousemove "$1" "$2" click 1; sleep 0.5
[ "$(dv zoom)" = 90 ] && pass "the status bar's zoom out: 90%" || fail "status bar zoom: $(dv zoom)"
click "Home"
# closing with changes asks; Don't Save leaves the file alone
xdotool type --delay 40 "x"; sleep 0.4
SUM=$(md5sum < "$C/t/typed.rtf")
xdotool key alt+F4; sleep 1.5
case "$(dv message)" in *"save changes to typed.rtf"*) pass "closing with changes asks to save";; *) fail "no save question: '$(dv message)'";; esac
shot ask
xdotool key alt+n; sleep 1.5
running && fail "WordPad still running after Don't Save" || pass "Don't Save closes"
[ "$(md5sum < "$C/t/typed.rtf")" = "$SUM" ] && pass "Don't Save leaves the file as it was" || fail "the file changed after Don't Save"

# --- 3. DOCX in and out ------------------------------------------------------------------------
start "$EXE" 'C:\t\in.docx'
wait_dump title "in.docx - WordPad" 20 || fail "in.docx did not open: '$(dv title)'"
sleep 2
shot docx
python3 - "$OUT/wordpad-docx.png" "$(dv edit)" > "$T/px.out" <<'EOF'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
l, t, r, b = map(int, sys.argv[2].split())
cnt = {"red text": 0, "yellow mark": 0, "picture red": 0, "picture blue": 0}
for y in range(t, b):
    for x in range(l, r):
        p = im.getpixel((x, y))
        if p == (255, 255, 0): cnt["yellow mark"] += 1
        if p == (0, 0, 255): cnt["picture blue"] += 1
        if p == (255, 0, 0): cnt["picture red"] += 1
        elif p[0] > 180 and p[1] < 90 and p[2] < 90: cnt["red text"] += 1
for k, v in cnt.items(): print(k, v)
EOF
for k in "red text:20" "yellow mark:200" "picture red:5000" "picture blue:5000"; do
    n=$(grep "^${k%%:*} " "$T/px.out" | awk '{print $NF}')
    [ "${n:-0}" -ge "${k##*:}" ] && pass "the .docx shows its ${k%%:*} ($n pixels)" || fail "the .docx's ${k%%:*}: ${n:-0} pixels"
done
[ "$(dv bold)" = 1 ] && [ "$(dv size)" = 16 ] && pass "the heading is bold 16 pt (its style, following basedOn)" || fail "heading: bold=$(dv bold) size=$(dv size)"
saveas 'c:\t\out.docx'
if [ -f "$C/t/out.docx" ]; then
    python3 - "$C/t/out.docx" > "$T/docx.out" <<'EOF'
import io, re, sys, zipfile
from PIL import Image
z = zipfile.ZipFile(sys.argv[1])
print("zip", "yes" if z.testzip() is None else "no")
d = z.read("word/document.xml").decode()
def has(name, ok): print(name, "yes" if ok else "no")
paras = re.findall(r"<w:p>.*?</w:p>", d)
has("heading bold", any("Quarterly Report" in p and "<w:b/>" in p for p in paras))
has("italic run", re.search(r"<w:i/>.*?italic</w:t>", d) is not None)
has("underline run", re.search(r'<w:u w:val="single"/>.*?underlined</w:t>', d) is not None)
has("red run", re.search(r'<w:color w:val="FF0000"/>.*?red</w:t>', d) is not None)
has("highlight", re.search(r'<w:highlight w:val="yellow"/>.*?marked</w:t>', d) is not None)
has("superscript", 'w:vertAlign w:val="superscript"' in d)
has("centred", any("Centred big" in p and 'w:jc w:val="center"' in p for p in paras))
has("list", d.count("<w:numPr>") >= 3 and "bullet" in z.read("word/numbering.xml").decode())
has("accent", "caf\u00e9" in d)
pngs = [n for n in z.namelist() if n.startswith("word/media/") and n.endswith(".png")]
ok = False
if pngs:
    im = Image.open(io.BytesIO(z.read(pngs[0]))).convert("RGB")
    w, h = im.size
    ok = im.getpixel((w // 2, h // 4))[0] > 200 and im.getpixel((w // 2, 3 * h // 4))[2] > 200
has("picture", ok)
tbl = re.search(r"<w:tbl>.*?</w:tbl>", d, re.S)
t = tbl.group(0) if tbl else ""
rows = re.findall(r"<w:tr>.*?</w:tr>", t, re.S)
has("table", len(rows) == 2 and all(r.count("<w:tc>") == 2 for r in rows) and "Cell A1" in rows[0] and "Cell B2" in rows[1])
w = [int(x) for x in re.findall(r'<w:gridCol w:w="(\d+)"/>', t)]
has("table widths", len(w) == 2 and abs(w[0] - 2000) <= 30 and abs(w[1] - 3000) <= 30)
has("table bold cell", bool(rows) and re.search(r"<w:b/>.*?Cell B1", rows[0], re.S) is not None and "Cell A1</w:t>" in rows[0])
EOF
    for k in zip "heading bold" "italic run" "underline run" "red run" "highlight" "superscript" "centred" "list" "accent" "picture" "table" "table widths" "table bold cell"; do
        grep -q "^$k yes" "$T/docx.out" && pass "out.docx: $k" || fail "out.docx: $k missing"
    done
else fail "Save As .docx wrote nothing"; fi
[ "$(dv format)" = docx ] && pass "saved in the .docx format" || fail "format after saving .docx: $(dv format)"

# --- 4. ODT in and out ---------------------------------------------------------------------------
start "$EXE" 'C:\t\in.odt'
wait_dump title "in.odt - WordPad" 20 || fail "in.odt did not open: '$(dv title)'"
sleep 2
shot odt
[ "$(dv bold)" = 1 ] && pass "the .odt heading is bold" || fail ".odt heading bold=$(dv bold)"
saveas 'c:\t\out.odt'
if [ -f "$C/t/out.odt" ]; then
    python3 - "$C/t/out.odt" > "$T/odt.out" <<'EOF'
import io, re, sys, zipfile
from PIL import Image
z = zipfile.ZipFile(sys.argv[1])
def has(name, ok): print(name, "yes" if ok else "no")
first = z.infolist()[0]
has("mimetype first and stored", first.filename == "mimetype" and first.compress_type == 0 and
    z.read("mimetype") == b"application/vnd.oasis.opendocument.text")
c = z.read("content.xml").decode()
styles = dict(re.findall(r'style:name="(T\d+)" style:family="text"><style:text-properties([^/]*)/>', c))
def span(word):
    m = re.search(r'<text:span text:style-name="(T\d+)">[^<]*' + word, c)
    return styles.get(m.group(1), "") if m else ""
has("bold heading", 'font-weight="bold"' in span("Quarterly"))
has("italic", 'font-style="italic"' in span("italic"))
has("red", 'fo:color="#ff0000"' in span("red"))
has("list", "<text:list " in c and "<text:list-item>" in c)
pics = [n for n in z.namelist() if n.startswith("Pictures/")]
ok = False
if pics:
    im = Image.open(io.BytesIO(z.read(pics[0]))).convert("RGB")
    w, h = im.size
    ok = im.getpixel((w // 2, h // 4))[0] > 200 and im.getpixel((w // 2, 3 * h // 4))[2] > 200
has("picture", ok and "Pictures/" in z.read("META-INF/manifest.xml").decode())
rows = re.findall(r"<table:table-row>.*?</table:table-row>", c, re.S)
has("table", len(rows) == 2 and all(r.count("<table:table-cell") == 2 for r in rows) and "Cell A1" in rows[0] and "Cell B2" in rows[1])
cw = [float(x) for x in re.findall(r'style:family="table-column"><style:table-column-properties style:column-width="([0-9.]+)in"', c)]
has("table widths", len(cw) == 2 and abs(cw[0] - 1.3889) < 0.03 and abs(cw[1] - 2.0833) < 0.03)
EOF
    for k in "mimetype first and stored" "bold heading" "italic" "red" "list" "picture" "table" "table widths"; do
        grep -q "^$k yes" "$T/odt.out" && pass "out.odt: $k" || fail "out.odt: $k missing"
    done
else fail "Save As .odt wrote nothing"; fi

# --- 5. a picture through RTF --------------------------------------------------------------------
start "$EXE" 'C:\t\in.docx'
wait_dump title "in.docx - WordPad" 20
sleep 1.5
saveas 'c:\t\pic.rtf'
grep -q 'trowd' "$C/t/pic.rtf" 2>/dev/null && grep -q 'cellx5000' "$C/t/pic.rtf" && pass "the .docx's table is a table in the RTF (trowd, cellx5000)" \
    || fail "no table in the RTF saved from the .docx"
grep -q 'dibitmap' "$C/t/pic.rtf" 2>/dev/null && pass "the picture is written into the RTF (\\dibitmap)" \
    || fail "the RTF has no picture (RichEdit without 0184 drops bitmaps)"
start "$EXE" 'C:\t\pic.rtf'
wait_dump title "pic.rtf - WordPad" 20
sleep 2
shot rtfpic
n=$(python3 - "$OUT/wordpad-rtfpic.png" "$(dv edit)" <<'EOF'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
l, t, r, b = map(int, sys.argv[2].split())
print(sum(1 for y in range(t, b) for x in range(l, r) if im.getpixel((x, y)) == (0, 0, 255)))
EOF
)
[ "${n:-0}" -ge 5000 ] && pass "reopened, the RTF's picture is there ($n blue pixels)" || fail "picture after the RTF round trip: ${n:-0} blue pixels"

# --- 6. plain text -------------------------------------------------------------------------------
printf '\377\376' > "$C/t/u16.txt"
printf 'caf\351 na\357ve\r\nline two\r\n' | iconv -f latin1 -t utf-16le >> "$C/t/u16.txt"
start "$EXE" 'C:\t\u16.txt'
wait_dump title "u16.txt - WordPad" 20 || fail "u16.txt did not open"
sleep 1
[ "$(dv length)" -ge 20 ] 2>/dev/null && pass "UTF-16 text opens ($(dv length) characters)" || fail "text length $(dv length)"
saveas 'c:\t\u16b.txt'
case "$(dv message)" in *Text-Only*) pass "saving as text warns about losing formatting";; *) fail "no Text-Only warning: '$(dv message)'";; esac
xdotool key Return; sleep 1.5
python3 -c "import sys; s = open(sys.argv[1], 'rb').read(); sys.exit(0 if s.startswith(b'\xff\xfe') and s[2:].decode('utf-16le').startswith('café naïve\r\nline two') else 1)" "$C/t/u16b.txt" \
    && pass "a Unicode text document saves back as UTF-16" || fail "u16b.txt: $(od -c "$C/t/u16b.txt" 2>/dev/null | head -2 | tr '\n' ' ')"
xdotool key ctrl+n; sleep 1
xdotool type --delay 40 "plain words"; sleep 0.3
saveas 'c:\t\u8.txt'
xdotool key Return; sleep 1.5
python3 -c "import sys; s = open(sys.argv[1], 'rb').read(); sys.exit(0 if s == b'plain words' else 1)" "$C/t/u8.txt" \
    && pass "a new document saves as .txt in UTF-8" || fail "u8.txt: $(od -c "$C/t/u8.txt" 2>/dev/null | head -2 | tr '\n' ' ')"
quit

# a table drawn as one grid: its long horizontal lines (the edit area's), evenly spaced
grid() {
    python3 - "$OUT/wordpad-$1.png" "$(dv edit)" <<'EOF'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("L")
l, t, r, b = map(int, sys.argv[2].split())
ys = []
for y in range(t, b):
    run = best = 0
    for x in range(l, r):
        if im.getpixel((x, y)) < 100: run += 1; best = max(best, run)
        else: run = 0
    if best > 300 and (not ys or y > ys[-1] + 1): ys.append(y)
gaps = [b2 - a for a, b2 in zip(ys, ys[1:])]
print(len(ys), "even" if gaps and max(gaps) - min(gaps) <= 2 else "uneven", ",".join(map(str, gaps)))
EOF
}
# --- 6b. Insert > Table ------------------------------------------------------------------------------
start "$EXE"
wait_dump title "Document - WordPad" 20
sleep 1.5
xdotool type --delay 40 "before"; sleep 0.3
click "Table"; sleep 1.2
shot tabledlg
xdotool key Return; sleep 1.2
xdotool type --delay 40 "xyz"; sleep 0.3
shot table
saveas 'c:\t\ins.docx'
python3 - "$C/t/ins.docx" > "$T/ins.out" <<'EOF'
import re, sys, zipfile
d = zipfile.ZipFile(sys.argv[1]).read("word/document.xml").decode()
t = re.search(r"<w:tbl>.*?</w:tbl>", d, re.S)
t = t.group(0) if t else ""
rows = re.findall(r"<w:tr>.*?</w:tr>", t, re.S)
cells = re.findall(r"<w:tc>.*?</w:tc>", rows[0], re.S) if rows else []
print("rows", len(rows), "cols", len(re.findall(r"<w:gridCol ", t)), "first", "xyz" in (cells[0] if cells else ""),
      "before", "before" in d.split("<w:tbl>")[0])
EOF
[ "$(cat "$T/ins.out")" = "rows 2 cols 3 first True before True" ] && pass "Insert > Table: a 3 x 2 table after the paragraph, typing goes into its first cell" \
    || fail "inserted table: $(cat "$T/ins.out")"
G=$(grid table)
case "$G" in "3 even "*) pass "it is drawn as one grid, no gaps between rows (lines $G)";; *) fail "the inserted table is drawn as: $G";; esac
# through RTF and back (wine-sg 0240: the first cell kept its \intbl, the rows their spacing)
saveas 'c:\t\ins.rtf'
start "$EXE" 'C:\t\ins.rtf'
wait_dump title "ins.rtf - WordPad" 20
sleep 1.5
shot tablertf
G=$(grid tablertf)
case "$G" in "3 even "*) pass "saved as RTF and reopened, still a table, one grid (lines $G)";; *) fail "the RTF table reopened as: $G";; esac
saveas 'c:\t\ins2.docx'
python3 - "$C/t/ins2.docx" > "$T/ins.out" <<'EOF'
import re, sys, zipfile
d = zipfile.ZipFile(sys.argv[1]).read("word/document.xml").decode()
t = re.search(r"<w:tbl>.*?</w:tbl>", d, re.S)
t = t.group(0) if t else ""
rows = re.findall(r"<w:tr>.*?</w:tr>", t, re.S)
print("rows", len(rows), "xyz", "xyz" in t)
EOF
[ "$(cat "$T/ins.out")" = "rows 2 xyz True" ] && pass "the reopened RTF's table saves as a table" || fail "reopened RTF table: $(cat "$T/ins.out")"
quit

# --- 6c. a Word 97-2003 document (made here by LibreOffice when it is installed) -------------------
if command -v soffice >/dev/null 2>&1; then
    ( cd "$C/t" && HOME="$T/lo" timeout 180 soffice --headless --convert-to 'doc:MS Word 97' in.docx >/dev/null 2>&1 )
fi
if [ -f "$C/t/in.doc" ]; then
    start "$EXE" 'C:\t\in.doc'
    wait_dump title "in.doc - WordPad" 20 || fail "in.doc did not open: '$(dv title)' $(dv message)"
    sleep 2
    shot doc
    [ "$(dv format)" = doc ] && pass "a Word 97-2003 document opens (our own reader)" || fail ".doc format: $(dv format)"
    [ "$(dv length)" -ge 150 ] 2>/dev/null && pass "its text is there ($(dv length) characters)" || fail ".doc text length $(dv length)"
    xdotool key ctrl+s; sleep 1.5; xdotool type --delay 30 'c:\t\fromdoc.docx'; xdotool key Return; sleep 2
    python3 - "$C/t/fromdoc.docx" > "$T/fromdoc.out" <<'EOF'
import re, sys, zipfile
d = zipfile.ZipFile(sys.argv[1]).read("word/document.xml").decode()
def has(name, ok): print(name, "yes" if ok else "no")
paras = re.findall(r"<w:p>.*?</w:p>", d)
# (LibreOffice writes the heading's style away: the bold that is in the .doc is Cell B1's)
has("the bold cell", re.search(r"<w:b/>(?:(?!</w:r>).)*Cell B1", d, re.S) is not None)
has("18 pt", re.search(r'<w:sz w:val="36"/>(?:(?!</w:r>).)*Centred big', d, re.S) is not None)
has("italic", re.search(r"<w:i/>.*?italic</w:t>", d) is not None)
has("red", re.search(r'<w:color w:val="FF0000"/>.*?red</w:t>', d) is not None)
has("centred", any("Centred big" in p and 'w:jc w:val="center"' in p for p in paras))
has("the accent", "caf\u00e9" in d)
t = re.search(r"<w:tbl>.*?</w:tbl>", d, re.S)
rows = re.findall(r"<w:tr>.*?</w:tr>", t.group(0), re.S) if t else []
has("the table", len(rows) == 2 and "Cell A1" in rows[0] and "Cell B1" in rows[0] and "Cell B2" in rows[1])
w = [int(x) for x in re.findall(r'<w:gridCol w:w="(\d+)"/>', t.group(0))] if t else []
has("its widths", len(w) == 2 and abs(w[0] - 2000) <= 60 and abs(w[1] - 3000) <= 60)
EOF
    for k in "the bold cell" "18 pt" "italic" "red" "centred" "the accent" "the table" "its widths"; do
        grep -q "^$k yes" "$T/fromdoc.out" && pass "from the .doc: $k" || fail "from the .doc: $k missing"
    done
    [ "$(dv title)" = "fromdoc.docx - WordPad" ] && pass "Save on a .doc asks where to save it as .docx" || fail "after Save on a .doc: '$(dv title)'"
    quit
else echo "      (no LibreOffice: the .doc checks are skipped)"; fi

# --- 7. printing: /p and print preview ------------------------------------------------------------
python3 - "$C/t/long.rtf" <<'EOF'
import sys
s = "{\\rtf1\\ansi{\\fonttbl{\\f0 Arial;}}\\f0\\fs24 "
s += "".join(f"Line number {i} of the long document.\\par\n" for i in range(1, 121))
open(sys.argv[1], "w").write(s + "}")
EOF
SG_WORDPAD_PRINT_EMF='C:\pr' SG_WORDPAD_DUMP='C:\wp.dump' w "$EXE" /p 'C:\t\long.rtf' >/dev/null 2>&1
N=$(head -1 "$C/pr/pages.txt" 2>/dev/null | tr -d '\r')
[ "$N" = 3 ] && pass "/p lays 120 lines out on 3 pages" || fail "/p printed ${N:-no} pages"
if [ -f "$C/pr/page1.emf" ]; then
    w 'C:\probe.exe' emf2bmp 'C:\pr\page1.emf' 'C:\pr\page1.bmp' >/dev/null 2>&1
    w 'C:\probe.exe' emf2bmp 'C:\pr\page3.emf' 'C:\pr\page3.bmp' >/dev/null 2>&1
    convert "$C/pr/page1.bmp" "$OUT/wordpad-page1.png" 2>/dev/null
    python3 - "$C/pr/page1.bmp" "$C/pr/page3.bmp" > "$T/page.out" <<'EOF'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("L")
w, h = im.size
ink = [(x, y) for y in range(h) for x in range(w) if im.getpixel((x, y)) < 128]
xs = [p[0] for p in ink]; ys = [p[1] for p in ink]
# Page Setup's default margins: 1.25" left (120 px at 96 dpi), 1" top/bottom (96 px)
print("ink", len(ink))
print("left", min(xs) if xs else -1)
print("top", min(ys) if ys else -1)
print("bottom", max(ys) if ys else -1)
im3 = Image.open(sys.argv[2]).convert("L")
print("ink3", sum(1 for y in range(im3.size[1]) for x in range(im3.size[0]) if im3.getpixel((x, y)) < 128))
EOF
    INK=$(sed -n 's/^ink //p' "$T/page.out"); LEFT=$(sed -n 's/^left //p' "$T/page.out")
    TOP=$(sed -n 's/^top //p' "$T/page.out"); BOT=$(sed -n 's/^bottom //p' "$T/page.out")
    [ "${INK:-0}" -gt 5000 ] && [ "$LEFT" -ge 118 ] && [ "$LEFT" -le 126 ] && [ "$TOP" -ge 96 ] && [ "$TOP" -le 110 ] && [ "$BOT" -le 960 ] \
        && pass "page 1 is printed inside the margins (text from x=$LEFT, y=$TOP..$BOT)" \
        || fail "page 1: ink=$INK left=$LEFT top=$TOP bottom=$BOT"
    [ "$(sed -n 's/^ink3 //p' "$T/page.out")" -gt 1000 ] 2>/dev/null && pass "page 3 has the last lines" || fail "page 3 is empty"
else fail "no printed pages"; fi
# page numbers and a header (as Page Setup keeps them)
OPT='HKCU\Software\Microsoft\Windows\CurrentVersion\Applets\Wordpad\Options'
reg "$OPT" /v PrintPageNumbers /t REG_DWORD /d 1
reg "$OPT" /v Header /d '&lLeft &f&rRight &p of &P'
rm -f "$C/pr/"*
SG_WORDPAD_PRINT_EMF='C:\pr' w "$EXE" /p 'C:\t\long.rtf' >/dev/null 2>&1
python3 - "$C/pr" > "$T/hf.out" <<'EOF'
import glob, os, struct, sys
pages = sorted(glob.glob(os.path.join(sys.argv[1], "page*.emf")), key=lambda p: int(p.rsplit("page", 1)[1][:-4]))
print("pages", len(pages))
for n, p in enumerate(pages, 1):
    d = open(p, "rb").read()
    off = 0
    while off + 8 <= len(d):
        typ, size = struct.unpack_from("<II", d, off)
        if size < 8: break
        if typ == 84:
            x, y, nch, offs = struct.unpack_from("<iiII", d, off + 36)
            s = d[off + offs: off + offs + 2 * nch].decode("utf-16le", "replace")
            if s.startswith(("Page", "Left", "Right")): print(n, x, y, s)
        off += size
EOF
grep -q '^1 .* Page 1$' "$T/hf.out" && grep -q '^3 .* Page 3$' "$T/hf.out" && pass "Print page numbers: Page 1 .. Page 3 at the foot" || fail "page numbers: $(tr '\n' '|' < "$T/hf.out")"
grep -q '^2 .* Left long.rtf$' "$T/hf.out" && grep -q '^2 .* Right 2 of 3$' "$T/hf.out" && pass "the header's &f, &p, &P on the left and the right" || fail "header: $(tr '\n' '|' < "$T/hf.out")"
HY=$(awk '$1 == 1 && $4 == "Left" { print $3; exit }' "$T/hf.out"); FY=$(awk '$1 == 1 && $4 == "Page" { print $3; exit }' "$T/hf.out")
[ -n "$HY" ] && [ -n "$FY" ] && [ "$HY" -lt 96 ] && [ "$FY" -gt 960 ] && pass "both are in the margins (header y=$HY, footer y=$FY)" || fail "header y=$HY footer y=$FY"
reg "$OPT" /v PrintPageNumbers /t REG_DWORD /d 0
reg "$OPT" /v Header /d ""
start "$EXE" 'C:\t\long.rtf'
wait_dump title "long.rtf - WordPad" 20
sleep 1
click "File"; sleep 0.8
xdotool key p; sleep 0.5; xdotool key v; sleep 2
case "$(dv preview)" in "1 3 0") pass "Print preview: page 1 of 3";; *) fail "print preview: '$(dv preview)'";; esac
shot preview
set -- $(awk '$1 == "previewbutton" && $2 == 2 { print $3, $4; exit }' "$D" | tr -d '\r')
[ $# -eq 2 ] && { xdotool mousemove "$1" "$2" click 1; sleep 1; }
case "$(dv preview)" in "2 3 0") pass "Next page: page 2";; *) fail "after Next page: '$(dv preview)'";; esac
shot preview2
xdotool key Escape; sleep 0.8
quit

echo
[ $RC -eq 0 ] && echo "wordpad-check: all passed" || echo "wordpad-check: FAILED"
exit $RC
