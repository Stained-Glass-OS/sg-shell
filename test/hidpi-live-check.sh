#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Our programs follow a new display scale while they run, crisp (David
# 2026-10-06: "top of the line DPI scaling across the board"). Each is
# per-monitor v2 aware (src/sg-dpi.h) and lays itself out again on
# WM_DPICHANGED (wine-sg 0890): its fonts, controls and pictures at the new
# scale -- not Wine's scaled picture of the old one, soft at 175%. Under Xvfb
# at 2736x1824 (a Surface Pro 7's), in the shell, started at 100%, then
# Settings' change to 175% (LogPixels 168) and back:
#   1. at 100%: each program's window at 96 DPI (the size it has there)
#   2. at 175%: each window drawn by its program at 168 DPI (per-monitor
#      aware: Wine does not scale its picture), 1.75 times as large on the
#      screen, and a control inside it laid out again (1.75 times too)
#   3. back at 100%: 96 DPI, the sizes they had (within a few pixels)
# Programs: Control Panel, WordPad, the Store, Character Map, Alarms & Clock,
# the Terminal, the Font Viewer, the voice typing bar, the Defender and
# restart notices.
#
#   WINE=<wine-sg's wine> sh test/hidpi-live-check.sh   (or SG_WINE_DIR=<root>)
#   Mutants: SG_MUTANT_DPI_SYSTEM_ONLY (src/sg-dpi.h: aware of the system DPI
#   only, every program fails 2); SG_MUTANT_CPL_DPI_IGNORED (control),
#   SG_MUTANT_WORDPAD_DPI_IGNORED, SG_MUTANT_STORE_DPI_IGNORED,
#   SG_MUTANT_CHARMAP_DPI_IGNORED, SG_MUTANT_CLOCK_DPI_IGNORED,
#   SG_MUTANT_TERMINAL_DPI_IGNORED, SG_MUTANT_FONTVIEW_DPI_IGNORED,
#   SG_MUTANT_NOTICE_DPI_IGNORED, SG_MUTANT_DICTATE_DPI_IGNORED: that
#   program fails 2.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="${WINE:-$WINE_DIR/bin/wine}"
WINESERVER="${WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER=$(dirname "$WINE")/server/wineserver
export WINESERVER
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in Xvfb "$MINGW"; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE" ] || { echo "SKIP: no wine-sg at $WINE"; exit 77; }
for x in sg-control sg-wordpad sg-store sg-charmap sg-clock sg-terminal sg-fontview sg-dictate sg-defender-notice sg-restart-notice; do
    [ -f "$HERE/build/${x}64.exe" ] || { echo "SKIP: build/${x}64.exe missing (make build)"; exit 77; }
done

T=$(mktemp -d /var/tmp/sg-hidpi-live.XXXXXX)
# shellcheck disable=SC2317
cleanup() { "$WINESERVER" -k 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; [ -n "${KEEP:-}" ] && echo "kept $T" || rm -rf "$T"; }
trap cleanup EXIT INT TERM
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" XDG_CONFIG_HOME="$T/config"
mkdir -p "$T/run" "$T/config"; export XDG_RUNTIME_DIR="$T/run" SG_LOCK_CONTROL=/nonexistent
"$MINGW" -O2 -o "$T/probe.exe" "$HERE/test/hidpi-live-probe.c" || { fail "the probe did not build"; exit 1; }
Xvfb -displayfd 3 -screen 0 2736x1824x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
DISPLAY=":$(cat "$T/display")"; export DISPLAY
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w
for r in "$HERE"/theme/*.reg; do "$WINE" regedit /S "$("$WINE" winepath -w "$r" 2>/dev/null)" >/dev/null 2>&1; done
"$WINE" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Control Panel\Desktop' /v LogPixels /t REG_DWORD /d 96 /f >/dev/null 2>&1
"$WINESERVER" -w
"$WINE" explorer "/desktop=shell,2736x1824" >/dev/null 2>&1 &
sleep 8

# the notices' stand-ins: a quarantined file (SG Defender), a reboot-required
mkdir -p "$T/defender/notices/$(id -u)"
printf '{"name": "invoice.exe", "folder": "/home/x/Downloads", "signature": "Win.Test.EICAR_HDB-1"}' \
    > "$T/defender/notices/$(id -u)/0123-abcd.json"
echo '*** System restart required ***' > "$T/reboot-required"
SG_DEFENDER_DIR=$("$WINE" winepath -w "$T/defender" 2>/dev/null | tr -d '\r')
SG_RESTART_FILE=$("$WINE" winepath -w "$T/reboot-required" 2>/dev/null | tr -d '\r')
SG_NOTICE_TIMEOUT_MS=300000
export SG_DEFENDER_DIR SG_RESTART_FILE SG_NOTICE_TIMEOUT_MS
FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
[ -f "$FONT" ] || FONT=$(find /usr/share/fonts -name '*.ttf' 2>/dev/null | head -1)
# program : its window's class : a control in it (or -) : its arguments
APPS="sg-control:SgControlWindow:Edit:
sg-wordpad:WordPadClass:SgWordPadRibbon:
sg-store:SgStore:Edit:
sg-charmap:SgCharMap:SgCharGrid:
sg-clock:SgClockWindow:-:
sg-terminal:SgTerminalWindow:SgTerminalView:
sg-fontview:SgFontView:SgFontViewPane:Z:$FONT
sg-dictate:SgDictateBar:-:/toggle
sg-defender-notice:SgDefenderNotice:-:0123-abcd
sg-restart-notice:SgRestartNotice:-:"
echo "$APPS" | while IFS=: read -r exe cls child args; do
    # shellcheck disable=SC2086
    "$WINE" "$HERE/build/${exe}64.exe" $args >/dev/null 2>&1 &
    sleep 3
done
sleep 6
probe() { "$WINE" "$T/probe.exe" win "$1" ${2:+"$2"} 2>/dev/null | tr -d '\r'; }
measure() {   # FILE: each program's line
    echo "$APPS" | while IFS=: read -r exe cls child args; do
        [ "$child" = - ] && child=
        echo "$cls $(probe "$cls" "$child")"
    done > "$1"
}
field() { echo "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }
ratio() { awk -v a="$1" -v b="$2" -v lo="$3" -v hi="$4" 'BEGIN { exit !(a > 0 && b / a >= lo && b / a <= hi) }'; }

# measure FILE DPI: once every window is at DPI and two looks a second apart
# agree (each program hears of the change in turn; a busy host is slow), at
# most 40 s
settled() {
    i=0; : > "$1.prev"
    while [ $i -lt 40 ]; do
        sleep 1; measure "$1"
        if ! grep -v -q " dpi=$2 " "$1" && cmp -s "$1" "$1.prev"; then return 0; fi
        cp "$1" "$1.prev"; i=$((i + 1))
    done
}
measure "$T/m100"
"$WINE" "$T/probe.exe" set 168 >/dev/null 2>&1
sleep 2
settled "$T/m175" 168
"$WINE" "$T/probe.exe" set 96 >/dev/null 2>&1
sleep 2
settled "$T/m100b" 96

echo "$APPS" | while IFS=: read -r exe cls child args; do
    a=$(sed -n "s/^$cls //p" "$T/m100"); b=$(sed -n "s/^$cls //p" "$T/m175"); c=$(sed -n "s/^$cls //p" "$T/m100b")
    if [ "$(field "$a" dpi)" != 96 ]; then echo "FAIL  $exe at 100%: '$a' (want its window at 96 DPI)"; continue; fi
    w1=$(field "$a" size | cut -dx -f1); h1=$(field "$a" size | cut -dx -f2)
    w2=$(field "$b" size | cut -dx -f1); h2=$(field "$b" size | cut -dx -f2)
    ok=1
    [ "$(field "$b" dpi)" = 168 ] || ok=0
    ratio "$w1" "$w2" 1.65 1.85 && ratio "$h1" "$h2" 1.65 1.85 || ok=0
    if [ "$child" != - ]; then
        ch1=$(field "$a" child | cut -dx -f2); ch2=$(field "$b" child | cut -dx -f2)
        ratio "$ch1" "$ch2" 1.6 1.9 || ok=0
    fi
    if [ $ok = 1 ]; then echo "PASS  $exe at 175%: drawn by itself at 168 DPI, laid out again: $a -> $b"
    else echo "FAIL  $exe at 175%: $a -> $b (want dpi=168, the window and its $child 1.75 times)"; fi
    w3=$(field "$c" size | cut -dx -f1); h3=$(field "$c" size | cut -dx -f2)
    if [ "$(field "$c" dpi)" = 96 ] && [ $((w3 - w1)) -le 6 ] && [ $((w1 - w3)) -le 6 ] && [ $((h3 - h1)) -le 6 ] && [ $((h1 - h3)) -le 6 ]; then
        echo "PASS  $exe back at 100%: $c"
    else echo "FAIL  $exe back at 100%: $c (was $a)"; fi
done > "$T/results"
cat "$T/results"
grep -q '^FAIL' "$T/results" && RC=1
[ "$(grep -c '^PASS' "$T/results")" -ge 20 ] || RC=1
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
