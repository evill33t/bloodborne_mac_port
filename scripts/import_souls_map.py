#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Repeatable, isolated Souls map import jobs for the Bloodborne mod loader.

Conversion coverage is recorded explicitly; a source inventory is never labelled
as a fully converted campaign. Game folders and existing outputs are read-only.
"""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[1]
PRESETS = {
    ("DS1", "m10_02_00_00"): "-51.8,-60.6,55.7",
    ("DS1R", "m10_02_00_00"): "-51.8,-60.6,55.7",
    ("DS2", "m10_04_00_00"): "9,6,-16",
    ("DS2S", "m10_04_00_00"): "9,6,-16",
}


def outside(path, roots):
    # macOS commonly uses case-insensitive APFS. Resolve() follows symlinks but
    # does not canonicalize casing; compare existing ancestor identities too.
    for ancestor in (path, *path.parents):
        for root in roots:
            if ancestor == root or ancestor.exists() and ancestor.samefile(root):
                raise ValueError("Output must be outside game folders: " + str(path))


def run(command, env, capture=False):
    result = subprocess.run(command, cwd=REPO, env=env, check=True,
                            stdout=subprocess.PIPE if capture else None,
                            text=True)
    return result.stdout if capture else None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, help="JSON import job; command-line values override it")
    parser.add_argument("--profile")
    parser.add_argument("--root", type=Path, help="Read-only source game root")
    parser.add_argument("--native-root", type=Path, help="Read-only Bloodborne game root")
    parser.add_argument("--map", dest="source_map")
    parser.add_argument("--target-map")
    parser.add_argument("--source-anchor")
    parser.add_argument("--target-anchor")
    parser.add_argument("--spawn-offset")
    parser.add_argument("--exposure-ev", type=float, help="Imported-area exposure adjustment in stops (default -0.4)")
    parser.add_argument("--fog-mode", choices=("calibrated", "native"), help="Calibrated imported haze or native fog")
    parser.add_argument("--fog-strength", type=float)
    parser.add_argument("--fog-distance", type=float)
    parser.add_argument("--fog-start", type=float)
    parser.add_argument("--fog-color-scale", type=float)
    parser.add_argument("--output", type=Path, help="New mod folder; existing outputs are never overwritten")
    parser.add_argument("--helper", type=Path)
    parser.add_argument("--smithbox", type=Path)
    parser.add_argument("--dotnet", help="Path to dotnet executable")
    parser.add_argument("--build", action="store_true", help="Build the managed adapter before importing")
    parser.add_argument("--plan-only", action="store_true", help="Inventory every map subsystem without converting assets")
    args = parser.parse_args(argv)
    job = json.loads(args.config.read_text()) if args.config else {}
    if not isinstance(job, dict) or job.get("schema", "bbmap-import-job-1") != "bbmap-import-job-1":
        raise ValueError("Expected a bbmap-import-job-1 JSON object")
    for key, value in vars(args).items():
        if key != "config" and value is not None and not isinstance(value, bool):
            job[key] = str(value) if isinstance(value, Path) else value
    for key in ("profile", "root", "native_root", "source_map", "output"):
        if not job.get(key):
            parser.error("Missing --" + key.replace("_", "-") + " or config field " + key)
    requested_output = Path(job["output"]).expanduser().absolute()
    if requested_output.is_symlink():
        raise FileExistsError("Choose a new output folder: " + str(requested_output))
    root, native, output = (Path(job[key]).expanduser().resolve() for key in ("root", "native_root", "output"))
    if job.get("smithbox"):
        job["smithbox"] = str(Path(job["smithbox"]).expanduser().resolve())
    if not root.is_dir() or not native.is_dir():
        raise ValueError("Both source and native game folders must exist")
    outside(output, (root, native))
    if output.exists() or output.is_symlink():
        raise FileExistsError("Choose a new output folder: " + str(output))
    target = job.setdefault("target_map", "m24_01_00_01")
    dotnet = job.get("dotnet") or shutil.which("dotnet") or "/usr/local/share/dotnet/dotnet"
    helper = Path(job.get("helper", str(REPO / "out/map-editor/souls-import/SoulsImport.dll"))).expanduser().resolve()
    env = dict(os.environ, DOTNET_CLI_HOME=str(REPO / "out/map-editor/dotnet-home"))
    if args.build:
        if job.get("smithbox"):
            env["SMITHBOX_DIR"] = str(Path(job["smithbox"]).expanduser().resolve())
        run(["/bin/bash", str(REPO / "scripts/macos/build_souls_import.sh")], env)
    if not helper.is_file():
        raise FileNotFoundError("Managed adapter is missing; use --build --smithbox /path/to/Smithbox")
    common = [dotnet, str(helper)]
    source = ["--profile", job["profile"], "--root", str(root), "--map", job["source_map"], "--native-root", str(native)]
    plan = json.loads(run(common + ["plan-map"] + source, env, capture=True))
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".bbmap-import-", dir=output.parent) as temporary:
        staging = Path(temporary) / "mod"
        if args.plan_only:
            staging.mkdir()
        else:
            anchor = job.get("source_anchor") or PRESETS.get((job["profile"], job["source_map"]))
            if not anchor:
                raise ValueError("This map needs an explicit --source-anchor; use --plan-only to inspect it first")
            job["source_anchor"] = anchor
            job.setdefault("target_anchor", "-195.6,-40.53,220.33")
            command = common + ["bake-map"] + source + ["--target-map", target, "--source-anchor", anchor,
                       "--target-anchor", job["target_anchor"], "--output", str(staging)]
            if job["profile"] in ("DS1", "DS1R"):
                for key, default in (("exposure_ev", -0.4), ("fog_mode", "calibrated"), ("fog_strength", 0.30),
                                     ("fog_distance", 80), ("fog_start", 25), ("fog_color_scale", 0.35)):
                    command += ["--" + key.replace("_", "-"), str(job.setdefault(key, default))]
            if job.get("smithbox"):
                command += ["--smithbox", job["smithbox"]]
            if job.get("spawn_offset"):
                command += ["--spawn-offset", job["spawn_offset"]]
            run(command, env)
        job.update(schema="bbmap-import-job-1", root=str(root), native_root=str(native), output=str(output), helper=str(helper))
        (staging / "import-job.json").write_text(json.dumps(job, indent=2) + "\n")
        (staging / "source-plan.json").write_text(json.dumps(plan, indent=2) + "\n")
        conversion = json.loads((staging / "conversion.json").read_text()) if (staging / "conversion.json").is_file() else {}
        pending = ["source AI/quests/dialogue", "source items", "source music/ambience", "source water rendering/physics",
                   "source navigation", "connected-map loading"]
        pending.append("exact source fog/tonemap/cubemaps" if conversion.get("environmentRows") else "source sky/lighting")
        manifest = {"schema": "bbmap-import-manifest-1", "source": job["profile"], "map": job["source_map"],
                    "target": target, "plan_only": args.plan_only,
                    "source_map_sha256": plan["sourceMapSha256"],
                    "coverage": "Read conversion.json for implemented stages; source-plan.json inventories dependencies.",
                    "unconverted_runtime_systems": pending}
        (staging / "import-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        # Temporary staging stays on the destination filesystem; the final mod
        # appears only after all conversion and metadata writes succeed.
        if output.exists() or output.is_symlink():
            raise FileExistsError("Output appeared during import: " + str(output))
        staging.rename(output)
    print(("Planned " if args.plan_only else "Imported ") + job["source_map"] + " -> " + str(output))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print("Map import: " + str(error), file=sys.stderr)
        raise SystemExit(1)
