/****************************************************************************
 * boards/xtensa/esp32s3/m5stack-cores3/src/esp32s3_board_camera.c
 *
 * CoreS3 GC0308 camera power and SCCB access.  GC0308 uses an 8-bit
 * register address; the CoreS3 camera module generates its own XCLK.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>

#include <nuttx/arch.h>
#include <nuttx/i2c/i2c_master.h>
#include <nuttx/signal.h>

#include "esp32s3_i2c.h"

#include "esp32s3-cores3.h"

#define GC0308_PID_REG 0x00
#define GC0308_PID     0x9b
#define GC0308_SCCB_FREQ (100 * 1000)

static int camera_sccb_write(uint8_t reg, uint8_t val)
{
  FAR struct i2c_master_s *i2c;
  struct i2c_config_s cfg = {
    .frequency = GC0308_SCCB_FREQ,
    .address   = BOARD_CAMERA_SCCB_ADDR,
    .addrlen   = 7,
  };
  uint8_t buf[2] = {reg, val};
  int ret;

  i2c = esp32s3_i2cbus_initialize(BOARD_I2C_BUS);
  if (i2c == NULL)
    {
      return -ENODEV;
    }

  ret = i2c_write(i2c, &cfg, buf, sizeof(buf));
  return ret < 0 ? ret : OK;
}

static int camera_sccb_read(uint8_t reg, FAR uint8_t *val)
{
  FAR struct i2c_master_s *i2c;
  struct i2c_config_s cfg = {
    .frequency = GC0308_SCCB_FREQ,
    .address   = BOARD_CAMERA_SCCB_ADDR,
    .addrlen   = 7,
  };
  int ret;

  if (val == NULL)
    {
      return -EINVAL;
    }

  i2c = esp32s3_i2cbus_initialize(BOARD_I2C_BUS);
  if (i2c == NULL)
    {
      return -ENODEV;
    }

  ret = i2c_write(i2c, &cfg, &reg, 1);
  if (ret < 0)
    {
      return ret;
    }

  ret = i2c_read(i2c, &cfg, val, 1);
  return ret < 0 ? ret : OK;
}

int board_camera_sccb_probe(FAR uint16_t *pid)
{
  uint8_t id = 0;
  int ret;

  if (pid == NULL)
    {
      return -EINVAL;
    }

  ret = board_cores3_camera_power(true);
  if (ret < 0)
    {
      return ret;
    }

  nxsig_usleep(50 * 1000);
  ret = camera_sccb_read(GC0308_PID_REG, &id);
  if (ret < 0)
    {
      return ret;
    }

  *pid = id;
  return id == GC0308_PID ? OK : -ENODEV;
}

int board_camera_sccb_write(uint16_t reg, uint8_t val)
{
  if (reg > 0xff)
    {
      return -EINVAL;
    }

  return camera_sccb_write((uint8_t)reg, val);
}

int board_camera_sccb_read(uint16_t reg, FAR uint8_t *val)
{
  if (reg > 0xff)
    {
      return -EINVAL;
    }

  return camera_sccb_read((uint8_t)reg, val);
}
