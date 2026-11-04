/* SPDX-License-Identifier: Apache-2.0 */
#ifndef ESP32S3_I2C_STRETCH_H
#define ESP32S3_I2C_STRETCH_H
#include <stdint.h>
/* The hardware counts 2^exponent I2C source clocks. Preserve the original
 * ten-cycle policy for ordinary devices. The PY32 firmware stretches SCL
 * while serving reads, so allow a bounded 20ms nominal wait for it only. */
static inline uint32_t esp32s3_i2c_timeout_exp(uint32_t half_cycle,
                                            uint32_t sclk,uint16_t addr)
{
  uint32_t value=32-__builtin_clz(5*half_cycle)+2;
  if(addr==0x6f || addr==0x71)
    {
      uint32_t cycles=(sclk+49)/50;
      uint32_t stretch=32-__builtin_clz(cycles-1);
      if(stretch>value)value=stretch;
    }
  return value;
}
#endif
