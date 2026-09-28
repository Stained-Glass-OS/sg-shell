#!/bin/sh
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Linux Terminal (Administrator): the Start menu lists it, its launcher has an
# icon, and the terminal asks for the user's password (sudo -i) before a root
# shell -- nothing opens as root on its own.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
S="$HERE/src/rootterm/sg-root-terminal"
command -v shellcheck >/dev/null && { shellcheck -s sh "$S" && pass "the script is clean sh" || fail "shellcheck"; }
grep -q "sudo -i" "$S" && pass "a root shell only through sudo (the user's password)" || fail "no sudo"
grep -q '^exec xterm ' "$S" && pass "in a terminal window (xterm)" || fail "no xterm"
grep -q 'add_beside(L"Linux Terminal (Administrator)", L"sg-rootterm64.exe")' "$HERE/src/sg-start.c" && pass "Start lists it" || fail "not in Start"
grep -q 'L"Linux Terminal (Administrator)", L"linux terminal root' "$HERE/src/sg-start.c" && pass "searching 'terminal', 'root' or 'linux' finds it" || fail "no keywords"
if [ -f "$HERE/build/sg-rootterm64.exe" ] && command -v wrestool >/dev/null; then
    wrestool -l -t 14 "$HERE/build/sg-rootterm64.exe" | grep -q group_icon && pass "its launcher has an icon" || fail "no icon"
fi
grep -q 'sg-rootterm64.exe src/rootterm/sg-root-terminal' "$HERE/debian/rules" && pass "packaged" || fail "not in debian/rules"
grep -q 'xterm' "$HERE/debian/control" && pass "xterm is a dependency" || fail "xterm not depended on"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
