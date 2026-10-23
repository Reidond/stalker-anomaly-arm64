#!/usr/bin/env python3
"""Groups the sanitizer reports in a stderr capture of the app (tools/sweep/run.sh writes stderr.txt).

  tools/sweep/triage.py stderr.txt [--frames N] [--ubsan-top N]

ASan: one line per distinct (kind, top <N> frames in the engine's own code), with the count and the first line number in the
file. UBSan: one line per distinct (file:line, check) with the count. TSan: one line per distinct (kind, the two accesses' top frames
in the engine's code), with the count. Paths are shortened to src/... and addresses dropped.
"""
import argparse
import collections
import re
import sys

SRC = re.compile(r".*?/(src|external)/")
FRAME = re.compile(r"^\s+#(\d+) 0x[0-9a-f]+ in (.*?) (\S+?:\d+)(?::\d+)?$")
FRAME_NOFILE = re.compile(r"^\s+#(\d+) 0x[0-9a-f]+ in (.*?) \((\S+?)\+0x[0-9a-f]+\)$")


def short_path(p):
    return SRC.sub(lambda m: m.group(1) + "/", p)


def short_fn(fn):
    fn = re.sub(r"\(.*", "", fn)  # drop the argument list
    fn = re.sub(r"<.*>", "<>", fn)
    return fn.strip()


def asan(lines, nframes):
    groups = collections.OrderedDict()
    i = 0
    while i < len(lines):
        m = re.match(r"==\d+==ERROR: AddressSanitizer: (\S+)", lines[i])
        if not m:
            i += 1
            continue
        kind = m.group(1)
        first = i
        access = ""
        frames = []
        i += 1
        section = "error"
        while i < len(lines) and not lines[i].startswith("SUMMARY:"):
            line = lines[i]
            if re.match(r"^(READ|WRITE) of size", line):
                access = line.split(" at ")[0]
            if line.startswith("0x") and "located" in line:
                pass
            if re.search(r"(freed by thread|allocated by thread|is located in stack|previously allocated)", line):
                section = "other"
            fm = FRAME.match(line)
            if fm and section == "error":
                frames.append((short_fn(fm.group(2)), short_path(fm.group(3))))
            i += 1
        # frames in our code first: skip the sanitizer runtime, libc and the C++ library
        mine = [f for f in frames if f[1].split("/")[0] in ("src", "external")]
        top = mine[:nframes] or frames[:nframes]
        key = (kind, access, tuple(top))
        if key not in groups:
            groups[key] = [0, first + 1]
        groups[key][0] += 1
    return groups


TSAN_FRAME = re.compile(r"^\s+#(\d+) (.*?) (\S+?:\d+)(?::\d+)? \(")
TSAN_ACCESS = re.compile(r"^\s+(Write|Read|Atomic write|Atomic read|Previous write|Previous read|Previous atomic write|Previous atomic read)"
                         r" of size (\d+) at 0x[0-9a-f]+ by (main thread|thread T\d+)")


def tsan(lines, nframes):
    """Blocks between '==================' lines that start with 'WARNING: ThreadSanitizer: <kind>'."""
    groups = collections.OrderedDict()
    i = 0
    while i < len(lines):
        m = re.match(r"^WARNING: ThreadSanitizer: (.*?)(?: \(pid=\d+\))?$", lines[i])
        if not m:
            i += 1
            continue
        kind = m.group(1)
        first = i
        stacks = []  # (what, [frames])
        cur = None
        i += 1
        while i < len(lines) and not lines[i].startswith("==================") and not lines[i].startswith("SUMMARY:"):
            line = lines[i]
            am = TSAN_ACCESS.match(line)
            if am:
                cur = (am.group(1).replace("Previous ", "prev "), [])
                stacks.append(cur)
            elif re.match(r"^\s+(Location|Thread T\d+|Mutex|As if|Heap block|Previous|Cycle)", line):
                cur = None  # context lines (thread creation stacks, locations): not an access
            else:
                fm = TSAN_FRAME.match(line)
                if fm and cur is not None:
                    cur[1].append((short_fn(fm.group(2)), short_path(fm.group(3))))
            i += 1
        top = []
        for what, frames in stacks[:2]:
            mine = [f for f in frames if f[1].split("/")[0] in ("src", "external")]
            top.append((what, tuple((mine or frames)[:nframes])))
        key = (kind, tuple(top))
        if key not in groups:
            groups[key] = [0, first + 1]
        groups[key][0] += 1
    return groups


def ubsan(lines):
    groups = collections.OrderedDict()
    for n, line in enumerate(lines, 1):
        m = re.match(r"^(\S+?:\d+):\d+: runtime error: (.*)$", line)
        if not m:
            continue
        where = short_path(m.group(1))
        msg = re.sub(r"0x[0-9a-f]+", "ADDR", m.group(2))
        msg = re.sub(r"-?\d+", "N", msg) if "shift" in msg or "overflow" in msg or "index" in msg else msg
        key = (where, msg)
        if key not in groups:
            groups[key] = [0, n]
        groups[key][0] += 1
    return groups


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stderr")
    ap.add_argument("--frames", type=int, default=3)
    ap.add_argument("--ubsan-top", type=int, default=60)
    args = ap.parse_args()
    with open(args.stderr, errors="replace") as f:
        lines = f.read().splitlines()

    a = asan(lines, args.frames)
    print(f"ASan: {sum(v[0] for v in a.values())} reports, {len(a)} distinct")
    for (kind, access, frames), (count, line) in a.items():
        print(f"  {count:4d}x {kind} {access} (line {line})")
        for fn, loc in frames:
            print(f"          {fn} {loc}")
    t = tsan(lines, args.frames)
    if t:
        print(f"TSan: {sum(v[0] for v in t.values())} reports, {len(t)} distinct")
        for (kind, accesses), (count, line) in sorted(t.items(), key=lambda kv: -kv[1][0]):
            print(f"  {count:4d}x {kind} (line {line})")
            for what, frames in accesses:
                print(f"          {what}:")
                for fn, loc in frames:
                    print(f"              {fn} {loc}")
    u = ubsan(lines)
    print(f"UBSan: {sum(v[0] for v in u.values())} reports, {len(u)} distinct places")
    for (where, msg), (count, line) in sorted(u.items(), key=lambda kv: -kv[1][0])[: args.ubsan_top]:
        print(f"  {count:5d}x {where}: {msg} (line {line})")


if __name__ == "__main__":
    sys.exit(main())
