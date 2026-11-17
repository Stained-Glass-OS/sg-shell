#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Remote Desktop Connection's /span and /multimon really span two monitors (sg-shell -282).
#
# A headless sg-compositor with two outputs (1280x720 each, side by side), its Xwayland the display;
# a FreeRDP shadow server of a private Xvfb on a local port; sg-mstsc (wine) with /span, /multimon
# and an .rdp file saying "use multimon:i:1", starting the real sdl-freerdp3. The client's windows
# must cover both outputs, and sg-mstsc must still be running while the session is.
# The client runs through a wrapper that adds only what this throwaway server needs (no certificate
# to trust, a dummy password): the arguments sg-mstsc gives are the client's own.
#
#   SG_WINE_DIR=<wine-sg> SG_COMPOSITOR_BUILD=<dir with sg-compositor> test/mstsc-span-check.sh
#   SG_SHADOW_ROOT=<an unpacked freerdp3-shadow-x11> when freerdp-shadow-cli3 is not installed
# Skips (77) without wine-sg, the built executable, sdl-freerdp3, the shadow server or the compositor.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
WINE_DIR=${SG_WINE_DIR:-/opt/wine-sg}
EXE="$HERE/build/sg-mstsc64.exe"
COMP="${SG_COMPOSITOR_BUILD:-}/sg-compositor"
SHADOW=freerdp-shadow-cli3; SHADOW_LIB=
if [ -n "${SG_SHADOW_ROOT:-}" ]; then SHADOW="$SG_SHADOW_ROOT/usr/bin/freerdp-shadow-cli3"; SHADOW_LIB="$SG_SHADOW_ROOT/usr/lib/x86_64-linux-gnu"; fi
[ -x "$WINE_DIR/bin/wine" ] || { echo "SKIP: wine-sg not installed at $WINE_DIR"; exit 77; }
[ -f "$EXE" ] || { echo "SKIP: $EXE not built (make build)"; exit 77; }
[ -x "$COMP" ] || { echo "SKIP: no sg-compositor (SG_COMPOSITOR_BUILD)"; exit 77; }
for t in sdl-freerdp3 Xvfb xwininfo "$SHADOW"; do command -v "$t" >/dev/null 2>&1 || { echo "SKIP: $t not installed"; exit 77; }; done

T=$(mktemp -d /var/tmp/sg-mstscspan.XXXXXX)
chmod 700 "$T"; mkdir -m 700 "$T/comp-xdg" "$T/xdg"
PIDS=
cleanup() { for p in $PIDS; do kill "$p" 2>/dev/null; done; WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null; rm -rf "$T"; }
trap cleanup EXIT INT TERM
unset DISPLAY WAYLAND_DISPLAY XAUTHORITY
export XDG_RUNTIME_DIR="$T/xdg" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDEBUG=-all PATH="$WINE_DIR/bin:$PATH"
export WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d;winewayland.drv=d" WAYLAND_DISPLAY=sg-no-wayland
FAILED=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; FAILED=1; }

# the two-output compositor; its child reports the X display
XDG_RUNTIME_DIR="$T/comp-xdg" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=pixman \
    "$COMP" -L "$T/priv.sock" -C "$T/ctl.sock" -U "$(id -u)" -- \
    sh -c "echo \$DISPLAY > $T/disp; exec sleep 900" > "$T/comp.log" 2>&1 &
PIDS="$PIDS $!"
i=0; while [ ! -s "$T/disp" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
DISPLAY=$(cat "$T/disp" 2>/dev/null); export DISPLAY
[ -n "$DISPLAY" ] && [ "$DISPLAY" != ":0" ] || { echo "FAIL  no display from the compositor"; exit 1; }
sleep 1
WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' wineboot -i >/dev/null 2>&1
wineserver -w

# the server: a shadow of a private Xvfb, no authentication, this machine only
for d in 269 268 267; do [ -e /tmp/.X11-unix/X$d ] || break; done
Xvfb :$d -screen 0 1600x900x24 -nolisten tcp >/dev/null 2>&1 & PIDS="$PIDS $!"
sleep 1
PORT=$((34000 + $$ % 1000))
DISPLAY=:$d LD_LIBRARY_PATH=$SHADOW_LIB "$SHADOW" /port:$PORT -auth /bind-address:127.0.0.1 > "$T/shadow.log" 2>&1 & PIDS="$PIDS $!"
sleep 2

cat > "$T/client" <<'EOF2'
#!/bin/sh
case " $* " in *" /u:"*) u= ;; *) u=/u:gate ;; esac
exec sdl-freerdp3 "$@" $u /cert:ignore /p:gate
EOF2
chmod +x "$T/client"
CLIENT="\\\\?\\unix$(printf '%s' "$T/client" | tr / '\\\\')"
HELPER="\\\\?\\unix$(printf '%s' "$HERE/src/sg-rdp-connect" | tr / '\\\\')"

# span NAME ARGS...: sg-mstsc with ARGS; the client's windows must cover both outputs while it runs
span() {
    _name=$1; shift
    SG_MSTSC_TEST=1 SG_RDP_CLIENT="$CLIENT" SG_RDP_CONNECT="$HELPER" timeout -s KILL 60 wine "$EXE" "$@" > "$T/mstsc.log" 2>&1 &
    _m=$!
    sleep 10
    _w=$(xwininfo -root -children 2>/dev/null | grep -c '"FreeRDP: 127.0.0.1')
    _x=$(xwininfo -root -children 2>/dev/null | grep '"FreeRDP: 127.0.0.1' | sed -n 's/.*  \([0-9]*\)x\([0-9]*\)+\([0-9-]*\)+\([0-9-]*\)  .*/\3 \1/p' |
         awk 'BEGIN { l = 99999; r = 0 } { if ($1 < l) l = $1; if ($1 + $2 > r) r = $1 + $2 } END { print l, r }')
    if kill -0 $_m 2>/dev/null; then _alive=yes; else _alive=no; fi
    if [ "$_x" = "0 2560" ]; then pass "$_name: the client covers both outputs ($_w windows, x $_x)"
    else fail "$_name: the client's windows span x '$_x' ($_w windows), want 0 2560"; tail -3 "$T/shadow.log"; fi
    [ "$_alive" = yes ] && pass "$_name: sg-mstsc runs as long as the session" || fail "$_name: sg-mstsc returned while the session ran"
    pkill -f "sdl-freerdp3 /v:127.0.0.1:$PORT" 2>/dev/null
    i=0; while kill -0 $_m 2>/dev/null && [ $i -lt 30 ]; do sleep 0.5; i=$((i + 1)); done
    kill -0 $_m 2>/dev/null && { fail "$_name: sg-mstsc did not return when the session ended"; kill $_m; } || pass "$_name: sg-mstsc returned when the session ended"
}
span "/span" /v:127.0.0.1:$PORT /span
span "/multimon" /v:127.0.0.1:$PORT /multimon
printf '%s\r\n' "full address:s:127.0.0.1:$PORT" 'username:s:gate' 'screen mode id:i:2' 'use multimon:i:1' > "$T/mm.rdp"
span ".rdp use multimon:i:1" "$(winepath -w "$T/mm.rdp")"

echo
if [ "$FAILED" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$FAILED"
