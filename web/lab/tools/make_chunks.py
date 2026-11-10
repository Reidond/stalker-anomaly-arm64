#!/usr/bin/env python3
"""Writes the storage test's stand-in data chunks and their manifest.

Usage: make_chunks.py OUT_DIR [COUNT]

Each chunk is 4 MiB of deterministic pseudo-random bytes (seeded by its index), so the build is
reproducible and the CRC32s in manifest.json let the wasm side verify what the I/O thread delivered.
"""
import json
import random
import sys
import zlib
from pathlib import Path

CHUNK_SIZE = 4 << 20


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = Path(sys.argv[1])
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    out.mkdir(parents=True, exist_ok=True)
    chunks = []
    for i in range(count):
        name = f"chunk-{i:03d}.bin"
        path = out / name
        data = random.Random(i).randbytes(CHUNK_SIZE)
        if not path.exists() or path.stat().st_size != CHUNK_SIZE or path.read_bytes() != data:
            path.write_bytes(data)
        chunks.append({"name": name, "crc32": zlib.crc32(data)})
    (out / "manifest.json").write_text(json.dumps({"chunk_size": CHUNK_SIZE, "chunks": chunks}, indent=1) + "\n")
    print(f"{count} chunks of {CHUNK_SIZE >> 20} MiB in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
