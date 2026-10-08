#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# High-resolution screens (David 2026-10-05, a Surface Pro 7 at 2736x1824:
# "everything is tiny"; "the taskbar will take up the same % of screen as on
# a 1080p screen does"). The display scale (LogPixels) is the recommended one
# for the screen until the user picks one -- the shorter side over 1080, to
# Windows' 25% steps, never below 100% -- and the shell, the title bars and
# Start take the share of the screen they take at 1080p and 100%. Under
# Xvfb, a shell desktop at 2736x1824:
#   1. the recommendation for each screen (sg-settings --set scale auto, the
#      screen stood in for by SG_FAKE_SCREEN): 1366x768 and 1920x1080 100%
#      (nothing written), 1920x1200 100%, 2560x1440 125%, 2560x1600 150%,
#      2736x1824 175%, 3840x2160 200%, portrait 1824x2736 175%, 1080x1920 100%
#   2. a pick sticks: 150% stays through sign-in (--set metrics) and a screen
#      change (--set desktop); picking the recommended one makes it automatic
#      again, and it follows the screen; a scale set before this existed is
#      the user's
#   3. at 2736x1824 automatic: 175%, and with the shell started at it (as at
#      sign-in) the taskbar is the share of the screen's height it is at
#      1080p (40/1080, 3.7%; within 8%: the scale goes in 25% steps), and so
#      are Start and a program's title bar; effects.conf hands the compositor the sizes in
#      the screen's pixels (its Linux title bars, the taskbar it keeps
#      maximized windows above, the pointer)
#   4. Settings > Display > Scale shows "175% (Recommended)"
#   5. (run first) at 1920x1080 nothing changes: no LogPixels written, a
#      40 px bar, title bars as before (Scale8 11); its sizes are the ones
#      3. compares with
#   6. a new scale while the shell runs (needs wine-sg 0890): 100% -> 175%,
#      the taskbar and Start 1.75 times their size, once; Settings, open
#      across the change, laid out again at it; the Linux side told
#      (sg-settingsctl display-scale 175)
#
#   SG_WINE_DIR=<wine-sg root> sh test/hidpi-check.sh
#   Mutants (sg-control/sg-settings): SG_MUTANT_SCALE_RECOMMEND_100,
#   SG_MUTANT_SCALE_NO_STICK, SG_MUTANT_TITLE_DOUBLE_SCALE, SG_MUTANT_TITLE_IGNORES_SCALE,
#   SG_MUTANT_CONF_UNSCALED,
#   SG_MUTANT_SCALE_LINUX_NOT_TOLD, SG_MUTANT_SETTINGS_DPI_IGNORED,
#   SG_MUTANT_DPI_UNAWARE (control main.c, wordpad, store, charmap); sg-start:
#   SG_MUTANT_START_SYSTEM_AWARE.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="$WINE_DIR/bin/wine"
export WINESERVER="$WINE_DIR/bin/wineserver"
EXE="${SG_SETTINGS_EXE:-$HERE/build/sg-settings64.exe}"
START="${SG_START_EXE:-$HERE/build/sg-start64.exe}"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in Xvfb xdotool "$MINGW"; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE" ] && [ -f "$EXE" ] && [ -f "$START" ] || { echo "SKIP: wine-sg, sg-settings or sg-start missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-hidpi.XXXXXX)
# shellcheck disable=SC2317
cleanup() { "$WINESERVER" -k 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; [ -n "${KEEP:-}" ] && echo "kept $T" || rm -rf "$T"; }
trap cleanup EXIT INT TERM
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" XDG_CONFIG_HOME="$T/config"
mkdir -p "$T/run" "$T/config"; export XDG_RUNTIME_DIR="$T/run" SG_LOCK_CONTROL=/nonexistent SG_START_DUMP="$T/startdump"
"$MINGW" -O2 -o "$T/probe.exe" "$HERE/test/hidpi-probe.c" -lgdi32 || { fail "the probe did not build"; exit 1; }
"$MINGW" -O2 -municode -mwindows -o "$T/poke.exe" "$HERE/test/sg-start-poke.c" || { fail "the poke did not build"; exit 1; }
cp "$EXE" "$T/sg-settings64.exe"
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w
for r in "$HERE"/theme/*.reg; do "$WINE" regedit /S "$("$WINE" winepath -w "$r" 2>/dev/null)" >/dev/null 2>&1; done
"$WINE" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
"$WINESERVER" -w

X=""
xserver() {   # WxH: a new X server of that size
    [ -n "$XP" ] && { "$WINESERVER" -k 2>/dev/null; kill "$XP" 2>/dev/null; wait "$XP" 2>/dev/null; sleep 1; }
    Xvfb -displayfd 3 -screen 0 "${1}x24" -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
    i=0; while [ ! -s "$T/display" ] && [ $i -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
    DISPLAY=":$(cat "$T/display")"; export DISPLAY; rm -f "$T/display"; X=$1
}
shell() {   # the shell at the X server's size, and Start, as at sign-in
    "$WINESERVER" -k 2>/dev/null; sleep 1
    "$WINE" explorer "/desktop=shell,$X" >/dev/null 2>&1 &
    i=0; while [ $i -lt 60 ] && ! xdotool search --name 'shell - Wine Desktop' >/dev/null 2>&1; do sleep 0.5; i=$((i + 1)); done
    sleep 3
    "$WINE" "$START" >/dev/null 2>&1 &
    sleep 4
}
sgs() { "$WINE" "$T/sg-settings64.exe" --set "$@" 2>/dev/null | tr -d '\r'; }
regv() {   # KEY VALUE: the DWORD in decimal, or "none"
    v=$("$WINE" reg query "$1" /v "$2" 2>/dev/null | tr -d '\r' | awk -v n="$2" '$1 == n { print $3 }')
    [ -n "$v" ] && printf '%d\n' "$v" || echo none
}
lp() { regv 'HKCU\Control Panel\Desktop' LogPixels; }
chosen() { regv 'HKCU\Software\Stained Glass\Display' ScaleChosen; }
fresh() {   # an account that never had a scale
    "$WINE" reg delete 'HKCU\Control Panel\Desktop' /v LogPixels /f >/dev/null 2>&1
    "$WINE" reg delete 'HKCU\Software\Stained Glass\Display' /f >/dev/null 2>&1
}
probe() { "$WINE" "$T/probe.exe" 2>/dev/null | tr -d '\r'; }
val() { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p"; }
conf() { sed -n "s/^$1=//p" "$T/config/stained-glass/effects.conf" 2>/dev/null; }
# PART/WHOLE within 8% of REF/REFWHOLE (the scale is in 25% steps: 1824
# lines at 175% are 1.69 times 1080 at 100%)
share() { awk -v p="$1" -v w="$2" -v r="$3" -v rw="$4" 'BEGIN { q = (p / w) / (r / rw); exit !(q > 0.92 && q < 1.08) }'; }
pct() { awk -v p="$1" -v w="$2" 'BEGIN { printf "%.2f%%", p / w * 100 }'; }

# --- 5 (first). 1080p: as it was, and the sizes the rest is measured against -------------------------------------------------------------------
xserver 1920x1080
fresh
"$WINE" reg delete 'HKCU\Software\Stained Glass\Style' /v Scale8 /f >/dev/null 2>&1
shell
sgs metrics >/dev/null
sleep 2
p=$(probe)
ref_bar=$(val "$p" barh); ref_cap=$(val "$p" caption)
"$WINE" "$T/poke.exe" >/dev/null 2>&1; sleep 3
rect=$(sed -n 's/^rect=//p' "$T/startdump" 2>/dev/null)
set -- $(echo "$rect" | tr ',' ' ')
ref_start=$(( ${4:-0} - ${2:-0} ))
"$WINE" "$T/poke.exe" >/dev/null 2>&1; sleep 1
[ "$(lp)" = none ] && [ "$(val "$p" barh)" = 40 ] && [ "$(regv 'HKCU\Software\Stained Glass\Style' Scale8)" = 11 ] && [ "$(conf taskbar)" = 40 ] \
    && pass "1920x1080: no LogPixels written, a 40 px taskbar, title bars as before (Scale8 11)" \
    || fail "1080p: LogPixels $(lp), $p, Scale8 $(regv 'HKCU\Software\Stained Glass\Style' Scale8), conf taskbar $(conf taskbar)"


xserver 2736x1824
shell

# --- 1. the recommendation ---------------------------------------------------------------
bad=""
for row in 1366x768:none 1920x1080:none 1920x1200:none 2560x1440:120 2560x1600:144 2736x1824:168 \
           3840x2160:192 1824x2736:168 1080x1920:none; do
    fresh
    SG_FAKE_SCREEN=${row%:*} sgs scale auto >/dev/null
    got=$(lp)
    [ "$got" = "${row#*:}" ] || bad="$bad ${row%:*}=$got(want ${row#*:})"
done
[ -z "$bad" ] && pass "the recommended scale: 768/1080/1200 lines 100% (nothing written), 1440 125%, 1600 150%, 1824 175%, 2160 200%, portrait by its shorter side" \
    || fail "recommendations:$bad"

# --- 2. a pick sticks --------------------------------------------------------------------
fresh
sgs metrics >/dev/null
[ "$(lp)" = 168 ] && pass "sign-in (--set metrics) at 2736x1824 sets 175% (LogPixels 168)" || fail "sign-in at 2736x1824: LogPixels $(lp)"
sgs scale 150 >/dev/null
a=$(lp); c=$(chosen)
sgs metrics >/dev/null
b=$(lp)
sgs desktop 2736x1824 >/dev/null
[ "$a" = 144 ] && [ "$c" = 1 ] && [ "$b" = 144 ] && [ "$(lp)" = 144 ] \
    && pass "150% picked by the user stays through sign-in and a screen change" || fail "a pick: set $a chosen $c, after sign-in $b, after the screen $(lp)"
sgs scale 175 >/dev/null
c=$(chosen)
SG_FAKE_SCREEN=1920x1080 sgs metrics >/dev/null
a=$(lp)
sgs metrics >/dev/null
[ "$c" = 0 ] && [ "$a" = 96 ] && [ "$(lp)" = 168 ] \
    && pass "picking the recommended 175% makes it automatic again: it follows the screen (1080p 100%, back to 175%)" \
    || fail "back to automatic: chosen $c, at 1080p $a, at 1824 $(lp)"
fresh
"$WINE" reg add 'HKCU\Control Panel\Desktop' /v LogPixels /t REG_DWORD /d 120 /f >/dev/null 2>&1
sgs metrics >/dev/null
[ "$(lp)" = 120 ] && [ "$(chosen)" = 1 ] && pass "125% set before the automatic scale existed is kept as the user's" \
    || fail "an earlier pick: LogPixels $(lp), chosen $(chosen)"

# --- 3. at 2736x1824 automatic, as a Surface Pro 7 signs in ---------------------------------
fresh
sgs metrics >/dev/null
shell
sgs metrics >/dev/null
sleep 2
p=$(probe)
barh=$(val "$p" barh); cap=$(val "$p" caption)
[ "$(val "$p" dpi)" = 168 ] && pass "programs started now get 168 DPI" || fail "the system DPI: $p"
share "${barh:-0}" 1824 "$ref_bar" 1080 && pass "the taskbar: ${barh} px of 1824, $(pct "${barh:-0}" 1824) of the screen's height (1080p: $ref_bar px, $(pct "$ref_bar" 1080))" \
    || fail "the taskbar at 2736x1824: $p ($(pct "${barh:-0}" 1824), want about $(pct "$ref_bar" 1080))"
"$WINE" "$T/poke.exe" >/dev/null 2>&1; sleep 3
rect=$(sed -n 's/^rect=//p' "$T/startdump" 2>/dev/null)
set -- $(echo "$rect" | tr ',' ' ')
if [ $# = 4 ] && [ "$ref_start" -gt 0 ] && share $(( $4 - $2 )) 1824 "$ref_start" 1080; then
    pass "Start: $(( $4 - $2 )) px tall, $(pct $(( $4 - $2 )) 1824) of the screen's height (1080p: $ref_start px, $(pct "$ref_start" 1080))"
else
    fail "Start at 2736x1824: rect '$rect' (want about $(pct "$ref_start" 1080) of 1824 tall)"
fi
"$WINE" "$T/poke.exe" >/dev/null 2>&1; sleep 1
s8=$(regv 'HKCU\Software\Stained Glass\Style' Scale8)
share "${cap:-0}" 1824 "$ref_cap" 1080 && [ "$s8" = 10 ] \
    && pass "a title bar: ${cap} px, $(pct "${cap:-0}" 1824) of the height (1080p: $ref_cap px, $(pct "$ref_cap" 1080); Scale8 $s8, of the 1042 lines the scale leaves)" \
    || fail "title bars at 175%: caption $cap ($(pct "${cap:-0}" 1824)), Scale8 $s8 (want 10: not scaled twice)"
[ "$(conf caption)" = "$cap" ] && [ "$(conf taskbar)" = "$barh" ] && [ "$(conf cursor)" = 42 ] \
    && pass "effects.conf in the screen's pixels: caption $(conf caption), taskbar $(conf taskbar), pointer $(conf cursor)" \
    || fail "effects.conf: caption $(conf caption) (want $cap), taskbar $(conf taskbar) (want $barh), cursor $(conf cursor) (want 42)"

# Control Panel, WordPad, the Store and Character Map draw themselves at the
# scale (their sizes go by the DPI): aware of the DPI, not Wine's scaled
# picture -- soft at 175%
for x in sg-control64.exe sg-wordpad64.exe sg-store64.exe sg-charmap64.exe; do
    "$WINE" "$HERE/build/$x" >/dev/null 2>&1 &
done
sleep 8
aw=$("$WINE" "$T/probe.exe" aware SgControlWindow WordPadClass SgStore SgCharMap 2>/dev/null | tr -d '\r')
[ "$aw" = "SgControlWindow=1 WordPadClass=1 SgStore=1 SgCharMap=1 " ] \
    && pass "Control Panel, WordPad, the Store and Character Map are aware of the DPI: drawn crisp at 175% ($aw)" \
    || fail "aware of the DPI at 175%: '$aw' (want all 1)"

# a scale picked over the recommended one makes the title bars larger with
# it, as on Windows: 225% on the Surface, 225/175 of their size at 175%
# (they stayed the share of the screen whatever the scale: smaller than at
# 175%; David 2026-10-08)
sgs scale 225 >/dev/null
p=$(probe); cap225=$(val "$p" caption); s8=$(regv 'HKCU\Software\Stained Glass\Style' Scale8)
awk -v a="${cap225:-0}" -v b="${cap:-1}" 'BEGIN { q = (a / b) / (225 / 175); exit !(q > 0.92 && q < 1.08) }' && [ "$s8" = 10 ] \
    && pass "225% picked: a title bar ${cap225} px, 225/175 of its ${cap} px at 175% (Scale8 $s8 still)" \
    || fail "title bars at 225%: caption $cap225 (175%: $cap; want about $(( ${cap:-0} * 225 / 175 ))), Scale8 $s8 (want 10)"
sgs scale 175 >/dev/null

# --- 4. Settings shows the recommendation ------------------------------------------------
export SG_SETTINGS_DUMP="$("$WINE" winepath -w "$T/sdump" 2>/dev/null | tr -d '\r')"
"$WINE" "$T/sg-settings64.exe" ms-settings:display >/dev/null 2>&1 &
i=0; while [ $i -lt 40 ] && ! grep -q '^page Display' "$T/sdump" 2>/dev/null; do sleep 0.5; i=$((i + 1)); done
sleep 1
tr -d '\r' < "$T/sdump" 2>/dev/null | grep -q 'ComboBox.*: 175% (Recommended)$' \
    && pass "Settings > Display > Scale: 175% (Recommended), selected" || fail "Settings' scale: $(grep ComboBox "$T/sdump" 2>/dev/null | head -2)"
unset SG_SETTINGS_DUMP

# --- 6. a new scale while the shell runs (wine-sg 0890) ----------------------------------
# 100% picked, the shell and Start started at it; then 175% picked while they
# run: the taskbar and Start take 1.75 times their size once (Start is per-
# monitor aware and sizes itself; aware of the system DPI only, Wine scaled
# it as well: 3 times); the Linux side is told (sg-settingsctl display-scale)
sgs scale 100 >/dev/null
shell
# Settings open at 100%, on its Display page
export SG_SETTINGS_DUMP="$("$WINE" winepath -w "$T/sdump6" 2>/dev/null | tr -d '\r')"
"$WINE" "$T/sg-settings64.exe" ms-settings:display >/dev/null 2>&1 &
i=0; while [ $i -lt 40 ] && ! grep -q '^page Display' "$T/sdump6" 2>/dev/null; do sleep 0.5; i=$((i + 1)); done
sleep 1
sr1=$(tr -d '\r' < "$T/sdump6" | sed -n 's/^rect //p')
unset SG_SETTINGS_DUMP
rm -f "$T/startdump"
"$WINE" "$T/poke.exe" >/dev/null 2>&1; sleep 3
p1=$(probe); s1=$(sed -n 's/^rect=//p' "$T/startdump" 2>/dev/null)
"$WINE" "$T/poke.exe" >/dev/null 2>&1; sleep 1
mkdir -p "$T/bin"
printf '#!/bin/sh\necho "$*" >> "%s/ctl"\nout=""; while [ $# -gt 0 ]; do [ "$1" = --out ] && out=$2; shift; done\n[ -n "$out" ] && printf "OK\\n" > "$out"\n' "$T" > "$T/bin/sg-settingsctl"
chmod +x "$T/bin/sg-settingsctl"
SG_SETTINGSCTL="$T/bin/sg-settingsctl" sgs scale 175 >/dev/null
sleep 3
rm -f "$T/startdump"
"$WINE" "$T/poke.exe" >/dev/null 2>&1; sleep 3
p2=$(probe); s2=$(sed -n 's/^rect=//p' "$T/startdump" 2>/dev/null)
"$WINE" "$T/poke.exe" >/dev/null 2>&1; sleep 1
ratio() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a > 0 && b / a > 1.70 && b / a < 1.80) }'; }
set -- $(echo "${s1:-0,0,0,0}" | tr ',' ' '); w1=$(( $3 - $1 )) h1=$(( $4 - $2 ))
set -- $(echo "${s2:-0,0,0,0}" | tr ',' ' '); w2=$(( $3 - $1 )) h2=$(( $4 - $2 ))
if ratio "$(val "$p1" barh)" "$(val "$p2" barh)" && ratio "$h1" "$h2" && ratio "$w1" "$w2" && [ "$(val "$p2" startpm)" = 1 ]; then
    pass "100% -> 175% while the shell runs: the taskbar $(val "$p1" barh) -> $(val "$p2" barh) px, Start ${w1}x$h1 -> ${w2}x$h2, per-monitor aware (1.75 times, once)"
else
    fail "100% -> 175% while the shell runs: taskbar $(val "$p1" barh) -> $(val "$p2" barh), Start ${w1}x$h1 -> ${w2}x$h2, per-monitor $(val "$p2" startpm) (want 1.75 times; aware of the system DPI only, Wine scales it again)"
fi
# Settings, open across the change: per-monitor aware, laid out again at 175%
# (its window 1.75 times as large in the screen's pixels -- not Wine's
# scaled picture of a 100% window, which it reports at its old size)
sleep 2
sr2=$(tr -d '\r' < "$T/sdump6" | sed -n 's/^rect //p')
set -- ${sr1:-0 0 0 0}; sw1=$(( $3 - $1 )) sh1=$(( $4 - $2 ))
set -- ${sr2:-0 0 0 0}; sw2=$(( $3 - $1 )) sh2=$(( $4 - $2 ))
ratio "$sw1" "$sw2" && ratio "$sh1" "$sh2" \
    && pass "Settings open across 100% -> 175%: laid out again, ${sw1}x$sh1 -> ${sw2}x$sh2 in the screen's pixels" \
    || fail "Settings open across 100% -> 175%: ${sw1}x$sh1 -> ${sw2}x$sh2 (want 1.75 times: per-monitor aware, laid out again)"
grep -q '^display-scale 175 --out ' "$T/ctl" 2>/dev/null && pass "the Linux side is told: sg-settingsctl display-scale 175" \
    || fail "the Linux side: sg-settingsctl got '$(cat "$T/ctl" 2>/dev/null)'"

[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
