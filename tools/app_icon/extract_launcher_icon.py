#!/usr/bin/python3 -I
"""Writes the largest PNG icon of a Windows executable (RT_GROUP_ICON / RT_ICON resources) to a file.

    extract_launcher_icon.py AnomalyLauncher.exe out.png

Standard library only. Exits 1 when the executable has no PNG-encoded icon (old BMP-encoded icons are not handled).
"""
import struct
import sys

RT_ICON, RT_GROUP_ICON = 3, 14
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def main(exe_path, out_path):
    exe = open(exe_path, "rb").read()
    pe = struct.unpack_from("<I", exe, 0x3C)[0]
    nsec, optsz = struct.unpack_from("<H", exe, pe + 6)[0], struct.unpack_from("<H", exe, pe + 20)[0]
    opt = pe + 24
    datadir = opt + (112 if struct.unpack_from("<H", exe, opt)[0] == 0x20B else 96)
    rsrc_rva = struct.unpack_from("<I", exe, datadir + 2 * 8)[0]

    sections = []
    for i in range(nsec):
        _, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", exe, opt + optsz + i * 40)
        sections.append((va, max(vsize, rsize), rptr))

    def rva_to_offset(rva):
        for va, size, ptr in sections:
            if va <= rva < va + size:
                return rva - va + ptr
        raise ValueError("RVA outside every section")

    base = rva_to_offset(rsrc_rva)

    def children(dir_offset):
        named, ids = struct.unpack_from("<HH", exe, dir_offset + 12)
        for i in range(named + ids):
            name_or_id, target = struct.unpack_from("<II", exe, dir_offset + 16 + i * 8)
            yield name_or_id, base + (target & 0x7FFFFFFF)

    resources = {}  # (type, id) -> bytes
    for type_id, type_dir in children(base):
        if type_id not in (RT_ICON, RT_GROUP_ICON):
            continue
        for res_id, lang_dir in children(type_dir):
            for _, data_entry in children(lang_dir):
                rva, size = struct.unpack_from("<II", exe, data_entry)
                resources[(type_id, res_id)] = exe[rva_to_offset(rva):rva_to_offset(rva) + size]

    best = None  # (width, png)
    for (type_id, _), group in resources.items():
        if type_id != RT_GROUP_ICON:
            continue
        count = struct.unpack_from("<H", group, 4)[0]
        for i in range(count):
            width, _, _, _, _, _, _, icon_id = struct.unpack_from("<BBBBHHIH", group, 6 + i * 14)
            image = resources.get((RT_ICON, icon_id))
            if image and image.startswith(PNG_MAGIC) and (best is None or (width or 256) > best[0]):
                best = (width or 256, image)
    if best is None:
        print("no PNG icon in " + exe_path, file=sys.stderr)
        return 1
    open(out_path, "wb").write(best[1])
    print("%s: %dx%d icon" % (exe_path, best[0], best[0]))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
