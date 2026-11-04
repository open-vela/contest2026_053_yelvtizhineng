/****************************************************************************
 * apps/plant-companion/components/system_monitor/battery_monitor.h
 *
 * Battery voltage monitor via ADC1 IO1.
 * Divider: R82(100K) + R83(100K) = 1/2.
 ****************************************************************************/

#ifndef __APPS_PLANT_COMPANION_BATTERY_MONITOR_H
#define __APPS_PLANT_COMPANION_BATTERY_MONITOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Legacy name: calibrated ADC-pin millivolts from the NuttX lower half. */
int battery_read_raw(int *raw);
/* Estimated pack mV after the board's 1:2 divider. Not externally calibrated. */
int battery_read_mv(int *mv);
/* Percent is unknown until a valid device-specific voltage curve exists:
 * sets *pct=-1 and returns -ENODATA. Never use this sentinel as low battery. */
int battery_read_pct(int *pct);

#ifdef __cplusplus
}
#endif

#endif
