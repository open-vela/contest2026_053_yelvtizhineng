#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check or prepare an independent SDK for the BOX v239 source package."""
import argparse
from datetime import datetime
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

def digest(path):
    return hashlib.sha256(path.read_bytes().replace(b'\r\n',b'\n')).hexdigest() if path.is_file() else None

def within(path,root):
    try:path.relative_to(root);return True
    except ValueError:return False

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('sdk_root',type=Path)
    mode=p.add_mutually_exclusive_group(required=True)
    mode.add_argument('--check',action='store_true')
    mode.add_argument('--apply',action='store_true')
    args=p.parse_args();root=args.sdk_root.resolve();package=Path(__file__).resolve().parents[2]
    trees={name:root/name for name in ['nuttx','apps']}
    patches=package/'patches/box';meta=json.loads((patches/'baselines.json').read_text('utf-8'))
    for name,tree in trees.items():
        if not tree.is_dir():p.error('Missing SDK directory: '+str(tree))
    pending={}
    for name,tree in trees.items():
        states=set()
        for row in meta['files']:
            if row['tree']!=name:continue
            actual=digest(tree/row['path'])
            if actual==row['after_sha256']:states.add('applied')
            elif actual==row['before_sha256']:states.add('pending')
            else:p.error('Baseline mismatch; no files changed: '+name+'/'+row['path'])
        if len(states)!=1:p.error('Partially patched tree; reconcile first: '+name)
        pending[name]=states=={'pending'}
        if pending[name]:subprocess.run(['git','-C',str(tree),'apply','--check',str(patches/(name+'.patch'))],check=True)
        print(name+': '+next(iter(states)))
    links=[(root/'apps/plant-companion',package/'app/plant-companion'),
           (root/'nuttx/boards/xtensa/esp32s3/esp32s3-box/configs/contest_v239',package/'board/box/configs/contest_v239')]
    for dest,source in links:
        if not within(dest.resolve(strict=False),root) and not (dest.is_symlink() and dest.resolve()==source):
            p.error('Destination escapes SDK: '+str(dest))
        if dest.is_symlink() and dest.resolve()!=source:p.error('Conflicting symlink: '+str(dest))
    if args.check:
        print('CHECK OK; no files changed. --apply backs up replaced files/directories and installs source links and config.');return
    backup=root/('box_prepare_backup_'+datetime.now().strftime('%Y%m%d_%H%M%S_%f'));backup.mkdir()
    saved=[];created=[];moved=[];config=root/'nuttx/.config';config_existed=config.exists()
    if config_existed:shutil.copy2(config,backup/'original.config')
    try:
        for row in meta['files']:
            if not pending[row['tree']]:continue
            dest=trees[row['tree']]/row['path'];copy=backup/row['tree']/row['path'];existed=dest.exists()
            if existed:copy.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(dest,copy)
            saved.append((dest,copy,existed))
        for name,tree in trees.items():
            if pending[name]:subprocess.run(['git','-C',str(tree),'apply',str(patches/(name+'.patch'))],check=True)
        for dest,source in links:
            if dest.is_symlink():continue
            if dest.exists():
                dest.relative_to(root);copy=backup/'replaced'/dest.relative_to(root);copy.parent.mkdir(parents=True,exist_ok=True)
                shutil.move(str(dest),str(copy));moved.append((dest,copy))
            dest.parent.mkdir(parents=True,exist_ok=True);dest.symlink_to(source,target_is_directory=True);created.append(dest)
        shutil.copy2(package/'board/box/configs/contest_v239/defconfig',config)
        for row in meta['files']:
            if digest(trees[row['tree']]/row['path'])!=row['after_sha256']:raise ValueError('Post-apply hash mismatch: '+row['path'])
    except Exception:
        for dest in reversed(created):dest.unlink()
        for dest,copy in reversed(moved):shutil.move(str(copy),str(dest))
        for dest,copy,existed in reversed(saved):
            if existed:shutil.copy2(copy,dest)
            else:dest.unlink(missing_ok=True)
        if config_existed:shutil.copy2(backup/'original.config',config)
        else:config.unlink(missing_ok=True)
        raise
    print('APPLIED; backup: '+str(backup))
if __name__=='__main__':main()
