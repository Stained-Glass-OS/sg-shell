#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for Get a web browser (sg-browser64.exe) and web links out of the box.
# On a shell desktop under Xvfb, with defaults/81-sg-browser.reg imported
# (pointing at this build), a winget-pkgs-shaped source served from this
# machine (python3 -m http.server) and stand-ins built here
# (test/sg-browser-fake.c: an installer, the browser it installs, winget):
#
#   - the manifest reader's unit test (test/browser-manifest-test.c, native)
#   - with no browser, a web link (wine start http://...) opens Get a web
#     browser, naming the link, offering what HKLM says
#   - an installer whose download is not the file the manifest's SHA-256
#     names is refused and never run
#   - Install: the newest version (1.10.0, not 1.9.0), the x64 installer,
#     downloaded, checked, run with the manifest's silent switches; the
#     browser registers, becomes the default (UserChoice, the user's http
#     class) and the link opens in it
#   - later links and .html files go straight to it; with the user's choice
#     reset and one browser, the link still opens there (and it is made the
#     default again); with two, "How do you want to open this?" and the one
#     picked opens it and, with "Always", becomes the default
#   - with winget installed (a stand-in), Install goes through winget
#   - Internet Explorer is offered for the link
#
# SG_BROWSER_ONLINE=1 also installs the real Mozilla Firefox from the real
# repository (GitHub) and its maker (network; a few minutes).
#
# Screenshots: build/browser-*.png. Needs wine-sg, mingw, gcc, Xvfb, xdotool,
# ImageMagick, python3; skips (77) without them. SG_BROWSER_EXE tests another
# build (mutation testing).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_BROWSER_EXE:-$HERE/build/sg-browser64.exe}"
OUT="$HERE/build"
MINGW="${MINGW64:-x86_64-w64-mingw32-gcc}"
RC=0; XP=""; HP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import python3 gcc "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-browser-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    [ -n "$HP" ] && kill "$HP" 2>/dev/null
    [ -n "${SG_KEEP:-}" ] || rm -rf "$T"
}
trap cleanup EXIT INT TERM

# --- the manifest reader, natively ---------------------------------------------------------------
if gcc -O1 -o "$T/manifest-test" "$HERE/test/browser-manifest-test.c" "$HERE/src/browser/manifest.c" 2>"$T/cc.log"; then
    "$T/manifest-test" > "$T/manifest.log" 2>&1 && pass "the manifest reader's unit test ($(grep -c ^PASS "$T/manifest.log") checks)" \
        || { fail "the manifest reader's unit test:"; grep ^FAIL "$T/manifest.log"; }
else fail "the manifest test does not build: $(head -3 "$T/cc.log")"; fi
"$MINGW" -municode -O1 -o "$T/fakebrowser-setup.exe" "$HERE/test/sg-browser-fake.c" || { fail "the stand-ins do not build"; exit 1; }
mkdir -p "$T/wg"; cp "$T/fakebrowser-setup.exe" "$T/wg/winget.exe"

# --- the source: winget-pkgs' layout on this machine --------------------------------------------------
W="$T/www"; mkdir -p "$W/api/f/Fake" "$W/api/b/Bad" "$W/raw/f/Fake/Browser/1.10.0" "$W/raw/b/Bad/Hash/2.0" "$W/files"
cp "$T/fakebrowser-setup.exe" "$W/files/fakebrowser-setup.exe"
SHA=$(sha256sum "$W/files/fakebrowser-setup.exe" | cut -d' ' -f1)
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
printf '[{"name": "1.9.0", "type": "dir"}, {"name": "1.10.0", "type": "dir"}, {"name": "de", "type": "dir"}]' > "$W/api/f/Fake/Browser"
printf '[{"name": "2.0", "type": "dir"}]' > "$W/api/b/Bad/Hash"
cat > "$W/raw/f/Fake/Browser/1.10.0/Fake.Browser.installer.yaml" <<EOF
# Created by the gate
PackageIdentifier: Fake.Browser
PackageVersion: 1.10.0
InstallerType: nullsoft
Scope: machine
InstallerSwitches:
  Silent: /S /gate-silent
Protocols:
- http
Installers:
- Architecture: x86
  InstallerUrl: http://127.0.0.1:$PORT/files/wrong-arch.exe
  InstallerSha256: 0000000000000000000000000000000000000000000000000000000000000000
- Architecture: x64
  InstallerUrl: http://127.0.0.1:$PORT/files/fakebrowser-setup.exe
  InstallerSha256: $SHA
ManifestType: installer
EOF
cat > "$W/raw/b/Bad/Hash/2.0/Bad.Hash.installer.yaml" <<EOF
PackageIdentifier: Bad.Hash
PackageVersion: 2.0
InstallerType: nullsoft
Installers:
- Architecture: x64
  InstallerUrl: http://127.0.0.1:$PORT/files/fakebrowser-setup.exe
  InstallerSha256: 1111111111111111111111111111111111111111111111111111111111111111
EOF
(cd "$W" && exec python3 -m http.server "$PORT" --bind 127.0.0.1 > "$T/http.log" 2>&1) & HP=$!

Xvfb -displayfd 3 -screen 0 1024x768x24 -nolisten tcp 3>"$T/display" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/display" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
DPY=$(cat "$T/display" 2>/dev/null); [ -n "$DPY" ] || { echo "FAIL  Xvfb did not start"; exit 1; }
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH"
export SG_BROWSER_LIST_URL="http://127.0.0.1:$PORT/api/" SG_BROWSER_RAW_URL="http://127.0.0.1:$PORT/raw/"
wine wineboot --init >/dev/null 2>&1; wineserver -w
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
python3 - "$HERE/defaults/81-sg-browser.reg" "$winexe" > "$T/browser.reg" <<'EOF2'
import sys
reg = open(sys.argv[1]).read()
esc = lambda p: p.replace("\\", "\\\\")
sys.stdout.write(reg.replace(esc(r"Z:\usr\libexec\stained-glass\shell\sg-browser64.exe"), esc(sys.argv[2])))
EOF2
wine reg import "$(wine winepath -w "$T/browser.reg" | tr -d '\r')" >/dev/null 2>&1 || fail "reg import failed"
# the gate's offers: the stand-in, and one whose download is not what its manifest says
wine reg delete 'HKLM\Software\Stained Glass\Web Browsers\Offers' /f >/dev/null 2>&1
reg 'HKLM\Software\Stained Glass\Web Browsers\Offers\01' /v Id /d Fake.Browser
reg 'HKLM\Software\Stained Glass\Web Browsers\Offers\01' /v Name /d 'Fake Browser'
reg 'HKLM\Software\Stained Glass\Web Browsers\Offers\01' /v Publisher /d 'The gate'
reg 'HKLM\Software\Stained Glass\Web Browsers\Offers\01' /v Description /d 'Installs from this machine.'
reg 'HKLM\Software\Stained Glass\Web Browsers\Offers\02' /v Id /d Bad.Hash
reg 'HKLM\Software\Stained Glass\Web Browsers\Offers\02' /v Name /d 'Tampered Browser'
reg 'HKLM\Software\Stained Glass\Web Browsers\Offers\02' /v Publisher /d 'Nobody'
reg 'HKLM\Software\Stained Glass\Web Browsers\Offers\02' /v Description /d 'Its download does not match its manifest.'
wineserver -w
G="$WINEPREFIX/drive_c/gate"; mkdir -p "$G"
printf '<html><title>x</title></html>\n' > "$WINEPREFIX/drive_c/page.html"

WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done

D="$G/dump"
field() { sed -n "s/^$1 //p" "${2:-$D}" 2>/dev/null | head -1; }
wait_line() {  # regex seconds [file]
    i=0
    while ! grep -qE "$1" "${3:-$D}" 2>/dev/null && [ $i -lt $(( $2 * 4 )) ]; do sleep 0.25; i=$((i + 1)); done
    grep -qE "$1" "${3:-$D}" 2>/dev/null
}
shot() { sleep 0.4; import -window root "$OUT/browser-$1.png" 2>/dev/null; }
click_hit() {  # name arg
    set -- $(grep "^hit $1 $2 " "$D" | head -1)
    [ $# -ge 5 ] || return 1
    xdotool mousemove "$4" "$5" click 1
}
regval() { wine reg query "$1" /v "$2" 2>/dev/null | tr -d '\r' | sed -n "s/^ *$2 *REG_SZ *//p"; }
regdef() { wine reg query "$1" /ve 2>/dev/null | tr -d '\r' | sed -n 's/^ *(Default) *REG_SZ *//p'; }
reset_defaults() {
    for k in http https .htm .html; do wine reg delete "HKCU\\Software\\Classes\\$k" /f >/dev/null 2>&1; done
    for k in http https; do wine reg delete "HKCU\\Software\\Microsoft\\Windows\\Shell\\Associations\\UrlAssociations\\$k" /f >/dev/null 2>&1; done
}
wd() { wine winepath -w "$1" 2>/dev/null | tr -d '\r'; }
export SG_BROWSER_DUMP='C:\gate\dump'

# --- no browser: a web link opens Get a web browser ---------------------------------------------------
wine start 'http://example.com/first' >/dev/null 2>&1 &
if wait_line '^mode get' 20; then pass "with no browser, a web link opens Get a web browser"
else fail "a web link: dump '$(head -3 "$D" 2>/dev/null | tr '\n' ' ')'"; fi
[ "$(field target)" = 'http://example.com/first' ] && pass "it names the link to open" || fail "target '$(field target)'"
grep -q '^offer Fake.Browser idle' "$D" && grep -q '^offer Bad.Hash idle' "$D" && pass "it offers what HKLM's Offers list" || fail "offers: $(grep ^offer "$D")"
grep -q '^hit ie ' "$D" && pass "Internet Explorer is offered for the link" || fail "no Internet Explorer link"
shot get

# --- a download that is not what its maker published ---------------------------------------------------
click_hit install 1 || fail "no Install button for the tampered browser"
if wait_line '^offer Bad.Hash failed' 20; then
    grep -q '^offer Bad.Hash failed .*SHA-256 does not match' "$D" && pass "a download whose SHA-256 is not the manifest's is refused" \
        || fail "tampered: '$(grep '^offer Bad.Hash' "$D")'"
else fail "tampered: '$(grep '^offer Bad.Hash' "$D")'"; fi
[ ! -e "$G/setup.log" ] && pass "and its installer never ran" || fail "the tampered installer ran: $(cat "$G/setup.log")"
shot refused

# --- install the stand-in ---------------------------------------------------------------------------------
click_hit install 0 || fail "no Install button for the stand-in"
if wait_line '^offer Fake.Browser done' 40; then pass "Install: downloaded, checked and installed"
else fail "install: '$(grep '^offer Fake.Browser' "$D")' $(grep ^status "$D")"; fi
set -- $(field package)
[ "${2:-}" = 1.10.0 ] && pass "the newest version, 1.10.0 (not 1.9.0 or 'de')" || fail "version '${2:-}'"
case "${4:-}" in *fakebrowser-setup.exe) pass "the x64 installer" ;; *) fail "installer URL '${4:-}'" ;; esac
[ "$(field installer)" = '/S /gate-silent' ] && grep -q '/S /gate-silent' "$G/setup.log" 2>/dev/null \
    && pass "run with the manifest's silent switches" || fail "installer args '$(field installer)' / setup.log '$(cat "$G/setup.log" 2>/dev/null)'"
[ "$(field method)" = manifest ] && pass "through the manifest (no winget)" || fail "method '$(field method)'"
[ -f "$WINEPREFIX/drive_c/Program Files/Fake Browser/fakebrowser.exe" ] && pass "the browser is installed" || fail "not installed"
[ "$(field default)" = FakeBrowserURL ] && pass "and is the default (UserChoice)" || fail "default '$(field default)'"
case "$(regdef 'HKCU\Software\Classes\http\shell\open\command')" in *fakebrowser.exe*) pass "the user's http class is the browser's" ;;
    *) fail "HKCU http: '$(regdef 'HKCU\Software\Classes\http\shell\open\command')'" ;; esac
wait_line 'http://example.com/first' 10 "$G/browser.log" && pass "the link opened in it" || fail "browser.log: '$(cat "$G/browser.log" 2>/dev/null)'"
shot installed
set -- $(grep '^hit close ' "$D"); [ $# -ge 5 ] && xdotool mousemove "$4" "$5" click 1
sleep 1
# Settings > Apps > Default apps shows it as the web browser
SET="$HERE/build/sg-settings64.exe"
if [ -f "$SET" ]; then
    SG_SETTINGS_DUMP='C:\gate\settings' wine "$SET" ms-settings:defaultapps >/dev/null 2>&1 &
    i=0; while ! { tr -d '\r' < "$G/settings"; } 2>/dev/null | grep -q '^control ComboBox'; do [ $i -gt 80 ] && break; sleep 0.25; i=$((i + 1)); done
    tr -d '\r' < "$G/settings" 2>/dev/null | grep -q '^control ComboBox.*: Fake Browser' \
        && pass "Settings > Default apps shows it as the web browser" \
        || fail "Default apps: $(tr -d '\r' < "$G/settings" 2>/dev/null | grep '^control ComboBox' | tr '\n' '|')"
    wine taskkill /f /im sg-settings64.exe >/dev/null 2>&1
fi

# --- later links and files go straight there -------------------------------------------------------------
rm -f "$D"
wine start 'https://second.example/' >/dev/null 2>&1
wait_line 'https://second.example/' 15 "$G/browser.log" && pass "the next link opens in the default browser" || fail "second link not opened"
[ "$(field mode)" = none ] && pass "without asking: the user's choice (UserChoice) is honoured" || fail "second: mode '$(field mode)'"
wine start 'C:\page.html' >/dev/null 2>&1
wait_line 'page.html' 15 "$G/browser.log" && pass "an .html file opens in the browser" || fail "page.html not opened"

# --- the choice reset, one browser: it opens there ---------------------------------------------------------
reset_defaults; rm -f "$D"
wine start 'http://third.example/' >/dev/null 2>&1
wait_line 'http://third.example/' 15 "$G/browser.log" && pass "choice reset, one browser: the link opens in it" || fail "third link not opened"
[ "$(field mode)" = none ] && [ "$(field default)" = FakeBrowserURL ] && pass "without asking, and it is the default again" \
    || fail "one browser: mode '$(field mode)' default '$(field default)'"

# --- two browsers: How do you want to open this? --------------------------------------------------------------
fb=$(wine winepath -w "$WINEPREFIX/drive_c/Program Files/Fake Browser/fakebrowser.exe" | tr -d '\r')
reg 'HKCU\Software\Clients\StartMenuInternet\OtherBrowser' /ve /d 'Other Browser'
reg 'HKCU\Software\Clients\StartMenuInternet\OtherBrowser\shell\open\command' /ve /d "\"$fb\" -other"
reg 'HKCU\Software\Clients\StartMenuInternet\OtherBrowser\Capabilities\URLAssociations' /v http /d OtherURL
reg 'HKCU\Software\Classes\OtherURL\shell\open\command' /ve /d "\"$fb\" -other \"%1\""
reset_defaults; rm -f "$D"
wine start 'http://fourth.example/' >/dev/null 2>&1 &
if wait_line '^mode choose' 20; then
    pass "two browsers: How do you want to open this?"
    [ "$(grep -c '^hit pick ' "$D")" = 2 ] && pass "both are listed" || fail "listed: $(grep -c '^hit pick ' "$D")"
    shot choose
    other=$(grep '^installed ' "$D" | grep -n 'Other Browser' | cut -d: -f1)
    click_hit pick $((other - 1)); sleep 0.5
    click_hit ok 0
    wait_line ' -other "?http://fourth.example/' 15 "$G/browser.log" && pass "the one picked opens the link" || fail "fourth: '$(tail -2 "$G/browser.log")'"
    wait_line '^default OtherURL' 5 && pass "and, with Always, becomes the default" || fail "default '$(field default)'"
else fail "two browsers: mode '$(field mode)'"; fi

# --- winget, when installed -------------------------------------------------------------------------------
wine reg delete 'HKCU\Software\Clients\StartMenuInternet\OtherBrowser' /f >/dev/null 2>&1
wine reg delete 'HKLM\Software\Clients\StartMenuInternet\FakeBrowser' /f >/dev/null 2>&1
reset_defaults; rm -f "$D" "$G/setup.log"
SG_BROWSER_WINGET="$(wd "$T/wg/winget.exe")" SG_FAKE_SETUP="$(wd "$T/fakebrowser-setup.exe")" wine start 'http://fifth.example/' >/dev/null 2>&1 &
if wait_line '^hit install 0 ' 20; then
    click_hit install 0
    wait_line '^offer Fake.Browser done' 30 && pass "with winget installed, Install works through it" || fail "winget install: '$(grep '^offer Fake' "$D")'"
    [ "$(field method)" = winget ] && grep -q 'install --id Fake.Browser -e --silent --accept-package-agreements --accept-source-agreements' "$G/winget.log" 2>/dev/null \
        && pass "winget install --id Fake.Browser -e --silent ..." || fail "winget: method '$(field method)' log '$(cat "$G/winget.log" 2>/dev/null)'"
    wait_line 'http://fifth.example/' 10 "$G/browser.log" && pass "and the link opens" || fail "fifth link not opened"
    shot winget
    set -- $(grep '^hit close ' "$D"); [ $# -ge 5 ] && xdotool mousemove "$4" "$5" click 1
else fail "winget: no Get a web browser ($(head -2 "$D" 2>/dev/null | tr '\n' ' '))"; fi

# --- the real thing -------------------------------------------------------------------------------------
if [ "${SG_BROWSER_ONLINE:-0}" = 1 ]; then
    unset SG_BROWSER_LIST_URL SG_BROWSER_RAW_URL
    wine reg delete 'HKLM\Software\Stained Glass\Web Browsers\Offers' /f >/dev/null 2>&1
    wine reg import "$(wine winepath -w "$T/browser.reg" | tr -d '\r')" >/dev/null 2>&1
    wine reg delete 'HKLM\Software\Clients\StartMenuInternet\FakeBrowser' /f >/dev/null 2>&1
    reset_defaults; rm -f "$D"
    wine start 'https://www.debian.org/' >/dev/null 2>&1 &
    if wait_line '^hit install 0 ' 20 && grep -q '^offer Mozilla.Firefox' "$D"; then
        click_hit install 0
        if wait_line '^offer Mozilla.Firefox (done|failed)' 900; then
            grep '^package\|^installer\|^offer Mozilla' "$D"
            grep -q '^offer Mozilla.Firefox done' "$D" && pass "ONLINE: Mozilla Firefox $(field package | cut -d' ' -f2) downloaded from Mozilla, checked and installed" \
                || fail "ONLINE: $(grep '^offer Mozilla' "$D")"
            [ -f "$WINEPREFIX/drive_c/Program Files/Mozilla Firefox/firefox.exe" ] && pass "ONLINE: firefox.exe is there" || fail "ONLINE: no firefox.exe"
            grep -q '^opened .*firefox.exe.*https://www.debian.org/' "$D" && pass "ONLINE: the link was opened in Firefox" || fail "ONLINE: opened '$(field opened)'"
            sleep 15; shot online
        else fail "ONLINE: the install did not finish: $(grep '^offer Mozilla' "$D")"; fi
    else fail "ONLINE: no Firefox offer"; fi
fi

[ $RC = 0 ] && echo "browser-check: all passed" || echo "browser-check: FAILED"
exit $RC
