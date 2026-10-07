#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate: Credential Manager (sg-control's page, `control /name
# Microsoft.CredentialManager`), the sign-in notice for saved passwords that
# did not open, and Remote Desktop Connection's "Remember me".
#
#   1. a keyring made with the old password, a session signed in with a new
#      one (an administrator's reset): the page says it is locked; at sign-in
#      (/keyring-signin, the Run key) "Saved passwords are locked" comes up;
#      typing the old and the current password unlocks it -- re-encrypted with
#      the current one by sg-session's sg-keyring -- and says so; the secret
#      a program saved is there
#   2. with the keyring open, /keyring-signin shows nothing and exits
#   3. sg-mstsc's "Remember me" saves TERMSRV/<computer> as mstsc does; the
#      next connection finds it (not for another user name typed); the page
#      lists it; its Remove deletes it. With a wine-sg that keeps
#      credentials in the keyring (patch 1380), secret-tool finds it there.
#
# Needs sg-session's source beside this repo (SG_SESSION_SRC) for sg-keyring
# and its PAM file, gnome-keyring, secret-tool, Xvfb, xdotool.
# Mutants: SG_MUTANT_KEYRING_NOTICE (no question at sign-in),
# SG_MUTANT_KEYRING_NOTICE_ORDER (the passwords swapped) -- test/credmgr-check.sh --mutant NAME.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
SESS="${SG_SESSION_SRC:-$HERE/../sg-session}"
MUTANT=
[ "${1:-}" = --mutant ] && MUTANT=${2:?mutant name}
DPY="${SG_CREDMGR_DPY:-93}"
RC=0; XP=""
pass() { printf "PASS  %s\n" "$*"; }
fail() { printf "FAIL  %s\n" "$*"; RC=1; }
skip() { echo "SKIP: $*"; exit 77; }
for t in gnome-keyring-daemon secret-tool dbus-run-session Xvfb xdotool cc pkg-config x86_64-w64-mingw32-gcc; do
    command -v "$t" >/dev/null || skip "needs $t"
done
[ -x "$WINE_DIR/bin/wine" ] || skip "no wine-sg at $WINE_DIR"
[ -f "$SESS/greeter/sg-keyring.c" ] || skip "no sg-session source at $SESS (SG_SESSION_SRC)"
T=$(mktemp -d /var/tmp/sg-credmgr.XXXXXX); chmod 755 "$T"
cleanup() {
    : > "$T/s.stop"; sleep 1
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X$DPY-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

if [ -n "$MUTANT" ]; then
    cp -r "$HERE/src" "$T/src"
    CTLDIR="$T/src/control"
    sed -i "1i #define SG_MUTANT_$MUTANT" "$CTLDIR/credmgr.c"
    LIBS=$(make -s -pn -C "$HERE" -f Makefile 2>/dev/null | sed -n 's/^CONTROL_LIBS = //p' | head -1)
    # shellcheck disable=SC2086
    x86_64-w64-mingw32-gcc -O2 -municode -mwindows -Wno-missing-field-initializers -I "$HERE/src" -o "$T/sg-control64.exe" \
        "$CTLDIR"/*.c $LIBS >"$T/build.log" 2>&1 \
        || { echo "mutant build failed: $(tail -3 "$T/build.log")"; exit 1; }
    CTL="$T/sg-control64.exe"
else
    CTL="$HERE/build/sg-control64.exe"
fi
MSTSC="$HERE/build/sg-mstsc64.exe"
[ -f "$CTL" ] && [ -f "$MSTSC" ] || skip "sg-control / sg-mstsc not built (make build)"

# sg-keyring, and its PAM file with common-auth stood in for (the current
# password in a file of the gate's)
cc -O2 -o "$T/sg-keyring" "$SESS/greeter/sg-keyring.c" $(pkg-config --cflags --libs gio-2.0) -lpam || skip "cannot build sg-keyring"
mkdir -p "$T/conf"
cat > "$T/check.sh" <<EOF
#!/bin/sh
p=\$(tr -d '\\0'); [ "\$p" = "\$(cat "$T/current")" ]
EOF
chmod +x "$T/check.sh"
sed "s|^@include common-auth\$|auth required pam_exec.so expose_authtok quiet $T/check.sh|" \
    "$SESS/config/pam/stained-glass-keyring" > "$T/conf/stained-glass-keyring"
OLD='Old#Pass1234' NEW='New#Pass5678'
echo "$NEW" > "$T/current"

RT="$T/run"; mkdir -p "$RT"; chmod 700 "$RT"
cat > "$T/session.sh" <<'EOF'
k() { for p in $(pgrep -u "$(id -u)" -x gnome-keyring-d); do
        tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null | grep -qx "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR" && kill "$p"; done; sleep 0.5; }
# made with the old password, a saved password in it ...
printf '%s' "$OLD" | gnome-keyring-daemon --unlock --components=secrets >/dev/null 2>&1
printf 'mail-secret' | secret-tool store --label='Mail' app sg-mail
k
# ... then the sign-in with the new one (after an administrator's reset)
printf '%s' "$NEW" | gnome-keyring-daemon --unlock --components=secrets >/dev/null 2>&1
echo "$DBUS_SESSION_BUS_ADDRESS" > "$2.tmp" && mv "$2.tmp" "$2"
while [ ! -e "$1" ]; do sleep 0.3; done
k
EOF
env -i PATH=/usr/bin:/bin HOME="$HOME" XDG_RUNTIME_DIR="$RT" OLD="$OLD" NEW="$NEW" \
    dbus-run-session -- sh "$T/session.sh" "$T/s.stop" "$T/s.bus" >/dev/null 2>&1 &
_w=0; while [ ! -s "$T/s.bus" ] && [ $_w -lt 150 ]; do sleep 0.1; _w=$((_w + 1)); done
BUS=$(cat "$T/s.bus" 2>/dev/null)
[ -n "$BUS" ] || { fail "no session bus came up"; exit 1; }

rm -f "/tmp/.X$DPY-lock"
Xvfb ":$DPY" -screen 0 1280x800x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES="winemenubuilder.exe=d;mscoree,mshtml=" \
    PATH="$WINE_DIR/bin:$PATH" DISPLAY=":$DPY" DBUS_SESSION_BUS_ADDRESS="$BUS" XDG_RUNTIME_DIR="$RT" \
    SG_PAM_CONFDIR="$T/conf"
wine wineboot -i >/dev/null 2>&1; wineserver -w
HELPER=$(wine winepath -w "$T/sg-keyring" 2>/dev/null | tr -d '\r')
export SG_KEYRING_HELPER="$HELPER" WINEDEBUG=-all
st() { "$T/sg-keyring" status 2>/dev/null; }
dump() { wine "$CTL" --dump credentials 2>/dev/null </dev/null | tr -d '\r'; }
win_named() { xdotool search --name "$1" 2>/dev/null | head -1; }
wait_win() { _w=0; while [ -z "$(win_named "$1")" ] && [ $_w -lt "${2:-60}" ]; do sleep 1; _w=$((_w + 1)); done; win_named "$1"; }

# --- 1. locked: the page says so, the sign-in asks ----------------------------
[ "$(st)" = locked ] || fail "(setup: the keyring is not locked: $(st))"
out=$(dump)
case "$out" in *keyring=locked*) pass "Credential Manager sees the keyring locked" ;; *) fail "dump: $out" ;; esac
wine "$CTL" /keyring-signin >/dev/null 2>&1 &
SP=$!
w=$(wait_win "Saved passwords are locked" 60)
if [ -n "$w" ]; then
    pass "at sign-in a locked keyring asks: \"Saved passwords are locked\""
    # (no window manager on the gate's X server: a click gives the focus)
    sleep 2
    xdotool mousemove --window "$w" 240 217 click 1; sleep 0.5
    xdotool type --delay 40 "$OLD"; xdotool key Tab Tab; sleep 0.3
    xdotool type --delay 40 "$NEW"
    [ -n "${SG_CREDMGR_SHOTS:-}" ] && import -window root "$SG_CREDMGR_SHOTS/typed.png" 2>/dev/null
    xdotool key Return
    m=$(wait_win "^Saved passwords$" 30)
    [ -n "${SG_CREDMGR_SHOTS:-}" ] && import -window root "$SG_CREDMGR_SHOTS/answer.png" 2>/dev/null
    if [ -n "$m" ]; then xdotool mousemove --window "$m" 20 20 click 1; sleep 0.5; xdotool key Return; fi
else
    fail "no question at sign-in"
fi
_w=0; while kill -0 "$SP" 2>/dev/null && [ $_w -lt 30 ]; do sleep 1; _w=$((_w + 1)); done
kill "$SP" 2>/dev/null
if [ "$(st)" = open ]; then pass "the old and the current password unlock it"
else fail "after the question the keyring is $(st)"; fi
got=$(env -i PATH=/usr/bin:/bin HOME="$HOME" XDG_RUNTIME_DIR="$RT" DBUS_SESSION_BUS_ADDRESS="$BUS" secret-tool lookup app sg-mail 2>/dev/null)
[ "$got" = mail-secret ] && pass "... and the saved password is there" || fail "after unlocking: '$got'"
case "$(dump)" in *keyring=open*) pass "Credential Manager sees it open" ;; *) fail "dump after: $(dump)" ;; esac

# --- 2. open: nothing at sign-in -----------------------------------------------------
start=$(date +%s)
timeout 60 wine "$CTL" /keyring-signin >/dev/null 2>&1; rc=$?
took=$(( $(date +%s) - start ))
[ "$rc" = 0 ] && [ -z "$(win_named "Saved passwords are locked")" ] && [ "$took" -lt 30 ] \
    && pass "with the keyring open the sign-in asks nothing (${took}s)" || fail "open keyring: rc $rc, ${took}s"

# --- 3. Remote Desktop's "Remember me", listed and removed -------------------------------
export SG_MSTSC_TEST=1
wine "$MSTSC" /sg-remember /v:rdp.sgtest.lan 'SGTEST\alice' 'Rdp#Pass7' >/dev/null 2>&1
r=$(wine "$MSTSC" /sg-saved /v:rdp.sgtest.lan 2>/dev/null | tr -d '\r')
[ "$r" = 'SAVED SGTEST\alice 9' ] && pass "\"Remember me\" saves the credentials; the next connection finds them ($r)" \
    || fail "saved: '$r'"
r=$(wine "$MSTSC" /sg-saved /v:rdp.sgtest.lan bob 2>/dev/null | tr -d '\r')
[ "$r" = NONE ] && pass "... not for another user name typed" || fail "another user: '$r'"
out=$(dump)
case "$out" in *"credential=domain	TERMSRV/rdp.sgtest.lan	SGTEST\\alice"*) pass "Credential Manager lists them" ;;
    *) fail "listing: $out" ;; esac
kr=$(env -i PATH=/usr/bin:/bin HOME="$HOME" XDG_RUNTIME_DIR="$RT" DBUS_SESSION_BUS_ADDRESS="$BUS" \
    secret-tool search --all key termsrv/rdp.sgtest.lan 2>/dev/null | grep -c '^\[/')
if [ "$kr" = 1 ]; then pass "... kept in the keyring (wine-sg 1380)"
else echo "NOTE  this wine-sg keeps them in the registry (no patch 1380)"; fi
wine "$CTL" --credential-delete TERMSRV/rdp.sgtest.lan domain >/dev/null 2>&1
r=$(wine "$MSTSC" /sg-saved /v:rdp.sgtest.lan 2>/dev/null | tr -d '\r')
case "$r:$(dump)" in NONE:*TERMSRV*) fail "Remove left it listed" ;; NONE:*) pass "Remove deletes them" ;; *) fail "after Remove: '$r'" ;; esac

[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
