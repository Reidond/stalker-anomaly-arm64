#!/usr/bin/env python3
"""Syntax-check engine module translation units with the flags CMake would use, without building anything.

Configure once with every module on (the module targets are opt-in), then probe:

    cmake -S . -B build/probe -G Ninja -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \\
          -DANOMALY_BUILD_ALL_MODULES=ON
    tools/syntax_probe.py build/probe --module xrGame --sample 80
    tools/syntax_probe.py build/probe --all --sample 30 --raw /tmp/probe.txt

The script reads <build>/compile_commands.json, picks translation units of the named module(s) (an evenly spaced
sample, or --all-files for every one), reruns each compile command with -fsyntax-only and prints, per module:
the number of TUs with no error, the error count and the number of unique error sites (file:line:message). Every
error is attributed to the module whose directory holds the file it points at. Errors in src/xrCore, src/Common
and src/Include count as "shim" errors, and errors in the Metal D3D11 headers as "d3d11"; the rest are "module"
errors (the module's own sources and headers) or "other:<module>" when they come from another module's headers.
--raw keeps the full compiler output for reading.
"""
import argparse
import collections
import concurrent.futures
import json
import os
import re
import shlex
import subprocess
import sys

MODULE_DIRS = {
    'xrCore': ['src/xrCore'],
    'xrCDB': ['src/xrCDB'],
    'xrXMLParser': ['src/xrXMLParser'],
    'xrParticles': ['src/xrParticles'],
    'xrCPU_Pipe': ['src/xrCPU_Pipe'],
    'xrSound': ['src/xrSound'],
    'xrPhysics': ['src/xrPhysics'],
    'xrNetServer': ['src/xrNetServer'],
    'xrEngine': ['src/xrEngine'],
    'xrGame': ['src/xrGame', 'src/xrServerEntities'],
    'xrAPI': ['src/Layers/xrAPI'],
    'xrRender_R4': ['src/Layers/xrRender', 'src/Layers/xrRenderDX10', 'src/Layers/xrRenderPC_R4'],
    'lua_extensions': ['src/3rd party/lua-extensions'],
}
SHIM_DIRS = ['src/xrCore', 'src/Common', 'src/Include']
D3D11_DIRS = ['src/Platform']
THIRD_PARTY = ['src/3rd party', 'external/', 'sdk/']

ERROR_RE = re.compile(r'^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?:\d+:)? (?P<kind>error|fatal error): (?P<msg>.*)$')


def rel(path, root):
    try:
        return os.path.relpath(os.path.realpath(path), root)
    except ValueError:
        return path


def attribute(path, module, root):
    """Where an error lives: module (the probed one), shim, d3d11, third_party or other:<module>."""
    p = rel(path, root).replace('\\', '/')
    for d in MODULE_DIRS.get(module, []):
        if p.lower().startswith(d.lower() + '/'):
            return 'module'
    for d in SHIM_DIRS:
        if p.lower().startswith(d.lower() + '/'):
            return 'shim'
    for d in D3D11_DIRS:
        if p.startswith(d + '/'):
            return 'd3d11'
    for d in THIRD_PARTY:
        if p.startswith(d):
            return 'third_party'
    for name, dirs in MODULE_DIRS.items():
        for d in dirs:
            if p.lower().startswith(d.lower() + '/'):
                return 'other:' + name
    return 'system'


def module_of(path, root):
    p = rel(path, root).replace('\\', '/')
    best = None
    for name, dirs in MODULE_DIRS.items():
        for d in dirs:
            if p.lower().startswith(d.lower() + '/') and (best is None or len(d) > best[1]):
                best = (name, len(d))
    return best[0] if best else None


def syntax_command(entry, extra_flags):
    args = shlex.split(entry['command'])
    out = []
    skip = 0
    for a in args:
        if skip:
            skip -= 1
            continue
        if a in ('-o', '-MT', '-MF', '-MQ'):
            skip = 1
            continue
        if a in ('-MD', '-MMD', '-MP'):
            continue
        out.append(a)
    out += ['-fsyntax-only', '-ferror-limit=0'] + extra_flags
    return out


def run(entry, extra_flags):
    cmd = syntax_command(entry, extra_flags)
    p = subprocess.run(cmd, cwd=entry['directory'], capture_output=True, text=True, errors='replace')
    return entry['file'], p.returncode, p.stderr


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('build_dir')
    ap.add_argument('--module', action='append', default=[], help='module to probe (repeatable)')
    ap.add_argument('--all', action='store_true', help='probe every module that has translation units')
    ap.add_argument('--sample', type=int, default=30, help='TUs per module (evenly spaced); default 30')
    ap.add_argument('--all-files', action='store_true', help='every TU of the selected modules')
    ap.add_argument('--file', action='append', default=[], help='probe TUs whose path contains this text')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--raw', help='write the full compiler output here')
    ap.add_argument('--top', type=int, default=12, help='most frequent error sites to print per module')
    ap.add_argument('--flag', action='append', default=[], help='extra compiler flag (repeatable)')
    ap.add_argument('--warnings', action='store_true', help='keep warnings in the raw output (default: -w)')
    args = ap.parse_args()

    build = os.path.realpath(args.build_dir)
    root = os.path.realpath(os.path.join(build, os.pardir, os.pardir))
    with open(os.path.join(build, 'compile_commands.json')) as f:
        entries = json.load(f)

    by_module = collections.defaultdict(list)
    for e in entries:
        m = module_of(e['file'], root)
        if m and m != 'xrCore' or (m == 'xrCore' and 'xrCore' in args.module):
            by_module[m].append(e)
    for m in by_module:
        by_module[m].sort(key=lambda e: e['file'])

    modules = sorted(by_module) if args.all else args.module
    if not modules and not args.file:
        ap.error('give --module, --file or --all')
    selected = []
    for m in modules:
        tus = by_module.get(m, [])
        if args.all_files or len(tus) <= args.sample:
            chosen = tus
        else:
            step = len(tus) / args.sample
            chosen = [tus[int(i * step)] for i in range(args.sample)]
        selected += [(m, e) for e in chosen]
    for text in args.file:
        for e in entries:
            if text in e['file']:
                selected.append((module_of(e['file'], root) or '?', e))

    extra = list(args.flag)
    if not args.warnings:
        extra.append('-w')
    raw = open(args.raw, 'w') if args.raw else None
    results = collections.defaultdict(lambda: {'tus': 0, 'clean': 0, 'sites': collections.Counter(),
                                                'total': collections.Counter(), 'where': collections.Counter()})
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        futures = {pool.submit(run, e, extra): (m, e) for m, e in selected}
        for fut in concurrent.futures.as_completed(futures):
            m, e = futures[fut]
            src, code, err = fut.result()
            r = results[m]
            r['tus'] += 1
            errors = 0
            for line in err.splitlines():
                mt = ERROR_RE.match(line)
                if not mt:
                    continue
                errors += 1
                where = attribute(mt['file'], m, root)
                site = (rel(mt['file'], root), mt['line'], mt['msg'])
                r['sites'][(where,) + site] += 1
                r['total'][where] += 1
            if errors == 0 and code == 0:
                r['clean'] += 1
            if raw:
                raw.write('===== %s (exit %d)\n%s\n' % (rel(src, root), code, err))
    if raw:
        raw.close()

    print('%-16s %5s %6s | %-34s | %s' % ('module', 'TUs', 'clean', 'errors by origin (total)', 'unique sites (module/shim/other)'))
    for m in sorted(results):
        r = results[m]
        by_where = collections.Counter()
        for (where, *_), n in r['sites'].items():
            by_where[where] += 1
        total = ' '.join('%s=%d' % (k, v) for k, v in sorted(r['total'].items())) or '-'
        sites = ' '.join('%s=%d' % (k, v) for k, v in sorted(by_where.items())) or '-'
        print('%-16s %5d %6d | %-34s | %s' % (m, r['tus'], r['clean'], total, sites))
    print()
    for m in sorted(results):
        r = results[m]
        if not r['sites']:
            continue
        print('--- %s: most frequent error sites' % m)
        for (where, f, line, msg), n in r['sites'].most_common(args.top):
            print('  %5d  [%s] %s:%s: %s' % (n, where, f, line, msg[:130]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
