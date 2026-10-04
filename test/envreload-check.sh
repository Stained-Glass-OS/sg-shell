#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Start and Terminal read environment variables again when told they changed
# (src/sg-envreload.h), as the taskbar does (wine-sg 0749): David 2026-10-04,
# Claude Code asked for PATH to be changed, and a PowerShell started again
# from Start or as a new Terminal tab still had the old one until a restart.
# After the Environment Variables dialog's broadcast (WM_SETTINGCHANGE
# "Environment") a variable added to the registry is in both programs'
# environments, one removed is gone, and the session's own (not the
# registry's) stays. Read from each running program's process parameters.
#
# Needs wine-sg, Xvfb and mingw; skips (77) without. SG_START_EXE /
# SG_TERMINAL_EXE run other builds (mutant: -DSG_MUTANT_NO_ENV_RELOAD).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="${SG_WINE:-$WINE_DIR/bin/wine}"
export WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
START="${SG_START_EXE:-$HERE/build/sg-start64.exe}"
TERM_EXE="${SG_TERMINAL_EXE:-$HERE/build/sg-terminal64.exe}"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for t in Xvfb "$MINGW"; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE" ] && [ -f "$START" ] && [ -f "$TERM_EXE" ] || { echo "SKIP: wine-sg, sg-start or sg-terminal missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-envreload.XXXXXX)
# shellcheck disable=SC2317
cleanup() { "$WINESERVER" -k 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; rm -rf "$T" "$SG_GATE_HOME"; }
trap cleanup EXIT INT TERM
"$MINGW" -O2 -municode -o "$T/probe.exe" "$HERE/test/envreload-probe.c" || { fail "probe did not build"; exit 1; }
Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/display")" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
"$WINE" wineboot --init >/dev/null 2>&1; "$WINESERVER" -w
cp "$START" "$WINEPREFIX/drive_c/sg-start64.exe"; cp "$TERM_EXE" "$WINEPREFIX/drive_c/sg-terminal64.exe"
# the session's own variable: in the programs' environment, not the registry's
export SG_GATE_SESSION=kept
"$WINE" 'C:\sg-start64.exe' >/dev/null 2>&1 &
"$WINE" 'C:\sg-terminal64.exe' cmd /k >/dev/null 2>&1 &
get() { "$WINE" "$T/probe.exe" get "$1" "$2" 2>/dev/null | tr -d '\r'; }
i=0; while [ $i -lt 60 ] && { [ "$(get sg-start64.exe SG_GATE_SESSION)" != kept ] || [ "$(get sg-terminal64.exe SG_GATE_SESSION)" != kept ]; }; do sleep 0.5; i=$((i + 1)); done
for e in sg-start64.exe sg-terminal64.exe; do
    [ "$(get $e SG_GATE_SESSION)" = kept ] || fail "$e not running or not readable ($(get $e SG_GATE_SESSION))"
done

# the dialog: a user variable added, then the broadcast
"$WINE" reg add 'HKCU\Environment' /v SG_GATE_NEW /d fresh /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Environment' /v Path /d 'C:\sg-gate-bin' /f >/dev/null 2>&1
"$WINE" "$T/probe.exe" broadcast >/dev/null 2>&1; sleep 1
for e in sg-start64.exe sg-terminal64.exe; do
    v=$(get $e SG_GATE_NEW)
    [ "$v" = fresh ] && pass "$e: a variable added and broadcast is in its environment" || fail "$e: SG_GATE_NEW is '$v'"
    case "$(get $e PATH)" in *'C:\sg-gate-bin'*) pass "$e: the user's new Path is in its PATH";;
        *) fail "$e: PATH lacks the user's Path ($(get $e PATH | cut -c1-80))";; esac
done
# removed, broadcast: gone; the session's own stays
"$WINE" reg delete 'HKCU\Environment' /v SG_GATE_NEW /f >/dev/null 2>&1
"$WINE" "$T/probe.exe" broadcast >/dev/null 2>&1; sleep 1
for e in sg-start64.exe sg-terminal64.exe; do
    v=$(get $e SG_GATE_NEW)
    [ "$v" = "(unset)" ] && pass "$e: a variable removed and broadcast is gone" || fail "$e: SG_GATE_NEW still '$v'"
    v=$(get $e SG_GATE_SESSION)
    [ "$v" = kept ] && pass "$e: the session's own variable stays" || fail "$e: SG_GATE_SESSION is '$v'"
done
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
