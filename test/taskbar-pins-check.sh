#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# The taskbar's first pins (defaults/89-sg-taskbar-pins.reg; the taskbar
# side is wine-sg 0741, gated there by test/defaultpins-gate.sh). David
# wanted SG Store pinned out of the box. The layout must name File Explorer
# then SG Store, each "Name|program", SG Store at the path this package
# installs sg-store64.exe to -- a wrong path is silently left out.
#
#   sh test/taskbar-pins-check.sh
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
REG="${REG:-$HERE/defaults/89-sg-taskbar-pins.reg}"
RC=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }

PINS=$(python3 - "$REG" <<'PY'
import re, sys
text = open(sys.argv[1], encoding="ascii").read()  # (a UTF-16 one would need decoding here)
key = re.search(r"^\[HKEY_LOCAL_MACHINE\\Software\\Stained Glass\\Taskbar\]$", text, re.M)
m = re.search(r'^"DefaultPins"=hex\(7\):((?:[0-9a-f]{2},?\\?\n?\s*)+)', text[key.end():] if key else "", re.M)
if not m:
    sys.exit(1)
data = bytes(int(b, 16) for b in re.findall(r"[0-9a-f]{2}", m.group(1)))
# an ASCII file's hex(7) bytes are ANSI text (reg import), a UTF-16 file's UTF-16
raw = open(sys.argv[1], "rb").read(2)
for s in data.decode("utf-16-le" if raw == b"\xff\xfe" else "latin-1").split("\0"):
    if s:
        print(s)
PY
) || { fail "no DefaultPins REG_MULTI_SZ under HKLM\\Software\\Stained Glass\\Taskbar in $REG"; echo "RESULT: FAIL"; exit 1; }
printf '%s\n' "$PINS" | sed 's/^/      /'
[ "$(printf '%s\n' "$PINS" | sed -n 1p)" = 'File Explorer|%SystemRoot%\explorer.exe' ] \
    && pass "File Explorer first" || fail "first pin: $(printf '%s\n' "$PINS" | sed -n 1p)"
STORE=$(printf '%s\n' "$PINS" | sed -n 's/^SG Store|//p')
[ -n "$STORE" ] && pass "SG Store is pinned ($STORE)" || fail "no SG Store pin"
# where debian/rules puts sg-store64.exe, as Wine sees it
DIR=$(awk '/build\/sg-store64.exe/ { s = 1 } s && /debian\/sg-shell\/usr\/libexec/ { print; exit }' "$HERE/debian/rules" | sed 's/.*debian\/sg-shell//; s/[[:space:]]*$//')
WANT="Z:$(printf '%s' "${DIR%/}/sg-store64.exe" | tr '/' '\\')"
[ "$STORE" = "$WANT" ] && pass "at the path the package installs it to" || fail "SG Store pin $STORE, the package installs $WANT"
[ "$(printf '%s\n' "$PINS" | grep -c '|')" = "$(printf '%s\n' "$PINS" | grep -c .)" ] \
    && pass "every entry is Name|program" || fail "malformed entries"

# and Wine's reg import reads the same (an ASCII file's hex(7) bytes are ANSI
# text to it: UTF-16 bytes there would arrive as "F\0i\0l\0e...")
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
if [ -x "$WINE_DIR/bin/wine" ]; then
    unset DISPLAY WAYLAND_DISPLAY
    T=$(mktemp -d /var/tmp/sg-pins-check.XXXXXX)
    export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
    W="$WINE_DIR/bin/wine"
    timeout -s KILL 300 "$W" wineboot -i >/dev/null 2>&1
    "$W" reg import "$("$W" winepath -w "$REG" | tr -d '\r')" >/dev/null 2>&1
    GOT=$("$W" reg query 'HKLM\Software\Stained Glass\Taskbar' /v DefaultPins 2>/dev/null | tr -d '\r' | sed -n 's/.*REG_MULTI_SZ *//p')
    "$WINE_DIR/bin/wineserver" -k 2>/dev/null; rm -rf "$T"
    WANTQ=$(printf '%s\n' "$PINS" | awk 'NR > 1 { printf "\\0" } { printf "%s", $0 }')
    [ "$GOT" = "$WANTQ" ] && pass "Wine's reg import reads the same list" || fail "reg import gave: $GOT"
else
    echo "SKIP  no Wine at $WINE_DIR: reg import not tried"
fi

echo
[ "$RC" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
