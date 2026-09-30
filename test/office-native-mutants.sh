#!/bin/sh
# Mutation test for test/office-native-check.sh: three broken builds of SG
# Office's programs, each of which the gate must fail --
#   nonative   never starts SG Office's own editors
#   dospath    hands the editors the Windows path, not the Unix one
#   nopolicy   ignores Editors = "LibreOffice"
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
MINGW64=${MINGW64:-x86_64-w64-mingw32-gcc}
command -v "$MINGW64" >/dev/null || { echo "SKIP: $MINGW64 missing"; exit 77; }
[ -f "$HERE/build/office/documents-res64.o" ] || { echo "SKIP: make office first"; exit 77; }
T=$(mktemp -d /var/tmp/sg-office-mutants.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM
RC=0
for m in nonative dospath nopolicy; do
    case $m in
        nonative) from='if (!print \&\& native_program(' to='if (0 \&\& native_program(' ;;
        dospath)  from='!(u = unix_path(argv\[i\]))' to='!(u = NULL)' ;;
        nopolicy) from='!_wcsicmp(choice, L"LibreOffice")' to='0' ;;
    esac
    sed "s/$from/$to/" "$HERE/office/launcher/launcher.c" > "$T/$m.c"
    cmp -s "$T/$m.c" "$HERE/office/launcher/launcher.c" && { echo "FAIL  mutant $m changed nothing"; RC=1; continue; }
    mkdir -p "$T/$m"
    for k in documents:writer spreadsheets:calc; do
        n=${k%%:*}; lo=${k##*:}
        "$MINGW64" -O2 -municode -mwindows -Wno-missing-field-initializers \
            -DSG_KIND="L\"--$lo\"" -DSG_NATIVE_KIND="L\"$n\"" -DSG_TITLE="L\"SG Office\"" \
            -o "$T/$m/sg-${n}64.exe" "$T/$m.c" "$HERE/build/office/$n-res64.o" -lshell32 -ladvapi32 -luser32 || exit 1
    done
    if SG_OFFICE_LAUNCHER_DIR="$T/$m" sh "$HERE/test/office-native-check.sh" > "$T/$m.log" 2>&1; then
        echo "FAIL  the gate passed mutant $m"; RC=1
    else
        echo "PASS  the gate fails mutant $m ($(grep -c '^FAIL' "$T/$m.log") checks)"
    fi
done
exit $RC
