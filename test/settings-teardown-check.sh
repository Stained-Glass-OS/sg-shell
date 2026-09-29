#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Settings: a page's controls, as they are destroyed, send nothing to the
# next page. David: selecting Personalization > Lock screen popped up the
# picture Browse dialog by itself -- Background's focused combo box sent
# CBN_KILLFOCUS as it went, with the ID Lock screen uses for Browse.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
WINE="${WINE:-/opt/wine-sg/bin/wine}"
[ -x "$WINE" ] || { echo "SKIP: no wine"; exit 77; }
for t in xvfb-run xdotool x86_64-w64-mingw32-gcc; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -f "$HERE/build/sg-settings64.exe" ] || { echo "SKIP: not built"; exit 77; }
T=$(mktemp -d /var/tmp/sg-teardown.XXXXXX)
export WINEPREFIX="$T/prefix" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
trap '"$(dirname "$WINE")/wineserver" -k 2>/dev/null; rm -rf "$T"' EXIT INT TERM
cat > "$T/wl.c" <<'C'
#include <windows.h>
#include <stdio.h>
static BOOL CALLBACK cb(HWND h, LPARAM l) { WCHAR t[128]; (void)l;
    if (IsWindowVisible(h) && GetWindowTextW(h, t, 128)) printf("%ls\n", t); return TRUE; }
int main(void) { EnumWindows(cb, 0); return 0; }
C
x86_64-w64-mingw32-gcc -o "$T/wl.exe" "$T/wl.c" || { fail "probe did not build"; exit 1; }
timeout -s KILL 300 env DISPLAY= "$WINE" wineboot -i >/dev/null 2>&1
"$(dirname "$WINE")/wineserver" -w   # its desktop had no display: start afresh
cat > "$T/run.sh" <<EOI
#!/bin/sh
"$WINE" "$HERE/build/sg-settings64.exe" ms-settings:personalization &
sleep 15
xdotool mousemove 90 314 click 1
sleep 4
"$WINE" "$T/wl.exe" 2>/dev/null | tr -d '\r' > "$T/windows.txt"
EOI
chmod +x "$T/run.sh"
timeout -s KILL 200 xvfb-run -a -s '-screen 0 1280x800x24' "$T/run.sh" >/dev/null 2>&1
sed 's/^/      /' "$T/windows.txt"
grep -qx Settings "$T/windows.txt" || fail "Settings did not come up"
grep -q "Choose a picture" "$T/windows.txt" && fail "Lock screen opened the picture dialog by itself" \
    || pass "Background > Lock screen: no picture dialog until Browse"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
