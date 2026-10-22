#!/usr/bin/env python3
"""Finds calls where one argument modifies something another argument of the same call reads.

The order function arguments are evaluated in is unspecified. MSVC (x64) evaluated them right to left in practice, clang on arm64
goes left to right, so `f(height, RayPick(..., height))` (RayPick takes `height` by reference) behaved on the PC and not here.

  tools/sweep/argorder.py -p build/macos-arm64-asan [-j 8] [--only src/xrGame] [files...]  > hits.txt

Needs `pip install libclang` (a venv is fine) and reads <build>/compile_commands.json. An argument "writes" X when it contains
X++ / ++X / X = / X op= , &X, a call that takes X by non-const reference or as a non-const pointer from &X, or a non-const member
function called on X. Another argument "reads" X when it mentions X outside an address-of or sizeof. Output: file:line: call, argument
indexes, variable. Only locations under the --only prefixes (default: the engine modules) are reported, once each.
"""
import argparse
import json
import multiprocessing
import os
import sys

import clang.cindex as ci

CK = ci.CursorKind

ASSIGN_OPS = {"=", "+=", "-=", "*=", "/=", "%=", "<<=", ">>=", "&=", "|=", "^="}


def lib_setup(lib):
    if lib:
        ci.Config.set_library_file(lib)


def tokens_of(cursor):
    try:
        return [t.spelling for t in cursor.get_tokens()]
    except Exception:
        return []


def decl_key(cursor):
    ref = cursor.referenced
    if ref is None:
        return None
    key = ref.get_usr() or "%s:%d:%d" % (ref.location.file, ref.location.line, ref.location.column)
    if cursor.kind == CK.MEMBER_REF_EXPR:
        # a member of which object: `a.x` and `b.x` are different things
        kids = list(cursor.get_children())
        if kids:
            key += "@" + "".join(tokens_of(kids[0]))
        else:
            key += "@this"
    return key


def base_of(cursor):
    """The variable or member an lvalue expression names (through [], ., ->, parens, casts), as a key."""
    c = cursor
    for _ in range(20):
        if c is None:
            return None
        if c.kind in (CK.DECL_REF_EXPR,):
            ref = c.referenced
            if ref is not None and ref.kind in (CK.VAR_DECL, CK.PARM_DECL, CK.FIELD_DECL):
                return decl_key(c)
            return None
        if c.kind == CK.MEMBER_REF_EXPR:
            # a.b writes "b" of whatever a is: key on the member (coarse, but a.b and c.b rarely meet in one call)
            return decl_key(c)
        if c.kind in (CK.UNEXPOSED_EXPR, CK.PAREN_EXPR, CK.ARRAY_SUBSCRIPT_EXPR, CK.CSTYLE_CAST_EXPR):
            kids = list(c.get_children())
            c = kids[0] if kids else None
            continue
        return None
    return None


def writes_in(cursor, out):
    """Collect keys of variables the expression may modify."""
    k = cursor.kind
    if k == CK.UNARY_OPERATOR:
        toks = tokens_of(cursor)
        kids = list(cursor.get_children())
        if toks and kids:
            op_first = toks[0]
            op_last = toks[-1]
            if op_first in ("++", "--", "&") or op_last in ("++", "--"):
                key = base_of(kids[0])
                if key:
                    out.add(key)
    elif k in (CK.BINARY_OPERATOR, CK.COMPOUND_ASSIGNMENT_OPERATOR):
        kids = list(cursor.get_children())
        if len(kids) == 2:
            # find the operator token between the two operands
            lhs_end = kids[0].extent.end.offset
            for t in cursor.get_tokens():
                if t.extent.start.offset >= lhs_end and t.spelling in ASSIGN_OPS:
                    key = base_of(kids[0])
                    if key:
                        out.add(key)
                    break
    elif k == CK.CALL_EXPR:
        callee = cursor.referenced
        args = list(cursor.get_arguments())
        if callee is not None and callee.kind in (CK.FUNCTION_DECL, CK.CXX_METHOD, CK.CONSTRUCTOR, CK.FUNCTION_TEMPLATE):
            try:
                params = list(callee.get_arguments())
            except Exception:
                params = []
            for i, a in enumerate(args):
                if i >= len(params):
                    break
                pt = params[i].type
                if pt.kind == ci.TypeKind.LVALUEREFERENCE and not pt.get_pointee().is_const_qualified():
                    key = base_of(a)
                    if key:
                        out.add(key)
            # a non-const member function modifies its object, if its body (when the definition is in view) writes a member
            if callee.kind == CK.CXX_METHOD and not callee.is_const_method() and not callee.is_static_method() and mutates(callee):
                kids = list(cursor.get_children())
                if kids and kids[0].kind == CK.MEMBER_REF_EXPR:
                    obj = list(kids[0].get_children())
                    if obj:
                        key = base_of(obj[0])
                        if key:
                            out.add(key)
    for c in cursor.get_children():
        writes_in(c, out)


_mutates_cache = {}


def mutates(method):
    """True when the member function's definition writes to something (a member, a global, a by-reference argument)."""
    usr = method.get_usr()
    if usr in _mutates_cache:
        return _mutates_cache[usr]
    _mutates_cache[usr] = False  # recursion guard
    d = method.get_definition()
    result = False
    if d is not None:
        w = set()
        try:
            writes_in(d, w)
        except RecursionError:
            w = {"?"}
        # writes to locals do not count, writes to members and globals do (usr of a local contains @F@ before its @V@)
        result = any(("@FI@" in k) or ("@V@" in k and "@F@" not in k.split("@V@")[0]) for k in w)
    _mutates_cache[usr] = result
    return result


def reads_in(cursor, out, skip_addr=True):
    k = cursor.kind
    if k == CK.UNARY_OPERATOR and skip_addr:
        toks = tokens_of(cursor)
        if toks and toks[0] == "&":
            return
    if k == CK.CXX_UNARY_EXPR:  # sizeof / alignof
        return
    if k in (CK.DECL_REF_EXPR, CK.MEMBER_REF_EXPR):
        ref = cursor.referenced
        if ref is not None and ref.kind in (CK.VAR_DECL, CK.PARM_DECL, CK.FIELD_DECL):
            key = decl_key(cursor)
            if key:
                out.add(key)
    for c in cursor.get_children():
        reads_in(c, out, skip_addr)


def analyse_call(call, hits):
    args = list(call.get_arguments())
    if len(args) < 2:
        return
    written = []
    read = []
    for a in args:
        w = set()
        writes_in(a, w)
        r = set()
        reads_in(a, r)
        written.append(w)
        read.append(r)
    for j, w in enumerate(written):
        if not w:
            continue
        for i, r in enumerate(read):
            if i == j:
                continue
            common = w & r
            for key in common:
                hits.append((call.location.file.name if call.location.file else "?", call.location.line, i, j, key))


def worker(job):
    path, argv, only, lib = job
    lib_setup(lib)
    idx = ci.Index.create()
    try:
        tu = idx.parse(path, args=argv, options=ci.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD & 0 | 0)
    except ci.TranslationUnitLoadError:
        return []
    hits = []
    seen = set()

    def visit(c):
        loc = c.location
        f = loc.file.name if loc.file else None
        if f is None:
            return
        if not any(f.startswith(o) for o in only):
            if c.kind in (CK.TRANSLATION_UNIT,):
                pass
            else:
                return
        if c.kind == CK.CALL_EXPR:
            key = (f, loc.line, loc.column)
            if key not in seen:
                seen.add(key)
                analyse_call(c, hits)
        for ch in c.get_children():
            visit(ch)

    for ch in tu.cursor.get_children():
        visit(ch)
    return hits


def clean_args(command, directory):
    import shlex

    parts = shlex.split(command)
    out = []
    skip = 0
    for i, p in enumerate(parts[1:], 1):
        if skip:
            skip -= 1
            continue
        if p in ("-o", "-MF", "-MT"):
            skip = 1
            continue
        if p in ("-c", "-MD", "-MMD") or p.startswith("-fsanitize") or p.startswith("-fno-sanitize"):
            continue
        if p.endswith((".cpp", ".c", ".mm", ".m")) and not p.startswith("-"):
            continue
        out.append(p)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", required=True, help="build directory with compile_commands.json")
    ap.add_argument("-j", type=int, default=8)
    ap.add_argument("--only", action="append", help="report only locations under this path prefix (repeatable)")
    ap.add_argument("--lib", help="libclang.dylib to use")
    ap.add_argument("files", nargs="*", help="source files (default: every .cpp of the --only prefixes)")
    a = ap.parse_args()
    root = os.path.abspath(os.path.join(a.p, "..", ".."))
    only = [os.path.join(root, o) for o in (a.only or ["src/xrGame", "src/xrEngine", "src/xrCore", "src/xrServerEntities", "src/xrPhysics",
                                                        "src/xrSound", "src/xrCDB", "src/xrParticles", "src/xrNetServer", "src/Layers"])]
    with open(os.path.join(a.p, "compile_commands.json")) as f:
        db = json.load(f)
    jobs = []
    wanted = set(os.path.abspath(x) for x in a.files)
    for e in db:
        path = e["file"]
        if not path.endswith((".cpp", ".cc")):
            continue
        if wanted:
            if os.path.abspath(path) not in wanted:
                continue
        elif not any(path.startswith(o) for o in only):
            continue
        jobs.append((path, clean_args(e["command"], e["directory"]), only, a.lib))
    print("%d translation units" % len(jobs), file=sys.stderr)
    all_hits = set()
    with multiprocessing.Pool(a.j) as pool:
        for n, hits in enumerate(pool.imap_unordered(worker, jobs), 1):
            for h in hits:
                all_hits.add(h)
            if n % 50 == 0:
                print("%d/%d, %d hits" % (n, len(jobs), len(all_hits)), file=sys.stderr)
    for f, line, i, j, key in sorted(all_hits):
        print("%s:%d: argument %d reads what argument %d modifies (%s)" % (f.replace(root + "/", ""), line, i, j, key))


if __name__ == "__main__":
    main()
