/****************************************************************************
 * apps/plant-companion/components/gesture_sensor/gesture_sensor.h
 *
 * ICM-42607-C IMU driver — public API.
 *
 * I2C address:  0x68 (AD0/SDO = GND)
 * WHO_AM_I:     0x60
 * Bus:          I2C0 (IO8=SDA, IO18=SCL)
 *
 * Register map mirrors IDF gesture_sensor.c — 100% reuse.
 ****************************************************************************/

#ifndef __APPS_PLANT_COMPANION_GESTURE_SENSOR_H
#define __APPS_PLANT_COMPANION_GESTURE_SENSOR_H

#include <stdint.h>

/**
 * @brief Initialize the ICM-42607-C IMU.
 *
 * Gets I2C0 bus handle, verifies WHO_AM_I (0x60), wakes accelerometer
 * and gyroscope. Must be called once before any read functions.
 *
 * @return 0 on success, negative errno on failure.
 */
int imu_init(void);

/**
 * @brief Read 3-axis accelerometer data.
 *
 * @param x  Output: X-axis acceleration (raw, 16-bit signed)
 * @param y  Output: Y-axis acceleration
 * @param z  Output: Z-axis acceleration
 * @return 0 on success, -ENODEV if not initialized, -EIO on I2C error.
 */
int imu_read_accel(int16_t *x, int16_t *y, int16_t *z);

/**
 * @brief Read 3-axis gyroscope data.
 *
 * @param x  Output: X-axis angular velocity (raw, 16-bit signed)
 * @param y  Output: Y-axis angular velocity
 * @param z  Output: Z-axis angular velocity
 * @return 0 on success, -ENODEV if not initialized, -EIO on I2C error.
 */
int imu_read_gyro(int16_t *x, int16_t *y, int16_t *z);

/**
 * @brief Read temperature from the IMU's internal sensor.
 *
 * @param temp_c  Output: Temperature in degrees Celsius
 * @return 0 on success, -ENODEV if not initialized, -EIO on I2C error.
 */
int imu_read_temp(float *temp_c);

#endif /* __APPS_PLANT_COMPANION_GESTURE_SENSOR_H */
