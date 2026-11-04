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

  if (ret != sizeof(sample)) return -EIO;

  /* esp32s3_adc.c au_receive() supplies calibrated pin millivolts. */
  *raw = sample.am_data;
  return 0;
}

/* Legacy name: this returns ADC-pin mV, not SAR counts. */
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

  /* The NuttX lower half already converts SAR counts to pin mV using
   * eFuse calibration. Only compensate the 1:2 board divider here.
   * The legacy raw*3300/4095 applied a second ADC conversion. */
  if (raw < 0) return -ERANGE;
  *mv = (int)((float)raw * BAT_DIVIDER);

  return 0;
}

/* A voltage reading is available, but the old 3.0-3.3V percent table was
 * based on the double-converted voltage and must never drive shutdown.
 * Current device: user reports nominal 3.7V, 2700mAh. An external voltage
 * cross-check / battery curve is still required before percent is trusted. */
int battery_read_pct(int *pct)
{
  if (!pct) return -EINVAL;
  *pct = -1;
  return -ENODATA;
}
