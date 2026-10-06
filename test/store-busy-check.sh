#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# SG Store while it downloads a big installer (the regression walk,
# 2026-10-05: during the Visual Basic 6 runtime's 332 MB download the window
# stopped painting and said only "Installing...").
#
# A pinned app whose download is a 96 MB file served slowly from this machine
# (about 8 s); its page (--page) is opened in the shell's desktop and Install
# clicked. While it downloads:
#   - the window answers at once (a message round trip and a repaint,
#     every 100 ms: none over half a second);
#   - it says how far the download is ("Downloading... N%"), the share growing
#     (the dump's "progress" lines, as painted);
# and then the download's SHA-256 is checked as always (here: it is not the
# catalogue's, so nothing is run and the page says why).
#
# Screenshot: build/store-busy-store.png.
#
# Mutant (built here from source): SG_MUTANT_NO_PROGRESS -- no progress shown.
#
#   SG_WINE=<wine> sh test/store-busy-check.sh
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE="${SG_WINE:-/opt/wine-sg/bin/wine}"
WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; XP=; HP=
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for need in Xvfb xdotool python3 "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE" ] || { echo "SKIP: no wine at $WINE"; exit 77; }

T=$(mktemp -d /var/tmp/sg-store-busy.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINESERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    [ -n "$HP" ] && kill "$HP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

STORE_SRC="$HERE/src/store/main.c $HERE/src/store/details.c $HERE/src/store/catalog.c $HERE/src/store/sysinstall.c $HERE/src/store/icons.c $HERE/src/browser/fetch.c $HERE/src/browser/manifest.c $HERE/src/zip/zipcore.c"
STORE_LIBS="-lsetupapi -lwininet -lbcrypt -lshlwapi -lshell32 -lgdi32 -luser32 -ladvapi32 -lole32 -luuid -lwindowscodecs -lmsimg32 -lcomdlg32"
build() { # outfile [define]
    # shellcheck disable=SC2086
    "$MINGW" -municode -mwindows -O1 -Wno-missing-field-initializers -I"$HERE/src/browser" -I"$HERE/src/store" -I"$HERE/src/zip" \
        ${2:+"-D$2"} -o "$1" $STORE_SRC $STORE_LIBS 2>>"$T/cc.log"
}
build "$T/store.exe" || { fail "the store does not build: $(tail -3 "$T/cc.log")"; exit 1; }
build "$T/mut-noprogress.exe" SG_MUTANT_NO_PROGRESS || { fail "mutant NO_PROGRESS does not build"; exit 1; }
"$MINGW" -municode -O1 -o "$T/busyprobe.exe" "$HERE/test/sg-store-busyprobe.c" -luser32 || { fail "the probe does not build"; exit 1; }

# a slow server: 96 MB in 64 KB pieces, about 12 MB/s
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
cat > "$T/slow.py" <<'EOF'
import http.server, sys, time
SIZE = 96 * 1024 * 1024
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(SIZE))
        self.end_headers()
        piece = b"\x5a" * 65536
        for _ in range(SIZE // 65536):
            self.wfile.write(piece)
            time.sleep(0.005)
    def log_message(self, *a): pass
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
EOF
python3 "$T/slow.py" "$PORT" & HP=$!

Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
DPY=$(cat "$T/display" 2>/dev/null); [ -n "$DPY" ] || { echo "FAIL  Xvfb did not start"; exit 1; }
case "$DPY" in 0) echo "FAIL  not the host's display"; exit 1 ;; esac
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDEBUG=-all WINESERVER
export WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
"$WINE" wineboot --init >/dev/null 2>&1; "$WINESERVER" -w
reg() { "$WINE" reg add "$@" /f >/dev/null 2>&1; }
K='HKLM\Software\Stained Glass\Store\Apps'
reg "$K\\01" /v Name /d 'Big App'; reg "$K\\01" /v Publisher /d 'The gate'
reg "$K\\01" /v Description /d 'Big App, a gate app with a big download.'; reg "$K\\01" /v Category /d Utilities
reg "$K\\01" /v Tier /d windows; reg "$K\\01" /v DetectName /d 'Big App'
reg "$K\\01" /v Source /d "pin:http://127.0.0.1:$PORT/big.exe|0000000000000000000000000000000000000000000000000000000000000000|exe|/S"
reg "$K\\01" /v PinVersion /d '1.0'
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
"$WINESERVER" -w
"$WINE" explorer /desktop=shell,1024x768 >/dev/null 2>&1 &
sleep 4
G="$WINEPREFIX/drive_c/gate"; mkdir -p "$G"
export SG_STORE_DUMP='C:\gate\dump' SG_STORE_DIRECT=1
D="$G/dump"
waitfor() { i=0; while ! grep -q "$2" "$1" 2>/dev/null && [ $i -lt "${3:-80}" ]; do sleep 0.25; i=$((i + 1)); done; grep -q "$2" "$1" 2>/dev/null; }

trial() { # exe -> sets BUSY, PROG (distinct progress values seen), MSG
    rm -f "$D"; BUSY=; PROG=0; MSG=
    "$WINE" "$1" --page 01 >/dev/null 2>&1 &
    waitfor "$D" '^detail 01 ready=1' 120 || { fail "--page 01 did not open: $(grep '^detail' "$D" | head -2)"; return; }
    sleep 1
    xy=$(grep '^hit install 01 ' "$D" | tail -1 | awk '{ print $(NF-1), $NF }')
    [ -n "$xy" ] || { fail "no Install button: $(grep '^hit' "$D" | head -3 | tr '\n' '|')"; return; }
    xdotool mousemove "${xy% *}" "${xy#* }" click 1
    waitfor "$D" '^progress 01 download' 40
    : > "$T/seen"
    ( i=0; while [ $i -lt 30 ]; do grep '^progress 01 download' "$D" 2>/dev/null >> "$T/seen"; sleep 0.2; i=$((i + 1)); done ) &
    SP=$!
    BUSY=$("$WINE" "$T/busyprobe.exe" 5 2>/dev/null | tr -d '\r')
    command -v import >/dev/null && import -window root "$HERE/build/store-busy-$(basename "$1" .exe).png" 2>/dev/null
    wait "$SP"
    PROG=$(sort -u "$T/seen" | wc -l)
    waitfor "$D" '^msg 01 ' 120; MSG=$(sed -n 's/^msg 01 //p' "$D" | tail -1)
    "$WINE" taskkill /f /im "$(basename "$1")" >/dev/null 2>&1; sleep 1
}
trial "$T/store.exe"
echo "      $BUSY; $PROG progress readings; then: $MSG"
case "$BUSY" in "BUSY maxms "*" slow 0 of "*) pass "the window answers at once while it downloads ($BUSY)" ;;
    *) fail "the window does not answer while it downloads: ${BUSY:-nothing}" ;; esac
[ "$PROG" -ge 3 ] && pass "it shows the download's progress, growing ($PROG readings): $(tail -1 "$T/seen")" \
    || fail "no progress while downloading ($PROG readings): $(tail -1 "$T/seen")"
case "$MSG" in *SHA-256*) pass "and then checks the download as always: $MSG" ;; *) fail "after the download: '$MSG'" ;; esac
cp "$T/seen" "$T/seen.good"
trial "$T/mut-noprogress.exe"
[ "$PROG" -lt 3 ] && pass "MUTANT NO_PROGRESS shows no progress (gate catches it)" || fail "NO_PROGRESS not detected ($PROG readings)"
echo
if [ "$RC" = 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
