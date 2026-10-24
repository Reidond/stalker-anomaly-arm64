#!/usr/bin/env python3
"""Packs a folder into an X-Ray archive (.db0 / .db1 ...), the format of src/xrCore/LocatorAPI.cpp.

    xrdb_pack.py <folder> <out.db0> [--entry-point gamedata|'$fs_root$\\gamedata\\'] [--compress]

The folder's files get the archive paths they have below it (use `scripts/foo.script`, not `gamedata/scripts/foo.script`;
the entry point supplies the `gamedata\\` part). Layout, as the shell's overlay packer writes it:
[chunk 0: file data][chunk 666: header ini][chunk 1: file table]. The table is not compressed (the engine accepts that).

--compress stores every file as an LZO1X stream made of one literal run (valid LZO, no matches): the entry then has
size_compressed != size_real, which is what makes the engine decompress it, so the compressed read path is exercised
without an LZO compressor. Real archives made by xrCompress have the same entry layout.
"""
import argparse
import os
import struct
import sys
import zlib

HEADER_CHUNK = 666
TABLE_CHUNK = 1
DATA_CHUNK = 0


def lzo1x_literals(data: bytes) -> bytes:
    """An LZO1X stream that copies `data` as literals and ends with the end-of-stream marker."""
    n = len(data)
    end = b"\x11\x00\x00"
    if n == 0:
        return end
    if n <= 3:
        return bytes([17 + n]) + data + end
    if n <= 18:
        return bytes([n - 3]) + data + end
    extra = n - 18
    zeros = (extra - 1) // 255
    last = extra - zeros * 255
    return b"\x00" * (1 + zeros) + bytes([last]) + data + end


def collect(folder: str):
    files = []
    for root, dirs, names in os.walk(folder):
        dirs.sort()
        for name in sorted(names):
            if name.startswith("."):
                continue
            full = os.path.join(root, name)
            rel = os.path.relpath(full, folder).replace(os.sep, "\\")
            files.append((rel, full))
    files.sort(key=lambda f: f[0])
    return files


def pack(folder: str, out_path: str, entry_point: str, compress: bool, level_name: str = "addons_test") -> int:
    data = bytearray()
    table = bytearray()
    files = collect(folder)
    if not files:
        raise SystemExit(f"{folder}: no files")
    for rel, full in files:
        with open(full, "rb") as f:
            raw = f.read()
        stored = lzo1x_literals(raw) if compress else raw
        offset = 8 + len(data)
        data += stored
        name = rel.encode("utf-8")
        entry = struct.pack("<III", len(raw), len(stored), zlib.crc32(raw) & 0xFFFFFFFF) + name + struct.pack("<I", offset)
        table += struct.pack("<H", len(entry)) + entry
    header = (
        "[header]\nauto_load = true\ncreator = \"addons_test\"\n"
        f"entry_point = {entry_point}\nlevel_name = {level_name}\nlevel_ver = 1.0\n"
    ).encode("ascii")
    out = bytearray()
    for chunk_id, payload in ((DATA_CHUNK, data), (HEADER_CHUNK, header), (TABLE_CHUNK, table)):
        out += struct.pack("<II", chunk_id, len(payload)) + payload
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(out)
    return len(files)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("out")
    ap.add_argument("--entry-point", default="$fs_root$\\gamedata\\")
    ap.add_argument("--compress", action="store_true")
    args = ap.parse_args()
    count = pack(args.folder, args.out, args.entry_point, args.compress)
    print(f"{args.out}: {count} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
