#!/bin/sh
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# SG Office -- the gate that needs no Windows LibreOffice: SG Office
# Functions' logic, the registrations (and that the file-type defaults file
# never changes once shipped), the programs and their resources, the payload
# Get SG Office applies, the templates (read back by a LibreOffice when one is
# installed here), Start and Default apps, and no Microsoft product names as
# ours. The whole suite under Wine is test/office-wine-check.sh.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
B="$HERE/build"
RC=0
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
PY=/usr/bin/python3
[ -x "$PY" ] || PY=python3

# 1. the functions' logic (Excel's documented results)
if "$PY" "$HERE/office/functions/test_functions.py" > "$B/office-functions.log" 2>&1; then
    pass "SG Office Functions: $(grep -c '^PASS' "$B/office-functions.log") cases match Excel"
else
    grep '^FAIL' "$B/office-functions.log"; fail "SG Office Functions' logic"
fi

# 2. the add-in: IDL identifiers LibreOffice accepts, and every function
#    under the name Excel writes into its files
X="$B/office/payload/extensions/sg-office-functions"
if [ -f "$X/XSgFunctions.idl" ] && ! grep -E '\[in\] [a-z< >]+ [A-Za-z0-9]*_' "$X/XSgFunctions.idl" >/dev/null; then
    pass "the type library's parameters are IDL identifiers (no underscores)"
else fail "XSgFunctions.idl missing or has an underscore identifier"; fi
"$PY" - "$HERE" "$X" <<'PY' && pass "every function has Excel's file name (compatibility name), prefixed as Excel prefixes it" || fail "compatibility names"
import sys, re, os
here, x = sys.argv[1], sys.argv[2]
sys.path.insert(0, os.path.join(here, "office/functions")); sys.path.insert(0, os.path.join(here, "office/parity"))
from sgoffice_spec import FUNCTIONS
import corpus
xcu = open(os.path.join(x, "CalcAddIn.xcu")).read()
bad = 0
for m, name, excel, *_ in FUNCTIONS:
    want = ("_xlfn." + name) if name in corpus.FUTURE else name
    if excel != want or ('<value xml:lang="en-US">%s</value>' % excel) not in xcu:
        print("  %s: %s (want %s)" % (name, excel, want)); bad += 1
    if not hasattr(__import__("sgoffice_functions"), m):
        print("  %s: no logic" % m); bad += 1
sys.exit(1 if bad else 0)
PY
for f in META-INF/manifest.xml description.xml component.py pythonpath/sgoffice_functions.py pythonpath/sgoffice_spec.py; do
    [ -f "$X/$f" ] || fail "the extension lacks $f"
done

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
[ -f "$B/sg-office-setup64.exe" ] && pass "Get SG Office is built" || fail "sg-office-setup64.exe was not built"
for n in Documents Spreadsheets Presentations; do
    grep -q "add_beside(L\"SG Office $n\", L\"sg-$(echo $n | tr A-Z a-z)64.exe\")" "$HERE/src/sg-start.c" || fail "Start does not list SG Office $n"
    grep -q "{ L\"$n\", { L\"" "$HERE/src/control/set_apps.c" || fail "Default apps has no $n row"
done
pass "Start lists the three programs (when installed); Default apps has a row for each kind"

# 5. no Microsoft product as our name: what users see as our programs' and types' names
if grep -E '"(FriendlyAppName|FriendlyTypeName|ApplicationName)"="[^"]*(Word|Excel|PowerPoint|Microsoft|Office 365|Windows)' \
        "$R" >/dev/null; then fail "a name of ours uses a Microsoft product name"
else pass "our names are our own (SG Office ..., Document, Spreadsheet, Presentation)"; fi

# 6. the payload
P="$B/office/payload"
"$PY" - "$P/office.ini" <<'PY' && pass "office.ini pins an https download from The Document Foundation and its SHA-256" || fail "office.ini"
import configparser, re, sys
c = configparser.ConfigParser(); c.read(sys.argv[1]); s = c["LibreOffice"]
ok = s["Url"].startswith("https://download.documentfoundation.org/") and re.fullmatch("[0-9a-f]{64}", s["Sha256"]) \
    and s["Version"] in s["Url"] and "REGISTER_NO_MSO_TYPES=1" in s["MsiProperties"] and s["Payload"].isdigit()
sys.exit(0 if ok else 1)
PY
if command -v xmllint >/dev/null; then
    sed 's|@TEMPLATEDIR@|/t|' "$P/sg-office.xcd.in" | xmllint --noout - && xmllint --noout "$P/sg-office-user.xcu" \
        && pass "the defaults (.xcd) and the new user's settings (.xcu) are well-formed" || fail "the payload's XML"
fi
grep -q '<dependency file="main"/>' "$P/sg-office.xcd.in" && grep -q 'file://@TEMPLATEDIR@/normal.ott' "$P/sg-office.xcd.in" \
    && pass "the defaults are read after LibreOffice's own, and name the templates" || fail "the .xcd's dependencies or templates"
for t in normal.ott book.ots blank.otp; do
    [ "$(unzip -Z1 "$P/templates/$t" 2>/dev/null | head -1)" = mimetype ] || fail "template $t is not an ODF package"
done
pass "the templates are ODF packages"

R2="$P/ui/scalc/notebookbar_sgoffice.ui"
if [ -f "$R2" ] && grep -q '\.uno:InsertCalcTable' "$R2" && grep -q 'gdSGHomeStyles' "$R2" && grep -q 'gdSGHomeEditing' "$R2" \
        && grep -q '<value>notebookbar_sgoffice.ui</value>' "$P/sg-office.xcd.in"; then
    "$PY" - "$R2" <<'PY' && pass "the spreadsheet ribbon's Home tab: Clipboard, Font, Alignment, Number, Styles (Format as Table), Cells, Editing -- and it is the default" || fail "the Home tab's groups"
import sys, xml.etree.ElementTree as ET
r = ET.parse(sys.argv[1]).getroot()
home = [o for o in r.iter("object") if o.get("id") == "pmhbHome"][0]
ids = [c.find("object").get("id") for c in home if c.tag == "child"]
want = ["gdHomeClipboard", "gdHomeFont", "gdHomeAlignment", "gdHomeNumber", "gdSGHomeStyles", "gdSGHomeCells", "gdSGHomeEditing"]
styles = [o for o in r.iter("object") if o.get("id") == "gdSGHomeStyles"][0]
acts = [p.text for p in styles.iter("property") if p.get("name") == "action-name"]
sys.exit(0 if ids == want and ".uno:InsertCalcTable" in acts and ".uno:ConditionalFormatMenu" in acts else 1)
PY
else fail "no SG Office spreadsheet ribbon in the payload, or it is not the default"; fi
grep -q '<node oor:name=".uno:InsertCalcTable" oor:op="fuse"><prop oor:name="Label" oor:type="xs:string"><value xml:lang="en-US">Format as Table</value>' "$P/sg-office.xcd.in" \
    && pass "the button reads Format as Table (menus keep LibreOffice's wording)" || fail "no Format as Table label"

# 7. the templates as LibreOffice reads them (a native LibreOffice, when this machine has one)
if command -v soffice >/dev/null && "$PY" -c "import uno" 2>/dev/null; then
    "$PY" "$HERE/test/office-templates.py" "$HERE" "$P" && pass "new documents, spreadsheets and presentations start as Office's do" \
        || fail "the templates, as LibreOffice reads them"
else
    echo "SKIP  templates in LibreOffice (no soffice/python3-uno here)"
fi

rm -rf "$T"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
