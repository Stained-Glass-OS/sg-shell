#!/usr/bin/python3
"""SG Office -- the VBA corpus runner: every '@test in a .vba corpus file
becomes a module of its own in a new document (Option VBASupport 1, as a
document's VBA is imported), is called through the document's script
provider, and its result compared with Excel's.

    vba.py [--wine SOFFICE.EXE] [--json OUT] [--report OUT.md] [--baseline BASE.json] FILE.vba...

Copyright (C) 2026 Stained Glass OS contributors
SPDX-License-Identifier: AGPL-3.0-or-later
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "parity"))


def parse(path):
    tests, cur = [], None
    for line in open(path, encoding="utf-8"):
        m = re.match(r"'@test (\w+) => (.*)$", line.rstrip("\n"))
        if m:
            cur = {"name": m.group(1), "expected": m.group(2).strip(), "code": [], "file": os.path.basename(path)}
            tests.append(cur)
        elif cur is not None:
            cur["code"].append(line.rstrip("\n"))
    for t in tests:
        e = t["expected"]
        t["want"] = e[1:-1].replace('""', '"') if e.startswith('"') else (e.upper() == "TRUE" if e.upper() in ("TRUE", "FALSE") else float(e))
        t["source"] = ("Option VBASupport 1\n" + "\n".join(t["code"]) +
                       "\nFunction SG_Run()\n    On Error GoTo SG_Fail\n    SG_Run = %s()\n    Exit Function\n"
                       "SG_Fail:\n    SG_Run = \"#ERR \" & Err.Number & \": \" & Err.Description\nEnd Function\n"
                       % t["name"])
    return tests


def same(got, want):
    if isinstance(want, bool):
        return got is want or got == want
    if isinstance(want, float):
        try:
            return abs(float(got) - want) <= 1e-9 * max(1, abs(want))
        except (TypeError, ValueError):
            return False
    return str(got) == want


def reset(doc, kind):
    """A clean workbook (one empty Sheet1) or document before each test."""
    if kind == "scalc":
        sheets = doc.Sheets
        while sheets.Count > 1:
            sheets.removeByName(sheets.getByIndex(sheets.Count - 1).Name)
        sh = sheets.getByIndex(0)
        sh.Name = "Sheet1"
        # 1023 = every kind of content (values, text, formulas, formats, notes, objects...)
        sh.getCellRangeByName("A1:AMJ1000").clearContents(1023)
        sh.DrawPage and [sh.DrawPage.remove(sh.DrawPage.getByIndex(0)) for _ in range(sh.DrawPage.Count)]
        doc.NamedRanges and [doc.NamedRanges.removeByName(n) for n in doc.NamedRanges.ElementNames]
        doc.CurrentController.setActiveSheet(sh)
    else:
        doc.Text.setString("")


WATCH = r"""
import subprocess, sys, time
mark = sys.argv[1]
while True:
    r = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", "^(Error|Application Error|BASIC .*)$"],
                       capture_output=True, text=True)
    for wid in r.stdout.split():
        # a Wine window takes a click, not a synthetic key: OK is at the
        # bottom middle of the message box
        g = subprocess.run(["xdotool", "getwindowgeometry", "--shell", wid], capture_output=True, text=True)
        v = dict(line.split("=", 1) for line in g.stdout.split() if "=" in line)
        if {"X", "Y", "WIDTH", "HEIGHT"} <= set(v) and int(v["HEIGHT"]) < 90:
            open(mark, "w").close()
            x, y = int(v["X"]) + int(v["WIDTH"]) // 2, int(v["Y"]) + int(v["HEIGHT"]) - 22
            subprocess.run(["xdotool", "mousemove", str(x), str(y), "click", "1"], capture_output=True)
    r = subprocess.run(["xdotool", "search", "--onlyvisible", "--name", " Basic$"], capture_output=True, text=True)
    for wid in r.stdout.split():
        subprocess.run(["xdotool", "windowclose", wid], capture_output=True)
    time.sleep(1)
"""


class DialogWatch:
    """A macro's error message box (a modal dialog) would wait for a click
    that never comes: a process of its own answers it (the UNO call waiting
    on the box holds Python's lock, so a thread could not), closes the Basic
    IDE the error opened, and leaves a mark the runner reads per test."""

    def __init__(self):
        import tempfile
        self.mark = tempfile.mktemp(prefix="sgvba-dialog-", dir=os.environ.get("TMPDIR", "/var/tmp"))
        self.proc = subprocess.Popen([sys.executable, "-c", WATCH, self.mark])

    @property
    def seen(self):
        return "error message" if os.path.exists(self.mark) else None

    def clear(self):
        if os.path.exists(self.mark):
            os.remove(self.mark)

    def close(self):
        self.proc.kill()
        self.clear()


def run(tests, kind, wine=None, seed=None, results=None):
    from lo import Office
    # Known: under wine-sg a new visible document sometimes ends LibreOffice
    # (an access violation in mergedlo.dll while handling a C++ exception,
    # then a fail-fast) -- start again, up to three times.
    watch = DialogWatch()     # from the start: a box may come up before any test
    for attempt in range(3):
        # one visible document, as a user's (ActiveSheet, selections and the
        # clipboard need a view; the display is the gate's own Xvfb): the one
        # LibreOffice opens as it starts (--calc/--writer, as a user's click
        # would) -- a visible document made over UNO under Wine can hang.
        # Each test gets a module of its own and a cleared workbook.
        office = Office(wine=wine, seed=seed, env={"LANG": "en_US.UTF-8", "LC_ALL": "en_US.UTF-8"}, headless=False,
                        start="--calc" if kind == "scalc" else "--writer")
        try:
            doc = office.current("com.sun.star.sheet.SpreadsheetDocument" if kind == "scalc"
                                 else "com.sun.star.text.TextDocument")
            break
        except Exception:
            office.close()
            if attempt == 2:
                watch.close()
                raise
    try:
        libs = doc.BasicLibraries
        libs.VBACompatibilityMode = True
        std = libs.getByName("Standard")
        for t in tests:
            print("running  %s" % t["name"], file=sys.stderr, flush=True)
            reset(doc, kind)
            watch.clear()
            mod = "T_" + t["name"]
            try:
                std.insertByName(mod, t["source"])
                script = doc.getScriptProvider().getScript(
                    "vnd.sun.star.script:Standard.%s.SG_Run?language=Basic&location=document" % mod)
                got = script.invoke((), (), ())[0]
            except Exception as e:           # a compile error, or no such object model
                got = "#ERR (not run): " + str(e).splitlines()[0][:160]
            finally:
                if std.hasByName(mod):
                    std.removeByName(mod)
            if watch.seen and not (isinstance(got, str) and got.startswith("#ERR")):
                got = "#ERR (an %s box)%s" % (watch.seen, "" if got is None else ": " + str(got))
            t["got"] = got
            t["status"] = "match" if same(got, t["want"]) else ("error" if isinstance(got, str) and got.startswith("#ERR")
                                                                  or got is None else "differs")
            if results:
                with open(results, "a") as f:
                    f.write(json.dumps({"name": t["name"], "got": str(got) if got is not None else None,
                                        "status": t["status"]}) + "\n")
        doc.close(True)
    finally:
        watch.close()
        office.close()


def supervise(path, wine, seed):
    """Run a corpus file in worker processes, so a macro that never returns
    (LibreOffice paused in its Basic debugger, a hung call) costs its own test
    and not the rest: no progress for STALL seconds ends LibreOffice (the
    prefix's wineserver) and a new worker carries on after it."""
    import tempfile
    stall = int(os.environ.get("SG_VBA_STALL", "150"))
    tests = parse(path)
    kind = "swriter" if "word" in os.path.basename(path) else "scalc"
    out = tempfile.mktemp(prefix="sgvba-results-", dir=os.environ.get("TMPDIR", "/var/tmp"))
    done = {}
    start = 0
    while start < len(tests):
        open(out, "w").close()
        w = subprocess.Popen([sys.executable, os.path.abspath(__file__), "--worker", "--start", str(start),
                              "--results", out] + (["--wine", wine] if wine else []) + (["--seed", seed] if seed else [])
                             + [path])
        last, seen = time.time(), 0
        while w.poll() is None:
            n = sum(1 for _ in open(out))
            if n != seen:
                seen, last = n, time.time()
            elif time.time() - last > stall:
                w.kill()
                subprocess.run([os.environ.get("WINESERVER", "wineserver"), "-k"], capture_output=True)
                break
            time.sleep(2)
        w.wait()
        lines = [json.loads(line) for line in open(out)]
        for r in lines:
            done[r["name"]] = r
        start += len(lines)
        if start < len(tests) and len(lines) == 0 or (w.returncode not in (0, None) and start < len(tests)):
            # the test at START never returned (or took LibreOffice down): recorded, skipped
            t = tests[start]
            done[t["name"]] = {"name": t["name"], "got": "#ERR (never returned: LibreOffice hung or ended)", "status": "error"}
            start += 1
    os.remove(out)
    for t in tests:
        r = done.get(t["name"], {"got": "#ERR (not run)", "status": "error"})
        t["got"], t["status"] = r["got"], r["status"]
    return tests


def main():
    import faulthandler
    faulthandler.dump_traceback_later(240, repeat=True)   # where it waits, should a test never return
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="+")
    ap.add_argument("--wine")
    ap.add_argument("--seed")
    ap.add_argument("--json")
    ap.add_argument("--report")
    ap.add_argument("--baseline")
    ap.add_argument("--worker", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--start", type=int, default=0, help=argparse.SUPPRESS)
    ap.add_argument("--results", help=argparse.SUPPRESS)
    a = ap.parse_args()
    if a.worker:
        f = a.files[0]
        run(parse(f)[a.start:], "swriter" if "word" in os.path.basename(f) else "scalc", a.wine, a.seed, a.results)
        return 0
    tests = []
    for f in a.files:
        tests += supervise(f, a.wine, a.seed)
    counts = {}
    for t in tests:
        counts[t["status"]] = counts.get(t["status"], 0) + 1
        print("%-8s %-32s want %-28s got %s" % (t["status"], t["file"] + ":" + t["name"], t["expected"], t["got"]))
    print("vba: " + ", ".join("%s %d" % kv for kv in sorted(counts.items())))
    res = {t["file"] + ":" + t["name"]: {"expected": t["expected"], "got": str(t["got"]), "status": t["status"]} for t in tests}
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1)
    if a.report:
        out = ["| Macro | Excel/Word gives | SG Office gives | Result |", "|---|---|---|---|"]
        for t in tests:
            out.append("| %s | %s | %s | %s |" % (t["name"], t["expected"].replace("|", "\\|"),
                                                  str(t["got"]).replace("|", "\\|"), t["status"]))
        open(a.report, "w").write("\n".join(out) + "\n")
    rc = 0
    if a.baseline:
        base = json.load(open(a.baseline))
        bad = [k for k, v in base.items() if v["status"] == "match" and res.get(k, {}).get("status") != "match"]
        for k in bad:
            print("REGRESSION  %s: %s" % (k, res.get(k)))
        rc = 1 if bad else 0
    return rc


if __name__ == "__main__":
    sys.exit(main())
