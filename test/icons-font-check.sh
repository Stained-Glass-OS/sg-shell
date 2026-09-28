#!/bin/sh
# The shell's icon font (src/sg-icons-font.py): it builds, it has the
# characters programs draw caption buttons with (Firefox: U+E921-E923,
# U+E8BB and their high-contrast forms), and theme/52-sg-fonts.reg maps
# both of Windows' icon-font names to it for GDI and DirectWrite.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
RC=0; T=$(mktemp -d); trap 'rm -rf "$T"' EXIT INT TERM
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
python3 -c 'import fontTools' 2>/dev/null || { echo "SKIP: python3-fonttools missing"; exit 77; }
python3 "$HERE/src/sg-icons-font.py" "$T/i.ttf" || { fail "the font did not build"; exit 1; }
cps=$(python3 -c "
from fontTools.ttLib import TTFont
f = TTFont('$T/i.ttf'); m = f.getBestCmap()
print(' '.join('%04X' % c for c in sorted(m)))
print(f['name'].getDebugName(1))")
for c in E921 E922 E923 E8BB EF2C EF2D EF2E EF2F; do
    printf '%s\n' "$cps" | head -1 | grep -qw "$c" || fail "no U+$c"
done
[ $RC = 0 ] && pass "caption-button characters: minimize, maximize, restore, close (and high contrast)"
[ "$(printf '%s\n' "$cps" | tail -1)" = "Stained Glass Icons" ] && pass "family: Stained Glass Icons" || fail "family: $(printf '%s\n' "$cps" | tail -1)"
n=$(grep -c '^"Segoe \(MDL2 Assets\|Fluent Icons\)"="Stained Glass Icons"' "$HERE/theme/52-sg-fonts.reg")
[ "$n" = 4 ] && pass "both icon-font names map to it, for GDI and DirectWrite" || fail "mappings: $n of 4"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
