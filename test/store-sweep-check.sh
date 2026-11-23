#!/bin/bash
. "$(dirname "$0")/scratch-home.sh"
# SG Store: the cleanup after an elevated uninstall happens inside the elevated
# step (David 2026-11-23, option a). Claws Mail's uninstaller (run as an
# administrator) removes Program Files\Claws Mail and leaves Claws-Mail.lnk in
# the all-users Start menu, as on Windows; the store's sweep of dead shortcuts
# ran afterwards as the person, who cannot delete in ProgramData, and the
# shortcut stayed.
#
# A standard user (SG_OTHER, default sgconf, in SG_GROUP) of a shared prefix
# uninstalls an app whose uninstaller needs an administrator (its manifest
# says so); the broker's client is a stand-in (SG_ELEVATE) that consents and
# runs the command, and notes whether the all-users shortcut is still there
# when the elevated step has ended:
#   - one consent, and what it runs is the store's --elevated-uninstall-run
#     with the uninstaller and its arguments;
#   - when the elevated step ends, the dead shortcut in the all-users Start
#     menu is gone, and a live one (another program's) is kept;
#   - the app is uninstalled;
#   - mutants SG_MUTANT_SWEEP_UNELEVATED (the old way: the uninstaller alone
#     elevated) and SG_MUTANT_ELEVATED_NOSWEEP (no sweep in the elevated step)
#     leave the shortcut after the elevated step.
# Needs wine-sg (0259, 8400), mingw, and passwordless `sudo -u $SG_OTHER`.
#
#   SG_WINE=<wine> test/store-sweep-check.sh
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE="${SG_WINE:-${SG_WINE_DIR:-/opt/wine-sg}/bin/wine}"
WINESERVER="${SG_WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
SG_OTHER=${SG_OTHER:-sgconf}
SG_GROUP=${SG_GROUP:-sgconfgrp}
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
WINDRES=${WINDRES:-x86_64-w64-mingw32-windres}
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for need in "$MINGW" "$WINDRES"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE" ] || { echo "SKIP: no wine at $WINE"; exit 77; }
id "$SG_OTHER" >/dev/null 2>&1 && sudo -n -u "$SG_OTHER" true 2>/dev/null || { echo "SKIP: no $SG_OTHER/sudo"; exit 77; }

W=$(mktemp -d /var/tmp/sg-store-sweep.XXXXXX); chmod 755 "$W"
PFX=$W/prefix
cleanup() {
    set +e
    WINEPREFIX=$PFX "$WINESERVER" -k 2>/dev/null
    sleep 1
    sudo -n rm -rf "$W"
}
trap cleanup EXIT INT TERM

STORE_SRC="$HERE/src/store/main.c $HERE/src/store/details.c $HERE/src/store/catalog.c $HERE/src/store/sysinstall.c $HERE/src/store/icons.c $HERE/src/store/msix.c $HERE/src/browser/fetch.c $HERE/src/browser/manifest.c $HERE/src/zip/zipcore.c"
STORE_LIBS="-lruntimeobject -lsetupapi -lwininet -lbcrypt -lshlwapi -lshell32 -lgdi32 -luser32 -ladvapi32 -lole32 -luuid -lwindowscodecs -lmsimg32 -lcomdlg32"
build() { # outfile [define]
    "$MINGW" -municode -mwindows -O1 -Wno-missing-field-initializers -I"$HERE/src/browser" -I"$HERE/src/store" -I"$HERE/src/zip" \
        ${2:+"-D$2"} -o "$1" $STORE_SRC $STORE_LIBS 2>>"$W/cc.log"
}
build "$W/store.exe" || { fail "the store does not build: $(tail -3 "$W/cc.log")"; exit 1; }
build "$W/mut-unelevated.exe" SG_MUTANT_SWEEP_UNELEVATED || { fail "mutant SWEEP_UNELEVATED does not build"; exit 1; }
build "$W/mut-nosweep.exe" SG_MUTANT_ELEVATED_NOSWEEP || { fail "mutant ELEVATED_NOSWEEP does not build"; exit 1; }

# the uninstaller: needs an administrator (its manifest); takes the program and its entry away
cat > "$W/uninst.manifest" <<'EOM'
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
<trustInfo xmlns="urn:schemas-microsoft-com:asm.v2"><security><requestedPrivileges>
<requestedExecutionLevel level="requireAdministrator" uiAccess="false"/>
</requestedPrivileges></security></trustInfo></assembly>
EOM
printf '1 24 "%s"\n' "$W/uninst.manifest" > "$W/uninst.rc"
cat > "$W/uninst.c" <<'EOC'
#include <windows.h>
int wmain(void)
{
    DeleteFileW(L"C:\\Program Files\\Gate App\\gateapp.exe");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\GateApp");
    return 0;
}
EOC
# a shortcut maker: mklnk.exe LINK TARGET
cat > "$W/mklnk.c" <<'EOC'
#define COBJMACROS
#include <windows.h>
#include <shlobj.h>
int wmain(int argc, WCHAR **argv)
{
    IShellLinkW *link; IPersistFile *file; HRESULT hr;
    if (argc < 3) return 2;
    CoInitialize(NULL);
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) return 3;
    IShellLinkW_SetPath(link, argv[2]);
    if (FAILED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&file))) return 4;
    hr = IPersistFile_Save(file, argv[1], TRUE);
    return SUCCEEDED(hr) ? 0 : 5;
}
EOC
"$WINDRES" "$W/uninst.rc" -O coff -o "$W/uninst.res" && "$MINGW" -municode -O2 -o "$W/uninst.exe" "$W/uninst.c" "$W/uninst.res" -ladvapi32 \
    && "$MINGW" -municode -O2 -o "$W/mklnk.exe" "$W/mklnk.c" -lole32 -luuid \
    || { fail "the stand-in uninstaller does not build"; exit 1; }
printf 'int main(void) { return 0; }\n' > "$W/prog.c" && "$MINGW" -O2 -o "$W/gateapp.exe" "$W/prog.c" || exit 1

# the broker's client, consenting: the answer "0", the command run elevated (SG_IN_BROKER, as the broker's
# programs are), and whether the all-users shortcut was still there when it ended
mkdir -m 777 "$W/out"
LNK="$PFX/drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs/Gate App.lnk"
LIVE="$PFX/drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs/Other App.lnk"
cat > "$W/elevate" <<EOS
#!/bin/sh
ready=""
code=""
while :; do
    case "\$1" in
    --ready) ready=\$2; shift 2 ;;
    --code) code=\$2; shift 2 ;;
    --wine) shift; break ;;
    *) break ;;
    esac
done
printf 'run %s\n' "\$*" >> "$W/out/elevate.log"
[ -n "\$ready" ] && printf 0 > "\$ready"
SG_IN_BROKER=1 "$WINE" "\$@"
rc=\$?
if [ -e "$LNK" ]; then echo "after dead-present" >> "$W/out/elevate.log"; else echo "after dead-gone" >> "$W/out/elevate.log"; fi
if [ -e "$LIVE" ]; then echo "after live-kept" >> "$W/out/elevate.log"; else echo "after live-gone" >> "$W/out/elevate.log"; fi
[ -n "\$code" ] && printf '%s' "\$rc" > "\$code"
exit \$rc
EOS
printf 'ready\ncode\n' > "$W/elevate.features"
chmod 755 "$W/elevate" "$W"/*.exe

mkdir "$PFX"; chgrp "$SG_GROUP" "$PFX"; chmod 2770 "$PFX"; touch "$PFX/.sg-system-prefix"
export WINEPREFIX=$PFX WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
unset DISPLAY
sg "$SG_GROUP" -c "umask 002; '$WINESERVER' -p"
sg "$SG_GROUP" -c "umask 002; '$WINE' wineboot -i" >/dev/null 2>&1
K='HKLM\Software\Stained Glass\Store\Apps'
U='HKLM\Software\Microsoft\Windows\CurrentVersion\Uninstall\GateApp'
reg() { sg "$SG_GROUP" -c "umask 002; '$WINE' reg add '$1' /v '$2' /d '$3' /f" >/dev/null 2>&1; }
D="$PFX/drive_c/Program Files/Gate App"
set_up() {
    mkdir -p "$D" "$(dirname "$LNK")"
    cp "$W/uninst.exe" "$W/gateapp.exe" "$D/"
    mkdir -p "$PFX/drive_c/Program Files/Other App"; cp "$W/gateapp.exe" "$PFX/drive_c/Program Files/Other App/other.exe"
    reg "$K\\01" Name 'Gate App'; reg "$K\\01" Tier windows; reg "$K\\01" Category Utilities
    reg "$K\\01" Source "pin:http://127.0.0.1:9/setup.exe|0000|exe|/S"
    reg "$K\\01" PinVersion 1.0; reg "$K\\01" DetectName 'Gate App'
    reg "$U" DisplayName 'Gate App'; reg "$U" DisplayVersion 1.0
    reg "$U" UninstallString '"C:\Program Files\Gate App\uninst.exe" /S'
    reg "$U" InstallLocation 'C:\Program Files\Gate App'
    sg "$SG_GROUP" -c "umask 002; '$WINE' '$W/mklnk.exe' 'C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Gate App.lnk' 'C:\Program Files\Gate App\gateapp.exe'" >/dev/null 2>&1
    sg "$SG_GROUP" -c "umask 002; '$WINE' '$W/mklnk.exe' 'C:\ProgramData\Microsoft\Windows\Start Menu\Programs\Other App.lnk' 'C:\Program Files\Other App\other.exe'" >/dev/null 2>&1
    chmod -R g+rwX "$PFX" 2>/dev/null
    rm -f "$W/out/elevate.log"
}
mkdir -m 777 "$W/home"
sudo -n -u "$SG_OTHER" mkdir -p "$PFX/drive_c/users/$SG_OTHER/Desktop" "$PFX/drive_c/users/$SG_OTHER/AppData/Local/Temp"

uninstall_as_other() { # exe -> the result line
    sudo -n rm -f "$PFX/drive_c/users/Public/dump.result"
    sudo -n -u "$SG_OTHER" env WINEPREFIX="$PFX" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" \
        HOME="$W/home" SG_ELEVATE="$W/elevate" SG_ELEVATE_FEATURES="$W/elevate.features" SG_STORE_DUMP='C:\users\Public\dump' \
        SG_STORE_UNINSTALL_GRACE=3000 SG_STORE_UNINSTALL_GONE_WAIT=3000 \
        timeout 180 "$WINE" "$1" --uninstall 01 >/dev/null 2>&1
    tr -d '\r' < "$PFX/drive_c/users/Public/dump.result" 2>/dev/null | grep '^uninstall 01'
}

set_up
[ -e "$LNK" ] && [ -e "$LIVE" ] || { fail "the gate's shortcuts were not made"; exit 1; }
out=$(uninstall_as_other "$W/store.exe")
echo "      $out"
LOG=$(cat "$W/out/elevate.log" 2>/dev/null)
[ "$(grep -c '^run ' <<<"$LOG")" = 1 ] && pass "one consent" || fail "consents: $(grep -c '^run ' <<<"$LOG"): $LOG"
grep '^run ' <<<"$LOG" | grep -q -- '--elevated-uninstall-run' && grep '^run ' <<<"$LOG" | grep -qi 'Gate App\\uninst.exe' \
    && pass "the elevated step is the store's, with the uninstaller: $(grep '^run ' <<<"$LOG" | head -1 | cut -c1-160)" \
    || fail "the elevated step: $LOG"
grep -q '^after dead-gone' <<<"$LOG" && pass "the dead all-users shortcut is gone when the elevated step ends" \
    || fail "the dead shortcut after the elevated step: $LOG"
grep -q '^after live-kept' <<<"$LOG" && pass "another program's shortcut is kept" || fail "the live shortcut: $LOG"
case "$out" in "uninstall 01 ok "*) pass "uninstalled" ;; *) fail "the uninstall: '$out'" ;; esac
for m in unelevated nosweep; do
    set_up
    out=$(uninstall_as_other "$W/mut-$m.exe")
    LOG=$(cat "$W/out/elevate.log" 2>/dev/null)
    grep -q '^after dead-present' <<<"$LOG" && pass "MUTANT $m: the shortcut stays through the elevated step (gate catches it)" \
        || fail "MUTANT $m not detected: $LOG"
done
echo
if [ "$RC" = 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
