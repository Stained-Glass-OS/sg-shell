#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners
# Terminal is the default terminal (David 2026-10-03: "All the terminal apps
# should support Tabs. Including the Linux and Windows terminal."): a console
# program started with a console of its own -- cmd here, from start /wait, as
# the Start menu or a shortcut starts PowerShell -- opens as a Terminal tab
# (wine-sg 0787 hands sg-terminal the console, --sg-handoff), not in a console
# window. What is typed in the tab runs; the program keeps its exit code; the
# tab and the window close when it ends. Read from sg-terminal's dump.
#
#   SG_WINE=/path/to/wine (with 0787) test/terminal-handoff-check.sh
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
W="${SG_WINE:-/opt/wine-sg/bin/wine}"
WS=$(dirname "$W")/wineserver; [ -x "$WS" ] || WS=$(dirname "$W")/server/wineserver
EXE="${SG_TERMINAL_EXE:-$HERE/build/sg-terminal64.exe}"
RC=0; XP=""
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }
for need in Xvfb xdotool; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$W" ] && [ -f "$EXE" ] || { echo "SKIP: wine ($W) or $EXE missing"; exit 77; }
# a Wine that hands consoles over (0787): its kernelbase knows SgConsoleHandoff
KB=$(for k in "$(dirname "$W")/../lib/wine/x86_64-windows/kernelbase.dll" "$(dirname "$W")/dlls/kernelbase/x86_64-windows/kernelbase.dll"; do [ -f "$k" ] && echo "$k"; done | head -1)
command -v strings >/dev/null || { echo "SKIP: strings (binutils) missing"; exit 77; }
[ -n "$KB" ] && strings -el "$KB" | grep -q SgConsoleHandoff \
    || { echo "SKIP: this Wine does not hand consoles over (wine-sg 0787; SG_WINE=)"; exit 77; }
T=$(mktemp -d /var/tmp/sg-term-handoff.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WS" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    [ "${KEEP:-0}" = 1 ] && echo "kept $T" || rm -rf "$T" "$SG_GATE_HOME"
}
trap cleanup EXIT INT TERM
Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/display")" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all WINESERVER="$WS"
timeout -s KILL 300 "$W" wineboot --init >/dev/null 2>&1; "$WS" -w
cp "$EXE" "$T/sg-terminal64.exe"
winexe=$("$W" winepath -w "$T/sg-terminal64.exe" 2>/dev/null | tr -d '\r')
esc=$(printf '%s' "$winexe" | sed 's/\\/\\\\\\\\/g')
# the package's own registration, pointed at this build
sed "s|Z:\\\\\\\\usr\\\\\\\\libexec\\\\\\\\stained-glass\\\\\\\\shell\\\\\\\\sg-terminal64.exe|$esc|g" \
    "$HERE/defaults/66-sg-terminal.reg" > "$T/terminal.reg"
grep -q '"SgConsoleHandoff"=dword:00000001' "$T/terminal.reg" && pass "the registration says Terminal takes handed consoles" || fail "no SgConsoleHandoff in 66-sg-terminal.reg"
"$W" reg import "$("$W" winepath -w "$T/terminal.reg" | tr -d '\r')" >/dev/null 2>&1
command -v x86_64-w64-mingw32-gcc >/dev/null && x86_64-w64-mingw32-gcc -O2 -o "$T/pfx/drive_c/conprobe.exe" "$HERE/test/sg-terminal-probe.c" || fail "the probe did not build"
"$WS" -w
DUMP="$T/terminal.dump"
SG_TERMINAL_DUMP="$("$W" winepath -w "$DUMP" | tr -d '\r')"; export SG_TERMINAL_DUMP
D() { sed -n "s/^$1 //p" "$DUMP" 2>/dev/null | head -1; }
wait_grep() { i=0; while ! grep -q "$1" "$DUMP" 2>/dev/null && [ $i -lt $(( ${2:-20} * 4 )) ]; do sleep 0.25; i=$((i + 1)); done; grep -q "$1" "$DUMP" 2>/dev/null; }
# the session's desktop, as on the machine
"$W" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
"$W" reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768 /f >/dev/null 2>&1
WINEDEBUG=trace+explorer "$W" explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
cd "$T/pfx/drive_c" || exit 1
( timeout -s KILL 120 "$W" start /wait cmd /q /k "prompt SG$ " >/dev/null 2>&1; echo "$?" > "$T/rc" ) &
if wait_grep '^tab 1 .*profile=Command Prompt alive=1' 40; then pass "cmd opened as a Terminal tab (Command Prompt's profile)"
else fail "no Terminal tab: $(head -8 "$DUMP" 2>/dev/null | tr '\n' '|')"; echo "RESULT: FAIL"; exit 1; fi
xdotool search --classname 'conhost.exe' > "$T/conwin" 2>/dev/null
[ ! -s "$T/conwin" ] && pass "no console window of its own" || fail "a console window opened too"
# shellcheck disable=SC2046
set -- $(D origin); xdotool mousemove $(( ${1:-100} + 300 )) $(( ${2:-100} + 200 )) click 1; sleep 0.4
xdotool type --delay 60 'set /a 40+2'; xdotool key Return
wait_grep '^row [0-9]*: .*42' 15 && pass "what is typed in the tab runs (set /a 40+2 shows 42)" || fail "no 42: $(grep '^row' "$DUMP" | head -6 | tr '\n' '|')"
# the console is the tab's size, and follows the window when it is resized
probe_size() {   # typed, and on the screen whole, before Enter (keys typed while it redraws can be lost)
    for _try in 1 2 3; do
        xdotool type --delay 80 'c:\conprobe.exe size'
        if wait_grep '^row [0-9]*: .*conprobe.exe size$' 20; then xdotool key Return; return; fi
        xdotool key Escape; sleep 0.5
    done
}
probe_size
s1=$(D size | tr ' ' x)
wait_grep "^row [0-9]*: size=$s1\$" 15 && pass "the console is the tab's size ($s1)" || fail "probe: $(grep 'size=' "$DUMP" | grep '^row' | tail -1) vs $s1"
# typed before, run after: keys typed into a full-screen redraw on Xvfb can be lost
xdotool type --delay 80 'c:\conprobe.exe size'; sleep 3
xdotool key F11; i=0     # full screen: a bigger tab
while [ "$(D size | tr ' ' x)" = "$s1" ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
s2=$(D size | tr ' ' x); sleep 2
xdotool key Return
[ "$s2" != "$s1" ] && wait_grep "^row [0-9]*: size=$s2\$" 45 && pass "resized, the console follows ($s1 -> $s2)" \
    || fail "after a resize the tab is $s2, the console: $(grep '^row' "$DUMP" | grep -v ': $' | tail -3 | tr '\n' '|')"
xdotool key F11; sleep 3
xdotool type --delay 60 'exit 7'; xdotool key Return
i=0; while [ ! -s "$T/rc" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
[ "$(cat "$T/rc" 2>/dev/null)" = 7 ] && pass "it is still the program: start /wait gives its exit code (7)" || fail "start /wait gave '$(cat "$T/rc" 2>/dev/null)'"
i=0; while grep -q '^tabs [1-9]' "$DUMP" 2>/dev/null && pgrep -f '[s]g-terminal64.exe --sg-handoff' >/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
pgrep -f '[s]g-terminal64.exe --sg-handoff' >/dev/null && fail "Terminal stayed open after the program ended: $(D tabs); $(pgrep -a -f '[.]exe' | grep -v -e services -e winedevice -e plugplay -e svchost -e rpcss -e explorer | cut -c1-90 | tr '\n' '|')" || pass "the tab, and Terminal, closed with the program"
# Terminal's Settings: Default terminal application -- Console window turns it off
"$W" start "$winexe" >/dev/null 2>&1
wait_grep '^tab 1 ' 30 || fail "Terminal did not open"
sleep 1; xdotool key ctrl+comma
wait_grep '^setctl 14 ' 15 || fail "no Default terminal application in Settings"
# shellcheck disable=SC2046
set -- $(sed -n 's/^setctl 14 //p' "$DUMP" | head -1); xdotool mousemove "$1" "$2" click 1; sleep 0.8; xdotool key End Return; sleep 0.5
# shellcheck disable=SC2046
set -- $(sed -n 's/^setctl 1 //p' "$DUMP" | head -1); xdotool mousemove "$1" "$2" click 1; sleep 1.5
v=$("$W" reg query 'HKCU\Console\%%Startup' /v DelegationTerminal 2>/dev/null | tr -d '\r' | awk '$1 == "DelegationTerminal" { print $3 }')
[ "$v" = '{B23D10C0-E52E-411E-9D5B-C09FDF709C7D}' ] && pass "Settings > Default terminal application: Console window (DelegationTerminal = the console host)" \
    || fail "DelegationTerminal is '$v'"
"$W" start cmd /k "title SGCONWIN" >/dev/null 2>&1 &
i=0; CW=; while [ -z "$CW" ] && [ $i -lt 40 ]; do sleep 0.5; CW=$(xdotool search --classname 'conhost.exe' 2>/dev/null | head -1); i=$((i + 1)); done
[ -n "$CW" ] && pass "...and a new console opens in a console window again" || fail "no console window with the console host chosen"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
