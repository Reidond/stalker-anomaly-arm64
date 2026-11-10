#!/usr/bin/env python3
"""Cuts game files into content-addressed chunks, builds index.json and uploads both to R2.

    publish.py publish --source GAME_DIR --path db/configs --bucket NAME [--local] [--dry-run]
    publish.py verify  --bucket NAME [--deep]

R2 layout (the Worker serves /data/* and /bin/* from the key with the same path):

    data/chunks/<sha256 hex>    immutable, one per distinct chunk
    data/index.json             written last, so a reader never sees an index whose chunks are missing

Uploads go through the Cloudflare `cf` CLI (`cf r2 objects put`), which uses the CLI's own login; the
script never handles credentials. Python 3.9 or newer, standard library only. See docs/web-port/HOSTING.md.
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf_futures
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xray_archive  # noqa: E402

INDEX_FORMAT = "anomaly-web-index/1"
DEFAULT_CHUNK_SIZE = 4 * 1024 * 1024
MAX_JOBS = 8
CACHE_VERSION = 2
HEX64 = re.compile(r"^[0-9a-f]{64}$")


def repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def parse_size(text: str) -> int:
    m = re.fullmatch(r"\s*(\d+)\s*(b|k|kb|kib|m|mb|mib|g|gb|gib)?\s*", text.lower())
    if not m:
        raise argparse.ArgumentTypeError(f"not a size: {text!r}")
    mult = {None: 1, "b": 1, "k": 1 << 10, "kb": 1 << 10, "kib": 1 << 10, "m": 1 << 20, "mb": 1 << 20,
            "mib": 1 << 20, "g": 1 << 30, "gb": 1 << 30, "gib": 1 << 30}[m.group(2)]
    return int(m.group(1)) * mult


def human(n: float) -> str:
    for unit in ("B", "KiB", "MiB", "GiB", "TiB"):
        if abs(n) < 1024 or unit == "TiB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return f"{n} B"


# ---- planning: files -> chunks ------------------------------------------------------------------


@dataclass
class FilePlan:
    path: str  # relative to the source root, forward slashes
    abs_path: Path
    size: int
    mtime_ns: int
    sha256: str
    chunks: list[str]  # hashes, in order
    archive: dict | None = None


@dataclass
class Plan:
    chunk_size: int
    files: list[FilePlan] = field(default_factory=list)

    def chunk_table(self) -> dict[str, tuple[FilePlan, int, int]]:
        """hash -> (a file holding it, offset, length), first occurrence wins."""
        table: dict[str, tuple[FilePlan, int, int]] = {}
        for fp in self.files:
            for i, h in enumerate(fp.chunks):
                if h not in table:
                    off = i * self.chunk_size
                    table[h] = (fp, off, min(self.chunk_size, fp.size - off))
        return table


def scan_files(source: Path, includes: list[str]) -> list[tuple[str, Path]]:
    """Regular files below each include path, sorted; dot files and symlinks are skipped."""
    found: dict[str, Path] = {}
    root = source.resolve()
    for inc in includes:
        target = (source / inc)
        if target.is_symlink():
            raise SystemExit(f"refusing symlink: {inc}")
        target = target.resolve()
        try:
            target.relative_to(root)
        except ValueError:
            raise SystemExit(f"--path {inc!r} leaves the source directory")
        if target.is_file():
            candidates = [target]
        elif target.is_dir():
            candidates = []
            for dirpath, dirnames, filenames in os.walk(target, followlinks=False):
                dirnames[:] = sorted(d for d in dirnames if not d.startswith(".") and not os.path.islink(os.path.join(dirpath, d)))
                for name in sorted(filenames):
                    p = Path(dirpath) / name
                    if name.startswith(".") or p.is_symlink():
                        continue
                    candidates.append(p)
        else:
            raise SystemExit(f"--path {inc!r} not found below {source}")
        for p in candidates:
            found[p.relative_to(root).as_posix()] = p
    return sorted(found.items())


def hash_file(path: Path, chunk_size: int) -> tuple[str, list[str], int]:
    whole = hashlib.sha256()
    chunks: list[str] = []
    total = 0
    with open(path, "rb") as f:
        while True:
            block = f.read(chunk_size)
            if not block:
                break
            total += len(block)
            whole.update(block)
            chunks.append(hashlib.sha256(block).hexdigest())
    return whole.hexdigest(), chunks, total


class PlanCache:
    """Remembers hashes and TOCs per (path, size, mtime, chunk size), so a rerun after an interruption
    does not read 16 GB again. --no-cache ignores it."""

    def __init__(self, path: Path | None):
        self.path = path
        self.data: dict[str, dict] = {}
        self.lock = threading.Lock()
        if path and path.is_file():
            try:
                loaded = json.loads(path.read_text())
                if loaded.get("version") == CACHE_VERSION:
                    self.data = loaded.get("files", {})
            except (OSError, ValueError):
                self.data = {}

    @staticmethod
    def key(rel: str, size: int, mtime_ns: int, chunk_size: int) -> str:
        return f"{rel}|{size}|{mtime_ns}|{chunk_size}"

    def get(self, rel: str, size: int, mtime_ns: int, chunk_size: int) -> dict | None:
        return self.data.get(self.key(rel, size, mtime_ns, chunk_size))

    def put(self, rel: str, size: int, mtime_ns: int, chunk_size: int, value: dict) -> None:
        with self.lock:
            self.data[self.key(rel, size, mtime_ns, chunk_size)] = value

    def save(self) -> None:
        if not self.path:
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.path.with_suffix(".tmp")
        tmp.write_text(json.dumps({"version": CACHE_VERSION, "files": self.data}))
        os.replace(tmp, self.path)


def archive_summary(abs_path: Path) -> dict:
    a = xray_archive.read_archive(str(abs_path))
    return {
        "nameEncoding": "latin-1",  # one character per byte of the name as stored in the archive
        "entryPoint": a.entry_point,
        "autoLoad": a.auto_load,
        "header": a.header_ini,
        "chunks": [[c.chunk_id, int(c.compressed), c.offset, c.size] for c in a.chunks],
        # name, absolute offset in the archive, real size, stored size, crc as in CLocatorAPI::Register
        "entries": [e.as_row() for e in a.entries],
    }


def build_plan(source: Path, includes: list[str], chunk_size: int, jobs: int, cache: PlanCache, log) -> Plan:
    files = scan_files(source, includes)
    if not files:
        raise SystemExit("no files selected")
    plan = Plan(chunk_size=chunk_size)

    def one(item: tuple[str, Path]) -> FilePlan:
        rel, p = item
        st = p.stat()
        cached = cache.get(rel, st.st_size, st.st_mtime_ns, chunk_size)
        if cached is None:
            sha, chunks, total = hash_file(p, chunk_size)
            if total != st.st_size:
                raise SystemExit(f"{rel} changed while it was read")
            cached = {"sha256": sha, "chunks": chunks}
            from_cache = False
        else:
            from_cache = True
        if "archive" not in cached and xray_archive.is_archive_name(p.name):
            try:
                cached["archive"] = archive_summary(p)
            except xray_archive.ArchiveError as exc:
                raise SystemExit(f"{rel}: not a readable X-Ray archive: {exc}")
        cache.put(rel, st.st_size, st.st_mtime_ns, chunk_size, cached)
        log(f"  {rel}: {human(st.st_size)}, {len(cached['chunks'])} chunks"
            f"{', ' + str(len(cached['archive']['entries'])) + ' archive entries' if 'archive' in cached else ''}"
            f"{' (cached)' if from_cache else ''}")
        return FilePlan(rel, p, st.st_size, st.st_mtime_ns, cached["sha256"], cached["chunks"], cached.get("archive"))

    with cf_futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        plan.files = list(pool.map(one, files))
    cache.save()
    return plan


def build_index(plan: Plan) -> bytes:
    files = []
    for fp in plan.files:
        entry: dict = {"path": fp.path, "size": fp.size, "sha256": fp.sha256, "chunks": fp.chunks}
        if fp.archive is not None:
            entry["archive"] = fp.archive
        files.append(entry)
    index = {
        "format": INDEX_FORMAT,
        "chunkSize": plan.chunk_size,
        "chunkPath": "chunks/",  # relative to the index URL
        "totalSize": sum(f.size for f in plan.files),
        "files": files,
    }
    return json.dumps(index, separators=(",", ":"), sort_keys=False).encode("utf-8")


# ---- R2 access through the cf CLI ---------------------------------------------------------------


class CfError(Exception):
    pass


class CfBucket:
    """`cf r2 objects list/put/get` against one bucket, real or the local simulation."""

    def __init__(self, bucket: str, cf_bin: str, local: bool, persist_to: str | None):
        self.bucket = bucket
        self.cf_bin = cf_bin
        self.base_flags = ["--bucket-name", bucket]
        if local:
            self.base_flags.append("--local")
            if persist_to:
                self.base_flags += ["--persist-to", persist_to]

    def _run(self, args: list[str], timeout: int = 600, binary: bool = False) -> subprocess.CompletedProcess:
        return subprocess.run([self.cf_bin, *args], capture_output=True, text=not binary, timeout=timeout,
                              stdin=subprocess.DEVNULL)

    def list(self, prefix: str) -> dict[str, dict]:
        """key -> object metadata for every key below prefix, paged with --start-after."""
        out: dict[str, dict] = {}
        after: str | None = None
        while True:
            args = ["r2", "objects", "list", *self.base_flags, "--prefix", prefix, "--per-page", "1000"]
            if after:
                args += ["--start-after", after]
            res = self._run(args)
            if res.returncode != 0:
                raise CfError(f"cf r2 objects list failed: {res.stderr.strip() or res.stdout.strip()}")
            try:
                page = json.loads(res.stdout)
            except ValueError as exc:
                raise CfError(f"cf r2 objects list printed something else than JSON: {res.stdout[:200]!r}") from exc
            if not page:
                return out
            for obj in page:
                out[obj["key"]] = obj
            after = page[-1]["key"]
            if len(page) < 1000:
                return out

    def put(self, key: str, file: Path, content_type: str) -> None:
        args = ["r2", "objects", "put", key, *self.base_flags, "--file", str(file), "--content-type", content_type]
        res = self._run(args)
        if res.returncode != 0:
            raise CfError(f"put {key}: {res.stderr.strip() or res.stdout.strip()}")

    def get(self, key: str, dest: Path) -> None:
        """cf prints the object body on stdout."""
        res = self._run(["r2", "objects", "get", key, *self.base_flags], binary=True)
        if res.returncode != 0:
            raise CfError(f"get {key}: {res.stderr.decode(errors='replace').strip()}")
        dest.write_bytes(res.stdout)


# ---- upload -------------------------------------------------------------------------------------


def read_chunk(fp: FilePlan, offset: int, length: int) -> bytes:
    with open(fp.abs_path, "rb") as f:
        f.seek(offset)
        data = f.read(length)
    if len(data) != length:
        raise CfError(f"{fp.path}: short read at {offset}")
    return data


def upload_chunks(bucket: CfBucket, key_prefix: str, missing: list[str], table, jobs: int, tmp_dir: Path,
                  retries: int, log) -> tuple[int, int, list[str]]:
    """Uploads the missing chunks with up to `jobs` cf processes. Returns (done, bytes, failed hashes)."""
    done = 0
    sent = 0
    failed: list[str] = []
    lock = threading.Lock()
    started = time.monotonic()

    def one(h: str) -> int:
        fp, off, length = table[h]
        data = read_chunk(fp, off, length)
        if hashlib.sha256(data).hexdigest() != h:
            raise CfError(f"{fp.path} changed since it was hashed (chunk at {off})")
        tmp = tmp_dir / f"{h}.part"
        tmp.write_bytes(data)
        try:
            last: Exception | None = None
            for attempt in range(retries + 1):
                try:
                    bucket.put(f"{key_prefix}chunks/{h}", tmp, "application/octet-stream")
                    return length
                except (CfError, subprocess.TimeoutExpired) as exc:
                    last = exc
                    time.sleep(min(2 ** attempt, 10))
            raise CfError(str(last))
        finally:
            try:
                tmp.unlink()
            except FileNotFoundError:
                pass

    with cf_futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(one, h): h for h in missing}
        for fut in cf_futures.as_completed(futures):
            h = futures[fut]
            try:
                n = fut.result()
            except Exception as exc:  # noqa: BLE001 - reported below, upload continues
                with lock:
                    failed.append(h)
                log(f"  FAILED {h[:12]}: {exc}")
                continue
            with lock:
                done += 1
                sent += n
                if done % 25 == 0 or done == len(missing):
                    rate = sent / max(time.monotonic() - started, 1e-6)
                    log(f"  uploaded {done}/{len(missing)} chunks, {human(sent)} ({human(rate)}/s)")
    return done, sent, failed


# ---- commands -----------------------------------------------------------------------------------


def make_bucket(args) -> CfBucket:
    return CfBucket(args.bucket, args.cf, args.local, args.persist_to)


def prepare_work_dir(args) -> Path:
    work = Path(args.work_dir) if args.work_dir else repo_root() / "build" / "web-publish"
    work.mkdir(parents=True, exist_ok=True)
    return work


def cmd_publish(args) -> int:
    log = lambda msg: print(msg, flush=True)  # noqa: E731
    jobs = max(1, min(args.jobs, MAX_JOBS))
    source = Path(args.source).resolve()
    if not source.is_dir():
        raise SystemExit(f"--source {source} is not a directory")
    includes = args.path or ["."]
    work = prepare_work_dir(args)
    cache = PlanCache(None if args.no_cache else work / "plan-cache.json")
    key_prefix = args.prefix.strip("/") + "/" if args.prefix.strip("/") else ""

    log(f"planning {len(includes)} path(s) under {source}, chunk size {human(args.chunk_size)}")
    plan = build_plan(source, includes, args.chunk_size, jobs, cache, log)
    index_bytes = build_index(plan)
    index_md5 = hashlib.md5(index_bytes).hexdigest()
    (work / "index.json").write_bytes(index_bytes)

    table = plan.chunk_table()
    total_refs = sum(len(f.chunks) for f in plan.files)
    distinct_bytes = sum(length for _, _, length in table.values())
    log(f"{len(plan.files)} files, {human(sum(f.size for f in plan.files))}; {total_refs} chunk references, "
        f"{len(table)} distinct chunks ({human(distinct_bytes)}); index.json {human(len(index_bytes))} "
        f"(a copy is at {work / 'index.json'})")

    bucket = make_bucket(args)
    log(f"listing {args.bucket}/{key_prefix}chunks/ ({'local simulation' if args.local else 'remote'})")
    remote = bucket.list(f"{key_prefix}chunks/")
    missing = []
    for h, (_, _, length) in table.items():
        obj = remote.get(f"{key_prefix}chunks/{h}")
        if obj is None or obj.get("size") != length:
            missing.append(h)
    missing_bytes = sum(table[h][2] for h in missing)
    log(f"{len(table) - len(missing)} chunks already stored, {len(missing)} to upload ({human(missing_bytes)})")

    index_key = f"{key_prefix}index.json"
    remote_index = bucket.list(index_key).get(index_key)
    index_same = remote_index is not None and remote_index.get("etag") == index_md5 and remote_index.get("size") == len(index_bytes)
    log(f"index.json: {'unchanged, skipped' if index_same else 'to upload'}")

    if args.dry_run:
        log("dry run: nothing uploaded")
        return 0

    sent_chunks = 0
    sent_bytes = 0
    if missing:
        tmp_dir = Path(tempfile.mkdtemp(prefix="publish-", dir=work))
        (tmp_dir / ".publish-tmp").write_text("created by publish.py\n")
        try:
            sent_chunks, sent_bytes, failed = upload_chunks(bucket, key_prefix, missing, table, jobs, tmp_dir,
                                                            args.retries, log)
        finally:
            # Only files this run made: the marker, leftover .part files, then the directory itself.
            if (tmp_dir / ".publish-tmp").is_file():
                for leftover in tmp_dir.iterdir():
                    if leftover.is_file() and (leftover.suffix == ".part" or leftover.name == ".publish-tmp"):
                        leftover.unlink()
                try:
                    tmp_dir.rmdir()
                except OSError:
                    pass
        if failed:
            log(f"{len(failed)} chunks failed; index.json NOT uploaded. Run the same command again to resume.")
            return 1

    if not index_same:
        bucket.put(index_key, work / "index.json", "application/json")
        log("uploaded index.json")
    log(f"done: {sent_chunks} chunks ({human(sent_bytes)}) and {0 if index_same else 1} index uploaded, "
        f"{len(table) - len(missing)} chunks skipped")
    return 0


def cmd_verify(args) -> int:
    """Checks that every chunk the stored index lists exists with the right size; --deep downloads
    each one and compares its SHA-256."""
    log = lambda msg: print(msg, flush=True)  # noqa: E731
    work = prepare_work_dir(args)
    key_prefix = args.prefix.strip("/") + "/" if args.prefix.strip("/") else ""
    bucket = make_bucket(args)
    index_key = f"{key_prefix}index.json"
    tmp_dir = Path(tempfile.mkdtemp(prefix="verify-", dir=work))
    (tmp_dir / ".publish-tmp").write_text("created by publish.py\n")
    problems = 0
    try:
        idx_path = tmp_dir / "index.json"
        bucket.get(index_key, idx_path)
        index = json.loads(idx_path.read_text())
        if index.get("format") != INDEX_FORMAT:
            raise SystemExit(f"unexpected index format {index.get('format')!r}")
        chunk_size = index["chunkSize"]
        want: dict[str, int] = {}
        for f in index["files"]:
            for i, h in enumerate(f["chunks"]):
                want.setdefault(h, min(chunk_size, f["size"] - i * chunk_size))
        remote = bucket.list(f"{key_prefix}chunks/")
        for h, size in want.items():
            obj = remote.get(f"{key_prefix}chunks/{h}")
            if obj is None:
                log(f"missing {h}")
                problems += 1
            elif obj.get("size") != size:
                log(f"wrong size {h}: {obj.get('size')} != {size}")
                problems += 1
        log(f"{len(want)} chunks listed by the index, {problems} missing or wrong size")
        if args.deep and not problems:
            def check(h: str) -> str | None:
                dest = tmp_dir / f"{h}.part"
                try:
                    bucket.get(f"{key_prefix}chunks/{h}", dest)
                    got = hashlib.sha256(dest.read_bytes()).hexdigest()
                    return None if got == h else f"hash mismatch for {h}: got {got}"
                except CfError as exc:
                    return str(exc)
                finally:
                    try:
                        dest.unlink()
                    except FileNotFoundError:
                        pass
            with cf_futures.ThreadPoolExecutor(max_workers=max(1, min(args.jobs, MAX_JOBS))) as pool:
                for err in pool.map(check, want):
                    if err:
                        log(err)
                        problems += 1
            log(f"deep check: {len(want)} chunks downloaded and hashed, {problems} problems")
    finally:
        for leftover in tmp_dir.iterdir():
            if leftover.is_file() and (leftover.suffix in (".part", ".json") or leftover.name == ".publish-tmp"):
                leftover.unlink()
        try:
            tmp_dir.rmdir()
        except OSError:
            pass
    return 1 if problems else 0


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--bucket", default=os.environ.get("ANOMALY_WEB_BUCKET"),
                        help="R2 bucket name (default: $ANOMALY_WEB_BUCKET)")
    common.add_argument("--prefix", default="data", help="key prefix for index and chunks (default: data)")
    common.add_argument("--cf", default=os.environ.get("CF_BIN", "cf"),
                        help="the cf executable (default: $CF_BIN or cf on PATH)")
    common.add_argument("--local", action="store_true", help="use cf's local simulation instead of the real bucket")
    common.add_argument("--persist-to", help="state directory of the local simulation (with --local)")
    common.add_argument("--jobs", type=int, default=MAX_JOBS, help=f"parallel cf processes, at most {MAX_JOBS}")
    common.add_argument("--work-dir", help="scratch directory (default: <repo>/build/web-publish)")
    sub = p.add_subparsers(dest="command", required=True)

    pub = sub.add_parser("publish", parents=[common], help="chunk, upload missing chunks, then upload index.json")
    pub.add_argument("--source", required=True, help="game directory (the one holding db/, gamedata/, ...)")
    pub.add_argument("--path", action="append", help="file or directory below --source (repeatable; default: all)")
    pub.add_argument("--chunk-size", type=parse_size, default=DEFAULT_CHUNK_SIZE, help="default: 4MiB")
    pub.add_argument("--retries", type=int, default=3, help="extra attempts per chunk")
    pub.add_argument("--no-cache", action="store_true", help="re-hash everything instead of using the plan cache")
    pub.add_argument("--dry-run", action="store_true", help="plan and compare with the bucket, upload nothing")
    pub.set_defaults(func=cmd_publish)

    ver = sub.add_parser("verify", parents=[common], help="check the stored index against the stored chunks")
    ver.add_argument("--deep", action="store_true", help="download every chunk and compare its SHA-256")
    ver.set_defaults(func=cmd_verify)

    args = p.parse_args(argv)
    if not args.bucket:
        p.error("--bucket (or $ANOMALY_WEB_BUCKET) is required")
    if args.command == "publish" and args.chunk_size < 64 * 1024:
        p.error("--chunk-size must be at least 64KiB")
    if args.command == "publish" and args.chunk_size > 300_000_000:
        p.error("--chunk-size must stay below the 300 MB limit of cf r2 objects put")
    if shutil.which(args.cf) is None and not os.path.isfile(args.cf):
        p.error(f"cf executable not found: {args.cf}")
    try:
        return args.func(args)
    except CfError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
