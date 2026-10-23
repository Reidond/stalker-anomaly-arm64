#!/usr/bin/env python3
"""Stages the test addons of tools/addons_test for one scenario.

    addons.py stage <scenario> <out-dir> [--mode boot|game] [--game-dir <Anomaly folder>] [--xrdb-unpack <binary>]
    addons.py scenarios

`stage` writes the tree a player would copy into the game folder (`Documents/Anomaly` on the iPad):

    <out-dir>/main/gamedata/...        loose files (script, config, texture, shader and MCM addons, the probe)
    <out-dir>/main/db/patches/...      an archive packed from addons/archive_patches
    <out-dir>/main/db/addons/...       an archive packed from addons/archive_addons
    <out-dir>/variants/GameData/...    scenario case_variants: the same kind of files under differently spelled roots
    <out-dir>/variants/DB/Addons/...     (they live in a tree of their own because a Mac volume is usually case-insensitive and would merge them
                                        with main/; the push puts both trees into the game folder)
    <out-dir>/manifest.txt        every file below <out-dir>, one per line, as `main/gamedata/...`

plus main/gamedata/Scripts/zz_addon_expect.script, which tells the probe what this scenario should find (`--mode game`: a game is started,
so axr_main calls the addon scripts' on_game_start; `boot`: the main menu only, the probe is run from the console). Nothing third-party is kept in
the repository: the textures are generated here, and the shader is the stock r3\\distort.ps read out of the game's
shaders.db0 with one line changed (so the game data and tools/xrdb_unpack are needed).
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import xrdb_pack  # noqa: E402

ADDONS = os.path.join(HERE, "addons")

# scenario -> what is installed. "probe" is the harness and is always there.
#   shader: 0 none, 1/2/3 = tint header v1 / v2 (header changed) / v3 (shader body changed)
SCENARIOS = {
    "installed": dict(script=1, config=1, texture=1, shader=1, mcm=1, patches=1, addons=1, loose_priority=1, mcm_write=42, mcm_expect=7),
    "installed_again": dict(script=1, config=1, texture=1, shader=1, mcm=1, patches=1, addons=1, loose_priority=1, mcm_expect=42),
    "shader_v2": dict(script=1, config=1, texture=1, shader=2, mcm=1, patches=1, addons=1, loose_priority=1, mcm_expect=42),
    "shader_v3": dict(script=1, config=1, texture=1, shader=3, mcm=1, patches=1, addons=1, loose_priority=1, mcm_expect=42),
    "case_variants": dict(script=1, config=1, texture=1, shader=1, mcm=1, patches=1, addons=1, loose_priority=1, variants=1, mcm_expect=42),
    "removed": dict(mcm_expect=None, mcm_reset=1),
}

TINTS = {1: "float3(1.0, 0.2, 0.2)", 2: "float3(0.2, 1.0, 0.2)", 3: "float3(0.2, 1.0, 0.2)"}


def find_xrdb_unpack(hint):
    candidates = [hint] if hint else []
    main = os.environ.get("ANOMALY_MAIN_CHECKOUT", "")
    for root in (REPO, main, os.path.join(REPO, "..", "..", "..")):
        if not root:
            continue
        for build in ("macos-arm64-release", "macos-arm64", "macos-modules"):
            candidates.append(os.path.join(root, "build", build, "tools", "xrdb_unpack", "xrdb_unpack"))
    for c in candidates:
        if c and os.path.isfile(c):
            return os.path.abspath(c)
    raise SystemExit("xrdb_unpack not found: build it (cmake --build build/macos-arm64-release --target xrdb_unpack) or pass --xrdb-unpack")


def find_game_dir(hint):
    candidates = [hint, os.environ.get("ANOMALY_GAME_DIR", ""), os.path.join(REPO, "game", "Anomaly"),
                  os.path.join(REPO, "..", "..", "..", "game", "Anomaly")]
    for c in candidates:
        if c and os.path.isfile(os.path.join(c, "db", "shaders.db0")):
            return os.path.abspath(c)
    raise SystemExit("game data not found: pass --game-dir <the Anomaly folder with db/> or set ANOMALY_GAME_DIR")


class Stock:
    """Reads sizes and files of the stock game out of its archives with xrdb_unpack."""

    def __init__(self, game_dir, xrdb_unpack):
        self.game_dir = game_dir
        self.tool = xrdb_unpack

    def size(self, archive, path):
        out = subprocess.run([self.tool, "list", os.path.join(self.game_dir, archive), path], capture_output=True, text=True, check=True).stdout
        for line in out.splitlines():
            parts = line.split()
            if parts and parts[-1] == path:
                return int(parts[0])
        raise SystemExit(f"{path} not found in {archive}")

    def read(self, archive, path):
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run([self.tool, "extract", os.path.join(self.game_dir, archive), "-o", tmp, path], capture_output=True, check=True)
            for root, _, names in os.walk(tmp):
                for name in names:
                    if name.lower() == os.path.basename(path).lower():
                        with open(os.path.join(root, name), "rb") as f:
                            return f.read()
        raise SystemExit(f"{path} could not be extracted from {archive}")


def dxt1(width, height, cell, colors):
    """A DXT1 DDS (no mip chain) with a checker of cell x cell pixel squares in two colors."""
    def rgb565(r, g, b):
        return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)

    c = [rgb565(*col) for col in colors]
    blocks = bytearray()
    for by in range(height // 4):
        for bx in range(width // 4):
            color = c[((bx * 4) // cell + (by * 4) // cell) % 2]
            blocks += struct.pack("<HHI", color, color, 0)
    header = b"DDS " + struct.pack("<7I", 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x80000, height, width, len(blocks), 0, 1)
    header += b"\x00" * 44
    header += struct.pack("<II4s5I", 32, 0x4, b"DXT1", 0, 0, 0, 0, 0)
    header += struct.pack("<5I", 0x1000, 0, 0, 0, 0)
    assert len(header) == 128
    return header + bytes(blocks)


def write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(path, mode, **({} if isinstance(data, bytes) else {"newline": ""})) as f:
        f.write(data)
    return len(data) if isinstance(data, bytes) else len(data.encode("utf-8"))


def copy_tree(src, dst):
    for root, dirs, names in os.walk(src):
        dirs.sort()
        for name in sorted(names):
            if name.startswith("."):
                continue
            rel = os.path.relpath(os.path.join(root, name), src)
            os.makedirs(os.path.dirname(os.path.join(dst, rel)), exist_ok=True)
            shutil.copyfile(os.path.join(root, name), os.path.join(dst, rel))


def lua_table(d, indent="\t"):
    lines = []
    for k, v in d.items():
        if v is None:
            continue
        if isinstance(v, dict):
            lines.append(f"{indent}{k} = {{\n{lua_table(v, indent + chr(9))}{indent}}},\n")
        elif isinstance(v, bool):
            lines.append(f"{indent}{k} = {'true' if v else 'false'},\n")
        elif isinstance(v, (int, float)):
            lines.append(f"{indent}{k} = {v},\n")
        else:
            lines.append(f'{indent}{k} = "{v}",\n')
    return "".join(lines)


def patched_distort(stock_ps, version, nonce):
    """The stock r3\\distort.ps (the main menu's image pass: CRenderTarget creates it at start-up) with the picture tinted."""
    text = stock_ps.decode("latin-1")
    if "common.h" not in text or not re.search(r"return\s+float4\s*\(\s*image\s*,\s*1\s*\)\s*;", text):
        raise SystemExit("stock distort.ps is not what this test expects")
    text = re.sub(r'(#include\s+"common\.h"[^\n]*)', lambda m: m.group(1) + '\n#include "zz_addon_tint.h"', text, count=1)
    factor = " * 0.9" if version == 3 else ""
    text = re.sub(r"return\s+float4\s*\(\s*image\s*,\s*1\s*\)\s*;", f"return float4(image * ZZ_ADDON_TINT{factor}, 1);", text, count=1)
    # the translation cache is content-addressed and keeps what it has seen: a run id makes every run's shader new to it
    text += f"\n// addons_test run {nonce}{' v3' if version == 3 else ''}\n"
    return text.encode("latin-1")


def stage(scenario, out, game_dir, xrdb_unpack, mode="boot", nonce="0"):
    if scenario not in SCENARIOS:
        raise SystemExit(f"unknown scenario {scenario}; known: {', '.join(SCENARIOS)}")
    cfg = SCENARIOS[scenario]
    stock = Stock(game_dir, xrdb_unpack)
    if os.path.exists(out):
        shutil.rmtree(out)
    main = os.path.join(out, "main")
    gamedata = os.path.join(main, "gamedata")
    os.makedirs(gamedata)

    sizes = {
        "main_menu_tex": stock.size("db/textures/textures_ui.db0", "textures/ui/ui_actor_main_menu2.dds"),
        "distort_ps": stock.size("db/shaders.db0", "shaders/r3/distort.ps"),
        "ph_car": stock.size("db/configs/scripts.db0", "scripts/ph_car_original.scriptx"),
        "axr_main": os.path.getsize(os.path.join(REPO, "gamedata", "scripts", "axr_main.script")),
        "ipad_debug": os.path.getsize(os.path.join(REPO, "gamedata", "scripts", "ipad_debug.script")),
    }
    layer = {}

    # the harness is always installed
    copy_tree(os.path.join(ADDONS, "probe", "gamedata"), gamedata)

    for name in ("script", "config", "mcm"):
        if cfg.get(name):
            copy_tree(os.path.join(ADDONS, name, "gamedata"), gamedata)
    if cfg.get("loose_priority"):
        copy_tree(os.path.join(ADDONS, "loose_priority", "gamedata"), gamedata)

    if cfg.get("texture"):
        # the main menu background, in the spelling of a Windows texture pack, as a 1024x1024 DXT1 test card
        sizes["main_menu_tex"] = write(os.path.join(gamedata, "Textures", "UI", "UI_Actor_Main_Menu2.DDS"), dxt1(1024, 1024, 64, ((255, 0, 255), (0, 255, 0))))
        sizes["new_tex"] = write(os.path.join(gamedata, "Textures", "ui", "ui_zz_addon_new.dds"), dxt1(64, 64, 8, ((255, 128, 0), (0, 128, 255))))

    if cfg.get("shader"):
        v = cfg["shader"]
        ps = patched_distort(stock.read("db/shaders.db0", "shaders/r3/distort.ps"), v, nonce)
        sizes["distort_ps"] = write(os.path.join(gamedata, "Shaders", "R3", "Distort.PS"), ps)
        header = f"// test addon \"shader\": tint of the main menu's image pass (version {v}, run {nonce})\n#define ZZ_ADDON_TINT {TINTS[v]}\n"
        sizes["tint_h"] = write(os.path.join(gamedata, "Shaders", "R3", "ZZ_Addon_Tint.H"), header)

    if cfg.get("patches"):
        pack = os.path.join(main, "db", "patches", "ZZ_Addon_Patches.DB0")
        xrdb_pack.pack(os.path.join(ADDONS, "archive_patches"), pack, "gamedata", True)
        sizes["ph_car"] = os.path.getsize(os.path.join(ADDONS, "archive_patches", "scripts", "ph_car_original.scriptx"))
        sizes["ipad_debug"] = os.path.getsize(os.path.join(ADDONS, "archive_patches", "scripts", "ipad_debug.script"))
        layer["only_patches"] = "patches"
    if cfg.get("addons"):
        pack = os.path.join(main, "db", "addons", "ZZ_Addon_Pack.db0")
        xrdb_pack.pack(os.path.join(ADDONS, "archive_addons"), pack, "$fs_root$\\gamedata\\", False)
        layer["only_addons"] = "addons"

    if cfg.get("variants"):
        copy_tree(os.path.join(ADDONS, "case_variants"), os.path.join(out, "variants"))
        xrdb_pack.pack(os.path.join(ADDONS, "case_variants_pack"), os.path.join(out, "variants", "DB", "Addons", "ZZ_Case_Pack.DB0"), "gamedata", False)
        layer["case_pack"] = "case"

    # the file in several layers: the last one mounted wins (stock < overlay < db\\patches < db\\addons < loose gamedata)
    for name, loose in (("prio_both", False), ("prio_loose", bool(cfg.get("loose_priority")))):
        winner = "loose" if loose else "addons" if cfg.get("addons") else "patches" if cfg.get("patches") else None
        if winner:
            layer[name] = winner

    want = {
        "game": mode == "game",
        "variants": bool(cfg.get("variants")),
        "script": bool(cfg.get("script")), "config": bool(cfg.get("config")), "texture": bool(cfg.get("texture")),
        "shader": bool(cfg.get("shader")), "mcm": bool(cfg.get("mcm")),
        "archive_patches": bool(cfg.get("patches")), "archive_addons": bool(cfg.get("addons")),
        "mcm_reset": bool(cfg.get("mcm_reset")),
        "mcm_write": cfg.get("mcm_write"), "mcm_expect": cfg.get("mcm_expect"),
        "size": sizes, "layer": layer,
    }
    write(os.path.join(gamedata, "Scripts", "zz_addon_expect.script"),
          f'-- generated by tools/addons_test/addons.py\nscenario = "{scenario}"\nwant = {{\n{lua_table(want)}}}\n')

    manifest = []
    for root, _, names in os.walk(out):
        for name in names:
            manifest.append(os.path.relpath(os.path.join(root, name), out))
    manifest.sort()
    write(os.path.join(out, "manifest.txt"), "\n".join(manifest) + "\n")
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("stage")
    p.add_argument("scenario")
    p.add_argument("out")
    p.add_argument("--mode", choices=("boot", "game"), default="boot")
    p.add_argument("--nonce", default=os.environ.get("ADDONS_TEST_NONCE", "0"),
                   help="run id written into the shader files so the translation cache has not seen them (default $ADDONS_TEST_NONCE)")
    p.add_argument("--game-dir")
    p.add_argument("--xrdb-unpack")
    sub.add_parser("scenarios")
    args = ap.parse_args()
    if args.cmd == "scenarios":
        for name in SCENARIOS:
            print(name)
        return 0
    manifest = stage(args.scenario, args.out, find_game_dir(args.game_dir), find_xrdb_unpack(args.xrdb_unpack), args.mode, args.nonce)
    print(f"{args.out}: {len(manifest)} files for scenario {args.scenario}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
