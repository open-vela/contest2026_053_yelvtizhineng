/****************************************************************************
 * apps/plant-companion/services/actuator_service.h
 *
 * 自动执行层（板卡侧）——服务器说"浇"，板卡真浇，浇完回执。
 *
 * 三步协议（接口签名见 server_bridge.h 末尾）：
 *   ① POST /api/v1/actuators/capabilities   声明板卡自带哪几路执行器
 *   ② GET  /api/v1/actuators/pending        取服务器排队的执行指令
 *   ③ POST /api/v1/actuators/jobs/{id}/ack  执行完回执（服务器写成长日记）
 *
 * 调用点：ui_app.c 的 ui_time_worker（与心跳同一条线程）。为什么不开新线程：
 * server_bridge 里的响应缓冲是 static，多线程并发调用会互相踩。
 *
 * ⚠️ 关于"到底真动了没有"：本板目前只焊了传感器，继电器/水泵还没接线。
 * 未接执行硬件（GPIO=-1）时本层按"干跑"执行：动作时序、回执照走，回执里
 * 写明 dry-run，服务器日记里能一眼看出没接硬件。等继电器接上，只需把
 * CONFIG_PLANT_ACTUATOR_GPIO_WATER 改成实际引脚号，代码不用动。
 ****************************************************************************/

#ifndef __PLANT_SERVICES_ACTUATOR_SERVICE_H
#define __PLANT_SERVICES_ACTUATOR_SERVICE_H

#ifdef __cplusplus
extern "C"
{
#endif

/* 开机初始化：把执行引脚配成输出并保持关断（没接线就什么都不做）。
 * 可以在配置服务器地址之前调用。返回 0。 */

int actuator_service_init(void);

/* 周期调用（每秒一次即可，内部自己节流）：
 *   ① 到点的动作关断并回执；
 *   ② 向服务器声明能力（成功一次就不再发；服务器换了会重发）；
 *   ③ 每 15s 取一次待执行指令并启动。
 * 未配置服务器时全部跳过，不产生任何网络等待。 */

void actuator_service_poll(void);

/* 本地自检：直接驱动一路（kind 见 actuator_service.c 的 g_caps）。
 * seconds <= 0 用默认 15s。返回 0 已启动；负 errno 失败。 */

int actuator_service_run(const char *kind, int seconds);

/* 最近一次执行摘要（排查用；静态字符串，始终非 NULL） */

const char *actuator_service_last(void);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_SERVICES_ACTUATOR_SERVICE_H */
