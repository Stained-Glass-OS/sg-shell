#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# The Visual C++ runtime is marked present (defaults/41-sg-vcruntime.reg):
# installers look at VisualStudio\14.0\VC\Runtimes before running the
# redistributable; without the marks Winamp's ran it with no arguments, its
# window waited for "I agree", and a silent install hung (Store walk
# 2026-10-01). Checked as a 32-bit and a 64-bit installer see the registry;
# with Winamp's installer at $SG_WINAMP_SETUP (sha256 d064eefc...), also that
# its silent install finishes without the redistributable's window.
# Needs wine-sg (SG_WINE_DIR); skips (77) without it.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
REG="${SG_VCRUNTIME_REG:-$HERE/defaults/41-sg-vcruntime.reg}"
RC=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }
[ -x "$WINE_DIR/bin/wine" ] || { echo "SKIP: no wine-sg at $WINE_DIR"; exit 77; }
T=$(mktemp -d /var/tmp/sg-vcruntime-check.XXXXXX)
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" PATH="$WINE_DIR/bin:$PATH"
trap 'wineserver -k 2>/dev/null; rm -rf "$T"' EXIT INT TERM
wine wineboot -i >/dev/null 2>&1; wineserver -w
if [ -f "$REG" ]; then
    cp "$REG" "$WINEPREFIX/drive_c/vc.reg"
    wine reg import 'C:\vc.reg' >/dev/null 2>&1; wineserver -w
fi
for view in 32 64; do
    for arch in x86 x64; do
        out=$(wine reg query "HKLM\\Software\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\$arch" /reg:$view 2>/dev/null | tr -d '\r')
        echo "$out" | grep -q 'Installed *REG_DWORD *0x1' && echo "$out" | grep -q 'Version *REG_SZ *v14\.' \
            && pass "the $arch runtime is marked installed, as a $view-bit installer sees it" \
            || fail "$arch runtime, $view-bit view: $(echo "$out" | tr '\n' ' ')"
    done
done
if [ -f "${SG_WINAMP_SETUP:-}" ] && command -v xvfb-run >/dev/null; then
    cp "$SG_WINAMP_SETUP" "$WINEPREFIX/drive_c/wa.exe"
    (cd "$WINEPREFIX/drive_c" && timeout 300 xvfb-run -a wine wa.exe /S >/dev/null 2>&1); wineserver -w
    [ -f "$WINEPREFIX/drive_c/Program Files (x86)/Winamp/winamp.exe" ] \
        && pass "Winamp's silent install finishes, without running the redistributable" \
        || fail "Winamp's silent install did not finish (the redistributable's window?)"
else echo "      (SG_WINAMP_SETUP not given: Winamp's install not tried)"; fi
[ "$RC" = 0 ] && echo "vcruntime-check: PASS" || echo "vcruntime-check: FAIL"
exit "$RC"
