#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# A Linux app started from Start comes to the front (src/linuxapps), as a Windows program started
# from Start does, even when another program's window was in front. Its window comes in the
# shell's frame, which the shell brings forward -- but the shell may not take the foreground from
# the window in front (wine-sg 9930's foreground lock): with File Explorer in front, GIMP started
# from Start opened behind it, its button flashing (linux1's QA VM, 2026-11-27). sg-linuxapp,
# started by the person's Start, hands its right to the foreground on to the shell
# (AllowSetForegroundWindow) before it starts the app.
# A stand-in for Start (a window the gate clicks, which runs sg-linuxapp64.exe --run, gives the
# foreground back to Notepad and hides), the shell's explorer with a stand-in sg-lockctl listing
# the app's window (an xterm that comes three seconds later, as a slow app's), Notepad in front:
#   - the app's frame is the foreground window.
#   mutant: -DSG_MUTANT_NO_SHELL_GRANT (src/linuxapps/sg-linuxapp.c)
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="${SG_WINE:-$WINE_DIR/bin/wine}"
export WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
EXE="${SG_LINUXAPP_EXE:-$HERE/build/sg-linuxapp64.exe}"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; T=$(mktemp -d /var/tmp/sg-linuxappfg.XXXXXX); XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
unset SESSION_MANAGER DISPLAY WAYLAND_DISPLAY
for t in Xvfb xdotool xterm xwininfo gio cc "$MINGW"; do command -v "$t" >/dev/null || { echo "SKIP: $t missing"; exit 77; }; done
[ -x "$WINE" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or sg-linuxapp not built"; exit 77; }
XTP=""
cleanup() { "$WINESERVER" -k 2>/dev/null; command -v sg_kill_prefix >/dev/null && sg_kill_prefix "$WINEPREFIX"; [ -n "$XTP" ] && kill "$XTP" 2>/dev/null; [ -n "$XP" ] && kill "$XP" 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
export WINEPREFIX="$T/pfx" WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
mkdir -p "$T/share/applications" "$T/run"
cat > "$T/share/applications/gate-slow.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Gate Slow
Exec=sh -c "sleep 3; exec xterm -geometry 50x12+300+150 -title GateLinux"
EOF
export XDG_DATA_DIRS="$T/share:/usr/share"
cat > "$T/fake-lockctl.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv)
{
    char path[4096], part[4200], buf[4096];
    const char *dir = getenv("SG_FAKE_DIR");
    FILE *f, *out;
    size_t n;
    if (!dir || argc != 4 || strcmp(argv[1], "XWINDOWS") || strcmp(argv[2], "--out")) return 0;
    snprintf(part, sizeof(part), "%s.part", argv[3]);
    if (!(out = fopen(part, "w"))) return 1;
    snprintf(path, sizeof(path), "%s/list", dir);
    if ((f = fopen(path, "r"))) { while ((n = fread(buf, 1, sizeof(buf), f))) fwrite(buf, 1, n, out); fclose(f); }
    fclose(out);
    return rename(part, argv[3]) != 0;
}
EOF
cc -O2 -o "$T/fake-lockctl" "$T/fake-lockctl.c" || { fail "stand-in did not build"; exit 1; }
"$MINGW" -O2 -municode -mwindows -o "$T/probe.exe" "$HERE/test/linuxapp-foreground-probe.c" || { fail "probe did not build"; exit 1; }
Xvfb -displayfd 3 -screen 0 1024x700x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/display")"
timeout -s KILL 300 "$WINE" wineboot -i >/dev/null 2>&1; "$WINESERVER" -w
"$WINE" reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
"$WINE" reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x700 /f >/dev/null 2>&1
"$WINESERVER" -w
export SG_LOCK_CONTROL=/nonexistent SG_LOCKCTL="$T/fake-lockctl" SG_FAKE_DIR="$T" XDG_RUNTIME_DIR="$T/run"
printf 'END\n' > "$T/list"
"$WINE" explorer /desktop=shell,1024x700 > "$T/explorer.out" 2>&1 &
i=0; while ! xwininfo -root -children 2>/dev/null | grep -q '"shell - Wine Desktop"' && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
sleep 3
"$WINE" notepad >/dev/null 2>&1 &
sleep 4
NP=$(xdotool search --name 'Notepad' 2>/dev/null | head -1)
[ -n "$NP" ] && { xdotool mousemove 500 300 click 1; sleep 1; }
fg() { "$WINE" "$T/probe.exe" fg 2>/dev/null | tr -d '\r'; }
case "$(fg)" in *Notepad*) pass "Notepad in front ($(fg))" ;; *) fail "Notepad not in front: $(fg)" ;; esac
DESK=$(echo "Z:$T/share/applications/gate-slow.desktop" | tr / '\\')
"$WINE" "$T/probe.exe" start "$(echo "Z:$EXE" | tr / '\\')" "$DESK" >/dev/null 2>&1 &
sleep 3
xdotool mousemove 150 500 click 1; sleep 1
i=0; while [ -z "$(xwininfo -root -tree | awk '/"GateLinux"/ {print $1; exit}')" ] && [ $i -lt 60 ]; do sleep 0.25; i=$((i + 1)); done
XID=$(xwininfo -root -tree | awk '/"GateLinux"/ {print $1; exit}')
[ -n "$XID" ] || { fail "the app's window never came"; echo "RESULT: FAIL"; exit 1; }
XTP=$(xdotool getwindowpid "$XID" 2>/dev/null)
TAB=$(printf '\t')
printf '%d shown - XTerm%sGateLinux\nEND\n' "$XID" "$TAB" > "$T/list"
sleep 5
case "$(fg)" in "fg SgLinuxWindow|GateLinux") pass "the Linux app started from Start comes to the front over Notepad" ;;
    *) fail "the Linux app's frame is not in front: $(fg)" ;; esac
echo
[ "$RC" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
