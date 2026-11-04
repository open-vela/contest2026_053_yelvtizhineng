#!/usr/bin/env python3
"""Verify the M5 review payload and apply it to an isolated SDK checkout."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "patches/m5/baselines.json"

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def git(directory, *args):
    return subprocess.check_output(["git", "-C", str(directory), *args], text=True).strip()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sdk", nargs="?", type=Path)
    parser.add_argument("--assets", type=Path, help="Root containing separately obtained build assets")
    parser.add_argument("--apply", action="store_true", help="Apply to a dedicated clean SDK")
    args = parser.parse_args()
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    entries = data["review_files"]
    for name, meta in entries.items():
        if sha(ROOT / name) != meta["sha256"]:
            parser.error(f"payload hash mismatch: {name}")
    print(f"Verified {len(entries)} payload files for {data['version']}")
    if args.sdk is None:
        if args.apply:
            parser.error("--apply requires the SDK path")
        return
    sdk = args.sdk.resolve()
    for repo, commit in data["sdk_heads"].items():
        if git(sdk / repo, "rev-parse", "HEAD") != commit:
            parser.error(f"unexpected SDK revision: {repo}")
        if args.apply and git(sdk / repo, "status", "--porcelain"):
            parser.error(f"refusing to modify a dirty SDK: {repo}")
    for name, meta in data.get("dependency_archives", {}).items():
        archive = sdk / name
        if archive.is_file() and sha(archive) != meta["sha256"]:
            parser.error(f"cached dependency hash mismatch: {name}")
    if not args.apply:
        return
    lvgl = sdk / "apps/graphics/lvgl/lvgl"
    if not (lvgl / "lvgl.h").is_file():
        parser.error("unpack the pinned LVGL v9.2.1 tree before applying the overlay")
    external = data.get("external_build_inputs", {})
    if external and args.assets is None:
        parser.error("--apply requires --assets with the exact external inputs in the manifest")
    for name, meta in external.items():
        asset = args.assets / name
        if not asset.is_file() or sha(asset) != meta["sha256"]:
            parser.error(f"external input missing or SHA-256 mismatch: {name}")
    for name, meta in {**entries, **external}.items():
        source_root = args.assets if name in external else ROOT
        dest = sdk / meta["sdk_path"]
        if dest == sdk / "nuttx/.config":
            dest = sdk / "nuttx/boards/xtensa/esp32s3/m5stack-cores3/configs/contest_v672/defconfig"
        if sdk not in dest.resolve().parents:
            parser.error(f"target outside SDK: {dest}")
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source_root / name, dest)
    print("Applied; configure m5stack-cores3:contest_v672 from nuttx")

if __name__ == "__main__":
    main()
