#!/bin/sh
# Mutation test for test/office-check.sh: copies of this source, each broken
# the way SG Office was or could be, which the gate must fail --
#   setup      SG Store's SG Office runs a setup program again (ours:setup)
#   noeditors  package sg-office no longer brings sg-office-editors
#   soffice    a launcher that starts LibreOffice's soffice.exe
#   noregister sg-admind installs SG Office without registering it
#   norun      SG Store's entry has nothing for Open to start
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
for k in documents spreadsheets presentations; do
    [ -f "$HERE/build/sg-${k}64.exe" ] || { echo "SKIP: make office first"; exit 77; }
done
T=$(mktemp -d /var/tmp/sg-office-check-mutants.XXXXXX)
trap 'rm -rf "$T"' EXIT INT TERM
RC=0
for m in setup noeditors soffice noregister norun; do
    C="$T/$m"; mkdir -p "$C/src/control" "$C/build" "$C/test"
    cp -r "$HERE/office" "$HERE/defaults" "$HERE/debian" "$HERE/admin" "$C/"
    cp -r "$HERE/src/store" "$C/src/"; cp "$HERE/src/sg-start.c" "$C/src/"; cp "$HERE/src/control/set_apps.c" "$C/src/control/"
    cp "$HERE"/build/sg-documents64.exe "$HERE"/build/sg-spreadsheets64.exe "$HERE"/build/sg-presentations64.exe "$C/build/"
    cp "$HERE/test/office-check.sh" "$C/test/"
    case $m in
        setup)      f="$C/defaults/85-sg-store.reg"; sed -i 's/^"Source"="ours:apt:sg-office"$/"Source"="ours:setup:sg-office-setup.exe"/' "$f" ;;
        noeditors)  f="$C/debian/control"; sed -i '/^Package: sg-office$/,/^$/s/, sg-office-editors$//' "$f" ;;
        soffice)    f="$C/office/launcher/launcher.c"; sed -i 's|L"Z:\\\\usr\\\\bin\\\\sg-office"|L"C:\\\\Program Files\\\\LibreOffice\\\\program\\\\soffice.exe"|' "$f" ;;
        noregister) f="$C/admin/sg-admind"; sed -i 's/^    if pkg in OUR_PACKAGES:$/    if False:/' "$f" ;;
        norun)      f="$C/defaults/85-sg-store.reg"; sed -i '/^"Run"="sg-documents.exe"$/d' "$f" ;;
    esac
    cmp -s "$f" "$HERE/${f#"$C"/}" && { echo "FAIL  mutant $m changed nothing"; RC=1; continue; }
    if sh "$C/test/office-check.sh" > "$T/$m.log" 2>&1; then
        echo "FAIL  the gate passed mutant $m"; RC=1
    else
        echo "PASS  the gate fails mutant $m ($(grep -c '^FAIL' "$T/$m.log") checks)"
    fi
done
exit $RC
