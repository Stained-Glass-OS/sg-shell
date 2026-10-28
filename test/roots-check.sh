#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for sg-roots (src/sg-roots.c): Windows' code signing and time stamping roots in the
# machine's Root store, each marked with its uses (the enhanced key usage property).
#  1. a bundle's roots are added, each marked with the uses the bundle gives
#  2. a root already there that the tool did not add (an administrator's) is left unmarked
#  3. a second sync changes nothing
#  4. a root the bundle no longer has is removed; the administrator's stays
# Mutants: SG_MUTANT_NOUSES (roots not marked), SG_MUTANT_NOREMOVE (an old root stays).
#   SG_WINE_DIR=/opt/wine-sg sh test/roots-check.sh
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WD="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="$WD/bin/wine"; WINESERVER="$WD/bin/wineserver"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
command -v "$MINGW" >/dev/null || { echo "SKIP: no mingw"; exit 77; }
command -v openssl >/dev/null || { echo "SKIP: no openssl"; exit 77; }
[ -x "$WINE" ] || { echo "SKIP: no wine at $WINE"; exit 77; }
T=$(mktemp -d /var/tmp/sg-roots-check.XXXXXX)
unset DISPLAY WAYLAND_DISPLAY
export HOME="$T/home" WINEPREFIX="$T/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" WINESERVER
mkdir -p "$HOME"
cleanup() { "$WINESERVER" -k 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
CF="-O2 -municode -mconsole -Wall"
"$MINGW" $CF -o "$T/sg-roots.exe" "$HERE/src/sg-roots.c" -lcrypt32 -ladvapi32 || { echo "FAIL  build"; exit 1; }
"$MINGW" $CF -DSG_MUTANT_NOUSES -o "$T/mut-nouses.exe" "$HERE/src/sg-roots.c" -lcrypt32 -ladvapi32 || exit 1
"$MINGW" $CF -DSG_MUTANT_NOREMOVE -o "$T/mut-noremove.exe" "$HERE/src/sg-roots.c" -lcrypt32 -ladvapi32 || exit 1
"$MINGW" $CF -o "$T/probe.exe" "$HERE/test/roots-probe.c" -lcrypt32 || exit 1
cd "$T" || exit 1
for c in One Two Admin; do
    openssl req -x509 -newkey rsa:2048 -nodes -keyout "$c.key" -out "$c.pem" -days 3650 -subj "/CN=SG Gate $c Root" \
        -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" >/dev/null 2>&1 || { echo "FAIL  certificates"; exit 1; }
done
openssl x509 -in Admin.pem -outform DER -out Admin.der
CS=1.3.6.1.5.5.7.3.3; TS=1.3.6.1.5.5.7.3.8
{ echo "# SG Gate One Root"; echo "# uses: $CS"; cat One.pem
  echo "# SG Gate Two Root"; echo "# uses: $CS $TS"; cat Two.pem
  echo "# SG Gate Admin Root"; echo "# uses: $CS"; cat Admin.pem; } > full.pem
{ echo "# uses: $CS"; cat One.pem; echo "# uses: $CS"; cat Admin.pem; } > less.pem
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1
"$WINESERVER" -w
w() { timeout -s KILL 120 "$WINE" "$@" 2>/dev/null | tr -d '\r'; }
v() { printf '%s\n' "$1" | sed -n "s/^ROOT SG Gate $2 Root uses=//p"; }

w "$T/probe.exe" add 'Z:'"$T/Admin.der" >/dev/null
out=$(w "$T/sg-roots.exe" --sync 'Z:'"$T/full.pem"); echo "      $out"
l=$(w "$T/probe.exe" list)
[ "$(v "$l" One)" = "$CS" ] && [ "$(v "$l" Two)" = "$CS,$TS" ] \
    && pass "the bundle's roots are in the machine's Root store, each marked with its uses" || fail "after sync: $l"
[ "$(v "$l" Admin)" = none ] && pass "an administrator's root, though in the bundle, is left as it was (not narrowed)" || fail "admin root: $(v "$l" Admin)"
out=$(w "$T/sg-roots.exe" --sync 'Z:'"$T/full.pem"); echo "      $out"
case "$out" in *"0 added, 2 kept, 0 removed, 1 left"*) pass "a second sync changes nothing" ;; *) fail "second sync: $out" ;; esac
out=$(w "$T/sg-roots.exe" --sync 'Z:'"$T/less.pem"); echo "      $out"
l=$(w "$T/probe.exe" list)
[ -z "$(v "$l" Two)" ] && [ "$(v "$l" One)" = "$CS" ] && [ "$(v "$l" Admin)" = none ] \
    && pass "a root the bundle no longer has is removed; the others stay" || fail "after the smaller bundle: $l"

# mutants, each in a fresh store state: remove ours (an empty record means a fresh start)
w reg delete 'HKLM\Software\Stained Glass\Code Signing Roots' /f >/dev/null
for c in One Two; do w reg delete 'HKLM\Software\Microsoft\SystemCertificates\Root\Certificates' /f >/dev/null 2>&1; done
w "$T/probe.exe" add 'Z:'"$T/Admin.der" >/dev/null
w "$T/mut-nouses.exe" --sync 'Z:'"$T/full.pem" >/dev/null
l=$(w "$T/probe.exe" list)
[ "$(v "$l" One)" != "$CS" ] && pass "MUTANT NOUSES leaves the roots unmarked (gate catches it)" || fail "mutant NOUSES: $l"
w "$T/sg-roots.exe" --sync 'Z:'"$T/full.pem" >/dev/null
w "$T/mut-noremove.exe" --sync 'Z:'"$T/less.pem" >/dev/null
l=$(w "$T/probe.exe" list)
[ -n "$(v "$l" Two)" ] && pass "MUTANT NOREMOVE keeps the dropped root (gate catches it)" || fail "mutant NOREMOVE: $l"
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
