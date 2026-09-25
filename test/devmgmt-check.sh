#!/bin/sh
# shellcheck disable=SC2046,SC2155,SC2086
# Gate for Device Manager (devmgmt.msc, sg-mmc), under Xvfb on a shell desktop,
# reading what it shows from SG_MMC_DUMP:
#
#   1. This machine, through the real sg-sysinfo bridge: devmgmt.msc opens
#      (wine-sg 0142's .msc file and mmc.exe launcher, else sg-mmc directly);
#      every display adapter and network adapter sg-sysinfo reports is listed
#      under Display adapters / Network adapters with its kernel driver;
#      Wine's own devices (SetupAPI) are there too; opening the display
#      adapter shows its Properties with its kernel module.
#   2. A made-up machine (sg-sysinfo's SG_SYSFS and stand-ins): a VM's display
#      adapter (bochs) and network adapter (virtio) are listed, and an NVIDIA
#      card with no driver is marked (the warning overlay, the banner, its
#      category opened by itself) and its Properties say "The drivers for this
#      device are not installed. (Code 28)" and name what sg-drivers would
#      install.
#   3. View > Devices by connection nests the network adapter under its PCI
#      bridge's parent tree.
#
# Screenshots: build/devmgmt-*.png. Needs wine-sg, Xvfb, xdotool,
# ImageMagick, python3; skips (77) without them. SG_MMC_EXE tests another
# build (mutants: -DSG_MUTANT_NOLINUX, -DSG_MUTANT_NOWARN); SG_WINE_DIR another
# Wine; SG_SYSINFO the sg-sysinfo (default ../sg-session/bin/sg-sysinfo).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_MMC_EXE:-$HERE/build/sg-mmc64.exe}"
OUT="$HERE/build"
SYSINFO="${SG_SYSINFO:-$HERE/../sg-session/bin/sg-sysinfo}"
[ -x "$SYSINFO" ] || SYSINFO=/usr/bin/sg-sysinfo
SYSINFO=$(readlink -f "$SYSINFO")
RC=0; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in systemd-socket-activate Xvfb xdotool import python3; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
if [ -x "$WINE_DIR/bin/wine" ]; then WBIN="$WINE_DIR/bin"; WSERVER="$WINE_DIR/bin/wineserver"
elif [ -x "$WINE_DIR/wine" ]; then WBIN="$WINE_DIR"; WSERVER="$WINE_DIR/server/wineserver"
else echo "SKIP: no wine in $WINE_DIR"; exit 77; fi
[ -f "$EXE" ] && [ -x "$SYSINFO" ] || { echo "SKIP: $EXE or sg-sysinfo missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-devmgmt-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WSERVER" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT INT TERM

Xvfb -displayfd 3 -screen 0 1280x800x24 -nolisten tcp 3>"$T/dpy" >/dev/null 2>&1 & XP=$!
i=0; while [ ! -s "$T/dpy" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
export DISPLAY=":$(cat "$T/dpy")" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WBIN:$PATH" SG_SYSINFO="$SYSINFO"
DUMP="$T/dump.txt"
wine wineboot --init >/dev/null 2>&1; "$WSERVER" -w
C="$WINEPREFIX/drive_c"
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
windump=$(wine winepath -w "$DUMP" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1280x800
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\mmc.exe' /ve /d "$winexe"
for f in "$HERE"/theme/5[0-2]-*.reg; do wine regedit /S "Z:$(printf %s "$f" | tr / '\\')" >/dev/null 2>&1; done
"$WSERVER" -w
export SG_MMC_DUMP="$windump"
wine explorer /desktop=shell,1280x800 > "$T/explorer.out" 2>&1 &
sleep 4

d() { [ -f "$DUMP" ] && tr -d '\r' < "$DUMP"; }
wait_dump() { i=0; while ! d | grep -Eq "$1" && [ $i -lt $(( ${2:-10} * 2 )) ]; do sleep 0.5; i=$((i + 1)); done; d | grep -Eq "$1"; }
dev_line() { d | awk -F'\t' -v n="$1" '$1 ~ /^DEV / && $4 == n { print; exit }'; }
dev_xy() { dev_line "$1" | awk -F'\t' '{ print $2 }'; }
cat_xy() { d | awk -v n="$1" '$1 == "CATEGORY" { t = $0; sub(/^CATEGORY -?[0-9]+ -?[0-9]+ /, "", t); if (t == n) { print $2, $3; exit } }'; }
dbl() { [ $# -ge 2 ] || return 0; xdotool mousemove "$1" "$2" click --repeat 2 --delay 120 1; sleep 1.2; }
shot() { import -window root "$OUT/devmgmt-$1.png" 2>/dev/null; }
open_console() {
    rm -f "$DUMP"
    if [ -f "$C/windows/system32/devmgmt.msc" ]; then wine start devmgmt.msc >/dev/null 2>&1 &
    else wine "$winexe" devmgmt >/dev/null 2>&1 & fi
    wait_dump '^CONSOLE devmgmt' 30 && wait_dump '^DEV ' 20
}
close_console() { wine taskkill /f /im sg-mmc64.exe >/dev/null 2>&1; sleep 1; }

# ---- 1. this machine ----------------------------------------------------------------
"$SYSINFO" devices > "$T/devices.txt" 2>/dev/null
names_of() { awk -v c="$1" '/^DEVICE /{n=""; cls=""} /^CLASS /{cls=$2} /^NAME /{sub(/^NAME /,""); n=$0} /^END$/{ if (cls == c) print n }' "$T/devices.txt"; }
if open_console; then pass "devmgmt.msc opens Device Manager$([ -f "$C/windows/system32/devmgmt.msc" ] || echo ' (sg-mmc directly: no devmgmt.msc in this Wine)')"
else fail "Device Manager did not open"; fi
d | grep -q '^BRIDGED 1' && pass "bridged to sg-sysinfo" || fail "not bridged to sg-sysinfo"
shot machine
for cls in display net; do
    cat=$([ $cls = display ] && echo "Display adapters" || echo "Network adapters")
    names_of $cls > "$T/want"
    if [ ! -s "$T/want" ]; then echo "NOTE  this machine has no $cls device for sg-sysinfo; the made-up machine checks it"; continue; fi
    while IFS= read -r n; do
        line=$(dev_line "$n")
        drv=$(printf %s "$line" | awk -F'\t' '{ print $7 }')
        case "$line" in *"	$cat	$n	"*) [ -n "$drv" ] && pass "$cat: $n, driver $drv" || fail "$cat: $n has no driver shown" ;;
                        *) fail "$cat: '$n' not listed there ($line)" ;; esac
    done < "$T/want"
done
d | grep -q '	wine	' && pass "Wine's own devices (SetupAPI) are listed too" || fail "no SetupAPI device listed"
first=$(names_of display | head -1)
if [ -n "$first" ]; then
    dbl $(cat_xy 'Display adapters'); sleep 0.5
    dbl $(dev_xy "$first")
    if wait_dump "^PROPS $first" 5; then
        mod=$(awk -v n="$first" '/^DEVICE /{hit=0} /^NAME /{sub(/^NAME /,""); hit=($0==n)} hit && /^MODULE /{print $2; exit}' "$T/devices.txt")
        d | grep -q "^PROPMODULE $mod" && pass "Properties of $first: kernel module $mod" || fail "Properties' module: $(d | grep '^PROPMODULE') want $mod"
        shot properties
        xdotool key Escape; sleep 1
    else fail "the display adapter's Properties did not open"; fi
fi
close_console

# ---- 2. a made-up machine: a VM with an NVIDIA card and no driver ---------------------------------
F="$T/fake"
python3 - "$F" <<'EOF'
import os, sys
root = sys.argv[1]
def write(p, t):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "w").write(t)
def link(t, p):
    os.makedirs(os.path.dirname(p), exist_ok=True)
    os.symlink(t, p)
dev = os.path.join(root, "sys/devices/pci0000:00")
def pci(slot, vendor, device, cls, driver=None, module=None):
    p = os.path.join(dev, slot)
    for k, v in (("vendor", vendor), ("device", device), ("class", cls)):
        write(os.path.join(p, k), "0x%s\n" % v)
    link("../../../devices/pci0000:00/" + slot, os.path.join(root, "sys/bus/pci/devices", slot))
    if driver:
        d = os.path.join(root, "sys/bus/pci/drivers", driver)
        os.makedirs(d, exist_ok=True)
        link(os.path.relpath(d, p), os.path.join(p, "driver"))
        m = os.path.join(root, "sys/module", module)
        write(os.path.join(m, "version"), "9.9\n")
        if not os.path.lexists(os.path.join(d, "module")):
            link(os.path.relpath(m, d), os.path.join(d, "module"))
    return p
pci("0000:00:01.0", "1234", "1111", "030000", "bochs-drm", "bochs")
pci("0000:01:00.0", "10de", "1c82", "030000")
net = pci("0000:00:03.0", "1af4", "1041", "020000", "virtio-pci", "virtio_pci")
write(os.path.join(net, "driver_override"), "(null)\n")
write(os.path.join(root, "sys/bus/pci/drivers/virtio-pci/unbind"), "")
write(os.path.join(root, "sys/bus/pci/drivers_probe"), "")
vn = os.path.join(net, "virtio0")
write(os.path.join(vn, "net/eth0/address"), "52:54:00:12:34:56\n")
link("../../devices/pci0000:00/0000:00:03.0/virtio0/net/eth0", os.path.join(root, "sys/class/net/eth0"))
link(os.path.relpath(vn, os.path.join(vn, "net/eth0")), os.path.join(vn, "net/eth0/device"))
write(os.path.join(root, "pci.ids"), "1234  Technical Corp.\n\t1111  QEMU Virtual Video Controller\n"
      "10de  NVIDIA Corporation\n\t1c82  GP107 [GeForce GTX 1050 Ti]\n1af4  Red Hat, Inc.\n\t1041  Virtio 1.0 network device\n")
write(os.path.join(root, "drivers"), "#!/bin/sh\n[ \"$1\" = --list ] && printf 'DEVICE 0000:01:00.0\\t10de:1c82\\tNVIDIA graphics\\tnvidia-driver firmware-misc-nonfree\\tNVIDIA\\047s driver\\n'\nexit 0\n")
write(os.path.join(root, "dpkg-query"), "#!/bin/sh\nexit 1\n")
os.chmod(os.path.join(root, "drivers"), 0o755)
os.chmod(os.path.join(root, "dpkg-query"), 0o755)
EOF
export SG_SYSFS="$F/sys" SG_PCI_IDS="$F/pci.ids" SG_USB_IDS="$F/none" SG_UDEV_DATA="$F/none" SG_DRIVERS="$F/drivers" SG_DPKG_QUERY="$F/dpkg-query"
if open_console; then pass "Device Manager on a made-up machine (SG_SYSFS)"; else fail "Device Manager did not open on the made-up machine"; fi
line=$(d | awk -F'\t' '$1 ~ /^DEV / && $3 == "Display adapters" && $4 ~ /QEMU/ { print; exit }')
case "$line" in *"	ok	bochs-drm	"*) pass "Display adapters: the VM's display adapter, driver bochs-drm" ;; *) fail "VM display adapter: '$line'" ;; esac
line=$(d | awk -F'\t' '$1 ~ /^DEV / && $3 == "Network adapters" && $4 ~ /[Vv]irtio/ { print; exit }')
case "$line" in *"	ok	virtio-pci	"*) pass "Network adapters: the virtio network adapter, driver virtio-pci" ;; *) fail "VM network adapter: '$line'" ;; esac
nv=$(d | awk -F'\t' '$1 ~ /^DEV / && $3 == "Display adapters" && $4 ~ /NVIDIA/ { print; exit }')
case "$nv" in *"	nodriver	"*) pass "the NVIDIA card is marked: no driver" ;; *) fail "NVIDIA card: '$nv'" ;; esac
nvname=$(printf %s "$nv" | awk -F'\t' '{ print $4 }')
nvxy=$(printf %s "$nv" | awk -F'\t' '{ print $2 }')
[ "$nvxy" != "-1 -1" ] && [ -n "$nvxy" ] && pass "its category is opened by itself (the device is visible)" || fail "the no-driver device is not visible ($nvxy)"
d | grep -q '^BANNER One device has no driver' && pass "the banner says a device has no driver" || fail "banner: $(d | grep '^BANNER')"
shot nodriver
dbl $nvxy
if wait_dump "^PROPS $nvname" 5; then
    d | grep -q '^PROPSTATUS The drivers for this device are not installed. (Code 28).*nvidia-driver' \
        && pass "its Properties: Code 28 and sg-drivers' nvidia-driver" || fail "status: $(d | grep '^PROPSTATUS')"
    shot nodriver-properties
    xdotool key Escape; sleep 1
else fail "the NVIDIA card's Properties did not open"; fi

# ---- 3. Devices by connection ---------------------------------------------------------------------
ac=$(d | awk '$1 == "LINK" && $2 == 0 { t = $0; sub(/^LINK 0 -?[0-9]+ -?[0-9]+ /, "", t); if (t == "Devices by connection") { print $3, $4; exit } }')
[ -n "$ac" ] && xdotool mousemove $ac click 1; sleep 1.5
wait_dump '^BYCONNECTION 1' 5 && ! d | grep -q '^CATEGORY' && pass "Devices by connection: no type categories, devices under their parents" \
    || fail "by connection: $(d | grep -cE '^CATEGORY') categories"
shot connection
close_console

# ---- 4. Disable device / Enable device, through sg-sysinfod -------------------------------------------
link_xy() { d | awk -v n="$1" '$1 == "LINK" && $2 == 0 { t = $0; sub(/^LINK 0 -?[0-9]+ -?[0-9]+ /, "", t); if (t == n) { print $3, $4; exit } }'; }
export SG_SYSINFO_SOCKET="$T/sysinfod.sock" SG_SYSINFO_STATE="$T/state"
SP=""
serve() {
    [ -n "$SP" ] && kill "$SP" 2>/dev/null; sleep 0.5; rm -f "$SG_SYSINFO_SOCKET"
    SG_ADMIN_GROUP="$1" SG_WINE_GROUP="$(id -gn)" systemd-socket-activate -l "$SG_SYSINFO_SOCKET" --inetd -a \
        -E SG_ADMIN_GROUP -E SG_WINE_GROUP -E SG_SYSFS -E SG_PCI_IDS -E SG_USB_IDS -E SG_UDEV_DATA -E SG_DRIVERS \
        -E SG_DPKG_QUERY -E SG_SYSINFO_STATE -E PATH "$SYSINFO" --serve >/dev/null 2>&1 &
    SP=$!
    i=0; while [ ! -S "$SG_SYSINFO_SOCKET" ] && [ $i -lt 20 ]; do sleep 0.2; i=$((i + 1)); done
}
OVR="$F/sys/devices/pci0000:00/0000:00:03.0/driver_override"
vnet() { d | awk -F'\t' '$1 ~ /^DEV / && $3 == "Network adapters" && $4 ~ /[Vv]irtio/ { print; exit }'; }
serve sg-nobody-here
open_console
dbl $(cat_xy 'Network adapters'); sleep 0.5
xy=$(vnet | awk -F'\t' '{ print $2 }')
# shellcheck disable=SC2086
[ -n "$xy" ] && xdotool mousemove $xy click 1; sleep 1
lx=$(link_xy 'Disable device')
[ -n "$lx" ] && pass "a PCI device offers Disable device" || fail "no Disable device link: $(d | grep '^LINK')"
# shellcheck disable=SC2086
xdotool mousemove $lx click 1; sleep 1
wait_dump '^MSG Disabling this device will cause it to stop functioning' 5 && pass "Disable asks first" || fail "no question: $(d | grep '^MSG')"
xdotool key y; sleep 2
if wait_dump '^MSG You must be an administrator to disable this device' 5 && [ "$(cat "$OVR")" = "(null)" ]; then
    pass "a standard user is refused (the administrator message), the device untouched"
else fail "standard user: $(d | grep '^MSG'), override '$(cat "$OVR")'"; fi
shot disable-refused
xdotool key Return; sleep 1
close_console
serve "$(id -gn)"
open_console
dbl $(cat_xy 'Network adapters'); sleep 0.5
xy=$(vnet | awk -F'\t' '{ print $2 }')
# shellcheck disable=SC2086
xdotool mousemove $xy click 1; sleep 1
# shellcheck disable=SC2046
xdotool mousemove $(link_xy 'Disable device') click 1; sleep 1
xdotool key y; sleep 3
case "$(vnet)" in *"	disabled	"*) pass "an administrator disables it: the row says disabled" ;; *) fail "not disabled: $(vnet)" ;; esac
[ "$(cat "$OVR")" = sg-disabled ] && [ "$(cat "$F/sys/bus/pci/drivers/virtio-pci/unbind")" = 0000:00:03.0 ] \
    && pass "sg-sysinfod unbound it (driver_override sg-disabled)" || fail "sysfs: '$(cat "$OVR")'"
[ -n "$(link_xy 'Enable device')" ] && pass "a disabled device offers Enable device" || fail "no Enable link: $(d | grep '^LINK')"
shot disabled
xy=$(vnet | awk -F'\t' '{ print $2 }')
dbl $xy
if wait_dump '^PROPSTATUS This device is disabled. \(Code 22\)' 5; then pass "its Properties: This device is disabled. (Code 22)"
else fail "status: $(d | grep '^PROPSTATUS')"; fi
shot disabled-properties
xdotool key Escape; sleep 1
# shellcheck disable=SC2086
xdotool mousemove $xy click 1; sleep 1
# shellcheck disable=SC2046
xdotool mousemove $(link_xy 'Enable device') click 1; sleep 3
case "$(vnet)" in *"	ok	"*) [ "$(cat "$OVR")" = "" ] || [ "$(cat "$OVR")" = "$(printf '\n')" ]
    pass "Enable device: working again, the override cleared" ;; *) fail "not enabled: $(vnet)" ;; esac
close_console
kill "$SP" 2>/dev/null

[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
