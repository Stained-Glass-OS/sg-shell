#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2046,SC2086
# Gate for the font viewer and the Fonts folder (sg-fontview; fontview.exe,
# control fonts), under Xvfb on a shell desktop of a SHARED prefix -- this
# user owns it (SYSTEM there, an administrator's elevated token) and the
# second Unix user SG_OTHER (default sgconf) is a standard user:
#
#   - the font file reader (fontinfo.c, run natively) against fontTools on a
#     generated TrueType font, a generated collection (.ttc), DejaVu Sans Bold,
#     an OpenType/CFF font and a Windows bitmap font (.fon): family, full name,
#     version, weight, italic, faces
#   - as the standard user: the viewer on a generated font ("SG Gate Serif",
#     never installed anywhere) shows the names read from the file, loads it
#     privately (GDI draws in it), and the sample line at 72 pt is several
#     times the height of the one at 12 pt (pixels)
#   - Install (a click) installs it for that user: the file in
#     %LOCALAPPDATA%\Microsoft\Windows\Fonts, the HKCU Fonts value naming it,
#     a copy fontconfig lists (fc-list, with the user's gate HOME), and a new
#     process enumerates the family
#   - the Fonts folder (control fonts, through sg-control) lists it; search
#     narrows the tiles; clicking selects it, "For you", its file; Delete
#     removes it -- the value, both files, and a new process no longer has it
#   - "Install for all users" refused for the standard user (exit 5, the
#     administrator message, nothing in %WINDIR%\Fonts or HKLM)
#   - as the owner (SYSTEM): installed for all users -- %WINDIR%\Fonts, HKLM
#     value = the file name, sg-admind (test mode, its own spool) copies it
#     to the Linux fonts folder; the standard user's new process sees it; the
#     Fonts folder says "For all users"; uninstalled again, all three gone
#
# Needs Xvfb, xdotool, ImageMagick, mingw, python3-fonttools, fonts-dejavu,
# fc-list and passwordless `sudo -u $SG_OTHER`; skips (77) without them.
# SG_WINE_DIR another Wine (a build tree works; per-user fonts need wine-sg
# 0183), SG_FONTVIEW_EXE another build (mutants: -DSG_MUTANT_NOREG,
# -DSG_MUTANT_NAMEID, -DSG_MUTANT_ANYONE). Screenshots: build/fontview-*.png.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_FONTVIEW_EXE:-$HERE/build/sg-fontview64.exe}"
CONTROL="${SG_CONTROL_EXE:-$HERE/build/sg-control64.exe}"
OUT="$HERE/build"
SG_OTHER=${SG_OTHER:-sgconf}
SG_GROUP=${SG_GROUP:-sgconfgrp}
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool xhost import convert gcc fc-list; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
python3 -c 'import fontTools' 2>/dev/null || { echo "SKIP: needs python3-fonttools"; exit 77; }
DJ=/usr/share/fonts/truetype/dejavu
[ -f "$DJ/DejaVuSerif.ttf" ] && [ -f "$DJ/DejaVuSansMono.ttf" ] || { echo "SKIP: needs fonts-dejavu"; exit 77; }
if [ -x "$WINE_DIR/bin/wine" ]; then WBIN="$WINE_DIR/bin"; WSERVER="$WINE_DIR/bin/wineserver"; WSHARE="$WINE_DIR/share/wine"
elif [ -x "$WINE_DIR/wine" ]; then WBIN="$WINE_DIR"; WSERVER="$WINE_DIR/server/wineserver"; WSHARE="$WINE_DIR/fonts/.."
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi
[ -f "$EXE" ] && [ -f "$CONTROL" ] || { echo "SKIP: $EXE or $CONTROL missing (make build)"; exit 77; }
id "$SG_OTHER" >/dev/null 2>&1 && sudo -n -u "$SG_OTHER" true 2>/dev/null || { echo "SKIP: no $SG_OTHER or sudo"; exit 77; }

T=$(mktemp -d /var/tmp/sg-fontview-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    set +e
    WINEPREFIX="$T/pfx" "$WSERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    sleep 1
    sudo -n rm -rf "$T"
}
trap cleanup EXIT INT TERM
mkdir -p "$OUT" "$T/gate" "$T/bin" "$T/home-owner" "$T/home-other" "$T/out"
chmod 777 "$T/out" "$T/home-other"; chmod 755 "$T/gate" "$T/bin"

# ---- the fonts: generated from DejaVu under names nothing else has --------------
python3 - "$T/gate" "$DJ" <<'EOF' || { echo "FAIL  could not make the test fonts"; exit 1; }
import sys
from fontTools.ttLib import TTFont
from fontTools.ttLib.ttCollection import TTCollection
out, dj = sys.argv[1], sys.argv[2]
def rename(src, family, sub):
    f = TTFont(src)
    for r in f['name'].names:
        if r.nameID in (1, 16): r.string = family
        elif r.nameID == 4: r.string = family if sub == 'Book' else family + ' ' + sub
        elif r.nameID == 6: r.string = (family + '-' + sub).replace(' ', '')
        elif r.nameID == 3: r.string = 'SGGate:' + family + ' ' + sub
        elif r.nameID == 5: r.string = 'Version 7.25 gate'
    return f
rename(dj + '/DejaVuSerif.ttf', 'SG Gate Serif', 'Book').save(out + '/sggate-serif.ttf')
rename(dj + '/DejaVuSans.ttf', 'SG Gate Machine', 'Book').save(out + '/sggate-machine.ttf')
c = TTCollection()
c.fonts = [rename(dj + '/DejaVuSansMono.ttf', 'SG Gate Mono', 'Book'), rename(dj + '/DejaVuSansMono-Bold.ttf', 'SG Gate Mono', 'Bold')]
c.save(out + '/sggate-mono.ttc')
EOF
chmod 644 "$T/gate"/*

# ---- the reader, natively, against fontTools ------------------------------------------
gcc -O2 -Wall -o "$T/fid" "$HERE/test/fontinfo-dump.c" "$HERE/src/fontview/fontinfo.c" ${SG_FONTINFO_CFLAGS:-} 2>/dev/null \
    || { fail "the reader did not build natively"; exit 1; }
OTF=$(ls /usr/share/fonts/opentype/*/*.otf 2>/dev/null | head -1)
FON=$(ls "$WSHARE"/fonts/coure.fon /opt/wine-sg/share/wine/fonts/coure.fon 2>/dev/null | head -1)
for f in "$T/gate/sggate-serif.ttf" "$T/gate/sggate-mono.ttc" "$DJ/DejaVuSans-Bold.ttf" ${OTF:+"$OTF"}; do
    "$T/fid" "$f" > "$T/fid.out"
    python3 - "$f" "$T/fid.out" <<'EOF' > "$T/cmp.out"
import sys
from fontTools.ttLib import TTFont, TTCollection
path, got = sys.argv[1], [l.rstrip('\n').split('\t') for l in open(sys.argv[2], encoding='utf-8')]
fonts = TTCollection(path).fonts if path.endswith('.ttc') else [TTFont(path)]
faces = [g for g in got if g[0] == 'FACE']
bad = []
if len(faces) != len(fonts): bad.append('faces %d != %d' % (len(faces), len(fonts)))
for i, (f, g) in enumerate(zip(fonts, faces)):
    n = f['name']
    want = {'family': n.getDebugName(1), 'full': n.getDebugName(4), 'version': n.getDebugName(5),
            'weight': str(f['OS/2'].usWeightClass), 'italic': str(f['OS/2'].fsSelection & 1)}
    have = {'family': g[2], 'full': g[3], 'version': g[5].strip(), 'weight': g[6], 'italic': g[7]}
    for k in want:
        if (want[k] or '').strip() != have[k].strip(): bad.append('face %d %s: %r != %r' % (i, k, have[k], want[k]))
print('OK' if not bad else '; '.join(bad))
EOF
    r=$(cat "$T/cmp.out")
    [ "$r" = OK ] && pass "reader = fontTools on $(basename "$f") ($(grep -c '^FACE' "$T/fid.out") face(s): $(sed -n 's/^FACE\t0\t\([^\t]*\)\t\([^\t]*\).*/\1 | \2/p' "$T/fid.out"))" \
        || fail "reader vs fontTools on $(basename "$f"): $r"
done
if [ -n "$FON" ]; then
    "$T/fid" "$FON" > "$T/fid.out"
    k=$(sed -n 's/^FILE\t[^\t]*\t\([0-9]*\).*/\1/p' "$T/fid.out"); fam=$(sed -n 's/^FACE\t0\t\([^\t]*\).*/\1/p' "$T/fid.out")
    [ "$k" = 3 ] && [ "$fam" = Courier ] && pass "a Windows bitmap font (.fon, $(basename "$FON")): Courier" || fail ".fon read as kind=$k family=$fam"
fi
"$T/fid" "$T/fid" /dev/null > "$T/fid.out" 2>&1
[ "$(grep -c unreadable "$T/fid.out")" = 2 ] && pass "an executable and an empty file are not fonts" || fail "non-fonts: $(cat "$T/fid.out")"

# ---- the shared prefix and the desktop ---------------------------------------------------------
cp "$EXE" "$T/bin/sg-fontview64.exe"; cp "$CONTROL" "$T/bin/sg-control64.exe"; chmod 755 "$T/bin"/*.exe
DPY=${SG_FONTVIEW_DPY:-181}
Xvfb ":$DPY" -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
i=0; while [ ! -e "/tmp/.X11-unix/X$DPY" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$DPY"
xhost "+SI:localuser:$SG_OTHER" >/dev/null 2>&1

PFX="$T/pfx"
mkdir "$PFX"; chgrp "$SG_GROUP" "$PFX"; chmod 2770 "$PFX"; touch "$PFX/.sg-system-prefix"
export WINEPREFIX="$PFX" WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all PATH="$WBIN:$PATH" HOME="$T/home-owner"
export XDG_CACHE_HOME="$T/home-owner/.cache"
sg "$SG_GROUP" -c "umask 002; '$WSERVER' -p"
sg "$SG_GROUP" -c "umask 002; wine wineboot -i" >/dev/null 2>&1
chmod -R g+rwX "$PFX" 2>/dev/null
C="$PFX/drive_c"
winpath() { wine winepath -w "$1" 2>/dev/null | tr -d '\r'; }
fvwin=$(winpath "$T/bin/sg-fontview64.exe")
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x800
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\fontview.exe' /ve /d "$fvwin"
for f in "$HERE"/theme/5[0-2]-*.reg; do wine regedit /S "Z:$(printf %s "$f" | tr / '\\')" >/dev/null 2>&1; done
other() {
    sudo -n -u "$SG_OTHER" env DISPLAY="$DISPLAY" WINEPREFIX="$PFX" WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=' \
        HOME="$T/home-other" XDG_CACHE_HOME="$T/home-other/.cache" PATH="$PATH" SG_FONTVIEW_DUMP="$ODUMP" SG_FONTVIEW_YES=1 "$@"
}
ODUMP='C:\users\Public\fv-other.txt'
other wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
other wine reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x800 /f >/dev/null 2>&1
for f in "$HERE"/theme/5[0-2]-*.reg; do other wine regedit /S "Z:$(printf %s "$f" | tr / '\\')" >/dev/null 2>&1; done
wine explorer /desktop=shell,1280x800 > "$T/explorer.out" 2>&1 &
sleep 5

D="$C/users/Public/fv-other.txt"
v() { tr -d '\r' 2>/dev/null < "$D" | sed -n "s/^$1=//p" | head -1; }
wait_v() { i=0; while [ "$(v "$1")" != "$2" ] && [ $i -lt $(( ${3:-15} * 2 )) ]; do sleep 0.5; i=$((i + 1)); done; [ "$(v "$1")" = "$2" ]; }
shot() { import -window root "$OUT/fontview-$1.png" 2>/dev/null; }
at() { xdotool mousemove "$(echo "$1" | cut -d, -f1)" "$(echo "$1" | cut -d, -f2)" click 1; sleep 1.5; }
ofam() { other wine "$fvwin" --families 'C:\users\Public\fam.txt' >/dev/null 2>&1; tr -d '\r' < "$C/users/Public/fam.txt" | grep -cx "$1"; }
ureg() { other wine reg query 'HKCU\Software\Microsoft\Windows NT\CurrentVersion\Fonts' 2>/dev/null | tr -d '\r'; }
mreg() { wine reg query 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\Fonts' 2>/dev/null | tr -d '\r'; }
fclist() { HOME="$T/home-other" XDG_CACHE_HOME="$T/fc-cache" fc-list : family 2>/dev/null | grep -c "$1"; }
cp "$T/gate/sggate-serif.ttf" "$C/users/Public/SG Gate Serif.ttf"; chmod 644 "$C/users/Public/SG Gate Serif.ttf"

[ "$(ofam 'SG Gate Serif')" = 0 ] && [ "$(fclist 'SG Gate Serif')" = 0 ] && pass "SG Gate Serif is installed nowhere to begin with" \
    || fail "the test font is already installed"

# ---- the viewer (standard user) ----------------------------------------------------------------
if [ -f "$C/windows/system32/fontview.exe" ]; then how="the .ttf's association (wine-sg 0183)"; other wine start 'C:\users\Public\SG Gate Serif.ttf' >/dev/null 2>&1 &
else how="sg-fontview directly (no wine-sg 0183)"; other wine "$fvwin" 'C:\users\Public\SG Gate Serif.ttf' >/dev/null 2>&1 & fi
# the viewer is up once it has laid out its sample lines
i=0; while [ -z "$(v size72)" ] && [ $i -lt 120 ]; do sleep 0.5; i=$((i + 1)); done; sleep 2
shot viewer
[ "$(v family)" = "SG Gate Serif" ] && [ "$(v version)" = "Version 7.25 gate" ] && [ "$(v title)" = "SG Gate Serif (TrueType)" ] \
    && pass "the viewer, through $how, shows the file's own names: $(v title), $(v version)" \
    || fail "viewer: title='$(v title)' family='$(v family)' version='$(v version)'"
[ "$(v added)" = 1 ] && [ "$(v gdi_face)" = "SG Gate Serif" ] && pass "loaded privately (AddFontResourceEx FR_PRIVATE), GDI draws in it" \
    || fail "private font: added=$(v added) gdi_face='$(v gdi_face)'"
[ "$(v type)" = "OpenType Layout, TrueType Outlines" ] && pass "type: $(v type)" || fail "type: '$(v type)'"
# the 12 pt and 72 pt lines: ink height in each band
band() {  # y0 y1 -> height of the dark ink's bounding box in that band
    convert "$OUT/fontview-viewer.png" -crop "600x$(( $2 - $1 ))+60+$1" +repage -colorspace Gray -threshold 50% -negate -trim \
        -format '%h' info: 2>/dev/null
}
y12=$(v size12); y18=$(v size18); y72=$(v size72)
h12=$(band "$y12" "$y18"); h72=$(band "$y72" $((y72 + 110)))
[ "${h12:-0}" -gt 6 ] && [ "${h72:-0}" -gt $(( ${h12:-99} * 4 )) ] && pass "the sample at 72 pt is drawn $h72 px tall, at 12 pt $h12 px" \
    || fail "sample heights: 12 pt $h12 px, 72 pt $h72 px"

# Install (a click): for this user
at "$(v install)"
wait_v scope 1 15
UD="$C/users/$SG_OTHER/AppData/Local/Microsoft/Windows/Fonts"
[ "$(v scope)" = 1 ] && [ "$(v install | cut -d, -f3)" = 0 ] && pass "Install: installed for this user ($(v status)), the button greyed" \
    || fail "after Install: scope=$(v scope) status='$(v status)' install=$(v install)"
ureg | grep -q "SG Gate Serif (TrueType) *REG_SZ *C:\\\\users\\\\$SG_OTHER\\\\AppData\\\\Local\\\\Microsoft\\\\Windows\\\\Fonts\\\\SG Gate Serif.ttf" \
    && [ -f "$UD/SG Gate Serif.ttf" ] && pass "HKCU\\...\\Fonts names the copy in %LOCALAPPDATA%\\Microsoft\\Windows\\Fonts" \
    || fail "per-user registration: $(ureg | grep -i gate) file=$(ls "$UD" 2>&1)"
[ "$(fclist 'SG Gate Serif')" -ge 1 ] && pass "fontconfig lists it for the user's Linux programs (~/.local/share/fonts/stained-glass)" \
    || fail "fc-list does not list it ($(ls "$T/home-other/.local/share/fonts/stained-glass" 2>&1))"
[ "$(ofam 'SG Gate Serif')" = 1 ] && pass "a new process of the user enumerates SG Gate Serif" || fail "a new process does not see the installed font"
shot installed
other wine taskkill /f /im sg-fontview64.exe >/dev/null 2>&1; sleep 1

# ---- the Fonts folder (control fonts) --------------------------------------------------------------
other wine "$(winpath "$T/bin/sg-control64.exe")" fonts >/dev/null 2>&1 &
wait_v window folder 30; sleep 2
n=$(v families)
[ "${n:-0}" -gt 5 ] && tr -d '\r' < "$D" | grep -q "	SG Gate Serif\$" && pass "control fonts opens the Fonts folder: $n families, SG Gate Serif among them" \
    || fail "Fonts folder: families=$n, has SG Gate Serif: $(tr -d '\r' < "$D" | grep -c 'SG Gate Serif')"
shot folder
at "$(v search)"
xdotool type --delay 60 'sg gate'; sleep 1.5
t=$(tr -d '\r' < "$D" | sed -n 's/^tile=\([0-9-]*,[0-9-]*\)\tSG Gate Serif$/\1/p')
[ "$(v shown)" -ge 1 ] && [ "$(v shown)" -lt 5 ] && [ -n "$t" ] && pass "search 'sg gate' narrows it to $(v shown) tile(s)" \
    || fail "search: shown=$(v shown) filter='$(v filter)' tile='$t'"
at "$t"
[ "$(v selected)" = "SG Gate Serif" ] && [ "$(v sel_scope)" = 1 ] && tr -d '\r' < "$D" | grep -q "^sel_file=1	.*SG Gate Serif.ttf" \
    && pass "clicking its tile selects it: installed for you, its file" || fail "selection: '$(v selected)' scope=$(v sel_scope) files=$(v sel_files)"
shot folder-selected
xdotool key Delete; sleep 3
tr -d '\r' < "$D" | grep -q "	SG Gate Serif\$" && fail "the family is still listed after Delete (status '$(v status)', shown $(v shown), selected '$(v selected)')" \
    || pass "Delete takes it out of the folder ($(v status))"
[ -z "$(ureg | grep -i 'gate serif')" ] && [ ! -f "$UD/SG Gate Serif.ttf" ] && [ ! -f "$T/home-other/.local/share/fonts/stained-glass/SG Gate Serif.ttf" ] \
    && pass "the HKCU value and both copies are gone" || fail "left behind: $(ureg | grep -i gate) $(ls "$UD" "$T/home-other/.local/share/fonts/stained-glass" 2>&1 | tr '\n' ' ')"
[ "$(ofam 'SG Gate Serif')" = 0 ] && [ "$(fclist 'SG Gate Serif')" = 0 ] && pass "no new process and no Linux program sees it any more" \
    || fail "still seen after Delete: new process $(ofam 'SG Gate Serif'), fc-list $(fclist 'SG Gate Serif')"
shot deleted
other wine taskkill /f /im sg-fontview64.exe >/dev/null 2>&1; sleep 1

# ---- for all users -----------------------------------------------------------------------------------
cp "$T/gate/sggate-machine.ttf" "$C/users/Public/sggate-machine.ttf"; chmod 644 "$C/users/Public/sggate-machine.ttf"
other wine "$fvwin" /install /allusers /quiet 'C:\users\Public\sggate-machine.ttf' >/dev/null 2>&1; code=$?
wait_v result 5 5
[ "$(v result)" = 5 ] && [ "$(v message)" = "You need to be an administrator to install fonts for all users." ] \
    && [ ! -f "$C/windows/Fonts/sggate-machine.ttf" ] && [ -z "$(mreg | grep -i 'gate machine')" ] \
    && pass "Install for all users is refused for the standard user (5, the administrator message, nothing written)" \
    || fail "standard user, all users: result=$(v result) message='$(v message)' file=$([ -f "$C/windows/Fonts/sggate-machine.ttf" ] && echo yes) reg=$(mreg | grep -ci 'gate machine')"

# the owner is SYSTEM here: sg-admind (test mode, its own spool) does the Linux part
SP="$T/spool"; mkdir -p "$SP/requests" "$SP/replies"; chmod 700 "$SP/requests"
ADMIN="$HERE/admin/sg-admind"
DST="$T/usr-local-fonts/stained-glass"
( i=0; while [ $i -lt 600 ] && [ ! -f "$T/stop-admind" ]; do
      SG_ADMIN_TEST=1 SG_ADMIN_SPOOL="$SP" SG_ADMIN_FONTS_SRC="$C/windows/Fonts" SG_ADMIN_FONTS_DST="$DST" \
          python3 "$ADMIN" 2>>"$T/admind.log"; sleep 0.3; i=$((i + 1)); done ) &
OWNDUMP="$C/users/Public/fv-owner.txt"
ow() { tr -d '\r' 2>/dev/null < "$OWNDUMP" | sed -n "s/^$1=//p" | head -1; }
SG_ADMIN_SPOOL="$SP" SG_FONTVIEW_DUMP='C:\users\Public\fv-owner.txt' wine "$fvwin" /install /allusers /quiet 'C:\users\Public\sggate-machine.ttf' >/dev/null 2>&1
[ "$(ow result)" = 0 ] && [ -f "$C/windows/Fonts/sggate-machine.ttf" ] && mreg | grep -q 'SG Gate Machine (TrueType) *REG_SZ *sggate-machine.ttf$' \
    && pass "the administrator (SYSTEM) installs it for all users: %WINDIR%\\Fonts, HKLM value = the file name" \
    || fail "all users: result=$(ow result) '$(ow message)' file=$(ls "$C/windows/Fonts" 2>&1 | tr '\n' ' ') reg=$(mreg | grep -i 'gate machine')"
[ -f "$DST/sggate-machine.ttf" ] && cmp -s "$DST/sggate-machine.ttf" "$T/gate/sggate-machine.ttf" \
    && pass "sg-admind copied it to the Linux fonts folder for everyone's Linux programs" || fail "no Linux copy ($(tail -2 "$T/admind.log"))"
[ "$(ofam 'SG Gate Machine')" = 1 ] && pass "the standard user's new process enumerates the machine font" || fail "the standard user does not see the machine font"
other wine "$fvwin" /folder >/dev/null 2>&1 &
wait_v window folder 30; sleep 2
at "$(v search)"; xdotool type --delay 60 'machine'; sleep 1.5
t=$(tr -d '\r' < "$D" | sed -n 's/^tile=\([0-9-]*,[0-9-]*\)\tSG Gate Machine$/\1/p')
[ -n "$t" ] && at "$t"
[ "$(v selected)" = "SG Gate Machine" ] && [ "$(v sel_scope)" = 2 ] && pass "the Fonts folder says it is installed for all users" \
    || fail "machine font in the folder: selected='$(v selected)' scope=$(v sel_scope)"
shot folder-machine
other wine taskkill /f /im sg-fontview64.exe >/dev/null 2>&1; sleep 1
SG_ADMIN_SPOOL="$SP" SG_FONTVIEW_DUMP='C:\users\Public\fv-owner.txt' wine "$fvwin" /uninstall /allusers /quiet 'C:\users\Public\sggate-machine.ttf' >/dev/null 2>&1
[ "$(ow result)" = 0 ] && [ ! -f "$C/windows/Fonts/sggate-machine.ttf" ] && [ -z "$(mreg | grep -i 'gate machine')" ] && [ ! -f "$DST/sggate-machine.ttf" ] \
    && pass "and uninstalled for all users: the file, the HKLM value and the Linux copy gone" \
    || fail "all-users uninstall: result=$(ow result) '$(ow message)' $(ls "$C/windows/Fonts" "$DST" 2>&1 | tr '\n' ' ')"
touch "$T/stop-admind"

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
