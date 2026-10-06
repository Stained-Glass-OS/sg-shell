#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for the printers' pages: Settings > Devices > Printers & scanners and
# the Control Panel's Devices and Printers (src/control/set_printers.c).
#
# A CUPS server of the gate's own has a label printer and an office printer;
# a stand-in sg-settingsctl (SG_SETTINGSCTL) gives their CUPS queues as the
# real one does, including the label printer's "(maker's driver)" queue for
# Linux programs. Checked through sg-control --dump printers, as the pages
# list them:
#   - each Windows printer once, by the name people know it by (CUPS's
#     description, not the queue's name with underscores), one the default;
#   - the printer's CUPS queues for Linux programs under it -- its own (the
#     Linux driver) and the maker's-driver one -- so one physical printer is
#     one entry;
#   - the status is the printer's: a queue paused with cupsdisable is
#     "Paused" (needs wine-sg 1222), and Ready again with cupsenable;
#   - the Settings window opened on ms-settings:printers shows the page
#     (SG_SETTINGS_DUMP), and Control Panel's "printers" name opens Devices
#     and Printers (--resolve).
#
#   SG_WINE_DIR=/opt/wine-sg test/printers-check.sh   (WINE=... another wine)
# Mutation: built with -DSG_MUTANT_PRINTERS_LINUX or -DSG_MUTANT_PRINTERS_STATUS
# (SG_CONTROL_EXE / SG_SETTINGS_EXE name those builds) the gate fails.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
WINE="${WINE:-$WINE_DIR/bin/wine}"
WINE="$(cd "$(dirname "$WINE")" && pwd)/$(basename "$WINE")"
WINESERVER="${WINESERVER:-$(dirname "$WINE")/wineserver}"
[ -x "$WINESERVER" ] || WINESERVER="$(dirname "$WINE")/server/wineserver"
CTL="${SG_CONTROL_EXE:-$HERE/build/sg-control64.exe}"
SET="${SG_SETTINGS_EXE:-$HERE/build/sg-settings64.exe}"
CUPSD="${CUPSD:-/usr/sbin/cupsd}"
RC=0; CP=""; XP=""; DPY="${SG_PRINTERS_DPY:-117}"
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
[ -x "$WINE" ] && [ -f "$CTL" ] || { echo "SKIP: wine-sg or sg-control not built"; exit 77; }
[ -x "$CUPSD" ] && [ -x /usr/sbin/lpadmin ] || { echo "SKIP: no cupsd/lpadmin"; exit 77; }
unset DISPLAY WAYLAND_DISPLAY
T=$(mktemp -d /var/tmp/sg-printers-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    "$WINESERVER" -k 2>/dev/null
    [ -n "$CP" ] && { kill "$CP" 2>/dev/null; wait "$CP" 2>/dev/null; }
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM
# no display, not the session's Wayland compositor either
mkdir -p -m 700 "$T/xdg"
export XDG_RUNTIME_DIR="$T/xdg" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDEBUG=-all WINESERVER
export WINEDLLOVERRIDES="mscoree,mshtml=;winemenubuilder.exe=d" CUPS_SERVER="$T/cups.sock" CUPS_SERVERROOT="$T/root"

# ---- a CUPS server: a label printer and an office printer --------------------------------
mkdir -p "$T/root/ppd" "$T/cache" "$T/state" "$T/spool" "$T/log"
cat > "$T/root/cupsd.conf" <<CONF
Listen $T/cups.sock
LogLevel warn
<Location />
Order allow,deny
Allow all
</Location>
CONF
cat > "$T/root/cups-files.conf" <<CONF
ServerRoot $T/root
CacheDir $T/cache
StateDir $T/state
RequestRoot $T/spool
TempDir $T/spool
ErrorLog $T/log/error_log
AccessLog $T/log/access_log
PageLog $T/log/page_log
ServerBin /usr/lib/cups
DataDir /usr/share/cups
FileDevice Yes
User $(id -un)
Group $(id -gn)
CONF
cat > "$T/root/printers.conf" <<CONF
<DefaultPrinter Label_Printer>
Info Label Printer 550
MakeModel SG Label Printer
DeviceURI file:///dev/null
State Idle
Accepting Yes
</DefaultPrinter>
<Printer Office>
Info Office Laser
MakeModel SG Office Laser
DeviceURI file:///dev/null
State Idle
Accepting Yes
</Printer>
CONF
for q in Label_Printer Office; do
cat > "$T/root/ppd/$q.ppd" <<PPD
*PPD-Adobe: "4.3"
*FormatVersion: "4.3"
*FileVersion: "1.0"
*LanguageVersion: English
*LanguageEncoding: ISOLatin1
*PCFileName: "SGTEST.PPD"
*Manufacturer: "Stained Glass"
*Product: "(SG $q)"
*ModelName: "SG $q"
*ShortNickName: "SG $q"
*NickName: "SG $q"
*PSVersion: "(3010.000) 0"
*LanguageLevel: "3"
*ColorDevice: False
*DefaultColorSpace: Gray
*FileSystem: False
*Throughput: "1"
*LandscapeOrientation: Plus90
*TTRasterizer: Type42
*OpenUI *PageSize/Media Size: PickOne
*OrderDependency: 10 AnySetup *PageSize
*DefaultPageSize: Letter
*PageSize Letter/Letter: "<</PageSize[612 792]/ImagingBBox null>>setpagedevice"
*CloseUI: *PageSize
*OpenUI *PageRegion: PickOne
*OrderDependency: 10 AnySetup *PageRegion
*DefaultPageRegion: Letter
*PageRegion Letter/Letter: "<</PageSize[612 792]/ImagingBBox null>>setpagedevice"
*CloseUI: *PageRegion
*DefaultImageableArea: Letter
*ImageableArea Letter/Letter: "18 18 594 774"
*DefaultPaperDimension: Letter
*PaperDimension Letter/Letter: "612 792"
*DefaultFont: Courier
*Font Courier: Standard "(002.004S)" Standard ROM
PPD
done
"$CUPSD" -f -c "$T/root/cupsd.conf" -s "$T/root/cups-files.conf" > "$T/cupsd.out" 2>&1 & CP=$!
i=0; while [ ! -S "$T/cups.sock" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i + 1)); done
lpstat -v 2>/dev/null | grep -q Office || { echo "SKIP: the gate's CUPS server did not start"; exit 77; }

# ---- the native half: CUPS's queues, as sg-settingsctl printers answers --------------------
cat > "$T/sg-settingsctl" <<'CTLS'
#!/bin/sh
out=""; while [ $# -gt 0 ]; do [ "$1" = --out ] && { out=$2; shift; }; shift; done
{
printf 'QUEUE Label_Printer\tSG Label Printer\tusb://SG/Label?serial=1\tidle\tLabel Printer 550\n'
printf "QUEUE Label_Printer_MakersDriver\tLabel Printer 550 (maker's driver)\tsgwindrv:/Label_Printer\tidle\tLabel Printer 550 (maker's driver)\n"
printf 'QUEUE Office\tSG Office Laser\tipp://office/ipp/print\tidle\tOffice Laser\n'
echo OK
} > "$out"
CTLS
chmod 755 "$T/sg-settingsctl"
export SG_SETTINGSCTL="$T/sg-settingsctl"

mkdir -p "$WINEPREFIX"
timeout -s KILL 300 "$WINE" wineboot --init >/dev/null 2>&1; "$WINESERVER" -w
cp "$CTL" "$T/sg-control64.exe"
ctl() { (cd "$T" && timeout 120 "$WINE" "$T/sg-control64.exe" "$@" 2>/dev/null </dev/null | tr -d '\r'); }

out=$(ctl --dump printers)
printf '%s\n' "$out" | sed 's/^/      /'
lab=$(printf '%s\n' "$out" | grep '^printer=Label_Printer|')
off=$(printf '%s\n' "$out" | grep '^printer=Office|')
[ "$(printf '%s\n' "$out" | grep -c '^printer=')" = 2 ] && pass "each printer once (2)" || fail "printers listed: $(printf '%s\n' "$out" | grep -c '^printer=')"
case "$lab" in "printer=Label_Printer|Label Printer 550|Default, Ready|"*) pass "the label printer by its description, the default, ready" ;;
    *) fail "the label printer: '$lab'" ;; esac
case "$off" in "printer=Office|Office Laser|Ready|"*) pass "the office printer by its description, ready" ;;
    *) fail "the office printer: '$off'" ;; esac
lin=$(printf '%s\n' "$out" | sed -n 's/^linux=Label_Printer|//p')
case "$lin" in *"Label Printer 550"*"(the Linux driver)"*"Label Printer 550 (maker's driver)"*"(the maker's Windows driver)"*)
        pass "its CUPS queues for Linux programs are under it: $lin" ;;
    *) fail "the label printer's Linux queues: '$lin'" ;; esac
case "$(printf '%s\n' "$out" | sed -n 's/^linux=Office|//p')" in *"Office Laser"*"(the Linux driver)"*) pass "the office printer's queue under it" ;;
    *) fail "the office printer's Linux queue is not listed" ;; esac

/usr/sbin/cupsdisable Office 2>/dev/null; sleep 2.2
off=$(ctl --dump printers | grep '^printer=Office|')
case "$off" in "printer=Office|Office Laser|Paused|"*) pass "a paused printer says so ($off)" ;;
    *) fail "a paused printer: '$off'" ;; esac
/usr/sbin/cupsenable Office 2>/dev/null; sleep 2.2
off=$(ctl --dump printers | grep '^printer=Office|')
case "$off" in "printer=Office|Office Laser|Ready|"*) pass "ready again" ;; *) fail "after cupsenable: '$off'" ;; esac

# the names that open the pages
for n in printers Microsoft.DevicesAndPrinters; do
    r=$(ctl --resolve $n 2>/dev/null)
    [ "$r" = "page=Devices and Printers" ] && pass "control $n opens Devices and Printers" || fail "control $n: '$r'"
done

# ---- the Settings page, in its window ---------------------------------------------------
if command -v Xvfb >/dev/null && [ -f "$SET" ]; then
    Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
    sleep 1
    cp "$SET" "$T/sg-settings64.exe"
    (cd "$T" && DISPLAY=":$DPY" SG_SETTINGS_DUMP="$T/settings.dump" timeout 60 "$WINE" "$T/sg-settings64.exe" ms-settings:printers \
        >/dev/null 2>&1 &)
    i=0; while ! grep -q "Label Printer 550" "$T/settings.dump" 2>/dev/null && [ $i -lt 60 ]; do sleep 1; i=$((i + 1)); done
    d=$(iconv -f UTF-8 -t UTF-8 "$T/settings.dump" 2>/dev/null | tr -d '\r')
    if printf '%s\n' "$d" | grep -q "Printers & scanners" && printf '%s\n' "$d" | grep -q "Label Printer 550" &&
       printf '%s\n' "$d" | grep -q "Office Laser"; then
        pass "ms-settings:printers shows Printers & scanners with both printers"
    else
        fail "the Settings page: $(printf '%s' "$d" | head -c 400)"
    fi
else
    echo "      (no Xvfb or sg-settings: the Settings window is not checked)"
fi
[ $RC = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit $RC
