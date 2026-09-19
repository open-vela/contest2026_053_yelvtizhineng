/****************************************************************************
 * soil_sensor.h — 土壤传感器驱动
 *
 * RS485 Modbus RTU 通信，8参数土壤传感器
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

#define SOIL_SENSOR_DEV_DEFAULT   "/dev/ttyS0"
#define SOIL_SENSOR_BAUD_DEFAULT  9600
#define SOIL_SENSOR_ADDR_DEFAULT  0x02

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int soil_sensor_init(const char *dev, int baudrate);

/* 摄像头占用 IO42/40 期间挂起土壤收发（进拍照页置 true 并停轮询，
 * 退出拍照、收回引脚后置 false 再恢复轮询）；挂起时 read 直接返回失败。 */

void soil_sensor_set_paused(bool paused);

/* 读一次 Modbus 8 寄存器；verbose=1 打印诊断（open/write/rx 原始字节/短帧） */

int soil_sensor_read(struct soil_data_s *data, int verbose);
/* 排查用：按指定从站地址/波特率发一帧查询，返回收到字节数（-1=无应答）。
 * plant soil scan 用（接线 vs 地址/波特率 的区分手段）。 */

int soil_sensor_probe(int addr, int baudrate, uint8_t *resp, int resp_size);

/* Modbus 整帧校验：地址+功能码 0x03+字节数 16+CRC16，任一不符返回 0。
 * plant soil scan 用它区分"真设备应答"和"RX 悬空噪声"。 */

int soil_sensor_frame_valid(const uint8_t *resp, int len, uint8_t addr);
void soil_sensor_deinit(void);
void soil_sensor_print_data(const struct soil_data_s *data);

#endif /* __SOIL_SENSOR_H */
