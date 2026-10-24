#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for Media Player's transport controls (sg-media, SystemMediaTransportControls):
# on a session bus of its own, wine-sg (10.0-563 and later) publishes them as an
# MPRIS player, and the desktop's media controls and the session's media keys
# (sg-volume -> sg-settingsctl media) drive it while another window is in front.
#
#   - a queue of two WAVs and an MP4 plays: the player is on the bus, Playing,
#     with the file's name as its title, Next there and Previous there
#   - PlayPause pauses and resumes it, Next moves to the second file (its
#     title), Stop stops it; the video's title is published too
#   - in front (WM_APPCOMMAND to it): Pause pauses and does not resume, Play
#     resumes and does not pause, Play/Pause toggles; it takes the keys
#
# Needs wine-sg with the MPRIS bridge, Xvfb, dbus-daemon, busctl, python3,
# ffmpeg and mingw; skips (77) without them. EXE= tests another build:
# mutant SG_MUTANT_MEDIA_NO_SMTC fails it.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${EXE:-$HERE/build/sg-media64.exe}"
RC=0; DPY=${DPY:-216}; XP=""; BP=""
# printf, not echo: dash's echo eats the backslashes of Windows paths
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }

for need in Xvfb dbus-daemon busctl python3 ffmpeg; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
find "$WINE_DIR/lib/wine" -name 'windows.media.mediacontrol.so' 2>/dev/null | grep -q . ||
    { echo "SKIP: this wine-sg has no MPRIS bridge (10.0-563 and later)"; exit 77; }

T=$(mktemp -d /var/tmp/sg-media-smtc.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    [ -n "$BP" ] && kill "$BP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

python3 - "$T/one.wav" "$T/two.wav" <<'EOF'
import math, struct, sys, wave
for path in sys.argv[1:]:
    w = wave.open(path, "wb"); w.setnchannels(1); w.setsampwidth(2); w.setframerate(22050)
    w.writeframes(b"".join(struct.pack("<h", int(4000 * math.sin(2 * math.pi * 440 * i / 22050))) for i in range(22050 * 30)))
    w.close()
EOF
ffmpeg -loglevel error -y -f lavfi -i color=c=0x20A060:size=160x120:rate=25:duration=20 -f lavfi -i sine=duration=20 \
    -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$T/clip.mp4" || { echo "SKIP: ffmpeg cannot make an H.264 clip"; exit 77; }

# --- a session bus, a display, Wine --------------------------------------------------------
dbus-daemon --session --nofork --print-address=1 > "$T/bus" & BP=$!
i=0; while [ ! -s "$T/bus" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
DBUS_SESSION_BUS_ADDRESS=$(head -1 "$T/bus"); export DBUS_SESSION_BUS_ADDRESS
[ -n "$DBUS_SESSION_BUS_ADDRESS" ] || { echo "FAIL  no session bus"; exit 1; }
[ -e "/tmp/.X11-unix/X$DPY" ] && { echo "SKIP: display :$DPY is taken (DPY= to choose another)"; exit 77; }
Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
i=0; while [ ! -e "/tmp/.X11-unix/X$DPY" ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
kill -0 "$XP" 2>/dev/null || { echo "FAIL  Xvfb :$DPY did not start"; exit 1; }
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES="$WINEDLLOVERRIDES;mscoree,mshtml=" WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH" SG_MEDIA_DUMP="$T/dump.txt"
wine wineboot --init >/dev/null 2>&1; wineserver -w
wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
wine reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768 /f >/dev/null 2>&1
wineserver -w
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

D() { sed -n "s/^$1 //p" "$SG_MEDIA_DUMP" 2>/dev/null | head -1; }
wait_for() {   # key value seconds
    i=0
    while [ "$(D "$1")" != "$2" ] && [ $i -lt $(( $3 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    [ "$(D "$1")" = "$2" ]
}
player() { busctl --user list --no-legend 2>/dev/null | awk '$1 ~ /^org\.mpris\.MediaPlayer2\./ {print $1; exit}'; }
prop() { busctl --user get-property "$NAME" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player "$1" 2>/dev/null; }
title() {
    busctl --user --json=short get-property "$NAME" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player Metadata 2>/dev/null |
        python3 -c 'import json,sys; print(json.load(sys.stdin)["data"].get("xesam:title",{}).get("data",""))' 2>/dev/null
}
call() { busctl --user call "$NAME" /org/mpris/MediaPlayer2 org.mpris.MediaPlayer2.Player "$1" >/dev/null 2>&1; }
wait_prop() {   # property value seconds
    i=0
    while [ "$(prop "$1")" != "$2" ] && [ $i -lt $(( $3 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    [ "$(prop "$1")" = "$2" ]
}

w() { wine winepath -w "$1" 2>/dev/null | tr -d '\r'; }
wine "$EXE" "$(w "$T/one.wav")" "$(w "$T/two.wav")" "$(w "$T/clip.mp4")" >/dev/null 2>&1 &
wait_for STATE Playing 30 || fail "the queue is not playing: state '$(D STATE)' error '$(D ERROR)'"

i=0; NAME=""
while [ -z "$NAME" ] && [ $i -lt 40 ]; do NAME=$(player); [ -n "$NAME" ] || sleep 0.25; i=$((i + 1)); done
if [ -z "$NAME" ]; then
    fail "Media Player is not on the session bus (SMTC $(D SMTC))"
    echo "RESULT: FAIL"; exit 1
fi
pass "Media Player is on the session bus: $NAME"
wait_prop PlaybackStatus 's "Playing"' 5 && pass "it is Playing" || fail "PlaybackStatus: $(prop PlaybackStatus)"
[ "$(title)" = one ] && pass "the title is the file's name (one)" || fail "title '$(title)'"
[ "$(prop CanGoNext)" = "b true" ] && pass "Next is there (a second file)" || fail "CanGoNext: $(prop CanGoNext)"
[ "$(prop CanGoPrevious)" = "b true" ] && pass "Previous is there (it restarts the track)" || fail "CanGoPrevious: $(prop CanGoPrevious)"

# --- the desktop's controls, the player not in front ----------------------------------------
call PlayPause
wait_for STATE Paused 5 && pass "PlayPause pauses it" || fail "PlayPause: state $(D STATE)"
wait_prop PlaybackStatus 's "Paused"' 5 && pass "and the bus says Paused" || fail "PlaybackStatus: $(prop PlaybackStatus)"
call PlayPause
wait_for STATE Playing 5 && pass "PlayPause resumes it" || fail "PlayPause again: state $(D STATE)"
call Next
wait_for QUEUE "1 3" 5 && pass "Next moves to the second file" || fail "Next: queue $(D QUEUE)"
i=0; while [ "$(title)" != two ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
[ "$(title)" = two ] && pass "the title follows (two)" || fail "title after Next '$(title)'"
call Stop
wait_for STATE Stopped 5 && pass "Stop stops it" || fail "Stop: state $(D STATE)"
wait_prop PlaybackStatus 's "Stopped"' 5 && pass "and the bus says Stopped" || fail "PlaybackStatus: $(prop PlaybackStatus)"
call Next
wait_for QUEUE "2 3" 10 && wait_for STATE Playing 20 || fail "Next to the video: queue $(D QUEUE) state $(D STATE)"
i=0; while [ "$(title)" != clip ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
[ "$(title)" = clip ] && pass "a video's title is published too (clip)" || fail "video title '$(title)'"
[ "$(prop CanGoNext)" = "b false" ] && pass "no Next after the last file" || fail "CanGoNext at the end: $(prop CanGoNext)"

# --- the media keys, the player in front: WM_APPCOMMAND to it ------------------------------
# (the keys' way to it is Wine's, gated there: test-appcmdkeys)
MINGW=$(command -v x86_64-w64-mingw32-gcc) || { echo "SKIP: no mingw for the key helper"; exit 77; }
"$MINGW" -O2 -municode -o "$T/appcmd.exe" "$HERE/test/sg-appcommand.c" 2>/dev/null || { fail "sg-appcommand did not build"; exit 1; }
key() { wine "$T/appcmd.exe" SgMediaPlayer "$1" 2>/dev/null | tr -d '\r' | sed -n 's/^RESULT //p'; }
r=$(key pause)
wait_for STATE Paused 5 && [ "$r" = 1 ] && pass "in front: Pause pauses it, and it takes the key" || fail "Pause: state $(D STATE) result '$r'"
key pause >/dev/null; sleep 1
[ "$(D STATE)" = Paused ] && pass "Pause again does not resume it" || fail "Pause resumed it: $(D STATE)"
key play >/dev/null
wait_for STATE Playing 5 && pass "Play resumes it" || fail "Play: state $(D STATE)"
key play >/dev/null; sleep 1
[ "$(D STATE)" = Playing ] && pass "Play again does not pause it" || fail "Play paused it: $(D STATE)"
key play-pause >/dev/null
wait_for STATE Paused 5 && pass "Play/Pause toggles it" || fail "Play/Pause: state $(D STATE)"
r=$(key previous)
[ "$r" = 1 ] && pass "Previous: it takes the key" || fail "Previous: result '$r'"

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
