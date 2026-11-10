#!/usr/bin/env python3
"""Groups the live heap of a run by call stack and by subsystem (docs/web-port/MEMORY.md).

    mem_top.py [--top N] <mallochist_<s>.txt>

The file is `malloc_history <pid> -allBySize` of an app started with MallocStackLogging=1, which run_scene.sh writes when
HARNESS_ENV contains MallocStackLogging and HARNESS_MEMSNAP names the seconds to take it at. Each line is "N calls for B bytes:
<stack>", outermost frame first. The subsystem of a stack is decided by its innermost frame that matches one of the patterns below
(the allocator's own frames are skipped); it is a cheap attribution, not an audit. The numbers cover malloc blocks only (stacks that end in
libsystem_malloc): LuaJIT's own heap, Metal textures and the mapped file windows are not in it.
"""
import re
import sys
from collections import defaultdict

MB = 1048576.0
LINE = re.compile(r"^(\d+) calls? for (\d+) bytes: (.*)$")
# frames that say nothing about who owns the memory: the allocator, containers, readers and the file layer
SKIP = re.compile(r"libsystem_malloc|libsystem_c|libc\+\+|xr_alloc|xr_malloc|xr_realloc|xr_calloc|xr_new|xr_strdup|xrMemory::|mem_alloc|mem_realloc|"
                  r"operator new|std::__1::|__cxx|allocator|xalloc|_malloc_zone|xr_apple_|CLocatorAPI::r_open|CLocatorAPI::file_from|"
                  r"CLocatorAPI::archive_|IReader|IReaderBase|CTempReader|CPackReader|rtc_decompress")
# the owner is decided by the innermost frame (after SKIP) that matches; the first pattern that matches that frame names it
SUBSYSTEMS = [
    ("sound: decoded-audio cache and OpenAL", r"SoundRender|xrSound|CSound|OpenAL|alc[A-Z]|\bal[A-Z][a-z]|ov_|vorbis"),
    ("collision model (xrCDB, Opcode)", r"CDB::|Opcode::|CObjectSpace|ICF::"),
    ("A-Life spawn registry, game graph, AI map and cover", r"CALifeSpawnRegistry|CGameGraph|CLevelGraph|CAI_Space|CCoverManager|CQuadTree|"
                                                          r"spawn_overlays|CGameGraphBuilder|CPatrolPath|CALife|alife"),
    ("models: skeletons, visuals, shared geometry containers", r"CModelPool|CKinematics|smem_container|FHierrarhyVisual|CSkeletonX|"
                                                              r"Fvisual|CSkeletonX_ext|FProgressive|FLOD|FTreeVisual"),
    ("ini files and settings (CInifile)", r"CInifile|DLTX|CScriptIniFile"),
    ("XML and UI (TinyXML, windows, fonts, ImGui)", r"TiXml|CUI|UI[A-Z]|CGameFont|XML|ImGui|ImFont|xr_imgui"),
    ("Lua, luabind and scripts", r"lua|Lua|luabind|script"),
    ("file system tables (archive directories)", r"CLocatorAPI|CArchive|FS_Path"),
    ("renderer: resources, shaders, blenders, details", r"xrRender|CRender|dx10|CTexture|CBlender|CDetailManager|CResourceManager|R_|"
                                                        r"ShaderXlat|vkd3d|D3DX|mtl_d3d11|Metal|MTL|CGlow|CLight|CPortal|CSector"),
    ("game objects and entities", r"xrGame|CGameObject|CSE_|CLevel|CObject|CActor|CInventory|CEntity|xrServer|CSpaceRestriction"),
    ("physics (ODE, CPhysics)", r"\bdx[A-Z]|\bd[A-Z][a-z]+(World|Space|Body|Geom)|CPH|CPhysics|ph_"),
    ("engine core (strings, containers, console, device)", r"shared_str|str_container|CConsole|CRenderDevice|CEngine|xrCore|IGame_|CCC_"),
]


def frames(stack):
    out = []
    for f in stack.split(" | "):
        m = re.match(r"^0x[0-9a-f]+ \(([^)]*)\) (.*)$", f.strip())
        out.append((m.group(1), m.group(2)) if m else ("", f.strip()))
    return out


def subsystem(fr):
    for mod, name in reversed(fr):
        if SKIP.search(mod + " " + name):
            continue
        for label, pat in SUBSYSTEMS:
            if re.search(pat, name):
                return label
    return "other / unattributed"


def main(argv):
    top = 25
    path = None
    i = 0
    while i < len(argv):
        if argv[i] == "--top":
            top = int(argv[i + 1])
            i += 2
        else:
            path = argv[i]
            i += 1
    if not path:
        print(__doc__)
        return 2
    stacks = []
    with open(path, errors="replace") as f:
        for line in f:
            m = LINE.match(line.strip())
            if m:
                fr = frames(m.group(3))
                # the log also holds VM allocations (Metal textures, thread stacks, mapped files): only malloc blocks count here
                if any("libsystem_malloc" in mod for mod, _ in fr):
                    stacks.append((int(m.group(2)), int(m.group(1)), fr))
    total = sum(b for b, _, _ in stacks)
    stacks.sort(key=lambda s: -s[0])
    by_sub = defaultdict(lambda: [0, 0])
    for b, n, fr in stacks:
        s = subsystem(fr)
        by_sub[s][0] += b
        by_sub[s][1] += n
    print(f"Live malloc blocks: {total / MB:,.0f} MB in {sum(n for _, n, _ in stacks):,} blocks, {len(stacks):,} call stacks\n")
    print("| subsystem | MB | share | blocks |")
    print("|---|---|---|---|")
    for s, (b, n) in sorted(by_sub.items(), key=lambda kv: -kv[1][0]):
        print(f"| {s} | {b / MB:,.0f} | {100.0 * b / total:.0f} % | {n:,} |")
    print(f"\nThe {top} largest call stacks (innermost engine frames last):\n")
    print("| MB | blocks | subsystem | stack (innermost 6 frames that are not the allocator) |")
    print("|---|---|---|---|")
    for b, n, fr in stacks[:top]:
        keep = [name.split("  ")[0] for mod, name in fr if not SKIP.search(mod + " " + name)][-6:]
        print(f"| {b / MB:,.1f} | {n:,} | {subsystem(fr)} | {' > '.join(k[:70] for k in keep)} |")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
