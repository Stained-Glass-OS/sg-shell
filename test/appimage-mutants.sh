#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# The mutants of test/appimage-check.sh: each must make it FAIL.
#   sg-appimage (environment): KEEP_EXEC, NO_REPLACE, KEEP_FILE,
#     NO_EXTRACT_FALLBACK, ANY_MACHINE
#   C (built here with -DSG_MUTANT_...): NO_APPIMAGE_UNINSTALL
#     (sg-linuxapp64.exe), AI_NO_SYNC (sg-store64.exe)
# Needs what the gate needs, and mingw (make build first: the store's
# resources). SG_WINE_DIR as for the gate.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0
T=$(mktemp -d /var/tmp/sg-appimage-mutants.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM
run() {   # NAME, then the environment for the gate
    name=$1; shift
    env -u SG_GATE_HOME "$@" sh "$HERE/test/appimage-check.sh" > "$T/$name.log" 2>&1
    if grep -q '^RESULT: FAIL' "$T/$name.log"; then echo "PASS  MUTANT $name caught: $(grep -m1 '^FAIL' "$T/$name.log" | cut -c7-90)"
    else echo "FAIL  MUTANT $name not caught ($(tail -1 "$T/$name.log"))"; RC=1; fi
}
for m in KEEP_EXEC NO_REPLACE KEEP_FILE NO_EXTRACT_FALLBACK ANY_MACHINE; do run "$m" "SG_MUTANT_$m=1"; done
command -v "$MINGW" >/dev/null && [ -f "$HERE/build/sg-store-res64.o" ] || { echo "SKIP  the C mutants (mingw, or make build first)"; exit "$RC"; }
CF="-O2 -municode -mwindows -Wall -Wextra"
cd "$HERE" || exit 1
"$MINGW" $CF -DSG_MUTANT_NO_APPIMAGE_UNINSTALL -o "$T/sg-linuxapp64.exe" src/linuxapps/sg-linuxapp.c -lshell32 -lole32 -luuid
run NO_APPIMAGE_UNINSTALL SG_LINUXAPP_EXE="$T/sg-linuxapp64.exe"
"$MINGW" $CF -DSG_MUTANT_AI_NO_SYNC -Wno-missing-field-initializers -Isrc/browser -Isrc/store -Isrc/zip -o "$T/sg-store64.exe" \
    src/store/main.c src/store/details.c src/store/catalog.c src/store/sysinstall.c src/store/icons.c src/browser/fetch.c \
    src/browser/manifest.c src/zip/zipcore.c build/sg-store-res64.o -lsetupapi -lwininet -lbcrypt -lshlwapi -lshell32 \
    -lgdi32 -luser32 -ladvapi32 -lole32 -luuid -lwindowscodecs -lmsimg32 -lcomdlg32
run AI_NO_SYNC SG_STORE_EXE="$T/sg-store64.exe"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
