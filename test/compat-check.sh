#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for the Compatibility tab of a program's Properties (src/compat,
# sgcompat64.dll), on its own Xvfb display:
#
#   - Wine's Properties dialog for an .exe shows a "Compatibility" tab
#     (the property sheet handler registered for exefile loads)
#   - setting Windows 7, run as administrator, a window of its own, OpenGL
#     (WineD3D), two environment variables and launch arguments, then Apply,
#     stores each where Wine and Windows look (AppDefaults\<exe>: Version,
#     Explorer\Desktop, DllOverrides, Environment, LaunchArgs;
#     AppCompatFlags\Layers\<path>: "~ WIN7RTM RUNASADMIN")
#   - recommended settings for the program (SG_COMPAT_PRESETS) are offered
#     and applied
#
# Screenshot: build/compat-tab.png. Needs wine-sg, Xvfb, ImageMagick and
# mingw; skips (77) without them.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="${WINE:-$WINE_DIR/bin/wine}"
WINESERVER="${WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
DLL="${SG_COMPAT_DLL:-$HERE/build/sgcompat64.dll}"
OUT="$HERE/build"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
RC=0; DPY=$(( 500 + $$ % 200 )); XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb import "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE" ] && [ -f "$DLL" ] || { echo "SKIP: wine or $DLL missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-compat-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINESERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

"$MINGW" -O2 -municode -o "$T/compat-probe.exe" "$HERE/test/compat-probe.c" -lole32 -luuid -lshell32 -lcomctl32 -luser32 \
    || { fail "probe did not build"; exit 1; }
Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
sleep 2
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEDEBUG=-all WINESERVER
export WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
mkdir -p "$WINEPREFIX"
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1
"$WINESERVER" -w
C="$WINEPREFIX/drive_c"
cp "$DLL" "$C/sgcompat64.dll"
cp "$T/compat-probe.exe" "$C/game.exe"
# the package's registration, pointed at this build
sed 's#Z:\\\\usr\\\\libexec\\\\stained-glass\\\\shell\\\\sgcompat64.dll#C:\\\\sgcompat64.dll#' \
    "$HERE/defaults/84-sg-compat.reg" > "$T/reg.reg"
"$WINE" regedit /S 'Z:'"$(printf '%s' "$T/reg.reg" | tr '/' '\\')" >/dev/null 2>&1
"$WINESERVER" -w
q() { "$WINE" reg query "$1" /v "$2" 2>/dev/null | tr -d '\r' | awk -v n="$(printf '%s' "$2" | sed 's/\\/\\\\/g')" '$1 == n { $1 = ""; $2 = ""; sub(/^  */, ""); print }'; }
A='HKCU\Software\Wine\AppDefaults\game.exe'

# 1. Wine's own Properties dialog shows the tab
out=$(timeout -s KILL 60 "$WINE" "$T/compat-probe.exe" tabs 'C:\game.exe' 2>/dev/null | tr -d '\r')
echo "$out" | sed 's/^/      /'
echo "$out" | grep -qx 'TAB Compatibility' && pass "an .exe's Properties has a Compatibility tab" || fail "no Compatibility tab: $out"

# 2. every setting, applied
out=$(SG_COMPAT_SHOT=1 timeout -s KILL 60 "$WINE" "$T/compat-probe.exe" set 'C:\game.exe' 2>/dev/null | tr -d '\r') &
PID=$!; sleep 3; import -window root "$OUT/compat-tab.png" 2>/dev/null; wait $PID
"$WINESERVER" -w
[ "$(q "$A" Version)" = win7 ] && pass "Windows 7: AppDefaults Version = win7" || fail "Version: '$(q "$A" Version)'"
L=$(q 'HKCU\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers' 'C:\game.exe')
[ "$L" = "~ WIN7RTM RUNASADMIN" ] && pass "Layers: '$L', as Windows writes it" || fail "Layers: '$L'"
[ "$(q "$A\\Explorer" Desktop)" = game.exe ] && [ "$(q 'HKCU\Software\Wine\Explorer\Desktops' game.exe)" = 1024x768 ] \
    && pass "a window of its own, 1024x768" || fail "desktop: '$(q "$A\\Explorer" Desktop)' '$(q 'HKCU\Software\Wine\Explorer\Desktops' game.exe)'"
[ "$(q "$A\\DllOverrides" d3d11)" = builtin ] && [ "$(q "$A\\DllOverrides" dxgi)" = builtin ] \
    && pass "OpenGL (WineD3D): d3d11 and dxgi builtin" || fail "graphics: '$(q "$A\\DllOverrides" d3d11)'"
[ "$(q "$A\\Environment" SGTEST_COMPAT)" = "from the tab" ] && [ "$(q "$A\\Environment" DXVK_HUD)" = fps ] \
    && pass "environment variables stored" || fail "environment: '$(q "$A\\Environment" SGTEST_COMPAT)'"
[ "$(q "$A" LaunchArgs)" = "-dx11 -windowed" ] && pass "launch arguments stored" || fail "LaunchArgs: '$(q "$A" LaunchArgs)'"

# 3. recommended settings
printf '[game.exe]\r\nNote=Tested: needs Vulkan and async shader compilation.\r\nGraphics=vulkan\r\nEnv=DXVK_ASYNC=1\r\n' > "$C/presets.ini"
out=$(SG_COMPAT_PRESETS='C:\presets.ini' timeout -s KILL 60 "$WINE" "$T/compat-probe.exe" preset 'C:\game.exe' 2>/dev/null | tr -d '\r')
"$WINESERVER" -w
echo "$out" | grep -q '^PRESET Tested: needs Vulkan' && pass "the recommended settings are offered" || fail "preset text: $out"
[ "$(q "$A\\DllOverrides" d3d11)" = "native,builtin" ] && [ "$(q "$A\\Environment" DXVK_ASYNC)" = 1 ] \
    && pass "and applied: Vulkan (DXVK), DXVK_ASYNC=1" || fail "preset applied: '$(q "$A\\DllOverrides" d3d11)' '$(q "$A\\Environment" DXVK_ASYNC)'"
[ -z "$(q "$A\\Environment" SGTEST_COMPAT)" ] && pass "the environment was replaced, not merged" || fail "old variable kept"

[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
