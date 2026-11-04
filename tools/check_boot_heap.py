"""Check the actual flat-build bootstrap heap AFTER its IMEM reservation.

The preceding guard checked the combined arena, which allowed a 128KB IMEM
reservation to leave too little memory for mm_heap_s and early boot tasks.
"""
import json,re,subprocess,sys
from pathlib import Path
nuttx=Path(sys.argv[1]).resolve()
nm=sys.argv[2] if len(sys.argv)>2 else 'xtensa-esp32s3-elf-nm'
config=(nuttx/'.config').read_text()
assert 'CONFIG_BUILD_FLAT=y' in config and 'CONFIG_MM_KERNEL_HEAP=y' not in config,'Unsupported heap layout; audit before building'
allocation=(nuttx/'arch/xtensa/src/esp32s3/esp32s3_allocateheap.c').read_text()
top=int(re.search(r'#define\s+ESP32S3_HEAP_TOP\s+(0x[0-9a-fA-F]+)',allocation)[1],16)
reserve=0
if 'CONFIG_XTENSA_IMEM_USE_SEPARATE_HEAP=y' in config:
    reserve=int(re.search(r'^CONFIG_XTENSA_IMEM_REGION_SIZE=(\S+)',config,re.M)[1],0)
symbols=subprocess.check_output([nm,str(nuttx/'nuttx')],text=True)
sheap=int(re.search(r'^([0-9a-fA-F]+)\s+\w\s+_sheap$',symbols,re.M)[1],16)
available=top-sheap-reserve
minimum=9*1024
result={'pass':available>=minimum,'combined_internal_arena_bytes':top-sheap,'imem_reserved_bytes':reserve,
        'bootstrap_heap_bytes':available,'minimum_bootstrap_heap_bytes':minimum,'sheap':hex(sheap),'heap_top':hex(top)}
print(json.dumps(result))
raise SystemExit(0 if result['pass'] else 2)
