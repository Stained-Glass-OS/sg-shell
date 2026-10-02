#!/bin/sh
# Linux Terminal's copy and paste, as in the Windows console (David
# 2026-10-01: Ctrl+Shift+V did nothing, nothing could be copied out):
# selected text is on the clipboard, a right-click pastes the clipboard, and
# Ctrl+Shift+V pastes too. In xterm, on an X server of its own (Xvfb), with
# the launcher's own settings.
# shellcheck disable=SC2015  # pass/fail one-liners
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
for c in xterm xclip xdotool Xvfb; do command -v "$c" >/dev/null || { echo "SKIP: $c missing"; exit 77; }; done
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
T=$(mktemp -d /var/tmp/sg-xtermcp.XXXXXX)
for n in $(seq 160 199); do [ -e "/tmp/.X11-unix/X$n" ] || break; done
Xvfb ":$n" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
trap 'kill $XP $TP 2>/dev/null; rm -rf "$T"' EXIT INT TERM
TP=
export DISPLAY=":$n"
i=0; while [ ! -e "/tmp/.X11-unix/X$n" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
# the launcher's settings, read from it (not copied into this test)
TBL=$(sed -n "s/^SG_XTERM_COPY_PASTE='\(.*\)'$/\1/p" "$HERE/src/rootterm/sg-linux-terminal")
[ -n "$TBL" ] || { fail "no copy/paste settings in sg-linux-terminal"; exit 1; }
grep -q -- '-xrm "$SG_XTERM_COPY_PASTE"' "$HERE/src/rootterm/sg-linux-terminal" && grep -q -- '-xrm "$SG_XTERM_COPY_PASTE"' "$HERE/src/rootterm/sg-root-terminal" \
    && pass "both terminals use them" || fail "a terminal does not use them"
# a terminal at the top-left that shows a word, then records two lines typed
cat > "$T/inner.sh" <<IN
printf 'sgcopyword\n'
read -r a; printf '%s\n' "\$a" > "$T/line1"
read -r b; printf '%s\n' "\$b" > "$T/line2"
sleep 30
IN
xterm -geometry 80x10+0+0 -fa 'DejaVu Sans Mono' -fs 11 -xrm 'XTerm*selectToClipboard: true' -xrm "$TBL" \
    -e sh "$T/inner.sh" 2> "$T/xterm.err" & TP=$!
sleep 2
W=$(xdotool search --pid $TP 2>/dev/null | tail -1)
[ -n "$W" ] || { fail "no xterm window: $(cat "$T/xterm.err")"; exit 1; }
grep -qi "translation" "$T/xterm.err" && fail "xterm rejects the settings: $(cat "$T/xterm.err")"
# select the first line by dragging across it (row 1)
eval "$(xdotool getwindowgeometry --shell "$W")"
xdotool mousemove $((X + 4)) $((Y + 9)) mousedown 1 mousemove $((X + 120)) $((Y + 9)) sleep 0.2 mouseup 1
sleep 0.5
got=$(xclip -o -selection clipboard 2>/dev/null)
case "$got" in sgcopyword*) pass "selecting copies to the clipboard ($got)";; *) fail "clipboard after selecting: '$got'";; esac
# keys through XTEST into the focused terminal (xterm ignores sent events)
xdotool windowfocus --sync "$W" 2>/dev/null || xdotool windowfocus "$W" 2>/dev/null
# right-click pastes
printf 'rightclickpaste' | xclip -selection clipboard; sleep 0.3
xdotool mousemove $((X + 200)) $((Y + 60)) click 3; sleep 0.3; xdotool key Return; sleep 0.8
[ "$(cat "$T/line1" 2>/dev/null)" = rightclickpaste ] && pass "a right-click pastes" || fail "right-click: '$(cat "$T/line1" 2>/dev/null)'"
# Ctrl+Shift+V pastes
printf 'ctrlshiftv' | xclip -selection clipboard; sleep 0.3
xdotool key ctrl+shift+v; sleep 0.3; xdotool key Return; sleep 0.8
[ "$(cat "$T/line2" 2>/dev/null)" = ctrlshiftv ] && pass "Ctrl+Shift+V pastes" || fail "Ctrl+Shift+V: '$(cat "$T/line2" 2>/dev/null)'"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
