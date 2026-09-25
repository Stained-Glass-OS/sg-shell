#!/bin/sh
# Gate for the network programs (sg-ncpa: Network Connections and its IPv4
# dialog; sg-netflyout: the taskbar's network icon and flyout).
#
# They run for real under Wine and talk through the real sg-netctl bridge to
# the real sg-netd (`sg-netctl --serve`, per connection on a socket of the
# test's own), which drives a stand-in nmcli that records every call and
# answers from a small made-up network: a wired adapter with a DHCP lease and
# a Wi-Fi adapter with three networks in range. So what is checked is the
# whole path a click takes, short of NetworkManager itself (sg-image's
# `make net-test` covers that in a VM with a real radio):
#
#   - what the window shows: tiles, and the Network Connection Details rows
#   - the IPv4 dialog's OK: a static address, mask, gateway and DNS become
#     exactly one `nmcli connection modify` with the right values; a bad mask
#     or an off-subnet gateway is refused before anything is asked; a standard
#     user is refused by sg-netd and nothing changes
#   - the flyout's list and Connect: the key reaches the saved profile and no
#     command line; a wrong key is reported as a wrong key
#   - the windows appear through the program's own re-launch via the bridge,
#     and screenshots of each (build/net-ui-*.png) for a person to look at
#
# Needs wine-sg, Xvfb, xdotool, ImageMagick's import, python3 and sg-netctl
# (SG_NETCTL_SCRIPT, the sibling sg-session checkout, or /usr/bin/sg-netctl);
# skips (77) without them.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
NCPA="$HERE/build/sg-ncpa64.exe"
FLY="$HERE/build/sg-netflyout64.exe"
OUT="$HERE/build"
RC=0; DPY=91; XP=""; SRV=""
T=$(mktemp -d); chmod 755 "$T"
export HOME="$T"
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
# shellcheck disable=SC2317
cleanup() {
    [ -n "$SRV" ] && kill "$SRV" 2>/dev/null
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

NETCTL="${SG_NETCTL_SCRIPT:-}"
[ -n "$NETCTL" ] || for c in "$HERE/../sg-session/bin/sg-netctl" /usr/bin/sg-netctl; do [ -f "$c" ] && { NETCTL=$c; break; }; done
for need in Xvfb xdotool import python3; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$NCPA" ] && [ -f "$FLY" ] && [ -n "$NETCTL" ] || { echo "SKIP: wine-sg, the programs or sg-netctl missing"; exit 77; }
NETCTL=$(CDPATH='' cd -- "$(dirname -- "$NETCTL")" && pwd)/$(basename -- "$NETCTL")

# --- the stand-in NetworkManager, and sg-netd on a socket of our own --------------
mkdir -p "$T/fakebin" "$T/nm"
chmod 700 "$T/nm"
cat > "$T/fakebin/nmcli" <<'EOF'
#!/usr/bin/python3
import json, os, sys
args = sys.argv[1:]
with open(os.environ["FAKE_NM_LOG"], "a") as f:
    f.write(json.dumps(args) + "\n")
st = json.load(open(os.environ["FAKE_NM_STATE"]))
a = [x for x in args if x not in ("-t", "-e", "yes")]
if a[:2] == ["--wait", "45"]: a = a[2:]
if a[:1] == ["-f"]: a = a[2:]
o = sys.stdout.write
if a[:2] == ["device", "status"]:
    o("eth0:ethernet:connected:u-eth\nwlan0:wifi:disconnected:--\n")
elif a[:2] == ["device", "show"] and a[2] == "eth0":
    o("GENERAL.DEVICE:eth0\nGENERAL.STATE:100 (connected)\nGENERAL.HWADDR:52\\:54\\:00\\:12\\:34\\:56\nGENERAL.MTU:1500\n"
      "GENERAL.CONNECTION:Wired connection 1\nIP4.ADDRESS[1]:10.0.2.15/24\nIP4.GATEWAY:10.0.2.2\nIP4.DNS[1]:10.0.2.3\n"
      "IP4.DOMAIN[1]:sgtest.lan\nIP6.ADDRESS[1]:fe80\\:\\:5054\\:ff\\:fe12\\:3456/64\n"
      "DHCP4.OPTION[1]:dhcp_lease_time = 86400\nDHCP4.OPTION[2]:expiry = 1800086400\nDHCP4.OPTION[3]:dhcp_server_identifier = 10.0.2.2\n")
elif a[:2] == ["device", "show"]:
    o("GENERAL.DEVICE:wlan0\nGENERAL.STATE:30 (disconnected)\nGENERAL.HWADDR:02\\:00\\:00\\:00\\:00\\:00\nGENERAL.MTU:1500\nGENERAL.CONNECTION:--\n")
elif a[:2] == ["connection", "show"] and len(a) == 2:
    pass
elif a[:2] == ["connection", "show"]:
    o("connection.autoconnect:yes\nipv4.method:auto\nipv4.ignore-auto-dns:no\nipv6.method:auto\n")
elif a[:3] == ["device", "wifi", "list"]:
    for n in st["scan"]:
        o("%s:%s:%d:%s:wlan0\n" % (" ", n["name"].encode().hex(), n["signal"], n["security"]))
elif a[:2] == ["connection", "up"] and st.get("up_fails"):
    sys.stderr.write("Error: Connection activation failed: Secrets were required, but not provided.\n"
                     "Hint: use 'journalctl -xe NM_CONNECTION=x + NM_DEVICE=wlan0' to get more details.\n")
    sys.exit(4)
elif a[:2] == ["radio", "wifi"]:
    o("enabled\n")
EOF
printf '#!/bin/sh\nexit 1\n' > "$T/fakebin/busctl"
chmod 755 "$T/fakebin/nmcli" "$T/fakebin/busctl"
MYGROUP=$(id -gn)
state() { printf '%s\n' "$1" > "$T/state.json"; : > "$T/nm.log"; }
SCAN='"scan": [{"name": "Cafe Net", "signal": 82, "security": "WPA2"}, {"name": "Library", "signal": 55, "security": ""}, {"name": "Neighbour 5G", "signal": 30, "security": "WPA3"}]'
state "{$SCAN}"
# admin mode: the server reads this file per connection
echo no > "$T/admin"
cat > "$T/netd.py" <<EOF
import os, socket, subprocess, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind("$T/netd.sock"); s.listen(8)
while True:
    c, _ = s.accept()
    admin = open("$T/admin").read().strip() == "yes"
    env = dict(os.environ, PATH="$T/fakebin:" + os.environ["PATH"], FAKE_NM_LOG="$T/nm.log",
               FAKE_NM_STATE="$T/state.json", SG_NM_DIR="$T/nm", SG_WINE_GROUP="$MYGROUP",
               SG_ADMIN_GROUP="$MYGROUP" if admin else "sg-no-such-group")
    subprocess.Popen([sys.executable, "$NETCTL", "--serve"], stdin=c, stdout=c, env=env)
    c.close()
EOF
python3 "$T/netd.py" & SRV=$!
export SG_NETD_SOCKET="$T/netd.sock" SG_NETCTL="$NETCTL"
export WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all PATH="$WINE_DIR/bin:$PATH"
wine wineboot --init >/dev/null 2>&1; wineserver -w
bridged() { python3 "$NETCTL" --bridge wine "$@" 2>&1 >/dev/null | tr -d '\r'; }

# --- Network Connections -------------------------------------------------------------
out=$(bridged "$NCPA" --bridged --dump)
echo "$out" > "$OUT/net-ui-ncpa-dump.txt"
echo "$out" | grep -q '^TILE eth0|Ethernet|sgtest.lan|' && pass "a tile for the wired adapter, named Ethernet, on its network" || fail "ethernet tile: $(echo "$out" | grep TILE)"
echo "$out" | grep -q '^TILE wlan0|Wi-Fi|Not connected|' && pass "a tile for the Wi-Fi adapter, not connected" || fail "wifi tile: $(echo "$out" | grep TILE)"
for row in 'IPv4 Address|10.0.2.15' 'IPv4 Subnet Mask|255.255.255.0' 'DHCP Enabled|Yes' 'Physical Address|52-54-00-12-34-56' \
           'IPv4 Default Gateway|10.0.2.2' 'IPv4 DHCP Server|10.0.2.2' 'IPv4 DNS Servers|10.0.2.3' \
           'Connection-specific DNS Suffix|sgtest.lan' 'Link-local IPv6 Address|fe80::5054:ff:fe12:3456'; do
    echo "$out" | grep -qF "DETAIL eth0|$row" && pass "details: $row" || fail "details row missing: $row"
done
echo "$out" | grep -q '^DETAIL eth0|Lease Expires|.' && pass "details: the lease expiry, as a date" || fail "no lease expiry"

state "{$SCAN}"
out=$(bridged "$NCPA" --bridged --set-ipv4 eth0 static 10.0.2.50 255.255.255.0 10.0.2.2 10.0.2.3 1.1.1.1)
if echo "$out" | grep -q '^RESULT ERROR 3 You need to be an administrator' && ! grep -q '"modify"' "$T/nm.log"; then
    pass "a standard user's static address is refused, in words, and nothing is changed"
else fail "standard user: $out"; fi
echo yes > "$T/admin"; state "{$SCAN}"
out=$(bridged "$NCPA" --bridged --set-ipv4 eth0 static 10.0.2.50 255.255.255.0 10.0.2.2 10.0.2.3 1.1.1.1)
if echo "$out" | grep -q '^RESULT OK' && grep -qF '"modify", "uuid", "u-eth", "ipv4.method", "manual", "ipv4.addresses", "10.0.2.50/24", "ipv4.gateway", "10.0.2.2", "ipv4.dns", "10.0.2.3,1.1.1.1", "ipv4.ignore-auto-dns", "yes"' "$T/nm.log"; then
    pass "an administrator's static address, mask, gateway and DNS become exactly that profile"
else fail "admin static: $out / $(grep modify "$T/nm.log")"; fi
state "{$SCAN}"
out=$(bridged "$NCPA" --bridged --set-ipv4 eth0 auto)
grep -qF '"ipv4.method", "auto"' "$T/nm.log" && echo "$out" | grep -q '^RESULT OK' \
    && pass "Obtain an IP address automatically: back to DHCP" || fail "auto: $out"
for bad in "255.0.255.0|10.0.2.2|The subnet mask entered is not valid" "255.255.255.0|10.9.9.1|not on the same network segment" \
           "-|10.0.2.2|You must enter a subnet mask"; do
    mask=${bad%%|*}; rest=${bad#*|}; gw=${rest%%|*}; msg=${rest#*|}
    state "{$SCAN}"
    out=$(bridged "$NCPA" --bridged --set-ipv4 eth0 static 10.0.2.50 "$mask" "$gw" - -)
    if echo "$out" | grep -q "^RESULT ERROR 2 .*$msg" && ! grep -q '"modify"' "$T/nm.log"; then
        pass "refused before asking anyone: $msg"
    else fail "validation ($msg): $out"; fi
done

# --- the flyout -----------------------------------------------------------------------------
echo no > "$T/admin"; state "{$SCAN}"
out=$(bridged "$FLY" --bridged --dump)
echo "$out" > "$OUT/net-ui-flyout-dump.txt"
echo "$out" | grep -q '^WIRED Ethernet|connected' && pass "the flyout shows the wired connection" || fail "flyout wired: $out"
CAFE=$(printf 'Cafe Net' | od -An -tx1 | tr -d ' \n')
echo "$out" | grep -q "^NET $CAFE|Cafe Net|82|wpa-psk|Secured|new" && echo "$out" | grep -q '|Library|55|open|Open|' \
    && pass "the flyout lists the networks with signal and security" || fail "flyout nets: $out"
echo "$out" | grep -q '^TIP Network/Internet access' && pass "the icon's tooltip says how it is connected" || fail "tip: $(echo "$out" | grep TIP)"
printf 'a-good-key-123\n' > "$T/key"
out=$(bridged "$FLY" --bridged --connect "$CAFE" --password-file "Z:$T/key")
f=$(ls "$T/nm"/sg-wifi-*.nmconnection 2>/dev/null | head -1)
if echo "$out" | grep -q '^RESULT OK' && [ -n "$f" ] && grep -q '^psk=a-good-key-123$' "$f" && ! grep -q 'a-good-key' "$T/nm.log"; then
    pass "Connect: a standard user joins; the key is in the saved profile and on no command line"
else fail "connect: $out"; fi
rm -f "$T/nm"/sg-wifi-*
state "{$SCAN, \"up_fails\": true}"
out=$(bridged "$FLY" --bridged --connect "$CAFE" --password-file "Z:$T/key")
echo "$out" | grep -q "^RESULT ERROR 4 The network security key isn't correct" && [ -z "$(ls "$T/nm")" ] \
    && pass "a wrong key: \"The network security key isn't correct\", and not remembered" || fail "wrong key: $out"
state "{$SCAN}"

# --- the windows, through the programs' own re-launch -----------------------------------------
# Every program on the shell's virtual desktop, as the session sets it up
# (sg-session's sg-run-explorer); without it each window is a bare X window
# outside the desktop, with no caption and no taskbar under it.
wine reg add 'HKCU\Software\Wine\Explorer' /v Desktop /d shell /f >/dev/null 2>&1
wine reg add 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768 /f >/dev/null 2>&1
wineserver -w
rm -f "/tmp/.X${DPY}-lock"
Xvfb ":$DPY" -screen 0 1024x768x24 -ac >/dev/null 2>&1 & XP=$!; sleep 2
export DISPLAY=":$DPY"
wine explorer /desktop=shell,1024x768 >/dev/null 2>&1 &
sleep 6
winwait() { _w=0; while [ $_w -lt 30 ]; do xdotool search --name "$1" >/dev/null 2>&1 && return 0; sleep 1; _w=$((_w+1)); done; return 1; }
shot() { sleep 2; import -window root "$OUT/net-ui-$1.png" 2>/dev/null; }
closeall() { for w in $(xdotool search --name "$1" 2>/dev/null); do xdotool windowclose "$w" 2>/dev/null; done; sleep 1; }

wine "$NCPA" >/dev/null 2>&1 &
if winwait '^Network Connections$'; then pass "Network Connections opens (re-launched through the bridge)"; shot ncpa
else fail "no Network Connections window"; fi
closeall '^Network Connections$'
wine "$NCPA" --open ipv4 eth0 >/dev/null 2>&1 &
winwait 'TCP/IPv4' && { pass "the IPv4 dialog opens"; shot ipv4; } || fail "no IPv4 dialog"
wineserver -k; sleep 1; wine explorer /desktop=shell,1024x768 >/dev/null 2>&1 & sleep 5
wine "$NCPA" --open status eth0 >/dev/null 2>&1 &
winwait 'Ethernet Status' && { pass "the Status dialog opens"; shot status; } || fail "no Status dialog"
wineserver -k; sleep 1; wine explorer /desktop=shell,1024x768 >/dev/null 2>&1 & sleep 5
wine "$NCPA" --open details eth0 >/dev/null 2>&1 &
winwait 'Network Connection Details' && { pass "the Details dialog opens"; shot details; } || fail "no Details dialog"
wineserver -k; sleep 1; wine explorer /desktop=shell,1024x768 >/dev/null 2>&1 & sleep 5
wine "$FLY" --open >/dev/null 2>&1 &
winwait '^Network$' && { pass "the flyout opens above the taskbar"; shot flyout; } || fail "no flyout"
wineserver -k; sleep 1; wine explorer /desktop=shell,1024x768 >/dev/null 2>&1 & sleep 5
wine "$FLY" --open --expand "$CAFE" --prompt >/dev/null 2>&1 &
winwait '^Network$' && { sleep 3; shot flyout-key; pass "the flyout asks for the network security key"; } || fail "no flyout (key)"
for p in "$OUT"/net-ui-*.png; do
    n=$(python3 -c 'import sys; from PIL import Image; im = Image.open(sys.argv[1]).convert("RGB"); print(len(set(im.getdata())))' "$p" 2>/dev/null || echo 99)
    [ "${n:-0}" -gt 8 ] || fail "$(basename "$p") is blank ($n colours)"
done

echo
if [ "$RC" -eq 0 ]; then echo "RESULT: PASS"; else echo "RESULT: FAIL"; fi
exit "$RC"
