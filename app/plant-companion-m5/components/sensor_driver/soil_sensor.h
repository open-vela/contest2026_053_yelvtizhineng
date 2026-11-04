/****************************************************************************
 * soil_sensor.h — 云端土壤缓存适配器
 *
 * 云端拉取的8参数数据；不连接物理传感器。
 ****************************************************************************/

#ifndef __SOIL_SENSOR_H
#define __SOIL_SENSOR_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stddef.h>
#include <stdbool.h>

/* 使用 ai_chat.h 中的 soil_data_s 定义 */

#include "../../ai_module/ai_chat/ai_chat.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_PLANT_SOIL_SENSOR_DEV
#  define SOIL_SENSOR_DEV_DEFAULT  CONFIG_PLANT_SOIL_SENSOR_DEV
#else
#  define SOIL_SENSOR_DEV_DEFAULT  "/dev/ttyS0"
#endif

#ifdef CONFIG_PLANT_SOIL_SENSOR_BAUD
#  define SOIL_SENSOR_BAUD_DEFAULT CONFIG_PLANT_SOIL_SENSOR_BAUD
#else
#  define SOIL_SENSOR_BAUD_DEFAULT 9600
#endif
#define SOIL_SENSOR_ADDR_DEFAULT  0x02

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int soil_sensor_init(const char *dev, int baudrate);

/* Compatibility no-op: no physical soil UART exists. */

void soil_sensor_set_paused(bool paused);

/* Read the shared cloud cache; no direct network or UART I/O. */

int soil_sensor_read(struct soil_data_s *data, int verbose);
/* Local probing is disabled; returns -ENOTSUP. */

int soil_sensor_probe(int addr, int baudrate, uint8_t *resp, int resp_size);

/* Legacy frame API is disabled and always rejects local frames. */

int soil_sensor_frame_valid(const uint8_t *resp, int len, uint8_t addr);
void soil_sensor_deinit(void);
void soil_sensor_print_data(const struct soil_data_s *data);

#endif /* __SOIL_SENSOR_H */
