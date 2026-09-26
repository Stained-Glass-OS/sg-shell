#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for Terminal (sg-terminal, wt.exe): its screen's unit test (native),
# then the program on a shell desktop under Xvfb, driven on the X keyboard and
# mouse and read back from its dump (SG_TERMINAL_DUMP) and screenshots:
#
#   - wt.exe through App Paths opens Command Prompt; what is typed runs and
#     its output is on the screen
#   - Ctrl+Shift+1 opens PowerShell 7 in a second tab: 6*7 is 42, and
#     Write-Host -ForegroundColor Red is drawn red
#   - clicking the first tab goes back to Command Prompt
#   - a mouse selection copies with Ctrl+Shift+C; Ctrl+V pastes (xclip)
#   - full screen (F11) resizes the pseudo console: cmd's `mode con` says the
#     new width
#   - `exit` closes its tab; wt.exe's command line (a command, ';', nt -p)
#     opens the tabs it names
#   - Alt+F4 closes the window
#
# Needs a wine-sg whose pseudo consoles give programs their handles, resize
# and interpret VT (0131, 0132): SG_WINE_DIR. PowerShell 7 from
# SG_PWSH (default /opt/sg-apps/powershell, or sg-image's staged build).
# Screenshots: build/terminal-*.png. SG_TERMINAL_EXE runs another build.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_TERMINAL_EXE:-$HERE/build/sg-terminal64.exe}"
OUT="$HERE/build"
RC=0; DPY="${SG_TERMINAL_DPY:-121}"; XP=""
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }

# --- the screen, natively ----------------------------------------------------------------
if command -v gcc >/dev/null; then
    gcc -O1 -Wall -o "$OUT/terminal-vt-test" "$HERE/test/terminal-vt-test.c" "$HERE/src/terminal/vt.c" &&
        "$OUT/terminal-vt-test" | sed 's/^/  vt: /' && pass "the screen's unit test" || fail "the screen's unit test"
fi

for need in Xvfb xdotool import xclip; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
PWSH="${SG_PWSH:-/opt/sg-apps/powershell}"
[ -f "$PWSH/pwsh.exe" ] || PWSH="$HERE/../../sg-image/build/extra-tree/opt/sg-apps/powershell"
[ -f "$PWSH/pwsh.exe" ] || PWSH=/home/david/Stained-Glass-OS/sg-image/build/extra-tree/opt/sg-apps/powershell

T=$(mktemp -d /var/tmp/sg-terminal-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; [ "${KEEP:-0}" = 1 ] && echo "kept $T" || rm -rf "$T"
}
trap cleanup EXIT INT TERM

Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
# only for creating the prefix: with mscoree disabled .NET programs (PowerShell 7) cannot load
unset WINEDLLOVERRIDES
cp "$EXE" "$T/sg-terminal64.exe"
winexe=$(wine winepath -w "$T/sg-terminal64.exe" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
for f in "$HERE/theme/50-sg-colors.reg" "$HERE/theme/52-sg-fonts.reg"; do
    wine reg import "$(wine winepath -w "$f" | tr -d '\r')" >/dev/null 2>&1
done
esc=$(printf '%s' "$winexe" | sed 's/\\/\\\\\\\\/g')
sed "s|Z:\\\\\\\\usr\\\\\\\\libexec\\\\\\\\stained-glass\\\\\\\\shell\\\\\\\\sg-terminal64.exe|$esc|g" \
    "$HERE/defaults/66-sg-terminal.reg" > "$T/terminal.reg"
wine reg import "$(wine winepath -w "$T/terminal.reg" | tr -d '\r')" >/dev/null 2>&1
# PowerShell 7 where sg-install-apps puts it
HAVE_PWSH=0
if [ -f "$PWSH/pwsh.exe" ]; then
    mkdir -p "$T/pfx/drive_c/Program Files/PowerShell"
    # a copy: through a symbolic link .NET looks for its assemblies beside the link
    cp -r "$PWSH" "$T/pfx/drive_c/Program Files/PowerShell/7"
    HAVE_PWSH=1
fi
# a console program that reports the console's size and writes colours
x86_64-w64-mingw32-gcc -O2 -o "$T/pfx/drive_c/conprobe.exe" "$HERE/test/sg-terminal-probe.c" || fail "the probe did not build"
wineserver -w

WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

DUMP="$T/terminal.dump"
export SG_TERMINAL_DUMP="$(wine winepath -w "$DUMP" | tr -d '\r')"
D() { sed -n "s/^$1 //p" "$DUMP" 2>/dev/null | head -1; }
has_row() { grep -q "^row [0-9]*: $1\$" "$DUMP" 2>/dev/null; }
wait_row() {   # a row that is exactly $1, within $2 seconds
    i=0; while ! has_row "$1" && [ $i -lt $(( ${2:-20} * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    has_row "$1"
}
wait_grep() { i=0; while ! grep -q "$1" "$DUMP" 2>/dev/null && [ $i -lt $(( ${2:-20} * 4 )) ]; do sleep 0.25; i=$((i + 1)); done; grep -q "$1" "$DUMP" 2>/dev/null; }
typ() { xdotool type --delay 60 "$1"; sleep 0.3; }
key() { xdotool key --delay 80 "$@"; sleep 0.4; }
shot() { import -window root "$OUT/terminal-$1.png" 2>/dev/null; }
focus_view() { set -- $(D origin); xdotool mousemove $(( $1 + 300 )) $(( $2 + 200 )) click 1; sleep 0.3; }

wine start wt.exe -p "Command Prompt" >/dev/null 2>&1
if wait_grep '^tab 1 .*profile=Command Prompt alive=1' 30; then pass "wt.exe (App Paths) opens a Command Prompt tab"
else fail "no Command Prompt tab: $(cat "$DUMP" 2>/dev/null | head -12)"; exit 1; fi
wait_grep '^row [0-9]*: C:\\users\\.*>' 30 && pass "cmd's prompt is on the screen, in the user's folder" || fail "no prompt: $(grep '^row' "$DUMP" | head -5)"
focus_view
typ "echo hello from cmd"; key Return
wait_row "hello from cmd" 15 && pass "cmd ran what was typed; its output is on the screen" || fail "no output: $(grep '^row' "$DUMP" | head -8)"
colour_of() {   # the fg codes of the last row that is exactly $1
    y=$(grep "^row [0-9]*: $1\$" "$DUMP" | tail -1 | sed 's/^row \([0-9]*\):.*/\1/')
    [ -n "$y" ] && sed -n "s/^fg $y: //p" "$DUMP"
}
typ "c:\\conprobe.exe vt"; key Return
if wait_row "vtred" 15; then
    case "$(colour_of vtred)" in 99999) pass "a program's own VT colours (SGR 91) come through conhost" ;; *) fail "vtred's colours: '$(colour_of vtred)'" ;; esac
else fail "no vtred: $(grep '^row' "$DUMP" | tail -4)"; fi
typ "c:\\conprobe.exe attr"; key Return
if wait_row "attrgreen" 15; then
    case "$(colour_of attrgreen)" in aaaaaaaaa) pass "SetConsoleTextAttribute colours come through as VT" ;; *) fail "attrgreen's colours: '$(colour_of attrgreen)'" ;; esac
else fail "no attrgreen"; fi
shot cmd

# --- a second tab: PowerShell 7 --------------------------------------------------------------
if [ $HAVE_PWSH = 1 ]; then
    grep -q '^profile 1 PowerShell' "$DUMP" && pass "PowerShell 7 is the first profile" || fail "profiles: $(grep '^profile' "$DUMP")"
    key ctrl+shift+1
    if wait_grep '^tab 2 .*profile=PowerShell alive=1' 20 && [ "$(D tabs)" = "2 active 2" ]; then pass "Ctrl+Shift+1 opens PowerShell in a second tab"
    else fail "tabs: $(grep '^tab' "$DUMP")"; fi
    wait_grep '^row [0-9]*: PS C:\\users\\.*>' 60 && pass "PowerShell's prompt" || fail "no PS prompt: $(grep '^row' "$DUMP" | head -5)"
    typ "Write-Output (6*7)"; key Return
    wait_row "42" 30 && pass "PowerShell 7 ran it: 42" || fail "no 42: $(grep '^row' "$DUMP" | head -10)"
    typ "Write-Host redtext -ForegroundColor Red"; key Return
    if wait_row "redtext" 30; then
        y=$(grep '^row [0-9]*: redtext$' "$DUMP" | tail -1 | sed 's/^row \([0-9]*\):.*/\1/')
        fg=$(sed -n "s/^fg $y: //p" "$DUMP")
        case "$fg" in 9999999*) pass "Write-Host -ForegroundColor Red is red (VT through conhost: $fg)" ;; *) fail "redtext's colours are '$fg'" ;; esac
    else fail "no redtext"; fi
    grep -q '^tab 2 .*PowerShell$' "$DUMP" && pass "the PowerShell tab is named for it ($(sed -n 's/^tab 2 .*: //p' "$DUMP"))" || fail "tab 2's title: $(grep '^tab 2' "$DUMP")"
    sleep 0.5; shot pwsh
    set -- $(sed -n 's/^tab 1 at=\([0-9]*\),\([0-9]*\).*/\1 \2/p' "$DUMP")
    xdotool mousemove "$1" "$2" click 1; sleep 0.8
    [ "$(D tabs)" = "2 active 1" ] && has_row "hello from cmd" && pass "clicking the first tab shows Command Prompt again" || fail "tab 1: $(D tabs)"
else
    echo "SKIP  PowerShell 7 not found (SG_PWSH)"
fi

# --- selection, copy and paste -----------------------------------------------------------------
y=$(grep '^row [0-9]*: hello from cmd$' "$DUMP" | head -1 | sed 's/^row \([0-9]*\):.*/\1/')
set -- $(D origin); ox=$1; oy=$2
set -- $(D font); cw=$2; ch=$3
if [ -n "$y" ]; then
    py=$(( oy + y * ch + ch / 2 ))
    xdotool mousemove $(( ox + 2 )) $py mousedown 1; sleep 0.2
    xdotool mousemove $(( ox + 7 * cw )) $py; sleep 0.2
    xdotool mousemove $(( ox + 14 * cw - 2 )) $py; sleep 0.2; xdotool mouseup 1; sleep 0.6
    [ "$(D selection)" = "hello from cmd" ] && pass "dragging selects 'hello from cmd'" || fail "selection is '$(D selection)'"
    shot selection
    key ctrl+shift+c; sleep 0.5
    [ "$(xclip -o -selection clipboard 2>/dev/null)" = "hello from cmd" ] && pass "Ctrl+Shift+C copies it" || fail "clipboard: '$(xclip -o -selection clipboard 2>/dev/null)'"
else fail "no hello row to select"; fi
printf 'echo pasted-text' | xclip -i -selection clipboard; sleep 0.5
focus_view; key ctrl+v; sleep 0.5; key Return
wait_row "pasted-text" 15 && pass "Ctrl+V pastes into the shell" || fail "paste: $(grep '^row' "$DUMP" | tail -5)"

# --- resize: the pseudo console follows the window ---------------------------------------------
before=$(D size)
key F11; sleep 1.5
after=$(D size)
if [ "$before" != "$after" ]; then
    pass "full screen: $before -> $after cells"
    cols=${after% *}; rows=${after#* }
    typ "c:\\conprobe.exe size"; key Return
    wait_row "size=${cols}x${rows}" 15 && pass "the program sees ${cols}x${rows} (ResizePseudoConsole)" \
        || fail "size: $(grep 'size=' "$DUMP")"
    shot fullscreen
    key F11; sleep 1
else fail "F11 did not change the size ($before)"; fi

# --- exit closes a tab --------------------------------------------------------------------------
n=$(D tabs | cut -d' ' -f1)
focus_view; typ "exit"; key Return
i=0; while [ "$(D tabs | cut -d' ' -f1)" = "$n" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
[ "$(D tabs | cut -d' ' -f1)" = $(( n - 1 )) ] && pass "exit closes its tab" || fail "tabs after exit: $(D tabs)"
wineserver -k 2>/dev/null; sleep 1
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

# --- the command line --------------------------------------------------------------------------------
rm -f "$DUMP"
wine start wt.exe cmd /k echo started-by-wt ";" nt -p "Command Prompt" --title Second >/dev/null 2>&1
if wait_grep '^tabs 2 ' 30; then pass "wt CMD ; nt -p ... opens two tabs"; else fail "tabs: $(grep '^tab' "$DUMP")"; fi
grep -q '^tab 2 .*: Second$' "$DUMP" && pass "--title names the second tab" || fail "titles: $(grep '^tab ' "$DUMP")"
grep -q '^tab 1 .*profile=Command Prompt' "$DUMP" && pass "a cmd command line gets the Command Prompt profile" || fail "tab 1: $(grep '^tab 1' "$DUMP")"
set -- $(sed -n 's/^tab 1 at=\([0-9]*\),\([0-9]*\).*/\1 \2/p' "$DUMP")
[ $# -eq 2 ] && xdotool mousemove "$1" "$2" click 1 && sleep 1
wait_row "started-by-wt" 20 && pass "the first tab runs the command line given" || fail "no started-by-wt: $(grep '^row' "$DUMP" | head -5)"
shot cmdline
# wt.exe by name, as a program or script starts it (CreateProcess: system32's launcher, wine-sg 0133)
if [ -f "$T/pfx/drive_c/windows/system32/wt.exe" ]; then
    focus_view; typ "wt.exe --title fromcmd"; key Return
    wait_grep '^tab 1 .*: fromcmd$' 20 && pass "wt.exe typed in cmd (system32's launcher) opens Terminal" || fail "wt.exe from cmd: $(grep '^tab' "$DUMP")"
else fail "no wt.exe in system32 (a wine-sg without 0133)"; fi

# --- Alt+F4 closes the window (it was sent to the shell as F4's sequence) ------------------------------
nterm() { n_=$(pgrep -fc 'sg-terminal64[.]exe' 2>/dev/null); echo "${n_:-0}"; }
n=$(nterm)
if [ "$n" -ge 1 ]; then
    focus_view; key alt+F4
    i=0; while [ "$(nterm)" -ge "$n" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
    [ "$(nterm)" -lt "$n" ] && pass "Alt+F4 closes the Terminal window" || { shot altf4; fail "Alt+F4 left $(nterm) of $n Terminal windows open"; }
else fail "no Terminal running before Alt+F4"; fi

[ $RC = 0 ] && echo "terminal-check: PASS" || echo "terminal-check: FAIL"
exit $RC
