#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# "Run as administrator" on a program's right-click menu (defaults/43-sg-
# runas.reg): the desktop's and File Explorer's item menu (shell32) lists it
# for programs, their shortcuts, batch files and installer packages (David
# 2026-10-02), and choosing it runs the program -- through the elevation
# broker on a Stained Glass PC; here, with none, the program itself. Without
# the file the menu has no such item (the gate's mutant: SG_RUNAS_REG=/dev/null).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
REG="${SG_RUNAS_REG:-$HERE/defaults/43-sg-runas.reg}"
RC=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }
[ -x "$WINE_DIR/bin/wine" ] || { echo "SKIP: no wine at $WINE_DIR"; exit 77; }
command -v x86_64-w64-mingw32-gcc >/dev/null || { echo "SKIP: no mingw"; exit 77; }
T=$(mktemp -d /var/tmp/sg-runas-check.XXXXXX)
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" PATH="$WINE_DIR/bin:$PATH" DISPLAY=
trap 'wineserver -k 2>/dev/null; rm -rf "$T"' EXIT INT TERM
wine wineboot -i >/dev/null 2>&1; wineserver -w
D="$WINEPREFIX/drive_c"
x86_64-w64-mingw32-gcc -municode -O2 -o "$D/probe.exe" "$HERE/test/sg-menu-probe.c" -lole32 -lshell32 -luuid || { fail "probe did not build"; exit 1; }
printf '#include <stdio.h>\nint main(void){ FILE *f = fopen("C:\\\\ran.txt", "w"); if (f) { fputs("ran", f); fclose(f); } return 0; }\n' > "$T/app.c"
x86_64-w64-mingw32-gcc -O2 -o "$D/app.exe" "$T/app.c"
printf '@echo off\r\n' > "$D/job.bat"; : > "$D/setup.msi"
[ -f "$REG" ] && [ -s "$REG" ] && cp "$REG" "$D/r.reg" && wine reg import 'C:\r.reg' >/dev/null 2>&1
wineserver -w
wine 'C:\probe.exe' link 'C:\app.lnk' 'C:\app.exe' >/dev/null 2>&1
for f in app.exe app.lnk job.bat setup.msi; do
    m=$(wine 'C:\probe.exe' menu "C:\\$f" 2>/dev/null | tr -d '\r')
    printf '%s\n' "$m" | grep -q '^runas	.*administrator' && pass "$f: the menu has 'Run as administrator'" \
        || fail "$f: no runas item ($(printf '%s' "$m" | cut -f1 | tr '\n' ' '))"
done
rm -f "$D/ran.txt"
wine 'C:\probe.exe' invoke 'C:\app.exe' runas >/dev/null 2>&1; sleep 2
[ -f "$D/ran.txt" ] && pass "choosing it runs the program" || fail "runas did not run it"
rm -f "$D/ran.txt"
wine 'C:\probe.exe' invoke 'C:\app.lnk' runas >/dev/null 2>&1; sleep 2
[ -f "$D/ran.txt" ] && pass "...a shortcut's too (its target)" || fail "runas on the shortcut did not run its target"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
