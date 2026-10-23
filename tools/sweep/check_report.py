#!/usr/bin/env python3
"""Collects the runs of tools/sweep/check.sh into one report (docs/ios-port/SYSTEMS-CHECK.md has the table it feeds).

    check_report.py --out <dir> <run dir>...

A run dir is what tools/sweep/run.sh leaves: engine.log, shell.log, summary.txt, stderr.txt and any crash file. What is read:

  - the driver's `[sweep] ... CHECK PASS|FAIL <system>/<item> | <detail>` lines (gamedata/scripts/sweep_check*.script),
  - Lua errors in the engine log (SCRIPT RUNTIME ERROR, FATAL ERROR, "attempt to ...") and the game's own `! ERROR` prints,
  - the sanitizer counts of summary.txt, crash files,
  - the footprint the app logs every 10 s (memory over time),
  - the `snd:` counters of xrSound (sources handed to OpenAL, AL errors).

Writes results.md, results.json and prints the one-line verdict. Exit status 1 when anything failed.
"""
import argparse
import json
import os
import re
import sys

CHECK_RE = re.compile(r"\[sweep\] \[t=(\d+) f=(\d+)\] CHECK (PASS|FAIL) (\S+) \| (.*)$")
SNAP_RE = re.compile(r"snd: emitters started (\d+), sources played (\d+) \(rewound (\d+), restarted after an underrun (\d+)\), stopped (\d+), rendering (\d+) of (\d+), emitters alive (\d+), AL errors (\d+)")
FOOT_RE = re.compile(r"^\[\s*([\d.]+)\] anomaly: .*footprint (\d+) MB \(peak (\d+)\)")
LUA_FATAL_RE = re.compile(r"SCRIPT RUNTIME ERROR|^FATAL ERROR|^! \[LUA\] .*(attempt to|bad argument|stack overflow|cannot resume)|\[error\]Expression|LUA error:")
PRINTE_RE = re.compile(r"^! ?(ERROR|Error|error)\b")


def read(path):
    try:
        with open(path, errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []


def norm(line):
    return re.sub(r"\d+(\.\d+)?", "N", line)[:160]


def parse_run(d):
    run = {"dir": d, "plan": os.path.basename(d.rstrip("/")), "checks": [], "lua_errors": [], "printe": {}, "asan": 0, "ubsan": 0, "crash": 0,
           "exit": None, "seconds": None, "snd": None, "footprint": [], "driver_fail": []}
    summary = read(os.path.join(d, "summary.txt"))
    if summary:
        m = re.match(r"plan (\S+), exit status (\d+), (\d+)s", summary[0])
        if m:
            run["plan"], run["exit"], run["seconds"] = m.group(1), int(m.group(2)), int(m.group(3))
        for line in summary:
            m = re.match(r"ASan: (\d+) reports", line)
            if m:
                run["asan"] = int(m.group(1))
            m = re.match(r"UBSan: (\d+) reports", line)
            if m:
                run["ubsan"] = int(m.group(1))
            m = re.match(r"crash files: (\d+)", line)
            if m:
                run["crash"] = int(m.group(1))
    log = read(os.path.join(d, "engine.log"))
    snd_first = snd_last = None
    for i, line in enumerate(log):
        m = CHECK_RE.search(line)
        if m:
            system, _, item = m.group(4).partition("/")
            run["checks"].append({"result": m.group(3), "system": system, "item": item, "detail": m.group(5), "t": int(m.group(1))})
            continue
        if "[sweep]" in line and re.search(r"\] FAIL (phase|plan)", line):
            run["driver_fail"].append(line.split("[sweep] ", 1)[-1])
        if LUA_FATAL_RE.search(line):
            run["lua_errors"].append(line[:240])
        elif PRINTE_RE.match(line):
            k = norm(line)
            run["printe"][k] = run["printe"].get(k, 0) + 1
        m = SNAP_RE.search(line)
        if m:
            snap = [int(x) for x in m.groups()]
            snd_first = snd_first or snap
            snd_last = snap
    if snd_last:
        run["snd"] = {"first": snd_first, "last": snd_last}
    for line in read(os.path.join(d, "shell.log")):
        m = FOOT_RE.match(line)
        if m:
            run["footprint"].append((float(m.group(1)), int(m.group(2))))
    return run


def slope(points):
    """MB per minute of a least-squares line through the footprint samples, ignoring the first 60 s (loading)."""
    pts = [(t, v) for t, v in points if t > 60]
    if len(pts) < 6:
        return None
    n = len(pts)
    mt = sum(t for t, _ in pts) / n
    mv = sum(v for _, v in pts) / n
    den = sum((t - mt) ** 2 for t, _ in pts)
    if den == 0:
        return None
    return 60.0 * sum((t - mt) * (v - mv) for t, v in pts) / den


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("runs", nargs="+")
    args = ap.parse_args()
    runs = [parse_run(d) for d in args.runs if os.path.isdir(d)]

    # sound: per run and as a whole
    snd_total = {"started": 0, "played": 0, "errors": 0}
    for r in runs:
        if r["snd"]:
            f, l = r["snd"]["first"], r["snd"]["last"]
            r["snd_delta"] = {"started": l[0] - f[0], "played": l[1] - f[1], "errors": l[8]}
            snd_total["errors"] += l[8]
            if r["plan"] == "chk_sound":
                snd_total["started"], snd_total["played"] = l[0] - f[0], l[1] - f[1]
                r["checks"].append({"result": "PASS" if l[0] - f[0] >= 30 else "FAIL", "system": "sound", "item": "events-reach-the-renderer",
                                    "detail": f"{l[0] - f[0]} sound events started in the run's {len(r['footprint']) * 10} s (first counter line {f[0]}, last {l[0]})", "t": 0})
                r["checks"].append({"result": "PASS" if l[1] - f[1] >= 10 else "FAIL", "system": "sound", "item": "sources-played",
                                    "detail": f"{l[1] - f[1]} sources handed to alSourcePlay, {l[2] - f[2]} looped rewinds, {l[3] - f[3]} underrun restarts, {l[4] - f[4]} stops", "t": 0})
                r["checks"].append({"result": "PASS" if l[8] == 0 else "FAIL", "system": "sound", "item": "al-errors",
                                    "detail": f"{l[8]} AL errors after play/queue calls in this run", "t": 0})

    # memory (soak or any long run)
    for r in runs:
        s = slope(r["footprint"])
        r["slope"] = s
        if r["plan"] == "chk_soak" and r["footprint"]:
            peak = max(v for _, v in r["footprint"])
            minutes = (r["footprint"][-1][0] - r["footprint"][0][0]) / 60.0
            ok = s is not None and s < 5.0
            r["checks"].append({"result": "PASS" if ok else "FAIL", "system": "soak", "item": "memory-slope",
                                "detail": f"footprint {r['footprint'][0][1]} -> {r['footprint'][-1][1]} MB over {minutes:.0f} min (peak {peak} MB), slope {s:+.1f} MB/min" if s is not None else "not enough samples", "t": 0})

    total_pass = total_fail = 0
    plan_rows = []
    verdict_fail = False
    for r in runs:
        p = sum(1 for c in r["checks"] if c["result"] == "PASS")
        f = sum(1 for c in r["checks"] if c["result"] == "FAIL")
        total_pass += p
        total_fail += f
        bad = []
        if f:
            bad.append(f"{f} failed")
        if r["asan"]:
            bad.append(f"ASan {r['asan']}")
        if r["crash"]:
            bad.append(f"{r['crash']} crash file(s)")
        if r["lua_errors"]:
            bad.append(f"{len(r['lua_errors'])} Lua error line(s)")
        if r["driver_fail"]:
            bad.append("driver failed")
        if r["exit"] not in (0, None):
            bad.append(f"exit {r['exit']}")
        if bad:
            verdict_fail = True
        plan_rows.append((r, p, f, bad))

    lines = []
    lines.append("# Systems check results\n")
    lines.append(f"{len(runs)} runs, {total_pass} checks passed, {total_fail} failed. Sound: {snd_total['errors']} AL errors in all runs.\n")
    lines.append("## Runs\n")
    lines.append("| Plan | Checks | ASan | UBSan | Crash | Lua error lines | Time | Footprint MB (peak, slope/min) | Verdict |")
    lines.append("|---|---|---|---|---|---|---|---|---|")
    for r, p, f, bad in plan_rows:
        foot = "-"
        if r["footprint"]:
            # a slope means something only for a steady state (the soak); level loads and a first minute dominate the others
            foot = f"{max(v for _, v in r['footprint'])}" + (f", {r['slope']:+.1f}" if r["slope"] is not None and r["plan"] == "chk_soak" else "")
        lines.append(f"| {r['plan']} | {p} pass / {f} fail | {r['asan']} | {r['ubsan']} | {r['crash']} | {len(r['lua_errors'])} | {r['seconds'] if r['seconds'] is not None else '-'} s | {foot} | {'**' + ', '.join(bad) + '**' if bad else 'ok'} |")
    lines.append("")

    # per system
    by_system = {}
    for r in runs:
        for c in r["checks"]:
            by_system.setdefault(c["system"], []).append((r["plan"], c))
    lines.append("## Checks by system\n")
    for system in sorted(by_system):
        rows = by_system[system]
        fails = sum(1 for _, c in rows if c["result"] == "FAIL")
        lines.append(f"### {system}: {len(rows) - fails} pass, {fails} fail\n")
        lines.append("| Plan | Item | Result | Detail |")
        lines.append("|---|---|---|---|")
        for plan, c in rows:
            detail = c["detail"].replace("|", "/")
            lines.append(f"| {plan} | {c['item']} | {c['result']} | {detail} |")
        lines.append("")
    # errors
    lines.append("## Lua errors and script error prints\n")
    any_err = False
    for r in runs:
        if r["lua_errors"] or r["printe"] or r["driver_fail"]:
            any_err = True
            lines.append(f"### {r['plan']}\n")
            for d in r["driver_fail"]:
                lines.append(f"- driver: `{d}`")
            seen = set()
            for e in r["lua_errors"]:
                k = norm(e)
                if k not in seen:
                    seen.add(k)
                    lines.append(f"- `{e}`")
            for k, n in sorted(r["printe"].items(), key=lambda kv: -kv[1])[:12]:
                lines.append(f"- x{n} `{k}`")
            lines.append("")
    if not any_err:
        lines.append("None.\n")
    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "results.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    with open(os.path.join(args.out, "results.json"), "w") as f:
        json.dump([{k: v for k, v in r.items() if k != "footprint"} for r in runs], f, indent=1)
    verdict = "FAIL" if (verdict_fail or total_fail) else "PASS"
    print(f"{verdict}: {len(runs)} runs, {total_pass} checks passed, {total_fail} failed; report {os.path.join(args.out, 'results.md')}")
    return 1 if verdict == "FAIL" else 0


if __name__ == "__main__":
    sys.exit(main())
