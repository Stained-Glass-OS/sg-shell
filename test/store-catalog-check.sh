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
# a pinned Windows download (winget's hash no longer matches what the maker
# serves: CCleaner, GOM; or the build is gone: AIMP) is https, carries its
# SHA-256 -- the Store refuses one without -- and the version it installs
badpin=$(awk 'function chk() {if (pin && !(ok && ver)) {sub(/.*\\/, "", key); sub(/]$/, "", key); print key}}
    /^\[/ {chk(); key=$0; pin=0; ok=0; ver=0}
    /^"Source"="pin:/ {pin=1; ok = ($0 ~ /^"Source"="pin:https:\/\/[^|]+\|[0-9a-f]{64}\|/)}
    /^"PinVersion"="[0-9]/ {ver=1}
    END {chk()}' "$CAT")
[ -z "$badpin" ] && pass "every pinned download is https with a SHA-256 and a version" || fail "bad pin: $badpin"
grep -q '"Source"="linux:apt:secrets"' "$CAT" && pass "GNOME Secrets is listed (David 2026-10-03)" || fail "GNOME Secrets is not listed"
# SG Mail: ours, from our apt repository; Open runs the desktop entry the
# package installs, as the Store's Linux apps do (0.1.0-124: an sg-mail.exe
# nothing provides did nothing)
awk '/^\[/ {sec=$0} sec ~ /Apps\\128\]/' "$CAT" | grep -q '"Source"="ours:apt:sg-mail"' \
  && awk '/^\[/ {sec=$0} sec ~ /Apps\\128\]/' "$CAT" | grep -q '"Run"="/usr/share/applications/sg-mail.desktop"' \
  && pass "SG Mail is listed as ours (sg-mail), opened by its desktop entry" || fail "SG Mail is not listed as ours:apt:sg-mail"
# an installer that writes no Uninstall entry: the Store records it itself
# from RegisterDir + RegisterProgram (and its uninstaller), all three or none
badreg=$(awk 'function chk() {if (d + p + u && d + p + u < 3) {sub(/.*\\/, "", key); sub(/]$/, "", key); print key}}
    /^\[/ {chk(); key=$0; d=0; p=0; u=0}
    /^"RegisterDir"=/ {d=1} /^"RegisterProgram"=/ {p=1} /^"RegisterUninstall"=/ {u=1}
    END {chk()}' "$CAT")
[ -z "$badreg" ] && pass "every app the Store records itself has its folder, program and uninstaller" || fail "incomplete Register*: $badreg"
# Dropbox's offline installer (DropboxUpdate) writes no Uninstall entry under
# Wine: the Store said "done" and offered Install again, never Uninstall
# (qa4 walks 605-615); its uninstaller takes Dropbox's own machine arguments
db=$(awk '/^\[/ {sec=""} /"Name"="Dropbox"/ {sec=1} sec && /^"Register/' "$CAT")
echo "$db" | grep -q '"RegisterProgram"="Dropbox.exe"' && echo "$db" | grep -q '"RegisterUninstall"="DropboxUninstaller.exe /InstallType:MACHINE /S"' \
  && pass "Dropbox is recorded by the Store (Dropbox.exe, DropboxUninstaller.exe /InstallType:MACHINE /S)" || fail "Dropbox has no Store record: $db"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
