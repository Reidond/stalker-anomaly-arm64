#!/usr/bin/env python3
"""Turns the memory runs of tools/render_harness/mem_report.sh into markdown tables (docs/web-port/MEMORY.md).

    mem_report.py [--static-mb N] [--stacks-mb N] <run-dir> [<run-dir> ...]

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
