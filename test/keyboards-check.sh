#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# Gate for Settings > Time & Language > Language (src/control/set_language.c): the person's keyboards, as
# Windows keeps them -- added, put in order and removed in Settings, switched with the Start key + Space
# (and Left Alt + Shift when chosen), at once and without signing out.
#
#   - ms-settings:regionlanguage and ms-settings:keyboard open the page; it lists the session's keyboards
#     (a stand-in sg-settingsctl's "keyboard": US and US Dvorak, a Compose option) by their Windows names
#   - Add a keyboard: German is given to the session ("keyboard set us,us,de ,dvorak, ..."), its variants
#     and the keymap's other options kept, the Start key + Space switching; HKCU\Keyboard Layout\Preload
#     lists the three, German (another language's keyboard) through Substitutes, as Windows writes them
#   - Move up puts German first; Remove takes Dvorak away; Preload follows
#   - Left Alt + Shift: grp:alt_shift_toggle, and Toggle's Hotkey / Language Hotkey "1"
#   - no compositor to tell (APPLIED no): the page says the keyboards change at the next sign-in
#
# Needs wine-sg, Xvfb, xdotool; skips (77) without them. SG_SETTINGS_EXE runs another build (the
# mutants: SG_MUTANT_KBD_NO_SUBST, SG_MUTANT_KBD_DROP_OPTIONS), SG_WINE_DIR another Wine.
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_SETTINGS_EXE:-$HERE/build/sg-settings64.exe}"
RC=0; DPY="${SG_KEYBOARDS_DPY:-121}"; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
while [ -e "/tmp/.X${DPY}-lock" ] || [ -e "/tmp/.X11-unix/X${DPY}" ]; do DPY=$((DPY + 1)); done

T=$(mktemp -d /var/tmp/sg-keyboards-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    [ "${KEEP:-0}" = 1 ] && echo "kept $T" || rm -rf "$T"
}
trap cleanup EXIT INT TERM

Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH" LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
wine wineboot --init >/dev/null 2>&1; wineserver -w
cp "$EXE" "$T/sg-settings64.exe"
winexe=$(wine winepath -w "$T/sg-settings64.exe" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
regq() { wine reg query "$1" /v "$2" 2>/dev/null | tr -d '\r' | grep -F "    $2    REG_" | sed 's/.*    REG_[A-Z_]*    //'; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
for f in "$HERE/theme/50-sg-colors.reg" "$HERE/theme/52-sg-fonts.reg"; do
    wine reg import "$(wine winepath -w "$f" | tr -d '\r')" >/dev/null 2>&1
done
esc=$(printf '%s' "$winexe" | sed 's/\\/\\\\\\\\/g')
sed "s|Z:\\\\\\\\usr\\\\\\\\libexec\\\\\\\\stained-glass\\\\\\\\shell\\\\\\\\sg-settings64.exe|$esc|g" \
    "$HERE/defaults/65-sg-settings.reg" > "$T/settings.reg"
wine reg import "$(wine winepath -w "$T/settings.reg" | tr -d '\r')" >/dev/null 2>&1
wineserver -w

# the native half: a stand-in sg-settingsctl keeping the session's keyboards in $T/kbd (layouts, variants,
# options: what the compositor would have) and logging what it is asked
CTL="$T/sg-settingsctl"; LOG="$T/ctl.log"; : > "$LOG"
printf 'us,us\t,dvorak\tcompose:ralt,grp:win_space_toggle\n' > "$T/kbd"
cat > "$CTL" <<EOF
#!/bin/sh
out=""; args=""
while [ \$# -gt 0 ]; do
    case "\$1" in --out) out="\$2"; shift 2 ;; *) args="\$args \$1"; shift ;; esac
done
echo "\${args# }" >> "$LOG"
set -- \$args
{
case "\$1" in
keyboard) if [ "\${2:-}" = set ]; then
        v=\$4; [ "\$4" = - ] && v=; o=\$5; [ "\$5" = - ] && o=
        printf '%s\t%s\t%s\n' "\$3" "\$v" "\$o" > "$T/kbd"
        if [ -e "$T/no-compositor" ]; then echo 'APPLIED no'; else echo 'APPLIED yes'; fi
    else
        printf 'KEYBOARD %s\tno\nCURRENT 0\n' "\$(cat "$T/kbd")"
    fi; echo OK ;;
*) echo 'ERROR invalid usage' ;;
esac
} > "\$out.part"
mv "\$out.part" "\$out"
EOF
chmod 755 "$CTL"
export SG_SETTINGSCTL="$CTL"

resolve() { wine "$T/sg-settings64.exe" --resolve "$1" 2>/dev/null | tr -d '\r' | sed -n 's/^page=//p'; }
for uri in ms-settings:regionlanguage ms-settings:keyboard; do
    got=$(resolve "$uri")
    [ "$got" = Language ] && pass "$uri -> Language" || fail "$uri -> '$got', want 'Language'"
done

wine explorer /desktop=shell,1024x768 >/dev/null 2>&1 &
sleep 4
DUMP="$T/settings.dump"
SG_SETTINGS_DUMP="$(wine winepath -w "$DUMP" | tr -d '\r')"; export SG_SETTINGS_DUMP
D() { tr -d '\r' < "$DUMP" 2>/dev/null | sed "s/^\xEF\xBB\xBF//" | sed -n "s/^$1 //p" | head -1; }
has() { tr -d '\r' < "$DUMP" 2>/dev/null | grep -qF -- "$1"; }
texts() { tr -d '\r' < "$DUMP" 2>/dev/null | sed -n 's/^text //p' | tr '\n' '|'; }
at_id() {   # the screen centre of the page's control with id $1
    tr -d '\r' < "$DUMP" | grep "^control .* id=$1 " | head -1 | sed -n 's/.* at=\([0-9-]*\),\([0-9-]*\).*/\1 \2/p'
}
click_id() { set -- $(at_id "$1"); [ $# -eq 2 ] && { xdotool mousemove "$1" "$2" click 1; sleep 0.3; }; }
# wait until the stand-in has been told something new, and the page is built again
told() { i=0; while [ "$(grep -c '^keyboard set' "$LOG")" -lt "$1" ] && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
         sleep 1.5; grep '^keyboard set' "$LOG" | sed -n "${1}p"; }
preload() { for n in 1 2 3 4; do v=$(regq 'HKCU\Keyboard Layout\Preload' $n); [ -n "$v" ] && printf '%s=%s ' $n "$v"; done; }
CMD=2000

wine start ms-settings:regionlanguage >/dev/null 2>&1
i=0; while [ "$(D page)" != Language ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
[ "$(D page)" = Language ] && pass "ms-settings:regionlanguage opens Language" || { fail "the page is '$(D page)'"; exit 1; }
sleep 1
case "$(texts)" in *"|US|Default keyboard • Typing now|United States-Dvorak|"*) pass "the session's keyboards by their Windows names: US (default, typing now), United States-Dvorak" ;;
     *) fail "the keyboards listed: $(texts)" ;; esac
has "id=$((CMD + 20)) state=0 at=" && tr -d '\r' < "$DUMP" | grep -q "id=$((CMD + 20)) .* disabled: Move up" \
    && pass "the first keyboard cannot move up" || fail "Move up of the first keyboard is not disabled"

# --- Add a keyboard: German ---
set -- $(at_id $((CMD + 1)))
# the list closed again, its focus kept: a letter chooses the first keyboard by it
xdotool mousemove "$1" "$2" click 1; sleep 0.6; xdotool key Escape; sleep 0.3; xdotool key Home; sleep 0.3; xdotool key g
got=$(told 1)
[ "$got" = "keyboard set us,us,de ,dvorak, compose:ralt,grp:win_space_toggle" ] \
    && pass "Add a keyboard: German, to the session with Dvorak and Compose kept ($got)" || fail "Add German told: '$got'"
p=$(preload)
[ "$p" = "1=00000409 2=d0010409 3=d0020409 " ] && pass "Preload: US, then two substitutes ($p)" || fail "Preload: '$p'"
[ "$(regq 'HKCU\Keyboard Layout\Substitutes' d0010409)" = 00010409 ] && [ "$(regq 'HKCU\Keyboard Layout\Substitutes' d0020409)" = 00000407 ] \
    && pass "Substitutes: d0010409 = Dvorak (00010409), d0020409 = German (00000407)" \
    || fail "Substitutes: $(regq 'HKCU\Keyboard Layout\Substitutes' d0010409) $(regq 'HKCU\Keyboard Layout\Substitutes' d0020409)"
case "$(texts)" in *"|German|"*) pass "the page lists German" ;; *) fail "German not listed: $(texts)" ;; esac

# --- Move up (German, the third), Remove (Dvorak, then the third) ---
click_id $((CMD + 22)); got=$(told 2)
[ "$got" = "keyboard set us,de,us ,,dvorak compose:ralt,grp:win_space_toggle" ] && pass "Move up: German second ($got)" || fail "Move up told: '$got'"
click_id $((CMD + 21)); got=$(told 3)
[ "$got" = "keyboard set de,us,us ,,dvorak compose:ralt,grp:win_space_toggle" ] && pass "Move up again: German first" || fail "Move up told: '$got'"
p=$(preload)
[ "$p" = "1=d0010409 2=00000409 3=d0020409 " ] && pass "Preload follows the order ($p)" || fail "Preload: '$p'"
click_id $((CMD + 42)); got=$(told 4)
[ "$got" = "keyboard set de,us - compose:ralt,grp:win_space_toggle" ] && pass "Remove: Dvorak goes ($got)" || fail "Remove told: '$got'"
p=$(preload)
[ "$p" = "1=d0010409 2=00000409 " ] && pass "Preload: German, US" || fail "Preload: '$p'"

# --- Left Alt + Shift ---
set -- $(tr -d '\r' < "$DUMP" | grep '^control SgSetCtl ' | head -1 | sed -n 's/.* at=\([0-9]*\),\([0-9]*\).*/\1 \2/p')
[ $# -eq 2 ] && { xdotool mousemove "$1" "$2" click 1; sleep 0.3; }
got=$(told 5)
[ "$got" = "keyboard set de,us - compose:ralt,grp:win_space_toggle,grp:alt_shift_toggle" ] \
    && pass "Left Alt + Shift switches too ($got)" || fail "Alt+Shift told: '$got'"
[ "$(regq 'HKCU\Keyboard Layout\Toggle' 'Language Hotkey')" = 1 ] && [ "$(regq 'HKCU\Keyboard Layout\Toggle' Hotkey)" = 1 ] \
    && pass "Toggle: Hotkey and Language Hotkey 1 (Left Alt + Shift)" || fail "Toggle: '$(regq 'HKCU\Keyboard Layout\Toggle' Hotkey)'"

# --- no compositor: at the next sign-in ---
: > "$T/no-compositor"
click_id $((CMD + 41)); got=$(told 6)
[ "$got" = "keyboard set de - compose:ralt" ] && pass "Remove the second: one keyboard, no switching keys ($got)" || fail "Remove told: '$got'"
i=0; while ! has "Your keyboards change the next time you sign in." && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
has "Your keyboards change the next time you sign in." && pass "no compositor to tell: the change is for the next sign-in" \
    || fail "no next-sign-in notice: $(texts)"
[ "$(regq 'HKCU\Keyboard Layout\Toggle' 'Language Hotkey')" = 1 ] && pass "one keyboard: Left Alt + Shift stays chosen (Toggle)" \
    || fail "Toggle lost the choice"

exit $RC
