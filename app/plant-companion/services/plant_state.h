/****************************************************************************
 * apps/plant-companion/services/plant_state.h
 *
 * 植物档案服务：名字/品种/种植日期/天数/心情/健康分（UI_SPEC ①）
 * 数据来自 NVS/文件持久化 + 传感器 + AI 规则聚合
 ****************************************************************************/

#ifndef __PLANT_SERVICES_PLANT_STATE_H
#define __PLANT_SERVICES_PLANT_STATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define PLANT_NAME_LEN    16
#define PLANT_SPECIES_LEN 24

/* 心情枚举（对应 emoji 占位） */

#define PLANT_MOOD_HAPPY   0   /* 😊 */
#define PLANT_MOOD_TIRED   1   /* 🥱 */
#define PLANT_MOOD_SAD     2   /* 😢 */

struct plant_state_s
{
  char     name[PLANT_NAME_LEN];      /* 小绿绿 */
  char     species[PLANT_SPECIES_LEN];/* 绿萝 */
  uint32_t plant_date_epoch;          /* 种植日期（epoch 秒） */
  int      plant_day;                 /* 今天是养花第 N 天 */
  int      health_score;              /* 0-100（进度环） */
  int      mood;                      /* PLANT_MOOD_* */
  bool     need_water;                /* 浇水建议 */
};

/**
 * 初始化植物档案：优先从持久化读取，否则用默认值（小绿绿/绿萝/今天）
 * @return 0 成功
 */
int plant_state_init(void);

/** 获取当前档案（拷贝） */
void plant_state_get(struct plant_state_s *out);

/**
 * 用传感器数据更新健康分与心情（规则：水分/温度/EC 加权）
 * @param moisture 水分 %
 * @param temp 温度 °C
 * @param ec_ms EC (mS/cm)
 */
void plant_state_update_from_sensor(float moisture, float temp, float ec_ms);

/** 持久化当前档案（写入文件/KVDB） */
int plant_state_save(void);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_SERVICES_PLANT_STATE_H */
