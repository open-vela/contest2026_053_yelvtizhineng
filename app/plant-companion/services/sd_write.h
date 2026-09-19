/****************************************************************************
 * apps/plant-companion/services/sd_write.h
 *
 * ⚠️ 2026-09-10 根因修复（"没有语音播放 / 界面卡死 / 点了没反应"）：
 *
 * 本板 SD(SDMMC) 写盘会把 FILE* 的用户缓冲直接交给 DMA，驱动断言要求
 * 该缓冲 4 字节对齐：
 *   arch/xtensa/src/esp32s3/esp32s3_sdmmc.c: esp32s3_dmasendsetup()
 *   DEBUGASSERT(buffer != NULL && buflen > 0 && ((uint32_t)buffer & 3) == 0);
 *
 * 只要传的是"基址 + 任意偏移"的指针（WAV 头 / BMP 逐行 / HTTP 响应体
 * 偏移 / 相机帧缓冲），就可能对不齐 → 断言 → 板子静默挂死：
 * TTS 不播放、界面随后也不响应。项目里已踩过两次（plant cap 截屏、
 * 语音 TTS 下载）。
 *
 * 约定：所有写 SD 的 fwrite 一律改走 sd_write_aligned()。
 ****************************************************************************/

#ifndef __PLANT_SERVICES_SD_WRITE_H
#define __PLANT_SERVICES_SD_WRITE_H

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* 先搬到 4 字节对齐的静态缓冲再写。返回 0 成功，-1 失败。
 * 每个编译单元各自持有 1KB 缓冲（函数内 static，无外部符号）。 */

static inline int sd_write_aligned(FILE *fp, const void *data, size_t len)
{
  /* ⚠️ 2026-09-16 缓冲 1KB → 4KB：实测本板 SD 写速率随块大小变化
   * 很大（4KB 块 168KB/s、32KB 块 320KB/s），1KB 小块每次都要走一遍
   * FAT + SDMMC 驱动开销。语音块 3.2KB → 现在 1 次 fwrite（原来 4 次）。 */
  static uint8_t s_buf[4096] __attribute__((aligned(4)));
  const uint8_t *src = (const uint8_t *)data;
  size_t done = 0;

  if (fp == NULL || (data == NULL && len > 0))
    {
      return -1;
    }

  while (done < len)
    {
      size_t n = len - done;

      if (n > sizeof(s_buf))
        {
          n = sizeof(s_buf);
        }

      memcpy(s_buf, src + done, n);
      if (fwrite(s_buf, 1, n, fp) != n)
        {
          return -1;
        }

      done += n;
    }

  return 0;
}

#endif /* __PLANT_SERVICES_SD_WRITE_H */
