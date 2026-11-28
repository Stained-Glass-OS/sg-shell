#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate: "No internet access" where Windows shows it (net3, ledger i0972). A connection whose
# NCSI probe goes unanswered (a captive portal, a router with its line down; wine-sg 9661 probes
# as Windows' NCSI does and the Network List Manager follows) was "Internet access" for any
# connection with a gateway:
#   - the taskbar's network icon (sg-netflyout --dump): its tooltip "Network / No internet access",
#     back to "Internet access" when the probe page answers;
#   - Settings > Network & Internet > Status (sg-settings --page network-status, SG_SETTINGS_DUMP):
#     "No Internet access", else "You're connected to the Internet".
# In a private network and mount namespace of its own (sudo -n): a dummy Ethernet with a default
# route, the probe page on 127.0.0.1:80 (ActiveWebProbeHost 127.0.0.1) answering or not, a stand-in
# sg-netctl bridge (a wired adapter, connected). Needs a wine-sg with 9661 (SG_WINE_DIR), Xvfb.
#   mutants: SG_MUTANT_TRAY_ALWAYS_INTERNET (sg-netflyout.c), SG_MUTANT_SETTINGS_ROUTE_IS_INTERNET
#   (control/network.c): SG_FLYOUT_EXE / SG_SETTINGS_EXE run other builds.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
FLY="${SG_FLYOUT_EXE:-$HERE/build/sg-netflyout64.exe}"
SET="${SG_SETTINGS_EXE:-$HERE/build/sg-settings64.exe}"
for need in Xvfb python3 unshare setpriv ip; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$FLY" ] && [ -f "$SET" ] || { echo "SKIP: wine-sg or the programs missing"; exit 77; }
sudo -n true 2>/dev/null || { echo "SKIP: no sudo"; exit 77; }
W=$(mktemp -d /var/tmp/sg-nointernet.XXXXXX) || exit 1
chmod 777 "$W"
# shellcheck disable=SC2317
cleanup() { sudo -n rm -rf "$W"; }
trap cleanup EXIT INT TERM
mkdir -p "$W/bin"; cp "$FLY" "$W/bin/sg-netflyout64.exe"; cp "$SET" "$W/bin/sg-settings64.exe"
cat > "$W/netctl" <<'PY'
#!/usr/bin/python3
# stand-in sg-netctl: only the bridge, for a wired adapter that is connected
import json, os, subprocess, sys
a = sys.argv[1:]
cmd = a[a.index("--bridge") + 1:]
if cmd and cmd[0] == "wine":
    cmd[0] = os.environ["FAKE_WINE"]
rep = open(os.environ["FAKE_REPORT"], "ab")
p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=rep)
for line in p.stdout:
    try:
        argv = json.loads(line)["argv"]
    except Exception:
        argv = []
    if argv[:1] == ["adapters"]:
        out = "ADAPTER eth0\nTYPE ethernet\nSTATE connected\nEND\nOK\n"
    elif argv[:3] == ["wifi", "radio", "status"]:
        out = "RADIO off\nOK\n"
    else:
        out = "OK\n"
    p.stdin.write(out.encode()); p.stdin.flush()
p.wait()
PY
chmod 755 "$W/netctl"
cat > "$W/server.py" <<'PY'
import socket, sys
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("127.0.0.1", 80)); srv.listen(8)
while True:
    c, _ = srv.accept(); c.settimeout(5)
    try: c.recv(4096)
    except OSError: pass
    b = b"Microsoft Connect Test"
    c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\nConnection: close\r\n\r\n" % len(b) + b); c.close()
PY
cat > "$W/inner.sh" <<'INNER'
set -u
W=$1; WD=$2; U=$3; G=$4; N=$5; H=$6
mount -t tmpfs -o mode=1777,size=64m tmpfs /tmp || exit 1
mkdir -m 1777 /tmp/.X11-unix
ip link set lo up
ip link add sgeth0 type dummy && ip addr add 10.56.0.5/24 dev sgeth0 && ip link set sgeth0 up || { echo "FAIL  no dummy interface"; exit 1; }
ip route add default via 10.56.0.1 dev sgeth0
me() { setpriv --reuid="$U" --regid="$G" --init-groups -- env USER="$N" LOGNAME="$N" HOME="$H" "$@"; }
me Xvfb :4715 -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
sleep 1
export DISPLAY=:4715 WINEPREFIX="$W/pfx" WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d"
export SG_NETCTL="$W/netctl" FAKE_WINE="$WD/bin/wine" PATH="$WD/bin:$PATH"
K='HKLM\System\CurrentControlSet\Services\NlaSvc\Parameters\Internet'
me timeout -s KILL 300 wine wineboot -i >/dev/null 2>&1
me wine reg add "$K" /v ActiveWebProbeHost /d 127.0.0.1 /f >/dev/null 2>&1
me wineserver -k; me wineserver -w
# a server that stays, and nsiproxy (the TCP/IP driver, which probes) loaded by a program that asks it
me wineserver -p
me wine ipconfig >/dev/null 2>&1
probe_result() {   # wait until nsiproxy's probe says $1 (1 none, 3 internet)
    i=0
    while [ $i -lt 60 ]; do
        me wine reg query "$K" /v SgActiveProbeResultV4 2>/dev/null | tr -d '\r' | grep -q "0x$1\$" && return 0
        sleep 1; i=$((i + 1))
    done
    return 1
}
look() {   # look NAME: the tray's tooltip and Settings' status page
    rm -f "$W/report" "$W/dump"
    FAKE_REPORT="$W/report" me timeout -s KILL 60 wine "$W/bin/sg-netflyout64.exe" --dump >/dev/null 2>&1
    i=0; while ! grep -q '^TIP' "$W/report" 2>/dev/null && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done
    SG_SETTINGS_DUMP="$W/dump" me wine "$W/bin/sg-settings64.exe" --page network-status >/dev/null 2>&1 & SPID=$!
    i=0; while [ ! -s "$W/dump" ] && [ $i -lt 40 ]; do sleep 1; i=$((i + 1)); done
    sleep 1; kill $SPID 2>/dev/null
    grep '^TIP' "$W/report" | sed "s/^/      $1 tray: /"
    grep -i -E 'internet|connected' "$W/dump" | head -3 | sed "s/^/      $1 settings: /"
}
# nothing answers the probe: no internet
if probe_result 1; then
    look none
    grep -q '^TIP Network/No internet access' "$W/report" && echo "PASS  the tray says No internet access" ||
        echo "FAIL  the tray: $(grep TIP "$W/report")"
    grep -q 'No Internet access' "$W/dump" && echo "PASS  Settings' status says No Internet access" ||
        echo "FAIL  Settings' status: $(grep -i internet "$W/dump" | head -2 | tr '\n' ' ')"
else echo "FAIL  the NCSI probe never said 'no internet' (a wine-sg without 9661?)"; fi
# the probe page answers: the internet
python3 "$W/server.py" & SRV=$!
ip addr add 10.56.0.6/24 dev sgeth0
if probe_result 3; then
    look ok
    grep -q '^TIP Network/Internet access' "$W/report" && echo "PASS  and Internet access once the probe page answers" ||
        echo "FAIL  the tray: $(grep TIP "$W/report")"
    grep -q "You're connected to the Internet" "$W/dump" && echo "PASS  Settings: You're connected to the Internet" ||
        echo "FAIL  Settings' status: $(grep -i internet "$W/dump" | head -2 | tr '\n' ' ')"
else echo "FAIL  the NCSI probe never said 'internet'"; fi
kill $SRV $XP 2>/dev/null
me wineserver -k 2>/dev/null
INNER
out=$(sudo -n -E unshare -n -m --propagation private -- sh "$W/inner.sh" "$W" "$WINE_DIR" "$(id -u)" "$(id -g)" "$(id -un)" "$HOME")
printf '%s\n' "$out"
if printf '%s\n' "$out" | grep -q '^FAIL' || ! printf '%s\n' "$out" | grep -q '^PASS'; then echo "RESULT: FAIL"; exit 1; fi
echo "RESULT: PASS"
