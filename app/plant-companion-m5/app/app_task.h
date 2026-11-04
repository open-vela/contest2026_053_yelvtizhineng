/****************************************************************************
 * app_task.h — 应用层多任务调度
 *
 * Phase 18: pthread_create all tasks, state machine dispatch
 ****************************************************************************/

#pragma once

#include <stdint.h>

/* Task priorities (higher = more urgent) */
#define TASK_PRIO_UI_RENDER      5
#define TASK_PRIO_CLOUD_COMM     4
#define TASK_PRIO_CAMERA_CAPTURE 3
#define TASK_PRIO_SENSOR_POLL    3
#define TASK_PRIO_WIFI_MONITOR   2
#define TASK_PRIO_SYSTEM_MONITOR 1

int app_tasks_start(void);

#ifdef __cplusplus
extern "C" {
#endif
#ifdef __cplusplus
}
#endif
