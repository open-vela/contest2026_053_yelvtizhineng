/****************************************************************************
 * boards/xtensa/esp32s3/esp32s3-box/src/esp32s3_board_imu.c
 *
 * ICM-42607-C IMU driver via NuttX I2C0 hardware (IO8=SDA, IO18=SCL).
 * Register logic from IDF gesture_sensor.c — 100% register map reuse.
 *
 * I2C address:  0x68 (AD0/SDO = GND)
 * WHO_AM_I:     0x60 (ICM-42607-C variant)
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdbool.h>
#include <errno.h>

#include <nuttx/arch.h>
#include <nuttx/signal.h>
#include <nuttx/i2c/i2c_master.h>

#include "esp32s3_i2c.h"
#include "esp32s3-box.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define I2C_BUS         0           /* I2C0: SDA=IO8, SCL=IO18 */
#define I2C_ADDR        0x68
#define I2C_FREQ        100000      /* 100 kHz standard mode */

/* ICM-42607-C register map (BANK0, from datasheet) */
#define REG_DEVICE_CONFIG   0x01
#define REG_PWR_MGMT0       0x1F
#define REG_GYRO_CONFIG0    0x20
#define REG_ACCEL_CONFIG0   0x21
#define REG_TEMP_CONFIG0    0x22
#define REG_TEMP_DATA1      0x09
#define REG_ACCEL_DATA_X1   0x0B
#define REG_GYRO_DATA_X1    0x11
#define REG_WHO_AM_I        0x75
#define REG_BLK_SEL_R       0x7C

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct i2c_master_s *g_i2c;
static bool g_imu_initialized;

/****************************************************************************
 * Private Functions — I2C register R/W via NuttX I2C API
 ****************************************************************************/

static bool reg_write(uint8_t reg, uint8_t val)
{
  uint8_t buf[] = { reg, val };
  struct i2c_msg_s msg =
  {
    .frequency = I2C_FREQ,
    .addr      = I2C_ADDR,
    .flags     = 0,
    .buffer    = buf,
    .length    = sizeof(buf)
  };

  int ret = I2C_TRANSFER(g_i2c, &msg, 1);
  if (ret < 0)
    {
      printf("imu: I2C write reg 0x%02X failed: %d\n", reg, ret);
      return false;
    }
  return true;
}

static bool reg_read(uint8_t reg, uint8_t *val)
{
  uint8_t reg_buf[] = { reg };
  struct i2c_msg_s msgv[2] =
  {
    {
      .frequency = I2C_FREQ,
      .addr      = I2C_ADDR,
      .flags     = 0,
      .buffer    = reg_buf,
      .length    = sizeof(reg_buf)
    },
    {
      .frequency = I2C_FREQ,
      .addr      = I2C_ADDR,
      .flags     = I2C_M_READ,
      .buffer    = val,
      .length    = 1
    }
  };

  int ret = I2C_TRANSFER(g_i2c, msgv, 2);
  if (ret < 0)
    {
      printf("imu: I2C read reg 0x%02X failed: %d\n", reg, ret);
      return false;
    }
  return true;
}

static bool reg_read_burst(uint8_t start_reg, uint8_t *buf, int len)
{
  uint8_t reg_buf[] = { start_reg };
  struct i2c_msg_s msgv[2] =
  {
    {
      .frequency = I2C_FREQ,
      .addr      = I2C_ADDR,
      .flags     = 0,
      .buffer    = reg_buf,
      .length    = sizeof(reg_buf)
    },
    {
      .frequency = I2C_FREQ,
      .addr      = I2C_ADDR,
      .flags     = I2C_M_READ,
      .buffer    = buf,
      .length    = len
    }
  };

  int ret = I2C_TRANSFER(g_i2c, msgv, 2);
  if (ret < 0)
    {
      printf("imu: I2C burst read 0x%02X len=%d failed: %d\n",
             start_reg, len, ret);
      return false;
    }
  return true;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int board_imu_initialize(void)
{
  printf("imu: Initializing ICM-42607-C (I2C%d, addr=0x%02X)\n",
         I2C_BUS, I2C_ADDR);

  /* Step 1: Get I2C bus (already initialized by touchscreen driver) */

  g_i2c = esp32s3_i2cbus_initialize(I2C_BUS);
  if (!g_i2c)
    {
      printf("imu: ERROR: Failed to get I2C%d bus\n", I2C_BUS);
      return -ENODEV;
    }

  /* Step 2: Verify chip identity */

  uint8_t whoami = 0;
  if (!reg_read(REG_WHO_AM_I, &whoami))
    {
      printf("imu: ERROR: Failed to read WHO_AM_I — IMU not responding\n");
      return -ENODEV;
    }
  printf("imu: WHO_AM_I = 0x%02X (expected 0x60)\n", whoami);

  /* Step 3: Read initial config */

  uint8_t pwr = 0, gyro_cfg = 0, accel_cfg = 0;
  reg_read(REG_PWR_MGMT0, &pwr);
  reg_read(REG_GYRO_CONFIG0, &gyro_cfg);
  reg_read(REG_ACCEL_CONFIG0, &accel_cfg);
  printf("imu: PWR=0x%02X GYRO=0x%02X ACCEL=0x%02X\n",
         pwr, gyro_cfg, accel_cfg);

  /* Step 4: Wake up sensors (enable ACCEL + GYRO) */

  printf("imu: Writing PWR_MGMT0[0x1F] = 0x0F...\n");
  if (!reg_write(REG_PWR_MGMT0, 0x0F))
    {
      printf("imu: ERROR: Failed to write PWR_MGMT0\n");
      return -EIO;
    }
  nxsig_usleep(50 * 1000);
  reg_read(REG_PWR_MGMT0, &pwr);
  printf("imu: PWR_MGMT0 = 0x%02X (after wake)\n", pwr);

  g_imu_initialized = true;
  printf("imu: ICM-42607-C ready\n");
  return OK;
}

int board_imu_read_accel(int16_t *x, int16_t *y, int16_t *z)
{
  if (!g_imu_initialized) return -ENODEV;

  uint8_t buf[6];
  if (!reg_read_burst(REG_ACCEL_DATA_X1, buf, 6)) return -EIO;

  *x = (int16_t)((buf[0] << 8) | buf[1]);
  *y = (int16_t)((buf[2] << 8) | buf[3]);
  *z = (int16_t)((buf[4] << 8) | buf[5]);
  return OK;
}

int board_imu_read_gyro(int16_t *x, int16_t *y, int16_t *z)
{
  if (!g_imu_initialized) return -ENODEV;

  uint8_t buf[6];
  if (!reg_read_burst(REG_GYRO_DATA_X1, buf, 6)) return -EIO;

  *x = (int16_t)((buf[0] << 8) | buf[1]);
  *y = (int16_t)((buf[2] << 8) | buf[3]);
  *z = (int16_t)((buf[4] << 8) | buf[5]);
  return OK;
}

int board_imu_read_temp(float *temp_c)
{
  if (!g_imu_initialized) return -ENODEV;

  uint8_t buf[2];
  if (!reg_read_burst(REG_TEMP_DATA1, buf, 2)) return -EIO;

  int16_t raw = (int16_t)((buf[0] << 8) | buf[1]);
  *temp_c = ((float)raw / 132.48f) + 25.0f;
  return OK;
}
