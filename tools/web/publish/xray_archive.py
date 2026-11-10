"""Reader for X-Ray engine archives (.db0, .xdb0, ...): chunk directory and table of contents.

Format, as read by CLocatorAPI (src/xrCore/LocatorAPI.cpp, open_chunk_fd / ProcessArchive /
LoadArchive) and written by xrCompress:

* The file is a sequence of chunks starting at offset 0: u32 type, u32 size, then `size` bytes.
  Bit 31 of the type (CFS_CompressMark) marks a payload compressed with LZHUF (src/xrCore/lzhuf.cpp).
* Chunk 0 holds the file data; table-of-contents entries point into the archive by absolute offset.
* Chunk 1 is the table of contents: records of u16 length, then u32 size_real, u32 size_compressed,
  u32 crc, the name (length - 16 bytes, no terminator), u32 offset.
* Chunk 666 (CFS_HeaderChunkID) is an optional INI text with `[header]` keys such as `entry_point`
  and `auto_load`.

Standard library only.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

CFS_COMPRESS_MARK = 1 << 31
CFS_TOC_CHUNK_ID = 1
CFS_HEADER_CHUNK_ID = 666

# ---- LZHUF (Okumura/Yoshizaki, as in src/xrCore/lzhuf.cpp) --------------------------------------

_N = 4096  # ring buffer size
_F = 60  # look-ahead size
_THRESHOLD = 2
_N_CHAR = 256 - _THRESHOLD + _F  # 314
_T = _N_CHAR * 2 - 1  # 627
_R = _T - 1
_MAX_FREQ = 0x4000

_P_LEN = [
    3, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
]
_P_CODE = [
    0x00, 0x20, 0x30, 0x40, 0x50, 0x58, 0x60, 0x68,
    0x70, 0x78, 0x80, 0x88, 0x90, 0x94, 0x98, 0x9C,
    0xA0, 0xA4, 0xA8, 0xAC, 0xB0, 0xB4, 0xB8, 0xBC,
    0xC0, 0xC2, 0xC4, 0xC6, 0xC8, 0xCA, 0xCC, 0xCE,
    0xD0, 0xD2, 0xD4, 0xD6, 0xD8, 0xDA, 0xDC, 0xDE,
    0xE0, 0xE2, 0xE4, 0xE6, 0xE8, 0xEA, 0xEC, 0xEE,
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
    0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
]


def _decode_tables() -> tuple[list[int], list[int]]:
    """d_code / d_len of lzhuf.cpp follow from p_len / p_code: byte b decodes to table row i when
    b falls in the 2^(8 - p_len[i]) values starting at p_code[i]."""
    d_code = [0] * 256
    d_len = [0] * 256
    for i in range(64):
        span = 1 << (8 - _P_LEN[i])
        for b in range(_P_CODE[i], _P_CODE[i] + span):
            d_code[b] = i
            d_len[b] = _P_LEN[i]
    return d_code, d_len


_D_CODE, _D_LEN = _decode_tables()


class ArchiveError(Exception):
    pass


def lz_decompress(src: bytes) -> bytes:
    """Inverse of _compressLZ: a u32 little-endian output size, then the adaptive Huffman stream."""
    if len(src) < 4:
        raise ArchiveError("LZ stream shorter than its size field")
    textsize = int.from_bytes(src[:4], "little")
    if textsize == 0:
        return b""

    freq = [0] * (_T + 1)
    son = [0] * _T
    prnt = [0] * (_T + _N_CHAR + 1)

    for i in range(_N_CHAR):
        freq[i] = 1
        son[i] = i + _T
        prnt[i + _T] = i
    i, j = 0, _N_CHAR
    while j <= _R:
        freq[j] = freq[i] + freq[i + 1]
        son[j] = i
        prnt[i] = prnt[i + 1] = j
        i += 2
        j += 1
    freq[_T] = 0xFFFF
    prnt[_R] = 0

    def reconst() -> None:
        j = 0
        for i in range(_T):
            if son[i] >= _T:
                freq[j] = (freq[i] + 1) // 2
                son[j] = son[i]
                j += 1
        i = 0
        for j in range(_N_CHAR, _T):
            f = freq[j] = freq[i] + freq[i + 1]
            k = j - 1
            while f < freq[k]:
                k -= 1
            k += 1
            n = j - k
            freq[k + 1 : k + 1 + n] = freq[k : k + n]
            freq[k] = f
            son[k + 1 : k + 1 + n] = son[k : k + n]
            son[k] = i
            i += 2
        for i in range(_T):
            k = son[i]
            if k >= _T:
                prnt[k] = i
            else:
                prnt[k] = prnt[k + 1] = i

    def update(c: int) -> None:
        if freq[_R] == _MAX_FREQ:
            reconst()
        c = prnt[c + _T]
        while True:
            freq[c] += 1
            k = freq[c]
            l = c + 1
            if k > freq[l]:
                l += 1
                while k > freq[l]:
                    l += 1
                l -= 1
                freq[c] = freq[l]
                freq[l] = k
                i = son[c]
                prnt[i] = l
                if i < _T:
                    prnt[i + 1] = l
                j = son[l]
                son[l] = i
                prnt[j] = c
                if j < _T:
                    prnt[j + 1] = c
                son[c] = j
                c = l
            c = prnt[c]
            if c == 0:
                break

    pos = 4
    end = len(src)
    getbuf = 0
    getlen = 0

    text_buf = bytearray(b" " * (_N - _F)) + bytearray(_F)  # the C globals leave the tail zeroed
    r = _N - _F
    out = bytearray()
    count = 0
    while count < textsize:
        # DecodeChar
        c = son[_R]
        while c < _T:
            while getlen <= 8:
                b = src[pos] if pos < end else 0
                pos += 1
                getbuf |= b << (8 - getlen)
                getlen += 8
            bit = (getbuf & 0x8000) >> 15
            getbuf = (getbuf << 1) & 0xFFFF
            getlen -= 1
            c = son[c + bit]
        c -= _T
        update(c)

        if c < 256:
            out.append(c)
            text_buf[r] = c
            r = (r + 1) & (_N - 1)
            count += 1
            continue

        # DecodePosition
        while getlen <= 8:
            b = src[pos] if pos < end else 0
            pos += 1
            getbuf |= b << (8 - getlen)
            getlen += 8
        i = (getbuf & 0xFF00) >> 8
        getbuf = (getbuf << 8) & 0xFFFF
        getlen -= 8
        hi = _D_CODE[i] << 6
        extra = _D_LEN[i] - 2
        while extra:
            while getlen <= 8:
                b = src[pos] if pos < end else 0
                pos += 1
                getbuf |= b << (8 - getlen)
                getlen += 8
            bit = (getbuf & 0x8000) >> 15
            getbuf = (getbuf << 1) & 0xFFFF
            getlen -= 1
            i = (i << 1) + bit
            extra -= 1
        start = (r - (hi | (i & 0x3F)) - 1) & (_N - 1)
        run = c - 255 + _THRESHOLD
        for k in range(run):
            ch = text_buf[(start + k) & (_N - 1)]
            out.append(ch)
            text_buf[r] = ch
            r = (r + 1) & (_N - 1)
            count += 1
    return bytes(out)


# ---- archive structure --------------------------------------------------------------------------


@dataclass
class Chunk:
    chunk_id: int
    compressed: bool
    offset: int  # of the payload
    size: int  # payload bytes as stored


@dataclass
class TocEntry:
    name: str
    offset: int
    size_real: int
    size_compressed: int
    crc: int

    def as_row(self) -> list:
        return [self.name, self.offset, self.size_real, self.size_compressed, self.crc]


@dataclass
class Archive:
    size: int
    chunks: list[Chunk] = field(default_factory=list)
    header_ini: str | None = None
    entry_point: str | None = None
    auto_load: bool | None = None
    entries: list[TocEntry] = field(default_factory=list)


def read_chunk_directory(f, file_size: int) -> list[Chunk]:
    """Walks the chunk list from offset 0, as open_chunk_fd does, until the end of the file."""
    chunks: list[Chunk] = []
    pos = 0
    while pos + 8 <= file_size:
        f.seek(pos)
        raw = f.read(8)
        if len(raw) < 8:
            break
        ctype, csize = struct.unpack("<II", raw)
        payload = pos + 8
        if payload + csize > file_size:
            raise ArchiveError(f"chunk at {pos} claims {csize} bytes but the file ends at {file_size}")
        chunks.append(Chunk(ctype & ~CFS_COMPRESS_MARK & 0xFFFFFFFF, bool(ctype & CFS_COMPRESS_MARK), payload, csize))
        pos = payload + csize
    if pos != file_size:
        raise ArchiveError(f"{file_size - pos} bytes after the last chunk")
    return chunks


def _read_chunk(f, chunk: Chunk) -> bytes:
    f.seek(chunk.offset)
    data = f.read(chunk.size)
    if len(data) != chunk.size:
        raise ArchiveError("short read of chunk payload")
    return lz_decompress(data) if chunk.compressed else data


def parse_toc(data: bytes, archive_size: int) -> list[TocEntry]:
    entries: list[TocEntry] = []
    pos = 0
    n = len(data)
    while pos < n:
        if pos + 2 > n:
            raise ArchiveError("truncated TOC record header")
        (length,) = struct.unpack_from("<H", data, pos)
        pos += 2
        if length < 16 or pos + length > n:
            raise ArchiveError(f"bad TOC record length {length} at {pos - 2}")
        size_real, size_compr, crc = struct.unpack_from("<III", data, pos)
        name_len = length - 16
        raw_name = data[pos + 12 : pos + 12 + name_len]
        (offset,) = struct.unpack_from("<I", data, pos + 12 + name_len)
        pos += length
        # Names are raw bytes of the Windows code page (a few stock textures have Cyrillic in them).
        # latin-1 maps every byte to one character, so nothing is lost and the engine's bytes come back
        # with ord(c) per character.
        name = raw_name.decode("latin-1")
        if offset + size_compr > archive_size:
            raise ArchiveError(f"{name}: data at {offset}+{size_compr} lies beyond the archive ({archive_size})")
        entries.append(TocEntry(name, offset, size_real, size_compr, crc))
    return entries


def parse_header_ini(text: str) -> tuple[str | None, bool | None]:
    """Reads entry_point and auto_load from the [header] section (CInifile semantics, simplified)."""
    section = None
    entry_point = None
    auto_load = None
    for line in text.splitlines():
        line = line.split(";", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1].strip().lower()
            continue
        if section != "header" or "=" not in line:
            continue
        key, value = (s.strip() for s in line.split("=", 1))
        key = key.lower()
        if key == "entry_point":
            entry_point = value.strip('"')
        elif key == "auto_load":
            auto_load = value.lower() in ("on", "yes", "true", "1")
    return entry_point, auto_load


def read_archive(path: str) -> Archive:
    import os

    size = os.path.getsize(path)
    with open(path, "rb") as f:
        archive = Archive(size=size, chunks=read_chunk_directory(f, size))
        toc_chunks = [c for c in archive.chunks if c.chunk_id == CFS_TOC_CHUNK_ID]
        if not toc_chunks:
            raise ArchiveError("no table-of-contents chunk")
        archive.entries = parse_toc(_read_chunk(f, toc_chunks[0]), size)
        for c in archive.chunks:
            if c.chunk_id == CFS_HEADER_CHUNK_ID:
                text = _read_chunk(f, c).decode("latin-1")
                archive.header_ini = text
                archive.entry_point, archive.auto_load = parse_header_ini(text)
                break
    return archive


def is_archive_name(filename: str) -> bool:
    """Same test as CLocatorAPI::file_list_open_ex: extension starts with .db or .xdb, not Thumbs.db."""
    if filename == "Thumbs.db":
        return False
    dot = filename.rfind(".")
    if dot < 0:
        return False
    ext = filename[dot:]
    return ext.startswith(".db") or ext.startswith(".xdb")
