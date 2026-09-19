# -*- coding: utf-8 -*-
"""发布固件（OTA）：把 nuttx.bin 放进 ota/firmware/ 并生成 ota/version.json。

用法：
    python publish_firmware.py <nuttx.bin 路径> <版本号，如 1.2.3> [硬件ID，默认 1]

板卡会向 plant.cfg 里配置的 server_host/server_port 请求：
    GET /version.json       → 本脚本生成的版本信息
    GET /firmware/<文件>    → 本脚本放进去的固件
所以发布完不用重启服务。
"""
import hashlib
import json
import os
import shutil
import sys

BASE = os.path.dirname(os.path.abspath(__file__))


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, ver = sys.argv[1], sys.argv[2]
    hw = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    if not os.path.isfile(src):
        print("找不到固件文件:", src)
        return 1

    fw_dir = os.path.join(BASE, "ota", "firmware")
    os.makedirs(fw_dir, exist_ok=True)
    name = "plant-companion-%s.bin" % ver
    dst = os.path.join(fw_dir, name)
    shutil.copyfile(src, dst)

    with open(dst, "rb") as f:
        data = f.read()
    info = {
        "version": ver,
        "size": len(data),
        "hardware_id": hw,
        "url": "/firmware/" + name,
        "sha256": hashlib.sha256(data).hexdigest(),
    }
    with open(os.path.join(BASE, "ota", "version.json"), "w", encoding="utf-8") as f:
        json.dump(info, f, ensure_ascii=False, indent=2)
        f.write("\n")

    print("已发布:", name)
    print(json.dumps(info, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())