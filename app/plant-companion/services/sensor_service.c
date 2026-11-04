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
#include "sensor_presentation.h"
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
static volatile bool g_upload_now;   /* read_now() 置位：下一次必上报 */
static int g_interval_ms;
static void (*g_on_update)(void *user_data);
static void *g_user_data;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct sensor_view_s g_view;
static struct sensor_history_s g_history;
#ifdef CONFIG_PLANT_AI_MODULE
#  define SENSOR_CLOUD_PULL_MS 300000u
static uint32_t g_cloud_attempt_ms;
static bool g_cloud_attempted;
static int g_cloud_config_epoch;
#endif
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

/* No species thresholds configured: preserve observations without health claims. */
static void view_update_notes(struct sensor_view_s *v)
{
  v->moisture_status=v->temp_status=v->ec_status=-1;
  v->light_status=-1;
  snprintf(v->moisture_note,sizeof(v->moisture_note),"查看读数");
  snprintf(v->temp_note,sizeof(v->temp_note),"土壤温度");
  snprintf(v->ec_note,sizeof(v->ec_note),"电导率");
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
  struct soil_data_s raw;
  struct sensor_view_s new_view;
  int ret;

  memset(&new_view, 0, sizeof(new_view));

#ifdef CONFIG_PLANT_AI_MODULE
  ret = server_bridge_cloud_soil_enabled() ? -ENODEV :
        soil_sensor_read(&raw, 0);
#else
  ret = soil_sensor_read(&raw, 0);
#endif
  if (ret < 0)
    {
#ifdef CONFIG_PLANT_AI_MODULE
      /* v190：本地无土壤（RS485 未接/超时）→ 云端无线土壤兜底。
       * 土壤网关把 DR164 数据写进 telemetry，这里从 devices/sync 拉回来。 */
      uint32_t now_ms = uptime_ms();
      struct sb_soil_s cloud;

      int epoch = server_bridge_config_epoch();
      if (epoch != g_cloud_config_epoch)
        {
          g_cloud_config_epoch = epoch;
          g_cloud_attempted = false;
        }

      /* Keep the last cloud snapshot between scheduled pulls. A screen
       * refresh or camera exit must not bypass the five-minute cadence. */
      if (g_cloud_attempted &&
          now_ms - g_cloud_attempt_ms < SENSOR_CLOUD_PULL_MS)
        return;

      if (server_bridge_configured())
        {
          g_cloud_attempt_ms = now_ms;
          g_cloud_attempted = true;
          printf("[CloudSoil] pull_ms=%lu interval_ms=%lu\n",
                 (unsigned long)now_ms, (unsigned long)SENSOR_CLOUD_PULL_MS);
        }

      /* Record attempts as well as successes: failures cannot hammer HTTP. */
      if (server_bridge_configured() &&
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
          new_view.field_mask = cloud.field_mask;
          new_view.source = SENSOR_SOURCE_CLOUD;
          /* API does not identify the source reporting period. Do not invent
           * a freshness threshold from this BOX's own upload configuration. */
          new_view.expected_interval_ms = 0;
          new_view.sample_unix_s = cloud.sample_unix_s;
          snprintf(new_view.measured_at,sizeof(new_view.measured_at),"%s",cloud.measured_at);
          snprintf(new_view.source_id,sizeof(new_view.source_id),"%s",cloud.source_id);
          new_view.valid = true;
          new_view.last_read_ms = now_ms;
          view_update_notes(&new_view);

          pthread_mutex_lock(&g_lock);
          new_view.version = g_view.version + 1;
          g_view = new_view;
          if((new_view.field_mask&3)==3)history_push(cloud.moisture, cloud.temp);
          pthread_mutex_unlock(&g_lock);

          if (g_on_update != NULL)
            {
              g_on_update(g_user_data);
            }

          return;
        }
#endif

      /* 读取失败：保留旧值，仅标 invalid */

      pthread_mutex_lock(&g_lock);
      if (g_view.last_read_ms == 0 || (uptime_ms() - g_view.last_read_ms) > 60000u)
        { g_view.valid = false; }
      pthread_mutex_unlock(&g_lock);
      return;
    }

  new_view.moisture  = raw.moisture;
  new_view.temp      = raw.temp;
  new_view.ec        = raw.ec;
  new_view.ph        = raw.ph;
  new_view.salt      = raw.salt;
  new_view.nitrogen  = raw.nitrogen;
  new_view.phosphorus = raw.phosphorus;
  new_view.potassium = raw.potassium;
  const float fields[]={raw.moisture,raw.temp,raw.ec,raw.salt,raw.ph,
                        raw.nitrogen,raw.phosphorus,raw.potassium};
  new_view.field_mask = sensor_valid_fields(fields);
  new_view.source = SENSOR_SOURCE_LOCAL;
  new_view.expected_interval_ms = g_interval_ms;
  snprintf(new_view.source_id,sizeof(new_view.source_id),"RS485");
  new_view.valid     = new_view.field_mask != 0;
  new_view.last_read_ms = uptime_ms();

  view_update_notes(&new_view);

  pthread_mutex_lock(&g_lock);
  new_view.version = g_view.version + 1;
  g_view = new_view;
  if((new_view.field_mask&3)==3)history_push(raw.moisture, raw.temp);
  pthread_mutex_unlock(&g_lock);

  if (g_on_update != NULL)
    {
      g_on_update(g_user_data);
    }
}

/* 采集上云（2026-09-11）：把**真实**的 8 个参数上报给服务器。
 *
 * 节奏：本地轮询保持快节奏（界面要实时感），上云按 SENSOR_UPLOAD_EVERY_MS
 * 节流（默认 30s）；用户打开数据页会 read_now() 强制补一次最新值。
 * 读失败（valid=false）不上报——服务器/手机端这个时段就没有数据，前端显示
 * "--"，绝不发假值（用户定案）。未配置服务器 / 未注册时直接跳过，不等待。
 *
 * 返回 0 = 真的发上去了（调用方据此推进节流计时）。 */

#define SENSOR_UPLOAD_EVERY_MS  30000

/* 上报周期：优先用服务器在心跳里下发的，拿不到用上面的本地默认值 */

static int sensor_upload_interval_ms(void)
{
#ifdef CONFIG_PLANT_AI_MODULE
  int s = server_bridge_upload_interval_s();

  if (s >= 10 && s <= 86400)
    {
      return s * 1000;
    }
#endif
  return SENSOR_UPLOAD_EVERY_MS;
}

static int sensor_cloud_upload(void)
{
#ifdef CONFIG_PLANT_AI_MODULE
  struct sensor_view_s v;

  if (!server_bridge_configured())
    {
      return -1;
    }

  sensor_service_get_view(&v);
  if (!v.valid || v.source != SENSOR_SOURCE_LOCAL || v.field_mask != SP_ALL_FIELDS)
    {
      return -1;
    }

  if (server_bridge_report_telemetry(v.moisture, v.temp, v.ec, v.ph,
                                     v.salt, v.nitrogen, v.phosphorus,
                                     v.potassium) != 0)
    {
      return -1;
    }

  printf("[SOIL] 上云 OK 水分=%.1f%% 温度=%.1f EC=%.0f pH=%.1f "
         "盐=%.0f N=%.0f P=%.0f K=%.0f\n",
         v.moisture, v.temp, v.ec, v.ph, v.salt, v.nitrogen,
         v.phosphorus, v.potassium);
  return 0;
#else
  return -1;
#endif
}

static void *sensor_task_entry(void *arg)
{
  uint32_t last_upload_ms = 0;
  (void)arg;

  printf("[SOIL] sensor thread started\n");

  while (g_running)
    {
      /* 等到周期到点，或被 sensor_service_read_now()（进数据页）提前唤醒 */

      struct timespec ts;
      uint32_t now_ms;

      clock_gettime(CLOCK_REALTIME, &ts);
      unsigned poll_ms=box_power_sensor_interval((unsigned)g_interval_ms);
      ts.tv_sec += poll_ms / 1000;
      ts.tv_nsec += (long)(poll_ms % 1000) * 1000000L;
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

      sensor_poll_once();

      /* 上云节流：首次读到 / 被 read_now 点名 / 距上次成功上报够久了 */

      now_ms = uptime_ms();
      if (g_upload_now || last_upload_ms == 0 ||
          (now_ms - last_upload_ms) >=
              (uint32_t)sensor_upload_interval_ms())
        {
          g_upload_now = false;
          if (sensor_cloud_upload() == 0)
            {
              last_upload_ms = now_ms;
            }
        }
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

  g_upload_now = true;
  sem_post(&g_wake);
  return 0;
}
