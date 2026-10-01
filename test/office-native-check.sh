#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# SG Office's programs with SG Office's own editors (the native program,
# package sg-office-editors), under Wine with a fake native program that
# records what it is given:
#
#   - sg-documents64.exe / sg-spreadsheets64.exe with no file start the
#     editors on a new document of their kind (--new documents|spreadsheets)
#   - a file given as a Windows path (C:\..., with a space in its name)
#     reaches the editors as its Unix path, as one argument
#   - /p (a Print verb an older registration left) opens the file: the
#     editors print from their own menu
#
# Needs wine-sg; skips (77) without it. SG_OFFICE_LAUNCHER_DIR runs another
# build of the programs (test/office-native-mutants.sh).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
L="${SG_OFFICE_LAUNCHER_DIR:-$HERE/build}"
RC=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; RC=1; }

[ -x "$WINE_DIR/bin/wine" ] || { echo "SKIP: wine-sg missing"; exit 77; }
for k in documents spreadsheets; do
    [ -f "$L/sg-${k}64.exe" ] || { echo "SKIP: $L/sg-${k}64.exe missing (make office)"; exit 77; }
done

T=$(mktemp -d /var/tmp/sg-office-native.XXXXXX); chmod 755 "$T"
export WINEPREFIX="$T/pfx" WINEDEBUG=-all
export WINEDLLOVERRIDES="$WINEDLLOVERRIDES;mscoree,mshtml="
unset DISPLAY WAYLAND_DISPLAY
# shellcheck disable=SC2317
cleanup() {
    "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ "${KEEP:-0}" = 1 ] && echo "kept $T" || rm -rf "$T"
}
trap cleanup EXIT INT TERM

# the programs alone
mkdir -p "$T/bin"
cp "$L/sg-documents64.exe" "$L/sg-spreadsheets64.exe" "$T/bin/"
# the fake editors: one line per argument, a blank line after each start
cat > "$T/fake-sg-office" <<EOF
#!/bin/sh
{ printf '%s\n' "\$@"; echo; } >> "$T/calls.txt"
EOF
chmod 755 "$T/fake-sg-office"
export SG_OFFICE_NATIVE="Z:$(printf '%s' "$T/fake-sg-office" | tr / '\\')"

run() {   # PROGRAM ARGS...: run it, wait for its start to be recorded
    rm -f "$T/calls.txt"
    _prog=$1; shift
    timeout 120 "$WINE_DIR/bin/wine" "$T/bin/$_prog" "$@" >/dev/null 2>&1 || true
    for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$T/calls.txt" ] && break; sleep 0.5; done
}
calls() { cat "$T/calls.txt" 2>/dev/null; }
# the one argument the editors got, resolved (Wine names C: through the
# prefix's dosdevices/c: link), and how many arguments there were
one_file() {
    [ "$(sed '/^$/d' "$T/calls.txt" 2>/dev/null | wc -l)" = 1 ] || return 1
    readlink -f "$(head -1 "$T/calls.txt")"
}

run sg-documents64.exe
[ "$(calls)" = "$(printf -- '--new\ndocuments\n')" ] \
    && pass "SG Office Documents with no file: a new document in the editors" \
    || fail "SG Office Documents with no file started: [$(calls | tr '\n' '|')]"
run sg-spreadsheets64.exe
[ "$(calls)" = "$(printf -- '--new\nspreadsheets\n')" ] \
    && pass "SG Office Spreadsheets with no file: a new workbook in the editors" \
    || fail "SG Office Spreadsheets with no file started: [$(calls | tr '\n' '|')]"

DOC="$T/pfx/drive_c/users/Public/Documents/Q3 report.docx"
mkdir -p "$(dirname "$DOC")" && : > "$DOC"
run sg-documents64.exe 'C:\users\Public\Documents\Q3 report.docx'
[ "$(one_file)" = "$(readlink -f "$DOC")" ] \
    && pass "a file opened from C: reaches the editors as its Unix path, one argument" \
    || fail "C:\\users\\Public\\Documents\\Q3 report.docx reached the editors as: [$(calls | tr '\n' '|')]"

run sg-documents64.exe /p 'C:\users\Public\Documents\Q3 report.docx'
[ "$(one_file)" = "$(readlink -f "$DOC")" ] && pass "/p opens the file in the editors (they print from their own menu)" \
    || fail "/p reached the editors as: [$(calls | tr '\n' '|')]"

exit $RC
