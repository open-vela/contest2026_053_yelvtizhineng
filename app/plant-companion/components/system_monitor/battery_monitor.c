/****************************************************************************
 * apps/plant-companion/components/system_monitor/battery_monitor.c
 *
 * Reads /dev/adc0 (IO1 = ADC1_CH0) via NuttX ADC driver.
 * Voltage divider 1/2 compensated in software.
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <nuttx/analog/adc.h>
#include <nuttx/analog/ioctl.h>

#include "../../hal/hal_log.h"
#include "battery_monitor.h"

#define ADC_DEV      "/dev/adc0"
#define BAT_DIVIDER  2.0f

static int adc_read_raw(int *raw)
{
  int fd = open(ADC_DEV, O_RDONLY);
  if (fd < 0)
    {
      /* 保留报错（但限频）：/dev/adc0 未注册 = 电池功能坏，必须暴露问题
       * 而不是静默掩盖。ui_refresh 每 500ms 调一次，这里用 static 计数
       * 只打印首次 + 每 100 次一次，避免刷屏淹没串口。 */
      static unsigned s_fail_cnt;

      if (s_fail_cnt++ == 0)
        {
          HAL_LOGE("batt", "open %s failed: %d (cnt=%u)",
                   ADC_DEV, errno, s_fail_cnt);
        }

      return -errno;
    }

  int ret = ioctl(fd, ANIOC_TRIGGER, 0);
  if (ret < 0)
    {
      HAL_LOGE("batt", "ioctl ANIOC_TRIGGER failed: %d", errno);
      close(fd);
      return -errno;
    }

  struct adc_msg_s sample;
  ret = read(fd, &sample, sizeof(sample));
  close(fd);

  if (ret < 0)
    {
      HAL_LOGE("batt", "read failed: %d", errno);
      return -errno;
    }

  *raw = sample.am_data;
  return 0;
}

int battery_read_raw(int *raw)
{
  if (!raw) return -EINVAL;
  return adc_read_raw(raw);
}

int battery_read_mv(int *mv)
{
  if (!mv) return -EINVAL;

  int raw;
  int ret = adc_read_raw(&raw);
  if (ret < 0) return ret;

  /* ADC reference = 3.3V, 12-bit = 4095 steps, then ×2 for divider */
  *mv = (int)((float)raw * 3300.0f / 4095.0f * BAT_DIVIDER);
  return 0;
}

/* 2026-09-11 电量标定（用户实测定案）。
 *
 * 现象：整机跑在电池上，满电时界面只显示 25%。
 *
 * 原由：旧映射按"3.0V = 0%，4.1V = 100%"（4.2V 满充锂电的口径）线性折算，
 * 而本机电池满电时 ADC 折算出来约 3.3V —— 落在旧区间的下段，自然只有二十几个点。
 *
 * 定案：本机电池满电压按 3.3V 计 → BAT_MV_FULL = 3300 即 100%；
 * 低端沿用 3.0V = 0%。两者之间线性折算。
 *
 * 注意：只改这段"电压 → 百分比"的映射，不动上面 raw → mV 的 ADC 换算
 * （分压是 R82/R83 = 100K+100K 的 1/2，换算本身是对的）。
 * 以后要调表，只改这两个常量。 */

#define BAT_MV_FULL   3300
#define BAT_MV_EMPTY  3000

int battery_read_pct(int *pct)
{
  if (!pct) return -EINVAL;

  int mv;
  int ret = battery_read_mv(&mv);
  if (ret < 0) return ret;

  if (mv >= BAT_MV_FULL)
    {
      *pct = 100;
    }
  else if (mv <= BAT_MV_EMPTY)
    {
      *pct = 0;
    }
  else
    {
      *pct = (int)((float)(mv - BAT_MV_EMPTY) * 100.0f /
                   (float)(BAT_MV_FULL - BAT_MV_EMPTY));
    }

  return 0;
}
