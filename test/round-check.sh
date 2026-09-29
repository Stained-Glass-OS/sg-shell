#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# The shell's flyouts ask for round corners (sg-round.h: DWMWA_WINDOW_CORNER_
# PREFERENCE = DWMWCP_ROUND), which the Rounded style gives them (wine-sg
# 0497) and the Classic style does not. Here the volume flyout and Start's
# panel (the network and battery flyouts, and the clock's toasts, call the
# same helper but need a network daemon or a battery to start). Read back
# with DwmGetWindowAttribute, which a wine-sg before 0483 does not answer
# (SKIP).
#
#   sh test/round-check.sh     SG_WINE=<wine> SG_WINESERVER=<wineserver>
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE="${SG_WINE:-${SG_WINE_DIR:-/opt/wine-sg}/bin/wine}"
WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for f in sg-volume64.exe sg-start64.exe; do [ -f "$HERE/build/$f" ] || { echo "SKIP: build/$f missing"; exit 77; }; done
[ -x "$WINE" ] && command -v "$MINGW" >/dev/null && command -v xvfb-run >/dev/null || { echo "SKIP: needs wine-sg, $MINGW, xvfb-run"; exit 77; }
T=$(mktemp -d /var/tmp/sg-round-check.XXXXXX)
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINESERVER
trap '"$WINESERVER" -k 2>/dev/null; rm -rf "$T"' EXIT INT TERM
"$MINGW" -O2 -municode -o "$T/probe.exe" "$HERE/test/round-probe.c" || { fail "probe did not build"; exit 1; }
DISPLAY= timeout 300 "$WINE" wineboot -i >/dev/null 2>&1
"$WINESERVER" -w
"$WINE" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x700 /f >/dev/null 2>&1
"$WINESERVER" -w
cat > "$T/session.sh" <<EOF
#!/bin/sh
"$WINE" explorer /desktop=shell,1024x700 >/dev/null 2>&1 &
sleep 6
"$WINE" "$HERE/build/sg-volume64.exe" >/dev/null 2>&1 &
"$WINE" "$HERE/build/sg-start64.exe" >/dev/null 2>&1 &
sleep 10
for w in "|Volume" "SgStartWindow|Start"; do
    echo "\$w \$("$WINE" "$T/probe.exe" "\${w%%|*}" "\${w#*|}" 2>/dev/null | tr -d '\r')"
done > "$T/out"
EOF
chmod +x "$T/session.sh"
timeout -s KILL 200 xvfb-run -a -s '-screen 0 1024x700x24' "$T/session.sh"
sed 's/^/      /' "$T/out"
grep -q "hr=80004001\|hr=80070057" "$T/out" && { echo "SKIP: this wine-sg does not report the corner preference (before 0483)"; exit 77; }
for w in "|Volume" "SgStartWindow|Start"; do
    l=$(grep -F "$w " "$T/out")
    case "$l" in *"pref=2 hr=00000000"*) pass "${w#*|}: asks for round corners (DWMWCP_ROUND)" ;; *) fail "${w#*|}: $l" ;; esac
done
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
