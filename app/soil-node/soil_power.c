#include <nuttx/config.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include <netutils/netlib.h>

#include "soil_app.h"
#include "soil_power.h"

#ifdef CONFIG_ARCH_CHIP_ESP32
extern void esp32_pmsleep(uint64_t time_in_us);
#endif

int soil_power_deep_sleep_us(uint64_t duration_us)
{
#ifdef CONFIG_ARCH_CHIP_ESP32
  netlib_ifdown("wlan0");
  netlib_ifdown("wlan1");
  soil_log("DEEP_SLEEP_US=%llu\n", (unsigned long long)duration_us);
  fflush(stdout);
  esp32_pmsleep(duration_us);
  return -EIO;
#else
  return -ENOTSUP;
#endif
}
