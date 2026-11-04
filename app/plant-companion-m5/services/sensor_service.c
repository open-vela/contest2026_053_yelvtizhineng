/****************************************************************************
 * apps/plant-companion/services/sensor_service.c
 *
 * 传感器业务服务：Modbus 轮询 → 缓存 + 评语规则 + 7天历史环
 *
 * 线程模型：
 *   - sensor_task（pthread）：周期 soil_sensor_read → 更新缓存
 *   - 缓存用 pthread_mutex 保护；on_update 回调在轮询线程里触发，
 *     但 UI 端必须把真正的 LVGL 刷新投递到 ui_task（用 lv_timer / msgq）
 *     —— 本服务不直接操作任何 LVGL 对象。
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <semaphore.h>

#include "sensor_service.h"
#include "power_service.h"
#include "sensor_net_gate.h"
#include "../components/sensor_driver/soil_sensor.h"
#ifdef CONFIG_PLANT_AI_MODULE
#  include "server_bridge.h"
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_t g_task;
static sem_t g_wake;        /* read_now() 提前唤醒轮询线程，不用等周期 */
static bool g_running;
static int g_interval_ms;
static void (*g_on_update)(void *user_data);
static void *g_user_data;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sensor_view_s g_view;
static bool g_view_from_cloud;
static struct sensor_net_gate g_fetch_gate,g_upload_gate;
static struct sensor_history_s g_history;
static int g_history_today_valid;   /* 今天槽位是否已写入（跨天归档用） */
static int g_last_read_day;         /* 上次读取的"天数"（判断是否跨天） */

/* 光照占位：无光照传感器，按水分+时间估算（仅演示） */

#define LIGHT_EST_LOW   0
#define LIGHT_EST_MID   1
#define LIGHT_EST_HIGH  2

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 阈值评语规则（离线，UI_SPEC ④ "我很好/很舒服/有点暗/营养够"） */

static void view_update_notes(struct sensor_view_s *v)
{
  /* 水分：目标 50-75% */

  if (v->moisture < 20.0f)
    {
      v->moisture_status = SENSOR_STATUS_BAD;
      strlcpy(v->moisture_note, "好渴呀", sizeof(v->moisture_note));
    }
  else if (v->moisture < 40.0f)
    {
      v->moisture_status = SENSOR_STATUS_WARN;
      strlcpy(v->moisture_note, "有点干", sizeof(v->moisture_note));
    }
  else if (v->moisture <= 75.0f)
    {
      v->moisture_status = SENSOR_STATUS_OK;
      strlcpy(v->moisture_note, "我很好", sizeof(v->moisture_note));
    }
  else
    {
      v->moisture_status = SENSOR_STATUS_WARN;
      strlcpy(v->moisture_note, "水太多", sizeof(v->moisture_note));
    }

  /* 温度：舒适 18-28°C */

  if (v->temp < 10.0f || v->temp > 35.0f)
    {
      v->temp_status = SENSOR_STATUS_BAD;
      strlcpy(v->temp_note, "太冷/热", sizeof(v->temp_note));
    }
  else if (v->temp < 18.0f || v->temp > 28.0f)
    {
      v->temp_status = SENSOR_STATUS_WARN;
      strlcpy(v->temp_note, "有点凉/热", sizeof(v->temp_note));
    }
  else
    {
      v->temp_status = SENSOR_STATUS_OK;
      strlcpy(v->temp_note, "好舒服", sizeof(v->temp_note));
    }

  /* EC：绿萝目标 ~1.0-1.6 mS/cm（传感器 μS/cm → /1000） */

  {
    float ec_ms = v->ec / 1000.0f;

    if (ec_ms < 0.6f)
      {
        v->ec_status = SENSOR_STATUS_WARN;
        strlcpy(v->ec_note, "缺营养", sizeof(v->ec_note));
      }
    else if (ec_ms > 2.2f)
      {
        v->ec_status = SENSOR_STATUS_WARN;
        strlcpy(v->ec_note, "营养太浓", sizeof(v->ec_note));
      }
    else
      {
        v->ec_status = SENSOR_STATUS_OK;
        strlcpy(v->ec_note, "营养够", sizeof(v->ec_note));
      }
  }

  /* 光照估算（占位规则）：水分健康且白天 → 中；否则低 */

  v->light_status = (v->moisture_status == SENSOR_STATUS_OK) ?
                    LIGHT_EST_MID : LIGHT_EST_LOW;
}

static uint32_t uptime_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

/* 单次轮询：读传感器 → 更新缓存 + 历史环 */


/* 跨天归档 + 写入今天槽位（本地/云端共用，v192 去重） */

static void history_push(float moisture, float temp)
{
  struct tm tm_now;
  time_t now = time(NULL);

  localtime_r(&now, &tm_now);
  if (g_last_read_day != tm_now.tm_yday)
    {
      int i;

      for (i = SENSOR_HISTORY_DAYS - 1; i >= 1; i--)
        {
          g_history.day[i] = g_history.day[i - 1];
          g_history.moisture[i] = g_history.moisture[i - 1];
          g_history.temp[i] = g_history.temp[i - 1];
          g_history.valid[i] = g_history.valid[i - 1];
        }

      g_history.day[0] = 0;
      g_history_today_valid = 0;
      g_last_read_day = tm_now.tm_yday;
    }

  if (!g_history_today_valid)
    {
      g_history.day[0] = 0;
      g_history.moisture[0] = moisture;
      g_history.temp[0] = temp;
      g_history.valid[0] = true;
      g_history_today_valid = 1;
    }
}

static void sensor_poll_once(void)
{
  struct sensor_view_s new_view;
  memset(&new_view,0,sizeof(new_view));
#ifdef CONFIG_PLANT_AI_MODULE
      /* Cloud soil is the only installed source; never open a local UART.
       * 土壤网关把 DR164 数据写进 telemetry，这里从 devices/sync 拉回来。 */
      uint32_t now_ms = uptime_ms();
      struct sb_soil_s cloud;

      /* One cloud attempt per minute, including errors and worker restarts. */
      if (server_bridge_configured() &&
          sensor_net_claim(&g_fetch_gate,now_ms,60000u) &&
          server_bridge_fetch_soil(&cloud) == 0 && cloud.valid)
        {
          new_view.moisture = cloud.moisture;
          new_view.temp = cloud.temp;
          new_view.ec = cloud.ec;
          new_view.ph = cloud.ph;
          new_view.salt = cloud.salt;
          new_view.nitrogen = cloud.nitrogen;
          new_view.phosphorus = cloud.phosphorus;
          new_view.potassium = cloud.potassium;
          new_view.valid = true;
          new_view.last_read_ms = now_ms;
          view_update_notes(&new_view);

          pthread_mutex_lock(&g_lock);
          g_view = new_view;
          g_view_from_cloud = true;
          history_push(cloud.moisture, cloud.temp);
          pthread_mutex_unlock(&g_lock);

          if (g_on_update != NULL)
            {
              g_on_update(g_user_data);
            }

          return;
        }
#endif
      pthread_mutex_lock(&g_lock);
      if (g_view.last_read_ms == 0 ||
          (uptime_ms() - g_view.last_read_ms) > 125000u)
        {
          g_view.valid = false;
        }

      pthread_mutex_unlock(&g_lock);
}

/* 采集上云（2026-09-11）：把**真实**的 8 个参数上报给服务器。
 *
 * 节奏：本地轮询保持快节奏（界面要实时感），上云按 SENSOR_UPLOAD_EVERY_MS
 * 节流（默认 30s）；用户打开数据页会 read_now() 强制补一次最新值。
 * 读失败（valid=false）不上报——服务器/手机端这个时段就没有数据，前端显示
 * "--"，绝不发假值（用户定案）。未配置服务器 / 未注册时直接跳过，不等待。
 *
 * 返回 0 = 真的发上去了（调用方据此推进节流计时）。 */

#define SENSOR_UPLOAD_EVERY_MS  60000

/* 上报周期：优先用服务器在心跳里下发的，拿不到用上面的本地默认值 */

static int sensor_upload_interval_ms(void)
{
#ifdef CONFIG_PLANT_AI_MODULE
  int s = server_bridge_upload_interval_s();

  if (s >= 10 && s <= 86400)
    {
      return s < 60 ? 60000 : s * 1000;
    }
#endif
  return SENSOR_UPLOAD_EVERY_MS;
}

static int sensor_cloud_upload(void)
{
  /* Cloud gateway already owns these measurements; never echo them back. */
  return 0;
}

static void *sensor_task_entry(void *arg)
{
  (void)arg;

  printf("[SOIL] sensor thread started\n");

  while (g_running)
    {
      /* 等到周期到点，或被 sensor_service_read_now()（进数据页）提前唤醒 */

      struct timespec ts;
      uint32_t now_ms;

      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += g_interval_ms / 1000;
      ts.tv_nsec += (long)(g_interval_ms % 1000) * 1000000L;
      if (ts.tv_nsec >= 1000000000L)
        {
          ts.tv_sec++;
          ts.tv_nsec -= 1000000000L;
        }

      while (sem_timedwait(&g_wake, &ts) != 0)
        {
          if (errno != EINTR)
            {
              break;   /* ETIMEDOUT：周期到点 */
            }
        }

      if (!g_running)
        {
          break;
        }

      if(!power_service_background_allowed())continue;
      sensor_poll_once();

      /* 上云节流：首次读到 / 被 read_now 点名 / 距上次成功上报够久了 */

      now_ms = uptime_ms();
#ifdef CONFIG_PLANT_AI_MODULE
      if (server_bridge_configured() && sensor_net_claim(&g_upload_gate,now_ms,
                                  (uint32_t)sensor_upload_interval_ms()))
        {
          sensor_cloud_upload();
        }
#endif
    }

  sem_destroy(&g_wake);
  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int sensor_service_start(int interval_ms,
                         void (*on_update)(void *user_data),
                         void *user_data)
{
  int ret;

  if (g_running)
    {
      return 0;
    }

  if (interval_ms <= 0)
    {
      interval_ms = 3000;
    }

  g_interval_ms = interval_ms;
  g_on_update = on_update;
  g_user_data = user_data;
  sem_init(&g_wake, 0, 0);
  g_running = true;

  {
    pthread_attr_t attr;

    /* 8KB：轮询线程现在还要跑 HTTP 上报（socket 收发 + JSON 拼装） */

    pthread_attr_init(&attr);
    /* v191: 8K 不够 sb_http_get(hdr[512]) + fetch_soil JSON，实测 stack_dump */
    pthread_attr_setstacksize(&attr, 16384);
    ret = pthread_create(&g_task, &attr, sensor_task_entry, NULL);
    pthread_attr_destroy(&attr);
  }

  if (ret != 0)
    {
      g_running = false;
      sem_destroy(&g_wake);
      return -ret;
    }

  return 0;
}

void sensor_service_stop(void)
{
  if (g_running)
    {
      g_running = false;
      /* 立刻唤醒轮询线程：否则要等它睡满一整个周期（最长 g_interval_ms）
       * 才醒来退出，拍照页进入时会卡这么久。 */

      sem_post(&g_wake);
      pthread_join(g_task, NULL);
    }
}

bool sensor_service_is_running(void)
{
  return g_running;
}

void sensor_service_get_view(struct sensor_view_s *out)
{
  if (out == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_lock);
  *out = g_view;
  pthread_mutex_unlock(&g_lock);
}

void sensor_service_get_history(struct sensor_history_s *out)
{
  if (out == NULL)
    {
      return;
    }

  pthread_mutex_lock(&g_lock);
  *out = g_history;
  pthread_mutex_unlock(&g_lock);
}

int sensor_service_read_now(void)
{
  /* 非阻塞：只喊醒轮询线程"立刻读一次并上报"，绝不在调用者（UI）线程里
   * 做串口收发和 HTTP——那是 LVGL 线程，卡一下就掉帧。 */

  if (!g_running)
    {
      return -1;
    }

  sem_post(&g_wake);
  return 0;
}
