#!/bin/bash
export PYTHONPATH=/home/sdr/openvela/prebuilts/tools/python/dist-packages/kconfiglib
export PATH=/home/sdr/bin:/home/sdr/.local/bin:/home/sdr/openvela/prebuilts/tools/python/bin:/home/sdr/openvela/prebuilts/gcc/linux-x86_64/xtensa-esp32s3-elf/bin:$PATH
cd /home/sdr/plantb/nuttx || exit 1
APP=/home/sdr/plantb/apps/plant-companion
LOG=/home/sdr/plantb/build98.log
echo "=== build98 start $(date) ==="
find $APP -name "*.o" -delete
rm -f $APP/.built
make -j8 USE_NXTMPDIR_ESP_REPO_DIRECTLY=y NXTMPDIR=/home/sdr/openvela/nxtmpdir > $LOG 2>&1
echo "BUILD83-RC=$?"
echo "--- error 计数 ---"
grep -c "error:" $LOG
grep -n "error:" $LOG | head -30
echo "--- plant-companion warning ---"
grep -n "warning:" $LOG | grep -i "plant-companion" | head -20
ls -l /home/sdr/plantb/nuttx/nuttx.bin
sha256sum /home/sdr/plantb/nuttx/nuttx.bin
echo "=== build98 end $(date) ==="
