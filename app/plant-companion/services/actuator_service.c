/****************************************************************************
 * apps/plant-companion/services/actuator_service.c
 *
 * 自动执行层（板卡侧）。协议与设计说明见 actuator_service.h。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <nuttx/clock.h>

#include "actuator_service.h"

#if defined(CONFIG_PLANT_AI_MODULE) && defined(CONFIG_PLANT_ACTUATOR)

#include "server_bridge.h"
#include "../hal/hal_gpio.h"

/****************************************************************************
 * 板卡自带哪几路执行器（开机声明给服务器）。
 * kind 必须与服务器 actuator_ops.KINDS 对齐：
 *   water 浇水 / light 补光 / fan 通风 / feeder 喂食 / heat 加温
 * 以后加硬件：这里加一行 + act_gpio_of() 给引脚 + Kconfig 加一个引脚配置项。
 ****************************************************************************/

static const struct sb_act_cap_s g_caps[] =
{
  { "water", "水泵" },
};

#define ACT_NCAPS        (sizeof(g_caps) / sizeof(g_caps[0]))

#define ACT_MAX_RUN      2      /* 最多同时跑几路动作 */
#define ACT_POLL_SEC     15     /* 取指令周期：服务器说"60s 内取走"，取 15s 更跟手 */
#define ACT_DECL_RETRY_SEC 30   /* 声明能力失败后的重试间隔 */
#define ACT_MAX_ON_SEC   120    /* 单次动作最长持续（防止服务器配个把小时烧泵） */
#define ACT_DEFAULT_SEC  15     /* 服务器没给 duration_s 时的默认（对齐服务器默认） */

/* 执行引脚：-1 = 还没接执行硬件（干跑）。见 Kconfig PLANT_ACTUATOR_GPIO_WATER */

#ifdef CONFIG_PLANT_ACTUATOR_GPIO_WATER
#  define ACT_GPIO_WATER  CONFIG_PLANT_ACTUATOR_GPIO_WATER
#else
#  define ACT_GPIO_WATER  (-1)
#endif

struct act_run_s
{
  bool busy;
  char job_id[40];        /* 空 = 本地自检，不回执 */
  char kind[16];
  int  remain_s;          /* 剩余秒数：poll 每秒调一次，本层用计数计时 */
};

static bool    g_act_declared;            /* 能力已声明成功 */
static clock_t g_act_decl_at;             /* 上次声明尝试（节流用） */
static clock_t g_act_poll_at;             /* 上次取指令（节流用） */
static int     g_act_epoch = -1;          /* 服务器配置版本：变了就重新声明 */
static char    g_act_last[96] = "尚未执行过任何动作";
static struct act_run_s g_runs[ACT_MAX_RUN];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 这一路用哪个引脚；-1 = 没接硬件 */

static int act_gpio_of(const char *kind)
{
  if (kind != NULL && strcmp(kind, "water") == 0)
    {
      return ACT_GPIO_WATER;
    }

  return -1;
}

/* 这一路是不是我们声明过的（服务器只会下发声明过的，保险起见还是查） */

static bool act_supported(const char *kind)
{
  size_t i;

  if (kind == NULL)
    {
      return false;
    }

  for (i = 0; i < ACT_NCAPS; i++)
    {
      if (strcmp(g_caps[i].kind, kind) == 0)
        {
          return true;
        }
    }

  return false;
}

/* 驱动一路（没接硬件就什么都不做——时序照走，回执里写明干跑）。
 * 多数继电器模块低电平吸合，由 CONFIG_PLANT_ACTUATOR_ACTIVE_LOW 决定。 */

static void act_drive(const char *kind, bool on)
{
  int pin = act_gpio_of(kind);

  if (pin < 0)
    {
      return;
    }

#ifdef CONFIG_PLANT_ACTUATOR_ACTIVE_LOW
  hal_gpio_write(pin, !on);
#else
  hal_gpio_write(pin, on);
#endif
}

/* 到点的动作：关断 + 回执（回执里如实写明有没有真接硬件） */

static void act_finish(void)
{
  int i;

  for (i = 0; i < ACT_MAX_RUN; i++)
    {
      struct act_run_s *r = &g_runs[i];
      char detail[64];
      int  pin;

      if (!r->busy)
        {
          continue;
        }

      if (r->remain_s > 0)
        {
          r->remain_s--;
          continue;
        }

      act_drive(r->kind, false);
      pin = act_gpio_of(r->kind);

      if (pin < 0)
        {
          snprintf(detail, sizeof(detail), "dry-run: no actuator wired");
        }
      else
        {
          snprintf(detail, sizeof(detail), "gpio%d pulsed", pin);
        }

      if (r->job_id[0] != '\0')
        {
          if (server_bridge_ack_job(r->job_id, 1, detail) == 0)
            {
              printf("[Act] 回执完成 job=%s (%s)\n", r->job_id, detail);
            }
        }
      else
        {
          printf("[Act] 本地自检结束 %s (%s)\n", r->kind, detail);
        }

      snprintf(g_act_last, sizeof(g_act_last), "%s %s",
               r->kind, detail);
      r->busy = false;
      r->job_id[0] = '\0';
    }
}

/* 声明能力（幂等；失败按 ACT_DECL_RETRY_SEC 重试） */

static void act_declare(clock_t now)
{
  int ret;

  if (g_act_declared)
    {
      return;
    }

  if (g_act_decl_at != 0 &&
      (now - g_act_decl_at) < (clock_t)(ACT_DECL_RETRY_SEC * TICK_PER_SEC))
    {
      return;
    }

  g_act_decl_at = now;

  ret = server_bridge_declare_capabilities(g_caps, (int)ACT_NCAPS);
  if (ret == 0)
    {
      g_act_declared = true;
      printf("[Act] 已向服务器声明 %d 路执行器\n", (int)ACT_NCAPS);
    }
  else if (ret != -ENOTCONN)
    {
      printf("[Act] 声明能力失败: %d\n", ret);
    }
}

/* 启动一条服务器下发的指令 */

static void act_start(const struct sb_act_job_s *job)
{
  int dur = job->duration_s;
  int slot = -1;
  int i;

  if (dur <= 0)
    {
      dur = ACT_DEFAULT_SEC;
    }

  if (dur > ACT_MAX_ON_SEC)
    {
      dur = ACT_MAX_ON_SEC;
    }

  if (!act_supported(job->kind))
    {
      server_bridge_ack_job(job->job_id, 0, "unsupported kind");
      printf("[Act] 不支持的执行器类型: %s\n", job->kind);
      return;
    }

  /* 干跑模式（没接执行硬件，引脚 = -1）：
   * 动作立等可取——取到指令当场完成并回执
   * （App 点「立即浇水」图标就出结果）。
   * 等继电器接上后 act_gpio_of() >= 0，走下面的真执行，
   * 这段自动不生效。 */

  if (act_gpio_of(job->kind) < 0)
    {
      server_bridge_ack_job(job->job_id, 1, "dry-run: no actuator wired");
      printf("[Act] 干跑完成 %s %ds（未接执行硬件，立即回执）job=%s\n",
             job->kind, dur, job->job_id);
      snprintf(g_act_last, sizeof(g_act_last), "%s 干跑完成（未接硬件）",
               job->kind);
      return;
    }

  for (i = 0; i < ACT_MAX_RUN; i++)
    {
      if (!g_runs[i].busy)
        {
          slot = i;
          break;
        }
    }

  if (slot < 0)
    {
      server_bridge_ack_job(job->job_id, 0, "device busy");
      printf("[Act] 通道占满，拒绝 job=%s\n", job->job_id);
      return;
    }

  act_drive(job->kind, true);

  memset(&g_runs[slot], 0, sizeof(g_runs[slot]));
  g_runs[slot].busy     = true;
  g_runs[slot].remain_s = dur;
  strncpy(g_runs[slot].job_id, job->job_id, sizeof(g_runs[slot].job_id) - 1);
  strncpy(g_runs[slot].kind, job->kind, sizeof(g_runs[slot].kind) - 1);

  printf("[Act] 执行 %s %d 秒 (job=%s, 引脚=%d, 说明=%s)\n",
         job->kind, dur, job->job_id, act_gpio_of(job->kind),
         job->reason[0] != '\0' ? job->reason : "-");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int actuator_service_init(void)
{
  size_t i;

  memset(g_runs, 0, sizeof(g_runs));
  g_act_declared = false;
  g_act_decl_at  = 0;
  g_act_poll_at  = 0;
  g_act_epoch    = -1;

  for (i = 0; i < ACT_NCAPS; i++)
    {
      int pin = act_gpio_of(g_caps[i].kind);

      if (pin >= 0)
        {
          hal_gpio_config(pin, HAL_GPIO_OUTPUT);
          act_drive(g_caps[i].kind, false);   /* 上电保持关断 */
        }
    }

  printf("[Act] 执行层就绪：%d 路，water_gpio=%d%s\n",
         (int)ACT_NCAPS, act_gpio_of("water"),
         act_gpio_of("water") < 0 ? "（未接硬件，干跑）" : "");

  return 0;
}

void actuator_service_poll(void)
{
  struct sb_act_job_s jobs[4];
  clock_t now = clock_systime_ticks();
  int njobs = 0;
  int epoch;
  int ret;
  int i;

  /* ① 先收尾：到点的动作关断 + 回执，腾出通道 */

  act_finish();

  /* ② 服务器配置变了（重连/换地址）→ 重新声明能力 */

  epoch = server_bridge_config_epoch();
  if (epoch != g_act_epoch)
    {
      g_act_epoch    = epoch;
      g_act_declared = false;
      g_act_decl_at  = 0;
      g_act_poll_at  = 0;
    }

  if (!server_bridge_configured())
    {
      return;             /* 没配服务器/没注册：不发任何请求 */
    }

  /* ③ 声明能力（幂等，成功一次就够） */

  act_declare(now);

  /* ④ 取指令（每 ACT_POLL_SEC 一次） */

  if ((now - g_act_poll_at) < (clock_t)(ACT_POLL_SEC * TICK_PER_SEC))
    {
      return;
    }

  g_act_poll_at = now;

  ret = server_bridge_fetch_pending(jobs,
                                    (int)(sizeof(jobs) / sizeof(jobs[0])),
                                    &njobs);
  if (ret < 0 || njobs <= 0)
    {
      return;
    }

  for (i = 0; i < njobs; i++)
    {
      act_start(&jobs[i]);
    }
}

int actuator_service_run(const char *kind, int seconds)
{
  int dur = seconds > 0 ? seconds : ACT_DEFAULT_SEC;
  int slot = -1;
  int i;

  if (!act_supported(kind))
    {
      return -EINVAL;
    }

  if (dur > ACT_MAX_ON_SEC)
    {
      dur = ACT_MAX_ON_SEC;
    }

  for (i = 0; i < ACT_MAX_RUN; i++)
    {
      if (!g_runs[i].busy)
        {
          slot = i;
          break;
        }
    }

  if (slot < 0)
    {
      return -EBUSY;
    }

  act_drive(kind, true);

  memset(&g_runs[slot], 0, sizeof(g_runs[slot]));
  g_runs[slot].busy     = true;
  g_runs[slot].remain_s = dur;
  strncpy(g_runs[slot].kind, kind, sizeof(g_runs[slot].kind) - 1);

  return 0;
}

const char *actuator_service_last(void)
{
  return g_act_last;
}

#else  /* !(CONFIG_PLANT_AI_MODULE && CONFIG_PLANT_ACTUATOR) */

/* 没开自动执行（或没开 AI 模块）：空实现，调用方不用改代码 */

int actuator_service_init(void)
{
  return 0;
}

void actuator_service_poll(void)
{
}

int actuator_service_run(const char *kind, int seconds)
{
  (void)kind;
  (void)seconds;
  return -ENOSYS;
}

const char *actuator_service_last(void)
{
  return "actuator layer disabled";
}

#endif
