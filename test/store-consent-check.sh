#!/bin/bash
. "$(dirname "$0")/scratch-home.sh"
# SG Store when the administrator's consent is not given (the regression
# walk, 2026-10-05: an unanswered consent prompt for the Visual Basic 6
# runtime ended as "msvbvm60.dll could not be put in place (code 1)").
#
# wine-sg 0981 makes ShellExecuteEx fail with ERROR_CANCELLED when the
# consent prompt says no or goes unanswered, as Windows' does; the store
# then says so. A standard user (SG_OTHER, default sgconf, in SG_GROUP) of a
# shared prefix installs a pinned app whose installer needs an administrator;
# the elevation broker's client is a stand-in (SG_ELEVATE) that declines.
#   - the result names the missing consent, not an error code;
#   - mutant SG_MUTANT_NOCONSENTMSG (built here) says "could not be started".
# Needs wine-sg with 0981, mingw, python3 and passwordless `sudo -u $SG_OTHER`.
#
#   SG_WINE=<wine> test/store-consent-check.sh
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE="${SG_WINE:-/opt/wine-sg/bin/wine}"
WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
SG_OTHER=${SG_OTHER:-sgconf}
SG_GROUP=${SG_GROUP:-sgconfgrp}
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
RC=0; HP=
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for need in python3 "$MINGW" "$WINDRES"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE" ] || { echo "SKIP: no wine at $WINE"; exit 77; }
id "$SG_OTHER" >/dev/null 2>&1 && sudo -n -u "$SG_OTHER" true 2>/dev/null || { echo "SKIP: no $SG_OTHER/sudo"; exit 77; }

W=$(mktemp -d /var/tmp/sg-store-consent.XXXXXX); chmod 755 "$W"
PFX=$W/prefix
cleanup() {
    set +e
    WINEPREFIX=$PFX "$WINESERVER" -k 2>/dev/null
    [ -n "$HP" ] && kill "$HP" 2>/dev/null
    sleep 1
    sudo -n rm -rf "$W"
}
trap cleanup EXIT INT TERM

STORE_SRC="$HERE/src/store/main.c $HERE/src/store/details.c $HERE/src/store/catalog.c $HERE/src/store/sysinstall.c $HERE/src/store/icons.c $HERE/src/browser/fetch.c $HERE/src/browser/manifest.c $HERE/src/zip/zipcore.c"
STORE_LIBS="-lsetupapi -lwininet -lbcrypt -lshlwapi -lshell32 -lgdi32 -luser32 -ladvapi32 -lole32 -luuid -lwindowscodecs -lmsimg32 -lcomdlg32"
build() { # outfile [define]
    "$MINGW" -municode -mwindows -O1 -Wno-missing-field-initializers -I"$HERE/src/browser" -I"$HERE/src/store" -I"$HERE/src/zip" \
        ${2:+"-D$2"} -o "$1" $STORE_SRC $STORE_LIBS 2>>"$W/cc.log"
}
build "$W/store.exe" || { fail "the store does not build: $(tail -3 "$W/cc.log")"; exit 1; }
build "$W/mut-noconsentmsg.exe" SG_MUTANT_NOCONSENTMSG || { fail "mutant NOCONSENTMSG does not build"; exit 1; }
# an installer that needs an administrator (its manifest says so)
mkdir -p "$W/www"
cat > "$W/inst.manifest" <<'EOM'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
<trustInfo xmlns="urn:schemas-microsoft-com:asm.v2"><security><requestedPrivileges>
<requestedExecutionLevel level="requireAdministrator" uiAccess="false"/>
</requestedPrivileges></security></trustInfo></assembly>
EOM
printf '1 24 "%s"\n' "$W/inst.manifest" > "$W/inst.rc"
printf 'int main(void) { return 0; }\n' > "$W/inst.c"
"$WINDRES" "$W/inst.rc" -O coff -o "$W/inst.res" && "$MINGW" -O2 -o "$W/www/setup.exe" "$W/inst.c" "$W/inst.res" \
    || { fail "the stand-in installer does not build"; exit 1; }
SHA=$(sha256sum "$W/www/setup.exe" | cut -d' ' -f1)
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
(cd "$W/www" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 > "$W/http.log" 2>&1) & HP=$!
# the broker's client, declining: the answer "1", and its own exit 1
mkdir -m 777 "$W/out"
cat > "$W/elevate" <<EOS
#!/bin/sh
ready=""
if [ "\$1" = --ready ]; then ready=\$2; shift 2; fi
printf '%s\n' "\$*" >> "$W/out/elevate.log"
sleep 1
[ -n "\$ready" ] && printf 1 > "\$ready"
exit 1
EOS
printf 'ready\n' > "$W/elevate.features"
chmod 755 "$W/elevate" "$W"/*.exe

mkdir "$PFX"; chgrp "$SG_GROUP" "$PFX"; chmod 2770 "$PFX"; touch "$PFX/.sg-system-prefix"
export WINEPREFIX=$PFX WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
unset DISPLAY
sg "$SG_GROUP" -c "umask 002; '$WINESERVER' -p"
sg "$SG_GROUP" -c "umask 002; '$WINE' wineboot -i" >/dev/null 2>&1
K='HKLM\Software\Stained Glass\Store\Apps'
reg() { sg "$SG_GROUP" -c "umask 002; '$WINE' reg add '$1' /v '$2' /d '$3' /f" >/dev/null 2>&1; }
reg "$K\\01" Name 'Admin App'; reg "$K\\01" Tier windows; reg "$K\\01" Category Utilities
reg "$K\\01" Source "pin:http://127.0.0.1:$PORT/setup.exe|$SHA|exe|/S"
reg "$K\\01" PinVersion 1.0; reg "$K\\01" DetectName 'Admin App'
chmod -R g+rwX "$PFX" 2>/dev/null
mkdir -m 777 "$W/home"
sudo -n -u "$SG_OTHER" mkdir -p "$PFX/drive_c/users/$SG_OTHER/Desktop" "$PFX/drive_c/users/$SG_OTHER/AppData/Local/Temp"

install_as_other() { # exe -> the result line
    sudo -n -u "$SG_OTHER" env WINEPREFIX="$PFX" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" \
        HOME="$W/home" SG_ELEVATE="$W/elevate" SG_ELEVATE_FEATURES="$W/elevate.features" SG_STORE_DUMP='C:\users\Public\dump' \
        timeout 120 "$WINE" "$1" --install 01 >/dev/null 2>&1
    tr -d '\r' < "$PFX/drive_c/users/Public/dump.result" 2>/dev/null | grep '^result 01'
}
out=$(install_as_other "$W/store.exe")
echo "      $out"
grep -q -- '--wine' "$W/out/elevate.log" 2>/dev/null && pass "the installer went to the elevation broker (stand-in declining)" \
    || fail "the broker was not asked: $(cat "$W/out/elevate.log" 2>/dev/null)"
case "$out" in "result 01 fail "*"consent was not given"*) pass "the store says the administrator's consent was not given" ;;
    *) fail "after the consent was refused: '$out'" ;; esac
case "$out" in *"code 1"*|*"error 1223"*) fail "it shows a code instead" ;; *) pass "no bare code in the message" ;; esac
sudo -n rm -f "$PFX/drive_c/users/Public/dump.result"
out=$(install_as_other "$W/mut-noconsentmsg.exe")
case "$out" in *"consent was not given"*) fail "NOCONSENTMSG not detected: '$out'" ;;
    *) pass "MUTANT NOCONSENTMSG says something else (gate catches it): ${out#result 01 fail }" ;; esac
echo
if [ "$RC" = 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
