/****************************************************************************
 * apps/plant-companion/services/record_service.h
 *
 * 记录服务：任务完成状态 + 日记事件的持久化（轻量版 3D-2）
 *
 * 存储：/data/plant_tasks.bin + /data/plant_diary.bin（littlefs）
 * 降级：/data 未挂载时静默仅内存（不影响 UI 演示）
 ****************************************************************************/

#ifndef __PLANT_SERVICES_RECORD_SERVICE_H
#define __PLANT_SERVICES_RECORD_SERVICE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define TASK_MAX     6
#define DIARY_MAX    8
#define STR_LEN      48

/* 任务项 */

struct record_task_s
{
  char  title[STR_LEN];
  char  time_slot[STR_LEN];
  bool  done;
};

/* 日记条目 */

struct record_diary_s
{
  uint32_t color;                 /* 圆点颜色 RGB */
  uint32_t day_offset;            /* 0=今天 … 6 */
  char  title[STR_LEN];
  char  detail[STR_LEN];
};

/**
 * 初始化记录服务：从 /data 加载（存在则读，否则用默认演示数据）
 * @return 0 成功
 */
int record_service_init(void);

/* --- 任务 --- */

int  record_task_get_count(void);
bool record_task_get(int idx, struct record_task_s *out);
void record_task_set_done(int idx, bool done);   /* 修改并立即持久化 */
void record_task_save(void);

/* --- 成就（3D-2：徽章系统） --- */

#define ACHIEVE_MAX 4

struct record_achieve_s
{
  const char *name;        /* 徽章名：一周养护 */
  bool unlocked;           /* 是否已解锁 */
};

/* 成就定义（静态表，解锁状态持久化） */

extern const struct record_achieve_s g_achieves[ACHIEVE_MAX];

/** 读取成就解锁状态 */
void record_achieve_get_all(bool unlocked[ACHIEVE_MAX]);

/** 尝试解锁指定成就；若刚解锁返回 true 并写入日记 */
bool record_achieve_unlock(int idx);

/** 已完成任务计数（跨重启持久化，用于成就判定） */
int  record_task_done_count(void);

/** 由任务服务调用：任务完成 → 计数 +1 并检查成就 */
void record_task_on_completed(void);

/* --- 日记 --- */

int  record_diary_get_count(void);
bool record_diary_get(int idx, struct record_diary_s *out);
void record_diary_add(const struct record_diary_s *entry);  /* 头部插入 */
void record_diary_save(void);

#ifdef __cplusplus
}
#endif

#endif /* __PLANT_SERVICES_RECORD_SERVICE_H */
