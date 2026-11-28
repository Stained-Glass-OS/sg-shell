#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate: Settings > Network & Internet > Proxy saves the connection's proxy settings where every
# kind of Windows program reads them. test/proxy-page-driver.c opens the real page (sg-settings64.exe
# --page network-proxy) on an Xvfb of its own, sets "Use a proxy server", the address, the exceptions
# and "local addresses" through the page's own controls, clicks Save, and asks:
#   - WinHTTP's view of the user's settings (WinHttpGetIEProxyConfigForCurrentUser: Chrome, Edge,
#     .NET's HttpClient). It reads only Connections\DefaultConnectionSettings; the page used to write
#     ProxyEnable/ProxyServer alone, so those programs went direct;
#   - WinINet's per-connection options;
#   - the page opened again; then the proxy turned off again.
#   SG_SETTINGS_EXE=... another build (the old one fails); SG_WINE_DIR another Wine.
# Run it in a private network namespace (no listener here, but it starts Xvfb):
#   /var/tmp/sgwork/netns-run.sh sh test/proxy-page-check.sh
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_SETTINGS_EXE:-$HERE/build/sg-settings64.exe}"
DPY="${SG_PROXY_DPY:-4713}"; XP=""
for need in Xvfb x86_64-w64-mingw32-gcc; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
T=$(mktemp -d /var/tmp/sg-proxypage.XXXXXX)
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    sg_kill_prefix "$T/pfx" 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM
TMPDIR=/var/tmp x86_64-w64-mingw32-gcc -O1 -o "$T/driver.exe" "$HERE/test/proxy-page-driver.c" -lwininet -lwinhttp \
    || { echo "FAIL  the driver did not build"; exit 1; }
Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
timeout 300 wine wineboot --init >/dev/null 2>&1; wineserver -w
cp "$EXE" "$T/pfx/drive_c/sg-settings64.exe"
cd "$T/pfx/drive_c" || exit 1
out=$(timeout 120 wine "$T/driver.exe" 'C:\sg-settings64.exe' 2>/dev/null | tr -d '\r')
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -qx 'RESULT: PASS'
