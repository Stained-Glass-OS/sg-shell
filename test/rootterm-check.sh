#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Linux Terminal: the Start menu lists it once; started, it is the user's own
# shell; run as administrator (--admin, which Start's "Run as administrator"
# passes), the terminal asks for the user's password (sudo -i) before a root
# shell -- nothing opens as root on its own. Its launcher has an icon.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
S="$HERE/src/rootterm/sg-root-terminal"
command -v shellcheck >/dev/null && { shellcheck -s sh "$S" && pass "the script is clean sh" || fail "shellcheck"; }
grep -q "sudo -i" "$S" && pass "a root shell only through sudo (the user's password)" || fail "no sudo"
grep -q '^exec xterm ' "$S" && pass "in a terminal window (xterm)" || fail "no xterm"
grep -q 'add_beside(L"Linux Terminal", L"sg-rootterm64.exe")' "$HERE/src/sg-start.c" && ! grep -q 'add_beside(L"Linux Terminal (Administrator)"' "$HERE/src/sg-start.c" \
    && pass "Start lists it, once" || fail "not in Start (or twice)"
grep -q 'L"Linux Terminal", L"linux terminal root' "$HERE/src/sg-start.c" && pass "searching 'terminal', 'root' or 'linux' finds it" || fail "no keywords"
grep -q 'L"runas") && wcsstr(e->path, L"sg-rootterm64.exe")' "$HERE/src/sg-start.c" && grep -q 'e->path, L"--admin"' "$HERE/src/sg-start.c" \
    && pass "Start's Run as administrator opens it with --admin" || fail "Run as administrator not wired"
U="$HERE/src/rootterm/sg-linux-terminal"
command -v shellcheck >/dev/null && { shellcheck -s sh "$U" && pass "the user terminal's script is clean sh" || fail "shellcheck user"; }
grep -q 'exec lxterminal ' "$U" && grep -q '^exec xterm ' "$U" && ! grep -q 'sudo' "$U" && pass "the user terminal is a plain shell, no sudo (LXTerminal, xterm without it)" || fail "user terminal"
# the launcher itself: plain, the user's; --admin, the root one (stand-in scripts)
W=${SG_WINE:-}
if [ -n "$W" ] && [ -x "$W" ] && [ -f "$HERE/build/sg-rootterm64.exe" ]; then
    T=$(mktemp -d /var/tmp/sg-rootterm.XXXXXX)
    for n in sg-linux-terminal sg-root-terminal; do printf '#!/bin/sh
echo %s > "%s/ran"
' "$n" "$T" > "$T/$n"; chmod 755 "$T/$n"; done
    run() { rm -f "$T/ran"; env -u DISPLAY -u WAYLAND_DISPLAY HOME="$T" WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="winemenubuilder.exe=d;mscoree,mshtml=" \
        SG_LINUX_TERMINAL_DIR="$T" timeout 120 "$W" "$HERE/build/sg-rootterm64.exe" "$@" >/dev/null 2>&1; i=0; while [ ! -s "$T/ran" ] && [ $i -lt 20 ]; do sleep 0.3; i=$((i + 1)); done; cat "$T/ran" 2>/dev/null; }
    [ "$(run)" = sg-linux-terminal ] && pass "started as it is: the user's terminal" || fail "plain start ran '$(cat "$T/ran" 2>/dev/null)'"
    [ "$(run --admin)" = sg-root-terminal ] && pass "with --admin: the root terminal" || fail "--admin ran '$(cat "$T/ran" 2>/dev/null)'"
    "$(dirname "$W")/wineserver" -k 2>/dev/null || true
    rm -rf "$T"
else
    echo "SKIP  the launcher's own test (set SG_WINE and build it)"
fi
if [ -f "$HERE/build/sg-rootterm64.exe" ] && command -v wrestool >/dev/null; then
    wrestool -l -t 14 "$HERE/build/sg-rootterm64.exe" | grep -q group_icon && pass "its launcher has an icon" || fail "no icon"
fi
grep -q 'sg-rootterm64.exe src/rootterm/sg-root-terminal src/rootterm/sg-linux-terminal' "$HERE/debian/rules" && pass "packaged" || fail "not in debian/rules"
grep -q 'xterm' "$HERE/debian/control" && pass "xterm is a dependency" || fail "xterm not depended on"
grep -qx 'lxterminal' "$HERE/src/linuxapps/linux-apps-hidden" && pass "Start lists Linux Terminal, not LXTerminal's own entry too" || fail "LXTerminal's own entry not hidden"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
