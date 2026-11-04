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

int battery_read_raw(int *raw);
int battery_read_mv(int *mv);
/* 电量百分比：3.3V = 100%，3.0V = 0%（区间内线性，见 .c 里的标定说明）。
 * 返回 0 成功；负 errno 失败（读不到 ADC 时不要拿 0 冒充电量）。 */
int battery_read_pct(int *pct);

#ifdef __cplusplus
}
#endif

#endif
