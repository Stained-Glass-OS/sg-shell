#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# The notification centre (sg-notify): the taskbar's notification icon and
# its panel. wine-sg (0815) keeps every toast and balloon in
# HKCU\Software\Stained Glass\Notifications\History; here three are planted,
# and under Xvfb, with the shell's taskbar:
#   - --dump lists them newest first, all new;
#   - /toggle (Win+A) opens the panel at the right of the screen, the work
#     area's height, listing them newest first, and marks them seen;
#   - a notification's x dismisses it (gone from the history too);
#   - Clear all dismisses the rest;
#   - /toggle again closes it;
#   - clicking one hands it to its program through wine-sg 0819's
#     SgActivateNotification (here a program gone, so it is started), the
#     panel closes and it leaves the history;
#   - a runtime a program needs (wine-sg 0817 names it under
#     Runtimes\Missing): it asks, once a session, and Yes opens SG Store on
#     its page (sg-store --page); a DLL the store does not offer: no question.
#
#   sh test/notify-check.sh        SG_WINE_DIR=<wine-sg>   (mutants SG_MUTANT_NOTIFY_NO_CLEAR, SG_MUTANT_NOTIFY_NO_ACTIVATE; SG_MUTANT_NO_RUNTIME_ASK, built here)
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="${SG_WINE:-$WINE_DIR/bin/wine}"
WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER=$(dirname "$WINE")/server/wineserver
EXE="${SG_NOTIFY_EXE:-$HERE/build/sg-notify64.exe}"
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in Xvfb xdotool; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
T=$(mktemp -d /var/tmp/sg-notify-check.XXXXXX); XP=
cleanup() { WINEPREFIX="$T/pfx" "$WINESERVER" -k 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
export WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' SG_NOTIFY_DUMP="$T/dump"
Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/display")"
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w
cp "$EXE" "$T/sg-notify64.exe"
"$WINE" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
add() {
    k="HKCU\\Software\\Stained Glass\\Notifications\\History\\$1"
    "$WINE" reg add "$k" /v App /d "$2" /f >/dev/null 2>&1
    "$WINE" reg add "$k" /v Title /d "$3" /f >/dev/null 2>&1
    "$WINE" reg add "$k" /v Body /d "$4" /f >/dev/null 2>&1
}
add 00000000 Firefox "New message from Alex" "Are we still on for lunch tomorrow?"
add 00000001 Backup "Backup finished" "Your files were copied."
add 00000002 Thunderbird "3 new emails" ""
"$WINE" reg add 'HKCU\Software\Stained Glass\Notifications\History' /v Next /t REG_DWORD /d 3 /f >/dev/null 2>&1
"$WINESERVER" -w
mkdir -p "$T/run"; export XDG_RUNTIME_DIR="$T/run" SG_LOCK_CONTROL=/nonexistent

out=$("$WINE" "$T/sg-notify64.exe" --dump 2>&1 | tr -d '\r' | grep -E '^(NOTIFICATIONS|ENTRY)')
echo "$out" | sed 's/^/      /'
[ "$(echo "$out" | head -1)" = "NOTIFICATIONS 3 unread 3" ] && [ "$(echo "$out" | sed -n 2p)" = "ENTRY 2|Thunderbird|3 new emails|" ] \
    && pass "--dump: the three, newest first, all new" || fail "--dump: $out"

"$WINE" explorer /desktop=shell,1280x800 >/dev/null 2>&1 &
sleep 8
"$WINE" "$T/sg-notify64.exe" >/dev/null 2>&1 &
sleep 3
d() { cat "$T/dump" 2>/dev/null; }
val() { d | sed -n "s/^$1=//p"; }
"$WINE" "$T/sg-notify64.exe" /toggle >/dev/null 2>&1; sleep 2
set -- $(val rect | tr ',' ' ')
[ "$(val visible)" = 1 ] && [ $# = 4 ] && [ "$3" = 1280 ] && [ "$2" = 0 ] && [ $(( $4 - $2 )) -ge 700 ] \
    && pass "/toggle (Win+A) opens the panel at the right, the work area's height: $(val rect)" || fail "panel: visible $(val visible) rect $(val rect)"
px=$1 py=$2
[ "$(d | grep '^entry' | awk '{print $2}' | tr '\n' ' ')" = "2 1 0 " ] && pass "it lists them newest first" \
    || fail "order: $(d | grep '^entry' | awk '{print $2}' | tr '\n' ' ')"
[ "$(val unread)" = 0 ] && pass "opening it marks them seen" || fail "unread after opening: $(val unread)"

# the middle one's x
set -- $(d | sed -n 's/^entry 1 [^ ]* close=\([0-9,]*\) .*/\1/p' | tr ',' ' ')
if [ $# = 4 ]; then
    cx=$(( px + ($1 + $3) / 2 )) cy=$(( py + ($2 + $4) / 2 ))
    xdotool mousemove $(( cx - 30 )) "$cy"; sleep 0.4; xdotool mousemove "$cx" "$cy"; sleep 0.4; xdotool click 1; sleep 1.2
fi
hist=$("$WINE" reg query 'HKCU\Software\Stained Glass\Notifications\History' 2>/dev/null | tr -d '\r')
[ "$(val count)" = 2 ] && ! printf '%s\n' "$hist" | grep -q '\\00000001$' \
    && pass "a notification's x dismisses it, from the history too" || fail "dismiss: count $(val count); $(printf '%s\n' "$hist" | grep -c '\\0000')"

set -- $(val clear | tr ',' ' ')
[ $# = 4 ] && { xdotool mousemove $(( px + ($1 + $3) / 2 )) $(( py + ($2 + $4) / 2 )); sleep 0.4; xdotool click 1; sleep 1.2; }
hist=$("$WINE" reg query 'HKCU\Software\Stained Glass\Notifications\History' 2>/dev/null | tr -d '\r')
[ "$(val count)" = 0 ] && ! printf '%s\n' "$hist" | grep -q '\\0000' \
    && pass "Clear all dismisses the rest" || fail "clear all: count $(val count); $(printf '%s\n' "$hist" | grep -c '\\0000') left"

"$WINE" "$T/sg-notify64.exe" /toggle >/dev/null 2>&1; sleep 1.5
[ "$(val visible)" = 0 ] && pass "/toggle again closes it" || fail "still open: $(val visible)"

# a click on one hands it to its program (wine-sg 0819 SgActivateNotification):
# here a program that is gone, with no activator, so it is started
if grep -aq SgActivateNotification "$WINEPREFIX/drive_c/windows/system32/windows.ui.dll" 2>/dev/null; then
    printf '@echo clicked> C:\\clicked.txt\r\n' > "$WINEPREFIX/drive_c/mark.bat"
    add 00000010 Marker "Click me" "It starts its program"
    "$WINE" reg add 'HKCU\Software\Stained Glass\Notifications\History\00000010' /v Exe /d 'C:\mark.bat' /f >/dev/null 2>&1
    "$WINE" "$T/sg-notify64.exe" /toggle >/dev/null 2>&1; sleep 2
    set -- $(val rect | tr ',' ' '); px=${1:-0} py=${2:-0}
    set -- $(d | sed -n 's/^entry 10 \([0-9,]*\) .*/\1/p' | tr ',' ' ')
    [ $# = 4 ] && { xdotool mousemove $(( px + $1 + 30 )) $(( py + ($2 + $4) / 2 )); sleep 0.4; xdotool click 1; }
    i=0; while [ ! -s "$WINEPREFIX/drive_c/clicked.txt" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
    sleep 1
    hist=$("$WINE" reg query 'HKCU\Software\Stained Glass\Notifications\History' 2>/dev/null | tr -d '\r')
    [ -s "$WINEPREFIX/drive_c/clicked.txt" ] && [ "$(val activated)" = "10 hr=00000000" ] && [ "$(val visible)" = 0 ] \
        && ! printf '%s\n' "$hist" | grep -q '\\00000010$' \
        && pass "clicking one hands it to its program (here: started), closes the panel, and it goes" \
        || fail "click: started $([ -s "$WINEPREFIX/drive_c/clicked.txt" ] && echo 1 || echo 0) activated '$(val activated)' visible $(val visible)"
else
    echo "SKIP  clicking one: this wine-sg has no SgActivateNotification (0819)"
fi
# --- a runtime a program needs (David 2026-10-05: Meedio's plug-ins and the Visual Basic 6
# runtime). wine-sg's loader (0817) names a missing DLL the store offers in
# HKCU\Software\Stained Glass\Runtimes\Missing; sg-notify asks whether to
# install it, and Yes opens SG Store on its page (a stand-in sg-store.exe,
# through App Paths, records how it was started). Asked once a session; a
# DLL the store does not offer is not asked about.
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
if command -v "$MINGW" >/dev/null; then
    cat > "$T/store-standin.c" <<'EOF2'
#include <windows.h>
#include <stdio.h>
int main(void)
{
    FILE *f = fopen("C:\\store-args.txt", "a");
    char *c = GetCommandLineA(), *p = c;
    if (*p == '"') { p = strchr(p + 1, '"'); p = p ? p + 1 : c; } else while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    if (f) { fprintf(f, "%s\n", p); fclose(f); }
    return 0;
}
EOF2
    "$MINGW" -O2 -o "$WINEPREFIX/drive_c/sg-store.exe" "$T/store-standin.c" 2>"$T/cc.log" || fail "the stand-in store does not build"
    { echo '#define SG_MUTANT_NO_RUNTIME_ASK'; cat "$HERE/src/sg-notify.c"; } > "$T/mut-notify.c"
    "$MINGW" -O2 -municode -mwindows -Wno-missing-field-initializers -I"$HERE/src" -o "$T/mut-notify.exe" "$T/mut-notify.c" \
        -lshell32 -luser32 -lgdi32 -ladvapi32 2>>"$T/cc.log" || fail "mutant NO_RUNTIME_ASK does not build: $(tail -3 "$T/cc.log")"
    "$WINE" reg add 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\sg-store.exe' /ve /d 'C:\sg-store.exe' /f >/dev/null 2>&1
    "$WINE" reg add 'HKLM\Software\Stained Glass\Store\Runtimes' /v msvbvm60.dll /d 127 /f >/dev/null 2>&1
    "$WINE" reg add 'HKLM\Software\Stained Glass\Store\Apps\127' /v Name /d 'Visual Basic 6 runtime' /f >/dev/null 2>&1
    missing() { "$WINE" reg add 'HKCU\Software\Stained Glass\Runtimes\Missing' /v "$1" /d "$2" /f >/dev/null 2>&1; }
    asked() { i=0; while [ $i -lt "${1:-40}" ]; do w=$(xdotool search --name '^A runtime is missing$' 2>/dev/null | head -1); [ -n "$w" ] && { echo "$w"; return 0; }; sleep 0.25; i=$((i + 1)); done; return 1; }
    rm -f "$WINEPREFIX/drive_c/store-args.txt"
    missing msvbvm60.dll General_RemovableInsert.dll
    if w=$(asked); then
        pass "a plug-in missing msvbvm60.dll (the store offers it): sg-notify asks whether to install it"
        import -window root "$HERE/build/notify-runtime.png" 2>/dev/null
        xdotool windowactivate --sync "$w" 2>/dev/null; xdotool key Return; sleep 3
        [ "$(tr -d '\r' < "$WINEPREFIX/drive_c/store-args.txt" 2>/dev/null)" = "--page 127" ] \
            && pass "...Yes opens SG Store on its page (sg-store.exe --page 127)" || fail "after Yes: '$(cat "$WINEPREFIX/drive_c/store-args.txt" 2>/dev/null)'"
        "$WINE" reg query 'HKCU\Software\Stained Glass\Runtimes\Missing' 2>/dev/null | grep -qi msvbvm60 \
            && fail "the Missing value was kept" || pass "...and the note is taken (the Missing value goes)"
    else fail "no question for a missing msvbvm60.dll"; fi
    rm -f "$WINEPREFIX/drive_c/store-args.txt"
    missing msvbvm60.dll General_HideTaskbar.dll
    asked >/dev/null 8 && fail "asked again in the same session" || pass "asked once a session: the second plug-in is not asked about again"
    missing sgnosuch.dll Other.dll
    asked >/dev/null 8 && fail "asked about a DLL the store does not offer" || pass "a DLL the store does not offer is not asked about"
    # the mutant, its own session of sg-notify
    "$WINE" taskkill /f /im sg-notify64.exe >/dev/null 2>&1; sleep 1
    "$WINE" "$T/mut-notify.exe" >/dev/null 2>&1 &
    sleep 3
    missing msvbvm60.dll General_RemovableInsert.dll
    if asked >/dev/null 12 || [ -s "$WINEPREFIX/drive_c/store-args.txt" ]; then fail "NO_RUNTIME_ASK not detected"
    else pass "MUTANT NO_RUNTIME_ASK never asks (gate catches it)"; fi
    "$WINE" taskkill /f /im mut-notify.exe >/dev/null 2>&1
else
    echo "NOTE  no $MINGW: the runtime question is not checked"
fi
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
