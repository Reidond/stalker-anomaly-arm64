#!/usr/bin/env python3
"""Build xrPhysics and ODE with different -ffp-contract settings and link xrphysics_smoke against each variant.

    cmake --preset macos-arm64-release -DANOMALY_BUILD_XRPHYSICS=ON -DANOMALY_BUILD_XRCDB=ON
    cmake --build --preset macos-arm64-release --target xrphysics_smoke
    tools/xrphysics_smoke/fp_contract_experiment.py build/macos-arm64-release /tmp/fp          # all four variants
    for v in off_off on_off off_on on_on; do /tmp/fp/$v/xrphysics_smoke --stress 60 --seed 3 --digest; done

A variant name is <xrPhysics>_<ODE>, each `on` (-ffp-contract=on: clang fuses a*b+c inside a statement, the default
on arm64) or `off`. The compile commands come from the CMake build (`ninja -t commands xrphysics_smoke`); only the
flag changes. ODE's quickstep.cpp is compiled from a patched copy with a fixed shuffle seed in every variant (the
original seeds std::mt19937 from std::random_device), so one variant gives the same digest on every run and two
variants can be compared. RELINK=1 skips the compilation and only relinks (after rebuilding main.cpp).
"""
import concurrent.futures
import os
import re
import shlex
import subprocess
import sys

build, scratch = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
variants = sys.argv[3:] or ['off_off', 'on_off', 'off_on', 'on_on']
src_root = os.path.abspath(os.path.join(build, '..', '..'))
ode_src = os.path.join(src_root, 'src', '3rd party', 'ode', 'ode', 'src')

cmds = subprocess.run(['ninja', '-C', build, '-t', 'commands', 'xrphysics_smoke'], capture_output=True, text=True,
                      check=True).stdout.splitlines()

compiles = []  # (group, source, argv)
link = None
for line in cmds:
    if ' -c ' in line and ('xrPhysics.dir' in line or 'ode.dir' in line):
        argv = shlex.split(line.strip().lstrip(': &').strip())
        # the command may start with "cd dir &&"; the ninja commands here are plain
        out_i = argv.index('-o')
        src = argv[argv.index('-c') + 1]
        group = 'phys' if 'xrPhysics.dir' in argv[out_i + 1] else 'ode'
        compiles.append((group, src, argv))
    elif 'xrphysics_smoke.dir/main.cpp.o' in line and '-o tools/xrphysics_smoke' in line:
        link = shlex.split(line.strip().lstrip(': &').strip().rstrip('&& :').strip())
print(len(compiles), 'compile commands, link:', bool(link))

os.makedirs(scratch, exist_ok=True)
patched_dir = os.path.join(scratch, 'patched')
os.makedirs(patched_dir, exist_ok=True)
qs = open(os.path.join(ode_src, 'quickstep.cpp'), encoding='utf-8', errors='surrogateescape').read()
qs2 = qs.replace('std::mt19937(std::random_device()())', 'std::mt19937(12345u)')
assert qs != qs2
patched_qs = os.path.join(patched_dir, 'quickstep.cpp')
open(patched_qs, 'w', encoding='utf-8', errors='surrogateescape').write(qs2)


def strip_deps(argv):
    out, i = [], 0
    while i < len(argv):
        if argv[i] in ('-MT', '-MF', '-MQ'):
            i += 2
            continue
        if argv[i] in ('-MD', '-MMD'):
            i += 1
            continue
        out.append(argv[i])
        i += 1
    return out


def compile_one(job):
    group, src, argv, vdir, contract = job
    argv = strip_deps(argv)
    out_i = argv.index('-o')
    obj_rel = argv[out_i + 1]
    c_i = argv.index('-c')
    if src.endswith('quickstep.cpp') and group == 'ode':
        argv[c_i + 1] = patched_qs
        argv.insert(1, '-I' + ode_src)
        out_i = argv.index('-o')
    obj = os.path.join(vdir, group, obj_rel.replace('/', '__'))
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    argv[out_i + 1] = obj
    argv = [('-ffp-contract=' + contract) if a.startswith('-ffp-contract=') else a for a in argv]
    if not any(a.startswith('-ffp-contract=') for a in argv):
        argv.insert(1, '-ffp-contract=' + contract)
    r = subprocess.run(argv, cwd=build, capture_output=True, text=True)
    if r.returncode:
        print('FAILED', src, r.stderr[-2000:])
        sys.exit(1)
    return group, obj


for v in variants:
    ph, od = v.split('_')
    vdir = os.path.join(scratch, v)
    jobs = [(g, s, a, vdir, ph if g == 'phys' else od) for (g, s, a) in compiles]
    if os.environ.get('RELINK'):
        results = []
        for g, s_, a, vd, c in jobs:
            ob = a[a.index('-o') + 1]
            results.append((g, os.path.join(vdir, g, ob.replace('/', '__'))))
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as ex:
            results = list(ex.map(compile_one, jobs))
    libs = {}
    for group in ('phys', 'ode'):
        objs = [o for g, o in results if g == group and not o.endswith('quickstep.cpp.o')]
        lib = os.path.join(vdir, 'lib%s.a' % group)
        if os.path.exists(lib):
            os.remove(lib)
        subprocess.run(['ar', 'qc', lib] + objs, check=True)
        libs[group] = lib
    qobj = [o for g, o in results if o.endswith('quickstep.cpp.o')]
    argv = list(link)
    o_i = argv.index('-o')
    exe = os.path.join(vdir, 'xrphysics_smoke')
    argv[o_i + 1] = exe
    new = []
    for a in argv:
        if a == 'libxrPhysics.a':
            new.append(libs['phys'])
        elif a.endswith('libode.a'):
            new += qobj + [libs['ode']]
        else:
            new.append(a)
    r = subprocess.run(new, cwd=build, capture_output=True, text=True)
    if r.returncode:
        print('LINK FAILED', r.stderr[-3000:])
        sys.exit(1)
    print('built', exe)
