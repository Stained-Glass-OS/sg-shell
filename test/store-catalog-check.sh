#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# The SG Store's real catalogue (defaults/85-sg-store.reg): every Linux app's
# package is one apt knows (Debian's, or a maker's repository the image adds:
# Chrome, Edge), each Linux app has its Run program, and the Apps\Ln keys are
# not used twice. Skips (77) without apt-cache.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
CAT="${SG_STORE_CATALOG:-$HERE/defaults/85-sg-store.reg}"
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
command -v apt-cache >/dev/null || { echo "SKIP: apt-cache missing"; exit 77; }
VENDOR="google-chrome-stable microsoft-edge-stable"   # from their makers' repositories
missing=""
for p in $(grep -o '"Source"="linux:apt:[^"]*"' "$CAT" | sed 's/.*linux:apt://; s/"$//' | sort -u); do
    case " $VENDOR " in *" $p "*) continue ;; esac
    apt-cache show "$p" >/dev/null 2>&1 || missing="$missing $p"
done
[ -z "$missing" ] && pass "every Linux app's package is in apt" || fail "not in apt:$missing"
norun=$(awk '/^\[/ {if (lx && !run) print key; key=$0; lx=0; run=0} /"Tier"="linux"/ {lx=1} /^"Run"=/ {run=1} END {if (lx && !run) print key}' "$CAT")
[ -z "$norun" ] && pass "every Linux app says what Open runs" || fail "no Run: $norun"
dup=$(grep -o '^\[HKEY_LOCAL_MACHINE\\Software\\Stained Glass\\Store\\Apps\\[^]]*\]' "$CAT" | sort | uniq -d)
[ -z "$dup" ] && pass "no app key is used twice" || fail "used twice: $dup"
grep -q '"Source"="linux:apt:secrets"' "$CAT" && pass "GNOME Secrets is listed (David 2026-10-03)" || fail "GNOME Secrets is not listed"
# SG Mail: ours, from our apt repository; Open starts it by its program name
# (sg-linuxapp gives Linux apps App Paths names: sg-mail.exe)
awk '/^\[/ {sec=$0} sec ~ /Apps\\128\]/' "$CAT" | grep -q '"Source"="ours:apt:sg-mail"' \
  && awk '/^\[/ {sec=$0} sec ~ /Apps\\128\]/' "$CAT" | grep -q '"Run"="sg-mail.exe"' \
  && pass "SG Mail is listed as ours (sg-mail), opened as sg-mail.exe" || fail "SG Mail is not listed as ours:apt:sg-mail"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
