#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for SG PDF's editor (sg-pdf64.exe with sg-session's sg-pdf, MuPDF),
# driven like a person on the X mouse and keyboard, on a shell desktop under
# Xvfb; every result is read back from the saved file from outside SG PDF
# (test/pdf-editor-probe.py: MuPDF, pdftotext, qpdf):
#
#   Edit PDF   the tool from the tools pane; a double-click on a paragraph
#              opens its editor, new text typed in is committed by a click
#              elsewhere and reflows in the block; a picture dragged moves,
#              its corner handle resizes it; a line picked and Del deleted;
#              Add text; Ctrl+S saves in place (qpdf --check); Ctrl+Z / Ctrl+Y
#              undo and redo what is saved
#   Comment    highlight (drag over text), sticky note (its dialog), a
#              rectangle, a free-form drawing, a text box; saved as those
#              annotations; one picked and deleted
#   Fill&Sign  a text field typed in, a check box, a radio button and a combo
#              box set with the mouse and saved; a typed signature placed;
#              the form flattened and saved as another file
#   Redact     Find text (TOPSECRET, every page) and the SSN pattern marked
#              from the dialog, text marked by dragging over it, an area
#              over a picture; Apply, then Remove hidden information; the
#              saved file has none of it in pdftotext, MuPDF or any raw
#              stream (hidden layer, invisible text, metadata, attachment,
#              JavaScript too), the picture's pixels under the mark changed
#   Organize   rotate, delete, drag a page to the front, a blank page
#   Protect    AES-256 with a password; the saved file needs it (qpdf) and
#              SG PDF asks for it when it is opened again
#   Export     to Word (.docx), Combine Files (a PDF and a picture) into a new
#              PDF that opens
#   Dark mode  the app mode's dark palette; closing with unsaved changes asks
#
# Screenshots: build/pdf-editor-*.png. SG_PDF_EXE tests another build and
# SG_PDF_HELPER another sg-pdf (mutation testing). Skips (77) without
# wine-sg, Xvfb, xdotool, ImageMagick, qpdf, pdftotext, python3-pymupdf.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_PDF_EXE:-$HERE/build/sg-pdf64.exe}"
OUT="$HERE/build"
PY=/usr/bin/python3
PROBE="$HERE/test/pdf-editor-probe.py"
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

HELPER="${SG_PDF_HELPER:-}"
if [ -z "$HELPER" ]; then
    for h in "$HERE/../sg-session/bin/sg-pdf" /usr/bin/sg-pdf; do [ -x "$h" ] && { HELPER=$(readlink -f "$h"); break; }; done
fi
for need in Xvfb xdotool import qpdf pdftotext; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
$PY -c 'import pymupdf, PIL' 2>/dev/null || { echo "SKIP: python3-pymupdf or PIL missing"; exit 77; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
[ -n "$HELPER" ] && [ -x "$HELPER" ] || { echo "SKIP: sg-pdf (sg-session) not found"; exit 77; }
echo 'quit' | "$HELPER" --serve >/dev/null 2>&1
printf 'state\nquit\n' | "$HELPER" --serve 2>/dev/null | grep -q 'ERR notopen' || { echo "SKIP: $HELPER is not the editor's sg-pdf (MuPDF)"; exit 77; }

T=$(mktemp -d /var/tmp/sg-pdf-editor.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

Xvfb -displayfd 3 -screen 0 1280x900x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
DPY=$(cat "$T/display" 2>/dev/null); [ -n "$DPY" ] || { echo "FAIL  Xvfb did not start"; exit 1; }
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH" SG_PDF="$HELPER" SG_PDF_QUIET=1
wine wineboot --init >/dev/null 2>&1; wineserver -w
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x900
reg 'HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' /v AppsUseLightTheme /t REG_DWORD /d 1
for f in "$HERE"/theme/*.reg; do wine reg import "$(wine winepath -w "$f" | tr -d '\r')" >/dev/null 2>&1; done
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
python3 - "$HERE/defaults/80-sg-pdf.reg" "$winexe" > "$T/pdf.reg" <<'EOF2'
import sys
reg = open(sys.argv[1]).read()
esc = lambda p: p.replace("\\", "\\\\")
sys.stdout.write(reg.replace(esc(r"Z:\usr\libexec\stained-glass\shell\sg-pdf64.exe"), esc(sys.argv[2])))
EOF2
wine reg import "$(wine winepath -w "$T/pdf.reg" | tr -d '\r')" >/dev/null 2>&1 || fail "reg import failed"
wineserver -w

DOCS="$WINEPREFIX/drive_c/docs"; mkdir -p "$DOCS" "$WINEPREFIX/drive_c/dumps"
$PY "$PROBE" make "$DOCS" || { fail "could not make the documents"; exit 1; }
for n in org prot combo; do cp "$DOCS/edit.pdf" "$DOCS/$n.pdf"; done
probe() { $PY "$PROBE" "$@"; }

WINEDEBUG=trace+explorer wine explorer /desktop=shell,1280x900 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

D=""
field() { sed -n "s/^$1 //p" "$D" 2>/dev/null | head -1; }
wait_field() {  # name value seconds
    i=0
    while [ "$(field "$1")" != "$2" ] && [ $i -lt $(( $3 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    [ "$(field "$1")" = "$2" ]
}
wait_line() {  # regex seconds
    i=0
    while ! grep -qE "$1" "$D" 2>/dev/null && [ $i -lt $(( $2 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    grep -qE "$1" "$D" 2>/dev/null
}
shot() { sleep 0.5; import -window root "$OUT/pdf-editor-$1.png" 2>/dev/null; }
mean() { convert "$1" -crop "$(($4 - $2))x$(($5 - $3))+$2+$3" +repage -colorspace Gray -format '%[fx:int(100*mean)]' info: 2>/dev/null; }
wd() { wine winepath -w "$1" 2>/dev/null | tr -d '\r'; }
# click the tool bar's button NAME, or the pane's
tb() { set -- $(grep "^tbtn $1 " "$D" | head -1); [ $# -ge 4 ] && xdotool mousemove "$3" "$4" click 1; sleep 0.6; }
pane() { set -- $(grep "^panebtn $1 " "$D" | head -1); [ $# -ge 4 ] && xdotool mousemove "$3" "$4" click 1; sleep 0.8; }
# the centre of a dump line's rectangle (fields 5-8 after the key)
centre() { echo "$(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 ))"; }
# page 1's rectangle, and a point on it at (X, Y) points
page_xy() {  # X Y -> screen x y (page 1 on the screen)
    set -- "$1" "$2" $(field 'page 1') 0 0 0 0
    python3 -c "l,t,r,b=$3,$4,$5,$6; s=(r-l)/612.0; print(max(0,int(l+$1*s)), max(0,int(t+$2*s)))"
}
launch() {  # FILE DUMP [ANSWERS]
    D="$WINEPREFIX/drive_c/dumps/$2"
    rm -f "$D"
    SG_PDF_DUMP="$(wd "$D")" SG_PDF_FILE_ANSWERS="${3:-}" wine "$EXE" "$1" >/dev/null 2>&1 &
    sleep 1
}
closeapp() { wine taskkill /f /im sg-pdf64.exe >/dev/null 2>&1; sleep 1.5; }
# the whole page on the screen: every point the gate clicks is on it
fitpage() { xdotool key ctrl+backslash; wait_field fit page 5 || fail "fit page: '$(field fit)'"; sleep 1; }

# ============================================================= Edit PDF
D="$WINEPREFIX/drive_c/dumps/edit"
SG_PDF_DUMP="$(wd "$D")" wine start 'C:\docs\edit.pdf' >/dev/null 2>&1 &
if wait_field pages 4 40; then pass "edit.pdf opened through the .pdf association"; else fail "edit.pdf did not open: '$(field pages)' '$(field error)'"; exit 1; fi
wait_field title 'edit.pdf - SG PDF' 5 && pass "the title is 'edit.pdf - SG PDF'" || fail "title '$(field title)'"
wait_line '^page 1 .* 1$' 15 || fail "page 1 not rendered"
fitpage
pane edit
wait_field tool '1 edit' 5 && pass "Edit PDF opens from the tools pane" || fail "tool '$(field tool)'"
wait_line '^obj 1 [0-9]+ text .*quick brown' 10 && pass "the page's text blocks, picture and paths are objects" || fail "no objects: $(grep -c '^obj' "$D")"
shot edit-tool
set -- $(grep -E '^obj 1 [0-9]+ text .*quick brown' "$D" | head -1)
xy=$(centre "$5" "$6" "$7" "$8")
# shellcheck disable=SC2086
xdotool mousemove $xy click --repeat 2 --delay 90 1
if wait_line '^editor ' 5; then
    pass "a double-click on the paragraph opens its editor"
    xdotool key ctrl+a
    xdotool type --delay 15 "Edited by the gate: a sentence long enough that it has to wrap inside the paragraph's own width."
    set -- $(page_xy 560 40); xdotool mousemove "$1" "$2" click 1
    wait_field undo 1 10 && wait_field dirty 1 3 && pass "a click elsewhere commits the text (one undo step, unsaved)" || fail "commit: undo '$(field undo)' dirty '$(field dirty)'"
    wait_field title '*edit.pdf - SG PDF' 3 || fail "title after the edit: '$(field title)'"
    sleep 1; shot edit-text
else fail "no editor after a double-click"; fi
# the picture: drag it, then its bottom-right handle
set -- $(grep -E '^obj 1 [0-9]+ image' "$D" | head -1)
if [ $# -ge 8 ]; then
    x=$(( ($5 + $7) / 2 )); y=$(( ($6 + $8) / 2 ))
    xdotool mousemove "$x" "$y" mousedown 1 mousemove $((x + 10)) $((y + 5)) mousemove $((x + 120)) $((y + 40)) mouseup 1
    wait_field undo 2 8 && pass "dragging the picture moves it" || fail "move: undo '$(field undo)'"
    sleep 0.5
    set -- $(grep -E '^obj 1 [0-9]+ image' "$D" | head -1)
    xdotool mousemove $(( $7 + 2 )) $(( $8 + 2 )) mousedown 1 mousemove $(( $7 + 10 )) $(( $8 + 8 )) mousemove $(( $7 + 50 )) $(( $8 + 40 )) mouseup 1
    wait_field undo 3 8 && pass "its corner handle resizes it" || fail "resize: undo '$(field undo)'"
else fail "no picture object"; fi
# the red line: pick it, Del
sleep 0.5
set -- $(grep -E '^obj 1 [0-9]+ path' "$D" | awk '$8 - $6 <= 4' | head -1)
if [ $# -ge 8 ]; then
    xdotool mousemove $(( ($5 + $7) / 2 )) $(( $6 + 1 )) click 1; sleep 0.5
    [ "$(field pick | cut -d' ' -f1)" = obj ] || fail "the line was not picked: '$(field pick)'"
    xdotool key Delete
    wait_field undo 4 8 && pass "a picked line is deleted with Del" || fail "delete: undo '$(field undo)'"
else fail "no line object"; fi
# Add text
tb addtext
set -- $(page_xy 420 190); xdotool mousemove "$1" "$2" click 1
if wait_line '^editor ' 5; then
    xdotool type --delay 15 "Added by the gate"
    set -- $(page_xy 560 40); xdotool mousemove "$1" "$2" click 1
    wait_field undo 5 8 && pass "Add text: a click, typing, a click elsewhere" || fail "add text: undo '$(field undo)'"
else fail "Add text opened no editor"; fi
sleep 1; shot edit-done
xdotool key ctrl+s
wait_field dirty 0 10 && pass "Ctrl+S saves" || fail "save: dirty '$(field dirty)' status '$(field status)'"
F="$DOCS/edit.pdf"
qpdf --check "$F" >/dev/null 2>&1 && pass "qpdf --check: the saved file is valid" || fail "qpdf --check failed"
t=$(probe text "$F")
case "$t" in *"Edited by the gate"*"inside the paragraph"*) pass "the new paragraph is in the file" ;; *) fail "text: $(echo "$t" | head -c 200)" ;; esac
case "$t" in *"quick brown"*) fail "the old paragraph is still in the file" ;; *) pass "the old paragraph's text is gone from the file" ;; esac
case "$t" in *"Added by the gate"*) pass "the added text is in the file" ;; *) fail "added text missing" ;; esac
set -- $(probe words "$F" 0 | awk '$5 == "Edited" || $5 == "wrap"' | awk '{print $1, $3}' | sort -n | tr '\n' ' ')
[ $# -ge 2 ] && [ "$1" -ge 70 ] && [ "$(echo "$@" | tr ' ' '\n' | sort -n | tail -1)" -le 405 ] && pass "the edited text stays within the paragraph's width" \
    || fail "edited words at: $*"
set -- $(probe images "$F" 0 | head -1)
[ $# -ge 4 ] && [ "$1" -gt 120 ] && [ "$(( $3 - $1 ))" -gt 110 ] && pass "the picture moved and grew ($1 $2 $3 $4)" || fail "picture at: $*"
[ "$(probe drawings "$F" 0)" = 1 ] && pass "the deleted line is gone, the box stays (1 path)" || fail "paths: $(probe drawings "$F" 0)"
xdotool key ctrl+z; wait_field undo 4 5; xdotool key ctrl+s; wait_field dirty 0 10
case "$(probe text "$F")" in *"Added by the gate"*) fail "Ctrl+Z did not undo the added text" ;; *) pass "Ctrl+Z undoes the added text (in the saved file)" ;; esac
xdotool key ctrl+y; wait_field undo 5 5; xdotool key ctrl+s; wait_field dirty 0 10
case "$(probe text "$F")" in *"Added by the gate"*) pass "Ctrl+Y redoes it" ;; *) fail "Ctrl+Y did not redo" ;; esac

# ============================================================= Comment
tb close
wait_field tool '0 none' 3 || fail "Close: tool '$(field tool)'"
pane comment
wait_field tool '2 comment' 5 && pass "Comment opens" || fail "tool '$(field tool)'"
tb highlight
set -- $(field 'word 1')
if [ $# -ge 5 ]; then
    y=$(( ($2 + $4) / 2 ))
    xdotool mousemove $(( $1 + 2 )) "$y" mousedown 1 mousemove $(( $1 + 30 )) "$y" mousemove $(( $3 + 60 )) "$y" mouseup 1
    wait_field listitems 1 8 && pass "dragging over text with Highlight makes a comment" || fail "highlight: listitems '$(field listitems)'"
fi
tb note
set -- $(page_xy 560 120); xdotool mousemove "$1" "$2" click 1
sleep 1.5; xdotool type "Gate sticky note"; xdotool key Tab Return
wait_field listitems 2 8 && pass "a sticky note from its dialog" || fail "note: listitems '$(field listitems)'"
tb rect
set -- $(page_xy 420 180); a=$1; b=$2; set -- $(page_xy 520 240)
xdotool mousemove "$a" "$b" mousedown 1 mousemove $((a + 20)) $((b + 20)) mousemove "$1" "$2" mouseup 1
wait_field listitems 3 8 && pass "a rectangle dragged out" || fail "rect: listitems '$(field listitems)'"
tb ink
set -- $(page_xy 100 170); a=$1; b=$2
xdotool mousemove "$a" "$b" mousedown 1 mousemove $((a + 20)) $((b + 10)) mousemove $((a + 40)) $((b - 10)) mousemove $((a + 70)) $((b + 15)) mouseup 1
wait_field listitems 4 8 && pass "a free-form drawing" || fail "ink: listitems '$(field listitems)'"
tb textbox
set -- $(page_xy 430 260); xdotool mousemove "$1" "$2" click 1
if wait_line '^editor ' 5; then
    xdotool type --delay 15 "Gate text box"
    set -- $(page_xy 560 40); xdotool mousemove "$1" "$2" click 1
    wait_field listitems 5 8 && pass "a text box typed in" || fail "text box: listitems '$(field listitems)'"
else fail "the text box opened no editor"; fi
sleep 1; shot comments
xdotool key ctrl+s; wait_field dirty 0 10
an=$(probe annots "$F")
for k in Highlight Text Square Ink FreeText; do
    echo "$an" | grep -q "^1 $k" && pass "saved as a $k annotation" || fail "no $k annotation: $an"
done
echo "$an" | grep -q "^1 Text Gate sticky note" && pass "the note's text is saved" || fail "note text: $an"
echo "$an" | grep -q "^1 FreeText Gate text box" && pass "the text box's text is saved" || fail "text box text: $an"
tb select
set -- $(grep '^annot 1 [0-9]* Square' "$D" | head -1)
if [ $# -ge 8 ]; then
    xdotool mousemove "$5" $(( ($6 + $8) / 2 )) click 1; sleep 0.5
    xdotool key Delete
    wait_field listitems 4 8 && pass "a picked comment is deleted with Del" || fail "delete comment: '$(field listitems)'"
    xdotool key ctrl+s; wait_field dirty 0 10
    probe annots "$F" | grep -q Square && fail "the rectangle is still in the file" || pass "the deleted comment is gone from the file"
else fail "no Square annotation on the screen"; fi
# closing with unsaved changes asks
tb highlight
set -- $(field 'word 1'); y=$(( ($2 + $4) / 2 ))
xdotool mousemove $(( $1 + 2 )) "$y" mousedown 1 mousemove $(( $3 + 20 )) "$y" mouseup 1
wait_field dirty 1 5
xdotool key ctrl+w; sleep 1.5; shot close-prompt; xdotool key Escape; sleep 1
[ "$(field pages)" = 4 ] && pass "closing with unsaved changes asks; Cancel keeps it open" || fail "after Cancel: pages '$(field pages)'"
xdotool key ctrl+w; sleep 1.5; xdotool key n
wait_field pages 0 5 && pass "No closes it without saving" || fail "after No: pages '$(field pages)'"
closeapp

# ============================================================= Fill & Sign
printf 'C:\\docs\\flat.pdf\n' > "$T/answers1"
launch 'C:\docs\form.pdf' form "$(wd "$T/answers1")"
wait_field pages 1 30 || fail "form.pdf did not open"
fitpage
wait_field form 1 5 && pass "the form's fields are known" || fail "form '$(field form)'"
set -- $(grep '^field 1 [0-9]* 0 ' "$D" | head -1)
if [ $# -ge 8 ]; then
    xdotool mousemove $(( ($5 + $7) / 2 )) $(( ($6 + $8) / 2 )) click 1
    if wait_line '^editor ' 5; then
        xdotool type --delay 15 "Jane Q Tester"; xdotool key Return
        wait_field undo 1 8 && pass "a text field typed in (no tool needed)" || fail "text field: undo '$(field undo)'"
    else fail "no editor on the text field"; fi
else fail "no text field on the screen"; fi
set -- $(grep '^field 1 [0-9]* 1 ' "$D" | head -1); xdotool mousemove $(( ($5 + $7) / 2 )) $(( ($6 + $8) / 2 )) click 1
wait_field undo 2 8 && pass "the check box ticked with a click" || fail "check box: undo '$(field undo)'"
set -- $(grep '^field 1 [0-9]* 2 ' "$D" | sed -n 2p); xdotool mousemove $(( ($5 + $7) / 2 )) $(( ($6 + $8) / 2 )) click 1
wait_field undo 3 8 && pass "the second radio button chosen" || fail "radio: undo '$(field undo)'"
set -- $(grep '^field 1 [0-9]* 3 ' "$D" | head -1); xdotool mousemove $(( ($5 + $7) / 2 )) $(( ($6 + $8) / 2 )) click 1
sleep 1; xdotool key Down Down Down Return
wait_field undo 4 8 && pass "a choice from the combo box's list" || fail "combo: undo '$(field undo)'"
xdotool key ctrl+s; wait_field dirty 0 10
fl=$(probe fields "$DOCS/form.pdf")
echo "$fl" | grep -q '^fullname=Jane Q Tester$' && pass "the text field's value is saved" || fail "fields: $fl"
echo "$fl" | grep -q '^agree=\(Yes\|On\|True\)' && pass "the check box is saved ticked" || fail "check box: $(echo "$fl" | grep agree)"
[ "$(echo "$fl" | grep '^size=' | sed -n 2p)" != "size=Off" ] && [ "$(echo "$fl" | grep '^size=' | sed -n 1p)" = "size=Off" ] \
    && pass "the second radio button is the one on" || fail "radio: $(echo "$fl" | grep size)"
echo "$fl" | grep -q '^colour=Blue$' && pass "the combo box's choice is saved" || fail "combo: $(echo "$fl" | grep colour)"
pane fill
wait_field tool '3 fill' 5 || fail "Fill & Sign: tool '$(field tool)'"
tb sign; sleep 1.5
shot signature
xdotool key ctrl+a; xdotool type --delay 15 "Jane Signer"; xdotool key Return
sleep 1
set -- $(page_xy 330 400); xdotool mousemove "$1" "$2" click 1
wait_field undo 5 8 && pass "a typed signature placed with a click" || fail "signature: undo '$(field undo)'"
tb flatten; sleep 1; xdotool key Return
wait_field form 0 8 && pass "Flatten (confirmed) leaves no fields" || fail "flatten: form '$(field form)'"
sleep 1; shot filled
xdotool key ctrl+shift+s
wait_field file 'C:\docs\flat.pdf' 10 && wait_field dirty 0 5 && pass "Save As writes flat.pdf" || fail "save as: file '$(field file)'"
t=$(probe text "$DOCS/flat.pdf")
case "$t" in *"Jane Q Tester"*) pass "the flattened value is page text" ;; *) fail "flat text: $t" ;; esac
case "$t" in *"Jane Signer"*) pass "the signature is on the page" ;; *) fail "signature text: $t" ;; esac
[ -z "$(probe fields "$DOCS/flat.pdf")" ] && pass "flat.pdf has no fields" || fail "flat.pdf fields: $(probe fields "$DOCS/flat.pdf")"
closeapp

# ============================================================= Redact
launch 'C:\docs\red.pdf' red
wait_field pages 2 30 || fail "red.pdf did not open"
wait_line '^page 1 .* 1$' 15
fitpage
pane redact
wait_field tool '4 redact' 5 && pass "Redact opens" || fail "tool '$(field tool)'"
tb findtext; sleep 1.5
xdotool type TOPSECRET; xdotool key Return
wait_field redactions 2 8 && pass "Find text marks TOPSECRET on both pages" || fail "find: redactions '$(field redactions)'"
tb findtext; sleep 1.5
xdotool key alt+p Tab Down Down Return
wait_field redactions 3 8 && pass "the social security number pattern is marked" || fail "pattern: redactions '$(field redactions)'"
# drag over the phone number's digits (text marks)
set -- $(page_xy 118 108); a=$1; b=$2; set -- $(page_xy 190 108)
xdotool mousemove "$a" "$b" mousedown 1 mousemove $((a + 10)) "$b" mousemove "$1" "$2" mouseup 1
wait_field redactions 4 8 && pass "dragging over text marks it" || fail "text mark: redactions '$(field redactions)'"
tb markarea
set -- $(page_xy 428 528); a=$1; b=$2; set -- $(page_xy 472 572)
xdotool mousemove "$a" "$b" mousedown 1 mousemove $((a + 10)) $((b + 10)) mousemove "$1" "$2" mouseup 1
wait_field redactions 5 8 && pass "an area over the picture is marked" || fail "area: redactions '$(field redactions)'"
sleep 1; shot redact-marked
tb apply; sleep 1.5; shot redact-confirm; xdotool key Return
sleep 2; xdotool key Return
wait_field redactions 0 10 && pass "Apply (confirmed) removes the marks' content" || fail "apply: redactions '$(field redactions)'"
sleep 1; xdotool key ctrl+s; wait_field dirty 0 10 && pass "saved" || fail "save: '$(field status)'"
sleep 1; shot redact-applied
F="$DOCS/red.pdf"
qpdf --check "$F" >/dev/null 2>&1 && pass "qpdf --check: the redacted file is valid" || fail "qpdf --check failed"
t=$(probe text "$F")
for w in TOPSECRET 123-45-6789 4567; do
    case "$t" in *"$w"*) fail "pdftotext still reads $w" ;; *) pass "pdftotext cannot read $w" ;; esac
done
case "$t" in *"Keep this line"*"keep this too"*) pass "the words around the mark stay" ;; *) fail "text: $t" ;; esac
left=$(probe raw "$F" TOPSECRET 123-45-6789 4567 LAYERSECRET HIDDENTEXT SECRETMETA ATTACHSECRET JSSECRET | tr '\n' ' ')
[ -z "$left" ] && pass "no stream or object of the file holds a secret (redacted text, hidden layer, invisible text, metadata, attachment, JavaScript)" \
    || fail "the raw file still has: $left"
rp=$(probe redpixels "$F")
[ "$rp" -ge 0 ] 2>/dev/null && [ "$rp" -lt 60 ] && pass "the picture's red pixels under the mark are gone from the image ($rp of 900 left)" || fail "red pixels: $rp"
closeapp

# ============================================================= Organize
launch 'C:\docs\org.pdf' org
wait_field pages 4 30 || fail "org.pdf did not open"
pane organize
wait_line '^org 4 ' 8 && pass "Organize Pages shows the pages" || fail "organize: $(grep -c '^org ' "$D")"
set -- $(field 'org 2'); xdotool mousemove $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 )) click 1; sleep 0.5
tb rotr
wait_field undo 1 8 && pass "page 2 rotated clockwise" || fail "rotate: undo '$(field undo)'"
set -- $(field 'org 3'); xdotool mousemove $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 )) click 1; sleep 0.5
tb delete
wait_field pages 3 8 && pass "page 3 deleted" || fail "delete: pages '$(field pages)'"
sleep 0.5
set -- $(field 'org 3'); a=$(( ($1 + $3) / 2 )); b=$(( ($2 + $4) / 2 )); xdotool mousemove "$a" "$b" click 1; sleep 1.2
set -- $(field 'org 1')
xdotool mousemove "$a" "$b" mousedown 1 mousemove $((a - 20)) "$b" mousemove $(( $1 + 10 )) $(( ($2 + $4) / 2 )) mouseup 1
wait_field undo 3 8 && pass "the last page dragged to the front" || fail "drag: undo '$(field undo)'"
tb blank
wait_field pages 4 8 && pass "a blank page inserted" || fail "blank: pages '$(field pages)'"
sleep 1; shot organize
xdotool key ctrl+s; wait_field dirty 0 10
pg=$(probe pages "$DOCS/org.pdf")
echo "$pg" | sed -n 1p | grep -q '^1 0 Page 4 marker' && echo "$pg" | sed -n 2p | grep -q '^2 0 *$' \
    && echo "$pg" | sed -n 3p | grep -q '^3 0 The quick' && echo "$pg" | sed -n 4p | grep -q '^4 90 Page 2 marker' \
    && pass "the saved pages: 4, blank, 1, 2 turned a quarter" || fail "pages: $(echo "$pg" | tr '\n' '|')"
closeapp

# ============================================================= Export, Combine, Protect
printf 'C:\\docs\\out.docx\nC:\\docs\\form.pdf|C:\\docs\\pic.png\nC:\\docs\\combined.pdf\n' > "$T/answers2"
launch 'C:\docs\prot.pdf' prot "$(wd "$T/answers2")"
wait_field pages 4 30 || fail "prot.pdf did not open"
pane export; sleep 1.5; shot export; xdotool key Return
i=0; while [ ! -s "$DOCS/out.docx" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
if $PY -c "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); d=z.read('word/document.xml').decode(); sys.exit(0 if 'quick brown fox' in d and 'Page 4 marker' in d and z.testzip() is None else 1)" "$DOCS/out.docx" 2>/dev/null
then pass "Export to Word writes a .docx with the text"; else fail "no valid out.docx"; fi
pane combine; sleep 1.5
xdotool key alt+a; sleep 1.5
shot combine; xdotool key Return
if wait_field file 'C:\docs\combined.pdf' 15 && wait_field pages 6 10; then pass "Combine Files makes combined.pdf (4 + 1 + 1 pages) and opens it"
else fail "combine: file '$(field file)' pages '$(field pages)'"; fi
qpdf --check "$DOCS/combined.pdf" >/dev/null 2>&1 && pass "qpdf --check: the combined file is valid" || fail "combined.pdf invalid"
pane protect; sleep 1.5
xdotool key Tab; xdotool type "gate pw"; xdotool key Tab; xdotool type "gate pw"; shot protect; xdotool key Return
wait_field protect aes256 8 && pass "Protect: the password waits for the save" || fail "protect: '$(field protect)'"
xdotool key ctrl+s; wait_field dirty 0 10
set -- $(probe info "$DOCS/combined.pdf" "gate pw")
[ "${2:-}" = AESv3 ] && pass "the saved file is AES-256 encrypted (qpdf: AESv3)" || fail "encryption: $*"
[ -z "$(probe text "$DOCS/combined.pdf")" ] && pass "without the password its text cannot be read" || fail "readable without the password"
case "$(probe text "$DOCS/combined.pdf" "gate pw")" in *"quick brown"*) pass "with it, it can" ;; *) fail "not readable with the password" ;; esac
closeapp
launch 'C:\docs\combined.pdf' prot2
sleep 4; shot password; xdotool type "gate pw"; xdotool key Return
wait_field pages 6 15 && [ "$(field encrypted)" = 1 ] && pass "opening it again asks for the password, and it opens" || fail "reopen: pages '$(field pages)' error '$(field error)'"
closeapp

# ============================================================= Dark mode
reg 'HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' /v AppsUseLightTheme /t REG_DWORD /d 0
launch 'C:\docs\edit.pdf' dark
wait_field pages 4 30 || fail "edit.pdf did not open (dark)"
pane redact; sleep 1
wait_field dark 1 5 && pass "the app mode is dark" || fail "dark '$(field dark)'"
shot dark
set -- $(field 'button sidebar'); by=$2
set -- $(field view)
m=$(mean "$OUT/pdf-editor-dark.png" "$1" $((by - 15)) "$3" $((by + 15)))
[ "$m" -le 30 ] 2>/dev/null && pass "the toolbar is drawn dark ($m% bright)" || fail "dark toolbar: $m%"
closeapp
reg 'HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' /v AppsUseLightTheme /t REG_DWORD /d 1

[ $RC = 0 ] && echo "pdf-editor-check: all passed" || echo "pdf-editor-check: FAILED"
exit $RC
