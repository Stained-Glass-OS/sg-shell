#!/bin/sh
# The Start menu (sg-start), under a headless shell session.
#
# Shortcuts are planted in the user's and the common Start Menu (one nested
# three folders deep, 200 more for size, one added after sg-start started),
# then the menu is opened the way explorer opens it (SgStartPanel,
# WM_USER+10) and driven with X-level keys and clicks. sg-start writes what it
# shows to SG_START_DUMP after every paint, and the checks read that and the
# screen: the layout above the taskbar, every app with its icon and letter
# headers, "Recently added", the default pinned tiles, type-to-search (apps
# and settings, Enter runs the best match, Escape clears then closes), the
# context menu pinning and unpinning, the power menu and machine policy,
# the expanding rail, keyboard selection, click-away, and how fast it opens.
# Screenshots: build/start-{open,search,context,power,rail}.png.
#
#   make test, or: sh test/start-check.sh
#   SG_WINE=<wine> SG_WINESERVER=<wineserver> to use another Wine build.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="${SG_WINE:-$WINE_DIR/bin/wine}"
export WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
START="$HERE/build/sg-start64.exe"
RC=0; DPY=87; T=$(mktemp -d); chmod 755 "$T"; XP=""
W=1280; H=800; BAR=40
export HOME="$T"
# shellcheck disable=SC2317
cleanup() { "$WINESERVER" -k 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"; }
trap cleanup EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for t in Xvfb xdotool import identify x86_64-w64-mingw32-gcc; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE" ] && [ -f "$START" ] || { echo "SKIP: wine-sg or sg-start not built"; exit 77; }

export WINEPREFIX="$T/pfx" WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all SG_START_DUMP="$T/dump"
"$WINE" wineboot --init >/dev/null 2>&1; "$WINESERVER" -w
"${MINGW64:-x86_64-w64-mingw32-gcc}" -O2 -municode -mwindows -o "$T/poke.exe" "$HERE/test/sg-start-poke.c" 2>/dev/null
"${MINGW64:-x86_64-w64-mingw32-gcc}" -O2 -municode -o "$T/mklnk.exe" "$HERE/test/sg-start-mklnk.c" -lole32 -luuid -lshell32
# programs join the shell's desktop, as sg-run-explorer arranges
# the shell's defaults (fonts, colours) as a new profile gets them
for r in "$HERE"/theme/*.reg; do "$WINE" regedit /S "$("$WINE" winepath -w "$r" 2>/dev/null)" >/dev/null 2>&1; done
"$WINE" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d ${W}x${H} /f >/dev/null 2>&1
USERPROG='C:\users\'"$(id -un)"'\AppData\Roaming\Microsoft\Windows\Start Menu\Programs'
COMMONPROG='C:\ProgramData\Microsoft\Windows\Start Menu\Programs'
"$WINESERVER" -w
"$WINE" "$T/mklnk.exe" "$USERPROG\\Tests\\Deeper\\Deepest" "Zeta Test App" 'C:\windows\notepad.exe' >/dev/null 2>&1
"$WINE" "$T/mklnk.exe" "$COMMONPROG\\Filler" "Filler" 'C:\windows\system32\winver.exe' 200 >/dev/null 2>&1
"$WINESERVER" -w
nlnk=$(find "$WINEPREFIX/drive_c" -path '*Start Menu/Programs*' -name '*.lnk' | wc -l)
[ "$nlnk" -ge 201 ] || { fail "the test's shortcuts were not made ($nlnk)"; echo "RESULT: FAIL"; exit 1; }

rm -f "/tmp/.X${DPY}-lock"
Xvfb ":$DPY" -screen 0 ${W}x${H}x24 -ac >/dev/null 2>&1 & XP=$!; sleep 2
export DISPLAY=":$DPY"
"$WINE" explorer /desktop=shell,${W}x${H} > "$T/explorer.log" 2>&1 &
i=0; while [ $i -lt 60 ] && ! xdotool search --name 'shell - Wine Desktop' >/dev/null 2>&1; do sleep 0.5; i=$((i+1)); done
sleep 2
"$WINE" "$START" >/dev/null 2>&1 &
sleep 4
# added after the menu first ran: "Recently added"
"$WINE" "$T/mklnk.exe" "$USERPROG" "Brand New App" 'C:\windows\notepad.exe' >/dev/null 2>&1

d() { cat "$T/dump" 2>/dev/null; }
has() { d | grep -qxF "$1"; }
val() { d | sed -n "s/^$1=//p"; }
poke() { "$WINE" "$T/poke.exe" >/dev/null 2>&1; sleep 2; }
shot() { import -window root "$HERE/build/start-$1.png" 2>/dev/null; }
panel_y=$((H - BAR - 640))   # the panel sits on the taskbar; 640 px tall at 96 DPI
click() { xdotool mousemove "$1" "$2" click "${3:-1}"; sleep 1.5; }

# --- opening -------------------------------------------------------------------
poke
[ "$(val visible)" = 1 ] && pass "Start opens on SgStartPanel's WM_USER+10 (the Start button, the Windows key)" || { fail "did not open"; echo "RESULT: FAIL"; exit 1; }
P=$(xdotool search --name '^Start$' 2>/dev/null | head -1)
[ "$(val rect)" = "0,$panel_y,708,$((H - BAR))" ] && pass "it stands on the taskbar at the left: $(val rect)" || fail "placement: $(val rect)"
[ "$(val open_ms)" -ge 0 ] 2>/dev/null && [ "$(val open_ms)" -lt 200 ] && pass "it opens in $(val open_ms) ms with $(val apps) apps" || fail "slow to open: $(val open_ms) ms"
has "item Zeta Test App" && pass "apps come from the Start Menu, three folders deep" || fail "the nested shortcut is missing"
[ "$(d | grep -c '^item Filler ')" -ge 200 ] && pass "all of them (200 more in the common Start Menu)" || fail "fillers: $(d | grep -c '^item Filler ')"
has "header Z" && has "header F" && pass "grouped under letter headers" || fail "no letter headers"
d | grep -q '(no icon)' && fail "some apps have no icon: $(d | grep '(no icon)' | head -3)" || pass "every app has its icon"
d | sed -n '/^header Recently added$/{n;p;}' | grep -qxF "item Brand New App" && pass "a new app is under Recently added" || fail "Recently added: $(d | head -12 | tr '\n' '|')"
has "tile File Explorer" && has "tile Notepad" && has "tile Control Panel" && pass "pinned by default: File Explorer, Control Panel, Notepad" || fail "tiles: $(d | grep '^tile')"
shot open
colors=$(import -window "$P" -depth 4 "$T/c.gif" 2>/dev/null; identify -format '%k' "$T/c.gif" 2>/dev/null || echo 1)
[ "${colors:-1}" -ge 8 ] && pass "it paints icons, tiles and text ($colors colours)" || fail "flat panel ($colors colours)"

# --- keyboard -------------------------------------------------------------------
xdotool key Down; sleep 0.8
d | grep -q '^selected ' && pass "arrow keys select in the list: $(d | sed -n 's/^selected //p')" || fail "no selection on Down"
xdotool key Tab; sleep 0.8
[ "$(d | sed -n 's/^selected-tile //p')" = "File Explorer" ] && pass "Tab goes to the tiles" || fail "Tab: $(d | grep selected)"

# --- search ----------------------------------------------------------------------
xdotool type --delay 80 note; sleep 1.5
[ "$(val search)" = note ] && [ "$(d | sed -n 's/^best //p')" = Notepad ] && pass "typing searches; Notepad is the best match for 'note'" || fail "search: $(val search) / $(d | grep '^best')"
has "item File Explorer" && fail "search results still list File Explorer" || pass "and only what matches is listed"
shot search
xdotool key Escape; sleep 1
[ "$(val search)" = "" ] && [ "$(val visible)" = 1 ] && pass "Escape clears the search, and Start stays open" || fail "Escape: visible $(val visible) search '$(val search)'"
xdotool type --delay 80 uninstall; sleep 1.5
[ "$(d | sed -n 's/^best //p')" = "Apps & features" ] && pass "settings are found too: 'uninstall' -> Apps & features" || fail "settings search: $(d | grep '^best')"
xdotool key Escape; sleep 0.8; xdotool key Escape; sleep 1
[ "$(val visible)" = 0 ] && pass "a second Escape closes Start" || fail "still open"

poke; xdotool type --delay 80 note; sleep 1; xdotool key Return; sleep 3
[ "$(val launched)" = Notepad ] && [ "$(val visible)" = 0 ] && pass "Enter runs the best match and closes Start" || fail "Enter: launched '$(val launched)' visible $(val visible)"
xdotool search --name 'Notepad' >/dev/null 2>&1 && pass "Notepad is running" || fail "no Notepad window"

# --- context menu: pin and unpin ------------------------------------------------------
poke; xdotool type --delay 80 zeta; sleep 1.5
# the best match row: below the search field and its header, at 96 DPI
click 110 $((panel_y + 112)) 3
[ "$(val menu)" = context ] && pass "right-click opens the app's menu: $(val menu_items)" || fail "no context menu: $(val menu)"
case "$(val menu_items)" in "Pin to Start,Run as administrator,Open file location,Uninstall") pass "Pin to Start, Run as administrator, Open file location, Uninstall" ;; *) fail "context items: $(val menu_items)" ;; esac
shot context
xdotool key p; sleep 1.2
has "tile Zeta Test App" && pass "Pin to Start adds a tile" || fail "not pinned: $(d | grep '^tile')"
"$WINE" reg query 'HKCU\Software\Stained Glass\Start' /v Pinned 2>/dev/null | grep -q 'Zeta Test App' && pass "the pins are kept in HKCU\\Software\\Stained Glass\\Start" || fail "Pinned not in the registry"
xdotool key Escape; sleep 0.8
# the new tile: the fourth pinned (File Explorer, Control Panel, Notepad, RDC, Zeta) -> second row, second column
n=$(d | grep -c '^tile')
col=$(( (n - 1) % 3 )); row=$(( (n - 1) / 3 ))
click $((384 + col * 104 + 50)) $((panel_y + 40 + row * 104 + 50)) 3
case "$(val menu_items)" in "Unpin from Start,"*) pass "a tile's menu offers Unpin" ;; *) fail "tile menu: $(val menu_items)" ;; esac
xdotool key p; sleep 1.2
has "tile Zeta Test App" && fail "still pinned" || pass "Unpin from Start removes the tile"

# --- the rail -----------------------------------------------------------------------
click 24 $((panel_y + 28))
[ "$(val rail_open)" = 1 ] && pass "the menu button opens the rail with its labels" || fail "rail did not open"
shot rail
click 24 $((panel_y + 28))
click 24 $((panel_y + 640 - 28))
[ "$(val menu)" = power ] && pass "Power opens its menu: $(val menu_items)" || fail "no power menu"
case "$(val menu_items)" in "Lock,Sign out,Restart,Shut down") pass "Lock, Sign out, Restart, Shut down" ;; *) fail "power items: $(val menu_items)" ;; esac
shot power
xdotool key Escape; sleep 0.8
"$WINE" reg add 'HKLM\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer' /v NoClose /t REG_DWORD /d 1 /f >/dev/null 2>&1
click 24 $((panel_y + 640 - 28))
case "$(val menu_items)" in "Lock,Sign out") pass "machine policy NoClose takes Restart and Shut down away" ;; *) fail "with NoClose: $(val menu_items)" ;; esac
xdotool key Escape; sleep 0.8

# --- click away --------------------------------------------------------------------------
[ "$(val visible)" = 1 ] || poke
click 1000 300
[ "$(val visible)" = 0 ] && pass "clicking outside closes Start" || fail "still open after clicking away"

echo
[ "$RC" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
