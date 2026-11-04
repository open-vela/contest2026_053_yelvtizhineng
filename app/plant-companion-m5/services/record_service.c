/****************************************************************************
 * apps/plant-companion/services/record_service.c
 *
 * 记录服务：任务完成状态 + 日记事件持久化（轻量版）
 *
 * 存储格式：定长结构数组序列化到 SD 卡（/mnt/sd，开机自动挂载）。
 * SD 未挂载时 fopen 失败 → 静默仅内存（可降级演示）。
 * 注：原写 /data littlefs（当前固件无 littlefs，fopen 一直失败 → 记录从未落盘）；
 * 用户要求任务/日记存 SD 侧（见 CLAUDE_SYSTEM.md BUG-3）。
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>

#include "record_service.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct record_task_s  s_tasks[TASK_MAX];
static int s_task_count;

static struct record_diary_s s_diary[DIARY_MAX];
static int s_diary_count;

static bool g_inited;

/* 成就状态 + 任务完成计数（持久化） */

static bool s_achieve_unlocked[ACHIEVE_MAX];
static int  s_task_done_count;

#define TASK_PATH    "/mnt/sd/plant_tasks.bin"
#define DIARY_PATH   "/mnt/sd/plant_diary.bin"
#define ACHIEVE_PATH "/mnt/sd/plant_achieve.bin"

/* 成就静态表（名称由 UI 直接引用） */

const struct record_achieve_s g_achieves[ACHIEVE_MAX] =
{
  { "一周养护",   false },
  { "小园丁",     false },
  { "爱心满满",   false },
  { "植物专家",   false },
};

/* 默认演示数据（首次启动/无持久化） */

static const struct record_task_s s_default_tasks[TASK_MAX] =
{
  { "看看小绿绿",     "早上 8:00",     true  },
  { "搬到窗边晒太阳", "上午 · AI建议", false },
  { "拍一张成长照片", "任意时间",      false },
  { "帮小绿剪黄叶",   "下午",          false },
  { "浇点水吧",       "傍晚 · AI建议", false },
  { "",               "",              false },
};

static const struct record_diary_s s_default_diary[DIARY_MAX] =
{
  { 0x22c55e, 0, "拍了第28天成长照",  "又长了一片新叶！" },
  { 0xf59e0b, 1, "浇了200ml水",      "" },
  { 0x3b82f6, 3, "长出第5片新叶！",   "成长里程碑 🎉" },
  { 0xec4899, 7, "获得「一周养护」徽章", "" },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void load_tasks(void)
{
  FILE *fp = fopen(TASK_PATH, "rb");

  s_task_count = 0;

  if (fp != NULL)
    {
      if (fread(s_tasks, sizeof(s_tasks[0]), TASK_MAX, fp) > 0)
        {
          int i;

          for (i = 0; i < TASK_MAX; i++)
            {
              if (s_tasks[i].title[0] != '\0')
                {
                  s_task_count = i + 1;
                }
              else
                {
                  break;
                }
            }
        }

      fclose(fp);
    }

  if (s_task_count == 0)
    {
      memcpy(s_tasks, s_default_tasks, sizeof(s_default_tasks));
      s_task_count = 5;
    }
}

static void load_diary(void)
{
  FILE *fp = fopen(DIARY_PATH, "rb");

  s_diary_count = 0;

  if (fp != NULL)
    {
      if (fread(s_diary, sizeof(s_diary[0]), DIARY_MAX, fp) > 0)
        {
          int i;

          for (i = 0; i < DIARY_MAX; i++)
            {
              if (s_diary[i].title[0] != '\0')
                {
                  s_diary_count = i + 1;
                }
              else
                {
                  break;
                }
            }
        }

      fclose(fp);
    }

  if (s_diary_count == 0)
    {
      memcpy(s_diary, s_default_diary, sizeof(s_default_diary));
      s_diary_count = 4;
    }
}

/* 成就状态加载（3D-2） */

static void load_achieve(void)
{
  FILE *fp = fopen(ACHIEVE_PATH, "rb");

  if (fp != NULL)
    {
      if (fread(s_achieve_unlocked, 1, sizeof(s_achieve_unlocked), fp) ==
          sizeof(s_achieve_unlocked))
        {
          /* 加载成功 */
        }
      else
        {
          memset(s_achieve_unlocked, 0, sizeof(s_achieve_unlocked));
        }

      fclose(fp);
    }
  else
    {
      memset(s_achieve_unlocked, 0, sizeof(s_achieve_unlocked));
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int record_service_init(void)
{
  if (g_inited)
    {
      return 0;
    }

  load_tasks();
  load_diary();
  load_achieve();
  g_inited = true;
  return 0;
}

/* --- 任务 --- */

int record_task_get_count(void)
{
  return s_task_count;
}

bool record_task_get(int idx, struct record_task_s *out)
{
  if (idx < 0 || idx >= s_task_count || out == NULL)
    {
      return false;
    }

  *out = s_tasks[idx];
  return true;
}

void record_task_set_done(int idx, bool done)
{
  if (idx < 0 || idx >= s_task_count)
    {
      return;
    }

  s_tasks[idx].done = done;
  record_task_save();
}

void record_task_save(void)
{
  FILE *fp = fopen(TASK_PATH, "wb");

  if (fp != NULL)
    {
      fwrite(s_tasks, sizeof(s_tasks[0]), s_task_count, fp);
      fclose(fp);
    }
}

/* --- 成就（3D-2） --- */

void record_achieve_get_all(bool unlocked[ACHIEVE_MAX])
{
  int i;

  for (i = 0; i < ACHIEVE_MAX; i++)
    {
      unlocked[i] = s_achieve_unlocked[i];
    }
}

int record_task_done_count(void)
{
  int i;
  int count = 0;

  for (i = 0; i < s_task_count; i++)
    {
      if (s_tasks[i].done)
        {
          count++;
        }
    }

  return count;
}

void record_task_on_completed(void)
{
  int done = record_task_done_count();

  /* 完成任务 → 写入日记事件（任务完成联动） */

  {
    struct record_diary_s entry;

    memset(&entry, 0, sizeof(entry));
    entry.color = 0x22c55e;
    entry.day_offset = 0;
    strlcpy(entry.title, "完成了一个任务", sizeof(entry.title));
    strlcpy(entry.detail, "今天的小任务打卡成功 🎉", sizeof(entry.detail));
    record_diary_add(&entry);
  }

  /* 成就判定（示例规则） */

  if (done >= 1)
    {
      record_achieve_unlock(0);   /* 一周养护 */
    }

  if (done >= 3)
    {
      record_achieve_unlock(1);   /* 小园丁 */
    }

  if (done >= 5)
    {
      record_achieve_unlock(2);   /* 爱心满满 */
    }
}

bool record_achieve_unlock(int idx)
{
  FILE *fp;

  if (idx < 0 || idx >= ACHIEVE_MAX || s_achieve_unlocked[idx])
    {
      return false;
    }

  s_achieve_unlocked[idx] = true;

  fp = fopen(ACHIEVE_PATH, "wb");
  if (fp != NULL)
    {
      fwrite(s_achieve_unlocked, 1, sizeof(s_achieve_unlocked), fp);
      fclose(fp);
    }

  /* 刚解锁 → 写入日记 */

  {
    struct record_diary_s entry;

    memset(&entry, 0, sizeof(entry));
    entry.color = 0xf59e0b;
    entry.day_offset = 0;
    strlcpy(entry.title, "获得徽章", sizeof(entry.title));
    snprintf(entry.detail, sizeof(entry.detail), "「%s」解锁！🏅",
             g_achieves[idx].name);
    record_diary_add(&entry);
  }

  return true;
}

/* --- 日记 --- */

int record_diary_get_count(void)
{
  return s_diary_count;
}

bool record_diary_get(int idx, struct record_diary_s *out)
{
  if (idx < 0 || idx >= s_diary_count || out == NULL)
    {
      return false;
    }

  *out = s_diary[idx];
  return true;
}

void record_diary_add(const struct record_diary_s *entry)
{
  int i;

  if (entry == NULL || s_diary_count >= DIARY_MAX)
    {
      return;
    }

  /* 头部插入（新事件在最上） */

  for (i = s_diary_count; i > 0; i--)
    {
      s_diary[i] = s_diary[i - 1];
    }

  s_diary[0] = *entry;
  s_diary_count++;

  if (s_diary_count > DIARY_MAX)
    {
      s_diary_count = DIARY_MAX;
    }

  record_diary_save();
}

void record_diary_save(void)
{
  FILE *fp = fopen(DIARY_PATH, "wb");

  if (fp != NULL)
    {
      fwrite(s_diary, sizeof(s_diary[0]), s_diary_count, fp);
      fclose(fp);
    }
}
