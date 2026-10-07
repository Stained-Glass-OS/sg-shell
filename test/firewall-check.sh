#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Stained Glass Firewall's Settings pages and its question (sg-session's
# sg-firewall does the filtering; its status file and sg-admind's spool are
# this gate's own: SG_FIREWALL_STATUS, SG_ADMIN_SPOOL):
#   - Settings > Network & Internet > Firewall: each kind of network, on or
#     off (a warning when off), the networks in use and their profile
#   - Allowed apps: the built-in groups and the apps, Private/Public boxes;
#     ticking a box asks sg-admind for "firewall rule-set ..."
#   - the question: a program listened with no rule (sg-firewall's
#     ask/<uid>/<id>.ask), the taskbar's network icon (sg-netflyout) has
#     Settings ask it: "Stained Glass Firewall has blocked some features of
#     this app", the network in use ticked; Allow access files
#     "firewall rule-add allow ... prompt" through the elevated copy; Cancel
#     leaves <id>.cancel for the firewall (which blocks the program)
#   - Control Panel: control firewall.cpl opens the Settings page
# Read back from SG_SETTINGS_DUMP / SG_FIREWALL_PROMPT_DUMP; clicks with
# xdotool on the gate's own Xvfb.
#
#   sh test/firewall-check.sh [--mutant FW_NO_QUESTIONS|FW_PROMPT_TICKS_NOTHING]
# Needs wine-sg, Xvfb, xdotool and mingw (SG_WINE_DIR: another Wine); skips (77) without.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
MUTANT=""
[ "${1:-}" = --mutant ] && MUTANT=${2:-}
RC=0; DPY="${SG_FIREWALL_DPY:-131}"; XP=""; LOOP=""
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }
for t in Xvfb xdotool python3 x86_64-w64-mingw32-gcc; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] || { echo "SKIP: no wine-sg at $WINE_DIR"; exit 77; }

T=$(mktemp -d /var/tmp/sg-firewall-check.XXXXXX)
# shellcheck disable=SC2317
cleanup() {
    [ -n "$LOOP" ] && kill "$LOOP" 2>/dev/null
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

# the programs: this tree's, or a mutant's build
B="$T/build"; mkdir -p "$B"
CF="-O2 -municode -mwindows -Wall -Wno-missing-field-initializers"
MD=""
case "$MUTANT" in
    "") ;;
    FW_NO_QUESTIONS|FW_PROMPT_TICKS_NOTHING) MD="-DSG_MUTANT_$MUTANT" ;;
    *) echo "unknown mutant $MUTANT"; exit 2 ;;
esac
python3 "$HERE/src/control/gen-icon.py" "$B/sg-control.ico" && \
x86_64-w64-mingw32-windres -I "$HERE/src/control" -I "$B" "$HERE/src/control/control.rc" -O coff -o "$B/res.o" && \
python3 "$HERE/src/settings/gen-icon.py" "$B/sg-settings.ico" && \
x86_64-w64-mingw32-windres -I "$HERE/src/settings" -I "$B" "$HERE/src/settings/settings.rc" -O coff -o "$B/sres.o" && \
python3 "$HERE/src/sg-net-icon.py" "$B/sg-net.ico" && \
x86_64-w64-mingw32-windres -I "$HERE/src" -I "$B" "$HERE/src/sg-net.rc" -O coff -o "$B/nres.o" || { fail "resources did not build"; exit 1; }
LIBS="-lcomctl32 -lshell32 -lgdi32 -luser32 -ladvapi32 -lmsimg32 -liphlpapi -lws2_32 -lole32 -luuid -lwindowscodecs -lcomdlg32 -lshlwapi -lwininet -lversion -lwinspool"
# shellcheck disable=SC2086
x86_64-w64-mingw32-gcc $CF $MD -o "$B/sg-control64.exe" "$HERE"/src/control/*.c "$B/res.o" $LIBS && \
x86_64-w64-mingw32-gcc $CF $MD -o "$B/sg-settings64.exe" "$HERE"/src/control/*.c "$B/sres.o" $LIBS && \
x86_64-w64-mingw32-gcc $CF $MD -o "$B/sg-netflyout64.exe" "$HERE/src/sg-netflyout.c" "$B/nres.o" -lcomctl32 -luxtheme -lshell32 -lshlwapi -lgdi32 -luser32 -lole32 -luuid \
    || { fail "the programs did not build"; exit 1; }

Xvfb ":$DPY" -screen 0 1280x900x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=,winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1
dos() { wine winepath -w "$1" 2>/dev/null | tr -d '\r'; }
SYS="$WINEPREFIX/drive_c/windows/system32"
cp "$B/sg-control64.exe" "$B/sg-settings64.exe" "$B/sg-netflyout64.exe" "$SYS/"
# ms-settings: is Settings' (as defaults/65-sg-settings.reg registers it)
wine reg add 'HKCR\ms-settings\shell\open\command' /ve /d '"C:\windows\system32\sg-settings64.exe" "%1"' /f >/dev/null 2>&1
wine reg add 'HKCR\ms-settings' /v 'URL Protocol' /d '' /f >/dev/null 2>&1

# the firewall's status, as sg-firewall writes it
ST="$T/status"
printf 'state\trunning\nprofile\tdomain\ton\nprofile\tprivate\ton\nprofile\tpublic\toff\ncurrent\tprivate,public\n' > "$ST"
printf 'network\tu-home\teth0\tprivate\t802-3-ethernet\tHome\nnetwork\tu-cafe\twlan0\tpublic\t802-11-wireless\tCafe\n' >> "$ST"
printf 'group\tssh\tall\tRemote access (SSH)\ngroup\tfile-sharing\tdomain,private\tFile and Printer Sharing\n' >> "$ST"
printf 'rule\ts0n0s000\t1\tallow\tprivate\tany\t*\tC:\\Program Files\\Sonos\\Sonos.exe\tSonos\tprompt\n' >> "$ST"
printf 'rule\tp0rt0000\t1\tallow\tpublic\ttcp\t8080\t*\tWeb server\tuser\n' >> "$ST"
export SG_FIREWALL_STATUS="$ST"

# sg-admind, in test mode, with a stand-in for sg-firewall that records what it is asked
S="$T/spool"; mkdir -p "$S/requests" "$S/replies" "$T/bin"; chmod 700 "$S/requests"
cat > "$T/bin/sg-firewall" <<EOF
#!/bin/sh
printf '%s\n' "\$*" >> "$T/calls"
echo OK
EOF
chmod +x "$T/bin/sg-firewall"; : > "$T/calls"
export SG_ADMIN_TEST=1 SG_ADMIN_SPOOL="$S" SG_ADMIN_SYSTEM_UID="$(id -u)" SG_ADMIN_PATH="$T/bin"
( while :; do for f in "$S"/requests/*.req; do [ -e "$f" ] && python3 "$HERE/admin/sg-admind" 2>>"$T/admind.log"; break; done; sleep 0.2; done ) &
LOOP=$!

show() {   # show URI MARK: open Settings there, wait for MARK in its dump, print it
    rm -f "$T/dump.txt"
    SG_SETTINGS_DUMP=$(dos "$T/dump.txt") wine "$SYS/sg-settings64.exe" "$1" >/dev/null 2>&1 &
    i=0; while [ $i -lt 60 ] && ! grep -q "$2" "$T/dump.txt" 2>/dev/null; do sleep 1; i=$((i + 1)); done
    sleep 1; tr -d '\r' < "$T/dump.txt" 2>/dev/null
}
out=$(show ms-settings:network-firewall "Restore firewalls")
printf '%s\n' "$out" | grep -q "Firewall & network protection" && pass "ms-settings:network-firewall opens Firewall & network protection" \
    || { fail "the page did not open"; printf '%s\n' "$out" | head -20; }
printf '%s\n' "$out" | grep -q "Private network (active)" && printf '%s\n' "$out" | grep -q "Networks: Home" \
    && pass "the networks in use, by kind (Private: Home)" || fail "no Private (active): $(printf '%s\n' "$out" | grep -i network | head -5)"
printf '%s\n' "$out" | grep -q "The firewall is off for public networks" && pass "the firewall off for Public: a warning" || fail "no warning"
[ "$(printf '%s\n' "$out" | grep -c 'state=1 .*: Stained Glass Firewall')" = 1 ] && [ "$(printf '%s\n' "$out" | grep -c 'state=0 .*: Stained Glass Firewall')" = 1 ] \
    && pass "a switch for each: on for Private, off for Public" || fail "switches: $(printf '%s\n' "$out" | grep ': Stained Glass Firewall')"
printf '%s\n' "$out" | grep -q "Cafe" && printf '%s\n' "$out" | grep -q "Home" && pass "each network's profile can be chosen" || fail "no network profiles"
wineserver -k 2>/dev/null; sleep 1

out=$(show ms-settings:network-firewall-apps "risks of allowing")
printf '%s\n' "$out" | grep -q "Allowed apps and features" && pass "Allowed apps opens" || { fail "Allowed apps did not open"; printf '%s\n' "$out" | head; }
for n in "Sonos" "Remote access (SSH)" "File and Printer Sharing"; do
    printf '%s\n' "$out" | grep -qF "$n" && pass "listed: $n" || fail "not listed: $n"
done
printf '%s\n' "$out" | grep -qF "Web server" && fail "a port rule listed as an app" || pass "a port rule is not an app"
# the Sonos row's boxes (rows are by name: File and Printer Sharing, Remote access (SSH), Sonos)
sp=$(printf '%s\n' "$out" | grep -n ': Private$' | sed -n 3p); spub=$(printf '%s\n' "$out" | grep -n ': Public$' | sed -n 3p)
case "$sp$spub" in *"state=1"*"state=0"*) pass "Sonos: Private ticked, Public not" ;; *) fail "Sonos's boxes: $sp / $spub" ;; esac
at=$(printf '%s\n' "$spub" | sed -n 's/.* at=\([0-9]*\),\([0-9]*\): Public$/\1 \2/p')
if [ -n "$at" ]; then
    # shellcheck disable=SC2086  # x y
    xdotool mousemove $at click 1
    i=0; while [ $i -lt 30 ] && ! grep -q 'rule-set' "$T/calls"; do sleep 0.5; i=$((i + 1)); done
    grep -qx 'rule-set s0n0s000 1 allow domain,private,public' "$T/calls" && pass "ticking Public: sg-admind's firewall rule-set (Private and Public)" \
        || fail "ticking Public asked: $(cat "$T/calls")"
else fail "no position for Sonos's Public box"; fi
wineserver -k 2>/dev/null; sleep 1

# the question, through the taskbar's network icon
ASK="$T/run/ask/$(id -u)"; mkdir -p "$ASK"; chmod 700 "$ASK"
printf 'program=C:\\windows\\system32\\notepad.exe\nkind=windows\nprotocol=tcp\nport=3400\ncategory=private\n' > "$ASK/a1b2c3d4.ask"
: > "$T/calls"
SG_FIREWALL_RUN="$T/run" SG_FIREWALL_PROMPT_DUMP=$(dos "$T/prompt.txt") wine "$SYS/sg-netflyout64.exe" --bridged >/dev/null 2>&1 &
i=0; while [ $i -lt 40 ] && [ ! -s "$T/prompt.txt" ]; do sleep 0.5; i=$((i + 1)); done
P=$(tr -d '\r' < "$T/prompt.txt" 2>/dev/null)
printf '%s\n' "$P" | grep -q '^headline=Stained Glass Firewall has blocked some features of this app$' \
    && pass "a program listened with no rule: the network icon has Settings ask (Security Alert)" || fail "no question: $P"
printf '%s\n' "$P" | grep -q '^path=C:\\windows\\system32\\notepad.exe$' && printf '%s\n' "$P" | grep -q '^name=.' \
    && pass "it names the program (its own name, its path)" || fail "program: $P"
printf '%s\n' "$P" | grep -q '^private=1$' && printf '%s\n' "$P" | grep -q '^public=0$' \
    && pass "the network in use (Private) is ticked, Public is not" || fail "ticks: $P"
[ -e "$ASK/a1b2c3d4.open" ] && [ ! -e "$ASK/a1b2c3d4.ask" ] && pass "the question is taken (.open): asked once" || fail "not taken: $(ls "$ASK")"
at=$(printf '%s\n' "$P" | sed -n 's/^allow_at=\([0-9-]*\),\([0-9-]*\)$/\1 \2/p')
if [ -n "$at" ]; then
    # shellcheck disable=SC2086  # x y
    xdotool mousemove $at click 1
    i=0; while [ $i -lt 40 ] && ! grep -q 'rule-add' "$T/calls"; do sleep 0.5; i=$((i + 1)); done
    grep -q '^rule-add allow domain,private any \* C:\\windows\\system32\\notepad.exe .* prompt$' "$T/calls" \
        && pass "Allow access: the elevated copy asks sg-admind for firewall rule-add (Private, the program, prompt)" \
        || fail "Allow access asked: $(cat "$T/calls")"
fi
wineserver -k 2>/dev/null; sleep 1

# Cancel: the firewall is told (it blocks the program, as Windows does)
printf 'program=/usr/bin/fake-server\nkind=linux\nprotocol=tcp\nport=9000\ncategory=public\n' > "$ASK/e5f6a7b8.ask"
rm -f "$T/prompt.txt"
SG_FIREWALL_PROMPT_DUMP=$(dos "$T/prompt.txt") wine "$SYS/sg-control64.exe" /firewall-prompt "$(dos "$ASK/e5f6a7b8.ask")" >/dev/null 2>&1 &
i=0; while [ $i -lt 40 ] && [ ! -s "$T/prompt.txt" ]; do sleep 0.5; i=$((i + 1)); done
P=$(tr -d '\r' < "$T/prompt.txt" 2>/dev/null)
printf '%s\n' "$P" | grep -q '^name=fake-server$' && printf '%s\n' "$P" | grep -q '^public=1$' \
    && pass "a Linux program is asked about too (on a Public network: Public ticked)" || fail "Linux question: $P"
W=$(xdotool search --name '^Security Alert$' 2>/dev/null | head -1)
[ -n "$W" ] && { xdotool windowactivate --sync "$W" 2>/dev/null; xdotool key --window "$W" Escape 2>/dev/null; }
i=0; while [ $i -lt 20 ] && [ ! -e "$ASK/e5f6a7b8.cancel" ]; do sleep 0.5; i=$((i + 1)); done
[ "$(cat "$ASK/e5f6a7b8.cancel" 2>/dev/null)" = fake-server ] && pass "Cancel: <id>.cancel for the firewall, with the program's name" \
    || fail "no cancel: $(ls "$ASK")"
wineserver -k 2>/dev/null; sleep 1

# the Control Panel's way in
r=$(wine "$SYS/sg-control64.exe" --resolve firewall.cpl 2>/dev/null | tr -d '\r')
out=$(SG_SETTINGS_DUMP=$(dos "$T/dump2.txt") wine "$SYS/sg-control64.exe" firewall.cpl >/dev/null 2>&1 &
      i=0; while [ $i -lt 40 ] && ! grep -q "Restore firewalls" "$T/dump2.txt" 2>/dev/null; do sleep 1; i=$((i + 1)); done; tr -d '\r' < "$T/dump2.txt" 2>/dev/null)
printf '%s\n' "$out" | grep -q "Firewall & network protection" && pass "control firewall.cpl opens the firewall's Settings page" || fail "firewall.cpl: $r $(printf '%s' "$out" | head -3)"

[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
