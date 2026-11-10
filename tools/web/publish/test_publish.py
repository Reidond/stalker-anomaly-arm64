"""Tests for the publisher. Run: python3 -I -m unittest discover -s tools/web/publish -p 'test_*.py'

Standard library only. The R2 side is a fake `cf` executable that keeps objects in a directory.
"""

from __future__ import annotations

import contextlib
import hashlib
import io
import json
import os
import random
import stat
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import publish  # noqa: E402
import xray_archive as xa  # noqa: E402

# ---- an independent LZHUF encoder, a port of Encode() in src/xrCore/lzhuf.cpp -------------------


class LzEncoder:
    N, F, THRESHOLD = 4096, 60, 2

    def __init__(self):
        self.n_char = 256 - self.THRESHOLD + self.F
        self.t = self.n_char * 2 - 1
        self.r = self.t - 1
        t = self.t
        self.freq = [0] * (t + 1)
        self.son = [0] * t
        self.prnt = [0] * (t + self.n_char + 1)
        for i in range(self.n_char):
            self.freq[i] = 1
            self.son[i] = i + t
            self.prnt[i + t] = i
        i, j = 0, self.n_char
        while j <= self.r:
            self.freq[j] = self.freq[i] + self.freq[i + 1]
            self.son[j] = i
            self.prnt[i] = self.prnt[i + 1] = j
            i += 2
            j += 1
        self.freq[t] = 0xFFFF
        self.prnt[self.r] = 0
        self.out = bytearray()
        self.putbuf = 0
        self.putlen = 0

    def put_code(self, length, code):
        self.putbuf |= code >> self.putlen
        self.putlen += length
        if self.putlen >= 8:
            self.out.append((self.putbuf >> 8) & 0xFF)
            self.putlen -= 8
            if self.putlen >= 8:
                self.out.append(self.putbuf & 0xFF)
                self.putlen -= 8
                self.putbuf = (code << (length - self.putlen)) & 0xFFFF
            else:
                self.putbuf = (self.putbuf << 8) & 0xFFFF

    def reconst(self):
        freq, son, prnt, t, n_char = self.freq, self.son, self.prnt, self.t, self.n_char
        j = 0
        for i in range(t):
            if son[i] >= t:
                freq[j] = (freq[i] + 1) // 2
                son[j] = son[i]
                j += 1
        i = 0
        for j in range(n_char, t):
            f = freq[j] = freq[i] + freq[i + 1]
            k = j - 1
            while f < freq[k]:
                k -= 1
            k += 1
            n = j - k
            freq[k + 1:k + 1 + n] = freq[k:k + n]
            freq[k] = f
            son[k + 1:k + 1 + n] = son[k:k + n]
            son[k] = i
            i += 2
        for i in range(t):
            k = son[i]
            if k >= t:
                prnt[k] = i
            else:
                prnt[k] = prnt[k + 1] = i

    def update(self, c):
        freq, son, prnt, t = self.freq, self.son, self.prnt, self.t
        if freq[self.r] == 0x4000:
            self.reconst()
        c = prnt[c + t]
        while True:
            freq[c] += 1
            k = freq[c]
            l = c + 1
            if k > freq[l]:
                l += 1
                while k > freq[l]:
                    l += 1
                l -= 1
                freq[c], freq[l] = freq[l], k
                i = son[c]
                prnt[i] = l
                if i < t:
                    prnt[i + 1] = l
                j = son[l]
                son[l] = i
                prnt[j] = c
                if j < t:
                    prnt[j + 1] = c
                son[c] = j
                c = l
            c = prnt[c]
            if c == 0:
                break

    def encode_char(self, c):
        i = j = 0
        k = self.prnt[c + self.t]
        while True:
            i >>= 1
            if k & 1:
                i += 0x8000
            j += 1
            k = self.prnt[k]
            if k == self.r:
                break
        self.put_code(j, i)
        self.update(c)

    def encode_position(self, pos):
        i = pos >> 6
        self.put_code(xa._P_LEN[i], xa._P_CODE[i] << 8)
        self.put_code(6, (pos & 0x3F) << 10)

    def encode(self, data: bytes) -> bytes:
        self.out += struct.pack("<I", len(data))
        pos = 0
        while pos < len(data):
            best_len, best_dist = 0, 0
            for dist in range(1, min(pos, self.N - self.F) + 1):
                n = 0
                while n < self.F and pos + n < len(data) and data[pos + n - dist] == data[pos + n]:
                    n += 1
                if n > best_len:
                    best_len, best_dist = n, dist
            if best_len > self.THRESHOLD:
                self.encode_char(255 - self.THRESHOLD + best_len)
                self.encode_position(best_dist - 1)
                pos += best_len
            else:
                self.encode_char(data[pos])
                pos += 1
        if self.putlen:
            self.out.append((self.putbuf >> 8) & 0xFF)
        return bytes(self.out)


def lz_encode(data: bytes) -> bytes:
    return LzEncoder().encode(data)


# ---- synthetic archives -------------------------------------------------------------------------


def make_archive(files: dict[str, bytes], header: str | None, compress_toc: bool = True,
                 bad_offset: bool = False) -> bytes:
    """Layout as xrCompress writes it: header chunk (666), data chunk (0), table of contents (1)."""
    out = bytearray()
    if header is not None:
        raw = header.encode()
        out += struct.pack("<II", xa.CFS_HEADER_CHUNK_ID, len(raw)) + raw
    data = bytearray()
    base = len(out) + 8
    records = bytearray()
    for name, content in files.items():
        offset = base + len(data) + (10**9 if bad_offset else 0)
        data += content
        name_b = name.encode("latin-1")
        records += struct.pack("<H", 16 + len(name_b)) + struct.pack("<III", len(content), len(content), 0xC0FFEE)
        records += name_b + struct.pack("<I", offset)
    out += struct.pack("<II", 0, len(data)) + data
    if compress_toc:
        packed = lz_encode(bytes(records))
        out += struct.pack("<II", 1 | xa.CFS_COMPRESS_MARK, len(packed)) + packed
    else:
        out += struct.pack("<II", 1, len(records)) + records
    return bytes(out)


HEADER = '[header]\nauto_load = true\nentry_point = $fs_root$\\gamedata\\\n'


class LzTests(unittest.TestCase):
    def roundtrip(self, data: bytes):
        self.assertEqual(xa.lz_decompress(lz_encode(data)), data)

    def test_empty_and_tiny(self):
        self.assertEqual(xa.lz_decompress(struct.pack("<I", 0)), b"")
        self.roundtrip(b"a")
        self.roundtrip(b"abcabcabcabcabc")

    def test_text_with_matches(self):
        self.roundtrip((b"[section]\nkey = value\n" * 300) + b"tail")

    def test_incompressible_runs_the_tree_rebuild(self):
        rng = random.Random(7)
        # more than 16384 literal symbols forces reconst()
        self.roundtrip(bytes(rng.randrange(256) for _ in range(20000)))

    def test_truncated_stream_is_padded_like_the_engine(self):
        packed = lz_encode(b"hello world, hello world")
        # the engine reads zero bits past the end; this must not raise
        xa.lz_decompress(packed[:-1])


class ArchiveTests(unittest.TestCase):
    def write(self, tmp: str, blob: bytes) -> str:
        p = os.path.join(tmp, "test.db0")
        Path(p).write_bytes(blob)
        return p

    def test_compressed_toc_and_header(self):
        files = {"configs\\a.ltx": b"x" * 100, "configs\\sub\\b.ltx": b"y" * 5}
        with tempfile.TemporaryDirectory() as tmp:
            a = xa.read_archive(self.write(tmp, make_archive(files, HEADER)))
        self.assertEqual([e.name for e in a.entries], list(files))
        self.assertEqual(a.entries[0].size_real, 100)
        self.assertEqual(a.entries[0].crc, 0xC0FFEE)
        self.assertEqual(a.entry_point, "$fs_root$\\gamedata\\")
        self.assertTrue(a.auto_load)
        self.assertEqual([c.chunk_id for c in a.chunks], [666, 0, 1])
        self.assertEqual([c.compressed for c in a.chunks], [False, False, True])

    def test_entry_offsets_address_the_data(self):
        blob = make_archive({"f": b"HELLO"}, HEADER, compress_toc=False)
        with tempfile.TemporaryDirectory() as tmp:
            a = xa.read_archive(self.write(tmp, blob))
        e = a.entries[0]
        self.assertEqual(blob[e.offset:e.offset + e.size_compressed], b"HELLO")

    def test_names_keep_every_byte(self):
        raw = b"textures\\prop \x97 \xea\xee\xef\xe8\xff.dds"
        blob = make_archive({raw.decode("latin-1"): b"1"}, HEADER)
        with tempfile.TemporaryDirectory() as tmp:
            a = xa.read_archive(self.write(tmp, blob))
        self.assertEqual(a.entries[0].name.encode("latin-1"), raw)

    def test_no_header_chunk(self):
        with tempfile.TemporaryDirectory() as tmp:
            a = xa.read_archive(self.write(tmp, make_archive({"f": b"1"}, None)))
        self.assertIsNone(a.header_ini)
        self.assertIsNone(a.entry_point)

    def test_rejects_offsets_beyond_the_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(xa.ArchiveError):
                xa.read_archive(self.write(tmp, make_archive({"f": b"1"}, HEADER, bad_offset=True)))

    def test_rejects_garbage(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(xa.ArchiveError):
                xa.read_archive(self.write(tmp, b"\x01\x00\x00\x00\xff\xff\xff\x7f" + b"0" * 20))

    def test_archive_names_follow_the_engine_rule(self):
        for name in ("a.db0", "a.db", "a.db12", "a.xdb0", "levels.xdb1"):
            self.assertTrue(xa.is_archive_name(name), name)
        for name in ("Thumbs.db", "a.ltx", "db0", "a.txt", "xdb"):
            self.assertFalse(xa.is_archive_name(name), name)

    def test_header_ini_parsing(self):
        self.assertEqual(xa.parse_header_ini('[header]\nentry_point = gamedata ; c\nauto_load = false\n'), ("gamedata", False))
        self.assertEqual(xa.parse_header_ini("[other]\nentry_point = x\n"), (None, None))


FAKE_CF = r'''#!/usr/bin/env python3
import hashlib, json, os, sys
store = os.environ["FAKE_R2_DIR"]
args = sys.argv[1:]
def opt(name, default=None):
    return args[args.index(name) + 1] if name in args else default
assert args[0] == "r2" and args[1] == "objects"
verb, key = args[2], (args[3] if len(args) > 3 and not args[3].startswith("--") else None)
def path(k): return os.path.join(store, k.replace("/", "__"))
if verb == "list":
    prefix, after = opt("--prefix", ""), opt("--start-after")
    per = int(opt("--per-page", "1000"))
    keys = sorted(f.replace("__", "/") for f in os.listdir(store))
    keys = [k for k in keys if k.startswith(prefix) and (after is None or k > after)][:per]
    out = []
    for k in keys:
        data = open(path(k), "rb").read()
        out.append({"key": k, "etag": hashlib.md5(data).hexdigest(), "size": len(data)})
    print(json.dumps(out))
elif verb == "put":
    fail = os.environ.get("FAKE_R2_FAIL_SUBSTRING")
    if fail and fail in key:
        sys.stderr.write("injected failure\n"); sys.exit(1)
    with open(opt("--file"), "rb") as f, open(path(key), "wb") as g:
        g.write(f.read())
    with open(os.path.join(os.path.dirname(store), "puts.log"), "a") as log:
        log.write(key + "\n")
    print(json.dumps({"key": key}))
elif verb == "get":
    if not os.path.exists(path(key)):
        sys.stderr.write("404\n"); sys.exit(1)
    sys.stdout.buffer.write(open(path(key), "rb").read())
else:
    sys.exit(2)
'''


class PublishTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.store = root / "store"
        self.store.mkdir()
        self.work = root / "work"
        self.src = root / "game"
        (self.src / "db" / "configs").mkdir(parents=True)
        self.fake = root / "fakecf"
        self.fake.write_text(FAKE_CF)
        self.fake.chmod(self.fake.stat().st_mode | stat.S_IXUSR)
        os.environ["FAKE_R2_DIR"] = str(self.store)
        os.environ.pop("FAKE_R2_FAIL_SUBSTRING", None)
        rng = random.Random(1)
        self.blob = bytes(rng.randrange(256) for _ in range(200_000))
        (self.src / "db" / "configs" / "blob.bin").write_bytes(self.blob)
        (self.src / "db" / "configs" / "copy.bin").write_bytes(self.blob)  # same chunks as blob.bin
        (self.src / "db" / "configs" / "a.db0").write_bytes(make_archive({"x\\y.ltx": b"z" * 1000}, HEADER))
        (self.src / "db" / "configs" / ".DS_Store").write_bytes(b"junk")

    def tearDown(self):
        os.environ.pop("FAKE_R2_DIR", None)
        os.environ.pop("FAKE_R2_FAIL_SUBSTRING", None)
        self.tmp.cleanup()

    def run_publish(self, *extra: str) -> tuple[int, str]:
        argv = ["publish", "--source", str(self.src), "--path", "db/configs", "--bucket", "b", "--cf", str(self.fake),
                "--work-dir", str(self.work), "--chunk-size", "64KiB", "--retries", "0", "--local", *extra]
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            code = publish.main(argv)
        return code, buf.getvalue()

    def stored_keys(self):
        return sorted(f.replace("__", "/") for f in os.listdir(self.store))

    def puts(self):
        log = self.store.parent / "puts.log"
        return log.read_text().split() if log.exists() else []

    def test_publish_then_nothing_to_do(self):
        code, out = self.run_publish()
        self.assertEqual(code, 0, out)
        keys = self.stored_keys()
        self.assertIn("data/index.json", keys)
        index = json.loads((self.store / "data__index.json").read_text())
        self.assertEqual(index["format"], publish.INDEX_FORMAT)
        self.assertEqual(index["chunkSize"], 65536)
        paths = [f["path"] for f in index["files"]]
        self.assertEqual(paths, ["db/configs/a.db0", "db/configs/blob.bin", "db/configs/copy.bin"])  # no .DS_Store
        blob = next(f for f in index["files"] if f["path"] == "db/configs/blob.bin")
        self.assertEqual(len(blob["chunks"]), 4)  # 200000 / 65536 rounded up
        self.assertEqual(blob["sha256"], hashlib.sha256(self.blob).hexdigest())
        self.assertEqual(blob["chunks"][0], hashlib.sha256(self.blob[:65536]).hexdigest())
        archive = next(f for f in index["files"] if f["path"] == "db/configs/a.db0")["archive"]
        self.assertEqual(archive["entries"][0][0], "x\\y.ltx")
        self.assertEqual(archive["entryPoint"], "$fs_root$\\gamedata\\")
        # chunk objects: distinct only, and every one hashes to its name
        chunk_keys = [k for k in keys if k.startswith("data/chunks/")]
        self.assertEqual(len(chunk_keys), len(set(chunk_keys)))
        referenced = {h for f in index["files"] for h in f["chunks"]}
        self.assertEqual({k.rsplit("/", 1)[1] for k in chunk_keys}, referenced)
        for k in chunk_keys:
            self.assertEqual(hashlib.sha256((self.store / k.replace("/", "__")).read_bytes()).hexdigest(), k.rsplit("/", 1)[1])
        # blob.bin and copy.bin share all chunks, so 4 + 1 (the archive) objects
        self.assertEqual(len(chunk_keys), 5)
        before = len(self.puts())

        code, out = self.run_publish()
        self.assertEqual(code, 0, out)
        self.assertEqual(len(self.puts()), before, "second run must upload nothing")
        self.assertIn("0 chunks (0 B) and 0 index uploaded", out)

    def test_dry_run_uploads_nothing(self):
        code, out = self.run_publish("--dry-run")
        self.assertEqual(code, 0, out)
        self.assertEqual(self.stored_keys(), [])
        self.assertIn("5 to upload", out)

    def test_resume_after_a_failed_chunk_and_no_index_until_complete(self):
        victim = hashlib.sha256(self.blob[65536:131072]).hexdigest()
        os.environ["FAKE_R2_FAIL_SUBSTRING"] = victim
        code, out = self.run_publish()
        self.assertEqual(code, 1, out)
        self.assertNotIn("data/index.json", self.stored_keys())
        self.assertEqual(len([k for k in self.stored_keys() if k.startswith("data/chunks/")]), 4)

        os.environ.pop("FAKE_R2_FAIL_SUBSTRING")
        before = len(self.puts())
        code, out = self.run_publish()
        self.assertEqual(code, 0, out)
        self.assertEqual(len(self.puts()) - before, 2, "only the failed chunk and the index")
        self.assertIn("data/index.json", self.stored_keys())

    def test_changed_file_gets_new_chunks_only_where_it_changed(self):
        self.assertEqual(self.run_publish()[0], 0)
        before = len(self.puts())
        data = bytearray(self.blob)
        data[0] ^= 0xFF  # first chunk only
        (self.src / "db" / "configs" / "blob.bin").write_bytes(bytes(data))
        code, out = self.run_publish()
        self.assertEqual(code, 0, out)
        self.assertEqual(len(self.puts()) - before, 2, "one new chunk plus the new index")

    def test_wrong_size_object_is_replaced(self):
        self.assertEqual(self.run_publish()[0], 0)
        key = next(k for k in self.stored_keys() if k.startswith("data/chunks/"))
        (self.store / key.replace("/", "__")).write_bytes(b"truncated")
        before = len(self.puts())
        self.assertEqual(self.run_publish()[0], 0)
        self.assertEqual(len(self.puts()) - before, 1)

    def test_verify(self):
        self.assertEqual(self.run_publish()[0], 0)
        argv = ["verify", "--bucket", "b", "--cf", str(self.fake), "--work-dir", str(self.work), "--deep"]
        with contextlib.redirect_stdout(io.StringIO()) as buf:
            self.assertEqual(publish.main(argv), 0, buf.getvalue())
        key = next(k for k in self.stored_keys() if k.startswith("data/chunks/"))
        path = self.store / key.replace("/", "__")
        path.write_bytes(b"x" * path.stat().st_size)  # right size, wrong content
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(publish.main(argv), 1)
        path.unlink()
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(publish.main(argv), 1)

    def test_work_dir_is_left_clean(self):
        self.assertEqual(self.run_publish()[0], 0)
        leftovers = sorted(p.name for p in self.work.iterdir())
        self.assertEqual(leftovers, ["index.json", "plan-cache.json"])

    def test_refuses_paths_outside_the_source_and_symlinks(self):
        with self.assertRaises(SystemExit):
            publish.scan_files(self.src, ["../"])
        link = self.src / "db" / "link"
        link.symlink_to(self.src / "db" / "configs")
        with self.assertRaises(SystemExit):
            publish.scan_files(self.src, ["db/link"])
        names = [rel for rel, _ in publish.scan_files(self.src, ["db"])]
        self.assertFalse(any("link" in n for n in names))

    def test_parse_size(self):
        self.assertEqual(publish.parse_size("4MiB"), 4 * 1024 * 1024)
        self.assertEqual(publish.parse_size("64k"), 65536)
        self.assertEqual(publish.parse_size("1000"), 1000)
        with self.assertRaises(Exception):
            publish.parse_size("lots")


if __name__ == "__main__":
    unittest.main()
