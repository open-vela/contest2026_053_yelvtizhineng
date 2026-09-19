/****************************************************************************
 * apps/plant-companion/services/sensor_service.h
 *
 * 传感器业务服务：Modbus 轮询 → 最新值缓存 + 阈值评语 + 7天历史环
 *
 * 设计（ARCHITECTURE.md §3）：
 *   - 独立 sensor_task 周期读 soil_sensor（1-5s）
 *   - 结果写入互斥保护的 sensor_view_t 缓存
 *   - 通过 on_update 回调通知 UI（回调在 ui_task 上下文执行）
 *   - 评语由本地阈值规则生成（离线可用，不依赖 AI）
 ****************************************************************************/

#ifndef __PLANT_SERVICES_SENSOR_SERVICE_H
#define __PLANT_SERVICES_SENSOR_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* 数据评语/状态：UI 用 🟢🟡🔴 与文字 */

#define SENSOR_STATUS_OK   0
#define SENSOR_STATUS_WARN 1
#define SENSOR_STATUS_BAD  2

#define SENSOR_VIEW_NOTE_LEN 16

/* 对外视图：UI 直接消费（避免暴露驱动结构体） */

struct sensor_view_s
{
  float   moisture;                 /* 土壤水分 % */
  float   temp;                     /* 温度 °C */
  float   ec;                       /* EC (μS/cm) */
  float   ph;                       /* pH */
  float   salt;                     /* 盐分（2026-09-11：8 参数上云） */
  float   nitrogen;                 /* 氮 (mg/kg) */
  float   phosphorus;               /* 磷 (mg/kg) */
  float   potassium;                /* 钾 (mg/kg) */
  int     moisture_status;          /* SENSOR_STATUS_* */
  int     temp_status;
  int     ec_status;
  int     light_status;             /* 占位：光照暂无硬件，0=低 1=中 2=高 */
  char    moisture_note[SENSOR_VIEW_NOTE_LEN];  /* 我很好/有点干… */
  char    temp_note[SENSOR_VIEW_NOTE_LEN];      /* 好舒服/有点热… */
  char    ec_note[SENSOR_VIEW_NOTE_LEN];        /* 营养够/缺营养… */
  bool    valid;                   /* 最近一次读取是否成功 */
  uint32_t last_read_ms;           /* 最近一次成功读取时间戳 */
};

/* 7天历史（每日一条，按天归档；用于④数据页趋势图） */

#define SENSOR_HISTORY_DAYS 7

struct sensor_history_s
{
  int      day[SENSOR_HISTORY_DAYS];   /* 距今天数偏移：0=今天 … 6=6天前 */
  float    moisture[SENSOR_HISTORY_DAYS];
  float    temp[SENSOR_HISTORY_DAYS];
  bool     valid[SENSOR_HISTORY_DAYS];
};

/**
 * 启动传感器轮询服务
 * @param interval_ms 轮询周期（如 3000）
 * @param on_update   数据更新回调（可为 NULL；在 ui_task 上下文调用）
 * @param user_data   透传
 * @return 0 成功；负值失败
 */
int sensor_service_start(int interval_ms,
                         void (*on_update)(void *user_data),
                         void *user_data);

/** 停止轮询服务 */
void sensor_service_stop(void);

/** 获取最新视图（线程安全：内部加锁拷贝） */
void sensor_service_get_view(struct sensor_view_s *out);

/** 获取7天历史（线程安全） */
void sensor_service_get_history(struct sensor_history_s *out);

/** 立即触发一次读取（UI 下拉刷新用） */
int sensor_service_read_now(void);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_SERVICES_SENSOR_SERVICE_H */
