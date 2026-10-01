#!/bin/sh
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# SG Office -- the shell's side of the suite: the registrations (and that the
# file-type defaults file never changes once shipped), the three programs and
# their resources, Start and Default apps, the package (it brings SG Office's
# editors, package sg-office-editors), SG Store's entry (installed with apt as
# our own package, opened by its program), no LibreOffice left, and no
# Microsoft product names as ours. The programs starting the editors under
# Wine are test/office-native-check.sh.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
B="$HERE/build"
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
PY=/usr/bin/python3
[ -x "$PY" ] || PY=python3

# 3. registrations: generated as committed; the types file frozen
T=$(mktemp -d "${TMPDIR:-/var/tmp}/office-check.XXXXXX")
"$PY" "$HERE/office/gen-reg.py" "$T" && cmp -s "$T/89-sg-office.reg" "$HERE/office/defaults/89-sg-office.reg" \
    && cmp -s "$T/89-sg-office-types.reg" "$HERE/office/defaults/89-sg-office-types.reg" \
    && pass "the .reg files are what office/gen-reg.py writes" || fail "office/defaults/*.reg differ from office/gen-reg.py's output"
# Re-importing a changed types file would take .docx & co. back from a suite
# the user installed later (sg-prefix-init imports a defaults file again when it
# changes): its contents are fixed forever.
TYPES_SHA=10a39a4a92131c7b2d22c03782520a45b889e7b65bf21c5fad18d734971f507c
[ "$(sha256sum < "$HERE/office/defaults/89-sg-office-types.reg" | cut -d' ' -f1)" = "$TYPES_SHA" ] \
    && pass "89-sg-office-types.reg is the file first shipped (never re-imported over another suite)" \
    || fail "89-sg-office-types.reg changed: it must never change once shipped (see office/gen-reg.py)"
R="$HERE/office/defaults/89-sg-office.reg"
for e in .docx .doc .xlsx .xls .pptx .ppt .odt .ods .odp .csv; do
    grep -q "^\\[HKEY_LOCAL_MACHINE\\\\Software\\\\Classes\\\\$e\\\\OpenWithProgids\\]" "$R" || fail "$e is not in Open with"
    grep -q "^\\[HKEY_LOCAL_MACHINE\\\\Software\\\\Classes\\\\$e\\]" "$HERE/office/defaults/89-sg-office-types.reg" || fail "$e has no default program"
done
pass "Office's document, spreadsheet and presentation types open with SG Office and stay in Open with"
for e in .docx .xlsx .pptx; do grep -q "\\\\$e\\\\SGOffice\\.[A-Za-z.0-9]*\\\\ShellNew\\]" "$R" || fail "no New > $e"; done
pass "File Explorer's New menu makes .docx, .xlsx and .pptx"
grep -o 'sg-[a-z]*64\.exe' "$R" | sort -u | while read -r exe; do
    grep -q "build/$exe" "$HERE/debian/rules" || echo "FAIL  $exe is registered but not packaged"
done | grep FAIL && RC=1 || pass "every registered program is packaged"
grep -q '"SG Office"="Software\\\\Stained Glass\\\\SG Office\\\\Capabilities"' "$R" && pass "SG Office is a registered application (Default apps)" || fail "no RegisteredApplications entry"
# SG Store's SG Office: our own apt package (sg-office, which brings the
# editors), installed through sg-admind -- which registers it at once -- and
# opened by a program this source registers and packages. No setup program.
"$PY" - "$HERE" <<'PY' && pass "SG Store installs SG Office as our apt package (sg-office), registered as it installs, opened by sg-documents.exe" || fail "SG Store's SG Office entry"
import glob, os, re, sys
here = sys.argv[1]
store = open(os.path.join(here, "defaults/85-sg-store.reg"), encoding="utf-8").read()
regs = "".join(open(f, encoding="utf-8").read() for f in glob.glob(os.path.join(here, "defaults/*.reg")) + glob.glob(os.path.join(here, "office/defaults/*.reg")))
rules = open(os.path.join(here, "debian/rules"), encoding="utf-8").read()
admind = open(os.path.join(here, "admin/sg-admind"), encoding="utf-8").read()
bad = 0
m = re.search(r'"Name"="SG Office"\n(?:"[^"]+"=[^\n]*\n)*', store)
entry = m.group(0) if m else ""
if '"Source"="ours:apt:sg-office"' not in entry:
    print("  the entry's Source is not ours:apt:sg-office"); bad += 1
if "ours:setup" in store or '"Package"=' in entry or "LibreOffice" in entry:
    print("  a setup-program entry is left"); bad += 1
if not re.search(r'OUR_PACKAGES = \([^)]*"sg-office"', admind) or "if pkg in OUR_PACKAGES:" not in admind:
    print("  sg-admind does not register sg-office as it installs"); bad += 1
run = re.search(r'"Run"="([^"]+)"', entry)
if not run:
    print("  no Run (what Open starts)"); bad += 1
else:
    a = re.search(r'\[HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\%s\]\n@="([^"]+)"' % re.escape(run.group(1)), regs, re.I)
    if not a or ("build/" + a.group(1).replace("\\\\", "\\").split("\\")[-1]) not in rules:
        print("  Run %s is not a registered, packaged program" % run.group(1)); bad += 1
sys.exit(1 if bad else 0)
PY
# the package brings the editors
sed -n '/^Package: sg-office$/,/^$/p' "$HERE/debian/control" | grep -q '^Depends:.*sg-office-editors' \
    && pass "package sg-office depends on sg-office-editors (the editors come with it)" || fail "sg-office does not depend on sg-office-editors"
# no LibreOffice path left: no download, no soffice.exe, no LibreOffice policy
if grep -rIl 'LibreOffice\|soffice\|sg-office-setup' "$HERE/office" "$HERE/src/store" "$HERE/debian/control" "$HERE/debian/rules" 2>/dev/null; then
    fail "LibreOffice is still named in SG Office's sources (above)"
else pass "no LibreOffice left: SG Office is our own suite"; fi

# 4. the programs: built, with the program's and its files' icons, named as ours
for k in documents spreadsheets presentations; do
    E="$B/sg-${k}64.exe"
    [ -f "$E" ] || { fail "sg-${k}64.exe was not built"; continue; }
    if command -v wrestool >/dev/null; then
        [ "$(wrestool -l -t 14 "$E" | grep -c group_icon)" = 2 ] && pass "sg-$k: the program's icon and its files' icon" || fail "sg-$k: icons"
        desc=$(wrestool -x --raw -t 16 "$E" | tr -d '\0' | grep -o "SG Office [A-Z][a-z]*" | head -1)
        [ "$desc" = "SG Office $(echo $k | sed 's/^./\U&/')" ] && pass "sg-$k is '$desc' (Default apps and Open with show it)" || fail "sg-$k's FileDescription is '$desc'"
    fi
done
for n in Documents Spreadsheets Presentations; do
    grep -q "add_beside(L\"SG Office $n\", L\"sg-$(echo $n | tr A-Z a-z)64.exe\")" "$HERE/src/sg-start.c" || fail "Start does not list SG Office $n"
    grep -q "{ L\"$n\", { L\"" "$HERE/src/control/set_apps.c" || fail "Default apps has no $n row"
done
pass "Start lists the three programs (when installed); Default apps has a row for each kind"

# 5. no Microsoft product as our name: what users see as our programs' and types' names
if grep -E '"(FriendlyAppName|FriendlyTypeName|ApplicationName)"="[^"]*(Word|Excel|PowerPoint|Microsoft|Office 365|Windows)' \
        "$R" >/dev/null; then fail "a name of ours uses a Microsoft product name"
else pass "our names are our own (SG Office ..., Document, Spreadsheet, Presentation)"; fi

rm -rf "$T"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
