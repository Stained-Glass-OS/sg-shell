#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Open with lists a Linux app for every type it opens (src/linuxapps), as Windows lists a program
# for every extension it registered (OpenWithProgids): sg-linuxapp64.exe --sync registers the app's
# ProgID (SG.LinuxApp.<desktop id>) under each extension the shared MIME database gives its
# MimeType= types (mime/globs2's plain globs, mime/aliases followed) -- Inkscape for .svg, GIMP for
# .xcf, not only the types Settings > Default apps chooses for. A type the app no longer names, or
# the app gone: its extensions let go of it, another app's kept.
#   mutant: -DSG_MUTANT_OPENWITH_DEFAULTS_ONLY (src/linuxapps/sg-linuxapp.c)
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
EXE="${SG_LINUXAPP_EXE:-$HERE/build/sg-linuxapp64.exe}"
WINE=${WINE:-wine}
command -v "$WINE" >/dev/null 2>&1 || { echo "SKIP: no wine"; exit 77; }
[ -f "$EXE" ] || { echo "SKIP: build sg-linuxapp64.exe first (make build)"; exit 77; }
RC=0; T=$(mktemp -d /var/tmp/sg-linuxopenwith.XXXXXX)
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
unset DISPLAY WAYLAND_DISPLAY DBUS_SESSION_BUS_ADDRESS
export HOME="$T/home" WINEPREFIX="$T/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="winemenubuilder.exe=d;mscoree,mshtml="
export XDG_DATA_HOME="$T/home/.local/share" XDG_DATA_DIRS="$T/share" SG_LINUXAPPS_HIDDEN="$T/hidden" SG_LINUXAPP_DEPS=/bin/true
unset XDG_CONFIG_HOME
WINESERVER=${WINESERVER:-$(dirname "$(command -v "$WINE")")/wineserver}
trap '"$WINESERVER" -k 2>/dev/null; command -v sg_kill_prefix >/dev/null && sg_kill_prefix "$WINEPREFIX"; rm -rf "$T"' EXIT INT TERM
mkdir -p "$HOME" "$T/share/applications" "$T/share/mime" "$T/bin" "$XDG_DATA_HOME/applications"
: > "$T/hidden"
printf '#!/bin/sh\nexit 0\n' > "$T/bin/gate-draw"; chmod 755 "$T/bin/gate-draw"; cp "$T/bin/gate-draw" "$T/bin/gate-paint"
export PATH="$T/bin:$PATH"
cat > "$T/share/mime/globs2" <<'EOF'
# a shared MIME database's globs (update-mime-database)
80:image/svg+xml:*.svg
80:image/svg+xml-compressed:*.svgz
50:image/x-xcf:*.xcf
50:image/png:*.png
50:image/x-gate-thing:*.gatething
50:image/x-gate-thing:gate-*.weird
EOF
printf 'image/x-old-gate image/x-gate-thing\n' > "$T/share/mime/aliases"
app() { id=$1; shift; { echo '[Desktop Entry]'; for l in "$@"; do echo "$l"; done; } > "$T/share/applications/$id.desktop"; }
app org.gate.Draw 'Type=Application' 'Name=Gate Draw' 'Exec=gate-draw %F' 'MimeType=image/svg+xml;image/svg+xml-compressed;image/x-old-gate;'
app org.gate.Paint 'Type=Application' 'Name=Gate Paint' 'Exec=gate-paint %F' 'MimeType=image/x-xcf;image/png;image/svg+xml;'
"$WINE" wineboot -i >/dev/null 2>&1
"$WINE" "$EXE" --sync; "$WINESERVER" -w
ow() { "$WINE" reg query "HKCU\\Software\\Classes\\$1\\OpenWithProgids" 2>/dev/null | tr -d '\r' | awk '/SG\.LinuxApp/ { print $1 }' | sort | tr '\n' ' '; }
[ "$(ow .svg)" = "SG.LinuxApp.org.gate.Draw SG.LinuxApp.org.gate.Paint " ] && pass ".svg (a type Default apps does not choose for): both apps that open it" || fail ".svg: '$(ow .svg)'"
[ "$(ow .svgz)" = "SG.LinuxApp.org.gate.Draw " ] && [ "$(ow .xcf)" = "SG.LinuxApp.org.gate.Paint " ] && pass "each extension of each type it names (.svgz, .xcf)" || fail ".svgz '$(ow .svgz)' .xcf '$(ow .xcf)'"
[ "$(ow .gatething)" = "SG.LinuxApp.org.gate.Draw " ] && pass "a type named by its alias (mime/aliases)" || fail "alias: '$(ow .gatething)'"
[ "$(ow .png)" = "SG.LinuxApp.org.gate.Paint " ] && pass "and the Default apps types as before (.png)" || fail ".png: '$(ow .png)'"
app org.gate.Draw 'Type=Application' 'Name=Gate Draw' 'Exec=gate-draw %F' 'MimeType=image/svg+xml;'
rm "$T/share/applications/org.gate.Paint.desktop"
"$WINE" "$EXE" --sync; "$WINESERVER" -w
[ "$(ow .svg)" = "SG.LinuxApp.org.gate.Draw " ] && [ -z "$(ow .svgz)" ] && [ -z "$(ow .xcf)" ] && [ -z "$(ow .png)" ] \
    && pass "a type no longer named, an app gone: their extensions let go (the other app kept)" || fail "after: .svg '$(ow .svg)' .svgz '$(ow .svgz)' .xcf '$(ow .xcf)' .png '$(ow .png)'"
echo
[ "$RC" -eq 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
