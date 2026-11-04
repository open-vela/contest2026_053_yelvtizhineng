/****************************************************************************
 * boards/xtensa/esp32s3/m5stack-cores3/src/esp32s3_board_touch_ft6336.c
 *
 * CoreS3 FT6336U touchscreen.
 ****************************************************************************/

#include <nuttx/config.h>

#include <debug.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/input/ft5x06.h>
#include <nuttx/irq.h>
#include <nuttx/signal.h>

#include "esp32s3_gpio.h"
#include "esp32s3_i2c.h"

#include "esp32s3-cores3.h"

static struct ft5x06_config_s g_touch_cfg;

#ifndef CONFIG_FT5X06_POLLMODE

static int touch_attach(FAR const struct ft5x06_config_s *config,
                        xcpt_t isr, FAR void *arg)
{
  int irq = ESP32S3_PIN2IRQ(TOUCHSCEEN_INT);
  int ret;

  UNUSED(config);

  esp32s3_gpioirqdisable(irq);
  ret = irq_attach(irq, isr, arg);
  if (ret < 0)
    {
      return ret;
    }

  esp32s3_configgpio(TOUCHSCEEN_INT, INPUT | PULLUP);
  esp32s3_gpioirqinitialize();
  return OK;
}

static void touch_enable(FAR const struct ft5x06_config_s *config,
                         bool enable)
{
  int irq = ESP32S3_PIN2IRQ(TOUCHSCEEN_INT);

  UNUSED(config);

  if (enable)
    {
      esp32s3_gpioirqenable(irq, FALLING);
    }
  else
    {
      esp32s3_gpioirqdisable(irq);
    }
}

static void touch_clear(FAR const struct ft5x06_config_s *config)
{
  UNUSED(config);
}

#endif

static void touch_wakeup(FAR const struct ft5x06_config_s *config)
{
  UNUSED(config);
}

static void touch_nreset(FAR const struct ft5x06_config_s *config, bool state)
{
  /* CoreS3 U10 P0_0 is TOUCH_RST. Reset only the touch controller;
   * BUS_OUT_EN is P0_1 and must not be pulsed by touch recovery. */

  UNUSED(config);
  board_cores3_ioexp_write(BOARD_IOEXP_TOUCH_RST, state);
}

int board_touchscreen_initialize(void)
{
  FAR struct i2c_master_s *i2c;
  int ret;

  ret = board_cores3_ioexp_init();
  if (ret < 0)
    {
      return ret;
    }

  ret = board_cores3_ioexp_set_dir(BOARD_IOEXP_TOUCH_RST, true);
  ret |= board_cores3_ioexp_set_dir(16 - 8 + 2, false);
  if (ret < 0)
    {
      return ret;
    }

  ret = board_cores3_ioexp_write(BOARD_IOEXP_TOUCH_RST, false);
  nxsig_usleep(10 * 1000);
  ret |= board_cores3_ioexp_write(BOARD_IOEXP_TOUCH_RST, true);
  nxsig_usleep(300 * 1000);
  if (ret < 0)
    {
      return ret;
    }

  i2c = esp32s3_i2cbus_initialize(TOUCHSCEEN_I2C);
  if (i2c == NULL)
    {
      return -ENODEV;
    }

  g_touch_cfg.address   = TOUCHSCEEN_ADDR;
  g_touch_cfg.frequency = TOUCHSCEEN_CLOCK;
#ifndef CONFIG_FT5X06_POLLMODE
  g_touch_cfg.attach    = touch_attach;
  g_touch_cfg.enable    = touch_enable;
  g_touch_cfg.clear     = touch_clear;
#endif
  g_touch_cfg.wakeup    = touch_wakeup;
  g_touch_cfg.nreset    = touch_nreset;

  ret = ft5x06_register(i2c, &g_touch_cfg, 0);
  if (ret < 0)
    {
      ierr("ERROR: ft5x06_register failed: %d\n", ret);
    }

  return ret;
}
