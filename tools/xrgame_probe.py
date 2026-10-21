#!/usr/bin/env python3
"""Shard-aware syntax probe for xrGame (wraps tools/syntax_probe.py).

Configure with -DANOMALY_BUILD_XRGAME=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON (see docs/ios-port/XRGAME-SHARDS.md), then:

    tools/xrgame_probe.py build/probe                       # every xrGame TU, report per shard
    tools/xrgame_probe.py build/probe --shard ui            # only the TUs of one shard
    tools/xrgame_probe.py build/probe --shard ui --top 30   # more of the most frequent error sites
    tools/xrgame_probe.py build/probe --json out.json       # machine-readable result (diff before/after)
    tools/xrgame_probe.py build/probe --markdown            # the table as it appears in XRGAME-SHARDS.md

Every TU is rerun with -fsyntax-only (nothing is written). An error is attributed to the shard that owns the file the
message points at (cmake/targets/xrgame_shards/<shard>.txt, "shared" for the headers the triage session owns, "other"
for anything outside xrGame/xrServerEntities). A *foreign* site is an error in a file that another shard (or "shared")
owns: it is re-reported in every TU that includes the file and must be fixed by that file's owner, not by the shard
that sees it. The goal of the shard setup is zero shared and zero cross-shard sites.
"""
import argparse
import collections
import concurrent.futures
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.realpath(__file__))
spec = importlib.util.spec_from_file_location('syntax_probe', os.path.join(HERE, 'syntax_probe.py'))
sp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sp)

ROOT = os.path.realpath(os.path.join(HERE, os.pardir))
SHARD_DIR = os.path.join(ROOT, 'cmake', 'targets', 'xrgame_shards')
SHARDS = ['ai-monsters-a', 'ai-monsters-b', 'ai-core', 'nav-alife', 'script-entities', 'ui',
          'objects-items', 'objects-world', 'core-physics', 'mp-stub']


def load_owners():
    owner = {}
    for shard in SHARDS:
        path = os.path.join(SHARD_DIR, shard + '.txt')
        if os.path.exists(path):
            for line in open(path):
                line = line.strip()
                if line:
                    owner['src/' + line.lower()] = shard
    shared = os.path.join(SHARD_DIR, 'shared.files')
    if os.path.exists(shared):
        for line in open(shared):
            line = line.strip()
            if line:
                owner['src/' + line.lower()] = 'shared'
    return owner


def owner_of(relpath, owners):
    p = relpath.replace('\\', '/').lower()
    o = owners.get(p)
    if o:
        return o
    for d in ('src/xrgame/', 'src/xrserverentities/'):
        if p.startswith(d):
            return 'unowned'
    if p.startswith(('src/xrcore/', 'src/common/', 'src/include/')):
        return 'shim'
    if p.startswith(('src/3rd party/', 'external/', 'sdk/')):
        return 'third_party'
    if p.startswith('src/'):
        return 'other-module'
    return 'system'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('build_dir')
    ap.add_argument('--shard', action='append', default=[], help='only the TUs of this shard (repeatable)')
    ap.add_argument('--file', action='append', default=[], help='only TUs whose path contains this text')
    ap.add_argument('--sample', type=int, metavar='N', help='only N evenly spaced TUs per shard')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--top', type=int, default=15, help='most frequent error sites to print')
    ap.add_argument('--json', help='write the aggregated result here')
    ap.add_argument('--raw', help='write the full compiler output here')
    ap.add_argument('--from-raw', help='aggregate a --raw file written earlier instead of compiling (before/after tables)')
    ap.add_argument('--markdown', action='store_true', help='print the per-shard table in markdown')
    ap.add_argument('--flag', action='append', default=[], help='extra compiler flag (repeatable)')
    args = ap.parse_args()

    build = os.path.realpath(args.build_dir)
    with open(os.path.join(build, 'compile_commands.json')) as f:
        entries = json.load(f)
    owners = load_owners()

    tus = []
    for e in entries:
        rel = sp.rel(e['file'], ROOT).replace('\\', '/')
        if sp.module_of(e['file'], ROOT) != 'xrGame':
            continue
        shard = owners.get(rel.lower(), 'unowned')
        if args.shard and shard not in args.shard:
            continue
        if args.file and not any(t in rel for t in args.file):
            continue
        tus.append((shard, rel, e))
    tus.sort(key=lambda t: t[1])
    if args.sample:
        picked = []
        for shard in SHARDS + ['unowned']:
            mine = [t for t in tus if t[0] == shard]
            if len(mine) > args.sample:
                step = len(mine) / args.sample
                mine = [mine[int(i * step)] for i in range(args.sample)]
            picked += mine
        tus = picked
    if not tus:
        sys.exit('no xrGame translation units selected (is ANOMALY_BUILD_XRGAME on? are the shard lists generated?)')

    extra = list(args.flag) + ['-w']
    raw = open(args.raw, 'w') if args.raw else None
    by_shard = collections.defaultdict(lambda: {'tus': 0, 'clean': 0, 'total': 0, 'other_total': 0,
                                                'sites': collections.Counter(), 'foreign_total': 0})
    # site -> {tus, first_tu_shard set, owner}
    site_info = {}

    saved = {}
    if args.from_raw:
        cur = None
        for chunk in open(args.from_raw, errors='replace').read().split('===== ')[1:]:
            head, _, body = chunk.partition('\n')
            name, _, code = head.rpartition(' (exit ')
            saved[name] = (int(code.rstrip(')')), body)

    def one(item):
        shard, rel, e = item
        if args.from_raw:
            code, err = saved.get(rel, (0, ''))
            return shard, rel, code, err
        file, code, err = sp.run(e, extra)
        return shard, rel, code, err

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for shard, rel, code, err in pool.map(one, tus):
            r = by_shard[shard]
            r['tus'] += 1
            errors = 0
            for line in err.splitlines():
                mt = sp.ERROR_RE.match(line)
                if not mt:
                    continue
                errors += 1
                f = sp.rel(mt['file'], ROOT).replace('\\', '/')
                o = owner_of(f, owners)
                key = (f, int(mt['line']), mt['msg'])
                s = site_info.setdefault(key, {'owner': o, 'tus': 0, 'shards': set()})
                s['tus'] += 1
                s['shards'].add(shard)
                if o in ('shim', 'third_party', 'other-module', 'system'):
                    r['other_total'] += 1
                else:
                    r['total'] += 1
                    if o != shard:
                        r['foreign_total'] += 1
                r['sites'][key] += 1
            if errors == 0 and code == 0:
                r['clean'] += 1
            if raw:
                raw.write('===== %s (exit %d)\n%s\n' % (rel, code, err))
    if raw:
        raw.close()

    # per-shard unique sites
    rows = []
    for shard in SHARDS + ['unowned']:
        r = by_shard.get(shard)
        if not r:
            continue
        own = foreign = shared = other = 0
        for key in r['sites']:
            o = site_info[key]['owner']
            if o == shard:
                own += 1
            elif o == 'shared':
                shared += 1
            elif o in ('shim', 'third_party', 'other-module', 'system'):
                other += 1
            else:
                foreign += 1
        rows.append((shard, r['tus'], r['clean'], r['total'], r['other_total'], len(r['sites']) - other, own, shared, foreign,
                     other))
    tot = ['total', sum(x[1] for x in rows), sum(x[2] for x in rows), sum(x[3] for x in rows), sum(x[4] for x in rows)]
    all_sites = [k for k, v in site_info.items() if v['owner'] not in ('shim', 'third_party', 'other-module', 'system')]
    cross = [k for k in all_sites if len(site_info[k]['shards']) > 1]
    owned_sites = collections.Counter(site_info[k]['owner'] for k in all_sites)
    tot += [len(all_sites), sum(owned_sites.get(s, 0) for s in SHARDS), owned_sites.get('shared', 0), len(cross)]

    if args.markdown:
        print('| shard | TUs | clean TUs | errors | unique sites | in own files | in shared headers | in other shards\' files |')
        print('|---|---:|---:|---:|---:|---:|---:|---:|')
        for row in rows:
            shard, tus_n, clean, total, other_total, sites, own, shared, foreign, other = row
            print('| %s | %d | %d | %d | %d | %d | %d | %d |' % (shard, tus_n, clean, total, sites, own, shared, foreign))
        print('| **total** | %d | %d | %d | %d | %d | %d | %d |' % (tot[1], tot[2], tot[3], tot[5], tot[6], tot[7],
                                                                    len(cross)))
    else:
        print('%-16s %5s %6s %8s %7s | %-6s %6s %7s %9s | %s' % ('shard', 'TUs', 'clean', 'errors', 'sites', 'own', 'shared',
                                                               'foreign', 'shim/3rd', 'errors outside xrGame'))
        for shard, tus_n, clean, total, other_total, sites, own, shared, foreign, other in rows:
            print('%-16s %5d %6d %8d %7d | %-6d %6d %7d %9d | %d' % (shard, tus_n, clean, total, sites, own, shared, foreign,
                                                                  other, other_total))
        print('%-16s %5d %6d %8d %7d | %-6d %6d %7d' % ('total', tot[1], tot[2], tot[3], tot[5], tot[6], tot[7], len(cross)))
        print()
        print('sites reported by more than one shard (cross-shard): %d' % len(cross))
        print()
        print('--- most frequent module error sites [owner]')
        ranked = sorted(all_sites, key=lambda k: -site_info[k]['tus'])[:args.top]
        for k in ranked:
            v = site_info[k]
            print('  %6d TUs  %d shard(s)  [%s] %s:%d: %s' % (v['tus'], len(v['shards']), v['owner'], k[0], k[1], k[2][:110]))
        print()
        print('--- files with the most distinct error sites (module files only)')
        per_file = collections.Counter(k[0] for k in all_sites)
        file_tus = collections.Counter()
        for k in all_sites:
            file_tus[k[0]] = max(file_tus[k[0]], site_info[k]['tus'])
        for f, n in per_file.most_common(args.top):
            print('  %5d sites  max %5d TUs  [%s] %s' % (n, file_tus[f], owner_of(f, owners), f))

    if args.json:
        out = {
            'rows': [dict(zip(['shard', 'tus', 'clean', 'errors', 'other_errors', 'sites', 'own', 'shared', 'foreign', 'other'], r))
                     for r in rows],
            'sites': [{'file': k[0], 'line': k[1], 'msg': k[2], 'owner': v['owner'], 'tus': v['tus'],
                       'shards': sorted(v['shards'])} for k, v in site_info.items()],
        }
        with open(args.json, 'w') as f:
            json.dump(out, f)
    return 0


if __name__ == '__main__':
    sys.exit(main())
