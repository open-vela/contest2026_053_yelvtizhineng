#!/usr/bin/env python3
"""Check or apply reviewed soil-node dependencies in an independent SDK."""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


def digest(path):
    if not path.exists():
        return None
    if not path.is_file():
        raise ValueError('Expected file: ' + str(path))
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sdk_root', type=Path)
    parser.add_argument('--esp-hal', type=Path, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--check', action='store_true', help='Read only; show planned operations')
    mode.add_argument('--apply', action='store_true', help='Back up and apply local dependencies')
    args = parser.parse_args()
    root = args.sdk_root.resolve()
    package = Path(__file__).resolve().parents[2]
    trees = {'nuttx': root / 'nuttx', 'apps': root / 'apps', 'esp-hal': args.esp_hal.resolve()}
    for name, tree in trees.items():
        if not tree.is_dir():
            parser.error('Missing ' + name + ' directory: ' + str(tree))
        try:
            tree.relative_to(root)
        except ValueError:
            parser.error('Use independent dependencies inside sdk_root: ' + str(tree))

    patches = package / 'patches/soil-node'
    metadata = json.loads((patches / 'baselines.json').read_text(encoding='utf-8'))
    pending = {}
    for name, tree in trees.items():
        states = []
        for item in metadata['files']:
            if item['tree'] != name:
                continue
            current = digest(tree / item['path'])
            if current == item['after_sha256']:
                states.append('applied')
            elif current == item['before_sha256']:
                states.append('pending')
            else:
                parser.error('File differs from reviewed before/after: ' + str(tree / item['path']))
        if len(set(states)) != 1:
            parser.error('Partially applied dependency; restore or reconcile first: ' + name)
        pending[name] = states[0] == 'pending'
        print(name + ': ' + states[0])
        if pending[name]:
            subprocess.run(['git', '-C', str(tree), 'apply', '--check', str(patches / (name + '.patch'))], check=True)

    links = [
        (root / 'apps/soil-node', package / 'app/soil-node'),
        (root / 'nuttx/boards/xtensa/esp32/esp32-devkitc/configs/soilnode', package / 'board/soil-node/configs/soilnode'),
        (root / 'nuttx/arch/xtensa/src/esp32/esp-hal-3rdparty', trees['esp-hal']),
    ]
    for dest, source in links:
        if dest.is_symlink():
            if dest.resolve() != source.resolve():
                parser.error('Conflicting link: ' + str(dest))
        elif dest.exists():
            parser.error('Refusing to replace existing directory: ' + str(dest))
    if args.check:
        print('CHECK OK; --apply will back up affected files, apply patches, create missing links and install .config.')
        return

    backup = root / ('soil_review_backup_' + datetime.now().strftime('%Y%m%d_%H%M%S_%f'))
    backup.mkdir()
    saved = []
    created_links = []
    config = root / 'nuttx/.config'
    config_existed = config.exists()
    if config_existed:
        shutil.copy2(config, backup / 'original.config')
    try:
        for item in metadata['files']:
            if not pending[item['tree']]:
                continue
            dest = trees[item['tree']] / item['path']
            source = backup / item['tree'] / item['path']
            existed = dest.exists()
            if existed:
                source.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(dest, source)
            saved.append((dest, source, existed))
        for name, tree in trees.items():
            if pending[name]:
                subprocess.run(['git', '-C', str(tree), 'apply', str(patches / (name + '.patch'))], check=True)
        for dest, source in links:
            if not dest.is_symlink():
                dest.parent.mkdir(parents=True, exist_ok=True)
                dest.symlink_to(source, target_is_directory=True)
                created_links.append(dest)
        shutil.copy2(package / 'board/soil-node/configs/soilnode/defconfig', config)
        for item in metadata['files']:
            if digest(trees[item['tree']] / item['path']) != item['after_sha256']:
                raise ValueError('Post-apply integrity failure: ' + item['path'])
    except Exception:
        for dest in reversed(created_links):
            dest.unlink()
        for dest, source, existed in reversed(saved):
            if existed:
                shutil.copy2(source, dest)
            else:
                dest.unlink(missing_ok=True)
        if config_existed:
            shutil.copy2(backup / 'original.config', config)
        else:
            config.unlink(missing_ok=True)
        raise
    print('APPLIED; backup: ' + str(backup))


if __name__ == '__main__':
    main()
