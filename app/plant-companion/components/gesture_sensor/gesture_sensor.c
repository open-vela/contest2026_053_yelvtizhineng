/****************************************************************************
 * apps/plant-companion/components/gesture_sensor/gesture_sensor.c
 *
 * ICM-42607-C IMU driver via NuttX I2C_TRANSFER (hal_i2c.h).
 * Register logic 100% from IDF gesture_sensor.c.
 *
 * I2C address:  0x68 (AD0/SDO = GND)
 * WHO_AM_I:     0x60 (ICM-42607-C variant)
 * Bus:          I2C0 (hardware, initialized by board bringup)
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdbool.h>
#include <errno.h>

#include <nuttx/i2c/i2c_master.h>

/* Forward declaration — esp32s3_i2cbus_initialize() lives in the kernel.
 * Apps can't include arch/xtensa/src/esp32s3/esp32s3_i2c.h directly,
 * but the symbol is available at link time. */
FAR struct i2c_master_s *esp32s3_i2cbus_initialize(int bus);

#include "../../hal/hal_i2c.h"
#include "../../hal/hal_delay.h"
#include "../../hal/hal_log.h"
#include "gesture_sensor.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define I2C_BUS         0
#define I2C_ADDR        0x68
#define I2C_FREQ        100000

/* ICM-42607-C register map (BANK0) */
#define REG_DEVICE_CONFIG   0x01
#define REG_PWR_MGMT0       0x1F
#define REG_GYRO_CONFIG0    0x20
#define REG_ACCEL_CONFIG0   0x21
#define REG_TEMP_DATA1      0x09
#define REG_ACCEL_DATA_X1   0x0B
#define REG_GYRO_DATA_X1    0x11
#define REG_WHO_AM_I        0x75

/****************************************************************************
 * Private Data
 ****************************************************************************/

static hal_i2c_dev_t g_i2c;
static bool g_initialized;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int imu_init(void)
{
  HAL_LOGI("imu", "Initializing ICM-42607-C (I2C%d, addr=0x%02X)",
           I2C_BUS, I2C_ADDR);

  /* Get I2C bus (assumes board bringup already initialized I2C0) */

  g_i2c = esp32s3_i2cbus_initialize(I2C_BUS);
  if (!g_i2c)
    {
      HAL_LOGE("imu", "Failed to get I2C%d bus", I2C_BUS);
      return -ENODEV;
    }

  /* Verify chip identity */

  uint8_t whoami = 0;
  int ret = hal_i2c_read_reg(g_i2c, I2C_ADDR, REG_WHO_AM_I, &whoami, I2C_FREQ);
  if (ret < 0)
    {
      HAL_LOGE("imu", "Failed to read WHO_AM_I — IMU not responding");
      return -ENODEV;
    }
  printf("imu: WHO_AM_I = 0x%02X (expected 0x60)\n", whoami);

  /* Wake up sensors (enable ACCEL + GYRO) */

  ret = hal_i2c_write_reg(g_i2c, I2C_ADDR, REG_PWR_MGMT0, 0x0F, I2C_FREQ);
  if (ret < 0)
    {
      HAL_LOGE("imu", "Failed to write PWR_MGMT0");
      return -EIO;
    }
  hal_delay_ms(50);

  uint8_t pwr = 0;
  hal_i2c_read_reg(g_i2c, I2C_ADDR, REG_PWR_MGMT0, &pwr, I2C_FREQ);
  printf("imu: PWR_MGMT0 = 0x%02X (after wake)\n", pwr);

  g_initialized = true;
  HAL_LOGI("imu", "ICM-42607-C ready");
  return 0;
}

int imu_read_accel(int16_t *x, int16_t *y, int16_t *z)
{
  if (!g_initialized) return -ENODEV;

  uint8_t buf[6];
  int ret = hal_i2c_read_burst(g_i2c, I2C_ADDR, REG_ACCEL_DATA_X1, buf, 6, I2C_FREQ);
  if (ret < 0) return -EIO;

  *x = (int16_t)((buf[0] << 8) | buf[1]);
  *y = (int16_t)((buf[2] << 8) | buf[3]);
  *z = (int16_t)((buf[4] << 8) | buf[5]);
  return 0;
}

int imu_read_gyro(int16_t *x, int16_t *y, int16_t *z)
{
  if (!g_initialized) return -ENODEV;

  uint8_t buf[6];
  int ret = hal_i2c_read_burst(g_i2c, I2C_ADDR, REG_GYRO_DATA_X1, buf, 6, I2C_FREQ);
  if (ret < 0) return -EIO;

  *x = (int16_t)((buf[0] << 8) | buf[1]);
  *y = (int16_t)((buf[2] << 8) | buf[3]);
  *z = (int16_t)((buf[4] << 8) | buf[5]);
  return 0;
}

int imu_read_temp(float *temp_c)
{
  if (!g_initialized) return -ENODEV;

  uint8_t buf[2];
  int ret = hal_i2c_read_burst(g_i2c, I2C_ADDR, REG_TEMP_DATA1, buf, 2, I2C_FREQ);
  if (ret < 0) return -EIO;

  int16_t raw = (int16_t)((buf[0] << 8) | buf[1]);
  *temp_c = ((float)raw / 132.48f) + 25.0f;
  return 0;
}
