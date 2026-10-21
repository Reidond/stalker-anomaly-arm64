#!/usr/bin/env python3
"""Apply clang's own fix-it hints (missing `typename`, `template`, `template<>`, `Class::` before a member-function address) and
the `this->` that clang requests for dependent-base members (it prints no fix-it for those) to xrGame sources.

Every selected translation unit is rerun with -fsyntax-only -fdiagnostics-parseable-fixits; identical hints reported by
several TUs are applied once. Only files under src/xrGame and src/xrServerEntities are touched, and only files that
--owner allows (a shard name, `shared`, or `any`). Clang suggests these three edits only where the fix is unambiguous,
but read the diff: a hint is as good as the diagnostic it belongs to.

    tools/xrgame_fixit.py build/probe --owner shared             # dry run: print what would change
    tools/xrgame_fixit.py build/probe --owner shared --write
    tools/xrgame_fixit.py build/probe --shard ui --owner ui --write    # only the TUs of one shard, only its own files
"""
import argparse
import collections
import concurrent.futures
import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.realpath(__file__))
ROOT = os.path.realpath(os.path.join(HERE, os.pardir))
spec = importlib.util.spec_from_file_location('xrgame_probe', os.path.join(HERE, 'xrgame_probe.py'))
xp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(xp)
sp = xp.sp

FIXIT = re.compile(r'^fix-it:"(?P<file>(?:[^"\\]|\\.)*)":\{(?P<l1>\d+):(?P<c1>\d+)-(?P<l2>\d+):(?P<c2>\d+)\}:"(?P<text>(?:[^"\\]|\\.)*)"$')
ALLOWED = ('typename ', 'template ', 'this->', 'template<> ')
QUALIFIER = re.compile(r'^[A-Za-z_]\w*::$')  # `&Class::member` for a member function address
EXPLICIT_QUAL = re.compile(r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): error: explicit qualification required to use member '(?P<name>[^']+)' from dependent base class")
# `xr_vector<T>::const_iterator I = ...;` without typename: clang reports "expected ';' after expression" and no fix-it,
# because it read the qualified name as an expression. Insert typename in front of such a name on the reported line.
EXPECTED_SEMI = re.compile(r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): error: expected ';' after expression")
DEPENDENT_TYPE = re.compile(
    r'(?<![\w:.>~])(?<!typename )((?:std::)?[A-Za-z_]\w*(?:::[A-Za-z_]\w*)*<[^;{}()]*?>::'
    r'(?:const_iterator|iterator|const_reverse_iterator|reverse_iterator|value_type|size_type|difference_type|const_pointer|pointer'
    r'|const_reference|reference|key_type|mapped_type|allocator_type))\b')
UNDECLARED = re.compile(r"^(?P<file>[^\s:][^:]*):(?P<line>\d+):(?P<col>\d+): error: use of undeclared identifier '(?P<name>[^']+)'")


def unescape(s):
    return re.sub(r'\\([0-7]{3}|.)', lambda m: chr(int(m.group(1), 8)) if len(m.group(1)) == 3 else {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), s)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('build_dir')
    ap.add_argument('--shard', action='append', default=[], help='only the TUs of this shard')
    ap.add_argument('--file', action='append', default=[], help='only TUs whose path contains this text')
    ap.add_argument('--owner', default='shared', help="apply to files owned by this shard, `shared` (default) or `any`")
    ap.add_argument('--write', action='store_true')
    ap.add_argument('--qualify-in', metavar='PATHS', default='',
                    help="comma separated path fragments: in these files qualify EVERY undeclared lower-case identifier with this-> "
                         "(for headers that are nothing but a class template over a dependent base)")
    ap.add_argument('--qualify', metavar='NAMES', default='',
                    help="comma separated identifiers: where the compiler reports `use of undeclared identifier 'N'`, insert `this->` "
                         "(members of a dependent base class that a template body names unqualified, e.g. object,select_state)")
    ap.add_argument('--hints', metavar='FILE', help='cache of the collected hints: reused when it exists, written otherwise '
                    '(a dry run followed by --write does not need a second pass over the TUs)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    build = os.path.realpath(args.build_dir)
    entries = json.load(open(os.path.join(build, 'compile_commands.json')))
    owners = xp.load_owners()
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
        tus.append(e)

    def one(e):
        cmd = sp.syntax_command(e, ['-w', '-fdiagnostics-parseable-fixits', '-fno-diagnostics-color'])
        import subprocess
        p = subprocess.run(cmd, cwd=e['directory'], capture_output=True, text=True, errors='replace')
        return p.stderr

    qualify = set(n for n in args.qualify.split(',') if n)
    qualify_in = [n for n in args.qualify_in.split(',') if n]
    hints = {}
    if args.hints and os.path.exists(args.hints):
        for path, line, col, text, rel in json.load(open(args.hints)):
            hints[(path, line, col, text)] = rel
        tus = []
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for err in pool.map(one, tus):
            for line in err.splitlines():
                if qualify or qualify_in:
                    u = UNDECLARED.match(line)
                    if u and (u['name'] in qualify or (qualify_in and not u['name'].isupper()
                                                       and any(x in u['file'] for x in qualify_in))):
                        path = os.path.realpath(u['file'])
                        rel = sp.rel(path, ROOT).replace('\\', '/')
                        if rel.lower().startswith(('src/xrgame/', 'src/xrserverentities/')):
                            owner = xp.owner_of(rel, owners)
                            if args.owner == 'any' or owner == args.owner:
                                hints[(path, int(u['line']), int(u['col']), 'this->')] = rel
                        continue
                q = EXPLICIT_QUAL.match(line)
                if q:
                    # clang offers no fix-it for this one; the cure is `this->` at the reported column
                    path = os.path.realpath(q['file'])
                    rel = sp.rel(path, ROOT).replace('\\', '/')
                    if rel.lower().startswith(('src/xrgame/', 'src/xrserverentities/')):
                        owner = xp.owner_of(rel, owners)
                        if args.owner == 'any' or owner == args.owner:
                            hints[(path, int(q['line']), int(q['col']), 'this->')] = rel
                    continue
                e = EXPECTED_SEMI.match(line)
                if e:
                    path = os.path.realpath(e['file'])
                    rel = sp.rel(path, ROOT).replace('\\', '/')
                    if rel.lower().startswith(('src/xrgame/', 'src/xrserverentities/')):
                        owner = xp.owner_of(rel, owners)
                        if args.owner == 'any' or owner == args.owner:
                            try:
                                src_line = open(path, 'rb').read().split(b'\n')[int(e['line']) - 1].decode('latin-1')
                            except (OSError, IndexError):
                                src_line = ''
                            for dm in DEPENDENT_TYPE.finditer(src_line):
                                if '//' not in src_line[:dm.start()]:
                                    hints[(path, int(e['line']), dm.start() + 1, 'typename ')] = rel
                    continue
                m = FIXIT.match(line)
                if not m:
                    continue
                text = unescape(m['text'])
                if text not in ALLOWED and not QUALIFIER.match(text):
                    continue
                path = os.path.realpath(unescape(m['file']))
                rel = sp.rel(path, ROOT).replace('\\', '/')
                if not rel.lower().startswith(('src/xrgame/', 'src/xrserverentities/')):
                    continue
                owner = xp.owner_of(rel, owners)
                if args.owner != 'any' and owner != args.owner:
                    continue
                if (m['l1'], m['c1']) != (m['l2'], m['c2']):
                    continue  # pure insertions only
                hints[(path, int(m['l1']), int(m['c1']), text)] = rel

    if args.hints and not os.path.exists(args.hints):
        json.dump([[p, l, c, t, r] for (p, l, c, t), r in hints.items()], open(args.hints, 'w'))
    by_file = collections.defaultdict(list)
    for (path, line, col, text), rel in hints.items():
        by_file[path].append((line, col, text))
    total = 0
    for path, items in sorted(by_file.items()):
        data = open(path, 'rb').read()
        lines = data.split(b'\n')
        for line, col, text in sorted(set(items), reverse=True):
            b = lines[line - 1]
            lines[line - 1] = b[:col - 1] + text.encode() + b[col - 1:]
            total += 1
        if args.write:
            open(path, 'wb').write(b'\n'.join(lines))
        print('%4d  %s' % (len(set(items)), sp.rel(path, ROOT)))
    print('%d hint(s) in %d file(s)%s' % (total, len(by_file), '' if args.write else ' (dry run, pass --write)'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
