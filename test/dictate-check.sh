#!/bin/sh
# Gate for voice typing's toolbar (sg-dictate): Notepad has the focus,
# `sg-dictate.exe /toggle` (found through App Paths, as explorer's Win+H finds
# it) opens the bar, which re-launches itself through its engine's bridge; an
# engine types into Notepad, and a probe reads Notepad's text back with
# WM_GETTEXT. Notepad must keep the focus throughout.
#
#   - a stand-in engine (speaking the bridge protocol, recording what it is
#     asked) checks the bar's side: where the text goes, typing and pasting
#     (the clipboard given back), the settings it sends, /toggle closing it,
#     "off" in the settings, and hold-to-talk on the X keyboard
#   - the real engine (sg-session's sg-dictate, Parakeet on the CPU), with a
#     spoken WAV standing in for the microphone, when a model is to hand
#     (SG_SPEECH_MODEL, or the machine's)
#
# Screenshots: build/dictate-*.png. Needs wine-sg, Xvfb, xdotool, ImageMagick
# and python3; skips (77) without them. WINH=1 also presses Win+H on the X
# keyboard (a wine-sg with patch 0092).
set -u
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
WINE_DIR="${SG_WINE_DIR:-/opt/wine-sg}"
EXE="$HERE/build/sg-dictate64.exe"
OUT="$HERE/build"
MINGW="${MINGW:-x86_64-w64-mingw32-gcc}"
RC=0; DPY=93; XP=""
pass() { echo "PASS  $*"; }
fail() { echo "FAIL  $*"; RC=1; }

for need in Xvfb xdotool import python3 "$MINGW"; do command -v "$need" >/dev/null || { echo "SKIP: $need missing"; exit 77; }; done
[ -x "$WINE_DIR/bin/wine" ] && [ -f "$EXE" ] || { echo "SKIP: wine-sg or build/sg-dictate64.exe missing"; exit 77; }

T=$(mktemp -d /var/tmp/sg-dictate-check.XXXXXX); chmod 755 "$T"
# shellcheck disable=SC2317
cleanup() {
    WINEPREFIX="$T/pfx" "$WINE_DIR/bin/wineserver" -k 2>/dev/null
    [ -n "$XP" ] && kill "$XP" 2>/dev/null
    rm -f "/tmp/.X${DPY}-lock"; rm -rf "$T"
}
trap cleanup EXIT INT TERM

"$MINGW" -O2 -municode -o "$T/probe.exe" "$HERE/test/sg-dictate-probe.c" -luser32 || { fail "probe did not build"; exit 1; }

# --- the stand-in engine -----------------------------------------------------------------
cat > "$T/fake-engine" <<'EOF'
#!/usr/bin/python3
# Speaks sg-dictate's bridge protocol. Each start "hears" the next of
# FAKE_TEXTS and types it; every request is recorded in FAKE_LOG.
import json, os, subprocess, sys, threading, time
args = sys.argv[1:]
# The Control Panel's modes: microphones, the model download, the level meter.
if args[:1] == ["--mics"]:
    out = args[args.index("--out") + 1]
    open(out + ".tmp", "w").write("MIC sg.test.mic\tTest Microphone\nMIC sg.other\tOther Mic\nDEFAULT sg.test.mic\nEND\n")
    os.replace(out + ".tmp", out); sys.exit(0)
if args[:1] == ["--download"]:
    d = os.environ["SG_SPEECH_DIR"]; os.makedirs(d + "/parakeet-tdt-0.6b-v3-int8", exist_ok=True)
    open(d + "/requests", "a").write("download\n")
    for done in (0, 336403781, 672807563):
        open(d + "/status.tmp", "w").write("STATE downloading\nDONE %d\nTOTAL 672807563\n" % done)
        os.replace(d + "/status.tmp", d + "/status"); time.sleep(float(os.environ.get("FAKE_DL_STEP", "1.5")))
    open(d + "/parakeet-tdt-0.6b-v3-int8/.verified", "w").write("ok\n")
    open(d + "/status", "w").write("STATE installed\nDONE 672807563\nTOTAL 672807563\n"); sys.exit(0)
if args[:1] == ["--meter"]:
    out = args[1]
    open(os.environ["FAKE_LOG"], "a").write("meter " + " ".join(args[2:]) + "\n")
    for i in range(300):
        if os.path.exists(out + ".stop"): break
        open(out + ".tmp", "w").write("LEVEL 42\n"); os.replace(out + ".tmp", out); time.sleep(0.1)
    open(out, "w").write("DONE\n")
    try: os.unlink(out + ".stop")
    except OSError: pass
    sys.exit(0)
assert args[0] == "--bridge"
log = open(os.environ["FAKE_LOG"], "a", buffering=1)
dr, dw = os.pipe(); ur, uw = os.pipe()
child = subprocess.Popen(args[1:], stdin=dr, stdout=uw, stderr=open(os.environ["FAKE_ERR"], "ab"), close_fds=True)
os.close(dr); os.close(uw)
out = os.fdopen(dw, "w", encoding="utf-8"); lock = threading.Lock()
def emit(line):
    with lock:
        try: out.write(line + "\n"); out.flush()
        except OSError: pass
texts = json.load(open(os.environ["FAKE_TEXTS"]))
n = int(open(os.environ["FAKE_N"]).read()) if os.path.exists(os.environ["FAKE_N"]) else 0
for raw in os.fdopen(ur, "rb"):
    line = raw.decode("utf-8", "replace").strip()
    if not line.startswith("{"): continue
    log.write(line + "\n")
    cmd = json.loads(line)["cmd"]
    if cmd == "start":
        emit("STATE loading"); emit("STATE listening"); emit("LEVEL 70")
        emit("TEXT " + json.dumps(texts[n % len(texts)], ensure_ascii=False))
        n += 1; open(os.environ["FAKE_N"], "w").write(str(n))
    elif cmd in ("stop", "cancel"):
        emit("STATE idle")
    elif cmd == "quit":
        break
child.wait()
EOF
chmod 755 "$T/fake-engine"
printf '%s' '["Hello from voice typing.", "Second line?\n", "Held to talk.", "Caf\u00e9 cr\u00e8me \u2013 fin"]' > "$T/texts.json"
export FAKE_LOG="$T/engine.log" FAKE_ERR="$T/bar.log" FAKE_TEXTS="$T/texts.json" FAKE_N="$T/n"
: > "$FAKE_LOG"; : > "$FAKE_ERR"

# --- Wine, a shell desktop and Notepad ----------------------------------------------------------
Xvfb ":$DPY" -screen 0 1024x768x24 -nolisten tcp >/dev/null 2>&1 & XP=$!
export DISPLAY=":$DPY" WINEPREFIX="$T/pfx" WINEARCH=win64 WINEDLLOVERRIDES='mscoree,mshtml=' WINEDEBUG=-all
export PATH="$WINE_DIR/bin:$PATH" SG_DICTATE="$T/fake-engine"
wine wineboot --init >/dev/null 2>&1; wineserver -w
cp "$T/probe.exe" "$WINEPREFIX/drive_c/probe.exe"
winexe=$(wine winepath -w "$EXE" 2>/dev/null | tr -d '\r')
reg() { wine reg add "$@" /f >/dev/null 2>&1; }
reg 'HKCU\Software\Wine\Explorer' /v Desktop /d shell
reg 'HKCU\Software\Wine\Explorer\Desktops' /v shell /d 1024x768
# App Paths as defaults/64-sg-dictate.reg has it, pointing at this build.
reg 'HKLM\Software\Microsoft\Windows\CurrentVersion\App Paths\sg-dictate.exe' /ve /d "$winexe"
speech() { reg 'HKCU\Software\Stained Glass\Speech' /v "$1" /t REG_DWORD /d "$2"; }
speech Enabled 1
speech SpokenPunctuation 0
wineserver -w
P() { wine 'C:\probe.exe' "$@" 2>/dev/null | tr -d '\r'; }
WINEDEBUG=trace+explorer wine explorer /desktop=shell,1024x768 > "$T/explorer.out" 2>&1 &
i=0; while ! grep -q 'desktop message loop starting' "$T/explorer.out" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
wine notepad >/dev/null 2>&1 &
i=0; while [ "$(P text Notepad)" = NOWINDOW ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
P activate Notepad; sleep 1
[ "$(P foreground)" = Notepad ] && pass "Notepad has the focus" || fail "Notepad is not in front: $(P foreground)"

# Wait until Notepad's text is $1 (up to $2 seconds).
wait_text() {
    i=0
    while [ "$(P text Notepad)" != "$1" ] && [ $i -lt $(( $2 * 2 )) ]; do sleep 0.5; i=$((i + 1)); done
    [ "$(P text Notepad)" = "$1" ]
}
wait_gone() {
    i=0
    while P bar | grep -q 'VISIBLE 1' && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
    ! P bar | grep -q 'VISIBLE 1'
}
toggle() { wine start sg-dictate.exe /toggle >/dev/null 2>&1; }

# --- /toggle: the bar, and typing into Notepad -------------------------------------------------
toggle
if wait_text "Hello from voice typing." 30; then
    pass "/toggle through App Paths: the engine's words were typed into Notepad"
else
    fail "Notepad has '$(P text Notepad)'"
fi
bar=$(P bar)
echo "$bar" | grep -q 'VISIBLE 1' && pass "the bar is up" || fail "no bar: $bar"
ex=$(echo "$bar" | sed -n 's/^EXSTYLE //p')
if [ -n "$ex" ] && [ $(( 0x$ex & 0x08000088 )) = $(( 0x08000088 )) ]; then
    pass "the bar is topmost, a tool window and never activated (exstyle $ex)"
else
    fail "exstyle $ex lacks NOACTIVATE|TOOLWINDOW|TOPMOST"
fi
set -- $(echo "$bar" | sed -n 's/^RECT //p')
if [ $# = 4 ] && [ $(( ($1 + $3) / 2 )) -ge 500 ] && [ $(( ($1 + $3) / 2 )) -le 524 ] && [ "$2" -lt 60 ]; then
    pass "the bar is at the top centre ($*)"
else
    fail "the bar is at $*"
fi
[ "$(P foreground)" = Notepad ] && pass "Notepad kept the focus" || fail "the focus went to $(P foreground)"
sleep 0.5; import -window root "$OUT/dictate-bar.png"
grep -q '"cmd": "start".*"continuous": true.*"spoken": false.*"fresh": true' "$FAKE_LOG" \
    && pass "the settings went with the request (continuous, spoken punctuation off)" \
    || fail "start request: $(grep start "$FAKE_LOG" | head -1)"
grep -q 'inserted 24 characters into Notepad by typing' "$FAKE_ERR" && pass "typed with SendInput" \
    || fail "bar log: $(cat "$FAKE_ERR")"

toggle
wait_gone && pass "/toggle again closes the bar" || fail "the bar is still up: $(P bar)"
sleep 1
tail -2 "$FAKE_LOG" | grep -q '"stop"' && tail -1 "$FAKE_LOG" | grep -q '"quit"' \
    && pass "closing stopped listening, then let the engine go" || fail "engine log end: $(tail -3 "$FAKE_LOG")"

# --- the clipboard way, and the clipboard given back -------------------------------------------------
speech InsertMethod 1
P clip-set "keep me"
P activate Notepad; sleep 0.5
toggle
if wait_text "Hello from voice typing.Second line?\\n" 30; then
    pass "pasted, with the line break"
else
    fail "after paste Notepad has '$(P text Notepad)'"
fi
grep -q 'by paste' "$FAKE_ERR" && pass "the bar pasted" || fail "no paste in the bar log"
sleep 1
[ "$(P clip-get)" = "keep me" ] && pass "the clipboard was given back" || fail "clipboard now '$(P clip-get)'"
toggle; wait_gone
speech InsertMethod 0

# --- voice typing turned off ------------------------------------------------------------------------------
speech Enabled 0
starts=$(grep -c '"start"' "$FAKE_LOG")
toggle
i=0; while ! grep -q 'state off' "$FAKE_ERR" && [ $i -lt 30 ]; do sleep 0.5; i=$((i + 1)); done
grep -q 'state off' "$FAKE_ERR" && pass "off in the settings: the bar says so" || fail "no 'off' state"
sleep 0.5; import -window root "$OUT/dictate-off.png"
[ "$(grep -c '"start"' "$FAKE_LOG")" = "$starts" ] && pass "...and the microphone was not opened" \
    || fail "a start was sent while off"
toggle; wait_gone
speech Enabled 1

# --- hold-to-talk, on the X keyboard ------------------------------------------------------------------------
speech HoldToTalk 1
: > "$FAKE_ERR"
wine "$EXE" /background >/dev/null 2>&1
i=0; while ! grep -q 'waiting for the hold-to-talk key' "$FAKE_ERR" && [ $i -lt 30 ]; do sleep 0.5; i=$((i + 1)); done
grep -q 'waiting for the hold-to-talk key' "$FAKE_ERR" && pass "the hold-to-talk listener is waiting" || fail "no listener: $(cat "$FAKE_ERR")"
P bar | grep -q 'VISIBLE 0' && pass "...with no bar showing" || fail "bar: $(P bar)"
P activate Notepad; sleep 0.5
before=$(P text Notepad)
starts=$(grep -c '"start"' "$FAKE_LOG")
# held longer than the hold delay, with C: a shortcut, never dictation
xdotool keydown Control_R; sleep 0.1; xdotool keydown c; sleep 0.6; xdotool keyup c; sleep 0.1; xdotool keyup Control_R; sleep 1
[ "$(grep -c '"start"' "$FAKE_LOG")" = "$starts" ] && pass "Right Ctrl+C is a shortcut, not dictation" \
    || fail "a chord started dictation"
xdotool keydown Control_R; sleep 1.5
P bar | grep -q 'VISIBLE 1' && pass "holding Right Ctrl opens the bar" || fail "no bar while held"
xdotool keyup Control_R; sleep 1
[ "$(P text Notepad)" = "${before}Held to talk." ] && pass "held to talk: typed into Notepad" \
    || fail "Notepad has '$(P text Notepad)'"
tail -1 "$FAKE_LOG" | grep -q '"stop"' && pass "releasing the key stops listening" || fail "log end: $(tail -2 "$FAKE_LOG")"
wait_gone && pass "...and the bar goes again" || fail "the bar stayed after hold-to-talk"
[ "$(P foreground)" = Notepad ] && pass "Notepad kept the focus" || fail "the focus went to $(P foreground)"
speech HoldToTalk 0
wine "$EXE" /reload >/dev/null 2>&1
sleep 1

# --- Control Panel > Speech Recognition ------------------------------------------------------
CTL="$HERE/build/sg-control64.exe"
if [ -f "$CTL" ]; then
    export SG_SPEECH_DIR="$T/speech" FAKE_DL_STEP=4
    mkdir -p "$SG_SPEECH_DIR"
    ctl() { wine "$CTL" "$@" 2>/dev/null </dev/null | tr -d '\r'; }
    val() { printf '%s\n' "$1" | sed -n "s/^speech\.$2=//p" | head -1; }
    wine reg delete 'HKCU\Software\Stained Glass\Speech' /f >/dev/null 2>&1
    out=$(ctl --dump speech)
    [ "$(val "$out" enabled)/$(val "$out" model)/$(val "$out" insert)/$(val "$out" hold_key)/$(val "$out" continuous)" = "off/missing/type/Right Ctrl/on" ] \
        && pass "Speech --dump: the defaults (off, no model, typing, Right Ctrl, continuous)" || fail "defaults: $out"
    [ "$(val "$out" model.size)" = "641.6 MB" ] && pass "Speech --dump: the download's size" || fail "size: $(val "$out" model.size)"
    speech Enabled 1; speech InsertMethod 1; speech HoldToTalk 1; speech HoldKey 165; speech Continuous 0
    speech SpokenPunctuation 0; speech RemoveFillers 0; speech FormatNumbers 0; speech AutoPunctuation 0
    reg 'HKCU\Software\Stained Glass\Speech' /v Microphone /d sg.other
    reg 'HKCU\Software\Stained Glass\Speech' /v Language /d auto
    printf 'STATE downloading\nDONE 1000\nTOTAL 672807563\n' > "$SG_SPEECH_DIR/status"
    out=$(ctl --dump speech)
    got="$(val "$out" enabled) $(val "$out" insert) $(val "$out" hold_to_talk) $(val "$out" hold_key) $(val "$out" continuous)"
    got="$got $(val "$out" spoken_punctuation) $(val "$out" remove_fillers) $(val "$out" format_numbers) $(val "$out" auto_punctuation)"
    got="$got $(val "$out" microphone) $(val "$out" language) $(val "$out" model) $(val "$out" model.progress)"
    [ "$got" = "on paste on Right Alt off off off off off sg.other auto downloading 1000/672807563" ] \
        && pass "Speech --dump reflects every setting and the download" || fail "dump: $got"
    wine reg delete 'HKCU\Software\Stained Glass\Speech' /f >/dev/null 2>&1
    rm -f "$SG_SPEECH_DIR/status"
    [ "$(ctl --resolve Microsoft.SpeechRecognition)" = "page=Speech Recognition" ] \
        && pass "control /name Microsoft.SpeechRecognition opens it (the bar's gear)" || fail "resolve: $(ctl --resolve Microsoft.SpeechRecognition)"

    # The page itself: turning voice typing on downloads the model, with progress.
    wine "$CTL" /name Microsoft.SpeechRecognition >/dev/null 2>&1 &
    i=0; while [ "$(P check SgControlWindow 'Turn on voice typing')" != 0 ] && [ $i -lt 40 ]; do sleep 0.5; i=$((i + 1)); done
    [ "$(P check SgControlWindow 'Turn on voice typing')" = 0 ] && pass "the Speech page opens, voice typing off" || fail "no page"
    sleep 1; import -window root "$OUT/dictate-cpl.png"
    P click SgControlWindow 'Turn on voice typing'
    # halfway (the stand-in holds each step FAKE_DL_STEP seconds): the bar says so
    i=0; while ! grep -q 'DONE 336403781' "$SG_SPEECH_DIR/status" 2>/dev/null && [ $i -lt 40 ]; do sleep 0.25; i=$((i + 1)); done
    sleep 1
    P progress SgControlWindow | head -1 | grep -qx 'PROGRESS 499' && pass "turning it on started the download, and the page shows its progress" \
        || fail "progress: $(P progress SgControlWindow) requests: $(cat "$SG_SPEECH_DIR/requests" 2>/dev/null)"
    import -window root "$OUT/dictate-cpl-download.png"
    i=0; while [ "$(ctl --dump speech | sed -n 's/^speech.model=//p')" != installed ] && [ $i -lt 20 ]; do sleep 0.5; i=$((i + 1)); done
    out=$(ctl --dump speech)
    [ "$(val "$out" enabled)/$(val "$out" model)" = on/installed ] && pass "voice typing on, model installed" || fail "after: $out"
    sleep 1
    [ "$(P progress SgControlWindow | wc -l)" != 1 ] && fail "the download bar stayed up: $(P progress SgControlWindow)" \
        || pass "...and the download's bar goes away"
    P click SgControlWindow 'Test microphone'
    i=0; while ! P progress SgControlWindow | grep -q 'PROGRESS 42' && [ $i -lt 20 ]; do sleep 0.25; i=$((i + 1)); done
    P progress SgControlWindow | grep -q 'PROGRESS 42' && pass "Test microphone: the level meter moves" || fail "meter: $(P progress SgControlWindow)"
    sleep 0.5; import -window root "$OUT/dictate-cpl-test.png"
    P click SgControlWindow 'Stop test'
    P click SgControlWindow 'Pasting them (quicker for long text; your clipboard is put back)'
    out=$(ctl --dump speech)
    [ "$(val "$out" insert)" = paste ] && pass "the page writes the settings (insert by pasting)" || fail "insert: $(val "$out" insert)"
    wine reg delete 'HKCU\Software\Stained Glass\Speech' /v InsertMethod /f >/dev/null 2>&1
    speech Enabled 1
fi

if [ "${WINH:-0}" = 1 ]; then
    P activate Notepad; sleep 0.5
    before=$(P text Notepad)
    t0=$(date +%s)
    xdotool key super+h
    i=0; while ! P bar | grep -q 'VISIBLE 1' && [ $i -lt 90 ]; do sleep 0.5; i=$((i + 1)); done
    P bar | grep -q 'VISIBLE 1' && pass "Win+H opens the bar ($(( $(date +%s) - t0 ))s)" || fail "Win+H: no bar"
    wait_text "${before}Café crème – fin" 20 && pass "Win+H: typed (with accents and a dash)" \
        || fail "Win+H: Notepad has '$(P text Notepad)'"
    [ "$(P foreground)" = Notepad ] && pass "Notepad kept the focus" || fail "the focus went to $(P foreground)"
    sleep 0.5; import -window root "$OUT/dictate-winh.png"
    xdotool key super+h; wait_gone && pass "Win+H again closes it" || fail "still up after Win+H"
fi

# --- the real engine ----------------------------------------------------------------------------------------
ENGINE=""
for c in "${SG_DICTATE_SCRIPT:-}" "$HERE/../sg-session/speech/sg-dictate" /usr/bin/sg-dictate; do
    [ -n "$c" ] && [ -f "$c" ] && { ENGINE=$(CDPATH='' cd -- "$(dirname -- "$c")" && pwd)/$(basename -- "$c"); break; }
done
MODEL="${SG_SPEECH_MODEL:-}"
[ -z "$MODEL" ] && [ -e /var/lib/stained-glass-speech/parakeet-tdt-0.6b-v3-int8/.verified ] && \
    MODEL=/var/lib/stained-glass-speech/parakeet-tdt-0.6b-v3-int8
if [ -n "$ENGINE" ] && [ -n "$MODEL" ] && command -v espeak-ng >/dev/null && python3 -c 'import onnxruntime' 2>/dev/null; then
    espeak-ng -v en-us -s 150 -w "$T/say.wav" "hello world this is voice typing period" 2>/dev/null
    speech SpokenPunctuation 1
    P activate Notepad; sleep 0.5
    before=$(P text Notepad)
    : > "$T/real.log"
    SG_DICTATE="$ENGINE" SG_SPEECH_LIB="$(dirname "$ENGINE")" SG_SPEECH_MODEL="$MODEL" \
        SG_DICTATE_AUDIO_FILE="$T/say.wav" SG_DICTATE_TIMING=1 \
        wine start sg-dictate.exe /toggle >/dev/null 2>"$T/real.log"
    want="${before}Hello world, this is voice typing."
    i=0; while [ "$(P text Notepad)" = "$before" ] && [ $i -lt 60 ]; do sleep 0.5; i=$((i + 1)); done
    sleep 1
    got=$(P text Notepad)
    import -window root "$OUT/dictate-real.png"
    # a space first: the toolbar told the engine what was before the caret
    case "$got" in "$before "[Hh]ello\ world*voice\ typing.) pass "the real engine typed what was said, after a space: ${got#"$before"}" ;;
        *) fail "real engine: Notepad has '$got' (wanted like '$want')" ;; esac
    [ "$(P foreground)" = Notepad ] && pass "Notepad kept the focus" || fail "the focus went to $(P foreground)"
    toggle; wait_gone || fail "real engine: the bar did not close"
else
    echo "SKIP  the real engine (needs sg-session's sg-dictate, a model, espeak-ng and python3-onnxruntime)"
fi

[ $RC = 0 ] && echo "dictate-check: OK" || echo "dictate-check: FAILED"
exit $RC
