/* StackChan body peripherals. Protocol derived from M5Stack's MIT HAL. */
#ifndef PLANT_STACKCHAN_BODY_H
#define PLANT_STACKCHAN_BODY_H

#include <stdbool.h>
#include "motion_limits.h"
struct stackchan_pose {int yaw,pitch,home_yaw,home_pitch;bool settled,ready;};
int stackchan_body_prepare(void);
int stackchan_body_get_pose(struct stackchan_pose *pose);
int stackchan_body_look_axis(int axis,int angle);
int stackchan_body_look(int yaw,int pitch);
int stackchan_body_look_raw(int yaw,int pitch);
void stackchan_body_focus(bool enabled);
int stackchan_body_start(void);
int stackchan_body_idle_power(void);
void stackchan_body_idle_wake(void);
void stackchan_body_stop(void);
int stackchan_body_take_touch(void);
int stackchan_body_command(int argc, char **argv);

#endif
