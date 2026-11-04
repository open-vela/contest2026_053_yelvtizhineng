#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Flash an application only after checking the BOX partition layout."""
import argparse
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

def check_partition(data,image_size):
    apps=[]
    for offset in range(0,len(data)-31,32):
        magic,kind,subtype,start,size,label,flags=struct.unpack_from('<HBBII16sI',data,offset)
        if magic!=0x50aa:break
        if kind==0:apps.append((start,size))
    if apps!=[(0x10000,0xb00000)]:raise ValueError('This image requires the validated single-application BOX layout; refusing an incompatible table: '+repr(apps))
    if not 0<image_size<=4*1024*1024:raise ValueError('Application outside the validated 4 MiB limit')
    return apps

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--image',type=Path,required=True);p.add_argument('--port');p.add_argument('--partition-table',type=Path);p.add_argument('--write',action='store_true');a=p.parse_args()
    size=a.image.stat().st_size
    if a.partition_table:check_partition(a.partition_table.read_bytes(),size)
    if not a.write:print('Plan: application only at 0x10000. Add --port PORT --write to read the actual table, validate it, and write.');return
    if not a.port:p.error('--port is required with --write')
    with tempfile.TemporaryDirectory(prefix='box_partition_') as d:
        table=Path(d)/'partitions.bin'
        base=[sys.executable,'-m','esptool','--chip','esp32s3','--port',a.port]
        subprocess.run(base+['read_flash','0x8000','0x1000',str(table)],check=True)
        check_partition(table.read_bytes(),size)
        subprocess.run(base+['--baud','460800','write_flash','0x10000',str(a.image)],check=True)
if __name__=='__main__':main()
