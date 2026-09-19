/****************************************************************************
 * apps/plant-companion/services/ota_service.h
 *
 * OTA 升级 UI 服务（阶段 A）：把阻塞的 ota_check_version / ota_check_update
 * 放进后台 worker 线程，结果写共享结构，ui_task 定时器轮询上屏。
 *
 * 线程模型（同 voice_service）：
 *   - worker 只写共享结构 + 标志，不碰 LVGL 对象
 *   - screen_ota 在 ui_task 里定时器轮询 ota_service_get()
 *   - worker 栈用静态缓冲（WiFi 后堆 ~17KB，不能吃堆）
 ****************************************************************************/

#ifndef __PLANT_SERVICES_OTA_SERVICE_H
#define __PLANT_SERVICES_OTA_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* UI 可显示的状态（screen_ota 按此切界面） */

enum ota_ui_state_e
{
  OTA_UI_IDLE = 0,       /* 无任务 */
  OTA_UI_CHECKING,       /* 检查更新中 */
  OTA_UI_AVAILABLE,      /* 发现新版本（new_* 有效） */
  OTA_UI_UP_TO_DATE,     /* 已是最新 */
  OTA_UI_ERROR,          /* 检查/升级失败（error 为原因文本） */
  OTA_UI_DOWNLOADING,    /* 升级下载中（downloaded/total 有效） */
  OTA_UI_REBOOTING       /* 下载完成，校验/写 otadata/即将重启 */
};

/* 共享结果（worker 写，ui_task 读） */

struct ota_ui_result_s
{
  int      state;                  /* enum ota_ui_state_e */
  int      current_major;          /* 当前固件版本 */
  int      current_minor;
  int      current_patch;
  int      new_major;              /* 服务器新版本 */
  int      new_minor;
  int      new_patch;
  char     error[64];              /* 失败原因（中文，供 UI 直接显示） */
  uint32_t downloaded;             /* 下载进度 */
  uint32_t total;
  bool     busy;                   /* worker 在跑（禁止重复点击） */
};

/** 初始化：抓当前固件版本。ui_app_start 调用一次。 */
void ota_service_init(void);

/** 起 worker 只检查版本（查询服务器，不下载不切换）。
 *  返回 0 成功；-EBUSY 已有 worker 在跑。 */
int ota_service_check(void);

/** 起 worker 执行完整升级（下载→SHA256→写 otadata→重启）。
 *  返回 0 成功；-EBUSY 已有 worker 在跑。 */
int ota_service_upgrade(void);

/** 取共享结果（UI 定时器轮询）。 */
void ota_service_get(struct ota_ui_result_s *out);

/** 页面销毁时调用：置取消标志。检查任务可中断；下载任务无法打断
 *  （ota_check_update 内部无取消点），仅丢弃结果。 */
void ota_service_abort(void);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_SERVICES_OTA_SERVICE_H */
