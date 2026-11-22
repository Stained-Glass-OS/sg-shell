#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate: Media Player's picture keeps pace with its sound through a long film.
#
# David's own render (H.264 High, AAC, ten minutes) froze in the player: the
# sound played on, the picture stopped, every film (2026-11-18). Wine's
# default video renderer (the VMR-7) draws by blits to a DirectDraw primary
# surface -- the GL front buffer of the video window -- and with NVIDIA's
# driver under Xwayland those draws stop reaching the window. sg-media now
# renders with the VMR-9, which presents through a swap chain.
#
# A ten-minute H.264 (High, B-frames) + AAC clip is made here (ffmpeg). Each
# frame carries its own time as a 14-bit bar code (tenths of a second), so a
# screenshot of the video pane says which frame the screen shows. Every two
# seconds the gate reads the player's position (SG_MEDIA_DUMP: the audio
# clock) and the frame on screen:
#   - the picture must never stand still while the position advances
#     (two samples in a row), and must stay within 1.5 s of the position;
#   - through a seek (Right: +5 s) and a pause and resume (Space);
#   - with the default renderer (dump RENDERER vmr9 -- the VMR-7 is the bug)
#     and again with the GDI Video Renderer (SG_MEDIA_RENDERER=gdi).
#
# The screen: a private headless weston with Xwayland, its GL renderer on the
# host's GPU (weston-screenshooter reads what the compositor shows), so on an
# NVIDIA host it is the path David's desktop takes; Xvfb without weston, or
# with SG_PACE_DISPLAY=xvfb. PACE_SECS (default 70) is how long the first
# part plays. EXE= tests another build. Skips (77) without Wine, ffmpeg,
# mingw, ImageMagick or a display server. The keys go to the player as
# WM_KEYDOWN from a small Windows program (headless Xwayland has no seat: an
# XTEST key there ends the X server).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${EXE:-$HERE/build/sg-media64.exe}"
PACE_SECS="${PACE_SECS:-70}"
RC=0; XP=""; DPY=""
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }

MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
for need in import xwininfo python3 ffmpeg "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-media-pace.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

# --- the clip: 960x540, 30 fps, its time as a bar code ----------------------------------
# A white strip across the top (which way up the screenshot is), and 14 boxes:
# box k is white when bit k of the frame's time in tenths of a second is set.
bars=""
k=0
while [ $k -lt 14 ]; do
    bars="$bars,drawbox=x=$((24 + k * 66)):y=200:w=60:h=140:color=white:t=fill:enable='eq(mod(floor(t*10/pow(2\\,$k))\\,2)\\,1)'"
    k=$((k + 1))
done
# PACE_SIZE=1920x1080 scales the clip up (David's renders are 1080p: four times the pixels to decode and convert).
PACE_SIZE="${PACE_SIZE:-960x540}"
ffmpeg -loglevel error -y -f lavfi -i "color=c=0x203040:s=960x540:r=30:d=600,drawbox=x=0:y=60:w=960:h=40:color=white:t=fill$bars,scale=${PACE_SIZE%x*}:${PACE_SIZE#*x}:flags=neighbor" \
    -f lavfi -i "sine=frequency=330:duration=600" -c:v libx264 -preset veryfast -profile:v high -bf 2 -g 15 -pix_fmt yuv420p \
    -c:a aac -shortest "$T/pace.mp4" || { echo "SKIP: ffmpeg cannot make an H.264 + AAC clip"; exit 77; }

# --- the display -------------------------------------------------------------------------
mode=xvfb
if [ "${SG_PACE_DISPLAY:-}" != xvfb ] && command -v weston >/dev/null && command -v weston-screenshooter >/dev/null \
        && command -v Xwayland >/dev/null; then
    mkdir -p "$T/shots"
    weston --backend=headless --renderer=gl --width=1280 --height=900 --xwayland --debug --idle-time=0 \
        --socket=sg-pace-wl > "$T/weston.log" 2>&1 & XP=$!
    i=0; while ! grep -q 'xserver listening on display' "$T/weston.log" 2>/dev/null && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
    DPY=$(sed -n 's/.*xserver listening on display :\([0-9]*\).*/\1/p' "$T/weston.log" | head -1)
    if [ -n "$DPY" ] && [ "$DPY" != 0 ] && grep -q 'Using GL renderer' "$T/weston.log"; then
        mode=weston
        printf 'INFO  weston + Xwayland on :%s, %s\n' "$DPY" "$(sed -n 's/.*GL renderer: //p' "$T/weston.log" | head -1)"
    else
        kill "$XP" 2>/dev/null; XP=""; DPY=""
    fi
fi
if [ $mode = xvfb ]; then
    command -v Xvfb >/dev/null || { echo "SKIP: no weston and no Xvfb"; exit 77; }
    DPY=${PACE_DPY:-219}
    [ -e "/tmp/.X11-unix/X$DPY" ] && { echo "SKIP: display :$DPY is taken (PACE_DPY= to choose another)"; exit 77; }
    Xvfb ":$DPY" -screen 0 1280x900x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
    i=0; while [ ! -e "/tmp/.X11-unix/X$DPY" ] && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
    printf 'INFO  Xvfb :%s\n' "$DPY"
fi
kill -0 "$XP" 2>/dev/null || { echo "FAIL  the display server did not start"; exit 1; }
case "$DPY" in ''|0) echo "FAIL  refusing display :$DPY"; exit 1 ;; esac

export DISPLAY=":$DPY" WAYLAND_DISPLAY=sg-pace-no-wayland WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDEBUG=-all
export WINEDLLOVERRIDES="$WINEDLLOVERRIDES;mscoree,mshtml="
export PATH="$WINE_DIR/bin:$PATH" WINESERVER="$WINE_DIR/bin/wineserver" SG_MEDIA_DUMP="$T/dump.txt"
wine wineboot --init >/dev/null 2>&1; wineserver -w
wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
wine reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x800 /f >/dev/null 2>&1
wineserver -w
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1280x800 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
clip=$(wine winepath -w "$T/pace.mp4" | tr -d '\r')
# keys for the player, posted to its window
cat > "$T/key.c" <<'EOF'
#include <windows.h>
int wmain(int argc, WCHAR **argv)
{
    HWND w = FindWindowW(L"SgMediaPlayer", NULL);
    if (!w || argc < 2) return 1;
    PostMessageW(w, WM_KEYDOWN, argv[1][0] == 's' ? VK_SPACE : VK_RIGHT, 0);
    return 0;
}
EOF
"$MINGW" -O2 -municode -o "$T/key.exe" "$T/key.c" || { echo "SKIP: mingw cannot build the key helper"; exit 77; }
key() { wine "$T/key.exe" "$1" >/dev/null 2>&1; }

D() { sed -n "s/^$1 //p" "$SG_MEDIA_DUMP" 2>/dev/null | head -1; }
wait_for() { i=0; while [ "$(D "$1")" != "$2" ] && [ $i -lt $(( $3 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done; [ "$(D "$1")" = "$2" ]; }
# where the desktop's X window is on the screen (weston places it below its panel)
origin() {
    w=$(xwininfo -root -tree 2>/dev/null | awk '/"shell - Wine Desktop"/ {print $1; exit}')
    if [ -n "$w" ]; then xwininfo -id "$w" | awk '/Absolute upper-left X/ {x=$4} /Absolute upper-left Y/ {y=$4} END {print x+0, y+0}'
    else echo 0 0; fi
}
# the screen as shown, as a PNG
grab() {
    if [ $mode = weston ]; then
        rm -f "$T/shots/"*.png
        (cd "$T/shots" && WAYLAND_DISPLAY=sg-pace-wl XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" timeout 10 weston-screenshooter >/dev/null 2>&1)
        f=$(ls "$T/shots/"*.png 2>/dev/null | head -1); [ -n "$f" ] && mv "$f" "$1"
    else
        import -window root "$1" 2>/dev/null
    fi
}
# the frame on screen: its time in ms from the bar code, or -1
frame_ms() {
    grab "$T/s.png" || { echo -1; return; }
    convert "$T/s.png" -depth 8 rgb:"$T/s.rgb" 2>/dev/null || { echo -1; return; }
    set -- $(D VIDEORECT)
    python3 - "$T/s.rgb" "$(identify -format '%w %h' "$T/s.png")" "$1 $2 $3 $4" "$(origin)" <<'EOF'
import sys
data = open(sys.argv[1], "rb").read()
W, H = map(int, sys.argv[2].split())
l, t, r, b = map(int, sys.argv[3].split())
ox, oy = map(int, sys.argv[4].split())
pw, ph = r - l, b - t
vw, vh = (pw, pw * 9 // 16) if pw * 9 <= ph * 16 else (ph * 16 // 9, ph)
vx, vy = ox + l + (pw - vw) // 2, oy + t + (ph - vh) // 2
def lum(cx, cy, flip):
    x = vx + cx * vw // 960
    y = vy + cy * vh // 540
    if flip: y = H - 1 - y
    if not (0 <= x < W and 0 <= y < H): return -1
    o = (y * W + x) * 3
    return (data[o] + data[o + 1] + data[o + 2]) // 3
for flip in (False, True):
    # the strip is white, the background beside the boxes is dark
    if lum(480, 80, flip) > 160 and lum(480, 160, flip) < 110 and lum(12, 270, flip) < 110:
        v = 0
        for k in range(14):
            if lum(54 + k * 66, 270, flip) > 128: v |= 1 << k
        print(v * 100); sys.exit()
print(-1)
EOF
}
# play_check LABEL SECONDS: sample every 2 s; fail on a stall or drift
play_check() {
    label=$1; secs=$2; n=0; stall=0; worst=0; far=0; bad=0; lastv=-2; lastp=-1; log=""
    end=$(( $(date +%s) + secs ))
    while [ "$(date +%s)" -lt "$end" ]; do
        sleep 2
        p=$(D POSITION_MS); v=$(frame_ms)
        n=$((n + 1)); log="$log $p/$v"
        if [ "$v" -lt 0 ]; then bad=$((bad + 1)); lastv=-2; continue; fi
        d=$(( p > v ? p - v : v - p ))
        [ "$d" -gt "$worst" ] && worst=$d
        [ "$d" -gt 1500 ] && far=$((far + 1))
        if [ "$v" = "$lastv" ] && [ $(( p - lastp )) -ge 1500 ]; then
            stall=$((stall + 1))
            [ $stall = 1 ] && printf 'INFO  %s: the picture stands at %d ms while the sound goes %d -> %d ms\n' "$label" "$v" "$lastp" "$p"
        fi
        lastv=$v; lastp=$p
    done
    printf 'INFO  %s: position/frame (ms):%s\n' "$label" "$log"
    [ $bad -le $(( n / 5 )) ] && pass "$label: the video pane shows the clip ($((n - bad)) of $n samples read)" \
        || fail "$label: the clip's frames are not on screen ($bad of $n samples unreadable)"
    [ $stall = 0 ] && pass "$label: the picture never stood still while the sound played ($n samples)" \
        || fail "$label: the picture stood still while the sound played on ($stall times)"
    [ $far = 0 ] && pass "$label: picture within 1.5 s of the sound (worst $worst ms)" \
        || fail "$label: the picture drifted from the sound ($far samples past 1.5 s, worst $worst ms)"
}

# --- 1: the renderer sg-media picks, a long play, a seek, a pause --------------------------
wine "$EXE" "$clip" >/dev/null 2>&1 &
if wait_for STATE Playing 40; then pass "pace.mp4 plays: $(D VIDEO), renderer $(D RENDERER)"
else fail "pace.mp4 is not playing: $(D STATE) $(D ERROR)"; exit 1; fi
dur=$(D DURATION_MS)
[ "$dur" -ge 599000 ] && [ "$dur" -le 601000 ] && pass "a ten-minute film (${dur} ms)" || fail "duration '$dur' ms"
[ "$(D RENDERER)" = vmr9 ] && pass "the Video Mixing Renderer 9 shows it (not the VMR-7)" \
    || fail "renderer is '$(D RENDERER)', not vmr9 (Wine's VMR-7 froze the picture)"
sleep 2
play_check "play" "$PACE_SECS"

p=$(D POSITION_MS); key right; sleep 1.5; q=$(D POSITION_MS)
[ $(( q - p )) -ge 5000 ] && pass "Right skips ahead ($p -> $q ms)" || fail "Right did not skip ahead ($p -> $q ms)"
play_check "after a seek" 12
key space
if wait_for STATE Paused 5; then
    sleep 3; p=$(D POSITION_MS); v=$(frame_ms); sleep 3; q=$(D POSITION_MS); w=$(frame_ms)
    [ $(( q - p )) -le 100 ] && [ "$v" = "$w" ] && pass "paused: sound and picture hold ($p ms, frame $v)" \
        || fail "paused: position $p -> $q, frame $v -> $w"
    key space
    wait_for STATE Playing 5 && pass "Space resumes" || fail "Space did not resume: $(D STATE)"
    play_check "after a pause" 12
else
    fail "Space did not pause: $(D STATE)"
fi

# --- 2: the GDI Video Renderer (NVIDIA before 555) ------------------------------------------
wineserver -k; sleep 1
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1280x800 > "$T/explorer2.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer2.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
rm -f "$SG_MEDIA_DUMP"
SG_MEDIA_RENDERER=gdi wine "$EXE" "$clip" >/dev/null 2>&1 &
if wait_for STATE Playing 40 && [ "$(D RENDERER)" = gdi ]; then
    pass "GDI renderer: pace.mp4 plays through it"
    sleep 2
    play_check "GDI renderer" 20
else
    fail "GDI renderer: $(D STATE) renderer '$(D RENDERER)' $(D ERROR)"
fi

[ $RC = 0 ] && echo "media-pace-check: all passed" || echo "media-pace-check: FAILED"
exit $RC
