#!/usr/bin/env python3
"""Turns the memory runs of tools/render_harness/mem_report.sh into markdown tables (docs/web-port/MEMORY.md).

    mem_report.py [--static-mb N] [--stacks-mb N] <run-dir> [<run-dir> ...]
    mem_report.py [--static-mb N] [--stacks-mb N] --summary <profile-dir>    one table: the scenarios of a profile
    mem_report.py [--static-mb N] [--stacks-mb N] --sweeps <sweep-dir> ...     one table: the setting sweeps

A run dir (build/render-harness/<tag>/<scene>) holds mem.csv (the sampler of --memstat), shell.log and engine.log. The sampler's
rows are split into phases at its marks ("engine up", "level ready <name>", the scene's own `mark` lines), and for each phase the
tables give the PEAK and the STEADY value (median of the phase's last 10 s) of every part of what a 32-bit WebAssembly heap would
have to hold:

  malloc        malloc_zone_statistics, bytes in live blocks (engine, OpenAL, the C++ runtime; LuaJIT is not in it)
  Lua           collectgarbage("count") from the scene's xr_harness.mem() lines, held until the next sample (the Mac's LuaJIT allocates
                outside malloc; plain Lua on the web would be inside the heap). Nothing can be sampled while a level loads, so
                the load rows carry the last value from before it
  windows       live mmap windows of the file readers (LocatorAPI, stream readers, loose files): heap buffers on the web
  Metal CPU     what the Metal shim keeps CPU-visible that a WebGPU backend would hold in the heap: dynamic buffers, staging
                buffers and textures, and the largest one-frame use of the per-frame rings
  stacks+static thread stacks (estimate, --stacks-mb) and the program's static data (--static-mb)

  heap = their sum. GPU-only memory (private textures and render targets, shared textures filled once, static buffers) is listed
  apart and is not heap. Everything is in MB (1 MB = 2^20 bytes).
"""
import csv
import re
import statistics
import sys
from pathlib import Path

MB = 1048576.0


def parse_csv(path):
    rows, marks, header = [], [], None
    with open(path, newline="") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            if line.startswith("#mark,"):
                _, t, label = line.split(",", 2)
                marks.append((float(t), label))
            elif header is None:
                header = line.split(",")
            else:
                parts = line.split(",")
                if len(parts) == len(header):
                    rows.append({h: float(v) for h, v in zip(header, parts)})
    return rows, marks


def shell_times(path):
    """(shell timestamp, text) of every 'anomaly:' line of the shell log."""
    out = []
    pat = re.compile(r"^\[\s*([\d.]+)\] anomaly: (.*)$")
    with open(path, errors="replace") as f:
        for line in f:
            m = pat.match(line.rstrip("\n"))
            if m:
                out.append((float(m.group(1)), m.group(2)))
    return out


def lua_samples(run, marks):
    """[(csv time, KB)] from the HARNESS MEM lines of engine.log, timed by the shell log's script steps."""
    eng = run / "engine.log"
    sh = run / "shell.log"
    if not eng.exists() or not sh.exists():
        return []
    shell = shell_times(sh)
    # offset between the shell log's clock and the sampler's: from the marks both wrote
    offs = []
    shell_marks = [(t, txt[len("mem: mark "):]) for t, txt in shell if txt.startswith("mem: mark ")]
    for (ts, st), (tc, ct) in zip(shell_marks, marks):
        if st == ct:
            offs.append(ts - tc)
    if not offs:
        return []
    off = statistics.median(offs)
    steps = []  # (shell ts, tag)
    pat = re.compile(r'script t=[\d.]+ console run_string xr_harness\.mem\("(.*)"\)')
    for ts, txt in shell:
        m = pat.search(txt)
        if m:
            steps.append((ts - off, m.group(1)))
    vals = []
    mpat = re.compile(r"HARNESS MEM (.*?) lua_kb=(\d+) level=(\S+)")
    with open(eng, errors="replace") as f:
        for line in f:
            m = mpat.search(line)
            if m:
                vals.append((m.group(1), float(m.group(2)), m.group(3)))
    out = []
    used = 0
    for t, tag in steps:
        while used < len(vals) and vals[used][0] != tag:
            used += 1
        if used < len(vals):
            out.append((t, vals[used][1] * 1024.0, vals[used][2]))
            used += 1
    return out


def load_start_time(run, marks):
    """The sampler's time of the first level load: the new-game click mark, or the script's `start server(...)` step."""
    for t, label in marks:
        if label.startswith("new game started"):
            return t
    sh = run / "shell.log"
    if not sh.exists():
        return None
    shell = shell_times(sh)
    shell_marks = [(t, txt[len("mem: mark "):]) for t, txt in shell if txt.startswith("mem: mark ")]
    offs = [ts - tc for (ts, st), (tc, ct) in zip(shell_marks, marks) if st == ct]
    if not offs:
        return None
    off = statistics.median(offs)
    for ts, txt in shell:
        if "console start server(" in txt:
            return ts - off
    return None


def summarize(run, static_mb, stacks_mb):
    rows, marks = parse_csv(run / "mem.csv")
    if not rows:
        return None
    lua = lua_samples(run, marks)
    ci = 0
    cur_lua = 0.0
    first_lua = lua[0] if lua else None
    load_start = load_start_time(run, marks)
    for r in rows:
        while ci < len(lua) and lua[ci][0] <= r["t_s"]:
            cur_lua = lua[ci][1]
            ci += 1
        r["lua"] = cur_lua
        # nothing can be sampled while the first level loads: from the moment the load starts, hold the first sample's value
        if ci == 0 and first_lua and load_start is not None and r["t_s"] >= load_start:
            r["lua"] = first_lua[1]
        r["shim_cpu"] = r.get("buf_dynamic", 0) + r.get("buf_staging", 0) + r.get("tex_staging", 0) + r.get("ring_frame_peak", 0)
        r["gpu_only"] = r.get("tex_private", 0) + r.get("tex_shared_static", 0) + r.get("tex_dynamic", 0) + r.get("buf_static", 0)
        r["fixed"] = (static_mb + stacks_mb) * MB
        r["heap"] = r["malloc_in_use"] + r["lua"] + r["map_total"] + r["shim_cpu"] + r["fixed"]
    end = rows[-1]["t_s"]
    bounds = [0.0] + [t for t, _ in marks] + [end + 1e-6]
    labels = ["start"] + [l for _, l in marks]
    phases = []
    for i, label in enumerate(labels):
        a, b = bounds[i], bounds[i + 1]
        seg = [r for r in rows if a <= r["t_s"] < b]
        if not seg:
            continue
        tail = [r for r in seg if r["t_s"] >= seg[-1]["t_s"] - 10.0] or seg
        phases.append((label, a, b if i + 1 < len(labels) else end, seg, tail))
    return rows, marks, lua, phases


def mbf(v):
    return f"{v / MB:,.0f}"


def table(run, static_mb, stacks_mb):
    res = summarize(run, static_mb, stacks_mb)
    if not res:
        return f"### {run}\n\nno mem.csv rows\n"
    rows, marks, lua, phases = res
    cols = [("heap", "heap"), ("malloc_in_use", "malloc"), ("lua", "Lua"), ("map_total", "windows"), ("shim_cpu", "Metal CPU"),
            ("gpu_only", "GPU-only (not heap)"), ("footprint", "footprint"), ("metal_allocated", "Metal alloc")]
    out = [f"### {run.parent.name}/{run.name}", "",
           "Each cell is steady / peak in MB: steady is the median of the phase's last 10 s, peak the largest sample in the phase.", "",
           "| phase (from the mark) | s | " + " | ".join(c[1] for c in cols) + " | threads |",
           "|---|---|" + "---|" * (len(cols) + 1)]
    for label, a, b, seg, tail in phases:
        cells = []
        for key, _ in cols:
            steady = statistics.median(r[key] for r in tail)
            peak = max(r[key] for r in seg)
            cells.append(f"{mbf(steady)} / {mbf(peak)}")
        out.append(f"| {label} | {a:.0f}-{b:.0f} | " + " | ".join(cells) + f" | {int(max(r['threads'] for r in seg))} |")
    allpeak = max(rows, key=lambda r: r["heap"])
    out.append("")
    out.append(f"Run peak: heap {mbf(allpeak['heap'])} MB at {allpeak['t_s']:.1f} s "
               f"(malloc {mbf(allpeak['malloc_in_use'])}, Lua {mbf(allpeak['lua'])}, windows {mbf(allpeak['map_total'])}, "
               f"Metal CPU {mbf(allpeak['shim_cpu'])}, stacks+static {mbf(allpeak['fixed'])}); "
               f"footprint peak {mbf(max(r['footprint'] for r in rows))}; exact window peak {mbf(max(r['map_peak'] for r in rows))}; "
               f"malloc_allocated peak {mbf(max(r['malloc_allocated'] for r in rows))} (blocks the zones hold from the OS, fragmentation included).")
    if lua:
        out.append("")
        out.append("Lua heap samples: " + ", ".join(f"{v / MB:.0f} MB ({lv})" for _, v, lv in lua[:12]) + (" ..." if len(lua) > 12 else ""))
    out.append("")
    return "\n".join(out)


def window(rows, a, b):
    seg = [r for r in rows if a <= r["t_s"] < b]
    if not seg:
        return None
    tail = [r for r in seg if r["t_s"] >= seg[-1]["t_s"] - 10.0] or seg
    peak = max(seg, key=lambda r: r["heap"])
    steady = {k: statistics.median(r[k] for r in tail) for k in peak}
    return peak, steady


def mark_time(marks, prefix, nth=0):
    hits = [t for t, label in marks if label.startswith(prefix)]
    return hits[nth] if len(hits) > nth else None


def summary(profile_dir, static_mb, stacks_mb):
    """One table for a profile: the scenarios of mem_report.sh, each as a peak (the heap's largest sample, with the parts of that
    sample) and a steady state (medians of the window's last 10 s)."""
    out = []
    cols = [("heap", "heap"), ("malloc_in_use", "malloc"), ("lua", "Lua"), ("map_total", "windows"), ("shim_cpu", "Metal CPU"),
            ("fixed", "stacks+static"), ("footprint", "footprint"), ("gpu_only", "GPU-only")]
    out.append("| scenario | | " + " | ".join(c[1] for c in cols) + " |")
    out.append("|---|---|" + "---|" * len(cols))

    def row(name, kind, w):
        if w is None:
            return
        peak, steady = w
        src = peak if kind == "peak" else steady
        out.append(f"| {name} | {kind} | " + " | ".join(mbf(src[k]) for k, _ in cols) + " |")

    def load(name):
        res = summarize(profile_dir / name, static_mb, stacks_mb) if (profile_dir / name / "mem.csv").exists() else None
        if not res:
            return None
        rows, marks, _, _ = res
        return rows, marks, rows[-1]["t_s"] + 1e-6

    r = load("mem_menu")
    if r:
        rows, marks, end = r
        row("main menu, from process start (engine init included)", "peak", window(rows, 0, end))
        a = mark_time(marks, "menu idle begins") or 0
        row("main menu, idle 1 min", "steady", window(rows, a, end))
    r = load("_newgame")
    if r:
        rows, marks, end = r
        s, ready = mark_time(marks, "new game started"), mark_time(marks, "level ready")
        if s is not None and ready is not None:
            row("new game: load of the Cordon", "peak", window(rows, s, ready))
            row("new game: 30 s after the level is ready", "steady", window(rows, ready, end))
    r = load("mem_cordon")
    if r:
        rows, marks, end = r
        up, ready, begin = mark_time(marks, "engine up"), mark_time(marks, "level ready"), mark_time(marks, "cordon begins")
        if up is not None and ready is not None:
            row("Cordon from the base save: load", "peak", window(rows, up, ready))
        if begin is not None:
            row("Cordon, 5 min of play", "peak", window(rows, begin, end))
            row("Cordon, 5 min of play (end)", "steady", window(rows, begin, end))
    r = load("mem_levels")
    if r:
        rows, marks, end = r
        names = [("l02_garbage", "loading l02_garbage"), ("pripyat", "loading pripyat"), ("jupiter", "loading jupiter"),
                 ("l01_escape (again)", "loading l01_escape again")]
        first = mark_time(marks, "level ready l01_escape", 0)
        nxt = mark_time(marks, "loading l02_garbage")
        if first is not None and nxt is not None:
            row("level chain: Cordon (first), steady", "steady", window(rows, first, nxt))
        for i, (label, mk) in enumerate(names):
            a = mark_time(marks, mk)
            ready_t = [t for t, lb in marks if lb.startswith("level ready") and a is not None and t > a]
            if a is None or not ready_t:
                continue
            b = ready_t[0]
            c = mark_time(marks, names[i + 1][1]) if i + 1 < len(names) else end
            row(f"level change to {label}: load", "peak", window(rows, a, b))
            row(f"{label}: 40 s after ready", "steady", window(rows, b, c if c else end))
    return "\n".join(out)


def sweeps(dirs, static_mb, stacks_mb):
    """One row per sweep dir (<tag>/sweep-*/mem_cordon_short): the Cordon load peak and the steady state after it."""
    cols = [("heap", "heap"), ("malloc_in_use", "malloc"), ("map_total", "windows"), ("shim_cpu", "Metal CPU"), ("tex_staging", "staging textures"),
            ("gpu_only", "GPU-only"), ("footprint", "footprint")]
    out = ["| setting | | " + " | ".join(c[1] for c in cols) + " |", "|---|---|" + "---|" * len(cols)]
    for d in dirs:
        res = summarize(d / "mem_cordon_short", static_mb, stacks_mb) if (d / "mem_cordon_short" / "mem.csv").exists() else None
        if not res:
            continue
        rows, marks, _, _ = res
        up, ready, begin = mark_time(marks, "engine up"), mark_time(marks, "level ready"), mark_time(marks, "cordon begins")
        end = rows[-1]["t_s"] + 1e-6
        for kind, w in (("load peak", window(rows, up, ready) if up is not None and ready is not None else None),
                        ("steady", window(rows, begin, end) if begin is not None else None)):
            if w:
                src = w[0] if kind == "load peak" else w[1]
                out.append(f"| {d.name} | {kind} | " + " | ".join(mbf(src.get(k, 0)) for k, _ in cols) + " |")
    return "\n".join(out)


def main(argv):
    static_mb, stacks_mb = 0.0, 64.0
    runs = []
    i = 0
    while i < len(argv):
        if argv[i] == "--static-mb":
            static_mb = float(argv[i + 1])
            i += 2
        elif argv[i] == "--stacks-mb":
            stacks_mb = float(argv[i + 1])
            i += 2
        elif argv[i] == "--sweeps":
            print(sweeps([Path(a) for a in argv[i + 1:]], static_mb, stacks_mb))
            return 0
        elif argv[i] == "--summary":
            print(summary(Path(argv[i + 1]), static_mb, stacks_mb))
            return 0
        else:
            runs.append(Path(argv[i]))
            i += 1
    if not runs:
        print(__doc__)
        return 2
    for run in runs:
        if (run / "mem.csv").exists():
            print(table(run, static_mb, stacks_mb))
        else:
            print(f"### {run}\n\nno mem.csv\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
