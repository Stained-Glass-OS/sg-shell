#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# SG Office under Wine, end to end: Get SG Office refuses an installer that is
# not the pinned file, installs The Document Foundation's LibreOffice from it
# silently with our payload on top, the three programs open documents (from a
# mapped drive too) in windows named SG Office, the Excel formula corpus
# matches its baseline, and the VBA corpus runs.
#
#   SG_OFFICE_MSI=/path/LibreOffice_<Version>_Win_x86-64.msi   the pinned file (office/office.ini)
#   SG_WINE, SG_WINESERVER     the Wine to test (default /opt/wine-sg/bin/{wine,wineserver})
#
# Skips (77) without the MSI, Wine, Xvfb, python3-uno or a built tree.
# Screenshots: build/office-{spreadsheets,documents,presentations}.png.
set -u
unset DISPLAY XAUTHORITY WAYLAND_DISPLAY
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
B="$HERE/build"
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
skip() { echo "SKIP  $*"; exit 77; }
WINE=${SG_WINE:-/opt/wine-sg/bin/wine}
WINESERVER=${SG_WINESERVER:-$(dirname "$WINE")/wineserver}
MSI=${SG_OFFICE_MSI:-}
[ -n "$MSI" ] && [ -f "$MSI" ] || skip "SG_OFFICE_MSI (the pinned LibreOffice installer) is not given"
[ -x "$WINE" ] || skip "no Wine at $WINE"
command -v Xvfb >/dev/null || skip "no Xvfb"
/usr/bin/python3 -c "import uno" 2>/dev/null || skip "no python3-uno"
[ -f "$B/sg-office-setup64.exe" ] && [ -d "$B/office/payload" ] || skip "build first (make office)"
export WINE WINESERVER
export WINEPREFIX="$SG_GATE_HOME/prefix" WINEDEBUG=-all TMPDIR="${TMPDIR:-/var/tmp}"
W=$(mktemp -d "$TMPDIR/office-wine.XXXXXX")
zpath() { printf 'Z:%s' "$(printf %s "$1" | tr / '\\')"; }

# a display of our own, never the user's
for n in $(seq 180 199); do [ -e "/tmp/.X11-unix/X$n" ] || { DPY=":$n"; break; }; done
Xvfb "$DPY" -screen 0 1400x900x24 -nolisten tcp >/dev/null 2>&1 &
XPID=$!
export DISPLAY="$DPY"
cleanup() { "$WINESERVER" -k 2>/dev/null; kill "$XPID" 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT INT TERM
sleep 1

"$WINE" wineboot -i >/dev/null 2>&1
"$WINESERVER" -w
# as in Stained Glass OS: the shell's font replacements (Calibri -> Carlito
# ...) and crashes reported, not a dialog nobody answers
"$WINE" reg import "$(zpath "$HERE/theme/52-sg-fonts.reg")" >/dev/null 2>&1
"$WINE" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f >/dev/null 2>&1
for r in "$HERE"/office/defaults/*.reg; do "$WINE" reg import "$(zpath "$r")" >/dev/null 2>&1 || fail "import $(basename "$r")"; done
"$WINESERVER" -w
export SG_OFFICE_NO_ELEVATE=1

# 0. not installed yet: a program offers Get SG Office
"$WINE" "$B/sg-spreadsheets64.exe" >/dev/null 2>&1 &
i=0; gw=
while [ $i -lt 60 ]; do gw=$(xdotool search --name "Get SG Office" 2>/dev/null | head -1); [ -n "$gw" ] && break; sleep 2; i=$((i + 2)); done
sleep 2; import -window root "$B/office-get.png" 2>/dev/null
[ -n "$gw" ] && pass "before LibreOffice is installed, SG Office Spreadsheets opens Get SG Office" || fail "no Get SG Office window"
"$WINESERVER" -k 2>/dev/null; "$WINESERVER" -w 2>/dev/null

# 1. an installer that is not the pinned file is never run
cp -r "$B/office/payload" "$W/badpay"
sed -i 's/^Sha256=.*/Sha256=0000000000000000000000000000000000000000000000000000000000000000/' "$W/badpay/office.ini"
SG_OFFICE_PAYLOAD=$(zpath "$W/badpay") "$WINE" "$B/sg-office-setup64.exe" /install /msi "$(zpath "$MSI")" /quiet >/dev/null 2>&1
rc=$?
PROG="$WINEPREFIX/drive_c/Program Files/LibreOffice"
[ "$rc" != 0 ] && [ ! -e "$PROG/program/soffice.exe" ] && pass "an installer whose SHA-256 is not the pinned one is refused (not run)" \
    || fail "a wrong installer was accepted (rc $rc)"

# 2. the real one: installed silently, the payload on top
SG_OFFICE_PAYLOAD=$(zpath "$B/office/payload")
export SG_OFFICE_PAYLOAD
if "$WINE" "$B/sg-office-setup64.exe" /install /msi "$(zpath "$MSI")" /quiet >"$W/install.log" 2>&1; then
    pass "Get SG Office installed LibreOffice from The Document Foundation's MSI"
else fail "Get SG Office /install ($(tail -1 "$W/install.log"))"; fi
[ -f "$PROG/program/soffice.exe" ] && pass "soffice.exe is in Program Files" || fail "no soffice.exe"
X="$PROG/share/registry/sg-office.xcd"
[ -f "$X" ] && ! grep -q '@TEMPLATEDIR@' "$X" && grep -q 'file:///C:/Program%20Files/LibreOffice/share/template/sg-office/normal.ott' "$X" \
    && pass "our defaults are installed, naming the installed templates" || fail "sg-office.xcd not installed right"
[ -f "$PROG/share/extensions/sg-office-functions/component.py" ] && pass "SG Office Functions is a bundled extension" || fail "no SG Office Functions"
set -- "$WINEPREFIX/drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs/"LibreOffice*
[ -e "$1" ] && fail "LibreOffice's own Start menu entries are still there" || pass "Start shows SG Office's programs, not LibreOffice's"
"$WINE" "$B/sg-office-setup64.exe" /status >/dev/null 2>&1 && pass "/status: installed with the current payload" || fail "/status"

# 3. the programs: a spreadsheet from a mapped drive, a new document, a new presentation
mkdir -p "$W/share"
/usr/bin/python3 -c "
import sys; sys.path.insert(0, '$HERE/office/parity')
from xlsxw import Workbook
wb = Workbook(); s = wb.sheet('Sheet1'); s.value(0, 0, 'Region'); s.value(0, 1, 'Units')
s.value(1, 0, 'East'); s.value(1, 1, 10); s.value(2, 0, 'West'); s.value(2, 1, 20); s.formula(3, 1, 'SUM(B2:B3)')
wb.save('$W/share/book.xlsx')"
ln -sfn "$W/share" "$WINEPREFIX/dosdevices/n:"
shot() {   # KIND TITLE-PART ARGS...
    kind=$1 want=$2; shift 2
    "$WINE" "$B/sg-${kind}64.exe" "$@" >/dev/null 2>&1 &
    i=0; found=
    while [ $i -lt 240 ]; do
        found=$(xdotool search --name "$want" 2>/dev/null | head -1)
        [ -n "$found" ] && break
        sleep 2; i=$((i + 2))
    done
    sleep 8
    import -window root "$B/office-$kind.png" 2>/dev/null
    if [ -n "$found" ]; then pass "sg-$kind: a window '$(xdotool getwindowname "$found")'"; else fail "sg-$kind: no window named '$want'"; fi
    # closed as a user closes it (a killed LibreOffice offers document
    # recovery at its next start)
    for w in $(xdotool search --name "SG Office" 2>/dev/null); do xdotool windowclose "$w" 2>/dev/null; done
    sleep 8
    "$WINESERVER" -k 2>/dev/null; "$WINESERVER" -w 2>/dev/null
    # the user's settings: seeded from the payload by the launcher (Excel's syntax)
    set -- "$WINEPREFIX"/drive_c/users/*/AppData/Roaming/LibreOffice/4/user/registrymodifications.xcu
    if [ -f "$1" ] && grep -q 'Formula/Syntax"><prop oor:name="Grammar" oor:op="fuse"><value>1</value>' "$1"; then
        [ "$kind" = spreadsheets ] && pass "a new user's profile starts with Excel's formula syntax (seeded by the launcher)"
    else fail "the launcher did not seed the user's profile"; fi
    # a fresh profile for the next program (a killed LibreOffice offers recovery)
    rm -rf "$WINEPREFIX"/drive_c/users/*/AppData/Roaming/LibreOffice
    "$WINE" reg delete 'HKCU\Software\Stained Glass\SG Office' /v ProfileSeeded /f >/dev/null 2>&1
}
shot spreadsheets "^book\.xlsx .* SG Office Spreadsheets$" 'N:\book.xlsx'
shot documents " SG Office Documents$"
shot presentations " SG Office Presentations$"

# 4. the Excel formula corpus: nothing that matched Excel may stop matching
if /usr/bin/python3 "$HERE/office/parity/corpus.py" --run --wine 'C:\Program Files\LibreOffice\program\soffice.exe' \
        --seed "$B/office/payload/sg-office-user.xcu" --baseline "$HERE/office/parity/baseline.json" \
        --json "$B/office-corpus.json" --report "$B/office-corpus.md" > "$W/corpus.log" 2>&1; then
    pass "formula corpus: $(grep '^results:' "$W/corpus.log") (no regression from the baseline)"
else grep -E "REGRESSION|Error|error" "$W/corpus.log" | head -20; fail "formula corpus"; fi

# 5. VBA
if /usr/bin/python3 "$HERE/office/vba/vba.py" --wine 'C:\Program Files\LibreOffice\program\soffice.exe' \
        --seed "$B/office/payload/sg-office-user.xcu" --baseline "$HERE/office/vba/baseline.json" \
        --json "$B/office-vba.json" "$HERE/office/vba/excel.vba" "$HERE/office/vba/word.vba" > "$W/vba.log" 2>&1; then
    pass "VBA corpus: $(grep '^vba:' "$W/vba.log") (no regression from the baseline)"
else grep -E "REGRESSION|Error" "$W/vba.log" | head -20; fail "VBA corpus"; fi

[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
