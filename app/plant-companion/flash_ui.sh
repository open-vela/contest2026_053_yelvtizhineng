#!/bin/bash
# ============================================================
# 植小伴 — ESP32-S3-BOX 固件烧录脚本（只烧 app，无需 erase）
# 用法: ./flash_ui.sh            # 烧录最新 nuttx.bin
# 首次/换分区布局时用: ./flash_ui.sh --full
# ============================================================
set -e

FW=/home/vboxuser/openvela/nuttx/nuttx.bin
PORT=${PORT:-/dev/ttyACM0}
BAUD=${BAUD:-921600}

if [ ! -f "$FW" ]; then
  echo "错误: 未找到 $FW，请先编译 ./build.sh vendor/espressif/boards/esp32s3/esp32s3-box/configs/openvela/"
  exit 1
fi

if [ ! -e "$PORT" ]; then
  echo "错误: 串口 $PORT 不存在。请确认板子已通过 USB 连接（lsusb 应见 303a:1001）"
  echo "提示: 若无权限，先 sudo chmod 666 $PORT"
  exit 1
fi

echo "=== 烧录 $FW ($(du -h "$FW" | cut -f1)) -> $PORT ==="

if [ "$1" = "--full" ]; then
  echo "--- 全量烧录（bootloader + 分区表 + app）---"
  esptool --chip esp32s3 --port "$PORT" --baud "$BAUD" \
    --before default-reset --after hard-reset erase_flash
  esptool --chip esp32s3 --port "$PORT" --baud "$BAUD" \
    --before default-reset --after hard-reset write-flash \
    --flash-mode dio --flash-freq 80m --flash-size 16MB \
    0x0      /home/vboxuser/openvela/nuttx/bootloader.bin \
    0x8000   /home/vboxuser/openvela/nuttx/partition-table.bin \
    0x10000  "$FW"
else
  echo "--- 只烧 app（迭代）---"
  esptool --chip esp32s3 --port "$PORT" --baud "$BAUD" \
    --before default-reset --after hard-reset write-flash \
    --flash-mode dio --flash-freq 80m --flash-size 16MB \
    0x10000  "$FW"
fi

echo "=== 烧录完成！板子将自动重启 ==="
echo "连接串口查看: minicom -D $PORT -b 115200"
