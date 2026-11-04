#ifndef BOX_POWER_SERVICE_H
#define BOX_POWER_SERVICE_H
#include "power_policy.h"
void box_power_activity(void); /* Thread-safe, no LVGL or hardware calls. */
void box_power_request(enum box_power_request request);
int box_power_poll(unsigned busy_mask); /* UI owner only; requested backlight percent. */
bool box_power_standby(void);
unsigned box_power_sensor_interval(unsigned normal_ms);
void box_power_applied(int percent,int result);
void box_power_status(void);
#endif
