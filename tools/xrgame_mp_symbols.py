#!/usr/bin/env python3
"""Which multiplayer symbols does single-player xrGame code still need? (the link check of the mp-stub shard)

With ANOMALY_XRGAME_MULTIPLAYER=OFF the mp-stub shard builds only mp-stub-compiled.txt (apple_mp_stubs.cpp and a few leaf
sources). Every symbol the other objects reference and only an excluded multiplayer source defines would be an unresolved
reference in libxrGame.a. This script lists them, per defining source, by comparing two builds:

    cmake -S . -B build/xrgame -G Ninja -DANOMALY_BUILD_XRGAME=ON                                  # multiplayer OFF (the default)
    cmake --build build/xrgame --target xrGame_shards -- -k 0
    cmake -S . -B build/mp -G Ninja -DANOMALY_BUILD_XRGAME=ON -DANOMALY_XRGAME_MULTIPLAYER=ON      # only for the dictionary
    cmake --build build/mp --target xrGame_mp_stub
    tools/xrgame_mp_symbols.py build/xrgame --mp-build build/mp

The first build is the one that is checked: its objects (every `CMakeFiles/xrGame_*.dir`) are the library. The second one is only
used to learn which symbols the excluded sources define; its other shards need not exist. Single-player code guarded with
XRGAME_NO_MULTIPLAYER is compiled differently in the two builds, which is why the check cannot be made on the multiplayer build.

Exit status 0 when nothing is left. The fix for a symbol, cheapest first: guard the single-player reference with
`#ifndef XRGAME_NO_MULTIPLAYER`, add the (leaf) source to cmake/targets/xrgame_shards/mp-stub-compiled.txt, or define the symbol in
src/xrGame/apple_mp_stubs.cpp (a class with a vtable needs all of its virtuals: guard the use instead). Symbols are demangled
for reading only.
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


def shard_objects(build_dir, only_mp_stub=False):
    cmake_files = os.path.join(build_dir, 'CMakeFiles')
    objs = []
    for d in sorted(os.listdir(cmake_files)):
        if d.startswith('xrGame_') and d.endswith('.dir') and (not only_mp_stub or d == 'xrGame_mp_stub.dir'):
            for dp, _, fn in os.walk(os.path.join(cmake_files, d)):
                objs += [os.path.join(dp, f) for f in fn if f.endswith('.o')]
    return objs


def scan(path):
    return path, nm(path, '-U'), nm(path, '-u')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('build_dir', help='build with ANOMALY_XRGAME_MULTIPLAYER=OFF, every xrGame_* shard built')
    ap.add_argument('--mp-build', required=True, help='build with ANOMALY_XRGAME_MULTIPLAYER=ON, xrGame_mp_stub built')
    ap.add_argument('--limit', type=int, default=40, help='symbols to list per defining source')
    args = ap.parse_args()

    objs = shard_objects(args.build_dir)
    mp_objs = shard_objects(args.mp_build, only_mp_stub=True)
    if not objs:
        sys.exit('no xrGame_* object files under %s/CMakeFiles (build target xrGame_shards first)' % args.build_dir)
    if not mp_objs:
        sys.exit('no xrGame_mp_stub object files under %s/CMakeFiles (build target xrGame_mp_stub with multiplayer on)'
                 % args.mp_build)

    with concurrent.futures.ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        lib = list(pool.map(scan, objs))
        mp = list(pool.map(scan, mp_objs))

    defined = set()
    for _, defs, _ in lib:
        defined |= defs
    defined_in = collections.defaultdict(set)  # symbol -> excluded multiplayer objects defining it
    for path, defs, _ in mp:
        for s in defs:
            defined_in[s].add(path)

    users = collections.defaultdict(set)  # symbol -> library objects that need it
    for path, _, undefs in lib:
        for s in undefs:
            if s not in defined and s in defined_in:
                users[s].add(path)

    needed = collections.defaultdict(list)
    for sym, paths in users.items():
        for path in defined_in[sym]:
            needed[path].append(sym)
    total = sum(len(v) for v in needed.values())
    print('%d object(s) checked; %d multiplayer symbol(s) in %d excluded source(s) are referenced by them\n'
          % (len(lib), total, len(needed)))
    for path, syms in sorted(needed.items(), key=lambda kv: -len(kv[1])):
        print('%4d  %s' % (len(syms), path.split('.dir/src/', 1)[-1]))
        syms = sorted(syms)
        names = subprocess.run(['c++filt'], input='\n'.join(syms), capture_output=True, text=True).stdout.splitlines()
        for n, sym in list(zip(names, syms))[:args.limit]:
            user = ', '.join(sorted(os.path.basename(p)[:-2] for p in users[sym]))
            print('        %s    <- %s' % (n[:100], user[:60]))
        if len(syms) > args.limit:
            print('        ... %d more' % (len(syms) - args.limit))
    return 1 if total else 0


if __name__ == '__main__':
    sys.exit(main())
