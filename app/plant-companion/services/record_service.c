/****************************************************************************
 * apps/plant-companion/services/record_service.c
 *
 * 记录服务：任务完成状态 + 日记事件持久化（轻量版）
 *
 * BOX语音版：可变记录保存到机内 LittleFS /data/box_voice。
 * 兼容读取原SD记录；SD以只读方式加载字体和素材，不删除原始记录。
 * 写入失败返回错误并恢复内存状态，避免错误宣告保存成功。
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>

#include "record_service.h"
#include "record_text_utf8.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct record_task_s  s_tasks[TASK_MAX];
static int s_task_count;

static struct record_diary_s s_diary[DIARY_MAX];
static int s_diary_count;

static bool g_inited;
/* Defaults are provisional until a record file has been read or saved.
 * SD can mount after UI initialization. Never replace a successful save. */
static bool s_tasks_pending = true;
static bool s_diary_pending = true;
static bool s_achieve_pending = true;

/* 成就状态 + 任务完成计数（持久化） */

static bool s_achieve_unlocked[ACHIEVE_MAX];
static int  s_task_done_count;

#define TASK_PATH    "/data/box_voice/plant_tasks.bin"
#define DIARY_PATH   "/data/box_voice/plant_diary.bin"
#define ACHIEVE_PATH "/data/box_voice/plant_achieve.bin"

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

/* Onboard writes are checked; the original SD records remain untouched. */
static FILE *record_open_read(const char *path)
{
  FILE *fp=fopen(path,"rb");
  if(!fp && !strncmp(path,"/data/",6)){
    char legacy[128];snprintf(legacy,sizeof(legacy),"/mnt/sd/%s",strrchr(path,'/')+1);
    fp=fopen(legacy,"rb");
  }
  return fp;
}
static int record_persist(const char *path,const void *data,size_t bytes)
{
  if(mkdir("/data/box_voice",0700)<0 && errno!=EEXIST)return -errno;
  char tmp[128];snprintf(tmp,sizeof(tmp),"%s.tmp",path);
  FILE *fp=fopen(tmp,"wb");if(!fp)return -errno;
  int ret=0;
  if(fwrite(data,1,bytes,fp)!=bytes || fflush(fp)!=0)ret=-EIO;
  if(fclose(fp)!=0)ret=-EIO;
  if(!ret && rename(tmp,path)!=0)ret=-errno;
  if(ret)remove(tmp);
  if(!ret){
    if(!strcmp(path,TASK_PATH))s_tasks_pending=false;
    if(!strcmp(path,DIARY_PATH))s_diary_pending=false;
    if(!strcmp(path,ACHIEVE_PATH))s_achieve_pending=false;
  }
  return ret;
}

static void load_tasks(void)
{
  FILE *fp = record_open_read(TASK_PATH);

  s_task_count = 0;

  if (fp != NULL)
    {
      size_t bytes = fread(s_tasks, 1, sizeof(s_tasks), fp);
      if (bytes > 0 && bytes % sizeof(s_tasks[0]) == 0 && !ferror(fp))
        {
          int i;

          for (i = 0; i < (int)(bytes / sizeof(s_tasks[0])); i++)
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

  else s_tasks_pending = false;
}

static void load_diary(void)
{
  FILE *fp = record_open_read(DIARY_PATH);

  s_diary_count = 0;

  if (fp != NULL)
    {
      size_t bytes = fread(s_diary, 1, sizeof(s_diary), fp);
      if (bytes > 0 && bytes % sizeof(s_diary[0]) == 0 && !ferror(fp))
        {
          int i;

          for (i = 0; i < (int)(bytes / sizeof(s_diary[0])); i++)
            {
              record_text_utf8(s_diary[i].title,sizeof(s_diary[i].title));
              record_text_utf8(s_diary[i].detail,sizeof(s_diary[i].detail));
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

  else s_diary_pending = false;
}

/* 成就状态加载（3D-2） */

static void load_achieve(void)
{
  FILE *fp = record_open_read(ACHIEVE_PATH);

  if (fp != NULL)
    {
      if (fread(s_achieve_unlocked, 1, sizeof(s_achieve_unlocked), fp) ==
          sizeof(s_achieve_unlocked))
        {
          s_achieve_pending = false;
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
  if(s_tasks_pending)load_tasks();
  return s_task_count;
}

bool record_task_get(int idx, struct record_task_s *out)
{
  if(s_tasks_pending)load_tasks();
  if (idx < 0 || idx >= s_task_count || out == NULL)
    {
      return false;
    }

  *out = s_tasks[idx];
  return true;
}

int record_task_set_done(int idx, bool done)
{
  if(s_tasks_pending)load_tasks();
  if(idx<0 || idx>=s_task_count)return -EINVAL;
  bool previous=s_tasks[idx].done;s_tasks[idx].done=done;
  int ret=record_task_save();if(ret<0)s_tasks[idx].done=previous;
  return ret;
}
int record_task_save(void)
{return record_persist(TASK_PATH,s_tasks,sizeof(s_tasks[0])*s_task_count);}

/* --- 成就（3D-2） --- */

void record_achieve_get_all(bool unlocked[ACHIEVE_MAX])
{
  if(s_achieve_pending)load_achieve();
  int i;

  for (i = 0; i < ACHIEVE_MAX; i++)
    {
      unlocked[i] = s_achieve_unlocked[i];
    }
}

int record_task_done_count(void)
{
  if(s_tasks_pending)load_tasks();
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
  if(s_achieve_pending)load_achieve();

  if (idx < 0 || idx >= ACHIEVE_MAX || s_achieve_unlocked[idx])
    {
      return false;
    }

  s_achieve_unlocked[idx] = true;

  if(record_persist(ACHIEVE_PATH,s_achieve_unlocked,sizeof(s_achieve_unlocked))<0){
    s_achieve_unlocked[idx]=false;return false;
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
  if(s_diary_pending)load_diary();
  return s_diary_count;
}

bool record_diary_get(int idx, struct record_diary_s *out)
{
  if(s_diary_pending)load_diary();
  if (idx < 0 || idx >= s_diary_count || out == NULL)
    {
      return false;
    }

  *out = s_diary[idx];
  return true;
}

int record_diary_add(const struct record_diary_s *entry)
{
  if(s_diary_pending)load_diary();
  if(!entry)return -EINVAL;
  struct record_diary_s *previous=malloc(sizeof(s_diary));if(!previous)return -ENOMEM;
  memcpy(previous,s_diary,sizeof(s_diary));int count=s_diary_count;
  for(int i=s_diary_count<DIARY_MAX?s_diary_count:DIARY_MAX-1;i>0;i--)s_diary[i]=s_diary[i-1];
  s_diary[0]=*entry;
  record_text_utf8(s_diary[0].title,sizeof(s_diary[0].title));
  record_text_utf8(s_diary[0].detail,sizeof(s_diary[0].detail));
  if(s_diary_count<DIARY_MAX)s_diary_count++;
  int ret=record_diary_save();
  if(ret<0){memcpy(s_diary,previous,sizeof(s_diary));s_diary_count=count;}
  free(previous);return ret;
}
int record_diary_save(void)
{return record_persist(DIARY_PATH,s_diary,sizeof(s_diary[0])*s_diary_count);}
