#!/usr/bin/env python3
"""Summarise an Instruments Time Profiler export (xctrace export --xpath '.../table[@schema="time-profile"]').

    xcrun xctrace record --device <udid> --template 'Time Profiler' --attach <pid> --time-limit 8s --output run.trace
    xcrun xctrace export --input run.trace --xpath '/trace-toc/run[1]/data/table[@schema="time-profile"]' > tp.xml
    python3 -I tools/ios/tp_summary.py tp.xml [--thread NAME] [--depth N] [--top N]

Prints CPU time per thread, then for the busiest threads (or --thread) the heaviest leaf functions and the heaviest
"inclusive" functions (every function on the stack counted once per sample). Time Profiler samples only running
threads, so a thread that is blocked does not show up: a main thread that is missing from the list is waiting.
"""
import argparse
import collections
import sys
import xml.etree.ElementTree as ET


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xml")
    ap.add_argument("--thread", help="substring of the thread name to detail")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--threads", type=int, default=4, help="how many of the busiest threads to detail")
    args = ap.parse_args()

    root = ET.parse(args.xml).getroot()
    refs = {}

    def resolve(el):
        # an element is either defined (id=...) or a reference to a defined one (ref=...)
        if el is None:
            return None
        if "ref" in el.attrib:
            return refs.get(el.attrib["ref"])
        if "id" in el.attrib:
            refs[el.attrib["id"]] = el
        return el

    per_thread = collections.Counter()
    leaf = collections.defaultdict(collections.Counter)
    incl = collections.defaultdict(collections.Counter)
    total = 0
    for row in root.iter("row"):
        thread_el = resolve(row.find("thread"))
        weight_el = resolve(row.find("weight"))
        stack_el = resolve(row.find("tagged-backtrace"))
        if thread_el is None or weight_el is None:
            continue
        # also register every nested id so later refs resolve
        for sub in row.iter():
            if "id" in sub.attrib:
                refs.setdefault(sub.attrib["id"], sub)
        name = thread_el.attrib.get("fmt", "?")
        weight = int(weight_el.text or 0)
        per_thread[name] += weight
        total += weight
        if stack_el is None:
            continue
        bt = resolve(stack_el.find("backtrace")) or stack_el
        frames = []
        for fr in bt.iter("frame"):
            fr = resolve(fr)
            if fr is not None:
                frames.append(fr.attrib.get("name", "?"))
        if frames:
            leaf[name][frames[0]] += weight
            for fn in set(frames):
                incl[name][fn] += weight

    print(f"total {total / 1e6:.0f} ms of CPU in the window")
    for name, w in per_thread.most_common(12):
        print(f"  {w / 1e6:8.0f} ms  {name}")
    wanted = [n for n, _ in per_thread.most_common()]
    if args.thread:
        wanted = [n for n in wanted if args.thread in n]
    else:
        wanted = wanted[: args.threads]
    for name in wanted:
        print(f"\n== {name}: {per_thread[name] / 1e6:.0f} ms")
        print("-- leaf")
        for fn, w in leaf[name].most_common(args.top):
            print(f"  {w / 1e6:8.0f} ms  {fn}")
        print("-- inclusive")
        for fn, w in incl[name].most_common(args.top):
            print(f"  {w / 1e6:8.0f} ms  {fn}")


if __name__ == "__main__":
    sys.exit(main())
