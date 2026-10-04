#!/bin/sh
. "$(dirname "$0")/scratch-home.sh"
# shellcheck disable=SC2015  # pass/fail one-liners: both only print
# Voice typing into a Linux program (David 2026-10-03: "doesn't make it into
# the Linux side, like Firefox for Linux"): with a Linux program's frame in
# front (SgLinuxWindow), what is said goes to xdotool -- which types into the
# X window that has the focus -- not to SendInput, which reaches the frame
# and never the program; "delete that" is xdotool's BackSpace, as many as
# the text had characters. A stand-in engine (dictate-check.sh's) and a
# stand-in xdotool (SG_XDOTOOL) that records what it is asked.
#
# Needs wine-sg, Xvfb, python3, mingw; skips (77) without. SG_DICTATE_EXE:
# another build (mutant SG_MUTANT_LINUX_BY_SENDINPUT).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="${SG_DICTATE_EXE:-$HERE/build/sg-dictate64.exe}"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
RC=0; DPY=${SG_DICTATE_DPY:-94}; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }
for need in Xvfb python3 "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or $EXE missing"; exit 77; }
T=$(mktemp -d /var/tmp/sg-dictate-linux.XXXXXX)
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; [ -n "${KEEP:-}" ] || rm -rf "$T"
}
trap cleanup EXIT INT TERM
"$MINGW" -O2 -municode -mwindows -o "$T/frame.exe" "$HERE/test/dictate-linux-frame.c" || { fail "frame stand-in did not build"; exit 1; }
# the stand-in engine, as dictate-check.sh has it
sed -n "/^cat > \"\$T\/fake-engine\" <<'EOF'\$/,/^EOF\$/p" "$HERE/test/dictate-check.sh" | sed '1d;$d' > "$T/fake-engine"
chmod 755 "$T/fake-engine"
grep -q -- '--bridge' "$T/fake-engine" || { fail "no stand-in engine taken from dictate-check.sh"; exit 1; }
# what is "heard": quotes, accents and a dash, then a phrase and "delete that"
printf '%s' '["H\u00e9llo \"Linux\" \u2013 c\u00f4t\u00e9", {"text": " two words", "after": 3, "cmd": "delete"}]' > "$T/texts.json"
cat > "$T/xdotool" <<EOS
#!/bin/sh
for a in "\$@"; do printf '[%s]' "\$a"; done >> "$T/xdotool.log"
echo >> "$T/xdotool.log"
EOS
chmod 755 "$T/xdotool"
export FAKE_LOG="$T/engine.log" FAKE_ERR="$T/bar.log" FAKE_TEXTS="$T/texts.json" FAKE_N="$T/n"
: > "$FAKE_LOG"; : > "$FAKE_ERR"
Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=,winemenubuilder.exe=d' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH" SG_DICTATE="$T/fake-engine"
wine wineboot --init >/dev/null 2>&1
SG_XDOTOOL=$(wine winepath -w "$T/xdotool" 2>/dev/null | tr -d '\r'); export SG_XDOTOOL
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
wine reg add 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\sg-dictate.exe' /ve /d "$winexe" /f >/dev/null 2>&1
wine reg add 'HKCU\Software\Stained Glass\Speech' /v Enabled /t REG_DWORD /d 1 /f >/dev/null 2>&1
wine reg add 'HKCU\Software\Stained Glass\Speech' /v SpokenPunctuation /t REG_DWORD /d 0 /f >/dev/null 2>&1
cp "$T/frame.exe" "$WINEPREFIX/drive_c/"
wine 'C:\frame.exe' >/dev/null 2>&1 &
sleep 4
wine start sg-dictate.exe /toggle >/dev/null 2>&1
i=0; while [ "$(cat "$T/xdotool.log" 2>/dev/null | grep -c '^\[type\]')" -lt 1 ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
grep -qxF '[type][--clearmodifiers][--delay][6][--][Héllo "Linux" – côté]' "$T/xdotool.log" 2>/dev/null \
    && pass "with a Linux program's frame in front, the words go to xdotool, exactly (quotes, accents, dash)" \
    || fail "xdotool asked: $(cat "$T/xdotool.log" 2>/dev/null); the bar: $(tail -3 "$T/bar.log")"
wine start sg-dictate.exe /toggle >/dev/null 2>&1; sleep 3
wine start sg-dictate.exe /toggle >/dev/null 2>&1
i=0; while ! grep -q 'BackSpace' "$T/xdotool.log" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
grep -qxF '[type][--clearmodifiers][--delay][6][--][ two words]' "$T/xdotool.log" && \
grep -qxF '[key][--clearmodifiers][--repeat][10][BackSpace]' "$T/xdotool.log" \
    && pass "\"delete that\" is as many BackSpaces as the text had characters (10), through xdotool" \
    || fail "delete: $(cat "$T/xdotool.log" 2>/dev/null)"
grep -q "inserted .* into SgLinuxWindow by xdotool" "$T/bar.log" && pass "the bar reports it typed into the Linux program by xdotool" \
    || fail "bar log: $(grep inserted "$T/bar.log")"
[ "$RC" = 0 ] && echo "RESULT: PASS" || echo "RESULT: FAIL"
exit "$RC"
