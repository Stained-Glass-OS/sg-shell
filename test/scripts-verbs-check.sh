#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# What a script file does when it is opened (defaults/45-sg-scripts.reg), as
# on Windows: a JScript file (.js) runs in Windows Script Host, with "Open
# with Command Prompt" (cscript), Edit and Print beside it; a PowerShell
# script (.ps1) opens in Notepad and runs from "Run with PowerShell". Before,
# neither type had a verb and double-clicking did nothing. The mutant is
# SG_SCRIPTS_REG=/dev/null (no file).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
REG="${SG_SCRIPTS_REG:-$HERE/defaults/45-sg-scripts.reg}"
RC=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }
[ -x "$WINE_DIR/bin/wine" ] || { echo "SKIP: no wine at $WINE_DIR"; exit 77; }
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "SKIP: no mingw"; exit 77; }
T=$(mktemp -d /var/tmp/sg-scripts-check.XXXXXX)
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" PATH="$WINE_DIR/bin:$PATH" DISPLAY=
trap 'wineserver -k 2>/dev/null; rm -rf "$T"' EXIT INT TERM
wine wineboot -i >/dev/null 2>&1; wineserver -w
D="$WINEPREFIX/drive_c"
x86_64-w64-mingw32-gcc -municode -O2 -o "$D/probe.exe" "$HERE/test/sg-menu-probe.c" -lole32 -lshell32 -luuid || { fail "probe did not build"; exit 1; }
printf 'var f = new ActiveXObject("Scripting.FileSystemObject").CreateTextFile("C:\\\\js.txt", true);\r\nf.WriteLine("ran " + WScript.Arguments.length);\r\nf.Close();\r\n' > "$D/x.js"
printf 'Set-Content C:\\ps.txt ran\r\n' > "$D/x.ps1"
[ -f "$REG" ] && [ -s "$REG" ] && cp "$REG" "$D/r.reg" && wine reg import 'C:\r.reg' >/dev/null 2>&1
wineserver -w
items() { wine 'C:\probe.exe' menu "C:\\$1" 2>/dev/null | tr -d '\r'; }
m=$(items x.js)
for v in open open2 edit print; do
    printf '%s\n' "$m" | grep -qi "^$v	" && pass "x.js: the menu has '$v'" || fail "x.js: no $v item ($(printf '%s' "$m" | cut -f1 | tr '\n' ' '))"
done
m=$(items x.ps1)
printf '%s\n' "$m" | grep -q '^0	Run with PowerShell' && pass "x.ps1: the menu has 'Run with PowerShell'" || fail "x.ps1: no Run with PowerShell ($(printf '%s' "$m" | cut -f1 | tr '\n' ' '))"
for v in open edit; do
    printf '%s\n' "$m" | grep -qi "^$v	" && pass "x.ps1: the menu has '$v'" || fail "x.ps1: no $v item"
done
q=$(wine reg query 'HKCR\Microsoft.PowerShellScript.1\Shell\Open\Command' /ve 2>/dev/null | tr -d '\r')
case "$q" in *notepad.exe*) pass "a PowerShell script opens in Notepad (it is not run on a double-click)" ;; *) fail "ps1 open: $q" ;; esac
rm -f "$D/js.txt"
v=$(items x.js | grep -i '^open	' | cut -f1)
wine 'C:\probe.exe' invoke 'C:\x.js' "${v:-open}" >/dev/null 2>&1; sleep 2; wineserver -w
grep -q 'ran 0' "$D/js.txt" 2>/dev/null && pass "opening x.js runs it in Windows Script Host" || fail "x.js did not run ($(cat "$D/js.txt" 2>/dev/null))"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
