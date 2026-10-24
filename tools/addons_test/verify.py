#!/usr/bin/env python3
"""Reads the engine log of a run and judges what the probe (zzz_addon_probe.script) logged.

    verify.py <engine.log> <scenario> [--game] [--translated none|some] [--shell-log <anomaly-shell.log>]

Exit status 0: every ADDON_TEST check passed, the probe's summary names the scenario, and the optional expectations hold:
  --game        the run started a game: the addon script's callbacks must have fired (or, when the scenario has no
                script addon, must not have)
  --translated  ShaderXlat's line of the log (`N shaders requested, M from the translation cache`):
                `none` = every shader came from the cache, `some` = at least one was translated (a changed shader was seen)
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # python3 -I does not add the script's folder
import addons  # noqa: E402  the scenarios


def read(path):
    with open(path, "rb") as f:
        return f.read().decode("cp1251", errors="replace").replace("\r", "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("engine_log")
    ap.add_argument("scenario")
    ap.add_argument("--game", action="store_true")
    ap.add_argument("--translated", choices=("none", "some"))
    ap.add_argument("--shell-log")
    args = ap.parse_args()

    log = read(args.engine_log)
    cfg = addons.SCENARIOS[args.scenario]
    if cfg.get("broken"):
        # no probe here: the point is where the engine says that the addon's script is broken
        lines = [l for l in log.splitlines() if "zz_broken_script" in l.lower() and re.search(r"error|fail|fatal|assert", l, re.I)]
        print(f"scenario {args.scenario}: {len(lines)} log lines about the broken script")
        for l in lines[:6]:
            print("  |", l.strip()[:220])
        if not lines:
            print("  FAIL the engine log does not mention the broken script")
        return 0 if lines else 1
    problems = []
    checks = {}
    for m in re.finditer(r"ADDON_TEST (\w+) (PASS|FAIL)(?: (.*))?", log):
        checks[m.group(1)] = (m.group(2), (m.group(3) or "").strip())

    summary = re.findall(r"ADDON_TEST_SUMMARY scenario=(\w+) pass=(\d+) fail=(\d+)", log)
    begin = re.findall(r"ADDON_TEST_BEGIN scenario=(\w+)", log)
    error = re.findall(r"ADDON_TEST_ERROR (.*)", log)
    if error:
        problems.append(f"probe error: {error[-1]}")
    if not begin:
        problems.append("the probe never ran (no ADDON_TEST_BEGIN): zzz_addon_probe.script was not loaded, or on_game_start was not called")
    elif begin[-1] != args.scenario:
        problems.append(f"the probe ran for scenario {begin[-1]}, expected {args.scenario} (stale zz_addon_expect.script?)")
    if not summary:
        problems.append("no ADDON_TEST_SUMMARY")

    for check_id, (verdict, detail) in checks.items():
        if check_id.startswith("cb_"):
            continue
        if verdict != "PASS":
            problems.append(f"{check_id}: {detail}")

    callbacks = {"cb_first_update", "cb_update"} & set(checks)
    if args.game:
        if cfg.get("script") and callbacks != {"cb_first_update", "cb_update"}:
            problems.append(f"in-game callbacks of the script addon: saw {sorted(callbacks) or 'none'}, wanted cb_first_update and cb_update")
        if not cfg.get("script") and callbacks:
            problems.append(f"in-game callbacks fired without the script addon: {sorted(callbacks)}")

    xlat = re.findall(r"ShaderXlat: (\d+) shaders requested, (\d+) from the translation cache, (\d+) failed", log)
    translated = None
    if xlat:
        requested, hits, failed = (int(v) for v in xlat[-1])
        translated = requested - hits
        if failed:
            problems.append(f"ShaderXlat failed on {failed} shaders")
    if args.translated:
        if translated is None:
            problems.append("no ShaderXlat statistics line in the log")
        elif args.translated == "none" and translated != 0:
            problems.append(f"{translated} shaders were translated again, expected all from the cache")
        elif args.translated == "some" and translated == 0:
            problems.append("no shader was translated: the changed shader was not picked up")

    if args.shell_log:
        # what the texture pack cost: the GPU texture bytes and the process footprint the shell logs every 10 s
        shell = read(args.shell_log)
        tex = [int(m) for m in re.findall(r"textures (\d+) MB", shell)]
        foot = [int(m) for m in re.findall(r"footprint (\d+) MB", shell)]
        if tex and foot:
            print(f"  memory: textures {max(tex)} MB, footprint {max(foot)} MB (max over the run)")

    passed = sum(1 for v, _ in checks.values() if v == "PASS")
    print(f"scenario {args.scenario}: {passed} checks passed, {len(checks) - passed} failed" + (f", {translated} shaders translated" if translated is not None else ""))
    if args.shell_log:
        shell = read(args.shell_log)
        for line in shell.splitlines():
            if "overlay:" in line or "fsgame.ltx" in line:
                print("  shell:", line.strip())
    for p in problems:
        print("  FAIL", p)
    if not problems:
        print("  OK")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
