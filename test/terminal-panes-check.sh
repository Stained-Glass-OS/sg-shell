#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for Terminal's split panes and search (sg-terminal, wt.exe), on a shell
# desktop under Xvfb, driven on the X keyboard and read back from the dump
# (SG_TERMINAL_DUMP: every pane's place, size and shown rows) and screenshots:
#
#   - Alt+Shift+Plus splits the pane to the right: a second Command Prompt
#     of its own; what is typed reaches only the focused pane and its output
#     appears only there
#   - each pane's program sees its own pane's size -- the first pane's pseudo
#     console was resized by the split (ResizePseudoConsole)
#   - Alt+Left / Alt+Up / Alt+Right move the focus; Alt+Shift+Minus splits
#     downwards; Alt+Shift+Right moves the divider and the program sees the
#     new size; Ctrl+Shift+W closes a pane and `exit` another, the rest
#     filling their space; Alt+Shift+D duplicates the pane
#   - Ctrl+Shift+F: a search typed into the box finds a match in the
#     scrollback, scrolls to it and draws it highlighted; Enter and
#     Shift+Enter step between matches; Alt+C matches case; Escape closes
#     and the keys go to the shell again
#   - wt.exe's command line: sp -V, sp -H, mf left
#
# Needs a wine-sg with 0131-0133 (SG_WINE_DIR). Screenshots:
# build/terminal-panes-*.png. SG_TERMINAL_EXE runs another build (mutants:
# -DSG_MUTANT_PANEINPUT, -DSG_MUTANT_NOSEARCH, -DSG_MUTANT_NORESIZE).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_TERMINAL_EXE:-$HERE/build/sg-terminal64.exe}"
OUT="$HERE/build"
RC=0; DPY="${SG_TERMINAL_PANES_DPY:-141}"; XP=""
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }

for need in Xvfb xdotool import convert; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-terminal-panes.XXXXXX); chmod 755 "$T"
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
x86_64-w64-mingw32-gcc -O2 -o "$T/pfx/drive_c/conprobe.exe" "$HERE/test/sg-terminal-probe.c" || fail "the probe did not build"
wineserver -w

start_explorer() {
    WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
    i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
}
start_explorer

DUMP="$T/terminal.dump"
export SG_TERMINAL_DUMP="$(wine winepath -w "$DUMP" | tr -d '\r')"
D() { sed -n "s/^$1 //p" "$DUMP" 2>/dev/null | head -1; }
wait_grep() { i=0; while ! grep -q "$1" "$DUMP" 2>/dev/null && [ $i -lt $(( ${2:-20} * 4 )) ]; do sleep 0.25; i=$((i + 1)); done; grep -q "$1" "$DUMP" 2>/dev/null; }
has_prow() { grep -q "^prow $1 [0-9]*: $2\$" "$DUMP" 2>/dev/null; }          # pane id, a whole row
prow_has() { grep -q "^prow $1 [0-9]*: .*$2" "$DUMP" 2>/dev/null; }          # pane id, part of a row
wait_prow() { i=0; while ! has_prow "$1" "$2" && [ $i -lt $(( ${3:-20} * 4 )) ]; do sleep 0.25; i=$((i + 1)); done; has_prow "$1" "$2"; }
pane_line() { grep "^pane [0-9]* id=$1 " "$DUMP" 2>/dev/null | head -1; }
pane_size() { r=$(pane_line "$1" | sed 's/.* size=\([0-9]*x[0-9]*\) .*/\1/'); echo "${r:-0x0}"; }
pane_rect() { r=$(pane_line "$1" | sed 's/.* rect=\([-0-9,]*\) .*/\1/' | tr ',' ' '); echo "${r:-0 0 0 0}"; }
pane_at() { pane_line "$1" | sed 's/.* at=\([0-9]*\),\([0-9]*\) .*/\1 \2/'; }
focus() { D focus; }
typ() { xdotool type --delay 60 "$1"; sleep 0.3; }
key() { xdotool key --delay 80 "$@"; sleep 0.5; }
shot() { import -window root "$OUT/terminal-panes-$1.png" 2>/dev/null; }
click_pane() { set -- $(pane_at "$1"); [ $# -eq 2 ] || { fail "no pane to click"; return 1; }; xdotool mousemove "$1" "$2" click 1; sleep 0.4; }
prompt() { wait_grep "^prow $1 [0-9]*: C:\\\\users\\\\.*>" "${2:-30}"; }
probe_size() {   # run the size probe in pane $1 and require it to report the pane's dump size
    typ "c:\\conprobe.exe size"; key Return
    sz=$(pane_size "$1")
    wait_prow "$1" "size=$sz" 15
}

wine start wt.exe -p "Command Prompt" >/dev/null 2>&1
if wait_grep '^panes 1$' 30; then pass "wt.exe opens one pane"; else fail "no pane: $(head -20 "$DUMP" 2>/dev/null)"; exit 1; fi
P1=$(focus)
prompt "$P1" && pass "the first pane's prompt" || fail "no prompt: $(grep '^prow' "$DUMP" | head -3)"
click_pane "$P1"
typ "echo alpha-one"; key Return
wait_prow "$P1" "alpha-one" 15 && pass "the first pane runs what is typed" || fail "no alpha-one"
S1=$(pane_size "$P1")

# --- split to the right ------------------------------------------------------------------------
key alt+shift+equal
if wait_grep '^panes 2$' 10 && [ "$(focus)" != "$P1" ]; then P2=$(focus); pass "Alt+Shift+Plus splits: a second pane, focused (ids $P1, $P2)"
else fail "no split: $(grep '^pane' "$DUMP")"; P2=0; fi
prompt "$P2" && pass "the new pane has a Command Prompt of its own" || fail "no prompt in the new pane: $(grep "^prow $P2" "$DUMP" | head -3)"
set -- $(pane_rect "$P1"); l1=$1; r1=$3; set -- $(pane_rect "$P2"); l2=$1
[ "$r1" -le "$l2" ] && [ "$l1" -lt "$l2" ] && pass "side by side: $(pane_rect "$P1") | $(pane_rect "$P2")" || fail "rects: $(pane_rect "$P1") / $(pane_rect "$P2")"
S1b=$(pane_size "$P1")
[ "${S1b%x*}" -lt "${S1%x*}" ] && pass "the first pane narrowed: $S1 -> $S1b" || fail "first pane $S1 -> $S1b"
typ "echo bravo-two"; key Return
if wait_prow "$P2" "bravo-two" 15 && ! prow_has "$P1" "bravo-two"; then pass "keys go to the focused pane only; its output is only there"
else fail "bravo-two: pane $P2 $(grep -c "^prow $P2 .*bravo" "$DUMP"), pane $P1 $(grep -c "^prow $P1 .*bravo" "$DUMP")"; fi
probe_size "$P2" && pass "the new pane's program sees its size ($(pane_size "$P2"))" || fail "pane 2 size: $(grep "^prow $P2 .*size=" "$DUMP") vs $(pane_size "$P2")"
shot split

key alt+Left
[ "$(focus)" = "$P1" ] && pass "Alt+Left moves the focus to the left pane" || fail "focus after Alt+Left: $(focus)"
probe_size "$P1" && pass "the first pane's program sees its narrowed size $(pane_size "$P1") (ResizePseudoConsole)" \
    || fail "pane 1 size: $(grep "^prow $P1 .*size=" "$DUMP") vs $(pane_size "$P1")"
prow_has "$P2" "size=$(pane_size "$P1")\$" && [ "$(pane_size "$P1")" != "$(pane_size "$P2")" ] && fail "pane 1's probe output reached pane 2"

# --- split downwards, move the divider -------------------------------------------------------
key alt+shift+minus
if wait_grep '^panes 3$' 10; then P3=$(focus); pass "Alt+Shift+Minus splits the left pane downwards (id $P3)"
else fail "no second split: $(grep '^pane' "$DUMP")"; P3=0; fi
prompt "$P3" || fail "no prompt in the lower pane"
set -- $(pane_rect "$P1"); t1l=$1; t1b=$4; set -- $(pane_rect "$P3"); t3l=$1; t3t=$2
[ "$t1l" = "$t3l" ] && [ "$t1b" -le "$t3t" ] && pass "one above the other: $(pane_rect "$P1") / $(pane_rect "$P3")" || fail "rects $(pane_rect "$P1") / $(pane_rect "$P3")"
c3=$(pane_size "$P3"); key alt+shift+Right; c3b=$(pane_size "$P3")
[ "${c3b%x*}" -gt "${c3%x*}" ] && pass "Alt+Shift+Right moves the divider: $c3 -> $c3b" || fail "resize: $c3 -> $c3b"
probe_size "$P3" && pass "the lower pane's program sees $(pane_size "$P3")" || fail "pane 3 size: $(grep "^prow $P3 .*size=" "$DUMP")"
key alt+Up
[ "$(focus)" = "$P1" ] && pass "Alt+Up to the pane above" || fail "focus after Alt+Up: $(focus)"
key alt+Right
[ "$(focus)" = "$P2" ] && pass "Alt+Right to the right pane" || fail "focus after Alt+Right: $(focus)"
sleep 0.5; shot three

# --- closing panes ---------------------------------------------------------------------------------
key ctrl+shift+w
if wait_grep '^panes 2$' 10 && [ -z "$(pane_line "$P2")" ]; then pass "Ctrl+Shift+W closes the focused pane"; else fail "after Ctrl+Shift+W: $(grep '^pane' "$DUMP")"; fi
set -- $(D rect); wr=$3; set -- $(pane_rect "$P1"); [ $(( wr - $3 )) -lt 40 ] && pass "the rest fill its space ($(pane_rect "$P1"))" || fail "pane 1 after closing: $(pane_rect "$P1") in $(D rect)"
click_pane "$P3"
[ "$(focus)" = "$P3" ] && pass "clicking a pane focuses it" || fail "focus after click: $(focus)"
typ "exit"; key Return
if wait_grep '^panes 1$' 10 && grep -q '^tabs 1 ' "$DUMP"; then pass "exit closes only its pane"; else fail "after exit: $(grep '^pane\|^tabs' "$DUMP")"; fi
key alt+shift+d
if wait_grep '^panes 2$' 10 && pane_line "$(focus)" | grep -q 'profile=Command Prompt'; then P4=$(focus); pass "Alt+Shift+D duplicates the pane"
else fail "duplicate: $(grep '^pane' "$DUMP")"; P4=0; fi
prompt "$P4" || fail "no prompt in the duplicate"
key ctrl+shift+w; wait_grep '^panes 1$' 10 || fail "the duplicate did not close"

# --- search ------------------------------------------------------------------------------------------
click_pane "$P1"
typ "echo needle-early"; key Return
typ "echo %processor_architecture%"; key Return
typ "for /l %i in (1,1,120) do @echo filler-%i"; key Return
wait_prow "$P1" "filler-120" 30 || fail "no filler output"
key ctrl+shift+f
wait_grep '^search open=1 ' 5 && pass "Ctrl+Shift+F opens the search box" || fail "search: $(grep '^search' "$DUMP")"
typ "needle-early"
if wait_grep '^search open=1 case=0 matches=2 ' 10; then pass "both matches are found, in the scrollback ($(D search))"
else fail "search: $(grep '^search' "$DUMP")"; fi
sc=$(D scroll | cut -d' ' -f1)
vr=$(D search | sed 's/.* viewrow=\([-0-9]*\) .*/\1/')
if [ "${sc:-0}" -gt 0 ] && [ "${vr:--1}" -ge 0 ] && grep -q "^row $vr: .*needle-early" "$DUMP"; then pass "it scrolled back $sc lines to the match (row $vr)"
else fail "scroll=$sc viewrow=$vr: $(grep "^row $vr:" "$DUMP")"; fi
cur1=$(D search | sed 's/.* current=\([0-9]*\) line=\([0-9]*\) .*/\1 \2/')
sleep 0.5; shot search
if [ "${vr:--1}" -ge 0 ]; then
    set -- $(D origin); ox=$1; oy=$2; set -- $(D font); cw=$2; ch=$3
    col=$(D search | sed 's/.* col=\([-0-9]*\) .*/\1/')
    px=$(convert "$OUT/terminal-panes-search.png" -crop "1x1+$(( ox + col * cw + 1 ))+$(( oy + vr * ch + 1 ))" -depth 8 txt:- | tail -1 | grep -o '#[0-9A-F]\{6\}')
    [ "$px" = "#E7C564" ] && pass "the current match is drawn highlighted ($px)" || fail "the match's pixel is $px"
fi
xdotool key Return; sleep 0.6
cur2=$(D search | sed 's/.* current=\([0-9]*\) line=\([0-9]*\) .*/\1 \2/')
[ -n "$cur1" ] && [ "$cur1" != "$cur2" ] && pass "Enter goes to the other match ($cur1 -> $cur2)" || fail "Enter: $cur1 -> $cur2"
xdotool key shift+Return; sleep 0.6
[ "$(D search | sed 's/.* current=\([0-9]*\) line=\([0-9]*\) .*/\1 \2/')" = "$cur1" ] && pass "Shift+Enter comes back" || fail "Shift+Enter: $(D search)"
xdotool key ctrl+a; typ "amd64"
wait_grep '^search open=1 case=0 matches=1 ' 5 && pass "without match case 'amd64' finds cmd's AMD64" || fail "amd64: $(D search)"
key alt+c
wait_grep '^search open=1 case=1 matches=0 ' 5 && pass "Alt+C: match case finds none" || fail "match case: $(D search)"
key alt+c
key Escape
if wait_grep '^search open=0 ' 5; then pass "Escape closes the search"; else fail "after Escape: $(D search)"; fi
typ "echo after-search"; key Return
wait_prow "$P1" "after-search" 15 && pass "keys reach the shell again" || fail "no after-search"

# --- the command line ------------------------------------------------------------------------------
wineserver -k 2>/dev/null; sleep 1; rm -f "$DUMP"
start_explorer
wine start wt.exe -p "Command Prompt" ";" sp -V -p "Command Prompt" ";" sp -H -p "Command Prompt" ";" mf left >/dev/null 2>&1
if wait_grep '^panes 3$' 30; then pass "wt ... ; sp -V ; sp -H opens three panes"; else fail "command line: $(grep '^pane' "$DUMP")"; fi
sleep 1
a=$(sed -n 's/^pane 1 id=\([0-9]*\) .*/\1/p' "$DUMP"); b=$(sed -n 's/^pane 2 id=\([0-9]*\) .*/\1/p' "$DUMP"); c=$(sed -n 's/^pane 3 id=\([0-9]*\) .*/\1/p' "$DUMP")
set -- $(pane_rect "$a"); ar=$3; set -- $(pane_rect "$b"); bl=$1; bb=$4; set -- $(pane_rect "$c"); cl=$1; ct=$2
[ "$ar" -le "$bl" ] && [ "$bl" = "$cl" ] && [ "$bb" -le "$ct" ] && pass "-V put one to the right, -H one below it" || fail "rects: $(grep '^pane ' "$DUMP")"
grep -q '^tabs 1 active 1$' "$DUMP" && pass "all in one tab" || fail "tabs: $(grep '^tab' "$DUMP")"
[ "$(focus)" = "$a" ] && pass "mf left focused the left pane" || fail "focus: $(focus) (left is $a)"
for id in $a $b $c; do prompt "$id" 30 || fail "no prompt in pane $id"; done
shot cmdline

[ $RC = 0 ] && echo "terminal-panes-check: PASS" || echo "terminal-panes-check: FAIL"
exit $RC
