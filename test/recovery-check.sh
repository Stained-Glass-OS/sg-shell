#!/bin/sh
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Settings > Recovery > Advanced startup: Restart now asks the boot menu to
# wait at the next start before restarting (it only restarted -- David).
# logind's SetRebootToBootLoaderMenu sets systemd-boot's one-shot menu
# timeout; an active session may call it (polkit's default). QA VM: the
# menu waits ("Boot in 40 s.") with the systems and the firmware entry.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
F="$HERE/src/control/set_misc.c"
grep -q '"SetRebootToBootLoaderMenu"' "$F" && grep -q 'usec\[\] = "60000000"' "$F" \
    && pass "Restart now sets the boot menu's one-shot timeout (a minute)" || fail "no boot menu request"
awk '/^BOOL set_cmd_recovery/,/^}/' "$F" | awk '/boot_menu_next_start\(\)/ {m=NR} /ExitWindowsEx/ {e=NR} END {exit !(m && e && m < e)}' \
    && pass "the menu is asked for before the restart" || fail "the restart comes before the boot menu request"
if command -v busctl >/dev/null && busctl introspect org.freedesktop.login1 /org/freedesktop/login1 2>/dev/null | grep -q SetRebootToBootLoaderMenu; then
    pass "logind here offers SetRebootToBootLoaderMenu"
else
    echo "info  logind not reachable here: the call itself is checked in the QA VM"
fi
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
