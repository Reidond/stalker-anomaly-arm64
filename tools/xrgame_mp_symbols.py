#!/usr/bin/env python3
"""Which multiplayer symbols does single-player xrGame code still need? (input for the mp-stub shard)

Build every shard object with the multiplayer sources included, then ask the object files:

    cmake -S . -B build/mp -G Ninja -DANOMALY_BUILD_XRGAME=ON -DANOMALY_XRGAME_MULTIPLAYER=ON -DANOMALY_XRGAME_PCH=ON
    cmake --build build/mp --target xrGame_shards -- -k 0
    tools/xrgame_mp_symbols.py build/mp

A symbol is *needed* when an object outside the mp-stub shard references it and only mp-stub objects define it. The
report groups those symbols by the defining multiplayer source: either keep that source in
cmake/targets/xrgame_shards/mp-stub-compiled.txt, or define the symbols in src/xrGame/apple_mp_stubs.cpp (the stub wins
when the symbol is a leaf such as a game-type factory or a GameSpy call). Symbols are demangled for reading only.
"""
import argparse
import collections
import concurrent.futures
import os
import subprocess
import sys


def nm(path, flag):
    p = subprocess.run(['nm', '-g', flag, '-j', path], capture_output=True, text=True, errors='replace')
    return set(l.strip() for l in p.stdout.splitlines() if l.strip())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('build_dir')
    ap.add_argument('--limit', type=int, default=40, help='symbols to list per defining source')
    args = ap.parse_args()

    objs = []  # (shard, path)
    cmake_files = os.path.join(args.build_dir, 'CMakeFiles')
    for d in sorted(os.listdir(cmake_files)):
        if d.startswith('xrGame_') and d.endswith('.dir'):
            shard = d[len('xrGame_'):-len('.dir')]
            for dp, _, fn in os.walk(os.path.join(cmake_files, d)):
                objs += [(shard, os.path.join(dp, f)) for f in fn if f.endswith('.o')]
    if not objs:
        sys.exit('no xrGame_* object files under %s/CMakeFiles (build target xrGame_shards first)' % args.build_dir)

    def scan(item):
        shard, path = item
        return shard, path, nm(path, '-U'), nm(path, '-u')

    defined_in = collections.defaultdict(set)   # symbol -> mp objects defining it
    defined_outside = set()
    undefined_outside = collections.defaultdict(set)  # symbol -> non-mp objects needing it
    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        for shard, path, defs, undefs in pool.map(scan, objs):
            if shard == 'mp_stub':
                for s in defs:
                    defined_in[s].add(path)
            else:
                defined_outside |= defs
                for s in undefs:
                    undefined_outside[s].add(path)

    needed = collections.defaultdict(list)
    for sym, users in undefined_outside.items():
        if sym in defined_outside or sym not in defined_in:
            continue
        for path in defined_in[sym]:
            needed[path].append((sym, len(users)))
    total = sum(len(v) for v in needed.values())
    print('%d multiplayer symbol(s) in %d source(s) are referenced by single-player objects\n' % (total, len(needed)))
    for path, syms in sorted(needed.items(), key=lambda kv: -len(kv[1])):
        src = os.path.relpath(path, cmake_files)
        print('%4d  %s' % (len(syms), src))
        names = subprocess.run(['c++filt'], input='\n'.join(s for s, _ in sorted(syms)), capture_output=True, text=True).stdout.splitlines()
        for n in names[:args.limit]:
            print('        ' + n)
        if len(names) > args.limit:
            print('        ... %d more' % (len(names) - args.limit))
    return 0


if __name__ == '__main__':
    sys.exit(main())
