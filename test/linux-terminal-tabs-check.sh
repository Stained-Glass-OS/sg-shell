#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners
# Linux Terminal has tabs and copies as Windows Terminal does (David
# 2026-10-03: "All the terminal apps should support Tabs", Ctrl+Shift+C did
# not copy from the Linux terminal): the launcher itself, on an X server of
# its own (Xvfb), with a HOME of its own -- an X11 window (not Wayland),
# Ctrl+Shift+T opens a second shell, a selection goes to the clipboard with
# Ctrl+Shift+C, Ctrl+Shift+V pastes, and the console's look is seeded into a
# new user's preferences without touching existing ones.
# LXTERMINAL_DIR: a folder holding an lxterminal to use (an unpacked .deb).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
[ -n "${LXTERMINAL_DIR:-}" ] && PATH="$LXTERMINAL_DIR:$PATH"
for c in lxterminal xclip xdotool Xvfb; do command -v "$c" >/dev/null || { echo "SKIP: $c missing"; exit 77; }; done
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
L="$HERE/src/rootterm/sg-linux-terminal"
T=$(mktemp -d /var/tmp/sg-lxterm.XXXXXX)
for n in $(seq 172 199); do [ -e "/tmp/.X11-unix/X$n" ] || break; done
Xvfb ":$n" -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
TP=
trap 'kill $TP 2>/dev/null; kill $XP 2>/dev/null; rm -rf "$T" "$SG_GATE_HOME"' EXIT INT TERM
export DISPLAY=":$n"
i=0; while [ ! -e "/tmp/.X11-unix/X$n" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
# a user's own preferences stay theirs
mkdir -p "$HOME/.config/lxterminal"
printf '[general]\nfontname=Mine 9\n' > "$T/own.conf"
# the launcher seeds a new user's; as a session would start it, with Wayland about
( cd /; env WAYLAND_DISPLAY=wayland-sg-none SHELL=/bin/bash PS1='$ ' sh "$L" > "$T/term.log" 2>&1 ) & TP=$!
W=; i=0
while [ -z "$W" ] && [ $i -lt 60 ]; do sleep 0.25; W=$(xdotool search --name '^Linux Terminal$' 2>/dev/null | tail -1); i=$((i + 1)); done
[ -n "$W" ] && pass "an X11 window titled Linux Terminal" || { fail "no window: $(head -5 "$T/term.log")"; exit 1; }
C="$HOME/.config/lxterminal/lxterminal.conf"
grep -q '^fontname=Cascadia Mono 11$' "$C" && grep -q '^palette_color_12=rgb(59,120,255)$' "$C" && grep -q '^hidemenubar=true$' "$C" \
    && pass "a new user starts with the console's look (Cascadia Mono, Campbell colours)" || fail "preferences not seeded: $(head -3 "$C" 2>/dev/null)"
# this display's lxterminal (never one of the user's own)
lxpid() { for p in $(pgrep -x lxterminal -U "$(id -u)"); do tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "DISPLAY=:$n" && echo "$p"; done | tail -1; }
LP=$(lxpid)
tr '\0' '\n' < "/proc/$LP/environ" | grep -qx 'GDK_BACKEND=x11' && pass "GTK told to use X11 (a Wine frame, not full-screen Wayland)" || fail "GDK_BACKEND not x11"
[ "$(readlink "/proc/$LP/cwd")" = "$HOME" ] && pass "it starts in the home folder" || fail "cwd $(readlink "/proc/$LP/cwd")"
shells() { pgrep -P "$LP" | wc -l; }
xdotool windowactivate --sync "$W" 2>/dev/null || xdotool windowfocus "$W" 2>/dev/null
sleep 1
# fill the screen with one word, so a double-click anywhere selects it
xdotool type --delay 20 "clear; for i in \$(seq 40); do printf 'sgcopyword sgcopyword sgcopyword\n'; done"; xdotool key Return; sleep 1.2
eval "$(xdotool getwindowgeometry --shell "$W")"
printf 'stale' | xclip -selection clipboard
xdotool mousemove $((X + 30)) $((Y + HEIGHT / 2)) click --repeat 2 --delay 80 1; sleep 0.4
xdotool key ctrl+shift+c; sleep 0.5
got=$(xclip -o -selection clipboard 2>/dev/null)
[ "$got" = sgcopyword ] && pass "Ctrl+Shift+C copies the selection" || fail "clipboard after Ctrl+Shift+C: '$got'"
printf 'echo pasted > %s/pasted' "$T" | xclip -selection clipboard; sleep 0.3
xdotool key ctrl+shift+v; sleep 0.3; xdotool key Return; sleep 0.8
[ "$(cat "$T/pasted" 2>/dev/null)" = pasted ] && pass "Ctrl+Shift+V pastes" || fail "Ctrl+Shift+V did not paste"
n1=$(shells)
xdotool key ctrl+shift+t; i=0
while [ "$(shells)" -le "$n1" ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
[ "$n1" -ge 1 ] && [ "$(shells)" -gt "$n1" ] && pass "Ctrl+Shift+T opens a second tab ($n1 -> $(shells) shells)" || fail "no new tab ($n1 -> $(shells) shells)"
[ -n "$LP" ] && kill "$LP" 2>/dev/null; kill "$TP" 2>/dev/null; sleep 0.5; TP=
# an existing configuration is left alone
cp "$T/own.conf" "$C"
( cd /; env SHELL=/bin/bash sh "$L" > "$T/term2.log" 2>&1 ) & TP=$!
i=0; while ! xdotool search --name '^Linux Terminal$' >/dev/null 2>&1 && [ $i -lt 60 ]; do sleep 0.25; i=$((i + 1)); done
sleep 0.5
grep -q '^fontname=Mine 9$' "$C" && ! grep -q Cascadia "$C" && pass "a user's own preferences are kept" || fail "own preferences replaced: $(head -3 "$C")"
kill "$(lxpid)" 2>/dev/null
grep -q 'lxterminal' "$HERE/debian/control" && pass "lxterminal is a dependency" || fail "lxterminal not depended on"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
