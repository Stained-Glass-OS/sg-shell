#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for Media Player (sg-media): real playback through Wine's DirectShow and
# GStreamer, driven like a person drives it.
#
#   - a WAV (python3) and a solid-colour H.264 MP4 and a WebM (ffmpeg) are
#     made at test time; defaults/74-sg-media.reg is imported, so a double-click
#     (ShellExecute of the file) opens the player through the association
#   - audio: Playing, the duration right, the position advancing; Space pauses
#     (the position holds) and resumes; a click on the seek bar moves it; Right
#     skips 5 s; Down lowers the volume, M mutes
#   - a second launch hands its file to the running player (one window); the
#     video plays in the video pane and a pixel inside it is the clip's colour;
#     F11 fills the screen with it, Escape comes back
#   - a queue of two: the Next button moves to the second
#
# SG_MEDIA_DUMP is the player's state file. Screenshots: build/media-*.png.
# Needs wine-sg, Xvfb, xdotool, ImageMagick, python3 and ffmpeg (with libx264
# and libvpx); skips (77) without them. EXE= tests another build (mutants).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${EXE:-$HERE/build/sg-media64.exe}"
OUT="$HERE/build"
RC=0; DPY=${DPY:-214}; XP=""
# printf, not echo: dash's echo eats the backslashes of Windows paths
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }

for need in Xvfb xdotool import python3 ffmpeg; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-media-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

# --- media, made here ---------------------------------------------------------------------
python3 - "$T/tone.wav" <<'EOF'
import math, struct, sys, wave
w = wave.open(sys.argv[1], "wb"); w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
frames = bytearray()
for i in range(44100 * 20):
    s = int(6000 * math.sin(2 * math.pi * 440 * i / 44100)); frames += struct.pack("<hh", s, s)
w.writeframes(bytes(frames)); w.close()
EOF
# A clip of one colour (RGB 32,160,96), so a pixel of the pane says the video is drawn.
ffmpeg -loglevel error -y -f lavfi -i color=c=0x20A060:size=320x240:rate=25:duration=20 \
    -f lavfi -i sine=frequency=330:duration=20 -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest "$T/clip.mp4" \
    || { echo "SKIP: ffmpeg cannot make an H.264 clip"; exit 77; }
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=320x240:rate=25:duration=8 -c:v libvpx "$T/second.webm" \
    || { echo "SKIP: ffmpeg cannot make a VP8 clip"; exit 77; }
# MPEG-4 Part 2 in AVI: Wine's own AVI splitter renders only its sound, so this
# proves the graph goes through winegstreamer's splitter.
ffmpeg -loglevel error -y -f lavfi -i testsrc2=size=320x240:rate=25:duration=8 -f lavfi -i sine=duration=8 \
    -c:v mpeg4 -c:a libmp3lame -shortest "$T/third.avi" || { echo "SKIP: ffmpeg cannot make an AVI"; exit 77; }

# --- Wine and a shell desktop ---------------------------------------------------------------
[ -e "/tmp/.X11-unix/X$DPY" ] && { echo "SKIP: display :$DPY is taken (DPY= to choose another)"; exit 77; }
Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
i=0; while [ ! -e "/tmp/.X11-unix/X$DPY" ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
kill -0 "$XP" 2>/dev/null || { echo "FAIL  Xvfb :$DPY did not start"; exit 1; }
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH" SG_MEDIA_DUMP="$T/dump.txt"
wine wineboot --init >/dev/null 2>&1; wineserver -w
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
# The package's associations, pointed at this build.
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
python3 - "$HERE/defaults/74-sg-media.reg" "$winexe" "$T/media.reg" <<'EOF'
import sys
s = open(sys.argv[1]).read()
exe = sys.argv[2].replace("\\", "\\\\")
open(sys.argv[3], "w").write(s.replace("Z:\\\\usr\\\\libexec\\\\stained-glass\\\\shell\\\\sg-media64.exe", exe))
EOF
wine reg import "$(wine winepath -w "$T/media.reg" | tr -d '\r')" >/dev/null 2>&1 \
    && pass "defaults/74-sg-media.reg imports" || fail "defaults/74-sg-media.reg does not import"
wineserver -w
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

D() { sed -n "s/^$1 //p" "$SG_MEDIA_DUMP" 2>/dev/null | head -1; }
wait_for() {   # key value seconds
    i=0
    while [ "$(D "$1")" != "$2" ] && [ $i -lt $(( $3 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    [ "$(D "$1")" = "$2" ]
}
pos() { D POSITION_MS; }
shot() { import -window root "$OUT/media-$1.png" 2>/dev/null; }
centre() { set -- $(D "$1"); echo $(( ($1 + $3) / 2 )) $(( ($2 + $4) / 2 )); }
focus_player() { set -- $(D PLAYRECT); xdotool mousemove $(( $1 - 200 )) $(( $2 - 20 )) click 1; sleep 0.3; }
pixel() { convert "$OUT/media-$1.png" -format "%[fx:int(255*p{$2,$3}.r)] %[fx:int(255*p{$2,$3}.g)] %[fx:int(255*p{$2,$3}.b)]" info: 2>/dev/null; }
near() {   # "r g b" "r g b" tolerance
    python3 -c "import sys; a=list(map(int,sys.argv[1].split())); b=list(map(int,sys.argv[2].split())); sys.exit(0 if len(a)==3 and all(abs(x-y)<=int(sys.argv[3]) for x,y in zip(a,b)) else 1)" "$1" "$2" "$3"
}

# --- audio, opened as a double-click opens it ------------------------------------------------
wine start /unix "$T/tone.wav" >/dev/null 2>&1
if wait_for STATE Playing 30; then pass "tone.wav opens through its association and plays"
else fail "tone.wav is not playing: state '$(D STATE)' error '$(D ERROR)'"; fi
case "$(D TITLE)" in "tone.wav - Media Player") pass "title: $(D TITLE)" ;; *) fail "title is '$(D TITLE)'" ;; esac
dur=$(D DURATION_MS)
[ -n "$dur" ] && [ "$dur" -ge 19800 ] && [ "$dur" -le 20200 ] && pass "duration ${dur} ms (20 s)" || fail "duration '$dur' ms, not 20 s"
[ "$(D VIDEO | cut -d' ' -f1)" = 0 ] && pass "audio has no video pane" || fail "audio claims video: $(D VIDEO)"
p1=$(pos); sleep 1.5; p2=$(pos)
[ $(( p2 - p1 )) -ge 1000 ] && [ $(( p2 - p1 )) -le 2200 ] && pass "position advances ($p1 -> $p2 ms)" || fail "position does not advance at speed ($p1 -> $p2)"
sleep 0.5; shot audio

focus_player
xdotool key space
wait_for STATE Paused 5 && pass "Space pauses" || fail "Space did not pause: $(D STATE)"
p1=$(pos); sleep 1.2; p2=$(pos)
[ $(( p2 - p1 )) -le 100 ] && pass "paused position holds ($p1 -> $p2)" || fail "position moves while paused ($p1 -> $p2)"
xdotool key space
wait_for STATE Playing 5 && pass "Space resumes" || fail "Space did not resume: $(D STATE)"

# a click at three quarters of the seek bar
set -- $(D SEEKRECT)
sx=$(( $1 + 8 + ( $3 - $1 - 16 ) * 3 / 4 )); sy=$(( ($2 + $4) / 2 ))
xdotool mousemove "$sx" "$sy" click 1; sleep 0.8
p=$(pos)
[ "$p" -ge 14000 ] && [ "$p" -le 16800 ] && pass "a click on the seek bar seeks ($p ms of 20000)" || fail "seek bar click left the position at $p ms"
xdotool key Left; sleep 0.5; q=$(pos)
[ $(( p - q )) -ge 3500 ] && [ $(( p - q )) -le 5500 ] && pass "Left skips back 5 s ($p -> $q)" || fail "Left did not skip back 5 s ($p -> $q)"
v=$(D VOLUME); xdotool key Down Down; sleep 0.4
[ "$(D VOLUME)" = $(( v - 10 )) ] && pass "Down lowers the volume ($v -> $(D VOLUME))" || fail "volume $v -> $(D VOLUME)"
xdotool key m; sleep 0.4
[ "$(D MUTED)" = 1 ] && pass "M mutes" || fail "M did not mute"
xdotool key m; sleep 0.3
shot audio-seek

# --- video: a second launch hands its file to this window -----------------------------------
wine "$winexe" "$(wine winepath -w "$T/clip.mp4" | tr -d '\r')" >/dev/null 2>&1 &
i=0; while ! D FILE | grep -q 'clip.mp4' && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
wait_for STATE Playing 20 && D FILE | grep -q clip.mp4 && pass "a second launch plays clip.mp4 in the same player" \
    || fail "clip.mp4 not playing: $(D FILE) $(D STATE) $(D ERROR)"
n=$(xdotool search --name ' - Media Player$' 2>/dev/null | wc -l)
[ "$n" -le 1 ] && pass "one Media Player window" || fail "$n Media Player windows"
[ "$(D VIDEO)" = "1 320 240" ] && pass "video 320x240 in the pane" || fail "video: '$(D VIDEO)'"
sleep 1.5; shot video
set -- $(centre VIDEORECT); vx=$1; vy=$2
px=$(pixel video "$vx" "$vy")
near "$px" "32 160 96" 40 && pass "the video pane shows the clip ($px at $vx,$vy)" || fail "video pane pixel is '$px', not the clip's 32 160 96"
focus_player
xdotool key F11; sleep 1.5
[ "$(D FULLSCREEN)" = 1 ] && pass "F11: full screen" || fail "F11 did not go full screen"
set -- $(D VIDEORECT)
[ "$1" -le 0 ] && [ "$2" -le 0 ] && [ "$3" -ge 1024 ] && [ "$4" -ge 768 ] && pass "the video fills the screen ($*)" || fail "full-screen video rect: $*"
shot fullscreen
px=$(pixel fullscreen 512 384)
near "$px" "32 160 96" 40 && pass "full screen shows the clip ($px)" || fail "full screen centre is '$px'"
xdotool key Escape; sleep 1
[ "$(D FULLSCREEN)" = 0 ] && pass "Escape leaves full screen" || fail "Escape did not leave full screen"

# --- a queue of three, and Next -----------------------------------------------------------------
wine "$winexe" "$(wine winepath -w "$T/tone.wav" | tr -d '\r')" "$(wine winepath -w "$T/second.webm" | tr -d '\r')" \
    "$(wine winepath -w "$T/third.avi" | tr -d '\r')" >/dev/null 2>&1 &
i=0; while [ "$(D QUEUE)" != "0 3" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
[ "$(D QUEUE)" = "0 3" ] && pass "three files queued" || fail "queue: $(D QUEUE)"
set -- $(centre NEXTRECT); xdotool mousemove "$1" "$2" click 1
i=0; while [ "$(D QUEUE)" != "1 3" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
[ "$(D QUEUE)" = "1 3" ] && wait_for STATE Playing 10 && D FILE | grep -q second.webm \
    && pass "Next plays the second (second.webm, VP8)" || fail "Next: $(D QUEUE) $(D FILE) $(D STATE) $(D ERROR)"
sleep 1; shot queue
set -- $(centre NEXTRECT); xdotool mousemove "$1" "$2" click 1
i=0; while [ "$(D QUEUE)" != "2 3" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
wait_for STATE Playing 10
[ "$(D QUEUE)" = "2 3" ] && [ "$(D VIDEO)" = "1 320 240" ] && pass "third.avi (MPEG-4 in AVI) plays with its picture" \
    || fail "third.avi: $(D QUEUE) $(D STATE) video '$(D VIDEO)' $(D ERROR)"
sleep 1; shot avi

echo
[ $RC = 0 ] && echo "media-check: all passed" || echo "media-check: FAILED"
exit $RC
