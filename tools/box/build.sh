#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
SDK_ROOT="$(cd "${1:?Usage: build.sh SDK_ROOT}" && pwd)"
PACKAGE_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
command -v xtensa-esp32s3-elf-gcc >/dev/null
python3 "$PACKAGE_ROOT/tools/box/prepare.py" "$SDK_ROOT" --apply
make -C "$SDK_ROOT/nuttx" -j"${JOBS:-8}" ${BOX_MAKE_OPTIONS:-}
python3 "$PACKAGE_ROOT/tools/check_boot_heap.py" "$SDK_ROOT/nuttx"
python3 - "$SDK_ROOT/nuttx/nuttx.bin" <<'PY'
from pathlib import Path
import hashlib,sys
p=Path(sys.argv[1]);assert p.stat().st_size<=4*1024*1024,'Preserved 4 MiB application limit exceeded'
print(p.stat().st_size,hashlib.sha256(p.read_bytes()).hexdigest(),p)
PY
