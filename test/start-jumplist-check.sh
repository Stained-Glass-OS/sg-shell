#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# An app's jump list in its Start menu (src/sg-start.c), as Windows 10's Start shows it: the
# recent files and tasks of the app a shortcut starts -- by the shortcut's AppUserModelID (a Linux
# app's, a Windows program's own), else its program -- at the top of the app's menu, each opening
# its file (or running its task).
#   mutant: -DSG_MUTANT_START_NO_JUMP (src/sg-start.c)
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="${SG_WINE:-$WINE_DIR/bin/wine}"
export WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
START="${SG_START_EXE:-$HERE/build/sg-start64.exe}"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; T=$(mktemp -d /var/tmp/sg-startjump.XXXXXX); chmod 755 "$T"; XP=""
W=1280; H=800; BAR=40
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in Xvfb xdotool "$MINGW"; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE" ] && [ -f "$START" ] || { echo "SKIP: wine-sg or sg-start not built"; exit 77; }
unset DISPLAY WAYLAND_DISPLAY
DPY=$((400 + $$ % 200))
cleanup() { "$WINESERVER" -k 2>/dev/null; command -v sg_kill_prefix >/dev/null && sg_kill_prefix "$WINEPREFIX"; [ -n "$XP" ] && kill "$XP" 2>/dev/null; rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"; }
trap cleanup EXIT INT TERM
export WINEPREFIX="$T/pfx" WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all SG_START_DUMP="$T/dump"
mkdir -p "$T/empty"; export XDG_DATA_DIRS="$T/empty" XDG_DATA_HOME="$T/empty"
"$MINGW" -O2 -municode -o "$T/probe.exe" "$HERE/test/start-jumplist-probe.c" -lole32 -luuid -lshell32 || { fail "probe did not build"; exit 1; }
"$MINGW" -O2 -municode -mwindows -o "$T/poke.exe" "$HERE/test/sg-start-poke.c" 2>/dev/null
"$WINE" wineboot --init >/dev/null 2>&1; "$WINESERVER" -w
U=$(id -un)
PROG='C:\users\'"$U"'\AppData\Roaming\Microsoft\Windows\Start Menu\Programs'
echo "jumper notes" > "$WINEPREFIX/drive_c/users/Public/jumpernotes.txt"
"$WINE" "$T/probe.exe" link "$PROG\\Gate Jumper.lnk" 'C:\windows\notepad.exe' Gate.Jumper || fail "the shortcut was not made"
"$WINE" "$T/probe.exe" recent Gate.Jumper 'C:\users\Public\jumpernotes.txt' || fail "the recent document was not added"
"$WINE" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d ${W}x${H} /f >/dev/null 2>&1
"$WINESERVER" -w
rm -f "/tmp/.X${DPY}-lock"
Xvfb ":$DPY" -screen 0 ${W}x${H}x24 -nolisten tcp >/dev/null 2>&1 & XP=$!; sleep 2
export DISPLAY=":$DPY"
"$WINE" explorer /desktop=shell,${W}x${H} > "$T/explorer.log" 2>&1 &
i=0; while [ $i -lt 60 ] && ! xdotool search --name 'shell - Wine Desktop' >/dev/null 2>&1; do sleep 0.5; i=$((i+1)); done
sleep 2
"$WINE" "$START" >/dev/null 2>&1 &
sleep 5
d() { cat "$T/dump" 2>/dev/null; }
val() { d | sed -n "s/^$1=//p"; }
panel_y=$((H - BAR - 640))
"$WINE" "$T/poke.exe" >/dev/null 2>&1; sleep 2
xdotool type --delay 80 'gate jumper'; sleep 1.5
xdotool mousemove 110 $((panel_y + 112)) click 3; sleep 1.5
items=$(val menu_items)
case "$items" in *jumpernotes.txt*"Pin to Start"*) pass "its menu begins with its jump list: the recent file ($items)" ;; *) fail "menu: '$items'" ;; esac
# the file: second in the menu (after the dimmed "Recent" heading, which the keys pass over as Windows' do)
xdotool key Down Down Return; sleep 4
xdotool search --name 'jumpernotes' >/dev/null 2>&1 && pass "choosing it opens the file" || fail "no window for the recent file"
echo
[ "$RC" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
