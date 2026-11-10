#!/usr/bin/env python3
"""Native (macOS arm64, RelWithDebInfo) against wasm (Node, Release): wall and CPU time of each smoke tool, median of N runs.

usage: python3 timing.py <repo root> [runs]
needs build/macos-arm64-release (configured with the five modules on, see docs/web-port/WASM-BUILD.md) and build/web-wasm32;
the game directory is found at <repo root>/../../../game/Anomaly (a worktree under .claude/worktrees). The tools write only
below <repo root>/build/timing-run. Close any running game first (pgrep -x anomaly): the machine is shared."""
import os, resource, statistics, subprocess, sys, time

ROOT = sys.argv[1]
N = int(sys.argv[2]) if len(sys.argv) > 2 else 7
NODE = os.environ.get("NODE", "node")
GAME = os.path.join(ROOT, "../../../game/Anomaly")
RUN = os.path.join(ROOT, "build/timing-run")  # created below; the tools write only here

def native(t): return os.path.join(ROOT, "build/macos-arm64-release/tools", t, t)
def wasm(t): return os.path.join(ROOT, "build/web-wasm32/tools", t, t + ".js")

CASES = [
    ("ode_smoke", [], []),
    ("xrcdb_smoke", [], []),
    ("xrcdb_smoke -mt_cdb", ["-mt_cdb"], []),
    ("xrparticles_smoke", [], []),
    ("xrcpu_pipe_smoke", [], []),
    ("xrphysics_smoke", [], []),
    ("xrcore_smoke (game data)", ["--game-dir", os.path.realpath(GAME), "--run-dir", RUN], []),
]

def run(cmd):
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    t0 = time.perf_counter()
    p = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    wall = time.perf_counter() - t0
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = (after.ru_utime - before.ru_utime) + (after.ru_stime - before.ru_stime)
    return p.returncode, wall, cpu

os.makedirs(RUN, exist_ok=True)
print("load", os.getloadavg(), "runs", N)
print("%-28s %10s %10s %10s %10s %7s" % ("tool", "native s", "wasm s", "native cpu", "wasm cpu", "wall x"))
for label, args, _ in CASES:
    tool = label.split()[0]
    res = {}
    for kind, base in (("native", [native(tool)]), ("wasm", [NODE, wasm(tool)])):
        walls, cpus = [], []
        for i in range(N + 1):
            rc, w, c = run(base + args)
            if rc != 0:
                print("FAILED", kind, label, rc)
            if i:  # the first run warms the file cache
                walls.append(w); cpus.append(c)
        res[kind] = (statistics.median(walls), statistics.median(cpus))
    n, w = res["native"], res["wasm"]
    print("%-28s %10.3f %10.3f %10.3f %10.3f %6.1fx" % (label, n[0], w[0], n[1], w[1], w[0] / n[0]))

