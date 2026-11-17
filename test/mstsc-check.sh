#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for sg-mstsc, Remote Desktop Connection.
#
# Two halves:
#   1. /sg-dry-run cases: what FreeRDP command line each mstsc-style input
#      produces -- switches, .rdp files in both encodings, IPv6 -- and that
#      every attempt to smuggle a FreeRDP option in through a connection file
#      or name is refused rather than passed on.
#   2. The real launch path: sg-mstsc starts a fake "client" through Wine's
#      \\?\unix\ path, and the fake writes down the argv it actually received.
#      That is the security boundary -- a Windows command line turned back into
#      a native argv by Wine -- so it is tested end to end, not assumed.
#
# Skips (77) without wine-sg or the built executable.
set -eu

HERE=$(cd "$(dirname "$0")/.." && pwd)
WINE_DIR=${SG_WINE_DIR:-/opt/wine-sg}
EXE="$HERE/build/sg-mstsc64.exe"
[ -x "$WINE_DIR/bin/wine" ] || { echo "SKIP: wine-sg not installed at $WINE_DIR"; exit 77; }
[ -f "$EXE" ] || { echo "SKIP: $EXE not built (make build)"; exit 77; }

T=$(mktemp -d)
trap 'WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null; rm -rf "$T"' EXIT
export WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDEBUG=-all PATH="$WINE_DIR/bin:$PATH"
# the session file and the password file live in a private runtime directory; the helper is this
# tree's (SG_RDP_CONNECT is honoured only with SG_MSTSC_TEST=1)
mkdir -m 700 "$T/xdg"; export XDG_RUNTIME_DIR="$T/xdg"
export SG_RDP_CONNECT="\\\\?\\unix$(printf '%s' "$HERE/src/sg-rdp-connect" | tr / '\\\\')"
unset DISPLAY WAYLAND_DISPLAY
WINEDLLOVERRIDES='mscoree,mshtml=' wineboot -i >/dev/null 2>&1   # throwaway prefix only
wineserver -w

FAILED=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; FAILED=1; }

CLIENT='\\?\unix\usr\bin\sdl-freerdp3'

# dry NAME EXPECTED ARGS... -- run a dry run and compare its one line of output.
dry() {
    _name=$1 _want=$2; shift 2
    _got=$(wine "$EXE" /sg-dry-run "$@" 2>/dev/null | tr -d '\r') || true
    if [ "$_got" = "$_want" ]; then pass "$_name"
    else fail "$_name"; printf '      want: %s\n      got:  %s\n' "$_want" "$_got"; fi
}

# An .rdp file as mstsc writes it: UTF-16LE with a byte-order mark.
rdp_utf16() { printf '%s\r\n' "$@" | iconv -f UTF-8 -t UTF-16LE | { printf '\377\376'; cat; }; }

echo "== command line"
dry "/v: host"                 "$CLIENT /v:server.sgtest.lan /dynamic-resolution +clipboard" /v:server.sgtest.lan
dry "/v: host:port and /f"     "$CLIENT /v:10.0.0.5:3390 /f +clipboard" /v:10.0.0.5:3390 /f
dry "/w: /h: size"             "$CLIENT /v:h1 /size:1280x720 +clipboard" /w:1280 /h:720 /v:h1
dry "bracketed IPv6 with port" "$CLIENT /v:[fe80::1]:3389 /dynamic-resolution +clipboard" '/v:[fe80::1]:3389'
dry "bare IPv6 gets brackets"  "$CLIENT /v:[fe80::1] /dynamic-resolution +clipboard" /v:fe80::1
dry "no server is refused"     "REFUSED The computer name is not valid."

echo "== connection files"
rdp_utf16 'full address:s:rdp.sgtest.lan:3390' 'username:s:SGTEST\alice' 'screen mode id:i:2' > "$T/mstsc.rdp"
dry "UTF-16 .rdp from mstsc"   "$CLIENT /v:rdp.sgtest.lan:3390 /u:alice /d:SGTEST /f +clipboard" "$(winepath -w "$T/mstsc.rdp")"

printf '%s\n' 'full address:s:files.sgtest.lan' 'username:s:bob@sgtest.lan' \
    'drivestoredirect:s:*' 'redirectdrives:i:1' 'redirectprinters:i:1' 'devicestoredirect:s:*' \
    'desktopwidth:i:1600' 'desktopheight:i:900' > "$T/handmade.rdp"
dry "UTF-8 .rdp; drives and devices ignored, printers kept" "$CLIENT /v:files.sgtest.lan /u:bob@sgtest.lan /size:1600x900 +clipboard /printer" \
    "$(winepath -w "$T/handmade.rdp")"

echo "== monitors, session and redirections (-282)"
dry "/span"                    "$CLIENT /v:h1 +span /f +clipboard" /v:h1 /span
dry "/multimon"                "$CLIENT /v:h1 /multimon /f +clipboard" /v:h1 /multimon
dry "/admin and /public"       "$CLIENT /v:h1 /dynamic-resolution +admin +clipboard /cache:bitmap:off" /v:h1 /admin /public
dry "/restrictedAdmin"         "$CLIENT /v:h1 /dynamic-resolution +restricted-admin +clipboard" /v:h1 /restrictedAdmin
printf '%s\n' 'full address:s:mm.sgtest.lan' 'use multimon:i:1' 'selectedmonitors:s:0,1' > "$T/mm.rdp"
dry ".rdp use multimon, selectedmonitors" "$CLIENT /v:mm.sgtest.lan /multimon /monitors:0,1 /f +clipboard" "$(winepath -w "$T/mm.rdp")"
printf '%s\n' 'full address:s:mm.sgtest.lan' 'span monitors:i:1' 'use multimon:i:0' > "$T/span.rdp"
dry ".rdp span monitors"       "$CLIENT /v:mm.sgtest.lan +span /f +clipboard" "$(winepath -w "$T/span.rdp")"
printf '%s\n' 'full address:s:mm.sgtest.lan' 'use multimon:i:0' > "$T/one.rdp"
dry "switches override the file" "$CLIENT /v:mm.sgtest.lan /multimon /f +clipboard" "$(winepath -w "$T/one.rdp")" /multimon
printf '%s\n' 'full address:s:gw.sgtest.lan' 'gatewayhostname:s:rdg.sgtest.lan' 'gatewayusagemethod:i:1' \
    'redirectclipboard:i:0' 'audiomode:i:2' 'smart sizing:i:1' 'administrative session:i:1' > "$T/gw.rdp"
dry ".rdp gateway, clipboard off, no audio, smart sizing, admin" \
    "$CLIENT /v:gw.sgtest.lan /dynamic-resolution /smart-sizing +admin /gateway:g:rdg.sgtest.lan -clipboard /audio-mode:2" "$(winepath -w "$T/gw.rdp")"
printf '%s\n' 'full address:s:gw.sgtest.lan' 'gatewayhostname:s:rdg.sgtest.lan' 'gatewayusagemethod:i:0' > "$T/nogw.rdp"
dry ".rdp gateway not used (method 0)" "$CLIENT /v:gw.sgtest.lan /dynamic-resolution +clipboard" "$(winepath -w "$T/nogw.rdp")"
# Remote Desktop Manager 2026.3's file for "Open (external)" with "Use multiple monitors", as it wrote it on qa5
rdp_utf16 'screen mode id:i:2' 'desktopwidth:i:1920' 'desktopheight:i:1080' 'winposstr:s:0,3,0,0,1920,1080' 'session bpp:i:32' \
    'full address:s:127.0.0.1' 'compression:i:1' 'keyboardhook:i:0' 'audiomode:i:0' 'audioqualitymode:i:0' 'redirectdrives:i:1' \
    'redirectprinters:i:0' 'redirectcomports:i:0' 'redirectsmartcards:i:0' 'redirectclipboard:i:1' 'redirectposdevices:i:0' \
    'displayconnectionbar:i:1' 'authentication level:i:0' 'enablecredsspsupport:i:1' 'prompt for credentials:i:0' \
    'username:s:rdptest' 'domain:s:' 'administrative session:i:0' 'gatewaycredentialssource:i:4' 'gatewayhostname:s:' \
    'gatewayusagemethod:i:2' 'smart sizing:i:0' 'use multimon:i:1' > "$T/rdm.rdp"
dry "Remote Desktop Manager's external file and /f" "$CLIENT /v:127.0.0.1 /u:rdptest /multimon /f +clipboard /sound" \
    "$(winepath -w "$T/rdm.rdp")" /f

echo "== injection attempts are refused"
printf '%s\n' 'full address:s:evil.example /drive:root,/' > "$T/inject-host.rdp"
dry "option smuggled in the address" "REFUSED The computer name is not valid." "$(winepath -w "$T/inject-host.rdp")"
printf '%s\n' 'full address:s:ok.example' 'username:s:bob /drive:root,/' > "$T/inject-user.rdp"
dry "option smuggled in the user"    "REFUSED The user name is not valid." "$(winepath -w "$T/inject-user.rdp")"
printf '%s\n' 'full address:s:ok.example' 'username:s:bo"b' > "$T/inject-quote.rdp"
dry "quote in the user name"         "REFUSED The user name is not valid." "$(winepath -w "$T/inject-quote.rdp")"
dry "address that is an option"      "REFUSED The computer name is not valid." '/v:/drive:root,/'
dry "unreadable file"                "REFUSED unreadable file" 'C:\no\such\file.rdp'
printf '%s\n' 'full address:s:ok.example' 'use multimon:i:1' 'selectedmonitors:s:0,/drive:root,/' > "$T/inject-mon.rdp"
dry "option smuggled in selectedmonitors" "REFUSED The selected monitors are not valid." "$(winepath -w "$T/inject-mon.rdp")"
printf '%s\n' 'full address:s:ok.example' 'gatewayhostname:s:gw /drive:root,/' 'gatewayusagemethod:i:1' > "$T/inject-gw.rdp"
dry "option smuggled in the gateway" "REFUSED The RD Gateway server name is not valid." "$(winepath -w "$T/inject-gw.rdp")"

echo "== the real launch path through Wine"
# A fake client that records its argv, one argument per line, and renames the
# record into place only when complete: Wine does not reliably report a native
# child as finished, so the gate polls, and must never read half a file. If quoting and
# Wine's command-line-to-argv conversion agree, it sees exactly the arguments
# sg-mstsc meant, however awkward.
# It lives in a directory with spaces, so its own path has to be quoted:
# every argument sg-mstsc passes today is validated to contain no spaces, and
# without this the test would never exercise the quoting at all.
mkdir -p "$T/Remote Tools"
cat > "$T/Remote Tools/fake client" <<'EOF'
#!/bin/sh
d=$(dirname "$0")
for a in "$@"; do printf '%s\n' "$a"; done > "$d/argv.tmp" && mv "$d/argv.tmp" "$d/argv.txt"
EOF
chmod +x "$T/Remote Tools/fake client"
SG_MSTSC_TEST=1 SG_RDP_CLIENT="\\\\?\\unix$(printf '%s' "$T/Remote Tools/fake client" | tr / '\\\\')" \
    wine "$EXE" /v:server.sgtest.lan:3390 /f >/dev/null 2>&1 || true
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -s "$T/Remote Tools/argv.txt" ] && break; sleep 1; done
want=$(printf '%s\n' /v:server.sgtest.lan:3390 /f +clipboard)
if [ -s "$T/Remote Tools/argv.txt" ] && [ "$(cat "$T/Remote Tools/argv.txt")" = "$want" ]; then
    pass "native client started via \\\\?\\unix with exactly the intended argv"
else
    fail "native client argv"
    printf '      want: %s\n      got:  %s\n' "$(printf '%s' "$want" | tr '\n' ' ')" \
        "$(tr '\n' ' ' < "$T/Remote Tools/argv.txt" 2>/dev/null || echo '(client never ran)')"
fi

echo "== quoting round-trips through Wine"
# Every string sg-mstsc passes for real is validated to be free of spaces and
# quotes, so the case above cannot tell good quoting from none at all (a mutant
# that never quotes passed it: CreateProcess resolves an unquoted path by trying
# each space-separated prefix). /sg-echo-args pushes arbitrary strings through
# the same append_arg to the fake client, and each must arrive byte for byte.
rm -f "$T/Remote Tools/argv.txt"
TAB=$(printf '\t')
SG_MSTSC_TEST=1 SG_RDP_CLIENT="\\\\?\\unix$(printf '%s' "$T/Remote Tools/fake client" | tr / '\\\\')" \
    wine "$EXE" /sg-echo-args 'two words' 'has "quotes" inside' 'trailing backslash\' \
        'back\slash\"quote' 'C:\Program Files\x\' '' "tab${TAB}inside" >/dev/null 2>&1 || true
for _ in 1 2 3 4 5 6 7 8 9 10; do [ -f "$T/Remote Tools/argv.txt" ] && break; sleep 1; done
want=$(printf '%s\n' 'two words' 'has "quotes" inside' 'trailing backslash\' \
        'back\slash\"quote' 'C:\Program Files\x\' '' "tab${TAB}inside")
if [ -f "$T/Remote Tools/argv.txt" ] && [ "$(cat "$T/Remote Tools/argv.txt")" = "$want" ]; then
    pass "seven awkward arguments arrive exactly as sent"
else
    fail "quoting round-trip"
    printf '%s\n' "$want" > "$T/want.txt"
    diff "$T/want.txt" "$T/Remote Tools/argv.txt" 2>&1 | od -c | sed 's/^/      /' | head -20
fi

# Without SG_MSTSC_TEST=1 the switch is an unknown switch like any other.
got=$(wine "$EXE" /sg-echo-args /sg-dry-run /v:inert.example 2>/dev/null | tr -d '\r') || true
if [ "$got" = "$CLIENT /v:inert.example /dynamic-resolution +clipboard" ]; then
    pass "the test switch is inert unless SG_MSTSC_TEST=1"
else
    fail "the test switch is not inert: got '$got'"
fi

echo "== saved credentials of Remote Desktop Manager (-282)"
# RDM saves an entry's credentials as a generic TERMSRV/<computer> credential before it starts mstsc
SG_MSTSC_TEST=1 wine "$EXE" /sg-remember-generic /v:rdm.sgtest.lan rdptest 'Gen#Pass9' >/dev/null 2>&1 || true
r=$(SG_MSTSC_TEST=1 wine "$EXE" /sg-saved /v:rdm.sgtest.lan 2>/dev/null | tr -d '\r')
[ "$r" = "SAVED rdptest 9" ] && pass "a generic TERMSRV credential (as RDM saves) is used" || fail "generic credential: '$r'"

echo "== a connection lasts as long as its session (-282)"
# RDM's "Open (external)" watches mstsc to know when the session ends: mstsc returns only when the
# client has exited (the client's standard output, a pipe, closes)
cat > "$T/Remote Tools/slow client" <<'EOF2'
#!/bin/sh
sleep 4
echo done > "$(dirname "$0")/slow.txt"
EOF2
chmod +x "$T/Remote Tools/slow client"
rm -f "$T/Remote Tools/slow.txt"
start=$(date +%s)
SG_MSTSC_TEST=1 SG_RDP_CLIENT="\\\\?\\unix$(printf '%s' "$T/Remote Tools/slow client" | tr / '\\\\')" \
    timeout 60 wine "$EXE" /v:server.sgtest.lan >/dev/null 2>&1 || true
took=$(( $(date +%s) - start ))
if [ -f "$T/Remote Tools/slow.txt" ] && [ "$took" -ge 4 ]; then pass "mstsc returned when the client ended (${took} s)"
else fail "mstsc returned after ${took} s, the client $( [ -f "$T/Remote Tools/slow.txt" ] && echo done || echo 'still running')"; fi

echo
if [ "$FAILED" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$FAILED"
