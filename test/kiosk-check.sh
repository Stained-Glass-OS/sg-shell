#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# The kiosk (David 2026-10-07: "a tablet that boots to the Sonos player"):
#
# 1. sg-kiosk64.exe, the kiosk app's launcher (sg-session's sg-kiosk runs
#    it): a stand-in app (test/sg-kiosk-probe.c) that shows its window
#    "normal", as WPF programs do, is maximized, started in its folder with
#    its arguments, and waited for (its exit code back); through a shortcut
#    too; and a launcher that hands over to the real app and exits is waited
#    past, until the app closes. Mutants SG_MUTANT_KIOSK_NOT_MAXIMIZED,
#    SG_MUTANT_KIOSK_NO_HANDOFF.
# 2. Settings > Accounts > Kiosk (ms-settings:assignedaccess), with sg-admind
#    in test mode on the gate's spool and files: choosing the account to sign
#    in automatically writes greetd's initial session; "Choose an app" (the
#    Start app named by SG_TEST_PICK) keeps a Windows program's path, folder
#    and arguments, or a Linux app's .desktop file; both undone again.
# 3. Settings > Apps > Startup: "Add an app" puts a Start app's shortcut in
#    the person's Startup folder (the shell opens it at sign-in, wine-sg
#    0754); Remove takes it away. Mutant SG_MUTANT_STARTUP_ADD_NOOP.
# 4. The sign-in question about locked saved passwords is not asked of the
#    account that signs in by itself. Mutant SG_MUTANT_AUTOLOGON_KEYRING.
#
# Needs wine-sg, Xvfb, xdotool, python3 3.11 and mingw; skips (77) without.
# SG_WINE_DIR another Wine (bin/wine, bin/wineserver).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; XP=""; LOOP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for c in Xvfb xdotool python3 "$MINGW"; do command -v "$c" >/dev/null || { echo "SKIP: $c missing"; exit 77; }; done
python3 -c 'import tomllib' 2>/dev/null || { echo "SKIP: python3 without tomllib"; exit 77; }
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$HERE/build/sg-settings64.exe" ] || { echo "SKIP: wine-sg or build/sg-settings64.exe missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-kiosk-check.XXXXXX)
# shellcheck disable=SC2317
cleanup() {
    [ -n "$LOOP" ] && kill "$LOOP" 2>/dev/null
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T" "$SG_GATE_HOME"
}
trap cleanup EXIT INT TERM

CFLAGS="-O2 -municode -mwindows -Wno-missing-field-initializers"
build_kiosk() {   # OUT [DEFINES...]
    _o=$1; shift
    # shellcheck disable=SC2086
    "$MINGW" $CFLAGS "$@" -o "$_o" "$HERE/src/sg-kiosk.c" -lshell32 -luser32 -lole32 -luuid
}
build_kiosk "$T/sg-kiosk64.exe" || { fail "sg-kiosk.c does not build"; exit 1; }
build_kiosk "$T/mut-max.exe" -DSG_MUTANT_KIOSK_NOT_MAXIMIZED
build_kiosk "$T/mut-handoff.exe" -DSG_MUTANT_KIOSK_NO_HANDOFF
# shellcheck disable=SC2086
"$MINGW" $CFLAGS -o "$T/probe.exe" "$HERE/test/sg-kiosk-probe.c" -lshell32 -luser32 -lole32 -luuid || { fail "the probe does not build"; exit 1; }

Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/display")" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
winpath() { wine winepath -w "$1" 2>/dev/null | tr -d '\r'; }

# ---- 1. the launcher -------------------------------------------------------------
APPDIR="$WINEPREFIX/drive_c/kprobe"
mkdir -p "$APPDIR"; cp "$T/probe.exe" "$APPDIR/sg-kiosk-probe64.exe"
MARK="$T/mark.txt"; WMARK=$(winpath "$MARK")
launch() {   # LAUNCHER PROGRAM DIR ARGS -> "rc seconds"
    rm -f "$MARK"
    _s=$(date +%s.%N)
    SG_KIOSK_POLL=200 timeout 60 wine "$1" "$2" "$3" "$4" >/dev/null 2>&1; _rc=$?
    echo "$_rc $(echo "$(date +%s.%N) - $_s" | bc)"
}
r=$(launch "$T/sg-kiosk64.exe" 'C:\kprobe\sg-kiosk-probe64.exe' 'C:\kprobe' "\"$WMARK\" 2")
set -- $r
grep -q '^zoomed=1 ' "$MARK" 2>/dev/null && pass "the app's window, shown \"normal\", is maximized" || fail "not maximized: $(cat "$MARK" 2>&1)"
tr -d '\r' < "$MARK" 2>/dev/null | grep -qi 'cwd=C:\\kprobe$' && pass "...started in its folder, with its arguments" || fail "folder: $(cat "$MARK" 2>&1)"
[ "$1" = 7 ] && awk -v s="$2" 'BEGIN { exit !(s >= 1.8) }' && pass "...and waited for: its exit code (7) after it closed (${2%.*} s)" || fail "wait: rc $1 after $2 s"
r=$(launch "$T/mut-max.exe" 'C:\kprobe\sg-kiosk-probe64.exe' 'C:\kprobe' "\"$WMARK\" 2")
grep -q '^zoomed=0 ' "$MARK" 2>/dev/null && pass "MUTANT KIOSK_NOT_MAXIMIZED (the window stays normal) is caught" || fail "KIOSK_NOT_MAXIMIZED not detected: $(cat "$MARK" 2>&1)"
wine "$T/probe.exe" --mklnk 'C:\kprobe\Probe.lnk' 'C:\kprobe\sg-kiosk-probe64.exe' "\"$WMARK\" 2" 'C:\kprobe' >/dev/null 2>&1
r=$(launch "$T/sg-kiosk64.exe" 'C:\kprobe\Probe.lnk' '' '')
set -- $r
grep -q '^zoomed=1 ' "$MARK" 2>/dev/null && [ "$1" = 7 ] && pass "a shortcut: its program, maximized, waited for" || fail "shortcut: rc $1, $(cat "$MARK" 2>&1)"
r=$(launch "$T/sg-kiosk64.exe" 'C:\kprobe\sg-kiosk-probe64.exe' 'C:\kprobe' "--handoff \"$WMARK\"")
set -- $r
grep -q '^zoomed=1 ' "$MARK" 2>/dev/null && awk -v s="$2" 'BEGIN { exit !(s >= 3.8) }' \
    && pass "a launcher that hands over and exits: the app it started is maximized and waited for (${2%.*} s)" || fail "hand-over: $2 s, $(cat "$MARK" 2>&1)"
r=$(launch "$T/mut-handoff.exe" 'C:\kprobe\sg-kiosk-probe64.exe' 'C:\kprobe' "--handoff \"$WMARK\"")
set -- $r
awk -v s="$2" 'BEGIN { exit !(s < 3.8) }' && pass "MUTANT KIOSK_NO_HANDOFF (back as the launcher exits: started again beside the app) is caught" \
    || fail "KIOSK_NO_HANDOFF not detected ($2 s)"
r=$(launch "$T/sg-kiosk64.exe" 'C:\kprobe\none.exe' '' '')
set -- $r
[ "$1" = 1 ] && pass "a program that is not there: exit code 1, at once" || fail "missing program: rc $1"
wineserver -k 2>/dev/null; sleep 1

# ---- 2. Settings > Accounts > Kiosk ----------------------------------------------
ME=$(id -un)
cat > "$T/greetd.toml" <<'TOML'
[terminal]
vt = 1

[default_session]
command = "/usr/lib/stained-glass/sg-login-ui"
user = "sggreet"
TOML
cp "$T/greetd.toml" "$T/greetd.orig"
S="$T/spool"; mkdir -p "$S/requests" "$S/replies"; chmod 700 "$S/requests"
export SG_ADMIN_TEST=1 SG_ADMIN_SPOOL="$S" SG_ADMIN_SYSTEM_UID="$(id -u)" \
    SG_ADMIN_GREETD_CONF="$T/greetd.toml" SG_ADMIN_AUTOLOGON_CONF="$T/autologon.conf" SG_AUTOLOGON_CONF="$T/autologon.conf"
( while :; do for f in "$S"/requests/*.req; do [ -e "$f" ] && python3 "$HERE/admin/sg-admind" 2>>"$T/admind.log"; break; done; sleep 0.2; done ) &
LOOP=$!
EXE="$HERE/build/sg-settings64.exe"
SP=""
show() {   # URI MARK: opens Settings there, waits for its dump to show MARK, prints the dump
    [ -n "$SP" ] && kill "$SP" 2>/dev/null; wineserver -k 2>/dev/null; sleep 0.5
    rm -f "$T/dump.txt"
    SG_SETTINGS_DUMP=$(winpath "$T/dump.txt") wine "$EXE" "$1" >/dev/null 2>&1 & SP=$!
    i=0; while [ $i -lt 60 ] && ! grep -q "$2" "$T/dump.txt" 2>/dev/null; do sleep 1; i=$((i + 1)); done
    sleep 1; tr -d '\r' < "$T/dump.txt" 2>/dev/null
}
at() { printf '%s\n' "$1" | grep "$2" | head -1 | sed 's/.* at=\([0-9]*\),\([0-9]*\).*/\1 \2/'; }
waitfor() { i=0; while [ $i -lt 60 ] && ! sh -c "$1" 2>/dev/null; do sleep 0.5; i=$((i + 1)); done; sh -c "$1" 2>/dev/null; }
toml_user() { python3 -c 'import sys, tomllib; print(tomllib.load(open(sys.argv[1], "rb")).get("initial_session", {}).get("user", ""))' "$T/greetd.toml"; }

out=$(show ms-settings:assignedaccess "Leaving the kiosk")
printf '%s\n' "$out" | grep -qx 'page Kiosk' && printf '%s\n' "$out" | grep -qx 'category Accounts' \
    && pass "Settings > Accounts > Kiosk (ms-settings:assignedaccess)" || fail "no Kiosk page: $(printf '%s\n' "$out" | head -5)"
printf '%s\n' "$out" | grep -q '^text .*Ctrl+Alt+Del and choose Sign out' && pass "...it says how to leave the kiosk" || fail "no escape route on the page"
printf '%s\n' "$out" | grep -q '^control ComboBox .*: Nobody (show the sign-in screen)$' \
    && pass "...nobody signs in automatically to begin with" || fail "combo: $(printf '%s\n' "$out" | grep ComboBox)"
# the account: the combo's entry for this account (Nobody, then the accounts by /etc/passwd order)
pos=$(python3 - <<PY
n = 0
for line in open("/etc/passwd"):
    f = line.rstrip("\n").split(":")
    if len(f) < 7 or not f[2].isdigit(): continue
    u = int(f[2])
    if u < 1000 or u >= 60000 or "nologin" in f[6] or "/false" in f[6]: continue
    n += 1
    if f[0] == "$ME": print(n); break
PY
)
xy=$(at "$out" 'control ComboBox')
for _try in 1 2 3; do   # the drop-down can miss a click while the page settles
    # shellcheck disable=SC2086
    xdotool mousemove $xy click 1; sleep 1.5; xdotool key Home; for _ in $(seq 1 "${pos:-1}"); do xdotool key Down; done; xdotool key Return
    waitfor "grep -qx 'user=$ME' '$T/autologon.conf' 2>/dev/null" >/dev/null && break
    xdotool key Escape; sleep 1
done
[ "$(toml_user)" = "$ME" ] && grep -qx "user=$ME" "$T/autologon.conf" \
    && pass "choosing the account (as an administrator): greetd signs $ME in when the PC starts" || fail "autologon: $(cat "$T/greetd.toml") $(tail -2 "$T/admind.log")"

# the kiosk app: a Windows program from Start
SMP="$WINEPREFIX/drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs"
wine "$T/probe.exe" --mklnk 'C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Kiosk Probe.lnk' \
    'C:\kprobe\sg-kiosk-probe64.exe' '--fullscreen "x y"' 'C:\kprobe' >/dev/null 2>&1
[ -f "$SMP/Kiosk Probe.lnk" ] || fail "(setup: no Start shortcut)"
export SG_TEST_PICK="Kiosk Probe"
out=$(show ms-settings:assignedaccess "Choose an app")
printf '%s\n' "$out" | grep -q "^text No kiosk app: $ME gets the ordinary desktop.$" && pass "...then: no kiosk app yet" || fail "kiosk text: $(printf '%s\n' "$out" | grep '^text' | tail -4)"
xy=$(at "$out" 'control Button .*: Choose an app$')
# shellcheck disable=SC2086
xdotool mousemove $xy click 1
waitfor "grep -q '^kiosk-app=' '$T/autologon.conf'" >/dev/null
grep -qxF 'kiosk-name=Kiosk Probe' "$T/autologon.conf" && grep -qxF 'kiosk-app=C:\kprobe\sg-kiosk-probe64.exe' "$T/autologon.conf" \
    && grep -qxF 'kiosk-args=--fullscreen "x y"' "$T/autologon.conf" && grep -qixF 'kiosk-dir=C:\kprobe' "$T/autologon.conf" \
    && pass "Choose an app: a Start app's program, arguments and folder kept for the kiosk" || fail "kiosk app: $(cat "$T/autologon.conf")"
out=$(show ms-settings:assignedaccess "Don't use a kiosk app")
printf '%s\n' "$out" | grep -q "^text Kiosk Probe opens full screen when $ME signs in" && pass "...and the page says so" || fail "page after: $(printf '%s\n' "$out" | grep '^text' | tail -4)"

# a Linux app (its shortcut runs sg-linuxapp64.exe --run "Z:\...\x.desktop")
printf '[Desktop Entry]\nType=Application\nName=Thing\nExec=true\n' > "$T/thing.desktop"
cp "$HERE/build/sg-linuxapp64.exe" "$T/sg-linuxapp64.exe" 2>/dev/null || : > "$T/sg-linuxapp64.exe"
wine "$T/probe.exe" --mklnk 'C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Linux apps\Thing.lnk' \
    "$(winpath "$T/sg-linuxapp64.exe")" "--run \"$(winpath "$T/thing.desktop")\"" '' >/dev/null 2>&1
export SG_TEST_PICK="Thing"
xy=$(at "$out" 'control Button .*: Choose another app$')
out=$(show ms-settings:assignedaccess "Choose another app")
xy=$(at "$out" 'control Button .*: Choose another app$')
# shellcheck disable=SC2086
xdotool mousemove $xy click 1
waitfor "grep -q '^kiosk-desktop=' '$T/autologon.conf'" >/dev/null
grep -qxF "kiosk-desktop=$T/thing.desktop" "$T/autologon.conf" && ! grep -q '^kiosk-app=' "$T/autologon.conf" \
    && pass "a Linux app from Start: its .desktop file is the kiosk app" || fail "Linux kiosk app: $(cat "$T/autologon.conf")"
out=$(show ms-settings:assignedaccess "Don't use a kiosk app")
xy=$(at "$out" "control Button .*: Don't use a kiosk app$")
# shellcheck disable=SC2086
xdotool mousemove $xy click 1
waitfor "! grep -q '^kiosk-' '$T/autologon.conf'" >/dev/null
! grep -q '^kiosk-' "$T/autologon.conf" && grep -qx "user=$ME" "$T/autologon.conf" && pass "Don't use a kiosk app: removed, the automatic sign-in stays" || fail "kiosk off: $(cat "$T/autologon.conf")"

# ---- 4. the keyring question (before the sign-in is turned off again) -------------
printf '#!/bin/sh\necho locked\n' > "$T/sg-keyring"; chmod +x "$T/sg-keyring"
export SG_KEYRING_HELPER; SG_KEYRING_HELPER=$(winpath "$T/sg-keyring")
CTL=$(winpath "$HERE/build/sg-control64.exe")
asks() {   # EXE -> "asks" if the question's window comes up
    wine "$1" /keyring-signin >/dev/null 2>&1 & _p=$!
    _i=0; _w=""
    while [ $_i -lt 20 ] && kill -0 $_p 2>/dev/null && [ -z "$_w" ]; do sleep 0.5; _w=$(xdotool search --name "Saved passwords are locked" 2>/dev/null | head -1); _i=$((_i + 1)); done
    kill $_p 2>/dev/null; wineserver -k 2>/dev/null; sleep 0.5
    [ -n "$_w" ] && echo asks || echo quiet
}
[ "$(asks "$HERE/build/sg-control64.exe")" = quiet ] && pass "the account that signs in by itself is not asked about its locked saved passwords" || fail "asked at an automatic sign-in"
# shellcheck disable=SC2086
"$MINGW" $CFLAGS -DSG_MUTANT_AUTOLOGON_KEYRING -Wno-missing-field-initializers -o "$T/mut-keyring.exe" "$HERE"/src/control/*.c \
    "$HERE/build/sg-control-res64.o" -lcomctl32 -lshell32 -lgdi32 -luser32 -ladvapi32 -lmsimg32 -liphlpapi -lws2_32 \
    -lole32 -luuid -lwindowscodecs -lcomdlg32 -lshlwapi -lwininet -lversion -lwinspool 2>"$T/mut-build.log"
[ "$(asks "$T/mut-keyring.exe")" = asks ] && pass "MUTANT AUTOLOGON_KEYRING (asked anyway) is caught" || fail "AUTOLOGON_KEYRING not detected ($(tail -1 "$T/mut-build.log"))"
printf 'user=somebody-else\n' > "$T/autologon.conf.other"
[ "$(SG_AUTOLOGON_CONF="$T/autologon.conf.other" asks "$HERE/build/sg-control64.exe")" = asks ] \
    && pass "...anyone else still is" || fail "not asked when another account signs in automatically"

# automatic sign-in off again
out=$(show ms-settings:assignedaccess "Leaving the kiosk")
xy=$(at "$out" 'control ComboBox')
for _try in 1 2 3; do
    # shellcheck disable=SC2086
    xdotool mousemove $xy click 1; sleep 1.5; xdotool key Home Return
    waitfor "[ ! -e '$T/autologon.conf' ]" >/dev/null && break
    xdotool key Escape; sleep 1
done
cmp -s "$T/greetd.toml" "$T/greetd.orig" && [ ! -e "$T/autologon.conf" ] \
    && pass "Nobody: greetd's configuration as it was, nothing kept" || fail "autologon off: $(cat "$T/greetd.toml")"

# ---- 3. Settings > Apps > Startup: Add an app, Remove ------------------------------
UPROG="$WINEPREFIX/drive_c/users/$ME/AppData/Roaming/Microsoft/Windows/Start Menu/Programs"
# the Startup folder, its name's case the prefix's (Wine: StartUp), once it exists
ustartup() { for d in "$UPROG"/[Ss]tart[Uu]p; do [ -d "$d" ] && { echo "$d"; return; }; done; echo "$UPROG/Startup"; }
startup_add() {   # EXE -> the page's dump after "Add an app"
    out=$(show ms-settings:startupapps "Add an app")
    xy=$(at "$out" 'control Button .*: Add an app$')
    # shellcheck disable=SC2086
    xdotool mousemove $xy click 1
    waitfor "ls '$UPROG'/[Ss]tart[Uu]p 2>/dev/null | grep -q 'Kiosk Probe'" >/dev/null
    USTARTUP=$(ustartup)
}
export SG_TEST_PICK="Kiosk Probe"
startup_add
[ -f "$USTARTUP/Kiosk Probe.lnk" ] && pass "Startup > Add an app: the Start app's shortcut in the person's Startup folder" || fail "not added: $(ls "$USTARTUP" 2>&1)"
cmp -s "$USTARTUP/Kiosk Probe.lnk" "$SMP/Kiosk Probe.lnk" && pass "...the same shortcut Start opens (program, arguments, folder)" || fail "a different shortcut"
out=$(show ms-settings:startupapps "Kiosk Probe")
printf '%s\n' "$out" | grep -q '^control SgSetCtl .*state=1.*: Kiosk Probe$' && pass "...listed, switched on" || fail "not listed: $(printf '%s\n' "$out" | grep -i probe)"
xy=$(at "$out" 'control SgCplLink .*: Remove$')
[ -z "$xy" ] && xy=$(at "$out" ': Remove$')
# shellcheck disable=SC2086
xdotool mousemove $xy click 1
waitfor "[ ! -e '$USTARTUP/Kiosk Probe.lnk' ]" >/dev/null
[ ! -e "$USTARTUP/Kiosk Probe.lnk" ] && pass "Remove: the shortcut is gone" || fail "Remove left it"
# MUTANT STARTUP_ADD_NOOP
# shellcheck disable=SC2086
"$MINGW" $CFLAGS -DSG_MUTANT_STARTUP_ADD_NOOP -o "$T/mut-startup.exe" "$HERE"/src/control/*.c \
    "$HERE/build/sg-settings-res64.o" -lcomctl32 -lshell32 -lgdi32 -luser32 -ladvapi32 -lmsimg32 -liphlpapi -lws2_32 \
    -lole32 -luuid -lwindowscodecs -lcomdlg32 -lshlwapi -lwininet -lversion -lwinspool 2>>"$T/mut-build.log"
EXE="$T/mut-startup.exe"
startup_add
[ ! -e "$USTARTUP/Kiosk Probe.lnk" ] && pass "MUTANT STARTUP_ADD_NOOP (nothing added) is caught" || fail "STARTUP_ADD_NOOP not detected"
[ -n "$SP" ] && kill "$SP" 2>/dev/null

[ "$RC" = 0 ] && echo "RESULT: PASS" || { echo "RESULT: FAIL"; tail -5 "$T/admind.log" 2>/dev/null; }
exit "$RC"
