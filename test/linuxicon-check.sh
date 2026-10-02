#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# A Linux app's .desktop file (Steam puts steam.desktop on the desktop) shows
# the app's icon, not the type's -- the Store's (David 2026-10-02):
# sglinuxicon64.dll, the .desktop type's icon handler (wine-sg 0767 asks it),
# names the icon sg-linuxapp made for the app:
#   - by the file's name (steam.desktop: steam.ico)
#   - else by its Icon= (a path's name without folder and extension)
#   - none made: the type's icon, as before
#
#   SG_WINE_DIR=/opt/wine-sg sh test/linuxicon-check.sh   (needs wine-sg 0767)
# Mutation: build sglinuxicon with -DSG_MUTANT_LINUXICON_NONE: the type's icon.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="${WINE:-$WINE_DIR/bin/wine}"
WINESERVER="${WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
DLL="${SG_LINUXICON_DLL:-$HERE/build/sglinuxicon64.dll}"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
RC=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }
command -v "$MINGW" >/dev/null || { echo "SKIP: $MINGW missing"; exit 77; }
[ -x "$WINE" ] && [ -f "$DLL" ] || { echo "SKIP: wine or $DLL missing"; exit 77; }
unset DISPLAY WAYLAND_DISPLAY
T=$(mktemp -d /var/tmp/sg-linuxicon.XXXXXX)
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" WINESERVER
trap '"$WINESERVER" -k 2>/dev/null; rm -rf "$T"' EXIT INT TERM
"$MINGW" -O2 -municode -o "$T/probe.exe" "$HERE/test/linuxicon-probe.c" -lshell32 -lole32 || { fail "probe did not build"; exit 1; }
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w
C="$WINEPREFIX/drive_c"
cp "$DLL" "$C/sglinuxicon64.dll"; cp "$T/probe.exe" "$C/"
sed 's#Z:\\\\usr\\\\libexec\\\\stained-glass\\\\shell\\\\sglinuxicon64.dll#C:\\\\sglinuxicon64.dll#' \
    "$HERE/defaults/88-sg-linuxapps.reg" > "$T/reg.reg"
grep -q 'C:\\\\sglinuxicon64.dll' "$T/reg.reg" || { fail "registration not found in defaults/88-sg-linuxapps.reg"; exit 1; }
"$WINE" regedit /S 'Z:'"$(printf '%s' "$T/reg.reg" | tr '/' '\\')" >/dev/null 2>&1
"$WINESERVER" -w
LA=$(find "$C/users" -maxdepth 4 -type d -path '*AppData/Local' | head -1)
[ -n "$LA" ] || { fail "no AppData\\Local in the scratch prefix"; exit 1; }
mkdir -p "$LA/Stained Glass/Linux app icons" "$C/desk"
printf 'icon' > "$LA/Stained Glass/Linux app icons/steam.ico"
printf 'icon' > "$LA/Stained Glass/Linux app icons/org.example.Paint.ico"
printf '[Desktop Entry]\nType=Application\nName=Steam\nExec=steam\nIcon=steam\n' > "$C/desk/steam.desktop"
printf '[Desktop Entry]\nType=Application\nName=Paint\nExec=paint\nIcon=/usr/share/icons/hicolor/48x48/apps/org.example.Paint.png\n' > "$C/desk/paint-launcher.desktop"
printf '[Desktop Entry]\nType=Application\nName=Nothing\nExec=nothing\nIcon=nothing-made\n' > "$C/desk/nothing.desktop"
q() { "$WINE" "$C/probe.exe" "$1" 2>/dev/null | tr -d '\r'; }
s=$(q 'C:\desk\steam.desktop'); p=$(q 'C:\desk\paint-launcher.desktop'); n=$(q 'C:\desk\nothing.desktop')
case "$s" in *'\Linux app icons\steam.ico,0') pass "steam.desktop: the app's own icon ($s)" ;; *) fail "steam.desktop: $s" ;; esac
case "$p" in *'\Linux app icons\org.example.Paint.ico,0') pass "by its Icon= path's name ($p)" ;; *) fail "Icon= path: $p" ;; esac
case "$n" in *'sg-store64.exe,0') pass "no icon made: the type's icon ($n)" ;; *) fail "no icon made: $n" ;; esac
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
