#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for SG PDF's professional tools (sg-pdf64.exe with sg-session's
# sg-pdf), driven like a person on the X mouse and keyboard, on a shell
# desktop under Xvfb; every result is read back from the saved file from
# outside SG PDF (test/pdf-pro-probe.py: MuPDF, pdftotext; poppler's pdfsig
# checks the signatures):
#
#   form      Prepare Form: Auto-detect makes the paper form's blanks into
#             12 fields (a date field, a signature field, check boxes,
#             number cells); a text field drawn with the mouse; its
#             Properties dialog names it Total, a currency format and the
#             formula Qty * Price + Qty_2 * Price_2; saved as the standard
#             scripts (AFNumber_Format, BVCALC ...)
#   fill      the form filled with the mouse and keyboard: the total
#             computes itself (3723.5), the date typed as words is kept as
#             10/06/2026, other readers see $3,723.50; a check mark placed
#             where there is no field
#   sign      a click on the empty signature field signs it with a digital
#             ID (its dialog: file, password, reason), saved as a new file;
#             a second signature in a box drawn on the page; pdfsig: both
#             valid, the second covers the whole file; the bar over the
#             pages and the Signatures pane say so
#   create    Create PDF from a picture, a text file and a PDF: an untitled
#             document of 3 pages, saved where asked
#   ocr       Recognize Text: the scanned page's words become text
#   decorate  a footer with page numbers, a watermark and Bates numbers on
#             every page (Edit PDF's bar)
#   view      Two Page View shows pages side by side, Single Page View one
#             page at a time (Page Down turns it), Dark Pages draws them dark
#   attach    Attach a File: listed in the Attachments pane, saved in the file
#   stamp     an Approved stamp placed from Comment's bar
#   frame     the familiar editor's frame: with no document, Home (13 tool
#             cards, recent files); a card asks for a file and opens its
#             tool; the Home and document tabs switch; every tool card opens
#             its tool or its dialog; the quick tools rail (select, comment,
#             highlight, draw, text, sign) sets each; the floating page
#             controls turn the page and zoom; the Menu button and Alt+F open
#             the menu; Ctrl+0 fit page, Ctrl+2 fit width, Ctrl+Shift+Plus
#             rotates, Ctrl+Tab goes Home and back; screenshots light and dark
#   scale     at 200% (LogPixels 192) the frame is drawn twice as large
#   more      a link made with Edit PDF's Link; a sticky note's reply and
#             status (Accepted) from the comments list; Organize's Replace
#             (page 2 replaced by another PDF's page)
#
# SG_PDF_PRO_ONLY="form fill ..." runs some sections (mutation tests).
# Screenshots: build/pdf-pro-*.png. SG_PDF_EXE tests another build, SG_PDF_HELPER
# another sg-pdf. Skips (77) without wine-sg, Xvfb, xdotool, ImageMagick,
# pdftotext, pdfsig, python3-pymupdf.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_PDF_EXE:-$HERE/build/sg-pdf64.exe}"
OUT="$HERE/build"
PY=/usr/bin/python3
PROBE="$HERE/test/pdf-pro-probe.py"
ONLY="${SG_PDF_PRO_ONLY:-frame form fill sign create ocr decorate view attach stamp more scale}"
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
want() { case " $ONLY " in *" $1 "*) return 0 ;; esac; return 1; }

HELPER="${SG_PDF_HELPER:-}"
if [ -z "$HELPER" ]; then
    for h in "$HERE/../sg-session/bin/sg-pdf" /usr/bin/sg-pdf; do [ -x "$h" ] && { HELPER=$(readlink -f "$h"); break; }; done
fi
for need in Xvfb xdotool import pdftotext pdfsig; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
$PY -c 'import pymupdf' 2>/dev/null || { echo "SKIP: python3-pymupdf missing"; exit 77; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
[ -n "$HELPER" ] && [ -x "$HELPER" ] || { echo "SKIP: sg-pdf (sg-session) not found"; exit 77; }
printf 'formdetect\nquit\n' | "$HELPER" --serve 2>/dev/null | grep -q 'ERR notopen' || { echo "SKIP: $HELPER has no Prepare Form (sg-session too old)"; exit 77; }

T=$(mktemp -d /var/tmp/sg-pdf-pro.XXXXXX); chmod 755 "$T"
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
wineserver -w

DOCS="$WINEPREFIX/drive_c/docs"; mkdir -p "$DOCS" "$WINEPREFIX/drive_c/dumps"
$PY "$PROBE" make "$DOCS" || { fail "could not make the documents"; exit 1; }
probe() { $PY "$PROBE" "$@"; }
# pdfsig's certificate database, in the gate's HOME
mkdir -p "$HOME/.pki/nssdb"
command -v certutil >/dev/null && certutil -N -d "sql:$HOME/.pki/nssdb" --empty-password >/dev/null 2>&1
# a digital ID, made by the engine
printf 'makeid\t%s\tgate pw\tname=Gate Signer\temail=gate@example.org\nquit\n' "$DOCS/gate.pfx" | "$HELPER" --serve | grep -q '^OK' \
    || fail "could not make the digital ID"

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
count() { grep -cE "$1" "$D" 2>/dev/null; }
wait_count() {  # regex n seconds
    i=0
    while [ "$(count "$1")" != "$2" ] && [ $i -lt $(( $3 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    [ "$(count "$1")" = "$2" ]
}
shot() { sleep 0.5; import -window root "$OUT/pdf-pro-$1.png" 2>/dev/null; }
wd() { wine winepath -w "$1" 2>/dev/null | tr -d '\r'; }
tb() { set -- $(grep "^tbtn $1 " "$D" | head -1); [ $# -ge 4 ] && xdotool mousemove "$3" "$4" click 1; sleep 0.8; }
pane() { set -- $(grep "^panebtn $1 " "$D" | head -1); [ $# -ge 4 ] && xdotool mousemove "$3" "$4" click 1; sleep 1; }
homecard() { wait_line "^homecard $1 " 5; set -- $(grep "^homecard $1 " "$D" | head -1); [ $# -ge 4 ] && xdotool mousemove "$3" "$4" click 1; sleep 1; }
dtab() { set -- $(grep "^doctab $1 " "$D" | head -1); [ $# -ge 4 ] && xdotool mousemove "$3" "$4" click 1; sleep 0.8; }
rail() { set -- $(grep "^railbtn $1 " "$D" | head -1); [ $# -ge 4 ] && xdotool mousemove "$3" "$4" click 1; sleep 0.8; }
flt() { set -- $(grep "^floatbtn $1 " "$D" | head -1); [ $# -ge 4 ] && xdotool mousemove "$3" "$4" click 1; sleep 0.8; }
page_xy() {  # X Y [PAGE] -> screen x y of the point (points) on the page
    set -- "$1" "$2" $(field "page ${3:-1}") 0 0 0 0
    python3 -c "l,t,r,b=$3,$4,$5,$6; s=(r-l)/612.0; print(max(0,int(l+$1*s)), max(0,int(t+$2*s)))"
}
# a form field's centre on the screen, by its name (the dump's "field" lines)
field_xy() {
    set -- $(grep -E "^field [0-9]+ [0-9]+ -?[0-9]+ -?[0-9]+ -?[0-9]+ -?[0-9]+ -?[0-9]+ $1=" "$D" | head -1)
    [ $# -ge 8 ] && echo "$(( ($5 + $7) / 2 )) $(( ($6 + $8) / 2 ))"
}
field_value() { grep -E "^field .* $1=" "$D" | head -1 | sed "s/.* $1=//"; }
fill_in() {  # NAME TEXT: click the field, type, Tab
    xy=$(field_xy "$1")
    [ -n "$xy" ] || { fail "no field $1 on the screen"; return; }
    # shellcheck disable=SC2086
    xdotool mousemove $xy click 1
    wait_line '^editor ' 5 || { fail "no editor on field $1"; return; }
    xdotool type --delay 20 "$2"; xdotool key Tab
    i=0; while grep -q '^editor ' "$D" && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
    sleep 0.5
}
launch() {  # FILE DUMP [ANSWERS]
    D="$WINEPREFIX/drive_c/dumps/$2"
    rm -f "$D"
    SG_PDF_DUMP="$(wd "$D")" SG_PDF_FILE_ANSWERS="${3:-}" wine "$EXE" ${1:+"$1"} >/dev/null 2>&1 &
    sleep 2
}
closeapp() { wine taskkill /f /im sg-pdf64.exe >/dev/null 2>&1; sleep 1.5; }
fitpage() { xdotool key ctrl+backslash; wait_field fit page 5 || fail "fit page: '$(field fit)'"; sleep 1; }
answers() { f="$T/answers-$1"; shift; : > "$f"; for a in "$@"; do printf '%s\n' "$a" >> "$f"; done; wd "$f"; }
dialog() {  # TITLE: wait for SG PDF's dialog of that name in front (the dump's "dialog" line)
    wait_line "^dialog .*$1" 10 && sleep 0.4
}
F="$DOCS/flat.pdf"

# ============================================================= the frame: Home, tabs, rail, floating controls, Menu
if want frame; then
    L="$DOCS/long.pdf"
    launch "" frame "$(answers frame "$(wd "$L")")"
    wait_field bridged 1 30 || fail "SG PDF did not start"
    wait_field home 1 10 && pass "with no document, Home is shown" || fail "home '$(field home)'"
    [ "$(count '^homecard ')" = 13 ] && pass "13 tool cards on Home" || fail "cards: $(count '^homecard ')"
    shot home-light
    homecard organize
    wait_field pages 6 30 && wait_field tool '5 organize' 10 && pass "the Organize Pages card asks for a file and opens the tool" \
        || fail "organize card: pages '$(field pages)' tool '$(field tool)'"
    [ "$(field home)" = 0 ] && pass "the document's tab is in front" || fail "home '$(field home)'"
    tb close
    dtab home
    wait_field home 1 5 && pass "the Home tab shows Home" || fail "Home tab: home '$(field home)'"
    grep -q '^recent 0 .*long.pdf$' "$D" && pass "long.pdf is the first recent file" || fail "recent: $(grep '^recent' "$D")"
    dtab doc
    wait_field home 0 5 && pass "the document's tab shows the document" || fail "doc tab: home '$(field home)'"
    for c in edit:1 comment:2 fill:3 redact:4 organize:5 form:6; do
        k=${c%%:*}; n=${c#*:}
        dtab home; homecard "$k"
        if [ "$(field tool | cut -d' ' -f1)" = "$n" ] && [ "$(field home)" = 0 ]; then pass "the $k card opens its tool"
        else fail "card $k: tool '$(field tool)' home '$(field home)'"; fi
        tb close
    done
    for c in export:Export protect:Protect ocr:Recognize compress:Reduce certificates:Certificate combine:Combine; do
        k=${c%%:*}; t=${c#*:}
        dtab home; homecard "$k"
        if dialog "$t"; then pass "the $k card opens its dialog"; else fail "card $k: no '$t' dialog ($(field dialog))"; fi
        xdotool key Escape; sleep 0.8
        dtab doc
    done
    wait_field home 0 5 || dtab doc
    s1=$(grep '^railbtn select ' "$D" | cut -d' ' -f4); s2=$(grep '^railbtn comment ' "$D" | cut -d' ' -f4)
    echo "$(( s2 - s1 ))" > "$T/railstep"
    for r in comment:2:3 highlight:2:4 draw:2:12 text:3:13; do
        k=${r%%:*}; rest=${r#*:}; tl=${rest%%:*}; sb=${rest#*:}
        rail "$k"
        if [ "$(field tool | cut -d' ' -f1)" = "$tl" ] && [ "$(field sub)" = "$sb" ] && grep -q "^railbtn $k .* 1$" "$D"; then
            pass "the rail's $k sets tool $tl, $sb"
        else fail "rail $k: tool '$(field tool)' sub '$(field sub)'"; fi
    done
    rail sign
    if dialog "Signature"; then pass "the rail's sign asks for the signature"; xdotool key Escape; sleep 0.5; else fail "rail sign: no dialog"; fi
    rail select
    [ "$(field tool)" = "0 none" ] && pass "the rail's select closes the tool" || fail "rail select: tool '$(field tool)'"
    shot doc-light
    c0=$(field current)
    flt next
    wait_field current $(( c0 + 1 )) 5 && pass "the floating Next goes to page $(( c0 + 1 ))" || fail "float next: current '$(field current)'"
    z0=$(field zoom); flt zoomin; sleep 0.5
    [ "$(field zoom)" -gt "$z0" ] 2>/dev/null && pass "the floating zoom in: $z0% -> $(field zoom)%" || fail "float zoom: $z0 -> $(field zoom)"
    xdotool key ctrl+0; wait_field fit page 5 && pass "Ctrl+0: fit page" || fail "Ctrl+0: fit '$(field fit)'"
    xdotool key ctrl+2; wait_field fit width 5 && pass "Ctrl+2: fit width" || fail "Ctrl+2: fit '$(field fit)'"
    xdotool key ctrl+shift+plus; wait_field rot 90 5 && pass "Ctrl+Shift+Plus rotates the view" || fail "rot '$(field rot)'"
    xdotool key ctrl+shift+minus; wait_field rot 0 5 || fail "Ctrl+Shift+Minus: rot '$(field rot)'"
    xdotool key ctrl+Tab; wait_field home 1 5 && pass "Ctrl+Tab: Home" || fail "Ctrl+Tab: home '$(field home)'"
    xdotool key ctrl+Tab; wait_field home 0 5 || fail "Ctrl+Tab back: home '$(field home)'"
    dtab menu
    wait_field menuopen 1 5 && pass "the Menu button opens the menu" || fail "menuopen '$(field menuopen)'"
    shot menu
    xdotool key Escape; wait_field menuopen 0 5 || { xdotool key Escape; sleep 0.5; }
    xdotool key alt+f
    wait_field menuopen 1 5 && pass "Alt+F opens the File menu" || fail "Alt+F: menuopen '$(field menuopen)'"
    xdotool key Escape; sleep 0.5
    closeapp
    # dark
    reg 'HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' /v AppsUseLightTheme /t REG_DWORD /d 0
    launch "" framedark
    wait_field home 1 20 && wait_field dark 1 5 && pass "dark: Home" || fail "dark home: home '$(field home)' dark '$(field dark)'"
    shot home-dark
    set -- $(grep '^recent 0 ' "$D" | head -1); [ $# -ge 4 ] && xdotool mousemove "$3" "$4" click 1
    wait_field pages 6 30 && pass "a recent file opens with a click" || fail "recent: pages '$(field pages)'"
    pane comment; sleep 1
    shot doc-dark
    closeapp
    reg 'HKCU\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize' /v AppsUseLightTheme /t REG_DWORD /d 1
fi

# ============================================================= Prepare Form
if want form; then
    launch "$(wd "$F")" form
    wait_field pages 1 40 || { fail "flat.pdf did not open: '$(field error)'"; exit 1; }
    wait_line '^page 1 .* 1$' 15 || fail "page 1 not rendered"
    fitpage
    pane form
    wait_field tool '6 form' 5 && pass "Prepare Form opens from the tools pane" || fail "tool '$(field tool)'"
    tb detect
    if wait_count '^formfield ' 12 15; then pass "Auto-detect: 12 fields from the paper form's blanks"
    else fail "Auto-detect made $(count '^formfield ') fields"; fi
    grep -q 'fmt=date:mm/dd/yyyy calc=- name=Date of birth$' "$D" && pass "the blank after 'Date of birth' is a date field" || fail "no date field: $(grep 'Date of birth' "$D")"
    grep -qE '^formfield 1 [0-9]+ 6 .* name=Signature$' "$D" && pass "the rule after 'Signature' is a signature field" || fail "no signature field"
    [ "$(grep -cE '^formfield 1 [0-9]+ 1 .* name=(Student|Senior)$' "$D")" = 2 ] && pass "the small squares are check boxes named by their labels" || fail "check boxes: $(grep -c '^formfield 1 [0-9]* 1 ' "$D")"
    grep -q 'fmt=number:0' "$D" && grep -q 'name=Qty_2$' "$D" && pass "the table's Qty cells are number fields, the second row named Qty_2" || fail "table cells: $(grep Qty "$D")"
    shot form-detect
    tb ftext
    set -- $(page_xy 380 400); a="$1 $2"; set -- $(page_xy 470 420)
    # shellcheck disable=SC2086
    xdotool mousemove $a mousedown 1 mousemove $(( $1 - 30 )) $(( $2 - 5 )) mousemove "$1" "$2" mouseup 1
    if wait_count '^formfield ' 13 8; then pass "a text field drawn with the mouse"; else fail "drawn field: $(count '^formfield ') fields"; fi
    [ "$(field formpick)" != "-1" ] && pass "the new field is selected" || fail "formpick '$(field formpick)'"
    xdotool key Return
    if dialog "Field Properties"; then
        pass "Enter opens the field's Properties"
        shot form-props
        xdotool key ctrl+a; xdotool type --delay 20 "Total"
        xdotool key alt+f; sleep 0.3; xdotool type "c"; sleep 0.3
        xdotool key alt+c; sleep 0.3; xdotool type "f"; sleep 0.3
        xdotool key Tab; sleep 0.2; xdotool type --delay 20 "Qty * Price + Qty_2 * Price_2"
        xdotool key Return
        wait_line 'fmt=number:2:\$:0 calc=expr:Qty \* Price \+ Qty_2 \* Price_2 name=Total$' 8 \
            && pass "named Total, currency format, the formula" || fail "Total: $(grep 'formfield 1 [0-9]* 0 .*name=Text\|name=Total' "$D")"
    else fail "no Properties dialog"; fi
    xdotool key ctrl+s
    wait_field dirty 0 10 && pass "Ctrl+S saves the form" || fail "save: dirty '$(field dirty)'"
    fl=$(probe fields "$F")
    [ "$(echo "$fl" | grep -c .)" = 13 ] && pass "13 fields in the saved file" || fail "fields in the file: $(echo "$fl" | grep -c .)"
    echo "$fl" | grep -q '^Total|Text|0|AFNumber_Format(2, 0, 0, 0, "\$", true);|/\*\* BVCALC Qty \* Price + Qty_2 \* Price_2 EVCALC' \
        && pass "the total's format and formula are the standard scripts" || fail "Total in the file: $(echo "$fl" | grep '^Total')"
    echo "$fl" | grep -q '^Date of birth|Text||AFDate_FormatEx("mm/dd/yyyy");' && pass "the date field's format is AFDate_FormatEx" || fail "date field: $(echo "$fl" | grep '^Date')"
    closeapp
fi

# ============================================================= Fill
if want fill; then
    d0=$(probe drawings "$F" 0)
    launch "$(wd "$F")" fill
    wait_field pages 1 40 || fail "the form did not open"
    wait_line '^field ' 10 || fail "no fields"
    fitpage
    fill_in Qty 3
    fill_in Price 1234.5
    fill_in Qty_2 2
    fill_in Price_2 10
    if [ "$(field_value Total)" = "3723.5" ]; then pass "the total computes itself: 3*1234.5 + 2*10 = 3723.5"
    else fail "Total = '$(field_value Total)'"; fi
    fill_in "Date of birth" "oct 6 2026"
    [ "$(field_value 'Date of birth')" = "10/06/2026" ] && pass "a date typed as words is kept as 10/06/2026" || fail "date = '$(field_value 'Date of birth')'"
    fill_in Qty abc
    [ "$(field_value Qty)" = "3" ] && pass "letters in a number field are refused" || fail "Qty = '$(field_value Qty)'"
    pane fill
    wait_field tool '3 fill' 5 || fail "Fill & Sign did not open: '$(field tool)'"
    tb check
    set -- $(page_xy 300 196); xdotool mousemove "$1" "$2" click 1
    wait_field undo 6 8 && pass "a check mark is placed where there is no field" || fail "check mark: undo '$(field undo)'"
    shot fill
    xdotool key ctrl+s
    wait_field dirty 0 10 && pass "saved" || fail "save: dirty '$(field dirty)'"
    lay=$(probe layout "$F")
    case "$lay" in *'$3,723.50'*'10/06/2026'*|*'10/06/2026'*'$3,723.50'*) pass "other readers see \$3,723.50 and 10/06/2026" ;;
        *) case "$lay" in *'$3,723.50'*) fail "the date is not shown: $(echo "$lay" | head -12)" ;; *) fail "the total is not shown formatted: $(echo "$lay" | head -12)" ;; esac ;; esac
    [ "$(probe drawings "$F" 0)" -gt "$d0" ] && pass "the check mark is drawn on the page" || fail "drawings: $d0 -> $(probe drawings "$F" 0)"
    closeapp
fi

# ============================================================= Sign
if want sign; then
    S1="$DOCS/signed.pdf"; S2="$DOCS/signed2.pdf"
    launch "$(wd "$F")" sign "$(answers sign "$(wd "$S1")" "$(wd "$S2")")"
    wait_field pages 1 40 || fail "the form did not open"
    wait_line '^field ' 10 || fail "no fields"
    fitpage
    xy=$(field_xy Signature)
    # shellcheck disable=SC2086
    [ -n "$xy" ] && xdotool mousemove $xy click 1
    if dialog "Sign with a Certificate"; then
        pass "a click on the empty signature field asks for the digital ID"
        xdotool type --delay 10 'C:\docs\gate.pfx'
        xdotool key alt+p; sleep 0.3; xdotool type --delay 20 "gate pw"
        xdotool key alt+r; sleep 0.3; xdotool type --delay 20 "Approved by the gate"
        sleep 0.5; shot sign-dialog
        xdotool key Return
        wait_field title 'signed.pdf - SG PDF' 20 && pass "signed and saved as signed.pdf" || fail "title '$(field title)' status '$(field status)'"
    else fail "no certificate dialog"; fi
    out=$(pdfsig "$S1" 2>&1)
    case "$out" in *"Signature is Valid"*) pass "pdfsig: the signature is valid" ;; *) fail "pdfsig: $out" ;; esac
    case "$out" in *"Gate Signer"*"Total document signed"*) pass "pdfsig: signed by Gate Signer, the whole document" ;; *) fail "pdfsig details: $out" ;; esac
    wait_line '^sigbanner 1 Signed, and the signature is valid' 5 && pass "the bar over the pages: valid, the identity not confirmed (self-signed)" || fail "banner: $(grep sigbanner "$D")"
    pane fill
    tb certsign
    if dialog "Sign with a Certificate"; then
        xdotool type --delay 20 "gate pw"; xdotool key Return
        sleep 1
        set -- $(page_xy 320 600); a="$1 $2"; set -- $(page_xy 540 660)
        # shellcheck disable=SC2086
        xdotool mousemove $a mousedown 1 mousemove $(( $1 - 20 )) $(( $2 - 5 )) mousemove "$1" "$2" mouseup 1
        wait_field title 'signed2.pdf - SG PDF' 20 && pass "a second signature in a box drawn on the page" || fail "second: title '$(field title)' status '$(field status)'"
    else fail "no certificate dialog the second time"; fi
    out=$(pdfsig "$S2" 2>&1)
    [ "$(echo "$out" | grep -c 'Signature is Valid')" = 2 ] && pass "pdfsig: both signatures are valid" || fail "pdfsig: $out"
    [ "$(echo "$out" | grep -c 'Total document signed')" = 1 ] && pass "pdfsig: the second covers the whole file, the first its revision" || fail "coverage: $out"
    set -- $(field sigbar) x x; xdotool mousemove "$1" "$2" click 1; sleep 1
    wait_field side signatures 5 && pass "the bar opens the Signatures pane" || fail "side '$(field side)'"
    [ "$(count '^sig 1 [0-9]+ 2 ')" = 2 ] && pass "two signatures listed: valid, identity unknown" || fail "sigs: $(grep '^sig ' "$D")"
    shot sign
    closeapp
fi

# ============================================================= Create
if want create; then
    C="$DOCS/created.pdf"
    launch "" create "$(answers create "$(wd "$DOCS/scan.png")|$(wd "$DOCS/notes.txt")|$(wd "$F")" "$(wd "$C")")"
    wait_field bridged 1 30 || fail "SG PDF did not start"
    wait_field home 1 10 || fail "no document: Home is not shown"
    homecard create
    if wait_field pages 3 30; then pass "Create PDF from a picture, a text file and a PDF: 3 pages"
    else fail "create: pages '$(field pages)' status '$(field status)'"; fi
    [ "$(field untitled)" = 1 ] && pass "the new document is untitled" || fail "untitled '$(field untitled)'"
    xdotool key ctrl+s
    wait_field untitled 0 10 && wait_field dirty 0 5 && pass "Save asks where, and saves" || fail "save: untitled '$(field untitled)' dirty '$(field dirty)'"
    t=$(probe text "$C")
    case "$t" in *"Plain text notes for the gate"*"Membership Application"*) pass "the text file and the PDF are in it" ;; *) fail "created: $t" ;; esac
    shot create
    closeapp
fi

# ============================================================= Recognize text
if want ocr; then
    O="$DOCS/ocr.pdf"
    if command -v ocrmypdf >/dev/null && tesseract --list-langs 2>/dev/null | grep -qx eng; then
        launch "" ocr "$(answers ocr "$(wd "$DOCS/scan.png")" "$(wd "$O")")"
        wait_field bridged 1 30
        wait_field home 1 10; homecard create; wait_field pages 1 30 || fail "the scan did not open"
        [ -z "$(probe text "$O" 2>/dev/null)" ] || true
        pane ocr
        if dialog "Recognize Text"; then
            xdotool key Return
            wait_field status 'Text recognized.' 120 && pass "Recognize Text ran" || fail "ocr: status '$(field status)'"
        else fail "no Recognize Text dialog"; fi
        xdotool key ctrl+s; wait_field dirty 0 10
        case "$(probe text "$O")" in *"Invoice number 48213"*) pass "the scanned page's words are text now" ;; *) fail "ocr text: $(probe text "$O")" ;; esac
        closeapp
    else echo "SKIP  ocr: ocrmypdf or English not installed"; fi
fi

# ============================================================= Header, footer, watermark, Bates
if want decorate; then
    L="$DOCS/long.pdf"
    launch "$(wd "$L")" deco
    wait_field pages 6 40 || fail "long.pdf did not open"
    pane edit
    tb headfoot
    if dialog "Header"; then xdotool key Return; wait_field undo 1 10 && pass "a footer with page numbers" || fail "footer: undo '$(field undo)'"; else fail "no header/footer dialog"; fi
    tb watermark
    if dialog "Watermark"; then xdotool key Return; wait_field undo 2 10 && pass "a watermark" || fail "watermark: undo '$(field undo)'"; else fail "no watermark dialog"; fi
    tb bates
    if dialog "Bates"; then
        xdotool type --delay 20 "ACME-"; xdotool key Return
        wait_field undo 3 10 && pass "Bates numbers" || fail "bates: undo '$(field undo)'"
    else fail "no Bates dialog"; fi
    sleep 1; shot decorate
    xdotool key ctrl+s; wait_field dirty 0 10
    t=$(probe text "$L" 4)
    case "$t" in *"Page 4 of 6"*) pass "page 4 says 'Page 4 of 6'" ;; *) fail "page 4: $t" ;; esac
    case "$t" in *"ACME-000004"*) pass "page 4's Bates number is ACME-000004" ;; *) fail "bates on page 4: $t" ;; esac
    case "$(echo "$t" | tr -d ' ')" in *CONFIDENTIAL*) pass "page 4 has the watermark" ;; *) fail "watermark on page 4: $t" ;; esac
    closeapp
fi

# ============================================================= Page display, dark pages
if want view; then
    launch "$(wd "$DOCS/long.pdf")" view
    wait_field pages 6 40 || fail "long.pdf did not open"
    wait_line '^page 1 .* 1$' 15
    xdotool key alt+v; sleep 0.4; xdotool key d; sleep 0.4; xdotool key t; sleep 1
    wait_field layout 3 5 && pass "View > Page Display > Two Page View" || fail "layout '$(field layout)'"
    set -- $(field 'page 1') x x x x; t1=$2; l1=$1
    set -- $(field 'page 2') x x x x; t2=$2; l2=$1
    [ "$t1" = "$t2" ] && [ "$l2" -gt "$l1" ] 2>/dev/null && pass "pages 1 and 2 side by side" || fail "page 1 at $l1,$t1 page 2 at $l2,$t2"
    [ "$(count '^page [0-9]+ ')" = 2 ] && pass "two pages at a time" || fail "$(count '^page [0-9]+ ') pages shown"
    shot two-page
    xdotool key alt+v; sleep 0.4; xdotool key d; sleep 0.4; xdotool key s; sleep 1
    wait_field layout 2 5 && pass "Single Page View" || fail "layout '$(field layout)'"
    [ "$(count '^page [0-9]+ ')" = 1 ] && pass "one page at a time" || fail "$(count '^page [0-9]+ ') pages shown"
    set -- $(field view); xdotool mousemove $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 )) click 1
    xdotool key Next; sleep 0.3; xdotool key Next; sleep 0.3; xdotool key Next
    wait_field current 2 5 && pass "Page Down turns to page 2" || fail "current '$(field current)'"
    xdotool key alt+v; sleep 0.4; xdotool key k; sleep 1
    wait_field night 1 5 && pass "View > Dark Pages" || fail "night '$(field night)'"
    wait_line '^page 2 .* 1$' 10
    shot dark-pages
    set -- $(field view)
    m=$(convert "$OUT/pdf-pro-dark-pages.png" -crop "200x120+$(( ($1 + $3) / 2 - 100 ))+$(( ($2 + $4) / 2 - 60 ))" +repage -colorspace Gray -format '%[fx:int(100*mean)]' info: 2>/dev/null)
    [ -n "$m" ] && [ "$m" -lt 30 ] && pass "the page is drawn dark ($m% bright)" || fail "dark page brightness '$m'"
    xdotool key alt+v; sleep 0.4; xdotool key k; sleep 0.4
    xdotool key alt+v; sleep 0.4; xdotool key d; sleep 0.4; xdotool key e; sleep 0.5
    closeapp
fi

# ============================================================= Attachments
if want attach; then
    A="$DOCS/attach.pdf"; cp "$DOCS/long.pdf" "$A"
    launch "$(wd "$A")" attach "$(answers attach "$(wd "$DOCS/data.bin")")"
    wait_field pages 6 40 || fail "attach.pdf did not open"
    xdotool key alt+t; sleep 0.4; xdotool key a; sleep 1
    wait_line '^attachment 0 5120 data.bin' 10 && pass "Attach a File: listed (5120 bytes)" || fail "attachments: $(grep '^attachment' "$D")"
    wait_field side attachments 5 && pass "the Attachments pane opens" || fail "side '$(field side)'"
    shot attach
    xdotool key ctrl+s; wait_field dirty 0 10
    [ "$(probe attachments "$A")" = "data.bin 5120" ] && pass "the file carries the attachment" || fail "attachments in the file: $(probe attachments "$A")"
    closeapp
fi

# ============================================================= Stamp
if want stamp; then
    K="$DOCS/stamp.pdf"; cp "$DOCS/long.pdf" "$K"
    launch "$(wd "$K")" stamp
    wait_field pages 6 40 || fail "stamp.pdf did not open"
    fitpage
    pane comment
    tb stamp
    sleep 0.5; xdotool key Down Return; sleep 0.5
    set -- $(page_xy 350 120); xdotool mousemove "$1" "$2" click 1
    wait_field undo 1 8 && pass "a stamp placed from Comment's bar" || fail "stamp: undo '$(field undo)'"
    xdotool key ctrl+s; wait_field dirty 0 10
    probe annots "$K" | grep -q '^1 Stamp$' && pass "saved as a Stamp annotation" || fail "annots: $(probe annots "$K")"
    closeapp
fi

# ============================================================= Link, replies, replace pages
if want more; then
    M="$DOCS/more.pdf"; cp "$DOCS/long.pdf" "$M"
    launch "$(wd "$M")" more "$(answers more "$(wd "$DOCS/flat.pdf")")"
    wait_field pages 6 40 || fail "more.pdf did not open"
    fitpage
    pane edit
    tb link
    set -- $(page_xy 72 80); a="$1 $2"; set -- $(page_xy 300 110)
    # shellcheck disable=SC2086
    xdotool mousemove $a mousedown 1 mousemove $(( $1 - 20 )) $(( $2 - 5 )) mousemove "$1" "$2" mouseup 1
    if dialog "Create Link"; then
        xdotool key ctrl+a; xdotool type --delay 20 "https://example.org/gate"; xdotool key Return
        wait_field undo 1 8 && pass "Edit PDF > Link: a box dragged, a web address" || fail "link: undo '$(field undo)'"
    else fail "no Create Link dialog"; fi
    tb close
    pane comment
    tb note
    set -- $(page_xy 500 150); xdotool mousemove "$1" "$2" click 1
    if dialog "Sticky Note"; then xdotool type --delay 20 "Please check"; xdotool key Tab Return; else fail "no note dialog"; fi
    wait_field undo 2 8 || fail "note: undo '$(field undo)'"
    set -- $(grep -E '^listitem 0 ' "$D" | head -1)
    [ $# -ge 6 ] && xdotool mousemove "$5" "$6" click --repeat 2 --delay 90 1
    if dialog "Replies and Status"; then
        xdotool type --delay 20 "Checked by the gate"
        xdotool key alt+s; sleep 0.3; xdotool type "a"; sleep 0.3
        xdotool key Return
        wait_field undo 4 8 && pass "a reply and the status Accepted, from the comments list" || fail "reply: undo '$(field undo)'"
        wait_line '^listitem 0 .*1 reply.*Accepted' 5 && pass "the list shows 1 reply and Accepted" || fail "list: $(grep '^listitem' "$D")"
    else fail "no Replies and Status dialog"; fi
    tb close
    pane organize
    wait_line '^org 2 ' 10 || fail "no page grid"
    set -- $(grep -E '^org 2 ' "$D" | head -1)
    [ $# -ge 6 ] && xdotool mousemove $(( ($3 + $5) / 2 )) $(( ($4 + $6) / 2 )) click 1
    sleep 0.5
    tb replace
    if dialog "Replace Pages"; then xdotool key Return; wait_field undo 5 10 && pass "Organize > Replace: page 2 replaced by the form's page" || fail "replace: undo '$(field undo)'"
    else fail "no Replace Pages prompt"; fi
    xdotool key ctrl+s; wait_field dirty 0 10
    case "$(probe text "$M" 2)" in *"Membership Application"*) pass "page 2 is the form's page now" ;; *) fail "page 2: $(probe text "$M" 2)" ;; esac
    probe links "$M" | grep -q 'https://example.org/gate' && pass "the link is in the file" || fail "links: $(probe links "$M")"
    [ "$(probe annots "$M" | grep -c Text)" -ge 3 ] && pass "the note, its reply and its status are in the file" || fail "annots: $(probe annots "$M")"
    closeapp
fi

# ============================================================= 200%
if want scale; then
    step=$(cat "$T/railstep" 2>/dev/null || echo 42)
    wine taskkill /f /im explorer.exe >/dev/null 2>&1; wineserver -k; sleep 1
    reg 'HKCU\Control Panel\Desktop' /v LogPixels /t REG_DWORD /d 192
    wineserver -w
    WINEDEBUG=-all wine explorer /desktop=shell,1280x900 >/dev/null 2>&1 &
    sleep 4
    launch "$(wd "$DOCS/long.pdf")" scale
    wait_field pages 6 40 || fail "long.pdf did not open at 200%"
    s1=$(grep '^railbtn select ' "$D" | cut -d' ' -f4); s2=$(grep '^railbtn comment ' "$D" | cut -d' ' -f4)
    [ -n "$s1" ] && [ $(( (s2 - s1) * 10 )) -ge $(( step * 18 )) ] && pass "at 200% the rail is twice as large ($step -> $(( s2 - s1 )) px a step)" \
        || fail "200%: rail step $step -> $(( ${s2:-0} - ${s1:-0} ))"
    shot scale-200
    closeapp
    reg 'HKCU\Control Panel\Desktop' /v LogPixels /t REG_DWORD /d 96
fi

[ $RC = 0 ] && echo "pdf-pro-check: all passed"
exit $RC
